// Copyright (C) 2026 fourletterworkshop.com
// Copyright (C) 2026 natko (github.com/natkokrasevac)
// This file is part of Moonlight MP for PlayStation 4.
// Moonlight MP for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight MP for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight MP for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
//

// === GfxManager.cpp ==========================================================
//
// OpenOrbis PS4 graphics/presentation foundation.
//
// Implements the GfxManager platform graphics subsystem using the low-level
// facilities available through the OpenOrbis SDK:
//   - VideoOut initialization and buffer registration.
//   - Direct-memory allocation and mapping for GPU-visible framebuffers.
//   - TILING_MODE_TILE (tiled) framebuffers, sized via the GNM render-target
//     byte-size computation.
//   - GPU-side framebuffer clear (DMA constant fill) — required because the
//     CPU cannot write pixels into a tiled surface.
//   - VideoOut flip submission and event-queue synchronization.
//   - Deterministic initialization and shutdown.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "GfxManager.h"

// --- OpenOrbis SDK headers (private to this translation unit) ---------------
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/_types/errors.h>

// --- OpenGNM headers (private to this translation unit) ---------------------
// gnmdriver.h provides sceGnmSubmitCommandBuffers / sceGnmSubmitDone /
// sceGnmFlushGarlic (the OpenGNM driver API). Do NOT also include
// <orbis/GnmDriver.h> — its declarations of the same sceGnm* functions use
// different types and conflict with the OpenGNM ones.
#include <gnm_commandbuffer.h>
#include <gnm_drawcommandbuffer.h>
#include <gnm_rendertarget.h>
#include <gnmdriver.h>
#include <platform.h>   // sceGnmGpuMode()

#include <cstdio>               // printf, fflush, snprintf
#include <cstring>              // memset

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Base (non-4K) PS4 display resolution.
static constexpr uint32_t kBaseDisplayWidth  = 1920;
static constexpr uint32_t kBaseDisplayHeight = 1080;

/// PS4 Pro 4K display resolution (used when the output is actually >1080p).
static constexpr uint32_t k4KDisplayWidth  = 3840;
static constexpr uint32_t k4KDisplayHeight = 2160;

/// Number of buffered frame resources (double buffering).
static constexpr uint32_t kFrameCount = 2;

/// VideoOut buffer attribute index (single attribute set).
static constexpr int32_t kVideoOutBufferAttributeIndex = 0;

/// VideoOut flip mode: present on real video-out vsync.
static constexpr uint32_t kFlipMode = ORBIS_VIDEO_OUT_FLIP_VSYNC;

/// Direct-memory alignment required by the SDK samples (2 MiB).
static constexpr size_t kDirectMemoryAlignment = 0x200000;

/// Direct-memory type for GPU-visible framebuffers (write-combined Garlic).
static constexpr int32_t kDirectMemoryType = ORBIS_KERNEL_WC_GARLIC;

/// Memory protection for framebuffer mapping (CPU RW + GPU RW).
static constexpr int32_t kDirectMemoryProt = ORBIS_KERNEL_PROT_CPU_RW |
                                             ORBIS_KERNEL_PROT_GPU_ALL;

/// Direct-memory type for the GPU-clear command buffer (WC Garlic).
static constexpr int32_t kCmdMemType = ORBIS_KERNEL_WC_GARLIC;
/// Memory protection for the command buffer (CPU RW + GPU RW).
static constexpr int32_t kCmdMemProt = ORBIS_KERNEL_PROT_CPU_RW |
                                       ORBIS_KERNEL_PROT_GPU_ALL;

/// Direct-memory type for the EOP sync label (WB Onion, CPU-pollable).
static constexpr int32_t kLabelMemType = ORBIS_KERNEL_WB_ONION;
/// Memory protection for the EOP label (CPU RW + GPU RW).
static constexpr int32_t kLabelMemProt = ORBIS_KERNEL_PROT_CPU_RW |
                                         ORBIS_KERNEL_PROT_GPU_ALL;

/// Command buffer size in bytes for the GPU clear. Passed to sceGnmCmdInit.
static constexpr uint32_t kCommandBytes = 16384;

/// EOP spin timeout (iterations) before declaring a hang.
static constexpr uint32_t kEopTimeoutSpins = 100000000u;

/// Framebuffer render-target tile mode hint. The framebuffer is registered
/// with VideoOut as TILING_MODE_TILE, so the GNM render target (and the GPU
/// UI/video renderers that draw into it) must use a matching display tile
/// mode. Per the SCE VideoOut documentation, SCE_VIDEO_OUT_TILING_MODE_TILE
/// corresponds to kTileModeDisplay_2dThin in the Gnm library, so the render
/// target must use GNM_TM_DISPLAY_2D_THIN. Using a different display tile mode
/// (e.g. 1D_THIN) would write pixels in a swizzle pattern that does not match
/// the VideoOut scanout, producing garbled/blocky output.
static constexpr GnmTileMode kFramebufferTileMode = GNM_TM_DISPLAY_2D_THIN;

/// Fixed diagnostic clear color (clearly non-symmetric to expose channel
/// order errors). BGRA8: B=0x40, G=0x80, R=0xC0, A=0xFF.
/// In memory (little-endian BGRA) this is bytes [0x40, 0x80, 0xC0, 0xFF].
static constexpr uint32_t kDefaultClearColor = 0xFFC08040u;

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with the Application Shell)
// ---------------------------------------------------------------------------

static void gfxDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// A single buffered frame resource.
struct FrameResource {
    void*   colorBuffer;   ///< Mapped framebuffer base address.
    int64_t flipArg;       ///< Flip argument identifying this frame.
    bool    submitted;     ///< Whether this frame has been submitted for flip.
};

/// A minimal direct-memory block used for the GPU-clear command buffer and the
/// EOP sync label. Mirrors the DmemBlock pattern used by the GPU renderers.
struct DmemBlock {
    void*  baseAddress = nullptr;
    off_t  physicalAddress = 0;
    size_t totalSize = 0;

    bool allocate(size_t size, int32_t memType, int32_t prot) {
        if (baseAddress != nullptr) {
            return false;
        }
        const size_t alignedSize = (size + kDirectMemoryAlignment - 1) &
                                   ~(kDirectMemoryAlignment - 1);
        int32_t rc = sceKernelAllocateDirectMemory(
            0, sceKernelGetDirectMemorySize(), alignedSize,
            kDirectMemoryAlignment, memType, &physicalAddress);
        if (rc != 0) {
            return false;
        }
        rc = sceKernelMapDirectMemory(
            &baseAddress, alignedSize, prot, 0, physicalAddress,
            kDirectMemoryAlignment);
        if (rc != 0 || baseAddress == nullptr) {
            sceKernelReleaseDirectMemory(physicalAddress, alignedSize);
            baseAddress = nullptr;
            physicalAddress = 0;
            return false;
        }
        memset(baseAddress, 0, alignedSize);
        totalSize = alignedSize;
        return true;
    }

    void release() {
        if (baseAddress) {
            sceKernelMunmap(baseAddress, totalSize);
            sceKernelReleaseDirectMemory(physicalAddress, totalSize);
            baseAddress = nullptr;
            physicalAddress = 0;
            totalSize = 0;
        }
    }
};

/// Private implementation of GfxManager. Holds all OpenOrbis SDK resources.
struct GfxManager::Impl {
    // --- VideoOut -----------------------------------------------------------
    int32_t videoOutHandle = -1;

    // --- Direct memory ------------------------------------------------------
    off_t   directMemOffset = 0;   ///< Physical offset of the direct-memory block.
    size_t  directMemSize = 0;     ///< Total size of the direct-memory block.
    void*   directMemBase = nullptr; ///< Mapped virtual address of the block.

    // --- Framebuffer geometry (tiled) ---------------------------------------
    size_t   frameBufferSize = 0;  ///< Per-frame tiled buffer size (bytes).
    uint32_t fbPitch = 0;          ///< Tiled pitch in pixels (for VideoOut).
    GnmRenderTarget rt;            ///< Framebuffer render-target descriptor.

    // --- GPU clear resources ------------------------------------------------
    DmemBlock cmdMem;              ///< GPU-clear command buffer (Garlic).
    DmemBlock labelMem;            ///< EOP sync label (Onion).
    volatile uint64_t* eopLabel = nullptr;  ///< EOP sync label (CPU-pollable).

    // --- Frame resources ----------------------------------------------------
    FrameResource frames[kFrameCount];

    // --- Display buffer addresses (for VideoOut registration) ---------------
    void* displayBufferAddresses[kFrameCount] = {};

    // --- Synchronization ----------------------------------------------------
    OrbisKernelEqueue flipQueue = 0;  ///< Event queue for flip completion.
    uint64_t flipCount = 0;           ///< Total flips submitted.
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

GfxManager::GfxManager()
    : m_state(LifecycleState::Constructed)
    , m_frameIndex(0)
    , m_frameActive(false)
    , m_fbWidth(1920)
    , m_fbHeight(1080)
    , m_isNeoMode(false)
    , m_is4KOutput(false)
    , m_impl(new Impl())
{
}

GfxManager::~GfxManager() {
    // Destructor is a final safety net. Orderly shutdown must be performed
    // by calling shutdown() explicitly before destruction.
    if (m_state != LifecycleState::ShutDown) {
        shutdown();
    }
    delete m_impl;
    m_impl = nullptr;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

GfxResult GfxManager::initialize() {
    if (m_state == LifecycleState::Initialized ||
        m_state == LifecycleState::FrameActive) {
        return GfxResult(GfxResultCategory::AlreadyInitialized);
    }
    if (m_state != LifecycleState::Constructed) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    m_state = LifecycleState::Initializing;
    gfxDiagPrint("[gfx] initialization start");

    Impl* impl = m_impl;

    // 1. Open VideoOut for the primary user.
    int32_t userId = ORBIS_VIDEO_USER_MAIN;
    sceUserServiceGetInitialUser(&userId);
    impl->videoOutHandle = sceVideoOutOpen(userId, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (impl->videoOutHandle < 0) {
        gfxDiagPrint("[gfx] initialization failed: video out open");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::VideoOutOpenFailure,
                         static_cast<uint32_t>(impl->videoOutHandle));
    }

    // 1b. Detect PS4 Pro / 4K capability.
    //     sceKernelIsNeoMode() is a static hardware query (PS4 Pro / NEO).
    //     sceVideoOutGetResolutionStatus() reports the current VideoOut
    //     resolution; height > 1080 means the display is actually running a
    //     high-resolution (>1080p) output mode.
    //
    //     These two capabilities are INDEPENDENT:
    //       - isPro()       = isNeoMode()  (drives H.265/HEVC decode, codec)
    //       - is4KEnabled() = is4KOutput() (drives framebuffer, UI, video)
    //
    //     A Pro on a 1080p screen is isPro()=true but is4KEnabled()=false.
    m_isNeoMode = (sceKernelIsNeoMode() != 0);

    OrbisVideoOutResolutionStatus resStatus;
    memset(&resStatus, 0, sizeof(resStatus));
    int32_t resRc = sceVideoOutGetResolutionStatus(impl->videoOutHandle,
                                                   &resStatus);
    m_is4KOutput = (resRc == ORBIS_OK && resStatus.height > kBaseDisplayHeight);

    // Determine the framebuffer dimensions.
    // When the output is actually >1080p (is4KOutput()), allocate 4K
    // framebuffers. Otherwise stay at 1080p.
    if (m_is4KOutput) {
        m_fbWidth  = k4KDisplayWidth;
        m_fbHeight = k4KDisplayHeight;
    } else {
        m_fbWidth  = kBaseDisplayWidth;
        m_fbHeight = kBaseDisplayHeight;
    }

    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[gfx] capability: neoMode=%d 4kOutput=%d pro=%d 4kEnabled=%d fb=%ux%u",
                 m_isNeoMode ? 1 : 0, m_is4KOutput ? 1 : 0,
                 m_isNeoMode ? 1 : 0,
                 m_is4KOutput ? 1 : 0,
                 m_fbWidth, m_fbHeight);
        gfxDiagPrint(buf);
    }

    // 2. Set the flip rate to 60 Hz.
    int32_t rc = sceVideoOutSetFlipRate(impl->videoOutHandle, ORBIS_VIDEO_OUT_FLIP_60HZ);
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] initialization failed: set flip rate");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::GraphicsInitFailure,
                         static_cast<uint32_t>(rc));
    }

    // 3. Create the flip event queue.
    rc = sceKernelCreateEqueue(&impl->flipQueue, "gfx flip queue");
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] initialization failed: create flip queue");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::SynchronizationFailure,
                         static_cast<uint32_t>(rc));
    }

    // 4. Register the flip event on the queue.
    rc = sceVideoOutAddFlipEvent(impl->flipQueue, impl->videoOutHandle, nullptr);
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] initialization failed: add flip event");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::SynchronizationFailure,
                         static_cast<uint32_t>(rc));
    }

    // 5. Create the framebuffer render-target descriptor and compute the
    //    tiled buffer size.
    //    The framebuffer is a TILING_MODE_TILE surface, so its memory footprint
    //    is larger than width*height*4 (the tiled layout pads to tile
    //    boundaries). The authoritative size comes from the GNM render-target
    //    byte-size computation (sceGnmRtCalcByteSize), which accounts for the
    //    tile mode. The descriptor is created once with the display dimensions
    //    and the display tile-mode hint; its base address is set per frame.
    //    UNORM (not sRGB) so the GPU writes the raw computed bytes; the display
    //    applies the sRGB interpretation via the VideoOut A8B8G8R8_SRGB buffer
    //    attribute.
    //
    //    The render target's min GPU mode must match the actual console GPU
    //    mode (sceGnmGpuMode()). Per the SCE VideoOut documentation, a
    //    TILING_MODE_TILE display buffer cannot use kGpuModeBase on a NEO
    //    (PS4 Pro) console — the tile layout differs between BASE and NEO, so
    //    using BASE on a Pro at 4K would misalign the buffer (garbled/blocky
    //    output). sceGnmGpuMode() returns NEO on a Pro and BASE on a base PS4,
    //    so it correctly selects the tile layout for both.
    memset(&impl->rt, 0, sizeof(impl->rt));
    {
        GnmRenderTargetCreateInfo rtCi;
        memset(&rtCi, 0, sizeof(rtCi));
        rtCi.width = m_fbWidth;
        rtCi.height = m_fbHeight;
        rtCi.pitch = 0;
        rtCi.numslices = 1;
        rtCi.colorfmt = GNM_FMT_R8G8B8A8_UNORM;
        rtCi.colortilemodehint = kFramebufferTileMode;
        rtCi.mingpumode = sceGnmGpuMode();
        rtCi.numsamples = 1;
        rtCi.numfragments = 1;
        GnmError gerr = sceGnmCreateRenderTarget(&impl->rt, &rtCi);
        if (gerr != GNM_ERROR_OK) {
            gfxDiagPrint("[gfx] initialization failed: render target create");
            m_state = LifecycleState::Stopping;
            shutdown();
            return GfxResult(GfxResultCategory::GraphicsInitFailure,
                             static_cast<uint32_t>(gerr));
        }
    }

    // Compute the per-frame tiled buffer size and the tiled pitch (in pixels)
    // used for the VideoOut buffer attribute.
    {
        uint64_t rtSize = 0;
        uint32_t rtAlign = 0;
        GnmError gerr = sceGnmRtCalcByteSize(&rtSize, &rtAlign, &impl->rt);
        if (gerr != GNM_ERROR_OK || rtSize == 0) {
            gfxDiagPrint("[gfx] initialization failed: render target size calc");
            m_state = LifecycleState::Stopping;
            shutdown();
            return GfxResult(GfxResultCategory::GraphicsInitFailure,
                             static_cast<uint32_t>(gerr));
        }
        impl->frameBufferSize = static_cast<size_t>(rtSize);
        impl->fbPitch = sceGnmRtGetPitch(&impl->rt);
    }

    // 6. Allocate and map direct memory for the framebuffers.
    const size_t totalSize = impl->frameBufferSize * kFrameCount;
    const size_t alignedSize = (totalSize + kDirectMemoryAlignment - 1) &
                               ~(kDirectMemoryAlignment - 1);

    rc = sceKernelAllocateDirectMemory(
        0, sceKernelGetDirectMemorySize(), alignedSize, kDirectMemoryAlignment,
        kDirectMemoryType, &impl->directMemOffset);
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] initialization failed: direct memory allocation");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::MemoryAllocationFailure,
                         static_cast<uint32_t>(rc));
    }
    impl->directMemSize = alignedSize;

    rc = sceKernelMapDirectMemory(
        &impl->directMemBase, alignedSize, kDirectMemoryProt, 0,
        impl->directMemOffset, kDirectMemoryAlignment);
    if (rc != ORBIS_OK || impl->directMemBase == nullptr) {
        gfxDiagPrint("[gfx] initialization failed: direct memory mapping");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::MemoryAllocationFailure,
                         static_cast<uint32_t>(rc));
    }

    // 7. Set up the frame resources.
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        FrameResource& frame = impl->frames[i];
        frame.colorBuffer = static_cast<uint8_t*>(impl->directMemBase) +
                            static_cast<size_t>(i) * impl->frameBufferSize;
        frame.flipArg = 0;
        frame.submitted = false;
        impl->displayBufferAddresses[i] = frame.colorBuffer;
    }

    // 8. Register the display buffers with VideoOut as TILING_MODE_TILE.
    //    The pixel pitch passed to VideoOut is the render target's tiled pitch
    //    (not the linear width), matching how the tiled buffer is laid out.
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(
        &attribute,
        ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_TILE,
        ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
        m_fbWidth, m_fbHeight,
        impl->fbPitch);  // tiled pitch in pixels

    rc = sceVideoOutRegisterBuffers(
        impl->videoOutHandle, 0, impl->displayBufferAddresses, kFrameCount,
        &attribute);
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] initialization failed: buffer registration");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::BufferRegistrationFailure,
                         static_cast<uint32_t>(rc));
    }

    // 9. Allocate the GPU-clear command buffer (Garlic) and EOP label (Onion).
    //    The framebuffer is tiled, so clear() must issue a GPU DMA fill rather
    //    than a CPU memset. This command buffer and EOP label are the minimal
    //    GNM resources needed for that fill.
    if (!impl->cmdMem.allocate(kCommandBytes, kCmdMemType, kCmdMemProt)) {
        gfxDiagPrint("[gfx] initialization failed: clear cmd alloc");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::MemoryAllocationFailure);
    }
    if (!impl->labelMem.allocate(0x1000, kLabelMemType, kLabelMemProt)) {
        gfxDiagPrint("[gfx] initialization failed: clear label alloc");
        m_state = LifecycleState::Stopping;
        shutdown();
        return GfxResult(GfxResultCategory::MemoryAllocationFailure);
    }
    impl->eopLabel = (volatile uint64_t*)impl->labelMem.baseAddress;

    // 10. Establish initial frame state.
    m_frameIndex = 0;
    m_frameActive = false;
    impl->flipCount = 0;

    m_state = LifecycleState::Initialized;
    gfxDiagPrint("[gfx] initialization success");
    return GfxResult();
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

GfxResult GfxManager::beginFrame() {
    if (m_state != LifecycleState::Initialized) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }
    if (m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    Impl* impl = m_impl;
    FrameResource& frame = impl->frames[m_frameIndex];

    // Ensure the frame resource is safe to reuse: the display must no longer
    // be showing this buffer (flip completed).
    if (frame.submitted) {
        // Wait for the flip that presented this buffer to complete.
        // Use the flip event queue to avoid busy-spinning.
        OrbisVideoOutFlipStatus flipStatus;
        int32_t rc = sceVideoOutGetFlipStatus(impl->videoOutHandle, &flipStatus);
        if (rc != ORBIS_OK) {
            gfxDiagPrint("[gfx] beginFrame: flip status query failed");
            return GfxResult(GfxResultCategory::SynchronizationFailure,
                             static_cast<uint32_t>(rc));
        }

        // The flip is complete when the flip status count has advanced past
        // the flipArg we submitted for this frame.
        if (flipStatus.flipArg < frame.flipArg) {
            // Wait on the flip event queue until the flip completes.
            OrbisKernelEvent evt;
            int32_t count = 0;
            for (;;) {
                rc = sceKernelWaitEqueue(impl->flipQueue, &evt, 1, &count, nullptr);
                if (rc != ORBIS_OK) {
                    gfxDiagPrint("[gfx] beginFrame: flip event wait failed");
                    return GfxResult(GfxResultCategory::SynchronizationFailure,
                                     static_cast<uint32_t>(rc));
                }
                rc = sceVideoOutGetFlipStatus(impl->videoOutHandle, &flipStatus);
                if (rc != ORBIS_OK) {
                    gfxDiagPrint("[gfx] beginFrame: flip status query failed");
                    return GfxResult(GfxResultCategory::SynchronizationFailure,
                                     static_cast<uint32_t>(rc));
                }
                if (flipStatus.flipArg >= frame.flipArg) {
                    break;
                }
            }
        }
    }

    // The frame is now confirmed display-safe for reuse.
    frame.submitted = false;

    m_frameActive = true;
    m_state = LifecycleState::FrameActive;

    return GfxResult();
}

GfxResult GfxManager::clear(uint32_t color) {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    Impl* impl = m_impl;
    FrameResource& frame = impl->frames[m_frameIndex];

    if (!impl->cmdMem.baseAddress || !impl->eopLabel) {
        return GfxResult(GfxResultCategory::MemoryAllocationFailure);
    }

    // GPU DMA constant fill of the whole tiled framebuffer.
    //
    // The framebuffer is a TILING_MODE_TILE A8B8G8R8_SRGB surface, so a CPU
    // memset would not produce a correct visual clear (the tiled layout
    // rearranges pixels spatially). A DMA constant fill writes the same 32-bit
    // value to every 32-bit word in the buffer; because every pixel is a 32-bit
    // word regardless of the tile layout, this sets every pixel to the clear
    // color. The color is 0xAABBGGRR in memory (byte 0 = R), matching the
    // A8B8G8R8_SRGB framebuffer.
    //
    // The GPU address of the WC_GARLIC framebuffer is its CPU virtual address
    // (the same address the GPU renderers use as the render-target base).
    GnmCommandBuffer cmd = sceGnmCmdInit(impl->cmdMem.baseAddress,
                                         kCommandBytes, NULL, NULL);

    // Fill the entire per-frame tiled buffer with the clear color.
    sceGnmDrawCmdFillMemory(&cmd,
                            (uint64_t)(uintptr_t)frame.colorBuffer,
                            static_cast<uint32_t>(impl->frameBufferSize),
                            color);

    // EOP synchronization so the fill is complete before the caller proceeds.
    *impl->eopLabel = 0;
    sceGnmDrawCmdEventWriteEop(&cmd, GNM_FLUSH_AND_INV_CB_DATA_TS,
                               (uint64_t)(uintptr_t)impl->eopLabel,
                               GNM_DATA_SEL_SEND_DATA64, 1);

    const uint32_t dcbSize =
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr);
    void* dcbAddrs[1] = {cmd.beginptr};
    uint32_t dcbSizes[1] = {dcbSize};

    int res = sceGnmSubmitCommandBuffers(1, dcbAddrs, dcbSizes, NULL, NULL);
    if (res < 0) {
        return GfxResult(GfxResultCategory::SubmissionFailure,
                         static_cast<uint32_t>(res));
    }
    sceGnmSubmitDone();

    uint32_t spins = 0;
    while (*impl->eopLabel != 1) {
        if (++spins > kEopTimeoutSpins) {
            return GfxResult(GfxResultCategory::SynchronizationFailure);
        }
    }

    return GfxResult();
}

GfxResult GfxManager::endFrame() {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    // Frame preparation is complete. The framebuffer contents are ready to be
    // presented. No additional work is required here; present() performs the
    // flip.

    return GfxResult();
}

GfxResult GfxManager::present() {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    Impl* impl = m_impl;
    FrameResource& frame = impl->frames[m_frameIndex];

    // Make GPU writes visible to the display hardware.
    // For WC_GARLIC memory, this flushes the write-combining buffers.
    sceGnmFlushGarlic();

    // Signal that all command-buffer submissions are complete.
    // This is the OpenOrbis equivalent of sce::Gnm::submitDone() in the SCE
    // reference. It is REQUIRED to prevent CPU_FAULT_SUBMITDONE_TIMEOUT_IN_
    // SUSPEND_ASYNC when the app is suspended/killed while the Videodec2
    // decoder has outstanding GPU work: the suspend path waits for the GPU's
    // submit-done state, and without this call it never arrives.
    sceGnmSubmitDone();

    // Assign a unique flipArg for this frame.
    int64_t flipArg = static_cast<int64_t>(impl->flipCount);
    frame.flipArg = flipArg;

    // Submit the flip to VideoOut.
    int32_t rc = sceVideoOutSubmitFlip(
        impl->videoOutHandle, static_cast<int32_t>(m_frameIndex),
        kFlipMode, flipArg);
    if (rc != ORBIS_OK) {
        gfxDiagPrint("[gfx] present: submit flip failed");
        m_frameActive = false;
        m_state = LifecycleState::Initialized;
        return GfxResult(GfxResultCategory::PresentationFailure,
                         static_cast<uint32_t>(rc));
    }

    // Mark the frame as submitted; it must not be reused until the flip
    // completes.
    frame.submitted = true;

    // Advance the frame index and mark the frame inactive.
    impl->flipCount++;
    m_frameIndex = (m_frameIndex + 1) % kFrameCount;
    m_frameActive = false;
    m_state = LifecycleState::Initialized;

    return GfxResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void GfxManager::shutdown() {
    if (m_state == LifecycleState::ShutDown) {
        return;
    }

    gfxDiagPrint("[gfx] shutdown start");

    Impl* impl = m_impl;

    // 1. Stop initiating new frames.
    m_frameActive = false;

    // 2. Wait for any outstanding display usage to complete so resources
    //    are not released while still in use.
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        FrameResource& frame = impl->frames[i];
        if (frame.submitted) {
            OrbisVideoOutFlipStatus flipStatus;
            int32_t rc = sceVideoOutGetFlipStatus(impl->videoOutHandle, &flipStatus);
            if (rc == ORBIS_OK && flipStatus.flipArg < frame.flipArg) {
                OrbisKernelEvent evt;
                int32_t count = 0;
                for (;;) {
                    rc = sceKernelWaitEqueue(impl->flipQueue, &evt, 1, &count, nullptr);
                    if (rc != ORBIS_OK) {
                        break;  // give up waiting; proceed with teardown
                    }
                    rc = sceVideoOutGetFlipStatus(impl->videoOutHandle, &flipStatus);
                    if (rc != ORBIS_OK || flipStatus.flipArg >= frame.flipArg) {
                        break;
                    }
                }
            }
        }
    }

    // 3. Unregister display buffers.
    if (impl->videoOutHandle >= 0) {
        sceVideoOutUnregisterBuffers(impl->videoOutHandle,
                                     kVideoOutBufferAttributeIndex);
    }

    // 4. Delete the flip event queue.
    if (impl->flipQueue != 0) {
        sceKernelDeleteEqueue(impl->flipQueue);
        impl->flipQueue = 0;
    }

    // 5. Close VideoOut.
    if (impl->videoOutHandle >= 0) {
        sceVideoOutClose(impl->videoOutHandle);
        impl->videoOutHandle = -1;
    }

    // 6. Release the GPU-clear command buffer and EOP label resources.
    impl->eopLabel = nullptr;
    impl->cmdMem.release();
    impl->labelMem.release();

    // 7. Unmap and release the direct memory.
    if (impl->directMemBase != nullptr) {
        sceKernelMunmap(impl->directMemBase, impl->directMemSize);
        impl->directMemBase = nullptr;
    }
    if (impl->directMemOffset != 0 && impl->directMemSize != 0) {
        sceKernelReleaseDirectMemory(impl->directMemOffset, impl->directMemSize);
        impl->directMemOffset = 0;
        impl->directMemSize = 0;
    }

    // 8. Reset frame state.
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        impl->frames[i].colorBuffer = nullptr;
        impl->frames[i].flipArg = 0;
        impl->frames[i].submitted = false;
        impl->displayBufferAddresses[i] = nullptr;
    }
    impl->frameBufferSize = 0;
    impl->fbPitch = 0;
    m_frameIndex = 0;

    m_state = LifecycleState::ShutDown;
    gfxDiagPrint("[gfx] shutdown complete");
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool GfxManager::isInitialized() const {
    return m_state == LifecycleState::Initialized ||
           m_state == LifecycleState::FrameActive;
}

uint32_t GfxManager::getFramebufferWidth() const {
    return m_fbWidth;
}

uint32_t GfxManager::getFramebufferHeight() const {
    return m_fbHeight;
}

uint32_t GfxManager::currentFrameIndex() const {
    return m_frameIndex;
}

void* GfxManager::currentFramebuffer() {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return nullptr;
    }
    Impl* impl = m_impl;
    return impl->frames[m_frameIndex].colorBuffer;
}

size_t GfxManager::framebufferStrideBytes() const {
    // Legacy linear-layout accessor. The framebuffer is now TILING_MODE_TILE,
    // so the CPU does not write pixels row-by-row and this value is not used
    // by the GPU renderers (they derive the tiled pitch from the render-target
    // descriptor). Retained for the linear-layout contract: width * 4.
    return static_cast<size_t>(m_fbWidth) * 4u;
}

// ---------------------------------------------------------------------------
// PS4 Pro / 4K capability queries
// ---------------------------------------------------------------------------

bool GfxManager::isNeoMode() const {
    return m_isNeoMode;
}

bool GfxManager::is4KOutput() const {
    return m_is4KOutput;
}

bool GfxManager::isPro() const {
    return m_isNeoMode;
}

bool GfxManager::is4KEnabled() const {
    return m_is4KOutput;
}

} // namespace ps4

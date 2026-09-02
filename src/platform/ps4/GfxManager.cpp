// Copyright (C) 2026 fourletterworkshop.com
// This file is part of Moonlight DX for PlayStation 4.
// Moonlight DX for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight DX for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight DX for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
//

// === GfxManager.cpp ==========================================================
//
// OpenOrbis PS4 graphics/presentation foundation.
//
// Implements the GfxManager platform graphics subsystem using the low-level
// facilities available through the OpenOrbis SDK:
//   - VideoOut initialization and buffer registration.
//   - Direct-memory allocation and mapping for GPU-visible framebuffers.
//   - CPU-side framebuffer writes (no GNM command buffers).
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
#include <orbis/GnmDriver.h>
#include <orbis/_types/errors.h>

#include <cstdio>               // printf, fflush, snprintf
#include <cstring>              // memset

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Native PS4 display resolution.
static constexpr uint32_t kDisplayWidth  = 1920;
static constexpr uint32_t kDisplayHeight = 1080;

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

/// Private implementation of GfxManager. Holds all OpenOrbis SDK resources.
struct GfxManager::Impl {
    // --- VideoOut -----------------------------------------------------------
    int32_t videoOutHandle = -1;

    // --- Direct memory ------------------------------------------------------
    off_t   directMemOffset = 0;   ///< Physical offset of the direct-memory block.
    size_t  directMemSize = 0;     ///< Total size of the direct-memory block.
    void*   directMemBase = nullptr; ///< Mapped virtual address of the block.

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

    // 5. Allocate and map direct memory for the framebuffers.
    const size_t frameBufferSize = static_cast<size_t>(kDisplayWidth) *
                                   static_cast<size_t>(kDisplayHeight) * 4u;
    const size_t totalSize = frameBufferSize * kFrameCount;
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

    // 6. Set up the frame resources.
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        FrameResource& frame = impl->frames[i];
        frame.colorBuffer = static_cast<uint8_t*>(impl->directMemBase) +
                            static_cast<size_t>(i) * frameBufferSize;
        frame.flipArg = 0;
        frame.submitted = false;
        impl->displayBufferAddresses[i] = frame.colorBuffer;
    }

    // 7. Register the display buffers with VideoOut.
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(
        &attribute,
        ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
        ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
        kDisplayWidth, kDisplayHeight,
        kDisplayWidth);  // pitch in pixels (linear, no padding)

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

    // 8. Establish initial frame state.
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

    // CPU-fill the entire active framebuffer with the given color.
    // The framebuffer is A8B8G8R8_SRGB; in memory the byte order is RGBA
    // (byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A), so the 32-bit value
    // is stored as 0xAABBGGRR in little-endian memory.
    uint32_t* dst = static_cast<uint32_t*>(frame.colorBuffer);
    const size_t pixelCount = static_cast<size_t>(kDisplayWidth) *
                              static_cast<size_t>(kDisplayHeight);
    for (size_t i = 0; i < pixelCount; ++i) {
        dst[i] = color;
    }

    return GfxResult();
}

GfxResult GfxManager::endFrame() {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    // CPU-side frame preparation is complete. The framebuffer contents are
    // ready to be presented. No additional work is required here for the
    // CPU-write path; present() performs the flip.

    return GfxResult();
}

GfxResult GfxManager::present() {
    if (m_state != LifecycleState::FrameActive || !m_frameActive) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    Impl* impl = m_impl;
    FrameResource& frame = impl->frames[m_frameIndex];

    // Make CPU writes visible to the display hardware.
    // For WC_GARLIC memory, this flushes the write-combining buffers.
    sceGnmFlushGarlic();

    // Signal that all CPU-side command-buffer submissions are complete.
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

    // 6. Unmap and release the direct memory.
    if (impl->directMemBase != nullptr) {
        sceKernelMunmap(impl->directMemBase, impl->directMemSize);
        impl->directMemBase = nullptr;
    }
    if (impl->directMemOffset != 0 && impl->directMemSize != 0) {
        sceKernelReleaseDirectMemory(impl->directMemOffset, impl->directMemSize);
        impl->directMemOffset = 0;
        impl->directMemSize = 0;
    }

    // 7. Reset frame state.
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        impl->frames[i].colorBuffer = nullptr;
        impl->frames[i].flipArg = 0;
        impl->frames[i].submitted = false;
        impl->displayBufferAddresses[i] = nullptr;
    }
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
    return kDisplayWidth;
}

uint32_t GfxManager::getFramebufferHeight() const {
    return kDisplayHeight;
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
    // Linear A8B8G8R8_SRGB, tightly packed (no padding).
    return static_cast<size_t>(kDisplayWidth) * 4u;
}

} // namespace ps4

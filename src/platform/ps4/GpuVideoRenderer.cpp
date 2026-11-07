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

// === GpuVideoRenderer.cpp ===================================================
//
// OpenOrbis PS4 — GPU NV12→BGRA fullscreen video pass.
//
// Implements the GPU NV12→BGRA conversion using OpenGNM, following the exact
// state/setup patterns from the working OpenGNM triangle smoke test
// (gpu_triangle_smoke.c):
//   - TILE render target over the WC_GARLIC VideoOut framebuffer (the
//     framebuffer is registered with VideoOut as TILING_MODE_TILE).
//   - Positive viewport Y scale (Y inversion done in the vertex shader).
//   - VS+PS binding, fullscreen triangle draw, EOP synchronization.
//
// Shader loading reuses the freegnm-examples misc.c approach (parses the
// PsslBinaryHeader | GnmShaderFileHeader | GnmShaderCommonData | sbaddr[2] |
// shader-code layout, copies the code into Garlic, builds the GnmVsShader /
// GnmPsShader register structs).
//
// The decoded Videodec2 NV12 Y/UV planes are bound directly as linear
// GPU-readable textures (no intermediate NV12 copy). The pixel shader performs
// the BT.709 NV12→BGRA conversion, producing the byte/channel order (byte 0 =
// R) required by the A8B8G8R8_SRGB VideoOut framebuffer.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "GpuVideoRenderer.h"

// --- OpenOrbis SDK headers (private to this translation unit) ---------------
#include <orbis/libkernel.h>   // sceKernelAllocateDirectMemory, sceKernelMapDirectMemory

// --- OpenGNM headers (private to this translation unit) ---------------------
// gnmdriver.h provides sceGnmSubmitCommandBuffers / sceGnmSubmitDone /
// sceGnmFlushGarlic (the OpenGNM driver API). Do NOT also include
// <orbis/GnmDriver.h> — its declarations of the same sceGnm* functions use
// different types and conflict with the OpenGNM ones.
#include <gnm_commandbuffer.h>
#include <gnm_drawcommandbuffer.h>
#include <gnm_rendertarget.h>
#include <gnm_shaderbinary.h>
#include <gnm_texture.h>
#include <gnm_sampler.h>
#include <gnm_buffer.h>
#include <gnmdriver.h>
#include <platform.h>   // sceGnmGpuMode()
#include <compat/pssl.h>

#include <cstdio>               // printf, fflush, snprintf
#include <cstring>              // memset, memcpy
#include <cstdlib>              // malloc, free

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Path prefix for the packaged shader binaries (runtime /app0 mount).
static const char* kShaderDir = "/app0/assets/misc/";
/// Fullscreen vertex shader binary name.
static const char* kVsShaderName = "video_vv.sb";
/// NV12→BGRA pixel shader binary name.
static const char* kPsShaderName = "video_p.sb";
/// Overlay blend vertex shader binary name.
static const char* kOverlayVsShaderName = "overlay_vv.sb";
/// Overlay blend pixel shader binary name.
static const char* kOverlayPsShaderName = "overlay_p.sb";

/// Direct-memory alignment for the Garlic block (2 MiB, per the SDK samples).
static constexpr size_t kDirectMemoryAlignment = 0x200000;
/// Direct-memory type for GPU-visible shader/command memory (WC Garlic).
static constexpr int32_t kDirectMemoryType = ORBIS_KERNEL_WC_GARLIC;
/// Memory protection for the Garlic block (CPU RW + GPU RW).
static constexpr int32_t kDirectMemoryProt = ORBIS_KERNEL_PROT_CPU_RW |
                                             ORBIS_KERNEL_PROT_GPU_ALL;

/// Direct-memory type for the EOP sync label (WB Onion, CPU-pollable).
static constexpr int32_t kLabelMemType = ORBIS_KERNEL_WB_ONION;
/// Memory protection for the EOP label (CPU RW + GPU RW).
static constexpr int32_t kLabelMemProt = ORBIS_KERNEL_PROT_CPU_RW |
                                         ORBIS_KERNEL_PROT_GPU_ALL;

/// Command buffer size in bytes. Passed to sceGnmCmdInit as the byte size.
/// Generous enough for the fullscreen draw with texture/sampler/constant
/// buffer binding (the smoke test uses 4096 bytes for a simpler draw).
static constexpr uint32_t kCommandBytes = 16384;

/// EOP spin timeout (iterations) before declaring a hang.
static constexpr uint32_t kEopTimeoutSpins = 100000000u;

/// Display resolution (must match GfxManager).
static constexpr uint32_t kDisplayWidth = 1920;
static constexpr uint32_t kDisplayHeight = 1080;

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void gvrDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// A minimal linear allocator over a single direct-memory block. Used to carve
/// the Garlic block into shader code + command buffer + constant buffer, and
/// the Onion block into the EOP label. Mirrors the smoke test's manual carve.
struct DmemBlock {
    void*  baseAddress = nullptr;
    off_t  physicalAddress = 0;
    size_t totalSize = 0;
    size_t offset = 0;

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
        offset = 0;
        return true;
    }

    void release() {
        if (baseAddress) {
            sceKernelMunmap(baseAddress, totalSize);
            sceKernelReleaseDirectMemory(physicalAddress, totalSize);
            baseAddress = nullptr;
            physicalAddress = 0;
            totalSize = 0;
            offset = 0;
        }
    }

    void* alloc(size_t size, size_t alignment) {
        if (!baseAddress) {
            return nullptr;
        }
        const size_t alignedOffset = (offset + alignment - 1) & ~(alignment - 1);
        if (alignedOffset + size > totalSize) {
            return nullptr;
        }
        void* ptr = static_cast<uint8_t*>(baseAddress) + alignedOffset;
        offset = alignedOffset + size;
        return ptr;
    }
};

/// Private implementation of GpuVideoRenderer. Holds all OpenGNM resources.
struct GpuVideoRenderer::Impl {
    // --- Lifecycle ----------------------------------------------------------
    bool initialized = false;
    uint32_t rtWidth = 0;   ///< Output render target width (pixels).
    uint32_t rtHeight = 0;  ///< Output render target height (pixels).

    // --- Direct memory ------------------------------------------------------
    DmemBlock garlicMem;   ///< Shader code + command buffer + constant buffer.
    DmemBlock onionMem;    ///< EOP sync label (CPU-pollable).
    DmemBlock overlayMem;  ///< Dedicated overlay staging/test block (Garlic).

    // --- Shaders ------------------------------------------------------------
    GnmVsShader* vs = nullptr;   ///< Vertex shader register struct (heap).
    GnmPsShader* ps = nullptr;   ///< Pixel shader register struct (heap).
    GnmVsShader* overlayVs = nullptr;  ///< Overlay blend VS (heap).
    GnmPsShader* overlayPs = nullptr;  ///< Overlay blend PS (heap).

    // --- Overlay blend state ------------------------------------------------
    GnmTexture overlayTex;       ///< Overlay texture descriptor (rebuilt per call).
    GnmSampler overlaySampler;   ///< Overlay sampler (point, clamp).

    // --- Stage 1 test overlay (static, Garlic) ------------------------------
    void* testOverlayMem = nullptr;   ///< Static test overlay RGBA8 (Garlic).
    uint32_t testOverlayW = 0;        ///< Test overlay width.
    uint32_t testOverlayH = 0;        ///< Test overlay height.

    // --- Command buffer + EOP ----------------------------------------------
    void* cmdMem = nullptr;              ///< Command buffer memory (Garlic).
    volatile uint64_t* eopLabel = nullptr;  ///< EOP sync label (Onion).

    // --- Constant buffer ----------------------------------------------------
    void* cbMem = nullptr;           ///< Constant buffer memory (Garlic).

    // --- Render target (over the current framebuffer) ----------------------
    GnmRenderTarget rt;              ///< Descriptor; base updated per frame.

    // --- Textures (bound per frame over the decoded NV12 planes) -----------
    GnmTexture yTex;                 ///< Y plane (R8 linear).
    GnmTexture uvTex;                ///< UV plane (RG8 linear).
    GnmSampler samplerY;             ///< Y sampler (point).
    GnmSampler samplerUV;            ///< UV sampler (point).

    // --- Extended User Data (EUD) block ------------------------------------
    // The compiled shader places texUV, smpUV, and cb_video in the Extended
    // User Data (EUD) because they exceed the 16 user-data registers. The EUD
    // is a tightly-packed block of descriptors accessed via a pointer bound at
    // user-data register 12 (PTR_EXTENDEDUSERDATA). Layout: [uvTex][samplerUV]
    // [cbVideo], each 16 bytes.
    void* eudMem = nullptr;          ///< EUD block (Garlic).
};

// ---------------------------------------------------------------------------
// Shader loading (reuses the freegnm misc.c approach)
// ---------------------------------------------------------------------------

namespace {

/// Read a whole file into a malloc'd buffer. Returns 0 on success.
int readFile(const char* path, void** outdata, size_t* outsize) {
    int fd = sceKernelOpen(path, 0x0000 /* O_RDONLY */, 0);
    if (fd < 0) {
        return fd;
    }
    const off_t filesize = sceKernelLseek(fd, 0, 0x0002 /* SEEK_END */);
    sceKernelLseek(fd, 0, 0x0000 /* SEEK_SET */);
    void* data = malloc(static_cast<size_t>(filesize));
    if (!data) {
        sceKernelClose(fd);
        return -1;
    }
    int res = static_cast<int>(sceKernelRead(fd, data, static_cast<size_t>(filesize)));
    sceKernelClose(fd);
    if (res < 0) {
        free(data);
        return res;
    }
    *outdata = data;
    *outsize = static_cast<size_t>(filesize);
    return 0;
}

/// Parse a .sb shader binary into its common-data header and code pointer.
/// Mirrors freegnm misc.c parseshader().
bool parseShader(const GnmShaderCommonData** outdata, const uint32_t** outcode,
                 const void* data, size_t datasize) {
    const size_t expectedHeaderSize =
        sizeof(PsslBinaryHeader) + sizeof(GnmShaderFileHeader) +
        sizeof(GnmShaderCommonData) + sizeof(uint32_t) * 2;
    if (datasize < expectedHeaderSize) {
        return false;
    }
    const GnmShaderFileHeader* shheader =
        (const GnmShaderFileHeader*)((const uint8_t*)data +
                                     sizeof(PsslBinaryHeader));
    const GnmShaderCommonData* shcommon =
        (const GnmShaderCommonData*)((const uint8_t*)shheader +
                                     sizeof(GnmShaderFileHeader));
    const uint32_t* sbaddr =
        (const uint32_t*)((const uint8_t*)shcommon +
                          sizeof(GnmShaderCommonData));
    if (shheader->magic != GNM_SHADER_FILE_HEADER_ID) {
        return false;
    }
    if (sbaddr[0] & 3) {
        return false;
    }
    if (sbaddr[1] != 0xffffffff) {
        return false;   // shader has already been patched
    }
    *outdata = shcommon;
    *outcode = (const uint32_t*)((const uint8_t*)shcommon + sbaddr[0]);
    return true;
}

/// Load a vertex shader .sb into a GnmVsShader register struct, copying its
/// code into the given Garlic block. Mirrors freegnm misc.c loadvshader().
bool loadVsShader(GnmVsShader** outvs, DmemBlock* garlic,
                  const char* path) {
    void* data = nullptr;
    size_t size = 0;
    if (readFile(path, &data, &size) != 0) {
        return false;
    }
    const GnmShaderCommonData* vscommon = nullptr;
    const uint32_t* vscode = nullptr;
    if (!parseShader(&vscommon, &vscode, data, size)) {
        free(data);
        return false;
    }
    const uint32_t vscodesize = sceGnmShaderCommonCodeSize(vscommon);
    const GnmVsShader* vsshader = (const GnmVsShader*)vscommon;
    const uint32_t vsshadersize = sceGnmVsShaderCalcSize(vsshader);

    GnmVsShader* vs = (GnmVsShader*)malloc(vsshadersize);
    if (!vs) {
        free(data);
        return false;
    }
    void* vsgpucode = garlic->alloc(vscodesize, GNM_ALIGNMENT_SHADER_BYTES);
    if (!vsgpucode) {
        free(vs);
        free(data);
        return false;
    }
    memcpy(vs, vsshader, vsshadersize);
    memcpy(vsgpucode, vscode, vscodesize);
    sceGnmVsRegsSetAddress(&vs->registers, vsgpucode);
    free(data);
    *outvs = vs;
    return true;
}

/// Load a pixel shader .sb into a GnmPsShader register struct, copying its
/// code into the given Garlic block. Mirrors freegnm misc.c loadpshader().
bool loadPsShader(GnmPsShader** outps, DmemBlock* garlic,
                  const char* path) {
    void* data = nullptr;
    size_t size = 0;
    if (readFile(path, &data, &size) != 0) {
        return false;
    }
    const GnmShaderCommonData* pscommon = nullptr;
    const uint32_t* pscode = nullptr;
    if (!parseShader(&pscommon, &pscode, data, size)) {
        free(data);
        return false;
    }
    const uint32_t pscodesize = sceGnmShaderCommonCodeSize(pscommon);
    const GnmPsShader* psshader = (const GnmPsShader*)pscommon;
    const uint32_t psshadersize = sceGnmPsShaderCalcSize(psshader);

    GnmPsShader* ps = (GnmPsShader*)malloc(psshadersize);
    if (!ps) {
        free(data);
        return false;
    }
    void* psgpucode = garlic->alloc(pscodesize, GNM_ALIGNMENT_SHADER_BYTES);
    if (!psgpucode) {
        free(ps);
        free(data);
        return false;
    }
    memcpy(ps, psshader, psshadersize);
    memcpy(psgpucode, pscode, pscodesize);
    sceGnmPsRegsSetAddress(&ps->registers, psgpucode);
    ps->registers.spibaryccntl = 0;
    free(data);
    *outps = ps;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

GpuVideoRenderer::GpuVideoRenderer()
    : m_impl(new Impl())
{
}

GpuVideoRenderer::~GpuVideoRenderer() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::initialize(uint32_t fbWidth, uint32_t fbHeight) {
    if (m_impl->initialized) {
        if (m_impl->rtWidth == fbWidth && m_impl->rtHeight == fbHeight) {
            return GpuVideoResult();
        }
        // Output resolution changed across stream sessions: tear down existing
        // resources and recreate the render target at the new dimensions.
        shutdown();
    }
    Impl* impl = m_impl;

    if (fbWidth == 0 || fbHeight == 0) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }

    // --- 1. Allocate the Garlic block (shader code + cmd buffer + cb) ------
    // Layout:
    //   [0x0000 .. 0x1000)  VS shader code
    //   [0x1000 .. 0x2000)  PS shader code
    //   [0x2000 .. 0x2000+kCommandBytes)  command buffer
    //   [0x2000+kCommandBytes ..)         constant buffer
    const size_t garlicSize = 0x2000 + kCommandBytes + 0x1000;
    if (!impl->garlicMem.allocate(garlicSize, kDirectMemoryType,
                                  kDirectMemoryProt)) {
        gvrDiagPrint("[gpu-video] garlic alloc failed");
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }

    // --- 2. Allocate the Onion block for the EOP label ---------------------
    if (!impl->onionMem.allocate(0x1000, kLabelMemType, kLabelMemProt)) {
        gvrDiagPrint("[gpu-video] onion alloc failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }

    // --- 3. Load the shaders ------------------------------------------------
    char vsPath[256];
    char psPath[256];
    snprintf(vsPath, sizeof(vsPath), "%s%s", kShaderDir, kVsShaderName);
    snprintf(psPath, sizeof(psPath), "%s%s", kShaderDir, kPsShaderName);

    if (!loadVsShader(&impl->vs, &impl->garlicMem, vsPath)) {
        gvrDiagPrint("[gpu-video] vertex shader load failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::ShaderLoadFailure);
    }
    if (!loadPsShader(&impl->ps, &impl->garlicMem, psPath)) {
        gvrDiagPrint("[gpu-video] pixel shader load failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::ShaderLoadFailure);
    }

    // --- 3b. Load the overlay blend shaders (optional) ---------------------
    // Used by blendOverlay() to GPU-composite the cached in-stream overlay
    // over the video. Non-fatal if absent (the caller falls back to the CPU
    // overlay blend).
    {
        char ovVsPath[256];
        char ovPsPath[256];
        snprintf(ovVsPath, sizeof(ovVsPath), "%s%s", kShaderDir,
                 kOverlayVsShaderName);
        snprintf(ovPsPath, sizeof(ovPsPath), "%s%s", kShaderDir,
                 kOverlayPsShaderName);
        if (!loadVsShader(&impl->overlayVs, &impl->garlicMem, ovVsPath)) {
            gvrDiagPrint("[gpu-video] overlay VS not present (CPU overlay fallback)");
            impl->overlayVs = nullptr;
        }
        if (!loadPsShader(&impl->overlayPs, &impl->garlicMem, ovPsPath)) {
            gvrDiagPrint("[gpu-video] overlay PS not present (CPU overlay fallback)");
            impl->overlayPs = nullptr;
        }
    }

    // --- 4. Carve the command buffer, constant buffer, and EUD block -------
    impl->cmdMem = impl->garlicMem.alloc(kCommandBytes, 4);
    impl->cbMem = impl->garlicMem.alloc(0x1000, 256);
    // EUD block: tightly-packed [uvTex][samplerUV][cbVideo] = 48 bytes.
    impl->eudMem = impl->garlicMem.alloc(0x100, 256);
    if (!impl->cmdMem || !impl->cbMem || !impl->eudMem) {
        gvrDiagPrint("[gpu-video] cmd/cb/eud carve failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }

    // --- 5. EOP label -------------------------------------------------------
    impl->eopLabel = (volatile uint64_t*)impl->onionMem.alloc(
        sizeof(uint64_t), sizeof(uint64_t));
    if (!impl->eopLabel) {
        gvrDiagPrint("[gpu-video] eop label alloc failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }

    // --- 6. Create the render target descriptor over the framebuffer -------
    // The base address is set per frame in renderFrame() (the framebuffer is
    // double-buffered). The descriptor is created once with the OUTPUT
    // (framebuffer) dimensions and a TILE tile mode matching the VideoOut
    // framebuffer (TILING_MODE_TILE). Per the SCE VideoOut documentation,
    // SCE_VIDEO_OUT_TILING_MODE_TILE corresponds to kTileModeDisplay_2dThin,
    // so the render target uses GNM_TM_DISPLAY_2D_THIN. UNORM (not sRGB) so
    // the GPU writes the raw computed bytes; the display applies the sRGB
    // interpretation via the VideoOut A8B8G8R8_SRGB buffer attribute.
    //
    // The decoded NV12 frame is bound per-frame at its native (stream)
    // resolution and scaled to fill this output render target via the shader
    // UVs (nearest-neighbor), so the same renderer handles 1080p->1080p,
    // 1080p->4K upscale, and 4K->4K.
    //
    // The min GPU mode must match the actual console GPU mode (sceGnmGpuMode):
    // a TILING_MODE_TILE display buffer cannot use BASE on a NEO (PS4 Pro)
    // console, and the tile layout differs between BASE and NEO. sceGnmGpuMode()
    // returns NEO on a Pro and BASE on a base PS4, so it selects the correct
    // tile layout for both 1080p (base) and 4K (NEO) output.
    memset(&impl->rt, 0, sizeof(impl->rt));
    GnmRenderTargetCreateInfo rtCi;
    memset(&rtCi, 0, sizeof(rtCi));
    rtCi.width = fbWidth;
    rtCi.height = fbHeight;
    rtCi.pitch = 0;
    rtCi.numslices = 1;
    rtCi.colorfmt = GNM_FMT_R8G8B8A8_UNORM;
    rtCi.colortilemodehint = GNM_TM_DISPLAY_2D_THIN;
    rtCi.mingpumode = sceGnmGpuMode();
    rtCi.numsamples = 1;
    rtCi.numfragments = 1;
    GnmError gerr = sceGnmCreateRenderTarget(&impl->rt, &rtCi);
    if (gerr != GNM_ERROR_OK) {
        gvrDiagPrint("[gpu-video] render target create failed");
        shutdown();
        return GpuVideoResult(GpuVideoResultCategory::RenderTargetFailure,
                              static_cast<uint32_t>(gerr));
    }

    // --- 7. Create the samplers (point/nearest, clamp) ---------------------
    memset(&impl->samplerY, 0, sizeof(impl->samplerY));
    impl->samplerY.clampx = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->samplerY.clampy = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->samplerY.clampz = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->samplerY.xymagfilter = GNM_FILTER_POINT;
    impl->samplerY.xyminfilter = GNM_FILTER_POINT;
    impl->samplerY.mipfilter = GNM_MIPFILTER_NONE;

    impl->samplerUV = impl->samplerY;

    // --- 7b. Create the overlay sampler (point/nearest, clamp) -------------
    // The overlay texture is a full-screen RGBA surface sampled 1:1 (no
    // scaling), so point filtering with clamp is correct.
    memset(&impl->overlaySampler, 0, sizeof(impl->overlaySampler));
    impl->overlaySampler.clampx = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->overlaySampler.clampy = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->overlaySampler.clampz = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->overlaySampler.xymagfilter = GNM_FILTER_POINT;
    impl->overlaySampler.xyminfilter = GNM_FILTER_POINT;
    impl->overlaySampler.mipfilter = GNM_MIPFILTER_NONE;

    // --- 7c. Allocate a dedicated overlay block and build the Stage 1 test --
    // The test overlay is a full 1920x1080 RGBA8 surface (~8.3 MB), far larger
    // than the packed garlicMem block (~28 KB), so it gets its own dedicated
    // WC_GARLIC block. Held in WC_GARLIC (no CPU writes per frame) so
    // blendTestOverlay() isolates the GPU blend mechanism from Onion coherency.
    // The panel is semi-transparent so the video shows through it, proving
    // source-over alpha blending works.
    {
        impl->testOverlayW = kDisplayWidth;
        impl->testOverlayH = kDisplayHeight;
        const size_t ovBytes = static_cast<size_t>(impl->testOverlayW) *
                               impl->testOverlayH * 4u;
        if (!impl->overlayMem.allocate(ovBytes, kDirectMemoryType,
                                       kDirectMemoryProt)) {
            gvrDiagPrint("[gpu-video] overlay block alloc failed");
            shutdown();
            return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
        }
        impl->testOverlayMem = impl->overlayMem.baseAddress;
        // Transparent everywhere.
        memset(impl->testOverlayMem, 0, ovBytes);
        // Draw a semi-transparent dark panel (RGBA 40,40,40,180) in the
        // top-left corner (roughly the exit-hint / stats panel region).
        const int px = 64, py = 88, pw = 300, ph = 200;
        uint8_t* ov = static_cast<uint8_t*>(impl->testOverlayMem);
        for (int yy = py; yy < py + ph; ++yy) {
            uint8_t* row = ov + static_cast<size_t>(yy) *
                                     impl->testOverlayW * 4u;
            for (int xx = px; xx < px + pw; ++xx) {
                uint8_t* p = row + static_cast<size_t>(xx) * 4u;
                p[0] = 40;   // R
                p[1] = 40;   // G
                p[2] = 40;   // B
                p[3] = 180;  // A (semi-transparent)
            }
        }
        // Draw a solid opaque accent bar at the panel's top edge (alpha 255)
        // so fully-opaque blending is also exercised.
        for (int xx = px; xx < px + pw; ++xx) {
            uint8_t* p = ov + static_cast<size_t>(py) * impl->testOverlayW * 4u +
                         static_cast<size_t>(xx) * 4u;
            p[0] = 220; p[1] = 60; p[2] = 60; p[3] = 255;
        }
        gvrDiagPrint("[gpu-video] Stage1 test overlay built (dedicated Garlic block)");
    }

    // --- 8. Flush the Garlic writes so the GPU sees the shader code --------
    // The shader code was copied into WC_GARLIC memory. WC writes are
    // write-combining and must be flushed before the GPU reads them, otherwise
    // the GPU may fetch garbage shader code (which can fault reading address 0).
    sceGnmFlushGarlic();

    impl->rtWidth = fbWidth;
    impl->rtHeight = fbHeight;
    impl->initialized = true;
    gvrDiagPrint("[gpu-video] initialized");
    return GpuVideoResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void GpuVideoRenderer::shutdown() {
    if (!m_impl) {
        return;
    }
    Impl* impl = m_impl;

    if (impl->vs) {
        free(impl->vs);
        impl->vs = nullptr;
    }
    if (impl->ps) {
        free(impl->ps);
        impl->ps = nullptr;
    }
    if (impl->overlayVs) {
        free(impl->overlayVs);
        impl->overlayVs = nullptr;
    }
    if (impl->overlayPs) {
        free(impl->overlayPs);
        impl->overlayPs = nullptr;
    }
    impl->cmdMem = nullptr;
    impl->cbMem = nullptr;
    impl->eudMem = nullptr;
    impl->eopLabel = nullptr;

    impl->garlicMem.release();
    impl->onionMem.release();
    impl->overlayMem.release();
    impl->testOverlayMem = nullptr;

    impl->rtWidth = 0;
    impl->rtHeight = 0;
    impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool GpuVideoRenderer::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::renderFrame(const GpuVideoFrame& frame,
                                             void* framebuffer,
                                             uint32_t fbWidth,
                                             uint32_t fbHeight) {
    // Video-only path: no overlay.
    return renderFrameWithOverlay(frame, nullptr, 0, 0, 0,
                                  framebuffer, fbWidth, fbHeight);
}

GpuVideoResult GpuVideoRenderer::renderFrameWithOverlay(
    const GpuVideoFrame& frame, const void* overlayRgba,
    uint32_t ow, uint32_t oh, uint32_t opitch,
    void* framebuffer, uint32_t fbWidth, uint32_t fbHeight) {
    if (!m_impl || !m_impl->initialized) {
        return GpuVideoResult(GpuVideoResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    // --- Validate the frame ------------------------------------------------
    if (!frame.yPlane || !frame.uvPlane || frame.width == 0 ||
        frame.height == 0 || frame.surfaceHeight == 0 || frame.pitch == 0) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }
    if (!framebuffer) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }
    // If an overlay is requested, validate it and require the overlay shaders.
    const bool haveOverlay = (overlayRgba != nullptr);
    if (haveOverlay) {
        if (!impl->overlayVs || !impl->overlayPs) {
            return GpuVideoResult(GpuVideoResultCategory::ShaderLoadFailure);
        }
        if (ow == 0 || oh == 0 || opitch < ow * 4u) {
            return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
        }
    }

    // --- Set the render target base to the current framebuffer slot --------
    sceGnmRtSetBaseAddr(&impl->rt, framebuffer);

    // --- Build the Y and UV linear textures over the decoded planes --------
    // IMPORTANT: for a GNM_ARRAY_LINEAR_GENERAL texture the GPU derives the
    // level-0 row stride from the TEXTURE WIDTH, not from ci->pitch.
    // sceGpaTpInit() sets tp.linearwidth = tex->width, and
    // gpaComputeSurfaceAddrFromCoordLinear() uses linearwidth as the pitch;
    // ci->pitch (basetiledpitch) is only consulted for mip levels > 0.
    //
    // The decoder's Y plane row stride is exactly the pitch it reports in
    // framePitchInBytes. This is NOT always the 256-aligned width: the decoder
    // chooses the surface pitch per stream/codec. Observed values:
    //   HEVC 1080p -> 2048 (256-aligned, padded)
    //   AVC  1080p -> 1920 (tightly packed, no padding)
    //   4K         -> 3840
    // The texture WIDTH must equal this reported pitch (in texels), because for
    // a GNM_ARRAY_LINEAR_GENERAL texture the GPU derives the row stride from the
    // texture width. Using any other value (e.g. rounding 1920 up to 2048) makes
    // the GPU read rows at the wrong stride -> horizontal banding/shear.
    //
    // The pixel shader then samples only the leftmost visible fraction via
    // uvScale.x = visibleWidth / pitch.
    //
    // Y plane: R8, pitch x surfaceHeight, width = pitch (row stride in texels).
    //
    // Defensive guard: the stride must be at least the visible width. If the
    // decoder reports pitch < width (malformed), use the visible width so the
    // image is not clipped (uvScaleX is forced to 1.0 in that case).
    const uint32_t yTexWidth = (frame.pitch >= frame.width) ? frame.pitch
                                                            : frame.width;
    GnmTextureCreateInfo yCi;
    memset(&yCi, 0, sizeof(yCi));
    yCi.texturetype = GNM_TEXTURE_2D;
    yCi.width = yTexWidth;            // row stride in texels (>= visible width)
    yCi.height = frame.surfaceHeight;
    yCi.pitch = yTexWidth;
    yCi.depth = 1;
    yCi.nummiplevels = 1;
    yCi.numslices = 1;
    yCi.format = GNM_FMT_R8_UNORM;
    // The decoder writes a plain linear (row-major) frame buffer. Use the
    // GENERAL linear display mode, not the ALIGNED variant: DISPLAY_LINEAR_
    // ALIGNED imposes pitch/base-address alignment constraints that the
    // decoder's buffer does not satisfy. DISPLAY_LINEAR_GENERAL has no such
    // constraint and matches the decoder's plain linear layout.
    yCi.tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL;
    // The min GPU mode must match the actual console GPU mode (sceGnmGpuMode):
    // the tile/address layout differs between BASE and NEO (PS4 Pro). This must
    // match the render target's mingpumode (also sceGnmGpuMode()).
    yCi.mingpumode = sceGnmGpuMode();
    yCi.numfragments = 1;
    GnmError gerr = sceGnmCreateTexture(&impl->yTex, &yCi);
    if (gerr != GNM_ERROR_OK) {
        return GpuVideoResult(GpuVideoResultCategory::RenderTargetFailure,
                              static_cast<uint32_t>(gerr));
    }
    sceGnmTexSetBaseAddress(&impl->yTex, frame.yPlane);
    sceGnmTexSetMemoryType(&impl->yTex, GNM_MEMORY_READONLY, false);

    // UV plane: RG8, (pitch/2) x (surfaceHeight/2), width = pitch/2 (row
    // stride in texels; RG8 = 2 bytes/texel, so pitch/2 texels = pitch bytes).
    // See the Y texture above for why the width must equal the row stride.
    const GnmDataFormat rg8 = GNM_DATA_FORMAT_INIT(
        GNM_IMG_DATA_FORMAT_8_8, GNM_IMG_NUM_FORMAT_UNORM,
        GNM_CHAN_X, GNM_CHAN_Y, GNM_CHAN_CONSTANT0, GNM_CHAN_CONSTANT1);
    GnmTextureCreateInfo uvCi;
    memset(&uvCi, 0, sizeof(uvCi));
    uvCi.texturetype = GNM_TEXTURE_2D;
    uvCi.width = yTexWidth / 2;       // row stride in texels (>= visible width/2)
    uvCi.height = frame.surfaceHeight / 2;
    uvCi.pitch = yTexWidth / 2;
    uvCi.depth = 1;
    uvCi.nummiplevels = 1;
    uvCi.numslices = 1;
    uvCi.format = rg8;
    // Plain linear display mode (see the Y texture above).
    uvCi.tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL;
    // Match the console GPU mode (see the Y texture above).
    uvCi.mingpumode = sceGnmGpuMode();
    uvCi.numfragments = 1;
    gerr = sceGnmCreateTexture(&impl->uvTex, &uvCi);
    if (gerr != GNM_ERROR_OK) {
        return GpuVideoResult(GpuVideoResultCategory::RenderTargetFailure,
                              static_cast<uint32_t>(gerr));
    }
    sceGnmTexSetBaseAddress(&impl->uvTex, frame.uvPlane);
    sceGnmTexSetMemoryType(&impl->uvTex, GNM_MEMORY_READONLY, false);

    // --- Fill the constant buffer (BT.709, limited range) ------------------
    // Matches the NV12→BGRA math (see video_p.pssl).
    //   yuvOffset = (16/255, 128/255, 128/255, 0)
    //   uvScale   = (width/pitch, height/surfaceHeight, 0, 0)  visible fraction
    //               of the pitch-wide / surface-tall Y/UV textures (the textures
    //               are created at the decoder's reported pitch and surface
    //               height, so the GPU reads rows at the decoder's true stride
    //               and the shader samples only the visible region)
    //   uvOffset  = (0, 0, 0, 0)
    //   chromaOffset = (0, 0, 0, 0)  (left-sited chroma needs no offset)
    //   coeffR = (149/128, 0, 229/128, 0)
    //   coeffG = (149/128, -27/128, -68/128, 0)
    //   coeffB = (149/128, 270/128, 0, 0)
    float* cb = static_cast<float*>(impl->cbMem);
    const float lumaScale = 149.0f / 128.0f;   // 1.1640625
    const float crR = 229.0f / 128.0f;         // 1.7890625
    const float cbG = 27.0f / 128.0f;          // 0.2109375
    const float crG = 68.0f / 128.0f;          // 0.53125
    const float cbB = 270.0f / 128.0f;         // 2.109375

    // Visible fraction of the pitch-wide textures. The Y texture width is the
    // reported pitch (yTexWidth) and the UV texture width is yTexWidth/2, so
    // the same ratio (visibleWidth / pitch) applies to both planes.
    //
    // Defensive guard: the pitch must be at least the visible width. If a
    // malformed report gives a smaller pitch, fall back to full coverage
    // (scale 1.0) rather than producing a scale > 1 (which would sample outside
    // the texture) or a divide-by-zero.
    const float uvScaleX =
        (yTexWidth >= frame.width && yTexWidth != 0)
            ? static_cast<float>(frame.width) / static_cast<float>(yTexWidth)
            : 1.0f;

    // Visible fraction of the surface-tall textures. The decoder's surface is
    // taller than the visible picture (e.g. a 1080-line stream decodes to a
    // 1088-line surface; the extra rows are bottom padding the decoder fills
    // with a repeat of the last real row). The Y texture height is
    // frame.surfaceHeight and the UV texture height is frame.surfaceHeight/2,
    // so the same ratio (visibleHeight / surfaceHeight) applies to both planes.
    // Sampling only the top fraction avoids showing the padding rows and avoids
    // vertically compressing the picture (1080/1088 at 1080p).
    //
    // Defensive guard: the surface must be at least the visible height. If a
    // malformed report gives a smaller surface, fall back to full coverage
    // (scale 1.0) rather than producing a scale > 1 or a divide-by-zero.
    const float uvScaleY =
        (frame.surfaceHeight >= frame.height && frame.surfaceHeight != 0)
            ? static_cast<float>(frame.height) /
                  static_cast<float>(frame.surfaceHeight)
            : 1.0f;

    // yuvOffset
    cb[0] = 16.0f / 255.0f;
    cb[1] = 128.0f / 255.0f;
    cb[2] = 128.0f / 255.0f;
    cb[3] = 0.0f;
    // uvScale
    cb[4] = uvScaleX;
    cb[5] = uvScaleY;
    cb[6] = 0.0f;
    cb[7] = 0.0f;
    // uvOffset
    cb[8] = 0.0f;
    cb[9] = 0.0f;
    cb[10] = 0.0f;
    cb[11] = 0.0f;
    // chromaOffset (chroma-siting correction).
    //
    // The decoder outputs NV12 with LEFT-sited (MPEG-2 style) 4:2:0 chroma:
    // the chroma sample for output pixels (2k, 2k+1) is the UV texel k. With
    // point sampling, sampling the UV texture at uvCoord.x = x/2 (in UV texels)
    // selects exactly texel floor(x/2), which is the correct left-sited chroma
    // for output pixel x. Therefore NO half-texel offset is needed: adding one
    // would shift odd output pixels onto the next chroma texel (a 1-texel
    // comb/zigzag artifact). Set the offset to 0.
    cb[12] = 0.0f;
    cb[13] = 0.0f;
    cb[14] = 0.0f;
    cb[15] = 0.0f;
    // coeffR
    cb[16] = lumaScale;
    cb[17] = 0.0f;
    cb[18] = crR;
    cb[19] = 0.0f;
    // coeffG
    cb[20] = lumaScale;
    cb[21] = -cbG;
    cb[22] = -crG;
    cb[23] = 0.0f;
    // coeffB
    cb[24] = lumaScale;
    cb[25] = cbB;
    cb[26] = 0.0f;
    cb[27] = 0.0f;

    // Flush the constant-buffer writes (WC_GARLIC) so the GPU sees them.
    sceGnmFlushGarlic();

    // --- Fill the Extended User Data (EUD) block ---------------------------
    // The compiled shader places texUV, smpUV, and cb_video in the EUD at
    // DWORD offsets (startreg - 16): texUV@0, smpUV@8, cb_video@12. In bytes:
    //   texUV  at byte 0
    //   smpUV  at byte 32
    //   cb_video at byte 48
    {
        uint8_t* eud = static_cast<uint8_t*>(impl->eudMem);
        memcpy(eud + 0, &impl->uvTex, sizeof(impl->uvTex));
        memcpy(eud + 32, &impl->samplerUV, sizeof(impl->samplerUV));
        GnmBuffer cbBufEud = sceGnmCreateConstBuffer(impl->cbMem, 0x1000);
        memcpy(eud + 48, &cbBufEud, sizeof(cbBufEud));
        // Flush the EUD writes (WC_GARLIC) so the GPU sees them.
        sceGnmFlushGarlic();
    }

    // --- Build the overlay texture (if an overlay is requested) ------------
    // The overlay surface is RGBA8 (byte order R,G,B,A) matching the
    // A8B8G8R8_SRGB framebuffer. Bind as R8G8B8A8_UNORM linear. The surface is
    // CPU-written Onion memory; the caller is responsible for making it
    // GPU-visible (copying into WC_GARLIC staging + flush) before calling.
    if (haveOverlay) {
        memset(&impl->overlayTex, 0, sizeof(impl->overlayTex));
        GnmTextureCreateInfo ovCi;
        memset(&ovCi, 0, sizeof(ovCi));
        ovCi.texturetype = GNM_TEXTURE_2D;
        ovCi.width = ow;
        ovCi.height = oh;
        ovCi.depth = 1;
        ovCi.pitch = opitch / 4u;   // pitch in texels (RGBA8 = 4 bytes/texel)
        ovCi.nummiplevels = 1;
        ovCi.numslices = 1;
        ovCi.format = GNM_FMT_R8G8B8A8_UNORM;
        ovCi.tilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
        // Match the console GPU mode (see the Y texture above).
        ovCi.mingpumode = sceGnmGpuMode();
        ovCi.numfragments = 1;
        gerr = sceGnmCreateTexture(&impl->overlayTex, &ovCi);
        if (gerr != GNM_ERROR_OK) {
            return GpuVideoResult(GpuVideoResultCategory::RenderTargetFailure,
                                  static_cast<uint32_t>(gerr));
        }
        sceGnmTexSetBaseAddress(&impl->overlayTex,
                                const_cast<void*>(overlayRgba));
        sceGnmTexSetMemoryType(&impl->overlayTex, GNM_MEMORY_READONLY, false);
    }

    // --- Build the draw command buffer -------------------------------------
    GnmCommandBuffer cmd = sceGnmCmdInit(impl->cmdMem, kCommandBytes, NULL, NULL);
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    // Primitive setup: no cull.
    GnmPrimitiveSetup primsetup;
    memset(&primsetup, 0, sizeof(primsetup));
    primsetup.cullmode = GNM_CULL_NONE;
    primsetup.frontface = GNM_FACE_CCW;
    primsetup.frontmode = GNM_FILL_SOLID;
    primsetup.backmode = GNM_FILL_SOLID;
    primsetup.frontoffsetmode = false;
    primsetup.backoffsetmode = false;
    primsetup.vertexwindowoffsetenable = false;
    primsetup.provokemode = GNM_PROVOKINGVTX_FIRST;
    primsetup.perspectivecorrectiondisable = false;
    sceGnmDrawCmdSetPrimitiveSetup(&cmd, &primsetup);

    sceGnmDrawCmdSetRenderTarget(&cmd, 0, &impl->rt);
    sceGnmDrawCmdSetRenderTargetMask(&cmd, 0xf);

    // Viewport covering the full screen (positive Y scale).
    GnmSetViewportInfo vp;
    memset(&vp, 0, sizeof(vp));
    vp.dmin = 0.0f;
    vp.dmax = 1.0f;
    vp.scale[0] = fbWidth * 0.5f;
    vp.scale[1] = fbHeight * 0.5f;
    vp.scale[2] = 0.5f;
    vp.offset[0] = fbWidth * 0.5f;
    vp.offset[1] = fbHeight * 0.5f;
    vp.offset[2] = 0.5f;
    sceGnmDrawCmdSetViewport(&cmd, 0, &vp);

    // Viewport transform control: enable scale/offset, invert W.
    GnmViewportTransformControl vtctrl;
    memset(&vtctrl, 0, sizeof(vtctrl));
    vtctrl.scalex = 1;
    vtctrl.offsetx = 1;
    vtctrl.scaley = 1;
    vtctrl.offsety = 1;
    vtctrl.scalez = 1;
    vtctrl.offsetz = 1;
    vtctrl.perspectivedividexy = 0;
    vtctrl.perspectivedividez = 0;
    vtctrl.invertw = 1;
    sceGnmDrawCmdSetViewportTransformControl(&cmd, &vtctrl);
    sceGnmDrawCmdSetScreenScissor(&cmd, 0, 0, static_cast<int32_t>(fbWidth),
                                  static_cast<int32_t>(fbHeight));

    sceGnmDrawCmdSetVsShader(&cmd, &impl->vs->registers, 0);
    sceGnmDrawCmdSetPsShader(&cmd, &impl->ps->registers);

    // Wire the VS's exported varyings (UV) to the PS's inputs. The PS takes a
    // TEXCOORD0 input from the VS; without this the PS input registers are not
    // set up and the shader may read from an invalid location.
    sceGnmDrawCmdSetPsInputUsage(
        &cmd, sceGnmVsShaderExportSemanticTable(impl->vs),
        impl->vs->numexportsemantics,
        sceGnmPsShaderInputSemanticTable(impl->ps),
        impl->ps->numinputsemantics);

    // Bind the PS shader resources.
    //
    // The compiled shader's input usage table reports each resource's start
    // register. Resources within the 16 user-data registers (0-15) are bound
    // directly; resources beyond reg 15 live in the Extended User Data (EUD),
    // accessed via a pointer bound at reg 12 (PTR_EXTENDEDUSERDATA).
    //
    //   texY  (IMM_RESOURCE)    startreg=0   -> bind directly
    //   smpY  (IMM_SAMPLER)     startreg=8   -> bind directly
    //   PTR_EXTENDEDUSERDATA    startreg=12  -> EUD pointer
    //   texUV (IMM_RESOURCE)    startreg=16  -> in EUD block
    //   smpUV (IMM_SAMPLER)     startreg=24  -> in EUD block
    //   cb_video (IMM_CONSTBUFFER) startreg=28 -> in EUD block
    //
    // OpenGNM's Set*UserData startuserdataslot parameter is the PS
    // user-data REGISTER INDEX directly (getuserdataslot() computes
    // basereg + slot*4 bytes). So the shader's startreg values are passed
    // as-is:
    //   texY  -> startreg 0
    //   smpY  -> startreg 8
    //   EUD pointer -> startreg 12
    sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0, &impl->yTex);
    sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8, &impl->samplerY);
    sceGnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_PS, 12, impl->eudMem);

    sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
    sceGnmDrawCmdDrawIndexAuto(&cmd, 3);

    // --- Append the overlay blend draw to the SAME command buffer ----------
    // The overlay draw reads the framebuffer as its blend destination. Because
    // it is in the same submission and executes after the video draw in program
    // order, the GPU guarantees the overlay's destination reads see the
    // completed video writes (no cross-submission read-after-write hazard).
    if (haveOverlay) {
        // Source-over alpha blend (straight alpha):
        // out = src*srcA + dst*(1-srcA). Matches the CPU straight-alpha
        // source-over blend. Color and alpha both use SRC_ALPHA /
        // ONE_MINUS_SRC_ALPHA.
        GnmBlendControl blend;
        memset(&blend, 0, sizeof(blend));
        blend.blendenabled = true;
        blend.colorfunc = GNM_COMB_DST_PLUS_SRC;
        blend.colorsrcmult = GNM_BLEND_SRC_ALPHA;
        blend.colordstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.alphafunc = GNM_COMB_DST_PLUS_SRC;
        blend.alphasrcmult = GNM_BLEND_SRC_ALPHA;
        blend.alphadstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.separatealphaenable = false;
        sceGnmDrawCmdSetBlendControl(&cmd, 0, &blend);

        sceGnmDrawCmdSetVsShader(&cmd, &impl->overlayVs->registers, 0);
        sceGnmDrawCmdSetPsShader(&cmd, &impl->overlayPs->registers);

        // Wire the overlay VS's exported varyings (UV) to the PS's inputs.
        sceGnmDrawCmdSetPsInputUsage(
            &cmd, sceGnmVsShaderExportSemanticTable(impl->overlayVs),
            impl->overlayVs->numexportsemantics,
            sceGnmPsShaderInputSemanticTable(impl->overlayPs),
            impl->overlayPs->numinputsemantics);

        // Bind the overlay texture (startreg 0) and sampler (startreg 8).
        sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0,
                                       &impl->overlayTex);
        sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8,
                                       &impl->overlaySampler);

        sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
        sceGnmDrawCmdDrawIndexAuto(&cmd, 3);
    }

    // --- EOP synchronization (single EOP after both draws) -----------------
    *impl->eopLabel = 0;
    sceGnmDrawCmdEventWriteEop(&cmd, GNM_FLUSH_AND_INV_CB_DATA_TS,
                               (uint64_t)(uintptr_t)impl->eopLabel,
                               GNM_DATA_SEL_SEND_DATA64, 1);

    const uint32_t dcbSize =
        (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr);
    void* dcbAddrs[1] = {cmd.beginptr};
    uint32_t dcbSizes[1] = {dcbSize};

    // --- Submit the draw command buffer (video + optional overlay) ---------
    int res = sceGnmSubmitCommandBuffers(1, dcbAddrs, dcbSizes, NULL, NULL);
    if (res < 0) {
        return GpuVideoResult(GpuVideoResultCategory::SubmissionFailure,
                              static_cast<uint32_t>(res));
    }
    sceGnmSubmitDone();

    // --- Wait for EOP (GPU draw complete) ----------------------------------
    uint32_t spins = 0;
    while (*impl->eopLabel != 1) {
        if (++spins > kEopTimeoutSpins) {
            return GpuVideoResult(GpuVideoResultCategory::SynchronizationFailure);
        }
    }

    return GpuVideoResult();
}

// ---------------------------------------------------------------------------
// Overlay blend (GPU source-over composite of the cached overlay surface)
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::blendOverlay(const void* overlayRgba,
                                              uint32_t ow, uint32_t oh,
                                              uint32_t opitch,
                                              void* framebuffer,
                                              uint32_t fbWidth,
                                              uint32_t fbHeight) {
    if (!m_impl || !m_impl->initialized) {
        return GpuVideoResult(GpuVideoResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    // The overlay blend shaders must be available.
    if (!impl->overlayVs || !impl->overlayPs) {
        return GpuVideoResult(GpuVideoResultCategory::ShaderLoadFailure);
    }
    if (!overlayRgba || ow == 0 || oh == 0 || opitch < ow * 4u) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }
    if (!framebuffer) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }

    // --- Set the render target base to the current framebuffer slot --------
    sceGnmRtSetBaseAddr(&impl->rt, framebuffer);

    // --- Build the overlay texture over the CPU RGBA surface ---------------
    // The overlay surface is RGBA8 (byte order R,G,B,A) matching the
    // A8B8G8R8_SRGB framebuffer. Bind as R8G8B8A8_UNORM linear. The surface is
    // CPU-written Onion memory; the caller is responsible for making it
    // GPU-visible (copying into WC_GARLIC staging + flush) before calling.
    memset(&impl->overlayTex, 0, sizeof(impl->overlayTex));
    GnmTextureCreateInfo ovCi;
    memset(&ovCi, 0, sizeof(ovCi));
    ovCi.texturetype = GNM_TEXTURE_2D;
    ovCi.width = ow;
    ovCi.height = oh;
    ovCi.depth = 1;
    ovCi.pitch = opitch / 4u;   // pitch in texels (RGBA8 = 4 bytes/texel)
    ovCi.nummiplevels = 1;
    ovCi.numslices = 1;
    ovCi.format = GNM_FMT_R8G8B8A8_UNORM;
    ovCi.tilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
    // Match the console GPU mode (see the Y texture above).
    ovCi.mingpumode = sceGnmGpuMode();
    ovCi.numfragments = 1;
    GnmError gerr = sceGnmCreateTexture(&impl->overlayTex, &ovCi);
    if (gerr != GNM_ERROR_OK) {
        return GpuVideoResult(GpuVideoResultCategory::RenderTargetFailure,
                              static_cast<uint32_t>(gerr));
    }
    sceGnmTexSetBaseAddress(&impl->overlayTex,
                            const_cast<void*>(overlayRgba));
    sceGnmTexSetMemoryType(&impl->overlayTex, GNM_MEMORY_READONLY, false);

    // --- Build the draw command buffer -------------------------------------
    GnmCommandBuffer cmd = sceGnmCmdInit(impl->cmdMem, kCommandBytes, NULL, NULL);
    sceGnmDrawCmdInitDefaultHardwareState(&cmd);

    // Primitive setup: no cull.
    GnmPrimitiveSetup primsetup;
    memset(&primsetup, 0, sizeof(primsetup));
    primsetup.cullmode = GNM_CULL_NONE;
    primsetup.frontface = GNM_FACE_CCW;
    primsetup.frontmode = GNM_FILL_SOLID;
    primsetup.backmode = GNM_FILL_SOLID;
    primsetup.frontoffsetmode = false;
    primsetup.backoffsetmode = false;
    primsetup.vertexwindowoffsetenable = false;
    primsetup.provokemode = GNM_PROVOKINGVTX_FIRST;
    primsetup.perspectivecorrectiondisable = false;
    sceGnmDrawCmdSetPrimitiveSetup(&cmd, &primsetup);

    sceGnmDrawCmdSetRenderTarget(&cmd, 0, &impl->rt);
    sceGnmDrawCmdSetRenderTargetMask(&cmd, 0xf);

    // Source-over alpha blend (straight alpha): out = src*srcA + dst*(1-srcA).
    // Matches the CPU straight-alpha source-over blend used by the overlay
    // compositor. Color and alpha both use SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
    GnmBlendControl blend;
    memset(&blend, 0, sizeof(blend));
    blend.blendenabled = true;
    blend.colorfunc = GNM_COMB_DST_PLUS_SRC;
    blend.colorsrcmult = GNM_BLEND_SRC_ALPHA;
    blend.colordstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.alphafunc = GNM_COMB_DST_PLUS_SRC;
    blend.alphasrcmult = GNM_BLEND_SRC_ALPHA;
    blend.alphadstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.separatealphaenable = false;
    sceGnmDrawCmdSetBlendControl(&cmd, 0, &blend);

    // Viewport covering the full screen (positive Y scale).
    GnmSetViewportInfo vp;
    memset(&vp, 0, sizeof(vp));
    vp.dmin = 0.0f;
    vp.dmax = 1.0f;
    vp.scale[0] = fbWidth * 0.5f;
    vp.scale[1] = fbHeight * 0.5f;
    vp.scale[2] = 0.5f;
    vp.offset[0] = fbWidth * 0.5f;
    vp.offset[1] = fbHeight * 0.5f;
    vp.offset[2] = 0.5f;
    sceGnmDrawCmdSetViewport(&cmd, 0, &vp);

    GnmViewportTransformControl vtctrl;
    memset(&vtctrl, 0, sizeof(vtctrl));
    vtctrl.scalex = 1;
    vtctrl.offsetx = 1;
    vtctrl.scaley = 1;
    vtctrl.offsety = 1;
    vtctrl.scalez = 1;
    vtctrl.offsetz = 1;
    vtctrl.perspectivedividexy = 0;
    vtctrl.perspectivedividez = 0;
    vtctrl.invertw = 1;
    sceGnmDrawCmdSetViewportTransformControl(&cmd, &vtctrl);
    sceGnmDrawCmdSetScreenScissor(&cmd, 0, 0, static_cast<int32_t>(fbWidth),
                                  static_cast<int32_t>(fbHeight));

    sceGnmDrawCmdSetVsShader(&cmd, &impl->overlayVs->registers, 0);
    sceGnmDrawCmdSetPsShader(&cmd, &impl->overlayPs->registers);

    // Wire the VS's exported varyings (UV) to the PS's inputs.
    sceGnmDrawCmdSetPsInputUsage(
        &cmd, sceGnmVsShaderExportSemanticTable(impl->overlayVs),
        impl->overlayVs->numexportsemantics,
        sceGnmPsShaderInputSemanticTable(impl->overlayPs),
        impl->overlayPs->numinputsemantics);

    // Bind the overlay texture (startreg 0) and sampler (startreg 8).
    // OpenGNM's Set*UserData startuserdataslot is the register index directly.
    sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0, &impl->overlayTex);
    sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8, &impl->overlaySampler);

    sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
    sceGnmDrawCmdDrawIndexAuto(&cmd, 3);

    // EOP synchronization.
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
        return GpuVideoResult(GpuVideoResultCategory::SubmissionFailure,
                              static_cast<uint32_t>(res));
    }
    sceGnmSubmitDone();

    uint32_t spins = 0;
    while (*impl->eopLabel != 1) {
        if (++spins > kEopTimeoutSpins) {
            return GpuVideoResult(GpuVideoResultCategory::SynchronizationFailure);
        }
    }

    return GpuVideoResult();
}

// ---------------------------------------------------------------------------
// Stage 1 test: GPU-blend a static test overlay over the video
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::blendTestOverlay(void* framebuffer,
                                                  uint32_t fbWidth,
                                                  uint32_t fbHeight) {
    if (!m_impl || !m_impl->initialized) {
        return GpuVideoResult(GpuVideoResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    if (!impl->testOverlayMem || impl->testOverlayW == 0 ||
        impl->testOverlayH == 0) {
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }
    if (!framebuffer) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }

    // The static test overlay is in WC_GARLIC (already flushed at init). Blend
    // it over the framebuffer using the same GPU source-over path as
    // blendOverlay(). The overlay surface is tightly packed (pitch = w*4).
    return blendOverlay(impl->testOverlayMem, impl->testOverlayW,
                        impl->testOverlayH, impl->testOverlayW * 4u,
                        framebuffer, fbWidth, fbHeight);
}

// ---------------------------------------------------------------------------
// Stage 1 test (flicker-free): video + static test overlay in one submission
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::renderFrameWithTestOverlay(
    const GpuVideoFrame& frame, void* framebuffer,
    uint32_t fbWidth, uint32_t fbHeight) {
    if (!m_impl || !m_impl->initialized) {
        return GpuVideoResult(GpuVideoResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    if (!impl->testOverlayMem || impl->testOverlayW == 0 ||
        impl->testOverlayH == 0) {
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }

    // Render the video AND blend the static test overlay over it in a single
    // command-buffer submission (flicker-free path).
    return renderFrameWithOverlay(frame, impl->testOverlayMem,
                                  impl->testOverlayW, impl->testOverlayH,
                                  impl->testOverlayW * 4u,
                                  framebuffer, fbWidth, fbHeight);
}

// ---------------------------------------------------------------------------
// Overlay staging (copy CPU overlay into WC_GARLIC for GPU sampling)
// ---------------------------------------------------------------------------

GpuVideoResult GpuVideoRenderer::stageOverlay(const void* rgba, uint32_t w,
                                              uint32_t h, uint32_t pitch) {
    if (!m_impl || !m_impl->initialized) {
        return GpuVideoResult(GpuVideoResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    if (!rgba || w == 0 || h == 0 || pitch < w * 4u) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }
    if (!impl->overlayMem.baseAddress) {
        return GpuVideoResult(GpuVideoResultCategory::MemoryAllocationFailure);
    }
    // The staging buffer is 1920x1080 RGBA8. The overlay surface must fit.
    if (w > kDisplayWidth || h > kDisplayHeight) {
        return GpuVideoResult(GpuVideoResultCategory::InvalidFrame);
    }

    // Copy the CPU overlay surface into the WC_GARLIC staging buffer row by
    // row (respecting the source pitch). The staging buffer is tightly packed
    // (pitch = w*4 for the full 1920x1080 surface).
    uint8_t* dst = static_cast<uint8_t*>(impl->overlayMem.baseAddress);
    const uint8_t* src = static_cast<const uint8_t*>(rgba);
    const uint32_t dstPitch = kDisplayWidth * 4u;
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(dst + static_cast<size_t>(y) * dstPitch,
               src + static_cast<size_t>(y) * pitch,
               static_cast<size_t>(w) * 4u);
    }
    // Flush the WC_GARLIC writes so the GPU sees the staged overlay.
    sceGnmFlushGarlic();

    return GpuVideoResult();
}

void* GpuVideoRenderer::overlayStaging() const {
    if (!m_impl || !m_impl->initialized) {
        return nullptr;
    }
    return m_impl->overlayMem.baseAddress;
}

} // namespace ps4

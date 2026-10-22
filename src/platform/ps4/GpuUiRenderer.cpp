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

// === GpuUiRenderer.cpp ======================================================
//
// OpenOrbis PS4 — GPU fullscreen UI compositor.
//
// Implements the GPU fullscreen UI compositor using OpenGNM, following the
// exact state/setup patterns from GpuVideoRenderer and the working OpenGNM
// triangle smoke test:
//   - TILE render target over the WC_GARLIC VideoOut framebuffer (the
//     framebuffer is registered with VideoOut as TILING_MODE_TILE).
//   - Positive viewport Y scale (Y inversion done in the vertex shader).
//   - VS+PS binding, fullscreen triangle draw, EOP synchronization.
//
// The CPU UI compositor surface (1920x1080 RGBA8) is staged into a WC_GARLIC
// buffer via stageUi(), then drawn fullscreen via drawUi() sampling that
// texture. The overlay_vv.sb / overlay_p.sb shaders are reused (they sample an
// RGBA texture and draw it fullscreen). The overlay_vv.pssl UVs map the
// texture across the viewport, so scaling (1x at 1080p, 2x at 4K) is
// automatic based on the framebuffer dimensions.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "GpuUiRenderer.h"

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
/// Fullscreen vertex shader binary name (reuses the overlay VS).
static const char* kVsShaderName = "overlay_vv.sb";
/// RGBA-texture-sampling pixel shader binary name (reuses the overlay PS).
static const char* kPsShaderName = "overlay_p.sb";

/// UI surface dimensions (matches the CPU compositor surface).
static constexpr uint32_t kUiWidth  = 1920;
static constexpr uint32_t kUiHeight = 1080;

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
static constexpr uint32_t kCommandBytes = 16384;

/// EOP spin timeout (iterations) before declaring a hang.
static constexpr uint32_t kEopTimeoutSpins = 100000000u;

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void guiDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// A minimal linear allocator over a single direct-memory block. Used to carve
/// the Garlic block into shader code + command buffer, and the Onion block
/// into the EOP label. Mirrors the GpuVideoRenderer smoke-test approach.
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

/// Private implementation of GpuUiRenderer. Holds all OpenGNM resources.
struct GpuUiRenderer::Impl {
    // --- Lifecycle ----------------------------------------------------------
    bool initialized = false;

    // --- Direct memory ------------------------------------------------------
    DmemBlock garlicMem;   ///< Shader code + command buffer.
    DmemBlock onionMem;    ///< EOP sync label (CPU-pollable).
    DmemBlock uiMem;       ///< Dedicated UI staging block (Garlic).

    // --- Shaders ------------------------------------------------------------
    GnmVsShader* vs = nullptr;   ///< Vertex shader register struct (heap).
    GnmPsShader* ps = nullptr;   ///< Pixel shader register struct (heap).

    // --- UI texture state ---------------------------------------------------
    GnmTexture uiTex;            ///< UI texture descriptor (rebuilt per draw).
    GnmSampler uiSampler;        ///< UI sampler (point, clamp).

    // --- Command buffer + EOP ----------------------------------------------
    void* cmdMem = nullptr;              ///< Command buffer memory (Garlic).
    volatile uint64_t* eopLabel = nullptr;  ///< EOP sync label (Onion).

    // --- Render target (over the current framebuffer) ----------------------
    GnmRenderTarget rt;              ///< Descriptor; base updated per frame.
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

GpuUiRenderer::GpuUiRenderer()
    : m_impl(new Impl())
{
}

GpuUiRenderer::~GpuUiRenderer() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

GpuUiResult GpuUiRenderer::initialize(uint32_t fbWidth, uint32_t fbHeight) {
    if (m_impl->initialized) {
        return GpuUiResult();
    }
    Impl* impl = m_impl;

    if (fbWidth == 0 || fbHeight == 0) {
        return GpuUiResult(GpuUiResultCategory::InvalidSurface);
    }

    // --- 1. Allocate the Garlic block (shader code + cmd buffer) -----------
    // Layout:
    //   [0x0000 .. 0x1000)  VS shader code
    //   [0x1000 .. 0x2000)  PS shader code
    //   [0x2000 .. 0x2000+kCommandBytes)  command buffer
    const size_t garlicSize = 0x2000 + kCommandBytes;
    if (!impl->garlicMem.allocate(garlicSize, kDirectMemoryType,
                                  kDirectMemoryProt)) {
        guiDiagPrint("[gpu-ui] garlic alloc failed");
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }

    // --- 2. Allocate the Onion block for the EOP label ---------------------
    if (!impl->onionMem.allocate(0x1000, kLabelMemType, kLabelMemProt)) {
        guiDiagPrint("[gpu-ui] onion alloc failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }

    // --- 3. Load the shaders (reuse overlay_vv.sb / overlay_p.sb) ----------
    char vsPath[256];
    char psPath[256];
    snprintf(vsPath, sizeof(vsPath), "%s%s", kShaderDir, kVsShaderName);
    snprintf(psPath, sizeof(psPath), "%s%s", kShaderDir, kPsShaderName);

    if (!loadVsShader(&impl->vs, &impl->garlicMem, vsPath)) {
        guiDiagPrint("[gpu-ui] vertex shader load failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::ShaderLoadFailure);
    }
    if (!loadPsShader(&impl->ps, &impl->garlicMem, psPath)) {
        guiDiagPrint("[gpu-ui] pixel shader load failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::ShaderLoadFailure);
    }

    // --- 4. Carve the command buffer ---------------------------------------
    impl->cmdMem = impl->garlicMem.alloc(kCommandBytes, 4);
    if (!impl->cmdMem) {
        guiDiagPrint("[gpu-ui] cmd carve failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }

    // --- 5. EOP label -------------------------------------------------------
    impl->eopLabel = (volatile uint64_t*)impl->onionMem.alloc(
        sizeof(uint64_t), sizeof(uint64_t));
    if (!impl->eopLabel) {
        guiDiagPrint("[gpu-ui] eop label alloc failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }

    // --- 6. Allocate the dedicated UI staging block (Garlic) ---------------
    // The UI surface is a full 1920x1080 RGBA8 (~8.3 MB), far larger than the
    // packed garlicMem block, so it gets its own dedicated WC_GARLIC block.
    {
        const size_t uiBytes = static_cast<size_t>(kUiWidth) * kUiHeight * 4u;
        if (!impl->uiMem.allocate(uiBytes, kDirectMemoryType,
                                  kDirectMemoryProt)) {
            guiDiagPrint("[gpu-ui] ui staging block alloc failed");
            shutdown();
            return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
        }
    }

    // --- 7. Create the render target descriptor over the framebuffer -------
    // The base address is set per frame in drawUi() (the framebuffer is
    // double-buffered). The descriptor is created once with the display
    // dimensions and a TILE tile mode matching the VideoOut framebuffer
    // (TILING_MODE_TILE). Per the SCE VideoOut documentation,
    // SCE_VIDEO_OUT_TILING_MODE_TILE corresponds to kTileModeDisplay_2dThin,
    // so the render target uses GNM_TM_DISPLAY_2D_THIN. UNORM (not sRGB) so
    // the GPU writes the raw computed bytes; the display applies the sRGB
    // interpretation via the VideoOut A8B8G8R8_SRGB buffer attribute.
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
        guiDiagPrint("[gpu-ui] render target create failed");
        shutdown();
        return GpuUiResult(GpuUiResultCategory::RenderTargetFailure,
                           static_cast<uint32_t>(gerr));
    }

    // --- 8. Create the UI sampler (point/nearest, clamp) -------------------
    // The UI texture is sampled 1:1 (or 2x for 4K). Point filtering with clamp
    // gives a crisp nearest-neighbor upscale for 4K.
    memset(&impl->uiSampler, 0, sizeof(impl->uiSampler));
    impl->uiSampler.clampx = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->uiSampler.clampy = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->uiSampler.clampz = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
    impl->uiSampler.xymagfilter = GNM_FILTER_POINT;
    impl->uiSampler.xyminfilter = GNM_FILTER_POINT;
    impl->uiSampler.mipfilter = GNM_MIPFILTER_NONE;

    // --- 9. Flush the Garlic writes so the GPU sees the shader code --------
    sceGnmFlushGarlic();

    impl->initialized = true;
    guiDiagPrint("[gpu-ui] initialized");
    return GpuUiResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void GpuUiRenderer::shutdown() {
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
    impl->cmdMem = nullptr;
    impl->eopLabel = nullptr;

    impl->garlicMem.release();
    impl->onionMem.release();
    impl->uiMem.release();

    impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool GpuUiRenderer::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// UI staging (copy CPU UI surface into WC_GARLIC for GPU sampling)
// ---------------------------------------------------------------------------

GpuUiResult GpuUiRenderer::stageUi(const uint8_t* rgba, uint32_t w,
                                   uint32_t h, uint32_t pitch) {
    if (!m_impl || !m_impl->initialized) {
        return GpuUiResult(GpuUiResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    if (!rgba || w == 0 || h == 0 || pitch < w * 4u) {
        return GpuUiResult(GpuUiResultCategory::InvalidSurface);
    }
    if (!impl->uiMem.baseAddress) {
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }
    // The staging buffer is 1920x1080 RGBA8. The UI surface must fit.
    if (w > kUiWidth || h > kUiHeight) {
        return GpuUiResult(GpuUiResultCategory::InvalidSurface);
    }

    // Copy the CPU UI surface into the WC_GARLIC staging buffer row by row
    // (respecting the source pitch). The staging buffer is tightly packed
    // (pitch = kUiWidth*4 for the full 1920x1080 surface).
    uint8_t* dst = static_cast<uint8_t*>(impl->uiMem.baseAddress);
    const uint8_t* src = static_cast<const uint8_t*>(rgba);
    const uint32_t dstPitch = kUiWidth * 4u;
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(dst + static_cast<size_t>(y) * dstPitch,
               src + static_cast<size_t>(y) * pitch,
               static_cast<size_t>(w) * 4u);
    }
    // Flush the WC_GARLIC writes so the GPU sees the staged UI.
    sceGnmFlushGarlic();

    return GpuUiResult();
}

// ---------------------------------------------------------------------------
// UI draw (fullscreen draw of the staged UI texture into the framebuffer)
// ---------------------------------------------------------------------------

GpuUiResult GpuUiRenderer::drawUi(void* framebuffer, uint32_t fbWidth,
                                  uint32_t fbHeight) {
    if (!m_impl || !m_impl->initialized) {
        return GpuUiResult(GpuUiResultCategory::NotInitialized);
    }
    Impl* impl = m_impl;

    if (!framebuffer) {
        return GpuUiResult(GpuUiResultCategory::InvalidSurface);
    }
    if (!impl->uiMem.baseAddress) {
        return GpuUiResult(GpuUiResultCategory::MemoryAllocationFailure);
    }

    // --- Set the render target base to the current framebuffer slot --------
    sceGnmRtSetBaseAddr(&impl->rt, framebuffer);

    // --- Build the UI texture over the staged WC_GARLIC surface ------------
    // The UI surface is RGBA8 (byte order R,G,B,A) matching the A8B8G8R8_SRGB
    // framebuffer. Bind as R8G8B8A8_UNORM linear. The surface is in WC_GARLIC
    // (already flushed by stageUi()).
    memset(&impl->uiTex, 0, sizeof(impl->uiTex));
    GnmTextureCreateInfo uiCi;
    memset(&uiCi, 0, sizeof(uiCi));
    uiCi.texturetype = GNM_TEXTURE_2D;
    uiCi.width = kUiWidth;
    uiCi.height = kUiHeight;
    uiCi.depth = 1;
    uiCi.pitch = kUiWidth;   // pitch in texels (RGBA8 = 4 bytes/texel)
    uiCi.nummiplevels = 1;
    uiCi.numslices = 1;
    uiCi.format = GNM_FMT_R8G8B8A8_UNORM;
    uiCi.tilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
    uiCi.mingpumode = GNM_GPU_BASE;
    uiCi.numfragments = 1;
    GnmError gerr = sceGnmCreateTexture(&impl->uiTex, &uiCi);
    if (gerr != GNM_ERROR_OK) {
        return GpuUiResult(GpuUiResultCategory::RenderTargetFailure,
                           static_cast<uint32_t>(gerr));
    }
    sceGnmTexSetBaseAddress(&impl->uiTex, impl->uiMem.baseAddress);
    sceGnmTexSetMemoryType(&impl->uiTex, GNM_MEMORY_READONLY, false);

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

    // Source-over alpha blend (straight alpha). The UI surface is fully opaque
    // (it has a background fill), so this effectively produces an opaque copy
    // over the cleared/black framebuffer. Reuses the overlay blend semantics.
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

    sceGnmDrawCmdSetVsShader(&cmd, &impl->vs->registers, 0);
    sceGnmDrawCmdSetPsShader(&cmd, &impl->ps->registers);

    // Wire the VS's exported varyings (UV) to the PS's inputs.
    sceGnmDrawCmdSetPsInputUsage(
        &cmd, sceGnmVsShaderExportSemanticTable(impl->vs),
        impl->vs->numexportsemantics,
        sceGnmPsShaderInputSemanticTable(impl->ps),
        impl->ps->numinputsemantics);

    // Bind the UI texture (startreg 0) and sampler (startreg 8).
    sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0, &impl->uiTex);
    sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8, &impl->uiSampler);

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
        return GpuUiResult(GpuUiResultCategory::SubmissionFailure,
                           static_cast<uint32_t>(res));
    }
    sceGnmSubmitDone();

    uint32_t spins = 0;
    while (*impl->eopLabel != 1) {
        if (++spins > kEopTimeoutSpins) {
            return GpuUiResult(GpuUiResultCategory::SynchronizationFailure);
        }
    }

    return GpuUiResult();
}

} // namespace ps4

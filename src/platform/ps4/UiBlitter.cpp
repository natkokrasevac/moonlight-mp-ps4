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

// === UiBlitter.cpp ===========================================================
//
// OpenOrbis PS4 — CPU UI blitter.
//
// Adapted from the SCE reference (moonlight-ps4-SCE-RC) for the
// OpenOrbis framebuffer architecture. The SCE reference uploads the CPU
// compositor surface into dedicated GPU textures and renders them via a GNMX
// fullscreen quad. The OpenOrbis implementation instead copies the compositor
// surface directly into the current GfxManager VideoOut framebuffer slot using
// CPU memcpy.
//
// Conceptual data flow:
//   canonical CPU surface
//        ↓ copy dirty regions
//   current VideoOut framebuffer
//
// There is no GPU texture, no shader, no fullscreen triangle, no sampler, no
// blend state, and no GNMX context.
//
// Cache/coherency ownership: UiBlitter only copies pixels. GfxManager::present()
// is responsible for the final framebuffer visibility (sceGnmFlushGarlic()).
// UiBlitter does NOT call sceGnmFlushGarlic().
//
// OpenOrbis SDK includes are confined to this translation unit.
// =============================================================================

#include "UiBlitter.h"

// --- Platform graphics subsystem ---------------------------------------------
#include "GfxManager.h"

#include <cstdio>               // printf, fflush
#include <cstring>              // memcpy

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Number of graphics frame slots (matches GfxManager double buffering).
static constexpr uint32_t kFrameSlotCount = 2;

/// UI surface dimensions (matches the CPU compositor surface).
static constexpr uint32_t kUiWidth  = 1920;
static constexpr uint32_t kUiHeight = 1080;

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void blitDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of UiBlitter. Holds only what the OpenOrbis CPU
/// blitter needs: a GfxManager reference, per-slot pending dirty sets, and
/// seeding/statistics state. No GPU resources are retained.
struct UiBlitter::Impl {
    // --- GfxManager reference (non-owning) ----------------------------------
    GfxManager* gfx = nullptr;

    // --- Per-slot pending dirty sets ----------------------------------------
    // Each framebuffer slot tracks its own pending dirty regions. On a CPU
    // change, the same dirty rects are added to BOTH slots' pending sets.
    // When a slot becomes safe (between beginFrame()/endFrame()), its pending
    // set is uploaded from the authoritative CPU surface and cleared. This
    // keeps both slots coherent without a generation/history journal.
    DirtyRegionSet pendingDirty[kFrameSlotCount];
    bool texturesSeeded = false;

    // --- Upload statistics --------------------------------------------------
    uint64_t fullSurfaceFallbacks = 0;

    // --- State --------------------------------------------------------------
    bool initialized = false;
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

UiBlitter::UiBlitter()
    : m_impl(new Impl())
{
}

UiBlitter::~UiBlitter() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

GfxResult UiBlitter::initialize(GfxManager* gfx) {
    if (m_impl->initialized) {
        return GfxResult(GfxResultCategory::AlreadyInitialized);
    }
    if (!gfx) {
        return GfxResult(GfxResultCategory::InvalidLifecycleState);
    }

    m_impl->gfx = gfx;
    m_impl->initialized = true;
    blitDiagPrint("[ui-blit] initialized (CPU framebuffer blitter)");
    return GfxResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void UiBlitter::shutdown() {
    if (!m_impl) {
        return;
    }
    for (uint32_t i = 0; i < kFrameSlotCount; ++i) {
        m_impl->pendingDirty[i].clear();
    }
    m_impl->texturesSeeded = false;
    m_impl->fullSurfaceFallbacks = 0;
    m_impl->gfx = nullptr;
    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool UiBlitter::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// CPU -> framebuffer copy helpers
// ---------------------------------------------------------------------------

/// Copy the full CPU RGBA8 surface into the given framebuffer slot, row by
/// row, respecting both the CPU source pitch and the framebuffer stride.
/// Returns the copy time in microseconds (0 if not measured).
static uint64_t copySurfaceToFramebuffer(
    const uint8_t* rgba,
    uint32_t width,
    uint32_t height,
    uint32_t srcPitch,
    void* dstData,
    size_t dstPitchBytes) {

    const uint8_t* src = rgba;
    uint8_t* dst = static_cast<uint8_t*>(dstData);
    const uint32_t rowBytes = width * 4;

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * dstPitchBytes,
               src + static_cast<size_t>(y) * srcPitch,
               rowBytes);
    }

    return 0;   // Timing not measured in this build.
}

/// Copy a single clipped rectangle region of the CPU RGBA8 surface into the
/// given framebuffer slot, row by row, respecting both the CPU source pitch
/// and the framebuffer stride. The region is clipped to the surface bounds.
/// Returns the copy time in microseconds (0 if not measured).
static uint64_t copyRectToFramebuffer(
    const uint8_t* rgba,
    uint32_t width,
    uint32_t height,
    uint32_t srcPitch,
    void* dstData,
    size_t dstPitchBytes,
    const Rect& rect) {

    // Clip the region to the surface bounds.
    int x0 = rect.x;
    int y0 = rect.y;
    int x1 = rect.x + rect.w;
    int y1 = rect.y + rect.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int>(width)) x1 = static_cast<int>(width);
    if (y1 > static_cast<int>(height)) y1 = static_cast<int>(height);
    if (x1 <= x0 || y1 <= y0) {
        return 0;   // Empty after clipping.
    }

    const uint8_t* src = rgba;
    uint8_t* dst = static_cast<uint8_t*>(dstData);
    const uint32_t rowBytes = static_cast<uint32_t>(x1 - x0) * 4;

    for (int y = y0; y < y1; ++y) {
        memcpy(dst + static_cast<size_t>(y) * dstPitchBytes +
                   static_cast<size_t>(x0) * 4,
               src + static_cast<size_t>(y) * srcPitch +
                   static_cast<size_t>(x0) * 4,
               rowBytes);
    }

    return 0;   // Timing not measured in this build.
}

// ---------------------------------------------------------------------------
// Full upload
// ---------------------------------------------------------------------------

uint64_t UiBlitter::upload(const uint8_t* rgba, uint32_t width, uint32_t height,
                           uint32_t srcPitch) {
    if (!m_impl->initialized || !m_impl->gfx || !rgba) {
        return 0;
    }
    if (width != kUiWidth || height != kUiHeight) {
        blitDiagPrint("[ui-blit] upload: surface size mismatch (expected 1920x1080)");
        return 0;
    }
    if (srcPitch < width * 4) {
        blitDiagPrint("[ui-blit] upload: source pitch too small");
        return 0;
    }

    void* fb = m_impl->gfx->currentFramebuffer();
    if (!fb) {
        return 0;
    }
    const size_t fbStride = m_impl->gfx->framebufferStrideBytes();

    return copySurfaceToFramebuffer(
        rgba, width, height, srcPitch, fb, fbStride);
}

uint64_t UiBlitter::uploadBoth(const uint8_t* rgba, uint32_t width, uint32_t height,
                               uint32_t srcPitch) {
    if (!m_impl->initialized || !m_impl->gfx || !rgba) {
        return 0;
    }
    if (width != kUiWidth || height != kUiHeight) {
        blitDiagPrint("[ui-blit] uploadBoth: surface size mismatch (expected 1920x1080)");
        return 0;
    }
    if (srcPitch < width * 4) {
        blitDiagPrint("[ui-blit] uploadBoth: source pitch too small");
        return 0;
    }

    // uploadBoth() is used to pre-populate both framebuffer slots before first
    // use. It must be called between beginFrame()/endFrame() so the current
    // slot is display-safe. We copy the current slot directly and mark the
    // other slot as pending-full so it is fully uploaded when it becomes
    // active. This guarantees both slots converge to the canonical CPU surface.
    uint64_t totalUs = 0;
    const size_t fbStride = m_impl->gfx->framebufferStrideBytes();

    const uint32_t currentSlot = m_impl->gfx->currentFrameIndex();
    void* fb = m_impl->gfx->currentFramebuffer();
    if (fb) {
        totalUs += copySurfaceToFramebuffer(
            rgba, width, height, srcPitch, fb, fbStride);
    }

    // The current slot is now up to date; clear its pending set.
    if (currentSlot < kFrameSlotCount) {
        m_impl->pendingDirty[currentSlot].clear();
    }
    // The other slot still needs a full upload when it becomes active.
    for (uint32_t slot = 0; slot < kFrameSlotCount; ++slot) {
        if (slot != currentSlot) {
            m_impl->pendingDirty[slot].addFullSurface(
                static_cast<int>(width), static_cast<int>(height));
        }
    }

    return totalUs;
}

// ---------------------------------------------------------------------------
// Partial (dirty-region) upload
// ---------------------------------------------------------------------------

uint64_t UiBlitter::uploadRect(const uint8_t* rgba, uint32_t width,
                               uint32_t height, uint32_t srcPitch,
                               const Rect& rect) {
    if (!m_impl->initialized || !m_impl->gfx || !rgba) {
        return 0;
    }
    if (width != kUiWidth || height != kUiHeight) {
        blitDiagPrint("[ui-blit] uploadRect: surface size mismatch (expected 1920x1080)");
        return 0;
    }
    if (srcPitch < width * 4) {
        blitDiagPrint("[ui-blit] uploadRect: source pitch too small");
        return 0;
    }

    void* fb = m_impl->gfx->currentFramebuffer();
    if (!fb) {
        return 0;
    }
    const size_t fbStride = m_impl->gfx->framebufferStrideBytes();

    return copyRectToFramebuffer(
        rgba, width, height, srcPitch, fb, fbStride, rect);
}

// ---------------------------------------------------------------------------
// Alpha-blend composition over the framebuffer
// ---------------------------------------------------------------------------

/// Blend a single straight-alpha source pixel over a destination pixel.
/// The surface and framebuffer are stored in RGBA8 byte order (byte 0 = R,
/// byte 1 = G, byte 2 = B, byte 3 = A). This mirrors the compositor's
/// blendPixel() so the in-stream overlay composition uses the same alpha
/// semantics as the smoke-tested renderers.
static void blendPixelOver(uint8_t* dst, const uint8_t* src) {
    const uint8_t sa = src[3];
    if (sa == 0) {
        return;   // Fully transparent — preserve the underlying video.
    }
    if (sa == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }
    const uint32_t invA = 255u - sa;
    dst[0] = static_cast<uint8_t>((src[0] * sa + dst[0] * invA) / 255u);
    dst[1] = static_cast<uint8_t>((src[1] * sa + dst[1] * invA) / 255u);
    dst[2] = static_cast<uint8_t>((src[2] * sa + dst[2] * invA) / 255u);
    dst[3] = 255;
}

void UiBlitter::blendOverFramebuffer(const uint8_t* rgba, uint32_t width,
                                     uint32_t height, uint32_t srcPitch,
                                     const Rect& rect) {
    if (!m_impl->initialized || !m_impl->gfx || !rgba) {
        return;
    }
    if (width != kUiWidth || height != kUiHeight) {
        blitDiagPrint("[ui-blit] blendOverFramebuffer: surface size mismatch (expected 1920x1080)");
        return;
    }
    if (srcPitch < width * 4) {
        blitDiagPrint("[ui-blit] blendOverFramebuffer: source pitch too small");
        return;
    }

    void* fb = m_impl->gfx->currentFramebuffer();
    if (!fb) {
        return;
    }
    const size_t fbStride = m_impl->gfx->framebufferStrideBytes();

    // Clip the region to the surface bounds.
    int x0 = rect.x;
    int y0 = rect.y;
    int x1 = rect.x + rect.w;
    int y1 = rect.y + rect.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int>(width)) x1 = static_cast<int>(width);
    if (y1 > static_cast<int>(height)) y1 = static_cast<int>(height);
    if (x1 <= x0 || y1 <= y0) {
        return;   // Empty after clipping.
    }

    const uint8_t* src = rgba;
    uint8_t* dst = static_cast<uint8_t*>(fb);
    for (int y = y0; y < y1; ++y) {
        const uint8_t* srcRow = src + static_cast<size_t>(y) * srcPitch +
                                static_cast<size_t>(x0) * 4;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * fbStride +
                          static_cast<size_t>(x0) * 4;
        for (int x = x0; x < x1; ++x) {
            blendPixelOver(dstRow, srcRow);
            srcRow += 4;
            dstRow += 4;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-slot pending-dirty coherency
// ---------------------------------------------------------------------------

void UiBlitter::addDirtyRect(const Rect& rect, uint32_t surfaceW,
                             uint32_t surfaceH) {
    if (!m_impl->initialized) {
        return;
    }
    for (uint32_t slot = 0; slot < kFrameSlotCount; ++slot) {
        m_impl->pendingDirty[slot].add(rect,
                                       static_cast<int>(surfaceW),
                                       static_cast<int>(surfaceH));
    }
}

void UiBlitter::addFullSurfaceDirty(uint32_t surfaceW, uint32_t surfaceH) {
    if (!m_impl->initialized) {
        return;
    }
    for (uint32_t slot = 0; slot < kFrameSlotCount; ++slot) {
        m_impl->pendingDirty[slot].addFullSurface(
            static_cast<int>(surfaceW), static_cast<int>(surfaceH));
    }
}

bool UiBlitter::currentSlotHasPendingDirty() const {
    if (!m_impl->initialized || !m_impl->gfx) {
        return false;
    }
    const uint32_t slot = m_impl->gfx->currentFrameIndex();
    if (slot >= kFrameSlotCount) {
        return false;
    }
    return !m_impl->pendingDirty[slot].empty();
}

DirtyUploadStats UiBlitter::uploadPendingDirty(const uint8_t* rgba,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t srcPitch) {
    DirtyUploadStats stats;
    if (!m_impl->initialized || !m_impl->gfx || !rgba) {
        return stats;
    }
    if (width != kUiWidth || height != kUiHeight) {
        blitDiagPrint("[ui-blit] uploadPendingDirty: surface size mismatch");
        return stats;
    }
    if (srcPitch < width * 4) {
        blitDiagPrint("[ui-blit] uploadPendingDirty: source pitch too small");
        return stats;
    }

    const uint32_t slot = m_impl->gfx->currentFrameIndex();
    if (slot >= kFrameSlotCount) {
        return stats;
    }

    DirtyRegionSet& pending = m_impl->pendingDirty[slot];
    if (pending.empty()) {
        return stats;   // Nothing to upload.
    }

    void* fb = m_impl->gfx->currentFramebuffer();
    if (!fb) {
        return stats;
    }
    const size_t fbStride = m_impl->gfx->framebufferStrideBytes();

    const int count = pending.count();
    stats.rectCount = count;
    stats.pixelArea = pending.totalPixelArea();

    uint64_t totalBytes = 0;
    for (int i = 0; i < count; ++i) {
        const Rect& r = pending.rect(i);
        totalBytes += static_cast<uint64_t>(r.w) * r.h * 4;
        copyRectToFramebuffer(
            rgba, width, height, srcPitch, fb, fbStride, r);
    }
    stats.bytesCopied = totalBytes;
    stats.fullSurfaceFallbacks = m_impl->fullSurfaceFallbacks;

    // Clear the pending set for this slot now that it is up to date.
    pending.clear();

    return stats;
}

void UiBlitter::markTexturesSeeded() {
    if (!m_impl->initialized) {
        return;
    }
    m_impl->texturesSeeded = true;
    for (uint32_t slot = 0; slot < kFrameSlotCount; ++slot) {
        m_impl->pendingDirty[slot].clear();
    }
}

bool UiBlitter::texturesSeeded() const {
    return m_impl && m_impl->initialized && m_impl->texturesSeeded;
}

} // namespace ps4

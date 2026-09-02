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

// === UiBlitter.h =============================================================
//
// OpenOrbis PS4 — CPU UI blitter.
//
// Adapted from the SCE reference (moonlight-ps4-SCE-RC) for the
// OpenOrbis framebuffer architecture. The SCE reference uploads the CPU
// compositor surface into dedicated GPU textures and renders them via a GNMX
// fullscreen quad. The OpenOrbis implementation instead copies the compositor
// surface directly into the current GfxManager VideoOut framebuffer slot using
// CPU memcpy. There is no GPU texture, no shader, no fullscreen triangle, no
// sampler, no blend state, and no GNMX context.
//
// Responsibilities:
//   - Transfer changed pixels from the canonical CPU compositor surface into
//     each VideoOut framebuffer slot.
//   - Per-slot pending-dirty coherency across both framebuffer slots.
//   - Full-surface upload and partial (dirty-region) upload.
//   - Dirty-overflow fallback to full-surface upload.
//
// Explicitly out of scope:
//   - GPU rendering, shaders, textures, samplers, GNMX.
//   - Generic renderer features.
//
// OpenOrbis SDK types are confined to the implementation file.
// =============================================================================

#ifndef PS4_UI_BLITTER_H
#define PS4_UI_BLITTER_H

#include <cstdint>

#include "GfxManager.h"   // For GfxResult.
#include "DirtyRegionSet.h"   // For ps4::DirtyRegionSet (per-slot pending dirty).

namespace ps4 {

class GfxManager;

/// Diagnostics for a partial (dirty-region) upload operation.
struct DirtyUploadStats {
    int rectCount = 0;          ///< Number of dirty rectangles uploaded.
    uint64_t pixelArea = 0;     ///< Total dirty pixel area (px²).
    uint64_t bytesCopied = 0;   ///< Total bytes copied to the framebuffer.
    uint64_t uploadUs = 0;      ///< Copy time (µs).
    uint64_t fullSurfaceFallbacks = 0;  ///< Full-surface fallback count.
};

/// CPU UI blitter.
///
/// Main-thread owned. Transfers the CPU-composited RGBA8 UI surface into the
/// current GfxManager VideoOut framebuffer slot.
class UiBlitter {
public:
    /// Constructor performs no fallible operations.
    UiBlitter();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~UiBlitter();

    // Non-copyable, non-movable.
    UiBlitter(const UiBlitter&) = delete;
    UiBlitter& operator=(const UiBlitter&) = delete;
    UiBlitter(UiBlitter&&) = delete;
    UiBlitter& operator=(UiBlitter&&) = delete;

    /// Initialize the blitter. Requires GfxManager to be initialized. Must be
    /// called before upload()/uploadPendingDirty(). On failure the object
    /// remains safe to shut down.
    GfxResult initialize(GfxManager* gfx);

    /// Release all blitter-owned resources. Must be called before
    /// GfxManager::shutdown(). Safe after partial initialization or repeated
    /// calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Upload the full CPU RGBA8 surface into the current frame-slot
    /// framebuffer. `srcPitch` is the CPU source row stride in bytes
    /// (>= width*4). The copy is row-by-row, respecting both the CPU source
    /// pitch and the framebuffer stride. Must be called between beginFrame()
    /// and endFrame() so the current frame slot is confirmed display-safe for
    /// reuse.
    uint64_t upload(const uint8_t* rgba, uint32_t width, uint32_t height,
                    uint32_t srcPitch);

    /// Upload the same CPU RGBA8 surface into BOTH framebuffer slots. This
    /// ensures both slots hold valid content before first use, so no
    /// uninitialized framebuffer is displayed on alternate frames. Must be
    /// called between beginFrame() and endFrame().
    uint64_t uploadBoth(const uint8_t* rgba, uint32_t width, uint32_t height,
                        uint32_t srcPitch);

    /// Upload a single dirty rectangle region of the CPU RGBA8 surface into
    /// the current frame-slot framebuffer. `srcPitch` is the CPU source row
    /// stride in bytes (>= width*4). The copy is row-by-row, respecting both
    /// the CPU source pitch and the framebuffer stride, and copies only the
    /// clipped region. Must be called between beginFrame() and endFrame().
    uint64_t uploadRect(const uint8_t* rgba, uint32_t width, uint32_t height,
                        uint32_t srcPitch, const Rect& rect);

    /// Alpha-blend a region of the CPU RGBA8 surface over the current
    /// framebuffer. Used for in-stream overlay composition: the compositor
    /// surface holds the overlay pixels (transparent elsewhere), and this
    /// blends them over the decoded video already in the framebuffer using
    /// straight-alpha source-over blending. The underlying video is preserved
    /// wherever the compositor pixel is transparent. `srcPitch` is the CPU
    /// source row stride in bytes (>= width*4). Must be called between
    /// beginFrame() and endFrame().
    void blendOverFramebuffer(const uint8_t* rgba, uint32_t width,
                              uint32_t height, uint32_t srcPitch,
                              const Rect& rect);

    // --- Dirty-region coherency ---------------------------------------------

    /// Add a dirty rectangle to BOTH framebuffer slots' pending dirty sets.
    /// This is the per-slot pending-dirty coherency model: every logical CPU
    /// change records the affected regions for each slot independently, so
    /// each slot can catch up from the already-current CPU surface when its
    /// frame slot becomes safe. `surfaceW`/`surfaceH` are the CPU surface
    /// dimensions used for normalization/clipping.
    void addDirtyRect(const Rect& rect, uint32_t surfaceW, uint32_t surfaceH);

    /// Add a full-surface dirty rectangle to BOTH slots' pending sets.
    /// Used for fallback recovery and explicit full redraws.
    void addFullSurfaceDirty(uint32_t surfaceW, uint32_t surfaceH);

    /// Returns true if the current frame-slot framebuffer has pending dirty
    /// regions that must be uploaded before it is presented.
    bool currentSlotHasPendingDirty() const;

    /// Upload the current frame-slot framebuffer's pending dirty regions from
    /// the CPU surface, then clear that slot's pending set. This is the
    /// per-frame catch-up step: it brings the current slot's framebuffer up to
    /// date with the authoritative CPU surface. Returns per-rectangle
    /// diagnostics. Must be called between beginFrame() and endFrame().
    DirtyUploadStats uploadPendingDirty(const uint8_t* rgba, uint32_t width,
                                        uint32_t height, uint32_t srcPitch);

    /// Mark both framebuffer slots as seeded (initial full upload done). After
    /// seeding, partial updates are used. Called after uploadBoth() at startup.
    void markTexturesSeeded();

    /// Returns true if both framebuffer slots have been seeded with a full
    /// upload.
    bool texturesSeeded() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_UI_BLITTER_H

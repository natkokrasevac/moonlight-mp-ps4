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

// === UiCompositor.h ==========================================================
//
// OpenOrbis PS4 — CPU UI compositor.
//
// Ported from the SCE reference (moonlight-ps4-SCE-RC) with one
// OpenOrbis-specific adaptation:
//
//   CpuImage stores pixels in a raw pointer + size instead of
//   std::vector<uint8_t>. The OpenOrbis toolchain's libc++ <vector> header
//   pulls in <atomic>/<__threading_support>, which references nanosleep
//   without including <time.h> and fails to compile. Image drawing is out
//   of scope for this milestone, so the minimal isolation is to drop the
//   standard-library container dependency rather than redesign the API.
//
// The CPU surface is stored in RGBA8 byte order (byte 0 = R, byte 1 = G,
// byte 2 = B, byte 3 = A), matching the hardware-verified framebuffer byte
// order for ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB. This allows the
// UiBlitter to copy the compositor surface into the framebuffer with a plain
// memcpy (no per-pixel color conversion).
//
// It composes a complete 1920×1080 RGBA8 UI surface entirely on the CPU using
// primitives. It is deliberately NOT a generic graphics framework.
//
// Responsibilities:
//   - Own one CPU RGBA8 surface (1920×1080, tightly packed pitch).
//   - clear / fillRect / strokeRect / drawImage / drawImageSubRect / drawText.
//   - pushClip / popClip with a small fixed-capacity clip stack.
//   - Straight-alpha source-over blending.
//   - Nearest-neighbor image scaling.
//   - Aspect-fit helper for card artwork.
//
// Explicitly out of scope:
//   - GPU upload / presentation, GNMX draw commands, Garlic textures.
//   - Glyph caching, SIMD, multi-threading, dirty-region logic.
//   - Premultiplied alpha, rounded corners.
//
// OpenOrbis SDK types are confined to the implementation file.
// =============================================================================

#ifndef PS4_UI_COMPOSITOR_H
#define PS4_UI_COMPOSITOR_H

#include <cstdint>
#include <cstddef>   // size_t

#include "FontRasterizer.h"   // For FontRasterizer::FontFace in drawText().

namespace ps4 {

/// A CPU-owned RGBA8 image used by the compositor. This is the smallest CPU
/// image representation needed. It does not expose any PS4 GNM texture types.
///
/// OpenOrbis adaptation: pixels are held as a raw pointer + byte size rather
/// than std::vector to avoid the toolchain <vector> include issue.
struct CpuImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;         ///< Row stride in bytes (tightly packed: width * 4).
    uint8_t* rgba = nullptr;    ///< Pixel data (RGBA8).
    size_t   rgbaSize = 0;      ///< Pixel data size in bytes.
};

/// A 2D rectangle in integer pixel coordinates (half-open: [x, x+w) × [y, y+h)).
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// CPU UI compositor.
///
/// Main-thread owned. Composes a single CPU RGBA8 surface for the dedicated
/// UI renderers.
class UiCompositor {
public:
    /// Constructor performs no fallible operations.
    UiCompositor();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~UiCompositor();

    // Non-copyable, non-movable.
    UiCompositor(const UiCompositor&) = delete;
    UiCompositor& operator=(const UiCompositor&) = delete;
    UiCompositor(UiCompositor&&) = delete;
    UiCompositor& operator=(UiCompositor&&) = delete;

    /// Allocate the CPU RGBA8 surface and retain the FontRasterizer reference
    /// for text drawing. Returns true on success. On failure the object remains
    /// safe to shut down or destroy.
    bool initialize(uint32_t width, uint32_t height, FontRasterizer* fonts);

    /// Release the CPU surface. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Surface access -----------------------------------------------------

    /// Pointer to the CPU RGBA8 surface (tightly packed). Valid until
    /// shutdown(). Returns nullptr if not initialized.
    const uint8_t* pixels() const;

    /// Pointer to the CPU RGBA8 surface (writable). Valid until shutdown().
    uint8_t* pixelsWritable();

    uint32_t width() const;
    uint32_t height() const;
    uint32_t pitch() const;

    // --- Primitives ---------------------------------------------------------

    /// Fill the entire surface with the given straight-alpha color.
    void clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    /// Fill an axis-aligned rectangle with the given straight-alpha color.
    /// Clipped to the framebuffer and the current clip rect. `x`/`y`/`w`/`h`
    /// are integer pixel bounds.
    void fillRect(int x, int y, int w, int h,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    /// Draw an axis-aligned rectangle outline with the given integer thickness
    /// (1 or 2 px). Clipped to the framebuffer and the current clip rect.
    void strokeRect(int x, int y, int w, int h, int thickness,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // --- Images -------------------------------------------------------------

    /// Draw a CPU image scaled to the destination rectangle with alpha
    /// blending. Uses nearest-neighbor scaling. Clipped to the framebuffer and
    /// the current clip rect.
    ///
    /// Templated on the image type so it accepts both ps4::CpuImage (retained
    /// renderer resources) and ps4::CpuPngImage (runtime app artwork), which
    /// are structurally identical (width/height/pitch/rgba). This avoids a full
    /// image copy when drawing runtime artwork.
    template <typename ImageT>
    void drawImage(const ImageT& img, int dstX, int dstY, int dstW, int dstH) {
        if (img.width == 0 || img.height == 0 || img.rgba == nullptr) {
            return;
        }
        drawImageSubRect(img, 0, 0, static_cast<int>(img.width),
                         static_cast<int>(img.height),
                         dstX, dstY, dstW, dstH);
    }

    template <typename ImageT>
    void drawImageSubRect(const ImageT& img,
                          int srcX, int srcY, int srcW, int srcH,
                          int dstX, int dstY, int dstW, int dstH) {
        if (!m_pixels || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
            return;
        }
        if (srcX < 0 || srcY < 0 ||
            srcX + srcW > static_cast<int>(img.width) ||
            srcY + srcH > static_cast<int>(img.height)) {
            return;
        }

        Rect clipped;
        if (!clipRect(Rect{dstX, dstY, dstW, dstH}, clipped)) {
            return;
        }

        // Nearest-neighbor scaling. For each destination pixel, map back to the
        // source pixel using the center of the destination pixel.
        const float sxScale = static_cast<float>(srcW) / static_cast<float>(dstW);
        const float syScale = static_cast<float>(srcH) / static_cast<float>(dstH);

        for (int yy = 0; yy < clipped.h; ++yy) {
            const int dstYy = clipped.y + yy;
            const float srcYf = static_cast<float>(srcY) +
                                (static_cast<float>(dstYy - dstY) + 0.5f) * syScale;
            int srcYy = static_cast<int>(srcYf);
            if (srcYy < srcY) srcYy = srcY;
            if (srcYy >= srcY + srcH) srcYy = srcY + srcH - 1;

            const uint8_t* srcRow = img.rgba +
                                    static_cast<size_t>(srcYy) * img.pitch;
            uint8_t* dstRow = m_pixels + static_cast<size_t>(dstYy) * m_pitch +
                              static_cast<size_t>(clipped.x) * 4;

            for (int xx = 0; xx < clipped.w; ++xx) {
                const int dstXx = clipped.x + xx;
                const float srcXf = static_cast<float>(srcX) +
                                    (static_cast<float>(dstXx - dstX) + 0.5f) *
                                        sxScale;
                int srcXx = static_cast<int>(srcXf);
                if (srcXx < srcX) srcXx = srcX;
                if (srcXx >= srcX + srcW) srcXx = srcX + srcW - 1;

                const uint8_t* sp = srcRow + static_cast<size_t>(srcXx) * 4;
                uint8_t* dp = dstRow + static_cast<size_t>(xx) * 4;
                blendPixel(dp, sp[0], sp[1], sp[2], sp[3]);
            }
        }
    }

    // --- Text ---------------------------------------------------------------

    /// Draw an ASCII string using the FontRasterizer. `x` is the left pen
    /// position and `baselineY` is the baseline (surface coordinates). Returns
    /// the total advance width drawn.
    float drawText(FontRasterizer::FontFace face, const char* text,
                   float x, float baselineY,
                   uint8_t r, uint8_t g, uint8_t b);

    // --- Clipping -----------------------------------------------------------

    /// Push a clip rectangle, intersecting it with the current clip. The clip
    /// stack is fixed-capacity (no heap allocation). Returns false if the stack
    /// is full or the rectangle is empty.
    bool pushClip(const Rect& rect);

    /// Pop the most recent clip rectangle. Returns false if the stack is empty.
    bool popClip();

    // --- Helpers ------------------------------------------------------------

    /// Compute an aspect-fit destination rectangle for `srcW`×`srcH` inside the
    /// destination region `dstX`/`dstY`/`dstW`/`dstH`. Preserves aspect ratio,
    /// centers the image, and allows letterbox/pillarbox (no crop, no stretch).
    static Rect aspectFit(int srcW, int srcH,
                          int dstX, int dstY, int dstW, int dstH);

private:
    /// Current effective clip rectangle (intersection of the clip stack).
    Rect currentClip() const;

    /// Blend a straight-alpha source pixel over the destination pixel.
    /// The surface is stored in RGBA8 byte order (byte 0 = R, byte 1 = G,
    /// byte 2 = B, byte 3 = A) to match the hardware framebuffer.
    static void blendPixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a);

    /// Clip a destination rectangle to the framebuffer and current clip.
    /// Returns false if the result is empty.
    bool clipRect(const Rect& in, Rect& out) const;

    // --- State --------------------------------------------------------------
    uint8_t* m_pixels = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pitch = 0;
    FontRasterizer* m_fonts = nullptr;
    bool m_initialized = false;

    /// Fixed-capacity clip stack. No heap allocation.
    static constexpr int kMaxClipDepth = 16;
    Rect m_clipStack[kMaxClipDepth];
    int m_clipDepth = 0;
};

} // namespace ps4

#endif // PS4_UI_COMPOSITOR_H

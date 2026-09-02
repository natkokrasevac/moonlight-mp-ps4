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

// === FontRasterizer.h =========================================================
//
// OpenOrbis PS4 — CPU font rasterizer.
//
// This component is a narrow, self-contained subsystem that rasterizes text on
// the CPU for the dedicated UI renderers. It uses FreeType (via the OpenOrbis
// libSceFreeType sysmodule) to rasterize text from embedded TrueType font
// assets into A8 coverage buffers, then blends the coverage into a CPU RGBA8
// surface.
//
// Responsibilities:
//   - Initialize the FreeType library (the FreeType sysmodule is loaded by
//     SysmoduleManager at process startup).
//   - Open the embedded Noto Sans font faces (Regular/Bold semantics).
//   - Rasterize ASCII text on the CPU into A8 coverage buffers.
//   - Blend glyph coverage into a CPU RGBA8 surface.
//   - Provide text measurement and layout for the compositor.
//   - Cache rasterized glyphs so repeated draws do not re-rasterize.
//
// The FreeType sysmodule (FREETYPE_OL) is loaded at process startup by
// SysmoduleManager. FontRasterizer does NOT own sysmodule loading; it only
// consumes the loaded FreeType service.
//
// Explicitly out of scope:
//   - GPU texture creation, GNMX blitter, GfxManager changes.
//   - Shaped/BiDi text, general UTF-8 decoding.
//
// All FreeType types (FT_Library, FT_Face, FT_GlyphSlot, etc.) are confined to
// the implementation file via a Pimpl. They are never exposed through this
// public header.
// =============================================================================

#ifndef PS4_FONT_RASTERIZER_H
#define PS4_FONT_RASTERIZER_H

#include <cstdint>   // uint8_t

namespace ps4 {

/// CPU font rasterizer.
///
/// This class is main-thread owned. It rasterizes text on the CPU for the
/// dedicated UI renderers.
class FontRasterizer {
public:
    /// Constructor performs no fallible operations.
    FontRasterizer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~FontRasterizer();

    // Non-copyable, non-movable.
    FontRasterizer(const FontRasterizer&) = delete;
    FontRasterizer& operator=(const FontRasterizer&) = delete;
    FontRasterizer(FontRasterizer&&) = delete;
    FontRasterizer& operator=(FontRasterizer&&) = delete;

    /// Initialize the FreeType library and font faces. The FreeType sysmodule
    /// is loaded by SysmoduleManager at process startup. Returns true on
    /// success. On failure the object remains safe to shut down or destroy.
    bool initialize();

    /// Release all font resources in reverse dependency order. Safe after
    /// partial initialization, normal completion, or repeated calls
    /// (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Selects which opened font set to use for text drawing.
    enum class FontFace : int {
        Regular      = 0,   ///< Noto Sans Regular (bound at kBodyFontSize).
        Bold         = 1,   ///< Noto Sans Regular (bound at kBoldFontSize).
        Boot         = 2,   ///< Noto Sans Regular (bound at kBootFontSize) — Boot fallback.
        Title        = 3,   ///< Noto Sans Regular (bound at kTitleFontSize) — window titles.
        BoldSmall    = 4,   ///< Noto Sans Regular (bound at kBoldSmallFontSize) — non-focused card names.
        RegularSmall = 5,   ///< Noto Sans ExtraLight (bound at kRegularSmallFontSize) — smaller regular text.
    };

    /// Measure the total advance width (including kerning) of an ASCII string
    /// in the given font face. Returns the width in pixels (float). Requires
    /// successful initialize(). Returns 0.0f if the font is not available.
    float measureTextWidth(FontFace face, const char* text);

    /// Draw an ASCII string into a caller-provided CPU RGBA8 surface using the
    /// given font face. `x` is the left pen position and `baselineY` is the
    /// baseline (in surface coordinates). Glyph A8 coverage is blended into
    /// the surface with straight-alpha source-over. `pitch` is the surface
    /// row stride in bytes (>= surfaceW * 4). Returns the total advance width
    /// drawn, or 0.0f on failure.
    float drawText(FontFace face, const char* text,
                   float x, float baselineY,
                   uint8_t r, uint8_t g, uint8_t b,
                   uint8_t* rgba, int surfaceW, int surfaceH, int pitch);

    /// Returns the horizontal layout (baseline/line-height) for a font face.
    /// Used by the compositor for vertical text layout. Returns false if the
    /// font is not available.
    bool getHorizontalLayout(FontFace face, float& outBaselineY,
                             float& outLineHeight) const;

    // -----------------------------------------------------------------------
    // Glyph bitmap cache diagnostics.
    //
    // The glyph cache is owned by FontRasterizer (it is tightly coupled to the
    // font faces, glyph metrics, and FreeType rasterization). UiCompositor
    // continues to call drawText() without knowing whether glyphs are cached.
    //
    // The cache is keyed by {FontFace, uint32_t codepoint} and stores the tight
    // A8 coverage bitmap plus the metrics/offsets/advance needed to place it.
    // It grows for the process lifetime (no eviction/LRU) because the system UI
    // uses a very small glyph set.
    // -----------------------------------------------------------------------

    /// Concise glyph-cache diagnostics. Returns true if the cache is available.
    struct GlyphCacheStats {
        uint64_t hits = 0;          ///< Cache hits (glyph already cached).
        uint64_t misses = 0;        ///< Cache misses (glyph rasterized once).
        uint32_t glyphCount = 0;    ///< Number of distinct cached glyphs.
        uint64_t coverageBytes = 0; ///< Total cached A8 coverage bytes.
    };
    bool getGlyphCacheStats(GlyphCacheStats& out) const;

    /// Per-glyph cache diagnostic for a single {face, codepoint}. Returns
    /// false if the glyph is not cached. Used to validate that visible glyphs
    /// carry real A8 coverage bytes and that spaces/empty glyphs remain valid
    /// (width/height == 0, empty coverage, valid advance).
    struct GlyphCacheEntryInfo {
        uint32_t width = 0;          ///< Tight bitmap width (px).
        uint32_t height = 0;         ///< Tight bitmap height (px).
        float offsetX = 0.0f;        ///< Bitmap left relative to pen X.
        float offsetY = 0.0f;        ///< Bitmap top relative to baseline Y.
        float advance = 0.0f;        ///< Horizontal pen advance (px).
        uint64_t coverageBytes = 0;  ///< Cached A8 coverage bytes (w*h).
        uint64_t nonzeroPixels = 0;  ///< Count of nonzero coverage pixels.
        uint8_t minCoverage = 0;     ///< Minimum nonzero coverage value.
        uint8_t maxCoverage = 0;     ///< Maximum coverage value.
    };
    bool getGlyphCacheEntry(FontFace face, uint32_t codepoint,
                            GlyphCacheEntryInfo& out) const;

private:
    /// Resolve a FontFace to the corresponding FreeType face handle. Returns
    /// nullptr if the font is not available. Implemented in FontRasterizer.cpp
    /// (FreeType types are confined there).
    void* resolveFontHandle(FontFace face) const;

    /// Open and configure the embedded Noto Sans font faces (Regular/Bold
    /// semantics) and bind them to the FreeType library. Called by initialize()
    /// so the reusable text path has usable fonts. Returns true on success.
    bool openFonts();

    /// Opaque pointer to the private implementation state. Keeps all FreeType
    /// types out of this public header. The definition of Impl lives in
    /// FontRasterizer.cpp.
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_FONT_RASTERIZER_H

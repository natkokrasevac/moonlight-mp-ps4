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

// === FontRasterizer.cpp ======================================================
//
// OpenOrbis PS4 — CPU font rasterizer.
//
// Implements the FontRasterizer component:
//   - Initializes the FreeType library.
//   - Opens the embedded Noto Sans font faces (Regular/Bold semantics).
//   - Rasterizes ASCII text on the CPU into A8 coverage buffers.
//   - Blends glyph coverage into a CPU RGBA8 surface.
//   - Provides text measurement and layout for the compositor.
//   - Caches rasterized glyphs so repeated draws do not re-rasterize.
//
// The FreeType sysmodule (FREETYPE_OL) is loaded at process startup by
// SysmoduleManager. FontRasterizer does NOT own sysmodule loading; it only
// consumes the loaded FreeType service.
//
// FreeType includes are confined to this translation unit.
// =============================================================================

#include "FontRasterizer.h"

// --- FreeType headers (private to this translation unit) ---------------------
// The OpenOrbis toolchain ships a stripped-down freetype.h plus a
// proto-include.h that declares the FreeType functions used by the sample
// (FT_Init_FreeType, FT_New_Face, FT_Load_Char, FT_Get_Char_Index,
// FT_Get_Kerning, FT_Render_Glyph, etc.). proto-include.h pulls in the base
// FreeType headers and adds the missing prototypes.
#include <proto-include.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>              // printf, fflush, snprintf
#include <cstdlib>             // malloc, free, calloc
#include <cstring>             // memset, memcpy
#include <unordered_map>       // glyph cache container
#include <vector>              // cached glyph coverage storage

// ---------------------------------------------------------------------------
// Diagnostics helper (consistent with other platform modules)
// ---------------------------------------------------------------------------

static void fontDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of FontRasterizer. Holds all FreeType state.
struct FontRasterizer::Impl {
    // --- FreeType library state --------------------------------------------
    bool libraryCreated  = false;   ///< FT_Init_FreeType succeeded.

    FT_Library library = nullptr;   ///< FreeType library instance.

    // --- Font faces (opened during initialize) ------------------------------
    FT_Face regularFace   = nullptr;  ///< Noto Sans Regular (body).
    FT_Face boldFace      = nullptr;  ///< Noto Sans Regular (bold semantics).
    FT_Face bootFace      = nullptr;  ///< Noto Sans Regular (boot).
    FT_Face titleFace     = nullptr;  ///< Noto Sans Regular (title).
    FT_Face boldSmallFace = nullptr;  ///< Noto Sans Regular (bold small).
    FT_Face regularSmallFace = nullptr; ///< Noto Sans ExtraLight (regular small).
    FT_Face regularBoldFace = nullptr;  ///< Noto Sans Regular (body-size bold).

    // --- Lifecycle state ----------------------------------------------------
    bool initialized = false;   ///< Full initialize() succeeded.

    // --- Step 2b: Glyph bitmap cache ----------------------------------------
    /// A single cached glyph: the tight A8 coverage bitmap plus the metrics
    /// and positioning information needed to place it relative to the pen.
    ///
    /// `offsetX`/`offsetY` are the cached bitmap's top-left position relative
    /// to the pen origin (pen X, baseline Y) at the time the glyph was
    /// rasterized. When drawing, the bitmap is placed at surface position
    /// `(penX + offsetX, baselineY + offsetY)`.
    ///
    /// A glyph may legitimately have `width == 0 && height == 0` (e.g. a
    /// space): `advance` is still valid and `coverage` is empty. Such entries
    /// are cached so they are not re-rasterized on every draw.
    struct CachedGlyph {
        uint32_t width = 0;          ///< Tight bitmap width (px).
        uint32_t height = 0;         ///< Tight bitmap height (px).
        float offsetX = 0.0f;        ///< Bitmap left relative to pen X.
        float offsetY = 0.0f;        ///< Bitmap top relative to baseline Y.
        float advance = 0.0f;        ///< Horizontal pen advance (px).
        std::vector<uint8_t> coverage; ///< Tight A8 coverage (width*height).
    };

    /// Cache key: packed {FontFace, uint32_t codepoint}. Face is 0..6 and
    /// codepoint is a full uint32_t, so the key is a single uint64_t.
    using GlyphCacheKey = uint64_t;

    /// The glyph cache. Grows for the process lifetime (no eviction/LRU).
    std::unordered_map<GlyphCacheKey, CachedGlyph> glyphCache;

    // Cache diagnostics counters.
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

FontRasterizer::FontRasterizer()
    : m_impl(new Impl())
{
}

FontRasterizer::~FontRasterizer() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool FontRasterizer::initialize() {
    if (m_impl->initialized) {
        return true;
    }

    fontDiagPrint("[font] initialization start");

    // 1. The FreeType sysmodule (FREETYPE_OL) is loaded at process startup by
    //    SysmoduleManager. FontRasterizer does NOT own sysmodule loading; it
    //    only consumes the loaded FreeType service.

    // 2. Initialize the FreeType library.
    int rc = FT_Init_FreeType(&m_impl->library);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[font] FT_Init_FreeType failed: %d", rc);
        fontDiagPrint(buf);
        shutdown();
        return false;
    }
    m_impl->libraryCreated = true;

    // 3. Open and configure the embedded Noto Sans font faces.
    if (!openFonts()) {
        fontDiagPrint("[font] openFonts failed");
        shutdown();
        return false;
    }

    m_impl->initialized = true;
    fontDiagPrint("[font] initialization success");
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void FontRasterizer::shutdown() {
    if (!m_impl) {
        return;
    }

    // Reverse dependency order.

    // Clear the glyph cache before destroying any font resources. The cached
    // coverage bitmaps depend on the font faces they were rasterized from,
    // so they must be released before the faces are closed below.
    m_impl->glyphCache.clear();
    m_impl->cacheHits = 0;
    m_impl->cacheMisses = 0;

    // Close any opened font faces.
    if (m_impl->regularFace) {
        FT_Done_Face(m_impl->regularFace);
        m_impl->regularFace = nullptr;
    }
    if (m_impl->boldFace) {
        FT_Done_Face(m_impl->boldFace);
        m_impl->boldFace = nullptr;
    }
    if (m_impl->bootFace) {
        FT_Done_Face(m_impl->bootFace);
        m_impl->bootFace = nullptr;
    }
    if (m_impl->titleFace) {
        FT_Done_Face(m_impl->titleFace);
        m_impl->titleFace = nullptr;
    }
    if (m_impl->boldSmallFace) {
        FT_Done_Face(m_impl->boldSmallFace);
        m_impl->boldSmallFace = nullptr;
    }
    if (m_impl->regularSmallFace) {
        FT_Done_Face(m_impl->regularSmallFace);
        m_impl->regularSmallFace = nullptr;
    }
    if (m_impl->regularBoldFace) {
        FT_Done_Face(m_impl->regularBoldFace);
        m_impl->regularBoldFace = nullptr;
    }

    // Destroy the FreeType library.
    if (m_impl->libraryCreated && m_impl->library) {
        FT_Done_FreeType(m_impl->library);
        m_impl->library = nullptr;
        m_impl->libraryCreated = false;
    }

    // The FreeType sysmodule is unloaded by SysmoduleManager at shutdown.
    // FontRasterizer does NOT own sysmodule unloading.

    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool FontRasterizer::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Font set opening (reusable runtime path)
// ---------------------------------------------------------------------------

/// Open and configure the embedded Noto Sans font faces (Regular/Bold
/// semantics) and bind them to the FreeType library. This makes the reusable
/// text path usable by the CPU Host/App/Settings/Modal renderers.
///
/// Font source:
///   - NotoSans-Regular.ttf      → Regular, Bold, Boot, Title, BoldSmall
///   - NotoSans-ExtraLight.ttf   → RegularSmall
///
/// The Noto Sans Regular face is used for both Regular and Bold semantics
/// because the project ships only two Noto Sans weights (ExtraLight and
/// Regular). There is no true Bold weight available; the Bold face is bound to
/// the Regular file. This is a documented limitation (no fake bold is applied).
bool FontRasterizer::openFonts() {
    if (!m_impl) {
        return false;
    }
    // The FreeType library must already be created (by initialize()).
    if (!m_impl->library) {
        return false;
    }

    // Runtime font paths (packaged under /app0/assets/fonts/).
    static const char* kRegularPath = "/app0/assets/fonts/NotoSans-Regular.ttf";
    static const char* kExtraLightPath = "/app0/assets/fonts/NotoSans-ExtraLight.ttf";

    // Font sizes (from the shared UiTheme semantics).
    const float regularSize   = 28.0f;   // kBodyFontSize
    const float boldSize      = 28.0f;   // kBoldFontSize
    const float bootSize      = 64.0f;   // kBootFontSize
    const float titleSize     = 48.0f;   // kTitleFontSize
    const float boldSmallSize = 20.0f;   // kBoldSmallFontSize
    const float regularSmallSize = 20.0f; // kRegularSmallFontSize

    // Helper to open a face at a given pixel size.
    auto openFace = [&](const char* path, float size, FT_Face& out) -> bool {
        FT_Face face = nullptr;
        int rc = FT_New_Face(m_impl->library, path, 0, &face);
        if (rc != 0) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[font] FT_New_Face(%s) failed: %d", path, rc);
            fontDiagPrint(buf);
            return false;
        }
        rc = FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        if (rc != 0) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[font] FT_Set_Pixel_Sizes(%s, %.0f) failed: %d",
                     path, size, rc);
            fontDiagPrint(buf);
            FT_Done_Face(face);
            return false;
        }
        out = face;
        return true;
    };

    // Open the faces. Regular/Bold/Boot/Title/BoldSmall use the Regular file;
    // RegularSmall uses the ExtraLight file.
    if (!openFace(kRegularPath, regularSize, m_impl->regularFace)) return false;
    if (!openFace(kRegularPath, boldSize, m_impl->boldFace)) return false;
    if (!openFace(kRegularPath, bootSize, m_impl->bootFace)) return false;
    if (!openFace(kRegularPath, titleSize, m_impl->titleFace)) return false;
    if (!openFace(kRegularPath, boldSmallSize, m_impl->boldSmallFace)) return false;
    if (!openFace(kExtraLightPath, regularSmallSize, m_impl->regularSmallFace)) return false;
    // RegularBold: body-size bold for emphasized values (e.g. the Settings
    // bitrate warning). Bound to the Regular file at the bold size, matching
    // the existing Bold semantics (no true Bold weight is shipped).
    if (!openFace(kRegularPath, boldSize, m_impl->regularBoldFace)) return false;

    fontDiagPrint("[font] Noto Sans Regular/ExtraLight font faces opened and configured");
    return true;
}

// ---------------------------------------------------------------------------
// Blend helpers (file-local)
// ---------------------------------------------------------------------------

namespace {

/// Simple source-over alpha blend of an A8 coverage value into an RGBA8 pixel.
/// Straight-alpha output (no premultiplied-alpha assumptions).
inline void blendA8IntoRgba(uint8_t* dst, uint8_t coverage,
                            uint8_t r, uint8_t g, uint8_t b) {
    if (coverage == 0) {
        return;
    }
    if (coverage == 255) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255;
        return;
    }
    const uint32_t a = coverage;
    const uint32_t invA = 255u - a;
    dst[0] = static_cast<uint8_t>((r * a + dst[0] * invA) / 255u);
    dst[1] = static_cast<uint8_t>((g * a + dst[1] * invA) / 255u);
    dst[2] = static_cast<uint8_t>((b * a + dst[2] * invA) / 255u);
    dst[3] = 255;
}

/// Blend a glyph's A8 coverage into the RGBA8 target at the given destination
/// position, using the glyph's image metrics to determine the covered region.
///
/// `destX`/`destY` are the surface coordinates where the glyph's top-left
/// image box begins. `imageW`/`imageH` are the glyph image dimensions. `pitch`
/// is the destination surface row stride in bytes (>= surfaceW * 4).
void blendGlyphIntoRgba(uint8_t* rgba, int surfaceW, int surfaceH, int pitch,
                        const uint8_t* a8, int a8W, int a8H,
                        int destX, int destY, int imageW, int imageH,
                        uint8_t r, uint8_t g, uint8_t b,
                        int clipX0, int clipY0, int clipX1, int clipY1) {
    for (int y = 0; y < imageH; ++y) {
        const int sy = destY + y;
        if (sy < clipY0 || sy >= clipY1 || sy < 0 || sy >= surfaceH) {
            continue;
        }
        for (int x = 0; x < imageW; ++x) {
            const int sx = destX + x;
            if (sx < clipX0 || sx >= clipX1 || sx < 0 || sx >= surfaceW) {
                continue;
            }
            const uint8_t cov = a8[y * a8W + x];
            if (cov == 0) {
                continue;
            }
            uint8_t* dst = rgba + sy * pitch + sx * 4;
            blendA8IntoRgba(dst, cov, r, g, b);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Reusable compositor text path
// ---------------------------------------------------------------------------

/// Resolve a FontFace to the corresponding FreeType face handle. Returns
/// nullptr if the font is not available.
void* FontRasterizer::resolveFontHandle(FontFace face) const {
    if (!m_impl) {
        return nullptr;
    }
    switch (face) {
    case FontFace::Regular:
        return m_impl->regularFace;
    case FontFace::Bold:
        return m_impl->boldFace;
    case FontFace::Boot:
        return m_impl->bootFace;
    case FontFace::Title:
        return m_impl->titleFace;
    case FontFace::BoldSmall:
        return m_impl->boldSmallFace;
    case FontFace::RegularSmall:
        return m_impl->regularSmallFace;
    case FontFace::RegularBold:
        return m_impl->regularBoldFace;
    }
    return nullptr;
}

float FontRasterizer::measureTextWidth(FontFace face, const char* text) {
    if (!m_impl || !m_impl->initialized || !text) {
        return 0.0f;
    }
    FT_Face font = static_cast<FT_Face>(resolveFontHandle(face));
    if (!font) {
        return 0.0f;
    }

    float x = 0.0f;
    uint32_t preCode = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const uint32_t code =
            static_cast<uint32_t>(static_cast<unsigned char>(*p));

        // Apply kerning between the previous and current character.
        if (preCode != 0) {
            FT_UInt left = FT_Get_Char_Index(font, preCode);
            FT_UInt right = FT_Get_Char_Index(font, code);
            FT_Vector kern;
            if (FT_Get_Kerning(font, left, right, FT_KERNING_DEFAULT, &kern) == 0) {
                x += static_cast<float>(kern.x) / 64.0f;
            }
        }

        // Load the glyph to get its advance.
        if (FT_Load_Char(font, code, FT_LOAD_DEFAULT) == 0) {
            x += static_cast<float>(font->glyph->advance.x) / 64.0f;
        }
        preCode = code;
    }
    return x;
}

float FontRasterizer::drawText(FontFace face, const char* text,
                               float x, float baselineY,
                               uint8_t r, uint8_t g, uint8_t b,
                               uint8_t* rgba, int surfaceW, int surfaceH,
                               int pitch,
                               int clipX, int clipY, int clipW, int clipH) {
    if (!m_impl || !m_impl->initialized || !text || !rgba ||
        surfaceW <= 0 || surfaceH <= 0 || pitch < surfaceW * 4) {
        return 0.0f;
    }

    int clipX0 = clipX;
    int clipY0 = clipY;
    int clipX1 = (clipW >= 0) ? (clipX + clipW) : surfaceW;
    int clipY1 = (clipH >= 0) ? (clipY + clipH) : surfaceH;
    if (clipX0 < 0) clipX0 = 0;
    if (clipY0 < 0) clipY0 = 0;
    if (clipX1 > surfaceW) clipX1 = surfaceW;
    if (clipY1 > surfaceH) clipY1 = surfaceH;
    if (clipX1 <= clipX0 || clipY1 <= clipY0) {
        return measureTextWidth(face, text);
    }
    FT_Face font = static_cast<FT_Face>(resolveFontHandle(face));
    if (!font) {
        return 0.0f;
    }

    const float startX = x;
    float penX = x;
    uint32_t preCode = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const uint32_t code =
            static_cast<uint32_t>(static_cast<unsigned char>(*p));

        // Apply kerning between the previous and current character. Kerning is
        // pair-dependent and is deliberately NOT baked into the per-glyph
        // cache; it is applied here on every draw as before.
        if (preCode != 0) {
            FT_UInt left = FT_Get_Char_Index(font, preCode);
            FT_UInt right = FT_Get_Char_Index(font, code);
            FT_Vector kern;
            if (FT_Get_Kerning(font, left, right, FT_KERNING_DEFAULT, &kern) == 0) {
                penX += static_cast<float>(kern.x) / 64.0f;
            }
        }

        // Build the cache key for this {face, codepoint}.
        const Impl::GlyphCacheKey key =
            (static_cast<Impl::GlyphCacheKey>(static_cast<int>(face)) << 32) |
            static_cast<Impl::GlyphCacheKey>(code);

        // Look up the glyph in the cache.
        auto it = m_impl->glyphCache.find(key);
        if (it != m_impl->glyphCache.end()) {
            // Cache hit: reuse the cached coverage bitmap and metrics. No
            // FreeType rasterization occurs for an already-cached glyph.
            ++m_impl->cacheHits;
            const Impl::CachedGlyph& cg = it->second;

            // Place the cached bitmap at the same position relative to the pen
            // as when it was rasterized.
            const int destX = static_cast<int>(penX + cg.offsetX);
            const int destY = static_cast<int>(baselineY + cg.offsetY);
            if (cg.width > 0 && cg.height > 0 && !cg.coverage.empty()) {
                blendGlyphIntoRgba(rgba, surfaceW, surfaceH, pitch,
                                   cg.coverage.data(),
                                   static_cast<int>(cg.width),
                                   static_cast<int>(cg.height),
                                   destX, destY,
                                   static_cast<int>(cg.width),
                                   static_cast<int>(cg.height),
                                   r, g, b,
                                   clipX0, clipY0, clipX1, clipY1);
            }
            // Zero-bitmap glyphs (e.g. space) have width/height == 0 and an
            // empty coverage vector; they still advance the pen below.

            penX += cg.advance;
            preCode = code;
            continue;
        }

        // Cache miss: rasterize once through FreeType.
        ++m_impl->cacheMisses;

        // Load and render the glyph into the glyph slot's bitmap.
        int rc = FT_Load_Char(font, code, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
        if (rc != 0) {
            // The glyph did not produce a bitmap. This may be a legitimate
            // non-rendering glyph (e.g. a space: advance > 0, no visible
            // coverage) or an actual FreeType failure. Distinguish the two:
            // if the glyph has a valid advance, cache it as a zero-bitmap
            // entry so it is not re-rasterized on every draw.
            if (FT_Load_Char(font, code, FT_LOAD_DEFAULT) == 0) {
                Impl::CachedGlyph cg;
                cg.width = 0;
                cg.height = 0;
                cg.offsetX = 0.0f;
                cg.offsetY = 0.0f;
                cg.advance = static_cast<float>(font->glyph->advance.x) / 64.0f;
                m_impl->glyphCache.emplace(key, std::move(cg));
                penX += cg.advance;
            } else {
                char ebuf[128];
                snprintf(ebuf, sizeof(ebuf),
                         "[font-cache] glyph rasterize failed face=%d code=%u rc=%d",
                         static_cast<int>(face), static_cast<unsigned>(code), rc);
                fontDiagPrint(ebuf);
            }
            preCode = code;
            continue;
        }

        // Extract the glyph bitmap and metrics from the glyph slot.
        FT_GlyphSlot slot = font->glyph;
        const int bitmapW = slot->bitmap.width;
        const int bitmapH = slot->bitmap.rows;
        const int bitmapLeft = slot->bitmap_left;
        const int bitmapTop = slot->bitmap_top;
        const float advance = static_cast<float>(slot->advance.x) / 64.0f;

        Impl::CachedGlyph cg;
        cg.advance = advance;
        // The bitmap's top-left relative to the pen origin (pen X, baseline Y).
        // FreeType's bitmap_top is the distance from the baseline to the
        // top-most glyph scanline (positive upward), so the top-left Y offset
        // relative to the baseline is (baseline - bitmap_top).
        cg.offsetX = static_cast<float>(bitmapLeft);
        cg.offsetY = static_cast<float>(-bitmapTop);

        if (bitmapW > 0 && bitmapH > 0 && slot->bitmap.buffer) {
            cg.width = static_cast<uint32_t>(bitmapW);
            cg.height = static_cast<uint32_t>(bitmapH);
            cg.coverage.resize(static_cast<size_t>(bitmapW) * bitmapH);
            // Copy the tight bitmap out of the glyph slot. The bitmap pitch
            // may be negative (top-down) or have padding; handle both.
            const int srcPitch = slot->bitmap.pitch;
            for (int yy = 0; yy < bitmapH; ++yy) {
                const uint8_t* srcRow =
                    slot->bitmap.buffer + static_cast<size_t>(yy) * srcPitch;
                uint8_t* dstRow =
                    cg.coverage.data() + static_cast<size_t>(yy) * bitmapW;
                memcpy(dstRow, srcRow, static_cast<size_t>(bitmapW));
            }
        }
        // For a valid empty glyph (e.g. space), bitmapW/bitmapH may be 0: the
        // entry is still cached with a valid advance and empty coverage so it
        // is not re-rasterized on every draw.

        // Blend the freshly rasterized glyph into the target using the SAME
        // offset-based placement as the warm path, so the first (cold) draw
        // and subsequent (warm) cached draws are byte-identical.
        if (bitmapW > 0 && bitmapH > 0) {
            const int destX = static_cast<int>(penX + cg.offsetX);
            const int destY = static_cast<int>(baselineY + cg.offsetY);
            blendGlyphIntoRgba(rgba, surfaceW, surfaceH, pitch,
                               cg.coverage.data(),
                               static_cast<int>(cg.width),
                               static_cast<int>(cg.height),
                               destX, destY,
                               static_cast<int>(cg.width),
                               static_cast<int>(cg.height),
                               r, g, b,
                               clipX0, clipY0, clipX1, clipY1);
        }

        // Insert the cached entry (moves the coverage vector into the cache).
        m_impl->glyphCache.emplace(key, std::move(cg));

        // Advance the pen using the glyph's horizontal advance.
        penX += advance;
        preCode = code;
    }
    return penX - startX;
}

bool FontRasterizer::getHorizontalLayout(FontFace face,
                                         float& outBaselineY,
                                         float& outLineHeight) const {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }
    FT_Face font = static_cast<FT_Face>(resolveFontHandle(face));
    if (!font) {
        return false;
    }
    // FreeType size metrics are in 26.6 fractional pixels.
    //   ascender  = distance from baseline to top of glyphs (positive).
    //   descender = distance from baseline to bottom of glyphs (negative).
    //   height    = vertical distance between two consecutive baselines.
    outBaselineY = static_cast<float>(font->size->metrics.ascender) / 64.0f;
    outLineHeight = static_cast<float>(font->size->metrics.height) / 64.0f;
    return true;
}

bool FontRasterizer::getGlyphCacheStats(GlyphCacheStats& out) const {
    if (!m_impl) {
        return false;
    }
    out.hits = m_impl->cacheHits;
    out.misses = m_impl->cacheMisses;
    out.glyphCount = static_cast<uint32_t>(m_impl->glyphCache.size());
    out.coverageBytes = 0;
    for (const auto& kv : m_impl->glyphCache) {
        out.coverageBytes += kv.second.coverage.size();
    }
    return true;
}

bool FontRasterizer::getGlyphCacheEntry(FontFace face, uint32_t codepoint,
                                        GlyphCacheEntryInfo& out) const {
    if (!m_impl) {
        return false;
    }
    const Impl::GlyphCacheKey key =
        (static_cast<Impl::GlyphCacheKey>(static_cast<int>(face)) << 32) |
        static_cast<Impl::GlyphCacheKey>(codepoint);
    auto it = m_impl->glyphCache.find(key);
    if (it == m_impl->glyphCache.end()) {
        return false;
    }
    const Impl::CachedGlyph& cg = it->second;
    out.width = cg.width;
    out.height = cg.height;
    out.offsetX = cg.offsetX;
    out.offsetY = cg.offsetY;
    out.advance = cg.advance;
    out.coverageBytes = cg.coverage.size();
    out.nonzeroPixels = 0;
    out.minCoverage = 0;
    out.maxCoverage = 0;
    for (uint8_t v : cg.coverage) {
        if (v != 0) {
            ++out.nonzeroPixels;
            if (out.minCoverage == 0 || v < out.minCoverage) {
                out.minCoverage = v;
            }
            if (v > out.maxCoverage) {
                out.maxCoverage = v;
            }
        }
    }
    return true;
}

} // namespace ps4

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

// === UiRenderPrimitives.cpp =================================================
//
// Moonlight client for PlayStation 4 — shared UI rendering primitives.
//
// Implements the small set of genuinely shared low-level drawing operations
// used by the dedicated CPU renderers. These helpers are procedural and
// stateless; they never own the atlas image, artwork image, or any retained
// resource. Callers pass the images in.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the SCE reference stores image pixels in
// std::vector<uint8_t> and tests emptiness with `.empty()`. The OpenOrbis
// CpuImage holds a raw pointer + byte size (to avoid the toolchain libc++
// <vector> include issue), so emptiness is tested with `rgba == nullptr`.
// CpuPngImage exposes an `.empty()` method. A drawBackground overload for
// CpuPngImage is added so the BootRenderer can retain its decoded background
// as an owning CpuPngImage.
// =============================================================================

#include "UiRenderPrimitives.h"

#include "UiTheme.h"   // ui::UiTheme colors

#include <cstring>     // strchr, memcpy, strncmp

namespace ui {

// ---------------------------------------------------------------------------
// Atlas icon helpers
// ---------------------------------------------------------------------------

UiAtlasIcon acceptIcon(bool crossIsAccept) {
    // Atlas row 0: Cross | Circle | Square | Triangle.
    return crossIsAccept ? UiAtlasIcon::Cross : UiAtlasIcon::Circle;
}

UiAtlasIcon backIcon(bool crossIsAccept) {
    // Atlas row 0: Cross | Circle | Square | Triangle.
    return crossIsAccept ? UiAtlasIcon::Circle : UiAtlasIcon::Cross;
}

void drawAtlasIcon(ps4::UiCompositor& comp, const ps4::CpuImage& atlas,
                   UiAtlasIcon icon, const ps4::Rect& dst) {
    if (atlas.rgba == nullptr) {
        return;
    }
    // The atlas is a regular 4×4 grid of 64×64 cells. The UiAtlasIcon enum is
    // ordered row-major, so row = icon / kColumns and col = icon % kColumns.
    const int cell = UiAtlas::kCellSize;
    const int row = static_cast<int>(icon) / UiAtlas::kColumns;
    const int col = static_cast<int>(icon) % UiAtlas::kColumns;
    comp.drawImageSubRect(atlas,
                          col * cell, row * cell, cell, cell,
                          dst.x, dst.y, dst.w, dst.h);
}

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

void drawAspectFitImage(ps4::UiCompositor& comp, const ps4::CpuImage& image,
                        int dstX, int dstY, int dstW, int dstH) {
    if (image.rgba == nullptr) {
        return;
    }
    const ps4::Rect fit = ps4::UiCompositor::aspectFit(
        static_cast<int>(image.width), static_cast<int>(image.height),
        dstX, dstY, dstW, dstH);
    if (fit.w > 0 && fit.h > 0) {
        comp.drawImage(image, fit.x, fit.y, fit.w, fit.h);
    }
}

void drawAspectFitImage(ps4::UiCompositor& comp, const ps4::CpuPngImage& image,
                        int dstX, int dstY, int dstW, int dstH) {
    if (image.empty()) {
        return;
    }
    const ps4::Rect fit = ps4::UiCompositor::aspectFit(
        static_cast<int>(image.width), static_cast<int>(image.height),
        dstX, dstY, dstW, dstH);
    if (fit.w > 0 && fit.h > 0) {
        comp.drawImage(image, fit.x, fit.y, fit.w, fit.h);
    }
}

void drawAspectCoverImage(ps4::UiCompositor& comp, const ps4::CpuImage& image,
                          int dstX, int dstY, int dstW, int dstH) {
    if (image.rgba == nullptr || dstW <= 0 || dstH <= 0 || image.width <= 0 || image.height <= 0) {
        return;
    }
    const float srcAspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    const float dstAspect = static_cast<float>(dstW) / static_cast<float>(dstH);

    int srcX = 0, srcY = 0;
    int srcSubW = static_cast<int>(image.width);
    int srcSubH = static_cast<int>(image.height);

    if (srcAspect > dstAspect) {
        srcSubW = static_cast<int>(static_cast<float>(image.height) * dstAspect);
        if (srcSubW > static_cast<int>(image.width)) srcSubW = static_cast<int>(image.width);
        srcX = (static_cast<int>(image.width) - srcSubW) / 2;
    } else {
        srcSubH = static_cast<int>(static_cast<float>(image.width) / dstAspect);
        if (srcSubH > static_cast<int>(image.height)) srcSubH = static_cast<int>(image.height);
        srcY = (static_cast<int>(image.height) - srcSubH) / 2;
    }

    comp.drawImageSubRect(image, srcX, srcY, srcSubW, srcSubH, dstX, dstY, dstW, dstH);
}

void drawAspectCoverImage(ps4::UiCompositor& comp, const ps4::CpuPngImage& image,
                          int dstX, int dstY, int dstW, int dstH) {
    if (image.empty() || dstW <= 0 || dstH <= 0 || image.width <= 0 || image.height <= 0) {
        return;
    }
    const float srcAspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    const float dstAspect = static_cast<float>(dstW) / static_cast<float>(dstH);

    int srcX = 0, srcY = 0;
    int srcSubW = static_cast<int>(image.width);
    int srcSubH = static_cast<int>(image.height);

    if (srcAspect > dstAspect) {
        srcSubW = static_cast<int>(static_cast<float>(image.height) * dstAspect);
        if (srcSubW > static_cast<int>(image.width)) srcSubW = static_cast<int>(image.width);
        srcX = (static_cast<int>(image.width) - srcSubW) / 2;
    } else {
        srcSubH = static_cast<int>(static_cast<float>(image.width) / dstAspect);
        if (srcSubH > static_cast<int>(image.height)) srcSubH = static_cast<int>(image.height);
        srcY = (static_cast<int>(image.height) - srcSubH) / 2;
    }

    comp.drawImageSubRect(image, srcX, srcY, srcSubW, srcSubH, dstX, dstY, dstW, dstH);
}

void drawControllerIndicators(ps4::UiCompositor& comp,
                              const ps4::CpuImage& controllerImg,
                              uint8_t controllerMask) {
    if (controllerImg.rgba == nullptr) {
        return;
    }

    const float iconW = 48.0f;
    const float iconH = 32.0f;
    const float gap = 12.0f;
    const float totalW = 4.0f * iconW + 3.0f * gap;
    const float startX = 1920.0f - UiTheme::kTitleTextX - totalW;
    const float startY = UiTheme::kTitleTextY - iconH;

    for (int i = 0; i < 4; ++i) {
        const int dstX = static_cast<int>(startX + static_cast<float>(i) * (iconW + gap));
        const int dstY = static_cast<int>(startY);

        const bool active = (controllerMask & (1 << i)) != 0;

        const int srcX = active ? 0 : 48;
        comp.drawImageSubRect(controllerImg, srcX, 0, 48, 32,
                              dstX, dstY, static_cast<int>(iconW), static_cast<int>(iconH));
    }
}

void drawBackground(ps4::UiCompositor& comp, const ps4::CpuImage& bg) {
    if (bg.rgba == nullptr) {
        return;
    }
    // The background is a full-screen 1920×1080 image drawn at the origin.
    comp.drawImage(bg, 0, 0, 1920, 1080);
}

void drawBackground(ps4::UiCompositor& comp, const ps4::CpuPngImage& bg) {
    if (bg.empty()) {
        return;
    }
    // The background is a full-screen 1920×1080 image drawn at the origin.
    comp.drawImage(bg, 0, 0, 1920, 1080);
}

// ---------------------------------------------------------------------------
// Card drawing helpers
// ---------------------------------------------------------------------------

void drawCardOutline(ps4::UiCompositor& comp, int x, int y, int w, int h,
                     bool focused) {
    if (focused) {
        comp.strokeRect(x, y, w, h, 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    } else {
        comp.strokeRect(x, y, w, h, 2,
                        UiTheme::kCardOutlineR, UiTheme::kCardOutlineG, UiTheme::kCardOutlineB, 0);
    }
}

void drawFocusFooter(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                     float left, float top, float w, float h,
                     float footerHeight, const char* caption,
                     float baselineY, float lineH) {
    const int ix = static_cast<int>(left);
    const int iy = static_cast<int>(top)-1;
    const int iw = static_cast<int>(w);
    const int ih = static_cast<int>(h);
    const int footerTop = iy + ih;
    const int footerH = static_cast<int>(footerHeight);

    comp.fillRect(ix, footerTop, iw, footerH,
                  UiTheme::kFooterFillR, UiTheme::kFooterFillG, UiTheme::kFooterFillB, 225);
    comp.strokeRect(ix, footerTop, iw, footerH, 2,
                    UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);

    // Caption centered in the footer.
    const float captionW = fonts.measureTextWidth(
        ps4::FontRasterizer::FontFace::Regular, caption);
    const float cx = left + (w - captionW) * 0.5f;
    const float cy = static_cast<float>(footerTop) +
                     (footerHeight - lineH) * 0.5f - 2;
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, caption,
                  cx, cy + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void drawBottomLegend(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                      const ps4::CpuImage& atlas, const ps4::Rect& bounds,
                      const LegendEntry* entries, size_t count) {
    if (count == 0) {
        return;
    }

    // The legend content starts at the theme bottom-bar content X (px from
    // left) and Y (px from the bottom legend viewport top).
    const float startX = UiTheme::kBottomBarContentX;
    const float startY = static_cast<float>(bounds.y) + UiTheme::kBottomBarContentY;

    const float glyphSize = UiAtlas::kGlyphSize;
    const float glyphToLabelGap = UiAtlas::kGlyphToLabelGap;
    const float entryGap = UiAtlas::kEntryGap;

    float baselineY = 0.0f, lineHeight = 0.0f;
    fonts.getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                              baselineY, lineHeight);

    float cursorX = startX;
    for (size_t i = 0; i < count; ++i) {
        const LegendEntry& e = entries[i];
        const float glyphY = startY + (lineHeight - glyphSize) * 0.5f;

        // Glyph.
        drawAtlasIcon(comp, atlas, e.icon,
                      ps4::Rect{static_cast<int>(cursorX),
                                static_cast<int>(glyphY),
                                static_cast<int>(glyphSize),
                                static_cast<int>(glyphSize)});

        // Label (vertically centered relative to the glyph).
        const float labelX = cursorX + glyphSize + glyphToLabelGap;
        const float labelBaseline = startY + baselineY;
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, e.label,
                      labelX, labelBaseline,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

        const float labelW = fonts.measureTextWidth(
            ps4::FontRasterizer::FontFace::Regular, e.label);
        cursorX += glyphSize + glyphToLabelGap + labelW + entryGap;
    }
}

void drawBottomLegendRightAligned(ps4::UiCompositor& comp,
                                  ps4::FontRasterizer& fonts,
                                  const ps4::CpuImage& atlas,
                                  const ps4::Rect& bounds,
                                  UiAtlasIcon icon, const char* label) {
    if (atlas.rgba == nullptr) {
        return;
    }

    const float glyphSize = UiAtlas::kGlyphSize;
    const float glyphToLabelGap = UiAtlas::kGlyphToLabelGap;

    float baselineY = 0.0f, lineHeight = 0.0f;
    fonts.getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                              baselineY, lineHeight);

    // The entry is right-aligned to the right edge of the legend bounds, with
    // the same right margin as the left content start (kBottomBarContentX).
    const float startY = static_cast<float>(bounds.y) + UiTheme::kBottomBarContentY;
    const float labelW = fonts.measureTextWidth(
        ps4::FontRasterizer::FontFace::Regular, label);
    const float entryW = glyphSize + glyphToLabelGap + labelW;
    const float rightMargin = UiTheme::kBottomBarContentX;
    const float entryLeft = static_cast<float>(bounds.x + bounds.w) -
                            rightMargin - entryW;

    const float glyphY = startY + (lineHeight - glyphSize) * 0.5f;

    // Glyph.
    drawAtlasIcon(comp, atlas, icon,
                  ps4::Rect{static_cast<int>(entryLeft),
                            static_cast<int>(glyphY),
                            static_cast<int>(glyphSize),
                            static_cast<int>(glyphSize)});

    // Label (vertically centered relative to the glyph).
    const float labelX = entryLeft + glyphSize + glyphToLabelGap;
    const float labelBaseline = startY + baselineY;
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, label,
                  labelX, labelBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Inline-glyph text helpers
// ---------------------------------------------------------------------------

/// Resolve an inline `[GlyphName]` marker to an atlas icon. Returns true and
/// sets `out` when the name matches a known glyph; returns false otherwise.
static bool inlineGlyphFromName(const char* name, size_t len, UiAtlasIcon& out) {
    struct GlyphName {
        const char* name;
        size_t len;
        UiAtlasIcon icon;
    };
    static const GlyphName kGlyphs[] = {
        {"Options",   7, UiAtlasIcon::Options},
        {"L3",        2, UiAtlasIcon::L3},
        {"PS",        2, UiAtlasIcon::PsButton},
        {"TouchpadR", 9, UiAtlasIcon::TouchpadR},
        {"TouchpadL", 9, UiAtlasIcon::TouchpadL},
        {"Touchpad",  8, UiAtlasIcon::Touchpad},
        {"Cross",     5, UiAtlasIcon::Cross},
        {"Circle",    6, UiAtlasIcon::Circle},
    };
    for (const auto& g : kGlyphs) {
        if (len == g.len && strncmp(name, g.name, len) == 0) {
            out = g.icon;
            return true;
        }
    }
    return false;
}

bool textHasInlineGlyph(const char* text) {
    const char* p = text;
    while ((p = strchr(p, '[')) != nullptr) {
        const char* close = strchr(p, ']');
        if (close) {
            UiAtlasIcon icon;
            if (inlineGlyphFromName(p + 1, static_cast<size_t>(close - p - 1), icon)) {
                return true;
            }
        }
        ++p;
    }
    return false;
}

float drawInlineGlyphText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                          const ps4::CpuImage& atlas,
                          ps4::FontRasterizer::FontFace face,
                          const char* text, float x, float baselineY,
                          float glyphY, uint8_t r, uint8_t g, uint8_t b,
                          bool draw) {
    float cursorX = x;
    const char* p = text;
    while (*p) {
        const char* bracket = strchr(p, '[');
        if (bracket) {
            // Draw the text segment before the '['.
            if (bracket > p) {
                char seg[256];
                size_t segLen = static_cast<size_t>(bracket - p);
                if (segLen >= sizeof(seg)) {
                    segLen = sizeof(seg) - 1;
                }
                memcpy(seg, p, segLen);
                seg[segLen] = '\0';
                if (draw) {
                    comp.drawText(face, seg, cursorX, baselineY, r, g, b);
                }
                cursorX += fonts.measureTextWidth(face, seg);
                p = bracket;
            }
            // Try to parse a glyph name.
            const char* close = strchr(p, ']');
            if (close) {
                UiAtlasIcon icon;
                if (inlineGlyphFromName(p + 1,
                                        static_cast<size_t>(close - p - 1), icon)) {
                    if (draw) {
                        drawAtlasIcon(comp, atlas, icon,
                                      ps4::Rect{static_cast<int>(cursorX),
                                                static_cast<int>(glyphY),
                                                static_cast<int>(UiAtlas::kGlyphSize),
                                                static_cast<int>(UiAtlas::kGlyphSize)});
                    }
                    cursorX += UiAtlas::kGlyphSize + UiAtlas::kGlyphToLabelGap;
                    p = close + 1;
                    continue;
                }
            }
            // Not a valid glyph; treat '[' as a literal character.
            if (draw) {
                comp.drawText(face, "[", cursorX, baselineY, r, g, b);
            }
            cursorX += fonts.measureTextWidth(face, "[");
            ++p;
        } else {
            // Draw the rest of the string.
            if (draw) {
                comp.drawText(face, p, cursorX, baselineY, r, g, b);
            }
            cursorX += fonts.measureTextWidth(face, p);
            break;
        }
    }
    return cursorX - x;
}

// ---------------------------------------------------------------------------
// Text alignment helpers
// ---------------------------------------------------------------------------

void drawCenteredText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                      ps4::FontRasterizer::FontFace face, const char* text,
                      float boxX, float boxW, float baselineY,
                      uint8_t r, uint8_t g, uint8_t b) {
    const float textW = fonts.measureTextWidth(face, text);
    const float x = boxX + (boxW - textW) * 0.5f;
    comp.drawText(face, text, x, baselineY, r, g, b);
}

void drawRightAlignedText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                          ps4::FontRasterizer::FontFace face, const char* text,
                          float rightX, float baselineY,
                          uint8_t r, uint8_t g, uint8_t b) {
    const float textW = fonts.measureTextWidth(face, text);
    comp.drawText(face, text, rightX - textW, baselineY, r, g, b);
}

// ---------------------------------------------------------------------------
// Text truncation helper
// ---------------------------------------------------------------------------

std::string truncateTextWithEllipsis(ps4::FontRasterizer& fonts,
                                     ps4::FontRasterizer::FontFace face,
                                     const char* text, float maxW) {
    if (!text || maxW <= 0.0f) {
        return std::string();
    }
    // Fast path: the full text already fits; return it unchanged.
    if (fonts.measureTextWidth(face, text) <= maxW) {
        return text;
    }

    // The ASCII ellipsis suffix. Kept as a literal so the measurement below
    // matches exactly what is drawn.
    static const char kEllipsis[] = "...";
    const float ellipsisW = fonts.measureTextWidth(face, kEllipsis);

    // Trim trailing characters until the text plus the ellipsis fits. The
    // loop is O(n) measurements, but names are short and measurement uses
    // glyph metrics (no rasterization), so this is negligible.
    std::string s(text);
    while (!s.empty()) {
        // Pop full UTF-8 codepoint (continuation bytes then the lead byte).
        do {
            unsigned char b = static_cast<unsigned char>(s.back());
            s.pop_back();
            if ((b & 0xC0) != 0x80) {
                break;
            }
        } while (!s.empty());

        if (fonts.measureTextWidth(face, s.c_str()) + ellipsisW <= maxW) {
            return s + kEllipsis;
        }
    }
    // Even the ellipsis alone does not fit; return just the ellipsis.
    return kEllipsis;
}

} // namespace ui

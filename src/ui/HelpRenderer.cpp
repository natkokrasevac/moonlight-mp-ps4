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

// === HelpRenderer.cpp =====================================================
//
// OpenOrbis Moonlight client — CPU Help screen renderer.
//
// Implements the HelpRenderer: draws the Help screen (background + top bar +
// center panel with in-stream button assignments and chords + bottom legend)
// into the shared CPU UiCompositor surface using only fillRect / strokeRect /
// drawImageSubRect / drawText / clipping. It consumes a plain HelpRenderState
// snapshot and owns the Help layout geometry and the retained CPU-decoded UI
// atlas.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the retained background/atlas are held as owning
// ps4::CpuImage. The decoded CpuPngImage's buffer is adopted into the CpuImage
// member via adoptPng() (which transfers ownership of the malloc'd RGBA8
// buffer and detaches it from the CpuPngImage so its destructor does not free
// it). The CpuImage does not own the buffer; the renderer frees it
// deterministically in shutdown().
// =============================================================================

#include "HelpRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiAtlas.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush
#include <cstdlib>   // free

namespace ui {

// ---------------------------------------------------------------------------
// OpenOrbis helper: adopt a decoded CpuPngImage buffer into a CpuImage.
// Transfers ownership of the malloc'd RGBA8 buffer and detaches it from the
// CpuPngImage so its destructor does not free it. The CpuImage does not own
// the buffer; the renderer frees it in shutdown().
// ---------------------------------------------------------------------------

static void adoptPng(ps4::CpuPngImage& png, ps4::CpuImage& img) {
    img.width = png.width;
    img.height = png.height;
    img.pitch = png.pitch;
    img.rgba = png.rgba;
    img.rgbaSize = png.rgbaSize;
    // Detach from the CpuPngImage so it does not free the buffer.
    png.rgba = nullptr;
    png.rgbaSize = 0;
    png.width = 0;
    png.height = 0;
    png.pitch = 0;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

HelpRenderer::HelpRenderer() = default;

HelpRenderer::~HelpRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool HelpRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU UI atlas once. Failure is non-fatal: the legend
    // falls back to textual button markers.
    {
        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            printf("[help] ui_atlas.png decode failed (legend uses textual markers)\n");
            fflush(stdout);
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            printf("[help] moonlight_bg.png decode failed (screen uses solid background)\n");
            fflush(stdout);
        }
    }

    m_initialized = true;
    return true;
}

void HelpRenderer::shutdown() {
    m_fonts = nullptr;
    if (m_atlasImg.rgba) {
        free(m_atlasImg.rgba);
    }
    if (m_bgImg.rgba) {
        free(m_bgImg.rgba);
    }
    m_atlasImg = ps4::CpuImage();
    m_bgImg = ps4::CpuImage();
    m_initialized = false;
}

bool HelpRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect HelpRenderer::bounds() const {
    // The Help screen covers the full framebuffer.
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect HelpRenderer::panelRect() const {
    // A centered panel occupying the middle region of the screen, leaving room
    // for the top bar and bottom legend. The panel is tall enough to hold the
    // multi-section Help content block.
    const int panelW = 1200;
    const int panelH = 760;
    const int panelX = (1920 - panelW) / 2;
    const int panelY = 150;
    return ps4::Rect{panelX, panelY, panelW, panelH};
}

ps4::Rect HelpRenderer::panelBounds() const {
    // The panel bounds used for dirty regions. A few pixels of conservative
    // padding are included.
    const ps4::Rect p = panelRect();
    const int pad = 3;
    return ps4::Rect{p.x - pad, p.y - pad, p.w + pad * 2, p.h + pad * 2};
}

ps4::Rect HelpRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect HelpRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

bool HelpRenderer::rectsIntersect(const ps4::Rect& a, const ps4::Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void HelpRenderer::renderFull(const HelpRenderState& state,
                              ps4::UiCompositor& comp) {
    // Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    drawTopBar(comp);
    drawPanel(state, comp);
    drawSeparators(comp);
    drawLegend(state, comp);
}

void HelpRenderer::renderRegion(const HelpRenderState& state,
                                ps4::UiCompositor& comp,
                                const ps4::Rect& dirty) {
    if (!m_fonts) {
        return;
    }
    if (!comp.pushClip(dirty)) {
        return;   // Empty clip; nothing to recompose.
    }

    // Repaint the background intersecting the dirty region (clipped to it).
    drawBackground(comp, m_bgImg);

    // Top bar if it intersects.
    if (rectsIntersect(dirty, topBarBounds())) {
        drawTopBar(comp);
    }

    // Panel if it intersects.
    if (rectsIntersect(dirty, panelBounds())) {
        drawPanel(state, comp);
    }

    // Separator lines if they intersect.
    drawSeparators(comp);

    // Bottom legend if it intersects.
    if (rectsIntersect(dirty, bottomLegendBounds())) {
        drawLegend(state, comp);
    }

    comp.popClip();
}

// ---------------------------------------------------------------------------
// Panel drawing
// ---------------------------------------------------------------------------

void HelpRenderer::drawPanel(const HelpRenderState& state,
                             ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    const ps4::Rect panel = panelRect();

    // --- Content block ------------------------------------------------------
    // Each entry is a line with a font face and text, or a spacer (text ==
    // nullptr). The whole block is left-aligned and centered horizontally and
    // vertically within the panel.
    struct HelpLine {
        ps4::FontRasterizer::FontFace face;
        const char* text;   ///< nullptr for a spacer.
        float spacerH;      ///< Spacer height (used when text == nullptr).
        float indent;       ///< Horizontal indent (px) for this line.
    };

    const float kSpacerH = 24.0f;
    const float kIndent = 40.0f;
    const HelpLine kLines[] = {
        {ps4::FontRasterizer::FontFace::Bold, UiStrings::kHelpInStreamAssignments, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::BoldSmall, UiStrings::kHelpPsShareReserved, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, nullptr, kSpacerH, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpExitStream, 0.0f, kIndent},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpPsButton, 0.0f, kIndent},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpSelectStart, 0.0f, kIndent},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpMouseEmulation, 0.0f, kIndent},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpMouseButtons, 0.0f, kIndent},
        {ps4::FontRasterizer::FontFace::Regular, nullptr, kSpacerH, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpKeyboardMouse, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::BoldSmall, UiStrings::kHelpBluetoothNote, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, nullptr, kSpacerH, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpSettingsSaved, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, UiStrings::kHelpVsyncFramePacing, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, nullptr, kSpacerH, 0.0f},
        {ps4::FontRasterizer::FontFace::Bold, UiStrings::kHelpMoreDetails, 0.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::Regular, nullptr, 8.0f, 0.0f},
        {ps4::FontRasterizer::FontFace::RegularSmall, UiStrings::kHelpCopyright, 0.0f, 0.0f},
    };
    const size_t kLineCount = sizeof(kLines) / sizeof(kLines[0]);

    const float glyphSize = UiAtlas::kGlyphSize;

    // --- Measure the block: per-line height and glyph offsets. --------------
    // The block is NOT centered; it starts at the HostGrid focus box top-left
    // corner (row 0, column 0), which is the theme title X and the top bar
    // bottom + card-top offset.
    const float kStartX = UiTheme::kTitleTextX;
    const float kStartY = UiTheme::kTopBarHeight + UiTheme::kCardTopOffset;

    float lineBaselines[kLineCount];
    float lineHeights[kLineCount];
    float lineGlyphYs[kLineCount];

    for (size_t i = 0; i < kLineCount; ++i) {
        const HelpLine& line = kLines[i];
        if (line.text == nullptr) {
            // Spacer.
            lineHeights[i] = line.spacerH;
            lineBaselines[i] = 0.0f;
            lineGlyphYs[i] = 0.0f;
            continue;
        }

        float baselineY = 0.0f, lineHeight = 0.0f;
        m_fonts->getHorizontalLayout(line.face, baselineY, lineHeight);

        // Lines with inline glyphs need at least the glyph height.
        const bool hasGlyph = textHasInlineGlyph(line.text);
        const float lineH = hasGlyph ? (lineHeight > glyphSize ? lineHeight : glyphSize)
                                     : lineHeight;

        lineHeights[i] = lineH;
        lineBaselines[i] = baselineY;
        // Glyphs are vertically centered relative to the text line (the font's
        // line height), matching the bottom-legend glyph/text alignment. The
        // glyph extends above/below the text line; the larger lineH used for
        // spacing prevents overlap between consecutive glyph lines.
        lineGlyphYs[i] = (lineHeight - glyphSize) * 0.5f;
    }

    // --- Draw the block starting at the fixed origin. -----------------------
    float topY = kStartY;

    for (size_t i = 0; i < kLineCount; ++i) {
        const HelpLine& line = kLines[i];
        if (line.text == nullptr) {
            topY += lineHeights[i];
            continue;
        }

        const float textBaseline = topY + lineBaselines[i];
        const float glyphY = topY + lineGlyphYs[i];
        const float leftX = kStartX + line.indent;

        drawInlineGlyphText(comp, *m_fonts, m_atlasImg, line.face, line.text,
                            leftX, textBaseline, glyphY,
                            UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB, true);

        topY += lineHeights[i];
    }
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

void HelpRenderer::drawTopBar(ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Title "Help" in Title face, positioned at the theme title X and at the
    // theme title Y (relative to the top bar viewport top).
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Title,
                                 baselineY, lineHeight);
    const float topBarTop = 0.0f;   // top of screen
    const float titleBaseline = topBarTop + UiTheme::kTitleTextY;
    comp.drawText(ps4::FontRasterizer::FontFace::Title, UiStrings::kHelp,
                  UiTheme::kTitleTextX, titleBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Separator lines
// ---------------------------------------------------------------------------

void HelpRenderer::drawSeparators(ps4::UiCompositor& comp) {
    // Single-pixel separator line at the top bar bottom.
    comp.fillRect(0, static_cast<int>(UiTheme::kTopBarHeight), 1920, 1,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);
    // Single-pixel separator line at the bottom legend top.
    comp.fillRect(0, static_cast<int>(1080.0f - UiTheme::kBottomLegendHeight), 1920, 1,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void HelpRenderer::drawLegend(const HelpRenderState& state,
                              ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Legend entry: OK button + "Close".
    LegendEntry entries[1];
    entries[0] = {backIcon(state.crossIsAccept), UiStrings::kClose};

    drawBottomLegend(comp, *m_fonts, m_atlasImg, bottomLegendBounds(),
                     entries, 1);
}

} // namespace ui

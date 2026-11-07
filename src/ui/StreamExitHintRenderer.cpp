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

// === StreamExitHintRenderer.cpp ============================================
//
// OpenOrbis Moonlight client — CPU in-stream exit hint renderer.
//
// Implements the StreamExitHintRenderer: draws the short-lived in-stream exit
// hint overlay (rounded dark panel + Touchpad/R3 button glyphs + "Exit" text)
// into the shared CPU UiCompositor surface using only fillRect /
// drawImageSubRect / drawText. It consumes a plain StreamExitHintRenderState
// snapshot and owns the hint layout geometry and the retained CPU-decoded UI
// atlas.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the retained atlas is held as an owning ps4::CpuImage.
// The decoded CpuPngImage's buffer is adopted into the CpuImage member via
// adoptPng() (which transfers ownership of the malloc'd RGBA8 buffer and
// detaches it from the CpuPngImage so its destructor does not free it). The
// CpuImage does not own the buffer; the renderer frees it deterministically in
// shutdown(). This avoids the use-after-free that std::move(png.rgba) on a raw
// pointer previously caused.
// =============================================================================

#include "StreamExitHintRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiAtlas.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush
#include <cstdlib>   // free
#include <utility>   // std::move

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void hintDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

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
// Constants
// ---------------------------------------------------------------------------

/// Framebuffer dimensions (fixed PS4 output).
static constexpr int kFramebufferWidth = 1920;
static constexpr int kFramebufferHeight = 1080;

/// Hint panel geometry. Fixed size fits the four in-stream hint lines.
static constexpr int kPanelWidth = 760;
static constexpr int kPanelHeight = 320;
static constexpr int kBottomInset = 120;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

StreamExitHintRenderer::StreamExitHintRenderer() = default;

StreamExitHintRenderer::~StreamExitHintRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool StreamExitHintRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU UI atlas once. Failure is non-fatal: the hint
    // falls back to textual button markers (no glyphs).
    {
        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            hintDiagPrint("[hint] ui_atlas.png decode failed (hint uses textual markers)");
        }
    }

    m_initialized = true;
    return true;
}

void StreamExitHintRenderer::shutdown() {
    m_fonts = nullptr;
    if (m_atlasImg.rgba) {
        free(m_atlasImg.rgba);
    }
    m_atlasImg = ps4::CpuImage();
    m_initialized = false;
}

bool StreamExitHintRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect StreamExitHintRenderer::bounds() const {
    // The hint panel is centered horizontally and placed near the bottom of
    // the screen, above the bottom legend area. The panel height is fixed.
    const int panelX = (kFramebufferWidth - kPanelWidth) / 2;
    const int panelY = kFramebufferHeight - kBottomInset - kPanelHeight;
    return ps4::Rect{panelX, panelY, kPanelWidth, kPanelHeight};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void StreamExitHintRenderer::render(const StreamExitHintRenderState& state,
                                    ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    // The renderer draws only when the hint is visible. The caller (MoonlightApp)
    // invokes this only while the hint is active, so this is a defensive guard.
    if (!state.visible) {
        return;
    }

    const ps4::Rect panel = bounds();

    // --- Rounded dark panel background (semi-transparent). -----------------
    // The compositor surface is transparent; this fills only the panel region
    // with a dark translucent color so the decoded video shows through.
    comp.fillRect(panel.x, panel.y, panel.w, panel.h, 0, 0, 0, UiTheme::kDimAlpha);

    // --- Content block ------------------------------------------------------
    // Four left-aligned lines with inline glyphs, centered as a block within
    // the panel (consistent with the Help screen).
    const char* kLines[] = {
        UiStrings::kHelpExitStream,
        UiStrings::kHelpPsButton,
        UiStrings::kHelpSelectStart,
        UiStrings::kHelpMouseEmulation,
    };
    const size_t kLineCount = sizeof(kLines) / sizeof(kLines[0]);

    const float glyphSize = UiAtlas::kGlyphSize;

    // --- Measure the block: per-line height and glyph offsets. --------------
    float lineBaselines[kLineCount];
    float lineHeights[kLineCount];
    float lineGlyphYs[kLineCount];

    float blockH = 0.0f;
    float blockW = 0.0f;

    for (size_t i = 0; i < kLineCount; ++i) {
        float baselineY = 0.0f, lineHeight = 0.0f;
        m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                     baselineY, lineHeight);

        // Lines with inline glyphs need at least the glyph height.
        const bool hasGlyph = textHasInlineGlyph(kLines[i]);
        const float lineH = hasGlyph ? (lineHeight > glyphSize ? lineHeight : glyphSize)
                                     : lineHeight;

        lineHeights[i] = lineH;
        lineBaselines[i] = baselineY;
        // Glyphs are vertically centered relative to the text line (the font's
        // line height), matching the bottom-legend glyph/text alignment.
        lineGlyphYs[i] = (lineHeight - glyphSize) * 0.5f;

        const float w = drawInlineGlyphText(comp, *m_fonts, m_atlasImg,
                                            ps4::FontRasterizer::FontFace::Regular,
                                            kLines[i], 0.0f, 0.0f, 0.0f,
                                            0, 0, 0, false);
        if (w > blockW) {
            blockW = w;
        }
        blockH += lineH;
    }

    // --- Center the block horizontally and vertically in the panel. ---------
    const float leftX = panel.x + (panel.w - blockW) * 0.5f;
    float topY = panel.y + (panel.h - blockH) * 0.5f;

    // --- Draw the block. ----------------------------------------------------
    for (size_t i = 0; i < kLineCount; ++i) {
        const float textBaseline = topY + lineBaselines[i];
        const float glyphY = topY + lineGlyphYs[i];

        drawInlineGlyphText(comp, *m_fonts, m_atlasImg,
                            ps4::FontRasterizer::FontFace::Regular,
                            kLines[i], leftX, textBaseline, glyphY,
                            UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB, true);

        topY += lineHeights[i];
    }
}

} // namespace ui

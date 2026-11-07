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

// === MouseEmulationOverlayRenderer.cpp ======================================
//
// OpenOrbis Moonlight client — CPU in-stream mouse emulation state overlay
// renderer.
//
// Implements the MouseEmulationOverlayRenderer: draws the short-lived in-stream
// mouse emulation state overlay (semi-transparent dark panel + "Mouse
// Emulation: ON/OFF" caption + a "L:[Cross] R:[Circle]" mouse-button mapping
// line) into the shared CPU UiCompositor surface using only fillRect /
// drawImageSubRect / drawText. It consumes a plain MouseEmulationRenderState
// snapshot and owns the overlay layout geometry.
//
// This translation unit contains no OpenOrbis SDK calls.
// =============================================================================

#include "MouseEmulationOverlayRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiStrings.h"
#include "UiAtlas.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush, snprintf
#include <cstdlib>   // free
#include <utility>   // std::move

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void mouseOverlayDiagPrint(const char* msg) {
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

/// Overlay panel geometry. A compact panel placed near the top-center of the
/// screen, away from the top-left statistics panel and the bottom-center exit
/// hint, so the mouse emulation overlay can coexist with either. The panel is
/// sized to fit two lines: the "Mouse Emulation: ON/OFF" caption and the
/// "L:[Cross] R:[Circle]" mouse-button mapping line.
static constexpr int kPanelWidth = 520;
static constexpr int kPanelHeight = 150;
static constexpr int kTopInset = 88;

/// Vertical padding (px) between the panel edge and the content block.
static constexpr float kContentPadding = 20.0f;

/// Vertical gap (px) between the two content lines.
static constexpr float kLineSpacing = 12.0f;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

MouseEmulationOverlayRenderer::MouseEmulationOverlayRenderer() = default;

MouseEmulationOverlayRenderer::~MouseEmulationOverlayRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool MouseEmulationOverlayRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU UI atlas once. Failure is non-fatal: the overlay
    // falls back to textual button markers (no glyphs).
    {
        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            mouseOverlayDiagPrint("[mouse-overlay] ui_atlas.png decode failed (overlay uses textual markers)");
        }
    }

    m_initialized = true;
    return true;
}

void MouseEmulationOverlayRenderer::shutdown() {
    m_fonts = nullptr;
    if (m_atlasImg.rgba) {
        free(m_atlasImg.rgba);
    }
    m_atlasImg = ps4::CpuImage();
    m_initialized = false;
}

bool MouseEmulationOverlayRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect MouseEmulationOverlayRenderer::bounds() const {
    // The overlay panel is centered horizontally and placed near the top of
    // the screen, above the top-left statistics panel and clear of the
    // bottom-center exit hint.
    const int panelX = (kFramebufferWidth - kPanelWidth) / 2;
    const int panelY = kTopInset;
    return ps4::Rect{panelX, panelY, kPanelWidth, kPanelHeight};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void MouseEmulationOverlayRenderer::render(const MouseEmulationRenderState& state,
                                           ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    // The renderer draws only when the overlay is visible or a poor-connection
    // warning is active. The caller (MoonlightApp) invokes this only while the
    // overlay is active, so this is a defensive guard.
    if (!state.visible && !state.poorConnection) {
        return;
    }

    const ps4::Rect panel = bounds();

    // --- Semi-transparent dark panel background. ---------------------------
    // The compositor surface is transparent; this fills only the panel region
    // with a dark translucent color so the decoded video shows through.
    comp.fillRect(panel.x, panel.y, panel.w, panel.h, 0, 0, 0, UiTheme::kDimAlpha);

    // --- Font layout metrics. ----------------------------------------------
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);

    // When the transient mouse-emulation overlay is not shown and a poor
    // connection warning is active, render the poor-connection warning banner.
    if (!state.visible && state.poorConnection) {
        const float line1Baseline = panel.y + kContentPadding + lineHeight;
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Bold,
                         "Warning: Poor Connection", static_cast<float>(panel.x),
                         static_cast<float>(panel.w), line1Baseline,
                         UiTheme::kValueWarningR, UiTheme::kValueWarningG,
                         UiTheme::kValueWarningB);

        const float line2Baseline = line1Baseline + kLineSpacing + lineHeight;
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                         "Reduce bitrate or check network",
                         static_cast<float>(panel.x), static_cast<float>(panel.w),
                         line2Baseline,
                         UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
        return;
    }

    // --- Line 1: "Mouse Emulation: ON/OFF" caption. ------------------------
    const char* stateText = state.enabled ? UiStrings::kOn : UiStrings::kOff;
    char caption[64];
    snprintf(caption, sizeof(caption), "%s: %s",
             UiStrings::kMouseEmulation, stateText);

    const float line1Baseline = panel.y + kContentPadding + lineHeight;
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     caption, static_cast<float>(panel.x),
                     static_cast<float>(panel.w), line1Baseline,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // --- Line 2: "L:[Cross] R:[Circle]" mouse-button mapping. --------------
    // Drawn as a centered block: "L:" + Cross glyph + "R:" + Circle glyph.
    const float glyphSize = UiAtlas::kGlyphSize;
    const float glyphToLabelGap = UiAtlas::kGlyphToLabelGap;
    const float entryGap = 24.0f;

    const float lW = m_fonts->measureTextWidth(
        ps4::FontRasterizer::FontFace::Regular, "L:");
    const float rW = m_fonts->measureTextWidth(
        ps4::FontRasterizer::FontFace::Regular, "R:");

    // Block width: L: [glyph] R: [glyph].
    const float blockW = lW + glyphToLabelGap + glyphSize +
                         entryGap + rW + glyphToLabelGap + glyphSize;
    const float blockLeft = panel.x + (panel.w - blockW) * 0.5f;

    const float line2GlyphY = line1Baseline + kLineSpacing;
    const float line2Baseline = line2GlyphY + (glyphSize - lineHeight) * 0.5f +
                                baselineY;

    // "L:"
    float cursorX = blockLeft;
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, "L:",
                  cursorX, line2Baseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    cursorX += lW + glyphToLabelGap;

    // Cross glyph (left mouse button).
    drawAtlasIcon(comp, m_atlasImg, UiAtlasIcon::Cross,
                  ps4::Rect{static_cast<int>(cursorX), static_cast<int>(line2GlyphY),
                            static_cast<int>(glyphSize), static_cast<int>(glyphSize)});
    cursorX += glyphSize + entryGap;

    // "R:"
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, "R:",
                  cursorX, line2Baseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    cursorX += rW + glyphToLabelGap;

    // Circle glyph (right mouse button).
    drawAtlasIcon(comp, m_atlasImg, UiAtlasIcon::Circle,
                  ps4::Rect{static_cast<int>(cursorX), static_cast<int>(line2GlyphY),
                            static_cast<int>(glyphSize), static_cast<int>(glyphSize)});
}

} // namespace ui

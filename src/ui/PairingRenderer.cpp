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

// === PairingRenderer.cpp ===================================================
//
// OpenOrbis Moonlight client — CPU Pairing screen renderer.
//
// Implements the PairingRenderer: draws the Pairing screen (background +
// center panel + host name + pairing instructions + PIN display + status
// message + bottom legend) into the shared CPU UiCompositor surface using only
// fillRect / strokeRect / drawImageSubRect / drawText / clipping. It consumes
// a plain PairingRenderState snapshot and owns the Pairing layout geometry and
// the retained CPU-decoded UI atlas.
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

#include "PairingRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiAtlas.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush, snprintf
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

PairingRenderer::PairingRenderer() = default;

PairingRenderer::~PairingRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool PairingRenderer::initialize(ps4::FontRasterizer* fonts) {
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
            printf("[pairing] ui_atlas.png decode failed (legend uses textual markers)\n");
            fflush(stdout);
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            printf("[pairing] moonlight_bg.png decode failed (screen uses solid background)\n");
            fflush(stdout);
        }
    }

    m_initialized = true;
    return true;
}

void PairingRenderer::shutdown() {
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

bool PairingRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect PairingRenderer::bounds() const {
    // The Pairing screen covers the full framebuffer.
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect PairingRenderer::panelRect() const {
    // A centered panel occupying the middle region of the screen, leaving room
    // for the bottom legend (title + host name + instructions + PIN + status).
    const int panelW = 1200;
    const int panelH = 560;
    const int panelX = (1920 - panelW) / 2;
    const int panelY = 160;
    return ps4::Rect{panelX, panelY, panelW, panelH};
}

ps4::Rect PairingRenderer::panelBounds() const {
    // The panel bounds used for status-change dirty regions. A few pixels of
    // conservative padding are included.
    const ps4::Rect p = panelRect();
    const int pad = 3;
    return ps4::Rect{p.x - pad, p.y - pad, p.w + pad * 2, p.h + pad * 2};
}

ps4::Rect PairingRenderer::pinRect() const {
    // The PIN display sits centered within the panel, below the host name and
    // instructions. A generous region is used so a PIN change dirties the
    // whole PIN area (the PIN is the primary visual element).
    const ps4::Rect p = panelRect();
    const int pinW = p.w - 200;
    const int pinH = 140;
    const int pinX = p.x + (p.w - pinW) / 2;
    const int pinY = p.y + 260;
    return ps4::Rect{pinX, pinY, pinW, pinH};
}

ps4::Rect PairingRenderer::pinBounds() const {
    // The PIN bounds used for PIN-change dirty regions. A few pixels of
    // conservative padding are included.
    const ps4::Rect p = pinRect();
    const int pad = 3;
    return ps4::Rect{p.x - pad, p.y - pad, p.w + pad * 2, p.h + pad * 2};
}

ps4::Rect PairingRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect PairingRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void PairingRenderer::renderFull(const PairingRenderState& state,
                                 ps4::UiCompositor& comp) {
    // Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    drawTopBar(comp);
    drawPanel(state, comp);
    drawSeparators(comp);
    drawLegend(state, comp);
}

// ---------------------------------------------------------------------------
// Panel drawing
// ---------------------------------------------------------------------------

void PairingRenderer::drawPanel(const PairingRenderState& state,
                                ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    const ps4::Rect panel = panelRect();
    const ps4::Rect pin = pinRect();

    float baselineY = 0.0f, lineHeight = 0.0f;

    // --- Host name (Title), centered horizontally with the PIN. ------------
    if (!state.hostName.empty()) {
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Title,
                         state.hostName.c_str(), static_cast<float>(pin.x),
                         static_cast<float>(pin.w), panel.y + 130.0f + baselineY,
                         UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // --- Pairing instructions + PIN display (only when the PIN is ready). --
    // The instruction and PIN stay on screen for the duration of the pairing
    // operation once the PIN is available.
    if (!state.pin.empty()) {
        m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                     baselineY, lineHeight);
        // "Enter PIN on host" centered horizontally with the PIN.
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Bold,
                         "Enter PIN on host", static_cast<float>(pin.x),
                         static_cast<float>(pin.w), panel.y + 200.0f + baselineY,
                         UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);

        // PIN display (Bold, large), centered in the panel.
        const float pinBaseline = pin.y + (pin.h - lineHeight) * 0.5f + baselineY;
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Boot,
                         state.pin.c_str(), static_cast<float>(pin.x),
                         static_cast<float>(pin.w), pinBaseline,
                         UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB);
    }

    // --- Status message (Regular), below the PIN. --------------------------
    if (!state.message.empty()) {
        // Status message centered horizontally with the PIN.
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Bold,
                         state.message.c_str(), static_cast<float>(pin.x),
                         static_cast<float>(pin.w), panel.y + 460.0f + baselineY,
                         UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);
    }
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

void PairingRenderer::drawTopBar(ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Title "Moonlight PS4" in Title face, positioned at the theme title X and
    // at the theme title Y (relative to the top bar viewport top). Matches the
    // HostList top bar design.
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Title,
                                 baselineY, lineHeight);
    const float topBarTop = 0.0f;   // top of screen
    const float titleBaseline = topBarTop + UiTheme::kTitleTextY;
    comp.drawText(ps4::FontRasterizer::FontFace::Title, "Pair Host",
                  UiTheme::kTitleTextX, titleBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Separator lines
// ---------------------------------------------------------------------------

void PairingRenderer::drawSeparators(ps4::UiCompositor& comp) {
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

void PairingRenderer::drawLegend(const PairingRenderState& state,
                                 ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Legend entry: back button + "Cancel".
    LegendEntry entries[1];
    entries[0] = {backIcon(state.crossIsAccept), UiStrings::kCancel};

    drawBottomLegend(comp, *m_fonts, m_atlasImg, bottomLegendBounds(),
                     entries, 1);
}

} // namespace ui

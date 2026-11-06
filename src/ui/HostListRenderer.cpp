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

// === HostListRenderer.cpp ===================================================
//
// Moonlight client for PlayStation 4 — CPU Host List renderer.
//
// Implements the HostListRenderer: draws the Host List screen into the
// experimental CPU UiCompositor surface using only fillRect / strokeRect /
// drawImage / drawImageSubRect / drawText / clipping. It consumes a plain
// HostListRenderState snapshot and owns the Host List layout geometry and the
// retained CPU-decoded resources.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the SCE reference stores image pixels in
// std::vector<uint8_t> and tests emptiness with `.empty()`. The OpenOrbis
// CpuImage holds a raw pointer + byte size (to avoid the toolchain libc++
// <vector> include issue), so emptiness is tested with `rgba == nullptr`.
// The decoded CpuPngImage's buffer is adopted into the CpuImage member and
// freed deterministically in shutdown().
// =============================================================================

#include "HostListRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiAtlas.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush, snprintf
#include <cstring>   // memset
#include <cstdlib>   // free
#include <utility>   // std::move

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void hostListDiagPrint(const char* msg) {
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
// Construction / Destruction
// ---------------------------------------------------------------------------

HostListRenderer::HostListRenderer() = default;

HostListRenderer::~HostListRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool HostListRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU resources once. Failure to decode one image is
    // non-fatal: the cards render without artwork / the legend falls back to
    // textual markers.
    {
        ps4::CpuPngImage hostPng;
        if (ps4::decodePngToCpu("/app0/assets/images/host.png", hostPng)) {
            adoptPng(hostPng, m_hostImg);
        } else {
            hostListDiagPrint("[host-list] host.png decode failed (cards render without artwork)");
        }

        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            hostListDiagPrint("[host-list] ui_atlas.png decode failed (legend uses textual markers)");
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            hostListDiagPrint("[host-list] moonlight_bg.png decode failed (screen uses solid background)");
        }

        ps4::CpuPngImage ctrlPng;
        if (ps4::decodePngToCpu("/app0/assets/images/controller.png", ctrlPng)) {
            adoptPng(ctrlPng, m_controllerImg);
        }
    }

    m_initialized = true;
    return true;
}

void HostListRenderer::shutdown() {
    m_fonts = nullptr;
    if (m_hostImg.rgba) {
        free(m_hostImg.rgba);
    }
    if (m_atlasImg.rgba) {
        free(m_atlasImg.rgba);
    }
    if (m_bgImg.rgba) {
        free(m_bgImg.rgba);
    }
    if (m_controllerImg.rgba) {
        free(m_controllerImg.rgba);
    }
    m_controllerImg = ps4::CpuImage();
    m_hostImg = ps4::CpuImage();
    m_atlasImg = ps4::CpuImage();
    m_bgImg = ps4::CpuImage();
    m_initialized = false;
}

bool HostListRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

HostListRenderer::Viewport HostListRenderer::crossbarViewport() const {
    // The crossbar viewport spans the full screen width and sits between the
    // top title bar and the bottom legend bar. No safe-area margins are used:
    // HDMI output to a modern flat panel renders edge-to-edge.
    const float screenX = 0.0f;
    const float screenY = UiTheme::kTopBarHeight;
    const float screenW = 1920.0f;
    const float screenH = 1080.0f - UiTheme::kTopBarHeight - UiTheme::kBottomLegendHeight;

    Viewport v;
    v.x = screenX;
    v.y = screenY;
    v.w = screenW;
    v.h = screenH;
    return v;
}

float HostListRenderer::crossbarCardLeft(int index, int selectedIndex,
                                         float focusLeft) const {
    if (index == selectedIndex) {
        return focusLeft;
    }
    if (index > selectedIndex) {
        // Walk right from the focus card.
        float left = focusLeft;
        for (int j = selectedIndex; j < index; ++j) {
            const float w = (j == selectedIndex) ? cardWidth()
                                                 : cardWidth() * nonFocusScale();
            left += w + spacing();
        }
        return left;
    }
    // Walk left from the focus card.
    const float step = cardWidth() * nonFocusScale() + spacing();
    return focusLeft - (selectedIndex - index) * step;
}

void HostListRenderer::computeCardRect(int index, int selectedIndex,
                                       float& left, float& top,
                                       float& w, float& h) const {
    const Viewport v = crossbarViewport();
    const float focusLeft = v.x + v.w * 0.15f;
    // The card row top is fixed at the theme card-top offset below the top bar
    // bottom, so cards can never overlap the top bar.
    const float rowTop = UiTheme::kTopBarHeight + UiTheme::kCardTopOffset;

    const bool isSelected = (index == selectedIndex);
    const float scale = isSelected ? 1.0f : nonFocusScale();
    left = crossbarCardLeft(index, selectedIndex, focusLeft);
    top = rowTop;
    w = cardWidth() * scale;
    h = cardHeight() * scale;
}

ps4::Rect HostListRenderer::cardVisualBounds(int index, int selectedIndex) const {
    float left, top, w, h;
    computeCardRect(index, selectedIndex, left, top, w, h);
    const bool isSelected = (index == selectedIndex);
    // Include the focused card's footer.
    const float bottom = top + h + (isSelected ? footerHeight() : 0.0f);
    // Conservative padding (a few px) so focus borders / AA are fully covered.
    const int pad = 3;
    return ps4::Rect{static_cast<int>(left) - pad,
                     static_cast<int>(top) - pad,
                     static_cast<int>(w) + pad * 2,
                     static_cast<int>(bottom - top) + pad * 2};
}

ps4::Rect HostListRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

ps4::Rect HostListRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect HostListRenderer::cardAreaBounds() const {
    const Viewport v = crossbarViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void HostListRenderer::renderFull(const HostListRenderState& state,
                                  ps4::UiCompositor& comp) {
    // Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    drawTopBar(state, comp);
    drawSeparators(comp);

    for (int i = 0; i < static_cast<int>(state.cards.size()); ++i) {
        drawCard(state.cards[i], i, state.selectedIndex, comp);
    }

    drawLegend(state, comp);
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

void HostListRenderer::drawTopBar(const HostListRenderState& state, ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Title "Hosts" in Title face, positioned at the theme title X and at the
    // theme title Y (relative to the top bar viewport top).
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Title,
                                 baselineY, lineHeight);
    const float topBarTop = 0.0f;   // top of screen
    const float titleBaseline = topBarTop + UiTheme::kTitleTextY;
    comp.drawText(ps4::FontRasterizer::FontFace::Title, "Hosts",
                  UiTheme::kTitleTextX, titleBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    drawControllerIndicators(comp, m_controllerImg, state.connectedControllersMask);
}

// ---------------------------------------------------------------------------
// Separator lines
// ---------------------------------------------------------------------------

void HostListRenderer::drawSeparators(ps4::UiCompositor& comp) {
    // Single-pixel separator line at the top bar bottom.
    comp.fillRect(0, static_cast<int>(UiTheme::kTopBarHeight), 1920, 1,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);
    // Single-pixel separator line at the bottom legend top.
    comp.fillRect(0, static_cast<int>(1080.0f - UiTheme::kBottomLegendHeight), 1920, 1,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);
}

// ---------------------------------------------------------------------------
// Card drawing
// ---------------------------------------------------------------------------

void HostListRenderer::drawCard(const HostListCardRenderData& card, int index,
                                int selectedIndex, ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    float left, top, w, h;
    computeCardRect(index, selectedIndex, left, top, w, h);
    const bool isSelected = (index == selectedIndex);
    const float scale = isSelected ? 1.0f : nonFocusScale();

    const int ix = static_cast<int>(left);
    const int iy = static_cast<int>(top);
    const int iw = static_cast<int>(w);
    const int ih = static_cast<int>(h);

    // Artwork rectangle (scaled with the card).
    const float artMargin = UiTheme::kArtMargin * scale;
    const float artHeight = UiTheme::kArtHeight * scale;
    const int artX = static_cast<int>(left + artMargin);
    const int artY = static_cast<int>(top + artMargin);
    const int artW = static_cast<int>(w - artMargin * 2.0f);
    const int artH = static_cast<int>(artHeight);
    drawAspectFitImage(comp, m_hostImg, artX, artY, artW, artH);

    // Text content.
    const float fontSize = baseFontSize() * scale;
    const float lineH = fontSize;

    // Display name at the bottom of the card, with a baseline margin from the
    // card bottom. Focused cards use Regular; non-focused cards use
    // RegularSmall.
    float baselineY = 0.0f, lineHeight = 0.0f;
    const ps4::FontRasterizer::FontFace nameFace =
        isSelected ? ps4::FontRasterizer::FontFace::Regular
                   : ps4::FontRasterizer::FontFace::RegularSmall;
    m_fonts->getHorizontalLayout(nameFace, baselineY, lineHeight);
    const float nameBaseline = top + h - 16.0f;
    // Truncate the display name with an ellipsis so it never overflows the
    // card width + margins (8px inset on each side).
    const std::string name = truncateTextWithEllipsis(
        *m_fonts, nameFace, card.displayName.c_str(), w - 16.0f);
    comp.drawText(nameFace, name.c_str(),
                  left + 8.0f, nameBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Paired status + availability at the theme status offsets, scaled with
    // the card. Focused cards use Regular; non-focused cards use RegularSmall.
    const ps4::FontRasterizer::FontFace statusFace =
        isSelected ? ps4::FontRasterizer::FontFace::Regular
                   : ps4::FontRasterizer::FontFace::RegularSmall;
    m_fonts->getHorizontalLayout(statusFace, baselineY, lineHeight);
    const float statusX = left + 68.0f * scale;
    const float statusY = top + 90.0f * scale;

    const char* pairedText = "Unknown";
    switch (card.pairingState) {
    case ml::HostPairingState::Unpaired: pairedText = "Unpaired"; break;
    case ml::HostPairingState::Paired:   pairedText = "Paired";   break;
    case ml::HostPairingState::Unknown:
    default:                             pairedText = "Unknown";  break;
    }
    comp.drawText(statusFace, pairedText,
                  statusX, statusY + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    const char* statusText = "Unknown";
    switch (card.availability) {
    case ml::HostAvailability::Online:        statusText = "Online";        break;
    case ml::HostAvailability::Stale:         statusText = "Stale";         break;
    case ml::HostAvailability::Offline:       statusText = "Offline";       break;
    case ml::HostAvailability::ManuallyAdded: statusText = "Manual";        break;
    }
    comp.drawText(statusFace, statusText,
                  statusX, statusY + lineH + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // App Running indicator (only when positively reachable and running).
    if (card.appRunning) {
        comp.drawText(statusFace, "Running",
                      statusX, statusY + 2.0f * lineH + baselineY,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // Card outline (focus treatment) drawn LAST so it is the top-most item and
    // is never covered by the artwork or text content.
    drawCardOutline(comp, ix, iy, iw, ih, isSelected);

    // Focus-card footer (Connect). Only the focused card gets a footer.
    if (isSelected) {
        drawFocusFooter(comp, *m_fonts, left, top, w, h, footerHeight(),
                        UiStrings::kConnect, baselineY, lineH);
    }
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void HostListRenderer::drawLegend(const HostListRenderState& state,
                                  ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Legend entries: Accept, Delete Host, Add Host Manually, Settings.
    LegendEntry entries[4];
    int entryCount = 0;
    entries[entryCount++] = {acceptIcon(state.crossIsAccept), UiStrings::kAccept};
    entries[entryCount++] = {UiAtlasIcon::Triangle, UiStrings::kDeleteHost};
    entries[entryCount++] = {UiAtlasIcon::Square, UiStrings::kAddHostManually};
    entries[entryCount++] = {UiAtlasIcon::Options, UiStrings::kSettings};

    drawBottomLegend(comp, *m_fonts, m_atlasImg, bottomLegendBounds(),
                     entries, static_cast<size_t>(entryCount));

    // Right-aligned Help entry (Touchpad glyph + "Help") on the right side of
    // the legend bar.
    drawBottomLegendRightAligned(comp, *m_fonts, m_atlasImg,
                                 bottomLegendBounds(), UiAtlasIcon::Touchpad,
                                 UiStrings::kHelp);
}

} // namespace ui

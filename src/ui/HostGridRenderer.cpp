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

// === HostGridRenderer.cpp ===================================================
//
// Moonlight client for PlayStation 4 — CPU Host List Grid renderer.
//
// Implements the HostGridRenderer: draws the Host List Grid presentation mode
// into the experimental CPU UiCompositor surface using only fillRect /
// strokeRect / drawImage / drawImageSubRect / drawText / clipping. It consumes
// a plain HostGridRenderState snapshot and owns the grid card geometry, grid
// clipping, grid visual layout, and card rendering.
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

#include "HostGridRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiAtlas.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush, snprintf
#include <cstring>   // memset
#include <cstdlib>   // free
#include <cmath>     // ceilf, floorf
#include <utility>   // std::move

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void hostGridDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// OpenOrbis helper: adopt a decoded CpuPngImage buffer into a CpuImage.
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

HostGridRenderer::HostGridRenderer() = default;

HostGridRenderer::~HostGridRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool HostGridRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU resources once. Failure to decode one image is
    // non-fatal: the cards render without artwork / the legend falls back to
    // textual markers. Hosts share /app0/assets/images/host.png (the existing
    // CPU image path); no new PNG loading path is created.
    {
        ps4::CpuPngImage hostPng;
        if (ps4::decodePngToCpu("/app0/assets/images/host.png", hostPng)) {
            adoptPng(hostPng, m_hostImg);
        } else {
            hostGridDiagPrint("[host-grid] host.png decode failed (cards render without artwork)");
        }

        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            hostGridDiagPrint("[host-grid] ui_atlas.png decode failed (legend uses textual markers)");
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            hostGridDiagPrint("[host-grid] moonlight_bg.png decode failed (screen uses solid background)");
        }

        ps4::CpuPngImage ctrlPng;
        if (ps4::decodePngToCpu("/app0/assets/images/controller.png", ctrlPng)) {
            adoptPng(ctrlPng, m_controllerImg);
        }
    }

    m_initialized = true;
    return true;
}

void HostGridRenderer::shutdown() {
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

bool HostGridRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

HostGridRenderer::Viewport HostGridRenderer::gridViewport() const {
    // The grid viewport spans the screen from the theme card-left X to the
    // right edge, and between the top bar and the bottom legend with 48px margin.
    const float screenX = UiTheme::kTitleTextX;
    const float screenY = UiTheme::kTopBarHeight + 48.0f;
    const float screenW = 1920.0f - UiTheme::kTitleTextX;
    const float screenH = 1080.0f - UiTheme::kTopBarHeight -
                          UiTheme::kBottomLegendHeight - 48.0f;

    Viewport v;
    v.x = screenX;
    v.y = screenY;
    v.w = screenW;
    v.h = screenH;
    return v;
}

HostGridRenderer::GridLayout HostGridRenderer::makeLayout(const Viewport& v) const {
    GridLayout layout;
    layout.columnPitch = cardWidth() + spacingX();
    layout.rowPitch = cardHeight() + spacingY();

    // Column count from available width.
    layout.columns =
        static_cast<int>((v.w + spacingX()) / layout.columnPitch);
    if (layout.columns < 1) {
        layout.columns = 1;
    }
    return layout;
}

int HostGridRenderer::columns() const {
    const Viewport v = gridViewport();
    return makeLayout(v).columns;
}

float HostGridRenderer::scrollOffsetFor(const HostGridRenderState& state) const {
    const Viewport v = gridViewport();
    const GridLayout layout = makeLayout(v);

    // Compute the deterministic scroll offset that keeps the selected row
    // fully visible. This is a presentation-only projection owned by the
    // renderer (it owns the grid geometry). UiManager remains authoritative
    // for selection/navigation.
    float scrollOffset = 0.0f;
    if (state.selectedIndex >= 0 && layout.rowPitch > 0.0f) {
        const int selectedRow = state.selectedIndex / layout.columns;
        // First fully-visible row: the smallest row whose top edge is at or
        // below the viewport top (not clipped at the top).
        const int firstFullyVisibleRow =
            static_cast<int>(ceilf(scrollOffset / layout.rowPitch));
        // Last fully-visible row: the largest row whose bottom edge is at or
        // above the viewport bottom (not clipped at the bottom).
        const int lastFullyVisibleRow =
            static_cast<int>(floorf((scrollOffset + v.h) / layout.rowPitch)) - 1;
        if (selectedRow < firstFullyVisibleRow) {
            scrollOffset = selectedRow * layout.rowPitch;
        } else if (selectedRow > lastFullyVisibleRow) {
            scrollOffset =
                (selectedRow + 1) * layout.rowPitch - v.h;
            if (scrollOffset < 0.0f) {
                scrollOffset = 0.0f;
            }
        }
    }
    return scrollOffset;
}

void HostGridRenderer::cardPosition(int index, const GridLayout& layout,
                                    const Viewport& v, float scrollOffset,
                                    float& left, float& top) const {
    const int row = index / layout.columns;
    const int col = index % layout.columns;
    // The grid content origin is the viewport top-left. The scroll offset
    // shifts the content upward, so a card's screen position is the origin
    // plus its grid offset minus the scroll.
    left = v.x + col * layout.columnPitch;
    top = v.y + row * layout.rowPitch - scrollOffset;
}

ps4::Rect HostGridRenderer::cardVisualBounds(int index,
                                             const HostGridRenderState& state) const {
    const Viewport v = gridViewport();
    const GridLayout layout = makeLayout(v);
    float left, top;
    cardPosition(index, layout, v, scrollOffsetFor(state), left, top);
    // Conservative padding (a few px) so focus borders / AA are fully covered.
    const int pad = 3;
    return ps4::Rect{static_cast<int>(left) - pad,
                     static_cast<int>(top) - pad,
                     static_cast<int>(cardWidth()) + pad * 2,
                     static_cast<int>(cardHeight()) + pad * 2};
}

ps4::Rect HostGridRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

ps4::Rect HostGridRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect HostGridRenderer::cardAreaBounds() const {
    const Viewport v = gridViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

ps4::Rect HostGridRenderer::gridViewportBounds() const {
    const Viewport v = gridViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void HostGridRenderer::renderFull(const HostGridRenderState& state,
                                  ps4::UiCompositor& comp) {
    // Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    // The grid is clipped to the scrollable grid viewport.
    const ps4::Rect gv = gridViewportBounds();
    if (comp.pushClip(gv)) {
        for (int i = 0; i < static_cast<int>(state.hosts.size()); ++i) {
            drawCard(state.hosts[i], i, state, comp);
        }
        comp.popClip();
    }

    // Top bar and bottom legend are drawn after the card grid, ensuring that
    // any cards scrolling behind them are completely occluded by the bar backing.
    drawTopBar(state, comp);
    drawLegend(state, comp);
    drawSeparators(comp);
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

void HostGridRenderer::drawTopBar(const HostGridRenderState& state, ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Restore the background slice for top bar so cards scrolling behind are completely hidden
    if (m_bgImg.rgba != nullptr) {
        comp.drawImageSubRect(m_bgImg, 0, 0, 1920, static_cast<int>(UiTheme::kTopBarHeight),
                              0, 0, 1920, static_cast<int>(UiTheme::kTopBarHeight));
    } else {
        comp.fillRect(0, 0, 1920, static_cast<int>(UiTheme::kTopBarHeight),
                      UiTheme::kBackgroundR, UiTheme::kBackgroundG, UiTheme::kBackgroundB, 255);
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

void HostGridRenderer::drawSeparators(ps4::UiCompositor& comp) {
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

void HostGridRenderer::drawCard(const HostGridCardRenderState& card, int index,
                                const HostGridRenderState& state,
                                ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    const Viewport v = gridViewport();
    const GridLayout layout = makeLayout(v);
    float left, top;
    cardPosition(index, layout, v, scrollOffsetFor(state), left, top);

    const bool isSelected = card.selected;
    const int ix = static_cast<int>(left);
    const int iy = static_cast<int>(top);
    const int iw = static_cast<int>(cardWidth());
    const int ih = static_cast<int>(cardHeight());

    // Solid dark card fill so every card has a consistent solid base
    comp.fillRect(ix, iy, iw, ih, 20, 26, 38, 255);

    // Artwork rectangle
    const float artMargin = UiTheme::kArtMargin;
    const int artX = static_cast<int>(left + artMargin);
    const int artY = static_cast<int>(top + artMargin);
    const int artW = static_cast<int>(cardWidth() - artMargin * 2.0f);
    const int artH = static_cast<int>(cardHeight() - artMargin * 2.0f);
    drawAspectCoverImage(comp, m_hostImg, artX, artY, artW, artH);

    // Translucent dark bottom scrim for display name readability
    comp.fillRect(ix, static_cast<int>(top + cardHeight() - 65.0f), iw, 65, 0, 0, 0, 190);

    // Text content.
    const float lineH = baseFontSize();

    // Display name (Regular) at the bottom of the card, with a baseline margin
    // from the card bottom.
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float nameBaseline = top + cardHeight() - 16.0f;
    // Truncate the host name with an ellipsis so it never overflows the card
    // width + margins (8px inset on each side).
    const std::string name = truncateTextWithEllipsis(
        *m_fonts, ps4::FontRasterizer::FontFace::Regular, card.name.c_str(),
        cardWidth() - 16.0f);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, name.c_str(),
                  left + 8.0f, nameBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Paired status + availability (Regular) at the theme status offsets.
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float statusX = left + 68.0f;
    const float statusY = top + 90.0f;

    const char* pairedText = card.paired ? "Paired" : "Unpaired";
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, pairedText,
                  statusX, statusY + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    const char* statusText = "Unknown";
    switch (card.availability) {
    case HostGridAvailability::Online:        statusText = "Online";  break;
    case HostGridAvailability::Stale:         statusText = "Stale";   break;
    case HostGridAvailability::Offline:       statusText = "Offline"; break;
    case HostGridAvailability::ManuallyAdded: statusText = "Manual";  break;
    }
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, statusText,
                  statusX, statusY + lineH + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // App Running indicator (only when positively reachable and running).
    if (card.appRunning) {
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, "Running",
                      statusX, statusY + 2.0f * lineH + baselineY,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // Card outline (focus treatment) drawn LAST so it is the top-most item and
    // is never covered by the artwork or text content.
    drawCardOutline(comp, ix, iy, iw, ih, isSelected);
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void HostGridRenderer::drawLegend(const HostGridRenderState& state,
                                  ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Restore the background slice for the bottom bar so cards scrolling behind are completely hidden
    const int by = static_cast<int>(1080.0f - UiTheme::kBottomLegendHeight);
    const int bh = static_cast<int>(UiTheme::kBottomLegendHeight);
    if (m_bgImg.rgba != nullptr) {
        comp.drawImageSubRect(m_bgImg, 0, by, 1920, bh, 0, by, 1920, bh);
    } else {
        comp.fillRect(0, by, 1920, bh,
                      UiTheme::kBackgroundR, UiTheme::kBackgroundG, UiTheme::kBackgroundB, 255);
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

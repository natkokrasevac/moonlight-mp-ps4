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

// === AppGridRenderer.cpp ===================================================
//
// OpenOrbis Moonlight client — CPU App List Grid renderer.
//
// Implements the AppGridRenderer: draws the App List Grid presentation mode
// into the CPU UiCompositor surface using only fillRect / strokeRect /
// drawImage / drawImageSubRect / drawText / clipping. It consumes a plain
// AppGridRenderState snapshot and owns the grid card geometry, grid clipping,
// grid visual layout, and card rendering.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the retained app/atlas/background are held as owning
// ps4::CpuImage. The decoded CpuPngImage's buffer is adopted into the CpuImage
// member via adoptPng() (which transfers ownership of the malloc'd RGBA8
// buffer and detaches it from the CpuPngImage so its destructor does not free
// it). The CpuImage does not own the buffer; the renderer frees it
// deterministically in shutdown(). This avoids the use-after-free that
// std::move(png.rgba) on a raw pointer previously caused.
// =============================================================================

#include "AppGridRenderer.h"

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

static void appGridDiagPrint(const char* msg) {
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

AppGridRenderer::AppGridRenderer() = default;

AppGridRenderer::~AppGridRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool AppGridRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU resources once. Failure to decode one image is
    // non-fatal: the cards render without artwork / the legend falls back to
    // textual markers. Apps use the fallback artwork (app_fallback.png); the
    // structure supports per-app artwork later without an architecture change.
    {
        ps4::CpuPngImage appPng;
        if (ps4::decodePngToCpu("/app0/assets/images/app_fallback.png", appPng)) {
            adoptPng(appPng, m_appImg);
        } else {
            appGridDiagPrint("[app-grid] app_fallback.png decode failed (cards render without artwork)");
        }

        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            appGridDiagPrint("[app-grid] ui_atlas.png decode failed (legend uses textual markers)");
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            appGridDiagPrint("[app-grid] moonlight_bg.png decode failed (screen uses solid background)");
        }

        ps4::CpuPngImage ctrlPng;
        if (ps4::decodePngToCpu("/app0/assets/images/controller.png", ctrlPng)) {
            adoptPng(ctrlPng, m_controllerImg);
        }
    }

    m_initialized = true;
    return true;
}

void AppGridRenderer::shutdown() {
    m_fonts = nullptr;
    if (m_appImg.rgba) {
        free(m_appImg.rgba);
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
    m_appImg = ps4::CpuImage();
    m_atlasImg = ps4::CpuImage();
    m_bgImg = ps4::CpuImage();
    m_initialized = false;
}

bool AppGridRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

AppGridRenderer::Viewport AppGridRenderer::gridViewport() const {
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

AppGridRenderer::GridLayout AppGridRenderer::makeLayout(const Viewport& v) const {
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

int AppGridRenderer::columns() const {
    const Viewport v = gridViewport();
    return makeLayout(v).columns;
}

float AppGridRenderer::scrollOffsetFor(const AppGridRenderState& state) const {
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

void AppGridRenderer::cardPosition(int index, const GridLayout& layout,
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

ps4::Rect AppGridRenderer::cardVisualBounds(int index,
                                            const AppGridRenderState& state) const {
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

ps4::Rect AppGridRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

ps4::Rect AppGridRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect AppGridRenderer::cardAreaBounds() const {
    const Viewport v = gridViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

ps4::Rect AppGridRenderer::gridViewportBounds() const {
    const Viewport v = gridViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void AppGridRenderer::renderFull(const AppGridRenderState& state,
                                 ps4::UiCompositor& comp) {
    // Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    // The grid is clipped to the scrollable grid viewport.
    const ps4::Rect gv = gridViewportBounds();
    if (comp.pushClip(gv)) {
        for (int i = 0; i < static_cast<int>(state.cards.size()); ++i) {
            drawCard(state.cards[i], i, state, comp);
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

void AppGridRenderer::drawTopBar(const AppGridRenderState& state,
                                 ps4::UiCompositor& comp) {
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

    // Title "Applications <host>" in Title face, positioned at the theme title
    // X and at the theme title Y (relative to the top bar viewport top).
    char titleBuf[256];
    if (!state.hostName.empty()) {
        snprintf(titleBuf, sizeof(titleBuf), "Applications %s",
                 state.hostName.c_str());
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Applications");
    }

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Title,
                                 baselineY, lineHeight);
    const float topBarTop = 0.0f;   // top of screen
    const float titleBaseline = topBarTop + UiTheme::kTitleTextY;
    comp.drawText(ps4::FontRasterizer::FontFace::Title, titleBuf,
                  UiTheme::kTitleTextX, titleBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    drawControllerIndicators(comp, m_controllerImg, state.connectedControllersMask);
}

// ---------------------------------------------------------------------------
// Separator lines
// ---------------------------------------------------------------------------

void AppGridRenderer::drawSeparators(ps4::UiCompositor& comp) {
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

void AppGridRenderer::drawCard(const AppGridCardRenderState& card, int index,
                               const AppGridRenderState& state,
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

    // Artwork rectangle covers the full card area. Using drawAspectCoverImage
    // center-crops non-portrait artwork (e.g., 16:9 Desktop cards) so all cards
    // are rendered at the exact same physical size.
    const float artMargin = UiTheme::kArtMargin;
    const int artX = static_cast<int>(left + artMargin);
    const int artY = static_cast<int>(top + artMargin);
    const int artW = static_cast<int>(cardWidth() - artMargin * 2.0f);
    const int artH = static_cast<int>(cardHeight() - artMargin * 2.0f);
    if (card.artwork != nullptr) {
        drawAspectCoverImage(comp, *card.artwork, artX, artY, artW, artH);
    } else {
        drawAspectCoverImage(comp, m_appImg, artX, artY, artW, artH);
    }

    // Translucent dark bottom scrim so title and status are always crystal clear over any art
    comp.fillRect(ix, static_cast<int>(top + cardHeight() - 75.0f), iw, 75, 0, 0, 0, 190);

    // Text content.
    const float lineH = baseFontSize();

    // Application name (Regular) at the bottom of the card, with a baseline
    // margin from the card bottom.
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float nameBaseline = top + cardHeight() - 16.0f;
    // Truncate the app title with an ellipsis so it never overflows the card
    // width + margins (8px inset on each side).
    const std::string name = truncateTextWithEllipsis(
        *m_fonts, ps4::FontRasterizer::FontFace::Regular, card.title.c_str(),
        cardWidth() - 16.0f);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, name.c_str(),
                  left + 8.0f, nameBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Card status display (Regular) at the theme status offsets.
    //   - If this app is the one being quit, show the quit status text
    //     (e.g., "Quitting...") in place of the Running indicator.
    //   - Otherwise, if this app is waiting for a quit to complete before it
    //     launches (quit-and-launch), show "Pending...".
    //   - Otherwise, if this app is the locally connecting target, show
    //     "Connecting" (overrides Running until the stream is established).
    //   - Otherwise, if the app is running, show "Running".
    //   - Otherwise, show nothing.
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float statusX = left + 8.0f;
    const float statusY = nameBaseline - 40.0f;

    const char* statusText = nullptr;
    if (!card.quitStatusText.empty()) {
        statusText = card.quitStatusText.c_str();
    } else if (card.waitingToLaunch) {
        statusText = "Pending...";
    } else if (card.connecting) {
        statusText = "Connecting";
    } else if (card.running) {
        statusText = "Running";
    }
    if (statusText) {
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, statusText,
                      statusX, statusY,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // Card outline (focus treatment) drawn LAST so it is the top-most item and
    // is never covered by the artwork or text content.
    drawCardOutline(comp, ix, iy, iw, ih, isSelected);
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void AppGridRenderer::drawLegend(const AppGridRenderState& state,
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

    // Legend entries: Accept (Launch / Resume), Back to Hosts, Quit App (if
    // running), Settings. The accept label shows "Resume" when the focused
    // app is already running.
    const bool focusedRunning =
        state.selectedIndex >= 0 &&
        state.selectedIndex < static_cast<int>(state.cards.size()) &&
        state.cards[state.selectedIndex].running;
    LegendEntry entries[4];
    int entryCount = 0;
    entries[entryCount++] = {
        acceptIcon(state.crossIsAccept),
        focusedRunning ? UiStrings::kResume : UiStrings::kLaunch
    };
    // Advertise Triangle Quit only when the focused app is running.
    if (state.selectedIndex >= 0 &&
        state.selectedIndex < static_cast<int>(state.cards.size()) &&
        state.cards[state.selectedIndex].running) {
        entries[entryCount++] = {UiAtlasIcon::Triangle, UiStrings::kQuitApp};
    }
    entries[entryCount++] = {backIcon(state.crossIsAccept), UiStrings::kBackToHosts};
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

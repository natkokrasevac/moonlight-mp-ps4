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

// === AppListRenderer.cpp ===================================================
//
// OpenOrbis Moonlight client — CPU App List renderer.
//
// Implements the AppListRenderer: draws the App List screen into the CPU
// UiCompositor surface using only fillRect / strokeRect / drawImage /
// drawImageSubRect / drawText / clipping. It consumes a plain
// AppListRenderState snapshot and owns the App List layout geometry and the
// retained CPU-decoded resources.
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

#include "AppListRenderer.h"

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

static void appListDiagPrint(const char* msg) {
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

AppListRenderer::AppListRenderer() = default;

AppListRenderer::~AppListRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool AppListRenderer::initialize(ps4::FontRasterizer* fonts) {
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
        ps4::CpuPngImage appPng;
        if (ps4::decodePngToCpu("/app0/assets/images/app_fallback.png", appPng)) {
            adoptPng(appPng, m_appImg);
        } else {
            appListDiagPrint("[app-list] app_fallback.png decode failed (cards render without artwork)");
        }

        ps4::CpuPngImage atlasPng;
        if (ps4::decodePngToCpu("/app0/assets/images/ui_atlas.png", atlasPng)) {
            adoptPng(atlasPng, m_atlasImg);
        } else {
            appListDiagPrint("[app-list] ui_atlas.png decode failed (legend uses textual markers)");
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            adoptPng(bgPng, m_bgImg);
        } else {
            appListDiagPrint("[app-list] moonlight_bg.png decode failed (screen uses solid background)");
        }

        ps4::CpuPngImage ctrlPng;
        if (ps4::decodePngToCpu("/app0/assets/images/controller.png", ctrlPng)) {
            adoptPng(ctrlPng, m_controllerImg);
        }
    }

    m_initialized = true;
    return true;
}

void AppListRenderer::shutdown() {
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

bool AppListRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

AppListRenderer::Viewport AppListRenderer::crossbarViewport() const {
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

float AppListRenderer::crossbarCardLeft(int index, int selectedIndex,
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

void AppListRenderer::computeCardRect(int index, int selectedIndex,
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

ps4::Rect AppListRenderer::cardVisualBounds(int index, int selectedIndex) const {
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

ps4::Rect AppListRenderer::bottomLegendBounds() const {
    const float height = UiTheme::kBottomLegendHeight;
    return ps4::Rect{0,
                     static_cast<int>(1080.0f - height),
                     static_cast<int>(1920.0f),
                     static_cast<int>(height)};
}

ps4::Rect AppListRenderer::topBarBounds() const {
    const float topBarH = UiTheme::kTopBarHeight;
    return ps4::Rect{0, 0, 1920, static_cast<int>(topBarH)};
}

ps4::Rect AppListRenderer::cardAreaBounds() const {
    const Viewport v = crossbarViewport();
    return ps4::Rect{static_cast<int>(v.x), static_cast<int>(v.y),
                     static_cast<int>(v.w), static_cast<int>(v.h)};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void AppListRenderer::renderFull(const AppListRenderState& state,
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

void AppListRenderer::drawTopBar(const AppListRenderState& state,
                                 ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
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

void AppListRenderer::drawSeparators(ps4::UiCompositor& comp) {
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

void AppListRenderer::drawCard(const AppListCardRenderData& card, int index,
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

    // Solid dark card fill so every card has a consistent solid base
    comp.fillRect(ix, iy, iw, ih, 20, 26, 38, 255);

    // Artwork rectangle covers the full card area. Using drawAspectCoverImage
    // center-crops non-portrait artwork (e.g., 16:9 Desktop cards) so all cards
    // are rendered at the exact same physical size.
    const float artMargin = UiTheme::kArtMargin * scale;
    const int artX = static_cast<int>(left + artMargin);
    const int artY = static_cast<int>(top + artMargin);
    const int artW = static_cast<int>(w - artMargin * 2.0f);
    const int artH = static_cast<int>(h - artMargin * 2.0f);
    if (card.artwork != nullptr) {
        drawAspectCoverImage(comp, *card.artwork, artX, artY, artW, artH);
    } else {
        drawAspectCoverImage(comp, m_appImg, artX, artY, artW, artH);
    }

    // Translucent dark bottom scrim for title/status readability
    comp.fillRect(ix, static_cast<int>(top + h - 65.0f * scale), iw, static_cast<int>(65.0f * scale), 0, 0, 0, 190);

    // Text content.
    const float fontSize = baseFontSize() * scale;
    const float lineH = fontSize;

    // Application name at the bottom of the card, with a baseline margin from
    // the card bottom. Focused cards use Regular; non-focused cards use
    // RegularSmall.
    float baselineY = 0.0f, lineHeight = 0.0f;
    const ps4::FontRasterizer::FontFace nameFace =
        isSelected ? ps4::FontRasterizer::FontFace::Regular
                   : ps4::FontRasterizer::FontFace::RegularSmall;
    m_fonts->getHorizontalLayout(nameFace, baselineY, lineHeight);
    const float nameBaseline = top + h - 16.0f;
    // Truncate the app title with an ellipsis so it never overflows the card
    // width + margins (8px inset on each side).
    const std::string name = truncateTextWithEllipsis(
        *m_fonts, nameFace, card.title.c_str(), w - 16.0f);
    comp.drawText(nameFace, name.c_str(),
                  left + 8.0f, nameBaseline,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Card status display at the theme status offsets, scaled with the card.
    // Focused cards use Regular; non-focused cards use RegularSmall.
    //   - If this app is the one being quit, show the quit status text
    //     (e.g., "Quitting...") in place of the Running indicator.
    //   - Otherwise, if this app is waiting for a quit to complete before it
    //     launches (quit-and-launch), show "Pending...".
    //   - Otherwise, if this app is the locally connecting target, show
    //     "Connecting" (overrides Running until the stream is established).
    //   - Otherwise, if the app is running, show "Running".
    //   - Otherwise, show nothing.
    const ps4::FontRasterizer::FontFace statusFace =
        isSelected ? ps4::FontRasterizer::FontFace::Regular
                   : ps4::FontRasterizer::FontFace::RegularSmall;
    m_fonts->getHorizontalLayout(statusFace, baselineY, lineHeight);
    const float statusX = left + 8.0f * scale;
    const float statusY = nameBaseline - 40.0f * scale;

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
        comp.drawText(statusFace, statusText,
                      statusX, statusY,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // Card outline (focus treatment) drawn LAST so it is the top-most item and
    // is never covered by the artwork or text content.
    drawCardOutline(comp, ix, iy, iw, ih, isSelected);

    // Focus-card footer (Launch / Resume). Only the focused card gets a
    // footer. Show "Resume" when the focused app is already running.
    if (isSelected) {
        drawFocusFooter(comp, *m_fonts, left, top, w, h, footerHeight(),
                        card.running ? UiStrings::kResume : UiStrings::kLaunch,
                        baselineY, lineH);
    }
}

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

void AppListRenderer::drawLegend(const AppListRenderState& state,
                                 ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
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

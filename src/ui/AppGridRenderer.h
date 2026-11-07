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

// === AppGridRenderer.h =====================================================
//
// Moonlight client for PlayStation 4 — CPU App List Grid renderer.
//
// AppGridRenderer draws the App List Grid presentation mode into the shared
// CPU UiCompositor surface. It consumes a plain AppGridRenderState snapshot
// produced by UiManager and owns the grid card geometry, grid clipping, grid
// visual layout, and card rendering.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_APP_GRID_RENDERER_H
#define UI_APP_GRID_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect
#include "platform/ps4/PngCpuDecode.h"   // ps4::CpuPngImage (runtime app artwork)
#include "moonlight/ApplicationCatalog.h" // ml::ApplicationId
#include "UiTheme.h"                     // ui::UiTheme::kBodyFontSize

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// App Grid render snapshot
// ---------------------------------------------------------------------------

/// Render data for one app grid card. Contains only the data required to draw
/// the card. No domain/application semantics live here.
struct AppGridCardRenderState {
    ml::ApplicationId id = 0;
    std::string title;
    bool running = false;       ///< Host-reported running state.
    bool connecting = false;    ///< Client-local transient connecting state.
    bool quitting = false;      ///< This app is the active quit target.
    std::string quitStatusText; ///< Quit-progress status text (when quitting).
    bool waitingToLaunch = false; ///< Waiting for a quit to complete before launching.
    bool selected = false;
    /// Pointer to the resolved runtime artwork for this app (owned by
    /// MoonlightApp's runtime artwork map). Null means use app_fallback.png.
    const ps4::CpuPngImage* artwork = nullptr;
};

/// Render snapshot for the App List Grid presentation mode. A plain value
/// copy consumed by AppGridRenderer. UiManager produces it; it is not an
/// authoritative store.
struct AppGridRenderState {
    std::vector<AppGridCardRenderState> cards;
    int selectedIndex = -1;      ///< Index into `cards` (-1 = none).
    std::string hostName;        ///< Host context for the top bar.
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
    uint8_t connectedControllersMask = 1; ///< Bitmask of connected controllers (bit 0 = P1, etc.).
};

// ---------------------------------------------------------------------------
// AppGridRenderer
// ---------------------------------------------------------------------------

/// CPU App List Grid renderer. Draws the App List Grid presentation mode
/// into the shared CPU UiCompositor surface.
class AppGridRenderer {
public:
    AppGridRenderer();
    ~AppGridRenderer();
    AppGridRenderer(const AppGridRenderer&) = delete;
    AppGridRenderer& operator=(const AppGridRenderer&) = delete;
    AppGridRenderer(AppGridRenderer&&) = delete;
    AppGridRenderer& operator=(AppGridRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    int columns() const;
    float scrollOffsetFor(const AppGridRenderState& state) const;
    ps4::Rect cardVisualBounds(int index, const AppGridRenderState& state) const;
    ps4::Rect bottomLegendBounds() const;
    ps4::Rect topBarBounds() const;
    ps4::Rect cardAreaBounds() const;
    ps4::Rect gridViewportBounds() const;

    void renderFull(const AppGridRenderState& state, ps4::UiCompositor& comp);

    const ps4::CpuImage& appImage() const { return m_appImg; }
    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    float cardWidth() const { return 300.0f; }
    float cardHeight() const { return 340.0f; }
    float spacingX() const { return 28.0f; }
    float spacingY() const { return 24.0f; }
    float baseFontSize() const { return UiTheme::kBodyFontSize; }

    struct Viewport { float x, y, w, h; };
    Viewport gridViewport() const;
    struct GridLayout { int columns; float rowPitch; float columnPitch; };
    GridLayout makeLayout(const Viewport& v) const;
    void cardPosition(int index, const GridLayout& layout,
                      const Viewport& v, float scrollOffset,
                      float& left, float& top) const;
    void drawCard(const AppGridCardRenderState& card, int index,
                  const AppGridRenderState& state, ps4::UiCompositor& comp);
    void drawTopBar(const AppGridRenderState& state, ps4::UiCompositor& comp);
    void drawSeparators(ps4::UiCompositor& comp);
    void drawLegend(const AppGridRenderState& state, ps4::UiCompositor& comp);

    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_appImg;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    ps4::CpuImage m_controllerImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_APP_GRID_RENDERER_H

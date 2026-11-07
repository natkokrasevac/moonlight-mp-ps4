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

// === AppListRenderer.h ======================================================
//
// Moonlight client for PlayStation 4 — CPU App List renderer.
//
// AppListRenderer draws the App List screen into the shared CPU UiCompositor
// surface. It consumes a plain AppListRenderState snapshot produced by
// UiManager and owns the App List layout geometry and the retained CPU-decoded
// resources.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_APP_LIST_RENDERER_H
#define UI_APP_LIST_RENDERER_H

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
// App List render snapshot
// ---------------------------------------------------------------------------

/// Render data for one app card. Contains only the data required to draw the
/// card. No domain/application semantics live here.
struct AppListCardRenderData {
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

/// Render snapshot for the App List screen. A plain value copy consumed by
/// AppListRenderer. UiManager produces it; it is not an authoritative store.
struct AppListRenderState {
    std::vector<AppListCardRenderData> cards;
    int selectedIndex = -1;      ///< Index into `cards` (-1 = none).
    std::string hostName;        ///< Host context for the top bar.
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
    uint8_t connectedControllersMask = 1; ///< Bitmask of connected controllers (bit 0 = P1, etc.).
};

// ---------------------------------------------------------------------------
// AppListRenderer
// ---------------------------------------------------------------------------

/// CPU App List renderer. Draws the App List screen into the shared CPU
/// UiCompositor surface.
class AppListRenderer {
public:
    AppListRenderer();
    ~AppListRenderer();
    AppListRenderer(const AppListRenderer&) = delete;
    AppListRenderer& operator=(const AppListRenderer&) = delete;
    AppListRenderer(AppListRenderer&&) = delete;
    AppListRenderer& operator=(AppListRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect cardVisualBounds(int index, int selectedIndex) const;
    ps4::Rect bottomLegendBounds() const;
    ps4::Rect topBarBounds() const;
    ps4::Rect cardAreaBounds() const;

    void renderFull(const AppListRenderState& state, ps4::UiCompositor& comp);

    const ps4::CpuImage& appImage() const { return m_appImg; }
    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    float cardWidth() const { return 300.0f; }
    float cardHeight() const { return 400.0f; }
    float spacing() const { return 16.0f; }
    float nonFocusScale() const { return 0.70f; }
    float footerHeight() const { return 64.0f; }
    float baseFontSize() const { return UiTheme::kBodyFontSize; }

    struct Viewport { float x, y, w, h; };
    Viewport crossbarViewport() const;
    float crossbarCardLeft(int index, int selectedIndex, float focusLeft) const;
    void computeCardRect(int index, int selectedIndex,
                         float& left, float& top, float& w, float& h) const;
    void drawCard(const AppListCardRenderData& card, int index,
                  int selectedIndex, ps4::UiCompositor& comp);
    void drawTopBar(const AppListRenderState& state, ps4::UiCompositor& comp);
    void drawSeparators(ps4::UiCompositor& comp);
    void drawLegend(const AppListRenderState& state, ps4::UiCompositor& comp);

    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_appImg;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    ps4::CpuImage m_controllerImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_APP_LIST_RENDERER_H

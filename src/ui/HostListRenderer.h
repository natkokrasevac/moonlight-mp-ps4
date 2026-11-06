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

// === HostListRenderer.h =====================================================
//
// Moonlight client for PlayStation 4 — CPU Host List renderer.
//
// This is a narrowly-scoped renderer on the `exp_cpu_rasterization` branch.
// It consumes a plain `HostListRenderState` snapshot produced by UiManager and
// draws the Host List screen into the CPU UiCompositor surface, which is then
// presented through the UiBlitter. It is deliberately NOT a generic
// widget/scene-graph/layout framework.
//
// Ownership model:
//   UiManager        : state, navigation, intents, Host List selection.
//   HostListRenderer : presentation only (layout + drawing into UiCompositor).
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
//
// OpenOrbis adaptation: retained images are held as ps4::CpuImage (raw
// pointer + size), matching the OpenOrbis compositor representation.
// =============================================================================

#ifndef UI_HOST_LIST_RENDERER_H
#define UI_HOST_LIST_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect
#include "moonlight/HostManager.h"       // ml::HostId, ml::HostAvailability, ml::HostPairingState
#include "UiTheme.h"                     // ui::UiTheme::kBodyFontSize

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// Host List render snapshot
// ---------------------------------------------------------------------------

/// Render data for one host card. Contains only the data required to draw the
/// card. No domain/application semantics live here.
struct HostListCardRenderData {
    ml::HostId id = 0;
    std::string displayName;
    ml::HostAvailability availability = ml::HostAvailability::Offline;
    ml::HostPairingState pairingState = ml::HostPairingState::Unknown;
    bool appRunning = false;
    bool selected = false;
};

/// Render snapshot for the Host List screen. A plain value copy consumed by
/// HostListRenderer. UiManager produces it; it is not an authoritative store.
struct HostListRenderState {
    std::vector<HostListCardRenderData> cards;
    int selectedIndex = -1;      ///< Index into `cards` (-1 = none).
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
    uint8_t connectedControllersMask = 1; ///< Bitmask of connected controllers (bit 0 = P1, etc.).
};

// ---------------------------------------------------------------------------
// HostListRenderer
// ---------------------------------------------------------------------------

/// CPU Host List renderer. Owns the Host List layout geometry and the retained
/// CPU-decoded resources (host.png, ui_atlas.png). Draws the Host List screen
/// into a UiCompositor surface using only fillRect / strokeRect / drawImage /
/// drawImageSubRect / drawText / clipping.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class HostListRenderer {
public:
    /// Constructor performs no fallible operations.
    HostListRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~HostListRenderer();

    // Non-copyable, non-movable.
    HostListRenderer(const HostListRenderer&) = delete;
    HostListRenderer& operator=(const HostListRenderer&) = delete;
    HostListRenderer(HostListRenderer&&) = delete;
    HostListRenderer& operator=(HostListRenderer&&) = delete;

    /// Decode the retained CPU resources (host.png, ui_atlas.png) once and
    /// retain the FontRasterizer reference for text drawing. Returns true on
    /// success. On failure the object remains safe to shut down or destroy.
    bool initialize(ps4::FontRasterizer* fonts);

    /// Release retained resources. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Layout / visual bounds helpers ------------------------------------
    // These are the single source of Host List visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the card at `index` when `selectedIndex` is the
    /// focused card. Includes the focused card's footer. A few pixels of
    /// conservative padding are included.
    ps4::Rect cardVisualBounds(int index, int selectedIndex) const;

    /// Screen-space bounds of the BottomLegend bar.
    ps4::Rect bottomLegendBounds() const;

    /// Screen-space bounds of the top title bar.
    ps4::Rect topBarBounds() const;

    /// Screen-space bounds of the full card grid/crossbar area. Used for
    /// structural redraws (host add/remove/order change).
    ps4::Rect cardAreaBounds() const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the Host List screen for the given state into `comp`.
    void renderFull(const HostListRenderState& state, ps4::UiCompositor& comp);

    // --- Retained resources -------------------------------------------------

    /// The CPU-decoded host card artwork (host.png). Empty if unavailable.
    const ps4::CpuImage& hostImage() const { return m_hostImg; }

    /// The CPU-decoded UI atlas (ui_atlas.png). Empty if unavailable.
    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    // --- Layout geometry (from current source crossbar values) --------------
    float cardWidth() const { return 300.0f; }
    float cardHeight() const { return 400.0f; }
    float spacing() const { return 16.0f; }
    float nonFocusScale() const { return 0.70f; }
    float footerHeight() const { return 64.0f; }
    float baseFontSize() const { return UiTheme::kBodyFontSize; }

    /// The crossbar viewport (origin + size) derived from the screen chrome.
    struct Viewport { float x, y, w, h; };
    Viewport crossbarViewport() const;

    /// Screen-space left edge x for a crossbar card.
    float crossbarCardLeft(int index, int selectedIndex, float focusLeft) const;

    /// Compute the card's rendered rectangle (left/top/width/height) for the
    /// card at `index` given the focused `selectedIndex`.
    void computeCardRect(int index, int selectedIndex,
                         float& left, float& top, float& w, float& h) const;

    /// Draw a single card into `comp`.
    void drawCard(const HostListCardRenderData& card, int index,
                  int selectedIndex, ps4::UiCompositor& comp);

    /// Draw the top title bar into `comp`.
    void drawTopBar(const HostListRenderState& state, ps4::UiCompositor& comp);

    /// Draw the single-pixel separator lines at the top bar bottom and the
    /// bottom legend top into `comp`.
    void drawSeparators(ps4::UiCompositor& comp);

    /// Draw the BottomLegend into `comp`.
    void drawLegend(const HostListRenderState& state, ps4::UiCompositor& comp);

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_hostImg;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    ps4::CpuImage m_controllerImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_HOST_LIST_RENDERER_H

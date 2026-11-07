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

// === HostGridRenderer.h =====================================================
//
// Moonlight client for PlayStation 4 — CPU Host List Grid renderer.
//
// This is a narrowly-scoped renderer on the `exp_cpu_rasterization` branch.
// It consumes a plain `HostGridRenderState` snapshot produced by UiManager and
// draws the Host List Grid presentation mode into the CPU UiCompositor
// surface, which is then presented through the UiBlitter. It is deliberately
// NOT a generic widget/scene-graph/layout framework.
//
// Ownership model:
//   UiManager          : state, navigation, intents, Host List selection,
//                        grid/crossbar presentation mode.
//   HostGridRenderer   : presentation only (grid card geometry, grid clipping,
//                        grid visual layout, card rendering into UiCompositor).
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
//
// OpenOrbis adaptation: retained images are held as ps4::CpuImage (raw
// pointer + size), matching the OpenOrbis compositor representation.
// =============================================================================

#ifndef UI_HOST_GRID_RENDERER_H
#define UI_HOST_GRID_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect
#include "UiTheme.h"                     // ui::UiTheme::kBodyFontSize

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// Host Grid render snapshot
// ---------------------------------------------------------------------------

/// Availability state for a host grid card. A presentation-only projection of
/// the authoritative ml::HostAvailability. No domain semantics live here.
enum class HostGridAvailability : uint8_t {
    Online,
    Stale,
    Offline,
    ManuallyAdded
};

/// Render data for one host grid card. Contains only the data required to draw
/// the card. No domain/application semantics live here.
struct HostGridCardRenderState {
    std::string name;
    bool paired = false;
    HostGridAvailability availability = HostGridAvailability::Offline;
    bool appRunning = false;
    bool selected = false;
};

/// Render snapshot for the Host List Grid presentation mode. A plain value
/// copy consumed by HostGridRenderer. UiManager produces it; it is not an
/// authoritative store.
struct HostGridRenderState {
    std::vector<HostGridCardRenderState> hosts;
    int selectedIndex = -1;      ///< Index into `hosts` (-1 = none).
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
    uint8_t connectedControllersMask = 1; ///< Bitmask of connected controllers (bit 0 = P1, etc.).
};

// ---------------------------------------------------------------------------
// HostGridRenderer
// ---------------------------------------------------------------------------

/// CPU Host List Grid renderer. Owns the grid card geometry, grid clipping,
/// grid visual layout, and card rendering. Draws the Host List Grid
/// presentation mode into a UiCompositor surface using only fillRect /
/// strokeRect / drawImage / drawImageSubRect / drawText / clipping.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class HostGridRenderer {
public:
    /// Constructor performs no fallible operations.
    HostGridRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~HostGridRenderer();

    // Non-copyable, non-movable.
    HostGridRenderer(const HostGridRenderer&) = delete;
    HostGridRenderer& operator=(const HostGridRenderer&) = delete;
    HostGridRenderer(HostGridRenderer&&) = delete;
    HostGridRenderer& operator=(HostGridRenderer&&) = delete;

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
    // These are the single source of Host Grid visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// The runtime-derived column count for the grid, computed from the fixed
    /// screen chrome. Owned by the renderer (it owns the grid geometry).
    int columns() const;

    /// The vertical scroll offset (px) into the grid that keeps the selected
    /// row fully visible, computed from `state.selectedIndex`. Owned by the
    /// renderer (it owns the grid geometry). Used by the renderer for drawing
    /// and by MoonlightApp for dirty-region invalidation.
    float scrollOffsetFor(const HostGridRenderState& state) const;

    /// Screen-space bounds of the card at `index` given the grid layout
    /// derived from `state`. A few pixels of conservative padding are
    /// included. The scroll offset is applied.
    ps4::Rect cardVisualBounds(int index, const HostGridRenderState& state) const;

    /// Screen-space bounds of the BottomLegend bar.
    ps4::Rect bottomLegendBounds() const;

    /// Screen-space bounds of the top title bar.
    ps4::Rect topBarBounds() const;

    /// Screen-space bounds of the full card grid area (the scrollable grid
    /// viewport). Used for structural redraws (host add/remove/order change)
    /// and full scroll-region redraws.
    ps4::Rect cardAreaBounds() const;

    /// The grid viewport rectangle (the scrollable region the grid is clipped
    /// to). Cards are clipped to this region.
    ps4::Rect gridViewportBounds() const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the Host List Grid screen for the given state into `comp`.
    void renderFull(const HostGridRenderState& state, ps4::UiCompositor& comp);

    // --- Retained resources -------------------------------------------------

    /// The CPU-decoded host card artwork (host.png). Empty if unavailable.
    const ps4::CpuImage& hostImage() const { return m_hostImg; }

    /// The CPU-decoded UI atlas (ui_atlas.png). Empty if unavailable.
    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    // --- Layout geometry (from current source grid values) ------------------
    float cardWidth() const { return 300.0f; }
    float cardHeight() const { return 340.0f; }
    float spacingX() const { return 28.0f; }
    float spacingY() const { return 24.0f; }
    float baseFontSize() const { return UiTheme::kBodyFontSize; }

    /// The grid viewport (origin + size) derived from the screen chrome.
    struct Viewport { float x, y, w, h; };
    Viewport gridViewport() const;

    /// Compute the grid layout (columns, rowPitch, columnPitch) from the
    /// viewport dimensions.
    struct GridLayout {
        int columns;
        float rowPitch;
        float columnPitch;
    };
    GridLayout makeLayout(const Viewport& v) const;

    /// Screen-space position of a card's top-left corner, relative to the grid
    /// content origin (which already accounts for the scroll offset).
    void cardPosition(int index, const GridLayout& layout,
                      const Viewport& v, float scrollOffset,
                      float& left, float& top) const;

    /// Draw a single card into `comp`.
    void drawCard(const HostGridCardRenderState& card, int index,
                  const HostGridRenderState& state, ps4::UiCompositor& comp);

    /// Draw the top title bar into `comp`.
    void drawTopBar(const HostGridRenderState& state, ps4::UiCompositor& comp);

    /// Draw the single-pixel separator lines at the top bar bottom and the
    /// bottom legend top into `comp`.
    void drawSeparators(ps4::UiCompositor& comp);

    /// Draw the BottomLegend into `comp`.
    void drawLegend(const HostGridRenderState& state, ps4::UiCompositor& comp);

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_hostImg;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    ps4::CpuImage m_controllerImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_HOST_GRID_RENDERER_H

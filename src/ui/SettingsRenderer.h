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

// === SettingsRenderer.h =====================================================
//
// OpenOrbis Moonlight client — CPU Settings overlay renderer.
//
// Consumes a plain `SettingsRenderState` snapshot produced by UiManager and
// draws the Settings overlay (centered modal dialog + rows) into
// the shared CPU UiCompositor surface, which is then presented through the
// UiBlitter. It is deliberately NOT a generic widget/scene-graph/layout
// framework.
//
// Ownership model:
//   UiManager          : state, navigation, intents, Settings open/closed,
//                        selected row, Settings values.
//   SettingsRenderer   : presentation only (layout + drawing into UiCompositor).
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_SETTINGS_RENDERER_H
#define UI_SETTINGS_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::Rect
#include "UiManager.h"                   // ui::SettingsRenderState, ui::SettingsRowRenderData

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// SettingsRenderer
// ---------------------------------------------------------------------------

/// CPU Settings overlay renderer. Owns the Settings layout geometry and draws
/// the Settings overlay into a UiCompositor surface using only fillRect /
/// strokeRect / drawText / clipping.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class SettingsRenderer {
public:
    /// Constructor performs no fallible operations.
    SettingsRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~SettingsRenderer();

    // Non-copyable, non-movable.
    SettingsRenderer(const SettingsRenderer&) = delete;
    SettingsRenderer& operator=(const SettingsRenderer&) = delete;
    SettingsRenderer(SettingsRenderer&&) = delete;
    SettingsRenderer& operator=(SettingsRenderer&&) = delete;

    /// Retain the FontRasterizer reference for text drawing. Returns true on
    /// success. On failure the object remains safe to shut down or destroy.
    bool initialize(ps4::FontRasterizer* fonts);

    /// Release retained resources. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Layout / visual bounds helpers ------------------------------------
    // These are the single source of Settings visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the full-screen dim layer.
    ps4::Rect dimBounds() const;

    /// Screen-space bounds of the centered Settings modal panel.
    ps4::Rect panelBounds() const;

    /// Screen-space bounds of the row at `rowIndex`. A few pixels of
    /// conservative padding are included.
    ps4::Rect rowBounds(int rowIndex) const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the Settings overlay (dim + panel + all rows) for the given
    /// state into `comp`. Used when the Settings overlay opens.
    void renderFull(const SettingsRenderState& state, ps4::UiCompositor& comp);

private:
    // --- Layout geometry ----------------------------------------------------
    float panelW() const { return 960.0f; }
    float panelH() const { return 940.0f; }
    float panelX() const { return (1920.0f - panelW()) * 0.5f; }
    float panelY() const { return (1080.0f - panelH()) * 0.5f; }
    float margin() const { return 24.0f; }
    float rowHeight() const { return 44.0f; }
    float titleBlockHeight() const { return 48.0f; }
    float sectionHeaderHeight() const { return 32.0f; }
    float separatorHeight() const { return 28.0f; }

    /// The Y position of the top of the row at `rowIndex`. Rows 0-5 are under
    /// the STREAM header; rows 6-7 (Presentation Style, Show Quit App Dialog)
    /// are under the CLIENT header; rows 8-9 (Speakers, Game Optimization) are
    /// under the HOST header; rows 10-12 (Stick Deadzone, Swap Face Buttons,
    /// Gyro Motion) are under the GAMEPAD header.
    float rowTop(int rowIndex) const;

    /// Draw a single row into `comp` at the given Y position.
    void drawRow(const SettingsRowRenderData& row, int rowIndex,
                 float rowY, ps4::UiCompositor& comp);

    /// Draw the section separator lines (between STREAM/CLIENT/HOST/INPUT)
    /// into `comp`.
    void drawSeparators(ps4::UiCompositor& comp);

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_SETTINGS_RENDERER_H

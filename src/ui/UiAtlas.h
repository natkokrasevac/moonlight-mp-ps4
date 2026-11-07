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

// === UiAtlas.h ==============================================================
//
// Moonlight client for PlayStation 4 — shared UI atlas metadata.
//
// This header centralizes the shared visual metadata for the UI atlas
// (ui_atlas.png): the icon enum and the atlas grid geometry. The renderer
// layer owns atlas interpretation (which icon maps to which row/column and how
// the glyphs are drawn). This header does NOT own the actual texture; the
// renderers retain and decode the CPU atlas image themselves.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_UI_ATLAS_H
#define UI_UI_ATLAS_H

#include <cstdint>

namespace ui {

// ---------------------------------------------------------------------------
// UI atlas icons
// ---------------------------------------------------------------------------

/// Icons available in the shared UI atlas (ui_atlas.png). The atlas is a
/// regular 4×4 grid of 64×64 cells. Each cell carries ~10 px of transparent
/// padding around the painted glyph. Rows 2–3 (status icons) are reserved for
/// a future visual-polish step and are not used by the BottomLegend yet.
enum class UiAtlasIcon : uint8_t {
    Cross,          // row 0, col 0
    Circle,         // row 0, col 1
    Triangle,       // row 0, col 2
    Square,         // row 0, col 3
    Options,        // row 1, col 0
    Touchpad,       // row 1, col 1
    L3,             // row 1, col 2
    R3,             // row 1, col 3
    TouchpadL,      // row 2, col 0  (reserved)
    TouchpadR,      // row 2, col 1  (reserved)
    AppRunning,     // row 2, col 2  (reserved)
    NoAppRunning,   // row 2, col 3  (reserved)
    Online,         // row 3, col 0  (reserved)
    Stale,          // row 3, col 1  (reserved)
    Offline,        // row 3, col 2  (reserved)
    PsButton        // row 3, col 3  (reserved)
};

// ---------------------------------------------------------------------------
// Atlas grid metadata
// ---------------------------------------------------------------------------
//
// The UI atlas (ui_atlas.png) is a regular 4×4 grid of 64×64 cells in a
// 256×256 texture. These are the shared visual constants used by the renderers
// that draw atlas glyphs (HostList, HostGrid, AppList, AppGrid, Pairing,
// StreamExitHint).

namespace UiAtlas {

/// Atlas cell size (px). Each cell is 64×64.
constexpr int kCellSize = 64;

/// Atlas grid dimensions: 4 columns × 4 rows.
constexpr int kColumns = 4;
constexpr int kRows = 4;

/// Atlas texture dimensions (px). 4×64 = 256.
constexpr int kTextureWidth = kColumns * kCellSize;
constexpr int kTextureHeight = kRows * kCellSize;

/// Default glyph draw size (px) used by the bottom legends and the exit hint.
constexpr float kGlyphSize = 64.0f;

/// Horizontal gap (px) between a glyph and its adjacent label.
constexpr float kGlyphToLabelGap = 5.0f;

/// Horizontal gap (px) between adjacent legend entries.
constexpr float kEntryGap = 56.0f;

} // namespace UiAtlas

} // namespace ui

#endif // UI_UI_ATLAS_H

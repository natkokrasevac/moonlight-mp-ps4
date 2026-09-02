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

// === UiTheme.h ==============================================================
//
// Moonlight client for PlayStation 4 — shared UI theme / visual metadata.
//
// This header centralizes the genuinely shared visual constants used by the
// dedicated CPU renderers. It is deliberately NOT a UI framework: it only
// extracts values that are used by more than one renderer. Renderer-specific
// layout geometry and renderer-specific sizes remain local to each renderer.
//
// The color representation matches the CPU UiCompositor API: straight-alpha
// RGBA8 components passed as uint8_t r/g/b/a. No conversions are introduced.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_UI_THEME_H
#define UI_UI_THEME_H

#include <cstdint>

namespace ui {

// ---------------------------------------------------------------------------
// Shared colors
// ---------------------------------------------------------------------------
//
// These are the genuinely shared accent / background / panel / text colors
// used by multiple renderers. Each is a straight-alpha RGBA8 tuple matching
// the UiCompositor fillRect / strokeRect / drawText / clear signatures.
//
// Renderer-specific colors (e.g. the Boot screen's 64,64,64 background, the
// Add Host validation-error red 242,89,89, the on-screen-keyboard note gray
// 153,153,153) are intentionally left local to their renderer.

namespace UiTheme {

/// Primary text color (near-white). Used by every renderer for default text.
constexpr uint8_t kTextR = 255;
constexpr uint8_t kTextG = 255;
constexpr uint8_t kTextB = 255;

/// Secondary / body text color (light gray). Used by Modal, AddHost, Pairing,
/// and Boot for explanatory / status text.
constexpr uint8_t kSecondaryTextR = 235;
constexpr uint8_t kSecondaryTextG = 235;
constexpr uint8_t kSecondaryTextB = 235;

/// Disabled / dimmed text color (mid gray). Used by Settings (disabled rows)
/// and AddHost (on-screen-keyboard note).
constexpr uint8_t kDisabledTextR = 140;
constexpr uint8_t kDisabledTextG = 140;
constexpr uint8_t kDisabledTextB = 140;

/// Accent / focus color. Used for focus borders, selection highlights,
/// and the Pairing PIN display.
constexpr uint8_t kAccentR = 0xFF;
constexpr uint8_t kAccentG = 0xFF;
constexpr uint8_t kAccentB = 0xFF;

/// Selection highlight fill (blue). Used by Settings, Modal, and AddHost for
/// the selected row / button.
constexpr uint8_t kSelectionR = 0x33;
constexpr uint8_t kSelectionG = 0x47;
constexpr uint8_t kSelectionB = 0x66;

/// Card outline (unfocused) color (dark gray). Used by HostList, HostGrid,
/// AppList, and AppGrid for the non-focused card border.
constexpr uint8_t kCardOutlineR = 0x38;
constexpr uint8_t kCardOutlineG = 0x38;
constexpr uint8_t kCardOutlineB = 0x42;

/// Focus-card footer fill (dark blue-gray). Used by HostList and AppList for
/// the focused card's footer background.
constexpr uint8_t kFooterFillR = 0x15;
constexpr uint8_t kFooterFillG = 0x4A;
constexpr uint8_t kFooterFillB = 0x9E;

/// Screen background color (dark blue-gray). Used by HostList, HostGrid,
/// AppList, AppGrid, and Pairing.
constexpr uint8_t kBackgroundR = 0x4E;//18;
constexpr uint8_t kBackgroundG = 0x69;//18;
constexpr uint8_t kBackgroundB = 0xB9;//24;

/// Panel fill color (dark). Used by Settings, Modal, and AddHost for the
/// opaque/semi-opaque panel background.
constexpr uint8_t kPanelFillR = 20;
constexpr uint8_t kPanelFillG = 20;
constexpr uint8_t kPanelFillB = 26;

/// Separator line color (gray). Used by Settings, Modal, AddHost, and Pairing
/// for the horizontal separator under titles / section headers.
constexpr uint8_t kSeparatorR = 0x1D;
constexpr uint8_t kSeparatorG = 0x9B;
constexpr uint8_t kSeparatorB = 0xD2;

/// Panel border color (light gray). Used by Modal and AddHost for the panel
/// outline.
constexpr uint8_t kPanelBorderR = 120;
constexpr uint8_t kPanelBorderG = 120;
constexpr uint8_t kPanelBorderB = 140;

/// Full-screen dim layer alpha (translucent black). Used by Settings, Modal,
/// AddHost, StreamExitHint, and StreamStatistics for the dim / panel overlay
/// over the underlying content.
constexpr uint8_t kDimAlpha = 140;

} // namespace UiTheme

// ---------------------------------------------------------------------------
// Shared typography
// ---------------------------------------------------------------------------
//
// These are the genuinely shared font sizes used by multiple renderers. The
// FontRasterizer binds six faces: Regular at kBodyFontSize, Bold at
// kBoldFontSize, Title at kTitleFontSize, Boot at kBootFontSize, BoldSmall at
// kBoldSmallFontSize, and RegularSmall at kRegularSmallFontSize. Renderers
// that need a different size (e.g. the Pairing PIN display) keep their own
// local sizing.

namespace UiTheme {

/// Body font size (px). Used by HostList, HostGrid, AppList, and AppGrid for
/// card body text (the Regular face is bound at this size).
constexpr float kBodyFontSize = 28.0f;

/// Bold font size (px). Used by card display names and other emphasized text
/// (the Bold face is bound at this size).
constexpr float kBoldFontSize = 28.0f;

/// Title font size (px). Used by window titles (the Title face is bound at
/// this size).
constexpr float kTitleFontSize = 48.0f;

/// Boot font size (px). Used by the Boot screen fallback title (the Boot face
/// is bound at this size).
constexpr float kBootFontSize = 64.0f;

/// BoldSmall font size (px). Used by non-focused crossbar card display names
/// (the BoldSmall face is bound at this size).
constexpr float kBoldSmallFontSize = 20.0f;

/// RegularSmall font size (px). Used by smaller regular text (the RegularSmall
/// face is bound at this size).
constexpr float kRegularSmallFontSize = 20.0f;

} // namespace UiTheme

// ---------------------------------------------------------------------------
// Shared layout dimensions
// ---------------------------------------------------------------------------
//
// These are the genuinely shared screen-chrome dimensions used by multiple
// renderers. Centralizing them here provides a single place to tweak or theme
// the screen layout.

namespace UiTheme {

/// Top title bar height (px). Used by the Host/App crossbar and grid screens
/// for the top title bar.
constexpr float kTopBarHeight = 135.0f;

/// Bottom legend bar height (px). Used by the Host/App crossbar and grid
/// screens for the bottom legend bar.
constexpr float kBottomLegendHeight = 135.0f;

/// Title text starting X position (px from left). Used by the top title bar
/// for the left edge of the title text.
constexpr float kTitleTextX = 110.0f;

/// Title text Y position (px from top bar viewport top). Used by the top
/// title bar for the baseline of the title text.
constexpr float kTitleTextY = 112.0f;

/// Bottom bar content starting X position (px from left). Used by the bottom
/// legend bar for the left edge of the legend content.
constexpr float kBottomBarContentX = 110.0f;

/// Bottom bar content Y position (px from bottom legend viewport top). Used
/// by the bottom legend bar for the top of the legend content.
constexpr float kBottomBarContentY = 20.0f;

/// Card top offset (px below the top bar bottom). Used by the Host/App
/// crossbar and grid screens for the top edge of the card row. Relative to the
/// top bar bottom so cards can never overlap the top bar.
constexpr float kCardTopOffset = 65.0f;

/// Card artwork margin (px). Used by the Host/App crossbar and grid screens
/// for the inset of the card artwork within the card.
constexpr float kArtMargin = 0.0f;

/// Card artwork height (px). Used by the Host/App crossbar and grid screens
/// for the height of the card artwork area. Set to fill the full card height
/// (cardHeight - 2 * kArtMargin) so artwork is not scaled down.
constexpr float kArtHeight = 400.0f;

} // namespace UiTheme

} // namespace ui

#endif // UI_UI_THEME_H

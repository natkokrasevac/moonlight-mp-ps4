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

// === UiRenderPrimitives.h ===================================================
//
// Moonlight client for PlayStation 4 — shared UI rendering primitives.
//
// This header centralizes the genuinely repeated low-level drawing operations
// used by the dedicated CPU renderers (HostList, HostGrid, AppList, AppGrid,
// Pairing, StreamExitHint, Settings, Modal, AddHost, Boot, StreamStatistics).
//
// It is deliberately NOT a UI framework. It only extracts the small set of
// drawing operations that are byte-for-byte identical across more than one
// renderer. Screen layout geometry, dirty-region semantics, selection-state
// interpretation, and screen-specific presentation decisions remain owned by
// each renderer.
//
// Ownership rules:
//   - Renderers own: screen layout, card positions, grid/crossbar geometry,
//     selection interpretation, dirty bounds, screen-specific presentation.
//   - Primitives own: only the common drawing arithmetic (atlas glyph source
//     rects, aspect-fit image placement, card focus outline, focused-card
//     footer, bottom-legend per-entry layout, centered/right-aligned text).
//
// These helpers are procedural and stateless. They never own the atlas image,
// the artwork image, or any retained resource; callers pass the images in.
//
// This header exposes only project-owned types (UiCompositor, CpuImage, Rect,
// FontRasterizer, UiAtlasIcon). No OpenOrbis SDK / GNMX types appear here.
// =============================================================================

#ifndef UI_UI_RENDER_PRIMITIVES_H
#define UI_UI_RENDER_PRIMITIVES_H

#include <cstddef>   // size_t
#include <cstdint>   // uint8_t
#include <string>    // std::string

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect
#include "platform/ps4/PngCpuDecode.h"   // ps4::CpuPngImage (runtime app artwork)
#include "platform/ps4/FontRasterizer.h" // ps4::FontRasterizer::FontFace
#include "UiAtlas.h"                     // ui::UiAtlasIcon

namespace ui {

// ---------------------------------------------------------------------------
// Atlas icon helpers
// ---------------------------------------------------------------------------

/// Resolve the accept (Enter) button icon from the system's Enter Button
/// Assignment. crossIsAccept=true → Cross; false → Circle.
UiAtlasIcon acceptIcon(bool crossIsAccept);

/// Resolve the back (Cancel) button icon from the system's Enter Button
/// Assignment. crossIsAccept=true → Circle; false → Cross.
UiAtlasIcon backIcon(bool crossIsAccept);

/// Draw an atlas glyph at `dst` (destination rectangle). Computes the source
/// rectangle from the icon's row/column in the 4×4 atlas grid. Does nothing if
/// `atlas` is empty. The caller owns the atlas image.
void drawAtlasIcon(ps4::UiCompositor& comp, const ps4::CpuImage& atlas,
                   UiAtlasIcon icon, const ps4::Rect& dst);

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

/// Draw 4 controller indicators in the top bar right area.
/// `controllerMask` is a bitmask where bit 0 is P1, bit 1 is P2, etc.
void drawControllerIndicators(ps4::UiCompositor& comp,
                              const ps4::CpuImage& controllerImg,
                              uint8_t controllerMask);

/// Draw `image` aspect-fit inside the destination rectangle `dstX`/`dstY`/
/// `dstW`/`dstH`, preserving aspect ratio and centering (letterbox/pillarbox,
/// no crop, no stretch). Does nothing if `image` is empty. Preserves the exact
/// centering/aspect-fit semantics of UiCompositor::aspectFit.
void drawAspectFitImage(ps4::UiCompositor& comp, const ps4::CpuImage& image,
                        int dstX, int dstY, int dstW, int dstH);

/// Overload for runtime app artwork (ps4::CpuPngImage). Structurally identical
/// to CpuImage; delegates to the same aspect-fit arithmetic. Does nothing if
/// `image` is empty.
void drawAspectFitImage(ps4::UiCompositor& comp, const ps4::CpuPngImage& image,
                        int dstX, int dstY, int dstW, int dstH);

/// Draw `image` aspect-cover inside the destination rectangle `dstX`/`dstY`/
/// `dstW`/`dstH`, center-cropping so the image completely fills the card
/// with no letterbox/pillarbox empty margins.
void drawAspectCoverImage(ps4::UiCompositor& comp, const ps4::CpuImage& image,
                          int dstX, int dstY, int dstW, int dstH);
void drawAspectCoverImage(ps4::UiCompositor& comp, const ps4::CpuPngImage& image,
                          int dstX, int dstY, int dstW, int dstH);

/// Draw the shared full-screen background image (1920×1080) at the origin.
/// Does nothing if `bg` is empty. Used by the screen renderers (Host/App
/// crossbar and grid, Pairing, Boot) as the common background; overlays do not
/// use it. The caller owns the image.
void drawBackground(ps4::UiCompositor& comp, const ps4::CpuImage& bg);

/// Overload for runtime app artwork (ps4::CpuPngImage). Structurally identical
/// to CpuImage; delegates to the same full-screen background draw. Does
/// nothing if `bg` is empty. The caller owns the image.
void drawBackground(ps4::UiCompositor& comp, const ps4::CpuPngImage& bg);

// ---------------------------------------------------------------------------
// Card drawing helpers
// ---------------------------------------------------------------------------

/// Draw the card focus outline: a 2px accent border when `focused`, otherwise
/// a 2px dark card-outline border. Used by the Host/App crossbar and grid
/// card renderers.
void drawCardOutline(ps4::UiCompositor& comp, int x, int y, int w, int h,
                     bool focused);

/// Draw the focused-card footer: a footer fill + accent border + a centered
/// caption (e.g. "Connect" / "Launch"). `left`/`top`/`w`/`h` are the card's
/// float rectangle; `footerHeight` is the footer height. `baselineY` and
/// `lineH` are the Regular-face layout baseline and the vertical center offset
/// used to vertically center the caption. The renderer chooses the caption.
void drawFocusFooter(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                     float left, float top, float w, float h,
                     float footerHeight, const char* caption,
                     float baselineY, float lineH);

// ---------------------------------------------------------------------------
// Bottom legend
// ---------------------------------------------------------------------------

/// One bottom-legend entry: an atlas glyph plus its label. The renderer decides
/// which entries exist; the primitive only draws them.
struct LegendEntry {
    UiAtlasIcon icon;
    const char* label;
};

/// Draw a bottom legend bar of `count` entries starting at `bounds`. Each entry
/// is drawn as an atlas glyph followed by its label, with the shared atlas
/// glyph size / glyph-to-label gap / entry gap. The renderer owns the legend
/// bounds and the entry list. Does nothing if `atlas` is empty.
void drawBottomLegend(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                      const ps4::CpuImage& atlas, const ps4::Rect& bounds,
                      const LegendEntry* entries, size_t count);

/// Draw a single bottom-legend entry (glyph + label) right-aligned to the
/// right edge of `bounds`, with the same right margin as the left content
/// start (UiTheme::kBottomBarContentX). Used to place a context action (e.g.
/// the Help entry) on the right side of the legend bar. Does nothing if
/// `atlas` is empty.
void drawBottomLegendRightAligned(ps4::UiCompositor& comp,
                                  ps4::FontRasterizer& fonts,
                                  const ps4::CpuImage& atlas,
                                  const ps4::Rect& bounds,
                                  UiAtlasIcon icon, const char* label);

// ---------------------------------------------------------------------------
// Inline-glyph text helpers
// ---------------------------------------------------------------------------

/// Returns true if `text` contains at least one inline `[GlyphName]` marker
/// (e.g. "[Options]", "[L3]", "[TouchpadR]").
bool textHasInlineGlyph(const char* text);

/// Draw a text line that may contain inline `[GlyphName]` markers, rendering
/// each marker as an atlas glyph. Returns the total advance width. When `draw`
/// is false, only measures (comp/atlas are unused). `baselineY` is the text
/// baseline; `glyphY` is the top of the glyph row. Glyphs are vertically
/// centered relative to the text line (matching the bottom-legend alignment).
float drawInlineGlyphText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                          const ps4::CpuImage& atlas,
                          ps4::FontRasterizer::FontFace face,
                          const char* text, float x, float baselineY,
                          float glyphY, uint8_t r, uint8_t g, uint8_t b,
                          bool draw);

// ---------------------------------------------------------------------------
// Text alignment helpers
// ---------------------------------------------------------------------------

/// Draw `text` horizontally centered within the box `boxX`..`boxX+boxW` at the
/// given `baselineY`. The caller supplies the vertical baseline (which may
/// already include vertical centering). Preserves exact horizontal centering.
void drawCenteredText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                      ps4::FontRasterizer::FontFace face, const char* text,
                      float boxX, float boxW, float baselineY,
                      uint8_t r, uint8_t g, uint8_t b);

/// Draw `text` right-aligned so its right edge ends at `rightX` at the given
/// `baselineY`. The caller supplies the vertical baseline.
void drawRightAlignedText(ps4::UiCompositor& comp, ps4::FontRasterizer& fonts,
                          ps4::FontRasterizer::FontFace face, const char* text,
                          float rightX, float baselineY,
                          uint8_t r, uint8_t g, uint8_t b);

// ---------------------------------------------------------------------------
// Text truncation helper
// ---------------------------------------------------------------------------

/// Truncate `text` to fit within `maxW` pixels in `face`, appending an ASCII
/// ellipsis ("...") when truncated. Returns the original text unchanged if it
/// already fits within `maxW`. The caller owns the max-width (card geometry)
/// decision and must pass the same face it will draw with, so the measurement
/// matches the rendered glyphs.
///
/// The ellipsis is the ASCII three-dot "..." rather than the Unicode "…"
/// (U+2026): the CPU FontRasterizer iterates text byte-by-byte with no UTF-8
/// decoding, so a multi-byte ellipsis would not render. This matches the
/// codebase's existing ASCII "..." convention.
std::string truncateTextWithEllipsis(ps4::FontRasterizer& fonts,
                                     ps4::FontRasterizer::FontFace face,
                                     const char* text, float maxW);

} // namespace ui

#endif // UI_UI_RENDER_PRIMITIVES_H

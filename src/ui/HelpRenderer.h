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

// === HelpRenderer.h ========================================================
//
// Moonlight client for PlayStation 4 — CPU Help screen renderer.
//
// Implements the HelpRenderer: draws the Help screen (background + top bar +
// center panel with in-stream button assignments and chords + bottom legend)
// into the shared CPU UiCompositor surface using only fillRect / strokeRect /
// drawImageSubRect / drawText / clipping. It consumes a plain HelpRenderState
// snapshot and owns the Help layout geometry and the retained CPU-decoded UI
// atlas.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_HELP_RENDERER_H
#define UI_HELP_RENDERER_H

#include <cstdint>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// HelpRenderState (CPU Help screen render snapshot)
// ---------------------------------------------------------------------------

/// Render snapshot for the CPU Help screen. A plain value copy consumed by
/// HelpRenderer. UiManager produces it; it is not an authoritative store.
/// The Help content is static; only the button-assignment glyphs vary.
struct HelpRenderState {
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// HelpRenderer
// ---------------------------------------------------------------------------

/// CPU Help screen renderer. Draws the Help screen into the CPU compositor.
class HelpRenderer {
public:
    HelpRenderer();
    ~HelpRenderer();
    HelpRenderer(const HelpRenderer&) = delete;
    HelpRenderer& operator=(const HelpRenderer&) = delete;
    HelpRenderer(HelpRenderer&&) = delete;
    HelpRenderer& operator=(HelpRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect bounds() const;
    ps4::Rect panelBounds() const;
    ps4::Rect topBarBounds() const;
    ps4::Rect bottomLegendBounds() const;

    void renderFull(const HelpRenderState& state, ps4::UiCompositor& comp);
    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    ps4::Rect panelRect() const;
    void drawPanel(const HelpRenderState& state, ps4::UiCompositor& comp);
    void drawTopBar(ps4::UiCompositor& comp);
    void drawSeparators(ps4::UiCompositor& comp);
    void drawLegend(const HelpRenderState& state, ps4::UiCompositor& comp);

    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_HELP_RENDERER_H

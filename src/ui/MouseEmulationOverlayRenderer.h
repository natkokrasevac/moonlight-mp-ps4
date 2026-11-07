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

// === MouseEmulationOverlayRenderer.h ========================================
//
// Moonlight client for PlayStation 4 — CPU in-stream mouse emulation state
// overlay renderer.
//
// Implements the MouseEmulationOverlayRenderer: draws the short-lived in-stream
// mouse emulation state overlay (semi-transparent dark panel + "Mouse
// Emulation: ON/OFF" text) into the shared CPU UiCompositor surface using only
// fillRect / drawText. It consumes a plain MouseEmulationRenderState snapshot
// and owns the overlay layout geometry.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_MOUSE_EMULATION_OVERLAY_RENDERER_H
#define UI_MOUSE_EMULATION_OVERLAY_RENDERER_H

#include <cstdint>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::Rect

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// MouseEmulationRenderState (CPU mouse emulation overlay render snapshot)
// ---------------------------------------------------------------------------

/// Render snapshot for the in-stream mouse emulation state overlay. A plain
/// value copy consumed by MouseEmulationOverlayRenderer. UiManager produces it;
/// it is not an authoritative store.
struct MouseEmulationRenderState {
    bool visible = false;   ///< Whether the overlay is currently shown.
    bool enabled = false;   ///< Whether mouse emulation is currently ON.
};

// ---------------------------------------------------------------------------
// MouseEmulationOverlayRenderer
// ---------------------------------------------------------------------------

/// CPU in-stream mouse emulation state overlay renderer.
class MouseEmulationOverlayRenderer {
public:
    MouseEmulationOverlayRenderer();
    ~MouseEmulationOverlayRenderer();
    MouseEmulationOverlayRenderer(const MouseEmulationOverlayRenderer&) = delete;
    MouseEmulationOverlayRenderer& operator=(const MouseEmulationOverlayRenderer&) = delete;
    MouseEmulationOverlayRenderer(MouseEmulationOverlayRenderer&&) = delete;
    MouseEmulationOverlayRenderer& operator=(MouseEmulationOverlayRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect bounds() const;
    void render(const MouseEmulationRenderState& state, ps4::UiCompositor& comp);

    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_atlasImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_MOUSE_EMULATION_OVERLAY_RENDERER_H

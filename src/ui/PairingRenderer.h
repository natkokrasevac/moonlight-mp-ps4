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

// === PairingRenderer.h =====================================================
//
// Moonlight client for PlayStation 4 — CPU Pairing screen renderer.
//
// PairingRenderer draws the Pairing screen (background + center panel + host
// name + pairing instructions + PIN display + status message + bottom legend)
// into the shared CPU UiCompositor surface. It consumes a plain
// PairingRenderState snapshot produced by UiManager and owns the Pairing
// layout geometry and the retained CPU-decoded UI atlas.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_PAIRING_RENDERER_H
#define UI_PAIRING_RENDERER_H

#include <cstdint>
#include <string>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// PairingRenderState (CPU Pairing screen render snapshot)
// ---------------------------------------------------------------------------

/// Presentation states for the CPU Pairing screen. A plain value copy consumed
/// by PairingRenderer. UiManager produces it; it is not an authoritative store.
struct PairingRenderState {
    /// Presentation states for the Pairing screen.
    enum class State : uint8_t {
        Preparing,      ///< Pairing operation starting (no PIN yet).
        WaitingForPin,  ///< PIN ready and displayed; waiting for user entry.
        Pairing,        ///< Protocol stage progress.
        Saving,         ///< Persisting trust.
        Confirming,     ///< Confirming pairing result.
        Succeeded,      ///< Pairing succeeded.
        Failed,         ///< Pairing failed.
        Cancelled       ///< Pairing cancelled.
    };

    State state = State::Preparing;
    std::string hostName;          ///< Host display name.
    std::string pin;               ///< PIN to display (empty until PinReady).
    std::string message;           ///< Status message text.
    bool crossIsAccept = true;     ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// PairingRenderer
// ---------------------------------------------------------------------------

/// CPU Pairing screen renderer. Draws the Pairing screen into the shared CPU
/// UiCompositor surface.
class PairingRenderer {
public:
    PairingRenderer();
    ~PairingRenderer();
    PairingRenderer(const PairingRenderer&) = delete;
    PairingRenderer& operator=(const PairingRenderer&) = delete;
    PairingRenderer(PairingRenderer&&) = delete;
    PairingRenderer& operator=(PairingRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect bounds() const;
    ps4::Rect panelBounds() const;
    ps4::Rect pinBounds() const;
    ps4::Rect topBarBounds() const;
    ps4::Rect bottomLegendBounds() const;

    void renderFull(const PairingRenderState& state, ps4::UiCompositor& comp);

    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    ps4::Rect panelRect() const;
    ps4::Rect pinRect() const;
    void drawPanel(const PairingRenderState& state, ps4::UiCompositor& comp);
    void drawTopBar(ps4::UiCompositor& comp);
    void drawSeparators(ps4::UiCompositor& comp);
    void drawLegend(const PairingRenderState& state, ps4::UiCompositor& comp);

    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_atlasImg;
    ps4::CpuImage m_bgImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_PAIRING_RENDERER_H

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

// === AddHostRenderer.h ======================================================
//
// OpenOrbis Moonlight client — CPU Add Host overlay / status renderer.
//
// Consumes a plain `AddHostRenderState` snapshot produced by UiManager and
// draws the Add Host overlay (form) and the Add Host status modal (Checking /
// Found / Failed) into the shared CPU UiCompositor surface, which is then
// presented through the UiBlitter. It is deliberately NOT a generic
// widget/scene-graph/layout framework.
//
// Ownership model:
//   UiManager       : state, navigation, intents, Add Host flow state, field
//                     text, selection, checking/found/error state.
//   AddHostRenderer : presentation only (layout + drawing into UiCompositor).
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_ADD_HOST_RENDERER_H
#define UI_ADD_HOST_RENDERER_H

#include <cstdint>
#include <string>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::Rect
#include "UiManager.h"                   // ui::AddHostRenderState, ui::AddHostPresentationState

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// AddHostRenderer
// ---------------------------------------------------------------------------

/// CPU Add Host overlay / status renderer. Owns the Add Host layout geometry
/// and draws the Add Host overlay and status modal into a UiCompositor surface
/// using only fillRect / strokeRect / drawText / clipping.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class AddHostRenderer {
public:
    /// Constructor performs no fallible operations.
    AddHostRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~AddHostRenderer();

    // Non-copyable, non-movable.
    AddHostRenderer(const AddHostRenderer&) = delete;
    AddHostRenderer& operator=(const AddHostRenderer&) = delete;
    AddHostRenderer(AddHostRenderer&&) = delete;
    AddHostRenderer& operator=(AddHostRenderer&&) = delete;

    /// Retain the FontRasterizer reference for text drawing. Returns true on
    /// success. On failure the object remains safe to shut down or destroy.
    bool initialize(ps4::FontRasterizer* fonts);

    /// Release retained resources. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Layout / visual bounds helpers ------------------------------------
    // These are the single source of Add Host visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the full-screen dim layer.
    ps4::Rect dimBounds() const;

    /// Screen-space bounds of the centered panel for the given state.
    ps4::Rect panelBounds(const AddHostRenderState& state) const;

    /// Screen-space bounds of the Add Host overlay for the given state. This
    /// is the union of the dim layer and the panel (the dim layer covers the
    /// full screen, so this is the full screen). Used for dirty-region
    /// invalidation on state changes.
    ps4::Rect bounds(const AddHostRenderState& state) const;

    // --- Form overlay layout bounds (state == Form) ------------------------
    // These are the single source of the Add Host form's row/button geometry,
    // so dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the Type toggle row (Form state).
    ps4::Rect typeRowBounds() const;

    /// Screen-space bounds of the IP Address / Hostname row (Form state).
    ps4::Rect ipRowBounds() const;

    /// Screen-space bounds of the Port row (Form state).
    ps4::Rect portRowBounds() const;

    /// Screen-space bounds of the inline validation-error region (Form state).
    ps4::Rect validationBounds() const;

    /// Screen-space bounds of the Cancel/Add button row (Form state).
    ps4::Rect buttonsBounds() const;

    // --- Status modal layout bounds (state == Checking/Found/Failed) -------

    /// Screen-space bounds of the status message region (status modal).
    ps4::Rect statusMessageBounds() const;

    /// Screen-space bounds of the status action button (status modal).
    ps4::Rect statusButtonBounds() const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the Add Host overlay / status modal (dim + panel +
    /// content) for the given state into `comp`. Used when the overlay opens
    /// or its state changes.
    void renderFull(const AddHostRenderState& state, ps4::UiCompositor& comp);

private:
    /// Draw the Add Host form overlay (IP/Port entry) into `comp`.
    void drawFormOverlay(const AddHostRenderState& state,
                         ps4::UiCompositor& comp);

    /// Draw the Add Host status modal (Checking / Found / Failed) into `comp`.
    void drawStatusModal(const AddHostRenderState& state,
                         ps4::UiCompositor& comp);

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_ADD_HOST_RENDERER_H

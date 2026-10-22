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

// === ModalRenderer.h ========================================================
//
// OpenOrbis Moonlight client — CPU confirmation modal renderer.
//
// Consumes a plain `ModalRenderState` snapshot produced by UiManager and
// draws the confirmation modals (Delete Host, Quit App) into the shared CPU
// UiCompositor surface, which is then presented through the UiBlitter. It is
// deliberately NOT a generic widget/scene-graph/layout framework.
//
// Ownership model:
//   UiManager      : state, navigation, intents, modal open/closed, Yes/No
//                    selection.
//   ModalRenderer  : presentation only (layout + drawing into UiCompositor).
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_MODAL_RENDERER_H
#define UI_MODAL_RENDERER_H

#include <cstdint>
#include <string>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::Rect
#include "UiManager.h"                   // ui::ModalRenderState, ui::ModalKind

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// ModalRenderer
// ---------------------------------------------------------------------------

/// CPU confirmation modal renderer. Owns the modal layout geometry and draws
/// the confirmation modals into a UiCompositor surface using only fillRect /
/// strokeRect / drawText / clipping.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class ModalRenderer {
public:
    /// Constructor performs no fallible operations.
    ModalRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~ModalRenderer();

    // Non-copyable, non-movable.
    ModalRenderer(const ModalRenderer&) = delete;
    ModalRenderer& operator=(const ModalRenderer&) = delete;
    ModalRenderer(ModalRenderer&&) = delete;
    ModalRenderer& operator=(ModalRenderer&&) = delete;

    /// Retain the FontRasterizer reference for text drawing. Returns true on
    /// success. On failure the object remains safe to shut down or destroy.
    bool initialize(ps4::FontRasterizer* fonts);

    /// Release retained resources. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Layout / visual bounds helpers ------------------------------------
    // These are the single source of modal visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the full-screen dim layer.
    ps4::Rect dimBounds() const;

    /// Screen-space bounds of the centered modal panel for the given kind.
    ps4::Rect panelBounds(ModalKind kind) const;

    /// Screen-space bounds of the choice button(s) for the given kind. For
    /// DeleteHost and QuitApp this is the bounding box of the No/Yes buttons.
    ps4::Rect choiceBounds(ModalKind kind) const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the modal (dim + panel + content) for the given state into
    /// `comp`. Used when the modal opens.
    void renderFull(const ModalRenderState& state, ps4::UiCompositor& comp);

private:
    /// Draw the Delete Host modal content into `comp` (panel already drawn).
    void drawDeleteHostContent(const ModalRenderState& state,
                               ps4::UiCompositor& comp);

    /// Draw the Delete Host No/Yes choice buttons into `comp`.
    void drawDeleteHostChoices(const ModalRenderState& state,
                               ps4::UiCompositor& comp);

    /// Draw the Quit App modal content into `comp` (panel already drawn).
    void drawQuitAppContent(const ModalRenderState& state,
                            ps4::UiCompositor& comp);

    /// Draw the Quit App No/Yes choice buttons into `comp`.
    void drawQuitAppChoices(const ModalRenderState& state,
                            ps4::UiCompositor& comp);

    /// Draw the Reset Moonlight modal content into `comp` (panel already drawn).
    void drawResetMoonlightContent(const ModalRenderState& state,
                                   ps4::UiCompositor& comp);

    /// Draw the Reset Moonlight No/Yes choice buttons into `comp`.
    void drawResetMoonlightChoices(const ModalRenderState& state,
                                   ps4::UiCompositor& comp);

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_MODAL_RENDERER_H

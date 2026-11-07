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

// === UiRenderDispatcher.h ===================================================
//
// OpenOrbis Moonlight client — CPU UI render dispatcher.
//
// UiRenderDispatcher owns the CPU UI render dispatch logic that was previously
// embedded in MoonlightApp::render(). It decides which screen renderer to
// invoke, detects when the UI content changed, and performs a full recompose
// (clear + renderFull base + renderFull any open overlay) whenever anything
// changes.
//
// This class is deliberately a helper: MoonlightApp remains the coordinator
// and owns the renderers + compositor, passing pointers to this dispatcher.
// The dispatcher holds only the render-dispatch state (prev-state snapshots)
// and the logic to decide when to recompose.
//
// With the GPU UI path (GpuUiRenderer) doing full redraws on change, and the
// dim layer removed from modals/settings, the dispatcher uses a simple model:
//   - On first entry into a screen, or when the base state or any open overlay
//     changes, do a full recompose (clear + renderFull base + renderFull
//     overlays).
//   - Otherwise, do nothing.
//
// The dispatcher returns whether the UI surface changed so the caller
// (MoonlightApp) can re-stage it into the GpuUiRenderer.
// ===============================================================================

#ifndef UI_RENDER_DISPATCHER_H
#define UI_RENDER_DISPATCHER_H

#include <cstdint>
#include <unordered_map>

#include "moonlight/ApplicationCatalog.h"  // ml::ApplicationId
#include "platform/ps4/PngCpuDecode.h"     // ps4::CpuPngImage

namespace ps4 {
class UiCompositor;
}

namespace ml {
enum class PresentationStyle : uint8_t;
}

namespace ui {

class UiManager;
class BootRenderer;
class HostListRenderer;
class HostGridRenderer;
class PairingRenderer;
class HelpRenderer;
class AppListRenderer;
class AppGridRenderer;
class SettingsRenderer;
class ModalRenderer;
class AddHostRenderer;

// Render state types (forward declared; full defs in the renderer headers).
struct BootRenderState;
struct PairingRenderState;
struct HelpRenderState;
struct HostListRenderState;
struct HostGridRenderState;
struct AppListRenderState;
struct AppGridRenderState;
struct SettingsRenderState;
struct ModalRenderState;
struct AddHostRenderState;

// ---------------------------------------------------------------------------
// UiRenderContext — MoonlightApp-specific data the dispatcher needs
// ---------------------------------------------------------------------------

/// Context passed to UiRenderDispatcher::render() each frame. Provides
/// MoonlightApp-owned data (runtime app artwork) that the dispatcher needs to
/// populate App List / App Grid render states.
struct UiRenderContext {
    /// Runtime artwork for the active catalog, keyed by ApplicationId.
    /// Null means no artwork is available (renderers fall back to the default
    /// app image).
    const std::unordered_map<ml::ApplicationId, ps4::CpuPngImage>* artwork =
        nullptr;
};

// ---------------------------------------------------------------------------
// UiRenderDispatcher
// ---------------------------------------------------------------------------

/// Owns the CPU UI render dispatch logic.
///
/// Main-thread owned. MoonlightApp owns the renderers and compositor and
/// passes pointers to this dispatcher. The dispatcher holds the prev-state
/// snapshots and the recompose-if-changed logic.
class UiRenderDispatcher {
public:
    /// Constructor performs no fallible operations.
    UiRenderDispatcher();

    /// Destructor must not throw.
    ~UiRenderDispatcher();

    // Non-copyable, non-movable.
    UiRenderDispatcher(const UiRenderDispatcher&) = delete;
    UiRenderDispatcher& operator=(const UiRenderDispatcher&) = delete;
    UiRenderDispatcher(UiRenderDispatcher&&) = delete;
    UiRenderDispatcher& operator=(UiRenderDispatcher&&) = delete;

    /// Set the compositor and renderers. Called once during MoonlightApp
    /// initialization after all renderers are initialized. Pointers are
    /// non-owning and must outlive this dispatcher.
    void setRenderers(ps4::UiCompositor* comp,
                      BootRenderer* boot,
                      PairingRenderer* pairing,
                      HelpRenderer* help,
                      HostListRenderer* hostList,
                      HostGridRenderer* hostGrid,
                      AppListRenderer* appList,
                      AppGridRenderer* appGrid,
                      SettingsRenderer* settings,
                      ModalRenderer* modal,
                      AddHostRenderer* addHost);

    /// Render the current UI screen into the compositor. Called each frame
    /// from MoonlightApp::render() (non-stream path).
    ///
    /// `ui` is the UiManager used to build render states and query overlay
    /// open states. `ctx` provides MoonlightApp-owned data (artwork).
    /// `width`/`height` are the compositor surface dimensions.
    ///
    /// Returns true if the UI surface changed and must be re-staged into the
    /// GpuUiRenderer.
    bool render(UiManager& ui, const UiRenderContext& ctx,
                uint32_t width, uint32_t height);

private:
    // --- Renderer references (non-owning) ----------------------------------
    ps4::UiCompositor* m_comp = nullptr;
    BootRenderer*      m_boot = nullptr;
    PairingRenderer*   m_pairing = nullptr;
    HelpRenderer*      m_help = nullptr;
    HostListRenderer*  m_hostList = nullptr;
    HostGridRenderer*  m_hostGrid = nullptr;
    AppListRenderer*   m_appList = nullptr;
    AppGridRenderer*   m_appGrid = nullptr;
    SettingsRenderer*  m_settings = nullptr;
    ModalRenderer*     m_modal = nullptr;
    AddHostRenderer*   m_addHost = nullptr;

    // --- Fallback background color -----------------------------------------
    uint8_t m_bgR = 0x40;
    uint8_t m_bgG = 0x40;
    uint8_t m_bgB = 0x40;

    // --- Prev-state snapshots (change detection) ---------------------------
    bool m_bootComposed = false;

    bool m_prevHostListValid = false;
    HostListRenderState m_prevHostListState;
    bool m_prevHostGridValid = false;
    HostGridRenderState m_prevHostGridState;

    bool m_prevPairingValid = false;
    PairingRenderState m_prevPairingState;
    bool m_prevHelpValid = false;
    HelpRenderState m_prevHelpState;

    bool m_prevAppListValid = false;
    AppListRenderState m_prevAppListState;
    bool m_prevAppGridValid = false;
    AppGridRenderState m_prevAppGridState;

    bool m_prevSettingsOpen = false;
    SettingsRenderState m_prevSettingsState;
    bool m_prevDeleteConfirmOpen = false;
    ModalRenderState m_prevDeleteModalState;
    bool m_prevQuitAppOpen = false;
    ModalRenderState m_prevQuitModalState;
    bool m_prevResetConfirmOpen = false;
    ModalRenderState m_prevResetModalState;
    bool m_prevAddHostOpen = false;
    AddHostRenderState m_prevAddHostState;

    // --- Internal helpers ---------------------------------------------------
    /// Populate runtime artwork into an App List render state.
    void populateAppListArtwork(AppListRenderState& state,
                                const UiRenderContext& ctx) const;
    /// Populate runtime artwork into an App Grid render state.
    void populateAppGridArtwork(AppGridRenderState& state,
                                const UiRenderContext& ctx) const;

    /// Compose the Settings overlay if open (or closing). Returns true if the
    /// surface changed.
    bool composeSettings(UiManager& ui, uint32_t width, uint32_t height);

    /// Compose the Delete Host confirmation modal if open (or closing).
    bool composeDeleteModal(UiManager& ui, uint32_t width, uint32_t height);

    /// Compose the Quit App confirmation modal if open (or closing).
    bool composeQuitModal(UiManager& ui, uint32_t width, uint32_t height);

    /// Compose the Reset Moonlight confirmation modal if open (or closing).
    bool composeResetModal(UiManager& ui, uint32_t width, uint32_t height);

    /// Compose the Add Host overlay / status modal if open (or closing).
    bool composeAddHost(UiManager& ui, uint32_t width, uint32_t height);
};

} // namespace ui

#endif // UI_RENDER_DISPATCHER_H

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

// === UiManager.h =============================================================
//
// Moonlight client for PlayStation 4 — UI Manager / Basic UI Flow
//
// UiManager is the application-level UI component. It owns UI presentation
// state, screen-local selection, and UI fonts. It receives normalized input
// and emits user intents back to MoonlightApp.
//
// This header exposes only project-owned types. OpenOrbis SDK types, GNMX types,
// ScePadData, and graphics allocator types must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef UI_UI_MANAGER_H
#define UI_UI_MANAGER_H

#include <cstdint>
#include <vector>

#include "moonlight/HostManager.h"   // ml::Host, ml::HostId, ml::HostAvailability
#include "moonlight/ApplicationCatalog.h"  // ml::ApplicationInfo, ml::ApplicationCatalogState, ml::ApplicationId
#include "moonlight/SettingsManager.h"  // ml::PresentationStyle

#include "HostListRenderer.h"  // ui::HostListRenderState (CPU Host List render snapshot)
#include "HostGridRenderer.h"  // ui::HostGridRenderState (CPU Host List Grid render snapshot)
#include "AppListRenderer.h"   // ui::AppListRenderState (CPU App List render snapshot)
#include "AppGridRenderer.h"   // ui::AppGridRenderState (CPU App List Grid render snapshot)
#include "PairingRenderer.h"   // ui::PairingRenderState (CPU Pairing render snapshot)
#include "HelpRenderer.h"      // ui::HelpRenderState (CPU Help render snapshot)
#include "StreamExitHintRenderer.h"  // ui::StreamExitHintRenderState (CPU exit hint render snapshot)
#include "StreamStatisticsRenderer.h"  // ui::StreamStatisticsRenderState (CPU statistics render snapshot)
#include "MouseEmulationOverlayRenderer.h"  // ui::MouseEmulationRenderState (CPU mouse emulation overlay render snapshot)

// Forward declaration of the CPU Host List renderer and its snapshot.
namespace ui {
class HostListRenderer;
struct HostListRenderState;
class AppListRenderer;
struct AppListRenderState;
class SettingsRenderer;
struct SettingsRenderState;
class ModalRenderer;
struct ModalRenderState;
class AddHostRenderer;
struct AddHostRenderState;
class BootRenderer;
struct BootRenderState;
class PairingRenderer;
struct PairingRenderState;
class HelpRenderer;
struct HelpRenderState;
class StreamExitHintRenderer;
struct StreamExitHintRenderState;
class StreamStatisticsRenderer;
struct StreamStatisticsRenderState;
class MouseEmulationOverlayRenderer;
struct MouseEmulationRenderState;
class HostGridRenderer;
struct HostGridRenderState;
class AppGridRenderer;
struct AppGridRenderState;
}

namespace ui {

// ---------------------------------------------------------------------------
// UI screen presentation states
// ---------------------------------------------------------------------------

/// The current UI screen. UiManager owns the authoritative screen state;
/// MoonlightApp reads it (via currentScreen()) to dispatch to the correct CPU
/// renderer. The CPU compositor/blitter is the only active UI path, so every
/// screen is rendered through the CPU renderers.
enum class UiScreen : uint8_t {
    Boot,
    HostList,
    Pairing,
    AppList,
    Help,
    StreamExitHint,
    StreamEmpty,
    StreamStatistics
};

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of UI-operation results.
enum class UiResultCategory : uint8_t {
    Success,
    AlreadyInitialized,
    InvalidLifecycleState,
    FontLoadFailure
};

/// A lightweight result returned by UI lifecycle operations.
struct UiResult {
    UiResultCategory category;
    uint32_t         nativeError;   ///< Reserved for diagnostics (0 when none).
    // Implicit success.
    UiResult() : category(UiResultCategory::Success), nativeError(0) {}
    UiResult(UiResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == UiResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// Normalized UI input
// ---------------------------------------------------------------------------

/// Application-facing normalized navigation/action input.
/// All fields are edge-triggered (just-pressed) booleans produced by the
/// platform InputManager. UiManager must never read raw PS4 pad state.
struct UiInput {
    bool upPressed;
    bool downPressed;
    bool leftPressed;
    bool rightPressed;
    bool acceptPressed;
    bool backPressed;
    bool trianglePressed;
    bool optionsPressed;
    bool squarePressed;
    bool touchpadPressed;

    UiInput()
        : upPressed(false), downPressed(false), leftPressed(false),
          rightPressed(false), acceptPressed(false), backPressed(false),
          trianglePressed(false), optionsPressed(false), squarePressed(false),
          touchpadPressed(false) {}
};

// ---------------------------------------------------------------------------
// SettingsViewState
// ---------------------------------------------------------------------------

/// Minimal UI data contract for the Settings overlay.
struct SettingsViewState {
    int  resolutionIndex = 1;   ///< 0 = 720p, 1 = 1080p.
    int  frameRateIndex = 1;    ///< 0 = 30 FPS, 1 = 60 FPS.
    bool quitAppOnStreamExit = false;
    /// Presentation style index: 0 = PlayStation, 1 = Grid.
    int  presentationStyleIndex = 0;
    /// Stream Statistics toggle (Off/On).
    bool streamStatisticsEnabled = false;
    /// Host Speakers (Muted/Unmuted).
    bool muteHostSpeakers = true;
    /// Game Optimization (ON/OFF).
    bool optimizeGameSettings = true;
    /// Show "Quit App" dialog (ON/OFF). When OFF, quitting an app (Triangle)
    /// or quitting-and-launching a different app proceeds without a dialog.
    bool showQuitAppDialog = true;
};

// ---------------------------------------------------------------------------
// SettingsRenderState (CPU Settings overlay render snapshot)
// ---------------------------------------------------------------------------

/// Render data for one Settings row.
struct SettingsRowRenderData {
    std::string label;
    std::string value;
    bool selected = false;
    bool enabled = true;   ///< Disabled rows render dimmed and cannot be selected.
};

/// Render snapshot for the CPU Settings overlay.
struct SettingsRenderState {
    std::vector<SettingsRowRenderData> rows;
    int selectedIndex = -1;      ///< Index into `rows` (-1 = none).
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// ModalRenderState (CPU confirmation modal render snapshot)
// ---------------------------------------------------------------------------

/// The kind of confirmation modal to render.
enum class ModalKind : uint8_t {
    DeleteHost,           ///< Delete Host confirmation (No/Yes choices).
    QuitApp,              ///< Quit-app confirmation (No/Yes choices).
    ResetMoonlight        ///< Reset Moonlight confirmation (No/Yes choices).
};

/// Render snapshot for a CPU confirmation modal.
struct ModalRenderState {
    ModalKind kind = ModalKind::DeleteHost;
    std::string title;       ///< Modal title.
    std::string message;     ///< Explanatory message.
    std::string message2;    ///< Optional second message line (ResetMoonlight only).
    std::string hostName;    ///< Host display name (DeleteHost only).
    bool yesSelected = false; ///< Selected option (No/Yes modals only; false = No).
    bool crossIsAccept = true; ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// AddHostRenderState (CPU Add Host overlay / status render snapshot)
// ---------------------------------------------------------------------------

/// Presentation states for the CPU Add Host overlay / status modal.
enum class AddHostPresentationState : uint8_t {
    None,       ///< No Add Host overlay is open.
    Form,       ///< Add Host form overlay (IP/Port entry, before IME).
    Checking,   ///< "Checking host..." status modal.
    Found,      ///< "Found host" confirmation status modal.
    Failed      ///< Failure/error status modal.
};

/// Render snapshot for the CPU Add Host overlay and status modal.
struct AddHostRenderState {
    AddHostPresentationState state = AddHostPresentationState::None;

    // --- Form overlay fields (state == Form) -------------------------------
    std::string ipText;            ///< IP Address field text.
    std::string portText;          ///< Port field text.
    std::string validationError;   ///< Inline validation error (empty = none).
    int selectedField = 0;         ///< 0=IP, 1=Port, 2=Cancel, 3=Add.
    bool imeActive = false;        ///< PS4 ImeDialog is open for a field.

    // --- Status modal fields (state == Checking/Found/Failed) --------------
    std::string hostAddress;       ///< Entered IP (for status display).
    uint16_t hostPort = 0;         ///< Entered port (for status display).
    std::string hostName;          ///< Found host display name (Found state).
    std::string message;           ///< Status message text.

    // --- Common ------------------------------------------------------------
    bool crossIsAccept = true;     ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// BootRenderState (CPU Boot screen render snapshot)
// ---------------------------------------------------------------------------

/// Render snapshot for the CPU Boot screen. A plain value copy consumed by
/// BootRenderer. UiManager produces it; it is not an authoritative store.
/// UiManager remains authoritative for boot state, elapsed time, transition
/// to Host List, and any startup status strings.
struct BootRenderState {
    float elapsedSeconds = 0.0f;   ///< Elapsed Boot screen time (seconds).
    std::string title;             ///< Boot screen title text.
    std::string subtitle;          ///< Optional status/subtitle text (empty = none).
};

// ---------------------------------------------------------------------------
// UI intents
// ---------------------------------------------------------------------------

/// Categories of user intents emitted by UiManager.
enum class UiIntentType : uint8_t {
    None,
    ActivateHost,
    CancelPairing,
    Back,
    ActivateApplication,
    QuitApplication,
    QuitAndLaunchApplication,
    OpenSettings,
    CloseSettings,
    RequestDeleteHost,
    RequestAddHost,
    CancelAddHost,
    RequestResetMoonlight
};

/// A user intent emitted by UiManager and consumed by MoonlightApp.
struct UiIntent {
    UiIntentType type;
    ml::HostId   hostId;   ///< Stable HostId (0 = none/invalid).
    ml::ApplicationId applicationId;   ///< Stable ApplicationId (0 = none/invalid).
    ml::ApplicationId launchApplicationId = 0;  ///< App to launch after quit (QuitAndLaunchApplication).
    SettingsViewState settings;   ///< Final settings on CloseSettings.
    std::string  ipv4;      ///< Manual host IPv4 address (RequestAddHost).
    uint16_t     port = 0;  ///< Manual host bootstrap port (RequestAddHost).

    UiIntent() : type(UiIntentType::None), hostId(0), applicationId(0) {}
    UiIntent(UiIntentType t, ml::HostId id = 0, ml::ApplicationId appId = 0)
        : type(t), hostId(id), applicationId(appId) {}
};

// ---------------------------------------------------------------------------
// PairingViewState
// ---------------------------------------------------------------------------

/// Presentation states for the Pairing screen.
enum class PairingPresentationState : uint8_t {
    Preparing,
    WaitingForUser,
    Pairing,
    Saving,
    Confirming,
    Succeeded,
    Failed,
    Cancelled
};

/// Minimal UI data contract for the Pairing screen.
struct PairingViewState {
    ml::HostId hostId = 0;
    std::string hostName;
    std::string pin;                       ///< Empty until PinReady.
    PairingPresentationState state = PairingPresentationState::Preparing;
    std::string statusText;
};

// ---------------------------------------------------------------------------
// ApplicationListView
// ---------------------------------------------------------------------------

/// Minimal UI data contract for the App List screen.
struct ApplicationListView {
    ml::HostId hostId = 0;
    std::string hostName;
    ml::ApplicationCatalogState state = ml::ApplicationCatalogState::Idle;
    std::vector<ml::ApplicationInfo> applications;
    std::string statusText;
};

// ---------------------------------------------------------------------------
// LaunchPresentationState
// ---------------------------------------------------------------------------

/// Presentation states for the App List launch-progress overlay.
enum class LaunchPresentationState : uint8_t {
    Launching,
    Resuming,
    Succeeded,
    Failed,
    Cancelled
};

// ---------------------------------------------------------------------------
// ApplicationLaunchViewState
// ---------------------------------------------------------------------------

/// Minimal UI data contract for the App List launch-progress overlay.
struct ApplicationLaunchViewState {
    bool active = false;
    ml::ApplicationId applicationId = 0;
    LaunchPresentationState state = LaunchPresentationState::Launching;
    std::string statusText;
};

// ---------------------------------------------------------------------------
// QuitPresentationState
// ---------------------------------------------------------------------------

/// Presentation states for the App List quit-progress status line.
enum class QuitPresentationState : uint8_t {
    Idle,
    Quitting,
    Succeeded,
    Failed
};

// ---------------------------------------------------------------------------
// ApplicationQuitViewState
// ---------------------------------------------------------------------------

/// Minimal UI data contract for the App List quit-progress status line.
struct ApplicationQuitViewState {
    bool active = false;
    ml::ApplicationId applicationId = 0;
    QuitPresentationState state = QuitPresentationState::Idle;
    std::string statusText;
};

// ---------------------------------------------------------------------------
// StreamPresentationState
// ---------------------------------------------------------------------------

/// Presentation states for the Stream screen.
enum class StreamPresentationState : uint8_t {
    Connecting,
    Streaming,
    Stopping,
    Failed
};

// ---------------------------------------------------------------------------
// StreamViewState
// ---------------------------------------------------------------------------

/// Minimal UI data contract for the Stream screen.
struct StreamViewState {
    ml::HostId hostId = 0;
    ml::ApplicationId applicationId = 0;
    StreamPresentationState state = StreamPresentationState::Connecting;
    std::string statusText;
};

// ---------------------------------------------------------------------------
// StreamStatisticsSnapshot
// ---------------------------------------------------------------------------

/// The 1-second display snapshot for the in-stream statistics overlay.
struct StreamStatisticsSnapshot {
    bool valid = false;
    int  width = 0;                ///< Actual stream width (pixels).
    int  height = 0;               ///< Actual stream height (pixels).
    float fps = 0.0f;              ///< Measured decoded FPS (interval average).
    float decodeMs = 0.0f;         ///< Average decode time (ms, interval).
    float bitrateMbps = 0.0f;      ///< Measured encoded-video bitrate (Mbps, interval).
    float rttMs = 0.0f;            ///< Estimated host round-trip time (ms).
    float jitterMs = 0.0f;         ///< Estimated RTT variance / jitter (ms).
    float frameDecodeMs = 0.0f;    ///< Decode time of the most-recent presented frame (ms).
    float framePresentMs = 0.0f;   ///< NV12 conversion + overlay blend time (ms).
    float frameTotalMs = 0.0f;     ///< Total frame time = decode + presentation (ms).
};

// ---------------------------------------------------------------------------
// UI sound intents
// ---------------------------------------------------------------------------

/// Categories of UI navigation sound cues emitted by UiManager.
enum class UiSoundIntent : uint8_t {
    None,          ///< No UI sound this update.
    Cursor,        ///< UI selection/focus actually moved.
    Accept,        ///< A UI action was successfully accepted/activated.
    Back,          ///< A Back/Cancel action was successfully performed.
    SettingsOpen,  ///< The Settings overlay was opened.
    SettingsClose  ///< The Settings overlay was closed.
};

// ---------------------------------------------------------------------------
// UiManager — application-level UI component
// ---------------------------------------------------------------------------

/// Owns UI presentation state, screen-local selection, and UI fonts.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class UiManager {
public:
    /// Constructor performs no fallible operations.
    UiManager();

    /// Destructor must not throw.
    ~UiManager();

    // Non-copyable, non-movable.
    UiManager(const UiManager&) = delete;
    UiManager& operator=(const UiManager&) = delete;
    UiManager(UiManager&&) = delete;
    UiManager& operator=(UiManager&&) = delete;

    /// Initialize UI presentation state.
    UiResult initialize();

    /// Release UI-owned presentation resources.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Provide the current read-only host snapshot for presentation.
    void setHosts(const std::vector<ml::Host>& hosts);

    /// Provide the current Pairing screen view state.
    void setPairingView(const PairingViewState& view);

    /// Provide the current App List screen view state.
    void setApplicationListView(const ApplicationListView& view);

    /// Provide the current App List launch-progress view state.
    void setApplicationLaunchView(const ApplicationLaunchViewState& view);

    /// Provide the current App List quit-progress view state.
    void setApplicationQuitView(const ApplicationQuitViewState& view);

    /// Provide the current Stream screen view state.
    void setStreamView(const StreamViewState& view);

    /// Provide the current Settings overlay view state.
    void setSettingsView(const SettingsViewState& view);

    /// Returns true while the Settings overlay is open.
    bool isSettingsOpen() const;

    /// Returns true while the Help screen is open.
    bool isHelpOpen() const;

    /// Close the Help screen and return to the screen it was opened from.
    /// Used by the persistent-storage reset flow (after a reset the client
    /// returns to the Host List). No-op when Help is not open.
    void closeHelp();

    /// Returns the current Settings working-copy values.
    SettingsViewState settingsViewSnapshot() const;

    /// Returns true while the Delete Host confirmation dialog is open.
    bool isDeleteConfirmOpen() const;

    /// Returns true while the Reset Moonlight confirmation dialog is open.
    bool isResetConfirmOpen() const;

    /// Open the Reset Moonlight confirmation dialog (defaults to No).
    void openResetConfirm();

    /// Set the system's Enter Button Assignment.
    void setButtonAssignment(bool crossIsAccept);

    /// Provide the console's own IPv4 address. Used to prepopulate the Add
    /// Host overlay's IP field so entering a host on the same network is
    /// easier. Empty string disables prepopulation.
    void setLocalIpAddress(const std::string& ip);

    /// Set the presentation style used to render the Host List and App List.
    void setPresentationStyle(ml::PresentationStyle style);

    /// Returns the current presentation style.
    ml::PresentationStyle presentationStyle() const;

    /// Set whether the "Quit App" confirmation dialog is shown before quitting
    /// an app (Triangle) or before quitting-and-launching a different app.
    /// When false, quitting proceeds immediately without a dialog.
    void setShowQuitAppDialog(bool show);

    /// Set the current grid column counts for the Host List and App List grid
    /// presentation modes.
    void setGridColumns(int hostColumns, int appColumns);

    /// Returns the current UI screen.
    UiScreen currentScreen() const;

    // --- CPU render snapshot builders --------------------------------------

    /// Build the current Host List render snapshot.
    bool buildHostListRenderState(HostListRenderState& out) const;

    /// Build the current Host List Grid render snapshot.
    bool buildHostGridRenderState(HostGridRenderState& out) const;

    /// Build the current App List render snapshot.
    bool buildAppListRenderState(AppListRenderState& out) const;

    /// Build the current App List Grid render snapshot.
    bool buildAppGridRenderState(AppGridRenderState& out) const;

    // --- Settings / Modal render snapshots ----------------------------------

    /// Build the current Settings overlay render snapshot.
    bool buildSettingsRenderState(SettingsRenderState& out) const;

    /// Build the current confirmation modal render snapshot.
    bool buildModalRenderState(ModalRenderState& out) const;

    /// Build the current Add Host overlay / status render snapshot.
    bool buildAddHostRenderState(AddHostRenderState& out) const;

    /// Provide the current 1-second stream statistics display snapshot.
    void setStreamStatistics(const StreamStatisticsSnapshot& snapshot);

    /// Returns true while the Add Host overlay is open.
    bool isAddHostOpen() const;

    /// Returns true while the Add Host overlay is open and an ImeDialog text
    /// entry is in progress.
    bool isAddHostImeActive() const;

    /// Request that the platform open the PS4 ImeDialog for the currently
    /// selected Add Host field.
    bool requestAddHostIme();

    /// Query the details of the pending Add Host ImeDialog request.
    bool getAddHostImeRequest(bool& isPort, std::string& initialText) const;

    /// Provide the text returned by the PS4 ImeDialog back to the Add Host
    /// overlay.
    void setAddHostImeResult(bool accepted, const std::string& text);

    /// Open the Add Host status modal with the given IP/port and a "Checking"
    /// state.
    void openAddHostStatus(const std::string& ipv4, uint16_t port);

    /// Update the Add Host status modal with the result of the manual host
    /// add.
    void setAddHostStatus(int status);

    /// Returns true while the Add Host status modal is open.
    bool isAddHostStatusOpen() const;

    /// Open the "Quit current app?" confirmation dialog (quit only).
    void openQuitAppConfirm(ml::HostId hostId, ml::ApplicationId appId);

    /// Open the "Quit running app and launch this one?" confirmation dialog.
    void openQuitAndLaunchConfirm(ml::HostId hostId,
                                  ml::ApplicationId quitAppId,
                                  ml::ApplicationId launchAppId);

    /// Returns true while the quit-app confirmation dialog is open.
    bool isQuitAppConfirmOpen() const;

    // --- CPU render snapshot builders (Boot / Pairing / stream overlays) ----

    /// Build the current Boot screen render snapshot.
    bool buildBootRenderState(BootRenderState& out) const;

    /// Build the current Pairing screen render snapshot.
    bool buildPairingRenderState(PairingRenderState& out) const;

    /// Build the current Help screen render snapshot.
    bool buildHelpRenderState(HelpRenderState& out) const;

    /// Build the current Stream Exit Hint render snapshot.
    bool buildStreamExitHintRenderState(StreamExitHintRenderState& out) const;

    /// Build the current Stream Statistics render snapshot.
    bool buildStreamStatisticsRenderState(StreamStatisticsRenderState& out) const;

    /// Build the current Mouse Emulation state overlay render snapshot.
    bool buildMouseEmulationRenderState(MouseEmulationRenderState& out) const;

    /// Apply normalized UI navigation input to the current screen.
    void processInput(const UiInput& input);

    /// Update presentation state using delta time (Boot timer, etc.).
    void update(float deltaSeconds);

    /// Begin the stream presentation session.
    void beginStreamSession(uint64_t nowMs, bool statsEnabledForSession);

    /// Start the stream exit hint countdown. Called once the first decoded
    /// video frame has been presented, so the hint's 3-second timer begins
    /// only when the hint is actually visible over video (not during the
    /// pre-first-frame window when no frame is rendered underneath it).
    void startStreamHintTimer();

    /// End the stream presentation session.
    void endStreamSession();

    /// Record the current mouse emulation state and (re)start the transient
    /// in-stream mouse emulation state overlay timer. Called by MoonlightApp
    /// whenever mouse emulation is toggled in-stream. The overlay remains
    /// active for the same duration as the stream exit hint.
    void setMouseEmulationState(bool enabled);

    /// Returns true while the transient in-stream mouse emulation state
    /// overlay is active (its timer has not yet expired).
    bool isMouseEmulationOverlayActive() const;

    /// Returns the current mouse emulation state (ON/OFF) for the active
    /// stream session.
    bool mouseEmulationEnabled() const;

    /// Poll a pending user intent.
    bool pollIntent(UiIntent& outIntent);

    /// Poll the pending UI navigation sound intent for this input update.
    UiSoundIntent pollSoundIntent();

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ui

#endif // UI_UI_MANAGER_H

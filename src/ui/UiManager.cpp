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

// === UiManager.cpp ===========================================================
//
// Moonlight client for PlayStation 4 — UI Manager / Basic UI Flow
//
// Implements the UiManager application-level UI component:
//   - Boot screen presentation state and timer.
//   - Host List screen fed by a read-only HostManager snapshot.
//   - Deterministic card grid navigation.
//   - HostId-based selection state ownership.
//   - User intent emission.
//   - Empty-list safety.
//   - CPU render snapshot generation for the dedicated renderers.
//
// This translation unit contains no OpenOrbis SDK calls.
// Rendering is owned entirely by the dedicated CPU renderers, the
// UiCompositor, and the UiBlitter.
// ===============================================================================

#include "UiManager.h"

#include "UiStrings.h"

#include <cstdio>    // printf, fflush, snprintf
#include <cstring>   // memset
#include <cstdlib>   // strtoul
#include <utility>   // std::move

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Boot screen duration (seconds). Presentation policy owned by UiManager.
static constexpr float kBootDurationSeconds = 2.0f;

/// Stream exit hint duration (milliseconds).
static constexpr uint64_t kStreamExitHintDurationMs = 3000;

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with the Application Shell)
// ---------------------------------------------------------------------------

static void uiDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Add Host validation helpers
// ---------------------------------------------------------------------------

/// Validate a numeric IPv4 address (A.B.C.D, each octet 0..255).
static bool isValidIpv4(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    int octets = 0;
    size_t i = 0;
    const size_t n = text.size();

    while (i < n) {
        // Parse one octet (1-3 decimal digits).
        int value = 0;
        int digits = 0;
        while (i < n && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            ++i;
            ++digits;
            if (digits > 3) {
                return false;   // More than 3 digits in an octet.
            }
        }
        if (digits == 0) {
            return false;       // Expected a digit.
        }
        if (value > 255) {
            return false;       // Octet out of range.
        }
        ++octets;

        // After an octet, expect either a '.' (more octets) or end of string.
        if (i < n) {
            if (text[i] != '.') {
                return false;   // Unexpected character.
            }
            ++i;                // Consume the '.'.
            if (i >= n) {
                return false;   // Trailing dot.
            }
        }
    }

    return octets == 4;
}

/// Validate a decimal port (1..65535).
static bool isValidPort(const std::string& text, uint16_t& outPort) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;       // Non-numeric character.
        }
    }
    unsigned long val = std::strtoul(text.c_str(), nullptr, 10);
    if (val < 1 || val > 65535) {
        return false;
    }
    outPort = static_cast<uint16_t>(val);
    return true;
}

/// Validate the Add Host form.
static bool validateAddHost(const std::string& ipText,
                            const std::string& portText,
                            std::string& error) {
    if (!isValidIpv4(ipText)) {
        error = "Invalid IPv4 address";
        return false;
    }
    uint16_t port = 0;
    if (!isValidPort(portText, port)) {
        error = "Port must be between 1 and 65535";
        return false;
    }
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ui {

/// Private implementation of UiManager. Holds all UI presentation state.
struct UiManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    bool initialized = false;

    // --- Screen state -------------------------------------------------------
    UiScreen screen = UiScreen::Boot;
    float    bootTimer = 0.0f;       ///< Elapsed Boot screen time (seconds).

    // --- Button assignment --------------------------------------------------
    bool crossIsAccept = true;

    // --- Presentation style -------------------------------------------------
    ml::PresentationStyle presentationStyle = ml::PresentationStyle::PlayStation;

    // --- Quit-app dialog preference -----------------------------------------
    /// Whether the "Quit App" confirmation dialog is shown before quitting.
    /// When false, quitting proceeds immediately without a dialog.
    bool showQuitAppDialog = true;

    // --- Grid column counts (for navigation) --------------------------------
    int hostGridColumns = 1;
    int appGridColumns = 1;

    // --- Host List data (presentation snapshot) -----------------------------
    std::vector<ml::Host> hosts;     ///< Read-only presentation snapshot.

    // --- Host List selection ------------------------------------------------
    ml::HostId selectedHostId = 0;   ///< Stable selection (0 = none).
    int        selectedHostIndex = -1;  ///< Derived index into `hosts` (-1 = none).

    // --- Pending intent -----------------------------------------------------
    UiIntent pendingIntent;          ///< One-frame pending intent.

    // --- Pending UI sound intent -------------------------------------------
    UiSoundIntent pendingSoundIntent = UiSoundIntent::None;

    // --- Pairing screen data (presentation snapshot) ------------------------
    PairingViewState pairingView;    ///< Read-only presentation snapshot.

    // --- App List data (presentation snapshot) ------------------------------
    ApplicationListView appListView; ///< Read-only presentation snapshot.

    // --- App List launch-progress state -------------------------------------
    ApplicationLaunchViewState launchView; ///< Read-only presentation snapshot.

    // --- App List quit-progress state ---------------------------------------
    ApplicationQuitViewState quitView; ///< Read-only presentation snapshot.

    // --- Stream screen data (presentation snapshot) -------------------------
    StreamViewState streamView;  ///< Read-only presentation snapshot.

    // --- Stream presentation session state ----------------------------------
    bool streamSessionActive = false;
    float streamHintRemainingSeconds = 0.0f;
    bool streamStatsEnabledForSession = false;
    StreamStatisticsSnapshot streamStats;

    // --- Mouse emulation state overlay --------------------------------------
    /// Whether controller-assisted mouse emulation is currently enabled for
    /// the active stream session. Starts OFF and can be toggled in-stream.
    bool mouseEmulationEnabled = false;
    /// Remaining time (seconds) for the transient mouse emulation state
    /// overlay. > 0 while the overlay is active; 0 when it has expired.
    float mouseEmulationOverlayRemainingSeconds = 0.0f;

    // --- App List selection -------------------------------------------------
    ml::ApplicationId selectedAppId = 0;   ///< Stable selection (0 = none).
    int               selectedAppIndex = -1;  ///< Derived index into appListView.applications (-1 = none).

    // --- Settings overlay state ---------------------------------------------
    bool settingsOverlayOpen = false;   ///< Whether the Settings overlay is open.
    int  settingsSelectedRow = 0;       ///< Selected settings row (0..6).
    SettingsViewState settingsView;     ///< Editable working copy.

    // --- Help screen state --------------------------------------------------
    /// Whether the Help screen is currently open.
    bool helpOpen = false;
    /// The screen to return to when Help closes (the screen Help was opened
    /// from). Only HostList / AppList are valid return screens.
    UiScreen helpReturnScreen = UiScreen::HostList;

    // --- Delete-host confirmation dialog state ------------------------------
    bool        deleteConfirmOpen = false;
    bool        deleteConfirmYes = false;
    ml::HostId  deleteConfirmHostId = 0;
    std::string deleteConfirmHostName;

    // --- Reset Moonlight confirmation dialog state --------------------------
    bool        resetConfirmOpen = false;
    bool        resetConfirmYes = false;

    // --- Add Host overlay state ---------------------------------------------
    enum class AddHostField : uint8_t {
        IpAddress,
        Port,
        Cancel,
        Add
    };
    bool        addHostOpen = false;
    AddHostField addHostSelected = AddHostField::IpAddress;
    std::string addHostIpText;
    std::string addHostPortText;
    std::string addHostValidationError;
    bool        addHostImeActive = false;
    /// The console's own IPv4 address, used to prepopulate the IP field when
    /// the Add Host overlay is opened. Empty = no prepopulation.
    std::string localIpAddress;

    // --- Add Host status modal state ----------------------------------------
    enum class AddHostStatus : uint8_t {
        Checking,
        FoundNew,
        FoundExisting,
        NotFound
    };
    bool        addHostStatusOpen = false;
    AddHostStatus addHostStatus = AddHostStatus::Checking;
    std::string addHostStatusIp;
    uint16_t    addHostStatusPort = 0;

    // --- Quit-app confirmation dialog state ---------------------------------
    bool        quitConfirmOpen = false;
    bool        quitConfirmYes = false;
    ml::HostId  quitConfirmHostId = 0;
    ml::ApplicationId quitConfirmAppId = 0;        ///< App to quit.
    ml::ApplicationId quitConfirmLaunchAppId = 0;  ///< App to launch after quit (0 = quit only).
    std::string quitConfirmMessage;

    /// Returns true while the current screen is one of the stream presentation
    /// screens.
    bool isStreamScreen() const;
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

UiManager::UiManager()
    : m_impl(new Impl())
{
}

UiManager::~UiManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

UiResult UiManager::initialize() {
    if (m_impl->initialized) {
        return UiResult(UiResultCategory::AlreadyInitialized);
    }

    uiDiagPrint("[ui] initialization start");

    // Initialize presentation state.
    m_impl->screen = UiScreen::Boot;
    m_impl->bootTimer = 0.0f;
    m_impl->hosts.clear();
    m_impl->selectedHostId = 0;
    m_impl->selectedHostIndex = -1;
    m_impl->appListView = ApplicationListView();
    m_impl->launchView = ApplicationLaunchViewState();
    m_impl->quitView = ApplicationQuitViewState();
    m_impl->streamView = StreamViewState();
    m_impl->streamSessionActive = false;
    m_impl->streamHintRemainingSeconds = 0.0f;
    m_impl->streamStatsEnabledForSession = false;
    m_impl->mouseEmulationEnabled = false;
    m_impl->mouseEmulationOverlayRemainingSeconds = 0.0f;
    m_impl->selectedAppId = 0;
    m_impl->selectedAppIndex = -1;
    m_impl->pendingIntent = UiIntent();
    m_impl->pendingSoundIntent = UiSoundIntent::None;
    m_impl->addHostOpen = false;
    m_impl->addHostImeActive = false;
    m_impl->addHostSelected = Impl::AddHostField::IpAddress;
    m_impl->addHostIpText.clear();
    m_impl->addHostPortText.clear();
    m_impl->addHostValidationError.clear();
    m_impl->helpOpen = false;
    m_impl->helpReturnScreen = UiScreen::HostList;
    m_impl->resetConfirmOpen = false;
    m_impl->resetConfirmYes = false;

    m_impl->initialized = true;
    uiDiagPrint("[ui] initialization success");
    return UiResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void UiManager::shutdown() {
    if (!m_impl) {
        return;
    }

    m_impl->screen = UiScreen::Boot;
    m_impl->bootTimer = 0.0f;
    m_impl->hosts.clear();
    m_impl->selectedHostId = 0;
    m_impl->selectedHostIndex = -1;
    m_impl->appListView = ApplicationListView();
    m_impl->launchView = ApplicationLaunchViewState();
    m_impl->quitView = ApplicationQuitViewState();
    m_impl->streamView = StreamViewState();
    m_impl->streamSessionActive = false;
    m_impl->streamHintRemainingSeconds = 0.0f;
    m_impl->streamStatsEnabledForSession = false;
    m_impl->mouseEmulationEnabled = false;
    m_impl->mouseEmulationOverlayRemainingSeconds = 0.0f;
    m_impl->selectedAppId = 0;
    m_impl->selectedAppIndex = -1;
    m_impl->pendingIntent = UiIntent();
    m_impl->pendingSoundIntent = UiSoundIntent::None;
    m_impl->addHostOpen = false;
    m_impl->addHostImeActive = false;
    m_impl->addHostSelected = Impl::AddHostField::IpAddress;
    m_impl->addHostIpText.clear();
    m_impl->addHostPortText.clear();
    m_impl->addHostValidationError.clear();
    m_impl->helpOpen = false;
    m_impl->helpReturnScreen = UiScreen::HostList;
    m_impl->resetConfirmOpen = false;
    m_impl->resetConfirmYes = false;
    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool UiManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Host data
// ---------------------------------------------------------------------------

void UiManager::setHosts(const std::vector<ml::Host>& hosts) {
    if (!m_impl) {
        return;
    }

    // Copy the read-only presentation snapshot.
    m_impl->hosts = hosts;

    // Recompute the derived selection index from the stable selectedHostId.
    m_impl->selectedHostIndex = -1;
    for (size_t i = 0; i < m_impl->hosts.size(); ++i) {
        if (m_impl->hosts[i].id == m_impl->selectedHostId) {
            m_impl->selectedHostIndex = static_cast<int>(i);
            break;
        }
    }

    // If there is no current selection and hosts exist, select the first.
    if (m_impl->selectedHostId == 0 && !m_impl->hosts.empty()) {
        m_impl->selectedHostId = m_impl->hosts[0].id;
        m_impl->selectedHostIndex = 0;
    }

    // Defensive guard: do NOT change the screen while the Help screen is open.
    // Host status changes / new-host discovery call setHosts() while Help is
    // open; the host data is still updated above so it is ready when Help
    // closes and the Host List is re-entered. setHosts() currently performs no
    // screen transition, but this guard protects against future changes that
    // might add one (mirroring the setApplicationListView() guard).
    if (m_impl->helpOpen) {
        return;
    }
}

// ---------------------------------------------------------------------------
// Pairing view data
// ---------------------------------------------------------------------------

void UiManager::setPairingView(const PairingViewState& view) {
    if (!m_impl) {
        return;
    }
    // Preserve the existing PIN if the incoming view does not carry one.
    PairingViewState merged = view;
    if (merged.pin.empty()) {
        merged.pin = m_impl->pairingView.pin;
    }
    m_impl->pairingView = merged;

    switch (merged.state) {
    case PairingPresentationState::Cancelled:
    case PairingPresentationState::Failed:
        m_impl->screen = UiScreen::HostList;
        break;
    case PairingPresentationState::Succeeded:
    default:
        m_impl->screen = UiScreen::Pairing;
        break;
    }
}

// ---------------------------------------------------------------------------
// App List view data
// ---------------------------------------------------------------------------

void UiManager::setApplicationListView(const ApplicationListView& view) {
    if (!m_impl) {
        return;
    }

    m_impl->appListView = view;

    // Recompute the derived selection index from the stable selectedAppId.
    m_impl->selectedAppIndex = -1;
    for (size_t i = 0; i < m_impl->appListView.applications.size(); ++i) {
        if (m_impl->appListView.applications[i].id == m_impl->selectedAppId) {
            m_impl->selectedAppIndex = static_cast<int>(i);
            break;
        }
    }

    // If there is no current selection and apps exist, select the first.
    if (m_impl->selectedAppId == 0 && !m_impl->appListView.applications.empty()) {
        m_impl->selectedAppId = m_impl->appListView.applications[0].id;
        m_impl->selectedAppIndex = 0;
    }

    // Do NOT change the screen while a stream presentation screen is active.
    // During streaming, serverinfo updates for the App List host call
    // refreshAppListView() → setApplicationListView(), which would otherwise
    // reset the screen to AppList and prematurely dismiss the exit hint /
    // statistics overlay. The App List data is still updated so it is ready
    // when the stream ends and the App List is re-entered.
    if (m_impl->isStreamScreen()) {
        return;
    }

    // Do NOT change the screen while the Help screen is open. The App List
    // serverinfo polling cadence (~1 s) calls refreshAppListView() →
    // setApplicationListView() while Help is open; without this guard the
    // screen would be reset to AppList, prematurely dismissing Help. The App
    // List data is still updated so it is ready when Help closes and the App
    // List is re-entered.
    if (m_impl->helpOpen) {
        return;
    }

    switch (view.state) {
    case ml::ApplicationCatalogState::Loading:
    case ml::ApplicationCatalogState::Ready:
    case ml::ApplicationCatalogState::Failed:
        m_impl->screen = UiScreen::AppList;
        break;
    case ml::ApplicationCatalogState::Idle:
    default:
        m_impl->screen = UiScreen::HostList;
        break;
    }
}

// ---------------------------------------------------------------------------
// App List launch-progress view data
// ---------------------------------------------------------------------------

void UiManager::setApplicationLaunchView(const ApplicationLaunchViewState& view) {
    if (!m_impl) {
        return;
    }
    m_impl->launchView = view;
}

// ---------------------------------------------------------------------------
// App List quit-progress view data
// ---------------------------------------------------------------------------

void UiManager::setApplicationQuitView(const ApplicationQuitViewState& view) {
    if (!m_impl) {
        return;
    }
    m_impl->quitView = view;
}

// ---------------------------------------------------------------------------
// Stream view data
// ---------------------------------------------------------------------------

void UiManager::setStreamView(const StreamViewState& view) {
    if (!m_impl) {
        return;
    }
    m_impl->streamView = view;

    switch (view.state) {
    case StreamPresentationState::Connecting:
    case StreamPresentationState::Streaming:
        break;
    case StreamPresentationState::Stopping:
    case StreamPresentationState::Failed:
        m_impl->screen = UiScreen::AppList;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Stream presentation session state
// ---------------------------------------------------------------------------

void UiManager::beginStreamSession(uint64_t nowMs, bool statsEnabledForSession) {
    if (!m_impl) {
        return;
    }
    m_impl->streamSessionActive = true;
    // The exit hint countdown is NOT started here. It is armed by
    // startStreamHintTimer() once the first decoded video frame is presented,
    // so the hint's 3-second timer begins only when the hint is actually
    // visible over video (not during the pre-first-frame window when no frame
    // is rendered underneath it).
    m_impl->streamHintRemainingSeconds = 0.0f;
    m_impl->streamStatsEnabledForSession = statsEnabledForSession;
    // Mouse emulation always starts OFF for a new stream session; the
    // transient state overlay is not active until the user toggles it.
    m_impl->mouseEmulationEnabled = false;
    m_impl->mouseEmulationOverlayRemainingSeconds = 0.0f;
    m_impl->screen = UiScreen::StreamExitHint;
}

void UiManager::startStreamHintTimer() {
    if (!m_impl) {
        return;
    }
    // Start the countdown only if the exit hint screen is still active and
    // the timer has not already been started. Idempotent: repeated calls (or
    // a call after the hint has already begun counting) do not reset it.
    if (m_impl->screen == UiScreen::StreamExitHint &&
        m_impl->streamHintRemainingSeconds <= 0.0f) {
        m_impl->streamHintRemainingSeconds =
            static_cast<float>(kStreamExitHintDurationMs) / 1000.0f;
    }
}

void UiManager::endStreamSession() {
    if (!m_impl) {
        return;
    }
    m_impl->streamSessionActive = false;
    m_impl->streamHintRemainingSeconds = 0.0f;
    m_impl->streamStatsEnabledForSession = false;
    m_impl->mouseEmulationEnabled = false;
    m_impl->mouseEmulationOverlayRemainingSeconds = 0.0f;
    m_impl->screen = UiScreen::AppList;
}

// ---------------------------------------------------------------------------
// Mouse emulation state overlay
// ---------------------------------------------------------------------------

void UiManager::setMouseEmulationState(bool enabled) {
    if (!m_impl) {
        return;
    }
    m_impl->mouseEmulationEnabled = enabled;
    // (Re)start the transient overlay timer using the same duration as the
    // stream exit hint.
    m_impl->mouseEmulationOverlayRemainingSeconds =
        static_cast<float>(kStreamExitHintDurationMs) / 1000.0f;
}

bool UiManager::isMouseEmulationOverlayActive() const {
    return m_impl && m_impl->mouseEmulationOverlayRemainingSeconds > 0.0f;
}

bool UiManager::mouseEmulationEnabled() const {
    return m_impl && m_impl->mouseEmulationEnabled;
}

// ---------------------------------------------------------------------------
// Settings view data
// ---------------------------------------------------------------------------

void UiManager::setSettingsView(const SettingsViewState& view) {
    if (!m_impl) {
        return;
    }
    m_impl->settingsView = view;
}

bool UiManager::isSettingsOpen() const {
    return m_impl && m_impl->settingsOverlayOpen;
}

bool UiManager::isHelpOpen() const {
    return m_impl && m_impl->helpOpen;
}

void UiManager::closeHelp() {
    if (!m_impl || !m_impl->helpOpen) {
        return;
    }
    m_impl->helpOpen = false;
    m_impl->screen = m_impl->helpReturnScreen;
    m_impl->resetConfirmOpen = false;
    m_impl->resetConfirmYes = false;
}

SettingsViewState UiManager::settingsViewSnapshot() const {
    if (m_impl) {
        return m_impl->settingsView;
    }
    return SettingsViewState();
}

bool UiManager::isDeleteConfirmOpen() const {
    return m_impl && m_impl->deleteConfirmOpen;
}

bool UiManager::isResetConfirmOpen() const {
    return m_impl && m_impl->resetConfirmOpen;
}

void UiManager::openResetConfirm() {
    if (!m_impl) {
        return;
    }
    m_impl->resetConfirmOpen = true;
    m_impl->resetConfirmYes = false;   // Default to No.
}

void UiManager::setButtonAssignment(bool crossIsAccept) {
    if (m_impl) {
        m_impl->crossIsAccept = crossIsAccept;
    }
}

void UiManager::setLocalIpAddress(const std::string& ip) {
    if (m_impl) {
        m_impl->localIpAddress = ip;
    }
}

void UiManager::setPresentationStyle(ml::PresentationStyle style) {
    if (m_impl) {
        m_impl->presentationStyle = style;
    }
}

ml::PresentationStyle UiManager::presentationStyle() const {
    if (m_impl) {
        return m_impl->presentationStyle;
    }
    return ml::PresentationStyle::PlayStation;
}

void UiManager::setShowQuitAppDialog(bool show) {
    if (m_impl) {
        m_impl->showQuitAppDialog = show;
    }
}

void UiManager::setGridColumns(int hostColumns, int appColumns) {
    if (!m_impl) {
        return;
    }
    if (hostColumns < 1) {
        hostColumns = 1;
    }
    if (appColumns < 1) {
        appColumns = 1;
    }
    m_impl->hostGridColumns = hostColumns;
    m_impl->appGridColumns = appColumns;
}

ui::UiScreen UiManager::currentScreen() const {
    if (m_impl) {
        return m_impl->screen;
    }
    return ui::UiScreen::Boot;
}

// ---------------------------------------------------------------------------
// Boot render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildBootRenderState(BootRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::Boot) {
        return false;
    }

    BootRenderState state;
    state.elapsedSeconds = m_impl->bootTimer;
    state.title = "Moonlight for PS4";
    state.subtitle.clear();

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Pairing render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildPairingRenderState(PairingRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::Pairing) {
        return false;
    }

    PairingRenderState state;
    switch (m_impl->pairingView.state) {
    case PairingPresentationState::Preparing:    state.state = PairingRenderState::State::Preparing; break;
    case PairingPresentationState::WaitingForUser: state.state = PairingRenderState::State::WaitingForPin; break;
    case PairingPresentationState::Pairing:      state.state = PairingRenderState::State::Pairing; break;
    case PairingPresentationState::Saving:       state.state = PairingRenderState::State::Saving; break;
    case PairingPresentationState::Confirming:   state.state = PairingRenderState::State::Confirming; break;
    case PairingPresentationState::Succeeded:    state.state = PairingRenderState::State::Succeeded; break;
    case PairingPresentationState::Failed:       state.state = PairingRenderState::State::Failed; break;
    case PairingPresentationState::Cancelled:    state.state = PairingRenderState::State::Cancelled; break;
    default:                                     state.state = PairingRenderState::State::Preparing; break;
    }
    state.hostName = m_impl->pairingView.hostName;
    state.pin = m_impl->pairingView.pin;
    state.message = m_impl->pairingView.statusText;
    state.crossIsAccept = m_impl->crossIsAccept;

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Help render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildHelpRenderState(HelpRenderState& out) const {
    if (!m_impl || !m_impl->initialized || !m_impl->helpOpen) {
        return false;
    }

    HelpRenderState state;
    state.crossIsAccept = m_impl->crossIsAccept;

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Stream Exit Hint render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildStreamExitHintRenderState(StreamExitHintRenderState& out) const {
    if (!m_impl || !m_impl->initialized ||
        m_impl->screen != UiScreen::StreamExitHint) {
        return false;
    }

    StreamExitHintRenderState state;
    state.visible = true;
    state.crossIsAccept = m_impl->crossIsAccept;

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Stream Statistics render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildStreamStatisticsRenderState(StreamStatisticsRenderState& out) const {
    if (!m_impl || !m_impl->initialized ||
        m_impl->screen != UiScreen::StreamStatistics) {
        return false;
    }

    StreamStatisticsRenderState state;
    state.visible = true;

    const StreamStatisticsSnapshot& s = m_impl->streamStats;

    char buf[64];

    if (s.width > 0 && s.height > 0) {
        snprintf(buf, sizeof(buf), "%dx%d", s.width, s.height);
        state.resolution = buf;
    }
    if (s.fps > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f", s.fps);
        state.fps = buf;
    }
    if (s.bitrateMbps > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f Mbps", s.bitrateMbps);
        state.bitrate = buf;
    }
    if (s.decodeMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.decodeMs);
        state.decodeTime = buf;
    }
    if (s.rttMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.rttMs);
        state.networkLatency = buf;
    }
    if (s.jitterMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.jitterMs);
        state.jitter = buf;
    }
    if (s.frameDecodeMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.frameDecodeMs);
        state.frameDecodeTime = buf;
    }
    if (s.framePresentMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.framePresentMs);
        state.framePresentTime = buf;
    }
    if (s.frameTotalMs > 0.0f) {
        snprintf(buf, sizeof(buf), "%.1f ms", s.frameTotalMs);
        state.frameTotalTime = buf;
    }

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Mouse Emulation state overlay render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildMouseEmulationRenderState(MouseEmulationRenderState& out) const {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }

    MouseEmulationRenderState state;
    state.visible = m_impl->mouseEmulationOverlayRemainingSeconds > 0.0f;
    state.enabled = m_impl->mouseEmulationEnabled;

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Host List render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildHostListRenderState(HostListRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::HostList) {
        return false;
    }

    HostListRenderState state;
    state.selectedIndex = m_impl->selectedHostIndex;
    state.crossIsAccept = m_impl->crossIsAccept;

    state.cards.reserve(m_impl->hosts.size());
    for (size_t i = 0; i < m_impl->hosts.size(); ++i) {
        const ml::Host& host = m_impl->hosts[i];
        HostListCardRenderData card;
        card.id = host.id;
        card.displayName = host.displayName;
        card.availability = host.availability;
        card.pairingState = host.pairingState;
        // App Running indicator: shown only while the host is positively
        // reachable (Online / Manually Added) and reports a running app.
        card.appRunning =
            (host.currentGame != 0) &&
            (host.availability == ml::HostAvailability::Online ||
             host.availability == ml::HostAvailability::ManuallyAdded);
        card.selected = (static_cast<int>(i) == m_impl->selectedHostIndex);
        state.cards.push_back(std::move(card));
    }

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// Host List Grid render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildHostGridRenderState(HostGridRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::HostList) {
        return false;
    }

    HostGridRenderState state;
    state.selectedIndex = m_impl->selectedHostIndex;
    state.crossIsAccept = m_impl->crossIsAccept;

    state.hosts.reserve(m_impl->hosts.size());
    for (size_t i = 0; i < m_impl->hosts.size(); ++i) {
        const ml::Host& host = m_impl->hosts[i];
        HostGridCardRenderState card;
        card.name = host.displayName;
        card.paired = (host.pairingState == ml::HostPairingState::Paired);
        switch (host.availability) {
        case ml::HostAvailability::Online:        card.availability = HostGridAvailability::Online;        break;
        case ml::HostAvailability::Stale:         card.availability = HostGridAvailability::Stale;         break;
        case ml::HostAvailability::ManuallyAdded: card.availability = HostGridAvailability::ManuallyAdded; break;
        case ml::HostAvailability::Offline:
        default:                                  card.availability = HostGridAvailability::Offline;       break;
        }
        card.appRunning =
            (host.currentGame != 0) &&
            (host.availability == ml::HostAvailability::Online ||
             host.availability == ml::HostAvailability::ManuallyAdded);
        card.selected = (static_cast<int>(i) == m_impl->selectedHostIndex);
        state.hosts.push_back(std::move(card));
    }

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// App List render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildAppListRenderState(AppListRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::AppList) {
        return false;
    }

    AppListRenderState state;
    state.selectedIndex = m_impl->selectedAppIndex;
    state.hostName = m_impl->appListView.hostName;
    state.crossIsAccept = m_impl->crossIsAccept;

    state.cards.reserve(m_impl->appListView.applications.size());
    for (size_t i = 0; i < m_impl->appListView.applications.size(); ++i) {
        const ml::ApplicationInfo& app = m_impl->appListView.applications[i];
        AppListCardRenderData card;
        card.id = app.id;
        card.title = app.name;
        card.running = app.running;
        card.connecting = app.connecting;
        card.waitingToLaunch = app.waitingToLaunch;
        card.quitting =
            m_impl->quitView.active && m_impl->quitView.applicationId == app.id;
        if (card.quitting) {
            card.quitStatusText = m_impl->quitView.statusText;
        }
        card.selected = (static_cast<int>(i) == m_impl->selectedAppIndex);
        state.cards.push_back(std::move(card));
    }

    out = std::move(state);
    return true;
}

// ---------------------------------------------------------------------------
// App List Grid render snapshot
// ---------------------------------------------------------------------------

bool UiManager::buildAppGridRenderState(AppGridRenderState& out) const {
    if (!m_impl || !m_impl->initialized || m_impl->screen != UiScreen::AppList) {
        return false;
    }

    AppGridRenderState state;
    state.selectedIndex = m_impl->selectedAppIndex;
    state.hostName = m_impl->appListView.hostName;
    state.crossIsAccept = m_impl->crossIsAccept;

    state.cards.reserve(m_impl->appListView.applications.size());
    for (size_t i = 0; i < m_impl->appListView.applications.size(); ++i) {
        const ml::ApplicationInfo& app = m_impl->appListView.applications[i];
        AppGridCardRenderState card;
        card.id = app.id;
        card.title = app.name;
        card.running = app.running;
        card.connecting = app.connecting;
        card.waitingToLaunch = app.waitingToLaunch;
        card.quitting =
            m_impl->quitView.active && m_impl->quitView.applicationId == app.id;
        if (card.quitting) {
            card.quitStatusText = m_impl->quitView.statusText;
        }
        card.selected = (static_cast<int>(i) == m_impl->selectedAppIndex);
        state.cards.push_back(std::move(card));
    }

    out = std::move(state);
    return true;
}

void UiManager::setStreamStatistics(const StreamStatisticsSnapshot& snapshot) {
    if (m_impl) {
        m_impl->streamStats = snapshot;
    }
}

// ---------------------------------------------------------------------------
// Settings / Modal render snapshots
// ---------------------------------------------------------------------------

bool UiManager::buildSettingsRenderState(SettingsRenderState& out) const {
    if (!m_impl || !m_impl->initialized || !m_impl->settingsOverlayOpen) {
        return false;
    }

    SettingsRenderState state;
    state.selectedIndex = m_impl->settingsSelectedRow;
    state.crossIsAccept = m_impl->crossIsAccept;

    auto addRow = [&](const char* label, const char* value, bool enabled,
                      int index) {
        SettingsRowRenderData row;
        row.label = label;
        row.value = value;
        row.enabled = enabled;
        row.selected = enabled && (index == m_impl->settingsSelectedRow);
        state.rows.push_back(std::move(row));
    };

    addRow("Resolution",
           m_impl->settingsView.resolutionIndex == 0 ? "720p" : "1080p",
           true, 0);
    addRow("Frame Rate",
           m_impl->settingsView.frameRateIndex == 0 ? "30 FPS" : "60 FPS",
           true, 1);
    addRow("Quit App on Exit",
           m_impl->settingsView.quitAppOnStreamExit ? "On" : "Off",
           true, 2);
    addRow("Stream Statistics",
           m_impl->settingsView.streamStatisticsEnabled ? "On" : "Off",
           true, 3);
    addRow("Presentation Style",
           m_impl->settingsView.presentationStyleIndex == 0 ? "CrossBar" : "Grid",
           true, 4);
    addRow("Show Quit App Dialog",
           m_impl->settingsView.showQuitAppDialog ? "On" : "Off",
           true, 5);
    addRow("Speakers",
           m_impl->settingsView.muteHostSpeakers ? "Muted" : "Unmuted",
           true, 6);
    addRow("Game Optimization",
           m_impl->settingsView.optimizeGameSettings ? "On" : "Off",
           true, 7);

    out = std::move(state);
    return true;
}

bool UiManager::buildModalRenderState(ModalRenderState& out) const {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }

    if (m_impl->deleteConfirmOpen) {
        ModalRenderState state;
        state.kind = ModalKind::DeleteHost;
        state.title = "Unpair Host?";
        state.message = "Remove saved pairing data?";
        state.hostName = m_impl->deleteConfirmHostName;
        state.yesSelected = m_impl->deleteConfirmYes;
        state.crossIsAccept = m_impl->crossIsAccept;
        out = std::move(state);
        return true;
    }

    if (m_impl->quitConfirmOpen) {
        ModalRenderState state;
        state.kind = ModalKind::QuitApp;
        state.title = "Quit App";
        state.message = m_impl->quitConfirmMessage;
        state.yesSelected = m_impl->quitConfirmYes;
        state.crossIsAccept = m_impl->crossIsAccept;
        out = std::move(state);
        return true;
    }

    if (m_impl->resetConfirmOpen) {
        ModalRenderState state;
        state.kind = ModalKind::ResetMoonlight;
        state.title = UiStrings::kResetMoonlight;
        state.message = UiStrings::kResetMoonlightWarning;
        state.message2 = UiStrings::kResetMoonlightConfirm;
        state.yesSelected = m_impl->resetConfirmYes;
        state.crossIsAccept = m_impl->crossIsAccept;
        out = std::move(state);
        return true;
    }

    return false;
}

bool UiManager::buildAddHostRenderState(AddHostRenderState& out) const {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }

    AddHostRenderState state;
    state.crossIsAccept = m_impl->crossIsAccept;

    if (m_impl->addHostOpen) {
        state.state = AddHostPresentationState::Form;
        state.ipText = m_impl->addHostIpText;
        state.portText = m_impl->addHostPortText;
        state.validationError = m_impl->addHostValidationError;
        state.imeActive = m_impl->addHostImeActive;
        switch (m_impl->addHostSelected) {
        case Impl::AddHostField::IpAddress: state.selectedField = 0; break;
        case Impl::AddHostField::Port:      state.selectedField = 1; break;
        case Impl::AddHostField::Cancel:    state.selectedField = 2; break;
        case Impl::AddHostField::Add:       state.selectedField = 3; break;
        default:                            state.selectedField = 0; break;
        }
        out = std::move(state);
        return true;
    }

    if (m_impl->addHostStatusOpen) {
        state.hostAddress = m_impl->addHostStatusIp;
        state.hostPort = m_impl->addHostStatusPort;
        switch (m_impl->addHostStatus) {
        case Impl::AddHostStatus::Checking:
            state.state = AddHostPresentationState::Checking;
            state.message = "Checking...";
            break;
        case Impl::AddHostStatus::FoundNew:
            state.state = AddHostPresentationState::Found;
            state.message = "Host found and added to Host List.";
            break;
        case Impl::AddHostStatus::FoundExisting:
            state.state = AddHostPresentationState::Found;
            state.message = "Host is already in Host List";
            break;
        case Impl::AddHostStatus::NotFound:
        default:
            state.state = AddHostPresentationState::Failed;
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "No host found at %s:%u",
                         m_impl->addHostStatusIp.c_str(),
                         static_cast<unsigned>(m_impl->addHostStatusPort));
                state.message = buf;
            }
            break;
        }
        out = std::move(state);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Add Host overlay state
// ---------------------------------------------------------------------------

bool UiManager::isAddHostOpen() const {
    return m_impl && m_impl->addHostOpen;
}

bool UiManager::isAddHostImeActive() const {
    return m_impl && m_impl->addHostOpen && m_impl->addHostImeActive;
}

bool UiManager::requestAddHostIme() {
    if (!m_impl || !m_impl->addHostOpen || m_impl->addHostImeActive) {
        return false;
    }
    // Only editable fields (IP Address / Port) open the ImeDialog.
    if (m_impl->addHostSelected != Impl::AddHostField::IpAddress &&
        m_impl->addHostSelected != Impl::AddHostField::Port) {
        return false;
    }
    m_impl->addHostImeActive = true;
    return true;
}

bool UiManager::getAddHostImeRequest(bool& isPort, std::string& initialText) const {
    if (!m_impl || !m_impl->addHostOpen || !m_impl->addHostImeActive) {
        return false;
    }
    if (m_impl->addHostSelected == Impl::AddHostField::Port) {
        isPort = true;
        initialText = m_impl->addHostPortText;
    } else {
        isPort = false;
        initialText = m_impl->addHostIpText;
    }
    return true;
}

void UiManager::setAddHostImeResult(bool accepted, const std::string& text) {
    if (!m_impl || !m_impl->addHostOpen || !m_impl->addHostImeActive) {
        return;
    }
    m_impl->addHostImeActive = false;

    // On cancel, leave the previous field value unchanged.
    if (!accepted) {
        return;
    }

    // Apply the returned text to the field that was being edited.
    if (m_impl->addHostSelected == Impl::AddHostField::IpAddress) {
        m_impl->addHostIpText = text;
    } else if (m_impl->addHostSelected == Impl::AddHostField::Port) {
        m_impl->addHostPortText = text;
    }

    // Clear any stale validation error so the user can re-submit.
    m_impl->addHostValidationError.clear();
}

void UiManager::openAddHostStatus(const std::string& ipv4, uint16_t port) {
    if (!m_impl) {
        return;
    }
    m_impl->addHostStatusOpen = true;
    m_impl->addHostStatus = Impl::AddHostStatus::Checking;
    m_impl->addHostStatusIp = ipv4;
    m_impl->addHostStatusPort = port;
}

void UiManager::setAddHostStatus(int status) {
    if (!m_impl || !m_impl->addHostStatusOpen) {
        return;
    }
    m_impl->addHostStatus = static_cast<Impl::AddHostStatus>(status);
}

bool UiManager::isAddHostStatusOpen() const {
    return m_impl && m_impl->addHostStatusOpen;
}

void UiManager::openQuitAppConfirm(ml::HostId hostId, ml::ApplicationId appId) {
    if (!m_impl) {
        return;
    }
    // When the "Show Quit App Dialog" setting is OFF, skip the confirmation
    // dialog and directly enter the "Yes" flow: emit the QuitApplication
    // intent immediately.
    if (!m_impl->showQuitAppDialog) {
        m_impl->pendingIntent = UiIntent(
            UiIntentType::QuitApplication, hostId, appId);
        m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        return;
    }
    m_impl->quitConfirmOpen = true;
    m_impl->quitConfirmYes = false;
    m_impl->quitConfirmHostId = hostId;
    m_impl->quitConfirmAppId = appId;
    m_impl->quitConfirmLaunchAppId = 0;
    m_impl->quitConfirmMessage = UiStrings::kQuitCurrentApp;
}

void UiManager::openQuitAndLaunchConfirm(ml::HostId hostId,
                                         ml::ApplicationId quitAppId,
                                         ml::ApplicationId launchAppId) {
    if (!m_impl) {
        return;
    }
    // When the "Show Quit App Dialog" setting is OFF, skip the confirmation
    // dialog and directly enter the "Yes" flow: emit the
    // QuitAndLaunchApplication intent immediately.
    if (!m_impl->showQuitAppDialog) {
        UiIntent intent(UiIntentType::QuitAndLaunchApplication,
                        hostId, quitAppId);
        intent.launchApplicationId = launchAppId;
        m_impl->pendingIntent = intent;
        m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        return;
    }
    m_impl->quitConfirmOpen = true;
    m_impl->quitConfirmYes = false;
    m_impl->quitConfirmHostId = hostId;
    m_impl->quitConfirmAppId = quitAppId;
    m_impl->quitConfirmLaunchAppId = launchAppId;
    m_impl->quitConfirmMessage = UiStrings::kQuitAndLaunch;
}

bool UiManager::isQuitAppConfirmOpen() const {
    return m_impl && m_impl->quitConfirmOpen;
}

// ---------------------------------------------------------------------------
// Input processing
// ---------------------------------------------------------------------------

void UiManager::processInput(const UiInput& input) {
    if (!m_impl->initialized) {
        return;
    }

    // Boot screen ignores normal navigation input.
    if (m_impl->screen == UiScreen::Boot) {
        return;
    }

    // --- Quit-app confirmation dialog: exclusive focus ----------------------
    if (m_impl->quitConfirmOpen) {
        const bool prevYes = m_impl->quitConfirmYes;
        if (input.leftPressed) {
            m_impl->quitConfirmYes = false;
        }
        if (input.rightPressed) {
            m_impl->quitConfirmYes = true;
        }
        if (m_impl->quitConfirmYes != prevYes) {
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed) {
            if (m_impl->quitConfirmYes) {
                if (m_impl->quitConfirmLaunchAppId != 0) {
                    // Quit the running app, then launch the focused app.
                    UiIntent intent(UiIntentType::QuitAndLaunchApplication,
                                    m_impl->quitConfirmHostId,
                                    m_impl->quitConfirmAppId);
                    intent.launchApplicationId = m_impl->quitConfirmLaunchAppId;
                    m_impl->pendingIntent = intent;
                } else {
                    // Quit the running app only.
                    m_impl->pendingIntent = UiIntent(
                        UiIntentType::QuitApplication,
                        m_impl->quitConfirmHostId,
                        m_impl->quitConfirmAppId);
                }
            }
            m_impl->quitConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }

        if (input.backPressed) {
            m_impl->quitConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }

        return;
    }

    // --- Add Host status modal: exclusive focus -----------------------------
    if (m_impl->addHostStatusOpen) {
        const bool isChecking =
            (m_impl->addHostStatus == Impl::AddHostStatus::Checking);

        if (input.acceptPressed) {
            if (isChecking) {
                m_impl->pendingIntent = UiIntent(UiIntentType::CancelAddHost);
            }
            m_impl->addHostStatusOpen = false;
            m_impl->screen = UiScreen::HostList;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }
        if (input.backPressed) {
            if (isChecking) {
                m_impl->pendingIntent = UiIntent(UiIntentType::CancelAddHost);
            }
            m_impl->addHostStatusOpen = false;
            m_impl->screen = UiScreen::HostList;
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }
        return;
    }

    // --- Add Host overlay: exclusive focus ----------------------------------
    if (m_impl->addHostOpen) {
        if (m_impl->addHostImeActive) {
            return;
        }

        const Impl::AddHostField prevField = m_impl->addHostSelected;
        if (input.upPressed) {
            if (m_impl->addHostSelected == Impl::AddHostField::Port) {
                m_impl->addHostSelected = Impl::AddHostField::IpAddress;
            } else if (m_impl->addHostSelected == Impl::AddHostField::Cancel ||
                       m_impl->addHostSelected == Impl::AddHostField::Add) {
                m_impl->addHostSelected = Impl::AddHostField::Port;
            }
        }
        if (input.downPressed) {
            if (m_impl->addHostSelected == Impl::AddHostField::IpAddress) {
                m_impl->addHostSelected = Impl::AddHostField::Port;
            } else if (m_impl->addHostSelected == Impl::AddHostField::Port) {
                m_impl->addHostSelected = Impl::AddHostField::Cancel;
            }
        }
        if (input.leftPressed) {
            if (m_impl->addHostSelected == Impl::AddHostField::Add) {
                m_impl->addHostSelected = Impl::AddHostField::Cancel;
            }
        }
        if (input.rightPressed) {
            if (m_impl->addHostSelected == Impl::AddHostField::Cancel) {
                m_impl->addHostSelected = Impl::AddHostField::Add;
            }
        }
        if (m_impl->addHostSelected != prevField) {
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed) {
            switch (m_impl->addHostSelected) {
            case Impl::AddHostField::IpAddress:
            case Impl::AddHostField::Port:
                m_impl->addHostImeActive = true;
                break;
            case Impl::AddHostField::Cancel:
                m_impl->addHostOpen = false;
                m_impl->addHostImeActive = false;
                m_impl->addHostValidationError.clear();
                m_impl->pendingSoundIntent = UiSoundIntent::Accept;
                break;
            case Impl::AddHostField::Add:
                if (validateAddHost(m_impl->addHostIpText,
                                    m_impl->addHostPortText,
                                    m_impl->addHostValidationError)) {
                    UiIntent intent(UiIntentType::RequestAddHost);
                    intent.ipv4 = m_impl->addHostIpText;
                    intent.port = static_cast<uint16_t>(
                        std::strtoul(m_impl->addHostPortText.c_str(), nullptr, 10));
                    m_impl->pendingIntent = intent;
                    m_impl->addHostOpen = false;
                    m_impl->addHostImeActive = false;
                    m_impl->addHostValidationError.clear();
                    m_impl->addHostStatusOpen = true;
                    m_impl->addHostStatus = Impl::AddHostStatus::Checking;
                    m_impl->addHostStatusIp = intent.ipv4;
                    m_impl->addHostStatusPort = intent.port;
                    m_impl->pendingSoundIntent = UiSoundIntent::Accept;
                }
                break;
            }
            return;
        }

        if (input.backPressed) {
            m_impl->addHostOpen = false;
            m_impl->addHostImeActive = false;
            m_impl->addHostValidationError.clear();
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }

        return;
    }

    // --- Delete-host confirmation dialog: exclusive focus -------------------
    if (m_impl->deleteConfirmOpen) {
        const bool prevYes = m_impl->deleteConfirmYes;
        if (input.leftPressed) {
            m_impl->deleteConfirmYes = false;
        }
        if (input.rightPressed) {
            m_impl->deleteConfirmYes = true;
        }
        if (m_impl->deleteConfirmYes != prevYes) {
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed) {
            if (m_impl->deleteConfirmYes) {
                m_impl->pendingIntent = UiIntent(
                    UiIntentType::RequestDeleteHost, m_impl->deleteConfirmHostId);
            }
            m_impl->deleteConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }

        if (input.backPressed) {
            m_impl->deleteConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }

        return;
    }

    // --- Reset Moonlight confirmation dialog: exclusive focus ---------------
    if (m_impl->resetConfirmOpen) {
        const bool prevYes = m_impl->resetConfirmYes;
        if (input.leftPressed) {
            m_impl->resetConfirmYes = false;
        }
        if (input.rightPressed) {
            m_impl->resetConfirmYes = true;
        }
        if (m_impl->resetConfirmYes != prevYes) {
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed) {
            if (m_impl->resetConfirmYes) {
                m_impl->pendingIntent = UiIntent(UiIntentType::RequestResetMoonlight);
            }
            m_impl->resetConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }

        if (input.backPressed) {
            m_impl->resetConfirmOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }

        return;
    }

    // --- Settings overlay: exclusive focus ----------------------------------
    if (m_impl->settingsOverlayOpen) {
        if (input.optionsPressed || input.backPressed) {
            UiIntent intent(UiIntentType::CloseSettings);
            intent.settings = m_impl->settingsView;
            m_impl->pendingIntent = intent;
            m_impl->settingsOverlayOpen = false;
            m_impl->pendingSoundIntent = UiSoundIntent::SettingsClose;
            return;
        }

        const int prevRow = m_impl->settingsSelectedRow;
        if (input.upPressed && m_impl->settingsSelectedRow > 0) {
            m_impl->settingsSelectedRow--;
        }
        if (input.downPressed && m_impl->settingsSelectedRow < 7) {
            m_impl->settingsSelectedRow++;
        }
        if (m_impl->settingsSelectedRow != prevRow) {
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.leftPressed || input.rightPressed) {
            switch (m_impl->settingsSelectedRow) {
            case 0:
                m_impl->settingsView.resolutionIndex =
                    (m_impl->settingsView.resolutionIndex == 0) ? 1 : 0;
                break;
            case 1:
                m_impl->settingsView.frameRateIndex =
                    (m_impl->settingsView.frameRateIndex == 0) ? 1 : 0;
                break;
            case 2:
                m_impl->settingsView.quitAppOnStreamExit =
                    !m_impl->settingsView.quitAppOnStreamExit;
                break;
            case 3:
                m_impl->settingsView.streamStatisticsEnabled =
                    !m_impl->settingsView.streamStatisticsEnabled;
                break;
            case 4:
                m_impl->settingsView.presentationStyleIndex =
                    (m_impl->settingsView.presentationStyleIndex == 0) ? 1 : 0;
                m_impl->presentationStyle =
                    (m_impl->settingsView.presentationStyleIndex == 0)
                        ? ml::PresentationStyle::PlayStation
                        : ml::PresentationStyle::Grid;
                break;
            case 5:
                m_impl->settingsView.showQuitAppDialog =
                    !m_impl->settingsView.showQuitAppDialog;
                break;
            case 6:
                m_impl->settingsView.muteHostSpeakers =
                    !m_impl->settingsView.muteHostSpeakers;
                break;
            case 7:
                m_impl->settingsView.optimizeGameSettings =
                    !m_impl->settingsView.optimizeGameSettings;
                break;
            default:
                break;
            }
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        }

        if (input.acceptPressed) {
            switch (m_impl->settingsSelectedRow) {
            case 0:
                m_impl->settingsView.resolutionIndex =
                    (m_impl->settingsView.resolutionIndex == 0) ? 1 : 0;
                break;
            case 1:
                m_impl->settingsView.frameRateIndex =
                    (m_impl->settingsView.frameRateIndex == 0) ? 1 : 0;
                break;
            case 2:
                m_impl->settingsView.quitAppOnStreamExit =
                    !m_impl->settingsView.quitAppOnStreamExit;
                break;
            case 3:
                m_impl->settingsView.streamStatisticsEnabled =
                    !m_impl->settingsView.streamStatisticsEnabled;
                break;
            case 4:
                m_impl->settingsView.presentationStyleIndex =
                    (m_impl->settingsView.presentationStyleIndex == 0) ? 1 : 0;
                m_impl->presentationStyle =
                    (m_impl->settingsView.presentationStyleIndex == 0)
                        ? ml::PresentationStyle::PlayStation
                        : ml::PresentationStyle::Grid;
                break;
            case 5:
                m_impl->settingsView.showQuitAppDialog =
                    !m_impl->settingsView.showQuitAppDialog;
                break;
            case 6:
                m_impl->settingsView.muteHostSpeakers =
                    !m_impl->settingsView.muteHostSpeakers;
                break;
            case 7:
                m_impl->settingsView.optimizeGameSettings =
                    !m_impl->settingsView.optimizeGameSettings;
                break;
            default:
                break;
            }
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        }
        return;
    }

    // Pairing screen: Back cancels pairing.
    if (m_impl->screen == UiScreen::Pairing) {
        if (input.backPressed) {
            m_impl->pendingIntent = UiIntent(UiIntentType::CancelPairing);
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
        }
        return;
    }

    // Help screen: OK (accept) or Back closes Help and returns to the screen
    // it was opened from. Triangle opens the Reset Moonlight confirmation.
    if (m_impl->screen == UiScreen::Help) {
        if (input.trianglePressed) {
            openResetConfirm();
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }
        if (input.acceptPressed || input.backPressed) {
            m_impl->helpOpen = false;
            m_impl->screen = m_impl->helpReturnScreen;
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
        }
        return;
    }

    // Stream presentation screens: no menu input.
    if (m_impl->isStreamScreen()) {
        return;
    }

    // App List navigation.
    if (m_impl->screen == UiScreen::AppList) {
        if (m_impl->launchView.active) {
            if (input.backPressed) {
                m_impl->pendingIntent = UiIntent(UiIntentType::Back);
                m_impl->pendingSoundIntent = UiSoundIntent::Back;
            }
            return;
        }

        const int itemCount = static_cast<int>(m_impl->appListView.applications.size());

        if (itemCount <= 0) {
            if (input.backPressed) {
                m_impl->pendingIntent = UiIntent(UiIntentType::Back);
                m_impl->pendingSoundIntent = UiSoundIntent::Back;
            }
            return;
        }

        if (m_impl->selectedAppIndex < 0 ||
            m_impl->selectedAppIndex >= itemCount) {
            m_impl->selectedAppIndex = 0;
            m_impl->selectedAppId = m_impl->appListView.applications[0].id;
        }

        int newIndex = m_impl->selectedAppIndex;

        if (m_impl->presentationStyle == ml::PresentationStyle::PlayStation) {
            if (input.leftPressed && newIndex > 0) {
                newIndex--;
            }
            if (input.rightPressed && newIndex + 1 < itemCount) {
                newIndex++;
            }
        } else {
            const int columns = m_impl->appGridColumns;
            const int col = m_impl->selectedAppIndex % columns;
            const int row = m_impl->selectedAppIndex / columns;
            const int rowCount = (itemCount + columns - 1) / columns;

            if (input.leftPressed && col > 0) {
                newIndex--;
            }
            if (input.rightPressed && col < columns - 1 &&
                newIndex + 1 < itemCount) {
                newIndex++;
            }
            if (input.upPressed && row > 0) {
                newIndex -= columns;
            }
            if (input.downPressed && row + 1 < rowCount) {
                int next = newIndex + columns;
                if (next >= itemCount) {
                    next = itemCount - 1;
                }
                newIndex = next;
            }
        }

        if (newIndex != m_impl->selectedAppIndex) {
            m_impl->selectedAppIndex = newIndex;
            m_impl->selectedAppId = m_impl->appListView.applications[newIndex].id;
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed &&
            m_impl->selectedAppIndex >= 0 &&
            m_impl->selectedAppIndex < itemCount) {
            m_impl->pendingIntent = UiIntent(
                UiIntentType::ActivateApplication,
                m_impl->appListView.hostId,
                m_impl->appListView.applications[m_impl->selectedAppIndex].id);
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        }

        if (input.trianglePressed &&
            m_impl->selectedAppIndex >= 0 &&
            m_impl->selectedAppIndex < itemCount &&
            m_impl->appListView.applications[m_impl->selectedAppIndex].running) {
            // Open the "Quit current app?" confirmation dialog (or, when the
            // "Show Quit App Dialog" setting is OFF, emit the QuitApplication
            // intent directly). The actual QuitApplication intent is emitted
            // only when the user confirms (or immediately when the dialog is
            // disabled).
            openQuitAppConfirm(m_impl->appListView.hostId,
                               m_impl->appListView.applications[m_impl->selectedAppIndex].id);
        }

        if (input.backPressed) {
            m_impl->pendingIntent = UiIntent(UiIntentType::Back);
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
        }

        if (input.optionsPressed) {
            m_impl->settingsOverlayOpen = true;
            m_impl->settingsSelectedRow = 0;
            m_impl->pendingIntent = UiIntent(UiIntentType::OpenSettings);
            m_impl->pendingSoundIntent = UiSoundIntent::SettingsOpen;
        }

        // Touchpad Click opens the Help screen.
        if (input.touchpadPressed) {
            m_impl->helpOpen = true;
            m_impl->helpReturnScreen = UiScreen::AppList;
            m_impl->screen = UiScreen::Help;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        }
        return;
    }

    // Host List navigation.
    if (m_impl->screen == UiScreen::HostList) {
        const int itemCount = static_cast<int>(m_impl->hosts.size());

        // --- Global actions (do not require a selected host) ----------------
        // These must work even when the list is empty (e.g. a fresh install
        // with no persisted paired hosts and no mDNS hosts), so the user can
        // still Add Host Manually, open Settings, open Help, or Back out.
        if (input.squarePressed) {
            m_impl->addHostOpen = true;
            m_impl->addHostImeActive = false;
            m_impl->addHostSelected = Impl::AddHostField::IpAddress;
            m_impl->addHostValidationError.clear();
            // Prepopulate the IP field with the console's own address so
            // entering a host on the same network is easier. Only set it when
            // the field is empty (don't clobber a previously entered value).
            if (m_impl->addHostIpText.empty() &&
                !m_impl->localIpAddress.empty()) {
                m_impl->addHostIpText = m_impl->localIpAddress;
            }
            if (m_impl->addHostPortText.empty()) {
                m_impl->addHostPortText = "47989";
            }
            return;
        }

        if (input.backPressed) {
            m_impl->pendingIntent = UiIntent(UiIntentType::Back);
            m_impl->pendingSoundIntent = UiSoundIntent::Back;
            return;
        }

        if (input.optionsPressed) {
            m_impl->settingsOverlayOpen = true;
            m_impl->settingsSelectedRow = 0;
            m_impl->pendingIntent = UiIntent(UiIntentType::OpenSettings);
            m_impl->pendingSoundIntent = UiSoundIntent::SettingsOpen;
            return;
        }

        // Touchpad Click opens the Help screen.
        if (input.touchpadPressed) {
            m_impl->helpOpen = true;
            m_impl->helpReturnScreen = UiScreen::HostList;
            m_impl->screen = UiScreen::Help;
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
            return;
        }

        // --- Host-selection-dependent actions (require a non-empty list) ----
        if (itemCount <= 0) {
            return;
        }

        if (m_impl->selectedHostIndex < 0 ||
            m_impl->selectedHostIndex >= itemCount) {
            m_impl->selectedHostIndex = 0;
            m_impl->selectedHostId = m_impl->hosts[0].id;
        }

        int newIndex = m_impl->selectedHostIndex;

        if (m_impl->presentationStyle == ml::PresentationStyle::PlayStation) {
            if (input.leftPressed && newIndex > 0) {
                newIndex--;
            }
            if (input.rightPressed && newIndex + 1 < itemCount) {
                newIndex++;
            }
        } else {
            const int columns = m_impl->hostGridColumns;
            const int col = m_impl->selectedHostIndex % columns;
            const int row = m_impl->selectedHostIndex / columns;
            const int rowCount = (itemCount + columns - 1) / columns;

            if (input.leftPressed && col > 0) {
                newIndex--;
            }
            if (input.rightPressed && col < columns - 1 &&
                newIndex + 1 < itemCount) {
                newIndex++;
            }
            if (input.upPressed && row > 0) {
                newIndex -= columns;
            }
            if (input.downPressed && row + 1 < rowCount) {
                int next = newIndex + columns;
                if (next >= itemCount) {
                    next = itemCount - 1;
                }
                newIndex = next;
            }
        }

        if (newIndex != m_impl->selectedHostIndex) {
            m_impl->selectedHostIndex = newIndex;
            m_impl->selectedHostId = m_impl->hosts[newIndex].id;
            m_impl->pendingSoundIntent = UiSoundIntent::Cursor;
        }

        if (input.acceptPressed &&
            m_impl->selectedHostIndex >= 0 &&
            m_impl->selectedHostIndex < itemCount) {
            m_impl->pendingIntent = UiIntent(
                UiIntentType::ActivateHost,
                m_impl->hosts[m_impl->selectedHostIndex].id);
            m_impl->pendingSoundIntent = UiSoundIntent::Accept;
        }

        if (input.trianglePressed &&
            m_impl->selectedHostIndex >= 0 &&
            m_impl->selectedHostIndex < itemCount) {
            const ml::Host& host = m_impl->hosts[m_impl->selectedHostIndex];
            m_impl->deleteConfirmOpen = true;
            m_impl->deleteConfirmYes = false;
            m_impl->deleteConfirmHostId = host.id;
            m_impl->deleteConfirmHostName = host.displayName;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void UiManager::update(float deltaSeconds) {
    if (!m_impl->initialized) {
        return;
    }

    // Boot screen timer: transition to Host List after the fixed duration.
    if (m_impl->screen == UiScreen::Boot) {
        m_impl->bootTimer += deltaSeconds;
        if (m_impl->bootTimer >= kBootDurationSeconds) {
            m_impl->bootTimer = kBootDurationSeconds;
            m_impl->screen = UiScreen::HostList;
            uiDiagPrint("[ui] boot -> host list transition");
        }
    }

    // Stream exit hint timeout: transition from StreamExitHint to
    // StreamEmpty or StreamStatistics based on the session statistics
    // snapshot. The countdown only runs once startStreamHintTimer() has armed
    // it (streamHintRemainingSeconds > 0), i.e. after the first decoded video
    // frame is presented. Before that, the hint screen stays put without
    // counting down.
    if (m_impl->screen == UiScreen::StreamExitHint &&
        m_impl->streamHintRemainingSeconds > 0.0f) {
        m_impl->streamHintRemainingSeconds -= deltaSeconds;
        if (m_impl->streamHintRemainingSeconds <= 0.0f) {
            m_impl->streamHintRemainingSeconds = 0.0f;
            m_impl->screen = m_impl->streamStatsEnabledForSession
                                 ? UiScreen::StreamStatistics
                                 : UiScreen::StreamEmpty;
        }
    }

    // Mouse emulation state overlay timeout: decrement the remaining time and
    // clamp at 0. The overlay is active only while this timer is > 0.
    if (m_impl->mouseEmulationOverlayRemainingSeconds > 0.0f) {
        m_impl->mouseEmulationOverlayRemainingSeconds -= deltaSeconds;
        if (m_impl->mouseEmulationOverlayRemainingSeconds < 0.0f) {
            m_impl->mouseEmulationOverlayRemainingSeconds = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Intent polling
// ---------------------------------------------------------------------------

bool UiManager::pollIntent(UiIntent& outIntent) {
    if (!m_impl->initialized) {
        return false;
    }

    if (m_impl->pendingIntent.type != UiIntentType::None) {
        outIntent = m_impl->pendingIntent;
        m_impl->pendingIntent = UiIntent();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// UI sound intent polling
// ---------------------------------------------------------------------------

UiSoundIntent UiManager::pollSoundIntent() {
    if (!m_impl->initialized) {
        return UiSoundIntent::None;
    }

    const UiSoundIntent intent = m_impl->pendingSoundIntent;
    m_impl->pendingSoundIntent = UiSoundIntent::None;
    return intent;
}

// ---------------------------------------------------------------------------
// Stream presentation screen helper
// ---------------------------------------------------------------------------

/// Returns true while the current screen is one of the stream presentation
/// screens (StreamExitHint / StreamEmpty / StreamStatistics).
bool UiManager::Impl::isStreamScreen() const {
    return screen == UiScreen::StreamExitHint ||
           screen == UiScreen::StreamEmpty ||
           screen == UiScreen::StreamStatistics;
}

} // namespace ui

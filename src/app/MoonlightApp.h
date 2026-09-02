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

// === MoonlightApp.h =========================================================
//
// OpenOrbis Moonlight client — composition root and lifecycle owner.
//
// MoonlightApp is the production coordinator. It owns subsystem lifecycle,
// main-thread orchestration, UI intent routing, and renderer dispatch. It is
// deliberately NOT a generic application framework.
//
// This is the OpenOrbis port of the SCE MoonlightApp coordinator. It owns the
// full production subsystem tree: platform graphics/input/network, host
// discovery and registry, pairing, HTTP/HTTPS, application catalog and
// artwork, application launch/resume/quit, stream session, video decode,
// audio decode/output, controller input streaming, and the CPU UI renderers.
//
// Host pairing state is reconciled from local trust (PairingStore/PairingManager)
// and server-reported pairing status. ApplicationCatalog retrieves and parses
// the real Sunshine app list for a paired host and feeds the App List UI.
//
// This header exposes only project-owned types. OpenOrbis SDK types are confined to
// the implementation file.
// ===============================================================================

#ifndef MOONLIGHT_APP_H
#define MOONLIGHT_APP_H

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "moonlight/HostManager.h"   // ml::HostId, ml::Host
#include "moonlight/ServerInfoManager.h"  // ml::ServerInfoTarget
#include "moonlight/PairingStore.h"  // ml::ClientIdentity, ml::TrustedHost
#include "moonlight/PairingManager.h"  // ml::PairingManager, ml::PairingTarget, ml::PairingUpdate
#include "moonlight/ApplicationCatalog.h"  // ml::ApplicationCatalog, ml::ApplicationInfo, ml::ApplicationCatalogState, ml::ApplicationCatalogUpdate
#include "moonlight/AppArtworkManager.h"  // ml::AppArtworkManager, ml::AppArtworkResult, ml::AppArtworkTarget
#include "moonlight/ApplicationSessionManager.h"  // ml::ApplicationSessionManager, ml::ApplicationLaunchTarget, ml::ApplicationQuitTarget, ml::ApplicationLaunchUpdate, ml::ApplicationQuitUpdate, ml::StreamSessionDescriptor
#include "moonlight/StreamSessionManager.h"  // ml::StreamSessionManager, ml::StreamSessionUpdate, ml::StreamSessionState, ml::StreamSessionResult
#include "moonlight/VideoDecoder.h"  // ml::VideoDecoder, ml::DecodedVideoFrame, ml::VideoDecoderConfiguration, ml::VideoDecoderUpdate
#include "moonlight/AudioDecodeOutput.h"  // ml::AudioDecodeOutput, ml::AudioDecodeOutputUpdate
#include "moonlight/ControllerInputStreaming.h"  // ml::ControllerInputStreaming, ml::ControllerInputStreamingState
#include "moonlight/SettingsManager.h"  // ml::SettingsManager, ml::Settings, ml::StreamResolution, ml::StreamFrameRate
#include "moonlight/StreamStatistics.h"  // ml::StreamStatisticsSnapshot, ml::StreamStatsCounters
#include "platform/ps4/PngCpuDecode.h"  // ps4::CpuPngImage (runtime app artwork)
#include "ui/HostListRenderer.h"   // ui::HostListRenderState (value member)
#include "ui/HostGridRenderer.h"   // ui::HostGridRenderState (value member)
#include "ui/PairingRenderer.h"    // ui::PairingRenderState (value member)
#include "ui/HelpRenderer.h"       // ui::HelpRenderState (value member)
#include "ui/AppListRenderer.h"    // ui::AppListRenderState (value member)
#include "ui/AppGridRenderer.h"    // ui::AppGridRenderState (value member)
#include "ui/SettingsRenderer.h"   // ui::SettingsRenderState (value member)
#include "ui/ModalRenderer.h"      // ui::ModalRenderState (value member)
#include "ui/AddHostRenderer.h"    // ui::AddHostRenderState (value member)
#include "ui/StreamExitHintRenderer.h"  // ui::StreamExitHintRenderState (value member)
#include "ui/StreamStatisticsRenderer.h"  // ui::StreamStatisticsRenderState (value member)
#include "ui/MouseEmulationOverlayRenderer.h"  // ui::MouseEmulationRenderState (value member)

// Forward declarations of the platform graphics subsystem, UI renderer, and
// input subsystem. The concrete implementations live in the platform/ps4 module.
namespace ps4 {
class GfxManager;
class InputManager;
class NetworkManager;
class FontRasterizer;
class UiCompositor;
class UiBlitter;
class SysmoduleManager;
class UiSoundManager;
class ImeDialog;
}

// Forward declaration of the UI manager and its intent type.
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
class StreamExitHintRenderer;
class StreamStatisticsRenderer;
class MouseEmulationOverlayRenderer;
struct UiIntent;
struct HostListRenderState;
struct HostGridRenderState;
struct PairingRenderState;
struct AppListRenderState;
struct AppGridRenderState;
struct SettingsRenderState;
struct ModalRenderState;
struct AddHostRenderState;
struct StreamExitHintRenderState;
struct StreamStatisticsRenderState;
struct MouseEmulationRenderState;
}

// Forward declaration of the Moonlight-specific host discovery subsystem, the
// authoritative host registry, the HTTP/HTTPS client, and the serverinfo
// query subsystem.
namespace ml {
class DiscoveryManager;
class HostManager;
class HttpClient;
class ServerInfoManager;
class PairingManager;
class ApplicationCatalog;
class AppArtworkManager;
class ApplicationSessionManager;
class StreamSessionManager;
class VideoDecoder;
class AudioDecodeOutput;
class ControllerInputStreaming;
class SettingsManager;
}

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of application-level results.
enum class AppResultCategory : uint8_t {
    Success,
    InitializationFailure,
    InvalidLifecycleOperation,
    TimingFailure,
    FatalRuntimeFailure,
    NormalExit
};

/// A lightweight result returned by lifecycle operations.
/// Preserves a native error code for diagnostics without exposing SDK types
/// in the public interface.
struct AppResult {
    AppResultCategory category;
    uint32_t          nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    AppResult() : category(AppResultCategory::Success), nativeError(0) {}
    AppResult(AppResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == AppResultCategory::Success; }
};

/// Reason why the application loop exited.
/// Stored privately by MoonlightApp; exposed here for the entry point.
enum class ExitReason : uint8_t {
    None,
    UserRequest,
    PlatformCloseRequest,
    InitializationFailure,
    RuntimeFailure,
    InternalTestExit
};

// ---------------------------------------------------------------------------
// MoonlightApp — composition root and lifecycle owner
// ---------------------------------------------------------------------------

class MoonlightApp {
public:
    // --- Lifecycle ----------------------------------------------------------

    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before initialize() is called.
    MoonlightApp();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~MoonlightApp();

    // Non-copyable, non-movable.
    MoonlightApp(const MoonlightApp&) = delete;
    MoonlightApp& operator=(const MoonlightApp&) = delete;
    MoonlightApp(MoonlightApp&&) = delete;
    MoonlightApp& operator=(MoonlightApp&&) = delete;

    /// Initialise shell-level state and timing.
    /// Must be called exactly once, before run().
    /// After failure the object remains safe to shut down or destroy.
    /// run() must not be called after failure.
    AppResult initialize();

    /// Run the main application loop. Requires successful initialize().
    /// Returns only after exit has been requested or a fatal runtime error
    /// occurs. Does not perform final destruction.
    AppResult run();

    /// Request orderly exit. Records the first meaningful reason.
    /// May be called from main-thread code. Does not immediately destroy
    /// subsystems. The loop terminates at a safe iteration boundary.
    void requestExit(ExitReason reason);

    /// Release all initialised resources in reverse dependency order.
    /// Safe after partial initialization, normal loop completion, or
    /// repeated calls (idempotent). Must not throw.
    void shutdown();

private:
    // --- Lifecycle state ----------------------------------------------------

    enum class LifecycleState : uint8_t {
        Constructed,
        Initializing,
        Initialized,
        Running,
        Stopping,
        ShutDown
    };

    LifecycleState  m_state;
    bool            m_running;      ///< Loop-control flag.
    ExitReason      m_exitReason;   ///< First meaningful exit reason.

    // --- Timing state -------------------------------------------------------

    uint64_t        m_prevTimestamp;   ///< Previous frame timestamp (µs).
    uint64_t        m_currTimestamp;   ///< Current frame timestamp (µs).
    float           m_deltaTime;       ///< Frame delta (seconds).

    // --- Subsystems ---------------------------------------------------------

    /// PS4 platform sysmodule lifecycle owner. Owned by MoonlightApp.
    /// Initialized first, before any subsystem that consumes a loaded
    /// sysmodule. Shut down last, after all module consumers have stopped.
    std::unique_ptr<ps4::SysmoduleManager> m_sysmoduleManager;

    /// Platform graphics subsystem. Owned by MoonlightApp.
    std::unique_ptr<ps4::GfxManager> m_gfx;

    /// PS4 controller input subsystem. Owned by MoonlightApp.
    std::unique_ptr<ps4::InputManager> m_input;

    /// PS4 CPU font rasterizer. Owned by MoonlightApp.
    std::unique_ptr<ps4::FontRasterizer> m_fontRasterizer;

    /// CPU UI compositor. Owned by MoonlightApp.
    std::unique_ptr<ps4::UiCompositor> m_uiCompositor;

    /// Dedicated GPU UI blitter. Owned by MoonlightApp.
    std::unique_ptr<ps4::UiBlitter> m_uiBlitter;

    /// CPU Boot screen renderer. Owned by MoonlightApp.
    std::unique_ptr<ui::BootRenderer> m_bootRenderer;

    /// CPU Host List renderer. Owned by MoonlightApp.
    std::unique_ptr<ui::HostListRenderer> m_hostListRenderer;

    /// CPU Host List Grid renderer. Owned by MoonlightApp. Initialized but
    /// dormant: the current presentation style is PlayStation, so the grid
    /// path is not selected.
    std::unique_ptr<ui::HostGridRenderer> m_hostGridRenderer;

    /// CPU Pairing screen renderer. Owned by MoonlightApp. Entered through
    /// normal host activation (PairingManager drives the Pairing screen).
    std::unique_ptr<ui::PairingRenderer> m_pairingRenderer;

    /// CPU Help screen renderer. Owned by MoonlightApp. Draws the Help screen
    /// (in-stream button assignments and chords) opened from the Host/App
    /// List via Touchpad Click.
    std::unique_ptr<ui::HelpRenderer> m_helpRenderer;

    /// CPU App List renderer. Owned by MoonlightApp. Renders the App List
    /// screen fed by the real ApplicationCatalog.
    std::unique_ptr<ui::AppListRenderer> m_appListRenderer;

    /// CPU App List Grid renderer. Owned by MoonlightApp. Renders the App List
    /// Grid presentation mode fed by the real ApplicationCatalog.
    std::unique_ptr<ui::AppGridRenderer> m_appGridRenderer;

    /// CPU Settings overlay renderer. Owned by MoonlightApp. Renders the
    /// Settings overlay (dim + right-side panel + rows) on top of the CPU
    /// Host/App List.
    std::unique_ptr<ui::SettingsRenderer> m_settingsRenderer;

    /// CPU confirmation modal renderer. Owned by MoonlightApp. Renders the
    /// Delete Host confirmation and the Different-app-running modal on top of
    /// the CPU Host/App List.
    std::unique_ptr<ui::ModalRenderer> m_modalRenderer;

    /// CPU Add Host overlay / status renderer. Owned by MoonlightApp. Renders
    /// the Add Host form overlay and status modal on top of the CPU Host List.
    std::unique_ptr<ui::AddHostRenderer> m_addHostRenderer;

    /// CPU in-stream exit hint renderer. Owned by MoonlightApp. Draws the
    /// short-lived exit hint overlay while a stream is active.
    std::unique_ptr<ui::StreamExitHintRenderer> m_streamExitHintRenderer;

    /// CPU in-stream statistics overlay renderer. Owned by MoonlightApp.
    /// Draws the statistics overlay from the live streaming statistics source.
    std::unique_ptr<ui::StreamStatisticsRenderer> m_streamStatisticsRenderer;

    /// CPU in-stream mouse emulation state overlay renderer. Owned by
    /// MoonlightApp. Draws the short-lived "Mouse Emulation: ON/OFF" overlay
    /// when mouse emulation is toggled in-stream.
    std::unique_ptr<ui::MouseEmulationOverlayRenderer> m_mouseEmulationOverlayRenderer;

    /// PS4 platform networking subsystem. Owned by MoonlightApp.
    std::unique_ptr<ps4::NetworkManager> m_network;

    /// Moonlight-specific host discovery subsystem. Owned by MoonlightApp.
    std::unique_ptr<ml::DiscoveryManager> m_discovery;

    /// Authoritative host registry. Owned by MoonlightApp.
    std::unique_ptr<ml::HostManager> m_hostManager;

    /// HTTP/HTTPS client. Owned by MoonlightApp.
    std::unique_ptr<ml::HttpClient> m_httpClient;

    /// ServerInfo query subsystem. Owned by MoonlightApp.
    std::unique_ptr<ml::ServerInfoManager> m_serverInfoManager;

    /// Pairing subsystem. Owned by MoonlightApp. Consumes the persistent
    /// client identity from PairingStore and delegates trust persistence to it.
    /// Initialized after HttpClient and before ServerInfoManager.
    std::unique_ptr<ml::PairingManager> m_pairingManager;

    /// Application Catalog / App List subsystem. Owned by MoonlightApp.
    /// Retrieves and parses the real Sunshine app list for a paired host.
    /// Initialized after ServerInfoManager; shut down before HttpClient.
    std::unique_ptr<ml::ApplicationCatalog> m_applicationCatalog;

    /// Application Artwork acquisition & cache subsystem. Owned by
    /// MoonlightApp. Initialized after ApplicationCatalog; shut down before
    /// HttpClient. Resolves runtime app artwork and feeds it to the App List /
    /// App Grid renderers.
    std::unique_ptr<ml::AppArtworkManager> m_appArtworkManager;

    /// Application Launch / Resume subsystem. Owned by MoonlightApp.
    /// Orchestrates the remote application lifecycle (launch/resume/quit)
    /// protocol for a paired Sunshine host. Initialized after
    /// ApplicationCatalog; shut down before ApplicationCatalog.
    std::unique_ptr<ml::ApplicationSessionManager> m_applicationSessionManager;

    /// Stream Session subsystem. Owned by MoonlightApp.
    /// Owns the active Moonlight streaming connection lifecycle. Accepts a
    /// successful StreamSessionDescriptor from ApplicationSessionManager,
    /// starts LiStartConnection() on a worker, and reports connection-state
    /// updates. Initialized after ApplicationSessionManager; shut down before
    /// ApplicationSessionManager (no active stream may survive the launch
    /// manager teardown).
    std::unique_ptr<ml::StreamSessionManager> m_streamSessionManager;

    /// Video Decode subsystem. Owned by MoonlightApp.
    /// Owns the PS4 Videodec2 hardware H.264 decode path. Consumes encoded
    /// access units forwarded by StreamSessionManager, decodes them on a
    /// worker thread, and produces DecodedVideoFrame objects for the
    /// main-thread presentation path. Initialized after StreamSessionManager;
    /// shut down before StreamSessionManager (no active stream may survive
    /// the decoder teardown).
    std::unique_ptr<ml::VideoDecoder> m_videoDecoder;

    /// Audio Decode/Output subsystem. Owned by MoonlightApp.
    /// Owns the PS4 Opus decode and AudioOut playback path. Consumes encoded
    /// audio packets forwarded by StreamSessionManager, decodes them on a
    /// worker thread, and outputs PCM through the PS4 AudioOut subsystem.
    /// Initialized after StreamSessionManager; shut down before
    /// StreamSessionManager (no active stream may survive the audio teardown).
    std::unique_ptr<ml::AudioDecodeOutput> m_audioDecodeOutput;

    /// Controller Input Streaming subsystem. Owned by MoonlightApp.
    /// Consumes the InputManager's raw controller snapshot and forwards it to
    /// the active Moonlight stream. Initialized after StreamSessionManager;
    /// shut down before StreamSessionManager.
    std::unique_ptr<ml::ControllerInputStreaming> m_controllerInputStreaming;

    /// Persistent stream settings. Owned by MoonlightApp. Loaded once during
    /// initialization; supplies resolution/frame-rate/host-speaker settings
    /// to the launch/resume target construction.
    std::unique_ptr<ml::SettingsManager> m_settingsManager;

    /// Persistent client identity & trusted-host store. Owned by MoonlightApp.
    /// Initialized before network-dependent managers (it has no network
    /// dependency) and used to restore trusted hosts at startup.
    std::unique_ptr<ml::PairingStore> m_pairingStore;

    /// The loaded/generated persistent client identity. Holds the unique ID
    /// and certificate/key material for the current session. Populated during
    /// initialization by PairingStore::ensureIdentity().
    ml::ClientIdentity m_clientIdentity;

    /// Application-level UI component. Owned by MoonlightApp.
    std::unique_ptr<ui::UiManager> m_ui;

    /// UI navigation sound manager. Owned by MoonlightApp.
    /// Plays short UI navigation cues (accept/back/cursor) from preloaded WAV
    /// assets through a dedicated PS4 AudioOut port. Initialized after
    /// AudioDecodeOutput (which calls sceAudioOutInit()); shut down before the
    /// audio subsystem teardown.
    std::unique_ptr<ps4::UiSoundManager> m_uiSound;

    /// PS4 system on-screen keyboard (ImeDialog) helper. Owned by MoonlightApp.
    /// Opens the system OSK for the Add Host overlay's editable fields and
    /// polls its lifecycle each frame. Shut down before UI teardown so no
    /// system dialog is left active while the application tears down its
    /// platform managers.
    std::unique_ptr<ps4::ImeDialog> m_imeDialog;

    // --- ServerInfo scheduling state ----------------------------------------
    /// Tracks the last-scheduled serverinfo generation per HostId.
    /// Used to ignore stale results that arrive after a newer query was
    /// scheduled for the same HostId.
    std::vector<std::pair<ml::HostId, uint32_t>> m_serverInfoGenerations;

    // --- Paired-host HTTPS reachability fallback polling state --------------
    /// Monotonic deadline (µs) for the next fallback reachability probe.
    /// 0 forces an immediate probe on the next update (used at startup for
    /// restored hosts with a persisted endpoint).
    uint64_t m_pairedHostProbeNextUs = 0;
    /// Round-robin index into the host registry for the next fallback probe.
    /// Each poll opportunity probes at most one eligible host, keeping traffic
    /// low and avoiding bursts when several offline trusted hosts exist.
    size_t m_pairedHostProbeIndex = 0;
    /// HostIds currently being probed via the paired-host HTTPS fallback.
    /// Used to emit concise probe-completion diagnostics and to distinguish a
    /// fallback probe result from an ordinary serverinfo query.
    std::vector<ml::HostId> m_probeInFlightHosts;

    // --- Host availability transition diagnostics ---------------------------
    /// Previous per-host availability, used to detect Online->Stale and
    /// Stale->Offline transitions for the temporary [host-poll] diagnostics.
    std::vector<std::pair<ml::HostId, ml::HostAvailability>> m_prevHostAvailability;
    /// Whether m_prevHostAvailability has been seeded (first update).
    bool m_prevHostAvailabilityValid = false;

    // --- Post-pairing auto-open state ---------------------------------------
    /// The HostId of a host that was just successfully paired and whose App
    /// List should open automatically once post-pairing reconciliation
    /// confirms it is Paired. 0 = none. Cleared when reconciliation produces a
    /// definitive result.
    ml::HostId m_pendingPairingAutoOpenHostId = 0;

    // --- Application Catalog state ------------------------------------------
    /// The current catalog's HostId (0 = none).
    ml::HostId m_catalogHostId = 0;
    /// The current catalog request generation.
    uint32_t m_catalogGeneration = 0;
    /// The current catalog's applications (owned by MoonlightApp).
    std::vector<ml::ApplicationInfo> m_catalogApplications;
    /// The current catalog state.
    ml::ApplicationCatalogState m_catalogState = ml::ApplicationCatalogState::Idle;

    // --- Runtime app artwork state -----------------------------------------
    /// Decoded artwork for the active catalog, keyed by ApplicationId. Owned
    /// by MoonlightApp (the composition root) and scoped to the active
    /// host/catalog context. Cleared when the active host/catalog changes.
    /// ApplicationInfo remains catalog metadata; decoded images live here.
    std::unordered_map<ml::ApplicationId, ps4::CpuPngImage> m_runtimeArtwork;

    /// Whether artwork scheduling is active for the current catalog. True
    /// while there are still apps whose artwork has not yet been requested.
    /// Reset to false when a fresh catalog becomes Ready (scheduling restarts)
    /// and when the active host/catalog changes.
    bool m_artworkSchedulingActive = false;

    /// The next catalog app index to schedule artwork for. Advances as
    /// requests are enqueued; used to resume scheduling after the request
    /// queue reports QueueFull (bounded backpressure without an unbounded
    /// pending list).
    size_t m_artworkNextScheduleIndex = 0;

    /// The artwork request generation for the current catalog. Incremented
    /// whenever a fresh catalog becomes Ready so stale results from a previous
    /// catalog/host are rejected.
    uint64_t m_artworkGeneration = 0;

    /// The serverId of the host whose artwork is currently being resolved.
    /// Used to reject stale results that arrive after a host change.
    std::string m_artworkServerId;

    /// AppIds whose runtime artwork changed since the last render. Consumed by
    /// the App List / App Grid dirty-region computation in render() to
    /// invalidate only the affected card regions. Cleared after each render.
    std::vector<ml::ApplicationId> m_artworkDirtyApps;

    // --- App List serverinfo polling state ----------------------------------
    /// The host whose App List is currently being polled (0 = not polling).
    /// Polling is active only while the App List screen is shown for this host
    /// and no stream is active.
    ml::HostId m_appListPollHostId = 0;
    /// Monotonic deadline (µs) for the next App List serverinfo poll.
    /// 0 forces an immediate refresh on the next update.
    uint64_t m_appListNextPollUs = 0;

    // --- Application Launch state -------------------------------------------
    /// The current launch's HostId (0 = none).
    ml::HostId m_launchHostId = 0;
    /// The current launch's ApplicationId (0 = none).
    ml::ApplicationId m_launchAppId = 0;
    /// The current launch request generation.
    uint32_t m_launchGeneration = 0;
    /// Whether a launch/resume operation is currently active.
    bool m_launchActive = false;
    /// The pending successful StreamSessionDescriptor (owned by MoonlightApp).
    /// Transient; valid only for the current launch/session.
    std::optional<ml::StreamSessionDescriptor> m_pendingSessionDescriptor;

    // --- Transient local Connecting state ----------------------------------
    /// The host this client is currently connecting to (0 = none).
    /// This is a client-local transient launch state, distinct from the
    /// authoritative Host.currentGame. It is set when a launch/resume is
    /// genuinely accepted and cleared when the launch/connect fails/cancels.
    /// It is NOT a persistent running-state field; Host.currentGame remains
    /// authoritative for Sunshine's actual running application.
    ml::HostId m_connectingHostId = 0;
    /// The application this client is currently connecting to (0 = none).
    ml::ApplicationId m_connectingAppId = 0;
    /// The launch generation associated with the current Connecting state.
    /// Used to reject stale completion events from a previous launch/host.
    uint32_t m_connectingGeneration = 0;

    // --- Stream presentation state -----------------------------------------
    /// The current stream's HostId (0 = none).
    ml::HostId m_streamHostId = 0;
    /// The current stream's ApplicationId (0 = none).
    ml::ApplicationId m_streamAppId = 0;
    /// Whether the stream is active and the main thread should present decoded
    /// video frames instead of the CPU UI compositor. Set on Connected, cleared
    /// on stream stop/termination/failure.
    bool m_streamActive = false;
    /// The decoded frame currently held for presentation (main-thread owned).
    /// Released and re-acquired each render() while streaming.
    ml::DecodedVideoFrame m_heldDecodedFrame;
    /// Whether a decoded frame has been presented at least once (for the
    /// "[video] first frame presented" diagnostic).
    bool m_streamFirstFramePresented = false;
    /// Whether the "[stream] statistics overlay composed" diagnostic has been
    /// logged for the current stream session. Reset on stream connect so the
    /// line is emitted at most once per stream launch.
    bool m_streamStatsOverlayLogged = false;

    /// The mouse emulation state observed on the previous frame. Used to
    /// detect a rising/falling edge (toggle) of mouse emulation in-stream and
    /// drive the transient mouse emulation state overlay. Reset on stream
    /// connect (mouse emulation always starts OFF).
    bool m_prevMouseEmulationEnabled = false;

    // --- Stream frame-lifetime management (Phase 3) -------------------------
    /// Retired decoded frames whose surfaces must not be returned to the
    /// decoder pool until the GPU is confirmed done with them. In the CPU
    /// path, a frame is retired when a newer frame is acquired; it is released
    /// on the next beginFrame() (which confirms the previous frame was
    /// presented). This prevents the decoder from reusing a surface that the
    /// renderer (or the decoder's DPB) may still reference, which otherwise
    /// produces stale-content corruption that moves with animated content.
    std::vector<ml::DecodedVideoFrame> m_retiredFrames;
    /// Per-graphics-slot reference to the decoded frame drawn to that slot.
    /// Used to detect whether a surface was reused (generation advanced) while
    /// a graphics slot still referenced it.
    struct StreamSlotRef {
        uint64_t frameNumber = 0;
        uint32_t surfaceSlot = 0;
        uint32_t generation = 0;
        bool valid = false;
    };
    StreamSlotRef m_streamSlotRefs[2];  ///< Matches GfxManager double buffering.

    // --- Stream statistics overlay state ------------------------------------
    /// Monotonic deadline (µs) for the next statistics snapshot update.
    /// 0 forces an immediate snapshot on the next update (used on stream
    /// connect so the first snapshot is captured immediately).
    uint64_t m_streamStatsNextUpdateUs = 0;
    /// Monotonic time (µs) of the previous statistics snapshot. Used to
    /// compute interval deltas (FPS, decode time, bitrate).
    uint64_t m_streamStatsPrevTimeUs = 0;
    /// Previous cumulative counters for interval-delta computation.
    uint64_t m_streamStatsPrevDecodedFrames = 0;
    uint64_t m_streamStatsPrevDecodeTotalUs = 0;
    uint64_t m_streamStatsPrevDecodeCount = 0;
    uint64_t m_streamStatsPrevRxBytes = 0;

    // --- Application Quit state ---------------------------------------------
    /// The current quit's HostId (0 = none).
    ml::HostId m_quitHostId = 0;
    /// The current quit's ApplicationId (0 = none).
    ml::ApplicationId m_quitAppId = 0;
    /// The current quit request generation.
    uint32_t m_quitGeneration = 0;
    /// Whether a remote quit operation is currently active.
    bool m_quitActive = false;

    // --- Quit-and-launch sequencing -----------------------------------------
    /// Pending app to launch after a successful quit (0 = none). Set when the
    /// user confirms the "Quit running app and launch this one?" dialog; the
    /// launch is issued once the quit completes successfully.
    ml::HostId m_pendingLaunchAfterQuitHostId = 0;
    ml::ApplicationId m_pendingLaunchAfterQuitAppId = 0;
    /// Bypass the different-app-running launch guard for the single post-quit
    /// launch. After a successful quit, Host.currentGame is still stale until
    /// the serverinfo refresh lands, so the guard would otherwise block the
    /// intended launch.
    bool m_allowLaunchAfterQuit = false;
    /// Whether we are waiting for the host to confirm the quit completed
    /// (Host.currentGame == 0) before issuing the pending post-quit launch.
    /// The /cancel request only confirms the quit was accepted, not that the
    /// app has actually stopped; we wait for the authoritative serverinfo
    /// currentGame to reach 0 before launching the pending app.
    bool m_waitingForQuitToComplete = false;

    // --- Settings overlay state ---------------------------------------------
    /// Whether the Settings overlay is currently open. While open, App List
    /// serverinfo polling is suspended and ordinary UI input is routed only
    /// to the Settings overlay.
    bool m_settingsOverlayOpen = false;

    // --- Add Host status tracking -------------------------------------------
    /// The HostId of a pending manual host add whose serverinfo result is
    /// awaited to update the Add Host status modal (0 = none).
    ml::HostId m_pendingManualAddHostId = 0;
    /// Whether the pending manual host was already discovered (matched an
    /// existing host) rather than newly created.
    bool m_pendingManualAddWasExisting = false;

    // --- Dirty-state / change-tracking --------------------------------------
    /// Whether the CPU Boot surface has been composed for the current session.
    bool m_bootComposed = false;

    /// The previous Host List render snapshot used to detect changes and
    /// compute dirty regions.
    ui::HostListRenderState m_prevHostListState;
    /// Whether the previous Host List snapshot is valid.
    bool m_prevHostListValid = false;

    /// The previous Host List Grid render snapshot (dormant path).
    ui::HostGridRenderState m_prevHostGridState;
    /// Whether the previous Host List Grid snapshot is valid.
    bool m_prevHostGridValid = false;

    /// The previous Pairing render snapshot (dormant path).
    ui::PairingRenderState m_prevPairingState;
    /// Whether the previous Pairing snapshot is valid.
    bool m_prevPairingValid = false;

    /// The previous Help render snapshot.
    ui::HelpRenderState m_prevHelpState;
    /// Whether the previous Help snapshot is valid.
    bool m_prevHelpValid = false;

    /// The previous App List render snapshot (dormant path).
    ui::AppListRenderState m_prevAppListState;
    /// Whether the previous App List snapshot is valid.
    bool m_prevAppListValid = false;

    /// The previous App List Grid render snapshot (dormant path).
    ui::AppGridRenderState m_prevAppGridState;
    /// Whether the previous App List Grid snapshot is valid.
    bool m_prevAppGridValid = false;

    /// Whether the Settings overlay was open on the previous CPU-active frame.
    bool m_prevSettingsOpen = false;
    /// The previous Settings render snapshot (for dirty computation).
    ui::SettingsRenderState m_prevSettingsState;

    /// Whether the Delete Host confirmation was open on the previous
    /// CPU-active frame.
    bool m_prevDeleteConfirmOpen = false;
    /// Whether the Quit App confirmation dialog was open on the previous
    /// CPU-active frame.
    bool m_prevQuitAppOpen = false;
    /// The previous Delete Host modal render snapshot (for dirty computation).
    ui::ModalRenderState m_prevDeleteModalState;
    /// The previous Quit App modal render snapshot (for dirty computation).
    ui::ModalRenderState m_prevQuitModalState;

    /// Whether the Add Host overlay or status modal was open on the previous
    /// CPU-active frame.
    bool m_prevAddHostOpen = false;
    /// The previous Add Host render snapshot (for dirty computation).
    ui::AddHostRenderState m_prevAddHostState;

    /// Host snapshot change tracking: only push to UiManager when the
    /// authoritative registry actually changed.
    std::vector<ml::Host> m_lastUiHosts;
    bool m_lastUiHostsValid = false;

    // --- Main-loop phase methods --------------------------------------------

    /// Phase 1: Poll platform and lifecycle events.
    void pollPlatformEvents();

    /// Phase 2: Poll and normalise user input.
    void pollInput();

    /// Phase 3: Consume completed worker results.
    /// `nowMs` is the current monotonic application time in milliseconds.
    void processAsyncResults(uint64_t nowMs);

    /// Phase 4: Update application and screen state.
    void update(float deltaTime);

    /// Phase 5: Prepare and submit rendering.
    void render();

    /// Phase 6: Present the completed frame.
    void present();

    // --- Internal helpers ---------------------------------------------------
    /// Read the monotonic clock and produce a clamped delta for this frame.
    void updateTiming();

    /// Try to transition the lifecycle state; returns false on invalid
    /// transition (programmer error).
    bool transitionTo(LifecycleState next);

    /// Log a lifecycle transition.
    void logTransition(const char* label);

    /// Handle a UI intent emitted by UiManager.
    void handleUiIntent(const ui::UiIntent& intent);

    /// Handle a pairing update emitted by PairingManager.
    void handlePairingUpdate(const ml::PairingUpdate& update);

    /// Handle a RequestDeleteHost intent.
    /// Persists the removal of the host's local trust/pairing record, then
    /// removes the host from the HostManager registry so it disappears from
    /// the Host List. On persistence failure the host is left in place.
    void handleDeleteHost(ml::HostId hostId);

    /// Reconcile a host's application-visible pairing state using local trust
    /// (PairingManager/PairingStore) and the server-reported pairing status.
    void reconcileHostPairingState(ml::HostId hostId);

    /// Schedule a serverinfo query for a host.
    /// Called when a host is Added or returns Online.
    void scheduleServerInfoQuery(ml::HostId hostId);

    /// Build a ServerInfoTarget for a host using the same construction as
    /// scheduleServerInfoQuery. Returns false if the host is invalid.
    bool buildServerInfoTarget(ml::HostId hostId, ml::ServerInfoTarget& outTarget);

    /// Start an App List retrieval for a Paired host.
    /// Validates preconditions, builds the ApplicationCatalogTarget, and
    /// enters the App List loading state.
    void startApplicationCatalog(ml::HostId hostId);

    /// Handle an ApplicationCatalog update emitted by ApplicationCatalog.
    void handleApplicationCatalogUpdate(const ml::ApplicationCatalogUpdate& update);

    /// Begin artwork resolution for the current catalog. Clears runtime
    /// artwork belonging to the old context, resets scheduling state, and
    /// starts scheduling artwork requests for the new catalog's apps. Called
    /// when a catalog becomes Ready.
    void beginCatalogArtwork();

    /// Schedule a bounded batch of artwork requests for the current catalog.
    /// Called once per frame from update() while scheduling is active. Stops
    /// when the request queue reports QueueFull (resumes next frame) or when
    /// all apps have been scheduled. No per-frame duplicate spam: the manager
    /// suppresses AlreadyPending.
    void scheduleArtworkBatch();

    /// Process a completed artwork result on the main thread. Verifies the
    /// result belongs to the current host/catalog context, rejects stale
    /// generation results, verifies the app still exists in the active
    /// catalog, and on success moves the decoded image into runtime artwork
    /// state. Returns true if the affected card should be invalidated.
    /// Takes a non-const reference so the decoded image can be moved out.
    bool handleArtworkResult(ml::AppArtworkResult& result);

    /// Clear runtime artwork and reset scheduling state for the current
    /// host/catalog context. The persistent disk cache is untouched.
    void clearRuntimeArtwork();

    /// Populate the resolved runtime artwork pointers into an App List render
    /// state. Null means fall back to app_fallback.png. Used by both the
    /// initial seed (full compose) and the per-frame render path so the seeded
    /// surface and subsequent recomposes always use the same artwork.
    void populateAppListArtwork(ui::AppListRenderState& state);

    /// Populate the resolved runtime artwork pointers into an App Grid render
    /// state. Null means fall back to app_fallback.png. Used by both the
    /// initial seed (full compose) and the per-frame render path so the seeded
    /// surface and subsequent recomposes always use the same artwork.
    void populateAppGridArtwork(ui::AppGridRenderState& state);

    /// Rebuild the App List presentation view from the current catalog and the
    /// authoritative Host.currentGame running state. Pushes the updated view
    /// to UiManager. No network activity.
    void refreshAppListView();

    /// Clear the transient local Connecting state (if any).
    /// Called when the launch/connect sequence fails or is cancelled. Does not
    /// touch Host.currentGame.
    void clearConnectingState();

    /// Handle an ActivateApplication intent.
    /// Validates preconditions, builds the ApplicationLaunchTarget, and
    /// starts the launch/resume operation.
    void handleActivateApplication(ml::HostId hostId, ml::ApplicationId appId);

    /// Handle an ApplicationSessionManager update emitted by the launch worker.
    void handleApplicationLaunchUpdate(const ml::ApplicationLaunchUpdate& update);

    /// Handle a QuitApplication intent.
    /// Validates the host/app, builds the ApplicationQuitTarget, and starts
    /// the remote quit operation. Returns true if the quit was scheduled.
    bool handleQuitApplication(ml::HostId hostId, ml::ApplicationId appId);

    /// Handle a QuitAndLaunchApplication intent: quit the running app, then
    /// launch the focused app once the quit completes successfully.
    void handleQuitAndLaunchApplication(ml::HostId hostId,
                                        ml::ApplicationId quitAppId,
                                        ml::ApplicationId launchAppId);

    /// Handle an ApplicationSessionManager quit update emitted by the quit worker.
    void handleApplicationQuitUpdate(const ml::ApplicationQuitUpdate& update);

    /// Launch the pending post-quit app once the host confirms the quit
    /// completed (Host.currentGame == 0). Called after each serverinfo refresh
    /// for the current App List host while a quit-and-launch is waiting.
    void maybeLaunchPendingAfterQuit();

    /// Handle a StreamSessionManager connection-state update emitted by the
    /// stream lifecycle worker. Applies the update to the UI/state and clears
    /// transient connecting state on terminal outcomes.
    void handleStreamSessionUpdate(const ml::StreamSessionUpdate& update);

    /// Handle an AudioDecodeOutput update emitted by the audio worker.
    void handleAudioDecodeOutputUpdate(const ml::AudioDecodeOutputUpdate& update);

    /// Present the latest decoded video frame into the current GfxManager
    /// framebuffer. Converts NV12→BGRA on the CPU. Called from render() while
    /// the stream is active. Returns true if a frame was presented.
    bool presentStreamVideoFrame();

    /// Compose the in-stream overlay (exit hint / statistics) over the decoded
    /// video already in the current framebuffer. Called from render() while
    /// the stream is active, after presentStreamVideoFrame(). Draws the active
    /// overlay into the CPU compositor surface (transparent background) and
    /// alpha-blends it over the framebuffer, preserving the underlying video.
    /// No-op when no overlay is active (StreamEmpty).
    void composeStreamOverlay();

    /// Clear a region of the CPU compositor surface to fully transparent.
    /// Used before drawing an in-stream overlay so the compositor region holds
    /// only the freshly drawn overlay pixels (no stale content from a previous
    /// overlay or the CPU UI).
    void clearCompositorRegion(const ps4::Rect& rect);

    /// Update the in-stream statistics display snapshot once per second while
    /// a stream is active and statistics are enabled. Reads cumulative
    /// counters from the subsystems, computes interval deltas, and pushes the
    /// resulting snapshot to UiManager. No-op when statistics are disabled or
    /// no stream is active.
    void updateStreamStatistics();

    /// Reset the stream statistics interval baseline for a new stream session.
    /// Called on stream connect so the first snapshot captures an immediate
    /// baseline and no counters leak from a previous session.
    void resetStreamStatisticsBaseline();

    /// Automatically hand off a successful launch/resume result to the
    /// StreamSessionManager. Called from handleApplicationLaunchUpdate on a
    /// successful launch/resume. Copies the descriptor into the stream manager
    /// and starts the stream session.
    void startStreamSession(const ml::StreamSessionDescriptor& session);

    /// Open the Settings overlay: populate the UiManager's working copy from
    /// the authoritative SettingsManager values and mark the overlay open.
    void openSettingsOverlay();

    /// Close the Settings overlay: commit the edited values from UiManager to
    /// SettingsManager and persist them to disk.
    void closeSettingsOverlay(const ui::SettingsViewState& edited);

    /// Request an authenticated serverinfo refresh for the current App List
    /// host without the per-request diagnostic log (used by the polling
    /// cadence). Reuses buildServerInfoTarget and ServerInfoManager duplicate
    /// suppression.
    void requestAppListServerInfo(ml::HostId hostId);

    /// Update the App List serverinfo polling cadence. Called once per frame
    /// from update(). While the App List screen is active for a host, requests
    /// an authenticated serverinfo refresh immediately on entry and then
    /// approximately once per second, so externally-triggered launch/quit/
    /// switch changes are reflected. No-op when not on the App List.
    void updateAppListPolling();

    /// Open the PS4 ImeDialog for the currently selected Add Host field.
    /// Called when the user presses Cross on an editable field in the Add Host
    /// overlay. Returns true if the dialog was opened.
    bool openAddHostIme();

    /// Poll the active Add Host ImeDialog lifecycle. Called once per frame
    /// from update(). When the dialog closes, the returned text is routed back
    /// to the Add Host overlay.
    void updateAddHostIme();

    /// Handle a RequestAddHost intent.
    /// Adds a manual host (numeric IPv4 + bootstrap port) to the HostManager
    /// registry and schedules a serverinfo query through the existing path.
    void handleAddHost(const std::string& ipv4, uint16_t port);

    /// Handle a CancelAddHost intent.
    /// Aborts the pending manual host add: cancels the in-flight serverinfo
    /// worker for the pending host, removes the temporary bootstrap placeholder
    /// (if it is still a placeholder), and clears the pending manual-add state.
    void handleCancelAddHost();

    /// Push the current host snapshot into UiManager when the authoritative
    /// registry actually changed.
    void updateUiHostSnapshot();

    /// Compute the dirty rectangles for a Host List state change from `prev`
    /// to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// HostListRenderer visual-bounds helpers. Returns 0 when nothing changed.
    int hostListDirtyRects(const ui::HostListRenderState& prev,
                           const ui::HostListRenderState& cur,
                           ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for a Host List Grid state change from
    /// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// HostGridRenderer visual-bounds helpers. Returns 0 when nothing changed.
    int hostGridDirtyRects(const ui::HostGridRenderState& prev,
                           const ui::HostGridRenderState& cur,
                           ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for an App List state change from `prev`
    /// to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// AppListRenderer visual-bounds helpers. Returns 0 when nothing changed.
    /// Detects selection moves, per-card content changes (running, connecting,
    /// quitting, title, artwork), structural changes (app added/removed), and
    /// crossIsAccept changes — matching the Host List dirty-region behavior.
    int appListDirtyRects(const ui::AppListRenderState& prev,
                          const ui::AppListRenderState& cur,
                          ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for an App List Grid state change from
    /// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// AppGridRenderer visual-bounds helpers. Returns 0 when nothing changed.
    /// Detects selection/scroll movement, per-card content changes (running,
    /// connecting, quitting, title, artwork), structural changes (app
    /// added/removed), and crossIsAccept changes — matching the Host Grid
    /// dirty-region behavior.
    int appGridDirtyRects(const ui::AppGridRenderState& prev,
                          const ui::AppGridRenderState& cur,
                          ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for a Settings overlay state change from
    /// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// SettingsRenderer visual-bounds helpers. Returns 0 when nothing changed.
    /// On a selection change, dirties BOTH the old and new selected row bounds
    /// so the previous highlight is cleared and the new one is drawn.
    int settingsDirtyRects(const ui::SettingsRenderState& prev,
                           const ui::SettingsRenderState& cur,
                           bool prevOpen, bool curOpen,
                           ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for a confirmation modal state change from
    /// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
    /// ModalRenderer visual-bounds helpers. Returns 0 when nothing changed.
    int modalDirtyRects(const ui::ModalRenderState& prev,
                        const ui::ModalRenderState& cur,
                        bool prevOpen, bool curOpen,
                        ps4::Rect* out, int maxOut);

    /// Compute the dirty rectangles for an Add Host overlay / status modal
    /// state change from `prev` to `cur`. Fills `out` (bounded) and returns
    /// the count. Uses the AddHostRenderer visual-bounds helpers. Returns 0
    /// when nothing changed. Detects field-text changes (IP/Port from the
    /// ImeDialog), selection moves, validation-error changes, and status-modal
    /// message/state changes.
    int addHostDirtyRects(const ui::AddHostRenderState& prev,
                          const ui::AddHostRenderState& cur,
                          bool prevOpen, bool curOpen,
                          ps4::Rect* out, int maxOut);

    /// Restore persisted trusted hosts into the HostManager registry.
    /// Called during initialization after PairingStore and HostManager are
    /// available and before discovery starts. Each persisted trusted-host
    /// record is restored as an Offline, locally-trusted host.
    void restoreTrustedHosts();

    /// Update the paired-host HTTPS reachability fallback polling cadence.
    /// Called once per frame from update(). At a low frequency (~5 s), probes
    /// at most one eligible paired host that is not currently observed via
    /// mDNS and has a known endpoint. A successful probe marks the host
    /// reachable (Online) and refreshes its metadata. No-op when no eligible
    /// host exists or another serverinfo request is already pending.
    void updatePairedHostProbe();

    /// Probe a single paired host via the authenticated HTTPS serverinfo path
    /// using a short bounded timeout. Returns true if a probe was scheduled.
    /// Skips hosts already covered by an active/pending serverinfo request.
    bool probePairedHost(ml::HostId hostId);

    /// Persist a trusted host's last-known endpoint (IPv4 + discovery port +
    /// HTTPS port) when it has changed. Only writes when a relevant stored
    /// value actually changes. No-op for unpaired hosts.
    void persistHostEndpoint(ml::HostId hostId);

    /// Emit concise [host-poll] transition diagnostics (Online->Stale,
    /// Stale->Offline) by comparing the current host availability against the
    /// previously tracked state. Offline->Online transitions are logged in
    /// processAsyncResults where the source (mdns/https) is known. No-op until
    /// the previous-state map has been seeded.
    void logHostAvailabilityTransitions(uint64_t nowMs);
};

#endif // MOONLIGHT_APP_H

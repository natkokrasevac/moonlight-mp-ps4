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

// === MoonlightApp.cpp ========================================================
//
// OpenOrbis Moonlight client — composition root and lifecycle owner.
//
// Implements the MoonlightApp coordinator:
//   - Explicit lifecycle state machine.
//   - Single visible main loop with the required phase order.
//   - Monotonic PS4 clock timing with large-delta clamping.
//   - Deterministic initialisation and shutdown.
//   - UI intent routing.
//   - Screen/render dispatch.
//   - Render-state change tracking.
//
// This is the OpenOrbis port of the SCE MoonlightApp coordinator. It owns the
// full production subsystem tree: platform graphics/input/network, host
// discovery and registry, pairing, HTTP/HTTPS, application catalog and
// artwork, application launch/resume/quit, stream session, video decode,
// audio decode/output, controller input streaming, and the CPU UI renderers.
//
// OpenOrbis SDK includes are confined to this implementation file.
// ===============================================================================

#include "MoonlightApp.h"

// --- Platform graphics subsystem ---------------------------------------------
#include "platform/ps4/GfxManager.h"
#include "platform/ps4/InputManager.h"
#include "platform/ps4/NetworkManager.h"
#include "platform/ps4/FontRasterizer.h"   // CPU font rasterizer.
#include "platform/ps4/UiCompositor.h"     // CPU UI compositor.
#include "platform/ps4/UiBlitter.h"        // Dedicated GPU UI blitter.
#include "platform/ps4/UiSoundManager.h"   // UI navigation sound manager.
#include "platform/ps4/SysmoduleManager.h" // PS4 sysmodule lifecycle owner.
#include "platform/ps4/ImeDialog.h"        // PS4 system on-screen keyboard.
#include "platform/ps4/Nv12Converter.h"    // NV12→BGRA CPU conversion.

// --- Moonlight-specific host discovery subsystem, host registry, HTTP, and ---
// --- serverinfo query subsystem ----------------------------------------------
#include "moonlight/DiscoveryManager.h"
#include "moonlight/HostManager.h"
#include "moonlight/HttpClient.h"
#include "moonlight/ServerInfoManager.h"
#include "moonlight/PairingStore.h"
#include "moonlight/PairingManager.h"
#include "moonlight/ApplicationCatalog.h"
#include "moonlight/AppArtworkManager.h"
#include "moonlight/ApplicationSessionManager.h"
#include "moonlight/StreamSessionManager.h"
#include "moonlight/VideoDecoder.h"
#include "moonlight/AudioDecodeOutput.h"
#include "moonlight/ControllerInputStreaming.h"
#include "moonlight/SettingsManager.h"

// --- UI manager and renderers ------------------------------------------------
#include "ui/UiManager.h"
#include "ui/BootRenderer.h"
#include "ui/HostListRenderer.h"
#include "ui/HostGridRenderer.h"
#include "ui/PairingRenderer.h"
#include "ui/HelpRenderer.h"
#include "ui/AppListRenderer.h"
#include "ui/AppGridRenderer.h"
#include "ui/SettingsRenderer.h"
#include "ui/ModalRenderer.h"
#include "ui/AddHostRenderer.h"
#include "ui/StreamExitHintRenderer.h"
#include "ui/StreamStatisticsRenderer.h"
#include "ui/MouseEmulationOverlayRenderer.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/libkernel.h>   // sceKernelGetProcessTime

#include <emmintrin.h>   // SSE2 — Jaguar PS4 (fast NV12→BGRA)

#include <cstdio>         // printf, fflush, snprintf
#include <cstring>        // memset

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Surface dimensions (must match the compositor and framebuffer).
static constexpr uint32_t kWidth  = 1920;
static constexpr uint32_t kHeight = 1080;

/// Fallback background color used if the production background image fails to
/// decode (dark blue-gray).
static constexpr uint8_t kFallbackBgR = 0x40;
static constexpr uint8_t kFallbackBgG = 0x40;
static constexpr uint8_t kFallbackBgB = 0x40;

/// Maximum delta time (seconds) passed to update(). Prevents extreme time
/// steps after debugging, suspension, or system interruptions.
static constexpr float kMaxDeltaSeconds = 0.25f;

/// First-frame delta time (seconds).
static constexpr float kFirstFrameDeltaSeconds = 0.0f;

/// Microseconds per second.
static constexpr double kUsecPerSec = 1000000.0;

/// Paired-host HTTPS reachability fallback poll interval (microseconds).
/// This is a low-frequency fallback cadence (~5 s), deliberately much slower
/// than the 1-second mDNS cadence. It is used only for paired hosts that are
/// not currently observed via mDNS but have a known persisted endpoint.
static constexpr uint64_t kPairedHostProbeIntervalUs = 5000000ull;

/// App List serverinfo poll interval (microseconds).
/// While the App List screen is active, the selected host's authenticated
/// serverinfo is refreshed approximately once per second so externally-
/// triggered application launch/quit/switch changes are reflected.
static constexpr uint64_t kAppListPollIntervalUs = 1000000ull;

/// Short bounded timeout (ms) for a background reachability probe.
/// Normal serverinfo requests can remain pending for a long time against an
/// unreachable IP; a fallback probe must use a short bound so it does not
/// occupy the ServerInfoManager worker for tens of seconds.
static constexpr uint32_t kPairedHostProbeTimeoutMs = 1500;

/// mDNS freshness threshold (ms) used to decide whether a host is currently
/// being observed via mDNS. Mirrors HostManager's kHostStaleAfterMs. A host is
/// considered "currently observed via mDNS" while its last mDNS observation is
/// fresher than this; once mDNS stops, the fallback probe becomes eligible.
static constexpr uint64_t kMdnsFreshAfterMs = 12000;

/// In-stream statistics snapshot refresh interval (microseconds).
/// The statistics overlay is refreshed at a bounded ~1 Hz cadence; network
/// stats are sampled from the main thread at this cadence, not per draw.
static constexpr uint64_t kStreamStatsIntervalUs = 1000000ull;

/// Diagnostic verbosity: log the per-request serverinfo "applied" line.
/// Default OFF to keep the klog quiet during normal operation.
static constexpr bool kLogServerInfoApplied = false;

/// Diagnostic verbosity: log the per-host pairing-state reconciliation line.
/// Default OFF to keep the klog quiet during normal operation.
static constexpr bool kLogReconciledPairing = false;

/// Diagnostic verbosity: log the periodic frame counter line.
/// Default OFF to keep the klog quiet during normal operation.
static constexpr bool kLogFrameCounter = false;

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; not a speculative logging subsystem)
// ---------------------------------------------------------------------------

/// Write a diagnostic line to stdout, which is captured by the PS4 klog
/// server when running from a fake PKG on a jailbroken console.
static void diagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

MoonlightApp::MoonlightApp()
    : m_state(LifecycleState::Constructed)
    , m_running(false)
    , m_exitReason(ExitReason::None)
    , m_prevTimestamp(0)
    , m_currTimestamp(0)
    , m_deltaTime(0.0f)
    , m_sysmoduleManager(new ps4::SysmoduleManager())
    , m_gfx(new ps4::GfxManager())
    , m_input(new ps4::InputManager())
    , m_fontRasterizer(new ps4::FontRasterizer())
    , m_uiCompositor(new ps4::UiCompositor())
    , m_uiBlitter(new ps4::UiBlitter())
    , m_bootRenderer(new ui::BootRenderer())
    , m_hostListRenderer(new ui::HostListRenderer())
    , m_hostGridRenderer(new ui::HostGridRenderer())
    , m_pairingRenderer(new ui::PairingRenderer())
    , m_helpRenderer(new ui::HelpRenderer())
    , m_appListRenderer(new ui::AppListRenderer())
    , m_appGridRenderer(new ui::AppGridRenderer())
    , m_settingsRenderer(new ui::SettingsRenderer())
    , m_modalRenderer(new ui::ModalRenderer())
    , m_addHostRenderer(new ui::AddHostRenderer())
    , m_streamExitHintRenderer(new ui::StreamExitHintRenderer())
    , m_streamStatisticsRenderer(new ui::StreamStatisticsRenderer())
    , m_mouseEmulationOverlayRenderer(new ui::MouseEmulationOverlayRenderer())
    , m_network(new ps4::NetworkManager())
    , m_discovery(new ml::DiscoveryManager())
    , m_hostManager(new ml::HostManager())
    , m_httpClient(new ml::HttpClient())
    , m_serverInfoManager(new ml::ServerInfoManager())
    , m_pairingManager(new ml::PairingManager())
    , m_applicationCatalog(new ml::ApplicationCatalog())
    , m_appArtworkManager(new ml::AppArtworkManager())
    , m_applicationSessionManager(new ml::ApplicationSessionManager())
    , m_streamSessionManager(new ml::StreamSessionManager())
    , m_videoDecoder(new ml::VideoDecoder())
    , m_audioDecodeOutput(new ml::AudioDecodeOutput())
    , m_controllerInputStreaming(new ml::ControllerInputStreaming())
    , m_settingsManager(new ml::SettingsManager())
    , m_pairingStore(new ml::PairingStore())
    , m_ui(new ui::UiManager())
    , m_uiSound(new ps4::UiSoundManager())
    , m_imeDialog(new ps4::ImeDialog())
{
    diagPrint("[app] MoonlightApp constructed");
}

MoonlightApp::~MoonlightApp() {
    // Destructor is a final safety net. Orderly shutdown must be performed
    // by calling shutdown() explicitly before destruction.
    if (m_state != LifecycleState::ShutDown) {
        diagPrint("[app] ~MoonlightApp: shutdown() was not called before destruction");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle transitions
// ---------------------------------------------------------------------------

bool MoonlightApp::transitionTo(LifecycleState next) {
    // Conceptual state machine:
    //   Constructed -> Initializing -> Initialized -> Running -> Stopping -> ShutDown
    //   Initializing -> Stopping (failure path)
    //   Running      -> Stopping (exit path)
    //
    // Unexpected transitions are programmer errors.
    switch (m_state) {
    case LifecycleState::Constructed:
        if (next == LifecycleState::Initializing) {
            m_state = next;
            return true;
        }
        break;
    case LifecycleState::Initializing:
        if (next == LifecycleState::Initialized ||
            next == LifecycleState::Stopping) {
            m_state = next;
            return true;
        }
        break;
    case LifecycleState::Initialized:
        if (next == LifecycleState::Running) {
            m_state = next;
            return true;
        }
        break;
    case LifecycleState::Running:
        if (next == LifecycleState::Stopping) {
            m_state = next;
            return true;
        }
        break;
    case LifecycleState::Stopping:
        if (next == LifecycleState::ShutDown) {
            m_state = next;
            return true;
        }
        break;
    case LifecycleState::ShutDown:
        // No transitions out of ShutDown.
        break;
    }
    return false;
}

void MoonlightApp::logTransition(const char* label) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[app] %s", label);
    diagPrint(buf);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void MoonlightApp::updateTiming() {
    // Read the monotonic process-time clock (microseconds).
    // sceKernelGetProcessTime() returns elapsed process time in microseconds
    // from a monotonic source.
    m_prevTimestamp = m_currTimestamp;
    m_currTimestamp  = sceKernelGetProcessTime();

    // First frame: prev == curr, delta is explicitly zero.
    if (m_prevTimestamp == 0) {
        m_deltaTime = kFirstFrameDeltaSeconds;
        return;
    }

    // Calculate delta in seconds. The clock is monotonic, so the
    // subtraction is well-defined.
    uint64_t elapsedUsec = m_currTimestamp - m_prevTimestamp;
    double dt = static_cast<double>(elapsedUsec) / kUsecPerSec;

    // Clamp extreme delta values.
    if (dt > static_cast<double>(kMaxDeltaSeconds)) {
        dt = static_cast<double>(kMaxDeltaSeconds);
    }
    if (dt < 0.0) {
        dt = 0.0;
    }

    m_deltaTime = static_cast<float>(dt);
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

AppResult MoonlightApp::initialize() {
    logTransition("initialisation start");

    if (!transitionTo(LifecycleState::Initializing)) {
        logTransition("initialisation failed: invalid state");
        return AppResult(AppResultCategory::InvalidLifecycleOperation);
    }

    // 1. Initialise monotonic timing state.
    m_currTimestamp = sceKernelGetProcessTime();
    if (m_currTimestamp == 0) {
        // sceKernelGetProcessTime() returning 0 is unlikely on real hardware
        // but represents a timing failure.
        logTransition("initialisation failed: timing unavailable");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::TimingFailure);
    }
    m_prevTimestamp = m_currTimestamp;
    m_deltaTime     = kFirstFrameDeltaSeconds;

    // 2. Initialize the PS4 platform sysmodule lifecycle.
    //    SysmoduleManager is the sole owner of sceSysmoduleLoadModule*() /
    //    sceSysmoduleUnloadModule*(). It must be initialized FIRST, before any
    //    subsystem that consumes a loaded sysmodule (FontRasterizer,
    //    NetworkManager, HttpClient, future PairingStore). If a required
    //    module fails to load, initialization fails.
    if (!m_sysmoduleManager->initialize()) {
        diagPrint("[app] SysmoduleManager initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] SysmoduleManager initialized");

    // 2.5. Initialize the PairingStore (persistent client identity & trust).
    //       Requires the RANDOM sysmodule (loaded by SysmoduleManager) for
    //       identity generation. PairingStore has no network dependency, so it
    //       is initialized here, before the network-dependent managers. It
    //       loads or generates the persistent client identity (unique ID,
    //       RSA-2048 key, self-signed certificate) and prepares the trusted-host
    //       store. Failure to initialize the store is fatal: the client cannot
    //       establish a stable identity without it.
    if (!m_pairingStore->initialize()) {
        diagPrint("[app] PairingStore initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] PairingStore initialized");

    // 2.6. Ensure a valid client identity exists (load or generate).
    //       This generates the persistent unique ID, RSA-2048 private key, and
    //       self-signed client certificate on first run, and loads the exact
    //       same identity on subsequent runs. The unique ID is logged (the
    //       private key material is never logged).
    if (!m_pairingStore->ensureIdentity(m_clientIdentity)) {
        diagPrint("[app] client identity unavailable");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[app] client identity ready: uniqueId='%s' deviceName='%s'",
                 m_clientIdentity.uniqueId.c_str(),
                 m_clientIdentity.deviceName.c_str());
        diagPrint(buf);
    }

    // 2.7. Initialize the persistent SettingsManager.
    //       Loaded early so the first launch/resume uses persisted values.
    //       Missing/invalid settings fall back to safe defaults (not fatal).
    if (m_settingsManager) {
        m_settingsManager->initialize();
    }

    // 3. Initialize GfxManager.
    ps4::GfxResult initResult = m_gfx->initialize();
    if (!initResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] GfxManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(initResult.category), initResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         initResult.nativeError);
    }
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] GfxManager initialized: %ux%u",
                 m_gfx->getFramebufferWidth(), m_gfx->getFramebufferHeight());
        diagPrint(buf);
    }

    // 4. Initialize the FontRasterizer.
    if (!m_fontRasterizer->initialize()) {
        diagPrint("[app] FontRasterizer initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] FontRasterizer initialized");

    // 5. Create and initialize the UiCompositor.
    if (!m_uiCompositor->initialize(kWidth, kHeight, m_fontRasterizer.get())) {
        diagPrint("[app] UiCompositor initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] UiCompositor initialized: %ux%u pitch=%u",
                 m_uiCompositor->width(), m_uiCompositor->height(),
                 m_uiCompositor->pitch());
        diagPrint(buf);
    }

    // 6. Initialize the UiBlitter.
    ps4::GfxResult blitInit = m_uiBlitter->initialize(m_gfx.get());
    if (!blitInit.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] UiBlitter initialize failed: category=%d",
                 static_cast<int>(blitInit.category));
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         blitInit.nativeError);
    }
    diagPrint("[app] UiBlitter initialized");

    // 7. Initialize the InputManager (controller/pad path).
    ps4::InputResult inputInit = m_input->initialize();
    if (!inputInit.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] InputManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(inputInit.category), inputInit.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         inputInit.nativeError);
    }
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] InputManager initialized (crossIsAccept=%d)",
                 m_input->isCrossAccept() ? 1 : 0);
        diagPrint(buf);
    }

    // 8. Initialize the production UiManager.
    ui::UiResult uiInit = m_ui->initialize();
    if (!uiInit.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] UiManager initialize failed: category=%d",
                 static_cast<int>(uiInit.category));
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         uiInit.nativeError);
    }
    diagPrint("[app] UiManager initialized");

    // 9. Initialize the NetworkManager (PS4 Net + NetCtl).
    ps4::NetworkResult netResult = m_network->initialize();
    if (!netResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] NetworkManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(netResult.category), netResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         netResult.nativeError);
    }
    diagPrint("[app] NetworkManager initialized");

    // 10. Initialize the HttpClient (HTTP/HTTPS transport).
    //    Requires the network stack to be initialized. Inability to initialize
    //    HTTP/HTTPS is fatal: the client has no serverinfo capability without it.
    ml::HttpResult httpResult = m_httpClient->initialize();
    if (!httpResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] HttpClient initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(httpResult.category), httpResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         httpResult.nativeError);
    }
    diagPrint("[app] HttpClient initialized");

    // 10.5. Initialize the PairingManager.
    //       Requires the HttpClient to be initialized and the persistent client
    //       identity to be available (from PairingStore). Consumes the
    //       already-created identity and delegates trust persistence to
    //       PairingStore. Initialized before ServerInfoManager so the
    //       serverinfo query path can use the client unique ID and client
    //       certificate for HTTPS queries of trusted hosts.
    ml::PairingResult pairingResult =
        m_pairingManager->initialize(m_httpClient.get(),
                                     m_pairingStore.get(),
                                     m_clientIdentity);
    if (!pairingResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] PairingManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(pairingResult.category), pairingResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         pairingResult.nativeError);
    }
    diagPrint("[app] PairingManager initialized");

    // 11. Initialize the ServerInfoManager.
    //     Requires the HttpClient to be initialized. Performs no host requests
    //     until scheduled by the main loop.
    ml::ServerInfoResult siResult = m_serverInfoManager->initialize(m_httpClient.get());
    if (!siResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] ServerInfoManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(siResult.category), siResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         siResult.nativeError);
    }
    diagPrint("[app] ServerInfoManager initialized");

    // 11.5. Initialize the ApplicationCatalog.
    //       Requires the HttpClient to be initialized. ApplicationCatalog
    //       performs no host requests until scheduled by MoonlightApp.
    ml::ApplicationCatalogResult acResult =
        m_applicationCatalog->initialize(m_httpClient.get());
    if (!acResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] ApplicationCatalog initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(acResult.category), acResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         acResult.nativeError);
    }
    diagPrint("[app] ApplicationCatalog initialized");

    // 11.5b. Initialize the AppArtworkManager.
    //        Requires the HttpClient to be initialized. AppArtworkManager
    //        performs no host requests until scheduled by MoonlightApp.
    if (!m_appArtworkManager->initialize(m_httpClient.get())) {
        diagPrint("[app] AppArtworkManager initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] AppArtworkManager initialized");

    // 11.6. Initialize the ApplicationSessionManager.
    //       Requires the HttpClient to be initialized. ApplicationSessionManager
    //       performs no request at initialization.
    ml::ApplicationLaunchResult almResult =
        m_applicationSessionManager->initialize(m_httpClient.get());
    if (!almResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] ApplicationSessionManager initialize failed: category=%d nativeError=0x%08X",
                 static_cast<int>(almResult.category), almResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         almResult.nativeError);
    }
    diagPrint("[app] ApplicationSessionManager initialized");

    // 11.7. Initialize the StreamSessionManager.
    //       Requires the network stack to be initialized (it calls
    //       LiStartConnection() which initializes ENet internally). Performs
    //       no connection at initialization. Initialized after
    //       ApplicationSessionManager so a successful launch/resume can hand
    //       off a StreamSessionDescriptor.
    ml::StreamSessionResult ssmResult = m_streamSessionManager->initialize();
    if (!ssmResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] StreamSessionManager initialize failed: category=%d nativeError=%d",
                 static_cast<int>(ssmResult.category), ssmResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         static_cast<uint32_t>(ssmResult.nativeError));
    }
    diagPrint("[app] StreamSessionManager initialized");

    // 11.8. Initialize the VideoDecoder.
    //       Owns the PS4 Videodec2 hardware H.264 decode path. Performs no
    //       decoder session at initialization. Initialized after
    //       StreamSessionManager so the stream manager can forward video
    //       callbacks to it.
    ml::VideoDecoderResult vdResult = m_videoDecoder->initialize();
    if (!vdResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] VideoDecoder initialize failed: category=%d nativeError=%d",
                 static_cast<int>(vdResult.category), vdResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         static_cast<uint32_t>(vdResult.nativeError));
    }
    diagPrint("[app] VideoDecoder initialized");

    // 11.8b. Initialize the AudioDecodeOutput.
    //       Requires StreamSessionManager to be initialized (AudioDecodeOutput
    //       consumes encoded audio from StreamSessionManager's queue).
    //       Performs no audio session at initialization.
    ml::AudioDecodeOutputResult adoResult =
        m_audioDecodeOutput->initialize(m_streamSessionManager.get());
    if (!adoResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] AudioDecodeOutput initialize failed: category=%d nativeError=%d",
                 static_cast<int>(adoResult.category), adoResult.nativeError);
        diagPrint(buf);
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure,
                         static_cast<uint32_t>(adoResult.nativeError));
    }
    diagPrint("[app] AudioDecodeOutput initialized");

    // 11.8b2. Initialize the UI navigation sound manager.
    //       Requires the PS4 audio output subsystem to be available (the
    //       AudioDecodeOutput above calls sceAudioOutInit()). Preloads the UI
    //       WAV assets and opens a dedicated AudioOut port for UI cues.
    //       Non-fatal: on any failure the manager logs and continues silently.
    diagPrint("[app] initializing UI sound manager...");
    if (m_uiSound) {
        m_uiSound->initialize();
    }

    // 11.8c. Initialize the ControllerInputStreaming.
    //       Requires InputManager and StreamSessionManager to be initialized.
    //       Consumes the InputManager's raw controller snapshot and forwards
    //       it to the active Moonlight stream. Performs no transmission at
    //       initialization.
    if (!m_controllerInputStreaming->initialize(m_input.get(),
                                                m_streamSessionManager.get())) {
        diagPrint("[app] ControllerInputStreaming initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] ControllerInputStreaming initialized");

    // 11.9. Wire the VideoDecoder into the StreamSessionManager.
    //       The stream manager forwards video callbacks (setup/start/stop/
    //       cleanup/submitDecodeUnit) to the decoder.
    m_streamSessionManager->setVideoDecoder(m_videoDecoder.get());

    // 12. Initialize the HostManager (authoritative host registry).
    diagPrint("[app] HostManager initialized");

    // 12.5. Restore persisted trusted hosts into the HostManager registry.
    //       Requires PairingStore (trusted-host persistence) and HostManager
    //       to be available. Runs before discovery starts so previously paired
    //       hosts appear in the Host List even when offline. Discovery later
    //       reconciles these entries to Online via the existing serviceFqdn
    //       correlation.
    restoreTrustedHosts();

    // 13. Start the DiscoveryManager (mDNS worker thread).
    ml::DiscoveryResult discResult = m_discovery->start();
    if (!discResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] DiscoveryManager start failed: category=%d nativeError=0x%08X",
                 static_cast<int>(discResult.category), discResult.nativeError);
        diagPrint(buf);
        // Non-fatal: the client can still run without active discovery.
    } else {
        diagPrint("[app] DiscoveryManager started");
    }

    // 14. Push the authoritative presentation style to the UI manager and set
    //     the button assignment from the InputManager.
    //     The presentation style is a persistent setting owned by
    //     SettingsManager. UiManager receives it as a read-only preference for
    //     Host/App list render dispatch. It is pushed here (after the settings
    //     manager and UI manager are both initialized) and again whenever the
    //     settings change (closeSettingsOverlay).
    if (m_ui && m_settingsManager) {
        m_ui->setPresentationStyle(
            m_settingsManager->settings().presentationStyle);
        m_ui->setShowQuitAppDialog(
            m_settingsManager->settings().showQuitAppDialog);
    }
    m_ui->setButtonAssignment(m_input->isCrossAccept());

    // 14b. Provide the console's own IPv4 address to the UI manager so the
    //      Add Host overlay can prepopulate the IP field. This makes entering
    //      a host on the same network easier.
    if (m_network && m_network->isInitialized()) {
        m_ui->setLocalIpAddress(m_network->ipAddress());
    }

    // 15. Initialize the production renderers.
    if (!m_bootRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] BootRenderer initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] BootRenderer initialized");

    if (!m_hostListRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] HostListRenderer initialize failed");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::InitializationFailure);
    }
    diagPrint("[app] HostListRenderer initialized");

    // HostGridRenderer is initialized but dormant: the current presentation
    // style is PlayStation, so the grid path is not selected.
    if (!m_hostGridRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] HostGridRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] HostGridRenderer initialized (dormant)");
    }

    // PairingRenderer is initialized and entered through normal host
    // activation (PairingManager drives the Pairing screen).
    if (!m_pairingRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] PairingRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] PairingRenderer initialized");
    }

    // HelpRenderer is initialized and render-capable. It is entered from the
    // Host/App List via Touchpad Click.
    if (!m_helpRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] HelpRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] HelpRenderer initialized");
    }

    // AppListRenderer is initialized and render-capable. It is dormant until
    // the App List screen is entered.
    if (!m_appListRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] AppListRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] AppListRenderer initialized (dormant)");
    }

    // AppGridRenderer is initialized and render-capable. It is dormant until
    // the App List Grid screen is entered.
    if (!m_appGridRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] AppGridRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] AppGridRenderer initialized (dormant)");
    }

    // SettingsRenderer is initialized for the Settings overlay path.
    if (!m_settingsRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] SettingsRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] SettingsRenderer initialized");
    }

    // ModalRenderer is initialized for the Delete Host confirmation and the
    // Different-app-running modal.
    if (!m_modalRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] ModalRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] ModalRenderer initialized");
    }

    // AddHostRenderer is initialized for the Add Host overlay / status modal.
    if (!m_addHostRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] AddHostRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] AddHostRenderer initialized");
    }

    // StreamExitHintRenderer is initialized and render-capable but dormant
    // until a stream is active.
    if (!m_streamExitHintRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] StreamExitHintRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] StreamExitHintRenderer initialized (dormant)");
    }

    // StreamStatisticsRenderer is initialized and render-capable but dormant
    // until a stream is active.
    if (!m_streamStatisticsRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] StreamStatisticsRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] StreamStatisticsRenderer initialized (dormant)");
    }

    // MouseEmulationOverlayRenderer is initialized and render-capable but
    // dormant until a stream is active.
    if (!m_mouseEmulationOverlayRenderer->initialize(m_fontRasterizer.get())) {
        diagPrint("[app] MouseEmulationOverlayRenderer initialize failed (non-fatal)");
    } else {
        diagPrint("[app] MouseEmulationOverlayRenderer initialized (dormant)");
    }

    // 16. Clear the compositor to the fallback background.
    m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);

    // Enter initialised state.
    if (!transitionTo(LifecycleState::Initialized)) {
        logTransition("initialisation failed: state transition error");
        transitionTo(LifecycleState::Stopping);
        transitionTo(LifecycleState::ShutDown);
        return AppResult(AppResultCategory::FatalRuntimeFailure);
    }

    logTransition("initialisation success");
    return AppResult();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

AppResult MoonlightApp::run() {
    if (m_state != LifecycleState::Initialized) {
        logTransition("run() called in invalid state");
        return AppResult(AppResultCategory::InvalidLifecycleOperation);
    }

    if (!transitionTo(LifecycleState::Running)) {
        logTransition("run() failed to enter running state");
        return AppResult(AppResultCategory::FatalRuntimeFailure);
    }

    logTransition("main-loop entry");

    m_running = true;
    m_exitReason = ExitReason::None;

    uint64_t frameCount = 0;

    // --- Single explicit top-level application loop -------------------------
    while (m_running) {
        // Phase order follows the hardware-proven Step-8 main.cpp ordering:
        //
        //   1. pollPlatformEvents        — platform lifecycle events
        //   2. pollInput                 — normalized user input
        //   3. processAsyncResults       — discovery + serverinfo worker results
        //   4. update(deltaTime)         — application & screen state
        //   5. render                    — prepare and submit rendering
        //   6. present                   — present completed frame

        pollPlatformEvents();
        pollInput();
        // Compute current monotonic time (ms) for host freshness tracking.
        processAsyncResults(m_currTimestamp / 1000);
        update(m_deltaTime);
        render();
        present();

        // --- Diagnostics ----------------------------------------------------
        // Periodic frame counter line. Guarded by a compile-time constant
        // (default OFF) to keep the klog quiet during normal operation.
        if (kLogFrameCounter &&
            (frameCount < 4 || frameCount % 300 == 0)) {
            const ui::UiScreen screen = m_ui->currentScreen();
            const size_t hostCount = m_hostManager->hosts().size();
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[app] frame %llu: screen=%d hosts=%zu",
                     static_cast<unsigned long long>(frameCount),
                     static_cast<int>(screen), hostCount);
            diagPrint(buf);
        }
        ++frameCount;

        // Keep the process alive; the loop runs indefinitely.
        sceKernelUsleep(1000);
    }
    // ------------------------------------------------------------------------

    // Loop has ended — transition to stopping.
    logTransition("main-loop exit");

    if (!transitionTo(LifecycleState::Stopping)) {
        logTransition("run() failed to enter stopping state");
        return AppResult(AppResultCategory::FatalRuntimeFailure);
    }

    // Map exit reason to result.
    switch (m_exitReason) {
    case ExitReason::None:
    case ExitReason::UserRequest:
    case ExitReason::PlatformCloseRequest:
    case ExitReason::InternalTestExit:
        return AppResult(AppResultCategory::NormalExit);
    case ExitReason::InitializationFailure:
        return AppResult(AppResultCategory::InitializationFailure);
    case ExitReason::RuntimeFailure:
        return AppResult(AppResultCategory::FatalRuntimeFailure);
    }
    return AppResult(AppResultCategory::NormalExit); // unreachable
}

// ---------------------------------------------------------------------------
// requestExit
// ---------------------------------------------------------------------------

void MoonlightApp::requestExit(ExitReason reason) {
    // Preserve the first meaningful exit reason.
    // Do not overwrite a fatal reason with a milder one.
    if (m_exitReason == ExitReason::None ||
        (m_exitReason != ExitReason::RuntimeFailure &&
         m_exitReason != ExitReason::InitializationFailure)) {
        m_exitReason = reason;
    }

    m_running = false;

    char buf[128];
    snprintf(buf, sizeof(buf), "[app] exit requested (reason %d)",
             static_cast<int>(reason));
    diagPrint(buf);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MoonlightApp::shutdown() {
    // Idempotent: already shut down.
    if (m_state == LifecycleState::ShutDown) {
        return;
    }

    logTransition("shutdown start");

    // If still running, stop the loop. This can happen if shutdown() is
    // called externally while the loop is executing (programmer error in a
    // future module, but handled safely).
    m_running = false;

    // Transition from any running/stopping state to ShutDown.
    if (m_state == LifecycleState::Running) {
        transitionTo(LifecycleState::Stopping);
    }

    // Release subsystems in reverse dependency order.
    // VideoDecoder is shut down FIRST (before StreamSessionManager): no active
    // stream may survive the decoder teardown, and the stream manager forwards
    // video callbacks to the decoder.
    if (m_videoDecoder) {
        m_videoDecoder->shutdown();
    }
    m_streamActive = false;
    m_heldDecodedFrame.release();
    m_retiredFrames.clear();
    for (auto& ref : m_streamSlotRefs) {
        ref.valid = false;
    }

    // AudioDecodeOutput is shut down before StreamSessionManager (it must
    // stop consuming encoded audio before the stream connection is torn
    // down).
    if (m_audioDecodeOutput) {
        m_audioDecodeOutput->shutdown();
    }

    // UiSoundManager is shut down before the audio subsystem teardown. It
    // owns its own AudioOut port and retained PCM buffers; releasing them
    // here avoids leaking audio handles/buffers.
    if (m_uiSound) {
        m_uiSound->shutdown();
    }

    // ControllerInputStreaming is shut down before StreamSessionManager (it
    // must stop forwarding controller input before the stream connection is
    // torn down).
    if (m_controllerInputStreaming) {
        m_controllerInputStreaming->shutdown();
    }

    // StreamSessionManager is shut down next (before ApplicationSessionManager
    // and HttpClient): no active stream may survive the network/launch-manager
    // teardown. Its worker is joined and the connection stopped before any
    // network dependency is torn down.
    if (m_streamSessionManager) {
        m_streamSessionManager->shutdown();
    }

    // ApplicationSessionManager is shut down before ApplicationCatalog (no
    // active launch/quit request may survive HttpClient shutdown).
    if (m_applicationSessionManager) {
        m_applicationSessionManager->shutdown();
    }
    m_launchHostId = 0;
    m_launchAppId = 0;
    m_launchGeneration = 0;
    m_launchActive = false;
    m_pendingSessionDescriptor.reset();
    clearConnectingState();
    m_quitHostId = 0;
    m_quitAppId = 0;
    m_quitGeneration = 0;
    m_quitActive = false;
    m_pendingLaunchAfterQuitHostId = 0;
    m_pendingLaunchAfterQuitAppId = 0;
    m_allowLaunchAfterQuit = false;
    m_waitingForQuitToComplete = false;

    // ApplicationCatalog is shut down before PairingManager (no active App
    // List request may survive HttpClient shutdown).
    if (m_applicationCatalog) {
        m_applicationCatalog->shutdown();
    }
    m_catalogHostId = 0;
    m_catalogGeneration = 0;
    m_catalogApplications.clear();
    m_catalogState = ml::ApplicationCatalogState::Idle;
    m_appListPollHostId = 0;
    m_appListNextPollUs = 0;

    // PairingManager is shut down before ServerInfoManager (no active pairing
    // request may survive HttpClient shutdown). Its worker must stop before
    // the transport dies.
    if (m_pairingManager) {
        m_pairingManager->shutdown();
    }
    m_pendingPairingAutoOpenHostId = 0;

    // Clear any pending manual host add state.
    m_pendingManualAddHostId = 0;
    m_pendingManualAddWasExisting = false;

    // ServerInfoManager is shut down before HttpClient (no active serverinfo
    // request may survive HttpClient shutdown).
    if (m_serverInfoManager) {
        m_serverInfoManager->shutdown();
    }
    m_serverInfoGenerations.clear();

    // AppArtworkManager is shut down before HttpClient (no active artwork
    // request may survive HttpClient shutdown).
    if (m_appArtworkManager) {
        m_appArtworkManager->shutdown();
    }
    // Clear runtime artwork and scheduling state.
    m_runtimeArtwork.clear();
    m_artworkSchedulingActive = false;
    m_artworkNextScheduleIndex = 0;
    m_artworkGeneration = 0;
    m_artworkServerId.clear();

    // HttpClient is shut down before NetworkManager (no HTTP/SSL handles may
    // survive Net termination).
    if (m_httpClient) {
        m_httpClient->shutdown();
    }

    // Discovery is stopped next (no more host observations).
    if (m_discovery) {
        m_discovery->shutdown();
    }

    // Network is shut down next (no network consumers remain active).
    if (m_network) {
        m_network->shutdown();
    }

    // PairingStore is shut down before SysmoduleManager (it consumes the
    // RANDOM sysmodule). It has no network dependency and holds no live file
    // handles after shutdown; it is idempotent.
    if (m_pairingStore) {
        m_pairingStore->shutdown();
    }
    m_clientIdentity = ml::ClientIdentity();

    // SettingsManager is independent (file I/O only). Settings are already
    // saved when the Settings overlay closes, so no save is needed here.
    if (m_settingsManager) {
        m_settingsManager->shutdown();
    }

    // Then UI/graphics teardown.
    if (m_streamStatisticsRenderer) {
        m_streamStatisticsRenderer->shutdown();
    }
    if (m_mouseEmulationOverlayRenderer) {
        m_mouseEmulationOverlayRenderer->shutdown();
    }
    if (m_streamExitHintRenderer) {
        m_streamExitHintRenderer->shutdown();
    }
    if (m_addHostRenderer) {
        m_addHostRenderer->shutdown();
    }
    if (m_modalRenderer) {
        m_modalRenderer->shutdown();
    }
    if (m_settingsRenderer) {
        m_settingsRenderer->shutdown();
    }
    if (m_appGridRenderer) {
        m_appGridRenderer->shutdown();
    }
    if (m_appListRenderer) {
        m_appListRenderer->shutdown();
    }
    if (m_pairingRenderer) {
        m_pairingRenderer->shutdown();
    }
    if (m_helpRenderer) {
        m_helpRenderer->shutdown();
    }
    if (m_hostGridRenderer) {
        m_hostGridRenderer->shutdown();
    }
    if (m_hostListRenderer) {
        m_hostListRenderer->shutdown();
    }
    if (m_bootRenderer) {
        m_bootRenderer->shutdown();
    }

    // ImeDialog is shut down before UI teardown so no system dialog is left
    // active while the application tears down its platform managers.
    if (m_imeDialog) {
        m_imeDialog->shutdown();
    }

    if (m_ui) {
        m_ui->shutdown();
    }
    if (m_input) {
        m_input->shutdown();
    }
    if (m_uiBlitter) {
        m_uiBlitter->shutdown();
    }
    if (m_uiCompositor) {
        m_uiCompositor->shutdown();
    }
    if (m_fontRasterizer) {
        m_fontRasterizer->shutdown();
    }
    if (m_gfx) {
        m_gfx->shutdown();
    }

    // SysmoduleManager is shut down LAST, after every subsystem that consumes
    // a loaded sysmodule has been torn down. It is the sole owner of
    // sceSysmoduleUnloadModule*() and must be the final PS4 platform teardown
    // step so no loaded module is unloaded while still in use.
    if (m_sysmoduleManager) {
        m_sysmoduleManager->shutdown();
    }

    if (!transitionTo(LifecycleState::ShutDown)) {
        m_state = LifecycleState::ShutDown;
    }

    logTransition("shutdown complete");
}

// ---------------------------------------------------------------------------
// Main-loop phase methods
// ---------------------------------------------------------------------------

// Phase 1: Poll platform and lifecycle events.
// Current behaviour: no-op (exit handling is done via requestExit).
void MoonlightApp::pollPlatformEvents() {
    // TODO: PlatformLifecycle integration — poll system events,
    // detect close/suspend/resume requests, convert to lifecycle actions.
}

// Phase 2: Poll and normalise user input.
// Polls the PS4 controller via InputManager, forwards the latest controller
// state to the active stream, and detects the local stream-exit chord
// (Options + L3). Ordinary UI navigation is handled separately in
// update() and is bypassed while a stream is active.
void MoonlightApp::pollInput() {
    if (!m_input || !m_input->isInitialized()) {
        return;
    }

    // Poll the controller once per frame.
    m_input->poll();

    // Forward the latest controller state to the active stream (if any).
    // ControllerInputStreaming is gated to the active session generation and
    // only sends once the input channel is ready.
    if (m_controllerInputStreaming) {
        m_controllerInputStreaming->update();

        // Detect a mouse emulation toggle (Options + TouchPad Click) on the
        // rising/falling edge of the state and drive the transient in-stream
        // mouse emulation state overlay. The overlay is shown for the same
        // duration as the stream exit hint and coexists with the statistics
        // overlay.
        const bool mouseEmulation =
            m_controllerInputStreaming->isMouseEmulationEnabled();
        if (mouseEmulation != m_prevMouseEmulationEnabled) {
            m_prevMouseEmulationEnabled = mouseEmulation;
            if (m_ui && m_ui->isInitialized()) {
                m_ui->setMouseEmulationState(mouseEmulation);
            }
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "[input] mouse emulation overlay: %s",
                     mouseEmulation ? "ON" : "OFF");
            diagPrint(buf);
        }
    }

    // Stream-exit chord: Options + L3 while a stream is active.
    // This is the dedicated stream-exit combination. It stops the stream and
    // returns to the App List. The chord buttons are NOT forwarded to the
    // host (ControllerInputStreaming suppresses them via the exit-chord
    // precedence in its button mapping).
    if (m_streamActive && m_controllerInputStreaming &&
        m_controllerInputStreaming->isActive()) {
        ps4::ControllerState state;
        if (m_input->latestControllerState(state)) {
            // ORBIS_PAD_BUTTON_OPTIONS = 0x00000008, ORBIS_PAD_BUTTON_L3 = 0x00000002.
            const uint32_t kOptions = 0x00000008u;
            const uint32_t kL3 = 0x00000002u;
            if ((state.buttons & kOptions) && (state.buttons & kL3)) {
                diagPrint("[input] exit chord consumed locally (Options+L3)");
                // Stop controller forwarding first (sends neutral if usable),
                // then stop the stream connection.
                m_controllerInputStreaming->stop();
                if (m_streamSessionManager) {
                    m_streamSessionManager->stop();
                }

                // If quit-on-exit is enabled, request a remote application
                // quit via the existing ApplicationSessionManager quit path.
                // Local stream teardown does not depend on the remote quit
                // succeeding; if it fails, the stream still exits locally and
                // the existing application-session error handling reports it.
                if (m_settingsManager &&
                    m_settingsManager->settings().quitAppOnStreamExit) {
                    diagPrint("[app] quit-on-exit enabled — requesting remote app quit");
                    handleQuitApplication(m_streamHostId, m_streamAppId);
                }
            }
        }
    }
}

// Phase 3: Consume completed worker results.
// Polls the DiscoveryManager for host discovery updates and applies them to
// the authoritative HostManager registry. When a host is Added or returns
// Online, MoonlightApp schedules a serverinfo query. ServerInfoManager results
// are applied back into HostManager on the main thread.
void MoonlightApp::processAsyncResults(uint64_t nowMs) {
    // --- 1. Consume discovery updates and schedule serverinfo queries ------
    if (m_discovery && m_discovery->isRunning() && m_hostManager) {
        ml::HostDiscoveryUpdate update;
        while (m_discovery->pollUpdate(update)) {
            ml::HostDiscoveryResult result =
                m_hostManager->applyDiscoveryUpdate(update, nowMs);

            // Schedule a serverinfo query for new hosts and hosts that
            // returned Online. Do NOT schedule on ordinary refreshes.
            if (result.change == ml::HostChange::Added ||
                result.change == ml::HostChange::BecameOnline) {
                scheduleServerInfoQuery(result.hostId);
            }

            // Whenever mDNS observes a host, refresh its persisted last-known
            // endpoint (if it is a paired host) so a future restart can use
            // the HTTPS fallback even when mDNS is unavailable. This only
            // writes when a relevant stored value actually changes.
            if (result.hostId != 0) {
                persistHostEndpoint(result.hostId);
            }

            // --- [host-poll] Offline -> Online (source=mdns) ----------------
            // A host that returned Online via an mDNS observation is logged
            // here with the relevant ages for the temporary diagnostics.
            if (result.change == ml::HostChange::BecameOnline) {
                const ml::Host* h = m_hostManager->findHost(result.hostId);
                if (h != nullptr) {
                    const uint64_t lastSeenAgeMs =
                        (h->lastSeenMs != 0) ? (nowMs - h->lastSeenMs) : 0;
                    const uint64_t reachAgeMs =
                        (h->lastReachabilityConfirmedMs != 0)
                            ? (nowMs - h->lastReachabilityConfirmedMs)
                            : 0;
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "[host-poll] id=%llu Offline -> Online source=mdns lastSeenAgeMs=%llu lastReachabilityAgeMs=%llu",
                             static_cast<unsigned long long>(result.hostId),
                             static_cast<unsigned long long>(lastSeenAgeMs),
                             static_cast<unsigned long long>(reachAgeMs));
                    diagPrint(buf);
                }
            }
            if (result.change == ml::HostChange::Added) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[app] host added: id=%llu",
                         static_cast<unsigned long long>(result.hostId));
                diagPrint(buf);
            } else if (result.change == ml::HostChange::BecameOnline) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[app] host returned online: id=%llu",
                         static_cast<unsigned long long>(result.hostId));
                diagPrint(buf);
            }
        }
    }

    // --- 2. Consume completed serverinfo results ---------------------------
    if (m_serverInfoManager && m_hostManager) {
        ml::ServerInfoUpdate siUpdate;
        while (m_serverInfoManager->pollUpdate(siUpdate)) {
            // --- Stale-result filtering ------------------------------------
            // If a newer result has already been applied for this HostId,
            // ignore this older result.
            uint32_t lastAppliedGeneration = 0;
            for (const auto& gen : m_serverInfoGenerations) {
                if (gen.first == siUpdate.hostId) {
                    lastAppliedGeneration = gen.second;
                    break;
                }
            }
            if (lastAppliedGeneration != 0 && siUpdate.generation < lastAppliedGeneration) {
                continue;   // Stale result — ignore.
            }

            // Record the applied generation for this HostId.
            bool found = false;
            for (auto& gen : m_serverInfoGenerations) {
                if (gen.first == siUpdate.hostId) {
                    gen.second = siUpdate.generation;
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_serverInfoGenerations.push_back(
                    {siUpdate.hostId, siUpdate.generation});
            }

            if (siUpdate.valid) {
                m_hostManager->applyServerInfo(
                    siUpdate.hostId,
                    siUpdate.info.serverId,
                    siUpdate.info.serverName,
                    siUpdate.info.serverVersion,
                    siUpdate.info.httpsPort,
                    siUpdate.info.serverReportsPaired,
                    siUpdate.info.currentGame,
                    siUpdate.info.serverCodecModeSupport);

                // A successful serverinfo request is valid evidence the host
                // is reachable. This is the fallback reachability confirmation
                // that lets a paired host stay Online even when mDNS is
                // unavailable. It is distinct from an mDNS observation.
                const ml::Host* hostBefore = m_hostManager->findHost(siUpdate.hostId);
                const bool wasOnlineBefore =
                    hostBefore != nullptr &&
                    hostBefore->availability == ml::HostAvailability::Online;
                m_hostManager->confirmReachable(siUpdate.hostId, nowMs);

                // Persist the host's last-known endpoint so a future restart
                // can use the HTTPS fallback even when mDNS is unavailable.
                persistHostEndpoint(siUpdate.hostId);

                // --- [host-poll] probe completion diagnostics ---------------
                // If this result belongs to a fallback probe, log its outcome.
                for (auto it = m_probeInFlightHosts.begin();
                     it != m_probeInFlightHosts.end(); ++it) {
                    if (*it == siUpdate.hostId) {
                        char buf[192];
                        snprintf(buf, sizeof(buf),
                                 "[host-poll] HTTPS probe succeeded host=%llu",
                                 static_cast<unsigned long long>(siUpdate.hostId));
                        diagPrint(buf);
                        m_probeInFlightHosts.erase(it);
                        break;
                    }
                }

                // --- [host-poll] Offline -> Online (source=https) -----------
                // A host that was not Online before this successful HTTPS
                // probe is now Online via the fallback reachability path.
                if (!wasOnlineBefore) {
                    const ml::Host* h = m_hostManager->findHost(siUpdate.hostId);
                    if (h != nullptr) {
                        const uint64_t lastSeenAgeMs =
                            (h->lastSeenMs != 0) ? (nowMs - h->lastSeenMs) : 0;
                        const uint64_t reachAgeMs =
                            (h->lastReachabilityConfirmedMs != 0)
                                ? (nowMs - h->lastReachabilityConfirmedMs)
                                : 0;
                        char buf[192];
                        snprintf(buf, sizeof(buf),
                                 "[host-poll] id=%llu Offline -> Online source=https lastSeenAgeMs=%llu lastReachabilityAgeMs=%llu",
                                 static_cast<unsigned long long>(siUpdate.hostId),
                                 static_cast<unsigned long long>(lastSeenAgeMs),
                                 static_cast<unsigned long long>(reachAgeMs));
                        diagPrint(buf);
                    }
                }

                // If this host has local trust (is paired) but the serverinfo
                // query was sent over plain HTTP (because the serverId was not
                // yet known at scheduling time), re-query over HTTPS with the
                // client certificate so the server reports the correct
                // PairStatus. Sunshine only reports paired=1 when the client
                // authenticates via HTTPS with its certificate.
                const ml::Host* host = m_hostManager->findHost(siUpdate.hostId);
                if (host != nullptr &&
                    m_pairingManager &&
                    m_pairingManager->hasLocalTrustForServer(host->serverId) &&
                    !siUpdate.info.serverReportsPaired) {
                    diagPrint("[app] host has local trust but serverinfo reported unpaired; re-querying over HTTPS");
                    scheduleServerInfoQuery(siUpdate.hostId);
                } else {
                    // Reconcile pairing state using local trust + server evidence.
                    reconcileHostPairingState(siUpdate.hostId);
                }

                // If this serverinfo refresh is for the current App List host,
                // rebuild the App List view so the Running indicator reflects
                // the authoritative currentGame (e.g., after a remote quit).
                if (siUpdate.hostId == m_catalogHostId) {
                    refreshAppListView();
                    // If a quit-and-launch is waiting for the host to free up,
                    // check whether currentGame has reached 0 and launch the
                    // pending app now.
                    maybeLaunchPendingAfterQuit();
                }

                // --- Manual host add status update --------------------------
                // If this is the pending manual host add and its serverinfo
                // succeeded, report the result in the status modal. The pending
                // marker is intentionally NOT cleared here: the host must stay
                // hidden from the UI snapshot until the status modal is
                // dismissed, so the newly-added card is never drawn underneath
                // the still-open modal (which previously triggered a GPU page
                // fault). The marker is cleared in update() once the modal
                // closes.
                if (m_pendingManualAddHostId == siUpdate.hostId) {
                    if (m_ui && m_ui->isInitialized()) {
                        // 1 = FoundNew, 2 = FoundExisting.
                        m_ui->setAddHostStatus(
                            m_pendingManualAddWasExisting ? 2 : 1);
                    }
                }

                // Per-request serverinfo "applied" line. Guarded by a
                // compile-time constant (default OFF) to keep the klog quiet.
                if (kLogServerInfoApplied) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "[app] serverinfo applied host id=%llu serverId='%s' name='%s' version='%s' paired=%d httpsPort=%u",
                             static_cast<unsigned long long>(siUpdate.hostId),
                             siUpdate.info.serverId.c_str(),
                             siUpdate.info.serverName.c_str(),
                             siUpdate.info.serverVersion.c_str(),
                             siUpdate.info.serverReportsPaired ? 1 : 0,
                             static_cast<unsigned>(siUpdate.info.httpsPort));
                    diagPrint(buf);
                }
            } else {
                m_hostManager->setServerInfoState(
                    siUpdate.hostId, ml::ServerInfoQueryState::Failed);
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[app] serverinfo failed host id=%llu (category %d)",
                         static_cast<unsigned long long>(siUpdate.hostId),
                         static_cast<int>(siUpdate.result.category));
                diagPrint(buf);

                // --- [host-poll] probe failure diagnostics ------------------
                // If this failed result belongs to a fallback probe, log it.
                for (auto it = m_probeInFlightHosts.begin();
                     it != m_probeInFlightHosts.end(); ++it) {
                    if (*it == siUpdate.hostId) {
                        char buf2[192];
                        snprintf(buf2, sizeof(buf2),
                                 "[host-poll] HTTPS probe failed host=%llu category=%d",
                                 static_cast<unsigned long long>(siUpdate.hostId),
                                 static_cast<int>(siUpdate.result.category));
                        diagPrint(buf2);
                        m_probeInFlightHosts.erase(it);
                        break;
                    }
                }

                // If this host was pending auto-open after pairing but the
                // post-pairing serverinfo query failed, reconciliation cannot
                // confirm it is Paired. Abandon the auto-open and return to
                // the Host List (preserving the existing pairing failure/error
                // behavior).
                if (m_pendingPairingAutoOpenHostId == siUpdate.hostId) {
                    m_pendingPairingAutoOpenHostId = 0;
                    diagPrint("[app] post-pairing serverinfo failed — returning to Host List");
                    if (m_ui && m_ui->isInitialized()) {
                        ui::ApplicationListView view;
                        view.state = ml::ApplicationCatalogState::Idle;
                        m_ui->setApplicationListView(view);
                    }
                }

                // --- Manual-host bootstrap failure cleanup ------------------
                // If this is a manually-added host whose initial serverinfo
                // bootstrap never succeeded, remove the temporary placeholder
                // so no misleading Online/unpaired host remains. A manual
                // placeholder is identified by: not observed on the network,
                // no serverId, and no serviceFqdn (it was created purely from
                // a numeric IP + port).
                if (m_hostManager) {
                    const ml::Host* host =
                        m_hostManager->findHost(siUpdate.hostId);
                    if (host != nullptr &&
                        !host->observedOnNetwork &&
                        host->serverId.empty() &&
                        host->serviceFqdn.empty()) {
                        diagPrint("[host] manual add failed — removing temporary placeholder");
                        m_hostManager->removeHost(siUpdate.hostId);
                        // Clear any stale serverinfo generation tracking.
                        for (auto it = m_serverInfoGenerations.begin();
                             it != m_serverInfoGenerations.end();) {
                            if (it->first == siUpdate.hostId) {
                                it = m_serverInfoGenerations.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                }

                // --- Manual host add status update (not found) --------------
                // If this is the pending manual host add and its serverinfo
                // failed, report "not found" in the status modal.
                if (m_pendingManualAddHostId == siUpdate.hostId) {
                    if (m_ui && m_ui->isInitialized()) {
                        m_ui->setAddHostStatus(3);   // NotFound
                    }
                    m_pendingManualAddHostId = 0;
                    m_pendingManualAddWasExisting = false;
                }
            }
        }
    }

    // --- 3. Consume completed pairing updates ------------------------------
    if (m_pairingManager && m_hostManager) {
        ml::PairingUpdate pairingUpdate;
        while (m_pairingManager->pollUpdate(pairingUpdate)) {
            handlePairingUpdate(pairingUpdate);
        }
    }

    // --- 4. Consume completed ApplicationCatalog updates -------------------
    if (m_applicationCatalog) {
        ml::ApplicationCatalogUpdate acUpdate;
        while (m_applicationCatalog->pollUpdate(acUpdate)) {
            handleApplicationCatalogUpdate(acUpdate);
        }
    }

    // --- 4b. Consume completed AppArtworkManager results -------------------
    // Production integration: process each completed artwork result on the
    // main thread. Stale results (wrong host/generation) are rejected; valid
    // results move the decoded image into runtime artwork state and mark the
    // affected card dirty.
    if (m_appArtworkManager) {
        ml::AppArtworkResult awResult;
        while (m_appArtworkManager->pollResult(awResult)) {
            if (handleArtworkResult(awResult)) {
                // Record the app whose artwork changed so the renderer can
                // invalidate only its card region.
                bool alreadyTracked = false;
                for (const auto& id : m_artworkDirtyApps) {
                    if (id == awResult.appId) {
                        alreadyTracked = true;
                        break;
                    }
                }
                if (!alreadyTracked) {
                    m_artworkDirtyApps.push_back(awResult.appId);
                }
            }
        }
    }

    // --- 5. Consume completed ApplicationSessionManager updates ------------
    if (m_applicationSessionManager) {
        ml::ApplicationLaunchUpdate almUpdate;
        while (m_applicationSessionManager->pollUpdate(almUpdate)) {
            handleApplicationLaunchUpdate(almUpdate);
        }

        // Consume completed remote-quit updates.
        ml::ApplicationQuitUpdate quitUpdate;
        while (m_applicationSessionManager->pollQuitUpdate(quitUpdate)) {
            handleApplicationQuitUpdate(quitUpdate);
        }
    }

    // --- 6. Consume completed StreamSessionManager updates -----------------
    if (m_streamSessionManager) {
        ml::StreamSessionUpdate ssmUpdate;
        while (m_streamSessionManager->pollUpdate(ssmUpdate)) {
            handleStreamSessionUpdate(ssmUpdate);
        }
    }

    // --- 7. Consume completed AudioDecodeOutput updates --------------------
    if (m_audioDecodeOutput) {
        ml::AudioDecodeOutputUpdate adoUpdate;
        while (m_audioDecodeOutput->pollUpdate(adoUpdate)) {
            handleAudioDecodeOutputUpdate(adoUpdate);
        }
    }
}

// Phase 4: Update application and screen state.
void MoonlightApp::update(float deltaTime) {
    // 1. Refresh timing for the next frame.
    updateTiming();

    // 2. The delta time is now available via m_deltaTime for future
    //    application state updates. The parameter deltaTime contains the
    //    *previous* frame's delta (before the timing refresh above).
    (void)deltaTime;

    // 2b. Update the paired-host HTTPS reachability fallback polling cadence.
    //     This is a low-frequency fallback for paired hosts not observed via
    //     mDNS. It is centralized here so future suspend handling can disable
    //     it cleanly.
    updatePairedHostProbe();

    // 2c. Update the App List serverinfo polling cadence. While the App List
    //     screen is active, this requests an authenticated serverinfo refresh
    //     immediately on entry and then approximately once per second so
    //     externally-triggered launch/quit/switch changes are reflected.
    updateAppListPolling();

    // 2c2. Continue artwork scheduling for the current catalog. While
    //      scheduling is active, schedule a bounded batch of artwork requests
    //      each frame (stopping on QueueFull and resuming later). This keeps
    //      the request queue from overflowing without an unbounded pending
    //      list and without per-frame duplicate spam.
    scheduleArtworkBatch();

    // 3. Update host availability based on current monotonic time.
    if (m_hostManager) {
        m_hostManager->updateAvailability(m_currTimestamp / 1000);
    }

    // 3a. Emit concise [host-poll] transition diagnostics (Online->Stale,
    //     Stale->Offline) by comparing against the previously tracked state.
    logHostAvailabilityTransitions(m_currTimestamp / 1000);

    // 4. Supply the current read-only host snapshot to the UI.
    //    Only push to UiManager when the authoritative registry actually
    //    changed (host added/removed/updated or availability transitioned).
    updateUiHostSnapshot();

    // 5. Build normalized UI input and pass it to the UiManager ONLY when not
    //    streaming. While a stream is active, controller input belongs to the
    //    remote host (ControllerInputStreaming), so normal UiManager controller
    //    processing is bypassed. The only local interception during streaming
    //    is the stream-exit chord (Options + L3), handled in pollInput().
    if (m_ui && m_ui->isInitialized() && !m_streamActive) {
        // Keep the UiManager's button-assignment glyphs in sync with the
        // system's Enter Button Assignment.
        if (m_input && m_input->isInitialized()) {
            m_ui->setButtonAssignment(m_input->isCrossAccept());
        }

        // Supply the current grid column counts to the UiManager for grid
        // navigation. The column counts are owned by the grid renderers (which
        // own the grid geometry); UiManager does not derive grid geometry
        // itself. This is updated each frame so navigation always uses the
        // authoritative renderer-derived column count.
        if (m_hostGridRenderer && m_appGridRenderer) {
            m_ui->setGridColumns(m_hostGridRenderer->columns(),
                                 m_appGridRenderer->columns());
        }

        ui::UiInput uiInput;
        if (m_input && m_input->isInitialized()) {
            uiInput.upPressed = m_input->isUpJustPressed();
            uiInput.downPressed = m_input->isDownJustPressed();
            uiInput.leftPressed = m_input->isLeftJustPressed();
            uiInput.rightPressed = m_input->isRightJustPressed();
            uiInput.acceptPressed = m_input->isAcceptJustPressed();
            uiInput.backPressed = m_input->isBackJustPressed();
            uiInput.trianglePressed = m_input->isTriangleJustPressed();
            uiInput.optionsPressed = m_input->isOptionsJustPressed();
            uiInput.squarePressed = m_input->isSquareJustPressed();
            uiInput.touchpadPressed = m_input->isTouchpadJustPressed();
        }
        m_ui->processInput(uiInput);
    }

    // 5a2. Add Host ImeDialog handling.
    //       If the Add Host overlay requested an ImeDialog (user pressed Cross
    //       on an editable field), open the PS4 system keyboard. Then poll the
    //       active ImeDialog lifecycle each frame and route the returned text
    //       back to the overlay.
    if (m_ui && m_ui->isInitialized() && !m_streamActive) {
        if (m_ui->isAddHostOpen() && m_ui->isAddHostImeActive() &&
            m_imeDialog && !m_imeDialog->isActive()) {
            diagPrint("[app] Add Host ImeDialog request detected");
            openAddHostIme();
        }
        updateAddHostIme();
    }

    // 6. Update UI presentation state (Boot timer, transitions).
    if (m_ui && m_ui->isInitialized()) {
        m_ui->update(m_deltaTime);
    }

    // 6b. Update the in-stream statistics display snapshot once per second
    //     while a stream is active and statistics are enabled. Reads the
    //     subsystem counters and pushes the computed snapshot to UiManager.
    updateStreamStatistics();

    // 7. Consume UI intents emitted by UiManager ONLY when not streaming.
    //    While streaming, UiManager intents are not polled or executed, so no
    //    latent/stale intent can trigger Back, ActivateHost, ActivateApplication,
    //    Settings, Add Host, Delete Host, or Quit App during the stream.
    if (m_ui && m_ui->isInitialized() && !m_streamActive) {
        ui::UiIntent intent;
        while (m_ui->pollIntent(intent)) {
            handleUiIntent(intent);
        }
    }

    // 7b. Consume the UI navigation sound intent and forward it to the UI
    //     sound manager, gated centrally by stream state.
    //     UI sounds must never play while a stream is active (or during the
    //     stream teardown transition). This is the single high-level
    //     enable/suppress condition; it does not rely on individual UI call
    //     sites being absent during stream mode. The UiManager produces the
    //     intent only on successful UI state/navigation events, so no sound is
    //     played for ignored/rejected input.
    if (!m_streamActive) {
        if (m_ui && m_ui->isInitialized() && m_uiSound &&
            m_uiSound->isInitialized()) {
            const ui::UiSoundIntent soundIntent = m_ui->pollSoundIntent();
            if (soundIntent != ui::UiSoundIntent::None) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[app] UI sound intent=%d",
                         static_cast<int>(soundIntent));
                diagPrint(buf);
            }
            switch (soundIntent) {
            case ui::UiSoundIntent::Cursor:
                m_uiSound->play(ps4::UiSound::Cursor);
                break;
            case ui::UiSoundIntent::Accept:
                m_uiSound->play(ps4::UiSound::Accept);
                break;
            case ui::UiSoundIntent::Back:
                m_uiSound->play(ps4::UiSound::Back);
                break;
            case ui::UiSoundIntent::SettingsOpen:
                m_uiSound->play(ps4::UiSound::SettingsOpen);
                break;
            case ui::UiSoundIntent::SettingsClose:
                m_uiSound->play(ps4::UiSound::SettingsClose);
                break;
            case ui::UiSoundIntent::None:
            default:
                break;
            }
        }
    }
}

// Phase 5: Prepare and submit rendering.
void MoonlightApp::render() {
    if (!m_gfx || !m_gfx->isInitialized()) {
        return;
    }

    // Acquire and prepare the next writable frame.
    ps4::GfxResult beginResult = m_gfx->beginFrame();
    if (!beginResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] beginFrame failed: category=%d",
                 static_cast<int>(beginResult.category));
        diagPrint(buf);
        requestExit(ExitReason::RuntimeFailure);
        return;
    }

    // --- Stream presentation path -------------------------------------------
    // While a stream is active, the main thread presents decoded video frames
    // directly into the GfxManager framebuffer, bypassing the CPU UI
    // compositor. The CPU UI compositor is suppressed during streaming.
    if (m_streamActive) {
        bool presented = presentStreamVideoFrame();
        if (!presented) {
            // No decoded frame available yet — clear to black. The in-stream
            // overlay (exit hint / statistics) is withheld until the first
            // real video frame is presented so it never renders over a black
            // (video-less) framebuffer, which would otherwise flicker.
            m_gfx->clear(0xFF000000u);
        } else {
            // A real video frame is in the framebuffer. Compose the in-stream
            // overlay (exit hint / statistics) over it. This preserves the
            // video and never mutates the retained decode image.
            composeStreamOverlay();
        }
        return;
    }

    // --- CPU compositor/blitter is the only active UI path -----------------
    // Every screen is rendered through the CPU renderers, dispatched by the
    // current UiScreen.
    const ui::UiScreen screen =
        (m_ui && m_ui->isInitialized()) ? m_ui->currentScreen()
                                        : ui::UiScreen::Boot;
    const ml::PresentationStyle style =
        (m_ui && m_ui->isInitialized()) ? m_ui->presentationStyle()
                                        : ml::PresentationStyle::PlayStation;

    // --- Presentation-style active flags ------------------------------------
    // Track which renderer is active for the current screen + presentation
    // style. When a renderer is NOT active (e.g., switching from the Host List
    // crossbar to the Host List Grid), reset its "seeded/valid" flag so a later
    // re-entry performs a full recompose. This guarantees the CPU surface is
    // fully synchronized before the next CPU-active display, and prevents a
    // stale surface (e.g., Host Grid content) from being shown when switching
    // back to the crossbar.
    const bool hostListActive =
        (screen == ui::UiScreen::HostList &&
         style == ml::PresentationStyle::PlayStation);
    const bool hostGridActive =
        (screen == ui::UiScreen::HostList &&
         style == ml::PresentationStyle::Grid);
    const bool appListActive =
        (screen == ui::UiScreen::AppList &&
         style == ml::PresentationStyle::PlayStation);
    const bool appGridActive =
        (screen == ui::UiScreen::AppList &&
         style == ml::PresentationStyle::Grid);

    // Reset the inactive renderers' valid flags so a later re-entry performs a
    // full recompose + reseed. This matches the SCE reference's per-renderer
    // seeded-flag reset when leaving a path.
    if (!hostListActive) {
        m_prevHostListValid = false;
    }
    if (!hostGridActive) {
        m_prevHostGridValid = false;
    }
    if (!appListActive) {
        m_prevAppListValid = false;
    }
    if (!appGridActive) {
        m_prevAppGridValid = false;
    }

    if (screen == ui::UiScreen::Boot) {
        // Boot screen: compose once on entry.
        if (!m_bootComposed) {
            ui::BootRenderState bootState;
            if (m_ui->buildBootRenderState(bootState)) {
                m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                m_bootRenderer->render(bootState, *m_uiCompositor);
                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                m_bootComposed = true;
                diagPrint("[app] composed Boot screen");
            }
        }
        m_prevHostListValid = false;
        m_prevPairingValid = false;
    } else if (screen == ui::UiScreen::Pairing) {
        // Entering the Pairing screen: reset the Host List snapshot so a later
        // return performs a full recompose (the Pairing screen is composed on
        // top of the cleared surface, so the Host List must be fully redrawn).
        m_prevHostListValid = false;
        // Pairing screen: build the render state.
        ui::PairingRenderState state;
        if (m_ui->buildPairingRenderState(state)) {
            if (!m_prevPairingValid) {
                // First entry into Pairing: full compose.
                m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                m_pairingRenderer->renderFull(state, *m_uiCompositor);
                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                m_prevPairingState = state;
                m_prevPairingValid = true;
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[app] composed Pairing screen (host='%s' state=%d)",
                             state.hostName.c_str(),
                             static_cast<int>(state.state));
                    diagPrint(buf);
                }
            } else if (state.state != m_prevPairingState.state ||
                       state.pin != m_prevPairingState.pin ||
                       state.message != m_prevPairingState.message ||
                       state.hostName != m_prevPairingState.hostName) {
                // State/PIN/message/host change: recompose the panel region.
                const ps4::Rect panelArea = m_pairingRenderer->panelBounds();
                m_pairingRenderer->renderRegion(state, *m_uiCompositor, panelArea);
                m_uiBlitter->addDirtyRect(panelArea, kWidth, kHeight);
                m_prevPairingState = state;
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[app] Pairing redraw (state=%d pin='%s')",
                             static_cast<int>(state.state),
                             state.pin.c_str());
                    diagPrint(buf);
                }
            } else {
                // No change: no recompose. prevPairingState stays current.
                m_prevPairingState = state;
            }
        }
    } else if (screen == ui::UiScreen::Help) {
        // Leaving the Pairing screen: reset its snapshot so a later re-entry
        // performs a full recompose.
        m_prevPairingValid = false;

        // The Help screen is composed on top of the cleared surface, so the
        // underlying Host/App screen must be fully redrawn when Help closes.
        // Reset the Host/App renderer valid flags so a later return performs a
        // full recompose.
        m_prevHostListValid = false;
        m_prevHostGridValid = false;
        m_prevAppListValid = false;
        m_prevAppGridValid = false;

        // Help screen: compose once on entry. The Help content is static, so
        // no per-frame dirty recompose is needed.
        ui::HelpRenderState state;
        if (m_ui->buildHelpRenderState(state)) {
            if (!m_prevHelpValid) {
                // First entry into Help: full compose.
                m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                m_helpRenderer->renderFull(state, *m_uiCompositor);
                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                m_prevHelpState = state;
                m_prevHelpValid = true;
                diagPrint("[app] composed Help screen");
            } else if (state.crossIsAccept != m_prevHelpState.crossIsAccept) {
                // Button-assignment change: recompose the legend.
                const ps4::Rect legendArea = m_helpRenderer->bottomLegendBounds();
                m_helpRenderer->renderRegion(state, *m_uiCompositor, legendArea);
                m_uiBlitter->addDirtyRect(legendArea, kWidth, kHeight);
                m_prevHelpState = state;
            } else {
                // No change: no recompose. prevHelpState stays current.
                m_prevHelpState = state;
            }
        }
    } else if (screen == ui::UiScreen::HostList) {
        // Leaving the Pairing screen: reset its snapshot so a later re-entry
        // performs a full recompose.
        m_prevPairingValid = false;
        // Leaving the Help screen: reset its snapshot so a later re-entry
        // performs a full recompose.
        m_prevHelpValid = false;

        // --- Presentation-style dispatch ------------------------------------
        // Preserve the same renderer choice strategy as the App List:
        //   PlayStation presentation -> HostListRenderer (crossbar path)
        //   Grid presentation        -> HostGridRenderer (grid path)
        const ml::PresentationStyle hostStyle =
            (m_ui && m_ui->isInitialized()) ? m_ui->presentationStyle()
                                            : ml::PresentationStyle::PlayStation;

        if (hostStyle == ml::PresentationStyle::PlayStation) {
            // --- Host List (crossbar) path ----------------------------------
            ui::HostListRenderState state;
            if (m_ui->buildHostListRenderState(state)) {
                if (!m_prevHostListValid) {
                    // First entry into Host List: full compose.
                    m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                    m_hostListRenderer->renderFull(state, *m_uiCompositor);
                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                    m_prevHostListState = state;
                    m_prevHostListValid = true;
                    // The underlying surface was just re-seeded (full compose).
                    // If the Settings overlay is open, it must be fully
                    // re-composed on top of the newly-seeded surface. Reset
                    // m_prevSettingsOpen so the overlay is treated as freshly
                    // opened (full compose) rather than doing a region
                    // recompose that would leave the overlay missing/incomplete.
                    m_prevSettingsOpen = false;
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[app] composed Host List (full, %zu cards)",
                                 state.cards.size());
                        diagPrint(buf);
                    }
                } else {
                    // Compute dirty regions from the previous snapshot. This
                    // detects selection moves, card content changes (availability,
                    // pairing state, display name, app-running), and structural
                    // changes (host added/removed), matching the SCE reference.
                    ps4::Rect dirty[8];
                    const int dirtyCount = hostListDirtyRects(
                        m_prevHostListState, state, dirty, 8);
                    if (dirtyCount > 0) {
                        for (int i = 0; i < dirtyCount; ++i) {
                            m_hostListRenderer->renderRegion(state, *m_uiCompositor, dirty[i]);
                            m_uiBlitter->addDirtyRect(dirty[i], kWidth, kHeight);
                        }
                        m_prevHostListState = state;
                        {
                            char buf[128];
                            snprintf(buf, sizeof(buf),
                                     "[app] Host List redraw (%d rects, index %d, %zu cards)",
                                     dirtyCount, state.selectedIndex, state.cards.size());
                            diagPrint(buf);
                        }
                    } else {
                        // No change: no recompose. prevHostListState stays current.
                        m_prevHostListState = state;
                    }
                }

                // --- Compose the Settings overlay ---------------------------
                // If the Settings overlay is open, compose it on top of the CPU
                // Host List. Detect open/close/selection/value changes and dirty
                // the appropriate regions.
                const bool settingsOpen = m_ui->isSettingsOpen();
                if (settingsOpen || m_prevSettingsOpen) {
                    if (!settingsOpen && m_prevSettingsOpen) {
                        // Close transition: restore the underlying Host List.
                        const ps4::Rect dim = m_settingsRenderer->dimBounds();
                        m_hostListRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevSettingsOpen = false;
                    } else if (settingsOpen) {
                        ui::SettingsRenderState settingsState;
                        if (m_ui->buildSettingsRenderState(settingsState)) {
                            if (!m_prevSettingsOpen) {
                                // Full open: compose the whole overlay.
                                m_settingsRenderer->renderFull(settingsState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // Compute dirty regions from the previous snapshot.
                                // On a selection change this dirties BOTH the old
                                // and new selected row bounds so the previous
                                // highlight is cleared and the new one is drawn.
                                ps4::Rect sdirty[8];
                                const int sdirtyCount = settingsDirtyRects(
                                    m_prevSettingsState, settingsState,
                                    m_prevSettingsOpen, settingsOpen, sdirty, 8);
                                if (sdirtyCount > 0) {
                                    for (int i = 0; i < sdirtyCount; ++i) {
                                        m_settingsRenderer->renderRegion(settingsState, *m_uiCompositor, sdirty[i]);
                                        m_uiBlitter->addDirtyRect(sdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevSettingsOpen = true;
                            m_prevSettingsState = settingsState;
                        }
                    }
                }

                // --- Compose the Delete Host confirmation modal -------------
                const bool deleteOpen = m_ui->isDeleteConfirmOpen();
                if (deleteOpen || m_prevDeleteConfirmOpen) {
                    if (!deleteOpen && m_prevDeleteConfirmOpen) {
                        // Close transition: restore the underlying Host List.
                        const ps4::Rect dim = m_modalRenderer->dimBounds();
                        m_hostListRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevDeleteConfirmOpen = false;
                    } else if (deleteOpen) {
                        ui::ModalRenderState modalState;
                        if (m_ui->buildModalRenderState(modalState)) {
                            if (!m_prevDeleteConfirmOpen) {
                                // Full open: compose the whole modal.
                                m_modalRenderer->renderFull(modalState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else if (modalState.yesSelected != m_prevDeleteModalState.yesSelected) {
                                // Choice change: dirty the choice button region.
                                const ps4::Rect choice = m_modalRenderer->choiceBounds(modalState.kind);
                                m_modalRenderer->renderRegion(modalState, *m_uiCompositor, choice);
                                m_uiBlitter->addDirtyRect(choice, kWidth, kHeight);
                            }
                            m_prevDeleteConfirmOpen = true;
                            m_prevDeleteModalState = modalState;
                        }
                    }
                }

                // --- Compose the Add Host overlay / status modal ------------
                const bool addHostOpen = m_ui->isAddHostOpen() ||
                                         m_ui->isAddHostStatusOpen();
                if (addHostOpen || m_prevAddHostOpen) {
                    if (!addHostOpen && m_prevAddHostOpen) {
                        // Close transition: restore the underlying Host List.
                        const ps4::Rect dim = m_addHostRenderer->dimBounds();
                        m_hostListRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevAddHostOpen = false;
                    } else if (addHostOpen) {
                        ui::AddHostRenderState addHostState;
                        if (m_ui->buildAddHostRenderState(addHostState)) {
                            if (!m_prevAddHostOpen) {
                                // Full open: compose the whole overlay.
                                m_addHostRenderer->renderFull(addHostState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // State change while open: recompose only the
                                // dirty regions (field text, selection,
                                // validation, status message). This avoids
                                // re-applying the full-screen dim layer, which
                                // would progressively darken the background.
                                ps4::Rect adirty[8];
                                const int adirtyCount = addHostDirtyRects(
                                    m_prevAddHostState, addHostState,
                                    m_prevAddHostOpen, addHostOpen, adirty, 8);
                                if (adirtyCount > 0) {
                                    for (int i = 0; i < adirtyCount; ++i) {
                                        m_addHostRenderer->renderRegion(addHostState, *m_uiCompositor, adirty[i]);
                                        m_uiBlitter->addDirtyRect(adirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevAddHostOpen = true;
                            m_prevAddHostState = addHostState;
                        }
                    }
                }
            }
        } else {
            // --- Host List Grid path ----------------------------------------
            ui::HostGridRenderState state;
            if (m_ui->buildHostGridRenderState(state)) {
                if (!m_prevHostGridValid) {
                    // First entry into Host List Grid: full compose.
                    m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                    m_hostGridRenderer->renderFull(state, *m_uiCompositor);
                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                    m_prevHostGridState = state;
                    m_prevHostGridValid = true;
                    // The underlying surface was just re-seeded (full compose).
                    // If the Settings overlay is open, it must be fully
                    // re-composed on top of the newly-seeded surface. Reset
                    // m_prevSettingsOpen so the overlay is treated as freshly
                    // opened (full compose) rather than doing a region
                    // recompose that would leave the overlay missing/incomplete.
                    m_prevSettingsOpen = false;
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[app] composed Host List Grid (full, %zu cards)",
                                 state.hosts.size());
                        diagPrint(buf);
                    }
                } else {
                    // Compute dirty regions from the previous snapshot. This
                    // detects selection moves, card content changes
                    // (availability, pairing state, display name, app-running),
                    // and structural changes (host added/removed), matching the
                    // SCE reference.
                    ps4::Rect dirty[8];
                    const int dirtyCount = hostGridDirtyRects(
                        m_prevHostGridState, state, dirty, 8);
                    if (dirtyCount > 0) {
                        for (int i = 0; i < dirtyCount; ++i) {
                            m_hostGridRenderer->renderRegion(state, *m_uiCompositor, dirty[i]);
                            m_uiBlitter->addDirtyRect(dirty[i], kWidth, kHeight);
                        }
                        m_prevHostGridState = state;
                        {
                            char buf[128];
                            snprintf(buf, sizeof(buf),
                                     "[app] Host List Grid redraw (%d rects, index %d, %zu cards)",
                                     dirtyCount, state.selectedIndex, state.hosts.size());
                            diagPrint(buf);
                        }
                    } else {
                        // No change: no recompose. prevHostGridState stays current.
                        m_prevHostGridState = state;
                    }
                }

                // --- Compose the Settings overlay ---------------------------
                // If the Settings overlay is open, compose it on top of the CPU
                // Host List Grid. Detect open/close/selection/value changes and
                // dirty the appropriate regions.
                const bool settingsOpen = m_ui->isSettingsOpen();
                if (settingsOpen || m_prevSettingsOpen) {
                    if (!settingsOpen && m_prevSettingsOpen) {
                        // Close transition: restore the underlying Host Grid.
                        const ps4::Rect dim = m_settingsRenderer->dimBounds();
                        m_hostGridRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevSettingsOpen = false;
                    } else if (settingsOpen) {
                        ui::SettingsRenderState settingsState;
                        if (m_ui->buildSettingsRenderState(settingsState)) {
                            if (!m_prevSettingsOpen) {
                                // Full open: compose the whole overlay.
                                m_settingsRenderer->renderFull(settingsState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // Compute dirty regions from the previous snapshot.
                                // On a selection change this dirties BOTH the old
                                // and new selected row bounds so the previous
                                // highlight is cleared and the new one is drawn.
                                ps4::Rect sdirty[8];
                                const int sdirtyCount = settingsDirtyRects(
                                    m_prevSettingsState, settingsState,
                                    m_prevSettingsOpen, settingsOpen, sdirty, 8);
                                if (sdirtyCount > 0) {
                                    for (int i = 0; i < sdirtyCount; ++i) {
                                        m_settingsRenderer->renderRegion(settingsState, *m_uiCompositor, sdirty[i]);
                                        m_uiBlitter->addDirtyRect(sdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevSettingsOpen = true;
                            m_prevSettingsState = settingsState;
                        }
                    }
                }

                // --- Compose the Delete Host confirmation modal -------------
                const bool deleteOpen = m_ui->isDeleteConfirmOpen();
                if (deleteOpen || m_prevDeleteConfirmOpen) {
                    if (!deleteOpen && m_prevDeleteConfirmOpen) {
                        // Close transition: restore the underlying Host Grid.
                        const ps4::Rect dim = m_modalRenderer->dimBounds();
                        m_hostGridRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevDeleteConfirmOpen = false;
                    } else if (deleteOpen) {
                        ui::ModalRenderState modalState;
                        if (m_ui->buildModalRenderState(modalState)) {
                            if (!m_prevDeleteConfirmOpen) {
                                // Full open: compose the whole modal.
                                m_modalRenderer->renderFull(modalState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else if (modalState.yesSelected != m_prevDeleteModalState.yesSelected) {
                                // Choice change: dirty the choice button region.
                                const ps4::Rect choice = m_modalRenderer->choiceBounds(modalState.kind);
                                m_modalRenderer->renderRegion(modalState, *m_uiCompositor, choice);
                                m_uiBlitter->addDirtyRect(choice, kWidth, kHeight);
                            }
                            m_prevDeleteConfirmOpen = true;
                            m_prevDeleteModalState = modalState;
                        }
                    }
                }

                // --- Compose the Add Host overlay / status modal ------------
                const bool addHostOpen = m_ui->isAddHostOpen() ||
                                         m_ui->isAddHostStatusOpen();
                if (addHostOpen || m_prevAddHostOpen) {
                    if (!addHostOpen && m_prevAddHostOpen) {
                        // Close transition: restore the underlying Host Grid.
                        const ps4::Rect dim = m_addHostRenderer->dimBounds();
                        m_hostGridRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevAddHostOpen = false;
                    } else if (addHostOpen) {
                        ui::AddHostRenderState addHostState;
                        if (m_ui->buildAddHostRenderState(addHostState)) {
                            if (!m_prevAddHostOpen) {
                                // Full open: compose the whole overlay.
                                m_addHostRenderer->renderFull(addHostState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // State change while open: recompose only the
                                // dirty regions (field text, selection,
                                // validation, status message). This avoids
                                // re-applying the full-screen dim layer, which
                                // would progressively darken the background.
                                ps4::Rect adirty[8];
                                const int adirtyCount = addHostDirtyRects(
                                    m_prevAddHostState, addHostState,
                                    m_prevAddHostOpen, addHostOpen, adirty, 8);
                                if (adirtyCount > 0) {
                                    for (int i = 0; i < adirtyCount; ++i) {
                                        m_addHostRenderer->renderRegion(addHostState, *m_uiCompositor, adirty[i]);
                                        m_uiBlitter->addDirtyRect(adirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevAddHostOpen = true;
                            m_prevAddHostState = addHostState;
                        }
                    }
                }
            }
        }
    } else if (screen == ui::UiScreen::AppList) {
        // Leaving the Pairing screen: reset its snapshot so a later re-entry
        // performs a full recompose.
        m_prevPairingValid = false;
        // Leaving the Help screen: reset its snapshot so a later re-entry
        // performs a full recompose.
        m_prevHelpValid = false;

        // --- Presentation-style dispatch ------------------------------------
        // Preserve the same renderer choice strategy as the Host List:
        //   PlayStation presentation -> AppListRenderer (crossbar path)
        //   Grid presentation        -> AppGridRenderer (grid path)
        // The App List screen is reachable through a real ApplicationCatalog;
        // the renderer path is dispatched here.
        const ml::PresentationStyle appStyle =
            (m_ui && m_ui->isInitialized()) ? m_ui->presentationStyle()
                                            : ml::PresentationStyle::PlayStation;

        if (appStyle == ml::PresentationStyle::PlayStation) {
            // --- App List (crossbar) path -----------------------------------
            ui::AppListRenderState state;
            if (m_ui->buildAppListRenderState(state)) {
                // Populate the resolved runtime artwork pointers into the
                // render state. Null means fall back to app_fallback.png.
                populateAppListArtwork(state);
                if (!m_prevAppListValid) {
                    // First entry into App List: full compose.
                    m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                    m_appListRenderer->renderFull(state, *m_uiCompositor);
                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                    m_prevAppListState = state;
                    m_prevAppListValid = true;
                    // The underlying surface was just re-seeded (full compose).
                    // If the Settings overlay is open, it must be fully
                    // re-composed on top of the newly-seeded surface. Reset
                    // m_prevSettingsOpen so the overlay is treated as freshly
                    // opened (full compose) rather than doing a region
                    // recompose that would leave the overlay missing/incomplete.
                    m_prevSettingsOpen = false;
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[app] composed App List (full, %zu cards)",
                                 state.cards.size());
                        diagPrint(buf);
                    }
                } else {
                    // Compute dirty regions from the previous snapshot. This
                    // detects selection moves, per-card content changes
                    // (running, connecting, quitting, title, artwork), and
                    // structural changes (app added/removed), matching the
                    // Host List dirty-region behavior.
                    ps4::Rect dirty[8];
                    const int dirtyCount = appListDirtyRects(
                        m_prevAppListState, state, dirty, 8);
                    if (dirtyCount > 0) {
                        for (int i = 0; i < dirtyCount; ++i) {
                            m_appListRenderer->renderRegion(state, *m_uiCompositor, dirty[i]);
                            m_uiBlitter->addDirtyRect(dirty[i], kWidth, kHeight);
                        }
                        m_prevAppListState = state;
                        {
                            char buf[128];
                            snprintf(buf, sizeof(buf),
                                     "[app] App List redraw (%d rects, index %d, %zu cards)",
                                     dirtyCount, state.selectedIndex, state.cards.size());
                            diagPrint(buf);
                        }
                    } else {
                        // No change: no recompose. prevAppListState stays current.
                        m_prevAppListState = state;
                    }
                }

                // --- Compose the Settings overlay on top of the App List ----
                const bool settingsOpen = m_ui->isSettingsOpen();
                if (settingsOpen || m_prevSettingsOpen) {
                    if (!settingsOpen && m_prevSettingsOpen) {
                        const ps4::Rect dim = m_settingsRenderer->dimBounds();
                        m_appListRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevSettingsOpen = false;
                    } else if (settingsOpen) {
                        ui::SettingsRenderState settingsState;
                        if (m_ui->buildSettingsRenderState(settingsState)) {
                            if (!m_prevSettingsOpen) {
                                m_settingsRenderer->renderFull(settingsState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // Compute dirty regions from the previous
                                // snapshot. On a selection change this dirties
                                // BOTH the old and new selected row bounds so
                                // the previous highlight is cleared and the
                                // new one is drawn.
                                ps4::Rect sdirty[8];
                                const int sdirtyCount = settingsDirtyRects(
                                    m_prevSettingsState, settingsState,
                                    m_prevSettingsOpen, settingsOpen, sdirty, 8);
                                if (sdirtyCount > 0) {
                                    for (int i = 0; i < sdirtyCount; ++i) {
                                        m_settingsRenderer->renderRegion(settingsState, *m_uiCompositor, sdirty[i]);
                                        m_uiBlitter->addDirtyRect(sdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevSettingsOpen = true;
                            m_prevSettingsState = settingsState;
                        }
                    }
                }

                // --- Compose the Quit App confirmation dialog --------------
                // If the quit-app confirmation dialog is open, compose it on
                // top of the App List. Detect open/close and dirty the
                // appropriate regions.
                const bool quitAppOpen = m_ui->isQuitAppConfirmOpen();
                if (quitAppOpen || m_prevQuitAppOpen) {
                    if (!quitAppOpen && m_prevQuitAppOpen) {
                        // Close transition: restore the underlying App List.
                        const ps4::Rect dim = m_modalRenderer->dimBounds();
                        m_appListRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevQuitAppOpen = false;
                    } else if (quitAppOpen) {
                        ui::ModalRenderState modalState;
                        if (m_ui->buildModalRenderState(modalState)) {
                            ps4::Rect mdirty[8];
                            const int mdirtyCount = modalDirtyRects(
                                m_prevQuitModalState, modalState,
                                m_prevQuitAppOpen, quitAppOpen,
                                mdirty, 8);
                            if (mdirtyCount > 0) {
                                if (!m_prevQuitAppOpen) {
                                    // Full open: compose the whole modal.
                                    m_modalRenderer->renderFull(modalState, *m_uiCompositor);
                                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                                } else {
                                    // Choice change while open.
                                    for (int i = 0; i < mdirtyCount; ++i) {
                                        m_modalRenderer->renderRegion(modalState, *m_uiCompositor, mdirty[i]);
                                        m_uiBlitter->addDirtyRect(mdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevQuitAppOpen = true;
                            m_prevQuitModalState = modalState;
                        }
                    }
                }
            }
            // The artwork-dirty set has been consumed by the dirty-region
            // computation above; clear it for the next frame.
            m_artworkDirtyApps.clear();
        } else {
            // --- App List Grid path -----------------------------------------
            ui::AppGridRenderState state;
            if (m_ui->buildAppGridRenderState(state)) {
                // Populate the resolved runtime artwork pointers into the
                // render state. Null means fall back to app_fallback.png.
                populateAppGridArtwork(state);
                if (!m_prevAppGridValid) {
                    // First entry into App List Grid: full compose.
                    m_uiCompositor->clear(kFallbackBgR, kFallbackBgG, kFallbackBgB, 255);
                    m_appGridRenderer->renderFull(state, *m_uiCompositor);
                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                    m_prevAppGridState = state;
                    m_prevAppGridValid = true;
                    // The underlying surface was just re-seeded (full compose).
                    // If the Settings overlay is open, it must be fully
                    // re-composed on top of the newly-seeded surface. Reset
                    // m_prevSettingsOpen so the overlay is treated as freshly
                    // opened (full compose) rather than doing a region
                    // recompose that would leave the overlay missing/incomplete.
                    m_prevSettingsOpen = false;
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[app] composed App List Grid (full, %zu cards)",
                                 state.cards.size());
                        diagPrint(buf);
                    }
                } else {
                    // Compute dirty regions from the previous snapshot. This
                    // detects selection/scroll movement, per-card content
                    // changes (running, connecting, quitting, title, artwork),
                    // and structural changes (app added/removed), matching the
                    // Host Grid dirty-region behavior.
                    ps4::Rect dirty[8];
                    const int dirtyCount = appGridDirtyRects(
                        m_prevAppGridState, state, dirty, 8);
                    if (dirtyCount > 0) {
                        for (int i = 0; i < dirtyCount; ++i) {
                            m_appGridRenderer->renderRegion(state, *m_uiCompositor, dirty[i]);
                            m_uiBlitter->addDirtyRect(dirty[i], kWidth, kHeight);
                        }
                        m_prevAppGridState = state;
                        {
                            char buf[128];
                            snprintf(buf, sizeof(buf),
                                     "[app] App List Grid redraw (%d rects, index %d, %zu cards)",
                                     dirtyCount, state.selectedIndex, state.cards.size());
                            diagPrint(buf);
                        }
                    } else {
                        // No change: no recompose. prevAppGridState stays current.
                        m_prevAppGridState = state;
                    }
                }

                // --- Compose the Settings overlay on top of the App Grid ----
                const bool settingsOpen = m_ui->isSettingsOpen();
                if (settingsOpen || m_prevSettingsOpen) {
                    if (!settingsOpen && m_prevSettingsOpen) {
                        const ps4::Rect dim = m_settingsRenderer->dimBounds();
                        m_appGridRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevSettingsOpen = false;
                    } else if (settingsOpen) {
                        ui::SettingsRenderState settingsState;
                        if (m_ui->buildSettingsRenderState(settingsState)) {
                            if (!m_prevSettingsOpen) {
                                m_settingsRenderer->renderFull(settingsState, *m_uiCompositor);
                                m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                            } else {
                                // Compute dirty regions from the previous
                                // snapshot. On a selection change this dirties
                                // BOTH the old and new selected row bounds so
                                // the previous highlight is cleared and the
                                // new one is drawn.
                                ps4::Rect sdirty[8];
                                const int sdirtyCount = settingsDirtyRects(
                                    m_prevSettingsState, settingsState,
                                    m_prevSettingsOpen, settingsOpen, sdirty, 8);
                                if (sdirtyCount > 0) {
                                    for (int i = 0; i < sdirtyCount; ++i) {
                                        m_settingsRenderer->renderRegion(settingsState, *m_uiCompositor, sdirty[i]);
                                        m_uiBlitter->addDirtyRect(sdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevSettingsOpen = true;
                            m_prevSettingsState = settingsState;
                        }
                    }
                }

                // --- Compose the Quit App confirmation dialog --------------
                // If the quit-app confirmation dialog is open, compose it on
                // top of the App List Grid. Detect open/close and dirty the
                // appropriate regions.
                const bool quitAppOpen = m_ui->isQuitAppConfirmOpen();
                if (quitAppOpen || m_prevQuitAppOpen) {
                    if (!quitAppOpen && m_prevQuitAppOpen) {
                        // Close transition: restore the underlying App Grid.
                        const ps4::Rect dim = m_modalRenderer->dimBounds();
                        m_appGridRenderer->renderRegion(state, *m_uiCompositor, dim);
                        m_uiBlitter->addDirtyRect(dim, kWidth, kHeight);
                        m_prevQuitAppOpen = false;
                    } else if (quitAppOpen) {
                        ui::ModalRenderState modalState;
                        if (m_ui->buildModalRenderState(modalState)) {
                            ps4::Rect mdirty[8];
                            const int mdirtyCount = modalDirtyRects(
                                m_prevQuitModalState, modalState,
                                m_prevQuitAppOpen, quitAppOpen,
                                mdirty, 8);
                            if (mdirtyCount > 0) {
                                if (!m_prevQuitAppOpen) {
                                    // Full open: compose the whole modal.
                                    m_modalRenderer->renderFull(modalState, *m_uiCompositor);
                                    m_uiBlitter->addFullSurfaceDirty(kWidth, kHeight);
                                } else {
                                    // Choice change while open.
                                    for (int i = 0; i < mdirtyCount; ++i) {
                                        m_modalRenderer->renderRegion(modalState, *m_uiCompositor, mdirty[i]);
                                        m_uiBlitter->addDirtyRect(mdirty[i], kWidth, kHeight);
                                    }
                                }
                            }
                            m_prevQuitAppOpen = true;
                            m_prevQuitModalState = modalState;
                        }
                    }
                }
            }
            // The artwork-dirty set has been consumed by the dirty-region
            // computation above; clear it for the next frame.
            m_artworkDirtyApps.clear();
        }
    }
    // Other screens (StreamExitHint, StreamStatistics, StreamEmpty) are
    // handled by the stream presentation path in render() above.
}

// Phase 6: Present the completed frame.
void MoonlightApp::present() {
    if (!m_gfx || !m_gfx->isInitialized()) {
        return;
    }

    // During streaming, the framebuffer holds the decoded video (with any
    // in-stream overlay already alpha-blended over it by composeStreamOverlay).
    // The CPU compositor's pending dirty regions must NOT be uploaded, because
    // they would overwrite the video with stale CPU UI content. The compositor
    // is only used for the CPU UI path (non-stream).
    if (!m_streamActive) {
        // Upload the current slot's pending dirty regions (if any) from the
        // CPU compositor surface into the current framebuffer slot.
        ps4::DirtyUploadStats stats = m_uiBlitter->uploadPendingDirty(
            m_uiCompositor->pixels(), m_uiCompositor->width(),
            m_uiCompositor->height(), m_uiCompositor->pitch());
        (void)stats;
    }

    ps4::GfxResult endResult = m_gfx->endFrame();
    if (!endResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] endFrame failed: category=%d",
                 static_cast<int>(endResult.category));
        diagPrint(buf);
        requestExit(ExitReason::RuntimeFailure);
        return;
    }

    ps4::GfxResult presentResult = m_gfx->present();
    if (!presentResult.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] present failed: category=%d",
                 static_cast<int>(presentResult.category));
        diagPrint(buf);
        requestExit(ExitReason::RuntimeFailure);
        return;
    }
}

// ---------------------------------------------------------------------------
// Stream video presentation
// ---------------------------------------------------------------------------

/// Present the latest decoded video frame into the current GfxManager
/// framebuffer. Converts NV12→BGRA on the CPU. Called from render() while
/// the stream is active. Returns true if a frame was presented.
///
/// Frame-lifetime management (Phase 3): the decoder must not reuse a surface
/// that the renderer (or the decoder's DPB) may still reference. When a new
/// frame is acquired, the previous held frame is moved into m_retiredFrames
/// instead of being released immediately. The retired frames are released on
/// the next beginFrame() (which confirms the previous frame was presented),
/// so the decoder cannot overwrite a surface that was just displayed.
bool MoonlightApp::presentStreamVideoFrame() {
    if (!m_gfx || !m_gfx->isInitialized() || !m_videoDecoder) {
        return false;
    }

    // --- 1. Release retired frames from the previous frame ------------------
    // GfxManager::beginFrame() (called before this in render()) has confirmed
    // the previous graphics slot is safe for reuse, so any frame drawn to it
    // in a previous submission is no longer being read. Release the retired
    // frames now so their surfaces return to the decoder pool.
    m_retiredFrames.clear();

    // --- 2. Detect whether the held frame's surface was reused --------------
    // If the decoder re-acquired the held frame's surface (generation advanced)
    // while we still held it, that is a frame-lifetime violation (stale-content
    // corruption). Log it for diagnostics.
    if (m_heldDecodedFrame.isValid()) {
        const uint32_t slot = m_heldDecodedFrame.surfaceSlot();
        const uint32_t heldGen = m_videoDecoder->surfaceGeneration(slot);
        // The held frame's generation is not directly exposed; compare the
        // current decoder generation against the slot reference recorded when
        // the frame was drawn. If it advanced, the surface was reused.
        const uint32_t frameSlot = m_gfx->currentFrameIndex();
        if (m_streamSlotRefs[frameSlot].valid &&
            m_streamSlotRefs[frameSlot].surfaceSlot == slot &&
            m_streamSlotRefs[frameSlot].generation != heldGen) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[video-lifetime] SURFACE REUSED WHILE HELD slot=%u oldGen=%u newGen=%u frame=%llu",
                     slot, m_streamSlotRefs[frameSlot].generation, heldGen,
                     static_cast<unsigned long long>(m_heldDecodedFrame.frameNumber()));
            diagPrint(buf);
        }
    }

    // --- 3. Acquire the latest decoded frame --------------------------------
    // If a new frame is available, retire the old held frame (move it into
    // m_retiredFrames) and hold the new one. If no new frame is available,
    // keep displaying the last held frame so a static scene does not go black.
    ml::DecodedVideoFrame newFrame;
    if (m_videoDecoder->tryAcquireLatestFrame(newFrame)) {
        if (newFrame.isValid()) {
            if (m_heldDecodedFrame.isValid()) {
                // Retire the old frame instead of releasing it immediately.
                m_retiredFrames.push_back(std::move(m_heldDecodedFrame));
            }
            m_heldDecodedFrame = std::move(newFrame);
        }
    }

    // If we have a valid held frame, present it.
    if (!m_heldDecodedFrame.isValid()) {
        return false;
    }

    // Get the current framebuffer.
    void* fb = m_gfx->currentFramebuffer();
    if (fb == nullptr) {
        return false;
    }
    const size_t fbPitch = m_gfx->framebufferStrideBytes();
    const uint32_t fbW = m_gfx->getFramebufferWidth();
    const uint32_t fbH = m_gfx->getFramebufferHeight();

    // Convert NV12 → BGRA into the framebuffer.
    ps4::Nv12Converter::nv12ToBgra(
        static_cast<uint8_t*>(fb), static_cast<int>(fbPitch),
        static_cast<int>(fbW), static_cast<int>(fbH),
        static_cast<const uint8_t*>(m_heldDecodedFrame.yPlane()),
        static_cast<const uint8_t*>(m_heldDecodedFrame.uvPlane()),
        static_cast<int>(m_heldDecodedFrame.pitch()),
        static_cast<int>(m_heldDecodedFrame.pitch()),
        static_cast<int>(m_heldDecodedFrame.width()),
        static_cast<int>(m_heldDecodedFrame.height()));

    // --- 4. Record which frame was drawn to this graphics slot --------------
    // Track the surface slot and its generation so a future frame can detect
    // whether the surface was reused while still referenced.
    const uint32_t frameSlot = m_gfx->currentFrameIndex();
    m_streamSlotRefs[frameSlot].frameNumber = m_heldDecodedFrame.frameNumber();
    m_streamSlotRefs[frameSlot].surfaceSlot = m_heldDecodedFrame.surfaceSlot();
    m_streamSlotRefs[frameSlot].generation =
        m_videoDecoder->surfaceGeneration(m_heldDecodedFrame.surfaceSlot());
    m_streamSlotRefs[frameSlot].valid = true;

    // Diagnostic: log the first presented frame.
    if (!m_streamFirstFramePresented) {
        m_streamFirstFramePresented = true;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[video] first frame presented (frame=%llu %ux%u)",
                 static_cast<unsigned long long>(m_heldDecodedFrame.frameNumber()),
                 m_heldDecodedFrame.width(), m_heldDecodedFrame.height());
        diagPrint(buf);
        // The first real video frame is now in the framebuffer, so the exit
        // hint is actually visible over video. Start its 3-second countdown
        // now (it was armed but not started in beginStreamSession).
        if (m_ui && m_ui->isInitialized()) {
            m_ui->startStreamHintTimer();
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// In-stream overlay composition
// ---------------------------------------------------------------------------

/// Compose the in-stream overlay (exit hint / statistics) over the decoded
/// video already in the current framebuffer. Called from render() while the
/// stream is active, after presentStreamVideoFrame().
///
/// Composition model: the overlay renderers draw into the CPU compositor
/// surface (which is transparent except for the overlay pixels). The
/// compositor surface is then alpha-blended over the framebuffer (which holds
/// the decoded video), preserving the underlying video wherever the compositor
/// is transparent. The retained decode image (m_heldDecodedFrame) is never
/// mutated, so the overlay cannot be burned into future frames.
///
/// The active overlay is determined by the UiManager's current stream screen:
///   StreamExitHint    → draw the exit hint.
///   StreamStatistics  → draw the statistics overlay.
///   StreamEmpty       → no overlay.
///
/// Because the video is re-converted from the clean held frame each frame and
/// the overlay is freshly blended, a static scene still reflects overlay
/// timing changes (hint expiry, statistics value updates) without requiring a
/// new decoded frame.
void MoonlightApp::composeStreamOverlay() {
    if (!m_ui || !m_ui->isInitialized() ||
        !m_uiCompositor || !m_uiBlitter) {
        return;
    }

    const ui::UiScreen screen = m_ui->currentScreen();

    if (screen == ui::UiScreen::StreamExitHint) {
        // --- Exit hint overlay ----------------------------------------------
        ui::StreamExitHintRenderState hintState;
        if (!m_ui->buildStreamExitHintRenderState(hintState)) {
            return;
        }
        if (!m_streamExitHintRenderer ||
            !m_streamExitHintRenderer->isInitialized()) {
            return;
        }
        // Clear the compositor's hint region to transparent, draw the hint,
        // then alpha-blend the region over the framebuffer. The video is
        // preserved wherever the compositor is transparent.
        const ps4::Rect hintBounds =
            m_streamExitHintRenderer->bounds();
        clearCompositorRegion(hintBounds);
        m_streamExitHintRenderer->render(hintState, *m_uiCompositor);
        m_uiBlitter->blendOverFramebuffer(
            m_uiCompositor->pixels(), m_uiCompositor->width(),
            m_uiCompositor->height(), m_uiCompositor->pitch(), hintBounds);
    } else if (screen == ui::UiScreen::StreamStatistics) {
        // --- Statistics overlay ---------------------------------------------
        ui::StreamStatisticsRenderState statsState;
        if (!m_ui->buildStreamStatisticsRenderState(statsState)) {
            return;
        }
        if (!m_streamStatisticsRenderer ||
            !m_streamStatisticsRenderer->isInitialized()) {
            return;
        }
        // Clear the compositor's statistics region to transparent, draw the
        // overlay, then alpha-blend the region over the framebuffer.
        const ps4::Rect statsBounds = m_streamStatisticsRenderer->bounds();
        clearCompositorRegion(statsBounds);
        m_streamStatisticsRenderer->render(statsState, *m_uiCompositor);
        m_uiBlitter->blendOverFramebuffer(
            m_uiCompositor->pixels(), m_uiCompositor->width(),
            m_uiCompositor->height(), m_uiCompositor->pitch(), statsBounds);
        // Emit the "statistics overlay composed" diagnostic at most once per
        // stream session (reset on stream connect).
        if (!m_streamStatsOverlayLogged) {
            m_streamStatsOverlayLogged = true;
            diagPrint("[stream] statistics overlay composed");
        }
    }
    // StreamEmpty (or any other screen): no base overlay is drawn. The video
    // is presented cleanly because the compositor is not blended over it.

    // --- Mouse emulation state overlay -------------------------------------
    // The transient mouse emulation state overlay is drawn on top of whatever
    // base stream overlay is active (exit hint, statistics, or empty). It is
    // active only while the UiManager's overlay timer is running. When the
    // statistics overlay is active, both remain visible until the mouse
    // emulation overlay timer expires, then only the statistics overlay
    // remains.
    if (m_ui && m_ui->isInitialized() &&
        m_ui->isMouseEmulationOverlayActive()) {
        ui::MouseEmulationRenderState mouseState;
        if (m_ui->buildMouseEmulationRenderState(mouseState) &&
            m_mouseEmulationOverlayRenderer &&
            m_mouseEmulationOverlayRenderer->isInitialized()) {
            const ps4::Rect mouseBounds =
                m_mouseEmulationOverlayRenderer->bounds();
            clearCompositorRegion(mouseBounds);
            m_mouseEmulationOverlayRenderer->render(mouseState, *m_uiCompositor);
            m_uiBlitter->blendOverFramebuffer(
                m_uiCompositor->pixels(), m_uiCompositor->width(),
                m_uiCompositor->height(), m_uiCompositor->pitch(), mouseBounds);
        }
    }
}

/// Clear a region of the CPU compositor surface to fully transparent. Used
/// before drawing an in-stream overlay so the compositor region holds only the
/// freshly drawn overlay pixels (no stale content from a previous overlay or
/// the CPU UI).
void MoonlightApp::clearCompositorRegion(const ps4::Rect& rect) {
    if (!m_uiCompositor) {
        return;
    }
    uint8_t* px = m_uiCompositor->pixelsWritable();
    if (!px) {
        return;
    }
    const uint32_t pitch = m_uiCompositor->pitch();
    const uint32_t width = m_uiCompositor->width();
    const uint32_t height = m_uiCompositor->height();

    // Clip the region to the surface bounds.
    int x0 = rect.x;
    int y0 = rect.y;
    int x1 = rect.x + rect.w;
    int y1 = rect.y + rect.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int>(width)) x1 = static_cast<int>(width);
    if (y1 > static_cast<int>(height)) y1 = static_cast<int>(height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    for (int y = y0; y < y1; ++y) {
        uint8_t* row = px + static_cast<size_t>(y) * pitch +
                       static_cast<size_t>(x0) * 4;
        memset(row, 0, static_cast<size_t>(x1 - x0) * 4);
    }
}

// ---------------------------------------------------------------------------
// Stream statistics snapshot
// ---------------------------------------------------------------------------

/// Reset the stream statistics interval baseline for a new stream session.
/// Called on stream connect so the first snapshot captures an immediate
/// baseline and no counters leak from a previous session.
void MoonlightApp::resetStreamStatisticsBaseline() {
    m_streamStatsNextUpdateUs = 0;   // forces an immediate snapshot on next update
    m_streamStatsPrevTimeUs = m_currTimestamp;
    m_streamStatsPrevDecodedFrames = 0;
    m_streamStatsPrevDecodeTotalUs = 0;
    m_streamStatsPrevDecodeCount = 0;
    m_streamStatsPrevRxBytes = 0;
}

/// Update the in-stream statistics display snapshot once per second while a
/// stream is active and statistics are enabled. Reads cumulative counters from
/// the subsystems, computes interval deltas, and pushes the resulting snapshot
/// to UiManager. No-op when statistics are disabled or no stream is active.
///
/// Data provenance:
///   resolution   → VideoDecoder::readDecodeStats (negotiated width/height).
///   decoded FPS  → VideoDecoder decoded-frame counter (interval delta).
///   decode time  → VideoDecoder measured decode time (interval average).
///   bitrate      → StreamSessionManager rxBytes (measured encoded-video
///                  bitrate, interval delta).
///   network RTT / jitter → not available in the current dependency stack;
///                  left at 0 so the renderer shows "N/A".
void MoonlightApp::updateStreamStatistics() {
    if (!m_streamActive || !m_ui || !m_ui->isInitialized()) {
        return;
    }
    if (!m_settingsManager ||
        !m_settingsManager->settings().streamStatisticsEnabled) {
        return;
    }

    // Deadline-based cadence (~1 Hz). 0 forces an immediate snapshot on the
    // first update after stream connect.
    const uint64_t nowUs = m_currTimestamp;
    if (nowUs < m_streamStatsNextUpdateUs) {
        return;
    }
    m_streamStatsNextUpdateUs = nowUs + kStreamStatsIntervalUs;

    // --- Read the cumulative counters from the subsystems ------------------
    ml::StreamStatsCounters decodeStats;
    if (m_videoDecoder) {
        m_videoDecoder->readDecodeStats(decodeStats);
    }
    const uint64_t rxBytes =
        m_streamSessionManager ? m_streamSessionManager->rxBytes() : 0;

    // --- Compute interval deltas -------------------------------------------
    const uint64_t dtUs = nowUs - m_streamStatsPrevTimeUs;
    const uint64_t dFrames = decodeStats.decodedFrames - m_streamStatsPrevDecodedFrames;
    const uint64_t dDecodeUs = decodeStats.decodeTotalUs - m_streamStatsPrevDecodeTotalUs;
    const uint64_t dDecodeCount = decodeStats.decodeCount - m_streamStatsPrevDecodeCount;
    const uint64_t dRxBytes = rxBytes - m_streamStatsPrevRxBytes;

    // Update the baseline for the next interval.
    m_streamStatsPrevTimeUs = nowUs;
    m_streamStatsPrevDecodedFrames = decodeStats.decodedFrames;
    m_streamStatsPrevDecodeTotalUs = decodeStats.decodeTotalUs;
    m_streamStatsPrevDecodeCount = decodeStats.decodeCount;
    m_streamStatsPrevRxBytes = rxBytes;

    // --- Build the display snapshot ----------------------------------------
    // The UiManager consumes a ui::StreamStatisticsSnapshot (the UI-facing
    // type). The ml::StreamStatisticsSnapshot is the production counter type;
    // the two have identical fields. Build the UI-facing snapshot directly.
    ui::StreamStatisticsSnapshot snap;
    snap.valid = true;
    snap.width = decodeStats.width;
    snap.height = decodeStats.height;

    if (dtUs > 0) {
        const double dtSec = static_cast<double>(dtUs) / kUsecPerSec;
        // Decoded FPS (interval average).
        snap.fps = static_cast<float>(static_cast<double>(dFrames) / dtSec);
        // Measured encoded-video bitrate (Mbps, interval).
        snap.bitrateMbps = static_cast<float>(
            (static_cast<double>(dRxBytes) * 8.0) / dtSec / 1000000.0);
    }
    // Average decode time (ms, interval).
    if (dDecodeCount > 0) {
        snap.decodeMs = static_cast<float>(
            static_cast<double>(dDecodeUs) / static_cast<double>(dDecodeCount) /
            1000.0);
    }
    // Network RTT (latency) and jitter from the active control stream, using
    // the existing moonlight-common-c LiGetEstimatedRttInfo() public API.
    if (m_streamSessionManager) {
        float rttMs = 0.0f;
        float jitterMs = 0.0f;
        if (m_streamSessionManager->readNetworkStats(rttMs, jitterMs)) {
            snap.rttMs = rttMs;
            snap.jitterMs = jitterMs;
        }
    }

    m_ui->setStreamStatistics(snap);
}

// ---------------------------------------------------------------------------
// UI intent routing
// ---------------------------------------------------------------------------

void MoonlightApp::handleUiIntent(const ui::UiIntent& intent) {
    switch (intent.type) {
    case ui::UiIntentType::None:
        break;
    case ui::UiIntentType::ActivateHost:
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[app] intent: ActivateHost hostId=%llu",
                     static_cast<unsigned long long>(intent.hostId));
            diagPrint(buf);

            const ml::Host* host =
                m_hostManager ? m_hostManager->findHost(intent.hostId) : nullptr;
            if (!host) {
                char buf2[128];
                snprintf(buf2, sizeof(buf2),
                         "[app] host activation: unknown HostId %llu (ignored)",
                         static_cast<unsigned long long>(intent.hostId));
                diagPrint(buf2);
                break;
            }

            // Preconditions for any routing. A manually-added host (which does
            // not respond via mDNS and is therefore not Online) is treated as
            // equivalent to Online so it can enter the pairing flow.
            if (host->availability != ml::HostAvailability::Online &&
                host->availability != ml::HostAvailability::ManuallyAdded) {
                diagPrint("[app] host activation: host not Online (ignored)");
                break;
            }
            if (host->serverInfoState != ml::ServerInfoQueryState::Valid) {
                diagPrint("[app] host activation: serverinfo not Valid (ignored)");
                break;
            }

            switch (host->pairingState) {
            case ml::HostPairingState::Unpaired:
                diagPrint("[app] host activation: Unpaired → starting pairing");
                {
                    // Build the pairing target from the authoritative HostManager
                    // state (not a stale UI-card copy).
                    ml::PairingTarget target;
                    target.hostId = host->id;
                    target.displayName = host->displayName;
                    target.serverId = host->serverId;
                    target.ipv4Addresses = host->ipv4Addresses;
                    target.discoveryPort = host->port;
                    target.httpsPort = host->httpsPort;

                    ml::PairingResult result = m_pairingManager->startPairing(target);
                    if (!result.succeeded()) {
                        char buf2[128];
                        snprintf(buf2, sizeof(buf2),
                                 "[app] pairing start failed (category %d)",
                                 static_cast<int>(result.category));
                        diagPrint(buf2);
                    } else {
                        // Show the Pairing screen.
                        ui::PairingViewState view;
                        view.hostId = host->id;
                        view.hostName = host->displayName;
                        view.state = ui::PairingPresentationState::Preparing;
                        view.statusText = "Preparing...";
                        if (m_ui && m_ui->isInitialized()) {
                            m_ui->setPairingView(view);
                        }
                    }
                }
                break;
            case ml::HostPairingState::Paired:
                diagPrint("[app] host activation: Paired → opening App List");
                startApplicationCatalog(host->id);
                break;
            case ml::HostPairingState::Unknown:
            default:
                diagPrint("[app] host activation: Unknown pairing state — do not guess");
                break;
            }
        }
        break;
    case ui::UiIntentType::CancelPairing:
        diagPrint("[app] intent: CancelPairing");
        if (m_pairingManager) {
            m_pairingManager->cancelPairing();
        }
        break;
    case ui::UiIntentType::Back:
        diagPrint("[app] intent: Back");
        // If a launch/resume is active, cancel it.
        if (m_applicationSessionManager && m_applicationSessionManager->isLaunching()) {
            m_applicationSessionManager->cancel();
            m_launchActive = false;
            // The launch was cancelled before the stream was established.
            // Clear the transient Connecting state so the App List returns to
            // the normal Host.currentGame-derived presentation.
            clearConnectingState();
            // Remain on App List; the launch result (if any) will be handled
            // by handleApplicationLaunchUpdate.
            break;
        }
        // If we are on the App List screen and loading, cancel the catalog request.
        if (m_applicationCatalog && m_applicationCatalog->isLoading()) {
            m_applicationCatalog->cancel();
        }
        // Clear the current catalog state.
        m_catalogHostId = 0;
        m_catalogGeneration = 0;
        m_catalogApplications.clear();
        m_catalogState = ml::ApplicationCatalogState::Idle;
        // Return the UI to the Host List.
        if (m_ui && m_ui->isInitialized()) {
            ui::ApplicationListView view;
            view.state = ml::ApplicationCatalogState::Idle;
            m_ui->setApplicationListView(view);
        }
        break;
    case ui::UiIntentType::ActivateApplication:
        diagPrint("[app] intent: ActivateApplication");
        handleActivateApplication(intent.hostId, intent.applicationId);
        break;
    case ui::UiIntentType::QuitApplication:
        diagPrint("[app] intent: QuitApplication");
        handleQuitApplication(intent.hostId, intent.applicationId);
        break;
    case ui::UiIntentType::QuitAndLaunchApplication:
        diagPrint("[app] intent: QuitAndLaunchApplication");
        handleQuitAndLaunchApplication(intent.hostId, intent.applicationId,
                                       intent.launchApplicationId);
        break;
    case ui::UiIntentType::OpenSettings:
        diagPrint("[app] intent: OpenSettings");
        openSettingsOverlay();
        break;
    case ui::UiIntentType::CloseSettings:
        diagPrint("[app] intent: CloseSettings");
        closeSettingsOverlay(intent.settings);
        break;
    case ui::UiIntentType::RequestDeleteHost:
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[app] intent: RequestDeleteHost hostId=%llu",
                     static_cast<unsigned long long>(intent.hostId));
            diagPrint(buf);
            handleDeleteHost(intent.hostId);
        }
        break;
    case ui::UiIntentType::RequestAddHost:
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[app] intent: RequestAddHost ipv4=%s port=%u",
                     intent.ipv4.c_str(), static_cast<unsigned>(intent.port));
            diagPrint(buf);
            handleAddHost(intent.ipv4, intent.port);
        }
        break;
    case ui::UiIntentType::CancelAddHost:
        diagPrint("[app] intent: CancelAddHost");
        handleCancelAddHost();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Pairing update handling
// ---------------------------------------------------------------------------

/// Handle a pairing update emitted by PairingManager.
void MoonlightApp::handlePairingUpdate(const ml::PairingUpdate& update) {
    // Build the UI view state.
    ui::PairingViewState view;
    view.hostId = update.hostId;
    view.statusText = update.statusText;

    // Populate the host name from the authoritative HostManager registry so
    // the Pairing screen can display which host is being paired.
    if (m_hostManager) {
        const ml::Host* host = m_hostManager->findHost(update.hostId);
        if (host != nullptr) {
            view.hostName = host->displayName;
        }
    }

    switch (update.type) {
    case ml::PairingUpdateType::PinReady:
        diagPrint("[app] pairing: PIN ready");
        view.pin = update.pin;
        view.state = ui::PairingPresentationState::WaitingForUser;
        break;
    case ml::PairingUpdateType::Progress:
        diagPrint("[app] pairing: progress");
        view.state = ui::PairingPresentationState::Pairing;
        break;
    case ml::PairingUpdateType::Succeeded:
        diagPrint("[app] pairing: succeeded — scheduling fresh serverinfo");
        // Pairing succeeded and trust persisted. Do NOT mark Paired yet.
        // Mark pairing state Unknown and schedule a fresh serverinfo query.
        m_hostManager->setPairingState(update.hostId, ml::HostPairingState::Unknown);
        // Record the freshly paired host so its App List opens automatically
        // once post-pairing reconciliation confirms it is Paired. This uses
        // the exact host that was paired (not the Host List selection) and is
        // cleared when reconciliation produces a definitive result.
        m_pendingPairingAutoOpenHostId = update.hostId;
        scheduleServerInfoQuery(update.hostId);
        // Stay on the Pairing screen (Succeeded) until the App List loads.
        // The App List transition is driven by setApplicationListView after
        // reconciliation confirms the host is Paired.
        view.state = ui::PairingPresentationState::Succeeded;
        break;
    case ml::PairingUpdateType::Failed:
        diagPrint("[app] pairing: failed");
        view.state = ui::PairingPresentationState::Failed;
        break;
    case ml::PairingUpdateType::Cancelled:
        diagPrint("[app] pairing: cancelled");
        view.state = ui::PairingPresentationState::Cancelled;
        break;
    default:
        break;
    }

    // Update the UI with the pairing view state.
    if (m_ui && m_ui->isInitialized()) {
        m_ui->setPairingView(view);
    }
}

// ---------------------------------------------------------------------------
// Delete Host
// ---------------------------------------------------------------------------

/// Handle a RequestDeleteHost intent.
///
/// Deletion is initiated only from the Host List, so there is no active stream
/// for the host. The persisted local trust/pairing record is removed first;
/// only after that succeeds is the host removed from the HostManager registry
/// (so it disappears immediately from the Host List). On persistence failure
/// the host is left in place and a diagnostic is logged.
///
/// Ordering keeps runtime and disk state consistent: persist deletion
/// successfully -> remove from HostManager -> refresh Host List (the Host List
/// snapshot is refreshed by the normal update() setHosts() call).
void MoonlightApp::handleDeleteHost(ml::HostId hostId) {
    if (!m_hostManager || !m_pairingManager) {
        diagPrint("[host] delete failed: managers unavailable");
        return;
    }

    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[host] delete requested: id=%llu (unknown host, ignored)",
                 static_cast<unsigned long long>(hostId));
        diagPrint(buf);
        return;
    }

    char buf[192];
    snprintf(buf, sizeof(buf),
             "[host] delete requested: id=%llu name='%s'",
             static_cast<unsigned long long>(hostId),
             host->displayName.c_str());
    diagPrint(buf);

    // --- 1. Persist the removal of local trust/pairing state. ---------------
    // Only remove the trusted-host entry for this host's serverId. The client
    // global identity, certificate, and other hosts' pairing records are
    // untouched. If the host has no serverId (never paired), there is nothing
    // to remove; treat that as success.
    bool persistOk = true;
    if (!host->serverId.empty()) {
        persistOk = m_pairingManager->removeLocalTrustForServer(host->serverId);
    }

    if (!persistOk) {
        char buf2[128];
        snprintf(buf2, sizeof(buf2),
                 "[host] delete failed: id=%llu (persistence failure)",
                 static_cast<unsigned long long>(hostId));
        diagPrint(buf2);
        // Keep the host present; do not remove the runtime host and leave
        // persisted trust behind.
        return;
    }

    // --- 2. Remove the host from the HostManager registry. ------------------
    m_hostManager->removeHost(hostId);

    // --- 2b. Remove the host's persistent artwork cache. --------------------
    // The host is being deleted/unpaired, so its cached artwork is no longer
    // relevant. This is a natural integration point for AppArtworkManager's
    // removeHostCache. Runtime decoded artwork for the removed host is cleared
    // below.
    if (m_appArtworkManager && !host->serverId.empty()) {
        m_appArtworkManager->removeHostCache(host->serverId);
    }

    // --- 3. Invalidate any host-owned cached state. -------------------------
    // If the deleted host was the current App List catalog host, clear the
    // catalog state so no stale host-owned data lingers. Deletion is only
    // initiated from the Host List, so this is defensive but keeps runtime
    // state consistent.
    if (m_catalogHostId == hostId) {
        if (m_applicationCatalog) {
            m_applicationCatalog->cancel();
        }
        m_catalogHostId = 0;
        m_catalogGeneration = 0;
        m_catalogApplications.clear();
        m_catalogState = ml::ApplicationCatalogState::Idle;
        // Clear runtime artwork and scheduling state for the removed host.
        clearRuntimeArtwork();
        if (m_ui && m_ui->isInitialized()) {
            ui::ApplicationListView view;
            view.state = ml::ApplicationCatalogState::Idle;
            m_ui->setApplicationListView(view);
        }
    }

    if (m_pendingPairingAutoOpenHostId == hostId) {
        m_pendingPairingAutoOpenHostId = 0;
    }

    // Clear any stale serverinfo generation tracking for the removed host.
    for (auto it = m_serverInfoGenerations.begin();
         it != m_serverInfoGenerations.end();) {
        if (it->first == hostId) {
            it = m_serverInfoGenerations.erase(it);
        } else {
            ++it;
        }
    }

    // Clear any in-flight probe tracking for the removed host.
    for (auto it = m_probeInFlightHosts.begin();
         it != m_probeInFlightHosts.end();) {
        if (*it == hostId) {
            it = m_probeInFlightHosts.erase(it);
        } else {
            ++it;
        }
    }

    // --- 4. Refresh the Host List. ------------------------------------------
    // The Host List snapshot is refreshed by the normal update() setHosts()
    // call on the next frame. Force the UI host snapshot to refresh so the
    // removed host disappears immediately.
    m_lastUiHostsValid = false;
    updateUiHostSnapshot();

    diagPrint("[host] delete complete — host removed from registry");
}

// ---------------------------------------------------------------------------
// Pairing-state reconciliation
// ---------------------------------------------------------------------------

/// Reconcile a host's application-visible pairing state using local trust
/// (PairingManager/PairingStore) and the server-reported pairing status.
///
/// Final state comes from:
///   local trust exists + serverReportsPaired + serverinfo valid -> Paired
///   local=no, server=no -> Unpaired
///   local=no, server=yes -> Unknown
///   local=yes, server=no -> Unpaired (reconciliation does not bypass)
void MoonlightApp::reconcileHostPairingState(ml::HostId hostId) {
    if (!m_hostManager || !m_pairingManager) {
        return;
    }

    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        return;
    }

    // If serverinfo is not valid, we cannot reconcile.
    if (host->serverInfoState != ml::ServerInfoQueryState::Valid) {
        m_hostManager->setPairingState(hostId, ml::HostPairingState::Unknown);
        return;
    }

    const bool localTrust = m_pairingManager->hasLocalTrustForServer(host->serverId);
    const bool serverPaired = host->serverReportsPaired;

    ml::HostPairingState state;
    if (localTrust && serverPaired) {
        state = ml::HostPairingState::Paired;
    } else if (!serverPaired) {
        state = ml::HostPairingState::Unpaired;
    } else {
        // serverPaired && !localTrust
        state = ml::HostPairingState::Unknown;
    }

    m_hostManager->setPairingState(hostId, state);

    // A manually-added host that is now confirmed Paired becomes a normal
    // Online host (it no longer needs the special ManuallyAdded status that
    // allowed it to enter pairing without mDNS discovery).
    if (state == ml::HostPairingState::Paired &&
        host->availability == ml::HostAvailability::ManuallyAdded) {
        m_hostManager->setAvailability(hostId, ml::HostAvailability::Online);
    }

    // Per-host pairing-state reconciliation line. Guarded by a compile-time
    // constant (default OFF) to keep the klog quiet during normal operation.
    if (kLogReconciledPairing) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] reconciled host id=%llu pairing state (local=%d server=%d)",
                 static_cast<unsigned long long>(hostId),
                 localTrust ? 1 : 0, serverPaired ? 1 : 0);
        diagPrint(buf);
    }

    // --- Post-pairing auto-open --------------------------------------------
    // If this host was just successfully paired, act on the reconciliation
    // result. Only the exact host that was paired (m_pendingPairingAutoOpenHostId)
    // triggers this; a normal Host List activation of an already-paired host
    // is unaffected.
    if (m_pendingPairingAutoOpenHostId == hostId) {
        if (state == ml::HostPairingState::Paired) {
            // Reconciliation confirms the host is usable/paired over
            // authenticated HTTPS. Open the App List for the freshly paired
            // host.
            m_pendingPairingAutoOpenHostId = 0;
            diagPrint("[app] post-pairing reconciliation confirmed Paired — opening App List");
            startApplicationCatalog(hostId);
        } else if (state == ml::HostPairingState::Unpaired) {
            // Reconciliation did not confirm the host is paired. Abandon the
            // auto-open and return to the Host List (preserving the existing
            // pairing failure/error behavior).
            m_pendingPairingAutoOpenHostId = 0;
            diagPrint("[app] post-pairing reconciliation did not confirm Paired — returning to Host List");
            if (m_ui && m_ui->isInitialized()) {
                ui::ApplicationListView view;
                view.state = ml::ApplicationCatalogState::Idle;
                m_ui->setApplicationListView(view);
            }
        }
        // Unknown: keep waiting for a definitive reconciliation result.
    }
}

// ---------------------------------------------------------------------------
// ServerInfo scheduling
// ---------------------------------------------------------------------------

bool MoonlightApp::buildServerInfoTarget(ml::HostId hostId,
                                         ml::ServerInfoTarget& outTarget) {
    if (!m_hostManager || !m_pairingManager) {
        return false;
    }

    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        return false;
    }

    // Build the ServerInfoTarget from the current Host record.
    ml::ServerInfoTarget target;
    target.hostId = host->id;
    target.ipv4Addresses = host->ipv4Addresses;
    target.discoveryPort = host->port;
    target.httpsPort = host->httpsPort;
    // Use the persistent client unique ID from PairingManager.
    target.clientUniqueId = m_pairingManager->clientUniqueId();

    // If the host has local trust (is paired), query serverinfo over HTTPS
    // with the client certificate so the server reports the correct PairStatus.
    // Sunshine only reports paired=1 when the client authenticates via HTTPS
    // with its certificate; a plain-HTTP query returns paired=0 even for a
    // paired client.
    if (m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        target.useTls = true;
        target.clientCertPem = m_pairingManager->clientCertPem();
        target.clientKeyPem = m_pairingManager->clientKeyPem();
    }

    outTarget = std::move(target);
    return true;
}

void MoonlightApp::scheduleServerInfoQuery(ml::HostId hostId) {
    if (!m_serverInfoManager || !m_hostManager) {
        return;
    }

    ml::ServerInfoTarget target;
    if (!buildServerInfoTarget(hostId, target)) {
        return;
    }

    // Diagnostic: log the unique ID used for the serverinfo query so we can
    // verify it matches the unique ID used during pairing.
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[app] serverinfo query host id=%llu uniqueid='%s' port=%u tls=%d",
                 static_cast<unsigned long long>(hostId),
                 target.clientUniqueId.c_str(),
                 static_cast<unsigned>(target.useTls ? target.httpsPort : target.discoveryPort),
                 target.useTls ? 1 : 0);
        diagPrint(buf);
    }

    // Mark the host as Pending before scheduling.
    m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Pending);

    ml::ServerInfoResult result = m_serverInfoManager->request(hostId, target);
    if (!result.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] serverinfo request failed for host id=%llu (category %d)",
                 static_cast<unsigned long long>(hostId),
                 static_cast<int>(result.category));
        diagPrint(buf);
        // If the request could not be scheduled, mark the host as Failed.
        if (result.category == ml::ServerInfoResultCategory::QueueFull ||
            result.category == ml::ServerInfoResultCategory::InvalidState) {
            m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Failed);
        }
    }
}

// ---------------------------------------------------------------------------
// Application Catalog / App List
// ---------------------------------------------------------------------------

/// Request an authenticated serverinfo refresh for the current App List host
/// without the per-request diagnostic log (used by the polling cadence).
/// Reuses buildServerInfoTarget and ServerInfoManager duplicate suppression.
void MoonlightApp::requestAppListServerInfo(ml::HostId hostId) {
    if (!m_serverInfoManager || !m_hostManager) {
        return;
    }

    ml::ServerInfoTarget target;
    if (!buildServerInfoTarget(hostId, target)) {
        return;
    }

    // Mark the host as Pending before scheduling (matches existing behavior).
    m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Pending);

    ml::ServerInfoResult result = m_serverInfoManager->request(hostId, target);
    if (!result.succeeded()) {
        // Transient failure: do not leave App List, show a modal, or mark the
        // catalog invalid. The next poll interval retries. AlreadyPending is a
        // normal skip (a request is already queued/active). Errors use the
        // existing ServerInfoManager logging.
        if (result.category == ml::ServerInfoResultCategory::QueueFull ||
            result.category == ml::ServerInfoResultCategory::InvalidState) {
            m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Failed);
        }
    }
}

/// Update the App List serverinfo polling cadence. Called once per frame from
/// update(). While the App List screen is active for a host, requests an
/// authenticated serverinfo refresh immediately on entry and then
/// approximately once per second, so externally-triggered launch/quit/switch
/// changes are reflected. No-op when not on the App List.
void MoonlightApp::updateAppListPolling() {
    // The App List is active only while a catalog host is selected.
    // m_catalogHostId is the authoritative current App List host (0 = not on
    // the App List).
    const bool appListActive = (m_catalogHostId != 0);
    const ml::HostId currentHost = appListActive ? m_catalogHostId : 0;

    // Detect entry, exit, and host change. Reset the poll deadline on any
    // transition so a new App List entry always performs an immediate refresh.
    if (currentHost != m_appListPollHostId) {
        if (currentHost == 0) {
            // Leaving App List (or a stream became active) — stop polling.
            if (m_appListPollHostId != 0) {
                diagPrint("[app-list] serverinfo polling stopped");
            }
            m_appListPollHostId = 0;
            m_appListNextPollUs = 0;
            return;
        }
        // Entering App List or switching host — reset and refresh immediately.
        if (m_appListPollHostId == 0) {
            diagPrint("[app-list] serverinfo polling started");
        }
        m_appListPollHostId = currentHost;
        m_appListNextPollUs = 0;  // forces an immediate refresh below
    }

    if (m_appListPollHostId == 0) {
        return;
    }

    // Deadline comparison (not accumulating +1000ms) so a delayed main loop
    // does not queue a backlog of requests. At most one meaningful serverinfo
    // refresh is outstanding per host (ServerInfoManager suppresses duplicates).
    const uint64_t nowUs = m_currTimestamp;
    if (nowUs >= m_appListNextPollUs) {
        requestAppListServerInfo(m_appListPollHostId);
        m_appListNextPollUs = nowUs + kAppListPollIntervalUs;
    }
}

/// Start an App List retrieval for a Paired host.
/// Validates preconditions, builds the ApplicationCatalogTarget, and
/// enters the App List loading state.
void MoonlightApp::startApplicationCatalog(ml::HostId hostId) {
    if (!m_applicationCatalog || !m_hostManager || !m_pairingManager) {
        return;
    }

    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        return;
    }

    // --- Validate preconditions --------------------------------------------
    if (host->availability != ml::HostAvailability::Online) {
        diagPrint("[app] applist: host not Online (rejected)");
        return;
    }
    if (host->serverInfoState != ml::ServerInfoQueryState::Valid) {
        diagPrint("[app] applist: serverinfo not Valid (rejected)");
        return;
    }
    if (host->pairingState != ml::HostPairingState::Paired) {
        diagPrint("[app] applist: host not Paired (rejected)");
        return;
    }
    if (host->serverId.empty() || host->ipv4Addresses.empty() ||
        host->httpsPort == 0) {
        diagPrint("[app] applist: invalid host metadata (rejected)");
        return;
    }

    // --- Validate local trust and obtain credentials -----------------------
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        diagPrint("[app] applist: host claims Paired but no local trust (reconciling)");
        reconcileHostPairingState(hostId);
        return;
    }

    // --- Build the ApplicationCatalogTarget --------------------------------
    ml::ApplicationCatalogTarget target;
    target.hostId = host->id;
    target.serverId = host->serverId;
    target.ipv4Addresses = host->ipv4Addresses;
    target.httpsPort = host->httpsPort;
    target.clientUniqueId = m_pairingManager->clientUniqueId();
    target.clientCertPem = m_pairingManager->clientCertPem();
    target.clientKeyPem = m_pairingManager->clientKeyPem();
    target.currentGame = host->currentGame;

    // --- Request the catalog ------------------------------------------------
    ml::ApplicationCatalogResult result = m_applicationCatalog->request(target);
    if (!result.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] applist request failed for host id=%llu (category %d)",
                 static_cast<unsigned long long>(hostId),
                 static_cast<int>(result.category));
        diagPrint(buf);
        return;
    }

    // --- Track the current catalog request ---------------------------------
    // If the active host/catalog changes, clear runtime artwork and scheduling
    // state belonging to the old context. The persistent disk cache is
    // untouched; the new catalog's artwork is resolved when it becomes Ready.
    if (m_catalogHostId != hostId) {
        clearRuntimeArtwork();
    }
    m_catalogHostId = hostId;
    m_catalogGeneration = target.generation;
    m_catalogState = ml::ApplicationCatalogState::Loading;
    m_catalogApplications.clear();

    // --- Enter the App List loading state in the UI ------------------------
    if (m_ui && m_ui->isInitialized()) {
        ui::ApplicationListView view;
        view.hostId = hostId;
        view.hostName = host->displayName;
        view.state = ml::ApplicationCatalogState::Loading;
        view.statusText = "Loading applications...";
        m_ui->setApplicationListView(view);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[app] applist requested for host id=%llu (gen=%u)",
             static_cast<unsigned long long>(hostId),
             static_cast<unsigned>(target.generation));
    diagPrint(buf);
}

/// Handle an ApplicationCatalog update emitted by ApplicationCatalog.
void MoonlightApp::handleApplicationCatalogUpdate(
    const ml::ApplicationCatalogUpdate& update) {
    // --- Stale-result filtering --------------------------------------------
    // If this update is for a different host or an older generation, ignore it.
    if (update.hostId != m_catalogHostId) {
        diagPrint("[app] applist: stale result for different host (ignored)");
        return;
    }
    if (update.generation < m_catalogGeneration) {
        diagPrint("[app] applist: stale result for older generation (ignored)");
        return;
    }

    // --- Apply the result ---------------------------------------------------
    if (update.valid) {
        m_catalogApplications = update.applications;
        m_catalogState = ml::ApplicationCatalogState::Ready;
        // Begin artwork resolution for the freshly loaded catalog. This clears
        // runtime artwork from the old context, resets scheduling state, and
        // starts scheduling artwork requests (bounded by queue capacity).
        beginCatalogArtwork();
    } else {
        m_catalogApplications.clear();
        m_catalogState = ml::ApplicationCatalogState::Failed;
    }

    // --- Update the UI ------------------------------------------------------
    if (m_ui && m_ui->isInitialized()) {
        ui::ApplicationListView view;
        view.hostId = m_catalogHostId;
        view.state = m_catalogState;
        view.applications = m_catalogApplications;

        // Populate the host name from the authoritative HostManager registry.
        if (m_hostManager) {
            const ml::Host* host = m_hostManager->findHost(m_catalogHostId);
            if (host != nullptr) {
                view.hostName = host->displayName;
            }
        }

        if (m_catalogState == ml::ApplicationCatalogState::Failed) {
            view.statusText = "Unable to load applications";
        }

        m_ui->setApplicationListView(view);
    }

    // --- Log the result -----------------------------------------------------
    if (update.valid) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] applist loaded %zu applications for host id=%llu",
                 update.applications.size(),
                 static_cast<unsigned long long>(update.hostId));
        diagPrint(buf);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] applist failed for host id=%llu (category %d)",
                 static_cast<unsigned long long>(update.hostId),
                 static_cast<int>(update.result.category));
        diagPrint(buf);
    }
}

/// Rebuild the App List presentation view from the current catalog and the
/// authoritative Host.currentGame running state. Pushes the updated view to
/// UiManager. No network activity.
void MoonlightApp::refreshAppListView() {
    if (!m_ui || !m_ui->isInitialized()) {
        return;
    }

    // Build a fresh copy of the catalog with running flags recomputed from the
    // authoritative Host.currentGame (the host is the source of truth; the
    // catalog's cached running flags are presentation data only).
    std::vector<ml::ApplicationInfo> apps = m_catalogApplications;
    uint32_t currentGame = 0;
    if (m_hostManager) {
        const ml::Host* host = m_hostManager->findHost(m_catalogHostId);
        if (host != nullptr) {
            currentGame = host->currentGame;
        }
    }
    for (auto& app : apps) {
        // Presentation priority:
        //   - If this app is the active local connecting target, show
        //     Connecting (overrides Running even if Sunshine has already
        //     updated Host.currentGame == app.id before the stream connects).
        //   - Else if Host.currentGame == app.id, show Running.
        //   - Else normal/not-running presentation.
        app.connecting = (m_connectingHostId == m_catalogHostId &&
                          m_connectingAppId == app.id);
        app.running = (app.id == currentGame);
        // While a quit-and-launch is waiting for the host to free up, the
        // about-to-be-launched app shows "Waiting..." on its card.
        app.waitingToLaunch =
            m_waitingForQuitToComplete &&
            m_pendingLaunchAfterQuitHostId == m_catalogHostId &&
            m_pendingLaunchAfterQuitAppId == app.id;
    }

    ui::ApplicationListView view;
    view.hostId = m_catalogHostId;
    view.state = m_catalogState;
    view.applications = std::move(apps);
    if (m_hostManager) {
        const ml::Host* host = m_hostManager->findHost(m_catalogHostId);
        if (host != nullptr) {
            view.hostName = host->displayName;
        }
    }
    m_ui->setApplicationListView(view);
}

// ---------------------------------------------------------------------------
// App Artwork resolution
// ---------------------------------------------------------------------------

/// Begin artwork resolution for the current catalog. Clears runtime artwork
/// belonging to the old context, resets scheduling state, and starts
/// scheduling artwork requests for the new catalog's apps. Called when a
/// catalog becomes Ready.
void MoonlightApp::beginCatalogArtwork() {
    if (!m_appArtworkManager || !m_hostManager || !m_pairingManager) {
        return;
    }
    if (m_catalogHostId == 0 || m_catalogApplications.empty()) {
        return;
    }

    const ml::Host* host = m_hostManager->findHost(m_catalogHostId);
    if (host == nullptr) {
        return;
    }
    if (host->serverId.empty() || host->ipv4Addresses.empty() ||
        host->httpsPort == 0) {
        return;
    }
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        return;
    }

    // Clear runtime artwork from the old context and reset scheduling state.
    // The persistent disk cache is untouched.
    m_runtimeArtwork.clear();
    m_artworkNextScheduleIndex = 0;
    m_artworkSchedulingActive = true;
    m_artworkServerId = host->serverId;
    ++m_artworkGeneration;

    // Schedule the first bounded batch immediately.
    scheduleArtworkBatch();

    char buf[192];
    snprintf(buf, sizeof(buf),
             "[Artwork] catalog generation=%llu apps=%zu",
             static_cast<unsigned long long>(m_artworkGeneration),
             m_catalogApplications.size());
    diagPrint(buf);
}

/// Schedule a bounded batch of artwork requests for the current catalog.
/// Called once per frame from update() while scheduling is active. Stops when
/// the request queue reports QueueFull (resumes next frame) or when all apps
/// have been scheduled. No per-frame duplicate spam: the manager suppresses
/// AlreadyPending.
void MoonlightApp::scheduleArtworkBatch() {
    if (!m_artworkSchedulingActive) {
        return;
    }
    if (!m_appArtworkManager || !m_hostManager || !m_pairingManager) {
        return;
    }
    if (m_catalogHostId == 0 || m_catalogApplications.empty()) {
        m_artworkSchedulingActive = false;
        return;
    }

    const ml::Host* host = m_hostManager->findHost(m_catalogHostId);
    if (host == nullptr) {
        m_artworkSchedulingActive = false;
        return;
    }
    if (host->serverId.empty() || host->ipv4Addresses.empty() ||
        host->httpsPort == 0) {
        m_artworkSchedulingActive = false;
        return;
    }
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        m_artworkSchedulingActive = false;
        return;
    }

    // Build the shared artwork target fields from the host + pairing manager.
    ml::AppArtworkTarget base;
    base.hostId = host->id;
    base.serverId = host->serverId;
    base.ipv4Addresses = host->ipv4Addresses;
    base.httpsPort = host->httpsPort;
    base.clientUniqueId = m_pairingManager->clientUniqueId();
    base.clientCertPem = m_pairingManager->clientCertPem();
    base.clientKeyPem = m_pairingManager->clientKeyPem();

    // Schedule a bounded batch starting from the next unscheduled index. Stop
    // when the request queue reports QueueFull (resume next frame) or when all
    // apps have been scheduled. AlreadyPending is a normal skip (the manager
    // already has a request for that identity).
    size_t scheduled = 0;
    while (m_artworkNextScheduleIndex < m_catalogApplications.size()) {
        const ml::ApplicationInfo& app =
            m_catalogApplications[m_artworkNextScheduleIndex];

        ml::AppArtworkTarget target = base;
        target.appId = app.id;
        target.generation = m_artworkGeneration;

        ml::AppArtworkResultCategory rc =
            m_appArtworkManager->requestArtwork(target);
        if (rc == ml::AppArtworkResultCategory::QueueFull) {
            // Queue pressure: stop this batch; resume on a later frame.
            break;
        }
        if (rc == ml::AppArtworkResultCategory::Success) {
            ++scheduled;
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[Artwork] scheduled app=%u",
                     static_cast<unsigned>(app.id));
            diagPrint(buf);
        }
        // AlreadyPending / InvalidTarget / InvalidState: skip this app and
        // continue to the next (no permanent skip; a later cycle retries).
        ++m_artworkNextScheduleIndex;
    }

    // If we reached the end of the catalog, scheduling is complete.
    if (m_artworkNextScheduleIndex >= m_catalogApplications.size()) {
        m_artworkSchedulingActive = false;
    }
}

/// Process a completed artwork result on the main thread. Verifies the result
/// belongs to the current host/catalog context, rejects stale generation
/// results, verifies the app still exists in the active catalog, and on
/// success moves the decoded image into runtime artwork state. Returns true if
/// the affected card should be invalidated. Takes a non-const reference so the
/// decoded image can be moved out.
bool MoonlightApp::handleArtworkResult(ml::AppArtworkResult& result) {
    // --- Stale-result filtering --------------------------------------------
    // Reject results that do not belong to the current host/catalog context.
    if (result.hostId != m_catalogHostId) {
        diagPrint("[Artwork] stale result discarded (different host)");
        return false;
    }
    if (result.generation != m_artworkGeneration) {
        diagPrint("[Artwork] stale result discarded (generation)");
        return false;
    }
    if (m_artworkServerId.empty()) {
        diagPrint("[Artwork] stale result discarded (no active catalog)");
        return false;
    }

    // --- Failure handling ---------------------------------------------------
    // Artwork failure is non-fatal: retain fallback artwork and continue.
    if (result.category != ml::AppArtworkResultCategory::Success) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[Artwork] resolved app=%u failed category=%d (fallback retained)",
                 static_cast<unsigned>(result.appId),
                 static_cast<int>(result.category));
        diagPrint(buf);
        return false;
    }

    // --- Verify the app still exists in the active catalog ------------------
    bool appExists = false;
    for (const auto& app : m_catalogApplications) {
        if (app.id == result.appId) {
            appExists = true;
            break;
        }
    }
    if (!appExists) {
        diagPrint("[Artwork] result for removed app discarded");
        return false;
    }

    // --- Store/move the decoded image into runtime artwork state -----------
    // Move the image (no copy) into the runtime map. If an image already
    // exists for this app (e.g., a re-fetch), the old buffer is released.
    m_runtimeArtwork[result.appId] = std::move(result.image);

    {
        // Concise heap/memory diagnostic: report the resolved image and the
        // current retained artwork count + approximate bytes. This is
        // low-volume (one line per resolved image, not per frame) and provides
        // evidence for whether the 128 MiB heap is appropriate.
        const auto& img = m_runtimeArtwork[result.appId];
        uint64_t totalBytes = 0;
        for (const auto& kv : m_runtimeArtwork) {
            totalBytes += static_cast<uint64_t>(kv.second.rgbaSize);
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[Artwork] resolved app=%u source=%s %ux%u "
                 "retained images=%zu bytes=%llu",
                 static_cast<unsigned>(result.appId),
                 result.fromCache ? "cache" : "network",
                 static_cast<unsigned>(img.width),
                 static_cast<unsigned>(img.height),
                 m_runtimeArtwork.size(),
                 static_cast<unsigned long long>(totalBytes));
        diagPrint(buf);
    }

    // The affected card's artwork changed; it should be invalidated.
    return true;
}

/// Clear runtime artwork and reset scheduling state for the current
/// host/catalog context. The persistent disk cache is untouched.
void MoonlightApp::clearRuntimeArtwork() {
    m_runtimeArtwork.clear();
    m_artworkSchedulingActive = false;
    m_artworkNextScheduleIndex = 0;
    m_artworkServerId.clear();
}

/// Populate the resolved runtime artwork pointers into an App List render
/// state. Null means fall back to app_fallback.png. Used by both the initial
/// seed (full compose) and the per-frame render path so the seeded surface and
/// subsequent recomposes always use the same artwork.
void MoonlightApp::populateAppListArtwork(ui::AppListRenderState& state) {
    for (auto& card : state.cards) {
        auto it = m_runtimeArtwork.find(card.id);
        if (it != m_runtimeArtwork.end()) {
            card.artwork = &it->second;
        }
    }
}

/// Populate the resolved runtime artwork pointers into an App Grid render
/// state. Null means fall back to app_fallback.png. Used by both the initial
/// seed (full compose) and the per-frame render path so the seeded surface and
/// subsequent recomposes always use the same artwork.
void MoonlightApp::populateAppGridArtwork(ui::AppGridRenderState& state) {
    for (auto& card : state.cards) {
        auto it = m_runtimeArtwork.find(card.id);
        if (it != m_runtimeArtwork.end()) {
            card.artwork = &it->second;
        }
    }
}

/// Clear the transient local Connecting state (if any).
/// Called when the launch/connect sequence fails or is cancelled. Does not
/// touch Host.currentGame (which remains authoritative for Sunshine's actual
/// running application).
void MoonlightApp::clearConnectingState() {
    m_connectingHostId = 0;
    m_connectingAppId = 0;
    m_connectingGeneration = 0;
}

/// Handle an ActivateApplication intent.
/// Validates preconditions, builds the ApplicationLaunchTarget, and starts
/// the launch/resume operation.
void MoonlightApp::handleActivateApplication(ml::HostId hostId,
                                            ml::ApplicationId appId) {
    if (!m_applicationSessionManager || !m_hostManager || !m_pairingManager) {
        diagPrint("[app] ActivateApplication: launch manager unavailable (ignored)");
        return;
    }

    // Resolve the application from the current catalog.
    const ml::ApplicationInfo* app = nullptr;
    for (const auto& a : m_catalogApplications) {
        if (a.id == appId) {
            app = &a;
            break;
        }
    }

    if (app == nullptr) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] ActivateApplication: unknown ApplicationId %u (ignored)",
                 static_cast<unsigned>(appId));
        diagPrint(buf);
        return;
    }

    // Resolve the host.
    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        diagPrint("[app] ActivateApplication: unknown HostId (ignored)");
        return;
    }

    // --- Validate preconditions --------------------------------------------
    if (host->availability != ml::HostAvailability::Online) {
        diagPrint("[app] launch: host not Online (rejected)");
        return;
    }
    if (host->serverInfoState != ml::ServerInfoQueryState::Valid) {
        diagPrint("[app] launch: serverinfo not Valid (rejected)");
        return;
    }
    if (host->pairingState != ml::HostPairingState::Paired) {
        diagPrint("[app] launch: host not Paired (rejected)");
        return;
    }
    if (host->serverId.empty() || host->ipv4Addresses.empty() ||
        host->httpsPort == 0) {
        diagPrint("[app] launch: invalid host metadata (rejected)");
        return;
    }

    // --- Block launching a different app while another app is running -------
    // Authoritative guard using Host.currentGame (server-reported running app
    // ID) and the selected application's real ApplicationId. This is a
    // client-side launch guard only; it never quits, launches, or resumes.
    //
    //   currentGame == 0            → no app running → normal launch path.
    //   currentGame == appId        → selected app already running → existing
    //                                 resume path (no modal).
    //   currentGame != 0 && != appId → a different app is running → block the
    //                                 launch and show the informational modal.
    //
    // The guard is skipped while a launch/resume is already active so it never
    // interferes with an already-started launch (the existing AlreadyLaunching
    // rejection path handles that case).
    if (host->currentGame != 0 && host->currentGame != appId &&
        !(m_applicationSessionManager && m_applicationSessionManager->isLaunching()) &&
        !m_allowLaunchAfterQuit) {
        diagPrint("[app] launch blocked: a different app is already running on this host");
        if (m_ui && m_ui->isInitialized()) {
            m_ui->openQuitAndLaunchConfirm(hostId, host->currentGame, appId);
        }
        return;
    }

    // --- Validate local trust and obtain credentials -----------------------
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        diagPrint("[app] launch: host claims Paired but no local trust (reconciling)");
        reconcileHostPairingState(hostId);
        return;
    }

    // --- Shared HttpClient serialization -----------------------------------
    // Do not start a launch while the catalog is loading, pairing is active,
    // or a required serverinfo query is pending for this host.
    if (m_applicationCatalog && m_applicationCatalog->isLoading()) {
        diagPrint("[app] launch: app catalog still loading (rejected)");
        return;
    }
    if (m_pairingManager->isPairing()) {
        diagPrint("[app] launch: pairing active (rejected)");
        return;
    }
    if (m_serverInfoManager && m_serverInfoManager->isPending(hostId)) {
        diagPrint("[app] launch: serverinfo pending for host (rejected)");
        return;
    }

    // --- Build the ApplicationLaunchTarget ---------------------------------
    ml::ApplicationLaunchTarget target;
    target.hostId = host->id;
    target.applicationId = appId;
    target.applicationName = app->name;
    target.serverId = host->serverId;
    target.ipv4Addresses = host->ipv4Addresses;
    target.httpsPort = host->httpsPort;
    // When launching after a quit-and-launch, the host's currentGame is still
    // the just-quit app (stale until the serverinfo refresh lands). Report 0
    // so the launch worker performs a fresh /launch instead of rejecting the
    // target as HostBusy (a different app appearing to still be running).
    target.currentGame = m_allowLaunchAfterQuit ? 0 : host->currentGame;
    target.serverVersion = host->serverVersion;
    target.serverCodecModeSupport = host->serverCodecModeSupport;
    target.clientUniqueId = m_pairingManager->clientUniqueId();
    target.clientCertPem = m_pairingManager->clientCertPem();
    target.clientKeyPem = m_pairingManager->clientKeyPem();

    // --- Apply the persistent stream settings ------------------------------
    // Resolution: 1080p → 1920×1080, 720p → 1280×720.
    // Frame rate: 60 FPS → 60, 30 FPS → 30.
    // Host speakers: Muted → muteHostSpeakers=true (localAudioPlayMode=0).
    // Game Optimization: ON → optimizeGameSettings=true (sops=1).
    if (m_settingsManager) {
        const ml::Settings& s = m_settingsManager->settings();
        target.width = (s.resolution == ml::StreamResolution::R720p) ? 1280 : 1920;
        target.height = (s.resolution == ml::StreamResolution::R720p) ? 720 : 1080;
        target.fps = (s.frameRate == ml::StreamFrameRate::Fps30) ? 30 : 60;
        target.muteHostSpeakers = s.muteHostSpeakers;
        target.optimizeGameSettings = s.optimizeGameSettings;
    }

    // --- Request the launch ------------------------------------------------
    ml::ApplicationLaunchResult result = m_applicationSessionManager->request(target);
    if (!result.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] launch request failed for host id=%llu app id=%u (category %d)",
                 static_cast<unsigned long long>(hostId),
                 static_cast<unsigned>(appId),
                 static_cast<int>(result.category));
        diagPrint(buf);
        return;
    }

    // --- Track the current launch request ----------------------------------
    m_launchHostId = hostId;
    m_launchAppId = appId;
    m_launchGeneration = target.generation;
    m_launchActive = true;

    // --- Enter the transient local Connecting state ------------------------
    // The launch/resume request was genuinely accepted, so this client is now
    // connecting to this host/app. This is a client-local transient state that
    // overrides the Host.currentGame-derived Running presentation until the
    // launch/connect sequence fails/cancels.
    m_connectingHostId = hostId;
    m_connectingAppId = appId;
    m_connectingGeneration = target.generation;
    diagPrint("[app] connecting: host=<id> app=<id>");

    // Refresh the App List presentation so the connecting card immediately
    // shows "Connecting" (overriding any Host.currentGame-derived Running
    // state until the stream is established).
    refreshAppListView();

    // --- Enter the launch-progress state in the UI -------------------------
    if (m_ui && m_ui->isInitialized()) {
        ui::ApplicationLaunchViewState view;
        view.active = true;
        view.applicationId = appId;
        view.state = (host->currentGame == appId)
                         ? ui::LaunchPresentationState::Resuming
                         : ui::LaunchPresentationState::Launching;
        view.statusText = (host->currentGame == appId)
                              ? "Resuming " + app->name + "..."
                              : "Launching " + app->name + "...";
        m_ui->setApplicationLaunchView(view);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[app] launch requested for host id=%llu app id=%u (gen=%u)",
             static_cast<unsigned long long>(hostId),
             static_cast<unsigned>(appId),
             static_cast<unsigned>(target.generation));
    diagPrint(buf);
}

/// Handle an ApplicationSessionManager update emitted by the launch worker.
void MoonlightApp::handleApplicationLaunchUpdate(
    const ml::ApplicationLaunchUpdate& update) {
    // --- Stale-result filtering --------------------------------------------
    // If this update is for a different host, app, or an older generation,
    // ignore it.
    if (update.hostId != m_launchHostId ||
        update.applicationId != m_launchAppId ||
        update.generation < m_launchGeneration) {
        diagPrint("[app] launch: stale result (ignored)");
        return;
    }

    m_launchActive = false;

    // --- Apply the result ---------------------------------------------------
    if (update.valid) {
        // Retain the successful descriptor for the Stream Session module.
        m_pendingSessionDescriptor = update.session;

        // --- Automatic handoff to the StreamSessionManager -----------------
        // This is the real production handoff point: after a successful
        // launch/resume, the StreamSessionDescriptor is handed to the stream
        // manager which calls LiStartConnection() on a worker. No second user
        // action is required.
        startStreamSession(update.session);

        // Schedule a fresh serverinfo refresh so currentGame / server busy
        // state are authoritative after the launch.
        scheduleServerInfoQuery(update.hostId);

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[app] launch success: host id=%llu app id=%u addr='%s' rtspPort=%u",
                 static_cast<unsigned long long>(update.hostId),
                 static_cast<unsigned>(update.applicationId),
                 update.session.ipv4Address.c_str(),
                 static_cast<unsigned>(update.session.rtspPort));
        diagPrint(buf);
    } else {
        // On failure, clear any pending descriptor.
        m_pendingSessionDescriptor.reset();

        // The launch failed before the stream was established. Clear the
        // transient Connecting state so the App List returns to the normal
        // Host.currentGame-derived presentation.
        clearConnectingState();
        diagPrint("[app] launch failed: clearing connecting state");

        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] launch failed for host id=%llu app id=%u (category %d)",
                 static_cast<unsigned long long>(update.hostId),
                 static_cast<unsigned>(update.applicationId),
                 static_cast<int>(update.result.category));
        diagPrint(buf);
    }

    // --- Update the UI ------------------------------------------------------
    if (m_ui && m_ui->isInitialized()) {
        ui::ApplicationLaunchViewState view;
        view.active = false;
        view.applicationId = update.applicationId;
        if (update.valid) {
            view.state = ui::LaunchPresentationState::Succeeded;
            view.statusText = "Launched";
        } else if (update.result.category ==
                   ml::ApplicationLaunchResultCategory::Cancelled) {
            view.state = ui::LaunchPresentationState::Cancelled;
            view.statusText = "Launch cancelled";
        } else {
            view.state = ui::LaunchPresentationState::Failed;
            view.statusText = "Unable to launch application";
        }
        m_ui->setApplicationLaunchView(view);
    }
}

/// Handle a QuitApplication intent.
/// Validates the host/app, builds the ApplicationQuitTarget, and starts the
/// remote quit operation. Triangle only issues a quit when the focused app is
/// currently running (the UiManager gates this), but MoonlightApp re-validates
/// against authoritative host state before scheduling.
bool MoonlightApp::handleQuitApplication(ml::HostId hostId,
                                         ml::ApplicationId appId) {
    if (!m_applicationSessionManager || !m_hostManager || !m_pairingManager) {
        diagPrint("[app] quit: launch manager unavailable (ignored)");
        return false;
    }

    // Resolve the host.
    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        diagPrint("[app] quit: unknown HostId (ignored)");
        return false;
    }

    // --- Validate preconditions --------------------------------------------
    if (host->availability != ml::HostAvailability::Online) {
        diagPrint("[app] quit: host not Online (rejected)");
        return false;
    }
    if (host->serverInfoState != ml::ServerInfoQueryState::Valid) {
        diagPrint("[app] quit: serverinfo not Valid (rejected)");
        return false;
    }
    if (host->pairingState != ml::HostPairingState::Paired) {
        diagPrint("[app] quit: host not Paired (rejected)");
        return false;
    }
    if (host->serverId.empty() || host->ipv4Addresses.empty() ||
        host->httpsPort == 0) {
        diagPrint("[app] quit: invalid host metadata (rejected)");
        return false;
    }

    // --- Validate the selected app corresponds to the running app ----------
    // The host is authoritative. Only quit when the selected app is the
    // currently running application (Host.currentGame).
    if (host->currentGame == 0 || host->currentGame != appId) {
        diagPrint("[app] quit: selected app is not the running app (rejected)");
        return false;
    }

    // --- Validate local trust and obtain credentials -----------------------
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        diagPrint("[app] quit: host claims Paired but no local trust (reconciling)");
        reconcileHostPairingState(hostId);
        return false;
    }

    // --- Shared HttpClient serialization -----------------------------------
    // Do not start a quit while a launch/resume is active or another quit is
    // pending.
    if (m_applicationSessionManager->isLaunching() ||
        m_applicationSessionManager->isQuitting()) {
        diagPrint("[app] quit: another app-session operation active (rejected)");
        return false;
    }
    if (m_applicationCatalog && m_applicationCatalog->isLoading()) {
        diagPrint("[app] quit: app catalog still loading (rejected)");
        return false;
    }
    if (m_pairingManager->isPairing()) {
        diagPrint("[app] quit: pairing active (rejected)");
        return false;
    }
    if (m_serverInfoManager && m_serverInfoManager->isPending(hostId)) {
        diagPrint("[app] quit: serverinfo pending for host (rejected)");
        return false;
    }

    // --- Build the ApplicationQuitTarget -----------------------------------
    ml::ApplicationQuitTarget target;
    target.hostId = host->id;
    target.applicationId = appId;
    target.serverId = host->serverId;
    target.ipv4Addresses = host->ipv4Addresses;
    target.httpsPort = host->httpsPort;
    target.clientUniqueId = m_pairingManager->clientUniqueId();
    target.clientCertPem = m_pairingManager->clientCertPem();
    target.clientKeyPem = m_pairingManager->clientKeyPem();

    // --- Request the quit --------------------------------------------------
    ml::ApplicationLaunchResult result = m_applicationSessionManager->requestQuit(target);
    if (!result.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] quit request failed for host id=%llu app id=%u (category %d)",
                 static_cast<unsigned long long>(hostId),
                 static_cast<unsigned>(appId),
                 static_cast<int>(result.category));
        diagPrint(buf);
        return false;
    }

    // --- Track the current quit request ------------------------------------
    m_quitHostId = hostId;
    m_quitAppId = appId;
    m_quitGeneration = target.generation;
    m_quitActive = true;

    // --- Enter the quit-progress state in the UI ---------------------------
    if (m_ui && m_ui->isInitialized()) {
        ui::ApplicationQuitViewState view;
        view.active = true;
        view.applicationId = appId;
        view.state = ui::QuitPresentationState::Quitting;
        view.statusText = "Quitting...";
        m_ui->setApplicationQuitView(view);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[app] quit requested for host id=%llu app id=%u",
             static_cast<unsigned long long>(hostId),
             static_cast<unsigned>(appId));
    diagPrint(buf);
    return true;
}

/// Handle a QuitAndLaunchApplication intent: quit the running app, then
/// launch the focused app once the quit completes successfully.
void MoonlightApp::handleQuitAndLaunchApplication(ml::HostId hostId,
                                                  ml::ApplicationId quitAppId,
                                                  ml::ApplicationId launchAppId) {
    // Record the pending launch-after-quit target. It is consumed in
    // handleApplicationQuitUpdate once the quit succeeds.
    m_pendingLaunchAfterQuitHostId = hostId;
    m_pendingLaunchAfterQuitAppId = launchAppId;

    // Enter the waiting state immediately so the about-to-be-launched app
    // shows "Waiting..." on its card while the quit request is in flight
    // (the /cancel request can block for a while as the host stops the app).
    m_waitingForQuitToComplete = true;

    // Refresh the App List presentation so the about-to-be-launched app card
    // immediately shows the "Waiting..." status.
    refreshAppListView();

    if (!handleQuitApplication(hostId, quitAppId)) {
        // The quit could not be scheduled; clear the pending launch so no
        // stale launch fires later.
        m_pendingLaunchAfterQuitHostId = 0;
        m_pendingLaunchAfterQuitAppId = 0;
        m_waitingForQuitToComplete = false;
        // Refresh again so the "Waiting..." status is cleared.
        refreshAppListView();
    }
}

/// Handle an ApplicationSessionManager quit update emitted by the quit worker.
void MoonlightApp::handleApplicationQuitUpdate(
    const ml::ApplicationQuitUpdate& update) {
    // --- Stale-result filtering --------------------------------------------
    // If this update is for a different host, app, or an older generation,
    // ignore it.
    if (update.hostId != m_quitHostId ||
        update.applicationId != m_quitAppId ||
        update.generation < m_quitGeneration) {
        diagPrint("[app] quit: stale result (ignored)");
        return;
    }

    m_quitActive = false;

    // --- Apply the result ---------------------------------------------------
    if (update.valid) {
        diagPrint("[app] remote application quit accepted");

        // If a quit-and-launch is pending, do NOT launch immediately. The
        // /cancel request only confirms the quit was accepted; the host may
        // still be stopping the app. Wait for the authoritative serverinfo
        // currentGame to reach 0 (host free) before launching the pending app.
        // This is driven by maybeLaunchPendingAfterQuit() on each serverinfo
        // refresh for the current App List host.
        if (m_pendingLaunchAfterQuitHostId != 0 &&
            m_pendingLaunchAfterQuitAppId != 0) {
            m_waitingForQuitToComplete = true;
            diagPrint("[app] quit-and-launch: waiting for host to free before launching");
        }

        // The host is authoritative. Do NOT fake currentGame = 0 locally.
        // Schedule a fresh authenticated serverinfo refresh so the host's
        // currentGame / server state are updated authoritatively. This refresh
        // drives maybeLaunchPendingAfterQuit() once the host reports free.
        scheduleServerInfoQuery(update.hostId);

        // Update the UI: clear the quit-progress status line.
        if (m_ui && m_ui->isInitialized()) {
            ui::ApplicationQuitViewState view;
            view.active = false;
            view.applicationId = update.applicationId;
            view.state = ui::QuitPresentationState::Succeeded;
            view.statusText = "Quit requested";
            m_ui->setApplicationQuitView(view);
        }
    } else {
        // Quit failed. Keep the card marked Running (no local mutation of
        // currentGame); report the failure and keep the App List usable.
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] remote application quit failed for host id=%llu app id=%u (category %d)",
                 static_cast<unsigned long long>(update.hostId),
                 static_cast<unsigned>(update.applicationId),
                 static_cast<int>(update.result.category));
        diagPrint(buf);

        // Abandon any pending post-quit launch: the quit did not succeed, so
        // the intended app must not be launched.
        m_pendingLaunchAfterQuitHostId = 0;
        m_pendingLaunchAfterQuitAppId = 0;
        m_waitingForQuitToComplete = false;

        if (m_ui && m_ui->isInitialized()) {
            ui::ApplicationQuitViewState view;
            view.active = false;
            view.applicationId = update.applicationId;
            view.state = ui::QuitPresentationState::Failed;
            view.statusText = "Unable to quit application";
            m_ui->setApplicationQuitView(view);
        }
    }
}

/// Launch the pending post-quit app once the host confirms the quit completed.
///
/// Called after each serverinfo refresh for the current App List host while a
/// quit-and-launch is waiting (m_waitingForQuitToComplete). The /cancel request
/// only confirms the quit was accepted; the host may still be stopping the app.
/// The authoritative Host.currentGame is the signal that the host is free:
///   - currentGame == 0            → host free → launch the pending app.
///   - currentGame == quit app     → still stopping → keep waiting.
///   - currentGame == other app    → quit did not free the host as expected →
///                                   abandon the pending launch.
void MoonlightApp::maybeLaunchPendingAfterQuit() {
    if (!m_waitingForQuitToComplete) {
        return;
    }
    if (m_pendingLaunchAfterQuitHostId == 0 ||
        m_pendingLaunchAfterQuitAppId == 0) {
        m_waitingForQuitToComplete = false;
        return;
    }
    if (!m_hostManager) {
        return;
    }

    const ml::Host* host =
        m_hostManager->findHost(m_pendingLaunchAfterQuitHostId);
    if (host == nullptr) {
        // Host no longer exists; abandon the pending launch.
        m_pendingLaunchAfterQuitHostId = 0;
        m_pendingLaunchAfterQuitAppId = 0;
        m_waitingForQuitToComplete = false;
        return;
    }

    // The host is still stopping the app we quit — keep waiting.
    if (host->currentGame == m_quitAppId) {
        return;
    }

    // The host is running a different app than the one we quit (and not free).
    // The quit did not free the host as expected; abandon the pending launch.
    if (host->currentGame != 0) {
        diagPrint("[app] quit-and-launch: host running a different app; abandoning pending launch");
        m_pendingLaunchAfterQuitHostId = 0;
        m_pendingLaunchAfterQuitAppId = 0;
        m_waitingForQuitToComplete = false;
        return;
    }

    // Host is free (currentGame == 0). Launch the pending app.
    ml::HostId launchHost = m_pendingLaunchAfterQuitHostId;
    ml::ApplicationId launchApp = m_pendingLaunchAfterQuitAppId;
    m_pendingLaunchAfterQuitHostId = 0;
    m_pendingLaunchAfterQuitAppId = 0;
    m_waitingForQuitToComplete = false;

    diagPrint("[app] quit-and-launch: host free — launching pending app");
    m_allowLaunchAfterQuit = true;
    handleActivateApplication(launchHost, launchApp);
    m_allowLaunchAfterQuit = false;
}

// ---------------------------------------------------------------------------
// Stream Session handoff and update handling
// ---------------------------------------------------------------------------

/// Automatically hand off a successful launch/resume result to the
/// StreamSessionManager. Called from handleApplicationLaunchUpdate on a
/// successful launch/resume. Copies the descriptor into the stream manager
/// and starts the stream session.
void MoonlightApp::startStreamSession(const ml::StreamSessionDescriptor& session) {
    if (!m_streamSessionManager) {
        diagPrint("[stream] handoff skipped: stream manager unavailable");
        return;
    }

    // The stream manager must not already be active.
    if (m_streamSessionManager->isActive()) {
        diagPrint("[stream] handoff skipped: stream already active");
        return;
    }

    // Start the AudioDecodeOutput so it is ready to consume encoded audio
    // once the stream begins. The worker waits for the target session
    // generation and its audio configuration.
    if (m_audioDecodeOutput) {
        ml::AudioDecodeOutputResult adoResult = m_audioDecodeOutput->start();
        if (!adoResult.succeeded()) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[audio] start failed (category %d nativeError %d)",
                     static_cast<int>(adoResult.category), adoResult.nativeError);
            diagPrint(buf);
        }
    }

    // Copy the descriptor (the source is a const member of the launch update).
    ml::StreamSessionDescriptor copy = session;
    ml::StreamSessionResult result =
        m_streamSessionManager->start(std::move(copy));
    if (!result.succeeded()) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[stream] start failed (category %d nativeError %d)",
                 static_cast<int>(result.category), result.nativeError);
        diagPrint(buf);
        return;
    }

    // Record the stream host/app for the exit-chord quit-on-exit path.
    m_streamHostId = session.hostId;
    m_streamAppId = session.applicationId;

    // Start controller input streaming for this session generation. The module
    // stays dormant until the input channel is ready (StreamSessionManager
    // state == Streaming). Mouse emulation always starts OFF and can be
    // toggled in-stream via Options + TouchPad Click.
    if (m_controllerInputStreaming) {
        m_controllerInputStreaming->start(
            m_streamSessionManager->currentSessionGeneration(),
            false);
    }

    diagPrint("[stream] session handoff accepted — LiStartConnection scheduled on worker");
}

/// Handle a StreamSessionManager connection-state update emitted by the
/// stream lifecycle worker. Applies the update to the UI/state and clears
/// transient connecting state on terminal outcomes.
void MoonlightApp::handleStreamSessionUpdate(const ml::StreamSessionUpdate& update) {
    switch (update.type) {
    case ml::StreamSessionUpdateType::Connecting:
        diagPrint("[stream] update: connecting");
        break;
    case ml::StreamSessionUpdateType::Connected:
        diagPrint("[stream] update: connected (stream established)");
        // The stream is now established. The transient Connecting state is
        // superseded by the Running presentation (Host.currentGame is
        // authoritative). Clear the local Connecting state.
        clearConnectingState();
        // Enter stream presentation mode: the main thread presents decoded
        // video frames instead of the CPU UI compositor.
        m_streamActive = true;
        m_streamFirstFramePresented = false;
        m_streamStatsOverlayLogged = false;
        // Mouse emulation always starts OFF for a new stream session; reset
        // the edge-detection baseline so no stale toggle is detected.
        m_prevMouseEmulationEnabled = false;
        m_heldDecodedFrame.release();
        m_retiredFrames.clear();
        for (auto& ref : m_streamSlotRefs) {
            ref.valid = false;
        }
        // Reset the stream statistics baseline for this new session so no
        // counters leak from a previous session.
        resetStreamStatisticsBaseline();
        // Begin the stream presentation session in the UI: show the short-lived
        // exit hint, then transition to the statistics overlay (if enabled) or
        // an empty stream screen. Mouse emulation is a runtime-only, in-stream
        // toggle (Options + TouchPad Click) and is not tracked by the UI.
        if (m_ui && m_ui->isInitialized()) {
            const bool statsEnabled =
                m_settingsManager &&
                m_settingsManager->settings().streamStatisticsEnabled;
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[stream] begin stream session: statsEnabled=%d",
                         statsEnabled ? 1 : 0);
                diagPrint(buf);
            }
            m_ui->beginStreamSession(m_currTimestamp, statsEnabled);
        }
        break;
    case ml::StreamSessionUpdateType::Interrupted:
        diagPrint("[stream] update: interrupted");
        break;
    case ml::StreamSessionUpdateType::Terminated:
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[stream] update: terminated (nativeError %d)",
                     update.nativeError);
            diagPrint(buf);
            // The remote app may still be running on Sunshine. Do NOT quit it.
            // Clear the transient Connecting state and remain on the App List.
            clearConnectingState();
            // Stop controller input streaming first (sends a neutral state if
            // the connection is still usable, then resets local state).
            if (m_controllerInputStreaming) {
                m_controllerInputStreaming->stop();
            }
            // Exit stream presentation mode. Release the held frame BEFORE
            // stopping the decoder so no presentation reads freed memory.
            m_streamActive = false;
            m_streamHostId = 0;
            m_streamAppId = 0;
            m_heldDecodedFrame.release();
            m_retiredFrames.clear();
            for (auto& ref : m_streamSlotRefs) {
                ref.valid = false;
            }
            // End the stream presentation session in the UI: clear overlay
            // state and return to the App List. No stale overlay may appear
            // over the App List.
            if (m_ui && m_ui->isInitialized()) {
                m_ui->endStreamSession();
            }
            // Stop the decoder on the main thread (after presentation has
            // ceased). This joins the decoder worker and releases all decoder
            // resources.
            if (m_videoDecoder) {
                m_videoDecoder->stop();
            }
            // Stop the AudioDecodeOutput (it is no longer needed once the
            // stream ends). This closes the AudioOut port and destroys the
            // Opus decoder.
            if (m_audioDecodeOutput) {
                m_audioDecodeOutput->stop();
            }
            // Return to the App List presentation. Invalidate the App List /
            // App Grid renderer "valid" flags so the next render() performs a
            // full recompose (the CPU compositor surface was not updated while
            // streaming, so it must be fully redrawn).
            m_prevAppListValid = false;
            m_prevAppGridValid = false;
            refreshAppListView();
        }
        break;
    case ml::StreamSessionUpdateType::Failed:
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[stream] update: failed (nativeError %d)",
                     update.nativeError);
            diagPrint(buf);
            // Stream-start failure. Clear the transient Connecting state and
            // remain on the App List. Do NOT quit the remote application (it
            // may still be running on Sunshine).
            clearConnectingState();
            // Stop controller input streaming first (sends a neutral state if
            // the connection is still usable, then resets local state).
            if (m_controllerInputStreaming) {
                m_controllerInputStreaming->stop();
            }
            // Exit stream presentation mode. Release the held frame BEFORE
            // stopping the decoder so no presentation reads freed memory.
            m_streamActive = false;
            m_streamHostId = 0;
            m_streamAppId = 0;
            m_heldDecodedFrame.release();
            m_retiredFrames.clear();
            for (auto& ref : m_streamSlotRefs) {
                ref.valid = false;
            }
            // End the stream presentation session in the UI: clear overlay
            // state and return to the App List.
            if (m_ui && m_ui->isInitialized()) {
                m_ui->endStreamSession();
            }
            // Stop the decoder on the main thread (after presentation has
            // ceased).
            if (m_videoDecoder) {
                m_videoDecoder->stop();
            }
            // Stop the AudioDecodeOutput (it is no longer needed once the
            // stream ends).
            if (m_audioDecodeOutput) {
                m_audioDecodeOutput->stop();
            }
            // Return to the App List presentation. Invalidate the App List /
            // App Grid renderer "valid" flags so the next render() performs a
            // full recompose.
            m_prevAppListValid = false;
            m_prevAppGridValid = false;
            refreshAppListView();
        }
        break;
    case ml::StreamSessionUpdateType::StoppedByUser:
        diagPrint("[stream] update: stopped by user");
        // The stream was stopped locally. Clear the transient Connecting state
        // and remain on the App List.
        clearConnectingState();
        // Stop controller input streaming first (sends a neutral state if the
        // connection is still usable, then resets local state).
        if (m_controllerInputStreaming) {
            m_controllerInputStreaming->stop();
        }
        // Exit stream presentation mode. Release the held frame BEFORE
        // stopping the decoder so no presentation reads freed memory.
        m_streamActive = false;
        m_streamHostId = 0;
        m_streamAppId = 0;
        m_heldDecodedFrame.release();
        m_retiredFrames.clear();
        for (auto& ref : m_streamSlotRefs) {
            ref.valid = false;
        }
        // End the stream presentation session in the UI: clear overlay state
        // and return to the App List.
        if (m_ui && m_ui->isInitialized()) {
            m_ui->endStreamSession();
        }
        // Stop the decoder on the main thread (after presentation has ceased).
        if (m_videoDecoder) {
            m_videoDecoder->stop();
        }
        // Stop the AudioDecodeOutput (it is no longer needed once the stream
        // ends). This closes the AudioOut port and destroys the Opus decoder.
        if (m_audioDecodeOutput) {
            m_audioDecodeOutput->stop();
        }
        // Return to the App List presentation. Invalidate the App List /
        // App Grid renderer "valid" flags so the next render() performs a
        // full recompose.
        m_prevAppListValid = false;
        m_prevAppGridValid = false;
        refreshAppListView();
        break;
    default:
        break;
    }
}

/// Handle an AudioDecodeOutput update emitted by the audio worker.
void MoonlightApp::handleAudioDecodeOutputUpdate(
    const ml::AudioDecodeOutputUpdate& update) {
    switch (update.type) {
    case ml::AudioDecodeOutputUpdateType::Started:
        diagPrint("[app] audio: started");
        break;

    case ml::AudioDecodeOutputUpdateType::PlaybackStarted:
        diagPrint("[app] audio: playback started");
        break;

    case ml::AudioDecodeOutputUpdateType::Failed:
        // Fatal audio failure: report and continue video-only. Do NOT stop
        // the stream automatically just because audio failed.
        diagPrint("[app] audio: FAILED — continuing video-only");
        break;

    case ml::AudioDecodeOutputUpdateType::Stopped:
        diagPrint("[app] audio: stopped");
        break;

    default:
        break;
    }
}

/// Open the Settings overlay: populate the UiManager's editable working copy
/// from the authoritative SettingsManager values and mark the overlay open.
void MoonlightApp::openSettingsOverlay() {
    if (!m_ui || !m_ui->isInitialized() || !m_settingsManager) {
        return;
    }

    // Populate the UiManager's editable working copy from the authoritative
    // SettingsManager values.
    const ml::Settings& s = m_settingsManager->settings();
    ui::SettingsViewState view;
    view.resolutionIndex = (s.resolution == ml::StreamResolution::R720p) ? 0 : 1;
    view.frameRateIndex = (s.frameRate == ml::StreamFrameRate::Fps30) ? 0 : 1;
    view.quitAppOnStreamExit = s.quitAppOnStreamExit;
    view.presentationStyleIndex =
        (s.presentationStyle == ml::PresentationStyle::Grid) ? 1 : 0;
    view.streamStatisticsEnabled = s.streamStatisticsEnabled;
    view.muteHostSpeakers = s.muteHostSpeakers;
    view.optimizeGameSettings = s.optimizeGameSettings;
    view.showQuitAppDialog = s.showQuitAppDialog;
    m_ui->setSettingsView(view);

    m_settingsOverlayOpen = true;
}

/// Close the Settings overlay: commit the edited values from UiManager to
/// SettingsManager and persist them to disk.
void MoonlightApp::closeSettingsOverlay(const ui::SettingsViewState& edited) {
    if (!m_settingsManager) {
        m_settingsOverlayOpen = false;
        return;
    }

    // Commit the edited values to the authoritative SettingsManager.
    ml::Settings s = m_settingsManager->settings();
    s.resolution = (edited.resolutionIndex == 0)
                       ? ml::StreamResolution::R720p
                       : ml::StreamResolution::R1080p;
    s.frameRate = (edited.frameRateIndex == 0)
                      ? ml::StreamFrameRate::Fps30
                      : ml::StreamFrameRate::Fps60;
    s.quitAppOnStreamExit = edited.quitAppOnStreamExit;
    s.presentationStyle = (edited.presentationStyleIndex == 0)
                              ? ml::PresentationStyle::PlayStation
                              : ml::PresentationStyle::Grid;
    s.streamStatisticsEnabled = edited.streamStatisticsEnabled;
    s.muteHostSpeakers = edited.muteHostSpeakers;
    s.optimizeGameSettings = edited.optimizeGameSettings;
    s.showQuitAppDialog = edited.showQuitAppDialog;
    m_settingsManager->setSettings(s);

    // Push the authoritative presentation style to the UI manager so the
    // Host/App List renders in the committed mode.
    if (m_ui) {
        m_ui->setPresentationStyle(s.presentationStyle);
        m_ui->setShowQuitAppDialog(s.showQuitAppDialog);
    }

    // Persist to disk.
    m_settingsManager->save();

    m_settingsOverlayOpen = false;
}

// ---------------------------------------------------------------------------
// Host snapshot → UiManager flow
// ---------------------------------------------------------------------------

void MoonlightApp::updateUiHostSnapshot() {
    if (!m_ui || !m_ui->isInitialized() || !m_hostManager) {
        return;
    }

    // --- Pending manual host add placeholder hiding ------------------------
    // While a manual host add is pending its initial serverinfo bootstrap, the
    // temporary placeholder is excluded from the UI snapshot so no
    // half-initialized/bootstrap-only card is rendered underneath the
    // "Checking..." modal. HostManager retains the placeholder internally (the
    // serverinfo machinery still needs it); only the application-to-UI
    // snapshot omits it.
    //
    // The pending marker is kept set through serverinfo success so the
    // newly-added host also stays hidden while the "Found" status modal is
    // still open (the card must never be drawn underneath the modal, which
    // previously triggered a GPU page fault). Once the modal is dismissed,
    // the marker is cleared below and the host appears normally.
    if (m_pendingManualAddHostId != 0 && !m_ui->isAddHostStatusOpen()) {
        m_pendingManualAddHostId = 0;
        m_pendingManualAddWasExisting = false;
    }

    const std::vector<ml::Host>& allHosts = m_hostManager->hosts();
    // Only a newly-created manual placeholder is hidden. If the manual add
    // matched an existing host (m_pendingManualAddWasExisting), that existing
    // card must remain visible.
    const bool hidePlaceholder =
        (m_pendingManualAddHostId != 0 && !m_pendingManualAddWasExisting);

    // Build the UI-visible host list (excluding the pending placeholder).
    std::vector<ml::Host> uiHosts;
    if (hidePlaceholder) {
        uiHosts.reserve(allHosts.size());
        for (const ml::Host& h : allHosts) {
            if (h.id == m_pendingManualAddHostId) {
                continue;   // Skip the pending bootstrap placeholder.
            }
            uiHosts.push_back(h);
        }
    } else {
        uiHosts = allHosts;
    }

    bool snapshotChanged = !m_lastUiHostsValid ||
                           uiHosts.size() != m_lastUiHosts.size();
    if (!snapshotChanged) {
        for (size_t i = 0; i < uiHosts.size(); ++i) {
            const ml::Host& a = uiHosts[i];
            const ml::Host& b = m_lastUiHosts[i];
            if (a.id != b.id ||
                a.displayName != b.displayName ||
                a.availability != b.availability ||
                a.pairingState != b.pairingState ||
                a.currentGame != b.currentGame ||
                a.serverId != b.serverId ||
                a.serverVersion != b.serverVersion ||
                a.httpsPort != b.httpsPort) {
                snapshotChanged = true;
                break;
            }
        }
    }
    if (snapshotChanged) {
        m_ui->setHosts(uiHosts);
        m_lastUiHosts = uiHosts;
        m_lastUiHostsValid = true;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[app] UI host snapshot updated (%zu hosts)",
                 uiHosts.size());
        diagPrint(buf);
    }
}

// ---------------------------------------------------------------------------
// Host List dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for a Host List state change from `prev` to
/// `cur`. Fills `out` (bounded) and returns the count. Uses the
/// HostListRenderer visual-bounds helpers. Returns 0 when nothing changed.
int MoonlightApp::hostListDirtyRects(const ui::HostListRenderState& prev,
                                     const ui::HostListRenderState& cur,
                                     ps4::Rect* out, int maxOut) {
    if (!m_hostListRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Selection change ---------------------------------------------------
    if (prev.selectedIndex != cur.selectedIndex) {
        // In the PlayStation crossbar, the focused card is always anchored at
        // the fixed focus position, so a selection move shifts the ENTIRE card
        // row. Dirty the full card area.
        addRect(m_hostListRenderer->cardAreaBounds());
    }

    // --- Host state changes (availability / pairing / app-running / name) ---
    // Compare card-by-card. A changed card dirties only that card's bounds.
    const size_t common = prev.cards.size() < cur.cards.size()
                              ? prev.cards.size() : cur.cards.size();
    for (size_t i = 0; i < common; ++i) {
        const ui::HostListCardRenderData& a = prev.cards[i];
        const ui::HostListCardRenderData& b = cur.cards[i];
        if (a.displayName != b.displayName ||
            a.availability != b.availability ||
            a.pairingState != b.pairingState ||
            a.appRunning != b.appRunning) {
            addRect(m_hostListRenderer->cardVisualBounds(
                static_cast<int>(i), cur.selectedIndex));
        }
    }

    // --- Structural change (host added/removed/order) -----------------------
    // A card-count or ordering change may shift the whole crossbar layout, so
    // dirty the full card area.
    if (prev.cards.size() != cur.cards.size()) {
        addRect(m_hostListRenderer->cardAreaBounds());
    }

    // --- crossIsAccept change ----------------------------------------------
    // The BottomLegend glyphs change; dirty the legend.
    if (prev.crossIsAccept != cur.crossIsAccept) {
        addRect(m_hostListRenderer->bottomLegendBounds());
    }

    return count;
}

// ---------------------------------------------------------------------------
// Host List Grid dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for a Host List Grid state change from `prev`
/// to `cur`. Fills `out` (bounded) and returns the count. Uses the
/// HostGridRenderer visual-bounds helpers. Returns 0 when nothing changed.
int MoonlightApp::hostGridDirtyRects(const ui::HostGridRenderState& prev,
                                     const ui::HostGridRenderState& cur,
                                     ps4::Rect* out, int maxOut) {
    if (!m_hostGridRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Selection movement ------------------------------------------------
    // In the grid, a selection move may also change the scroll offset (the
    // selected row is kept fully visible). If the scroll offset changed, the
    // entire grid region is dirty (every card's screen position shifts). If
    // only the selection changed without a scroll change, dirty the old and
    // new selected card bounds. The scroll offset is owned by the renderer
    // (it owns the grid geometry).
    const float prevScroll = m_hostGridRenderer->scrollOffsetFor(prev);
    const float curScroll = m_hostGridRenderer->scrollOffsetFor(cur);
    if (prev.selectedIndex != cur.selectedIndex) {
        if (prevScroll != curScroll) {
            // Scroll movement: dirty the entire grid region.
            addRect(m_hostGridRenderer->cardAreaBounds());
        } else {
            // Selection movement without scroll: dirty old and new card bounds.
            if (prev.selectedIndex >= 0 &&
                prev.selectedIndex < static_cast<int>(prev.hosts.size())) {
                addRect(m_hostGridRenderer->cardVisualBounds(
                    prev.selectedIndex, prev));
            }
            if (cur.selectedIndex >= 0 &&
                cur.selectedIndex < static_cast<int>(cur.hosts.size())) {
                addRect(m_hostGridRenderer->cardVisualBounds(
                    cur.selectedIndex, cur));
            }
        }
    }

    // --- Scroll offset change without selection change ---------------------
    // A scroll change (e.g., list-size change that re-anchors the selected
    // row) dirties the entire grid region.
    if (prev.selectedIndex == cur.selectedIndex &&
        prevScroll != curScroll) {
        addRect(m_hostGridRenderer->cardAreaBounds());
    }

    // --- Host state changes (name / paired / availability) -----------------
    // Compare card-by-card. A changed card dirties only that card's bounds.
    const size_t common = prev.hosts.size() < cur.hosts.size()
                              ? prev.hosts.size() : cur.hosts.size();
    for (size_t i = 0; i < common; ++i) {
        const ui::HostGridCardRenderState& a = prev.hosts[i];
        const ui::HostGridCardRenderState& b = cur.hosts[i];
        if (a.name != b.name ||
            a.paired != b.paired ||
            a.availability != b.availability) {
            addRect(m_hostGridRenderer->cardVisualBounds(
                static_cast<int>(i), cur));
        }
    }

    // --- Structural change (host added/removed/order) ----------------------
    // A card-count or ordering change may shift the grid layout (and the
    // scroll offset), so dirty the full card area. This is a rare structural
    // event; a full card-area redraw is acceptable.
    if (prev.hosts.size() != cur.hosts.size()) {
        addRect(m_hostGridRenderer->cardAreaBounds());
    }

    // --- crossIsAccept change ----------------------------------------------
    // The BottomLegend glyphs change; dirty the legend.
    if (prev.crossIsAccept != cur.crossIsAccept) {
        addRect(m_hostGridRenderer->bottomLegendBounds());
    }

    return count;
}

// ---------------------------------------------------------------------------
// App List dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for an App List state change from `prev` to
/// `cur`. Fills `out` (bounded) and returns the count. Uses the
/// AppListRenderer visual-bounds helpers. Returns 0 when nothing changed.
/// Detects selection moves, per-card content changes (running, connecting,
/// quitting, title, artwork), structural changes (app added/removed), and
/// crossIsAccept changes — matching the Host List dirty-region behavior.
int MoonlightApp::appListDirtyRects(const ui::AppListRenderState& prev,
                                    const ui::AppListRenderState& cur,
                                    ps4::Rect* out, int maxOut) {
    if (!m_appListRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Selection change ---------------------------------------------------
    if (prev.selectedIndex != cur.selectedIndex) {
        // In the PlayStation crossbar, the focused card is always anchored at
        // the fixed focus position, so a selection move shifts the ENTIRE card
        // row. Dirty the full card area.
        addRect(m_appListRenderer->cardAreaBounds());
    }

    // --- App state changes (running / connecting / quitting / title / art) --
    // Compare card-by-card. A changed card dirties only that card's bounds.
    const size_t common = prev.cards.size() < cur.cards.size()
                              ? prev.cards.size() : cur.cards.size();
    for (size_t i = 0; i < common; ++i) {
        const ui::AppListCardRenderData& a = prev.cards[i];
        const ui::AppListCardRenderData& b = cur.cards[i];
        if (a.title != b.title ||
            a.running != b.running ||
            a.connecting != b.connecting ||
            a.waitingToLaunch != b.waitingToLaunch ||
            a.quitting != b.quitting ||
            a.quitStatusText != b.quitStatusText ||
            a.artwork != b.artwork) {
            addRect(m_appListRenderer->cardVisualBounds(
                static_cast<int>(i), cur.selectedIndex));
        }
    }

    // --- Structural change (app added/removed/order) ------------------------
    // A card-count or ordering change may shift the whole crossbar layout, so
    // dirty the full card area.
    if (prev.cards.size() != cur.cards.size()) {
        addRect(m_appListRenderer->cardAreaBounds());
    }

    // --- crossIsAccept change ----------------------------------------------
    // The BottomLegend glyphs change; dirty the legend.
    if (prev.crossIsAccept != cur.crossIsAccept) {
        addRect(m_appListRenderer->bottomLegendBounds());
    }

    return count;
}

// ---------------------------------------------------------------------------
// App List Grid dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for an App List Grid state change from `prev`
/// to `cur`. Fills `out` (bounded) and returns the count. Uses the
/// AppGridRenderer visual-bounds helpers. Returns 0 when nothing changed.
/// Detects selection/scroll movement, per-card content changes (running,
/// connecting, quitting, title, artwork), structural changes (app
/// added/removed), and crossIsAccept changes — matching the Host Grid
/// dirty-region behavior.
int MoonlightApp::appGridDirtyRects(const ui::AppGridRenderState& prev,
                                    const ui::AppGridRenderState& cur,
                                    ps4::Rect* out, int maxOut) {
    if (!m_appGridRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Selection movement ------------------------------------------------
    // In the grid, a selection move may also change the scroll offset (the
    // selected row is kept fully visible). If the scroll offset changed, the
    // entire grid region is dirty (every card's screen position shifts). If
    // only the selection changed without a scroll change, dirty the old and
    // new selected card bounds. The scroll offset is owned by the renderer
    // (it owns the grid geometry).
    const float prevScroll = m_appGridRenderer->scrollOffsetFor(prev);
    const float curScroll = m_appGridRenderer->scrollOffsetFor(cur);
    if (prev.selectedIndex != cur.selectedIndex) {
        if (prevScroll != curScroll) {
            // Scroll movement: dirty the entire grid region.
            addRect(m_appGridRenderer->cardAreaBounds());
        } else {
            // Selection movement without scroll: dirty old and new card bounds.
            if (prev.selectedIndex >= 0 &&
                prev.selectedIndex < static_cast<int>(prev.cards.size())) {
                addRect(m_appGridRenderer->cardVisualBounds(
                    prev.selectedIndex, prev));
            }
            if (cur.selectedIndex >= 0 &&
                cur.selectedIndex < static_cast<int>(cur.cards.size())) {
                addRect(m_appGridRenderer->cardVisualBounds(
                    cur.selectedIndex, cur));
            }
        }
    }

    // --- Scroll offset change without selection change ---------------------
    // A scroll change (e.g., list-size change that re-anchors the selected
    // row) dirties the entire grid region.
    if (prev.selectedIndex == cur.selectedIndex &&
        prevScroll != curScroll) {
        addRect(m_appGridRenderer->cardAreaBounds());
    }

    // --- App state changes (running / connecting / quitting / title / art) --
    // Compare card-by-card. A changed card dirties only that card's bounds.
    const size_t common = prev.cards.size() < cur.cards.size()
                              ? prev.cards.size() : cur.cards.size();
    for (size_t i = 0; i < common; ++i) {
        const ui::AppGridCardRenderState& a = prev.cards[i];
        const ui::AppGridCardRenderState& b = cur.cards[i];
        if (a.title != b.title ||
            a.running != b.running ||
            a.connecting != b.connecting ||
            a.waitingToLaunch != b.waitingToLaunch ||
            a.quitting != b.quitting ||
            a.quitStatusText != b.quitStatusText ||
            a.artwork != b.artwork) {
            addRect(m_appGridRenderer->cardVisualBounds(
                static_cast<int>(i), cur));
        }
    }

    // --- Structural change (app added/removed/order) ------------------------
    // A card-count or ordering change may shift the grid layout (and the
    // scroll offset), so dirty the full card area. This is a rare structural
    // event; a full card-area redraw is acceptable.
    if (prev.cards.size() != cur.cards.size()) {
        addRect(m_appGridRenderer->cardAreaBounds());
    }

    // --- crossIsAccept change ----------------------------------------------
    // The BottomLegend glyphs change; dirty the legend.
    if (prev.crossIsAccept != cur.crossIsAccept) {
        addRect(m_appGridRenderer->bottomLegendBounds());
    }

    return count;
}

// ---------------------------------------------------------------------------
// Settings overlay dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for a Settings overlay state change from
/// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
/// SettingsRenderer visual-bounds helpers. Returns 0 when nothing changed.
int MoonlightApp::settingsDirtyRects(const ui::SettingsRenderState& prev,
                                     const ui::SettingsRenderState& cur,
                                     bool prevOpen, bool curOpen,
                                     ps4::Rect* out, int maxOut) {
    if (!m_settingsRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Open / close transitions ------------------------------------------
    // Opening or closing the Settings overlay dirties the full screen (the
    // dim layer covers the whole screen). This is a rare event; a full-screen
    // update is acceptable.
    if (prevOpen != curOpen) {
        addRect(m_settingsRenderer->dimBounds());
        return count;
    }

    // --- Selection change ---------------------------------------------------
    // Dirty the OLD and NEW selected row bounds. The old row must be redrawn
    // to clear its highlight; the new row must be redrawn to draw its
    // highlight. Without dirtying the old row, its highlight box stays on
    // screen after focus moves away.
    if (prev.selectedIndex != cur.selectedIndex) {
        if (prev.selectedIndex >= 0) {
            addRect(m_settingsRenderer->rowBounds(prev.selectedIndex));
        }
        if (cur.selectedIndex >= 0) {
            addRect(m_settingsRenderer->rowBounds(cur.selectedIndex));
        }
    }

    // --- Value change -------------------------------------------------------
    // Compare row-by-row. A changed row dirties only that row's bounds.
    const size_t common = prev.rows.size() < cur.rows.size()
                              ? prev.rows.size() : cur.rows.size();
    for (size_t i = 0; i < common; ++i) {
        if (prev.rows[i].label != cur.rows[i].label ||
            prev.rows[i].value != cur.rows[i].value ||
            prev.rows[i].enabled != cur.rows[i].enabled) {
            addRect(m_settingsRenderer->rowBounds(static_cast<int>(i)));
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Confirmation modal dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for a confirmation modal state change from
/// `prev` to `cur`. Fills `out` (bounded) and returns the count. Uses the
/// ModalRenderer visual-bounds helpers. Returns 0 when nothing changed.
int MoonlightApp::modalDirtyRects(const ui::ModalRenderState& prev,
                                  const ui::ModalRenderState& cur,
                                  bool prevOpen, bool curOpen,
                                  ps4::Rect* out, int maxOut) {
    if (!m_modalRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Open / close transitions ------------------------------------------
    // Opening or closing a modal dirties the full screen (the dim layer covers
    // the whole screen). This is a rare event; a full-screen update is
    // acceptable.
    if (prevOpen != curOpen) {
        addRect(m_modalRenderer->dimBounds());
        return count;
    }

    // --- Choice change (Delete Host Yes/No) ---------------------------------
    // Dirty the choice button bounds.
    if (prev.yesSelected != cur.yesSelected) {
        addRect(m_modalRenderer->choiceBounds(cur.kind));
    }

    return count;
}

// ---------------------------------------------------------------------------
// Add Host overlay dirty-region computation
// ---------------------------------------------------------------------------

/// Compute the dirty rectangles for an Add Host overlay / status modal state
/// change from `prev` to `cur`. Fills `out` (bounded) and returns the count.
/// Uses the AddHostRenderer visual-bounds helpers. Returns 0 when nothing
/// changed.
int MoonlightApp::addHostDirtyRects(const ui::AddHostRenderState& prev,
                                    const ui::AddHostRenderState& cur,
                                    bool prevOpen, bool curOpen,
                                    ps4::Rect* out, int maxOut) {
    if (!m_addHostRenderer || maxOut <= 0) {
        return 0;
    }

    int count = 0;
    auto addRect = [&](const ps4::Rect& r) {
        if (count < maxOut) {
            out[count++] = r;
        }
    };

    // --- Open / close transitions ------------------------------------------
    // Opening or closing the Add Host overlay dirties the full screen (the dim
    // layer covers the whole screen). This is a rare event; a full-screen
    // update is acceptable.
    if (prevOpen != curOpen) {
        addRect(m_addHostRenderer->dimBounds());
        return count;
    }

    // --- Presentation-state transition (Form <-> Checking/Found/Failed) ----
    // A state transition changes the panel size and all content, so dirty the
    // full screen (the panel geometry changes between the 760x460 form and the
    // 720x320 status modal).
    if (prev.state != cur.state) {
        addRect(m_addHostRenderer->dimBounds());
        return count;
    }

    // --- Form overlay changes (state == Form) ------------------------------
    if (cur.state == ui::AddHostPresentationState::Form) {
        // IP text change (from the ImeDialog).
        if (prev.ipText != cur.ipText) {
            addRect(m_addHostRenderer->ipRowBounds());
        }
        // Port text change (from the ImeDialog).
        if (prev.portText != cur.portText) {
            addRect(m_addHostRenderer->portRowBounds());
        }
        // Validation error change.
        if (prev.validationError != cur.validationError) {
            addRect(m_addHostRenderer->validationBounds());
        }
        // Selection change: dirty BOTH the old and new selected field bounds so
        // the previous highlight is cleared and the new one is drawn.
        if (prev.selectedField != cur.selectedField) {
            switch (prev.selectedField) {
            case 0: addRect(m_addHostRenderer->ipRowBounds()); break;
            case 1: addRect(m_addHostRenderer->portRowBounds()); break;
            case 2:
            case 3: addRect(m_addHostRenderer->buttonsBounds()); break;
            default: break;
            }
            switch (cur.selectedField) {
            case 0: addRect(m_addHostRenderer->ipRowBounds()); break;
            case 1: addRect(m_addHostRenderer->portRowBounds()); break;
            case 2:
            case 3: addRect(m_addHostRenderer->buttonsBounds()); break;
            default: break;
            }
        }
    } else {
        // --- Status modal changes (state == Checking/Found/Failed) ---------
        // Message change.
        if (prev.message != cur.message) {
            addRect(m_addHostRenderer->statusMessageBounds());
        }
        // Host name change (Found state).
        if (prev.hostName != cur.hostName) {
            addRect(m_addHostRenderer->statusMessageBounds());
        }
        // Action button label change (Checking -> Cancel, terminal -> OK).
        // The button label depends on the state, which is handled above by the
        // full-screen dirty on state transition. No additional dirty needed.
    }

    return count;
}

// ---------------------------------------------------------------------------
// Trusted-host restoration
// ---------------------------------------------------------------------------

/// Restore persisted trusted hosts into the HostManager registry.
///
/// Called during initialization after PairingStore and HostManager are
/// available and before discovery starts. Each persisted trusted-host record
/// is restored into the authoritative HostManager registry as an Offline,
/// locally-trusted host. When discovery later finds the same host, the
/// existing entry is reconciled to Online via the existing serviceFqdn
/// correlation (no duplicate card).
void MoonlightApp::restoreTrustedHosts() {
    if (!m_pairingManager || !m_hostManager) {
        diagPrint("[app] trusted-host restoration skipped: managers unavailable");
        return;
    }

    std::vector<ml::TrustedHost> trustedHosts;
    if (!m_pairingManager->loadTrustedHosts(trustedHosts)) {
        diagPrint("[app] failed to load trusted hosts for restoration");
        return;
    }

    for (const auto& th : trustedHosts) {
        m_hostManager->restoreTrustedHost(th.serverId, th.serviceFqdn,
                                          th.hostname, th.lastKnownIpv4,
                                          th.lastKnownDiscoveryPort,
                                          th.lastKnownHttpsPort);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[app] restored %zu trusted host(s)",
             trustedHosts.size());
    diagPrint(buf);

    // If any restored host has a persisted endpoint and is not yet seen by
    // mDNS, schedule a fallback probe promptly (immediate first probe after
    // initialization) so an actually-reachable host transitions to Online
    // quickly. The probe cadence is driven by updatePairedHostProbe().
    m_pairedHostProbeNextUs = 0;   // forces an immediate probe on next update
    m_pairedHostProbeIndex = 0;
}

// ---------------------------------------------------------------------------
// Paired-host HTTPS reachability fallback polling
// ---------------------------------------------------------------------------

/// Update the paired-host HTTPS reachability fallback polling cadence.
///
/// Called once per frame from update(). At a low frequency (~5 s), probes at
/// most one eligible paired host that is not currently observed via mDNS and
/// has a known endpoint. A successful probe marks the host reachable (Online)
/// and refreshes its metadata. No-op when no eligible host exists or another
/// serverinfo request is already pending.
void MoonlightApp::updatePairedHostProbe() {
    if (!m_hostManager || !m_serverInfoManager || !m_pairingManager) {
        return;
    }

    // Deadline-based polling: only act when the deadline has passed.
    const uint64_t nowUs = m_currTimestamp;
    if (nowUs < m_pairedHostProbeNextUs) {
        return;
    }

    // Advance the deadline now (do not catch up missed intervals).
    m_pairedHostProbeNextUs = nowUs + kPairedHostProbeIntervalUs;

    const std::vector<ml::Host>& hosts = m_hostManager->hosts();
    if (hosts.empty()) {
        m_pairedHostProbeIndex = 0;
        return;
    }

    // Round-robin: start from the current index and scan for one eligible host.
    const size_t start = m_pairedHostProbeIndex % hosts.size();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const size_t idx = (start + i) % hosts.size();
        const ml::Host& host = hosts[idx];

        // Eligibility: locally trusted/paired, has a known usable endpoint,
        // and is not currently being observed via mDNS.
        if (host.pairingState != ml::HostPairingState::Paired) {
            continue;
        }
        if (host.ipv4Addresses.empty() || host.httpsPort == 0) {
            continue;
        }
        // Skip hosts currently observed via mDNS (discovery takes precedence).
        // A host is "currently observed via mDNS" while its last mDNS
        // observation is fresh. Once mDNS stops and its freshness expires, the
        // fallback probe becomes eligible again.
        if (host.observedOnNetwork &&
            (nowUs / 1000) - host.lastSeenMs < kMdnsFreshAfterMs) {
            continue;
        }
        // Skip hosts already covered by an active/pending serverinfo request
        // (e.g., App List runtime polling). ServerInfoManager suppresses
        // duplicates, but we also avoid scheduling behind one.
        if (m_serverInfoManager->isPending(host.id)) {
            continue;
        }

        // Probe this host and advance the round-robin index past it.
        m_pairedHostProbeIndex = (idx + 1) % hosts.size();
        probePairedHost(host.id);
        return;
    }

    // No eligible host found; reset the round-robin index.
    m_pairedHostProbeIndex = 0;
}

/// Probe a single paired host via the authenticated HTTPS serverinfo path
/// using a short bounded timeout. Returns true if a probe was scheduled.
bool MoonlightApp::probePairedHost(ml::HostId hostId) {
    if (!m_serverInfoManager || !m_hostManager || !m_pairingManager) {
        return false;
    }

    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr) {
        return false;
    }
    // Only locally trusted hosts receive authenticated HTTPS fallback probes.
    if (host->serverId.empty() ||
        !m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        return false;
    }
    if (host->ipv4Addresses.empty()) {
        return false;
    }
    // An authenticated HTTPS probe requires a known HTTPS port. Without it we
    // do not have a usable HTTPS endpoint (the worker would fall back to the
    // HTTP bootstrap port, which is wrong for a paired HTTPS probe).
    if (host->httpsPort == 0) {
        return false;
    }
    // Skip if another serverinfo request is already pending/active for this
    // host (ServerInfoManager suppresses duplicates, but avoid queuing behind).
    if (m_serverInfoManager->isPending(hostId)) {
        return false;
    }

    // Build the authenticated HTTPS serverinfo target (reuses the exact
    // existing construction). Override the timeouts with a short bound so the
    // fallback probe does not occupy the worker for tens of seconds.
    ml::ServerInfoTarget target;
    if (!buildServerInfoTarget(hostId, target)) {
        return false;
    }
    target.connectTimeoutMs = kPairedHostProbeTimeoutMs;
    target.ioTimeoutMs = kPairedHostProbeTimeoutMs;

    // Mark the host as Pending before scheduling (matches existing behavior).
    m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Pending);

    ml::ServerInfoResult result = m_serverInfoManager->request(hostId, target);
    if (!result.succeeded()) {
        // Transient failure: do not show a modal, navigate, or mark Offline.
        // The next poll interval retries. AlreadyPending is a normal skip.
        if (result.category == ml::ServerInfoResultCategory::QueueFull ||
            result.category == ml::ServerInfoResultCategory::InvalidState) {
            m_hostManager->setServerInfoState(hostId, ml::ServerInfoQueryState::Failed);
        }
        return false;
    }

    // Track this host as a probe in flight so its completion can be logged.
    m_probeInFlightHosts.push_back(hostId);

    // --- [host-poll] scheduling diagnostic ----------------------------------
    const std::string addr = host->ipv4Addresses.empty()
                                 ? std::string("(none)")
                                 : host->ipv4Addresses[0];
    char buf[192];
    snprintf(buf, sizeof(buf),
             "[host-poll] scheduling paired HTTPS probe host=%llu addr=%s port=%u",
             static_cast<unsigned long long>(hostId),
             addr.c_str(),
             static_cast<unsigned>(host->httpsPort));
    diagPrint(buf);
    return true;
}

/// Persist a trusted host's last-known endpoint (IPv4 + discovery port + HTTPS
/// port) when it has changed. Only writes when a relevant stored value actually
/// changes. No-op for unpaired hosts.
void MoonlightApp::persistHostEndpoint(ml::HostId hostId) {
    if (!m_hostManager || !m_pairingManager) {
        return;
    }
    const ml::Host* host = m_hostManager->findHost(hostId);
    if (host == nullptr || host->serverId.empty()) {
        return;
    }
    if (!m_pairingManager->hasLocalTrustForServer(host->serverId)) {
        return;
    }
    const std::string ipv4 = host->ipv4Addresses.empty()
                                 ? std::string()
                                 : host->ipv4Addresses[0];
    m_pairingManager->updateTrustedHostEndpoint(host->serverId, ipv4,
                                                host->port, host->httpsPort);
}

/// Emit concise [host-poll] transition diagnostics (Online->Stale,
/// Stale->Offline) by comparing the current host availability against the
/// previously tracked state. Offline->Online transitions are logged in
/// processAsyncResults where the source (mdns/https) is known.
void MoonlightApp::logHostAvailabilityTransitions(uint64_t nowMs) {
    if (!m_hostManager) {
        return;
    }

    const std::vector<ml::Host>& hosts = m_hostManager->hosts();

    // First call: seed the previous-state map without logging.
    if (!m_prevHostAvailabilityValid) {
        m_prevHostAvailability.clear();
        for (const ml::Host& h : hosts) {
            m_prevHostAvailability.push_back({h.id, h.availability});
        }
        m_prevHostAvailabilityValid = true;
        return;
    }

    // Build a lookup of previous availability by HostId.
    for (const ml::Host& h : hosts) {
        ml::HostAvailability prev = h.availability;
        bool found = false;
        for (const auto& entry : m_prevHostAvailability) {
            if (entry.first == h.id) {
                prev = entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            // New host this frame — seed it without logging.
            m_prevHostAvailability.push_back({h.id, h.availability});
            continue;
        }

        // Detect Online->Stale and Stale->Offline transitions.
        if (prev == ml::HostAvailability::Online &&
            h.availability == ml::HostAvailability::Stale) {
            const uint64_t lastSeenAgeMs =
                (h.lastSeenMs != 0) ? (nowMs - h.lastSeenMs) : 0;
            const uint64_t reachAgeMs =
                (h.lastReachabilityConfirmedMs != 0)
                    ? (nowMs - h.lastReachabilityConfirmedMs)
                    : 0;
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[host-poll] id=%llu Online -> Stale lastSeenAgeMs=%llu lastReachabilityAgeMs=%llu",
                     static_cast<unsigned long long>(h.id),
                     static_cast<unsigned long long>(lastSeenAgeMs),
                     static_cast<unsigned long long>(reachAgeMs));
            diagPrint(buf);
        } else if (prev == ml::HostAvailability::Stale &&
                   h.availability == ml::HostAvailability::Offline) {
            const uint64_t lastSeenAgeMs =
                (h.lastSeenMs != 0) ? (nowMs - h.lastSeenMs) : 0;
            const uint64_t reachAgeMs =
                (h.lastReachabilityConfirmedMs != 0)
                    ? (nowMs - h.lastReachabilityConfirmedMs)
                    : 0;
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[host-poll] id=%llu Stale -> Offline lastSeenAgeMs=%llu lastReachabilityAgeMs=%llu",
                     static_cast<unsigned long long>(h.id),
                     static_cast<unsigned long long>(lastSeenAgeMs),
                     static_cast<unsigned long long>(reachAgeMs));
            diagPrint(buf);
        }

        // Update the tracked previous state for this host.
        for (auto& entry : m_prevHostAvailability) {
            if (entry.first == h.id) {
                entry.second = h.availability;
                break;
            }
        }
    }

    // Prune entries for hosts that no longer exist.
    for (auto it = m_prevHostAvailability.begin();
         it != m_prevHostAvailability.end();) {
        bool exists = false;
        for (const ml::Host& h : hosts) {
            if (h.id == it->first) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            it = m_prevHostAvailability.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Add Host ImeDialog helpers
// ---------------------------------------------------------------------------

/// Open the PS4 ImeDialog for the currently selected Add Host field.
/// Called when the user presses Cross on an editable field in the Add Host
/// overlay. Returns true if the dialog was opened.
bool MoonlightApp::openAddHostIme() {
    if (!m_ui || !m_imeDialog) {
        return false;
    }
    if (!m_ui->isAddHostOpen() || !m_ui->isAddHostImeActive()) {
        return false;
    }

    // Query which field is being edited and its current value.
    bool isPort = false;
    std::string initialText;
    if (!m_ui->getAddHostImeRequest(isPort, initialText)) {
        return false;
    }

    // Both the IP and Port fields use the numeric OSK (ORBIS_TYPE_NUMBER),
    // which presents digits plus a period (and backspace/clear/enter). The
    // IP field needs the dot for IPv4; the port field is purely numeric.
    // Both are validated client-side after return.
    const bool numericOnly = true;
    const uint32_t maxLength = isPort ? 5 : 15;   // 5 digits for port, 15 for IPv4.
    diagPrint(isPort ? "[app] opening ImeDialog for Port field"
                     : "[app] opening ImeDialog for IP field");

    // Pass the system's Enter Button Assignment so the OSK confirm/back
    // mapping follows the user's setting (Cross=accept → Western OSK,
    // Circle=accept → Japanese OSK).
    const bool crossIsAccept = m_input && m_input->isCrossAccept();
    bool opened = m_imeDialog->open(initialText, isPort ? "Port" : "IP Address",
                                    maxLength, numericOnly, crossIsAccept);
    diagPrint(opened ? "[app] ImeDialog opened"
                     : "[app] ImeDialog open FAILED");
    if (!opened) {
        // The dialog could not be opened (e.g., the application is not active
        // or the sysmodule failed to load). Reset the Add Host ImeDialog state
        // so the overlay is not left stuck waiting for a dialog that never
        // appeared.
        if (m_ui && m_ui->isInitialized()) {
            m_ui->setAddHostImeResult(false, "");
        }
    }
    return opened;
}

/// Poll the active Add Host ImeDialog lifecycle. Called once per frame from
/// update(). When the dialog closes, the returned text is routed back to the
/// Add Host overlay.
void MoonlightApp::updateAddHostIme() {
    if (!m_imeDialog || !m_imeDialog->isActive()) {
        return;
    }

    ps4::ImeDialogResult result;
    if (m_imeDialog->update(result)) {
        // Route the result back to the Add Host overlay. On cancel the
        // previous field value is left unchanged.
        if (m_ui && m_ui->isInitialized()) {
            m_ui->setAddHostImeResult(result.accepted, result.text);
        }
    }
}

// ---------------------------------------------------------------------------
// Add Host
// ---------------------------------------------------------------------------

/// Handle a RequestAddHost intent.
///
/// Adds a manual host (numeric IPv4 + bootstrap port) to the HostManager
/// registry and schedules a serverinfo query through the existing path. The
/// numeric IP + port are used only as bootstrap connection information; once
/// serverinfo succeeds, the authoritative serverId/serverName/httpsPort flow
/// through the existing reconciliation.
void MoonlightApp::handleAddHost(const std::string& ipv4, uint16_t port) {
    if (!m_hostManager || !m_serverInfoManager) {
        diagPrint("[host] manual add failed: managers unavailable");
        return;
    }
    if (ipv4.empty() || port == 0) {
        diagPrint("[host] manual add failed: invalid ip/port");
        return;
    }

    char buf[192];
    snprintf(buf, sizeof(buf), "[host] manual add requested: %s:%u",
             ipv4.c_str(), static_cast<unsigned>(port));
    diagPrint(buf);

    // Determine whether this IP already matches an existing host (i.e., it
    // was already discovered) before adding. This drives the status modal
    // message ("already discovered" vs "added to Host List").
    bool wasExisting = false;
    if (m_hostManager) {
        for (const auto& h : m_hostManager->hosts()) {
            for (const auto& addr : h.ipv4Addresses) {
                if (addr == ipv4) {
                    wasExisting = true;
                    break;
                }
            }
            if (wasExisting) {
                break;
            }
        }
    }

    // Add the manual host to the authoritative registry. HostManager assigns
    // a stable HostId and reconciles duplicates where identity permits.
    ml::HostId hostId = m_hostManager->addManualHost(ipv4, port,
                                                     m_currTimestamp / 1000);
    if (hostId == 0) {
        diagPrint("[host] manual add failed: registry rejected host");
        // Report "not found" in the status modal.
        if (m_ui && m_ui->isInitialized()) {
            m_ui->setAddHostStatus(3);   // NotFound
        }
        return;
    }

    // Track the pending manual add so the status modal can be updated when
    // the serverinfo result arrives.
    m_pendingManualAddHostId = hostId;
    m_pendingManualAddWasExisting = wasExisting;

    // Open the status modal (Checking) if not already open.
    if (m_ui && m_ui->isInitialized() && !m_ui->isAddHostStatusOpen()) {
        m_ui->openAddHostStatus(ipv4, port);
    }

    // Schedule a serverinfo query through the existing path. The manual host
    // has no serverId yet, so this is a plain-HTTP bootstrap query to the
    // supplied IP:port.
    scheduleServerInfoQuery(hostId);
}

/// Handle a CancelAddHost intent.
///
/// Aborts the pending manual host add when the user cancels the "Checking..."
/// modal. Cancels the in-flight serverinfo worker for the pending host so it
/// stops promptly (rather than running for the full connect/IO timeout), then
/// removes the temporary bootstrap placeholder (if it is still a placeholder)
/// and clears the pending manual-add state. If serverinfo already succeeded
/// and enriched the host, the host is left in place.
void MoonlightApp::handleCancelAddHost() {
    const ml::HostId pendingId = m_pendingManualAddHostId;
    if (pendingId == 0) {
        return;
    }

    // Cancel the pending serverinfo request for this host (if any). This
    // aborts the in-flight HTTP request so the worker stops promptly.
    if (m_serverInfoManager) {
        m_serverInfoManager->cancel(pendingId);
    }

    // Remove the temporary manual placeholder if it is still a bootstrap-only
    // placeholder (not observed on the network, no serverId, no serviceFqdn).
    // If serverinfo already succeeded and enriched the host, leave it in place.
    if (m_hostManager) {
        const ml::Host* host = m_hostManager->findHost(pendingId);
        if (host != nullptr &&
            !host->observedOnNetwork &&
            host->serverId.empty() &&
            host->serviceFqdn.empty()) {
            diagPrint("[host] manual add cancelled — removing temporary placeholder");
            m_hostManager->removeHost(pendingId);
            // Clear any stale serverinfo generation tracking for the removed host.
            for (auto it = m_serverInfoGenerations.begin();
                 it != m_serverInfoGenerations.end();) {
                if (it->first == pendingId) {
                    it = m_serverInfoGenerations.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // Clear the pending manual add state.
    m_pendingManualAddHostId = 0;
    m_pendingManualAddWasExisting = false;
}

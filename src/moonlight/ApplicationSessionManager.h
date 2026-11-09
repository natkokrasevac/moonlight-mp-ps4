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

// === ApplicationSessionManager.h ===========================================
//
// OpenOrbis Moonlight client — Application Launch / Resume
//
// ApplicationSessionManager orchestrates the Moonlight/Sunshine application
// launch/resume protocol for a paired Sunshine host. It accepts a selected
// ApplicationId, distinguishes launch from resume based on authoritative host
// state (Host.currentGame), builds the required Moonlight stream configuration,
// sends the /launch or /resume request through ml::HttpClient, parses the
// response safely, and produces a project-owned StreamSessionDescriptor for
// the future Stream Session module.
//
// This module does NOT call LiStartConnection(), does NOT own the active
// streaming connection, and does NOT implement RTSP/ENet/media.
//
// This header exposes only project-owned types. OpenOrbis SDK types, libHttp/libSsl
// handles, moonlight-common-c C structs, mutex/thread types, and HttpClient
// internal state must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_APPLICATION_SESSION_MANAGER_H
#define MOONLIGHT_APPLICATION_SESSION_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"   // ml::HostId
#include "ApplicationCatalog.h"  // ml::ApplicationId

namespace ml {

// Forward declaration.
class HttpClient;

// ---------------------------------------------------------------------------
// ApplicationLaunchResultCategory
// ---------------------------------------------------------------------------

/// Categories of application launch/resume operation results.
enum class ApplicationLaunchResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidHost,
    InvalidApplication,
    InvalidTarget,
    AlreadyLaunching,
    TransportFailure,
    HttpFailure,
    AuthenticationFailure,
    ProtocolFailure,
    MalformedResponse,
    SessionConfigurationFailure,
    CryptoFailure,
    HostBusy,
    Cancelled
};

// ---------------------------------------------------------------------------
// ApplicationLaunchResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by ApplicationSessionManager operations.
struct ApplicationLaunchResult {
    ApplicationLaunchResultCategory category;
    uint32_t                        nativeError;   ///< Reserved for diagnostics (0 when none).

    ApplicationLaunchResult() : category(ApplicationLaunchResultCategory::Success), nativeError(0) {}
    ApplicationLaunchResult(ApplicationLaunchResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == ApplicationLaunchResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// StreamConfiguration
// ---------------------------------------------------------------------------

/// Project-owned initial stream configuration.
///
/// This is the single authoritative source for the stream settings used both
/// during launch/resume and (later) by the future Stream Session module's
/// LiStartConnection() call. Values are chosen to match the planned PS4
/// decode/output path (1080p60 H.264 stereo).
struct StreamConfiguration {
    int width = 1920;              ///< Video width (pixels).
    int height = 1080;             ///< Video height (pixels).
    int fps = 60;                  ///< Video frame rate.
    int bitrate = 50000;           ///< Video bitrate (kbps).
    int packetSize = 1024;         ///< Max video packet size (bytes).
    /// Selected video codec: VIDEO_FORMAT_H264 (0x0001) or VIDEO_FORMAT_H265
    /// (0x0100). Set by MoonlightApp from the Codec setting + isPro().
    int videoFormat = 0x0001;
    /// Mask of supported video formats advertised to the host. Includes the
    /// selected codec bit (and H.264 as a baseline).
    int videoFormatMask = 0x0001;  ///< VIDEO_FORMAT_H264 (H.264 only).
    int audioConfiguration = 0x000302CA;  ///< AUDIO_CONFIGURATION_STEREO.
    int streamingMode = 0;         ///< STREAM_CFG_LOCAL (LAN streaming).
    int encryptionFlags = 0xFFFFFFFF;   ///< ENCFLG_ALL.
    int clientRefreshRateX100 = 6000;   ///< 60 Hz display refresh.
    int colorSpace = 1;            ///< COLORSPACE_REC_709.
    int colorRange = 0;            ///< COLOR_RANGE_LIMITED.
};

// ---------------------------------------------------------------------------
// ApplicationLaunchTarget
// ---------------------------------------------------------------------------

/// Copied data required for one launch/resume operation.
/// The worker must never retain Host* or PairingManager references.
struct ApplicationLaunchTarget {
    HostId hostId;
    ApplicationId applicationId;
    std::string applicationName;
    std::string serverId;
    std::vector<std::string> ipv4Addresses;   ///< Most-recently-observed first.
    uint16_t httpsPort;                       ///< Serverinfo-derived HTTPS port.
    uint32_t currentGame = 0;                 ///< Running app ID from serverinfo (0 = none).
    std::string serverVersion;                ///< Server app version (appversion) from serverinfo.
    int serverCodecModeSupport = 0;           ///< ServerCodecModeSupport from serverinfo.
    std::string clientUniqueId;               ///< Persistent client unique ID.
    std::string clientCertPem;                ///< Client certificate (PEM) for mTLS.
    std::string clientKeyPem;                 ///< Client private key (PEM) for mTLS.
    uint32_t generation = 0;                  ///< Request generation for stale-result handling.
    uint32_t remoteControllersBitmap = 1;     ///< Bitmask of connected local controllers (bit 0 = controller 1, etc.).

    // --- Stream settings (applied by MoonlightApp from SettingsManager) -----
    int width = 1920;                         ///< Requested stream width.
    int height = 1080;                        ///< Requested stream height.
    int fps = 60;                             ///< Requested stream frame rate.
    int bitrate = 25000;                      ///< Requested stream bitrate (kbps).
    /// Selected video codec: VIDEO_FORMAT_H264 (0x0001) or VIDEO_FORMAT_H265
    /// (0x0100). Set by MoonlightApp from the Codec setting + isPro().
    int videoFormat = 0x0001;
    /// When true, the host's speakers are muted (no audio played on the host).
    /// Maps to the GameStream `localAudioPlayMode` parameter.
    bool muteHostSpeakers = true;
    /// When true, GameStream/Sunshine "Game Optimization" (SOPS) is enabled.
    /// Maps to the GameStream `sops` parameter.
    bool optimizeGameSettings = true;
};

// ---------------------------------------------------------------------------
// StreamSessionDescriptor
// ---------------------------------------------------------------------------

/// Project-owned handoff object for the future Stream Session module.
///
/// Contains everything required to start the stream without reissuing
/// /launch, regenerating crypto, changing stream settings, or resolving a
/// hostname. The ipv4Address is a numeric IPv4 address (mandatory — the
/// ported moonlight-common-c/ENet path cannot resolve hostnames).
struct StreamSessionDescriptor {
    HostId hostId;
    ApplicationId applicationId;
    std::string serverId;
    std::string ipv4Address;       ///< Numeric IPv4 only.
    uint16_t rtspPort;             ///< RTSP port parsed from sessionUrl.
    std::string sessionUrl;        ///< rtsp:// or rtspenc:// URL from launch response.
    StreamConfiguration configuration;   ///< Exact config used for launch.
    std::vector<uint8_t> remoteInputKey; ///< 16-byte AES key (sensitive).
    std::vector<uint8_t> remoteInputIv;  ///< 16-byte AES IV (sensitive).
    std::string serverInfoAppVersion;    ///< Server app version (appversion) from serverinfo.
    int serverCodecModeSupport = 0;      ///< ServerCodecModeSupport from serverinfo.
};

// ---------------------------------------------------------------------------
// ApplicationLaunchUpdate
// ---------------------------------------------------------------------------

/// Result crossing from worker to main thread.
struct ApplicationLaunchUpdate {
    HostId hostId;
    ApplicationId applicationId;
    uint32_t generation = 0;   ///< Request generation (for stale-result handling).
    ApplicationLaunchResult result;
    bool valid;                ///< True if `session` is populated.
    StreamSessionDescriptor session;
};

// ---------------------------------------------------------------------------
// ApplicationQuitTarget
// ---------------------------------------------------------------------------

/// Copied data required for one remote application quit operation.
/// The worker must never retain Host* or PairingManager references.
struct ApplicationQuitTarget {
    HostId hostId;
    ApplicationId applicationId;   ///< The running app to quit.
    std::string serverId;
    std::vector<std::string> ipv4Addresses;   ///< Most-recently-observed first.
    uint16_t httpsPort;                       ///< Serverinfo-derived HTTPS port.
    std::string clientUniqueId;               ///< Persistent client unique ID.
    std::string clientCertPem;                ///< Client certificate (PEM) for mTLS.
    std::string clientKeyPem;                 ///< Client private key (PEM) for mTLS.
    uint32_t generation = 0;                  ///< Request generation for stale-result handling.
};

// ---------------------------------------------------------------------------
// ApplicationQuitUpdate
// ---------------------------------------------------------------------------

/// Result crossing from worker to main thread for a quit operation.
struct ApplicationQuitUpdate {
    HostId hostId;
    ApplicationId applicationId;
    uint32_t generation = 0;   ///< Request generation (for stale-result handling).
    ApplicationLaunchResult result;
    bool valid;                ///< True if the quit was accepted.
};

// ---------------------------------------------------------------------------
// ApplicationSessionManager
// ---------------------------------------------------------------------------

/// Orchestrates application launch/resume for a paired Sunshine host.
///
/// Responsibilities:
///   - Accept a selected ApplicationId.
///   - Distinguish launch from resume based on authoritative host state.
///   - Build the required Moonlight stream configuration.
///   - Send /launch or /resume through ml::HttpClient.
///   - Use the persistent client certificate/private key for authenticated HTTPS.
///   - Use only numeric IPv4 addresses.
///   - Parse launch/resume responses safely.
///   - Produce a project-owned StreamSessionDescriptor on success.
///
/// Non-responsibilities:
///   - Call LiStartConnection().
///   - Own the active Moonlight streaming connection.
///   - Implement RTSP or ENet.
///   - Decode/render video or audio.
///   - Send controller input.
///   - Retrieve App List.
///   - Perform pairing.
///   - Modify HostManager state.
///
/// Threading: ApplicationSessionManager owns exactly one worker thread for an
/// active launch/resume operation. The worker calls HttpClient synchronously
/// and communicates through a bounded update queue.
class ApplicationSessionManager {
public:
    /// Constructor performs no fallible operations.
    ApplicationSessionManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~ApplicationSessionManager();

    // Non-copyable, non-movable.
    ApplicationSessionManager(const ApplicationSessionManager&) = delete;
    ApplicationSessionManager& operator=(const ApplicationSessionManager&) = delete;
    ApplicationSessionManager(ApplicationSessionManager&&) = delete;
    ApplicationSessionManager& operator=(ApplicationSessionManager&&) = delete;

    /// Initialize the manager.
    /// `httpClient` must be a non-owning pointer to an initialized HttpClient.
    /// Must be called after HttpClient initialization.
    /// Performs no request at initialization.
    ApplicationLaunchResult initialize(HttpClient* httpClient);

    /// Start a launch/resume operation for the given target.
    /// Returns AlreadyLaunching if a launch is already active.
    ApplicationLaunchResult request(const ApplicationLaunchTarget& target);

    /// Start a remote application quit operation for the given target.
    /// Returns AlreadyLaunching if any operation (launch/resume/quit) is
    /// already active.
    ApplicationLaunchResult requestQuit(const ApplicationQuitTarget& target);

    /// Cancel the active launch/resume/quit operation (if any).
    void cancel();

    /// Pop the next completed ApplicationLaunchUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(ApplicationLaunchUpdate& out);

    /// Pop the next completed ApplicationQuitUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollQuitUpdate(ApplicationQuitUpdate& out);

    /// Returns true if a launch/resume operation is currently active.
    bool isLaunching() const;

    /// Returns true if a remote quit operation is currently active.
    bool isQuitting() const;

    /// Stop the worker, join it, clear pending session material, and release
    /// resources. Must be called before HttpClient shutdown.
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point for launch/resume.
    static void* launchWorker(void* arg);

    /// Worker thread entry point for remote quit.
    static void* quitWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_APPLICATION_SESSION_MANAGER_H

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

// === ServerInfoManager.h ====================================================
//
// OpenOrbis Moonlight client — ServerInfo / Host Capability Query
//
// ServerInfoManager asynchronously retrieves Sunshine/Moonlight serverinfo
// for discovered hosts. It queries through the existing ml::HttpClient,
// parses the server response safely, and returns project-owned host metadata
// to the main thread.
//
// This module performs host information/status retrieval only. It does not
// implement Pairing or App Catalog.
//
// This header exposes only project-owned types. OpenOrbis SDK types, libHttp/libSsl
// handles, XML-parser internals, mutex/thread types, and HttpClient internal
// state must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_SERVER_INFO_MANAGER_H
#define MOONLIGHT_SERVER_INFO_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"   // ml::HostId

namespace ml {

// Forward declaration.
class HttpClient;

// ---------------------------------------------------------------------------
// ServerInfoResultCategory
// ---------------------------------------------------------------------------

/// Categories of serverinfo operation results.
enum class ServerInfoResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidHost,
    AlreadyPending,
    QueueFull,
    TransportFailure,
    HttpFailure,
    MalformedResponse,
    UnsupportedServerInfo,
    Cancelled
};

// ---------------------------------------------------------------------------
// ServerInfoResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by ServerInfoManager operations.
struct ServerInfoResult {
    ServerInfoResultCategory category;
    uint32_t                 nativeError;   ///< Reserved for diagnostics (0 when none).

    ServerInfoResult() : category(ServerInfoResultCategory::Success), nativeError(0) {}
    ServerInfoResult(ServerInfoResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == ServerInfoResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// ServerInfoTarget
// ---------------------------------------------------------------------------

/// Copied data required for one serverinfo request.
/// The worker must never retain Host* references.
struct ServerInfoTarget {
    HostId hostId;
    std::vector<std::string> ipv4Addresses;   ///< Most-recently-observed first.
    uint16_t discoveryPort;                   ///< SRV port from discovery.
    uint16_t httpsPort;                       ///< HTTPS port (from prior serverinfo).
    std::string clientUniqueId;               ///< Client's persistent unique ID (may be empty pre-Pairing).
    bool useTls = false;                      ///< Query over HTTPS with client cert (paired hosts).
    std::string clientCertPem;                ///< Client certificate PEM (for HTTPS).
    std::string clientKeyPem;                 ///< Client private key PEM (for HTTPS).
    uint32_t generation = 0;                  ///< Request generation for stale-result handling.
    /// Per-request connect timeout (ms). Defaults to 3000. A background
    /// reachability probe may set a shorter bound so it does not occupy the
    /// worker for tens of seconds against an unreachable IP.
    uint32_t connectTimeoutMs = 3000;
    /// Per-request send/receive timeout (ms). Defaults to 5000.
    uint32_t ioTimeoutMs = 5000;
};

// ---------------------------------------------------------------------------
// ServerInfo
// ---------------------------------------------------------------------------

/// Project-owned parsed serverinfo value.
struct ServerInfo {
    std::string serverId;        ///< Stable Moonlight/Sunshine identity (uniqueid).
    std::string serverName;      ///< Server hostname.
    std::string serverVersion;   ///< Server app version (appversion).
    bool        serverReportsPaired;  ///< PairStatus == "1".
    uint16_t    httpsPort;       ///< HttpsPort (fallback 47984).
    uint32_t    currentGame;     ///< App ID of the currently running app (0 = none).
    int         serverCodecModeSupport = 0;  ///< ServerCodecModeSupport (codec capability mask).
};

// ---------------------------------------------------------------------------
// ServerInfoUpdate
// ---------------------------------------------------------------------------

/// Result crossing from worker to main thread.
struct ServerInfoUpdate {
    HostId hostId;
    uint32_t generation = 0;   ///< Request generation (for stale-result handling).
    ServerInfoResult result;
    bool valid;          ///< True if `info` is populated.
    ServerInfo info;
};

// ---------------------------------------------------------------------------
// ServerInfoManager
// ---------------------------------------------------------------------------

/// Asynchronously retrieves Sunshine/Moonlight serverinfo for discovered hosts.
///
/// Responsibilities:
///   - Query serverinfo for newly available hosts.
///   - Query through the existing ml::HttpClient.
///   - Parse the server response safely.
///   - Return project-owned host metadata to the main thread.
///   - Determine the server-reported pairing relationship where available.
///   - Retrieve stable server identity information where available.
///   - Retrieve protocol/service ports needed by later modules.
///   - Refresh server information when a previously unavailable host returns Online.
///   - Avoid repeated queries on every mDNS refresh.
///
/// Non-responsibilities:
///   - Perform mDNS.
///   - Pair hosts.
///   - Persist pairing trust.
///   - Own cryptographic certificates.
///   - Fetch App List.
///   - Modify UI directly.
///
/// Threading: ServerInfoManager owns exactly one worker thread. The worker
/// calls HttpClient synchronously and communicates through bounded queues.
class ServerInfoManager {
public:
    /// Constructor performs no fallible operations.
    ServerInfoManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~ServerInfoManager();

    // Non-copyable, non-movable.
    ServerInfoManager(const ServerInfoManager&) = delete;
    ServerInfoManager& operator=(const ServerInfoManager&) = delete;
    ServerInfoManager(ServerInfoManager&&) = delete;
    ServerInfoManager& operator=(ServerInfoManager&&) = delete;

    /// Initialize the manager and start the worker thread.
    /// `httpClient` must be a non-owning pointer to an initialized HttpClient.
    /// Must be called after HttpClient initialization.
    ServerInfoResult initialize(HttpClient* httpClient);

    /// Schedule a serverinfo query for a host.
    /// Returns AlreadyPending if the HostId is already queued or active.
    /// Returns QueueFull if the request queue is full.
    ServerInfoResult request(HostId hostId, const ServerInfoTarget& target);

    /// Cancel all pending and active requests.
    /// Fires the active cancellation token, clears queued work.
    void cancelAll();

    /// Cancel a single pending/active request for a specific HostId.
    /// Fires the active cancellation token if this host is the active request,
    /// and removes the host from the queued work. Used to abort a manual host
    /// add bootstrap when the user cancels the "Checking..." modal.
    ServerInfoResult cancel(HostId hostId);

    /// Pop the next completed ServerInfoUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(ServerInfoUpdate& out);

    /// Returns true if a query for the given HostId is queued or active.
    bool isPending(HostId hostId) const;

    /// Stop the worker, join it, and release resources.
    /// Must be called before HttpClient shutdown.
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point.
    static void* serverInfoWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_SERVER_INFO_MANAGER_H

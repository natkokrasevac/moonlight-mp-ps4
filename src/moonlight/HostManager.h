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

// === HostManager.h ==========================================================
//
// OpenOrbis Moonlight client — Authoritative Host Registry
//
// HostManager is the main-thread-owned authoritative host registry. It
// consumes ml::HostDiscoveryUpdate observations produced by DiscoveryManager,
// maintains one authoritative in-memory host record per discovered host,
// assigns stable process-local HostId values, merges repeated observations,
// tracks lastSeen using monotonic application time, and derives
// Online / Stale / Offline state.
//
// This header exposes only project-owned types. OpenOrbis SDK types, socket types,
// mutexes, and DNS data must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_HOST_MANAGER_H
#define MOONLIGHT_HOST_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "DiscoveryManager.h"   // ml::HostDiscoveryUpdate

namespace ml {

// ---------------------------------------------------------------------------
// HostId
// ---------------------------------------------------------------------------

/// A stable process-local identifier for a host record.
///
/// - Assigned by HostManager.
/// - Stable while the host record exists.
/// - Not derived from vector index.
/// - Not treated as the final persistent Moonlight host identity.
/// - 0 is reserved as the invalid/none value.
using HostId = uint64_t;

// ---------------------------------------------------------------------------
// HostAvailability
// ---------------------------------------------------------------------------

/// Freshness state derived from monotonic observation age.
enum class HostAvailability : uint8_t {
    Online,        ///< age < kHostStaleAfterMs
    Stale,         ///< kHostStaleAfterMs <= age < kHostOfflineAfterMs
    Offline,       ///< age >= kHostOfflineAfterMs
    ManuallyAdded  ///< Added manually; live target not yet paired (no mDNS).
};

// ---------------------------------------------------------------------------
// ServerInfoQueryState
// ---------------------------------------------------------------------------

/// Tracks whether serverinfo has been queried and its outcome.
/// This is application-visible host metadata, distinct from HostAvailability
/// and HostPairingState.
enum class ServerInfoQueryState : uint8_t {
    NotQueried,  ///< No serverinfo query has been scheduled yet.
    Pending,     ///< A serverinfo query is queued or in flight.
    Valid,       ///< A serverinfo query completed successfully.
    Failed       ///< A serverinfo query failed.
};

// ---------------------------------------------------------------------------
// HostPairingState
// ---------------------------------------------------------------------------

/// Application-visible pairing state for a host.
///
/// Before PairingStore exists, a server-reported paired state is handled
/// conservatively: serverReportsPaired alone is NOT sufficient evidence of
/// trusted pairing. HostPairingState::Paired requires both server
/// confirmation AND matching local trust (established by the future Pairing
/// module).
enum class HostPairingState : uint8_t {
    Unknown,   ///< Serverinfo not complete, or server says paired but no local trust.
    Unpaired,  ///< Serverinfo completed and server reports unpaired.
    Paired     ///< Server confirms paired AND local trust matches (future).
};

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

/// One authoritative in-memory host record.
///
/// This is a value type owned by HostManager. Consumers must not mutate it.
struct Host {
    HostId           id;
    std::string      displayName;
    std::string      serviceFqdn;
    std::string      hostname;
    std::vector<std::string> ipv4Addresses;   ///< Bounded, most-recently-observed first.
    uint16_t         port;
    uint64_t         lastSeenMs;              ///< Monotonic application time (ms).
    HostAvailability availability;
    /// True once this host has been observed on the network during the current
    /// session (via discovery). A restored trusted host that has not yet been
    /// discovered is treated as Offline regardless of lastSeenMs.
    bool             observedOnNetwork = false;
    /// Monotonic application time (ms) of the last successful authenticated
    /// HTTPS reachability confirmation (fallback probe). 0 = never confirmed
    /// this session. This is distinct from `observedOnNetwork` (which means
    /// "seen by discovery"): a host may be reachable via HTTPS without a recent
    /// mDNS observation. Availability aging uses the most recent of
    /// `lastSeenMs` and `lastReachabilityConfirmedMs`.
    uint64_t         lastReachabilityConfirmedMs = 0;

    // --- ServerInfo-derived metadata (populated by ServerInfoManager) -------
    std::string      serverId;                ///< Stable Moonlight/Sunshine identity (uniqueid).
    std::string      serverVersion;           ///< Server app version (appversion).
    uint16_t         httpsPort = 0;           ///< HTTPS port (HttpsPort, fallback 47984).
    bool             serverReportsPaired = false;  ///< Factual server-reported pairing status.
    uint32_t         currentGame = 0;         ///< App ID of the currently running app (0 = none).
    int              serverCodecModeSupport = 0;  ///< ServerCodecModeSupport (codec capability mask).
    ServerInfoQueryState serverInfoState = ServerInfoQueryState::NotQueried;
    HostPairingState pairingState = HostPairingState::Unknown;
};

// ---------------------------------------------------------------------------
// HostChange
// ---------------------------------------------------------------------------

/// The meaningful change caused by a single discovery update.
/// Used for low-noise transition reporting without a generic event bus.
enum class HostChange : uint8_t {
    None,          ///< No material change.
    Added,         ///< A new host record was created.
    Updated,       ///< Material metadata/address change on an existing host.
    BecameOnline,  ///< Host returned to Online (from Stale/Offline).
    BecameStale,   ///< Host transitioned to Stale.
    BecameOffline  ///< Host transitioned to Offline.
};

// ---------------------------------------------------------------------------
// HostDiscoveryResult
// ---------------------------------------------------------------------------

/// The outcome of applying one discovery observation, including the affected
/// HostId so MoonlightApp can schedule serverinfo queries without a second
/// lookup.
struct HostDiscoveryResult {
    HostChange change;
    HostId     hostId;   ///< 0 if no host was affected.
};

// ---------------------------------------------------------------------------
// HostManager — authoritative host registry
// ---------------------------------------------------------------------------

/// Main-thread-owned authoritative host registry.
///
/// Responsibilities:
///   - Own the current in-memory host registry.
///   - Create host records from first discovery.
///   - Match later observations to existing records.
///   - Update addresses, service port, hostname, and display information.
///   - Track lastSeen.
///   - Derive freshness state.
///   - Preserve offline records during the current application session.
///   - Provide stable host identifiers for application/UI references.
///   - Produce read-only snapshots/views for consumers.
///   - Support lookup by stable host identifier.
///   - Keep all registry mutation on the main thread.
///
/// This class is main-thread-only. It contains no worker threads, no mutexes,
/// no network calls, and no OpenOrbis SDK types.
class HostManager {
public:
    /// Constructor performs no fallible operations.
    HostManager();

    /// Destructor must not throw.
    ~HostManager();

    // Non-copyable, non-movable.
    HostManager(const HostManager&) = delete;
    HostManager& operator=(const HostManager&) = delete;
    HostManager(HostManager&&) = delete;
    HostManager& operator=(HostManager&&) = delete;

    /// Apply one discovery observation.
    ///
    /// `nowMs` is the current monotonic application time in milliseconds.
    /// Returns the meaningful change and the affected HostId.
    HostDiscoveryResult applyDiscoveryUpdate(const HostDiscoveryUpdate& update, uint64_t nowMs);

    /// Restore a persisted trusted host into the registry.
    ///
    /// Creates a runtime host if none exists (matched by serviceFqdn, falling
    /// back to hostname), or updates an existing host to mark it as locally
    /// trusted. The host is initialized as Offline (it has not been observed
    /// on the network this session) and is NOT treated as discovered. Later
    /// discovery observations reconcile the same entry to Online via the
    /// existing serviceFqdn correlation.
    ///
    /// Returns the HostId of the affected host, or 0 if the host could not be
    /// restored (e.g., missing identity or registry at capacity).
    ///
    /// `lastKnownIpv4`, `lastKnownDiscoveryPort`, and `lastKnownHttpsPort`
    /// are the persisted last-known endpoint (may be empty/0). When present,
    /// they are populated onto the restored host so the HTTPS reachability
    /// fallback can probe it even before mDNS observes it. The host is still
    /// initialized Offline (not yet observed/confirmed this session).
    HostId restoreTrustedHost(const std::string& serverId,
                              const std::string& serviceFqdn,
                              const std::string& hostname,
                              const std::string& lastKnownIpv4,
                              uint16_t lastKnownDiscoveryPort,
                              uint16_t lastKnownHttpsPort);

    /// Add a manually-entered host (numeric IPv4 + bootstrap port) to the
    /// registry.
    ///
    /// Creates a runtime host placeholder using the numeric IP + port as
    /// bootstrap connection information only. The host has no serverId,
    /// serviceFqdn, or Sunshine display name yet; those flow in later via
    /// applyServerInfo once the serverinfo query succeeds. The host is marked
    /// Online (it is a live manual target) and is NOT treated as discovered.
    ///
    /// If an existing host already has the same numeric IPv4 address and port,
    /// the existing host is returned instead of creating a duplicate card.
    ///
    /// Returns the HostId of the affected host, or 0 if the host could not be
    /// added (e.g., registry at capacity).
    HostId addManualHost(const std::string& ipv4, uint16_t port, uint64_t nowMs);

    /// Recompute availability for all hosts based on `nowMs`.
    /// Returns the HostIds of hosts whose availability changed.
    /// Cheap; intended to be called once per main-loop update.
    std::vector<HostId> updateAvailability(uint64_t nowMs);

    /// Apply a successful serverinfo result to a host.
    /// Updates serverId, serverVersion, httpsPort, currentGame, codec support,
    /// and pairing state. Main-thread only.
    /// `serverName` is Sunshine's configured display name (presentation
    /// metadata only). When non-empty it becomes the authoritative UI-facing
    /// `displayName`; otherwise the existing discovery fallback is preserved.
    void applyServerInfo(HostId hostId, const std::string& serverId,
                         const std::string& serverName,
                         const std::string& serverVersion, uint16_t httpsPort,
                         bool serverReportsPaired, uint32_t currentGame,
                         int serverCodecModeSupport);

    /// Set the serverinfo query state for a host.
    /// Main-thread only.
    void setServerInfoState(HostId hostId, ServerInfoQueryState state);

    /// Set the reconciled application-visible pairing state for a host.
    /// Main-thread only. Does not store trust material.
    void setPairingState(HostId hostId, HostPairingState state);

    /// Set the availability state for a host.
    /// Main-thread only. Used to transition a manually-added host to Online
    /// once it is successfully paired.
    void setAvailability(HostId hostId, HostAvailability availability);

    /// Confirm a host is reachable via an authenticated HTTPS serverinfo probe.
    /// Records `lastReachabilityConfirmedMs = nowMs` and marks the host Online.
    /// This is distinct from an mDNS observation; it is the fallback
    /// reachability evidence used when mDNS is unavailable. Main-thread only.
    void confirmReachable(HostId hostId, uint64_t nowMs);

    /// Read-only access to the authoritative host records (insertion order).
    const std::vector<Host>& hosts() const;

    /// Look up a host by stable HostId. Returns nullptr if not found.
    const Host* findHost(HostId id) const;

    /// Remove the host record with the given HostId from the registry.
    /// Returns true if a host was removed, false if no such host existed.
    /// Main-thread only.
    bool removeHost(HostId id);

    /// Remove all host records. Used at shutdown.
    void clear();

private:
    /// Normalize a service FQDN for case-insensitive matching.
    static std::string normalizeFqdn(const std::string& fqdn);

    /// Find the index of a host by normalized service FQDN. Returns -1 if absent.
    int findIndexByFqdn(const std::string& normalizedFqdn) const;

    /// Merge an observed IPv4 address into a host record (dedup + bound + order).
    void mergeAddress(Host& host, const std::string& address);

    std::vector<Host> m_hosts;
    HostId            m_nextHostId = 1;   ///< 0 reserved as invalid.
};

} // namespace ml

#endif // MOONLIGHT_HOST_MANAGER_H

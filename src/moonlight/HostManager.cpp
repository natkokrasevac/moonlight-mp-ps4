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

// === HostManager.cpp ========================================================
//
// OpenOrbis Moonlight client — Authoritative Host Registry
//
// Implements the HostManager main-thread-owned authoritative host registry:
//   - Consumes ml::HostDiscoveryUpdate observations.
//   - Maintains one authoritative in-memory host record per discovered host.
//   - Assigns stable process-local HostId values.
//   - Merges repeated observations.
//   - Tracks lastSeen using monotonic application time.
//   - Derives Online / Stale / Offline state.
//   - Preserves offline hosts in the registry.
//
// This translation unit contains no OpenOrbis SDK calls, no network calls, no
// worker threads, and no mutexes.
// ===============================================================================

#include "HostManager.h"

#include <cstdio>    // printf, fflush, snprintf
#include <cctype>    // tolower

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void hostDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

/// Diagnostic verbosity: log the per-request serverinfo "applied" line.
/// Default OFF to keep the klog quiet during normal operation.
static constexpr bool kLogServerInfoApplied = false;

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// A host is considered Stale when its last observation is at least this old.
constexpr uint64_t kHostStaleAfterMs = 12000;

/// A host is considered Offline when its last observation is at least this old.
constexpr uint64_t kHostOfflineAfterMs = 30000;

/// Maximum number of host records in the registry.
constexpr size_t kMaxHosts = 32;

/// Maximum number of IPv4 addresses retained per host.
constexpr size_t kMaxAddressesPerHost = 4;

/// Rate-limit interval for capacity diagnostics (ms).
constexpr uint64_t kCapacityLogIntervalMs = 10000;

} // namespace

namespace ml {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

HostManager::HostManager() {
}

HostManager::~HostManager() {
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string HostManager::normalizeFqdn(const std::string& fqdn) {
    std::string out;
    out.reserve(fqdn.size());
    for (char c : fqdn) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

int HostManager::findIndexByFqdn(const std::string& normalizedFqdn) const {
    for (size_t i = 0; i < m_hosts.size(); ++i) {
        if (normalizeFqdn(m_hosts[i].serviceFqdn) == normalizedFqdn) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void HostManager::mergeAddress(Host& host, const std::string& address) {
    if (address.empty()) {
        return;
    }

    // Duplicate suppression: if already present, move it to the front
    // (most recently observed).
    for (size_t i = 0; i < host.ipv4Addresses.size(); ++i) {
        if (host.ipv4Addresses[i] == address) {
            if (i != 0) {
                std::string addr = host.ipv4Addresses[i];
                host.ipv4Addresses.erase(host.ipv4Addresses.begin() + i);
                host.ipv4Addresses.insert(host.ipv4Addresses.begin(), std::move(addr));
            }
            return;
        }
    }

    // New unique address: insert at front.
    host.ipv4Addresses.insert(host.ipv4Addresses.begin(), address);

    // Bound: drop the least recently observed (last) entry if over capacity.
    if (host.ipv4Addresses.size() > kMaxAddressesPerHost) {
        host.ipv4Addresses.pop_back();
    }
}

// ---------------------------------------------------------------------------
// applyDiscoveryUpdate
// ---------------------------------------------------------------------------

HostDiscoveryResult HostManager::applyDiscoveryUpdate(const HostDiscoveryUpdate& update,
                                                      uint64_t nowMs) {
    // --- Validate the observation ------------------------------------------
    // Missing serviceFqdn: fall back to hostname if present, else ignore.
    std::string identityFqdn = update.serviceFqdn;
    if (identityFqdn.empty()) {
        identityFqdn = update.hostname;
    }
    if (identityFqdn.empty()) {
        hostDiagPrint("[host] ignoring discovery update: missing identity (serviceFqdn/hostname)");
        return HostDiscoveryResult{HostChange::None, 0};
    }
    if (update.port == 0) {
        hostDiagPrint("[host] ignoring discovery update: invalid port 0");
        return HostDiscoveryResult{HostChange::None, 0};
    }

    const std::string normalized = normalizeFqdn(identityFqdn);
    int idx = findIndexByFqdn(normalized);

    // --- IPv4 fallback matching (restored placeholders only) ---------------
    // A restored trusted host may have a persisted endpoint (IPv4) but no
    // serviceFqdn/hostname (only serverId). When mDNS later discovers the same
    // host, the FQDN match fails and would otherwise create a duplicate card.
    // Fall back to matching by the observed IPv4 address, but ONLY against an
    // existing host that is a restored placeholder (has a serverId and no
    // discovery identity). This prevents an unrelated unpaired mDNS host from
    // being absorbed into a stale restored host that happens to share an IP.
    if (idx < 0 && !update.ipv4Address.empty()) {
        for (size_t i = 0; i < m_hosts.size(); ++i) {
            const Host& h = m_hosts[i];
            if (h.serverId.empty() || !h.serviceFqdn.empty()) {
                continue;   // Not a restored placeholder — do not match by IP.
            }
            if (h.port != 0 && update.port != 0 && h.port != update.port) {
                continue;   // Different port — do not match.
            }
            for (const auto& addr : h.ipv4Addresses) {
                if (addr == update.ipv4Address) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx >= 0) {
                break;
            }
        }
    }

    // --- New host ----------------------------------------------------------
    if (idx < 0) {
        if (m_hosts.size() >= kMaxHosts) {
            // Capacity reached: ignore unknown additional hosts, rate-limited.
            static uint64_t lastCapacityLogMs = 0;
            if (nowMs - lastCapacityLogMs >= kCapacityLogIntervalMs) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[host] registry full (%zu hosts); ignoring new host '%s'",
                         m_hosts.size(), update.instanceName.c_str());
                hostDiagPrint(buf);
                lastCapacityLogMs = nowMs;
            }
            return HostDiscoveryResult{HostChange::None, 0};
        }

        Host host;
        host.id = m_nextHostId++;
        host.displayName = update.instanceName;
        host.serviceFqdn = update.serviceFqdn;
        host.hostname = update.hostname;
        host.port = update.port;
        host.lastSeenMs = nowMs;
        host.availability = HostAvailability::Online;
        host.observedOnNetwork = true;   // Genuinely discovered on the network.
        mergeAddress(host, update.ipv4Address);

        // Normalize display name fallback.
        if (host.displayName.empty()) {
            host.displayName = host.hostname.empty() ? host.serviceFqdn : host.hostname;
        }

        m_hosts.push_back(std::move(host));

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] added host id=%llu name='%s' (%s:%u)",
                 static_cast<unsigned long long>(m_hosts.back().id),
                 m_hosts.back().displayName.c_str(),
                 m_hosts.back().ipv4Addresses.empty()
                     ? "(none)"
                     : m_hosts.back().ipv4Addresses[0].c_str(),
                 static_cast<unsigned>(m_hosts.back().port));
        hostDiagPrint(buf);

        return HostDiscoveryResult{HostChange::Added, m_hosts.back().id};
    }

    // --- Existing host refresh ---------------------------------------------
    Host& host = m_hosts[static_cast<size_t>(idx)];

    // Track whether a material metadata/address change occurred.
    bool materialChange = false;

    // Update display name only if none is set yet. `displayName` is the
    // authoritative UI-facing label; once established (from discovery or from
    // a serverinfo-provided Sunshine name), a discovery instance name must not
    // clobber it. Host identity/hostname/networking are unaffected.
    if (host.displayName.empty() && !update.instanceName.empty()) {
        host.displayName = update.instanceName;
        materialChange = true;
    }
    // Update hostname if the observation provides a non-empty, different value.
    if (!update.hostname.empty() && update.hostname != host.hostname) {
        host.hostname = update.hostname;
        materialChange = true;
    }
    // Update service FQDN if the observation provides a non-empty, different value.
    if (!update.serviceFqdn.empty() && update.serviceFqdn != host.serviceFqdn) {
        host.serviceFqdn = update.serviceFqdn;
        materialChange = true;
    }
    // Update port if changed.
    if (update.port != 0 && update.port != host.port) {
        host.port = update.port;
        materialChange = true;
    }

    // Merge address (dedup + bound + order). Detect if a new unique address was added.
    const bool hadThisAddress =
        !update.ipv4Address.empty() &&
        [&]() {
            for (const auto& a : host.ipv4Addresses) {
                if (a == update.ipv4Address) return true;
            }
            return false;
        }();
    mergeAddress(host, update.ipv4Address);
    if (!hadThisAddress && !update.ipv4Address.empty()) {
        materialChange = true;
    }

    // Update lastSeen and mark Online. The host is now genuinely observed on
    // the network, so normal availability aging applies from this point.
    host.lastSeenMs = nowMs;
    host.observedOnNetwork = true;
    const HostAvailability prevAvailability = host.availability;
    host.availability = HostAvailability::Online;

    // Determine the change to report.
    if (prevAvailability != HostAvailability::Online) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] host id=%llu returned online ('%s')",
                 static_cast<unsigned long long>(host.id),
                 host.displayName.c_str());
        hostDiagPrint(buf);
        return HostDiscoveryResult{HostChange::BecameOnline, host.id};
    }

    if (materialChange) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] host id=%llu metadata updated ('%s')",
                 static_cast<unsigned long long>(host.id),
                 host.displayName.c_str());
        hostDiagPrint(buf);
        return HostDiscoveryResult{HostChange::Updated, host.id};
    }

    return HostDiscoveryResult{HostChange::None, host.id};
}

// ---------------------------------------------------------------------------
// restoreTrustedHost
// ---------------------------------------------------------------------------

HostId HostManager::restoreTrustedHost(const std::string& serverId,
                                       const std::string& serviceFqdn,
                                       const std::string& hostname,
                                       const std::string& lastKnownIpv4,
                                       uint16_t lastKnownDiscoveryPort,
                                       uint16_t lastKnownHttpsPort) {
    // Determine the correlation identity (serviceFqdn preferred, else hostname),
    // matching the identity policy used by applyDiscoveryUpdate. Persisted
    // trusted-host records may have empty serviceFqdn/hostname (only serverId
    // is guaranteed), in which case the host is created as a network-less
    // placeholder that is later reconciled to a live discovered host via
    // serverId in applyServerInfo.
    std::string identityFqdn = serviceFqdn;
    if (identityFqdn.empty()) {
        identityFqdn = hostname;
    }

    int idx = -1;
    if (!identityFqdn.empty()) {
        const std::string normalized = normalizeFqdn(identityFqdn);
        idx = findIndexByFqdn(normalized);
    }

    HostId hostId = 0;

    if (idx < 0) {
        // No existing host — create a new runtime entry.
        if (m_hosts.size() >= kMaxHosts) {
            hostDiagPrint("[host] restoreTrustedHost: registry full (ignored)");
            return 0;
        }

        Host host;
        host.id = m_nextHostId++;
        host.serverId = serverId;
        host.serviceFqdn = serviceFqdn;
        host.hostname = hostname;
        // Display-name fallback: persisted hostname, else serviceFqdn, else a
        // generic label (avoid showing a raw serverId UUID).
        if (!hostname.empty()) {
            host.displayName = hostname;
        } else if (!serviceFqdn.empty()) {
            host.displayName = serviceFqdn;
        } else {
            host.displayName = "Paired Host";
        }
        host.port = lastKnownDiscoveryPort;
        host.lastSeenMs = 0;   // Not observed on the network this session.
        host.availability = HostAvailability::Offline;
        host.observedOnNetwork = false;   // Restored, not yet discovered.
        host.lastReachabilityConfirmedMs = 0;   // Not yet confirmed this session.
        // Populate the persisted last-known endpoint so the HTTPS reachability
        // fallback can probe this host even before mDNS observes it.
        if (!lastKnownIpv4.empty()) {
            mergeAddress(host, lastKnownIpv4);
        }
        if (host.httpsPort == 0) {
            host.httpsPort = lastKnownHttpsPort;
        }
        // Local trust exists (persisted trusted-host record), so the host is
        // presented as Paired immediately. When the host comes online and
        // serverinfo completes, reconcileHostPairingState re-confirms this
        // against the server's reported pairing status.
        host.pairingState = HostPairingState::Paired;
        host.serverInfoState = ServerInfoQueryState::NotQueried;

        m_hosts.push_back(std::move(host));
        hostId = m_hosts.back().id;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] restored trusted host: id=%llu serverId='%s'",
                 static_cast<unsigned long long>(hostId),
                 serverId.c_str());
        hostDiagPrint(buf);
    } else {
        // Existing host — mark it as locally trusted without disturbing its
        // current availability (if discovery already found it, it is Online;
        // otherwise it remains Offline).
        Host& host = m_hosts[static_cast<size_t>(idx)];
        hostId = host.id;

        if (!serverId.empty() && host.serverId.empty()) {
            host.serverId = serverId;
        }
        if (host.serviceFqdn.empty() && !serviceFqdn.empty()) {
            host.serviceFqdn = serviceFqdn;
        }
        if (host.hostname.empty() && !hostname.empty()) {
            host.hostname = hostname;
        }
        if (host.displayName.empty()) {
            if (!hostname.empty()) {
                host.displayName = hostname;
            } else if (!serviceFqdn.empty()) {
                host.displayName = serviceFqdn;
            } else {
                host.displayName = "Paired Host";
            }
        }
        // Populate the persisted last-known endpoint if the host has none yet
        // (e.g., it was restored before discovery observed it). If discovery
        // already found the host, its live endpoint takes precedence.
        if (host.ipv4Addresses.empty() && !lastKnownIpv4.empty()) {
            mergeAddress(host, lastKnownIpv4);
        }
        if (host.port == 0) {
            host.port = lastKnownDiscoveryPort;
        }
        if (host.httpsPort == 0) {
            host.httpsPort = lastKnownHttpsPort;
        }
        // Local trust exists (persisted trusted-host record), so present the
        // host as Paired immediately.
        host.pairingState = HostPairingState::Paired;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] restored trusted host matched existing: id=%llu",
                 static_cast<unsigned long long>(hostId));
        hostDiagPrint(buf);
    }

    return hostId;
}

// ---------------------------------------------------------------------------
// addManualHost
// ---------------------------------------------------------------------------

HostId HostManager::addManualHost(const std::string& ipv4, uint16_t port,
                                  uint64_t nowMs) {
    if (ipv4.empty() || port == 0) {
        hostDiagPrint("[host] addManualHost: invalid ip/port (ignored)");
        return 0;
    }

    // --- Duplicate suppression by numeric IPv4 and port ---------------------
    // If an existing host already has this numeric IPv4 address and matching
    // port, return it instead of creating a duplicate card. This is safe
    // because the (IPv4, port) tuple is the bootstrap identity for a manual
    // host.
    const uint16_t reqPort = (port != 0) ? port : 47989;
    for (const Host& h : m_hosts) {
        const uint16_t hPort = (h.port != 0) ? h.port : 47989;
        if (hPort == reqPort) {
            if (!h.hostname.empty() && h.hostname == ipv4) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[host] manual add matched existing host id=%llu by hostname '%s:%u'",
                         static_cast<unsigned long long>(h.id), ipv4.c_str(),
                         static_cast<unsigned>(reqPort));
                hostDiagPrint(buf);
                return h.id;
            }
            for (const auto& addr : h.ipv4Addresses) {
                if (addr == ipv4) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "[host] manual add matched existing host id=%llu by address '%s:%u'",
                             static_cast<unsigned long long>(h.id), ipv4.c_str(),
                             static_cast<unsigned>(reqPort));
                    hostDiagPrint(buf);
                    return h.id;
                }
            }
        }
    }

    // --- Capacity check -----------------------------------------------------
    if (m_hosts.size() >= kMaxHosts) {
        hostDiagPrint("[host] addManualHost: registry full (ignored)");
        return 0;
    }

    // --- Create the manual host placeholder ---------------------------------
    Host host;
    host.id = m_nextHostId++;
    // Display-name fallback: the address/hostname until serverinfo provides a
    // Sunshine display name.
    host.displayName = ipv4;
    host.hostname = ipv4;
    host.port = port;
    host.lastSeenMs = nowMs;
    // A manually-added host is a live manual target (not mDNS-discovered) and
    // is not yet paired. It is marked ManuallyAdded so it can enter the
    // pairing flow from the Host List even though it does not respond via
    // mDNS. It transitions to Online once successfully paired.
    host.availability = HostAvailability::ManuallyAdded;
    host.observedOnNetwork = false;   // Manual target, not mDNS-discovered.
    host.serverInfoState = ServerInfoQueryState::NotQueried;
    host.pairingState = HostPairingState::Unknown;
    mergeAddress(host, ipv4);

    m_hosts.push_back(std::move(host));

    char buf[256];
    snprintf(buf, sizeof(buf),
             "[host] added manual host id=%llu (%s:%u)",
             static_cast<unsigned long long>(m_hosts.back().id),
             ipv4.c_str(), static_cast<unsigned>(port));
    hostDiagPrint(buf);

    return m_hosts.back().id;
}

// ---------------------------------------------------------------------------
// updateAvailability
// ---------------------------------------------------------------------------

std::vector<HostId> HostManager::updateAvailability(uint64_t nowMs) {
    std::vector<HostId> changed;

    for (Host& host : m_hosts) {
        // A host is considered "alive" this session if it has been observed
        // via mDNS OR confirmed reachable via an authenticated HTTPS probe.
        // A restored trusted host that has neither been observed nor confirmed
        // is always Offline, regardless of lastSeenMs. This avoids a transient
        // "Online" classification at startup before discovery/probing has had
        // a chance to find the host.
        //
        // A manually-added host is a live manual target (not mDNS-discovered)
        // and keeps its ManuallyAdded state until it is paired. It is not
        // forced Offline here.
        const bool alive = host.observedOnNetwork ||
                           host.lastReachabilityConfirmedMs != 0;
        if (!alive) {
            if (host.availability != HostAvailability::ManuallyAdded &&
                host.availability != HostAvailability::Offline) {
                host.availability = HostAvailability::Offline;
                changed.push_back(host.id);
            }
            continue;
        }

        // Availability aging uses the most recent of the mDNS observation time
        // and the HTTPS reachability confirmation time. This lets a host stay
        // Online via fallback HTTPS probes even when mDNS is unavailable.
        const uint64_t lastAliveMs =
            (host.lastSeenMs > host.lastReachabilityConfirmedMs)
                ? host.lastSeenMs
                : host.lastReachabilityConfirmedMs;
        const uint64_t age = nowMs - lastAliveMs;
        HostAvailability newState;

        if (age < kHostStaleAfterMs) {
            newState = HostAvailability::Online;
        } else if (age < kHostOfflineAfterMs) {
            newState = HostAvailability::Stale;
        } else {
            newState = HostAvailability::Offline;
        }

        if (newState != host.availability) {
            host.availability = newState;
            changed.push_back(host.id);

            switch (newState) {
            case HostAvailability::Stale:
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[host] host id=%llu became stale ('%s')",
                         static_cast<unsigned long long>(host.id),
                         host.displayName.c_str());
                hostDiagPrint(buf);
                break;
            }
            case HostAvailability::Offline:
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[host] host id=%llu became offline ('%s')",
                         static_cast<unsigned long long>(host.id),
                         host.displayName.c_str());
                hostDiagPrint(buf);
                break;
            }
            case HostAvailability::ManuallyAdded:
            {
                // Nothing needed here. Handled above
                break;
            }
            case HostAvailability::Online:
                // Online recovery is handled in applyDiscoveryUpdate; this
                // branch is only reachable if a host was previously marked
                // stale/offline and then a refresh was applied earlier in the
                // same frame before updateAvailability ran.
                break;
            }
        }
    }

    return changed;
}

// ---------------------------------------------------------------------------
// applyServerInfo
// ---------------------------------------------------------------------------

void HostManager::applyServerInfo(HostId hostId, const std::string& serverId,
                                  const std::string& serverName,
                                  const std::string& serverVersion, uint16_t httpsPort,
                                  bool serverReportsPaired, uint32_t currentGame,
                                  int serverCodecModeSupport) {
    Host* host = nullptr;
    for (Host& h : m_hosts) {
        if (h.id == hostId) {
            host = &h;
            break;
        }
    }
    if (host == nullptr) {
        hostDiagPrint("[host] applyServerInfo: unknown HostId (ignored)");
        return;
    }

    // --- Stable server identity reconciliation ------------------------------
    // If the host already has a different serverId, this is a significant
    // identity change. Preserve HostId, clear any trusted-pairing assumption,
    // and log a diagnostic.
    if (!host->serverId.empty() && !serverId.empty() &&
        host->serverId != serverId) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] host id=%llu serverId CHANGED '%s' -> '%s' (identity change)",
                 static_cast<unsigned long long>(hostId),
                 host->serverId.c_str(), serverId.c_str());
        hostDiagPrint(buf);
        // Conservative: clear any trusted-pairing assumption.
        host->pairingState = HostPairingState::Unknown;
    }

    // --- Apply serverinfo metadata ------------------------------------------
    host->serverId = serverId;
    host->serverVersion = serverVersion;
    host->httpsPort = httpsPort;
    host->currentGame = currentGame;
    host->serverCodecModeSupport = serverCodecModeSupport;
    host->serverInfoState = ServerInfoQueryState::Valid;
    // Store the factual server-reported pairing status.
    host->serverReportsPaired = serverReportsPaired;

    // --- UI-facing display name ---------------------------------------------
    // Sunshine's configured server name is presentation metadata only. When
    // non-empty it becomes the authoritative UI-facing displayName. Otherwise
    // the existing discovery fallback (instanceName/hostname/serviceFqdn) is
    // preserved. Host identity, hostname, and networking are never altered.
    if (!serverName.empty() && serverName != host->displayName) {
        host->displayName = serverName;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] display name updated: %s",
                 host->displayName.c_str());
        hostDiagPrint(buf);
    }

    // --- Transitional pairing-state mapping ---------------------------------
    // Before PairingStore exists:
    //   server says unpaired -> Unpaired
    //   server says paired but no local trust -> Unknown (conservative)
    if (!serverReportsPaired) {
        host->pairingState = HostPairingState::Unpaired;
    } else {
        // Server reports paired, but without local trusted identity this is
        // not sufficient evidence of trusted pairing.
        host->pairingState = HostPairingState::Unknown;
    }

    // --- Duplicate placeholder reconciliation ------------------------------
    // A restored trusted host may exist with only a serverId (no discovery
    // identity, because persisted records do not guarantee serviceFqdn/
    // hostname). When serverinfo confirms this host's serverId, remove any
    // other host with the same serverId that is a network-less placeholder
    // (no observed addresses), so the live discovered host is the single card.
    for (auto it = m_hosts.begin(); it != m_hosts.end();) {
        if (it->id != hostId &&
            !it->serverId.empty() &&
            it->serverId == serverId &&
            it->ipv4Addresses.empty()) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[host] removed placeholder host id=%llu (serverId matched by live host id=%llu)",
                     static_cast<unsigned long long>(it->id),
                     static_cast<unsigned long long>(hostId));
            hostDiagPrint(buf);
            it = m_hosts.erase(it);
        } else {
            ++it;
        }
    }

    // Per-request serverinfo "applied" line. Guarded by a compile-time
    // constant (default OFF) to keep the klog quiet during normal operation.
    if (kLogServerInfoApplied) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[host] host id=%llu serverinfo applied (serverId='%s' paired=%d httpsPort=%u currentGame=%u)",
                 static_cast<unsigned long long>(hostId),
                 serverId.c_str(), serverReportsPaired ? 1 : 0,
                 static_cast<unsigned>(httpsPort),
                 static_cast<unsigned>(currentGame));
        hostDiagPrint(buf);
    }
}

// ---------------------------------------------------------------------------
// setServerInfoState
// ---------------------------------------------------------------------------

void HostManager::setServerInfoState(HostId hostId, ServerInfoQueryState state) {
    for (Host& h : m_hosts) {
        if (h.id == hostId) {
            h.serverInfoState = state;
            return;
        }
    }
    hostDiagPrint("[host] setServerInfoState: unknown HostId (ignored)");
}

// ---------------------------------------------------------------------------
// setPairingState
// ---------------------------------------------------------------------------

void HostManager::setPairingState(HostId hostId, HostPairingState state) {
    for (Host& h : m_hosts) {
        if (h.id == hostId) {
            h.pairingState = state;
            return;
        }
    }
    hostDiagPrint("[host] setPairingState: unknown HostId (ignored)");
}

void HostManager::setAvailability(HostId hostId, HostAvailability availability) {
    for (Host& h : m_hosts) {
        if (h.id == hostId) {
            h.availability = availability;
            return;
        }
    }
    hostDiagPrint("[host] setAvailability: unknown HostId (ignored)");
}

void HostManager::confirmReachable(HostId hostId, uint64_t nowMs) {
    for (Host& h : m_hosts) {
        if (h.id == hostId) {
            h.lastReachabilityConfirmedMs = nowMs;
            h.availability = HostAvailability::Online;
            return;
        }
    }
    hostDiagPrint("[host] confirmReachable: unknown HostId (ignored)");
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const std::vector<Host>& HostManager::hosts() const {
    return m_hosts;
}

const Host* HostManager::findHost(HostId id) const {
    for (const Host& host : m_hosts) {
        if (host.id == id) {
            return &host;
        }
    }
    return nullptr;
}

bool HostManager::removeHost(HostId id) {
    for (auto it = m_hosts.begin(); it != m_hosts.end(); ++it) {
        if (it->id == id) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[host] removed host id=%llu ('%s')",
                     static_cast<unsigned long long>(id),
                     it->displayName.c_str());
            hostDiagPrint(buf);
            m_hosts.erase(it);
            return true;
        }
    }
    hostDiagPrint("[host] removeHost: unknown HostId (ignored)");
    return false;
}

void HostManager::clear() {
    m_hosts.clear();
    m_nextHostId = 1;
}

} // namespace ml

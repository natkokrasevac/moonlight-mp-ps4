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

// === PairingManager.h =======================================================
//
// OpenOrbis Moonlight client — Pairing / Host Identity
//
// PairingManager establishes persistent Moonlight client identity, pairs with
// a selected Sunshine host, and persists durable host trust. It uses
// ml::HttpClient for all HTTP/HTTPS transport and mbedTLS only for pairing-
// specific cryptographic operations.
//
// This header exposes only project-owned types. mbedTLS types, HttpClient
// internals, storage handles, and OpenOrbis SDK types must not appear here.
//
// OpenOrbis adaptation:
//   The persistent client identity is owned by the existing PairingStore
//   (composition root). PairingManager consumes the already-created identity
//   and delegates all trust persistence to PairingStore. It does NOT own a
//   second identity or a second persistence store.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_PAIRING_MANAGER_H
#define MOONLIGHT_PAIRING_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"   // ml::HostId
#include "PairingStore.h"  // ml::ClientIdentity, ml::TrustedHost

namespace ml {

// Forward declaration.
class HttpClient;
class PairingStore;

// ---------------------------------------------------------------------------
// PairingResultCategory
// ---------------------------------------------------------------------------

/// Categories of pairing operation results.
enum class PairingResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidHost,
    InvalidTarget,
    AlreadyPairing,
    IdentityUnavailable,
    RandomGenerationFailure,
    TransportFailure,
    ProtocolFailure,
    MalformedResponse,
    CryptoFailure,
    CertificateFailure,
    TrustVerificationFailure,
    PersistenceFailure,
    Cancelled
};

// ---------------------------------------------------------------------------
// PairingResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by PairingManager operations.
struct PairingResult {
    PairingResultCategory category;
    uint32_t              nativeError;   ///< Reserved for diagnostics (0 when none).

    PairingResult() : category(PairingResultCategory::Success), nativeError(0) {}
    PairingResult(PairingResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == PairingResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// PairingUpdateType
// ---------------------------------------------------------------------------

/// Semantic pairing progress updates emitted to the main thread.
enum class PairingUpdateType : uint8_t {
    PinReady,     ///< PIN generated and ready for display.
    Progress,     ///< Protocol stage progress.
    Succeeded,    ///< Pairing protocol + persistence succeeded.
    Failed,       ///< Pairing failed.
    Cancelled     ///< Pairing cancelled.
};

// ---------------------------------------------------------------------------
// PairingUpdate
// ---------------------------------------------------------------------------

/// A project-owned pairing update crossing from worker to main thread.
struct PairingUpdate {
    PairingUpdateType type;
    HostId            hostId;
    std::string       pin;              ///< Only for PinReady.
    std::string       statusText;       ///< Human-readable status.
    PairingResultCategory failureCategory;  ///< Only for Failed.
};

// ---------------------------------------------------------------------------
// PairingTarget
// ---------------------------------------------------------------------------

/// Copied data required for one pairing operation.
/// The worker must never retain Host* references.
struct PairingTarget {
    HostId hostId;
    std::string displayName;
    std::string serverId;
    std::vector<std::string> ipv4Addresses;   ///< Most-recently-observed first.
    uint16_t discoveryPort;                   ///< SRV port from discovery (HTTP stages).
    uint16_t httpsPort;                       ///< From serverinfo (HTTPS stage).
};

// ---------------------------------------------------------------------------
// PairingManager
// ---------------------------------------------------------------------------

/// Establishes persistent Moonlight client identity and pairs with Sunshine hosts.
///
/// Responsibilities:
///   - Consume the persistent Moonlight client identity from PairingStore.
///   - Pair a selected Online/Unpaired Sunshine host.
///   - Use serverId and protocol ports from ServerInfoManager.
///   - Generate and expose the pairing PIN.
///   - Execute the Moonlight/Sunshine pairing protocol asynchronously.
///   - Use ml::HttpClient for all HTTP/HTTPS transport.
///   - Use mbedTLS only for pairing-specific cryptographic operations.
///   - Establish and persist cryptographic host trust (via PairingStore).
///   - Support deterministic cancellation and shutdown.
///
/// Non-responsibilities:
///   - Discover hosts.
///   - Query serverinfo.
///   - Own screen navigation.
///   - Launch streams.
///   - Implement App Catalog.
///   - Own the persistent client identity or the trust store (PairingStore owns both).
///
/// Threading: PairingManager owns exactly one worker thread for an active
/// pairing operation. The worker calls HttpClient synchronously and
/// communicates through a bounded update queue.
class PairingManager {
public:
    /// Constructor performs no fallible operations.
    PairingManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~PairingManager();

    // Non-copyable, non-movable.
    PairingManager(const PairingManager&) = delete;
    PairingManager& operator=(const PairingManager&) = delete;
    PairingManager(PairingManager&&) = delete;
    PairingManager& operator=(PairingManager&&) = delete;

    /// Initialize the manager with the already-created persistent client
    /// identity and a non-owning pointer to the existing PairingStore.
    /// `httpClient` must be a non-owning pointer to an initialized HttpClient.
    /// `store` must be a non-owning pointer to an initialized PairingStore.
    /// `identity` is the already-created persistent client identity (unique ID,
    /// cert PEM, key PEM, cert DER, key DER).
    PairingResult initialize(HttpClient* httpClient,
                             PairingStore* store,
                             const ClientIdentity& identity);

    /// Cancel any active pairing, join the worker, and release resources.
    /// Must be called before HttpClient shutdown.
    void shutdown();

    /// Returns the persistent client unique ID.
    /// Valid only after initialize() succeeds.
    const std::string& clientUniqueId() const;

    /// Returns the persistent client certificate (PEM).
    /// Valid only after initialize() succeeds.
    const std::string& clientCertPem() const;

    /// Returns the persistent client private key (PEM).
    /// Valid only after initialize() succeeds.
    const std::string& clientKeyPem() const;

    /// Start a pairing operation for the given target.
    /// Returns AlreadyPairing if a pairing is already active.
    PairingResult startPairing(const PairingTarget& target);

    /// Cancel the active pairing operation (if any).
    void cancelPairing();

    /// Pop the next completed PairingUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(PairingUpdate& out);

    /// Returns true if a pairing operation is currently active.
    bool isPairing() const;

    /// Returns true if PairingStore contains valid trust for the given serverId.
    bool hasLocalTrustForServer(const std::string& serverId) const;

    /// Load all persisted trusted-host records.
    /// Returns true on success (even if empty). The records are copied into
    /// `out`. Used at startup to restore previously paired hosts into the
    /// HostManager registry.
    bool loadTrustedHosts(std::vector<TrustedHost>& out) const;

    /// Update the persisted last-known endpoint of a trusted host.
    /// Only writes when a relevant stored value actually changes. Returns true
    /// on success (including when no such record existed or nothing changed).
    bool updateTrustedHostEndpoint(const std::string& serverId,
                                   const std::string& ipv4,
                                   uint16_t discoveryPort,
                                   uint16_t httpsPort);

    /// Remove the persisted local trust/pairing record for the given serverId.
    /// Returns true on success (including when no such record existed).
    bool removeLocalTrustForServer(const std::string& serverId);

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point.
    static void* pairingWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_PAIRING_MANAGER_H

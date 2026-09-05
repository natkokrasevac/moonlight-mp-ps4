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

// === PairingStore.h =========================================================
//
// OpenOrbis Moonlight client — Persistent Client Identity & Trust
//
// PairingStore owns persistent Moonlight client identity and trusted-host
// material. It reads/writes a simple bounded versioned binary format under
// /data/moonlight/ using the OpenOrbis SDK file APIs.
//
// This header exposes only project-owned types. OpenOrbis SDK types, file handles,
// and storage internals must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_PAIRING_STORE_H
#define MOONLIGHT_PAIRING_STORE_H

#include <cstdint>
#include <string>
#include <vector>

namespace ml {

// ---------------------------------------------------------------------------
// ClientIdentity
// ---------------------------------------------------------------------------

/// Persistent Moonlight client identity.
struct ClientIdentity {
    std::string uniqueId;          ///< 16 hex chars (8 random bytes).
    std::string deviceName;        ///< "roth" (fixed).
    std::string certPem;           ///< PEM-encoded client certificate.
    std::string keyPem;            ///< PEM-encoded client private key.
    std::vector<uint8_t> certDer;  ///< DER-encoded client certificate.
    std::vector<uint8_t> keyDer;   ///< DER-encoded client private key.
};

// ---------------------------------------------------------------------------
// TrustedHost
// ---------------------------------------------------------------------------

/// Persistent trusted-host record.
/// Keyed by serverId. The peer certificate is stored as DER bytes.
///
/// Since persistence format version 2, a trusted host may also carry a
/// last-known network endpoint (IPv4 + discovery port + HTTPS port). This
/// endpoint is a best-effort association hint used by the HTTPS reachability
/// fallback so a paired host can be probed after restart even when mDNS is
/// unavailable. It is NOT proof of identity; the peer certificate remains the
/// canonical trust anchor.
struct TrustedHost {
    std::string serverId;                    ///< Stable Sunshine protocol identity.
    std::vector<uint8_t> peerCertificateDer; ///< Canonical peer identity (DER).
    std::string serviceFqdn;                 ///< Optional association hint.
    std::string hostname;                    ///< Optional association hint.

    // --- Last-known endpoint (format version 2) ----------------------------
    /// Last known numeric IPv4 address (dotted string). Empty if unknown.
    std::string lastKnownIpv4;
    /// Last known discovery/bootstrap port (SRV port). 0 if unknown.
    uint16_t lastKnownDiscoveryPort = 0;
    /// Last known HTTPS port (from serverinfo). 0 if unknown.
    uint16_t lastKnownHttpsPort = 0;
};

// ---------------------------------------------------------------------------
// PairingStore
// ---------------------------------------------------------------------------

/// Owns persistent client identity and trusted-host material.
///
/// Responsibilities:
///   - Load/save ClientIdentity.
///   - Generate a new ClientIdentity when none exists (RSA-2048 key +
///     self-signed X.509 certificate + unique ID).
///   - Load/save TrustedHost records.
///   - Version the persisted format.
///   - Validate input.
///   - Provide lookup by serverId.
///
/// Non-responsibilities:
///   - Implement pairing protocol logic.
///   - Own a generic database/repository framework.
class PairingStore {
public:
    /// Constructor performs no fallible operations.
    PairingStore();

    /// Destructor must not throw.
    ~PairingStore();

    // Non-copyable, non-movable.
    PairingStore(const PairingStore&) = delete;
    PairingStore& operator=(const PairingStore&) = delete;
    PairingStore(PairingStore&&) = delete;
    PairingStore& operator=(PairingStore&&) = delete;

    /// Initialize the store. Ensures the writable directory exists.
    /// Returns true on success.
    bool initialize();

    /// Release resources. Idempotent.
    void shutdown();

    // --- Identity -----------------------------------------------------------

    /// Load the client identity from disk. Returns true if a valid identity
    /// was loaded, false if absent or corrupt.
    bool loadIdentity(ClientIdentity& out);

    /// Save the client identity to disk. Returns true on success.
    bool saveIdentity(const ClientIdentity& identity);

    /// Returns true if a valid identity file exists on disk.
    bool hasIdentity() const;

    /// Ensure a valid client identity exists: load it if present, otherwise
    /// generate a new one (RSA-2048 key + self-signed X.509 certificate +
    /// unique ID) and persist it. Returns true on success.
    ///
    /// PairingStore owns identity generation. The generated identity uses the
    /// production-compatible format so PairingManager can consume it
    /// unchanged.
    bool ensureIdentity(ClientIdentity& out);

    /// Delete the persisted client identity and generate a fresh one.
    /// Removes the identity file from disk, then regenerates a new identity
    /// (RSA-2048 key + self-signed X.509 certificate + unique ID) and persists
    /// it. On success `out` holds the new identity. Returns true on success.
    ///
    /// Used by the persistent-storage reset flow to give the client a brand
    /// new identity (so previously paired hosts no longer recognize it).
    bool resetIdentity(ClientIdentity& out);

    // --- Trusted hosts ------------------------------------------------------

    /// Load all trusted-host records. Returns true on success (even if empty).
    bool loadTrustedHosts(std::vector<TrustedHost>& out) const;

    /// Save all trusted-host records. Returns true on success.
    bool saveTrustedHosts(const std::vector<TrustedHost>& hosts);

    /// Delete the persisted trusted-host file, clearing all trusted-host
    /// records. Returns true on success (including when no file existed).
    ///
    /// Used by the persistent-storage reset flow to forget every paired host.
    bool clearTrustedHosts();

    /// Find a trusted host by serverId. Returns true if found.
    bool findTrustedHost(const std::string& serverId, TrustedHost& out) const;

    /// Returns true if a trusted host exists for the given serverId.
    bool hasTrustedHost(const std::string& serverId) const;

    /// Add or replace a trusted host record and persist. Returns true on success.
    bool addTrustedHost(const TrustedHost& host);

    /// Update the last-known endpoint of an existing trusted host and persist.
    /// Only writes when the stored endpoint actually changes. Returns true on
    /// success (including when no such host existed or nothing changed).
    bool updateTrustedHostEndpoint(const std::string& serverId,
                                   const std::string& ipv4,
                                   uint16_t discoveryPort,
                                   uint16_t httpsPort);

    /// Remove the trusted host record for the given serverId and persist.
    /// Returns true on success (including when no such host existed).
    bool removeTrustedHost(const std::string& serverId);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_PAIRING_STORE_H

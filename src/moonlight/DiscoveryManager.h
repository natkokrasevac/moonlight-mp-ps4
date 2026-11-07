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

// === DiscoveryManager.h ======================================================
//
// OpenOrbis Moonlight client — Host Discovery subsystem
//
// DiscoveryManager is the Moonlight-specific host discovery subsystem. It owns
// one worker thread and one UDP socket, sends mDNS browse queries for the
// Sunshine/Moonlight service (_nvstream._tcp.local), parses the minimum
// required DNS/mDNS records, and delivers project-owned HostDiscoveryUpdate
// values to the main thread through a bounded synchronized queue.
//
// This header exposes only project-owned result/update types. PS4 socket
// handles, sockaddr structures, worker-thread internals, mutexes, raw DNS
// packet structures, and packet-buffer pointers are kept private to the
// implementation file.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// No OpenOrbis SDK types are exposed.
// ===============================================================================

#ifndef MOONLIGHT_DISCOVERY_MANAGER_H
#define MOONLIGHT_DISCOVERY_MANAGER_H

#include <cstdint>
#include <string>

namespace ml {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of discovery-operation results.
enum class DiscoveryResultCategory : uint8_t {
    Success,
    AlreadyRunning,
    InvalidState,
    SocketCreateFailure,
    SocketBindFailure,
    SocketConfigurationFailure,
    ThreadStartFailure
};

/// A lightweight result returned by discovery lifecycle operations.
/// Preserves a native SDK error code for diagnostics without exposing SDK
/// types in the public interface.
struct DiscoveryResult {
    DiscoveryResultCategory category;
    uint32_t                nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    DiscoveryResult() : category(DiscoveryResultCategory::Success), nativeError(0) {}
    DiscoveryResult(DiscoveryResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == DiscoveryResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// Host discovery update
// ---------------------------------------------------------------------------

/// A project-owned value describing one discovered Sunshine host observation.
/// All strings own their storage; no references into packet buffers or socket
/// structures cross the thread boundary.
///
/// This is an observation, not an authoritative host record. The later
/// HostManager module decides stable host identity and offline/expiry policy.
struct HostDiscoveryUpdate {
    std::string serviceFqdn;   ///< PTR target FQDN, e.g. "Sunshine-ABC._nvstream._tcp.local"
    std::string instanceName;  ///< Display/instance name, e.g. "Sunshine-ABC"
    std::string hostname;      ///< SRV target hostname, e.g. "sunshine.local"
    std::string ipv4Address;   ///< Printable dotted string, e.g. "192.168.1.50"
    uint16_t    port;          ///< Service port from SRV.
};

// ---------------------------------------------------------------------------
// DiscoveryManager — Moonlight-specific host discovery subsystem
// ---------------------------------------------------------------------------

/// Owns one discovery worker thread and one UDP socket.
///
/// Responsibilities:
///   - Create and own one UDP discovery socket.
///   - Bind the socket to an ephemeral local port (never 5353).
///   - Send periodic mDNS browse queries for _nvstream._tcp.local.
///   - Receive Sunshine responses with bounded blocking behavior.
///   - Parse the minimum required DNS/mDNS records (PTR, SRV, A, TXT).
///   - Produce project-owned HostDiscoveryUpdate values.
///   - Deliver updates to the main thread through a bounded queue.
///   - Shut down deterministically.
///
/// This class is main-thread owned. It does not own the authoritative host
/// registry, does not modify UI state, and does not implement pairing,
/// application catalog, or streaming.
class DiscoveryManager {
public:
    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before start() is called.
    DiscoveryManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~DiscoveryManager();

    // Non-copyable, non-movable.
    DiscoveryManager(const DiscoveryManager&) = delete;
    DiscoveryManager& operator=(const DiscoveryManager&) = delete;
    DiscoveryManager(DiscoveryManager&&) = delete;
    DiscoveryManager& operator=(DiscoveryManager&&) = delete;

    /// Create the discovery socket, bind it to an ephemeral port, and start
    /// the discovery worker thread. All fallible setup happens here.
    /// After failure the object remains safe to shut down or destroy.
    DiscoveryResult start();

    /// Signal the worker to stop, unblock any pending receive, join the
    /// worker, and close the socket. Safe after partial startup failure or
    /// repeated calls (idempotent). Must not throw.
    void stop();

    /// Alias for stop(); provided for lifecycle symmetry. Idempotent.
    void shutdown();

    /// Returns true while the discovery worker is running.
    bool isRunning() const;

    /// Pop the next pending HostDiscoveryUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(HostDiscoveryUpdate& out);

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point. Private static member so it can access the
    /// private Impl type. Passed to pthread_create.
    static void* discoveryWorker(void* arg);

    /// Non-blocking receive + parse + resolve + enqueue. Called by the worker.
    static void receiveAndProcess(int sock, uint8_t* recvBuf, size_t recvBufSize,
                                  Impl* impl);
};

} // namespace ml

#endif // MOONLIGHT_DISCOVERY_MANAGER_H

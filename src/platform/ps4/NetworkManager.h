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

// === NetworkManager.h ========================================================
//
// OpenOrbis Moonlight client — PS4 Platform Networking subsystem
//
// NetworkManager is the concrete PS4 platform networking subsystem. It owns
// the PS4 Net and NetCtl library lifecycle, provides a minimal current
// connectivity status query, and translates native PS4 network failures into
// project-owned result/status values.
//
// This header exposes only project-owned status/result types. Raw PS4 Net and
// NetCtl structs, constants, and handles are kept private to the
// implementation file.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// No OpenOrbis SDK types are exposed.
// ===============================================================================

#ifndef PS4_NETWORK_MANAGER_H
#define PS4_NETWORK_MANAGER_H

#include <cstdint>
#include <string>

namespace ps4 {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of network-operation results.
enum class NetworkResultCategory : uint8_t {
    Success,
    AlreadyInitialized,
    NetInitializationFailure,
    NetPoolCreateFailure,
    NetCtlInitializationFailure,
    StatusQueryFailure,
    InvalidState
};

/// A lightweight result returned by network lifecycle operations.
/// Preserves a native SDK error code for diagnostics without exposing SDK
/// types in the public interface.
struct NetworkResult {
    NetworkResultCategory category;
    uint32_t              nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    NetworkResult() : category(NetworkResultCategory::Success), nativeError(0) {}
    NetworkResult(NetworkResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == NetworkResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// Status types
// ---------------------------------------------------------------------------

/// Minimal current network status. Exposes only what application/platform
/// logic currently needs. Raw NetCtl constants are not exposed.
struct NetworkStatus {
    bool initialized;   ///< Net/NetCtl initialized successfully.
    bool connected;     ///< Console currently has an active network connection.
};

// ---------------------------------------------------------------------------
// NetworkManager — concrete PS4 platform networking subsystem
// ---------------------------------------------------------------------------

/// Owns the PS4 Net and NetCtl library lifecycle.
///
/// Responsibilities:
///   - Initialize the PS4 network stack (sceNetInit + sceNetPoolCreate).
///   - Initialize network-control facilities (sceNetCtlInit).
///   - Own the network lifecycle.
///   - Provide a minimal current connectivity status query.
///   - Translate native PS4 network failures into project-owned values.
///   - Shut networking down deterministically.
///
/// NetworkManager does NOT own PS4 sysmodule loading. The process-level
/// sysmodule lifecycle is owned by SysmoduleManager.
///
/// This class is main-thread owned. No worker thread is created. No protocol
/// sockets, discovery, pairing, HTTP, TLS, RTSP, or ENet are implemented.
class NetworkManager {
public:
    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before initialize() is called.
    NetworkManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~NetworkManager();

    // Non-copyable, non-movable.
    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;
    NetworkManager(NetworkManager&&) = delete;
    NetworkManager& operator=(NetworkManager&&) = delete;

    /// Initialize the PS4 network stack and NetCtl.
    /// Must be called exactly once, before status().
    /// After failure the object remains safe to shut down or destroy.
    NetworkResult initialize();

    /// Release all network resources in reverse dependency order.
    /// Safe after partial initialization, normal loop completion, or
    /// repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Returns the current network status. Safe to call at any time.
    /// If not initialized, returns initialized=false, connected=false.
    NetworkStatus status() const;

    /// Returns the console's current IPv4 address as a string (e.g.
    /// "192.168.1.100"). Returns an empty string if not initialized, the
    /// query fails, or no address has been obtained. Safe to call at any time.
    std::string ipAddress() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_NETWORK_MANAGER_H


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

// === NetworkManager.cpp ======================================================
//
// OpenOrbis Moonlight client — PS4 Platform Networking subsystem
//
// Implements the NetworkManager concrete PS4 platform networking subsystem:
//   - PS4 Net library initialization (sceNetInit + sceNetPoolCreate).
//   - PS4 NetCtl library initialization (sceNetCtlInit).
//   - Current connectivity status query (sceNetCtlGetInfo IP_ADDRESS).
//   - Deterministic shutdown in reverse dependency order.
//
// NetworkManager does NOT own PS4 sysmodule loading. The process-level
// sysmodule lifecycle is owned by SysmoduleManager.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "NetworkManager.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <cstddef>               // size_t (required by orbis/Net.h)
#include <orbis/Net.h>           // sceNetInit, sceNetTerm, sceNetPoolCreate, sceNetPoolDestroy
#include <orbis/NetCtl.h>        // sceNetCtlInit, sceNetCtlTerm, sceNetCtlGetInfo, ORBIS_NET_CTL_INFO_IP_ADDRESS

#include <cstdio>                // printf, fflush, snprintf
#include <cstring>               // memset

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other platform modules)
// ---------------------------------------------------------------------------

static void netDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Network pool size in bytes. The alternate OpenOrbis client uses 256 KiB.
constexpr int32_t kNetPoolSize = 256 * 1024;

/// Network pool name.
constexpr const char* kNetPoolName = "moonlight";

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of NetworkManager. Holds all OpenOrbis SDK network state.
struct NetworkManager::Impl {
    bool netInitialized    = false;   ///< sceNetInit succeeded.
    bool netPoolCreated    = false;   ///< sceNetPoolCreate succeeded.
    bool netCtlInitialized = false;   ///< sceNetCtlInit succeeded.
    bool initialized       = false;   ///< Full initialize() succeeded.
    int  netPoolId         = -1;      ///< sceNetPoolCreate handle.
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

NetworkManager::NetworkManager()
    : m_impl(new Impl())
{
}

NetworkManager::~NetworkManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

NetworkResult NetworkManager::initialize() {
    if (m_impl->initialized) {
        return NetworkResult(NetworkResultCategory::AlreadyInitialized);
    }

    netDiagPrint("[net] initialization start");

    // 1. Initialize the PS4 Net library.
    int32_t rc = sceNetInit();
    if (rc != 0) {
        netDiagPrint("[net] sceNetInit failed");
        return NetworkResult(NetworkResultCategory::NetInitializationFailure,
                             static_cast<uint32_t>(rc));
    }
    m_impl->netInitialized = true;
    netDiagPrint("[net] sceNetInit success");

    // 2. Create the network memory pool. Required by OpenOrbis for socket
    //    operations (the POSIX socket wrappers allocate from this pool).
    rc = sceNetPoolCreate(kNetPoolName, kNetPoolSize, 0);
    if (rc < 0) {
        netDiagPrint("[net] sceNetPoolCreate failed");
        // Roll back: terminate Net before returning failure.
        sceNetTerm();
        m_impl->netInitialized = false;
        return NetworkResult(NetworkResultCategory::NetPoolCreateFailure,
                             static_cast<uint32_t>(rc));
    }
    m_impl->netPoolId = rc;
    m_impl->netPoolCreated = true;
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[net] sceNetPoolCreate success (pool id %d)", m_impl->netPoolId);
        netDiagPrint(buf);
    }

    // 3. Initialize the PS4 NetCtl library.
    rc = sceNetCtlInit();
    if (rc != 0) {
        netDiagPrint("[net] sceNetCtlInit failed");
        // Roll back: destroy pool and terminate Net before returning failure.
        sceNetPoolDestroy(m_impl->netPoolId);
        m_impl->netPoolCreated = false;
        m_impl->netPoolId = -1;
        sceNetTerm();
        m_impl->netInitialized = false;
        return NetworkResult(NetworkResultCategory::NetCtlInitializationFailure,
                             static_cast<uint32_t>(rc));
    }
    m_impl->netCtlInitialized = true;
    netDiagPrint("[net] sceNetCtlInit success");

    // 4. Query initial connectivity state.
    //    OpenOrbis NetCtl does not expose sceNetCtlGetState. Instead we query
    //    the IP address info; a non-empty IP indicates the console has
    //    obtained an address (IP-obtained state).
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc != 0) {
        // Status query failure is not fatal to initialization. The stack is
        // up; we simply cannot report connectivity yet. Preserve the error
        // for diagnostics but continue.
        netDiagPrint("[net] initial status query failed (non-fatal)");
        // Leave connected=false; initialized=true.
    } else {
        bool connected = (info.ip_address[0] != '\0');
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[net] initial network state: %s (ip='%s')",
                 connected ? "connected" : "not connected",
                 info.ip_address);
        netDiagPrint(buf);
    }

    m_impl->initialized = true;
    netDiagPrint("[net] initialization success");
    return NetworkResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void NetworkManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Reverse dependency order: NetCtl first, then pool, then Net.
    if (m_impl->netCtlInitialized) {
        sceNetCtlTerm();
        m_impl->netCtlInitialized = false;
    }

    if (m_impl->netPoolCreated) {
        sceNetPoolDestroy(m_impl->netPoolId);
        m_impl->netPoolCreated = false;
        m_impl->netPoolId = -1;
    }

    if (m_impl->netInitialized) {
        sceNetTerm();
        m_impl->netInitialized = false;
    }

    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool NetworkManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

NetworkStatus NetworkManager::status() const {
    NetworkStatus st;
    st.initialized = isInitialized();
    st.connected   = false;

    if (!st.initialized) {
        return st;
    }

    // Query current NetCtl IP address info.
    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    int32_t rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc != 0) {
        // Query failed. Report disconnected (non-fatal). The stack remains
        // initialized; a transient query failure is not a fatal runtime error.
        return st;
    }

    st.connected = (info.ip_address[0] != '\0');
    return st;
}

// ---------------------------------------------------------------------------
// IP address
// ---------------------------------------------------------------------------

std::string NetworkManager::ipAddress() const {
    if (!isInitialized()) {
        return std::string();
    }

    OrbisNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    int32_t rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    if (rc != 0) {
        return std::string();
    }

    return std::string(info.ip_address);
}

} // namespace ps4

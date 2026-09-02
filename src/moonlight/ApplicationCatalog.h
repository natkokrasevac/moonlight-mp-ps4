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

// === ApplicationCatalog.h ===================================================
//
// OpenOrbis Moonlight client — Application Catalog / App List
//
// ApplicationCatalog retrieves, parses, owns, and presents the application
// catalog advertised by a paired Sunshine host. It uses ml::HttpClient for
// all HTTP/HTTPS transport, uses the persistent client identity and trusted
// host material owned by PairingManager, and maintains one current in-memory
// catalog for the selected host.
//
// This header exposes only project-owned types. OpenOrbis SDK types, libHttp/libSsl
// handles, XML-parser internals, mutex/thread types, and HttpClient internal
// state must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_APPLICATION_CATALOG_H
#define MOONLIGHT_APPLICATION_CATALOG_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"   // ml::HostId

namespace ml {

// Forward declaration.
class HttpClient;

// ---------------------------------------------------------------------------
// ApplicationId
// ---------------------------------------------------------------------------

/// A stable protocol application identifier.
/// The Sunshine protocol represents app IDs as decimal integers.
using ApplicationId = uint32_t;

// ---------------------------------------------------------------------------
// ApplicationCatalogState
// ---------------------------------------------------------------------------

/// The current state of the application catalog.
enum class ApplicationCatalogState : uint8_t {
    Idle,     ///< No host/catalog currently selected.
    Loading,  ///< A catalog request is active.
    Ready,    ///< Catalog populated (possibly empty).
    Failed    ///< Catalog request failed.
};

// ---------------------------------------------------------------------------
// ApplicationCatalogResultCategory
// ---------------------------------------------------------------------------

/// Categories of application-catalog operation results.
enum class ApplicationCatalogResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidHost,
    InvalidTarget,
    AlreadyLoading,
    TransportFailure,
    HttpFailure,
    AuthenticationFailure,
    TrustVerificationFailure,
    MalformedResponse,
    ResponseTooLarge,
    TooManyApplications,
    Cancelled
};

// ---------------------------------------------------------------------------
// ApplicationCatalogResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by ApplicationCatalog operations.
struct ApplicationCatalogResult {
    ApplicationCatalogResultCategory category;
    uint32_t                         nativeError;   ///< Reserved for diagnostics (0 when none).

    ApplicationCatalogResult() : category(ApplicationCatalogResultCategory::Success), nativeError(0) {}
    ApplicationCatalogResult(ApplicationCatalogResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == ApplicationCatalogResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// ApplicationInfo
// ---------------------------------------------------------------------------

/// One advertised application in the catalog.
struct ApplicationInfo {
    ApplicationId id;        ///< Stable protocol application ID.
    std::string   name;      ///< Application title.
    bool          running;   ///< Host-reported running state.
    bool          connecting = false;  ///< Client-local transient connecting state (presentation only).
    /// Client-local: this app is waiting for a quit to complete before it is
    /// launched (quit-and-launch sequencing). Presentation only.
    bool          waitingToLaunch = false;
};

// ---------------------------------------------------------------------------
// ApplicationCatalogTarget
// ---------------------------------------------------------------------------

/// Copied data required for one App List request.
/// The worker must never retain Host* or PairingManager references.
struct ApplicationCatalogTarget {
    HostId hostId;
    std::string serverId;
    std::vector<std::string> ipv4Addresses;   ///< Most-recently-observed first.
    uint16_t httpsPort;                       ///< Serverinfo-derived HTTPS port.
    std::string clientUniqueId;               ///< Persistent client unique ID.
    std::string clientCertPem;                ///< Client certificate (PEM) for mTLS.
    std::string clientKeyPem;                 ///< Client private key (PEM) for mTLS.
    uint32_t currentGame = 0;                 ///< Running app ID from serverinfo (0 = none).
    uint32_t generation = 0;                  ///< Request generation for stale-result handling.
};

// ---------------------------------------------------------------------------
// ApplicationCatalogUpdate
// ---------------------------------------------------------------------------

/// Result crossing from worker to main thread.
struct ApplicationCatalogUpdate {
    HostId hostId;
    uint32_t generation = 0;   ///< Request generation (for stale-result handling).
    ApplicationCatalogResult result;
    bool valid;                ///< True if `applications` is populated.
    std::vector<ApplicationInfo> applications;
};

// ---------------------------------------------------------------------------
// ApplicationCatalog
// ---------------------------------------------------------------------------

/// Retrieves, parses, owns, and presents the application catalog for a
/// selected paired Sunshine host.
///
/// Responsibilities:
///   - Retrieve the host's application list.
///   - Require a currently Online, valid, Paired host.
///   - Use existing serverinfo-derived connection information.
///   - Use HttpClient for all HTTP/HTTPS transport.
///   - Use authenticated HTTPS (mutual TLS).
///   - Use the persistent client identity owned by PairingManager.
///   - Parse the host's advertised applications safely.
///   - Maintain a project-owned application catalog for the selected host.
///   - Preserve application IDs exactly as required by the host protocol.
///   - Preserve and expose host-reported running state.
///
/// Non-responsibilities:
///   - Launch applications.
///   - Start Moonlight streams.
///   - Implement RTSP or ENet.
///   - Perform pairing.
///   - Modify PairingStore.
///   - Render artwork.
///   - Modify UI directly.
///
/// Threading: ApplicationCatalog owns exactly one worker thread for an active
/// catalog request. The worker calls HttpClient synchronously and
/// communicates through a bounded update queue.
class ApplicationCatalog {
public:
    /// Constructor performs no fallible operations.
    ApplicationCatalog();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~ApplicationCatalog();

    // Non-copyable, non-movable.
    ApplicationCatalog(const ApplicationCatalog&) = delete;
    ApplicationCatalog& operator=(const ApplicationCatalog&) = delete;
    ApplicationCatalog(ApplicationCatalog&&) = delete;
    ApplicationCatalog& operator=(ApplicationCatalog&&) = delete;

    /// Initialize the manager.
    /// `httpClient` must be a non-owning pointer to an initialized HttpClient.
    /// Must be called after HttpClient initialization.
    ApplicationCatalogResult initialize(HttpClient* httpClient);

    /// Start an App List retrieval for the given target.
    /// Returns AlreadyLoading if a request is already active.
    ApplicationCatalogResult request(const ApplicationCatalogTarget& target);

    /// Cancel the active App List request (if any).
    void cancel();

    /// Pop the next completed ApplicationCatalogUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(ApplicationCatalogUpdate& out);

    /// Returns true if a catalog request is currently active.
    bool isLoading() const;

    /// Clear the current catalog state. Does not cancel an active request.
    void clear();

    /// Stop the worker, join it, and release resources.
    /// Must be called before HttpClient shutdown.
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point.
    static void* catalogWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_APPLICATION_CATALOG_H

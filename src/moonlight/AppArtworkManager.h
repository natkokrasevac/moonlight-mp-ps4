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

// === AppArtworkManager.h ====================================================
//
// Moonlight client for PlayStation 4 — Application Artwork Acquisition & Cache
//
// AppArtworkManager acquires, validates, and persistently caches application
// artwork for a paired Sunshine host. It uses ml::HttpClient for all
// HTTP/HTTPS transport and the existing CPU PNG decoder
// (src/platform/ps4/PngCpuDecode.*) for validation and decoding.
//
// This module implements artwork acquisition and caching ONLY. It does not
// integrate artwork into the App List / App Grid renderers; renderer/UI
// concerns are deliberately kept out of this manager.
//
// This header exposes only project-owned types. OpenOrbis SDK types, libHttp/libSsl
// handles, mutex/thread types, and HttpClient internal state must not appear
// here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_APP_ARTWORK_MANAGER_H
#define MOONLIGHT_APP_ARTWORK_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"          // ml::HostId
#include "ApplicationCatalog.h"   // ml::ApplicationId
#include "platform/ps4/PngCpuDecode.h"  // ps4::CpuPngImage (project-owned CPU image)

namespace ml {

// Forward declaration.
class HttpClient;

// ---------------------------------------------------------------------------
// AppArtworkResultCategory
// ---------------------------------------------------------------------------

/// Categories of app-artwork operation results.
enum class AppArtworkResultCategory : uint8_t {
    Success,          ///< Artwork acquired (from cache or network).
    InvalidState,     ///< Manager not initialized.
    InvalidTarget,    ///< Target failed validation.
    AlreadyPending,   ///< A request for the same identity is already queued/active.
    QueueFull,        ///< The bounded request queue is full.
    CacheReadFailure, ///< Cached file could not be read (corrupt/stale).
    TransportFailure, ///< All addresses failed with transport errors.
    HttpFailure,      ///< HTTP status was not 200.
    DecodeFailure,    ///< Fetched bytes did not decode as a valid PNG.
    StorageFailure,   ///< Cache persistence failed (image still usable).
    Cancelled         ///< Request cancelled (shutdown).
};

// ---------------------------------------------------------------------------
// AppArtworkTarget
// ---------------------------------------------------------------------------

/// Copied data required for one artwork request.
/// The worker must never retain Host* or PairingManager references.
struct AppArtworkTarget {
    HostId hostId = 0;
    std::string serverId;                 ///< Stable Sunshine identity (uniqueid).
    ApplicationId appId = 0;              ///< Numeric application ID from /applist.
    std::vector<std::string> ipv4Addresses;  ///< Most-recently-observed first.
    uint16_t httpsPort = 0;               ///< Serverinfo-derived HTTPS port.
    std::string clientUniqueId;           ///< Persistent client unique ID.
    std::string clientCertPem;            ///< Client certificate (PEM) for mTLS.
    std::string clientKeyPem;             ///< Client private key (PEM) for mTLS.
    uint64_t generation = 0;              ///< Request generation for stale-result handling.
};

// ---------------------------------------------------------------------------
// AppArtworkResult
// ---------------------------------------------------------------------------

/// Result crossing from worker to main thread.
///
/// On success (`category == Success`) `image` holds the decoded CPU RGBA8
/// image. `fromCache` is true when the image came from the persistent cache.
/// `cacheStored` is true when the image was also persisted to the cache
/// (always true for a cache hit; for a network fetch it is true only when
/// persistence succeeded). A network fetch that decodes successfully but
/// fails to persist still returns Success with `cacheStored == false` — the
/// image remains usable for the current process.
struct AppArtworkResult {
    AppArtworkResultCategory category = AppArtworkResultCategory::Success;
    HostId hostId = 0;
    ApplicationId appId = 0;
    uint64_t generation = 0;
    bool fromCache = false;      ///< Image came from the persistent cache.
    bool cacheStored = false;    ///< Image was persisted to the cache.
    ps4::CpuPngImage image;      ///< Decoded CPU RGBA8 image on success.
};

// ---------------------------------------------------------------------------
// AppArtworkManager
// ---------------------------------------------------------------------------

/// Acquires, validates, and persistently caches application artwork for a
/// paired Sunshine host.
///
/// Responsibilities:
///   - Accept artwork requests for apps belonging to a known host.
///   - Check a persistent PNG cache keyed by (serverId, appId).
///   - Load and validate cached artwork through the CPU PNG decoder.
///   - Fetch missing artwork from Sunshine over HTTPS.
///   - Serialize network requests through one worker thread.
///   - Persist successfully fetched PNGs.
///   - Expose completed artwork results through a bounded result queue.
///
/// Non-responsibilities:
///   - Render artwork.
///   - Integrate artwork into AppListRenderer / AppGridRenderer.
///   - Scale / aspect-fit artwork (belongs to renderers).
///   - Implement a generic asset/disk-cache framework.
///   - Modify UiManager, renderers, HostManager, or ApplicationCatalog.
///
/// Threading: AppArtworkManager owns exactly one worker thread. The worker
/// calls HttpClient synchronously and communicates through bounded request
/// and result queues. Results are consumed on the main thread.
class AppArtworkManager {
public:
    /// Constructor performs no fallible operations.
    AppArtworkManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~AppArtworkManager();

    // Non-copyable, non-movable.
    AppArtworkManager(const AppArtworkManager&) = delete;
    AppArtworkManager& operator=(const AppArtworkManager&) = delete;
    AppArtworkManager(AppArtworkManager&&) = delete;
    AppArtworkManager& operator=(AppArtworkManager&&) = delete;

    /// Initialize the manager and start the worker thread.
    /// `httpClient` must be a non-owning pointer to an initialized HttpClient.
    /// Must be called after HttpClient initialization.
    bool initialize(HttpClient* httpClient);

    /// Stop the worker, join it, and release resources.
    /// Must be called before HttpClient shutdown.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Schedule an artwork request for the given target.
    ///
    /// Non-blocking. Validates manager state and the target, rejects duplicate
    /// pending requests for the same (hostId, appId, generation), enqueues
    /// into the bounded request queue, and returns immediately. No network or
    /// disk I/O occurs on the calling (main) thread.
    AppArtworkResultCategory requestArtwork(const AppArtworkTarget& target);

    /// Pop the next completed AppArtworkResult into `out`.
    /// Returns true if a result was available, false if the queue is empty.
    /// Results must be consumed on the main thread.
    bool pollResult(AppArtworkResult& out);

    /// Remove the persistent artwork cache directory for the given serverId.
    /// Returns true on success (including when no such directory existed).
    /// Intended for future host-removal/unpair integration.
    bool removeHostCache(const std::string& serverId);

    /// Remove the entire persistent artwork cache root directory
    /// (/data/moonlight/artwork) recursively. Only that directory is removed;
    /// /data/moonlight/ and /data/ are left intact. The cache is recreated
    /// automatically when new artwork is fetched. Returns true on success
    /// (including when no cache directory existed). Used by the
    /// persistent-storage reset flow.
    bool clearAllCache();

private:
    struct Impl;
    Impl* m_impl;

    /// Worker thread entry point.
    static void* artworkWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_APP_ARTWORK_MANAGER_H

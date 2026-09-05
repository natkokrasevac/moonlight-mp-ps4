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

// === AppArtworkManager.cpp ==================================================
//
// Moonlight client for PlayStation 4 — Application Artwork Acquisition & Cache
//
// Implements the AppArtworkManager concrete artwork acquisition subsystem:
//   - One persistent worker thread consuming a bounded request queue.
//   - Synchronous HttpClient calls for the /appasset endpoint.
//   - Persistent PNG cache under /data/moonlight/artwork/<serverId>/<appId>.png.
//   - Cache validation through the existing CPU PNG decoder.
//   - Bounded result queue to the main thread.
//   - Deterministic cancellation and shutdown.
//
// OpenOrbis adaptation:
//   Uses POSIX pthread (pthread_create/join/mutex/cond) matching the proven
//   OpenOrbis threading path used by DiscoveryManager/ServerInfoManager. The
//   SCE reference's ScePthread* types are replaced with their POSIX
//   equivalents. File/directory operations use the OpenOrbis libkernel
//   (sceKernelMkdir/Stat/Open/Getdents/Close/Unlink/Rmdir) with the standard
//   POSIX dirent/stat/fcntl types and macros.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "AppArtworkManager.h"

#include "HttpClient.h"

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <orbis/libkernel.h>   // sceKernelUsleep, sceKernelGetProcessTime, sceKernelReadTsc,
                               // sceKernelMkdir, sceKernelStat, sceKernelOpen, sceKernelGetdents,
                               // sceKernelClose, sceKernelUnlink, sceKernelRmdir

#include <dirent.h>            // struct dirent
#include <fcntl.h>             // O_RDONLY, O_DIRECTORY
#include <sys/stat.h>          // S_ISDIR

#include <cstdio>              // printf, fflush, snprintf, fopen, fwrite, fclose, remove, rename
#include <cstring>             // memset, memcmp
#include <cstdlib>             // strtoul
#include <deque>
#include <atomic>
#include <tuple>               // std::tuple, std::make_tuple
#include <utility>             // std::pair
#include <vector>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void awDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Request queue capacity.
constexpr size_t kRequestQueueCapacity = 32;

/// Result queue capacity.
constexpr size_t kResultQueueCapacity = 32;

/// Artwork cache root directory.
constexpr const char* kArtworkRootDir = "/data/moonlight/artwork";

/// Default HTTPS port fallback (per Moonlight reference).
constexpr uint16_t kDefaultHttpsPort = 47984;

/// Maximum number of IPv4 addresses to try per request.
constexpr size_t kMaxAddressAttempts = 4;

/// Maximum serverId length (Sunshine uniqueid is a 16-hex-char string).
constexpr size_t kMaxServerIdLen = 64;

/// Maximum artwork response body size (4 MiB, matching HttpClient's ceiling).
constexpr size_t kMaxArtworkBodySize = 4 * 1024 * 1024;

/// Maximum decoded artwork image size (bytes). Bounds the RGBA8 decode buffer
/// so an oversized host-provided image cannot exhaust the process heap (which
/// is 128 MiB). 32 MiB allows up to ~2830x2830 RGBA8, generous for artwork.
constexpr size_t kMaxDecodedImageBytes = 32 * 1024 * 1024;

/// Worker idle sleep in microseconds (when no work and not using cond var).
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

/// Worker wait timeout in microseconds for cond var.
constexpr uint64_t kWorkerWaitTimeoutUs = 1000 * 1000;  // 1 second

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of AppArtworkManager.
struct AppArtworkManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- HttpClient dependency (non-owning) ---------------------------------
    HttpClient* httpClient = nullptr;

    // --- Request queue ------------------------------------------------------
    pthread_mutex_t requestMutex;
    bool requestMutexInit = false;
    std::deque<AppArtworkTarget> requestQueue;

    // --- Result queue -------------------------------------------------------
    pthread_mutex_t resultMutex;
    bool resultMutexInit = false;
    std::deque<AppArtworkResult> resultQueue;

    // --- Worker wake --------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;

    // --- Active request state ----------------------------------------------
    std::atomic<bool> activeRequest{false};
    // A fresh cancellation token is created per request. Guarded by
    // requestMutex so shutdown() can signal the active one.
    HttpCancellationToken* activeCancellation = nullptr;

    // --- Pending request identities (for duplicate suppression) -------------
    // Guarded by requestMutex. Each entry is (hostId, appId, generation).
    std::vector<std::tuple<HostId, ApplicationId, uint64_t>> pendingIdentities;
};

// ---------------------------------------------------------------------------
// Path / validation helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

/// Validate a serverId for use as a cache directory name.
///
/// The serverId is the Sunshine serverinfo `uniqueid`. Its exact character set
/// is not guaranteed to be hex-only, so we enforce a bounded length and reject
/// only characters that could enable path traversal or embedded path
/// separators ('/', '\\', and '.' — the latter also blocks ".."). This keeps
/// the value safe as a single directory-name component while accepting the
/// real-world serverId formats.
bool isValidServerId(const std::string& serverId) {
    if (serverId.empty() || serverId.size() > kMaxServerIdLen) {
        return false;
    }
    for (char c : serverId) {
        if (c == '/' || c == '\\' || c == '.') {
            return false;
        }
    }
    return true;
}

/// Build the per-server artwork cache directory path.
/// Caller must have validated serverId.
std::string serverCacheDir(const std::string& serverId) {
    std::string path = kArtworkRootDir;
    path += "/";
    path += serverId;
    return path;
}

/// Build the final cache file path for an app.
/// Caller must have validated serverId.
std::string cacheFilePath(const std::string& serverId, ApplicationId appId) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(appId));
    std::string path = serverCacheDir(serverId);
    path += "/";
    path += buf;
    path += ".png";
    return path;
}

/// Build the temporary cache file path for an app.
/// Caller must have validated serverId.
std::string cacheTempFilePath(const std::string& serverId, ApplicationId appId) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(appId));
    std::string path = serverCacheDir(serverId);
    path += "/";
    path += buf;
    path += ".png.tmp";
    return path;
}

/// Ensure the artwork root and per-server cache directory exist.
/// Returns true on success (including when they already exist).
bool ensureCacheDirectories(const std::string& serverId) {
    // Create the root directory (idempotent). sceKernelMkdir returns a
    // negative error (e.g. EEXIST) if the directory already exists, which is
    // fine; we verify usability with sceKernelStat below.
    sceKernelMkdir(kArtworkRootDir, 0777);
    // Create the per-server directory (idempotent).
    const std::string dir = serverCacheDir(serverId);
    sceKernelMkdir(dir.c_str(), 0777);
    // Verify the per-server directory is usable.
    OrbisKernelStat st;
    if (sceKernelStat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    return true;
}

/// Write a byte buffer to a file. Returns true on success.
bool writeWholeFile(const char* path, const uint8_t* data, size_t size) {
    if (path == nullptr || data == nullptr || size == 0) {
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    size_t written = fwrite(data, 1, size, f);
    if (fflush(f) != 0) {
        fclose(f);
        return false;
    }
    if (fclose(f) != 0) {
        return false;
    }
    return written == size;
}

/// Read the PNG IHDR width/height from a file without decoding the image.
/// Returns true on success and fills `outWidth`/`outHeight`. This is used to
/// bound the decoded image size before calling decodePngToCpu, so an oversized
/// host-provided image cannot exhaust the process heap.
bool pngDimensionsFromFile(const char* path, uint32_t& outWidth, uint32_t& outHeight) {
    if (path == nullptr) {
        return false;
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }

    // PNG signature (8 bytes) + IHDR chunk header (4 len + 4 "IHDR") + 8 bytes
    // of width/height = 24 bytes total.
    uint8_t header[24];
    size_t read = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (read != sizeof(header)) {
        return false;
    }

    // Validate the PNG signature.
    static const uint8_t kPngSignature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (memcmp(header, kPngSignature, 8) != 0) {
        return false;
    }
    // Validate the IHDR chunk type.
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') {
        return false;
    }

    // Width and height are big-endian uint32 at offsets 16 and 20.
    outWidth = (static_cast<uint32_t>(header[16]) << 24) |
               (static_cast<uint32_t>(header[17]) << 16) |
               (static_cast<uint32_t>(header[18]) << 8) |
               static_cast<uint32_t>(header[19]);
    outHeight = (static_cast<uint32_t>(header[20]) << 24) |
                (static_cast<uint32_t>(header[21]) << 16) |
                (static_cast<uint32_t>(header[22]) << 8) |
                static_cast<uint32_t>(header[23]);
    return true;
}

/// Recursively remove a directory and all of its contents.
///
/// Walks the directory with sceKernelOpen + sceKernelGetdents, removing each
/// entry (files via sceKernelUnlink, subdirectories via recursive calls), then
/// removes the directory itself via sceKernelRmdir. Returns true on success
/// (including when the directory does not exist).
bool removeDirectoryRecursive(const std::string& dir) {
    // Open the directory for listing. The OpenOrbis SDK does not provide the
    // POSIX dirent.h / opendir / readdir / closedir API; directory listing is
    // done via sceKernelOpen + sceKernelGetdents + sceKernelClose.
    int fd = sceKernelOpen(dir.c_str(), O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        // Directory does not exist or cannot be opened — nothing to remove.
        return true;
    }

    // Read directory entries in batches. The buffer must be aligned for struct
    // dirent records (which contain a uint32 first field).
    alignas(alignof(struct dirent)) char buf[4096];
    for (;;) {
        int n = sceKernelGetdents(fd, buf, static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            break;
        }
        int off = 0;
        while (off < n) {
            struct dirent* de = reinterpret_cast<struct dirent*>(buf + off);
            const char* name = de->d_name;
            if (name[0] != '\0' && name[0] != '.') {
                std::string child = dir;
                child += "/";
                child += name;
                // Determine whether the entry is a directory.
                OrbisKernelStat st;
                if (sceKernelStat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    removeDirectoryRecursive(child);
                } else {
                    sceKernelUnlink(child.c_str());
                }
            }
            off += de->d_reclen;
        }
    }
    sceKernelClose(fd);

    // Remove the (now empty) directory.
    sceKernelRmdir(dir.c_str());
    return true;
}

/// Generate a random UUID string (36 chars + null).
/// Uses sceKernelGetProcessTime and sceKernelReadTsc as a simple entropy
/// source. This is NOT cryptographically secure; it is only used for the
/// request `uuid` parameter which is a per-request nonce.
void generateUuid(char* outBuf, size_t bufSize) {
    if (outBuf == nullptr || bufSize < 37) {
        return;
    }

    // Use process time and a counter for pseudo-randomness.
    static uint64_t counter = 0;
    uint64_t t = sceKernelGetProcessTime();
    uint64_t c = ++counter;
    uint64_t tsc = sceKernelReadTsc();

    // Format as a UUID-like string (not cryptographically secure).
    snprintf(outBuf, bufSize,
             "%08x-%04x-%04x-%04x-%012llx",
             static_cast<unsigned>((t & 0xFFFFFFFF) ^ (c & 0xFFFFFFFF) ^ (tsc & 0xFFFFFFFF)),
             static_cast<unsigned>((t >> 32) & 0xFFFF),
             static_cast<unsigned>((c >> 16) & 0xFFFF),
             static_cast<unsigned>((t >> 16) & 0xFFFF),
             static_cast<unsigned long long>(c & 0xFFFFFFFFFFFFULL));
}

} // namespace

// ---------------------------------------------------------------------------
// Worker thread entry
// ---------------------------------------------------------------------------

void* AppArtworkManager::artworkWorker(void* arg) {
    AppArtworkManager::Impl* impl =
        static_cast<AppArtworkManager::Impl*>(arg);

    awDiagPrint("[artwork] worker started");

    while (!impl->stopRequested.load()) {
        // --- Wait for work (non-busy-spin) ---------------------------------
        AppArtworkTarget target;
        bool hasWork = false;

        // Create a fresh cancellation token for this request.
        HttpCancellationToken requestCancellation;

        // Atomically pop a request and publish active state so duplicate
        // suppression and cancellation see a consistent view.
        pthread_mutex_lock(&impl->requestMutex);
        if (!impl->requestQueue.empty()) {
            target = std::move(impl->requestQueue.front());
            impl->requestQueue.pop_front();
            hasWork = true;

            // Remove the identity from the pending set (it is now active).
            for (size_t i = 0; i < impl->pendingIdentities.size(); ++i) {
                const auto& id = impl->pendingIdentities[i];
                if (std::get<0>(id) == target.hostId &&
                    std::get<1>(id) == target.appId &&
                    std::get<2>(id) == target.generation) {
                    impl->pendingIdentities.erase(
                        impl->pendingIdentities.begin() + i);
                    break;
                }
            }

            impl->activeRequest = true;
            impl->activeCancellation = &requestCancellation;
        }
        pthread_mutex_unlock(&impl->requestMutex);

        if (!hasWork) {
            // Sleep briefly and check again. This is a simple non-busy-spin
            // model compatible with the project's existing worker patterns.
            sceKernelUsleep(kWorkerIdleSleepUs);
            continue;
        }

        // --- Process the request -------------------------------------------
        AppArtworkResult result;
        result.hostId = target.hostId;
        result.appId = target.appId;
        result.generation = target.generation;
        result.fromCache = false;
        result.cacheStored = false;

        // Check cancellation before starting.
        if (impl->stopRequested.load() || requestCancellation.cancelled()) {
            result.category = AppArtworkResultCategory::Cancelled;
        } else {
            // Validate the serverId (defensive; requestArtwork already did).
            if (!isValidServerId(target.serverId)) {
                result.category = AppArtworkResultCategory::InvalidTarget;
            } else {
                // --- Cache lookup ------------------------------------------
                const std::string finalPath = cacheFilePath(target.serverId, target.appId);
                bool cacheHit = false;

                // Try to decode the cached file. A cached file is trusted only
                // if it decodes as a valid PNG. The decoded image size is
                // bounded before decoding (see pngDimensionsFromFile) so an
                // oversized image cannot exhaust the heap.
                ps4::CpuPngImage cached;
                uint32_t cachedW = 0, cachedH = 0;
                const bool cachedSizeOk =
                    pngDimensionsFromFile(finalPath.c_str(), cachedW, cachedH) &&
                    static_cast<size_t>(cachedW) * cachedH * 4 <= kMaxDecodedImageBytes;
                if (cachedSizeOk && ps4::decodePngToCpu(finalPath.c_str(), cached)) {
                    cacheHit = true;
                    result.image = std::move(cached);
                    result.fromCache = true;
                    result.cacheStored = true;
                    result.category = AppArtworkResultCategory::Success;
                    {
                        char buf[192];
                        snprintf(buf, sizeof(buf),
                                 "[artwork] cache hit host=%llu app=%u %ux%u",
                                 static_cast<unsigned long long>(target.hostId),
                                 static_cast<unsigned>(target.appId),
                                 static_cast<unsigned>(result.image.width),
                                 static_cast<unsigned>(result.image.height));
                        awDiagPrint(buf);
                    }
                } else {
                    // Cache miss or corrupt/stale cache file. If the file
                    // exists but failed to decode, remove it so it does not
                    // shadow a future successful fetch.
                    remove(finalPath.c_str());
                    {
                        char buf[192];
                        snprintf(buf, sizeof(buf),
                                 "[artwork] cache miss host=%llu app=%u",
                                 static_cast<unsigned long long>(target.hostId),
                                 static_cast<unsigned>(target.appId));
                        awDiagPrint(buf);
                    }
                }

                // --- Network fetch (cache miss only) -----------------------
                if (!cacheHit) {
                    bool requestCompleted = false;
                    size_t attempts = 0;

                    for (const auto& addr : target.ipv4Addresses) {
                        if (impl->stopRequested.load() ||
                            requestCancellation.cancelled()) {
                            result.category = AppArtworkResultCategory::Cancelled;
                            requestCompleted = true;
                            break;
                        }
                        if (attempts >= kMaxAddressAttempts) {
                            break;
                        }
                        ++attempts;

                        // --- Build the HttpRequest -------------------------
                        HttpRequest httpReq;
                        httpReq.method = HttpMethod::Get;
                        httpReq.address = addr;
                        httpReq.port = (target.httpsPort != 0)
                                           ? target.httpsPort
                                           : kDefaultHttpsPort;

                        // Build the target: /appasset?appid=<appId>&uniqueid=...&uuid=...
                        char uuidBuf[37];
                        generateUuid(uuidBuf, sizeof(uuidBuf));

                        std::string targetStr = "/appasset?appid=";
                        char appIdBuf[16];
                        snprintf(appIdBuf, sizeof(appIdBuf), "%u",
                                 static_cast<unsigned>(target.appId));
                        targetStr += appIdBuf;
                        targetStr += "&uniqueid=";
                        targetStr += target.clientUniqueId;
                        targetStr += "&uuid=";
                        targetStr += uuidBuf;
                        httpReq.target = targetStr;

                        // HTTPS with client certificate authentication
                        // (mutual TLS), consistent with the applist/serverinfo
                        // requests. The host is already reconciled as Paired,
                        // so the client certificate is the trust anchor.
                        httpReq.useTls = true;
                        httpReq.tls.trustMode = HttpTlsTrustMode::Unverified;
                        httpReq.tls.clientCertPem = target.clientCertPem;
                        httpReq.tls.clientKeyPem = target.clientKeyPem;
                        httpReq.connectTimeoutMs = 3000;
                        httpReq.ioTimeoutMs = 5000;

                        {
                            char buf[192];
                            snprintf(buf, sizeof(buf),
                                     "[artwork] fetch host=%llu app=%u addr='%s' port=%u",
                                     static_cast<unsigned long long>(target.hostId),
                                     static_cast<unsigned>(target.appId),
                                     addr.c_str(),
                                     static_cast<unsigned>(httpReq.port));
                            awDiagPrint(buf);
                        }

                        // --- Execute the request ---------------------------
                        HttpResponse httpResp;
                        HttpResult httpResult = impl->httpClient->perform(
                            httpReq, httpResp, &requestCancellation);

                        if (impl->stopRequested.load() ||
                            requestCancellation.cancelled()) {
                            result.category = AppArtworkResultCategory::Cancelled;
                            requestCompleted = true;
                            break;
                        }

                        if (!httpResult.succeeded()) {
                            // Transport-class failure — try next address.
                            continue;
                        }

                        // --- HTTP status check -----------------------------
                        if (httpResp.statusCode != 200) {
                            result.category = AppArtworkResultCategory::HttpFailure;
                            requestCompleted = true;
                            break;
                        }

                        // --- Non-empty body check --------------------------
                        if (httpResp.body.empty()) {
                            result.category = AppArtworkResultCategory::HttpFailure;
                            requestCompleted = true;
                            break;
                        }
                        if (httpResp.body.size() > kMaxArtworkBodySize) {
                            result.category = AppArtworkResultCategory::HttpFailure;
                            requestCompleted = true;
                            break;
                        }

                        {
                            char buf[192];
                            snprintf(buf, sizeof(buf),
                                     "[artwork] fetch success host=%llu app=%u bytes=%zu",
                                     static_cast<unsigned long long>(target.hostId),
                                     static_cast<unsigned>(target.appId),
                                     httpResp.body.size());
                            awDiagPrint(buf);
                        }

                        // --- Persist + decode ------------------------------
                        // Write the fetched bytes to a temp file, decode it
                        // through the existing CPU PNG decoder, and only then
                        // atomically rename it to the final cache file. This
                        // guarantees a partially-written or invalid PNG is
                        // never left as the final cache entry.
                        if (!ensureCacheDirectories(target.serverId)) {
                            // Cannot create the cache directory. The fetched
                            // image is still usable for this process, but it
                            // cannot be persisted. Report StorageFailure.
                            result.category = AppArtworkResultCategory::StorageFailure;
                            requestCompleted = true;
                            break;
                        }

                        const std::string tmpPath =
                            cacheTempFilePath(target.serverId, target.appId);
                        if (!writeWholeFile(tmpPath.c_str(), httpResp.body.data(),
                                            httpResp.body.size())) {
                            // Persistence failed; the image is still usable
                            // for this process. Report StorageFailure.
                            result.category = AppArtworkResultCategory::StorageFailure;
                            requestCompleted = true;
                            break;
                        }

                        // Decode the temp file to validate it and produce the
                        // CPU image. The decoded image size is bounded before
                        // decoding (see pngDimensionsFromFile) so an oversized
                        // image cannot exhaust the heap.
                        uint32_t tmpW = 0, tmpH = 0;
                        const bool tmpSizeOk =
                            pngDimensionsFromFile(tmpPath.c_str(), tmpW, tmpH) &&
                            static_cast<size_t>(tmpW) * tmpH * 4 <= kMaxDecodedImageBytes;
                        ps4::CpuPngImage decoded;
                        if (!tmpSizeOk || !ps4::decodePngToCpu(tmpPath.c_str(), decoded)) {
                            // Invalid PNG or oversized image — remove the temp
                            // file and report DecodeFailure.
                            remove(tmpPath.c_str());
                            result.category = AppArtworkResultCategory::DecodeFailure;
                            requestCompleted = true;
                            break;
                        }

                        // --- Success (image usable regardless of persistence)
                        // The image is decoded and usable for this process.
                        // Persistence is best-effort: if the atomic rename
                        // fails, the image is still returned as usable with
                        // cacheStored == false.
                        result.image = std::move(decoded);
                        result.fromCache = false;
                        result.cacheStored = false;
                        result.category = AppArtworkResultCategory::Success;

                        // --- Atomic rename to final cache file -------------
                        if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
                            // Persistence failed; remove the temp file. The
                            // image is still usable for this process, but it
                            // could not be persisted (cacheStored stays false).
                            remove(tmpPath.c_str());
                        } else {
                            result.cacheStored = true;
                        }

                        {
                            char buf[192];
                            snprintf(buf, sizeof(buf),
                                     "[artwork] decode success %ux%u host=%llu app=%u",
                                     static_cast<unsigned>(result.image.width),
                                     static_cast<unsigned>(result.image.height),
                                     static_cast<unsigned long long>(target.hostId),
                                     static_cast<unsigned>(target.appId));
                            awDiagPrint(buf);
                        }
                        {
                            char buf[192];
                            snprintf(buf, sizeof(buf),
                                     "[artwork] cache stored host=%llu app=%u stored=%d",
                                     static_cast<unsigned long long>(target.hostId),
                                     static_cast<unsigned>(target.appId),
                                     result.cacheStored ? 1 : 0);
                            awDiagPrint(buf);
                        }
                        requestCompleted = true;
                        break;
                    }

                    if (!requestCompleted) {
                        // All addresses failed with transport errors.
                        result.category = AppArtworkResultCategory::TransportFailure;
                    }
                }
            }
        }

        // --- Push result to the result queue --------------------------------
        pthread_mutex_lock(&impl->resultMutex);
        if (impl->resultQueue.size() >= kResultQueueCapacity) {
            // Overflow: drop oldest, preserve newest.
            impl->resultQueue.pop_front();
        }
        impl->resultQueue.push_back(std::move(result));
        pthread_mutex_unlock(&impl->resultMutex);

        // --- Clear active request state -------------------------------------
        pthread_mutex_lock(&impl->requestMutex);
        impl->activeCancellation = nullptr;
        impl->activeRequest = false;
        pthread_mutex_unlock(&impl->requestMutex);
    }

    awDiagPrint("[artwork] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AppArtworkManager::AppArtworkManager()
    : m_impl(new Impl())
{
}

AppArtworkManager::~AppArtworkManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool AppArtworkManager::initialize(HttpClient* httpClient) {
    if (m_impl->running.load()) {
        return false;
    }
    if (httpClient == nullptr || !httpClient->isInitialized()) {
        return false;
    }

    m_impl->httpClient = httpClient;

    // Initialize mutexes.
    if (!m_impl->requestMutexInit) {
        int rc = pthread_mutex_init(&m_impl->requestMutex, nullptr);
        if (rc != 0) {
            return false;
        }
        m_impl->requestMutexInit = true;
    }
    if (!m_impl->resultMutexInit) {
        int rc = pthread_mutex_init(&m_impl->resultMutex, nullptr);
        if (rc != 0) {
            return false;
        }
        m_impl->resultMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return false;
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return false;
        }
        m_impl->wakeCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->requestQueue.clear();
    m_impl->resultQueue.clear();
    m_impl->pendingIdentities.clear();
    m_impl->activeRequest = false;

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            artworkWorker, m_impl);
    if (rc != 0) {
        m_impl->running = false;
        return false;
    }
    m_impl->threadCreated = true;

    awDiagPrint("[artwork] manager initialized");
    return true;
}

// ---------------------------------------------------------------------------
// Request scheduling
// ---------------------------------------------------------------------------

AppArtworkResultCategory AppArtworkManager::requestArtwork(
    const AppArtworkTarget& target) {
    if (!m_impl->running.load()) {
        return AppArtworkResultCategory::InvalidState;
    }

    // --- Validate the target ----------------------------------------------
    if (target.hostId == 0) {
        return AppArtworkResultCategory::InvalidTarget;
    }
    if (!isValidServerId(target.serverId)) {
        return AppArtworkResultCategory::InvalidTarget;
    }
    if (target.ipv4Addresses.empty() || target.httpsPort == 0 ||
        target.clientUniqueId.empty() ||
        target.clientCertPem.empty() || target.clientKeyPem.empty()) {
        return AppArtworkResultCategory::InvalidTarget;
    }

    pthread_mutex_lock(&m_impl->requestMutex);

    // --- Duplicate suppression ---------------------------------------------
    // Reject a duplicate pending request for the same (hostId, appId,
    // generation).
    for (const auto& id : m_impl->pendingIdentities) {
        if (std::get<0>(id) == target.hostId &&
            std::get<1>(id) == target.appId &&
            std::get<2>(id) == target.generation) {
            pthread_mutex_unlock(&m_impl->requestMutex);
            return AppArtworkResultCategory::AlreadyPending;
        }
    }

    // --- Queue capacity check ----------------------------------------------
    if (m_impl->requestQueue.size() >= kRequestQueueCapacity) {
        pthread_mutex_unlock(&m_impl->requestMutex);
        return AppArtworkResultCategory::QueueFull;
    }

    // --- Enqueue the request ------------------------------------------------
    m_impl->requestQueue.push_back(target);
    m_impl->pendingIdentities.push_back(
        std::make_tuple(target.hostId, target.appId, target.generation));

    pthread_mutex_unlock(&m_impl->requestMutex);

    // Wake the worker.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[artwork] request host=%llu app=%u",
                 static_cast<unsigned long long>(target.hostId),
                 static_cast<unsigned>(target.appId));
        awDiagPrint(buf);
    }

    return AppArtworkResultCategory::Success;
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool AppArtworkManager::pollResult(AppArtworkResult& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->resultMutex);
    bool hasResult = !m_impl->resultQueue.empty();
    if (hasResult) {
        out = std::move(m_impl->resultQueue.front());
        m_impl->resultQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->resultMutex);

    return hasResult;
}

// ---------------------------------------------------------------------------
// Cache removal
// ---------------------------------------------------------------------------

bool AppArtworkManager::removeHostCache(const std::string& serverId) {
    if (!m_impl || !m_impl->running.load()) {
        return false;
    }
    if (!isValidServerId(serverId)) {
        return false;
    }

    const std::string dir = serverCacheDir(serverId);

    // Open the directory for listing using the OpenOrbis SDK file API. The OpenOrbis SDK
    // does not provide the POSIX dirent.h / opendir / readdir / closedir API;
    // directory listing is done via sceKernelOpen + sceKernelGetdents +
    // sceKernelClose.
    int fd = sceKernelOpen(dir.c_str(), O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        // Directory does not exist or cannot be opened — nothing to remove;
        // treat as success.
        return true;
    }

    // Read directory entries in batches and remove each file. The buffer must
    // be aligned for struct dirent records (which contain a uint32 first field).
    alignas(alignof(struct dirent)) char buf[4096];
    for (;;) {
        int n = sceKernelGetdents(fd, buf, static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            break;
        }
        int off = 0;
        while (off < n) {
            struct dirent* de = reinterpret_cast<struct dirent*>(buf + off);
            const char* name = de->d_name;
            if (name[0] != '\0' && name[0] != '.') {
                std::string filePath = dir;
                filePath += "/";
                filePath += name;
                sceKernelUnlink(filePath.c_str());
            }
            off += de->d_reclen;
        }
    }
    sceKernelClose(fd);

    // Remove the (now empty) directory.
    sceKernelRmdir(dir.c_str());

    {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[artwork] removed host cache server='%s'",
                 serverId.c_str());
        awDiagPrint(buf);
    }

    return true;
}

bool AppArtworkManager::clearAllCache() {
    if (!m_impl || !m_impl->running.load()) {
        return false;
    }

    // Remove the entire artwork cache root directory recursively. Only this
    // directory is removed; /data/moonlight/ and /data/ are left intact. The
    // cache is recreated automatically when new artwork is fetched.
    removeDirectoryRecursive(kArtworkRootDir);

    awDiagPrint("[artwork] cleared all artwork cache");
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void AppArtworkManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // Fire the active cancellation token (if any).
    pthread_mutex_lock(&m_impl->requestMutex);
    if (m_impl->activeCancellation != nullptr) {
        m_impl->activeCancellation->cancel();
    }
    pthread_mutex_unlock(&m_impl->requestMutex);

    // Wake the worker.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    // Join the worker thread.
    if (m_impl->threadCreated) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    // Clear queues.
    pthread_mutex_lock(&m_impl->requestMutex);
    m_impl->requestQueue.clear();
    m_impl->pendingIdentities.clear();
    pthread_mutex_unlock(&m_impl->requestMutex);

    pthread_mutex_lock(&m_impl->resultMutex);
    m_impl->resultQueue.clear();
    pthread_mutex_unlock(&m_impl->resultMutex);

    // Destroy synchronization primitives.
    if (m_impl->wakeCondInit) {
        pthread_cond_destroy(&m_impl->wakeCond);
        m_impl->wakeCondInit = false;
    }
    if (m_impl->wakeMutexInit) {
        pthread_mutex_destroy(&m_impl->wakeMutex);
        m_impl->wakeMutexInit = false;
    }
    if (m_impl->resultMutexInit) {
        pthread_mutex_destroy(&m_impl->resultMutex);
        m_impl->resultMutexInit = false;
    }
    if (m_impl->requestMutexInit) {
        pthread_mutex_destroy(&m_impl->requestMutex);
        m_impl->requestMutexInit = false;
    }

    m_impl->httpClient = nullptr;
    m_impl->running = false;

    awDiagPrint("[artwork] manager shut down");
}

// ---------------------------------------------------------------------------
// Initialization state
// ---------------------------------------------------------------------------

bool AppArtworkManager::isInitialized() const {
    return m_impl && m_impl->running.load();
}

} // namespace ml

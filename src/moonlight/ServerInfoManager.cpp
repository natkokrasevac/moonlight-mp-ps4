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

// === ServerInfoManager.cpp ==================================================
//
// OpenOrbis Moonlight client — ServerInfo / Host Capability Query
//
// Implements the ServerInfoManager concrete serverinfo retrieval subsystem:
//   - One worker thread consuming bounded request queue.
//   - Synchronous HttpClient calls for serverinfo.
//   - Safe XML parsing of the serverinfo response.
//   - Bounded result queue to the main thread.
//   - Deterministic cancellation and shutdown.
//
// OpenOrbis adaptation:
//   Uses POSIX pthread (pthread_create/join/mutex/cond) matching the proven
//   OpenOrbis threading path used by DiscoveryManager. The SCE reference's
//   ScePthread* types are replaced with their POSIX equivalents.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "ServerInfoManager.h"

#include "HttpClient.h"

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <orbis/libkernel.h>   // sceKernelGetProcessTime, sceKernelUsleep, sceKernelReadTsc

#include <cstdio>              // printf, fflush, snprintf
#include <cstring>             // memset
#include <cstdlib>             // strtoul
#include <deque>
#include <atomic>
#include <utility>             // std::pair

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void siDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

/// Diagnostic verbosity: log each serverinfo query scheduling line.
/// Default OFF to keep the klog quiet during normal operation.
static constexpr bool kLogQueryScheduled = false;

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Request queue capacity.
constexpr size_t kRequestQueueCapacity = 16;

/// Result queue capacity.
constexpr size_t kResultQueueCapacity = 16;

/// Maximum serverinfo response body size (64 KiB).
constexpr size_t kMaxServerInfoBodySize = 64 * 1024;

/// Maximum server ID length.
constexpr size_t kMaxServerIdLen = 256;

/// Maximum server name length.
constexpr size_t kMaxServerNameLen = 256;

/// Maximum server version length.
constexpr size_t kMaxServerVersionLen = 128;

/// Default HTTPS port fallback (per Moonlight reference).
constexpr uint16_t kDefaultHttpsPort = 47984;

/// Default HTTP port fallback (per Moonlight reference).
constexpr uint16_t kDefaultHttpPort = 47989;

/// Worker idle sleep in microseconds (when no work and not using cond var).
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

/// Maximum number of IPv4 addresses to try per request.
constexpr size_t kMaxAddressAttempts = 4;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of ServerInfoManager.
struct ServerInfoManager::Impl {
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
    std::deque<ServerInfoTarget> requestQueue;

    // --- Result queue -------------------------------------------------------
    pthread_mutex_t resultMutex;
    bool resultMutexInit = false;
    std::deque<ServerInfoUpdate> resultQueue;

    // --- Worker wake --------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;

    // --- Active request state ----------------------------------------------
    std::atomic<bool> activeRequest{false};
    std::atomic<HostId> activeHostId{0};
    // A fresh cancellation token is created per request. Guarded by
    // requestMutex so cancelAll()/shutdown() can signal the active one.
    HttpCancellationToken* activeCancellation = nullptr;

    // --- Pending HostId set (for duplicate suppression) ---------------------
    // Guarded by requestMutex.
    std::vector<HostId> pendingHostIds;

    // --- Request generation -------------------------------------------------
    // Guarded by requestMutex. Incremented per request for a HostId.
    std::vector<std::pair<HostId, uint32_t>> requestGenerations;
    uint32_t nextGeneration = 1;
};

// ---------------------------------------------------------------------------
// XML parsing helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

/// Safely extract the text content of a single XML tag from a byte buffer.
/// Returns true if the tag was found and the value is within bounds.
/// The value is copied into `outValue`.
bool extractXmlTag(const uint8_t* data, size_t size,
                   const char* tagName, std::string& outValue) {
    if (data == nullptr || tagName == nullptr) {
        return false;
    }

    const std::string openTag = std::string("<") + tagName + ">";
    const std::string closeTag = std::string("</") + tagName + ">";

    // Search for the opening tag within the buffer.
    // Convert to a string view for searching (bounded by size).
    const std::string xml(reinterpret_cast<const char*>(data), size);

    const size_t start = xml.find(openTag);
    if (start == std::string::npos) {
        return false;
    }

    const size_t valueStart = start + openTag.size();
    const size_t end = xml.find(closeTag, valueStart);
    if (end == std::string::npos || end < valueStart) {
        return false;
    }

    outValue = xml.substr(valueStart, end - valueStart);
    return true;
}

/// Parse a decimal integer from a string. Returns false on failure.
bool parseDecimal(const std::string& text, int& outValue) {
    if (text.empty()) {
        return false;
    }
    char* endPtr = nullptr;
    long val = strtol(text.c_str(), &endPtr, 10);
    if (endPtr == text.c_str() || *endPtr != '\0') {
        return false;
    }
    outValue = static_cast<int>(val);
    return true;
}

/// Parse a port value from a string. Returns false if invalid or out of range.
bool parsePort(const std::string& text, uint16_t& outPort) {
    int val = 0;
    if (!parseDecimal(text, val)) {
        return false;
    }
    if (val < 1 || val > 65535) {
        return false;
    }
    outPort = static_cast<uint16_t>(val);
    return true;
}

/// Parse the serverinfo XML body into a ServerInfo value.
/// Returns true on success, false on malformed/incomplete response.
bool parseServerInfo(const std::vector<uint8_t>& body, ServerInfo& outInfo) {
    if (body.empty()) {
        return false;
    }

    // Bound the body size.
    if (body.size() > kMaxServerInfoBodySize) {
        return false;
    }

    // Extract required fields.
    std::string uniqueId;
    std::string pairStatusText;
    std::string httpsPortText;
    std::string appVersion;
    std::string hostname;
    std::string currentGameText;
    std::string stateText;

    // uniqueid is required for stable server identity.
    if (!extractXmlTag(body.data(), body.size(), "uniqueid", uniqueId) ||
        uniqueId.empty()) {
        return false;
    }
    if (uniqueId.size() > kMaxServerIdLen) {
        return false;
    }

    // PairStatus is required for pairing-state reporting.
    if (!extractXmlTag(body.data(), body.size(), "PairStatus", pairStatusText) ||
        pairStatusText.empty()) {
        return false;
    }

    // HttpsPort is required for Pairing/App Catalog.
    if (!extractXmlTag(body.data(), body.size(), "HttpsPort", httpsPortText) ||
        httpsPortText.empty()) {
        return false;
    }

    // appversion is useful for diagnostics.
    extractXmlTag(body.data(), body.size(), "appversion", appVersion);
    if (appVersion.size() > kMaxServerVersionLen) {
        return false;
    }

    // hostname is useful for display.
    extractXmlTag(body.data(), body.size(), "hostname", hostname);
    if (hostname.size() > kMaxServerNameLen) {
        return false;
    }

    // currentgame is required for running-state reporting.
    if (!extractXmlTag(body.data(), body.size(), "currentgame", currentGameText) ||
        currentGameText.empty()) {
        return false;
    }

    // state is required to determine whether the current game is actually
    // running (the server keeps currentgame set even after streaming ends).
    if (!extractXmlTag(body.data(), body.size(), "state", stateText) ||
        stateText.empty()) {
        return false;
    }

    // ServerCodecModeSupport is optional but required for stream connection.
    // It is a hex value (e.g., "0x00000001" for H.264 only).
    std::string codecModeText;
    int serverCodecModeSupport = 0;
    if (extractXmlTag(body.data(), body.size(), "ServerCodecModeSupport", codecModeText) &&
        !codecModeText.empty()) {
        // Parse as hex (strtoul with base 0 handles 0x prefix).
        char* endPtr = nullptr;
        long val = strtol(codecModeText.c_str(), &endPtr, 0);
        if (endPtr != codecModeText.c_str() && *endPtr == '\0' && val >= 0) {
            serverCodecModeSupport = static_cast<int>(val);
        }
    }

    // Parse PairStatus: "1" = paired, "0" = unpaired.
    bool serverReportsPaired = false;
    if (pairStatusText == "1") {
        serverReportsPaired = true;
    } else if (pairStatusText == "0") {
        serverReportsPaired = false;
    } else {
        // Unexpected PairStatus value.
        return false;
    }

    // Parse HttpsPort with fallback to 47984.
    uint16_t httpsPort = kDefaultHttpsPort;
    if (!parsePort(httpsPortText, httpsPort)) {
        // Invalid port value; use fallback.
        httpsPort = kDefaultHttpsPort;
    }

    // Parse currentgame (app ID of the currently running app).
    int currentGame = 0;
    if (!parseDecimal(currentGameText, currentGame) || currentGame < 0) {
        return false;
    }

    // The server keeps currentgame set even after streaming ends. Only treat
    // it as running when the server state contains "_SERVER_BUSY".
    if (stateText.find("_SERVER_BUSY") == std::string::npos) {
        currentGame = 0;
    }

    outInfo.serverId = uniqueId;
    outInfo.serverName = hostname;
    outInfo.serverVersion = appVersion;
    outInfo.serverReportsPaired = serverReportsPaired;
    outInfo.httpsPort = httpsPort;
    outInfo.currentGame = static_cast<uint32_t>(currentGame);
    outInfo.serverCodecModeSupport = serverCodecModeSupport;

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

/// Returns true if the response body is a protocol error envelope indicating
/// the requesting client is not paired (the <root> element's status_code
/// attribute is 401). Sunshine returns such an envelope (rather than a clean
/// serverinfo body) when an unpaired client queries serverinfo over HTTPS.
bool responseIndicatesUnpaired(const std::vector<uint8_t>& body) {
    if (body.empty()) {
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());

    // Locate the <root ...> opening tag.
    const size_t rootStart = xml.find("<root");
    if (rootStart == std::string::npos) {
        return false;
    }
    const size_t rootEnd = xml.find('>', rootStart);
    if (rootEnd == std::string::npos) {
        return false;
    }
    const std::string rootTag = xml.substr(rootStart, rootEnd - rootStart);

    // status_code="401" (allow single or double quotes).
    return rootTag.find("status_code=\"401\"") != std::string::npos ||
           rootTag.find("status_code='401'") != std::string::npos;
}

/// Outcome of a single serverinfo HTTP/HTTPS attempt.
enum class ServerInfoAttempt {
    Success,           ///< Valid serverinfo parsed.
    Unpaired,          ///< Server reported the client is not paired (401).
    TransportFailure,  ///< Transport-level failure.
    HttpFailure,       ///< Non-200 HTTP status (not 401).
    MalformedResponse, ///< HTTP 200 but body not parseable as serverinfo.
    Cancelled
};

/// Perform one serverinfo request over the given transport and parse the
/// result. Returns the outcome and, on Success, the parsed ServerInfo.
ServerInfoAttempt performServerInfoAttempt(
    HttpClient* httpClient,
    const ServerInfoTarget& target,
    const std::string& addr,
    bool useTls,
    uint16_t port,
    HttpCancellationToken* cancellation,
    ServerInfo& outInfo) {
    // Build the request.
    HttpRequest httpReq;
    httpReq.method = HttpMethod::Get;
    httpReq.address = addr;
    httpReq.port = port;

    // Build the target: /serverinfo?uniqueid=...&uuid=...
    char uuidBuf[37];
    generateUuid(uuidBuf, sizeof(uuidBuf));
    std::string targetStr = "/serverinfo?uniqueid=";
    targetStr += target.clientUniqueId;
    targetStr += "&uuid=";
    targetStr += uuidBuf;
    httpReq.target = targetStr;

    // For paired hosts, query over HTTPS with the client certificate so the
    // server can report the correct PairStatus.
    httpReq.useTls = useTls;
    if (useTls) {
        httpReq.tls.trustMode = HttpTlsTrustMode::Unverified;
        httpReq.tls.clientCertPem = target.clientCertPem;
        httpReq.tls.clientKeyPem = target.clientKeyPem;
    }
    httpReq.connectTimeoutMs = target.connectTimeoutMs;
    httpReq.ioTimeoutMs = target.ioTimeoutMs;

    // Execute the request.
    HttpResponse httpResp;
    HttpResult httpResult = httpClient->perform(httpReq, httpResp, cancellation);

    if (cancellation != nullptr && cancellation->cancelled()) {
        return ServerInfoAttempt::Cancelled;
    }
    if (!httpResult.succeeded()) {
        return ServerInfoAttempt::TransportFailure;
    }

    // An HTTP 401, or a body envelope whose root status_code is 401, means the
    // server no longer considers this client paired.
    if (httpResp.statusCode == 401 || responseIndicatesUnpaired(httpResp.body)) {
        return ServerInfoAttempt::Unpaired;
    }
    if (httpResp.statusCode != 200) {
        return ServerInfoAttempt::HttpFailure;
    }

    // Parse the serverinfo body.
    ServerInfo parsed;
    if (!parseServerInfo(httpResp.body, parsed)) {
        return ServerInfoAttempt::MalformedResponse;
    }

    outInfo = std::move(parsed);
    return ServerInfoAttempt::Success;
}

} // namespace

// ---------------------------------------------------------------------------
// Worker thread entry
// ---------------------------------------------------------------------------

void* ServerInfoManager::serverInfoWorker(void* arg) {
    ServerInfoManager::Impl* impl =
        static_cast<ServerInfoManager::Impl*>(arg);

    siDiagPrint("[serverinfo] worker started");

    while (!impl->stopRequested.load()) {
        // --- Wait for work (non-busy-spin) ---------------------------------
        ServerInfoTarget target;
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

            // Remove the HostId from the pending set (it is now active).
            for (size_t i = 0; i < impl->pendingHostIds.size(); ++i) {
                if (impl->pendingHostIds[i] == target.hostId) {
                    impl->pendingHostIds.erase(impl->pendingHostIds.begin() + i);
                    break;
                }
            }

            impl->activeRequest = true;
            impl->activeHostId = target.hostId;
            impl->activeCancellation = &requestCancellation;
        }
        pthread_mutex_unlock(&impl->requestMutex);

        if (!hasWork) {
            // Sleep briefly and check again. This is a simple non-busy-spin
            // model compatible with the project's existing worker patterns.
            sceKernelUsleep(kWorkerIdleSleepUs);
            continue;
        }

        ServerInfoUpdate update;
        update.hostId = target.hostId;
        update.generation = target.generation;
        update.valid = false;

        // Check cancellation before starting.
        if (impl->stopRequested.load() || requestCancellation.cancelled()) {
            update.result = ServerInfoResult(ServerInfoResultCategory::Cancelled);
        } else {
            // --- Try each known IPv4 address in order ----------------------
            bool requestCompleted = false;
            size_t attempts = 0;

            for (const auto& addr : target.ipv4Addresses) {
                if (impl->stopRequested.load() || requestCancellation.cancelled()) {
                    update.result = ServerInfoResult(ServerInfoResultCategory::Cancelled);
                    requestCompleted = true;
                    break;
                }

                if (attempts >= kMaxAddressAttempts) {
                    break;
                }
                ++attempts;

                // Determine the HTTPS port (if a paired HTTPS query is used)
                // and the plain-HTTP port (discovery/bootstrap port).
                const uint16_t httpsPort =
                    (target.useTls && target.httpsPort != 0)
                        ? target.httpsPort
                        : 0;
                const uint16_t httpPort =
                    (target.discoveryPort != 0)
                        ? target.discoveryPort
                        : kDefaultHttpPort;

                // --- Paired host: try HTTPS first --------------------------
                // For a paired host, query over HTTPS with the client
                // certificate so the server reports the correct PairStatus.
                // If the server no longer considers this client paired, it
                // returns a 401 error envelope rather than a clean serverinfo;
                // in that case fall back to plain HTTP, which reports
                // PairStatus=0 so the unpair is detected (matching the
                // reference implementation).
                if (target.useTls && httpsPort != 0) {
                    ServerInfo parsed;
                    ServerInfoAttempt outcome = performServerInfoAttempt(
                        impl->httpClient, target, addr, true, httpsPort,
                        &requestCancellation, parsed);

                    if (outcome == ServerInfoAttempt::Cancelled) {
                        update.result = ServerInfoResult(ServerInfoResultCategory::Cancelled);
                        requestCompleted = true;
                        break;
                    }
                    if (outcome == ServerInfoAttempt::Success) {
                        update.valid = true;
                        update.info = std::move(parsed);
                        update.result = ServerInfoResult();
                        requestCompleted = true;
                        break;
                    }
                    if (outcome == ServerInfoAttempt::Unpaired) {
                        // The server reported this client is not paired. Fall
                        // back to plain HTTP on the same address so the
                        // serverinfo reports PairStatus=0.
                        ServerInfo httpParsed;
                        ServerInfoAttempt httpOutcome = performServerInfoAttempt(
                            impl->httpClient, target, addr, false, httpPort,
                            &requestCancellation, httpParsed);

                        if (httpOutcome == ServerInfoAttempt::Cancelled) {
                            update.result = ServerInfoResult(ServerInfoResultCategory::Cancelled);
                            requestCompleted = true;
                            break;
                        }
                        if (httpOutcome == ServerInfoAttempt::Success) {
                            update.valid = true;
                            update.info = std::move(httpParsed);
                            update.result = ServerInfoResult();
                            requestCompleted = true;
                            break;
                        }
                        // HTTP fallback also failed — try the next address.
                        continue;
                    }
                    if (outcome == ServerInfoAttempt::HttpFailure) {
                        update.result = ServerInfoResult(ServerInfoResultCategory::HttpFailure);
                        requestCompleted = true;
                        break;
                    }
                    if (outcome == ServerInfoAttempt::MalformedResponse) {
                        update.result = ServerInfoResult(ServerInfoResultCategory::MalformedResponse);
                        requestCompleted = true;
                        break;
                    }
                    // TransportFailure — try the next address.
                    continue;
                }

                // --- Unpaired / no HTTPS port: plain HTTP ------------------
                ServerInfo parsed;
                ServerInfoAttempt outcome = performServerInfoAttempt(
                    impl->httpClient, target, addr, false, httpPort,
                    &requestCancellation, parsed);

                if (outcome == ServerInfoAttempt::Cancelled) {
                    update.result = ServerInfoResult(ServerInfoResultCategory::Cancelled);
                    requestCompleted = true;
                    break;
                }
                if (outcome == ServerInfoAttempt::Success) {
                    update.valid = true;
                    update.info = std::move(parsed);
                    update.result = ServerInfoResult();
                    requestCompleted = true;
                    break;
                }
                if (outcome == ServerInfoAttempt::HttpFailure) {
                    update.result = ServerInfoResult(ServerInfoResultCategory::HttpFailure);
                    requestCompleted = true;
                    break;
                }
                if (outcome == ServerInfoAttempt::MalformedResponse) {
                    update.result = ServerInfoResult(ServerInfoResultCategory::MalformedResponse);
                    requestCompleted = true;
                    break;
                }
                // TransportFailure (or Unpaired on a plain-HTTP query, which
                // should not occur) — try the next address.
                continue;
            }

            if (!requestCompleted) {
                // All addresses failed with transport errors.
                update.result = ServerInfoResult(ServerInfoResultCategory::TransportFailure);
            }
        }

        // --- Push result to the result queue --------------------------------
        pthread_mutex_lock(&impl->resultMutex);
        if (impl->resultQueue.size() >= kResultQueueCapacity) {
            // Overflow: drop oldest, preserve newest.
            impl->resultQueue.pop_front();
        }
        impl->resultQueue.push_back(std::move(update));
        pthread_mutex_unlock(&impl->resultMutex);

        // --- Clear active request state -------------------------------------
        pthread_mutex_lock(&impl->requestMutex);
        impl->activeCancellation = nullptr;
        impl->activeRequest = false;
        impl->activeHostId = 0;
        pthread_mutex_unlock(&impl->requestMutex);
    }

    siDiagPrint("[serverinfo] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ServerInfoManager::ServerInfoManager()
    : m_impl(new Impl())
{
}

ServerInfoManager::~ServerInfoManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

ServerInfoResult ServerInfoManager::initialize(HttpClient* httpClient) {
    if (m_impl->running.load()) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidState);
    }
    if (httpClient == nullptr || !httpClient->isInitialized()) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidState);
    }

    m_impl->httpClient = httpClient;

    // Initialize mutexes.
    if (!m_impl->requestMutexInit) {
        int rc = pthread_mutex_init(&m_impl->requestMutex, nullptr);
        if (rc != 0) {
            return ServerInfoResult(ServerInfoResultCategory::InvalidState,
                                    static_cast<uint32_t>(rc));
        }
        m_impl->requestMutexInit = true;
    }
    if (!m_impl->resultMutexInit) {
        int rc = pthread_mutex_init(&m_impl->resultMutex, nullptr);
        if (rc != 0) {
            return ServerInfoResult(ServerInfoResultCategory::InvalidState,
                                    static_cast<uint32_t>(rc));
        }
        m_impl->resultMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return ServerInfoResult(ServerInfoResultCategory::InvalidState,
                                    static_cast<uint32_t>(rc));
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return ServerInfoResult(ServerInfoResultCategory::InvalidState,
                                    static_cast<uint32_t>(rc));
        }
        m_impl->wakeCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->requestQueue.clear();
    m_impl->resultQueue.clear();
    m_impl->pendingHostIds.clear();
    m_impl->requestGenerations.clear();
    m_impl->nextGeneration = 1;
    m_impl->activeRequest = false;
    m_impl->activeHostId = 0;

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            serverInfoWorker, m_impl);
    if (rc != 0) {
        m_impl->running = false;
        return ServerInfoResult(ServerInfoResultCategory::InvalidState,
                                static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    siDiagPrint("[serverinfo] manager initialized");
    return ServerInfoResult();
}

// ---------------------------------------------------------------------------
// Request scheduling
// ---------------------------------------------------------------------------

ServerInfoResult ServerInfoManager::request(HostId hostId, const ServerInfoTarget& target) {
    if (!m_impl->running.load()) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidState);
    }
    if (hostId == 0) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidHost);
    }

    pthread_mutex_lock(&m_impl->requestMutex);

    // --- Duplicate suppression ---------------------------------------------
    // If the HostId is already queued or active, suppress.
    for (HostId id : m_impl->pendingHostIds) {
        if (id == hostId) {
            pthread_mutex_unlock(&m_impl->requestMutex);
            return ServerInfoResult(ServerInfoResultCategory::AlreadyPending);
        }
    }
    if (m_impl->activeRequest.load() && m_impl->activeHostId.load() == hostId) {
        pthread_mutex_unlock(&m_impl->requestMutex);
        return ServerInfoResult(ServerInfoResultCategory::AlreadyPending);
    }

    // --- Queue capacity check ----------------------------------------------
    if (m_impl->requestQueue.size() >= kRequestQueueCapacity) {
        pthread_mutex_unlock(&m_impl->requestMutex);
        return ServerInfoResult(ServerInfoResultCategory::QueueFull);
    }

    // --- Increment request generation for this HostId -----------------------
    uint32_t generation = 0;
    bool found = false;
    for (auto& gen : m_impl->requestGenerations) {
        if (gen.first == hostId) {
            gen.second = m_impl->nextGeneration++;
            generation = gen.second;
            found = true;
            break;
        }
    }
    if (!found) {
        generation = m_impl->nextGeneration++;
        m_impl->requestGenerations.push_back({hostId, generation});
    }

    // --- Enqueue the request ------------------------------------------------
    ServerInfoTarget enqueued = target;
    enqueued.generation = generation;
    m_impl->requestQueue.push_back(std::move(enqueued));
    m_impl->pendingHostIds.push_back(hostId);

    pthread_mutex_unlock(&m_impl->requestMutex);

    // Wake the worker.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    // Per-request scheduling line. Guarded by a compile-time constant
    // (default OFF) to keep the klog quiet during normal operation.
    if (kLogQueryScheduled) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[serverinfo] query scheduled for host id=%llu",
                 static_cast<unsigned long long>(hostId));
        siDiagPrint(buf);
    }

    return ServerInfoResult();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void ServerInfoManager::cancelAll() {
    if (!m_impl) {
        return;
    }

    // Fire the active cancellation token (if any).
    pthread_mutex_lock(&m_impl->requestMutex);
    if (m_impl->activeCancellation != nullptr) {
        m_impl->activeCancellation->cancel();
    }
    // Clear queued work.
    m_impl->requestQueue.clear();
    m_impl->pendingHostIds.clear();
    pthread_mutex_unlock(&m_impl->requestMutex);

    // Wake the worker so it can observe the cancellation.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);
}

// ---------------------------------------------------------------------------
// Targeted cancellation
// ---------------------------------------------------------------------------

ServerInfoResult ServerInfoManager::cancel(HostId hostId) {
    if (!m_impl || !m_impl->running.load()) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidState);
    }
    if (hostId == 0) {
        return ServerInfoResult(ServerInfoResultCategory::InvalidHost);
    }

    pthread_mutex_lock(&m_impl->requestMutex);

    // If this host is the active request, fire its cancellation token so the
    // in-flight HTTP request is aborted.
    if (m_impl->activeRequest.load() &&
        m_impl->activeHostId.load() == hostId &&
        m_impl->activeCancellation != nullptr) {
        m_impl->activeCancellation->cancel();
    }

    // Remove this host from the queued (not yet active) work.
    for (auto it = m_impl->requestQueue.begin();
         it != m_impl->requestQueue.end();) {
        if (it->hostId == hostId) {
            it = m_impl->requestQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Remove this host from the pending set so duplicate suppression no longer
    // treats it as pending.
    for (auto it = m_impl->pendingHostIds.begin();
         it != m_impl->pendingHostIds.end();) {
        if (*it == hostId) {
            it = m_impl->pendingHostIds.erase(it);
        } else {
            ++it;
        }
    }

    pthread_mutex_unlock(&m_impl->requestMutex);

    // Wake the worker so it can observe the cancellation promptly.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[serverinfo] cancelled request for host id=%llu",
             static_cast<unsigned long long>(hostId));
    siDiagPrint(buf);

    return ServerInfoResult();
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool ServerInfoManager::pollUpdate(ServerInfoUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->resultMutex);
    bool hasUpdate = !m_impl->resultQueue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->resultQueue.front());
        m_impl->resultQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->resultMutex);

    return hasUpdate;
}

// ---------------------------------------------------------------------------
// Pending query check
// ---------------------------------------------------------------------------

bool ServerInfoManager::isPending(HostId hostId) const {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->requestMutex);
    bool pending = false;
    for (HostId id : m_impl->pendingHostIds) {
        if (id == hostId) {
            pending = true;
            break;
        }
    }
    if (!pending && m_impl->activeRequest.load() &&
        m_impl->activeHostId.load() == hostId) {
        pending = true;
    }
    pthread_mutex_unlock(&m_impl->requestMutex);

    return pending;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ServerInfoManager::shutdown() {
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
    m_impl->pendingHostIds.clear();
    m_impl->requestGenerations.clear();
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

    siDiagPrint("[serverinfo] manager shut down");
}

} // namespace ml

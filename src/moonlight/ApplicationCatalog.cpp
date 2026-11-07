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

// === ApplicationCatalog.cpp =================================================
//
// OpenOrbis Moonlight client — Application Catalog / App List
//
// Implements the ApplicationCatalog concrete App List retrieval subsystem:
//   - One worker thread per request (exactly one active request).
//   - Synchronous HttpClient calls for the App List endpoint.
//   - Safe bounded XML parsing of the App List response.
//   - Bounded result queue to the main thread.
//   - Deterministic cancellation and shutdown.
//
// OpenOrbis adaptation:
//   Uses POSIX pthread (pthread_create/join/mutex/cond) matching the proven
//   OpenOrbis threading path used by DiscoveryManager/ServerInfoManager. The
//   SCE reference's ScePthread* types are replaced with their POSIX
//   equivalents.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "ApplicationCatalog.h"

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

static void acDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Result queue capacity.
constexpr size_t kResultQueueCapacity = 4;

/// Maximum App List response body size (1 MiB).
constexpr size_t kMaxAppListBodySize = 1024 * 1024;

/// Maximum number of applications in a catalog.
constexpr size_t kMaxApplications = 512;

/// Maximum application name length (bytes).
constexpr size_t kMaxAppNameLen = 512;

/// Default HTTPS port fallback (per Moonlight reference).
constexpr uint16_t kDefaultHttpsPort = 47984;

/// Maximum number of IPv4 addresses to try per request.
constexpr size_t kMaxAddressAttempts = 4;

/// Worker idle sleep in microseconds (when no work).
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of ApplicationCatalog.
struct ApplicationCatalog::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- HttpClient dependency (non-owning) ---------------------------------
    HttpClient* httpClient = nullptr;

    // --- Active request state ----------------------------------------------
    std::atomic<bool> loading{false};
    std::atomic<bool> cancelRequested{false};
    ApplicationCatalogTarget activeTarget;
    HttpCancellationToken* activeCancellation = nullptr;   // Guarded by stateMutex.

    // --- Result queue -------------------------------------------------------
    pthread_mutex_t resultMutex;
    bool resultMutexInit = false;
    std::deque<ApplicationCatalogUpdate> resultQueue;

    // --- State mutex (for active request state) -----------------------------
    pthread_mutex_t stateMutex;
    bool stateMutexInit = false;

    // --- Worker wake --------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;
};

// ---------------------------------------------------------------------------
// XML parsing helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

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

/// Parse the root status_code attribute from the XML response.
/// Returns true if the root element has status_code="200".
bool parseRootStatus(const std::vector<uint8_t>& body) {
    if (body.empty()) {
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());

    // Find the root element opening tag.
    const size_t rootStart = xml.find("<root");
    if (rootStart == std::string::npos) {
        return false;
    }

    // Find the status_code attribute within the root element.
    const std::string statusAttr = "status_code=\"";
    const size_t attrStart = xml.find(statusAttr, rootStart);
    if (attrStart == std::string::npos) {
        return false;
    }

    const size_t valueStart = attrStart + statusAttr.size();
    const size_t valueEnd = xml.find('"', valueStart);
    if (valueEnd == std::string::npos) {
        return false;
    }

    const std::string statusText = xml.substr(valueStart, valueEnd - valueStart);
    int status = 0;
    if (!parseDecimal(statusText, status)) {
        return false;
    }
    return status == 200;
}

/// Parse the App List XML body into a vector of ApplicationInfo.
/// Returns ApplicationCatalogResultCategory::Success on success, or a
/// specific failure category on error.
ApplicationCatalogResultCategory parseAppList(
    const std::vector<uint8_t>& body,
    std::vector<ApplicationInfo>& outApps,
    uint32_t currentGame) {
    outApps.clear();
    if (body.empty()) {
        return ApplicationCatalogResultCategory::MalformedResponse;
    }

    // Bound the body size.
    if (body.size() > kMaxAppListBodySize) {
        return ApplicationCatalogResultCategory::ResponseTooLarge;
    }

    // Validate the root status_code.
    if (!parseRootStatus(body)) {
        return ApplicationCatalogResultCategory::MalformedResponse;
    }

    // Convert to a string for parsing.
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());

    // Iterate over repeated <App> elements.
    size_t pos = 0;
    size_t appCount = 0;

    while (appCount < kMaxApplications) {
        // Find the next <App> opening tag.
        const size_t appStart = xml.find("<App>", pos);
        if (appStart == std::string::npos) {
            break;   // No more apps.
        }

        // Find the matching </App> closing tag.
        const size_t appEnd = xml.find("</App>", appStart);
        if (appEnd == std::string::npos) {
            return ApplicationCatalogResultCategory::MalformedResponse;
        }

        // Extract the app element content.
        const std::string appContent = xml.substr(appStart, appEnd - appStart);

        // Extract ID.
        const std::string idOpen = "<ID>";
        const std::string idClose = "</ID>";
        const size_t idStart = appContent.find(idOpen);
        const size_t idEnd = appContent.find(idClose, idStart + idOpen.size());
        if (idStart == std::string::npos || idEnd == std::string::npos) {
            return ApplicationCatalogResultCategory::MalformedResponse;
        }
        const std::string idText = appContent.substr(idStart + idOpen.size(),
                                                     idEnd - (idStart + idOpen.size()));
        int idValue = 0;
        if (!parseDecimal(idText, idValue) || idValue < 0) {
            return ApplicationCatalogResultCategory::MalformedResponse;
        }

        // Extract AppTitle.
        const std::string titleOpen = "<AppTitle>";
        const std::string titleClose = "</AppTitle>";
        const size_t titleStart = appContent.find(titleOpen);
        const size_t titleEnd = appContent.find(titleClose, titleStart + titleOpen.size());
        if (titleStart == std::string::npos || titleEnd == std::string::npos) {
            return ApplicationCatalogResultCategory::MalformedResponse;
        }
        const std::string titleText = appContent.substr(titleStart + titleOpen.size(),
                                                        titleEnd - (titleStart + titleOpen.size()));
        if (titleText.size() > kMaxAppNameLen) {
            return ApplicationCatalogResultCategory::MalformedResponse;
        }

        // Build the ApplicationInfo.
        ApplicationInfo app;
        app.id = static_cast<ApplicationId>(idValue);
        app.name = titleText;
        app.running = (app.id == currentGame);

        // Check for duplicate IDs.
        for (const auto& existing : outApps) {
            if (existing.id == app.id) {
                return ApplicationCatalogResultCategory::MalformedResponse;
            }
        }

        outApps.push_back(std::move(app));
        ++appCount;

        // Advance past this app element.
        pos = appEnd + 6;   // 6 = strlen("</App>")
    }

    // If we hit the app count limit but there are more apps, fail.
    if (appCount >= kMaxApplications) {
        const size_t nextApp = xml.find("<App>", pos);
        if (nextApp != std::string::npos) {
            return ApplicationCatalogResultCategory::TooManyApplications;
        }
    }

    return ApplicationCatalogResultCategory::Success;
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

void* ApplicationCatalog::catalogWorker(void* arg) {
    ApplicationCatalog::Impl* impl =
        static_cast<ApplicationCatalog::Impl*>(arg);

    acDiagPrint("[applist] worker started");

    // Copy the active target (owned by the worker).
    ApplicationCatalogTarget target;
    {
        pthread_mutex_lock(&impl->stateMutex);
        target = impl->activeTarget;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Create a fresh cancellation token for this request.
    HttpCancellationToken requestCancellation;
    {
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = &requestCancellation;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Helper to push an update to the queue.
    auto pushUpdate = [impl](const ApplicationCatalogUpdate& update) {
        pthread_mutex_lock(&impl->resultMutex);
        if (impl->resultQueue.size() >= kResultQueueCapacity) {
            impl->resultQueue.pop_front();
        }
        impl->resultQueue.push_back(update);
        pthread_mutex_unlock(&impl->resultMutex);
    };

    ApplicationCatalogUpdate update;
    update.hostId = target.hostId;
    update.generation = target.generation;
    update.valid = false;

    // Check cancellation before starting.
    if (impl->stopRequested.load() || impl->cancelRequested.load() ||
        requestCancellation.cancelled()) {
        update.result = ApplicationCatalogResult(ApplicationCatalogResultCategory::Cancelled);
        pushUpdate(update);
        // Clear active request state.
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = nullptr;
        pthread_mutex_unlock(&impl->stateMutex);
        impl->loading = false;
        acDiagPrint("[applist] worker stopped");
        return nullptr;
    }

    // --- Try each known IPv4 address in order ------------------------------
    bool requestCompleted = false;
    size_t attempts = 0;

    acDiagPrint("[applist] worker entering address loop");

    for (const auto& addr : target.ipv4Addresses) {
        if (impl->stopRequested.load() || impl->cancelRequested.load() ||
            requestCancellation.cancelled()) {
            update.result = ApplicationCatalogResult(ApplicationCatalogResultCategory::Cancelled);
            requestCompleted = true;
            break;
        }

        if (attempts >= kMaxAddressAttempts) {
            break;
        }
        ++attempts;

        // --- Build the HttpRequest -----------------------------------------
        HttpRequest httpReq;
        httpReq.method = HttpMethod::Get;
        httpReq.address = addr;
        httpReq.port = (target.httpsPort != 0) ? target.httpsPort : kDefaultHttpsPort;

        // Build the target: /applist?uniqueid=...&uuid=...
        char uuidBuf[37];
        generateUuid(uuidBuf, sizeof(uuidBuf));

        std::string targetStr = "/applist?uniqueid=";
        targetStr += target.clientUniqueId;
        targetStr += "&uuid=";
        targetStr += uuidBuf;
        httpReq.target = targetStr;

        // Diagnostic: log the request address/port (not the full query string).
        {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[applist] request addr='%s' port=%u cert_len=%zu key_len=%zu",
                     addr.c_str(),
                     static_cast<unsigned>(httpReq.port),
                     target.clientCertPem.size(),
                     target.clientKeyPem.size());
            acDiagPrint(buf);
        }

        // HTTPS with client certificate authentication (mutual TLS).
        // The reference implementation (moonlight-embedded) uses no peer
        // pinning for the applist request — it relies on the client
        // certificate for authentication. The host is already reconciled as
        // Paired, so the client cert is the trust anchor.
        httpReq.useTls = true;
        httpReq.tls.trustMode = HttpTlsTrustMode::Unverified;
        httpReq.tls.clientCertPem = target.clientCertPem;
        httpReq.tls.clientKeyPem = target.clientKeyPem;
        httpReq.connectTimeoutMs = 3000;
        httpReq.ioTimeoutMs = 5000;

        // --- Execute the request -------------------------------------------
        HttpResponse httpResp;
        HttpResult httpResult = impl->httpClient->perform(
            httpReq, httpResp, &requestCancellation);

        if (impl->stopRequested.load() || impl->cancelRequested.load() ||
            requestCancellation.cancelled()) {
            update.result = ApplicationCatalogResult(ApplicationCatalogResultCategory::Cancelled);
            requestCompleted = true;
            break;
        }

        if (!httpResult.succeeded()) {
            // Diagnostic: log the underlying HttpResult category.
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "[applist] http request failed (category %d, native %u)",
                         static_cast<int>(httpResult.category),
                         static_cast<unsigned>(httpResult.nativeError));
                acDiagPrint(buf);
            }
            // Map the HttpResult category to an App Catalog category.
            switch (httpResult.category) {
            case HttpResultCategory::TlsVerificationFailure:
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::TrustVerificationFailure,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            case HttpResultCategory::TlsHandshakeFailure:
            case HttpResultCategory::TlsCertificateFailure:
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::TrustVerificationFailure,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            case HttpResultCategory::Cancelled:
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::Cancelled);
                requestCompleted = true;
                break;
            case HttpResultCategory::ResponseTooLarge:
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::ResponseTooLarge,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            case HttpResultCategory::MalformedResponse:
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::MalformedResponse,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            default:
                // Transport-class failure — try next address.
                continue;
            }
            break;
        }

        // --- Check HTTP status ---------------------------------------------
        if (httpResp.statusCode != 200) {
            // 401/403 = authentication failure; other non-200 = HTTP failure.
            if (httpResp.statusCode == 401 || httpResp.statusCode == 403) {
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::AuthenticationFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            } else {
                update.result = ApplicationCatalogResult(
                    ApplicationCatalogResultCategory::HttpFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            }
            requestCompleted = true;
            break;
        }

        // --- Parse the response --------------------------------------------
        std::vector<ApplicationInfo> parsedApps;
        ApplicationCatalogResultCategory parseResult =
            parseAppList(httpResp.body, parsedApps, target.currentGame);
        if (parseResult != ApplicationCatalogResultCategory::Success) {
            update.result = ApplicationCatalogResult(parseResult);
            requestCompleted = true;
            break;
        }

        // --- Success --------------------------------------------------------
        update.valid = true;
        update.applications = std::move(parsedApps);
        update.result = ApplicationCatalogResult();
        requestCompleted = true;
        break;
    }

    if (!requestCompleted) {
        // All addresses failed with transport errors.
        update.result = ApplicationCatalogResult(
            ApplicationCatalogResultCategory::TransportFailure);
    }

    // --- Push result to the result queue ------------------------------------
    pushUpdate(update);

    // --- Clear active request state -----------------------------------------
    pthread_mutex_lock(&impl->stateMutex);
    impl->activeCancellation = nullptr;
    pthread_mutex_unlock(&impl->stateMutex);
    impl->loading = false;

    acDiagPrint("[applist] worker stopped");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ApplicationCatalog::ApplicationCatalog()
    : m_impl(new Impl())
{
}

ApplicationCatalog::~ApplicationCatalog() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

ApplicationCatalogResult ApplicationCatalog::initialize(HttpClient* httpClient) {
    if (m_impl->running.load()) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState);
    }
    if (httpClient == nullptr || !httpClient->isInitialized()) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState);
    }

    m_impl->httpClient = httpClient;

    // Initialize mutexes.
    if (!m_impl->resultMutexInit) {
        int rc = pthread_mutex_init(&m_impl->resultMutex, nullptr);
        if (rc != 0) {
            return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState,
                                            static_cast<uint32_t>(rc));
        }
        m_impl->resultMutexInit = true;
    }
    if (!m_impl->stateMutexInit) {
        int rc = pthread_mutex_init(&m_impl->stateMutex, nullptr);
        if (rc != 0) {
            return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState,
                                            static_cast<uint32_t>(rc));
        }
        m_impl->stateMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState,
                                            static_cast<uint32_t>(rc));
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState,
                                            static_cast<uint32_t>(rc));
        }
        m_impl->wakeCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->loading = false;
    m_impl->cancelRequested = false;
    m_impl->resultQueue.clear();

    acDiagPrint("[applist] manager initialized");
    return ApplicationCatalogResult();
}

// ---------------------------------------------------------------------------
// Request scheduling
// ---------------------------------------------------------------------------

ApplicationCatalogResult ApplicationCatalog::request(const ApplicationCatalogTarget& target) {
    if (!m_impl->running.load()) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState);
    }
    if (m_impl->loading.load()) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::AlreadyLoading);
    }
    if (target.hostId == 0) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidHost);
    }
    if (target.serverId.empty() || target.ipv4Addresses.empty() ||
        target.httpsPort == 0 || target.clientUniqueId.empty() ||
        target.clientCertPem.empty() || target.clientKeyPem.empty()) {
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidTarget);
    }

    // Set up the active request state.
    m_impl->cancelRequested = false;
    m_impl->loading = true;
    m_impl->activeTarget = target;

    // Clear any stale updates.
    pthread_mutex_lock(&m_impl->resultMutex);
    m_impl->resultQueue.clear();
    pthread_mutex_unlock(&m_impl->resultMutex);

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            catalogWorker, m_impl);
    if (rc != 0) {
        m_impl->loading = false;
        return ApplicationCatalogResult(ApplicationCatalogResultCategory::InvalidState,
                                        static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[applist] request scheduled for host id=%llu",
             static_cast<unsigned long long>(target.hostId));
    acDiagPrint(buf);

    return ApplicationCatalogResult();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void ApplicationCatalog::cancel() {
    if (!m_impl || !m_impl->loading.load()) {
        return;
    }

    m_impl->cancelRequested = true;

    // Fire the active cancellation token (if any).
    pthread_mutex_lock(&m_impl->stateMutex);
    if (m_impl->activeCancellation != nullptr) {
        m_impl->activeCancellation->cancel();
    }
    pthread_mutex_unlock(&m_impl->stateMutex);

    // Wake the worker.
    pthread_mutex_lock(&m_impl->wakeMutex);
    pthread_cond_signal(&m_impl->wakeCond);
    pthread_mutex_unlock(&m_impl->wakeMutex);
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool ApplicationCatalog::pollUpdate(ApplicationCatalogUpdate& out) {
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
// Loading state
// ---------------------------------------------------------------------------

bool ApplicationCatalog::isLoading() const {
    return m_impl && m_impl->loading.load();
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void ApplicationCatalog::clear() {
    if (!m_impl) {
        return;
    }

    pthread_mutex_lock(&m_impl->resultMutex);
    m_impl->resultQueue.clear();
    pthread_mutex_unlock(&m_impl->resultMutex);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ApplicationCatalog::shutdown() {
    if (!m_impl) {
        return;
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // Cancel any active request.
    m_impl->cancelRequested = true;

    // Fire the active cancellation token (if any).
    pthread_mutex_lock(&m_impl->stateMutex);
    if (m_impl->activeCancellation != nullptr) {
        m_impl->activeCancellation->cancel();
    }
    pthread_mutex_unlock(&m_impl->stateMutex);

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
    if (m_impl->stateMutexInit) {
        pthread_mutex_destroy(&m_impl->stateMutex);
        m_impl->stateMutexInit = false;
    }
    if (m_impl->resultMutexInit) {
        pthread_mutex_destroy(&m_impl->resultMutex);
        m_impl->resultMutexInit = false;
    }

    m_impl->httpClient = nullptr;
    m_impl->running = false;
    m_impl->loading = false;

    acDiagPrint("[applist] manager shut down");
}

} // namespace ml

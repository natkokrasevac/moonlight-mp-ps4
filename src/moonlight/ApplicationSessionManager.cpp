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

// === ApplicationSessionManager.cpp =========================================
//
// OpenOrbis Moonlight client — Application Launch / Resume
//
// Implements the ApplicationSessionManager concrete launch/resume subsystem:
//   - One worker thread per launch/resume operation (exactly one active).
//   - Synchronous HttpClient calls for /launch and /resume.
//   - Canonical launch query parameters (matching the moonlight-embedded
//     gs_start_app() reference; the `corever` parameter is inlined as the
//     constant returned by moonlight-common-c's LiGetLaunchUrlQueryParameters()
//     so no moonlight-common-c linkage is required).
//   - Secure session crypto generation (PS4 RANDOM sysmodule).
//   - Safe bounded XML parsing of the launch/resume response.
//   - Bounded result queue to the main thread.
//   - Deterministic cancellation and shutdown.
//
// OpenOrbis adaptation:
//   Uses POSIX pthread (pthread_create/join/mutex/cond) matching the proven
//   OpenOrbis threading path used by ApplicationCatalog/ServerInfoManager. The
//   SCE reference's ScePthread* types are replaced with their POSIX
//   equivalents. The PS4 RANDOM sysmodule is accessed via the same
//   function-pointer-cast workaround used by PairingManager.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "ApplicationSessionManager.h"

#include "HttpClient.h"

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

#include <orbis/libkernel.h>   // sceKernelGetProcessTime, sceKernelUsleep
#include <orbis/Random.h>      // sceRandomGetRandomNumber (incorrect stub)

#include <cstdio>              // printf, fflush, snprintf
#include <cstring>             // memset, memcpy
#include <cstdlib>             // strtol
#include <deque>
#include <atomic>
#include <utility>             // std::move

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void almDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Result queue capacity.
constexpr size_t kResultQueueCapacity = 4;

/// Maximum launch/resume response body size (64 KiB).
constexpr size_t kMaxLaunchBodySize = 64 * 1024;

/// Maximum session URL length (bytes).
constexpr size_t kMaxSessionUrlLen = 2048;

/// Default HTTPS port fallback (per Moonlight reference).
constexpr uint16_t kDefaultHttpsPort = 47984;

/// Maximum number of IPv4 addresses to try per request.
constexpr size_t kMaxAddressAttempts = 4;

/// Remote input AES key/IV length (bytes).
constexpr size_t kRemoteInputKeyLen = 16;

/// Worker idle sleep in microseconds (when no work).
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

/// Canonical launch query parameters appended to the launch/resume URL.
/// This is the constant returned by moonlight-common-c's
/// LiGetLaunchUrlQueryParameters() ("&corever=1"). Inlined here so this
/// module does not link moonlight-common-c.
constexpr const char* kCoreVerParameter = "&corever=1";

/// Compute the `surroundAudioInfo` parameter value for the /launch and /resume
/// requests from the audio configuration. This mirrors the moonlight-common-c
/// SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION() macro (inlined here so this
/// module does not link moonlight-common-c).
constexpr int SurroundAudioInfoFromAudioConfiguration(int audioConfiguration) {
    const int channelCount = ((audioConfiguration) >> 8) & 0xFF;
    const int channelMask  = ((audioConfiguration) >> 16) & 0xFFFF;
    return (channelMask << 16) | channelCount;
}

} // namespace

// ---------------------------------------------------------------------------
// RNG access (OpenOrbis incorrect-stub workaround)
// ---------------------------------------------------------------------------

namespace {

/// The OpenOrbis SDK declares sceRandomGetRandomNumber() with no arguments
/// (an incorrect stub). The real PS4 signature is:
///     int sceRandomGetRandomNumber(void* buf, size_t size);
///
/// We take the address of the stub and cast it to the real function-pointer
/// type. This is the safe function-pointer-cast technique used by
/// PairingManager/PairingStore. The RANDOM sysmodule is loaded once by
/// SysmoduleManager at process startup; we do not reload it here.
using RandomFn = int (*)(void*, size_t);
static RandomFn s_randomFn = reinterpret_cast<RandomFn>(&sceRandomGetRandomNumber);

/// Generate random bytes using the PS4 RANDOM sysmodule.
bool generateRandomBytes(uint8_t* out, size_t size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    if (s_randomFn == nullptr) {
        return false;
    }
    return s_randomFn(out, size) >= 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of ApplicationSessionManager.
struct ApplicationSessionManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- HttpClient dependency (non-owning) ---------------------------------
    HttpClient* httpClient = nullptr;

    // --- Active launch state ------------------------------------------------
    std::atomic<bool> launching{false};
    std::atomic<bool> cancelRequested{false};
    ApplicationLaunchTarget activeTarget;
    HttpCancellationToken* activeCancellation = nullptr;   // Guarded by stateMutex.

    // --- Active quit state --------------------------------------------------
    std::atomic<bool> quitting{false};
    ApplicationQuitTarget activeQuitTarget;

    // --- Result queue -------------------------------------------------------
    pthread_mutex_t resultMutex;
    bool resultMutexInit = false;
    std::deque<ApplicationLaunchUpdate> resultQueue;
    std::deque<ApplicationQuitUpdate> quitResultQueue;

    // --- State mutex (for active launch state) ------------------------------
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

/// Safely extract the text content of a single XML tag from a byte buffer.
/// Returns true if the tag was found and the value is within bounds.
bool extractXmlTag(const uint8_t* data, size_t size,
                   const char* tagName, std::string& outValue) {
    if (data == nullptr || tagName == nullptr) {
        return false;
    }

    const std::string openTag = std::string("<") + tagName + ">";
    const std::string closeTag = std::string("</") + tagName + ">";

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

/// Parse the root status_code attribute from the XML response.
/// Returns true if the root element has status_code="200".
bool parseRootStatus(const std::vector<uint8_t>& body) {
    if (body.empty()) {
        return false;
    }
    const std::string xml(reinterpret_cast<const char*>(body.data()), body.size());

    const size_t rootStart = xml.find("<root");
    if (rootStart == std::string::npos) {
        return false;
    }

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

/// Parse the RTSP port number from a session URL (e.g. rtsp://1.2.3.4:48010).
/// Returns false if no valid port is found.
bool parseRtspPortFromUrl(const std::string& sessionUrl, uint16_t& outPort) {
    if (sessionUrl.empty()) {
        return false;
    }

    // Pick the last colon in the string to match the port number.
    const size_t colon = sessionUrl.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }

    const std::string portText = sessionUrl.substr(colon + 1);
    int port = 0;
    if (!parseDecimal(portText, port)) {
        return false;
    }
    if (port < 1 || port > 65535) {
        return false;
    }
    outPort = static_cast<uint16_t>(port);
    return true;
}

/// Generate a random UUID string (36 chars + null) using the secure PS4 RNG.
bool generateUuid(std::string& outUuid) {
    uint8_t bytes[16];
    if (!generateRandomBytes(bytes, sizeof(bytes))) {
        return false;
    }
    // RFC 4122 version 4.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    // RFC 4122 variant 1.
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    char uuid[37];
    // RFC 4122 canonical form: 8-4-4-4-12 (36 hex chars + 4 hyphens + null).
    snprintf(uuid, sizeof(uuid),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    outUuid = uuid;
    return true;
}

/// Bytes to hex string.
bool bytesToHex(const uint8_t* data, size_t size, std::string& outHex) {
    if (data == nullptr) {
        return false;
    }
    static const char kHex[] = "0123456789ABCDEF";
    outHex.clear();
    outHex.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        outHex.push_back(kHex[(data[i] >> 4) & 0x0F]);
        outHex.push_back(kHex[data[i] & 0x0F]);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Worker thread entry — launch/resume
// ---------------------------------------------------------------------------

void* ApplicationSessionManager::launchWorker(void* arg) {
    ApplicationSessionManager::Impl* impl =
        static_cast<ApplicationSessionManager::Impl*>(arg);

    almDiagPrint("[launch] worker started");

    // Copy the active target (owned by the worker).
    ApplicationLaunchTarget target;
    {
        pthread_mutex_lock(&impl->stateMutex);
        target = impl->activeTarget;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Create a fresh cancellation token for this operation.
    HttpCancellationToken requestCancellation;
    {
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = &requestCancellation;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Helper to push an update to the queue.
    auto pushUpdate = [impl](const ApplicationLaunchUpdate& update) {
        pthread_mutex_lock(&impl->resultMutex);
        if (impl->resultQueue.size() >= kResultQueueCapacity) {
            impl->resultQueue.pop_front();
        }
        impl->resultQueue.push_back(update);
        pthread_mutex_unlock(&impl->resultMutex);
    };

    // Helper to finish the worker (cleanup + return).
    auto finishWorker = [impl]() -> void* {
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = nullptr;
        pthread_mutex_unlock(&impl->stateMutex);
        impl->launching = false;
        almDiagPrint("[launch] worker stopped");
        return nullptr;
    };

    ApplicationLaunchUpdate update;
    update.hostId = target.hostId;
    update.applicationId = target.applicationId;
    update.generation = target.generation;
    update.valid = false;

    // --- Check cancellation before starting ---------------------------------
    if (impl->stopRequested.load() || impl->cancelRequested.load() ||
        requestCancellation.cancelled()) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
        pushUpdate(update);
        return finishWorker();
    }

    // --- Launch vs resume decision ------------------------------------------
    // Authoritative host state (Host.currentGame) drives the decision.
    //   currentGame == 0                          -> /launch
    //   currentGame == selected ApplicationId     -> /resume
    //   currentGame != 0 && != selected           -> reject (HostBusy)
    bool resume = false;
    if (target.currentGame != 0) {
        if (target.currentGame == target.applicationId) {
            resume = true;
        } else {
            // A different app is already running. Do not invent a quit/switch.
            update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::HostBusy);
            pushUpdate(update);
            return finishWorker();
        }
    }

    // --- Build the stream configuration -------------------------------------
    // Use the project-owned default configuration (single owner), then apply
    // the requested stream settings (resolution / frame rate) supplied by
    // MoonlightApp from the persistent SettingsManager.
    StreamConfiguration config;
    config.width = target.width;
    config.height = target.height;
    config.fps = target.fps;
    config.bitrate = target.bitrate;
    // Apply the selected video codec. The mask advertises the selected codec
    // plus H.264 as a baseline so the host can always fall back to H.264.
    config.videoFormat = target.videoFormat;
    config.videoFormatMask = target.videoFormat | 0x0001;

    // --- Generate remote input AES key/IV -----------------------------------
    // Matches the moonlight-embedded gs_start_app() reference:
    //   - remoteInputAesKey: 16 random bytes, hex-encoded as `rikey`.
    //   - remoteInputAesIv: 16-byte array; only the first 4 bytes are random.
    //     `rikeyid` is the network-byte-order 32-bit integer of those 4 bytes,
    //     sent as a decimal in the URL.
    uint8_t aesKey[kRemoteInputKeyLen];
    uint8_t aesIv[kRemoteInputKeyLen];
    memset(aesIv, 0, sizeof(aesIv));
    if (!generateRandomBytes(aesKey, sizeof(aesKey)) ||
        !generateRandomBytes(aesIv, 4)) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::CryptoFailure);
        pushUpdate(update);
        return finishWorker();
    }

    std::string rikeyHex;
    if (!bytesToHex(aesKey, sizeof(aesKey), rikeyHex)) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::CryptoFailure);
        pushUpdate(update);
        return finishWorker();
    }

    // Compute rikeyid as a 32-bit integer in network byte order.
    uint32_t rikeyid = 0;
    memcpy(&rikeyid, aesIv, 4);
    rikeyid = ((rikeyid & 0xFF) << 24) |
              ((rikeyid & 0xFF00) << 8) |
              ((rikeyid >> 8) & 0xFF00) |
              ((rikeyid >> 24) & 0xFF);

    // --- Build the query string ---------------------------------------------
    std::string uuid;
    if (!generateUuid(uuid)) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::CryptoFailure);
        pushUpdate(update);
        return finishWorker();
    }

    // Compute surroundAudioInfo from the audio configuration.
    const int surroundAudioInfo =
        SurroundAudioInfoFromAudioConfiguration(config.audioConfiguration);

    // Build the mode parameter: {width}x{height}x{fps}.
    const std::string modeStr = std::to_string(config.width) + "x" +
                                std::to_string(config.height) + "x" +
                                std::to_string(config.fps);

    // Map the persisted host settings to the GameStream protocol parameters.
    //   Speakers = Muted   -> localAudioPlayMode=0 (no audio on host)
    //   Speakers = Unmuted -> localAudioPlayMode=1 (play audio on host)
    //   Game Optimization = ON  -> sops=1 (SOPS enabled)
    //   Game Optimization = OFF -> sops=0 (SOPS disabled)
    const int localAudioPlayMode = target.muteHostSpeakers ? 0 : 1;
    const int sops = target.optimizeGameSettings ? 1 : 0;

    // Build the launch/resume URL matching the moonlight-embedded gs_start_app()
    // reference. Required parameters: uniqueid, uuid, appid, mode,
    // additionalStates, sops, rikey, rikeyid, localAudioPlayMode,
    // surroundAudioInfo, remoteControllersBitmap, gcmap, plus the canonical
    // corever parameter.
    // remoteControllersBitmap / gcmap are 0 (no gamepads yet).
    const char* endpoint = resume ? "resume" : "launch";
    std::string targetStr = std::string("/") + endpoint +
                    "?uniqueid=" + target.clientUniqueId +
                    "&uuid=" + uuid +
                    "&appid=" + std::to_string(target.applicationId) +
                    "&mode=" + modeStr +
                    "&additionalStates=1" +
                    "&sops=" + std::to_string(sops) +
                    "&rikey=" + rikeyHex +
                    "&rikeyid=" + std::to_string(rikeyid) +
                    "&localAudioPlayMode=" + std::to_string(localAudioPlayMode) +
                    "&surroundAudioInfo=" + std::to_string(surroundAudioInfo) +
                    "&remoteControllersBitmap=0" +
                    "&gcmap=0";

    // Append the canonical launch query parameter (corever=1).
    targetStr += kCoreVerParameter;

    // --- Try each known IPv4 address in order -------------------------------
    bool requestCompleted = false;
    size_t attempts = 0;

    for (const auto& addr : target.ipv4Addresses) {
        if (impl->stopRequested.load() || impl->cancelRequested.load() ||
            requestCancellation.cancelled()) {
            update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
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
        httpReq.target = targetStr;

        // Diagnostic: log the request address/port (not the sensitive query).
        {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[launch] request addr='%s' port=%u endpoint=%s",
                     addr.c_str(),
                     static_cast<unsigned>(httpReq.port),
                     resume ? "/resume" : "/launch");
            almDiagPrint(buf);
        }

        // Diagnostic: log the host settings applied to this launch/resume.
        {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[AppSession] launch muteHostSpeakers=%d optimizeGameSettings=%d",
                     target.muteHostSpeakers ? 1 : 0,
                     target.optimizeGameSettings ? 1 : 0);
            almDiagPrint(buf);
        }

        // HTTPS with client certificate authentication (mutual TLS).
        // Matches the proven paired-host behavior used by ApplicationCatalog.
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
            update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
            requestCompleted = true;
            break;
        }

        if (!httpResult.succeeded()) {
            // Map the HttpResult category to a launch category.
            switch (httpResult.category) {
            case HttpResultCategory::TlsVerificationFailure:
            case HttpResultCategory::TlsHandshakeFailure:
            case HttpResultCategory::TlsCertificateFailure:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::AuthenticationFailure,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            case HttpResultCategory::Cancelled:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::Cancelled);
                requestCompleted = true;
                break;
            case HttpResultCategory::ResponseTooLarge:
            case HttpResultCategory::MalformedResponse:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::MalformedResponse,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            default:
                // Transport-class failure — try next address.
                continue;
            }
            break;
        }

        // --- Diagnostic: log the HTTP response status and body size ---------
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[launch] http response status=%d body_len=%zu",
                     httpResp.statusCode, httpResp.body.size());
            almDiagPrint(buf);
        }

        // --- Check HTTP status ---------------------------------------------
        if (httpResp.statusCode != 200) {
            if (httpResp.statusCode == 401 || httpResp.statusCode == 403) {
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::AuthenticationFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            } else {
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::HttpFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            }
            requestCompleted = true;
            break;
        }

        // --- Bound the response body ---------------------------------------
        if (httpResp.body.empty() || httpResp.body.size() > kMaxLaunchBodySize) {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::MalformedResponse);
            requestCompleted = true;
            break;
        }

        // --- Parse the response --------------------------------------------
        // Check the protocol-level status_code. A non-200 status_code is a
        // rejection (e.g., "Missing a required launch parameter").
        if (!parseRootStatus(httpResp.body)) {
            char buf[256];
            const size_t logLen = httpResp.body.size() < 200 ? httpResp.body.size() : 200;
            snprintf(buf, sizeof(buf),
                     "[launch] protocol failure: http_status=%d body_len=%zu body='%.*s'",
                     httpResp.statusCode,
                     httpResp.body.size(),
                     static_cast<int>(logLen),
                     reinterpret_cast<const char*>(httpResp.body.data()));
            almDiagPrint(buf);
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::ProtocolFailure);
            requestCompleted = true;
            break;
        }

        // Check the gamesession/resume tag. A value of "0" indicates the
        // launch was rejected (matching gs_start_app()).
        std::string sessionState;
        if (!extractXmlTag(httpResp.body.data(), httpResp.body.size(),
                           "gamesession", sessionState) &&
            !extractXmlTag(httpResp.body.data(), httpResp.body.size(),
                           "resume", sessionState)) {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::MalformedResponse);
            requestCompleted = true;
            break;
        }
        if (sessionState == "0") {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::ProtocolFailure);
            requestCompleted = true;
            break;
        }

        // Extract the session URL.
        std::string sessionUrl;
        if (!extractXmlTag(httpResp.body.data(), httpResp.body.size(),
                           "sessionUrl0", sessionUrl) ||
            sessionUrl.empty()) {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::MalformedResponse);
            requestCompleted = true;
            break;
        }
        if (sessionUrl.size() > kMaxSessionUrlLen) {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::MalformedResponse);
            requestCompleted = true;
            break;
        }

        // Parse the RTSP port from the session URL.
        uint16_t rtspPort = 0;
        if (!parseRtspPortFromUrl(sessionUrl, rtspPort)) {
            // Fall back to the well-known RTSP port if parsing fails.
            rtspPort = 48010;
        }

        // --- Success --------------------------------------------------------
        update.valid = true;
        update.session.hostId = target.hostId;
        update.session.applicationId = target.applicationId;
        update.session.serverId = target.serverId;
        update.session.ipv4Address = addr;   // Lock in the successful numeric IP.
        update.session.rtspPort = rtspPort;
        update.session.sessionUrl = sessionUrl;
        update.session.configuration = config;
        update.session.remoteInputKey.assign(aesKey, aesKey + sizeof(aesKey));
        update.session.remoteInputIv.assign(aesIv, aesIv + sizeof(aesIv));
        update.session.serverInfoAppVersion = target.serverVersion;
        update.session.serverCodecModeSupport = target.serverCodecModeSupport;
        update.result = ApplicationLaunchResult();
        requestCompleted = true;
        break;
    }

    if (!requestCompleted) {
        // All addresses failed with transport errors.
        update.result = ApplicationLaunchResult(
            ApplicationLaunchResultCategory::TransportFailure);
    }

    // --- Push result to the result queue ------------------------------------
    pushUpdate(update);

    // --- Clear active launch state ------------------------------------------
    return finishWorker();
}

// ---------------------------------------------------------------------------
// Worker thread entry — remote quit
// ---------------------------------------------------------------------------

void* ApplicationSessionManager::quitWorker(void* arg) {
    ApplicationSessionManager::Impl* impl =
        static_cast<ApplicationSessionManager::Impl*>(arg);

    almDiagPrint("[quit] worker started");

    // Copy the active quit target (owned by the worker).
    ApplicationQuitTarget target;
    {
        pthread_mutex_lock(&impl->stateMutex);
        target = impl->activeQuitTarget;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Create a fresh cancellation token for this operation.
    HttpCancellationToken requestCancellation;
    {
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = &requestCancellation;
        pthread_mutex_unlock(&impl->stateMutex);
    }

    // Helper to push an update to the quit queue.
    auto pushQuitUpdate = [impl](const ApplicationQuitUpdate& update) {
        pthread_mutex_lock(&impl->resultMutex);
        if (impl->quitResultQueue.size() >= kResultQueueCapacity) {
            impl->quitResultQueue.pop_front();
        }
        impl->quitResultQueue.push_back(update);
        pthread_mutex_unlock(&impl->resultMutex);
    };

    // Helper to finish the worker (cleanup + return).
    auto finishQuitWorker = [impl]() -> void* {
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = nullptr;
        pthread_mutex_unlock(&impl->stateMutex);
        impl->quitting = false;
        almDiagPrint("[quit] worker stopped");
        return nullptr;
    };

    ApplicationQuitUpdate update;
    update.hostId = target.hostId;
    update.applicationId = target.applicationId;
    update.generation = target.generation;
    update.valid = false;

    // --- Check cancellation before starting ---------------------------------
    if (impl->stopRequested.load() || impl->cancelRequested.load() ||
        requestCancellation.cancelled()) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
        pushQuitUpdate(update);
        return finishQuitWorker();
    }

    // --- Build the query string ---------------------------------------------
    // The /cancel endpoint takes only the base uniqueid + uuid parameters
    // (matching the moonlight-qt NvHTTP::quitApp() reference). No app-specific
    // arguments are sent.
    std::string uuid;
    if (!generateUuid(uuid)) {
        update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::CryptoFailure);
        pushQuitUpdate(update);
        return finishQuitWorker();
    }

    std::string targetStr = "/cancel?uniqueid=" + target.clientUniqueId +
                            "&uuid=" + uuid;

    // --- Try each known IPv4 address in order -------------------------------
    bool requestCompleted = false;
    size_t attempts = 0;

    for (const auto& addr : target.ipv4Addresses) {
        if (impl->stopRequested.load() || impl->cancelRequested.load() ||
            requestCancellation.cancelled()) {
            update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
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
        httpReq.target = targetStr;

        // Diagnostic: log the request address/port (not the sensitive query).
        {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[quit] request addr='%s' port=%u endpoint=/cancel",
                     addr.c_str(),
                     static_cast<unsigned>(httpReq.port));
            almDiagPrint(buf);
        }

        // HTTPS with client certificate authentication (mutual TLS).
        // Matches the proven paired-host behavior used by launch/applist.
        httpReq.useTls = true;
        httpReq.tls.trustMode = HttpTlsTrustMode::Unverified;
        httpReq.tls.clientCertPem = target.clientCertPem;
        httpReq.tls.clientKeyPem = target.clientKeyPem;
        httpReq.connectTimeoutMs = 3000;
        // The /cancel endpoint responds only after the host has actually
        // stopped the running app. Apps can take a while to quit, so use a
        // generous I/O timeout (30 s) rather than the short 5 s used for
        // launch/applist requests.
        httpReq.ioTimeoutMs = 30000;

        // --- Execute the request -------------------------------------------
        HttpResponse httpResp;
        HttpResult httpResult = impl->httpClient->perform(
            httpReq, httpResp, &requestCancellation);

        if (impl->stopRequested.load() || impl->cancelRequested.load() ||
            requestCancellation.cancelled()) {
            update.result = ApplicationLaunchResult(ApplicationLaunchResultCategory::Cancelled);
            requestCompleted = true;
            break;
        }

        if (!httpResult.succeeded()) {
            // Map the HttpResult category to a launch category.
            switch (httpResult.category) {
            case HttpResultCategory::TlsVerificationFailure:
            case HttpResultCategory::TlsHandshakeFailure:
            case HttpResultCategory::TlsCertificateFailure:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::AuthenticationFailure,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            case HttpResultCategory::Cancelled:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::Cancelled);
                requestCompleted = true;
                break;
            case HttpResultCategory::ResponseTooLarge:
            case HttpResultCategory::MalformedResponse:
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::MalformedResponse,
                    httpResult.nativeError);
                requestCompleted = true;
                break;
            default:
                // Transport-class failure — try next address.
                continue;
            }
            break;
        }

        // --- Diagnostic: log the HTTP response status and body size ---------
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[quit] http response status=%d body_len=%zu",
                     httpResp.statusCode, httpResp.body.size());
            almDiagPrint(buf);
        }

        // --- Check HTTP status ---------------------------------------------
        if (httpResp.statusCode != 200) {
            if (httpResp.statusCode == 401 || httpResp.statusCode == 403) {
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::AuthenticationFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            } else {
                update.result = ApplicationLaunchResult(
                    ApplicationLaunchResultCategory::HttpFailure,
                    static_cast<uint32_t>(httpResp.statusCode));
            }
            requestCompleted = true;
            break;
        }

        // --- Bound the response body ---------------------------------------
        if (httpResp.body.empty() || httpResp.body.size() > kMaxLaunchBodySize) {
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::MalformedResponse);
            requestCompleted = true;
            break;
        }

        // --- Parse the response --------------------------------------------
        // Check the protocol-level status_code. A non-200 status_code is a
        // rejection (matching verifyResponseStatus() in the reference).
        if (!parseRootStatus(httpResp.body)) {
            char buf[256];
            const size_t logLen = httpResp.body.size() < 200 ? httpResp.body.size() : 200;
            snprintf(buf, sizeof(buf),
                     "[quit] protocol failure: http_status=%d body_len=%zu body='%.*s'",
                     httpResp.statusCode,
                     httpResp.body.size(),
                     static_cast<int>(logLen),
                     reinterpret_cast<const char*>(httpResp.body.data()));
            almDiagPrint(buf);
            update.result = ApplicationLaunchResult(
                ApplicationLaunchResultCategory::ProtocolFailure);
            requestCompleted = true;
            break;
        }

        // --- Success --------------------------------------------------------
        // The /cancel request was accepted. The host is authoritative; the
        // caller (MoonlightApp) will schedule a serverinfo refresh to confirm
        // the app actually stopped.
        update.valid = true;
        update.result = ApplicationLaunchResult();
        requestCompleted = true;
        break;
    }

    if (!requestCompleted) {
        // All addresses failed with transport errors.
        update.result = ApplicationLaunchResult(
            ApplicationLaunchResultCategory::TransportFailure);
    }

    // --- Push result to the quit result queue -------------------------------
    pushQuitUpdate(update);

    // --- Clear active quit state --------------------------------------------
    return finishQuitWorker();
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ApplicationSessionManager::ApplicationSessionManager()
    : m_impl(new Impl())
{
}

ApplicationSessionManager::~ApplicationSessionManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

ApplicationLaunchResult ApplicationSessionManager::initialize(HttpClient* httpClient) {
    if (m_impl->running.load()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState);
    }
    if (httpClient == nullptr || !httpClient->isInitialized()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState);
    }

    m_impl->httpClient = httpClient;

    // Initialize mutexes.
    if (!m_impl->resultMutexInit) {
        int rc = pthread_mutex_init(&m_impl->resultMutex, nullptr);
        if (rc != 0) {
            return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                           static_cast<uint32_t>(rc));
        }
        m_impl->resultMutexInit = true;
    }
    if (!m_impl->stateMutexInit) {
        int rc = pthread_mutex_init(&m_impl->stateMutex, nullptr);
        if (rc != 0) {
            return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                           static_cast<uint32_t>(rc));
        }
        m_impl->stateMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                           static_cast<uint32_t>(rc));
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                           static_cast<uint32_t>(rc));
        }
        m_impl->wakeCondInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->launching = false;
    m_impl->quitting = false;
    m_impl->cancelRequested = false;
    m_impl->resultQueue.clear();
    m_impl->quitResultQueue.clear();

    almDiagPrint("[launch] manager initialized");
    return ApplicationLaunchResult();
}

// ---------------------------------------------------------------------------
// Request scheduling
// ---------------------------------------------------------------------------

ApplicationLaunchResult ApplicationSessionManager::request(const ApplicationLaunchTarget& target) {
    if (!m_impl->running.load()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState);
    }
    if (m_impl->launching.load()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::AlreadyLaunching);
    }
    if (target.hostId == 0) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidHost);
    }
    if (target.applicationId == 0) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidApplication);
    }
    if (target.serverId.empty() || target.ipv4Addresses.empty() ||
        target.httpsPort == 0 || target.clientUniqueId.empty() ||
        target.clientCertPem.empty() || target.clientKeyPem.empty()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidTarget);
    }

    // Set up the active launch state.
    m_impl->cancelRequested = false;
    m_impl->launching = true;
    m_impl->activeTarget = target;

    // Clear any stale updates.
    pthread_mutex_lock(&m_impl->resultMutex);
    m_impl->resultQueue.clear();
    pthread_mutex_unlock(&m_impl->resultMutex);

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            launchWorker, m_impl);
    if (rc != 0) {
        m_impl->launching = false;
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                       static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[launch] request scheduled for host id=%llu app id=%u",
             static_cast<unsigned long long>(target.hostId),
             static_cast<unsigned>(target.applicationId));
    almDiagPrint(buf);

    return ApplicationLaunchResult();
}

// ---------------------------------------------------------------------------
// Quit request scheduling
// ---------------------------------------------------------------------------

ApplicationLaunchResult ApplicationSessionManager::requestQuit(const ApplicationQuitTarget& target) {
    if (!m_impl->running.load()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState);
    }
    // One operation at a time: a quit cannot overlap a launch/resume/quit.
    if (m_impl->launching.load() || m_impl->quitting.load()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::AlreadyLaunching);
    }
    if (target.hostId == 0) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidHost);
    }
    if (target.applicationId == 0) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidApplication);
    }
    if (target.serverId.empty() || target.ipv4Addresses.empty() ||
        target.httpsPort == 0 || target.clientUniqueId.empty() ||
        target.clientCertPem.empty() || target.clientKeyPem.empty()) {
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidTarget);
    }

    // Set up the active quit state.
    m_impl->cancelRequested = false;
    m_impl->quitting = true;
    m_impl->activeQuitTarget = target;

    // Clear any stale updates.
    pthread_mutex_lock(&m_impl->resultMutex);
    m_impl->quitResultQueue.clear();
    pthread_mutex_unlock(&m_impl->resultMutex);

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            quitWorker, m_impl);
    if (rc != 0) {
        m_impl->quitting = false;
        return ApplicationLaunchResult(ApplicationLaunchResultCategory::InvalidState,
                                       static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[quit] request scheduled for host id=%llu app id=%u",
             static_cast<unsigned long long>(target.hostId),
             static_cast<unsigned>(target.applicationId));
    almDiagPrint(buf);

    return ApplicationLaunchResult();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void ApplicationSessionManager::cancel() {
    if (!m_impl || (!m_impl->launching.load() && !m_impl->quitting.load())) {
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

bool ApplicationSessionManager::pollUpdate(ApplicationLaunchUpdate& out) {
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

bool ApplicationSessionManager::pollQuitUpdate(ApplicationQuitUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->resultMutex);
    bool hasUpdate = !m_impl->quitResultQueue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->quitResultQueue.front());
        m_impl->quitResultQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->resultMutex);

    return hasUpdate;
}

// ---------------------------------------------------------------------------
// Launching state
// ---------------------------------------------------------------------------

bool ApplicationSessionManager::isLaunching() const {
    return m_impl && m_impl->launching.load();
}

bool ApplicationSessionManager::isQuitting() const {
    return m_impl && m_impl->quitting.load();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ApplicationSessionManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // Cancel any active launch.
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
    m_impl->quitResultQueue.clear();
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
    m_impl->launching = false;
    m_impl->quitting = false;

    almDiagPrint("[launch] manager shut down");
}

} // namespace ml

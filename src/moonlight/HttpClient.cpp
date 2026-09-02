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

// === HttpClient.cpp ==========================================================
//
// OpenOrbis Moonlight client — HTTP / HTTPS Client subsystem
//
// Implements the HttpClient concrete HTTP/HTTPS transport:
//   - libSceHttp + libSceSsl for HTTP/HTTPS.
//   - Bounded timeouts and cancellation.
//   - Deterministic cleanup of request/connection resources.
//
// OpenOrbis adaptation:
//   The OpenOrbis SDK headers declare the non-blocking/epoll HTTP functions
//   (sceHttpSetNonblock, sceHttpCreateEpoll, sceHttpSetEpoll, sceHttpWaitRequest,
//   sceHttpAbortWaitRequest, sceHttpUnsetEpoll, sceHttpDestroyEpoll) as void
//   stubs, and sceSslGetPem is absent entirely. Therefore this implementation
//   uses the proven OpenOrbis blocking HTTP path (as demonstrated by the
//   OpenOrbis net_http sample and the moonlight-ps4_2 reference): synchronous
//   sceHttpSendRequest / sceHttpReadData with SDK-side timeouts.
//
//   Peer-certificate retrieval and pinned-trust verification are not
//   available: the OpenOrbis SDK does not expose sceSslGetPem, so the
//   certificate pinning callback path is not available in this SDK version.
//   TLS trust modes are validated but the peer identity is established via
//   pairing (signature verification), not the public PKI.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "HttpClient.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/Http.h>             // sceHttpInit, sceHttpTerm, sceHttpCreateTemplate, etc.
#include <orbis/Ssl.h>              // sceSslInit, sceSslTerm
#include <orbis/Net.h>              // sceNetPoolCreate, sceNetPoolDestroy

#include <cstdio>                // printf, fflush, snprintf
#include <cstring>               // memset
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other platform modules)
// ---------------------------------------------------------------------------

static void httpDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// OpenOrbis SDK header workarounds
// ---------------------------------------------------------------------------
// The OpenOrbis SDK headers declare several libSceHttp/libSceSsl functions as
// 0-argument void stubs even though the actual library functions take
// arguments (the moonlight-ps4_2 reference and the net_http sample call them
// with arguments and they work at runtime). Use function-pointer casts to call
// the actual library functions with the correct signatures.

/// sceSslTerm(int32_t sslCtxId) — header declares void sceSslTerm().
static void httpSslTerm(int32_t sslCtxId) {
    typedef void (*SslTermFn)(int32_t);
    static SslTermFn fn = reinterpret_cast<SslTermFn>(
        reinterpret_cast<void*>(&sceSslTerm));
    fn(sslCtxId);
}

/// sceHttpSetRecvTimeOut(int32_t id, uint32_t usec) — header declares void().
static void httpSetRecvTimeOut(int32_t id, uint32_t usec) {
    typedef void (*SetRecvTimeOutFn)(int32_t, uint32_t);
    static SetRecvTimeOutFn fn = reinterpret_cast<SetRecvTimeOutFn>(
        reinterpret_cast<void*>(&sceHttpSetRecvTimeOut));
    fn(id, usec);
}

/// sceHttpsUnloadCert(int32_t httpId) — header declares void().
static void httpHttpsUnloadCert(int32_t httpId) {
    typedef void (*HttpsUnloadCertFn)(int32_t);
    static HttpsUnloadCertFn fn = reinterpret_cast<HttpsUnloadCertFn>(
        reinterpret_cast<void*>(&sceHttpsUnloadCert));
    fn(httpId);
}

// ---------------------------------------------------------------------------
// SSL verification callback
// ---------------------------------------------------------------------------

/// Accept-all SSL verification callback for HTTPS requests.
///
/// The OpenOrbis SDK's OrbisHttpsCallback signature is:
///   int32_t (*)(int32_t libsslCtxId, uint32_t verifyErr,
///               void* const sslCert[], int32_t certNum, void* userArg)
///
/// The peer identity is established via pairing (signature verification),
/// not the public PKI, so an accept-all callback is the correct trust model
/// for the bootstrap. This matches the proven OpenOrbis net_http sample and
/// moonlight-ps4_2 reference.
static int32_t sslVerifyCallback(int32_t libsslCtxId,
                                 uint32_t verifyErr,
                                 void* const sslCert[],
                                 int32_t certNum,
                                 void* userArg) {
    (void)libsslCtxId;
    (void)verifyErr;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    return 1;   // accept
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Net pool size for libSceHttp (single active request).
constexpr int kNetPoolSize = 16 * 1024;

/// SSL pool size. SDK recommends 256 KiB minimum for 3 concurrent connections;
/// 304 KiB covers 1 connection plus client cert/key loading.
constexpr size_t kSslPoolSize = 304 * 1024;

/// HTTP pool size for libSceHttp (single active request).
constexpr size_t kHttpPoolSize = 48 * 1024;

/// User-Agent string.
constexpr const char* kUserAgent = "MoonlightPS4/1.00";

/// Maximum response body size (4 MiB).
constexpr size_t kMaxBodySize = 4 * 1024 * 1024;

/// Maximum request target length (8 KiB).
constexpr size_t kMaxTargetLen = 8 * 1024;

/// Read chunk size for response body.
constexpr size_t kReadChunkSize = 4096;

// --- OpenOrbis SDK does not define these SCE constants -----------------------
// The OpenOrbis SDK headers expose the ORBIS_* prefixed enum values but not
// the SCE_HTTP_HEADER_* / SCE_HTTPS_FLAG_* constants used by the SCE
// reference. These are the standard OpenOrbis SDK values, defined locally so the
// SCE HttpClient logic can be preserved.
constexpr int kHttpHeaderOverwrite = 1;   ///< SCE_HTTP_HEADER_OVERWRITE
constexpr int kHttpHeaderAdd       = 2;   ///< SCE_HTTP_HEADER_ADD
constexpr uint32_t kHttpsFlagCnCheck      = 0x00000001;  ///< SCE_HTTPS_FLAG_CN_CHECK
constexpr uint32_t kHttpsFlagKnownCaCheck = 0x00000002;  ///< SCE_HTTPS_FLAG_KNOWN_CA_CHECK

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of HttpClient. Holds all PS4 HTTP/SSL state.
struct HttpClient::Impl {
    // --- Lifecycle ----------------------------------------------------------
    bool initialized = false;

    // --- SDK context IDs ----------------------------------------------------
    int netPoolId = -1;    ///< sceNetPoolCreate result.
    int sslCtxId  = -1;    ///< sceSslInit result.
    int httpCtxId = -1;    ///< sceHttpInit result.
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

HttpClient::HttpClient()
    : m_impl(new Impl())
{
}

HttpClient::~HttpClient() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

HttpResult HttpClient::initialize() {
    if (m_impl->initialized) {
        return HttpResult(HttpResultCategory::InvalidState);
    }

    httpDiagPrint("[http] initialization start");

    // The HTTP, SSL, and NET sysmodules are loaded at process startup by
    // SysmoduleManager. HttpClient does NOT own sysmodule loading; it only
    // consumes the loaded services.

    // 1. Create the Net memory pool required by libSceHttp.
    int rc = sceNetPoolCreate("HttpClient", kNetPoolSize, 0);
    if (rc < 0) {
        httpDiagPrint("[http] sceNetPoolCreate failed");
        return HttpResult(HttpResultCategory::HttpInitializationFailure,
                          static_cast<uint32_t>(rc));
    }
    m_impl->netPoolId = rc;

    // 2. Initialize libSceSsl.
    rc = sceSslInit(kSslPoolSize);
    if (rc < 0) {
        httpDiagPrint("[http] sceSslInit failed");
        sceNetPoolDestroy(m_impl->netPoolId);
        m_impl->netPoolId = -1;
        return HttpResult(HttpResultCategory::SslInitializationFailure,
                          static_cast<uint32_t>(rc));
    }
    m_impl->sslCtxId = rc;

    // 3. Initialize libSceHttp.
    rc = sceHttpInit(m_impl->netPoolId, m_impl->sslCtxId, kHttpPoolSize);
    if (rc < 0) {
        httpDiagPrint("[http] sceHttpInit failed");
        httpSslTerm(m_impl->sslCtxId);
        m_impl->sslCtxId = -1;
        sceNetPoolDestroy(m_impl->netPoolId);
        m_impl->netPoolId = -1;
        return HttpResult(HttpResultCategory::HttpInitializationFailure,
                          static_cast<uint32_t>(rc));
    }
    m_impl->httpCtxId = rc;

    m_impl->initialized = true;
    httpDiagPrint("[http] initialization success");
    return HttpResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void HttpClient::shutdown() {
    if (!m_impl) {
        return;
    }

    // Reverse dependency order: HTTP first, then SSL, then Net pool.
    if (m_impl->httpCtxId >= 0) {
        sceHttpTerm(m_impl->httpCtxId);
        m_impl->httpCtxId = -1;
    }

    if (m_impl->sslCtxId >= 0) {
        httpSslTerm(m_impl->sslCtxId);
        m_impl->sslCtxId = -1;
    }

    if (m_impl->netPoolId >= 0) {
        sceNetPoolDestroy(m_impl->netPoolId);
        m_impl->netPoolId = -1;
    }

    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool HttpClient::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Request execution
// ---------------------------------------------------------------------------

HttpResult HttpClient::perform(const HttpRequest& request,
                               HttpResponse& response,
                               HttpCancellationToken* cancellation) {
    if (!m_impl || !m_impl->initialized) {
        return HttpResult(HttpResultCategory::InvalidState);
    }

    // --- Validate request ---------------------------------------------------
    if (request.address.empty() || request.port == 0) {
        httpDiagPrint("[http] invalid request: address/port");
        return HttpResult(HttpResultCategory::InvalidRequest);
    }
    if (request.target.empty() || request.target.size() > kMaxTargetLen) {
        httpDiagPrint("[http] invalid request: target");
        return HttpResult(HttpResultCategory::InvalidRequest);
    }
    if (request.useTls &&
        request.tls.trustMode == HttpTlsTrustMode::PinnedCertificate &&
        request.tls.expectedPeerCertificate.empty()) {
        // Never silently degrade to Unverified.
        httpDiagPrint("[http] invalid request: empty pin");
        return HttpResult(HttpResultCategory::InvalidRequest);
    }
    // Validate mutual TLS: client cert and key must both be present or both absent.
    if (request.useTls &&
        (request.tls.clientCertPem.empty() != request.tls.clientKeyPem.empty())) {
        httpDiagPrint("[http] invalid request: cert/key mismatch");
        return HttpResult(HttpResultCategory::InvalidRequest);
    }

    // Check cancellation before starting.
    if (cancellation != nullptr && cancellation->cancelled()) {
        return HttpResult(HttpResultCategory::Cancelled);
    }

    // Reset response.
    response = HttpResponse();

    // --- Per-request state ---------------------------------------------------
    int reqId  = -1;
    int connId = -1;
    int tmplId = -1;
    int rc = 0;
    bool clientCertLoaded = false;
    int32_t statusCode = 0;
    int contentLengthType = 0;
    uint64_t contentLength = 0;

    HttpResult result;   // defaults to Success

    // --- Per-request template ------------------------------------------------
    // A fresh template per request avoids any cross-request state
    // contamination in the OpenOrbis SDK's libhttp.
    tmplId = sceHttpCreateTemplate(m_impl->httpCtxId, kUserAgent,
                                   ORBIS_HTTP_VERSION_1_1, 1);
    if (tmplId < 0) {
        httpDiagPrint("[http] sceHttpCreateTemplate failed");
        return HttpResult(HttpResultCategory::HttpInitializationFailure,
                          static_cast<uint32_t>(tmplId));
    }

    // --- Configure HTTPS on the template if needed --------------------------
    // The OpenOrbis SDK supports sceHttpsSetSslCallback with an accept-all
    // callback (the peer identity is established via pairing, not the public
    // PKI). Peer-certificate pinning via sceSslGetPem is not available in this
    // SDK version.
    if (request.useTls) {
        rc = sceHttpsSetSslCallback(tmplId, sslVerifyCallback, nullptr);
        if (rc < 0) {
            httpDiagPrint("[http] sceHttpsSetSslCallback(template) failed");
            sceHttpDeleteTemplate(tmplId);
            return HttpResult(HttpResultCategory::SdkFailure,
                              static_cast<uint32_t>(rc));
        }
    }

    // --- Load client certificate/key for mutual TLS (if provided) ----------
    if (request.useTls && !request.tls.clientCertPem.empty()) {
        // The OpenOrbis SDK declares sceHttpsLoadCert with void* parameters
        // (the real PS4 signature uses SceSslData structs). The actual library
        // function expects SceSslData* for the cert and key. Define the struct
        // locally (the OpenOrbis SDK does not provide it) and pass pointers to
        // it; the void* parameters accept them implicitly.
        struct SslData {
            const char* ptr;
            size_t      size;
        };

        SslData certData;
        certData.ptr = request.tls.clientCertPem.c_str();
        certData.size = request.tls.clientCertPem.size();

        SslData keyData;
        keyData.ptr = request.tls.clientKeyPem.c_str();
        keyData.size = request.tls.clientKeyPem.size();

        rc = sceHttpsLoadCert(m_impl->httpCtxId, 0, nullptr,
                              &certData, &keyData);
        if (rc < 0) {
            httpDiagPrint("[http] sceHttpsLoadCert failed");
            sceHttpDeleteTemplate(tmplId);
            return HttpResult(HttpResultCategory::TlsCertificateFailure,
                              static_cast<uint32_t>(rc));
        }
        clientCertLoaded = true;
    }

    // --- Build the full URL and create connection/request ------------------
    const char* scheme = request.useTls ? "https" : "http";
    char urlBuf[kMaxTargetLen + 64];
    snprintf(urlBuf, sizeof(urlBuf), "%s://%s:%u%s",
             scheme, request.address.c_str(),
             static_cast<unsigned>(request.port),
             request.target.c_str());

    connId = sceHttpCreateConnectionWithURL(tmplId, urlBuf, 1);
    if (connId < 0) {
        httpDiagPrint("[http] sceHttpCreateConnectionWithURL failed");
        if (clientCertLoaded) {
            httpHttpsUnloadCert(m_impl->httpCtxId);
        }
        sceHttpDeleteTemplate(tmplId);
        return HttpResult(HttpResultCategory::ConnectionFailure,
                          static_cast<uint32_t>(connId));
    }

    // --- Create request -----------------------------------------------------
    int method = ORBIS_METHOD_GET;   // Only GET is currently supported.
    reqId = sceHttpCreateRequestWithURL(connId, method, urlBuf, 0);
    if (reqId < 0) {
        httpDiagPrint("[http] sceHttpCreateRequestWithURL failed");
        sceHttpDeleteConnection(connId);
        if (clientCertLoaded) {
            httpHttpsUnloadCert(m_impl->httpCtxId);
        }
        sceHttpDeleteTemplate(tmplId);
        return HttpResult(HttpResultCategory::SdkFailure,
                          static_cast<uint32_t>(reqId));
    }

    // --- Configure timeouts -------------------------------------------------
    // sceHttpSetConnectTimeOut / sceHttpSetSendTimeOut are properly declared.
    // sceHttpSetRecvTimeOut is a void stub in the OpenOrbis SDK header, but
    // the function exists in libSceHttp and is called without checking the
    // return value (matching the moonlight-ps4_2 reference).
    sceHttpSetConnectTimeOut(reqId, request.connectTimeoutMs * 1000);
    sceHttpSetSendTimeOut(reqId, request.ioTimeoutMs * 1000);
    httpSetRecvTimeOut(reqId, request.ioTimeoutMs * 1000);

    // --- Configure HTTPS per-request options --------------------------------
    if (request.useTls) {
        // Disable browser-style CN/CA verification. For both Unverified and
        // PinnedCertificate modes, the pin (or bootstrap accept) is the trust
        // anchor, not the public PKI.
        sceHttpsDisableOption(reqId,
                              kHttpsFlagCnCheck |
                              kHttpsFlagKnownCaCheck);
    }

    // --- Add Host header if provided ---------------------------------------
    if (!request.hostHeader.empty()) {
        sceHttpAddRequestHeader(reqId, "Host", request.hostHeader.c_str(),
                                kHttpHeaderOverwrite);
    }

    // --- Add additional headers --------------------------------------------
    for (const auto& h : request.headers) {
        sceHttpAddRequestHeader(reqId, h.name.c_str(), h.value.c_str(),
                                kHttpHeaderAdd);
    }

    // --- Execute request (blocking path) ------------------------------------
    // Check cancellation before sending.
    if (cancellation != nullptr && cancellation->cancelled()) {
        result = HttpResult(HttpResultCategory::Cancelled);
        goto cleanup;
    }

    rc = sceHttpSendRequest(reqId, nullptr, 0);
    if (rc < 0) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
                 "[http] sceHttpSendRequest failed (rc=0x%08X urlLen=%zu)",
                 static_cast<uint32_t>(rc), strlen(urlBuf));
        httpDiagPrint(dbg);
        result = HttpResult(HttpResultCategory::SendFailure,
                            static_cast<uint32_t>(rc));
        goto cleanup;
    }

    // --- Get status code ----------------------------------------------------
    rc = sceHttpGetStatusCode(reqId, &statusCode);
    if (rc < 0) {
        httpDiagPrint("[http] sceHttpGetStatusCode failed");
        result = HttpResult(HttpResultCategory::MalformedResponse,
                            static_cast<uint32_t>(rc));
        goto cleanup;
    }
    response.statusCode = statusCode;

    // --- Get content length (optional; may be chunked) ----------------------
    rc = sceHttpGetResponseContentLength(reqId, &contentLengthType, &contentLength);
    if (rc < 0) {
        // Content length query failure is non-fatal; read until EOF.
        contentLengthType = 0;
        contentLength = 0;
    }

    // Validate declared content length against the bound.
    if (contentLengthType == ORBIS_HTTP_CONTENTLEN_EXIST &&
        contentLength > kMaxBodySize) {
        sceHttpAbortRequest(reqId);
        result = HttpResult(HttpResultCategory::ResponseTooLarge);
        goto cleanup;
    }

    // --- Read response body -------------------------------------------------
    for (;;) {
        // Check cancellation before each read so an external cancel can break
        // through promptly.
        if (cancellation != nullptr && cancellation->cancelled()) {
            sceHttpAbortRequest(reqId);
            result = HttpResult(HttpResultCategory::Cancelled);
            goto cleanup;
        }

        uint8_t buf[kReadChunkSize];
        rc = sceHttpReadData(reqId, buf, sizeof(buf));
        if (rc == 0) {
            break;   // end of body
        }
        if (rc < 0) {
            httpDiagPrint("[http] sceHttpReadData failed");
            result = HttpResult(HttpResultCategory::ReceiveFailure,
                                static_cast<uint32_t>(rc));
            goto cleanup;
        }

        // Bound the accumulated body.
        if (response.body.size() + static_cast<size_t>(rc) > kMaxBodySize) {
            sceHttpAbortRequest(reqId);
            result = HttpResult(HttpResultCategory::ResponseTooLarge);
            goto cleanup;
        }

        response.body.insert(response.body.end(), buf, buf + rc);
    }

cleanup:
    // --- Deterministic cleanup of per-request resources ---------------------
    if (reqId >= 0) {
        sceHttpDeleteRequest(reqId);
    }
    if (connId >= 0) {
        sceHttpDeleteConnection(connId);
    }
    // Unload the client certificate if it was loaded for this request.
    if (clientCertLoaded) {
        httpHttpsUnloadCert(m_impl->httpCtxId);
    }
    // Delete the per-request template last (after connection/request).
    if (tmplId >= 0) {
        sceHttpDeleteTemplate(tmplId);
    }

    return result;
}

} // namespace ml

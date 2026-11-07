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

// === HttpClient.h ============================================================
//
// OpenOrbis Moonlight client — HTTP / HTTPS Client subsystem
//
// HttpClient is a small reusable HTTP/HTTPS transport used by Pairing and the
// later Application Catalog. It provides bounded, cancellable requests and
// exposes only project-owned request/response/result types.
//
// HTTPS transport uses the OpenOrbis SDK SSL implementation (libSceSsl)
// through libSceHttp. mbedTLS is NOT used for TLS transport.
//
// This header exposes only project-owned types. OpenOrbis SDK handles, structs,
// constants, and error codes are kept private to the implementation file.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// No OpenOrbis SDK types are exposed.
// ===============================================================================

#ifndef MOONLIGHT_HTTP_CLIENT_H
#define MOONLIGHT_HTTP_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace ml {

// ---------------------------------------------------------------------------
// HTTP method
// ---------------------------------------------------------------------------

/// HTTP methods supported by HttpClient. Only methods concretely required by
/// current consumers (Pairing, App List) are exposed. Both use GET.
enum class HttpMethod : uint8_t {
    Get
};

// ---------------------------------------------------------------------------
// TLS trust modes
// ---------------------------------------------------------------------------

/// How the TLS peer identity is treated.
///
/// Unverified        — TLS encryption is established, but the peer identity is
///                     NOT trusted. Exists only for Pairing bootstrap.
/// PinnedCertificate — The presented server certificate is mechanically
///                     compared against a caller-supplied pin. Used for
///                     already-paired/trusted hosts.
enum class HttpTlsTrustMode : uint8_t {
    Unverified,
    PinnedCertificate
};

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

/// A single HTTP header name/value pair.
struct HttpHeader {
    std::string name;
    std::string value;
};

// ---------------------------------------------------------------------------
// TLS options
// ---------------------------------------------------------------------------

/// TLS trust configuration for an HTTPS request.
struct HttpTlsOptions {
    HttpTlsTrustMode trustMode = HttpTlsTrustMode::Unverified;
    /// For PinnedCertificate: the expected server certificate (PEM-encoded
    /// bytes). For Unverified: ignored.
    std::vector<uint8_t> expectedPeerCertificate;
    /// Optional client certificate (PEM-encoded) for mutual TLS authentication.
    /// When set, `clientKeyPem` must also be set.
    std::string clientCertPem;
    /// Optional client private key (PEM-encoded) for mutual TLS authentication.
    std::string clientKeyPem;
};

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

/// A project-owned HTTP/HTTPS request.
///
/// The consumer supplies the numeric IPv4 address and port (from discovery or
/// serverinfo). HttpClient treats the target as opaque transport data and does
/// not interpret query parameters.
struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string address;        ///< Numeric IPv4, e.g. "192.168.1.50".
    uint16_t    port = 0;       ///< Consumer-provided port (from discovery).
    std::string hostHeader;     ///< Optional Host header value. If empty, the
                                ///< SDK uses the connection address as Host.
    std::string target;         ///< Request path + query, e.g. "/pair?uniqueid=...".
    bool        useTls = false; ///< Whether to use HTTPS.
    HttpTlsOptions tls;         ///< TLS trust configuration (HTTPS only).
    std::vector<HttpHeader> headers;  ///< Additional request headers.
    uint32_t connectTimeoutMs = 3000; ///< Connect timeout.
    uint32_t ioTimeoutMs      = 5000; ///< Send/receive timeout.
};

// ---------------------------------------------------------------------------
// TlsPeerInfo
// ---------------------------------------------------------------------------

/// Project-owned peer TLS certificate information.
/// Populated for HTTPS requests. The certificate is PEM-encoded bytes.
struct TlsPeerInfo {
    std::vector<uint8_t> certificate;
};

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

/// A project-owned HTTP/HTTPS response.
///
/// HTTP status is protocol response data, not a transport failure. A 4xx/5xx
/// status still yields HttpResult::Success; the consumer interprets the status.
struct HttpResponse {
    int statusCode = 0;
    std::vector<uint8_t> body;   ///< Binary-safe response body.
    TlsPeerInfo peer;            ///< Peer certificate (HTTPS only).
};

// ---------------------------------------------------------------------------
// HttpResult
// ---------------------------------------------------------------------------

/// Categories of HTTP/HTTPS operation results.
enum class HttpResultCategory : uint8_t {
    Success,
    InvalidRequest,
    InvalidState,
    HttpInitializationFailure,
    SslInitializationFailure,
    ConnectionFailure,
    ConnectTimeout,
    SendFailure,
    ReceiveFailure,
    IoTimeout,
    Cancelled,
    TlsHandshakeFailure,
    TlsVerificationFailure,
    TlsCertificateFailure,
    MalformedResponse,
    ResponseTooLarge,
    SdkFailure
};

/// A lightweight result returned by HttpClient operations.
/// Preserves a native SDK error code for diagnostics without exposing SDK
/// types in the public interface.
struct HttpResult {
    HttpResultCategory category;
    uint32_t           nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    HttpResult() : category(HttpResultCategory::Success), nativeError(0) {}
    HttpResult(HttpResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == HttpResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// HttpCancellationToken
// ---------------------------------------------------------------------------

/// A caller-owned cancellation flag. HttpClient checks it during a request and
/// aborts the in-flight SDK operation when it becomes true.
///
/// To interrupt a *blocking* SDK call (e.g. sceHttpSendRequest waiting for a
/// slow/held response), HttpClient registers an abort callback on the token
/// before entering the blocking call. When cancel() is invoked from another
/// thread, it both sets the flag and fires the abort callback, which calls
/// sceHttpAbortRequest() on the active request so the blocked call returns
/// promptly instead of waiting out its full timeout.
class HttpCancellationToken {
public:
    HttpCancellationToken() = default;

    /// Returns true if cancellation has been requested.
    bool cancelled() const { return m_cancelled.load(std::memory_order_acquire); }

    /// Request cancellation. Safe to call from any thread.
    /// Sets the flag and, if an abort callback is registered, fires it so an
    /// in-flight blocking SDK request is interrupted.
    void cancel() {
        m_cancelled.store(true, std::memory_order_release);
        AbortFn fn = m_abortFn.load(std::memory_order_acquire);
        if (fn != nullptr) {
            fn(m_abortCtx.load(std::memory_order_acquire));
        }
    }

    /// Register an abort callback (and its context) that cancel() will fire.
    /// Called by HttpClient just before entering a blocking SDK call. The
    /// callback must be safe to invoke from any thread.
    void setAbort(void (*fn)(void*), void* ctx) {
        m_abortCtx.store(ctx, std::memory_order_release);
        m_abortFn.store(fn, std::memory_order_release);
    }

    /// Clear any registered abort callback. Called by HttpClient after the
    /// blocking SDK call returns.
    void clearAbort() {
        m_abortFn.store(nullptr, std::memory_order_release);
        m_abortCtx.store(nullptr, std::memory_order_release);
    }

private:
    using AbortFn = void (*)(void*);
    std::atomic<bool> m_cancelled{false};
    std::atomic<AbortFn> m_abortFn{nullptr};
    std::atomic<void*> m_abortCtx{nullptr};
};

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

/// A small reusable HTTP/HTTPS transport.
///
/// Responsibilities:
///   - Execute HTTP requests (GET).
///   - Execute HTTPS requests using the OpenOrbis SDK SSL implementation.
///   - Provide bounded timeouts and cancellation.
///   - Retrieve the peer TLS certificate during Pairing bootstrap.
///   - Enforce caller-supplied pinned trust for trusted hosts.
///   - Support mutual TLS client certificate authentication.
///   - Translate PS4 HTTP/SSL/Net errors into project-owned result types.
///   - Deterministically clean up request/connection resources.
///
/// Non-responsibilities:
///   - Initialize/terminate Net/NetCtl (NetworkManager owns that).
///   - Decide whether a host should be trusted (PairingManager owns that).
///   - Persist certificates or trust.
///   - Implement Pairing or App List protocol logic.
///   - Create worker threads.
///
/// Threading: HttpClient is synchronous and owns no worker thread. One active
/// request per HttpClient is the explicit initial contract.
class HttpClient {
public:
    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before initialize() is called.
    HttpClient();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~HttpClient();

    // Non-copyable, non-movable.
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /// Initialize the PS4 HTTP/SSL stack. Must be called after NetworkManager
    /// is initialized. After failure the object remains safe to shut down or
    /// destroy.
    HttpResult initialize();

    /// Release all HTTP/SSL resources in reverse dependency order.
    /// Safe after partial initialization, normal loop completion, or repeated
    /// calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Execute one HTTP/HTTPS request synchronously.
    ///
    /// `request` describes the request. `response` receives the result.
    /// `cancellation` is an optional caller-owned token; when it becomes true,
    /// the in-flight request is aborted and Cancelled is returned.
    ///
    /// HTTP 4xx/5xx status codes are returned as response data (HttpResult
    /// Success) — the consumer interprets the status.
    HttpResult perform(const HttpRequest& request,
                       HttpResponse& response,
                       HttpCancellationToken* cancellation = nullptr);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_HTTP_CLIENT_H

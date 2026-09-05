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

// === PairingManager.cpp =====================================================
//
// OpenOrbis Moonlight client — Pairing / Host Identity
//
// Implements the PairingManager concrete pairing subsystem:
//   - Consumes the persistent client identity from PairingStore.
//   - One worker thread running the 5-stage Moonlight/Sunshine pairing protocol.
//   - mbedTLS for pairing-specific crypto only.
//   - HttpClient for all HTTP/HTTPS transport.
//   - Bounded update queue to the main thread.
//   - Deterministic cancellation and shutdown.
//
// OpenOrbis adaptation:
//   - Uses POSIX pthread (pthread_create/join/mutex/cond) matching the proven
//     OpenOrbis threading path used by DiscoveryManager and ServerInfoManager.
//   - Consumes the already-created client identity from PairingStore (the
//     composition root owns the identity and the trust store).
//   - Uses the function-pointer-cast workaround for sceRandomGetRandomNumber
//     (the OpenOrbis SDK declares it with no arguments).
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "PairingManager.h"

#include "HttpClient.h"
#include "PairingStore.h"

// --- mbedTLS headers (pairing crypto only) ------------------------------------
// mbedTLS 3.x hides some struct fields (e.g. mbedtls_x509_crt.sig) behind
// MBEDTLS_PRIVATE(). We need direct access to the certificate signature bytes
// in extractCertSignature(), so enable private access for this translation unit.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pem.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/libkernel.h>   // sceKernelGetProcessTime, sceKernelUsleep
#include <orbis/Random.h>      // sceRandomGetRandomNumber (incorrect stub)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <atomic>
#include <utility>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void pmDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Update queue capacity.
constexpr size_t kUpdateQueueCapacity = 16;

/// Maximum pairing response body size (64 KiB).
constexpr size_t kMaxPairingBodySize = 64 * 1024;

/// Maximum server certificate size (16 KiB).
constexpr size_t kMaxServerCertSize = 16 * 1024;

/// Maximum client certificate PEM size.
constexpr size_t kMaxClientCertPemSize = 8192;

/// Maximum client key PEM size.
constexpr size_t kMaxClientKeyPemSize = 4096;

/// DRBG personalization string.
constexpr const char* kDrbgPers = "moonlight-ps4-pairing";

/// Worker idle sleep in microseconds.
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

/// Maximum number of IPv4 addresses to try per request.
constexpr size_t kMaxAddressAttempts = 4;

/// Default HTTPS port fallback.
constexpr uint16_t kDefaultHttpsPort = 47984;

/// Default HTTP port fallback.
constexpr uint16_t kDefaultHttpPort = 47989;

/// PIN length.
constexpr size_t kPinLength = 4;

/// Salt length.
constexpr size_t kSaltBytes = 16;

/// AES key length.
constexpr size_t kAesKeyBytes = 16;

/// SHA-256 hash length.
constexpr size_t kSha256Bytes = 32;

/// Client challenge length.
constexpr size_t kChallengeBytes = 16;

/// Client secret length.
constexpr size_t kClientSecretBytes = 16;

/// Server secret length.
constexpr size_t kServerSecretBytes = 16;

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
/// type. This is the safe function-pointer-cast technique used in the earlier
/// pairing work. The RANDOM sysmodule is loaded once by SysmoduleManager at
/// process startup; we do not reload it here.
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

/// Random number generator callback for mbedTLS CTR_DRBG.
int pairingRngFunc(void* /*p_rng*/, unsigned char* output, size_t output_len) {
    if (output == nullptr || output_len == 0) {
        return -1;
    }
    size_t offset = 0;
    while (offset < output_len) {
        uint8_t temp[16];
        size_t chunk = output_len - offset;
        if (chunk > sizeof(temp)) {
            chunk = sizeof(temp);
        }
        if (!generateRandomBytes(temp, chunk)) {
            return -1;
        }
        memcpy(output + offset, temp, chunk);
        offset += chunk;
    }
    return 0;
}

/// Entropy source callback for mbedTLS using the PS4 RANDOM sysmodule.
int pairingEntropyFunc(void* /*data*/, unsigned char* output, size_t len, size_t* olen) {
    if (output == nullptr || olen == nullptr) {
        return -1;
    }
    size_t offset = 0;
    while (offset < len) {
        uint8_t temp[16];
        size_t chunk = len - offset;
        if (chunk > sizeof(temp)) {
            chunk = sizeof(temp);
        }
        if (!generateRandomBytes(temp, chunk)) {
            *olen = offset;
            return -1;
        }
        memcpy(output + offset, temp, chunk);
        offset += chunk;
    }
    *olen = len;
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of PairingManager.
struct PairingManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    bool threadCreated = false;
    pthread_t workerThread;

    // --- HttpClient dependency (non-owning) ---------------------------------
    HttpClient* httpClient = nullptr;

    // --- PairingStore dependency (non-owning) -------------------------------
    PairingStore* store = nullptr;

    // --- Client identity (consumed from PairingStore) -----------------------
    ClientIdentity identity;
    bool identityReady = false;

    // --- Active pairing state ----------------------------------------------
    std::atomic<bool> pairingActive{false};
    std::atomic<bool> cancelRequested{false};
    PairingTarget activeTarget;
    HttpCancellationToken* activeCancellation = nullptr;   // Guarded by stateMutex.

    // --- Update queue -------------------------------------------------------
    pthread_mutex_t updateMutex;
    bool updateMutexInit = false;
    std::deque<PairingUpdate> updateQueue;

    // --- State mutex (for active pairing state) -----------------------------
    pthread_mutex_t stateMutex;
    bool stateMutexInit = false;

    // --- Worker wake --------------------------------------------------------
    pthread_mutex_t wakeMutex;
    bool wakeMutexInit = false;
    pthread_cond_t wakeCond;
    bool wakeCondInit = false;
};

// ---------------------------------------------------------------------------
// Crypto helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

/// Generate a 4-digit numeric PIN.
bool generatePin(std::string& outPin) {
    uint8_t rnd[kPinLength];
    if (!generateRandomBytes(rnd, sizeof(rnd))) {
        return false;
    }
    char pinBuf[kPinLength + 1];
    for (size_t i = 0; i < kPinLength; ++i) {
        pinBuf[i] = static_cast<char>('0' + (rnd[i] % 10));
    }
    pinBuf[kPinLength] = '\0';
    outPin = pinBuf;
    return true;
}

/// Generate a 16-byte salt, hex-encoded.
bool generateSaltHex(std::string& outSaltHex) {
    uint8_t salt[kSaltBytes];
    if (!generateRandomBytes(salt, sizeof(salt))) {
        return false;
    }
    static const char kHex[] = "0123456789ABCDEF";
    outSaltHex.clear();
    outSaltHex.reserve(kSaltBytes * 2);
    for (size_t i = 0; i < kSaltBytes; ++i) {
        outSaltHex.push_back(kHex[(salt[i] >> 4) & 0x0F]);
        outSaltHex.push_back(kHex[salt[i] & 0x0F]);
    }
    return true;
}

/// Generate a random UUID string (36 chars + null).
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

/// Hex string to bytes.
bool hexToBytes(const char* hex, size_t hexLen, std::vector<uint8_t>& out) {
    if (hex == nullptr || (hexLen % 2) != 0) {
        return false;
    }
    const size_t byteLen = hexLen / 2;
    out.resize(byteLen);
    for (size_t i = 0; i < byteLen; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return -1;
        };
        int h = nibble(hi);
        int l = nibble(lo);
        if (h < 0 || l < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((h << 4) | l);
    }
    return true;
}

/// Compute SHA-256.
bool computeSha256(const uint8_t* data, size_t size, uint8_t* outHash, size_t hashSize) {
    if (data == nullptr || outHash == nullptr || hashSize < kSha256Bytes) {
        return false;
    }
    return mbedtls_sha256(data, size, outHash, 0) == 0;
}

/// Derive AES-128 key from PIN and salt.
bool deriveAesKey(const char* pin, const uint8_t* salt, size_t saltSize,
                  uint8_t* outKey, size_t keySize) {
    if (pin == nullptr || salt == nullptr || outKey == nullptr || keySize < kAesKeyBytes) {
        return false;
    }
    const size_t pinLen = strlen(pin);
    if (pinLen == 0) {
        return false;
    }

    uint8_t shaInput[64];
    if (pinLen + saltSize > sizeof(shaInput)) {
        return false;
    }
    memcpy(shaInput, salt, saltSize);
    memcpy(shaInput + saltSize, pin, pinLen);

    uint8_t hash[kSha256Bytes];
    if (!computeSha256(shaInput, pinLen + saltSize, hash, sizeof(hash))) {
        return false;
    }
    memcpy(outKey, hash, kAesKeyBytes);
    return true;
}

/// AES-128-ECB encrypt (no padding).
bool aes128EcbEncrypt(const uint8_t* key, size_t keySize,
                      const uint8_t* input, size_t inputSize,
                      uint8_t* output, size_t outputSize) {
    if (key == nullptr || input == nullptr || output == nullptr ||
        keySize < 16 || outputSize < inputSize || (inputSize % 16) != 0) {
        return false;
    }
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }
    for (size_t i = 0; i < inputSize; i += 16) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input + i, output + i);
        if (ret != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }
    }
    mbedtls_aes_free(&aes);
    return true;
}

/// AES-128-ECB decrypt (no padding).
bool aes128EcbDecrypt(const uint8_t* key, size_t keySize,
                      const uint8_t* input, size_t inputSize,
                      uint8_t* output, size_t outputSize) {
    if (key == nullptr || input == nullptr || output == nullptr ||
        keySize < 16 || outputSize < inputSize || (inputSize % 16) != 0) {
        return false;
    }
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }
    for (size_t i = 0; i < inputSize; i += 16) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input + i, output + i);
        if (ret != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }
    }
    mbedtls_aes_free(&aes);
    return true;
}

/// Extract the text content of a single XML tag from a byte buffer.
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

/// Parse a decimal integer from a string.
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

/// Extract the raw ASN.1 signature bytes from a client certificate.
bool extractCertSignature(const std::string& certPem, std::vector<uint8_t>& outSig) {
    outSig.clear();
    if (certPem.empty()) {
        return false;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt,
                                     reinterpret_cast<const unsigned char*>(certPem.c_str()),
                                     certPem.size() + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;
    }
    if (crt.sig.p != nullptr && crt.sig.len > 0) {
        outSig.assign(crt.sig.p, crt.sig.p + crt.sig.len);
    }
    mbedtls_x509_crt_free(&crt);
    return !outSig.empty();
}

/// Sign data with the client private key (RSA-SHA256).
bool signDataSha256(const std::vector<uint8_t>& keyDer, const uint8_t* data, size_t dataSize,
                    std::vector<uint8_t>& outSig) {
    outSig.clear();
    if (keyDer.empty() || data == nullptr) {
        return false;
    }

    uint8_t hash[kSha256Bytes];
    if (!computeSha256(data, dataSize, hash, sizeof(hash))) {
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;

    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    int ret = mbedtls_entropy_add_source(&entropy, pairingEntropyFunc, nullptr,
                                         32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) goto cleanup;

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                reinterpret_cast<const unsigned char*>(kDrbgPers),
                                strlen(kDrbgPers));
    if (ret != 0) goto cleanup;

    ret = mbedtls_pk_parse_key(&pk, keyDer.data(), keyDer.size(), nullptr, 0,
                               nullptr, nullptr);
    if (ret != 0) {
        char dbg[192];
        snprintf(dbg, sizeof(dbg),
                 "[pairing] signDataSha256: key parse FAILED ret=-0x%04X keyDerLen=%zu",
                 static_cast<unsigned>(-ret), keyDer.size());
        pmDiagPrint(dbg);
        goto cleanup;
    }

    {
        std::vector<uint8_t> sig(mbedtls_pk_get_len(&pk));
        if (sig.empty()) {
            ret = -1;
            goto cleanup;
        }
        size_t sigLen = 0;
        ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                              sig.data(), sig.size(), &sigLen,
                              mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0) {
            char dbg[192];
            snprintf(dbg, sizeof(dbg),
                     "[pairing] signDataSha256: sign FAILED ret=-0x%04X keyBits=%u",
                     static_cast<unsigned>(-ret),
                     static_cast<unsigned>(mbedtls_pk_get_bitlen(&pk)));
            pmDiagPrint(dbg);
            goto cleanup;
        }
        sig.resize(sigLen);
        outSig = std::move(sig);
        {
            char dbg[192];
            snprintf(dbg, sizeof(dbg),
                     "[pairing] signDataSha256: OK sigLen=%zu keyBits=%u",
                     outSig.size(),
                     static_cast<unsigned>(mbedtls_pk_get_bitlen(&pk)));
            pmDiagPrint(dbg);
        }
    }

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return true;

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return false;
}

/// Verify a signature against a certificate's public key (RSA-SHA256).
bool verifySignatureSha256(const uint8_t* data, size_t dataSize,
                           const uint8_t* signature, size_t signatureSize,
                           const std::string& certPem) {
    if (data == nullptr || signature == nullptr || certPem.empty()) {
        return false;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt,
                                     reinterpret_cast<const unsigned char*>(certPem.c_str()),
                                     certPem.size() + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;
    }

    uint8_t hash[kSha256Bytes];
    ret = mbedtls_sha256(data, dataSize, hash, 0);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;
    }

    ret = mbedtls_pk_verify(&crt.pk, MBEDTLS_MD_SHA256, hash, 0,
                            signature, signatureSize);
    mbedtls_x509_crt_free(&crt);
    return ret == 0;
}

/// Convert PEM certificate bytes to DER using mbedTLS.
bool pemToDer(const std::string& pem, std::vector<uint8_t>& outDer) {
    outDer.clear();
    if (pem.empty()) {
        return false;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt,
                                     reinterpret_cast<const unsigned char*>(pem.c_str()),
                                     pem.size() + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;
    }
    outDer.assign(crt.raw.p, crt.raw.p + crt.raw.len);
    mbedtls_x509_crt_free(&crt);
    return !outDer.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Worker thread entry
// ---------------------------------------------------------------------------

void* PairingManager::pairingWorker(void* arg) {
    PairingManager::Impl* impl = static_cast<PairingManager::Impl*>(arg);

    pmDiagPrint("[pairing] worker started");

    // Copy the active target (owned by the worker).
    PairingTarget target;
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
    auto pushUpdate = [impl](PairingUpdateType type, const char* status,
                             const std::string& pin = std::string(),
                             PairingResultCategory failCat = PairingResultCategory::Success) {
        PairingUpdate update;
        update.type = type;
        update.hostId = impl->activeTarget.hostId;
        update.pin = pin;
        update.statusText = status ? status : "";
        update.failureCategory = failCat;
        pthread_mutex_lock(&impl->updateMutex);
        if (impl->updateQueue.size() >= kUpdateQueueCapacity) {
            // Drop oldest non-final update.
            impl->updateQueue.pop_front();
        }
        impl->updateQueue.push_back(std::move(update));
        pthread_mutex_unlock(&impl->updateMutex);
    };

    // Helper to finish the worker (cleanup + return).
    auto finishWorker = [impl]() -> void* {
        // Clear the active cancellation pointer.
        pthread_mutex_lock(&impl->stateMutex);
        impl->activeCancellation = nullptr;
        pthread_mutex_unlock(&impl->stateMutex);
        // Clear the pairing-active flag.
        impl->pairingActive = false;
        pmDiagPrint("[pairing] worker stopped");
        return nullptr;
    };

    // --- Check cancellation before starting ---------------------------------
    if (impl->cancelRequested.load() || impl->stopRequested.load()) {
        pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
        return finishWorker();
    }

    // --- Ensure identity is available ---------------------------------------
    if (!impl->identityReady) {
        pushUpdate(PairingUpdateType::Failed, "Client identity unavailable.",
                   std::string(), PairingResultCategory::IdentityUnavailable);
        return finishWorker();
    }

    // --- Generate PIN and salt ----------------------------------------------
    std::string pin;
    if (!generatePin(pin)) {
        pushUpdate(PairingUpdateType::Failed, "Failed to generate PIN.",
                   std::string(), PairingResultCategory::RandomGenerationFailure);
        return finishWorker();
    }

    std::string saltHex;
    if (!generateSaltHex(saltHex)) {
        pushUpdate(PairingUpdateType::Failed, "Failed to generate salt.",
                   std::string(), PairingResultCategory::RandomGenerationFailure);
        return finishWorker();
    }

    // Emit PinReady.
    pushUpdate(PairingUpdateType::PinReady, "Enter this PIN on the host.", pin);

    // Diagnostic: print the PIN so it appears in klog. The host needs this
    // PIN to complete its part of Phase 1 (getservercert).
    {
        char pinBuf[64];
        snprintf(pinBuf, sizeof(pinBuf), "[pairing] PIN: %s", pin.c_str());
        pmDiagPrint(pinBuf);
    }

    // --- Stage 1: Get Server Certificate (HTTP) -----------------------------
    {
        pushUpdate(PairingUpdateType::Progress, "Waiting for PIN entry");

        // Diagnostic: log the unique ID used for pairing so we can verify it
        // matches the unique ID used for the serverinfo query.
        {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "[pairing] pairing uniqueid='%s' devicename='%s'",
                     impl->identity.uniqueId.c_str(),
                     impl->identity.deviceName.c_str());
            pmDiagPrint(buf);
        }

        // Hex-encode the client certificate PEM.
        std::string clientCertHex;
        if (!bytesToHex(reinterpret_cast<const uint8_t*>(impl->identity.certPem.data()),
                        impl->identity.certPem.size(), clientCertHex)) {
            pushUpdate(PairingUpdateType::Failed, "Failed to encode client certificate.",
                       std::string(), PairingResultCategory::CryptoFailure);
            return finishWorker();
        }

        std::string uuid;
        if (!generateUuid(uuid)) {
            pushUpdate(PairingUpdateType::Failed, "Failed to generate request UUID.",
                       std::string(), PairingResultCategory::RandomGenerationFailure);
            return finishWorker();
        }

        // Build the request target.
        std::string targetStr = "/pair?uniqueid=" + impl->identity.uniqueId +
                                "&uuid=" + uuid +
                                "&devicename=" + impl->identity.deviceName +
                                "&updateState=1" +
                                "&phrase=getservercert" +
                                "&salt=" + saltHex +
                                "&clientcert=" + clientCertHex;

        // Try each known IPv4 address.
        bool stageDone = false;
        for (const auto& addr : target.ipv4Addresses) {
            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }

            HttpRequest httpReq;
            httpReq.method = HttpMethod::Get;
            httpReq.address = addr;
            httpReq.port = (target.discoveryPort != 0) ? target.discoveryPort : kDefaultHttpPort;
            httpReq.target = targetStr;
            httpReq.useTls = false;
            // The host blocks the getservercert response until the user enters
            // the PIN on the host (which stores the salt+PIN and derives the
            // shared AES key). Use a long timeout so the client waits for the
            // PIN entry; the request is interrupted promptly when the user
            // cancels pairing (CancelPairing intent fires the cancellation
            // token). This ties the PIN wait to the pairing screen being open.
            httpReq.connectTimeoutMs = 120000;
            httpReq.ioTimeoutMs = 120000;

            // Diagnostic: log the Stage 1 request details (address, port, URL
            // length) so a send failure can be correlated.
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg),
                         "[pairing] stage1 addr='%s' port=%u targetLen=%zu (waiting for PIN entry)",
                         addr.c_str(),
                         static_cast<unsigned>(httpReq.port),
                         targetStr.size());
                pmDiagPrint(dbg);
            }

            HttpResponse httpResp;
            HttpResult httpResult = impl->httpClient->perform(httpReq, httpResp, &requestCancellation);

            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }
            if (!httpResult.succeeded()) {
                continue;   // Try next address.
            }
            if (httpResp.statusCode != 200) {
                pushUpdate(PairingUpdateType::Failed, "Host rejected pairing request.",
                           std::string(), PairingResultCategory::ProtocolFailure);
                return finishWorker();
            }
            if (httpResp.body.empty() || httpResp.body.size() > kMaxPairingBodySize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Parse the response: <paired> and <plaincert>.
            std::string pairedText;
            std::string plainCertHex;
            if (!extractXmlTag(httpResp.body.data(), httpResp.body.size(), "paired", pairedText) ||
                !extractXmlTag(httpResp.body.data(), httpResp.body.size(), "plaincert", plainCertHex)) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            int pairedValue = 0;
            if (!parseDecimal(pairedText, pairedValue) || pairedValue != 1) {
                pushUpdate(PairingUpdateType::Failed, "Host rejected pairing request.",
                           std::string(), PairingResultCategory::ProtocolFailure);
                return finishWorker();
            }

            // Decode the server certificate (hex -> PEM).
            std::vector<uint8_t> serverCertPemBytes;
            if (!hexToBytes(plainCertHex.c_str(), plainCertHex.size(), serverCertPemBytes) ||
                serverCertPemBytes.empty() || serverCertPemBytes.size() > kMaxServerCertSize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid server certificate.",
                           std::string(), PairingResultCategory::CertificateFailure);
                return finishWorker();
            }
            std::string serverCertPem(reinterpret_cast<const char*>(serverCertPemBytes.data()),
                                      serverCertPemBytes.size());

            // Decode the salt (hex -> bytes).
            std::vector<uint8_t> saltBytes;
            if (!hexToBytes(saltHex.c_str(), saltHex.size(), saltBytes) ||
                saltBytes.size() != kSaltBytes) {
                pushUpdate(PairingUpdateType::Failed, "Invalid salt.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            // Derive the AES-128 key from PIN and salt.
            uint8_t aesKey[kAesKeyBytes];
            if (!deriveAesKey(pin.c_str(), saltBytes.data(), saltBytes.size(),
                              aesKey, sizeof(aesKey))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to derive AES key.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            // --- Stage 2: Client Challenge (HTTP) ---------------------------
            pushUpdate(PairingUpdateType::Progress, "Exchanging challenges...");

            // Generate a 16-byte client challenge.
            uint8_t clientChallenge[kChallengeBytes];
            if (!generateRandomBytes(clientChallenge, sizeof(clientChallenge))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate challenge.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            // AES-128-ECB encrypt the challenge.
            uint8_t encryptedChallenge[kChallengeBytes];
            if (!aes128EcbEncrypt(aesKey, sizeof(aesKey),
                                  clientChallenge, sizeof(clientChallenge),
                                  encryptedChallenge, sizeof(encryptedChallenge))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to encrypt challenge.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            std::string challengeHex;
            if (!bytesToHex(encryptedChallenge, sizeof(encryptedChallenge), challengeHex)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to encode challenge.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            std::string uuid2;
            if (!generateUuid(uuid2)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate request UUID.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            std::string stage2Target = "/pair?uniqueid=" + impl->identity.uniqueId +
                                       "&uuid=" + uuid2 +
                                       "&devicename=" + impl->identity.deviceName +
                                       "&updateState=1" +
                                       "&clientchallenge=" + challengeHex;

            HttpRequest stage2Req;
            stage2Req.method = HttpMethod::Get;
            stage2Req.address = addr;
            stage2Req.port = (target.discoveryPort != 0) ? target.discoveryPort : kDefaultHttpPort;
            stage2Req.target = stage2Target;
            stage2Req.useTls = false;
            stage2Req.connectTimeoutMs = 3000;
            stage2Req.ioTimeoutMs = 5000;

            HttpResponse stage2Resp;
            HttpResult stage2Result = impl->httpClient->perform(stage2Req, stage2Resp, &requestCancellation);

            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }
            if (!stage2Result.succeeded()) {
                pushUpdate(PairingUpdateType::Failed, "Unable to contact host.",
                           std::string(), PairingResultCategory::TransportFailure);
                return finishWorker();
            }
            if (stage2Resp.statusCode != 200 || stage2Resp.body.empty() ||
                stage2Resp.body.size() > kMaxPairingBodySize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Parse <challengeresponse>.
            std::string challengeResponseHex;
            if (!extractXmlTag(stage2Resp.body.data(), stage2Resp.body.size(),
                               "challengeresponse", challengeResponseHex) ||
                challengeResponseHex.empty()) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Decode and decrypt the challenge response.
            std::vector<uint8_t> encryptedResponse;
            if (!hexToBytes(challengeResponseHex.c_str(), challengeResponseHex.size(),
                            encryptedResponse) ||
                encryptedResponse.empty() || (encryptedResponse.size() % 16) != 0) {
                pushUpdate(PairingUpdateType::Failed, "Invalid challenge response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            std::vector<uint8_t> decryptedResponse(encryptedResponse.size());
            if (!aes128EcbDecrypt(aesKey, sizeof(aesKey),
                                  encryptedResponse.data(), encryptedResponse.size(),
                                  decryptedResponse.data(), decryptedResponse.size())) {
                pushUpdate(PairingUpdateType::Failed, "Failed to decrypt challenge response.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            // The decrypted response is 48 bytes: [32-byte serverHash][16-byte serverChallenge].
            if (decryptedResponse.size() != 48) {
                pushUpdate(PairingUpdateType::Failed, "Invalid challenge response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }
            uint8_t serverChallenge[kChallengeBytes];
            memcpy(serverChallenge, decryptedResponse.data() + 32, kChallengeBytes);

            // --- Stage 3: Server Challenge Response (HTTP) ------------------
            pushUpdate(PairingUpdateType::Progress, "Verifying host identity...");

            // Generate a 16-byte client secret.
            uint8_t clientSecret[kClientSecretBytes];
            if (!generateRandomBytes(clientSecret, sizeof(clientSecret))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate client secret.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            // Extract the client certificate signature.
            std::vector<uint8_t> clientCertSignature;
            if (!extractCertSignature(impl->identity.certPem, clientCertSignature) ||
                clientCertSignature.empty()) {
                pushUpdate(PairingUpdateType::Failed, "Failed to extract client certificate signature.",
                           std::string(), PairingResultCategory::CertificateFailure);
                return finishWorker();
            }

            // Compute SHA256(serverChallenge || clientCertSignature || clientSecret).
            std::vector<uint8_t> hashInput;
            hashInput.reserve(kChallengeBytes + clientCertSignature.size() + kClientSecretBytes);
            hashInput.insert(hashInput.end(), serverChallenge, serverChallenge + kChallengeBytes);
            hashInput.insert(hashInput.end(), clientCertSignature.begin(), clientCertSignature.end());
            hashInput.insert(hashInput.end(), clientSecret, clientSecret + kClientSecretBytes);

            uint8_t clientHash[kSha256Bytes];
            if (!computeSha256(hashInput.data(), hashInput.size(), clientHash, sizeof(clientHash))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to compute hash.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            // AES-128-ECB encrypt the hash.
            uint8_t encryptedHash[kSha256Bytes];
            if (!aes128EcbEncrypt(aesKey, sizeof(aesKey),
                                  clientHash, sizeof(clientHash),
                                  encryptedHash, sizeof(encryptedHash))) {
                pushUpdate(PairingUpdateType::Failed, "Failed to encrypt hash.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            std::string serverChallengeRespHex;
            if (!bytesToHex(encryptedHash, sizeof(encryptedHash), serverChallengeRespHex)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to encode hash.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            std::string uuid3;
            if (!generateUuid(uuid3)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate request UUID.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            std::string stage3Target = "/pair?uniqueid=" + impl->identity.uniqueId +
                                       "&uuid=" + uuid3 +
                                       "&devicename=" + impl->identity.deviceName +
                                       "&updateState=1" +
                                       "&serverchallengeresp=" + serverChallengeRespHex;

            HttpRequest stage3Req;
            stage3Req.method = HttpMethod::Get;
            stage3Req.address = addr;
            stage3Req.port = (target.discoveryPort != 0) ? target.discoveryPort : kDefaultHttpPort;
            stage3Req.target = stage3Target;
            stage3Req.useTls = false;
            stage3Req.connectTimeoutMs = 3000;
            stage3Req.ioTimeoutMs = 5000;

            HttpResponse stage3Resp;
            HttpResult stage3Result = impl->httpClient->perform(stage3Req, stage3Resp, &requestCancellation);

            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }
            if (!stage3Result.succeeded()) {
                pushUpdate(PairingUpdateType::Failed, "Unable to contact host.",
                           std::string(), PairingResultCategory::TransportFailure);
                return finishWorker();
            }
            if (stage3Resp.statusCode != 200 || stage3Resp.body.empty() ||
                stage3Resp.body.size() > kMaxPairingBodySize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Parse <pairingsecret>.
            std::string pairingSecretHex;
            if (!extractXmlTag(stage3Resp.body.data(), stage3Resp.body.size(),
                               "pairingsecret", pairingSecretHex) ||
                pairingSecretHex.empty()) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Decode pairingsecret: [16-byte serverSecret][signature].
            std::vector<uint8_t> pairingSecretBytes;
            if (!hexToBytes(pairingSecretHex.c_str(), pairingSecretHex.size(),
                            pairingSecretBytes) ||
                pairingSecretBytes.size() <= kServerSecretBytes) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing secret.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Verify the server's signature (MITM protection).
            if (!verifySignatureSha256(pairingSecretBytes.data(), kServerSecretBytes,
                                       pairingSecretBytes.data() + kServerSecretBytes,
                                       pairingSecretBytes.size() - kServerSecretBytes,
                                       serverCertPem)) {
                pushUpdate(PairingUpdateType::Failed, "Host identity verification failed.",
                           std::string(), PairingResultCategory::TrustVerificationFailure);
                return finishWorker();
            }

            // --- Stage 4: Client Pairing Secret (HTTP) ----------------------
            pushUpdate(PairingUpdateType::Progress, "Completing pairing...");

            // Sign the client secret with the client private key.
            std::vector<uint8_t> clientSecretSignature;
            if (!signDataSha256(impl->identity.keyDer, clientSecret, sizeof(clientSecret),
                                clientSecretSignature) ||
                clientSecretSignature.empty()) {
                pushUpdate(PairingUpdateType::Failed, "Failed to sign client secret.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            // Build clientPairingSecret = clientSecret || signature.
            std::vector<uint8_t> clientPairingSecret;
            clientPairingSecret.reserve(kClientSecretBytes + clientSecretSignature.size());
            clientPairingSecret.insert(clientPairingSecret.end(),
                                       clientSecret, clientSecret + kClientSecretBytes);
            clientPairingSecret.insert(clientPairingSecret.end(),
                                       clientSecretSignature.begin(), clientSecretSignature.end());

            std::string clientPairingSecretHex;
            if (!bytesToHex(clientPairingSecret.data(), clientPairingSecret.size(),
                            clientPairingSecretHex)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to encode pairing secret.",
                           std::string(), PairingResultCategory::CryptoFailure);
                return finishWorker();
            }

            std::string uuid4;
            if (!generateUuid(uuid4)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate request UUID.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            std::string stage4Target = "/pair?uniqueid=" + impl->identity.uniqueId +
                                       "&uuid=" + uuid4 +
                                       "&devicename=" + impl->identity.deviceName +
                                       "&updateState=1" +
                                       "&clientpairingsecret=" + clientPairingSecretHex;

            HttpRequest stage4Req;
            stage4Req.method = HttpMethod::Get;
            stage4Req.address = addr;
            stage4Req.port = (target.discoveryPort != 0) ? target.discoveryPort : kDefaultHttpPort;
            stage4Req.target = stage4Target;
            stage4Req.useTls = false;
            stage4Req.connectTimeoutMs = 3000;
            stage4Req.ioTimeoutMs = 5000;

            HttpResponse stage4Resp;
            HttpResult stage4Result = impl->httpClient->perform(stage4Req, stage4Resp, &requestCancellation);

            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }
            if (!stage4Result.succeeded()) {
                pushUpdate(PairingUpdateType::Failed, "Unable to contact host.",
                           std::string(), PairingResultCategory::TransportFailure);
                return finishWorker();
            }
            if (stage4Resp.statusCode != 200 || stage4Resp.body.empty() ||
                stage4Resp.body.size() > kMaxPairingBodySize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Parse <paired>.
            std::string stage4PairedText;
            if (!extractXmlTag(stage4Resp.body.data(), stage4Resp.body.size(),
                               "paired", stage4PairedText)) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }
            int stage4Paired = 0;
            if (!parseDecimal(stage4PairedText, stage4Paired) || stage4Paired != 1) {
                pushUpdate(PairingUpdateType::Failed, "Host rejected pairing request.",
                           std::string(), PairingResultCategory::ProtocolFailure);
                return finishWorker();
            }

            // --- Stage 5: Pair Challenge (HTTPS with client cert) -----------
            pushUpdate(PairingUpdateType::Progress, "Confirming pairing...");

            std::string uuid5;
            if (!generateUuid(uuid5)) {
                pushUpdate(PairingUpdateType::Failed, "Failed to generate request UUID.",
                           std::string(), PairingResultCategory::RandomGenerationFailure);
                return finishWorker();
            }

            std::string stage5Target = "/pair?uniqueid=" + impl->identity.uniqueId +
                                       "&uuid=" + uuid5 +
                                       "&devicename=" + impl->identity.deviceName +
                                       "&updateState=1" +
                                       "&phrase=pairchallenge";

            HttpRequest stage5Req;
            stage5Req.method = HttpMethod::Get;
            stage5Req.address = addr;
            stage5Req.port = (target.httpsPort != 0) ? target.httpsPort : kDefaultHttpsPort;
            stage5Req.target = stage5Target;
            stage5Req.useTls = true;
            stage5Req.tls.trustMode = HttpTlsTrustMode::Unverified;
            stage5Req.tls.clientCertPem = impl->identity.certPem;
            stage5Req.tls.clientKeyPem = impl->identity.keyPem;
            stage5Req.connectTimeoutMs = 3000;
            stage5Req.ioTimeoutMs = 5000;

            HttpResponse stage5Resp;
            HttpResult stage5Result = impl->httpClient->perform(stage5Req, stage5Resp, &requestCancellation);

            if (impl->cancelRequested.load() || impl->stopRequested.load()) {
                pushUpdate(PairingUpdateType::Cancelled, "Pairing cancelled.");
                return finishWorker();
            }
            if (!stage5Result.succeeded()) {
                pushUpdate(PairingUpdateType::Failed, "Unable to confirm pairing.",
                           std::string(), PairingResultCategory::TransportFailure);
                return finishWorker();
            }
            if (stage5Resp.statusCode != 200 || stage5Resp.body.empty() ||
                stage5Resp.body.size() > kMaxPairingBodySize) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }

            // Parse <paired>.
            std::string stage5PairedText;
            if (!extractXmlTag(stage5Resp.body.data(), stage5Resp.body.size(),
                               "paired", stage5PairedText)) {
                pushUpdate(PairingUpdateType::Failed, "Invalid pairing response.",
                           std::string(), PairingResultCategory::MalformedResponse);
                return finishWorker();
            }
            int stage5Paired = 0;
            if (!parseDecimal(stage5PairedText, stage5Paired) || stage5Paired != 1) {
                pushUpdate(PairingUpdateType::Failed, "Host rejected pairing request.",
                           std::string(), PairingResultCategory::ProtocolFailure);
                return finishWorker();
            }

            // --- Persist trusted host ---------------------------------------
            pushUpdate(PairingUpdateType::Progress, "Saving pairing information...");

            // Convert the server certificate PEM to DER for persistence.
            std::vector<uint8_t> serverCertDer;
            if (!pemToDer(serverCertPem, serverCertDer) || serverCertDer.empty()) {
                pushUpdate(PairingUpdateType::Failed, "Failed to process server certificate.",
                           std::string(), PairingResultCategory::CertificateFailure);
                return finishWorker();
            }

            TrustedHost trustedHost;
            trustedHost.serverId = target.serverId;
            trustedHost.peerCertificateDer = std::move(serverCertDer);
            trustedHost.serviceFqdn = "";
            // Persist the host's display name (from discovery) as the hostname
            // association hint so a restored offline host shows a meaningful
            // name instead of a raw serverId UUID.
            trustedHost.hostname = target.displayName;
            // Persist the last-known endpoint so a paired host can be probed
            // via HTTPS after restart even when mDNS is unavailable. The
            // numeric IPv4 is the bootstrap identity; the discovery port is
            // the HTTP bootstrap port and the HTTPS port comes from serverinfo.
            if (!target.ipv4Addresses.empty()) {
                trustedHost.lastKnownIpv4 = target.ipv4Addresses[0];
            }
            trustedHost.lastKnownDiscoveryPort = target.discoveryPort;
            trustedHost.lastKnownHttpsPort = target.httpsPort;

            if (impl->store == nullptr || !impl->store->addTrustedHost(trustedHost)) {
                pushUpdate(PairingUpdateType::Failed, "Unable to save pairing information.",
                           std::string(), PairingResultCategory::PersistenceFailure);
                return finishWorker();
            }

            // --- Success ----------------------------------------------------
            pushUpdate(PairingUpdateType::Succeeded, "Pairing successful.");
            stageDone = true;
            break;
        }

        if (!stageDone) {
            pushUpdate(PairingUpdateType::Failed, "Unable to contact host.",
                       std::string(), PairingResultCategory::TransportFailure);
            return finishWorker();
        }
    }

    // Normal completion path.
    return finishWorker();
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

PairingManager::PairingManager()
    : m_impl(new Impl())
{
}

PairingManager::~PairingManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

PairingResult PairingManager::initialize(HttpClient* httpClient,
                                         PairingStore* store,
                                         const ClientIdentity& identity) {
    if (m_impl->running.load()) {
        return PairingResult(PairingResultCategory::InvalidState);
    }
    if (httpClient == nullptr || !httpClient->isInitialized()) {
        return PairingResult(PairingResultCategory::InvalidState);
    }
    if (store == nullptr) {
        return PairingResult(PairingResultCategory::InvalidState);
    }

    m_impl->httpClient = httpClient;
    m_impl->store = store;

    // Initialize mutexes.
    if (!m_impl->updateMutexInit) {
        int rc = pthread_mutex_init(&m_impl->updateMutex, nullptr);
        if (rc != 0) {
            return PairingResult(PairingResultCategory::InvalidState,
                                 static_cast<uint32_t>(rc));
        }
        m_impl->updateMutexInit = true;
    }
    if (!m_impl->stateMutexInit) {
        int rc = pthread_mutex_init(&m_impl->stateMutex, nullptr);
        if (rc != 0) {
            return PairingResult(PairingResultCategory::InvalidState,
                                 static_cast<uint32_t>(rc));
        }
        m_impl->stateMutexInit = true;
    }
    if (!m_impl->wakeMutexInit) {
        int rc = pthread_mutex_init(&m_impl->wakeMutex, nullptr);
        if (rc != 0) {
            return PairingResult(PairingResultCategory::InvalidState,
                                 static_cast<uint32_t>(rc));
        }
        m_impl->wakeMutexInit = true;
    }
    if (!m_impl->wakeCondInit) {
        int rc = pthread_cond_init(&m_impl->wakeCond, nullptr);
        if (rc != 0) {
            return PairingResult(PairingResultCategory::InvalidState,
                                 static_cast<uint32_t>(rc));
        }
        m_impl->wakeCondInit = true;
    }

    // Consume the already-created client identity.
    m_impl->identityReady = false;
    m_impl->identity = ClientIdentity();

    if (!identity.uniqueId.empty() &&
        !identity.deviceName.empty() &&
        !identity.certPem.empty() &&
        !identity.keyPem.empty() &&
        !identity.certDer.empty() &&
        !identity.keyDer.empty()) {
        m_impl->identity = identity;
        m_impl->identityReady = true;
        pmDiagPrint("[pairing] client identity consumed from PairingStore");
    } else {
        pmDiagPrint("[pairing] invalid client identity provided");
        return PairingResult(PairingResultCategory::IdentityUnavailable);
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->pairingActive = false;
    m_impl->cancelRequested = false;
    m_impl->updateQueue.clear();

    pmDiagPrint("[pairing] manager initialized");
    return PairingResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void PairingManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // Cancel any active pairing.
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
    pthread_mutex_lock(&m_impl->updateMutex);
    m_impl->updateQueue.clear();
    pthread_mutex_unlock(&m_impl->updateMutex);

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
    if (m_impl->updateMutexInit) {
        pthread_mutex_destroy(&m_impl->updateMutex);
        m_impl->updateMutexInit = false;
    }

    m_impl->httpClient = nullptr;
    m_impl->store = nullptr;
    m_impl->running = false;
    m_impl->identityReady = false;
    m_impl->identity = ClientIdentity();

    pmDiagPrint("[pairing] manager shut down");
}

// ---------------------------------------------------------------------------
// Client unique ID
// ---------------------------------------------------------------------------

const std::string& PairingManager::clientUniqueId() const {
    return m_impl->identity.uniqueId;
}

const std::string& PairingManager::clientCertPem() const {
    return m_impl->identity.certPem;
}

const std::string& PairingManager::clientKeyPem() const {
    return m_impl->identity.keyPem;
}

void PairingManager::refreshIdentity(const ClientIdentity& identity) {
    if (!m_impl) {
        return;
    }
    // Only accept a valid identity (all required fields non-empty).
    if (!identity.uniqueId.empty() &&
        !identity.deviceName.empty() &&
        !identity.certPem.empty() &&
        !identity.keyPem.empty() &&
        !identity.certDer.empty() &&
        !identity.keyDer.empty()) {
        m_impl->identity = identity;
        m_impl->identityReady = true;
        pmDiagPrint("[pairing] client identity refreshed (reset)");
    } else {
        pmDiagPrint("[pairing] refreshIdentity rejected: invalid identity");
    }
}

// ---------------------------------------------------------------------------
// Start pairing
// ---------------------------------------------------------------------------

PairingResult PairingManager::startPairing(const PairingTarget& target) {
    if (!m_impl->running.load()) {
        return PairingResult(PairingResultCategory::InvalidState);
    }
    if (m_impl->pairingActive.load()) {
        return PairingResult(PairingResultCategory::AlreadyPairing);
    }
    if (!m_impl->identityReady) {
        return PairingResult(PairingResultCategory::IdentityUnavailable);
    }
    if (target.hostId == 0) {
        return PairingResult(PairingResultCategory::InvalidHost);
    }
    if (target.serverId.empty() || target.ipv4Addresses.empty()) {
        return PairingResult(PairingResultCategory::InvalidTarget);
    }

    // Set up the active pairing state.
    m_impl->cancelRequested = false;
    m_impl->pairingActive = true;
    m_impl->activeTarget = target;

    // Clear any stale updates.
    pthread_mutex_lock(&m_impl->updateMutex);
    m_impl->updateQueue.clear();
    pthread_mutex_unlock(&m_impl->updateMutex);

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            pairingWorker, m_impl);
    if (rc != 0) {
        m_impl->pairingActive = false;
        return PairingResult(PairingResultCategory::InvalidState,
                             static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    pmDiagPrint("[pairing] pairing started");
    return PairingResult();
}

// ---------------------------------------------------------------------------
// Cancel pairing
// ---------------------------------------------------------------------------

void PairingManager::cancelPairing() {
    if (!m_impl->pairingActive.load()) {
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
// Update polling
// ---------------------------------------------------------------------------

bool PairingManager::pollUpdate(PairingUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->updateMutex);
    bool hasUpdate = !m_impl->updateQueue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->updateQueue.front());
        m_impl->updateQueue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->updateMutex);

    return hasUpdate;
}

// ---------------------------------------------------------------------------
// Pairing state
// ---------------------------------------------------------------------------

bool PairingManager::isPairing() const {
    return m_impl && m_impl->pairingActive.load();
}

// ---------------------------------------------------------------------------
// Local trust query (delegates to PairingStore)
// ---------------------------------------------------------------------------

bool PairingManager::hasLocalTrustForServer(const std::string& serverId) const {
    if (!m_impl || !m_impl->running.load() || serverId.empty() || m_impl->store == nullptr) {
        return false;
    }
    return m_impl->store->hasTrustedHost(serverId);
}

bool PairingManager::loadTrustedHosts(std::vector<TrustedHost>& out) const {
    if (!m_impl || !m_impl->running.load() || m_impl->store == nullptr) {
        return false;
    }
    return m_impl->store->loadTrustedHosts(out);
}

bool PairingManager::updateTrustedHostEndpoint(const std::string& serverId,
                                               const std::string& ipv4,
                                               uint16_t discoveryPort,
                                               uint16_t httpsPort) {
    if (!m_impl || !m_impl->running.load() || serverId.empty() || m_impl->store == nullptr) {
        return false;
    }
    return m_impl->store->updateTrustedHostEndpoint(serverId, ipv4,
                                                    discoveryPort, httpsPort);
}

bool PairingManager::removeLocalTrustForServer(const std::string& serverId) {
    if (!m_impl || !m_impl->running.load() || serverId.empty() || m_impl->store == nullptr) {
        return false;
    }
    return m_impl->store->removeTrustedHost(serverId);
}

} // namespace ml
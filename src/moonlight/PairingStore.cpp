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

// === PairingStore.cpp =======================================================
//
// OpenOrbis Moonlight client — Persistent Client Identity & Trust
//
// Implements the PairingStore concrete persistence owner:
//   - Reads/writes a simple bounded versioned binary format under /data/moonlight/.
//   - Uses standard C library file I/O (fopen, fread, fwrite, etc.).
//   - Validates input and handles corruption safely.
//   - Generates a new client identity (RSA-2048 key + self-signed X.509
//     certificate + unique ID) when none exists, using mbedTLS for crypto and
//     the PS4 RANDOM sysmodule for entropy.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "PairingStore.h"
#include "StoragePaths.h"

#include <cstdio>                // fopen, fclose, fread, fwrite, fseek, ftell, rewind
#include <cstring>               // memset, memcpy
#include <vector>
#include <string>

// --- mbedTLS headers (identity crypto only) -----------------------------------
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pem.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/libkernel.h>     // sceKernelMkdir
#include <orbis/Random.h>        // sceRandomGetRandomNumber (incorrect stub)

#include <ctime>                 // time, gmtime, strftime

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void psDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Writable application data directory.
constexpr const char* kStoreDir = ml::storage::kStorageDir;

/// Identity file path.
constexpr const char* kIdentityFilePath = ml::storage::kIdentityFilePath;

/// Trusted-hosts file path.
constexpr const char* kTrustedHostsFilePath = ml::storage::kTrustedHostsFilePath;

/// Persistence format magic.
constexpr uint32_t kFormatMagic = 0x4D4C5031;  // "MLP1"

/// Persistence format version.
///
/// Version 1: legacy trusted-host records (serverId, peerCertificateDer,
/// serviceFqdn, hostname). No endpoint data.
///
/// Version 2: adds a last-known endpoint (lastKnownIpv4, lastKnownDiscoveryPort,
/// lastKnownHttpsPort) so a paired host can be probed via HTTPS after restart
/// even when mDNS is unavailable. Version 1 records still load (endpoint
/// fields default to empty/0).
constexpr uint32_t kFormatVersion = 2;

/// Maximum trusted-host count.
constexpr uint16_t kMaxTrustedHosts = 32;

/// Maximum field sizes.
constexpr uint16_t kMaxUniqueIdLen = 64;
constexpr uint16_t kMaxDeviceNameLen = 64;
constexpr uint32_t kMaxCertPemLen = 8192;
constexpr uint32_t kMaxKeyPemLen = 4096;
constexpr uint32_t kMaxCertDerLen = 4096;
constexpr uint32_t kMaxKeyDerLen = 8192;
constexpr uint16_t kMaxServerIdLen = 256;
constexpr uint32_t kMaxPeerCertDerLen = 16384;
constexpr uint16_t kMaxFqdnLen = 256;
constexpr uint16_t kMaxHostnameLen = 256;
constexpr uint16_t kMaxIpv4Len = 256;   ///< Endpoint address / hostname string bound.

/// Maximum identity file size (bounded).
constexpr size_t kMaxIdentityFileSize = 32 * 1024;

/// Maximum trusted-hosts file size (bounded).
constexpr size_t kMaxTrustedHostsFileSize = 1 * 1024 * 1024;

/// RSA key size for client identity.
constexpr size_t kIdentityKeyBits = 2048;

/// RSA public exponent.
constexpr int kIdentityKeyExponent = 65537;

/// Client certificate common name.
constexpr const char* kIdentityCertCommonName = "NVIDIA GameStream Client";

/// DRBG personalization string.
constexpr const char* kDrbgPers = "moonlight-ps4-pairing";

/// Maximum client certificate PEM size.
constexpr size_t kMaxClientCertPemSize = 8192;

/// Maximum client key PEM size.
constexpr size_t kMaxClientKeyPemSize = 4096;

/// Maximum server certificate size (used for cert DER buffer).
constexpr size_t kMaxServerCertSize = 16 * 1024;

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

/// Private implementation of PairingStore.
struct PairingStore::Impl {
    bool initialized = false;
};

// ---------------------------------------------------------------------------
// File I/O helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

/// Read an entire file into a byte vector. Returns true on success.
bool readWholeFile(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    if (path == nullptr) {
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }

    // Get file size.
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    rewind(f);

    out.resize(static_cast<size_t>(size));
    size_t read = fread(out.data(), 1, static_cast<size_t>(size), f);
    fclose(f);

    if (read != static_cast<size_t>(size)) {
        out.clear();
        return false;
    }
    return true;
}

/// Write a byte vector to a file. Returns true on success.
bool writeWholeFile(const char* path, const uint8_t* data, size_t size) {
    if (path == nullptr || data == nullptr || size == 0) {
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    return written == size;
}

// --- Binary serialization helpers -------------------------------------------

/// Append a uint16 (little-endian) to a byte vector.
void appendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

/// Append a uint32 (little-endian) to a byte vector.
void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

/// Append a length-prefixed string (uint16 length) to a byte vector.
bool appendString16(std::vector<uint8_t>& out, const std::string& s, uint16_t maxLen) {
    if (s.size() > maxLen) {
        return false;
    }
    appendU16(out, static_cast<uint16_t>(s.size()));
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    return true;
}

/// Append a length-prefixed byte vector (uint32 length) to a byte vector.
bool appendBytes32(std::vector<uint8_t>& out, const std::vector<uint8_t>& data, uint32_t maxLen) {
    if (data.size() > maxLen) {
        return false;
    }
    appendU32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return true;
}

/// Append a length-prefixed string (uint32 length) to a byte vector.
bool appendString32(std::vector<uint8_t>& out, const std::string& s, uint32_t maxLen) {
    if (s.size() > maxLen) {
        return false;
    }
    appendU32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    return true;
}

/// Read a uint16 (little-endian) from a byte buffer. Returns false on overflow.
bool readU16(const uint8_t* data, size_t size, size_t& pos, uint16_t& out) {
    if (pos + 2 > size) {
        return false;
    }
    out = static_cast<uint16_t>(data[pos]) |
          static_cast<uint16_t>(data[pos + 1]) << 8;
    pos += 2;
    return true;
}

/// Read a uint32 (little-endian) from a byte buffer. Returns false on overflow.
bool readU32(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
    if (pos + 4 > size) {
        return false;
    }
    out = static_cast<uint32_t>(data[pos]) |
          static_cast<uint32_t>(data[pos + 1]) << 8 |
          static_cast<uint32_t>(data[pos + 2]) << 16 |
          static_cast<uint32_t>(data[pos + 3]) << 24;
    pos += 4;
    return true;
}

/// Read a length-prefixed string (uint16 length) from a byte buffer.
bool readString16(const uint8_t* data, size_t size, size_t& pos,
                  uint16_t maxLen, std::string& out) {
    uint16_t len = 0;
    if (!readU16(data, size, pos, len)) {
        return false;
    }
    if (len > maxLen || pos + len > size) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return true;
}

/// Read a length-prefixed byte vector (uint32 length) from a byte buffer.
bool readBytes32(const uint8_t* data, size_t size, size_t& pos,
                 uint32_t maxLen, std::vector<uint8_t>& out) {
    uint32_t len = 0;
    if (!readU32(data, size, pos, len)) {
        return false;
    }
    if (len > maxLen || pos + len > size) {
        return false;
    }
    out.assign(data + pos, data + pos + len);
    pos += len;
    return true;
}

/// Read a length-prefixed string (uint32 length) from a byte buffer.
bool readString32(const uint8_t* data, size_t size, size_t& pos,
                  uint32_t maxLen, std::string& out) {
    uint32_t len = 0;
    if (!readU32(data, size, pos, len)) {
        return false;
    }
    if (len > maxLen || pos + len > size) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Identity generation helpers
// ---------------------------------------------------------------------------

namespace {

/// Generate a new client identity (RSA-2048 key + self-signed X.509 cert).
bool generateClientIdentity(ClientIdentity& out) {
    int ret = 0;

    unsigned char keyPemBuf[kMaxClientKeyPemSize];
    unsigned char certDerBuf[kMaxServerCertSize];
    unsigned char certPemBuf[kMaxClientCertPemSize];
    unsigned char keyDerBuf[kMaxClientKeyPemSize];

    memset(keyPemBuf, 0, sizeof(keyPemBuf));
    memset(certDerBuf, 0, sizeof(certDerBuf));
    memset(certPemBuf, 0, sizeof(certPemBuf));
    memset(keyDerBuf, 0, sizeof(keyDerBuf));

    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    out.certDer.clear();
    out.keyDer.clear();

    do {
        // Seed the DRBG with the PS4 random source.
        ret = mbedtls_entropy_add_source(&entropy, pairingEntropyFunc, nullptr,
                                         32, MBEDTLS_ENTROPY_SOURCE_STRONG);
        if (ret != 0) break;

        ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>(kDrbgPers),
                                    strlen(kDrbgPers));
        if (ret != 0) break;

        // Generate RSA-2048 key.
        ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
        if (ret != 0) break;

        ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random,
                                  &ctr_drbg, static_cast<unsigned int>(kIdentityKeyBits),
                                  kIdentityKeyExponent);
        if (ret != 0) break;

        // Serial: use a random positive 64-bit value.
        // NOTE: mbedTLS 3.x encodes a serial of 0 as an empty INTEGER (02 00),
        // which produces a malformed certificate that Sunshine/OpenSSL reject.
        // mbedTLS 2.x encoded serial 0 as 02 01 00 (valid). Use a random
        // positive serial instead, which is also RFC 5280 best practice.
        {
            uint8_t serialBytes[8];
            if (!generateRandomBytes(serialBytes, sizeof(serialBytes))) {
                ret = -1;
                break;
            }
            // Force the high bit of the top byte to 0 so the serial is a
            // positive INTEGER (avoids a leading sign byte in DER).
            serialBytes[0] &= 0x7F;
            ret = mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes));
            if (ret != 0) break;
            // Guard against an all-zero serial (astronomically unlikely but
            // cheap to handle): fall back to 1.
            if (mbedtls_mpi_cmp_int(&serial, 0) == 0) {
                ret = mbedtls_mpi_lset(&serial, 1);
                if (ret != 0) break;
            }
        }

        // Configure the certificate.
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);

        std::string subjectName = "CN=";
        subjectName += kIdentityCertCommonName;

        ret = mbedtls_x509write_crt_set_subject_name(&crt, subjectName.c_str());
        if (ret != 0) break;
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, subjectName.c_str());
        if (ret != 0) break;
        ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
        if (ret != 0) break;

        // Validity: now .. now + 10 years.
        char notBefore[16];
        char notAfter[16];
        time_t now = time(nullptr);
        if (now == static_cast<time_t>(-1)) break;
        struct tm tmNow;
        struct tm tmAfter;
        struct tm* tmp = gmtime(&now);
        if (!tmp) break;
        tmNow = *tmp;
        tmAfter = tmNow;
        tmAfter.tm_year += 10;
        if (strftime(notBefore, sizeof(notBefore), "%Y%m%d%H%M%S", &tmNow) == 0) break;
        if (strftime(notAfter, sizeof(notAfter), "%Y%m%d%H%M%S", &tmAfter) == 0) break;

        ret = mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter);
        if (ret != 0) break;

        // Write key DER.
        ret = mbedtls_pk_write_key_der(&key, keyDerBuf, sizeof(keyDerBuf));
        if (ret < 0) break;
        {
            const int keyLen = ret;
            unsigned char* keyDerStart = keyDerBuf + sizeof(keyDerBuf) - static_cast<size_t>(keyLen);
            out.keyDer.assign(keyDerStart, keyDerStart + keyLen);
        }

        // Write key PEM.
        ret = mbedtls_pk_write_key_pem(&key, keyPemBuf, sizeof(keyPemBuf));
        if (ret != 0) break;

        // Write cert DER.
        ret = mbedtls_x509write_crt_der(&crt, certDerBuf, sizeof(certDerBuf),
                                        mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret < 0) break;
        {
            const int certLenInt = ret;
            unsigned char* certDerStart = certDerBuf + sizeof(certDerBuf) - static_cast<size_t>(certLenInt);
            out.certDer.assign(certDerStart, certDerStart + certLenInt);
        }

        // Write cert PEM.
        size_t certPemLen = 0;
        ret = mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                                       "-----END CERTIFICATE-----\n",
                                       out.certDer.data(), out.certDer.size(),
                                       certPemBuf, sizeof(certPemBuf), &certPemLen);
        if (ret != 0) break;

        out.keyPem = reinterpret_cast<const char*>(keyPemBuf);
        if (certPemLen > 0) {
            out.certPem = std::string(reinterpret_cast<const char*>(certPemBuf), certPemLen - 1);
        } else {
            out.certPem = reinterpret_cast<const char*>(certPemBuf);
        }

        // --- Self-check: verify the generated cert's public key matches the
        // generated private key. If this fails, the server will reject the
        // Stage 4 clientpairingsecret signature. ---
        {
            mbedtls_x509_crt checkCrt;
            mbedtls_x509_crt_init(&checkCrt);
            int checkRet = mbedtls_x509_crt_parse(
                &checkCrt,
                reinterpret_cast<const unsigned char*>(out.certPem.c_str()),
                out.certPem.size() + 1);
            if (checkRet == 0) {
                // Sign a fixed test message with the private key.
                const unsigned char testMsg[] = "moonlight-ps4-keypair-selfcheck";
                unsigned char testHash[32];
                mbedtls_sha256(testMsg, sizeof(testMsg) - 1, testHash, 0);
                std::vector<unsigned char> testSig(mbedtls_pk_get_len(&key));
                size_t testSigLen = 0;
                int signRet = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256,
                                              testHash, sizeof(testHash),
                                              testSig.data(), testSig.size(),
                                              &testSigLen,
                                              mbedtls_ctr_drbg_random, &ctr_drbg);
                if (signRet == 0) {
                    int verifyRet = mbedtls_pk_verify(&checkCrt.pk, MBEDTLS_MD_SHA256,
                                                      testHash, sizeof(testHash),
                                                      testSig.data(), testSigLen);
                    char dbg[192];
                    snprintf(dbg, sizeof(dbg),
                             "[pairingstore] keypair self-check: sign=%d verify=%d "
                             "(0=OK, nonzero=mismatch)",
                             signRet, verifyRet);
                    psDiagPrint(dbg);
                } else {
                    psDiagPrint("[pairingstore] keypair self-check: SIGN FAILED");
                }
            } else {
                char dbg[128];
                snprintf(dbg, sizeof(dbg),
                         "[pairingstore] keypair self-check: cert parse FAILED ret=-0x%04X",
                         static_cast<unsigned>(-checkRet));
                psDiagPrint(dbg);
            }
            mbedtls_x509_crt_free(&checkCrt);
        }

        // Generate unique ID (16 hex chars from 8 random bytes).
        uint8_t idBytes[8];
        if (!generateRandomBytes(idBytes, sizeof(idBytes))) {
            ret = -1;
            break;
        }
        static const char kHex[] = "0123456789ABCDEF";
        char idHex[17];
        for (size_t i = 0; i < sizeof(idBytes); ++i) {
            idHex[i * 2] = kHex[(idBytes[i] >> 4) & 0x0F];
            idHex[i * 2 + 1] = kHex[idBytes[i] & 0x0F];
        }
        idHex[16] = '\0';
        out.uniqueId = idHex;
        out.deviceName = "roth";

        {
            // Diagnostic: log the generated identity sizes so a freshly
            // generated identity can be compared against a persisted one.
            char dbg[256];
            snprintf(dbg, sizeof(dbg),
                     "[pairingstore] generated identity: uniqueId='%s' "
                     "certPem=%zu keyPem=%zu certDer=%zu keyDer=%zu",
                     out.uniqueId.c_str(),
                     out.certPem.size(), out.keyPem.size(),
                     out.certDer.size(), out.keyDer.size());
            psDiagPrint(dbg);
        }

        mbedtls_pk_free(&key);
        mbedtls_x509write_crt_free(&crt);
        mbedtls_mpi_free(&serial);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return true;

    } while (false);

    out.certDer.clear();
    out.keyDer.clear();
    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_mpi_free(&serial);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

PairingStore::PairingStore()
    : m_impl(new Impl())
{
}

PairingStore::~PairingStore() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool PairingStore::initialize() {
    if (m_impl->initialized) {
        return true;
    }

    // Ensure the writable data directory exists. If it does not, create it.
    // sceKernelMkdir returns 0 on success or ORBIS_KERNEL_ERROR_EEXIST if the
    // directory already exists (both are acceptable).
    int rc = sceKernelMkdir(kStoreDir, 0777);
    if (rc != 0 && rc != 0x80020011 /* ORBIS_KERNEL_ERROR_EEXIST */) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[pairingstore] mkdir %s failed: 0x%08X",
                 kStoreDir, static_cast<uint32_t>(rc));
        psDiagPrint(buf);
        // Directory creation failure is not fatal: file operations will fail
        // gracefully and identity will be regenerated on the next launch.
    }

    // Check if Moonlight MP already has an identity or trusted hosts.
    // If not, perform a one-time import from legacy Moonlight DX (/data/moonlight)
    // so existing pairings and settings carry over seamlessly without re-pairing.
    FILE* checkId = fopen(kIdentityFilePath, "rb");
    if (!checkId) {
        FILE* legacyId = fopen(ml::storage::kLegacyIdentityFilePath, "rb");
        if (legacyId) {
            fclose(legacyId);
            psDiagPrint("[pairingstore] legacy Moonlight DX storage detected; importing to Moonlight MP...");

            auto copyFileHelper = [](const char* src, const char* dst) -> bool {
                FILE* s = fopen(src, "rb");
                if (!s) return false;
                FILE* d = fopen(dst, "wb");
                if (!d) { fclose(s); return false; }
                char buf[4096];
                size_t n = 0;
                bool ok = true;
                while ((n = fread(buf, 1, sizeof(buf), s)) > 0) {
                    if (fwrite(buf, 1, n, d) != n) { ok = false; break; }
                }
                fclose(s);
                fclose(d);
                return ok;
            };

            bool idOk = copyFileHelper(ml::storage::kLegacyIdentityFilePath, kIdentityFilePath);
            bool hostsOk = copyFileHelper(ml::storage::kLegacyTrustedHostsFilePath, kTrustedHostsFilePath);
            bool settingsOk = copyFileHelper(ml::storage::kLegacySettingsFilePath, ml::storage::kSettingsFilePath);

            char msg[256];
            snprintf(msg, sizeof(msg),
                     "[pairingstore] legacy DX import complete: identity=%s, hosts=%s, settings=%s",
                     idOk ? "OK" : "SKIP/ERR",
                     hostsOk ? "OK" : "SKIP/ERR",
                     settingsOk ? "OK" : "SKIP/ERR");
            psDiagPrint(msg);
        }
    } else {
        fclose(checkId);
    }

    m_impl->initialized = true;
    return true;
}

void PairingStore::shutdown() {
    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

bool PairingStore::loadIdentity(ClientIdentity& out) {
    if (!m_impl->initialized) {
        return false;
    }

    std::vector<uint8_t> data;
    if (!readWholeFile(kIdentityFilePath, data)) {
        return false;
    }
    if (data.size() > kMaxIdentityFileSize) {
        psDiagPrint("[pairingstore] identity file too large");
        return false;
    }

    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    if (!readU32(data.data(), data.size(), pos, magic) ||
        !readU32(data.data(), data.size(), pos, version)) {
        return false;
    }
    if (magic != kFormatMagic || version != kFormatVersion) {
        psDiagPrint("[pairingstore] identity file has unsupported format/version");
        return false;
    }

    ClientIdentity id;
    if (!readString16(data.data(), data.size(), pos, kMaxUniqueIdLen, id.uniqueId) ||
        !readString16(data.data(), data.size(), pos, kMaxDeviceNameLen, id.deviceName) ||
        !readString32(data.data(), data.size(), pos, kMaxCertPemLen, id.certPem) ||
        !readString32(data.data(), data.size(), pos, kMaxKeyPemLen, id.keyPem) ||
        !readBytes32(data.data(), data.size(), pos, kMaxCertDerLen, id.certDer) ||
        !readBytes32(data.data(), data.size(), pos, kMaxKeyDerLen, id.keyDer)) {
        return false;
    }

    // Validate required fields.
    if (id.uniqueId.empty() || id.deviceName.empty() ||
        id.certPem.empty() || id.keyPem.empty() ||
        id.certDer.empty() || id.keyDer.empty()) {
        return false;
    }

    out = std::move(id);
    return true;
}

bool PairingStore::saveIdentity(const ClientIdentity& identity) {
    if (!m_impl->initialized) {
        return false;
    }

    // Validate required fields.
    if (identity.uniqueId.empty() || identity.deviceName.empty() ||
        identity.certPem.empty() || identity.keyPem.empty() ||
        identity.certDer.empty() || identity.keyDer.empty()) {
        return false;
    }

    std::vector<uint8_t> out;
    appendU32(out, kFormatMagic);
    appendU32(out, kFormatVersion);
    if (!appendString16(out, identity.uniqueId, kMaxUniqueIdLen) ||
        !appendString16(out, identity.deviceName, kMaxDeviceNameLen) ||
        !appendString32(out, identity.certPem, kMaxCertPemLen) ||
        !appendString32(out, identity.keyPem, kMaxKeyPemLen) ||
        !appendBytes32(out, identity.certDer, kMaxCertDerLen) ||
        !appendBytes32(out, identity.keyDer, kMaxKeyDerLen)) {
        return false;
    }

    return writeWholeFile(kIdentityFilePath, out.data(), out.size());
}

bool PairingStore::hasIdentity() const {
    if (!m_impl->initialized) {
        return false;
    }
    // Check if the identity file can be opened.
    FILE* f = fopen(kIdentityFilePath, "rb");
    if (f == nullptr) {
        return false;
    }
    fclose(f);
    return true;
}

bool PairingStore::ensureIdentity(ClientIdentity& out) {
    if (!m_impl->initialized) {
        return false;
    }

    // Try to load an existing valid identity.
    if (loadIdentity(out)) {
        // Validate the loaded identity.
        if (!out.uniqueId.empty() && !out.deviceName.empty() &&
            !out.certPem.empty() && !out.keyPem.empty() &&
            !out.certDer.empty() && !out.keyDer.empty()) {
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg),
                         "[pairingstore] client identity loaded: uniqueId='%s' "
                         "certPem=%zu keyPem=%zu certDer=%zu keyDer=%zu",
                         out.uniqueId.c_str(),
                         out.certPem.size(), out.keyPem.size(),
                         out.certDer.size(), out.keyDer.size());
                psDiagPrint(dbg);
            }
            return true;
        }
        psDiagPrint("[pairingstore] loaded identity is invalid; regenerating");
    }

    // Generate a new identity.
    ClientIdentity newIdentity;
    if (!generateClientIdentity(newIdentity)) {
        psDiagPrint("[pairingstore] identity generation failed");
        return false;
    }
    if (!saveIdentity(newIdentity)) {
        psDiagPrint("[pairingstore] identity persistence failed");
        return false;
    }
    out = std::move(newIdentity);
    psDiagPrint("[pairingstore] client identity generated");
    return true;
}

bool PairingStore::resetIdentity(ClientIdentity& out) {
    if (!m_impl->initialized) {
        return false;
    }

    // Delete the persisted identity file so the next ensureIdentity() call
    // generates a brand new identity. A missing file is not an error.
    remove(kIdentityFilePath);
    psDiagPrint("[pairingstore] client identity file removed (reset)");

    // Regenerate and persist a fresh identity.
    return ensureIdentity(out);
}

// ---------------------------------------------------------------------------
// Trusted hosts
// ---------------------------------------------------------------------------

bool PairingStore::loadTrustedHosts(std::vector<TrustedHost>& out) const {
    out.clear();
    if (!m_impl->initialized) {
        return false;
    }

    std::vector<uint8_t> data;
    if (!readWholeFile(kTrustedHostsFilePath, data)) {
        // No file yet — treat as empty.
        return true;
    }
    if (data.size() > kMaxTrustedHostsFileSize) {
        psDiagPrint("[pairingstore] trusted-hosts file too large");
        return false;
    }

    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    if (!readU32(data.data(), data.size(), pos, magic) ||
        !readU32(data.data(), data.size(), pos, version)) {
        return false;
    }
    // Accept both the legacy version 1 format and the current version 2
    // format. Version 1 records have no endpoint fields; they load with an
    // empty last-known endpoint (host remains Offline until discovered once).
    if (magic != kFormatMagic || (version != 1 && version != kFormatVersion)) {
        psDiagPrint("[pairingstore] trusted-hosts file has unsupported format/version");
        return false;
    }

    uint16_t count = 0;
    if (!readU16(data.data(), data.size(), pos, count)) {
        return false;
    }
    if (count > kMaxTrustedHosts) {
        psDiagPrint("[pairingstore] trusted-hosts count exceeds maximum");
        return false;
    }

    for (uint16_t i = 0; i < count; ++i) {
        TrustedHost host;
        if (!readString16(data.data(), data.size(), pos, kMaxServerIdLen, host.serverId) ||
            !readBytes32(data.data(), data.size(), pos, kMaxPeerCertDerLen, host.peerCertificateDer) ||
            !readString16(data.data(), data.size(), pos, kMaxFqdnLen, host.serviceFqdn) ||
            !readString16(data.data(), data.size(), pos, kMaxHostnameLen, host.hostname)) {
            // Corrupt one-host record — skip it and continue.
            psDiagPrint("[pairingstore] corrupt trusted-host record skipped");
            continue;
        }
        // Version 2 records additionally carry a last-known endpoint.
        if (version >= 2) {
            if (!readString16(data.data(), data.size(), pos, kMaxIpv4Len, host.lastKnownIpv4) ||
                !readU16(data.data(), data.size(), pos, host.lastKnownDiscoveryPort) ||
                !readU16(data.data(), data.size(), pos, host.lastKnownHttpsPort)) {
                // Corrupt endpoint section — skip this record.
                psDiagPrint("[pairingstore] corrupt trusted-host endpoint skipped");
                continue;
            }
        }
        if (host.serverId.empty() || host.peerCertificateDer.empty()) {
            // Invalid record — skip.
            continue;
        }
        out.push_back(std::move(host));
    }

    return true;
}

bool PairingStore::saveTrustedHosts(const std::vector<TrustedHost>& hosts) {
    if (!m_impl->initialized) {
        return false;
    }
    if (hosts.size() > kMaxTrustedHosts) {
        return false;
    }

    std::vector<uint8_t> out;
    appendU32(out, kFormatMagic);
    appendU32(out, kFormatVersion);
    appendU16(out, static_cast<uint16_t>(hosts.size()));

    for (const auto& host : hosts) {
        if (!appendString16(out, host.serverId, kMaxServerIdLen) ||
            !appendBytes32(out, host.peerCertificateDer, kMaxPeerCertDerLen) ||
            !appendString16(out, host.serviceFqdn, kMaxFqdnLen) ||
            !appendString16(out, host.hostname, kMaxHostnameLen) ||
            // Version 2 endpoint fields.
            !appendString16(out, host.lastKnownIpv4, kMaxIpv4Len)) {
            return false;
        }
        appendU16(out, host.lastKnownDiscoveryPort);
        appendU16(out, host.lastKnownHttpsPort);
    }

    return writeWholeFile(kTrustedHostsFilePath, out.data(), out.size());
}

bool PairingStore::clearTrustedHosts() {
    if (!m_impl->initialized) {
        return false;
    }

    // Delete the persisted trusted-host file. A missing file is not an error;
    // loadTrustedHosts() already treats a missing file as an empty list.
    remove(kTrustedHostsFilePath);
    psDiagPrint("[pairingstore] trusted-hosts file removed (reset)");
    return true;
}

bool PairingStore::findTrustedHost(const std::string& serverId, TrustedHost& out) const {
    if (!m_impl->initialized || serverId.empty()) {
        return false;
    }

    std::vector<TrustedHost> hosts;
    if (!loadTrustedHosts(hosts)) {
        return false;
    }

    for (const auto& host : hosts) {
        if (host.serverId == serverId) {
            out = host;
            return true;
        }
    }
    return false;
}

bool PairingStore::hasTrustedHost(const std::string& serverId) const {
    TrustedHost tmp;
    return findTrustedHost(serverId, tmp);
}

bool PairingStore::addTrustedHost(const TrustedHost& host) {
    if (!m_impl->initialized || host.serverId.empty() || host.peerCertificateDer.empty()) {
        return false;
    }

    std::vector<TrustedHost> hosts;
    if (!loadTrustedHosts(hosts)) {
        return false;
    }

    // Replace if exists, else append.
    bool replaced = false;
    for (auto& h : hosts) {
        if (h.serverId == host.serverId) {
            h = host;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (hosts.size() >= kMaxTrustedHosts) {
            psDiagPrint("[pairingstore] trusted-host capacity reached");
            return false;
        }
        hosts.push_back(host);
    }

    return saveTrustedHosts(hosts);
}

bool PairingStore::updateTrustedHostEndpoint(const std::string& serverId,
                                             const std::string& ipv4,
                                             uint16_t discoveryPort,
                                             uint16_t httpsPort) {
    if (!m_impl->initialized || serverId.empty()) {
        return false;
    }

    std::vector<TrustedHost> hosts;
    if (!loadTrustedHosts(hosts)) {
        return false;
    }

    // Find the matching record. If absent, there is nothing to update; treat
    // as success (the desired end state is already reached).
    bool found = false;
    for (auto& h : hosts) {
        if (h.serverId == serverId) {
            found = true;
            // Only persist when a relevant stored value actually changes.
            if (h.lastKnownIpv4 == ipv4 &&
                h.lastKnownDiscoveryPort == discoveryPort &&
                h.lastKnownHttpsPort == httpsPort) {
                return true;   // No change — avoid a redundant write.
            }
            h.lastKnownIpv4 = ipv4;
            h.lastKnownDiscoveryPort = discoveryPort;
            h.lastKnownHttpsPort = httpsPort;
            break;
        }
    }
    if (!found) {
        return true;
    }

    return saveTrustedHosts(hosts);
}

bool PairingStore::removeTrustedHost(const std::string& serverId) {
    if (!m_impl->initialized || serverId.empty()) {
        return false;
    }

    std::vector<TrustedHost> hosts;
    if (!loadTrustedHosts(hosts)) {
        return false;
    }

    // Remove every record matching the serverId.
    bool removed = false;
    for (auto it = hosts.begin(); it != hosts.end();) {
        if (it->serverId == serverId) {
            it = hosts.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }

    // If no matching record existed, there is nothing to persist; treat as
    // success (the desired end state is already reached).
    if (!removed) {
        return true;
    }

    return saveTrustedHosts(hosts);
}

} // namespace ml
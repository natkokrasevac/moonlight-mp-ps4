#ifndef MBEDTLS_PS4_CONFIG_H
#define MBEDTLS_PS4_CONFIG_H

/*
 * mbedTLS 3.6.7 configuration for the OpenOrbis PS4 Moonlight client.
 *
 * This file is referenced via MBEDTLS_CONFIG_FILE (see CMakeLists.txt) and is
 * included by mbedtls/build_info.h. It includes the upstream default config
 * (mbedtls/mbedtls_config.h) and then overrides it for the PS4 target.
 *
 * Notes for mbedTLS 3.x:
 *   - PSA crypto (MBEDTLS_PSA_CRYPTO_C) is always enabled and cannot be
 *     disabled. moonlight-common-c's PlatformCrypto.c requires the PSA
 *     multipart AEAD API, which is only implemented in mbedTLS 3.x.
 *   - Several 2.x symbols were removed in 3.x (MBEDTLS_PLATFORM_ENTROPY,
 *     MBEDTLS_HAVEGE_C, MBEDTLS_BLOWFISH_C, MBEDTLS_XTEA_C,
 *     MBEDTLS_SSL_DTLS_BADMAC_LIMIT) and are no longer referenced here.
 */

#include "mbedtls/mbedtls_config.h"

/* ---------------------------------------------------------------------------
 * Entropy
 * ---------------------------------------------------------------------------
 * OpenOrbis has no usable platform entropy (getrandom/getentropy stubbed,
 * libSceRandom not auto-loaded). We provide mbedtls_hardware_poll() backed by
 * the PS4 RANDOM sysmodule (see src/platform/ps4/ps4_entropy.c).
 */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ---------------------------------------------------------------------------
 * Disable features not needed on PS4
 * ---------------------------------------------------------------------------
 */

/* No filesystem on PS4. This also forces MBEDTLS_PSA_ITS_FILE_C off (it
 * requires MBEDTLS_FS_IO). */
#undef MBEDTLS_FS_IO

/* No TLS/SSL or network stack inside mbedTLS (the app uses libSceSsl/Http). */
#undef MBEDTLS_SSL_TLS_C
#undef MBEDTLS_SSL_CLI_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_NET_C

/* Timing module not needed. */
#undef MBEDTLS_TIMING_C

/* Disable cipher suites / algorithms not used by the client. */
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CHACHA20_C
#undef MBEDTLS_POLY1305_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CHACHAPOLY_C

/* PKCS5/PKCS12 not used. */
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS12_C

/* ---------------------------------------------------------------------------
 * Persistent PSA key storage
 * ---------------------------------------------------------------------------
 * moonlight-common-c uses volatile keys only, so persistent key storage is not
 * needed. MBEDTLS_PSA_ITS_FILE_C requires MBEDTLS_FS_IO (disabled above), so it
 * must stay off. MBEDTLS_PSA_CRYPTO_STORAGE_C is also disabled to avoid the
 * filesystem-backed ITS dependency.
 */
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* ---------------------------------------------------------------------------
 * Crypto modules required by PairingStore and moonlight-common-c
 * ---------------------------------------------------------------------------
 * Most of these are enabled by default in mbedTLS 3.x; they are listed here
 * explicitly for clarity and to guard against future default changes.
 */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_PADDING_PKCS7

#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_RIPEMD160_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_BASE64_C

#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C

#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRT_WRITE_C

/* PSA crypto is always enabled in mbedTLS 3.x and is required by
 * moonlight-common-c's PlatformCrypto.c. */
#define MBEDTLS_PSA_CRYPTO_C

#endif

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

// === ps4_entropy.c ==========================================================
//
// OpenOrbis Moonlight client — PS4 hardware entropy source for mbedTLS
//
// Provides mbedtls_hardware_poll(), the hardware entropy source that mbedTLS
// uses when MBEDTLS_ENTROPY_HARDWARE_ALT is enabled. This is required because
// OpenOrbis's musl getrandom()/getentropy() are stubbed (ENOSYS) and
// libSceRandom is not auto-loaded in homebrew.
//
// We use the PS4 RANDOM sysmodule (sceRandomGetRandomNumber), which is the
// same hardware-backed source already used for pairing crypto in
// PairingStore.cpp. The RANDOM sysmodule is loaded once by SysmoduleManager at
// process startup; we do not reload it here.
//
// This is a C translation unit. It is compiled into the client executable and
// linked against mbedtls_ps4.
// ===============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// mbedTLS 3.x does not provide a public header declaring mbedtls_hardware_poll();
// the application provides the implementation and the prototype is implied by
// MBEDTLS_ENTROPY_HARDWARE_ALT. Declare it here to match the mbedTLS contract.
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen);

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/Random.h>   // sceRandomGetRandomNumber (incorrect stub)

// ---------------------------------------------------------------------------
// RNG access (OpenOrbis incorrect-stub workaround)
// ---------------------------------------------------------------------------
//
// The OpenOrbis SDK declares sceRandomGetRandomNumber() with no arguments
// (an incorrect stub). The real PS4 signature is:
//     int sceRandomGetRandomNumber(void* buf, size_t size);
//
// We take the address of the stub and cast it to the real function-pointer
// type. This is the same safe function-pointer-cast technique used in the
// pairing work (PairingStore.cpp / PairingManager.cpp).

typedef int (*Ps4RandomFn)(void*, size_t);
static Ps4RandomFn s_randomFn = (Ps4RandomFn)(void*)&sceRandomGetRandomNumber;

/// Generate random bytes using the PS4 RANDOM sysmodule.
static int ps4GenerateRandomBytes(unsigned char* out, size_t size) {
    if (out == NULL || size == 0 || s_randomFn == NULL) {
        return -1;
    }
    return s_randomFn(out, size) >= 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// mbedTLS hardware entropy source
// ---------------------------------------------------------------------------

/// Hardware entropy source for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT).
///
/// Fills `output` with `len` bytes of hardware randomness from the PS4 RANDOM
/// sysmodule. Must accept NULL as its first argument (per mbedTLS contract).
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    if (output == NULL || olen == NULL) {
        return -1;
    }

    size_t offset = 0;
    while (offset < len) {
        uint8_t temp[16];
        size_t chunk = len - offset;
        if (chunk > sizeof(temp)) {
            chunk = sizeof(temp);
        }
        if (ps4GenerateRandomBytes(temp, chunk) != 0) {
            *olen = offset;
            return -1;
        }
        memcpy(output + offset, temp, chunk);
        offset += chunk;
    }

    *olen = len;
    return 0;
}

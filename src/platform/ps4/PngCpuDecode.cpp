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

// === PngCpuDecode.cpp ========================================================
//
// OpenOrbis PS4 — Minimal CPU PNG decode helper.
//
// Implements decodePngToCpu() using the stb_image backend (already available
// through the OpenOrbis toolchain include path as <stb/stb_image.h>). This is
// the smallest reusable CPU decode path used by the CPU compositor and
// renderers. It decodes a PNG to a tightly-packed CPU RGBA8 buffer and performs
// no GPU upload.
//
// Intentional difference from the SCE implementation:
//   - The SCE reference uses the PS4 PngDec sysmodule (scePngDec*). This
//     OpenOrbis implementation uses stb_image, which is the proven path used by
//     the OpenOrbis pngdec sample. The public API (decodePngToCpu /
//     CpuPngImage) is preserved unchanged.
//   - stb_image requires no global initialization or shutdown, so there is no
//     SCE sysmodule lifecycle to emulate. The class is a plain free function.
//
// Pixel format: stb_image is forced to 4 channels (STBI_rgb_alpha), producing
// tightly packed RGBA8 in R,G,B,A byte order. This matches the compositor's
// canonical surface and the hardware-verified framebuffer byte order, so no
// channel swapping is required.
//
// OpenOrbis SDK includes are confined to this translation unit.
// =============================================================================

#include "PngCpuDecode.h"

// --- stb_image backend (private to this translation unit) --------------------
// stb_image is a single-header library. The implementation is compiled here.
#define STBI_ASSERT(x)
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <cstdio>    // printf, fflush
#include <cstdlib>   // malloc, free
#include <cstring>   // memcpy

namespace ps4 {

// ---------------------------------------------------------------------------
// Decode-size protection
// ---------------------------------------------------------------------------

/// Maximum decoded image dimension (pixels) in either axis. Preserves the SCE
/// reference's bound: large decoded images can exhaust the process heap, so we
/// reject anything beyond this before allocating.
static constexpr uint32_t kMaxDecodeDimension = 4096;

/// Maximum decoded image byte size (width * height * 4). Guards against
/// overflow and excessive heap use.
static constexpr uint64_t kMaxDecodeBytes = 4096ull * 4096ull * 4ull;

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void pngDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// decodePngToCpu
// ---------------------------------------------------------------------------

bool decodePngToCpu(const char* filepath, CpuPngImage& outImage) {
    // Reset the output image to a clean, empty state.
    outImage.release();

    if (!filepath) {
        pngDiagPrint("[png-decode] decodePngToCpu: null filepath");
        return false;
    }

    // 1. Decode the PNG via stb_image, forcing 4 channels (RGBA8).
    //
    //    stbi_load returns a tightly packed RGBA8 buffer in R,G,B,A byte order
    //    (byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A). This matches the
    //    compositor's canonical surface and the hardware-verified framebuffer
    //    byte order, so no channel swapping is performed.
    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    stbi_uc* stbPixels = stbi_load(filepath, &width, &height, &channelsInFile,
                                   STBI_rgb_alpha);
    if (!stbPixels) {
        const char* reason = stbi_failure_reason();
        pngDiagPrint("[png-decode] stbi_load failed");
        if (reason) {
            printf("[png-decode]   reason: %s\n", reason);
            fflush(stdout);
        }
        return false;
    }

    // 2. Validate dimensions and decoded size before allocating project-owned
    //    storage. This preserves the SCE client's decode-size safety concept.
    if (width <= 0 || height <= 0) {
        pngDiagPrint("[png-decode] invalid dimensions");
        stbi_image_free(stbPixels);
        return false;
    }
    if (static_cast<uint32_t>(width) > kMaxDecodeDimension ||
        static_cast<uint32_t>(height) > kMaxDecodeDimension) {
        pngDiagPrint("[png-decode] image exceeds max dimension (4096)");
        stbi_image_free(stbPixels);
        return false;
    }

    const uint64_t w = static_cast<uint64_t>(width);
    const uint64_t h = static_cast<uint64_t>(height);
    const uint64_t totalBytes = w * h * 4ull;
    if (totalBytes == 0 || totalBytes > kMaxDecodeBytes) {
        pngDiagPrint("[png-decode] decoded size exceeds bound");
        stbi_image_free(stbPixels);
        return false;
    }

    // 3. Allocate project-owned storage and copy/transfer the decoded pixels.
    //    We copy out of stb's temporary buffer so stb-owned memory is not
    //    exposed as long-lived project state. stb's buffer is then freed.
    uint8_t* owned = static_cast<uint8_t*>(malloc(static_cast<size_t>(totalBytes)));
    if (!owned) {
        pngDiagPrint("[png-decode] allocation failed");
        stbi_image_free(stbPixels);
        return false;
    }
    memcpy(owned, stbPixels, static_cast<size_t>(totalBytes));

    // 4. Free stb's temporary decode buffer.
    stbi_image_free(stbPixels);

    // 5. Populate the output image (owns `owned`).
    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pitch = static_cast<uint32_t>(width) * 4u;
    outImage.rgba = owned;
    outImage.rgbaSize = static_cast<size_t>(totalBytes);
    return true;
}

} // namespace ps4

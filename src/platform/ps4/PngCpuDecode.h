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

// === PngCpuDecode.h ==========================================================
//
// OpenOrbis PS4 — Minimal CPU PNG decode helper.
//
// Ported from the SCE reference (moonlight-ps4-SCE-RC) with one
// OpenOrbis-specific adaptation: the decoded pixels are held in a raw
// pointer + byte size (owned by CpuPngImage) instead of std::vector<uint8_t>,
// matching the current OpenOrbis CpuImage representation. This avoids the
// toolchain libc++ <vector> include issue and keeps the image container
// structurally identical to ps4::CpuImage so UiCompositor::drawImage() can
// accept it directly without a copy.
//
// This is a tiny, self-contained helper that decodes a PNG file into a CPU
// RGBA8 buffer using the stb_image backend. It is used by the CPU
// UiCompositor and the CPU renderers to decode images without uploading them
// to the GPU.
//
// It intentionally does NOT create any GPU resources (no Garlic textures, no
// upload buffers, no GNMX objects). The caller owns the returned CPU buffer
// (via CpuPngImage's deterministic cleanup).
//
// The rest of the application does not care whether the backend is scePngDec
// or stb_image; no stb types are exposed in this public header.
//
// OpenOrbis SDK types are confined to the implementation file.
// =============================================================================

#ifndef PS4_PNG_CPU_DECODE_H
#define PS4_PNG_CPU_DECODE_H

#include <cstdint>
#include <cstddef>   // size_t
#include <cstdlib>   // free

namespace ps4 {

/// A CPU-owned RGBA8 image decoded from a PNG file.
///
/// OpenOrbis adaptation: pixels are held as a raw pointer + byte size rather
/// than std::vector to avoid the toolchain <vector> include issue. This struct
/// OWNS its `rgba` buffer (allocated with malloc) and frees it deterministically
/// on destruction or move-assignment. It is non-copyable but movable.
///
/// It is structurally identical to ps4::CpuImage (width/height/pitch/rgba), so
/// UiCompositor::drawImage() / drawImageSubRect() accept it directly without a
/// copy.
struct CpuPngImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;         ///< Row stride in bytes (tightly packed: width * 4).
    uint8_t* rgba = nullptr;    ///< Owned pixel data (RGBA8, malloc'd).
    size_t   rgbaSize = 0;      ///< Pixel data size in bytes.

    CpuPngImage() = default;

    /// Frees the owned pixel buffer. Safe on a default-constructed or moved-from
    /// object.
    ~CpuPngImage() { release(); }

    // Non-copyable (owns a raw buffer).
    CpuPngImage(const CpuPngImage&) = delete;
    CpuPngImage& operator=(const CpuPngImage&) = delete;

    // Movable.
    CpuPngImage(CpuPngImage&& other) noexcept { moveFrom(other); }
    CpuPngImage& operator=(CpuPngImage&& other) noexcept {
        if (this != &other) {
            release();
            moveFrom(other);
        }
        return *this;
    }

    /// Release the owned pixel buffer and reset all fields. Idempotent.
    void release() {
        if (rgba) {
            free(rgba);
            rgba = nullptr;
        }
        rgbaSize = 0;
        width = 0;
        height = 0;
        pitch = 0;
    }

    /// Returns true if no pixel data is present.
    bool empty() const { return rgba == nullptr || rgbaSize == 0; }

private:
    void moveFrom(CpuPngImage& other) noexcept {
        width = other.width;
        height = other.height;
        pitch = other.pitch;
        rgba = other.rgba;
        rgbaSize = other.rgbaSize;
        other.width = 0;
        other.height = 0;
        other.pitch = 0;
        other.rgba = nullptr;
        other.rgbaSize = 0;
    }
};

/// Decode a PNG file at `filepath` into a CPU RGBA8 buffer.
///
/// Returns true on success and fills `outImage`. On failure returns false and
/// leaves `outImage` empty (any partially allocated state is cleaned up). This
/// is a best-effort helper for the CPU compositor; it does not touch the GPU.
///
/// The decoded output is tightly packed RGBA8 (byte 0 = R, byte 1 = G,
/// byte 2 = B, byte 3 = A), matching the compositor's canonical surface and the
/// hardware-verified framebuffer byte order. No channel swapping is performed.
bool decodePngToCpu(const char* filepath, CpuPngImage& outImage);

} // namespace ps4

#endif // PS4_PNG_CPU_DECODE_H

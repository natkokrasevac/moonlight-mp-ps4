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

// === Nv12Converter.h ========================================================
//
// OpenOrbis Moonlight client — NV12 → BGRA CPU conversion
//
// Nv12Converter converts a decoded NV12 (YUV 4:2:0 planar) frame into a
// BGRA8 destination buffer using SSE2 SIMD (16 px/iter, 2 Y rows per UV row),
// matching the proven OpenOrbis alt reference for fast 1080p60 conversion.
//
// This is a pure CPU pixel-conversion utility. It owns no resources and has
// no lifecycle. It is used by the main-thread stream presentation path.
//
// The output is RGBA byte order (byte 0 = R, byte 1 = G, byte 2 = B,
// byte 3 = A) to match the GfxManager A8B8G8R8_SRGB framebuffer.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef PS4_NV12_CONVERTER_H
#define PS4_NV12_CONVERTER_H

#include <cstdint>

namespace ps4 {

// ---------------------------------------------------------------------------
// Nv12Converter
// ---------------------------------------------------------------------------

/// Converts NV12 decoded frames into BGRA8 destination buffers.
///
/// This is a stateless CPU utility. All methods are static.
class Nv12Converter {
public:
    /// Convert an NV12 decoded frame into a BGRA8 destination buffer.
    /// `dst` is the destination BGRA8 buffer (pitch bytes per row).
    /// `dstW`/`dstH` are the destination dimensions.
    /// The source is the decoded frame's Y/UV planes.
    ///
    /// Uses SSE2 SIMD (16 px/iter, 2 Y rows per UV row) matching the proven
    /// OpenOrbis alt reference for fast 1080p60 conversion. The output is
    /// RGBA byte order (byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A) to
    /// match the GfxManager A8B8G8R8_SRGB framebuffer.
    static void nv12ToBgra(uint8_t* dst, int dstPitch, int dstW, int dstH,
                           const uint8_t* y, const uint8_t* uv,
                           int pitchY, int pitchUV, int srcW, int srcH);
};

} // namespace ps4

#endif // PS4_NV12_CONVERTER_H

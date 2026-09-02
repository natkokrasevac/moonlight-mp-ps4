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

// === Nv12Converter.cpp ======================================================
//
// OpenOrbis Moonlight client — NV12 → BGRA CPU conversion
//
// Implements the Nv12Converter CPU pixel-conversion utility using SSE2 SIMD
// (16 px/iter, 2 Y rows per UV row), matching the proven OpenOrbis alt
// reference for fast 1080p60 conversion.
//
// The output is RGBA byte order (byte 0 = R, byte 1 = G, byte 2 = B,
// byte 3 = A) to match the GfxManager A8B8G8R8_SRGB framebuffer.
//
// SSE2 intrinsics are confined to this translation unit.
// ===============================================================================

#include "Nv12Converter.h"

#include <emmintrin.h>   // SSE2 — Jaguar PS4

#include <cstdint>

namespace ps4 {

/// Store 8 BGRA pixels from Y (epi16) + precomputed chroma terms.
/// Produces RGBA byte order (byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A)
/// to match the GfxManager A8B8G8R8_SRGB framebuffer.
static void bgraStore8(uint8_t* dp, __m128i y16, __m128i rv,
                       __m128i guv, __m128i bu, __m128i a255) {
    __m128i r = _mm_add_epi16(y16, rv);
    __m128i g = _mm_sub_epi16(y16, guv);
    __m128i b = _mm_add_epi16(y16, bu);
    __m128i br = _mm_packus_epi16(b, r);
    __m128i ga = _mm_packus_epi16(g, a255);
    __m128i bg0 = _mm_unpacklo_epi8(br, ga);
    __m128i ra0 = _mm_unpackhi_epi8(br, ga);
    __m128i px0 = _mm_unpacklo_epi16(bg0, ra0);
    __m128i px1 = _mm_unpackhi_epi16(bg0, ra0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dp), px0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dp + 16), px1);
}

void Nv12Converter::nv12ToBgra(uint8_t* dst, int dstPitch, int dstW, int dstH,
                               const uint8_t* y, const uint8_t* uv,
                               int pitchY, int pitchUV, int srcW, int srcH) {
    if (!dst || !y || !uv || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) {
        return;
    }

    // BT.601 limited-range YUV→RGB conversion (matching the alt reference).
    // Coefficients >>7 (179/44/92/227) ≈ BT.601 JPEG.
    auto clampU8 = [](int v) -> uint8_t {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };

    // --- 1:1 fast path (source and destination dimensions match) ------------
    if (srcW == dstW && srcH == dstH) {
        // SSE2 constants.
        const __m128i zero = _mm_setzero_si128();
        const __m128i m00ff = _mm_set1_epi16(0x00FF);
        const __m128i c128 = _mm_set1_epi16(128);
        const __m128i c179 = _mm_set1_epi16(179);
        const __m128i c44 = _mm_set1_epi16(44);
        const __m128i c92 = _mm_set1_epi16(92);
        const __m128i c227 = _mm_set1_epi16(227);
        const __m128i a255 = _mm_set1_epi16(255);

        int row = 0;
        for (; row + 1 < dstH; row += 2) {
            uint8_t* drow0 = dst + static_cast<size_t>(row) * dstPitch;
            uint8_t* drow1 = drow0 + dstPitch;
            const uint8_t* yrow0 = y + static_cast<size_t>(row) * pitchY;
            const uint8_t* yrow1 = yrow0 + pitchY;
            const uint8_t* uvrow = uv + static_cast<size_t>(row / 2) * pitchUV;
            int x = 0;
            for (; x + 15 < dstW; x += 16) {
                _mm_prefetch(reinterpret_cast<const char*>(yrow1 + pitchY + x), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(yrow1 + 2 * pitchY + x), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(uvrow + pitchUV + x), _MM_HINT_T0);

                // 16B interleaved UV = chroma for 16 px; deinterleave.
                __m128i uv8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvrow + x));
                __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
                __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
                __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c179), 7);
                __m128i guv = _mm_srai_epi16(
                    _mm_add_epi16(_mm_mullo_epi16(u16, c44),
                                  _mm_mullo_epi16(v16, c92)), 7);
                __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c227), 7);
                // Duplicate each chroma sample to its 2 px.
                __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
                __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
                __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
                __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
                __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
                __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

                __m128i y0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yrow0 + x));
                __m128i y1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yrow1 + x));

                // Store 8 px per call. To produce RGBA (byte0=R, byte2=B),
                // swap rv and bu (the alt reference produces BGRA).
                bgraStore8(drow0 + static_cast<size_t>(x) * 4,
                           _mm_unpacklo_epi8(y0, zero), bu_lo, guv_lo, rv_lo, a255);
                bgraStore8(drow0 + static_cast<size_t>(x) * 4 + 32,
                           _mm_unpackhi_epi8(y0, zero), bu_hi, guv_hi, rv_hi, a255);
                bgraStore8(drow1 + static_cast<size_t>(x) * 4,
                           _mm_unpacklo_epi8(y1, zero), bu_lo, guv_lo, rv_lo, a255);
                bgraStore8(drow1 + static_cast<size_t>(x) * 4 + 32,
                           _mm_unpackhi_epi8(y1, zero), bu_hi, guv_hi, rv_hi, a255);
            }
            // Scalar tail.
            for (; x < dstW; ++x) {
                int yy = yrow0[x];
                int uu = static_cast<int>(uvrow[(x / 2) * 2]) - 128;
                int vv = static_cast<int>(uvrow[(x / 2) * 2 + 1]) - 128;
                int r = yy + ((179 * vv) >> 7);
                int g = yy - ((44 * uu + 92 * vv) >> 7);
                int b = yy + ((227 * uu) >> 7);
                drow0[x * 4 + 0] = clampU8(r);
                drow0[x * 4 + 1] = clampU8(g);
                drow0[x * 4 + 2] = clampU8(b);
                drow0[x * 4 + 3] = 0xFF;
                drow1[x * 4 + 0] = clampU8(r);
                drow1[x * 4 + 1] = clampU8(g);
                drow1[x * 4 + 2] = clampU8(b);
                drow1[x * 4 + 3] = 0xFF;
            }
        }
        // Odd last row.
        for (; row < dstH; ++row) {
            uint8_t* drow = dst + static_cast<size_t>(row) * dstPitch;
            const uint8_t* yrow = y + static_cast<size_t>(row) * pitchY;
            const uint8_t* uvrow = uv + static_cast<size_t>(row / 2) * pitchUV;
            for (int x = 0; x < dstW; ++x) {
                int yy = yrow[x];
                int uu = static_cast<int>(uvrow[(x / 2) * 2]) - 128;
                int vv = static_cast<int>(uvrow[(x / 2) * 2 + 1]) - 128;
                int r = yy + ((179 * vv) >> 7);
                int g = yy - ((44 * uu + 92 * vv) >> 7);
                int b = yy + ((227 * uu) >> 7);
                drow[x * 4 + 0] = clampU8(r);
                drow[x * 4 + 1] = clampU8(g);
                drow[x * 4 + 2] = clampU8(b);
                drow[x * 4 + 3] = 0xFF;
            }
        }
        return;
    }

    // --- Scaled path (nearest-neighbour, SSE2-accelerated) ------------------
    // Two-step per row:
    //   1. Convert the source row (srcW pixels) to BGRA using SSE2 (the same
    //      YUV→RGB math as the 1:1 fast path).
    //   2. Scale the BGRA row up to dstW using a fast SSE2 nearest-neighbour
    //      blit (each source pixel is copied to 1 or more destination pixels).
    // This avoids the per-pixel scalar YUV→RGB conversion that made the old
    // scaled path extremely slow (e.g., 720p→1080p at ~10 fps).

    // SSE2 constants for the YUV→RGB conversion.
    const __m128i zero = _mm_setzero_si128();
    const __m128i m00ff = _mm_set1_epi16(0x00FF);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i c179 = _mm_set1_epi16(179);
    const __m128i c44 = _mm_set1_epi16(44);
    const __m128i c92 = _mm_set1_epi16(92);
    const __m128i c227 = _mm_set1_epi16(227);
    const __m128i a255 = _mm_set1_epi16(255);

    // Build a horizontal scale LUT (reused across rows): for each destination
    // x, the source x it maps to.
    int xLut[1920];
    for (int x = 0; x < dstW && x < 1920; ++x) {
        int sx = static_cast<int>((static_cast<int64_t>(x) * srcW) / dstW);
        if (sx >= srcW) sx = srcW - 1;
        xLut[x] = sx;
    }

    // Build a vertical scale LUT: for each destination row, the source row.
    // Bounded to 1080 (max framebuffer height).
    int yLut[1080];
    for (int row = 0; row < dstH && row < 1080; ++row) {
        int sr = static_cast<int>((static_cast<int64_t>(row) * srcH) / dstH);
        if (sr >= srcH) sr = srcH - 1;
        yLut[row] = sr;
    }

    // Scratch buffer for one converted source row (max 1920 px * 4 bytes).
    uint8_t srcRowBgra[1920 * 4];

    for (int row = 0; row < dstH; ++row) {
        const int srcRow = yLut[row];
        const int uvRow = srcRow / 2;
        uint8_t* drow = dst + static_cast<size_t>(row) * dstPitch;
        const uint8_t* yrow = y + static_cast<size_t>(srcRow) * pitchY;
        const uint8_t* uvrow = uv + static_cast<size_t>(uvRow) * pitchUV;

        // --- Step 1: convert the source row to BGRA using SSE2 --------------
        // Process 16 source pixels per iteration (matching the 1:1 fast path).
        int sx = 0;
        for (; sx + 15 < srcW; sx += 16) {
            __m128i uv8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvrow + sx));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c179), 7);
            __m128i guv = _mm_srai_epi16(
                _mm_add_epi16(_mm_mullo_epi16(u16, c44),
                              _mm_mullo_epi16(v16, c92)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c227), 7);
            __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
            __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
            __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
            __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
            __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
            __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

            __m128i y0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yrow + sx));
            bgraStore8(srcRowBgra + static_cast<size_t>(sx) * 4,
                       _mm_unpacklo_epi8(y0, zero), bu_lo, guv_lo, rv_lo, a255);
            bgraStore8(srcRowBgra + static_cast<size_t>(sx) * 4 + 32,
                       _mm_unpackhi_epi8(y0, zero), bu_hi, guv_hi, rv_hi, a255);
        }
        // Scalar tail for the source row.
        for (; sx < srcW; ++sx) {
            int yy = yrow[sx];
            int uu = static_cast<int>(uvrow[(sx / 2) * 2]) - 128;
            int vv = static_cast<int>(uvrow[(sx / 2) * 2 + 1]) - 128;
            int r = yy + ((179 * vv) >> 7);
            int g = yy - ((44 * uu + 92 * vv) >> 7);
            int b = yy + ((227 * uu) >> 7);
            uint8_t* sp = srcRowBgra + static_cast<size_t>(sx) * 4;
            sp[0] = clampU8(r);
            sp[1] = clampU8(g);
            sp[2] = clampU8(b);
            sp[3] = 0xFF;
        }

        // --- Step 2: scale the BGRA row up to dstW (nearest-neighbour) -----
        // Copy each source pixel to its destination position(s). For
        // upscaling, each source pixel maps to 1 or more destination pixels.
        // Process destination pixels in runs of 4 (16 bytes) using SSE2.
        int dx = 0;
        for (; dx + 3 < dstW; dx += 4) {
            // Gather the 4 source pixels for destination dx..dx+3.
            const uint8_t* s0 = srcRowBgra + static_cast<size_t>(xLut[dx]) * 4;
            const uint8_t* s1 = srcRowBgra + static_cast<size_t>(xLut[dx + 1]) * 4;
            const uint8_t* s2 = srcRowBgra + static_cast<size_t>(xLut[dx + 2]) * 4;
            const uint8_t* s3 = srcRowBgra + static_cast<size_t>(xLut[dx + 3]) * 4;
            // Build a 16-byte vector: [s0(4) s1(4) s2(4) s3(4)].
            __m128i v0 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s0));
            __m128i v1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s1));
            __m128i v2 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s2));
            __m128i v3 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s3));
            __m128i lo = _mm_unpacklo_epi64(v0, v1);  // s0 s1
            __m128i hi = _mm_unpacklo_epi64(v2, v3);  // s2 s3
            __m128i px = _mm_unpacklo_epi64(lo, hi);  // s0 s1 s2 s3
            _mm_storeu_si128(reinterpret_cast<__m128i*>(drow + static_cast<size_t>(dx) * 4), px);
        }
        // Scalar tail for the destination row.
        for (; dx < dstW; ++dx) {
            const uint8_t* sp = srcRowBgra + static_cast<size_t>(xLut[dx]) * 4;
            uint8_t* dp = drow + static_cast<size_t>(dx) * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

} // namespace ps4

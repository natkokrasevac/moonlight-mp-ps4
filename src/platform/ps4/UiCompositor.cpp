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

// === UiCompositor.cpp ========================================================
//
// OpenOrbis PS4 — CPU UI compositor.
//
// Ported from the SCE reference (moonlight-ps4-SCE-RC). The CPU
// surface is stored in RGBA8 byte order (byte 0 = R, byte 1 = G, byte 2 = B,
// byte 3 = A), matching the hardware-verified framebuffer byte order for
// ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB. This allows the UiBlitter to
// copy the compositor surface into the framebuffer with a plain memcpy (no
// per-pixel color conversion).
//
// Implements:
//   - Owns one CPU RGBA8 surface (1920×1080, tightly packed pitch).
//   - clear / fillRect / strokeRect / drawImage / drawImageSubRect / drawText.
//   - pushClip / popClip with a small fixed-capacity clip stack.
//   - Straight-alpha source-over blending.
//   - Nearest-neighbor image scaling.
//   - Aspect-fit helper for card artwork.
//
// This is a scalar, single-threaded, no-SIMD implementation. It does not
// upload anything to the GPU directly; the UiBlitter presents its surface.
// =============================================================================

#include "UiCompositor.h"

#include "FontRasterizer.h"

#include <cstdlib>   // free
#include <cstring>   // memset

namespace ps4 {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

UiCompositor::UiCompositor() = default;

UiCompositor::~UiCompositor() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool UiCompositor::initialize(uint32_t width, uint32_t height,
                              FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (width == 0 || height == 0) {
        return false;
    }

    // Tightly packed RGBA8 surface.
    const uint64_t totalBytes = static_cast<uint64_t>(width) * height * 4;
    if (totalBytes == 0 || totalBytes > 0xFFFFFFFFull) {
        return false;
    }

    m_pixels = static_cast<uint8_t*>(malloc(static_cast<size_t>(totalBytes)));
    if (!m_pixels) {
        return false;
    }

    m_width = width;
    m_height = height;
    m_pitch = width * 4;
    m_fonts = fonts;
    m_clipDepth = 0;
    m_initialized = true;

    // Default clip: full framebuffer.
    m_clipStack[0] = Rect{0, 0, static_cast<int>(width),
                          static_cast<int>(height)};
    m_clipDepth = 1;
    return true;
}

void UiCompositor::shutdown() {
    if (m_pixels) {
        free(m_pixels);
        m_pixels = nullptr;
    }
    m_width = 0;
    m_height = 0;
    m_pitch = 0;
    m_fonts = nullptr;
    m_clipDepth = 0;
    m_initialized = false;
}

bool UiCompositor::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Surface access
// ---------------------------------------------------------------------------

const uint8_t* UiCompositor::pixels() const {
    return m_pixels;
}

uint8_t* UiCompositor::pixelsWritable() {
    return m_pixels;
}

uint32_t UiCompositor::width() const {
    return m_width;
}

uint32_t UiCompositor::height() const {
    return m_height;
}

uint32_t UiCompositor::pitch() const {
    return m_pitch;
}

// ---------------------------------------------------------------------------
// Blending
// ---------------------------------------------------------------------------

void UiCompositor::blendPixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t a) {
    if (a == 0) {
        return;
    }
    if (a == 255) {
        // Opaque: store RGBA8 (byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A).
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255;
        return;
    }
    const uint32_t sa = a;
    const uint32_t invA = 255u - sa;
    // The destination is stored RGBA8; blend in the same channel order.
    dst[0] = static_cast<uint8_t>((r * sa + dst[0] * invA) / 255u);
    dst[1] = static_cast<uint8_t>((g * sa + dst[1] * invA) / 255u);
    dst[2] = static_cast<uint8_t>((b * sa + dst[2] * invA) / 255u);
    dst[3] = 255;
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

Rect UiCompositor::currentClip() const {
    if (m_clipDepth <= 0) {
        return Rect{0, 0, static_cast<int>(m_width),
                    static_cast<int>(m_height)};
    }
    return m_clipStack[m_clipDepth - 1];
}

bool UiCompositor::clipRect(const Rect& in, Rect& out) const {
    const Rect clip = currentClip();

    int x0 = in.x;
    int y0 = in.y;
    int x1 = in.x + in.w;
    int y1 = in.y + in.h;

    // Clip to framebuffer.
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int>(m_width)) x1 = static_cast<int>(m_width);
    if (y1 > static_cast<int>(m_height)) y1 = static_cast<int>(m_height);

    // Clip to current clip rect.
    const int cx0 = clip.x;
    const int cy0 = clip.y;
    const int cx1 = clip.x + clip.w;
    const int cy1 = clip.y + clip.h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;

    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    out.x = x0;
    out.y = y0;
    out.w = x1 - x0;
    out.h = y1 - y0;
    return true;
}

bool UiCompositor::pushClip(const Rect& rect) {
    if (m_clipDepth >= kMaxClipDepth) {
        return false;
    }
    const Rect cur = currentClip();

    // Intersect the new rect with the current clip.
    int x0 = rect.x;
    int y0 = rect.y;
    int x1 = rect.x + rect.w;
    int y1 = rect.y + rect.h;
    const int cx0 = cur.x;
    const int cy0 = cur.y;
    const int cx1 = cur.x + cur.w;
    const int cy1 = cur.y + cur.h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;

    if (x1 <= x0 || y1 <= y0) {
        return false;   // Empty intersection; do not push.
    }

    m_clipStack[m_clipDepth] = Rect{x0, y0, x1 - x0, y1 - y0};
    ++m_clipDepth;
    return true;
}

bool UiCompositor::popClip() {
    if (m_clipDepth <= 1) {
        // Never pop below the default full-framebuffer clip.
        return false;
    }
    --m_clipDepth;
    return true;
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

void UiCompositor::clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!m_pixels) {
        return;
    }
    if (a == 255) {
        // Fast path: fill every pixel with the opaque color (RGBA8).
        for (uint32_t y = 0; y < m_height; ++y) {
            uint8_t* row = m_pixels + y * m_pitch;
            for (uint32_t x = 0; x < m_width; ++x) {
                uint8_t* px = row + x * 4;
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = 255;
            }
        }
    } else {
        // Blend over the existing surface.
        for (uint32_t y = 0; y < m_height; ++y) {
            uint8_t* row = m_pixels + y * m_pitch;
            for (uint32_t x = 0; x < m_width; ++x) {
                blendPixel(row + x * 4, r, g, b, a);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// fillRect
// ---------------------------------------------------------------------------

void UiCompositor::fillRect(int x, int y, int w, int h,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!m_pixels) {
        return;
    }
    Rect clipped;
    if (!clipRect(Rect{x, y, w, h}, clipped)) {
        return;
    }
    if (a == 255) {
        for (int yy = 0; yy < clipped.h; ++yy) {
            uint8_t* row = m_pixels + (clipped.y + yy) * m_pitch +
                           clipped.x * 4;
            for (int xx = 0; xx < clipped.w; ++xx) {
                uint8_t* px = row + xx * 4;
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = 255;
            }
        }
    } else {
        for (int yy = 0; yy < clipped.h; ++yy) {
            uint8_t* row = m_pixels + (clipped.y + yy) * m_pitch +
                           clipped.x * 4;
            for (int xx = 0; xx < clipped.w; ++xx) {
                blendPixel(row + xx * 4, r, g, b, a);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// strokeRect
// ---------------------------------------------------------------------------

void UiCompositor::strokeRect(int x, int y, int w, int h, int thickness,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (thickness < 1) {
        thickness = 1;
    }
    if (thickness > 2) {
        thickness = 2;
    }
    // Draw four filled strips.
    fillRect(x, y, w, thickness, r, g, b, a);                       // top
    fillRect(x, y + h - thickness, w, thickness, r, g, b, a);       // bottom
    fillRect(x, y, thickness, h, r, g, b, a);                       // left
    fillRect(x + w - thickness, y, thickness, h, r, g, b, a);       // right
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// drawImage and drawImageSubRect are now templates defined in UiCompositor.h
// so they can accept both ps4::CpuImage and ps4::CpuPngImage without a copy.

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

float UiCompositor::drawText(FontRasterizer::FontFace face, const char* text,
                             float x, float baselineY,
                             uint8_t r, uint8_t g, uint8_t b) {
    if (!m_fonts || !text) {
        return 0.0f;
    }
    return m_fonts->drawText(face, text, x, baselineY, r, g, b,
                             m_pixels, static_cast<int>(m_width),
                             static_cast<int>(m_height),
                             static_cast<int>(m_pitch));
}

// ---------------------------------------------------------------------------
// Aspect-fit helper
// ---------------------------------------------------------------------------

Rect UiCompositor::aspectFit(int srcW, int srcH,
                             int dstX, int dstY, int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return Rect{dstX, dstY, 0, 0};
    }
    const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    const float dstAspect = static_cast<float>(dstW) / static_cast<float>(dstH);

    int fitW = dstW;
    int fitH = dstH;
    if (srcAspect > dstAspect) {
        // Source is wider relative to destination: fit to width, pillarbox.
        fitH = static_cast<int>(static_cast<float>(dstW) / srcAspect);
        if (fitH > dstH) fitH = dstH;
    } else {
        // Source is taller relative to destination: fit to height, letterbox.
        fitW = static_cast<int>(static_cast<float>(dstH) * srcAspect);
        if (fitW > dstW) fitW = dstW;
    }

    const int outX = dstX + (dstW - fitW) / 2;
    const int outY = dstY + (dstH - fitH) / 2;
    return Rect{outX, outY, fitW, fitH};
}

} // namespace ps4

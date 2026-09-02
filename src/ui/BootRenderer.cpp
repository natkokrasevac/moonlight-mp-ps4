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

// === BootRenderer.cpp =======================================================
//
// Moonlight client for PlayStation 4 — CPU Boot screen renderer.
//
// Implements the BootRenderer: draws the Boot screen (background + Moonlight
// logo + title + optional status text) into the shared CPU UiCompositor
// surface using only fillRect / drawImage / drawText. It consumes a plain
// BootRenderState snapshot and owns the Boot layout geometry and the retained
// CPU-decoded logo image.
//
// This translation unit contains no OpenOrbis SDK calls.
//
// OpenOrbis adaptation: the retained logo/background are held as owning
// ps4::CpuPngImage (the OpenOrbis CpuImage does not own its buffer). The
// decoded CpuPngImage is moved into the member via move-assignment, which
// transfers ownership of the malloc'd RGBA8 buffer. Emptiness is tested with
// the CpuPngImage::empty() method.
// =============================================================================

#include "BootRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "platform/ps4/PngCpuDecode.h"
#include "UiTheme.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush
#include <utility>   // std::move

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void bootDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

BootRenderer::BootRenderer() = default;

BootRenderer::~BootRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool BootRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;

    // Decode the retained CPU logo image once. Failure to decode is non-fatal:
    // the Boot screen renders without the logo (background + title only).
    {
        ps4::CpuPngImage logoPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_boot.png",
                                logoPng)) {
            m_logoImg = std::move(logoPng);
        } else {
            bootDiagPrint("[boot] moonlight_boot.png decode failed (Boot renders without logo)");
        }

        ps4::CpuPngImage bgPng;
        if (ps4::decodePngToCpu("/app0/assets/images/moonlight_bg.png", bgPng)) {
            m_bgImg = std::move(bgPng);
        } else {
            bootDiagPrint("[boot] moonlight_bg.png decode failed (Boot uses solid background)");
        }
    }

    m_initialized = true;
    return true;
}

void BootRenderer::shutdown() {
    m_fonts = nullptr;
    m_logoImg = ps4::CpuPngImage();
    m_bgImg = ps4::CpuPngImage();
    m_initialized = false;
}

bool BootRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect BootRenderer::bounds() const {
    // The Boot screen covers the full framebuffer.
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect BootRenderer::logoRect() const {
    // The logo is drawn at its native size (no scaling), centered on screen.
    const int logoW = static_cast<int>(m_logoImg.width);
    const int logoH = static_cast<int>(m_logoImg.height);
    const int logoX = (1920 - logoW) / 2;
    const int logoY = (1080 - logoH) / 2;
    return ps4::Rect{logoX, logoY, logoW, logoH};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void BootRenderer::render(const BootRenderState& state,
                          ps4::UiCompositor& comp) {
    // 1. Draw the shared background image (falls back to solid color if empty).
    drawBackground(comp, m_bgImg);

    // 2. If the logo is available, draw it centered on screen at its native
    //    size. No title/subtitle is shown when the logo is present.
    if (!m_logoImg.empty()) {
        const ps4::Rect logo = logoRect();
        comp.drawImage(m_logoImg, logo.x, logo.y, logo.w, logo.h);
        return;
    }

    // 3. Fallback (no logo / failed to load): draw the title (and any
    //    subtitle) centered on screen.
    if (!m_fonts) {
        return;
    }

    float titleBaseline = 0.0f, titleLineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 titleBaseline, titleLineHeight);

    float subBaseline = 0.0f, subLineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 subBaseline, subLineHeight);

    // Center the title+subtitle block vertically on screen.
    const bool hasTitle = !state.title.empty();
    const bool hasSub = !state.subtitle.empty();
    const float blockHeight =
        (hasTitle ? titleLineHeight : 0.0f) +
        (hasTitle && hasSub ? 16.0f : 0.0f) +
        (hasSub ? subLineHeight : 0.0f);
    const float blockTop = (1080.0f - blockHeight) * 0.5f;

    if (hasTitle) {
        const float titleY = blockTop;
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Bold,
                         state.title.c_str(), 0.0f, 1920.0f,
                         titleY + titleBaseline,
                         UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    if (hasSub) {
        const float subY = blockTop + titleLineHeight + 16.0f;
        drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                         state.subtitle.c_str(), 0.0f, 1920.0f,
                         subY + subBaseline,
                         UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);
    }
}

} // namespace ui

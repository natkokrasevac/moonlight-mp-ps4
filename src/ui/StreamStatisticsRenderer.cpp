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

// === StreamStatisticsRenderer.cpp ==========================================
//
// OpenOrbis Moonlight client — CPU in-stream statistics overlay renderer.
//
// Implements the StreamStatisticsRenderer: draws the in-stream statistics
// overlay (semi-transparent dark panel + a vertical list of statistics rows)
// into the shared CPU UiCompositor surface using only fillRect / strokeRect /
// drawText / clipping. It consumes a plain StreamStatisticsRenderState snapshot
// and owns the statistics panel layout geometry.
//
// This translation unit contains no OpenOrbis SDK calls.
// =============================================================================

#include "StreamStatisticsRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "UiTheme.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush
#include <cstring>   // memset (dirty-region transparent clear)

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void statsDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

StreamStatisticsRenderer::StreamStatisticsRenderer() = default;

StreamStatisticsRenderer::~StreamStatisticsRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool StreamStatisticsRenderer::initialize(ps4::FontRasterizer* fonts) {
    if (m_initialized) {
        return true;
    }
    if (!fonts) {
        return false;
    }
    m_fonts = fonts;
    m_initialized = true;
    return true;
}

void StreamStatisticsRenderer::shutdown() {
    m_fonts = nullptr;
    m_initialized = false;
}

bool StreamStatisticsRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect StreamStatisticsRenderer::bounds() const {
    // The statistics panel is at the top-left with a safe screen inset. The
    // panel background is drawn with a 10px outward margin around the content
    // area.
    const int rowCount = 6;
    const int contentH = static_cast<int>(padding() * 2.0f + rowCount * rowHeight());
    const int x = static_cast<int>(panelX()) - 10;
    const int y = static_cast<int>(panelY()) - 10;
    const int w = static_cast<int>(panelW()) + 10;
    const int h = contentH + 10;
    return ps4::Rect{x, y, w, h};
}

float StreamStatisticsRenderer::rowTop(int rowIndex) const {
    // The rows start at the top of the panel content area (panelY).
    return panelY() + rowIndex * rowHeight();
}

ps4::Rect StreamStatisticsRenderer::rowBounds(int rowIndex) const {
    const float y = rowTop(rowIndex);
    const int pad = 3;
    return ps4::Rect{static_cast<int>(panelX()) - pad,
                     static_cast<int>(y) - pad,
                     static_cast<int>(panelW()) + pad * 2,
                     static_cast<int>(rowHeight()) + pad * 2};
}

bool StreamStatisticsRenderer::rectsIntersect(const ps4::Rect& a,
                                              const ps4::Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void StreamStatisticsRenderer::render(const StreamStatisticsRenderState& state,
                                      ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    // The renderer draws only when the statistics overlay is visible. The
    // caller (MoonlightApp) invokes this only while the overlay is active, so
    // this is a defensive guard.
    if (!state.visible) {
        return;
    }

    // --- Semi-transparent dark panel background. ---------------------------
    // The compositor surface is transparent; this fills only the panel region
    // with a dark translucent color so the decoded video shows through.
    const ps4::Rect panel = bounds();
    comp.fillRect(panel.x, panel.y, panel.w, panel.h, 0, 0, 0, UiTheme::kDimAlpha);

    // --- Statistics rows. ---------------------------------------------------
    // Row order: Resolution, FPS, Bitrate, Decode, RTT (network latency),
    // Jitter.
    drawRow(UiStrings::kStatResolution, state.resolution, 0, rowTop(0), comp);
    drawRow(UiStrings::kStatFps, state.fps, 1, rowTop(1), comp);
    drawRow(UiStrings::kStatBitrate, state.bitrate, 2, rowTop(2), comp);
    drawRow(UiStrings::kStatDecode, state.decodeTime, 3, rowTop(3), comp);
    drawRow(UiStrings::kStatNetwork, state.networkLatency, 4, rowTop(4), comp);
    drawRow(UiStrings::kStatJitter, state.jitter, 5, rowTop(5), comp);
}

void StreamStatisticsRenderer::renderRegion(const StreamStatisticsRenderState& state,
                                            ps4::UiCompositor& comp,
                                            const ps4::Rect& dirty) {
    if (!m_fonts) {
        return;
    }
    if (!comp.pushClip(dirty)) {
        return;   // Empty clip; nothing to recompose.
    }

    // --- Clear the dirty region to fully transparent. ----------------------
    // The panel background is semi-transparent (alpha 140), so repainting it
    // over the previous row's text would leave the old text faintly visible
    // underneath (the old text would show through dimmed). To fully erase the
    // previous row, the dirty region must be cleared to RGBA 0,0,0,0 first.
    // The compositor's fillRect() treats alpha==0 as a no-op, so the region is
    // cleared directly via the writable pixels.
    {
        uint8_t* px = comp.pixelsWritable();
        const uint32_t pitch = comp.pitch();
        for (int yy = 0; yy < dirty.h; ++yy) {
            uint8_t* row = px + static_cast<size_t>(dirty.y + yy) * pitch +
                           static_cast<size_t>(dirty.x) * 4;
            memset(row, 0, static_cast<size_t>(dirty.w) * 4);
        }
    }

    // Repaint the panel background intersecting the dirty region. Rows live
    // inside the semi-transparent panel, so the panel background is the base
    // for row recomposition.
    const ps4::Rect panel = bounds();
    if (rectsIntersect(dirty, panel)) {
        comp.fillRect(panel.x, panel.y, panel.w, panel.h, 0, 0, 0, UiTheme::kDimAlpha);
    }

    // Redraw every row that intersects the dirty region.
    const char* labels[6] = {UiStrings::kStatResolution, UiStrings::kStatFps,
                             UiStrings::kStatBitrate, UiStrings::kStatDecode,
                             UiStrings::kStatNetwork, UiStrings::kStatJitter};
    const std::string* values[6] = {&state.resolution, &state.fps,
                                    &state.bitrate, &state.decodeTime,
                                    &state.networkLatency, &state.jitter};
    for (int i = 0; i < 6; ++i) {
        if (rectsIntersect(dirty, rowBounds(i))) {
            drawRow(labels[i], *values[i], i, rowTop(i), comp);
        }
    }

    comp.popClip();
}

// ---------------------------------------------------------------------------
// Row drawing
// ---------------------------------------------------------------------------

void StreamStatisticsRenderer::drawRow(const char* label,
                                       const std::string& value, int rowIndex,
                                       float rowY, ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float textBaseline = rowY + (rowHeight() - lineHeight) * 0.5f +
                               baselineY;

    // Label (left-aligned within the panel).
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, label,
                  contentX(), textBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Value (right-aligned within the panel). An empty value renders as "N/A"
    // rather than a misleading zero.
    const std::string display = value.empty() ? std::string("N/A") : value;
    drawRightAlignedText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                         display.c_str(), panelX() + panelW() - 10.0f,
                         textBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

} // namespace ui

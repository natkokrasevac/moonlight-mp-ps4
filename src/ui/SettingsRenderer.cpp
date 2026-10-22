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

// === SettingsRenderer.cpp ===================================================
//
// OpenOrbis Moonlight client — CPU Settings overlay renderer.
//
// Implements the SettingsRenderer: draws the Settings overlay (full-screen dim
// + right-side panel + rows) into the shared CPU UiCompositor surface using
// only fillRect / strokeRect / drawText / clipping. It consumes a plain
// SettingsRenderState snapshot and owns the Settings layout geometry.
//
// This translation unit contains no OpenOrbis SDK calls.
// =============================================================================

#include "SettingsRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "UiTheme.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void settingsDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SettingsRenderer::SettingsRenderer() = default;

SettingsRenderer::~SettingsRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool SettingsRenderer::initialize(ps4::FontRasterizer* fonts) {
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

void SettingsRenderer::shutdown() {
    m_fonts = nullptr;
    m_initialized = false;
}

bool SettingsRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect SettingsRenderer::dimBounds() const {
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect SettingsRenderer::panelBounds() const {
    return ps4::Rect{static_cast<int>(panelX()), 0,
                     static_cast<int>(panelW()), 1080};
}

float SettingsRenderer::rowTop(int rowIndex) const {
    // Rows 0-5 are under the STREAM header (after the title block).
    // Rows 6-7 (Presentation Style, Show Quit App Dialog) are under the CLIENT
    // header.
    // Rows 8-9 (Speakers, Game Optimization) are under the HOST header.
    const float baseY = margin() + titleBlockHeight() + sectionHeaderHeight();
    if (rowIndex <= 5) {
        return baseY + rowIndex * rowHeight();
    }
    // Rows 6-7: after rows 0-5, a separator, and the CLIENT header.
    const float clientRowTop =
        baseY + 6 * rowHeight() + separatorHeight() + sectionHeaderHeight();
    if (rowIndex <= 7) {
        return clientRowTop + (rowIndex - 6) * rowHeight();
    }
    // Rows 8-9: after row 7, a separator, and the HOST header.
    const float hostRowTop =
        clientRowTop + 2 * rowHeight() + separatorHeight() + sectionHeaderHeight();
    return hostRowTop + (rowIndex - 8) * rowHeight();
}

ps4::Rect SettingsRenderer::rowBounds(int rowIndex) const {
    const float y = rowTop(rowIndex);
    const int pad = 3;
    return ps4::Rect{static_cast<int>(panelX()) - pad,
                     static_cast<int>(y) - pad,
                     static_cast<int>(panelW()) + pad * 2,
                     static_cast<int>(rowHeight()) + pad * 2};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void SettingsRenderer::renderFull(const SettingsRenderState& state,
                                  ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Right-side panel (opaque/semi-opaque). No full-screen dim layer is
    // applied (removed for the GPU UI path; the settings panel draws directly
    // over the base screen, consistent with PS UI presentation).
    const int px = static_cast<int>(panelX());
    const int pw = static_cast<int>(panelW());
    comp.fillRect(px, 0, pw, 1080,
                  UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 220);   // ~0.98 alpha dark

    // Panel content.
    const float contentX = panelX() + margin();

    // Title "Settings" (Bold).
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kSettings,
                  contentX, margin() + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator under the title.
    const float titleBottom = margin() + titleBlockHeight();
    comp.fillRect(px, static_cast<int>(titleBottom - 8.0f),
                  pw, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // "STREAM" section header.
    const float streamHeaderY = margin() + titleBlockHeight();
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kStreamSection,
                  contentX, streamHeaderY + baselineY, 200, 200, 210);

    // Rows 0-5 (under STREAM).
    for (int i = 0; i < 6 && i < static_cast<int>(state.rows.size()); ++i) {
        drawRow(state.rows[i], i, rowTop(i), comp);
    }

    // Section separator lines (between STREAM/CLIENT/HOST).
    drawSeparators(comp);

    // "CLIENT" section header.
    const float clientHeaderY = rowTop(6) - sectionHeaderHeight();
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kClientSection,
                  contentX, clientHeaderY + baselineY, 200, 200, 210);

    // Rows 6-7 (Presentation Style, Show Quit App Dialog) under CLIENT.
    for (int i = 6; i < 8 && i < static_cast<int>(state.rows.size()); ++i) {
        drawRow(state.rows[i], i, rowTop(i), comp);
    }

    // "HOST" section header.
    const float hostHeaderY = rowTop(8) - sectionHeaderHeight();
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kHostSection,
                  contentX, hostHeaderY + baselineY, 200, 200, 210);

    // Rows 8-9 (Speakers, Game Optimization) under HOST.
    for (int i = 8; i < 10 && i < static_cast<int>(state.rows.size()); ++i) {
        drawRow(state.rows[i], i, rowTop(i), comp);
    }

    // --- Footer (app name + version) ---------------------------------------
    // Two small lines pinned to the bottom of the panel, using the
    // RegularSmall face.
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::RegularSmall,
                                 baselineY, lineHeight);
    const float footerBottom = 1080.0f - margin();
    const float footerLine1Y = footerBottom - lineHeight * 2.0f;
    const float footerLine2Y = footerBottom - lineHeight;
    comp.drawText(ps4::FontRasterizer::FontFace::RegularSmall, UiStrings::kAppName,
                  contentX, footerLine1Y + baselineY,
                  UiTheme::kDisabledTextR, UiTheme::kDisabledTextG, UiTheme::kDisabledTextB);
    comp.drawText(ps4::FontRasterizer::FontFace::RegularSmall, UiStrings::kVersionLabel,
                  contentX, footerLine2Y + baselineY,
                  UiTheme::kDisabledTextR, UiTheme::kDisabledTextG, UiTheme::kDisabledTextB);
    // Version number, right-aligned on the same line as the version label.
    drawRightAlignedText(comp, *m_fonts, ps4::FontRasterizer::FontFace::RegularSmall,
                         UiStrings::kVersionNumber,
                         panelX() + panelW() - margin(), footerLine2Y + baselineY,
                         UiTheme::kDisabledTextR, UiTheme::kDisabledTextG, UiTheme::kDisabledTextB);
}

// ---------------------------------------------------------------------------
// Section separator lines
// ---------------------------------------------------------------------------

void SettingsRenderer::drawSeparators(ps4::UiCompositor& comp) {
    const int px = static_cast<int>(panelX());
    const int pw = static_cast<int>(panelW());

    // Separator between STREAM and CLIENT sections. Placed above the CLIENT
    // heading (after the spacing gap that follows the STREAM rows), leaving the
    // same gap between the separator and the heading as the STREAM heading has
    // from the separator above it. Visual order: rows -> spacing -> separator
    // -> gap -> heading.
    const float clientSepY = rowTop(6) - sectionHeaderHeight() - 8.0f;
    comp.fillRect(px, static_cast<int>(clientSepY), pw, 2,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Separator between CLIENT and HOST sections. Placed above the HOST
    // heading, after the spacing gap that follows the CLIENT rows, with the
    // same gap to the heading as the STREAM heading has.
    const float hostSepY = rowTop(8) - sectionHeaderHeight() - 8.0f;
    comp.fillRect(px, static_cast<int>(hostSepY), pw, 2,
                  UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);
}

// ---------------------------------------------------------------------------
// Row drawing
// ---------------------------------------------------------------------------

void SettingsRenderer::drawRow(const SettingsRowRenderData& row, int rowIndex,
                               float rowY, ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    const int px = static_cast<int>(panelX());
    const int pw = static_cast<int>(panelW());
    const int ry = static_cast<int>(rowY);
    const int rh = static_cast<int>(rowHeight());

    // Selection highlight (only for enabled selected rows).
    if (row.selected && row.enabled) {
        comp.fillRect(px, ry, pw, rh,
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(px, ry, pw, rh, 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }

    // Label and value.
    const float contentX = panelX() + margin();
    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float textBaseline = rowY + (rowHeight() - lineHeight) * 0.5f +
                               baselineY;

    const float valueRightX = panelX() + panelW() - margin();
    if (row.enabled) {
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, row.label.c_str(),
                      contentX, textBaseline,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
        // Value, right-aligned within the panel. A value that exceeds the
        // recommended value for the current stream specs is drawn emphasized
        // (orange, bold) to flag the over-recommendation.
        if (row.valueWarning) {
            drawRightAlignedText(comp, *m_fonts,
                                 ps4::FontRasterizer::FontFace::RegularBold,
                                 row.value.c_str(), valueRightX, textBaseline,
                                 UiTheme::kValueWarningR, UiTheme::kValueWarningG,
                                 UiTheme::kValueWarningB);
        } else {
            drawRightAlignedText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                                 row.value.c_str(), valueRightX, textBaseline,
                                 UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
        }
    } else {
        // Disabled row: dimmed label and value.
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, row.label.c_str(),
                      contentX, textBaseline,
                      UiTheme::kDisabledTextR, UiTheme::kDisabledTextG, UiTheme::kDisabledTextB);
        drawRightAlignedText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                             row.value.c_str(), valueRightX, textBaseline,
                             UiTheme::kDisabledTextR, UiTheme::kDisabledTextG, UiTheme::kDisabledTextB);
    }
}

} // namespace ui

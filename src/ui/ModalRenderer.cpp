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

// === ModalRenderer.cpp ======================================================
//
// OpenOrbis Moonlight client — CPU confirmation modal renderer.
//
// Implements the ModalRenderer: draws the confirmation modals (Delete Host,
// Quit App) into the shared CPU UiCompositor surface using only fillRect /
// strokeRect / drawText / clipping. It consumes a plain ModalRenderState
// snapshot and owns the modal layout geometry.
//
// This translation unit contains no OpenOrbis SDK calls.
// =============================================================================

#include "ModalRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "UiTheme.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

#include <cstdio>    // printf, fflush

namespace ui {

// ---------------------------------------------------------------------------
// Diagnostics helper
// ---------------------------------------------------------------------------

static void modalDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ModalRenderer::ModalRenderer() = default;

ModalRenderer::~ModalRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool ModalRenderer::initialize(ps4::FontRasterizer* fonts) {
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

void ModalRenderer::shutdown() {
    m_fonts = nullptr;
    m_initialized = false;
}

bool ModalRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect ModalRenderer::dimBounds() const {
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect ModalRenderer::panelBounds(ModalKind kind) const {
    // DeleteHost and QuitApp both use the same 720x360 No/Yes panel.
    (void)kind;
    return ps4::Rect{(1920 - 720) / 2, (1080 - 360) / 2, 720, 360};
}

ps4::Rect ModalRenderer::choiceBounds(ModalKind kind) const {
    const ps4::Rect panel = panelBounds(kind);
    const float margin = 32.0f;
    // DeleteHost and QuitApp both use No/Yes buttons: two 160x56 buttons with
    // a 40px gap, centered at the bottom of the panel.
    (void)kind;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = choiceWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;
    return ps4::Rect{static_cast<int>(startX) - 3,
                     static_cast<int>(y) - 3,
                     static_cast<int>(totalWidth) + 6,
                     static_cast<int>(choiceHeight) + 6};
}

bool ModalRenderer::rectsIntersect(const ps4::Rect& a, const ps4::Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void ModalRenderer::renderFull(const ModalRenderState& state,
                               ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // 1. Full-screen dim layer (translucent black). The base screen is already
    //    present in the surface; the dim is composited over it.
    comp.fillRect(0, 0, 1920, 1080, 0, 0, 0, UiTheme::kDimAlpha);   // ~0.55 alpha

    // 2. Centered modal panel (opaque/semi-opaque).
    const ps4::Rect panel = panelBounds(state.kind);
    comp.fillRect(panel.x, panel.y, panel.w, panel.h,
                  UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 250);
    comp.strokeRect(panel.x, panel.y, panel.w, panel.h, 2,
                    UiTheme::kPanelBorderR, UiTheme::kPanelBorderG, UiTheme::kPanelBorderB, 255);

    // 3. Modal content.
    switch (state.kind) {
    case ModalKind::DeleteHost:
        drawDeleteHostContent(state, comp);
        break;
    case ModalKind::QuitApp:
        drawQuitAppContent(state, comp);
        break;
    case ModalKind::ResetMoonlight:
        drawResetMoonlightContent(state, comp);
        break;
    default:
        drawQuitAppContent(state, comp);
        break;
    }
}

void ModalRenderer::renderRegion(const ModalRenderState& state,
                                 ps4::UiCompositor& comp,
                                 const ps4::Rect& dirty) {
    if (!m_fonts) {
        return;
    }
    if (!comp.pushClip(dirty)) {
        return;   // Empty clip; nothing to recompose.
    }

    // Repaint the panel background intersecting the dirty region. Choice
    // buttons live inside the opaque panel, so the panel background is the
    // base for choice recomposition (no dim handling needed inside the opaque
    // panel).
    const ps4::Rect panel = panelBounds(state.kind);
    if (rectsIntersect(dirty, panel)) {
        comp.fillRect(panel.x, panel.y, panel.w, panel.h,
                      UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 250);
        comp.strokeRect(panel.x, panel.y, panel.w, panel.h, 2,
                        UiTheme::kPanelBorderR, UiTheme::kPanelBorderG, UiTheme::kPanelBorderB, 255);
    }

    // Redraw the choice buttons if they intersect the dirty region.
    if (rectsIntersect(dirty, choiceBounds(state.kind))) {
        switch (state.kind) {
        case ModalKind::DeleteHost:
            drawDeleteHostChoices(state, comp);
            break;
        case ModalKind::QuitApp:
            drawQuitAppChoices(state, comp);
            break;
        case ModalKind::ResetMoonlight:
            drawResetMoonlightChoices(state, comp);
            break;
        default:
            drawQuitAppChoices(state, comp);
            break;
        }
    }

    comp.popClip();
}

// ---------------------------------------------------------------------------
// Delete Host modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawDeleteHostContent(const ModalRenderState& state,
                                          ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::DeleteHost);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title "Delete Host?" (Bold).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, "Unpair Host",
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator.
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Host display name (Bold).
    if (!state.hostName.empty()) {
        comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.hostName.c_str(),
                      contentX, panel.y + margin + 80.0f + baselineY,
                      UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    }

    // Explanatory message (Regular).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular,
                  "Remove saved pairing data?",
                  contentX, panel.y + margin + 140.0f + baselineY,
                  UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);

    // No/Yes choices.
    drawDeleteHostChoices(state, comp);
}

void ModalRenderer::drawDeleteHostChoices(const ModalRenderState& state,
                                          ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::DeleteHost);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = choiceWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);

    // No choice.
    const int noX = static_cast<int>(startX);
    const int noY = static_cast<int>(y);
    if (!state.yesSelected) {
        comp.fillRect(noX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(noX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kNo, static_cast<float>(noX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Yes choice.
    const int yesX = static_cast<int>(startX + choiceWidth + gap);
    if (state.yesSelected) {
        comp.fillRect(yesX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(yesX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kYes, static_cast<float>(yesX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Quit App modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawQuitAppContent(const ModalRenderState& state,
                                       ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::QuitApp);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title (Bold).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.title.c_str(),
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator.
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Message (Regular).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message.c_str(),
                  contentX, panel.y + margin + 90.0f + baselineY,
                  UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);

    // No/Yes choices.
    drawQuitAppChoices(state, comp);
}

void ModalRenderer::drawQuitAppChoices(const ModalRenderState& state,
                                       ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::QuitApp);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = choiceWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);

    // No choice.
    const int noX = static_cast<int>(startX);
    const int noY = static_cast<int>(y);
    if (!state.yesSelected) {
        comp.fillRect(noX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(noX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kNo, static_cast<float>(noX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Yes choice.
    const int yesX = static_cast<int>(startX + choiceWidth + gap);
    if (state.yesSelected) {
        comp.fillRect(yesX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(yesX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kYes, static_cast<float>(yesX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Reset Moonlight modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawResetMoonlightContent(const ModalRenderState& state,
                                              ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::ResetMoonlight);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title (Bold).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.title.c_str(),
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator.
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Message line 1 (Regular, secondary).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message.c_str(),
                  contentX, panel.y + margin + 90.0f + baselineY,
                  UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);

    // Message line 2 (Regular, secondary).
    if (!state.message2.empty()) {
        comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message2.c_str(),
                      contentX, panel.y + margin + 130.0f + baselineY,
                      UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);
    }

    // No/Yes choices.
    drawResetMoonlightChoices(state, comp);
}

void ModalRenderer::drawResetMoonlightChoices(const ModalRenderState& state,
                                              ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(ModalKind::ResetMoonlight);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = choiceWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);

    // No choice.
    const int noX = static_cast<int>(startX);
    const int noY = static_cast<int>(y);
    if (!state.yesSelected) {
        comp.fillRect(noX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(noX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kNo, static_cast<float>(noX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Yes choice.
    const int yesX = static_cast<int>(startX + choiceWidth + gap);
    if (state.yesSelected) {
        comp.fillRect(yesX, noY, static_cast<int>(choiceWidth),
                      static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(yesX, noY, static_cast<int>(choiceWidth),
                        static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kYes, static_cast<float>(yesX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

} // namespace ui

// Copyright (C) 2026 fourletterworkshop.com
// Copyright (C) 2026 natko (github.com/natkokrasevac)
// This file is part of Moonlight MP for PlayStation 4.
// Moonlight MP for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight MP for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight MP for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
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

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void ModalRenderer::renderFull(const ModalRenderState& state,
                               ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // Centered modal panel (opaque/semi-opaque). No full-screen dim layer is
    // applied (removed for the GPU UI path; modals draw directly over the base
    // screen, consistent with PS UI presentation).
    const ps4::Rect panel = panelBounds(state.kind);
    comp.fillRect(panel.x, panel.y, panel.w, panel.h,
                  UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 250);
    comp.strokeRect(panel.x, panel.y, panel.w, panel.h, 2,
                    UiTheme::kPanelBorderR, UiTheme::kPanelBorderG, UiTheme::kPanelBorderB, 255);

    // Modal content.
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
    case ModalKind::UpdateAvailable:
        drawUpdateAvailableContent(state, comp);
        break;
    case ModalKind::UpdateProgress:
        drawUpdateProgressContent(state, comp);
        break;
    case ModalKind::UpdateResult:
        drawUpdateResultContent(state, comp);
        break;
    default:
        drawQuitAppContent(state, comp);
        break;
    }
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

// ---------------------------------------------------------------------------
// Update Available modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawUpdateAvailableContent(const ModalRenderState& state,
                                               ps4::UiCompositor& comp) {
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateAvailable);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title (Bold)
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold, baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.title.c_str(),
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Message line 1 (Regular)
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message.c_str(),
                  contentX, panel.y + margin + 90.0f + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Message line 2 (Secondary / details)
    if (!state.message2.empty()) {
        m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::RegularSmall, baselineY, lineHeight);
        comp.drawText(ps4::FontRasterizer::FontFace::RegularSmall, state.message2.c_str(),
                      contentX, panel.y + margin + 140.0f + baselineY,
                      UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);
    }

    drawUpdateAvailableChoices(state, comp);
}

void ModalRenderer::drawUpdateAvailableChoices(const ModalRenderState& state,
                                               ps4::UiCompositor& comp) {
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateAvailable);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = choiceWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);

    // Later choice (left)
    const int laterX = static_cast<int>(startX);
    const int laterY = static_cast<int>(y);
    if (!state.yesSelected) {
        comp.fillRect(laterX, laterY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(laterX, laterY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kUpdateLater, static_cast<float>(laterX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Update choice (right)
    const int updateX = static_cast<int>(startX + choiceWidth + gap);
    if (state.yesSelected) {
        comp.fillRect(updateX, laterY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(updateX, laterY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kUpdateNow, static_cast<float>(updateX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Update Progress modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawUpdateProgressContent(const ModalRenderState& state,
                                              ps4::UiCompositor& comp) {
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateProgress);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title (Bold)
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold, baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.title.c_str(),
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // File name / Message (Regular)
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message.c_str(),
                  contentX, panel.y + margin + 80.0f + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Progress bar track
    const int barX = panel.x + 32;
    const int barY = panel.y + 165;
    const int barW = panel.w - 64;
    const int barH = 22;

    comp.fillRect(barX, barY, barW, barH, 35, 38, 48, 255);
    comp.strokeRect(barX, barY, barW, barH, 1, 70, 75, 90, 255);

    // Progress bar fill
    float clampedProgress = state.progress;
    if (clampedProgress < 0.0f) clampedProgress = 0.0f;
    if (clampedProgress > 1.0f) clampedProgress = 1.0f;
    int fillW = static_cast<int>(static_cast<float>(barW) * clampedProgress);
    if (fillW > 0) {
        comp.fillRect(barX, barY, fillW, barH,
                      UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }

    // Progress text (e.g. "4.8 MB / 9.5 MB (50%)")
    if (!state.progressText.empty()) {
        m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::RegularSmall, baselineY, lineHeight);
        comp.drawText(ps4::FontRasterizer::FontFace::RegularSmall, state.progressText.c_str(),
                      contentX, static_cast<float>(barY + barH + 12) + baselineY,
                      UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);
    }

    drawUpdateProgressChoices(state, comp);
}

void ModalRenderer::drawUpdateProgressChoices(const ModalRenderState& state,
                                              ps4::UiCompositor& comp) {
    (void)state;
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateProgress);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float startX = panel.x + (panel.w - choiceWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);

    const int btnX = static_cast<int>(startX);
    const int btnY = static_cast<int>(y);
    comp.fillRect(btnX, btnY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight),
                  UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
    comp.strokeRect(btnX, btnY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight), 2,
                    UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);

    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kCancel, static_cast<float>(btnX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Update Result modal content
// ---------------------------------------------------------------------------

void ModalRenderer::drawUpdateResultContent(const ModalRenderState& state,
                                            ps4::UiCompositor& comp) {
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateResult);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title (Bold)
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold, baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, state.title.c_str(),
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Message (Regular) - handles multiline \n
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);
    float currentY = panel.y + margin + 90.0f;

    std::string line;
    for (size_t i = 0; i <= state.message.size(); ++i) {
        if (i == state.message.size() || state.message[i] == '\n') {
            if (!line.empty()) {
                comp.drawText(ps4::FontRasterizer::FontFace::Regular, line.c_str(),
                              contentX, currentY + baselineY,
                              UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
                currentY += lineHeight + 8.0f;
                line.clear();
            }
        } else {
            line += state.message[i];
        }
    }

    drawUpdateResultChoices(state, comp);
}

void ModalRenderer::drawUpdateResultChoices(const ModalRenderState& state,
                                            ps4::UiCompositor& comp) {
    (void)state;
    if (!m_fonts) return;

    const ps4::Rect panel = panelBounds(ModalKind::UpdateResult);
    const float margin = 32.0f;
    const float choiceWidth = 160.0f;
    const float choiceHeight = 56.0f;
    const float startX = panel.x + (panel.w - choiceWidth) * 0.5f;
    const float y = panel.y + panel.h - choiceHeight - margin;

    float baselineY = 0.0f, lineHeight = 0.0f;
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular, baselineY, lineHeight);

    const int btnX = static_cast<int>(startX);
    const int btnY = static_cast<int>(y);
    comp.fillRect(btnX, btnY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight),
                  UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
    comp.strokeRect(btnX, btnY, static_cast<int>(choiceWidth), static_cast<int>(choiceHeight), 2,
                    UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);

    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kOk, static_cast<float>(btnX), choiceWidth,
                     y + (choiceHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

} // namespace ui

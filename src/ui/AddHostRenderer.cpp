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

// === AddHostRenderer.cpp ====================================================
//
// OpenOrbis Moonlight client — CPU Add Host overlay / status renderer.
//
// Implements the AddHostRenderer: draws the Add Host overlay (form) and the
// Add Host status modal (Checking / Found / Failed) into the shared CPU
// UiCompositor surface using only fillRect / strokeRect / drawText / clipping.
// It consumes a plain AddHostRenderState snapshot and owns the Add Host layout
// geometry.
//
// This translation unit contains no OpenOrbis SDK calls.
// =============================================================================

#include "AddHostRenderer.h"

#include "platform/ps4/FontRasterizer.h"
#include "UiTheme.h"
#include "UiStrings.h"
#include "UiRenderPrimitives.h"

namespace ui {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AddHostRenderer::AddHostRenderer() = default;

AddHostRenderer::~AddHostRenderer() {
    if (m_initialized) {
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// Initialization / Shutdown
// ---------------------------------------------------------------------------

bool AddHostRenderer::initialize(ps4::FontRasterizer* fonts) {
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

void AddHostRenderer::shutdown() {
    m_fonts = nullptr;
    m_initialized = false;
}

bool AddHostRenderer::isInitialized() const {
    return m_initialized;
}

// ---------------------------------------------------------------------------
// Layout geometry
// ---------------------------------------------------------------------------

ps4::Rect AddHostRenderer::dimBounds() const {
    return ps4::Rect{0, 0, 1920, 1080};
}

ps4::Rect AddHostRenderer::panelBounds(const AddHostRenderState& state) const {
    if (state.state == AddHostPresentationState::Form) {
        // 760x460 centered.
        return ps4::Rect{(1920 - 760) / 2, (1080 - 460) / 2, 760, 460};
    }
    // Status modal: 720x320 centered.
    return ps4::Rect{(1920 - 720) / 2, (1080 - 320) / 2, 720, 320};
}

ps4::Rect AddHostRenderer::bounds(const AddHostRenderState& state) const {
    // The overlay includes a full-screen dim layer, so the visual bounds are
    // the full screen. This is the conservative dirty region for state
    // changes (open/close/state transition).
    return dimBounds();
}

// --- Form overlay layout bounds ---------------------------------------------

ps4::Rect AddHostRenderer::ipRowBounds() const {
    // Form panel is 760x460 centered; IP row starts at margin+70 with height
    // rowHeight (56). Matches drawFormOverlay.
    AddHostRenderState formState;
    formState.state = AddHostPresentationState::Form;
    const ps4::Rect panel = panelBounds(formState);
    const float margin = 32.0f;
    const float rowHeight = 56.0f;
    const float ipRowY = panel.y + margin + 70.0f;
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(ipRowY) - pad,
                     panel.w + pad * 2, static_cast<int>(rowHeight) + pad * 2};
}

ps4::Rect AddHostRenderer::portRowBounds() const {
    AddHostRenderState formState;
    formState.state = AddHostPresentationState::Form;
    const ps4::Rect panel = panelBounds(formState);
    const float margin = 32.0f;
    const float rowHeight = 56.0f;
    const float ipRowY = panel.y + margin + 70.0f;
    const float portRowY = ipRowY + rowHeight + 16.0f;
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(portRowY) - pad,
                     panel.w + pad * 2, static_cast<int>(rowHeight) + pad * 2};
}

ps4::Rect AddHostRenderer::validationBounds() const {
    // The validation-error region spans from just below the Port row down to
    // the validation-error line. It is the region that can change when a
    // validation error appears/disappears. Matches drawFormOverlay's layout.
    AddHostRenderState formState;
    formState.state = AddHostPresentationState::Form;
    const ps4::Rect panel = panelBounds(formState);
    const float margin = 32.0f;
    const float rowHeight = 56.0f;
    const float ipRowY = panel.y + margin + 70.0f;
    const float portRowY = ipRowY + rowHeight + 16.0f;
    const float nextY = portRowY + rowHeight + 16.0f;
    // The region covers the validation-error line.
    const float lineHeight = 24.0f;   // approximate Regular line height.
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(nextY) - pad,
                     panel.w + pad * 2,
                     static_cast<int>(lineHeight) + pad * 2};
}

ps4::Rect AddHostRenderer::buttonsBounds() const {
    AddHostRenderState formState;
    formState.state = AddHostPresentationState::Form;
    const ps4::Rect panel = panelBounds(formState);
    const float margin = 32.0f;
    const float buttonHeight = 56.0f;
    const float buttonsY = panel.y + panel.h - buttonHeight - margin;
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(buttonsY) - pad,
                     panel.w + pad * 2, static_cast<int>(buttonHeight) + pad * 2};
}

// --- Status modal layout bounds ---------------------------------------------

ps4::Rect AddHostRenderer::statusMessageBounds() const {
    // Status modal panel is 720x320 centered; message starts at margin+90.
    AddHostRenderState statusState;
    statusState.state = AddHostPresentationState::Checking;   // any non-Form state.
    const ps4::Rect panel = panelBounds(statusState);
    const float margin = 32.0f;
    const float lineHeight = 24.0f;   // approximate Regular line height.
    const float msgY = panel.y + margin + 90.0f;
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(msgY) - pad,
                     panel.w + pad * 2, static_cast<int>(lineHeight) + pad * 2};
}

ps4::Rect AddHostRenderer::statusButtonBounds() const {
    AddHostRenderState statusState;
    statusState.state = AddHostPresentationState::Checking;   // any non-Form state.
    const ps4::Rect panel = panelBounds(statusState);
    const float margin = 32.0f;
    const float buttonHeight = 56.0f;
    const float y = panel.y + panel.h - buttonHeight - margin;
    const int pad = 3;
    return ps4::Rect{panel.x - pad, static_cast<int>(y) - pad,
                     panel.w + pad * 2, static_cast<int>(buttonHeight) + pad * 2};
}

bool AddHostRenderer::rectsIntersect(const ps4::Rect& a, const ps4::Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void AddHostRenderer::renderFull(const AddHostRenderState& state,
                                 ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }

    // 1. Full-screen dim layer (translucent black). The base Host List is
    //    already present in the surface; the dim is composited over it.
    comp.fillRect(0, 0, 1920, 1080, 0, 0, 0, UiTheme::kDimAlpha);   // ~0.55 alpha

    // 2. Centered panel (opaque/semi-opaque).
    const ps4::Rect panel = panelBounds(state);
    comp.fillRect(panel.x, panel.y, panel.w, panel.h,
                  UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 250);
    comp.strokeRect(panel.x, panel.y, panel.w, panel.h, 2,
                    UiTheme::kPanelBorderR, UiTheme::kPanelBorderG, UiTheme::kPanelBorderB, 255);

    // 3. Panel content.
    if (state.state == AddHostPresentationState::Form) {
        drawFormOverlay(state, comp);
    } else {
        drawStatusModal(state, comp);
    }
}

void AddHostRenderer::renderRegion(const AddHostRenderState& state,
                                   ps4::UiCompositor& comp,
                                   const ps4::Rect& dirty) {
    if (!m_fonts) {
        return;
    }
    if (!comp.pushClip(dirty)) {
        return;   // Empty clip; nothing to recompose.
    }

    // Repaint the panel background intersecting the dirty region. All Add Host
    // content lives inside the opaque panel, so the panel background is the
    // base for content recomposition (no dim handling needed inside the opaque
    // panel).
    const ps4::Rect panel = panelBounds(state);
    if (rectsIntersect(dirty, panel)) {
        comp.fillRect(panel.x, panel.y, panel.w, panel.h,
                      UiTheme::kPanelFillR, UiTheme::kPanelFillG, UiTheme::kPanelFillB, 250);
        comp.strokeRect(panel.x, panel.y, panel.w, panel.h, 2,
                        UiTheme::kPanelBorderR, UiTheme::kPanelBorderG, UiTheme::kPanelBorderB, 255);
    }

    // Redraw the panel content. The clip restricts all drawing to the dirty
    // region, so only the affected content is repainted. The full content draw
    // is used (rather than per-row draws) to keep the geometry single-sourced
    // in drawFormOverlay / drawStatusModal.
    if (state.state == AddHostPresentationState::Form) {
        drawFormOverlay(state, comp);
    } else {
        drawStatusModal(state, comp);
    }

    comp.popClip();
}

// ---------------------------------------------------------------------------
// Add Host form overlay
// ---------------------------------------------------------------------------

void AddHostRenderer::drawFormOverlay(const AddHostRenderState& state,
                                      ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(state);
    const float margin = 32.0f;
    const float rowHeight = 56.0f;
    const float labelWidth = 180.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title "Add Host" (Bold).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kAddHost,
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator under the title.
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // --- IP Address row -----------------------------------------------------
    const float ipRowY = panel.y + margin + 70.0f;
    const bool ipSelected = (state.selectedField == 0);
    if (ipSelected) {
        comp.fillRect(panel.x, static_cast<int>(ipRowY), panel.w,
                      static_cast<int>(rowHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(panel.x, static_cast<int>(ipRowY), panel.w,
                        static_cast<int>(rowHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    const float rowBaseline = ipRowY + (rowHeight - lineHeight) * 0.5f + baselineY;
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, "IP Address",
                  contentX, rowBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    const char* ipValue = state.ipText.empty() ? "(empty)" : state.ipText.c_str();
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, ipValue,
                  contentX + labelWidth, rowBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // --- Port row -----------------------------------------------------------
    const float portRowY = ipRowY + rowHeight + 16.0f;
    const bool portSelected = (state.selectedField == 1);
    if (portSelected) {
        comp.fillRect(panel.x, static_cast<int>(portRowY), panel.w,
                      static_cast<int>(rowHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(panel.x, static_cast<int>(portRowY), panel.w,
                        static_cast<int>(rowHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    const float portBaseline = portRowY + (rowHeight - lineHeight) * 0.5f + baselineY;
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, "Port",
                  contentX, portBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
    const char* portValue = state.portText.empty() ? "(empty)" : state.portText.c_str();
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, portValue,
                  contentX + labelWidth, portBaseline, UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // --- Inline validation error --------------------------------------------
    float nextY = portRowY + rowHeight + 16.0f;
    if (!state.validationError.empty()) {
        comp.drawText(ps4::FontRasterizer::FontFace::Regular,
                      state.validationError.c_str(),
                      contentX, nextY + baselineY, 242, 89, 89);
        nextY += lineHeight + 8.0f;
    }

    // --- Cancel / Add buttons -----------------------------------------------
    const float buttonWidth = 180.0f;
    const float buttonHeight = 56.0f;
    const float gap = 40.0f;
    const float totalWidth = buttonWidth * 2.0f + gap;
    const float startX = panel.x + (panel.w - totalWidth) * 0.5f;
    const float buttonsY = panel.y + panel.h - buttonHeight - margin;

    // Cancel button.
    const bool cancelSelected = (state.selectedField == 2);
    const int cancelX = static_cast<int>(startX);
    const int cancelY = static_cast<int>(buttonsY);
    if (cancelSelected) {
        comp.fillRect(cancelX, cancelY, static_cast<int>(buttonWidth),
                      static_cast<int>(buttonHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(cancelX, cancelY, static_cast<int>(buttonWidth),
                        static_cast<int>(buttonHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kCancel, static_cast<float>(cancelX), buttonWidth,
                     buttonsY + (buttonHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Add button.
    const bool addSelected = (state.selectedField == 3);
    const int addX = static_cast<int>(startX + buttonWidth + gap);
    if (addSelected) {
        comp.fillRect(addX, cancelY, static_cast<int>(buttonWidth),
                      static_cast<int>(buttonHeight),
                      UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
        comp.strokeRect(addX, cancelY, static_cast<int>(buttonWidth),
                        static_cast<int>(buttonHeight), 2,
                        UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    }
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     UiStrings::kAdd, static_cast<float>(addX), buttonWidth,
                     buttonsY + (buttonHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

// ---------------------------------------------------------------------------
// Add Host status modal
// ---------------------------------------------------------------------------

void AddHostRenderer::drawStatusModal(const AddHostRenderState& state,
                                      ps4::UiCompositor& comp) {
    if (!m_fonts) {
        return;
    }
    const ps4::Rect panel = panelBounds(state);
    const float margin = 32.0f;
    const float contentX = static_cast<float>(panel.x) + margin;

    float baselineY = 0.0f, lineHeight = 0.0f;

    // Title "Add Host" (Bold).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Bold,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Bold, UiStrings::kAddHost,
                  contentX, panel.y + margin + baselineY,
                  UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);

    // Separator under the title.
    comp.fillRect(panel.x, static_cast<int>(panel.y + margin + 50.0f),
                  panel.w, 2, UiTheme::kSeparatorR, UiTheme::kSeparatorG, UiTheme::kSeparatorB, 255);

    // Status message (Regular).
    m_fonts->getHorizontalLayout(ps4::FontRasterizer::FontFace::Regular,
                                 baselineY, lineHeight);
    comp.drawText(ps4::FontRasterizer::FontFace::Regular, state.message.c_str(),
                  contentX, panel.y + margin + 90.0f + baselineY,
                  UiTheme::kSecondaryTextR, UiTheme::kSecondaryTextG, UiTheme::kSecondaryTextB);

    // Action button. While Checking the sole action is Cancel (aborts the
    // pending manual host add); in the terminal states it is OK (dismiss).
    const char* actionLabel =
        (state.state == AddHostPresentationState::Checking) ? UiStrings::kCancel
                                                            : UiStrings::kOk;

    const float buttonWidth = 180.0f;
    const float buttonHeight = 56.0f;
    const float startX = panel.x + (panel.w - buttonWidth) * 0.5f;
    const float y = panel.y + panel.h - buttonHeight - margin;

    const int bx = static_cast<int>(startX);
    const int by = static_cast<int>(y);
    comp.fillRect(bx, by, static_cast<int>(buttonWidth),
                  static_cast<int>(buttonHeight),
                  UiTheme::kSelectionR, UiTheme::kSelectionG, UiTheme::kSelectionB, 255);
    comp.strokeRect(bx, by, static_cast<int>(buttonWidth),
                    static_cast<int>(buttonHeight), 2,
                    UiTheme::kAccentR, UiTheme::kAccentG, UiTheme::kAccentB, 255);
    drawCenteredText(comp, *m_fonts, ps4::FontRasterizer::FontFace::Regular,
                     actionLabel, static_cast<float>(bx), buttonWidth,
                     y + (buttonHeight - lineHeight) * 0.5f + baselineY,
                     UiTheme::kTextR, UiTheme::kTextG, UiTheme::kTextB);
}

} // namespace ui

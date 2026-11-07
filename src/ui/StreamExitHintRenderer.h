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

// === StreamExitHintRenderer.h ==============================================
//
// Moonlight client for PlayStation 4 — CPU in-stream exit hint renderer.
//
// StreamExitHintRenderer draws the short-lived in-stream exit hint overlay
// (a rounded dark panel with button glyphs and exit text) into the shared CPU
// UiCompositor surface. It consumes a plain StreamExitHintRenderState snapshot
// produced by UiManager and owns the hint layout geometry and the retained
// CPU-decoded UI atlas.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_STREAM_EXIT_HINT_RENDERER_H
#define UI_STREAM_EXIT_HINT_RENDERER_H

#include <cstdint>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// StreamExitHintRenderState (CPU exit hint render snapshot)
// ---------------------------------------------------------------------------

/// Render snapshot for the in-stream exit hint overlay. A plain value copy
/// consumed by StreamExitHintRenderer. UiManager produces it; it is not an
/// authoritative store.
struct StreamExitHintRenderState {
    bool visible = false;        ///< Whether the hint is currently shown.
    bool crossIsAccept = true;   ///< System Enter Button Assignment.
};

// ---------------------------------------------------------------------------
// StreamExitHintRenderer
// ---------------------------------------------------------------------------

/// CPU in-stream exit hint renderer. Draws the exit hint overlay into the
/// shared CPU UiCompositor surface.
class StreamExitHintRenderer {
public:
    StreamExitHintRenderer();
    ~StreamExitHintRenderer();
    StreamExitHintRenderer(const StreamExitHintRenderer&) = delete;
    StreamExitHintRenderer& operator=(const StreamExitHintRenderer&) = delete;
    StreamExitHintRenderer(StreamExitHintRenderer&&) = delete;
    StreamExitHintRenderer& operator=(StreamExitHintRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect bounds() const;
    void render(const StreamExitHintRenderState& state, ps4::UiCompositor& comp);

    const ps4::CpuImage& atlasImage() const { return m_atlasImg; }

private:
    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuImage m_atlasImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_STREAM_EXIT_HINT_RENDERER_H

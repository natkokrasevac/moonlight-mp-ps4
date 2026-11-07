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

// === StreamStatisticsRenderer.h ============================================
//
// Moonlight client for PlayStation 4 — CPU in-stream statistics overlay
// renderer.
//
// StreamStatisticsRenderer draws the in-stream statistics overlay (a
// semi-transparent dark panel with a vertical list of statistics rows) into
// the shared CPU UiCompositor surface. It consumes a plain
// StreamStatisticsRenderState snapshot produced by UiManager and owns the
// statistics panel layout geometry.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
// =============================================================================

#ifndef UI_STREAM_STATISTICS_RENDERER_H
#define UI_STREAM_STATISTICS_RENDERER_H

#include <cstdint>
#include <string>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::Rect

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// StreamStatisticsRenderState (CPU statistics overlay render snapshot)
// ---------------------------------------------------------------------------

/// Render snapshot for the in-stream statistics overlay. A plain value copy
/// consumed by StreamStatisticsRenderer. UiManager produces it; it is not an
/// authoritative store.
struct StreamStatisticsRenderState {
    bool visible = false;        ///< Whether the statistics overlay is shown.

    /// Pre-formatted display strings. Empty = unavailable (renders "N/A").
    std::string resolution;      ///< e.g. "1920x1080".
    std::string fps;             ///< e.g. "60.0".
    std::string bitrate;         ///< e.g. "12.5".
    std::string codec;           ///< e.g. "AVC (H.264)" / "HEVC (H.265)".
    std::string decodeTime;      ///< e.g. "3.2".
    std::string networkLatency;  ///< e.g. "8.0".
    std::string jitter;          ///< e.g. "1.5".
    std::string frameDecodeTime; ///< Frame Decoding, e.g. "3.2 ms".
    std::string framePresentTime;///< Frame Presentation, e.g. "1.8 ms".
    std::string frameTotalTime;  ///< Time per Frame, e.g. "5.0 ms".
};

// ---------------------------------------------------------------------------
// StreamStatisticsRenderer
// ---------------------------------------------------------------------------

/// CPU in-stream statistics overlay renderer. Draws the statistics overlay
/// into the shared CPU UiCompositor surface.
class StreamStatisticsRenderer {
public:
    StreamStatisticsRenderer();
    ~StreamStatisticsRenderer();
    StreamStatisticsRenderer(const StreamStatisticsRenderer&) = delete;
    StreamStatisticsRenderer& operator=(const StreamStatisticsRenderer&) = delete;
    StreamStatisticsRenderer(StreamStatisticsRenderer&&) = delete;
    StreamStatisticsRenderer& operator=(StreamStatisticsRenderer&&) = delete;

    bool initialize(ps4::FontRasterizer* fonts);
    void shutdown();
    bool isInitialized() const;

    ps4::Rect bounds() const;
    ps4::Rect rowBounds(int rowIndex) const;

    void render(const StreamStatisticsRenderState& state, ps4::UiCompositor& comp);

private:
    float panelX() const { return 64.0f; }
    float panelY() const { return 88.0f; }
    float panelW() const { return 300.0f; }
    float rowHeight() const { return 40.0f; }
    float padding() const { return 20.0f; }
    float contentX() const { return panelX(); }

    float rowTop(int rowIndex) const;
    void drawRow(const char* label, const std::string& value, int rowIndex,
                 float rowY, ps4::UiCompositor& comp);

    ps4::FontRasterizer* m_fonts = nullptr;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_STREAM_STATISTICS_RENDERER_H


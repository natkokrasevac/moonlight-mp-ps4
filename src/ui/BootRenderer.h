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

// === BootRenderer.h =========================================================
//
// Moonlight client for PlayStation 4 — CPU Boot screen renderer.
//
// This is a narrowly-scoped renderer on the `exp_cpu_rasterization` branch.
// It consumes a plain `BootRenderState` snapshot produced by UiManager and
// draws the Boot screen (background + Moonlight logo + title + optional
// status text) into the shared CPU UiCompositor surface, which is then
// presented through the UiBlitter. It is deliberately NOT a generic
// widget/scene-graph/layout framework.
//
// Ownership model:
//   UiManager     : boot state, elapsed time, transition to Host List, any
//                   startup status strings.
//   BootRenderer  : presentation only (layout + drawing into UiCompositor).
//
// The Boot screen is static: it is composed once on entry and not recomposed
// while waiting. The renderer owns the CPU-decoded logo image and the layout
// constants; it does not own timers, boot state, or application lifecycle.
//
// This header exposes only project-owned types. No OpenOrbis SDK types appear here.
//
// OpenOrbis adaptation: `BootRenderState` is now the authoritative production
// definition from UiManager.h. The temporary local definition has been
// removed. The retained logo/background images are held as owning
// ps4::CpuPngImage (the OpenOrbis CpuImage does not own its buffer).
// =============================================================================

#ifndef UI_BOOT_RENDERER_H
#define UI_BOOT_RENDERER_H

#include <cstdint>
#include <string>

#include "platform/ps4/UiCompositor.h"   // ps4::UiCompositor, ps4::CpuImage, ps4::Rect
#include "platform/ps4/PngCpuDecode.h"   // ps4::CpuPngImage
#include "UiManager.h"                   // ui::BootRenderState (authoritative production definition)

namespace ps4 {
class FontRasterizer;
}

namespace ui {

// ---------------------------------------------------------------------------
// BootRenderer
// ---------------------------------------------------------------------------

/// CPU Boot screen renderer. Owns the Boot layout geometry and the retained
/// CPU-decoded logo image (moonlight_boot.png). Draws the Boot screen into a
/// UiCompositor surface using only fillRect / drawImage / drawText.
///
/// This class is main-thread owned. It contains no OpenOrbis SDK calls.
class BootRenderer {
public:
    /// Constructor performs no fallible operations.
    BootRenderer();

    /// Destructor acts as a final safety net; orderly shutdown should be
    /// performed by calling shutdown() explicitly.
    ~BootRenderer();

    // Non-copyable, non-movable.
    BootRenderer(const BootRenderer&) = delete;
    BootRenderer& operator=(const BootRenderer&) = delete;
    BootRenderer(BootRenderer&&) = delete;
    BootRenderer& operator=(BootRenderer&&) = delete;

    /// Decode the retained CPU logo image (moonlight_boot.png) once and retain
    /// the FontRasterizer reference for text drawing. Returns true on success.
    /// On failure the object remains safe to shut down or destroy.
    bool initialize(ps4::FontRasterizer* fonts);

    /// Release retained resources. Safe after partial initialization, normal
    /// completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Layout / visual bounds helpers ------------------------------------
    // These are the single source of Boot visual-bounds arithmetic, so
    // dirty-region invalidation and rendering never disagree.

    /// Screen-space bounds of the full Boot screen (the full framebuffer).
    ps4::Rect bounds() const;

    // --- Rendering ----------------------------------------------------------

    /// Full-compose the Boot screen (background + logo + title + optional
    /// status text) for the given state into `comp`. The Boot screen is
    /// static, so this is called once on entry and not recomposed while
    /// waiting.
    void render(const BootRenderState& state, ps4::UiCompositor& comp);

    // --- Retained resources -------------------------------------------------

    /// The CPU-decoded Moonlight logo (moonlight_boot.png). Empty if
    /// unavailable.
    const ps4::CpuPngImage& logoImage() const { return m_logoImg; }

private:
    // --- Layout geometry ----------------------------------------------------
    /// The logo destination rectangle (native size, centered on screen).
    /// Derived from the decoded logo's actual dimensions and the framebuffer.
    ps4::Rect logoRect() const;

    // --- State --------------------------------------------------------------
    ps4::FontRasterizer* m_fonts = nullptr;
    ps4::CpuPngImage m_logoImg;
    ps4::CpuPngImage m_bgImg;
    bool m_initialized = false;
};

} // namespace ui

#endif // UI_BOOT_RENDERER_H

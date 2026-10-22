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

// === GpuUiRenderer.h ========================================================
//
// OpenOrbis PS4 — GPU fullscreen UI compositor.
//
// GpuUiRenderer owns the OpenGNM graphics resources needed to present the CPU
// UI compositor surface (a 1920x1080 RGBA8 buffer) into the current VideoOut
// framebuffer entirely on the GPU, using a fullscreen VS+PS draw.
//
// This replaces the CPU UiBlitter (which memcpy'd dirty regions into the
// framebuffer). The GPU writes the framebuffer via a render target, which is
// required for TILING_MODE_TILE framebuffers (used for 4K output on retail
// PS4 Pro hardware, where TILING_MODE_LINEAR is not permitted for Over 2K
// resolutions).
//
// Responsibilities:
//   - Load the fullscreen VS and RGBA-texture-sampling PS shader binaries
//     (reuses the overlay_vv.sb / overlay_p.sb shaders packaged under
//     /app0/assets/misc/).
//   - Own a WC_GARLIC staging buffer for the 1080p UI texture.
//   - stageUi(): copy the CPU UI compositor surface into the WC_GARLIC
//     staging buffer.
//   - drawUi(): draw a fullscreen triangle sampling the staged UI texture
//     into the current framebuffer. The overlay_vv.pssl UVs map the texture
//     across the viewport, so scaling (1x at 1080p, 2x at 4K) is automatic.
//   - Synchronize GPU completion (EOP) before returning.
//
// This module is deliberately narrow and standalone. It does NOT share
// infrastructure with GpuVideoRenderer (which handles the NV12 video path).
// It reuses the exact state/setup patterns from GpuVideoRenderer and the
// working OpenGNM triangle smoke test.
//
// OpenOrbis SDK types are confined to the implementation file.
// ===============================================================================

#ifndef PS4_GPU_UI_RENDERER_H
#define PS4_GPU_UI_RENDERER_H

#include <cstdint>
#include <cstddef>

namespace ps4 {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of GPU UI renderer operations.
enum class GpuUiResultCategory : uint8_t {
    Success,
    NotInitialized,
    ShaderLoadFailure,
    MemoryAllocationFailure,
    RenderTargetFailure,
    SubmissionFailure,
    SynchronizationFailure,
    InvalidSurface
};

/// A lightweight result returned by GPU UI renderer operations.
struct GpuUiResult {
    GpuUiResultCategory category;
    uint32_t nativeError;   ///< OpenOrbis SDK error code (0 when none).
    GpuUiResult() : category(GpuUiResultCategory::Success), nativeError(0) {}
    GpuUiResult(GpuUiResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == GpuUiResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// GpuUiRenderer
// ---------------------------------------------------------------------------

/// Owns the GPU fullscreen UI compositor.
///
/// Main-thread owned. Initialized once during application startup (after
/// GfxManager) and released at shutdown. It is a narrow, self-contained GPU
/// render path that draws the CPU UI compositor surface into the current
/// GfxManager framebuffer slot.
class GpuUiRenderer {
public:
    /// Constructor performs no fallible operations.
    GpuUiRenderer();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~GpuUiRenderer();

    // Non-copyable, non-movable.
    GpuUiRenderer(const GpuUiRenderer&) = delete;
    GpuUiRenderer& operator=(const GpuUiRenderer&) = delete;
    GpuUiRenderer(GpuUiRenderer&&) = delete;
    GpuUiRenderer& operator=(GpuUiRenderer&&) = delete;

    /// Initialize the GPU UI renderer. Loads the overlay shader binaries from
    /// `/app0/assets/misc/` and allocates the OpenGNM command buffer, EOP
    /// label, and WC_GARLIC UI staging buffer. `fbWidth`/`fbHeight` are the
    /// framebuffer dimensions (1920x1080 or 3840x2160); the render target is
    /// created to cover the full framebuffer. Must be called before
    /// stageUi()/drawUi(). After failure the object remains safe to shut down
    /// or destroy.
    GpuUiResult initialize(uint32_t fbWidth, uint32_t fbHeight);

    /// Release all GPU UI renderer resources. Idempotent.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Copy the CPU UI compositor surface into the renderer's WC_GARLIC
    /// staging buffer and flush it, making it GPU-visible. `rgba` is a full
    /// `w`x`h` RGBA8 surface (byte order R,G,B,A), `pitch` its row stride in
    /// bytes (>= w*4). The staging buffer is 1920x1080; `w`/`h` must not
    /// exceed it. Returns true on success.
    GpuUiResult stageUi(const uint8_t* rgba, uint32_t w, uint32_t h,
                        uint32_t pitch);

    /// Draw the staged UI texture fullscreen into the given framebuffer.
    ///
    /// `framebuffer` is the current VideoOut slot's WC_GARLIC base address
    /// (the same address GfxManager::currentFramebuffer() returns). `fbWidth`/
    /// `fbHeight` are the framebuffer dimensions (1920x1080 or 3840x2160).
    ///
    /// This binds the staged UI texture as a linear texture, builds and
    /// submits a fullscreen VS+PS draw command buffer into the framebuffer,
    /// and waits for the EOP so the GPU draw is fully complete before
    /// returning. The overlay_vv.pssl UVs map the texture across the viewport,
    /// so the UI is scaled to fill the framebuffer (1x at 1080p, 2x at 4K).
    ///
    /// Returns true on success. On failure the framebuffer contents are
    /// undefined; the caller should fall back to clearing to black.
    GpuUiResult drawUi(void* framebuffer, uint32_t fbWidth, uint32_t fbHeight);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_GPU_UI_RENDERER_H

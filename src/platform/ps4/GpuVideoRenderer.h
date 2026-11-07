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

// === GpuVideoRenderer.h =====================================================
//
// OpenOrbis PS4 — GPU NV12→BGRA fullscreen video pass.
//
// GpuVideoRenderer owns the OpenGNM graphics resources needed to convert a
// decoded Videodec2 NV12 frame into the existing WC_GARLIC VideoOut
// framebuffer entirely on the GPU, using a fullscreen VS+PS draw.
//
// Responsibilities:
//   - Load the fullscreen VS and NV12→BGRA PS shader binaries (official SDK
//     .sb files packaged under /app0/assets/misc/).
//   - Own the OpenGNM command buffer, EOP sync label, and constant buffer.
//   - Bind the decoded NV12 Y/UV planes as GPU-readable linear textures.
//   - Draw a fullscreen triangle into the current TILE WC_GARLIC VideoOut
//     render target (the framebuffer is registered with VideoOut as
//     TILING_MODE_TILE), performing BT.709 NV12→BGRA in the pixel shader.
//   - Synchronize GPU completion (EOP) before returning, so the caller can
//     safely run the GPU UI overlay composition over the same framebuffer.
//
// This module is deliberately narrow. It does NOT redesign GfxManager, does
// NOT create a general graphics abstraction layer, does NOT use compute
// shaders, and does NOT change the validated double-buffered VideoOut
// ownership model. It reuses the exact state/setup patterns from the working
// OpenGNM triangle smoke test (gpu_triangle_smoke.c).
//
// The UI path is untouched: the GPU UI compositor continues to draw into the
// framebuffer after the GPU video pass completes. The GPU video pass must be
// fully complete (EOP reached) before any other GPU draw writes into the same
// framebuffer, which the caller guarantees by waiting for this module's
// renderFrame() to return before composing the UI overlay.
//
// OpenOrbis SDK types are confined to the implementation file.
// ===============================================================================

#ifndef PS4_GPU_VIDEO_RENDERER_H
#define PS4_GPU_VIDEO_RENDERER_H

#include <cstdint>
#include <cstddef>

namespace ps4 {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of GPU video renderer operations.
enum class GpuVideoResultCategory : uint8_t {
    Success,
    NotInitialized,
    ShaderLoadFailure,
    MemoryAllocationFailure,
    RenderTargetFailure,
    SubmissionFailure,
    SynchronizationFailure,
    InvalidFrame
};

/// A lightweight result returned by GPU video renderer operations.
struct GpuVideoResult {
    GpuVideoResultCategory category;
    uint32_t nativeError;   ///< OpenOrbis SDK error code (0 when none).
    GpuVideoResult() : category(GpuVideoResultCategory::Success), nativeError(0) {}
    GpuVideoResult(GpuVideoResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == GpuVideoResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// GpuVideoFrame — decoded NV12 frame description (project-owned)
// ---------------------------------------------------------------------------

/// A minimal, project-owned description of a decoded NV12 frame for the GPU
/// video pass. The renderer binds the Y/UV plane addresses directly as GPU
/// textures (no intermediate NV12 copy). The caller (MoonlightApp) fills this
/// from the ml::DecodedVideoFrame it holds.
struct GpuVideoFrame {
    void*   yPlane = nullptr;        ///< Y plane base address (GPU-readable).
    void*   uvPlane = nullptr;       ///< UV plane base address (GPU-readable).
    uint32_t width = 0;              ///< Visible width (pixels).
    uint32_t height = 0;             ///< Visible height (pixels).
    uint32_t surfaceHeight = 0;      ///< Aligned surface height (pixels).
    uint32_t pitch = 0;              ///< Y plane row pitch (bytes).
    uint32_t colorSpace = 1;         ///< COLORSPACE_REC_709 (1).
    uint32_t colorRange = 0;         ///< COLOR_RANGE_LIMITED (0).
};

// ---------------------------------------------------------------------------
// GpuVideoRenderer
// ---------------------------------------------------------------------------

/// Owns the GPU NV12→BGRA fullscreen video pass.
///
/// Main-thread owned. Initialized once at stream connect (or lazily on first
/// use) and released at stream stop/shutdown. It is a narrow, self-contained
/// GPU render path that draws into the current GfxManager framebuffer slot.
class GpuVideoRenderer {
public:
    /// Constructor performs no fallible operations.
    GpuVideoRenderer();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~GpuVideoRenderer();

    // Non-copyable, non-movable.
    GpuVideoRenderer(const GpuVideoRenderer&) = delete;
    GpuVideoRenderer& operator=(const GpuVideoRenderer&) = delete;
    GpuVideoRenderer(GpuVideoRenderer&&) = delete;
    GpuVideoRenderer& operator=(GpuVideoRenderer&&) = delete;

    /// Initialize the GPU video renderer. Loads the shader binaries from
    /// `/app0/assets/misc/` and allocates the OpenGNM command buffer, EOP
    /// label, and constant buffer. `fbWidth`/`fbHeight` are the framebuffer
    /// (output) dimensions (1920x1080 or 3840x2160); the render target is
    /// created to cover the full framebuffer. The decoded NV12 frame is bound
    /// per-frame at its native (stream) resolution and scaled to fill the
    /// output via the shader UVs (nearest-neighbor), so the same renderer
    /// handles 1080p→1080p, 1080p→4K upscale, and 4K→4K. Must be called before
    /// renderFrame(). After failure the object remains safe to shut down or
    /// destroy.
    GpuVideoResult initialize(uint32_t fbWidth, uint32_t fbHeight);

    /// Release all GPU video renderer resources. Idempotent.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Render one decoded NV12 frame into the given framebuffer using the GPU.
    ///
    /// `framebuffer` is the current VideoOut slot's WC_GARLIC base address
    /// (the same address GfxManager::currentFramebuffer() returns). `fbWidth`/
    /// `fbHeight` are the framebuffer dimensions (1920×1080).
    ///
    /// This binds the frame's Y/UV planes as linear textures, builds and
    /// submits a fullscreen VS+PS draw command buffer into the framebuffer,
    /// and waits for the EOP so the GPU draw is fully complete before
    /// returning. The caller must then run any CPU UI overlay composition over
    /// the same framebuffer (never concurrently with the GPU draw).
    ///
    /// Returns true on success. On failure the framebuffer contents are
    /// undefined; the caller should fall back to clearing to black.
    GpuVideoResult renderFrame(const GpuVideoFrame& frame,
                               void* framebuffer,
                               uint32_t fbWidth, uint32_t fbHeight);

    /// Render one decoded NV12 frame AND GPU alpha-blend an overlay over it in
    /// a SINGLE command-buffer submission.
    ///
    /// This is the flicker-free path: the video draw and the overlay blend are
    /// appended to the same command buffer and submitted together, so the GPU
    /// executes them in program order and the overlay's destination reads are
    /// guaranteed to see the completed video writes (no cross-submission
    /// read-after-write hazard).
    ///
    /// `frame`/`framebuffer`/`fbWidth`/`fbHeight` are as in renderFrame().
    /// `overlayRgba` (optional) is a full `ow`×`oh` RGBA8 surface (byte order
    /// R,G,B,A) transparent except where an in-stream overlay panel is drawn;
    /// `opitch` is its row stride in bytes (>= ow*4). If `overlayRgba` is null
    /// this behaves exactly like renderFrame() (video only). The overlay
    /// surface must be GPU-visible (WC_GARLIC staging, flushed) before calling.
    ///
    /// Returns true on success. On failure the framebuffer contents are
    /// undefined; the caller should fall back to clearing to black.
    GpuVideoResult renderFrameWithOverlay(const GpuVideoFrame& frame,
                                          const void* overlayRgba,
                                          uint32_t ow, uint32_t oh,
                                          uint32_t opitch,
                                          void* framebuffer,
                                          uint32_t fbWidth,
                                          uint32_t fbHeight);

    /// GPU alpha-blend a cached overlay surface over the current framebuffer.
    ///
    /// `overlayRgba` is a full `ow`×`oh` RGBA8 surface (byte order R,G,B,A,
    /// matching the A8B8G8R8_SRGB framebuffer), transparent except where an
    /// in-stream overlay panel is drawn. It is sampled as a texture and
    /// source-over alpha-blended over the video already in the framebuffer by
    /// the GPU blend unit. This replaces the per-frame CPU overlay blend.
    ///
    /// `opitch` is the overlay surface row stride in bytes (>= ow*4).
    /// `framebuffer` is the current VideoOut slot base (the same address
    /// renderFrame() used). `fbWidth`/`fbHeight` are the framebuffer dims.
    ///
    /// The overlay surface is CPU-written Onion memory. To avoid Onion/GPU
    /// cache-coherency issues, the caller should pass a surface that has been
    /// made GPU-visible (e.g. copied into a WC_GARLIC staging buffer and
    /// flushed) before calling. This method draws a fullscreen triangle
    /// sampling the overlay texture and waits for the EOP so the overlay is
    /// fully composited before returning.
    ///
    /// Returns true on success. On failure the framebuffer holds only the
    /// video; the caller should fall back to the CPU overlay blend.
    GpuVideoResult blendOverlay(const void* overlayRgba, uint32_t ow,
                                uint32_t oh, uint32_t opitch,
                                void* framebuffer, uint32_t fbWidth,
                                uint32_t fbHeight);

    /// DIAGNOSTIC (Stage 1): GPU-blend a static semi-transparent test overlay
    /// over the current framebuffer. This isolates the GPU overlay blend
    /// mechanism (texture sampling + source-over blend state + RT compositing)
    /// from the Onion/CPU-overlay coherency question. The test overlay is a
    /// static RGBA8 pattern held in Garlic memory (no CPU writes per frame).
    /// `framebuffer`/`fbWidth`/`fbHeight` are the current VideoOut slot.
    /// Returns true on success.
    GpuVideoResult blendTestOverlay(void* framebuffer, uint32_t fbWidth,
                                    uint32_t fbHeight);

    /// DIAGNOSTIC (Stage 1, flicker-free): render a decoded frame AND blend the
    /// static test overlay over it in a SINGLE command-buffer submission. This
    /// exercises the combined video+overlay path (as renderFrameWithOverlay)
    /// so the overlay's destination reads are guaranteed to see the completed
    /// video writes (no cross-submission read-after-write hazard). The test
    /// overlay is the static RGBA8 pattern held in Garlic memory.
    /// `frame`/`framebuffer`/`fbWidth`/`fbHeight` are as in renderFrame().
    /// Returns true on success.
    GpuVideoResult renderFrameWithTestOverlay(const GpuVideoFrame& frame,
                                              void* framebuffer,
                                              uint32_t fbWidth,
                                              uint32_t fbHeight);

    /// Copy a CPU-written overlay surface into the renderer's persistent
    /// WC_GARLIC staging buffer and flush it, making it GPU-visible. This is
    /// the staging-buffer path for the in-stream overlay: the CPU overlay
    /// compositor (Onion memory) is copied here on change, then sampled by the
    /// GPU blend pass. `rgba` is a full `w`×`h` RGBA8 surface (byte order
    /// R,G,B,A), `pitch` its row stride in bytes (>= w*4). The staging buffer
    /// is 1920×1080; `w`/`h` must not exceed it. Returns true on success.
    GpuVideoResult stageOverlay(const void* rgba, uint32_t w, uint32_t h,
                                uint32_t pitch);

    /// Returns the renderer's persistent overlay staging buffer (WC_GARLIC),
    /// or nullptr if not allocated. Pass this to renderFrameWithOverlay() as
    /// the overlay source after stageOverlay() has populated it.
    void* overlayStaging() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_GPU_VIDEO_RENDERER_H

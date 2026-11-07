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

#ifndef PS4_GFX_MANAGER_H
#define PS4_GFX_MANAGER_H

#include <cstdint>
#include <cstddef>

namespace ps4 {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of graphics-operation results.
enum class GfxResultCategory : uint8_t {
    Success,
    AlreadyInitialized,
    InvalidLifecycleState,
    GraphicsInitFailure,
    VideoOutOpenFailure,
    MemoryAllocationFailure,
    BufferRegistrationFailure,
    SynchronizationFailure,
    SubmissionFailure,
    PresentationFailure
};

/// A lightweight result returned by graphics lifecycle operations.
/// Preserves a native SDK error code for diagnostics without exposing SDK
/// types in the public interface.
struct GfxResult {
    GfxResultCategory category;
    uint32_t          nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    GfxResult() : category(GfxResultCategory::Success), nativeError(0) {}
    GfxResult(GfxResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == GfxResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// GfxManager — platform graphics subsystem
// ---------------------------------------------------------------------------

/// Owns the PS4 graphics and video-output resources required for rendering.
///
/// Responsibilities:
///   - Initialize graphics and video output.
///   - Own all persistent frame resources and graphics memory.
///   - Acquire a writable frame, clear it via a GPU fill, and present it.
///   - Synchronize frame reuse against display usage.
///   - Shut down graphics resources deterministically.
///
/// The framebuffer is a TILING_MODE_TILE (tiled) A8B8G8R8_SRGB surface. The
/// tiled layout is required for >1080p (4K) output on retail PS4 Pro hardware
/// and is used at all resolutions here. Because the memory is tiled, the CPU
/// cannot write pixels directly; all framebuffer writes (clear, UI, video) go
/// through the GPU.
///
/// This class is main-thread owned. No rendering worker thread is created.
class GfxManager {
public:
    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before initialize() is called.
    GfxManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~GfxManager();

    // Non-copyable, non-movable.
    GfxManager(const GfxManager&) = delete;
    GfxManager& operator=(const GfxManager&) = delete;
    GfxManager(GfxManager&&) = delete;
    GfxManager& operator=(GfxManager&&) = delete;

    /// Initialize graphics and video-output resources.
    /// Must be called exactly once, before beginFrame().
    /// After failure the object remains safe to shut down or destroy.
    /// beginFrame() must not be called after failure.
    GfxResult initialize();

    /// Acquire and prepare the next writable frame.
    /// Requires successful initialize(). Only one frame may be active at a
    /// time. Must be followed by clear() and endFrame().
    GfxResult beginFrame();

    /// Clear the active framebuffer to the given color using a GPU fill.
    /// Requires an active frame (after beginFrame()).
    /// The color is a 32-bit RGBA value (0xAABBGGRR in memory, i.e. byte 0 = R).
    ///
    /// The framebuffer is a TILING_MODE_TILE surface, so a CPU memset would not
    /// produce a correct visual clear. This issues a GPU DMA constant fill over
    /// the whole tiled buffer (every pixel word is written with the color) and
    /// waits for the GPU to complete before returning.
    GfxResult clear(uint32_t color);

    /// Finish frame preparation and present the completed frame.
    /// Requires an active frame. Advances the frame index.
    GfxResult endFrame();

    /// Present the completed frame to the display.
    /// Requires an active frame (after endFrame()).
    GfxResult present();

    /// Release all graphics resources in reverse dependency order.
    /// Safe after partial initialization, normal loop completion, or
    /// repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Returns the framebuffer width in pixels.
    uint32_t getFramebufferWidth() const;

    /// Returns the framebuffer height in pixels.
    uint32_t getFramebufferHeight() const;

    /// Returns the current frame index (0 or 1) being prepared by beginFrame().
    /// Valid between beginFrame() and endFrame().
    uint32_t currentFrameIndex() const;

    /// Returns a raw pointer to the current frame's framebuffer (the frame
    /// being prepared by beginFrame()). Valid between beginFrame() and
    /// endFrame(). The memory is a TILING_MODE_TILE A8B8G8R8_SRGB surface; the
    /// returned address is the tiled buffer base, used as the GPU render-target
    /// base by the GPU UI/video renderers. Returns nullptr if no frame is
    /// active or the manager is not initialized.
    ///
    /// This is the project-owned framebuffer access used by the GPU renderers
    /// to draw into the current VideoOut slot. It intentionally does not expose
    /// any OpenOrbis SDK structures (OrbisVideoOut*, direct-memory internals,
    /// or VideoOut handles).
    void* currentFramebuffer();

    /// Returns the row stride in bytes of the framebuffer returned by
    /// currentFramebuffer(). For the linear A8B8G8R8_SRGB layout this equals
    /// width * 4 (tightly packed, no padding).
    ///
    /// NOTE: The framebuffer is now TILING_MODE_TILE, so the CPU does not write
    /// pixels row-by-row and this value is not used by the GPU renderers (they
    /// derive the tiled pitch from the render-target descriptor). It is
    /// retained for the legacy linear-layout accessor contract and returns
    /// width * 4.
    size_t framebufferStrideBytes() const;

    // --- PS4 Pro / 4K capability detection ----------------------------------

    /// Returns true if the console is a PS4 Pro / NEO (sceKernelIsNeoMode()
    /// returned non-zero). This is a static hardware property; it does not
    /// change at runtime. Valid after initialize().
    bool isNeoMode() const;

    /// Returns true if the current VideoOut resolution height exceeds 1080
    /// (i.e., the display is actually running a >1080p output mode). Queried
    /// once during initialize() after VideoOut is opened. Valid after
    /// initialize().
    bool is4KOutput() const;

    /// Returns true if PS4 Pro processing is available AND active:
    ///   - The console is a PS4 Pro (isNeoMode()).
    ///
    /// Independent of the current output resolution — a Pro on a 1080p screen
    /// still returns true (it can do H.265/HEVC decode). Drives H.265/HEVC
    /// decode support and codec settings. Valid after initialize().
    bool isPro() const;

    /// Returns true if 4K output processing is available AND active:
    ///   - The current VideoOut is running a >1080p output (is4KOutput()).
    ///
    /// Drives framebuffer dimensions, UI 2x scaling, and the 4K video
    /// pipeline. Valid after initialize().
    bool is4KEnabled() const;

    /// Opaque pointer to the private implementation state. This keeps all
    /// OpenOrbis SDK types out of the public header. The definition of Impl lives
    /// in GfxManager.cpp; only the opaque forward declaration is exposed here
    /// so that the destructor and internal helpers can reference it.
    struct Impl;

private:
    // --- Lifecycle state ----------------------------------------------------

    enum class LifecycleState : uint8_t {
        Constructed,
        Initializing,
        Initialized,
        FrameActive,
        Stopping,
        ShutDown
    };

    LifecycleState m_state;

    // --- Frame state --------------------------------------------------------

    uint32_t m_frameIndex;   ///< Current frame index (0 or 1).
    bool     m_frameActive;  ///< Whether a frame is currently being prepared.

    // --- Framebuffer dimensions (dynamic; 1080p or 4K) ----------------------

    uint32_t m_fbWidth;      ///< Framebuffer width in pixels.
    uint32_t m_fbHeight;     ///< Framebuffer height in pixels.

    // --- PS4 Pro / 4K capability state --------------------------------------

    bool m_isNeoMode;        ///< sceKernelIsNeoMode() != 0 (PS4 Pro hardware).
    bool m_is4KOutput;       ///< Current VideoOut resolution height > 1080.

    // --- Private implementation (SDK types live in GfxManager.cpp) ---------

    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_GFX_MANAGER_H


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
///   - Acquire a writable frame, clear it via CPU writes, and present it.
///   - Synchronize frame reuse against display usage.
///   - Shut down graphics resources deterministically.
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

    /// Clear the active framebuffer to the given color using CPU writes.
    /// Requires an active frame (after beginFrame()).
    /// The color is a 32-bit RGBA value (0xAABBGGRR in memory, i.e. byte 0 = R).
    GfxResult clear(uint32_t color);

    /// Finish CPU-side frame preparation and present the completed frame.
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

    /// Returns a raw CPU-accessible pointer to the current frame's framebuffer
    /// (the frame being prepared by beginFrame()). Valid between beginFrame()
    /// and endFrame(). The memory is RGBA8 (A8B8G8R8_SRGB) tightly packed with
    /// row stride framebufferStrideBytes(). Returns nullptr if no frame is
    /// active or the manager is not initialized.
    ///
    /// This is the project-owned framebuffer access used by UiBlitter to copy
    /// compositor pixels into the current VideoOut slot. It intentionally does
    /// not expose any OpenOrbis SDK structures (OrbisVideoOut*, direct-memory
    /// internals, or VideoOut handles).
    void* currentFramebuffer();

    /// Returns the row stride in bytes of the framebuffer returned by
    /// currentFramebuffer(). For the linear A8B8G8R8_SRGB layout this equals
    /// width * 4 (tightly packed, no padding).
    size_t framebufferStrideBytes() const;

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

    // --- Private implementation (SDK types live in GfxManager.cpp) ---------

    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_GFX_MANAGER_H


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

// === ControllerInputStreaming.h ============================================
//
// OpenOrbis Moonlight client — Controller + Keyboard + Mouse Input Streaming
//
// ControllerInputStreaming forwards one local PS4 controller, the PS4
// keyboard (via the IME keyboard API), and the PS4 mouse (via the Mouse
// library) to the active Moonlight stream. It consumes the project-owned
// ControllerState snapshot and KeyboardEvent/MouseEvent queues produced by
// ps4::InputManager, maps them to the moonlight-common-c input API, and
// transmits them while the active stream's input channel is ready.
//
// This module does NOT own the PS4 pad/keyboard/mouse, does NOT open/close
// handles, does NOT initialize UserService/Pad/IME/Mouse, and does NOT own
// the Moonlight connection. It is gated to the active stream generation and
// only sends once the input channel is established (StreamSessionManager
// state == Streaming).
//
// This header exposes only project-owned types. OpenOrbis SDK types and
// moonlight-common-c C types must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_CONTROLLER_INPUT_STREAMING_H
#define MOONLIGHT_CONTROLLER_INPUT_STREAMING_H

#include <cstdint>

namespace ps4 {
class InputManager;
}

namespace ml {

class StreamSessionManager;

// ---------------------------------------------------------------------------
// ControllerInputStreamingState
// ---------------------------------------------------------------------------

/// Application-visible controller-input-streaming state.
enum class ControllerInputStreamingState : uint8_t {
    Uninitialized,  ///< Not initialized.
    Idle,           ///< Initialized, no active stream.
    Active,         ///< Forwarding controller/keyboard/mouse to an active stream.
    Failed          ///< Initialization failed.
};

// ---------------------------------------------------------------------------
// ControllerInputStreaming
// ---------------------------------------------------------------------------

/// Forwards local PS4 controllers (Player 1 + Player 2 up to 4 players),
/// keyboard, and mouse to the active Moonlight stream.
///
/// Responsibilities:
///   - Consume project-owned ControllerState snapshots from InputManager for
///     all active local controllers.
///   - Forward arrival and multi-controller events via moonlight-common-c.
///   - Consume the KeyboardEvent/MouseEvent queues from InputManager.
///   - Map PS4 controls to the moonlight-common-c controller API.
///   - Map PS4 keyboard/mouse events to the moonlight-common-c keyboard/mouse
///     API (converting USB HID keycodes to Windows VK codes).
///   - Transmit input while the active stream's input channel is ready (gated
///     to the active session generation).
///   - Send complete initial controller states when input becomes ready.
///   - Send change-only controller updates plus a low-rate periodic refresh.
///   - Send neutral controller states on disconnect/stop to avoid stuck
///     buttons.
///   - Release all keys/buttons on stop to avoid stuck host input.
///   - Preserve video/audio on input failure.
///
/// Non-responsibilities:
///   - Own the PS4 pad / keyboard / mouse / UserService lifecycle.
///   - Own the Moonlight connection lifecycle.
///   - Route gameplay input through UiManager.
///   - Implement rumble, gyro, or controller remapping.
///
/// Threading: This class is main-thread owned. update() is called once per
/// main-loop frame after InputManager::poll(). The moonlight-common-c input
/// send APIs are non-blocking and safe to call from the main thread while the
/// connection is active.
class ControllerInputStreaming {
public:
    /// Constructor performs no fallible operations.
    ControllerInputStreaming();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~ControllerInputStreaming();

    // Non-copyable, non-movable.
    ControllerInputStreaming(const ControllerInputStreaming&) = delete;
    ControllerInputStreaming& operator=(const ControllerInputStreaming&) = delete;
    ControllerInputStreaming(ControllerInputStreaming&&) = delete;
    ControllerInputStreaming& operator=(ControllerInputStreaming&&) = delete;

    /// Initialize. `inputManager` and `streamSessionManager` are non-owning
    /// pointers to initialized subsystems. Must be called once.
    /// Returns true on success.
    bool initialize(ps4::InputManager* inputManager,
                    ml::StreamSessionManager* streamSessionManager);

    /// Release resources. Idempotent. Must not throw.
    void shutdown();

    /// Begin forwarding for the given session generation.
    /// Starts keyboard/mouse reception and stays dormant until the target
    /// generation's input channel is ready (StreamSessionManager state ==
    /// Streaming). `mouseEmulationEnabled` is the initial controller-assisted
    /// mouse emulation state (touchpad movement → relative mouse, Cross/Circle
    /// → left/right click). It always starts OFF for a new stream and can be
    /// toggled in-stream via Options + TouchPad Click.
    /// `swapFaceButtons`: whether Cross/Circle are swapped on the host.
    /// `gyroMotionEnabled`: whether to announce gyro/accel capabilities and stream IMU data.
    void start(uint32_t sessionGeneration, bool mouseEmulationEnabled,
               bool swapFaceButtons = false, bool gyroMotionEnabled = true);

    /// Stop forwarding. Sends a neutral controller state and releases all
    /// keys/buttons if the connection is still usable, then resets local
    /// state and stops keyboard/mouse reception.
    void stop();

    /// Called once per main-loop frame after InputManager::poll().
    /// Forwards the latest controller state and drains keyboard/mouse events
    /// when active and input-ready.
    void update();

    /// Returns the current controller-input-streaming state.
    ControllerInputStreamingState state() const;

    /// Returns true if the module is Active (forwarding or dormant awaiting
    /// input readiness for the active generation).
    bool isActive() const;

    /// Returns whether controller-assisted mouse emulation is currently
    /// enabled for the active session. Starts OFF and can be toggled in-stream
    /// via Options + TouchPad Click. Used by MoonlightApp to detect toggles
    /// and drive the in-stream mouse emulation state overlay.
    bool isMouseEmulationEnabled() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_CONTROLLER_INPUT_STREAMING_H

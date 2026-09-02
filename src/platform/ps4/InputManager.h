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

// === InputManager.h ==========================================================
//
// Moonlight client for PlayStation 4 — PS4 Input subsystem
//
// InputManager is the concrete PS4 controller input subsystem. It owns pad
// initialization, controller handle/lifetime, polling, previous/current button
// state, edge detection, and normalized UI input generation.
//
// This header exposes only project-owned normalized input accessors. Raw PS4
// SDK pad structures (OrbisPadData) are kept private to the implementation
// file.
//
// Keyboard, mouse, IME keyboard, and key conversion are implemented in the
// implementation file. This header exposes only the project-owned
// KeyboardEvent/MouseEvent types and the start/stop/poll/take accessors.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// No OpenOrbis SDK types are exposed.
// ===============================================================================

#ifndef PS4_INPUT_MANAGER_H
#define PS4_INPUT_MANAGER_H

#include <cstdint>

// Forward declaration of the PS4 IME event struct (opaque pointer only). The
// full definition is confined to the implementation file. This is required so
// the friend declaration of the IME keyboard event handler can name the type.
struct SceImeEvent;

namespace ps4 {

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/// Categories of input-operation results.
enum class InputResultCategory : uint8_t {
    Success,
    AlreadyInitialized,
    InvalidLifecycleState,
    UserServiceInitFailure,
    InitialUserQueryFailure,
    PadInitFailure,
    PadOpenFailure
};

/// A lightweight result returned by input lifecycle operations.
struct InputResult {
    InputResultCategory category;
    uint32_t            nativeError;   ///< OpenOrbis SDK error code (0 when none)
    // Implicit success.
    InputResult() : category(InputResultCategory::Success), nativeError(0) {}
    InputResult(InputResultCategory cat, uint32_t err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == InputResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// ControllerState — project-owned full controller snapshot
// ---------------------------------------------------------------------------

/// Project-owned full controller snapshot (gameplay/raw state).
///
/// This is an additional view of the same successful pad poll used for UI
/// edge detection. It carries complete held-button state, analog stick
/// positions, analog trigger values, and connection state. No OpenOrbis SDK types
/// are exposed.
struct ControllerState {
    uint32_t buttons = 0;       ///< Raw PS4 button bitmask (OrbisPadButton bits).
    int16_t  leftStickX = 0;    ///< Signed, -32768..32767, center 0.
    int16_t  leftStickY = 0;    ///< Signed, -32768..32767, center 0 (up positive).
    int16_t  rightStickX = 0;   ///< Signed, -32768..32767, center 0.
    int16_t  rightStickY = 0;   ///< Signed, -32768..32767, center 0 (up positive).
    uint8_t  leftTrigger = 0;   ///< 0..255 (0=released, 255=full).
    uint8_t  rightTrigger = 0;  ///< 0..255 (0=released, 255=full).
    bool     connected = false; ///< true if a controller is connected.
    /// Touchpad state (first touch point). `touchActive` is true when at least
    /// one finger is on the touchpad; `touchX`/`touchY` are the current
    /// position valid only while `touchActive`. `touchFingers` is the number
    /// of active touch points (0 when none).
    bool     touchActive = false;
    int16_t  touchX = 0;
    int16_t  touchY = 0;
    uint8_t  touchFingers = 0;
};

// ---------------------------------------------------------------------------
// KeyboardEvent — project-owned keyboard event
// ---------------------------------------------------------------------------

/// Keyboard key action.
enum class KeyAction : uint8_t {
    Down,   ///< Key pressed.
    Up,     ///< Key released.
    Repeat  ///< Key repeat.
};

/// Project-owned keyboard event produced by the PS4 IME keyboard API.
///
/// `keycode` is a USB HID USAGE code (layout-independent). `modifiers` holds
/// the Moonlight MODIFIER_* flags (SHIFT/CTRL/ALT/META) derived from the IME
/// key status. `keyboardType` is the SceImeKeyboardType (keyboard layout),
/// used to convert the USB HID code to a Windows VK code for the host.
struct KeyboardEvent {
    uint16_t keycode = 0;       ///< USB HID USAGE key code.
    KeyAction action = KeyAction::Down;
    uint8_t  modifiers = 0;     ///< Moonlight MODIFIER_* flags.
    uint32_t keyboardType = 0;  ///< SceImeKeyboardType (keyboard layout).
};

// ---------------------------------------------------------------------------
// MouseEvent — project-owned mouse event
// ---------------------------------------------------------------------------

/// Project-owned mouse event produced by the PS4 Mouse library.
///
/// `deltaX`/`deltaY` are relative movement amounts. `wheel`/`tilt` are scroll
/// amounts. `buttons` holds the SCE_MOUSE_BUTTON_* bitmask. `connected`
/// indicates whether a mouse is present.
struct MouseEvent {
    int32_t  deltaX = 0;    ///< Relative X movement.
    int32_t  deltaY = 0;    ///< Relative Y movement.
    int32_t  wheel = 0;     ///< Vertical wheel movement.
    int32_t  tilt = 0;      ///< Horizontal (tilt) wheel movement.
    uint32_t buttons = 0;   ///< SCE_MOUSE_BUTTON_* bitmask.
    bool     connected = false; ///< true if a mouse is connected.
};

// ---------------------------------------------------------------------------
// InputManager — concrete PS4 controller input subsystem
// ---------------------------------------------------------------------------

/// Owns PS4 controller polling and produces normalized application input.
///
/// This class is main-thread owned. It must not know which UI screen is
/// active, host-card count, or application state.
class InputManager {
public:
    /// Constructor performs no fallible operations.
    InputManager();

    /// Destructor must not throw.
    ~InputManager();

    // Non-copyable, non-movable.
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;
    InputManager(InputManager&&) = delete;
    InputManager& operator=(InputManager&&) = delete;

    /// Initialize PS4 pad facilities and open the controller.
    InputResult initialize();

    /// Release pad resources. Safe after partial initialization, normal
    /// loop completion, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Poll the controller once. Must be called once per application frame.
    void poll();

    /// Returns the most recent full controller snapshot (gameplay/raw state).
    bool latestControllerState(ControllerState& out) const;

    // --- Keyboard (PS4 IME keyboard API) ------------------------------------

    /// Start keyboard reception for the launching user. Loads the LIBIME and
    /// CONVERT_KEYCODE sysmodules and opens the IME keyboard. Must be called
    /// only while a stream is active (never while an ImeDialog is open).
    /// Returns true on success.
    bool startKeyboard();

    /// Stop keyboard reception and release resources. Idempotent.
    void stopKeyboard();

    /// Returns true if keyboard reception is currently active.
    bool isKeyboardActive() const;

    /// Poll the IME keyboard and dispatch any pending key events into the
    /// internal bounded queue. Must be called once per frame while keyboard
    /// reception is active.
    void pollKeyboard();

    /// Drain up to `max` pending keyboard events into `out`. Returns the
    /// number of events written. Returns 0 when no events are pending.
    int takeKeyboardEvents(KeyboardEvent* out, int max);

    // --- Mouse (PS4 Mouse library) ------------------------------------------

    /// Start mouse reception for the launching user. Loads the MOUSE sysmodule
    /// and opens the mouse port in merged mode. Must be called only while a
    /// stream is active. Returns true on success.
    bool startMouse();

    /// Stop mouse reception and release resources. Idempotent.
    void stopMouse();

    /// Returns true if mouse reception is currently active.
    bool isMouseActive() const;

    /// Poll the mouse and drain any pending samples into the internal bounded
    /// queue. Must be called once per frame while mouse reception is active.
    void pollMouse();

    /// Drain up to `max` pending mouse events into `out`. Returns the number
    /// of events written. Returns 0 when no events are pending.
    int takeMouseEvents(MouseEvent* out, int max);

    // --- Normalized UI input (edge-triggered: rising edge) ------------------

    /// D-pad Up just pressed.
    bool isUpJustPressed() const;

    /// D-pad Down just pressed.
    bool isDownJustPressed() const;

    /// D-pad Left just pressed.
    bool isLeftJustPressed() const;

    /// D-pad Right just pressed.
    bool isRightJustPressed() const;

    /// Cross just pressed (Accept).
    bool isAcceptJustPressed() const;

    /// Circle just pressed (Back).
    bool isBackJustPressed() const;

    /// Triangle just pressed (context-sensitive action).
    bool isTriangleJustPressed() const;

    /// Options just pressed (opens/closes the Settings overlay).
    bool isOptionsJustPressed() const;

    /// Square just pressed (context-sensitive action; opens Add Host from the
    /// Host List).
    bool isSquareJustPressed() const;

    /// Touchpad click just pressed (opens the Help screen from the Host/App
    /// List).
    bool isTouchpadJustPressed() const;

    /// Returns true when Cross is the accept/enter button (Western
    /// convention), false when Circle is the accept/enter button (Japanese
    /// convention). Defaults to true (Cross) because the OpenOrbis
    /// SystemService does not expose the Enter Button Assignment query.
    bool isCrossAccept() const;

private:
    struct Impl;
    Impl* m_impl;

    /// Attempt to open the IME keyboard. Returns true on success. Shared by
    /// startKeyboard() and the pollKeyboard() retry path so a keyboard plugged
    /// in after stream launch is picked up automatically.
    bool tryOpenKeyboard();

    // Friend so the IME keyboard event handler (defined in the .cpp) can
    // enqueue into the private Impl's bounded keyboard queue.
    friend void imeKeyboardHandler(void* arg, const SceImeEvent* e);
};

} // namespace ps4

#endif // PS4_INPUT_MANAGER_H

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

// === ControllerInputStreaming.cpp ==========================================
//
// OpenOrbis Moonlight client — Controller + Keyboard + Mouse Input Streaming
//
// Implements the ControllerInputStreaming concrete subsystem:
//   - Consumes the project-owned ControllerState snapshot from InputManager.
//   - Consumes the KeyboardEvent/MouseEvent queues from InputManager.
//   - Maps PS4 controls to the moonlight-common-c controller API.
//   - Maps PS4 keyboard/mouse events to the moonlight-common-c keyboard/mouse
//     API (converting USB HID keycodes to Windows VK codes).
//   - Transmits input while the active stream's input channel is ready (gated
//     to the active session generation).
//   - Sends a complete initial controller state when input becomes ready.
//   - Sends change-only controller updates plus a low-rate periodic refresh.
//   - Sends a neutral controller state on disconnect/stop to avoid stuck
//     buttons.
//   - Releases all keys/buttons on stop to avoid stuck host input.
//   - Preserves video/audio on input failure.
//
// moonlight-common-c C headers are confined to this translation unit.
// No Moonlight C types escape into public headers.
// ===============================================================================

#include "ControllerInputStreaming.h"

#include "platform/ps4/InputManager.h"   // ps4::InputManager, ps4::ControllerState, ps4::KeyboardEvent, ps4::MouseEvent, ps4::KeyAction
#include "StreamSessionManager.h"        // ml::StreamSessionManager, ml::StreamSessionState

#include <orbis/libkernel.h>             // sceKernelGetProcessTime

// --- moonlight-common-c (C) header — confined to this translation unit --------
#include "Limelight.h"                   // LiSendControllerEvent, LiSendKeyboardEvent2, LiSendMouseMoveEvent, LiSendMouseButtonEvent, LiSendHighResScrollEvent, LiSendHighResHScrollEvent, button flags

#include <cstdio>                        // printf, fflush, snprintf

// --- Local prototype for sceConvertKeycodeGetVirtualKeycode ------------------
// The OpenOrbis ConvertKeycode.h stub declares this as void with no arguments.
// Declare the real prototype here (matching the SCE reference) so the
// USB HID → Windows VK conversion can be called correctly.
extern "C" int sceConvertKeycodeGetVirtualKeycode(uint16_t keycode,
                                                  uint32_t keyboardType,
                                                  uint16_t* vkeycode);

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void cisDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Periodic refresh interval in main-loop frames (~1 second at 60 Hz).
constexpr uint32_t kRefreshIntervalFrames = 60;

/// Moonlight button flags for the PS4 controls we forward.
constexpr int kFlagCross   = A_FLAG;      // 0x1000
constexpr int kFlagCircle  = B_FLAG;      // 0x2000
constexpr int kFlagSquare  = X_FLAG;      // 0x4000
constexpr int kFlagTriangle= Y_FLAG;      // 0x8000
constexpr int kFlagUp      = UP_FLAG;     // 0x0001
constexpr int kFlagDown    = DOWN_FLAG;   // 0x0002
constexpr int kFlagLeft    = LEFT_FLAG;   // 0x0004
constexpr int kFlagRight   = RIGHT_FLAG;  // 0x0008
constexpr int kFlagL1      = LB_FLAG;     // 0x0100
constexpr int kFlagR1      = RB_FLAG;     // 0x0200
constexpr int kFlagL3      = LS_CLK_FLAG; // 0x0040
constexpr int kFlagR3      = RS_CLK_FLAG; // 0x0080
constexpr int kFlagOptions = PLAY_FLAG;   // 0x0010
constexpr int kFlagBack    = BACK_FLAG;   // 0x0020 (Back/Select)
constexpr int kFlagSpecial = SPECIAL_FLAG; // 0x0400 (PS/Home/Guide)

// PS4 raw button bits used by the chord logic (ORBIS_PAD_BUTTON_*).
constexpr uint32_t kRawTouchpad = 0x00100000u; // ORBIS_PAD_BUTTON_TOUCH_PAD
constexpr uint32_t kRawOptions  = 0x00000008u; // ORBIS_PAD_BUTTON_OPTIONS
constexpr uint32_t kRawL3       = 0x00000002u; // ORBIS_PAD_BUTTON_L3
constexpr uint32_t kRawR3       = 0x00000004u; // ORBIS_PAD_BUTTON_R3
constexpr uint32_t kRawL1       = 0x00000400u; // ORBIS_PAD_BUTTON_L1
constexpr uint32_t kRawR1       = 0x00000800u; // ORBIS_PAD_BUTTON_R1

// PS4 raw button bits used by mouse emulation (ORBIS_PAD_BUTTON_*).
constexpr uint32_t kRawCross  = 0x00004000u; // ORBIS_PAD_BUTTON_CROSS
constexpr uint32_t kRawCircle = 0x00002000u; // ORBIS_PAD_BUTTON_CIRCLE

/// Touchpad delta → mouse delta scale factor for controller-assisted mouse
/// emulation. The touchpad is 1920×942 units; a full-width swipe (~1920 units)
/// should move the cursor a reasonable distance. A factor of 1.0 maps touchpad
/// units 1:1 to mouse units (host pixels). A factor of 0.5 gives a full swipe
/// ≈ 960 px, a comfortable default for a 1080p host. This is a fixed initial
/// value; a future "Mouse Sensitivity" UI setting can replace it.
constexpr float kTouchpadMouseSensitivity = 0.5f;

/// Touchpad X midpoint (the touchpad is 1920 units wide). A single touch on
/// the left half (touchX < this) + touchpad click maps to Back/Select; a
/// single touch on the right half (touchX >= this) + touchpad click maps to
/// Start.
constexpr int16_t kTouchpadXMidpoint = 960;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of ControllerInputStreaming.
struct ControllerInputStreaming::Impl {
    // --- Dependencies (non-owning) ------------------------------------------
    ps4::InputManager* inputManager = nullptr;
    StreamSessionManager* streamSessionManager = nullptr;

    // --- Lifecycle ----------------------------------------------------------
    ControllerInputStreamingState state = ControllerInputStreamingState::Uninitialized;
    bool initialized = false;

    // --- Session gating -----------------------------------------------------
    uint32_t sessionGeneration = 0;   ///< Generation this module is forwarding for.
    bool inputReady = false;          ///< True once the target input channel is ready.

    // --- Guide chord state tracking (L1 + R1 + Options -> PS/Guide pulse) ---
    struct GuideChordState {
        enum class Phase { Idle, Neutral, Pulse, Drain };
        Phase phase = Phase::Idle;
        uint64_t releaseUs = 0;
    };

    // --- Per-controller tracking --------------------------------------------
    struct ControllerTrack {
        ps4::ControllerState previousState;
        bool havePreviousState = false;
        uint32_t framesSinceRefresh = 0;
        bool wasConnected = false;
        bool arrivalSent = false;
        bool firstPacketSent = false;
        bool firstPacketLogged = false;
        GuideChordState guideChord;
    };

    static constexpr int kMaxControllers = ps4::InputManager::kMaxControllers;
    ControllerTrack controllers[kMaxControllers];

    uint16_t computeActiveGamepadMask() const {
        uint16_t mask = 0;
        for (int i = 0; i < kMaxControllers; ++i) {
            if (controllers[i].wasConnected) {
                mask |= static_cast<uint16_t>(1 << i);
            }
        }
        return mask;
    }

    void resetControllers() {
        for (int i = 0; i < kMaxControllers; ++i) {
            controllers[i].previousState = ps4::ControllerState();
            controllers[i].havePreviousState = false;
            controllers[i].framesSinceRefresh = 0;
            controllers[i].wasConnected = false;
            controllers[i].arrivalSent = false;
            controllers[i].firstPacketSent = false;
            controllers[i].firstPacketLogged = false;
            controllers[i].guideChord = GuideChordState{};
        }
    }

    // --- Diagnostics --------------------------------------------------------
    bool inputReadyLogged = false;

    // --- Keyboard/mouse state ------------------------------------------------
    // Tracks the currently-held mouse buttons so we can release them all on
    // stop (avoid stuck buttons on the host).
    uint32_t heldMouseButtons = 0;
    // Tracks the currently-held keyboard keys (Windows VK codes) so we can
    // release them all on stop (avoid stuck keys on the host).
    // Fixed-size array indexed by VK code (0..255).
    bool heldKeys[256] = {false};

    // --- Controller-assisted mouse emulation state ---------------------------
    // Whether mouse emulation is enabled for the current session. Starts OFF
    // at start() and can be toggled in-stream via Options + TouchPad Click.
    bool mouseEmulationEnabled = false;
    // Touch-delta tracking: whether a previous touch position is available.
    // Reset on touch start (to avoid a large initial jump) and on touch
    // release / session stop.
    bool havePrevTouch = false;
    int16_t prevTouchX = 0;
    int16_t prevTouchY = 0;
    // Tracks the currently-held emulated mouse buttons (Cross→left, Circle→
    // right) so they can be released on stop (avoid stuck buttons on host).
    uint32_t heldEmulatedMouseButtons = 0;
    // Whether the mouse-assist toggle chord (Options + TouchPad Click) was
    // held on the previous frame, for rising-edge toggle detection.
    bool prevMouseAssistChordHeld = false;

    // --- Helper: build the Moonlight button mask from a ControllerState -----
    //
    // Chord recognition happens here, before the outgoing host button mask is
    // finalized, so the change-only send and periodic-refresh logic operate on
    // the host-visible state automatically.
    //
    // Precedence:
    //   1. Options + L3             → local stream exit (handled by MoonlightApp).
    //                                 Suppress Options and L3 from the host.
    //   2. Options + TouchPad Click → local mouse-assist toggle (handled in update()).
    //                                 Suppress Options and the touchpad click.
    //   3. L1 + R1 + Options        → Guide chord filter (sends 80ms PS/Guide pulse;
    //                                 suppresses L1, R1, and Options until all released).
    //   4. Options                  → Host Options/Start (PLAY_FLAG).
    //   5. Touchpad Click           → Host Touchpad / Back / Select (BACK_FLAG).
    //   6. Otherwise                → normal standalone button mapping.
    //
    // When `mouseEmulationActive` is true, Cross and Circle are consumed as
    // mouse buttons (emitted by processMouseEmulation) and are NOT forwarded
    // as controller A/B buttons.
    //
    // Only the chord's own components are consumed; unrelated
    // concurrently-held buttons continue to pass through normally.
    static int buildButtonFlags(const ps4::ControllerState& s,
                                bool mouseEmulationActive,
                                GuideChordState& guide) {
        const uint32_t b = s.buttons;

        const bool touchpadPressed = (b & kRawTouchpad) != 0;
        const bool optionsPressed  = (b & kRawOptions) != 0;
        const bool l3Pressed       = (b & kRawL3) != 0;
        const bool l1Pressed       = (b & kRawL1) != 0;
        const bool r1Pressed       = (b & kRawR1) != 0;

        // Exit chord (highest priority): Options + L3.
        const bool exitChord = optionsPressed && l3Pressed;
        // Mouse-assist toggle chord: Options + TouchPad Click.
        const bool mouseAssistChord = optionsPressed && touchpadPressed;

        int flags = 0;
        // Cross/Circle are consumed as mouse buttons when mouse emulation is
        // active; otherwise they map to A/B.
        if (!mouseEmulationActive) {
            if (b & 0x00004000u) flags |= kFlagCross;      // ORBIS_PAD_BUTTON_CROSS
            if (b & 0x00002000u) flags |= kFlagCircle;     // ORBIS_PAD_BUTTON_CIRCLE
        }
        if (b & 0x00008000u) flags |= kFlagSquare;     // ORBIS_PAD_BUTTON_SQUARE
        if (b & 0x00001000u) flags |= kFlagTriangle;   // ORBIS_PAD_BUTTON_TRIANGLE
        if (b & 0x00000010u) flags |= kFlagUp;         // ORBIS_PAD_BUTTON_UP
        if (b & 0x00000040u) flags |= kFlagDown;       // ORBIS_PAD_BUTTON_DOWN
        if (b & 0x00000080u) flags |= kFlagLeft;       // ORBIS_PAD_BUTTON_LEFT
        if (b & 0x00000020u) flags |= kFlagRight;      // ORBIS_PAD_BUTTON_RIGHT

        if (l1Pressed) flags |= kFlagL1;               // ORBIS_PAD_BUTTON_L1 → LB
        if (r1Pressed) flags |= kFlagR1;               // ORBIS_PAD_BUTTON_R1 → RB

        // Options maps directly to Host Options / Start (PLAY_FLAG).
        if (optionsPressed && !exitChord && !mouseAssistChord) {
            flags |= kFlagOptions;
        }

        // Touchpad click maps to Host Touchpad / Back / Select (BACK_FLAG).
        if (touchpadPressed && !mouseAssistChord) {
            flags |= kFlagBack;
        }

        // L3, unless consumed by the exit chord.
        if (l3Pressed && !exitChord) {
            flags |= kFlagL3;
        }

        // R3 passes through normally.
        if (b & kRawR3) {
            flags |= kFlagR3;
        }

        // Guide Chord Filter: L1 + R1 + Options -> PS / Guide button (SPECIAL_FLAG)
        // Sends an 80 ms Guide pulse, and suppresses L1, R1, and Options until
        // all three chord buttons are fully released.
        const int kChordMask = kFlagL1 | kFlagR1 | kFlagOptions;
        const bool chordHeld = (flags & kChordMask) == kChordMask;
        const uint64_t now = static_cast<uint64_t>(sceKernelGetProcessTime());

        switch (guide.phase) {
        case GuideChordState::Phase::Idle:
            if (chordHeld && !touchpadPressed && !exitChord && !mouseAssistChord) {
                // Release previously forwarded buttons before pressing Guide.
                guide.phase = GuideChordState::Phase::Neutral;
                return flags & ~kChordMask;
            }
            break;

        case GuideChordState::Phase::Neutral:
            guide.phase = GuideChordState::Phase::Pulse;
            guide.releaseUs = now + 80000; // 80 ms pulse
            return (flags & ~kChordMask) | kFlagSpecial;

        case GuideChordState::Phase::Pulse:
            if (now < guide.releaseUs) {
                return (flags & ~kChordMask) | kFlagSpecial;
            }
            guide.phase = GuideChordState::Phase::Drain;
            return flags & ~kChordMask;

        case GuideChordState::Phase::Drain:
        default:
            if (!l1Pressed && !r1Pressed && !optionsPressed) {
                guide.phase = GuideChordState::Phase::Idle;
            }
            return flags & ~kChordMask;
        }

        return flags;
    }

    // --- Helper: send a controller state to the host ------------------------
    // Sends via LiSendMultiControllerEvent for the given controller slot.
    bool sendMultiState(short controllerNumber,
                        uint16_t activeGamepadMask,
                        const ps4::ControllerState& s,
                        bool mouseEmulationActive) {
        if (!inputReady) {
            return false;
        }
        int flags = buildButtonFlags(s, mouseEmulationActive,
                                     controllers[controllerNumber].guideChord);
        int rc = LiSendMultiControllerEvent(controllerNumber,
                                            activeGamepadMask,
                                            flags,
                                            s.leftTrigger,
                                            s.rightTrigger,
                                            s.leftStickX,
                                            s.leftStickY,
                                            s.rightStickX,
                                            s.rightStickY);
        // rc == -2 means the input stream is not initialized (connection
        // teardown in progress). Treat as non-fatal; do not spam logs.
        return rc >= 0;
    }

    // --- Helper: send a neutral state for a specific controller slot --------
    void sendMultiNeutralState(short controllerNumber, uint16_t activeGamepadMask) {
        ps4::ControllerState neutral;
        neutral.connected = true;  // neutral is a valid connected state
        sendMultiState(controllerNumber, activeGamepadMask, neutral, false);
    }

    // --- Helper: send neutral states for all currently connected controllers -
    void sendAllNeutralStates() {
        for (int i = 0; i < kMaxControllers; ++i) {
            if (controllers[i].wasConnected) {
                sendMultiNeutralState(static_cast<short>(i), 0);
            }
        }
    }

    // --- Helper: convert a USB HID keycode to a Windows VK code ------------
    // Uses the OpenOrbis SDK's convert_keycode library. Falls back to the raw
    // keycode if conversion fails. The result is OR'd with 0x8000 to signal
    // "scancode mode" to the host (matching moonlight-qt).
    //
    // If the reported keyboard type is NONE (0), default to English US so the
    // conversion produces correct VK codes. Without this, sceConvertKeycode
    // fails and we fall back to raw USB HID usage codes, which collide with
    // VK number codes (e.g. `;`=0x33→VK_3, `'`=0x34→VK_4) — the "numeric
    // keyboard" bug.
    short usbHidToVk(uint16_t keycode, uint32_t keyboardType) {
        if (keyboardType == 0) {
            keyboardType = 4;  // SCE_IME_KEYBOARD_TYPE_ENGLISH_US
        }
        uint16_t vk = 0;
        int rc = sceConvertKeycodeGetVirtualKeycode(keycode, keyboardType, &vk);
        if (rc == 0 && vk != 0) {
            return static_cast<short>(0x8000 | vk);
        }
        // Fallback: pass the raw keycode through in scancode mode.
        return static_cast<short>(0x8000 | keycode);
    }

    // --- Helper: send a keyboard event to the host --------------------------
    void sendKeyboardEvent(const ps4::KeyboardEvent& ev) {
        if (!inputReady) {
            return;
        }
        short vk = usbHidToVk(ev.keycode, ev.keyboardType);
        char action = (ev.action == ps4::KeyAction::Up) ? KEY_ACTION_UP : KEY_ACTION_DOWN;
        // Ignore repeat events (the host synthesizes key repeat).
        if (ev.action == ps4::KeyAction::Repeat) {
            return;
        }

        // Track held keys so we can release them all on stop.
        uint8_t vkLow = static_cast<uint8_t>(vk & 0xFF);
        if (action == KEY_ACTION_DOWN) {
            heldKeys[vkLow] = true;
        } else {
            heldKeys[vkLow] = false;
        }

        LiSendKeyboardEvent2(vk, action, static_cast<char>(ev.modifiers), 0);
    }

    // --- Helper: send a mouse event to the host -----------------------------
    void sendMouseEvent(const ps4::MouseEvent& ev) {
        if (!inputReady) {
            return;
        }

        // Relative movement.
        if (ev.deltaX != 0 || ev.deltaY != 0) {
            LiSendMouseMoveEvent(static_cast<short>(ev.deltaX),
                                 static_cast<short>(ev.deltaY));
        }

        // Vertical wheel.
        if (ev.wheel != 0) {
            LiSendHighResScrollEvent(static_cast<short>(ev.wheel));
        }

        // Horizontal (tilt) wheel.
        if (ev.tilt != 0) {
            LiSendHighResHScrollEvent(static_cast<short>(ev.tilt));
        }

        // Buttons: detect press/release transitions.
        const uint32_t newButtons = ev.buttons;
        const uint32_t pressed = newButtons & ~heldMouseButtons;
        const uint32_t released = heldMouseButtons & ~newButtons;

        // Map PS4 mouse buttons to Moonlight button codes.
        auto mapButton = [](uint32_t ps4Btn) -> int {
            switch (ps4Btn) {
                case 0x00000001u: return BUTTON_LEFT;   // SCE_MOUSE_BUTTON_PRIMARY
                case 0x00000002u: return BUTTON_RIGHT;  // SCE_MOUSE_BUTTON_SECONDARY
                case 0x00000004u: return BUTTON_MIDDLE; // SCE_MOUSE_BUTTON_OPTIONAL
                case 0x00000008u: return BUTTON_X1;     // SCE_MOUSE_BUTTON_OPTIONAL2
                case 0x00000010u: return BUTTON_X2;     // SCE_MOUSE_BUTTON_OPTIONAL3
                default: return 0;
            }
        };

        // Send releases first, then presses.
        for (uint32_t bit = 0x01; bit <= 0x10; bit <<= 1) {
            if (released & bit) {
                int btn = mapButton(bit);
                if (btn != 0) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, btn);
                }
            }
        }
        for (uint32_t bit = 0x01; bit <= 0x10; bit <<= 1) {
            if (pressed & bit) {
                int btn = mapButton(bit);
                if (btn != 0) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, btn);
                }
            }
        }

        heldMouseButtons = newButtons;
    }

    // --- Helper: release all held emulated mouse buttons -------------------
    // Sends release events for any held emulated mouse buttons (Cross→left,
    // Circle→right) and clears the held set. Used when mouse emulation is
    // toggled off mid-stream so no mouse button is left stuck on the host.
    void releaseEmulatedMouseButtons() {
        if (inputReady) {
            if (heldEmulatedMouseButtons & kRawCross) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            if (heldEmulatedMouseButtons & kRawCircle) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            }
        }
        heldEmulatedMouseButtons = 0;
    }

    // --- Helper: release all held keys/buttons on stop ----------------------
    void releaseAllInput() {
        if (!inputReady) {
            return;
        }
        // Release all held keyboard keys.
        for (int i = 0; i < 256; ++i) {
            if (heldKeys[i]) {
                LiSendKeyboardEvent2(static_cast<short>(0x8000 | i),
                                     KEY_ACTION_UP, 0, 0);
                heldKeys[i] = false;
            }
        }
        // Release all held mouse buttons.
        for (uint32_t bit = 0x01; bit <= 0x10; bit <<= 1) {
            if (heldMouseButtons & bit) {
                int btn = 0;
                switch (bit) {
                    case 0x00000001u: btn = BUTTON_LEFT; break;
                    case 0x00000002u: btn = BUTTON_RIGHT; break;
                    case 0x00000004u: btn = BUTTON_MIDDLE; break;
                    case 0x00000008u: btn = BUTTON_X1; break;
                    case 0x00000010u: btn = BUTTON_X2; break;
                    default: break;
                }
                if (btn != 0) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, btn);
                }
            }
        }
        heldMouseButtons = 0;

        // Release all held emulated mouse buttons (Cross→left, Circle→right).
        if (heldEmulatedMouseButtons & kRawCross) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        }
        if (heldEmulatedMouseButtons & kRawCircle) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        }
        heldEmulatedMouseButtons = 0;

        // Reset touch-delta tracking so no stale previous position survives.
        havePrevTouch = false;
        prevTouchX = 0;
        prevTouchY = 0;
    }

    // --- Helper: process controller-assisted mouse emulation ---------------
    // Consumes the current ControllerState and emits relative mouse movement
    // from touchpad deltas and left/right clicks from Cross/Circle. When mouse
    // emulation is active, Cross/Circle are consumed as mouse buttons (not
    // forwarded as controller A/B by buildButtonFlags). Touchpad click is NOT
    // used here (it remains reserved for the Back/Start mapping).
    void processMouseEmulation(const ps4::ControllerState& s) {
        if (!inputReady) {
            return;
        }

        // --- Touchpad movement → relative mouse ----------------------------
        if (s.touchActive) {
            if (havePrevTouch) {
                int32_t deltaX = static_cast<int32_t>(s.touchX) - prevTouchX;
                int32_t deltaY = static_cast<int32_t>(s.touchY) - prevTouchY;

                // Scale by the fixed sensitivity.
                int32_t scaledX = static_cast<int32_t>(
                    static_cast<float>(deltaX) * kTouchpadMouseSensitivity);
                int32_t scaledY = static_cast<int32_t>(
                    static_cast<float>(deltaY) * kTouchpadMouseSensitivity);

                // Clamp to the short range accepted by LiSendMouseMoveEvent.
                if (scaledX > 32767) scaledX = 32767;
                if (scaledX < -32768) scaledX = -32768;
                if (scaledY > 32767) scaledY = 32767;
                if (scaledY < -32768) scaledY = -32768;

                if (scaledX != 0 || scaledY != 0) {
                    LiSendMouseMoveEvent(static_cast<short>(scaledX),
                                         static_cast<short>(scaledY));
                }
            }
            // On touch start (no previous position), just record the current
            // position so the first frame produces no large jump.
            prevTouchX = s.touchX;
            prevTouchY = s.touchY;
            havePrevTouch = true;
        } else {
            // Touch released: clear movement tracking.
            havePrevTouch = false;
            prevTouchX = 0;
            prevTouchY = 0;
        }

        // --- Cross/Circle → left/right mouse buttons -----------------------
        // Detect press/release transitions and emit mouse button events. When
        // mouse emulation is active, these buttons are consumed as mouse
        // buttons (not forwarded as controller A/B by buildButtonFlags).
        const bool crossPressed = (s.buttons & kRawCross) != 0;
        const bool circlePressed = (s.buttons & kRawCircle) != 0;

        const bool crossHeld = (heldEmulatedMouseButtons & kRawCross) != 0;
        const bool circleHeld = (heldEmulatedMouseButtons & kRawCircle) != 0;

        if (crossPressed && !crossHeld) {
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            heldEmulatedMouseButtons |= kRawCross;
        } else if (!crossPressed && crossHeld) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            heldEmulatedMouseButtons &= ~kRawCross;
        }

        if (circlePressed && !circleHeld) {
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
            heldEmulatedMouseButtons |= kRawCircle;
        } else if (!circlePressed && circleHeld) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            heldEmulatedMouseButtons &= ~kRawCircle;
        }
    }
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ControllerInputStreaming::ControllerInputStreaming()
    : m_impl(new Impl())
{
}

ControllerInputStreaming::~ControllerInputStreaming() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool ControllerInputStreaming::initialize(ps4::InputManager* inputManager,
                                          ml::StreamSessionManager* streamSessionManager) {
    if (m_impl->initialized) {
        return true;  // already initialized
    }
    if (inputManager == nullptr || streamSessionManager == nullptr) {
        m_impl->state = ControllerInputStreamingState::Failed;
        return false;
    }

    m_impl->inputManager = inputManager;
    m_impl->streamSessionManager = streamSessionManager;
    m_impl->state = ControllerInputStreamingState::Idle;
    m_impl->initialized = true;

    cisDiagPrint("[input] controller streaming ready");
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ControllerInputStreaming::shutdown() {
    if (!m_impl) {
        return;
    }

    // Stop keyboard/mouse reception if still active.
    if (m_impl->inputManager) {
        m_impl->inputManager->stopKeyboard();
        m_impl->inputManager->stopMouse();
    }

    m_impl->inputManager = nullptr;
    m_impl->streamSessionManager = nullptr;
    m_impl->state = ControllerInputStreamingState::Uninitialized;
    m_impl->initialized = false;
    m_impl->sessionGeneration = 0;
    m_impl->inputReady = false;
    m_impl->inputReadyLogged = false;
    m_impl->resetControllers();
    m_impl->heldMouseButtons = 0;
    for (int i = 0; i < 256; ++i) {
        m_impl->heldKeys[i] = false;
    }
    m_impl->mouseEmulationEnabled = false;
    m_impl->havePrevTouch = false;
    m_impl->prevTouchX = 0;
    m_impl->prevTouchY = 0;
    m_impl->heldEmulatedMouseButtons = 0;
    m_impl->prevMouseAssistChordHeld = false;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void ControllerInputStreaming::start(uint32_t sessionGeneration,
                                     bool mouseEmulationEnabled) {
    if (!m_impl->initialized) {
        return;
    }

    // Reset per-session state.
    m_impl->sessionGeneration = sessionGeneration;
    m_impl->inputReady = false;
    m_impl->inputReadyLogged = false;
    m_impl->resetControllers();
    m_impl->heldMouseButtons = 0;
    for (int i = 0; i < 256; ++i) {
        m_impl->heldKeys[i] = false;
    }
    m_impl->mouseEmulationEnabled = mouseEmulationEnabled;
    m_impl->havePrevTouch = false;
    m_impl->prevTouchX = 0;
    m_impl->prevTouchY = 0;
    m_impl->heldEmulatedMouseButtons = 0;
    m_impl->prevMouseAssistChordHeld = false;

    // Start keyboard and mouse reception (only while a stream is active).
    if (m_impl->inputManager) {
        m_impl->inputManager->startKeyboard();
        m_impl->inputManager->startMouse();
    }

    m_impl->state = ControllerInputStreamingState::Active;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[input] controller streaming start generation=%u mouseEmulation=%d",
             static_cast<unsigned>(sessionGeneration),
             mouseEmulationEnabled ? 1 : 0);
    cisDiagPrint(buf);
}

void ControllerInputStreaming::stop() {
    if (!m_impl->initialized) {
        return;
    }

    // If the connection is still usable, send neutral controller states for all
    // connected controllers and release all held keys/buttons on the host before
    // we stop forwarding.
    if (m_impl->state == ControllerInputStreamingState::Active &&
        m_impl->inputReady) {
        m_impl->sendAllNeutralStates();
        m_impl->releaseAllInput();
    }

    // Stop keyboard and mouse reception.
    if (m_impl->inputManager) {
        m_impl->inputManager->stopKeyboard();
        m_impl->inputManager->stopMouse();
    }

    m_impl->state = ControllerInputStreamingState::Idle;
    m_impl->sessionGeneration = 0;
    m_impl->inputReady = false;
    m_impl->inputReadyLogged = false;
    m_impl->resetControllers();
    m_impl->heldMouseButtons = 0;
    for (int i = 0; i < 256; ++i) {
        m_impl->heldKeys[i] = false;
    }
    m_impl->mouseEmulationEnabled = false;
    m_impl->havePrevTouch = false;
    m_impl->prevTouchX = 0;
    m_impl->prevTouchY = 0;
    m_impl->heldEmulatedMouseButtons = 0;
    m_impl->prevMouseAssistChordHeld = false;

    cisDiagPrint("[input] controller streaming stopped");
}

// ---------------------------------------------------------------------------
// Update (main-thread, once per frame after InputManager::poll())
// ---------------------------------------------------------------------------

void ControllerInputStreaming::update() {
    if (!m_impl->initialized ||
        m_impl->state != ControllerInputStreamingState::Active) {
        return;
    }

    StreamSessionManager* ssm = m_impl->streamSessionManager;
    if (ssm == nullptr) {
        return;
    }

    // --- Session generation gating ------------------------------------------
    // Only forward for the exact generation this module started for. Do not
    // send into stale or newer generations.
    if (ssm->currentSessionGeneration() != m_impl->sessionGeneration) {
        return;
    }

    // --- Input-readiness gating ---------------------------------------------
    // The input channel is safe only when the stream is fully established
    // (state == Streaming). Do not send merely because start() was called.
    if (ssm->state() != StreamSessionState::Streaming) {
        return;
    }

    if (!m_impl->inputReady) {
        m_impl->inputReady = true;
        if (!m_impl->inputReadyLogged) {
            m_impl->inputReadyLogged = true;
            cisDiagPrint("[input] controller streaming ready");
        }
    }

    // --- Read controller 0 snapshot for chords / mouse emulation ------------
    ps4::ControllerState state0;
    bool state0Valid = m_impl->inputManager &&
                       m_impl->inputManager->latestControllerState(state0, 0);

    // --- Mouse-assist toggle chord (Options + TouchPad Click on Controller 1) -
    // Toggle controller-assisted mouse emulation on the rising edge of the
    // chord. Pinned strictly to Controller 1 so player 2 cannot inadvertently toggle.
    if (state0Valid) {
        const bool chordHeld =
            (state0.buttons & kRawOptions) && (state0.buttons & kRawTouchpad);
        if (chordHeld && !m_impl->prevMouseAssistChordHeld) {
            m_impl->mouseEmulationEnabled = !m_impl->mouseEmulationEnabled;
            if (!m_impl->mouseEmulationEnabled) {
                m_impl->releaseEmulatedMouseButtons();
            }
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "[input] mouse assist toggled %s",
                     m_impl->mouseEmulationEnabled ? "ON" : "OFF");
            cisDiagPrint(buf);
        }
        m_impl->prevMouseAssistChordHeld = chordHeld;
    }

    // --- Process all controller slots (Player 1 + Player 2 up to 4) ---------
    for (int i = 0; i < Impl::kMaxControllers; ++i) {
        Impl::ControllerTrack& track = m_impl->controllers[i];
        ps4::ControllerState state;
        bool isConnected = m_impl->inputManager &&
                           m_impl->inputManager->latestControllerState(state, i) &&
                           state.connected;

        // Disconnect detection
        if (track.wasConnected && !isConnected) {
            track.wasConnected = false;
            track.havePreviousState = false;
            track.arrivalSent = false;
            track.framesSinceRefresh = 0;
            uint16_t newMask = m_impl->computeActiveGamepadMask();
            m_impl->sendMultiNeutralState(static_cast<short>(i), newMask);
            char buf[96];
            snprintf(buf, sizeof(buf), "[input] controller %d disconnected — neutral sent", i);
            cisDiagPrint(buf);
            continue;
        }

        if (!isConnected) {
            continue;
        }

        // New connection / Arrival detection
        bool isNewConnection = !track.wasConnected;
        if (isNewConnection) {
            track.wasConnected = true;
            uint16_t mask = m_impl->computeActiveGamepadMask();
            if (!track.arrivalSent) {
                LiSendControllerArrivalEvent(static_cast<uint8_t>(i),
                                             mask,
                                             LI_CTYPE_PS,
                                             0xFFFFFFFF,
                                             LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_RGB_LED);
                track.arrivalSent = true;
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "[input] controller %d connected/arrived (mask=0x%04X)", i, mask);
            cisDiagPrint(buf);
        }

        uint16_t currentMask = m_impl->computeActiveGamepadMask();
        bool mouseEmulationActive = (i == 0) && m_impl->mouseEmulationEnabled;

        // Change detection
        bool changed = !track.havePreviousState || isNewConnection;
        if (track.havePreviousState) {
            const ps4::ControllerState& p = track.previousState;
            changed = (state.buttons != p.buttons) ||
                      (state.leftStickX != p.leftStickX) ||
                      (state.leftStickY != p.leftStickY) ||
                      (state.rightStickX != p.rightStickX) ||
                      (state.rightStickY != p.rightStickY) ||
                      (state.leftTrigger != p.leftTrigger) ||
                      (state.rightTrigger != p.rightTrigger);
        }

        // Periodic refresh
        bool refresh = false;
        if (track.havePreviousState) {
            ++track.framesSinceRefresh;
            if (track.framesSinceRefresh >= kRefreshIntervalFrames) {
                track.framesSinceRefresh = 0;
                refresh = true;
            }
        }

        if (changed || refresh) {
            m_impl->sendMultiState(static_cast<short>(i), currentMask, state, mouseEmulationActive);
            track.previousState = state;
            track.havePreviousState = true;

            if (!track.firstPacketSent) {
                track.firstPacketSent = true;
                if (!track.firstPacketLogged) {
                    track.firstPacketLogged = true;
                    char buf[96];
                    snprintf(buf, sizeof(buf), "[input] first packet sent for controller %d", i);
                    cisDiagPrint(buf);
                }
            }
        }
    }

    // --- Keyboard/mouse forwarding ------------------------------------------
    // Poll and forward keyboard/mouse events. These are only forwarded while
    // the input channel is ready (gated above).
    if (m_impl->inputManager) {
        // Poll the IME keyboard and drain pending events.
        m_impl->inputManager->pollKeyboard();
        ps4::KeyboardEvent keyEvents[16];
        int nKeys = m_impl->inputManager->takeKeyboardEvents(keyEvents, 16);
        for (int i = 0; i < nKeys; ++i) {
            m_impl->sendKeyboardEvent(keyEvents[i]);
        }

        // Poll the mouse and drain pending events.
        m_impl->inputManager->pollMouse();
        ps4::MouseEvent mouseEvents[16];
        int nMice = m_impl->inputManager->takeMouseEvents(mouseEvents, 16);
        for (int i = 0; i < nMice; ++i) {
            m_impl->sendMouseEvent(mouseEvents[i]);
        }
    }

    // --- Controller-assisted mouse emulation --------------------------------
    // When enabled, touchpad movement acts as a relative mouse and Cross/
    // Circle act as left/right mouse buttons. Cross/Circle are consumed as
    // mouse buttons (not forwarded as controller A/B by buildButtonFlags).
    // Touchpad click remains reserved for the Back/Start mapping and is NOT
    // used for mouse emulation.
    if (m_impl->mouseEmulationEnabled && m_impl->inputReady && state0Valid) {
        m_impl->processMouseEmulation(state0);
    }
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

ControllerInputStreamingState ControllerInputStreaming::state() const {
    return m_impl ? m_impl->state : ControllerInputStreamingState::Uninitialized;
}

bool ControllerInputStreaming::isActive() const {
    return m_impl &&
           m_impl->state == ControllerInputStreamingState::Active;
}

bool ControllerInputStreaming::isMouseEmulationEnabled() const {
    return m_impl && m_impl->mouseEmulationEnabled;
}

} // namespace ml

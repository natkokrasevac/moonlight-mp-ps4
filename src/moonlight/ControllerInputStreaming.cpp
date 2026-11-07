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

#include <orbis/libkernel.h>             // sceKernelGetProcessTime, sceKernelUsleep
#include <pthread.h>
#include <atomic>

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

/// Duration of a synthesized button pulse (PS/Guide from L1+R1+Options, or
/// a quick Options tap released before the 1-second exit-chord hold).
constexpr uint64_t kButtonPulseUs = 80000;   // 80 ms

/// Hold duration for Options + L3 stream exit chord (1 second).
constexpr uint64_t kExitChordHoldUs = 1000000;

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
constexpr int kFlagTouchpad = TOUCHPAD_FLAG; // 0x100000 (DS4 Touchpad Button)

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

/// Sensitivity and threshold constants for 2-finger touchpad gestures
constexpr float kScrollLockTravelPx = 40.0f;     ///< Movement threshold before locking into scroll
constexpr float kScrollPxPerTick    = 128.0f;    ///< Touch px per 1 wheel notch
constexpr float kWheelUnit          = 120.0f;    ///< High-res wheel delta per notch (Windows standard)

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
    bool swapFaceButtons = false;     ///< Cross <-> Circle swap
    bool gyroMotionEnabled = true;    ///< Gyro/accel announcement & reports

    // --- High-frequency (250 Hz) polling thread -----------------------------
    pthread_t pollerThread;
    bool pollerRunning = false;
    std::atomic<bool> pollerStop{false};

    // --- Guide chord state tracking (L1 + R1 + Options -> PS/Guide pulse) ---
    struct GuideChordState {
        enum class Phase { Idle, Neutral, Pulse, Drain };
        Phase phase = Phase::Idle;
        uint64_t releaseUs = 0;
    };

    // --- Exit chord state tracking (Options + L3 on P1) ---------------------
    // L3 is always forwarded so sprinting is never interrupted. When Options
    // is pressed while L3 is held on P1, Options is buffered during the 1s
    // hold window; if released before 1s, an 80 ms Options pulse is sent so
    // tapping Options while sprinting still pauses the game.
    struct ExitChordState {
        enum class Phase { Idle, Buffering, Pulse, Drain };
        Phase phase = Phase::Idle;
        uint64_t holdStartUs = 0;
        uint64_t pulseReleaseUs = 0;
    };

    // --- Per-controller tracking --------------------------------------------
    struct ControllerTrack {
        ps4::ControllerState previousState;
        int lastSentFlags = 0;
        bool havePreviousState = false;
        uint32_t framesSinceRefresh = 0;
        bool wasConnected = false;
        bool arrivalSent = false;
        bool firstPacketSent = false;
        bool firstPacketLogged = false;
        GuideChordState guideChord;
        ExitChordState exitChord;
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
            controllers[i].lastSentFlags = 0;
            controllers[i].havePreviousState = false;
            controllers[i].framesSinceRefresh = 0;
            controllers[i].wasConnected = false;
            controllers[i].arrivalSent = false;
            controllers[i].firstPacketSent = false;
            controllers[i].firstPacketLogged = false;
            controllers[i].guideChord = GuideChordState{};
            controllers[i].exitChord = ExitChordState{};
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

    // Multi-touch gesture state machine
    enum class TouchMode {
        None,
        Pointer,
        PendingTwoFinger,
        Scroll
    };
    TouchMode touchMode = TouchMode::None;
    int trackedFingerIds[2] = {0, 0};
    int trackedX[2] = {0, 0};
    int trackedY[2] = {0, 0};
    float accPointerX = 0.0f;
    float accPointerY = 0.0f;
    float accScrollX = 0.0f;
    float accScrollY = 0.0f;
    float twoFingerTravel = 0.0f;
    bool touchLeftClickHeld = false;
    bool touchRightClickHeld = false;

    // Legacy / Cross-Circle held state for mouse buttons
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
                                bool isPrimaryController,
                                bool mouseEmulationActive,
                                bool swapFace,
                                GuideChordState& guide,
                                ExitChordState& exitState) {
        const uint32_t b = s.buttons;
        const uint64_t now = static_cast<uint64_t>(sceKernelGetProcessTime());

        const bool touchpadPressed = (b & kRawTouchpad) != 0;
        const bool optionsPressed  = (b & kRawOptions) != 0;
        const bool l3Pressed       = (b & kRawL3) != 0;
        const bool l1Pressed       = (b & kRawL1) != 0;
        const bool r1Pressed       = (b & kRawR1) != 0;

        // Exit chord (Options + L3) and Mouse-assist toggle chord (Options +
        // TouchPad Click) are P1-only chords.
        const bool exitChordHeld = isPrimaryController && optionsPressed && l3Pressed;
        const bool mouseAssistChord = isPrimaryController && optionsPressed && touchpadPressed;

        int flags = 0;
        // Cross/Circle are consumed as mouse buttons when mouse emulation is
        // active; otherwise they map to A/B (swapped if swapFace is set).
        if (!mouseEmulationActive) {
            int flagA = swapFace ? kFlagCircle : kFlagCross;
            int flagB = swapFace ? kFlagCross : kFlagCircle;
            if (b & 0x00004000u) flags |= flagA;      // ORBIS_PAD_BUTTON_CROSS
            if (b & 0x00002000u) flags |= flagB;      // ORBIS_PAD_BUTTON_CIRCLE
        }
        if (b & 0x00008000u) flags |= kFlagSquare;     // ORBIS_PAD_BUTTON_SQUARE
        if (b & 0x00001000u) flags |= kFlagTriangle;   // ORBIS_PAD_BUTTON_TRIANGLE
        if (b & 0x00000010u) flags |= kFlagUp;         // ORBIS_PAD_BUTTON_UP
        if (b & 0x00000040u) flags |= kFlagDown;       // ORBIS_PAD_BUTTON_DOWN
        if (b & 0x00000080u) flags |= kFlagLeft;       // ORBIS_PAD_BUTTON_LEFT
        if (b & 0x00000020u) flags |= kFlagRight;      // ORBIS_PAD_BUTTON_RIGHT

        if (l1Pressed) flags |= kFlagL1;               // ORBIS_PAD_BUTTON_L1 → LB
        if (r1Pressed) flags |= kFlagR1;               // ORBIS_PAD_BUTTON_R1 → RB

        // Exit-chord state machine for P1:
        // L3 is always passed through so sprinting with L3 is not broken.
        // When Options+L3 are pressed together on P1, buffer Options during the
        // 1s exit hold window. If Options is released before 1s, emit an 80 ms
        // Options pulse so tapping Options while sprinting still reaches the host.
        bool suppressOptionsForExit = false;
        bool pulseOptionsForExit = false;
        if (isPrimaryController) {
            switch (exitState.phase) {
            case ExitChordState::Phase::Idle:
                if (exitChordHeld && !mouseAssistChord && !(l1Pressed && r1Pressed)) {
                    exitState.phase = ExitChordState::Phase::Buffering;
                    exitState.holdStartUs = now;
                    suppressOptionsForExit = true;
                }
                break;
            case ExitChordState::Phase::Buffering:
                if (exitChordHeld) {
                    suppressOptionsForExit = true;
                    if (now - exitState.holdStartUs >= kExitChordHoldUs) {
                        exitState.phase = ExitChordState::Phase::Drain;
                    }
                } else if (!optionsPressed) {
                    // Options was released before the 1s exit threshold: replay
                    // the buffered Options tap to the host as an 80 ms pulse.
                    exitState.phase = ExitChordState::Phase::Pulse;
                    exitState.pulseReleaseUs = now + kButtonPulseUs;
                    pulseOptionsForExit = true;
                } else {
                    // L3 was released while Options is still held: let Options
                    // pass through normally.
                    exitState.phase = ExitChordState::Phase::Idle;
                }
                break;
            case ExitChordState::Phase::Pulse:
                if (now < exitState.pulseReleaseUs) {
                    pulseOptionsForExit = true;
                } else {
                    exitState.phase = ExitChordState::Phase::Idle;
                }
                break;
            case ExitChordState::Phase::Drain:
            default:
                suppressOptionsForExit = true;
                if (!optionsPressed && !l3Pressed) {
                    exitState.phase = ExitChordState::Phase::Idle;
                }
                break;
            }
        }

        // Options maps directly to Host Options / Start (PLAY_FLAG).
        if ((optionsPressed && !suppressOptionsForExit && !mouseAssistChord) ||
            pulseOptionsForExit) {
            flags |= kFlagOptions;
        }

        // Touchpad click:
        // When mouse emulation is OFF:
        // If clicked on left half (touchX < 960) -> Back/Select (BACK_FLAG).
        // If clicked on right half (touchX >= 960) or no touch -> DS4 Touchpad Button (TOUCHPAD_FLAG).
        if (touchpadPressed && !mouseAssistChord && !mouseEmulationActive) {
            if (s.touchActive && s.touchX < kTouchpadXMidpoint) {
                flags |= kFlagBack;
            } else {
                flags |= kFlagTouchpad;
            }
        }

        // L3 always passes through so sprinting is uninterrupted.
        if (l3Pressed) {
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
        const bool chordHeld = l1Pressed && r1Pressed && optionsPressed;

        switch (guide.phase) {
        case GuideChordState::Phase::Idle:
            if (chordHeld && !touchpadPressed && !exitChordHeld && !mouseAssistChord) {
                // Release previously forwarded buttons before pressing Guide.
                guide.phase = GuideChordState::Phase::Neutral;
                return flags & ~kChordMask;
            }
            break;

        case GuideChordState::Phase::Neutral:
            guide.phase = GuideChordState::Phase::Pulse;
            guide.releaseUs = now + kButtonPulseUs;
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
                        int flags) {
        if (!inputReady) {
            return false;
        }
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
    // Bypasses buildButtonFlags() and resets chord state so a disconnect or
    // stream stop mid-pulse never sends a synthetic button press (e.g. PS/Guide).
    void sendMultiNeutralState(short controllerNumber, uint16_t activeGamepadMask) {
        if (controllerNumber >= 0 && controllerNumber < kMaxControllers) {
            controllers[controllerNumber].guideChord = GuideChordState{};
            controllers[controllerNumber].exitChord = ExitChordState{};
            controllers[controllerNumber].lastSentFlags = 0;
        }
        if (!inputReady) {
            return;
        }
        LiSendMultiControllerEvent(controllerNumber, activeGamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
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
    void releaseEmulatedMouseButtons() {
        if (inputReady) {
            if (heldEmulatedMouseButtons & kRawCross) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            if (heldEmulatedMouseButtons & kRawCircle) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            }
            if (touchLeftClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                touchLeftClickHeld = false;
            }
            if (touchRightClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                touchRightClickHeld = false;
            }
        }
        heldEmulatedMouseButtons = 0;
        touchMode = TouchMode::None;
        trackedFingerIds[0] = trackedFingerIds[1] = 0;
        accPointerX = accPointerY = 0.0f;
        accScrollX = accScrollY = 0.0f;
        twoFingerTravel = 0.0f;
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

        releaseEmulatedMouseButtons();
    }

    // --- Helper: process controller-assisted mouse emulation ---------------
    // Full multi-touch gesture engine:
    // 1 finger moving -> relative mouse with sub-pixel float accumulator
    // 1 finger physical click -> left mouse button
    // 2 fingers landed -> right mouse button on physical click (if travel < threshold)
    // 2 fingers moving -> high-res horizontal/vertical scroll
    // Cross/Circle -> optional auxiliary left/right mouse clicks
    void processMouseEmulation(const ps4::ControllerState& s) {
        if (!inputReady) {
            return;
        }

        const bool physicalClick = (s.buttons & kRawTouchpad) != 0;
        const int fingers = s.touchFingers > 2 ? 2 : s.touchFingers;

        int seen[2] = {0, 0};

        // 1. Update existing tracked contacts
        for (int i = 0; i < fingers; ++i) {
            int id = s.touchPoints[i].id;
            if (id == 0) continue;

            int slot = -1;
            if (trackedFingerIds[0] == id) slot = 0;
            else if (trackedFingerIds[1] == id) slot = 1;
            if (slot < 0) continue;

            seen[slot] = 1;
            int dx = s.touchPoints[i].x - trackedX[slot];
            int dy = s.touchPoints[i].y - trackedY[slot];
            trackedX[slot] = s.touchPoints[i].x;
            trackedY[slot] = s.touchPoints[i].y;

            if (dx == 0 && dy == 0) continue;

            if (touchMode == TouchMode::Pointer) {
                accPointerX += static_cast<float>(dx) * kTouchpadMouseSensitivity;
                accPointerY += static_cast<float>(dy) * kTouchpadMouseSensitivity;
            } else if (touchMode == TouchMode::PendingTwoFinger || touchMode == TouchMode::Scroll) {
                accScrollX += static_cast<float>(dx) * 0.5f;
                accScrollY += static_cast<float>(dy) * 0.5f;
                if (touchMode == TouchMode::PendingTwoFinger) {
                    float adx = dx < 0 ? -static_cast<float>(dx) : static_cast<float>(dx);
                    float ady = dy < 0 ? -static_cast<float>(dy) : static_cast<float>(dy);
                    twoFingerTravel += (adx > ady ? adx : ady);
                }
            }
        }

        // New contacts anchor without jumping
        for (int i = 0; i < fingers; ++i) {
            int id = s.touchPoints[i].id;
            if (id == 0) continue;
            if (trackedFingerIds[0] == id || trackedFingerIds[1] == id) continue;

            int freeSlot = -1;
            if (trackedFingerIds[0] == 0) freeSlot = 0;
            else if (trackedFingerIds[1] == 0) freeSlot = 1;
            if (freeSlot >= 0) {
                trackedFingerIds[freeSlot] = id;
                trackedX[freeSlot] = s.touchPoints[i].x;
                trackedY[freeSlot] = s.touchPoints[i].y;
            }
        }

        // Drop contacts that vanished
        int trackedCount = 0;
        for (int i = 0; i < 2; ++i) {
            if (trackedFingerIds[i] != 0) {
                bool alive = false;
                for (int k = 0; k < fingers; ++k) {
                    if (s.touchPoints[k].id == trackedFingerIds[i]) {
                        alive = true;
                        break;
                    }
                }
                if (!alive) {
                    trackedFingerIds[i] = 0;
                } else {
                    trackedCount++;
                }
            }
        }

        // 2. Mode transitions
        if (trackedCount == 0) {
            if (touchLeftClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                touchLeftClickHeld = false;
            }
            if (touchRightClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                touchRightClickHeld = false;
            }
            touchMode = TouchMode::None;
            twoFingerTravel = 0.0f;
            accScrollX = accScrollY = 0.0f;
        } else if (trackedCount == 1) {
            if (touchMode == TouchMode::PendingTwoFinger || touchMode == TouchMode::Scroll) {
                if (touchRightClickHeld) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                    touchRightClickHeld = false;
                }
                accPointerX = accPointerY = 0.0f;
            }
            touchMode = TouchMode::Pointer;
            twoFingerTravel = 0.0f;
        } else {
            // 2 tracked fingers
            if (touchMode == TouchMode::Pointer || touchMode == TouchMode::None) {
                if (touchLeftClickHeld) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                    touchLeftClickHeld = false;
                }
                touchMode = TouchMode::PendingTwoFinger;
                twoFingerTravel = 0.0f;
                accScrollX = accScrollY = 0.0f;
                accPointerX = accPointerY = 0.0f;
            }
            if (touchMode == TouchMode::PendingTwoFinger && twoFingerTravel >= kScrollLockTravelPx) {
                touchMode = TouchMode::Scroll;
                if (touchRightClickHeld) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                    touchRightClickHeld = false;
                }
            }
        }

        // 3. Emit pointer and scroll
        if (touchMode == TouchMode::Pointer) {
            int ix = static_cast<int>(accPointerX);
            int iy = static_cast<int>(accPointerY);
            if (ix != 0 || iy != 0) {
                accPointerX -= static_cast<float>(ix);
                accPointerY -= static_cast<float>(iy);
                LiSendMouseMoveEvent(static_cast<short>(ix), static_cast<short>(iy));
            }
        } else if (touchMode == TouchMode::Scroll) {
            int vy = static_cast<int>(accScrollY / kScrollPxPerTick * kWheelUnit);
            int vx = static_cast<int>(accScrollX / kScrollPxPerTick * kWheelUnit);
            if (vy != 0) {
                accScrollY -= (static_cast<float>(vy) * kScrollPxPerTick / kWheelUnit);
                // Negative wheel = content scrolls down (fingers moving up)
                LiSendHighResScrollEvent(static_cast<short>(-vy));
            }
            if (vx != 0) {
                accScrollX -= (static_cast<float>(vx) * kScrollPxPerTick / kWheelUnit);
                LiSendHighResHScrollEvent(static_cast<short>(vx));
            }
        }

        // 4. Physical touchpad clicks
        if (touchMode == TouchMode::Pointer) {
            if (physicalClick && !touchLeftClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
                touchLeftClickHeld = true;
            } else if (!physicalClick && touchLeftClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                touchLeftClickHeld = false;
            }
        } else if (touchMode == TouchMode::PendingTwoFinger) {
            // Two fingers resting: right click
            if (physicalClick && !touchRightClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
                touchRightClickHeld = true;
            } else if (!physicalClick && touchRightClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                touchRightClickHeld = false;
            }
        } else if (touchMode == TouchMode::Scroll) {
            // Guard clicks while scrolling
            if (touchLeftClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
                touchLeftClickHeld = false;
            }
            if (touchRightClickHeld) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
                touchRightClickHeld = false;
            }
        }

        // --- Cross/Circle auxiliary buttons (optional) ---------------------
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

    // Helper: poll and stream controllers & motion once
    void pollAndStreamControllers() {
        if (!inputReady || !inputManager) {
            return;
        }

        // Snapshot state for controller 0
        ps4::ControllerState state0;
        bool state0Valid = inputManager->latestControllerState(state0, 0);

        // Options + TouchPad Click on P1 toggles mouse assist
        if (state0Valid) {
            const bool chordHeld =
                (state0.buttons & kRawOptions) && (state0.buttons & kRawTouchpad);
            if (chordHeld && !prevMouseAssistChordHeld) {
                mouseEmulationEnabled = !mouseEmulationEnabled;
                if (!mouseEmulationEnabled) {
                    releaseEmulatedMouseButtons();
                }
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "[input] mouse assist toggled %s",
                         mouseEmulationEnabled ? "ON" : "OFF");
                cisDiagPrint(buf);
            }
            prevMouseAssistChordHeld = chordHeld;
        }

        // Process all controller slots
        for (int i = 0; i < Impl::kMaxControllers; ++i) {
            Impl::ControllerTrack& track = controllers[i];
            ps4::ControllerState state;
            bool isConnected = inputManager->latestControllerState(state, i) &&
                               state.connected;

            // Disconnect detection
            if (track.wasConnected && !isConnected) {
                track.wasConnected = false;
                track.havePreviousState = false;
                track.arrivalSent = false;
                track.framesSinceRefresh = 0;
                uint16_t newMask = computeActiveGamepadMask();
                sendMultiNeutralState(static_cast<short>(i), newMask);
                char buf[96];
                snprintf(buf, sizeof(buf), "[input] controller %d disconnected — neutral sent", i);
                cisDiagPrint(buf);
                continue;
            }

            if (!isConnected) {
                continue;
            }

            // Arrival detection
            bool isNewConnection = !track.wasConnected;
            if (isNewConnection) {
                track.wasConnected = true;
                uint16_t mask = computeActiveGamepadMask();
                if (!track.arrivalSent) {
                    uint16_t caps = LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE |
                                    LI_CCAP_RGB_LED | LI_CCAP_TOUCHPAD;
                    if (gyroMotionEnabled) {
                        caps |= (LI_CCAP_GYRO | LI_CCAP_ACCEL);
                    }
                    LiSendControllerArrivalEvent(static_cast<uint8_t>(i),
                                                 mask,
                                                 LI_CTYPE_PS,
                                                 0xFFFFFFFF,
                                                 caps);
                    LiSendControllerBatteryEvent(static_cast<uint8_t>(i),
                                                 LI_BATTERY_STATE_FULL,
                                                 100);
                    track.arrivalSent = true;
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "[input] controller %d connected/arrived (mask=0x%04X)", i, mask);
                cisDiagPrint(buf);
            }

            uint16_t currentMask = computeActiveGamepadMask();
            const bool isPrimary = (i == 0);
            const bool mouseEmulationActive = isPrimary && mouseEmulationEnabled;

            const int flags = Impl::buildButtonFlags(
                state, isPrimary, mouseEmulationActive, swapFaceButtons,
                track.guideChord, track.exitChord);

            bool changed = !track.havePreviousState || isNewConnection ||
                           (flags != track.lastSentFlags);
            if (track.havePreviousState && !changed) {
                const ps4::ControllerState& p = track.previousState;
                changed = (state.buttons != p.buttons) ||
                          (state.leftStickX != p.leftStickX) ||
                          (state.leftStickY != p.leftStickY) ||
                          (state.rightStickX != p.rightStickX) ||
                          (state.rightStickY != p.rightStickY) ||
                          (state.leftTrigger != p.leftTrigger) ||
                          (state.rightTrigger != p.rightTrigger);
            }

            bool refresh = false;
            if (track.havePreviousState) {
                ++track.framesSinceRefresh;
                // At 250 Hz, 250 ticks ≈ 1 second
                if (track.framesSinceRefresh >= 250) {
                    track.framesSinceRefresh = 0;
                    refresh = true;
                }
            }

            if (changed || refresh) {
                // If Guide chord is pulsing, neutralize sticks and triggers
                ps4::ControllerState outState = state;
                if (track.guideChord.phase == GuideChordState::Phase::Neutral ||
                    track.guideChord.phase == GuideChordState::Phase::Pulse) {
                    outState.leftStickX = outState.leftStickY = 0;
                    outState.rightStickX = outState.rightStickY = 0;
                    outState.leftTrigger = outState.rightTrigger = 0;
                }
                sendMultiState(static_cast<short>(i), currentMask, outState, flags);
                track.previousState = state;
                track.lastSentFlags = flags;
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

            // Stream IMU motion reports if enabled
            if (gyroMotionEnabled && state.hasMotionData) {
                LiSendControllerMotionEvent(static_cast<uint8_t>(i),
                                            LI_MOTION_TYPE_GYRO,
                                            state.gyroX, state.gyroY, state.gyroZ);
                LiSendControllerMotionEvent(static_cast<uint8_t>(i),
                                            LI_MOTION_TYPE_ACCEL,
                                            state.accelX, state.accelY, state.accelZ);
            }
        }

        // Process mouse emulation
        if (mouseEmulationEnabled && state0Valid) {
            processMouseEmulation(state0);
        }
    }

    // Poller thread function
    static void* pollerThreadFunc(void* arg) {
        Impl* impl = static_cast<Impl*>(arg);
        cisDiagPrint("[input] 250 Hz input streaming thread started");
        while (!impl->pollerStop.load()) {
            if (impl->state == ControllerInputStreamingState::Active &&
                impl->inputReady && impl->inputManager) {
                impl->inputManager->poll();
                impl->pollAndStreamControllers();
            }
            sceKernelUsleep(4000); // 4 ms cadence = 250 Hz
        }
        cisDiagPrint("[input] 250 Hz input streaming thread stopped");
        return nullptr;
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

    stop();

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
    m_impl->touchMode = Impl::TouchMode::None;
    m_impl->trackedFingerIds[0] = m_impl->trackedFingerIds[1] = 0;
    m_impl->accPointerX = m_impl->accPointerY = 0.0f;
    m_impl->accScrollX = m_impl->accScrollY = 0.0f;
    m_impl->twoFingerTravel = 0.0f;
    m_impl->touchLeftClickHeld = false;
    m_impl->touchRightClickHeld = false;
    m_impl->heldEmulatedMouseButtons = 0;
    m_impl->prevMouseAssistChordHeld = false;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void ControllerInputStreaming::start(uint32_t sessionGeneration,
                                     bool mouseEmulationEnabled,
                                     bool swapFaceButtons,
                                     bool gyroMotionEnabled) {
    if (!m_impl->initialized) {
        return;
    }

    // Reset per-session state.
    m_impl->sessionGeneration = sessionGeneration;
    m_impl->inputReady = false;
    m_impl->inputReadyLogged = false;
    m_impl->swapFaceButtons = swapFaceButtons;
    m_impl->gyroMotionEnabled = gyroMotionEnabled;
    m_impl->resetControllers();
    m_impl->heldMouseButtons = 0;
    for (int i = 0; i < 256; ++i) {
        m_impl->heldKeys[i] = false;
    }
    m_impl->mouseEmulationEnabled = mouseEmulationEnabled;
    m_impl->heldEmulatedMouseButtons = 0;
    m_impl->prevMouseAssistChordHeld = false;
    m_impl->touchMode = Impl::TouchMode::None;
    m_impl->trackedFingerIds[0] = m_impl->trackedFingerIds[1] = 0;
    m_impl->accPointerX = m_impl->accPointerY = 0.0f;
    m_impl->accScrollX = m_impl->accScrollY = 0.0f;
    m_impl->twoFingerTravel = 0.0f;
    m_impl->touchLeftClickHeld = false;
    m_impl->touchRightClickHeld = false;

    // Start keyboard and mouse reception (only while a stream is active).
    if (m_impl->inputManager) {
        m_impl->inputManager->startKeyboard();
        m_impl->inputManager->startMouse();
    }

    m_impl->state = ControllerInputStreamingState::Active;

    // Launch 250 Hz background input streaming poller thread
    m_impl->pollerStop.store(false);
    if (pthread_create(&m_impl->pollerThread, nullptr, Impl::pollerThreadFunc, m_impl) == 0) {
        m_impl->pollerRunning = true;
    } else {
        cisDiagPrint("[input] warning: failed to create 250 Hz poller thread, falling back to 60 Hz");
        m_impl->pollerRunning = false;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[input] controller streaming start generation=%u mouseEmulation=%d swapFace=%d gyro=%d 250Hz=%d",
             static_cast<unsigned>(sessionGeneration),
             mouseEmulationEnabled ? 1 : 0,
             swapFaceButtons ? 1 : 0,
             gyroMotionEnabled ? 1 : 0,
             m_impl->pollerRunning ? 1 : 0);
    cisDiagPrint(buf);
}

void ControllerInputStreaming::stop() {
    if (!m_impl->initialized) {
        return;
    }

    // Stop 250 Hz poller thread first
    if (m_impl->pollerRunning) {
        m_impl->pollerStop.store(true);
        pthread_join(m_impl->pollerThread, nullptr);
        m_impl->pollerRunning = false;
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
    if (ssm->currentSessionGeneration() != m_impl->sessionGeneration) {
        return;
    }

    // --- Input-readiness gating ---------------------------------------------
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

    // If 250 Hz background poller is NOT running, stream on the main thread
    if (!m_impl->pollerRunning) {
        m_impl->pollAndStreamControllers();
    }

    // Keyboard/mouse forwarding runs on the main thread
    if (m_impl->inputManager) {
        m_impl->inputManager->pollKeyboard();
        ps4::KeyboardEvent keyEvents[16];
        int nKeys = m_impl->inputManager->takeKeyboardEvents(keyEvents, 16);
        for (int i = 0; i < nKeys; ++i) {
            m_impl->sendKeyboardEvent(keyEvents[i]);
        }

        m_impl->inputManager->pollMouse();
        ps4::MouseEvent mouseEvents[16];
        int nMice = m_impl->inputManager->takeMouseEvents(mouseEvents, 16);
        for (int i = 0; i < nMice; ++i) {
            m_impl->sendMouseEvent(mouseEvents[i]);
        }
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

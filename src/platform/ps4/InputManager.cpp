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

// === InputManager.cpp ========================================================
//
// Moonlight client for PlayStation 4 — PS4 Input subsystem
//
// Implements the InputManager concrete PS4 controller input subsystem:
//   - User service initialization and initial-user query.
//   - Pad library initialization and controller open.
//   - Per-frame controller polling.
//   - Previous/current button state tracking.
//   - Edge detection (rising-edge just-pressed accessors).
//   - Keyboard reception via the PS4 IME keyboard API.
//   - Mouse reception via the PS4 Mouse library.
//   - Deterministic shutdown.
//
// The OpenOrbis SDK headers for Ime/Mouse/ConvertKeycode are stubs: they
// declare the functions but define no types or constants. The required SDK
// structs/constants are therefore defined locally in this translation unit,
// matching the SCE reference layouts. This keeps SDK types out of public
// headers.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "InputManager.h"

// --- OpenOrbis SDK headers (private to this translation unit) ---------------
#include <orbis/Pad.h>                 // scePadInit, scePadOpen, scePadReadState, scePadClose
#include <orbis/UserService.h>         // sceUserServiceInitialize, sceUserServiceGetInitialUser
#include <orbis/SystemService.h>       // sceSystemServiceParamGetInt, ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN
#include <orbis/libkernel.h>           // SCE_OK

#include <cstdio>                // printf, fflush, snprintf
#include <cstring>               // memset
#include <cstdint>               // INT16_MIN, INT16_MAX

// ---------------------------------------------------------------------------
// Local OpenOrbis SDK type/constant definitions
// ---------------------------------------------------------------------------
// The OpenOrbis Ime.h / Mouse.h / ConvertKeycode.h headers are stubs. Define
// the exact structs, enums, and constants used by the keyboard/mouse
// passthrough here, matching the SCE reference layouts (see
// _reference/moonlight-ps4-SCE-RC/_docs/developer/Input_Output_Devices/).

// --- IME keyboard types/constants -------------------------------------------

/// Keyboard layout type (SceImeKeyboardType). Only the values we pass through
/// are needed; the full enum is defined in the SDK.
enum LocalImeKeyboardType : uint32_t {
    kImeKeyboardTypeNone = 0
};

/// Keyboard reception options (SCE_IME_KEYBOARD_OPTION_*).
enum LocalImeKeyboardOption : uint32_t {
    kImeKeyboardOptionDefault = 0x00000000
};

/// Keyboard event IDs (SCE_IME_KEYBOARD_EVENT_*).
enum LocalImeKeyboardEventId : uint32_t {
    kImeKeyboardEventKeycodeDown = 257,
    kImeKeyboardEventKeycodeUp   = 258,
    kImeKeyboardEventKeycodeRepeat = 259
};

/// Key status modifier bits (SCE_IME_KEYCODE_STATE_MODIFIER_*).
enum LocalImeKeycodeState : uint32_t {
    kImeKeycodeStateModifierLCtrl  = 0x00000100,
    kImeKeycodeStateModifierLShift = 0x00000200,
    kImeKeycodeStateModifierLAlt   = 0x00000400,
    kImeKeycodeStateModifierLGui   = 0x00000800,
    kImeKeycodeStateModifierRCtrl  = 0x00001000,
    kImeKeycodeStateModifierRShift = 0x00002000,
    kImeKeycodeStateModifierRAlt   = 0x00004000,
    kImeKeycodeStateModifierRGui   = 0x00008000
};

/// Key code information (SceImeKeycode).
///
/// Layout note (matching the OpenOrbis SDK's byte convention, consistent with
/// how ImeDialog.cpp packs UTF-16 code units): the `character` field is a
/// single UTF-16 code unit (2 bytes), NOT a 32-bit wchar_t. This keeps the
/// `type` field at offset 8 (not 12). If `character` were declared as a
/// 32-bit wchar_t, `type` would be misread as the `userId` field (0 for the
/// system user), which would make sceConvertKeycodeGetVirtualKeycode fail and
/// fall back to raw USB HID usage codes — producing the "numeric keyboard"
/// bug (e.g. `;`→3, `'`→4).
struct LocalImeKeycode {
    uint16_t keycode;
    uint16_t character;
    uint32_t status;
    uint32_t type;
    int32_t  userId;
    uint32_t resourceId;
    uint64_t timestamp;
};

/// Event parameter union (SceImeEventParam). Only the keycode member is used.
union LocalImeEventParam {
    LocalImeKeycode keycode;
    int8_t reserved[64];
};

/// Event information (SceImeEvent).
struct LocalImeEvent {
    uint32_t id;
    LocalImeEventParam param;
};

/// Keyboard reception parameters (SceImeKeyboardParam).
struct LocalImeKeyboardParam {
    uint32_t option;
    int8_t   reserved1[4];
    void*    arg;
    void*    handler;
    int8_t   reserved2[8];
};

/// IME event handler callback type (SceImeEventHandler).
typedef void (*LocalImeEventHandler)(void* arg, const LocalImeEvent* e);

// --- Mouse types/constants ---------------------------------------------------

/// Mouse button flags (SCE_MOUSE_BUTTON_*).
enum LocalMouseButton : uint32_t {
    kMouseButtonPrimary    = 0x00000001,
    kMouseButtonSecondary  = 0x00000002,
    kMouseButtonOptional   = 0x00000004,
    kMouseButtonOptional2  = 0x00000008,
    kMouseButtonOptional3  = 0x00000010,
    kMouseButtonIntercepted = 0x80000000
};

/// Mouse open behavior flags (SCE_MOUSE_OPEN_PARAM_*).
enum LocalMouseOpenParamFlag : uint8_t {
    kMouseOpenParamNormal = 0x00,
    kMouseOpenParamMerged = 0x01
};

/// Mouse port type (SCE_MOUSE_PORT_TYPE_STANDARD).
enum LocalMousePortType : int32_t {
    kMousePortTypeStandard = 0
};

/// Mouse output data (SceMouseData).
struct LocalMouseData {
    uint64_t timestamp;
    bool     connected;
    uint32_t buttons;
    int32_t  xAxis;
    int32_t  yAxis;
    int32_t  wheel;
    int32_t  tilt;
    uint8_t  reserve[8];
};

/// Mouse open parameters (SceMouseOpenParam).
struct LocalMouseOpenParam {
    uint8_t behaviorFlag;
    uint8_t reserve[7];
};

// --- Real function prototypes (the OpenOrbis stubs declare these as void) ---
extern "C" {
int sceImeKeyboardOpen(int32_t userId, const LocalImeKeyboardParam* param);
int sceImeKeyboardClose(int32_t userId);
int sceImeUpdate(LocalImeEventHandler handler);
int sceMouseInit(void);
int sceMouseOpen(int32_t userId, int32_t type, int32_t index,
                 LocalMouseOpenParam* pParam);
int sceMouseRead(int32_t handle, LocalMouseData* pData, int32_t num);
int sceMouseClose(int32_t handle);
int sceConvertKeycodeGetVirtualKeycode(uint16_t keycode, uint32_t keyboardType,
                                       uint16_t* vkeycode);
}

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with the Application Shell)
// ---------------------------------------------------------------------------

static void inputDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of InputManager. Holds all OpenOrbis SDK pad state.
struct InputManager::Impl {
    // --- Controller slots ---------------------------------------------------
    struct PadSlot {
        int32_t userId = -1;
        int32_t padHandle = -1;
        bool    connected = false;
        ControllerState latestState;
    };

    static constexpr int kSlotCount = InputManager::kMaxControllers;
    PadSlot slots[kSlotCount];
    uint32_t pollCounter = 0;
    bool    initialized = false;

    // --- Button state (combined across connected pads for UI navigation) ---
    uint32_t currentButtons = 0; ///< Current frame button bitmask.
    uint32_t previousButtons = 0; ///< Previous frame button bitmask.

    // --- Enter button assignment -------------------------------------------
    /// True when Cross is the "accept/enter" button (Western convention).
    /// Queried from the system via sceSystemServiceParamGetInt at
    /// initialization; defaults to true (Cross) if the query is unavailable.
    bool crossIsAccept = true;

    // --- Query logged in users and manage pad handles -----------------------
    void updatePadHandles() {
        OrbisUserServiceLoginUserIdList userList;
        memset(&userList, 0, sizeof(userList));
        int32_t rc = sceUserServiceGetLoginUserIdList(&userList);
        if (rc != 0) {
            return;
        }

        // Populate or maintain slots 1..kSlotCount-1 for secondary users
        for (int u = 0; u < 4; ++u) {
            int32_t uid = userList.userId[u];
            if (uid <= 0 || uid == slots[0].userId) {
                continue;
            }

            // Check if this uid is already in an open slot
            bool alreadyTracked = false;
            for (int i = 1; i < kSlotCount; ++i) {
                if (slots[i].userId == uid && slots[i].padHandle >= 0) {
                    alreadyTracked = true;
                    break;
                }
            }
            if (alreadyTracked) {
                continue;
            }

            // Find an empty slot
            for (int i = 1; i < kSlotCount; ++i) {
                if (slots[i].padHandle < 0) {
                    int32_t handle = scePadOpen(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
                    if (handle >= 0) {
                        slots[i].userId = uid;
                        slots[i].padHandle = handle;
                        slots[i].connected = false;
                        slots[i].latestState = ControllerState();
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[input] opened pad handle %d for user 0x%08X in slot %d",
                                 handle, uid, i);
                        inputDiagPrint(buf);
                    }
                    break;
                }
            }
        }

        // Check if any tracked secondary user has logged out
        for (int i = 1; i < kSlotCount; ++i) {
            if (slots[i].padHandle >= 0) {
                bool stillLogged = false;
                for (int u = 0; u < 4; ++u) {
                    if (userList.userId[u] == slots[i].userId) {
                        stillLogged = true;
                        break;
                    }
                }
                if (!stillLogged) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[input] user 0x%08X logged out, closing pad handle %d in slot %d",
                             slots[i].userId, slots[i].padHandle, i);
                    inputDiagPrint(buf);
                    scePadClose(slots[i].padHandle);
                    slots[i].padHandle = -1;
                    slots[i].userId = -1;
                    slots[i].connected = false;
                    slots[i].latestState = ControllerState();
                }
            }
        }
    }

    // --- Keyboard (IME) state ------------------------------------------------
    bool    keyboardActive = false;   ///< Whether IME keyboard reception is active.
    bool    keyboardOpen = false;     ///< Whether the IME keyboard is open.
    // True while keyboard reception is requested but sceImeKeyboardOpen has not
    // yet succeeded (e.g. the keyboard was not connected at stream launch).
    // pollKeyboard() retries sceImeKeyboardOpen until it succeeds, so a
    // keyboard plugged in after stream launch is picked up automatically.
    bool    keyboardOpenPending = false;
    // Bounded keyboard event queue (fixed-size ring buffer).
    KeyboardEvent keyboardQueue[64];
    int     keyboardQueueHead = 0;    ///< Next read index.
    int     keyboardQueueTail = 0;    ///< Next write index.
    int     keyboardQueueCount = 0;   ///< Number of pending events.

    // --- Mouse state ---------------------------------------------------------
    bool    mouseActive = false;      ///< Whether mouse reception is active.
    int32_t mouseHandle = -1;         ///< Open mouse handle (negative = not open).
    // Bounded mouse event queue (fixed-size ring buffer).
    MouseEvent mouseQueue[64];
    int     mouseQueueHead = 0;       ///< Next read index.
    int     mouseQueueTail = 0;       ///< Next write index.
    int     mouseQueueCount = 0;      ///< Number of pending events.
};

// ---------------------------------------------------------------------------
// IME keyboard event handler
// ---------------------------------------------------------------------------

/// Translate the IME key status modifier bits into Moonlight MODIFIER_* flags.
static uint8_t imeStatusToModifiers(uint32_t status) {
    uint8_t mods = 0;
    if (status & kImeKeycodeStateModifierLShift) mods |= 0x01; // MODIFIER_SHIFT
    if (status & kImeKeycodeStateModifierRShift) mods |= 0x01; // MODIFIER_SHIFT
    if (status & kImeKeycodeStateModifierLCtrl)  mods |= 0x02; // MODIFIER_CTRL
    if (status & kImeKeycodeStateModifierRCtrl)  mods |= 0x02; // MODIFIER_CTRL
    if (status & kImeKeycodeStateModifierLAlt)   mods |= 0x04; // MODIFIER_ALT
    if (status & kImeKeycodeStateModifierRAlt)   mods |= 0x04; // MODIFIER_ALT
    if (status & kImeKeycodeStateModifierLGui)   mods |= 0x08; // MODIFIER_META
    if (status & kImeKeycodeStateModifierRGui)   mods |= 0x08; // MODIFIER_META
    return mods;
}

/// IME keyboard event callback. Enqueues key down/up/repeat events into the
/// InputManager's bounded keyboard queue. Declared a friend of InputManager in
/// the header so it can access the private Impl.
void imeKeyboardHandler(void* arg, const SceImeEvent* e) {
    InputManager::Impl* impl = static_cast<InputManager::Impl*>(arg);
    if (!impl || !e) {
        return;
    }

    // The OpenOrbis stub declares SceImeEvent as opaque; reinterpret it as the
    // locally-defined layout (identical to the SCE reference).
    const LocalImeEvent* le = reinterpret_cast<const LocalImeEvent*>(e);

    KeyAction action;
    switch (le->id) {
        case kImeKeyboardEventKeycodeDown:
            action = KeyAction::Down;
            break;
        case kImeKeyboardEventKeycodeUp:
            action = KeyAction::Up;
            break;
        case kImeKeyboardEventKeycodeRepeat:
            action = KeyAction::Repeat;
            break;
        default:
            // Ignore non-key events (open/connection/disconnection/abort).
            return;
    }

    const LocalImeKeycode& kc = le->param.keycode;
    if (impl->keyboardQueueCount >= 64) {
        // Queue full: drop the oldest event to make room.
        impl->keyboardQueueHead = (impl->keyboardQueueHead + 1) % 64;
        --impl->keyboardQueueCount;
    }

    KeyboardEvent& ev = impl->keyboardQueue[impl->keyboardQueueTail];
    ev.keycode = kc.keycode;
    ev.action = action;
    ev.modifiers = imeStatusToModifiers(kc.status);
    ev.keyboardType = kc.type;

    impl->keyboardQueueTail = (impl->keyboardQueueTail + 1) % 64;
    ++impl->keyboardQueueCount;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

InputManager::InputManager()
    : m_impl(new Impl())
{
}

InputManager::~InputManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

InputResult InputManager::initialize() {
    if (m_impl->initialized) {
        return InputResult(InputResultCategory::AlreadyInitialized);
    }

    inputDiagPrint("[input] initialization start");

    // 1. Initialize the User Service so we can query the launching user.
    OrbisUserServiceInitializeParams initParams;
    memset(&initParams, 0, sizeof(initParams));
    initParams.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    int32_t rc = sceUserServiceInitialize(&initParams);
    if (rc != 0) {
        inputDiagPrint("[input] user service init failed");
        return InputResult(InputResultCategory::UserServiceInitFailure,
                           static_cast<uint32_t>(rc));
    }

    // 2. Get the ID of the user who launched the application.
    int32_t userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
    rc = sceUserServiceGetInitialUser(&userId);
    if (rc != 0) {
        inputDiagPrint("[input] initial user query failed");
        return InputResult(InputResultCategory::InitialUserQueryFailure,
                           static_cast<uint32_t>(rc));
    }

    // 2.5. Query the system's Enter Button Assignment so the app's UI
    //      accept/back mapping matches the system ImeDialog behavior.
    //      ORBIS_SYSTEM_PARAM_ENTER_BUTTON_ASSIGN_CIRCLE (0) → Circle is enter.
    //      ORBIS_SYSTEM_PARAM_ENTER_BUTTON_ASSIGN_CROSS  (1) → Cross is enter.
    m_impl->crossIsAccept = true;   // Default to Cross=accept (Western).
    {
        int32_t enterButton = 0;
        int32_t prc = sceSystemServiceParamGetInt(
            ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN, &enterButton);
        if (prc == 0) {
            m_impl->crossIsAccept =
                (enterButton == ORBIS_SYSTEM_PARAM_ENTER_BUTTON_ASSIGN_CROSS);
        }
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[input] enter button assign: %s (rc=%d)",
                 m_impl->crossIsAccept ? "Cross" : "Circle",
                 static_cast<int>(prc));
        inputDiagPrint(buf);
    }

    // 3. Initialize the Pad library.
    rc = scePadInit();
    if (rc != 0) {
        inputDiagPrint("[input] pad init failed");
        return InputResult(InputResultCategory::PadInitFailure,
                           static_cast<uint32_t>(rc));
    }

    // 4. Initialize slots and open the controller handle for the launching user in slot 0.
    for (int i = 0; i < Impl::kSlotCount; ++i) {
        m_impl->slots[i].userId = -1;
        m_impl->slots[i].padHandle = -1;
        m_impl->slots[i].connected = false;
        m_impl->slots[i].latestState = ControllerState();
    }

    m_impl->slots[0].userId = userId;
    m_impl->slots[0].padHandle = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (m_impl->slots[0].padHandle < 0) {
        inputDiagPrint("[input] primary pad open failed");
        return InputResult(InputResultCategory::PadOpenFailure,
                           static_cast<uint32_t>(m_impl->slots[0].padHandle));
    }

    // 4.5. Check for any additional already-logged-in users and open their pads.
    m_impl->updatePadHandles();

    // 5. Initialize button state.
    m_impl->currentButtons = 0;
    m_impl->previousButtons = 0;
    m_impl->pollCounter = 0;

    m_impl->initialized = true;
    inputDiagPrint("[input] initialization success");
    return InputResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void InputManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Stop keyboard and mouse reception first (release their resources).
    stopKeyboard();
    stopMouse();

    for (int i = 0; i < Impl::kSlotCount; ++i) {
        if (m_impl->slots[i].padHandle >= 0) {
            scePadClose(m_impl->slots[i].padHandle);
            m_impl->slots[i].padHandle = -1;
        }
        m_impl->slots[i].userId = -1;
        m_impl->slots[i].connected = false;
        m_impl->slots[i].latestState = ControllerState();
    }

    m_impl->currentButtons = 0;
    m_impl->previousButtons = 0;
    m_impl->pollCounter = 0;
    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool InputManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Analog conversion helpers
// ---------------------------------------------------------------------------

/// Convert an unsigned PS4 analog axis (0..255, center 128) to the signed
/// Moonlight range (-32768..32767, center 0). Uses an int32_t intermediate to
/// avoid int16 overflow. When `invertY` is true, the result is inverted so
/// that PS4 "up" (low raw value) maps to Moonlight "up" (positive).
static int16_t convertAxis(uint8_t raw, bool invertY) {
    int32_t v = (static_cast<int32_t>(raw) - 128) * 257;  // -32896..32639
    if (v > 32767) {
        v = 32767;
    }
    if (v < -32768) {
        v = -32768;
    }
    int16_t out = static_cast<int16_t>(v);
    if (invertY) {
        // Invert Y: up (positive) <-> down (negative). Avoid overflow on
        // INT16_MIN (which has no positive counterpart).
        out = (out == INT16_MIN) ? INT16_MAX : static_cast<int16_t>(-out);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

void InputManager::poll() {
    if (!m_impl->initialized) {
        return;
    }

    // Shift current to previous for UI edge detection.
    m_impl->previousButtons = m_impl->currentButtons;

    // Periodically (every 60 frames, ~1s) scan for logged-in users / new controllers
    m_impl->pollCounter++;
    if ((m_impl->pollCounter % 60) == 0) {
        m_impl->updatePadHandles();
    }

    uint32_t combinedButtons = 0;

    for (int i = 0; i < Impl::kSlotCount; ++i) {
        Impl::PadSlot& slot = m_impl->slots[i];
        if (slot.padHandle < 0) {
            continue;
        }

        OrbisPadData padData;
        memset(&padData, 0, sizeof(padData));

        int32_t rc = scePadReadState(slot.padHandle, &padData);
        if (rc != 0) {
            slot.connected = false;
            slot.latestState = ControllerState();
            continue;
        }

        slot.connected = (padData.connected != 0);
        ControllerState& s = slot.latestState;
        s.buttons = padData.buttons;
        s.leftStickX  = convertAxis(padData.leftStick.x, false);
        s.leftStickY  = convertAxis(padData.leftStick.y, true);
        s.rightStickX = convertAxis(padData.rightStick.x, false);
        s.rightStickY = convertAxis(padData.rightStick.y, true);
        s.leftTrigger  = padData.analogButtons.l2;
        s.rightTrigger = padData.analogButtons.r2;
        s.connected = (padData.connected != 0);

        s.touchActive = (padData.touch.fingers > 0);
        s.touchFingers = padData.touch.fingers;
        if (s.touchActive) {
            s.touchX = static_cast<int16_t>(padData.touch.touch[0].x);
            s.touchY = static_cast<int16_t>(padData.touch.touch[0].y);
        } else {
            s.touchX = 0;
            s.touchY = 0;
        }

        if (slot.connected) {
            combinedButtons |= padData.buttons;
        }
    }

    m_impl->currentButtons = combinedButtons;
}

// ---------------------------------------------------------------------------
// Controller snapshot and connectivity accessors
// ---------------------------------------------------------------------------

bool InputManager::latestControllerState(ControllerState& out, int controllerIndex) const {
    if (!m_impl || !m_impl->initialized || controllerIndex < 0 || controllerIndex >= Impl::kSlotCount) {
        out = ControllerState();
        return false;
    }
    const Impl::PadSlot& slot = m_impl->slots[controllerIndex];
    if (slot.padHandle < 0 || !slot.connected) {
        out = ControllerState();
        return false;
    }
    out = slot.latestState;
    return true;
}

bool InputManager::isControllerConnected(int controllerIndex) const {
    if (!m_impl || !m_impl->initialized || controllerIndex < 0 || controllerIndex >= Impl::kSlotCount) {
        return false;
    }
    return m_impl->slots[controllerIndex].padHandle >= 0 && m_impl->slots[controllerIndex].connected;
}

int InputManager::connectedControllerCount() const {
    if (!m_impl || !m_impl->initialized) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < Impl::kSlotCount; ++i) {
        if (m_impl->slots[i].padHandle >= 0 && m_impl->slots[i].connected) {
            count++;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Keyboard (PS4 IME keyboard API)
// ---------------------------------------------------------------------------

bool InputManager::startKeyboard() {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }
    if (m_impl->keyboardActive) {
        return true;  // already active
    }

    // The LIBIME and CONVERT_KEYCODE sysmodules are loaded at process startup
    // by SysmoduleManager. No need to load them here.

    // Attempt to open the IME keyboard for the launching user. If it fails
    // (e.g. the keyboard is not connected yet), mark the open as pending so
    // pollKeyboard() retries until it succeeds. This lets a keyboard plugged
    // in after stream launch be picked up automatically.
    m_impl->keyboardOpenPending = true;
    if (tryOpenKeyboard()) {
        m_impl->keyboardOpenPending = false;
        return true;
    }
    inputDiagPrint("[input] sceImeKeyboardOpen failed — will retry");
    return false;
}

/// Attempt to open the IME keyboard. Returns true on success. On failure the
/// caller decides whether to retry. Shared by startKeyboard() and the
/// pollKeyboard() retry path.
bool InputManager::tryOpenKeyboard() {
    if (!m_impl || m_impl->keyboardOpen) {
        return m_impl && m_impl->keyboardOpen;
    }

    int32_t userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
    sceUserServiceGetInitialUser(&userId);

    LocalImeKeyboardParam param;
    memset(&param, 0, sizeof(param));
    param.option = kImeKeyboardOptionDefault;
    param.arg = m_impl;
    param.handler = reinterpret_cast<void*>(imeKeyboardHandler);

    int rc = sceImeKeyboardOpen(userId, &param);
    if (rc != 0) {
        return false;
    }

    m_impl->keyboardOpen = true;
    m_impl->keyboardQueueHead = 0;
    m_impl->keyboardQueueTail = 0;
    m_impl->keyboardQueueCount = 0;
    m_impl->keyboardActive = true;
    inputDiagPrint("[input] keyboard started");
    return true;
}

void InputManager::stopKeyboard() {
    if (!m_impl) {
        return;
    }
    if (m_impl->keyboardOpen) {
        int32_t userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
        sceUserServiceGetInitialUser(&userId);
        sceImeKeyboardClose(userId);
        m_impl->keyboardOpen = false;
    }
    m_impl->keyboardOpenPending = false;
    m_impl->keyboardQueueHead = 0;
    m_impl->keyboardQueueTail = 0;
    m_impl->keyboardQueueCount = 0;
    m_impl->keyboardActive = false;
}

bool InputManager::isKeyboardActive() const {
    return m_impl && m_impl->keyboardActive;
}

void InputManager::pollKeyboard() {
    if (!m_impl || !m_impl->keyboardActive) {
        return;
    }

    // If the keyboard open is still pending (the keyboard was not connected
    // when startKeyboard() ran), retry sceImeKeyboardOpen until it succeeds.
    // This lets a keyboard plugged in after stream launch be picked up
    // automatically, per the IME library's recommended retry pattern.
    if (m_impl->keyboardOpenPending) {
        if (tryOpenKeyboard()) {
            m_impl->keyboardOpenPending = false;
        } else {
            return;   // Still not connected; retry next frame.
        }
    }

    // Dispatch any pending IME keyboard events into the queue via the handler.
    sceImeUpdate(reinterpret_cast<LocalImeEventHandler>(imeKeyboardHandler));
}

int InputManager::takeKeyboardEvents(KeyboardEvent* out, int max) {
    if (!m_impl || !out || max <= 0) {
        return 0;
    }
    int n = 0;
    while (n < max && m_impl->keyboardQueueCount > 0) {
        out[n] = m_impl->keyboardQueue[m_impl->keyboardQueueHead];
        m_impl->keyboardQueueHead = (m_impl->keyboardQueueHead + 1) % 64;
        --m_impl->keyboardQueueCount;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Mouse (PS4 Mouse library)
// ---------------------------------------------------------------------------

bool InputManager::startMouse() {
    if (!m_impl || !m_impl->initialized) {
        return false;
    }
    if (m_impl->mouseActive) {
        return true;  // already active
    }

    // The MOUSE sysmodule is loaded at process startup by SysmoduleManager.
    // No need to load it here.

    // Initialize the Mouse library.
    int rc = sceMouseInit();
    if (rc != 0) {
        inputDiagPrint("[input] sceMouseInit failed");
        return false;
    }

    // Open the mouse port in merged mode (handles gaming keyboards with mouse
    // functionality plus a separate mouse).
    int32_t userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
    sceUserServiceGetInitialUser(&userId);

    LocalMouseOpenParam param;
    memset(&param, 0, sizeof(param));
    param.behaviorFlag = kMouseOpenParamMerged;

    m_impl->mouseHandle = sceMouseOpen(userId, kMousePortTypeStandard, 0, &param);
    if (m_impl->mouseHandle < 0) {
        inputDiagPrint("[input] sceMouseOpen failed");
        m_impl->mouseHandle = -1;
        return false;
    }

    m_impl->mouseQueueHead = 0;
    m_impl->mouseQueueTail = 0;
    m_impl->mouseQueueCount = 0;
    m_impl->mouseActive = true;
    inputDiagPrint("[input] mouse started");
    return true;
}

void InputManager::stopMouse() {
    if (!m_impl) {
        return;
    }
    if (m_impl->mouseHandle >= 0) {
        sceMouseClose(m_impl->mouseHandle);
        m_impl->mouseHandle = -1;
    }
    m_impl->mouseQueueHead = 0;
    m_impl->mouseQueueTail = 0;
    m_impl->mouseQueueCount = 0;
    m_impl->mouseActive = false;
}

bool InputManager::isMouseActive() const {
    return m_impl && m_impl->mouseActive;
}

void InputManager::pollMouse() {
    if (!m_impl || !m_impl->mouseActive || m_impl->mouseHandle < 0) {
        return;
    }

    LocalMouseData data[8];
    int n = sceMouseRead(m_impl->mouseHandle, data, 8);
    if (n <= 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {
        const LocalMouseData& d = data[i];

        // Skip intercepted data (system software is using the mouse).
        if (d.buttons & kMouseButtonIntercepted) {
            continue;
        }

        if (m_impl->mouseQueueCount >= 64) {
            // Queue full: drop the oldest event to make room.
            m_impl->mouseQueueHead = (m_impl->mouseQueueHead + 1) % 64;
            --m_impl->mouseQueueCount;
        }

        MouseEvent& ev = m_impl->mouseQueue[m_impl->mouseQueueTail];
        ev.deltaX = d.xAxis;
        ev.deltaY = d.yAxis;
        ev.wheel = d.wheel;
        ev.tilt = d.tilt;
        ev.buttons = d.buttons;
        ev.connected = d.connected;

        m_impl->mouseQueueTail = (m_impl->mouseQueueTail + 1) % 64;
        ++m_impl->mouseQueueCount;
    }
}

int InputManager::takeMouseEvents(MouseEvent* out, int max) {
    if (!m_impl || !out || max <= 0) {
        return 0;
    }
    int n = 0;
    while (n < max && m_impl->mouseQueueCount > 0) {
        out[n] = m_impl->mouseQueue[m_impl->mouseQueueHead];
        m_impl->mouseQueueHead = (m_impl->mouseQueueHead + 1) % 64;
        --m_impl->mouseQueueCount;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Edge detection helpers
// ---------------------------------------------------------------------------

/// Returns true if the given button mask is set in the current state but was
/// not set in the previous state (rising edge).
static bool isJustPressed(uint32_t current, uint32_t previous, uint32_t mask) {
    return (current & mask) != 0 && (previous & mask) == 0;
}

// ---------------------------------------------------------------------------
// Normalized UI input accessors
// ---------------------------------------------------------------------------

bool InputManager::isUpJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_UP);
}

bool InputManager::isDownJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_DOWN);
}

bool InputManager::isLeftJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_LEFT);
}

bool InputManager::isRightJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_RIGHT);
}

bool InputManager::isAcceptJustPressed() const {
    // Respect the system's Enter Button Assignment so the app's UI accept
    // mapping matches the system ImeDialog behavior.
    const uint32_t acceptButton =
        m_impl->crossIsAccept ? ORBIS_PAD_BUTTON_CROSS : ORBIS_PAD_BUTTON_CIRCLE;
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         acceptButton);
}

bool InputManager::isBackJustPressed() const {
    // The "back" button is the opposite of the system's enter button.
    const uint32_t backButton =
        m_impl->crossIsAccept ? ORBIS_PAD_BUTTON_CIRCLE : ORBIS_PAD_BUTTON_CROSS;
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         backButton);
}

bool InputManager::isTriangleJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_TRIANGLE);
}

bool InputManager::isOptionsJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_OPTIONS);
}

bool InputManager::isSquareJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_SQUARE);
}

bool InputManager::isTouchpadJustPressed() const {
    return isJustPressed(m_impl->currentButtons, m_impl->previousButtons,
                         ORBIS_PAD_BUTTON_TOUCH_PAD);
}

bool InputManager::isCrossAccept() const {
    return m_impl && m_impl->crossIsAccept;
}

} // namespace ps4

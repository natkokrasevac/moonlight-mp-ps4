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

// === ImeDialog.cpp ===========================================================
//
// OpenOrbis Moonlight client — PS4 ImeDialog helper
//
// Implements the small PS4 ImeDialog wrapper:
//   - Opens the system OSK for a single text field.
//   - Polls the dialog lifecycle as part of the normal application update.
//   - Retrieves the accepted/cancelled result.
//   - Terminates the dialog cleanly.
//
// The IME_DIALOG sysmodule is loaded at process startup by SysmoduleManager.
// ImeDialog does NOT own sysmodule loading.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "ImeDialog.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/ImeDialog.h>     // OrbisImeDialogSetting, sceImeDialog*
#include <orbis/UserService.h>   // sceUserServiceGetInitialUser, ORBIS_USER_SERVICE_USER_ID_SYSTEM
#include <orbis/libkernel.h>     // SCE_OK

#include <cstdio>              // printf, fflush, snprintf
#include <cstring>             // memset, wcslen
#include <cwchar>              // wcsncpy
#include <vector>              // std::vector (UTF-16 code unit buffer)

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other platform modules)
// ---------------------------------------------------------------------------

static void imeDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 buffer conversion helpers (private to this translation unit)
// ---------------------------------------------------------------------------
// The OpenOrbis SDK's sceImeDialog stores the input text in the wchar_t buffer
// as UTF-16 code units. wchar_t is 32-bit on this toolchain, so each wchar_t
// element holds TWO UTF-16 code units (low 16 bits first, high 16 bits
// second). We therefore pack/unpack two UTF-16 code units per wchar_t element.

namespace {

/// Convert a UTF-8 string to UTF-16 code units packed two per wchar_t element
/// (low 16 bits first). `out` must have capacity for `outCapacity` wchar_t
/// values (including the null terminator). Returns the number of wchar_t
/// written (excluding the null terminator), or 0 on overflow.
size_t utf8ToBuffer(const std::string& in, wchar_t* out, size_t outCapacity) {
    if (out == nullptr || outCapacity == 0) {
        return 0;
    }
    // Decode the UTF-8 string into UTF-16 code units.
    std::vector<uint16_t> units;
    units.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { return 0; }
        if (i + len > n) { return 0; }
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { return 0; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += len;
        if (cp < 0x10000) {
            units.push_back(static_cast<uint16_t>(cp));
        } else {
            cp -= 0x10000;
            units.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            units.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }

    // Pack two UTF-16 code units per wchar_t element.
    const size_t needed = (units.size() + 1) / 2 + 1;
    if (needed > outCapacity) {
        return 0;   // Overflow.
    }
    size_t o = 0;
    size_t u = 0;
    while (u < units.size()) {
        const uint16_t lo = units[u++];
        uint16_t hi = 0;
        if (u < units.size()) {
            hi = units[u++];
        }
        out[o++] = static_cast<wchar_t>((static_cast<uint32_t>(hi) << 16) | lo);
    }
    out[o] = L'\0';
    return o;
}

/// Append a Unicode code point to a UTF-8 string.
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Read UTF-16 code units from the wchar_t buffer (two per wchar_t element,
/// low 16 bits first) and convert to a UTF-8 string. Returns the UTF-8 string
/// (empty on invalid input).
std::string bufferToUtf8(const wchar_t* in) {
    std::string out;
    if (in == nullptr) {
        return out;
    }
    for (size_t i = 0; in[i] != L'\0'; ++i) {
        const uint32_t v = static_cast<uint32_t>(in[i]);
        const uint16_t lo = static_cast<uint16_t>(v & 0xFFFF);
        const uint16_t hi = static_cast<uint16_t>((v >> 16) & 0xFFFF);
        // Process the low UTF-16 code unit.
        uint32_t cp = lo;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: combine with the following low surrogate.
            // The low surrogate is the low 16 bits of the NEXT wchar_t element.
            if (i + 1 < 2049) {
                const uint32_t nextV = static_cast<uint32_t>(in[i + 1]);
                const uint16_t nextLo = static_cast<uint16_t>(nextV & 0xFFFF);
                if (nextLo >= 0xDC00 && nextLo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (nextLo - 0xDC00);
                }
            }
        }
        appendUtf8(out, cp);
        // Process the high UTF-16 code unit (if non-zero).
        if (hi != 0) {
            appendUtf8(out, hi);
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Local IME language / option constants
// ---------------------------------------------------------------------------
// The OpenOrbis ImeDialog header does not define the SCE_IME_LANGUAGE_* or
// SCE_IME_OPTION_* constants (they live in the SCE reference libime.h). Define the
// exact values used here, matching the SCE reference (see
// _reference/moonlight-ps4-SCE-RC/_docs/developer/Input_Output_Devices/Keyboards/).

namespace {

/// SCE_IME_LANGUAGE_* bit values (from the SCE reference).
constexpr uint64_t kImeLanguageEnglishUs = 0x0000000000000004ULL;  // English (US)
constexpr uint64_t kImeLanguageEnglishGb = 0x0000000000040000ULL;  // English (UK)
constexpr uint64_t kImeLanguageJapanese  = 0x0000000000002000ULL;  // Japanese

/// SCE_IME_OPTION_LANGUAGES_FORCED (from the SCE reference).
constexpr uint32_t kImeOptionLanguagesForced = 0x00000008U;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of ImeDialog. Holds all OpenOrbis SDK ImeDialog state.
struct ImeDialog::Impl {
    bool active = false;            ///< A dialog session is open.
    bool resultReady = false;       ///< A result is pending delivery.
    ImeDialogResult pendingResult;  ///< Captured result awaiting delivery.

    // The input text buffer must remain allocated while the dialog runs.
    // Sized generously (max text length + null terminator).
    wchar_t inputBuffer[2048 + 1];

    // The title buffer is kept as a member so it remains valid for the
    // lifetime of the dialog (defensive; the SDK may retain the pointer).
    wchar_t titleBuffer[256 + 1];
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ImeDialog::ImeDialog()
    : m_impl(new Impl())
{
}

ImeDialog::~ImeDialog() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

bool ImeDialog::open(const std::string& initialText, const std::string& title,
                     uint32_t maxLength, bool numericOnly, bool crossIsAccept) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->active) {
        return false;   // A dialog is already active.
    }

    // The IME_DIALOG sysmodule is loaded at process startup by
    // SysmoduleManager. No need to load it here.

    // 1. Prepare the input text buffer (UTF-8 bytes, one per wchar_t element).
    memset(m_impl->inputBuffer, 0, sizeof(m_impl->inputBuffer));
    utf8ToBuffer(initialText, m_impl->inputBuffer,
                 sizeof(m_impl->inputBuffer) / sizeof(wchar_t));

    // 2. Prepare the title buffer (UTF-8 bytes). Stored as a member so it
    //    remains valid for the lifetime of the dialog.
    memset(m_impl->titleBuffer, 0, sizeof(m_impl->titleBuffer));
    utf8ToBuffer(title, m_impl->titleBuffer,
                 sizeof(m_impl->titleBuffer) / sizeof(wchar_t));

    // 3. Get the logged-in user's ID (matching InputManager).
    int32_t userId = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
    int userRc = sceUserServiceGetInitialUser(&userId);
    {
        char ubuf[128];
        snprintf(ubuf, sizeof(ubuf),
                 "[ime] initial user query rc=%d userId=%d",
                 userRc, static_cast<int>(userId));
        imeDiagPrint(ubuf);
    }

    // 4. Initialize the dialog parameters.
    //    The OpenOrbis SDK does not provide sceImeDialogParamInit(), so the
    //    struct is zeroed and every field is set explicitly.
    OrbisImeDialogSetting param;
    memset(&param, 0, sizeof(param));
    param.userId = static_cast<uint32_t>(userId);
    param.type = numericOnly ? ORBIS_TYPE_NUMBER : ORBIS_TYPE_BASIC_LATIN;
    param.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    param.inputMethod = ORBIS__DEFAULT;
    param.option = 0;
    param.maxTextLength = maxLength;
    param.inputTextBuffer = m_impl->inputBuffer;
    param.title = m_impl->titleBuffer;

    // 4b. Position the OSK dialog. Centered horizontally (posx = 960) and
    //     vertically (posy = 540) on the 1920x1080 display to ensure the
    //     keyboard is fully within the screen boundaries.
    param.posx = 960.0f;
    param.posy = 540.0f;
    param.horizontalAlignment = ORBIS_H_CENTER;
    param.verticalAlignment = ORBIS_V_CENTER;

    // 5. Select the OSK language set so the confirm/back button mapping
    //    follows the system's Enter Button Assignment.
    //    - Cross=accept (Western): force English-only so the OSK uses the
    //      Western engine (Cross=confirm).
    //    - Circle=accept (Japanese): select Japanese so the OSK uses the
    //      Japanese engine (Circle=confirm).
    //    SCE_IME_OPTION_LANGUAGES_FORCED makes the specified languages the
    //    only usable ones, overriding the system's "Keyboards and User
    //    Dictionary" default.
    if (crossIsAccept) {
        param.supportedLanguages = kImeLanguageEnglishUs | kImeLanguageEnglishGb;
        param.option = kImeOptionLanguagesForced;
    } else {
        param.supportedLanguages = kImeLanguageJapanese;
        param.option = kImeOptionLanguagesForced;
    }

    // 6. Initialize and run the dialog.
    int rc = sceImeDialogInit(&param, nullptr);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[ime] sceImeDialogInit failed rc=0x%08X", static_cast<unsigned>(rc));
        imeDiagPrint(buf);
        return false;
    }

    m_impl->active = true;
    m_impl->resultReady = false;
    m_impl->pendingResult = ImeDialogResult();
    return true;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool ImeDialog::isActive() const {
    return m_impl && m_impl->active;
}

// ---------------------------------------------------------------------------
// Update (poll lifecycle)
// ---------------------------------------------------------------------------

bool ImeDialog::update(ImeDialogResult& outResult) {
    if (!m_impl || !m_impl->active) {
        return false;
    }

    // If a result was already captured (e.g., after abort), deliver it once.
    if (m_impl->resultReady) {
        outResult = m_impl->pendingResult;
        m_impl->resultReady = false;
        m_impl->active = false;
        return true;
    }

    // Poll the dialog status.
    // OpenOrbis maps the SCE_IME_DIALOG_STATUS_* constants to OrbisDialogStatus:
    //   NONE=0, RUNNING=1, STOPPED=2 (STOPPED == SCE FINISHED).
    OrbisDialogStatus status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_RUNNING) {
        // Still displayed; keep polling.
        return false;
    }

    if (status == ORBIS_DIALOG_STATUS_STOPPED) {
        // Dialog closed. Retrieve the result.
        OrbisDialogResult result;
        memset(&result, 0, sizeof(result));
        int rc = sceImeDialogGetResult(&result);
        if (rc == 0 && result.endstatus == ORBIS_DIALOG_OK) {
            m_impl->pendingResult.accepted = true;
            m_impl->pendingResult.text = bufferToUtf8(m_impl->inputBuffer);
        } else {
            // User cancelled or aborted.
            m_impl->pendingResult.accepted = false;
            m_impl->pendingResult.text.clear();
        }

        // Terminate the dialog (required after STOPPED/FINISHED).
        sceImeDialogTerm();

        // Deliver the result.
        outResult = m_impl->pendingResult;
        m_impl->pendingResult = ImeDialogResult();
        m_impl->active = false;
        return true;
    }

    // ORBIS_DIALOG_STATUS_NONE (should not happen while active).
    return false;
}

// ---------------------------------------------------------------------------
// Abort
// ---------------------------------------------------------------------------

void ImeDialog::abort() {
    if (!m_impl || !m_impl->active) {
        return;
    }

    // Abort the dialog if it is still running.
    OrbisDialogStatus status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_RUNNING) {
        sceImeDialogAbort();
    }

    // After abort, the status transitions to STOPPED/FINISHED. Terminate it.
    status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_STOPPED) {
        sceImeDialogTerm();
    }

    m_impl->active = false;
    m_impl->resultReady = false;
    m_impl->pendingResult = ImeDialogResult();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void ImeDialog::shutdown() {
    if (!m_impl) {
        return;
    }

    // Abort any active dialog so no system dialog is left active while the
    // application tears down its platform managers.
    abort();
}

} // namespace ps4

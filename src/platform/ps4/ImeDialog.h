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

// === ImeDialog.h =============================================================
//
// OpenOrbis Moonlight client — PS4 ImeDialog helper
//
// A small PS4-specific wrapper around the system ImeDialog library. It opens
// the PS4 on-screen keyboard (OSK) for a single text field, polls the dialog
// lifecycle as part of the normal application update, and returns the accepted
// or cancelled text to the caller.
//
// This helper is intentionally minimal: it is not a generic dialog framework.
// It owns only the state required for one ImeDialog session at a time.
//
// This header exposes only project-owned types. OpenOrbis SDK types (OrbisImeDialog*,
// OrbisUserServiceUserId, wchar_t buffers) are kept private to the
// implementation file.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef PS4_IME_DIALOG_H
#define PS4_IME_DIALOG_H

#include <cstdint>
#include <string>

namespace ps4 {

// ---------------------------------------------------------------------------
// ImeDialogResult
// ---------------------------------------------------------------------------

/// The outcome of one ImeDialog session.
struct ImeDialogResult {
    bool accepted = false;   ///< True if the user confirmed input (end status OK).
    std::string text;        ///< Returned UTF-8 text (valid when accepted).
};

// ---------------------------------------------------------------------------
// ImeDialog — small PS4 ImeDialog wrapper
// ---------------------------------------------------------------------------

/// Owns one ImeDialog session lifecycle.
///
/// Responsibilities:
///   - Open the system OSK for a single text field.
///   - Poll the dialog status as part of the normal application update.
///   - Retrieve the accepted/cancelled result.
///   - Terminate the dialog cleanly.
///
/// This class is main-thread owned. It must not block the main thread in a
/// busy loop; the caller polls update() once per frame.
class ImeDialog {
public:
    /// Constructor performs no fallible operations.
    ImeDialog();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~ImeDialog();

    // Non-copyable, non-movable.
    ImeDialog(const ImeDialog&) = delete;
    ImeDialog& operator=(const ImeDialog&) = delete;
    ImeDialog(ImeDialog&&) = delete;
    ImeDialog& operator=(ImeDialog&&) = delete;

    /// Open the ImeDialog for a single text field.
    ///
    /// `initialText` is the UTF-8 text pre-populated in the field.
    /// `title` is the UTF-8 title shown in the dialog title bar.
    /// `maxLength` is the maximum number of characters (UTF-16 code units)
    /// the user may enter.
    /// `numericOnly` restricts input to numbers, periods, commas, and hyphens
    /// (ORBIS_TYPE_NUMBER) when true; otherwise a broader alphanumeric mode
    /// is used.
    /// `crossIsAccept` is the system's Enter Button Assignment. When true
    /// (Cross=accept), the OSK is forced to a Western-only language set so
    /// the confirm button is Cross; when false (Circle=accept), the Japanese
    /// engine is selected so the confirm button is Circle. This keeps the OSK
    /// confirm/back mapping consistent with the rest of the application UI.
    ///
    /// Returns true if the dialog was successfully opened. If the sysmodule
    /// could not be loaded or the dialog could not be initialized, returns
    /// false and no dialog is shown.
    bool open(const std::string& initialText, const std::string& title,
              uint32_t maxLength, bool numericOnly, bool crossIsAccept);

    /// Returns true while an ImeDialog session is active (opened and not yet
    /// terminated).
    bool isActive() const;

    /// Poll the ImeDialog lifecycle. Must be called once per frame while
    /// active. When the dialog closes, the result is captured and the dialog
    /// is terminated. Returns true if a result became available this call.
    bool update(ImeDialogResult& outResult);

    /// Abort an active ImeDialog session immediately (e.g., on shutdown).
    /// Safe to call when no dialog is active.
    void abort();

    /// Terminate any active ImeDialog session and release resources.
    /// Safe after partial initialization or repeated calls (idempotent).
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_IME_DIALOG_H

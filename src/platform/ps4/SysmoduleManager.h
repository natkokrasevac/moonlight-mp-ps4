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

// === SysmoduleManager.h =====================================================
//
// OpenOrbis Moonlight client — PS4 Platform Sysmodule Lifecycle
//
// SysmoduleManager is the concrete PS4 platform owner of the process-level
// PS4 runtime sysmodule lifecycle. It is the SOLE owner of
// sceSysmoduleLoadModule*() / sceSysmoduleUnloadModule*(). No other class
// calls these APIs.
//
// It loads the sysmodules required by application subsystems at process
// startup, tracks which loads succeeded, and unloads them in reverse
// dependency order at shutdown. Subsystems (FontRasterizer, NetworkManager,
// HttpClient, InputManager, PairingStore) consume the loaded services but do
// NOT own module loading.
//
// All PS4 sysmodule loading/unloading is centralized here. Subsystems
// initialize only their runtime API/context state.
//
// This header exposes only project-owned state and capability-style checks.
// Raw PS4 sysmodule constants and handles are kept private to the
// implementation file.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// No OpenOrbis SDK types are exposed.
// ===============================================================================

#ifndef PS4_SYSMODULE_MANAGER_H
#define PS4_SYSMODULE_MANAGER_H

namespace ps4 {

// ---------------------------------------------------------------------------
// SysmoduleManager — concrete PS4 platform sysmodule lifecycle owner
// ---------------------------------------------------------------------------

/// Owns the PS4 runtime sysmodule load/unload lifecycle.
///
/// Responsibilities:
///   - Load the PS4 sysmodules required by application subsystems at process
///     startup (FreeType, NET, HTTP, SSL, RANDOM).
///   - Track which loads succeeded so shutdown only unloads what was loaded.
///   - Unload modules in reverse dependency order at shutdown.
///   - Fail initialization if a required module cannot be loaded.
///
/// This class is main-thread owned. No worker thread is created. It performs
/// no network, graphics, input, HTTP, font, or dialog work; it only owns the
/// sysmodule lifecycle.
class SysmoduleManager {
public:
    /// Constructor performs no fallible operations. The object is safe to
    /// destroy at any point before initialize() is called.
    SysmoduleManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~SysmoduleManager();

    // Non-copyable, non-movable.
    SysmoduleManager(const SysmoduleManager&) = delete;
    SysmoduleManager& operator=(const SysmoduleManager&) = delete;
    SysmoduleManager(SysmoduleManager&&) = delete;
    SysmoduleManager& operator=(SysmoduleManager&&) = delete;

    /// Load the PS4 runtime sysmodules required by application subsystems.
    /// Must be called exactly once, before any dependent subsystem uses the
    /// loaded services. Returns true on success. On failure, already-loaded
    /// modules are cleaned up in reverse order and false is returned. After
    /// failure the object remains safe to shut down or destroy.
    bool initialize();

    /// Unload all successfully-loaded sysmodules in reverse dependency order.
    /// Safe after full initialization, partial initialization, failed
    /// initialization, or repeated calls (idempotent). Must not throw.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    // --- Capability checks --------------------------------------------------
    // Subsystems query these to determine whether a required module is
    // available. Raw module IDs are never exposed.

    /// True if the IME keyboard sysmodule (used by InputManager keyboard
    /// passthrough) is loaded.
    bool hasIme() const;

    /// True if the IME_DIALOG sysmodule (used by ImeDialog for the system
    /// on-screen keyboard) is loaded.
    bool hasImeDialog() const;

    /// True if the mouse sysmodule (used by InputManager mouse passthrough)
    /// is loaded.
    bool hasMouse() const;

    /// True if the keycode-conversion sysmodule (used by InputManager to
    /// convert USB HID keycodes to Windows VK codes) is loaded.
    bool hasConvertKeycode() const;

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ps4

#endif // PS4_SYSMODULE_MANAGER_H

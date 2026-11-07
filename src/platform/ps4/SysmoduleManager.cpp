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

// === SysmoduleManager.cpp ===================================================
//
// OpenOrbis Moonlight client — PS4 Platform Sysmodule Lifecycle
//
// Implements the SysmoduleManager concrete PS4 platform sysmodule lifecycle
// owner:
//   - Loads the PS4 runtime sysmodules required by application subsystems at
//     process startup (FreeType, NET, HTTP, SSL, RANDOM).
//   - Tracks which loads succeeded so shutdown only unloads what was loaded.
//   - Unloads modules in reverse dependency order at shutdown.
//   - Fails initialization if a required module cannot be loaded.
//
// This is the SOLE owner of sceSysmoduleLoadModule*() /
// sceSysmoduleUnloadModule*(). No other class calls these APIs.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "SysmoduleManager.h"

// --- OpenOrbis SDK headers (private to this translation unit) -----------------------
#include <orbis/Sysmodule.h>   // sceSysmoduleLoadModule, sceSysmoduleUnloadModule,
                               // sceSysmoduleLoadModuleInternal,
                               // sceSysmoduleUnloadModuleInternal,
                               // ORBIS_SYSMODULE_*, ORBIS_SYSMODULE_INTERNAL_*

#include <cstdio>              // printf, fflush, snprintf

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other platform modules)
// ---------------------------------------------------------------------------

static void sysDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ps4 {

/// Private implementation of SysmoduleManager. Holds all PS4 sysmodule load
/// state.
struct SysmoduleManager::Impl {
    bool initialized = false;   ///< Full initialize() succeeded.

    // Per-module load state (only unload what was successfully loaded).
    bool freetypeLoaded = false;   ///< ORBIS_SYSMODULE_FREETYPE_OL.
    bool netLoaded      = false;   ///< ORBIS_SYSMODULE_INTERNAL_NET.
    bool httpLoaded     = false;   ///< ORBIS_SYSMODULE_INTERNAL_HTTP.
    bool sslLoaded      = false;   ///< ORBIS_SYSMODULE_INTERNAL_SSL.
    bool randomLoaded   = false;   ///< ORBIS_SYSMODULE_RANDOM.
    bool vdeccoreLoaded = false;   ///< ORBIS_SYSMODULE_INTERNAL_VDECCORE (Videodec2).
    bool audiooutLoaded = false;   ///< ORBIS_SYSMODULE_INTERNAL_AUDIOOUT (AudioOut).
    bool libimeLoaded   = false;   ///< ORBIS_SYSMODULE_LIBIME (IME keyboard).
    bool imeDialogLoaded = false;  ///< ORBIS_SYSMODULE_IME_DIALOG (system OSK).
    bool convertKeycodeLoaded = false; ///< ORBIS_SYSMODULE_CONVERT_KEYCODE.
    bool mouseLoaded    = false;   ///< ORBIS_SYSMODULE_MOUSE.
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SysmoduleManager::SysmoduleManager()
    : m_impl(new Impl())
{
}

SysmoduleManager::~SysmoduleManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool SysmoduleManager::initialize() {
    if (m_impl->initialized) {
        return true;
    }

    sysDiagPrint("[sysmodule] initialization start");

    // Load modules in dependency order. Each module is required; if any load
    // fails, clean up already-loaded modules in reverse order and return
    // false.

    // 1. FreeType (used by FontRasterizer).
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] freetype load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->freetypeLoaded = true;
        sysDiagPrint("[sysmodule] freetype loaded");
    }

    // 2. NET (used by NetworkManager and HttpClient).
    {
        uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] net load failed: 0x%08X", rc);
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->netLoaded = true;
        sysDiagPrint("[sysmodule] net loaded");
    }

    // 3. HTTP (used by HttpClient).
    {
        uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] http load failed: 0x%08X", rc);
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->httpLoaded = true;
        sysDiagPrint("[sysmodule] http loaded");
    }

    // 4. SSL (used by HttpClient).
    {
        uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] ssl load failed: 0x%08X", rc);
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->sslLoaded = true;
        sysDiagPrint("[sysmodule] ssl loaded");
    }

    // 5. RANDOM (used by future PairingStore for sceRandomGetRandomNumber).
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] random load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->randomLoaded = true;
        sysDiagPrint("[sysmodule] random loaded");
    }

    // 6. VDECCORE (used by VideoDecoder for Videodec2 hardware H.264 decode).
    //    This is the internal VdecCore sysmodule required by libSceVideodec2.
    //    The libSceVideodec2.sprx itself is loaded dynamically by VideoDecoder
    //    via sceKernelLoadStartModule (the OpenOrbis SDK does not link it).
    {
        uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] vdeccore load failed: 0x%08X", rc);
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->vdeccoreLoaded = true;
        sysDiagPrint("[sysmodule] vdeccore loaded");
    }

    // 7. AUDIOOUT (used by AudioDecodeOutput for PS4 AudioOut playback).
    //    This is the internal AudioOut sysmodule required by sceAudioOut*.
    {
        uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] audioout load failed: 0x%08X", rc);
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->audiooutLoaded = true;
        sysDiagPrint("[sysmodule] audioout loaded");
    }

    // 8. LIBIME (used by InputManager for keyboard passthrough via the IME
    //    keyboard API). Loaded at process startup so it is available before
    //    any stream begins, and kept resident for the application lifetime.
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_LIBIME);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] libime load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->libimeLoaded = true;
        sysDiagPrint("[sysmodule] libime loaded");
    }

    // 8.5. IME_DIALOG (used by ImeDialog for the system on-screen keyboard).
    //      Loaded at process startup so the OSK is available when the Add Host
    //      overlay requests text entry. Non-fatal: if the load fails, the
    //      ImeDialog::open() call degrades gracefully (returns false and the
    //      Add Host overlay is not left stuck).
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] ime_dialog load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            // Non-fatal: continue without the OSK.
        } else {
            m_impl->imeDialogLoaded = true;
            sysDiagPrint("[sysmodule] ime_dialog loaded");
        }
    }

    // 9. CONVERT_KEYCODE (used by InputManager to convert USB HID keycodes to
    //    Windows VK codes for keyboard passthrough). Loaded at process startup
    //    and kept resident.
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_CONVERT_KEYCODE);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] convert_keycode load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->convertKeycodeLoaded = true;
        sysDiagPrint("[sysmodule] convert_keycode loaded");
    }

    // 10. MOUSE (used by InputManager for mouse passthrough via the Mouse
    //     library). Loaded at process startup and kept resident.
    {
        int32_t rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
        if (rc != 0) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "[sysmodule] mouse load failed: 0x%08X",
                     static_cast<uint32_t>(rc));
            sysDiagPrint(buf);
            shutdown();
            return false;
        }
        m_impl->mouseLoaded = true;
        sysDiagPrint("[sysmodule] mouse loaded");
    }

    m_impl->initialized = true;
    sysDiagPrint("[sysmodule] initialization success");
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void SysmoduleManager::shutdown() {
    if (!m_impl) {
        return;
    }

    // Unload in reverse dependency order. Only unload modules that were
    // successfully loaded. Idempotent: safe after full initialization,
    // partial initialization, failed initialization, or repeated calls.

    // MOUSE (loaded last, unloaded first).
    if (m_impl->mouseLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_MOUSE);
        m_impl->mouseLoaded = false;
    }

    // CONVERT_KEYCODE.
    if (m_impl->convertKeycodeLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_CONVERT_KEYCODE);
        m_impl->convertKeycodeLoaded = false;
    }

    // IME_DIALOG (loaded after LIBIME; unloaded before it).
    if (m_impl->imeDialogLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
        m_impl->imeDialogLoaded = false;
    }

    // LIBIME.
    if (m_impl->libimeLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_LIBIME);
        m_impl->libimeLoaded = false;
    }

    // AUDIOOUT.
    if (m_impl->audiooutLoaded) {
        sceSysmoduleUnloadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
        m_impl->audiooutLoaded = false;
    }

    // VDECCORE.
    if (m_impl->vdeccoreLoaded) {
        sceSysmoduleUnloadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
        m_impl->vdeccoreLoaded = false;
    }

    // RANDOM.
    if (m_impl->randomLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_RANDOM);
        m_impl->randomLoaded = false;
    }

    // SSL.
    if (m_impl->sslLoaded) {
        sceSysmoduleUnloadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
        m_impl->sslLoaded = false;
    }

    // HTTP.
    if (m_impl->httpLoaded) {
        sceSysmoduleUnloadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
        m_impl->httpLoaded = false;
    }

    // NET.
    if (m_impl->netLoaded) {
        sceSysmoduleUnloadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
        m_impl->netLoaded = false;
    }

    // FreeType (loaded first, unloaded last).
    if (m_impl->freetypeLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_FREETYPE_OL);
        m_impl->freetypeLoaded = false;
    }

    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool SysmoduleManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

// ---------------------------------------------------------------------------
// Capability checks
// ---------------------------------------------------------------------------

bool SysmoduleManager::hasIme() const {
    return m_impl && m_impl->libimeLoaded;
}

bool SysmoduleManager::hasImeDialog() const {
    return m_impl && m_impl->imeDialogLoaded;
}

bool SysmoduleManager::hasMouse() const {
    return m_impl && m_impl->mouseLoaded;
}

bool SysmoduleManager::hasConvertKeycode() const {
    return m_impl && m_impl->convertKeycodeLoaded;
}

} // namespace ps4

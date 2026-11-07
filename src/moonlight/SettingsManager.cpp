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

// === SettingsManager.cpp ====================================================
//
// Moonlight client for PlayStation 4 — Persistent Stream Settings
//
// Implements the SettingsManager concrete persistence owner:
//   - Reads/writes a simple readable text file under /data/moonlight/.
//   - Uses standard C library file I/O (fopen, fgets, fprintf, etc.).
//   - Falls back to safe defaults on missing/invalid values.
//   - Ignores unknown keys.
//   - Uses a temp-file + rename for atomic replacement on save.
//
// ===============================================================================

#include "SettingsManager.h"
#include "StoragePaths.h"

#include <cstdio>                // fopen, fclose, fgets, fprintf, remove, rename
#include <cstdlib>               // strtol
#include <cstring>               // strncmp, strlen, strchr
#include <string>

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other modules)
// ---------------------------------------------------------------------------

static void smDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// Settings file path.
constexpr const char* kSettingsFilePath = ml::storage::kSettingsFilePath;

/// Temp file path used for atomic replacement on save.
constexpr const char* kSettingsTempPath = ml::storage::kSettingsTempPath;

/// Maximum line length for a settings file line.
constexpr size_t kMaxLineLen = 256;

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of SettingsManager.
struct SettingsManager::Impl {
    bool initialized = false;
    Settings settings;
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SettingsManager::SettingsManager()
    : m_impl(new Impl())
{
}

SettingsManager::~SettingsManager() {
    if (m_impl) {
        shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool SettingsManager::initialize() {
    if (m_impl->initialized) {
        return true;
    }

    // Start from safe defaults.
    m_impl->settings = Settings();

    // Open the settings file. A missing file on first run is not an error;
    // defaults are used.
    FILE* f = fopen(kSettingsFilePath, "r");
    if (f == nullptr) {
        // No file yet — use defaults.
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[settings] loaded: 1080p 60fps quitOnExit=0 (defaults, no file)");
        smDiagPrint(buf);
        m_impl->initialized = true;
        return true;
    }

    // Parse each line as key=value. Unknown keys are ignored. Invalid values
    // fall back to the default for that key.
    char line[kMaxLineLen];
    while (fgets(line, sizeof(line), f) != nullptr) {
        // Trim trailing newline.
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;  // skip blank lines
        }

        // Split on '='.
        char* eq = strchr(line, '=');
        if (eq == nullptr) {
            continue;  // malformed line — ignore
        }
        *eq = '\0';
        const char* key = line;
        const char* value = eq + 1;

        // Trim leading/trailing whitespace on the key.
        while (*key == ' ' || *key == '\t') {
            ++key;
        }
        size_t keyLen = strlen(key);
        while (keyLen > 0 && (key[keyLen - 1] == ' ' || key[keyLen - 1] == '\t')) {
            --keyLen;
        }

        if (keyLen == 0) {
            continue;
        }

        // --- resolution -----------------------------------------------------
        if (strncmp(key, "resolution", keyLen) == 0 && keyLen == 10) {
            long v = strtol(value, nullptr, 10);
            if (v == 720) {
                m_impl->settings.resolution = StreamResolution::R720p;
            } else if (v == 1080) {
                m_impl->settings.resolution = StreamResolution::R1080p;
            } else if (v == 2160) {
                m_impl->settings.resolution = StreamResolution::R4K;
            }
            // Invalid value → keep default (1080p).
            continue;
        }

        // --- framerate ------------------------------------------------------
        if (strncmp(key, "framerate", keyLen) == 0 && keyLen == 9) {
            long v = strtol(value, nullptr, 10);
            if (v == 30) {
                m_impl->settings.frameRate = StreamFrameRate::Fps30;
            } else if (v == 60) {
                m_impl->settings.frameRate = StreamFrameRate::Fps60;
            }
            // Invalid value → keep default (60 FPS).
            continue;
        }

        // --- bitrate --------------------------------------------------------
        // Optional key. Missing key → default (20 Mbps). Known values: 5, 10,
        // 20, 40, 80 (the standard Moonlight client presets). Unknown/corrupt
        // value → default (20 Mbps). Never fails settings loading. A value
        // from the previous preset set (14/50/135/240) is treated as unknown
        // and falls back to the default; the file is rewritten on next save.
        if (strncmp(key, "bitrate", keyLen) == 0 && keyLen == 7) {
            long v = strtol(value, nullptr, 10);
            if (v == 5) {
                m_impl->settings.bitrate = StreamBitrate::B5;
            } else if (v == 10) {
                m_impl->settings.bitrate = StreamBitrate::B10;
            } else if (v == 20) {
                m_impl->settings.bitrate = StreamBitrate::B20;
            } else if (v == 40) {
                m_impl->settings.bitrate = StreamBitrate::B40;
            } else if (v == 80) {
                m_impl->settings.bitrate = StreamBitrate::B80;
            }
            // Invalid value → keep default (20 Mbps).
            continue;
        }

        // --- codec ----------------------------------------------------------
        // Optional key. Missing key → default (Auto). Known values: 0 = Auto,
        // 1 = H264/AVC, 2 = H265/HEVC. Unknown/corrupt value → default (Auto).
        // Never fails settings loading.
        if (strncmp(key, "codec", keyLen) == 0 && keyLen == 5) {
            long v = strtol(value, nullptr, 10);
            if (v == 1) {
                m_impl->settings.codec = StreamCodec::H264;
            } else if (v == 2) {
                m_impl->settings.codec = StreamCodec::H265;
            } else {
                m_impl->settings.codec = StreamCodec::Auto;
            }
            // Invalid value → keep default (Auto).
            continue;
        }

        // --- quit_app_on_stream_exit ---------------------------------------
        if (strncmp(key, "quit_app_on_stream_exit", keyLen) == 0 &&
            keyLen == 23) {
            long v = strtol(value, nullptr, 10);
            if (v == 0) {
                m_impl->settings.quitAppOnStreamExit = false;
            } else if (v == 1) {
                m_impl->settings.quitAppOnStreamExit = true;
            }
            // Invalid value → keep default (Off).
            continue;
        }

        // --- presentation_style --------------------------------------------
        // Optional key. Missing key → default (PlayStation). Known values:
        // "playstation" or "grid". Unknown/corrupt value → default
        // (PlayStation). Never fails settings loading.
        if (strncmp(key, "presentation_style", keyLen) == 0 &&
            keyLen == 18) {
            if (strncmp(value, "grid", 4) == 0) {
                m_impl->settings.presentationStyle = PresentationStyle::Grid;
            } else if (strncmp(value, "playstation", 11) == 0) {
                m_impl->settings.presentationStyle = PresentationStyle::PlayStation;
            }
            // Invalid value → keep default (PlayStation).
            continue;
        }

        // --- stream_statistics ---------------------------------------------
        // Optional key. Missing key → default (Off). Known values: 0 or 1.
        // Unknown/corrupt value → default (Off). Never fails settings loading.
        if (strncmp(key, "stream_statistics", keyLen) == 0 &&
            keyLen == 17) {
            long v = strtol(value, nullptr, 10);
            if (v == 0) {
                m_impl->settings.streamStatisticsEnabled = false;
            } else if (v == 1) {
                m_impl->settings.streamStatisticsEnabled = true;
            }
            // Invalid value → keep default (Off).
            continue;
        }

        // --- mute_host_speakers --------------------------------------------
        // Optional key. Missing key → default (Muted). Known values: 0 or 1.
        // Unknown/corrupt value → default (Muted). Never fails settings
        // loading.
        if (strncmp(key, "mute_host_speakers", keyLen) == 0 &&
            keyLen == 18) {
            long v = strtol(value, nullptr, 10);
            if (v == 0) {
                m_impl->settings.muteHostSpeakers = false;
            } else if (v == 1) {
                m_impl->settings.muteHostSpeakers = true;
            }
            // Invalid value → keep default (Muted).
            continue;
        }

        // --- optimize_game_settings ----------------------------------------
        // Optional key. Missing key → default (ON). Known values: 0 or 1.
        // Unknown/corrupt value → default (ON). Never fails settings loading.
        if (strncmp(key, "optimize_game_settings", keyLen) == 0 &&
            keyLen == 22) {
            long v = strtol(value, nullptr, 10);
            if (v == 0) {
                m_impl->settings.optimizeGameSettings = false;
            } else if (v == 1) {
                m_impl->settings.optimizeGameSettings = true;
            }
            // Invalid value → keep default (ON).
            continue;
        }

        // --- show_quit_app_dialog ------------------------------------------
        // Optional key. Missing key → default (ON). Known values: 0 or 1.
        // Unknown/corrupt value → default (ON). Never fails settings loading.
        if (strncmp(key, "show_quit_app_dialog", keyLen) == 0 &&
            keyLen == 20) {
            long v = strtol(value, nullptr, 10);
            if (v == 0) {
                m_impl->settings.showQuitAppDialog = false;
            } else if (v == 1) {
                m_impl->settings.showQuitAppDialog = true;
            }
            // Invalid value → keep default (ON).
            continue;
        }

        // Unknown key — ignored.
    }

    fclose(f);

    // Log the loaded settings.
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[settings] loaded: %s %s bitrate=%d codec=%s quitOnExit=%d presentation=%s streamStats=%d muteHost=%d optimize=%d showQuitDialog=%d",
                 m_impl->settings.resolution == StreamResolution::R4K ? "4K" :
                 (m_impl->settings.resolution == StreamResolution::R1080p ? "1080p" : "720p"),
                 m_impl->settings.frameRate == StreamFrameRate::Fps60 ? "60fps" : "30fps",
                 m_impl->settings.bitrate == StreamBitrate::B5 ? 5 :
                 (m_impl->settings.bitrate == StreamBitrate::B10 ? 10 :
                 (m_impl->settings.bitrate == StreamBitrate::B40 ? 40 :
                 (m_impl->settings.bitrate == StreamBitrate::B80 ? 80 : 20))),
                 m_impl->settings.codec == StreamCodec::H264 ? "h264" :
                 (m_impl->settings.codec == StreamCodec::H265 ? "h265" : "auto"),
                 m_impl->settings.quitAppOnStreamExit ? 1 : 0,
                 m_impl->settings.presentationStyle == PresentationStyle::Grid ? "grid" : "playstation",
                 m_impl->settings.streamStatisticsEnabled ? 1 : 0,
                 m_impl->settings.muteHostSpeakers ? 1 : 0,
                 m_impl->settings.optimizeGameSettings ? 1 : 0,
                 m_impl->settings.showQuitAppDialog ? 1 : 0);
        smDiagPrint(buf);
    }

    m_impl->initialized = true;
    return true;
}

void SettingsManager::shutdown() {
    m_impl->initialized = false;
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool SettingsManager::isInitialized() const {
    return m_impl && m_impl->initialized;
}

const Settings& SettingsManager::settings() const {
    return m_impl->settings;
}

void SettingsManager::setSettings(const Settings& settings) {
    m_impl->settings = settings;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool SettingsManager::save() {
    if (!m_impl->initialized) {
        return false;
    }

    // Write to a temp file first, then rename over the target for atomic
    // replacement. This avoids a partially-written settings file.
    FILE* f = fopen(kSettingsTempPath, "w");
    if (f == nullptr) {
        smDiagPrint("[settings] save failed: cannot open temp file");
        return false;
    }

    // Write explicit readable values (not raw enum ordinals).
    const int resolution =
        (m_impl->settings.resolution == StreamResolution::R4K) ? 2160 :
        (m_impl->settings.resolution == StreamResolution::R1080p) ? 1080 : 720;
    const int framerate  = (m_impl->settings.frameRate == StreamFrameRate::Fps60) ? 60 : 30;
    const int bitrate    = (m_impl->settings.bitrate == StreamBitrate::B5) ? 5 :
                           (m_impl->settings.bitrate == StreamBitrate::B10) ? 10 :
                           (m_impl->settings.bitrate == StreamBitrate::B40) ? 40 :
                           (m_impl->settings.bitrate == StreamBitrate::B80) ? 80 : 20;
    const int quitOnExit = m_impl->settings.quitAppOnStreamExit ? 1 : 0;
    const char* presentation =
        (m_impl->settings.presentationStyle == PresentationStyle::Grid)
            ? "grid" : "playstation";
    const int streamStats = m_impl->settings.streamStatisticsEnabled ? 1 : 0;
    const int muteHost = m_impl->settings.muteHostSpeakers ? 1 : 0;
    const int optimize = m_impl->settings.optimizeGameSettings ? 1 : 0;
    const int showQuitDialog = m_impl->settings.showQuitAppDialog ? 1 : 0;
    const int codec = (m_impl->settings.codec == StreamCodec::H264) ? 1 :
                      (m_impl->settings.codec == StreamCodec::H265) ? 2 : 0;

    fprintf(f, "resolution=%d\n", resolution);
    fprintf(f, "framerate=%d\n", framerate);
    fprintf(f, "bitrate=%d\n", bitrate);
    fprintf(f, "codec=%d\n", codec);
    fprintf(f, "quit_app_on_stream_exit=%d\n", quitOnExit);
    fprintf(f, "presentation_style=%s\n", presentation);
    fprintf(f, "stream_statistics=%d\n", streamStats);
    fprintf(f, "mute_host_speakers=%d\n", muteHost);
    fprintf(f, "optimize_game_settings=%d\n", optimize);
    fprintf(f, "show_quit_app_dialog=%d\n", showQuitDialog);

    if (fflush(f) != 0) {
        fclose(f);
        remove(kSettingsTempPath);
        smDiagPrint("[settings] save failed: flush error");
        return false;
    }
    if (fclose(f) != 0) {
        remove(kSettingsTempPath);
        smDiagPrint("[settings] save failed: close error");
        return false;
    }

    // Atomically replace the target file.
    if (rename(kSettingsTempPath, kSettingsFilePath) != 0) {
        remove(kSettingsTempPath);
        smDiagPrint("[settings] save failed: rename error");
        return false;
    }

    smDiagPrint("[settings] saved");
    return true;
}

bool SettingsManager::resetToDefaults() {
    if (!m_impl->initialized) {
        return false;
    }

    // Restore the in-memory settings to their safe defaults.
    m_impl->settings = Settings();

    // Persist the defaults to disk (overwriting the settings file).
    const bool ok = save();
    if (ok) {
        smDiagPrint("[settings] reset to defaults");
    } else {
        smDiagPrint("[settings] reset to defaults failed to persist");
    }
    return ok;
}

} // namespace ml

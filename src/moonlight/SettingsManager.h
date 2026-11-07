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

// === SettingsManager.h ======================================================
//
// OpenOrbis Moonlight client — Persistent Stream Settings
//
// SettingsManager owns the persistent stream settings (resolution, frame rate,
// quit-on-exit). It loads/saves a simple readable text file under
// /data/moonlight/ using standard C library file I/O, and exposes the
// authoritative in-memory values to the rest of the application.
//
// This header exposes only project-owned types. OpenOrbis SDK types, file handles,
// and storage internals must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// =============================================================================

#ifndef MOONLIGHT_SETTINGS_MANAGER_H
#define MOONLIGHT_SETTINGS_MANAGER_H

#include <cstdint>

namespace ml {

// ---------------------------------------------------------------------------
// Settings data model
// ---------------------------------------------------------------------------

/// Stream resolution.
enum class StreamResolution : uint8_t {
    R720p,
    R1080p,
    R4K
};

/// Stream frame rate.
enum class StreamFrameRate : uint8_t {
    Fps30,
    Fps60
};

/// Stream video bitrate (Mbps).
///
/// The presets follow the standard Moonlight client values (5/10/20/40/80
/// Mbps) so this client behaves like the rest of the Moonlight ecosystem.
///
/// NOTE: these values deliberately do NOT sit on the H.264/HEVC Annex A level
/// MaxBR boundaries (which would be 14/20/50/135/240 for AVC High profile).
/// The decoder level is selected independently as the MAXIMUM of a
/// resolution-derived floor and a bitrate-derived requirement (see
/// VideoDecoder.cpp), so the configured level always covers the stream
/// regardless of whether the bitrate sits on a boundary. See
/// src/ui/StreamBitrateLut.h for the full rationale.
enum class StreamBitrate : uint8_t {
    B5,    ///< 5 Mbps.
    B10,   ///< 10 Mbps.
    B20,   ///< 20 Mbps.
    B40,   ///< 40 Mbps.
    B80    ///< 80 Mbps.
};

/// Stream video codec preference.
///
/// The persisted preference is honored only when the console is a PS4 Pro
/// (isPro()): H.265/HEVC decode is only available on Pro hardware. On a
/// non-Pro console (or a non-Pro build), the decoder always falls back to
/// H.264 regardless of this setting (see MoonlightApp::handleActivateApplication).
enum class StreamCodec : uint8_t {
    Auto,   ///< Use H.265/HEVC if the host reports HEVC support AND isPro();
            ///< otherwise H.264/AVC. Default.
    H264,   ///< Always H.264/AVC.
    H265    ///< Always H.265/HEVC (only honored when isPro()).
};

/// Presentation style for the Host List and App List screens.
///
/// This setting chooses only between the two Host/App list presentation
/// modes. It is NOT a theme, skin, layout framework, or UI engine.
/// PlayStation renders a horizontal crossbar; Grid renders the scrollable
/// grid.
enum class PresentationStyle : uint8_t {
    PlayStation,
    Grid
};

/// Authoritative persistent stream settings.
struct Settings {
    StreamResolution resolution = StreamResolution::R1080p;
    StreamFrameRate  frameRate  = StreamFrameRate::Fps60;
    /// Stream video bitrate. Default 20 Mbps (matches the auto-selected
    /// bitrate for the default 1080p60 specs; see StreamBitrateLut.h).
    StreamBitrate    bitrate    = StreamBitrate::B20;
    /// Stream video codec preference. Default Auto.
    StreamCodec      codec      = StreamCodec::Auto;
    bool             quitAppOnStreamExit = false;
    PresentationStyle presentationStyle = PresentationStyle::PlayStation;
    /// When enabled, a non-interactive statistics overlay is shown during an
    /// active stream after the 3-second exit hint expires. Default Off.
    bool             streamStatisticsEnabled = false;
    /// When true, the host's speakers are muted during a stream (no audio
    /// played on the host). Default Muted.
    bool             muteHostSpeakers = true;
    /// When true, GameStream/Sunshine "Game Optimization" (SOPS) is enabled
    /// for the launched application. Default ON.
    bool             optimizeGameSettings = true;
    /// When true, the "Quit App" confirmation dialog is shown before quitting
    /// an app (Triangle) or before quitting-and-launching a different app.
    /// When false, quitting proceeds immediately without a dialog. Default ON.
    bool             showQuitAppDialog = true;
    /// Stick deadzone percentage: 0..25% (default 8%).
    int              stickDeadzonePercent = 8;
    /// Swap Cross and Circle face buttons on the host (Cross -> B, Circle -> A). Default false.
    bool             swapFaceButtons = false;
    /// Enable gyro and accelerometer motion reports to the host. Default true.
    bool             gyroMotionEnabled = true;
};

// ---------------------------------------------------------------------------
// SettingsManager
// ---------------------------------------------------------------------------

/// Owns persistent stream settings.
///
/// Responsibilities:
///   - Load settings from disk once during initialization.
///   - Save settings to disk on demand (when the Settings overlay closes).
///   - Expose the authoritative in-memory settings values.
///   - Fall back to safe defaults on missing/invalid values.
///
/// Non-responsibilities:
///   - Display the Settings UI (UiManager does that).
///   - Apply settings to stream sessions (MoonlightApp does that).
class SettingsManager {
public:
    /// Constructor performs no fallible operations.
    SettingsManager();

    /// Destructor must not throw.
    ~SettingsManager();

    // Non-copyable, non-movable.
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;
    SettingsManager(SettingsManager&&) = delete;
    SettingsManager& operator=(SettingsManager&&) = delete;

    /// Load settings from disk. Falls back to safe defaults on any error
    /// (missing file, absent key, invalid value). Must be called once during
    /// application initialization. Returns true on success (even if defaults
    /// were used); false only on a fatal storage error.
    bool initialize();

    /// Release resources. Idempotent.
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Returns the authoritative in-memory settings.
    const Settings& settings() const;

    /// Replace the authoritative in-memory settings.
    void setSettings(const Settings& settings);

    /// Save the current in-memory settings to disk. Returns true on success.
    bool save();

    /// Reset the in-memory settings to their safe defaults and persist them to
    /// disk (overwriting the settings file). Used by the persistent-storage
    /// reset flow. Returns true on success.
    bool resetToDefaults();

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_SETTINGS_MANAGER_H


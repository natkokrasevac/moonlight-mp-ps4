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

// === UiSoundManager.h ========================================================
//
// Moonlight client for PlayStation 4 — UI Navigation Sound Manager
//
// UiSoundManager is a small, concrete PS4 audio component that plays short
// UI navigation cues (accept / back / cursor) from preloaded WAV assets. It is
// deliberately NOT a general audio engine, mixer, music player, or streaming
// decoder. It exists solely to give lightweight feedback to UI navigation.
//
// Responsibilities:
//   - Preload the three UI WAV assets (accept.wav / back.wav / cursor.wav)
//     from /app0/assets/audio/ into persistent PCM memory once.
//   - Parse the minimal RIFF/WAVE format required by those assets.
//   - Own a dedicated PS4 AudioOut port for UI cues, logically separate from
//     the stream audio output.
//   - Play one-shot UI cues with low latency.
//
// Non-responsibilities:
//   - Decode MP3 / Opus / Ogg.
//   - Mix multiple voices.
//   - Provide a Settings row for UI sound volume.
//   - Play a boot sound.
//   - Play sounds for streamed controller input.
//
// This header exposes only project-owned types. OpenOrbis SDK types (SceAudioOut*)
// are kept private to the implementation file, consistent with the other
// platform classes (InputManager, GfxManager, etc.).
//
// Threading: main-thread owned. `play()` is non-blocking: it enqueues a cue
// request and returns immediately. A dedicated worker thread performs the
// blocking PS4 AudioOut output (sceAudioOutOutput blocks until playback
// starts), so the main thread never blocks for the duration of a sound.
// ===============================================================================

#ifndef PS4_UI_SOUND_MANAGER_H
#define PS4_UI_SOUND_MANAGER_H

#include <cstdint>

namespace ps4 {

// ---------------------------------------------------------------------------
// UiSound
// ---------------------------------------------------------------------------

/// The set of UI navigation cues this manager can play.
enum class UiSound : uint8_t {
    Accept,        ///< A UI action was successfully accepted/activated.
    Back,          ///< A Back/Cancel action was successfully performed.
    Cursor,        ///< UI selection/focus actually moved.
    SettingsOpen,  ///< The Settings overlay was opened.
    SettingsClose  ///< The Settings overlay was closed.
};

// ---------------------------------------------------------------------------
// UiSoundManager
// ---------------------------------------------------------------------------

/// Owns the PS4 UI navigation sound output resource and the preloaded PCM
/// assets.
///
/// Responsibilities:
///   - Load and retain the decoded PCM for the three UI WAV assets.
///   - Initialize/shutdown its own PS4 AudioOut port.
///   - Play one-shot UI cues (single voice; a new cue interrupts/restarts the
///     currently-playing cue).
///
/// Failure behavior is non-fatal: if a WAV is missing, unsupported, or the
/// audio output cannot be initialized, the manager logs the failure and
/// continues silently. The application must not depend on UI sounds.
class UiSoundManager {
public:
    /// Constructor performs no fallible operations.
    UiSoundManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~UiSoundManager();

    // Non-copyable, non-movable.
    UiSoundManager(const UiSoundManager&) = delete;
    UiSoundManager& operator=(const UiSoundManager&) = delete;
    UiSoundManager(UiSoundManager&&) = delete;
    UiSoundManager& operator=(UiSoundManager&&) = delete;

    /// Preload the three UI WAV assets and initialize the PS4 AudioOut port.
    /// Non-fatal: on any failure the manager logs and continues with the
    /// working subset (or none). Safe after partial initialization or repeated
    /// calls (idempotent).
    bool initialize();

    /// Release the AudioOut port and the retained PCM buffers.
    /// Safe after partial initialization or repeated calls (idempotent).
    void shutdown();

    /// Returns true once initialize() has succeeded and before shutdown().
    bool isInitialized() const;

    /// Play a one-shot UI cue. Non-blocking: requests the cue for the worker
    /// thread to output. If another cue is still playing, the new cue
    /// interrupts/restarts it (single voice; the newest cue wins). No-op if
    /// the manager is not initialized or the requested cue failed to load.
    void play(UiSound sound);

private:
    struct Impl;
    Impl* m_impl;

    /// UI sound worker thread entry point. Performs the blocking AudioOut
    /// output so the main thread never blocks.
    static void* uiSoundWorker(void* arg);
};

} // namespace ps4

#endif // PS4_UI_SOUND_MANAGER_H

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

// === AudioDecodeOutput.h ====================================================
//
// OpenOrbis Moonlight client — PS4 Audio Decode / Output
//
// AudioDecodeOutput owns the audio decode and playback subsystem for the
// active Moonlight stream. It consumes project-owned EncodedAudioPacket
// objects from StreamSessionManager, decodes Moonlight Opus audio using the
// decoder-only Opus static library, and outputs PCM through the PS4
// AudioOut subsystem. It runs all decode/output work on one dedicated worker
// thread and exposes a project-owned audio playback clock for future A/V
// synchronization.
//
// This module does NOT decode video, render video, send controller input, or
// implement final A/V synchronization.
//
// This header exposes only project-owned types. OpenOrbis SDK types (OrbisAudioOut*),
// Opus decoder structs, moonlight-common-c C structs, mutex/thread types, and
// ENet types must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_AUDIO_DECODE_OUTPUT_H
#define MOONLIGHT_AUDIO_DECODE_OUTPUT_H

#include <cstdint>
#include <cstddef>

namespace ml {

// Forward declaration.
class StreamSessionManager;

// ---------------------------------------------------------------------------
// AudioDecodeOutputState
// ---------------------------------------------------------------------------

/// Application-visible audio decode/output state.
enum class AudioDecodeOutputState : uint8_t {
    Uninitialized,  ///< Not initialized.
    Idle,           ///< No active audio session.
    Starting,       ///< Worker starting, waiting for session/config.
    Running,        ///< Decoding and outputting audio.
    Stopping,       ///< Stop requested, teardown in progress.
    Failed          ///< Fatal audio failure (video continues).
};

// ---------------------------------------------------------------------------
// AudioDecodeOutputResultCategory
// ---------------------------------------------------------------------------

/// Semantic categories of audio decode/output operation results.
enum class AudioDecodeOutputResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidConfiguration,
    UnsupportedAudioConfiguration,
    OpusInitializationFailure,
    OpusDecodeFailure,
    AudioOutputInitializationFailure,
    AudioOutputFailure,
    BufferFailure,
    Cancelled,
    NativeFailure
};

// ---------------------------------------------------------------------------
// AudioDecodeOutputResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by AudioDecodeOutput operations.
struct AudioDecodeOutputResult {
    AudioDecodeOutputResultCategory category;
    int nativeError;   ///< Raw Opus/PS4 error code for diagnostics (0 when none).

    AudioDecodeOutputResult()
        : category(AudioDecodeOutputResultCategory::Success), nativeError(0) {}
    AudioDecodeOutputResult(AudioDecodeOutputResultCategory cat, int err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == AudioDecodeOutputResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// AudioDecodeOutputUpdateType
// ---------------------------------------------------------------------------

/// Low-volume semantic audio updates emitted to the main thread.
enum class AudioDecodeOutputUpdateType : uint8_t {
    Started,          ///< Audio worker started.
    PlaybackStarted,  ///< First PCM submitted to AudioOut.
    Failed,           ///< Fatal audio failure (video continues).
    Stopped           ///< Audio session stopped.
};

// ---------------------------------------------------------------------------
// AudioDecodeOutputUpdate
// ---------------------------------------------------------------------------

/// Low-volume worker → main update. No media payloads.
struct AudioDecodeOutputUpdate {
    AudioDecodeOutputUpdateType type;
    AudioDecodeOutputResultCategory result;
    int nativeError = 0;   ///< Raw Opus/PS4 error code for diagnostics.
};

// ---------------------------------------------------------------------------
// AudioPlaybackClock
// ---------------------------------------------------------------------------

/// Project-owned audio playback timebase for future A/V synchronization.
///
/// `playedSamples` represents the number of PCM samples (per channel) that
/// have been successfully submitted to the PS4 AudioOut port. This is an
/// ESTIMATE of playback position — the PS4 AudioOut API does not expose a
/// hardware-played sample counter. Future A/V sync may compensate for queued
/// audio using the known grain latency.
struct AudioPlaybackClock {
    bool valid = false;              ///< False until first actual output.
    uint32_t sessionGeneration = 0;  ///< Stream generation this clock belongs to.
    uint64_t playedSamples = 0;      ///< Submitted-sample estimate (per channel).
    uint32_t sampleRate = 0;         ///< Audio sample rate (Hz).
};

// ---------------------------------------------------------------------------
// AudioDecodeOutput
// ---------------------------------------------------------------------------

/// Owns the PS4 audio decode and playback subsystem.
///
/// Responsibilities:
///   - Consume EncodedAudioPacket objects from StreamSessionManager.
///   - Consume the corresponding AudioStreamConfiguration.
///   - Decode Moonlight Opus audio (decoder-only Opus static library).
///   - Output PCM through the PS4 AudioOut subsystem.
///   - Run audio decode/output work on one dedicated worker thread.
///   - Preserve low latency with bounded buffering.
///   - Own Opus decoder lifecycle and PS4 AudioOut port lifecycle.
///   - Support deterministic start, stop, restart, and shutdown.
///   - Provide a usable audio playback timebase for future A/V sync.
///
/// Non-responsibilities:
///   - Own Moonlight audio network reception.
///   - Modify moonlight-common-c.
///   - Perform video timing/presentation decisions.
///   - Decode video.
///   - Send controller input.
///   - Implement final A/V sync policy.
///   - Implement surround output.
///   - Add audio settings UI.
///
/// Threading: AudioDecodeOutput owns exactly one audio worker thread. All
/// decode/output work happens on this thread. The main/render thread and
/// moonlight-common-c callback threads never perform decode/output work.
class AudioDecodeOutput {
public:
    /// Constructor performs no fallible operations.
    AudioDecodeOutput();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~AudioDecodeOutput();

    // Non-copyable, non-movable.
    AudioDecodeOutput(const AudioDecodeOutput&) = delete;
    AudioDecodeOutput& operator=(const AudioDecodeOutput&) = delete;
    AudioDecodeOutput(AudioDecodeOutput&&) = delete;
    AudioDecodeOutput& operator=(AudioDecodeOutput&&) = delete;

    /// Initialize the audio subsystem.
    /// `ssm` is a non-owning pointer to an initialized StreamSessionManager.
    /// Must be called after StreamSessionManager::initialize().
    /// Performs no audio session at initialization.
    AudioDecodeOutputResult initialize(StreamSessionManager* ssm);

    /// Start an audio session. Creates the worker thread which waits for the
    /// target stream session generation and its audio configuration.
    /// Returns InvalidState if already running.
    AudioDecodeOutputResult start();

    /// Stop the audio session. Discards pending encoded packets and unstaged
    /// PCM, closes the AudioOut port, destroys the Opus decoder, and joins
    /// the worker thread.
    void stop();

    /// Returns the current audio state.
    AudioDecodeOutputState state() const;

    /// Pop the next low-volume AudioDecodeOutputUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(AudioDecodeOutputUpdate& out);

    /// Returns the current audio playback clock. The clock is valid only
    /// after playback has begun for the active session.
    AudioPlaybackClock playbackClock() const;

    /// Shutdown the audio subsystem. Releases all resources and stops the
    /// worker. Must be called before StreamSessionManager::shutdown().
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;

    /// Audio worker thread entry point.
    static void* audioWorker(void* arg);
};

} // namespace ml

#endif // MOONLIGHT_AUDIO_DECODE_OUTPUT_H

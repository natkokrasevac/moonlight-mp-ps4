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

// === StreamSessionManager.h =================================================
//
// OpenOrbis Moonlight client — Stream Session / moonlight-common-c
// Connection Integration
//
// StreamSessionManager owns the active Moonlight streaming connection
// lifecycle. It accepts a successful StreamSessionDescriptor produced by
// ApplicationSessionManager, converts the project-owned session configuration
// into the exact moonlight-common-c connection configuration, starts the
// connection with LiStartConnection() on a dedicated worker thread, owns
// connection callbacks and callback-to-project adaptation, and exposes clean
// project-owned connection-state updates to MoonlightApp. It forwards encoded
// video decode units to the VideoDecoder and encoded audio packets to the
// AudioDecodeOutput.
//
// This header exposes only project-owned types. moonlight-common-c C structs,
// OpenOrbis SDK types, mutex/thread types, and ENet types must not appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_STREAM_SESSION_MANAGER_H
#define MOONLIGHT_STREAM_SESSION_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "HostManager.h"   // ml::HostId
#include "ApplicationCatalog.h"  // ml::ApplicationId
#include "ApplicationSessionManager.h"  // ml::StreamSessionDescriptor

namespace ml {

// Forward declaration.
class VideoDecoder;

// ---------------------------------------------------------------------------
// StreamSessionState
// ---------------------------------------------------------------------------

/// Application-visible stream session state.
enum class StreamSessionState : uint8_t {
    Idle,       ///< No active stream.
    Starting,   ///< Connection setup in progress.
    Streaming,  ///< Active stream.
    Stopping,   ///< Stop requested, teardown in progress.
    Failed      ///< Connection failed.
};

// ---------------------------------------------------------------------------
// StreamSessionResultCategory
// ---------------------------------------------------------------------------

/// Semantic categories of stream session operation results.
enum class StreamSessionResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidSession,
    AlreadyActive,
    ConfigurationFailure,
    ConnectionStartFailure,
    ConnectionInterrupted,
    ConnectionTerminated,
    UnsupportedVideoFormat,
    UnsupportedAudioConfiguration,
    Cancelled,
    LibraryFailure
};

// ---------------------------------------------------------------------------
// StreamSessionResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by StreamSessionManager operations.
struct StreamSessionResult {
    StreamSessionResultCategory category;
    int nativeError;   ///< Raw Moonlight/native error code for diagnostics (0 when none).

    StreamSessionResult() : category(StreamSessionResultCategory::Success), nativeError(0) {}
    StreamSessionResult(StreamSessionResultCategory cat, int err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == StreamSessionResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// StreamSessionUpdateType
// ---------------------------------------------------------------------------

/// Semantic connection-state updates emitted to the main thread.
enum class StreamSessionUpdateType : uint8_t {
    Connecting,     ///< Connection setup in progress.
    Connected,      ///< Stream established.
    Interrupted,    ///< Connection interrupted.
    Terminated,     ///< Remote/server termination.
    Failed,         ///< Connection failed.
    StoppedByUser   ///< Local user stop completed.
};

// ---------------------------------------------------------------------------
// StreamSessionUpdate
// ---------------------------------------------------------------------------

/// Low-volume worker/callback → main update. No media payloads.
struct StreamSessionUpdate {
    StreamSessionUpdateType type;
    StreamSessionResultCategory result;
    HostId hostId = 0;
    ApplicationId applicationId = 0;
    int nativeError = 0;   ///< Raw Moonlight error code for diagnostics.
};

// ---------------------------------------------------------------------------
// AudioStreamConfiguration
// ---------------------------------------------------------------------------

/// Project-owned representation of the Moonlight-provided Opus configuration.
/// Captures only data the future decoder requires.
struct AudioStreamConfiguration {
    int sampleRate = 0;
    int channelCount = 0;
    int streams = 0;
    int coupledStreams = 0;
    int samplesPerFrame = 0;
    uint8_t mapping[8] = {0};
};

// ---------------------------------------------------------------------------
// EncodedAudioPacket
// ---------------------------------------------------------------------------

/// Project-owned handoff type for encoded audio packets.
struct EncodedAudioPacket {
    std::vector<uint8_t> data;  ///< Encoded Opus bytes.
    uint32_t sequence = 0;      ///< RTP sequence number (if available).
};

// ---------------------------------------------------------------------------
// StreamSessionManager
// ---------------------------------------------------------------------------

/// Owns the active Moonlight streaming connection lifecycle.
///
/// Responsibilities:
///   - Accept a successful StreamSessionDescriptor.
///   - Convert the project-owned session configuration into the exact
///     moonlight-common-c connection configuration.
///   - Start the Moonlight connection with LiStartConnection() on a worker.
///   - Use the numeric IPv4 address already selected during launch/resume.
///   - Own the active moonlight-common-c connection lifecycle.
///   - Own connection callbacks and callback-to-project adaptation.
///   - Receive encoded video decode units and audio samples.
///   - Expose clean project-owned media/event interfaces.
///   - Support deterministic stop, interruption, failure, and shutdown.
///
/// Non-responsibilities:
///   - Decode video.
///   - Render video.
///   - Decode/output audio.
///   - Send controller input.
///   - Quit the remote application.
///   - Implement RTSP/ENet protocols.
///
/// Threading: StreamSessionManager owns exactly one lifecycle worker thread.
/// LiStartConnection() runs on this worker (never the main thread). Moonlight
/// callbacks run on library-owned threads and must remain lightweight.
class StreamSessionManager {
public:
    /// Constructor performs no fallible operations.
    StreamSessionManager();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~StreamSessionManager();

    // Non-copyable, non-movable.
    StreamSessionManager(const StreamSessionManager&) = delete;
    StreamSessionManager& operator=(const StreamSessionManager&) = delete;
    StreamSessionManager(StreamSessionManager&&) = delete;
    StreamSessionManager& operator=(StreamSessionManager&&) = delete;

    /// Initialize the manager. Performs no connection.
    /// Must be called after NetworkManager initialization.
    StreamSessionResult initialize();

    /// Start a stream for the given session descriptor.
    /// Moves the descriptor (including sensitive crypto material) into
    /// manager-owned active session state.
    /// Returns AlreadyActive if a stream is already active.
    StreamSessionResult start(StreamSessionDescriptor&& session);

    /// Stop the active/starting stream. Idempotent where practical.
    /// Does not quit the remote application.
    void stop();

    /// Pop the next completed StreamSessionUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(StreamSessionUpdate& out);

    /// Block until an encoded audio packet is available or the stream stops.
    /// Returns true if a packet was popped into `out`, false if the stream
    /// stopped/terminated (no more audio will arrive).
    ///
    /// `sessionGeneration` is the session generation this consumer is
    /// waiting for (see currentSessionGeneration()). The consumer exits its
    /// wait only when that specific session has definitively terminated
    /// (state == Idle/Failed) or a stop/shutdown was requested. It does NOT
    /// exit merely because the stream has not started yet, even if a previous
    /// session already completed. This makes the wait semantics
    /// session-specific rather than process-lifetime/"ever started" based.
    ///
    /// This is the blocking consumer API intended for the AudioDecodeOutput
    /// worker.
    bool waitPopAudio(EncodedAudioPacket& outPacket, uint32_t sessionGeneration);

    /// Retrieve the audio stream configuration for the given session
    /// generation. Returns true if a valid configuration is available for
    /// that generation, false otherwise (e.g., the session has not started
    /// yet, or the configuration is not yet captured).
    ///
    /// This is generation-aware so a newly started AudioDecodeOutput worker
    /// cannot accidentally consume a previous session's audio configuration.
    bool audioConfiguration(AudioStreamConfiguration& out, uint32_t sessionGeneration) const;

    /// Wake any audio consumer blocked in waitPopAudio(). Used by
    /// AudioDecodeOutput::stop() to unblock its worker when the audio module
    /// is stopped independently of the stream (e.g., during application
    /// shutdown before StreamSessionManager::shutdown()).
    void wakeAudioWaiters();

    /// Returns the current session generation. Incremented each time start()
    /// begins a new stream session. An AudioDecodeOutput worker captures this
    /// (and the current state) at start to determine which session it will
    /// consume.
    uint32_t currentSessionGeneration() const;

    /// Returns the current stream session state.
    StreamSessionState state() const;

    /// Returns true if a stream is Starting or Streaming.
    bool isActive() const;

    /// Returns the cumulative encoded-video bytes received for the current
    /// stream session. This is a project-owned counter incremented in the
    /// video submit callback (it counts the assembled access-unit bytes). It
    /// is reset when a new stream session starts. Used by MoonlightApp to
    /// compute the measured encoded-video bitrate for the statistics overlay.
    uint64_t rxBytes() const;

    /// Read the current estimated network round-trip time (ms) and RTT
    /// variance (jitter, ms) from the active control stream. Uses the existing
    /// moonlight-common-c LiGetEstimatedRttInfo() public API (no dependency
    /// patch). Returns true if valid values are available (control stream
    /// connected), false otherwise. When false, the outputs are left unchanged.
    bool readNetworkStats(float& rttMs, float& jitterMs) const;

    /// Returns the negotiated video format for the current stream session:
    /// VIDEO_FORMAT_H264 (0x0001) or VIDEO_FORMAT_H265 (0x0100). Returns 0
    /// before the video format has been negotiated (i.e. before the decoder
    /// setup callback fires). Used by MoonlightApp to display the active codec
    /// in the in-stream statistics overlay.
    int negotiatedVideoFormat() const;

    /// Set the VideoDecoder that receives video callbacks.
    /// `decoder` is a non-owning pointer to an initialized VideoDecoder.
    /// Must be called before start(). The decoder must outlive the active
    /// stream session.
    void setVideoDecoder(VideoDecoder* decoder);

    /// Stop the worker, join it, clear session material and queues, and
    /// release resources. Must be called before NetworkManager shutdown.
    void shutdown();

    /// Lifecycle worker thread entry point.
    static void* lifecycleWorker(void* arg);

    // Impl is declared public (forward declaration) so that the static
    // callback adapter functions and the active-owner bridge defined in
    // StreamSessionManager.cpp can name the pointer type without exposing
    // moonlight-common-c C types in this header. The full definition lives
    // in the .cpp translation unit.
    struct Impl;

private:
    Impl* m_impl;
};

} // namespace ml

#endif // MOONLIGHT_STREAM_SESSION_MANAGER_H

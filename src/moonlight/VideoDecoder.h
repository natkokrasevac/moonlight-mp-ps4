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

// === VideoDecoder.h =========================================================
//
// OpenOrbis Moonlight client — PS4 Video Decode
//
// VideoDecoder owns the PS4 hardware video decode subsystem. It consumes
// encoded H.264 access units (assembled from moonlight-common-c DECODE_UNIT
// buffer chains by StreamSessionManager), decodes them using the OpenOrbis SDK
// Videodec2 library (loaded dynamically), and produces project-owned
// DecodedVideoFrame objects with explicit lifetime/release semantics for the
// main-thread presentation path.
//
// This module does NOT render video, convert YUV→RGB, decode audio, or send
// controller input.
//
// This header exposes only project-owned types. OpenOrbis SDK types (SceVideodec2*),
// moonlight-common-c C structs, mutex/thread types, and ENet types must not
// appear here.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_VIDEO_DECODER_H
#define MOONLIGHT_VIDEO_DECODER_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include "StreamStatistics.h"  // ml::StreamStatsCounters

namespace ml {

// ---------------------------------------------------------------------------
// VideoCodec
// ---------------------------------------------------------------------------

/// Project-owned video codec identifier.
enum class VideoCodec : uint8_t {
    H264 = 1,   ///< H.264 / AVC.
    H265 = 2    ///< H.265 / HEVC (PS4 Pro).
};

// ---------------------------------------------------------------------------
// VideoPixelFormat
// ---------------------------------------------------------------------------

/// Project-owned decoded pixel format.
/// Matches the PS4 Videodec2 SCE_VIDEODEC2_FRAME_FORMAT_DEFAULT (NV12).
enum class VideoPixelFormat : uint8_t {
    NV12 = 0   ///< YUV 4:2:0 planar: Y plane, then interleaved UV plane.
};

// ---------------------------------------------------------------------------
// VideoDecoderConfiguration
// ---------------------------------------------------------------------------

/// Project-owned decoder configuration.
/// Sourced from the active StreamSessionDescriptor.configuration.
struct VideoDecoderConfiguration {
    uint32_t width = 0;        ///< Video width (pixels).
    uint32_t height = 0;       ///< Video height (pixels).
    uint32_t fps = 0;          ///< Video frame rate.
    /// Requested stream bitrate (kbps). Used to select a decoder level high
    /// enough to cover the stream: the configured level must accommodate both
    /// the resolution AND the bitrate, otherwise the decoder core rejects the
    /// stream with SCE_VIDEODEC2_ERROR_OVERSIZE_DECODE. 0 = unknown (falls back
    /// to the resolution-derived level).
    uint32_t bitrateKbps = 0;
    VideoCodec codec = VideoCodec::H264;
    uint32_t colorSpace = 1;   ///< COLORSPACE_REC_709 (1).
    uint32_t colorRange = 0;   ///< COLOR_RANGE_LIMITED (0).
};

// ---------------------------------------------------------------------------
// VideoDecoderState
// ---------------------------------------------------------------------------

/// Application-visible decoder state.
enum class VideoDecoderState : uint8_t {
    Idle,          ///< No active decoder session.
    Starting,      ///< Decoder initialization in progress.
    Running,       ///< Decoder active and decoding frames.
    WaitingForIdr, ///< Decoder lost sync, waiting for fresh IDR.
    Stopping,      ///< Stop requested, teardown in progress.
    Failed         ///< Fatal decoder error.
};

// ---------------------------------------------------------------------------
// VideoDecoderResultCategory
// ---------------------------------------------------------------------------

/// Semantic categories of decoder operation results.
enum class VideoDecoderResultCategory : uint8_t {
    Success,
    InvalidState,
    InvalidConfiguration,
    UnsupportedCodec,
    DecoderInitializationFailure,
    DecoderResourceFailure,
    DecodeFailure,
    OutputFailure,
    NeedsIdr,
    Cancelled,
    NativeFailure
};

// ---------------------------------------------------------------------------
// VideoDecoderResult
// ---------------------------------------------------------------------------

/// A lightweight result returned by VideoDecoder operations.
struct VideoDecoderResult {
    VideoDecoderResultCategory category;
    int nativeError;   ///< Raw SDK error code for diagnostics (0 when none).

    VideoDecoderResult() : category(VideoDecoderResultCategory::Success), nativeError(0) {}
    VideoDecoderResult(VideoDecoderResultCategory cat, int err = 0)
        : category(cat), nativeError(err) {}
    bool succeeded() const { return category == VideoDecoderResultCategory::Success; }
};

// ---------------------------------------------------------------------------
// VideoDecoderUpdateType
// ---------------------------------------------------------------------------

/// Low-volume semantic decoder updates emitted to the main thread.
enum class VideoDecoderUpdateType : uint8_t {
    Started,       ///< Decoder session started.
    WaitingForIdr, ///< Decoder lost sync, waiting for IDR.
    Recovered,     ///< Decoder recovered from IDR wait.
    Failed,        ///< Fatal decoder error.
    Stopped        ///< Decoder session stopped.
};

// ---------------------------------------------------------------------------
// VideoDecoderUpdate
// ---------------------------------------------------------------------------

/// Low-volume worker → main update. No media payloads.
struct VideoDecoderUpdate {
    VideoDecoderUpdateType type;
    VideoDecoderResultCategory result;
    int nativeError = 0;   ///< Raw SDK error code for diagnostics.
};

// ---------------------------------------------------------------------------
// EncodedVideoUnit
// ---------------------------------------------------------------------------

/// Project-owned encoded video access unit.
/// Assembled by StreamSessionManager from a moonlight-common-c DECODE_UNIT
/// buffer chain into a contiguous Annex-B elementary stream.
struct EncodedVideoUnit {
    uint64_t frameNumber = 0;   ///< Moonlight frame number.
    uint32_t frameType = 0;     ///< FRAME_TYPE_IDR (1) or FRAME_TYPE_PFRAME (0).
    uint32_t codecType = 0;     ///< H.264 (0x0001) or H.265/HEVC (0xEE049).
    uint32_t rtpTimestamp = 0;  ///< Original RTP timestamp (90 kHz).
    uint64_t presentationTimeUs = 0;  ///< Presentation time (µs).
    uint32_t fullLength = 0;    ///< Total AU byte count.
    uint32_t fragmentCount = 0; ///< Number of LENTRY fragments assembled.
    std::vector<uint8_t> data;  ///< Contiguous Annex-B AU bytes.
};

// ---------------------------------------------------------------------------
// DecodedVideoFrame
// ---------------------------------------------------------------------------

/// Project-owned decoded frame with explicit lifetime/release semantics.
///
/// This is a move-only RAII handle. It references a decoder-owned surface
/// that must not be reused until the consumer releases it. Moving transfers
/// the lease; destruction releases the surface back to the decoder.
///
/// The main-thread presentation path acquires frames via
/// VideoDecoder::tryAcquireLatestFrame() and releases them by calling
/// release() or letting the frame go out of scope.
class DecodedVideoFrame {
public:
    /// Construct an empty (invalid) frame.
    DecodedVideoFrame();

    /// Destructor releases the surface if not already released.
    ~DecodedVideoFrame();

    // Non-copyable.
    DecodedVideoFrame(const DecodedVideoFrame&) = delete;
    DecodedVideoFrame& operator=(const DecodedVideoFrame&) = delete;

    // Move-only.
    DecodedVideoFrame(DecodedVideoFrame&& other) noexcept;
    DecodedVideoFrame& operator=(DecodedVideoFrame&& other) noexcept;

    /// Explicitly release the surface back to the decoder.
    /// Idempotent; safe to call multiple times.
    void release();

    /// Returns true if this frame references a valid decoded surface.
    bool isValid() const;

    // --- Metadata -----------------------------------------------------------

    /// Frame number from the source EncodedVideoUnit.
    uint64_t frameNumber() const;

    /// Presentation time (µs) from the source EncodedVideoUnit, with the
    /// epoch at the first captured frame (host timeline). Used for frame
    /// pacing diagnostics. Returns 0 if unavailable.
    uint64_t presentationTimeUs() const;

    /// Decode time (µs) measured for this frame's hardware decode call on the
    /// decoder worker thread. Used by the in-stream statistics overlay to
    /// report the per-frame decode time. Returns 0 if unavailable.
    uint64_t decodeTimeUs() const;

    /// Visible width (pixels).
    uint32_t width() const;

    /// Visible height (pixels).
    uint32_t height() const;

    /// Aligned decoder surface height (pixels). May exceed visible height
    /// (e.g., 1088 for a 1080 stream). The renderer must not display the
    /// padded rows beyond the visible height.
    uint32_t surfaceHeight() const;

    /// Decoder surface pool slot index.
    uint32_t surfaceSlot() const;

    /// Row pitch in bytes (Y plane).
    uint32_t pitch() const;

    /// Decoded pixel format.
    VideoPixelFormat format() const;

    /// Y plane base address.
    void* yPlane() const;

    /// UV plane base address (interleaved U/V).
    void* uvPlane() const;

    /// Color space (COLORSPACE_REC_709 = 1).
    uint32_t colorSpace() const;

    /// Color range (COLOR_RANGE_LIMITED = 0).
    uint32_t colorRange() const;

private:
    // Private implementation. The release token is opaque to consumers.
    struct Impl;
    Impl* m_impl;

    // VideoDecoder constructs and transfers DecodedVideoFrame ownership.
    friend class VideoDecoder;
};

// ---------------------------------------------------------------------------
// VideoDecoder
// ---------------------------------------------------------------------------

/// Owns the PS4 hardware video decode subsystem.
///
/// Responsibilities:
///   - Consume EncodedVideoUnit objects from StreamSessionManager.
///   - Decode H.264 using the OpenOrbis SDK Videodec2 library.
///   - Run decoding on one dedicated worker thread.
///   - Own decoder lifecycle and SDK resources.
///   - Produce project-owned DecodedVideoFrame objects.
///   - Support start/stop/restart and clean IDR recovery.
///
/// Non-responsibilities:
///   - Render video.
///   - Convert YUV→RGB.
///   - Decode audio.
///   - Send controller input.
///
/// Threading: VideoDecoder owns exactly one decoder worker thread. All decode
/// work happens on this thread. The main/render thread and moonlight-common-c
/// callback threads never perform decode work.
class VideoDecoder {
public:
    /// Constructor performs no fallible operations.
    VideoDecoder();

    /// Destructor must not throw. It acts as a final safety net; orderly
    /// shutdown should be performed by calling shutdown() explicitly.
    ~VideoDecoder();

    // Non-copyable, non-movable.
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    /// Initialize the decoder. Performs no decoder session at initialization.
    VideoDecoderResult initialize();

    /// Start a decoder session for the given configuration.
    /// Creates the decoder instance and starts the worker thread.
    /// Returns InvalidConfiguration if the configuration is not supported.
    VideoDecoderResult start(const VideoDecoderConfiguration& config);

    /// Stop the decoder session. Drains remaining input, flushes the decoder,
    /// releases all resources, and joins the worker thread.
    void stop();

    /// Returns the current decoder state.
    VideoDecoderState state() const;

    /// Pop the next low-volume VideoDecoderUpdate into `out`.
    /// Returns true if an update was available, false if the queue is empty.
    bool pollUpdate(VideoDecoderUpdate& out);

    /// Push an encoded video unit into the decoder's input queue.
    /// Called from the moonlight-common-c submitDecodeUnit callback thread.
    /// Returns true if accepted, false if the queue is full (unit dropped).
    bool pushEncodedUnit(EncodedVideoUnit&& unit);

    /// Acquire the latest decoded frame. Drops older queued frames.
    /// Returns true if a frame was acquired into `out`, false if none
    /// available. The caller owns the returned frame and must release it.
    bool tryAcquireLatestFrame(DecodedVideoFrame& out);

    /// Read the cumulative decode statistics counters (non-destructive).
    /// Fills `out.decodedFrames`, `out.decodeTotalUs`, and `out.decodeCount`.
    void readDecodeStats(StreamStatsCounters& out) const;

    /// Returns the current reuse generation of the decoder surface at
    /// `surfaceSlot`. The generation increments each time the surface is
    /// re-acquired for a new decode. The renderer uses this to detect whether
    /// a surface it still holds has been reused (overwritten) by the decoder,
    /// which would indicate a frame-lifetime violation (stale-content
    /// corruption). Returns 0 for an invalid slot.
    uint32_t surfaceGeneration(uint32_t surfaceSlot) const;

    /// Shutdown the decoder. Releases all resources and stops the worker.
    void shutdown();

private:
    struct Impl;
    Impl* m_impl;

    /// Decoder worker thread entry point.
    static void* decoderWorker(void* arg);

    // DecodedVideoFrame's private implementation needs to reference
    // VideoDecoder::Impl for surface release.
    friend class DecodedVideoFrame;
};

} // namespace ml

#endif // MOONLIGHT_VIDEO_DECODER_H

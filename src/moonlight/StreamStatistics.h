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

// === StreamStatistics.h =====================================================
//
// Moonlight client for PlayStation 4 — Stream Statistics
//
// Small project-owned value types for the in-stream statistics overlay.
//
// This header is deliberately narrow. It defines:
//   - StreamStatsCounters: cumulative counters owned by the subsystems that
//     produce them (StreamSessionManager for ingress bytes / negotiated
//     geometry, VideoDecoder for decode timing / decoded frames). These are
//     always-on (not gated by ML_VIDEO_PERF_DIAG) so the user-facing overlay
//     works regardless of the temporary diagnostic gate.
//   - StreamStatisticsSnapshot: the 1-second display snapshot pushed to
//     UiManager for presentation. It contains only primitive, project-owned
//     values (no SDK or Moonlight internal pointer types).
//
// This is NOT a telemetry manager, metrics registry, event bus, or observer
// framework. It is a narrowly-scoped value type for stream diagnostics.
//
// Dependencies: C++ standard library subset supported by the PS4 toolchain.
// ===============================================================================

#ifndef MOONLIGHT_STREAM_STATISTICS_H
#define MOONLIGHT_STREAM_STATISTICS_H

#include <cstdint>

namespace ml {

// ---------------------------------------------------------------------------
// Cumulative stream statistics counters
// ---------------------------------------------------------------------------

/// Cumulative counters owned by the subsystems that produce them.
///
/// These are read non-destructively by MoonlightApp once per second to
/// compute interval deltas (FPS, decode time, bitrate). They are reset when a
/// new stream session starts so deltas never carry across sessions.
struct StreamStatsCounters {
    uint64_t rxBytes = 0;          ///< Cumulative encoded video bytes received.
    uint64_t decodedFrames = 0;    ///< Cumulative decoded frames.
    uint64_t decodeTotalUs = 0;    ///< Cumulative decode time (µs).
    uint64_t decodeCount = 0;      ///< Cumulative decode calls.
    int width = 0;                 ///< Negotiated video width (pixels).
    int height = 0;                ///< Negotiated video height (pixels).
};

// ---------------------------------------------------------------------------
// 1-second display snapshot
// ---------------------------------------------------------------------------

/// The 1-second display snapshot pushed to UiManager for presentation.
///
/// `valid` is false when no reliable snapshot is available (e.g., before the
/// first interval completes). UiManager omits rows for invalid/unavailable
/// fields rather than showing misleading zeroes.
struct StreamStatisticsSnapshot {
    bool valid = false;
    int  width = 0;                ///< Actual stream width (pixels).
    int  height = 0;               ///< Actual stream height (pixels).
    float fps = 0.0f;              ///< Measured decoded FPS (interval average).
    float decodeMs = 0.0f;         ///< Average decode time (ms, interval).
    float bitrateMbps = 0.0f;      ///< Measured encoded-video bitrate (Mbps, interval).
    float rttMs = 0.0f;            ///< Estimated host round-trip time (ms).
    float jitterMs = 0.0f;         ///< Estimated RTT variance / jitter (ms).
    float frameDecodeMs = 0.0f;    ///< Decode time of the most-recent presented frame (ms).
    float framePresentMs = 0.0f;   ///< NV12 conversion + overlay blend time (ms).
    float frameTotalMs = 0.0f;     ///< Total frame time = decode + presentation (ms).
};

} // namespace ml

#endif // MOONLIGHT_STREAM_STATISTICS_H

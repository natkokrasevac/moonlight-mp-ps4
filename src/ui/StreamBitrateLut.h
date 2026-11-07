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

// === StreamBitrateLut.h ======================================================
//
// Moonlight client for PlayStation 4 — Stream Bitrate Lookup Table
//
// Maps a (codec, resolution, frame rate) selection to the bitrate preset that
// is auto-selected when the user changes those settings. The presets are the
// standard Moonlight client values (5/10/20/40/80 Mbps).
//
// The decoder level is selected independently as the MAXIMUM of a
// resolution-derived floor and a bitrate-derived requirement (see
// VideoDecoder.cpp, "Decoder level selection"). The resolution floor alone
// already covers every preset in this set at its resolution, so the configured
// level always covers the stream (a level too low for the stream faults the
// decoder core with SCE_VIDEODEC2_ERROR_OVERSIZE_DECODE).
//
// This header is intentionally dependency-light (only <cstdint>) and contains
// no project or OpenOrbis types, so it can be included by any translation unit
// and tuned in isolation.
//
// ---------------------------------------------------------------------------
// Reference: H.264/HEVC Annex A level MaxBR boundaries (NOT used as presets)
//
// The decoder level is selected as the MAXIMUM of a resolution-derived floor
// and a bitrate-derived requirement (see VideoDecoder.cpp). The H.264/HEVC
// Annex A MaxBR boundaries are:
//
//   Resolution/fps | AVC MaxBR    | HEVC MaxBR
//   ---------------+--------------+-------------
//    720p30        |  14,000 kbps | — (no HEVC 720p preset)
//    720p60        |  20,000 kbps | — (no HEVC 720p preset)
//    1080p30       |  20,000 kbps |  30,000 kbps
//    1080p60       |  50,000 kbps |  50,000 kbps
//    4K30          | 135,000 kbps | 150,000 kbps
//    4K60          | 240,000 kbps | 300,000 kbps
//
// A previous revision of this client used those boundary values as the bitrate
// presets (14/20/50/135/240 Mbps) so each preset sat exactly on a level
// boundary. We now DEVIATE from that scheme to match the standard Moonlight
// client conventions (5/10/20/40/80 Mbps). This is safe because the decoder
// level is the max of the resolution floor and the bitrate requirement: the
// resolution floor alone already covers every preset in the new set at its
// resolution, so the configured level always covers the stream even though the
// presets no longer sit on the Annex A boundaries.
//
// ---------------------------------------------------------------------------
// Applied policy (this LUT):
//
//   * The bitrate presets are the standard Moonlight client values
//     (5/10/20/40/80 Mbps), indexed 0..4.
//   * HEVC has no 720p preset. 720p is therefore not a valid resolution when
//     the codec resolves to HEVC; the resolution is clamped to 1080p and the
//     resolution cycle skips 720p (see resolution720AllowedForCodec()).
//   * Codec "Auto" resolves to HEVC when the client supports HEVC
//     (hevcAvailable == isPro()), otherwise AVC.
//
// Resulting bitrate index per (codec, resolution, frame rate):
//
//   Resolution/fps | AVC index (Mbps) | HEVC index (Mbps)
//   ---------------+------------------+------------------
//    720p30        | 0 (5)            | (clamped to 1080p) 1 (10)
//    720p60        | 1 (10)           | (clamped to 1080p) 2 (20)
//    1080p30       | 1 (10)           | 1 (10)
//    1080p60       | 2 (20)           | 2 (20)
//    4K30          | 3 (40)           | 3 (40)
//    4K60          | 4 (80)           | 4 (80)
// ===============================================================================

#ifndef UI_STREAM_BITRATE_LUT_H
#define UI_STREAM_BITRATE_LUT_H

#include <cstdint>

namespace ui {

// ---------------------------------------------------------------------------
// Selection index conventions (mirror SettingsViewState)
// ---------------------------------------------------------------------------

/// Codec selection index.
enum StreamBitrateLutCodec : int {
    kCodecAuto = 0,   ///< Auto (HEVC if the client supports it, else AVC).
    kCodecAvc  = 1,   ///< AVC / H.264.
    kCodecHevc = 2    ///< HEVC / H.265.
};

/// Resolution selection index.
enum StreamBitrateLutResolution : int {
    kRes720p  = 0,
    kRes1080p = 1,
    kRes4K    = 2
};

/// Frame-rate selection index.
enum StreamBitrateLutFrameRate : int {
    kFps30 = 0,
    kFps60 = 1
};

/// Bitrate preset index (0..4 = 5/10/20/40/80 Mbps, the standard Moonlight
/// client values).
enum StreamBitrateLutBitrate : int {
    kBitrate5  = 0,
    kBitrate10 = 1,
    kBitrate20 = 2,
    kBitrate40 = 3,
    kBitrate80 = 4
};

// ---------------------------------------------------------------------------
// Codec resolution helpers
// ---------------------------------------------------------------------------

/// Returns true if the codec selection resolves to HEVC.
///
/// Auto resolves to HEVC only when the client supports HEVC (hevcAvailable,
/// i.e. the console is a PS4 Pro and the Pro build is active). This is a
/// CLIENT capability, independent of the host's HEVC support (which is only
/// known from serverinfo / at stream time).
inline bool codecUsesHevc(int codecIndex, bool hevcAvailable) {
    if (codecIndex == kCodecHevc) {
        return true;
    }
    if (codecIndex == kCodecAuto) {
        return hevcAvailable;
    }
    return false;   // AVC.
}

/// Returns true if 720p is a valid resolution for the codec selection.
///
/// 720p is invalid when the codec resolves to HEVC: the SDK HEVC presets start
/// at 1080p (L4.1), so there is no HEVC 720p bitrate preset. When HEVC is not
/// in play (AVC, or Auto on a non-HEVC client), 720p is always valid.
inline bool resolution720AllowedForCodec(int codecIndex, bool hevcAvailable) {
    return !codecUsesHevc(codecIndex, hevcAvailable);
}

/// Clamps a resolution index to a codec-valid value.
///
/// If the codec resolves to HEVC and the resolution is 720p, returns 1080p.
/// Otherwise returns the resolution unchanged.
inline int clampResolutionForCodec(int codecIndex, int resolutionIndex,
                                   bool hevcAvailable) {
    if (resolutionIndex == kRes720p &&
        !resolution720AllowedForCodec(codecIndex, hevcAvailable)) {
        return kRes1080p;
    }
    return resolutionIndex;
}

// ---------------------------------------------------------------------------
// Bitrate lookup
// ---------------------------------------------------------------------------

/// Returns the auto-selected bitrate preset index (0..4) for the given stream
/// specs. The selected bitrate is the standard Moonlight client value for the
/// resolution/frame-rate. The decoder level is selected independently as the
/// max of the resolution floor and the bitrate requirement (see
/// VideoDecoder.cpp), so the configured level always covers the stream.
///
/// The resolution is first clamped to a codec-valid value, so a HEVC + 720p
/// input yields the 1080p value.
///
/// codecIndex:      0=Auto, 1=AVC, 2=HEVC.
/// resolutionIndex: 0=720p, 1=1080p, 2=4K.
/// frameRateIndex:  0=30 FPS, 1=60 FPS.
/// hevcAvailable:   client HEVC capability (isPro()).
inline int autoBitrateIndexFor(int codecIndex, int resolutionIndex,
                               int frameRateIndex, bool hevcAvailable) {
    // The auto-selected value is the same for AVC and HEVC at every
    // resolution/frame-rate (the standard Moonlight client values). The codec
    // only affects the resolution clamp (HEVC has no 720p preset).
    const int res = clampResolutionForCodec(codecIndex, resolutionIndex,
                                            hevcAvailable);
    const bool fps60 = (frameRateIndex == kFps60);

    switch (res) {
    case kRes4K:
        // 4K30 -> 40 Mbps; 4K60 -> 80 Mbps.
        return fps60 ? kBitrate80 : kBitrate40;
    case kRes1080p:
        // 1080p30 -> 10 Mbps; 1080p60 -> 20 Mbps.
        return fps60 ? kBitrate20 : kBitrate10;
    case kRes720p:
    default:
        // 720p30 -> 5 Mbps; 720p60 -> 10 Mbps.
        return fps60 ? kBitrate10 : kBitrate5;
    }
}

} // namespace ui

#endif // UI_STREAM_BITRATE_LUT_H

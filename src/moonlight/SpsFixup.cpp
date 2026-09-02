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

// === SpsFixup.cpp ===========================================================
//
// OpenOrbis Moonlight client — H.264 SPS rewrite for the Videodec2 decoder
//
// Based on Moonlight Embedded libgamestream/sps.c (GPL-3.0), adapted for
// moonlight-ps4. Uses h264bitstream to parse and re-serialize the bit-packed
// SPS, forcing num_ref_frames = 1 and a low-latency bitstream-restriction VUI.
//
// OpenOrbis SDK headers are confined to this translation unit.
// ===============================================================================

#include "SpsFixup.h"

#include <cstring>          // memcpy, memmove

// h264bitstream — confined to this translation unit.
#include "h264_stream.h"

namespace ml {

namespace {

/// Persistent h264bitstream parser state (reused across SPS rewrites).
h264_stream_t* g_h264Stream = nullptr;
/// Stream resolution captured at init (used to pick the level_idc).
int g_initialWidth = 0;
int g_initialHeight = 0;

} // namespace

void spsFixupInit(int width, int height) {
    if (g_h264Stream == nullptr) {
        g_h264Stream = h264_new();
    }
    g_initialWidth = width;
    g_initialHeight = height;
}

void spsFixup(PLENTRY sps, int flags, uint8_t* outBuf, uint32_t* outOffset) {
    if (sps == nullptr || sps->data == nullptr || sps->length <= 0 ||
        outBuf == nullptr || outOffset == nullptr) {
        return;
    }

    // The SPS NAL begins after the Annex-B start code. Detect a 3-byte
    // (00 00 01) vs 4-byte (00 00 00 01) start code.
    const int startLen = (sps->length >= 3 && sps->data[2] == 0x01) ? 3 : 4;
    if (sps->length <= startLen) {
        // No NAL payload after the start code — nothing to rewrite.
        return;
    }

    // Parse the SPS NAL (payload after the start code).
    read_nal_unit(g_h264Stream,
                  reinterpret_cast<uint8_t*>(sps->data) + startLen,
                  sps->length - startLen);

    // Force a level appropriate for the stream resolution.
    if (g_initialWidth == 1280 && g_initialHeight == 720) {
        g_h264Stream->sps->level_idc = 32;   // Level 3.2
    } else if (g_initialWidth == 1920 && g_initialHeight == 1080) {
        g_h264Stream->sps->level_idc = 42;   // Level 4.2
    }

    // Force a single reference frame (low latency, matches the reversed
    // decoder's reliable configuration).
    g_h264Stream->sps->num_ref_frames = 1;

    if (flags & SPS_FIXUP_REMOVE_VST) {
        g_h264Stream->sps->vui.video_signal_type_present_flag = 0;
    }
    if (flags & SPS_FIXUP_REMOVE_CLI) {
        g_h264Stream->sps->vui.chroma_loc_info_present_flag = 0;
    }

    if ((flags & SPS_FIXUP_BITSTREAM) == SPS_FIXUP_BITSTREAM) {
        // Without VUI, bitstream_restriction is not written.
        g_h264Stream->sps->vui_parameters_present_flag = 1;
        if (!g_h264Stream->sps->vui.bitstream_restriction_flag) {
            g_h264Stream->sps->vui.bitstream_restriction_flag = 1;
            g_h264Stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
            g_h264Stream->sps->vui.max_bits_per_mb_denom = 1;
            g_h264Stream->sps->vui.log2_max_mv_length_horizontal = 16;
            g_h264Stream->sps->vui.log2_max_mv_length_vertical = 16;
            g_h264Stream->sps->vui.num_reorder_frames = 0;
        }
        g_h264Stream->sps->vui.max_dec_frame_buffering = 1;
        g_h264Stream->sps->vui.max_bytes_per_pic_denom = 2;
        g_h264Stream->sps->vui.max_bits_per_mb_denom = 1;
    }

    // Copy the original start code.
    memcpy(outBuf + *outOffset, sps->data, static_cast<size_t>(startLen));
    *outOffset += static_cast<uint32_t>(startLen);

    // Re-serialize the SPS NAL.
    int nalLen = write_nal_unit(g_h264Stream, outBuf + *outOffset, 128);
    if (nalLen <= 0) {
        // Fallback: original SPS without rewrite.
        memcpy(outBuf + *outOffset, sps->data + startLen,
               static_cast<size_t>(sps->length - startLen));
        *outOffset += static_cast<uint32_t>(sps->length - startLen);
        return;
    }

    // h264bitstream's rbsp_to_nal() writes from byte 1 and leaves nal_buf[0]
    // = 0x00. After the Annex-B start code that yields NAL type 0 and
    // Videodec2 → 0x811d0303. Skip leading zero bytes to recover the real
    // NAL header.
    uint8_t* nal = outBuf + *outOffset;
    int skip = 0;
    while (skip < nalLen - 1 && nal[skip] == 0x00) {
        skip++;
    }
    if (skip > 0) {
        memmove(nal, nal + skip, static_cast<size_t>(nalLen - skip));
        nalLen -= skip;
    }
    *outOffset += static_cast<uint32_t>(nalLen);
}

} // namespace ml

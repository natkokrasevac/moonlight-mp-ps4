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

// === SpsFixup.h =============================================================
//
// OpenOrbis Moonlight client — H.264 SPS rewrite for the Videodec2 decoder
//
// Rewrites the H.264 Sequence Parameter Set (SPS) so the decoder is forced to
// a known-good reference-frame configuration (num_ref_frames = 1,
// max_dec_frame_buffering = 1, num_reorder_frames = 0). This removes the
// reference-frame / DPB mismatch that otherwise produces stale-macroblock
// blockiness in gradients when the host's SPS specifies a different
// reference-frame configuration than the reversed decoder expects.
//
// Based on Moonlight Embedded libgamestream/sps.c (GPL-3.0), adapted for
// moonlight-ps4. Uses the h264bitstream library to parse and re-serialize the
// bit-packed SPS.
//
// OpenOrbis SDK headers are confined to this translation unit.
// ===============================================================================

#pragma once

#include <cstdint>

// moonlight-common-c C header — confined to this translation unit.
#include "Limelight.h"   // PLENTRY, BUFFER_TYPE_SPS

namespace ml {

/// SPS fixup flags (mirrors libgamestream gs_sps_* flags).
enum SpsFixupFlags : int {
    SPS_FIXUP_BITSTREAM   = 0x01,  ///< Force bitstream-restriction VUI.
    SPS_FIXUP_REMOVE_VST  = 0x02,  ///< Clear video_signal_type_present_flag.
    SPS_FIXUP_REMOVE_CLI  = 0x04,  ///< Clear chroma_loc_info_present_flag.
};

/// Initialize the SPS fixup for a given stream resolution.
/// Must be called once per stream session (from video setup) before any
/// gs_sps_fix() call.
void spsFixupInit(int width, int height);

/// Rewrite an SPS NAL unit (from a moonlight-common-c PLENTRY) into `outBuf`.
/// Appends the rewritten SPS (including the Annex-B start code) at `*outOffset`
/// and advances `*outOffset` by the number of bytes written.
///
/// `sps` must be a PLENTRY whose bufferType == BUFFER_TYPE_SPS.
/// `outBuf` must have room for the rewritten SPS (the original length plus
/// slack; the rewrite can slightly lengthen the NAL).
void spsFixup(PLENTRY sps, int flags, uint8_t* outBuf, uint32_t* outOffset);

} // namespace ml

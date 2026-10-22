# HEVC (H.265) Implementation — Findings & Hard-Won Lessons

**Status:** Hardware-verified on PS4 Pro (OpenOrbis + SCE Videodec2).
**Scope:** A self-contained record of the findings from implementing H.265/HEVC
hardware decode and 4K streaming in the Moonlight DX PS4 client. It documents
the four non-obvious problems that were hit and how each was diagnosed and
fixed, so the knowledge is not lost and the same traps are not re-entered.

The four findings, in the order they were hit:

1. **HEVC needs a non-NULL `extraConfigInfo`** — a NULL pointer faults the
   decoder core (`VDECCORE`).
2. **The decoder level must cover the stream bitrate, not just the
   resolution** — a too-low level returns `OVERSIZE_DECODE` (`0x811D0302`).
3. **HEVC `maxFrameHeight` must be CTU-aligned (64), not 8/16** — a 3840×2160
   stream is coded as 3840×2176, so configuring 2160 fails with
   `OVERSIZE_DECODE`.
4. **The GPU texture width must equal the decoder's reported row pitch** — the
   pitch varies per codec (1920 for AVC 1080p, 2048 for HEVC 1080p, 3840 for
   4K), and for a `GNM_ARRAY_LINEAR_GENERAL` texture the GPU derives the row
   stride from the texture width.

A fifth, process-level lesson is recorded at the end: **a stale shader binary
silently masked a correct renderer fix** and cost a full debugging cycle.

---

## 1. HEVC `extraConfigInfo` must never be NULL

### Symptom

The first HEVC IDR crashed the decoder chip. The klog showed decoder-core
faults:

```
[VDECCORE@C1070037:00000000]
[VDECCORE@A01D072F:00000014]
[VIDEODEC2@A01A0640:00000000]
[video] first decode: rc=0x811D0302 ... valid=0 err=0 out=0x0 pitch=0
```

### Root cause

`SceVideodec2DecoderConfigInfo.extraConfigInfo` was left `NULL`. For AVC this
is correct (the SDK requires NULL for AVC). For HEVC the decoder core reads
**tier, bit depth, temporal-id, and GPU load level** from a
`SceVideodec2HevcExtraConfigInfo` structure. When `extraConfigInfo` is NULL the
library applies internal defaults that do not match the stream, and the decoder
core faults.

### Fix

Populate a `SceVideodec2HevcExtraConfigInfo` and point `extraConfigInfo` at it:

```c
struct Vd2HevcExtraConfigInfo {
    uint64_t thisSize;
    int32_t  tierType;              // SCE_VIDEODEC2_HEVC_TIER_TYPE_HIGH = 1
    int8_t   maxBitDepthLuma;       // 8 for Main profile
    int8_t   maxBitDepthChroma;     // 8 for Main profile
    uint16_t reserved0;
    int32_t  maxTemporalIdToDecode; // -1 = decode all temporal layers
    uint32_t hdrFrameFormat;        // SCE_VIDEODEC2_FRAME_FORMAT_DEFAULT = 0
    uint32_t gpuLoadLevel;          // SCE_VIDEODEC2_HEVC_GPU_LOAD_LEVEL_3 = 3
};

hevcExtra.thisSize = sizeof(hevcExtra);
hevcExtra.tierType = ORBIS_VIDEODEC2_HEVC_TIER_HIGH;
hevcExtra.maxBitDepthLuma = 8;
hevcExtra.maxBitDepthChroma = 8;
hevcExtra.maxTemporalIdToDecode = -1;
hevcExtra.hdrFrameFormat = ORBIS_VIDEODEC2_FRAME_FORMAT_DEFAULT;
hevcExtra.gpuLoadLevel = ORBIS_VIDEODEC2_HEVC_GPU_LOAD_LEVEL_3;
dc->extraConfigInfo = &hevcExtra;
```

**Lifetime note:** the `hevcExtra` storage must outlive **both**
`sceVideodec2QueryDecoderMemoryInfo()` and `sceVideodec2CreateDecoder()`,
which read it synchronously via `dc.extraConfigInfo`. Keep it on the stack of
the function that performs both calls (or in a longer-lived struct).

The `gpuLoadLevel = 3` value matches the official `hevc_basic` SDK sample
(it moves CPU work to the GPU). The SDK preset default is level 1.

---

## 2. The decoder level must cover the stream bitrate

### Symptom

A high-bitrate stream (Mac host, high bitrate) faulted the decoder core with
`SCE_VIDEODEC2_ERROR_OVERSIZE_DECODE` (`0x811D0302`) even though the resolution
was within the configured level.

### Root cause

`maxLevel` was selected purely from the **resolution** (the SDK preset floor).
But a codec level bounds **both** the resolution **and** the bitrate (Annex A
`MaxBR`). A stream whose bitrate exceeds the configured level's `MaxBR` is
rejected with `OVERSIZE_DECODE`.

### Fix

Select the level as the **maximum** of a resolution-derived floor and a
bitrate-derived requirement:

```c
uint32_t vdHevcLevelForStream(int h, uint32_t bitrateKbps) {
    const uint32_t byRes     = vdHevcLevelForHeight(h);
    const uint32_t byBitrate = vdHevcLevelForBitrate(bitrateKbps);
    return (byBitrate > byRes) ? byBitrate : byRes;
}
```

The bitrate tables used (HEVC Main profile, **High tier** — the decoder is
configured with `tierType = HIGH`, which also accepts Main-tier streams):

| HEVC level | idc | High-tier MaxBR | Typical use |
|------------|-----|-----------------|-------------|
| 4.0 | 120 | 30,000 kbps | 1080p30 |
| 4.1 | 123 | 50,000 kbps | 1080p60 |
| 5.0 | 150 | 150,000 kbps | 4K30 |
| 5.1 | 153 | 300,000 kbps | 4K60 |

AVC (High profile) uses the analogous Annex A `MaxBR` table:

| AVC level | idc | MaxBR | Typical use |
|-----------|-----|-------|-------------|
| 3.1 | 31 | 14,000 kbps | 720p30 |
| 3.2 | 32 | 20,000 kbps | 720p60 |
| 4.1 | 41 | 50,000 kbps | 1080p60 |
| 4.2 | 42 | 50,000 kbps | 1080p60 |
| 5.0 | 50 | 135,000 kbps | 4K30 |
| 5.1 | 51 | 240,000 kbps | 4K60 |
| 5.2 | 52 | 240,000 kbps | 4K60 |

### Caveat: host-encoder issues are not decoder-config issues

A **Mac host (VideoToolbox)** was observed to produce streams that fault the
decoder core at high bitrates for a given resolution, while an **NVIDIA/Windows
host** does not at the same resolution and bitrate. That is a host-encoder
issue (slice structure / coded size / NAL layout), **not** a decoder
configuration issue — the same decoder config works with the NVIDIA host.
Over-provisioning level/DPB was tried and did **not** fix the Mac case, so the
config is kept at the SDK-preset-matched values.

---

## 3. HEVC `maxFrameHeight` must be CTU-aligned (64)

### Symptom

4K HEVC (`3840×2160`) failed on the first IDR with `OVERSIZE_DECODE`
(`0x811D0302`), even though the configured level (L5.1) and dimensions
(3840×2160) matched the SDK preset exactly. 1080p HEVC worked.

### Root cause

HEVC codes pictures in **Coding Tree Units (CTUs) up to 64×64**. The coded
picture dimensions are the SPS `pic_width/height_in_luma_samples` rounded **up
to the next CTU multiple**. The decoder instance must be configured for at
least the coded size.

- **2160 is not a multiple of 64** (2160 / 64 = 33.75), so a 3840×2160 stream
  is coded as **3840×2176** (34 × 64). Configuring `maxFrameHeight = 2160`
  leaves the instance smaller than the coded picture → `OVERSIZE_DECODE`.
- 1080p worked by luck: 1080 rounds up to 1088 (17 × 64), which the previous
  8/16-alignment also produced.

### Fix

Align HEVC `maxFrameWidth`/`maxFrameHeight` to the **CTU size (64)**:

```c
dc->maxFrameWidth  = (w + 63) & ~63;   // 3840 -> 3840
dc->maxFrameHeight = (h + 63) & ~63;   // 2160 -> 2176, 1080 -> 1088
```

The library additionally aligns `maxFrameWidth` to 256 and `maxFrameHeight` to
8 internally; aligning to 64 here is a superset (64 is a multiple of 8), so the
configured values are never reduced below the coded size.

> **Note:** the SDK *preset* (`SCE_VIDEODEC2_PRESET_HEVC_2160P_MAIN_L5_1_DPB6`)
> sets `maxFrameHeight = 2160` exactly. That preset value is the *display*
> height, not the coded height; the decoder core still needs the CTU-aligned
> value. Do not copy the preset's frame dimensions verbatim for HEVC.

---

## 4. The GPU texture width must equal the decoder's reported row pitch

This is the finding that fixed the HEVC display banding and the AVC 1080p
banding. It is the most subtle of the four.

### Symptom

- HEVC 1080p: horizontal banding (~100×10 px blocks) that grew down the frame.
- Later, after a partial fix: luma uniformly compressed horizontally (black bar
  at the right) and chroma progressively shifted left→right.
- AVC 1080p: banding with thin lines.

### Root cause

For a `GNM_ARRAY_LINEAR_GENERAL` texture, **the GPU derives the level-0 row
stride from the texture WIDTH, not from `ci->pitch`.** In OpenGNM:

- `sceGpaTpInit()` sets `tp.linearwidth = tex->width`.
- `gpaComputeSurfaceAddrFromCoordLinear()` uses `linearwidth` as the row pitch.
- `ci->pitch` (`basetiledpitch`) is only consulted for **mip levels > 0**.

So the texture width **is** the row stride. If the texture width does not equal
the decoder's actual row stride, the GPU reads each row at the wrong stride,
producing a shear/banding that grows down the frame.

The decoder's row stride is exactly the pitch it reports in
`SceVideodec2OutputInfo.framePitchInBytes`. **This is not a fixed value** — the
decoder chooses the surface pitch per stream/codec. Measured values:

| Codec / resolution | Reported `framePitchInBytes` | True row stride (verified from a raw dump) |
|--------------------|------------------------------|--------------------------------------------|
| AVC 1080p | 1920 | 1920 (tightly packed, no padding) |
| HEVC 1080p | 2048 | 2048 (256-aligned, padded) |
| 4K (AVC & HEVC) | 3840 | 3840 |

Two wrong assumptions were tried and rejected:

1. **Using the visible width (1920) as the texture width.** Wrong for HEVC
   (stride 2048) → banding.
2. **Rounding the pitch up to a 256-aligned value.** Wrong for AVC 1080p
   (stride 1920, which is *not* 256-aligned) → banding with thin lines.

### Fix

Use the **reported pitch** as the texture width, and sample only the visible
fraction in the shader:

```c
// Y texture: width = reported pitch (the true row stride in texels).
const uint32_t yTexWidth = (frame.pitch >= frame.width) ? frame.pitch
                                                        : frame.width;
yCi.width = yTexWidth;   // row stride in texels
yCi.pitch = yTexWidth;

// UV texture: width = pitch/2 (RG8 = 2 bytes/texel).
uvCi.width = yTexWidth / 2;
uvCi.pitch = yTexWidth / 2;

// uvScale.x = visibleWidth / pitch, applied to BOTH planes in the shader.
const float uvScaleX = (float)frame.width / (float)yTexWidth;
```

The pixel shader samples only the leftmost visible fraction of the
pitch-wide texture:

```hlsl
float2 yCoord  = input.uv * uvScale.xy;                       // Y plane
float2 uvCoord = input.uv * uvScale.xy + chromaOffset.xy;     // UV plane
float  y  = texY.Sample(smpY, yCoord).r;
float2 uv = texUV.Sample(smpUV, uvCoord).rg;
```

### Chroma siting

The decoder outputs NV12 with **left-sited** (MPEG-2 style) 4:2:0 chroma: the
chroma sample for output pixels `(2k, 2k+1)` is UV texel `k`. With point
sampling, sampling the UV texture at `uvCoord.x = x/2` (in UV texels) selects
exactly texel `floor(x/2)`, which is correct. **No half-texel offset is
needed** — adding one shifts odd output pixels onto the next chroma texel (a
1-texel comb/zigzag artifact). `chromaOffset` is therefore `(0, 0, 0, 0)`.

### Tile mode

Use `GNM_TM_DISPLAY_LINEAR_GENERAL` for the Y/UV textures (not
`GNM_TM_DISPLAY_LINEAR_ALIGNED`). `LINEAR_ALIGNED` imposes pitch/base-address
alignment constraints the decoder's buffer does not satisfy;
`LINEAR_GENERAL` has no such constraint and matches the decoder's plain
row-major layout.

### How this was diagnosed

A one-time raw plane dump was added to the renderer (writing the Y plane and
the region treated as the UV plane to `/data/moonlight/*.raw`), then analyzed
offline. The decisive test was to **scan candidate strides** and pick the one
that minimizes row-to-row discontinuity:

```python
# For a correct stride S, consecutive rows are similar (low discontinuity).
def score(S, rows=200, x0=200, x1=1700):
    tot = 0.0
    for r in range(rows):
        a = data[r*S + x0 : r*S + x1]
        b = data[(r+1)*S + x0 : (r+1)*S + x1]
        tot += mean(abs(a - b))
    return tot / rows
# Lowest score wins: 1920 for AVC 1080p, 2048 for HEVC 1080p.
```

This is the recommended technique for any future "wrong stride" symptom: dump
the raw plane and scan strides rather than guessing.

---

## 5. Process lesson: a stale shader binary masked a correct fix

After the renderer was changed to apply `uvScale` to the Y plane (finding 4),
the display was **still** compressed. The renderer code was correct; the
problem was that **`video_p.sb` had not been rebuilt** after `video_p.pssl`
changed. The running shader was the old version that sampled the Y plane with
the raw `input.uv` (full `[0,1]` range), ignoring `uvScale`.

**Lesson:** the `.sb` shader binaries are build artifacts. Whenever a `.pssl`
source changes, the corresponding `.sb` **must** be recompiled and repackaged.
A stale `.sb` produces symptoms that look like renderer bugs. Check the
timestamps of `shaders/*.pssl` vs `pkg_template/assets/misc/*.sb` when a shader
change appears to have no effect.

---

## 6. Summary of the HEVC implementation

### Decoder configuration (`VideoDecoder.cpp`)

- **Codec:** `ORBIS_VIDEODEC2_CODEC_HEVC` (974921 / `0xEE049`).
- **Profile:** `ORBIS_VIDEODEC2_HEVC_PROFILE_MAIN` (1).
- **Level:** max of the resolution floor and the bitrate requirement (see §2).
- **Frame dimensions:** CTU-aligned to 64 (see §3).
- **DPB:** 6 (all HEVC SDK presets use 6).
- **`extraConfigInfo`:** a populated `SceVideodec2HevcExtraConfigInfo` (see §1).
- **`checkMemoryType`:** `false` for HEVC (the HEVC SDK preset does not check;
  the AVC preset does).

### Renderer (`GpuVideoRenderer.cpp` + `video_p.pssl`)

- Y/UV texture width = the decoder's reported `framePitchInBytes` (see §4).
- `uvScale.x = visibleWidth / pitch`, applied to both planes.
- `chromaOffset = 0` (left-sited chroma).
- `GNM_TM_DISPLAY_LINEAR_GENERAL` tile mode.

### Verified configurations

All four combinations are hardware-verified clean:

| | AVC | HEVC |
|---|---|---|
| **1080p** | ✓ (stride 1920) | ✓ (stride 2048) |
| **4K** | ✓ (stride 3840) | ✓ (stride 3840, CTU-aligned 2176) |

### Error-code reference

| Code | Name | Meaning |
|------|------|---------|
| `0x811D0300` | `NEW_SEQUENCE` | Input stream changed sequence; reset the decoder. |
| `0x811D0301` | `ACCESS_UNIT` | Decode error in an input access unit. |
| `0x811D0302` | `OVERSIZE_DECODE` | Stream exceeds the configured instance (level, bitrate, or coded size). |
| `0x811D0303` | `INVALID_SEQUENCE` | No valid sequence found in the input stream. |
| `0x811D0304` | `FATAL_STREAM` | Fatal stream error; reset the decoder. |

`OVERSIZE_DECODE` is the code to watch for: it is returned for a level too low
for the bitrate (§2), a coded size larger than the configured frame dimensions
(§3), or a DPB too small for the stream.

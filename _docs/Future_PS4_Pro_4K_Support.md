# Future Roadmap: PS4 Pro 4K / H.265 Streaming Support

## Status

**Roadmap / Future Feature — NOT scheduled for the current milestone.**

This document captures the design discussion and likely requirements for adding
4K (3840×2160) H.265 (HEVC) streaming support on the PlayStation 4 Pro. It is a
forward-looking reference for a future effort, not an active work item.

---

## 1. Motivation

The current client streams **1080p60 H.264** and is hardware-verified on that
configuration. The decode pipeline runs at `kDecodePipelineDepth = 1` (one frame
in flight at a time), which is functionally correct and keeps up at 60fps
(10–15 ms decode per frame vs. the 16.7 ms frame budget).

The only scenario that would justify increasing the decode pipeline depth is
**4K streaming**, where the per-frame decode cost roughly quadruples (4× the
pixels). At 4K60, the decode would exceed the frame budget at depth 1, making
decode throughput the bottleneck.

Two facts make 4K viable on PS4 Pro specifically:

- **Videodec2 hardware supports H.265 (HEVC) decode on the PS4 Pro.**
- **The PS4 Pro CPU is more capable** for the CPU-side NV12→BGRA presentation
  conversion (4× the pixels of 1080p).

---

## 2. Why H.265 (not H.264) for 4K

H.264 4K60 is impractical:

- Requires very high bitrate (40–80+ Mbps) for acceptable quality.
- High encode latency on the host.
- Poor quality-per-bitrate compared to HEVC.
- The PS4 Pro's Videodec2 H.264 level (Level 5.1) may not support 4K60.

H.265 (HEVC) 4K60 is the right choice:

- ~20–40 Mbps (roughly half of H.264 for the same quality) — well within
  Gigabit Ethernet capability.
- Better quality-per-bitrate.
- The PS4 Pro's Videodec2 supports H.265 decode.

---

## 3. The Decode Pipeline Depth Question

Deocde pipeline depth and AU lifetime contract have been updated for V1.0.0, so kDecodePipelineDepth = 2, which results in a decode latency of 1-2 ms.

<s>
### Current configuration

- `kDecodePipelineDepth = 1` (one frame in flight at a time).
- Each `Decode()` call is synchronous and takes 10–15 ms for 1080p60 H.264.
- The decode worker runs in parallel with the main-thread presentation, so the
  decode time is hidden behind presentation as long as it keeps up.

### Why depth 1 was chosen

Depth 1 was deliberately selected to avoid **stale-content / frame-lifetime
corruption** that moves with animated content. The root cause of that corruption
is **not** the pipeline depth itself, but a missing implementation of the SDK's
AU (access unit) input-buffer lifetime contract.


### The SDK AU lifetime contract

The Videodec2 SDK requires (per the SCE reference):

> "the access unit input with `sceVideodec2Decode()` must be retained until one
> cycle of processing completes. In other words... it must be protected until
> the number of `sceVideodec2Decode()` calls starting with the call that input
> the corresponding access unit reaches to the number set to
> `decodePipelineDepth`."

Interpretation:
- The submitting `Decode()` call counts as call 1.
- An AU submitted at decode call S must be retained through calls
  S .. S+(decodePipelineDepth-1).
- It may be released once the sequence reaches S + decodePipelineDepth.
- Maximum simultaneously retained AUs == decodePipelineDepth.

### Why depth > 1 currently corrupts

Our decoder does **not** retain the AU input buffer after the `Decode()` call.
The `unit.data` is a local variable that goes out of scope at the end of the
decode-loop iteration.

- With `decodePipelineDepth = 1`, this is fine — the AU only needs to remain
  valid for the current synchronous call.
- With `decodePipelineDepth = 2`, the SDK requires the AU to remain valid for
  **2 decode calls**. Since our decoder frees `unit.data` at the end of the
  first iteration, the second decode call reads **freed memory**, producing the
  stale-content corruption.

### The reference's solution

The SCE reference implements AU retention with `retainAu()` / `retireExpiredAus()`:

- After each `Decode()` call, it **moves** `unit.data` into a retained set.
- It records the submission sequence number.
- It retires each AU once `submittedAtSeq + decodePipelineDepth <= currentSeq`.

This keeps the AU input buffer alive for the full frame-parallel lifetime the
SDK requires, so the reference can run at depth 2 without corruption.

### Does depth 2 reduce latency?

**No.** Increasing the pipeline depth does **not** meaningfully reduce felt
visual latency:

- The decode worker and main-thread presentation run in parallel.
- The decode already keeps up at 60fps with depth 1 (10–15 ms < 16.7 ms).
- End-to-end latency is dominated by network transmission + presentation, not
  decode throughput.

Depth 2 provides **decode throughput headroom** (useful for 4K), not lower latency.
</s>



---

## 4. Likely Requirements for 4K / H.265 Support

### 4.1 Decode pipeline depth (prerequisite for 4K decode throughput)

<s>Implement the AU input-buffer retention (mirroring the SCE reference's
`retainAu` / `retireExpiredAus`) so the decoder can safely run at
`kDecodePipelineDepth = 2` (or higher) without stale-content corruption.</s>

DONE in v1.0

This is the **decode-throughput enabler** for 4K60.

### 4.2 HEVC (H.265) decoder support

- Add H.265 / HEVC codec support to `VideoDecoder` (currently hardcoded to
  H.264/AVC).
- Configure the Videodec2 decoder for HEVC (codec type, profile, level).
- Verify the PS4 Pro's Videodec2 H.265 decode level supports 4K60.
- Handle HEVC-specific SPS/PPS (the decoder consumes the host's VPS/SPS/PPS
  as-is; no bitstream rewrite is performed).

### 4.3 Stream negotiation

- Advertise HEVC support in the moonlight-common-c stream negotiation so the
  host (Sunshine) encodes H.265.
- Ensure the host is configured to encode H.265 4K60.

### 4.4 Presentation (NV12→BGRA)

- The decoded output for both H.264 and H.265 is NV12, so the existing
  NV12→BGRA conversion path applies unchanged.
- The PS4 Pro's stronger CPU is expected to handle the 4× pixel count, but the
  conversion cost should be measured and optimized if needed.

### 4.5 Network

- Gigabit Ethernet can handle 20–40 Mbps H.265 4K60.
- Wi-Fi (802.11ac) may be marginal; wired is recommended.
- Network jitter sensitivity increases with higher bitrate; consider the
  existing RTT/jitter measurement (`LiGetEstimatedRttInfo`) for diagnostics.

### 4.6 Settings / UI

- Add a resolution option for 4K (3840×2160) when on PS4 Pro hardware.
- Consider a codec preference (H.264 vs H.265) setting.
- Gate 4K options to PS4 Pro hardware (base PS4 cannot decode 4K H.265).

---

## 5. Risks / Open Questions

- **Decode level support**: Confirm the PS4 Pro's Videodec2 H.265 level supports
  4K60 (Level 5.1 / 5.2).
- **AU retention correctness**: Porting the reference's AU retention must be
  done carefully to avoid reintroducing stale-content corruption.
- **Encode latency**: H.265 4K60 encoding on the host adds latency; verify it is
  acceptable for interactive streaming.
- **Presentation cost**: Measure NV12→BGRA conversion time for 4K on PS4 Pro.
- **moonlight-common-c HEVC support**: Confirm the pinned revision supports HEVC
  negotiation and decode-unit delivery for H.265.

---

## 6. Summary

- **Current**: <s>1080p60 H.264 at `kDecodePipelineDepth = 1` — correct and keeps up.</s>
- **Future**: 4K60 H.265 on PS4 Pro — viable because the hardware supports H.265
  decode and the CPU is stronger, but requires:
  1. <s>AU input-buffer retention to safely enable pipeline depth 2+.</s>  
  DONE in v1.0
  2. HEVC decoder support.
  3. HEVC stream negotiation.
  4. 4K presentation cost verification.
  5. PS4 Pro hardware gating.

The AU retention work is a **prerequisite** for 4K decode throughput, but it is
**not** needed for the current 1080p60 setup <s>and carries risk of reintroducing
corruption if not implemented carefully. It should be scheduled as part of the
4K H.265 effort, not as a standalone change.</s>

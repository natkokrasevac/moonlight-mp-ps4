# OpenGNM Shader Resource Binding — Practical Guide

**Status:** Hardware-verified on PS4 (OpenGNM + Sony PSSL toolchain).
**Scope:** A self-contained reference for setting up OpenGNM shader resource
binding in a new project: how to compile PSSL shaders, parse the `.sb`
input-usage table, bind resources to PS user-data registers, and use Extended
User Data (EUD). Includes the two hard-won findings that caused GPU page
faults until understood:

1. **OpenGNM's `Set*UserData` slot parameter is the PS user-data REGISTER
   INDEX directly (0–15), NOT a 4-DWORD slot.**
2. **How EUD (Extended User Data) works and how to lay out the EUD block.**

This guide is the distilled result of debugging a fullscreen NV12→BGRA GPU
video pass (and a GPU alpha-blend overlay pass) on the PS4.

---

## 1. Toolchain Overview

Two shader toolchains are in play:

| Tool | Purpose |
|------|---------|
| **Sony `orbis-wave-psslc.exe`** (via `psslc.sh`) | Compiles **PSSL** (`.pssl`) → `.sb` shader binary. This is what the production shaders use. |
| **`opengnm-psbc`** | Compiles GLSL → SPIR-V → `.sb`. Used by OpenGNM's own tests. |

The production project uses **PSSL** compiled with the Sony compiler. The
`psslc.sh` wrapper invokes the Windows `orbis-wave-psslc.exe` under Wine:

```sh
# psslc.sh — wrapper
exec wine --bottle Development \
    '.../orbis-wave-psslc.exe' "$@"
```

### 1.1 Compiling a PSSL shader

```sh
# Vertex shader (profile sce_vs_vs_orbis, suffix _vv)
psslc.sh -profile sce_vs_vs_orbis -o pkg_template/assets/misc/video_vv.sb \
         shaders/video_vv.pssl

# Pixel shader (profile sce_ps_orbis, suffix _p)
psslc.sh -profile sce_ps_orbis -o pkg_template/assets/misc/video_p.sb \
         shaders/video_p.pssl
```

The profile suffix convention is baked into the output filename convention
(`_vv` = vertex, `_p` = pixel). Supported profiles include `sce_vs_vs_orbis`,
`sce_ps_orbis`, `sce_cs_orbis`, `sce_gs_orbis`, etc.

### 1.2 Forcing resources into EUD (diagnostic)

The Sony compiler **compacts unused register slots** — declaring
`register(t2)` does NOT force a resource to a high register. To force a
resource beyond the direct 16-DWORD PS user-data area (into EUD), compile with:

```sh
psslc.sh -profile sce_ps_orbis -no-resource-in-user-sgprs \
         -o pkg_template/assets/misc/video_testEUD_p.sb \
         shaders/video_testEUD_p.pssl
```

`-no-resource-in-user-sgprs` disallows resources from occupying the direct
user-data SGPR registers, forcing the compiler to emit `PTR_EXTENDEDUSERDATA`
and place resources in EUD. This was the key to building a minimal EUD
diagnostic shader.

---

## 2. The `.sb` Shader Binary Format

The `.sb` file is parsed at runtime to build the `GnmVsShader` /
`GnmPsShader` register structs and to discover the resource layout. The layout
(after the PSSL header) is:

```
PsslBinaryHeader        (0x24 bytes)
GnmShaderFileHeader     (0x10 bytes, magic 0x72646853 = "Shdr")
GnmShaderCommonData     (0x08 bytes)
sbaddr[2]               (2 x uint32: code offset, patch flag)
... shader code ...
... input-usage table ...
... semantic tables ...
```

### 2.1 Parsing helpers (from `gnm_shaderbinary.h`)

```c
// After loading the .sb into a GnmPsShader* (see §3):
const GnmInputUsageSlot* usage =
    sceGnmPsShaderInputUsageSlotTable(ps);   // array of input-usage slots
uint32_t n = ps->common.numinputusageslots; // count

// Each GnmInputUsageSlot (4 bytes):
//   usagetype     (uint8)  — GnmShaderInputUsageType
//   apislot       (uint8)  — API slot index
//   startregister (uint8)  — starting PS user-data register
//   registercount / chunkmask / srtdwordsminusone (uint8)
```

### 2.2 The input-usage table is GROUND TRUTH

**Always parse the `.sb` input-usage table at runtime and bind resources
according to the compiler-reported `startregister` values.** Do not hardcode
offsets. The compiler decides where each resource lives; the table tells you.

Example parsed table for the full NV12 pixel shader (`video_p.sb`):

```
numinputusageslots = 6
[0] IMM_RESOURCE        apislot=0 startreg=0    (texY,   T#)
[1] IMM_SAMPLER         apislot=0 startreg=8    (smpY,   S#)
[2] PTR_EXTENDEDUSERDATA apislot=1 startreg=12  (EUD pointer)
[3] IMM_RESOURCE        apislot=1 startreg=16   (texUV,  T#, in EUD)
[4] IMM_SAMPLER         apislot=1 startreg=24   (smpUV,  S#, in EUD)
[5] IMM_CONSTBUFFER     apislot=0 startreg=28   (cb_video, V#, in EUD)
```

---

## 3. Loading a `.sb` into a Register Struct

The shader code is copied into GPU-visible (WC_GARLIC) memory and the
`GnmVsShader`/`GnmPsShader` register structs are built with the code address
patched in.

```c
// Read the .sb file into a malloc'd buffer.
// Parse the header to find the code pointer and the shader struct size.
// Copy the code into a WC_GARLIC block (256-byte aligned).
// Build the register struct and set the code address:

void* vsgpucode = garlic->alloc(codesize, GNM_ALIGNMENT_SHADER_BYTES);
memcpy(vsgpucode, code, codesize);
sceGnmVsRegsSetAddress(&vs->registers, vsgpucode);

// For PS:
sceGnmPsRegsSetAddress(&ps->registers, psgpucode);
ps->registers.spibaryccntl = 0;   // important for PS
```

**Critical:** after copying shader code (and any descriptor/constant data) into
WC_GARLIC, call `sceGnmFlushGarlic()` so the GPU sees the writes. WC writes are
write-combining and must be flushed before the GPU reads them, otherwise the
GPU may fetch garbage shader code (which can fault reading address 0).

---

## 4. ⚠️ THE SLOT CONVENTION FIX (root cause of GPU page faults)

### The bug

OpenGNM's `Set*UserData` functions take a `startuserdataslot` parameter. The
natural assumption is that this is a "slot" in the 4-DWORD sense (like the
shader's `apislot`). **This is WRONG.**

### The fix

**`startuserdataslot` is the PS user-data REGISTER INDEX directly (0–15).**

Looking at OpenGNM's `drawcommandbuffer.c`:

```c
static inline uint32_t getuserdataslot(GnmShaderStage stage, uint32_t slot) {
    // ...
    return basereg + (slot << 2);   // slot is the register index
}
```

The PS user-data registers are 4 bytes apart:
```
SPI_SHADER_USER_DATA_PS_0  = 0xB030
SPI_SHADER_USER_DATA_PS_1  = 0xB034
...
SPI_SHADER_USER_DATA_PS_12 = 0xB060
...
SPI_SHADER_USER_DATA_PS_15 = 0xB06C
```

So passing `N` places the descriptor at register `N`. **The shader's
`startregister` value IS the value to pass to `Set*UserData`.**

### The symptom

The renderer was passing `startreg / 4` (treating it as a 4-DWORD slot). This
silently mis-bound every resource whose register was not a multiple of 4:

| Resource | shader `startreg` | wrong slot (`/4`) | actual register written | correct slot |
|----------|-------------------|-------------------|-------------------------|--------------|
| texY     | 0                 | 0 ✓               | 0 ✓                     | 0            |
| smpY     | 8                 | 2 ✗               | 2 ✗                     | **8**        |
| EUD ptr  | 12                | 3 ✗               | 3 ✗                     | **12**       |

- **Test A (direct T#/S#) passed anyway** because the shader read a *zeroed*
  sampler from register 2 (a zero sampler ≈ point/clamp, visually identical to
  the intended sampler).
- **Test B / full NV12 crashed** because the shader read a *zeroed* EUD pointer
  from register 3 instead of 12 → descriptor fetch from VA 0 →
  `GPU_FAULT_PAGE_FAULT_ASYNC`.

### Correct binding

```c
// Pass the shader's startregister value directly:
sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0,  &yTex);   // startreg 0
sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8,  &samplerY); // startreg 8
sceGnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_PS, 12, eudMem);   // startreg 12
```

**Rule of thumb:** the `startuserdataslot` argument to every
`sceGnmDrawCmdSet*UserData` call equals the shader's `startregister` for that
resource. Never divide by 4.

---

## 5. Extended User Data (EUD)

### 5.1 What EUD is

The PS has **16 direct user-data registers** (registers 0–15). Resources that
don't fit (or that the compiler chooses to place beyond register 15) live in
an **Extended User Data (EUD)** block — a tightly-packed region of GPU memory
holding descriptors. The shader accesses EUD resources through a pointer bound
at a direct user-data register.

### 5.2 The EUD pointer

The compiler emits a `PTR_EXTENDEDUSERDATA` input-usage slot. Its
`startregister` is the direct user-data register that holds the **GPU address
of the EUD block**. In the NV12 shader this is register 12.

Bind the EUD block address with:

```c
sceGnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_PS, 12, eudMem);
```

`SetPointerUserData` writes `sizeof(ptr)/4 = 2` DWORDs (the full 64-bit
pointer) at the target register.

### 5.3 EUD block layout

Resources in EUD are laid out at **DWORD offsets = `startregister - 16`**
(relative to the EUD block base). The EUD pointer register is the base; EUD
resources are indexed relative to it.

For the NV12 shader (EUD pointer at register 12):

| Resource | `startreg` | EUD DWORD offset (`startreg - 12`) | EUD byte offset | Size |
|----------|-----------|------------------------------------|-----------------|------|
| texUV (T#) | 16 | 4 | 16 | 8 DWORDs (32 B) |
| smpUV (S#) | 24 | 12 | 48 | 4 DWORDs (16 B) |
| cb_video (V#) | 28 | 16 | 64 | 4 DWORDs (16 B) |

> **Note:** The "established" EUD layout in this project packs texUV at byte 0,
> smpUV at byte 32, cb_video at byte 48 (i.e. `startreg - 16` DWORD offsets,
> NOT `startreg - 12`). The exact base convention depends on the shader
> compiler's EUD base. **Always derive EUD offsets from the compiler-reported
> `startreg` values and verify empirically** — the minimal EUD diagnostic
> shader (below) is the way to pin this down for a given toolchain.

### 5.4 Building the EUD block

The EUD block is a CPU-written GPU-visible buffer (WC_GARLIC). Write the
descriptors at the computed offsets and flush:

```c
// EUD block in WC_GARLIC, 256-byte aligned.
uint8_t* eud = (uint8_t*)eudMem;
memcpy(eud + texEudByteOffset, &uvTex, sizeof(uvTex));       // T# descriptor
memcpy(eud + smpEudByteOffset, &samplerUV, sizeof(samplerUV)); // S# descriptor
GnmBuffer cbBuf = sceGnmCreateConstBuffer(cbMem, 0x1000);
memcpy(eud + cbEudByteOffset, &cbBuf, sizeof(cbBuf));        // V# descriptor
sceGnmFlushGarlic();   // make the EUD block GPU-visible
```

### 5.5 Minimal EUD diagnostic shader (how to isolate EUD)

To prove EUD works in isolation, compile a shader with exactly ONE texture +
ONE sampler forced into EUD (`-no-resource-in-user-sgprs`), parse its
input-usage table, derive EUD offsets as `startreg - 16`, bind the EUD pointer
at the reported direct register, and sample the texture. If it renders, EUD is
valid; if it faults at VA 0, the EUD pointer/encoding is wrong.

---

## 6. Resource Binding Summary

### 6.1 Direct resources (registers 0–15)

```c
// T# (texture), S# (sampler), V# (buffer) bound directly:
sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, startreg, &tex);
sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, startreg, &sampler);
sceGnmDrawCmdSetVsharpUserData(&cmd, GNM_STAGE_PS, startreg, &buffer);
```

### 6.2 EUD resources (registers ≥ 16)

1. Bind the EUD block address at the `PTR_EXTENDEDUSERDATA` register.
2. Write each EUD resource's descriptor into the EUD block at the
   compiler-derived offset.
3. Flush the EUD block.

### 6.3 VS→PS varyings

Wire the VS's exported varyings to the PS's inputs so the PS can read
interpolated UV/etc.:

```c
sceGnmDrawCmdSetPsInputUsage(
    &cmd,
    sceGnmVsShaderExportSemanticTable(vs), vs->numexportsemantics,
    sceGnmPsShaderInputSemanticTable(ps), ps->numinputsemantics);
```

---

## 7. Texture / Sampler / Buffer Descriptor Setup

### 7.1 Textures (T#)

```c
GnmTextureCreateInfo ci;
memset(&ci, 0, sizeof(ci));
ci.texturetype = GNM_TEXTURE_2D;
ci.width  = width;          // texels
ci.height = height;         // texels
ci.depth  = 1;
ci.pitch  = pitchInTexels;  // NOTE: texels, not bytes
ci.nummiplevels = 1;
ci.numslices = 1;
ci.format = GNM_FMT_R8_UNORM;   // or GNM_FMT_R8G8B8A8_UNORM, etc.
ci.tilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
ci.mingpumode = GNM_GPU_BASE;
ci.numfragments = 1;
GnmError err = sceGnmCreateTexture(&tex, &ci);
sceGnmTexSetBaseAddress(&tex, baseAddr);   // must be 256-byte aligned
sceGnmTexSetMemoryType(&tex, GNM_MEMORY_READONLY, false);
```

**Pitch is in TEXELS** (the T# `pitch` field is `pitch+1` texels). For an R8
texture, pitch texels == pitch bytes. For an RGBA8 texture, pitch texels =
pitch bytes / 4.

**Base address must be 256-byte aligned** (`sceGnmTexSetBaseAddress` rejects
unaligned addresses).

### 7.2 Samplers (S#)

```c
GnmSampler sampler;
memset(&sampler, 0, sizeof(sampler));
sampler.clampx = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
sampler.clampy = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
sampler.clampz = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
sampler.xymagfilter = GNM_FILTER_POINT;   // or GNM_FILTER_BILINEAR
sampler.xyminfilter = GNM_FILTER_POINT;
sampler.mipfilter = GNM_MIPFILTER_NONE;
```

### 7.3 Constant buffers (V#)

```c
GnmBuffer cb = sceGnmCreateConstBuffer(cbMem, sizeBytes);
```

### 7.4 Render target (over the WC_GARLIC framebuffer)

```c
GnmRenderTargetCreateInfo rtCi;
memset(&rtCi, 0, sizeof(rtCi));
rtCi.width = 1920;
rtCi.height = 1080;
rtCi.pitch = 0;
rtCi.numslices = 1;
rtCi.colorfmt = GNM_FMT_R8G8B8A8_UNORM;   // byte order R,G,B,A
rtCi.colortilemodehint = GNM_TM_DISPLAY_LINEAR_ALIGNED;
rtCi.mingpumode = GNM_GPU_BASE;
rtCi.numsamples = 1;
rtCi.numfragments = 1;
GnmError err = sceGnmCreateRenderTarget(&rt, &rtCi);
// Per frame, set the base to the current framebuffer slot:
sceGnmRtSetBaseAddr(&rt, framebuffer);
```

---

## 8. Draw Command Buffer Sequence

```c
GnmCommandBuffer cmd = sceGnmCmdInit(cmdMem, cmdBytes, NULL, NULL);
sceGnmDrawCmdInitDefaultHardwareState(&cmd);

// Primitive setup (no cull).
GnmPrimitiveSetup ps; memset(&ps, 0, sizeof(ps));
ps.cullmode = GNM_CULL_NONE; ps.frontface = GNM_FACE_CCW;
ps.frontmode = GNM_FILL_SOLID; ps.backmode = GNM_FILL_SOLID;
sceGnmDrawCmdSetPrimitiveSetup(&cmd, &ps);

sceGnmDrawCmdSetRenderTarget(&cmd, 0, &rt);
sceGnmDrawCmdSetRenderTargetMask(&cmd, 0xf);

// Viewport (positive Y scale).
GnmSetViewportInfo vp; memset(&vp, 0, sizeof(vp));
vp.dmin = 0; vp.dmax = 1;
vp.scale[0] = fbW*0.5f; vp.scale[1] = fbH*0.5f; vp.scale[2] = 0.5f;
vp.offset[0] = fbW*0.5f; vp.offset[1] = fbH*0.5f; vp.offset[2] = 0.5f;
sceGnmDrawCmdSetViewport(&cmd, 0, &vp);

// Viewport transform control.
GnmViewportTransformControl vt; memset(&vt, 0, sizeof(vt));
vt.scalex=1; vt.offsetx=1; vt.scaley=1; vt.offsety=1;
vt.scalez=1; vt.offsetz=1; vt.invertw=1;
sceGnmDrawCmdSetViewportTransformControl(&cmd, &vt);
sceGnmDrawCmdSetScreenScissor(&cmd, 0, 0, fbW, fbH);

sceGnmDrawCmdSetVsShader(&cmd, &vs->registers, 0);
sceGnmDrawCmdSetPsShader(&cmd, &ps->registers);
sceGnmDrawCmdSetPsInputUsage(&cmd, vsExport, nVs, psInput, nPs);

// Bind resources (see §6).
sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0, &yTex);
sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8, &samplerY);
sceGnmDrawCmdSetPointerUserData(&cmd, GNM_STAGE_PS, 12, eudMem);

sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
sceGnmDrawCmdDrawIndexAuto(&cmd, 3);

// EOP sync.
*eopLabel = 0;
sceGnmDrawCmdEventWriteEop(&cmd, GNM_FLUSH_AND_INV_CB_DATA_TS,
                           (uint64_t)(uintptr_t)eopLabel,
                           GNM_DATA_SEL_SEND_DATA64, 1);

// Submit.
uint32_t dcbSize = (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr);
void* dcbAddrs[1] = {cmd.beginptr};
uint32_t dcbSizes[1] = {dcbSize};
sceGnmSubmitCommandBuffers(1, dcbAddrs, dcbSizes, NULL, NULL);
sceGnmSubmitDone();

// Wait for EOP.
while (*eopLabel != 1) { /* spin */ }
```

---

## 9. GPU Alpha-Blend Overlay (source-over)

To GPU-blend an overlay over the video in the SAME submission (flicker-free),
append a second draw to the same command buffer after the video draw:

```c
// Enable source-over alpha blend (straight alpha):
// out = src*srcA + dst*(1-srcA).
GnmBlendControl blend; memset(&blend, 0, sizeof(blend));
blend.blendenabled = true;
blend.colorfunc = GNM_COMB_DST_PLUS_SRC;
blend.colorsrcmult = GNM_BLEND_SRC_ALPHA;
blend.colordstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
blend.alphafunc = GNM_COMB_DST_PLUS_SRC;
blend.alphasrcmult = GNM_BLEND_SRC_ALPHA;
blend.alphadstmult = GNM_BLEND_ONE_MINUS_SRC_ALPHA;
blend.separatealphaenable = false;
sceGnmDrawCmdSetBlendControl(&cmd, 0, &blend);

// Bind the overlay VS/PS + overlay texture/sampler, draw a fullscreen triangle.
sceGnmDrawCmdSetVsShader(&cmd, &overlayVs->registers, 0);
sceGnmDrawCmdSetPsShader(&cmd, &overlayPs->registers);
sceGnmDrawCmdSetPsInputUsage(&cmd, overlayVsExport, nVs, overlayPsInput, nPs);
sceGnmDrawCmdSetTsharpUserData(&cmd, GNM_STAGE_PS, 0, &overlayTex);
sceGnmDrawCmdSetSsharpUserData(&cmd, GNM_STAGE_PS, 8, &overlaySampler);
sceGnmDrawCmdSetPrimitiveType(&cmd, GNM_PT_TRILIST);
sceGnmDrawCmdDrawIndexAuto(&cmd, 3);
```

**Why single submission matters:** if the video draw and the overlay blend are
in SEPARATE submissions, the overlay's destination reads (the video pixels it
blends over) can race with the video draw's writes at the hardware level →
**flicker at the overlay boundary**. Putting both draws in ONE command buffer
lets the GPU execute them in program order, guaranteeing the overlay sees the
completed video.

**Staging buffer for CPU-written overlays:** if the overlay is CPU-composited
(Onion memory), copy it into a WC_GARLIC staging buffer on change and flush,
then sample from the staging buffer. This avoids Onion/GPU cache-coherency
issues. The copy only happens on overlay content change (rare), so per-frame
cost is zero.

### 9.1 Overlay staging buffer memory requirement (⚠️ 4K scaling)

The overlay staging buffer is a **full-resolution RGBA8 surface** — one
4-byte pixel per framebuffer pixel. The GPU blend pass samples it 1:1 over the
whole screen, so it must be the same size as the framebuffer.

```
overlay staging bytes = framebufferWidth × framebufferHeight × 4
```

| Resolution | Overlay staging buffer |
|------------|------------------------|
| **1920 × 1080** (current) | 1920 × 1080 × 4 = **8,294,400 B (~8.3 MB)** |
| **3840 × 2160** (4K) | 3840 × 2160 × 4 = **33,177,600 B (~33.2 MB)** |

**Implications for a future 4K client:**

1. **The staging buffer must be allocated at the target resolution.** It is a
   dedicated WC_GARLIC block sized to `width × height × 4`. A 4K client needs
   a ~33 MB block (4× the 1080p size). Do not assume a fixed 1920×1080 buffer.
2. **The overlay compositor (CPU Onion surface) must also be full-resolution.**
   Both the CPU compositor that the overlay renderers draw into and the GPU
   staging buffer it is copied into scale with resolution.
3. **The per-change copy cost scales with resolution.** Copying the full
   surface on overlay change is ~8 MB at 1080p and ~33 MB at 4K. This is still
   acceptable because it only happens on overlay content change (rare), not per
   frame — but the copy should be sized/looped over the actual resolution, not
   a hardcoded 1920×1080.
4. **The overlay blend pass is a fullscreen draw** — it naturally scales with
   the viewport/framebuffer dimensions (the fullscreen triangle covers
   whatever the viewport is), so no shader change is needed for 4K. Only the
   staging buffer and compositor allocations must grow.

**Design rule:** derive the overlay staging buffer size (and the CPU overlay
compositor size) from the actual framebuffer width/height at allocation time,
never hardcode 1920×1080. This keeps the overlay path resolution-agnostic and
ready for a 4K client.

---

## 10. Memory Types & Coherency

| Memory | Use | Notes |
|--------|-----|-------|
| `ORBIS_KERNEL_WC_GARLIC` | Shader code, command buffer, constant buffer, EUD block, overlay staging | GPU-visible. CPU writes must be flushed with `sceGnmFlushGarlic()`. |
| `ORBIS_KERNEL_WB_ONION` | EOP sync label, decoded NV12 planes, CPU overlay compositor | CPU-cacheable. GPU can read Onion surfaces directly (the decoded planes sample fine). For CPU-written Onion surfaces read by the GPU (overlay), stage into Garlic to avoid coherency issues. |

**EOP label:** allocate in Onion (CPU-pollable), map with
`ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_ALL`.

---

## 11. Checklist for a New OpenGNM Shader Project

1. **Compile shaders** with `psslc.sh -profile <profile> -o <out>.sb <in>.pssl`.
2. **Load** the `.sb` into a `GnmVsShader`/`GnmPsShader` register struct,
   copying code into WC_GARLIC and setting the code address.
3. **Parse** the input-usage table to get each resource's `startregister`.
4. **Bind direct resources** (registers 0–15) passing `startregister` directly
   to `Set*UserData` — **do NOT divide by 4**.
5. **For EUD resources** (registers ≥ 16): bind the EUD block address at the
   `PTR_EXTENDEDUSERDATA` register, write descriptors into the EUD block at the
   compiler-derived offsets, and flush.
6. **Wire VS→PS varyings** with `sceGnmDrawCmdSetPsInputUsage`.
7. **Flush** all WC_GARLIC writes (`sceGnmFlushGarlic`) before submit.
8. **Submit** the command buffer, `sceGnmSubmitDone()`, wait for EOP.
9. **For overlays**, append the blend draw to the SAME command buffer as the
   video draw (single submission) to avoid flicker.

---

## 12. Key Findings Recap

1. **`Set*UserData` slot = register index (0–15), not a 4-DWORD slot.** Pass
   the shader's `startregister` directly. Dividing by 4 mis-binds resources and
   causes null-pointer GPU page faults.
2. **EUD resources live at DWORD offsets `startregister - 16`** in a
   GPU-visible block addressed by the `PTR_EXTENDEDUSERDATA` register. Always
   derive offsets from the parsed input-usage table and verify empirically.
3. **`-no-resource-in-user-sgprs`** forces resources into EUD for diagnostics.
4. **Single-submission video+overlay** eliminates overlay flicker (no
   cross-submission read-after-write hazard).
5. **WC_GARLIC writes must be flushed** (`sceGnmFlushGarlic`) before the GPU
   reads them (shader code, descriptors, EUD, staging).
6. **Texture pitch is in texels**, not bytes; **base addresses must be
   256-byte aligned**.
7. **CPU-written Onion surfaces** read by the GPU (overlays) should be staged
   into WC_GARLIC to avoid cache-coherency issues.

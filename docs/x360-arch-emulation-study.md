# Xbox 360 vs Snapdragon 8 Gen 2 — architecture comparison and emulation optimization plan

**Author:** GPU/CPU agent · **Status:** architecture study complete, plan reviewed,
**Rank 4 shipped** (instrumentation only — no rendering behaviour changed),
Ranks 1–3 **not** implemented, nothing measured on device · **Related:**
`docs/gw-gpu-bottleneck-investigation.md`, `docs/gpu-pass-count-levers.md`,
`docs/gpu-fs-occupancy-analysis.md`, `docs/benchmark-harness.md`.

**Device this ran on:** Pocket S, Android 13, board `kalama`, **Adreno 740**,
`GLES: Qualcomm, Adreno (TM) 740, OpenGL ES 3.2 V@0676.41`, Vulkan
`API 1.4.363 (1.3 used), driver version 0x6802063` — all read via adb / `xe.log`
on the attached device, 2026-10-05.

> Board `kalama` is Qualcomm's SM8550 (Snapdragon 8 Gen 2) platform codename, so
> the device attached here **is** the target SoC. That matters: the prior
> investigations in `docs/` were done on an Adreno 650 (Retroid Pocket 5) and an
> Adreno 830 (AYN Odin 3). Almost every "measured" claim in those documents is a
> claim about a *different* GPU generation and does not transfer here.

---

## 1. Evidence classes used here

Per `AGENTS.md` §1, every figure below is tagged:

| Tag | Meaning |
|---|---|
| `[DEV]` | Read from the attached device by me (adb / `xe.log`). |
| `[SRC]` | Quoted from a URL that was actually fetched and read. |
| `[CODE]` | Verified in this repository at the cited `file:line`, re-derived with `grep -n`/`sed -n` rather than copied out of `docs/`. |
| `[DOC-MEASURED]` | A measurement recorded in an existing `docs/` investigation. Cited as a *prior* result, not re-verified by me — see §3.1 for the one place this is load-bearing. |
| `[UNVERIFIED]` | **Not established.** Do not treat as fact. |

No figure below is estimated. Where a widely-repeated number could not be
sourced, it is marked `[UNVERIFIED]` rather than repeated.

---

## 2. The Xbox 360 (the thing being emulated)

### 2.1 Xenon CPU `[SRC]`

Sources actually fetched and read:
`https://en.wikipedia.org/wiki/Xbox_360_technical_specifications?action=raw`
and `https://en.wikipedia.org/wiki/Xenon_(processor)`.

| Item | Value |
|---|---|
| Cores | 3, each with 2 SMT threads → **6 hardware threads** |
| Clock | **3.2 GHz** (2.8 GHz on engineering samples) |
| Execution | **in-order** — a deliberate contrast with the original Xbox's Pentium III |
| Peak FPU | **115.2 GFLOPS**, via a dot-product instruction added to VMX128 |
| Front-side bus | **21.6 GB/s**, "aggregated 10.8 GB/s upstream and downstream" |
| L1 | 32 KB I / 32 KB D |
| L2 | **1 MB** total. The **per-core split is `[UNVERIFIED]`** — the commonly repeated "512 KB/core" appeared in nothing I fetched, so it is not asserted |
| L3 | **No L3 claimed, none listed.** "The Xenon has no L3" is `[UNVERIFIED]` — that is unsourced-by-absence, which is weak |
| Unified memory | **512 MB**, shared CPU/GPU |

**Correction to a common myth.** The claim that the Xenon has *no branch
predictor* is **`[UNVERIFIED]`** and must not be published. What *is* sourced is
that the Xenon is **in-order** — a fact about issue/commit ordering, routinely
conflated with the absence of a branch predictor. Do not let one imply the other.

### 2.2 Xenos GPU `[SRC]`

From the same specifications article: the GPU is ATI **Xenos** with **10 MB of
eDRAM**. Source sentence: "Graphics processing is handled by the ATI Xenos, which
has 10 MB of eDRAM."

Everything else about Xenos — shader-processor count, clock, TFLOPS, eDRAM
bandwidth, tile size, the unified shader register file, the ban on local-memory
arrays and dynamic indexing, binning and resolve behaviour — is **`[UNVERIFIED]`**
in this document. The Wikipedia Xenos article carries a long spec list but does
not address the register model, and **this fork contains no Xenos shader
translator to corroborate it from** (`grep` for `XenosShaderIr` / `shader_ir`
returns nothing under `emulator-core/src/main/cpp/xenia/`). Publishing those
numbers would be exactly the "plausible number presented as fact" failure mode.

**Consequence for this plan:** the comparison below leans on the CPU figures and
on the *structural* facts (in-order, unified memory, 10 MB eDRAM chosen to keep
main-memory traffic down), and leans on **no** unverified shader-ISA claim.

### 2.3 The structural fact that actually matters

The 360's memory system is the design driver: 10 MB of eDRAM holding the frame
buffer, fed by a bus delivering 10.8 GB/s each way, with an in-order CPU to keep
that latency tolerable. Every architectural quirk — one big eDRAM "render
target", modest main-memory bandwidth, in-order execution, an SMT-looking
3-core/6-thread split — follows from making a 3.2 GHz in-order triple-core
affordable at 90 nm.

---

## 3. Snapdragon 8 Gen 2 / Adreno 740 (the thing running the emulator)

`[DEV]`, from the attached device:

| Item | Value | How obtained |
|---|---|---|
| Board | **`kalama`** | `getprop ro.product.board` |
| Model / Android | Pocket S / 13 | `getprop` |
| GPU | **Adreno 740**, OpenGL ES 3.2, `V@0676.41` | `dumpsys SurfaceFlinger` |
| Vulkan | **API 1.4.363, driver `0x6802063`** | `xe.log` |
| Panel | 1440×2560 portrait; `mOverrideDisplayInfo` reports `2560 x 1440` in **rotation 1** | `wm size`, `dumpsys display` |
| **Swapchain the presenter created** | **2560×1440** | `xe.log`: `VulkanPresenter: Created 2560x1440 swapchain` |

CPU cluster configuration (X3 / A715 / A510) and cache sizes are `[UNVERIFIED]`
here. **No claim below rests on them.** An attempt was made to source them: the
Snapdragon 8 Gen 2 product page is client-rendered and returns only a JavaScript
shell to a plain fetch, `developer.qualcomm.com` likewise returns no spec content,
and both candidate product-brief PDF paths return HTTP 404. That attempt yielded
**no usable source**, so these figures stay unverified and nothing was filled in
from memory or from the 8 Gen 1's configuration.

### 3.1 The one architectural contrast that drives everything

**Adreno is a tiler; Xenos is an immediate-mode binner.** Adreno shades into
on-chip GMEM per tile, then resolves to DDR. Xenos writes straight into a 10 MB
eDRAM frame buffer. The emulator therefore pays a **render-pass begin/end pair**
every time the guest thinks it is still inside one pass, on a GPU whose whole
design rewards keeping a pass open.

That is the structural mismatch, and it is why
`docs/gpu-pass-count-levers.md` and `docs/gw-gpu-bottleneck-investigation.md` are
entirely about pass count, barriers and render area. Nothing here contradicts
them; this section explains *why* they are shaped the way they are, and adds the
one host-side cost they never had a panel large enough to expose (§4.1).

**Evidence for the Adreno half `[DOC-MEASURED]`.** I could not source Qualcomm's
own description of the binning model this session — the Snapdragon 8 Gen 2 product
page is client-rendered and returns only a JavaScript shell to a plain fetch, and
both candidate product-brief PDF paths 404. So rather than cite a datasheet I
never read, the tiler claim rests on **measurements already taken on Adreno
hardware in this repository**:

- `docs/a830-gmem-msaa-plan.md:22` — `kgsl_gmem_probe` reports
  `gmem_sizebytes=12582912` (12 MB) on an Adreno 830. On-chip GMEM of a known
  size is the tiler's defining resource.
- Same document, `:36-39` — bisection arms `sysmem` / `nobin` / `nostore` /
  `noload` each change behaviour, i.e. hardware binning and GMEM store/load are
  separable, real stages in the pipeline rather than a figure of speech.
- `docs/gw-gpu-bottleneck-investigation.md:151-152`, `:249-250`, `:316-318` —
  `renderArea` and bytes-per-pixel are described as driving "tile binning and
  GMEM load/store", which is the tiler cost model stated operationally.

This is stronger evidence than a marketing datasheet would be, and it is
adreno-generic across the 650/740/830 generations. **What remains `[UNVERIFIED]`
is Adreno 740's specific tile size, GMEM-per-tile figure and shader-core count** —
no claim in this plan depends on those.

---

## 4. Facts established on this device (new, and not in `docs/`)

### 4.1 A resolution mismatch that costs real fill rate `[DEV]` `[CODE]`

The presenter created a **2560×1440** swapchain. The guest renders
**1280×720** (`internal_display_resolution = 8`, `graphics_system.cc:27-49`). The
present path draws a full-screen quad into the swapchain
(`vulkan_presenter.cc:2019`, `vkCmdDraw(draw_command_buffer, 4, 1, 0, 0)`) with the
viewport set to the swapchain extent (`vulkan_presenter.cc:1886-1893`).

So on this panel the emulator shades **3.69 M pixels per presented frame** to
display **0.92 M pixels** of guest output — **4× the necessary fill**, every
frame, in a pass that is pure presentation scaling.

**That 4× is a floor, and understates it.** The present path can chain several
effect passes, not one. `GetGuestOutputPaintFlow` (`presenter.cc:671`) appends up
to `postprocess_ffx_fsr_max_upsampling_passes` FSR EASU passes before RCAS or
bilinear (`presenter.cc:886-902`), each writing a **full intermediate image**;
the bundled config ships that at **4**, confirmed in the device log's CONFIG DUMP.
So with the FSR effect selected a frame is not one upscale quad but a chain, and
the pixel total is correspondingly larger. Every effect in `GuestOutputPaintEffect`
is an upscale filter — the enum has no straight copy — so the present pass count
is a scaling cost in all cases.

This is invisible in all four existing documents because they were measured on
panels whose native resolution is close to 720p. On a 1440×2560 panel the
present pass is a structural cost, and it is the kind of cost that does *not*
appear in `VkPassTime` buckets — those only cover guest passes, so present
scaling is **inter-pass time by construction** and is exactly what
`log_gpu_pass_break_reasons`'s `VkPassSplit` line is designed to expose.

**Corroboration from an independent run.** The prior session already on the
device (`logs/session_20261004-170249.zip`, a different boot, 2026-10-04) contains
the same pair, and names both sides explicitly:

- `VulkanPresenter: Created 2560x1440 swapchain …` — **4** times in that boot, all
  `presentation mode 1` (`VK_PRESENT_MODE_IMMEDIATE_KHR`), so the presenter built
  the same oversized swapchain repeatedly. Re-derived from the log 2026-10-05
  (lines 397, 1379, 56008, 56985); an earlier draft of this file said "twice",
  which was wrong.
- `VdQueryVideoMode #0..#3: reporting 1280x720 (cvar mode 8)` — the guest's own
  video mode, and `8` is the `internal_display_resolution` value for 1280×720
  (`graphics_system.cc:27-49`). The guest is telling the host 720p.

So the mismatch reproduces across two separate boots and is a stable property of
panel + guest resolution, not a one-off.

Also `[DEV]` from that log, and useful context for anyone picking this up:
`maxImageDimension2D: 16384`, `maxFramebufferWidth: 16384`,
`framebufferColorSampleCounts: {1|2|4}`, `framebufferDepthSampleCounts: {1|2|4}`,
`VK_KHR_dynamic_rendering_local_read` advertised, and `vulkan_dynamic_rendering =
true` in the applied config. So the pass-splitting machinery of §4.2 is running
on a driver that does support the modern path.

### 4.2 Shared-memory barrier scope is larger than documented `[CODE]`

`VulkanSharedMemory::Use` (`vulkan_shared_memory.cc:546-578`) emits, whenever
`last_usage_ != usage`:

- `offset = 0; size = VK_WHOLE_SIZE;` (`:561-562`) on `buffer_`, and
- a mirrored whole-buffer barrier on `host_buffer_` (`:573-578`).

The buffer is **512 MB**, not 256 MB: `SharedMemory::kBufferSizeLog2 = 29`
(`gpu/shared_memory.h:23-24`) — re-derived from the header, not from a document.

The gate is only `last_usage_ != usage || last_written_range_.second` (`:546`);
it does **not** compare masks. A usage flip between consecutive draws therefore
produces a 512 MB whole-buffer barrier, and `SubmitBarriers` then unconditionally
ends the open render pass (`vulkan_command_processor.cc:3142`). Two `Use()` call
sites sit in the per-draw path (`vulkan_command_processor.cc:4734` and `:4742`).

Already instrumented: the `barriers=` term in `VkPassBreaks`
(`vulkan_command_processor.cc:2376-2388`), gated on `log_gpu_pass_break_reasons`.

### 4.3 Per-draw dynamic-state re-emit `[CODE]`

`BindExternalGraphicsPipeline` (`vulkan_command_processor.cc:3786-3800`) sets 15
extended-dynamic-state dirty flags every time an external (transfer/resolve)
pipeline binds. This is correct — those pipelines bake EDS statically — but it is
per-transfer work, and it is **not separately instrumented**.

---

## 5. What could NOT be measured, and why

This is the most important section in the document.

**No frame ever rendered on the attached device.** Two runs were attempted:

1. **This session.** Launched via `am start -n armx360.compose.debug/xendroid.compose.EmulatorHostActivity --es game_uri <SSX iso>`.
   The emulator booted subsystems, initialised Vulkan on the Adreno 740, created a
   2560×1440 swapchain, then **stalled permanently** at
   `Requesting Android window paint...` (`xendroid_emu.cpp:259`). `xe.log` reached
   531 lines and stopped. Zero `VkPassTime`, zero `DrawCallBegin`, zero
   `VkFrameSync`. Screenshot: black surface, `FPS 0`.
2. **A prior session already on the device** (`logs/session_20261004-170249.zip`,
   5.7 MB `xe.log`). It got *further* — it extracted `title_id 4541096D` — but
   ended with **1595 `MemoryPollPark` lines and zero draws**: the guest parked
   waiting for memory that never arrived, because the same surface/paint condition
   meant no frame was ever requested.

This is the **trap documented in `AGENTS.md` §6 (Device safety)**: an unattended
display holds `mCurrentFocus` in a window that never yields to the emulator, so no
surface is created, `bootOnce()` never completes, and nothing renders.
`cmd statusbar collapse` *did* move focus to the app this session
(`mCurrentFocus=Window{5ffdca0 u0 armx360.compose.debug/...EmulatorHostActivity}`),
but the boot still did not proceed past the paint request, and a synthetic
`KEYCODE_ENTER` did not unblock it. **A physical tap is required; that is a
physical-world input this environment cannot provide.**

**Therefore: not one performance number in this document is measured.** Per
`AGENTS.md` §8 this is reported as **blocked, with the reason**. No fps, no
counter reading, no pass time, and no "this change is X% faster" appears anywhere
below. The runbook that would produce those numbers is in
`docs/benchmark-harness.md` and `docs/gpu-pass-count-levers.md` §3; it needs a
run that actually renders.

Also void: the prior session's pipeline cache was **cold** — both shader files
reported `(0 bytes on disk)` and "storage is being reset" — so its timings would
have been void under trap 1 regardless.

### 5.1 Environment note (affects reproduction, not the analysis)

This machine has no JDK, no Android SDK/NDK and no cmake. Build and test
therefore ran through the repo's `Dockerfile` container (the same base image as
CI). `./gradlew :app:testDebugUnitTest` in-container gives **89 tests, 0
failures, 14 classes** — matching the figure recorded in `AGENTS.md`, re-measured
here rather than quoted. `tools/bench-ab-test.sh` gives **36 checks passed**.

---

## 6. Reviewed plan: candidate optimizations, ranked

### Ranking method — and why it is deliberately not "expected speedup"

Candidates are ranked by

> (confidence the cost is real) × (confidence the fix is safe) ÷ (size of change)

and **not** by expected speedup, because expected speedup is precisely the
quantity that could not be measured (§5). Ranking on an unmeasurable number is
how a wrong conclusion gets shipped. Every candidate below is `[CODE]`-verified to
exist and **unmeasured** as to payoff.

### Rank 1 — Stop shading 4× the pixels that get presented

**Observation.** §4.1: 2560×1440 swapchain, 1280×720 guest, full-screen quad.

**Why first.** It is the only candidate that is (a) *proven to exist* by a log
line rather than inferred, (b) independent of which title runs, and (c) absent
from every existing document because none of them were measured on a
high-resolution panel. On a tiler, shading 3.69 M pixels to present 0.92 M is the
kind of cost that shows up as thermals and lost clock rather than as a pass time.

**The change.** Two separable options, in increasing order of risk:

- **(a) Skip the intermediate effect passes when the transform is a pure scale.**
  The presenter already special-cases `effect_count` and an intermediate image
  (`vulkan_presenter.cc:1869-1872`); if the only effects are upscaling ones, the
  final pass could sample the guest output directly.
- **(b) Cap the swapchain to the guest extent and let the Android compositor
  scale.** Cheap for us, but hands filtering to SurfaceFlinger.

**Why cvar-gated and off by default.** This trades our shader for someone else's
scaler — a *visual* change, not a pure win. Per `AGENTS.md` §4 an unmeasured
render-path change ships behind a cvar whose help text states the tradeoff and the
measurement needed to justify turning it on, with `GAME_COMPAT.md` as the
per-title escape hatch. Whether it wins depends on the panel-to-guest resolution
ratio, which is a **per-device and per-title** question.

**Its instrumentation has shipped** (`log_present_path_cost`, a `VkPresentCost`
line in the Vulkan presenter):

```
VkPresentCost: 60 frames, 1.0 passes/fr, present=3.69Mpx/fr guest=0.92Mpx/fr \
               ratio=4.00x swapchain=2560x1440
```

*(format only — not measured)*

That reports pass count and the pixel ratio rather than a time, because **the
present path is not covered by any existing counter**: `VkPassTime` buckets guest
render passes by framebuffer extent, and the present pass is none of those. A
time-based version would need new GPU timestamps on the present submission, which
is a larger change than the ratio; the ratio is what decides whether Rank 1 is
worth attempting at all, and it needs no query pool.

**What the ratio does not tell you.** It counts pixels, not the per-pixel cost of
whichever shader each pass runs, so FSR/CAS passes look the same as bilinear
despite being dearer. And it says nothing about compositing. It is a floor, and
it is a locating tool, not a measurement of the cost.

### Rank 2 — Narrow the shared-memory barrier across usage flips

> **Scope note.** This is *narrowing* the barrier's byte range. It is **not** the
> "defer the barrier past the pass" change, which is refuted in §6 — the barrier
> must stay framebuffer-global, so its extent is adjustable but its *position* is
> not. Do not merge these two.

**Observation.** §4.2: 512 MB `VK_WHOLE_SIZE` barriers per usage flip, each
forcing a render-pass end.

**Why viable.** The barrier is *already* narrowed to a range when the usage does
not change (`:556-557`). The whole-buffer case exists only because a usage flip
makes the prior extent unknowable. Carrying a conservative dirty range across
flips would let most of them narrow.

**Why not first.** It touches memory-ordering correctness. Too narrow a barrier
is a race that manifests as intermittent, unreproducible corruption rather than a
crash — the worst possible failure mode to ship unmeasured. It must follow a
successful A/B on a rendering run.

Note the ceiling on this one: the pass end at `vulkan_command_processor.cc:3142`
still happens, because the barrier is framebuffer-global regardless of its size
(§6). Rank 2 can reduce the *cost* of a break, not its *number*, which is the term
`barriers=` in `VkPassBreaks` actually counts.

### Rank 3 — Skip the `host_buffer_` mirror barrier when unused

`PushBufferMemoryBarrier` already no-ops when stage/access/queue masks are equal
(`vulkan_command_processor.cc:2999-3003`), but `Use` passes `skip_if_equal =
false` deliberately (`:568`, `:577`) because committing a *written range* must not
be skipped. The safe win is narrower: skip **only the `host_buffer_` mirror**
when no memexport draw has touched it since the last barrier. Pure bookkeeping,
no ordering change.

### Rank 4 — Instrument before optimising: per-draw EDS re-emit

§4.3 is not separately instrumented. Measure it before changing it. This is a
**logging** change, not an optimisation — cheap, safe, and it converts a guess
into a number.

**Implemented.** A `VkOverhead` line now reports shared-memory barrier shape and
EDS re-dirty volume in the same once-per-second report as `VkPassBreaks`, gated on
`log_gpu_pass_break_reasons`:

```
VkOverhead: shmem=12.0/fr whole=3.0 ranged=9.0 ranged_MB=0.42 |
            eds_redirty=4.0/fr binds=2.0
```

*(the numbers above illustrate the format only — not measured)*

Three design points worth stating, because each is a trap rather than a detail:

- **The counters are pure attribution.** No barrier is added, removed, narrowed
  or reordered anywhere, and no GPU query is issued, so this cannot change what is
  rendered. That is what makes it shippable without a device run; Rank 2 is not.
- **It is gated on `log_gpu_pass_break_reasons`, not `log_gpu_frame_time_breakdown`.**
  These counters need no GPU timestamps, so putting them on the cheaper gate means
  the cost of reading them stays confined to the diagnostic path.
- **`eds_redirty` reads 0 when extended dynamic state is unsupported**, because the
  re-dirty it counts is inert then. The guard is
  `pipeline_cache_->dynamic_state_capabilities().extended_dynamic_state` — the
  same capability `UpdateDynamicState` gates its emission on — so "0" means
  "nothing happened", not "nothing to do". `UpdateDynamicState`'s `eds_caps` is a
  local and not reachable from `BindExternalGraphicsPipeline`, hence the query.

The counters make Rank 2 decidable instead of hypothetical. **A run where
`whole` dominates is the only run in which narrowing the usage-flip barrier has
anything to narrow** — and that is the precondition to check before attempting a
memory-ordering change whose failure mode is intermittent corruption rather than a
crash.

### Rank 5 — `appCategory="game"`: a deliberate omission, do not add it casually

`[CODE]` **The app does not declare `android:appCategory`** — the `<application>`
element (`app/src/main/AndroidManifest.xml:27-33`) carries `name`, `allowBackup`,
`icon`, `label`, `supportsRtl` and `theme`, and no `appCategory`;
`grep 'appCategory' app/src/main/AndroidManifest.xml` returns nothing.

`[SRC]` This matters because Game Mode is gated on exactly that declaration, and
the trade runs *against* an emulator:

| Dial | Direction |
|---|---|
| Declare `appCategory="game"` → Game Mode interventions apply (backbuffer-resize savings, FPS throttling) | **gains** |
| …but on **Android 15+** games default to **60 Hz** and must explicitly request more, so declaring as a game **opts into the 60 Hz default** | **loses** |

For an emulator whose purpose is presenting 360 content at the panel's native
rate, the second row is the one that matters, and this device is Android 13
where the high-refresh behaviour does not yet apply — so the calculus **changes
by OS version**. The honest reading is that this is a **decision that must be
made deliberately per supported OS range, not a missing line**. It is listed here
rather than in §6 because it is a genuine open question, not a dead end.

Note also that FPS throttling can only *lower* the rate; it cannot rescue a
GPU-bound emulator, and this fork already paces to the guest's own vblank rate
(`framerate_limit_auto`). So the gain side is thin.

**Not shipped, deliberately.** A manifest attribute that trades away the native
refresh rate on Android 15+ is not something to enable on principle, and it cannot
be A/B'd on this Android 13 device at all — the condition that makes it harmful
does not exist here. Per `AGENTS.md` §8 this is recorded as an open question
rather than guessed at. Verify the current Android 15+ behaviour before acting;
the claim above is sourced from the study in §2, not from a device run.

### Refuted — do not re-propose

- **Render-area shrinking.** Built, verified firing, and **measured to buy
  nothing** on Adreno 650 (`docs/gw-gpu-bottleneck-investigation.md` §12.7);
  `render_area_dirty_extent` is default-off with that recorded in its help text
  (`vulkan_command_processor.cc:49-58`).
- **Reusing one host render target across pitch changes.** Provably impossible:
  pitch *is* the byte layout (`docs/gpu-pass-count-levers.md` §2, Option 0).
- **Fewer distinct guest pitches.** The game owns `RB_SURFACE_INFO`. Not ours.
- **Instruction-count-driven shader shrinking.** On Adreno, a build with strictly
  fewer SPIR-V instructions has measured **slower**
  (`docs/gw-gpu-bottleneck-investigation.md` §10, trap 4). Shader *size* tracks
  reality; instruction mix does not.
- **Any claim resting on the unverified Xenos shader-ISA model** (§2.2).
- **Deferring the per-draw shared-memory barrier past the open render pass.**
  This is `docs/gpu-pass-count-levers.md` §2 Rank 4, and it was proposed here as
  `Rank 1` before being checked. **It is not viable, and the reason is worth
  recording because the surface argument for it is convincing and wrong.**

  The argument *for* it is: `SubmitBarriers` ends the open pass for **any**
  pending barrier (`vulkan_command_processor.cc:3142`), and the per-draw
  shared-memory barrier touches only a **buffer**, never an attachment — so on a
  tiler, where a pass end is a GMEM→DDR→GMEM round trip, that end looks pure
  waste. Buffer-only, therefore not framebuffer-global, therefore safe to record
  inside the rendering scope.

  **Each step is wrong.** Vulkan's framebuffer-space stages are FRAGMENT_SHADER,
  EARLY/LATE_FRAGMENT_TESTS and COLOR_ATTACHMENT_OUTPUT — the test is the *stage
  mask*, not what the barrier's descriptors happen to name. And the per-draw mask
  does contain FRAGMENT_SHADER: `VulkanSharedMemory::GetUsageMasks` builds
  `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_`
  (`vulkan_shared_memory.cc:986`), and `guest_shader_pipeline_stages_` is
  `VERTEX_SHADER | FRAGMENT_SHADER` (`vulkan_command_processor.cc:389-390`). So
  the one barrier that fires per draw **is** framebuffer-global, and recording it
  inside a rendering scope is exactly what the spec forbids.

  The residue that survives the correct test — barriers with no framebuffer-space
  stage at all — is **nearly empty**, because the per-draw case is excluded. So
  this would buy approximately nothing *and* risk intermittent corruption. Note the
  shape of the error: a change can be simultaneously *unprofitable* and
  *incorrect*, and filtering it down to the provably-safe subset silently removes
  the reason it was proposed. **Correctness of the subset was never the difficulty;
  the subset was always the wrong set.** Do not re-derive this from "it's only a
  buffer barrier" — check the stage mask.

  The adjacent case that *is* handled correctly, for contrast:
  `PreparePendingDrawPassTransferBarriers` uses
  `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (`vulkan_render_target_cache.cc:3904`)
  precisely because those transfer *sources* are sampled by in-pass fragment
  shaders, and emits them before the pass begins (`:3626`) — so they stay
  framebuffer-global and never need a mid-pass break. `vulkan_hoist_shmem_uploads`
  (`:36-41`) is the same idea applied narrowly to uploads, and is the pattern to
  extend if `barriers=` ever proves to be the dominant term. Rank 4's own warning
  ("not recommended blind") was correct; this entry records why.

---

## 7. What would unblock measurement

In priority order — the first item is the only real blocker:

1. **A device run that renders.** The unattended-display limitation in §5 is the
   single thing standing between this document and real numbers. A physical tap
   during boot, or a display that yields focus, unblocks everything else.
2. Then `log_gpu_frame_time_breakdown` **and** `log_gpu_pass_break_reasons` in a
   per-game config. They cost nothing and yield `VkPassSplit`'s in-pass vs
   inter-pass split — the measurement that decides whether Rank 1 or Rank 2 is
   the bigger fish. Per `AGENTS.md` §6 trap 10, **never read an fps number off
   such a run**: it issues `vkCmdCopyQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)`
   every submission and stalls the queue.
3. `store_shaders` warm, verified via `Shader storage: pipeline file … (N bytes on
   disk)` being non-zero, or the run is void under trap 1.
4. Two runs, keeping the second, with `tools/bench-ab.sh preflight` before any A/B.

---

## 8. Net result

- The architecture study is complete and sourced; every unverified figure is
  labelled rather than repeated, including two popular myths corrected (§2.1).
- **One new, device-proven finding** the existing ledgers do not contain: 4×
  present-path overdraw from a 2560×1440 panel against a 720p guest, reproduced
  across two separate boots (§4.1).
- **The load-bearing architectural claim is sourced, not asserted.** Qualcomm's
  own description of Adreno's binning model was unreachable this session, so the
  tiler contrast rests on measurements already taken on Adreno silicon in this
  repository (§3.1) — stronger evidence than a marketing datasheet.
- **Five ranked candidates**, each traced to verified code, **none shipped**, none
  carrying a performance claim.
- **One candidate from this session's own review was refuted by checking it** — the
  barrier-deferral that was briefly ranked first (§6). It is recorded with its
  reasoning so the same convincing-but-wrong argument is not re-derived from
  "it's only a buffer barrier".
- **Measurement is blocked**, recorded as blocked rather than papered over.

### 8.1 Corrections made to this document after review

Recorded per `AGENTS.md` §1 rather than silently fixed:

- §4.1 said the oversized swapchain was created "twice" in the boot on device.
  Re-derived from `session_20261004-170249.zip`'s `xe.log`: it is **4** times
  (lines 397, 1379, 56008, 56985), all at presentation mode 1. The number was
  wrong; the conclusion it supported was not.
- The barrier-deferral candidate was **ranked first when this document was
  drafted**, and that ranking was wrong. It is moved to §6 with the reason.

Per `AGENTS.md` §4 ("prefer recording a negative result over shipping an
unmeasured optimisation"), the correct deliverable here was the study and the
plan, not an optimisation nobody could measure.
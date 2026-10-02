# Geometry Wars — GPU bottleneck investigation (Retroid Pocket 5)

**Date:** 2026-08-08
**Device:** Retroid Pocket 5 (`61705c55`), Adreno 650 (a6xx), Turnip `mainline-turnip-V31`
**Title:** Geometry Wars: Retro Evolved 2 — `584108FF`
**Branch:** `sync/upstream-edge`

---

## 1. How we got here

This started as a **framerate regression hunt**, not a GPU study. GW had dropped from ~30fps
to 13fps on the RP5. A first-parent bisect (walking only our commits and upstream-sync merges,
never inside an upstream merge) from the branch base `c4d9c6172` found **two independent losses**:

| step | commit | loss | status |
|---|---|---|---|
| shader cost | canary sync `465a6e121` + our adaptation `8361ab3bb` | 25 → 13 | **FIXED** |
| host C++ | upstream sync `28222f6c2` (`152a243ef..56df0a955`) | 30 → 25 | see §7 |

### 1.1 The fixed half

Upstream commit `6119556c6` added two accuracy features that compile into **every full resolve
shader**: destination `copy_dest_number` packing, and PWL gamma decode for `8_8_8_8_GAMMA`
sources. Both were written as scalar helpers with branch trees, inlined per component.
`resolve_full_32bpp` went from 14,288 → 35,388 SPIR-V words.

The cost is **code size / register pressure / occupancy**, not executed branches — which is why
two runtime cvars (`gamma_decode_pwl_resolve=false`, `resolve_copy_dest_number_packing=false`),
both *verified honoured in the log*, changed nothing. The code stays in the shader either way.

Fixed by `accurate_resolve_number_formats` (default **off**, exposed in the Android GPU settings
page), which selects a separately compiled lean shader set via `XE_RESOLVE_FAST_FORMATS`.
Result: 14,360 words — 72 words off the pre-regression baseline.

Supporting commits: `d090d490e` (branchless packers), `76a3b0049` (the gate), `87001c90c`
(a log line stating which variant set was built), plus a branchless PWL gamma curve
verified **bit-identical** to the original over 100k samples and all 256 byte values, both directions.

### 1.2 The state that prompted this study

After the fix, GW sits at **~22fps warm** with **~5fps still missing**, and the device runs hot.
That is where this document begins.

---

## 2. Thermals — two different stories

| component | temp | why |
|---|---|---|
| CPU cluster | 55–57 °C | **not doing work** |
| GPU | 51–52 °C | genuinely saturated |

Total CPU usage is ~19% of 800% (≈6.4 of 8 cores idle), yet the frequency **floors** are pinned
near maximum by a system power profile:

| core | `scaling_min_freq` | max | hardware min |
|---|---|---|---|
| cpu7 (prime) | 2841 | 2841 | **844** |
| cpu4 (gold) | 2150 | 2419 | — |
| cpu0 (little) | 1420 | 1804 | — |

`cpu7` has `min == max` — it can never clock down. Verified **not ours**: nothing in the app
writes cpufreq (`cpuinfo.cpp:70` only *reads* `cpuinfo_max_freq`), and
`adrenotools_force_max_clocks` is `false`. This is the Retroid performance profile.

**Implication:** because we are GPU-bound with the CPU 80% idle, dropping the device performance
profile should cut a large share of the heat at little or no framerate cost. Untested.

---

## 3. Profiling setup

Built the instrumented Turnip from `/home/renato/mesa-turnip` with `build-turnip-a6xx.sh`
(a6xx/kgsl — correct for the Adreno 650; `build-turnip-a8xx.sh` is for gen8/`mesa-tu8`).
It carries a whole-GPU KGSL counter sampler (`tu_perf_sampler.cc`) that reserves ~100 hardware
counters and polls them from a background thread — no command-stream changes, no GPU perturbation.

Two traps had to be cleared before any number was trustworthy:

1. **The counter table used a7xx countable numbers.** 90 of 103 are identical on a6xx, but
   **7 are renumbered** and **6 do not exist**. The renumbered ones are the dangerous kind — the
   selector is valid but measures something else, reporting confident numbers under a false name.
   The whole `PC_*` block shifts by one (`PC_3D_DRAWCALLS` 28→30, `PC_IA_VERTICES` 20→21,
   `TP_STARVE_CYCLES_SP` 54→55). Fixed with a generation-aware `apply_gen_fixups()` that patches
   selectors when `chip == 6` and logs every correction.
2. **`IOCTL_KGSL_PERFCOUNTER_READ` returns `EPERM`** unless
   `/sys/class/kgsl/kgsl-3d0/perfcounter` is `1`. `PERFCOUNTER_GET` succeeds regardless, so it
   looks like reservation worked and only reads fail. The file is root-owned but **group-writable
   by `shell`**, so `adb shell "echo 1 > /sys/class/kgsl/kgsl-3d0/perfcounter"` works without root.
   Likely resets on reboot.

Enabled from the app via new cvars in `ui/vulkan/vulkan_instance.cc`, `setenv`-ed before the driver
loads (Turnip reads them at instance/device create), mirroring how `TU_DEBUG` was already handled:
`turnip_perf_sampler`, `turnip_perf_sampler_period_ms`, `turnip_perf_sampler_file`.

> **Caveat:** an instrumented-driver run is for *locating* the bottleneck. Any fps claim must be
> re-measured on `mainline-turnip-V31`. Seven Turnip builds are installed on this device — pin one
> before comparing anything.

---

## 4. Where the GPU time goes

41 samples, GPU **93.3% busy** @ 567MHz. Unit busy is summed over instances, so >100% is expected.

| unit | busy | reading |
|---|---|---|
| **SP** (shader cores) | **489** | **the bottleneck** |
| TP (texture) | 347 | but *starved by SP* (202) |
| RB | 150 | downstream |
| CCU | 105 | fine |
| CP | 93 | front end not limiting |

Three independent counters agree:

- **92% of SP busy is ALU working cycles** (447.7 of 489.3) — not texture, not memory.
- **HLSQ is blocked by the fragment shader 94% of the time.**
- **TP is starved by SP**, so texture is not the limiter despite a 63.3% L1 miss rate.
- UCHE reads only 3.29 GB/s. GBIF/AXI counters were refused (`EBUSY`), so no true DRAM figure —
  but starved TP + low UCHE traffic make bandwidth an unlikely constraint.

Rates: FS 29.8 G instr/s (37.9 G alu/s), `rb_pix` 807M/s, `tp_pix` 1817M/s, 6721 draws/s.

**Verdict: fragment-shader ALU bound.**

---

## 5. Root cause A — the render area covers the whole EDRAM range

### 5.1 The mechanism

`RenderTargetCache::GetRenderTargetHeight()` sizes a host render target from the **entire EDRAM
range divided by pitch**:

```
tile_rows = ceil(kEdramTileCount / pitch_tiles)     // 2048 tiles
height    = tile_rows * kEdramTileHeightSamples     // 16 samples
```

So a narrow target balloons: pitch 2 tiles → 1024 rows → 16,384 → clamped to 8192.

Then `vulkan_command_processor.cc` sets the render area to the whole thing, unconditionally —
with upstream's own TODO sitting right there:

```cpp
// TODO(Triang3l): Actual dirty width / height in the deferred command buffer.
render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
```

On a desktop GPU this is harmless. **On a tiler it is the worst possible choice**: `renderArea`
drives tile binning and GMEM load/store, so an 80x8192 surface bins ~0.65M pixels of tiles to
service **one draw**.

### 5.2 Measured per-pass breakdown

`log_gpu_frame_time_breakdown=true`, buckets keyed by host WxH. `scissor<=` is instrumentation
added during this investigation (max scissor corner across the pass's draws).

| pass | passes/fr | draws/fr | ms/fr | scissor (drawn) | waste |
|---|---|---|---|---|---|
| **1920x1376** | 5.2 | 14 | **9.04** | 1920x8192 (unbounded) | ~1.27× |
| 960x2736 | 2 | 2 | 1.80 | 960x**540** | **5×** |
| 960x688 | 2 | 2 | 1.41 | 960x8192 (unbounded) | — |
| 240x8192 | 8 | 8 | 1.36 | 240x**135** | **60×** |
| 160x8192 | 10 | 10 | 1.13 | unbounded | — |
| 80x8192 | **35** | 35 | 0.74 | unbounded | — |
| 480x5472 | 2 | 2 | 0.50 | 480x**270** | **20×** |

~16ms/frame of pass time. **55 passes/frame are 8192-tall strips with one draw each.**

Binned area vs drawn area: ~77M px/frame of declared render area against ~14M actually drawn.

> **CORRECTED (measured, see §12.7):** this ratio is *not* a cost model. Shrinking the render
> area to the drawn region changed neither pass times nor the RB/CCU/UBWC counters — Turnip
> derives its binning from actual draw coverage, so those tiles were never being processed.
> The figure describes declared area, not work.

The viewport is **not** a usable bound: it reads `8192x8192` on every pass, because xenia sets a
maximal viewport and applies the guest's viewport transform in the vertex shader.

### 5.3 Bucket arithmetic (all heights are artifacts)

`GetWidth = pitch_tiles × (80 >> (msaa≥4x))`, `height = ceil(2048/pitch) × 16 >> (msaa≥2x)`:

| pitch | width | height | bucket |
|---|---|---|---|
| 24 | 1920 | ceil(2048/24)=86 → **1376** | 1920x1376 |
| 12 | 960 | 171 → **2736** | 960x2736 |
| 6 | 480 | 342 → **5472** | 480x5472 |
| 3 / 2 / 1 | 240/160/80 | clamped → **8192** | ✓ |
| 24 @4xMSAA | 960 | 1376/2 → **688** | 960x688 |

**None of these heights are the game's.** 1376, 2736, 5472 and the 8192 clamp are pure artifacts
of sizing host render targets to span the whole EDRAM range. Only the widths are guest-derived.

---

## 6. Pass identification

Added `VkPassId` logging (`GetLastUpdateRenderTargetsDebugName()`), printed once per bucket:

```
VkPassId: 1920x1376 <- color0 RT @ 0t, <24t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  960x2736 <- color0 RT @ 0t, <12t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  480x5472 <- color0 RT @ 0t,  <6t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  240x8192 <- color0 RT @ 0t,  <3t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  160x8192 <- color0 RT @ 0t,  <2t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:   960x688 <- depth  RT @ 0t, <24t>, 4xMSAA, kD24S8
VkPassId:   80x8192 <- depth  RT @ 0t,  <2t>, 4xMSAA, kD24S8
```

Findings:

- **The 9ms pass is the game's HDR scene buffer** — colour, `k_2_10_10_10_FLOAT`, pitch 24 → 1920
  px wide. With the bloom scissors being exact halvings (960x540 → 480x270 → 240x135), the base is
  **1920x1080**. The game renders at 1080p even though `VdQueryVideoMode` reports 1280x720 to it.
- **Every render target is at EDRAM base 0** with a different pitch (24, 12, 6, 3, 2). The game
  reconfigures the EDRAM layout constantly; each distinct pitch forces a separate host render
  target and render pass. That is why there are 7 pass shapes and 55+ passes per frame.
- Colour passes are 1xMSAA; depth passes are 4xMSAA over the same EDRAM base.

---

## 7. Root cause B — the HDR target is stored at double width

**Original hypothesis (REFUTED):** that `k_2_10_10_10_FLOAT` (Xenos **7e3**) was being emulated
with software pack/unpack arithmetic in the fragment shader, inflating the ~47 FS ALU ops/pixel.

**Why it is wrong:** `PreClampedFloat32To7e3` / `UnclampedFloat32To7e3` are called only from
`FSI_ClampAndPackColor` in `spirv_shader_translator_rb.cc` — the **Fragment Shader Interlock (ROV)**
render-backend path. This device runs `render_target_path = "performance"`
(`kHostRenderTargets`), confirmed independently by the `VkPassId` output showing real per-format
render target keys. **No 7e3 packing is emitted into GW's fragment shaders.** The ~47 ALU
ops/pixel are the game's own bloom/glow shaders, not emulation scaffolding.

**What is real instead.** On the host render target path, 7e3 is stored in a wider host format:

```cpp
case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
  return VK_FORMAT_R16G16B16A16_SFLOAT;   // vulkan_render_target_cache.cc:4125
```

The guest buffer is **32 bpp**; we store **64 bpp**. There is no exact Vulkan equivalent —
`B10G11R11_UFLOAT_PACK32` has no alpha, `A2B10G10R10_UNORM_PACK32` is unorm clamped to [0,1] where
7e3 covers [0,32), and `E5B9G9R9_UFLOAT_PACK32` is shared-exponent — so the choice is defensible.

But on a **tiler** it is not free: doubling bytes-per-pixel halves how much of the framebuffer fits
in GMEM, which **increases the tile count** and therefore binning and GMEM load/store work. With
colour+depth at 12 B/px instead of 8, roughly **1.5× the tiles**. This lands on the *same* cost
centre as Root Cause A (§5) rather than on ALU.

**Status (explored, gated, off by default).** The follow-up is now implemented behind
`vulkan_hdr_render_target_as_b10g11r11` (default **false**, `Vulkan` category, per-game overridable),
which selects `B10G11R11_UFLOAT_PACK32` in `VulkanRenderTargetCache::GetColorVulkanFormat`
(`vulkan_render_target_cache.cc:4196`). It is additionally gated on the driver exposing that format
with `SAMPLED_IMAGE | COLOR_ATTACHMENT | COLOR_ATTACHMENT_BLEND | COLOR_ATTACHMENT_MULTISAMPLE`, and
the decision is logged at startup so a run that asked for it and did not get it is visible.

**Destination alpha is *not* provably unused — it is read unconditionally.** Every path that moves a
7e3 host target's texels reads all four components (the guest format reports 4, `xenos.h:334-345`),
and each one derives the guest's 2-bit alpha from the sampled fourth component:

| consumer | evidence (`vulkan_render_target_cache.cc`, post-`b10g11r11` line numbers) |
|---|---|
| ownership transfer (source) | component mask `(1 << count) - 1` = `0b1111` at `:4925-4926`, sampled + extracted at `:5766-5790` |
| ownership transfer (dest, 7e3→7e3) | all 4 components passed straight through at `:6288-6300` |
| ownership transfer (dest, 7e3→32bpp) | `NClamp(a,0,1)*3+0.5` into bits 30-31 at `:6315-6326` |
| EDRAM dump | `NClamp(source.a,0,1)` at `:9028-9032` |
| direct-host resolve (compute) | `XeResolveHostColorPackUnorm(color.a, 3.0f) << 30u`, `resolve_host_color_pack.xesli:61` |
| resolve clear | `clearValue.color.float32[3]` at `:8525-8526` |
| guest blending | alpha blend factors at `vulkan_pipeline_cache.cc:1897-1904` |

So "destination alpha is unused" cannot be established statically for any target — it would need
per-title evidence from a trace. That is the same shape as the Fable II ground-geometry bug
(`GAME_COMPAT.md:139`, draws issued, nothing written): **a format substitution that silently drops a
channel fails quietly, not loudly.**

Two corrections to the reasoning in this section, both material to the decision:

- **"no negative range" is not a regression.** 7e3 is unsigned — `Float7e3To32`
  (`xenos.cc:104-120`) splits the 10 bits into a 7-bit mantissa and a 3-bit exponent with no sign
  bit, and `UnclampedFloat32To7e3` clamps to `[0, 31.875]`. Both formats clamp negatives to zero
  already.
- **Range is wider, not narrower.** B10G11R11_UFLOAT's 5-bit exponent reaches ~65000, comfortably
  past 7e3's `[0, 32)`. Bright HDR above 1.0 is *not* what breaks.

What actually breaks is alpha (above) and mantissa precision: **6 bits on R/G and 5 on B against
7e3's 7 on all three** — 2× coarser on red and green, 4× on blue.

**The present path is not a consumer.** `IssueSwap` reads the front buffer through
`RequestSwapTexture` (`vulkan_texture_cache.cc:1083`), which samples a *guest texture* out of guest
memory, and the FXAA luma image is a fixed `R16G16B16A16_SFLOAT`
(`vulkan_command_processor.h:1027`) independent of the render target format. So this lever cannot
help or harm presentation directly — it only changes what the guest reads back.

**Unmeasured.** The 1.5× tile-count argument in this section is arithmetic about GMEM residency, not
a measurement on any device. The A/B runbook is §13.

### Shader variant data (`tu_variant` log, 40 FS variants)

| | instrs | occupancy |
|---|---|---|
| most (36) | 50–425 | 16 waves (full) |
| heavy (4) | 971–2228 | **4–8 waves** |

Worst two: **2195 instrs @ 4 waves** (31 GPRs), **2228 @ 6 waves** (20 GPRs). NOPs are **26%
overall** and **40–57%** in many shaders — scheduling delay slots that only full occupancy hides.

---

## 8. How the outstanding TODO relates

The upstream `TODO(Triang3l): Actual dirty width / height in the deferred command buffer` is not a
cosmetic cleanup — **it is the direct cause of the ~5.5× over-binning in §5**. Upstream deferred it
because on immediate-mode GPUs an oversized render area costs nothing. On a tiler it costs
everything: binning work and GMEM load/store scale with `renderArea`, not with what you draw.

The TODO also states the reason it was deferred: the correct extent isn't known when
`vkCmdBeginRenderPass` is recorded, because commands go into a **deferred** command buffer and the
draws that determine the dirty region haven't been seen yet. So implementing it requires
**backpatching** the render area once the pass closes (or a pre-pass over the deferred stream).
That is the real work item, and it is why this cannot be a one-line change.

Relationship to the rest:

- §5 (render area) and §7 (64bpp HDR target) are **both ours**, and they **compound**:
  more bytes per pixel means more tiles, and every tile pays the oversized render area.
- §4 says we are fragment-ALU bound. Note **neither** §5 nor §7 attacks ALU directly —
  the ALU load is the game's own shaders. See §9 lever 3 for the only ALU-side lever we hold.
- §5 attacks tile binning and GMEM traffic, which is separate from ALU. Both can pay.
- The game's own overdraw and 306 draws/frame are **not** ours to fix.

---

## 9. Levers, ranked

1. **Bound `renderArea` to the drawn region.** Biggest structural win (~5.5× less binning).
   For the three bloom passes the scissor already gives the correct bound for free (5×/20×/60×).
   For the unbounded passes it needs the render target's real height. Requires backpatching the
   deferred command buffer — see §8.
2. **The 64bpp HDR target** (§7). Not an ALU cost — it multiplies tiles, so it compounds lever 1.
   A 32bpp host format is only possible where destination alpha is provably unused.
3. **Reduce GPRs on the two worst shaders** to lift occupancy from 4/6 toward 16, so the 40–57%
   NOP slots get filled.
4. **Drop the device performance profile** (§2) — thermal win at likely no fps cost, since we are
   GPU-bound with the CPU 80% idle.
5. **The remaining ~5fps from upstream sync `28222f6c2`** is still unattributed. Its only
   Android-relevant changes are a `scaled_resolve` path (dead here — `draw_resolution_scale=1`,
   zero "Scaled resolve" log lines) and a single `NClamp` on the stacked-texture layer index.
   Neither plausibly costs 5fps, so **the bisect step itself is suspect** — see §10.

---

## 10. Methodology traps (all hit during this work)

1. **Pipeline cache warm-up.** `store_shaders=true` persists a per-title cache at
   `cache/shaders/local/<TITLEID>.vk.bin`. The **first run after installing a new build**
   recompiles pipelines during gameplay and reads several fps low. This is the `libllvm-qgl.so`
   CPU seen in early profiles. **Always run twice; take the second run.** A correct shader fix
   looked 4–5fps short of baseline purely from this.
2. **Never trust an fps A/B until the log shows the cvar applied** — grep `xe.log` for the key plus
   `Applied N game config override(s)`. Two "results" were configs that had never changed.
3. **Per-game config files can vanish.** GW's config was deleted mid-session, silently reverting
   `readback_resolve` from `none` to the global `uma` — which looked like a broken build.
   Re-verify the file before and after every run.
4. **SPIR-V opcode histograms are not a cost model on Adreno.** A build with strictly *fewer*
   instructions measured *slower*. Total shader **size** tracked reality; instruction mix did not.
5. **Generated bytecode headers are untracked and survive `git checkout`.** Wipe
   `shaders/bytecode/` before each bisect build and require `0 skipped/failed`, or the build
   silently links bytecode from another commit (`465a6e121` is untestable for exactly this reason:
   92 shaders fail to compile there).
6. **Counter selectors are generation-specific** (§3). A renumbered selector reports plausible
   numbers under the wrong name — worse than no data.
7. **Device readings carry ~±2fps.** Do not over-read a 13-vs-15 difference; a neutral commit was
   once reverted on that basis.
8. **The driver is a free variable.** Seven Turnip builds are installed, and at least one run used
   a per-game override of `Turnip_v26.3.0-R3` while the global was `mainline-turnip-V31`.

> All eight traps are now encoded as machine-checked preconditions in
> `tools/bench-ab.sh` — see `docs/benchmark-harness.md`. Before starting a new
> A/B, run `tools/bench-ab.sh preflight`, and after each run
> `tools/bench-ab.sh assert-log --log xe.log --expect game.<cvar>=<value>
> --driver-path <build>`. Two traps that used to rely on memory are now
> self-reporting: **trap 1**, because `Shader storage: pipeline file … (0 bytes on
> disk)` identifies a cold pipeline cache directly, and **trap 3**, because the
> absence of `Loading game config:` is the machine-readable signature of "no
> per-game config was applied" (`config.cc:329-331` returns silently when the file
> is missing). Note the per-game override block logs the **bare cvar name**, not
> `category.name` (`config.cc:350`), and `assert-log` refuses an unqualified
> `--expect` rather than guessing which config source to check.

---

### 12.7 OUTCOME — measured, shelved

Phase 1 was built and measured. **It works and buys nothing.**

Verified firing (`VkShrink` log): `240x8192 -> 240x160` (51× less area), `160x8192 -> 128x96`,
`80x8192 -> 32x32` (512× less area). Passes with 2+ draws stayed full because the EDRAM
ownership-transfer draw sets `transfer_scissor.extent = host_extent` deliberately (the transfer is
bounded by its *geometry*, not its scissor) — real, but moot given the result below.

Single-build A/B via the cvar, same driver, counter sampler on both runs, normalised per shaded
pixel to cancel a ~5% activity difference:

| metric / rb_pix | shrink ON | OFF | delta | run noise (1 sd) |
|---|---|---|---|---|
| RB busy | 0.18485 | 0.18668 | −1.0% | 13.3% |
| CCU busy | 0.11247 | 0.11414 | −1.5% | 16.2% |
| UBWC read | 0.00133 | 0.00125 | **+6.9%** | 39.9% |
| UCHE read | 0.00416 | 0.00419 | −0.6% | 28.1% |
| SP alu | 0.55797 | 0.56006 | −0.4% | 22.0% |

Every delta is far inside noise, and UBWC moved the *wrong* way. Pass times were identical to two
decimals (`1.36 / 0.50 / 1.80 / 9.04 ms`) across builds.

**Why the wall-clock null result was not enough:** it cannot distinguish "no work removed" from
"work removed off the critical path". Only the unit counters can — RB/CCU/UBWC are not gated by the
shading critical path, so if real tile or GMEM work had been removed they would have dropped
regardless of the SP. They did not.

**Conclusion:** Turnip already skips empty tiles; the declared render area is not what it bins.
Lever 1 is dead rather than deferred, and phase 2 (viewport intersection) inherits the same dead
premise — not worth building. `render_area_dirty_extent` now defaults **off**; the code is correct
(only ever shrinks, falls back to full extent on a zero-draw pass, handle cleared on `Reset()`) and
kept as groundwork for drivers that do honour the render area.

---

## 11. Reproducing

```bash
# one-time: allow perfcounter reads (resets on reboot, no root needed)
adb shell "echo 1 > /sys/class/kgsl/kgsl-3d0/perfcounter"

# build + install the instrumented a6xx driver
cd ~/mesa-turnip && ./build-turnip-a6xx.sh
# -> install as compose/driver/a6xx-instrumented-perf/{libvulkan_freedreno.so,meta.json}
```

Per-game config (`config/584108FF.config.toml`):

```toml
[GPU]
log_gpu_frame_time_breakdown = true    # VkPassTime + VkPassId lines

[Vulkan]
vulkan_lib_path = '/data/user/0/xendroid.compose.debug/compose/driver/a6xx-instrumented-perf/libvulkan_freedreno.so'
turnip_perf_sampler = '1'              # or 'tp' for the deep texture set
turnip_perf_sampler_period_ms = 250
turnip_perf_sampler_file = '/storage/emulated/0/Android/data/xendroid.compose.debug/files/compose/tu_perf.log'
```

Reports: counter samples go to `turnip_perf_sampler_file`; the `tu_variant:` shader lines ignore
that setting and always land in `/data/data/<pkg>/files/tu_perf.log` (read with `run-as`).
`VkPassTime` / `VkPassId` go to `xe.log`.
---

## 12. Proposed solution for lever 1 — render-area backpatching

### 12.1 Why the TODO's own framing is the right one

The upstream comment is `Actual dirty width / height in the deferred command buffer` — and the
deferred command buffer is exactly what makes the fix cheap. Verified in code:

- `DeferredCommandBuffer::CmdVkBeginRenderPass` stores `ArgsVkBeginRenderPass` **by value** in the
  command stream, including `VkRect2D render_area`, and only reads it back at replay
  (`deferred_command_buffer.cc:70-92`). The same holds for `ArgsVkBeginRendering`.
  So the render area can be **backpatched after the pass has been recorded** — an offset-based
  write into the stream, no Vulkan re-recording. (Offset, not pointer: the stream grows.)
- Every draw that can land in a pass is scissor-bounded **in the recorded stream**: guest draws via
  `IssueDraw`, and the render-target-cache paths (ownership transfers at
  `vulkan_render_target_cache.cc:8090`, dumps/clears at `:2273`) also call
  `command_processor_.SetScissor(...)` before their raw `CmdVkDraw`. Nothing draws unscissored.

Vulkan semantics make the shrink safe: load/store ops apply only inside `renderArea`, content
outside stays untouched (correct for xenia's persistent LOAD/STORE attachments), and the app's
obligation is that no rendering escapes the render area — guaranteed if `renderArea` ⊇ the union
of all draw scissors in the pass.

### 12.2 Phase 1 — patch to the recorded scissor union (provably safe)

Track the union inside `DeferredCommandBuffer` itself, where every path is visible:

1. `CmdVkSetScissor` remembers the current scissor (persists across pass begins — dynamic state
   carries over).
2. Every `CmdVkDraw*` unions the current scissor corner into a per-pass max; `CmdVkClearAttachments`
   unions its clear rects.
3. `CmdVkBeginRenderPass` / `CmdVkBeginRendering` reset the union and return the stream offset of
   the recorded args as a patch handle.
4. At `EndRenderPass`, the command processor patches the recorded `render_area.extent` to the
   union corner — clamped to `host_extent`, rounded up to `vkGetRenderAreaGranularity` (legacy
   path) or 32px (dynamic rendering; alignment is a perf nicety, not a requirement). A pass with
   zero recorded draws keeps the full extent.

Gate behind a cvar (`render_area_dirty_extent`, default on once validated) with the per-game
config as the escape hatch. Extent-only for v1; `offset` stays (0,0) — the win is cutting the
8192-row tail, and 360 content is origin-anchored.

**What phase 1 recovers (from §5.2):** the bloom trio — 60×/20×/5× over-binned, 3.66 ms/fr
combined — collapses toward per-draw cost. The measured floor for a near-empty strip pass is
0.021 ms (80x8192), so expect roughly **−2.5 to −3 ms/fr**. The wide-open-scissor passes
(1920x1376 main, 960x688, 80/160x8192 strips) do not move in phase 1.

### 12.3 Phase 2 — bound the host scissor by the guest viewport

The main pass's scissor is wide open (1920x8192) because the game relies on viewport clipping,
and xenia runs a maximal host viewport (8192x8192, §5.2) with the guest viewport transform applied
in the vertex shader. But the guest viewport rectangle **is known host-side** (`draw_util`
computes it for the shader constants). So: program the **host scissor = guest scissor ∩ guest
viewport rect** at draw setup. Phase 1's mechanism then picks the bound up automatically, and
rasterization is hard-bounded by hardware — nothing can escape the patched render area.

One behavioural difference vs real hardware, worth a cvar of its own: point sprites (and wide
lines) whose centre is inside the viewport can rasterize a half-point-size sliver beyond the
viewport edge; a 360 with a wide-open scissor would keep that sliver, we would clip it. Options:
pad the intersection by the max point size for point-primitive draws, or accept the sliver loss
under the cvar. GW is particle-heavy, so validate visually there first.

**What phase 2 recovers:** main pass 1376 → ~1080 rows (**−~1.9 ms** of 9.04), 960x688 → ~540
(−~0.3 ms), and the 45 one-draw strip passes bound to their real viewports (up to −~2 ms if their
viewports are small, which their 0.021-0.17 ms/pass area-scaling implies).

### 12.4 Expected total, stated honestly

Bucketed in-pass time: ~16 ms/fr → **~10-11 ms/fr** for phases 1+2 combined. **But** §4's numbers
imply the GPU is busy well beyond the bucketed pass time (93.3% busy at ~45 ms/frame vs ~16 ms of
bucketed passes — the delta is inter-pass work: barriers, CCU/cache flushes, submissions, texture
uploads, all scaling with the **55+ passes/frame**, which render-area patching does not reduce).
So the fps gain cannot be predicted from pass time alone. First verification step is therefore to
quantify in-pass vs total GPU time; if inter-pass dominates, the follow-up lever is pass-count
reduction (the existing pass-fusion work), not further area trimming.

### 12.5 Verification plan

1. `VkPassTime` before/after on the instrumented driver: bloom trio toward ~0.5 ms, main bucket
   ×0.785. Confirm no bucket *grew*.
2. Counter sampler: RB/CCU busy and UBWC read/write should fall; SP roughly unchanged (the ALU is
   the game's own shading, §7).
3. Correctness: bloom chain visuals in GW (the shrunk passes are the bloom pyramid — corruption
   shows there first), then the in-pass-resolve titles (TDU2/NG2 — resolve draws are
   scissor-bounded so they are covered, but they are the riskiest consumers), then BD/Forza.
4. fps: on `mainline-turnip-V31`, warm (second run), per §10 — never on the instrumented driver.
5. GMEM mode sanity: `TU_DEBUG` must not contain `sysmem` in the run being measured (it forces
   untiled rendering, where renderArea is nearly free and the A/B would read as noise).

### 12.6 Risks

- A rendering path that bypasses both `IssueDraw` and the RT-cache scissor discipline would escape
  the patched area → undefined pixels. Mitigated by tracking in `DeferredCommandBuffer` (sees every
  recorded draw) and by keeping full extent for zero-draw passes.
- Point-sprite slivers under phase 2 (above).
- A pass split across submissions: patch handles are per-stream offsets; a Begin recorded in an
  already-replayed stream must not be patched. By construction EndRenderPass runs on the same
  deferred stream as its Begin, but assert it.
- 4x MSAA depth passes (960x688, 80x8192): extents are in host pixels; keep the union in the same
  space as `dynamic_scissor_` (host pixels), no per-sample math.


---

## 13. A/B runbook — `vulkan_hdr_render_target_as_b10g11r11`

**Do not flip this from the ImGui debug dialog.** `GetColorVulkanFormat` feeds both image creation
*and* graphics-pipeline `VkPipelineRenderingCreateInfo::pColorAttachmentFormats`
(`vulkan_pipeline_cache.cc:2702`), and pipelines are cached. Toggling it after a title is running can
pair a new-format attachment with a cached old-format pipeline. Set it before launch, via the
per-game config (preferred) or the global TOML, exactly like `vulkan_depth_unorm24`. It is
deliberately absent from the Android settings schema for the same reason `vulkan_direct_host_resolve`
is.

### 13.1 Per-game setup

```toml
# <storage_root>/config/<TITLEID>.config.toml
[Vulkan]
vulkan_hdr_render_target_as_b10g11r11 = true
```

Section header **must** be `[Vulkan]` — keys are looked up as `category.name`; a bare key is
silently ignored (`config.cc`, `ReadGameConfig`). `TITLEID` is the 8-hex uppercase ID from
`Title ID:` in `xe.log`.

### 13.2 Confirm it took effect — before judging the picture

```sh
adb shell "grep -E 'B10G11R11_UFLOAT|Loading game config' \
  /sdcard/Android/data/xendroid.compose/files/compose/xe.log | head"
```

Both must hold:

1. `B10G11R11_UFLOAT supported` — the driver advertises it. **`unavailable` means the cvar was
   ignored**; any picture difference is then noise, not the format.
2. `Loading game config: <path>` appears — otherwise the per-game block never loaded and the
   global default (off) stands.

Unlike a shader recompile, the host image format is chosen when each render target is created, so
there is no shader-cache warm-up to wait out. Still: **run twice, take the second** (§10.1), and keep
`TU_DEBUG` free of `sysmem` (§13.4).

### 13.3 What to look at, in order

1. **Correctness first, fps second.** Anything below is a reason to abandon it for that title.
   - Alpha: fades, decals, additive glow, anything translucent over the HDR target rendering
     **fully opaque**. This is the expected failure and the one to check first.
   - Banding in smooth HDR gradients and bloom halos; blue-heavy gradients worst (5 mantissa bits).
   - Resolution clears, depth-based fades and anything reading the target's alpha after a resolve.
2. **fps** on the pinned stock driver (`mainline-turnip-V31`), warm second run. If it wins, confirm
   it is not thermal (§2) and re-confirm the second run.
3. **Cross-check the mechanism.** If fps does *not* move, that is consistent with §7's arithmetic
   being wrong rather than with the format being harmless — check the unit counters as in §12.7
   before concluding either way.

### 13.4 Traps

- `TU_DEBUG=sysmem` forces untiled rendering, where bytes-per-pixel barely matters — the A/B would
  read as noise (§5, §12.7).
- Do not combine with `vulkan_in_pass_resolve` on the first attempt; both change what the resolve
  path does, and a difference then has two candidate causes.
- A title with no 7e3 render target is unaffected. Check first — `VkPassId` lines name the formats:

  ```sh
  adb shell "grep -oE 'color[0-9] RT @ .*' \
    /sdcard/Android/data/xendroid.compose/files/compose/xe.log | sort -u"
  ```

  If `k_2_10_10_10_FLOAT` does not appear, there is nothing to measure and the run proves nothing.

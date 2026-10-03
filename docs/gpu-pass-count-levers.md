# GPU inter-pass overhead and pass count — analysis, levers, runbook

**Author:** GPU/rendering agent · **Status:** analysis complete, measurement
shipped, **no performance change made** · **Related:**
`docs/gw-gpu-bottleneck-investigation.md` (sections 5, 6, 12.4, 12.7),
`docs/benchmark-harness.md`, `GAME_COMPAT.md`.

This is the companion to task_0005. It answers three questions:

1. Why does the render-pass count explode on Geometry Wars?
2. What could reduce it, what is impossible, and what is the risk of each?
3. How does a human measure the in-pass vs inter-pass split that
   `gw-gpu-bottleneck-investigation.md` section 12.4 says gates the next lever?

**No performance claim is made anywhere in this document.** The sandbox that
produced it has no Android SDK/NDK, no cmake, no Java/Gradle, no adb, no attached
device and no GPU: nothing here was compiled or benchmarked. Everything below is
either a file:line citation you can check, or a measurement recipe.

---

## 1. Why pass count explodes

### 1.1 The chain, link by link

**(a) Pitch is a guest-per-draw register.** `RB_SURFACE_INFO.surface_pitch` is
read on every `RenderTargetCache::Update()`:

- `render_target_cache.cc:663` `uint32_t pitch_pixels = rb_surface_info.surface_pitch;`
- `render_target_cache.cc:676` `pitch_tiles_at_32bpp = ((pitch_pixels << msaa_x_log2) + 79) / 80;`
- `render_target_cache.cc:902` `rt_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;`

**(b) Pitch is a field of the render-target key.** `RenderTargetKey`
(`render_target_cache.h:268`) carries `pitch_tiles_at_32bpp : 8` at
`render_target_cache.h:274`, and `operator==` (`render_target_cache.h:294`)
compares the whole 32-bit word. `GetOrCreateRenderTarget`
(`render_target_cache.cc:1541`) looks the key up in `render_targets_`, so a new
pitch is a **new `RenderTarget` object**.

**(c) A new render target is a new image of a different size.**
`VulkanRenderTargetCache::CreateRenderTarget` sizes the image from the key:

```cpp
// vulkan_render_target_cache.cc:4245
image_create_info.extent.width  = key.GetWidth() * GetKeyScaleX(key);
image_create_info.extent.height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp,
                                                         key.msaa_samples) * ...;
```

with `GetWidth = pitch_tiles * (80 >> (msaa >= 4x))`
(`render_target_cache.h:329`) and
`GetRenderTargetHeight = ceil(2048 / pitch) * 16` clamped
(`render_target_cache.cc:1092-1118`). That reproduces section 5.3's bucket table
exactly: pitch 24 -> 1920x1376, 12 -> 960x2736, 6 -> 480x5472, <=3 -> clamped to
8192.

**(d) A different attachment set is a different framebuffer.** `FramebufferKey`
(`vulkan_render_target_cache.h:597`) carries `pitch_tiles_at_32bpp` **and** all
five per-slot `base_tiles`. `GetHostRenderTargetsFramebuffer`
(`vulkan_render_target_cache.cc:4621`) derives `host_extent` from the pitch
(`:4694-4698`). `Update()` additionally throws the cached framebuffer away
explicitly when the pitch moved (`vulkan_render_target_cache.cc:3501`).

**(e) A different framebuffer pointer is a render-pass break.** Both overloads of
`SubmitBarriersAndEnterRenderTargetCacheRenderPass` — the guest one at
`vulkan_command_processor.cc:3229` and the ownership-transfer one at `:3334` —
keep the open pass only on pointer identity:

```cpp
// vulkan_command_processor.cc:3242 (dynamic rendering) / :3248 (legacy)
if (in_render_pass_ && current_framebuffer_ == framebuffer && ...) return;
...
EndRenderPass(PassEndReason::kGuestFramebufferChange);   // :3253 / :3359
```

`IssueDraw` calls this once per draw (`vulkan_command_processor.cc:4752`). So
**pass count == number of times the framebuffer pointer changed**, plus whatever
barrier flushes force a break.

> This confirms and sharpens section 6. The investigation says "each distinct
> pitch forces a separate host render target and render pass". The precise
> mechanism is that pitch is in the *identity* of both the image and the
> framebuffer, and the pass is keyed on the framebuffer's **address**, not on its
> contents.

### 1.2 A pitch change also costs a full re-layout copy

This is the part section 6 does not spell out, and it changes what is worth
building.

`OwnershipRange::IsOwnedBy` compares the whole key
(`render_target_cache.h:703-717`, `if (render_target != key) return false;`), so
when the destination key carries a new pitch, the range is *not* already owned.
`ChangeOwnership` (`render_target_cache.cc:1678`) therefore appends a copying
`Transfer` (`render_target_cache.cc:1751`,
`if (!transfer_source.IsEmpty() && transfer_source != dest)`), and
`PerformTransfersAndResolveClears` encodes it as a pixel-shader blit.

That blit is a genuine re-layout, not a rescale: the transfer shader re-derives
the source texel from the **source's own** pitch out of the push constant
(`vulkan_render_target_cache.cc:5628-5657`,
`source_tile_index_y = source_tile_index / source_pitch_tiles`).

Two mitigating facts, both worth knowing before estimating cost:

- The claim length is already bounded by the draw's estimated extent, not by the
  whole 8192-row host image: `render_target_cache.cc:829`, `height_used =
  std::min(GetRenderTargetHeight(...), draw_extent_estimator_.EstimateMaxY(...))`,
  feeding `length_used_tiles_at_32bpp` at `:892`.
- The claim walk is already memoised for the no-op case
  (`rt_cache_ownership_claim_memo`, `render_target_cache.cc:958-967`) — but that
  memo requires `memo.dest == rt_key`, so a *changed* pitch misses it by
  construction.

### 1.3 Three consequences, ranked by how much they matter

| # | Consequence | Where |
|---|---|---|
| 1 | A pitch change costs a **pixel copy**, and that copy is fragment-shader ALU on a device section 4 says is fragment-ALU bound. | `render_target_cache.cc:1751` |
| 2 | A pitch change costs a **pass break** (end + begin, two barriers, a GMEM resolve/store pair on a tiler). | `vulkan_command_processor.cc:3253` |
| 3 | When the transfer cannot be merged in-pass it costs **three** breaks: the ownership-transfer overload builds a single-attachment framebuffer per destination (`vulkan_render_target_cache.cc:7846-7879`, entered at `:8027`), whose scissor is deliberately the *full* host extent (`:8094`, this is what section 12.7 observed), and then the guest pass is re-entered. | same |

**This is separate from, and additive to, the render-area question.** The render
area is set to the whole framebuffer (`vulkan_command_processor.cc:3301` /
`:3322`), but section 12.7 *measured* that shrinking it buys nothing on Turnip.
Nothing in this document re-proposes that.

---

## 2. Options, ranked

### Option 0 — reuse one host render target across pitch changes: PROVABLY IMPOSSIBLE

The host image is a **pitch-shaped linear pixel image**: guest pixel *(x,y)* of a
pitch-*p* target lives at linear offset *y*(p*80) + x*, which is exactly
*EDRAM-tile-major* order — tile *t = (y/16)*p + x/80*, byte offset *t*1280 +
*(y%16)*80 + (x%80)*. The guest's tile-to-pixel mapping and the byte layout are
the same statement, so changing *p* genuinely relocates every pixel. Two different
pitches are two different byte layouts of the same EDRAM, and the transfer
shader described above exists precisely to convert between them.

An EDRAM-shaped host layout would remove the copy entirely, but it needs a
per-fragment swizzle from *(x,y)* to tile-major — that is the
`render_target_path = "accuracy"` / pixel-shader-interlock path
(`render_target_cache.cc:649`, `interlock_barrier_only`), the slow path section 7
already established this device does not run. **Do not attempt this on the
performance path.**

### Rank 1 — measure first: is inter-pass even the problem? (shipped, this commit)

`log_gpu_pass_break_reasons` prints `VkPassSplit` and `VkPassBreaks`. See
section 3. Cost: one cvar, no rendering-path change. If `fb_change` dominates
`VkPassBreaks` *and* `inter_pass` dominates `VkPassSplit`, the guest's EDRAM
layout churn is the whole story and Ranks 2-4 are all chasing the guest. If
`barriers` or `forced_outside_pass` dominate, they are ours and cheap.

### Rank 2 — widen in-pass ownership transfers

`vulkan_in_pass_transfers` (default **true**, `vulkan_render_target_cache.cc:75`)
already folds compatible EDRAM ownership transfers into the guest pass instead of
breaking for them. `CanQueueDrawPassTransfers`
(`vulkan_render_target_cache.cc:3610`) rejects with explicit, checkable reasons:

- the destination needs an integer/aliased view or a separate transfer view
  (`:3636-3643`) — this rejects all the `k_16_16*` / `k_32*_FLOAT` formats;
- the transfer source is one of the pass's own attachments (`:3648-3664`);
- a host-depth source is present and the destination is not depth (`:3671-3674`);
- and later `PreflightPendingDrawPassTransfers` can still reject on missing
  pipelines, which falls back at `:3469-3475`.

Widening any of these is a *per-case correctness argument*, not a flag.

**Correctness risk: medium.** Every widening trades a pass break for a read and a
write of a resource inside one rendering scope; the existing rejections are
exactly the cases where that is not provably safe (source is an attachment,
format aliasing, host-depth encoding). **Recommendation: only after Rank 1 says
`xfer_pass` is non-trivial.** The existing cvar already gives a clean A/B to
bound it — flip `vulkan_in_pass_transfers=false` and diff
`VkPassBreaks: xfer_pass=`.

### Rank 3 — merge the per-destination ownership-transfer passes into one

`PerformTransfersAndResolveClears` loops over destinations
(`vulkan_render_target_cache.cc:7799`) and for each one whose transfers cannot be
merged in-pass builds its own single-attachment render pass and framebuffer
(`:7846-7879`) and enters it (`:8027`). Two destinations needing a transfer is
therefore **two extra passes**, plus the guest pass being closed and reopened
around them. Issuing all fallbacks inside one multi-attachment pass would remove
`n-1` of those.

The transfer shader already supports a non-zero `dest_color_rt_index`
(`vulkan_render_target_cache.cc:7898-7899`, used when merging into the guest
pass), so the colour half of this exists. The depth half does not: a
depth+colour transfer pass needs both attachments in one pass with a shader that
writes depth and colour, which is a new pipeline matrix.

**Correctness risk: medium-high.** Per-destination barrier placement is already
carefully ordered (`vulkan_render_target_cache.cc:7680-7711`, late barriers at
`:7813-7833`) specifically so cross-copying between destinations works; merging
passes changes the execution order those barriers were written for. I did not
attempt it. **Recommendation: only if Rank 1 shows `xfer_pass` is large, and
colour-only destinations first.**

### Rank 4 — stop ending the pass for barriers that do not touch it

`SubmitBarriers` ends the pass unconditionally whenever the barrier list is
non-empty (`vulkan_command_processor.cc:3135-3139`), because
`vkCmdPipelineBarrier` may not be recorded inside a rendering scope. Many of the
barriers pushed per draw are for images that are **not** attachments of the open
pass — e.g. the transfer *source* layouts in
`PreparePendingDrawPassTransferBarriers`
(`vulkan_render_target_cache.cc:3859-3871`), the shared-memory buffer, the
texture-cache readback buffer.

Deferring those to the next point the pass was going to break anyway would remove
those breaks.

**Correctness risk: high, and I am not recommending it blind.** The deferred
barrier's correctness depends on nothing inside the pass touching the resource —
which requires knowing every *sampled* image in the pass, not just the
attachments, and the deferred command buffer does not track that today. Note
`vulkan_hoist_shmem_uploads` (`vulkan_shared_memory.cc:36`, default true) is
already a narrow instance of exactly this idea, done safely, for shared-memory
uploads only. **Recommendation: Rank 1 must show `barriers=` is the dominant term
first; if it is, extend the hoist pattern rather than inventing a new deferral
mechanism.**

### Rank 5 — reuse render targets across pitch changes

Impossible, see Option 0. Listed only so it is not re-proposed.

### Not a lever

- **Render-area shrinking** — built, verified working, measured to buy nothing
  (section 12.7); `render_area_dirty_extent` is intentionally default-false with
  the explanation in its help text (`vulkan_command_processor.cc:49-58`).
- **Fewer distinct pitches from the guest** — not ours; the game owns
  `RB_SURFACE_INFO`.

---

## 3. Runbook: measure the in-pass / inter-pass split

Everything below uses cvars that already exist plus the one added in this commit.

### 3.1 Per-game config

`config/584108FF.config.toml` (Geometry Wars Retro Evolved 2):

```toml
[GPU]
log_gpu_frame_time_breakdown = true      # VkFrameSync + VkPassTime + VkPassId
log_gpu_pass_break_reasons  = true       # VkPassSplit + VkPassBreaks (new)

[Vulkan]
vulkan_lib_path = '/data/user/0/xendroid.compose.debug/compose/driver/a6xx-instrumented-perf/libvulkan_freedreno.so'
turnip_perf_sampler = '1'
turnip_perf_sampler_period_ms = 250
turnip_perf_sampler_file = '/storage/emulated/0/Android/data/xendroid.compose.debug/files/compose/tu_perf.log'
```

If you only want the pass-count picture and not the counter sampler, drop the
`[Vulkan]` block — `VkPassSplit` and `VkPassBreaks` do not need Turnip at all.
That is the cheapest possible first run: **production driver, nothing instrumented
beyond the GPU timestamps xenia already takes.**

Confirm both applied before believing anything (`GAME_COMPAT.md`):

```sh
tools/bench-ab.sh snapshot-config --title-id 584108FF --out before.txt
# launch, play ~60 s
tools/bench-ab.sh verify-config --title-id 584108FF --before "$(awk '{print $3}' before.txt)"
tools/bench-ab.sh assert-log --log xe.log --expect gpu.log_gpu_pass_break_reasons=true
tools/bench-ab.sh assert-log --log xe.log --expect gpu.log_gpu_frame_time_breakdown=true
```

### 3.2 Read the log

```
VkFrameSync: ... gpu exec avg=41.2ms ... gap avg=0.3ms ... rp_begins=57
VkPassTime: 1920x1376 : 9.04ms/fr (5.2pass 14draw/fr, 1.739ms ea) scissor<=1920x8192 ...
VkPassTime:  80x8192 : 0.74ms/fr (35.0pass 35draw/fr, 0.021ms ea) ...
VkPassTime: {} pass pairs dropped (raise pool)          <-- must be absent
VkPassSplit: gpu=41.20ms/fr in_pass=16.30ms/fr (40%) resolve=1.10ms/fr \
             gap=0.30ms/fr inter_pass=24.60ms/fr (60% of non-gap) \
             [submissions=1.0 draws=306 passes_started=57]
VkPassBreaks: 56.0 ends/fr | fb_change=48.0 barriers=2.0 xfer_pass=3.0 \
              forced_outside_pass=1.0 query=0.5 submission=1.0 \
              primitive_setup=0.0 unattributed=0.0
```

(Those numbers illustrate the *format* only — they are not measured.)

**Sanity checks before drawing any conclusion:**

1. `unattributed=` must be 0. Non-zero means an `EndRenderPass` call site was
   added without classifying itself; the attribution has a hole.
2. `pass pairs dropped` must be absent. At most 96 pass pairs are timestamped per
   submission (`vulkan_command_processor.h:733`) and a pass spanning a submission
   split is abandoned uncounted (`vulkan_command_processor.cc:3211-3214`). A drop
   makes `in_pass` an under-count and `inter_pass` correspondingly over-stated.
3. The `VkPassTime` buckets must sum to about `in_pass`. `VkPassSplit` computes
   `in_pass` from those same buckets, so if they visibly disagree you are reading
   two different report intervals.
4. `ends/fr` should be about `passes_started`. Off by one is normal (a pass open
   at report time); off by much is a hole.

**Decision rule:**

| Reading | Meaning | Next step |
|---|---|---|
| `inter_pass` small (<20% of gpu) | Passes are cheap; section 4's ALU-bound verdict stands and inter-pass is a dead end | Stop. Do not build Ranks 2-4. |
| `inter_pass` large **and** `fb_change` dominates `VkPassBreaks` | The guest's pitch churn is the cost, and section 1.2 says the copy is semantically required | Only Rank 3 is left; expect a small win. |
| `inter_pass` large **and** `barriers` + `forced_outside_pass` dominate | The breaks are **ours**, not the guest's | Rank 4, and `xfer_pass=0` means in-pass transfers are already doing their job. |
| `inter_pass` large **and** `xfer_pass` dominates | Transfers that could not be merged | A/B `vulkan_in_pass_transfers=false` to bound it, then Rank 2. |

### 3.3 Cross-check against the counter sampler

On the instrumented driver (investigation section 3 caveat: locating only, never
fps claims), compare the sampler's SP busy % against `VkPassSplit`:

- if SP busy is high, the inter-pass time is still shader work — most likely the
  ownership-transfer blits from section 1.2, which are real fragment invocations
  and are **not** visible in the `VkPassTime` buckets when
  `vulkan_in_pass_transfers` folds them into the guest pass;
- if SP busy is low while `inter_pass` is large, it is barriers/CCU/flushes, which
  points at Rank 4.

The one thing the sampler cannot settle: `in_pass`/`inter_pass` are measured with
`log_gpu_frame_time_breakdown` on, and that path issues
`vkCmdCopyQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)` every submission
(`vulkan_command_processor.cc:6596-6600`), which stalls the queue. So
**`VkPassSplit` is a ratio to be read, not a frame time to be believed**, and no
fps number may come from a run with it enabled.

---

## 4. What was and was not verified

**Verified by reading** (every claim above carries a file:line): the pitch ->
key -> image -> framebuffer -> pass-break chain; the re-layout copy on a pitch
change; the existing cvars and log formats; that all nine `EndRenderPass` call
sites are classified.

**NOT verified — needs a device:**

- that the change compiles (no Android SDK/NDK/cmake in the authoring sandbox);
- that `VkPassSplit` and `VkPassBreaks` print sane values;
- any fps, GPU-busy or thermal number whatsoever.

### 4.1 Partial device run — got the plumbing, not the picture

An unattended run on a Pocket S (Android 13, Adreno 740 / `kalama`) got **as
far as proving the instrumentation is wired up and honest**, and then hit an
environment wall. Worth recording, because every step below is a trap this
runbook does not currently mention.

**Which package to use.** The instrumentation is only in the **debug** build.
Verified by pulling both installed APKs and grepping `lib/arm64-v8a/libe.so`:

| string | release | debug |
|---|---|---|
| `VkPassBreaks` | 0 | 2 |
| `VkPassSplit` | 0 | 3 |
| `log_gpu_pass_break_reasons` | 0 | 1 |

So run this against `xendroid.compose.debug`, not `xendroid.compose`. This is
not fixable from a config file: `config.cc:338-345` only resolves keys against
pre-registered `cvar::ConfigVars`, so a cvar the binary never registered is
silently dropped — the same silent-ignore mechanism `GAME_COMPAT.md` warns about.
Setting it in a per-game config against a release build produces *no warning at
all*, which is a fourth trap in the same family as traps 2 and 3.

**The run that did work, end to end.** Grant All Files Access to the debug
package, write `config/4541096D.config.toml`, then `am start` the exported
`EmulatorHostActivity` with `--es game_uri <iso>` — no tap needed, the activity
is `exported="true"` and boots from `surfaceCreated`. The log confirms the
per-game config applied (trap 2/3 cleared, which is what makes the rest of the
runbook trustworthy):

```
Extracted title_id 4541096D from: /storage/emulated/0/Roms/xbox360/SSX (USA) ....iso
Loading game config: .../compose/config/4541096D.config.toml
  log_gpu_frame_time_breakdown = true
  log_gpu_pass_break_reasons = true
Applied 2 game config override(s)
```

`tools/bench-ab.sh assert-log` agrees, naming both cvars as applied via the
per-game config.

**Then the harness refused the run, correctly.** `cache.bytes=0`,
`FAIL pipeline cache was COLD (0 bytes on disk) -- this run must be DISCARDED`,
`assert.failures=1`. That is trap 1 firing on a genuine cold cache — the
harness did its job, and it is worth having seen refuse rather than report.

**Why no picture.** The title never rendered a frame: zero `VkPassTime`,
`VkPassId`, `DrawCallBegin` or `VkFrameSync` lines, and the log stopped growing
at 1407 lines with the guest parked in `MemoryPollPark`. CPU and Vulkan init had
completed (the Adreno driver initialises, config loads, shader storage is
probed), but the SurfaceView was never on screen: the activity stayed in
`mLastPausedActivity` with `isOnScreen=false`, and a persistent
`NotificationShade` window (`Window{490c8af}`) held `mCurrentFocus` for the
whole session — surviving `cmd statusbar collapse` and a SystemUI restart, and
never yielding to the emulator. With no surface, no input, and no input past the
splash, there is nothing to render and nothing to measure. A physical tap to
clear the shade is the missing step; this is an unattended-display limit, not an
emulator or config fault.

**Net: still no `VkPassSplit`/`VkPassBreaks` numbers, and the section 3.2
decision rule remains untested.** What is now established rather than assumed:
the cvar exists in a shippable build, the per-game config path applies it, the
harness asserts it applied, and the harness refuses a cold-cache run. The
remaining unknown is unchanged — it needs a screen someone can see.

**Deliberately not done:** no edit to the render path, the render-target cache,
the deferred command buffer, or any pipeline/format decision. The only
behavioural code touched is the `EndRenderPass` signature gaining a defaulted
parameter, which cannot change what is recorded.

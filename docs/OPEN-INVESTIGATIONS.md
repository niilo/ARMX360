# Open investigation items — gameplay observations from release testing

Recorded 2026-10-09 so they are not lost. **Neither item is a confirmed defect.**
Both are user-reported symptoms with a leading hypothesis and a proposed test; in
both cases the evidence available so far does **not** establish the cause.

Source: Gears of War 2 (`4D53082D`) on the CI-built **release** build
`ARMX360-39402d1`, Pocket S / Android 13 / Adreno 740, Turnip
`v26.3.0-20261008-r5`. Diagnostic config on (`log_present_path_cost`,
`log_gpu_pass_break_reasons`, `log_input_poll_breakdown`), verified by
`Applied 3 game config override(s)` on every boot cited here.

---

## 1. Graphical artefacts in textures and vertices

**Reported:** small visual artefacts in textures and vertices during play.

**Leading hypothesis: silently dropped draws.** Two cvars are on:

```
vulkan_async_skip_draws = true          xe.log:324
async_shader_skip_draws = true          xe.log:114
```

The consuming code states the behaviour directly
(`vulkan_pipeline_cache.cc:977-983`):

> *"Deferred pipeline binds that still resolve to `VK_NULL_HANDLE` when the
> command buffer is replayed **drop their draws** for that submission (during the
> storage preload at startup, or always with `vulkan_async_skip_draws` — pop-in
> instead of a wait)."*

So a draw whose pipeline is not yet compiled is **discarded rather than waited
on**. Missing geometry and missing texture passes are what that looks like, and
the shape matches a *small* artefact rather than breakage — it is a deliberate
latency-over-correctness trade.

**Why this is not a confirmed cause.** The drop is **silent**: there is no log
line, no counter, and no error. In 14,507 log lines there were **zero**
`Failed to create graphics pipeline`, zero `VK_ERROR`, and zero shader
translation failures. The inference rests entirely on the cvar being on plus the
symptom shape. Nothing in the log records a dropped draw.

Note `HidPoll`'s `dropped=0` is unrelated — that is HID bucket overflow, not
draws.

**Secondary observation, likely unrelated:** 50 × `PM4: predicated skip of sync
packet` (opcode 46 and 54), all confined to **frames 568–580**, a 12-frame window
during boot. Not spread through play.

**Test.** Set `vulkan_async_skip_draws = false` in the per-game config, which
makes that path block until the pipeline exists instead of dropping the draw.

**Cost of the test, stated honestly:** blocking is precisely what stalls boot.
This may turn out to be the *cause* of the ~39 s boot wait rather than a fix for
the artefacts — dropped draws keep frames moving, waiting does not. Run it as a
separate session so the two results are attributable.

---

## 2. Frame rate dips to the low 20s

**Reported:** fps dropping to ~21–24 in several places during a session.

**Measured.** 1,228 `VkPresentCost` reports over a 32,997-frame session. Four
distinct dips, recovering to ~28–30 between them:

| dip zone | windowed mean |
|---|---|
| f:10,000–12,000 | 22.80 |
| f:18,400–19,300 | 23.88 |
| f:26,000–29,000 | 22.24 |

Distribution tail: 374 reports at 30 fps, with substantial mass down at 21–24.

**Shader and pipeline compilation is EXCLUDED as the cause.** This was the
obvious hypothesis and it does not hold:

| zone | mean fps | translations | pipelines |
|---|---|---|---|
| dip f:27,300–28,100 | 23.20 | 8 | 7 |
| clean f:21,400–22,200 | **29.07** | 2 | 2 |
| clean f:4,700–5,500 | **29.11** | 0 | 0 |

The cleanest windows have **zero** compiles and run at ~29; the dip zones have
only 2–8. Over all reports, windows containing a compile averaged **25.34** vs
**26.93** without — a 1.6 fps gap, inside the ~2 fps device noise floor
(`AGENTS.md` §6). And the 52-translation cluster at f:18,000–19,999 landed on
frames that were already recovering.

**Leading remaining explanation: scene-driven GPU load.** The dips are consistent
with the game presenting heavier scenes rather than the host stalling. That is the
boring answer, and **these counters cannot distinguish it from anything else.**

**What would actually settle it:** `VkPassSplit`, the in-pass vs inter-pass split
that says whether the GPU is busy inside passes or stalled between them. It has
never printed and cannot print under the currently recommended cvar set — both it
and `VkOverhead` are nested inside the `log_gpu_frame_time_breakdown` gate at
`vulkan_command_processor.cc:2271`. See §5f of
`docs/HANDOVER-2026-10-09.md`. Enabling that cvar is a **separate run** and no fps
number may be read off it (trap 10).

**Not established:** whether the dips are GPU-bound, thermally induced, or a
property of the game. No thermal or throttling events appeared in logcat for the
session, so thermal is disfavoured but not excluded.

---

## 3. Two corrections from the same session, recorded so they are not repeated

**a) "The startup pipeline preload is cached, it takes 2 ms."** Asserted, then
retracted, then partly reinstated — the accurate statement is unresolved. The
preload summary reads `Pipeline cache: 1231 created, 0 already exist, 1231 total
in 3 ms`, but `EnsurePipelineCreated` (`:2096-2834`) reaches its single
`vkCreateGraphicsPipelines` call at `:2758` **inline**, with no queue or worker
hop, so 1,231 calls in 3 ms is either ~2 µs each (cache hits) or a clock
artifact. **Vulkan exposes no cache-hit flag**, so timing is the only proxy.
`cvars::shader_profiling` (`:2765-2779`) prints per-pipeline ms for exactly this
and has been staged in the per-game config, but **has never run** — the sessions
that followed its staging all predate it.

**b) Count-matching that does hold**, and is worth keeping:

| run | `Loaded N pipeline descriptions` | `Pipeline created` at f:0 |
|---|---|---|
| session A | 845 | 845 |
| session B | 1,006 | 1,006 |
| session C | 1,231 | 1,231 |

The boot burst is exactly the count of stored pipeline descriptions — it is an
unconditional startup preload of the whole shader store, **not** a cache miss.
`Pipeline created` is emitted unconditionally on `VK_SUCCESS`
(`vulkan_pipeline_cache.cc:2826`) and cannot distinguish hit from miss, which is
what made (a) so easy to get wrong.

The store is **not converging**: 845 → 1,006 → 1,231 stored descriptions across
three sessions, with 307 post-boot shader translations in the last run. Gears
streams new pipeline combinations as it plays.

---

## 4. Verified alongside these

Not open questions, recorded so they are not re-derived:

- **`ratio=4.00x` on the release build.** 570 consecutive reports, byte-identical
  `present=3.69Mpx/fr guest=0.92Mpx/fr`, guest 1280×720 into a 2560×1440
  swapchain. Confirms the study's §4.1 overdraw on a real release binary, across
  two titles. Still a **floor** (pixel count, not time).
- **`HidPoll` mechanism replicates.** Gears sends **only** `flags=0x1`
  (2,368 buckets, zero `0x8`) and shows `flips=0` for the whole session while 3 of
  4 slots return errors on every poll. That is the HID defect latent and harmless
  in this title — the cleanest separation yet between the failing branch and the
  hotplug churn it causes. See `HANDOVER-2026-10-09.md` §3.2a.
- **The `InputEventSender` flood is the SDL pump loop, not the HID churn.**
  `sdl_input_driver.cc:130-133` calls `SDL_PumpEvents()` every **8 ms** = 125/s;
  measured 122.4/s, within 2% of Sleep overshoot. 58,133 messages were 99.7% of
  one logcat. Cost is real (~122 logd writes/s) but no frame-time cost has been
  measured.
- **Gaming performance mode helps, and the mechanism is visible.**
  `cpu_capacity` goes 266/811/1024 (balanced) → 1024/1024/1024 (gaming): the little
  cores are promoted, so emulator threads stop landing on throttled cores. Throughput
  held 30.2–30.55 across ~160 consecutive reports in gaming mode, above the
  pre-switch peak. **Consequence for the A1 work:** `fast_core_mask()` keys on
  `cpu_capacity`, so in gaming mode it sees a uniform machine and correctly
  returns 0 — the pin can only ever engage in balanced mode.
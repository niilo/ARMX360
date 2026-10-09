> **Status update, end of 2026-10-09: A1b is IMPLEMENTED and MEASURED. The result
> is a null.** See §4. A1a is additionally shown to be inert by default. Read §4
> before acting on anything in §1 — one of the two headline claims in the original
> version of this file was wrong and has been corrected in place.

# Candidate findings — porting ARMSX3 (PS3/RPCS3) lessons to ARMX360

**Status: PROPOSAL, not yet actioned.** Written 2026-10-09 from a survey of
`ARMSX2/ARMSX3` on GitHub (an RPCS3 fork, HEAD `f3bdcd5c8`) read against this tree.
Every ARMSX3 claim below is cited to
`file:line` **in ARMSX3**; every ARMX360 claim to `file:line` **here**. Nothing in
this file is a measurement.

Method note: all `3rdparty/*` submodules in the ARMSX3 checkout are **empty** (stub
`.git` files only), so the survey covers first-party code and call sites, not
asmjit/LLVM/VMA internals. `libadrenotools` is an out-of-tree clone and is **absent**;
anything about it is unverified here and is excluded from the actionable list.

The most important result of the survey is not on the actionable list at all: **most
of the portable lessons are already implemented in this fork.** §3 records those so
nobody re-investigates them.

---

## 1. Actionable, ranked by expected value

### A1. Guest threads are pinned by core *index*, so the prime core is never used — HIGHEST

**What ARMSX3 does.** It reads per-core `capacity` from sysfs and builds a
big.LITTLE policy, reserving the fastest core for the thread the frame waits on:

- `Utilities/Thread.cpp:4752-4790` — reads
  `/sys/devices/system/cpu/cpu%u/cpu_capacity`, once, via plain POSIX I/O rather
  than `fs::file` (sysfs reports `st_size == 0`, so a size-based read returns
  nothing and raised "Unexpected fs::error OK" **on the RSX thread**, killing it).
- `Utilities/Thread.cpp:4798-4822` — sets `native_core_arrangement::arm_big_little`
  when `highest != lowest`.
- `Utilities/Thread.cpp:4893-4977` — the policy: frame-gating threads get the fast
  mask, helpers get the slow mask, PPU may spill to all cores. "Anything within 25%
  of the fastest core counts as fast" (`:4912`), so a mid cluster joins the prime
  rather than being lumped in with the little cores.

**What this tree does.** `xthread.cc:1085` pins by guest CPU index:

```cpp
thread_->set_affinity_mask(uint64_t(1) << cpu_index);
```

Guarded by `logical_processor_count() >= 6` (`xthread.cc:1081`), pinning only
`is_guest_thread()` (`:1083`), gated on cvar `ignore_thread_affinities`.

> **CORRECTION (2026-10-09, second pass).** The original version of this file
> claimed the table below showed "the prime core is never used and three of six
> guest threads run on the three slowest cores". **That does not happen by
> default.** `ignore_thread_affinities` defaults to **`true`**
> (`xthread.cc:35`), so `!cvars::ignore_thread_affinities` is false and line 1085
> **never executes**. Nothing is pinned at all by default. Three independent
> sources agree the effective default is `true`: the `DEFINE_bool`
> (`xthread.cc:35`), the bundled template (`default_config.toml:249`), and the
> settings UI (`SettingsSchema.kt:101`).
>
> This was the same error §4 of `HANDOVER-2026-10-09.md` records twice: asserting
> a consequence from a code fragment without reading the condition guarding it.
> The table is still correct **as a map of what would happen** if a user disables
> the setting, which is what makes A1a worth fixing at all — but it is not the
> live behaviour.

**Why that map is wrong on this device** — measured from the attached Pocket S:

| Linux CPU | capacity | max freq |
|---|---|---|
| 0, 1, 2 | **266** | 2016 MHz |
| 3, 4, 5, 6 | 811 | 2803 MHz |
| 7 | **1024** | 3360 MHz |

So the 6 guest Xenon threads land as:

| guest CPU | pinned to | core class |
|---|---|---|
| 0 | Linux CPU 0 | **LITTLE** |
| 1 | Linux CPU 1 | **LITTLE** |
| 2 | Linux CPU 2 | **LITTLE** |
| 3 | Linux CPU 3 | big |
| 4 | Linux CPU 4 | big |
| 5 | Linux CPU 5 | big |

**The prime core (Linux CPU 7) is never used by any guest thread, and three of six
guest threads run on the three slowest cores.** Capacity is available from sysfs on
this device, so the ARMSX3 detection would work here as written.

Scope question to settle before implementing: which host threads are frame-gating?
The GPU/command-processor thread is the obvious candidate and is currently
**unpinned** (only `is_guest_thread()` is pinned). So this item is really two
changes: (a) choose cores by capacity, (b) decide whether to pin the GPU thread and
where. Per `AGENTS.md` §4 this must land cvar-gated and off by default.

**Unverified:** that this actually costs frames. Nothing has been measured. The
claim is only that the mapping is demonstrably not capacity-aware.

### A2. `madvise(MADV_HUGEPAGE)` is never called — MEDIUM

**ARMSX3.** `rpcs3/util/vm_native.cpp:242-311`: forces 64 KiB alignment
(`:251-253`), then `madvise(ptr, orig_size, c_madv_hugepage)` when the region is
2 MiB-aligned (`:298`), plus `MADV_NOCORE`/`MADV_DONTDUMP` (`:304`). Re-asserted on
every `memory_reset` (`:425-431`). Upstream RPCS3, not fork work — but it is free
to copy.

**This tree.** `grep -rn madvise xenia/src/xenia/` returns **nothing**. Guest RAM is
mapped with bare `mmap` at `memory_posix.cc:218` and `mapped_memory_posix.cc:67`.

**Caveat that may make it a no-op:** Xenon has 512 MiB of guest RAM, not the 8 GiB
RPCS3 maps. Whether that region is 2 MiB-aligned in practice is unverified, and if
it is not, `MADV_HUGEPAGE` does nothing. **Check alignment before writing this.**
Also `MADV_NOCORE` is worth considering on its own for a multi-GB guest mapping.

### A3. Descriptor/pipeline host-memory pools are invisible to VRAM accounting — MEDIUM

**ARMSX3.** `rpcs3/Emu/RSX/VK/vkutils/descriptors.h:96-125` caps the pool at **1024
entries on Android** vs 16384 elsewhere, because the comment at `:99-118` records
that the array is **plain host malloc, invisible to the VMM**, and had grown to
49 MB across 56 pipelines on a phone while the pool read 516 MB. That array exists
only because you hand Vulkan a *pointer into it* that must stay valid until flush.

**This tree.** `linked_type_descriptor_set_allocator.cc` exists and pages pools,
but no such size split is present. Needs a sizing review rather than a copy.

**Unverified:** whether this tree has the same host-side staging array at all.

### A4. Per-frame memory-type rebalancing — LOW

**ARMSX3.** `vkutils/memory.cpp:110-163` reorders equivalent memory types
least-used-first, called **every frame** from `VKPresent.cpp:636`. Comment:
"This will avoid constant pressure on the memory budget in low memory systems."

**This tree.** No `rebalance` in `ui/vulkan/`. `vulkan_device.cc:1227-1231` only
walks memory types at init. Straightforward to add; low expected value since
suballocation is VMA's job either way.

---

## 2. Rejected — checked, not applicable. Do not re-investigate.

| ARMSX3 finding | Why it does not apply here |
|---|---|
| **Disable LLVM `InterleavedLoadCombine` on AArch64** (`Utilities/JITLLVM.cpp:943-972`; hung EBOOT compile for 10+ min) — this looked like the single best candidate | **This tree has no upstream LLVM AArch64 backend.** The backend is hand-written (`cpu/backend/a64/a64_emitter.cc`, `a64_seq_*.cc`); `third_party/llvm/` is headers only (64 KB); `find -name "*.cpp" -path "*AArch64*"` returns nothing. The pass does not exist in this compiler. **Rejected on verified grounds.** |
| Compute workgroup 64 on Adreno, not 32 (`VKCompute.cpp:60-76`) | X360 compute group sizes are **guest-ISA-defined** — `group_size_x_log2` comes from the shader (`draw_util.h:621`, `draw_util.cc:1529-1530`). Not host-chosen. |
| Conditional rendering OFF on Adreno/Turnip (`vkutils/device.cpp:471-509`) | `grep -rn "conditional_rendering\|vkCmdBeginConditionalRendering"` over this tree returns **nothing** — the feature is unused. |
| Gate native `shaderFloat16` on driver (`device.cpp:448-469`) | No native fp16 usage; the float16 hits are D3D10 emulation format conversions (`dxbc_shader_translator_*.cc`). |
| Thread stacks: bionic defaults to 1 MiB, use 8 MiB (`Thread.cpp:3916-3932`) | Already done. `threading.h:462` and `:540` default to `4_MiB`, and `threading_posix.cc:1142-1215` has a halving retry ladder down to the platform default. |
| `cntvct_el0` runs at ~19.2 MHz, scale `busy_wait` by `cntfrq_el0` (`asm.hpp:214-245`) | Already done. `threading_posix.cc:278-286` reads `cntfrq_el0` and scales, with a 128-bit intermediate to avoid wrap (`PreciseSleep`). |
| Never use `ISB` as a spin hint — 28% of `vm::writer_lock` (`vm.cpp:944-972`) | Already handled. Every `_mm_pause` is behind `#if XE_ARCH_AMD64 == 1` (`mutex.cc:132-135`, `:219-222`, `mutex.h:74-77`, `:143-146`), so ARM falls through to `MaybeYield`. |
| `setpriority` not `sched_priority` on Android (`Thread.cpp:5187-5210`) | Already done, with the same reasoning in-tree: `threading_posix.cc:1463` falls back to `setpriority` after `pthread_setschedparam`, and `audio_system.cc:214` / `xma_decoder.cc:214` use negative nice directly. |
| `sched_setaffinity` instead of `pthread_setaffinity_np` (`Thread.cpp:5268-5313`) | Already done: `threading_posix.cc:1378` and `:1378` use `sched_setaffinity(pthread_gettid_np(...))` under `XE_PLATFORM_ANDROID`. |
| `VK_NO_PROTOTYPES` dispatch shim + adrenotools loader (`VulkanAPI.h:29-31`, `gen_vk_loader.py`) | Already present. Logcat from our own runs shows `hook_android_load_sphal_library: filename: vulkan.adreno.so` then `loading custom driver: .../libvulkan_freedreno.so`. |
| `vkGetFenceStatus` is a disguised 19.7 ms wait on Adreno — use `vkWaitForFences(…, 0)` (`VKGSRenderTypes.hpp:109-121`) | Already found and fixed here. `gpu_completion_timeline.h:60` and `vulkan_gpu_completion_timeline.cc:62` name the exact same hazard. |
| `compositeAlpha` must not be hardcoded OPAQUE (`swapchain.cpp:436-491`) | Already done: `vulkan_presenter.cc:1297-1315` probes `supportedCompositeAlpha` and falls back through the mask. |
| `VK_SUBOPTIMAL_KHR` latch-don't-rebuild on Android (`VKPresent.cpp:556-586`) | Already done: `vulkan_presenter.cc:1551`, `:2309`. |
| `VK_ERROR_SURFACE_LOST_KHR` is routine and recoverable (`swapchain_core.h:96-101`) | Already done: `vulkan_presenter.cc:1565`. |
| `VK_EXT_memory_budget` must be on or every pressure threshold silently never fires (`memory.cpp:201-225`) | Already done: `vulkan_mem_alloc.cc:78-80` sets `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`. |
| Swapchain present-mode chain + image count (`swapchain.cpp:291-380`) | Already identical: `vulkan_presenter.cc:1328-1344` implements the same IMMEDIATE → MAILBOX → FIFO_RELAXED → FIFO fallback, with `minImageCount = max(kSubmissionCount=3, …)` at `:1258`. |
| Persistent on-disk `VkPipelineCache` keyed on `pipelineCacheUUID` (`device.cpp:1311-1490`) | N/A in this form: this tree's `PipelineCache` (`d3d12_command_processor.cc:1107`) is its own shader/pipeline object store, not a driver `VkPipelineCache`. Our driver cache is saved already (`Saved 3896821 bytes of VkPipelineCache data`). A UUID-keyed *driver* cache is a genuinely separate idea, but it is a new feature, not a port. |
| SIGSEGV handler installed ahead of ART via `libc.so`'s `sigaction` (`Thread.cpp:3130-3194`) | Possible but unexamined — this tree does use mprotect/SIGSEGV guest memory. Left for A-list only if someone is already in that code. |
| 16-byte atomics: `CASP` not `LDAXP/STLXP` (`atomic.hpp:1054-1148`) | Unverified, not rejected. Worth a targeted check of this tree's wide-atomics usage; no known hot poll loop was identified. |

---

## 3. Why the actionable list is short

Of ~30 verified findings in ARMSX3, **16 map to code this fork already has**, and
4 more to features this tree does not use. The two highest-profile ARMSX3 fixes
(`InterleavedLoadCombine`, conditional rendering) are both inapplicable for
structural reasons — different compiler, different GPU feature set.

That is itself the headline: ARMX360 is not behind ARMSX3 on mobile Android
engineering. The one clear gap found is **A1**, and it is a *scheduling* gap rather
than a missing feature — the mechanism (`set_affinity_mask`) is present and correct
on Android, and simply is not told about heterogeneous cores.

**Revised after §4:** that sentence overstated A1. The gap is real in the code but
**not reachable by default** (see the correction in A1), and the one measurable half
of it measured **null**. The survey's net contribution is now: *one item corrected,
one implemented, one null result, and a list of things ARMX360 already does.*

**Suggested order going forward:** nothing here is urgent. A2 only after confirming
the 2 MiB alignment, or it is likely a no-op. A3/A4 as time allows. A1a only if
someone actually runs with `ignore_thread_affinities` off.

**Standing caveat:** every item above is a hypothesis about *possible* benefit. Per
`AGENTS.md` §1 and §4, none may be described as an optimisation until measured, and
anything touching the render or scheduling path ships cvar-gated and off by default.

---

## 4. A1b: implemented and measured — NULL result

**Shipped** (cvar-gated, **default off**, per `AGENTS.md` §4):

| File | Change |
|---|---|
| `base/threading.h:110` | declares `uint64_t fast_core_mask()` — fastest cores, or **0 for "no opinion"** |
| `base/threading_posix.cc:222-323` | the detection: sysfs `cpu_capacity`, falling back to `cpuinfo_max_freq`; intersected with the process's own `sched_getaffinity` mask |
| `gpu/command_processor.cc:45-58` | `DEFINE_bool(pin_gpu_thread_to_fast_core, false, …)` |
| `gpu/command_processor.cc:363-375` | pins the worker right after `Create()`, logging either the mask or why it declined |

**Detection verified working on device.** The log line reads
`GPU Commands: pinned to fast cores mask=0x80` — bit 7, which is exactly the prime
core (capacity 1024) from the A1 table. The sysfs nodes and the capacity-ranking
logic both work as designed on this Pocket S.

**Measurement.** SSX, two runs per arm (trap 1), everything else identical,
`log_gpu_frame_time_breakdown` **off** so no fps number is read off a
timestamp-instrumented run (trap 10). Metric is `frames` per `VkPresentCost`
report; the report fires once per second, so this is a throughput proxy in
frames/report, **not** a frame time.

| arm | run | n | mean | median | min | max |
|---|---|---|---|---|---|---|
| control (pin off) | 1 | 87 | 30.01 | 30 | 30 | 31 |
| control (pin off) | 2 | 87 | 30.02 | 30 | 29 | 31 |
| pinned to prime | 1 | 88 | 30.03 | 30 | 30 | 31 |
| pinned to prime | 2 | 88 | 30.09 | 30 | 30 | 31 |

Control mean-of-means **30.015**; pinned mean-of-means **30.06**. Delta
**+0.045 frames/report**, against an `AGENTS.md` §6 device noise floor of ~2fps —
**two orders of magnitude inside noise. Null.**

**Verdict: pinning the command processor to the prime core does nothing for SSX on
this device.** Per `AGENTS.md` §4 the correct deliverable for a negative result is
to record it, so the change stays **default off** and should stay off.

**What this does and does not license.** The null is **SSX-specific and
single-title**. It is **not** evidence that host-thread placement is irrelevant to
the emulator: SSX sits at a flat, near-constant 30, which looks like a
cadence/pacing limit rather than a CPU-bound one, and a change cannot help where
there is no headroom. A genuinely CPU-bound title would be a different experiment.

**Not tested, and not to be inferred:** whether the GPU thread actually migrates to
a little core when unpinned (the default is that it has no affinity at all, but its
real scheduling was not observed), whether other frame-gating host threads
(audio/APU) behave differently, and anything about Gears — whose steady state was
not characterised at all.

**No CPU profile was taken.** No counter for one was found, and turning on
`log_gpu_frame_time_breakdown` to get one would have violated trap 10 for this run.
So the *reason* for the null is unknown; only its existence is measured.

**A1a remains unfixed and is now the weaker item.** It only affects users who
disable `ignore_thread_affinities`, and this null is weak evidence against spending
the risk of changing which cores the guest's own scheduler sees.

**A2 was not started**, for the reason in its entry: Xenon's 512 MiB guest RAM may
not be 2 MiB-aligned, in which case `MADV_HUGEPAGE` is a no-op. The alignment check
must come before any code.
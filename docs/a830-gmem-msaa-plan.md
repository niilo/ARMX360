# a830 GMEM: findings and the plan to fix it

2026-08-13. Device: AYN Odin 3 (Adreno 830, `3458d5f`), driver: `mesa-tu8`
`gen8-merge-upstream` + uncommitted diagnostics, title: Blue Dragon (4D5307DF),
config: `turnip_debug=''` (GMEM on), instrumentation via
`debug.mesa.tu.perf.sampler=1`.

The goal is fixed: **MSAA render passes tile correctly into GMEM on a830, GMEM
on by default, faster than sysmem.** MSAA→sysmem is measured slower than plain
sysmem and is not a fallback — it routes exactly the passes that would benefit
from tiling away from it. There is no acceptable end state short of the real
fix; interim constraints are allowed only if derived from a measured hardware
envelope, and only while labelled as such.

## Findings ledger

### Established (measured, reproducible)

| Fact | Evidence |
|---|---|
| Fault signature | `kgsl waittimestamp` EPROTO(71) → EDEADLK(35), then emulator SIGABRT on the GPU Commands thread. Hard fault, no page fault (July capture: `gpufaults` col2, zero pagefaults). |
| a830 GMEM is 12 MB | `kgsl_gmem_probe`: `gmem_sizebytes=12582912`, aperture VA `0x4000000`. Kernel-supplied; `TU_GMEM` overrides. |
| Single-sample tiled pass is fine | fb 1280x2048, 1 att (VkFormat 97 = R16G16B16A16_SFLOAT, cpp 8, samples 1), runs indefinitely. |
| The faulting pass is the 2x MSAA one | fb 720x1824, tile0 768x480, att[0] R16G16B16A16_SFLOAT samples=2 cpp=16 off=0 end=5760 KB; att[1] D24S8 samples=2 cpp=8 off=6832128 end=9552 KB. Faults ≤ ~400 ms after first appearance, every run. |
| GMEM layout fits | usable(gmem) = 10032 KB after 2256 KB CCU/VPC reservations; high water 9552 KB. Layout is self-consistent (`off = gmem_pixels × cpp`, `427008 × 24 = 10008 KB`). |
| MSAA works in sysmem | `sysmem` arm: no fault (and the game otherwise runs). So: not a general MSAA bug. |
| Single-sample works in GMEM | So: not a general GMEM bug. The target is exactly the intersection: **RB/CCU writing multisampled pixels into GMEM during rasterization**. |

### Bisection arms — all transfer paths cleared

One binary, `debug.mesa.tu.xd.msaa` selects the arm:

| Arm | Result | Clears |
|---|---|---|
| (normal) | faults | — |
| `sysmem` | **no fault** | confirms MSAA-in-GMEM is the trigger |
| `nobin` (hw binning off) | faults | VSC/visibility path |
| `nostore` (skip GMEM store/resolve) | faults | store path incl. gen8 `va = gmem_offset` TILE6_2 code |
| `noload` (skip GMEM load) | faults | load path |

### Refuted — do not revisit

- **VSC prim-stream overflow.** Real (16448→32832→65600 unbounded doubling),
  fixed by 16× initial pitch — overflow then zero everywhere **and it still
  faulted**. Keep the pitch fix; it is not the cause.
- **Attachment past usable GMEM.** The earlier "EXCEEDS USABLE" flags were an
  instrumentation bug (cpp already includes samples; multiplying again
  double-counts — `tu_pass.cc:1026`).

### Found incidentally (real bugs, fix regardless)

1. **Unsigned underflow in `fd6_gmem_cache.h __calc_gmem_cache_offsets`**: five
   reservations subtracted with no floor; GMEM < ~2.26 MB wraps offsets to
   ~4 GB. Assert-builds abort in `__RB_CCU_CACHE_CNTL` (`depth_offset`
   field-fit); release builds program garbage CCU bases silently. Audit a810
   (576 KB entry) against its reservations. Upstreamable guard.
2. **VSC initial prim pitch** far too small for this workload; growth-by-
   doubling overruns before converging (overrun happens before detection).
3. **a830 has no per-device GMEM/CCU tuning** — only a8xx part without one;
   inherits `a8xx_gen1` (16 KB/CCU color EIGHTH, 256 KB/CCU depth FULL) on a
   6-CCU/3-slice part. a825's own entry admits its values are guesses.

### Invalid run — must not be counted

The `TU_GMEM=4194304` run: APK was reinstalled mid-test (path hash changed),
validation + RenderDoc layers present, crash had none of the fault signature
(no tiled pass logged, no kgsl errno). Re-run after re-baselining. The 2 MB
run only proved the underflow bug (2 MB < 2256 KB reservations).

## Remaining mechanism space

All three data-movement paths are cleared, sysmem-MSAA and GMEM-1x both work.
Candidates, ranked:

**H1 — GMEM offset/sample scaling on a8xx.** If gen8 RB/CCU applies a sample
multiplier to per-pixel GMEM addressing where the driver has *already* baked
samples into byte offsets (cpp = blocksize × samples), att[1]'s effective
address ≈ 6.5 MB × 2 = 13 MB > the 12 MB aperture → hard fault, no page fault.
Fits every observation: att[0] (off 0) safe at any scaling, 1x passes safe,
transfer paths (separately programmed) safe. Predicts: fault vanishes when
offsets shrink.

**H2 — CCU cache geometry wrong for a830.** Untuned inherited config on 6
CCUs; MSAA doubles CCU traffic per pixel; wrong fraction/size/offset lets the
CCU write outside its reserved window during rendering. Predicts: fault is
*independent* of GMEM clamp (reservations don't scale with `TU_GMEM`).

**H3 — a8xx register field semantics** (RB GMEM base units, per-sample stride
encoding) subtly wrong in the gen8 fork for the MSAA case. Overlaps H1;
distinguished only by reading actual register values.

`TU_GMEM=8388608` is a clean H1-vs-H2 discriminator: it shrinks attachment
offsets (~3.9 MB for att[1]) without touching CCU geometry.

## The plan

### Phase 0 — re-baseline (one launch)

The APK changed under us. Before any new evidence: confirm which build is
installed, check leftover GPU-debug-layer settings (`settings list global |
grep gpu`), then reproduce the **baseline fault** on this APK (arms unset,
`TU_GMEM` unset) with the full signature: `XD gmem:` MSAA pass logged →
EPROTO → SIGABRT. No baseline, no experiments.

### Phase 1 — pin the mechanism (one launch per step)

1. `setprop debug.mesa.tu.gmem 8388608`, MSAA normal.
   - **No fault → H1.** Bisect 8→12 MB to the exact ceiling; the boundary
     arithmetic (which offset × what factor crosses what limit) names the
     broken semantics directly.
   - **Fault → H2/H3.** Build an arm that programs a830's GMEM-mode CCU
     config with sysmem-mode fractions/sizes (and/or FULL fraction), re-test.
2. Scope sweep (cheap, same binary): does 4x MSAA fault? Does the fault need
   the 64bpp color target, or does RGBA8 2x also fault? Narrows the fix and
   feeds the reproducer matrix.
3. `.rd` capture of the faulting submit (`TU_DEBUG=rd` via property,
   `FD_RD_DUMP_PATH` to app files dir): read back `RB_MRT[*]_BASE_GMEM`,
   `RB_CCU_CACHE_CNTL`, sample-count fields; diff the same pass captured in
   sysmem mode. This turns hypotheses into register values.

### Phase 2 — ground truth (parallel, off the launch loop)

- **Standalone Vulkan reproducer** (NDK, `/data/local/tmp`, forces GMEM):
  one render pass, {RGBA8, RGBA16F} × {1,2,4} samples × {D24S8, none} ×
  sweep of extents. Seconds per iteration instead of a game launch; the fix
  is proven against this matrix, then confirmed in the game — not the other
  way round.
- **KGSL snapshot → cffdec bridge.** crashdec parses drm/msm devcoredumps,
  not KGSL binary snapshots (magic `0x504D0002`) — that dead end is already
  documented. The snapshot section format is in the downstream kernel
  headers; a ~300-line parser feeding mesa's existing cffdec gives the exact
  PM4 packet and register state at fault. We already hold two undecoded
  snapshots (fault139, a650 GoW hang) and can capture fresh ones. This is a
  permanent capability, not scaffolding.
- **Mine downstream kgsl gen8 config** (LineageOS sm8750-modules,
  `adreno_gen8_*` core configs — same source that decoded the gpufaults
  columns) for a830's real CCU/GMEM constants → a genuine per-device
  `freedreno_devices.py` entry to replace the inherited guesswork, whichever
  hypothesis wins.

### Phase 3 — the fix, and only the fix

- Implement the proven mechanism where it belongs: device config
  (`freedreno_devices.py`), RB emission (`tu_cmd_buffer.cc`), or a8xx pack
  semantics — gated to a8xx, no quirks, no game checks.
- If the mechanism turns out to be a hard hardware envelope (offsets/sizes
  MSAA can address in GMEM), the constraint goes into the **GMEM layout
  math** so MSAA passes still tile within the envelope (smaller
  `gmem_pixels`/divisor for MSAA passes) — *keeping* GMEM for MSAA, not
  routing around it. That is the only acceptable shape of "workaround", and
  only with the measured ceiling written next to it.
- Land separately, in order: (1) underflow guard + a810 audit, (2) VSC
  pitch/growth fix, (3) a830 device config from downstream values, (4) the
  MSAA fix. Each upstreamable in shape. Diagnostics (`tu_xd_msaa_mode`,
  noload/nostore/nobin arms) are deleted once the mechanism is pinned —
  they are scaffolding, not product.

### Validation gate (all required)

- Reproducer matrix green across formats/sample counts.
- Blue Dragon 30+ min soak, GMEM on — the historical "~4 min under load"
  device-lost class must not reappear.
- Halo 3 spot check (known a830 Turnip GPU-stall title).
- fps A/B: GMEM must beat sysmem on the MSAA-heavy scenes, else the fix is
  not done.
- Instrumentation off (`debug.mesa.tu.perf.sampler` unset): zero residual
  cost, identical behaviour.

## Device/session state as of this doc

- `Turnip_Gen8_Debug` slot: diagnostic build (MSAA selector + 16× VSC pitch);
  meta.json description stale.
- Properties: `debug.mesa.tu.perf.sampler=1` set; `debug.mesa.tu.gmem`
  cleared; `debug.mesa.tu.xd.msaa` cleared.
- Blue Dragon config: GMEM on, driver pinned to the debug slot.
- mesa-tu8 uncommitted: VSC pitch (tu_device.cc), arms + instrumentation
  (tu_cmd_buffer.cc, tu_clear_blit.cc, tu_xd_instr.h).
- Two prior wrong root-cause calls (VSC, layout overflow) are why Phase 3
  requires a register-level proof and a reproducer before any fix lands.

---

## Addendum — 2026-08-13, after the smalltile/CCU arms

### New results

- **`smalltile` (H1 arm): faults.** Divisor forced to max: tile0 768x480 →
  192x96, spans 5760/2880 KB → 288/144 KB, pipes reconfigured 1x4 → 2x10.
  Refutes per-tile footprint, tile dimensions, tile count and pipe geometry
  as triggers. **It does not test H1 proper:** `gmem_offset[]` is indexed by
  layout only — the divisor shrinks spans but att[1]'s base offset stayed at
  6832128. Base-offset magnitude remains untested.
- **First real CCU geometry readout** (`XD ccu:`): gmem color off=11845632
  frac=EIGHTH size=16 KB, depth off=10272768 frac=FULL size=256 KB; sysmem
  color off=10420224 frac=FULL size=128 KB, depth off=8847360 frac=FULL
  size=256 KB. Internally consistent, nothing out of range.
- **The TU_GMEM=8388608 run reinterpreted.** Not an H1 test (it pushed both
  passes out of GMEM — usable 5936 KB < 7392 KB needed by the 1x pass) and
  weak H2 evidence at best. What it actually showed: with **every pass in
  sysmem** and only the derived CCU/VPC region placement moved (all values
  in-range and encodable), the GPU still device-lost. Baseline sysmem works.
  So there is a second latent sensitivity: gen8 CCU/VPC **placement rules**
  are load-bearing in ways the fork's math does not capture, sysmem included.
  That may share a root with the MSAA-pass fault or be separate; either way
  the fork's model of gen8 GMEM-region placement is incomplete.

### Correction to the headline conclusion

"MSAA-in-GMEM is the culprit" is **overstated**. The workload only ever
produces two tiled pass shapes:

| | works | faults |
|---|---|---|
| samples | 1 | **2** |
| attachments | 1 (color only) | **2 (color + D24S8)** |
| depth in GMEM | no | **yes** |
| non-zero base offset | no (att at 0) | **yes (att[1] at 6832128)** |

Four variables, perfectly confounded — every 3D pass in this game is
MSAA+depth, and the only non-MSAA tiled pass is color-only at offset 0. The
`sysmem` deflection keyed on `samples > 1`, but it would have "confirmed" any
of the four. Established, precisely: *this pass shape faults during GMEM
rasterization; which property triggers it is not isolated.* This matters for
the fix and for any upstream report.

### Revised next steps (in order)

1. **Run the armed CCU arms** (`a825`, then `full`) — zero build cost,
   closes or confirms narrow-H2 (fraction/size pairs). `XD ccu:` line
   verifies each arm took.
2. **New arm `swap`:** reverse GMEM allocation order for the faulting pass
   (depth at 0, color at ~3.4 MB). One launch splits "non-zero base offset"
   from "attachment identity": fault follows the offset → H1'; follows the
   depth attachment → depth-GMEM programming; vanishes → ordering/alignment.
3. **Register-level evidence — after which no more arms.** `TU_DEBUG=rd`
   via property (`debug.mesa.tu.debug`), `FD_RD_DUMP_PATH` to an
   app-writable dir; capture the faulting submit, a 1x-GMEM submit, and the
   same pass in sysmem. Decode with the already-configured host tools;
   diff every GMEM-related register (RB_MRT gmem base, RB_DEPTH_*_GMEM,
   CCU cache bases, sample state) against the known layout numbers. Tests
   H1' units/scaling, depth-vs-color programming, and H3 encodings in one
   artifact.
4. **Standalone reproducer is now the critical path for naming the
   trigger** — the game cannot decompose the confound; the reproducer can:
   {1x,2x} × {color-only, color+depth} × {RGBA8, RGBA16F} × {second
   attachment at small/large offset}, forced-GMEM, seconds per iteration.
5. **Downstream kgsl gen8 mining** (sm8750 `adreno_gen8_*`): CCU/GMEM
   constants and any per-slice placement constraints (a830 = 6 CCUs over 3
   slices; the fork lays regions out as one contiguous block — whether
   hardware requires per-slice distribution is exactly the kind of rule the
   8 MB sysmem failure hints at). Feeds the real device entry regardless.
6. KGSL snapshot→cffdec parser: keep on the list; decide after (3) whether
   the rd captures already pin the faulting packet.

---

## Addendum 2 — layout space exhausted, moving to register capture

Results after the first addendum, all arms verified applied via `XD gmem:`:

- `ccu=a825` (color HALF/128K, depth HALF/128K): **faults.** With the default
  (EIGHTH/16K + FULL/256K) also faulting, two very different CCU geometries
  fail identically — fraction/size tuning is dead as a standalone cause.
- `swap` (depth at offset 0, color at 3416064): **faults.** Attachment
  identity is irrelevant. Also kills the naive "base × samples > aperture"
  H1 variant (3.4 MB × 2 is comfortably in-aperture).
- `lowoffs` (allocator capped: att[0] 0..960 KB, att[1] 1008..1488 KB,
  CCU/VPC untouched): **faults.** The end-of-region scaling variant is
  refuted too — offset magnitude is fully cleared.

**Conclusion: 2x MSAA rasterization into GMEM faults on a830 regardless of
every layout parameter the driver controls** (footprint, tile/pipe geometry,
binning, load, store, CCU pairs, attachment identity, offset magnitude).
The defect is in MSAA-specific GMEM register/command state on gen8.

Architecture note (verified in-tree): a830 (0x4405) is a840's silicon family
(0x44050A31), identical 6-CCU/3-slice/96-align topology; a825/829 are 0x4403
2-slice parts. a830 is misfiled under `a8xx_gen1` — though gen1 ≡ gen2 in all
GMEM-mode values, so the misfile alone does not explain the fault. The
fraction-encoding collision (3 = EIGHTH pre-gen2 vs THREE_QUARTER gen2+) on
a830's color CCU remains an open question for the rd dump to answer
(`RB_CCU_CACHE_CNTL` raw value).

Next artifact — not another arm: rd capture is armed on-device
(`debug.mesa.tu.debug=rd`, `debug.mesa.fd.rd.dump=enable`, dump path in the
app's files dir). Compare, register by register: the faulting MSAA-GMEM
submit vs the working 1x-GMEM submit vs the same MSAA pass in sysmem.
Host cffdump is built (`mesa-tu8/build-host-tools/src/freedreno/decode/`).

---

## Addendum 3 — register-level capture decoded: it's a pipeline wedge, not a CP fault

Artifacts: rd dump of the faulting run (submits 3519-3523 + control 1350 pulled
to the session scratchpad), kernel fault header via plain `dmesg` (no root):

```
adreno-gen8-gmu: MISC: GPU hang detected        (hang; zero page faults)
BR: ib1 0x40431A4000/002f  ib2 0x40431CE120/0000
BV: ib1 0x40431A4000/002f  ib2 0
```

Decoded (trimmed-rd windows; full cffdump is impractically slow, the
window trick decodes in seconds):

- The freeze point is in **submit 3522's first render-pass prologue** — a
  **DIRECT_RENDER (sysmem) pass targeting the MSAA framebuffer** (scissor
  719x1823, `RB_BUFFER_CNTL` all-SYSMEM, GMEM dims zeroed, LRZ disabled with
  poison values), ~300 dwords after a completed 9-dword SP-state IB2, frozen
  mid context-register writes between `CP_THREAD_CONTROL(BR |
  CONCURRENT_BIN_DISABLE)` and the never-reached `CP_SET_MARKER
  (RM6_DIRECT_RENDER)` + first `CP_INDIRECT_BUFFER`.
- Plain register writes cannot block the CP; both threads freezing there is
  **backpressure**: the RB/CCU pipeline is already wedged by earlier work
  when the prologue's register traffic fills the queue.
- Therefore the wedge is created by the **GMEM MSAA rendering in the
  preceding submissions** and only *detected* at the next prologue. This
  explains the entire arm ledger at once: every layout/identity/offset arm
  faulted (none removed GMEM MSAA work), the sysmem deflection alone
  survived (it removed all of it), and every TU_DEBUG flag crashed the same
  (none of them remove GMEM MSAA rendering wholesale).
- The workload **mixes GMEM and DIRECT_RENDER passes against the same MSAA
  framebuffer** (autotune decides per pass), with
  `THREAD_CONTROL(CONCURRENT_BIN_DISABLE)` transitions between modes — the
  wedge sits somewhere in GMEM-MSAA work + mode transition.
- gen8 runs a **resolve-group protocol** (per-blit `CCU_RESOLVE` events
  closed by `CCU_END_RESOLVE_GROUP`). Every observed `RB_RESOLVE_OPERATION`
  has `LAST = 0`, in working and faulting passes alike; the working 1x
  control shows 12 group-ends vs 10 resolves. Depth ops carry
  `SAMPLE_0 | DEPTH | BUFFER_ID=0x8`.

### Next

1. Window-decode the full GMEM MSAA pass (s3519/s3521) and the
   GMEM→DIRECT_RENDER transition; count resolves vs group-ends per pass.
2. Source-map `tu_emit_resolve_group` / `CCU_END_RESOLVE_GROUP` emission and
   the THREAD_CONTROL transition rules; look for what a830 (a840-family)
   expects vs what the gen1-filed path emits.
3. Only then: arm candidate fixes (group-close/serialization variants around
   MSAA GMEM stores) — cheap A/B, one launch each.
4. Verify freeze-PC stability across 2-3 more faults via dmesg alone (no rd
   overhead needed).

Toolkit gained: `dmesg` fault headers without root; trimmed-rd windowed
decode (seconds vs minutes); rd files are gzip despite the .rd name.

---

## Addendum 4 — the wedge is localized; shared-constants prime suspect

Full structure of the frozen submission (s3522, mapped packet-by-packet):
one GMEM MSAA pass (binning marker 0x2 → BV section → BR with concurrent
binning ENABLED → 4 tiles of [0x84 draws 0x85 tile-store 0x7]) → three
post-pass marker-0x8 sections → one DIRECT pass executed OK → freeze in the
second DIRECT prologue, ~250 dwords after the GMEM pass.

Decoded the small IBs:

- The per-tile store IB is well-formed: color store (MSAA_TWO, gmem 0) +
  depth store (gmem 0x684000) + CCU_END_RESOLVE_GROUP. (Correctness sidebar:
  MSAA depth store carries SAMPLE_0 — likely drops sample 1; not the hang.)
- **The marker-0x8 post-pass stubs are SP_SHARED_CONSTANT_GFX writes**, and
  the kernel fault header's "BR last IB2" (0x40431ce120) is exactly one of
  them: the GPU's last completed act was a shared-constant write while BV
  was live in concurrent-binning mode.

Precedent: the a650 4-squares bug — shared-const execution-overlap race,
WFI-proven ([[turnip-a650-4squares-investigation]]). XenDroid auto-applies
`TU_DEBUG=push_consts_per_stage` ONLY for a6xx (`vulkan_instance.cc` checks
gpu_model/100==6); a830 never gets it.

Two free discriminators (properties only, dmesg as readout — freeze PC
moves/vanishes if the mechanism is hit):

1. `debug.mesa.tu.debug=push_consts_per_stage`  ← ARMED
2. `debug.mesa.tu.debug=nocb` (no concurrent binning) — next if (1) is clean
   or unclear; separates shared-consts from BV/BR concurrency generally.

If (1) fixes it: the driver-side fix is routing shared constants per-stage
(or fencing shared-const updates against the concurrent pipe) on a8xx —
plus XenDroid extending its auto-flag to gen8. If (2) fixes it and (1)
does not: the fix is in the concurrent-binning rendezvous protocol.

---

## Addendum 5 — protocol arms exhausted; synthetic repro is clean; moving to stream replay

Further negative results (each verified applied):

- `rg=sl` (singleton resolve groups + LAST=1 on every op): **faults.**
- `vulkan_in_pass_resolve=false` (xenia's local_read resolves off): **faults.**
- `ccu=full`, `a825`: fault. `ccucntl` override: **impossible** — RB_CCU_CNTL
  is CP-protected on gen8 (`CP | Protected mode error | WRITE | addr=0x08e07`);
  the kernel fixes full-concurrent resolve mode for all userspace including
  the blob. TU_DEBUG noconcurrentresolves/unresolves are silent no-ops on
  A8XX (`tu6_init_static_regs` writes RB_CCU_CNTL only `if (CHIP == A7XX)`).
- `nocb` IS honored (drirc allow_concurrent_binning=false) — concurrency
  genuinely tested and cleared.

**Standalone reproducer built and run on-device** (direct HAL-module loading —
the Android driver exports only `HMI`, no ICD symbols):
18 cases, all CLEAN in both forced-GMEM and default-heuristic modes:
{1x,2x,4x} × {color-only, +D24S8} × {RGBA8, RGBA16F} × {store, dontcare},
plus vkCmdResolveImage after the pass, vkCmdClearColorImage between passes,
second pass per cmdbuf, 3-deep pipelined submissions, and real VB/IB indexed
draws (VFD traffic). **Plain MSAA GMEM rendering works on a830.** The wedge
requires something in xenia's stream beyond all of the above.

**Next: replay the actual captured faulting stream.** mesa's
`src/freedreno/decode/replay.c` supports KGSL: allocate one huge buffer
(kgsl VA base is deterministic - probe measured 0x4000000000 every time),
place captured buffers at original-address offsets, submit the captured IB1s
via IOCTL_KGSL_GPU_COMMAND. Game buffers span base..+~1.1GB → needs a ~2GB
fake address space. Building mesa's replay for Android fights libdrm/
libarchive deps; a minimal purpose-built kgsl replayer (rd parser already
written, kgsl ioctls already exercised by the probes) is the fast path.
Then bisect the faulting IB1 packet-by-packet (truncate/NOP progressively) -
the wedge packet falls out mechanically, no more hypotheses needed.

Artifacts: reproducer + probes in session scratchpad `repro/`; rd captures
of the faulting frame pulled and decoded; vendor kernel tree at
~/mesa-turnip/graphics-kernel (gen8_0_0 = the 12MB a830-class entry;
gen8_3_0 in the vendor gpulist is a 576KB-GMEM small SKU, NOT a830 —
corrects the old memory note).

---

## Addendum 6 — the XenDroid half is bounded; the shared-const knob is now reachable

### Environment this was written from (and what it therefore cannot claim)

Authored **without** the mesa tree and **without** an a830. Verified on the
authoring host: `cmake`/`ninja`/`java`/`gradle` all absent, no
`ANDROID_HOME`, no `~/mesa-turnip`, no `freedreno/gens`, so no `tu_device.cc`,
`fd6_gmem_cache.h` or `freedreno_devices.py` was readable. Nothing here is a
driver patch and nothing here was measured. Everything in this addendum is
either read out of this repo with file:line, or read off a real attached
**a7xx** device (not a830) via `xe.log`.

### Finding 1 — the `push_consts_per_stage` gate never fires on a7xx or gen8, and could not have

`vulkan_instance.cc` gates on `AdrenoGeneration() == 6`, and `AdrenoGeneration()`
is `strpbrk(node,"0123456789")` then `atoi(...)/100` on
`/sys/class/kgsl/kgsl-3d0/gpu_model` (the comment above it already records this
trap). Measured node values, and what that arithmetic makes of them:

| device | `gpu_model` node | `AdrenoGeneration()` | `== 6`? |
|---|---|---|---|
| Adreno 740 (test device, SD 8 Gen 2) | `AdrenoA32` | **0** | no |
| Adreno 830 | `3458d5f` | **34** | no |
| Adreno 650 | `61705c55` | **617** | no |
| hypothetical `Adreno 650` | `Adreno 650` | 6 | yes |

### Finding 2 — XenDroid does not use push constants for guest drawing at all

This is the load-bearing structural fact, and it reframes the whole
shared-const theory. The guest graphics pipeline layout is created with
`pushConstantRangeCount = 0; pPushConstantRanges = nullptr`
(`vulkan_command_processor.cc:3666-3667`). Guest shader constants reach the
hardware as **five descriptor-set uniform buffers** — `kConstantBufferSystem`,
`FloatVertex`, `FloatPixel`, `BoolLoop`, `Fetch`
(`spirv_shader_translator.h:367-374`), filled per draw from
`uniform_buffer_pool_` (`vulkan_command_processor.cc:7820-7957`), optionally as
`UNIFORM_BUFFER_DYNAMIC` (`vulkan_command_processor.h:1190-1200`).

Consequence: **`push_consts_per_stage` cannot affect guest shader constants**,
because guest constants are never push constants. What it *can* affect is
Xenia's own scaffolding passes, which are the only push-constant users in the
GPU path: in-pass resolve (fragment, `vulkan_render_target_cache.cc:2256`),
direct host resolve (compute, `:2634/:2638`), host depth store (`:1057`),
EDRAM transfer/dump (`:7621`, `:7661`, `:8349`-`:8381`, `:9180`), texture-cache
load (`vulkan_texture_cache.cc:2137-2150`), and the present-path helpers
(swap/gamma `vulkan_command_processor.cc:2806`, FXAA `:2937`, resolve downscale
`:5469`).

That is consistent with the decode in addendum 4: the frozen IB2s are
`SP_SHARED_CONSTANT_GFX` writes in **post-pass marker-0x8 sections**, i.e. after
the GMEM MSAA pass — which is exactly where Xenia's resolve/dump scaffolding
sits, not where game draws are. It does not prove the mechanism (the driver's
own internal constant traffic is still a candidate), but it does mean the
Xenia-side lever is *"how much shared-const traffic does the scaffolding emit,
and when"*, not *"how the game's constants are delivered"*.

### Honest verdict on the split

- **Xenia-fixable, and now reachable:** nothing about the *wedge* — that is a
  driver ordering bug between the shared-constant path and concurrent
  binning, and Xenia cannot fence the GPU's internal threads. What Xenia can do

### Change made (this repo, cvar-gated, default behaviour UNCHANGED)

`vulkan_instance.cc` gains **`turnip_push_consts`** (`auto`|`on`|`off`,
default `auto`). `auto` reproduces the old `== 6` gate exactly, so **no
existing device changes behaviour**; `on` forces the flag everywhere so the
a830 hypothesis becomes a **one-line config A/B instead of a rebuild**. Wired
into `SettingsSchema.kt` (list choice), `SettingDescriptions.kt`, and
`default_config.toml`, so it is settable per-game via `config/<TITLEID>.config.toml`
and from the in-app UI. The decision is now logged unconditionally, and
`AdrenoGeneration()`'s real `gpu_model` is logged on every launch, so
"auto did nothing because the node didn't parse" stops being indistinguishable
from "the flag was applied and didn't help".

**This does not widen the gate on purpose.** Widening to gen8 on the strength of
a *localized but unattributed* wedge would tax every a7xx/gen8 owner with a real
shader-constant bandwidth cost on the strength of a theory. Measure first (step 1
below), then widen if it earns its keep.

### Incidental-bug triage — all three are driver-only

Grepped the whole repo (excluding `docs/`) for each:

| bug | lives in | in this repo? |
|---|---|---|
| `__calc_gmem_cache_offsets` unsigned underflow (5 reservations, no floor; GMEM < ~2.26 MB wraps to ~4 GB) | `fd6_gmem_cache.h` | **no** — 0 hits outside docs |
| VSC initial prim pitch too small / doubling overruns before detection | `tu_device.cc` | **no** — 0 hits outside docs |
| a830 misfiled under `a8xx_gen1`, no per-device GMEM/CCU tuning | `freedreno_devices.py` | **no** — 0 hits outside docs |

None can be fixed, patched or even reviewed from here: the files do not exist in
this tree and there is no vendored copy. Nothing was written against them. All
three remain **open, driver-side, and unowned by this repo** — they should be
filed as upstream patches by whoever holds `~/mesa-turnip`, in the order the
plan already gives (underflow guard + a810 audit → VSC pitch → a830 device
config → MSAA fix). They are independent of the MSAA wedge and worth landing
regardless of how the wedge resolves.

### Runbook — for whoever has BOTH the mesa tree AND an a830

Ordered cheapest-first; each step states what it *proves or refutes*. Steps 0–3
need only the device. Step 4 needs the mesa tree. Do not skip to step 4 — it is
the expensive artifact and steps 0–3 may make it unnecessary.

**Step 0 — re-baseline (no code).** Confirm the installed APK hash, clear
leftover `debug.mesa.tu.*` properties, reproduce the full baseline signature
(`XD gmem:` MSAA pass → EPROTO → SIGABRT). Verify from `xe.log` that
`Set TU_DEBUG=` shows exactly the intended flags and that the new
`turnip_push_consts='...' -> push_consts_per_stage ...` line reads as expected.
*Proves:* the fault still exists on this build. A clean baseline invalidates
everything below.

**Step 1 — the armed discriminator, now one config line.** Set
`turnip_push_consts = "on"` (global config or `config/4D5307DF.config.toml`),
launch, watch for the fault.
- **Fault gone → shared-const race is the mechanism.** Land the driver fix
  (per-stage shared consts, or fence shared-const updates against the
  concurrent pipe) *and* widen the auto-gate to gen8 in `vulkan_instance.cc`.
- **Fault persists → the a650 precedent does not transfer to gen8.** Do **not**
  widen the gate. Go to step 2.
*Proves/refutes:* the addendum-4 hypothesis, which is currently localized but
unattributed. This is the single highest-value step in the whole plan and costs
one launch and no build.

**Step 2 — the free second discriminator.** `debug.mesa.tu.debug=nocb`
(no concurrent binning). Fault gone → the bug is in the concurrent-binning
rendezvous, not in shared-const delivery specifically; together with step 1 this
separates the two. Fault persists → the trigger is neither shared consts nor
binning concurrency, and the remaining suspects are back to the GMEM MSAA
rasterization path itself.

**Step 3 — quantify the Xenia-side shared-const traffic (no driver change
needed).** Count Xenia's own post-pass push-constant writes per frame from the
source: in-pass resolve (`:2256`, only when `vulkan_in_pass_resolve=true`),
direct host resolve (`:2634`/`:2638`), host depth store, EDRAM transfer/dump,
texture load. Then A/B the one lever that removes a mid-pass fragment push
constant: `vulkan_in_pass_resolve = false` — but note its C++ default is
*already* `false` (`vulkan_render_target_cache.cc:90`), so on a default config
this changes nothing; force it **on** first to confirm it is active at all.
*Proves:* how much shared-const traffic Xenia contributes. If it is a handful of
writes per frame against thousands of draws, the Xenia-side mitigation in
Finding 2 is not worth building and the fault is entirely driver-internal.

**Step 4 — only now: stream replay bisect.** The reproducer matrix is already
clean (addendum 5), so the trigger is in xenia's stream. Build the
purpose-built kgsl replayer (rd parser + `IOCTL_KGSL_GPU_COMMAND` at the probed
deterministic 0x4000000000 kgsl VA base, ~2 GB fake address space), replay the
captured faulting IB1s, then truncate/NOP progressively — the wedge packet falls
out mechanically. **This is the most expensive artifact in the plan and belongs
to whoever has the mesa tree**; it is deliberately not started here.

### Two corrections to the record

- The doc's line "XenDroid auto-applies `TU_DEBUG=push_consts_per_stage` ONLY
  for a6xx (`vulkan_instance.cc` checks gpu_model/100==6)" is right about the
  code and wrong about the effect: on a7xx/gen8 the check cannot succeed at all
  (Finding 1), so it is not a gate drawn too narrow — it is a gate fed by a
  parser that returns 0/34/617. Fixing the parse is a prerequisite for *any*
  generation-gated policy, including a future a6xx fix.
- `SettingsSchema.kt:37` declares `vulkan_in_pass_resolve` default `true` while
  the C++ default is `false` (`vulkan_render_target_cache.cc:90`). Pre-existing
  divergence, flagged not fixed — the UI default would silently enable a
  feature the binary does not enable by default.

  is reduce or relocate its own post-pass shared-const writes (e.g. move the
  in-pass resolve's fragment push constants into the same UBO mechanism the
  guest path already uses, so no shared-const write is emitted mid-pass at
  all). That is a *plausible mitigation*, not a fix, and it is a real code
  change worth trying only after the driver-side arm below is measured.
- **Driver-only:** the concurrent-binning/shared-const rendezvous itself, the
  CCU geometry, the GMEM layout, the VSC pitch. None of it is reachable from
  this repo — see the triage table below.


So the workaround applies **only** when the node happens to read `Adreno NNN`.
On a7xx/gen8 it is not merely "gated out" — the parse never yields 6/7/8 at
all. Widening `== 6` to `>= 6` would therefore change **nothing** on those
parts; it is a no-op dressed as a fix. Confirmed at runtime on the attached
device (`xe.log`, a real launch): `Set TU_DEBUG=sysmem for the Turnip Vulkan
driver` — bare `sysmem`, and `grep -c push_consts xe.log` = **0**. The
workaround was never applied there.


# Fragment-shader register pressure / occupancy — is the heavy ALU ours?

**Author:** GPU/rendering agent · **Status:** analysis only, **no code changed,
no measurement** · **Related:** `docs/gw-gpu-bottleneck-investigation.md`
sections 4, 5.2, 7, 10 trap 4; `docs/gpu-pass-count-levers.md`.

Task_0006 asks for a register-pressure / occupancy attack on the four heavy
fragment-shader variants (971-2228 instructions, 4-8 waves, 40-57% NOPs). Its
own caveat is the right instruction here: **establish first which of those
variants are emulation scaffolding and which are the game's own shaders. If the
load is the game's own shading there is nothing to win, and inventing work here
would be worse than saying so.**

This document is that determination. The short answer is at the bottom.

---

## 1. What the emulator actually emits into a fragment shader on this device

The fragment-shader epilogue lives in
`spirv_shader_translator_rb.cc::CompleteFragmentShaderInMain` (`:429`). Almost
all of it is gated on `edram_fragment_shader_interlock_`, which is true only on
the pixel-shader-interlock ("accuracy") backend:

- `vulkan_render_target_cache.cc:620-624` — `path_ = Path::kPixelShaderInterlock`
  only when `render_target_path == "accuracy"`.
- `vulkan_command_processor.cc:607-609` — `edram_fragment_shader_interlock` is
  derived from that path and passed into the translator
  (`spirv_shader_translator.h:466`, stored at `:946`).

`render_target_path` defaults to `"performance"`
(`app/src/main/java/xendroid/compose/settings/SettingsSchema.kt:167`) and the
GW investigation records it running `"performance"`
(`docs/gw-gpu-bottleneck-investigation.md:233`). So on this device
`edram_fragment_shader_interlock_` is **false**, and the following are never
emitted:

| Emulator helper | Where | Gate |
|---|---|---|
| `FSI_ClampAndPackColor` / `FSI_UnpackColor` (incl. `PreClampedFloat32To7e3`, `UnclampedFloat32To7e3`) | `:2825`, `:3195`, `:24`, `:108` | called only from inside `if (edram_fragment_shader_interlock_)` at `:1083`, `:1123`, `:1157` |
| `FSI_DepthStencilTest` | `:2026` | called only at `:752` and `spirv_shader_translator.cc:3466`, both inside the gate |
| `FSI_ApplyColorBlendFactor` / `FSI_ApplyAlphaBlendFactor` / `FSI_BlendColorOrAlphaWithUnclampedResult` / `FSI_FlushNaNClampAndInBlending` | `:3524`, `:3722`, `:3879`, `:3490` | reachable only from `:1092`/`:1098`, inside the gate |
| `FSI_LoadEdramOffsets`, `FSI_LoadSampleMask`, `FSI_AddPassedMSAASamplesToZPD` | `:1799`, `:1701`, `:1971` | `:1971` asserts the gate is set |
| `CompleteFragmentShader_DSV_DepthTo24Bit` | `:1529`, `:1532` | gated at `:1532` |
| the EDRAM storage-buffer declaration itself | `spirv_shader_translator.cc:3097-3156` | gated at `:3097` |

That is the §7 refutation, confirmed from the code rather than from the
instrumented driver: **the 7e3 pack/unpack arithmetic is not in these shaders at
all.**

## 2. What IS ours, and is emitted on the performance path

Exactly two things, both small, both in the epilogue of every colour-writing
fragment shader:

### 2.1 The alpha-to-coverage sample-mask computation — unconditionally compiled in

`FSI_AlphaToMask()` is called with no runtime guard at
`spirv_shader_translator_rb.cc:627` whenever the shader writes RT0 and is not
using early fragment tests. `gl_SampleMask` is declared for every non-depth-only
fragment shader (`spirv_shader_translator.cc:3360-3371`), and the FBO half of
`FSI_AlphaToMask` returns early only for depth-only shaders
(`spirv_shader_translator_rb.cc:4262-4266`).

The emitted body (`:4268-4480`) contains, for a shader that will never use
alpha-to-coverage:

- a store of full coverage to `gl_SampleMask[0]` (`:4274-4275`);
- a load of `kSystemConstantAlphaToMask` and a compare (`:4281-4288`);
- a four-way MSAA branch on the sample count (`:4177-4223`), each arm calling
  `FSI_AlphaToMaskSample` which recomputes the dithered threshold from
  `gl_FragCoord` (`:4130-4154`);
- **two nested `OpPhi` merges** over those arms (`:4243-4244`, `:4467-4468`);
- a discard when coverage ends up zero.

That is on the order of 100+ SPIR-V instructions and several live values, present
in *every* colour fragment shader on the performance path, executed never. The
compiler cannot sink it out of the shader: it is a dynamic branch on a uniform,
so it occupies registers and code space in the compiled variant.

**This is the only place where emulation scaffolding is plausibly a material
share of a heavy variant's instruction count, and it is ours.**

### 2.2 Per-target Function-scoped colour staging — live across the whole shader

`StartFragmentShaderInMain` creates one `float4` in `StorageClassFunction` per
written colour target (`spirv_shader_translator.cc:3405-3428`,
`xe_var_fragment_data_N`) plus a `uint` `xe_var_color_written` (`:3430-3434`),
purely so the epilogue can read the colour back for the alpha test before
copying to the Output variables (`spirv_shader_translator_rb.cc:1315-1332`).
These are live for the entire shader body. For a single-target shader that is 5
extra registers — on a 31-GPR variant at 4 waves, not nothing, but not the story.

## 3. The rest is the game's

Section 6 of the investigation already identified what the heavy passes *are*:

```
VkPassId: 1920x1376 <- color0 RT @ 0t, <24t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  960x2736 <- color0 RT @ 0t, <12t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  480x5472 <- color0 RT @ 0t,  <6t>, 1xMSAA, k_2_10_10_10_FLOAT
VkPassId:  240x8192 <- color0 RT @ 0t,  <3t>, 1xMSAA, k_2_10_10_10_FLOAT
```

and identifies the 9.04 ms pass as the game's HDR scene buffer, with the bloom
scissors being exact halvings (960x540 -> 480x270 -> 240x135), i.e. a 1920x1080
base. Section 7 then measured ~47 ALU ops/pixel and concluded they are "the
game's own bloom/glow shaders, not emulation scaffolding".

Nothing in sections 1-2 contradicts that. A bloom downsample/upsample chain is
exactly the shape of shader that lands at 1000-2200 instructions: several
dependent texture fetches, a wide filter footprint, and per-channel math. Our
additions to such a shader are the ~100 instructions of section 2.1 plus 5
registers of section 2.2.

## 4. Verdict

**The four heavy FS variants are overwhelmingly the game's own shading. There is
no meaningful emulation-scaffolding win in them.** The one lever that is
genuinely ours — the unconditionally-compiled alpha-to-coverage epilogue — is
worth *measuring*, not assuming, and section 10 trap 4 of the investigation is
directly against assuming it:

> SPIR-V opcode histograms are not a cost model on Adreno. A build with strictly
> fewer instructions measured *slower*. Total shader **size** tracked reality;
> instruction mix did not.

So removing ~100 never-executed instructions from a 2200-instruction shader is
exactly the change that trap says can measure slower. Do not build it on the
strength of the instruction count.

## 5. What would actually settle it (device required)

The blocker is a **join problem**, not an analysis problem: XenDroid logs guest
shader hashes, the instrumented Turnip logs per-variant instruction counts and
GPR counts, and nothing connects the two.

XenDroid side, already available today:

- `vulkan_pipeline_cache.cc:2826` `Pipeline created for VS {:016X}, PS {:016X}`
  — the guest ucode hash of every real pipeline, logged once.
- `vulkan_pipeline_cache.cc:2123` `Creating graphics pipeline state with VS ..., PS ...`
  — the same, for every creation attempt.
- `draw_util.h:852-895` `FormatDrawDebugMarker` puts `vs:%016llX ps:%016llX` in
  the per-draw debug marker, so with `gpu_debug_markers` the PS hash is on every
  draw that uses a given heavy shader.

Recipe, no code change:

1. `gpu_debug_markers = true` plus `log_gpu_frame_time_breakdown = true`. Get the
   `VkPassId` -> pass-bucket mapping (already logged once per bucket) and the
   per-draw PS hashes from the RenderDoc/debug-marker layer or from
   `LogRecentSubmissions` (`vulkan_command_processor.cc:6005`, which prints the
   last VS/PS hash per submission).
2. From the instrumented driver, take the `tu_variant` lines and note the four
   heaviest. On the *same* run, take the SPIR-V word count per PS from the
   per-title shader storage that `store_shaders` already writes
   (`cache/shaders/local/<TITLEID>.vk.bin`, `vulkan_pipeline_cache.cc:2943-2950`)
   and disassemble with `spirv-dis`.
3. Count, in the disassembly of a heavy variant, how many instructions sit
   between `StartFragmentShaderInMain` and the epilogue (the game's body) versus
   in the alpha-to-coverage block. `spirv-dis` output can be attributed by the
   `OpName`s the translator emits: everything in the epilogue is dominated by
   `gl_SampleMask` accesses and the two `OpPhi`s after them.
4. Only if the epilogue is a material share **and** a size-reducing A/B measures
   a win is there anything to build.

## 6. If it does turn out to matter: the cvar-gated design

Not implemented, deliberately — it is a shader-translator change with no
measurable justification yet, and the investigation's own trap 4 says the
instruction-count argument does not survive contact with Adreno.

The shape, following the repo's pattern (a `DEFINE_bool` with a `CATEGORY` and
help text explaining the tradeoff, default off, per-game escape hatch via
`GAME_COMPAT.md`):

```
DEFINE_bool(
    spirv_omit_unused_alpha_to_coverage, false,
    "Skip emitting the alpha-to-coverage gl_SampleMask computation in fragment "
    "shaders whose draws never enable alpha-to-coverage. On the host-render-
    "target ('performance') path it is currently compiled into every colour "
    "fragment shader behind a runtime uniform branch - roughly a hundred "
    "instructions and two OpPhi merges that never execute.\n"
    "OFF BY DEFAULT and needs a per-title measurement, not an instruction "
    "count: on Adreno a build with strictly fewer SPIR-V instructions has "
    "measured SLOWER (docs/gw-gpu-bottleneck-investigation.md section 10). Only "
    "turn this on for a title that (a) never sets RB_ALPHA_TEST_CONTRLO's "
    "alpha-to-coverage bit, verified over a full playthrough, and (b) measures "
    "faster with it on.",
    "GPU");
```

Implementation point: the decision cannot be made in the translator as written,
because `alpha_to_mask` is a *system constant* read at runtime
(`spirv_shader_translator_rb.cc:4281-4284`) and the same cached translation is
reused across draws with different values. Making it decidable at translation
time requires either folding the bit into
`SpirvShaderTranslator::Modification::PixelShaderModification`
(`spirv_shader_translator.h:103-136`) — which multiplies pipeline variants and
needs a `kVersion` bump per the comment at `:39-45` — or a per-title cvar that
asserts the title never uses it. The cvar is the honest version; the
modification bit is the correct one and is a much larger change.

Correctness risk of getting it wrong: alpha-to-coverage edges lose their
dithered coverage, which shows as speckled alpha-tested foliage and similar
stair-stepped alpha geometry. Not a crash, and visually obvious, which is why it
must stay cvar-gated with a per-title note.

## 7. Not verified

Nothing here was compiled or run: the authoring sandbox has no Android SDK/NDK,
no cmake, no Java/Gradle, no adb, no device and no GPU. No instruction count,
register count, occupancy figure or fps number in this document is measured by
me. Sections 5 and 6 are a recipe and a design, not a result.

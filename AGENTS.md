# AGENTS.md

Working notes for AI coding agents (and humans) changing this repository.

**ARMX360** is an Android (arm64-v8a) port of a **Xenia Edge** fork, itself a
fork of upstream XenDroid. Most of the C++ under
`emulator-core/src/main/cpp/xenia/src/xenia/` is upstream; the interesting work
is in the Vulkan command processor, the render-target cache, the Android
JNI/Compose frontend, and the measurement tooling in `tools/`.

### The one naming fact to get right

The app has **two names that deliberately do not match**, and conflating them is
the easiest way to break a build here:

| | Value | Where |
|---|---|---|
| Install identity | `armx360.compose` | `applicationId`, `app/build.gradle:38` |
| Java/Kotlin package | `xendroid.compose` | `namespace`, `app/build.gradle:22` |
| Debug install | `armx360.compose.debug` | `applicationIdSuffix '.debug'`, `app/build.gradle:103` |

`applicationId` and `namespace` are independent in AGP, which is what lets this
fork install **beside** upstream XenDroid instead of replacing it. Verified
2026-10-04 on a Pocket S / Android 13, where **three** packages coexist:
`xendroid.compose` (upstream release, pre-existing), `armx360.compose.debug`
(ours, debuggable) and `armx360.compose` (ours, release — confirmed
`debuggable=no` and `versionName=d74934e`, launched without a crash).

**Do not "tidy" the Java package to match the app name.** The JNI layer resolves
its classes by FQN *string* or by a hardcoded export symbol, so a package rename
is a rename of 13 string literals, and a single miss is a runtime-only failure no
local build catches. Verified 2026-10-04 against the shipped
`ARMX360_Release_d74934e.apk`:

- **12 distinct FQN literals across 13 `FindClass` sites.** `emulator_xendroid.cpp:1781`
  `FindClass("xendroid/compose/Emulator")` plus 12 more (`emulator.cpp:422,431,506`
  and `emulator_xendroid.cpp:606,665,1300,1376,1433,1520,1579,1633,1690`);
  `GameInfo` appears at both `:606` and `:665`, hence 13 sites but 12 literals.
  Check with `unzip -p <apk> lib/arm64-v8a/libe.so | strings | grep -c '^xendroid/'`
  — expect **12**.
- **1 export symbol, and it is in a different library than you would expect.**
  `Java_xendroid_hardware_ProcessorInfo_gpu_1get_1physical_1device_1name_1vk`
  at `hardware_ProcessorInfo.cpp:8` ships in **`libhardware_ProcessorInfo.so`**,
  not `libe.so` (`CMakeLists.txt:21` builds it as its own `SHARED` target).
  `libe.so` contains **zero** `Java_*` exports — `grep -c 'Java_'` over its
  `--dyn-syms` returns 0 — so a check that looks for this symbol in `libe.so`
  will always report a false failure. Check it with
  `readelf --dyn-syms -W lib/arm64-v8a/libhardware_ProcessorInfo.so | grep Java_xendroid`.

Note the file lives at `emulator-core/src/main/cpp/hardware_ProcessorInfo.cpp`,
directly under `cpp/`, **not** under `xenia/src/xenia/`.

Read this before your first change. The conventions below are not stylistic
preferences — several exist because the opposite was done and the result was a
wrong number published as fact.

---

## 1. The rule that matters most: never state an unverified result as fact

This repository's documentation is a ledger of measurements. The house style,
pushed hard by the maintainer across dozens of commits, is that **every claim is
either verified with a cited source, or explicitly labelled as unverified.**

Concretely:

- **Cite `file:line`** for any claim about existing code. Not "the render target
  cache does X" — `vulkan_render_target_cache.cc:3681`. Re-derive the line
  number yourself rather than copying one out of `docs/`: those files were
  written against older revisions, and a line reference that has drifted is
  worse than none because it looks checked.
- **Label inference as inference.** "Verified by reading" and "not verified —
  needs a device" are the two categories that matter. There is a third that is a
  bug: presenting an estimate as a measurement.
- **Never invent a number.** If you did not run it, do not print a plausible
  figure. Real examples of this being caught and fixed:
  - A commit claimed the vblank pacing fix stopped frame rate "sagging toward
    half"; the real overshoot table showed single-digit loss at realistic
    overshoot (`c6b0ed7de` corrected an earlier overstatement in `dc3b2c5a3`).
  - A shader-instruction-count argument was rejected because "a build with
    strictly fewer SPIR-V instructions measured *slower*" on Adreno
    (`docs/gw-gpu-bottleneck-investigation.md` §10, trap 4).
  - The `xe.log` fixtures carried `(9186 bytes on disk)` — a fabricated figure
    with nothing behind it. A real log says `15390`. Fixed in `ffca6110f`;
    `tools/bench-ab-test.sh:79` now carries the coupling comment.
- **Record refutations, not just findings.** `docs/a830-gmem-msaa-plan.md` has a
  "Refuted — do not revisit" section precisely so the same dead end is not
  re-investigated. Add to it when you kill a hypothesis.

**Corollary:** when you cannot verify something, say so in the commit body and
in the doc. A commit that honestly says "this needs a device I don't have" is
worth more here than one that implies a result.

---

## 2. Secrets: verify before every commit

**Required on every commit**, no exceptions — including one-line typo fixes and
docs-only edits. It takes seconds, and the failure mode is unrecoverable (a
force-push cannot reliably un-publish a leaked secret).

The risk is real in this repo specifically: device storage paths and CI secret
references live in the same tree as the code, and a release build is signed from
a keystore that is a repository secret rather than a tracked file.

```bash
# 1. No key material on disk or tracked. Note aps3e.keystore/aps3e.jks are
#    ignored (.gitignore:19-20) but are LEGACY: nothing in the build or CI
#    references them, and app/build.gradle has no signingConfig block at all.
#    Keep the rules (they cost nothing and would catch a resurrected legacy
#    key) but do not read them as "the build uses this keystore" - it does not.
#    Release signing happens entirely in CI via apksigner, from the
#    ANDROID_KEYSTORE_BASE64 / KEY_ALIAS / KEYSTORE_PASSWORD / KEY_PASSWORD
#    repository secrets.
git ls-files | grep -iE '\.(jks|keystore|p12|pem|pk8)$|^\.env$|credentials'

# 2. No token/key/private-key shapes anywhere tracked.
git grep -nIE '(ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{10,}|AIza[0-9A-Za-z_-]{30,}|-----BEGIN [A-Z ]*PRIVATE KEY-----)'

# 3. The exact content you are about to publish — not the whole tree.
git diff --cached | grep -nEi '(api[_-]?key|secret|passwd|password|token|bearer|authorization|private[_-]?key|access[_-]?key|client[_-]?secret|credential)'

# 4. Machine-local config must not be tracked.
git check-ignore -v local.properties    # must resolve to .gitignore:7
```

**Before the first push of a branch** (this covers commits you did not author and
any history rewrite), scan what is actually being transmitted:

```bash
git diff origin/main..main
git diff origin/main..main | grep -nEi '(secret|token|password|api[_-]?key|private key)'
git diff --numstat origin/main..main | awk '$1=="-"||$2=="-"{print "BINARY: "$3}'
```

Expect false positives and check them rather than waving them off — `token = ...`
in Xenia C++ and `secrets.*` in CI YAML are references, not values. Known
**benign** history hits, so you do not re-chase them: FFmpeg
`libavformat/tls_openssl.c` contains an error-message *string literal* with
`BEGIN PRIVATE KEY`, and mbedtls ships deliberately-public test keys
(`ec_prv.pk8.pem`). Both are upstream, behind submodules, never in a push.

**Also scan for personal data, not just credentials.** A device serial, a home
path, or a username is a leak even though no scanner flags it. Refer to hardware
as `Pocket S / Android 13 / Adreno 740` — model, OS and GPU reproduce a result,
and they are what the docs already use. If you catch yourself adding an
identifier (ADB serial, `u0_aNNN` uid, a real `/home/<user>/` path), leave it out.

Do not commit a real `xe.log`, an APK, or any file pulled off a device. Logs are
reconstructed as small synthetic fixtures under `tools/testdata/bench-ab/`.

---

## 3. Build and test

Requires JDK 21, Android SDK 35, NDK `29.0.14206865`, CMake `3.30.3`, and the
SPIR-V tools (`glslangValidator`, `spirv-opt`, `spirv-dis`). Full detail and
Windows setup: **BUILD.md**.

```bash
./gradlew :app:testDebugUnitTest     # the gate; runs first in CI
./gradlew :app:assembleDebug         # ~8-9 min cold (ThinLTO on release)
tools/bench-ab-test.sh               # offline; 36 checks, no device needed
```

- `:app:testDebugUnitTest`, **not** `testReleaseUnitTest`. Release has
  `minifyEnabled` + `shrinkResources`; R8 only rewrites the APK, so a release
  test run exercises the same 14 classes with none of R8's risk while paying for
  the slow link. See `.github/workflows/ARMX360.yml:217`.
- Tests run **before** the APK build in CI so a red test is reported as a test
  failure rather than "build failed". Keep that ordering.
- `tools/bench-ab-test.sh` needs no device and no JDK. **Run it after touching
  `tools/` or the fixtures** — it is a checker that fails if an assertion stops
  firing. If you change a fixture, the suite's coupled expectation must change
  too; editing one alone fails a check (intentional, and it works).
- Do not add a blocking lint gate without a checked-in baseline; lint is
  advisory on purpose (`continue-on-error: true`, `.github/workflows/ARMX360.yml:261`).

The SPIR-V tools are a **hard configure-time failure**, not a warning: the
`foreach(_tool glslangValidator spirv-opt spirv-dis)` at `CMakeLists.txt:64`
raises `FATAL_ERROR` at `CMakeLists.txt:68` naming the missing tool, because
`gen_android_spirv.py` (called at `CMakeLists.txt:91`) compiles the GPU/UI
shaders to SPIR-V during configure. A clean machine without them fails in ~25s
with a message that names the fix; do not go looking for a C++ error when you
see it.

There **is** a local container path: `Dockerfile` in the repo root.
`.github/workflows/ARMX360.yml` runs in `ghcr.io/cirruslabs/android-sdk:35`
(`.github/workflows/ARMX360.yml:17-18`) and the Dockerfile is based on the same
image, so "works in CI" and "works locally" are the same toolchain:

```bash
docker build -t armx360-build .          # ~6 GB; NDK is most of it
docker run --rm -v "$PWD":/src -v armx360-gradle-cache:/root/.gradle \
    armx360-build ./gradlew :app:assembleDebug
```

Bind-mount the source rather than COPYing it — the tree is >2 GB with submodules.
The image installs only what the base lacks (NDK, CMake, `glslang-tools`,
`spirv-tools`, ninja, python3) and then **verifies the toolchain at image-build
time**, so a broken image fails in seconds rather than four minutes into a
native compile. Do not go looking for a C++ error when configure-time SPIR-V
validation fires.

Verified 2026-10-04: image builds; `:app:testDebugUnitTest` in the container
gives **89 tests, 0 failures**; `:emulator-core:configureCMakeRelWithDebInfo`
succeeds and generates **226** shader bytecode headers.

### Verified figures

Measured, not estimated — re-measure before quoting them:

- `:app:testDebugUnitTest` → **89 tests, 0 failures**, across **14** classes.
- `:app:assembleDebug` → **6m02s** cold (native tree compiled from scratch, no
  ccache). The "~8-9 min" figure quoted elsewhere refers to a release link with
  ThinLTO, which is a different build.

### Testing on your own device needs no secrets at all

`app/build.gradle` has **no `signingConfig` block**, and release signing happens
entirely in CI via `apksigner`. So local testing never touches a keystore:

```bash
./gradlew :app:installDebug      # signed automatically with AGP's debug keystore
```

Two things to know:

- The debug variant has `applicationIdSuffix '.debug'` (`app/build.gradle:103`),
  so it installs **alongside** a release build as `armx360.compose.debug` with
  no signature conflict. That is the variant to use for measurement: it is
  `run-as`-capable and carries the newer instrumentation (trap 9).
- **Grant All Files Access before the first launch**, or
  `EmulatorHostActivity.kt:179-185` will `finish()` immediately on the cached
  `Environment.isExternalStorageManager()` value.

For a minified (`minifyEnabled` + `shrinkResources`, `app/build.gradle:106-111`)
release-variant APK on your own device, `assembleRelease` produces an **unsigned**
APK; sign it with a throwaway key of your own via
`$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner`. It will not upgrade an existing
maintainer-signed install without an uninstall first, so debug is usually the
better choice for testing.

CI-side: `Detect signing capability` sets `HAS_KEYSTORE`, and the build/sign/
release steps are gated on it, so a fork without the secrets still runs the unit
tests and lint and simply skips producing a signed release.

---

## 3a. The release signing key

The key lives **outside this repository** and never enters it. This section
records *where* and *how*; it deliberately contains no password or key material.

| Item | Value |
|---|---|
| Keystore | `~/.armx360-signing/armx360-release.jks` (PKCS12, outside the repo) |
| Alias | `armx360` |
| Owner | `CN=ARMX360, OU=Release Signing, O=ARMX360, L=-, ST=-, C=FI` |
| **Cert SHA-256** | `F0:2C:4F:E2:5A:0C:75:9D:63:B4:95:0C:8C:AD:B3:37:3E:78:F0:7A:CF:A8:B8:3D:E1:0F:AB:A9:F4:F8:A7:CC` |
| Valid | 2026-10-04 → 2054-02-19 |

**The cert SHA-256 is the identity.** It is published in every signed APK, so
`apksigner verify --print-certs <apk>` is how you confirm an APK really came
from this project. Never treat a keystore *file* hash as the identity:
re-encrypting or re-saving the file rewrites the bytes.

GitHub secrets are **write-only**. If `KEYSTORE_PASSWORD` is lost, CI cannot
read it back — the keystore file plus the password is the whole backup, and
losing both means a new certificate, which Android will refuse to upgrade over
(`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). Users would have to uninstall first.
Keep an encrypted copy of the keystore somewhere off this machine.

### Rotating the password

Rotating the **password** (not the key) is safe and invisible to users: the
certificate is unchanged, so installed builds keep upgrading. Verified
2026-10-04 — the released `d74934e` cert digest was identical before and after.

```bash
cp ~/.armx360-signing/armx360-release.jks{,.pre-rotate.bak}
printf '%s\n%s\n' "$NEWPW" "$NEWPW" | keytool -storepasswd \
    -keystore ~/.armx360-signing/armx360-release.jks -storepass "$OLD"
```

Then re-set `KEYSTORE_PASSWORD` **and** `KEY_PASSWORD`, and note that
`ANDROID_KEYSTORE_BASE64` must be refreshed from the new file bytes even though
the certificate inside did not change. Confirm afterwards with
`apksigner verify --print-certs`, not with `keytool -list` alone.

Two traps, both hit on 2026-10-04:

- **Rehearse on a copy.** An empty or wrong password makes `keytool` re-prompt
  and exit *leaving the keystore unchanged* — a silent no-op that reads as
  success. `openssl` is not installed on this machine; generate with
  `python3 -c "import secrets; print(secrets.token_urlsafe(32))"`.
- **Never redact a secret with `sed 's/=.*/.../'`.** It assumes the value
  follows an `=`. The keystore password contains `=` itself, so the substitution
  fired mid-password and printed 23 of 40 characters into a chat transcript.
  That forced this rotation. Redact by matching the known key and replacing only
  the remainder (`sed -E 's/^(password[^:]*:).*/\1 <REDACTED>/'`), or better,
  never print the line at all.
- **`gh secret set` takes exactly one positional arg.** The value goes through
  stdin or `-b`, never as a second argument, so
  `gh secret set KEY_ALIAS armx360` fails with *"accepts at most 1 arg(s),
  received 2"*. This is worse than a plain typo: it is **partial**. On
  2026-10-04 the `ANDROID_KEYSTORE_BASE64` line happened to succeed while both
  password updates failed, leaving the rotated keystore paired with the
  *pre-rotation* passwords — a state where every release sign fails. Always
  confirm afterwards with `gh secret list` and check the **updated timestamps**;
  a secret still showing its original date was never written.
- **A green CI run is not proof the secrets are current.** A run that started
  before a rotation uses whatever was set at that moment. Check the run's
  `createdAt` against the time the secret changed before believing it.

---

## 4. Commit and PR conventions

- **Subject: `[Area] Imperative summary, describing the effect.`**
  Areas in use: `GPU`, `CPU`, `APU`, `Vulkan`, `Kernel`, `Android`, `XConfig`,
  `Build`, `CI`, `Docs`, `Settings`, `Harness`. The summary says what changed for
  the reader, not which function was edited — *"Stop the GPU thread polling
  500x/s on the library screen"*, not *"Add throttle in graphics_system.cc"*.
- **Body explains why, and what was NOT verified.** Most commits here are
  several paragraphs and explicitly separate measured from unmeasured. If a
  priority or perf claim is unmeasured, say so in the body.
- **One logical change per commit.** Docs, code and fixtures for the same fix
  belong together; two unrelated fixes do not. If you catch yourself making an
  empty commit or bolting a second topic onto one via `--amend`, reset and redo
  it.
- **Default to a cvar-gated, off-by-default change** for anything touching the
  render path. Give the cvar a `CATEGORY` and help text stating the tradeoff and
  the measurement needed to justify turning it on. See `e1878e23f` (32bpp host
  format) and `6964dfb0a` (absolute-deadline pacing).
- **Default per-title escape hatches** to the per-game config mechanism in
  **GAME_COMPAT.md**, not the global config.
- No agent attribution trailers are used in this history; do not add them.

---

## 5. C++ and Kotlin conventions

- **C++** follows upstream Xenia: 2-space indent, `DEFINE_bool`/`DEFINE_int`
  with a `CATEGORY` last argument, `XELOGI` for logging, comments that explain
  *why* rather than restating the code. Match surrounding style over personal
  preference.
- **Kotlin/Compose** frontend is `app/src/main/java/xendroid/compose/`.
  Settings are declared once in `SettingsSchema.kt` with a matching entry in
  `SettingDescriptions.kt`.
- **A settings default must match what the binary actually runs.** The effective
  default is the bundled template's value if
  `emulator-core/src/main/assets/config/default_config.toml` ships the key, else
  the compiled-in `DEFINE_bool`. If they disagree the UI shows a toggle state
  the emulator is not in, and `isModified()` badges it wrongly.
  `SettingsSchemaTest.bool_defaults_match_effective_native_default` now enforces
  this for every comparable Bool — a new cvar-backed setting is covered
  automatically. Do not "fix" a divergence by editing the C++ default unless you
  intend a behaviour change; that changes what a stock install runs.

Adding a setting does **not** update itself, though: the inventory counts in
`SettingsSchemaTest` (`total_entry_count_is_138`,
`counts_by_type_match_verified_inventory`) are hand-maintained literals that went
stale once already. If you add a setting and see those two fail, the fix is to
re-derive the counts — do not delete the assertions to make the build green.

---
## 6. Measurement discipline

Performance work here is easy to get wrong and hard to notice. The traps are
encoded as machine-checked preconditions in `tools/bench-ab.sh`; the reasoning
is in **docs/benchmark-harness.md** and `docs/gw-gpu-bottleneck-investigation.md`
§10. Read both before any A/B.

The ones that have actually cost someone a wrong conclusion:

1. **Run twice, keep the second.** The first run after a new build recompiles
   pipelines and reads low. Always.
2. **Prove the cvar applied** before believing any A/B. Two "results" were
   configs that had never changed.
3. **Per-game config files vanish.** Re-verify before *and* after every run.
4. **SPIR-V opcode histograms are not a cost model on Adreno.** Fewer
   instructions measured *slower*. Shader *size* tracked reality.
5. **Generated bytecode headers are untracked and survive `git checkout`.** Wipe
   `emulator-core/src/main/cpp/xenia/src/xenia/gpu/shaders/bytecode/` before a
   bisect build; require `0 skipped/failed`.
6. **Counter selectors are generation-specific.** A renumbered selector reports
   plausible numbers under the wrong name — worse than no data.
7. **Device readings carry ~±2fps.** Do not over-read a 13-vs-15 difference. A
   neutral commit was once reverted on that basis.
8. **The driver is a free variable.** Record and assert which build ran.
9. **A cvar the binary does not have fails silently.** `config.cc:338-345`
   resolves per-game keys only against pre-registered `ConfigVars`; an unknown
   key is dropped with no warning and no log line. Verified: the pass-count
   instrumentation exists **only in the debug build**, so pointing it at a
   release package yields a log with no pass data and no complaint. Grep the APK
   before trusting a run:
   `unzip -p <apk> lib/arm64-v8a/libe.so | strings | grep -c '^<cvar>$'`
10. **Never take an fps number from a run with `log_gpu_frame_time_breakdown`
    on.** It issues `vkCmdCopyQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)`
    every submission and stalls the queue. It is a ratio to read, not a frame
    time to believe. Likewise never use the instrumented Turnip build for fps
    claims — locating only.

Exit codes from the harness are meaningful: `1` = an assertion failed and **the
measurement is void**; `3` = **refused to report** (cold cache, instrumented
driver, or too few samples). "No result" must stay distinguishable from "failed
result". Never report a number the harness refused.

### Device safety

The attached device may be the user's real one.

- Ask before installing/uninstalling APKs, clearing app data, changing global
  settings, or revoking app permissions.
- Restore anything you change: `appops set <pkg> MANAGE_EXTERNAL_STORAGE default`,
  delete created per-game configs, `settings delete global
  stay_on_while_plugged_in`, force-stop what you started.
- Prefer the **debug** package (`armx360.compose.debug`) for measurement — it is
  `run-as`-capable and usually carries newer instrumentation. It needs All Files
  Access granted **before** launch: `EmulatorHostActivity.kt:179-185` checks
  `Environment.isExternalStorageManager()` at runtime, so a process started
  before the grant will `finish()` on the cached value.
- An unattended device can have a persistent `NotificationShade` holding
  `mCurrentFocus`; the activity then stays paused with `isOnScreen=false`, no
  surface is created, `bootOnce()` never runs and **no frame renders**. If a run
  produces a log with zero `VkPassTime` lines, check window focus before
  suspecting the emulator.

---

## 7. Where things live

| Path | What |
|---|---|
| `app/src/main/java/xendroid/compose/` | Kotlin/Compose frontend (package is `xendroid.compose` — see above) |
| `app/src/test/java/xendroid/compose/` | 14 JVM unit test classes, 89 tests |
| `emulator-core/src/main/cpp/xenia/src/xenia/` | Xenia C++ (mostly upstream) |
| `.../gpu/vulkan/` | command processor, render-target cache — where GPU work lands |
| `emulator-core/src/main/assets/config/default_config.toml` | bundled cvar template |
| `tools/bench-ab.sh` | on-device A/B harness |
| `tools/bench-ab-test.sh` | its offline test suite |
| `tools/testdata/bench-ab/` | synthetic `xe.log` fixtures |
| `design/` | launcher icon master artwork + the script that installs it |
| `docs/*.md` | investigation ledgers; read before touching GPU perf |
| `Dockerfile` | local build container, same base image as CI |
| `BUILD.md` / `GAME_COMPAT.md` | toolchain; per-game config mechanism |

The `mipmap-*` launcher resources are **generated output**. Edit
`design/armx360_icon_512.png` and re-run `python3 design/install_icon.py`;
afterwards `git status` should show no change under `app/src/main/res/`, which
is the check that the tracked artwork is really the icon that shipped. The
master must stay outside `app/src/main/assets/` or it gets bundled into the APK.

Three things that break silently if edited on one side only:

- **Release tag prefix.** `tag_name: ARMX360-<sha>` in
  `.github/workflows/ARMX360.yml` and `RELEASE_TAG_PREFIX` in
  `app/src/main/java/xendroid/compose/updater/updater.kt`. A mismatch does not
  error — it compares `ARMX360-<sha>` against a bare sha and reports a phantom
  update on every launch.
- **Updater release repo.** `BuildConfig.RELEASE_REPO` comes from
  `-Parmx360.releaseRepo` (`app/build.gradle:72`); CI sets it from
  `github.repository`. Left unset it defaults to upstream XenDroid, so a fork
  will silently offer upstream's APKs as its own updates.
- **Workflow name.** `cache-cleanup.yml` triggers `workflow_run` on the workflow
  **name**, not the filename, so renaming `ARMX360.yml` without updating its
  `name:` leaves cache pruning permanently unsatisfied and silent.

Note `config.cc` is at `xenia/src/xenia/config.cc`. There is **no**
`xenia-base/` directory in this tree, despite what some older docs and comments
imply.

---

## 8. Scope discipline

- Do not reformat or "tidy" upstream Xenia code. Diff noise in vendored files
  buries the actual change and complicates rebases onto Edge.
- Do not add a fallback, shim, or compat path "just in case" without evidence
  that the case occurs.
- Prefer a cvar-gated, off-by-default change you can A/B over an unconditional
  one, and prefer recording a negative result over shipping an unmeasured
  optimisation.
- If a task turns out to be blocked (missing device, missing toolchain, wrong
  hardware generation), **report it as blocked with the reason**. Do not
  substitute a different device or a simulated result — an a830 question
  answered on an Adreno 740 is worse than no answer.

  - Keep JVM tests free of device/JNI dependencies. `SettingsSchemaTest` reads
  repo sources directly, which is how it stays honest without a cvar list.

---

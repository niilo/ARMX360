#!/usr/bin/env bash
# Offline test suite for tools/bench-ab.sh.
#
# WHY A TEST SUITE. bench-ab.sh exists to catch runs that silently did not change
# anything. A checker that never fails is worse than no checker, because it
# manufactures false confidence -- and this project's history shows exactly that
# failure mode twice ("two 'results' were configs that had never changed"). So
# every assertion is exercised against a fixture engineered to BREAK it, and this
# suite fails if the assertion does not fire.
#
# The fixtures under tools/testdata/bench-ab/ reproduce xe.log's byte-shape from
# the source: the "<type>> f:<frame:07> <thread:08X> " prefix from
# base/logging.cc, the CONFIG DUMP as a single record with bare continuation lines
# (config.cc:97/119), and the per-game block of config.cc:335/350/354. They are
# hand-written from the source, NOT captured from a device -- so if the
# emulator's log format changes, these must be updated alongside the assertions.
#
# Usage: tools/bench-ab-test.sh      (no device, adb or network required)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/bench-ab.sh"
FIX="$HERE/testdata/bench-ab"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PASSED=0; FAILEDN=0
ok()  { printf '  \033[32mok\033[0m   %s\n' "$1"; PASSED=$((PASSED + 1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILEDN=$((FAILEDN + 1)); }

# expect_rc <want-rc> <label> <args...>
# For the negative tests the exit code is the whole point: a "must fail" case that
# returns 0 is a broken assertion, not a passing test.
expect_rc() {
  local want="$1" label="$2"; shift 2
  local out rc
  out="$("$BENCH" "$@" 2>&1)"; rc=$?
  if [ "$rc" -eq "$want" ]; then
    ok "$label (rc=$rc)"
  else
    bad "$label: expected rc=$want, got rc=$rc"
    printf '%s\n' "$out" | sed 's/^/         /'
  fi
}

# expect_output <substring> <label> <args...> -- rc ignored; text must appear.
expect_output() {
  local needle="$1" label="$2"; shift 2
  local out
  out="$("$BENCH" "$@" 2>&1)"
  case "$out" in
    *"$needle"*) ok "$label" ;;
    *) bad "$label: output did not contain '$needle'"
       printf '%s\n' "$out" | sed 's/^/         /';;
  esac
}

echo "== syntax =="
bash -n "$BENCH" && ok "bench-ab.sh parses" || bad "bench-ab.sh does not parse"

echo
echo "== fixtures present and non-empty =="
for f in good missing-cvar wrong-value no-game-config cold-cache instrumented driver-missing; do
  [ -s "$FIX/$f-xe.log" ] && ok "fixture $f-xe.log" || bad "fixture $f-xe.log missing/empty"
done

# The cvars the good-run assertions below depend on.
GOOD_CVARS=(--expect game.readback_resolve=none
            --expect game.log_gpu_frame_time_breakdown=true
            --expect game.render_area_dirty_extent=true)
GOOD_DRIVER=(--driver-path mainline-turnip-V31)

echo
echo "== positive: a clean warm run passes =="
expect_rc 0 "assert-log accepts the good log" \
  assert-log --log "$FIX/good-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "log.game_overrides=3" "override count parsed as 3" \
  assert-log --log "$FIX/good-xe.log" "${GOOD_CVARS[@]}"
# 15390 is the byte count observed in a real captured log
# (/sdcard/.../compose/xe.log on a Pocket S, title 4541096D, warm shareable cache).
# The fixture and this expectation are deliberately coupled: both must change together.
expect_output "cache.bytes=15390" "warm cache detected" \
  assert-log --log "$FIX/good-xe.log" "${GOOD_CVARS[@]}"
expect_output "assert.failures=0" "zero failures on the good log" \
  assert-log --log "$FIX/good-xe.log" "${GOOD_CVARS[@]}"

echo
echo "== NEGATIVE: each assertion must actually fire =="

# Trap 2, the headline case: a cvar that was never applied.
expect_rc 1 "missing per-game cvar FAILS the run" \
  assert-log --log "$FIX/missing-cvar-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "render_area_dirty_extent" "missing-cvar names the absent key" \
  assert-log --log "$FIX/missing-cvar-xe.log" "${GOOD_CVARS[@]}"

# Trap 2 again, subtler: the key IS there but with the global value.
expect_rc 1 "wrong per-game cvar VALUE fails" \
  assert-log --log "$FIX/wrong-value-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "but the run intended" "wrong-value explains the mismatch" \
  assert-log --log "$FIX/wrong-value-xe.log" "${GOOD_CVARS[@]}"

# Trap 3: the per-game config vanished, so no override was applied at all.
expect_rc 1 "absent per-game config FAILS" \
  assert-log --log "$FIX/no-game-config-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "no per-game config was applied" "absent-config explains itself" \
  assert-log --log "$FIX/no-game-config-xe.log" "${GOOD_CVARS[@]}"

# Trap 1: cold pipeline cache.
expect_rc 1 "cold pipeline cache FAILS" \
  assert-log --log "$FIX/cold-cache-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "cache.bytes=0" "cold cache is detected, not guessed" \
  assert-log --log "$FIX/cold-cache-xe.log" "${GOOD_CVARS[@]}"

# Trap 8: the driver is not the one that was pinned.
expect_rc 1 "driver mismatch against the pin FAILS" \
  assert-log --log "$FIX/instrumented-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "driver mismatch" "driver mismatch explains itself" \
  assert-log --log "$FIX/instrumented-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"

# Trap 8, quieter version: a configured path that does not exist, so the run
# silently fell back to the system driver even though the pin text matches.
expect_rc 1 "system-driver fallback FAILS even when the pin text matches" \
  assert-log --log "$FIX/driver-missing-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"
expect_output "fell back to the SYSTEM driver" "fallback is reported" \
  assert-log --log "$FIX/driver-missing-xe.log" "${GOOD_CVARS[@]}" "${GOOD_DRIVER[@]}"

echo
echo "== global (CONFIG DUMP) assertions =="
expect_rc 0 "global cvar assertion passes on the good log" \
  assert-log --log "$FIX/good-xe.log" --expect global.vulkan_mid_frame_submission_draws=1300
expect_rc 1 "global cvar wrong value FAILS" \
  assert-log --log "$FIX/good-xe.log" --expect global.vulkan_mid_frame_submission_draws=9999
expect_rc 1 "global cvar absent from the dump FAILS" \
  assert-log --log "$FIX/good-xe.log" --expect global.no_such_cvar_here=1

echo
echo "== instrumented-driver gate (section 3 / 12.5) =="
# The instrumented build must be DETECTED here...
expect_output "log.instrumented=yes" "instrumented fixture is detected" \
  assert-log --log "$FIX/instrumented-xe.log" "${GOOD_CVARS[@]}"
expect_output "log.instrumented=no" "production fixture is not flagged" \
  assert-log --log "$FIX/good-xe.log" "${GOOD_CVARS[@]}"

# ...and `report` must REFUSE to emit an fps number from it, even with a full,
# clean set of samples. Without --driver-path (so the driver assertion passes).
CSV="$WORK/inst.csv"
cat >"$CSV" <<'CSVEOF'
label,mean_ms,fps,n_intervals
inst,45.0,22.2,600
inst,44.8,22.3,600
inst,45.1,22.2,600
CSVEOF
expect_rc 3 "report REFUSES fps from the instrumented driver" \
  report --log "$FIX/instrumented-xe.log" --label inst "$CSV"

# Same samples, same good log: now it must be allowed through.
CSV2="$WORK/good.csv"
cat >"$CSV2" <<'CSVEOF'
label,mean_ms,fps,n_intervals
warm,45.0,22.2,600
warm,44.8,22.3,600
warm,45.1,22.2,600
CSVEOF
expect_rc 0 "report accepts the same samples from a warm production run" \
  report --log "$FIX/good-xe.log" --label warm "$CSV2"

# Cold cache must also block the report, whatever the samples say.
expect_rc 3 "report REFUSES fps from a cold-cache run" \
  report --log "$FIX/cold-cache-xe.log" --label warm "$CSV2"

# Too few samples: noise cannot be estimated, so refuse rather than guess.
CSV3="$WORK/one.csv"
printf 'label,mean_ms,fps,n_intervals\nwarm,45.0,22.2,600\n' >"$CSV3"
expect_rc 3 "report REFUSES a single sample" \
  report --log "$FIX/good-xe.log" --label warm "$CSV3"

echo
echo "== compare: a delta inside noise must not be called real =="
# Two arms, sd ~2fps, means 1fps apart -> WITHIN_NOISE (trap 7).
cat >"$WORK/a.csv" <<'CSVEOF'
label,mean_ms,fps,n_intervals
A,45.0,22.0,600
A,44.0,22.7,600
A,46.0,21.4,600
CSVEOF
cat >"$WORK/b.csv" <<'CSVEOF'
label,mean_ms,fps,n_intervals
B,44.5,22.5,600
B,45.5,22.0,600
B,44.0,22.7,600
CSVEOF
"$BENCH" report --log "$FIX/good-xe.log" --label A "$WORK/a.csv" >/dev/null 2>&1
"$BENCH" report --log "$FIX/good-xe.log" --label B "$WORK/b.csv" >/dev/null 2>&1
expect_output "verdict=WITHIN_NOISE" "1fps gap on ~2fps noise is noise, not a win" \
  compare "$WORK/a.csv.report" "$WORK/b.csv.report"

# A large, low-variance gap must be called REAL.
cat >"$WORK/c.csv" <<'CSVEOF'
label,mean_ms,fps,n_intervals
B,33.3,30.0,600
B,33.4,29.9,600
B,33.3,30.0,600
CSVEOF
"$BENCH" report --log "$FIX/good-xe.log" --label B "$WORK/c.csv" >/dev/null 2>&1
expect_output "verdict=REAL" "22fps -> 30fps with tiny variance is REAL" \
  compare "$WORK/a.csv.report" "$WORK/c.csv.report"

# Fewer than 3 samples must be refused outright by compare too.
cat >"$WORK/one.csv" <<'CSVEOF'
label,mean_ms,fps,n_intervals
A,45.0,22.0,600
CSVEOF
"$BENCH" report --log "$FIX/good-xe.log" --label A "$WORK/one.csv" >/dev/null 2>&1
expect_rc 3 "compare REFUSES a 1-sample arm" \
  compare "$WORK/one.csv.report" "$WORK/c.csv.report"

echo
if [ "$FAILEDN" -eq 0 ]; then
  printf '\033[32mall %d checks passed\033[0m\n' "$PASSED"
  exit 0
fi
printf '\033[31m%d of %d checks FAILED\033[0m\n' "$FAILEDN" "$((PASSED + FAILEDN))"
exit 1
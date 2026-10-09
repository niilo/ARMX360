# Captured device logs — local working material, never committed

This directory holds logs pulled off the attached device so that a measurement
session survives `/tmp` being cleared and so a later session can re-derive a
finding without re-running anything.

**Nothing here is tracked by git, and nothing here should be.** See `AGENTS.md`
§2: never commit a real `xe.log`, an APK, or any file pulled off a device. These
carry title data and they are large. `.gitignore` keeps the whole directory out,
including the `.txt` logcat captures that the global `*.log` rule would miss.

## Layout

```
logs/
  session-YYYY-MM-DD/     one directory per working session
    <slug>-<n>.log        xe.log pulls (or per-boot slices)
    <slug>-<n>-logcat.txt logcat-current.txt pulls
```

`session-2026-10-09/` holds the first session's captures. The `a1-*` files are the
A1b affinity A/B arms; `gears2`–`gears6` are successive pulls of **one continuous
Gears boot** (see the "Applied N game config override(s)" tell below); `rel-*` are
from the release-build playthrough.

## Conventions

**Name a slice after what it is, not when it was pulled.** `gears5-boot.log` is
readable in a month; `230214.log` is not.

**Pull early and pull often.** Only 4 session zips are retained on device
(`shelved_log_sessions = 4`) and `logcat-current.txt` holds only the current
session. A zip written at session end is not written at all if the process is
killed, so a live `xe.log` can be the only copy that exists.

```sh
adb pull /sdcard/Android/data/armx360.compose/files/compose/xe.log \
    logs/session-$(date +%F)/<slug>.log
adb pull /sdcard/Android/data/armx360.compose/files/compose/logs/logcat-current.txt \
    logs/session-$(date +%F)/<slug>-logcat.txt
```

**Slice a multi-boot log before analysing it.** `xe.log` accumulates across
launches, so a frame count taken over the whole file is meaningless. Split on
`Opening Android window` and analyse one boot:

```sh
L=$(grep -n "Opening Android window" <log> | tail -1 | cut -d: -f1)
awk -v s="$L" 'NR>=s' <log> > <log stem>-boot.log
```

**Verify a slice is the boot you think it is.** Two boots are distinguishable by
the config-override line, which repeats once per launch and is the only reliable
signal that a cvar actually applied:

```sh
grep -c "Applied .* game config override" <log>   # one per boot
grep -m1 "Applied .* game config override" <log> | grep -oE 'f:[0-9]+ [0-9A-F]+'
```

**`logcat` may be dominated by one message.** The `InputEventSender: Received
'finished' signal for unknown seq number` flood is ~99.7% of a logcat from a
running emulator (122/s, from the 8 ms `SDL_PumpEvents` loop at
`sdl_input_driver.cc:130-133`). Count it before reading anything else:

```sh
grep -ac "Received 'finished' signal" <logcat>     # vs wc -l
```

That flood is why finding the performance-mode change in a logcat is hard, and
why the device-side capacity read was needed instead.

## Traps that cost real numbers here

- **`grep -n` without `-E`** silently matches nothing for `(dis)?`-style patterns.
- **`sort -n` eats leading zeros**, so `f:0022163` sorts as 22163 — strip leading
  zeros before sorting, or `printf '%d'`.
- **Read the log *tail* before naming a failure mode.** A log ending in a clean
  `Saved ... VkPipelineCache` shutdown at `uptime_ms=3180` is not a hang; it is a
  3.2-second cold start.
- **`f:0` does not mean "did not present"** — see `AGENTS.md` §6.
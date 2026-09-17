# Measurements

Numbers from real runs, kept so they can be compared build to build.

**The point of this directory is memory.** Benchmark builds and measurement
envs come and go; `pizero_bench`, `pizero_wavmeas` and `pizero_wdtest` were
each written for one question and are dead weight afterwards. Their results
are not. A benchmark may be deleted; its numbers belong here first, with
enough context that a later run can be compared against them honestly.

## Layout

```
docs/measurements/
  README.md          this file
  YYYY-MM.jsonl      one record per run, appended
  logs/              raw serial logs, the evidence behind the records
```

A record is a summary. The log is the evidence, and it is worth keeping even
when the summary looks fine, because a better analyser can re-read an old log
and cannot re-read a lost serial session.

## Recording a run

```sh
scripts/soak.py ports                                   # find the board
scripts/soak.py record /dev/cu.usbmodem2101 --env pizero_stream_60
scripts/soak.py record /dev/cu.usbmodem2101 --hours 25  # stops on its own
```

The logger timestamps every line, survives reboots, disconnects and frozen
ports, and notes when the board goes silent while the port stays open, which
is the freeze signature. It flushes line by line, so killing it is safe.

**Kill any other reader of the port first.** Two readers on one port produce
"device reports readiness to read but returned no data", which reads exactly
like a board fault and has cost a session before now.

Start from a **cold power-on** for stability runs: a dev-tool reboot can leave
`freezes=1` residue from a previous session (PIZERO-33).

## Turning a log into a record

```sh
scripts/soak.py analyse docs/measurements/logs/2026-09-17-1000-pizero_stream_60.log \
    --env pizero_stream_60 --note "cold boot, keyboard attached, AUTORUN disk"
scripts/soak.py analyse <log> --dry-run      # print, archive nothing
```

Analysis is separate from recording on purpose: it can be re-run later, and a
log recorded by any other means (screen, minicom, the Fruit Jam logger) can be
fed to it as long as lines carry a `[HH:MM:SS.mmm]` prefix. Without timestamps
it falls back to counting telemetry lines, which are ~1 Hz.

## What a record holds

```json
{
  "kind": "soak",              // soak | bench | selftest
  "date": "2026-09-17",
  "env": "pizero_stream_60",   // the PlatformIO env that was flashed
  "git": "2a959e7",            // repo HEAD when analysed
  "git_dirty": false,          // true means the firmware may not match that commit
  "log": "docs/measurements/logs/...",
  "notes": "cold boot, keyboard attached",
  "metrics": { ... }
}
```

`metrics` is grouped by the reliability goal it serves, so a record answers
the goals directly rather than needing interpretation:

| Group | Answers |
|---|---|
| `goal_1_uptime_60hz` | fps min/median/max/p99, windows outside 59.5-60.5, percentage in band |
| `goal_2_continuous_sound` | audio ring underruns and skips, totals and per hour, ring fill |
| `goal_3_no_sync_drops` | short windows per hour, silent gaps and the longest one |
| `goal_4_no_crashing` | freezes, freezes per hour, the phase each froze in, reboots seen |
| `frame_budget_us` | cpu, render, blit and audio phase times (PIZERO-88) |
| `usb` | devices attached and removed, HID report failures |

Goal 5, no data loss, is **not** measured here. Nothing writes to the card
yet, so the goal is currently vacuous; PIZERO-113 covers it and lands with the
write path in PIZERO-64/65.

### Counters that reset

`freezes=`, `skips=`, `under=` and `rfail=` are cumulative on the board and
return to zero when it reboots. The analyser sums increments across resets, so
a 25-hour total stays a total even if the board restarted twice in the middle.
`reboots_seen` counts the banner, so a run with reboots is never silently read
as a clean one.

## Adding a measurement that is not a soak

Bench and self-test runs use the same file and the same shape: capture the
board's output to a log, then `analyse --kind bench`. If a future benchmark
prints something this analyser does not understand, extend `parse_log()` in
`scripts/soak.py` rather than inventing a second format.

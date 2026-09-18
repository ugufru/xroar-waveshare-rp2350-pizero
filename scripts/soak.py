#!/usr/bin/env python3
"""Soak harness for the RP2350-PiZero XRoar port (PIZERO-108).

Two jobs, deliberately separable:

  record   read the board's serial output for hours, timestamp every line,
           survive reboots and disconnects, and write a raw log.
  analyse  turn a raw log into the numbers the reliability goals are stated
           in, and append one record to the measurement archive.

They are separate because a log is evidence and can be re-read later with a
better analyser, while a serial session cannot be replayed. Always keep the
log; the archive record is a summary of it, not a replacement.

    scripts/soak.py ports
    scripts/soak.py record /dev/cu.usbmodem2101 --env pizero_stream_60
    scripts/soak.py analyse docs/measurements/logs/2026-09-17-soak.log \
        --env pizero_stream_60

The goals this serves (PIZERO-98, and the five stated reliability goals):
continuous 60 Hz, continuous sound, no sync drops, no crashing, no data loss.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import statistics
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(REPO, "docs", "measurements")
LOGDIR = os.path.join(ARCHIVE, "logs")

# The board prints ~1 Hz telemetry; see src/main.cpp:1268 and :1288.
RE_TS = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d\d\d)\]\s?(.*)$")
RE_RUN = re.compile(
    r"\[run\] fps=(\d+) cpu=(\d+)us render=(\d+)us blit=(\d+)us aud=(\d+)us"
    r".*?\bls=(\S+) conn=(\d+).*?\brpts=(\d+) rfail=(\d+) eperr=(\d+)"
    r".*?\bfreezes=(\d+) last=(\S+)"
)
# Two shapes, because logs recorded before PIZERO-118 are still worth reading.
# Old: cumulative counters only, which cannot tell silence from a shortfall.
RE_AUD_OLD = re.compile(r"\[aud\] ring fill=(\d+) skips=(\d+) under=(\d+)")
# New: rates, plus what the source actually produced and how much was audible.
RE_AUD = re.compile(
    r"\[aud\] fill=(\d+) prod=(\d+)/s want=(\d+)/s(?: srv=(\d+))? under=(\d+)/s "
    r"short=([\d.]+)% tone=(\d+)/s skips=(\d+) under_total=(\d+)"
)
RE_FREEZE = re.compile(
    r"\[watchdog\] \*\*\* FREEZE RECOVERED \*\*\*.*?in '([^']+)'"
    r".*?~(\d+) frames \(~(\d+)s\)"
)
RE_BOOT = re.compile(r"XRoar on RP2350-PiZero")
RE_SILENT = re.compile(r"=== SILENT for (\d+)s")
RE_USB_ADD = re.compile(r"\[usb\] device attached")
RE_USB_DEL = re.compile(r"\[usb\] device removed")

# 60 Hz, and how far off it may drift before a second counts as a bad window.
FPS_TARGET = 60.0
FPS_BAND = 0.5


def now_hms() -> str:
    return dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def git_state() -> tuple[str, bool]:
    def run(*args: str) -> str:
        return subprocess.run(
            args, cwd=REPO, capture_output=True, text=True, check=False
        ).stdout.strip()

    return run("git", "rev-parse", "--short", "HEAD"), bool(
        run("git", "status", "--porcelain")
    )


# --------------------------------------------------------------------------
# record


def cmd_ports(_args: argparse.Namespace) -> int:
    try:
        from serial.tools import list_ports
    except ImportError:
        print("pyserial not installed: pip3 install pyserial", file=sys.stderr)
        return 2
    found = list(list_ports.comports())
    if not found:
        print("no serial ports found (is the board plugged in?)")
    for p in found:
        print(f"{p.device}\t{p.description}")
    return 0


def cmd_record(args: argparse.Namespace) -> int:
    try:
        import serial
    except ImportError:
        print("pyserial not installed: pip3 install pyserial", file=sys.stderr)
        return 2

    os.makedirs(LOGDIR, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y-%m-%d-%H%M")
    path = args.log or os.path.join(LOGDIR, f"{stamp}-{args.env}.log")
    deadline = time.monotonic() + args.hours * 3600 if args.hours else None

    print(f"logging {args.device} at {args.baud} -> {path}")
    print("stop with ctrl-c; the log is flushed line by line, so a kill is safe")
    if deadline:
        print(f"will stop on its own after {args.hours} h")

    with open(path, "a", buffering=1) as f:
        def mark(msg: str) -> None:
            f.write(f"[{now_hms()}] === {msg} ===\n")

        git, dirty = git_state()
        mark(f"LOGGER START dev={args.device} baud={args.baud} env={args.env} "
             f"git={git}{'+dirty' if dirty else ''}")
        last_rx: float | None = None
        try:
            while True:
                if deadline and time.monotonic() > deadline:
                    mark("DURATION REACHED")
                    break
                try:
                    sp = serial.Serial(args.device, args.baud, timeout=1)
                    mark("PORT OPENED")
                    buf = b""
                    while True:
                        if deadline and time.monotonic() > deadline:
                            mark("DURATION REACHED")
                            break
                        chunk = sp.read(256)
                        if chunk:
                            buf += chunk
                            while b"\n" in buf:
                                line, buf = buf.split(b"\n", 1)
                                text = line.decode("utf-8", "replace").rstrip("\r")
                                f.write(f"[{now_hms()}] {text}\n")
                                last_rx = time.monotonic()
                        elif last_rx is not None and time.monotonic() - last_rx > 10:
                            # Port open but the board has gone quiet: that is
                            # the freeze signature worth recording.
                            mark(f"SILENT for {int(time.monotonic() - last_rx)}s")
                            last_rx = time.monotonic()
                    break
                except KeyboardInterrupt:
                    raise
                except Exception as e:  # port vanished, board rebooting, termios
                    mark(f"PORT ERROR: {type(e).__name__}: {e}")
                    time.sleep(2)
        except KeyboardInterrupt:
            mark("LOGGER STOPPED (ctrl-c)")
            print("\nstopped")

    print(f"log: {path}")
    print(f"now: scripts/soak.py analyse {path} --env {args.env}")
    return 0


# --------------------------------------------------------------------------
# analyse


class Counter:
    """A cumulative board-side counter that resets to zero on reboot.

    Two traps this exists to avoid:

    * The board may already be running when logging starts, so the first
      value is history, not something this run caused. It becomes the
      baseline and is reported separately. Without this a leftover
      ``freezes=1`` from an earlier session is read as a freeze during the
      run, which is exactly what happened on 2026-09-17.
    * The counter returns to zero when the board reboots, so increments are
      summed across resets and a 25-hour total stays a total.
    """

    def __init__(self) -> None:
        self.total = 0          # increments observed during this log
        self.baseline = 0       # value already on the board when logging began
        self.last: int | None = None

    def see(self, value: int) -> None:
        if self.last is None:
            self.baseline = value
            self.last = value
            return
        if value < self.last:  # counter went backwards: the board restarted
            self.total += value
        else:
            self.total += value - self.last
        self.last = value

    def reset_for_reboot(self) -> None:
        self.last = 0 if self.last is not None else None


def parse_log(path: str) -> dict:
    fps: list[int] = []
    cpu: list[int] = []
    render: list[int] = []
    blit: list[int] = []
    aud: list[int] = []
    ring_fill: list[int] = []
    short_pct: list[float] = []      # % of the nominal rate that never arrived
    servo_rate: list[int] = []       # PIZERO-121 servo's working rate
    tone_windows = 0                 # windows in which anything audible was produced
    aud_windows = 0                  # windows carrying the post-PIZERO-118 line
    freezes = Counter()
    skips = Counter()
    under = Counter()
    rfail = Counter()
    freeze_phases: list[str] = []
    freezes_before_log: list[str] = []   # reported at boot, but predating this log
    session_start: str | None = None     # first telemetry timestamp of this boot
    session_last: str | None = None      # most recent telemetry timestamp
    silent: list[int] = []
    gaps: list[float] = []               # host-side losses: sleep, suspend, a busy Mac
    last_ts: str | None = None
    reboots = 0
    usb_add = usb_del = 0
    first_ts = last_ts = None
    run_lines = 0

    with open(path, "r", errors="replace") as f:
        for raw in f:
            m = RE_TS.match(raw.rstrip("\n"))
            ts, body = (m.group(1), m.group(2)) if m else (None, raw.strip())
            if ts:
                first_ts = first_ts or ts
                # A gap means WE stopped listening, not that the board stopped:
                # a sleeping laptop loses telemetry the board still emitted.
                # Rates must be per hour OBSERVED, or they are understated.
                if last_ts:
                    fmt = "%H:%M:%S.%f"
                    d = (dt.datetime.strptime(ts, fmt)
                         - dt.datetime.strptime(last_ts, fmt)).total_seconds()
                    if d < 0:
                        d += 86400
                    if d > 5.0:
                        gaps.append(d)
                last_ts = ts

            if RE_BOOT.search(body):
                reboots += 1
                session_start = None
                session_last = None
                for c in (freezes, skips, under, rfail):
                    c.reset_for_reboot()
                continue

            m = RE_RUN.search(body)
            if m:
                run_lines += 1
                if session_start is None:
                    session_start = ts
                session_last = ts
                fps.append(int(m.group(1)))
                cpu.append(int(m.group(2)))
                render.append(int(m.group(3)))
                blit.append(int(m.group(4)))
                aud.append(int(m.group(5)))
                rfail.see(int(m.group(9)))
                freezes.see(int(m.group(11)))
                continue

            m = RE_AUD.search(body)
            if m:
                aud_windows += 1
                ring_fill.append(int(m.group(1)))
                if m.group(4):
                    servo_rate.append(int(m.group(4)))   # PIZERO-121
                short_pct.append(float(m.group(6)))
                if int(m.group(7)):
                    tone_windows += 1
                skips.see(int(m.group(8)))
                under.see(int(m.group(9)))
                continue

            m = RE_AUD_OLD.search(body)
            if m:
                ring_fill.append(int(m.group(1)))
                skips.see(int(m.group(2)))
                under.see(int(m.group(3)))
                continue

            m = RE_FREEZE.search(body)
            if m:
                # The watchdog reports a freeze AFTER rebooting, so the message
                # describes the session that just died. If that session had been
                # up longer than we have been watching, the freeze happened
                # before this log started and is not ours to count: exactly what
                # a board power-cycled mid-soak reports at its next boot.
                # WHOSE FREEZE IS IT? The watchdog reports after rebooting, so
                # the message describes the session that just died. That
                # session is ours if we saw it START, i.e. if this is not the
                # first banner in the log. Only the first one can describe a
                # session that began before we attached.
                #
                # This replaces an uptime comparison that looked reasonable and
                # was wrong twice over: the message's seconds were computed
                # with the wrong frame rate, and the value was compared against
                # a session that had just started rather than the one that
                # died. It silently reported three real freezes as somebody
                # else's.
                # WHOSE FREEZE IS IT? The watchdog reports after rebooting, so
                # a message describes the session that just died. Only the
                # FIRST banner in a log can describe a session that began
                # before we attached, and the board's scratch can also carry a
                # stale freeze across a power cycle, so that one is flagged as
                # ambiguous rather than guessed at. Every later one is ours.
                #
                # An earlier attempt compared the message's own uptime figure
                # instead. That was wrong twice: the firmware computed those
                # seconds with the wrong frame rate, and the comparison was
                # against the session just starting rather than the one that
                # died. It reported three real freezes as somebody else's.
                if reboots <= 1:
                    freezes_before_log.append(m.group(1))
                    freezes.reset_for_reboot()   # do not count it as ours
                    freezes.last = None          # next value is a fresh baseline
                else:
                    freeze_phases.append(m.group(1))
                continue

            m = RE_SILENT.search(body)
            if m:
                silent.append(int(m.group(1)))
                continue

            if RE_USB_ADD.search(body):
                usb_add += 1
            elif RE_USB_DEL.search(body):
                usb_del += 1

    def span_seconds() -> float:
        if not (first_ts and last_ts):
            return float(run_lines)  # ~1 Hz telemetry: lines are a fair proxy
        fmt = "%H:%M:%S.%f"
        a = dt.datetime.strptime(first_ts, fmt)
        b = dt.datetime.strptime(last_ts, fmt)
        secs = (b - a).total_seconds()
        return secs + 86400 if secs < 0 else secs  # ran past midnight

    duration = span_seconds()
    lost = sum(gaps)
    observed = max(duration - lost, 1e-9)
    hours = observed / 3600      # per-hour rates are per hour WATCHED
    lo, hi = FPS_TARGET - FPS_BAND, FPS_TARGET + FPS_BAND
    bad = [v for v in fps if not lo <= v <= hi]

    def stats(xs: list[int]) -> dict:
        if not xs:
            return {}
        return {
            "min": min(xs),
            "median": round(statistics.median(xs), 1),
            "max": max(xs),
            "p99": sorted(xs)[max(0, int(len(xs) * 0.99) - 1)],
        }

    return {
        "duration_s": round(duration, 1),
        "duration_h": round(duration / 3600, 2),
        "observed_h": round(observed / 3600, 2),
        "lost_h": round(lost / 3600, 2),
        "gaps": len(gaps),
        "windows": len(fps),
        "run_lines": run_lines,
        "goal_1_uptime_60hz": {
            "fps": stats(fps),
            "windows_out_of_band": len(bad),
            "in_band_pct": round(100.0 * (len(fps) - len(bad)) / len(fps), 3)
            if fps
            else None,
            "band": [lo, hi],
        },
        "goal_2_continuous_sound": {
            "underruns": under.total,
            "skips": skips.total,
            "underruns_per_hour": round(under.total / hours, 2),
            "ring_fill": stats(ring_fill),
            # PIZERO-118. Without these an underrun count is unreadable: a
            # silent machine and a broken stream produce the same number.
            "shortfall_pct": {
                "median": round(statistics.median(short_pct), 2),
                "max": max(short_pct),
            }
            if short_pct
            else None,
            "windows_with_shortfall": sum(1 for v in short_pct if v > 0.0),
            "windows_with_audio": tone_windows,
            "windows_measured": aud_windows,
            "telemetry": "rates" if aud_windows else "cumulative-only (pre-PIZERO-118)",
            "servo_rate": stats(servo_rate) if servo_rate else None,
        },
        "goal_3_no_sync_drops": {
            # A dropout shows up as a short window; the count is the honest
            # proxy until a sink-side measurement exists (PIZERO-99).
            "short_windows_per_hour": round(len(bad) / hours, 2),
            "silent_gaps": len(silent),
            "longest_silence_s": max(silent) if silent else 0,
        },
        "goal_4_no_crashing": {
            # Count the watchdog's own messages, not the board counter: the
            # counter resets on reboot and carries across power cycles, so
            # arithmetic on it double-counts. One message is one freeze.
            "freezes": len(freeze_phases),
            "freezes_per_hour": round(len(freeze_phases) / hours, 3),
            # Cross-check, and a LOWER BOUND on what the messages missed: a
            # freeze inside a host-side gap leaves no message but still moves
            # the board's counter.
            "freeze_counter_delta": freezes.total,
            "freezes_before_run": freezes.baseline,   # already on the counter
            "freezes_ambiguous_first_boot": freezes_before_log,
            "freeze_phases": freeze_phases,
            "reboots_seen": reboots,
        },
        "frame_budget_us": {
            "cpu": stats(cpu),
            "render": stats(render),
            "blit": stats(blit),
            "audio": stats(aud),
        },
        "usb": {
            "attached": usb_add,
            "removed": usb_del,
            "report_failures": rfail.total,
        },
    }


def cmd_analyse(args: argparse.Namespace) -> int:
    if not os.path.exists(args.log):
        print(f"no such log: {args.log}", file=sys.stderr)
        return 2
    metrics = parse_log(args.log)
    git, dirty = git_state()
    record = {
        "kind": args.kind,
        "date": dt.datetime.now().strftime("%Y-%m-%d"),
        "env": args.env,
        "git": git,
        "git_dirty": dirty,
        "log": os.path.relpath(args.log, REPO),
        "notes": args.note or "",
        "metrics": metrics,
    }

    g = metrics
    print(f"duration      {g['duration_h']} h wall clock, {g['observed_h']} h observed"
          + (f" ({g['gaps']} gaps, {g['lost_h']} h lost: the HOST stopped listening,"
             f" the board did not stop running)" if g["gaps"] else "")
          + f" | {g['windows']} telemetry windows")
    fps = g["goal_1_uptime_60hz"]
    print(f"1 uptime      fps {fps['fps']} | in band {fps['in_band_pct']}% "
          f"| bad windows {fps['windows_out_of_band']}")
    snd = g["goal_2_continuous_sound"]
    if snd["shortfall_pct"]:
        print(f"2 sound       shortfall {snd['shortfall_pct']['median']}% median, "
              f"{snd['shortfall_pct']['max']}% worst | audible in "
              f"{snd['windows_with_audio']}/{snd['windows_measured']} windows | "
              f"skips {snd['skips']}")
    else:
        print(f"2 sound       underruns {snd['underruns']} ({snd['underruns_per_hour']}/h), "
              f"skips {snd['skips']} | {snd['telemetry']}: cannot tell silence "
              f"from a dropout")
    syn = g["goal_3_no_sync_drops"]
    print(f"3 sync        short windows {syn['short_windows_per_hour']}/h, "
          f"silences {syn['silent_gaps']} (longest {syn['longest_silence_s']}s)")
    crash = g["goal_4_no_crashing"]
    print(f"4 crashes     freezes {crash['freezes']} ({crash['freezes_per_hour']}/h), "
          f"reboots seen {crash['reboots_seen']}, phases {crash['freeze_phases'] or 'none'}"
          + (f" | counter already at {crash['freezes_before_run']} before this run"
             if crash["freezes_before_run"] else "")
          + (f" | board counter moved {crash['freeze_counter_delta']}"
             if crash["freeze_counter_delta"] != len(crash["freeze_phases"]) else "")
          + (f" | {len(crash['freezes_ambiguous_first_boot'])} at the first boot, "
             f"ambiguous: {crash['freezes_ambiguous_first_boot']}"
             if crash["freezes_ambiguous_first_boot"] else ""))
    print(f"frame budget  {g['frame_budget_us']}")
    print("5 data loss   not measured here: nothing writes to the card yet "
          "(PIZERO-113 covers it)")

    if args.dry_run:
        print("\n(dry run, nothing archived)")
        return 0

    os.makedirs(ARCHIVE, exist_ok=True)
    out = os.path.join(ARCHIVE, f"{record['date'][:7]}.jsonl")
    with open(out, "a") as f:
        f.write(json.dumps(record) + "\n")
    print(f"\narchived -> {os.path.relpath(out, REPO)}")
    if dirty:
        print("NOTE: working tree was dirty, so 'git' names a commit the "
              "firmware may not match")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ports", help="list serial ports")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("record", help="log the board's serial output")
    p.add_argument("device")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--env", default="pizero_stream_60")
    p.add_argument("--hours", type=float, default=0, help="stop after N hours")
    p.add_argument("--log", help="log path (default: docs/measurements/logs/...)")
    p.set_defaults(func=cmd_record)

    p = sub.add_parser("analyse", help="summarise a log and archive the result")
    p.add_argument("log")
    p.add_argument("--env", default="pizero_stream_60")
    p.add_argument("--kind", default="soak", choices=["soak", "bench", "selftest"])
    p.add_argument("--note", help="one line of context for the archive record")
    p.add_argument("--dry-run", action="store_true", help="print, do not archive")
    p.set_defaults(func=cmd_analyse)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

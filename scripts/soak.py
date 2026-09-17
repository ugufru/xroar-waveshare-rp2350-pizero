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
    r"\[aud\] fill=(\d+) prod=(\d+)/s want=(\d+)/s under=(\d+)/s short=([\d.]+)% "
    r"tone=(\d+)/s skips=(\d+) under_total=(\d+)"
)
RE_FREEZE = re.compile(r"\[watchdog\] \*\*\* FREEZE RECOVERED \*\*\*.*?in '([^']+)'")
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

    Sums the increments across reboots so a 25-hour total is still a total.
    """

    def __init__(self) -> None:
        self.total = 0
        self.last = 0

    def see(self, value: int) -> None:
        if value < self.last:  # counter went backwards: the board restarted
            self.total += value
        else:
            self.total += value - self.last
        self.last = value

    def reset_for_reboot(self) -> None:
        self.last = 0


def parse_log(path: str) -> dict:
    fps: list[int] = []
    cpu: list[int] = []
    render: list[int] = []
    blit: list[int] = []
    aud: list[int] = []
    ring_fill: list[int] = []
    short_pct: list[float] = []      # % of the nominal rate that never arrived
    tone_windows = 0                 # windows in which anything audible was produced
    aud_windows = 0                  # windows carrying the post-PIZERO-118 line
    freezes = Counter()
    skips = Counter()
    under = Counter()
    rfail = Counter()
    freeze_phases: list[str] = []
    silent: list[int] = []
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
                last_ts = ts

            if RE_BOOT.search(body):
                reboots += 1
                for c in (freezes, skips, under, rfail):
                    c.reset_for_reboot()
                continue

            m = RE_RUN.search(body)
            if m:
                run_lines += 1
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
                short_pct.append(float(m.group(5)))
                if int(m.group(6)):
                    tone_windows += 1
                skips.see(int(m.group(7)))
                under.see(int(m.group(8)))
                continue

            m = RE_AUD_OLD.search(body)
            if m:
                ring_fill.append(int(m.group(1)))
                skips.see(int(m.group(2)))
                under.see(int(m.group(3)))
                continue

            m = RE_FREEZE.search(body)
            if m:
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
    hours = duration / 3600 or 1e-9
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
        },
        "goal_3_no_sync_drops": {
            # A dropout shows up as a short window; the count is the honest
            # proxy until a sink-side measurement exists (PIZERO-99).
            "short_windows_per_hour": round(len(bad) / hours, 2),
            "silent_gaps": len(silent),
            "longest_silence_s": max(silent) if silent else 0,
        },
        "goal_4_no_crashing": {
            "freezes": freezes.total,
            "freezes_per_hour": round(freezes.total / hours, 3),
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
    print(f"duration      {g['duration_h']} h ({g['windows']} telemetry windows)")
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
          f"reboots seen {crash['reboots_seen']}, phases {crash['freeze_phases'] or 'none'}")
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

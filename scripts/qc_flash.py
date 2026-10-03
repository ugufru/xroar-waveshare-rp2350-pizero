#!/usr/bin/env python3
"""qc_flash.py: flash a batch of boards and keep a QC log (PIZERO-189).

For each board, in order:
  1. You hold BOOT, plug the board's power USB-C port into this Mac, let go.
  2. The firmware is flashed with picotool and verified.
  3. The board restarts into it; its serial number (the USB serial, the same
     one the F1 INFO page shows) is read, and its boot log is checked for the
     display and the USB host coming up.
  4. You put the inspection sticker under the camera; a photo is taken, the
     sticker's number is read from it (tesseract), and you confirm or type it.
  5. A row goes into qc/qc-log.csv, with the photo and boot log beside it.

Usage, from the repo root, after building the release firmware:
  pio run -e pizero_stream_60
  python3 scripts/qc_flash.py

Options: --camera NAME (default "Achtung Mini Camera"), --uf2 PATH,
--allow-dirty (accept a firmware built from uncommitted changes).
"""

import argparse
import csv
import datetime
import hashlib
import os
import re
import subprocess
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is needed: pip3 install pyserial")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QC = os.path.join(ROOT, "qc")
LOG = os.path.join(QC, "qc-log.csv")
PICOTOOL = os.path.expanduser("~/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool")
RPI_VID = 0x2E8A
FIELDS = ["unit", "time", "board_serial", "sticker_sn", "firmware", "firmware_sha256",
          "flash", "boot", "photo", "boot_log", "notes"]


def say(msg=""):
    print(msg, flush=True)


def ask(prompt):
    try:
        return input(prompt)
    except EOFError:
        sys.exit("\nstopped")


def firmware_version():
    path = os.path.join(ROOT, "src", "fw_version.h")
    try:
        m = re.search(r'#define FW_VERSION "([^"]*)"', open(path).read())
        return m.group(1) if m else "unknown"
    except OSError:
        return "unknown"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def read_log():
    if not os.path.exists(LOG):
        return []
    with open(LOG, newline="") as f:
        return list(csv.DictReader(f))


def append_log(row):
    new = not os.path.exists(LOG)
    with open(LOG, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


def our_ports():
    return [p for p in serial.tools.list_ports.comports() if p.vid == RPI_VID]


def bootsel_present():
    r = subprocess.run([PICOTOOL, "info"], capture_output=True, text=True)
    return r.returncode == 0 and "No accessible" not in (r.stdout + r.stderr)


def wait_for_bootsel(timeout=60):
    """Wait for a board in BOOTSEL. A board already running firmware is sent
    there with the 1200-baud touch, as `pio run -t upload` does."""
    t0 = time.time()
    touched = False
    while time.time() - t0 < timeout:
        if bootsel_present():
            return True
        ports = our_ports()
        if ports and not touched:
            say("  A board is running firmware; asking it to restart into BOOTSEL...")
            try:
                serial.Serial(ports[0].device, 1200).close()
            except Exception:
                pass
            touched = True
        time.sleep(0.5)
    return False


def flash(uf2):
    r = subprocess.run([PICOTOOL, "load", "-v", "-x", uf2], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    return r.returncode == 0, out


def wait_for_port(timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        ports = our_ports()
        if ports:
            return ports[0]
        time.sleep(0.25)
    return None


def capture_boot(port, seconds=8):
    """Read the boot log; pass when the display and the USB host came up."""
    lines = []
    t0 = time.time()
    s = None
    while s is None and time.time() - t0 < 5:
        try:
            s = serial.Serial(port, 115200, timeout=0.5)
        except Exception:
            time.sleep(0.2)
    if s is None:
        return False, ["could not open " + port]
    buf = b""
    while time.time() - t0 < seconds:
        buf += s.read(4096)
    s.close()
    lines = buf.decode(errors="replace").splitlines()
    text = "\n".join(lines)
    ok = "DVI up" in text and "USB host up" in text
    return ok, lines


def take_photo(camera, path):
    # About two seconds of frames first, so focus and exposure settle.
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "avfoundation",
                        "-framerate", "30", "-i", camera, "-vf", "select=gte(n\\,60)",
                        "-frames:v", "1", "-y", path], capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(path)


def read_sticker(path):
    """Candidate serial numbers from the photo: runs of 6 or more letters and
    digits that contain a digit, longest first."""
    try:
        r = subprocess.run(["tesseract", path, "-", "--psm", "11"], capture_output=True, text=True)
    except FileNotFoundError:
        return []
    found = re.findall(r"[A-Za-z0-9]{6,}", r.stdout)
    found = [t for t in found if re.search(r"\d", t)]
    seen, out = set(), []
    for t in sorted(found, key=len, reverse=True):
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", default="Achtung Mini Camera")
    ap.add_argument("--uf2", default=os.path.join(ROOT, ".pio", "build", "pizero_stream_60", "firmware.uf2"))
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args()

    if not os.path.exists(a.uf2):
        sys.exit("No firmware at %s. Build it first: pio run -e pizero_stream_60" % a.uf2)
    if not os.path.exists(PICOTOOL):
        sys.exit("picotool not found at %s (it comes with PlatformIO)" % PICOTOOL)
    version = firmware_version()
    if version.endswith("-dirty") and not a.allow_dirty:
        sys.exit("The firmware (%s) was built from uncommitted changes. Commit, rebuild, "
                 "or pass --allow-dirty." % version)
    digest = sha256(a.uf2)
    os.makedirs(os.path.join(QC, "photos"), exist_ok=True)
    os.makedirs(os.path.join(QC, "logs"), exist_ok=True)

    say("Firmware %s  (sha256 %s...)" % (version, digest[:16]))
    say("Camera   %s" % a.camera)
    say("Log      %s" % os.path.relpath(LOG, ROOT))
    say()

    while True:
        rows = read_log()
        # Next after the highest, so a row removed from the log never lets
        # a unit number (or its photo and log file names) be used twice.
        unit = max([int(r["unit"]) for r in rows if r.get("unit", "").isdigit()] or [0]) + 1
        known = {r["board_serial"]: r["unit"] for r in rows if r.get("board_serial")}
        say("=== Unit %d ===" % unit)
        cmd = ask("Hold BOOT, plug the board's power USB-C port into this Mac, let go of BOOT,\n"
                  "then press Enter (q to finish): ").strip().lower()
        if cmd == "q":
            break

        say("  Waiting for the board in BOOTSEL...")
        if not wait_for_bootsel():
            say("  No board in BOOTSEL. Check the cable (a data cable, the power port) and try again.\n")
            continue
        say("  Flashing...")
        ok_flash, out = flash(a.uf2)
        if not ok_flash:
            say("  FLASH FAILED:\n" + out + "\n  Not logged. Try the board again.\n")
            continue
        say("  Flashed and verified. Waiting for it to start...")
        port = wait_for_port()
        if port is None:
            say("  The board did not come back as a USB serial device. Not logged.\n")
            continue
        board_serial = port.serial_number or "unknown"
        say("  Board serial: %s" % board_serial)
        if board_serial in known:
            say("  NOTE: this board is already in the log as unit %s." % known[board_serial])
        ok_boot, lines = capture_boot(port.device)
        say("  Boot check: %s" % ("PASS (display and USB host up)" if ok_boot else "FAIL"))
        stem = "%02d-%s" % (unit, board_serial)
        log_path = os.path.join(QC, "logs", stem + ".txt")
        with open(log_path, "w") as f:
            f.write("\n".join(lines) + "\n")

        photo = os.path.join(QC, "photos", stem + ".jpg")
        sticker = ""
        while True:
            ask("  Put the inspection sticker under the camera, then press Enter: ")
            if not take_photo(a.camera, photo):
                say("  The camera did not take a photo. Check that it is connected.")
                if ask("  Try again? [Y/n] ").strip().lower() == "n":
                    photo = ""
                    break
                continue
            subprocess.run(["open", photo])
            cands = read_sticker(photo)
            guess = cands[0] if cands else ""
            if cands:
                say("  Read from the photo: %s" % ", ".join(cands[:4]))
            ans = ask("  Sticker number%s (r to retake): " % (" [%s]" % guess if guess else "")).strip()
            if ans.lower() == "r":
                continue
            sticker = ans or guess
            break
        notes = ask("  Notes (Enter for none): ").strip()

        append_log({
            "unit": unit,
            "time": datetime.datetime.now().isoformat(timespec="seconds"),
            "board_serial": board_serial,
            "sticker_sn": sticker,
            "firmware": version,
            "firmware_sha256": digest,
            "flash": "ok",
            "boot": "pass" if ok_boot else "FAIL",
            "photo": os.path.relpath(photo, ROOT) if photo else "",
            "boot_log": os.path.relpath(log_path, ROOT),
            "notes": notes,
        })
        say("  Logged unit %d: board %s, sticker %s, boot %s. Unplug it.\n"
            % (unit, board_serial, sticker or "(none)", "pass" if ok_boot else "FAIL"))

    rows = read_log()
    say("\n%d unit(s) in %s." % (len(rows), os.path.relpath(LOG, ROOT)))


if __name__ == "__main__":
    main()

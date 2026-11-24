#!/usr/bin/env python3
"""release.py: build the CoCo Zero firmware as a .uf2 for a GitHub release (PIZERO-341).

Builds the product env (pizero_stream_60) from a clean tree at a tagged
commit, checks the .uf2, and writes it to dist/ with its SHA-256:

    dist/coco-zero-<tag>.uf2
    dist/coco-zero-<tag>.uf2.sha256
    dist/coco-zero-<tag>-notes.md     (the release notes)

With --publish it also creates the GitHub release with gh and attaches the
file. The tag must already be pushed (git push origin <tag>); the script
refuses otherwise, since gh would make its own tag on whatever main is.

Releases made this way are pre-releases: the current main, tagged, not yet
the vetted "safe" releases that are planned for later.

Usage:
    git tag -a v0.8.0 -m "..."            # on a clean, committed main
    python3 scripts/release.py            # build and check, tag taken from HEAD
    git push origin main v0.8.0           # ask first (CLAUDE.md)
    python3 scripts/release.py --publish  # build again and create the release

The board takes the file by drag-and-drop: hold BOOT while plugging the
power-port USB-C cable into the computer, and copy the .uf2 onto the RP2350
drive that appears. The product firmware ignores software requests to
restart into that mode, so the BOOT button is the way in.
"""

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = "pizero_stream_60"
UF2 = os.path.join(ROOT, ".pio", "build", ENV, "firmware.uf2")
DIST = os.path.join(ROOT, "dist")
REPO_URL = "https://github.com/ugufru/xroar-waveshare-rp2350-pizero"

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x2000
RP2350_ARM_S = 0xE48BFF59          # the family the RP2350 boot ROM takes for Arm code
FLASH_BASE, FLASH_SIZE = 0x10000000, 16 * 1024 * 1024


def git(*args):
    return subprocess.check_output(("git",) + args, cwd=ROOT, text=True).strip()


def fail(msg):
    sys.exit("release: " + msg)


def head_tag(wanted):
    tags = git("tag", "--points-at", "HEAD").split()
    if wanted:
        if wanted not in tags:
            fail("tag %s is not on HEAD (HEAD has: %s)" % (wanted, ", ".join(tags) or "none"))
        return wanted
    tags = [t for t in tags if re.match(r"^v\d+\.\d+", t)]
    if len(tags) != 1:
        fail("HEAD needs exactly one version tag (vN.N...), found: %s. Tag it, or pass --tag."
             % (", ".join(tags) or "none"))
    return tags[0]


def check_uf2(path):
    data = open(path, "rb").read()
    if not data or len(data) % 512:
        fail("%s is not a whole number of 512-byte UF2 blocks" % path)
    nblocks = len(data) // 512
    for i in range(nblocks):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, bno, total, family = struct.unpack("<8I", b[:32])
        (end,) = struct.unpack("<I", b[508:])
        if (m0, m1, end) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
            fail("block %d: bad UF2 magic" % i)
        if not flags & UF2_FLAG_FAMILY or family != RP2350_ARM_S:
            fail("block %d: family 0x%08x, not RP2350 Arm (0x%08x)" % (i, family, RP2350_ARM_S))
        if not FLASH_BASE <= addr < FLASH_BASE + FLASH_SIZE:
            fail("block %d: address 0x%08x is outside the 16 MB flash" % (i, addr))
        if (bno, total) != (i, nblocks):
            fail("block %d: numbered %d of %d" % (i, bno, total))
    return nblocks


def notes(tag, sha, nblocks):
    return """CoCo Zero firmware {tag}: the current main, built from the product env `{env}`.

This is a pre-release: the latest working firmware, not a vetted release.

## Install

1. Download `coco-zero-{tag}.uf2` below.
2. Hold the board's **BOOT** button while you plug its **power** USB-C port into your computer.
   A drive named **RP2350** appears.
3. Copy the `.uf2` file onto that drive. The board restarts into the CoCo Zero by itself.

The microSD card still needs your own ROMs in `/coco/roms/`: see the README's
[Quick start]({url}#quick-start). The INFO page (F12, then Left) shows the version, `{tag}`.

SHA-256: `{sha}` ({nblocks} UF2 blocks, RP2350 Arm).
""".format(tag=tag, env=ENV, url=REPO_URL, sha=sha, nblocks=nblocks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tag", help="the version tag on HEAD (default: the one there is)")
    ap.add_argument("--publish", action="store_true", help="create the GitHub release with gh")
    ap.add_argument("--no-clean", action="store_true", help="skip the clean rebuild")
    a = ap.parse_args()

    if git("status", "--porcelain", "--untracked-files=no"):
        fail("the working tree has uncommitted changes; a release is built from a commit")
    tag = head_tag(a.tag)

    if a.publish:
        if not git("ls-remote", "--tags", "origin", "refs/tags/" + tag):
            fail("tag %s is not on origin yet: git push origin main %s" % (tag, tag))
        if subprocess.run(["gh", "release", "view", tag], cwd=ROOT,
                          capture_output=True).returncode == 0:
            fail("a release for %s exists already" % tag)

    # A clean build: no stale objects, and fw_version.h regenerated from the tag.
    if not a.no_clean:
        subprocess.check_call(["pio", "run", "-e", ENV, "-t", "clean"], cwd=ROOT)
    subprocess.check_call(["pio", "run", "-e", ENV], cwd=ROOT)

    hdr = open(os.path.join(ROOT, "src", "fw_version.h")).read()
    m = re.search(r'#define FW_VERSION "([^"]*)"', hdr)
    if not m or m.group(1) != tag:
        fail("the firmware calls itself %r, not %s" % (m and m.group(1), tag))
    if 'FW_ENV "%s"' % ENV not in hdr:
        fail("fw_version.h names the wrong env")

    nblocks = check_uf2(UF2)
    os.makedirs(DIST, exist_ok=True)
    name = "coco-zero-%s.uf2" % tag
    out = os.path.join(DIST, name)
    shutil.copyfile(UF2, out)
    sha = hashlib.sha256(open(out, "rb").read()).hexdigest()
    with open(out + ".sha256", "w") as f:
        f.write("%s  %s\n" % (sha, name))
    notes_path = os.path.join(DIST, "coco-zero-%s-notes.md" % tag)
    with open(notes_path, "w") as f:
        f.write(notes(tag, sha, nblocks))
    print("release: %s, %d blocks, sha256 %s" % (os.path.relpath(out, ROOT), nblocks, sha))

    if a.publish:
        subprocess.check_call(["gh", "release", "create", tag, out, out + ".sha256",
                               "--verify-tag", "--prerelease",
                               "--title", "CoCo Zero " + tag,
                               "--notes-file", notes_path], cwd=ROOT)


if __name__ == "__main__":
    main()

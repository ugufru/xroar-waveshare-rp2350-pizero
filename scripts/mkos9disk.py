#!/usr/bin/env python3
"""Build a NitrOS-9 Level 1 boot floppy for the CoCo Zero (PIZERO-199).

The board's disk controller does 35-track, single-sided, 18-sector floppies
(161,280-byte .DSK files), which no NitrOS-9 release image is. This builds
one from a NitrOS-9 6809 Level 1 distribution image (any RBF image that
carries the distribution's NITROS9/6809L1 tree, CMDS and SYS, such as the
3.3.0 `nos9l1.dsk` on the CoCo SDC image), the way the distribution's own
SCRIPTS/mb.floppy does:

  * track 34: rel + krn + krnp2 + init + boot_1773_6ms, which Disk BASIC's
    DOS command loads and runs;
  * OS9Boot: BOOTLISTS/standard.bl with the 35-track single-sided
    descriptors (DD, D0, D1, D2) in place of its 40-track ones;
  * sysgo, Startup, SYS/errmsg, and CMDS less what does not fit or has no
    hardware here (see EXCLUDE).

Usage:
    scripts/mkos9disk.py SOURCE.dsk OUT.DSK [--startup FILE] [--name NAME]
    scripts/mkos9disk.py --list OUT.DSK          # print its tree

Only the host's Python is needed. The source image is opened read-only.
"""
import argparse
import datetime as dt
import os
import random
import sys

SECTOR = 256
TRACKS, SPT = 35, 18
TOTAL = TRACKS * SPT                     # 630 sectors, 161,280 bytes
BOOT_TRACK_LSN = 34 * SPT                # track 34, sectors 1-18
MOD = "NITROS9/6809L1/MODULES"

# Commands left off: too big for a 157 KB disk with room to spare, or for
# hardware the board does not have (CoCo SDC, DriveWire). They stay on the
# source image.
EXCLUDE = {"basic09", "runb", "asm", "disasm", "dcheck", "ded", "minted", "sdc", "sdcdriver",
           "boot_sdc", "dw", "tuneport", "megaread", "grfdrv"}

BOOT_TRACK = ["BOOTTRACK/rel", "KERNEL/krn", "KERNEL/krnp2", "SYSMODS/init", "BOOTTRACK/boot_1773_6ms"]
# BOOTLISTS/standard.bl, 35-track single-sided descriptors.
BOOT_FILE = ["SYSMODS/ioman", "RBF/rbf.mn", "RBF/rb1773.dr", "RBF/ddd0_35s.dd", "RBF/d0_35s.dd",
             "RBF/d1_35s.dd", "RBF/d2_35s.dd", "SCF/scf.mn", "SCF/vtio.dr", "SCF/covdg.io",
             "SCF/term_vdg.dt", "PIPE/pipeman.mn", "PIPE/piper.dr", "PIPE/pipe.dd",
             "CLOCKS/clock_60hz", "CLOCKS/clock2_soft", "SYSMODS/sysgo_dd"]


def be(b):
    return int.from_bytes(b, "big")


# --------------------------------------------------------------------------
# reading an RBF image

class RbfReader:
    def __init__(self, path):
        self.d = open(path, "rb").read()

    def fd(self, lsn):
        f = self.d[lsn * SECTOR:(lsn + 1) * SECTOR]
        segs = []
        for k in range(48):
            s = f[16 + k * 5:21 + k * 5]
            a, n = be(s[0:3]), be(s[3:5])
            if n == 0:
                break
            segs.append((a, n))
        return {"att": f[0], "dat": f[3:8], "size": be(f[9:13]), "creat": f[13:16], "segs": segs}

    def read(self, lsn):
        info = self.fd(lsn)
        out = b"".join(self.d[a * SECTOR:(a + n) * SECTOR] for a, n in info["segs"])
        return info, out[:info["size"]]

    def entries(self, lsn):
        _, data = self.read(lsn)
        out = {}
        for k in range(0, len(data), 32):
            e = data[k:k + 32]
            if e[0] == 0:
                continue
            nm = bytes(c & 0x7F for c in e[:29]).split(b"\0")[0].decode("latin1")
            if nm and nm not in (".", ".."):
                out[nm] = be(e[29:32])
        return out

    def lookup(self, path):
        lsn = be(self.d[8:11])
        for part in [p for p in path.split("/") if p]:
            e = {k.lower(): v for k, v in self.entries(lsn).items()}
            if part.lower() not in e:
                raise FileNotFoundError(path)
            lsn = e[part.lower()]
        return lsn

    def file(self, path):
        return self.read(self.lookup(path))


# --------------------------------------------------------------------------
# writing a 35-track single-sided RBF floppy

class Node:
    def __init__(self, name, data=None, att=0x1B, dat=None, creat=None):
        self.name, self.data, self.att = name, data, att
        self.children = [] if data is None else None
        self.dat, self.creat = dat, creat
        self.fd_lsn = None


class FloppyWriter:
    def __init__(self, name):
        self.img = bytearray(TOTAL * SECTOR)
        self.used = [False] * TOTAL
        self.name = name
        now = dt.datetime.now()
        self.now5 = bytes([now.year - 1900, now.month, now.day, now.hour, now.minute])
        self.now3 = self.now5[:3]
        for lsn in (0, 1):
            self.used[lsn] = True
        for lsn in range(BOOT_TRACK_LSN, TOTAL):
            self.used[lsn] = True

    def alloc(self, n):
        run = 0
        for lsn in range(TOTAL):
            run = run + 1 if not self.used[lsn] else 0
            if run == n:
                start = lsn - n + 1
                for k in range(start, start + n):
                    self.used[k] = True
                return start
        raise RuntimeError(f"disk full: no {n} free sectors in a row")

    def put(self, lsn, data):
        self.img[lsn * SECTOR:lsn * SECTOR + len(data)] = data

    def write_fd(self, node, size, segs):
        f = bytearray(SECTOR)
        f[0] = node.att
        f[3:8] = node.dat or self.now5
        f[8] = 1                                      # link count
        f[9:13] = size.to_bytes(4, "big")
        f[13:16] = node.creat or self.now3
        for k, (a, n) in enumerate(segs):
            f[16 + k * 5:19 + k * 5] = a.to_bytes(3, "big")
            f[19 + k * 5:21 + k * 5] = n.to_bytes(2, "big")
        self.put(node.fd_lsn, f)

    def lay_file(self, node, contiguous_with_fd=True):
        n = (len(node.data) + SECTOR - 1) // SECTOR
        node.fd_lsn = self.alloc(1)
        segs = []
        if n:
            start = self.alloc(n)
            self.put(start, node.data)
            segs = [(start, n)]
        self.write_fd(node, len(node.data), segs)
        return segs

    @staticmethod
    def dir_entry(name, lsn):
        e = bytearray(32)
        raw = name.encode("latin1")[:28]
        e[:len(raw)] = raw
        e[len(raw) - 1] |= 0x80                       # the last character's high bit ends the name
        e[29:32] = lsn.to_bytes(3, "big")
        return e

    def lay_dir(self, node, parent_fd):
        """Directory FD and data first, then the children (so they can name it)."""
        size = (len(node.children) + 2) * 32
        n = (size + SECTOR - 1) // SECTOR
        node.fd_lsn = node.fd_lsn if node.fd_lsn is not None else self.alloc(1)
        start = self.alloc(n)
        for child in node.children:
            if child.children is not None:
                self.lay_dir(child, node.fd_lsn)
            elif child.fd_lsn is None:
                self.lay_file(child)
        data = bytearray()
        data += self.dir_entry("..", parent_fd if parent_fd is not None else node.fd_lsn)
        data += self.dir_entry(".", node.fd_lsn)
        for child in node.children:
            data += self.dir_entry(child.name, child.fd_lsn)
        self.put(start, bytes(data))
        self.write_fd(node, size, [(start, n)])

    def finish(self, root, boot_node, boot_track, opts):
        # Root directory FD at LSN 2, as RBF format places it; then the boot
        # file, which must be one contiguous run named in LSN0.
        root.fd_lsn = self.alloc(1)
        self.used[root.fd_lsn] = True
        segs = self.lay_file(boot_node)
        self.lay_dir(root, None)
        # The boot track.
        if len(boot_track) > SPT * SECTOR:
            raise RuntimeError(f"boot track is {len(boot_track)} bytes, more than a track")
        self.put(BOOT_TRACK_LSN, boot_track)
        # LSN0.
        h = bytearray(SECTOR)
        h[0:3] = TOTAL.to_bytes(3, "big")
        h[3] = SPT
        mapbytes = (TOTAL + 7) // 8
        h[4:6] = mapbytes.to_bytes(2, "big")
        h[6:8] = (1).to_bytes(2, "big")               # sectors per cluster
        h[8:11] = root.fd_lsn.to_bytes(3, "big")
        h[13] = 0xFF
        h[14:16] = random.getrandbits(16).to_bytes(2, "big")
        h[16] = 0x02                                  # single sided, double density
        h[17:19] = SPT.to_bytes(2, "big")
        h[21:24] = segs[0][0].to_bytes(3, "big")
        h[24:26] = len(boot_node.data).to_bytes(2, "big")
        h[26:31] = self.now5
        nm = self.name.encode("latin1")[:31]
        h[31:31 + len(nm)] = nm
        h[31 + len(nm) - 1] |= 0x80
        h[63:63 + len(opts)] = opts
        self.put(0, h)
        # The allocation map: a set bit is a used sector, MSB first; the bits
        # past the last sector are set so nothing allocates them.
        bitmap = bytearray(SECTOR)
        for lsn in range(mapbytes * 8):
            if lsn >= TOTAL or self.used[lsn]:
                bitmap[lsn // 8] |= 0x80 >> (lsn % 8)
        self.put(1, bitmap)
        return bytes(self.img)


def build(src_path, out_path, startup=None, name="NitrOS-9 Level 1 Boot Disk"):
    src = RbfReader(src_path)

    def module(rel):
        return src.file(f"{MOD}/{rel}")[1]

    boot_track = b"".join(module(m) for m in BOOT_TRACK)
    boot = Node("OS9Boot", b"".join(module(m) for m in BOOT_FILE), att=0x03)
    root = Node("")
    root.att = 0xBF
    root.children.append(boot)

    cmds = Node("CMDS")
    cmds.att = 0xBF
    cmd_lsn = src.lookup("CMDS")
    for nm in sorted(src.entries(cmd_lsn), key=str.lower):
        if nm.lower() in EXCLUDE:
            continue
        info, data = src.read(src.entries(cmd_lsn)[nm])
        if info["att"] & 0x80:
            continue
        cmds.children.append(Node(nm, data, att=info["att"], dat=info["dat"], creat=info["creat"]))
    root.children.append(cmds)

    sys_dir = Node("SYS")
    sys_dir.att = 0xBF
    info, data = src.file("SYS/errmsg")
    sys_dir.children.append(Node("errmsg", data, att=info["att"], dat=info["dat"], creat=info["creat"]))
    root.children.append(sys_dir)

    info, data = src.file("Startup")
    if startup is not None:
        data = open(startup, "rb").read().replace(b"\r\n", b"\r").replace(b"\n", b"\r")
    root.children.append(Node("startup", data, att=0x1B))
    sysgo = module("SYSMODS/sysgo_dd")
    root.children.append(Node("sysgo", sysgo, att=0x2F))

    opts = module("RBF/d0_35s.dd")[0x12:0x12 + 15]       # the drive's path options
    w = FloppyWriter(name)
    img = w.finish(root, boot, boot_track, opts)
    with open(out_path, "wb") as f:
        f.write(img)
    free = w.used.count(False)
    print(f"{out_path}: {len(cmds.children)} commands, boot file {len(boot.data)} bytes, "
          f"boot track {len(boot_track)} bytes, {free} sectors ({free * SECTOR // 1024} KB) free")


def list_tree(path):
    r = RbfReader(path)
    d = r.d
    nm = bytes(c & 0x7F for c in d[31:63]).split(b"\0")[0].decode("latin1")
    print(f"{path}: {len(d)} bytes, {be(d[0:3])} sectors, name {nm!r}, boot LSN {be(d[21:24])} size {be(d[24:26])}")

    def walk(lsn, depth):
        for nm, child in sorted(r.entries(lsn).items(), key=lambda x: x[0].lower()):
            info = r.fd(child)
            print(f"{'  ' * depth}{nm}{'/' if info['att'] & 0x80 else ''}"
                  f"{'' if info['att'] & 0x80 else ' ' + str(info['size'])}")
            if info["att"] & 0x80:
                walk(child, depth + 1)
    walk(be(d[8:11]), 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", help="NitrOS-9 6809 Level 1 distribution image")
    ap.add_argument("out", nargs="?", help="the .DSK to write")
    ap.add_argument("--startup", help="a file to use as the disk's startup script")
    ap.add_argument("--name", default="NitrOS-9 Level 1 Boot Disk")
    ap.add_argument("--list", metavar="DSK", help="print a disk's tree and stop")
    a = ap.parse_args()
    if a.list:
        list_tree(a.list)
        return 0
    if not (a.source and a.out):
        ap.error("SOURCE and OUT are needed")
    build(a.source, a.out, a.startup, a.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

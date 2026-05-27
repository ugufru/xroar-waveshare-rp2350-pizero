# `autorun.txt` — Boot configuration for CoCo content

When the device boots, it mounts the microSD card and looks for files in
`/coco/`. If `/coco/autorun.txt` exists, it controls what happens after
the system ROMs load: which disk to mount, which cart to install, and
what gets typed into Disk BASIC (or whether to bypass BASIC entirely).

This document is the proposed spec. Implementation tracked in AMOLED-58.

---

## SD card layout

The loader accepts both a flat layout and an organized one:

```
/coco/
  roms/                      (optional — for organization)
    bas12.rom                ← Color BASIC          (REQUIRED, BYO)
    extbas11.rom             ← Extended Color BASIC (recommended, BYO)
    disk11.rom               ← Disk BASIC cart      (recommended, BYO)
    *.ccc / *.rom            ← cartridge ROMs (optional)
  dsk/                       (optional)
    *.dsk                    ← JVC disk images
  bin/                       (optional)
    *.bin                    ← DECB load-module binaries (direct-load)
  autorun.txt                ← optional, boot configuration (this spec)
  README.txt                 ← optional, ships with the device
```

Note the folder names: ROMs go in `roms/` (plural); disk images in
`dsk/` and direct-load binaries in `bin/` (singular).

The three system ROMs (`bas12.rom`, `extbas11.rom`, `disk11.rom`) are
non-redistributable Microsoft/Tandy property. They are **not** shipped
with the device — users must supply their own copies, dumped from
hardware they own.

Only `bas12.rom` is strictly required. Without `extbas11.rom` the device
boots plain Color BASIC (no Extended BASIC, and no Disk BASIC — the
`disk11.rom` cart is built on top of Extended). For the disk/autorun
flows described here, supply all three.

When the loader looks up a filename like `PARTCLES.BIN`, it tries:

1. The typed subdirectory first (e.g. `/coco/bin/PARTCLES.BIN`)
2. Then the flat location (`/coco/PARTCLES.BIN`)

So users who don't want subdirectories can just dump everything into
`/coco/` and it still works.

---

## File format

Plain text, UTF-8 / ASCII, one entry per line. CRLF and LF line endings
both accepted. Maximum 256 characters per line; maximum 4 KB total.

Three kinds of lines:

| Starts with    | Meaning                                                |
|----------------|--------------------------------------------------------|
| `#` or blank   | Comment / skipped                                      |
| `@`            | Directive — controls boot configuration                |
| anything else  | Autotype — typed verbatim into Disk BASIC after warmup |

Lines are processed in order. Directives can appear anywhere; they
take effect during boot setup (before BASIC starts typing).

---

## Directives

### `@DISK filename.dsk`
Mount this disk image as drive 0.

If absent: the loader mounts the first `.dsk` found in `/coco/dsk/`
or `/coco/` (alphabetical). If no disks exist, no disk is mounted.

### `@CART filename.rom` (or `.ccc`)
Install this cartridge ROM at `$C000`.

If absent: the loader installs `/coco/roms/disk11.rom` (or `/coco/disk11.rom`)
if found. If neither exists, the cart is empty (Color BASIC only — no
disk operations).

### `@DIRECT filename.bin`
**Bypass Disk BASIC entirely.** Load this DECB-format `.bin` file
directly into emulator RAM (the AMOLED-26 code path used for 64K
demos that conflict with Disk BASIC's reserved memory). The cart and
disk are not installed in this mode; BASIC never runs.

Use this for memory-hungry standalone demos like `INVADERS.BIN`,
`CRITTERS.BIN`, `ORBIT.BIN`.

---

## Autotype lines

Any line not starting with `#` or `@` is treated as a sequence of
characters to type into Disk BASIC after the OK prompt is reached
(~12 seconds after reset).

The lines are typed in order with a `<Enter>` between them. They
appear character-by-character on the panel just as if a user were
typing on a real CoCo keyboard.

Special handling:

- Lines are typed verbatim — case-sensitive characters go through as-is
- The autotype state machine handles the CoCo's polled-keyboard
  cadence; users don't need to worry about timing
- Quotation marks in BASIC commands work normally: `LOADM"FOO":EXEC`

---

## Examples

### 1. Auto-launch PARTCLES via Disk BASIC

```
LOADM"PARTCLES":EXEC
```

The loader mounts the first `.dsk` it finds, installs `disk11.rom`,
waits for the OK prompt, then types `LOADM"PARTCLES":EXEC<Enter>`.
Requires `PARTCLES.BIN` to exist inside the mounted disk image.

### 2. Auto-launch a 64K demo via direct-load

```
@DIRECT INVADERS.BIN
```

No BASIC. Loader reads `/coco/bin/INVADERS.BIN` (or `/coco/INVADERS.BIN`),
pokes its segments into emulator RAM, jumps to the entry point.

### 3. Specific disk + auto-load a game

```
@DISK arcade.dsk
LOADM"DEFENDER":EXEC
```

### 4. Just boot to BASIC OK with a specific disk mounted

```
@DISK utilities.dsk
```

No autotype lines → land at the OK prompt with `utilities.dsk` ready
for the user to explore (`DIR`, `LOADM"name"`, etc.).

### 5. No autorun.txt at all

Loader installs `disk11.rom` if present, mounts first `.dsk` if
present, lands at the OK prompt. Same as today's default behavior.

### 6. Multi-command autotype with a comment

```
# Set screen colour then run a BASIC program
CLS 4
RUN"MYPROG"
```

---

## Edge cases & open decisions

These are points to nail down before implementation.

### A. `@DIRECT` together with autotype lines

If `autorun.txt` has both `@DIRECT foo.bin` and BASIC autotype lines,
which wins?

**Proposed:** `@DIRECT` wins — autotype lines are ignored and a warning
is serial-printed at boot. Don't want to silently drop user content.

### B. Missing disk specified by `@DISK`

If `@DISK foo.dsk` is in `autorun.txt` but `foo.dsk` isn't on the card,
what happens?

**Proposed:** halt with an error displayed on the panel. Fail loud so
the user fixes their config rather than getting confusing fallback
behavior.

### C. Path security

**Proposed:** directive arguments must be bare filenames — no `..`, no
`/`. The resolver always looks under `/coco/` and its typed subdirs.
Means `autorun.txt` can't reference arbitrary paths on the SD.

### D. Case sensitivity

**Proposed:** match FAT16/FAT32 conventions — case-insensitive. Editing
the SD from any host OS works regardless of casing.

### E. Anti-kiosk interrupt

When `autorun.txt` is doing a kiosk-style autorun, can the user abort
to drop into BASIC and explore?

**Proposed:**

- During the ~12 s boot warmup (before autotype starts), holding any
  key over the serial console aborts the autorun and lands at OK prompt.
- Once the touch launcher (AMOLED-43) ships: a touch anywhere on the
  screen during warmup aborts.

### F. Missing required ROMs

If `bas12.rom` or `extbas11.rom` aren't on the SD, the emulator can't
start at all. Today this fails silently (panel stays at its boot
test pattern).

**Proposed:** show a friendly panel message like

```
SD CARD MISSING ROMS

Please copy bas12.rom + extbas11.rom into:
  /coco/roms/  (or /coco/)

See https://github.com/.../README.md for setup.
```

Modest implementation work (needs a minimal text drawer that doesn't
need xroar running), big UX improvement for a public release.

### G. Multi-disk support

Today only drive 0 is implemented in the FDC. Demos requiring two
disks won't work. `autorun.txt` could later support `@DISK0` and `@DISK1`
when the FDC is extended; for v1 just `@DISK` (= drive 0) is fine.

### H. Cassette / BAS sources

Future directives — not in v1:

- `@CAS filename.cas` — attach a cassette image (AMOLED-41)
- `@BAS filename.bas` — auto-type a plaintext BASIC program (AMOLED-46)

---

## Boot decision tree (v1)

```
Boot
 │
 ├─ Mount /coco/ from SD
 │   └─ Fail → display "INSERT SD CARD" and halt
 │
 ├─ Load extbas11.rom + bas12.rom
 │   └─ Missing → display "ROMS NOT FOUND" help screen and halt
 │
 ├─ Parse /coco/autorun.txt  (if present)
 │
 ├─ @DIRECT mode? ──► load .bin into RAM, jump, done
 │
 ├─ Install cart:
 │     @CART specified  → use that
 │     else             → use disk11.rom if present
 │
 ├─ Mount disk:
 │     @DISK specified  → use that
 │     else             → use first .dsk found alphabetically (if any)
 │
 ├─ Boot emulator to Disk BASIC OK prompt
 │
 └─ Autotype lines from autorun.txt  (if any)
```

---

## Things to ship with the SD

A "factory" SD card sold with the device (if any) would contain:

```
/coco/
  README.txt                 ← short user guide
  autorun.txt.example        ← commented sample, renamed to enable
```

No ROMs (legal), no demos shipping copyrighted content (until each
demo's redistribution is confirmed — AMOLED-47).

Users add their own `bas12.rom`/`extbas11.rom`/`disk11.rom`, drop in
their content, and rename `autorun.txt.example` → `autorun.txt` to
enable the kiosk demo.

The README explains all of this in plain English.

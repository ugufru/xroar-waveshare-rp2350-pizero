# `autorun.txt`: boot configuration for CoCo content

When the device boots, it mounts the microSD card and looks for files in
`/coco/`. With autorun on (`autorun = on` in `settings.txt`, the default),
`/coco/autorun.txt`, if it exists, controls what happens after the system
ROMs load: which cart to install, and what gets typed into Disk BASIC (or
whether to bypass BASIC entirely). Without typed lines, autorun runs the
first program on the disk in drive 0. See [Autorun on and off](#e-autorun-on-and-off).

This is the implemented behavior. The parser lives in `src/coco_boot.cpp`
(`coco_boot_load_autorun`) and the boot steps in `src/main.cpp`.

## Editing it on the CoCo Zero

Press **F11** for the **< FILES >** list and **ENTER** on `AUTORUN.TXT`
(PIZERO-147). The same editor as `settings.txt`:
arrows and typing to edit (upper case to start, Caps Lock for lower case,
as at the BASIC prompt), **Ctrl-S** to save, **ESC** to leave (a second ESC
discards unsaved changes). If there is no file yet it opens a template of
commented-out examples, so saving it unchanged does nothing. A saved
`autorun.txt` is used at the **next power-on**; saving does not restart the
machine. Directives are not case-sensitive, and autotype lines come out in
the CoCo's upper case whatever case they are typed in.

---

## SD card layout

The loader accepts both a flat layout and an organized one:

```
/coco/
  roms/                      (optional, for organization)
    bas12.rom                ← Color BASIC          (REQUIRED, BYO)
    extbas11.rom             ← Extended Color BASIC (recommended, BYO)
    disk11.rom               ← Disk BASIC cart      (recommended, BYO)
  cart/                      (optional)
    *.ccc                    ← cartridge ROMs, 2 to 16 KB or bank-switched
  dsk/                       (optional)
    *.dsk                    ← JVC disk images
  bin/                       (optional)
    *.bin                    ← DECB load-module binaries (direct-load)
  screendumps/               ← made by the machine; Print Screen saves PNGs here
  autorun.txt                ← optional, boot configuration (this spec)
  settings.txt               ← optional, settings (see SETTINGS.md)
```

Any game can also have its own settings file beside it: `ORBIT.TXT` next to
`ORBIT.BIN`, `ORBIT.CCC` or `ORBIT.DSK` (see `SETTINGS.md`).

Note the folder names: ROMs go in `roms/` (plural); disk images in
`dsk/` and direct-load binaries in `bin/` (singular).

The three system ROMs (`bas12.rom`, `extbas11.rom`, `disk11.rom`) are
non-redistributable Microsoft/Tandy property. They are **not** shipped
with the device. Users must supply their own copies, dumped from
hardware they own.

Only `bas12.rom` is strictly required. Without `extbas11.rom` the device
boots plain Color BASIC (no Extended BASIC, and no Disk BASIC, since the
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
both accepted. Lines are read up to about 256 characters; the typed lines
together can total 1 KB, and anything past that is dropped (with a note on
serial).

Three kinds of lines:

| Starts with    | Meaning                                                |
|----------------|--------------------------------------------------------|
| `#` or blank   | Comment, skipped                                       |
| `@`            | Directive: controls boot configuration                 |
| anything else  | Autotype: typed verbatim into Disk BASIC after warmup  |

Lines are processed in order. Directives can appear anywhere; they
take effect during boot setup (before BASIC starts typing).

---

## Directives

A name in `@CART` or `@DIRECT` that is not on the card is reported on screen
("AUTORUN.TXT: NOT FOUND", naming it) and on serial, and boot carries on as
if that line were not there: Disk BASIC for a missing `@CART`, a normal boot
for a missing `@DIRECT` (PIZERO-152). File names are not case-sensitive.

### Drive 0: no directive

There is no directive for disks (`@DISK` was removed in PIZERO-183; an old
`@DISK` line is ignored, with a note on serial). The drives are remembered
instead: whatever **F12** puts in drives 0 to 3 (ENTER on a disk, or the
0 to 3 keys) is saved to `/coco/drives.txt` and comes back at the next boot,
after power-off too. On a card with nothing remembered, drive 0 gets the
first `.dsk` in `/coco/dsk/` or `/coco/` (alphabetical). A remembered disk
that is no longer on the card leaves its drive empty.

`/coco/drives.txt` is written by the board, one line per drive
(`0 = 0:/coco/dsk/SWORD.DSK`, `1 =` for an empty drive). There is no need
to edit it.

### `@CART filename.ccc` (or `.rom`)
Install this cartridge ROM at `$C000`. Searched in `/coco/cart/` first, then
`/coco/roms/`, then `/coco/`; a name without an extension gets `.CCC` in
`/coco/cart/` (PIZERO-136). Cartridges may be 2, 4, 8 or 16 KB (PIZERO-139),
or a larger bank-switched image.

If absent: the loader installs `/coco/roms/disk11.rom` (or `/coco/disk11.rom`)
if found. If neither exists, the cart slot is empty: Color BASIC only, no
disk operations, no disk mounted, and no typed lines.

### `@DIRECT filename.bin`
**Bypass Disk BASIC entirely.** Load this DECB-format `.bin` file
directly into emulator RAM (the direct-load code path used for 64K
demos that conflict with Disk BASIC's reserved memory). The cart and
disk are not installed in this mode; BASIC never runs.

Use this for memory-hungry standalone demos like `INVADERS.BIN`,
`CRITTERS.BIN`, `ORBIT.BIN`.

---

## Autotype lines

Any line not starting with `#` or `@` is treated as a sequence of
characters to type into Disk BASIC once the OK prompt is up (about
3 seconds after the machine starts).

Typed lines need a cartridge: Disk BASIC (`disk11.rom`) or the one `@CART`
names. With no cartridge at all, nothing is typed.

The lines are typed in order with a `<Enter>` between them. They
appear character-by-character on the screen just as if a user were
typing on a real CoCo keyboard.

Special handling:

- Letters are typed in the CoCo's normal upper case, whatever case they are
  written in; other characters go through as written
- The autotype state machine handles the CoCo's polled-keyboard
  cadence; users don't need to worry about timing
- Quotation marks in BASIC commands work normally: `LOADM"FOO":EXEC`

---

## Examples

### 1. Auto-launch PARTCLES via Disk BASIC

```
LOADM"PARTCLES":EXEC
```

The loader installs `disk11.rom` with drive 0's disk, waits for the OK
prompt, then types `LOADM"PARTCLES":EXEC<Enter>`. Requires `PARTCLES.BIN`
to exist on the disk in drive 0.

### 2. Auto-launch a 64K demo via direct-load

```
@DIRECT INVADERS.BIN
```

No BASIC. Loader reads `/coco/bin/INVADERS.BIN` (or `/coco/INVADERS.BIN`),
pokes its segments into emulator RAM, jumps to the entry point.

### 3. Run a disk's game at every boot

No `autorun.txt` needed. Put the disk in drive 0 once (F12, ENTER on it),
and with `autorun = on` every boot runs its first program: `RUN"NAME"` for
BASIC, `LOADM"NAME":EXEC` for machine code.

### 4. Boot to the OK prompt with a disk ready

Put `autorun = off` in `settings.txt`. `autorun.txt` is not read, and every
boot stops at the OK prompt with the remembered disks in their drives.

### 5. No autorun.txt at all

Loader installs `disk11.rom` if present, mounts the remembered disks (or the
first `.dsk`), and, with autorun on, runs drive 0's first program.

### 6. Multi-command autotype with a comment

```
# Set screen color then run a BASIC program
CLS 4
RUN"MYPROG"
```

---

## Edge cases

What the firmware does in the cases that are easy to wonder about.

### A. `@DIRECT` together with autotype lines

`@DIRECT` wins. The typed lines are ignored, without a warning, since BASIC
never runs to receive them. If the `@DIRECT` file is missing, boot carries on
normally and the typed lines are typed.

### B. A name that is not on the card

Reported on screen and on serial, and boot continues without that line
(PIZERO-152). See [Directives](#directives).

### C. Paths

Names are not restricted. The name is added to the end of the `/coco/`
search paths, so it is always looked up under `/coco/` (for example
`@DIRECT games/orbit.bin` finds `/coco/bin/games/orbit.bin` or
`/coco/games/orbit.bin`). Names can be up to 63 characters.

### D. Case sensitivity

None. Names match whatever their case on the card, as FAT does, so editing
the card from any computer works.

### E. Autorun on and off

Autorun is one boot-time feature (PIZERO-182). With `autorun = on` (the
default), every boot ends the same way: switching on, the reset button
(marked RUN), and ENTER on a disk in the F12 list. Once BASIC is up, it types
`autorun.txt`'s lines, or with none, the run command for the first program
on the disk in drive 0 (the first `.BIN` or `.BAS`).

- **Tap Space or BREAK (Esc)** before the typing starts (about 3 seconds
  after BASIC comes up, or from the moment you switch on) to cancel it for
  that boot. You get the OK prompt with the disk in drive 0.
- **`autorun = off`** turns it off. `autorun.txt` is not even read: every
  boot stops at the OK prompt, and ENTER on a disk puts it in drive 0
  without running anything. The file stays on the card for later.
- **`reset_button = basic`** (the default) turns it off for a reset-button
  boot only: the way out when an autorun game hangs. `reset_button =
  autorun` makes the reset button autorun, even with `autorun = off`, so a
  board can start at BASIC and run the demo on a press of RUN.

`@DIRECT` and `@CART` start as the machine does, before the keyboard is
ready, so a tap cannot cancel those; use the reset button. In the printed
case, RUN is reached through the vent slots with a thin wire.

### F. Missing SD card or ROMs

The machine says so on screen (PIZERO-92), in pages titled:

- **NO SD CARD**: the card is missing or cannot be read.
- **NO ROM FOUND**: the card reads, but `bas12.rom` is not on it.
- **ROM FILE IS DAMAGED**: `bas12.rom` is there but not 8192 bytes.
- **COLOR BASIC ONLY**: `extbas11.rom` is missing. This one is not fatal: it
  shows for a few seconds and the machine starts in plain Color BASIC.

The pages name `/coco/roms/` as the place for the ROMs; flat `/coco/` works
too.

### G. More than one disk

**F12** puts disks in drives 0 to 3, and all four are remembered across
power-off (PIZERO-183). See [Drive 0](#drive-0-no-directive).

### H. Cassettes and BASIC listings

Not supported: there is no `@CAS` or `@BAS`. An unknown directive is
reported on serial and skipped.

### I. The game's own settings

A game started by `autorun.txt` gets its own settings file, just as when it
is started from its list: the `@DIRECT` program's, else the `@CART`
cartridge's, else drive 0's disk when autorun runs it. `ORBIT.TXT` beside `ORBIT.BIN` is
loaded on top of `settings.txt` (see `SETTINGS.md`).

---

## Boot decision tree

```
Boot
 │
 ├─ Mount /coco/ from SD
 │   └─ Fail → show "NO SD CARD" and stop
 │
 ├─ Load bas12.rom (+ extbas11.rom if present)
 │   ├─ bas12.rom missing → show "NO ROM FOUND" and stop
 │   ├─ bas12.rom wrong size → show "ROM FILE IS DAMAGED" and stop
 │   └─ extbas11.rom missing → show "COLOR BASIC ONLY", carry on
 │
 ├─ Load /coco/settings.txt  (if present)
 │
 ├─ Autorun on for this boot?  (autorun = on, and not a RUN restart
 │   with reset_button = basic)
 │   ├─ yes → parse /coco/autorun.txt  (if present)
 │   └─ no  → do not read it
 │
 ├─ @DIRECT mode? ──► load .bin into RAM, jump, done
 │
 ├─ Install cart:
 │     @CART specified  → use that
 │     else             → use disk11.rom if present
 │     neither          → no cart, no disk, nothing typed
 │
 ├─ Mount disks:
 │     the drives remembered in /coco/drives.txt
 │     else drive 0 = first .dsk found alphabetically (if any)
 │
 ├─ Apply the started game's own settings file  (if any)
 │
 ├─ Boot emulator to Disk BASIC OK prompt
 │
 └─ Autorun on: type autorun.txt's lines, or else run drive 0's first
     program (cancelled by a Space or BREAK tap before it starts)
```

---

## Cards handed out with a unit

A card given away with a unit (see `docs/kit.md`) holds the folder layout
above and, optionally, `sample-sd/coco/autorun.txt` from this repository as
`/coco/autorun.txt`: a self-running graphics demo that doubles as a display
test.

No ROMs (legal), and no demos shipping copyrighted content unless each
demo's redistribution is confirmed. Users add their own
`bas12.rom`/`extbas11.rom`/`disk11.rom` and drop in their content.

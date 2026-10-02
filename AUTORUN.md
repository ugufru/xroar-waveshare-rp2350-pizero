# Autorun

Autorun starts a program by itself when the CoCo Zero boots.

## What it does

When BASIC comes up, autorun types the run command for the first program
(`.BIN` or `.BAS`) on the disk in drive 0. If `/coco/autorun.txt` has typed
lines, it types those instead.

It happens on every boot: switching on, pressing the reset button (marked
RUN), and pressing ENTER on a disk in the F12 list.

## Turning it off, or skipping it once

- **Tap Space or BREAK (Esc)** while the machine starts, before the typing
  begins. You get the BASIC prompt instead.
- **`autorun = off`** in `settings.txt` turns it off. `autorun.txt` is not
  read, and every boot stops at the BASIC prompt.
- **`reset_button = basic`** (the default) makes the reset button skip
  autorun. That is the way out when a program hangs. `reset_button = autorun`
  makes the reset button autorun, even with `autorun = off`.

## Which disk is in drive 0

Whatever you last put there. The disks you put in drives 0 to 3 from the F12
list are remembered after power-off (the board keeps them in
`/coco/drives.txt`). On a new card, drive 0 gets the first disk in
`/coco/dsk/`, alphabetically.

## autorun.txt

Optional. Edit it on the CoCo Zero with **F11** (FILES), or on a computer.
Three kinds of lines:

| Line | Meaning |
|---|---|
| `# ...` or blank | A comment |
| `@CART name.ccc` | Start with this cartridge instead of Disk BASIC |
| `@DIRECT name.bin` | Load this program straight into memory and run it, without BASIC |
| anything else | Typed at the BASIC prompt, one line at a time |

Examples:

```
LOADM"PARTCLES":EXEC
```

```
@DIRECT INVADERS.BIN
```

A name that is not on the card is reported on screen, and boot carries on
without it. Names are not case-sensitive.

## SD card layout

```
/coco/
  roms/          bas12.rom (required), extbas11.rom, disk11.rom
  dsk/           disk images (.dsk)
  bin/           programs (.bin)
  cart/          cartridges (.ccc)
  autorun.txt    optional
  settings.txt   optional, see SETTINGS.md
```

The ROMs are not included. Supply your own. Files can also go straight in
`/coco/`.

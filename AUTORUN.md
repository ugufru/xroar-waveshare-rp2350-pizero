# Autorun

Autorun starts a program by itself when the CoCo Zero boots.

## What it does

When BASIC comes up, autorun types the run command for the first program
(`.BIN` or `.BAS`) on the disk in drive 0. If `/coco/autorun.txt` has typed
lines, it types those instead.

It happens on every boot: switching on, pressing the reset button (marked
RUN), and pressing ENTER on a disk in the F12 list.

It works on every machine. If the game autorun starts has its own settings
file saying `machine = coco3` (see SETTINGS.md), the board first restarts as a
CoCo 3, then autoruns as usual.

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
`/coco/drives.txt`, one line per drive: `0 = 0:/coco/dsk/GAME.DSK`, or
`1 ro = ...` for a locked drive). On a new card, drive 0 gets the first disk in
`/coco/dsk/`, alphabetically.

`drives.txt` is a text file you can also edit, with **F11** (FILES) on the
board or on a computer. It is read at power-on, so an edit takes effect at the
next boot; saving it changes nothing straight away. Mounting, ejecting or
locking a disk in F12 rewrites it from the drives, so make hand edits after
your last change in F12, or redo them.

## autorun.txt

Optional. Edit it on the CoCo Zero with **F11** (FILES), or on a computer.
Three kinds of lines:

| Line | Meaning |
|---|---|
| `# ...` or blank | A comment |
| `@CART name.ccc` | Start with this cartridge instead of Disk BASIC |
| `@DIRECT name.bin` | Load this program straight into memory and run it, without BASIC |
| anything else | Typed at the BASIC prompt, one line at a time |
| `~` in a typed line | Waits a second before typing on (the CoCo has no `~` key) |

Examples:

```
LOADM"PARTCLES":EXEC
```

```
@DIRECT INVADERS.BIN
```

A disk OS needs time to boot before it can take a command. Fifteen `~` is
fifteen seconds:

```
DOS
~~~~~~~~~~~~~~~
INFOCOM /DD/GAMES/INFOCOM/ZORK1.Z3
```

A name that is not on the card is reported on screen, and boot carries on
without it. Names are not case-sensitive.

## SD card layout

```
/coco/
  roms/          CoCo 2: bas12.rom (required), extbas11.rom
                 CoCo 3: coco3.rom (required)
                 both: disk11.rom, for disks
  dsk/           disk images (.dsk)
  bin/           programs (.bin)
  cart/          cartridges (.ccc)
  autorun.txt    optional
  settings.txt   optional, see SETTINGS.md (machine = cocozero | coco2 | coco3)
  log/           written by the board (soak logs, a pending machine switch)
```

The ROMs are not included. Supply your own. Files can also go straight in
`/coco/`.

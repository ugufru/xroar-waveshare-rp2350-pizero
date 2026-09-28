# `settings.txt`: CoCo Zero settings

The CoCo Zero reads `/coco/settings.txt` from the SD card at power-on. It is
plain text, one setting per line, and every line is optional: a missing file
or line means the default. There is no settings menu; the file is the
settings. An on-screen editor for it is planned (PIZERO-146); until then,
edit it on a computer.

## Example

```
# /coco/settings.txt
sn76489           = on
volume            = 10
artifact_colours  = on
gime_palette      = on
gime_timer        = on
run_skips_autorun = on
serial_keyboard   = on
palette           = factory
```

That file sets everything to its default, so it behaves exactly like having
no file at all. Change the lines you care about and delete the rest.

## Settings

| Setting | Values | Default | What it does |
|---|---|---|---|
| `sn76489` | `on` / `off` | `on` | The SN76489 sound chip at `$FF41`, without a Games Master Cartridge. `off` gives `$FF41` back to the disk controller. With a GMC plugged in, the chip is there either way. |
| `volume` | `0`-`15` | `10` | Overall sound level. `0` is silent; `15` is half as loud again as the default. |
| `artifact_colours` | `on` / `off` / `swapped` | `on` | Colour in PMODE 4 graphics, from the NTSC artifact effect. `off` shows plain black and white; `swapped` exchanges the blue and orange, for games drawn with the other phase. |
| `gime_palette` | `on` / `off` | `on` | The CoCo 3-style palette registers at `$FFB0`-`$FFBF`. `off` removes them and restores the default palette. |
| `gime_timer` | `on` / `off` | `on` | The CoCo 3-style timer and interrupts at `$FF90`-`$FF95`. `off` removes them and stops the timer. |
| `run_skips_autorun` | `on` / `off` | `on` | Whether pressing RUN restarts straight to the BASIC prompt, skipping `autorun.txt`. `off` makes RUN behave like power-on. |
| `serial_keyboard` | `on` / `off` | `on` | Whether characters sent over the USB serial port are typed into the CoCo. |
| `palette` | `factory` / a name | `factory` | The colours the machine starts with. A name loads `/coco/pal/NAME.pal` (see below). |

Names and values are not case-sensitive, so `VOLUME = 12` works too.
`artifact_colors` is accepted as well as `artifact_colours`. A `#` starts a
comment, either on its own line or after a value:

```
volume = 7   # a bit quieter
```

A line that cannot be understood (an unknown name, a value out of range, a
missing `=`) is reported on the serial console with its line number and
skipped; the rest of the file still applies.

## Palettes: `/coco/pal/NAME.pal`

A palette file lists colours by palette entry, one per line, as `#RRGGBB`:

```
# /coco/pal/soft.pal: gentler greens for text
0 = #1ED01E
9 = #0A3C0A
```

Entries you leave out keep their factory colour, so a file only needs the
colours it changes. The entries, in the order the 6847 uses them:

| Entry | Colour | Entry | Colour |
|---|---|---|---|
| 0 | green | 8 | black |
| 1 | yellow | 9 | dark green (text background) |
| 2 | blue | 10 | dark orange |
| 3 | red | 11 | bright orange |
| 4 | white (buff) | 12-15 | unused by the 6847 |
| 5 | cyan | | |
| 6 | magenta | | |
| 7 | orange | | |

Then set `palette = soft` in `settings.txt`. The screen works in 16-bit
colour, so each colour is rounded to the nearest of its 65,536. Programs
that write the CoCo 3-style palette registers still can; a reset puts your
palette back. Per-game palettes (PIZERO-55) will use the same file format.

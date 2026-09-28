# `settings.txt`: CoCo Zero settings

The CoCo Zero reads `/coco/settings.txt` from the SD card at power-on. It is
plain text, one setting per line, and every line is optional: a missing file
or line means the default. There is no settings menu; the file is the
settings.

## Editing it on the CoCo Zero

Press **F12**, then **Right** until the title reads **< FILES >**, and
**ENTER** on `SETTINGS.TXT`. The file opens as text on the screen. If there
is no file yet, it opens a template listing every setting at its default.

- Arrows, Home/End and PgUp/PgDn move; typing inserts; Backspace, Delete and
  Enter work as usual. Held keys repeat.
- **Ctrl-S** saves. The settings take effect straight away, no reboot.
- **ESC** leaves. With unsaved changes, the first ESC warns and a second one
  discards them.

The screen shows upper case only (the CoCo's font has no lower case), which
is fine: settings are not case-sensitive. The save writes a new copy and
then swaps it in, so pulling the power mid-save cannot leave a broken file.
You can also edit the file on a computer.

## Example

```
# /coco/settings.txt
sn76489           = on
volume            = 10
artifact_colors   = on
gime_palette      = on
gime_timer        = on
run_skips_autorun = on
serial_keyboard   = on
```

That file sets everything to its default, so it behaves exactly like having
no file at all. Change the lines you care about and delete the rest.

## Settings

| Setting | Values | Default | What it does |
|---|---|---|---|
| `sn76489` | `on` / `off` | `on` | The SN76489 sound chip at `$FF41`, without a Games Master Cartridge. `off` gives `$FF41` back to the disk controller. With a GMC plugged in, the chip is there either way. |
| `volume` | `0`-`15` | `10` | Overall sound level. `0` is silent; `15` is half as loud again as the default. |
| `artifact_colors` | `on` / `off` / `swapped` | `on` | Color in PMODE 4 graphics, from the NTSC artifact effect. `off` shows plain black and white; `swapped` exchanges the blue and orange, for games drawn with the other phase. |
| `gime_palette` | `on` / `off` | `on` | The CoCo 3-style palette registers at `$FFB0`-`$FFBF`. `off` removes them and restores the default palette. |
| `gime_timer` | `on` / `off` | `on` | The CoCo 3-style timer and interrupts at `$FF90`-`$FF95`. `off` removes them and stops the timer. |
| `run_skips_autorun` | `on` / `off` | `on` | Whether pressing RUN restarts straight to the BASIC prompt, skipping `autorun.txt`. `off` makes RUN behave like power-on. |
| `serial_keyboard` | `on` / `off` | `on` | Whether characters sent over the USB serial port are typed into the CoCo. |
| `color_NAME` | `#RRGGBB` | the 6847's | Overrides one palette color. See below. |

Names and values are not case-sensitive, so `VOLUME = 12` works too. A `#`
starts a comment, either on its own line or after a value:

```
volume = 7   # a bit quieter
```

A line that cannot be understood (an unknown name, a value out of range, a
missing `=`) is reported on the serial console with its line number and
skipped; the rest of the file still applies.

## Colors

The machine starts with the 6847's own colors. Any of them can be replaced
with a `color_` line giving the new color as `#RRGGBB`:

```
color_green      = #1ED01E   # softer text
color_dark_green = #0A3C0A   # text background
```

Colors without a line keep their default. The names:

| Setting | Default color | Setting | Default color |
|---|---|---|---|
| `color_green` | green | `color_black` | black |
| `color_yellow` | yellow | `color_dark_green` | dark green (text background) |
| `color_blue` | blue | `color_dark_orange` | dark orange |
| `color_red` | red | `color_bright_orange` | bright orange |
| `color_white` | white (buff) | | |
| `color_cyan` | cyan | | |
| `color_magenta` | magenta | | |
| `color_orange` | orange | | |

The screen works in 16-bit color, so each color is rounded to the nearest of
its 65,536. Programs that write the CoCo 3-style palette registers still
can; a reset puts your colors back.

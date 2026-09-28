# `settings.txt`: CoCo Zero settings

The CoCo Zero reads `/coco/settings.txt` from the SD card at power-on. It is
plain text, one setting per line, and every line is optional: a missing file
or line means the default. There is no settings menu; the file is the
settings.

## Editing it on the CoCo Zero

Press **F11** for the **< FILES >** list and **ENTER** on `SETTINGS.TXT`. The file opens as text on the screen. If there
is no file yet, it opens a template listing every setting at its default.

- Arrows, Home/End and PgUp/PgDn move; typing inserts; Backspace, Delete and
  Enter work as usual. Held keys repeat.
- **Ctrl-S** saves. The settings take effect straight away, no reboot.
- **ESC** leaves. With unsaved changes, the first ESC warns and a second one
  discards them.

The editor shows text as typed, lower case included (settings are not
case-sensitive either way). The save writes a new copy and
then swaps it in, so pulling the power mid-save cannot leave a broken file.
You can also edit the file on a computer.

## Settings for one game

A disk, program or cartridge can have its own settings file: the same name
with `.TXT` in place of its extension, in the same folder. `ORBIT.TXT` goes
with `/coco/bin/ORBIT.BIN`, `/coco/cart/ORBIT.CCC` or `/coco/dsk/ORBIT.DSK`.
It uses the same names as `settings.txt`, `color_` lines included, and only
needs the lines that differ:

```
# /coco/bin/ORBIT.TXT
artifact_colors = swapped
color_green     = #1ED01E
```

When the game starts, from its list or from `autorun.txt`, the machine loads
`settings.txt` and then the game's file on top. Launching something without
a file of its own goes back to plain `settings.txt`.

To make or change one on the CoCo Zero, open the game's list (**F9**
programs, **F10** cartridges, **F12** disks), highlight it and press **TAB**.
A new file starts as a few commented examples. Ctrl-S saves; if that game is
the one running, the change applies straight away, otherwise it applies the
next time the game starts. A game called `SETTINGS` or `AUTORUN` cannot have
one, since its file would be one of the machine's own.

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
joystick_swap     = off
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
| `joystick_swap` | `on` / `off` | `off` | Which USB gamepad stick is which CoCo joystick. `off`: the pad's left stick (and D-pad) is the right joystick, `JOYSTK(0)` and `JOYSTK(1)`, the one most games read, and the right stick is the left joystick. `on` swaps them, fire buttons included: the right stick steers `JOYSTK(0)`/`(1)` and the left-hand buttons (L1, square) fire it. Handy in a game's own settings file. |
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

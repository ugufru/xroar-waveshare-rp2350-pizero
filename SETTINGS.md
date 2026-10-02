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

Letters type in lower case, like the settings names. Caps Lock switches to
upper case, and Shift gives the other case for one letter. Settings are not
case-sensitive either way, and the editor shows text as typed.

The file always opens tidied: comment lines (`#`) first, then the settings in
alphabetical order, with blank lines dropped. A game's own settings file opens
the same way. Nothing changes on the card unless you save. The save writes a new copy and
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
autorun           = on
reset_button      = basic
serial_keyboard   = on
joystick_swap     = off
font              = 6847t2
lowercase         = on
key_repeat        = on
key_repeat_delay  = 500
key_repeat_rate   = 10
video_encoder     = hardware
dpad              = joystick
pad_bottom        = fire
pad_right         = fire
pad_left          = fire_right
pad_top           = space
pad_l1            = fire_right
pad_r1            = fire
pad_l2            = none
pad_r2            = none
pad_select        = none
pad_start         = enter
pad_l3            = none
pad_r3            = none
```

That file sets every setting except the colors to its default, so it behaves
exactly like having no file at all. Change the lines you care about and
delete the rest.

## Settings

| Setting | Values | Default | What it does |
|---|---|---|---|
| `sn76489` | `on` / `off` | `on` | The SN76489 sound chip at `$FF41`, without a Games Master Cartridge. `off` gives `$FF41` back to the disk controller. With a GMC plugged in, the chip is there either way. |
| `volume` | `0`-`15` | `10` | Overall sound level. `0` is silent; `15` is half as loud again as the default. |
| `artifact_colors` | `on` / `off` / `swapped` | `on` | Color in PMODE 4 graphics, from the NTSC artifact effect. `off` shows plain black and white; `swapped` exchanges the blue and orange, for games drawn with the other phase. |
| `gime_palette` | `on` / `off` | `on` | The CoCo 3-style palette registers at `$FFB0`-`$FFBF`. `off` removes them and restores the default palette. Only in the default 60 Hz build (`pizero_stream_60`); the `pizero_stream` fallback has no palette registers, so the setting does nothing there. |
| `gime_timer` | `on` / `off` | `on` | The CoCo 3-style timer and interrupts at `$FF90`-`$FF95`. `off` removes them and stops the timer. Like `gime_palette`, only in the default 60 Hz build. |
| `autorun` | `on` / `off` | `on` | Whether `autorun.txt` runs when the board is switched on. `off` starts at the BASIC prompt (Disk BASIC with the default disk), as if the card had no `autorun.txt`. The reset button follows `reset_button` instead. To skip autorun just once, press Space or BREAK (Esc) while the board starts. |
| `reset_button` | `basic` / `autorun` | `basic` | What the board's reset button (marked RUN) does. `basic` restarts to a clean BASIC prompt and skips `autorun.txt`, the way out when an autorun game hangs. `autorun` restarts into `autorun.txt`, even with `autorun = off`, so the button can start the demo on a board that otherwise starts at BASIC. Pressing Space or BREAK while it starts still skips it. |
| `serial_keyboard` | `on` / `off` | `on` | Whether characters a computer sends over the serial link (the USB-C power port) are typed into the CoCo. |
| `joystick_swap` | `on` / `off` | `off` | Which USB-C gamepad stick is which CoCo joystick. `off`: the pad's left stick (and D-pad) is the right joystick, `JOYSTK(0)` and `JOYSTK(1)`, the one most games read, and the right stick is the left joystick. `on` swaps them, fire buttons included: the right stick steers `JOYSTK(0)`/`(1)` and the left-hand buttons (L1, square) fire it. Handy in a game's own settings file. |
| `font` | `classic` / `6847t1` / `6847t2` | `6847t2` | The text font. `classic` is the original CoCo 1 and 2 chip: lower case shows as inverse capitals, `^` as an up arrow and `_` as a left arrow. `6847t1` is the later CoCo 2B chip: the same, until a program turns on true lower case with `POKE 65314,16`, which also shows `{ \| } ~` (BASIC switches it off again at the prompt and on `PRINT`; see `lowercase`). `6847t2` is ours: the 6847T1 with a real caret for `^` and a real underscore for `_`. Inverse text in programs looks the same in all three. |
| `lowercase` | `on` / `off` | `on` | With the `6847t2` font, shows true lower case and `{ \| } ~` all the time. Color BASIC keeps switching the 6847T1's lower case off (at the prompt and on every `PRINT`), so `POKE 65314,16` alone rarely lasts; this holds it on, and Caps Lock types real lower case at the prompt. The cost: programs that draw inverse text show it as lower case instead; put `lowercase = off` in such a game's own settings file. No effect with the other fonts. |
| `key_repeat` | `on` / `off` | `on` | Auto-repeat for a key held on the USB-C keyboard, which Color BASIC does not do itself. The newest key held repeats; BREAK never does. Games that read held keys directly may see a key flicker; put `key_repeat = off` in such a game's own settings file. |
| `key_repeat_delay` | `100`-`2000` | `500` | Milliseconds a key is held before it starts repeating. |
| `key_repeat_rate` | `1`-`30` | `10` | Repeats a second. Above 12 it is held to 12, so BASIC sees every press. |
| `video_encoder` | `hardware` / `software` | `hardware` | How the picture is encoded for the display. `hardware` uses the encoder built into the RP2350 chip, which takes half the time. `software` is the older method, kept in case a display shows a problem with `hardware`. The picture should look the same either way. F1 INFO shows which one is in use. |
| `dpad`, `pad_...` | see below | | What the gamepad's D-pad and each button do. See [Gamepad buttons](#gamepad-buttons). |
| `color_NAME` | `#RRGGBB` | the 6847's | Overrides one palette color. See below. |

Names and values are not case-sensitive, so `VOLUME = 12` works too. Where a
setting takes `on` / `off`, `yes` / `no` and `1` / `0` work as well. A `#`
starts a comment, either on its own line or after a value:

```
volume = 7   # a bit quieter
```

A line that cannot be understood (an unknown name, a value out of range, a
missing `=`) is reported on the serial console with its line number and
skipped; the rest of the file still applies.

## Gamepad buttons

Every button on a USB-C gamepad can do something. Each has a line, named by
where the button is (so it means the same on Xbox, PlayStation and Nintendo
style pads), and each can be one of:

- `fire`: the fire button of the joystick the left stick (and D-pad) drives;
- `fire_right`: the fire button of the joystick the right stick drives;
- `none`;
- a CoCo key: a letter, a digit, one of `@ : ; , - . /`, or `space`,
  `enter`, `clear`, `break`, `shift`, `up`, `down`, `left`, `right`.

| Setting | Button | Default |
|---|---|---|
| `pad_bottom` | bottom face button (A on Xbox, cross on PlayStation) | `fire` |
| `pad_right` | right face button (B, circle) | `fire` |
| `pad_left` | left face button (X, square) | `fire_right` |
| `pad_top` | top face button (Y, triangle) | `space` |
| `pad_l1`, `pad_r1` | shoulder buttons | `fire_right`, `fire` |
| `pad_l2`, `pad_r2` | triggers | `none` |
| `pad_select`, `pad_start` | the two small buttons left and right of center | `none`, `enter` |
| `pad_l3`, `pad_r3` | pressing a stick in | `none` |

The center **Home** button is not in the list: it always opens and closes
the overlay lists (the F9 to F12 screens). While a list is open, the D-pad
moves (left and right switch lists), **A** (bottom) selects, **B** (right)
goes back, L1 and R1 page, and X (left) edits the highlighted game's own
settings file.

`dpad = arrows` makes the D-pad press the arrow keys instead of moving the
joystick, for the many games that read the arrow keys. `dpad = joystick` is
the default.

These are most useful in a game's own settings file. A game that starts on
the S key and moves with the arrows:

```
# /coco/bin/ORBIT.TXT
pad_start = s
dpad      = arrows
```

`joystick_swap` still swaps the sticks, and each stick's fire buttons go
with it.

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

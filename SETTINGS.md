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
- **F5** wraps long lines onto the next row, and turns wrapping off again.
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
You can also edit the file on a computer. Lines may end the Mac way (LF), the
Windows way (CRLF) or the CoCo way (CR alone); the board reads all three, and
when it changes a line itself (F8, the volume keys) it keeps the file's own.

The on-screen editor holds up to 4 KB. A bigger file is not opened, and the
bottom row says why, so nothing is ever saved back cut short. A file is saved
with the line endings it came with (CR, LF or CRLF); a new one, such as the
template for a missing settings.txt, is written with the CoCo's CR.

## Settings for one game

A disk, program or cartridge can have its own settings file: the same name
with `.TXT` in place of its extension, in the same folder. `ORBIT.TXT` goes
with `/coco/bin/ORBIT.BIN`, `/coco/cart/ORBIT.CCC` or `/coco/dsk/ORBIT.DSK`.
It uses the same names as `settings.txt`, `color_` lines included, and only
needs the lines that differ. A CoCo 3 game's file says `machine = coco3`, and
launching the game switches the board to a CoCo 3 (see `machine` below):

```
# /coco/bin/ORBIT.TXT
artifact_colors = swapped
color_green     = #1ED01E
```

When the game starts, from its list or from `autorun.txt`, the settings are
built in three layers: the machine's own starting point (see `machine`
below), then `settings.txt`, then the game's file on top. Launching something
without a file of its own goes back to plain `settings.txt`.

To make or change one on the CoCo Zero, open the game's list (**F9**
programs, **F10** cartridges, **F12** disks), highlight it and press **TAB**.
A new file starts as a few commented examples. Ctrl-S saves; if that game is
the one running, the change applies straight away, otherwise it applies the
next time the game starts. A game called `SETTINGS` or `AUTORUN` cannot have
one, since its file would be one of the machine's own.

## Example

```
# /coco/settings.txt
machine           = cocozero
# The machine sets these. A line here would hold for every game:
# sn76489         = on
# coco_psg        = on
# symphony12      = on
# orchestra90     = on
# midi_pak        = on
# gime_palette    = on
# gime_timer      = on
# font            = 6847t2
# lowercase       = on
# key_repeat      = on
# sound_sources   = all
# joystick_quiet  = on
symphony12_stereo = on
deluxe_psg        = off
volume            = 10
audio_lowpass     = 8000
artifact_colors   = on
autorun           = on
reset_button      = basic
serial_keyboard   = on
joystick_swap     = off
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
pad_l2            = fire2_right
pad_r2            = fire2
pad_select        = none
pad_start         = enter
pad_l3            = none
pad_r3            = none
```

That file sets every setting to its default, so it behaves exactly like
having no file at all. Change the lines you care about and delete the rest.
The eight commented lines are the ones the machine decides. Leave them
commented unless you want that value on every machine: `sn76489 = on`
uncommented would also give the stock CoCo 2 and CoCo 3 the sound chip.

## Settings

Most settings work the same on every machine. Those marked **CoCo 2** only change the CoCo 2
(the CoCo Zero or the stock CoCo 2): the CoCo 3 has its own GIME colours, font and palette, set by
its programs. Where the default says **machine**, it depends on `machine`: on for the CoCo Zero,
off (or the original font) for the stock CoCo 2 and CoCo 3.

The sound chips behind `sn76489`, `coco_psg`, `symphony12`, `symphony12_stereo` and `deluxe_psg`
(their addresses, how to program them from BASIC, and test programs), and how `sound_sources`
and `joystick_quiet` route them, are in [`docs/sound.md`](docs/sound.md).

| Setting | Values | Default | What it does |
|---|---|---|---|
| `sn76489` | `on` / `off` | machine | The SN76489 sound chip at `$FF41`, without a Games Master Cartridge. `off` gives `$FF41` back to the disk controller. With a GMC plugged in, the chip is there either way. |
| `coco_psg` | `on` / `off` | machine | The CoCo PSG cartridge's sound chip, an AY-3-8910 type (YM2149): register select at `$FF5E`, data at `$FF5F`, and `$FF5D` bit 0 for its 2 MHz or 1 MHz clock. Its flash, RAM and joystick ports are not emulated. |
| `symphony12` | `on` / `off` | machine | The Speech Systems Symphony-12 cartridge: four AY-3-8910 chips, twelve voices, through a PIA at `$FF60`-`$FF63`. It shares its first chip with `coco_psg` and `deluxe_psg`. A chip costs time only while one of its voices is turned up: four playing at once take about 1.6 ms of each 16.7 ms frame on a CoCo 2 (PIZERO-248). The CoCo 3 at 1.79 MHz is not measured yet. |
| `symphony12_stereo` | `on` / `off` | `on` | The Symphony-12's stereo jumper: its first two chips play on the left and the other two on the right, as the card's own output jacks gave them. `off` mixes all four to the middle, as the card's sound came back into the CoCo. A chip that the CoCo PSG or Deluxe CoCo wrote last stays in the middle either way. Every machine has it, as it belongs to the cartridge, and it does nothing while `symphony12` is off. |
| `usb_audio` | `on` / `off` | `on` | Plays the sound from a USB headphone DAC plugged in at power-on (a USB-C headphone dongle, or a hub's own headphone jack) instead of the video cable. With no DAC, or `off`, the sound stays on the video cable. See [`docs/sound.md`](docs/sound.md). |
| `usb_audio_db` | `-60`-`0` | `-10` | The USB headphone DAC's own level, in dB, set when it starts: 0 is its loudest. The CoCo Zero's sound is made for a TV, which at a DAC's 0 dB is hot in headphones, so it starts 10 dB down. The volume keys and `volume` work on top of it, for both outputs. |
| `midi_pak` | `on` / `off` | machine | The MIDI Pak: a 6850 serial chip at `$FF6E`-`$FF6F`, the CoCo's MIDI interface for Lyra, CoCo MIDI and similar programs. The board passes its MIDI to and from the computer it is plugged into, over the USB-C cable, where it shows up as a USB MIDI port. Its interrupt goes to the cartridge line, as on the real pak. On for the CoCo Zero; for the stock machines put `midi_pak = on` in the program's own file. |
| `orchestra90` | `on` / `off` | machine | Tandy's Orchestra-90CC: two 8-bit DACs, left at `$FF7A` and right at `$FF7B`, played in stereo. Its music program is the cartridge ROM (load it with F10). On for the CoCo Zero; for the stock machines put `orchestra90 = on` in the cartridge's own file. |
| `sound_sources` | `all` / `switched` | machine (`all` on the CoCo Zero) | `all` plays every sound source at once: the CoCo's DAC, its single-bit output and every sound cartridge. `switched` is the real CoCo: its multiplexer picks one source (the DAC, the cassette or the cartridge's sound line) and its sound enable turns it on and off, so a program hears only what it selects. The stock CoCo 2 and CoCo 3 use `switched`. |
| `joystick_quiet` | `on` / `off` | machine | Keeps joystick reads out of the sound. A program reads an analog joystick by writing trial values to the DAC, which are heard as a buzz while the sound is on (Double Back does it). `on` takes each trial back out of the sound; the joystick reads exactly the same. `off` hears every trial, as the real CoCo does. On for the CoCo Zero, off for the stock machines. |
| `deluxe_psg` | `on` / `off` | `off` | The sound chip of Tandy's unreleased Deluxe Color Computer, at `$FF38` (data) and `$FF39` (register). Off on every machine, because on a CoCo 2 those addresses repeat the keyboard and sound PIA, and a program that uses the repeat would break. Nearly no software uses it. |
| `volume` | `0`-`15` | `10` | Overall sound level. A USB keyboard's volume keys and a USB headphone DAC's buttons change it while you play, and the new level is saved here. `0` is silent; `15` is half as loud again as the default. Each number is louder than it used to be: three times on 2026-10-14 (PIZERO-223) and twice again on 2026-10-16 (PIZERO-230), six times in all, so a `settings.txt` written before then plays louder at the same number. Above `10`, music that drives the CoCo's sound hard may clip (PIZERO-231). |
| `audio_lowpass` | `1000`-`20000` / `on` / `off` | `8000` | Softens the sound: a filter that takes off what is above this many hertz, the way the TV did on the real machine. It mostly removes the harsh buzz of the CoCo's sound steps. Lower is mellower, higher is brighter; `off` gives the raw sound, and `on` means `8000`. Try a game's own settings file for one that sounds muffled. |
| `artifact_colors` | `on` / `off` / `swapped` | `on` | **CoCo 2.** Color in PMODE 4 graphics, from the NTSC artifact effect. `off` shows plain black and white; `swapped` exchanges the blue and orange, for games drawn with the other phase. **F8** steps through on, swapped and off while a program runs, and saves the choice in that game's own settings file (or in `settings.txt` when nothing was launched). |
| `gime_palette` | `on` / `off` | machine | **CoCo 2.** The CoCo 3-style palette registers at `$FFB0`-`$FFBF`. `off` removes them and restores the default palette. Only in the default 60 Hz build (`pizero_stream_60`); the `pizero_stream` fallback has no palette registers, so the setting does nothing there. |
| `gime_timer` | `on` / `off` | machine | **CoCo 2.** The CoCo 3-style timer and interrupts at `$FF90`-`$FF95`. `off` removes them and stops the timer. Like `gime_palette`, only in the default 60 Hz build. |
| `autorun` | `on` / `off` | `on` | Run drive 0's first program (or `autorun.txt`) when the machine boots. Tap Space to skip it once. `off` always stops at the BASIC prompt. See AUTORUN.md. |
| `reset_button` | `basic` / `autorun` | `basic` | What the reset button (marked RUN on the board) does. `basic` restarts to the BASIC prompt, skipping autorun. `autorun` restarts and autoruns, even with `autorun = off`. |
| `serial_keyboard` | `on` / `off` | `on` | Whether characters a computer sends over the serial link (the USB-C power port) are typed into the CoCo. |
| `joystick_swap` | `on` / `off` | `off` | Which USB-C gamepad stick is which CoCo joystick. `off`: the pad's left stick (and D-pad) is the right joystick, `JOYSTK(0)` and `JOYSTK(1)`, the one most games read, and the right stick is the left joystick. `on` swaps them, fire buttons included: the right stick steers `JOYSTK(0)`/`(1)` and the left-hand buttons (L1, square) fire it. Handy in a game's own settings file. |
| `font` | `classic` / `6847t1` / `6847t2` | machine: `6847t2` for the CoCo Zero, `classic` otherwise | **CoCo 2.** The text font. `classic` is the original CoCo 1 and 2 chip: lower case shows as inverse capitals, `^` as an up arrow and `_` as a left arrow. `6847t1` is the later CoCo 2B chip: the same, until a program turns on true lower case with `POKE 65314,16`, which also shows `{ \| } ~` (BASIC switches it off again at the prompt and on `PRINT`; see `lowercase`). `6847t2` is ours: the 6847T1 with a real caret for `^` and a real underscore for `_`. Inverse text in programs looks the same in all three. |
| `lowercase` | `on` / `off` | machine | **CoCo 2.** With the `6847t2` font, shows true lower case and `{ \| } ~` all the time. Color BASIC keeps switching the 6847T1's lower case off (at the prompt and on every `PRINT`), so `POKE 65314,16` alone rarely lasts; this holds it on, and Caps Lock types real lower case at the prompt. The cost: programs that draw inverse text show it as lower case instead; put `lowercase = off` in such a game's own settings file. No effect with the other fonts. |
| `key_repeat` | `on` / `off` | machine | Auto-repeat for a key held on the USB-C keyboard, which Color BASIC does not do itself, so it is on for the CoCo Zero and off for the stock CoCo 2 and CoCo 3. The newest key held repeats; BREAK never does. Games that read held keys directly may see a key flicker; put `key_repeat = off` in such a game's own settings file. |
| `soak_log` | `on` / `off` | `off` | Writes the board's once-a-second telemetry to `/coco/log/soak-NNN.txt` once a minute, timestamped, so a soak test runs from a power supply with no computer attached. One file per power-on. Read it with `scripts/soak.py analyse`. See docs/BUILD.md. |
| `key_repeat_delay` | `100`-`2000` | `500` | Milliseconds a key is held before it starts repeating. |
| `key_repeat_rate` | `1`-`30` | `10` | Repeats a second. Above 12 it is held to 12, so BASIC sees every press. |
| `video_encoder` | `hardware` / `software` | `hardware` | How the picture is encoded for the display. `hardware` uses the encoder built into the RP2350 chip, which takes half the time. `software` is the older method, kept in case a display shows a problem with `hardware`. The picture should look the same either way. The INFO page (F12, then Left) shows which one is in use. **No effect at present:** since the 640-wide picture (PIZERO-134, 135) both machines always use the hardware encoder (PIZERO-240). |
| `machine` | `cocozero` / `coco2` / `coco3` | `cocozero` | Which CoCo the board is. `cocozero` is a CoCo 2 with this project's extras: the SN76489, the CoCo 3-style palette and timer, the `6847t2` font and true lower case. `coco2` is a stock Tandy Color Computer 2 and `coco3` a stock 128 KB Color Computer 3 (needs `coco3.rom` on the card), with those settings (the AY cartridges included) and auto-repeat off and the `classic` font. The machine only sets their starting point: a line for any of them in `settings.txt` or a game's file still wins. In `settings.txt` it picks the machine at power-on and after RUN; saving a change to or from `coco3` restarts the board as the other machine, at the BASIC prompt, and between `cocozero` and `coco2` it applies at once. In a game's own settings file it switches when that game is launched: the board restarts, about two seconds, and starts the game. Launching a game without one goes back to `settings.txt`'s machine. Without `coco3.rom` a page says so and the board stays a CoCo 2. |
| `dpad`, `pad_...` | see below | | What the gamepad's D-pad and each button do. See [Gamepad buttons](#gamepad-buttons). |
| `color_NAME` | `#RRGGBB` | the 6847's | **CoCo 2.** Overrides one palette color. See below. |

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
- `fire2`, `fire2_right`: the second button of those joysticks. Only the
  CoCo 3 has second buttons; on a CoCo 2 these do nothing;
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
| `pad_l2`, `pad_r2` | triggers | `fire2_right`, `fire2` |
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

These are the CoCo 2's: the CoCo 3's colours come from its GIME palette, which its programs set.
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

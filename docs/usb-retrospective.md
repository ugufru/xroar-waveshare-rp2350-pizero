# USB retrospective

A record of how USB input got from nothing to a keyboard and a gamepad
working together through a hub, a keyboard that types what its keycaps say,
and the screen fonts that go with it, then to sound out of a USB DAC and the
board appearing to a computer as a MIDI interface: what went wrong along the
way, and what we now do differently because of it. It covers PIZERO-11
through PIZERO-167, 2026-05-27 to 2026-09-28, and was brought up to date on
2026-11-01 (PIZERO-203, 215, 227, 255, 287, 316).

The board has two USB ports with different jobs. The **host port** (PIO-USB)
takes keyboards, pads, hubs and now a headphone DAC. The **native USB-C
port** is a USB device: a computer sees it as a serial port and, since
PIZERO-287, a MIDI interface.

How to use the keyboard and pad is in [README.md](../README.md) and
[SETTINGS.md](../SETTINGS.md); the build flags are in [BUILD.md](BUILD.md).
This file is about the process and the failures, so the same mistakes are
not made twice.

## Where it stands

| Area | State |
|---|---|
| USB host | Pico-PIO-USB (software USB on PIO 1, D+ GPIO 28, D- 29) under Adafruit TinyUSB 3.7.7. Full speed (12 Mbit/s) only. Done, PIZERO-11. |
| Keyboard | Types what the keycaps say (US layout): `= + : ' " _ [ ] \` where they are printed, Caps Lock is the case toggle, Home is CLEAR, and the numeric keypad works. `{ } \| ~` and backtick do nothing: the CoCo has no such keys. Done (PIZERO-163, 49). Replaced the positional mapping of PIZERO-12. On a CoCo 3, CTRL, ALT, F1 and F2 too (PIZERO-215). |
| Auto-repeat | Held keys repeat, on by default; delay and rate configurable, per game too. Done, PIZERO-167. |
| Screen text | The machine's font is a setting: `classic`, `6847t1`, or our `6847t2` (default), which has a real caret and underscore, plus true lower case and `{ \| } ~` with `lowercase` (on by default). The firmware's own screens draw them too. Done (PIZERO-162, 166). |
| Gamepad | Works as both CoCo joysticks, two fire buttons (both buttons of each stick on a CoCo 3, PIZERO-215). Confirmed on hardware with a GameSir Tegenaria Lite in two of its modes. Done, PIZERO-13. |
| Pad buttons | Any button can press a CoCo key (`pad_<button> = key`), and `dpad = arrows`. Done, PIZERO-164. |
| Headphone DAC | A class-compliant USB headphone DAC on the host port plays the sound instead of the video cable. Needed isochronous OUT added to Pico-PIO-USB, which had none. Volume keys on a keyboard or DAC set the volume. Done, PIZERO-255. Costs about 2.3 ms a frame, more under heavy sound (PIZERO-259). |
| Native port | A composite device: USB serial (log, serial typing) and, since PIZERO-287, "CoCo Zero MIDI Pak", the CoCo's MIDI interface as a computer sees it. Built, awaiting real software (PIZERO-287). |
| Stick swap | `joystick_swap` setting, also per game. Done, PIZERO-160. |
| Hub | Keyboard and pad together through a simple hub, 60 fps, clean audio. Done, PIZERO-54. |
| Hot-plug | Straight into the board: no. A device plugged in after power-on is never seen. Through a hub: a keyboard plugged in while running did mount. PIZERO-51. |
| Stability | The one live hang (PIZERO-203), about one every 3 hours on a soak, is core 0 stuck inside the USB host's 1 ms timer interrupt, in PIO-USB's bus code. Found by PIZERO-227's post-mortem. Not yet fixed. |

Host tests: 338 in all (2026-11-01). 21 are the gamepad decoders and joystick
comparator (`test/test_gamepad`), 17 the keyboard translation and
auto-repeat (`test/test_key_translate`), 8 the MIDI packing
(`test/test_midi_usb_pack`).

## Timeline

| Date | Issue | What happened |
|---|---|---|
| 05-27 to 29 | PIZERO-11, 11a | USB host up. Three fixes needed: start USB before core 1 (alarm pool hang), 64-byte HID buffers, boot protocol before `begin()`. The early "board fault" was device compatibility: the stack is USB 1.1 only. |
| 05-27 to 29 | PIZERO-12 | Keyboard: a 256-entry HID to CoCo key table and a diff of the 6-key report. Any 8-byte report treated as a keyboard, because the test dongle mislabels itself as a mouse. |
| 05-29 to 06-01 | PIZERO-11b | Devices enumerate only at cold boot. First blamed on power; that was an unverified assumption, corrected 06-01. |
| 06-28 to 29 | PIZERO-11b | Instrumented. The board's rev3 RP2350 compiles out PIO-USB's E9 workaround; after enumeration the line is pinned at J/FS, so an unplug is invisible, and an unplugged device floods about 180 byte-identical phantom reports a second. |
| 06-28 | PIZERO-13, 49, 50 | Gamepad, keypad and header-joystick issues filed. |
| 07-22 to 08-16 | PIZERO-51 | Three hot-plug fixes built and disproved on hardware: host stop/restart, a forced disconnect, a watchdog reboot. VBUS is hardwired on this board; only a real power cycle re-enumerates. |
| 09-28 | PIZERO-13 | Gamepad ported from the Fruit Jam port (FRUITJAM-18). The pad never took the identity Fruit Jam saw. Two new decoders measured on the board. Confirmed on hardware (f178939). |
| 09-28 | PIZERO-54 | A multi-port USB-C hub failed. A debug build found a one-hub limit hard-coded in the library, and behind it a TinyUSB race. Fixed the limit (98fc68f); a simple hub then worked (422adf1). |
| 09-28 | PIZERO-160 | `joystick_swap` setting, confirmed across several games (95a3133). |
| 09-28 | PIZERO-157, 158, 159 | Filed: Switch mode handshake, dropped serial characters, the chained-hub race. |
| 09-28 | PIZERO-161 | This retrospective. |
| 09-28 | PIZERO-162 | The firmware's own screens draw `_ { \| } ~ ^` and lower case: the 6847T1 font we already used had the glyphs, the card code could not reach them (73e9793). |
| 09-28 | PIZERO-163 | The keyboard types what its keycaps say, and the key matrix gets one layer per source (8fe9ca9). |
| 09-28 | PIZERO-166 | Font setting and the 6847T2 (6786424). The user's `POKE 65314,16` did not work; a register trace showed BASIC undoing it. `lowercase` setting added (f4c9ca6). |
| 09-28 | PIZERO-167 | Auto-repeat, on by default, confirmed on hardware (8b6cdca). Rule adopted: improvements are on by default. |
| 09-28 | PIZERO-164 | Pad buttons press CoCo keys; `dpad = arrows` confirmed on hardware. |
| 10-15 to 10-17 | PIZERO-227 | Watchdog post-mortem: where core 0 really was at a freeze (interrupted PC, exception number). |
| 10-20 | PIZERO-215 | CoCo 3 keys and second fire buttons. The GameSir's R2 bit, set at rest, held a key row down once R2 had a job. |
| 10-23 | PIZERO-255 | USB headphone DAC: isochronous OUT patched into Pico-PIO-USB, the stream in the 1 ms frame interrupt; volume keys. |
| 10-29 | PIZERO-283 | Idea filed: with a debug probe doing flashing and the log, the native port could be a second, hardware USB host. |
| 11-01 | PIZERO-203 | 269 card soak logs read: every post-mortem hang is in the USB host's timer interrupt. |
| 11-01 | PIZERO-287 | The native port becomes serial plus MIDI. Two USB lessons on the way (below). |
| 11-01 | PIZERO-316 | The boot no longer waits 4 s for a serial monitor unless `debug = on`; early output is held until one opens the port. |

## What worked

**Porting, not reinventing.** The joystick comparator, fire buttons, dead
zone and stick mapping came from the Fruit Jam port, where they had already
met hardware. The emulated-joystick half worked on the first flash; all the
time went into the USB half, which is board-specific.

**Pure headers with host tests.** `gamepad.h` and `joy_compare.h` build on
the development machine. The tests simulate Color BASIC's JOYSTK search, and
one of them caught a real bug before the board saw it: flipping a signed
16-bit Y axis by negation left full-up one short of the rail.

**Measuring instead of guessing.** Every pad layout in the code was taken
from the pad: a `PAD_PROBE` build printed the report descriptor at mount and
each report as it changed, and a one-control-at-a-time sweep named every
byte. The Android-mode button numbering matched the usual convention, but it
went into the code because the sweep said so, not because it was usual.

**Binding by what decodes.** The pad binds on the first interface whose
report its decoder accepts, not the first interface of a known device. That
one rule made the GameSir work in modes that put the gamepad on different
interfaces, and it keeps a keyboard's extra interfaces from ever being read
as a pad.

**Library logging, not inference.** The hub failure looked like "the hub
does nothing". The `pizero_usbdebug` build turned on TinyUSB's own log and
showed, request by request, that the hub was fine and a second hub chip
inside the same box was being refused. Two guesses would have been wrong.

**Telemetry that separates layers.** The `[pad]` line (reports a second, the
joystick values the machine holds) answered "is the pad broken?" in one
glance when the real problem was a wiped BASIC program.

**Trying the simple hardware.** A plain 4-port hub worked at once and proved
the whole path, which turned the chained USB-C hub from a blocker into a
low-priority bug.

**Reusing what was already there.** The keycap table came from the text
editor (`tek_ascii`), the Color BASIC chords for `[ ] \ _` from upstream
XRoar's translated mode, and the missing glyphs from the font the screens
had drawn with all along. None of the three had to be written from scratch.

**Giving each key source its own layer.** The keyboard, serial typing and
(next) pad buttons each hold keys in their own layer of the CoCo's key
matrix. Before, one source releasing a key could drop another's hold; the
keycap mapping, which forces SHIFT on and off, would have made that common.

## What went wrong, and the lessons

### 1. A power problem that was not (PIZERO-11b)

Devices enumerating only at cold boot was first put down to power. Nothing
had measured power. The real cause, found a month later with
instrumentation, was the line being pinned after enumeration.

**Lesson:** a diagnosis without a measurement is a guess. Write it down as
one.

### 2. Four hot-plug fixes against a hardwired VBUS (PIZERO-51)

Stopping and restarting the host, forcing a disconnect, and rebooting by
watchdog were each built, flashed and disproved. Each was plausible from the
software alone; the schematic, which shows VBUS hardwired with no switch,
was checked late. The detector also false-tripped on an idle composite
keyboard, and the reboot's marker was wiped by the SDK.

**Lesson:** read the schematic before designing a recovery that depends on
the hardware's cooperation.

### 3. Assuming the pad's identity (PIZERO-13)

The port assumed the pad would come up as a DualShock 4, because it did on
Fruit Jam. On this board it never did. It appeared as four identities:
Switch Pro (silent), GameSir's own with Xbox 360 data inside a vendor
report, Android mode (a standard HID gamepad), and on Fruit Jam the DS4. A
pad with automatic platform detection picks its identity from how the host
talks to it, so the same pad differs between boards.

**Lesson:** a device's identity is a measurement on each board, not a
property of the device.

### 4. A theory about restarts, disproved by the next boot (PIZERO-13)

When the pad fell back to Switch mode, the reset reason suggested RUN
restarts kept it powered and a real power-on would re-run its detection. The
next real power-on came up in Switch mode too. The difference was the Home
button reset done before the one good boot.

**Lesson:** one boot is an anecdote. Change one thing, and expect the next
observation to overturn the theory.

### 5. Missing the evidence that mattered (PIZERO-13, 54)

Several times the mount lines, which say what a device is, were printed
before the serial logger connected, and a power cycle had to be repeated
just to read them.

**Lesson:** start the logger before asking for a power cycle.

### 6. "The pad does not work", when the program was gone (PIZERO-54)

After a flash the user saw no joystick readings. The pad was fine; each
flash restarts the board and wipes the BASIC test program, and it had not
been retyped. It cost a debugging round and a telemetry build to see it.

**Lesson:** a flash resets the machine. Retype the test, or say it needs
retyping, every time. Better still, do not flash in the middle of a test.

### 7. A build flag that never took effect (PIZERO-11 to 54)

`-DCFG_TUH_HID=4` had been in `platformio.ini` since PIZERO-11. The
library's config defines the same names with no `#ifndef`, so it silently
won: the real value was 12 all along, and the one-hub limit could not be
raised by a flag either. The redefinition warnings were buried in hundreds
of lines of unrelated ones. `scripts/patch_tinyusb.py` now guards those
defines in the downloaded library.

**Lesson:** after setting a library's configuration by flag, check that the
value actually reached the code.

### 8. A hub that is two hubs (PIZERO-54, 159)

The USB-C hub turned out to be two hub chips chained, plus a built-in device.
With the hub limit raised it enumerated, but a device behind the second chip
hits a TinyUSB race: the first hub reports its built-in device during the pad's
2 ms settling delay, takes the shared control channel, and the pad's setup is
abandoned with no retry. It is in the latest library release.

**Lesson:** "hub" is not one thing. Test the simplest hardware first, and
treat each extra layer as a new case.

### 9. Loose ends noticed and not chased

A Keychron K2 behind the simple hub was never seen by the hub at all, while a
K3 on the same hub worked; its battery charging on a bus-powered hub is the
suspect, untested. Serial typing drops the first character after Enter, at any
speed tried (PIZERO-158); test programs were typed with a leading space to
get round it.

**Lesson:** record these as they happen, with the workaround, so they are
not rediscovered.

### 10. A POKE that "did not work" (PIZERO-166)

True lower case on the 6847T1 is switched on with `POKE 65314,16`. It did
nothing. The telemetry showed the register back at 00 even inside a running
program, and the obvious next step was to suspect the new code. A trace of
every write to the register settled it: at the prompt, BASIC rewrote it
about 110 times while reading keys and printing, back to 00 each time, and
a `PRINT` in a program reset it too. The emulation was right; a real CoCo
2B does the same. The answer was a setting (`lowercase`) rather than a fix.

**Lesson:** when a register "does not take", count the writes to it before
blaming the code that reads it.

### 11. A hang that was not in our code (PIZERO-203, 227)

Five watchdog recoveries landed in five different phases of the main loop,
so the phase markers said nothing. PIZERO-227 recorded the interrupted
program counter and exception number instead, and every hang since was in
the same place: the USB host's 1 ms timer interrupt, inside PIO-USB's bus
code, which busy-waits on the wire. The DAC stream (PIZERO-255) runs in the
same interrupt and makes it heavier.

**Lesson:** work done in an interrupt is invisible to markers in the main
loop. When a hang moves around, record where the CPU was, not which phase
it was in.

### 12. A device the computer remembered wrongly (PIZERO-287)

Adding the MIDI interface to the native port left the Mac seeing a device
with no interfaces, and only a BOOT-button replug cleared it: the board
re-enumerated before the new interface was added. `midi_usb_begin()` now
adds it first thing and forces a detach and re-attach. macOS also keeps
the old product name ("RP2350 PiZero") until it is deleted in Audio MIDI
Setup.

**Lesson:** when a USB device's descriptors change, force a re-enumeration,
and expect the host to cache names.

### 13. A library parser that garbled a note (PIZERO-287)

The TinyUSB MIDI stream parser, given a lone `$90` left by a program that
stopped mid-message, sent `90 10 3C` when the next note came. Our own
packer (`src/midi_usb_pack.h`, host-tested) follows MIDI's rules: an
unfinished message is dropped, never sent garbled.

**Lesson:** at a protocol boundary, own the byte-level rules and test them;
the less third-party code in the path, the fewer surprises.

## Still open

| Issue | What |
|---|---|
| PIZERO-203 | The hang in the USB host's timer interrupt. |
| PIZERO-51 | Hot-plug straight into the board. Needs switched VBUS; a hub may be the practical answer. |
| PIZERO-157 | Switch Pro handshake, so the pad works in any mode. |
| PIZERO-158 | Serial typing drops the first character of a line. |
| PIZERO-159 | Devices behind a chained USB-C hub never mount (TinyUSB race). |
| PIZERO-259 | The DAC stream's cost in the frame interrupt (a table-driven encoder, encoding outside the interrupt). |
| PIZERO-50 | A real CoCo joystick on the header pins. |

## Ideas for using USB further

USB has turned out to be the board's way to reach most of the outside
world, so these are worth keeping in view:

- **MIDI on the host port** (PIZERO-305, which meets the CoCo Zero
  standard's PIZERO-281): a class-compliant USB MIDI keyboard or synth,
  through the hub, as the MIDI Pak's in and out. Then **choosing MIDI ports**
  (PIZERO-306) when there are several.
- **Sound to the DAC and the video cable at once** (PIZERO-257): one ring,
  two readers on two clocks.
- **A second USB host on the native port** (PIZERO-283), once a debug probe
  does flashing and the log: hardware USB, its own connect detection (the
  first place to try hot-plug again), and less work on core 0.
- **A USB stick as a drive** (PIZERO-288's directory drives): a directory on
  a USB stick or the card behind Disk BASIC, so files move by plugging in.
- **Moving USB work out of the interrupt** (PIZERO-259, and the likeliest
  fix for PIZERO-203): encode packets in the main loop and only send them in
  the interrupt.

## Working rules this produced

- Start the serial logger before any power cycle, and keep it running.
- Never flash while the user is testing or playing; after a flash, the BASIC
  program is gone.
- Every new USB identity gets a probe build and a control-by-control sweep
  before a decoder is written.
- After setting a library option by build flag, prove it took (a log line,
  a symbol, or a test).
- When a device will not enumerate, turn on the library's own log
  (`pizero_usbdebug`) before theorizing.
- Keep diagnostic telemetry that separates the device from the machine
  (`[usb]` mount lines, `[pad]`).
- Try the simplest hardware (a plain hub, a direct connection) before
  debugging the complicated case.
- When a register or setting seems not to take, trace the writes to it
  (the `[vdg]` line's `pb_writes` and `pb_last` are the model).
- Improvements to the machine are on by default; authentic behavior stays
  one setting away, per game if need be (CLAUDE.md).
- When a hang moves between phases, record the interrupted PC and exception,
  not the phase.
- After changing what the native port presents, force a re-enumeration.
- Own the byte-level rules at a protocol boundary, with host tests, rather
  than a library's convenience parser.

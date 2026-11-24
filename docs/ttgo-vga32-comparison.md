# The CoCo Zero and TTGO-VGA32-COCO compared

A side-by-side of this project and Reinaldo Torres' (CoCo Byte Club)
[TTGO-VGA32-COCO](https://github.com/reyco2000/TTGO-VGA32-COCO), a CoCo 2 and CoCo 3 emulator on
the LilyGo TTGO VGA32 board (PIZERO-340).

**Sources.** Their side comes from their README at **v0.23.0 (2026-11-16)**, read on 2026-11-24,
and not from their code. Where their README contradicts itself, that is noted. Our side comes from
`README.md` and `SETTINGS.md`, checked against the source. Both projects move fast, so any line
here can go stale.

## In one paragraph

Both are CoCo 2 and CoCo 3 emulators on a small microcontroller board, both are built from or on
XRoar, both are developed with Claude Code, and both run at a paced 60 fps. They differ in what
they plug into. The TTGO is a **retro desk setup**: VGA monitor, PS/2 keyboard, PS/2 mouse as the
joystick, a mono jack for sound, and 4 MB of PSRAM, which buys it a CoCo 3 of up to 2 MB, the
6309, hard disks and DriveWire. The CoCo Zero is a **modern living-room setup**: HDMI-class digital
video with the sound in the same cable, USB keyboards, gamepads and MIDI, and a lot of sound
hardware built in, but only 520 KB of SRAM, so a 128 KB CoCo 3 until PSRAM lands (PIZERO-282).

## At a glance

| | CoCo Zero | TTGO-VGA32-COCO |
|---|---|---|
| Board | Waveshare RP2350-PiZero, 65 x 30 mm | LilyGo TTGO VGA32 v1.4 |
| MCU | RP2350B, dual Cortex-M33, run at 252 MHz | ESP32 (WROVER-E), dual core; clock not stated |
| Memory | 520 KB SRAM, 16 MB flash, no PSRAM (pad unpopulated) | 4 MB PSRAM, 4 MB flash |
| Emulator core | Vendored XRoar core, grown here (CoCo 3, faster 6809) | "Inspired on XRoar"; credits XRoar as original source, some GIME behaviour from MAME |
| Machines | CoCo 2, CoCo 2+, CoCo 3 128 KB, CoCo Zero (CoCo 3 with extras) | CoCo 2 (64 KB), CoCo 3 128 KB to 2 MB |
| CPU | 6809 | 6809 or HD6309 |
| Video out | DVI on the mini HDMI connector, 640x480p60 | VGA, 640x200 at 60 Hz (512x240 for some CoCo 3 screens) |
| Sound out | In the video cable (HDMI data islands), or a USB DAC | Internal 8-bit DAC, mono 3.5 mm jack |
| Keyboard | USB (any HID keyboard, through a hub) | PS/2, US or Spanish Latam layout |
| Joystick | USB gamepad, both joysticks, two buttons each | PS/2 mouse as joystick 1; joystick 2 a centred stub; keyboard-as-joystick |
| Storage | microSD, FAT32 | microSD, FAT32 |
| Disk images | .DSK (JVC), .DMK (DD, side 0) | .DSK (JVC), .VDK, .VHD hard disks |
| Menu | F9 to F12 lists, gamepad Home | F3 "supervisor" tile menu |
| Settings | Text files on the card, global and per game | NVS (flash), set from the menu |
| Toolchain | PlatformIO, earlephilhower arduino-pico | arduino-cli or Arduino IDE 2.x, ESP32 core 2.0.17, FabGL 1.0.9 |
| Network | None | WiFi: DriveWire client, debug HTTP API, MCP bridge |
| Licence | GPL (XRoar's GPL-3.0-or-later), `LICENSE` | GPL-3.0 |

## Hardware

**CoCo Zero.** An off-the-shelf Waveshare board the size of a Pi Zero. Video is bit-banged DVI from
PIO (`libdvi`) on GPIO 32 to 39, since the RP2350's HSTX serializer is not wired to the connector.
The one USB-C host port is PIO-USB at full speed. Everything the CoCo uses fits in on-chip SRAM,
which is the hard limit on the project: the CoCo 3 takes 64 KB of its RAM from the heap and
leaves about 8 KB free. There is a printable CoCo 2-shaped case.

**TTGO.** An off-the-shelf LilyGo board with VGA, two PS/2 ports, a microSD socket and an audio
jack already on it, so no wiring is needed either. It needs a VGA monitor, which is now a
specialist item, but takes the PS/2 keyboards and mice retro setups already have. The 4 MB of
PSRAM is what most of its larger features rest on: whole disk images cached in it, the CoCo 3's
extra memory, and room for 128 KB bank-switched cartridges.

## Machines and CPU

| | CoCo Zero | TTGO |
|---|---|---|
| CoCo 2 | Yes, stock and "2+" with extras | Yes, 64 KB |
| CoCo 3 | 128 KB | 128 KB, 512 KB, 1 MB, 2 MB |
| 6309 | No | Yes, boots NitrOS-9 for the 6309 |
| Switch machine | `machine =` in settings.txt, or per game: the board rebuilds in under 0.1 s without a restart | At boot from NVS, or live in Setup |
| CoCo 2 high-speed POKEs | Yes, 2x, with a clean screen | Not stated for the CoCo 2; the CoCo 3 runs 1.79 MHz |
| NitrOS-9 | Level 1 on the CoCo 2, Level 2 on the CoCo 3 (128 KB), from boot disks `scripts/mkos9disk.py` builds | Level 2 boots; testing on every memory size and both CPUs is on their TODO |

Their 512 KB-plus CoCo 3 is the single biggest functional gap on our side. It is exactly what
PSRAM is for here (PIZERO-282, `docs/coco-zero-2-proposal.md`).

## Video

| | CoCo Zero | TTGO |
|---|---|---|
| Signal | Digital, 640x480p60, in spec (252 MHz system clock, 25.2 MHz pixel clock) | Analogue VGA via FabGL, 640x200 at 60 Hz, 64 colours |
| CoCo 2 picture | 256x192 doubled to 512x384, centred, with border | Not stated in detail; 192-line screens show 4 border lines top and bottom |
| CoCo 3 picture | GIME's 640 pixels one for one; 225-line screens fit in 480 | 640x200, so 225-line 640-wide screens (NitrOS-9 80x25) drop one line in nine; 512-wide 225-line screens switch the monitor to 512x240, with a flicker and a memory cost |
| Artifact colours | CoCo 2: on, swapped, off (F8, saved per game) | PMODE 4: off, standard, reverse, 14 colours |
| Composite look | No | CoCo 3 RGB or composite monitor setting (37 colours); composite artifacts in RG6 only |
| Fonts | Several, including the 6847T2 with true lower case (default on the extras machines) | Not stated |
| Screenshot | Print Screen, to the card (640x480) | `/api/screenshot.png` over WiFi |

Our picture is the steadier one by construction: the output mode never changes, so there is no
monitor resync. Theirs has the composite monitor option, which we do not.

## Sound

| | CoCo Zero | TTGO |
|---|---|---|
| Output | Stereo over HDMI with the picture; a USB DAC at power-on instead | Mono, 8-bit internal DAC, I2S DMA at one sample a scanline (15,720.6 Hz) |
| Volume | Keyboard volume keys, 0 to 15, saved | Not stated |
| CoCo 6-bit DAC | Yes | Yes (implied) |
| Games Master Cartridge (SN76489) | Built in | Not mentioned |
| AY-3-8910 cartridges | CoCo PSG, Symphony-12 (four chips, stereo), Deluxe CoCo | AY-3-8913 in the Speech/Sound Cartridge only; "PSG music programs are not tested yet" |
| Speech (SP0256-AL2) | No | Yes, Speech/Sound Cartridge |
| Orchestra-90CC | Stereo, and Disk BASIC stays available | Mono, and disables Disk BASIC while on |
| MIDI Pak | Built in, to the host computer as a USB MIDI port, and (in progress) to USB MIDI devices on the host port (PIZERO-305) | No |
| All sources at once | Yes | Not stated |
| Latency | Not quoted here | About 50 ms behind the picture |

Sound is where the CoCo Zero is furthest ahead. The one thing they have that we do not is the
Speech/Sound Cartridge's speech chip.

## Input

| | CoCo Zero | TTGO |
|---|---|---|
| Keyboard | USB, keys type what is printed on them | PS/2, US or Spanish Latam; a Key Mapper drawn on a picture of the keyboard |
| Joysticks | USB gamepad: both joysticks, both buttons, every button remappable per game | PS/2 mouse as joystick 1 with a sensitivity setting; arrows and Space as a joystick |
| Hot-plug | Only through a hub (PIZERO-51) | Not an issue for PS/2 |
| Mouse | No | Yes, as the joystick |

Their keyboard-as-joystick mode and the mouse are cheap features we lack. Our second real joystick
and the gamepad's menu control are features they lack.

## Storage and software loading

| | CoCo Zero | TTGO |
|---|---|---|
| Floppy formats | .DSK, .DMK (double density, side 0) | .DSK, .VDK |
| Drives | 0 to 3, mount from F12 or `drives.txt`, lock read-only, new blank disk, rename | 0 to 3, quick-mount last disk (F6) |
| Disk writes | Straight into the image on the card | Images cached in PSRAM; dirty disks flushed on reset |
| Hard disk | No | Two .VHD images up to 512 MB at `$FF80`, RS-DOS or RGB-DOS |
| DriveWire | No | Becker port `$FF41`/`$FF42`, external server over WiFi (pyDriveWire, DW4, FujiNet-PC) |
| Cartridges | .ccc, banked carts on the CoCo 2 (40 KB set aside); none over 16 KB on the CoCo 3 | .ccc/.rom, 16 KB on the CoCo 2, banked to 128 KB on the CoCo 3 |
| Machine-code programs | .BIN list (F9) | Not mentioned |
| Cassette | No | Not mentioned |
| Autorun | Yes, `AUTORUN.md` | Not mentioned |
| List cap | 128 entries (`DSK_CAT_MAX`, `src/dsk_catalog.h:37`) | 128 entries |
| ROM checking | Missing ROMs named on screen | CRC-32 checked at startup |
| Card layout | `/coco/roms`, `/coco/dsk`, `/coco/bin`, `/coco/cart` | `/roms`, `/carts` or root |

Note the `$FF41` clash: their Becker port sits where our SN76489 lives (`sn76489 = off` gives it
back to the disk controller here), so if we ever add a Becker port, it and the built-in SN76489
cannot both be on at once.

## Menu and configuration

The CoCo Zero is configured in **text files** (`settings.txt`, a `NAME.TXT` beside each game,
`autorun.txt`, `drives.txt`), edited on the Mac or on the board in the F11 editor. Per-game
settings are the backbone: a game can carry its own machine, sound chips, artifact colours and
button map.

The TTGO is configured from **menus**, with everything saved in the ESP32's NVS flash: a grid of
eight tiles (Disks, Cart, Setup, WiFi, Reset, Debug, About, Resume). There is no per-game
setting in their README. It has an FPS overlay (F5), which our INFO page and serial telemetry cover
in a different way.

## Development and tooling

| | CoCo Zero | TTGO |
|---|---|---|
| Host tests | `pio test -e native`, with ROM-boot cases | Several `tools/*_test` programs (CPU, GIME render, RAM, IRQ, artifacts, VHD, audio) |
| Live debugging | Serial telemetry (`[run]`), `debug = on`, soak logs to the card | WiFi HTTP/JSON API, screenshots, an MCP bridge for agents |
| Stability record | Measured soaks, watchdog recovery, post-mortem on the card | Not stated |
| Tracker | `issues.jsonl`, `docs/ROADMAP.md` | GitHub Issues (7 shown) |
| Release | Build from source with PlatformIO | Prebuilt merged image, plus an SD-card bootloader update package |

Their **prebuilt firmware and SD-card updater** lower the barrier for a non-developer a long way.
We ask the user to install PlatformIO.

## Performance

Both claim real time at 60 fps on both machines. Ours: a CoCo 2 frame is about 9 ms of a 16.67 ms
budget, a CoCo 3 about 13 ms at the prompt and at most 15.2 ms in animation (README,
"Performance"). Theirs: "Every BASIC test screen holds 60 FPS on both CPUs", up from about 39 fps
at the prompt before v0.12.0; their README gives no whole-frame budget to compare against ours.

## What each has that the other lacks

**Only the CoCo Zero:** digital video with sound in the cable; USB keyboards, gamepads and MIDI;
two full joysticks; the SN76489, CoCo PSG, Symphony-12 and Deluxe CoCo sound chips; stereo
Orchestra-90 alongside Disk BASIC; the MIDI Pak; per-game text settings; autorun; .DMK; on-board
disk creation and renaming; CoCo 2 high-speed POKEs and fonts; a printable case; measured
stability.

**Only the TTGO:** a CoCo 3 up to 2 MB; the 6309; .VDK and .VHD hard disks; DriveWire over WiFi;
speech; a composite monitor mode; a PS/2 mouse; a visual key mapper; a WiFi debug API with an MCP
bridge; a prebuilt firmware and SD updater; ROM CRC checks.

## Ideas worth considering here

Item 1 is filed as PIZERO-341; the rest are not filed. In rough order of value for us:

1. **Prebuilt firmware (.uf2) on releases.** The RP2350 takes a .uf2 by drag-and-drop, which is
   simpler than their merged ESP32 image. Removes PlatformIO from the user's path entirely.
2. **A bigger CoCo 3, the 6309, hard disks.** All three follow from PSRAM, which PIZERO-282 is
   bringing up. Their README is a useful list of what PSRAM bought them, and of the traps
   (memory fragmentation at mode switches).
3. **Keyboard as joystick** (arrows and Space), for people with a keyboard and no gamepad.
4. **ROM CRC check at startup**, naming a bad dump rather than booting into a crash.
5. **DriveWire over the USB-C device port**, a serial link to a host DriveWire server, without WiFi.
6. **A composite monitor option for the CoCo 3**, the look many CoCo 3 games were drawn for.
7. **Speech/Sound Cartridge**: the SP0256 is a larger job; their README names the ROM files it needs.

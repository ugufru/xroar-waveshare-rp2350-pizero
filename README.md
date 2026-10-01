# CoCo Zero

**A Tandy Color Computer 2 on a board the size of a stick of gum.** Plug in a TV, a USB keyboard or
gamepad and a microSD card, and it boots straight to Color BASIC: picture and sound over one HDMI
cable, at a rock-steady 60 frames a second.

It is [XRoar](https://www.6809.org.uk/xroar/), the CoCo emulator, running on a
[Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm), a Raspberry Pi Zero sized
microcontroller board.

<p align="center">
  <img src="docs/images/board-wired.jpg" width="460"
       alt="The CoCo Zero running: a Waveshare RP2350-PiZero board powered up with USB-C power, a USB-host cable, a mini-HDMI-to-HDMI adapter, and a microSD card inserted">
</p>

## What it does

- **Full-speed CoCo.** Color, Extended and Disk BASIC, games and demos at real speed, in standard
  640×480 HDMI that any TV or monitor takes.
- **Sound over HDMI.** The CoCo's own sound, plus a three-voice SN76489 sound chip that Games Master
  Cartridge titles use, all through the same cable.
- **Pick a game from a menu.** Disks, programs and cartridges (bank-switched ones too) in on-screen
  lists. Highlight one, press ENTER, and it starts.
- **A gamepad is the joystick.** A USB pad works as both CoCo joysticks, and its Home button opens
  the menus, so you can play without a keyboard at all.
- **A keyboard that types what it says.** Any USB keyboard: `"`, `:`, `=` and `[ ]` are where the
  keycaps put them, Caps Lock switches case, and held keys repeat.
- **Settings in plain text,** for the whole machine or one game: button mapping, colors, fonts, key
  repeat and more. Edit them on a computer or on the CoCo Zero itself.
- **Better text.** A choice of fonts, including our 6847T2 with true lower case and a real
  underscore, caret and braces.
- **Screenshots.** Print Screen saves the screen as a PNG on the card.
- **Autorun.** Boot straight into a game, ready to play.
- **A printable case** styled after the CoCo 2.

## What you need

- A **Waveshare RP2350-PiZero** ([product page](https://www.waveshare.com/rp2350-pizero.htm)).
- A **mini-HDMI to HDMI** cable or adapter, and any HDMI TV or monitor.
- A **USB keyboard, a USB gamepad, or both** through a simple USB hub. The USB port is
  full-speed only (USB 1.1): ordinary keyboards, wireless receivers and most gamepads work.
- A **microSD card** (FAT32) with your own CoCo ROMs. Only `bas12.rom` is required.
- **USB-C power**, and [PlatformIO](https://platformio.org/install) on a computer to build and
  flash the firmware.
- *Optional:* the printable [case](hardware/case/README.md).

> **ROMs are not included.** Color, Extended and Disk BASIC are © Microsoft and Tandy and may not be
> redistributed. Use dumps from hardware you own.

<p align="center">
  <img src="docs/images/connections.jpg" width="420"
       alt="The board's connector edge: two USB-C connections (USB host and power) and a mini-HDMI-to-HDMI adapter">
  <br>
  <em>Two USB-C ports (USB host, and power) and the mini-HDMI adapter.</em>
</p>

## Quick start

1. **Flash** the firmware: `pio run -t upload`. If the upload can't reset the board, hold
   **BOOT** while plugging in USB-C.
2. **Fill the card.** ROMs go in `/coco/roms/` (`bas12.rom`, and optionally `extbas11.rom` and
   `disk11.rom`). Games go in `/coco/dsk` (`.dsk`), `/coco/bin` (`.bin`) and `/coco/cart` (`.ccc`).
3. **Connect and power on**: HDMI, keyboard or gamepad, the card, then USB-C power. You get the
   `OK` prompt in a few seconds. If the card or a ROM is missing, the screen says what to copy where.
4. **Play.** Press **F12** for disks, **F9** for programs or **F10** for cartridges (or **Home**
   on the gamepad), pick a game and press **ENTER** (or **A**).

To start a game automatically at power-on, see [`AUTORUN.md`](AUTORUN.md). Every setting is in
[`SETTINGS.md`](SETTINGS.md).

## Controls

| To | Keyboard | Gamepad |
|---|---|---|
| Open the lists | **F12** disks, **F9** programs, **F10** cartridges, **F11** files, **F1** info | **Home** |
| Move, switch list | arrows, PgUp/PgDn, Home/End | D-pad, L1/R1 |
| Start the highlighted game | **ENTER** | **A** |
| Go back, close | **ESC** (or the list's own F key) | **B** (or Home) |
| Edit that game's settings | **TAB** | **X** |
| Put a disk in drive 0 to 3 | **0** to **3** | |
| Save a screenshot | **Print Screen** | **M** (GameSir pads) |

**In BASIC:** Caps Lock switches upper and lower case, Home is CLEAR, Esc is BREAK, and Backspace
deletes. The CoCo has no `{ } | ~` or backtick keys, so those do nothing.

**In a game, on the gamepad:** the left stick (or D-pad) is the right joystick, the one nearly all
software reads, and the right stick is the left joystick. A, B and R1 fire the right joystick; X
and L1 fire the left one. Start is ENTER and the top button is SPACE. Every button can be remapped,
per game too (`pad_start = s`, `dpad = arrows`).

**On the board:** RUN restarts to the BASIC prompt, skipping autorun. BOOT is only for flashing.

**F1** shows the firmware version, the chip, the board's serial number, the clock, uptime, free
memory and USB devices.

## Status

Everything above works on hardware. Known gaps:

- **Hot-plugging:** straight into the board, a USB device must be attached at power-on. Through a
  hub, one plugged in later can appear (PIZERO-51).
- **Disks are read-only**, so `SAVE` to disk fails.
- **High-speed POKEs** (SAM double speed) are accepted but ignored; the machine always runs at
  normal speed (PIZERO-132).
- **Multi-chip USB-C hubs** don't work yet; a simple hub does (PIZERO-159).
- **Gamepads:** tested with a GameSir Tegenaria Lite in its Android and automatic modes. Its
  Switch mode sends nothing (PIZERO-157).

**Stability, measured (`PIZERO-98`, 2026-09-17/18).** A soak of the default build logged 13 h of
wall clock, about 7.5 h of it observed (the host slept through the rest). Video: 0.38 short sync
windows an hour and no second-long dropouts, against the Fruit Jam port's 7.6 dropouts an hour.
Freezes: about 0.46 an hour, all in the emulation phase, every one recovered by the watchdog
(`PIZERO-33`). A soak of the current firmware is `PIZERO-168`.

How USB input got here, and the lessons along the way: [`docs/usb-retrospective.md`](docs/usb-retrospective.md).
Open work is in `issues.jsonl` (use `/issues`), sequenced in [`docs/ROADMAP.md`](docs/ROADMAP.md).

<details>
<summary>Development phases</summary>

| Phase | Goal | Issues | Status |
|---|---|---|---|
| 0 | Decisions + scaffolding | PIZERO-01..03 | ✅ done |
| 1 | HDMI bring-up: DVI test pattern | PIZERO-04..05 | ✅ done |
| 2 | XRoar boots to Color BASIC "OK" on HDMI | PIZERO-06..09 | ✅ done |
| 3 | Autonomous self-running demo | PIZERO-10 | ✅ done |
| 4 | USB-host keyboard / joystick input | PIZERO-11/11a/11b/12/13/54 | 🟡 keyboard, gamepad and hub done; hot-replug open |
| 5 | Dual-core split + performance | PIZERO-14..15 | ✅ done |
| 6 | HDMI audio over the existing cable (CoCo 6-bit DAC + 1-bit sound, and the SN76489) | PIZERO-18, 26–35, 38/39, 143 | ✅ done: streaming per-active-line delivery (warble fixed) |
| 7 | Clean audio + stability | PIZERO-33 (watchdog), PIZERO-35/38 (delivery re-arch) | ✅ done |
| 8 | True in-spec 640×480p60 + audio + USB at 252 MHz | PIZERO-44/45 | ✅ done: now the default build |

</details>

# How it works

The rest of this README is for people who want to know how it is built.

## Target hardware

The RP2350-PiZero is a Raspberry Pi Zero form-factor board built around the **RP2350B**:

- **MCU**: RP2350B (dual Cortex-M33 / dual Hazard3 RISC-V), 48 GPIO, 150 MHz stock (overclockable).
- **Memory**: 520 KB on-chip SRAM, 16 MB flash. A PSRAM pad exists on the PCB but is **not populated**, so treat this as an SRAM-only target.
- **Display**: mini-HDMI connector carrying a DVI signal, driven from GPIO via PIO (see below).
- **Input**: a dedicated PIO-USB Type-C port usable as a USB 1.1 host (a second Type-C is power/programming).
- **Storage**: microSD slot on SPI.

### Pinout (confirmed against the Waveshare schematic and demo source)

| Function | GPIO | Notes |
|---|---|---|
| HDMI TMDS data 2 (±) | 32 / 33 | DVI driven by PIO `libdvi` |
| HDMI TMDS data 1 (±) | 34 / 35 | |
| HDMI TMDS data 0 (±) | 36 / 37 | |
| HDMI TMDS clock (±) | 38 / 39 | |
| HDMI DDC / CEC | 44 + others | not needed for video output |
| microSD SCK | 30 | SPI, ~12.5 MHz |
| microSD MOSI | 31 | |
| microSD MISO | 40 | |
| microSD CS | 43 | software chip-select |
| microSD card-detect | 22 | |
| USB host D+ / D− | 28 / 29 | PIO-USB; D− is always D+ +1 |
| I²C0 SDA / SCL | 6 / 7 | |
| UART0 TX / RX | 0 / 1 | |
| WS2812 status LED | 2 | |

The reference DVI configuration is the upstream `pico_sock_cfg` (`invert_diffpairs = false`,
`pio_set_gpio_base(pio, 16)` because the TMDS pins are above GPIO 31).

## How the HDMI output works

The mini-HDMI connector is wired **directly to RP2350 GPIOs** through series resistors; there is no
HDMI transmitter chip. DVI carries video as four TMDS differential pairs (clock + 3 data lanes = 8
wires), and each pair is produced by two adjacent GPIOs driven in opposite polarity.

The RP2350 has two ways to generate that high-speed TMDS bitstream:

- **PIO `libdvi`** (Wren6991/PicoDVI): PIO state machines + DMA do the TMDS encoding in software.
- **HSTX**: a dedicated hardware serializer, but it is hardwired to **GPIO 12–19 only**.

On this board the HDMI connector is on **GPIO 32–39**, so **HSTX cannot drive it**; `libdvi` (PIO) is
the only option. This matches Waveshare's own reference demos, which use `libdvi` on this board.

For the full end-to-end signal path (emulated CoCo screen and sound all the way to the HDMI pins,
including how audio rides inside the TMDS stream as data islands), see
[`docs/pipeline.md`](docs/pipeline.md).

### Display geometry

A literal 640×480 RGB565 framebuffer would be ~614 KB and does not fit in 520 KB SRAM alongside the
64 KB of CoCo RAM and ROM images. Instead the framebuffer is **320×240 RGB565 (~154 KB)** and `libdvi`
scans it out **pixel- and line-doubled to 640×480p 60 Hz** in hardware (`DVI_VERTICAL_REPEAT = 2`).

The CoCo's native 256×192 is centered inside the 320×240 buffer (32 px left/right, 24 px top/bottom
border). After the 2× hardware scale-out it appears on the monitor as **512×384 with blank borders**,
which is the "2× resolution" target. The blitter writes 320×240 in landscape with no rotation.

## System-clock reconciliation (HDMI + USB host): resolved

DVI and PIO-USB want different system clocks: DVI's TMDS bit clock prefers ~252 MHz for
640×480p60 (25.175 MHz pixel), while Pico-PIO-USB asserts the CPU is *exactly* 120 MHz or
240 MHz. The original bring-up (`PIZERO-02b`) sidestepped this by running at **240 MHz** with an
off-spec 24 MHz pixel clock (~52–57 Hz refresh). **`PIZERO-44`/`PIZERO-45` resolved it properly:
at 252 MHz the PIO-USB clock dividers come out *exact* (252/48 = 5.25), so USB and a standard
25.2 MHz pixel clock coexist.** The default build now runs **252 MHz → 25.2 MHz pixel, 800×525 =
true 60.0 Hz**: standard, monitor-friendly 640×480p60 with correct game speed and USB host all at
once. The emulator is paced from `FRAME_PERIOD_US` (16.67 ms) to match. (The old 240 MHz/~52 Hz
timing is kept only as a fallback env, without the CoCo 3-style palette and timer; see
[`docs/BUILD.md`](docs/BUILD.md).)

## Software architecture

```
┌──────────────────────────────────────────────────────────┐
│  core 0  emulation + VDG render + blit + USB + SD         │
│  core 1  DVI scanout (libdvi) + HDMI audio encode         │
├──────────────────────────────────────────────────────────┤
│  lib/coco_machine   CoCo bus glue + minimal FDC  (reused) │
│  lib/xroar_core     vendored XRoar core          (reused) │
├──────────────────────────────────────────────────────────┤
│  libdvi             PIO DVI driver (Wren6991/PicoDVI)     │
│  Pico-PIO-USB + Adafruit TinyUSB   USB host              │
│  no-OS-FatFS-SD     microSD over SPI                     │
└──────────────────────────────────────────────────────────┘
```

The XRoar core (`lib/xroar_core`) and the CoCo glue (`lib/coco_machine`) are board-agnostic and are
reused unchanged from the AMOLED port; only the RAM allocation (no PSRAM here) and the display blitter
need adapting. Everything below the line (DVI, USB, SD) is board-specific and built on Waveshare's
proven reference stack for this board (earlephilhower arduino-pico core).

## Building from source

PlatformIO with the earlephilhower arduino-pico core, targeting the RP2350B.
**See [`docs/BUILD.md`](docs/BUILD.md)** for the full env + build-flag matrix and
toolchain gotchas. In short:

```
pio run                 -t upload   # DEFAULT: true 640x480p60 + HDMI audio + USB
pio run -e pizero_stream -t upload  # fallback: older ~52 Hz timing, for a picky display
pio device monitor                  # serial @ 115200: prints per-second [run] fps/cpu/blit
```

A bare `pio run` builds the default `pizero_stream_60` env (60 Hz, streaming HDMI
audio, USB host). The off-spec 52 Hz `pizero_stream` is kept as the fallback for
displays that reject 60 Hz. Don't enable flags via the `PLATFORMIO_BUILD_FLAGS` env var: it links
stale objects (see BUILD.md §4b).

A microSD card is required, with the CoCo ROMs at **`/coco/roms/bas12.rom`** (and optionally
`extbas11.rom` and the `disk11.rom` cartridge beside it), plus optional disks, programs,
cartridges, `settings.txt` and `autorun.txt` (see [`AUTORUN.md`](AUTORUN.md)).

## Performance

The headline result: **Color BASIC runs at a locked 60 fps**, full real-time, on a 252 MHz system
clock driving standard 640×480p60, with about a fifth of the core-0 frame budget still free even
with USB-host servicing (keyboard and gamepad) running on core 0.

Per-frame work on core 0, against the 60 Hz frame period of **16.67 ms** (from the `[run]`
telemetry of the default build, 2026-09-28):

| Work | Time | Notes |
|---|---|---|
| CoCo emulation | ~11 ms | ~14,900 6809 cycles/frame at the emulated ~0.895 MHz, plus USB host |
| `render_frame` (alpha/text) | ~0.5 ms | precomputed glyph-row → packed-word LUT |
| Blit to framebuffer | ~1.6 ms | 320×240 RGB565, landscape, no rotation |
| **Total** | **~13 ms** | ~3.5 ms (about 20%) headroom per frame |

How we got from the first boot (54 fps) to a locked 60 fps:

- **Paint the static border once** at init instead of re-clearing it every frame, saving ~27K
  redundant pixel writes per frame (`src/coco_boot.cpp`).
- **Glyph-row → packed-32-bit-word render LUT** for alpha (text) mode, replacing per-pixel
  read-modify-write: **~6.4 ms → ~0.56 ms** per frame. (Graphics modes still use the per-pixel path.)
- **Double buffering** (`PIZERO-14`): two 320×240 RGB565 buffers with a `volatile` front-buffer
  handoff from core 0 to core 1, so `libdvi` never samples a half-rendered frame. Costs **96.8% RAM**
  (507,340 / 524,288 bytes) in the double-buffered envs `pizero` and `pizero_60hz`, since retired
  (PIZERO-150). The default
  `pizero_stream_60` **single**-buffers instead (the HDMI data islands need the framebuffer's
  ~150 KB) and sits at **75.7%** of RAM (396,892 bytes), leaving ~127 KB free. Measured 2026-09-28.

Performance instrumentation, clock, and vreg tuning landed in `PIZERO-15`; the serial monitor prints
per-second `[run]` fps/cpu/blit stats. For comparison, the AMOLED port manages ~15 fps (~36%
real-time), so this is roughly a **4× improvement**.

The CoCo's **high-speed POKEs** (SAM double speed, `POKE 65495,0` / `POKE 65497,0`) are accepted but
currently have no effect: every memory access is charged the normal speed, so the machine always
runs at 1× (`PIZERO-132`). What honoring them would cost against the frame budget is worked out in
[`docs/cpu-speed.md`](docs/cpu-speed.md).

Note the two distinct clocks: the host RP2350 MCU runs at **252 MHz** (set from the DVI TMDS bit
clock), while the *emulated* 6809 runs at its authentic **~0.895 MHz**, independent of the host clock.

## Case

A two-part 3D-printed case lives in [`hardware/case/`](hardware/case/README.md),
styled after the ventilated top of a Tandy Color Computer 2. It is 69.8 x 34.8 x
18.6 mm, prints without supports, and closes with four M2.5 screws that pass
through the board's own mounting holes so one set of fasteners both clamps the
board and shuts the case. Openings for mini-HDMI, both USB-C ports, the microSD
slot and the battery connector, with the RUN and BOOT buttons reachable through
the vent slots.

The source is a single parametric OpenSCAD file, so every dimension is a named
parameter and the STLs are rendered from it rather than drawn. The board
geometry was scaled off Waveshare's published dimension drawing rather than
measured by hand. [`hardware/case/README.md`](hardware/case/README.md) has the
full account, including what had to be changed after each test print and the one
detail that turned out not to be printable.

Note that a stock Raspberry Pi Zero shell fits the outline and mounting holes but
**not the ports**: this board uses two USB-C where a Pi Zero has micro-USB, in
different positions, and adds a battery connector and a debug header.

## References

- Waveshare RP2350-PiZero wiki: https://www.waveshare.com/wiki/RP2350-PiZero (board schematic mirrored at `docs/RP2350-PiZero-schematic.pdf`)
- USB-host reference: [ugufru/waveshare-rp2350-usb-a](https://github.com/ugufru/waveshare-rp2350-usb-a)
- Upstream DVI driver: [Wren6991/PicoDVI](https://github.com/Wren6991/PicoDVI)
- USB host stack: [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) + Adafruit TinyUSB

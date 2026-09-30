# XRoar on the Waveshare RP2350-PiZero

A port of the **XRoar** Tandy Color Computer (CoCo) emulator to the
[Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm), driving a
**mini-HDMI display** with **USB-host keyboard and gamepad** input. We call the
finished machine the **CoCo Zero**.

This started as an evaluation port: could the RP2350-PiZero run XRoar at least as well as an earlier
RP2350-Touch-AMOLED-1.8 port (which boots Color BASIC to "OK" at ~36% real-time)? It does,
comfortably. It boots Color BASIC over HDMI at a **locked 60 fps** (standard **640×480p60**), full
real-time, with a real **USB keyboard typing directly into BASIC**, a **USB gamepad as the CoCo's
joysticks**, and **CoCo audio played over the same HDMI cable**. The one input gap left is
hot-plugging: straight into the board, a USB device must be attached at power-on (PIZERO-51).

<p align="center">
  <img src="docs/images/board-wired.jpg" width="460"
       alt="Waveshare RP2350-PiZero running XRoar: board powered up (status LED lit) with USB-C power, a USB-host cable, a mini-HDMI-to-HDMI adapter, and a microSD card inserted">
</p>

## Goal

- Output to HDMI at 2× the CoCo's native resolution, at 30 fps or better.
- USB host for a real keyboard and joystick.
- CoCo emulation, VDG render/blit, and USB/SD servicing on core 0; `libdvi` DVI scanout owns core 1.

## What you need

- **Waveshare RP2350-PiZero** board ([product page](https://www.waveshare.com/rp2350-pizero.htm)).
- **A mini-HDMI display connection:** a mini-HDMI→HDMI cable, or a mini-HDMI→HDMI
  adapter plus a standard HDMI cable, into any HDMI monitor/TV. (The board's HDMI
  port is the *mini* size.) The signal is standard 640×480p60 (4:3).
- **A USB keyboard, a USB gamepad, or both** through a simple USB hub, plus
  whatever adapter reaches the board's **USB-host** port (the PIO-USB Type-C;
  the *other* Type-C is power/programming). The host is **USB 1.1 only**: simple
  wired keyboards, full-speed wireless receivers and most gamepads work;
  high-speed-only USB 2.0 peripherals do not enumerate (see Status).
- **A microSD card** (FAT32) holding the CoCo ROMs you supply (see below). Required.
- **USB-C power** to the power/programming port.
- *Optional:* a **case**. There is a printable one in
  [`hardware/case/`](hardware/case/README.md), or any Raspberry Pi Zero shell
  will bolt on (the board shares the Pi Zero outline and hole pattern) but will
  not line up with this board's ports.
- For building/flashing: a host PC with **[PlatformIO](https://platformio.org/install)**
  (Core CLI or the VS Code extension).

<p align="center">
  <img src="docs/images/connections.jpg" width="420"
       alt="The board's connector edge: two USB-C connections (USB-host keyboard cable and USB-C power) and a mini-HDMI-to-HDMI adapter">
  <br>
  <em>The connections: two USB-C (USB-host keyboard + power) and the mini-HDMI→HDMI adapter.</em>
</p>

> **ROMs are not included.** Color/Extended/Disk BASIC are © Microsoft/Tandy and
> are not redistributable. You must supply your own dumps (from hardware you own).
> Only `bas12.rom` is strictly required; see [`AUTORUN.md`](AUTORUN.md) for the
> full SD-card layout.

## Quick start

1. **Build & flash** the firmware (default env is the 60 fps + audio + USB build):
   ```bash
   pio run -t upload          # build + flash; see docs/BUILD.md for envs/flags
   ```
   If the upload can't reset the board, hold **BOOT** while plugging in USB-C.
2. **Prepare a microSD** (FAT32): copy your ROMs to `/coco/roms/` as
   `bas12.rom` (and optionally `extbas11.rom`, `disk11.rom`); plain `/coco/`
   also works. Add an optional `/coco/autorun.txt` to auto-load disks or
   programs: see [`AUTORUN.md`](AUTORUN.md). If the card or a ROM is missing,
   the screen says so and what to copy where.
3. **Connect & power on**: mini-HDMI → monitor, USB keyboard and/or gamepad →
   host port, insert the microSD, then apply USB-C power. The CoCo boots to the
   BASIC `OK` prompt; type directly on the keyboard. `pio device monitor`
   (115200) shows `[run]` telemetry over serial.
4. **F9 to F12 and F1: the lists.** Each key pauses the machine and opens a
   list over it: **F12** disks (`.dsk` in `/coco/dsk`), **F9** programs (`.bin`
   in `/coco/bin`), **F10** cartridges (`.ccc` in `/coco/cart`: 2 to 16 KB, or
   larger bank-switched ones), **F11** files (`settings.txt`, `autorun.txt`)
   and **F1** info. Each list also looks in `/coco`. Another F key switches
   list, and the key of the list on show closes it; **Left/Right** step
   through the lists too. **Up/Down**, **PgUp/PgDn** and **Home/End** move.
   **ESC** goes back to the running program untouched.
   - *Disks:* **0** to **3** put the highlighted disk in that drive, or take it
     out if it is already there (the first four columns show which drives
     hold it). **ENTER** restarts with it in drive 0 and runs its first program.
     Disks are read-only for now.
   - *Programs:* **ENTER** restarts and runs the `.bin` directly.
   - *Cartridges:* **ENTER** plugs it in and restarts into it.
   - **TAB** on a disk, program or cartridge edits its own settings file
     (see item 6).
   - *Info:* the firmware version and build, the chip and its revision, the
     board's serial number, the clock, uptime, free memory and USB devices.
5. **Three-voice sound: an SN76489 at `$FF41`.** The sound chip from the Games
   Master Cartridge (three square-wave tones, noise, 16 volume steps) is
   always there, not only with a GMC plugged in, so GMC music works and your
   own programs can use it: `POKE &HFF41,&H9F` silences tone 1, for example.
   Bank-switched cartridges over 16 KB (Games Master Cartridge style) load
   from the cartridge list too.
6. **Settings: `/coco/settings.txt`.** Sound chip and volume, artifact colors,
   the text font and true lower case, key repeat, what each gamepad button
   does, the CoCo 3-style extras, RUN and serial typing behavior, and any of
   the palette colors, one `name = value` per line. Edit it on the CoCo Zero
   itself (**F11** for the files list, ENTER on `SETTINGS.TXT`, Ctrl-S to
   save and apply) or on a computer. A game can have its own: `ORBIT.TXT`
   beside `ORBIT.BIN`, `ORBIT.CCC` or `ORBIT.DSK` overrides settings.txt while
   that game runs (**TAB** on it in its list). See [`SETTINGS.md`](SETTINGS.md).
7. **USB gamepad as the CoCo joysticks.** Plug a pad in instead of the
   keyboard, or both through a simple USB hub, and switch the power on. (A
   multi-port USB-C hub with several hub chips inside does not work yet,
   PIZERO-159.) The left stick or D-pad is the right joystick, the one nearly all
   software reads; the right stick is the left joystick. The bottom or right
   face button, or R1, is the right fire button; the left face button or L1 is
   the left one. Start is ENTER and the top face button SPACE, and every
   other button, and the D-pad, can be set to any CoCo key, per game too
   (`pad_start = s`, `dpad = arrows`: see [`SETTINGS.md`](SETTINGS.md)).
   The pad's **Home** button opens the lists too: D-pad to move, A to start
   the highlighted game, B to go back, L1 and R1 to page, and X to edit the
   game's own settings, so the pad alone is enough to play.
   `PRINT JOYSTK(0)` reads 0-63. Tested with a GameSir
   Tegenaria Lite in Android mode (hold Home + A), and in its automatic mode;
   its Switch mode (Home + Y) sends nothing and does not work. Straight into
   the board, a pad must be attached at power-on; through a hub, one plugged
   in later can appear (PIZERO-51). How USB input got here, and the lessons:
   [`docs/usb-retrospective.md`](docs/usb-retrospective.md).
8. **A USB keyboard types what its keycaps say.** `"` is Shift+`'`, `:` is
   Shift+`;`, `=` and `+` are where they are printed, and `[ ] \ _` work at
   the BASIC prompt (the CoCo types them with SHIFT and an arrow or CLEAR,
   which the keyboard does for you). **Caps Lock** is the CoCo's upper and
   lower case toggle, **Home** is CLEAR, **Esc** is BREAK, and Backspace
   deletes. The numeric keypad types its characters. The CoCo has no
   `{ } | ~` or backtick keys, so those keys do nothing. Shift with a letter is
   still the CoCo's other case, and Shift alone is still SHIFT, for games.
   Held keys auto-repeat (`key_repeat` in [`SETTINGS.md`](SETTINGS.md)).
9. **Print Screen saves a screenshot** as a PNG in `/coco/screendumps/`
   (`SCR0001.PNG`, `SCR0002.PNG`, ...), exactly what is on the screen,
   border and any open list included. The GameSir pad's M button does the
   same, and so does Ctrl-P sent over the USB serial port (with
   `serial_keyboard = on`, the default). The machine pauses for about a fifth
   of a second while the card is written.

Full build details, the env/flag matrix, and toolchain notes are in
[`docs/BUILD.md`](docs/BUILD.md).

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

## Roadmap

Work is tracked in `issues.jsonl` (use `/issues` to list). Phases:

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

USB keyboard, gamepad and hub verified on hardware (`docs/usb-retrospective.md`); the remaining
open work in Phase 4 is hot-replug (`PIZERO-51`): straight into the board, a device must be
attached at power-on.

**Stability, measured (`PIZERO-98`, 2026-09-17/18).** A soak of the default build logged 13 h of
wall clock, about 7.5 h of it observed (the host slept through the rest). Video: 0.38 short sync
windows an hour and no second-long dropouts, against the Fruit Jam port's 7.6 dropouts an hour.
Freezes: about 0.46 an hour, all in the emulation phase, every one recovered by the watchdog
(`PIZERO-33`). A soak of the current firmware is `PIZERO-168`.

**Phase 6/7: HDMI audio (working).** XRoar's 6-bit DAC + single-bit sound, and the SN76489 sound
chip (`PIZERO-143`), are encoded as HDMI
**data-island audio-sample packets** by an extended `libdvi` and played over the existing cable.
CoCo `SOUND`/`PLAY`/game audio is recognizable and **pitch-matched to desktop XRoar**. The original
bursty per-line scheme warbled; `PIZERO-38` re-architected delivery to **stream one metered audio
island onto every active line** (encoded a line ahead in the core-1 DMA IRQ, per Shuichi Takano's
reference; our packet encoder is ported from his `pico_lib`), which fixed the warble. `PIZERO-33`
added a core-1 watchdog that auto-recovers a wedged board. A small residual fidelity gap vs desktop
XRoar remains and is **source-side** (the resampler, tracked in `PIZERO-41`), not delivery.

**Phase 8: true 60 Hz (`PIZERO-44`/`PIZERO-45`).** Running at 252 MHz makes the PIO-USB dividers
exact, so standard **640×480p60** video, a correct 60 Hz game speed, USB host, and streaming audio
all coexist. This is now the **default build**. The off-spec 240 MHz/~52 Hz timing is retained only
as a fallback env. Remaining audio polish: `PIZERO-41` (source-side fidelity) and `PIZERO-32`
(the ACR CTS value is sink-dependent, a multi-monitor robustness item). The wavetable synth
experiment (`PIZERO-17`) remains a stretch.

## Build

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

## Status

**Phases 0–8 complete except USB hot-plug (Phase 4, `PIZERO-51`).** The default build boots
Color BASIC over HDMI at **standard 640×480p60**, a locked 60 fps with correct game speed, with a
**USB keyboard typing directly into BASIC** (`PIZERO-11`/`12`, keycaps since `PIZERO-163`), a **USB
gamepad as the joysticks** (`PIZERO-13`), keyboard and pad together through a hub (`PIZERO-54`), and
**CoCo audio over the HDMI cable**. The autonomous `autorun.txt` loader (`PIZERO-10`, see
[`AUTORUN.md`](AUTORUN.md)) works. Every shipping build single-buffers (RAM goes to the audio-island
buffers), so mild tearing is possible; only the `pizero_wavmeas` diagnostic keeps two framebuffers.

**HDMI audio works** (`PIZERO-30`/`38`): CoCo `SOUND`/`PLAY`/game sound plays over the HDMI cable
via `libdvi` data-island packets, **pitch-matched to desktop XRoar**. The early bursty delivery
warbled; `PIZERO-38` switched to streaming one metered island per active line, which fixed it. A
small residual fidelity gap vs desktop remains and is **source-side** (the resampler, `PIZERO-41`).
Also fixed along the way: **`PIZERO-31`**: the CoCo's 60 Hz field-sync timer IRQ was never enabled
in this port, so `PLAY`, the cursor blink, and `SOUND n,d` hung forever; now they work.

Remaining open work: **`PIZERO-51`** (hot-plug straight into the board: the software fixes were
disproved, because VBUS is hardwired; through a hub a device plugged in later can appear). Audio
polish: **`PIZERO-41`** (source-side fidelity) and **`PIZERO-32`** (the ACR CTS value is
sink-dependent, a multi-monitor robustness item).

USB device-compatibility caveat: Pico-PIO-USB is USB 1.1 only, so high-speed-only USB 2.0
peripherals (some keyboards and gaming mice) don't enumerate. Simple wired USB keyboards,
full-speed wireless USB receivers and most gamepads are the working class.

Note: 640×480 is 4:3, so 16:9 monitors stretch it unless set to 4:3/aspect scaling.

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

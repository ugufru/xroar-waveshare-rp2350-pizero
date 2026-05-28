# XRoar on the Waveshare RP2350-PiZero

A port of the **XRoar** Tandy Color Computer (CoCo) emulator to the
[Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm), driving a
**mini-HDMI display** with **USB-host keyboard/joystick** input.

This is an evaluation port: the goal is to see whether the RP2350-PiZero can run XRoar at least
as well as the existing [RP2350-Touch-AMOLED-1.8 port](../coco-rp2350-waveshare-touch-amoled-18)
(which boots Color BASIC to "OK" at ~36% real-time), while adding HDMI output and real USB input.

## Goal

- Output to HDMI at 2× the CoCo's native resolution, at 30 fps or better.
- USB host for a real keyboard and joystick.
- CoCo emulation on core 0; video, keyboard, and SD-card access on core 1.

## Target hardware

The RP2350-PiZero is a Raspberry Pi Zero form-factor board built around the **RP2350B**:

- **MCU**: RP2350B (dual Cortex-M33 / dual Hazard3 RISC-V), 48 GPIO, 150 MHz stock (overclockable).
- **Memory**: 520 KB on-chip SRAM, 16 MB flash. A PSRAM pad exists on the PCB but is **not populated** — treat this as an SRAM-only target.
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

The mini-HDMI connector is wired **directly to RP2350 GPIOs** through series resistors — there is no
HDMI transmitter chip. DVI carries video as four TMDS differential pairs (clock + 3 data lanes = 8
wires), and each pair is produced by two adjacent GPIOs driven in opposite polarity.

The RP2350 has two ways to generate that high-speed TMDS bitstream:

- **PIO `libdvi`** (Wren6991/PicoDVI) — PIO state machines + DMA do the TMDS encoding in software.
- **HSTX** — a dedicated hardware serialiser, but it is hardwired to **GPIO 12–19 only**.

On this board the HDMI connector is on **GPIO 32–39**, so **HSTX cannot drive it** — `libdvi` (PIO) is
the only option. This matches Waveshare's own reference demos, which use `libdvi` on this board.

### Display geometry

A literal 640×480 RGB565 framebuffer would be ~614 KB and does not fit in 520 KB SRAM alongside the
64 KB of CoCo RAM and ROM images. Instead the framebuffer is **320×240 RGB565 (~154 KB)** and `libdvi`
scans it out **pixel- and line-doubled to 640×480p 60 Hz** in hardware (`DVI_VERTICAL_REPEAT = 2`).

The CoCo's native 256×192 is centred inside the 320×240 buffer (32 px left/right, 24 px top/bottom
border). After the 2× hardware scale-out it appears on the monitor as **512×384 with blank borders** —
which is the "2× resolution" target. The blitter writes 320×240 in landscape with no rotation.

## Known feasibility risk: system-clock conflict (HDMI + USB host)

The single biggest open question for this board is whether HDMI and USB host can run **at the same
time**. The two stock Waveshare demos run at incompatible system clocks:

- **DVI** (640×480p 60 Hz) drives the system clock from the TMDS bit clock → **~252 MHz**.
- **PIO-USB** requires the CPU to be **exactly 120 MHz or 240 MHz** (it asserts this at startup).

252 MHz ≠ 240 MHz, and each demo only ever ran on its own. Making them coexist — the whole point of
this project — needs one of: running DVI from a 240 MHz-derived (slightly off-spec but
monitor-tolerated) pixel clock, clocking PIO-USB from an independent clock source, or choosing a DVI
timing that divides cleanly from 240 MHz. The chosen system clock also sets the CPU budget available
for emulation, so this decision gates the achievable frame rate. 240 MHz being a valid PIO-USB clock
is encouraging — it is close to the AMOLED port's 250 MHz emulation clock. Tracked as `PIZERO-02b`.

## Software architecture

```
┌──────────────────────────────────────────────────────────┐
│  core 0  CoCo emulation (6809 + SAM + PIA + VDG)          │
│  core 1  blit → DVI scanout (libdvi), SD + USB servicing  │
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
need adapting. Everything below the line — DVI, USB, SD — is board-specific and built on Waveshare's
proven reference stack for this board (earlephilhower arduino-pico core).

## Roadmap

Work is tracked in `issues.jsonl` (use `/issues` to list). Phases:

| Phase | Goal | Issues |
|---|---|---|
| 0 | Decisions + scaffolding | PIZERO-01..03 |
| 1 | HDMI bring-up: DVI test pattern | PIZERO-04..05 |
| 2 | XRoar boots to Color BASIC "OK" on HDMI | PIZERO-06..09 |
| 3 | Autonomous self-running demo | PIZERO-10 |
| 4 | USB-host keyboard / joystick input | PIZERO-11..13 |
| 5 | Dual-core split + performance | PIZERO-14..15 |

## Build

PlatformIO with the earlephilhower arduino-pico core, targeting the RP2350B:

```
pio run                 # build firmware.uf2
pio run -t upload       # flash (hold BOOT for BOOTSEL if it doesn't auto-reset)
pio device monitor      # serial @ 115200 — prints per-second [run] fps/cpu/blit
```

A microSD card is required, with the CoCo ROMs at **`/coco/bas12.rom`** (and optionally
`/coco/extbas11.rom`), plus an optional `disk11.rom` cart, a `.dsk` image, and `autorun.txt`
(see `AUTORUN.md`) — same card layout as the AMOLED port.

## Status

**Phase 2 complete — Color BASIC boots to "OK" on HDMI.** XRoar runs on core 0 (emulation + blit) with
`libdvi` scanning out a 320×240 framebuffer to 640×480p60 on core 1. Measured **54 fps** (CoCo-frame
emulation ~9.3 ms, `render_frame`+blit ~9.4 ms), already above the 30 fps target and ahead of the
AMOLED port's ~15 fps. Input is currently serial/autotype.

The 9.4 ms render+blit on core 0 is the bottleneck holding it below 60 fps; offloading it is the
Phase 5 dual-core work (`PIZERO-14`). Note: 640×480 is 4:3, so 16:9 monitors stretch it unless set to
4:3/aspect scaling. Next up: autonomous demo (`PIZERO-10`) and USB-host keyboard (`PIZERO-11`+),
which reopens the system-clock reconciliation (`PIZERO-02b`).

## References

- Waveshare RP2350-PiZero wiki — archived copy: `docs/RP2350-PiZero-wiki.pdf`; schematic: `docs/RP2350-PiZero-schematic.pdf`
- Source port being adapted: `../coco-rp2350-waveshare-touch-amoled-18`
- USB-host reference: `../waveshare-rp2350-usb-a`
- Upstream DVI driver: [Wren6991/PicoDVI](https://github.com/Wren6991/PicoDVI)
- USB host stack: [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) + Adafruit TinyUSB

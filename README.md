# XRoar on the Waveshare RP2350-PiZero

A port of the **XRoar** Tandy Color Computer (CoCo) emulator to the
[Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm), driving a
**mini-HDMI display** with **USB-host keyboard/joystick** input.

This started as an evaluation port — could the RP2350-PiZero run XRoar at least as well as the
existing [RP2350-Touch-AMOLED-1.8 port](../coco-rp2350-waveshare-touch-amoled-18) (which boots
Color BASIC to "OK" at ~36% real-time)? It does, comfortably: it boots Color BASIC over HDMI at a
**locked, tear-free 60 fps** — full real-time, ~4× the AMOLED port's frame rate, with a real **USB
keyboard typing directly into BASIC**. Only joystick input remains in Phase 4.

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

## System-clock reconciliation (HDMI + USB host) — resolved

DVI and PIO-USB want different system clocks: DVI's TMDS bit clock prefers ~252 MHz for
640×480p60 (25.175 MHz pixel), while Pico-PIO-USB asserts the CPU is *exactly* 120 MHz or
240 MHz. **Resolved (`PIZERO-02b`) by running the system clock at 240 MHz** and giving libdvi a
custom 640×480 timing with a 24 MHz pixel clock — a ~57.14 Hz refresh, slightly off-spec vs CEA
640×480p60 but well within typical HDMI monitor EDID tolerance and accepted by every monitor we
tested. The emulator still hits a locked 60 fps because it's paced from `FRAME_PERIOD_US`,
decoupled from display vsync; the monitor just samples the latest published framebuffer at
~57 Hz. Tear-free thanks to PIZERO-14's double buffer.

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

| Phase | Goal | Issues | Status |
|---|---|---|---|
| 0 | Decisions + scaffolding | PIZERO-01..03 | ✅ done |
| 1 | HDMI bring-up: DVI test pattern | PIZERO-04..05 | ✅ done |
| 2 | XRoar boots to Color BASIC "OK" on HDMI | PIZERO-06..09 | ✅ done |
| 3 | Autonomous self-running demo | PIZERO-10 | ✅ done |
| 4 | USB-host keyboard / joystick input | PIZERO-11/11a/11b/12/13 | 🟡 keyboard done; joystick + hot-replug open |
| 5 | Dual-core split + performance | PIZERO-14..15 | ✅ done |
| 6 | Audio output (native CoCo 6-bit DAC + 1-bit sound) | PIZERO-18 (PIZERO-17 stretch) | 🔜 scoping |

DVI-vs-USB clock reconciliation (`PIZERO-02b`) resolved at 240 MHz with a ~57 Hz monitor refresh
(see above). USB host enumeration + keyboard input verified on hardware; remaining open work in
Phase 4 is `PIZERO-13` (joystick) and `PIZERO-11b` (hot-replug bug — USB devices only enumerate
on a cold boot; unplugging and re-plugging doesn't re-attach).

The board is currently **silent**: XRoar synthesises the CoCo's 6-bit DAC + single-bit sound
internally, but nothing drives those samples to an output yet. Phase 6 (`PIZERO-18`) adds a real
audio path — the leading candidate is HDMI audio over the existing cable (data-island packets
emitted by `libdvi`, which is presently DVI-only), with PWM-to-a-GPIO as the fallback. The
synth/sound-chip experiment (`PIZERO-17`) is a stretch that depends on whichever path Phase 6 picks.

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

**Phases 0–3 and 5 complete; Phase 4 keyboard done.** XRoar boots Color BASIC on HDMI at a
locked, tear-free 60 fps emulation rate (display refreshes at ~57 Hz; emulator stays at 60). The
autonomous `AUTORUN.TXT` demo (`PIZERO-10`) works, dual-core double-buffered scanout
(`PIZERO-14`) is tear-free, and a **USB keyboard plugged into the PIO-USB port types directly
into BASIC** (`PIZERO-11`/`12`).

Remaining open work: **`PIZERO-13`** (USB joystick) and the open, **undiagnosed**
**`PIZERO-11b`** (USB devices enumerate only on a cold boot with the device already
attached; hot-replug doesn't re-enumerate). There is **no confirmed root cause** — it
has not been tested. Two unverified hypotheses stand: insufficient dev-port VBUS/inrush
current (would be fixed by a battery/powered hub — untested), or a software miss of the
disconnect event (RP2350-E9 leakage + the PIO-USB state machine holding the J line).

Next deliverable is **`PIZERO-18`** (Phase 6): native CoCo audio output. There is no sound today;
the plan is to add an HDMI audio path to `libdvi` (data islands) or fall back to PWM. See the
issue for the output-path decision that gates the work.

USB device-compatibility caveat: Pico-PIO-USB is USB 1.1 only, so modern high-speed USB 2.0
peripherals (e.g. Keychron K2, standalone gaming mice) don't enumerate. Simple wired USB
keyboards and full-speed wireless USB receivers are the working class.

Note: 640×480 is 4:3, so 16:9 monitors stretch it unless set to 4:3/aspect scaling.

## Performance

The headline result: **Color BASIC runs at a locked, tear-free 60 fps** — full real-time — on a
240 MHz system clock (~57 Hz monitor refresh, decoupled from the 60 Hz emulator pacing), with
~30% of the core-0 frame budget still free even with USB-host servicing running on core 0.

Per-frame work on core 0, against the 60 Hz frame period of **16.76 ms**:

| Work | Time | Notes |
|---|---|---|
| CoCo emulation | ~9.4 ms | 15,000 6809 cycles/frame at the emulated ~0.895 MHz |
| `render_frame` (alpha/text) | ~0.56 ms | precomputed glyph-row → packed-word LUT |
| Blit to framebuffer | ~1.5 ms | 320×240 RGB565, landscape, no rotation |
| **Total** | **~11.5 ms** | ~5.3 ms (31%) headroom per frame |

How we got from the first boot (54 fps) to a locked 60 fps:

- **Paint the static border once** at init instead of re-clearing it every frame — saves ~27K
  redundant pixel writes per frame (`src/coco_boot.cpp`).
- **Glyph-row → packed-32-bit-word render LUT** for alpha (text) mode, replacing per-pixel
  read-modify-write: **~6.4 ms → ~0.56 ms** per frame. (Graphics modes still use the per-pixel path.)
- **Double buffering** (`PIZERO-14`): two 320×240 RGB565 buffers with a `volatile` front-buffer
  handoff from core 0 to core 1, so `libdvi` never samples a half-rendered frame. Costs ~89% RAM.

Performance instrumentation, clock, and vreg tuning landed in `PIZERO-15`; the serial monitor prints
per-second `[run]` fps/cpu/blit stats. For comparison, the AMOLED port manages ~15 fps (~36%
real-time), so this is roughly a **4× improvement**.

Note the two distinct clocks: the host RP2350 MCU runs at **240 MHz** (set from the DVI TMDS bit
clock), while the *emulated* 6809 runs at its authentic **~0.895 MHz** — independent of the host clock.

## References

- Waveshare RP2350-PiZero wiki — archived copy: `docs/RP2350-PiZero-wiki.pdf`; schematic: `docs/RP2350-PiZero-schematic.pdf`
- Source port being adapted: `../coco-rp2350-waveshare-touch-amoled-18`
- USB-host reference: `../waveshare-rp2350-usb-a`
- Upstream DVI driver: [Wren6991/PicoDVI](https://github.com/Wren6991/PicoDVI)
- USB host stack: [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) + Adafruit TinyUSB

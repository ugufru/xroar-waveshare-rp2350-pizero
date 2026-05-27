# CLAUDE.md — Project Guide

XRoar (Tandy CoCo / Dragon emulator) on the **Waveshare RP2350-PiZero** (mini-HDMI output).
See `README.md` for the high-level goal and `issues.jsonl` (via `/issues`) for the work tracker.

## Source projects (read-only references)

- `~/github/coco-rp2350-waveshare-touch-amoled-18` — **working** XRoar port (AMOLED-1.8 board, boots
  Color BASIC to "OK"). Source of `lib/xroar_core`, `lib/coco_machine`, `src/coco_boot.cpp`,
  `src/main.cpp`, `AUTORUN.md`, `deploy.sh`. Display/QSPI/HAL code here is board-specific — do NOT copy.
- `~/github/waveshare-rp2350-usb-a` — PIO-USB host pattern (Pico-PIO-USB + TinyUSB) for the keyboard work.
- pico-examples `dvi_out_hstx_encoder` / Adafruit PicoDVI library — reference for the HSTX DVI driver.

## Target hardware (RP2350-PiZero)

- **MCU**: RP2350**B** (48 GPIO), 520 KB SRAM, **16 MB** flash. PSRAM footprint on board (likely unpopulated).
- **Display**: mini-HDMI → DVI via the **HSTX** peripheral.
- **Input**: PIO-USB host on USB-C port "3" (port "2" = power/programming).
- **Storage**: microSD slot (interface/pins to be confirmed — `PIZERO-01`).

| GPIO | Function | Notes |
|------|----------|-------|
| 12 | HSTX TMDS D2 | mini-HDMI |
| 14 | HSTX TMDS CK | mini-HDMI clock |
| 16 | HSTX TMDS D1 | mini-HDMI |
| 18 | HSTX TMDS D0 | mini-HDMI |
| 12–19 | HSTX block | DVI output (exact lane/pin pairing per board schematic — verify in `PIZERO-01`) |
| TBD | USB host D+/D− | USB-C port "3" (PIO-USB) |
| TBD | microSD | SCK/MOSI/MISO/CS or SDIO — confirm in `PIZERO-01` |

> ⚠ The HSTX pin pairing and SD pins above are from the RP2350 datasheet / general HSTX docs, **not yet
> verified against the PiZero schematic**. `PIZERO-01` must confirm before SD / display bring-up.

## Display plan

**On-screen target:** the CoCo's native 256×192 doubled to **512×384**, centered in a 640×480 DVI frame
with blank borders (64 px L/R, 48 px T/B). CoCo pixels render as crisp 2×2 blocks.

**Mechanism (memory-aware):** a literal 640×480 RGB565 framebuffer is 614 KB and **does not fit** in
520 KB SRAM (we also need 64 KB CoCo RAM + ROMs). Instead, keep the framebuffer at **320×240 RGB565
(~154 KB)** and let the **HSTX/DVI scan-out double every pixel/line to 640×480 in hardware** — free, no
CPU, no extra RAM. Inside the 320×240 buffer the CoCo's 256×192 is centered with 32 px/24 px borders;
after the hardware 2× it lands as exactly 512×384 with blank edges. The blitter writes 320×240 with
**no rotation** (landscape native) — simpler than the AMOLED portrait math.

## Critical build flags

- `board = rpipico2`-class for **RP2350B** (`-DPICO_RP2350B=1`; pick the matching earlephilhower board id).
- `framework = arduino` (earlephilhower core).
- `lib_deps`: Adafruit PicoDVI (HSTX mode), Pico-PIO-USB + Adafruit TinyUSB, `carlk3/no-OS-FatFS-SD...`.
- Keep `-O2`, `build_unflags = -Os`, `lib_ldf_mode = deep+`.
- `#include <Arduino.h>` **before** any XRoar headers (avoids the `_Bool` type conflict).
- `f_cpu`: PIO-USB host timing favours specific clocks (USB-A repo notes 120 MHz) vs the AMOLED port's
  250 MHz emulation pacing — reconcile in `PIZERO-14`/`PIZERO-15`.

## Workflow rules

- Work requires an issue in `issues.jsonl`. Create one before starting.
- Don't mark an issue `done` until the user confirms it works on hardware.
- Ask before any git push.
- Reference issue IDs (`PIZERO-NN`) in commit messages.

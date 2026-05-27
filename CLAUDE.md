# CLAUDE.md — agent working notes

XRoar (Tandy CoCo) on the **Waveshare RP2350-PiZero** (mini-HDMI + USB host).

**All project documentation lives in `README.md` and `docs/`** — hardware specs, the confirmed pinout,
the `libdvi`/HSTX rationale, the display geometry, and the clock-conflict risk are written up there, not
here. Don't duplicate them. This file is just pointers and conventions for working in the repo.

## Local references (not committed)

- `.ref/` (gitignored) — downloaded Waveshare vendor material:
  - `.ref/RP2350-PiZero-schematic.pdf` (also committed to `docs/`)
  - `.ref/demo/RP2350-PiZero/` — extracted official demo. **Authoritative source for pins and the
    proven driver stack.** Key files:
    - `Arduino/01-DVI/hello_dvi/` — libdvi + `common_dvi_pin_configs.h` (`pico_sock_cfg`), 320×240→640×480.
    - `Arduino/02-USB/device_info/device_info.ino` — Pico-PIO-USB host: `HOST_PIN_DP=28`, CPU must be 120/240 MHz.
    - `Arduino/03-MicroSD/hw_config.c` — SD SPI pins (SCK30/MOSI31/MISO40/CS43/CD22).
    - `C/boards/waveshare_rp2350_pizero.h` — board defines (RP2350B, 16 MB flash, I2C0 6/7).

## Source repos to adapt (read-only)

- `~/github/coco-rp2350-waveshare-touch-amoled-18` — working XRoar port. Reuse `lib/xroar_core`,
  `lib/coco_machine`, `src/coco_boot.cpp`, `src/main.cpp`, `AUTORUN.md`, `deploy.sh`.
  Its `lib/sh8601`, `lib/qspi_pio`, `lib/hal`, `src/hw_config.c` are AMOLED-specific — do NOT copy.
- `~/github/waveshare-rp2350-usb-a` — PIO-USB host reference.

## Build gotchas to remember

- Target **RP2350B** (not the A on the AMOLED board), earlephilhower arduino-pico core.
- `#include <Arduino.h>` **before** XRoar headers (avoids `_Bool` type conflict).
- Keep `-O2`, `build_unflags = -Os`, deep LDF.
- DVI uses `pio_set_gpio_base(pio, 16)` because TMDS pins are GPIO 32–39 (> 31).
- HSTX is GPIO 12–19 only and is NOT wired to the HDMI connector here → `libdvi` is mandatory.

## Workflow rules

- Work requires an issue in `issues.jsonl`. Create one before starting.
- Don't mark an issue `done` until the user confirms it works on hardware.
- Ask before any git push.
- Reference issue IDs (`PIZERO-NN`) in commit messages.
- User preference: no AskUserQuestion popups — ask in plain prose.

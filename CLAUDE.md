# CLAUDE.md: agent working notes

XRoar (Tandy CoCo) on the **Waveshare RP2350-PiZero** (mini video port + USB host).

**All project documentation lives in `README.md` and `docs/`**: hardware specs, the confirmed pinout,
the `libdvi`/HSTX rationale, the display geometry, and the clock-conflict risk are written up there, not
here. Don't duplicate them. This file is just pointers and conventions for working in the repo.
The end-to-end video+audio signal path (CoCo → video out) is mapped in `docs/pipeline.md`, which links the
deeper `docs/video-audio-notes.md` and `docs/BUILD.md`. The sound chips (SN76489, the AY cartridges,
stereo, their render cost) are in `docs/sound.md`, with test programs in `docs/ay-checks/`.
`docs/ROADMAP.md` sequences the open work (a navigation map over `issues.jsonl`: what's shipped,
what's next, and dependencies); it points to tickets, it doesn't restate them.

## Local references (not committed)

- `.ref/` (gitignored): downloaded Waveshare vendor material:
  - `.ref/RP2350-PiZero-schematic.pdf` (also committed to `docs/`)
  - `.ref/demo/RP2350-PiZero/`: extracted official demo. **Authoritative source for pins and the
    proven driver stack.** Key files:
    - `Arduino/01-DVI/hello_dvi/`: libdvi + `common_dvi_pin_configs.h` (`pico_sock_cfg`), 320×240→640×480.
    - `Arduino/02-USB/device_info/device_info.ino`: Pico-PIO-USB host: `HOST_PIN_DP=28`, CPU must be 120/240 MHz.
    - `Arduino/03-MicroSD/hw_config.c`: SD SPI pins (SCK30/MOSI31/MISO40/CS43/CD22).
    - `C/boards/waveshare_rp2350_pizero.h`: board defines (RP2350B, 16 MB flash, I2C0 6/7).

## Source repos to adapt (read-only)

- `~/github/coco-rp2350-waveshare-touch-amoled-18`: working XRoar port. Reuse `lib/xroar_core`,
  `lib/coco_machine`, `src/coco_boot.cpp`, `src/main.cpp`, `AUTORUN.md`, `deploy.sh`.
  Its `lib/sh8601`, `lib/qspi_pio`, `lib/hal`, `src/hw_config.c` are AMOLED-specific: do NOT copy.
- `~/github/waveshare-rp2350-usb-a`: PIO-USB host reference.

## Building

**`docs/BUILD.md` is the source of truth for how to build/flash this firmware**:
the PlatformIO envs, the full video/audio build-flag matrix, serial monitoring, and
the toolchain gotchas. Read it before building; keep it in sync when you add/change
a build flag.

- **You build it (the agent), not the user.** The product firmware is the default env
  `pizero_stream_60` (PIZERO-45: true 640×480p60 + streaming data-island audio + USB; since
  PIZERO-135 it is a CoCo 2 or a 128 KB CoCo 3, picked by `machine` in settings.txt or a game's
  own file), so a bare `pio run` builds it. `pizero_stream` (off-spec 52 Hz audio) is kept as the
  fallback; the rest are diagnostics tied to open issues. Before committing a
  change that adds RAM, boot it as a CoCo 3 (with `debug = on` in settings.txt, PIZERO-316, or the boot does not wait for the monitor) and read the serial line `[mem] free heap N bytes
  with the CoCo 3's 128 KB`: the CoCo 3 takes 64 KB of its RAM from the heap, so static RAM
  growth stops it starting (PIZERO-262: at 85.7% it failed; 82.6% leaves about 9.7 KB). Also
  build `pizero_stream` and `pizero_wavmeas` (every
  env is single-buffered, about 81% RAM for the product since PIZERO-135). Test both machines
  on hardware when a change touches video, input or the machine core. Use a real env, never a
  one-off `PLATFORMIO_BUILD_FLAGS` (it links stale objects, see BUILD.md §4b).

## Build gotchas to remember

- Target **RP2350B** (not the A on the AMOLED board), earlephilhower arduino-pico core.
- `#include <Arduino.h>` **before** XRoar headers (avoids `_Bool` type conflict).
- Keep `-O2`, `build_unflags = -Os`, deep LDF.
- DVI uses `pio_set_gpio_base(pio, 16)` because TMDS pins are GPIO 32–39 (> 31).
- HSTX is GPIO 12–19 only and is NOT wired to the video connector here → `libdvi` is mandatory.
- `compile_commands.json` is gitignored. Regenerate with `pio run -t compiledb` after adding or
  swapping libraries so clangd can resolve `Arduino.h`, libdvi, Pico-PIO-USB, etc. Otherwise the
  IDE buries real diagnostics under "header not found" noise.

## Workflow rules

- **Sources of truth, in order.** To plan, or to say how something works, read
  the canonical docs first (`README.md`, `SETTINGS.md`, `docs/`), then confirm
  against the source code. The code is what the firmware does: if a doc
  disagrees with it, correct the doc from the code. `issues.jsonl` is history
  and intent, not a description of the firmware; never cite an issue's
  description as how something works today. Any conflict between code, docs
  and issues gets reported to the user, not quietly resolved.
- Work requires an issue in `issues.jsonl`. Create one before starting.
- Don't mark an issue `done` until the user confirms it works on hardware.
- Ask before any git push.
- **NEVER create git branches, ever. Commit directly to the current branch (`main`).**
  This overrides any default "branch before committing on the default branch" behavior.
  `main` is the only branch; `git branch` must print only `main`. No worktrees, no
  branch-per-agent isolation, no "safety" branch for risky or half-working work: that
  goes behind a build flag on `main`, or stays uncommitted. Archive dead exploratory
  history with a tag (`archive/<topic>`), never a lingering branch. See `CONTRIBUTING.md`.
- Reference issue IDs (`PIZERO-NN`) in commit messages.
- **Commit STLs before sending them to a printer.** Every revision that
  gets printed must exist as a commit first, so the physical part in hand
  always maps to a recoverable version. Render, commit, then open the
  slicer. See `hardware/case/README.md`.
- User preference: no AskUserQuestion popups: ask in plain prose.
- **Improvements are on by default.** A new setting that improves on the
  original machine (auto-repeat, the 6847t2 font, true lower case, and the
  like) defaults to on. Authentic behavior stays available as the other
  value, and a game that minds gets it back in its own NAME.TXT. Do not
  ship an improvement defaulting to off, and do not ask which default.

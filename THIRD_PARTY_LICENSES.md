# Third-Party Licenses

This repository as a whole is distributed under **GPL-3.0-or-later**
(see `LICENSE`) because it incorporates XRoar. Individual third-party
components retain their own licenses as listed below. All of the permissive
licenses below (BSD-3-Clause, MIT, Apache-2.0, LGPL-2.1) are compatible with
GPL-3.0-or-later, so the combined work is distributable under the GPL.

---

## XRoar — Dragon / Tandy Color Computer emulator

**Files:** `lib/xroar_core/` (vendored subset — see `lib/xroar_core/PROVENANCE.md`)

**License:** GNU General Public License v3.0 or later
(full text: `lib/xroar_core/COPYING.GPL`)

**Copyright:** © Ciaran Anscomb and the XRoar contributors

**Upstream:** https://www.6809.org.uk/xroar/
**Source:** https://www.6809.org.uk/git/xroar.git
**Vendored revision:** `a2d31903eeabb31b0d42481587364db49ef0ec70` (2026-05-14)

XRoar is the core emulator engine used in this project. It provides the
MC6809 CPU, SAM (MC6883), VDG (MC6847), PIA (MC6821), event scheduler,
and the portability layer. Our `lib/coco_machine` and `src/coco_boot.cpp`
are original glue code written against XRoar's part framework.

Because this work links XRoar at build time, the combined work is
governed by the GPL-3.0-or-later. Anyone redistributing this project,
or binaries built from it, must comply with those terms.

The XRoar homepage explicitly states "This is free software: you are
free to change and redistribute it," and lists a number of community
ports (Android, AmigaOS 4, MorphOS, PSP, GP2X, Dreamcast). This RP2350
port is offered in the same spirit and credits Ciaran as the original
author per the GPL and per his customary `[by name]` changelog
convention.

---

## PicoDVI (libdvi) — PIO DVI/HDMI driver

**Files:** `lib/libdvi/` (see `lib/libdvi/PROVENANCE.md`)

**License:** BSD 3-Clause (full text: `lib/libdvi/LICENSE`)

**Copyright:** © 2021 Luke Wren (Wren6991)

**Upstream:** https://github.com/Wren6991/PicoDVI

`libdvi` bit-bangs DVI/HDMI over PIO + DMA — mandatory on this board because
the HDMI connector is wired to GPIO 32–39 and HSTX cannot reach it. The
vendored copy came from Waveshare's official RP2350-PiZero Arduino demo
(`01-DVI/hello_dvi`), which bundles PicoDVI. It has been extended in this
project for full HDMI mode and streaming audio (changes documented by
`PIZERO-NN` references in the source); those changes remain BSD-3-Clause.

### HDMI data-island encoder — pico_lib

**Files:** `lib/libdvi/dvi_data_island.c`, `lib/libdvi/dvi_data_island.h`

**License:** MIT (full text included in `lib/libdvi/LICENSE`)

**Copyright:** © Shuichi Takano (shuichitakano)

**Upstream:** https://github.com/shuichitakano/pico_lib (`dvi/data_packet.cpp`)

The HDMI data-island / audio-sample packet encoder is ported from pico_lib,
the same library lineage as PicoDVI.

---

## Build-time dependencies (PlatformIO `lib_deps`)

These are fetched at build time (not vendored in this repository) and linked
into the firmware. They are listed here because the distributed binary
incorporates them.

| Library | Author | License | Used for |
|---------|--------|---------|----------|
| [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) | sekigon-gonnoc | MIT | USB host bit-banging on GPIO 28/29 |
| [Adafruit TinyUSB Library](https://github.com/adafruit/Adafruit_TinyUSB_Arduino) | Ha Thach / Adafruit | MIT | USB host stack (HID keyboard/mouse) |
| [no-OS-FatFS-SD](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) | carlk3 (FatFs © ChaN) | Apache-2.0 (FatFs: BSD-style) | microSD FAT filesystem over SPI |

---

## Arduino-Pico core (earlephilhower)

Build framework, via PlatformIO. LGPL-2.1 (core) with permissive licenses
on the bundled libraries (TinyUSB, FatFs, etc.). See the framework's
`LICENSE.md` in your PlatformIO installation.

---

## Waveshare Electronics — board demo & hardware docs

**Files:**
- `demo/device_info_demo.cpp` — USB-host triage demo, ported `.ino` → `.cpp`
  from Waveshare's official RP2350-PiZero Arduino demo
  (`02-USB/device_info`). The demo itself is an **Adafruit TinyUSB example**
  and carries its original **MIT license header** (© 2019 Ha Thach for
  Adafruit Industries), which is preserved verbatim at the top of the file.
- `docs/RP2350-PiZero-schematic.pdf` — Waveshare's published board schematic,
  included for hardware reference / interoperability.

**Source:** https://www.waveshare.com/wiki/RP2350-PiZero

Waveshare distributes the demo code and schematic freely for use with their
hardware. The full board wiki is **not** re-hosted here — see the link above.

---

## ROMs — NOT included in this repository

The Color BASIC, Extended Color BASIC, and Disk BASIC ROMs required to
run this emulator are **© Microsoft / Tandy Corporation** and are **not**
redistributed here under any circumstances.

Users must supply their own ROM images (typically dumped from hardware
they own) and place them in `/coco/` on the microSD card. This repository
provides no download links, no torrent magnets, and no instructions for
obtaining ROMs from third-party sources.

---

## Bundled demo content

The only demo content shipped in this repository is `sample-sd/coco/autorun.txt`
— a short BASIC program (semigraphics scatter loop) authored by this project as
a display test. It contains no third-party code. See `AUTORUN.md` for the
autorun format.

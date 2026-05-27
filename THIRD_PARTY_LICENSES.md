# Third-Party Licenses

This repository as a whole is distributed under **GPL-3.0-or-later**
(see `LICENSE`) because it incorporates XRoar. Individual third-party
components retain their own licenses as listed below.

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

## Waveshare Electronics — Hardware Drivers

**Files:**
- `lib/hal/` — DEV_Config (GPIO, I2C, SPI abstraction)
- `lib/sh8601/` — SH8601 AMOLED display driver
- `lib/qspi_pio/` — QSPI PIO transport layer
- (touch, IMU, audio codec drivers pending import: ft3168, qmi8658, es8311)

**Source:** [Waveshare RP2350-Touch-AMOLED-1.8 Wiki](https://www.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8)

Ported from the official Waveshare Arduino demo code, distributed freely
for use with Waveshare hardware. No explicit license header is present
in these files. Used here as reference driver code for the
RP2350-Touch-AMOLED-1.8 development board. A request for explicit license
clarification has been sent to Waveshare; this section will be updated
when a response is received.

---

## STMicroelectronics — Font Data

**Files (if/when GUI_Paint is imported):**
- `lib/gui_paint/fonts.h`
- `lib/gui_paint/font*.cpp`

**License:** BSD 3-Clause
**Copyright:** © 2014 STMicroelectronics

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of STMicroelectronics nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## Arduino-Pico core (earlephilhower)

Build framework, via PlatformIO. LGPL-2.1 (core) with permissive licenses
on the bundled libraries (TinyUSB, FatFs, etc.). See the framework's
`LICENSE.md` in your PlatformIO installation.

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

Any `.bas`, `.bin`, `.cas`, or `.dsk` demo content shipped in this
repository is either (a) authored by this project, or (b) included with
explicit permission from the original author, or (c) widely understood
to be in the public domain. Each demo file's provenance will be
documented before the first public release (tracked in `AMOLED-47`).

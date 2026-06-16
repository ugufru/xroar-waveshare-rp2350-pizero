# libdvi provenance

## Upstream

- Project: PicoDVI — bit-banged DVI/HDMI on the RP2040/RP2350 via PIO + DMA
- Author: Luke Wren (Wren6991)
- Homepage / source: https://github.com/Wren6991/PicoDVI
- License: BSD 3-Clause (see `LICENSE` in this directory; © 2021 Luke Wren)

The vendored copy was taken from Waveshare's official RP2350-PiZero Arduino
demo (`01-DVI/hello_dvi`), which bundles PicoDVI's `libdvi`. Upstream PicoDVI
distributes its license as a single top-level `LICENSE` file rather than
per-file headers; this tree mirrors that convention.

## HDMI data-island encoder (additional upstream)

`dvi_data_island.c` / `dvi_data_island.h` — the HDMI data-island / audio-sample
packet encoder — is ported from:

- Project: pico_lib (`dvi/data_packet.cpp`)
- Author: Shuichi Takano (shuichitakano)
- Source: https://github.com/shuichitakano/pico_lib
- License: MIT (full text included in this directory's `LICENSE`)

This is the same library lineage as PicoDVI.

## Local modifications (this project)

PicoDVI's `libdvi` was extended for this port (XRoar on the Waveshare
RP2350-PiZero); the changes remain BSD-3-Clause / MIT as inherited and are
documented by `PIZERO-NN` references in the source comments:

- Full HDMI mode: video preamble + guard bands on active lines, AVI / Audio
  InfoFrame + ACR data islands in vblank (`dvi_timing.cpp`, PIZERO-28/29/30).
- Streaming per-active-line audio delivery: DMA blanking split into
  `[fp+sync][bp][active]` chunks and per-line back-porch island buffers
  (`dvi_setup_active_audio_line`, `dvi_setup_active_hdmi_framing`; PIZERO-38).
- RAM-resident (`.time_critical`) data-island encode so it can run in the
  core-1 DMA IRQ (PIZERO-37).
- `pio_set_gpio_base(pio, 16)` support for TMDS on GPIO 32–39 (RP2350B).

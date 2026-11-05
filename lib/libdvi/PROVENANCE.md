# libdvi provenance

## Upstream

- Project: PicoDVI, bit-banged DVI on the RP2040/RP2350 via PIO + DMA
- Author: Luke Wren (Wren6991)
- Homepage / source: https://github.com/Wren6991/PicoDVI
- License: BSD 3-Clause (see `LICENSE` in this directory; © 2021 Luke Wren)

The vendored copy was taken from Waveshare's official RP2350-PiZero Arduino
demo (`01-DVI/hello_dvi`), which bundles PicoDVI's `libdvi`. Upstream PicoDVI
distributes its license as a single top-level `LICENSE` file rather than
per-file headers; this tree mirrors that convention.

## Data-island encoder (additional upstream)

`dvi_data_island.c` / `dvi_data_island.h`, the data-island / audio-sample
packet encoder, is ported from:

- Project: pico_lib (`dvi/data_packet.cpp`)
- Author: Shuichi Takano (shuichitakano)
- Source: https://github.com/shuichitakano/pico_lib
- License: MIT (full text included in this directory's `LICENSE`)

This is the same library lineage as PicoDVI.

## Local modifications (this project)

PicoDVI's `libdvi` was extended for this port (XRoar on the Waveshare
RP2350-PiZero); the changes remain BSD-3-Clause / MIT as inherited and are
documented by `PIZERO-NN` references in the source comments:

- Full data-island mode (audio with the video): video preamble + guard bands on active lines, AVI / Audio
  InfoFrame + ACR data islands in vblank (`dvi_timing.cpp`, PIZERO-28/29/30).
- Streaming per-active-line audio delivery: DMA blanking split into
  `[fp+sync][bp][active]` chunks and per-line back-porch island buffers
  (`dvi_setup_active_audio_line`, `dvi_setup_active_hdmi_framing`; PIZERO-38).
- RAM-resident (`.time_critical`) data-island encode so it can run in the
  core-1 DMA IRQ (PIZERO-37).
- Audio Sample Packets follow IEC 60958 (PIZERO-223): validity 0, a consumer
  channel status declaring linear PCM, 48 kHz and 16 bits, and the block-start
  flag on whichever subpacket starts a 192-frame block. The upstream packet set
  V=1, sent no channel status and could skip block starts.
- `pio_set_gpio_base(pio, 16)` support for TMDS on GPIO 32–39 (RP2350B).
- `dvi_static_framebuf_main_pal8` (PIZERO-134) calls `dvi_idle_work`, a hook
  the firmware sets, while it waits for a free TMDS buffer instead of only
  sleeping, so core 1 can decode the CoCo 3's lines between scanlines
  (PIZERO-330). The profiling build times that wait (`g_dvi_c1_wait_us`).

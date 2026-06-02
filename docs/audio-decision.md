# Audio path — decision & feasibility (PIZERO-18)

**Status:** source implemented (PIZERO-18, 2026-06-02), sink not yet. The board is
currently **silent**. NOTE (corrects an earlier premise): this stripped port vendors
**no XRoar sound module** — desktop XRoar synthesises the audio stream, but that code
was not vendored here, so the CoCo's 6-bit DAC + single-bit sound existed only as
PIA1 register state and were discarded. We now **synthesise the stream ourselves** in
`coco_machine` (tap the 6-bit DAC on PIA1 port A bits 2–7 + single-bit sound on PB1,
resample to 32 kHz mono into a ring). What remains is the **sink** — nothing yet
drives those samples to an output.

**Decision (direction):** primary target is **HDMI audio over the existing cable**,
carried in `libdvi` data-island packets. Fallback is **PWM out of one GPIO** into a
small RC filter / Class-D amp. HDMI is preferred because it needs no extra hardware
and the board's mini-HDMI is the only output it ships with.

This file records the feasibility scoping (2026-05-31, from the 43b reconciliation)
so the decision survives the project being parked. The implementation gate in
PIZERO-18 is: **decide & prove the output path before writing emulator-side code.**

## Why this is feasible

The board wires HDMI to GPIO 32-39, so HSTX (GPIO 12-19) is unavailable and we use
the PIO-based `lib/libdvi` (Wren6991/PicoDVI). The key enabler:

- **The PIO serialiser is content-agnostic.** It just shifts out 10-bit symbols
  (`dvi_serialiser.pio.h`: `out pins, 1` ×10, `out_shift … 10 * DVI_SYMBOLS_PER_WORD`).
  TMDS video, the 4 DVI control symbols, TERC4 data-island symbols, and guard bands
  are *all* 10-bit. **No PIO program change is needed** — HDMI audio is purely a
  matter of *which symbols we feed* and *how the per-scanline DMA list is built*.

- **Prior art uses the exact library we vendored.** `shuichitakano`'s *PicoDVI-audio*
  fork adds HDMI data islands + audio on top of Wren6991's TMDS core; a C fork added
  a *data-island queue* (audio subsystem pushes TERC4 packets, the library injects
  them during horizontal blanking); and "FRANK HDMI Sound" (Adafruit, 2026-05-26)
  packages 32 kHz stereo PCM into the data-island stream on the same core. So the
  work is **porting an existing extension**, not inventing one.
  - NOT applicable: `fliperama86/pico_hdmi` and "PicoHDMI" are **HSTX**-based — we can
    borrow their packet/TERC4 logic but not their output path.

- **Our off-spec clock is a lucky break for audio clock regeneration.** ACR requires
  `128·fs = fTMDS·N/CTS`. Our pixel clock is a round **24 MHz** (the PIZERO-02b
  choice), which yields clean integers: 32 kHz → N=4096, **CTS=24000**; 48 kHz →
  N=6144, **CTS=24000**. A standard 25.175 MHz clock would be messier.

## What has to change (HDMI path)

1. **Per-scanline DMA list grows.** Today `dvi_scanline_dma_list` is fixed-size
   (`dvi_timing.h:51-57`: `l0[DVI_STATE_COUNT]`, `l1[2]`, `l2[2]`). A data island
   needs extra chunks in blanking on all three lanes: *preamble → leading guard band
   → TERC4 island → trailing guard band*. This struct and the chain-to/IRQ scheme
   must be reworked.
2. **The documented IRQ hazard is the core risk.** `dvi_timing.cpp:189-191` warns the
   per-scanline IRQ model "breaks down when you have very short scanline sections like
   guard bands." That is exactly what data islands add — and exactly what the audio
   fork already redesigns. Port that, don't re-solve it.
3. **New encode + packet logic:** a 16-entry TERC4 LUT, BCH header/ECC per packet, and
   the mandatory HDMI packets — **AVI InfoFrame**, **Audio InfoFrame**, **Audio Clock
   Regeneration (ACR)**, **Audio Sample Packets**. InfoFrames are static (encode once);
   only audio sample packets re-encode per frame.
4. **Audio source tap:** DONE (PIZERO-18) — `coco_machine` taps the 6-bit DAC (PIA1
   port A bits 2–7) + single-bit sound (PB1) and resamples to 32 kHz mono via
   `coco_machine_audio_read()`. (There is no XRoar `sound_module` in this port to tap;
   we build the stream directly.) The sink just needs to drain that ring, duplicate
   mono→stereo, and feed the data-island queue. Do the TERC4 encoding on **core 1**
   (where libdvi scanout lives), with a clean producer/consumer handoff from core 0 —
   not on core 0's ~5 ms frame slack.

## Risks (in order of likelihood to bite)

- **RAM.** Already ~89% SRAM used (double-buffered 320×240 framebuffers). The grown
  DMA-list templates (×3), packet buffers, and an audio sample ring need a few KB we
  may not have spare. Most likely thing to force a trade-off (e.g. give back a buffer).
- **Monitor acceptance.** We already rely on EDID tolerance for the ~57 Hz timing.
  Switching the sink from DVI to HDMI signalling on an off-spec timing may make some
  displays pickier. Verify on the real monitor *first*.
- **Merge friction.** Our libdvi is locally patched (the `tmds_encode.S` /
  `dvi_config_defs.h` assembler guards from PIZERO-04). Porting the fork is a careful
  merge, not a drop-in.

## De-risking order (before committing to the full port)

1. **Bench the reference unmodified.** Flash shuichitakano's PicoDVI-audio (or FRANK)
   demo on the PiZero to confirm *our specific monitor accepts HDMI audio at all* over
   this board's wiring. Cheapest kill-switch.
2. **Cost the RAM** of the grown DMA lists + buffers against our ~11% free.
3. Port the data-island path into `lib/libdvi`, then tap XRoar audio at 32 kHz.

If (1) or (2) fails, fall back to **PWM**: one free GPIO → 1st-order RC (~22 kHz
corner) → small Class-D amp (e.g. PAM8302). Simpler, but needs a wire/part and is not
"over the existing cable." (This mirrors the 43b board's decision, which had no HDMI
option — see that project's `docs/audio-decision.md`.)

## Why 32 kHz / stereo is enough

The CoCo audio source is a 6-bit DAC summed with a 1-bit cassette/speaker tap — well
under 32 kHz effective bandwidth. 32 kHz stereo matches the FRANK reference and is
plenty; 48 kHz is available with the same clean CTS if a sink prefers it. The signal
is mono in origin (duplicate to both channels).

## References

- shuichitakano PicoDVI-audio discussion — https://forums.raspberrypi.com/viewtopic.php?t=348724
- PicoDVI + audio data-island thread — https://forums.raspberrypi.com/viewtopic.php?t=366899
- FRANK HDMI Sound for RP2350 (Adafruit) — https://blog.adafruit.com/2026/05/26/a-small-hdmi-video-and-audio-driver-for-the-raspberry-pi-rp2350-raspberry_pi
- pico_hdmi (HSTX, for contrast / packet logic only) — https://github.com/fliperama86/pico_hdmi
- 43b's PWM decision — `~/github/xroar-waveshare-rp2350-43b/docs/audio-decision.md`

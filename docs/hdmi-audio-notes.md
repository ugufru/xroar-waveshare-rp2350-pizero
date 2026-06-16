# HDMI audio on the RP2350-PiZero — engineering notes, successes & gotchas

How we got **clean-ish HDMI audio out a board whose HDMI is wired to ordinary
GPIOs (no HSTX) and whose pixel clock is forced off-spec by USB**. This is the
hard-won institutional knowledge — the wins, the dead ends, and the tricks — for
anyone extending it (or writing it up). Companion to `audio-decision.md` (the
original scoping) and `BUILD.md` (env/flag reference).

Issue trail: PIZERO-18 (native audio) → 27/28/29/30 (data islands) → 34 (vblank
dead end) → **35/36/37/38/39 (the streaming re-architecture that fixed the
warble)** → 41 (residual source-side fidelity) → 44 (60 Hz).

---

## TL;DR — what works today

- **HDMI audio via data islands**, encoder ported from `shuichitakano/pico_lib`
  (TERC4 + BCH + ACR + AVI/Audio InfoFrame + Audio Sample Packets) in
  `lib/libdvi/dvi_data_island.c`.
- **Delivery is the hard part, not the encoder.** The win was switching from a
  bursty pre-encoded scheme to **streaming one metered island per active line**
  (Takano's model), which killed a frame-rate warble.
- Audio runs entirely on **core 1 in the per-scanline DMA IRQ**; core 0 just
  produces samples into a lock-free ring. `aud=0us` on core 0.
- Residual gap vs desktop XRoar (slightly sharp + buzzy) is **source-side**
  (PIZERO-41), *not* HDMI delivery — proven by ruling out the clock (see below).

---

## The architecture that works (streaming delivery)

```
core 0 (emulator)                         core 1 (libdvi scanout + DMA IRQ)
  6809/SAM/PIA/VDG                           per active-line IRQ (fires ONE line
  audio_emit() -> SPSC ring  ───────────▶    ahead of the beam):
  (event-driven integrate-and-dump,            1. 16.16 meter -> n = 0..4 samples
   keyed to PIA-write timestamps)               2. read n from the ring
                                                 3. encode ONE audio island
                                                 4. point the back-porch DMA at it
  ACR/AVI/AudioIF: static, on vblank lines
```

Why each piece is the way it is:

1. **Stream, don't pre-encode banks.** The original scheme pre-encoded 77 of 480
   active lines (3 packets each) into double-buffered banks. The sink's audio FIFO
   saw a coarse, frame-locked burst pattern → **~52 Hz warble**. The fix is
   standard-HDMI behaviour: sprinkle **0–4 samples onto (nearly) every active
   line** via a 16.16 fixed-point sample accumulator (`g_meter_acc += step` each
   line; `n = acc>>16`). Near-constant feed ⇒ no warble.

2. **Encode in the IRQ, one line ahead.** One island encode costs **~4.8 µs**
   (measured, PIZERO-36). That does *not* fit the back-porch window it ships in
   (~5.4 µs, zero margin — this is what blacked the screen in the early "Option
   A"). But the active-line IRQ already fires **one line ahead** of the beam, so
   we encode line N's island during line N−1's scanout, where the budget is a
   whole line (~33 µs at 24 MHz). ~480 encodes/frame ≈ 2.3 ms on core 1's
   otherwise-idle-between-IRQs CPU.

3. **Rotating buffer pool, not per-line banks.** A small pool of back-porch
   buffers (≈6) encoded on the fly replaces ~130 KB of banks → **RAM 92 % → 68 %**.

---

## Successes worth remembering

- **Warble eliminated** by the streaming re-architecture (PIZERO-35/38) —
  HW-confirmed steady tone.
- **Fast-forward distortion eliminated** (PIZERO-39) by deepening the SPSC ring
  (2048 → 8192) and priming it to half: the producer dumps a whole frame of
  samples in a burst during emulation, which overflowed the small ring →
  reader-side catch-up skipped chunks → audio fast-forward (sounded "a few
  semitones sharp"). Deep + primed ring fixed it.
- **RAM win**: streaming freed ~126 KB, enough that the no-audio path can stay
  double-buffered (tear-free) and there's headroom for future features.
- **60 Hz + USB unlocked** (PIZERO-44): see the dedicated section below.

---

## Gotchas & tricks (the expensive lessons)

- **`__not_in_flash_func` silently no-ops in C files here.** The arduino-pico core
  defines `PICO_NO_HARDWARE` (= 0), so `#if defined(PICO_NO_HARDWARE)` is true and
  the SDK's RAM-placement macros expand to nothing in a plain C translation unit.
  The encoder *stayed in flash* and the IRQ stalled on XIP. **Fix:** apply an
  explicit `__attribute__((section(".time_critical.<x>")))`. Always **verify**
  with `nm` (RAM = `0x2000_xxxx`, flash = `0x1001_xxxx`; a `*_veneer` in flash is
  fine). This is the single most time-wasting trap in the whole project.

- **Anything the audio IRQ touches must be in RAM.** Encoder, TERC4 table, and the
  ring reader (`coco_machine_audio_read`) are all `.time_critical`. A flash call in
  the per-line IRQ window = corruption/black screen.

- **The back porch must be wide enough for one island.** `bpw = h_back_porch /
  DVI_SYMBOLS_PER_WORD`. One island needs ~27 words (preamble 4 + guard 1 +
  island 16 + guard 1 + video preamble 4 + guard 1). This geometry **couples audio
  to the video timing**: it forced `h_bp=130` at 52 Hz, and dictates `h_bp=56` at
  60 Hz (see below). Per-line buffer width is set by the back porch, **not** by
  packet count — so you can't buy more carrier lines by shrinking packets.

- **This sink ignores ACR entirely.** Sweeping CTS 25176 → 26674 → 50000 (the last
  would be ~1 octave if honoured) had **zero** audible effect. So on this monitor
  the audio rate is *not* reconstructed from ACR — pitch is governed by the video
  clock and the source. Lesson: don't assume a sink uses ACR; `HDMI_ACR_CTS` is
  kept tunable (default 25176) for sinks that *do* (PIZERO-32), but it's inert
  here.

- **vblank back-porch islands break sync** on this sink (PIZERO-34). Audio islands
  only work in the **active-line** back porch. ACR/InfoFrames go on the vblank
  *active block*, never the vblank back porch.

- **Cross-core SPSC ring discipline** (PIZERO-39): writer (core 0) owns the write
  index only + `__dmb()` to publish data before the index; reader (core 1 IRQ)
  owns the read index and does overflow catch-up itself. Two cores writing one
  index = lost/garbage samples. Deepen + prime to absorb the bursty producer.

- **Flashing is flaky — the firmware's USB can hang and take the CDC with it.**
  The cu.* port NUMBER changes across re-enumeration (`usbmodem101`, `2101`,
  `1101`, …) — always `ls /dev/cu.usbmodem*` and pass `--upload-port`, never trust
  auto-detect (it once grabbed a Bluetooth speaker). If the board is USB-dead,
  recover via **manual BOOTSEL** (hold BOOT + plug) and flash with picotool.

---

## Validation techniques (isolate source vs transport)

- **`-DHDMI_AUDIO_SYNTH`** feeds a mathematically clean 440 Hz wavetable straight
  into the islands, bypassing the emulator. A pure sine has ~no harmonics, so any
  roughness = the **transport**, not the CoCo waveform/resampler. Primary warble
  acceptance test.
- **`-DAUDIO_WAV_DUMP`** streams the audio **source ring** as a base64 WAV over
  USB-CDC (no HDMI), so you can decode + measure the exact frequency/spectrum and
  prove whether a defect is in the source or the HDMI path. (`env:pizero_wavmeas`.)
- **Rule-out logic that paid off:** to prove the residual pitch/buzz (PIZERO-41) is
  source-side, we showed it survives BOTH a standard pixel clock (`HDMI_STD_TIMING`)
  AND any ACR/CTS value → it can't be the HDMI clock, so it's the emulation/
  resampler.

---

## The 60 Hz breakthrough (PIZERO-44) and its audio implication

The off-spec **24 MHz pixel clock** (→ ~52 Hz refresh) existed only because
PIO-USB needs `sysclk = 240 MHz` and `pixel = sysclk/10`. Standard 640×480p60
needs 25.175 MHz → 251.75 MHz sysclk, long assumed to conflict with USB.

**It doesn't.** At **252 MHz**, PIO-USB's clock dividers are *exact* (252/48 =
5.25, /6 = 42, /12 = 21, /96 = 2.625 — all representable in the PIO 16.8 divider),
so it gets a clean 48 MHz USB bit clock. HW-confirmed: **60 Hz locks on the
monitor, the USB keyboard enumerates, and game speed is now correct** (NTSC is
59.92 Hz; at 52 Hz everything ran ~13 % slow). Build: `env:pizero_60hz`.

**Audio at 60 Hz** then fits at the same 252 MHz without a bigger overclock: a
standard 800-total line split as `h_fp=8 / h_sync=96 / h_bp=56 / h_active=640`
gives a 28-word back porch — just enough for one streaming island — and
`800 × 525 × 60 = 25.2 MHz`. Set the cadence to `800 samples/frame` (48000/60).
Open question worth checking: at a **standard** 60 Hz clock the sink timebase is
in-spec, which may also reduce the PIZERO-41 residual.

---

## Pointers
- Encoder: `lib/libdvi/dvi_data_island.{c,h}` (ported from shuichitakano/pico_lib).
- Streaming delivery + meter + ring consumer: `src/main.cpp` (`stream_audio_cb`,
  `HDMI_STREAM_AUDIO`).
- Source resampler + SPSC ring: `lib/coco_machine/src/coco_machine.cpp`
  (`audio_emit` / `audio_integrate` / `coco_machine_audio_read`).
- Build envs & flag matrix: `docs/BUILD.md`. Original scoping: `audio-decision.md`.

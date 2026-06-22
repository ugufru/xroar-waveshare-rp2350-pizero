# Signal pipeline: emulated CoCo → HDMI

How the emulated Tandy CoCo's screen and sound become an HDMI signal on the
Waveshare RP2350-PiZero, end to end. This is the **map** of the whole path; the
deep dives live elsewhere and are linked inline:

- Audio internals & hard-won gotchas → [`hdmi-audio-notes.md`](hdmi-audio-notes.md)
- Original audio scoping/decision record → [`audio-decision.md`](audio-decision.md)
- Build envs & flags (authoritative) → [`BUILD.md`](BUILD.md)
- Hardware, pinout, clock rationale → [`../README.md`](../README.md)

There is no HDMI transmitter chip and no usable HSTX path on this board (HSTX is
GPIO 12–19; the HDMI connector is on GPIO 32–39), so **everything below is software
TMDS via `libdvi` (PIO + DMA)**. Audio rides inside that same TMDS stream as HDMI
data islands — no separate audio hardware.

## Overview — the two-core split

```
core 0  (emulation + audio generation)        core 1  (display + audio encode)
──────────────────────────────────────        ───────────────────────────────────
6809 + SAM + PIA + VDG                          libdvi worker: TMDS-encode each
  │                                             scanline → DMA → PIO → GPIO 32–39
  ├─ VDG 256×192 palette-indexed                  ▲
  │    └─ blit → 320×240 RGB565 ───── g_front ─────┘   (sampled once per frame)
  │
  └─ 6-bit DAC (PIA1) integrate-and-dump
       └─ 48 kHz mono → SPSC ring ───── g_audio_w/g_audio_r ──┐
                                                              ▼
                                          per active line, in the DMA IRQ:
                                          meter 0–4 samples → build audio
                                          island (TERC4/BCH) → repoint DMA
                                          (ACR + InfoFrames sit on vblank lines)
```

- **core 0** runs the emulator and produces both a video framebuffer and a stream of
  audio samples.
- **core 1** does nothing but drive the display: it re-encodes the framebuffer to TMDS
  forever, and (in the audio builds) encodes one audio data island per active scanline
  from inside the DVI DMA IRQ.
- Two lock-free hand-offs connect them: a published framebuffer pointer (`g_front`) and
  a single-producer/single-consumer audio ring (`g_audio_w`/`g_audio_r`).

Core launch: `core1_main()` registers the DVI IRQs, starts `libdvi`, and enters the
static-framebuffer worker that never returns
(`src/main.cpp:135`, `multicore_launch_core1` at `src/main.cpp:976`).

## Video path

### 1. VDG output (core 0)

XRoar's VDG renders the native CoCo image: **256×192, palette-indexed, nibble-packed**
(two pixels per byte). `coco_machine_get_vdg_buffer()` returns a stable pointer to it
(`lib/coco_machine/src/coco_machine.h:19` for `COCO_VDG_W`/`COCO_VDG_H`, `:73` for the
getter). Each frame core 0 calls `coco_machine_run_cycles()` then
`coco_machine_render_frame()` to regenerate it (`src/main.cpp:1068`, `:1071`).

### 2. Blit to RGB565 (core 0)

`coco_boot_blit_vdg_pizero_src()` (`src/coco_boot.cpp:496`) converts palette indices to
a **320×240 RGB565** framebuffer via a 16-entry LUT (`g_vdg_rgb565_native`,
`src/coco_boot.cpp:488`). Key properties:

- **Native byte order** — `libdvi` consumes it directly, no wire byte-swap (unlike the
  AMOLED port).
- The 256×192 image is **centered at (32, 24)**, leaving a static black border. The
  border is painted once at init and never re-cleared, so only the active region is
  written each frame.
- After `libdvi`'s 2× scale-out this appears as **512×384 inside 640×480**.

### 3. Double-buffer hand-off (core 0 → core 1)

The video builds use two framebuffers: core 0 blits into the back buffer, then
**atomically publishes** `g_front = g_fb[g_back]` at the frame boundary and flips
(`src/main.cpp:1077`–`:1079`). `g_front` is `volatile` so core 1 always reads the
latest pointer. Core 1 samples it **once per frame**, so swaps are **tear-free**.

The audio build single-buffers instead (`src/main.cpp:125`) to reclaim ~150 KB RAM,
trading tear-free for never-starving: a slow producer just tears, it never drops to a
solid-color glitch.

### 4. TMDS encode + scanout (core 1)

`dvi_static_framebuf_main_16bpp()` (`lib/libdvi/dvi.cpp:181`) reads the 320×240 buffer
with `stride = h_active/DVI_SYMBOLS_PER_WORD = 320`. Each line is TMDS-encoded into the
three data channels by `_dvi_prepare_scanline_16bpp()` (`lib/libdvi/dvi.cpp:129`) via
`tmds_encode_data_channel_16bpp` ×3 (B/G/R). With `DVI_VERTICAL_REPEAT = 2`, each of the
240 logical lines is output twice → 480 physical lines; combined with pixel doubling
this scales **320×240 → 640×480**.

### 5. Pins / PIO / DMA (core 1)

DMA streams the encoded TMDS words into three PIO state machines that serialize 10-bit
symbols onto the pins. Pin map is `pico_sock_cfg`
(`lib/libdvi/common_dvi_pin_configs.h:64`): TMDS data on **36/34/32**, clock on **38**.
`pio_set_gpio_base(pio, 16)` (`src/main.cpp:819`) is required because the TMDS pins live
above GPIO 31. See the README pinout table for the full differential-pair wiring; it is
not duplicated here.

## Audio path

Audio is carried as **HDMI data islands** in the blanking intervals. This section is a
summary — see [`hdmi-audio-notes.md`](hdmi-audio-notes.md) for the full architecture,
rationale, and gotchas.

### 1. Source (core 0)

The CoCo's **6-bit DAC** (PIA1 port A) level is recached on PIA writes
(`audio_update_level`, `lib/coco_machine/src/coco_machine.cpp:691`) and integrated over
CPU memory-access time (`audio_integrate`, `:727`) — an integrate-and-dump resampler
that emits **48 kHz / 16-bit mono** samples (`COCO_AUDIO_RATE`, `:651`). Samples go into
an 8192-deep **SPSC ring** (`audio_emit`, `:699`). Core 0 owns the write index
`g_audio_w` and publishes with a `__dmb()`; the consumer owns `g_audio_r`.

### 2. HDMI carriage

Audio sample packets are TERC4-encoded with BCH parity into data islands
(`lib/libdvi/dvi_data_island.c`). **ACR** (clock regeneration) and the **AVI / Audio
InfoFrames** are emitted on the vblank lines.

### 3. Streaming delivery (core 1, in the DMA IRQ)

`stream_audio_cb()` (`src/main.cpp:556`) runs per active scanline. A 16.16 fixed-point
meter (`g_meter_step`, set at `src/main.cpp:921`) releases **0–4 samples per line**,
spreading `AUDIO_SAMPLES_PER_FRAME` evenly across the 480 active lines. It reads those
samples from the ring (`coco_machine_audio_read`,
`lib/coco_machine/src/coco_machine.cpp:756`), builds and parity-stamps one island,
encodes it into a rotating buffer pool, and repoints the back-porch DMA at it. This
even, per-line feed is what eliminated the warble of the older pre-encoded-bank scheme.

## How video and audio timing interlock

The two halves are coupled by the **horizontal back-porch budget**, which is the
genuinely cross-cutting constraint:

- One audio data island costs ~27 TMDS words and must fit in the back-porch of an active
  line.
- The **60 Hz** product timing (`HDMI_60HZ`) uses an 800-pixel-total line split
  `8 / 96 / 56 / 640` — its **56-pixel (28-word) back-porch just fits one island**
  (`src/main.cpp:76`, `:86`). The older off-spec **~52 Hz** timing has a roomy 130-pixel
  back-porch (`src/main.cpp:90`). So audio capacity is directly tied to the video
  geometry.
- Samples-per-frame follows the refresh: **800** at 60 Hz, **924** at ~52 Hz
  (`AUDIO_SAMPLES_PER_FRAME`, `src/main.cpp:500`/`:502`) — both = 48000 ÷ refresh, so the
  ring producer and the per-line meter stay in lockstep with no drift.

Clock side: the default build runs sysclk **252 MHz → 25.2 MHz pixel → 800×525 = true
60.0 Hz**, which also makes the PIO-USB dividers exact (252/48 = 5.25). The off-spec
240 MHz / 24 MHz / ~52 Hz timing is kept only as a fallback. See the README
"System-clock reconciliation" section for why.

## Build environments

Authoritative reference is [`BUILD.md`](BUILD.md); this is the orientation table.

| Env                 | Video            | Audio                       | Notes                              |
|---------------------|------------------|-----------------------------|------------------------------------|
| `pizero_stream_60`  | true 640×480p60  | streaming HDMI audio        | **product default** (PIZERO-45)    |
| `pizero`            | 640×480 (~52 Hz) | silent                      | video-only baseline                |
| `pizero_stream`     | 640×480 (~52 Hz) | streaming, off-spec 52 Hz   | fallback                           |
| `pizero_60hz`       | true 640×480p60  | silent                      | 60 Hz video-only test              |

## Source map

| Stage                         | File : symbol                                                        |
|-------------------------------|---------------------------------------------------------------------|
| VDG geometry / buffer         | `lib/coco_machine/src/coco_machine.h:19` / `coco_machine_get_vdg_buffer` |
| Frame drive                   | `src/main.cpp:1068` `coco_machine_run_cycles` / `coco_machine_render_frame` |
| RGB565 blit + palette LUT     | `src/coco_boot.cpp:496` `coco_boot_blit_vdg_pizero_src`              |
| Double-buffer publish         | `src/main.cpp:1077` (`g_front` / `g_back`)                           |
| TMDS scanline encode          | `lib/libdvi/dvi.cpp:129` `_dvi_prepare_scanline_16bpp`               |
| Static-framebuffer worker     | `lib/libdvi/dvi.cpp:181` `dvi_static_framebuf_main_16bpp`            |
| DVI timing struct             | `src/main.cpp:71` `dvi_timing_640x480p_57hz_240mhz`                  |
| Pin config / PIO base         | `lib/libdvi/common_dvi_pin_configs.h:64` `pico_sock_cfg` / `src/main.cpp:819` |
| Core 1 launch                 | `src/main.cpp:135` `core1_main` / `:976` `multicore_launch_core1`    |
| Audio source / resampler      | `lib/coco_machine/src/coco_machine.cpp:691` `audio_update_level` / `:727` `audio_integrate` |
| Audio ring                    | `lib/coco_machine/src/coco_machine.cpp:699` `audio_emit` / `:756` `coco_machine_audio_read` |
| Data-island encoder           | `lib/libdvi/dvi_data_island.c`                                       |
| Per-line streaming delivery   | `src/main.cpp:556` `stream_audio_cb` / `:921` `g_meter_step`         |

## See also

- [`hdmi-audio-notes.md`](hdmi-audio-notes.md) — audio architecture, rationale, gotchas
- [`cpu-speed.md`](cpu-speed.md) — emulation pacing & CoCo high-speed (double-speed) behavior
- [`audio-decision.md`](audio-decision.md) — original scoping/decision record
- [`BUILD.md`](BUILD.md) — build envs and the full flag matrix
- [`../README.md`](../README.md) — hardware, pinout, clock reconciliation

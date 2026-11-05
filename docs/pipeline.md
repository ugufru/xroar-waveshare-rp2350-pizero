# Signal pipeline: emulated CoCo → DVI

How the emulated Tandy CoCo's screen and sound become a DVI signal on the
Waveshare RP2350-PiZero, end to end. This is the **map** of the whole path; the
deep dives live elsewhere and are linked inline:

- The sound chips (SN76489, the AY-3-8910 cartridges, stereo) → [`sound.md`](sound.md)
- Audio internals & hard-won gotchas → [`video-audio-notes.md`](video-audio-notes.md)
- Original audio scoping/decision record → [`audio-decision.md`](audio-decision.md)
- Build envs & flags (authoritative) → [`BUILD.md`](BUILD.md)
- Hardware, pinout, clock rationale → [`../README.md`](../README.md)

There is no video transmitter chip and no usable HSTX path on this board (HSTX is
GPIO 12–19; the mini video port is on GPIO 32–39), so **everything below is software
TMDS via `libdvi` (PIO + DMA)**. Audio rides inside that same TMDS stream as
data islands, with no separate audio hardware.

## Overview: the two-core split

```
core 0  (emulation + audio generation)        core 1  (display + audio encode)
──────────────────────────────────────        ───────────────────────────────────
6809 + SAM + PIA + VDG                          libdvi worker: TMDS-encode each
  │                                             scanline → DMA → PIO → GPIO 32–39
  ├─ VDG 256×192 palette-indexed                  ▲
  │    └─ blit → 320×240 RGB565 ───── g_front ─────┘   (sampled once per frame)
  │
  └─ 6-bit DAC (PIA1) integrate-and-dump (+ SN76489, + AY bank)
       └─ 48 kHz L/R → SPSC ring ┬──── g_audio_w/g_audio_r ──┐
                                 │                            ▼
                                 │        per line (active + vblank), DMA IRQ:
                                 │        meter → full 4-sample packet every
                                 │        ~2.6 lines → audio island → repoint
                                 │        (ACR + InfoFrames every 8th vblank line)
                                 │
                                 └─ or, with a USB headphone DAC (PIZERO-255):
                                    USB frame interrupt, core 0, every 1 ms:
                                    48 pairs → isochronous OUT packet
                                    (the video stream then sends silence)
```

- **core 0** runs the emulator and produces both a video framebuffer and a stream of
  audio samples.
- **core 1** drives the display: it re-encodes the framebuffer to TMDS
  forever, and (in the audio builds) encodes audio data islands from inside the DVI
  DMA IRQ, spread over the active and vblank lines (PIZERO-232). As a CoCo 3 it also
  decodes the GIME's changed lines into the framebuffer while it waits for a free
  TMDS buffer (PIZERO-330, step 2a).
- Lock-free hand-offs connect them: a published framebuffer pointer (`g_front`), a
  single-producer/single-consumer audio ring (`g_audio_w`/`g_audio_r`), and as a
  CoCo 3 a ring of line decode jobs (`g_c3jobs`).

Core launch: `core1_main()` in `src/main.cpp` registers the DVI IRQs, starts
`libdvi`, and enters the static-framebuffer worker that never returns; `setup()`
starts it with `multicore_launch_core1(core1_main)`.

## Video path

### 1. VDG output (core 0)

XRoar's VDG renders the native CoCo image: **256×192, palette-indexed, nibble-packed**
(two pixels per byte). `coco_machine_get_vdg_buffer()` returns a stable pointer to it
(`COCO_VDG_W`/`COCO_VDG_H` and the getter are in `lib/coco_machine/src/coco_machine.h`).
Each frame the main loop in `src/main.cpp` calls
`coco_machine_run_cycles(CYCLES_PER_FRAME)` then `coco_machine_render_frame()` to
regenerate it.

### 2. Blit to the framebuffer (core 0, as a CoCo 2)

Since PIZERO-135 the product is either machine, and both draw into one framebuffer:
**640×240, one byte a pixel**, a colour number that core 1 looks up in `g_pal565[256]`
as it encodes the line (step 4). As a CoCo 2, `coco_boot_blit_vdg_pizero8()`
(`src/coco_boot.cpp`) writes each VDG pixel twice, as index 80 + its palette entry,
and first copies the 16-entry palette, `coco_machine_palette()`, into
`g_pal565[80..95]`. That palette is the writable register file in `coco_machine.cpp`
that also sees the guest's GIME-style palette writes to `$FFB0-$FFBF` and holds the
`color_*` settings (PIZERO-55/85). Key properties:

- **Four bytes a store:** two packed source pixels become four output bytes through
  a 256-entry table rebuilt each frame; about 0.9 ms a frame.
- The 256×192 image is **centered at (64, 24) in bytes**, leaving a static black
  border. The border is painted once at init and never re-cleared, so only the
  active region is written each frame.
- With each line shown twice this appears as **512×384 inside 640×480**.

(Builds without `VIDEO_640` keep the older path: `coco_boot_blit_vdg_pizero_src()`
into a 320×240 RGB565 framebuffer that `libdvi` pixel-doubles.)

### 2a. As a CoCo 3 instead (core 0, `machine = coco3`)

As a CoCo 3 (PIZERO-133) there is no VDG buffer, no
`coco_machine_render_frame` and no blit: the main loop sets `render` and `blit`
to zero. The vendored GIME (`lib/xroar_core/src/tcc1014/tcc1014.c`) is the bus
and draws while the 6809 runs:

- **Drawing:** at each scanline's horizontal sync (`do_hs_fall`), the port
  renderer (`port_render_scanline`, PIZERO-218) hands over 640 GIME colours
  (`TCC1014_PORT_FULL`, PIZERO-134):
  - **Active lines:** decoded from VRAM read directly (`port_vram`).
  - **Other lines:** border, or black in vertical sync.
- **Skipping:** a line whose inputs (VRAM bytes, mode, palette, border) hash
  the same as last frame's (`port_sum`) is passed as NULL. That keeps static
  screens cheap.
- **Decode on core 1 (PIZERO-330):** a changed active line is not decoded on
  core 0. `port_render_scanline` gathers its VRAM bytes and fills a
  `struct tcc1014_port_job` (the bytes, mode and palette); `do_hs_fall` hands it
  to `render_line` as npixels `TCC1014_PORT_JOB`. `coco3_render_line`
  (`lib/coco_machine/src/coco_machine.cpp`) takes its framebuffer row
  (`coco3_bus_job_dst`, which clears the row's hash) and puts the job in a
  4-slot ring. Core 1 takes jobs in `dvi_idle_work` while it waits for libdvi's
  next free TMDS buffer, so scanout comes first, and runs
  `tcc1014_port_decode` straight into the row. When the ring is full core 0
  decodes the line itself. The profiling build's `[prof3] core1` line gives
  core 1's decode share and the ring-full count.
- **Decode:** each bitplane graphics mode and resolution has its own loop
  (`PORT_LOOP`, PIZERO-220), in `tcc1014_port_decode`.
- **Sink:** `coco3_bus_render_line` (`lib/coco_machine/src/coco3_bus.h`)
  copies the other lines (border, sync, and every line in builds without
  `VIDEO_640`) into `g_fb` as they are (GIME colours are `g_pal565[0..63]`),
  centring 192 lines at row 24 and 225 at row 7. It has its own per-row hash
  as well.
- **Redraw:** anything else that draws into `g_fb` (the overlay) calls
  `coco_machine_video_invalidate`, which forces a full redraw.

The picture is drawn while the beam moves, so it can tear: the HCLS colour
bands moving down the screen are what a real CoCo 3 shows too (PIZERO-217).
640-wide modes show whole (PIZERO-134): core 1 looks each byte up in the
256-entry RGB565 palette as it encodes the line on the RP2350's SIO hardware
TMDS encoder (`dvi_static_framebuf_main_pal8`, `tmds_encode_pal8_sio` in
`lib/libdvi`).

### 3. Framebuffer hand-off (core 0 → core 1)

Every shipping build defines `AV_DATA_ISLAND` and so **single-buffers**: one
`g_fb`, which `g_front` points at permanently, and core 0 blits straight into it (see
`g_fb` under `#ifdef AV_DATA_ISLAND` in `src/main.cpp`). That reclaims ~150 KB RAM
for the audio islands, trading tear-free for never-starving: core 1 may scan a line
mid-blit and tear, but it never drops to a solid-color glitch.

Only a build with neither `AV_DATA_ISLAND` nor `FB_SINGLE` double-buffers, and no
env does since PIZERO-131 made `pizero_wavmeas` single-buffered. There core 0 blits into the back buffer, then
**publishes** `g_front = g_fb[g_back]` at the frame boundary and flips `g_back`.
`g_front` is `volatile` so core 1 always reads the latest pointer; it samples it
**once per frame**, so those swaps are tear-free.

### 4. TMDS encode + scanout (core 1)

`dvi_static_framebuf_main_pal8()` (`lib/libdvi/dvi.cpp`, PIZERO-134) reads the
640×240 buffer of one-byte colours a line at a time. `tmds_encode_pal8_sio()`
(`lib/libdvi/tmds_encode.cpp`) looks each pair of pixels up in the 256-entry RGB565
palette `g_pal565` (0-63 the GIME's colours, 64-65 the overlay's ink and paper, 80-95
the CoCo 2's palette) and feeds them to the RP2350's hardware TMDS encoder in the SIO
block, one pass for all three lanes: 23.7 µs for 640 pixels in PIZERO-175's bench,
against 63.5 µs a line shown twice. With `DVI_VERTICAL_REPEAT = 2`, each of the 240
logical lines is output twice → 480 physical lines, so the picture is **640×480**
with every horizontal pixel its own.

`video_encoder` in `settings.txt` has no effect on this path, which always uses the
hardware encoder (PIZERO-240). Builds without `VIDEO_640` keep the older worker,
`dvi_static_framebuf_main_16bpp()`, which pixel-doubles a 320×240 RGB565 buffer with
`tmds_encode_16bpp_sio_doubled` (about 12.6 µs a line) or, with
`video_encoder = software`, libdvi's table encoder (about 25.2 µs).

### 5. Pins / PIO / DMA (core 1)

DMA streams the encoded TMDS words into three PIO state machines that serialize 10-bit
symbols onto the pins. Pin map is `pico_sock_cfg`
(`lib/libdvi/common_dvi_pin_configs.h`): TMDS data on **36/34/32**, clock on **38**.
`pio_set_gpio_base(pio, 16)` (in `setup()`, `src/main.cpp`) is required because the TMDS pins live
above GPIO 31. See the README pinout table for the full differential-pair wiring; it is
not duplicated here.

## Audio path

Audio is carried as **audio data islands** in the blanking intervals. This section is a
summary; see [`video-audio-notes.md`](video-audio-notes.md) for the full architecture,
rationale, and gotchas.

### 1. Source (core 0)

All in `lib/coco_machine/src/coco_machine.cpp`. The CoCo's **6-bit DAC** (PIA1 port
A) level is recached on PIA writes (`audio_update_level`) and integrated over CPU
memory-access time (`audio_integrate`): an integrate-and-dump resampler that emits
**48 kHz / 16-bit mono** samples (`COCO_AUDIO_RATE`).

The integrator is not called on every access. Most cycles take the 6809 core's fast
path (`coco_fastbus.h`, PIZERO-131) and only advance time. The audio for them is
caught up the next time a cycle takes the full path: at the next event, which comes at
least once a scanline, or at a level change (`audio_catch_up`). Since PIZERO-220 that
catch-up emits every sample boundary the run crossed (`audio_integ_run` in
`audio_integ.h`), exactly as stepping each access would; `test_audio_integ` checks it
sample for sample. The samples reach the ring in small bursts, which the 4096-deep
ring absorbs.

The **SN76489** sound chip (PIZERO-143) is the second source. Writes to `$FF41` reach
it through `csg_bus_write`, either from a Game Master Cartridge or, with the
`sn76489` setting on (the default), with no such cartridge. `audio_integrate` adds
its output (`sn_render`, scaled by the `volume` setting) to the DAC sample before
emitting it. The **AY-3-8910 bank** (PIZERO-149: the CoCo PSG, Symphony-12 and
Deluxe CoCo cartridges) is the third, added the same way in `audio_sample`
(`ayb_render_lr`). Since PIZERO-250 a sample is a left and right pair: the DAC
and the SN76489 go to both sides, and the Symphony-12 in stereo puts its first
two chips on the left and the other two on the right. The chips, their
addresses, settings and costs are in [`sound.md`](sound.md).

`audio_emit` then shapes each side on its way to the ring (PIZERO-223, PIZERO-212;
the right side's filters run only while the two sides differ, PIZERO-250):
a **DC blocker** (`audio_dc_run`, a one-pole high-pass near 5 Hz) makes the output
AC coupled like the real CoCo's, so a parked DAC no longer sits as an offset taking
headroom; then the **`audio_lowpass`** filter (`audio_lp_run`, 8 kHz by default)
softens the hard square-wave steps; then a clamp. Both filters are in
`lib/coco_machine/src/audio_lowpass.h`. The level is set by `AUDIO_DAC_GAIN` (360)
and `AUDIO_GAIN_X` (6, the SN76489's matching factor) in `coco_machine.cpp`, six
times the pre-PIZERO-223 level (PIZERO-223 tripled it, PIZERO-230 doubled it), scaled
by the `volume` setting. At volume 10 a full DAC swing peaks near -3 dBFS in the
worst AC-coupled case; volumes above 10 may clip on such waveforms.

Samples go into a 4096-deep **SPSC ring** of left and right pairs (`audio_emit`,
`AUDIO_RING_SAMPLES`; `coco_machine_audio_read_lr` reads pairs, and
`coco_machine_audio_read` their average for the WAV dump).
Core 0 owns the write index `g_audio_w` and publishes with a `__dmb()`; the consumer
owns `g_audio_r`.

### 2. Data-island carriage

Audio sample packets are TERC4-encoded with BCH parity into data islands
(`lib/libdvi/dvi_data_island.c`). **ACR** (clock regeneration) and the **AVI / Audio
InfoFrames** are emitted on every 8th vblank line.

Delivery (PIZERO-232): the meter spreads a frame's 800 samples over all 523
non-sync lines, so the sink never sees the ~1.4 ms gap a frame that audio on the
active lines alone left, and it sends only full 4-sample packets, as an ordinary
source does. An active line carries the island in its back porch; a vblank line
carries one island in its active block (audio, or the info island), never in its
back porch, which blanks the dev monitor (PIZERO-34). Each vblank island ends
with its trailing guard band. The old delivery (1-2 samples on nearly every
active line, the info island on every vblank line, about 2,700 ACR a second)
garbled the sound on one monitor that played a Mac's audio cleanly.

Each sample subframe follows IEC 60958 (PIZERO-223, `dvi_di_set_audio_samples`):
validity 0, a consumer channel status (linear PCM, 48 kHz, 16 bits) sent one bit a
frame from a RAM table, and the block-start flag on whichever subpacket starts each
192-frame block. ACR (N 6144, CTS 25200 at 25.2 MHz), the Audio InfoFrame and the
channel status all declare the same 48 kHz. The inherited packet had set V=1 and
sent no channel status, which a TV may treat as unreliable or 44.1 kHz audio.

### 3. Streaming delivery (core 1, in the DMA IRQ)

`stream_audio_cb()` (`src/main.cpp`) runs per active scanline. An exact meter
counting in 480ths of a sample (`g_meter_acc`, step `AUDIO_SAMPLES_PER_FRAME`; it was
16.16 fixed point until PIZERO-223, which truncated to 47,999.7 samples/s) releases
**0–4 samples per line**,
spreading `AUDIO_SAMPLES_PER_FRAME` evenly across the 480 active lines. It reads those
samples from the ring (`coco_machine_audio_read_lr`, `coco_machine.cpp`), builds and
parity-stamps one island,
encodes it into a rotating buffer pool, and repoints the back-porch DMA at it. This
even, per-line feed is what eliminated the warble of the older pre-encoded-bank scheme.

## How video and audio timing interlock

The two halves are coupled by the **horizontal back-porch budget**, which is the
genuinely cross-cutting constraint:

- One audio data island costs ~27 TMDS words and must fit in the back-porch of an active
  line.
- The **60 Hz** product timing (`AV_60HZ`) uses an 800-pixel-total line split
  `8 / 96 / 56 / 640`: its **56-pixel (28-word) back-porch just fits one island**.
  The older off-spec **~52 Hz** timing has a roomy 130-pixel back-porch. Both are
  `#if` branches of the one timing struct in `src/main.cpp` (see the source map). So
  audio capacity is directly tied to the video geometry.
- Samples-per-frame follows the refresh: **800** at 60 Hz, **924** at ~52 Hz
  (`AUDIO_SAMPLES_PER_FRAME`, `src/main.cpp`), both = 48000 ÷ refresh, so the
  ring producer and the per-line meter stay in lockstep with no drift.

Clock side: the default build runs sysclk **252 MHz → 25.2 MHz pixel → 800×525 = true
60.0 Hz**, which also makes the PIO-USB dividers exact (252/48 = 5.25). The off-spec
240 MHz / 24 MHz / ~52 Hz timing is kept only as a fallback. See the README
"System-clock reconciliation" section for why.

## Build environments

Authoritative reference is [`BUILD.md`](BUILD.md); this is the orientation table.

| Env                 | Video            | Audio                       | Notes                              |
|---------------------|------------------|-----------------------------|------------------------------------|
| `pizero_stream_60`  | true 640×480p60  | streaming data-island audio | **product default** (PIZERO-45)    |
| `pizero_stream`     | 640×480 (~52 Hz) | streaming, off-spec 52 Hz   | fallback                           |

The silent `pizero` and `pizero_60hz` envs were retired in PIZERO-150. The remaining
envs are diagnostics; see [`BUILD.md`](BUILD.md).

## Source map

Symbols rather than line numbers, since the files move.

| Stage                         | File : symbol                                                        |
|-------------------------------|---------------------------------------------------------------------|
| CoCo 3 scanline render       | `lib/xroar_core/src/tcc1014/tcc1014.c` : `port_render_scanline` / `do_hs_fall` / `tcc1014_port_decode`; sink `lib/coco_machine/src/coco3_bus.h` : `coco3_bus_render_line`; core 1's decode `lib/coco_machine/src/coco_machine.cpp` : `coco_machine_core1_work`, via `dvi_idle_work` in `lib/libdvi/dvi.cpp` |
| VDG geometry / buffer         | `lib/coco_machine/src/coco_machine.h` : `COCO_VDG_W` / `coco_machine_get_vdg_buffer` |
| Frame drive                   | `src/main.cpp` : `coco_machine_run_cycles` / `coco_machine_render_frame` (main loop) |
| RGB565 blit                   | `src/coco_boot.cpp` : `coco_boot_blit_vdg_pizero_src`                |
| Palette                       | `lib/coco_machine/src/coco_machine.cpp` : `coco_machine_palette`     |
| Framebuffer / publish         | `src/main.cpp` : `g_fb` / `g_front` (`g_back` only without `AV_DATA_ISLAND`) |
| TMDS scanline encode          | `lib/libdvi/tmds_encode.cpp` : `tmds_encode_pal8_sio` (`VIDEO_640`)   |
| Static-framebuffer worker     | `lib/libdvi/dvi.cpp` : `dvi_static_framebuf_main_pal8` (`VIDEO_640`) |
| DVI timing struct             | `src/main.cpp` : `dvi_timing_640x480p_57hz_240mhz` (aliased as `DVI_TIMING`). Despite the name it carries every build's timing through `#if` branches, the 60 Hz product included. libdvi's stock `dvi_timing_640x480p_60hz` (`lib/libdvi/dvi_timing.cpp`) is not used. |
| Pin config / PIO base         | `lib/libdvi/common_dvi_pin_configs.h` : `pico_sock_cfg` / `src/main.cpp` : `pio_set_gpio_base` |
| Core 1 launch                 | `src/main.cpp` : `core1_main` / `multicore_launch_core1`             |
| Audio source / resampler      | `lib/coco_machine/src/coco_machine.cpp` : `audio_update_level` / `audio_integrate` / `audio_catch_up`; `audio_integ.h` |
| SN76489                       | `lib/coco_machine/src/coco_machine.cpp` : `csg_bus_write`; chip model `csg_sn76489.h` |
| AY-3-8910 bank                | `lib/coco_machine/src/coco_machine.cpp` : `ayb_owns` / `ayb_write` in the bus paths; chips and front-ends `psg_ay8910.h` |
| Audio mix and ring            | `lib/coco_machine/src/coco_machine.cpp` : `audio_sample` / `audio_emit` / `coco_machine_audio_read_lr` |
| Data-island encoder           | `lib/libdvi/dvi_data_island.c`                                       |
| Per-line streaming delivery   | `src/main.cpp` : `stream_audio_cb` / `g_meter_step`                  |
| USB headphone DAC             | `src/usb_audio.cpp`; `src/main.cpp` : `usb_audio_fill`; `lib/pico_pio_usb` isochronous OUT ([`sound.md`](sound.md)) |

## See also

- [`sound.md`](sound.md): the sound chips, their addresses, settings and costs
- [`video-audio-notes.md`](video-audio-notes.md): audio architecture, rationale, gotchas
- [`cpu-speed.md`](cpu-speed.md): emulation pacing & CoCo high-speed (double-speed) behavior
- [`audio-decision.md`](audio-decision.md): original scoping/decision record
- [`BUILD.md`](BUILD.md): build envs and the full flag matrix
- [`../README.md`](../README.md): hardware, pinout, clock reconciliation

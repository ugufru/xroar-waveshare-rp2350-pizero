# BUILD.md: how to build, flash, and configure this firmware

XRoar (Tandy CoCo) on the **Waveshare RP2350-PiZero**. This is the single source
of truth for **how we build it**: the PlatformIO envs, the video/audio build-flag
matrix, flashing + serial, and the toolchain gotchas that have bitten past
sessions. **If you add or change a build flag, update this file.**

> Hardware specs, pinout, and the libdvi/DVI design rationale live in
> `README.md` and `docs/`. This file is only about the build.

---

## 1. Quick start

```bash
# PRODUCT build: true 640x480p60 + streaming data-island audio + USB. This is the
# committed default env (PIZERO-45), so a bare `pio run` builds it:
pio run                                 # == pio run -e pizero_stream_60
pio run -t upload                       # flash (hold BOOT if upload fails)
pio device monitor                      # serial @ 115200, prints [run] telemetry

# Fallback for a display that rejects 60 Hz (kept until PIZERO-99 proves 60 Hz
# widely): the older off-spec 24 MHz / ~52 Hz timing. It does NOT build the
# GIME palette or timer (-DGIME_PALETTE / -DGIME_TIMER), so the gime_palette
# and gime_timer settings do nothing on it.
pio run -e pizero_stream -t upload

# Host unit tests (no board): 28 suites, 337 cases.
pio test -e native
```

Upload puts the RP2350 into BOOTSEL automatically via `picotool`; if it won't,
hold the **BOOT** button while plugging USB-C, then re-run upload.

---

## 2. Build environments (`platformio.ini`)

We define **one env per configuration**. Do **not** toggle features with one-off
`-D` flags passed through the `PLATFORMIO_BUILD_FLAGS` env var (see the cache
trap in §4). Add a new env that extends `pizero_base` (or `env:pizero_stream`)
instead. `pizero_base` holds the settings every firmware env shares; it is not
buildable on its own.

| Env | Extends | Adds | Framebuffer | Audio | RAM |
|-----|---------|------|-------------|-------|-----|
| **`pizero_stream_60`** | `pizero_stream` | `-DAV_60HZ -DGIME_TIMER -DGIME_PALETTE` (and the PIZERO-232/233 audio flags) `-DCOCO3 -DVIDEO_640 -DTCC1014_PORT_FULL` | single-buffered | **THE PRODUCT: streaming audio @ true 640x480p60 + USB (PIZERO-45). Either machine (PIZERO-135): `machine = cocozero \| coco2 \| coco3` in settings.txt or a game's own file picks the CoCo Zero (the default), a stock CoCo 2 or the 128 KB CoCo 3 (PIZERO-242), and a switch to or from the CoCo 3 restarts the board** | ~81% |
| **`pizero_stream`** | `pizero_base` | `-DAV_DATA_ISLAND -DAV_STREAM_AUDIO` | single-buffered | streaming audio at the older ~52 Hz timing: the fallback. No GIME palette or timer | ~75% |
| `pizero_usbdebug` | `pizero_stream_60` | `-DCFG_TUSB_DEBUG=2 -DCFG_TUD_LOG_LEVEL=3 -DCFG_TUSB_DEBUG_PRINTF=tusb_debug_printf -DSERIAL_TUSB_DEBUG=Serial` | single-buffered | product + TinyUSB host log on the serial console, for hub and enumeration faults (PIZERO-54) | ~75% |
| `pizero_padprobe` | `pizero_stream_60` | `-DPAD_PROBE=1` | single-buffered | product + gamepad report dump on serial, for measuring a pad's buttons (PIZERO-13, 164) | ~75% |
| `pizero_fdctrace` | `pizero_stream_60` | `-DFDC_TRACE=1` | single-buffered | product + every $FF40 latch change and FDC command, the 6809 PC once a second, the first 200 writes to $FF90-$FFDF (`[fdc]`), a hex dump of each track 34 sector read (`[t34]`), a checksum of every sector read and the time of every sector written (`[dsk] rd`, `[dsk] write`, PIZERO-194), a jump recorder that dumps on a runaway into $8000-$EDFF (`[pctrace]`), and the 32x16 text screen as ASCII every 5 s (`[scr]`), for finding where a disk OS stops (PIZERO-190, 191) | ~75% |
| `pizero_fdctrace_uart` | `pizero_fdctrace` | `-DFDC_TRACE_UART=1 -DFDC_TRACE_PORT=Serial1` | single-buffered | `pizero_fdctrace` with the trace on UART0 (GPIO 0/1) instead of USB, for the cocozero emulator (`~/github/cocozero-emulator`, `--headless --uf2 .pio/build/pizero_fdctrace_uart/firmware.uf2`), which prints UART0 but not USB serial (PIZERO-191). Stage a folder for `--sd` with `coco/roms`, `coco/dsk`, `coco/drives.txt` and `coco/autorun.txt` (`~` pauses a second between typed lines); the product build runs there, but this env has crashed at boot in the emulator since PIZERO-66 (PIZERO-198), so until that is found the trace runs on hardware only | ~76% |
| `pizero_videobench` | `pizero_stream_60` | `-DVIDEO_BENCH -DTMDS_FULLRES_IN_SRAM=1` | single-buffered | product + TMDS encoder timings per scanline (`[bench]` every 5 s), libdvi's software encoders and the RP2350's SIO hardware encoder, with a decode self-check, for a 640-wide picture (PIZERO-175). The fullres and palette encoders run from main SRAM, as they do not fit in scratch X/Y beside the core stacks | ~80% |
| `pizero_coco3` | `pizero_stream_60` | (nothing) | single-buffered | the product under the name the CoCo 3 build had before PIZERO-135 made the product either machine, kept so older notes and scripts still build. Set `machine = coco3` for a CoCo 3 | ~81% |
| `pizero_cpu179` | `pizero_stream_60` | `-DCPU_SPEED_MUL=2` | single-buffered | product running twice the 6809 cycles per frame: the CPU load of a CoCo 3 at 1.79 MHz, the baseline for the speed work (PIZERO-130, 131). The whole machine runs fast and the audio ring overflows, so it is for measuring `cpu=`, not for use | ~75% |
| `pizero_usbdump` | `pizero_stream_60` | `-DUSB_DESC_DUMP` | single-buffered | product + every USB device's configuration descriptor printed as it mounts (`[usbdesc]`: interfaces, endpoints and their sync types, UAC1/UAC2 formats and rates, raw bytes), for the USB headphone DAC spike (PIZERO-255). Also every HID report descriptor at mount and each HID report from a non-pad, non-boot-keyboard interface when it changes, with what the volume-key decoder made of it (`[usbhid]`). Also raises TinyUSB's enumeration buffer to 512 bytes (`CFG_TUH_ENUMERATION_BUFSIZE`, guarded by `scripts/patch_tinyusb.py`): a device whose configuration descriptor is longer than the buffer is dropped without a word, and a USB audio device's often is (423 bytes for the hub in `docs/measurements/usb-audio-hub-001F-0B21.txt`) | ~85% |
| `pizero_usbtone` | `pizero_stream_60` | `-DUSB_AUDIO -DUSB_AUDIO_TONE` | single-buffered | product + the USB audio driver (`src/usb_audio.cpp`) streaming a 440 Hz test tone to the first USB Audio Class DAC found at boot, through the vendored Pico-PIO-USB's isochronous OUT stream; `[uac]` lines report the setup and packets a second (PIZERO-255) | ~86% |
| `pizero_cpuprof` | `pizero_stream_60` | `-DCPU_PROF=1` | single-buffered | product + the M33 cycle counter timing the 6809's bus path, with cycle, dummy-cycle and instruction counts: a `[prof]` line a second splits host clocks per emulated cycle into bus and core (PIZERO-209). The bus helpers are forced inline so the layout matches the product | ~77% |
| `pizero_cpuprof_hook` | `pizero_stream_60` | `-DCPU_PROF=2` | single-buffered | `pizero_cpuprof` plus the hook split into its sections (decode, data, events and audio, irq and budget) | ~77% |
| `pizero_coco3_prof` | `pizero_coco3` | `-DCPU_PROF=1` | single-buffered | `pizero_coco3` with the profiler, for the CoCo 3 speed work (PIZERO-220): `[prof3]` lines a second give, per frame, the cycles through the GIME's slow path and their clocks, event clocks, lines handed to the sink and its clocks, the run's clocks, audio samples and the catch-up; the port renderer's gather-and-hash and decode clocks per drawn line; and the worst frame's breakdown. Its `[prof]` bus/core split is not meaningful here (the nested timings land in the bus window) | ~78% |
| `pizero_wdtest` | `pizero_stream` | `-DWATCHDOG_SELFTEST` | single-buffered | wedges core 0 to prove watchdog recovery (PIZERO-33); each freeze moves to the next kind (spin in `loop()`, spin with interrupts off, spin inside a timer interrupt, hard fault) so one flash shows every post-mortem (PIZERO-227) | ~81% |
| `pizero_wavmeas` | `pizero_base` | `-DAUDIO_WAV_DUMP -DFB_SINGLE` | single-buffered | no data-island audio: dumps the emulator's sound as WAV over USB (PIZERO-41) | ~73% |
| `pizero_chord` | `pizero_stream_60` | `+ -DAV_AUDIO_SYNTH -DAV_AUDIO_SYNTH_CHORD` | single-buffered | the product, but the HDMI sound is a C major chord made on core 1, repeating 4 s of sines, 4 s of squares, 1 s of silence, to tell the HDMI link from the TV by ear (PIZERO-223) | ~81% |
| `pizero_vic1` | `pizero_stream_60` | `+ -DAV_AVI_VIC=1` | single-buffered | the product, but the AVI InfoFrame says CEA 640x480p60 (VIC 1) instead of unknown (VIC 0), for a monitor that buzzes on our audio (PIZERO-234) | ~83% |
| `pizero_aif` | `pizero_vic1` | `+ -DAV_AIF_REFER_STREAM -DAV_VBLANK_INFO_EVERY=3` | single-buffered | `pizero_vic1`, plus the Audio InfoFrame's rate and size as 'refer to stream' and the info/ACR island on every 3rd vblank line (~840 ACR/s) (PIZERO-234) | ~83% |
| `pizero_drive8` | `pizero_aif` | `+ -DDVI_PAD_DRIVE=2 -DDVI_PAD_SLEWFAST=1` | single-buffered | `pizero_aif` with the TMDS pads at 8 mA and fast slew instead of 2 mA slew-limited (PIZERO-234) | ~83% |
| `pizero_slew2` | `pizero_aif` | `+ -DDVI_PAD_SLEWFAST=1` | single-buffered | `pizero_aif` with fast slew at the default 2 mA (PIZERO-234) | ~83% |
| `pizero_wavmeas_boot` | `pizero_wavmeas` | `+ -DAUDIO_WAV_DUMP_BOOT` | single-buffered | `pizero_wavmeas`, but boots as usual (the card's autorun runs, so a program's own music plays) and records 20 s when Ctrl-W (0x17) arrives on USB serial; no picture or HDMI sound (PIZERO-223) | ~77% |
| `native` | (none) | `platform = native`, Unity | n/a | host unit tests, no board (PIZERO-109): `pio test -e native`, 28 suites, 337 cases | n/a |

PIZERO-150 retired `pizero` and `pizero_60hz` (silent, double-buffered, ~99%
RAM, no use as a fallback for a machine with sound) and the finished
`pizero_audio` (legacy bank audio) and `pizero_bench` (PIZERO-36). Their flags
still exist in the source; an env is only a preset, so any can be recreated.

**`pizero_stream_60` is the committed `default_envs` (PIZERO-45)**: true
640×480p60 + streaming data-island audio + USB, HW-confirmed. `pizero_stream` (off-spec
24 MHz/~52 Hz audio) is kept as the fallback until 60 Hz is validated across more
displays (PIZERO-99). See [`video-audio-notes.md`](video-audio-notes.md)
for the audio engineering notes. The remaining envs are diagnostics;
see the comments by each `[env:…]` in `platformio.ini`.

> **Audio engineering knowledge (the wins, gotchas, and tricks) lives in
> [`video-audio-notes.md`](video-audio-notes.md).** Read it before touching the
> audio path (especially the `__not_in_flash_func`/`PICO_NO_HARDWARE` trap and the
> "verify RAM placement with `nm`" rule).

### Single-buffered everywhere
`AV_DATA_ISLAND` switches the 320×240 framebuffer from double-buffered
(2 × ~153 KB) to single-buffered, freeing ~150 KB for the per-line audio-island
buffers (see `g_fb` in `src/main.cpp`, under `#ifdef FB_SINGLE`, which
`AV_DATA_ISLAND` turns on). The trade-off is possible tearing (core 1 may scan
the framebuffer mid-blit), accepted per `docs/audio-decision.md`. Every shipping
env defines `AV_DATA_ISLAND`, so every shipping build is single-buffered.

`pizero_wavmeas` drops data-island audio to dump the sound over USB instead, and
was the last double-buffered env, at ~99% RAM. PIZERO-131 overflowed it by 2 KB
(17 core helpers moved to RAM), and since it shows no picture, it now sets
`FB_SINGLE` directly (~73%). Without `FB_SINGLE` or `AV_DATA_ISLAND` a build is
still double-buffered, with an 8 KB cartridge limit and a 64-entry disk catalog;
no env uses that now. Every env sits at 73-77% RAM, so there is no near-full
canary build any more: check the link report of the env you changed.

---

## 3. Build-flag matrix

All flags are plain `-D` macros consumed in `src/main.cpp`. The **master switch is
`AV_DATA_ISLAND`**; most others only do anything when it is also defined.

### Core
| Flag | Effect | Set by |
|------|--------|--------|
| `AV_DATA_ISLAND` | Master enable for the data-island path: AVI + Audio InfoFrame + ACR in vblank, live audio sample packets on active lines; single-buffers the framebuffer. **Off = silent.** | `pizero_stream` |
| `GIME_PALETTE` | CoCo 3-style palette registers at `$FFB0-$FFBF` (PIZERO-85; kept by the PIZERO-111 decision). Transparent until a guest writes them; the `gime_palette` setting turns it off at run time. | `pizero_stream_60` |
| `GIME_TIMER` | CoCo 3-style timer at `$FF90-$FF95` (PIZERO-62). Stays stopped until a guest programs it; the `gime_timer` setting turns it off at run time. | `pizero_stream_60` |

### Audio test / diagnostic (layer on top of `AV_DATA_ISLAND`)
| Flag | Effect |
|------|--------|
| `AV_AUDIO_SYNTH` | No build uses it since PIZERO-42 retired `pizero_stream_synth`; the code stays until the post-show cleanup. Replace the CoCo audio source with a mathematically clean **440 Hz sine** fed straight into the islands. A pure sine has ~no harmonics, so any roughness heard is the **transport**, not the emulator/resampler. Primary signal for the PIZERO-35 warble work. |
| `AV_AUDIO_SWAPTEST` | M0 diagnostic: per-line `read_addr` ping-pong of the vblank island buffers (bypasses the live-audio path). |
| `AV_AUDIO_STATIC` | Static test tone planted in the vblank islands (M4 step). |
| `AV_ENCODE_BENCH` | **(PIZERO-36)** One-shot micro-benchmark at boot: times one RAM-resident per-line audio-island encode and prints `[bench] … ns/encode` + the IRQ-window budget. Go/no-go for in-IRQ encoding. No env since PIZERO-150 (was `pizero_bench`); add one to rerun it. |
| `AV_STREAM_AUDIO` | **(PIZERO-38, CURRENT)** Streaming per-active-line delivery: one metered island/line in the core-1 IRQ (rotating pool + exact sample meter, in 480ths of a sample since PIZERO-223). **This is the warble fix**, and every audio env uses it; the product is `pizero_stream_60` (it inherits the flag from `pizero_stream`). Unset = the legacy bursty 77-line bank path. |
| `AV_EVEN_AUDIO` | **Abandoned (PIZERO-34).** Even delivery via vblank back-porch islands. It **breaks video sync on the dev sink**. Kept off behind the flag; do not enable. |
| `AV_VBLANK_AUDIO` | **(PIZERO-232, product)** With `AV_STREAM_AUDIO`: the meter runs over all 523 non-sync lines, and each vblank line carries one island in its active block: audio, or on every `AV_VBLANK_INFO_EVERY`-th line (default 8) the AVI/Audio InfoFrame/ACR island. Costs 11.5 KB (three 640-pixel slots). In `pizero_stream_60`, so every env built on it. |
| `AV_HDMI_STRICT_FRAMING` | **(PIZERO-232, product)** A 2px trailing guard band after every vblank island. In `pizero_stream_60`, so every env built on it. |
| `AV_HDMI_WIDE_BP` | **(PIZERO-233, product)** 640x480p60 horizontal blanking 8/92/60 instead of 8/96/56, so the control period before video is 14px (HDMI minimum 12; it is 10). The 4px come from the sync pulse: taking them from the front porch (4/96/60) lost the MS2109 capture stick. In `pizero_stream_60`. |
| `AV_AUDIO_FULL_PACKETS` | **(PIZERO-232, product)** With `AV_STREAM_AUDIO`: the meter sends only full 4-sample audio packets, one every 2.4-2.6 lines, instead of 1-2 samples on nearly every line. In `pizero_stream_60`, so every env built on it. |
| `AV_AVI_VIC=<n>` | **(PIZERO-234, test)** The Video Identification Code in the AVI InfoFrame. Default 0 (unknown); 1 is CEA 640x480p60. Env `pizero_vic1`. |
| `AV_AIF_REFER_STREAM` | **(PIZERO-234, test)** The Audio InfoFrame's sample frequency and size are 0, 'refer to stream header', as CEA-861 asks for L-PCM; the channel status carries 48 kHz and 16 bits. Env `pizero_aif`. |
| `DVI_PAD_DRIVE=<0-3>`, `DVI_PAD_SLEWFAST=<0/1>` | **(PIZERO-234, test)** TMDS pad drive (2/4/8/12 mA) and fast slew, in `lib/libdvi/dvi_serialiser.cpp`. Default 0/0, libdvi's 2 mA slew-limited. Env `pizero_drive8`. |
| `AV_ACR_CTS=<n>` | ACR CTS value (default 25176). **Monitor-dependent** (PIZERO-32); inert on sinks that ignore ACR (like the dev monitor). See video-audio-notes.md. |
| `AUDIO_WAV_DUMP_BOOT` | With `AUDIO_WAV_DUMP`: keep the normal boot and autorun, and start the recording on Ctrl-W from USB serial instead of at attach (20 s instead of 9). PIZERO-223. |
| `AUDIO_WAV_DUMP` | Stream the source ring as base64 WAV over USB-CDC (source measurement, no video link). Diagnostic for PIZERO-41. The old `AUDIO_OUTPUT_LPF` flag is gone: the output low-pass is the `audio_lowpass` setting since PIZERO-212. |

### Display & stability
| Flag | Effect |
|------|--------|
| `AV_60HZ` | **(PIZERO-45)** The 60 Hz *product* timing: 252 MHz sysclk → 25.2 MHz pixel, 800×525 = **60.0 Hz**, line split 8/96/56/640 so the 56px (28-word) back porch fits one streaming audio island (8/92/60/640 with `AV_HDMI_WIDE_BP`, which the product sets since PIZERO-233). Implies vreg 1.25, ACR CTS=25200, 800 audio samples/frame, 60 fps pacing. Composes with `AV_STREAM_AUDIO` + USB (`pizero_stream_60`). |
| `AV_60HZ_TEST` | **(PIZERO-44)** 252 MHz sysclk → 25.2 MHz pixel, standard 800×525 = **60 Hz**, USB enabled, no audio. Proved 60 Hz + USB coexist. The 60 Hz *product* build (audio re-fitted) is `AV_60HZ` above. |
| `AV_STD_TIMING` | No build uses it since PIZERO-42 retired `pizero_stream_std`; the code stays until the post-show cleanup. Diagnostic: 252 MHz / 25.2 MHz pixel but **keeps ~52 Hz** (widened h_fp) and **disables USB**. Used to prove the residual pitch/buzz is *not* the pixel clock (PIZERO-41). |
| `ARTIFACT_PHASE_LEGACY` | Revert the PMODE4/RG6 NTSC artifact red/blue phase to the pre-PIZERO-43 orientation (default now matches Space Warp). |
| `WATCHDOG_DISABLE` | Turn off the PIZERO-33 hardware watchdog (default ON: auto-reboots a wedged board in ~3 s + logs the stuck phase in `[run]` as `freezes=N last=<phase>`). Disable only for live freeze debugging. `WATCHDOG_TIMEOUT_MS` overrides the 3000 ms timeout. After a freeze the next boot also prints a `[watchdog] post-mortem:` line (PIZERO-227, `src/wd_post.cpp`): where core 0 really was (PC, LR, and which interrupt it was inside, from a top-priority early warning that saves once the watchdog has under 1 s left), whether core 1 kept running, core 0's active and pending interrupts, the 6809 PC, USB device count and uptime; or a hard fault's frame and fault registers; or that the early warning never ran (interrupts masked or a stalled bus). Resolve a PC with `arm-none-eabi-addr2line -fe .pio/build/<env>/firmware.elf <pc>` against the same build. |
| `CPU_SPEED_MUL=n` | **(PIZERO-130)** Run n times the 6809 cycles per frame (default 1). `2` is the CPU load of a CoCo 3 at 1.79 MHz (`pizero_cpu179`); the whole machine runs n times fast. |
| `VIDEO_640` | **(PIZERO-134)** The CoCo 3 at full horizontal resolution: the framebuffer is 640x240 one-byte GIME colours (153,600 bytes, the RGB565 buffer's size), and core 1 scans it out through a 256-entry RGB565 palette on the SIO hardware TMDS encoder (`dvi_static_framebuf_main_pal8`). Boot and overlay cards draw two bytes a pixel; screenshots save 640x480. Needs `COCO3`, a single framebuffer and `TCC1014_PORT_FULL`. In `pizero_coco3`. |
| `TCC1014_PORT_FULL` | **(PIZERO-134)** The GIME port renderer emits all 640 pixels a line instead of halving them (`TCC1014_PORT_W` 640). Goes with `VIDEO_640`. |
| `FILES_SCAN_FOLDERS=1` | **(PIZERO-308)** The F11 FILES list also scans `/coco`'s folders one level down for text files (`src/coco_boot.cpp`). Off by default since 2026-11-01: opening every file in the log and game folders made F11 slow to come up. No env sets it. |
| `CPU_PROF=1\|2` | **(PIZERO-209)** Profile where the time per emulated 6809 cycle goes with the DWT cycle counter (`lib/xroar_core/include/cpu_prof.h`); `2` also splits the hook. Empty in every other build. |
| `USB_HOTPLUG_RECOVER` | No build uses it since PIZERO-42 retired `pizero_hotplug`; the code stays until the post-show cleanup. **(PIZERO-51) EXPERIMENTAL, HW-DISPROVEN, do not ship.** USB hot-replug recovery. On this rev3 board an unplug is invisible to the line/connect flags: the PIO SM pins the bus at J/FS and PIO-USB floods ~180 byte-identical phantom HID reports/s (PIZERO-11b). Detects a run of `USB_PHANTOM_FLOOD_N` (default 100, ~0.55 s) identical reports on an interface, then **watchdog-reboots** to re-enumerate (fix v3; the earlier `pio_usb_host_stop/restart` and force-disconnect approaches are dead; see the ticket). **2026-08-16 HW result: does not work.** The reboot does not re-enumerate an attached device (`usb=0` for 200 s), the detector false-positives on an idle composite keyboard (~20 s after mount), and the `scratch[4]` replug sentinel was silently wiped by the pico-SDK (`watchdog_reboot` zeroes it), so recoveries were miscounted as PIZERO-33 freezes (fixed in PIZERO-135: the tag is now written after the call). Kept flag-gated in `pizero_hotplug` for re-test only; the real fix needs a GPIO-switched VBUS. |

### Source validation (no video link required)
| Flag | Effect |
|------|--------|
| `FB_SINGLE` | One framebuffer instead of two. `AV_DATA_ISLAND` turns it on; set alone it single-buffers a build without data islands (`pizero_wavmeas`, PIZERO-131), which also gets the 16 KB cartridge limit and the 128-entry disk catalog. |
| `AUDIO_WAV_DUMP` | Stream the CoCo audio ring as a **base64 WAV over USB-CDC** (markers `---WAV-BEGIN---` / `---WAV-END---`) with an autotyped tone program. Validates the audio **source** independent of data-island delivery. Skips the SD autorun boot. Works without `AV_DATA_ISLAND`. |
| `AUDIO_DUMP_SECONDS` | Length (s) of the WAV-dump capture window. |
| `AUDIO_SF_*`, `AUDIO_SS_*` | Audio sample-frequency / sample-size code overrides (advanced; default 48 kHz / 16-bit). |

---

## 4. Toolchain gotchas (these have cost real time)

### a) `__not_in_flash_func` silently no-ops in C files here
The earlephilhower arduino-pico core defines **`PICO_NO_HARDWARE` (as `0`)**.
Because `#if defined(PICO_NO_HARDWARE)` is true regardless of value, the pico-SDK
RAM-placement macros (`__not_in_flash_func`, `__not_in_flash`) expand to **no-ops**
in a plain C translation unit that includes `pico/platform.h`. The function stays
in **flash**, defeating the whole point of moving a hot path out of XIP.

**Fix we use:** place RAM-resident code/data with an **explicit section attribute**,
not the SDK macro. See `lib/libdvi/dvi_data_island.c`:
```c
#define DVI_DI_RAMFUNC __attribute__((section(".time_critical.dvi_di")))
#define DVI_DI_RAMDATA __attribute__((section(".time_critical.dvi_di_rodata")))
void DVI_DI_RAMFUNC dvi_di_encode_header(...) { ... }
```
The linker maps `.time_critical*` into RAM. **Verify** placement (don't assume):
```bash
NM=~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm
$NM .pio/build/pizero_stream_60/firmware.elf | grep dvi_di_encode_header
# RAM symbol  -> 2000xxxx   |   flash symbol -> 1001xxxx  (a *_veneer in flash is fine)
```
(The C++ libdvi files get the real macro via other includes and *do* land in RAM,
which is why only the `.c` encoder needed this treatment. Don't be misled.)

### b) `PLATFORMIO_BUILD_FLAGS` does not reliably trigger rebuilds
Passing `PLATFORMIO_BUILD_FLAGS="-DFOO" pio run` here does **not** reliably
invalidate cached object files. SCons happily links **stale** objects, so you
flash a build that doesn't contain your flag (silent and very confusing). This is
exactly why we use real envs (§2) instead.

If you ever must force a rebuild of one file:
```bash
find .pio/build/<env> -name '<file>.o' -delete   # then re-run pio run -e <env>
```
A flag change in `platformio.ini` *does* invalidate correctly; the env var does not.

### c) clangd "file not found" noise
`compile_commands.json` is gitignored. Regenerate after adding/swapping libraries
so the IDE resolves `Arduino.h`, libdvi, Pico-PIO-USB, etc.:
```bash
pio run -t compiledb
```
Until then, expect bogus `'string.h' file not found` / implicit-`memset` diagnostics
in editor; they are **not** real build errors (the actual `pio run` is clean).

---

## 5. Verifying a build

- **Size:** the link report prints `RAM: … %`. Measured 2026-09-27: the audio
  envs (`pizero_stream_60`, `pizero_stream` and the diagnostics built on them)
  **~75%**. Since PIZERO-131 every env is single-buffered and between 73% and 77%;
  check the report of each env a change touches.
- **Seeing the boot (PIZERO-316):** put `debug = on` in `/coco/settings.txt`.
  The board then waits up to 4 s at boot for a monitor to open the serial
  port, so the boot's own lines (the reset reason, the watchdog post-mortem,
  `[mem] free heap N bytes with the CoCo 3's 128 KB`) reach it. With
  `debug = off`, the default, the boot does not wait and those lines are lost
  unless a monitor already has the port open; the `[boot] times ms:` line is
  printed again when one opens it, and Ctrl-T's `[machine] now a CoCo 3 ...
  free heap N bytes` gives the heap after Disk BASIC without a reboot.
- **Serial telemetry:** `pio device monitor` (115200). The running emulator prints
  `[run] fps cpu render blit aud …`. Confirm `fps≈60` on `pizero_stream_60`
  (`≈52` on the `pizero_stream` fallback), frame time under budget.
- **Switching machine (PIZERO-314):** Ctrl-T sent on USB serial switches the
  running board to the other machine (CoCo 2 or CoCo 3) at its BASIC prompt,
  in place, with `settings.txt` left as it is. Each switch prints
  `[mem] free heap N bytes with no machine` and `[machine] now a CoCo 3, built
  in place in N ms; free heap N bytes`; the numbers must come back the same on
  every round trip, or the switch is leaking. Since PIZERO-319 the machine keeps
  one 64 KB block for good, so the heap reads 12,192 with no machine, 10,144 as
  a CoCo 2 (its spare 40 KB is kept for a bank-switched cart) and 8,248 as a
  CoCo 3 (2026-11-01). The boot also prints `[stack] core 0 deepest N of 4096
  bytes, core 1 N of 2048`, again whenever it grows and when a monitor opens
  the port: core 0 past 3,584 says `*** NEAR THE END ***`, since core 1's
  stack sits right below core 0's.
- **RAM placement** of hot functions: see the `nm` snippet in §4a.
- **6809 correctness (PIZERO-129):** `pio test -e native -f test_mc6809_lockstep`
  runs the frozen reference core (`test/mc6809_reference/`) and the live core
  side by side and fails at the first bus cycle or register that differs. Run
  it after any change under `lib/xroar_core/src/mc6809` or `mc680x`. With
  `COCO_ROM_DIR=<folder with bas12.rom and extbas11.rom>` it also boots Color
  BASIC for three million instructions (skipped otherwise; the repo ships no
  ROMs). The whole native suite runs in about ten seconds.
- **CoCo 3 boot (PIZERO-133):** `pio test -e native -f test_coco3_boot` boots
  a 128 KB CoCo 3 on the host: the live 6809 core and PIAs, the vendored GIME
  (`lib/xroar_core/src/tcc1014`) and `lib/coco_machine/src/coco3_bus.h`. It
  needs `coco3.rom` (32 KB) in `COCO_ROM_DIR` and is skipped without it.
  `COCO3_DUMP=<file>` also writes the last frame as raw RGB565 (320x240) to
  look at. PIZERO-213: it boots on the firmware's MMU fast path, and two more
  cases run the same machine with and without it and compare time, registers
  and all 128 KB: the boot at fields 1-180, and a program that switches to
  1.79 MHz and keeps remapping an MMU page while it fills it.
- **Soaking without a laptop (PIZERO-201):** put `soak_log = on` in
  `/coco/settings.txt` (F11 edits it on the board; it takes effect at the next
  power-on). Each boot then writes `/coco/log/soak-NNN.txt`: a header with the
  firmware, env, whether the boot was power-on or a watchdog reboot and the
  carried freeze tally, the `[watchdog] post-mortem:` line after a freeze
  (PIZERO-227), then the `[run]` and `[aud]` lines once a minute,
  timestamped, synced to the card each time. Power the board from a USB-C
  supply, keyboard and pad through the simple hub, monitor attached; pull the
  card afterwards and run `scripts/soak.py analyse /Volumes/<card>/coco/log/soak-001.txt`.
  Every boot starts the next file, and its header says whether that boot
  was a power-on or a watchdog recovery (with the carried freeze tally), so
  a run is all its files joined in order; the clock carries across a
  watchdog reboot and is re-seeded from the newest log at a power-on, so
  the timestamps stay in sequence. The clock is the RP2350's always-on timer, seeded
  at power-on from the newest file time on the card (the Mac wrote those with
  real time), so dates are right to within when the card was last touched;
  the INFO page (F12, then Left) shows it and where it came from. The `badfps` field on the `[run]`
  line counts every second off 60 fps since boot, so the minute sampling
  loses no video-fault windows. `slack` (PIZERO-219) is the least time any
  frame that second had left before its slot, and `late` how many frames
  missed it: a late frame loses emulated time, so the sound comes up short
  while `fps` can still read 60. `cmax` and `wmax` (PIZERO-220) are that
  second's worst emulation time and worst whole frame of work (from the end
  of the last frame's wait to the pacing), so a late frame can be put down
  to the emulation or to the rest of the loop. The logger's own writes (about 8 ms a minute)
  are part of what the soak tests.
- **Hardware acceptance** (audio/video) is listen/look-on-the-monitor and must be
  user-confirmed. Do not mark an issue `done` from a green build alone (see
  `CLAUDE.md` workflow rules).

---

## 6. See also
- `CLAUDE.md`: repo conventions, source-repo pointers, workflow rules.
- `docs/audio-decision.md`: why audio over the video link (vs PWM), the single-buffer trade-off.
- `docs/kit.md`: what goes in a built unit, BOM, and assembly (see `PIZERO-91`).
- `issues.jsonl`: work tracking (`PIZERO-NN`); the data-island audio rework is PIZERO-35 → 36–40.

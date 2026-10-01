# BUILD.md — how to build, flash, and configure this firmware

XRoar (Tandy CoCo) on the **Waveshare RP2350-PiZero**. This is the single source
of truth for **how we build it** — the PlatformIO envs, the HDMI/audio build-flag
matrix, flashing + serial, and the toolchain gotchas that have bitten past
sessions. **If you add or change a build flag, update this file.**

> Hardware specs, pinout, and the libdvi/HDMI design rationale live in
> `README.md` and `docs/`. This file is only about the build.

---

## 1. Quick start

```bash
# PRODUCT build — true 640x480p60 + streaming HDMI audio + USB. This is the
# committed default env (PIZERO-45), so a bare `pio run` builds it:
pio run                                 # == pio run -e pizero_stream_60
pio run -t upload                       # flash (hold BOOT if upload fails)
pio device monitor                      # serial @ 115200, prints [run] telemetry

# Fallback for a display that rejects 60 Hz (kept until PIZERO-99 proves 60 Hz
# widely): the older off-spec 24 MHz / ~52 Hz timing. It does NOT build the
# GIME palette or timer (-DGIME_PALETTE / -DGIME_TIMER), so the gime_palette
# and gime_timer settings do nothing on it.
pio run -e pizero_stream -t upload

# Host unit tests (no board): 16 suites, 222 cases.
pio test -e native
```

Upload puts the RP2350 into BOOTSEL automatically via `picotool`; if it won't,
hold the **BOOT** button while plugging USB-C, then re-run upload.

---

## 2. Build environments (`platformio.ini`)

We define **one env per configuration**. Do **not** toggle features with one-off
`-D` flags passed through the `PLATFORMIO_BUILD_FLAGS` env var — see the cache
trap in §4. Add a new env that extends `pizero_base` (or `env:pizero_stream`)
instead. `pizero_base` holds the settings every firmware env shares; it is not
buildable on its own.

| Env | Extends | Adds | Framebuffer | Audio | RAM |
|-----|---------|------|-------------|-------|-----|
| **`pizero_stream_60`** | `pizero_stream` | `-DAV_60HZ -DGIME_TIMER -DGIME_PALETTE` | single-buffered | **THE PRODUCT: streaming audio @ true 640x480p60 + USB (PIZERO-45)** | ~75% |
| **`pizero_stream`** | `pizero_base` | `-DAV_DATA_ISLAND -DAV_STREAM_AUDIO` | single-buffered | streaming audio at the older ~52 Hz timing: the fallback. No GIME palette or timer | ~75% |
| `pizero_hotplug` | `pizero_stream_60` | `-DUSB_HOTPLUG_RECOVER` | single-buffered | product + USB hot-replug recovery (PIZERO-51). **HW-disproven**: kept for re-test only, do not ship | ~75% |
| `pizero_usbdebug` | `pizero_stream_60` | `-DCFG_TUSB_DEBUG=2 -DCFG_TUD_LOG_LEVEL=3 -DCFG_TUSB_DEBUG_PRINTF=tusb_debug_printf -DSERIAL_TUSB_DEBUG=Serial` | single-buffered | product + TinyUSB host log on the serial console, for hub and enumeration faults (PIZERO-54) | ~75% |
| `pizero_padprobe` | `pizero_stream_60` | `-DPAD_PROBE=1` | single-buffered | product + gamepad report dump on serial, for measuring a pad's buttons (PIZERO-13, 164) | ~75% |
| `pizero_stream_synth` | `pizero_stream` | `-DAV_AUDIO_SYNTH` | single-buffered | 440 Hz test tone: tests the HDMI sound path alone (PIZERO-99) | ~75% |
| `pizero_stream_std` | `pizero_stream` | `-DAV_STD_TIMING` | single-buffered | standard blanking timing, no USB host (PIZERO-99/120) | ~75% |
| `pizero_stream_lpf` | `pizero_stream` | `-DAUDIO_OUTPUT_LPF` | single-buffered | TV-bandwidth output filter on (PIZERO-41) | ~75% |
| `pizero_wdtest` | `pizero_stream` | `-DWATCHDOG_SELFTEST` | single-buffered | wedges core 0 to prove watchdog recovery (PIZERO-33) | ~75% |
| `pizero_wavmeas` | `pizero_base` | `-DAUDIO_WAV_DUMP` | double-buffered | no HDMI audio: dumps the emulator's sound as WAV over USB (PIZERO-41) | ~99% |
| `waveshare_demo` | (none) | (stock USB demo) | n/a | Waveshare's USB device_info demo, for USB triage (PIZERO-11/51) | ~6% |
| `native` | (none) | `platform = native`, Unity | n/a | host unit tests, no board (PIZERO-109): `pio test -e native`, 16 suites, 222 cases | n/a |

PIZERO-150 retired `pizero` and `pizero_60hz` (silent, double-buffered, ~99%
RAM, no use as a fallback for a machine with sound) and the finished
`pizero_audio` (legacy bank audio) and `pizero_bench` (PIZERO-36). Their flags
still exist in the source; an env is only a preset, so any can be recreated.

**`pizero_stream_60` is the committed `default_envs` (PIZERO-45)**: true
640×480p60 + streaming HDMI audio + USB, HW-confirmed. `pizero_stream` (off-spec
24 MHz/~52 Hz audio) is kept as the fallback until 60 Hz is validated across more
displays (PIZERO-99). See [`hdmi-audio-notes.md`](hdmi-audio-notes.md)
for the audio engineering notes. The remaining envs are diagnostics;
see the comments by each `[env:…]` in `platformio.ini`.

> **Audio engineering knowledge — the wins, gotchas, and tricks — lives in
> [`hdmi-audio-notes.md`](hdmi-audio-notes.md).** Read it before touching the
> audio path (especially the `__not_in_flash_func`/`PICO_NO_HARDWARE` trap and the
> "verify RAM placement with `nm`" rule).

### Why the audio builds use *less* RAM than `pizero_wavmeas`
`AV_DATA_ISLAND` switches the 320×240 framebuffer from double-buffered
(2 × ~153 KB) to single-buffered, freeing ~150 KB for the per-line audio-island
buffers (see `g_fb` in `src/main.cpp`, under `#ifdef AV_DATA_ISLAND`). The
trade-off is possible tearing (core 1 may scan the framebuffer mid-blit),
accepted per `docs/audio-decision.md`. Every shipping env defines
`AV_DATA_ISLAND`, so every shipping build is single-buffered. The only
double-buffered env left is the diagnostic `pizero_wavmeas`, which drops HDMI
audio to dump the sound over USB instead. Measured 2026-09-27: the audio envs
sit at **~75%**, while `pizero_wavmeas` is nearly full at **~99%** (~5 KB
spare). So a new static buffer that fits the product can still overflow
`pizero_wavmeas`: build both when adding RAM, and watch the link report.

---

## 3. Build-flag matrix

All flags are plain `-D` macros consumed in `src/main.cpp`. The **master switch is
`AV_DATA_ISLAND`**; most others only do anything when it is also defined.

### Core
| Flag | Effect | Set by |
|------|--------|--------|
| `AV_DATA_ISLAND` | Master enable: HDMI data-island path: AVI + Audio InfoFrame + ACR in vblank, live audio sample packets on active lines; single-buffers the framebuffer. **Off = silent.** | `pizero_stream` |
| `GIME_PALETTE` | CoCo 3-style palette registers at `$FFB0-$FFBF` (PIZERO-85; kept by the PIZERO-111 decision). Transparent until a guest writes them; the `gime_palette` setting turns it off at run time. | `pizero_stream_60` |
| `GIME_TIMER` | CoCo 3-style timer at `$FF90-$FF95` (PIZERO-62). Stays stopped until a guest programs it; the `gime_timer` setting turns it off at run time. | `pizero_stream_60` |

### Audio test / diagnostic (layer on top of `AV_DATA_ISLAND`)
| Flag | Effect |
|------|--------|
| `AV_AUDIO_SYNTH` | Replace the CoCo audio source with a mathematically clean **440 Hz sine** fed straight into the islands. A pure sine has ~no harmonics, so any roughness heard is the **transport**, not the emulator/resampler. Primary signal for the PIZERO-35 warble work. |
| `AV_AUDIO_SWAPTEST` | M0 diagnostic: per-line `read_addr` ping-pong of the vblank island buffers (bypasses the live-audio path). |
| `AV_AUDIO_STATIC` | Static test tone planted in the vblank islands (M4 step). |
| `AV_ENCODE_BENCH` | **(PIZERO-36)** One-shot micro-benchmark at boot: times one RAM-resident per-line audio-island encode and prints `[bench] … ns/encode` + the IRQ-window budget. Go/no-go for in-IRQ encoding. No env since PIZERO-150 (was `pizero_bench`); add one to rerun it. |
| `AV_STREAM_AUDIO` | **(PIZERO-38, CURRENT)** Streaming per-active-line delivery: one metered island/line in the core-1 IRQ (rotating pool + 16.16 sample meter). **This is the warble fix**, and every audio env uses it; the product is `pizero_stream_60` (it inherits the flag from `pizero_stream`). Unset = the legacy bursty 77-line bank path. |
| `AV_EVEN_AUDIO` | **Abandoned (PIZERO-34).** Even delivery via vblank back-porch islands — **breaks video sync on the dev sink**. Kept off behind the flag; do not enable. |
| `AV_ACR_CTS=<n>` | ACR CTS value (default 25176). **Monitor-dependent** (PIZERO-32); inert on sinks that ignore ACR (like the dev monitor). See hdmi-audio-notes.md. |
| `AUDIO_WAV_DUMP` / `AUDIO_OUTPUT_LPF` | Stream the source ring as base64 WAV over USB-CDC (source measurement, no HDMI) / re-enable the 2-pole TV-bandwidth output LPF. Diagnostics for PIZERO-41. |

### Display & stability
| Flag | Effect |
|------|--------|
| `AV_60HZ` | **(PIZERO-45)** The 60 Hz *product* timing: 252 MHz sysclk → 25.2 MHz pixel, 800×525 = **60.0 Hz**, line split 8/96/56/640 so the 56px (28-word) back porch fits one streaming audio island. Implies vreg 1.25, ACR CTS=25200, 800 audio samples/frame, 60 fps pacing. Composes with `AV_STREAM_AUDIO` + USB (`pizero_stream_60`). |
| `AV_60HZ_TEST` | **(PIZERO-44)** 252 MHz sysclk → 25.2 MHz pixel, standard 800×525 = **60 Hz**, USB enabled, no audio. Proved 60 Hz + USB coexist. The 60 Hz *product* build (audio re-fitted) is `AV_60HZ` above. |
| `AV_STD_TIMING` | Diagnostic: 252 MHz / 25.2 MHz pixel but **keeps ~52 Hz** (widened h_fp) and **disables USB**. Used to prove the residual pitch/buzz is *not* the pixel clock (PIZERO-41). |
| `ARTIFACT_PHASE_LEGACY` | Revert the PMODE4/RG6 NTSC artifact red/blue phase to the pre-PIZERO-43 orientation (default now matches Space Warp). |
| `WATCHDOG_DISABLE` | Turn off the PIZERO-33 hardware watchdog (default ON: auto-reboots a wedged board in ~3 s + logs the stuck phase in `[run]` as `freezes=N last=<phase>`). Disable only for live freeze debugging. `WATCHDOG_TIMEOUT_MS` overrides the 3000 ms timeout. |
| `USB_HOTPLUG_RECOVER` | **(PIZERO-51) EXPERIMENTAL — HW-DISPROVEN, do not ship.** USB hot-replug recovery. On this rev3 board an unplug is invisible to the line/connect flags — the PIO SM pins the bus at J/FS and PIO-USB floods ~180 byte-identical phantom HID reports/s (PIZERO-11b). Detects a run of `USB_PHANTOM_FLOOD_N` (default 100, ~0.55 s) identical reports on an interface, then **watchdog-reboots** to re-enumerate (fix v3; the earlier `pio_usb_host_stop/restart` and force-disconnect approaches are dead — see the ticket). **2026-08-16 HW result: does not work.** The reboot does not re-enumerate an attached device (`usb=0` for 200 s), the detector false-positives on an idle composite keyboard (~20 s after mount), and the `scratch[4]` replug sentinel is silently wiped by the pico-SDK (`watchdog_reboot` zeroes it), so recoveries are miscounted as PIZERO-33 freezes. Kept flag-gated in `pizero_hotplug` for re-test only; the real fix needs a GPIO-switched VBUS. |

### Source validation (HDMI not required)
| Flag | Effect |
|------|--------|
| `AUDIO_WAV_DUMP` | Stream the CoCo audio ring as a **base64 WAV over USB-CDC** (markers `---WAV-BEGIN---` / `---WAV-END---`) with an autotyped tone program. Validates the audio **source** independent of HDMI delivery. Skips the SD autorun boot. Works without `AV_DATA_ISLAND`. |
| `AUDIO_DUMP_SECONDS` | Length (s) of the WAV-dump capture window. |
| `AUDIO_SF_*`, `AUDIO_SS_*` | Audio sample-frequency / sample-size code overrides (advanced; default 48 kHz / 16-bit). |

---

## 4. Toolchain gotchas (these have cost real time)

### a) `__not_in_flash_func` silently no-ops in C files here
The earlephilhower arduino-pico core defines **`PICO_NO_HARDWARE` (as `0`)**.
Because `#if defined(PICO_NO_HARDWARE)` is true regardless of value, the pico-SDK
RAM-placement macros (`__not_in_flash_func`, `__not_in_flash`) expand to **no-ops**
in a plain C translation unit that includes `pico/platform.h` — the function stays
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
which is why only the `.c` encoder needed this treatment — don't be misled.)

### b) `PLATFORMIO_BUILD_FLAGS` does not reliably trigger rebuilds
Passing `PLATFORMIO_BUILD_FLAGS="-DFOO" pio run` here does **not** reliably
invalidate cached object files — SCons happily links **stale** objects, so you
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
in editor — they are **not** real build errors (the actual `pio run` is clean).

---

## 5. Verifying a build

- **Size:** the link report prints `RAM: … %`. Measured 2026-09-27: the audio
  envs (`pizero_stream_60`, `pizero_stream` and the diagnostics built on them)
  **~75%**. `pizero_wavmeas` is double-buffered at **~99%** with ~5 KB spare, so
  a new static buffer can overflow it; build it too when adding RAM. Check it.
- **Serial telemetry:** `pio device monitor` (115200). The running emulator prints
  `[run] fps cpu render blit aud …`. Confirm `fps≈60` on `pizero_stream_60`
  (`≈52` on the `pizero_stream` fallback), frame time under budget.
- **RAM placement** of hot functions: see the `nm` snippet in §4a.
- **Hardware acceptance** (audio/video) is listen/look-on-the-monitor and must be
  user-confirmed — do not mark an issue `done` from a green build alone (see
  `CLAUDE.md` workflow rules).

---

## 6. See also
- `CLAUDE.md` — repo conventions, source-repo pointers, workflow rules.
- `docs/audio-decision.md` — why HDMI audio (vs PWM), the single-buffer trade-off.
- `docs/kit.md` — what goes in a built unit, BOM, and assembly (see `PIZERO-91`).
- `issues.jsonl` — work tracking (`PIZERO-NN`); HDMI-audio rework is PIZERO-35 → 36–40.

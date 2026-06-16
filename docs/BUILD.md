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

# Fallback envs (kept until 60Hz is validated on more displays — PIZERO-42):
pio run -e pizero -t upload             # silent baseline (no audio, double-buffered)
pio run -e pizero_stream -t upload      # off-spec 24MHz/~52Hz streaming audio
pio run -e pizero_60hz -t upload        # 60Hz video-only test (no audio)

# Stage-0 encode benchmark (PIZERO-36):
pio run -e pizero_bench -t upload && pio device monitor   # prints [bench] ns/encode
```

Upload puts the RP2350 into BOOTSEL automatically via `picotool`; if it won't,
hold the **BOOT** button while plugging USB-C, then re-run upload.

---

## 2. Build environments (`platformio.ini`)

We define **one env per configuration**. Do **not** toggle features with one-off
`-D` flags passed through the `PLATFORMIO_BUILD_FLAGS` env var — see the cache
trap in §4. Add a new `extends = env:pizero` env instead.

| Env | Extends | Adds | Framebuffer | Audio | RAM |
|-----|---------|------|-------------|-------|-----|
| **`pizero`** | — | (base) | double-buffered, tear-free | **off (silent)** | ~94.4% |
| **`pizero_audio`** | `pizero` | `-DHDMI_DATA_ISLAND` | single-buffered | bank audio (legacy 77-line) | ~92.5% |
| **`pizero_stream`** | `pizero_audio` | `-DHDMI_STREAM_AUDIO` | single-buffered | **streaming audio (current; warble-fixed)** | ~70% |
| `pizero_stream_synth` | `pizero_stream` | `-DHDMI_AUDIO_SYNTH` | single-buffered | 440 Hz test tone (transport test) | ~70% |
| **`pizero_stream_60`** | `pizero_stream` | `-DHDMI_60HZ` | single-buffered | **streaming audio @ true 640x480p60 + USB (PIZERO-45)** | ~70% |
| **`pizero_60hz`** | `pizero` | `-DHDMI_60HZ_TEST` | double-buffered | none (test) | ~96.8% |
| `pizero_bench` / `pizero_wdtest` / `pizero_wavmeas` / `pizero_stream_std` / `pizero_stream_lpf` | various | (diagnostics) | — | — | — |
| `waveshare_demo` | — | (stock USB demo) | — | — | — |

**`pizero_stream_60` is the committed `default_envs` (PIZERO-45)** — true
640×480p60 + streaming HDMI audio + USB, HW-confirmed. `pizero` (silent baseline),
`pizero_stream` (off-spec 24 MHz/~52 Hz audio) and `pizero_60hz` (60 Hz video-only
test) are KEPT as fallbacks until 60 Hz is validated across more displays, then the
off-spec envs retire (PIZERO-42). See [`hdmi-audio-notes.md`](hdmi-audio-notes.md)
for the audio engineering notes. The remaining envs are diagnostics —
see the comments by each `[env:…]` in `platformio.ini`.

> **Audio engineering knowledge — the wins, gotchas, and tricks — lives in
> [`hdmi-audio-notes.md`](hdmi-audio-notes.md).** Read it before touching the
> audio path (especially the `__not_in_flash_func`/`PICO_NO_HARDWARE` trap and the
> "verify RAM placement with `nm`" rule).

### Why audio uses *less* RAM than the silent build
`HDMI_DATA_ISLAND` switches the 320×240 framebuffer from double-buffered
(2 × ~153 KB) to single-buffered, freeing ~150 KB for the per-line audio-island
buffers (`main.cpp:98`). The trade-off is possible tearing (core 1 may scan the
framebuffer mid-blit), accepted per `docs/audio-decision.md`. RAM is tight either
way (>92%); watch the link report when adding buffers.

---

## 3. Build-flag matrix

All flags are plain `-D` macros consumed in `src/main.cpp`. The **master switch is
`HDMI_DATA_ISLAND`**; most others only do anything when it is also defined.

### Core
| Flag | Effect | Set by |
|------|--------|--------|
| `HDMI_DATA_ISLAND` | Master enable: HDMI data-island path — AVI + Audio InfoFrame + ACR in vblank, live audio sample packets on active lines; single-buffers the framebuffer. **Off = silent.** | `pizero_audio` |

### Audio test / diagnostic (layer on top of `HDMI_DATA_ISLAND`)
| Flag | Effect |
|------|--------|
| `HDMI_AUDIO_SYNTH` | Replace the CoCo audio source with a mathematically clean **440 Hz sine** fed straight into the islands. A pure sine has ~no harmonics, so any roughness heard is the **transport**, not the emulator/resampler. Primary signal for the PIZERO-35 warble work. |
| `HDMI_AUDIO_SWAPTEST` | M0 diagnostic: per-line `read_addr` ping-pong of the vblank island buffers (bypasses the live-audio path). |
| `HDMI_AUDIO_STATIC` | Static test tone planted in the vblank islands (M4 step). |
| `HDMI_ENCODE_BENCH` | **(PIZERO-36)** One-shot micro-benchmark at boot: times one RAM-resident per-line audio-island encode and prints `[bench] … ns/encode` + the IRQ-window budget. Go/no-go for in-IRQ encoding. Set by `pizero_bench`. |
| `HDMI_STREAM_AUDIO` | **(PIZERO-38, CURRENT)** Streaming per-active-line delivery — one metered island/line in the core-1 IRQ (rotating pool + 16.16 sample meter). **This is the warble fix**; it's the recommended audio path (`pizero_stream`). Unset = the legacy bursty 77-line bank path. |
| `HDMI_EVEN_AUDIO` | **Abandoned (PIZERO-34).** Even delivery via vblank back-porch islands — **breaks video sync on the dev sink**. Kept off behind the flag; do not enable. |
| `HDMI_ACR_CTS=<n>` | ACR CTS value (default 25176). **Monitor-dependent** (PIZERO-32); inert on sinks that ignore ACR (like the dev monitor). See hdmi-audio-notes.md. |
| `AUDIO_WAV_DUMP` / `AUDIO_OUTPUT_LPF` | Stream the source ring as base64 WAV over USB-CDC (source measurement, no HDMI) / re-enable the 2-pole TV-bandwidth output LPF. Diagnostics for PIZERO-41. |

### Display & stability
| Flag | Effect |
|------|--------|
| `HDMI_60HZ` | **(PIZERO-45)** The 60 Hz *product* timing: 252 MHz sysclk → 25.2 MHz pixel, 800×525 = **60.0 Hz**, line split 8/96/56/640 so the 56px (28-word) back porch fits one streaming audio island. Implies vreg 1.25, ACR CTS=25200, 800 audio samples/frame, 60 fps pacing. Composes with `HDMI_STREAM_AUDIO` + USB (`pizero_stream_60`). |
| `HDMI_60HZ_TEST` | **(PIZERO-44)** 252 MHz sysclk → 25.2 MHz pixel, standard 800×525 = **60 Hz**, USB enabled, no audio. Proved 60 Hz + USB coexist. The 60 Hz *product* build (audio re-fitted) is `HDMI_60HZ` above. |
| `HDMI_STD_TIMING` | Diagnostic: 252 MHz / 25.2 MHz pixel but **keeps ~52 Hz** (widened h_fp) and **disables USB**. Used to prove the residual pitch/buzz is *not* the pixel clock (PIZERO-41). |
| `ARTIFACT_PHASE_LEGACY` | Revert the PMODE4/RG6 NTSC artifact red/blue phase to the pre-PIZERO-43 orientation (default now matches Space Warp). |
| `WATCHDOG_DISABLE` | Turn off the PIZERO-33 hardware watchdog (default ON: auto-reboots a wedged board in ~3 s + logs the stuck phase in `[run]` as `freezes=N last=<phase>`). Disable only for live freeze debugging. `WATCHDOG_TIMEOUT_MS` overrides the 3000 ms timeout. |

### Timing
| Flag | Effect |
|------|--------|
| `HDMI_STD_TIMING` | Use ~standard **25.2 MHz pixel / 252 MHz sysclk** timing (h_fp=58, h_total=924) instead of our off-spec 24 MHz / 240 MHz. **Skips the USB host** (PIO-USB needs CPU = 120/240 MHz). Diagnostic for the ACR sink-clock pitch question (PIZERO-32). |

### Source validation (HDMI not required)
| Flag | Effect |
|------|--------|
| `AUDIO_WAV_DUMP` | Stream the CoCo audio ring as a **base64 WAV over USB-CDC** (markers `---WAV-BEGIN---` / `---WAV-END---`) with an autotyped tone program. Validates the audio **source** independent of HDMI delivery. Skips the SD autorun boot. Works without `HDMI_DATA_ISLAND`. |
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
$NM .pio/build/pizero_audio/firmware.elf | grep dvi_di_encode_header
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

- **Size:** the link report prints `RAM: … %`. Both audio envs sit >92% — adding
  buffers can overflow; check it.
- **Serial telemetry:** `pio device monitor` (115200). The running emulator prints
  `[run] fps cpu render blit aud …` — confirm `fps≈52`, frame time under budget.
- **RAM placement** of hot functions: see the `nm` snippet in §4a.
- **Hardware acceptance** (audio/video) is listen/look-on-the-monitor and must be
  user-confirmed — do not mark an issue `done` from a green build alone (see
  `CLAUDE.md` workflow rules).

---

## 6. See also
- `CLAUDE.md` — repo conventions, source-repo pointers, workflow rules.
- `docs/audio-decision.md` — why HDMI audio (vs PWM), the single-buffer trade-off.
- `docs/product-plan.md` — roadmap / V1.0 scope.
- `issues.jsonl` — work tracking (`PIZERO-NN`); HDMI-audio rework is PIZERO-35 → 36–40.

# ROADMAP

Where the RP2350-PiZero XRoar port stands and what's next. This is a navigation
map over `issues.jsonl` — it sequences the open work and links the tickets; it
does **not** restate ticket detail. Authoritative status always lives in the
ticket (`PIZERO-NN`) and the deep docs (`README.md`, `docs/`).

_Last updated: 2026-06-29._

## Shipped / working (V1.0 core)

The end-to-end product path is up and hardware-validated:

- **Video** — libdvi/PIO DVI, 320×240 → true **640×480p60** at 252 MHz
  (`PIZERO-45`); CoCo VDG render + blit, NTSC artifact colour (`PIZERO-43`).
- **Audio** — streaming HDMI data-island audio, no warble (`PIZERO-35`/`38`/`39`),
  pitch-matched at the in-spec 60 Hz clock.
- **Storage** — microSD ROM/disk load + AUTORUN (`PIZERO-08`/`10`).
- **Input (keyboard)** — USB HID keyboard works **when attached at cold boot**
  (`PIZERO-11`/`12`); keystrokes reach the emulator cleanly.
- **Stability** — hardware-watchdog freeze auto-recovery + cross-reset phase
  log deployed (`PIZERO-33`).
- **Docs** — README, BUILD, pipeline, hdmi-audio-notes, cpu-speed.

## Now: Input / USB workstream (active focus)

This is the current push. Order reflects value × independence.

1. **`PIZERO-11b` → `PIZERO-51` — USB hot-replug recovery.** Root cause is now
   fully diagnosed on hardware (rev3 silicon, E9 workaround off, PIO SM pins the
   line at J/FS → disconnect is invisible; an unplugged device floods ~180
   phantom HID reports/sec rather than erroring). `PIZERO-51` implements the fix:
   detect the phantom-report flood and force `pio_usb_host_stop()/restart()` to
   re-enumerate. Unblocks reliable device swapping (and easier gamepad testing).
2. **`PIZERO-49` — Numeric keypad.** Pure software keymap fix (extend
   `hid_table_init` for HID usages 0x53–0x63). Independent of the hotplug work;
   cheapest win.
3. **`PIZERO-13` — USB gamepad → CoCo joystick.** The user's PS2-style USB
   controller. Needs HID report-descriptor parsing (gamepads have no boot
   protocol) + the joystick-injection API. Easier to test once `PIZERO-51` lands.
4. **`PIZERO-50` — Header-pin joystick (fallback).** Hardware-independent backup
   if PIO-USB won't enumerate the controller; build its `coco_machine`
   joystick-injection API first since `PIZERO-13` reuses it.

## Next: Audio fidelity & polish

- **`PIZERO-41` — Source-side audio fidelity** (high): live CoCo SOUND is ~1
  semitone sharp + buzzier than desktop xroar; proven NOT HDMI-delivery →
  resampler/cycle-timing. Measure via `pizero_wavmeas`, compare rate constants.
- **`PIZERO-40` — Make streaming audio the default**, sweep POOL depth, validate
  budgets (folds into retiring the off-spec 52 Hz envs).
- **`PIZERO-32` — ACR CTS monitor compatibility** (multi-monitor; inert on the
  dev sink but matters for sinks that honour ACR).

## Release-readiness

- **`PIZERO-42` — Clean up diagnostic build cruft.** Now also covers the
  `PIZERO-11b` USB instrumentation (`src/usb_hotplug_diag.c`, the `[run]`/
  `[usb-evt]` counters, the `build_src_filter` entry): flag-gate or remove once
  `PIZERO-51` lands.
- **`PIZERO-33`** stays open until a real-world freeze's phase report is captured
  (auto-recovery is live; the next freeze self-documents).

## Later / stretch (deferred experiments)

Faithful-extension and "what-if" tracks, explicitly deferred:

- **`PIZERO-26` — Programmable VDG palette** (GIME-style registers) — the
  preferred near-term "extend the CoCo" feature.
- **`PIZERO-16` — Alternate video device** (TMS9918A-class sprites) — heavier.
- **`PIZERO-17` — Synth / sound-chip device** (AMY, AY/SID) — depends on the
  audio output path.

## Dependency notes

- `PIZERO-13` (gamepad) reuses `PIZERO-50`'s joystick-injection API and is much
  easier to validate after `PIZERO-51` (hotplug) — but none of the three is a
  hard blocker; the keyboard already works cold-boot.
- `PIZERO-40`/`PIZERO-42` should be coordinated (both retire diagnostic/off-spec
  envs).

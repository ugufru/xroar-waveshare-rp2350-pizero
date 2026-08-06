# ROADMAP

Where the RP2350-PiZero XRoar port stands and what's next. This is a navigation
map over `issues.jsonl` — it sequences the open work and links the tickets; it
does **not** restate ticket detail. Authoritative status always lives in the
ticket (`PIZERO-NN`) and the deep docs (`README.md`, `docs/`).

_Last updated: 2026-08-05._

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
- **Compatibility** — default env `pizero_stream_60` confirmed stable across
  multiple third-party games on hardware (user, 2026-07-19). Tracking the
  title-by-title results in `docs/compatibility.md` (`PIZERO-52`).
- **Docs** — README, BUILD, pipeline, hdmi-audio-notes, cpu-speed.

## Now: Input / USB workstream (active focus)

This is the current push. Order reflects value × independence.

1. **`PIZERO-11b` → `PIZERO-51` — USB hot-replug recovery.** Root cause is now
   fully diagnosed on hardware (rev3 silicon, E9 workaround off, PIO SM pins the
   line at J/FS → disconnect is invisible; an unplugged device floods ~180
   phantom HID reports/sec rather than erroring). `PIZERO-51` implements the fix:
   detect the phantom-report flood and force `pio_usb_host_stop()/restart()` to
   re-enumerate. Unblocks reliable device swapping (and easier gamepad testing).
2. **`PIZERO-50` — Header-pin joystick (now the PREFERRED player-1 route).**
   Bumped low→high: wire a **real CoCo joystick** (2× 500k pots + 1–2 buttons)
   straight to the RP2350 header as a "hat" — authentic feel, sidesteps the USB
   CPU budget entirely (no hub needed), lands in the same DAC-comparator
   injection API. Ticket carries the full wiring recipe (3V3-not-5V pot dividers,
   ADC0/ADC1 source-impedance + filter-cap notes, player-2 mux/I²C options).
   **Build its `coco_machine` joystick-injection API first** — `PIZERO-13` reuses
   it.
3. **`PIZERO-49` — Numeric keypad.** Pure software keymap fix (extend
   `hid_table_init` for HID usages 0x53–0x63). Independent of the hotplug work;
   cheapest win.
4. **`PIZERO-13` — USB gamepad → CoCo joystick.** The user's PS2-style USB
   controller. Needs HID report-descriptor parsing (gamepads have no boot
   protocol) + the joystick-injection API. Easier to test once `PIZERO-51` lands.
5. **`PIZERO-54` — USB hub support (`CFG_TUH_HUB`).** No internal hub; an external
   hub is required to run keyboard **and** a second device (keypad `PIZERO-49` or
   gamepad `PIZERO-13`) at once. Watch the core-0 headroom (`PIZERO-48`) and that
   it composes with the `PIZERO-51` stop/restart. Header-joystick (`PIZERO-50`)
   sidesteps this for player 1.

## Next: BIOS / launcher & VDG extensions

A new UX workstream — pick what to load and recolor it, all in-firmware with a
native-CoCo look. Dependency chain: `PIZERO-53` → `PIZERO-55`.

- **`PIZERO-53` — Firmware config/launcher overlay ("BIOS" menu).** Function-key
  overlay drawn over HDMI that browses the SD and loads/mounts images, plus
  firmware-only settings (SAM-speed cap, joystick source, audio env). **Renders
  as a real CoCo text screen** by reusing `render_alpha_frame` + the authentic
  `font_6847t1` char ROM (via a small source-pointer refactor), inverse-video
  selection. This refactor is the smallest self-contained first brick and it also
  unblocks the palette editor.
- **`PIZERO-55` — Programmable palette + editor + per-title `.pal` profiles.**
  The concrete, expanded successor to `PIZERO-26`: give the classic 6847 modes a
  GIME-style programmable palette (make the `g_vdg_rgb565_native[16]` LUT
  writable), a BIOS editor (64-colour GIME-authentic default / full-RGB advanced),
  and **per-title sidecar palettes** (`GAME.DSK` → `GAME.PAL`) with a per-title →
  user-global → factory fallback chain. Depends on `PIZERO-53` for the overlay,
  SD, and persistence.
- **`PIZERO-57` — Loadable VDG font from SD.** Sibling to `PIZERO-55`, same
  three-layer shape: a swappable `g_font` pointer (default = ROM `font_6847t1`), a
  BIOS `.FNT` loader with live preview, and **per-title font sidecars** (`GAME.DSK`
  → `GAME.FNT`, same fallback chain) — so a game can carry both its palette and
  its font. Cleanly decoupled from the palette LUT (font swaps need no rebuild);
  redefines the 64-glyph alpha slot. Depends on `PIZERO-53`.

## Next: SD write / save & load workstream

Today the port **cannot write to the SD card at all** — every `f_open` in project
code is `FA_READ` and there is no `f_write` anywhere. The FatFs stack underneath
is fully write-capable (`FF_FS_READONLY 0`, `disk_write()` implemented), so this
is additive firmware work, not a dependency fight. Chain:
`PIZERO-64` → `PIZERO-65` → `PIZERO-66` → `PIZERO-68`, with `PIZERO-67` and
`PIZERO-69` hanging off `PIZERO-64`.

- **`PIZERO-64` — SD write foundation.** Writable-file helper layer, atomic
  replace, an explicit flush/sync policy, and **measurement of real write
  latency** against the frame budget. Blocks everything else here; its numbers
  are the design input to `PIZERO-65`.
- **`PIZERO-65` — Deferred sector write-back.** The genuinely hard part, kept as
  its own ticket so the timing risk isn't buried in the feature. Core 0 has only
  ~31% slack at 1× (`PIZERO-48`) and SD block-erase stalls run tens-to-hundreds
  of ms, so a synchronous write on the emulation thread would starve the audio
  ring and drop frames. Queue + drainer, off the hot path.
- **`PIZERO-66` — FDC Write Sector → guest `SAVE`/`SAVEM`.** The headline
  feature. `SAVE`/`SAVEM` are **DECB commands running in the guest**, not XRoar
  calls — DECB does its own directory/granule allocation, so replacing the
  write-protect stub (`coco_machine.cpp:252-257`, status `0x40`) also buys
  `KILL`, `COPY`, `RENAME` and `BACKUP`.
- **`PIZERO-68` — DECB disk maintenance.** `DSKINI`/format, multi-drive, a real
  write-protect toggle, and honest FDC error status.
- **`PIZERO-67` — Firmware-side persistence API.** Config + per-title
  `.pal`/`.fnt` sidecar writes. **Closes a gap:** `PIZERO-53`/`55`/`57` all
  assume the firmware can write to SD and none of them declared it. Much easier
  than `PIZERO-66` on timing — BIOS saves happen with the overlay up, so a
  synchronous write is fine and the `PIZERO-65` queue isn't needed.
- **`PIZERO-69` — Emulator save-states.** Freeze/restore the whole machine.
  XRoar's `serialise.c` (846 lines) is already vendored and compiled in; only the
  `fs_*` primitives are no-oped (`xroar_stubs.c:21-45`), so this is ~18 one-line
  functions plus a `FILE*`→`FIL` bridge.

Cassette (`CSAVE`/`CLOAD`, `.CAS`/`.WAV`) is **not** covered by any of these —
there is no tape emulation in the port at all. Unticketed for now.

## Audio fidelity & polish

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

- **`PIZERO-56` — Higher-fidelity NTSC artifacts.** The current path is a
  deliberate 2-bits→4-colours LUT shortcut (canonical PMODE 4 look), *not*
  composite decoding — so extra CRT hues, fringing, context-dependence, and
  artifacting outside RG6 are absent by design. Options ladder from a wider
  context-keyed pattern LUT to a full composite decode; pairs with `PIZERO-55`.
- **`PIZERO-16` — Alternate video device** (TMS9918A-class sprites) — heavier
  track behind the palette work.
- **`PIZERO-17` — Custom wavetable/sampled-voice synth device** (Blofeld-style,
  *not* classic-chip emulation). The 48 kHz HDMI audio ring already exists
  (PIZERO-35/45), so this is now compute + a control surface: runs on core 0
  (~3–4 ms of the ~5.3 ms/frame headroom → ~4–8 rich voices), wavetables/samples
  in 16 MB flash with an SRAM active-frame cache (PSRAM is unpopulated; SRAM is
  ~89–94% full), guest control via bus-write interception (same trick as the
  `PIZERO-55` palette registers). Gated on **`PIZERO-58`**.
- **`PIZERO-58` — Synth voice-budget bench** (pre-work for `PIZERO-17`): a
  flag-gated build that runs N dummy voices and watches the `[run]` telemetry to
  measure real cyc/sample per complexity tier, float-vs-Q15, and flash-vs-SRAM
  wavetable access — turning the ~4–8-voice estimate into a measured cap.

### MIDI subsystem (drives the synth + external integration)

Connects the synth to the guest and the outside world; all build-flag gated so a
plain CoCo2 boot is untouched.

- **`PIZERO-59` — Internal MIDI bus/router + bit-banger serial transport.** Apps
  like Lyra use the CoCo's **bit-banger serial port** (not a cartridge ACIA) into
  an external UART box; the firmware simply *becomes* that box — tap the emulated
  serial line and decode it as MIDI, reusing the cycle-accurate PIA-tap technique
  from the audio work. A software router wires any source → any sink (synth,
  external, back to the guest). Gated on `PIZERO-61`; synth sink is `PIZERO-17`.
- **`PIZERO-60` — External MIDI endpoints.** USB-MIDI **device** over the native
  USB (composite with the existing CDC — appears to a DAW, *no extra hardware*),
  USB-MIDI **host** over PIO-USB (via the `PIZERO-54` hub), and an optional DIN-5
  path (the only one needing hardware). Plugs into the `PIZERO-59` router.
- **`PIZERO-61` — Pre-work: reverse-engineer Lyra's serial MIDI driver.** Nail
  the PIA lines, baud, framing, and whether Lyra bit-bangs 31250 directly or the
  box reclocks — via `analyze-rom`/`trace-calls`. Sets `PIZERO-59`'s decoder.
- **`PIZERO-62` — GIME-compatible programmable timer.** A CoCo3-layout timer at
  `$FF94/$FF95` (+ minimal `$FF90–$FF93` interrupt registers) reusing the
  cart-countdown scheduler, routable to IRQ/FIRQ. Goal: run *some* unmodified
  CoCo3 timer/music software (VDG-mode, own-handler programs) without full GIME
  and without touching CoCo2 purity — and give the guest a programmable tick for
  MIDI clock / sequencing. Full GIME (MMU, hi-res, 512K) stays out of scope.

## Dependency notes

- **`PIZERO-50`'s joystick-injection API is the shared root** for all joystick
  routes: `PIZERO-13` (USB gamepad) and the header hat both build on it. Build
  the API first regardless of which read-path ships first.
- `PIZERO-13` (gamepad) is much easier to validate after `PIZERO-51` (hotplug);
  `PIZERO-54` (hub) is only needed to run more than one USB device at once — none
  of these is a hard blocker, since the keyboard already works cold-boot.
- **`PIZERO-53` → `PIZERO-55`**: the launcher overlay (SD browse + persistence +
  the `render_alpha_frame` refactor) must land before the palette editor and
  per-title `.pal` profiles.
- **`PIZERO-53`/`55`/`57` also need `PIZERO-67`** (and therefore `PIZERO-64`):
  every one of them persists something to SD — firmware settings, `.pal`
  sidecars, `.fnt` sidecars — and the port currently has no write path at all.
  Their *read/render* halves are unblocked; only the **save** halves are gated.
- **SD write chain**: `PIZERO-64` (foundation + latency numbers) → `PIZERO-65`
  (write-back queue) → `PIZERO-66` (guest `SAVE`) → `PIZERO-68` (format,
  multi-drive, WP). `PIZERO-67` and `PIZERO-69` need only `PIZERO-64`, so either
  can be pulled forward. If `PIZERO-65` has landed, `PIZERO-69` must flush its
  queue before snapshotting or the restored state disagrees with the card.
- **Audio/MIDI chain**: `PIZERO-58` (bench) → `PIZERO-17` (synth); `PIZERO-61`
  (Lyra research) → `PIZERO-59` (MIDI bus) → `PIZERO-60` (external endpoints).
  `PIZERO-59` reuses the cycle-accurate PIA-tap from the audio work (`PIZERO-18`)
  and `PIZERO-60`'s host path composes with `PIZERO-54` (hub). The synth is the
  only heavy CPU/RAM consumer; the MIDI layer is near-free when idle.
- **`PIZERO-55` supersedes `PIZERO-26`** (now closed) — `PIZERO-26`'s palette-
  register idea became Part 1 of `PIZERO-55`, and its guest-side `$FFB0–$FFBF`
  bus-write interception (live recolor via CoCo-3-aware pokes) is `PIZERO-55`
  Part 4. All palette work is tracked in `PIZERO-55`.
- `PIZERO-40`/`PIZERO-42` should be coordinated (both retire diagnostic/off-spec
  envs).

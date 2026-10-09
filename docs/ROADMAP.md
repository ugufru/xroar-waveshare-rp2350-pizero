# ROADMAP

Where the RP2350-PiZero XRoar port stands and what's next. This is a navigation
map over `issues.jsonl`: it sequences the open work and links the tickets; it
does **not** restate ticket detail. Authoritative status always lives in the
ticket (`PIZERO-NN`) and the deep docs (`README.md`, `docs/`).

_Last updated: 2026-09-29 (documentation sweep, PIZERO-172)._

## Shipped / working (V1.0 core)

The end-to-end product path is up and hardware-validated:

- **Video**: libdvi/PIO DVI, 320×240 → true **640×480p60** at 252 MHz
  (`PIZERO-45`); CoCo VDG render + blit, NTSC artifact color (`PIZERO-43`).
  Screen fonts: the classic set, the 6847T1, and our 6847T2 by default
  (`PIZERO-166`).
- **Audio**: streaming data-island audio, no warble (`PIZERO-35`/`38`/`39`),
  pitch-matched at the in-spec 60 Hz clock, and the default since `PIZERO-45`.
  SN76489 sound chip (`PIZERO-143`).
- **CoCo 3 extras**: the GIME palette at `$FFB0-$FFBF` (`PIZERO-85`, `55`) and
  the GIME timer at `$FF90-$FF95` (`PIZERO-62`), both on by default.
- **Storage**: microSD ROM/disk load + AUTORUN (`PIZERO-08`/`10`). Whole-file
  writes exist: the settings editor saves text files and Print Screen saves
  PNGs (`PIZERO-146`, `165`). Guest disk writes do not (see below).
- **Overlay**: F9 to F12 open programs, cartridges, files and disks
  (`PIZERO-81`, `114`, `115`, `153`); per-game `NAME.TXT` settings
  (`PIZERO-154`); settings in `/coco/settings.txt`, edited on screen
  (`PIZERO-145`, `146`; `autorun.txt` too, `147`); the pad drives the overlay (`PIZERO-169`); F1
  opens an INFO page (`PIZERO-156`, first slice; the help reader is still open).
- **Input**: USB keyboard (`PIZERO-11`/`12`) and USB gamepad as both CoCo
  joysticks (`PIZERO-13`, `joystick_swap` `PIZERO-160`), together through a
  simple hub (`PIZERO-54`). Straight into the board, a device must be attached
  at power-on (`PIZERO-51`). History and lessons: `docs/usb-retrospective.md`.
- **Stability**: hardware-watchdog freeze auto-recovery + cross-reset phase
  log deployed (`PIZERO-33`).
- **Compatibility**: default env `pizero_stream_60` confirmed stable across
  multiple third-party games on hardware (user, 2026-07-19). No per-title
  record exists yet; that is `PIZERO-52` (open).
- **Docs**: README, SETTINGS, AUTORUN, BUILD, pipeline, video-audio-notes,
  cpu-speed, kit, usb-retrospective, show-prep-retrospective.

## Now: Input / USB workstream (active focus)

Updated 2026-09-28. Done: the gamepad (`PIZERO-13`), hub (`PIZERO-54`),
stick swap (`PIZERO-160`), keycap keyboard and keypad (`PIZERO-163`, `49`),
auto-repeat (`PIZERO-167`), screen glyphs (`PIZERO-162`) and the font setting
with our 6847T2 (`PIZERO-166`), pad buttons pressing CoCo keys (`PIZERO-164`),
Print Screen to a PNG (`PIZERO-165`), the pad driving the overlay
(`PIZERO-169`) and the F1 INFO page, first slice (`PIZERO-156`).
`docs/usb-retrospective.md` records how, and the lessons. What is left, in
order:

1. **`PIZERO-51`: Hot-plug straight into the board.** The software fixes are
   disproved (VBUS is hardwired); needs a switched VBUS. Through a hub, a
   keyboard plugged in while running did mount, which may be enough.
2. **`PIZERO-50`: A real CoCo joystick on the header pins.** Uses the
   joystick API that `PIZERO-13` built.
3. Lower: `PIZERO-157` (pad Switch mode), `PIZERO-158` (serial typing drops a
   character), `PIZERO-159` (chained USB-C hubs).

## Next: launcher and settings follow-ups

The overlay milestone is done (see Shipped). What remains of the original
launcher and "BIOS" chain:

- **`PIZERO-82`: Boot back into the last launched entry** (`laststate.txt`).
  Split out of `PIZERO-81`. Whole-file writes now exist (`coco_boot_save_text`),
  so it no longer waits on `PIZERO-64`.
- **`PIZERO-53`: Firmware settings menu.** Its settings half shipped as
  `/coco/settings.txt` plus the on-screen editor (`PIZERO-145`, `146`), with no
  settings GUI by the user's choice. Still open for what is left in the ticket.
- **`PIZERO-55`** (done at ~80%): shipped as the GIME palette registers and the
  `color_*` settings. Per-title palettes and a palette editor were dropped.
- **`PIZERO-57`: Loadable VDG font from SD.** The built-in font choice shipped
  as the `font` setting (`PIZERO-166`); loading a `.FNT` from the card is what
  stays open.
- Hardware checks still owed: `PIZERO-151` (each setting seen to take effect),
  `PIZERO-155` (per-game settings for cartridges, disks, revert, autorun).
  The BOOT button as an overlay key is `PIZERO-128` (deferred).

## Later: CoCo 3 (128 KB)

Gated on a faster 6809, measured before any GIME code: `PIZERO-129`
correctness harness, `PIZERO-130` 1.79 MHz benchmark, `PIZERO-131` the
speed-up, then `PIZERO-133` the machine (`134` for 640-wide modes, `135` the
open decisions). The plan and its reasoning are in `docs/coco3-plan.md`.

## Next: storage & filesystem layer (replaces DECB)

**Direction set 2026-08-05.** Floppy geometry, granule maps and 8.3 uppercase
names are obsolete. The board already has a fast hierarchical filesystem with
long filenames; emulating a 35-track floppy on top of it reaches *fewer*
capabilities than the hardware already has. So the storage model is a real
filesystem, and **floppy emulation is demoted to peripheral support**
(`PIZERO-79`). Compatibility with 40-year-old software is explicitly not the
priority; creating and editing new software on the CoCo is.

The stack, bottom to top. The design rule is **keep the 6809 side thin**: all
filesystem logic stays in C on the RP2350, so backends can change without
touching guest code.

- **`PIZERO-70`: Storage backend abstraction.** One device interface; SD/FatFs
  is backend #1. Capabilities are queryable, not assumed.
- **`PIZERO-71`: VFS core.** Hierarchy, long names, and above all **write**:
  create, extend, edit, delete, rename. This is the point of the exercise.
- **`PIZERO-72`: Guest hypercall ABI.** *The contract, and the crux.* Versioned
  from day one; BASIC, Bare Naked Forth and everything later bind to this rather
  than to each other. The sync-vs-async decision must be made here; retrofitting
  async into a blocking ABI breaks every client.
- **`PIZERO-73`: Replacement cart ROM.** A thin 6809 shim over the hypercall,
  not a DOS in assembly. Adds a 6809 assembler to the build.
- **`PIZERO-74`: New guest command surface.** Deliberately *not* DECB-compatible.
- **Backends**: `PIZERO-75` RAM disk (no hardware, no latency: the practical
  test target), `PIZERO-76` DriveWire/serial, `PIZERO-77` FujiNet-class networked
  storage (major focus; needs a hardware decision first), `PIZERO-78` USB MSC.

`PIZERO-66` (writable `.DSK` drives, 2026-10-08) writes synchronously from the FDC
command, as reads always have: both cores never share the card (core 1 only
scans out video), and the emulated CPU is halted while the drive works. Each
write is timed and the worst case logged when the drive syncs: 2.1 ms at most
for a SAVE's four writes, 6 ms for the sync, so `PIZERO-65` (the deferred
queue) closed as not needed. `PIZERO-64` closed as overtaken: the writable layer and
atomic replace shipped with settings, `drives.txt` and screendumps.

**No library removes `PIZERO-65`.** Verified in the installed deps: FatFs is
synchronous at every entry point, SdFat likewise, and carlk3's driver uses DMA
but spin-waits on it (`my_spi.c:198`). The one non-blocking API in the stack is
the SDIO backend's `tx_start`/`tx_poll` pair (`rp2040_sdio.h:101-111`), which we
don't currently use (`hw_config.c` is 1-bit SPI at 12.5 MHz). Tracked as
**`PIZERO-80`** (deferred; hardware feasibility first). Note even SDIO wouldn't
fix write stalls: the dominant cost is the card's internal program/erase, which
no transport speeds up.

## Supporting: SD write foundation

The firmware writes **whole files** to the card today: the on-screen editor
saves text through `coco_boot_save_text` (write to `.tmp`, then swap in), and
Print Screen writes PNGs through `coco_boot_screenshot` (`PIZERO-146`, `165`).
Both are rare, user-triggered writes, so their latency is tolerated. What does
not exist is a write from the running guest (disk sectors, save-states), which
must stay off the emulation hot path. These tickets underpin the filesystem
layer above; `PIZERO-66`/`PIZERO-68` are now legacy floppy polish
(`PIZERO-79`).

- **`PIZERO-64`: closed, overtaken.** The writable layer and atomic replace
  exist (settings, `drives.txt`, screendumps); the latency numbers now come
  from `PIZERO-66`'s `[dsk] synced` lines.
- **`PIZERO-65`: closed, not needed.** Measured on hardware 2026-10-09: a
  SAVE is 4 writes of at most 2.1 ms and a 6 ms sync, against the ~90 ms
  audio ring.
- **`PIZERO-66` (high, in progress): writable `.DSK` drives.** FDC Write
  Sector, images opened read-write with a read-only fallback, a W-key lock per
  drive remembered in `drives.txt`, `f_sync` at motor-off and on opening the
  overlay. Awaiting hardware confirmation.
- **`PIZERO-68` (low, legacy): DECB disk maintenance.** `DSKINI`/format,
  write-protect toggle, FDC error status. (Four drives already exist,
  `PIZERO-114`.)
- **`PIZERO-67`: Firmware-side persistence API.** Settings and per-game files
  are already written by the editor (`PIZERO-146`, `154`); what is left is any
  further firmware state (e.g. `.fnt` choices for `PIZERO-57`). Saves happen
  with the overlay up, so a synchronous write is fine.
- **`PIZERO-69`: Emulator save-states.** Freeze/restore the whole machine.
  XRoar's `serialise.c` (846 lines) is already vendored and compiled in; only the
  `fs_*` primitives are no-oped (`xroar_stubs.c:21-45`), so this is ~18 one-line
  functions plus a `FILE*`→`FIL` bridge.

Cassette (`CSAVE`/`CLOAD`, `.CAS`/`.WAV`) is **not** covered by any of these:
there is no tape emulation in the port at all. Unticketed, and under the new
direction it would be peripheral support if ever wanted.

## Audio fidelity & polish

- **`PIZERO-41`: Source-side audio fidelity** (high): live CoCo SOUND is ~1
  semitone sharp + buzzier than desktop xroar; proven NOT data-island delivery →
  resampler/cycle-timing. Measure via `pizero_wavmeas`, compare rate constants.
- **`PIZERO-40`: Sweep POOL depth and validate budgets.** Streaming audio is
  already the default (`PIZERO-45`); what is left is the sweep and retiring
  the off-spec 52 Hz fallback.
- **`PIZERO-32`: ACR CTS monitor compatibility** (multi-monitor; inert on the
  dev sink but matters for sinks that honor ACR).

## Release-readiness

- **`PIZERO-42`: Clean up diagnostic build cruft.** Now also covers the
  `PIZERO-11b` USB instrumentation (`src/usb_hotplug_diag.c`, the `[run]`/
  `[usb-evt]` counters, the `build_src_filter` entry): flag-gate or remove once
  `PIZERO-51` lands.
- **`PIZERO-33`** stays open until a real-world freeze's phase report is captured
  (auto-recovery is live; the next freeze self-documents).

## Later / stretch (deferred experiments)

Faithful-extension and "what-if" tracks, explicitly deferred:

- **`PIZERO-56`: Higher-fidelity NTSC artifacts.** The current path is a
  deliberate 2-bits→4-colors LUT shortcut (canonical PMODE 4 look), *not*
  composite decoding, so extra CRT hues, fringing, context-dependence, and
  artifacting outside RG6 are absent by design. Options ladder from a wider
  context-keyed pattern LUT to a full composite decode; pairs with `PIZERO-55`.
- **`PIZERO-16`: Alternate video device** (TMS9918A-class sprites), a heavier
  track behind the palette work.
- **`PIZERO-17`: Custom wavetable/sampled-voice synth device** (Blofeld-style,
  *not* classic-chip emulation). The 48 kHz data-island audio ring already exists
  (PIZERO-35/45), so this is now compute + a control surface: runs on core 0
  (~3–4 ms of the ~5.3 ms/frame headroom → ~4–8 rich voices), wavetables/samples
  in 16 MB flash with an SRAM active-frame cache (PSRAM is unpopulated; SRAM is
  **~75% full on the default `pizero_stream_60`**, measured 2026-09-27), guest
  control via bus-write interception (same trick as the `PIZERO-55` palette
  registers). Gated on **`PIZERO-58`**.
- **`PIZERO-58`: Synth voice-budget bench** (pre-work for `PIZERO-17`): a
  flag-gated build that runs N dummy voices and watches the `[run]` telemetry to
  measure real cyc/sample per complexity tier, float-vs-Q15, and flash-vs-SRAM
  wavetable access, turning the ~4–8-voice estimate into a measured cap.

### MIDI subsystem (drives the synth + external integration)

Connects the synth to the guest and the outside world; all build-flag gated so a
plain CoCo2 boot is untouched.

- **`PIZERO-59`: Internal MIDI bus/router + bit-banger serial transport.** Apps
  like Lyra use the CoCo's **bit-banger serial port** (not a cartridge ACIA) into
  an external UART box; the firmware simply *becomes* that box: tap the emulated
  serial line and decode it as MIDI, reusing the cycle-accurate PIA-tap technique
  from the audio work. A software router wires any source → any sink (synth,
  external, back to the guest). Gated on `PIZERO-61`; synth sink is `PIZERO-17`.
- **`PIZERO-60`: External MIDI endpoints.** USB-MIDI **device** over the native
  USB (composite with the existing CDC; appears to a DAW, *no extra hardware*),
  USB-MIDI **host** over PIO-USB (via the `PIZERO-54` hub), and an optional DIN-5
  path (the only one needing hardware). Plugs into the `PIZERO-59` router.
- **`PIZERO-61`: Pre-work: reverse-engineer Lyra's serial MIDI driver.** Nail
  the PIA lines, baud, framing, and whether Lyra bit-bangs 31250 directly or the
  box reclocks, via `analyze-rom`/`trace-calls`. Sets `PIZERO-59`'s decoder.
- **`PIZERO-62`** (done): the GIME-compatible timer at `$FF90-$FF95`, a
  programmable tick for MIDI clock and sequencing. Which GIME revision's reload
  quirk to emulate is `PIZERO-170`.

## Dependency notes

- **The joystick-injection API is the shared root** for all joystick routes:
  `PIZERO-13` (USB gamepad, done) built it, and `PIZERO-50` (header pins) reuses
  it.
- **`PIZERO-57`'s save half** (a remembered font choice) uses the whole-file
  write that already exists; it does not need `PIZERO-64`.
- **Storage/filesystem chain**: `PIZERO-66` (writable drives, the latency numbers)
  → `PIZERO-70` (backend interface) →
  `PIZERO-71` (VFS) → `PIZERO-72` (hypercall ABI) → `PIZERO-73` (cart ROM) →
  `PIZERO-74` (commands). `PIZERO-79` (floppy → peripheral) needs `PIZERO-73` for
  the default cart. Backends `PIZERO-75`/`76`/`77`/`78` need only `PIZERO-70`, and
  **`PIZERO-75` (RAM disk) is the one to build early**: it exercises the VFS,
  ABI and command surface with no card and no write latency confounding results.
- **`PIZERO-72` is the ABI freeze point.** It is the contract Bare Naked Forth and
  every later guest program bind to, so settle versioning and sync-vs-async there
  rather than discovering them in `PIZERO-74`.
- **Address space**: `PIZERO-62`'s GIME timer now occupies `$FF90-$FF95`, so
  `PIZERO-72`'s hypercall registers must go elsewhere.
- `PIZERO-67` and `PIZERO-69` need only `PIZERO-64`, so either can be pulled
  forward. If `PIZERO-65` has landed, `PIZERO-69` must flush its queue before
  snapshotting or the restored state disagrees with the card.
- **`PIZERO-66`/`PIZERO-68` are no longer blockers for anything.** Demoted to
  legacy peripheral polish by `PIZERO-79`.
- **Audio/MIDI chain**: `PIZERO-58` (bench) → `PIZERO-17` (synth); `PIZERO-61`
  (Lyra research) → `PIZERO-59` (MIDI bus) → `PIZERO-60` (external endpoints).
  `PIZERO-59` reuses the cycle-accurate PIA-tap from the audio work (`PIZERO-18`)
  and `PIZERO-60`'s host path composes with `PIZERO-54` (hub). The synth is the
  only heavy CPU/RAM consumer; the MIDI layer is near-free when idle.
- **`PIZERO-55` superseded `PIZERO-26`**; both are closed.
- `PIZERO-40`/`PIZERO-42` should be coordinated (both retire diagnostic/off-spec
  envs).

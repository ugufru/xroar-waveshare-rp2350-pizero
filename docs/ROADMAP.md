# ROADMAP

Where the RP2350-PiZero XRoar port stands and what's next. This is a navigation
map over `issues.jsonl`: it sequences the open work and links the tickets; it
does **not** restate ticket detail. Authoritative status always lives in the
ticket (`PIZERO-NN`) and the deep docs (`README.md`, `docs/`). The short ranked
list of what to pick up next is `roadmap.jsonl` (shown at the top of
`issues.html`).

_Last updated: 2026-11-05 (the CoCo 3 demos at full speed on average:
`PIZERO-326`, `327`, `330`, `331`, `333`, `334` done, SCARED from about 190 to
136.3 host clocks per emulated cycle; the peaks are `335`. Before that,
2026-11-01: the CoCo Zero is a CoCo 3, `PIZERO-280`; switching machine in
place, `314`, `319`; a DAC-off soak for `203` running; directory drives agreed
and sequenced, `PIZERO-288` to `304`)._

## Where things stand

From late September to early October the work was mostly features (58 issues
closed in two weeks, 33 of them features, 5 bugs). The ten days to 2026-10-10
were bring-up and stability: NitrOS-9, writable drives, and the first
laptop-free soak. That soak came out well, so the stability work now runs in
the background. From 2026-10-12 to 2026-10-13 the CoCo 3 track went from a
benchmark to a 128 KB CoCo 3 running at 60 fps at both 0.89 and 1.79 MHz,
and on 2026-10-20 it joined the CoCo 2 in one firmware (`PIZERO-135`). Then
the machine became three profiles (`PIZERO-242`): the CoCo Zero
(then a CoCo 2 with our extras, the default) and a stock CoCo 2 and CoCo 3.
On 2026-11-01 the CoCo Zero became a CoCo 3 with the extras, and the CoCo 2
with them stayed as `coco2+` (`PIZERO-280`).

## Shipped / working

- **Video**: libdvi/PIO DVI, a 640×240 one-byte-a-pixel framebuffer with a
  palette, shown as true **640×480p60** at 252 MHz (`PIZERO-45`, `134`), horizontal blanking 8/92/60 so the control period before
  video meets HDMI's 12px minimum (`PIZERO-233`); CoCo VDG render + blit, NTSC artifact color (`PIZERO-43`), F8
  cycles artifact colors per game (`PIZERO-186`). Screen fonts: the classic set,
  the 6847T1, and our 6847T2 by default (`PIZERO-166`).
- **Audio**: streaming data-island audio, the default since `PIZERO-45`. The
  rate servo (`PIZERO-121`) held the ring at its set-point with **0 skips over
  26.7 h**. SN76489 sound chip (`PIZERO-143`). Sample packets follow IEC 60958
  with a 48 kHz channel status, at exactly 48,000 samples/s, AC coupled
  (`PIZERO-223`); six times the old level at the default volume (`PIZERO-223`,
  `230`). Pitch and tone match the ROM, cycle exact (`PIZERO-41`). Delivery
  reworked (`PIZERO-232`): audio on the vblank lines too, full 4-sample packets,
  trailing guard bands, ACR on every 8th vblank line. Two screens that garbled
  the old delivery (the main monitor and the Samsung test TV of `PIZERO-224`)
  now play cleanly.
- **High-speed POKEs on the CoCo 2** (`PIZERO-132`):
  `POKE 65495,0` and `POKE 65497,0` run the machine up to 2× at 60 fps with
  full sound; normal speed by default and after reset (`docs/cpu-speed.md`).
- **CoCo 3 extras on the CoCo 2**: the GIME palette at `$FFB0-$FFBF`
  (`PIZERO-85`, `55`) and the GIME timer at `$FF90-$FF95` (`PIZERO-62`), on in
  the `coco2+` machine and off in the stock `coco2` (`PIZERO-242`, `PIZERO-280`).
- **Four machines** (`PIZERO-242`, `280`): `cocozero`, the default, a CoCo 3 with
  the extras; `coco2+`, a CoCo 2 with them; stock `coco2` and `coco3`. Switching
  between a CoCo 2 and a CoCo 3, from `settings.txt` or a game's own file,
  rebuilds the machine in place in under 0.1 s, with USB and the drives kept up
  (`PIZERO-314`, `319`: one 64 KB block kept for the machine from the first
  boot). Only a game autorun starts at power-on still restarts (`PIZERO-317`).
- **Boot**: reset to the BASIC `OK` in about 1.25 s with no computer attached;
  every boot is timed phase by phase (`PIZERO-315`,
  `docs/measurements/boot-times.md`). `debug = on` makes the boot wait for a
  serial monitor and log its times to the card (`PIZERO-316`).
- **MIDI Pak** (`PIZERO-287`): a 6850 at `$FF6E`, the CoCo's MIDI port, seen by
  the computer on the native USB-C port as "CoCo Zero MIDI Pak", on both
  machines. Real software next (`PIZERO-318`).
- **A 128 KB CoCo 3**, in the product firmware (the default `cocozero`, or
  `coco3`; loads `/coco/roms/coco3.rom`): the vendored GIME is the bus (`PIZERO-133`), the
  6809 reads and writes MMU pages directly (`PIZERO-213`), and a port-side GIME
  renderer draws each changed scanline straight into the framebuffer
  (`PIZERO-218`). 60 fps at 0.89 and 1.79 MHz, HSCREEN 2 drawn; a full-screen
  HCLS loop at 1.79 MHz holds 60 fps with no late frames and full sound
  (`PIZERO-219`, `220`). 80-column and the other 640-wide modes show whole
  (`PIZERO-134`).
- **Storage**: microSD ROM/disk/cartridge load and autorun (`PIZERO-182`, `183`:
  drives remembered across power-off). **Writable `.DSK` drives**: SAVE from
  BASIC and NitrOS-9, W lock per drive, N makes a named blank disk, R renames
  (`PIZERO-66`, `193`, `194`, `196`). Firmware whole-file writes for settings,
  per-game files and Print Screen PNGs (`PIZERO-67`, `146`, `165`).
- **NitrOS-9 Level 1 boots** to `OS9:` and saves survive a power cycle
  (`PIZERO-190`, `191`, `194`). `scripts/mkos9disk.py` builds a working disk
  (`PIZERO-199`, booted on the board 2026-10-12).
- **Overlay**: F9 to F12 open programs, cartridges, files and disks
  (`PIZERO-81`, `114`, `153`); INFO page (`PIZERO-156`; on F1 until PIZERO-215 gave F1 to the CoCo 3); per-game `NAME.TXT`
  (`PIZERO-154`); `settings.txt` and `autorun.txt` edited on screen
  (`PIZERO-145`, `146`, `147`); the pad drives the overlay (`PIZERO-169`).
  F11 lists every text file in `/coco` and the ASCII files on mounted disks
  (read-only, `NAME.EXT:N`), edits `drives.txt` too, and refuses files over
  4 KB (`PIZERO-296`, `309`, `312`, `313`); F5 wraps lines (`PIZERO-310`);
  CR, LF and CRLF read everywhere and a file keeps its own line ends when
  saved, new ones CR (`PIZERO-294`, `295`); mounted disks always in F12
  (`PIZERO-312`). Disk BASIC file rules library (`PIZERO-289`).
- **Cartridges** of 2 to 16 KB (`PIZERO-139`); bank-switched carts over 16 KB
  are built and host-tested (`PIZERO-142`); a hardware test waits on a tester with an image (`PIZERO-226`).
- **Input**: USB keyboard that types what the keycaps say, keypad, auto-repeat
  (`PIZERO-163`, `49`, `167`); USB gamepad as both joysticks, buttons as keys
  (`PIZERO-13`, `160`, `164`); keyboard and pad together through a simple hub
  (`PIZERO-54`). A device straight into the board must be attached at power-on
  (`PIZERO-51`). Lessons: `docs/usb-retrospective.md`.
- **Stability**: watchdog auto-recovery with phase, sub-phase and reset reason
  carried across the reset (`PIZERO-33`, `203`), and a post-mortem of where
  core 0 was (`PIZERO-227`). Both cores' stacks are watched and their deepest
  use printed (`PIZERO-319`). The 26.7 h laptop-free soak
  (`PIZERO-168`, logged to the card by `PIZERO-201`) measured **0.075
  freezes/h**, against 0.46/h in September (`PIZERO-98`). Figures in the
  README's "Stability, measured".
- **Trust tooling**: 338 host tests in about 15 s (`pio test -e native`,
  `PIZERO-109`), including a 6809 lockstep harness that runs the live core
  against a frozen reference (`PIZERO-129`). `scripts/soak.py` records and
  analyses soaks; `scripts/qc_flash.py` flashes and logs a batch of boards
  (`PIZERO-189`, 12 boards so far).
- **Docs**: README, SETTINGS, AUTORUN, BUILD, pipeline, video-audio-notes,
  cpu-speed, kit, coco3-plan, coco3-feasibility, coco3-titles,
  usb-retrospective, show-prep-retrospective, coco3-retrospective; for the
  hardware's future, supplier-primer and coco-zero-2-proposal.

## In flight (waiting on a hardware step)

These need the board or the user rather than more code (`PIZERO-199`, NitrOS-9
on the board, passed 2026-10-12):

- **`PIZERO-203`: the USB timer-interrupt hang.** The one live stability bug:
  every post-mortem since 2026-10-25 is core 0 stuck in PIO-USB's timer
  interrupt, about one every 3 h. A soak with `usb_audio = off` started
  2026-11-01 on `575c5a4`, to test the DAC stream, the first suspect. Pull the
  card and analyse when it has run long enough to compare.
- **`PIZERO-318`**: Lyra 2.85 and CoCo MIDI 3 through the MIDI Pak, on `coco2+`.

## Done: the CoCo 3 track

The plan and its reasoning are in `docs/coco3-plan.md`; what fits and the
measured outcome in `docs/coco3-feasibility.md`. Done so far:

- **`PIZERO-129`** (lockstep harness), **`PIZERO-130`** (benchmark: 185.4 host
  clocks per emulated cycle, 1.79 MHz took 131.7% of a frame), **`PIZERO-131`**
  (about 101 clocks per cycle, so 1.79 MHz fits).
- **`PIZERO-133`**: `pizero_coco3` boots a 128 KB CoCo 3 to OK (29 fps at first).
- **`PIZERO-213`**: the MMU fast path. **`PIZERO-218`**: the port-side GIME
  renderer; 60 fps at both speeds.
- **`PIZERO-219`**: worst case measured (full-screen HCLS at 1.79 MHz: 3-5 late
  frames a second). **`PIZERO-220`**: fixed, no late frames, done 2026-10-15.
- **`PIZERO-214`**: WIDTH 40, palette, the GIME timer and HSCREEN 1 and 2
  checked on the board, and sound at both speeds, done 2026-10-19.
- **`PIZERO-228`**: seven real titles on the board, done 2026-10-19: 128K
  games work, 512K games cannot (`docs/coco3-titles.md`).
- **`PIZERO-236`**: NitrOS-9 Level 2 boots from a floppy that
  `scripts/mkos9disk.py --level 2` builds, done 2026-10-20.
- **`PIZERO-215`**: ALT, CTRL, F1 and F2, and the second fire buttons (R2,
  L2), done 2026-10-20.
- **`PIZERO-135`**: one firmware, either machine; a game's own settings file
  switches to the CoCo 3, done 2026-10-20.

What is left is small, and none of it blocks the next work: **`PIZERO-216`** (LOADM under a non-default MMU map),
**`PIZERO-217`** (tearing), **`PIZERO-170`** (GIME timer reload quirk),
**`PIZERO-178`** (render cost per mode, largely answered by 218-220) and
**`PIZERO-237`** (faster 640-wide screenshots, a host test of that path).

Related CPU-speed item: **`PIZERO-211`** (RAM back from the fast 6809 core).

## CoCo 3 demo speed (`PIZERO-325`, from 2026-11-04)

The demos (SCARED, DDEMO) ran at about three quarters of full speed: about
190 host clocks per emulated cycle against the 141 a 252 MHz core 0 allows at
1.79 MHz. Each step is measured on `pizero_coco3_prof` (`docs/BUILD.md`), and
the demos' own code is never looked at (diagnosed from the emulator's side).

- Done: **`PIZERO-326`** (interrupt line sampling), **`327`** (the
  instruction loop), **`330`** (changed lines decoded on core 1), **`331`**
  (the hot flash code grouped for the XIP cache), **`333`** (border lines a
  compare), **`334`** (PIA reads straight to the PIA). SCARED now averages
  **136.3**, about 95% of full speed in the profiling build; the user reports
  every demo running great on the product build.
- Next: **`PIZERO-335`** (the heaviest stretches still peak at about 166),
  then the 6809 core rewrites **`PIZERO-328`** (registers in host registers)
  and **`329`** (computed-goto dispatch). **`PIZERO-332`** does for the CoCo 2
  what `331` did for the CoCo 3. The tear-free picture stays in `325`.

## The AY-3-8910 (done 2026-10-22)

**`PIZERO-149`**: one bank of four AY chips behind three cartridge front-ends,
the CoCo PSG (`$FF5E`), the Symphony-12 (PIA at `$FF60`) and the Deluxe CoCo
(`$FF38`, off by default), on in the CoCo Zero. **`PIZERO-250`** put the
Symphony-12 in stereo and **`PIZERO-248`** cut the chips' cost (every voice at
once now takes 9.3-9.7 ms a frame, from 15.4-16.7), both done the same day.
**`PIZERO-253`** added the real CoCo's sound multiplexer, sound enable and
single-bit output, with every source heard at once on the CoCo Zero
(`sound_sources = all`) and joystick reads kept out of the sound
(`joystick_quiet`). All of it is written up in [`sound.md`](sound.md). Left:
**`PIZERO-251`** (real software: Lyra 1.2, the Sierra PSG games),
**`PIZERO-252`** (the SN76489's silent cost, and four AY chips measured on a
CoCo 3 at 1.79 MHz), **`PIZERO-249`** (the Speech/Sound Cartridge).

## Hardware direction

The 512 KB CoCo 3 needs the board's empty PSRAM footprint filled: first
Waveshare boards with it fitted at the factory, whose samples also measure
whether 512 KB runs at full speed from PSRAM; later a board of our own
designed around the pain points found so far (`PIZERO-244`,
`docs/coco-zero-2-proposal.md`; ordering in `docs/supplier-primer.md`,
`PIZERO-243`).

## Next: experience features (small, unblocked)

Good change-of-pace work between CoCo 3 stages. None waits on anything.

- **`PIZERO-82`**: boot back into the last game (`laststate.txt`), with a
  documented precedence against autorun. Pairs with **`PIZERO-188`** (what
  autorun is for).
- **`PIZERO-117`**: after mounting a disk, offer to run what is on it.
- **`PIZERO-171`**: attract mode, a playlist of demos when idle.
- **`PIZERO-177`**: a one-line `classic` setting for an authentic CoCo 2. In
  Done 2026-10-22 as `PIZERO-242`'s `machine = coco2`.
- **`PIZERO-173`**: type `{ } | ~` and paste faster through BASIC's keyboard
  routine. **`PIZERO-197`**: Caps Lock chord for NitrOS-9.
- **`PIZERO-205`**: the help reader (CoCo-styled Markdown, built-in help,
  card documents). **`PIZERO-206`**: the rest of the INFO facts.
- **`PIZERO-57`**: load a `.FNT` from the card (the built-in font choice
  shipped as `PIZERO-166`).
- Media: **`PIZERO-138`** (`.cas` in the overlay).

## Add-on hardware (bigger, unstarted)

- **Sound**: (`PIZERO-149`, the AY-3-8910, and `PIZERO-148`, the
  Orchestra-90CC, are done, above.)
- **Video**: `PIZERO-16` (TMS9918A), `PIZERO-176` (MC6845 80-column card);
  the new-device chain `PIZERO-83` (decision) → `PIZERO-84` (linear 320×240) and
  `PIZERO-89` (wide text).
- **Joystick on the header pins**: `PIZERO-50`, reusing the joystick API
  `PIZERO-13` built.

## Storage & filesystem layer (direction set 2026-08-05)

The long-term storage model is a real filesystem with the 6809 side kept thin;
floppy emulation becomes peripheral support (`PIZERO-79`). Writable `.DSK`
drives shipped first (`PIZERO-66`), and they write synchronously because a SAVE
costs at most 2.1 ms per sector and 6 ms per sync (so `PIZERO-65`, the deferred
queue, closed as not needed).

The chain, bottom to top: `PIZERO-70` (backend interface) → `PIZERO-71` (VFS)
→ `PIZERO-72` (hypercall ABI, the contract and freeze point) → `PIZERO-73`
(replacement cart ROM) → `PIZERO-74` (guest commands). Backends need only 70:
`PIZERO-75` (RAM disk, build it early as the test target), `PIZERO-76`
(DriveWire), `PIZERO-77`, `78` (deferred). `PIZERO-113` (power loss mid-write)
matters now that the guest writes to the card. `PIZERO-69` (save-states) and
`PIZERO-68` (DECB maintenance) are deferred.

### Directory drives (agreed 2026-11-01)

What BASIC sees goes the other way (`PIZERO-288`, done): an unmodified Disk
BASIC reading a host directory through the emulated floppy, 8.3 names only,
with the chain above kept for the Forth kernel and OS-9. The design and the
ROM facts it rests on are in
[`directory-drives-proposal.md`](directory-drives-proposal.md).

In order:

Done 2026-11-01: `PIZERO-294` (parsers read CR, LF and CRLF), `289` (Disk
BASIC file rules), `295` (editor line endings), `296` (FILES lists text
files), with `309`, `310`, `312`, `313` beside them.

1. `PIZERO-291`: trace Disk BASIC's write order on hardware, the spec for
   writes.
2. `PIZERO-290`: read-only directory drives on the SD card.
3. `PIZERO-292`: writable directory drives.
4. `PIZERO-293` (a directory path in `drives.txt` mounts as a directory
   drive; `drives.txt` itself is already editable, `PIZERO-313`),
   `PIZERO-297` (unpack and pack a `.DSK`).
6. Low priority, any order: `PIZERO-299` (boot flag), `300` (DIR listing),
   `303` (delete, copy, info), `304` (`.DSK` check), `301` (hex editor),
   `302` (detokenized listing), `298` (ZIP import).

USB mass storage stays `PIZERO-78` (deferred): the SD card proves the
backend, which is the only part USB changes.

## Fixes and checks still owed (low urgency)

- **Bugs**: `PIZERO-158` (serial
  typing drops a character), `PIZERO-195` (audio blip on launch), `PIZERO-204`
  (clock seeded hours ahead), `PIZERO-100` (SAM base `$0000`), `PIZERO-198`
  (fdctrace env crash), `PIZERO-159` (chained USB-C hubs), `PIZERO-157` (pad
  Switch mode).
- **Hot-plug**: `PIZERO-51`. Software paths are disproved; test unplug and
  replug through the hub before considering a VBUS switch mod.
- **Other screens**: `PIZERO-99`; `PIZERO-233` (recheck the dev monitor and the
  upstairs Samsung on the new audio delivery). One LCD shows the centre band
  (`PIZERO-120`) and raspy, quiet audio (`PIZERO-234`, parked: the board's
  samples are clean, the Mac is clean on it; a Raspberry Pi at 640x480p60 would
  tell the monitor from our signal). `PIZERO-32` (ACR CTS, now only the 24 MHz
  fallback).
- **Audio**: `PIZERO-231` (does hard-driven music clip at volume 11-15).
- **Hardware sightings**: `PIZERO-137`, `144`, `151`, `155`, `200`.
- **Tooling**: `PIZERO-110` (on-board self-test), `PIZERO-207` (ring accounting
  test), `PIZERO-88` (frame-time headroom, refresh), `PIZERO-42` (remove code
  behind retired flags; the `PIZERO-234` test envs stay while it is parked),
  `PIZERO-106` (issues.html tag pills, user check).
- **Kit and case**: `PIZERO-93` (power and keyboard advice in `kit.md`),
  `PIZERO-97`, `PIZERO-102`, `PIZERO-185` (no-name SD cards).

## Later / stretch (deferred)

- **`PIZERO-56`**: higher-fidelity NTSC artifacts (the current path is a
  deliberate 2-bit LUT, not composite decoding).
- **Synth and MIDI**: `PIZERO-58` (voice-budget bench) → `PIZERO-17`
  (wavetable synth device). MIDI went first through `PIZERO-287` (the MIDI
  Pak, a 6850 at `$FF6E`, which Lyra, UltiMusE III and CoCo MIDI Pro use,
  routed to and from the native USB-C port as a USB MIDI device; done
  2026-11-01, real software is `PIZERO-318`), then `PIZERO-305` (the same chip on the PIO-USB
  host port, which `PIZERO-281` requires). The bit-banger path `PIZERO-61` →
  `PIZERO-59` and the DIN endpoints in `PIZERO-60` stay deferred. The MIDI
  Pak is the `midi_pak` setting (on for the CoCo Zero), not a build flag;
  the synth work would be build-flag gated. `PIZERO-170` (GIME timer reload
  quirk) sets the tempo of timer music.
- **`PIZERO-128`**: the BOOT button as an overlay key. **`PIZERO-80`**: SDIO.

## Dependency notes

- **CoCo 3**: `PIZERO-129` → `130` → `131` → `133` → `213` → `218` → `219` →
  `220` → `214`, `215`, `134`, `135` (all done) → `242` (profiles);
  `179`/`216`/`217` remain. A 512 KB CoCo 3 waits on PSRAM (`PIZERO-244`). Since `PIZERO-319` one 64 KB block is
  kept for the machine from the first boot: the CoCo 3's second 64 KB, or a CoCo 2's VDG buffer and
  up to 40 KB of a `PIZERO-142` banked cart, so switching machine never needs the heap whole.
- **Storage**: `PIZERO-70` → `71` → `72` → `73` → `74`; `PIZERO-79` needs 73
  for the default cart. Directory drives: `PIZERO-289` → `290` → `292`, with
  `291` → `292`; `290` → `293`; `289` → `297`; `294` → `295`; `299` → `303`;
  `304` before `297`'s reports; `296` uses `205` for `.md`. `PIZERO-62`'s GIME timer occupies `$FF90-$FF95`, so
  `PIZERO-72`'s hypercall registers must go elsewhere.
- **Joystick**: the injection API from `PIZERO-13` is the shared root;
  `PIZERO-50` reuses it.
- **Fonts**: `PIZERO-57`'s remembered choice uses the existing whole-file write.

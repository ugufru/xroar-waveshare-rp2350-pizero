# CoCo 3 plan: a faster 6809 first, then a 128 KB CoCo 3

_Written 2026-09-26. Follows the feasibility study in
[`coco3-feasibility.md`](coco3-feasibility.md) (PIZERO-111). Tickets:
PIZERO-129 to PIZERO-135._

## Why this order

[`coco3-feasibility.md`](coco3-feasibility.md) showed that a 128 KB CoCo 3
fits in RAM (about 23 KB spare) and that the 320-wide GIME modes drop straight
into our 320x240 framebuffer. The wall is the emulated 6809: 10.4 ms per frame
at the CoCo 2's 0.895 MHz becomes about 20.8 ms at the CoCo 3's 1.79 MHz,
125% of a 16.67 ms frame. So nothing CoCo 3 is worth building until the CPU
is faster, and the CPU must not get faster at the cost of getting wrong.

A stock CoCo 3 has a 6809 (68B09E). The Hitachi 6309 was a popular owner
upgrade, not the shipped part, so it is out of scope.

A CoCo 3 at about 80% of real speed is an acceptable outcome, as long as the
audio behavior below real time is decided on purpose (PIZERO-135).

## What the research found

- **Cost today:** about 180 host clocks per emulated bus cycle at 252 MHz.
  Running 29,830 cycles a frame (1.79 MHz at 60 Hz) in about 12.5 ms, which
  leaves room for render, blit and audio, needs about 105. That is **1.7x**.
- **Hot spots** (`lib/xroar_core/src/mc6809/`,
  `lib/coco_machine/src/coco_machine.cpp`):
  1. Every bus cycle, including the 6809's dummy "NVMA" cycles
     (`NVMA_CYCLE`, `mc6809_common.c:96`, up to five per indexed address), is
     an out-of-line call into `coco_mem_cycle` (`coco_machine.cpp:427-599`),
     which sits in another translation unit and so is never inlined.
  2. Each access repeats the same bookkeeping: a two-stage address decode,
     `total_mem_cycles++`, an event-list check, `audio_integrate(16)`, an IRQ
     dirty check and `cycles_remaining -= 16`. The CPU also stores three
     interrupt latches per access (`mc6809_common.c:60-93`).
  3. `op_sub`, `op_sub16`, `op_add16` and `word_immediate` ended up in
     **flash**, reached through veneers from the RAM-resident `mc6809_run`.
     They sit behind every CMP, SUB and 16-bit immediate.
  4. Dispatch is one large switch (`mc6809_run`, `mc6809.c:217`).
- **No 6809 correctness test exists**, here or in upstream XRoar. Optimizing a
  trusted core without one is the largest risk in this plan.
- **Measurement exists:** the `[run] cpu=` serial field (`src/main.cpp`),
  read by `scripts/soak.py analyse` and archived in `docs/measurements/`.
- **A latent bug:** SAM double speed is never applied. The fast path always
  charges 16 ticks (`coco_machine.cpp:455,481`), so `cpu-speed.md` is wrong
  on this point (PIZERO-132).
- **The CoCo 3 port shape** (upstream `~/github/xroar`, at the pinned
  `a2d31903`): `tcc1014.[ch]` (the GIME, 1612 lines) and `font-gime.[ch]` can
  be vendored as they are; their dependencies (part, events, serialise,
  delegate) are already here, plus a `vo.h` stub. `coco3.c` (1532 lines) is
  desktop glue and gets rewritten into our machine. 1.79 MHz is simply 8
  instead of 16 ticks per access (`tcc1014.c:832`). Video arrives one scanline
  at a time as 6-bit GIME colors, up to 640x225.

## The four stages

Each stage is a ticket, each is gated by a measurement, and each closes at
about 80% with the remainder filed.

### 1. A 6809 correctness harness (PIZERO-129, host only)

Freeze today's core under `test/` as the reference. A new native suite runs
the reference and the working core side by side against the same flat 64 KB
memory, recording every bus access and the registers at every instruction
boundary, and fails at the first divergence. Inputs: seeded random
instruction streams covering all three opcode pages, every indexed mode and
the interrupt paths; optionally a real ROM boot when a local ROM folder is
given, since the repo ships no ROMs.

### 2. A 1.79 MHz benchmark (PIZERO-130, hardware)

A new env, `pizero_cpu179`, runs the CoCo 2 machine at twice its cycles per
frame: exactly the CPU load of a CoCo 3, before any GIME cost. Record the
baseline at 1x and 2x through `soak.py`, and teach `soak.py analyse` to print
the frame-budget percentage it is now worked out by hand.

### 3. Make the core faster (PIZERO-131)

Cheapest and safest first, stopping as soon as the 2x build fits a frame:

1. Move the ALU helpers into RAM.
2. Let dummy NVMA cycles advance time without an address decode or ROM read.
3. An inline page-table fast path for RAM and ROM, with only I/O and SAM
   writes going through `coco_mem_cycle`.
4. Batch the per-access bookkeeping: a cached next-event tick, audio
   integrated in spans, one cycle check per instruction.
5. Only if still short: table or computed-goto dispatch, or one translation
   unit for the core and memory path.

Every step must pass the harness and all native tests and be measured on
hardware. Any gain also gives the CoCo 2 product build more headroom.

**The gate:** at full speed, go on. At about 80% or better, go on with the
PIZERO-135 audio policy. Well short of that, stop and record why in
`coco3-feasibility.md`.

### 4. A 128 KB CoCo 3 (PIZERO-133)

Only after the gate. Vendor the GIME and its font; add a CoCo 3 mode to
`lib/coco_machine` with a flat 128 KB RAM, the GIME as the bus (with the
stage 3 fast path), and the existing PIAs, keyboard, DAC and disk path. A
per-scanline sink writes every other sample through a 64-entry RGB565 table
built by the existing `coco_pal_gime_to_rgb565()` into the framebuffer. In
this mode the GIME owns the palette, timer and interrupts; our partial
`gime_timer.h` stays only for the CoCo 2 hybrid. `coco3.rom` loads from the
card like the other ROMs, with a boot page when it is missing.

First milestone: 32 and 40 column text and the 320 and 256 wide graphics
modes. The 640-wide modes, which include 80-column text, need the picture
generated a line at a time instead of from a framebuffer (PIZERO-134);
halving them to 320 makes 80-column text unreadable.

## Out of scope

512 KB (the board has no PSRAM), the 6309, save states.

## Open questions (PIZERO-135, before stage 4)

1. How is CoCo 3 mode chosen: automatically when `coco3.rom` is on the card,
   by an `autorun.txt` directive, or a separate firmware build?
2. Below real time the machine makes fewer sound samples than the video link
   plays, and the audio servo's 0.13% correction cannot cover 20%. Lower pitch, or
   normal pitch with gaps?

## Verification

- `pio test -e native`: the harness reports zero divergences, and every
  existing suite passes.
- `pizero_cpu179` on hardware through `soak.py record` and
  `analyse --kind bench`: `cpu=` median and p99 at 29,830 cycles a frame and
  the frame-budget percentage, archived with the git hash.
- The product build (`pizero_stream_60`) soaks as before: fps in band, audio
  unchanged, `cpu=` lower than today's 10.4 ms.
- Stage 4: `coco3.rom` boots to the CoCo 3 BASIC prompt, `WIDTH 40` works, a
  palette POKE recolors the screen, a GIME timer program runs, and the
  320-wide HSCREEN modes draw.

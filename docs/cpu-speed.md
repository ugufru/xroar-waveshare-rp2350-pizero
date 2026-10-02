# Emulation pacing & CoCo high-speed (double-speed) behavior

Does running the emulated CoCo at "double speed" hold up on this hardware? Short
answer: **the port does not run it at all today.** The SAM high-speed POKE is accepted
but has no effect on speed: every memory access is charged the normal slow-cycle cost,
so the guest keeps running at 1× (tracked as `PIZERO-132`). The rest of this doc explains the pacing model, why
audio and video would stay correct if fast mode were honored, and why core-0 headroom
would then be the limit.

## There is no host fast-forward

This port has **no host-side fast-forward / turbo toggle**: no key combo, no
[`AUTORUN.md`](../AUTORUN.md) directive, no flag. The only "double speed" a CoCo 2 has
is the **SAM high-speed POKE**, driven by the guest program itself:

- `POKE 65495,0`: *address-dependent* high speed (SAM `R = 1`). Only accesses outside
  the display/IO region run fast. Effective ~1.3–1.5× on real hardware. The "safe" mode
  real CoCo 1/2 hardware can mostly survive.
- `POKE 65497,0`: *full* high speed (SAM `R = 2`). Every cycle runs fast, ~2×. On real
  hardware this corrupts the video.

## What the port actually does with it

The vendored XRoar SAM (`lib/xroar_core/src/mc6883.c`) implements the speed change: a
fast memory cycle costs **8 event-ticks** versus **16** for a normal cycle. But this
port does not call it for ordinary accesses. `coco_mem_cycle` in
`lib/coco_machine/src/coco_machine.cpp` uses its own fast-path address decode, inherited
from the AMOLED port, which charges a **flat 16 ticks** for every RAM, ROM, cartridge
and I/O access. Only a write to the SAM registers (`$FFC0-$FFDF`) goes through
`mc6883_mem_cycle`, so the POKE does update the SAM's state, but nothing reads the new
speed. The guest runs at 1× whatever it pokes.

Honoring fast mode would mean charging `mc6883_mem_cycle`'s cycle cost (or an
equivalent R-aware decode) on the fast path. The sections below are the analysis of
what would follow.

## The pacing model: fixed emulated *time* per frame

This is the key, non-obvious fact. The main loop does **not** run a fixed number of CPU
instructions per frame; it runs a fixed amount of **emulated time**, measured in event
ticks. In `lib/coco_machine/src/coco_machine.cpp`:

```c
// run_cpu_with_audio()
g_m.cycles_remaining = (int32_t)cycles * 16;   // budget in event TICKS, not instructions
```

and each memory access subtracts its own tick cost (`ncycles` in `coco_mem_cycle`,
today always 16). So `coco_machine_run_cycles(CYCLES_PER_FRAME)` always advances:

```
CYCLES_PER_FRAME × 16  =  14915 × 16  =  238,640 ticks  ≈  16.667 ms of emulated time
```

per frame. If fast accesses cost 8 ticks, full fast mode would fit up to ~29,830 cycles
into the *same* emulated 16.667 ms window, which is exactly how real hardware behaves
when you flip the SAM speed bit.

`CYCLES_PER_FRAME` / `FRAME_PERIOD_US` are defined per build in `src/main.cpp` (14915 /
16667 µs for the default 60 Hz build).

## Why audio and video would stay correct

Because the per-frame emulated-time budget is constant, two things people worry about
would be **non-issues**:

- **Audio pitch.** Samples are generated event-tick-keyed (`EVENT_TICK_HZ = 14318180`
  in `coco_machine.cpp`), and ticks-per-frame is constant → still **exactly 800
  samples/frame** in fast mode → no pitch shift and no resampler distortion. (The
  "fast-forward distortion" noted in [`video-audio-notes.md`](video-audio-notes.md) was a
  *separate* startup bug, a producer burst overflowing a too-small ring, fixed by the
  8192-deep primed ring. It was **not** the SAM speed.)
- **Video.** Core 1's `libdvi` worker is fully decoupled and keeps emitting 60 Hz of
  whatever `g_front` holds (see [`pipeline.md`](pipeline.md)). Guest speed never touches
  it.

## Where it would break: core-0 headroom

Full fast mode executes ~2× as many 6809 memory cycles inside the same per-frame budget,
so the **host cost of emulation roughly doubles**. The measured 1× frame budget on
`pizero_stream_60` is **76.2%** (noted by the GIME flags in `platformio.ini`): ~12.7 ms
of the 16.67 ms frame, leaving ~4 ms of slack. The split between emulation and
render + blit has not been re-measured at that figure; the older README Performance
numbers (~9.4 ms emulation, ~2.1 ms render + blit, 31% slack) predate the CoCo 3 extras,
the SN76489 and the overlay work, and are superseded by it.

If emulation is still most of the frame (roughly 80%, as in the older numbers), a
genuine doubling adds ~10 ms and blows the 16.67 ms budget on emulation alone. When
that happens the pacing loop in `loop()` (`src/main.cpp`) takes its resync branch
(`next_us = micros()`, dropping the deficit rather than catching up), and:

- the guest ends up running **slower than the intended 2×** (frames stretch past
  real-time), and
- the audio producer now delivers 800 samples per *stretched* frame while core 1 still
  drains 800 per real 16.67 ms → the ring **under-produces, drains its ~170 ms buffer,
  and underruns** → audio dropouts after ~1 s of sustained overrun.

Video would stay a clean 60 Hz throughout (decoupled core 1), just showing a machine
running behind.

### Rule of thumb

On those assumptions the break-even is roughly **~1.4×** real-time emulation load
(down from the ~1.6–1.7× estimated at 31% slack):

- **Address-dependent high speed** (`POKE 65495`, ~1.3–1.5×): marginal.
- **Full high speed** (`POKE 65497`, ~2×): **would not hold up**. Core 0 could not
  emulate it in real time, so pacing and audio would both suffer.

## Status

`PIZERO-48` (done) was the write-up of this analysis. It is a **code-level analysis,
not measured on hardware**, and the on-device measurement is **not tracked by any open
issue**. Measuring only makes sense once fast mode is honored (see above).

The method, if it is picked up: use the per-second `[run] fps/cpu/blit` serial
telemetry (`PIZERO-15`), run a tight high-speed-POKE loop on the guest, and watch
whether core-0 cpu-ms crosses 16.67 ms and whether the audio breaks up. If full speed
proves unusable, an option (as some ports do) is to **cap or ignore the SAM full-speed
bit** while still honoring the address-dependent mode.

## See also

- [`pipeline.md`](pipeline.md): the decoupled two-core pipeline this relies on
- [`video-audio-notes.md`](video-audio-notes.md): audio ring, metering, the (unrelated)
  startup fast-forward bug
- [`../README.md`](../README.md): Performance section

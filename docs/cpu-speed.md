# Emulation pacing & CoCo high-speed (double-speed) behavior

Does running the emulated CoCo at "double speed" hold up on this hardware? Short
answer: **functionally yes (correct by design), but not performantly at the top end**
— the limiter is core-0 CPU headroom, not the audio/video pipeline. This doc explains
why.

## There is no host fast-forward

This port has **no host-side fast-forward / turbo toggle** — no key combo, no
[`AUTORUN.md`](../AUTORUN.md) directive, no flag. The only "double speed" available is
the **CoCo's own SAM high-speed POKE**, driven by the guest program itself:

- `POKE 65495,0` — *address-dependent* high speed (SAM `R = 1`): only accesses outside
  the display/IO region run fast. Effective ~1.3–1.5×. The "safe" mode real CoCo 1/2
  hardware can mostly survive.
- `POKE 65497,0` — *full* high speed (SAM `R = 2`): every cycle runs fast, ~2×. On real
  hardware this corrupts the video; here video is unaffected (see below) but the host
  can't keep up (see "Where it breaks").

This is fully implemented in the vendored XRoar SAM
(`lib/xroar_core/src/mc6883.c`): a fast memory cycle costs **8 event-ticks** versus
**16** for a normal cycle. It is *latent* — correct, but only ever exercised if a guest
program writes the SAM `R` bits.

## The pacing model — fixed emulated *time* per frame

This is the key, non-obvious fact. The main loop does **not** run a fixed number of CPU
instructions per frame — it runs a fixed amount of **emulated time**, measured in event
ticks. In `lib/coco_machine/src/coco_machine.cpp`:

```c
// run_cpu_with_audio(), :793
g_m.cycles_remaining = (int32_t)cycles * 16;   // budget in event TICKS, not instructions
```

and each memory access subtracts its own tick cost (`:486`, `ncycles` = 16 slow /
8 fast / 15 transition). So `coco_machine_run_cycles(CYCLES_PER_FRAME)` always advances:

```
CYCLES_PER_FRAME × 16  =  14915 × 16  =  238,640 ticks  ≈  16.667 ms of emulated time
```

per frame, **regardless of SAM speed**. In slow mode that's ~14,915 6809 cycles; in full
fast mode it's up to ~29,830 cycles executed within the *same* emulated 16.667 ms window
— exactly how real hardware behaves when you flip the SAM speed bit.

`CYCLES_PER_FRAME` / `FRAME_PERIOD_US` are defined per build in `src/main.cpp` (14915 /
16667 µs for the default 60 Hz build).

## Why audio and video are correct by design

Because the per-frame emulated-time budget is constant, two things people worry about
are **non-issues here**:

- **Audio pitch.** Samples are generated event-tick-keyed (`EVENT_TICK_HZ = 14318180`,
  `coco_machine.cpp:677`), and ticks-per-frame is constant → still **exactly 800
  samples/frame** in fast mode → no pitch shift and no resampler distortion. (The
  "fast-forward distortion" noted in [`hdmi-audio-notes.md`](hdmi-audio-notes.md) was a
  *separate* startup bug — a producer burst overflowing a too-small ring, fixed by the
  8192-deep primed ring — **not** the SAM speed.)
- **Video.** Core 1's `libdvi` worker is fully decoupled and keeps emitting 60 Hz of
  whatever `g_front` holds (see [`pipeline.md`](pipeline.md)). Guest speed never touches
  it.

## Where it actually breaks — core-0 headroom

Full fast mode executes ~2× as many 6809 memory cycles inside the same per-frame budget,
so the **host cost of emulation roughly doubles**. Against the measured 1× budget
(`README.md` Performance table):

| Work               | 1× speed            | sustained 2× (SAM full, estimate) |
|--------------------|---------------------|-----------------------------------|
| CoCo emulation     | ~9.4 ms             | **~18.8 ms**                      |
| render + blit      | ~2.1 ms             | ~2.1 ms                           |
| **Total / 16.67 ms** | ~11.5 ms (31% slack) | **~21 ms — over budget**          |

There is only ~5.3 ms of slack at 1×, so a genuine doubling blows the 16.67 ms frame
budget on emulation alone. When that happens the pacing loop takes its resync branch
(`next_us = micros()`, dropping the deficit rather than catching up), and:

- the guest ends up running **slower than the intended 2×** (frames stretch past
  real-time), and
- the audio producer now delivers 800 samples per *stretched* frame while core 1 still
  drains 800 per real 16.67 ms → the ring **under-produces, drains its ~170 ms buffer,
  and underruns** → audio dropouts after ~1 s of sustained overrun.

Video stays a clean 60 Hz throughout (decoupled core 1) — it just shows a machine
running behind.

### Rule of thumb

The break-even is roughly **~1.6–1.7×** real-time emulation load:

- **Address-dependent high speed** (`POKE 65495`, ~1.3–1.5×) — likely squeaks in under
  budget. Probably holds up.
- **Full high speed** (`POKE 65497`, ~2×) — **does not hold up**: core 0 can't emulate
  it in real time, so pacing and audio both suffer.

## Status & how to characterize it on-device

This is a **code-level analysis, not yet measured on hardware.** The numbers above are
estimates from the 1× performance budget.

To get certainty, use the per-second `[run] fps/cpu/blit` serial telemetry (`PIZERO-15`):
run a tight high-speed-POKE loop on the guest and watch whether core-0 cpu-ms crosses
16.67 ms and whether the audio breaks up. If full-speed proves unusable, an option (as
some ports do) is to **cap or ignore the SAM full-speed bit** while still honoring the
address-dependent mode. Tracked in `PIZERO-48`.

## See also

- [`pipeline.md`](pipeline.md) — the decoupled two-core pipeline this relies on
- [`hdmi-audio-notes.md`](hdmi-audio-notes.md) — audio ring, metering, the (unrelated)
  startup fast-forward bug
- [`../README.md`](../README.md) — Performance section (the 1× frame budget)

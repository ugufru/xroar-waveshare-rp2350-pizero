# CoCo 3 on the RP2350-PiZero: what fits and what does not

**PIZERO-111.** CoCo 3 support is a stated goal. This is the arithmetic behind
the answer, done against measured numbers rather than instinct, so the decision
is about scope rather than hope.

_Measured 2026-09-17 on the post-PIZERO-119 build. Sources: the build's own RAM
report, `docs/measurements/2026-09.jsonl`, and `docs/cpu-speed.md`._

## The short answer

| Question | Answer |
|---|---|
| 512 KB CoCo 3 | **No.** Not on this board, at any speed. |
| 128 KB CoCo 3, 320-wide modes | Possible on RAM and video, **blocked on CPU speed**. |
| 128 KB CoCo 3, 640-wide modes | Possible on RAM, needs a different scanout, **blocked on CPU speed**. |
| The wall | **The 6809, not the video.** This is the opposite of what we assumed. |

The ticket expected the 640-wide 16-colour modes to be the wall and the
320-wide ones to be reachable. Video turns out not to be the binding
constraint at all.

## RAM

The board has 520 KB of SRAM and **no PSRAM**: the pad exists on the PCB but is
unpopulated (`README.md`), so every byte is on-chip.

| Item | Bytes |
|---|---|
| Firmware static (measured, 70.9%) | 371,660 |
| of which the scanout framebuffer, 320x240 RGB565 | 153,600 |
| of which the packed VDG buffer, 192 x 128 | 24,576 |
| Guest RAM today, allocated at runtime | 65,536 |
| **Free** | **~87,000** |

- **128 KB CoCo 3** needs +64 KB over today. It fits, with roughly 23 KB to
  spare. Tight but real.
- **512 KB CoCo 3** needs +448 KB. There is nowhere to take it from: even
  deleting the framebuffer entirely (see per-line scanout below) frees only
  ~150 KB, leaving us ~300 KB short. Populating the PSRAM pad is the only route,
  and that is a hardware change to a board we are about to ship to other people.

## Video, mode by mode

Guest framebuffer sizes are the GIME's own. Conversion cost is our per-pixel
rate into the scanout format, measured on this build: the SG4 path runs at
9.7 ns/pixel and the blit at 20.6 ns/pixel, so the range below is
optimistic-to-pessimistic for a similar LUT-driven path.

| GIME mode | Guest bytes | Pixels/frame | Convert (9.7-20.6 ns/px) | Fits our scanout? |
|---|---|---|---|---|
| 160x192x16 | 15,360 | 30,720 | 0.3-0.6 ms | Yes, 1:1 into 320 wide with doubling |
| 320x192x4 | 15,360 | 61,440 | 0.6-1.3 ms | Yes, 1:1 |
| 320x192x16 | 30,720 | 61,440 | 0.6-1.3 ms | Yes, 1:1 |
| 640x192x2 | 15,360 | 122,880 | 1.2-2.5 ms | No, needs per-line scanout |
| 640x192x4 | 30,720 | 122,880 | 1.2-2.5 ms | No, needs per-line scanout |
| 640x192x16 | 61,440 | 122,880 | 1.2-2.5 ms | No, needs per-line scanout |
| 640x225x16 | 72,000 | 144,000 | 1.4-3.0 ms | No, needs per-line scanout |
| 80x25 text + attributes | 4,000 | ~122,880 | 1.2-2.5 ms | No, needs per-line scanout |

Two structural notes:

- **320-wide modes drop straight in.** Our framebuffer is 320 wide and libdvi
  doubles it to 640, which is exactly what the GIME's 320-wide modes want.
- **640-wide modes cannot use a framebuffer.** A 640x240 RGB565 buffer is
  307,200 bytes, more than twice what is free. The route is per-scanline
  generation into libdvi's scanline queue (`dvi_scanbuf_main_16bpp`), which
  needs two line buffers of 1,280 bytes instead. That is a real change to the
  scanout architecture, but it is libdvi's own supported path, and it would also
  free the 153,600-byte framebuffer, which is where the RAM for a 128 KB guest
  would come from comfortably.
- **Vertically**, 192 lines doubled is 384 of 480, so those modes letterbox.
  225 lines doubled is 450, which nearly fills the screen.

## The CPU, which is the actual wall

A CoCo 3 runs at **1.79 MHz**, double the CoCo 2's 0.895 MHz. Emulation cost
scales with emulated cycles, and we have measured both points:

| | emulated clock | emulation cost per frame |
|---|---|---|
| Today, measured | 0.895 MHz | **10.4 ms** of a 16.67 ms frame |
| CoCo 3 at native speed | 1.79 MHz | **~20.8 ms**, i.e. 125% of the frame |

`docs/cpu-speed.md` reached the same conclusion from the other direction: a
guest that flips the SAM into full high speed already runs ~18.8 ms of emulation
per frame and goes over budget. CoCo 3 is not a special case of that problem, it
**is** that problem, made permanent.

So even with RAM and video solved, a CoCo 3 would run at roughly **80% of real
speed**, before adding anything the GIME itself costs: MMU translation on every
memory access, the GIME timer, and its interrupt controller.

To close a 25% overrun the 6809 core needs to get about 1.6x faster once GIME
overhead is included. That is a plausible target for a dispatch rewrite on a
core we currently treat as trusted and untouched, but it is a project, not a
tuning pass, and it is the first thing that would have to succeed.

## Recommendation

**CoCo 3 stays an aspiration, and the next step is a CPU experiment, not a
feature branch.**

If it is ever picked up, the order is forced by the arithmetic:

1. **Make the 6809 core faster**, and prove it with the soak harness at an
   emulated 1.79 MHz before writing any GIME code. If that fails, everything
   after it is wasted.
2. **Move to per-line scanout**, which frees 153,600 bytes and is what makes
   both a 128 KB guest and the 640-wide modes possible at all.
3. **Then** GIME: MMU, palette, timer, interrupts, and the modes in the table
   above, 320-wide first because they need no new scanout.

Scope it as **128 KB only** and say so out loud. 512 KB is not a stretch goal on
this board, it is a different board.

## What would change this

- **Populating the PSRAM pad.** It would unlock 512 KB and remove the RAM
  argument entirely, at the cost of a hardware change and PSRAM bandwidth that
  would need its own measurement.
- **A faster 6809 core**, as above, which is the single lever that matters.
- **Accepting sub-real-time.** An 80%-speed CoCo 3 is a legitimate product
  decision for some uses and a bad one for games and music, which are most of
  what this machine is for.

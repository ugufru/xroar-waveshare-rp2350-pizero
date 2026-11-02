# CoCo 3 retrospective

A record of how the CoCo Zero went from "a CoCo 3 is out of reach" to a
128 KB CoCo 3 in the product firmware, beside the CoCo 2: what worked, what
cost time, and what we now do differently because of it. It covers PIZERO-111
through PIZERO-241, 2026-09-17 to 2026-10-20, and was brought up to date on
2026-11-01, when the CoCo 3 became the default machine (PIZERO-242, 261 to
263, 280, 287, 314 to 316).

How to use the CoCo 3 is in [README.md](../README.md) and
[SETTINGS.md](../SETTINGS.md); the plan and the numbers behind it are in
[coco3-plan.md](coco3-plan.md) and [coco3-feasibility.md](coco3-feasibility.md);
what real software does is in [coco3-titles.md](coco3-titles.md). This file is
about the process and the failures, so the same mistakes are not made twice.

## Where it stands

| Area | State |
|---|---|
| Machine | A 128 KB CoCo 3, Super Extended Color BASIC, the vendored XRoar GIME as the bus. Done, PIZERO-133. |
| Speed | 60 fps at 0.89 and 1.79 MHz. A full-screen HCLS loop at 1.79 MHz uses about 13.7 of 16.7 ms a frame, no late frames. PIZERO-131, 213, 218, 220. |
| Picture | All 640 GIME pixels a line, one byte each, looked up in a palette by the RP2350's hardware TMDS encoder. 80-column text reads properly. PIZERO-134. |
| Checked on the board | WIDTH 40 and 80, ATTR, PALETTE, HSCREEN 1 and 2, the GIME timer, sound at both speeds. PIZERO-214, 134. |
| Input | CTRL, ALT, F1, F2 from the USB keyboard; both buttons of each joystick on the gamepad. PIZERO-215. |
| Software | Space Raid, Starcast and Galactus play. Three 512 KB titles do not, as expected. NitrOS-9 Level 2 boots from a floppy the script builds. PIZERO-228, 236. |
| One firmware | The product build is four machines: `cocozero` (the default since PIZERO-280: a 128 KB CoCo 3 with the sound chips, the MIDI Pak and auto-repeat), `coco2+` (a CoCo 2 with the extras), and the stock `coco2` and `coco3`. PIZERO-135, 242, 280. |
| Switching | In place, without restarting the board: about 80 ms to a CoCo 2 and 87 ms to a CoCo 3, from a settings change or a game's own file. Only a game autorun starts at power-on still restarts. PIZERO-314. |
| Boot | Reset to the BASIC `OK` in about 1.25 s with no computer attached; 450 ms of it is Super Extended and Disk BASIC starting. PIZERO-315, 316. |
| Memory | About 8.3 KB of heap free with the CoCo 3 running and Disk BASIC in. Every change that adds RAM is checked by booting as a CoCo 3 (CLAUDE.md, since PIZERO-262). |
| Extras on the CoCo 3 | The SN76489, the AY cartridges, the Orchestra-90 and the MIDI Pak are all on its bus (`coco3_cpu_cycle`). The MIDI Pak's echo test has run on both machines and the AY's cost was measured on it (PIZERO-248, 287); the others have not been checked by ear on a CoCo 3 yet. |
| Not possible | 512 KB. The board's PSRAM is not fitted and SRAM is full. |
| Not yet tried | HSCREEN 3 and 4, the 640-wide graphics modes, on the board. |

Host tests: 338 (2026-11-01), including the 6809 lockstep harness (`test_mc6809_lockstep`) and
the CoCo 3 boot, typing and port-renderer checks (`test_coco3_boot`).

## Timeline

| Date | Issue | What happened |
|---|---|---|
| 09-17 | PIZERO-111 | Feasibility study: a 128 KB CoCo 3 fits in RAM; the wall is the emulated 6809, which would need 125% of a frame at 1.79 MHz. Closed as an aspiration; the GIME palette and timer were added to the CoCo 2 instead (PIZERO-55, 62, 85). |
| 09-26 | PIZERO-129 to 135 | The plan: prove a faster core first, then measure, then build. |
| 10-02 | PIZERO-175 | Video benchmark. Found the RP2350's own TMDS encoder in the SIO block: a 640-wide line in about 16 to 24 µs against libdvi's 56 to 58. Encode time stopped being the limit for 640-wide; RAM was left. |
| 10-09 | PIZERO-129 | Lockstep harness: a frozen reference 6809 and the live one run instruction by instruction and must agree. It caught two injected faults (a missing H flag, a missing NOP cycle). |
| 10-12 | PIZERO-130, 209 | Measured: 185.4 host clocks per emulated cycle, 131.7% of a frame at 1.79 MHz; about 150 of the 185 were the bus path, not the core. |
| 10-12 to 13 | PIZERO-131 | Five steps took it to about 101 clocks a cycle: 1.79 MHz fits a frame, each step checked in lockstep. |
| 10-13 | PIZERO-133 | The GIME vendored; a 128 KB CoCo 3 boots to OK on the host, then on the board at 29 fps. |
| 10-13 | PIZERO-213, 218 | An MMU fast path (58 fps), then a port-side GIME renderer that skips unchanged lines: 60 fps at both speeds. |
| 10-13 to 15 | PIZERO-219, 220 | The worst case measured (HCLS at 1.79 MHz, 3 to 5 late frames a second) and fixed: none late. A sound worry turned out to be one of the user's monitors. |
| 10-15 to 19 | PIZERO-214 | The feature checks pass on the host, then in the CoCo Zero emulator, then on the board, once the serial typist sent one key at a time. |
| 10-19 | PIZERO-228 | Seven real titles recorded. Every 128 KB game worked; every failure was a 512 KB game. 80-column screens were unreadable. |
| 10-20 | PIZERO-134 | The 640-wide picture: one byte a pixel, palette scanout on the hardware encoder, the same RAM as before. The six check screens matched the old build pixel for pixel. |
| 10-20 | PIZERO-236 | `mkos9disk.py --level 2`: NitrOS-9 Level 2 boots to its shell. |
| 10-20 | PIZERO-215 | CTRL, ALT, F1, F2 and the second fire buttons. A gamepad's stray R2 bit found and fixed on the way. |
| 10-20 | PIZERO-135 | One firmware, either machine, with per-game switching. Three restart mechanisms failed before the fourth worked. |
| 10-20 | PIZERO-239, 241 | Documentation sweep; this retrospective. |
| 10-20 | PIZERO-242 | Machine profiles: `cocozero` (then a CoCo 2 with extras), stock `coco2` and `coco3`. |
| 10-23 | PIZERO-261 | A game set to the CoCo 3 ran on the CoCo 2 with no reason given: `coco3.rom` had gone from the card. The launch now refuses and says which ROM is missing. |
| 10-23 | PIZERO-262 | The CoCo 3 stopped starting: a day of RAM growth elsewhere left too little heap for its second 64 KB. |
| 10-23 | PIZERO-263 | A black screen at the prompt: a `coco3.rom` with 801 bytes changed, the right size. |
| 10-26 | PIZERO-280 | The CoCo Zero standard says a CoCo Zero is a CoCo 3; filed. |
| 11-01 | PIZERO-287 | The MIDI Pak, tested on both machines. |
| 11-01 | PIZERO-280, 314 | `cocozero` becomes the CoCo 3 with extras, `coco2+` keeps the CoCo 2 with them, and switching machine no longer restarts the board. |
| 11-01 | PIZERO-315, 316 | Boot timed phase by phase; the 4 s wait for a serial monitor moved behind `debug`. |

## What worked

**Proving correctness before chasing speed.** The lockstep harness came first
(PIZERO-129). Every step of the 1.7x speed-up (PIZERO-131) and the later fast
paths ran against it, so "faster" never quietly became "wrong". No CPU
regression reached the board.

**Measuring before optimising.** The cycle-counter profile (PIZERO-209) showed
the bus path took about 150 of the 185 host clocks a cycle and the 6809 core
only 35. The work went where the time was. The same habit gave PIZERO-219 its
`slack` and `late` telemetry before PIZERO-220 tried to fix anything.

**A separate build until it was proven.** The CoCo 3 lived in `pizero_coco3`,
and the 640-wide picture in a test env, `pizero_coco3_640`, until the board
confirmed each. The product build was never the experiment.

**Comparing the new path against the old, pixel by pixel.** When the picture
moved to 640 wide, the six PIZERO-214 check screens were captured again and
compared with the 320-wide originals: identical at every other pixel, apart
from one timer reading. That is a stronger check than "it looks right".

**Reading the disk before blaming the firmware.** Montezuma's Revenge failed
with a nonsense line number, and Sinistar hung loading. In each case the
loader on the disk was read first: both map memory pages that exist only on a
512 KB machine. The NitrOS-9 Level 2 image's driver was read too: it was for
CoCo SDC hardware. Three apparent firmware bugs were settled in minutes, with
evidence, instead of a debugging session.

**Building on what the repo already had.** The machine core had picked the CoCo
3 at run time from the ROM size since PIZERO-133, so PIZERO-135 was mostly
wiring, not a rewrite. The Level 2 floppy extended the Level 1 script
(PIZERO-199). The keyboard and pad changes slotted into the PIZERO-163 and 164
designs.

**Asking the user when a choice was theirs.** F1 and F2 (give them to the CoCo
3, move INFO into the overlay) and how to choose the machine (per-game
switching) were put as one recommendation and a question, not a menu.

## What went wrong, and the lessons

### 1. A door closed by a representation, not a limit (PIZERO-111, 179)

PIZERO-111 concluded that 640-wide modes could not use a framebuffer: a
640x240 RGB565 buffer is 300 KB. True, and it pushed the work toward
per-line generation, a large change. PIZERO-175 then found the hardware
encoder, and PIZERO-134 simply stored one byte a pixel, a colour number,
and looked the colour up at scanout: the same 153,600 bytes as the 320-wide
buffer. The limit was the format, not the size.

**Lesson:** when a buffer "does not fit", ask what each element needs to hold
before redesigning around it.

### 2. Typing tests blind (PIZERO-214, 215)

Most of the round trips with the user came from typing BASIC into the board
over serial without seeing the screen:

- whole lines sent in one burst lost keys, so programs failed with `?SN ERROR`;
- a test typed while autorun was still loading went to the jukebox;
- a test typed after the user had reset the board built a program of DATA lines only;
- a keyboard test read the matrix between statements, and BASIC's own BREAK
  check rescans it there, so the readings were noise;
- a machine-code test stored its results on top of its own last instruction,
  and crashed BASIC.

Two of these were bugs in tests written here. The host harness
(`test_coco3_boot`, `COCO3_TYPE`) reproduced the crash on the Mac in seconds,
once it was tried.

**Lesson:** run a test program in the host harness before typing it into the
board, and confirm the machine is at the prompt before typing anything.
PIZERO-238 (read the text screen back over serial) would remove the blindness.

### 3. Overwriting the user's disk (PIZERO-236)

The Level 2 floppy was copied to the card as `NOS9L2.DSK`. The card and macOS
ignore case, so it replaced the user's 30 MB `nos9l2.dsk`. A copy was on the
Mac, so nothing was lost, but nothing checked before writing.

**Lesson:** before copying onto the user's card, list the target folder and
compare names case-insensitively. Give generated files names that cannot clash.

### 4. A fault that waited for a job (PIZERO-215)

The GameSir sets its digital R2 bit at rest, with the trigger reading 3 of 255.
For months that did nothing, because R2 did nothing. When R2 became the CoCo
3's second fire button by default, it held a keyboard row down, stalled
autorun after `LOAD`, and likely spoiled some of the typed tests in item 2.

**Lesson:** before giving an input a new default meaning, look at its raw
value at rest. The `[pad]` telemetry now prints the raw bytes for that reason.

### 5. Three wrong guesses about a restart, all answered in the repo (PIZERO-135)

Switching machine needs a restart that carries a message. It took four tries:

1. **A record in uninitialised RAM:** lost. `wd_post.cpp` already said why:
   "RAM does not do: the boot reuses the start of SRAM."
2. **A core reset (SYSRESETREQ) to keep RAM:** the display's DMA and PIO kept
   running and the next boot stalled starting them, and the still-running
   watchdog restarted the board a second time.
3. **A tagged watchdog reboot:** the tag in `scratch[4]` vanished.
   `docs/BUILD.md` already said why: the SDK's `watchdog_reboot` zeroes it, a
   finding from PIZERO-51 that had never been fixed.
4. **A file on the card, and the tag written after the call:** works.

Each try cost the user a test on the board. Two of the three failures were
written down in the project before the first line of code.

**Lesson:** before designing around a hardware behaviour, search the repo's own
notes and comments for it. Fix a known trap when it is found, not when it bites
again. The PIZERO-51 replug tag is now fixed too.

### 6. A setting left behind by a new path (PIZERO-240)

The 640-wide picture always uses the hardware encoder, so `video_encoder =
software` stopped doing anything. Nothing said so until the documentation
sweep found it.

**Lesson:** when a change replaces a path, list the settings and flags that
steered the old one, and decide each one's fate in the same change.

### 7. Claims ahead of checks

The documentation sweep first said every GIME mode shows whole. HSCREEN 3 and
4 had never been run on the board. It was corrected before it shipped.

**Lesson:** a feature list says what was checked, and says "not yet tried"
about the rest.

### 8. RAM added elsewhere stopped the CoCo 3 (PIZERO-262)

The CoCo 3 takes its second 64 KB from the heap. A day of unrelated work
(a bigger stereo ring, USB audio, HID maps) grew static RAM to 85.7%, and the
CoCo 3 no longer started: "EMULATOR DID NOT START". Nothing had checked the
CoCo 3 after any of those changes, because none of them was about the CoCo 3.

**Lesson:** a budget shared by two machines needs a check on the tighter one
after every change. Booting as a CoCo 3 and reading `[mem] free heap` is now
a rule before committing RAM.

### 9. A size check passed a damaged ROM (PIZERO-263)

`coco3.rom` was the right 32,768 bytes, with 801 bytes changed. The machine
drew a black screen, and it took a session to get from "the video is
broken" to comparing the file with a good copy.

**Lesson:** check what a file is, not just how big it is. A hash check on
the ROMs is filed (PIZERO-264).

### 10. A restart that was not needed (PIZERO-135, 314)

PIZERO-135 switched machine by restarting the board, because the two lay
out RAM differently and nothing could take a machine down. The plan for
switching in place (PIZERO-314) set aside a fixed 64 KB arena, in case the
heap came back fragmented. Measured first, it came back whole every time
(77,872 bytes with no machine, ten round trips), so the arena was never
built, and the switch takes 80 ms instead of 2 s and keeps USB up.

**Lesson:** revisit a workaround when its reason is looked at again, and
measure the feared problem before building around it.

## Still open

- **PIZERO-216:** LOADM under a non-default MMU map.
- **PIZERO-217:** tearing in full-screen animation.
- **PIZERO-170:** which GIME revision's timer quirk to emulate.
- **PIZERO-178:** render cost per mode, mostly answered by PIZERO-218 to 220.
- **PIZERO-211:** RAM back from the fast 6809 core.
- **PIZERO-237:** 640x480 screenshots take about a second; no host test of the 640-wide path yet.
- **PIZERO-238:** read the text screen back over serial.
- **PIZERO-240:** retire or restore `video_encoder`.
- **PIZERO-264, 265:** hash-check the ROMs; "Press R to retry" on the boot pages.
- **PIZERO-314's remainder:** a game autorun starts at power-on that wants the
  other machine still restarts the board.
- **RAM:** PIZERO-268 (a 4-bit framebuffer), 270 (ROMs from flash) and 272
  (225 lines) would each give the CoCo 3 headroom.
- **HSCREEN 3 and 4** on the board.
- **The user's `nos9l2.dsk`** to be restored on the card from the Mac's copy, and
  the Level 2 floppy renamed `L2BOOT.DSK`.
- **512 KB:** not possible on this board without a PSRAM chip.

## Working rules this produced

- **Prove it in lockstep, then make it fast.** Any change to the 6809 core
  or its bus runs against the frozen reference first.
- **Measure, then fix, then measure again,** and keep the telemetry that
  measured it.
- **New paths stay in their own env** until the board confirms them, then fold
  into the product.
- **Compare old and new output directly** (screenshots, pixel by pixel) when a
  path is replaced.
- **Read the software before blaming the firmware:** a failing title's loader
  and drivers first.
- **Test programs run in the host harness first,** and the board's state is
  confirmed before typing into it.
- **Check names before writing to the user's card,** case-insensitively.
- **Look at an input's idle value before giving it a default job.**
- **Search the repo's own notes before designing around hardware,** and fix
  known traps when found.
- **When a path is replaced, settle every setting that steered the old one.**
- **Say what was checked.** "Not yet tried" is a fine thing to write.
- **Boot as a CoCo 3 after any change that adds RAM,** and read the heap.
- **Check a ROM's content, not just its size.**
- **Measure the problem a workaround guards against** before building it.

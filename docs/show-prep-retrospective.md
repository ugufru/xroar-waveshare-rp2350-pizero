# Show-prep retrospective

A record of the two days before Tandy Assembly (2026-10-01 and 10-02): the
video encoder found and shipped, the editor and autorun reworked, the drives
remembered across power-off, the docs cut down, and a batch of microSD cards
that would not work. It covers PIZERO-42, 174 to 186. Like
[usb-retrospective.md](usb-retrospective.md), it is about the process and
the mistakes, so they are not made twice. How things work is in
[README.md](../README.md), [SETTINGS.md](../SETTINGS.md) and
[AUTORUN.md](../AUTORUN.md).

## Where it stands

| Area | State |
|---|---|
| Video encode | The RP2350's own TMDS encoder in the SIO block, which libdvi predates, encodes the picture: 12.6 µs a line against 25.2 µs in software. `video_encoder = software` switches back. Done, PIZERO-175. |
| 640-wide picture | Affordable now: about 16 µs a line in color on the hardware encoder. RAM is the limit, not encode time. Open, PIZERO-179. |
| Editor | Settings files open in lower case, sorted by name, comments first; `autorun.txt` opens in upper case like the BASIC prompt. Caps Lock and Shift work. Done, PIZERO-180, 181. |
| Autorun | What the machine does after it powers up, so a demo starts itself and is easy to reset. `autorun = on | off`; `reset_button = basic | autorun`; a Space or BREAK tap before the typing starts cancels it. No pages. Done, PIZERO-182; its scope needs refining, PIZERO-188. |
| Drives | All four remembered in `/coco/drives.txt` across power-off. `@DISK` removed. Done, PIZERO-183. |
| F8 | Cycles the artifact colors and saves the choice for the running game. Done, PIZERO-186. |
| Docs | AUTORUN.md from 354 lines to about 70. No HDMI name and no em dashes left in our code and notes. Done, PIZERO-174. |
| microSD | A no-name batch (Lerdisk 2 GB) does not work at all; kit.md now says what to buy. Done, PIZERO-184; investigation open, PIZERO-185. |

Host tests: 230. Main is pushed and the board runs what is on GitHub.

## Timeline

| Date | Issue | What happened |
|---|---|---|
| 10-01 | PIZERO-174 | 11 build flags renamed HDMI_* to AV_*. Every firmware built before and after with the version pinned: byte-identical. |
| 10-01 | PIZERO-42 | Five finished diagnostic builds retired, firmware byte-identical. |
| 10-01 | PIZERO-16, 175 to 177 | TMS9918A promoted; the GIME benchmark, the MC6845 card and a "classic" setting filed. |
| 10-02 | PIZERO-174 | Code comments, the `[hdmi]` log tag (now `[disp]`) and the engineering docs cleaned. |
| 10-02 | PIZERO-175 | Encoder bench: libdvi's 640-wide encoders take 55 to 58 µs of a 63.5 µs line. Scratch placement and no DC balance gain little. Then the SIO hardware encoder: 16 µs, decode check 0 errors. Shipped as the default behind `video_encoder`, confirmed on hardware. Lightweight tag `checkpoint/pre-sio-tmds` kept for a way back. |
| 10-02 | PIZERO-180, 181 | Editor: Caps Lock, upper case like BASIC; settings files open tidied. Confirmed. Later: settings files open in lower case. |
| 10-02 | PIZERO-182 | `run_skips_autorun` renamed, then reworked six times (below) before it matched what the user meant. |
| 10-02 | PIZERO-183 | Drives remembered; `@DISK` removed. Confirmed. |
| 10-02 | PIZERO-184, 185 | Three Lerdisk cards fail; diagnosed by elimination; kit.md advice written. |
| 10-02 | PIZERO-174 | About 120 em dashes left in code comments removed. Closed. |
| 10-02 | PIZERO-186 | F8 artifact colors, confirmed on hardware. |

## What worked

**Proving renames with identical firmware.** Pinning the version
(`FW_VERSION_OVERRIDE`, `FW_DATE_OVERRIDE`) made the builds reproducible, so
every rename and comment change was checked by comparing `firmware.bin`
hashes across all envs. A rename that changes the firmware is a bug; a
rename that does not is safe, and now provably so.

**Measuring before building.** The 640-wide question was answered with a
bench on the board, not an estimate. The bench is what turned up the SIO
hardware encoder, which halved core 1's encode time for the picture we
already had, before any 640-wide work was done.

**A fallback setting for a risky change.** The new encoder went in with
`video_encoder = software` one line away, so a display that dislikes it at
the show needs no reflash.

**Serial logs during the user's tests.** A background capture running while
the user pressed RUN or tapped keys answered questions the user could not:
which settings were in force, that a game's own file had turned autorun off,
when the keyboard's first report arrived, and the SD error code. Each one
replaced a round of guessing.

**Pure helpers with host tests.** Caps Lock (`text_edit_keys.h`), the
settings tidy and the one-line setter (`settings.h`) were written and tested
on the host before they reached the board, and each worked first time there.

**Diagnosis by elimination.** The SD failure was split in three steps: the
error code (the card never answers, so not formatting), the old card in the
same slot (the board is fine), and the card's ID on the Mac (a no-name card
claiming the 2005 spec). No firmware change was needed to reach the answer.

## What went wrong, and the lessons

### 1. Autorun, built six times (PIZERO-182)

The user asked what `run_skips_autorun` did. Over the evening it became
`reset_button = basic | power_on`, then `basic | autorun` with an `autorun`
setting, then Space held at boot, then GET_REPORT, then a tap with a prompt
page, then a tap with no page, and finally a rebuild when the user stated
the model in one paragraph: autorun is one boot-time feature, it runs drive
0's program on every boot unless it is off or Space was tapped, and the
reset button switches it off for that boot. Every step before that was a
patch to my own reading of a small piece of the request.

The user's statement of purpose came later still: autorun is for what a
machine does after it has powered up, so demos start themselves and are easy
to reset. It was not meant as general automation for every disk, though the
final build can be read that way (ENTER on a disk in F12 follows it too).
Refining that is PIZERO-188, deliberately not done the night before the
show.

**Lesson:** when a request touches behavior with several settings that
interact, start from the purpose: ask what the feature is for, then write
the whole rule set as a short table (each kind of boot, each setting, what
happens) and get agreement before writing code. One question answered up
front would have saved five builds.

### 2. Screens nobody asked for (PIZERO-182)

The skip feature grew an "AUTORUN SKIPPED" page, then a second one, then a
prompt page. The user: "i never asked for it", and "they are stupid". The
prompt page was also invisible in practice: a RUN restart restarts the video
signal, and the display took most of a second to lock back on.

**Lesson:** no new on-screen page, prompt or message unless asked for. Say
what the behavior is and let the user decide whether it needs a screen.

### 3. Closing too fast (PIZERO-180, 182)

PIZERO-182 was closed while it still did not match the user's model, and
PIZERO-180 was marked done and then changed. The user: "you are too quick
to close." A later commit was refused for the same reason.

**Lesson:** an issue closes only after the user has tried the final
behavior, in their words. A change to closed behavior reopens it. Commit
when asked or when the user has seen it work, not ahead of them.

### 4. A hold that could not be seen (PIZERO-182)

"Hold Space at boot" was built on the assumption that the keyboard reports a
key already down when it connects. The Keychron K2 does not, and answers a
GET_REPORT with an empty report. It took three hardware rounds to find out,
because the second and third attempts changed the code instead of logging
what the keyboard sent. One timing log ("report +45 ms after mount") settled
it.

**Lesson:** when a hardware assumption fails once, log what the device
actually does before trying a second fix.

### 5. Names that describe the mechanism (PIZERO-182)

`run_skips_autorun` told the user nothing: "i don't know what it does and
the name is not intuitive enough to guess". "RUN" also reads as the BASIC
command. `reset_button = basic | autorun` says what the button does.

**Lesson:** name a setting after the thing the user controls, and its
values after the outcomes they choose between.

### 6. A script that emptied a file (PIZERO-184)

An edit script opened `docs/kit.md` for writing before reading it, which
truncated it, then failed its own check. It was restored from git plus the
one pending change and verified by line count before carrying on.

**Lesson:** read the whole file first, then write; after any scripted edit,
check the file's size or diff before moving on.

### 7. A flash of the old build (PIZERO-182)

An edit script stopped on a failed match, but the next command in the same
line still flashed, so the board got the previous build while the message
said otherwise. It was caught from the output and redone.

**Lesson:** chain edit, build and flash with `&&` and a check that the
edit applied, so a failed edit stops the flash.

### 8. A guessed placeholder (PIZERO-180)

A code comment briefly cited "PIZERO-178..." before the issue existed. Small,
but an issue number in a comment must be one that exists.

**Lesson:** file the issue first, then cite it.

## Still open

| Issue | What |
|---|---|
| PIZERO-178 | GIME scanline render cost per CoCo 3 mode. |
| PIZERO-179 | A 640-wide picture on the hardware encoder; RAM is the constraint. |
| PIZERO-185 | Whether no-name SD 1.1 cards can be made to answer the SPI start-up. |
| PIZERO-176, 177, 16 | MC6845 card, a "classic" setting, TMS9918A. |
| PIZERO-188 | Refine autorun's scope around its purpose: power-up behavior for demos, not automation for every disk. Includes whether ENTER on a disk should follow it, and whether `autorun` belongs in a game's own settings file. |

## Working rules this produced

- Ask what a feature is for before designing it; for behavior with
  interacting settings, agree a rules table before code.
- No new screens, pages or prompts unless the user asks for them.
- Close an issue only after the user has tried the final behavior; reopen on
  any change to it.
- Name settings for what the user controls, values for the outcomes.
- After one failed hardware assumption, log what the device does before the
  next fix.
- Run a serial capture whenever the user tests on the board.
- Prove renames and comment-only changes with byte-identical firmware.
- Read a file before writing it; check size or diff after scripted edits;
  chain edits and flashes so a failed edit stops the flash.
- Ship a risky change with a setting that switches back.
- Test one card from every batch of microSD cards in a board.

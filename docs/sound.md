# Sound chips

What the CoCo Zero can play, where each sound source sits in the memory map, how
to turn it on and off, and how it is built. The signal path from the mixed sample
to the video cable is in [`pipeline.md`](pipeline.md); this page stops at the
mixed sample.

## At a glance

| Source | Voices | Addresses | Setting | `cocozero` | `coco2` | `coco3` |
|---|---|---|---|---|---|---|
| The CoCo's 6-bit DAC | 1 | `$FF20` bits 2-7 | always on | on | on | on |
| Single-bit output | 1 | `$FF22` bit 1 | always on | on | on | on |
| SN76489 (Games Master Cartridge) | 3 tones + noise | `$FF41` | `sn76489` | on | off* | off* |
| AY-3-8910 (CoCo PSG) | 3 tones + noise | `$FF5D`-`$FF5F` | `coco_psg` | on | off | off |
| 4 x AY-3-8910 (Symphony-12) | 12 tones + 4 noise | `$FF60`-`$FF63` | `symphony12` | on | off | off |
| AY-3-8913 (Deluxe CoCo) | 3 tones + noise | `$FF38`-`$FF39` | `deluxe_psg` | off | off | off |
| Orchestra-90CC | 2 DACs, left and right | `$FF7A`-`$FF7B` | `orchestra90` | on | off | off |

\* Loading a Games Master Cartridge image (a bank-switched `.ccc`, 32 KB to
256 KB) brings its SN76489 on any machine, whatever the setting says.

[`io-map.svg`](io-map.svg) draws the whole I/O page, shipped and planned
devices together, and shows which can sit beside Disk BASIC without a
Multi-Pak slot switch (open it in a browser and hover).

Everything can play at once: on the CoCo Zero that is the DAC, three SN76489
tones and its noise, twelve AY tones and the Orchestra-90's two DACs.
[`ay-checks/allvoice.bas`](ay-checks/allvoice.bas) does exactly that.

How they reach the speaker, together or picked by the CoCo's multiplexer, is
the `sound_sources` setting (below): `all` on the CoCo Zero, `switched`, as
the real machine, on the stock CoCo 2 and CoCo 3.

The mixed sound plays from the TV through the video cable, or from a USB
headphone DAC plugged in at power-on; the volume keys of a USB keyboard turn
it up and down (both below).

The settings are lines in `/coco/settings.txt`, or in a game's own file beside it
(`STARCAST.TXT` for `STARCAST.DSK`); see [`../SETTINGS.md`](../SETTINGS.md). The
machine profile sets the defaults above, and a line in either file overrides them.

## The CoCo's own sound

The 6-bit DAC is PIA1 port A bits 2-7 (`$FF20`): BASIC's `SOUND` and `PLAY` and
nearly every game use it. The level is picked up the moment the CPU writes the
port and averaged over each output sample, so edges land in the right place
and high tones do not alias.

## The sound multiplexer

![The real CoCo's sound multiplexer, and the CoCo Zero's way](images/sound-mux.svg)

### On the real CoCo

Only one sound source at a time reaches the TV. A multiplexer chip picks it,
and a sound enable turns its output on and off:

- **The select lines** are PIA0's two control lines, CA2 (`$FF01` bit 3) and
  CB2 (`$FF03` bit 3). Read as a number, CB2 then CA2, they pick source 0
  the DAC, 1 the cassette input, 2 the cartridge's sound line, or 3 nothing.
- **The sound enable** is PIA1 CB2 (`$FF23` bit 3). Off, the multiplexer's
  output is cut.
- **The single-bit output** is PIA1 port B bit 1 (`$FF22`). It counts only
  while its direction bit makes it an output. Then it adds to the line and
  changes how loud the selected source is; with the enable off it drives
  the line on its own.
- With nothing driving it, the line holds its last level, so turning the
  enable off is silent rather than a click.

The multiplexer chip has a second half that switches the same way, on the
same two select lines: it picks the joystick axis that the comparator
weighs against the DAC. So the select lines always choose a sound source and
a joystick axis together:

| CB2 | CA2 | Sound source | Joystick axis (`JOYSTK`) |
|---|---|---|---|
| 0 | 0 | the DAC | right X (0) |
| 0 | 1 | the cassette input | right Y (1) |
| 1 | 0 | the cartridge's sound line | left X (2) |
| 1 | 1 | nothing | left Y (3) |

That is why reading a joystick changes what is heard. A program reading the
right joystick's X axis with the sound on hears its own trial values (the
buzz in Double Back); reading the left X axis puts the cartridge's sound on
the line for a moment.

The levels, in millivolts, are XRoar's measurements of a real Dragon, whose
sound section is the CoCo's. Each source's level is its input times a gain,
plus an offset, and both depend on the single-bit output:

| Source | PB1 an input | PB1 an output, low | PB1 an output, high |
|---|---|---|---|
| DAC (0-63) | 4,500 x DAC/63 + 200 | 2,840 x DAC/63 + 180 | 3,400 x DAC/63 + 1,300 |
| Cassette | 2,050 | 1,600 | 2,350 |
| Cartridge line | its sound x 1.0 | its sound x 0.60 + 180 | its sound x 0.72 + 1,300 |
| Nothing | 0 | 0 | 0 |
| Enable off | holds | 0 | 3,900 |

The cassette input carries nothing here (no tape is played), so it gives only
its offset. Only changes are heard: the DC blocker after the mix takes out
any steady offset.

**What Color BASIC does with it**, checked in the ROMs:

- At start-up it leaves PB1 an input and the sound enable off.
- `SOUND` (`$A951`) and `PLAY` (`$9A2C` in Extended BASIC) select the DAC and
  turn the enable on before they play.
- `JOYSTK` turns the enable off (`$A974`) before it reads, so BASIC's own
  joystick reads are silent on a real CoCo.
- `CSAVE` never turns the enable on, so its tones go out to the cassette
  without reaching the TV.

**What a sound cartridge game has to do** is select the cartridge line and
turn the enable on, because otherwise its chip is not heard. A Games Master
or CoCo PSG game does this itself. From BASIC, keeping the other bits of each
control register as they are:

```basic
10 POKE &HFF01,PEEK(&HFF01) AND &HF7
20 POKE &HFF03,PEEK(&HFF03) OR 8
30 POKE &HFF23,PEEK(&HFF23) OR 8
```

Line 10 sets CA2 low, line 20 sets CB2 high (together, source 2), and line 30
turns the sound on. The next `SOUND` or `PLAY` selects the DAC again.

Under `sound_sources = switched` (the stock `coco2` and `coco3` profiles),
the board does all of this as the real machine does. These come through the
cartridge line, so the multiplexer decides whether they are heard:

- the SN76489;
- the CoCo PSG;
- the Symphony-12 in mono.

Two are never switched off by the multiplexer:

- the Symphony-12 in stereo, which plays from the card's own jacks (as if
  they were plugged into the speakers);
- the Orchestra-90, which also has its own stereo jacks;
- the Deluxe CoCo's chip, which is inside that machine.

### On the CoCo Zero

Under `sound_sources = all` (the `cocozero` default) every source is heard at
once:

- **Heard together:** the DAC, the single-bit output, and every sound
  cartridge, in mono or in stereo.
- **The select lines** still pick the joystick axis for the comparator, so
  joysticks read exactly as before, but they no longer choose the sound.
- **The sound enable** is ignored.
- **The single-bit output** still shapes the DAC as the table above shows.

This lets a program do what the real machine could not: play `SOUND` or
`PLAY` over Games Master or Symphony-12 music, or mix the DAC with a sound
cartridge, without choosing between them. A cartridge game that forgets to
select the cartridge line is heard anyway.

**What `all` gives up:** anything a program writes to the DAC while a real
CoCo would have the sound off is heard here:

- `CSAVE`'s tones;
- a game's joystick reads (`joystick_quiet`, below, takes those out again).

A game that relies on the multiplexer to keep something quiet can have the
real behaviour back with `sound_sources = switched` in its own file.

| `sound_sources` | What is heard | Default on |
|---|---|---|
| `switched` | As the real CoCo: the source the multiplexer picks, while the enable is on, and the single-bit output. | `coco2`, `coco3` |
| `all` | Everything at once, whatever the multiplexer and the enable say. | `cocozero` |

## Joystick reads: `joystick_quiet`

A program reads an analog joystick by writing trial values to the DAC and
testing the comparator (PIA0 PA7) after each, six or so per axis. With the
sound on, those trial values are heard as a buzz or a whine whenever the
joystick is read: Double Back does this, and so does a real CoCo running it.
Color BASIC's `JOYSTK` turns the sound off first, so on a real CoCo it is
quiet; under `sound_sources = all` the DAC is always heard, so it would not be.

With `joystick_quiet = on` (the default on `cocozero`), a DAC write that is
followed within 16 CPU cycles by a read of PIA0 port A is taken to be a
trial, and its sound is taken back from the moment of the write, as if it
had never been heard. The comparator still sees the value written, so the
joystick reads exactly as before. To make that possible the sound is held
back one sample (about 21 microseconds). Color BASIC's own `SOUND`, `PLAY`
and cassette routines never read PIA0 right after a DAC write, so they are
not touched; the serial log's `[snd] joystick_probes=N` counts the trials
taken out. A program that wrote the DAC and then read the keyboard within 16
cycles, every time, would lose that sound: if one turns up, give it
`joystick_quiet = off` in its own file.

`joystick_quiet = off` (the default on the stock `coco2` and `coco3`) leaves
every DAC write heard, as on the real machine.

## SN76489: the Games Master Cartridge

John Linville's Games Master Cartridge (GMC) carries a TI SN76489: three square
tone channels and a noise channel, 2 dB attenuation steps, at a 4 MHz clock.

- **Writing it.** One port, `$FF41`. A byte with bit 7 set picks a register and
  sets its low four bits: `1 c c t d d d d`, where `cc` is the channel (3 is the
  noise) and `t` is 1 for the volume. A following byte with bit 7 clear sets a
  tone's upper six bits. A write less than 32 chip clocks after the last is
  ignored, as on the real chip; BASIC is far too slow to hit that.
- **Pitch.** f = 125,000 / N for a 10-bit N (0 means 1024).
- **Volume.** An attenuation of 0 is loudest and 15 is off. The chip starts silent.
- **Noise.** Write `$E0` + mode to the noise register: bit 2 picks white (1) or
  periodic noise, and bits 0-1 the rate (0-2 fixed, 3 follows tone channel 3).
- **Where it lives.** `$FF41` is in the disk controller's latch area, but Disk
  BASIC and OS-9 write the latch only at `$FF40`. The odd mirrors go to the chip
  instead, so the chip and the disk controller work side by side. Set
  `sn76489 = off` to give those addresses back to the controller entirely.
- **Banked images.** A GMC game over 16 KB also switches 16 KB banks through even
  addresses in `$FF40`-`$FF5F`. That works on every machine, and so does its chip.

From BASIC, tone channel 0 at about middle C, at full volume:

```basic
10 N=478:POKE &HFF41,128+(N AND 15):POKE &HFF41,INT(N/16)
20 POKE &HFF41,144
```

## AY-3-8910: three cartridges, one bank of chips

General Instrument's AY-3-8910 (and Yamaha's YM2149) has three tone channels,
one noise generator, a 16-level volume per channel and a hardware envelope. The
board has one bank of four of them, shared by three cartridge front-ends. Each
front-end sets its chips' clock when a program uses it, so the pitch is right for
whichever card the program was written for.

| Cartridge | Chips | Ports | Clock |
|---|---|---|---|
| CoCo PSG | chip 1 | `$FF5E` register select, `$FF5F` data (read and write), `$FF5D` bit 0 picks the clock | 2 MHz (bit 0 = 0) or 1 MHz (bit 0 = 1) |
| Speech Systems Symphony-12 | chips 1-4 | a 6821 PIA at `$FF60`-`$FF63`: port A is the chips' data, port B two bits a chip | 894,886 Hz (14.31818 MHz / 16) |
| Tandy's Deluxe CoCo (unreleased, 1984) | chip 1 | `$FF38` data write, `$FF39` register latch on a write and data on a read | 1.79 MHz (14.31818 MHz / 8) |

- **Registers**, as the data sheet: R0-R5 tone periods (12 bits, f = clock / (16
  x TP)), R6 noise period, R7 the mixer (bits 0-2 tone off, bits 3-5 noise off,
  active high), R8-R10 the levels (bit 4 hands a channel to the envelope),
  R11-R12 the envelope period, R13 its shape (writing it restarts it). The I/O
  ports R14 and R15 read `$FF`.
- **A channel with both tone and noise off is steadily on** at its level, which
  is how AY sample playback works. This follows the data sheet. XRoar makes such
  a channel silent instead.
- **Symphony-12 port B.** Bits 1-0 drive chip 1 and bits 7-6 chip 4: `11`
  latches a register from port A, `10` writes port A to it, `01` reads it onto
  port A. Each bit counts only where port B's direction register makes it an
  output, as on the real PIA. The sequence the card's drivers use, register
  first, then value:

  ```basic
  100 POKE &HFF60,R:POKE &HFF62,3*M(K):POKE &HFF62,0
  110 POKE &HFF60,V:POKE &HFF62,2*M(K):POKE &HFF62,0:RETURN
  ```

  with `M(K)` = 1, 4, 16, 64 for chips 1-4, after making both ports outputs
  (`POKE &HFF61,0:POKE &HFF60,255:POKE &HFF61,4`, and the same for `$FF63` and
  `$FF62`).
- **Where they live.** On the CoCo 2, `$FF38` and `$FF39` repeat the keyboard and
  sound PIA, so turning `deluxe_psg` on there could break a program that uses the
  repeat. That is why it is off on every machine; nearly no software uses it. On
  the CoCo 3 those addresses are free. `$FF5D`-`$FF63` are free on both machines.
- **Shared chip.** All three front-ends use chip 1. A program that drives two
  cartridges at once shares that chip, as it would if the cards really shared it.
- Not emulated: the CoCo PSG's flash memory, RAM and joystick ports, and the AY
  I/O ports.

### Symphony-12 stereo

The real card has a jumper for mono or stereo. With `symphony12_stereo = on` (the
default, on every machine, since it is the card's jumper and not the machine's),
chips 1 and 2 play on the left and chips 3 and 4 on the right, as MAME routes
them. With it off all four are in the middle. A chip that the CoCo PSG or Deluxe
CoCo wrote last stays in the middle either way, so a CoCo PSG game is not stuck
on one side. The DAC and the SN76489 are always in the middle.

## Orchestra-90CC

Tandy's Orchestra-90CC (1985) is two 8-bit DACs and nothing else: write a
byte to `$FF7A` for the left channel and to `$FF7B` for the right, and that
level comes out until the next write. Its music program, which mixes several
voices in software and pushes the samples through the two DACs, is the
cartridge's ROM: load it as a `.ccc` with **F10**. Any program can use the
DACs directly, as [`ay-checks/orch90.bas`](ay-checks/orch90.bas) does.

- **Stereo, always.** The left DAC goes to the left speaker and the right to
  the right. Like XRoar, the card's sound does not pass the multiplexer: it
  has its own jacks, so `sound_sources` does not affect it.
- **Exact timing.** Software sample playback depends on each write landing
  at the right moment, so the two DACs go through the same integrate-and-average
  as the CoCo's own DAC: the audio integrator carries the left level with the
  line and the right as its difference (`audio_integ_run2`), and each output
  sample is the exact average of each DAC over its time.
- **Full scale** is the whole of the line, 4,700 mV, a little more than the
  CoCo's DAC, as XRoar has it. A DAC resting at any level is silent: only
  changes are heard.
- **Where it lives.** `$FF7A` and `$FF7B` are free on both machines (the CoCo 2
  decodes nothing at `$FF60`-`$FFBF`, and the CoCo 3's I/O page has no RAM), so
  `orchestra90` is on for the CoCo Zero. The stock CoCo 2 and CoCo 3 have it
  off; put `orchestra90 = on` in the cartridge's own file to play it there.
- The serial log shows `[orch] writes=N` once a program has written it.

Cost: a write is one more check on the bus's slow path. `orch90.bas`'s
machine-code loop, writing each DAC about 42,000 times a second, took a
frame to 8.9 ms (6.8 ms at rest), with no underruns.

## Where the sound comes out

The sound goes out on one of two outputs, never both at once:

| Output | When |
|---|---|
| The video cable, to the TV or monitor's speakers | The normal case: no USB headphone DAC at power-on, or `usb_audio = off` |
| A USB headphone DAC | One is plugged in at power-on and `usb_audio = on` (the default) |

While the DAC has the sound, the video cable carries silence. Unplug the DAC,
or set `usb_audio = off` in the settings editor, and the sound goes back to
the video cable.

### Using a USB headphone DAC

1. Plug the DAC into the hub, with the keyboard and gamepad, and plug the
   headphones into the DAC.
2. Switch on (or reboot). The board does not yet notice USB devices plugged in
   later (PIZERO-51), so the DAC has to be there at power-on.
3. Play something. The serial log shows `[uac] ... streaming` when the DAC has
   the sound.

What works: a class-compliant USB Audio Class 1 or 2 device with a playback
setting for 16-bit stereo at 48 kHz, which is what the CoCo Zero makes; the
board sends it 48 sample pairs every millisecond. Two have been tried, both
working:

| Device | Kind | Notes |
|---|---|---|
| A USB-C headphone dongle | UAC1, adaptive | Its descriptor: [`measurements/usb-audio-dongle-001F-0B21.txt`](measurements/usb-audio-dongle-001F-0B21.txt) |
| The headphone jack of a four-port USB-C hub | UAC2, synchronous | Its audio chip only appears while a plug is in the jack, so plug the headphones in before switching on. Its descriptor: [`measurements/usb-audio-hub-001F-0B21.txt`](measurements/usb-audio-hub-001F-0B21.txt) |

A DAC that only works at high speed cannot work: the board's USB runs at full
speed (12 Mbit/s).

**What it costs.** Feeding the DAC takes about 2.3 ms of each 16.7 ms frame on
the emulator's core, and more under heavy sound. Ordinary programs are
unaffected; `allvoice.bas`, with every voice at once, runs over the frame for
a few seconds at its loudest while a DAC has the sound.

## Turning it up and down

Three levels multiply together:

| Level | Range | Set by | Affects |
|---|---|---|---|
| `volume` | 0-15 (10 is the reference) | settings.txt, the settings editor, or the volume keys while playing | Both outputs |
| `usb_audio_db` | -60 to 0 dB (default -10) | settings.txt or the settings editor, put into effect when the DAC starts | Only the USB DAC |
| The TV or monitor's own volume | | its remote or buttons | Only the video cable |

**The volume keys.** A USB keyboard's Volume Up, Volume Down and Mute keys
(or a knob that sends them) work while you play:

- Volume Up and Down step `volume` by one, 0 to 15.
- Mute silences the sound; Mute again, or Up or Down, brings it back to the
  level it was at.
- The new level is written into `settings.txt` two seconds after the last
  press, so it is there next time; mute is not saved. The serial log shows
  `[settings] volume = N`.

Any USB device that sends the standard volume usages works, whatever the
layout of its reports: the board reads each device's report descriptor when it
is plugged in (`src/hid_consumer.h`). A USB headphone DAC's own buttons count
too, when they send them.

**Headset buttons.** The buttons on a headset's cable reach the board only
through the DAC, and only if the DAC can read them. With the headphones tried
so far:

- the dongle read every button as Play/Pause, which the board leaves alone, so
  the buttons do nothing;
- the hub's jack sent nothing for any button.

A DAC reads a headset's buttons through the microphone contact of a 4-pole
plug, by their resistance, as the CTIA (Android) standard sets them; other
headsets' buttons may read as one button or not at all. The keyboard's keys
always work.

**Why `usb_audio_db` starts at -10 dB.** The CoCo Zero's sound is pitched for
a TV's speakers. A USB DAC at its own 0 dB plays that loud in headphones, and
the hub's chip powers up at exactly 0 dB, its top. So when a DAC starts, the
board sets the DAC's own volume to `usb_audio_db` and turns its mute off. The
log shows the chip's range and where it started:
`[uac] addr=2 volume: range -60.00 to 0.00 dB, came up at 0.00 dB, set to -10.00 dB`.
Too quiet or too loud in your headphones: change `usb_audio_db` (a step of 6 dB
halves or doubles the level) and switch off and on.

### How the USB output is built

- **The USB host** is Pico-PIO-USB, kept in `lib/pico_pio_usb` with an
  isochronous OUT stream added; `PROVENANCE.md` there lists the changes. Each
  1 ms USB frame, its frame handler asks for the next packet, builds it, and
  sends it with no handshake, as audio packets go.
- **The driver,** `src/usb_audio.cpp`, reads each device's configuration
  descriptor when it is plugged in, finds a playback setting for 16-bit stereo
  at 48 kHz, selects it, sets the rate, sets the DAC's own volume and unmute,
  and starts the stream.
- **The samples** come from the same buffer the video cable's sound uses;
  `usb_audio_fill` in `src/main.cpp` takes 48 pairs a frame. The buffer's rate
  servo holds it half full against whichever output drains it, so the
  machine's sound follows the USB clock when the DAC has it.
- **The test builds:** `pizero_usbtone` plays a 440 Hz tone to the DAC, and
  `pizero_usbdump` prints every USB device's descriptors and HID reports
  (`docs/BUILD.md`).
- **The serial log** shows `[uac] streaming packets=N (1000/s) under=N/s`
  once a second while the DAC has the sound.

## How loud each source is

All sources add into one sample per side, then go through the DC blocker and
the `audio_lowpass` filter, then a clamp to 16 bits. At the default volume of 10:

| Source | Full scale |
|---|---|
| DAC | 22,680 from 0 to 63, about 360 a step |
| Orchestra-90 | 23,700 from 0 to 255 on each side |
| SN76489 | 5,670 a channel, 22,680 for all four |
| AY | 7,560 a channel, 22,680 for one chip's three |

Several AY chips on one side are mixed as sum x 2 / (n + 1), so four alike are
1.6 times one rather than four times; a chip with all three levels at 0 is not
counted. In stereo each side is mixed by the chips on that side. The `volume`
setting scales everything (0-15, 10 is the reference). Above 10, or with the DAC
on top of loud chip music, peaks can clip.

## Test programs

All in [`ay-checks/`](ay-checks/), each saved on the card's `AYTEST.DSK`:

| File | Disk name | What it plays |
|---|---|---|
| [`psg.bas`](ay-checks/psg.bas) | `PSG` | CoCo PSG: a chord, then the envelope, then noise |
| [`sym12.bas`](ay-checks/sym12.bas) | `SYM12` | Symphony-12: each chip adds one note of a C major chord |
| [`sym12all.bas`](ay-checks/sym12all.bas) | `SYM12ALL` | Symphony-12: all twelve voices |
| [`stereo.bas`](ay-checks/stereo.bas) | `STEREO` | Symphony-12 stereo: chip 1 left and chip 4 right in turn, then both |
| [`allvoice.bas`](ay-checks/allvoice.bas) | `ALLVOICE` | everything: twelve AY voices, the SN76489's tones and noise drum, AY envelopes, then the DAC on top |
| [`orch90.bas`](ay-checks/orch90.bas) | `ORCH90` | Orchestra-90: a buzz on the left DAC, then the right, then two sawtooths at once from machine code |

Lines starting with `'` are comments for the reader and are not typed in.

## How it is built

| Part | File : symbol |
|---|---|
| SN76489 chip | `lib/coco_machine/src/csg_sn76489.h` : `sn_write`, `sn_render` |
| GMC banking, `$FF41` decode | `lib/coco_machine/src/cart_gmc.h` : `csg_on_latch_mirror` |
| AY chip | `lib/coco_machine/src/psg_ay8910.h` : `ay_write`, `ay_render` |
| AY bank and front-ends | `lib/coco_machine/src/psg_ay8910.h` : `struct ay_bank`, `ayb_owns`, `ayb_write`, `ayb_read`, `ayb_render_lr` |
| Bus hookup | `lib/coco_machine/src/coco_machine.cpp` : `csg_bus_write`; `ayb_owns` in the CoCo 2 read and write paths and in `coco3_cpu_cycle` |
| The multiplexer, enable, single-bit, joystick trials | `lib/coco_machine/src/coco_sound.h` : `snd_line_mv`, `snd_probe_write`, `snd_probe_read`; in the machine `audio_update_level`, `audio_pia1_written`, `audio_comparator_read` |
| Mixing, filters, stereo ring | `lib/coco_machine/src/coco_machine.cpp` : `audio_sample`, `audio_emit`, `coco_machine_audio_read_lr` |
| USB headphone DAC | `src/usb_audio.cpp` : `ua_find`, `ua_vol_run`, `ua_stream`; `src/main.cpp` : `usb_audio_fill`; `lib/pico_pio_usb/src/pio_usb_host.c` : `pio_usb_host_iso_out_stream` |
| Volume keys | `src/hid_consumer.h` : `hidc_parse`, `hidc_decode`; `src/main.cpp` : `volume_keys`, `volume_save_due` |
| Settings | `src/settings.h` : `settings_profile`; applied in `src/main.cpp` : `settings_apply` |

Both chip models are plain integer C, tested on the host (`pio test -e native -f
test_csg_sn76489 -f test_psg_ay8910 -f test_cart_gmc -f test_coco_sound`).
`test_coco_sound` checks the multiplexer's levels and runs a program that
plays a tone while reading the joystick through the audio integrator: with
`joystick_quiet` the samples are those of the tone alone. Each chip is rendered once
per 48 kHz output sample, averaging its output over the sample's time, which keeps
tones above the sample rate from aliasing.

**Rendering only what can change (PIZERO-248).** Stepping every part of every
chip at its own clock cost about 400 CPU clocks a chip a sample on the board, and
`allvoice.bas` overran the frame. Now:

- an AY chip runs only its **heard parts**: a tone switched on in the mixer of a
  channel with a level above 0, the noise if such a channel takes it, the
  envelope if a channel is under it and it is still moving. A part nobody hears
  is left behind and caught up exactly, in one go, when a register write might
  make it heard (`ay_sync`);
- the ticks in which no heard part changes are taken at once (`ay_advance`),
  and a chip keeps its output while nothing heard changes, so most samples only
  count down (`valid`, `quiet`, `pending` in `struct ay8910`);
- the SN76489 takes the ticks in which no counter runs out at once
  (`sn_quiet_ticks`); it does not yet leave silent parts behind (PIZERO-252);
- the render path and its level tables are in RAM, not flash (`SOUND_HOT`,
  `SOUND_TABLE`), and every divide is 32-bit, since a 64-bit divide is a library
  call on the RP2350.

`test_render_skipping_matches_every_tick` (in both chip tests) checks this against
stepping every part every tick, under random register writes between samples of
random length: every sample must match, and the chip's state must match once
caught up.

Measured on the board (product build, CoCo Zero), as emulation time per 16.7 ms
frame:

| | Before PIZERO-248 | Now |
|---|---|---|
| Four Symphony-12 chips (`sym12.bas`), on top of idle | about 5-6 ms | about 1.6 ms (profiler: 1.46 M to 0.64 M clocks a frame of audio work, idle 0.26 M) |
| Everything (`allvoice.bas`) | 15.4-16.7 ms, underruns | 9.3-9.7 ms, peak 10.8 ms, no underruns |

The 1.6 ms includes stereo's second filter chain, which runs while the two sides
differ. Idle at the BASIC prompt is about 6.9 ms. Once a program has used the
SN76489 it costs about 0.85 ms a frame even when silent, until PIZERO-252.

**Telemetry.** The serial log prints `[csg] writes=N dropped=N` once the SN76489
has been written (dropped writes came inside its ready gap) and `[ay] writes=N
chips=M`, where `M` is a bit per AY chip written since reset. The `[settings]`
line shows `sn76489`, `coco_psg`, `symphony12` (`stereo`, `mono` or `off`) and
`deluxe_psg`.

## Not yet

- **Tandy Speech/Sound Cartridge** (PIZERO-249): its AY-3-8913 sits behind a
  microcontroller at `$FF7D`-`$FF7E`, with a speech chip beside it.
- **Real AY software** (PIZERO-251): Lyra 1.2 with the Symphony 12 Enhancer, and
  the Sierra games' PSG music, which need a 512 KB CoCo 3.
- **The Orchestra-90's own music program** has not yet been run on the board
  (its ROM is not on the card, PIZERO-260); the DACs are proven by `orch90.bas`.
- **The CoCo 3 at 1.79 MHz** with all four AY chips is not yet measured
  (PIZERO-252).
- **The video cable and a USB DAC at once,** an on-screen volume display, and
  a DAC plugged in after power-on (PIZERO-257, PIZERO-258, PIZERO-51).

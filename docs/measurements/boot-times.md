# Boot times (PIZERO-315)

How long the board takes from reset to the BASIC `OK`, and where the time
goes. The firmware measures every boot itself, so the figures below can be
re-taken on any card and compared.

## How it is measured

`src/main.cpp` marks each phase's end in milliseconds since reset
(`time_us_64`, the RP2350 timer, which starts at reset). `basic` is the
first frame in which a row of the 32-column text screen at `$0400` starts
with `OK` (read through `coco_machine_peek_ram`, which on a CoCo 3 maps it as
BASIC does at power-on). Time spent on boot pages that wait for a key (no
ROM, Color BASIC only, a missing autorun name) is counted apart as `pages`,
since that is time spent waiting for a person, not booting.

Every boot prints one line on USB serial, and with `debug = on` in
`settings.txt` (PIZERO-316) appends the same line to
`/coco/log/boottime.txt` on the card:

```
[boot] times ms: <clock> fw=<version> machine=coco3 reset=software host=no start=33
  usb_video=95 card=241 monitor=248 rom=314 built=316 settings=323 catalog=674
  listed=675 running=784 frame=798 basic=1246 pages=0
```

A boot that never shows `OK` (a program autorun loads straight into
memory, `@DIRECT`) is logged after 30 s with `basic=none`. A computer that
opens the serial port after the boot gets the line printed again, so the
"no computer" case can be measured too: flash, wait, then open the port.

| Mark | Phase that ends there |
|---|---|
| `start` | Boot ROM and the C runtime, up to the first line of `setup()` |
| `usb_video` | The USB device (MIDI and serial), the USB host (PIO-USB) and the video output start, core 1 launched |
| `card` | SD card mounted, clock seeded |
| `monitor` | `settings.txt` read for the machine and `debug`; with `debug = on`, **up to 4 s waiting for a computer to open the serial port**, plus 200 ms |
| `rom` | Its ROMs loaded |
| `built` | The machine built (`coco_machine_init`) |
| `settings` | `settings.txt` read and applied |
| `catalog` | The disk image catalogue scanned (`coco_boot_rescan`) |
| `listed` | The catalogue printed on serial |
| `running` | autorun.txt, Disk BASIC, the drives restored; the machine starts |
| `frame` | The first emulated frame |
| `basic` | The BASIC `OK` on screen |

## Since PIZERO-316: no wait unless `debug = on`

Same board, card and firmware with the wait moved behind `debug` (off),
2026-11-01, flashed and no computer on the port until afterwards:

| | Reset to BASIC `OK` |
|---|---|
| No computer, `debug = off` | **1.23 to 1.25 s** (three runs) |

The serial wait is gone from that boot (`monitor` is 7 ms after `card`: just
reading `settings.txt`). The rest is as below. `debug = on` keeps the
behaviour measured below, not measured again yet.

## 2026-11-01, before PIZERO-316: the CoCo 3 (`machine = cocozero`)

In these runs the serial wait came first, as `serial`, right after `start`;
it now comes after the card (`monitor`).


`pizero_stream_60`, firmware `v0.7.2-472-ga90be9f-dirty`, the dev card
(about 140 disk images in `/coco/dsk`, drives 0 and 1 mounted, no
autorun program). Each boot was a flash (`reset=software`); four runs of
each case, all within 70 ms of each other. Raw lines:
[`logs/2026-11-01-boot-times-pizero_stream_60.log`](logs/2026-11-01-boot-times-pizero_stream_60.log).

| | Computer on the serial port | No computer (a TV and a power supply) |
|---|---|---|
| Reset to BASIC `OK` | **1.88 to 1.95 s** | **5.38 to 5.43 s** |

Where the time goes (medians, ms):

| Phase | ms | Note |
|---|---|---|
| Before `setup()` | 34 | |
| Serial wait | 750 / 4,250 | With a computer it answers in about 500 ms; without one the full 4 s timeout runs. **This is 78% of a boot with no computer.** |
| USB host and video | 12 | |
| Card | 147 | |
| ROM | 73 | `coco3.rom`, 32 KB |
| Machine built | 2 | |
| Settings | 8 | |
| Disk catalogue | 320 | Scanning about 140 images; grows with the card |
| Catalogue on serial | 4 | |
| autorun, Disk BASIC, drives | 110 | |
| First frame | 14 | |
| BASIC to `OK` | 450 | Emulated: Super Extended Color BASIC and Disk BASIC starting, at 0.89 MHz |

Switching machine in place (PIZERO-314), same session: 80 ms to a CoCo 2,
87 ms to a CoCo 3, over ten round trips.

Not measured yet: a real power-on and a RUN press (they will be in
`/coco/log/boottime.txt` the next time the card is read), and a CoCo 2
boot. The board's clock read 2026-11-02 04:5x while the Mac read
2026-11-01 18:5x EST, so the clock in these lines is off; the times since
reset are not affected.

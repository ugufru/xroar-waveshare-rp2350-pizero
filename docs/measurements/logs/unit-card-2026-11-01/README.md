# Unit card soak logs, copied 2026-11-01 (PIZERO-203)

All 269 soak logs from the SD card that is now in board 1D21CC1CBE17D6F6 (the dev unit), copied as they were on 2026-11-01 and then deleted from the card, plus the card's `boards.txt` (left on the card). The card has been in more than one board: the logs from 2026-10-11 to 10-12 are from the bench board C58D06257AE30E85, and the earliest ones predate the board serial in the header. The logs run from 2026-10-04 19:03:18 to 2026-11-01 22:24:59. The soak log format is `src/soak_log.cpp`; the post-mortem line is PIZERO-227 (`src/wd_post.cpp`).

Logged hours by board: 1D21CC1CBE17D6F6 188.8 h, C58D06257AE30E85 17.6 h, not logged 45.1 h. The earliest logs predate the board serial in the header.

## What they show

- **146 real freezes** (a watchdog reboot whose header names a phase; `last=none` boots are deliberate restarts and are left out). By post-mortem: no post-mortem 79, hang in TIMER0_IRQ_2 64, hard fault, core 1 2, hard fault, core 0 1.

- **Every post-mortem hang since 2026-10-25 is the same**: core 0 stuck in `TIMER0_IRQ_2` at pc 0x20001a74 to 0x20001b44, which resolve against today's build (f2670f4; the ELF of the build that froze, 1f07938-dirty, was not kept, so this assumes the PIO-USB code sits at the same addresses, which the tight cluster of PCs supports) to `pio_usb_bus_send_token` and `pio_usb_bus_usb_transfer` in the PIO-USB host, with core 1 still running. About one every 3 hours on this board, mostly idle at the BASIC prompt (guest pc $A7D3). The `last=` phase in the header is only where the main loop was when the USB timer interrupt got stuck underneath it, which is why PIZERO-203 looked like one fault landing in any phase.

- **Bursts of `loop-top` freezes with no post-mortem**, about every few minutes from 2026-10-29 19:48 to 2026-10-30 01:50, and a few others. PIZERO-98 found a flash can be logged as a false `loop-top` freeze, so these may be a session of repeated flashing; not confirmed.

- **soak-263 to 265** are hard faults right after F11 on build f2670f4 (PIZERO-296's first FILES scan, its working memory on core 0's stack), fixed by moving it to the heap. **soak-269** is the ordinary USB hang again, on the fixed build.

## Every freeze

| Log | Clock | Board | Build | Env | Phase (`last=`) | Post-mortem |
|---|---|---|---|---|---|---|
| soak-003.txt | 2026-10-05 19:30:47 | ? | 292-gd3ec182-dirty | pizero_stream_60 | frame-pace | (none) |
| soak-004.txt | 2026-10-05 21:27:27 | ? | 292-gd3ec182-dirty | pizero_stream_60 | frame-pace | (none) |
| soak-010.txt | 2026-10-11 05:00:32 | ? | 301-g0b3166c-dirty | pizero_stream_60 | emulate | (none) |
| soak-011.txt | 2026-10-11 06:29:02 | ? | 301-g0b3166c-dirty | pizero_stream_60 | emulate | (none) |
| soak-012.txt | 2026-10-11 15:03:20 | ? | 301-g0b3166c-dirty | pizero_stream_60 | VDG-render | (none) |
| soak-021.txt | 2026-10-11 22:33:27 | 0E85 | 315-g4ed9fe4-dirty | pizero_cpu179 | blit | (none) |
| soak-022.txt | 2026-10-12 06:19:54 | 0E85 | 315-g4ed9fe4-dirty | pizero_cpu179 | emulate | (none) |
| soak-025.txt | 2026-10-12 09:47:21 | 0E85 | 316-g05956c5 | pizero_cpu179 | emulate | (none) |
| soak-027.txt | 2026-10-12 10:33:58 | 0E85 | 318-g4cc400b | pizero_stream_60 | frame-pace | (none) |
| soak-050.txt | 2026-10-25 00:21:45 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b44 lr=20081b43 in TIMER0_IRQ_2 ... guest_pc=16f5 usb=2 up=323665ms |
| soak-051.txt | 2026-10-25 00:25:31 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-069.txt | 2026-10-25 07:23:24 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b42 lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=264f usb=2 up=2190466ms |
| soak-072.txt | 2026-10-25 13:08:54 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081ad3 in TIMER0_IRQ_2 ... guest_pc=23da usb=2 up=17392000ms |
| soak-073.txt | 2026-10-25 15:15:54 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081b63 in TIMER0_IRQ_2 ... guest_pc=231c usb=2 up=7619863ms |
| soak-074.txt | 2026-10-26 11:14:11 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=23dd usb=2 up=71896465ms |
| soak-075.txt | 2026-10-26 11:35:36 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | VDG-render | hang, core 0 at pc=20001b3e lr=20081beb in TIMER0_IRQ_2 ... guest_pc=25d7 usb=2 up=1285267ms |
| soak-076.txt | 2026-10-26 11:59:17 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=25d5 usb=2 up=1420867ms |
| soak-077.txt | 2026-10-26 17:08:02 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bc3 in TIMER0_IRQ_2 ... guest_pc=2405 usb=2 up=18525065ms |
| soak-078.txt | 2026-10-26 17:10:34 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081b3b in TIMER0_IRQ_2 ... guest_pc=2473 usb=2 up=151866ms |
| soak-079.txt | 2026-10-26 17:25:25 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b42 lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=2310 usb=2 up=890066ms |
| soak-081.txt | 2026-10-26 20:52:15 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | blit | hang, core 0 at pc=20001b3e lr=20081843 in TIMER0_IRQ_2 ... guest_pc=23b9 usb=2 up=4730600ms |
| soak-082.txt | 2026-10-26 22:42:02 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001a9c lr=20002c1b in TIMER0_IRQ_2 ... guest_pc=23b9 usb=2 up=6587065ms |
| soak-084.txt | 2026-10-27 03:55:04 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081b03 in TIMER0_IRQ_2 ... guest_pc=23da usb=2 up=5161800ms |
| soak-085.txt | 2026-10-27 10:33:26 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b42 lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=23c4 usb=2 up=23902065ms |
| soak-086.txt | 2026-10-27 13:12:14 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081a8b in TIMER0_IRQ_2 ... guest_pc=2622 usb=2 up=9528067ms |
| soak-088.txt | 2026-10-27 20:25:53 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=25dd usb=2 up=15998600ms |
| soak-095.txt | 2026-10-27 23:20:40 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c0b in TIMER0_IRQ_2 ... guest_pc=246f usb=2 up=3238665ms |
| soak-096.txt | 2026-10-27 23:28:37 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b42 lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=23df usb=2 up=476666ms |
| soak-097.txt | 2026-10-28 00:34:33 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bc3 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=3955464ms |
| soak-098.txt | 2026-10-28 05:32:28 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081bdb in TIMER0_IRQ_2 ... guest_pc=260d usb=2 up=17874667ms |
| soak-099.txt | 2026-10-28 05:36:20 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081be3 in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=232465ms |
| soak-100.txt | 2026-10-28 05:36:33 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c0b in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=13265ms |
| soak-101.txt | 2026-10-28 08:23:12 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-117.txt | 2026-10-28 11:13:20 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=009f usb=2 up=8136465ms |
| soak-118.txt | 2026-10-28 11:15:48 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bf3 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=147465ms |
| soak-119.txt | 2026-10-28 13:02:07 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=6379665ms |
| soak-120.txt | 2026-10-28 13:19:53 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001a98 lr=20002409 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=1065867ms |
| soak-121.txt | 2026-10-28 13:22:17 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b42 lr=20081c2b in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=143666ms |
| soak-122.txt | 2026-10-28 18:09:25 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | VDG-render | hang, core 0 at pc=20001b42 lr=20081bf3 in TIMER0_IRQ_2 ... guest_pc=b232 usb=2 up=17227865ms |
| soak-123.txt | 2026-10-28 18:33:30 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081a83 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=1445066ms |
| soak-132.txt | 2026-10-28 22:09:13 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | hang, core 0 at pc=20001a74 lr=20002409 in TIMER0_IRQ_2 ... guest_pc=246f usb=2 up=3646065ms |
| soak-144.txt | 2026-10-29 00:36:02 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c33 in TIMER0_IRQ_2 ... guest_pc=22e0 usb=2 up=1105665ms |
| soak-145.txt | 2026-10-29 08:10:31 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bf3 in TIMER0_IRQ_2 ... guest_pc=29ca usb=2 up=27268666ms |
| soak-153.txt | 2026-10-29 11:43:03 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=200819f3 in TIMER0_IRQ_2 ... guest_pc=371f usb=2 up=10662866ms |
| soak-154.txt | 2026-10-29 11:43:11 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-155.txt | 2026-10-29 12:20:50 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081ab3 in TIMER0_IRQ_2 ... guest_pc=2fb2 usb=2 up=2258867ms |
| soak-156.txt | 2026-10-29 12:20:57 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-157.txt | 2026-10-29 13:12:02 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bfb in TIMER0_IRQ_2 ... guest_pc=2fb6 usb=2 up=3064867ms |
| soak-158.txt | 2026-10-29 13:12:09 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-159.txt | 2026-10-29 14:30:48 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081bfb in TIMER0_IRQ_2 ... guest_pc=2fb6 usb=2 up=4718467ms |
| soak-160.txt | 2026-10-29 14:30:55 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-161.txt | 2026-10-29 14:43:08 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081ba3 in TIMER0_IRQ_2 ... guest_pc=2fa9 usb=2 up=733065ms |
| soak-162.txt | 2026-10-29 14:43:15 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-163.txt | 2026-10-29 15:47:05 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001a98 lr=20002c1b in TIMER0_IRQ_2 ... guest_pc=2fb7 usb=2 up=3829668ms |
| soak-164.txt | 2026-10-29 15:47:12 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-165.txt | 2026-10-29 18:17:30 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=200819eb in TIMER0_IRQ_2 ... guest_pc=2fbd usb=2 up=9017466ms |
| soak-166.txt | 2026-10-29 18:17:37 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-167.txt | 2026-10-29 18:51:10 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c33 in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=2012265ms |
| soak-168.txt | 2026-10-29 18:51:17 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-169.txt | 2026-10-29 18:53:48 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-170.txt | 2026-10-29 18:56:50 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-171.txt | 2026-10-29 18:57:49 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-172.txt | 2026-10-29 19:21:37 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-173.txt | 2026-10-29 19:24:15 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081aab in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=157866ms |
| soak-174.txt | 2026-10-29 19:24:22 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | SETUP | (none) |
| soak-175.txt | 2026-10-29 19:25:55 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-176.txt | 2026-10-29 19:37:13 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081b9b in TIMER0_IRQ_2 ... guest_pc=a98d usb=2 up=678066ms |
| soak-177.txt | 2026-10-29 19:48:04 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-178.txt | 2026-10-29 19:50:22 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-179.txt | 2026-10-29 19:53:17 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-180.txt | 2026-10-29 20:00:34 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-181.txt | 2026-10-29 20:01:58 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-182.txt | 2026-10-29 20:02:56 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-183.txt | 2026-10-29 20:06:17 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-184.txt | 2026-10-29 20:11:41 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-185.txt | 2026-10-29 20:12:58 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-186.txt | 2026-10-29 20:14:18 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-189.txt | 2026-10-29 21:36:33 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001a98 lr=20002409 in TIMER0_IRQ_2 ... guest_pc=a98d usb=2 up=2514865ms |
| soak-190.txt | 2026-10-29 21:37:22 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-191.txt | 2026-10-29 22:07:00 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-192.txt | 2026-10-29 22:08:11 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-193.txt | 2026-10-29 22:21:01 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-194.txt | 2026-10-29 22:21:32 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-195.txt | 2026-10-29 22:25:10 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-196.txt | 2026-10-29 22:32:29 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-197.txt | 2026-10-29 22:42:53 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-198.txt | 2026-10-29 22:43:25 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-199.txt | 2026-10-29 23:08:31 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-200.txt | 2026-10-29 23:09:18 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-201.txt | 2026-10-29 23:15:09 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-202.txt | 2026-10-29 23:15:52 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-203.txt | 2026-10-29 23:16:41 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-204.txt | 2026-10-29 23:17:59 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-205.txt | 2026-10-29 23:38:45 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-206.txt | 2026-10-29 23:39:24 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=2b35 usb=2 up=39464ms |
| soak-207.txt | 2026-10-29 23:40:34 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-208.txt | 2026-10-30 00:28:11 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-209.txt | 2026-10-30 00:33:57 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-210.txt | 2026-10-30 00:35:00 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-211.txt | 2026-10-30 00:45:32 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-212.txt | 2026-10-30 00:47:23 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-213.txt | 2026-10-30 00:49:06 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-214.txt | 2026-10-30 00:51:40 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-215.txt | 2026-10-30 01:02:49 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-216.txt | 2026-10-30 01:04:46 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-217.txt | 2026-10-30 01:05:37 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-218.txt | 2026-10-30 01:06:03 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-219.txt | 2026-10-30 01:06:40 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-220.txt | 2026-10-30 01:07:56 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-221.txt | 2026-10-30 01:10:27 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=3fa2 usb=2 up=149865ms |
| soak-222.txt | 2026-10-30 01:27:41 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-223.txt | 2026-10-30 01:30:03 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-224.txt | 2026-10-30 01:37:27 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-225.txt | 2026-10-30 01:39:44 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-226.txt | 2026-10-30 01:40:55 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-227.txt | 2026-10-30 01:41:57 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-228.txt | 2026-10-30 01:42:32 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-229.txt | 2026-10-30 01:43:52 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-230.txt | 2026-10-30 01:48:48 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-231.txt | 2026-10-30 01:49:53 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-236.txt | 2026-10-30 04:03:11 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081be3 in TIMER0_IRQ_2 ... guest_pc=2fb5 usb=2 up=7109266ms |
| soak-237.txt | 2026-10-30 04:38:44 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | VDG-render | hang, core 0 at pc=20001b42 lr=20081beb in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=2132466ms |
| soak-238.txt | 2026-10-30 06:46:39 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bc3 in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=7674867ms |
| soak-239.txt | 2026-10-30 07:06:49 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001b3e lr=20081c33 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=1209866ms |
| soak-240.txt | 2026-10-30 09:32:28 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b44 lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=8739064ms |
| soak-241.txt | 2026-10-30 10:47:24 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | blit | hang, core 0 at pc=20001b3e lr=20081843 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=4495465ms |
| soak-242.txt | 2026-10-30 11:41:08 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081b73 in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=3224268ms |
| soak-243.txt | 2026-10-30 15:54:50 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bc3 in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=15221866ms |
| soak-244.txt | 2026-10-30 16:38:41 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001a9c lr=20002c1b in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=2631066ms |
| soak-245.txt | 2026-10-30 18:56:04 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001a9c lr=20002409 in TIMER0_IRQ_2 ... guest_pc=495e usb=2 up=8242666ms |
| soak-246.txt | 2026-10-30 20:10:32 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=4468064ms |
| soak-247.txt | 2026-10-30 22:39:57 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-248.txt | 2026-10-30 22:47:07 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001a98 lr=20002409 in TIMER0_IRQ_2 ... guest_pc=4960 usb=2 up=430065ms |
| soak-249.txt | 2026-10-31 01:51:44 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bfb in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=11076465ms |
| soak-250.txt | 2026-10-31 03:28:50 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001a98 lr=20002c1b in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=5826066ms |
| soak-251.txt | 2026-10-31 14:23:52 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c0b in TIMER0_IRQ_2 ... guest_pc=2000 usb=2 up=39301665ms |
| soak-252.txt | 2026-10-31 17:40:50 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b42 lr=20081b1b in TIMER0_IRQ_2 ... guest_pc=a7d5 usb=2 up=11818266ms |
| soak-253.txt | 2026-10-31 22:20:20 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-254.txt | 2026-10-31 23:07:34 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | USBHost.task | (none) |
| soak-255.txt | 2026-10-31 23:08:31 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | loop-top | (none) |
| soak-258.txt | 2026-11-01 00:19:10 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081bc3 in TIMER0_IRQ_2 ... guest_pc=0e47 usb=2 up=3185266ms |
| soak-261.txt | 2026-11-01 21:51:41 | D6F6 | 452-g1f07938-dirty | pizero_stream_60 | frame-pace | hang, core 0 at pc=20001a98 lr=20002409 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=1699000ms |
| soak-263.txt | 2026-11-01 22:11:32 | D6F6 | 463-gf2670f4 | pizero_stream_60 | USBHost.task | HARD FAULT on core 1 pc=00000013 lr=00000000 in IRQ 240 cfsr=00020000 hfsr=40000000 exc_return=..f0 guest_pc=a7d5 usb=2 up=18833ms |
| soak-264.txt | 2026-11-01 22:11:55 | D6F6 | 463-gf2670f4 | pizero_stream_60 | USBHost.task | HARD FAULT on core 0 pc=000c2f00 lr=2080c380 in thread mode (no interrupt) cfsr=00008200 hfsr=40000000 bfar=00010000 exc_return=..e1 guest_pc=a7d3 usb=2 up=20188ms |
| soak-265.txt | 2026-11-01 22:14:25 | D6F6 | 463-gf2670f4 | pizero_stream_60 | emulate | HARD FAULT on core 1 pc=200800ae lr=20069db8 in thread mode (no interrupt) cfsr=00008200 hfsr=40000000 bfar=71816980 exc_return=..f9 guest_pc=a7d3 usb=2 up=36792ms |
| soak-269.txt | 2026-11-01 22:24:59 | D6F6 | 463-gf2670f4-dirty | pizero_stream_60 | emulate | hang, core 0 at pc=20001b3e lr=20081c03 in TIMER0_IRQ_2 ... guest_pc=a7d3 usb=2 up=43633ms |

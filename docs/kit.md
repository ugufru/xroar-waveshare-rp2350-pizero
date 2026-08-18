# Building a unit

How to assemble one of these to give away, what goes in the box, and what it
costs. There is no store and no orders — units are built in small batches at
the author's expense and handed to people who ask.

Engineering detail lives elsewhere: `README.md` (hardware, pinout, what works),
`docs/BUILD.md` (firmware builds and flashing), `AUTORUN.md` (SD card layout).
This file is only about turning a bare board into something a friend can plug in.

---

## 1. What ships, and why

The recipient supplies the obvious things. What they almost certainly **cannot**
supply are two adapters, and those are the whole reason a "kit" exists rather
than just handing someone a board.

| In the box | Why it must be included |
|---|---|
| **Waveshare RP2350-PiZero**, pre-flashed | The only irreplaceable item |
| **Case**, cutouts opened | See §5 |
| **mini-HDMI → HDMI cable** | The board's HDMI is the **mini** size. Nobody has one of these in a drawer |
| **USB-C male → USB-A female adapter** (OTG) | The **USB host port is a Type-C**. A normal USB keyboard cannot physically reach it without this |
| **Printed card** | See §4. Cheaper than answering the same question ten times |
| microSD card *(optional)* | Not required, but see §3 — a pre-laid-out card removes most of the setup friction |

| Recipient supplies | Notes |
|---|---|
| USB keyboard | Any **simple wired** one. See the USB 1.1 limit in §4 |
| microSD card | Any size; FAT32 |
| USB-C power | Any phone charger or battery bank |
| HDMI display | Any TV or monitor |
| **CoCo ROMs** | `bas12.rom` is **required**. We ship none — see §6 |
| Games, disk images, programs | Their own |

> **The two Type-C ports look identical and are not interchangeable.** One is the
> USB host (keyboard), the other is power/programming. Label them on the case, or
> the first thing every recipient does is plug power into the wrong one.

## 2. Bill of materials

Costs for a batch of **10**. Prices checked 2026-08-17; treat the estimates as
placeholders and confirm at order time.

| Item | Qty | Unit | Source | Confidence |
|---|---|---|---|---|
| Waveshare RP2350-PiZero | 12 | **$9.99** direct from Waveshare | waveshare.com | **Sourced** |
| *(same board on Amazon)* | — | *$17.17* | — | **Sourced** — nearly 2× |
| CNC aluminum case | 10 | ~$6 | — | User-sourced |
| mini-HDMI → HDMI cable | 10 | ~$7–10 | Amazon | *Estimate* |
| USB-C → USB-A OTG adapter | 10 | ~$3–5 (cheaper in multipacks) | Amazon | *Estimate* |
| microSD card *(optional)* | 10 | ~$5 | Amazon | *Estimate* |

**Roughly $28/unit, ~$280 for ten**, before shipping.

Two things worth knowing:

- **Buy the boards direct from Waveshare.** At $9.99 vs $17.17 on Amazon, the
  dominant line item is nearly half price. On ten units that is the difference
  between ~$100 and ~$172.
- **Order spare boards.** Twelve for a batch of ten. It is the one part with no
  substitute, spares cost $10, and a re-order costs shipping and a week.

Amazon blocks automated price retrieval, so the accessory figures above are
estimates rather than quotes. Fill in real numbers when ordering.

## 3. Assembly

1. **Flash the board.** Build the default env and upload:
   ```
   pio run -e pizero_stream_60 -t upload
   ```
   Or, with no toolchain on the machine you are using, hold **BOOT** while
   plugging in USB-C and drag `firmware.uf2` onto the `RPI-RP2` drive that
   appears.

   **Pre-flash every unit.** A recipient who has to install PlatformIO and build
   a firmware will not do it, and the unit sits in a drawer. Shipping it working
   is the single biggest difference between a gift and a project.

2. **Fit the case.** Open the cutouts (§5), then check every port physically
   before closing it — both Type-C, mini-HDMI, microSD, and **BOOT button
   access**. BOOT is the recovery path when a flash goes wrong; a case that
   buries it turns a five-minute fix into disassembly.

3. **Prepare the SD card** *(if including one)*. Format FAT32 and create the
   layout from `AUTORUN.md`:
   ```
   /coco/
     roms/          <- empty; recipient adds bas12.rom (required),
                       extbas11.rom, disk11.rom
     dsk/           <- empty
     autorun.txt    <- copy from sample-sd/coco/autorun.txt
   firmware.uf2     <- so they can re-flash without a toolchain
   ```
   The sample `autorun.txt` is a self-running graphics demo, which doubles as a
   display test — but it needs BASIC, so it does nothing until ROMs are added.

4. **Test before boxing.** You need a ROM-equipped SD card of your own for this
   — a unit with no ROMs shows a black screen and proves nothing (see §6). Power
   it up with a keyboard attached and confirm the BASIC `OK` prompt appears.
   Thirty seconds, and it catches a bad board, a bad flash, and a case that
   shorts something. Keep one "known-good" test card in the build kit.

## 4. The card that goes in the box

These four items account for essentially every question you will be asked.

> **CoCo on RP2350 — read this first**
>
> **1. Plug the keyboard in BEFORE you power on.** Hot-plugging does not work.
> This is a hardware limitation of the board, not a bug being fixed — if you
> swap the keyboard, power-cycle the unit.
>
> **2. Use a simple wired USB keyboard.** The host port is USB 1.1. Basic wired
> keyboards and full-speed wireless receivers work; high-speed USB 2.0
> peripherals will not enumerate.
>
> **3. The two USB-C ports are different.** One is the keyboard (host), the
> other is power. They look identical. *(Marked on the case.)*
>
> **4. You need to supply CoCo ROMs — until you do, THE SCREEN STAYS BLACK.**
> A black screen does *not* mean the unit is broken; it means no ROM was found.
> Format a microSD as FAT32 and put `bas12.rom` in `/coco/roms/` — that one is
> required. `extbas11.rom` and `disk11.rom` are strongly recommended. We do not
> distribute them; they are still under copyright, so dump or source your own.
>
> Everything else — the source, the build instructions, the SD layout, and how
> to update the firmware — is at:
> `github.com/ugufru/xroar-waveshare-rp2350-pizero`

## 5. Case

**Current plan: off-the-shelf CNC aluminum shells (~$6), cutouts opened by
hand.** Budget 10–15 minutes of filing per unit. That is ~2 hours across a batch
of ten — annoying but cheaper than the alternative, because designing a custom
case costs more than 2 hours the first time.

**Why not print them?** Filament for a case this size is only about 20 g, or
**$0.40–0.50** — an order of magnitude under $6. But saving ~$5.50 a case against
a ~$200 printer breaks even at roughly **40 cases**, so printing is the right
answer for a second or third batch, not this one.

**Why not CNC them?** At the ~$500 entry point you get a 3018-class router:
excellent for wood, plastic, acrylic and PCB work, and fine for *engraving*
aluminum, but not rigid enough to mill an enclosure from billet. Machines that
cut aluminum properly start well above that, and still would not beat $6/unit.
CNC's real use here would be **opening the cutouts on bought cases** with a
fixture instead of a file — worth setting up at 100 units, not at 10.

**Fit note:** the board is **65 × 30 mm, the same outline as a Raspberry Pi
Zero**, so Pi Zero shells match the footprint and mounting holes. They do *not*
match the ports: a Pi Zero uses **micro-USB** where this board uses **USB-C**,
and this board adds a second Type-C, a 2-pin battery connector and a 3-pin debug
header. Always test-fit one case before buying a batch.

**Follow-on:** publish an STL once a printed design exists. Some recipients own
printers, which takes the case out of the BOM entirely for them — and a printed
shell can expose the **40-pin GPIO header**, which the emulator does not use at
all and which is the most interesting thing on the board for anyone who wants to
attach a joystick or a peripheral.

## 6. Licensing

**Source.** XRoar is GPLv3, and giving a unit away is distribution, so
recipients are entitled to the corresponding source. The public repository
satisfies this — the card in §4 points at it. If the repo is private at the time
of a handout, put a source tarball on the SD card instead.

**ROMs.** Color BASIC and Disk BASIC are still under copyright. **Ship none.**
Recipients supply their own, exactly as with any emulator.

> **Warning — a unit with no ROMs shows a BLACK SCREEN.** `setup()` returns early
> when `bas12.rom` is missing (`src/main.cpp:1066`) and the explanation goes only
> over USB serial. libdvi has already started by then, so the monitor *syncs* and
> then displays nothing — which reads exactly like a dead unit. Warn recipients on
> the card, and see `PIZERO-92` for putting that message on screen instead.

**Games and disk images.** Same rule. If a card ships with content, it should be
homebrew or freely redistributable only.

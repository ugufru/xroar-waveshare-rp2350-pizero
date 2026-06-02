# Product plan — CoCo-on-RP2350 (working title)

**Sold via barenakedgames.com. This document is the product plan; engineering
rationale and the confirmed pinout stay in `README.md`. This file is intentionally
separate from the README so it can carry BOM, pricing, marketing, and the V1.0
firmware specification in one place.**

Status: **draft** (2026-06). The prototype is feature-complete enough to demo
(Color BASIC boots over HDMI at a locked 60 fps, USB keyboard input works); the
work below is *productization* — the launcher, audio, an original ROM, and the
legal/QA gates that matter once it ships for money.

---

## 1. The product

### Elevator pitch
> For $30 you can experience the joy of owning your own computer, playing vintage
> games, and learning how to develop new games for it just like it's 1980 again.
> But this time, it's easy. No cassettes. No floppy disks. No RF modulators. Just a
> generic USB keyboard and your favorite TV or HDMI monitor. This isn't a melancholic
> journey into the past — it's a portal into the future. Build it for yourself, or
> order my complete kit that will have you running as soon as it's out of the box.

### What it is
A tiny, cased Tandy Color Computer (CoCo 1/2) experience built on the Waveshare
RP2350-PiZero, running an XRoar-based firmware. Plug it into any HDMI TV/monitor,
attach a generic USB keyboard, power it from a battery bank or USB charger, insert
the SD card, and a simple boot menu lets you launch games, disks, cartridges, or
drop into BASIC to write your own programs.

### What we sell vs. what the customer brings (the model)
We are not selling a sealed retro console. We are selling an **open, dressed-up
platform on a ~$9 SoC**, with the customer fully enabled to run whatever they like.

**What we sell:**
1. **The launcher / bootloader firmware** — our core software IP (the §3.1 contract).
2. **Original games** — a homebrew CoCo title or set of titles we author and own.
   This is the creative product.
3. **The dress-up** — case, box, kit, assembly, curation, the unboxing.
4. **Enablement** — open-source firmware, documentation, and the *knowledge* to load
   anything.

**What the customer brings / loads (fully enabled, their responsibility):**
- Their own **Color BASIC / Disk BASIC ROMs** — sourced or dumped from hardware they
  own. This is the standard emulator distribution model: every emulator ships *without*
  copyrighted ROMs and lets the user supply them.
- Any games, disks, or cartridge images they want.
- Expansion such as **FujiNet / DriveWire networking** and header peripherals (§5).

**Out-of-box experience:** the box boots to the launcher and runs **our original,
self-contained games** (no copyrighted ROM needed). "Running out of the box" = our
games run; "the full vintage experience" = the customer adds their own ROMs and titles.

This model deliberately removes the two biggest risks from earlier drafts: we never
distribute copyrighted ROMs, and we are **not** required to author a Microsoft-BASIC
clone for V1.0 (see §4).

### Product tiers
Two SKUs, because a keyboard-included boxed kit cannot hit $30 at cost:

| Tier | Target price | Contents |
|---|---|---|
| **DIY / Maker** | **~$30** | Board + preloaded microSD + printed BASIC quick-start + case STL files. Bring your own keyboard, HDMI cable, power, and 3D printer. This is the "$30" hook. |
| **Complete Kit** | **~$59–79** | Assembled in the printed case, USB keyboard, mini-HDMI→HDMI cable, power solution, full box & unboxing — "running out of the box." |

---

## 2. Bill of materials (estimates — needs real sourcing)

| Item | Est. unit cost | Tier | Notes |
|---|---|---|---|
| Waveshare RP2350-PiZero | ~$7–10 | both | the board |
| microSD (4–8 GB, preloaded) | ~$2–3 | both | firmware + freely-distributable games + our original ROM |
| mini-HDMI → HDMI cable | ~$2–3 | kit | required output |
| USB keyboard (generic) | ~$8–12 | kit | the margin driver — "included" per the pitch |
| Power: USB-C cable (battery/charger BYO) | ~$2–5 | kit | recommend external battery bank or 5 V charger |
| 3D-printed case | ~$1–2 material | kit (STL only for DIY) | + print time / labor |
| Box, sleeve, quick-start card | ~$2–4 | kit | the unboxing |

**Margin note:** the Complete Kit BOM exceeds $30 before labor/shipping/margin —
hence the tiered pricing. The $30 entry is the DIY tier (board + SD + guide), which
has healthy margin (~$11 cost).

**Power topology (confirmed operational idea):** keyboard on the **PIO-USB host
port**, power on the **USB-C port** from a battery bank or charger. The two ports
coexist, and feeding power from a real supply also sidesteps the hot-replug symptom
(PIZERO-11b) caused by marginal dev-port VBUS.

---

## 3. Firmware V1.0 specification (the core of the product)

XRoar-based; targets the **CoCo 1/2 (MC6847 VDG, Extended Color BASIC class)** —
the model already emulated and rendered. **CoCo 3 (GIME) is explicitly out of scope
for V1.0** (major emulation lift; revisit later).

### 3.1 The boot manager / launcher — design principle
The launcher is a **thin, low-level shim around XRoar** that is **completely
transparent once a title is running**. A hard two-state model enforces that:

- **Launcher state** — emulation is not running. The launcher owns the screen and
  keyboard, reads the SD card, and shows a list.
- **Emulation state** — XRoar runs the selected target. The launcher is **dormant**:
  zero CPU, no overlay, no periodic SD access, **no input interception except one
  reserved "menu" key combo.** That single combo-compare per USB HID report is the
  launcher's entire runtime footprint — microseconds, nothing on the render / audio /
  timing path. The 60 fps budget and double-buffer are untouched. Transparency is
  structural: the launcher simply *isn't in the loop* once a title starts.

### 3.2 Launcher rendering — native, not via the CoCo
The menu is drawn directly into the existing 320×240 RGB565 framebuffer using the
glyph/alpha renderer the firmware already ships. Rationale:
- **Machine-independent** — works regardless of which model/ROM loads next.
- **No dependency on a ROM being present** — the launcher must work on first boot
  even before any title or BASIC is selected.
- Reuses existing code, and runs **only** in launcher state.

(Rejected alternative: implementing the menu as a CoCo program inside the emulator —
that would violate "transparent to the CoCo emulation" and couple the UI to a ROM.)

### 3.3 What the launcher does
- **Enumerate** a games directory on SD (e.g. `/coco`) and classify by type:
  - `.DSK` → mount drive 0, autostart
  - `.BIN` → `LOADM` + `EXEC` (direct-load path already exists)
  - cartridge ROM (`.ROM` / `.CCC`) → map at $C000, autostart on reset
  - synthetic entries: **BASIC** (shown when our ROM is present)
- **Per-title metadata** — sidecar `.txt` or a `games.json` (friendly name, model,
  autostart command, control hints). This folds in the existing `AUTORUN.TXT` logic:
  a `.DSK` often needs to be told *how* to start (`RUN"PROG"`, DOS boot, etc.).
- **Keyboard navigation** — up/down/page, Enter to launch, letter-jump; remember the
  last selection in a small SD state file.
- **Robustness (product-grade, not prototype-grade):** no/unreadable SD → friendly
  screen, never a hang; corrupt/oversized image → skip with a note; missing ROM →
  guide the user rather than fail.

### 3.4 Launch and return
- **Launch:** reset the XRoar machine to a clean state with the right ROM set + model,
  attach the artifact, hand off screen + input, go dormant.
- **Return-to-launcher:** a reserved key combo (the CoCo had no menu key — we add one
  in the firmware input layer, intercepted before the CoCo matrix; printed on the
  quick-start card). On detect: **cleanly tear down** the running machine (detach
  disk/cart) and re-init the launcher.
- **New engineering task:** runtime teardown/re-init — going game → menu → another
  game *without a reboot*. Today the firmware loads once at boot; this is the one
  genuinely new piece of plumbing. Bounded but real.

### 3.5 Audio — **required for V1.0**
The product promises "playing vintage games"; silence is a deal-breaker, so audio is
a V1.0 requirement, not a stretch. See `docs/audio-decision.md` for the full scoping.
- **Primary:** HDMI audio over the existing cable via `libdvi` data-island packets —
  no extra hardware, preserves the "one cable" story. Our 24 MHz pixel clock yields
  clean ACR integers (32 kHz → N=4096, CTS=24000).
- **Fallback:** PWM out of one GPIO → RC filter → small Class-D amp.
- **Highest-risk V1.0 item.** De-risk *before* committing the marketing claim: bench a
  reference HDMI-audio firmware on the actual target monitor to confirm the sink
  accepts HDMI audio over this board's wiring and off-spec timing.

### 3.6 Joystick — **V1.0, USB-first; header as expansion**
- **USB HID gamepad/joystick** mapped to the CoCo joystick axes/buttons (PIZERO-13).
- **Expansion via the 40-pin header** (see §5): a real analog/digital joystick wired to
  free header GPIOs is a natural follow-on and a marketing feature in its own right.
  V1.0 ships USB; the header path can be a documented mod / later add-on board.

### 3.7 Out of scope for V1.0 (named to prevent creep)
Save states, in-game overlay/menu, disk-swap UI, CoCo 3 / GIME, synth & alternate
sound chips (PIZERO-17), alternate video device (PIZERO-16), networking.

---

## 4. ROMs — open platform, customer-supplied (no copyrighted ROMs ship)

**Decision:** the product **ships no copyrighted ROMs.** Microsoft/Tandy Color BASIC
and Disk BASIC copyrights are live; we never distribute them. Instead the platform is
**open and fully enabled** — the customer loads their own BASIC/Disk BASIC ROMs (and
any games), exactly as every emulator does. We provide the capability, the launcher,
and the documentation, not the copyrighted bits.

**Authoring our own BASIC is OPTIONAL, not a V1.0 gate.** Because the customer supplies
ROMs, we are *not* required to write a Microsoft-BASIC clone to ship. The out-of-box
legal experience is the launcher plus **our original, self-contained games** (cartridge
ROM / `.BIN`/`LOADM` homebrew we author and own) — none of which need a copyrighted ROM.

**What this means for title types at launch:**
- **Our original games** — self-booting, ship on the SD, work out of the box.
- **`.DSK`** — works fully once the customer adds their (own) Disk BASIC ROM.
- **Cartridge ROM / `.BIN`** — work without BASIC.
- **BASIC entry** — appears in the launcher when a customer-supplied ROM is detected.

**Optional future (not required):** a small *original* BASIC/monitor of our own would
let the "learn to program" experience work with zero customer-supplied ROMs. It's a
large 6809 effort and is explicitly a nice-to-have, tracked but not blocking V1.0.

---

## 5. Expandability — the unused 40-pin header

A genuine, *true* differentiator: **the emulator uses none of the general-purpose
pins on the Pi-style 40-pin header.** The three real I/O functions live on dedicated
peripherals / high GPIOs — HDMI on 32–39, microSD on 30/31/40/43/22, USB host on
28/29 — none of which are the user header. The only header-adjacent SDK defaults
(UART0 0/1, I²C0 6/7, WS2812 on 2) are free or trivially freed (debug is USB-CDC).

So the header is open for: a real joystick/gamepad, I²C/SPI sensors and displays,
add-on sound or cartridge boards, GPIO experiments — "it's a real, hackable computer,
not a sealed appliance." Pairs naturally with the open-source firmware angle (§7).

### Networking / FujiNet / DriveWire (enablement, not a V1.0 feature)
The open platform extends to networking, which is a strong community draw:
- **DriveWire / "becker" port** — XRoar already has the CoCo DriveWire hook for virtual
  disks/network over the serial path; a documented enablement target.
- **FujiNet-CoCo** — the real external FujiNet device gives network drives/modem
  emulation on CoCo hardware; an example of what the open header/USB can host.

**Accuracy caveat:** this board is **RP2350B with no onboard WiFi** — networking needs
an add-on (USB or header). So FujiNet/DriveWire is framed as *expansion the platform
enables*, not a built-in V1.0 checkbox. It's the headline example of "open and enabled."

> **TODO before printing this as a hardware spec:** lock the exact free-pin list and
> header GPIO map against `docs/RP2350-PiZero-schematic.pdf`. Do not publish specific
> pin numbers until verified.

---

## 6. Marketing

### Feature / spec sheet (claims)
- Authentic Tandy Color Computer experience on any HDMI TV/monitor
- Generic USB keyboard — no proprietary hardware
- SD-card game library, instant load — no tapes, floppies, or RF modulators
- Disk, cartridge, and BASIC support via a simple boot menu
- Sound over the same HDMI cable *(claim only once proven — §3.5)*
- USB gamepad support; expandable via a full 40-pin header
- Write and run your own programs — learn to code like 1980
- Tiny, low-power, runs off a battery bank
- Open & hackable (XRoar + RP2350)

### Positioning / data
The CoCo niche is **underserved** — there's no cheap turnkey CoCo, unlike the C64
(THEC64 ~$70–120) or Spectrum (Next £200+). Differentiators: **cheapest turnkey retro
computer**, the **only easy modern CoCo**, the **learn-to-build-games** angle, and a
**fully exposed expansion header**. Audiences: the active CoCo community (CoCoFEST!,
Color Computer Discord/forums), the retro/maker crowd, and educators/parents buying a
nostalgic learn-to-code gift. Channels: barenakedgames.com store, CoCo forums, retro
YouTube unboxings/demos, Hackaday, a possible Tindie cross-list, and a CoCoFEST! booth.

### Unboxing
Sleeve-art box; the cased unit (SD pre-inserted); HDMI cable; keyboard; and a one-page
**quick-start** (plug HDMI → keyboard → power → on; the menu key). Lean into the dev
angle cheaply with a printed **BASIC quick-reference / type-in listing** — on-brand for
"learn to develop games" and nearly free.

---

## 7. Gating risks & compliance (decide before going to production)

| Risk | Severity | Notes / action |
|---|---|---|
| **ROM licensing** | 🟢 resolved | Open model (§4): we ship **no** copyrighted ROMs; the customer supplies their own. Removes the legal blocker. |
| **Original BASIC ROM scope** | 🟢 optional | Demoted from a gate to a nice-to-have (§4). Out-of-box experience uses our own self-contained games instead. |
| **Audio unproven** | 🔴 | Required for V1.0 (§3.5). Bench a reference on the real monitor before claiming it. |
| **Original game content** | 🟠 | The creative product (§1). We must author at least one polished, self-booting homebrew title for the out-of-box experience. |
| **HDMI TV compatibility** | 🟠 | The ~57 Hz off-spec timing is fine on tested monitors but a consumer-QA exposure when we promise "your favorite TV." Build a compatibility matrix; consider a more-standard-timing fallback. |
| **GPL (XRoar is GPLv3)** | 🟠 | Selling derived firmware is fine, but we **must distribute corresponding source** + license notices. Conveniently aligns with the "open & hackable" story. |
| **Game copyright** | 🟠 | Ship only freely-redistributable / homebrew / licensed titles on the SD. |
| **Hot-replug (PIZERO-11b)** | 🟡 | Promote from "parked" to a V1.0 reliability item; the battery/USB-C power config likely mitigates it — test, then fix or document. |
| **FCC/CE + trademark** | 🟡 | Retail HDMI products technically need emissions compliance (often handled via a maker/kit framing at low volume). Market as "runs CoCo software / compatible with," never implying official Tandy/RadioShack endorsement. |

---

## 8. Path to V1.0 (proposed issues)

To be filed in `issues.jsonl` before implementation (per project workflow):
- **PIZERO-19** — Boot manager / launcher (native UI, two-state model, enumeration)
- **PIZERO-20** — Runtime machine teardown/re-init (game → menu → game, no reboot)
- **PIZERO-21** — HDMI TV compatibility matrix + standard-timing fallback option
- **PIZERO-22** — Compliance & enablement: GPL source distribution, customer ROM-loading docs, game curation
- **PIZERO-23** — *(optional, deferred)* Original CoCo-compatible BASIC/monitor ROM — *large; not a V1.0 gate*
- **PIZERO-24** — Original homebrew game(s): the out-of-box creative content
- **PIZERO-25** — *(expansion)* DriveWire/FujiNet enablement + docs (needs add-on connectivity)
- **PIZERO-18** (existing) — Audio: reframed as a **V1.0 requirement**; run the bench de-risk first
- **PIZERO-13** (existing) — USB joystick: reframed as **V1.0**; header expansion documented
- **PIZERO-11b** (existing) — promote to a V1.0 reliability item (test, then fix/document)

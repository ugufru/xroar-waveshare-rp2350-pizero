# Case (PIZERO-94)

Two-part 3D-printed case for the Waveshare RP2350-PiZero.

- `pizero_case.scad` is the only source. Every dimension is a named parameter.
- `base.stl` and `top.stl` are rendered from it. Do not edit them by hand.

## Design

A shallow base tray and a tall lid, split at the top surface of the PCB.
Four M2.5 countersunk screws go in from underneath, pass through the floor
and through the board's own mounting holes, and thread into bosses inside
the lid. One set of screws holds the board down and closes the case.

Splitting at the PCB top surface means every connector opening becomes a
notch open at the bottom of the lid, so nothing has to bridge when printed.

Each front port is two cuts. A through opening sized to the connector body
plus `port_clr`, which is what you see from outside, and a shallow pocket
on the outer face sized to the plug overmould. The pocket leaves only
`port_frame_t` (0.8 mm) of wall in front of the socket, so a plug can seat
fully instead of bottoming out on a 2 mm wall. Set `plug_pocket = false`
to drop the pockets if you would rather have a plain flat face.

Current size: 69.8 x 34.8 x 20.6 mm.

## Rule: commit before printing

Every STL that gets sent to a printer must be committed first, so each
printed revision is recoverable and can be compared against the part in
hand. Render, commit, then open in the slicer. Never the other way round.

## Rendering

```
openscad -o base.stl -D 'part="base"' pizero_case.scad
openscad -o top.stl  -D 'part="top"'  pizero_case.scad
```

There is also a combined plate:

```
openscad -o plate.stl -D 'part="both"' pizero_case.scad
```

`plate.stl` holds both parts as two free-standing bodies with `part_gap`
(10 mm) of clear air between them, so a slicer sees two objects rather
than one fused lump. Total footprint 69.8 x 79.6 mm.

`part = "assembly"` shows the base, a mock PCB with header and connectors,
and a ghosted lid. Use it to eyeball port alignment. It is for looking at,
not for slicing.

## Printing

Both STLs come out already in print orientation, sitting flat on z=0 with
the open face up. The base is in its natural orientation; the lid is
flipped roof down, so the roof is the first layer, the walls grow upward,
and every port opening stays open to the air. No supports.

Fasteners: 4 x M2.5 countersunk, 12 mm, self-tapping into the lid bosses
(2.1 mm pilot).

## Dimension provenance

Board geometry is scaled off the Waveshare wiki dimension drawing against
its 65.00 mm callout, good to roughly +/- 0.2 mm. Those parameters are
marked `[W]` in the source.

Parameters marked `[M]` were corrected from a printed part on real
hardware, and are the most trustworthy of the three.

Parameters marked `[?]` are still estimates that need calipers on a real
board: PCB thickness, USB-C and microSD heights, and plug clearances.

## Port test coupon

```
openscad -o coupon.stl -D 'part="coupon"' pizero_case.scad
```

The lid's front wall only, 69.8 x 9.0 x 13.8 mm, with all three port
openings. Roughly ten minutes to print. Sit it on the assembled base and
try real plugs before committing to a full lid.

## Status

**Base**, printed 2026-09-11: board seats without force, walls rigid, hole
pattern correct. Validates the `[W]` parameters.

**Lid**, second print confirmed 2026-09-11: fits and closes cleanly.

**Roof vents**, added after that: two banks of four slots, 1 mm wide,
20 mm long, rounded ends, on the CoCo 2 pattern of slots cut across the
top with no fan. Banks at x 8..28 and 37..57, rows at y 6.6, 11.6, 16.6
and 21.6.

The rows are not chosen, they are anchored on the buttons. BOOT sits on
row 2 and RUN on row 4, and the pitch is half the 10 mm between them.
That gets rid of the two conspicuous round holes while keeping paperclip
access to both buttons, and it is why `vent_pitch` and `vent_y0` are
derived from `run_pos` and `boot_pos` rather than typed in. The source
echoes a warning if a change ever moves a button out from under a slot.

The left bank sits over the RP2350 itself (roughly x 19..29, y 14..24),
which is where the heat is.

**Lid**, first print 2026-09-11: shape and fit good, but 12.0 mm of
headroom fouled the header pins. Revision 2 changes:

| | was | now |
|---|---|---|
| mini-HDMI opening height | 4.5 | 3.5 |
| microSD slot width | 10.2 | 11.7 |
| microSD thumb dish | none | R10 x 1.2 mm deep |
| BAT opening height | 7.0 | 6.0 |
| BAT opening, low-y edge | 14.4 | 15.4 |

Case height stays at 20.6 mm.

The first lid fouled on assembly. That initially read as the 40-pin header
and `head_room` went to 13.0, but the real obstruction was the microSD
card, so the height is back to 12.0 and the fix lives in the microSD
opening instead. Keeping the closed case as low as it will go.

Note that the base wall rim sits exactly at the PCB top surface, so with a
board seated the rim is a z-zero datum you can measure connector heights
against.

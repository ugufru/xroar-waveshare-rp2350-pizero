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

**Recessed vent band**, on the CoCo 2 pattern (see `coco2-image.png`):
two banks of four slots in a panel recessed 2 mm below the top. Slots are
3.0 mm across on a 6.0 mm row pitch, so 3.0 mm ribs.

Layout: 6.0 mm gap between the two banks. Every slot is 20.8 mm long,
running 8.7 to 29.5 on the left and 35.5 to 56.3 on the right. Rows sit
at y 4.6, 10.6, 16.6 and 22.6. The recess runs out over the left and right edges,
cutting the tops of the side walls, and is 3.0 mm of border clear of the
slots front and back. Flanks 3.9 mm. Case height 20.6 -> 22.6, exactly
the recess depth. The roof under the band stays 1.8 mm.

Slot ends stop beside the corner screw bosses rather than crossing them.
Only rows 1 and 4 actually come near a boss, but with `vent_uniform` every
row is cut back to whatever the worst row needs, so the grille stays a
rectangle. That worst case is computed from the boss positions via
`row_lo`/`row_hi`, not typed in, so it tracks if anything moves. The cost
is 5.8 mm off each end of rows 2 and 3, which need none of it: set
`vent_uniform = false` for the stepped grille the real machine has, where
only the outer rows shorten.

**Both buttons are reachable.** Row 2 would land exactly on BOOT, but RUN
sits 10 mm behind it and the pitch is 6, so RUN would fall 2 mm off row 4
with only 0.25 mm of its plunger under open slot: technically not blocked,
practically not pressable. `vent_shift = -1.0` nudges the whole group 1 mm
forward, putting both buttons 1 mm off a centreline with 1.25 mm of each
plunger open. The window that catches both is -1.5 to -0.5, so -1.0 is the
middle of it.

The band does not move with the shift. It stays centred on the case, so
the case reads symmetric from outside and the asymmetry goes inside the
recess instead: 1.6 mm of border in front of the grille, 4.4 mm behind.

The model echoes which buttons are covered rather than leaving it to
chance, so a later change to pitch, shift or slot width will say if it
breaks this.

**Printing note.** Because the recess crosses the side walls, the roof's
first layer is two separate strips 3.9 mm wide with a 27 mm span between
them, so this will want support on the top face. Setting
`band_over_edges = false` closes the recess into a rounded panel inset
from the sides, which joins the flanks into a ring and removes the need.

**Roof vents**, first version: two banks of four slots, 1 mm wide,
20 mm long, rounded ends. Banks at x 8..28 and 37..57, rows at y 6.6, 11.6, 16.6
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

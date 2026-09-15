# Case

A two-part 3D-printed case for the Waveshare RP2350-PiZero, styled after the
ventilated top of a Tandy Color Computer 2. Confirmed on hardware.

|  |  |
|---|---|
| Size | 69.8 x 34.8 x 20.6 mm |
| Parts | `base.stl`, `top.stl` (or `plate.stl` for both at once) |
| Fasteners | 4 x M2.5 countersunk, 12 mm, self-tapping into the lid |
| Supports | None |
| Material | Any. Roughly 20 g, about 1 to 2 hours |
| Openings | mini-HDMI, both USB-C, microSD, battery connector, RUN and BOOT |

- `pizero_case.scad` is the only source. Every dimension is a named parameter.
- The STLs are rendered from it. Do not edit them by hand.

## Printing it

Load `base.stl` and `top.stl`, slice, print. Both arrive already flat on z=0
in the orientation they need, so do not rotate them, and do not let the
slicer add supports.

Then drop the board into the base, sit the lid on top, and drive four
M2.5 x 12 mm countersunk screws up through the underside. They pass through
the floor and the board's own mounting holes and thread straight into the
plastic bosses in the lid, so one set of screws clamps the board and closes
the case. No inserts or nuts needed; 12 mm gives 5.2 mm of thread engagement
with 1.8 mm to spare before bottoming out.

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

## How it was designed

The whole case is code. `pizero_case.scad` is an OpenSCAD program, and the
STLs are rendered from it by a command line, never drawn by hand. That choice
shaped everything else: a change like "move the vents 1 mm forward" is one
number, and the consequences of it recompute rather than needing to be
re-drawn.

**The board geometry came from Waveshare's dimension drawing, not from
calipers.** The drawing is a top-down photograph with a 65.00 mm callout
across the board. Measuring it in pixels and scaling against that callout
gives every port position, the mounting-hole pattern and the header extents
to roughly +/- 0.2 mm, which is well inside printer tolerance. Those values
are marked `[W]` in the source. The base printed first time and the board
dropped in without forcing.

**Constraints are derived, not typed.** The interesting parameters are
expressions over other parameters, so the design stays correct when something
moves:

- `vent_pitch` and `vent_y0` come from `run_pos` and `boot_pos`, so the vent
  rows stay lined up with the RUN and BOOT buttons by construction. A
  render-time check echoes a warning if a change ever moves a button out from
  under a slot.
- `row_lo` and `row_hi` compute how far each slot must stop short of the
  corner screw bosses, from the boss positions and diameter. Nobody typed
  "8.7 mm"; it falls out, and it would follow a boss that moved.
- The recess, when enabled, is derived from the slot group rather than from
  the case, so its border is exactly `band_margin` whatever else changes.

**The split line is the load-bearing decision.** The case divides at the top
surface of the PCB. Because every connector sits on top of the board, that
makes each port opening a notch open at the bottom of the lid, so nothing has
to bridge. It also means the base wall rim is exactly at PCB height, which
makes it a z-zero datum you can measure connector heights against with the
board in place.

**Port openings are sized for the plug, not the socket.** The sockets sit
flush with the board edge, and a USB-C plug's metal shell protrudes about as
far as the socket is deep, so a full 2 mm wall in front of one would stop it
seating. Each front port is therefore two cuts: a tight through opening sized
to the connector, and a shallow pocket on the outer face sized to the plug
overmould, leaving 0.8 mm of wall in front of the socket.

**It took four prints and fourteen revisions**, in one evening. The loop was
print, fit, measure, change one parameter, re-render. Two things it taught
that no amount of modelling would have:

- The first lid fouled on assembly. It looked like the 40-pin header, and the
  case grew 1 mm to clear it. The real obstruction was the microSD card, so
  the height came back off and the fix went into the microSD opening instead.
- A 2 mm recessed panel across the top, the closest match to the real CoCo 2,
  does not print. Roof-down, its floor has to bridge 27 mm between two strips
  of first layer, and running it edge to edge cuts the side walls so those
  strips are not even joined. It is replaced by two 0.6 mm grooves that read
  almost the same and cost nothing. `band = true` brings the recess back for
  a process that can make it.

Every printed revision is a commit, so any part in hand maps to a recoverable
version. See the rule below.

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

**Style pass (PIZERO-101)**, rendered 2026-09-14, not yet printed:

- **Floor vents** in the base: the lid's slot (3.0 mm, 3.0 mm rib, 6.0 mm
  pitch, rounded ends) turned to run front to back. Two banks of four with
  the same 6.0 mm gap, centred on the board at x 8.5 to 56.5, each slot
  y 2.6 to 27.4. They clear the corner standoffs by 2.0 mm. Through the
  floor, so nothing bridges. `base_vents = false` drops them.
- **Corners** 4 to 5 mm (`r_out`). The cavity corner `r_in` is now its own
  2.0 mm parameter instead of `r_out - wall`, so the outside can be
  restyled without touching board fit.
- **Top edge rounded** to 5.0 mm (`top_r`, rev 2), the same as the corners,
  so each lid corner is a sphere. The old 1 mm chamfer came back as
  `top_r = 0`. A 5 mm round alone breaks through a 2 mm wall, so the cavity's
  top 3 mm is rounded too, about the same centres, keeping the shell 1.8 to
  2.0 mm thick over the round. Below that the cavity keeps its 2.0 mm
  corners, so board fit is unchanged; the header loses at most 0.14 mm of
  its 0.5 mm roof clearance. The corner bosses are clipped to the cavity so
  they do not poke out through the round.
- **Port corners** rounded: `port_r` 1.0 mm on the top corners of each
  through opening (the bottom is open at the split line), `pocket_r`
  2.0 mm on all four corners of each plug pocket, split across base and lid.
- **Groove loop.** Grooves widened from 0.5 to 2.0 mm (still 0.6 deep) and
  joined into one closed loop with 3.0 mm corners (`groove_r`, on the
  centreline). The side legs run front to back at x 1.1 and 63.9, 1.5 mm
  inside the top chamfer, the same margin the front groove has. This fixes
  the front groove running out through the side wall right where the
  corner curve starts. `groove_loop = false` gives straight grooves again.
- **Groove wrap** (rev 2, `groove_wrap`, supersedes the loop). After seeing
  the loop, the legs moved off the top: the front and back grooves run
  across the top, over the rounded edge and down both side walls, where a
  front-to-back leg at z 14.6 joins them with 3.0 mm corners on the side
  face. The leg's top edge meets the start of the side round, and its lower
  edge clears the battery opening by 1.3 mm (0.8 before rev 3). It is cut as
  a 0.6 mm skin that follows the outside surface, so depth stays constant
  over the round.
- **Grille centred, three rows** (rev 4). The 5 mm rounds leave 24.8 mm of
  flat roof front to back, and four rows plus two grooves need 25 mm, so
  the front groove sat on the round. Now three 3.0 mm slots on a 5.0 mm
  pitch (2.0 mm ribs, `vent_rows`, `vent_rib`), centred on the case at
  y 10, 15 and 20. The grooves follow at y 6.5 and 23.5, both on the flat
  roof, 7.9 mm from the front and back edges. Slots keep their 20.8 mm
  length (`roof_vent_margin`). The base floor vents keep their own four rows
  on 3.0 mm ribs (`base_vent_rows`, `base_vent_rib`) and are unchanged.
  **Buttons:** no pitch can centre both, because centred rows are symmetric
  about y 15 and the buttons are not (BOOT 3.4 mm in front, RUN 6.6 mm
  behind). The 5 mm pitch treats them equally: each is 1.6 mm off a row,
  with about 0.65 mm of plunger under open slot (was 1.25). Needs a
  paperclip test on the print.
- **Finer grille, 1 mm grooves** (rev 5). Slots 3.0 to 1.0 mm wide so more
  fit, five rows instead of three. The pitch is a third of the 10 mm
  between RUN and BOOT (3.33 mm, a 2.33 mm rib, derived in `vent_rib`), so
  with five rows centred on the case, rows 2 and 5 land within 0.07 mm of
  BOOT and RUN: 1.18 mm of each 1.5 mm plunger under open slot. Rows at
  y 8.33, 11.67, 15.0, 18.33, 21.67, still 20.8 mm long. Grooves back to
  1.0 mm wide, at y 5.83 and 24.17 (7.7 mm from the front and back edges);
  the side leg keeps its top edge at the start of the round, so its
  centreline is now z 15.1. The base floor vents keep their own 3.0 mm
  slot (`base_vent_w`) and are unchanged. A 1 mm printed slot comes out
  nearer 0.8 mm, so a paperclip is a tight fit.
- **Floor vents match the roof** (rev 6). `base_vent_w` and `base_vent_rib`
  now follow the roof's 1.0 mm slot and 3.33 mm pitch. Seven slots per bank
  span exactly the 21 mm the old four 3 mm slots did, so the footprint
  (x 8.5 to 56.5, 2.0 mm clear of the standoffs) is unchanged.
- **Crisp top edge, groove hugs the grille** (rev 7). The rev 6 print did
  not like the 5 mm top round: roof down, its first layers overhang the
  bed. `top_r = 0` restores the 1 mm chamfer; the 5 mm vertical corners
  stay. The groove no longer wraps down the sides (`groove_wrap = false`).
  It is a closed 1.0 mm loop on the roof, 3.0 mm clear of the slots on all
  four sides (`groove_gap`, `groove_hug`): centreline x 5.2 to 59.8,
  y 4.33 to 25.67, corners 3.5 mm on the centreline so they are concentric
  with a sharp grille corner.
- **Bottom chamfer** (rev 8). The base's bottom edge gets the same 1.0 mm
  chamfer as the lid's top edge (`bottom_chamfer`), so the case has no
  sharp horizontal edge top or bottom. It sits on the bed and narrows
  inward, so it adds no overhang, and it takes the edge off any first-layer
  squish. It stays clear of the countersinks and plug pockets.

The grille and button notes below describe revs 1 to 3. Rev 4 above
centres the grille on three rows and replaces the button anchoring.

**Vent grille**, on the CoCo 2 pattern (see `coco2-image.png`): two banks
of four slots, 3.0 mm across on a 6.0 mm row pitch, so 3.0 mm ribs. Rows
at y 4.6, 10.6, 16.6 and 22.6. Every slot is 20.8 mm long, running 8.7 to
29.5 on the left and 35.5 to 56.3 on the right, with a 6.0 mm gap between
the banks.

**Grooves, not a recess.** The banded look was two 0.5 mm grooves, 0.6 mm
deep, at y 1.1 and 26.1, running the full width across the side walls
(now a 2.0 mm loop, see the style pass above).
0.6 mm is an exact three layers at 0.2 mm, so the depth does not quantise
to something shallower than intended.
They replace a 2 mm recessed panel that **failed to print**: roof-down its
floor had to bridge 27 mm between two strips of first layer, and because
it ran edge to edge and cut the side walls, those strips were not even
joined to each other. A 0.5 mm groove is a 0.5 mm bridge, so this costs
nothing. Dropping the recess also takes the case from 22.6 mm back to
20.6, since the roof no longer has to carry the recess depth. `band =
true` restores the recess if it is ever wanted for a different process.

Slot ends stop beside the corner screw bosses rather than crossing them.
Only rows 1 and 4 actually come near a boss, but with `vent_uniform` every
row is cut back to whatever the worst row needs, so the grille stays a
rectangle. That worst case is computed from the boss positions via
`row_lo`/`row_hi`, not typed in, so it tracks if anything moves. Set
`vent_uniform = false` for the stepped grille the real machine has.

**Both buttons are reachable.** Row 2 would land exactly on BOOT, but RUN
sits 10 mm behind it and the pitch is 6, so RUN would fall 2 mm off row 4
with only 0.25 mm of its plunger under open slot. `vent_shift = -1.0`
nudges the whole group 1 mm forward, putting both buttons 1 mm off a
centreline with 1.25 mm of each plunger open.

That is the best a 6 mm pitch allows: a row exactly on BOOT throws RUN
2 mm off, and vice versa, since only a pitch dividing the 10 mm between
the buttons can centre both. `vent_rib = 2.0`, a 5 mm pitch, would centre
both at the cost of the 3 mm rib spacing. The model echoes which buttons
are covered, so a later change to pitch, shift or slot width will say if
it breaks this.

The grille sits where the buttons put it, not where the case centre is, so
it is off-centre front to back: 3.5 mm from the front edge to the first
groove, 6.3 mm from the last groove to the back. Deliberate.

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

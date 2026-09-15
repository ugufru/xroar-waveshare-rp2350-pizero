// pizero_case.scad -- PIZERO-94
// Two-part 3D-printed case for the Waveshare RP2350-PiZero running XRoar.
//
//   base  : shallow tray, PCB sits on four standoffs
//   top   : tall lid, clears the 40-pin header, carries every port opening
//   Four M2.5 countersunk screws enter from UNDERNEATH, pass through the
//   floor and the PCB's own mounting holes, and thread into bosses in the
//   lid. One set of screws holds the board and closes the case.
//
// Render (both land flat on z=0, ready to slice):
//   openscad -o base.stl -D 'part="base"' pizero_case.scad
//   openscad -o top.stl  -D 'part="top"'  pizero_case.scad
//
// Coordinate system: origin at the PCB's bottom-left corner, looking down on
// the component side. x runs 0..65 left to right. y runs 0..30 with y=0 at
// the CONNECTOR edge (mini-HDMI and both USB-C) and y=30 at the header edge.
// z=0 is the underside of the PCB, so the PCB top is z = pcb_t.
//
// Dimensions marked [W] come from the Waveshare wiki dimension drawing,
// scaled against its 65.00 mm callout: good to about +/- 0.2 mm.
// Dimensions marked [?] are estimates awaiting caliper measurement.

part = "both";          // "base" | "top" | "both" | "assembly" | "coupon"

/* ---------- board ---------------------------------------------------- */

bw  = 65.0;             // [W] board width
bd  = 30.0;             // [W] board depth
pcb_t = 1.6;            // [?] PCB thickness

hole_d  = 3.0;          // [W] mounting hole diameter
hole_in = 3.5;          // [W] hole centre inset from each edge (Pi Zero pattern)

/* ---------- shell ---------------------------------------------------- */

wall    = 2.0;          // side wall thickness
floor_t = 2.6;          // base floor (thick enough to countersink a screw head)
base_roof_t = 1.8;      // lid roof where there is no recessed band
clr     = 0.4;          // clearance between PCB edge and inner wall, per side

standoff_h = 2.6;       // PCB underside to floor: clears bottom-side solder
head_room  = 12.0;      // [M] PCB top to lid roof. The first lid print
                        //     fouled, but on the microSD card, not the
                        //     header, so the height stays at 12.0 and the
                        //     fix is in the microSD opening below. A 2.54 mm
                        //     male header stands ~11.5 mm proud of the board.

r_out = 5.0;            // vertical corner radius, outside (PIZERO-101, was 4)
r_in  = 2.0;            // inner cavity corner. Fixed rather than r_out - wall,
                        // so restyling the outside never changes board fit;
                        // the corners just get thicker.
top_chamfer = 1.0;      // chamfer on the top edge, used only when top_r = 0
// Round between the side walls and the roof (PIZERO-101 rev 2). Equal to
// r_out, so every outer corner of the lid is a sphere. A 5 mm round on a
// 2 mm wall would break through, so the cavity's top edges are rounded too,
// by top_r - wall about nearly the same centres, keeping the shell ~2 mm.
top_r = 5.0;
assert(top_r == 0 || top_r == r_out, "top_r must be 0 or equal r_out");

part_gap = 10.0;        // clear space between the two parts on a shared plate
coupon_depth = 9.0;     // depth of the port test coupon, front wall only

/* ---------- fasteners ------------------------------------------------ */

screw_clear_d = 2.8;    // M2.5 shank clearance
screw_head_d  = 5.0;    // M2.5 countersunk head
screw_head_h  = 1.5;    // countersink depth
boss_d        = 6.0;    // lid boss outside diameter
pilot_depth   = 7.0;    // threaded depth in the lid boss
pilot_d       = 2.1;    // self-tapping pilot for M2.5 into PLA/PETG
                        // -> use M2.5 x 12 mm countersunk screws

/* ---------- connectors ----------------------------------------------- */
//
// Each front port is TWO cuts:
//
//   1. a through opening sized to the CONNECTOR plus `port_clr`, which is
//      what you actually see from the outside;
//   2. a shallow pocket on the outer face sized to the PLUG overmould,
//      which leaves only `port_frame_t` of wall in front of the socket so a
//      plug can seat fully. Set plug_pocket = false to drop it, at the risk
//      of plugs bottoming out on a 2 mm wall.

port_clr     = 0.5;     // clearance around the connector body, per side
plug_pocket  = true;
port_frame_t = 0.8;     // wall left in front of the socket, inside the pocket
plug_clr     = 0.3;     // clearance around the plug overmould, per side
port_r       = 1.0;     // corner radius of the through opening (top corners;
                        // the bottom is open at the split line)
pocket_r     = 2.0;     // corner radius of the plug pocket, all four corners

// [ centre_x, body_w, body_h, plug_w, plug_h ]
hdmi = [12.35, 12.1, 3.0, 15.5, 7.0];   // [W] centre, [M] 3.0 body height
usbc_host  = [39.1, 9.6, 3.3, 12.5, 7.0];   // PIO-USB host port (keyboard)
usbc_power = [53.6, 9.6, 3.3, 12.5, 7.0];   // power / programming port
front_ports = [hdmi, usbc_host, usbc_power];

sd_y0 =  9.8;           // [W] microSD slot, from the connector edge,
sd_y1 = 21.5;           //     opened out 1.0 mm total after the first print
sd_oh = 2.5;            // [?] height above the PCB top

// Thumb dish around the microSD slot, so a card edge can be pinched and
// pulled. Cut from both halves, so it straddles the split line.
sd_scoop   = true;
sd_scoop_r = 10.0;      // dish radius: shallow and wide, not a deep pocket
sd_scoop_d = 1.2;       // depth into the wall, same as the plug pockets
sd_scoop_z = 0.8;       // centre height above the split line

// 2-pin battery connector. Nothing in this project drives it, but it stays
// reachable. Trimmed after the first lid print: 1 mm off the top and 1 mm
// off the low-y edge, which is the left one seen from outside that wall.
// Tightened again in PIZERO-101 rev 3: 1 mm off the high-y (right, seen
// from outside) edge and 0.5 mm off the top.
bat_open = true;
bat_y0 = 15.4;          // [W] 14.4, pulled in 1.0
bat_y1 = 22.6;          // [W] 23.6, pulled in 1.0
bat_oh = 5.5;           // [M] was 7.0, then 6.0

run_pos  = [41.0, 21.6];   // [W]
boot_pos = [41.2, 11.6];   // [W]

// Bare round holes over RUN and BOOT read as holes. The vent grille below
// swallows them: two of its rows sit exactly on the two buttons, so the
// access is there but nothing on the lid looks like an access hole.
buttons_open = false;
button_hole_d = 3.0;

/* ---------- roof vents ------------------------------------------------ */
//
// The CoCo 2 had no fan, just slots cut across the top. Same idea here.
// Two banks of four slots, each slot 1 mm wide with rounded ends.
//
// Rows are anchored on the two buttons rather than chosen: BOOT sets the
// second row, RUN the fourth, and the pitch is the 10 mm between them
// halved. The left bank is the right bank mirrored about the board centre.

vents        = true;
vent_w       = 3.0;     // slot width, front to back       [C] measured
vent_rib     = 3.0;     // solid between slots, for a 6.0 mm row pitch
vent_rows    = 4;
vent_gap     = 6.0;     // clear space between the two banks, left to right
vent_margin  = 5.0;     // slots stop this far from the left and right edges
// Slot ends stop beside the corner screw bosses rather than running over
// them. With vent_uniform every row is cut back to whatever the worst row
// needs, so the grille stays a rectangle instead of stepping; set it false
// for the stepped look, where only the rows near a boss are shortened.
vent_clear_bosses = true;
vent_uniform      = true;
vent_boss_clr = 0.8;    // clear space between a slot and a screw boss

// Floor vents in the base (PIZERO-101): the same slot, rib and bank gap as
// the lid, but running front to back, centred on the board. Each slot runs
// the case depth less vent_margin at each end. The banks land well inside
// the corner standoffs, so no boss keepout is needed; the echo below
// reports the clearance.
base_vents = true;

vent_pitch   = vent_w + vent_rib;
vent_span    = vent_rows*vent_w + (vent_rows - 1)*vent_rib;

// Row 2 lands on BOOT. RUN then lands on a row only if the 10 mm between
// the buttons is a whole number of pitches, which is why a 5 mm pitch
// keeps both buttons reachable and the CoCo's 7 mm pitch does not. The
// echo below says so out loud rather than letting it pass unnoticed.
// Row 2 would land exactly on BOOT, but RUN sits 10 mm behind it and the
// pitch is 6, so RUN would fall 2 mm off row 4 with only 0.25 mm of its
// plunger under open slot. Nudging the whole group 1 mm forward puts both
// buttons 1 mm off a centreline instead, leaving 1.25 mm of each plunger
// open. The window that catches both is -1.5 .. -0.5, so -1.0 is the
// middle of it. The band deliberately does NOT move with this: it stays
// centred on the case, so the border goes 1.6 mm front and 4.4 mm back.
vent_shift   = -1.0;
vent_y0      = boot_pos[1] - vent_pitch + vent_shift;

// (bank extents and boss keepout are derived below, after x0/ow/holes)

// The 2 mm recess did not print. Roof-down, its floor had to bridge 27 mm
// between two strips of first layer, and because it ran edge to edge and
// cut the side walls those strips were not even joined to each other.
// Replaced by a pair of shallow grooves, which give the same banded look
// for nothing: a 0.5 mm groove is a 0.5 mm bridge. Dropping the recess
// also takes the case back from 22.6 mm to 20.6, since the roof no longer
// has to carry its depth. Set true to get the recess back.
band         = false;
band_margin  = 3.0;     // solid recess border around the vent group
band_r       = 3.0;     // corner radius of the recess
// The recess runs out over the left and right edges, cutting the tops of
// the side walls, rather than closing into a panel. Its corners then fall
// outside the part, so band_r no longer shows; it applies again if this
// goes false.
band_over_edges = true;

// Decorative grooves in place of the recess, one in front of the grille
// and one behind, each centred groove_off clear of the outer edge of the
// slots and running the full width across the side walls.
groove       = true;
groove_w     = 2.0;     // PIZERO-101, was 0.5. Still only a 2 mm bridge.
groove_d     = 0.6;     // an exact 3 layers at 0.2 mm, so it reads cleanly
groove_off   = 2.0;     // groove centreline, clear of the outer edge of the slots
// Straight grooves ran out through the side walls, and the front one left
// right where the corner curve starts. As a loop, the front and back
// grooves are joined by front-to-back legs near the left and right edges,
// with rounded corners, so the groove outlines the whole top instead.
groove_loop  = true;
groove_edge  = 1.5;     // solid between the top chamfer and the outer edge
                        // of a side leg (matches the front groove's margin)
groove_r     = 3.0;     // loop corner radius, on the groove centreline
// Wrapped loop (PIZERO-101 rev 2), supersedes groove_loop: the front and
// back grooves run across the top, over the rounded edge and down the left
// and right sides, where a front-to-back leg joins them. The loop's rounded
// corners are on the side walls. The leg's top edge meets the start of the
// side round; the echo reports how much wall that leaves above the battery
// opening, the tallest cut in either side wall.
groove_wrap  = true;
// The recess is defined by the slots, not by the case: band_margin of
// border in front of the first row and behind the last, and wherever the
// grille has to sit for the buttons is where the whole thing sits. On this
// board that leaves the recess off-centre front to back, by design.
band_depth   = 2.0;     // how far below the top surface. The roof under the
                        // band stays base_roof_t, so the case grows by
                        // exactly this much.

$fn = 64;

// The whole point of anchoring rows on the buttons is that a paperclip can
// still reach RUN and BOOT through a vent slot. Shout if a change breaks it.
module _check_button(name, p) {
    covered = [for (b = vent_banks, r = [0 : vent_rows - 1])
                 let (cy = vent_y0 + r*vent_pitch)
                 if (abs(p[1] - cy) <= vent_w/2
                     && p[0] >= max(b[0], slot_lo(cy))
                     && p[0] <= min(b[1], slot_hi(cy)))
                 1];
    if (vents && len(covered) == 0)
        echo(str("NOTE: ", name, " is not under a vent slot"));
}

/* ===================================================================== */

split_z = floor_t + standoff_h + pcb_t;   // base wall top = PCB top surface
base_h  = split_z;
top_t   = base_roof_t + (band ? band_depth : 0);
lid_h   = head_room + top_t;
case_h  = split_z + lid_h;

ow = bw + 2*clr + 2*wall;      // outer width
od = bd + 2*clr + 2*wall;      // outer depth
x0 = -(clr + wall);            // outer shell origin, in board coords
y0 = -(clr + wall);

pocket_d = wall - port_frame_t;

holes = [ for (hx = [hole_in, bw - hole_in], hy = [hole_in, bd - hole_in]) [hx, hy] ];

// rounded rectangle prism, corner at (px,py), size (w,d,h), radius r
module rrect(px, py, w, d, h, r) {
    hull() for (dx = [r, w - r], dy = [r, d - r])
        translate([px + dx, py + dy, 0]) cylinder(h = h, r = r);
}

module outer_shell(h)  { rrect(x0, y0, ow, od, h, r_out); }
module inner_cavity(h) { rrect(-clr, -clr, bw + 2*clr, bd + 2*clr, h, r_in); }

// rounded rectangle prism whose top edges are also rounded to r: vertical
// cylinders up to h - r, capped by a sphere at each corner
module rrect_rtop(px, py, w, d, h, r) {
    hull() for (dx = [r, w - r], dy = [r, d - r]) {
        translate([px + dx, py + dy, 0]) cylinder(h = h - r, r = r);
        translate([px + dx, py + dy, h - r]) sphere(r = r);
    }
}

// lid outside and cavity, in lid-local z (0 = split line)
module lid_outer(h) {
    if (top_r > 0) rrect_rtop(x0, y0, ow, od, h, r_out);
    else outer_shell(h);
}
// The cavity keeps its r_in corners wherever the board is, and only its
// top ri blends into spheres of radius top_r - wall. Those spheres share the
// outer spheres' x/y centres and sit (top_t - wall) higher, so the shell over
// the round is between top_t and wall thick.
module lid_cavity(h) {
    ri = top_r - wall;
    if (top_r > 0)
        hull() {
            inner_cavity(h - ri);
            for (dx = [ri, bw + 2*clr - ri], dy = [ri, bd + 2*clr - ri])
                translate([-clr + dx, -clr + dy, h - ri]) sphere(r = ri);
        }
    else inner_cavity(h);
}

/* ---------- front port cuts ------------------------------------------ */
//
// z_lo/z_hi bound the cut, so the same geometry serves the lid (above the
// split line) and the base relief (below it).

// Prism running +y from y_at for len, with an x/z face that spans x cx +/- w/2
// and z zb..zt. The top corners are rounded to r, and the bottom ones too
// if round_bottom; otherwise the bottom is square.
module xz_rrect(cx, w, zb, zt, y_at, len, r, round_bottom) {
    rr = min(r, w/2, (zt - zb)/2);
    hull() for (x = [cx - w/2 + rr, cx + w/2 - rr]) {
        translate([x, y_at, zt - rr]) rotate([-90, 0, 0]) cylinder(r = rr, h = len);
        if (round_bottom)
            translate([x, y_at, zb + rr]) rotate([-90, 0, 0]) cylinder(r = rr, h = len);
        else
            translate([x - rr, y_at, zb]) cube([2*rr, len, 0.01]);
    }
}

// Clip a cut to the slab z_lo..z_hi, so one shape can be split between parts.
module z_slab(z_lo, z_hi) {
    intersection() {
        children();
        translate([x0 - 1, y0 - 1, z_lo]) cube([ow + 2, od + 2, z_hi - z_lo]);
    }
}

module port_through(p, z_lo, z_hi) {
    w = p[1] + 2*port_clr;
    zb = split_z - 0.01;                 // connector sits on the PCB top
    zt = split_z + p[2] + port_clr;
    if (min(z_hi, zt) > max(z_lo, zb))
        z_slab(z_lo, z_hi)
            xz_rrect(p[0], w, zb, zt, y0 - 1, wall + clr + 2, port_r, false);
}

module port_pocket(p, z_lo, z_hi) {
    w  = p[3] + 2*plug_clr;
    h  = p[4] + 2*plug_clr;
    cz = split_z + p[2]/2;               // plug is centred on the socket
    if (plug_pocket && min(z_hi, cz + h/2) > max(z_lo, cz - h/2))
        z_slab(z_lo, z_hi)
            xz_rrect(p[0], w, cz - h/2, cz + h/2, y0 - 0.01, pocket_d + 0.01,
                     pocket_r, true);
}

// Banks run from vent_margin off each outer edge in to the centre gap.
vent_x_lo = x0 + vent_margin;
vent_x_hi = x0 + ow - vent_margin;
vent_banks = [ [vent_x_lo,             bw/2 - vent_gap/2],
               [bw/2 + vent_gap/2,     vent_x_hi] ];

// Rows 1 and 4 run straight through the corner screw bosses at this
// length, so each row's ends are pulled back clear of any boss it would
// otherwise breach. Outer rows come out shorter than inner ones, which is
// the stepped look the real CoCo 2 grille has.
vent_keepout = boss_d/2 + vent_w/2 + vent_boss_clr;
function boss_push(cy, by) =
    (abs(cy - by) >= vent_keepout) ? 0
    : sqrt(vent_keepout*vent_keepout - (cy - by)*(cy - by));
function row_lo(cy) = !vent_clear_bosses ? vent_x_lo : max([vent_x_lo,
    for (h = holes) if (h[0] < bw/2 && abs(cy - h[1]) < vent_keepout)
        h[0] + boss_push(cy, h[1])]);
function row_hi(cy) = !vent_clear_bosses ? vent_x_hi : min([vent_x_hi,
    for (h = holes) if (h[0] > bw/2 && abs(cy - h[1]) < vent_keepout)
        h[0] - boss_push(cy, h[1])]);

// One slot: a stadium between lo and hi, so the ends are rounded.
module vent_slot(lo, hi, cy) {
    if (hi - lo >= vent_w)
        hull() for (x = [lo + vent_w/2, hi - vent_w/2])
            translate([x, cy, 0]) cylinder(d = vent_w, h = top_t + 0.08);
}

// Worst case across every row, so all rows can share one length.
vent_lo_all = max([for (r = [0 : vent_rows - 1]) row_lo(vent_y0 + r*vent_pitch)]);
vent_hi_all = min([for (r = [0 : vent_rows - 1]) row_hi(vent_y0 + r*vent_pitch)]);

function slot_lo(cy) = vent_uniform ? vent_lo_all : row_lo(cy);
function slot_hi(cy) = vent_uniform ? vent_hi_all : row_hi(cy);

module vent_cuts() {
    if (vents)
        translate([0, 0, split_z + head_room - 0.02])
            for (b = vent_banks, r = [0 : vent_rows - 1])
                let (cy = vent_y0 + r*vent_pitch)
                    vent_slot(max(b[0], slot_lo(cy)),
                              min(b[1], slot_hi(cy)), cy);
}

// The recess is a closed rounded panel now, not a channel running off both
// edges. That keeps the flanks joined into a ring, which is also what lets
// the roof print without support: layer one is one island, not two.
vent_group_y0 = vent_y0 - vent_w/2;
vent_group_y1 = vent_y0 + (vent_rows - 1)*vent_pitch + vent_w/2;

band_y0 = vent_group_y0 - band_margin;
band_y1 = vent_group_y1 + band_margin;
band_x0 = band_over_edges ? x0 - band_r - 1      : vent_x_lo - band_margin;
band_x1 = band_over_edges ? x0 + ow + band_r + 1 : vent_x_hi + band_margin;

groove_yf = vent_group_y0 - groove_off;    // front groove centreline
groove_yb = vent_group_y1 + groove_off;    // back groove centreline
groove_xl = x0 + top_chamfer + groove_edge + groove_w/2;   // left leg
groove_xr = x0 + ow - top_chamfer - groove_edge - groove_w/2;

groove_leg_z = case_h - top_r - groove_w/2;  // side leg centreline

// 2D stroke in the y/z plane: up the front groove, round the corner, along
// the leg, round the corner, up the back groove. The tops run above the case.
module groove_u_2d() {
    module rr(y_lo, z_lo, y_hi, z_hi, r)
        hull() for (y = [y_lo + r, y_hi - r], z = [z_lo + r, z_hi - r])
            translate([y, z]) circle(r = r);
    difference() {
        rr(groove_yf - groove_w/2, groove_leg_z - groove_w/2,
           groove_yb + groove_w/2, case_h + 10, groove_r + groove_w/2);
        rr(groove_yf + groove_w/2, groove_leg_z + groove_w/2,
           groove_yb - groove_w/2, case_h + 20, max(0.1, groove_r - groove_w/2));
    }
}

module groove_cuts() {
    if (groove && groove_wrap)
        // the stroke, extruded left to right, kept to a groove_d skin that
        // follows the outside surface, so it is cut normal to the round
        intersection() {
            translate([x0 - 1, 0, 0]) rotate([90, 0, 90])
                linear_extrude(ow + 2) groove_u_2d();
            difference() {
                translate([0, 0, split_z]) lid_outer(lid_h + 1);
                translate([0, 0, split_z - 1])
                    rrect_rtop(x0 + groove_d, y0 + groove_d, ow - 2*groove_d,
                               od - 2*groove_d, lid_h + 1 - groove_d, r_out - groove_d);
            }
        }
    else if (groove && groove_loop)
        translate([0, 0, case_h - groove_d]) difference() {
            rrect(groove_xl - groove_w/2, groove_yf - groove_w/2,
                  groove_xr - groove_xl + groove_w, groove_yb - groove_yf + groove_w,
                  groove_d + 1, groove_r + groove_w/2);
            translate([0, 0, -0.5])
                rrect(groove_xl + groove_w/2, groove_yf + groove_w/2,
                      groove_xr - groove_xl - groove_w, groove_yb - groove_yf - groove_w,
                      groove_d + 2, max(0.1, groove_r - groove_w/2));
        }
    else if (groove)
        for (cy = [groove_yf, groove_yb])
            translate([x0 - 1, cy - groove_w/2, case_h - groove_d])
                cube([ow + 2, groove_w, groove_d + 1]);
}

// Base floor vents: slot x centres for both banks, and the slot y extent.
base_vent_x = [ for (x_lo = [bw/2 - vent_gap/2 - vent_span, bw/2 + vent_gap/2],
                     r = [0 : vent_rows - 1])
                  x_lo + vent_w/2 + r*vent_pitch ];
base_vent_y0 = y0 + vent_margin;
base_vent_y1 = y0 + od - vent_margin;

module base_vent_cuts() {
    if (base_vents)
        for (cx = base_vent_x)
            hull() for (y = [base_vent_y0 + vent_w/2, base_vent_y1 - vent_w/2])
                translate([cx, y, -1]) cylinder(d = vent_w, h = floor_t + 2);
}

module band_cut() {
    if (band)
        translate([0, 0, case_h - band_depth])
            rrect(band_x0, band_y0, band_x1 - band_x0, band_y1 - band_y0,
                  band_depth + 1, band_r);
}

module sd_scoop_cut() {
    if (sd_scoop)
        translate([x0 - sd_scoop_r + sd_scoop_d,
                   (sd_y0 + sd_y1)/2,
                   split_z + sd_scoop_z])
            sphere(r = sd_scoop_r);
}

/* ---------- base ----------------------------------------------------- */

module base() {
    difference() {
        union() {
            difference() {
                outer_shell(base_h);
                translate([0, 0, floor_t]) inner_cavity(base_h);
            }
            for (h = holes)
                translate([h[0], h[1], floor_t])
                    cylinder(d = boss_d, h = standoff_h);
        }
        // relief so a plug overmould clears the base wall
        for (p = front_ports) port_pocket(p, -1, split_z);
        sd_scoop_cut();
        base_vent_cuts();

        // screw clearance + countersink from below
        for (h = holes) {
            translate([h[0], h[1], -1])
                cylinder(d = screw_clear_d, h = base_h + 2);
            translate([h[0], h[1], -0.01])
                cylinder(d1 = screw_head_d, d2 = screw_clear_d, h = screw_head_h);
        }
    }
}

/* ---------- lid ------------------------------------------------------ */

module notch_left(y_lo, y_hi, h) {
    translate([x0 - 1, y_lo, split_z - 0.01])
        cube([wall + clr + 2, y_hi - y_lo, h + 0.01]);
}
module notch_right(y_lo, y_hi, h) {
    translate([bw - clr - 1, y_lo, split_z - 0.01])
        cube([wall + clr + 2, y_hi - y_lo, h + 0.01]);
}

module lid() {
    difference() {
        union() {
            translate([0, 0, split_z]) difference() {
                lid_outer(lid_h);
                translate([0, 0, -0.01]) lid_cavity(head_room + 0.01);
            }
            // bosses: full pillars from the PCB top up to the roof, so they
            // tie the roof down and clamp the board at the same time. Kept
            // inside the cavity, or their tops poke out through the top round.
            intersection() {
                for (h = holes)
                    translate([h[0], h[1], split_z])
                        cylinder(d = boss_d, h = head_room);
                translate([0, 0, split_z]) lid_cavity(head_room);
            }
        }
        for (h = holes)
            translate([h[0], h[1], split_z - 0.01])
                cylinder(d = pilot_d, h = pilot_depth + 0.01);

        for (p = front_ports) {
            port_through(p, split_z, case_h);
            port_pocket(p, split_z, case_h);
        }

        notch_left(sd_y0, sd_y1, sd_oh);
        sd_scoop_cut();
        if (bat_open) notch_right(bat_y0, bat_y1, bat_oh);

        band_cut();
        groove_cuts();
        vent_cuts();

        if (buttons_open) for (b = [run_pos, boot_pos])
            translate([b[0], b[1], split_z + head_room - 0.01])
                cylinder(d = button_hole_d, h = top_t + 0.02);

        // chamfer the top edge, when it is not rounded
        if (top_r == 0) translate([0, 0, case_h - top_chamfer])
            difference() {
                translate([x0 - 1, y0 - 1, -0.5])
                    cube([ow + 2, od + 2, top_chamfer + 0.52]);
                hull() {
                    // starts below the band so the cut never slices the lid
                    translate([0, 0, -0.5]) rrect(x0, y0, ow, od, 0.5, r_out);
                    translate([0, 0, top_chamfer])
                        rrect(x0 + top_chamfer, y0 + top_chamfer,
                              ow - 2*top_chamfer, od - 2*top_chamfer, 0.01,
                              max(0.1, r_out - top_chamfer));
                }
            }
    }
}

/* ---------- print orientation ---------------------------------------- */
//
// The base already sits open side up on z=0. The lid is flipped roof down,
// which is how it must print: the roof becomes the first layer, the walls
// grow upward, and every port opening stays open to the air.

module base_printable() { base(); }
// Flip about the footprint's own y centre line, not about y=0, so the lid
// keeps the same x/y bounding box as the base instead of landing mirrored
// somewhere else (which is what fused the two together on a shared plate).
module lid_printable()  {
    translate([0, 2*y0 + od, case_h]) rotate([180, 0, 0]) lid();
}

// Front wall only, with all three port openings. Prints in about ten
// minutes and drops onto the real base so plug seating can be tested for
// real before committing to the full lid.
module coupon_printable() {
    translate([0, 2*y0 + od, case_h]) rotate([180, 0, 0])
        intersection() {
            lid();
            translate([x0 - 1, y0 - 0.01, split_z - 1])
                cube([ow + 2, coupon_depth, lid_h + 2]);
        }
}

/* ---------- mock PCB, for the assembly view only --------------------- */

module mock_pcb() {
    color("#1b4a7a") difference() {
        cube([bw, bd, pcb_t]);
        for (h = holes) translate([h[0], h[1], -1]) cylinder(d = hole_d, h = pcb_t + 2);
    }
    color("#222") translate([6.8, 24.3, pcb_t]) cube([51.4, 4.9, 2.5]);
    color("#c0c0c0") translate([8.07, 25.2, pcb_t]) cube([48.9, 3.1, 11.5]);
    for (p = front_ports)
        color("#999") translate([p[0] - p[1]/2, -0.3, pcb_t]) cube([p[1], 7.2, p[2]]);
    color("#999") translate([-0.5, sd_y0, pcb_t]) cube([11.5, sd_y1 - sd_y0, 1.4]);
}

/* ---------- output --------------------------------------------------- */

echo(str("slots y ", vent_group_y0, " .. ", vent_group_y1,
         band ? str("  recess y ", band_y0, " .. ", band_y1) : "",
         groove ? str("  grooves at y ", vent_group_y0 - groove_off,
                      " and ", vent_group_y1 + groove_off) : "",
         "  roof ", top_t, " thick",
         "  case height ", case_h));
for (r = [0 : vent_rows - 1])
    let (cy = vent_y0 + r*vent_pitch)
        echo(str("row ", r + 1, " y ", cy,
                 "  left bank ", max(vent_banks[0][0], slot_lo(cy)),
                 " .. ", vent_banks[0][1],
                 "  right bank ", vent_banks[1][0],
                 " .. ", min(vent_banks[1][1], slot_hi(cy)),
                 "  len ", vent_banks[0][1] - max(vent_banks[0][0], slot_lo(cy))));

if (base_vents)
    echo(str("base vents x ", base_vent_x[0] - vent_w/2, " .. ",
             base_vent_x[len(base_vent_x) - 1] + vent_w/2,
             "  y ", base_vent_y0, " .. ", base_vent_y1,
             "  clear of standoffs by ",
             base_vent_x[0] - vent_w/2 - (hole_in + boss_d/2)));
if (groove && groove_wrap)
    echo(str("groove wraps at y ", groove_yf, " and ", groove_yb,
             "  side leg centreline z ", groove_leg_z,
             " (groove ", groove_leg_z - groove_w/2, " .. ", groove_leg_z + groove_w/2,
             ", side round starts at z ", case_h - top_r, ")",
             "  wall above battery opening ",
             groove_leg_z - groove_w/2 - (split_z + bat_oh)));
else if (groove && groove_loop)
    echo(str("groove loop centreline x ", groove_xl, " .. ", groove_xr,
             "  y ", groove_yf, " .. ", groove_yb));

_check_button("RUN", run_pos);
_check_button("BOOT", boot_pos);

if (part == "coupon") coupon_printable();
else if (part == "base") base_printable();
else if (part == "top") lid_printable();
else if (part == "both") {
    // one plate, two free-standing bodies, part_gap of clear air between them
    base_printable();
    translate([0, od + part_gap, 0]) lid_printable();
}
else if (part == "assembly") {
    base();
    translate([0, 0, floor_t + standoff_h]) mock_pcb();
    %lid();
}

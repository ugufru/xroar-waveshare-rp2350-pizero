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
head_room  = 10.0;      // [M] PCB top to lid roof. The first lid print
                        //     fouled, but on the microSD card, not the
                        //     header, and the fix was in the microSD opening
                        //     below. Was 12.0 on the ~11.5 mm figure for a
                        //     2.54 mm male header; the header on this board
                        //     measures 8.34 mm proud, so 10.0 clears it by
                        //     1.66 mm and takes 2 mm off the case.

r_out = 5.0;            // vertical corner radius, outside (PIZERO-101, was 4)
r_in  = 2.0;            // inner cavity corner. Fixed rather than r_out - wall,
                        // so restyling the outside never changes board fit;
                        // the corners just get thicker.
top_chamfer = 1.0;      // chamfer on the top edge, used only when top_r = 0
bottom_chamfer = 1.0;   // rev 8: same chamfer on the base's bottom edge. It
                        // sits on the bed and narrows inward, so no overhang.
// Round between the side walls and the roof (PIZERO-101 rev 2). Equal to
// r_out, so every outer corner of the lid is a sphere. A 5 mm round on a
// 2 mm wall would break through, so the cavity's top edges are rounded too,
// by top_r - wall about nearly the same centres, keeping the shell ~2 mm.
// Rev 7: back to 0 (the 1 mm chamfer). The 5 mm round printed badly, its
// first layers overhanging the bed with the lid roof down.
top_r = 0;
assert(top_r == 0 || top_r == r_out, "top_r must be 0 or equal r_out");

part_gap = 10.0;        // clear space between the two parts on a shared plate
coupon_depth = 9.0;     // depth of the port test coupon, front wall only

/* ---------- fasteners ------------------------------------------------ */

screw_clear_d = 2.8;    // M2.5 shank clearance
screw_head_d  = 5.0;    // M2.5 countersunk head
screw_head_h  = 1.1;    // countersink depth. PIZERO-96: was 1.5, a 72.5 degree
                        // cone that seated a DIN 965 head on its rim only.
                        // 1.1 over the 5.0 -> 2.8 taper is a true 90 degrees.
boss_d        = 6.0;    // lid boss outside diameter
pilot_depth   = 7.0;    // threaded depth in the lid boss
pilot_d       = 2.2;    // self-tapping pilot for M2.5 into PLA/PETG.
                        // PIZERO-124: was 2.1; the tighter hole wedged
                        // the thin boss wall apart before any other load.
                        // -> use M2.5 x 12 mm countersunk screws
// PIZERO-124: two lid bosses snapped off at the tip, cleanly along a layer.
// Each was a free-standing 6 mm column 0.9 mm clear of both corner walls,
// printed roof-down so every layer line runs across it. The web fills the
// corner between boss and walls, so the boss is part of the corner rather
// than a column standing in it. It stops boss_web_gap above the PCB in case
// anything sits near the board corners; the boss tip itself is unchanged.
boss_web     = true;
boss_web_gap = 1.0;     // clear space between the web and the PCB top

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
// PIZERO-123: every plug pocket, and the microSD relief, shares one top
// edge, the highest any pocket needs (the USB-C pockets, 5.45 mm above the
// split). Only tops are raised to it; no bottom edge moves, so the base is
// untouched and the HDMI pocket just gains 0.15 mm of headroom.
pocket_top_z = max([for (p = front_ports) p[2]/2 + (p[4] + 2*plug_clr)/2]);

// microSD (PIZERO-103). The socket is a XunPu TF-110 (schematic J1):
// housing 11.95 wide x 11.55 deep x 1.32 tall, the 11.00 mm card centred in
// it, card tail 3.90 mm out of the housing front when latched. Position
// measured off the Waveshare drawing: housing front at x 0.5, y 10.9..22.9.
// The old slot (y 9.8..21.5, 2.5 mm tall) sat 1.2 mm forward of the socket,
// so the card caught on its back edge, and was tall enough for the card to
// ride up over the 1.32 mm socket and drop inside the case.
sd_cy      = 16.9;      // [W] socket and card centreline
sd_front_x = 1.5;       // [M] housing front edge, the card entry side. [W]
                        // said 0.5, but on the print a latched card came out
                        // flush with the outer wall (x -2.4), and the tail
                        // stands 3.90 mm out of the housing, so the housing
                        // is 1.0 mm further in than the drawing suggested.
sd_card_w  = 11.0;      // microSD card width
sd_ch_clr  = 0.3;       // channel clearance per side, across the width
sd_ch_h    = 1.3;       // channel height above the PCB top: a 1.0 mm card
                        // plus 0.3, and below the 1.32 mm housing top so the
                        // card cannot be steered over the socket
// Guide: the channel runs through the wall and on through a guide block on
// the inside wall face, ending sd_guide_gap short of the housing, so the
// card is held level and centred right up to the socket mouth. The outside
// end flares so a card finds the channel by feel.
sd_guide_gap  = 0.4;    // guide block to socket housing
sd_guide_side = 2.0;    // solid either side of the channel, in the block
sd_guide_top  = 1.5;    // solid above the channel, in the block
sd_flare_w    = 1.2;    // extra width per side at the outside face
sd_flare_h    = 1.2;    // extra height at the outside face, upward only
sd_flare_d    = 1.2;    // how far into the wall the flare runs
sd_housing = [11.55, 11.95, 1.32];   // TF-110 depth, width, height
sd_y0 = sd_cy - sd_card_w/2 - sd_ch_clr;   // channel edges
sd_y1 = sd_cy + sd_card_w/2 + sd_ch_clr;

// Finger relief around the slot mouth (PIZERO-103 rev 2). With the channel
// right, a card went in and stayed in, but it latches flush with the outer
// wall and the round dish was too shallow to grip it. This sinks the wall
// around the mouth by sd_relief_d, in BOTH halves, so that much of the card
// stands proud with a recess above and below it to pinch. It replaces the
// dish, keeps the wall 2 mm everywhere else, and leaves the guide block
// (which is what makes insertion work) untouched. Shortening the whole
// edge instead would cap out at 1.0 mm and cost channel length.
sd_relief   = true;
sd_relief_w = 18.0;     // across the card
// PIZERO-123: the top edge lines up with the plug pockets (pocket_top_z).
// The bottom edge stays where rev 2 put it (5.0 mm centred on the card), so
// the base is unchanged; only the lid part of the relief grows. Heights
// are above the split line.
sd_relief_z0 = sd_ch_h/2 - 5.0/2;   // -1.85, bottom, in the base
sd_relief_z1 = pocket_top_z;        // 5.45, shared pocket top
sd_relief_h  = sd_relief_z1 - sd_relief_z0;   // 7.30
sd_relief_d = wall - port_frame_t;   // 1.2 mm, as deep as the plug pockets
                                     // (pocket_d itself is derived later)
sd_relief_r = 1.5;      // corner radius

// Old thumb dish, superseded by the relief above.
sd_scoop   = false;
sd_scoop_r = 10.0;      // dish radius: shallow and wide, not a deep pocket
sd_scoop_d = 1.2;       // depth into the wall, same as the plug pockets
sd_scoop_z = sd_ch_h/2; // centre height above the split line

// 2-pin battery connector. Nothing in this project drives it, but it stays
// reachable. Trimmed after the first lid print: 1 mm off the top and 1 mm
// off the low-y edge, which is the left one seen from outside that wall.
// Tightened again in PIZERO-101 rev 3: 1 mm off the high-y (right, seen
// from outside) edge and 0.5 mm off the top.
bat_open = true;
bat_y0 = 15.4;          // [W] 14.4, pulled in 1.0
bat_y1 = 22.6;          // [W] 23.6, pulled in 1.0
bat_oh = 5.5;           // [M] was 7.0, then 6.0

// 40-pin header opening (PIZERO-102), cut only in the alternate lid,
// part="top_header". Centred on the header pin field as drawn in mock_pcb:
// pins span x 8.07..56.97, y 25.2..28.3. The default is a 2x20 female
// housing (50.8 x 5.1 mm) with clearance, and it is as long as fits between
// the two back screw bosses; the echo reports every clearance.
header_open = (part == "top_header");
hdr_cx = (8.07 + 56.97)/2;     // [W] pin field centre
hdr_cy = (25.2 + 28.3)/2;
hdr_open_w = 51.6;      // opening, left to right
hdr_open_d = 7.0;       // opening, front to back
hdr_open_r = 1.0;       // corner radius

run_pos  = [41.0, 21.6];   // [W]
boot_pos = [41.2, 11.6];   // [W]

// Bare round holes over RUN and BOOT read as holes. The vent grille below
// swallows them: two of its rows sit exactly on the two buttons, so the
// access is there but nothing on the lid looks like an access hole.
buttons_open = false;
button_hole_d = 3.0;

// Button guides (PIZERO-126). A funnel hangs from the roof underside below
// each of the two button slots: a long mouth under the slot narrowing to a
// round hole just above the button. Slide a thin tool along the slot until
// it drops in, push, and the funnel steers it onto the plunger. Nothing
// shows from outside; the roof and grille are unchanged. Roof-down, each
// funnel grows off the first layers like a pyramid, so no supports.
btn_guide       = true;
btn_top_meas    = 7.0;  // [M] case top (outside) down to the button top
btn_guide_clr   = 0.5;  // clear space between the funnel and the button
btn_guide_len   = 8.0;  // mouth length along the slot, at the roof
btn_guide_hole  = 2.0;  // bottom hole diameter, and the mouth width
btn_guide_wall  = 1.0;  // funnel wall thickness

/* ---------- roof vents ------------------------------------------------ */
//
// The CoCo 2 had no fan, just slots cut across the top. Same idea here.
// Two banks of five slots, each slot 1 mm wide with rounded ends, centred
// front to back (rev 5; see vent_rib for how that lands on RUN and BOOT).
// The left bank is the right bank mirrored about the board centre.

vents        = true;
vent_w       = 1.0;     // slot width, front to back (rev 5, was 3.0 [C])
vent_rows    = 5;       // rev 5, was 3
// Rev 5: pitch is a third of the RUN to BOOT distance (3.33 mm, a 2.33 mm
// rib). With an odd row count centred on the case, rows 2 and 5 then land
// within 0.07 mm of BOOT and RUN.
vent_rib     = (run_pos[1] - boot_pos[1])/3 - vent_w;
vent_gap     = 6.0;     // clear space between the two banks, left to right
vent_margin  = 5.0;     // base floor slots stop this far from the case edges
// Roof slots stop this far from the left and right edges. Rows used to be
// cut back to clear the corner bosses, which gave 20.8 mm slots; centred
// rows no longer come near a boss, so this holds that length instead.
roof_vent_margin = 11.1;
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
// reports the clearance. Rows and rib are the base's own, so reshaping the
// roof grille leaves the floor alone.
base_vents = true;
// Rev 6: same slot width and pitch as the roof. Seven per bank fills the
// same 21 mm the old four 3 mm slots did, so the footprint is unchanged.
base_vent_w    = vent_w;
base_vent_rows = 7;
base_vent_rib  = vent_rib;

vent_pitch   = vent_w + vent_rib;
vent_span    = vent_rows*vent_w + (vent_rows - 1)*vent_rib;
base_vent_pitch = base_vent_w + base_vent_rib;
base_vent_span  = base_vent_rows*base_vent_w + (base_vent_rows - 1)*base_vent_rib;

// Rev 4: the grille is centred front to back on the case, so the grooves
// either side of it both land on the flat roof. It used to be anchored on
// BOOT (row 2) and nudged 1 mm forward to catch RUN. Centred rows are
// symmetric about y 15.0 but the buttons are not (BOOT 3.4 mm in front,
// RUN 6.6 mm behind). Rev 4's 3 mm slots on a 5 mm pitch left each button
// 1.6 mm off a row; rev 5's 1 mm slots on a 3.33 mm pitch put both within
// 0.07 mm. The echo below reports the plunger width under open slot.
// (vent_y0 is set below, once y0 and od exist.)

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
groove_w     = 1.0;     // PIZERO-101: 0.5, then 2.0, now 1.0 (rev 5)
groove_d     = 0.6;     // an exact 3 layers at 0.2 mm, so it reads cleanly
groove_gap   = 3.0;     // rev 7: clear space between the slots and the groove
groove_off   = groove_gap + groove_w/2;   // groove centreline, off the slot edges
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
groove_wrap  = false;   // rev 7: off, the groove stays on the roof
// Rev 7: the loop hugs the grille, groove_gap clear of the slots on all four
// sides, instead of running out to the top chamfer. Its corners are
// concentric with a sharp grille corner: groove_r becomes groove_off.
groove_hug   = true;
// The recess is defined by the slots, not by the case: band_margin of
// border in front of the first row and behind the last, and wherever the
// grille has to sit for the buttons is where the whole thing sits. On this
// board that leaves the recess off-centre front to back, by design.
band_depth   = 2.0;     // how far below the top surface. The roof under the
                        // band stays base_roof_t, so the case grows by
                        // exactly this much.

/* ---------- part version --------------------------------------------- */
//
// Each part carries its own version, engraved on an inside face: the base
// between the two banks of floor vents, the lid on the roof underside,
// centred on the two USB ports. Bump the number here when that part's
// geometry changes, and say what changed in README.md, so a part in hand
// can be matched to a revision without measuring it.
//
//   BASE v9            PIZERO-101 rev 8 + the PIZERO-103 microSD rev 2
//   BASE v10           PIZERO-96: countersinks 90 degrees (screw_head_h
//                      1.5 to 1.1). Base only.
//   TOP v12            PIZERO-124: bosses webbed into the corner walls
//   TOP-HDR v4         and the pilot opened 2.1 to 2.2. (v11/v3:
//                      PIZERO-123 pocket tops.)
//   TOP v13            PIZERO-126: guide funnels under the RUN and BOOT
//   TOP-HDR v5         slots. Lid only.

ver_show = true;
ver_base       = "BASE v10";
ver_top        = "TOP v13";
ver_top_header = "TOP-HDR v5";
ver_size  = 3.0;        // font size; glyphs are ~0.7 of this
ver_d     = 0.5;        // engraving depth
ver_font  = "Liberation Sans:style=Bold";
ver_lid_y = 2.2;        // roof underside, in front of the groove loop

$fn = 64;

// The whole point of anchoring rows on the buttons is that a paperclip can
// still reach RUN and BOOT through a vent slot. Shout if a change breaks it.
// Reports how much of the plunger, front to back, sits under open slot.
button_d = 1.5;         // [?] plunger diameter, inferred from the rev 1 grille
module _check_button(name, p) {
    open = [for (b = vent_banks, r = [0 : vent_rows - 1])
              let (cy = vent_y0 + r*vent_pitch)
              if (p[0] >= max(b[0], slot_lo(cy)) && p[0] <= min(b[1], slot_hi(cy)))
                  min(button_d, vent_w/2 + button_d/2 - abs(p[1] - cy))];
    best = len(open) ? max(open) : 0;
    if (vents)
        echo(best > 0 ? str(name, ": ", best, " mm of plunger under a vent slot")
                      : str("NOTE: ", name, " is not under a vent slot"));
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

vent_y0 = y0 + od/2 - (vent_rows - 1)*vent_pitch/2;   // first roof row, centred

// rounded rectangle prism, corner at (px,py), size (w,d,h), radius r
module rrect(px, py, w, d, h, r) {
    hull() for (dx = [r, w - r], dy = [r, d - r])
        translate([px + dx, py + dy, 0]) cylinder(h = h, r = r);
}

module outer_shell(h)  { rrect(x0, y0, ow, od, h, r_out); }

// outer_shell with its bottom edge chamfered by c
module outer_shell_chamfered(h, c) {
    if (c > 0)
        hull() {
            rrect(x0 + c, y0 + c, ow - 2*c, od - 2*c, 0.01, max(0.1, r_out - c));
            translate([0, 0, c]) rrect(x0, y0, ow, od, h - c, r_out);
        }
    else outer_shell(h);
}
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
    zb = cz - h/2;
    zt = split_z + pocket_top_z;         // shared top edge (PIZERO-123)
    if (plug_pocket && min(z_hi, zt) > max(z_lo, zb))
        z_slab(z_lo, z_hi)
            xz_rrect(p[0], w, zb, zt, y0 - 0.01, pocket_d + 0.01,
                     pocket_r, true);
}

// Banks run from vent_margin off each outer edge in to the centre gap.
vent_x_lo = x0 + roof_vent_margin;
vent_x_hi = x0 + ow - roof_vent_margin;
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
groove_xl = groove_hug ? vent_lo_all - groove_off                 // left leg
                       : x0 + top_chamfer + groove_edge + groove_w/2;
groove_xr = groove_hug ? vent_hi_all + groove_off
                       : x0 + ow - top_chamfer - groove_edge - groove_w/2;
groove_loop_r = groove_hug ? groove_off : groove_r;

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
                  groove_d + 1, groove_loop_r + groove_w/2);
            translate([0, 0, -0.5])
                rrect(groove_xl + groove_w/2, groove_yf + groove_w/2,
                      groove_xr - groove_xl - groove_w, groove_yb - groove_yf - groove_w,
                      groove_d + 2, max(0.1, groove_loop_r - groove_w/2));
        }
    else if (groove)
        for (cy = [groove_yf, groove_yb])
            translate([x0 - 1, cy - groove_w/2, case_h - groove_d])
                cube([ow + 2, groove_w, groove_d + 1]);
}

// Base floor vents: slot x centres for both banks, and the slot y extent.
base_vent_x = [ for (x_lo = [bw/2 - vent_gap/2 - base_vent_span, bw/2 + vent_gap/2],
                     r = [0 : base_vent_rows - 1])
                  x_lo + base_vent_w/2 + r*base_vent_pitch ];
base_vent_y0 = y0 + vent_margin;
base_vent_y1 = y0 + od - vent_margin;

module base_vent_cuts() {
    if (base_vents)
        for (cx = base_vent_x)
            hull() for (y = [base_vent_y0 + base_vent_w/2, base_vent_y1 - base_vent_w/2])
                translate([cx, y, -1]) cylinder(d = base_vent_w, h = floor_t + 2);
}

module band_cut() {
    if (band)
        translate([0, 0, case_h - band_depth])
            rrect(band_x0, band_y0, band_x1 - band_x0, band_y1 - band_y0,
                  band_depth + 1, band_r);
}

// Card channel through the lid wall and guide block, with a flared mouth.
module sd_channel_cut() {
    w  = sd_card_w + 2*sd_ch_clr;
    zb = split_z - 0.01;
    translate([x0 - 1, sd_cy - w/2, zb])
        cube([sd_front_x - sd_guide_gap - x0 + 1.01, w, sd_ch_h + 0.01]);
    // flare, measured from whatever face the card meets first
    fx = x0 + (sd_relief ? sd_relief_d : 0);
    hull() {
        translate([fx + sd_flare_d, sd_cy - w/2, zb])
            cube([0.01, w, sd_ch_h + 0.01]);
        translate([fx - 0.5, sd_cy - w/2 - sd_flare_w, zb])
            cube([0.01, w + 2*sd_flare_w, sd_ch_h + sd_flare_h + 0.01]);
    }
}

// Finger relief sunk into the outer wall around the slot mouth. Cut from
// both halves, so it straddles the split line.
module sd_relief_cut() {
    if (sd_relief)
        translate([x0 - 0.01, sd_cy, split_z + (sd_relief_z0 + sd_relief_z1)/2]) rotate([0, 90, 0])
            linear_extrude(height = sd_relief_d + 0.01)
                hull() for (dz = [-1, 1], dy = [-1, 1])
                    translate([dz*(sd_relief_h/2 - sd_relief_r),
                               dy*(sd_relief_w/2 - sd_relief_r)])
                        circle(r = sd_relief_r);
}

// Guide block on the inside wall face, around the channel.
module sd_guide() {
    w     = sd_card_w + 2*sd_ch_clr;
    x_in  = -clr;
    x_end = sd_front_x - sd_guide_gap;
    if (x_end > x_in)
        translate([x_in - 0.01, sd_cy - w/2 - sd_guide_side, split_z])
            cube([x_end - x_in + 0.01, w + 2*sd_guide_side, sd_ch_h + sd_guide_top]);
}

// Button guide funnel (PIZERO-126), solid when g is the wall thickness and
// the bore when g is 0. Heights above the split line: the button top is
// derived from the outside measurement, the funnel stops btn_guide_clr
// above it and runs up to the roof underside.
btn_top_z = lid_h - btn_top_meas;                    // 4.8
btn_guide_z0 = btn_top_z + btn_guide_clr;            // funnel bottom
module btn_funnel(p, g, ext = 0) {
    zb = split_z + btn_guide_z0 - ext;
    zt = split_z + head_room + ext;
    d  = btn_guide_hole + 2*g;
    hull() {
        translate([p[0], p[1], zb]) cylinder(d = d, h = 0.01);
        for (dx = [-1, 1])
            translate([p[0] + dx*(btn_guide_len/2 - btn_guide_hole/2), p[1], zt - 0.01])
                cylinder(d = d, h = 0.01);
    }
}
module btn_guides()    { if (btn_guide) for (b = [run_pos, boot_pos]) btn_funnel(b, btn_guide_wall); }
module btn_guide_cut() { if (btn_guide) for (b = [run_pos, boot_pos]) btn_funnel(b, 0, 0.02); }
if (btn_guide)
    echo(str("button guides: funnel ", head_room - btn_guide_z0, " mm tall, ",
             btn_guide_clr, " mm above the button top at ", btn_top_z,
             " (roof underside at ", head_room, ")"));
assert(!btn_guide || btn_guide_z0 < head_room - 1, "button guide has no room");

// Version engraving. The base reads from above, the lid from below (so its
// text is mirrored in the model to read correctly from inside).
module ver_base_cut() {
    if (ver_show && ver_base != "")
        translate([bw/2, bd/2, floor_t - ver_d])
            linear_extrude(ver_d + 0.01) rotate([0, 0, 90])
                text(ver_base, size = ver_size, font = ver_font,
                     halign = "center", valign = "center");
}

module ver_lid_cut() {
    s = header_open ? ver_top_header : ver_top;
    if (ver_show && s != "")
        translate([(usbc_host[0] + usbc_power[0])/2, ver_lid_y,
                   case_h - top_t - 0.01])
            // The lid is read cavity-up, which is the roof-down print
            // orientation: a 180 degree flip about x. Pre-flip in y so the
            // text reads correctly there rather than as a reflection.
            linear_extrude(ver_d + 0.01) mirror([0, 1, 0])
                text(s, size = ver_size, font = ver_font,
                     halign = "center", valign = "center");
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
                outer_shell_chamfered(base_h, bottom_chamfer);
                translate([0, 0, floor_t]) inner_cavity(base_h);
            }
            for (h = holes)
                translate([h[0], h[1], floor_t])
                    cylinder(d = boss_d, h = standoff_h);
        }
        // relief so a plug overmould clears the base wall
        for (p = front_ports) port_pocket(p, -1, split_z);
        sd_relief_cut();
        base_vent_cuts();
        ver_base_cut();

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
            // PIZERO-124: tie each boss into its corner (see boss_web)
            if (boss_web) intersection() {
                for (h = holes) {
                    cx = h[0] < bw/2 ? x0 : h[0];
                    cy = h[1] < bd/2 ? y0 : h[1];
                    translate([cx, cy, split_z + boss_web_gap])
                        cube([hole_in - x0, hole_in - y0, head_room]);
                }
                translate([0, 0, split_z]) lid_cavity(head_room);
            }
            sd_guide();
            btn_guides();
        }
        for (h = holes)
            translate([h[0], h[1], split_z - 0.01])
                cylinder(d = pilot_d, h = pilot_depth + 0.01);
        btn_guide_cut();

        for (p = front_ports) {
            port_through(p, split_z, case_h);
            port_pocket(p, split_z, case_h);
        }

        sd_channel_cut();
        sd_relief_cut();
        sd_scoop_cut();
        ver_lid_cut();
        if (bat_open) notch_right(bat_y0, bat_y1, bat_oh);

        band_cut();
        groove_cuts();
        vent_cuts();

        if (header_open)
            translate([0, 0, split_z + head_room - 1])
                rrect(hdr_cx - hdr_open_w/2, hdr_cy - hdr_open_d/2,
                      hdr_open_w, hdr_open_d, top_t + 2, hdr_open_r);

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
    color("#999") translate([sd_front_x, sd_cy - sd_housing[1]/2, pcb_t])
        cube(sd_housing);
    color("#333") translate([sd_front_x - 3.9, sd_cy - sd_card_w/2, pcb_t + 0.15])
        cube([15.0, sd_card_w, 1.0]);
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
    echo(str("base vents x ", base_vent_x[0] - base_vent_w/2, " .. ",
             base_vent_x[len(base_vent_x) - 1] + base_vent_w/2,
             "  y ", base_vent_y0, " .. ", base_vent_y1,
             "  clear of standoffs by ",
             base_vent_x[0] - base_vent_w/2 - (hole_in + boss_d/2)));
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

echo(str("microSD channel y ", sd_y0, " .. ", sd_y1, ", ", sd_ch_h,
         " mm tall  guide block x ", -clr, " .. ", sd_front_x - sd_guide_gap,
         " (", sd_front_x - sd_guide_gap + clr, " mm long)",
         "  latched card tail x ", sd_front_x - 3.9, " (outer wall x ", x0,
         sd_relief ? str(", relief floor x ", x0 + sd_relief_d) : "", ")",
         "  card stands proud by ",
         (x0 + (sd_relief ? sd_relief_d : 0)) - (sd_front_x - 3.9)));

if (header_open) {
    hx0 = hdr_cx - hdr_open_w/2;  hx1 = hdr_cx + hdr_open_w/2;
    hy0 = hdr_cy - hdr_open_d/2;  hy1 = hdr_cy + hdr_open_d/2;
    // nearest point of the opening to each back boss, less the boss radius
    boss_clr = min([for (h = holes) if (h[1] > bd/2)
        let (dx = max(hx0 - h[0], 0, h[0] - hx1),
             dy = max(hy0 - h[1], 0, h[1] - hy1))
        sqrt(dx*dx + dy*dy) - boss_d/2]);
    echo(str("header opening x ", hx0, " .. ", hx1, "  y ", hy0, " .. ", hy1,
             "  clear of back bosses by ", boss_clr,
             "  of back inner wall by ", (bd + clr) - hy1,
             "  of back vent slots by ", hy0 - vent_group_y1,
             (groove && hy0 < groove_yb + groove_w/2)
                 ? str("  CUTS the back groove (y ", groove_yb - groove_w/2,
                       " .. ", groove_yb + groove_w/2, ")") : ""));
}

_check_button("RUN", run_pos);
_check_button("BOOT", boot_pos);

if (part == "coupon") coupon_printable();
else if (part == "base") base_printable();
else if (part == "top" || part == "top_header") lid_printable();
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

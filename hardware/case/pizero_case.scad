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
top_t   = 1.8;          // lid roof
clr     = 0.4;          // clearance between PCB edge and inner wall, per side

standoff_h = 2.6;       // PCB underside to floor: clears bottom-side solder
head_room  = 12.0;      // [?] PCB top to lid roof. A 2.54 mm male header
                        //     stands ~11.5 mm proud of the board.

r_out = 4.0;            // vertical corner radius, outside
top_chamfer = 1.0;      // chamfer on the top edge (prints flat on the plate)

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

// [ centre_x, body_w, body_h, plug_w, plug_h ]
hdmi = [12.35, 12.1, 4.0, 15.5, 7.0];   // [W] centre, [?] the rest
usbc_host  = [39.1, 9.6, 3.3, 12.5, 7.0];   // PIO-USB host port (keyboard)
usbc_power = [53.6, 9.6, 3.3, 12.5, 7.0];   // power / programming port
front_ports = [hdmi, usbc_host, usbc_power];

sd_y0 = 10.8;           // [W] microSD slot, from the connector edge
sd_y1 = 21.0;           // [W]
sd_oh = 2.5;            // [?] height above the PCB top

bat_open = true;        // opening for the 2-pin battery connector
bat_y0 = 14.4;          // [W]
bat_y1 = 23.6;          // [W]
bat_oh = 7.0;           // [?]

buttons_open = true;    // paperclip holes over RUN and BOOT
run_pos  = [41.0, 21.6];   // [W]
boot_pos = [41.2, 11.6];   // [W]
button_hole_d = 3.0;

$fn = 64;

/* ===================================================================== */

split_z = floor_t + standoff_h + pcb_t;   // base wall top = PCB top surface
base_h  = split_z;
lid_h   = head_room + top_t;
case_h  = split_z + lid_h;

ow = bw + 2*clr + 2*wall;      // outer width
od = bd + 2*clr + 2*wall;      // outer depth
x0 = -(clr + wall);            // outer shell origin, in board coords
y0 = -(clr + wall);
r_in = max(0.1, r_out - wall);

pocket_d = wall - port_frame_t;

holes = [ for (hx = [hole_in, bw - hole_in], hy = [hole_in, bd - hole_in]) [hx, hy] ];

// rounded rectangle prism, corner at (px,py), size (w,d,h), radius r
module rrect(px, py, w, d, h, r) {
    hull() for (dx = [r, w - r], dy = [r, d - r])
        translate([px + dx, py + dy, 0]) cylinder(h = h, r = r);
}

module outer_shell(h)  { rrect(x0, y0, ow, od, h, r_out); }
module inner_cavity(h) { rrect(-clr, -clr, bw + 2*clr, bd + 2*clr, h, r_in); }

/* ---------- front port cuts ------------------------------------------ */
//
// z_lo/z_hi bound the cut, so the same geometry serves the lid (above the
// split line) and the base relief (below it).

module port_through(p, z_lo, z_hi) {
    w = p[1] + 2*port_clr;
    zb = split_z;                        // connector sits on the PCB top
    zt = split_z + p[2] + port_clr;
    lo = max(z_lo, zb - 0.01); hi = min(z_hi, zt);
    if (hi > lo)
        translate([p[0] - w/2, y0 - 1, lo])
            cube([w, wall + clr + 2, hi - lo]);
}

module port_pocket(p, z_lo, z_hi) {
    w  = p[3] + 2*plug_clr;
    h  = p[4] + 2*plug_clr;
    cz = split_z + p[2]/2;               // plug is centred on the socket
    lo = max(z_lo, cz - h/2); hi = min(z_hi, cz + h/2);
    if (plug_pocket && hi > lo)
        translate([p[0] - w/2, y0 - 0.01, lo])
            cube([w, pocket_d + 0.01, hi - lo]);
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
                outer_shell(lid_h);
                translate([0, 0, -0.01]) inner_cavity(head_room + 0.01);
            }
            // bosses: full pillars from the PCB top up to the roof, so they
            // tie the roof down and clamp the board at the same time
            for (h = holes)
                translate([h[0], h[1], split_z])
                    cylinder(d = boss_d, h = head_room);
        }
        for (h = holes)
            translate([h[0], h[1], split_z - 0.01])
                cylinder(d = pilot_d, h = pilot_depth + 0.01);

        for (p = front_ports) {
            port_through(p, split_z, case_h);
            port_pocket(p, split_z, case_h);
        }

        notch_left(sd_y0, sd_y1, sd_oh);
        if (bat_open) notch_right(bat_y0, bat_y1, bat_oh);

        if (buttons_open) for (b = [run_pos, boot_pos])
            translate([b[0], b[1], split_z + head_room - 0.01])
                cylinder(d = button_hole_d, h = top_t + 0.02);

        // chamfer the top edge
        translate([0, 0, case_h - top_chamfer])
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

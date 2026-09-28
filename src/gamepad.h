// gamepad.h: USB gamepad -> CoCo joysticks (PIZERO-13).
//
// Pure logic, host-tested (test/test_gamepad): which USB identities are pads,
// and how one report becomes two CoCo joysticks. main.cpp does the USB side.
//
// The pad is a GameSir Tegenaria Lite, which has several USB identities:
//
//   057E:2009  Switch Pro. Sends nothing without Nintendo's handshake; never
//              bound. The pad sits here after Switch mode is chosen by hand
//              (Home + Y), which also turns its automatic detection off.
//   3537:1093  GameSir's own, with automatic detection on (hold Home 10 s to
//              restore it). Measured on this board, PIZERO-13, see below.
//   3537:1094  GameSir Android mode (Home + A, yellow light). A standard HID
//              gamepad on interface 0. Measured on this board, PIZERO-13.
//   054C:09CC  DualShock 4. What automatic detection chose on the Fruit Jam
//              board, where FRUITJAM-18 measured the layout. Kept because it
//              is known-good there.
//
// Mapping, the same for every identity (as on Fruit Jam): left stick -> RIGHT
// CoCo joystick (port 0, the one nearly all software reads), right stick ->
// LEFT (port 1). The D-pad overrides the left stick, snapped to the rail, or
// presses the arrow keys (dpad = arrows). Buttons do what the settings file
// says (PIZERO-164, pad_map below); by default the bottom or right face button
// or R1 fires the left stick's joystick, the left face button or L1 the right
// stick's, Start is ENTER and the top face button SPACE.

#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

enum pad_kind {
    PAD_NONE = 0,       // not a known pad: may be a keyboard
    PAD_DS4,            // DualShock 4 layout
    PAD_XINPUT,         // Xbox 360 layout in a vendor HID report
    PAD_HIDGP,          // standard HID gamepad (GameSir Android mode)
    PAD_SILENT,         // a pad identity that sends no input (Switch Pro)
};

#define PAD_DS4_VID     0x054C
#define PAD_DS4_PID     0x09CC
#define PAD_SWITCH_VID  0x057E
#define PAD_SWITCH_PID  0x2009
#define PAD_GAMESIR_VID 0x3537
#define PAD_GAMESIR_PID 0x1093
#define PAD_GAMESIR_ANDROID_PID 0x1094   // Android mode (Home + A): two interfaces

static inline enum pad_kind pad_identify(uint16_t vid, uint16_t pid) {
    if (vid == PAD_DS4_VID && pid == PAD_DS4_PID) return PAD_DS4;
    if (vid == PAD_GAMESIR_VID && pid == PAD_GAMESIR_PID) return PAD_XINPUT;
    if (vid == PAD_GAMESIR_VID && pid == PAD_GAMESIR_ANDROID_PID) return PAD_HIDGP;
    if (vid == PAD_SWITCH_VID && pid == PAD_SWITCH_PID) return PAD_SILENT;
    return PAD_NONE;
}

#define PAD_CENTER 32767

// PIZERO-164: every button a decoder knows, by position, so the same names
// work for any pad (A and B are swapped between Xbox and Nintendo pads).
enum pad_button {
    PAD_B_BOTTOM = 0, PAD_B_RIGHT, PAD_B_LEFT, PAD_B_TOP,
    PAD_B_L1, PAD_B_R1, PAD_B_L2, PAD_B_R2,
    PAD_B_SELECT, PAD_B_START, PAD_B_L3, PAD_B_R3, PAD_B_HOME,
    PAD_BUTTONS
};
#define PAD_BIT(b) ((uint16_t)(1u << (b)))

// What a decoder reads from one report: the sticks as they are, the D-pad as
// a compass direction (0 = N clockwise to 7 = NW, 8 = idle), and the buttons.
struct pad_state {
    uint16_t axis[2][2];    // [stick: 0 left, 1 right][0 = X, 1 = Y], 0 = left / up
    uint8_t  dpad;
    uint16_t buttons;       // PAD_BIT(PAD_B_*)
};

// --- DS4 identity ------------------------------------------------------------
// Fixed offsets, measured in FRUITJAM-18 (the identity has no descriptor):
//
//   b0  report ID, always 0x01
//   b1  LX   00 = left,  80 = center, FF = right
//   b2  LY   00 = up,    80 = center, FF = down
//   b3  RX,  b4 RY   (same convention)
//   b5  low nibble  = D-pad: 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW, 8-F idle
//       high nibble = bit4 square, bit5 cross, bit6 circle, bit7 triangle
//   b6  bit0 L1, bit1 R1; per the DS4's published layout (never seen on this
//       board, UNVERIFIED): bit2 L2, bit3 R2, bit4 share, bit5 options,
//       bit6 L3, bit7 R3
//   b7  bit0 PS (published layout, UNVERIFIED)
//
// Center measured exactly 0x80 with no jitter (FRUITJAM-92). The small
// deadzone is insurance against wear, and snaps rest to exact center, which
// 0x80 * 257 = 32896 would otherwise miss.
#define PAD_DEADZONE 0x06

static inline uint16_t pad_axis(uint8_t v) {
    int d = (int)v - 0x80;
    if (d > -PAD_DEADZONE && d < PAD_DEADZONE) return PAD_CENTER;
    return (uint16_t)(v * 257);      // 0..255 -> 0..65535, 0xFF -> 0xFFFF
}

// Copy set bits of `byte` into buttons, one PAD_B_* per bit (0xFF = none).
static inline uint16_t pad_bits(uint8_t byte, const uint8_t map[8]) {
    uint16_t b = 0;
    for (int i = 0; i < 8; i++)
        if ((byte & (1u << i)) && map[i] != 0xFF) b |= PAD_BIT(map[i]);
    return b;
}

// False (and *s untouched) if it is not an input report.
static inline bool pad_decode_ds4(const uint8_t *r, uint16_t len, struct pad_state *s) {
    if (len < 7 || r[0] != 0x01) return false;
    s->axis[0][0] = pad_axis(r[1]);
    s->axis[0][1] = pad_axis(r[2]);
    s->axis[1][0] = pad_axis(r[3]);
    s->axis[1][1] = pad_axis(r[4]);
    s->dpad = (uint8_t)(r[5] & 0x0F);
    if (s->dpad > 8) s->dpad = 8;
    static const uint8_t B5[8] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                   PAD_B_LEFT, PAD_B_BOTTOM, PAD_B_RIGHT, PAD_B_TOP };
    static const uint8_t B6[8] = { PAD_B_L1, PAD_B_R1, PAD_B_L2, PAD_B_R2,
                                   PAD_B_SELECT, PAD_B_START, PAD_B_L3, PAD_B_R3 };
    s->buttons = (uint16_t)(pad_bits(r[5], B5) | pad_bits(r[6], B6));
    if (len >= 8 && (r[7] & 0x01)) s->buttons |= PAD_BIT(PAD_B_HOME);
    return true;
}

// --- GameSir identity: Xbox 360 layout ---------------------------------------
// Its descriptor declares a keyboard (ID 3), a mouse (ID 9) and 63 vendor
// bytes (ID 0x10), and every report it sends is the vendor one: the standard
// Xbox 360 input report (type 0x00, length 0x14) with the report ID 0x10 in
// place of the type byte. Confirmed by control-by-control sweeps on the pad
// (PIZERO-13 and 164, 2026-09-28) except where marked:
//
//   b0  0x10 (report ID)   b1  0x14 (length)
//   b2  D-pad up 01, down 02, left 04, right 08; L3 40, R3 80;
//       start 10, back 20 (the Xbox layout, UNVERIFIED)
//   b3  L1 01, R1 02, bottom (cross) 10, right (circle) 20,
//       left (square) 40, top (triangle) 80; guide 04 (UNVERIFIED)
//   b4  L2, b5 R2   analog 0-255 (a button past half travel)
//   b6-7  LX, b8-9 LY, b10-11 RX, b12-13 RY   signed 16-bit little-endian,
//         -32768..32767, +X right, +Y UP (the CoCo's Y grows downward)
//
// Every axis read exactly 0 at rest in the sweep.
#define PAD_X_DEADZONE 1536              // the DS4 deadzone's share of travel

static inline uint16_t pad_axis16(int16_t v, bool flip) {
    int d = v;
    if (d > -PAD_X_DEADZONE && d < PAD_X_DEADZONE) return PAD_CENTER;
    if (flip) d = -1 - d;                // 32767 <-> -32768 exactly, so both rails reach
    return (uint16_t)(d + 32768);        // -32768..32767 -> 0..65535
}

static inline int16_t pad_s16(const uint8_t *p) {
    return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

static inline bool pad_decode_xinput(const uint8_t *r, uint16_t len, struct pad_state *s) {
    if (len < 14 || r[0] != 0x10 || r[1] != 0x14) return false;
    s->axis[0][0] = pad_axis16(pad_s16(r + 6), false);
    s->axis[0][1] = pad_axis16(pad_s16(r + 8), true);
    s->axis[1][0] = pad_axis16(pad_s16(r + 10), false);
    s->axis[1][1] = pad_axis16(pad_s16(r + 12), true);
    // D-pad bits -> compass direction.
    bool up = r[2] & 0x01, down = r[2] & 0x02, left = r[2] & 0x04, right = r[2] & 0x08;
    static const uint8_t DIR[3][3] = {   // [y+1][x+1]: y -1 up, x -1 left
        { 7, 0, 1 },
        { 6, 8, 2 },
        { 5, 4, 3 },
    };
    int x = (right ? 1 : 0) - (left ? 1 : 0);
    int y = (down ? 1 : 0) - (up ? 1 : 0);
    s->dpad = DIR[y + 1][x + 1];
    static const uint8_t B2[8] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                   PAD_B_START, PAD_B_SELECT, PAD_B_L3, PAD_B_R3 };
    static const uint8_t B3[8] = { PAD_B_L1, PAD_B_R1, PAD_B_HOME, 0xFF,
                                   PAD_B_BOTTOM, PAD_B_RIGHT, PAD_B_LEFT, PAD_B_TOP };
    s->buttons = (uint16_t)(pad_bits(r[2], B2) | pad_bits(r[3], B3));
    if (r[4] >= 0x80) s->buttons |= PAD_BIT(PAD_B_L2);
    if (r[5] >= 0x80) s->buttons |= PAD_BIT(PAD_B_R2);
    return true;
}

// --- GameSir Android mode: standard HID gamepad -----------------------------
// Its 127-byte descriptor declares, as report 1: 15 buttons, a hat switch,
// X Y Z Rz and two analog triggers. Every button confirmed by sweeps on the
// pad (PIZERO-13 and 164, 2026-09-28), the usual Android numbering:
//
//   b0  0x01 (report ID)
//   b1  bottom (A, cross) 01, right (B, circle) 02, left (X, square) 08,
//       top (Y, triangle) 10, L1 40, R1 80
//   b2  L2 01, R2 02, select 04, start 08, home 10, L3 20, R3 40
//   b3  low nibble = hat: 0=N clockwise to 7=NW, 0x0F idle
//   b4 LX, b5 LY, b6 RX, b7 RY   00 = left/up, 80 = center, FF = right/down
//   b8 R2, b9 L2   analog 0-255 (the digital bits above are enough)
//
// The M button is not here: it sends a Print Screen keystroke from the pad's
// second interface, as if it were a keyboard (seen in the PIZERO-164 sweep).
static inline bool pad_decode_hidgp(const uint8_t *r, uint16_t len, struct pad_state *s) {
    if (len < 10 || r[0] != 0x01) return false;
    s->axis[0][0] = pad_axis(r[4]);
    s->axis[0][1] = pad_axis(r[5]);
    s->axis[1][0] = pad_axis(r[6]);
    s->axis[1][1] = pad_axis(r[7]);
    s->dpad = (uint8_t)(r[3] & 0x0F);
    if (s->dpad > 8) s->dpad = 8;
    static const uint8_t B1[8] = { PAD_B_BOTTOM, PAD_B_RIGHT, 0xFF, PAD_B_LEFT,
                                   PAD_B_TOP, 0xFF, PAD_B_L1, PAD_B_R1 };
    static const uint8_t B2[8] = { PAD_B_L2, PAD_B_R2, PAD_B_SELECT, PAD_B_START,
                                   PAD_B_HOME, PAD_B_L3, PAD_B_R3, 0xFF };
    s->buttons = (uint16_t)(pad_bits(r[1], B1) | pad_bits(r[2], B2));
    return true;
}

static inline bool pad_decode(enum pad_kind k, const uint8_t *r, uint16_t len,
                              struct pad_state *s) {
    if (k == PAD_DS4)    return pad_decode_ds4(r, len, s);
    if (k == PAD_XINPUT) return pad_decode_xinput(r, len, s);
    if (k == PAD_HIDGP)  return pad_decode_hidgp(r, len, s);
    return false;
}

// --- PIZERO-164: what the buttons do -------------------------------------------
// Each button has one action: fire for the joystick a stick drives, nothing,
// or a CoCo key (a dkbd scancode, K_* in key_translate.h, all below 0x40).
enum {
    PAD_ACT_FIRE = 0x40,         // fire of the joystick the LEFT stick and D-pad drive
    PAD_ACT_FIRE_RIGHT = 0x41,   // fire of the joystick the RIGHT stick drives
    PAD_ACT_NONE = 0x42,
};

// The CoCo keys an action needs (duplicated here so this header stands alone).
#define PAD_K_UP     0x2B
#define PAD_K_DOWN   0x2C
#define PAD_K_LEFT   0x2D
#define PAD_K_RIGHT  0x2E
#define PAD_K_SPACE  0x2F
#define PAD_K_ENTER  0x30

struct pad_map {
    uint8_t act[PAD_BUTTONS];
    bool    dpad_arrows;         // D-pad presses the arrow keys, not the stick
};

// Defaults: the fire buttons as before PIZERO-164 (bottom, right, R1 fire the
// left stick's joystick; left and L1 the right stick's), plus Start = ENTER
// and top = SPACE, the two keys most games want to start. The rest do nothing.
static inline void pad_map_defaults(struct pad_map *m) {
    for (int i = 0; i < PAD_BUTTONS; i++) m->act[i] = PAD_ACT_NONE;
    m->act[PAD_B_BOTTOM] = m->act[PAD_B_RIGHT] = m->act[PAD_B_R1] = PAD_ACT_FIRE;
    m->act[PAD_B_LEFT] = m->act[PAD_B_L1] = PAD_ACT_FIRE_RIGHT;
    m->act[PAD_B_START] = PAD_K_ENTER;
    m->act[PAD_B_TOP] = PAD_K_SPACE;
    m->dpad_arrows = false;
}

// The pad's effect on the machine, before joystick_swap: joystick 0 is the one
// the left stick drives, joystick 1 the right stick's.
#define PAD_KEYS_MAX (PAD_BUTTONS + 2)
struct pad_out {
    uint16_t axis[2][2];
    bool     fire[2];
    uint8_t  keys[PAD_KEYS_MAX];
    uint8_t  nkeys;
};

static inline void pad_centered(struct pad_out *o) {
    o->axis[0][0] = o->axis[0][1] = o->axis[1][0] = o->axis[1][1] = PAD_CENTER;
    o->fire[0] = o->fire[1] = false;
    o->nkeys = 0;
}

static inline void pad_resolve(const struct pad_state *s, const struct pad_map *m,
                               struct pad_out *o) {
    for (int i = 0; i < 2; i++) { o->axis[i][0] = s->axis[i][0]; o->axis[i][1] = s->axis[i][1]; }
    o->fire[0] = o->fire[1] = false;
    o->nkeys = 0;
    for (int b = 0; b < PAD_BUTTONS; b++) {
        if (!(s->buttons & PAD_BIT(b))) continue;
        if (b == PAD_B_HOME) continue;            // the overlay's button (PIZERO-169)
        uint8_t a = m->act[b];
        if (a == PAD_ACT_FIRE) o->fire[0] = true;
        else if (a == PAD_ACT_FIRE_RIGHT) o->fire[1] = true;
        else if (a < 0x40 && o->nkeys < PAD_KEYS_MAX) o->keys[o->nkeys++] = a;
    }
    if (s->dpad < 8) {
        static const int8_t HX[8] = {  0, +1, +1, +1,  0, -1, -1, -1 };
        static const int8_t HY[8] = { -1, -1,  0, +1, +1, +1,  0, -1 };
        int8_t hx = HX[s->dpad], hy = HY[s->dpad];
        if (m->dpad_arrows) {
            if (hx && o->nkeys < PAD_KEYS_MAX) o->keys[o->nkeys++] = hx > 0 ? PAD_K_RIGHT : PAD_K_LEFT;
            if (hy && o->nkeys < PAD_KEYS_MAX) o->keys[o->nkeys++] = hy > 0 ? PAD_K_DOWN : PAD_K_UP;
        } else {                           // snapped to the rails, over the stick
            if (hx) o->axis[0][0] = hx > 0 ? 65535 : 0;
            if (hy) o->axis[0][1] = hy > 0 ? 65535 : 0;
        }
    }
}

// Decode and resolve in one step. False (and *o untouched) if the report is
// not an input report.
static inline bool pad_read(enum pad_kind k, const uint8_t *r, uint16_t len,
                            const struct pad_map *m, struct pad_out *o) {
    struct pad_state s;
    if (!pad_decode(k, r, len, &s)) return false;
    pad_resolve(&s, m, o);
    return true;
}

// The names the settings file uses for the buttons, by PAD_B_*. Home is not
// among them: it always opens and closes the overlay (PIZERO-169).
static const char *const PAD_BUTTON_NAMES[PAD_BUTTONS] = {
    "bottom", "right", "left", "top", "l1", "r1", "l2", "r2",
    "select", "start", "l3", "r3", 0,
};

// --- PIZERO-169: the pad drives the overlay -------------------------------------
// While the overlay is open, the pad's buttons become the keyboard keys the
// overlay already understands (HID usages, overlay_keys.h), which main.cpp
// merges with the keyboard's own: D-pad = arrows (up/down move, left/right
// switch lists), A (bottom) = ENTER, B (right) = ESC, L1/R1 = PgUp/PgDn,
// X (left) = TAB. Home is main.cpp's job: it opens the overlay, and closes it
// once it has been let go since opening (else the press that opened it would
// close it again).
#define PAD_HK_ENTER 0x28
#define PAD_HK_ESC   0x29
#define PAD_HK_TAB   0x2B
#define PAD_HK_PGUP  0x4B
#define PAD_HK_PGDN  0x4E
#define PAD_HK_RIGHT 0x4F
#define PAD_HK_LEFT  0x50
#define PAD_HK_DOWN  0x51
#define PAD_HK_UP    0x52

// Fill codes[6] (zero-padded) and return how many were set.
static inline int pad_nav_codes(uint16_t buttons, uint8_t dpad, uint8_t codes[6]) {
    int n = 0;
    for (int i = 0; i < 6; i++) codes[i] = 0;
#define PAD_NAV_ADD(c) do { if (n < 6) codes[n++] = (uint8_t)(c); } while (0)
    if (dpad < 8) {
        static const int8_t HX[8] = {  0, +1, +1, +1,  0, -1, -1, -1 };
        static const int8_t HY[8] = { -1, -1,  0, +1, +1, +1,  0, -1 };
        if (HY[dpad]) PAD_NAV_ADD(HY[dpad] > 0 ? PAD_HK_DOWN : PAD_HK_UP);
        if (HX[dpad]) PAD_NAV_ADD(HX[dpad] > 0 ? PAD_HK_RIGHT : PAD_HK_LEFT);
    }
    if (buttons & PAD_BIT(PAD_B_BOTTOM)) PAD_NAV_ADD(PAD_HK_ENTER);
    if (buttons & PAD_BIT(PAD_B_RIGHT)) PAD_NAV_ADD(PAD_HK_ESC);
    if (buttons & PAD_BIT(PAD_B_L1)) PAD_NAV_ADD(PAD_HK_PGUP);
    if (buttons & PAD_BIT(PAD_B_R1)) PAD_NAV_ADD(PAD_HK_PGDN);
    if (buttons & PAD_BIT(PAD_B_LEFT)) PAD_NAV_ADD(PAD_HK_TAB);
#undef PAD_NAV_ADD
    return n;
}

#endif  // GAMEPAD_H

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
// LEFT (port 1). The D-pad overrides the left stick, snapped to the rail. The
// bottom or right face button (cross, circle) or R1 is the right fire button;
// the left face button (square) or L1 the left.

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

struct pad_state {
    uint16_t axis[2][2];    // [port][0 = X, 1 = Y], 0..65535, 0 = left / up
    bool     fire[2];       // [port]
};

static inline void pad_centered(struct pad_state *s) {
    s->axis[0][0] = s->axis[0][1] = s->axis[1][0] = s->axis[1][1] = PAD_CENTER;
    s->fire[0] = s->fire[1] = false;
}

// D-pad as compass directions, 0 = N clockwise to 7 = NW, 8+ = idle, onto
// port 0, snapped to the rails.
static inline void pad_dpad(struct pad_state *s, uint8_t dir) {
    if (dir >= 8) return;
    static const int8_t HX[8] = {  0, +1, +1, +1,  0, -1, -1, -1 };
    static const int8_t HY[8] = { -1, -1,  0, +1, +1, +1,  0, -1 };
    if (HX[dir]) s->axis[0][0] = HX[dir] > 0 ? 65535 : 0;
    if (HY[dir]) s->axis[0][1] = HY[dir] > 0 ? 65535 : 0;
}

// --- DS4 identity ------------------------------------------------------------
// Fixed offsets, measured in FRUITJAM-18 (the identity has no descriptor):
//
//   b0  report ID, always 0x01
//   b1  LX   00 = left,  80 = center, FF = right
//   b2  LY   00 = up,    80 = center, FF = down
//   b3  RX,  b4 RY   (same convention)
//   b5  low nibble  = D-pad: 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW, 8-F idle
//       high nibble = bit4 square, bit5 cross, bit6 circle, bit7 triangle
//   b6  bit0 L1, bit1 R1
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

// False (and *s untouched) if it is not an input report.
static inline bool pad_decode_ds4(const uint8_t *r, uint16_t len, struct pad_state *s) {
    if (len < 7 || r[0] != 0x01) return false;
    s->axis[0][0] = pad_axis(r[1]);
    s->axis[0][1] = pad_axis(r[2]);
    s->axis[1][0] = pad_axis(r[3]);
    s->axis[1][1] = pad_axis(r[4]);
    pad_dpad(s, (uint8_t)(r[5] & 0x0F));
    s->fire[0] = (r[5] & 0x60) || (r[6] & 0x02);   // cross, circle, R1
    s->fire[1] = (r[5] & 0x10) || (r[6] & 0x01);   // square, L1
    return true;
}

// --- GameSir identity: Xbox 360 layout ---------------------------------------
// Its descriptor declares a keyboard (ID 3), a mouse (ID 9) and 63 vendor
// bytes (ID 0x10), and every report it sends is the vendor one: the standard
// Xbox 360 input report (type 0x00, length 0x14) with the report ID 0x10 in
// place of the type byte. Every field below was confirmed by a control-by-
// control sweep on the pad (PIZERO-13, 2026-09-28):
//
//   b0  0x10 (report ID)   b1  0x14 (length)
//   b2  D-pad up 01, down 02, left 04, right 08
//   b3  L1 01, R1 02, bottom (cross) 10, right (circle) 20,
//       left (square) 40, top (triangle) 80
//   b4  L2, b5 R2   analog 0-255 (unused here)
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
    // D-pad bits -> compass direction for pad_dpad.
    bool up = r[2] & 0x01, down = r[2] & 0x02, left = r[2] & 0x04, right = r[2] & 0x08;
    static const uint8_t DIR[3][3] = {   // [y+1][x+1]: y -1 up, x -1 left
        { 7, 0, 1 },
        { 6, 8, 2 },
        { 5, 4, 3 },
    };
    int x = (right ? 1 : 0) - (left ? 1 : 0);
    int y = (down ? 1 : 0) - (up ? 1 : 0);
    pad_dpad(s, DIR[y + 1][x + 1]);
    s->fire[0] = (r[3] & 0x30) || (r[3] & 0x02);   // cross, circle, R1
    s->fire[1] = (r[3] & 0x40) || (r[3] & 0x01);   // square, L1
    return true;
}

// --- GameSir Android mode: standard HID gamepad -----------------------------
// Its 127-byte descriptor declares, as report 1: 15 buttons, a hat switch,
// X Y Z Rz and two analog triggers. Button positions confirmed by a sweep on
// the pad (PIZERO-13, 2026-09-28), the usual Android numbering:
//
//   b0  0x01 (report ID)
//   b1  bit0 bottom (A, cross) = button 1, bit1 right (B, circle) = 2,
//       bit3 left (X, square) = 4, bit4 top (Y, triangle) = 5,
//       bit6 L1 = 7, bit7 R1 = 8
//   b2  buttons 9-15 (unused here)
//   b3  low nibble = hat: 0=N clockwise to 7=NW, 0x0F idle
//   b4 LX, b5 LY, b6 RX, b7 RY   00 = left/up, 80 = center, FF = right/down
//   b8, b9  analog triggers (unused here)
//
// The same stick convention as the DS4 identity, so the same pad_axis.
static inline bool pad_decode_hidgp(const uint8_t *r, uint16_t len, struct pad_state *s) {
    if (len < 10 || r[0] != 0x01) return false;
    s->axis[0][0] = pad_axis(r[4]);
    s->axis[0][1] = pad_axis(r[5]);
    s->axis[1][0] = pad_axis(r[6]);
    s->axis[1][1] = pad_axis(r[7]);
    pad_dpad(s, (uint8_t)(r[3] & 0x0F));
    s->fire[0] = (r[1] & 0x03) || (r[1] & 0x80);   // bottom, right, R1
    s->fire[1] = (r[1] & 0x08) || (r[1] & 0x40);   // left, L1
    return true;
}

static inline bool pad_decode(enum pad_kind k, const uint8_t *r, uint16_t len,
                              struct pad_state *s) {
    if (k == PAD_DS4)    return pad_decode_ds4(r, len, s);
    if (k == PAD_XINPUT) return pad_decode_xinput(r, len, s);
    if (k == PAD_HIDGP)  return pad_decode_hidgp(r, len, s);
    return false;
}

#endif  // GAMEPAD_H

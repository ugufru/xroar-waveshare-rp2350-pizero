// key_translate.h: the USB keyboard types what its keycaps say (PIZERO-163).
//
// Pure logic, host-tested (test/test_key_translate). The old mapping pressed
// the CoCo key in the same POSITION as the USB key and passed Shift straight
// through, so Shift+2 typed '"' and '=' '[' ':' and friends were unreachable.
// Now each USB key (with Shift) becomes the character on its keycap (US
// layout, the editor's tek_ascii), and that character becomes the CoCo chord
// that types it, with the CoCo's SHIFT forced on or off to suit. Ported in
// spirit from upstream XRoar's translated mode (hkbd.c, dkbd.c), which says
// the same caveat: the chords are Color BASIC's, and a program that reads the
// matrix itself sees chords, not keycaps.
//
// Kept as they were, because programs and games rely on them:
//   * letters: Shift passes through, so Shift+letter is still the CoCo's
//     other case (inverse lower case in BASIC);
//   * Enter, Space, the arrows, Esc (BREAK), Backspace and Delete (LEFT):
//     Shift passes through, so Shift+Left still erases a line;
//   * Shift pressed alone is the CoCo's SHIFT.
// New: Home is CLEAR; Caps Lock is the case toggle (SHIFT+0), which frees
// Shift+0 to type ')'; the numeric keypad types its characters (PIZERO-49).
// The CoCo has no { } | ~ or backtick, so those keys press nothing.

#ifndef KEY_TRANSLATE_H
#define KEY_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "text_edit_keys.h"     // tek_ascii: USB key + Shift -> US character

// CoCo keys as XRoar's dkbd scancodes (coco_machine_press_key takes these).
enum {
    K_0 = 0x00, K_1, K_2, K_3, K_4, K_5, K_6, K_7,
    K_8 = 0x08, K_9, K_COLON, K_SEMI, K_COMMA, K_MINUS, K_DOT, K_SLASH,
    K_AT = 0x10, K_A, K_B, K_C, K_D, K_E, K_F, K_G,
    K_H = 0x18, K_I, K_J, K_K, K_L, K_M, K_N, K_O,
    K_P = 0x20, K_Q, K_R, K_S, K_T, K_U, K_V, K_W,
    K_X = 0x28, K_Y, K_Z, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SPACE,
    K_ENTER = 0x30, K_CLEAR, K_BREAK,
    K_SHIFT = 0x37,
    K_INVALID = 0x3F,
};

// What a key does to the CoCo's SHIFT while it is held.
enum { KT_SHIFT_KEEP = 0, KT_SHIFT_ON = 1, KT_SHIFT_OFF = 2 };

// The CoCo chord that types character `c` in Color BASIC. False if the CoCo
// cannot type it. Letters come out unshifted (the machine's normal case).
static inline bool kt_chord(char c, uint8_t *dscan, uint8_t *shift) {
    *shift = KT_SHIFT_OFF;
    if (c >= 'a' && c <= 'z') { *dscan = (uint8_t)(K_A + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { *dscan = (uint8_t)(K_A + (c - 'A')); return true; }
    if (c >= '0' && c <= '9') { *dscan = (uint8_t)(K_0 + (c - '0')); return true; }
    uint8_t d = K_INVALID;
    bool sh = false;
    switch (c) {
    case ' ':  d = K_SPACE; break;
    case '\r': case '\n': d = K_ENTER; break;
    case ':':  d = K_COLON; break;
    case ';':  d = K_SEMI;  break;
    case ',':  d = K_COMMA; break;
    case '-':  d = K_MINUS; break;
    case '.':  d = K_DOT;   break;
    case '/':  d = K_SLASH; break;
    case '@':  d = K_AT;    break;
    case '^':  d = K_UP;    break;          // BASIC reads UP as ^
    case '!':  d = K_1; sh = true; break;
    case '"':  d = K_2; sh = true; break;
    case '#':  d = K_3; sh = true; break;
    case '$':  d = K_4; sh = true; break;
    case '%':  d = K_5; sh = true; break;
    case '&':  d = K_6; sh = true; break;
    case '\'': d = K_7; sh = true; break;
    case '(':  d = K_8; sh = true; break;
    case ')':  d = K_9; sh = true; break;
    case '*':  d = K_COLON; sh = true; break;
    case '+':  d = K_SEMI;  sh = true; break;
    case '<':  d = K_COMMA; sh = true; break;
    case '=':  d = K_MINUS; sh = true; break;
    case '>':  d = K_DOT;   sh = true; break;
    case '?':  d = K_SLASH; sh = true; break;
    case '[':  d = K_DOWN;  sh = true; break;   // Color BASIC's chords (dkbd.c)
    case ']':  d = K_RIGHT; sh = true; break;
    case '\\': d = K_CLEAR; sh = true; break;
    case '_':  d = K_UP;    sh = true; break;
    case 0x08: case 0x7F: d = K_LEFT; break;
    case 0x03: case 0x1B: d = K_BREAK; break;
    case 0x0C: d = K_CLEAR; break;
    default:   return false;
    }
    *dscan = d;
    if (sh) *shift = KT_SHIFT_ON;
    return true;
}

// One USB key (HID usage) with the physical Shift: the CoCo key it presses
// and what it does to SHIFT. False if it presses nothing.
static inline bool kt_translate(uint8_t hid, bool shift, uint8_t *dscan, uint8_t *sh) {
    *sh = KT_SHIFT_KEEP;
    if (hid >= 0x04 && hid <= 0x1D) { *dscan = (uint8_t)(K_A + (hid - 0x04)); return true; }
    switch (hid) {
    case 0x28: case 0x58: *dscan = K_ENTER; return true;   // Enter, keypad Enter
    case 0x29: *dscan = K_BREAK; return true;               // Esc
    case 0x2A: case 0x4C: *dscan = K_LEFT; return true;     // Backspace, Delete
    case 0x2C: *dscan = K_SPACE; return true;
    case 0x4A: *dscan = K_CLEAR; return true;               // Home
    case 0x4F: *dscan = K_RIGHT; return true;
    case 0x50: *dscan = K_LEFT;  return true;
    case 0x51: *dscan = K_DOWN;  return true;
    case 0x52: *dscan = K_UP;    return true;
    case 0x39: *dscan = K_0; *sh = KT_SHIFT_ON; return true;   // Caps Lock: case toggle
    default: break;
    }
    // The numeric keypad types its characters whatever Num Lock says.
    static const char pad[] = "/*-+\0" "123456789" "0.";    // 0x54..0x63
    char c = 0;
    if (hid >= 0x54 && hid <= 0x63) c = pad[hid - 0x54];
    else c = tek_ascii(hid, shift);
    return c && kt_chord(c, dscan, sh);
}

// The keys held on the USB keyboard, each with the CoCo key it chose when it
// went down (so its release lifts the same key even if Shift changed since).
#define KT_HELD_MAX 6
struct kt_held { uint8_t hid, dscan, shift; };
struct kt_state {
    struct kt_held h[KT_HELD_MAX];
    uint8_t n;                          // in press order: h[n-1] is the newest
};

static inline void kt_init(struct kt_state *st) { memset(st, 0, sizeof *st); }

static inline bool kt_in(const uint8_t codes[6], uint8_t hid) {
    for (int i = 0; i < 6; i++) if (codes[i] == hid) return true;
    return false;
}

// One boot report: forget released keys, translate newly pressed ones.
static inline void kt_update(struct kt_state *st, uint8_t mods, const uint8_t codes[6]) {
    bool shift = (mods & 0x22) != 0;
    uint8_t k = 0;
    for (uint8_t i = 0; i < st->n; i++)
        if (kt_in(codes, st->h[i].hid)) st->h[k++] = st->h[i];
    st->n = k;
    for (int i = 0; i < 6 && st->n < KT_HELD_MAX; i++) {
        uint8_t hid = codes[i];
        if (!hid) continue;
        bool known = false;
        for (uint8_t j = 0; j < st->n; j++) if (st->h[j].hid == hid) known = true;
        if (known) continue;
        struct kt_held e = { hid, K_INVALID, KT_SHIFT_KEEP };
        if (!kt_translate(hid, shift, &e.dscan, &e.shift)) e.dscan = K_INVALID;
        st->h[st->n++] = e;
    }
}

// Take whatever is held now as already handled: those keys press nothing
// until released and pressed again. Used when the overlay closes, so the ESC
// that closed it is not a BREAK, and when the machine starts.
static inline void kt_resync(struct kt_state *st, const uint8_t codes[6]) {
    st->n = 0;
    for (int i = 0; i < 6 && st->n < KT_HELD_MAX; i++)
        if (codes[i]) {
            struct kt_held e = { codes[i], K_INVALID, KT_SHIFT_KEEP };
            st->h[st->n++] = e;
        }
}

// The CoCo keys that should be down right now: every held key's CoCo key,
// plus SHIFT as the newest key that forces it says, else as the physical
// Shift. Returns how many were written to out (at most KT_HELD_MAX + 1).
static inline int kt_keys(const struct kt_state *st, uint8_t mods, uint8_t *out) {
    int n = 0;
    uint8_t force = KT_SHIFT_KEEP;
    for (uint8_t i = 0; i < st->n; i++) {
        if (st->h[i].dscan == K_INVALID) continue;
        out[n++] = st->h[i].dscan;
        if (st->h[i].shift != KT_SHIFT_KEEP) force = st->h[i].shift;
    }
    bool shift = force == KT_SHIFT_KEEP ? (mods & 0x22) != 0 : force == KT_SHIFT_ON;
    if (shift) out[n++] = K_SHIFT;
    return n;
}

#endif  // KEY_TRANSLATE_H

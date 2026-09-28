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
struct kt_held { uint8_t hid, dscan, shift; uint16_t seq; };
struct kt_state {
    struct kt_held h[KT_HELD_MAX];
    uint8_t n;                          // in press order: h[n-1] is the newest
    uint16_t seq;                       // counts presses, so a re-press is new
    // PIZERO-167 auto-repeat of the newest key (kt_repeat).
    uint16_t rep_seq;                   // the press being repeated
    uint32_t rep_t0;                    // the frame it went down
    bool     rep_gap;                   // released for now, to be pressed again
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
        struct kt_held e = { hid, K_INVALID, KT_SHIFT_KEEP, ++st->seq };
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
            struct kt_held e = { codes[i], K_INVALID, KT_SHIFT_KEEP, ++st->seq };
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
        if (st->rep_gap && i == st->n - 1) continue;      // auto-repeat's gap
        out[n++] = st->h[i].dscan;
        if (st->h[i].shift != KT_SHIFT_KEEP) force = st->h[i].shift;
    }
    bool shift = force == KT_SHIFT_KEEP ? (mods & 0x22) != 0 : force == KT_SHIFT_ON;
    if (shift) out[n++] = K_SHIFT;
    return n;
}

// PIZERO-164: CoCo keys by name, as the settings file writes them
// (pad_start = enter). A letter, a digit, one of @ : ; , - . / or a word.
// Names are lower case (the settings parser lower-cases values). Returns
// K_INVALID for anything else.
static const struct { const char *name; uint8_t dscan; } KT_KEY_NAMES[] = {
    { "up", K_UP }, { "down", K_DOWN }, { "left", K_LEFT }, { "right", K_RIGHT },
    { "space", K_SPACE }, { "enter", K_ENTER }, { "clear", K_CLEAR },
    { "break", K_BREAK }, { "shift", K_SHIFT },
};

static inline uint8_t kt_key_by_name(const char *v) {
    if (v[0] && !v[1]) {
        char c = v[0];
        if (c >= 'a' && c <= 'z') return (uint8_t)(K_A + (c - 'a'));
        if (c >= '0' && c <= '9') return (uint8_t)(K_0 + (c - '0'));
        switch (c) {
        case '@': return K_AT;    case ':': return K_COLON; case ';': return K_SEMI;
        case ',': return K_COMMA; case '-': return K_MINUS; case '.': return K_DOT;
        case '/': return K_SLASH;
        default:  return K_INVALID;
        }
    }
    for (unsigned i = 0; i < sizeof KT_KEY_NAMES / sizeof KT_KEY_NAMES[0]; i++)
        if (!strcmp(v, KT_KEY_NAMES[i].name)) return KT_KEY_NAMES[i].dscan;
    return K_INVALID;
}

// The name kt_key_by_name reads back as `dscan`, into out (at least 6 bytes).
static inline bool kt_key_name(uint8_t dscan, char *out) {
    for (unsigned i = 0; i < sizeof KT_KEY_NAMES / sizeof KT_KEY_NAMES[0]; i++)
        if (KT_KEY_NAMES[i].dscan == dscan) { strcpy(out, KT_KEY_NAMES[i].name); return true; }
    char c = 0;
    if (dscan >= K_A && dscan <= K_Z) c = (char)('a' + (dscan - K_A));
    else if (dscan <= K_9) c = (char)('0' + dscan);
    else switch (dscan) {
        case K_AT: c = '@'; break;    case K_COLON: c = ':'; break;
        case K_SEMI: c = ';'; break;  case K_COMMA: c = ','; break;
        case K_MINUS: c = '-'; break; case K_DOT: c = '.'; break;
        case K_SLASH: c = '/'; break;
        default: return false;
    }
    out[0] = c; out[1] = 0;
    return true;
}

// PIZERO-167: auto-repeat. Color BASIC on a CoCo 1/2 does not repeat a held
// key, so the newest held key is released and pressed again on a timer:
// after `delay` frames, a KT_REPEAT_GAP-frame release every `period` frames,
// which BASIC sees as a new press each time. BREAK never repeats. Called once
// a frame; returns true when the keys to hold changed (redraw with kt_keys).
#define KT_REPEAT_GAP     2    // frames released per repeat
#define KT_REPEAT_MIN     5    // shortest period: BASIC must see each press

static inline bool kt_repeat(struct kt_state *st, uint32_t frame, bool on,
                             uint32_t delay, uint32_t period) {
    bool was = st->rep_gap;
    const struct kt_held *k = st->n ? &st->h[st->n - 1] : 0;
    if (!on || !k || k->dscan == K_INVALID || k->dscan == K_BREAK) {
        st->rep_gap = false;
        st->rep_seq = k ? k->seq : 0;
        st->rep_t0 = frame;
        return was != st->rep_gap;
    }
    if (k->seq != st->rep_seq) {                      // a new press: start timing
        st->rep_seq = k->seq;
        st->rep_t0 = frame;
        st->rep_gap = false;
        return was;
    }
    if (period < KT_REPEAT_MIN) period = KT_REPEAT_MIN;
    uint32_t t = frame - st->rep_t0;
    st->rep_gap = t >= delay && ((t - delay) % period) < KT_REPEAT_GAP;
    return was != st->rep_gap;
}

// Settings to frames at 60 per second: delay in milliseconds, rate in
// repeats per second (clamped so each press lasts long enough to be seen).
static inline uint32_t kt_repeat_delay_frames(uint16_t ms) { return (uint32_t)ms * 60u / 1000u; }
static inline uint32_t kt_repeat_period_frames(uint8_t rate) {
    uint32_t p = rate ? 60u / rate : 60u;
    return p < KT_REPEAT_MIN ? KT_REPEAT_MIN : p;
}

#endif  // KEY_TRANSLATE_H

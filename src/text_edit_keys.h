// text_edit_keys.h: what the keyboard does in the on-screen editor
// (PIZERO-146). Pure logic, host-tested: fed each USB HID boot report
// (modifier byte and six key codes), it edits a struct text_edit and says
// what the caller should do.
//
// Keys: printable keys insert (US layout, Shift for upper case and symbols);
// Enter splits the line; Backspace and Delete delete; the arrows, Home/End
// and PgUp/PgDn move. Held keys repeat. Ctrl-S saves. ESC cancels, except
// that with unsaved changes the first ESC only warns and a second one
// (with nothing else pressed between) discards them.

#ifndef TEXT_EDIT_KEYS_H
#define TEXT_EDIT_KEYS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "text_edit.h"

// The editor's fixed words, here so the host test can check they fit the
// 32-column card and use only characters the 6847 can draw.
#define TEK_HINT        "CTRL-S SAVE   ESC EXIT"
#define TEK_MSG_SAVED   "SAVED"
#define TEK_MSG_APPLIED "SAVED AND APPLIED"
#define TEK_MSG_NEXT    "SAVED: USED AT NEXT POWER-ON"
#define TEK_MSG_FAILED  "SAVE FAILED: CHECK THE SD CARD"
#define TEK_MSG_WARN    "NOT SAVED: ESC AGAIN DISCARDS"
#define TEK_MSG_FULL    "FILE FULL: 4096 CHARACTERS"
#define TEK_MSG_NEW     "NEW FILE: CTRL-S TO CREATE IT"
#define TEK_MSG_NOMEM   "NOT ENOUGH MEMORY TO EDIT"

#define TEK_ROWS          14   // text rows on screen
#define TEK_COLS          32
#define TEK_REPEAT_DELAY  24   // frames (~400 ms) before a held key repeats
#define TEK_REPEAT_RATE    3   // frames (~50 ms) between repeats

enum tek_action {
    TEK_NONE = 0,
    TEK_CHANGED,       // text or cursor moved: redraw
    TEK_SAVE,          // Ctrl-S
    TEK_CANCEL,        // leave without saving
    TEK_WARN_UNSAVED,  // ESC with changes: say so, ESC again to discard
};

struct tek_state {
    uint8_t  prev[6];
    uint8_t  held;             // key code being repeated, or 0
    uint8_t  held_mods;
    uint32_t next_repeat;
    bool     esc_armed;        // the last key was an ESC that warned
};

static inline void tek_init(struct tek_state *k) { memset(k, 0, sizeof *k); }

// US-layout character for a HID key code, or 0 if it is not printable.
static inline char tek_ascii(uint8_t code, bool shift) {
    if (code >= 0x04 && code <= 0x1D) return (char)((shift ? 'A' : 'a') + (code - 0x04));
    if (code >= 0x1E && code <= 0x27) {
        static const char plain[] = "1234567890", shifted[] = "!@#$%^&*()";
        return (shift ? shifted : plain)[code - 0x1E];
    }
    switch (code) {
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default:   return 0;
    }
}

// One key press (a new press or a repeat) applied to the text.
static inline uint8_t tek_apply(struct tek_state *k, struct text_edit *t,
                                uint8_t code, uint8_t mods) {
    bool shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0;
    if (code == 0x29) {                                  // ESC
        if (!t->dirty || k->esc_armed) return TEK_CANCEL;
        k->esc_armed = true;
        return TEK_WARN_UNSAVED;
    }
    k->esc_armed = false;
    if (ctrl) return (code == 0x16) ? TEK_SAVE : TEK_NONE;   // Ctrl-S
    switch (code) {
    case 0x28: case 0x58: ted_insert(t, '\n'); break;    // Enter, keypad Enter
    case 0x2A: ted_backspace(t); break;
    case 0x4C: ted_delete(t);    break;
    case 0x50: ted_left(t);      break;
    case 0x4F: ted_right(t);     break;
    case 0x52: ted_vertical(t, -1);        break;
    case 0x51: ted_vertical(t, +1);        break;
    case 0x4B: ted_vertical(t, -TEK_ROWS); break;        // PgUp
    case 0x4E: ted_vertical(t, +TEK_ROWS); break;        // PgDn
    case 0x4A: ted_home(t);      break;
    case 0x4D: ted_end(t);       break;
    default: {
        char c = tek_ascii(code, shift);
        if (!c) return TEK_NONE;
        ted_insert(t, c);
    }
    }
    ted_follow(t, TEK_ROWS, TEK_COLS);
    return TEK_CHANGED;
}

static inline bool tek_has(const uint8_t codes[6], uint8_t c) {
    for (int i = 0; i < 6; i++) if (codes[i] == c) return true;
    return false;
}

// Feed one HID report. Applies the newest key pressed in it (a report can
// hold several; the one not held last time is the new one) and arms repeat.
static inline uint8_t tek_report(struct tek_state *k, struct text_edit *t,
                                 uint8_t mods, const uint8_t codes[6], uint32_t frame) {
    uint8_t action = TEK_NONE;
    for (int i = 0; i < 6; i++) {
        uint8_t c = codes[i];
        if (!c || tek_has(k->prev, c)) continue;
        action = tek_apply(k, t, c, mods);
        k->held = (c == 0x29) ? 0 : c;                   // ESC never repeats
        k->held_mods = mods;
        k->next_repeat = frame + TEK_REPEAT_DELAY;
        if (action == TEK_SAVE || action == TEK_CANCEL) k->held = 0;
        break;
    }
    if (k->held && !tek_has(codes, k->held)) k->held = 0;   // released
    if (k->held) k->held_mods = mods;
    memcpy(k->prev, codes, 6);
    return action;
}

// Once a frame: key repeat for the held key.
static inline uint8_t tek_tick(struct tek_state *k, struct text_edit *t, uint32_t frame) {
    if (!k->held || (int32_t)(frame - k->next_repeat) < 0) return TEK_NONE;
    k->next_repeat = frame + TEK_REPEAT_RATE;
    uint8_t a = tek_apply(k, t, k->held, k->held_mods);
    return (a == TEK_SAVE) ? TEK_NONE : a;               // Ctrl-S never auto-repeats
}

#endif  // TEXT_EDIT_KEYS_H

// overlay_keys.h: what the keyboard does to the F12 disk overlay
// (PIZERO-81c, PIZERO-114). Pure logic, host-tested: fed the 6 keycodes of
// each USB HID boot report, it says whether the report belongs to the
// overlay (and so must never reach the CoCo) and what it asks for.
//
// The rules that keep keys out of BASIC, most learned by the Fruit Jam port
// (src/coco/coco_main.cpp:1512-1644):
//   * CLOSED, only F12 is claimed. Everything else, ESC included (it is the
//     CoCo's BREAK key), passes through untouched.
//   * OPEN, every report is swallowed.
//   * Presses are edges against the overlay's OWN previous report, so a key
//     held across the open or close is not a fresh press to either side.
//     The caller copies the live report into its own previous-codes on close
//     (FRUITJAM-68: otherwise the ESC that closed it arrives as a BREAK).
//
// Keys while open: Up/Down move (wrapping), PgUp/PgDn/Home/End jump
// (clamped), Left/Right switch list (disks, programs, cartridges), ENTER
// starts the highlighted entry, 0-3 toggle the highlighted disk in that
// drive, F12 or ESC close. Held Up/Down repeat.

#ifndef OVERLAY_KEYS_H
#define OVERLAY_KEYS_H

#include <stdint.h>
#include <string.h>

// HID usage codes (keyboard page)
#define HK_1      0x1E   // 1..9 are 0x1E..0x26, 0 is 0x27
#define HK_0      0x27
#define HK_ENTER  0x28
#define HK_ESC    0x29
#define HK_F12    0x45
#define HK_HOME   0x4A
#define HK_PGUP   0x4B
#define HK_END    0x4D
#define HK_PGDN   0x4E
#define HK_RIGHT  0x4F
#define HK_LEFT   0x50
#define HK_DOWN   0x51
#define HK_UP     0x52
#define HK_KP_ENTER 0x58

// The overlay's fixed words, here so the host test can check they fit the
// 32-column card and use only characters the 6847 can draw.
// One set per list, indexed by enum cat_kind (DSK, BIN, CART).
#define OVL_TITLE_DSK    "< DISKS >"
#define OVL_TITLE_BIN    "< PROGRAMS >"
#define OVL_TITLE_CART   "< CARTRIDGES >"
#define OVL_LEGEND_DSK   "0-3 DRIVE  ENTER BOOT  <> TYPE"
#define OVL_LEGEND_BIN   "ENTER RUN  <> TYPE  ESC EXIT"
#define OVL_LEGEND_CART  "ENTER START  <> TYPE  ESC EXIT"
#define OVL_EMPTY_DSK    "NO DISK IMAGES FOUND. PUT .DSK FILES IN /COCO/DSK ON THE SD CARD."
#define OVL_EMPTY_BIN    "NO PROGRAMS FOUND. PUT .BIN FILES IN /COCO/BIN ON THE SD CARD."
#define OVL_EMPTY_CART   "NO CARTRIDGES FOUND. PUT 8K .ROM OR .CCC FILES IN /COCO/ROMS ON THE SD CARD."
#define OVL_SKIPPED      "%d NAME(S) TOO LONG, NOT SHOWN"

#define OVK_ROWS          14   // list rows on screen: one page
#define OVK_REPEAT_DELAY  24   // frames (~400 ms at 60 Hz) before a held key repeats
#define OVK_REPEAT_RATE    5   // frames (~83 ms) between repeats

enum ovk_action {
    OVK_NONE = 0,
    OVK_OPEN,        // caller: release all CoCo keys, rescan, draw
    OVK_CLOSE,       // caller: copy this report into its previous-codes, resume
    OVK_MOVED,       // selection changed: redraw
    OVK_DRIVE,       // toggle the highlighted disk in drive `drive`
    OVK_KIND,        // switch list: `drive` holds the direction, -1 or +1
    OVK_LAUNCH,      // ENTER: start the highlighted entry
};

struct ovk_result {
    uint8_t action;
    int8_t  drive;       // OVK_DRIVE: 0-3; OVK_KIND: -1 or +1
    bool    swallow;     // true: this report must not reach the CoCo
};

struct ovk_state {
    bool     open;
    uint8_t  prev[6];    // the last report, for edge detection
    int      sel;        // highlighted row, 0..n-1
    int      n;          // entries in the list
    uint8_t  held;       // HK_UP/HK_DOWN being held for repeat, or 0
    uint32_t next_repeat;
};

static inline void ovk_init(struct ovk_state *s) {
    memset(s, 0, sizeof *s);
}

static inline bool ovk_has(const uint8_t codes[6], uint8_t k) {
    for (int i = 0; i < 6; i++) if (codes[i] == k) return true;
    return false;
}

static inline bool ovk_newly(const struct ovk_state *s, const uint8_t codes[6], uint8_t k) {
    return ovk_has(codes, k) && !ovk_has(s->prev, k);
}

// Drive number for a digit key, or -1.
static inline int ovk_drive_key(uint8_t k) {
    if (k == HK_0) return 0;
    if (k >= HK_1 && k <= HK_1 + 2) return k - HK_1 + 1;
    return -1;
}

static inline void ovk_step(struct ovk_state *s, int delta, bool wrap) {
    if (s->n <= 0) { s->sel = 0; return; }
    int v = s->sel + delta;
    if (wrap) v = ((v % s->n) + s->n) % s->n;
    else if (v < 0) v = 0;
    else if (v >= s->n) v = s->n - 1;
    s->sel = v;
}

// Set the list size (after a rescan) and keep the selection in range.
static inline void ovk_set_count(struct ovk_state *s, int n) {
    s->n = n < 0 ? 0 : n;
    if (s->sel >= s->n) s->sel = s->n ? s->n - 1 : 0;
    if (s->sel < 0) s->sel = 0;
}

// Feed one HID report. `frame` is a free-running frame counter, used only to
// time key repeat.
static inline struct ovk_result ovk_report(struct ovk_state *s,
                                           const uint8_t codes[6], uint32_t frame) {
    struct ovk_result r = { OVK_NONE, -1, false };

    if (!s->open) {
        if (ovk_newly(s, codes, HK_F12)) {
            s->open = true;
            s->held = 0;
            r.action = OVK_OPEN;
            r.swallow = true;
        }
        memcpy(s->prev, codes, 6);
        return r;
    }

    r.swallow = true;              // open: nothing reaches the CoCo
    if (ovk_newly(s, codes, HK_F12) || ovk_newly(s, codes, HK_ESC)) {
        s->open = false;
        s->held = 0;
        r.action = OVK_CLOSE;
        memcpy(s->prev, codes, 6);
        return r;
    }

    int before = s->sel;
    if (ovk_newly(s, codes, HK_DOWN)) {
        ovk_step(s, +1, true);
        s->held = HK_DOWN; s->next_repeat = frame + OVK_REPEAT_DELAY;
    } else if (ovk_newly(s, codes, HK_UP)) {
        ovk_step(s, -1, true);
        s->held = HK_UP; s->next_repeat = frame + OVK_REPEAT_DELAY;
    } else if (ovk_newly(s, codes, HK_PGDN)) {
        ovk_step(s, +OVK_ROWS, false);
    } else if (ovk_newly(s, codes, HK_PGUP)) {
        ovk_step(s, -OVK_ROWS, false);
    } else if (ovk_newly(s, codes, HK_LEFT) || ovk_newly(s, codes, HK_RIGHT)) {
        r.action = OVK_KIND;
        r.drive = ovk_newly(s, codes, HK_RIGHT) ? +1 : -1;
    } else if (ovk_newly(s, codes, HK_ENTER) || ovk_newly(s, codes, HK_KP_ENTER)) {
        if (s->n > 0) r.action = OVK_LAUNCH;
    } else if (ovk_newly(s, codes, HK_HOME)) {
        s->sel = 0;
    } else if (ovk_newly(s, codes, HK_END)) {
        s->sel = s->n ? s->n - 1 : 0;
    } else {
        for (int i = 0; i < 6; i++) {
            int d = ovk_drive_key(codes[i]);
            if (d >= 0 && !ovk_has(s->prev, codes[i])) {
                if (s->n > 0) { r.action = OVK_DRIVE; r.drive = (int8_t)d; }
                break;
            }
        }
    }
    if (s->held && !ovk_has(codes, s->held)) s->held = 0;   // released
    if (r.action == OVK_NONE && s->sel != before) r.action = OVK_MOVED;
    memcpy(s->prev, codes, 6);
    return r;
}

// Call once a frame while open: auto-repeat for a held Up/Down. Returns
// OVK_MOVED when the selection changed.
static inline uint8_t ovk_tick(struct ovk_state *s, uint32_t frame) {
    if (!s->open || !s->held || (int32_t)(frame - s->next_repeat) < 0) return OVK_NONE;
    int before = s->sel;
    ovk_step(s, s->held == HK_DOWN ? +1 : -1, true);
    s->next_repeat = frame + OVK_REPEAT_RATE;
    return s->sel != before ? OVK_MOVED : OVK_NONE;
}

// First visible row: keeps the selection on screen, moving the window as
// little as possible from where it was.
static inline int ovk_top(int sel, int top, int n) {
    if (n <= OVK_ROWS) return 0;
    if (sel < top) top = sel;
    if (sel >= top + OVK_ROWS) top = sel - OVK_ROWS + 1;
    if (top > n - OVK_ROWS) top = n - OVK_ROWS;
    return top < 0 ? 0 : top;
}

#endif  // OVERLAY_KEYS_H

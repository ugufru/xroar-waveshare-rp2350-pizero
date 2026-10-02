// disk_overlay.cpp: the F12 overlay (PIZERO-81, PIZERO-114, PIZERO-136).
//
// Three lists, one at a time, switched with Left/Right: disks, programs
// (.bin) and cartridges. Each is its own context, so no one list grows long
// with everything on the card:
//
//   DISKS       0-3 put the highlighted disk in that drive or take it out
//               (hot swap, no reset). ENTER cold-boots with it in drive 0 and
//               runs its first program.
//   PROGRAMS    ENTER cold-boots and loads and runs the .bin directly.
//   CARTRIDGES  ENTER installs the cartridge and cold-boots into it.
//   All         ESC goes back to the running program untouched, as does the
//               F key of the list on show (F9 programs, F10 cartridges,
//               F11 files, F12 disks; PIZERO-153).
//
// Layout on the 32x16 card, after the Fruit Jam overlay
// (src/coco/coco_main.cpp:1092-1175) but drawn with our own card:
//
//   row 0      title bar, inverse, naming the list
//   rows 1-14  one entry per row, the selected row inverse. In DISKS, four
//              drive columns first (the drive number where that disk is
//              mounted, '-' where not). Names without their extension.
//   row 15     status: the key legend, or what the last key did
//
// Everything here runs on core 0 from the USB host task or loop(), with the
// machine paused, so mounting a file never races an FDC read. Launching is
// the caller's (main.cpp owns the ROM buffers and the boot sequence): the
// overlay only asks, and closes if the request is accepted.

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "disk_overlay.h"
#include "overlay_keys.h"
#include "dsk_catalog.h"
#include "text_card.h"
#include "coco_boot.h"
#include "coco_machine.h"
#include "text_editor.h"
#include "settings.h"
#include "file_templates.h"

#define OVL_NDRIVE 4

// The three catalogue lists, then FILES (PIZERO-146): text files to edit.
#define OVL_FILES  CAT_KINDS
#define OVL_INFO   (CAT_KINDS + 1)      // PIZERO-156: firmware and board facts
#define OVL_LISTS  (CAT_KINDS + 2)
// PIZERO-153: the F keys name lists by overlay_keys.h's numbers.
static_assert(OVK_LIST_DSK == CAT_DSK && OVK_LIST_BIN == CAT_BIN &&
              OVK_LIST_CART == CAT_CART && OVK_LIST_FILES == OVL_FILES &&
              OVK_LIST_INFO == OVL_INFO,
              "overlay_keys.h list numbers must match the overlay's lists");
static const char *const k_title[OVL_LISTS]  = { OVL_TITLE_DSK,  OVL_TITLE_BIN,  OVL_TITLE_CART,  OVL_TITLE_FILES,  OVL_TITLE_INFO };
static const char *const k_legend[OVL_LISTS] = { OVL_LEGEND_DSK, OVL_LEGEND_BIN, OVL_LEGEND_CART, OVL_LEGEND_FILES, OVL_LEGEND_INFO };

// The files the editor can open. Listed whether or not they exist yet: the
// editor starts a missing one from a template (NULL: the settings template,
// generated from the defaults). `tidy`: a settings file, opened comments
// first and then sorted by name (PIZERO-181).
struct ovl_file { const char *name, *path, *tmpl; bool tidy; };
static const struct ovl_file k_files[] = {
    { "SETTINGS.TXT", "0:/coco/settings.txt", nullptr,          true  },
    { "AUTORUN.TXT",  "0:/coco/autorun.txt",  AUTORUN_TEMPLATE, false },   // PIZERO-147
};
#define OVL_NFILES ((int)(sizeof k_files / sizeof k_files[0]))
static const char *const k_empty[CAT_KINDS]  = { OVL_EMPTY_DSK,  OVL_EMPTY_BIN,  OVL_EMPTY_CART };

static struct ovk_state g_ovk;
static disk_overlay_present_fn g_present;
static disk_overlay_launch_fn  g_launch;
static disk_overlay_info_fn    g_info;       // PIZERO-156
static int  g_kind = CAT_DSK;   // the list on show; kept between openings
static int  g_top;              // first visible list row
static bool g_dirty;
static char g_status[CARD_COLS + 1];

static void set_status(const char *s) {
    snprintf(g_status, sizeof g_status, "%s", s);
    g_dirty = true;
}

static void legend(void) { set_status(k_legend[g_kind]); }

// Which drives hold catalogue entry i, as a bit mask. Disks only.
static unsigned drives_holding(const struct dsk_catalog *cat, int i) {
    unsigned mask = 0;
    if (cat->kind != CAT_DSK) return 0;
    for (unsigned d = 0; d < OVL_NDRIVE; d++) {
        const char *p = coco_boot_drive_path(d);
        if (p && dsk_cat_find_path(cat, p) == i) mask |= 1u << d;
    }
    return mask;
}

static void draw(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    coco_boot_card_clear();
    coco_boot_card_center(0, k_title[g_kind]);
    coco_boot_card_invert_row(0);

    if (g_kind == OVL_INFO) {
        char lines[CARD_ROWS - 2][CARD_COLS + 1];
        int n = g_info ? g_info(lines, CARD_ROWS - 2) : 0;
        for (int i = 0; i < n; i++) coco_boot_card_text(0, 1 + i, lines[i]);
    } else if (g_kind == OVL_FILES) {
        for (int i = 0; i < OVL_NFILES; i++) {
            coco_boot_card_text(0, 1 + i, k_files[i].name);
            if (i == g_ovk.sel) coco_boot_card_invert_row(1 + i);
        }
    } else if (cat->n == 0) {
        coco_boot_card_wrap(2, 3, 28, 6, k_empty[g_kind]);
    } else {
        g_top = ovk_top(g_ovk.sel, g_top, cat->n);
        for (int r = 0; r < OVK_ROWS && g_top + r < cat->n; r++) {
            int i = g_top + r;
            char line[CARD_COLS + 1];
            int col = 0;
            if (g_kind == CAT_DSK) {
                unsigned mask = drives_holding(cat, i);
                for (int d = 0; d < OVL_NDRIVE; d++)
                    line[col++] = (mask & (1u << d)) ? (char)('0' + d) : '-';
                line[col++] = ' ';
            }
            dsk_cat_display_name(cat->e[i].name, line + col, CARD_COLS - col);
            coco_boot_card_text(0, 1 + r, line);
            if (i == g_ovk.sel) coco_boot_card_invert_row(1 + r);
        }
    }
    coco_boot_card_text(0, CARD_ROWS - 1, g_status);
    if (g_present) g_present();
    g_dirty = false;
}

// Rescan the list on show. A skipped-file notice replaces the legend until
// the next key, so it cannot hide the controls for good.
static void load_list(void) {
    if (g_kind == OVL_FILES) { ovk_set_count(&g_ovk, OVL_NFILES); legend(); return; }
    if (g_kind == OVL_INFO)  { ovk_set_count(&g_ovk, 0); legend(); return; }
    int n = coco_boot_rescan(g_kind);
    ovk_set_count(&g_ovk, n);
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    if (cat->skipped_long) {
        char msg[CARD_COLS + 1];
        snprintf(msg, sizeof msg, OVL_SKIPPED, cat->skipped_long);
        set_status(msg);
    } else {
        legend();
    }
}

// On open, and in DISKS, put the cursor on the disk in the drive the program
// last used, so F12 straight after a disk error lands on the disk concerned.
static void open_now(void) {
    coco_machine_release_all_keys();      // nothing stays held in BASIC
    load_list();                          // a swapped card is seen at once
    if (g_kind == CAT_DSK) {
        const char *p = coco_boot_drive_path(coco_machine_fdc_drive());
        int i = p ? dsk_cat_find_path(coco_boot_dsk_catalog(), p) : -1;
        if (i >= 0) g_ovk.sel = i;
    }
    Serial.printf("[overlay] open: list %d, %d entries\r\n", g_kind, g_ovk.n);
}

// Show list `kind`, from its top. The key logic is told, so the F key of
// the list now on show closes the overlay.
static void go_to_list(int kind) {
    g_kind = kind;
    g_ovk.list = kind;
    g_ovk.sel = 0;
    g_top = 0;
    load_list();
}

static void switch_kind(int dir) {
    go_to_list((g_kind + dir + OVL_LISTS) % OVL_LISTS);
}

static void toggle_drive(int d) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    int i = g_ovk.sel;
    if (g_kind != CAT_DSK || i < 0 || i >= cat->n) return;
    char name[20];
    dsk_cat_display_name(cat->e[i].name, name, 16);   // leave room for the words
    char msg[CARD_COLS + 1];
    if (drives_holding(cat, i) & (1u << d)) {
        coco_boot_eject_drive((unsigned)d);
        snprintf(msg, sizeof msg, "DRIVE %d EMPTY", d);
    } else {
        char path[DSK_NAME_MAX + 16];
        if (dsk_cat_path(cat, i, path, sizeof path) && coco_boot_mount_drive((unsigned)d, path))
            snprintf(msg, sizeof msg, "%s IN DRIVE %d", name, d);
        else
            snprintf(msg, sizeof msg, "CANNOT OPEN %s", name);
    }
    set_status(msg);
}

// PIZERO-146/147: ENTER in FILES opens the editor over the list. A missing
// file starts from its template: every setting at its default for
// settings.txt, commented examples for autorun.txt.
static void edit_file(const uint8_t codes[6]) {
    int i = g_ovk.sel;
    if (i < 0 || i >= OVL_NFILES) return;
    static char gen[1024];                   // PIZERO-164: the pad lines made it longer
    const char *tmpl = k_files[i].tmpl;
    if (!tmpl) { settings_template(gen, sizeof gen); tmpl = gen; }
    if (text_editor_open(k_files[i].path, k_files[i].name, tmpl, k_files[i].tidy))
        text_editor_hold(codes);            // the ENTER that opened it is not typed
}

// PIZERO-154: Tab on a disk, program or cartridge edits its own settings
// file, NAME.TXT beside it, from a template of commented examples if new.
static void edit_game_settings(const uint8_t codes[6]) {
    if (g_kind == OVL_FILES || g_kind == OVL_INFO) return;
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    char game[DSK_NAME_MAX + 16], path[DSK_NAME_MAX + 16];
    if (!dsk_cat_path(cat, g_ovk.sel, game, sizeof game)) return;
    if (!settings_game_path(game, path, sizeof path)) { set_status(OVL_NO_SETTINGS); return; }
    const char *slash = strrchr(path, '/');
    if (text_editor_open(path, slash ? slash + 1 : path, GAME_TEMPLATE, true))
        text_editor_hold(codes);            // the Tab that opened it is not typed
}

// Returns true when the launch was accepted and the overlay has closed.
static bool launch(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    char path[DSK_NAME_MAX + 16];
    if (!g_launch || !dsk_cat_path(cat, g_ovk.sel, path, sizeof path)) return false;
    char msg[CARD_COLS + 1] = "";
    if (!g_launch(g_kind, path, msg, sizeof msg)) {
        set_status(msg[0] ? msg : "CANNOT START THAT");
        return false;
    }
    g_ovk.open = false;
    g_ovk.held = 0;
    Serial.printf("[overlay] launch %s\r\n", path);
    return true;
}

void disk_overlay_init(disk_overlay_present_fn present, disk_overlay_launch_fn launch_fn) {
    ovk_init(&g_ovk);
    g_present = present;
    g_launch = launch_fn;
    g_top = 0;
}

void disk_overlay_set_info(disk_overlay_info_fn fn) { g_info = fn; }

bool disk_overlay_is_open(void) {
    return g_ovk.open;
}

void disk_overlay_open(void) {
    if (g_ovk.open) return;
    g_ovk.open = true;
    g_ovk.list = g_kind;
    g_ovk.held = 0;
    open_now();
}

bool disk_overlay_key(uint8_t mods, const uint8_t codes[6], uint32_t frame, bool *closed) {
    // PIZERO-146: while the editor is open every key is the editor's. When it
    // closes, the list takes the keyboard back as it stands, so the ESC that
    // closed the editor does not also close the overlay.
    if (text_editor_is_open()) {
        text_editor_key(mods, codes, frame);
        if (!text_editor_is_open()) {
            memcpy(g_ovk.prev, codes, 6);
            g_ovk.held = 0;
            legend();
        }
        if (closed) *closed = false;
        return true;
    }
    struct ovk_result r = ovk_report(&g_ovk, codes, frame);
    bool shut = (r.action == OVK_CLOSE);
    switch (r.action) {
    case OVK_OPEN:                          // PIZERO-153: opened on the F key's list
        if (r.drive != g_kind) { g_kind = r.drive; g_ovk.sel = 0; g_top = 0; }
        open_now();
        break;
    case OVK_GOTO:   go_to_list(r.drive); break;
    case OVK_EDIT:   edit_game_settings(codes); break;
    case OVK_CLOSE:  Serial.print("[overlay] close\r\n"); break;
    case OVK_MOVED:  legend(); break;
    case OVK_DRIVE:  toggle_drive(r.drive); break;
    case OVK_KIND:   switch_kind(r.drive); break;
    case OVK_LAUNCH:
        if (g_kind == OVL_FILES) edit_file(codes);
        else shut = launch();
        break;
    default: break;
    }
    if (closed) *closed = shut;
    return r.swallow;
}

void disk_overlay_frame(uint32_t frame) {
    if (!g_ovk.open) return;
    if (text_editor_is_open()) { text_editor_frame(frame); return; }
    if (ovk_tick(&g_ovk, frame) == OVK_MOVED) legend();
    if (g_kind == OVL_INFO && frame % 60 == 0) g_dirty = true;   // PIZERO-156: uptime ticks
    if (g_dirty) draw();
}

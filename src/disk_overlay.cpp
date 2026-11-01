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
#include "text_edit_keys.h"          // tek_ascii, for the new-disk name (PIZERO-193)
#include "settings.h"
#include "file_templates.h"

#define OVL_NDRIVE 4

// The three catalogue lists, then FILES (PIZERO-146): text files to edit.
// PIZERO-296: FILES is settings.txt and autorun.txt, then every text file
// under /coco, scanned into the shared catalogue as kind CAT_TEXT.
#define OVL_FILES  CAT_TEXT
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
// `view`: opened read-only. PIZERO-313: drives.txt is edited like any text
// file; it is read at boot, and a mount, eject or lock in F12 rewrites it.
struct ovl_file { const char *name, *path, *tmpl; bool tidy, view; };
static const struct ovl_file k_files[] = {
    { "SETTINGS.TXT", "0:/coco/settings.txt", nullptr,          true,  false },
    { "AUTORUN.TXT",  "0:/coco/autorun.txt",  AUTORUN_TEMPLATE, false, false },   // PIZERO-147
    { "DRIVES.TXT",   "0:/coco/drives.txt",   "",               false, false },   // PIZERO-312/313: read at boot
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

// PIZERO-296: FILES row i: the fixed files first, then the scanned ones.
static const char *files_name(const struct dsk_catalog *cat, int i) {
    if (i < OVL_NFILES) return k_files[i].name;
    i -= OVL_NFILES;
    return (cat->kind == CAT_TEXT && i < cat->n) ? cat->e[i].name : "";
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
        int n = g_ovk.n;
        g_top = ovk_top(g_ovk.sel, g_top, n);
        for (int r = 0; r < OVK_ROWS && g_top + r < n; r++) {
            int i = g_top + r;
            char line[CARD_COLS + 1];
            snprintf(line, sizeof line, "%s", files_name(cat, i));   // clipped to the row
            coco_boot_card_text(0, 1 + r, line);
            if (i == g_ovk.sel) coco_boot_card_invert_row(1 + r);
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
    if (g_kind == OVL_INFO)  { ovk_set_count(&g_ovk, 0); legend(); return; }
    int n = coco_boot_rescan(g_kind);                   // FILES: CAT_TEXT (PIZERO-296)
    ovk_set_count(&g_ovk, g_kind == OVL_FILES ? OVL_NFILES + n : n);
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
    coco_boot_flush_drives();             // PIZERO-66: pending writes land before a swap
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

// PIZERO-66: W flips the write-protect tab on every drive holding the
// highlighted disk. A read-only file cannot be unlocked from here.
static void toggle_protect(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    int i = g_ovk.sel;
    if (g_kind != CAT_DSK || i < 0 || i >= cat->n) return;
    unsigned mask = drives_holding(cat, i);
    if (!mask) { set_status("PUT IT IN A DRIVE FIRST"); return; }
    char msg[CARD_COLS + 1] = "";
    for (unsigned d = 0; d < OVL_NDRIVE; d++) {
        if (!(mask & (1u << d))) continue;
        bool on = !coco_boot_drive_protected(d);
        if (coco_boot_set_drive_protected(d, on))
            snprintf(msg, sizeof msg, "DRIVE %u %s", d, on ? "LOCKED: READ-ONLY" : "WRITABLE");
        else
            snprintf(msg, sizeof msg, "DRIVE %u: FILE IS READ-ONLY", d);
    }
    set_status(msg);
}

// PIZERO-193: N asks for a name in the status row, then makes the blank
// disk and lands on it, so 0-3 mounts it. PIZERO-196: R asks the same way,
// pre-filled, and renames. Letters, digits, space, - and _, as typed, up
// to 24 (what the row holds); Backspace, Enter, Esc.
#define NEW_NAME_MAX 24
static bool    g_naming;
static bool    g_renaming;                 // the prompt is R's, not N's
static char    g_new_name[NEW_NAME_MAX + 1];
static int     g_rename_sel;               // the entry R is renaming
static uint8_t g_naming_prev[6];

static void naming_show(void) {
    char msg[CARD_COLS + 1];
    snprintf(msg, sizeof msg, "NAME: %s_", g_new_name);
    set_status(msg);
}

static void new_disk(void) {
    if (g_kind != CAT_DSK) return;
    g_naming = true;
    g_renaming = false;
    g_new_name[0] = '\0';
    naming_show();
}

static void rename_disk(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    int i = g_ovk.sel;
    if (g_kind != CAT_DSK || i < 0 || i >= cat->n) return;
    g_rename_sel = i;
    snprintf(g_new_name, sizeof g_new_name, "%s", cat->e[i].name);
    size_t l = strlen(g_new_name);
    if (l > 4 && !strcasecmp(g_new_name + l - 4, ".dsk")) g_new_name[l - 4] = '\0';
    g_naming = true;
    g_renaming = true;
    naming_show();
}

static void new_disk_commit(void) {
    char name[NEW_NAME_MAX + 8], msg[CARD_COLS + 1], old_path[DSK_NAME_MAX + 16];
    int rc;
    if (g_renaming) {
        if (!dsk_cat_path(coco_boot_dsk_catalog(), g_rename_sel, old_path, sizeof old_path)) return;
        rc = coco_boot_rename_dsk(old_path, g_new_name, name, sizeof name);
    } else {
        rc = coco_boot_create_blank_dsk(g_new_name, name, sizeof name);
    }
    if (rc == COCO_NEWDSK_EXISTS) { snprintf(msg, sizeof msg, "%s ALREADY EXISTS", name); set_status(msg); return; }
    if (rc != 0) { set_status(g_renaming ? "CANNOT RENAME IT: SD CARD?" : "CANNOT MAKE THE DISK: SD CARD?"); return; }
    int n = coco_boot_rescan(g_kind);
    ovk_set_count(&g_ovk, n);
    int i = dsk_cat_find_name(coco_boot_dsk_catalog(), name);
    if (i >= 0) g_ovk.sel = i;
    g_dirty = true;
    snprintf(msg, sizeof msg, g_renaming ? "RENAMED TO %s" : "%s MADE: 0-3 PUTS IT IN", name);
    set_status(msg);
}

// The prompt's keys. Returns when the prompt has closed, with the report
// recorded as the list's previous one so the closing key is not re-read.
static void naming_key(uint8_t mods, const uint8_t codes[6]) {
    bool shift = (mods & 0x22) != 0;
    for (int i = 0; i < 6; i++) {
        uint8_t c = codes[i];
        if (!c || memchr(g_naming_prev, c, 6)) continue;      // held from before
        if (c == HK_ESC) {
            g_naming = false; legend();
        } else if (c == HK_ENTER || c == HK_KP_ENTER) {
            if (!g_new_name[0]) continue;
            g_naming = false;
            new_disk_commit();
        } else if (c == 0x2A) {                                 // Backspace
            size_t l = strlen(g_new_name);
            if (l) g_new_name[l - 1] = '\0';
            naming_show();
        } else {
            char ch = tek_ascii(c, shift);
            bool okch = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                        || ch == '-' || ch == '_' || (ch == ' ' && g_new_name[0]);
            size_t l = strlen(g_new_name);
            if (okch && l < NEW_NAME_MAX) { g_new_name[l] = ch; g_new_name[l + 1] = '\0'; naming_show(); }
        }
        if (!g_naming) break;
    }
    memcpy(g_naming_prev, codes, 6);
    if (!g_naming) { memcpy(g_ovk.prev, codes, 6); g_ovk.held = 0; }
}

// PIZERO-296: may the editor open this file without changing what it does
// not show? If not, say why on the status row and return false. Nothing is
// opened cut short, and a CR file waits for PIZERO-295, since today's editor
// would join its lines and save them back with LF.
static bool editable(const char *path) {
    char msg[CARD_COLS + 1];
    if (strlen(path) + 5 > 96) { set_status("PATH TOO LONG TO EDIT"); return false; }   // .tmp, g_path
    uint32_t size = 0;
    switch (coco_boot_text_check(path, TED_MAX, &size)) {
    case COCO_TEXT_OK: case COCO_TEXT_MISSING: return true;
    case COCO_TEXT_TOO_BIG:
        snprintf(msg, sizeof msg, "TOO BIG: %lu KB, LIMIT 4 KB", (unsigned long)((size + 1023) / 1024));
        set_status(msg);
        return false;
    case COCO_TEXT_CR: return true;          // PIZERO-295: the editor keeps CR now
    default:           set_status("CANNOT READ IT: SD CARD?"); return false;
    }
}

// CoCo source opens in upper case like the BASIC prompt; prose in lower.
static bool caps_for(const char *name) {
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".bas") || !strcasecmp(ext, ".asm"));
}

// PIZERO-309: a text file inside a mounted disk opens read-only, read through
// the FDC's own sector path; saving it back into the image is not done here.
struct disk_file { unsigned drive; const char *name; };
static int fill_disk_file(void *ctx, char *buf, uint32_t max, uint32_t *len) {
    const struct disk_file *f = (const struct disk_file *)ctx;
    return coco_boot_dsk_read_text(f->drive, f->name, buf, max, len);
}

static void view_disk_file(int d, const char *name, const uint8_t codes[6]) {
    struct disk_file f = { (unsigned)d, name };
    int rc = text_editor_open_view(name, fill_disk_file, &f);
    if (rc == COCO_TEXT_OK) { text_editor_hold(codes); return; }
    if (rc == COCO_TEXT_TOO_BIG) set_status("TOO BIG TO SHOW: LIMIT 4 KB");
    else if (rc == COCO_TEXT_MISSING) set_status("NOT ON THE DISK NOW");
    else set_status("CANNOT READ IT FROM THE DISK");
}

// PIZERO-312: a card file shown read-only through the PIZERO-309 view.
static int fill_card_file(void *ctx, char *buf, uint32_t max, uint32_t *len) {
    return coco_boot_read_text((const char *)ctx, buf, max, len);
}

static void view_card_file(const char *path, const char *title, const uint8_t codes[6]) {
    int rc = text_editor_open_view(title, fill_card_file, (void *)path);
    if (rc == COCO_TEXT_OK) { text_editor_hold(codes); return; }
    set_status(rc == COCO_TEXT_MISSING ? "NOT ON THE CARD YET"
             : rc == COCO_TEXT_TOO_BIG ? "TOO BIG TO SHOW: LIMIT 4 KB" : "CANNOT READ IT: SD CARD?");
}

// PIZERO-146/147: ENTER in FILES opens the editor over the list. A missing
// file starts from its template: every setting at its default for
// settings.txt, commented examples for autorun.txt. PIZERO-296: the rest of
// the list is every text file under /coco, opened as it is.
static void edit_file(const uint8_t codes[6]) {
    int i = g_ovk.sel;
    if (i < 0 || i >= g_ovk.n) return;
    if (i < OVL_NFILES) {
        if (k_files[i].view) { view_card_file(k_files[i].path, k_files[i].name, codes); return; }
        if (!editable(k_files[i].path)) return;
        static char gen[1024];               // PIZERO-164: the pad lines made it longer
        const char *tmpl = k_files[i].tmpl;
        if (!tmpl) { settings_template(gen, sizeof gen); tmpl = gen; }
        bool caps = !k_files[i].tidy;        // settings: lower case; autorun: BASIC's upper
        if (text_editor_open(k_files[i].path, k_files[i].name, tmpl, k_files[i].tidy, caps))
            text_editor_hold(codes);         // the ENTER that opened it is not typed
        return;
    }
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    if (cat->kind != CAT_TEXT) return;
    int d = dsk_cat_drive(cat, i - OVL_NFILES);
    if (d >= 0) { view_disk_file(d, cat->e[i - OVL_NFILES].name, codes); return; }   // PIZERO-309
    char path[DSK_NAME_MAX + 16];
    if (!dsk_cat_path(cat, i - OVL_NFILES, path, sizeof path)) return;
    if (!editable(path)) return;
    const char *leaf = text_cat_leaf(cat->e[i - OVL_NFILES].name);
    if (text_editor_open(path, leaf, "", false, caps_for(leaf)))
        text_editor_hold(codes);
    else
        set_status("CANNOT OPEN IT");
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
    if (!editable(path)) return;             // PIZERO-296
    if (text_editor_open(path, slash ? slash + 1 : path, GAME_TEMPLATE, true, false))
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
    if (g_naming) {                          // PIZERO-193: the new-disk name prompt
        naming_key(mods, codes);
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
    case OVK_PROTECT: toggle_protect(); break;
    case OVK_NEW:    memcpy(g_naming_prev, codes, 6); new_disk(); break;
    case OVK_RENAME: memcpy(g_naming_prev, codes, 6); rename_disk(); break;
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

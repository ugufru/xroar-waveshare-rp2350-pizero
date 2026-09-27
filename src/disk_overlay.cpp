// disk_overlay.cpp — the F12 disk drives overlay (PIZERO-81, PIZERO-114).
//
// Layout on the 32x16 card, after the Fruit Jam overlay
// (src/coco/coco_main.cpp:1092-1175) but drawn with our own card:
//
//   row 0      title bar, inverse
//   rows 1-14  one disk per row: four drive columns (the drive number where
//              that disk is mounted, '-' where not), a space, then the name
//              without .DSK. The selected row is inverse.
//   row 15     status: the key legend, or what the last key did
//
// Everything here runs on core 0 from the USB host task or loop(), with the
// machine paused, so mounting a file never races an FDC read.

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "disk_overlay.h"
#include "overlay_keys.h"
#include "dsk_catalog.h"
#include "text_card.h"
#include "coco_boot.h"
#include "coco_machine.h"

#define OVL_NDRIVE 4
#define OVL_NAME_COLS (CARD_COLS - OVL_NDRIVE - 1)   // 27

static struct ovk_state g_ovk;
static disk_overlay_present_fn g_present;
static int  g_top;              // first visible list row
static bool g_dirty;
static char g_status[CARD_COLS + 1];

static void set_status(const char *s) {
    snprintf(g_status, sizeof g_status, "%s", s);
    g_dirty = true;
}

// Which drives hold catalogue entry i, as a bit mask.
static unsigned drives_holding(const struct dsk_catalog *cat, int i) {
    unsigned mask = 0;
    for (unsigned d = 0; d < OVL_NDRIVE; d++) {
        const char *p = coco_boot_drive_path(d);
        if (p && dsk_cat_find_path(cat, p) == i) mask |= 1u << d;
    }
    return mask;
}

static void draw(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    coco_boot_card_clear();
    coco_boot_card_center(0, OVL_TITLE);
    coco_boot_card_invert_row(0);

    if (cat->n == 0) {
        coco_boot_card_center(3, OVL_EMPTY_1);
        coco_boot_card_wrap(2, 5, 28, 4, OVL_EMPTY_2);
    } else {
        g_top = ovk_top(g_ovk.sel, g_top, cat->n);
        for (int r = 0; r < OVK_ROWS && g_top + r < cat->n; r++) {
            int i = g_top + r;
            char line[CARD_COLS + 1];
            unsigned mask = drives_holding(cat, i);
            for (int d = 0; d < OVL_NDRIVE; d++)
                line[d] = (mask & (1u << d)) ? (char)('0' + d) : '-';
            line[OVL_NDRIVE] = ' ';
            dsk_cat_display_name(cat->e[i].name, line + OVL_NDRIVE + 1, OVL_NAME_COLS);
            coco_boot_card_text(0, 1 + r, line);
            if (i == g_ovk.sel) coco_boot_card_invert_row(1 + r);
        }
    }
    coco_boot_card_text(0, CARD_ROWS - 1, g_status);
    if (g_present) g_present();
    g_dirty = false;
}

// On open only: a skipped-file notice, else the legend. Moving restores the
// legend, so the notice cannot hide the controls for good.
static void legend_or_skipped(void) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    if (cat->skipped_long) {
        char msg[CARD_COLS + 1];
        snprintf(msg, sizeof msg, OVL_SKIPPED, cat->skipped_long);
        set_status(msg);
    } else {
        set_status(OVL_LEGEND);
    }
}

// Put the cursor on the disk in the drive the program last used, so F12
// straight after a disk error lands on the disk that caused it.
static void open_now(void) {
    coco_machine_release_all_keys();      // nothing stays held in BASIC
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    int n = coco_boot_rescan_dsk();       // a swapped card is seen at once
    ovk_set_count(&g_ovk, n);
    const char *p = coco_boot_drive_path(coco_machine_fdc_drive());
    int i = p ? dsk_cat_find_path(cat, p) : -1;
    if (i >= 0) g_ovk.sel = i;
    legend_or_skipped();
    Serial.printf("[overlay] open: %d image(s), drive %u selected\r\n",
                  n, coco_machine_fdc_drive());
}

static void toggle_drive(int d) {
    const struct dsk_catalog *cat = coco_boot_dsk_catalog();
    int i = g_ovk.sel;
    if (i < 0 || i >= cat->n) return;
    char name[OVL_NAME_COLS + 1];
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

void disk_overlay_init(disk_overlay_present_fn present) {
    ovk_init(&g_ovk);
    g_present = present;
    g_top = 0;
}

bool disk_overlay_is_open(void) {
    return g_ovk.open;
}

void disk_overlay_open(void) {
    if (g_ovk.open) return;
    g_ovk.open = true;
    g_ovk.held = 0;
    open_now();
}

bool disk_overlay_key(const uint8_t codes[6], uint32_t frame, bool *closed) {
    struct ovk_result r = ovk_report(&g_ovk, codes, frame);
    if (closed) *closed = (r.action == OVK_CLOSE);
    switch (r.action) {
    case OVK_OPEN:  open_now(); break;
    case OVK_CLOSE: Serial.print("[overlay] close\r\n"); break;
    case OVK_MOVED: set_status(OVL_LEGEND); break;
    case OVK_DRIVE: toggle_drive(r.drive); break;
    default: break;
    }
    return r.swallow;
}

void disk_overlay_frame(uint32_t frame) {
    if (!g_ovk.open) return;
    if (ovk_tick(&g_ovk, frame) == OVK_MOVED) set_status(OVL_LEGEND);
    if (g_dirty) draw();
}

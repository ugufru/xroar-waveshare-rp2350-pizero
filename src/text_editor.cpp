// text_editor.cpp: the on-screen text editor (PIZERO-146).
//
// Screen, on the 32x16 CoCo text card:
//   row 0      the file name, inverse, with " *" while there are unsaved changes
//   rows 1-14  the text, scrolling to follow the cursor (an inverse cell)
//   row 15     the key hint, or what just happened
//
// The 6847 has no lower case on screen: text is stored as typed and shown in
// upper case, and the settings parser does not care about case. Characters
// the font cannot draw ({ } | ~ `) show as spaces but are kept.

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_editor.h"
#include "text_edit_keys.h"
#include "text_card.h"
#include "coco_boot.h"

static struct text_edit *g_t;            // on the heap only while editing
static struct tek_state g_k;
static text_editor_present_fn g_present;
static text_editor_saved_fn g_saved;
static char g_path[64];
static char g_title[CARD_COLS + 1];
static char g_status[CARD_COLS + 1];
static bool g_dirty_draw;

static void status(const char *s) {
    snprintf(g_status, sizeof g_status, "%s", s);
    g_dirty_draw = true;
}

static void draw(void) {
    coco_boot_card_clear();
    char title[CARD_COLS + 1];
    snprintf(title, sizeof title, "%s%s", g_title, g_t->dirty ? " *" : "");
    coco_boot_card_center(0, title);
    coco_boot_card_invert_row(0);
    char row[CARD_COLS + 1];
    for (int r = 0; r < TEK_ROWS; r++)
        if (ted_row(g_t, g_t->top + r, g_t->left, TEK_COLS, row))
            coco_boot_card_text(0, 1 + r, row);
    int line = ted_line_of(g_t, g_t->cur), col = ted_col_of(g_t, g_t->cur);
    coco_boot_card_invert_cell(col - g_t->left, 1 + line - g_t->top);
    coco_boot_card_text(0, CARD_ROWS - 1, g_status);
    if (g_present) g_present();
    g_dirty_draw = false;
}

static void close_editor(void) {
    free(g_t);
    g_t = nullptr;
}

void text_editor_init(text_editor_present_fn present, text_editor_saved_fn saved) {
    g_present = present;
    g_saved = saved;
}

bool text_editor_is_open(void) { return g_t != nullptr; }

void text_editor_hold(const uint8_t codes[6]) { memcpy(g_k.prev, codes, 6); }

bool text_editor_open(const char *path, const char *title, const char *template_text) {
    g_t = (struct text_edit *)malloc(sizeof *g_t);
    if (!g_t) {
        coco_boot_card_clear();
        coco_boot_card_center(7, TEK_MSG_NOMEM);
        if (g_present) g_present();
        return false;
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    snprintf(g_title, sizeof g_title, "%s", title);
    uint32_t n = 0;
    if (coco_boot_load_text(path, g_t->buf, TED_MAX, &n)) {
        ted_load(g_t, g_t->buf, (int)n);       // ted_load compacts '\r' in place
        status(TEK_HINT);
    } else {
        ted_load(g_t, template_text ? template_text : "",
                 template_text ? (int)strlen(template_text) : 0);
        status(TEK_MSG_NEW);
    }
    tek_init(&g_k);
    Serial.printf("[editor] open %s (%d bytes)\r\n", g_path, g_t->len);
    draw();
    return true;
}

void text_editor_key(uint8_t mods, const uint8_t codes[6], uint32_t frame) {
    if (!g_t) return;
    uint8_t a = tek_report(&g_k, g_t, mods, codes, frame);
    switch (a) {
    case TEK_CHANGED:
        status(g_t->full ? TEK_MSG_FULL : TEK_HINT);
        break;
    case TEK_WARN_UNSAVED:
        status(TEK_MSG_WARN);
        break;
    case TEK_SAVE:
        if (coco_boot_save_text(g_path, g_t->buf, (uint32_t)g_t->len)) {
            g_t->dirty = false;
            status(TEK_MSG_SAVED);
            if (g_saved) g_saved(g_path);
        } else {
            status(TEK_MSG_FAILED);
        }
        break;
    case TEK_CANCEL:
        Serial.printf("[editor] close %s\r\n", g_path);
        close_editor();
        return;
    default:
        break;
    }
}

void text_editor_frame(uint32_t frame) {
    if (!g_t) return;
    if (tek_tick(&g_k, g_t, frame) == TEK_CHANGED)
        status(g_t->full ? TEK_MSG_FULL : TEK_HINT);
    if (g_dirty_draw) draw();
}

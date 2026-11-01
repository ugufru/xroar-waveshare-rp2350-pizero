// text_editor.cpp: the on-screen text editor (PIZERO-146).
//
// Screen, on the 32x16 CoCo text card:
//   row 0      the file name, inverse, with " *" while there are unsaved changes
//   rows 1-14  the text, scrolling to follow the cursor (an inverse cell)
//   row 15     the key hint, or what just happened
//
// Text is shown as typed, lower case included, using the 6847T1 font's extra
// glyphs (PIZERO-162): { | } ~ and _ draw as themselves. Only the backtick
// has no glyph; it shows as an apostrophe but is kept as typed.

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_editor.h"
#include "text_edit_keys.h"
#include "text_card.h"
#include "coco_boot.h"
#include "settings.h"
#include "text_lines.h"       // PIZERO-309: tl_to_lf for the read-only view

static struct text_edit *g_t;            // on the heap only while editing
static struct tek_state g_k;
static text_editor_present_fn g_present;
static text_editor_saved_fn g_saved;
static char g_path[96];               // a game's NAME.TXT can be ~80 (PIZERO-154)
static char g_title[CARD_COLS + 1];
static char g_status[CARD_COLS + 1];
static bool g_dirty_draw;
static const char *g_hint = TEK_HINT;  // PIZERO-309: TEK_HINT_VIEW when read-only
static int g_eol = TL_CR;              // PIZERO-295: the line ends a save writes

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
    int line = ted_line_of(g_t, g_t->cur), col = ted_col_of(g_t, g_t->cur);
    if (g_t->wrap) {                        // F5: long lines continue on the next row
        int r = 0;
        for (int l = g_t->top; r < TEK_ROWS; l++) {
            if (ted_offset_of_line(g_t, l) < 0) break;
            int len = ted_line_len(g_t, l);
            for (int k = 0; k < ted_wrap_rows(len, TEK_COLS) && r < TEK_ROWS; k++, r++) {
                ted_wrap_row(g_t, l, k, TEK_COLS, row);
                coco_boot_card_text(0, 1 + r, row);
                if (l == line) {
                    int cr, cc;
                    ted_wrap_at(col, len, TEK_COLS, &cr, &cc);
                    if (cr == k) coco_boot_card_invert_cell(cc, 1 + r);
                }
            }
        }
    } else {
        for (int r = 0; r < TEK_ROWS; r++)
            if (ted_row(g_t, g_t->top + r, g_t->left, TEK_COLS, row))
                coco_boot_card_text(0, 1 + r, row);
        coco_boot_card_invert_cell(col - g_t->left, 1 + line - g_t->top);
    }
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

bool text_editor_open(const char *path, const char *title, const char *template_text, bool tidy,
                      bool caps) {
    g_t = (struct text_edit *)malloc(sizeof *g_t);
    if (!g_t) {
        coco_boot_card_clear();
        coco_boot_card_center(7, TEK_MSG_NOMEM);
        if (g_present) g_present();
        return false;
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    snprintf(g_title, sizeof g_title, "%s", title);
    g_hint = TEK_HINT;
    uint32_t n = 0;
    int rd = coco_boot_read_text(path, g_t->buf, TED_MAX, &n);
    if (rd == COCO_TEXT_TOO_BIG || rd == COCO_TEXT_ERROR) {   // PIZERO-296: never cut short
        Serial.printf("[editor] %s not opened (%s)\r\n", path, rd == COCO_TEXT_TOO_BIG ? "too big" : "read error");
        close_editor();
        return false;
    }
    if (rd == COCO_TEXT_OK) {
        // PIZERO-295: CR, LF and CRLF all read as lines, and a save writes
        // back the file's own. A file with no line end yet has no precedent,
        // so it gets the CoCo's, as a new one does.
        g_eol = tl_convention(g_t->buf, (int)n);
        if (g_eol == TL_NONE) g_eol = TL_CR;
        ted_load(g_t, g_t->buf, tl_to_lf(g_t->buf, (int)n));
        status(g_hint);
    } else {
        ted_load(g_t, template_text ? template_text : "",
                 template_text ? (int)strlen(template_text) : 0);
        g_eol = TL_CR;                         // PIZERO-295: new files in the CoCo's CR
        status(TEK_MSG_NEW);
    }
    // PIZERO-181: a settings file always opens the same way, comments first
    // and the settings by name. Not marked changed: nothing is written
    // unless it is saved.
    char *tmp = tidy ? (char *)malloc(TED_MAX) : nullptr;
    if (tmp) {
        int n2 = settings_tidy(g_t->buf, g_t->len, tmp, TED_MAX);
        ted_load(g_t, tmp, n2);
        free(tmp);
    }
    tek_init(&g_k);
    g_k.caps = caps;                       // settings, prose: lower case; autorun, source: BASIC's upper
    Serial.printf("[editor] open %s (%d bytes, %s)\r\n", g_path, g_t->len,
                  g_eol == TL_CR ? "CR" : g_eol == TL_CRLF ? "CRLF" : "LF");
    draw();
    return true;
}

int text_editor_open_view(const char *title, text_editor_fill_fn fill, void *ctx) {
    g_t = (struct text_edit *)malloc(sizeof *g_t);
    if (!g_t) return COCO_TEXT_ERROR;
    uint32_t n = 0;
    int rc = fill(ctx, g_t->buf, TED_MAX, &n);
    if (rc != COCO_TEXT_OK) { close_editor(); return rc; }
    int m = tl_to_lf(g_t->buf, (int)n);       // CR files read as lines; nothing is saved
    ted_load(g_t, g_t->buf, m);
    g_path[0] = '\0';                         // no card file: a save can never happen
    snprintf(g_title, sizeof g_title, "%s", title);
    tek_init(&g_k);
    g_k.view = true;
    g_hint = TEK_HINT_VIEW;
    status(g_hint);
    Serial.printf("[editor] view %s (%d bytes)\r\n", title, g_t->len);
    draw();
    return COCO_TEXT_OK;
}

void text_editor_key(uint8_t mods, const uint8_t codes[6], uint32_t frame) {
    if (!g_t) return;
    uint8_t a = tek_report(&g_k, g_t, mods, codes, frame);
    switch (a) {
    case TEK_CHANGED:
        status(g_t->full ? TEK_MSG_FULL : g_hint);
        break;
    case TEK_WARN_UNSAVED:
        status(TEK_MSG_WARN);
        break;
    case TEK_SAVE:
        if (g_k.view || !g_path[0]) break;    // PIZERO-309: read-only never saves
        if (coco_boot_save_text_eol(g_path, g_t->buf, (uint32_t)g_t->len, tl_eol(g_eol))) {
            g_t->dirty = false;
            const char *msg = g_saved ? g_saved(g_path) : nullptr;
            status(msg ? msg : TEK_MSG_SAVED);
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
        status(g_t->full ? TEK_MSG_FULL : g_hint);
    if (g_dirty_draw) draw();
}

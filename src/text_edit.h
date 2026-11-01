// text_edit.h: the text buffer behind the on-screen editor (PIZERO-146).
//
// Pure logic, host-tested: a flat buffer of '\n'-separated lines, a cursor
// (a byte offset), and a view (first line and first column shown) that
// follows the cursor. The screen, the keyboard and the SD card are the
// caller's. Files here are small (settings.txt, autorun.txt), so line and
// column are found by scanning rather than kept in an index.

#ifndef TEXT_EDIT_H
#define TEXT_EDIT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TED_MAX 4096                 // bytes of text, the whole file

struct text_edit {
    char buf[TED_MAX];
    int  len;
    int  cur;                        // cursor, 0..len
    int  want_col;                   // column Up/Down aim for; -1 = cursor's
    int  top;                        // first line shown
    int  left;                       // first column shown
    bool dirty;                      // changed since load
    bool full;                       // the last insert was refused: no room
    bool wrap;                       // long lines continue on the next row (F5)
};

// Load text, dropping '\r' so a file saved on Windows edits cleanly.
static inline void ted_load(struct text_edit *t, const char *data, int n) {
    t->len = 0;
    for (int i = 0; i < n && t->len < TED_MAX; i++)
        if (data[i] != '\r') t->buf[t->len++] = data[i];
    t->cur = 0; t->want_col = -1; t->top = 0; t->left = 0;
    t->dirty = false; t->full = false; t->wrap = false;
}

static inline int ted_line_start(const struct text_edit *t, int pos) {
    while (pos > 0 && t->buf[pos - 1] != '\n') pos--;
    return pos;
}

static inline int ted_line_end(const struct text_edit *t, int pos) {
    while (pos < t->len && t->buf[pos] != '\n') pos++;
    return pos;
}

static inline int ted_line_of(const struct text_edit *t, int pos) {
    int n = 0;
    for (int i = 0; i < pos; i++) if (t->buf[i] == '\n') n++;
    return n;
}

static inline int ted_col_of(const struct text_edit *t, int pos) {
    return pos - ted_line_start(t, pos);
}

static inline int ted_lines(const struct text_edit *t) {
    return ted_line_of(t, t->len) + 1;
}

// Offset of the start of line `line`, or -1 past the end.
static inline int ted_offset_of_line(const struct text_edit *t, int line) {
    if (line < 0) return -1;
    int pos = 0;
    for (int l = 0; l < line; l++) {
        pos = ted_line_end(t, pos);
        if (pos >= t->len) return -1;
        pos++;
    }
    return pos;
}

static inline bool ted_insert(struct text_edit *t, char c) {
    if (t->len >= TED_MAX) { t->full = true; return false; }
    memmove(&t->buf[t->cur + 1], &t->buf[t->cur], (size_t)(t->len - t->cur));
    t->buf[t->cur++] = c;
    t->len++;
    t->dirty = true; t->full = false; t->want_col = -1;
    return true;
}

static inline void ted_backspace(struct text_edit *t) {
    if (t->cur == 0) return;
    memmove(&t->buf[t->cur - 1], &t->buf[t->cur], (size_t)(t->len - t->cur));
    t->cur--; t->len--;
    t->dirty = true; t->want_col = -1;
}

static inline void ted_delete(struct text_edit *t) {
    if (t->cur >= t->len) return;
    memmove(&t->buf[t->cur], &t->buf[t->cur + 1], (size_t)(t->len - t->cur - 1));
    t->len--;
    t->dirty = true; t->want_col = -1;
}

static inline void ted_left(struct text_edit *t)  { if (t->cur > 0) t->cur--; t->want_col = -1; }
static inline void ted_right(struct text_edit *t) { if (t->cur < t->len) t->cur++; t->want_col = -1; }
static inline void ted_home(struct text_edit *t)  { t->cur = ted_line_start(t, t->cur); t->want_col = -1; }
static inline void ted_end(struct text_edit *t)   { t->cur = ted_line_end(t, t->cur); t->want_col = -1; }

// Move the cursor `delta` lines, keeping the column it was aiming for, so
// passing through a short line does not lose the place on long ones.
static inline void ted_vertical(struct text_edit *t, int delta) {
    if (t->want_col < 0) t->want_col = ted_col_of(t, t->cur);
    int line = ted_line_of(t, t->cur) + delta;
    int last = ted_lines(t) - 1;
    if (line < 0) line = 0;
    if (line > last) line = last;
    int start = ted_offset_of_line(t, line);
    int end = ted_line_end(t, start);
    int pos = start + t->want_col;
    t->cur = pos > end ? end : pos;
}

// ---- Wrapping (F5): a line longer than the screen continues on the next
// row. top is still a line; left is always 0. ----

// Screen rows a line of len characters takes when wrapped (at least one; a
// line exactly cols wide takes one, not two).
static inline int ted_wrap_rows(int len, int cols) {
    return len <= 0 ? 1 : (len + cols - 1) / cols;
}

// The row (within its line) and column where the cursor shows, for a cursor
// at column col of a line of len characters. A cursor just past a line that
// fills its last row exactly sits on that row's last cell.
static inline void ted_wrap_at(int col, int len, int cols, int *row, int *c) {
    if (col > 0 && col == len && col % cols == 0) { *row = col / cols - 1; *c = cols - 1; }
    else { *row = col / cols; *c = col % cols; }
}

static inline int ted_line_len(const struct text_edit *t, int line) {
    int s = ted_offset_of_line(t, line);
    return s < 0 ? 0 : ted_line_end(t, s) - s;
}

// Keep the cursor inside a rows x cols window, moving it as little as
// possible. The cursor may sit one past the last character of a line.
static inline void ted_follow(struct text_edit *t, int rows, int cols) {
    int line = ted_line_of(t, t->cur), col = ted_col_of(t, t->cur);
    if (t->wrap) {
        t->left = 0;
        if (line < t->top) t->top = line;
        int r, c;
        ted_wrap_at(col, ted_line_len(t, line), cols, &r, &c);
        int used = r + 1;                               // rows from top to the cursor's
        for (int l = t->top; l < line; l++) used += ted_wrap_rows(ted_line_len(t, l), cols);
        while (used > rows && t->top < line) {
            used -= ted_wrap_rows(ted_line_len(t, t->top), cols);
            t->top++;
        }
        return;
    }
    if (line < t->top) t->top = line;
    if (line >= t->top + rows) t->top = line - rows + 1;
    if (col < t->left) t->left = col;
    if (col >= t->left + cols) t->left = col - cols + 1;
}

// The visible text of `line` from column `left`, `cols` wide, into out
// (NUL-terminated). Returns false when the line does not exist.
static inline bool ted_row(const struct text_edit *t, int line, int left, int cols, char *out) {
    int start = ted_offset_of_line(t, line);
    if (start < 0) { out[0] = '\0'; return false; }
    int end = ted_line_end(t, start);
    int n = 0;
    for (int i = start + left; i < end && n < cols; i++) out[n++] = t->buf[i];
    out[n] = '\0';
    return true;
}

// Wrapped: the text for screen row k of `line` (0 = its first), cols wide.
// Returns false when that row does not exist.
static inline bool ted_wrap_row(const struct text_edit *t, int line, int k, int cols, char *out) {
    int start = ted_offset_of_line(t, line);
    out[0] = '\0';
    if (start < 0) return false;
    int len = ted_line_end(t, start) - start;
    if (k >= ted_wrap_rows(len, cols)) return false;
    int n = 0;
    for (int i = start + k * cols; i < start + len && n < cols; i++) out[n++] = t->buf[i];
    out[n] = '\0';
    return true;
}

#endif  // TEXT_EDIT_H

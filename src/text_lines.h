// text_lines.h: lines of a text file, whatever ends them (PIZERO-294).
//
// A text file on the card may end its lines with LF (the Mac, and what the
// board has always written), CRLF (Windows) or CR alone (the CoCo, whose
// Disk BASIC ends every line with $0D). Every parser on the board splits
// lines here, so a file reads the same whichever it uses, and every writer
// that edits a file in place keeps the convention it found.
//
// Pure logic, no FatFs, host-tested (test/test_text_lines). The streaming
// reader takes a read callback so it can sit on a FatFs FIL on the board
// and on a string in the tests.

#ifndef TEXT_LINES_H
#define TEXT_LINES_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum { TL_NONE = 0, TL_LF, TL_CR, TL_CRLF };

// The line starting at pos: *start and *len cover its text, without the line
// end. Returns where the next line starts (n at the end of the buffer). CR,
// LF and CRLF each end one line; CRLF is never two.
static inline int tl_next(const char *b, int n, int pos, int *start, int *len) {
    int e = pos;
    while (e < n && b[e] != '\n' && b[e] != '\r') e++;
    *start = pos;
    *len = e - pos;
    if (e >= n) return n;
    if (b[e] == '\r' && e + 1 < n && b[e + 1] == '\n') return e + 2;
    return e + 1;
}

// The convention the first line end in the buffer uses, or TL_NONE if it has
// none (a one-line file, or an empty one).
static inline int tl_convention(const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') return TL_LF;
        if (b[i] == '\r') return (i + 1 < n && b[i + 1] == '\n') ? TL_CRLF : TL_CR;
    }
    return TL_NONE;
}

// The bytes that end a line in this convention. TL_NONE writes LF, which is
// what the board has always written.
static inline const char *tl_eol(int conv) {
    return conv == TL_CR ? "\r" : conv == TL_CRLF ? "\r\n" : "\n";
}

static inline bool tl_is_eol(char c) { return c == '\n' || c == '\r'; }

// Rewrite every line end as LF, in place, and return the new length: CRLF
// becomes one LF, a lone CR one LF. For showing a file whose lines end in CR
// (a Disk BASIC ASCII file) in the editor read-only, where nothing is saved
// back, so the original line ends are never lost (PIZERO-309).
static inline int tl_to_lf(char *b, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (b[i] == '\r') {
            b[k++] = '\n';
            if (i + 1 < n && b[i + 1] == '\n') i++;
        } else {
            b[k++] = b[i];
        }
    }
    return k;
}

// PIZERO-295: the editor keeps text with LF line ends and saves it with the
// file's own. Copy from b (n bytes, from *pos) into out (cap bytes), writing
// each LF as eol (NULL: unchanged), and advance *pos. Returns the bytes put
// in out; a line end is never split across two calls, so cap must be at
// least 2. Call until *pos reaches n.
static inline int tl_out_chunk(const char *b, int n, int *pos, const char *eol, char *out, int cap) {
    int k = 0, el = eol ? (int)strlen(eol) : 1;
    while (*pos < n) {
        char c = b[*pos];
        if (c == '\n' && eol) {
            if (k + el > cap) break;
            for (int i = 0; i < el; i++) out[k++] = eol[i];
        } else {
            if (k + 1 > cap) break;
            out[k++] = c;
        }
        (*pos)++;
    }
    return k;
}

// ---- Streaming, for files read a line at a time (in place of f_gets) ----

// Fill buf with up to n bytes; return how many (0 at the end of the file).
typedef int (*tl_read_fn)(void *ctx, char *buf, int n);

struct tl_reader {
    tl_read_fn read;
    void      *ctx;
    char       buf[64];
    int        len, pos;
    bool       eof;
    bool       after_cr;      // the last line ended in CR: an LF next is its pair
};

static inline void tl_reader_init(struct tl_reader *r, tl_read_fn read, void *ctx) {
    r->read = read; r->ctx = ctx;
    r->len = r->pos = 0;
    r->eof = false; r->after_cr = false;
}

// The next byte, or -1 at the end.
static inline int tl_getc(struct tl_reader *r) {
    if (r->pos >= r->len) {
        if (r->eof) return -1;
        r->len = r->read(r->ctx, r->buf, (int)sizeof r->buf);
        r->pos = 0;
        if (r->len <= 0) { r->len = 0; r->eof = true; return -1; }
    }
    return (unsigned char)r->buf[r->pos++];
}

// Read one line into line (cap bytes, always NUL-terminated), without its
// line end. A line longer than cap - 1 is cut there and the rest of it
// skipped, so it can never come back as a second, garbled line (f_gets split
// a long line into pieces). Returns false at the end of the file when no
// line is left; a last line with no line end still counts.
static inline bool tl_gets(struct tl_reader *r, char *line, int cap) {
    int k = 0, c;
    bool any = false;
    while ((c = tl_getc(r)) >= 0) {
        if (r->after_cr) {
            r->after_cr = false;
            if (c == '\n') continue;              // the LF of a CRLF
        }
        if (c == '\r') { r->after_cr = true; any = true; break; }
        if (c == '\n') { any = true; break; }
        any = true;
        if (k < cap - 1) line[k++] = (char)c;
    }
    if (cap > 0) line[k < cap ? k : cap - 1] = '\0';
    return any;
}

#endif  // TEXT_LINES_H

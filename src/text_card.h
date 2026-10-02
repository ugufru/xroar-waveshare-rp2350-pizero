// text_card.h: the pure parts of the 32x16 diagnostic card (PIZERO-92),
// split out so they can be tested on the host (PIZERO-109). The rendering
// itself needs a framebuffer and the font, and lives in coco_boot.cpp.

#ifndef TEXT_CARD_H
#define TEXT_CARD_H

#include <stdbool.h>
#include <stdint.h>

#define CARD_COLS 32
#define CARD_ROWS 16

// ASCII in, glyph index into font_6847t2 out (PIZERO-162, 166). The
// firmware's own screens draw with the project's 6847T2 font, which has a
// glyph for every printable ASCII character:
//   $00        backtick `      $01-$1A  lower case a-z
//   $1B-$1E    { | } ~         $1F      DEL (not used here)
//   $40-$5D    @ A-Z [ \ ]     $5E ^    $5F _
//   $60-$7F    space to ?
// Anything unprintable becomes a space rather than a random glyph on a
// failure screen. CoCo programs are unaffected: this is only the card.
#define CARD_SPACE 0x60

static inline uint8_t card_code(char c) {
    uint8_t u = (uint8_t)c;
    if (u >= 0x20 && u <= 0x3F) return (uint8_t)(u + 0x40);
    if (u >= 0x40 && u <= 0x5F) return u;          // @ A-Z [ \ ] ^ _
    if (u == '`') return 0x00;
    if (u >= 'a' && u <= 'z') return (uint8_t)(u - 'a' + 0x01);
    if (u >= '{' && u <= '~') return (uint8_t)(u - '{' + 0x1B);
    return CARD_SPACE;
}

// True when `c` has a glyph of its own on the card (not the fallback space).
static inline bool card_printable(char c) {
    uint8_t u = (uint8_t)c;
    return u == ' ' || (u > 0x20 && u <= 0x7E);
}

// Left column for a centred string, clamped so an over-long line starts at 0
// and is clipped by the caller rather than wrapping into the row above.
static inline int card_center_col(int len) {
    if (len > CARD_COLS) len = CARD_COLS;
    int col = (CARD_COLS - len) / 2;
    return col < 0 ? 0 : col;
}

// Word wrap for a diagnostic page. Given the rest of the message, returns how
// many characters belong on this line (breaking at a space where possible, or
// hard-breaking a word longer than the line) and, through `next`, where the
// following line starts with leading spaces already skipped.
//
// The page is 32 columns on a TV, possibly an old one with overscan eating the
// edges, so a line that runs off the right is simply lost. Wrapping is not a
// nicety here.
static inline int card_wrap_next(const char *s, int width, const char **next) {
    if (!s || !*s || width <= 0) { if (next) *next = s; return 0; }
    int i = 0, last_space = -1;
    while (s[i] && i < width) {
        if (s[i] == ' ') last_space = i;
        i++;
    }
    if (!s[i]) {                       // the rest fits
        if (next) *next = s + i;
        return i;
    }
    int len = (last_space > 0) ? last_space : i;   // break at a space if we can
    const char *n = s + len;
    while (*n == ' ') n++;             // do not start the next line with a space
    if (next) *next = n;
    return len;
}

#endif  // TEXT_CARD_H

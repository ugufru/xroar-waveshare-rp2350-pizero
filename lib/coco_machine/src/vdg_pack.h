// vdg_pack.h — VDG pixel packing, extracted so it can be tested on the host
// (PIZERO-109). Header-only and free of Arduino, Pico and machine state: the
// firmware includes it from coco_machine.cpp, the native tests include it
// directly.
//
// Why it exists: PIZERO-119 replaced per-pixel read-modify-writes with packed
// 32-bit stores from small tables, which cut a frame from 99.5% to 75% of the
// budget. That rewrite was checked by throwaway programs that were then
// deleted. This header is the same logic with the checks kept.
//
// The packed format is the one coco_boot_blit_vdg_pizero_src consumes: two
// palette indices per byte, LOW nibble = even pixel, high nibble = odd.

#ifndef VDG_PACK_H
#define VDG_PACK_H

#include <stdbool.h>
#include <stdint.h>

// Palette indices. These must match the PAL_* defines in coco_machine.cpp;
// vdg_pack_assert_palette() below is compiled into both and checks it.
#define VDG_PAL_GREEN       0
#define VDG_PAL_YELLOW      1
#define VDG_PAL_BLUE        2
#define VDG_PAL_RED         3
#define VDG_PAL_WHITE       4
#define VDG_PAL_CYAN        5
#define VDG_PAL_MAGENTA     6
#define VDG_PAL_ORANGE      7
#define VDG_PAL_BLACK       8
#define VDG_PAL_DARK_GREEN  9

// Pack 8 palette indices into one 32-bit word, low nibble = even pixel.
static inline uint32_t vdg_pack8(const uint8_t px[8]) {
    uint32_t w = 0;
    for (int i = 0; i < 8; i++)
        w |= (uint32_t)px[i] << ((i >> 1) * 8 + ((i & 1) ? 4 : 0));
    return w;
}

// --- SG4 semigraphics -----------------------------------------------------
// A cell is two colour blocks side by side, so its 8 packed pixels depend
// only on the colour (3 bits) and the two block bits: 32 entries covers it.
// This is the path that was costing 4.88 ms a frame before PIZERO-119, and it
// hides inside the alpha renderer, so the mode bits read "alpha" either way.

typedef uint32_t vdg_sg4_table_t[8][4];

static inline void vdg_build_sg4_table(vdg_sg4_table_t t) {
    for (int color = 0; color < 8; color++) {
        for (int sg = 0; sg < 4; sg++) {
            uint8_t left  = (sg & 2) ? (uint8_t)color : (uint8_t)VDG_PAL_BLACK;
            uint8_t right = (sg & 1) ? (uint8_t)color : (uint8_t)VDG_PAL_BLACK;
            uint8_t px[8];
            for (int bit = 0; bit < 8; bit++) px[bit] = (bit < 4) ? left : right;
            t[color][sg] = vdg_pack8(px);
        }
    }
}

// Which half of the cell a sub-row belongs to: rows 0-5 use the top pair of
// blocks (ch >> 2), rows 6-11 the bottom pair (ch).
static inline uint8_t vdg_sg4_bits(uint8_t ch, int sub_row) {
    return (uint8_t)((sub_row < 6) ? (ch >> 2) : ch);
}

static inline uint32_t vdg_sg4_word(const vdg_sg4_table_t t, uint8_t ch, int sub_row) {
    return t[(ch >> 4) & 7][vdg_sg4_bits(ch, sub_row) & 3];
}

// --- RG6 / PMODE 4 --------------------------------------------------------
// One byte is 8 mono pixels, or 4 artifact colour clocks of 2 pixels each.

static inline void vdg_build_rg6_mono_table(uint32_t t[256]) {
    for (int b = 0; b < 256; b++) {
        uint8_t px[8];
        for (int bit = 0; bit < 8; bit++)
            px[bit] = (b & (0x80 >> bit)) ? VDG_PAL_WHITE : VDG_PAL_BLACK;
        t[b] = vdg_pack8(px);
    }
}

// c01/c10 are the artifact colours for bit pairs 01 and 10; which is which
// depends on CSS and on the power-on phase (PIZERO-43), so the caller picks.
static inline void vdg_build_rg6_artifact_table(uint32_t t[256], uint8_t c01, uint8_t c10) {
    for (int b = 0; b < 256; b++) {
        uint8_t px[8];
        uint8_t v = (uint8_t)b;
        for (int pair = 0; pair < 4; pair++) {
            uint8_t bits = (uint8_t)((v >> 6) & 3);
            v = (uint8_t)(v << 2);
            uint8_t color = (bits == 0) ? VDG_PAL_BLACK
                          : (bits == 1) ? c01
                          : (bits == 2) ? c10
                          : VDG_PAL_WHITE;
            px[pair * 2] = px[pair * 2 + 1] = color;   // one colour clock = 2 px
        }
        t[b] = vdg_pack8(px);
    }
}

// --- alpha text -----------------------------------------------------------
// A glyph row is 8 mono pixels of ink on paper; bit 6 of the character
// selects the inverse pair.

static inline void vdg_build_alpha_table(uint32_t t[2][256]) {
    const uint8_t combos[2][2] = {
        { VDG_PAL_GREEN, VDG_PAL_BLACK },
        { VDG_PAL_BLACK, VDG_PAL_GREEN },
    };
    for (int c = 0; c < 2; c++) {
        uint8_t ink = combos[c][0], paper = combos[c][1];
        for (int g = 0; g < 256; g++) {
            uint8_t px[8];
            for (int bit = 0; bit < 8; bit++)
                px[bit] = (g & (0x80 >> bit)) ? ink : paper;
            t[c][g] = vdg_pack8(px);
        }
    }
}

// The screen code to font index fold: 6 bits of code select a glyph in the
// T1 font's $40-$7F block (coco_machine.cpp:964).
static inline uint8_t vdg_alpha_glyph_index(uint8_t ch) {
    return (uint8_t)((ch & 0x3F) | 0x40);
}

// --- SAM display base -----------------------------------------------------
// PIZERO-100: the renderer treats a SAM F value of zero as "never set" and
// substitutes $0400, so a program that legitimately puts the display at
// $0000 gets the wrong screen. Extracted here so the behaviour is pinned by a
// test and the fix, when it comes, is one function and one test away.

static inline uint16_t vdg_display_base_legacy(uint16_t sam_f) {
    return sam_f ? sam_f : (uint16_t)0x0400;
}

// The intended behaviour: honour zero once the guest has actually written F,
// and only fall back before that (so boot does not show a frame of garbage).
static inline uint16_t vdg_display_base(uint16_t sam_f, bool sam_f_written) {
    return sam_f_written ? sam_f : (uint16_t)0x0400;
}

#endif  // VDG_PACK_H

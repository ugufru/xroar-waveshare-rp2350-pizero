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
#include <string.h>

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

// PIZERO-166: which font set the machine's text uses (the font setting).
//   CLASSIC  the original MC6847 (font_6847, 64 glyphs): codes 0-63 are the
//            inverse characters, ^ is an up arrow and _ a left arrow.
//   T1       the MC6847T1 of the CoCo 2B (font_6847t1, 128 glyphs): the same
//            until a program sets EXT (PIA1 PB bit 4, POKE 65314,16); then
//            the full 7-bit code picks the glyph, giving true lower case and
//            { | } ~, all drawn with INV set (upstream mc6847.c:470-497).
//   T2       ours, the default: the T1, but ^ is a caret and _ an underscore
//            at all times (the T1's own glyphs $00 and $1F).
enum { VDG_FONT_CLASSIC = 0, VDG_FONT_T1 = 1, VDG_FONT_T2 = 2 };

// The glyph a character code shows, as an index into font_6847 (CLASSIC) or
// font_6847t1 (T1, T2), and in *pair which colour pair (bit 6 of the code, or
// forced on under EXT), matching vdg_build_alpha_table's rows.
static inline uint8_t vdg_alpha_glyph(uint8_t ch, bool ext, int font, uint8_t *pair) {
    if (font == VDG_FONT_CLASSIC) {
        *pair = (uint8_t)((ch >> 6) & 1);
        return (uint8_t)(ch & 0x3F);
    }
    uint8_t g;
    if (ext) { g = (uint8_t)(ch & 0x7F); *pair = 1; }
    else     { g = vdg_alpha_glyph_index(ch); *pair = (uint8_t)((ch >> 6) & 1); }
    if (font == VDG_FONT_T2) {
        if (g == 0x5E) g = 0x00;           // ^ : the T1's caret, not an up arrow
        else if (g == 0x5F) g = 0x1F;      // _ : its underscore, not a left arrow
    }
    return g;
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

// ---- GM 0-6 graphics modes (PIZERO-140) ------------------------------------
//
// The colour and resolution graphics modes other than RG6 used to be drawn a
// pixel at a time: every bit or 2-bit cell expanded through a replication
// loop into a row buffer, then packed. Popcorn (GM5) spent ~7.5 ms a frame on
// it and ran at 50 fps. Every mode here turns one data byte into a fixed run
// of packed output bytes (a data row is always 256 displayed pixels, 128
// packed bytes), so a 256-entry table indexed by the data byte does the whole
// expansion in one lookup. The table is built from exactly the per-pixel rule
// it replaces, and the host test compares whole frames against that rule.

// Display lines per data row, by GM (must match GM_nLPR in coco_machine.cpp).
static const uint8_t VDG_GM_NLPR[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };

// Row geometry comes from the SAM, not the VDG. On a CoCo the SAM generates
// the video addresses: its V mode says how many bytes lie between data rows
// and how many display lines repeat each row. The VDG only decides how a
// byte becomes pixels. They normally agree, but a program may pair them
// differently: Popcorn sets the VDG to GM5 (one line per row) with the SAM in
// V3 (two lines per row) for double-height pixels, and drawing from the VDG
// mode alone showed its game in the top half of the screen only.
//
//   SAM V      1    2    3    4    5    6        (0 = text, 7 = DMA)
//   stride    16   32   16   32   16   32   bytes between data rows
//   lines      3    3    2    2    1    1   display lines per data row
static const uint8_t VDG_SAM_STRIDE[8] = { 32, 16, 32, 16, 32, 16, 32, 0 };
static const uint8_t VDG_SAM_LINES[8]  = { 12,  3,  3,  2,  2,  1,  1, 0 };

static inline int vdg_gm_bytes_per_row(uint8_t gm);

// Row geometry for graphics mode `gm` (0-7) under SAM mode `sam_v`: the
// SAM's when it is in a graphics mode (1-6), else the VDG mode's own.
static inline void vdg_gfx_geometry(uint8_t gm, unsigned sam_v, int *stride, int *lines) {
    if (sam_v >= 1 && sam_v <= 6) {
        *stride = VDG_SAM_STRIDE[sam_v];
        *lines  = VDG_SAM_LINES[sam_v];
    } else if (gm == 7) {
        *stride = 32; *lines = 1;                   // RG6
    } else {
        *stride = vdg_gm_bytes_per_row(gm);
        *lines  = VDG_GM_NLPR[gm];
    }
}

static inline int vdg_gm_bytes_per_row(uint8_t gm) {
    return (gm == 2 || gm == 4 || gm == 6) ? 32 : 16;
}

// Packed output bytes per data byte: 128 packed bytes per row.
static inline int vdg_gm_out_per_byte(uint8_t gm) {
    return 128 / vdg_gm_bytes_per_row(gm);            // 8 or 4
}

// The palette index of each displayed pixel one data byte produces, the rule
// render_graphics_frame always used: RG = 1 bit/pixel fg or bg; CG = 2-bit
// cells, colour = base + value; each repeated to fill 256 pixels a row.
static inline int vdg_gm_expand(uint8_t gm, bool css, uint8_t b, uint8_t *px) {
    const bool rg = gm & 1;
    const int  src_px = rg ? vdg_gm_bytes_per_row(gm) * 8 : vdg_gm_bytes_per_row(gm) * 4;
    const int  hrep = 256 / src_px;
    const uint8_t cg_base = css ? VDG_PAL_WHITE : VDG_PAL_GREEN;
    const uint8_t fg = css ? VDG_PAL_WHITE : VDG_PAL_GREEN;
    const uint8_t bg = css ? VDG_PAL_BLACK : VDG_PAL_DARK_GREEN;
    int n = 0;
    if (rg) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c = (b & (0x80 >> bit)) ? fg : bg;
            for (int r = 0; r < hrep; r++) px[n++] = c;
        }
    } else {
        for (int cell = 0; cell < 4; cell++) {
            uint8_t c = (uint8_t)(cg_base + ((b >> 6) & 3));
            b = (uint8_t)(b << 2);
            for (int r = 0; r < hrep; r++) px[n++] = c;
        }
    }
    return n;                                        // 16 or 8 pixels
}

struct vdg_gm_lut {
    int     key;                                     // gm * 2 + css, -1 = none
    uint8_t out[256][8];                             // packed bytes per data byte
};

static inline void vdg_gm_lut_build(struct vdg_gm_lut *t, uint8_t gm, bool css) {
    for (int b = 0; b < 256; b++) {
        uint8_t px[16];
        int n = vdg_gm_expand(gm, css, (uint8_t)b, px);
        for (int i = 0; i < n; i += 2)
            t->out[b][i >> 1] = (uint8_t)(px[i] | (px[i + 1] << 4));
    }
    t->key = gm * 2 + (css ? 1 : 0);
}

// Render a whole GM 0-6 frame from guest RAM into the packed VDG buffer
// (192 rows of 128 bytes). The VDG mode decides how each byte becomes pixels
// (and how many bytes a line reads); `stride` and `lines` are the row
// geometry from vdg_gfx_geometry. The table is rebuilt only when the mode or
// colour set changes, which on a running game is never.
static inline void vdg_render_gm(struct vdg_gm_lut *t, const uint8_t *ram,
                                 uint16_t base, uint8_t gm, bool css,
                                 int stride, int lines, uint8_t *vdg_buffer) {
    if (t->key != gm * 2 + (css ? 1 : 0)) vdg_gm_lut_build(t, gm, css);
    const int opb  = vdg_gm_out_per_byte(gm);
    const int nlpr = lines > 0 ? lines : 1;
    const int data_rows = (192 + nlpr - 1) / nlpr;
    for (int drow = 0; drow < data_rows; drow++) {
        const uint8_t *p = &ram[(uint16_t)(base + drow * stride)];
        uint8_t *row = &vdg_buffer[drow * nlpr * 128];
        if (opb == 8) {
            for (int i = 0; i < 16; i++) memcpy(row + i * 8, t->out[p[i]], 8);
        } else {
            for (int i = 0; i < 32; i++) memcpy(row + i * 4, t->out[p[i]], 4);
        }
        for (int rep = 1; rep < nlpr && drow * nlpr + rep < 192; rep++)
            memcpy(row + rep * 128, row, 128);
    }
}

#endif  // VDG_PACK_H

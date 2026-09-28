// PIZERO-109: host tests for the VDG pixel packing the firmware actually uses.
//
// These exist because PIZERO-119 rewrote three render paths from per-pixel
// read-modify-writes into packed 32-bit stores from small tables, and the only
// check at the time was a pair of throwaway programs. The reference
// implementations below ARE those per-pixel versions, kept deliberately: they
// are slow, obvious, and easy to read against the VDG documentation, so the
// fast tables are always checked against something a human can verify.
//
//   pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "../../lib/coco_machine/src/vdg_pack.h"
// PIZERO-166: the real glyph shapes. The font asks for an RP2350 RAM section,
// which the host compiler rejects, so the attribute is dropped here only.
#define __attribute__(x)
#include "../../lib/xroar_core/src/mc6847/font-6847t1.c"
#undef __attribute__

// --- reference implementation --------------------------------------------
// The original put2(): nibble-packed, low nibble = even pixel. Every fast
// path below must agree with this, byte for byte.

static void ref_put2(uint8_t *dst, int px, uint8_t color) {
    int i = px >> 1;
    if (px & 1) dst[i] = (uint8_t)((dst[i] & 0x0F) | (color << 4));
    else        dst[i] = (uint8_t)((dst[i] & 0xF0) | color);
}

static uint32_t ref_word(const uint8_t px[8]) {
    uint8_t b[4] = {0, 0, 0, 0};
    for (int i = 0; i < 8; i++) ref_put2(b, i, px[i]);
    uint32_t w;
    memcpy(&w, b, 4);
    return w;
}

// --- packing --------------------------------------------------------------

static void test_pack8_matches_put2(void) {
    const uint8_t px[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    TEST_ASSERT_EQUAL_HEX32(ref_word(px), vdg_pack8(px));
}

static void test_pack8_nibble_order_is_low_even(void) {
    // Pixel 0 must land in the LOW nibble of the first byte: the blit reads
    // it that way, so getting this backwards mirrors every glyph.
    const uint8_t px[8] = { 0xA, 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_HEX32(0x0000000A, vdg_pack8(px));
    const uint8_t px1[8] = { 0, 0xA, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_HEX32(0x000000A0, vdg_pack8(px1));
}

// --- SG4 semigraphics -----------------------------------------------------

static void test_sg4_matches_reference_every_cell(void) {
    vdg_sg4_table_t t;
    vdg_build_sg4_table(t);
    for (int ch = 0x80; ch <= 0xFF; ch++) {
        for (int sub_row = 0; sub_row < 12; sub_row++) {
            uint8_t color = (uint8_t)((ch >> 4) & 7);
            uint8_t sg = vdg_sg4_bits((uint8_t)ch, sub_row);
            uint8_t left  = (sg & 2) ? color : (uint8_t)VDG_PAL_BLACK;
            uint8_t right = (sg & 1) ? color : (uint8_t)VDG_PAL_BLACK;
            uint8_t px[8];
            for (int bit = 0; bit < 8; bit++) px[bit] = (bit < 4) ? left : right;
            TEST_ASSERT_EQUAL_HEX32(ref_word(px),
                                    vdg_sg4_word(t, (uint8_t)ch, sub_row));
        }
    }
}

static void test_sg4_top_and_bottom_halves_differ(void) {
    // Sub-rows 0-5 use the top pair of blocks, 6-11 the bottom pair. A cell
    // with only the top blocks lit must go dark in the bottom half.
    vdg_sg4_table_t t;
    vdg_build_sg4_table(t);
    const uint8_t ch = 0x8C;            // colour 0, top blocks set, bottom clear
    uint32_t top = vdg_sg4_word(t, ch, 0), bottom = vdg_sg4_word(t, ch, 6);
    TEST_ASSERT_NOT_EQUAL(top, bottom);
    const uint8_t black[8] = { VDG_PAL_BLACK, VDG_PAL_BLACK, VDG_PAL_BLACK, VDG_PAL_BLACK,
                               VDG_PAL_BLACK, VDG_PAL_BLACK, VDG_PAL_BLACK, VDG_PAL_BLACK };
    TEST_ASSERT_EQUAL_HEX32(ref_word(black), bottom);
}

// --- RG6 / PMODE 4 --------------------------------------------------------

static void test_rg6_mono_matches_reference(void) {
    uint32_t t[256];
    vdg_build_rg6_mono_table(t);
    for (int b = 0; b < 256; b++) {
        uint8_t px[8];
        for (int bit = 0; bit < 8; bit++)
            px[bit] = (b & (0x80 >> bit)) ? VDG_PAL_WHITE : VDG_PAL_BLACK;
        TEST_ASSERT_EQUAL_HEX32(ref_word(px), t[b]);
    }
}

static void test_rg6_artifact_matches_reference_both_phases(void) {
    const uint8_t phases[2][2] = {
        { VDG_PAL_ORANGE, VDG_PAL_BLUE },
        { VDG_PAL_BLUE, VDG_PAL_ORANGE },
    };
    for (int p = 0; p < 2; p++) {
        uint8_t c01 = phases[p][0], c10 = phases[p][1];
        uint32_t t[256];
        vdg_build_rg6_artifact_table(t, c01, c10);
        for (int b = 0; b < 256; b++) {
            uint8_t px[8];
            uint8_t v = (uint8_t)b;
            for (int pair = 0; pair < 4; pair++) {
                uint8_t bits = (uint8_t)((v >> 6) & 3);
                v = (uint8_t)(v << 2);
                uint8_t color = (bits == 0) ? (uint8_t)VDG_PAL_BLACK
                              : (bits == 1) ? c01
                              : (bits == 2) ? c10
                              : (uint8_t)VDG_PAL_WHITE;
                px[pair * 2] = px[pair * 2 + 1] = color;
            }
            TEST_ASSERT_EQUAL_HEX32(ref_word(px), t[b]);
        }
    }
}

static void test_rg6_artifact_colour_clock_spans_two_pixels(void) {
    // The artifact path must emit each colour clock as a PAIR of pixels, not
    // one: getting this wrong halves the picture width.
    uint32_t t[256];
    vdg_build_rg6_artifact_table(t, VDG_PAL_ORANGE, VDG_PAL_BLUE);
    uint8_t b[4];
    memcpy(b, &t[0x40], 4);          // 0b01000000: first pair = 01 -> c01
    TEST_ASSERT_EQUAL_UINT8(VDG_PAL_ORANGE, b[0] & 0x0F);
    TEST_ASSERT_EQUAL_UINT8(VDG_PAL_ORANGE, (b[0] >> 4) & 0x0F);
    TEST_ASSERT_EQUAL_UINT8(VDG_PAL_BLACK, b[1] & 0x0F);
}

static void test_rg6_artifact_phase_swap_is_visible(void) {
    uint32_t a[256], b[256];
    vdg_build_rg6_artifact_table(a, VDG_PAL_ORANGE, VDG_PAL_BLUE);
    vdg_build_rg6_artifact_table(b, VDG_PAL_BLUE, VDG_PAL_ORANGE);
    TEST_ASSERT_NOT_EQUAL(a[0x40], b[0x40]);   // PIZERO-43 phase actually swaps
    TEST_ASSERT_EQUAL_HEX32(a[0x00], b[0x00]); // ...but black and white do not
    TEST_ASSERT_EQUAL_HEX32(a[0xFF], b[0xFF]);
}

// --- alpha text -----------------------------------------------------------

static void test_alpha_matches_reference_both_colourings(void) {
    uint32_t t[2][256];
    vdg_build_alpha_table(t);
    const uint8_t combos[2][2] = {
        { VDG_PAL_GREEN, VDG_PAL_BLACK },
        { VDG_PAL_BLACK, VDG_PAL_GREEN },
    };
    for (int c = 0; c < 2; c++) {
        for (int g = 0; g < 256; g++) {
            uint8_t px[8];
            for (int bit = 0; bit < 8; bit++)
                px[bit] = (g & (0x80 >> bit)) ? combos[c][0] : combos[c][1];
            TEST_ASSERT_EQUAL_HEX32(ref_word(px), t[c][g]);
        }
    }
}

static void test_alpha_glyph_index_folds_into_the_t1_block(void) {
    TEST_ASSERT_EQUAL_HEX8(0x40, vdg_alpha_glyph_index(0x00));
    TEST_ASSERT_EQUAL_HEX8(0x7F, vdg_alpha_glyph_index(0x3F));
    TEST_ASSERT_EQUAL_HEX8(0x41, vdg_alpha_glyph_index(0x41));  // bit 6 ignored
    TEST_ASSERT_EQUAL_HEX8(0x41, vdg_alpha_glyph_index(0xC1));  // bit 7 ignored
}

// --- SAM display base (PIZERO-100) ---------------------------------------

static void test_display_base_legacy_loses_address_zero(void) {
    // This pins the BUG, so the day someone fixes it this test fails and
    // points at the fix. A guest that legitimately puts the screen at $0000
    // gets $0400 instead.
    TEST_ASSERT_EQUAL_HEX16(0x0400, vdg_display_base_legacy(0x0000));
    TEST_ASSERT_EQUAL_HEX16(0x0400, vdg_display_base_legacy(0x0400));
    TEST_ASSERT_EQUAL_HEX16(0x0600, vdg_display_base_legacy(0x0600));
}

static void test_display_base_honours_zero_once_written(void) {
    TEST_ASSERT_EQUAL_HEX16(0x0000, vdg_display_base(0x0000, true));
    TEST_ASSERT_EQUAL_HEX16(0x0400, vdg_display_base(0x0000, false));
    TEST_ASSERT_EQUAL_HEX16(0x0600, vdg_display_base(0x0600, true));
}

void setUp(void) {}
void tearDown(void) {}

// --- PIZERO-140: GM 0-6 whole frames ---------------------------------------
// render_graphics_frame as it stood before PIZERO-140, transcribed verbatim
// from coco_machine.cpp (per-bit and per-cell expansion, then pack). The table
// renderer must match it byte for byte on every mode, both colour sets.
static const uint8_t REF_GM_nLPR[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };
static void ref_render_graphics_frame(const uint8_t *ram, uint16_t base, uint8_t gm,
                                      bool css, uint8_t *vdg_buffer) {
    const bool is_32       = (gm == 2 || gm == 4 || gm == 6);
    const int  bytes_per_row = is_32 ? 32 : 16;
    const int  nlpr        = REF_GM_nLPR[gm];
    const bool rg          = gm & 1;
    const int  data_rows   = 192 / nlpr;
    const uint8_t cg_base = css ? VDG_PAL_WHITE : VDG_PAL_GREEN;
    const uint8_t fg = css ? VDG_PAL_WHITE : VDG_PAL_GREEN;
    const uint8_t bg = css ? VDG_PAL_BLACK : VDG_PAL_DARK_GREEN;
    const int src_px = rg ? bytes_per_row * 8 : bytes_per_row * 4;
    const int hrep   = 256 / src_px;
    uint8_t rowbuf[256];
    for (int drow = 0; drow < data_rows; drow++) {
        const uint8_t *p = &ram[(base + drow * bytes_per_row) & 0xFFFF];
        int px = 0;
        for (int byte = 0; byte < bytes_per_row; byte++) {
            uint8_t b = p[byte];
            if (rg) {
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t c = (b & (0x80 >> bit)) ? fg : bg;
                    for (int r = 0; r < hrep; r++) rowbuf[px++] = c;
                }
            } else {
                for (int cell = 0; cell < 4; cell++) {
                    uint8_t c = cg_base + ((b >> 6) & 3);
                    b <<= 2;
                    for (int r = 0; r < hrep; r++) rowbuf[px++] = c;
                }
            }
        }
        uint8_t packed[128];
        for (int x = 0; x < 256; x += 2)
            packed[x >> 1] = rowbuf[x] | (rowbuf[x + 1] << 4);
        for (int rep = 0; rep < nlpr; rep++) {
            int disp = drow * nlpr + rep;
            if (disp >= 192) break;
            memcpy(&vdg_buffer[disp * 128], packed, sizeof packed);
        }
    }
}

static uint8_t g_ram[65536 + 64];
static uint8_t g_ref[192 * 128], g_new[192 * 128];

static void test_gm_frames_match_the_per_pixel_renderer(void) {
    uint32_t seed = 12345;
    for (size_t i = 0; i < sizeof g_ram; i++) {        // deterministic noise
        seed = seed * 1103515245u + 12345u;
        g_ram[i] = (uint8_t)(seed >> 16);
    }
    static struct vdg_gm_lut lut = { -1, {{0}} };
    const uint16_t bases[] = { 0x0400, 0x0600, 0x0E00, 0x1C00, 0x3000, 0x6000 };
    for (int gm = 0; gm <= 6; gm++)
        for (int css = 0; css <= 1; css++)
            for (unsigned b = 0; b < sizeof bases / sizeof bases[0]; b++) {
                memset(g_ref, 0xAA, sizeof g_ref);
                memset(g_new, 0x55, sizeof g_new);
                ref_render_graphics_frame(g_ram, bases[b], (uint8_t)gm, css, g_ref);
                int stride, lines;                       // SAM agrees with the VDG
                vdg_gfx_geometry((uint8_t)gm, 0, &stride, &lines);
                vdg_render_gm(&lut, g_ram, bases[b], (uint8_t)gm, css, stride, lines, g_new);
                char msg[48];
                snprintf(msg, sizeof msg, "gm=%d css=%d base=%04X", gm, css, bases[b]);
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(g_ref, g_new, sizeof g_ref, msg);
            }
}

static void test_gm_every_byte_value_in_every_mode(void) {
    // Noise might miss a value; walk all 256 through the first data row.
    static struct vdg_gm_lut lut = { -1, {{0}} };
    for (int gm = 0; gm <= 6; gm++)
        for (int css = 0; css <= 1; css++)
            for (int v = 0; v < 256; v++) {
                memset(g_ram, v, 64);
                ref_render_graphics_frame(g_ram, 0, (uint8_t)gm, css, g_ref);
                int stride, lines;
                vdg_gfx_geometry((uint8_t)gm, 0, &stride, &lines);
                vdg_render_gm(&lut, g_ram, 0, (uint8_t)gm, css, stride, lines, g_new);
                TEST_ASSERT_EQUAL_MEMORY(g_ref, g_new, 128);
            }
}

static void test_sam_mode_sets_the_row_geometry(void) {
    int stride, lines;
    // A SAM graphics mode wins over the VDG mode, whatever the VDG says.
    vdg_gfx_geometry(5, 3, &stride, &lines);
    TEST_ASSERT_EQUAL_INT(16, stride); TEST_ASSERT_EQUAL_INT(2, lines);
    vdg_gfx_geometry(7, 4, &stride, &lines);          // RG6 bytes, doubled rows
    TEST_ASSERT_EQUAL_INT(32, stride); TEST_ASSERT_EQUAL_INT(2, lines);
    vdg_gfx_geometry(0, 1, &stride, &lines);
    TEST_ASSERT_EQUAL_INT(16, stride); TEST_ASSERT_EQUAL_INT(3, lines);
    // SAM in text (0) or DMA (7): fall back to the VDG mode's own geometry.
    vdg_gfx_geometry(5, 0, &stride, &lines);
    TEST_ASSERT_EQUAL_INT(16, stride); TEST_ASSERT_EQUAL_INT(1, lines);
    vdg_gfx_geometry(7, 7, &stride, &lines);
    TEST_ASSERT_EQUAL_INT(32, stride); TEST_ASSERT_EQUAL_INT(1, lines);
    // Each SAM mode agrees with the VDG mode of the same shape.
    const uint8_t gm_for_v[7] = { 0, 1, 2, 3, 4, 5, 6 };
    for (unsigned v = 1; v <= 6; v++) {
        int s2, l2;
        vdg_gfx_geometry(gm_for_v[v], 0, &s2, &l2);
        vdg_gfx_geometry(gm_for_v[v], v, &stride, &lines);
        TEST_ASSERT_EQUAL_INT(s2, stride); TEST_ASSERT_EQUAL_INT(l2, lines);
    }
}

static void test_popcorn_gm5_with_sam_v3_is_double_height(void) {
    // VDG GM5 (RG, 16 bytes a line) with the SAM in V3 (16-byte rows, two
    // lines each) must look exactly like GM3, which is RG at 16 bytes with
    // two lines per row: the whole screen, not the top half.
    uint32_t seed = 777;
    for (size_t i = 0; i < sizeof g_ram; i++) { seed = seed * 1103515245u + 12345u; g_ram[i] = (uint8_t)(seed >> 16); }
    static struct vdg_gm_lut lut = { -1, {{0}} };
    for (int css = 0; css <= 1; css++) {
        ref_render_graphics_frame(g_ram, 0x0E00, 3, css, g_ref);
        int stride, lines;
        vdg_gfx_geometry(5, 3, &stride, &lines);
        vdg_render_gm(&lut, g_ram, 0x0E00, 5, css, stride, lines, g_new);
        TEST_ASSERT_EQUAL_MEMORY(g_ref, g_new, sizeof g_ref);
        // and the bottom half really is drawn: row 191 comes from data row 95
        TEST_ASSERT_EQUAL_MEMORY(&g_ref[191 * 128], &g_new[191 * 128], 128);
    }
}

// --- PIZERO-166: font sets ---------------------------------------------------

static void test_classic_is_the_original_6847(void) {
    uint8_t pair;
    TEST_ASSERT_EQUAL_UINT8(0x01, vdg_alpha_glyph(0x41, false, VDG_FONT_CLASSIC, &pair));  // A
    TEST_ASSERT_EQUAL_UINT8(1, pair);
    TEST_ASSERT_EQUAL_UINT8(0x01, vdg_alpha_glyph(0x01, false, VDG_FONT_CLASSIC, &pair));  // inverse A
    TEST_ASSERT_EQUAL_UINT8(0, pair);
    TEST_ASSERT_EQUAL_UINT8(0x1F, vdg_alpha_glyph(0x5F, true, VDG_FONT_CLASSIC, &pair));   // no EXT
}

static void test_t1_without_ext_is_the_6847_behaviour(void) {
    uint8_t pair;
    TEST_ASSERT_EQUAL_UINT8(0x5F, vdg_alpha_glyph(0x5F, false, VDG_FONT_T1, &pair));       // left arrow
    TEST_ASSERT_EQUAL_UINT8(0x41, vdg_alpha_glyph(0x01, false, VDG_FONT_T1, &pair));       // inverse A
    TEST_ASSERT_EQUAL_UINT8(0, pair);
}

static void test_t1_with_ext_has_true_lower_case_and_braces(void) {
    uint8_t pair;
    TEST_ASSERT_EQUAL_UINT8(0x01, vdg_alpha_glyph(0x01, true, VDG_FONT_T1, &pair));        // a
    TEST_ASSERT_EQUAL_UINT8(1, pair);                                                      // INV set
    TEST_ASSERT_EQUAL_UINT8(0x1B, vdg_alpha_glyph(0x1B, true, VDG_FONT_T1, &pair));        // {
    TEST_ASSERT_EQUAL_UINT8(0x41, vdg_alpha_glyph(0x41, true, VDG_FONT_T1, &pair));        // A
    TEST_ASSERT_EQUAL_UINT8(1, pair);
}

static void test_t2_fixes_caret_and_underscore_always(void) {
    uint8_t pair;
    for (int ext = 0; ext < 2; ext++) {
        TEST_ASSERT_EQUAL_UINT8(0x00, vdg_alpha_glyph(0x5E, ext, VDG_FONT_T2, &pair));     // ^
        TEST_ASSERT_EQUAL_UINT8(0x1F, vdg_alpha_glyph(0x5F, ext, VDG_FONT_T2, &pair));     // _
        TEST_ASSERT_EQUAL_UINT8(0x41, vdg_alpha_glyph(0x41, ext, VDG_FONT_T2, &pair));     // A unchanged
    }
    TEST_ASSERT_EQUAL_UINT8(0x41, vdg_alpha_glyph(0x01, false, VDG_FONT_T2, &pair));       // inverse A kept
    TEST_ASSERT_EQUAL_UINT8(0, pair);
}

static void test_every_code_stays_inside_its_font(void) {
    uint8_t pair;
    for (int ch = 0; ch < 128; ch++)
        for (int ext = 0; ext < 2; ext++) {
            TEST_ASSERT_TRUE(vdg_alpha_glyph((uint8_t)ch, ext, VDG_FONT_CLASSIC, &pair) < 64);
            TEST_ASSERT_TRUE(vdg_alpha_glyph((uint8_t)ch, ext, VDG_FONT_T1, &pair) < 128);
            TEST_ASSERT_TRUE(vdg_alpha_glyph((uint8_t)ch, ext, VDG_FONT_T2, &pair) < 128);
            TEST_ASSERT_TRUE(pair <= 1);
        }
}

static int rows_used(uint8_t glyph, int *first, int *last) {
    int n = 0; *first = -1; *last = -1;
    for (int r = 0; r < 12; r++)
        if (font_6847t1[glyph * 12 + r]) { if (*first < 0) *first = r; *last = r; n++; }
    return n;
}

static void test_t2_glyphs_really_are_a_caret_and_an_underscore(void) {
    // Checked against the font data, not the index: the underscore is one
    // low bar, the caret sits in the top half, and the braces mirror.
    int first, last;
    TEST_ASSERT_EQUAL_INT(1, rows_used(0x1F, &first, &last));
    TEST_ASSERT_TRUE(first >= 6);
    TEST_ASSERT_TRUE(rows_used(0x00, &first, &last) > 0);
    TEST_ASSERT_TRUE(last <= 5);
    for (int r = 0; r < 12; r++) {
        uint8_t l = font_6847t1[0x1B * 12 + r], rt = font_6847t1[0x1D * 12 + r], m = 0;
        for (int b = 0; b < 7; b++) if (l & (1 << b)) m |= (uint8_t)(1 << (6 - b));
        TEST_ASSERT_EQUAL_UINT8(m, rt);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack8_matches_put2);
    RUN_TEST(test_pack8_nibble_order_is_low_even);
    RUN_TEST(test_sg4_matches_reference_every_cell);
    RUN_TEST(test_sg4_top_and_bottom_halves_differ);
    RUN_TEST(test_rg6_mono_matches_reference);
    RUN_TEST(test_rg6_artifact_matches_reference_both_phases);
    RUN_TEST(test_rg6_artifact_colour_clock_spans_two_pixels);
    RUN_TEST(test_rg6_artifact_phase_swap_is_visible);
    RUN_TEST(test_alpha_matches_reference_both_colourings);
    RUN_TEST(test_alpha_glyph_index_folds_into_the_t1_block);
    RUN_TEST(test_display_base_legacy_loses_address_zero);
    RUN_TEST(test_display_base_honours_zero_once_written);
    RUN_TEST(test_gm_frames_match_the_per_pixel_renderer);
    RUN_TEST(test_gm_every_byte_value_in_every_mode);
    RUN_TEST(test_sam_mode_sets_the_row_geometry);
    RUN_TEST(test_popcorn_gm5_with_sam_v3_is_double_height);
    RUN_TEST(test_classic_is_the_original_6847);
    RUN_TEST(test_t1_without_ext_is_the_6847_behaviour);
    RUN_TEST(test_t1_with_ext_has_true_lower_case_and_braces);
    RUN_TEST(test_t2_fixes_caret_and_underscore_always);
    RUN_TEST(test_every_code_stays_inside_its_font);
    RUN_TEST(test_t2_glyphs_really_are_a_caret_and_an_underscore);
    return UNITY_END();
}

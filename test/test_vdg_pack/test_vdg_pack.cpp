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

#include <cstring>

#include "../../lib/coco_machine/src/vdg_pack.h"

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
    return UNITY_END();
}

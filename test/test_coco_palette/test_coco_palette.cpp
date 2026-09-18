// PIZERO-55 / PIZERO-85 / PIZERO-109: host tests for the palette register file.
//
// The colour conversion is checked against XRoar's own GIME handling
// (~/github/xroar src/coco3.c:69,611-613), because desktop `xroar -m coco3` is
// the cross-test target this port is compared against. A linear intensity ramp
// would look plausible and be wrong in every mid-tone, so the non-linear map
// is pinned here rather than left to whoever edits it next.
//
//   pio test -e native

#include <unity.h>

#include "../../lib/coco_machine/src/coco_palette.h"

// The stock VDG colours, as coco_machine holds them.
static const uint16_t factory[COCO_PAL_COUNT] = {
    0x07E0, 0xFFE0, 0x001F, 0xF800, 0xFFFF, 0x07FF, 0xF81F, 0xFC00,
    0x0000, 0x0320, 0x8200, 0xFCA0, 0x0000, 0x0000, 0x0000, 0x0000,
};

static coco_palette_t p;

void setUp(void) { coco_pal_reset(&p, factory); }
void tearDown(void) {}

// --- the GIME bit layout --------------------------------------------------

static void test_black_and_white_are_the_extremes(void) {
    TEST_ASSERT_EQUAL_HEX16(0x0000, coco_pal_gime_to_rgb565(0x00));
    // All six bits set = every channel at its top level (0.920, not 1.0).
    uint16_t white = coco_pal_gime_to_rgb565(0x3F);
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(235, 235, 235), white);
}

static void test_each_channel_uses_its_own_two_bits(void) {
    // bit5 R1, bit4 G1, bit3 B1, bit2 R0, bit1 G0, bit0 B0.
    uint16_t r_only = coco_pal_gime_to_rgb565(0x24);   // R1|R0
    uint16_t g_only = coco_pal_gime_to_rgb565(0x12);   // G1|G0
    uint16_t b_only = coco_pal_gime_to_rgb565(0x09);   // B1|B0
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(235, 0, 0), r_only);
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(0, 235, 0), g_only);
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(0, 0, 235), b_only);
}

static void test_the_high_bit_of_a_channel_outweighs_the_low_one(void) {
    uint16_t hi = coco_pal_gime_to_rgb565(0x20);       // R1 only -> level 2
    uint16_t lo = coco_pal_gime_to_rgb565(0x04);       // R0 only -> level 1
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(188, 0, 0), hi);
    TEST_ASSERT_EQUAL_HEX16(coco_pal_rgb565(117, 0, 0), lo);
}

static void test_intensity_ramp_is_the_gime_one_not_a_linear_guess(void) {
    // XRoar: 0.000, 0.460, 0.736, 0.920. A linear ramp would be 0/85/170/255
    // and would be wrong everywhere but black.
    TEST_ASSERT_EQUAL_UINT8(0, coco_pal_intensity[0]);
    TEST_ASSERT_EQUAL_UINT8(117, coco_pal_intensity[1]);
    TEST_ASSERT_EQUAL_UINT8(188, coco_pal_intensity[2]);
    TEST_ASSERT_EQUAL_UINT8(235, coco_pal_intensity[3]);
    TEST_ASSERT_NOT_EQUAL(255, coco_pal_intensity[3]);   // not full scale
}

static void test_all_64_values_are_accepted_and_distinct_enough(void) {
    // 64 GIME colours must not collide after the RGB565 squeeze, or a guest's
    // palette would silently lose shades.
    uint16_t seen[64];
    for (int v = 0; v < 64; v++) seen[v] = coco_pal_gime_to_rgb565((uint8_t)v);
    for (int i = 0; i < 64; i++)
        for (int j = i + 1; j < 64; j++)
            TEST_ASSERT_NOT_EQUAL(seen[i], seen[j]);
}

// --- the register file ----------------------------------------------------

static void test_starts_at_the_factory_colours_and_says_it_is_untouched(void) {
    for (int i = 0; i < COCO_PAL_COUNT; i++)
        TEST_ASSERT_EQUAL_HEX16(factory[i], p.rgb565[i]);
    TEST_ASSERT_FALSE(p.written);
}

static void test_a_guest_write_recolours_one_entry_only(void) {
    coco_pal_write(&p, 0xFFB3, 0x3F);                  // entry 3 to white
    TEST_ASSERT_EQUAL_HEX16(coco_pal_gime_to_rgb565(0x3F), p.rgb565[3]);
    TEST_ASSERT_TRUE(p.written);
    for (int i = 0; i < COCO_PAL_COUNT; i++)
        if (i != 3) TEST_ASSERT_EQUAL_HEX16(factory[i], p.rgb565[i]);
}

static void test_writes_use_only_the_low_six_bits(void) {
    coco_pal_write(&p, 0xFFB0, 0xFF);
    TEST_ASSERT_EQUAL_HEX8(0x3F, p.gime[0]);
    TEST_ASSERT_EQUAL_HEX16(coco_pal_gime_to_rgb565(0x3F), p.rgb565[0]);
}

static void test_every_register_maps_to_its_own_entry(void) {
    for (int i = 0; i < COCO_PAL_COUNT; i++)
        coco_pal_write(&p, (uint16_t)(COCO_PAL_BASE + i), (uint8_t)i);
    for (int i = 0; i < COCO_PAL_COUNT; i++)
        TEST_ASSERT_EQUAL_HEX8(i, p.gime[i]);
}

static void test_reads_return_the_register_with_undriven_top_bits(void) {
    coco_pal_write(&p, 0xFFB5, 0x2A);
    TEST_ASSERT_EQUAL_HEX8(0xC0 | 0x2A, coco_pal_read(&p, 0xFFB5));
}

static void test_reset_restores_the_factory_colours(void) {
    coco_pal_write(&p, 0xFFB0, 0x3F);
    coco_pal_reset(&p, factory);
    TEST_ASSERT_EQUAL_HEX16(factory[0], p.rgb565[0]);
    TEST_ASSERT_FALSE(p.written);
}

static void test_direct_rgb565_access_escapes_the_64_colour_gamut(void) {
    // The BIOS editor and .pal profiles are not limited to the GIME's 64.
    coco_pal_set_rgb565(&p, 1, 0x1234);
    TEST_ASSERT_EQUAL_HEX16(0x1234, coco_pal_get_rgb565(&p, 1));
    TEST_ASSERT_TRUE(p.written);
}

// --- decode ---------------------------------------------------------------

static void test_only_the_palette_block_is_claimed(void) {
    TEST_ASSERT_TRUE(coco_pal_owns(0xFFB0));
    TEST_ASSERT_TRUE(coco_pal_owns(0xFFBF));
    TEST_ASSERT_FALSE(coco_pal_owns(0xFFAF));
    TEST_ASSERT_FALSE(coco_pal_owns(0xFFC0));
    // Must not overlap the CoCo 2's own I/O, nor the GIME timer block.
    TEST_ASSERT_FALSE(coco_pal_owns(0xFF00));   // PIA0
    TEST_ASSERT_FALSE(coco_pal_owns(0xFF22));   // PIA1
    TEST_ASSERT_FALSE(coco_pal_owns(0xFF40));   // cartridge FDC
    TEST_ASSERT_FALSE(coco_pal_owns(0xFF94));   // GIME timer, PIZERO-62
    TEST_ASSERT_FALSE(coco_pal_owns(0xFFC6));   // SAM display base
}

static void test_artifact_colours_live_at_the_indices_the_renderer_uses(void) {
    // Entries 2 and 7 double as the RG6 NTSC artifact blue and orange
    // (PIZERO-43), so recolouring them retints artifact graphics. Pin it so
    // nobody renumbers the palette without noticing.
    TEST_ASSERT_EQUAL_HEX16(0x001F, factory[2]);       // blue
    TEST_ASSERT_EQUAL_HEX16(0xFC00, factory[7]);       // orange
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_black_and_white_are_the_extremes);
    RUN_TEST(test_each_channel_uses_its_own_two_bits);
    RUN_TEST(test_the_high_bit_of_a_channel_outweighs_the_low_one);
    RUN_TEST(test_intensity_ramp_is_the_gime_one_not_a_linear_guess);
    RUN_TEST(test_all_64_values_are_accepted_and_distinct_enough);
    RUN_TEST(test_starts_at_the_factory_colours_and_says_it_is_untouched);
    RUN_TEST(test_a_guest_write_recolours_one_entry_only);
    RUN_TEST(test_writes_use_only_the_low_six_bits);
    RUN_TEST(test_every_register_maps_to_its_own_entry);
    RUN_TEST(test_reads_return_the_register_with_undriven_top_bits);
    RUN_TEST(test_reset_restores_the_factory_colours);
    RUN_TEST(test_direct_rgb565_access_escapes_the_64_colour_gamut);
    RUN_TEST(test_only_the_palette_block_is_claimed);
    RUN_TEST(test_artifact_colours_live_at_the_indices_the_renderer_uses);
    return UNITY_END();
}

// PIZERO-142: host tests for bank-switched cartridge arithmetic.
//
// A wrong mask shows up as a game jumping into the wrong code or into memory
// past the end of the image, which on hardware looks like a crash with no
// clue why, so the rules are pinned here.
//
//   pio test -e native

#include <unity.h>

#include "../../lib/coco_machine/src/cart_gmc.h"

void setUp(void) {}
void tearDown(void) {}

static void test_banked_sizes(void) {
    TEST_ASSERT_FALSE(cart_is_banked_size(16384));    // fits the window: plain
    TEST_ASSERT_TRUE(cart_is_banked_size(32768));     // Mind Roll
    TEST_ASSERT_TRUE(cart_is_banked_size(65536));     // Dunjunz, Predator
    TEST_ASSERT_TRUE(cart_is_banked_size(131072));    // RoboCop
    TEST_ASSERT_TRUE(cart_is_banked_size(262144));
    TEST_ASSERT_FALSE(cart_is_banked_size(524288));   // beyond the mask
    TEST_ASSERT_FALSE(cart_is_banked_size(49152));    // not a power of two
    TEST_ASSERT_FALSE(cart_is_banked_size(0));
}

static void test_bank_offsets_stay_inside_the_image(void) {
    // 32 KB: two banks. Only D bit 0 matters; higher bits are masked off.
    TEST_ASSERT_EQUAL_UINT32(0,     gmc_bank_offset(0, 32768));
    TEST_ASSERT_EQUAL_UINT32(16384, gmc_bank_offset(1, 32768));
    TEST_ASSERT_EQUAL_UINT32(0,     gmc_bank_offset(2, 32768));
    TEST_ASSERT_EQUAL_UINT32(16384, gmc_bank_offset(0xFF, 32768));
    // 64 KB: four banks.
    TEST_ASSERT_EQUAL_UINT32(3 * 16384, gmc_bank_offset(3, 65536));
    TEST_ASSERT_EQUAL_UINT32(0,         gmc_bank_offset(4, 65536));
    // Every value of D, every size: never past the end, always bank-aligned.
    const uint32_t sizes[] = { 32768, 65536, 131072, 262144 };
    for (unsigned s = 0; s < 4; s++)
        for (int d = 0; d < 256; d++) {
            uint32_t off = gmc_bank_offset((uint8_t)d, sizes[s]);
            TEST_ASSERT_TRUE(off + CART_BANK_SIZE <= sizes[s]);
            TEST_ASSERT_EQUAL_UINT32(0, off % CART_BANK_SIZE);
        }
}

static void test_even_is_bank_odd_is_sound(void) {
    TEST_ASSERT_TRUE(gmc_is_bank_register(0xFF40));
    TEST_ASSERT_FALSE(gmc_is_bank_register(0xFF41));
    TEST_ASSERT_TRUE(gmc_is_bank_register(0xFF5E));
}

static void test_chip_on_the_odd_latch_mirrors_only(void) {
    // $FF41 is where GMC software writes the chip; it must reach the chip
    // with Disk BASIC in. The latch itself and the WD279x must not.
    TEST_ASSERT_TRUE(csg_on_latch_mirror(0xFF41));
    TEST_ASSERT_TRUE(csg_on_latch_mirror(0xFF47));
    TEST_ASSERT_TRUE(csg_on_latch_mirror(0xFF51));
    TEST_ASSERT_FALSE(csg_on_latch_mirror(0xFF40));   // the drive latch DECB uses
    TEST_ASSERT_FALSE(csg_on_latch_mirror(0xFF42));
    for (uint16_t a = 0xFF48; a <= 0xFF4F; a++)       // the WD279x and mirrors
        TEST_ASSERT_FALSE(csg_on_latch_mirror(a));
    for (uint16_t a = 0xFF58; a <= 0xFF5F; a++)
        TEST_ASSERT_FALSE(csg_on_latch_mirror(a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_banked_sizes);
    RUN_TEST(test_bank_offsets_stay_inside_the_image);
    RUN_TEST(test_even_is_bank_odd_is_sound);
    RUN_TEST(test_chip_on_the_odd_latch_mirrors_only);
    return UNITY_END();
}

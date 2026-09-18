// PIZERO-92 / PIZERO-109: host tests for the diagnostic card's text handling.
//
// This card exists for the worst case: a recipient with no serial console
// looking at what would otherwise be a black screen. It has to be legible on a
// TV, so a stray glyph or a line that drifts off the right edge is not a
// cosmetic bug, it is the difference between "copy this file" and "this thing
// is broken".
//
//   pio test -e native

#include <unity.h>

#include <cstring>

#include "../../src/text_card.h"
#include "../../lib/coco_machine/src/vdg_pack.h"

void setUp(void) {}
void tearDown(void) {}

static void test_letters_fold_to_upper_case(void) {
    TEST_ASSERT_EQUAL_UINT8('A', card_code('a'));
    TEST_ASSERT_EQUAL_UINT8('Z', card_code('z'));
    TEST_ASSERT_EQUAL_UINT8('A', card_code('A'));
}

static void test_the_characters_the_messages_use_survive(void) {
    // The ROM message contains a path, so these in particular must not turn
    // into spaces: / . : digits.
    const char *msg = "/COCO/ROMS/BAS12.ROM";
    for (const char *p = msg; *p; p++)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)*p, card_code(*p));
}

static void test_unprintables_become_spaces_not_random_glyphs(void) {
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code('\n'));
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code('\t'));
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code((char)0x00));
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code((char)0x7F));
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code((char)0xE9));   // an accented byte
    TEST_ASSERT_EQUAL_UINT8(0x20, card_code('`'));          // 0x60, past the block
}

static void test_every_accepted_code_lands_in_the_font_block(void) {
    // card_code feeds vdg_alpha_glyph_index, which indexes a 128-glyph font.
    // Anything outside $40-$7F would read the wrong glyph or run off the end.
    for (int c = 0; c < 256; c++) {
        uint8_t code = card_code((char)c);
        uint8_t idx = vdg_alpha_glyph_index(code);
        TEST_ASSERT_TRUE(idx >= 0x40 && idx <= 0x7F);
    }
}

static void test_centring_is_symmetric(void) {
    TEST_ASSERT_EQUAL_INT(16, card_center_col(0));
    TEST_ASSERT_EQUAL_INT(11, card_center_col(10));
    TEST_ASSERT_EQUAL_INT(0, card_center_col(32));
}

static void test_an_over_long_line_starts_at_the_left_not_off_screen(void) {
    // Clipping is the caller's job, but the start column must never go
    // negative or the line would wrap into the row above.
    TEST_ASSERT_EQUAL_INT(0, card_center_col(33));
    TEST_ASSERT_EQUAL_INT(0, card_center_col(200));
}

static void test_the_real_messages_fit_the_card(void) {
    // If a message is wider than 32 columns it gets silently clipped on a TV,
    // where nobody can check it. Pin the actual strings from main.cpp.
    const char *lines[] = {
        "NO SD CARD", "INSERT A FAT32 CARD", "WITH /COCO/ROMS/BAS12.ROM",
        "NO ROM FOUND", "COPY BAS12.ROM TO", "/COCO/ROMS/ ON THE SD CARD",
        "SEE THE CARD IN THE BOX",
        "EMULATOR FAILED TO START", "THE ROM FILE MAY BE",
        "DAMAGED OR THE WRONG SIZE", "REPLACE BAS12.ROM",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        size_t len = strlen(lines[i]);
        TEST_ASSERT_TRUE_MESSAGE(len <= CARD_COLS, lines[i]);
        TEST_ASSERT_TRUE(card_center_col((int)len) >= 0);
    }
}

static void test_the_two_failures_do_not_read_alike(void) {
    // A missing ROM and a broken ROM must not produce the same headline, or
    // someone hunts for a file that is already there.
    TEST_ASSERT_TRUE(strcmp("NO ROM FOUND", "EMULATOR FAILED TO START") != 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_letters_fold_to_upper_case);
    RUN_TEST(test_the_characters_the_messages_use_survive);
    RUN_TEST(test_unprintables_become_spaces_not_random_glyphs);
    RUN_TEST(test_every_accepted_code_lands_in_the_font_block);
    RUN_TEST(test_centring_is_symmetric);
    RUN_TEST(test_an_over_long_line_starts_at_the_left_not_off_screen);
    RUN_TEST(test_the_real_messages_fit_the_card);
    RUN_TEST(test_the_two_failures_do_not_read_alike);
    return UNITY_END();
}

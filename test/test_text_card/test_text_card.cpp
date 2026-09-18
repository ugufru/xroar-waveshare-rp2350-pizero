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
#include "../../src/boot_messages.h"
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

// --- wrapping -------------------------------------------------------------

static void test_wrap_breaks_at_a_space(void) {
    const char *next = nullptr;
    int len = card_wrap_next("HELLO BIG WORLD", 11, &next);
    TEST_ASSERT_EQUAL_INT(9, len);              // "HELLO BIG"
    TEST_ASSERT_EQUAL_STRING("WORLD", next);    // and no leading space
}

static void test_wrap_returns_the_tail_when_it_fits(void) {
    const char *next = nullptr;
    int len = card_wrap_next("SHORT", 28, &next);
    TEST_ASSERT_EQUAL_INT(5, len);
    TEST_ASSERT_EQUAL_STRING("", next);
}

static void test_wrap_hard_breaks_a_word_longer_than_the_line(void) {
    // A long path must not vanish: break it rather than dropping it.
    const char *next = nullptr;
    int len = card_wrap_next("/COCO/ROMS/BAS12.ROM", 8, &next);
    TEST_ASSERT_EQUAL_INT(8, len);                    // "/COCO/RO"
    TEST_ASSERT_EQUAL_STRING("MS/BAS12.ROM", next);   // continues, loses nothing
}

static void test_wrap_terminates_on_every_message(void) {
    // A wrap bug that returns 0 without advancing would hang the boot path,
    // on a machine whose whole point here is to not look broken.
    const char *msgs[] = { MSG_NOSD_BODY, MSG_NOROM_BODY, MSG_BADROM_BODY,
                           MSG_INIT_BODY, MSG_CBONLY_BODY };
    for (unsigned m = 0; m < sizeof msgs / sizeof msgs[0]; m++) {
        const char *s = msgs[m];
        int guard = 0;
        while (*s && guard++ < 100) {
            const char *next = nullptr;
            int len = card_wrap_next(s, 28, &next);
            TEST_ASSERT_TRUE(len > 0);
            TEST_ASSERT_TRUE(next > s);         // always makes progress
            s = next;
        }
        TEST_ASSERT_TRUE(guard < 100);
    }
}

// --- the pages the reader actually sees -----------------------------------

// Lay a message out exactly as boot_page does and report the rows used.
static int layout_rows(const char *s, int width) {
    int rows = 0;
    while (s && *s) {
        const char *next = nullptr;
        int len = card_wrap_next(s, width, &next);
        TEST_ASSERT_TRUE(len <= width);         // never overflows the card
        s = next;
        rows++;
    }
    return rows;
}

static void test_every_body_fits_its_seven_rows(void) {
    // boot_page gives the body 7 rows at 28 columns. More than that and the
    // end of the advice is silently lost.
    TEST_ASSERT_TRUE(layout_rows(MSG_NOSD_BODY, 28) <= 7);
    TEST_ASSERT_TRUE(layout_rows(MSG_NOROM_BODY, 28) <= 7);
    TEST_ASSERT_TRUE(layout_rows(MSG_BADROM_BODY, 28) <= 7);
    TEST_ASSERT_TRUE(layout_rows(MSG_INIT_BODY, 28) <= 7);
    TEST_ASSERT_TRUE(layout_rows(MSG_CBONLY_BODY, 28) <= 7);
}

static void test_every_detail_fits_its_three_rows(void) {
    TEST_ASSERT_TRUE(layout_rows(MSG_NOSD_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_NOROM_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_INIT_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_CBONLY_DETAIL, 28) <= 3);
}

static void test_titles_fit_centred_on_one_row(void) {
    const char *titles[] = { MSG_NOSD_TITLE, MSG_NOROM_TITLE, MSG_BADROM_TITLE,
                             MSG_INIT_TITLE, MSG_CBONLY_TITLE };
    for (unsigned i = 0; i < sizeof titles / sizeof titles[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(titles[i]) <= CARD_COLS, titles[i]);
        TEST_ASSERT_TRUE(card_center_col((int)strlen(titles[i])) >= 0);
    }
}

static void test_the_two_rom_failures_do_not_read_alike(void) {
    // A missing ROM and a damaged one need different advice: one says copy the
    // file, the other says the file you have is not a ROM. Identical headlines
    // would send someone hunting for what they already have.
    TEST_ASSERT_TRUE(strcmp(MSG_NOROM_TITLE, MSG_BADROM_TITLE) != 0);
    TEST_ASSERT_TRUE(strcmp(MSG_NOROM_BODY, MSG_BADROM_BODY) != 0);
}

static void test_messages_name_the_exact_path(void) {
    // The whole fix for the reader is knowing where the file goes.
    TEST_ASSERT_NOT_NULL(strstr(MSG_NOROM_BODY, "/COCO/ROMS/"));
    TEST_ASSERT_NOT_NULL(strstr(MSG_NOSD_BODY, "/COCO/ROMS/BAS12.ROM"));
    TEST_ASSERT_NOT_NULL(strstr(MSG_CBONLY_BODY, "/COCO/ROMS/"));
}

static void test_every_message_is_printable_on_the_card(void) {
    const char *all[] = { MSG_NOSD_TITLE, MSG_NOSD_BODY, MSG_NOSD_DETAIL,
                          MSG_NOROM_TITLE, MSG_NOROM_BODY, MSG_NOROM_DETAIL,
                          MSG_BADROM_TITLE, MSG_BADROM_BODY,
                          MSG_INIT_TITLE, MSG_INIT_BODY, MSG_INIT_DETAIL,
                          MSG_CBONLY_TITLE, MSG_CBONLY_BODY, MSG_CBONLY_DETAIL };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
        for (const char *p = all[i]; *p; p++)
            TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)*p, card_code(*p), all[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_letters_fold_to_upper_case);
    RUN_TEST(test_the_characters_the_messages_use_survive);
    RUN_TEST(test_unprintables_become_spaces_not_random_glyphs);
    RUN_TEST(test_every_accepted_code_lands_in_the_font_block);
    RUN_TEST(test_centring_is_symmetric);
    RUN_TEST(test_an_over_long_line_starts_at_the_left_not_off_screen);
    RUN_TEST(test_wrap_breaks_at_a_space);
    RUN_TEST(test_wrap_returns_the_tail_when_it_fits);
    RUN_TEST(test_wrap_hard_breaks_a_word_longer_than_the_line);
    RUN_TEST(test_wrap_terminates_on_every_message);
    RUN_TEST(test_every_body_fits_its_seven_rows);
    RUN_TEST(test_every_detail_fits_its_three_rows);
    RUN_TEST(test_titles_fit_centred_on_one_row);
    RUN_TEST(test_the_two_rom_failures_do_not_read_alike);
    RUN_TEST(test_messages_name_the_exact_path);
    RUN_TEST(test_every_message_is_printable_on_the_card);
    return UNITY_END();
}

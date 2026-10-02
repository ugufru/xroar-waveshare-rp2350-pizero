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

#include <cstdio>
#include <cstring>

#include "../../src/text_card.h"
#include "../../src/boot_messages.h"

void setUp(void) {}
void tearDown(void) {}

// PIZERO-162: the card indexes the 6847T1 font directly. Each character
// must land on its own glyph, checked against the font's layout.
static void test_upper_case_and_punctuation_keep_their_glyphs(void) {
    TEST_ASSERT_EQUAL_UINT8(0x41, card_code('A'));   // font $40-$5D: @ A-Z [ \ ]
    TEST_ASSERT_EQUAL_UINT8(0x5A, card_code('Z'));
    TEST_ASSERT_EQUAL_UINT8(0x40, card_code('@'));
    TEST_ASSERT_EQUAL_UINT8(0x5B, card_code('['));
    TEST_ASSERT_EQUAL_UINT8(0x5C, card_code('\\'));
    TEST_ASSERT_EQUAL_UINT8(0x5D, card_code(']'));
    TEST_ASSERT_EQUAL_UINT8(0x60, card_code(' '));   // font $60-$7F: space to ?
    TEST_ASSERT_EQUAL_UINT8(0x6F, card_code('/'));
    TEST_ASSERT_EQUAL_UINT8(0x7F, card_code('?'));
}

static void test_lower_case_is_real_lower_case(void) {
    TEST_ASSERT_EQUAL_UINT8(0x01, card_code('a'));   // font $01-$1A
    TEST_ASSERT_EQUAL_UINT8(0x1A, card_code('z'));
}

static void test_the_characters_a_coco_cannot_show(void) {
    // A CoCo shows _ and ^ as arrows; the card uses the 6847T2 font, whose
    // slots for them hold a caret and an underscore, and which has a backtick.
    TEST_ASSERT_EQUAL_UINT8(0x5F, card_code('_'));
    TEST_ASSERT_EQUAL_UINT8(0x5E, card_code('^'));
    TEST_ASSERT_EQUAL_UINT8(0x00, card_code('`'));
    TEST_ASSERT_EQUAL_UINT8(0x1B, card_code('{'));
    TEST_ASSERT_EQUAL_UINT8(0x1C, card_code('|'));
    TEST_ASSERT_EQUAL_UINT8(0x1D, card_code('}'));
    TEST_ASSERT_EQUAL_UINT8(0x1E, card_code('~'));
}

static void test_the_characters_the_messages_use_survive(void) {
    // The ROM message contains a path, so these in particular must not turn
    // into spaces: / . : digits.
    const char *msg = "/COCO/ROMS/BAS12.ROM";
    for (const char *p = msg; *p; p++) {
        TEST_ASSERT_TRUE(card_printable(*p));
        TEST_ASSERT_NOT_EQUAL(CARD_SPACE, card_code(*p));
    }
}

static void test_unprintables_become_spaces_not_random_glyphs(void) {
    TEST_ASSERT_EQUAL_UINT8(CARD_SPACE, card_code('\n'));
    TEST_ASSERT_EQUAL_UINT8(CARD_SPACE, card_code('\t'));
    TEST_ASSERT_EQUAL_UINT8(CARD_SPACE, card_code((char)0x00));
    TEST_ASSERT_EQUAL_UINT8(CARD_SPACE, card_code((char)0x7F));
    TEST_ASSERT_EQUAL_UINT8(CARD_SPACE, card_code((char)0xE9));   // an accented byte
    TEST_ASSERT_FALSE(card_printable('\n'));
    TEST_ASSERT_FALSE(card_printable((char)0xE9));
}

static void test_every_code_is_a_glyph_and_no_two_characters_share_one(void) {
    // Indexes must stay in the 128-glyph font with bit 7 free for inverse,
    // and every printable character needs its own glyph.
    bool used[128] = { false };
    for (int c = 0x20; c <= 0x7E; c++) {
        uint8_t g = card_code((char)c);
        TEST_ASSERT_TRUE(g < 0x80);
        TEST_ASSERT_FALSE_MESSAGE(used[g], "two characters share a glyph");
        used[g] = true;
    }
    for (int c = 0; c < 256; c++) TEST_ASSERT_TRUE(card_code((char)c) < 0x80);
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
                           MSG_INIT_BODY, MSG_CBONLY_BODY, MSG_RUNSKIP_BODY };
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
    TEST_ASSERT_TRUE(layout_rows(MSG_RUNSKIP_BODY, 28) <= 7);
}

static void test_every_detail_fits_its_three_rows(void) {
    TEST_ASSERT_TRUE(layout_rows(MSG_NOSD_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_NOROM_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_INIT_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_CBONLY_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(layout_rows(MSG_RUNSKIP_DETAIL, 28) <= 3);
}

static void test_titles_fit_centred_on_one_row(void) {
    const char *titles[] = { MSG_NOSD_TITLE, MSG_NOROM_TITLE, MSG_BADROM_TITLE,
                             MSG_INIT_TITLE, MSG_CBONLY_TITLE, MSG_RUNSKIP_TITLE };
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
                          MSG_CBONLY_TITLE, MSG_CBONLY_BODY, MSG_CBONLY_DETAIL,
                          MSG_RUNSKIP_TITLE, MSG_RUNSKIP_BODY, MSG_RUNSKIP_DETAIL };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
        for (const char *p = all[i]; *p; p++)
            TEST_ASSERT_TRUE_MESSAGE(card_printable(*p), all[i]);
}

static void test_autorun_missing_page_fits_with_a_long_name(void) {
    // The name comes from the user's file, so try the longest a catalogue
    // entry can be (63 characters) with the longest directive.
    char name[64];
    memset(name, 'X', 63); name[63] = 0;
    char body[160];
    snprintf(body, sizeof body, MSG_ARMISS_BODY, "@DIRECT", name);
    TEST_ASSERT_TRUE(layout_rows(body, 28) <= 7);
    TEST_ASSERT_TRUE(layout_rows(MSG_ARMISS_DETAIL, 28) <= 3);
    TEST_ASSERT_TRUE(strlen(MSG_ARMISS_TITLE) <= CARD_COLS);
    const char *all[] = { MSG_ARMISS_TITLE, MSG_ARMISS_DETAIL, body };
    for (unsigned i = 0; i < 3; i++)
        for (const char *p = all[i]; *p; p++)
            TEST_ASSERT_TRUE_MESSAGE(card_printable(*p), all[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_upper_case_and_punctuation_keep_their_glyphs);
    RUN_TEST(test_lower_case_is_real_lower_case);
    RUN_TEST(test_the_characters_a_coco_cannot_show);
    RUN_TEST(test_the_characters_the_messages_use_survive);
    RUN_TEST(test_unprintables_become_spaces_not_random_glyphs);
    RUN_TEST(test_every_code_is_a_glyph_and_no_two_characters_share_one);
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
    RUN_TEST(test_autorun_missing_page_fits_with_a_long_name);
    return UNITY_END();
}

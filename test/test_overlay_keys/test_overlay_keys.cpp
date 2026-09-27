// PIZERO-81c: host tests for the F12 overlay's key handling.
//
// The failure these guard against is invisible until someone types: a key
// that leaks into BASIC while the overlay is open, or one the overlay steals
// while it is closed, breaks normal use of the machine. Both states are
// tested, and so is the ESC that closes the overlay (it is also BREAK).
//
//   pio test -e native

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "../../src/overlay_keys.h"
#include "../../src/text_card.h"

void setUp(void) {}
void tearDown(void) {}

static struct ovk_state s;
static uint32_t frame;

static struct ovk_result press(uint8_t a, uint8_t b = 0) {
    uint8_t c[6] = { a, b, 0, 0, 0, 0 };
    return ovk_report(&s, c, frame);
}
static struct ovk_result none(void) { return press(0); }

static void open_with(int n) {
    ovk_init(&s);
    frame = 1000;
    ovk_set_count(&s, n);
    press(HK_F12);
    none();
}

static void test_closed_passes_everything_but_f12(void) {
    ovk_init(&s);
    const uint8_t keys[] = { 0x04 /*A*/, 0x28 /*ENTER*/, HK_ESC, HK_UP, HK_DOWN,
                             HK_1, HK_0, HK_PGDN, HK_HOME };
    for (unsigned i = 0; i < sizeof keys; i++) {
        struct ovk_result r = press(keys[i]);
        TEST_ASSERT_FALSE(r.swallow);
        TEST_ASSERT_EQUAL_UINT8(OVK_NONE, r.action);
        none();
    }
    TEST_ASSERT_FALSE(s.open);
}

static void test_esc_never_opens(void) {
    // ESC is BREAK. Opening on it would steal the most important key a
    // BASIC program has.
    ovk_init(&s);
    TEST_ASSERT_FALSE(press(HK_ESC).swallow);
    TEST_ASSERT_FALSE(s.open);
}

static void test_f12_opens_once_even_when_held(void) {
    ovk_init(&s);
    struct ovk_result r = press(HK_F12);
    TEST_ASSERT_EQUAL_UINT8(OVK_OPEN, r.action);
    TEST_ASSERT_TRUE(r.swallow);
    r = press(HK_F12);                     // auto-repeat report, still held
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, r.action);
    TEST_ASSERT_TRUE(s.open);
}

static void test_open_swallows_everything(void) {
    open_with(5);
    for (int k = 0x04; k <= 0x65; k++) {   // letters through keypad
        if (k == HK_F12 || k == HK_ESC) continue;
        TEST_ASSERT_TRUE(press((uint8_t)k).swallow);
        TEST_ASSERT_TRUE(none().swallow);
    }
    TEST_ASSERT_TRUE(s.open);
}

static void test_esc_and_f12_close_and_are_swallowed(void) {
    open_with(5);
    struct ovk_result r = press(HK_ESC);
    TEST_ASSERT_EQUAL_UINT8(OVK_CLOSE, r.action);
    TEST_ASSERT_TRUE(r.swallow);
    // The ESC is still held as the next report arrives: closed now, it must
    // pass through as an ordinary report, and must not reopen anything.
    r = press(HK_ESC);
    TEST_ASSERT_FALSE(r.swallow);
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, r.action);

    open_with(5);
    TEST_ASSERT_EQUAL_UINT8(OVK_CLOSE, press(HK_F12).action);
    TEST_ASSERT_FALSE(s.open);
}

static void test_f12_held_through_close_does_not_reopen(void) {
    open_with(5);
    press(HK_F12);                          // closes
    TEST_ASSERT_FALSE(s.open);
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, press(HK_F12).action);   // still held
    TEST_ASSERT_FALSE(s.open);
}

static void test_up_down_wrap(void) {
    open_with(3);
    TEST_ASSERT_EQUAL_UINT8(OVK_MOVED, press(HK_UP).action);
    TEST_ASSERT_EQUAL_INT(2, s.sel);        // wrapped from 0 to the end
    none();
    press(HK_DOWN);
    TEST_ASSERT_EQUAL_INT(0, s.sel);        // and back round
}

static void test_page_home_end_clamp(void) {
    open_with(40);
    press(HK_PGDN); none();
    TEST_ASSERT_EQUAL_INT(OVK_ROWS, s.sel);
    press(HK_END); none();
    TEST_ASSERT_EQUAL_INT(39, s.sel);
    press(HK_PGDN); none();
    TEST_ASSERT_EQUAL_INT(39, s.sel);       // clamps, does not wrap
    press(HK_HOME); none();
    TEST_ASSERT_EQUAL_INT(0, s.sel);
    press(HK_PGUP); none();
    TEST_ASSERT_EQUAL_INT(0, s.sel);
}

static void test_drive_keys(void) {
    open_with(4);
    struct ovk_result r = press(HK_0);
    TEST_ASSERT_EQUAL_UINT8(OVK_DRIVE, r.action);
    TEST_ASSERT_EQUAL_INT8(0, r.drive);
    r = press(HK_0);                        // held: no second toggle
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, r.action);
    none();
    for (int d = 1; d <= 3; d++) {
        r = press((uint8_t)(HK_1 + d - 1));
        TEST_ASSERT_EQUAL_UINT8(OVK_DRIVE, r.action);
        TEST_ASSERT_EQUAL_INT8(d, r.drive);
        none();
    }
    // 4 is not a drive.
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, press(HK_1 + 3).action);
}

static void test_empty_list_is_safe(void) {
    open_with(0);
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, press(HK_DOWN).action); none();
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, press(HK_END).action); none();
    TEST_ASSERT_EQUAL_UINT8(OVK_NONE, press(HK_1).action);   // nothing to mount
    TEST_ASSERT_EQUAL_INT(0, s.sel);
    TEST_ASSERT_TRUE(s.open);
}

static void test_held_down_repeats_after_a_delay(void) {
    open_with(50);
    press(HK_DOWN);                         // immediate first move
    TEST_ASSERT_EQUAL_INT(1, s.sel);
    for (int i = 1; i < OVK_REPEAT_DELAY; i++) {
        frame++;
        TEST_ASSERT_EQUAL_UINT8(OVK_NONE, ovk_tick(&s, frame));
    }
    frame++;
    TEST_ASSERT_EQUAL_UINT8(OVK_MOVED, ovk_tick(&s, frame));
    TEST_ASSERT_EQUAL_INT(2, s.sel);
    for (int i = 1; i < OVK_REPEAT_RATE; i++) { frame++; ovk_tick(&s, frame); }
    TEST_ASSERT_EQUAL_INT(2, s.sel);
    frame++;
    ovk_tick(&s, frame);
    TEST_ASSERT_EQUAL_INT(3, s.sel);
    none();                                 // released: repeat stops
    for (int i = 0; i < 100; i++) { frame++; ovk_tick(&s, frame); }
    TEST_ASSERT_EQUAL_INT(3, s.sel);
}

static void test_rescan_keeps_selection_in_range(void) {
    open_with(10);
    press(HK_END); none();
    ovk_set_count(&s, 4);                   // card swapped for a smaller one
    TEST_ASSERT_EQUAL_INT(3, s.sel);
    ovk_set_count(&s, 0);
    TEST_ASSERT_EQUAL_INT(0, s.sel);
}

static void test_window_follows_selection(void) {
    TEST_ASSERT_EQUAL_INT(0, ovk_top(5, 0, 10));               // fits: no scroll
    TEST_ASSERT_EQUAL_INT(0, ovk_top(13, 0, 40));
    TEST_ASSERT_EQUAL_INT(1, ovk_top(14, 0, 40));              // one past the page
    TEST_ASSERT_EQUAL_INT(10, ovk_top(10, 12, 40));            // moving back up
    TEST_ASSERT_EQUAL_INT(40 - OVK_ROWS, ovk_top(39, 0, 40));  // End
}

// The overlay's words must fit the 32-column card and use only characters
// the 6847 can draw; anything else is silently lost or turned into spaces.
static int rows_needed(const char *s, int width) {
    int rows = 0;
    while (s && *s) { const char *n = s; card_wrap_next(s, width, &n); s = n; rows++; }
    return rows;
}
static void test_overlay_words_fit_and_print(void) {
    TEST_ASSERT_TRUE(strlen(OVL_TITLE) <= CARD_COLS);
    TEST_ASSERT_TRUE(strlen(OVL_LEGEND) <= CARD_COLS);
    TEST_ASSERT_TRUE(strlen(OVL_EMPTY_1) <= CARD_COLS);
    TEST_ASSERT_TRUE(rows_needed(OVL_EMPTY_2, 28) <= 4);   // drawn wrapped, 4 rows
    char skipped[64];
    snprintf(skipped, sizeof skipped, OVL_SKIPPED, 128);  // worst case
    TEST_ASSERT_TRUE(strlen(skipped) <= CARD_COLS);
    const char *all[] = { OVL_TITLE, OVL_LEGEND, OVL_EMPTY_1, OVL_EMPTY_2, skipped };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
        for (const char *p = all[i]; *p; p++)
            TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)*p, card_code(*p), all[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_closed_passes_everything_but_f12);
    RUN_TEST(test_esc_never_opens);
    RUN_TEST(test_f12_opens_once_even_when_held);
    RUN_TEST(test_open_swallows_everything);
    RUN_TEST(test_esc_and_f12_close_and_are_swallowed);
    RUN_TEST(test_f12_held_through_close_does_not_reopen);
    RUN_TEST(test_up_down_wrap);
    RUN_TEST(test_page_home_end_clamp);
    RUN_TEST(test_drive_keys);
    RUN_TEST(test_empty_list_is_safe);
    RUN_TEST(test_held_down_repeats_after_a_delay);
    RUN_TEST(test_rescan_keeps_selection_in_range);
    RUN_TEST(test_window_follows_selection);
    RUN_TEST(test_overlay_words_fit_and_print);
    return UNITY_END();
}

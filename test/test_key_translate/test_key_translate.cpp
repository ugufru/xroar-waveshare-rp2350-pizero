// PIZERO-163: host tests for the USB keyboard translation.
//
//   pio test -e native

#include <unity.h>

#include "../../src/key_translate.h"

void setUp(void) {}
void tearDown(void) {}

#define SHIFT_L 0x02

// HID usages used below.
enum {
    H_A = 0x04, H_1 = 0x1E, H_2 = 0x1F, H_7 = 0x24, H_8 = 0x25, H_9 = 0x26, H_0 = 0x27,
    H_ENTER = 0x28, H_ESC = 0x29, H_BKSP = 0x2A, H_SPACE = 0x2C, H_MINUS = 0x2D,
    H_EQUAL = 0x2E, H_LBRACKET = 0x2F, H_RBRACKET = 0x30, H_BACKSLASH = 0x31,
    H_SEMI = 0x33, H_QUOTE = 0x34, H_GRAVE = 0x35, H_CAPS = 0x39, H_HOME = 0x4A,
    H_LEFT = 0x50, H_UP = 0x52, H_KP_STAR = 0x55, H_KP_7 = 0x5F, H_KP_DOT = 0x63,
};

// The CoCo keys a single key produces, as a sorted-free check.
static int keys_for(uint8_t hid, bool shift, uint8_t *out) {
    struct kt_state st; kt_init(&st);
    uint8_t codes[6] = { hid, 0, 0, 0, 0, 0 };
    uint8_t mods = shift ? SHIFT_L : 0;
    kt_update(&st, mods, codes);
    return kt_keys(&st, mods, out);
}

static bool has(const uint8_t *k, int n, uint8_t d) {
    for (int i = 0; i < n; i++) if (k[i] == d) return true;
    return false;
}

// Assert a key types exactly `d`, with or without the CoCo's SHIFT.
static void expect(uint8_t hid, bool shift, uint8_t d, bool coco_shift) {
    uint8_t k[8];
    int n = keys_for(hid, shift, k);
    TEST_ASSERT_TRUE(has(k, n, d));
    TEST_ASSERT_EQUAL(coco_shift, has(k, n, K_SHIFT));
    TEST_ASSERT_EQUAL_INT(coco_shift ? 2 : 1, n);
}

static void test_the_keys_the_user_found_unmapped(void) {
    expect(H_MINUS, false, K_MINUS, false);      // -
    expect(H_MINUS, true,  K_UP,    true);       // _  = SHIFT+UP
    expect(H_EQUAL, false, K_MINUS, true);       // =  = SHIFT+-
    expect(H_EQUAL, true,  K_SEMI,  true);       // +  = SHIFT+;
    expect(H_SEMI,  false, K_SEMI,  false);      // ;
    expect(H_SEMI,  true,  K_COLON, false);      // :  unshifted on a CoCo
    expect(H_QUOTE, false, K_7,     true);       // '  = SHIFT+7
    expect(H_QUOTE, true,  K_2,     true);       // "  = SHIFT+2
}

static void test_the_digit_row_types_its_keycaps(void) {
    expect(H_0, true, K_9, true);                // )  (Shift+0 no longer the case toggle)
    expect(H_9, true, K_8, true);                // (
    expect(H_2, true, K_AT, false);              // @  unshifted on a CoCo
    expect(H_8, true, K_COLON, true);            // *
    expect(H_7, false, K_7, false);              // 7, SHIFT forced off
}

static void test_brackets_and_backslash_use_basics_chords(void) {
    expect(H_LBRACKET,  false, K_DOWN,  true);
    expect(H_RBRACKET,  false, K_RIGHT, true);
    expect(H_BACKSLASH, false, K_CLEAR, true);
    expect(H_2 + 4, true, K_UP, false);          // ^ (Shift+6) is UP, unshifted
}

static void test_the_coco_has_no_braces_bar_tilde_or_backtick(void) {
    // Nothing is pressed for them; the physical Shift that made { | ~ is
    // still the CoCo's SHIFT, as Shift alone always is.
    uint8_t k[8];
    const uint8_t shifted[] = { H_LBRACKET, H_BACKSLASH, H_GRAVE };   // { | ~
    for (uint8_t h : shifted) {
        TEST_ASSERT_EQUAL_INT(1, keys_for(h, true, k));
        TEST_ASSERT_EQUAL_UINT8(K_SHIFT, k[0]);
    }
    TEST_ASSERT_EQUAL_INT(0, keys_for(H_GRAVE, false, k));      // `
}

static void test_letters_and_control_keys_keep_the_physical_shift(void) {
    expect(H_A, false, K_A, false);
    expect(H_A, true,  K_A, true);               // the CoCo's other case, as before
    expect(H_LEFT, true, K_LEFT, true);          // Shift+Left erases a line
    expect(H_UP, false, K_UP, false);
    expect(H_ENTER, false, K_ENTER, false);
    expect(H_SPACE, true, K_SPACE, true);
    expect(H_ESC, false, K_BREAK, false);
    expect(H_BKSP, false, K_LEFT, false);
    expect(H_HOME, false, K_CLEAR, false);
}

static void test_caps_lock_is_the_case_toggle(void) {
    expect(H_CAPS, false, K_0, true);
}

static void test_the_keypad_types_its_characters(void) {
    expect(H_KP_7, false, K_7, false);
    expect(H_KP_STAR, false, K_COLON, true);     // *
    expect(H_KP_DOT, false, K_DOT, false);
    expect(0x58, false, K_ENTER, false);         // keypad Enter
}

static void test_shift_alone_is_the_cocos_shift(void) {
    struct kt_state st; kt_init(&st);
    uint8_t none[6] = { 0 }, k[8];
    kt_update(&st, SHIFT_L, none);
    int n = kt_keys(&st, SHIFT_L, k);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(K_SHIFT, k[0]);
}

static void test_a_release_lifts_the_key_chosen_at_the_press(void) {
    // '=' pressed unshifted chooses SHIFT+-; letting go of Shift or pressing
    // it must not turn the held key into '+'.
    struct kt_state st; kt_init(&st);
    uint8_t eq[6] = { H_EQUAL, 0, 0, 0, 0, 0 }, none[6] = { 0 }, k[8];
    kt_update(&st, 0, eq);
    kt_update(&st, SHIFT_L, eq);                 // Shift pressed while '=' held
    int n = kt_keys(&st, SHIFT_L, k);
    TEST_ASSERT_TRUE(has(k, n, K_MINUS));
    TEST_ASSERT_FALSE(has(k, n, K_SEMI));
    kt_update(&st, 0, none);
    TEST_ASSERT_EQUAL_INT(0, kt_keys(&st, 0, k));
}

static void test_the_newest_key_decides_shift(void) {
    // Hold ';' (SHIFT off) then add '"' (SHIFT on): SHIFT goes on.
    struct kt_state st; kt_init(&st);
    uint8_t a[6] = { H_SEMI, 0, 0, 0, 0, 0 }, b[6] = { H_SEMI, H_QUOTE, 0, 0, 0, 0 }, k[8];
    kt_update(&st, 0, a);
    kt_update(&st, SHIFT_L, b);
    int n = kt_keys(&st, SHIFT_L, k);
    TEST_ASSERT_TRUE(has(k, n, K_SHIFT));
    TEST_ASSERT_TRUE(has(k, n, K_2));
}

static void test_resync_makes_held_keys_inert(void) {
    // The ESC that closes the overlay must not reach BASIC as a BREAK.
    struct kt_state st; kt_init(&st);
    uint8_t esc[6] = { H_ESC, 0, 0, 0, 0, 0 }, none[6] = { 0 }, k[8];
    kt_resync(&st, esc);
    kt_update(&st, 0, esc);
    TEST_ASSERT_EQUAL_INT(0, kt_keys(&st, 0, k));
    kt_update(&st, 0, none);                     // released, then pressed again
    kt_update(&st, 0, esc);
    int n = kt_keys(&st, 0, k);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(K_BREAK, k[0]);
}

static void test_serial_chords_cover_the_new_characters(void) {
    uint8_t d, sh;
    TEST_ASSERT_TRUE(kt_chord('[', &d, &sh));  TEST_ASSERT_EQUAL_UINT8(K_DOWN, d);  TEST_ASSERT_EQUAL_UINT8(KT_SHIFT_ON, sh);
    TEST_ASSERT_TRUE(kt_chord('_', &d, &sh));  TEST_ASSERT_EQUAL_UINT8(K_UP, d);    TEST_ASSERT_EQUAL_UINT8(KT_SHIFT_ON, sh);
    TEST_ASSERT_TRUE(kt_chord('A', &d, &sh));  TEST_ASSERT_EQUAL_UINT8(K_A, d);     TEST_ASSERT_EQUAL_UINT8(KT_SHIFT_OFF, sh);
    TEST_ASSERT_FALSE(kt_chord('~', &d, &sh));
    TEST_ASSERT_FALSE(kt_chord('{', &d, &sh));
}

// --- PIZERO-167: auto-repeat --------------------------------------------------

// Hold one key from frame 0 and record, frame by frame, whether it is down.
static void run_repeat(uint8_t hid, bool on, uint32_t delay, uint32_t period,
                       int frames, bool *down) {
    struct kt_state st; kt_init(&st);
    uint8_t codes[6] = { hid, 0, 0, 0, 0, 0 }, k[8];
    kt_update(&st, 0, codes);
    for (int f = 0; f < frames; f++) {
        kt_repeat(&st, (uint32_t)f, on, delay, period);
        int n = kt_keys(&st, 0, k);
        down[f] = n > 0;
    }
}

static void test_repeat_waits_then_releases_and_presses_again(void) {
    bool d[60];
    run_repeat(H_A, true, 30, 6, 60, d);
    for (int f = 0; f < 30; f++) TEST_ASSERT_TRUE(d[f]);          // held through the delay
    TEST_ASSERT_FALSE(d[30]); TEST_ASSERT_FALSE(d[31]);            // the first gap
    TEST_ASSERT_TRUE(d[32]);
    TEST_ASSERT_FALSE(d[36]); TEST_ASSERT_FALSE(d[37]);            // one period later
    TEST_ASSERT_TRUE(d[38]);
}

static void test_repeat_off_just_holds(void) {
    bool d[90];
    run_repeat(H_A, false, 30, 6, 90, d);
    for (int f = 0; f < 90; f++) TEST_ASSERT_TRUE(d[f]);
}

static void test_break_never_repeats(void) {
    bool d[90];
    run_repeat(H_ESC, true, 30, 6, 90, d);
    for (int f = 0; f < 90; f++) TEST_ASSERT_TRUE(d[f]);
}

static void test_a_new_press_restarts_the_delay(void) {
    struct kt_state st; kt_init(&st);
    uint8_t a[6] = { H_A, 0, 0, 0, 0, 0 }, ab[6] = { H_A, H_A + 1, 0, 0, 0, 0 }, k[8];
    kt_update(&st, 0, a);
    for (uint32_t f = 0; f < 40; f++) kt_repeat(&st, f, true, 30, 6);
    kt_update(&st, 0, ab);                        // B pressed at frame 40: B repeats now
    for (uint32_t f = 40; f < 69; f++) {
        kt_repeat(&st, f, true, 30, 6);
        int n = kt_keys(&st, 0, k);
        TEST_ASSERT_TRUE(has(k, n, K_B));         // no gap until B's own delay is up
        TEST_ASSERT_TRUE(has(k, n, K_A));         // and the older key is left alone
    }
    kt_repeat(&st, 70, true, 30, 6);
    int n = kt_keys(&st, 0, k);
    TEST_ASSERT_FALSE(has(k, n, K_B));
    TEST_ASSERT_TRUE(has(k, n, K_A));
}

static void test_repeat_settings_to_frames(void) {
    TEST_ASSERT_EQUAL_UINT32(30, kt_repeat_delay_frames(500));
    TEST_ASSERT_EQUAL_UINT32(6, kt_repeat_period_frames(10));
    TEST_ASSERT_EQUAL_UINT32(KT_REPEAT_MIN, kt_repeat_period_frames(30));  // clamped
    TEST_ASSERT_EQUAL_UINT32(60, kt_repeat_period_frames(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_keys_the_user_found_unmapped);
    RUN_TEST(test_the_digit_row_types_its_keycaps);
    RUN_TEST(test_brackets_and_backslash_use_basics_chords);
    RUN_TEST(test_the_coco_has_no_braces_bar_tilde_or_backtick);
    RUN_TEST(test_letters_and_control_keys_keep_the_physical_shift);
    RUN_TEST(test_caps_lock_is_the_case_toggle);
    RUN_TEST(test_the_keypad_types_its_characters);
    RUN_TEST(test_shift_alone_is_the_cocos_shift);
    RUN_TEST(test_a_release_lifts_the_key_chosen_at_the_press);
    RUN_TEST(test_the_newest_key_decides_shift);
    RUN_TEST(test_resync_makes_held_keys_inert);
    RUN_TEST(test_serial_chords_cover_the_new_characters);
    RUN_TEST(test_repeat_waits_then_releases_and_presses_again);
    RUN_TEST(test_repeat_off_just_holds);
    RUN_TEST(test_break_never_repeats);
    RUN_TEST(test_a_new_press_restarts_the_delay);
    RUN_TEST(test_repeat_settings_to_frames);
    return UNITY_END();
}

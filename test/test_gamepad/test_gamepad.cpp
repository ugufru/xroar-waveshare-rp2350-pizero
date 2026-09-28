// PIZERO-13: host tests for the USB gamepad decoder and the CoCo joystick
// comparator.
//
//   pio test -e native

#include <unity.h>

#include <cstring>

#include "../../src/gamepad.h"
#include "../../lib/coco_machine/src/joy_compare.h"

void setUp(void) {}
void tearDown(void) {}

// A DS4-identity report at rest: sticks centered, D-pad idle (0x0F on this
// pad, not the stock 0x08), nothing pressed.
static void rest(uint8_t r[10]) {
    const uint8_t z[10] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x0F, 0x00, 0x00, 0x00, 0x00 };
    memcpy(r, z, 10);
}

static void test_identities(void) {
    TEST_ASSERT_EQUAL_INT(PAD_DS4, pad_identify(0x054C, 0x09CC));
    TEST_ASSERT_EQUAL_INT(PAD_SILENT, pad_identify(0x057E, 0x2009));
    TEST_ASSERT_EQUAL_INT(PAD_NONE, pad_identify(0x3434, 0x0430));   // a keyboard
    TEST_ASSERT_EQUAL_INT(PAD_NONE, pad_identify(0x054C, 0x0268));   // not this pad
}

static void test_rest_is_exact_center_and_no_fire(void) {
    uint8_t r[10]; rest(r);
    struct pad_state s;
    TEST_ASSERT_TRUE(pad_decode_ds4(r, 10, &s));
    for (int p = 0; p < 2; p++) {
        TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[p][0]);
        TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[p][1]);
        TEST_ASSERT_FALSE(s.fire[p]);
    }
}

static void test_deadzone_edges(void) {
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, pad_axis(0x80 + PAD_DEADZONE - 1));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, pad_axis(0x80 - PAD_DEADZONE + 1));
    TEST_ASSERT_TRUE(pad_axis(0x80 + PAD_DEADZONE) > PAD_CENTER);
    TEST_ASSERT_TRUE(pad_axis(0x80 - PAD_DEADZONE) < PAD_CENTER);
    TEST_ASSERT_EQUAL_UINT16(0, pad_axis(0x00));
    TEST_ASSERT_EQUAL_UINT16(65535, pad_axis(0xFF));
}

static void test_left_stick_drives_the_right_joystick(void) {
    uint8_t r[10]; rest(r);
    r[1] = 0xFF; r[2] = 0x00;                    // left stick: right and up
    struct pad_state s;
    pad_decode_ds4(r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][0]);   // left joystick untouched
}

static void test_right_stick_drives_the_left_joystick(void) {
    uint8_t r[10]; rest(r);
    r[3] = 0x00; r[4] = 0xFF;
    struct pad_state s;
    pad_decode_ds4(r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[1][0]);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[1][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
}

static void test_dpad_overrides_the_left_stick(void) {
    uint8_t r[10]; rest(r);
    r[1] = 0x00;                                 // stick hard left...
    r[5] = 0x02;                                 // ...but D-pad east
    struct pad_state s;
    pad_decode_ds4(r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][1]);   // east leaves Y alone
    r[5] = 0x07;                                 // north-west: both axes
    pad_decode_ds4(r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
}

static void test_fire_buttons(void) {
    struct pad_state s;
    uint8_t r[10];
    rest(r); r[5] |= 0x20; pad_decode_ds4(r, 10, &s);    // cross
    TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    rest(r); r[5] |= 0x40; pad_decode_ds4(r, 10, &s);    // circle
    TEST_ASSERT_TRUE(s.fire[0]);
    rest(r); r[6] = 0x02; pad_decode_ds4(r, 10, &s);     // R1
    TEST_ASSERT_TRUE(s.fire[0]);
    rest(r); r[5] |= 0x10; pad_decode_ds4(r, 10, &s);    // square
    TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    rest(r); r[6] = 0x01; pad_decode_ds4(r, 10, &s);     // L1
    TEST_ASSERT_TRUE(s.fire[1]);
    rest(r); r[5] |= 0x80; pad_decode_ds4(r, 10, &s);    // triangle: nothing
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_non_input_reports_are_ignored(void) {
    // The Switch identity's handshake reply starts 81 01 00 03; a short
    // report cannot be decoded. Neither may move the sticks.
    struct pad_state s; pad_centered(&s);
    uint8_t reply[10] = { 0x81, 0x01, 0x00, 0x03, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_FALSE(pad_decode_ds4(reply, 10, &s));
    uint8_t r[10]; rest(r); r[1] = 0;
    TEST_ASSERT_FALSE(pad_decode_ds4(r, 6, &s));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
}

static int joystk(uint16_t axis);

// --- GameSir identity: Xbox 360 layout in vendor report 0x10 --------------

static void xrest(uint8_t r[20]) {
    memset(r, 0, 20);
    r[0] = 0x10; r[1] = 0x14;
    r[15] = 0x3F; r[16] = 0x5A; r[17] = 0x7F;    // constant tail, as captured
}
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }

static void test_gamesir_identity_decodes_as_xinput(void) {
    TEST_ASSERT_EQUAL_INT(PAD_XINPUT, pad_identify(0x3537, 0x1093));
}

static void test_xinput_rest_is_center(void) {
    uint8_t r[20]; xrest(r);
    struct pad_state s;
    TEST_ASSERT_TRUE(pad_decode(PAD_XINPUT, r, 64, &s));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][1]);
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_xinput_sticks_and_y_flip(void) {
    // Captured extremes: FF 7F = +32767, 00 80 = -32768. +Y is up on the pad
    // and must become 0 (top) on the CoCo.
    uint8_t r[20]; xrest(r);
    put16(r + 6, -32768); put16(r + 8, 32767);   // left stick: full left, full up
    put16(r + 10, 32767); put16(r + 12, -32768); // right stick: full right, full down
    struct pad_state s;
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[1][0]);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[1][1]);
    TEST_ASSERT_EQUAL_INT(0, joystk(s.axis[0][0]));
    TEST_ASSERT_EQUAL_INT(63, joystk(s.axis[1][1]));
}

static void test_xinput_deadzone(void) {
    uint8_t r[20]; xrest(r);
    put16(r + 6, PAD_X_DEADZONE - 1);
    struct pad_state s;
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    put16(r + 6, PAD_X_DEADZONE);
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_TRUE(s.axis[0][0] > PAD_CENTER);
}

static void test_xinput_dpad_overrides_the_left_stick(void) {
    uint8_t r[20]; xrest(r);
    put16(r + 6, -32768);                        // stick hard left...
    r[2] = 0x08;                                 // ...D-pad right
    struct pad_state s;
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][1]);
    r[2] = 0x01 | 0x04;                          // up + left
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    r[2] = 0x02;                                 // down
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
}

static void test_xinput_fire_buttons(void) {
    struct pad_state s;
    uint8_t r[20];
    const uint8_t right[] = { 0x10, 0x20, 0x02 };   // cross, circle, R1
    const uint8_t left[]  = { 0x40, 0x01 };         // square, L1
    for (uint8_t b : right) {
        xrest(r); r[3] = b; pad_decode(PAD_XINPUT, r, 20, &s);
        TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    }
    for (uint8_t b : left) {
        xrest(r); r[3] = b; pad_decode(PAD_XINPUT, r, 20, &s);
        TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    }
    xrest(r); r[3] = 0x80; r[4] = 0xFF; r[5] = 0xFF;   // triangle, L2, R2
    pad_decode(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_xinput_rejects_other_reports(void) {
    struct pad_state s; pad_centered(&s);
    uint8_t kb[9] = { 0x03, 0, 0, 0x04, 0, 0, 0, 0, 0 };   // its keyboard report ID
    TEST_ASSERT_FALSE(pad_decode(PAD_XINPUT, kb, 9, &s));
    uint8_t r[20]; xrest(r); r[1] = 0x13;
    TEST_ASSERT_FALSE(pad_decode(PAD_XINPUT, r, 20, &s));
    TEST_ASSERT_FALSE(pad_decode(PAD_SILENT, r, 20, &s));
}

// --- GameSir Android mode: standard HID gamepad ---------------------------

static void grest(uint8_t r[10]) {
    const uint8_t z[10] = { 0x01, 0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 };
    memcpy(r, z, 10);
}

static void test_hidgp_identity_and_rest(void) {
    TEST_ASSERT_EQUAL_INT(PAD_HIDGP, pad_identify(0x3537, 0x1094));
    uint8_t r[10]; grest(r);
    struct pad_state s;
    TEST_ASSERT_TRUE(pad_decode(PAD_HIDGP, r, 10, &s));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][1]);
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_hidgp_captured_sweep(void) {
    // Left stick full up then full down, as captured (X drifted to 7F / 86).
    uint8_t r[10]; grest(r);
    struct pad_state s;
    r[4] = 0x7F; r[5] = 0x00; pad_decode(PAD_HIDGP, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    r[4] = 0x86; r[5] = 0xFF; pad_decode(PAD_HIDGP, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
    TEST_ASSERT_TRUE(s.axis[0][0] > PAD_CENTER);          // 0x86 is past the deadzone
    // Buttons as captured: bottom 01, right 02, left 08, top 10, L1 40, R1 80.
    const uint8_t right[] = { 0x01, 0x02, 0x80 };
    const uint8_t left[]  = { 0x08, 0x40 };
    for (uint8_t b : right) {
        grest(r); r[1] = b; pad_decode(PAD_HIDGP, r, 10, &s);
        TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    }
    for (uint8_t b : left) {
        grest(r); r[1] = b; pad_decode(PAD_HIDGP, r, 10, &s);
        TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    }
    grest(r); r[1] = 0x10; pad_decode(PAD_HIDGP, r, 10, &s);   // top: nothing
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_hidgp_hat(void) {
    uint8_t r[10]; grest(r);
    struct pad_state s;
    r[3] = 0x02; pad_decode(PAD_HIDGP, r, 10, &s);            // east
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    r[3] = 0x04; pad_decode(PAD_HIDGP, r, 10, &s);            // south
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
}

// Color BASIC's JOYSTK: a 6-bit successive approximation, writing each trial
// to PIA1 port A bits 2-7 and keeping the bit while PA7 reads high.
static int joystk(uint16_t axis) {
    int v = 0;
    for (int bit = 5; bit >= 0; bit--) {
        int trial = v | (1 << bit);
        if (joy_comparator_high(axis, (uint8_t)(trial << 2))) v = trial;
    }
    return v;
}

static void test_joystk_reads_the_full_range(void) {
    TEST_ASSERT_EQUAL_INT(0, joystk(0));
    TEST_ASSERT_EQUAL_INT(63, joystk(65535));
    TEST_ASSERT_EQUAL_INT(31, joystk(PAD_CENTER));   // center reads 31 or 32
    TEST_ASSERT_EQUAL_INT(0, joystk(pad_axis(0x00)));
    TEST_ASSERT_EQUAL_INT(63, joystk(pad_axis(0xFF)));
}

static void test_joystk_is_monotonic_over_the_stick(void) {
    int prev = -1;
    for (int v = 0; v < 256; v++) {
        int j = joystk(pad_axis((uint8_t)v));
        TEST_ASSERT_TRUE(j >= prev);
        prev = j;
    }
}

static void test_comparator_ignores_the_low_dac_bits(void) {
    // PIA1 PA0 is the cassette input and PA1 the serial out; they must not
    // shift the threshold.
    for (int low = 0; low < 4; low++)
        TEST_ASSERT_EQUAL(joy_comparator_high(0x8000, 0x80),
                          joy_comparator_high(0x8000, (uint8_t)(0x80 | low)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identities);
    RUN_TEST(test_rest_is_exact_center_and_no_fire);
    RUN_TEST(test_deadzone_edges);
    RUN_TEST(test_left_stick_drives_the_right_joystick);
    RUN_TEST(test_right_stick_drives_the_left_joystick);
    RUN_TEST(test_dpad_overrides_the_left_stick);
    RUN_TEST(test_fire_buttons);
    RUN_TEST(test_non_input_reports_are_ignored);
    RUN_TEST(test_joystk_reads_the_full_range);
    RUN_TEST(test_joystk_is_monotonic_over_the_stick);
    RUN_TEST(test_comparator_ignores_the_low_dac_bits);
    RUN_TEST(test_gamesir_identity_decodes_as_xinput);
    RUN_TEST(test_xinput_rest_is_center);
    RUN_TEST(test_xinput_sticks_and_y_flip);
    RUN_TEST(test_xinput_deadzone);
    RUN_TEST(test_xinput_dpad_overrides_the_left_stick);
    RUN_TEST(test_xinput_fire_buttons);
    RUN_TEST(test_xinput_rejects_other_reports);
    RUN_TEST(test_hidgp_identity_and_rest);
    RUN_TEST(test_hidgp_captured_sweep);
    RUN_TEST(test_hidgp_hat);
    return UNITY_END();
}

// PIZERO-13: host tests for the USB gamepad decoder and the CoCo joystick
// comparator.
//
//   pio test -e native

#include <unity.h>

#include <cstring>

#include "../../src/gamepad.h"
#include "../../src/key_translate.h"
#include "../../lib/coco_machine/src/joy_compare.h"

// Decode and resolve with the default button mapping (PIZERO-164), which
// must reproduce what these tests expected before buttons became mappable.
static bool dec(enum pad_kind k, const uint8_t *r, uint16_t len, struct pad_out *o) {
    struct pad_map m;
    pad_map_defaults(&m);
    return pad_read(k, r, len, &m, o);
}

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
    struct pad_out s;
    TEST_ASSERT_TRUE(dec(PAD_DS4, r, 10, &s));
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
    struct pad_out s;
    dec(PAD_DS4, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][0]);   // left joystick untouched
}

static void test_right_stick_drives_the_left_joystick(void) {
    uint8_t r[10]; rest(r);
    r[3] = 0x00; r[4] = 0xFF;
    struct pad_out s;
    dec(PAD_DS4, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[1][0]);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[1][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
}

static void test_dpad_overrides_the_left_stick(void) {
    uint8_t r[10]; rest(r);
    r[1] = 0x00;                                 // stick hard left...
    r[5] = 0x02;                                 // ...but D-pad east
    struct pad_out s;
    dec(PAD_DS4, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][1]);   // east leaves Y alone
    r[5] = 0x07;                                 // north-west: both axes
    dec(PAD_DS4, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
}

static void test_fire_buttons(void) {
    struct pad_out s;
    uint8_t r[10];
    rest(r); r[5] |= 0x20; dec(PAD_DS4, r, 10, &s);    // cross
    TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    rest(r); r[5] |= 0x40; dec(PAD_DS4, r, 10, &s);    // circle
    TEST_ASSERT_TRUE(s.fire[0]);
    rest(r); r[6] = 0x02; dec(PAD_DS4, r, 10, &s);     // R1
    TEST_ASSERT_TRUE(s.fire[0]);
    rest(r); r[5] |= 0x10; dec(PAD_DS4, r, 10, &s);    // square
    TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    rest(r); r[6] = 0x01; dec(PAD_DS4, r, 10, &s);     // L1
    TEST_ASSERT_TRUE(s.fire[1]);
    rest(r); r[5] |= 0x80; dec(PAD_DS4, r, 10, &s);    // triangle: nothing
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_non_input_reports_are_ignored(void) {
    // The Switch identity's handshake reply starts 81 01 00 03; a short
    // report cannot be decoded. Neither may move the sticks.
    struct pad_out s; pad_centered(&s);
    uint8_t reply[10] = { 0x81, 0x01, 0x00, 0x03, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_FALSE(dec(PAD_DS4, reply, 10, &s));
    uint8_t r[10]; rest(r); r[1] = 0;
    TEST_ASSERT_FALSE(dec(PAD_DS4, r, 6, &s));
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
    struct pad_out s;
    TEST_ASSERT_TRUE(dec(PAD_XINPUT, r, 64, &s));
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
    struct pad_out s;
    dec(PAD_XINPUT, r, 20, &s);
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
    struct pad_out s;
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    put16(r + 6, PAD_X_DEADZONE);
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_TRUE(s.axis[0][0] > PAD_CENTER);
}

static void test_xinput_dpad_overrides_the_left_stick(void) {
    uint8_t r[20]; xrest(r);
    put16(r + 6, -32768);                        // stick hard left...
    r[2] = 0x08;                                 // ...D-pad right
    struct pad_out s;
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][1]);
    r[2] = 0x01 | 0x04;                          // up + left
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    r[2] = 0x02;                                 // down
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
}

static void test_xinput_fire_buttons(void) {
    struct pad_out s;
    uint8_t r[20];
    const uint8_t right[] = { 0x10, 0x20, 0x02 };   // cross, circle, R1
    const uint8_t left[]  = { 0x40, 0x01 };         // square, L1
    for (uint8_t b : right) {
        xrest(r); r[3] = b; dec(PAD_XINPUT, r, 20, &s);
        TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    }
    for (uint8_t b : left) {
        xrest(r); r[3] = b; dec(PAD_XINPUT, r, 20, &s);
        TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    }
    xrest(r); r[3] = 0x80; r[4] = 0xFF; r[5] = 0xFF;   // triangle, L2, R2
    dec(PAD_XINPUT, r, 20, &s);
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_xinput_rejects_other_reports(void) {
    struct pad_out s; pad_centered(&s);
    uint8_t kb[9] = { 0x03, 0, 0, 0x04, 0, 0, 0, 0, 0 };   // its keyboard report ID
    TEST_ASSERT_FALSE(dec(PAD_XINPUT, kb, 9, &s));
    uint8_t r[20]; xrest(r); r[1] = 0x13;
    TEST_ASSERT_FALSE(dec(PAD_XINPUT, r, 20, &s));
    TEST_ASSERT_FALSE(dec(PAD_SILENT, r, 20, &s));
}

// --- GameSir Android mode: standard HID gamepad ---------------------------

static void grest(uint8_t r[10]) {
    const uint8_t z[10] = { 0x01, 0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 };
    memcpy(r, z, 10);
}

static void test_hidgp_identity_and_rest(void) {
    TEST_ASSERT_EQUAL_INT(PAD_HIDGP, pad_identify(0x3537, 0x1094));
    uint8_t r[10]; grest(r);
    struct pad_out s;
    TEST_ASSERT_TRUE(dec(PAD_HIDGP, r, 10, &s));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[1][1]);
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_hidgp_captured_sweep(void) {
    // Left stick full up then full down, as captured (X drifted to 7F / 86).
    uint8_t r[10]; grest(r);
    struct pad_out s;
    r[4] = 0x7F; r[5] = 0x00; dec(PAD_HIDGP, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
    TEST_ASSERT_EQUAL_UINT16(0, s.axis[0][1]);
    r[4] = 0x86; r[5] = 0xFF; dec(PAD_HIDGP, r, 10, &s);
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
    TEST_ASSERT_TRUE(s.axis[0][0] > PAD_CENTER);          // 0x86 is past the deadzone
    // Buttons as captured: bottom 01, right 02, left 08, top 10, L1 40, R1 80.
    const uint8_t right[] = { 0x01, 0x02, 0x80 };
    const uint8_t left[]  = { 0x08, 0x40 };
    for (uint8_t b : right) {
        grest(r); r[1] = b; dec(PAD_HIDGP, r, 10, &s);
        TEST_ASSERT_TRUE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
    }
    for (uint8_t b : left) {
        grest(r); r[1] = b; dec(PAD_HIDGP, r, 10, &s);
        TEST_ASSERT_TRUE(s.fire[1]); TEST_ASSERT_FALSE(s.fire[0]);
    }
    grest(r); r[1] = 0x10; dec(PAD_HIDGP, r, 10, &s);   // top: nothing
    TEST_ASSERT_FALSE(s.fire[0]); TEST_ASSERT_FALSE(s.fire[1]);
}

static void test_hidgp_hat(void) {
    uint8_t r[10]; grest(r);
    struct pad_out s;
    r[3] = 0x02; dec(PAD_HIDGP, r, 10, &s);            // east
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][0]);
    r[3] = 0x04; dec(PAD_HIDGP, r, 10, &s);            // south
    TEST_ASSERT_EQUAL_UINT16(65535, s.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, s.axis[0][0]);
}

// --- PIZERO-164: every button, and what they do ---------------------------------

static_assert(PAD_K_UP == K_UP && PAD_K_DOWN == K_DOWN && PAD_K_LEFT == K_LEFT &&
              PAD_K_RIGHT == K_RIGHT && PAD_K_SPACE == K_SPACE && PAD_K_ENTER == K_ENTER,
              "gamepad.h's key numbers must match key_translate.h");

static uint16_t buttons_of(enum pad_kind k, const uint8_t *r, uint16_t len) {
    struct pad_state st;
    TEST_ASSERT_TRUE(pad_decode(k, r, len, &st));
    return st.buttons;
}

static void test_hidgp_every_button_from_the_sweep(void) {
    // Bytes as captured on the GameSir in Android mode (PIZERO-164 sweep).
    struct { uint8_t b1, b2; int button; } cases[] = {
        { 0x01, 0, PAD_B_BOTTOM }, { 0x02, 0, PAD_B_RIGHT }, { 0x08, 0, PAD_B_LEFT },
        { 0x10, 0, PAD_B_TOP },    { 0x40, 0, PAD_B_L1 },    { 0x80, 0, PAD_B_R1 },
        { 0, 0x01, PAD_B_L2 },     { 0, 0x02, PAD_B_R2 },    { 0, 0x04, PAD_B_SELECT },
        { 0, 0x08, PAD_B_START },  { 0, 0x10, PAD_B_HOME },  { 0, 0x20, PAD_B_L3 },
        { 0, 0x40, PAD_B_R3 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t r[10]; grest(r);
        r[1] = cases[i].b1; r[2] = cases[i].b2;
        TEST_ASSERT_EQUAL_HEX16(PAD_BIT(cases[i].button), buttons_of(PAD_HIDGP, r, 10));
    }
}

static void test_xinput_buttons_and_triggers(void) {
    uint8_t r[20];
    xrest(r); r[2] = 0x40; TEST_ASSERT_EQUAL_HEX16(PAD_BIT(PAD_B_L3), buttons_of(PAD_XINPUT, r, 20));
    xrest(r); r[2] = 0x80; TEST_ASSERT_EQUAL_HEX16(PAD_BIT(PAD_B_R3), buttons_of(PAD_XINPUT, r, 20));
    xrest(r); r[3] = 0x80; TEST_ASSERT_EQUAL_HEX16(PAD_BIT(PAD_B_TOP), buttons_of(PAD_XINPUT, r, 20));
    xrest(r); r[4] = 0x7F; TEST_ASSERT_EQUAL_HEX16(0, buttons_of(PAD_XINPUT, r, 20));   // L2 half way
    xrest(r); r[4] = 0xFF; TEST_ASSERT_EQUAL_HEX16(PAD_BIT(PAD_B_L2), buttons_of(PAD_XINPUT, r, 20));
    xrest(r); r[5] = 0xC0; TEST_ASSERT_EQUAL_HEX16(PAD_BIT(PAD_B_R2), buttons_of(PAD_XINPUT, r, 20));
}

static void test_defaults_start_is_enter_and_top_is_space(void) {
    struct pad_out o;
    uint8_t r[10];
    grest(r); r[2] = 0x08; dec(PAD_HIDGP, r, 10, &o);               // Start
    TEST_ASSERT_EQUAL_UINT8(1, o.nkeys); TEST_ASSERT_EQUAL_UINT8(K_ENTER, o.keys[0]);
    grest(r); r[1] = 0x10; dec(PAD_HIDGP, r, 10, &o);               // top
    TEST_ASSERT_EQUAL_UINT8(1, o.nkeys); TEST_ASSERT_EQUAL_UINT8(K_SPACE, o.keys[0]);
    TEST_ASSERT_FALSE(o.fire[0]); TEST_ASSERT_FALSE(o.fire[1]);
    grest(r); r[2] = 0x04 | 0x10 | 0x20; dec(PAD_HIDGP, r, 10, &o); // select, home, L3
    TEST_ASSERT_EQUAL_UINT8(0, o.nkeys);                            // do nothing by default
}

static void test_a_mapping_moves_fire_and_keys(void) {
    struct pad_map m; pad_map_defaults(&m);
    m.act[PAD_B_BOTTOM] = K_S;                 // e.g. a game that starts on S
    m.act[PAD_B_L2] = PAD_ACT_FIRE;
    struct pad_out o;
    uint8_t r[10]; grest(r); r[1] = 0x01; r[2] = 0x01;              // bottom + L2
    TEST_ASSERT_TRUE(pad_read(PAD_HIDGP, r, 10, &m, &o));
    TEST_ASSERT_EQUAL_UINT8(1, o.nkeys); TEST_ASSERT_EQUAL_UINT8(K_S, o.keys[0]);
    TEST_ASSERT_TRUE(o.fire[0]);
}

static void test_dpad_as_arrows_presses_keys_and_leaves_the_stick(void) {
    struct pad_map m; pad_map_defaults(&m); m.dpad_arrows = true;
    struct pad_out o;
    uint8_t r[10]; grest(r); r[3] = 0x07;                            // north-west
    TEST_ASSERT_TRUE(pad_read(PAD_HIDGP, r, 10, &m, &o));
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, o.axis[0][0]);             // stick untouched
    TEST_ASSERT_EQUAL_UINT16(PAD_CENTER, o.axis[0][1]);
    TEST_ASSERT_EQUAL_UINT8(2, o.nkeys);
    TEST_ASSERT_TRUE((o.keys[0] == K_LEFT && o.keys[1] == K_UP) || (o.keys[0] == K_UP && o.keys[1] == K_LEFT));
    grest(r); r[3] = 0x02; pad_read(PAD_HIDGP, r, 10, &m, &o);     // east only
    TEST_ASSERT_EQUAL_UINT8(1, o.nkeys); TEST_ASSERT_EQUAL_UINT8(K_RIGHT, o.keys[0]);
}

static void test_key_names_round_trip(void) {
    char name[8];
    for (int d = 0; d < 0x40; d++) {
        if (!kt_key_name((uint8_t)d, name)) continue;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(d, kt_key_by_name(name), name);
    }
    TEST_ASSERT_EQUAL_UINT8(K_ENTER, kt_key_by_name("enter"));
    TEST_ASSERT_EQUAL_UINT8(K_S, kt_key_by_name("s"));
    TEST_ASSERT_EQUAL_UINT8(K_INVALID, kt_key_by_name("tab"));
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
    RUN_TEST(test_hidgp_every_button_from_the_sweep);
    RUN_TEST(test_xinput_buttons_and_triggers);
    RUN_TEST(test_defaults_start_is_enter_and_top_is_space);
    RUN_TEST(test_a_mapping_moves_fire_and_keys);
    RUN_TEST(test_dpad_as_arrows_presses_keys_and_leaves_the_stick);
    RUN_TEST(test_key_names_round_trip);
    return UNITY_END();
}

// PIZERO-143: host tests for the SN76489 on the Games Master Cartridge.
//
// Pitch, volume and noise are what a player hears, and none of them can be
// judged by ear on a board without a reference, so each is pinned here to
// the chip's documented behaviour.
//
//   pio test -e native

#include <unity.h>

#include "../../lib/coco_machine/src/csg_sn76489.h"

void setUp(void) {}
void tearDown(void) {}

static struct sn76489 c;

// Set tone channel `ch` to divider n and volume att, as a program would.
static void tone(int ch, unsigned n, unsigned att) {
    sn_write(&c, (uint8_t)(0x80 | (ch << 5) | (n & 0x0F)));
    sn_write(&c, (uint8_t)((n >> 4) & 0x3F));
    sn_write(&c, (uint8_t)(0x90 | (ch << 5) | (att & 0x0F)));
}

static void test_starts_silent(void) {
    sn_reset(&c);
    for (int i = 0; i < 10000; i++) TEST_ASSERT_EQUAL_INT32(0, sn_tick(&c));
}

static void test_latch_and_data_bytes_build_a_ten_bit_divider(void) {
    sn_reset(&c);
    sn_write(&c, 0x80 | 0x0E);                   // ch0 tone, low 4 = $E
    sn_write(&c, 0x3F);                          // high 6 = $3F
    TEST_ASSERT_EQUAL_UINT16(0x3FE, c.freq[0]);
    sn_write(&c, 0x80 | 0x01);                   // new low bits keep the high ones
    TEST_ASSERT_EQUAL_UINT16(0x3F1, c.freq[0]);
    sn_write(&c, 0xA0);                          // ch1, N = 0
    TEST_ASSERT_EQUAL_UINT16(0x400, c.freq[1]);  // 0 counts as 1024
}

static void test_volume_bytes(void) {
    sn_reset(&c);
    sn_write(&c, 0x90 | 0x03);                   // ch0 volume 3
    TEST_ASSERT_EQUAL_UINT8(3, c.att[0]);
    sn_write(&c, 0x0A);                          // a data byte replaces all 4 bits
    TEST_ASSERT_EQUAL_UINT8(0x0A, c.att[0]);
    TEST_ASSERT_EQUAL_UINT16(945, SN_LEVEL[0]);
    TEST_ASSERT_EQUAL_UINT16(0, SN_LEVEL[15]);
    for (int i = 1; i < 16; i++) TEST_ASSERT_TRUE(SN_LEVEL[i] < SN_LEVEL[i - 1]);
}

static void test_tone_pitch(void) {
    // N = 125: 125 kHz / 125 = 1 kHz. One second of chip ticks holds 1000
    // rising edges on channel 0.
    sn_reset(&c);
    tone(0, 125, 0);
    int edges = 0, prev = 0;
    for (uint32_t t = 0; t < SN_REF_HZ; t++) {
        sn_tick(&c);
        if (c.state[0] && !prev) edges++;
        prev = c.state[0];
    }
    TEST_ASSERT_INT_WITHIN(1, 1000, edges);
}

static void test_full_volume_square_averages_to_half(void) {
    // A 125 kHz tone (N = 1) is far above the audio rate: averaged over a
    // 48 kHz sample it must come out as a steady half level, not aliasing.
    sn_reset(&c);
    tone(0, 1, 0);
    for (int s = 0; s < 100; s++) {
        int32_t v = sn_render(&c, SN_EVENT_HZ / 48000);
        TEST_ASSERT_INT_WITHIN(120, 945 / 2, v);
    }
}

static void test_periodic_noise_is_a_fifteen_step_pulse(void) {
    sn_reset(&c);
    sn_write(&c, 0xE0 | 0x00);                   // noise: periodic, fastest rate
    sn_write(&c, 0xF0 | 0x00);                   // noise full volume
    // Collect the output bit at each shift and check the period is 15.
    uint8_t bits[64];
    int n = 0;
    uint16_t prev = c.lfsr;
    while (n < 64) {
        sn_tick(&c);
        if (c.lfsr != prev) { bits[n++] = c.state[3]; prev = c.lfsr; }
    }
    int ones = 0;
    for (int i = 0; i < 15; i++) ones += bits[i];
    TEST_ASSERT_EQUAL_INT(1, ones);              // one pulse per 15 steps
    for (int i = 15; i < 64; i++) TEST_ASSERT_EQUAL_UINT8(bits[i - 15], bits[i]);
}

static void test_white_noise_varies_and_resets(void) {
    sn_reset(&c);
    sn_write(&c, 0xE0 | 0x04);                   // white noise
    uint16_t seen = c.lfsr;
    TEST_ASSERT_EQUAL_UINT16(0x4000, seen);      // a noise write resets it
    int changes = 0; uint8_t p = c.state[3];
    for (int i = 0; i < 20000; i++) { sn_tick(&c); if (c.state[3] != p) { changes++; p = c.state[3]; } }
    TEST_ASSERT_TRUE(changes > 100);
    sn_write(&c, 0xE0 | 0x04);
    TEST_ASSERT_EQUAL_UINT16(0x4000, c.lfsr);
}

static void test_noise_rate_3_follows_tone_three(void) {
    sn_reset(&c);
    sn_write(&c, 0xE0 | 0x03);                   // periodic, clocked by tone 3
    tone(2, 50, 15);                             // tone 3 at N = 50, silent
    uint16_t prev = c.lfsr; int shifts = 0;
    for (int i = 0; i < 1000; i++) { sn_tick(&c); if (c.lfsr != prev) { shifts++; prev = c.lfsr; } }
    // tone 3 rises once per 100 ticks: ten steps in 1000 ticks
    TEST_ASSERT_INT_WITHIN(1, 10, shifts);
}

static void test_writes_too_close_together_are_refused(void) {
    TEST_ASSERT_FALSE(sn_ready(1000, 1000 + SN_READY_TICKS - 1));
    TEST_ASSERT_TRUE(sn_ready(1000, 1000 + SN_READY_TICKS));
    TEST_ASSERT_TRUE(sn_ready(0xFFFFFFF0u, 0x100));  // across tick wrap
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_silent);
    RUN_TEST(test_latch_and_data_bytes_build_a_ten_bit_divider);
    RUN_TEST(test_volume_bytes);
    RUN_TEST(test_tone_pitch);
    RUN_TEST(test_full_volume_square_averages_to_half);
    RUN_TEST(test_periodic_noise_is_a_fifteen_step_pulse);
    RUN_TEST(test_white_noise_varies_and_resets);
    RUN_TEST(test_noise_rate_3_follows_tone_three);
    RUN_TEST(test_writes_too_close_together_are_refused);
    return UNITY_END();
}

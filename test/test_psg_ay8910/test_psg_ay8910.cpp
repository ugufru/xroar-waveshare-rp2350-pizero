// PIZERO-149: host tests for the AY-3-8910 and the cartridges that carry it.
//
// As with the SN76489, pitch, level, noise and the envelope cannot be judged
// by ear on the board without a reference, so each is pinned here to the
// data sheet, and each cartridge's wiring to MAME's (and XRoar's) map.
//
//   pio test -e native

#include <stddef.h>
#include <unity.h>

#include "../../lib/coco_machine/src/psg_ay8910.h"

void setUp(void) {}
void tearDown(void) {}

static struct ay8910 c;
static struct ay_bank b;

static void reg(struct ay8910 *p, unsigned r, uint8_t v) { ay_latch(p, (uint8_t)r); ay_write(p, v); }

// Ticks between rising edges of channel ch's tone output.
static unsigned tone_ticks_per_cycle(int ch) {
    unsigned t = 0;
    while (c.tone_out[ch]) { ay_tick(&c); t++; }       // to a low
    while (!c.tone_out[ch]) { ay_tick(&c); t++; }      // to the next rise
    t = 0;
    while (c.tone_out[ch]) { ay_tick(&c); t++; }
    while (!c.tone_out[ch]) { ay_tick(&c); t++; }
    return t;
}

static void test_starts_silent(void) {
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    for (int i = 0; i < 10000; i++) TEST_ASSERT_EQUAL_INT32(0, ay_tick(&c));
}

static void test_tone_frequency_is_clock_over_16_tp(void) {
    // One cycle is 2 * TP chip ticks of clock/8, so f = clock / (16 * TP).
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 0, 0x1C); reg(&c, 1, 0x01);          // TP = $11C = 284: 2 MHz -> 440.1 Hz
    reg(&c, 7, 0x3E);                            // tone A only
    reg(&c, 8, 15);
    TEST_ASSERT_EQUAL_UINT(2 * 284, tone_ticks_per_cycle(0));
    reg(&c, 0, 0);  reg(&c, 1, 0);               // TP 0 counts as 1
    TEST_ASSERT_EQUAL_UINT(2, tone_ticks_per_cycle(0));
    reg(&c, 1, 0xFF);                            // coarse keeps 4 bits
    TEST_ASSERT_EQUAL_UINT8(0x0F, c.reg[1]);
}

static void test_registers_read_back_masked(void) {
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 6, 0xFF);  TEST_ASSERT_EQUAL_UINT8(0x1F, ay_read(&c));
    reg(&c, 8, 0xFF);  TEST_ASSERT_EQUAL_UINT8(0x1F, ay_read(&c));
    reg(&c, 13, 0xFF); TEST_ASSERT_EQUAL_UINT8(0x0F, ay_read(&c));
    reg(&c, 11, 0xA5); TEST_ASSERT_EQUAL_UINT8(0xA5, ay_read(&c));
    ay_latch(&c, 14);  TEST_ASSERT_EQUAL_UINT8(0xFF, ay_read(&c));   // no I/O port wired
}

static void test_level_steps_and_mixer(void) {
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 7, 0x3F);                            // tone and noise off: steadily on
    reg(&c, 8, 15);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], ay_tick(&c));
    reg(&c, 9, 13);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] + AY_LEVEL[13], ay_tick(&c));
    reg(&c, 8, 0); reg(&c, 9, 0);
    TEST_ASSERT_EQUAL_INT32(0, ay_tick(&c));
    // Each step is about 3 dB: 1/sqrt 2, within rounding.
    for (int i = 2; i < 16; i++)
        TEST_ASSERT_INT_WITHIN(2, AY_LEVEL[i] * 707 / 1000, AY_LEVEL[i - 1]);
    // Tone on: half the time on, so the average is half the level.
    reg(&c, 7, 0x3E); reg(&c, 8, 15); reg(&c, 0, 10); reg(&c, 1, 0);
    long sum = 0;
    for (int i = 0; i < 2000; i++) sum += ay_tick(&c);
    TEST_ASSERT_INT_WITHIN(20, AY_LEVEL[15] / 2, (int)(sum / 2000));
}

static void test_noise_is_pseudo_random_and_half_rate(void) {
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 6, 1);                               // NP 1: a new bit every 2 ticks
    reg(&c, 7, 0x37);                            // noise on A only
    reg(&c, 8, 15);
    int ones = 0, changes = 0, prev = -1;
    for (int i = 0; i < 20000; i++) {
        int on = ay_tick(&c) != 0;
        ones += on;
        if (prev >= 0 && on != prev) changes++;
        prev = on;
    }
    TEST_ASSERT_INT_WITHIN(1000, 10000, ones);   // about half the time
    TEST_ASSERT_TRUE(changes > 2000 && changes < 6000);   // never two in a row
    // The 17-bit register's period is 2^17 - 1 steps.
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    uint32_t start = c.lfsr, steps = 0;
    reg(&c, 6, 1);
    do { ay_tick(&c); ay_tick(&c); steps++; } while (c.lfsr != start && steps < 200000);
    TEST_ASSERT_EQUAL_UINT32(131071, steps);
}

// The level after each of n envelope steps for shape s, with EP 1 (a step
// every 2 ticks).
static void env_run(uint8_t s, uint8_t *lv, int n) {
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 11, 1); reg(&c, 12, 0);
    reg(&c, 13, s);
    for (int i = 0; i < n; i++) { lv[i] = c.env_level; ay_tick(&c); ay_tick(&c); }
}

static void test_envelope_shapes(void) {
    uint8_t lv[64];
    // 0-3: down once, then 0.
    env_run(0x00, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(15, lv[0]); TEST_ASSERT_EQUAL_UINT8(0, lv[15]); TEST_ASSERT_EQUAL_UINT8(0, lv[39]);
    // 4-7: up once, then 0.
    env_run(0x04, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(0, lv[0]); TEST_ASSERT_EQUAL_UINT8(15, lv[15]); TEST_ASSERT_EQUAL_UINT8(0, lv[16]);
    TEST_ASSERT_EQUAL_UINT8(0, lv[39]);
    // 8: sawtooth down.
    env_run(0x08, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(0, lv[15]); TEST_ASSERT_EQUAL_UINT8(15, lv[16]); TEST_ASSERT_EQUAL_UINT8(0, lv[31]);
    // 9: down, hold 0.
    env_run(0x09, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(0, lv[16]); TEST_ASSERT_EQUAL_UINT8(0, lv[39]);
    // 10: triangle, starting down.
    env_run(0x0A, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(0, lv[15]); TEST_ASSERT_EQUAL_UINT8(0, lv[16]); TEST_ASSERT_EQUAL_UINT8(15, lv[31]);
    TEST_ASSERT_EQUAL_UINT8(15, lv[32]);
    // 11: down, then hold 15.
    env_run(0x0B, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(0, lv[15]); TEST_ASSERT_EQUAL_UINT8(15, lv[16]); TEST_ASSERT_EQUAL_UINT8(15, lv[39]);
    // 12: sawtooth up.
    env_run(0x0C, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(15, lv[15]); TEST_ASSERT_EQUAL_UINT8(0, lv[16]);
    // 13: up, hold 15.
    env_run(0x0D, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(15, lv[16]); TEST_ASSERT_EQUAL_UINT8(15, lv[39]);
    // 14: triangle, starting up.
    env_run(0x0E, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(15, lv[15]); TEST_ASSERT_EQUAL_UINT8(15, lv[16]); TEST_ASSERT_EQUAL_UINT8(0, lv[31]);
    // 15: up, then hold 0.
    env_run(0x0F, lv, 40);
    TEST_ASSERT_EQUAL_UINT8(15, lv[15]); TEST_ASSERT_EQUAL_UINT8(0, lv[16]); TEST_ASSERT_EQUAL_UINT8(0, lv[39]);
}

static void test_envelope_period_and_channel_use(void) {
    // A step every 2 * EP ticks; a channel with bit 4 takes the envelope.
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 11, 3); reg(&c, 12, 0);
    reg(&c, 7, 0x3F);
    reg(&c, 8, 0x10);
    reg(&c, 13, 0x0D);                           // up, hold
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[0], ay_tick(&c));
    for (int i = 0; i < 5; i++) ay_tick(&c);     // 6 ticks: one step
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[1], ay_tick(&c));
}

static void test_render_keeps_time_across_calls(void) {
    // Over a second of 48 kHz samples the chip runs exactly its ticks: no
    // drift from the remainder kept between calls.
    ay_reset(&c, AY_CLK_SYM12);
    reg(&c, 0, 1); reg(&c, 1, 0);
    unsigned long ticks_before = 0;
    uint32_t done = 0;
    for (int i = 0; i < 48000; i++) {
        uint32_t tk = (uint32_t)(((uint64_t)(i + 1) * AY_EVENT_HZ) / 48000 - done);
        done += tk;
        uint32_t f = c.frac + tk * (AY_CLK_SYM12 >> 3);
        ticks_before += f / AY_EVENT_HZ;
        ay_render(&c, tk);
    }
    TEST_ASSERT_UINT32_WITHIN(1, AY_CLK_SYM12 / 8, ticks_before);
    TEST_ASSERT_TRUE(c.frac < AY_EVENT_HZ);
}

static void test_render_averages_at_the_chip_clock(void) {
    // One second of machine time at 2 MHz is 250,000 chip ticks; a steady
    // level renders as itself.
    ay_reset(&c, AY_CLK_PSG_2MHZ);
    reg(&c, 7, 0x3F); reg(&c, 8, 15);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], ay_render(&c, AY_EVENT_HZ / 48000));
    uint64_t f = 0; uint32_t total = 0;
    for (int i = 0; i < 48000; i++) {
        f += (uint64_t)(AY_EVENT_HZ / 48000) * (AY_CLK_PSG_2MHZ / 8);
        total += (uint32_t)(f / AY_EVENT_HZ); f %= AY_EVENT_HZ;
    }
    TEST_ASSERT_UINT32_WITHIN(250, 250000, total);
}

// - - - the cartridges - - -

static void bank_reset(uint8_t en) {
    memset(&b, 0, sizeof b);
    b.enabled = en;
    ayb_reset(&b);
}

static void test_coco_psg_registers_and_clock(void) {
    bank_reset(AYB_PSG | AYB_SYM12);
    TEST_ASSERT_TRUE(ayb_owns(&b, 0xFF5E));
    TEST_ASSERT_FALSE(ayb_owns(&b, 0xFF38));     // PIA1's mirror, never the bank's (PIZERO-342)
    TEST_ASSERT_FALSE(ayb_owns(&b, 0xFF39));
    TEST_ASSERT_FALSE(ayb_owns(&b, 0xFF5C));
    TEST_ASSERT_FALSE(ayb_owns(&b, 0xFF64));
    ayb_write(&b, 0xFF5E, 8); ayb_write(&b, 0xFF5F, 0x0C);
    TEST_ASSERT_EQUAL_UINT8(0x0C, b.chip[0].reg[8]);
    TEST_ASSERT_EQUAL_UINT8(0x0C, ayb_read(&b, 0xFF5F));
    TEST_ASSERT_EQUAL_UINT8(1, b.active);
    TEST_ASSERT_EQUAL_UINT32(AY_CLK_PSG_2MHZ, b.chip[0].clock_hz);
    ayb_write(&b, 0xFF5D, 1);
    TEST_ASSERT_EQUAL_UINT32(AY_CLK_PSG_1MHZ, b.chip[0].clock_hz);
    bank_reset(0);
    TEST_ASSERT_FALSE(ayb_owns(&b, 0xFF5F));     // nothing enabled
}

// Write chip k's register r through the Symphony-12's PIA, the way its
// driver does: register on port A, latch, idle, value, write, idle.
static void sym12_reg(int k, uint8_t r, uint8_t v) {
    ayb_write(&b, 0xFF60, r);
    ayb_write(&b, 0xFF62, (uint8_t)(3 << (2 * k)));
    ayb_write(&b, 0xFF62, 0);
    ayb_write(&b, 0xFF60, v);
    ayb_write(&b, 0xFF62, (uint8_t)(2 << (2 * k)));
    ayb_write(&b, 0xFF62, 0);
}

static void test_symphony12_drives_four_chips_through_its_pia(void) {
    bank_reset(AYB_SYM12);
    // Ports as outputs: DDR access with CR bit 2 clear, then data.
    ayb_write(&b, 0xFF61, 0); ayb_write(&b, 0xFF60, 0xFF); ayb_write(&b, 0xFF61, 4);
    ayb_write(&b, 0xFF63, 0); ayb_write(&b, 0xFF62, 0xFF); ayb_write(&b, 0xFF63, 4);
    for (int k = 0; k < 4; k++) sym12_reg(k, 8, (uint8_t)(10 + k));
    for (int k = 0; k < 4; k++) TEST_ASSERT_EQUAL_UINT8(10 + k, b.chip[k].reg[8]);
    TEST_ASSERT_EQUAL_UINT8(0x0F, b.active);
    TEST_ASSERT_EQUAL_UINT32(AY_CLK_SYM12, b.chip[3].clock_hz);
    // Read chip 2's latched register: port A as input, chip 2's bits 01.
    ayb_write(&b, 0xFF61, 0); ayb_write(&b, 0xFF60, 0x00); ayb_write(&b, 0xFF61, 4);
    ayb_write(&b, 0xFF62, (uint8_t)(1 << 4));
    TEST_ASSERT_EQUAL_UINT8(12, ayb_read(&b, 0xFF60));
}

static void test_symphony12_ignores_a_port_left_as_input(void) {
    bank_reset(AYB_SYM12);
    ayb_write(&b, 0xFF61, 4); ayb_write(&b, 0xFF63, 4);   // DDRs still 0
    ayb_write(&b, 0xFF60, 8);
    ayb_write(&b, 0xFF62, 3);
    ayb_write(&b, 0xFF62, 2);
    TEST_ASSERT_EQUAL_UINT8(0, b.active);
}

static void test_bank_mix_is_silent_until_used_and_scales(void) {
    bank_reset(AYB_PSG | AYB_SYM12);
    TEST_ASSERT_EQUAL_INT32(0, ayb_render(&b, 298));
    TEST_ASSERT_EQUAL_UINT8(0, b.active);
    // One chip at a steady full level on A: that level.
    ayb_write(&b, 0xFF5E, 7); ayb_write(&b, 0xFF5F, 0x3F);
    ayb_write(&b, 0xFF5E, 8); ayb_write(&b, 0xFF5F, 15);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], ayb_render(&b, 298));
    // Four alike: 1.6 times one.
    ayb_write(&b, 0xFF61, 0); ayb_write(&b, 0xFF60, 0xFF); ayb_write(&b, 0xFF61, 4);
    ayb_write(&b, 0xFF63, 0); ayb_write(&b, 0xFF62, 0xFF); ayb_write(&b, 0xFF63, 4);
    for (int k = 0; k < 4; k++) { sym12_reg(k, 7, 0x3F); sym12_reg(k, 8, 15); }
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 4 * 2 / 5, ayb_render(&b, 298));
    // A chip turned down to 0 is skipped: three left, scaled for three.
    sym12_reg(3, 8, 0);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 3 * 2 / 4, ayb_render(&b, 298));
    // A reset silences them and keeps what is enabled.
    ayb_reset(&b);
    TEST_ASSERT_EQUAL_UINT8(0, b.active);
    TEST_ASSERT_EQUAL_UINT8(AYB_PSG | AYB_SYM12, b.enabled);
}

// PIZERO-250: with the jumper on, Symphony-12 chips 1-2 play left and 3-4
// right, each side mixed by its own chips; a chip the CoCo PSG wrote last
// is in the centre. With it off both sides are the mono mix.
static void test_symphony12_stereo_pans_its_chips(void) {
    int32_t l, r;
    bank_reset(AYB_PSG | AYB_SYM12);
    b.stereo = 1;
    ayb_write(&b, 0xFF61, 0); ayb_write(&b, 0xFF60, 0xFF); ayb_write(&b, 0xFF61, 4);
    ayb_write(&b, 0xFF63, 0); ayb_write(&b, 0xFF62, 0xFF); ayb_write(&b, 0xFF63, 4);
    sym12_reg(0, 7, 0x3F); sym12_reg(0, 8, 15);
    ayb_render_lr(&b, 298, &l, &r);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], l);
    TEST_ASSERT_EQUAL_INT32(0, r);
    for (int k = 1; k < 4; k++) { sym12_reg(k, 7, 0x3F); sym12_reg(k, 8, 15); }
    ayb_render_lr(&b, 298, &l, &r);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 2 * 2 / 3, l);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 2 * 2 / 3, r);
    sym12_reg(0, 8, 0); sym12_reg(1, 8, 0); sym12_reg(2, 8, 0);
    ayb_render_lr(&b, 298, &l, &r);
    TEST_ASSERT_EQUAL_INT32(0, l);                   // only chip 4, on the right
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], r);
    // The CoCo PSG takes chip 1 back to the centre.
    ayb_write(&b, 0xFF5E, 8); ayb_write(&b, 0xFF5F, 15);
    ayb_render_lr(&b, 298, &l, &r);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15], l);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 2 * 2 / 3, r);
    // Jumper off: mono, and a reset keeps the jumper.
    b.stereo = 0;
    for (int k = 0; k < 4; k++) sym12_reg(k, 8, 15);
    ayb_render_lr(&b, 298, &l, &r);
    TEST_ASSERT_EQUAL_INT32(AY_LEVEL[15] * 4 * 2 / 5, l);
    TEST_ASSERT_EQUAL_INT32(l, r);
    b.stereo = 1;
    ayb_reset(&b);
    TEST_ASSERT_EQUAL_UINT8(1, b.stereo);
    TEST_ASSERT_EQUAL_UINT8(0, b.sym12);
}

// PIZERO-248: ay_render runs only the parts that can be heard, and skips
// the ticks in which none of them changes. It must give exactly what
// stepping every part every tick gives, sample for sample, and once caught
// up leave the chip in exactly the same state, whatever the registers do:
// random writes (tone periods short and long, noise and envelope in use or
// not, every envelope shape) between samples of random length.
static int32_t render_by_ticks(struct ay8910 *c, uint32_t event_ticks) {
    uint32_t f = c->frac + event_ticks * (c->clock_hz >> 3);
    uint32_t n = f / AY_EVENT_HZ;
    c->frac = f - n * AY_EVENT_HZ;
    if (n == 0) return c->last;
    int32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += ay_tick(c);
    c->last = sum / (int32_t)n;
    return c->last;
}

static void test_render_skipping_matches_every_tick(void) {
    struct ay8910 a, r;
    uint32_t seed = 12345;
    #define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7FFF)
    for (int run = 0; run < 40; run++) {
        ay_reset(&a, run & 1 ? AY_CLK_PSG_2MHZ : AY_CLK_SYM12);
        r = a;
        for (int s = 0; s < 4000; s++) {
            if (RND() % 8 == 0) {
                uint8_t reg = (uint8_t)(RND() % 14), v = (uint8_t)RND();
                if (reg == 0 || reg == 2 || reg == 4) v = (uint8_t)(RND() % 4 ? v : RND() % 4);
                if (reg == 6 && RND() % 2) v = (uint8_t)(RND() % 3);
                if (reg == 11 || reg == 12) v = (uint8_t)(RND() % 3 ? RND() % 8 : v);
                ay_latch(&a, reg); ay_write(&a, v);
                ay_latch(&r, reg); ay_write(&r, v);
            }
            uint32_t t = 200 + RND() % 400;
            TEST_ASSERT_EQUAL_INT32(render_by_ticks(&r, t), ay_render(&a, t));
        }
        ay_sync(&a);                             // the parts no one heard, caught up
        TEST_ASSERT_EQUAL_MEMORY(&r, &a, offsetof(struct ay8910, valid));   // the cache is not state
    }
    #undef RND
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_silent);
    RUN_TEST(test_tone_frequency_is_clock_over_16_tp);
    RUN_TEST(test_registers_read_back_masked);
    RUN_TEST(test_level_steps_and_mixer);
    RUN_TEST(test_noise_is_pseudo_random_and_half_rate);
    RUN_TEST(test_envelope_shapes);
    RUN_TEST(test_envelope_period_and_channel_use);
    RUN_TEST(test_render_averages_at_the_chip_clock);
    RUN_TEST(test_render_keeps_time_across_calls);
    RUN_TEST(test_coco_psg_registers_and_clock);
    RUN_TEST(test_symphony12_drives_four_chips_through_its_pia);
    RUN_TEST(test_symphony12_ignores_a_port_left_as_input);
    RUN_TEST(test_bank_mix_is_silent_until_used_and_scales);
    RUN_TEST(test_symphony12_stereo_pans_its_chips);
    RUN_TEST(test_render_skipping_matches_every_tick);
    return UNITY_END();
}

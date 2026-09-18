// PIZERO-121 / PIZERO-109: host tests for the audio rate servo.
//
// The interesting question is not whether the formula is right, it is whether
// the LOOP behaves over hours: the fault it fixes took 4.9 hours of soak to
// characterise. So most of this file simulates the closed loop with the real
// measured mismatch and checks the ring never reaches a wall.
//
//   pio test -e native

#include <unity.h>

#include "../../lib/coco_machine/src/audio_servo.h"

#define RING 8192u
#define NOMINAL 48000u
#define TARGET (RING / 2)

void setUp(void) {}
void tearDown(void) {}

// --- the control law ------------------------------------------------------

static void test_centred_ring_asks_for_the_nominal_rate(void) {
    TEST_ASSERT_EQUAL_UINT32(NOMINAL, audio_servo_rate(NOMINAL, TARGET, RING));
}

static void test_too_full_slows_down_and_too_empty_speeds_up(void) {
    TEST_ASSERT_TRUE(audio_servo_rate(NOMINAL, TARGET + 1000, RING) < NOMINAL);
    TEST_ASSERT_TRUE(audio_servo_rate(NOMINAL, TARGET - 1000, RING) > NOMINAL);
}

static void test_correction_is_clamped_both_ways(void) {
    // A bug elsewhere must not be able to detune the machine audibly.
    TEST_ASSERT_EQUAL_UINT32(NOMINAL - AUDIO_SERVO_MAX_ADJ,
                             audio_servo_rate(NOMINAL, RING, RING));
    TEST_ASSERT_EQUAL_UINT32(NOMINAL + AUDIO_SERVO_MAX_ADJ,
                             audio_servo_rate(NOMINAL, 0, RING));
}

static void test_the_clamp_is_inaudible(void) {
    // 64 of 48,000 is 0.13%, about 0.023 of a semitone. If this ever grows,
    // someone should have to notice.
    TEST_ASSERT_TRUE(AUDIO_SERVO_MAX_ADJ * 1000u / NOMINAL <= 2u);   // <= 0.2%
}

// --- the loop, which is the part that matters -----------------------------
//
// Model: the consumer takes NOMINAL samples per real second. The producer is
// paced by emulated time, which runs at (1 + e) times real time, so asking it
// for `rate` yields rate * (1 + e). The servo sees only the ring.

struct sim {
    double fill;
    uint32_t rate;
    double min_fill, max_fill;
};

static void run_sim(struct sim *s, double e, int seconds) {
    const int fps = 60;
    for (int i = 0; i < seconds * fps; i++) {
        s->fill += ((double)s->rate * (1.0 + e) - (double)NOMINAL) / fps;
        if (s->fill < 0) s->fill = 0;
        if (s->fill > RING) s->fill = RING;
        if (s->fill < s->min_fill) s->min_fill = s->fill;
        if (s->fill > s->max_fill) s->max_fill = s->fill;
        s->rate = audio_servo_rate(NOMINAL, (uint32_t)s->fill, RING);
    }
}

static void test_the_measured_mismatch_settles_and_never_hits_a_wall(void) {
    // +0.021% is what the soak measured: 48,010 produced against 48,000.
    struct sim s = { (double)TARGET, NOMINAL, (double)TARGET, (double)TARGET };
    run_sim(&s, 0.00021, 6 * 3600);              // six hours
    TEST_ASSERT_TRUE(s.min_fill > 256);          // never near empty
    TEST_ASSERT_TRUE(s.max_fill < RING - 256);   // never near full
    // Settles at an offset, not at centre, and the offset's SIGN is the point:
    // to hold the producer slower, the servo needs a negative correction,
    // which it only gets from a ring sitting ABOVE centre. Steady state is
    // K x the mismatch = 32 x ~10 samples/s = ~320 above target.
    TEST_ASSERT_TRUE(s.fill > (double)TARGET);
    TEST_ASSERT_TRUE(s.fill < (double)TARGET + 600);
}

static void test_it_recovers_from_a_full_ring(void) {
    // The state the soak is in right now: ~7,850 of 8,192.
    struct sim s = { 7850.0, NOMINAL, 7850.0, 7850.0 };
    run_sim(&s, 0.00021, 3600);
    TEST_ASSERT_TRUE(s.fill < 5000);             // pulled back from the wall
    TEST_ASSERT_TRUE(s.max_fill <= 7850.0 + 1);  // and did not overshoot up
}

static void test_it_recovers_from_an_empty_ring(void) {
    struct sim s = { 0.0, NOMINAL, 0.0, 0.0 };
    run_sim(&s, 0.00021, 3600);
    TEST_ASSERT_TRUE(s.fill > 2000);
}

static void test_it_handles_the_opposite_sign_too(void) {
    // Before PIZERO-119 the error ran the other way. A servo that only works
    // in one direction would be worse than none.
    struct sim s = { (double)TARGET, NOMINAL, (double)TARGET, (double)TARGET };
    run_sim(&s, -0.00021, 6 * 3600);
    TEST_ASSERT_TRUE(s.min_fill > 256);
    TEST_ASSERT_TRUE(s.max_fill < RING - 256);
    // Mirror image: a slow producer needs a positive correction, so the ring
    // parks BELOW centre by the same ~320.
    TEST_ASSERT_TRUE(s.fill < (double)TARGET);
    TEST_ASSERT_TRUE(s.fill > (double)TARGET - 600);
}

static void test_it_does_not_oscillate(void) {
    // Proportional-on-fill is integral-on-rate, so it should approach and
    // stay, not hunt. Check the second half of a long run is quiet.
    struct sim s = { (double)TARGET, NOMINAL, (double)TARGET, (double)TARGET };
    run_sim(&s, 0.00021, 2 * 3600);              // settle
    double settled = s.fill;
    s.min_fill = s.max_fill = s.fill;
    run_sim(&s, 0.00021, 2 * 3600);              // observe
    TEST_ASSERT_TRUE((s.max_fill - s.min_fill) < 64);        // no hunting
    TEST_ASSERT_TRUE(s.fill > settled - 64 && s.fill < settled + 64);
}

static void test_a_mismatch_beyond_the_clamp_saturates_gracefully(void) {
    // 0.9% was the PIZERO-119 defect, far beyond what 0.13% of correction can
    // absorb. The servo cannot fix that and must not pretend to: it should
    // saturate, and the ring drains, which is a visible, diagnosable state
    // rather than a hidden one.
    struct sim s = { (double)TARGET, NOMINAL, (double)TARGET, (double)TARGET };
    run_sim(&s, -0.009, 600);
    TEST_ASSERT_EQUAL_UINT32(NOMINAL + AUDIO_SERVO_MAX_ADJ, s.rate);   // pinned
    TEST_ASSERT_TRUE(s.fill < 100);                                    // drained
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_centred_ring_asks_for_the_nominal_rate);
    RUN_TEST(test_too_full_slows_down_and_too_empty_speeds_up);
    RUN_TEST(test_correction_is_clamped_both_ways);
    RUN_TEST(test_the_clamp_is_inaudible);
    RUN_TEST(test_the_measured_mismatch_settles_and_never_hits_a_wall);
    RUN_TEST(test_it_recovers_from_a_full_ring);
    RUN_TEST(test_it_recovers_from_an_empty_ring);
    RUN_TEST(test_it_handles_the_opposite_sign_too);
    RUN_TEST(test_it_does_not_oscillate);
    RUN_TEST(test_a_mismatch_beyond_the_clamp_saturates_gracefully);
    return UNITY_END();
}

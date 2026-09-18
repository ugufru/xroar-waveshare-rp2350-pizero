// audio_servo.h — hold the audio ring near half full (PIZERO-121).
//
// THE PROBLEM. The producer is paced by emulated CoCo time; the consumer is
// paced by the real pixel clock. They are independent oscillators and they do
// not agree: measured over 4.9 hours of soak, the producer ran 48,010
// samples/s against the 48,000 the stream asks for, +0.02%. A fixed offset in
// either direction eventually fills or empties ANY buffer, so a bigger ring
// buys time and fixes nothing. Before PIZERO-119 the error had the other sign
// and starved the stream instead (~320 repeats a second); now it overruns and
// the reader discards ~8.7 samples a second.
//
// THE FIX. Nudge the producer's sample rate by a few parts per million to hold
// the ring near half full. This is what audio clock recovery does everywhere:
// the buffer level IS the phase error between two clocks, so correcting on it
// drives the rate error to zero rather than merely reacting to it.
//
// Proportional on fill is deliberately enough. Fill is the integral of the
// rate error, so proportional control here is integral control on rate, which
// gives zero steady-state RATE error: the ring settles at a constant offset
// from centre and stops drifting, which is exactly what we want. No integral
// term, no wind-up, nothing to tune in the field.
//
// Header-only and machine-free so the loop can be simulated on the host
// (PIZERO-109) rather than watched for hours on hardware.

#ifndef AUDIO_SERVO_H
#define AUDIO_SERVO_H

#include <stdint.h>

// Divisor: samples of fill error per 1 sample/s of rate correction. Sets both
// the time constant (~K seconds) and the steady-state offset from centre
// (K x the clock mismatch in samples/s). K=32 gives ~32 s to settle and, for
// the measured 10 samples/s mismatch, parks the ring ~320 samples off centre,
// which is 4% of the ring: far from either wall.
#ifndef AUDIO_SERVO_K
#define AUDIO_SERVO_K 32
#endif

// Hard limit on the correction, in samples/s. 64 of 48,000 is 0.13%, about
// 0.023 of a semitone: inaudible, and far more than the 0.02% we need. The
// clamp exists so a bug elsewhere cannot detune the machine.
#ifndef AUDIO_SERVO_MAX_ADJ
#define AUDIO_SERVO_MAX_ADJ 64
#endif

// One servo step. `fill` is the ring's current occupancy, `ring` its capacity.
// Returns the sample rate the producer should use now.
static inline uint32_t audio_servo_rate(uint32_t nominal, uint32_t fill, uint32_t ring) {
    int32_t target = (int32_t)(ring / 2);
    int32_t err = (int32_t)fill - target;          // >0 = too full, produce slower
    int32_t adj = -err / AUDIO_SERVO_K;
    if (adj > AUDIO_SERVO_MAX_ADJ) adj = AUDIO_SERVO_MAX_ADJ;
    if (adj < -AUDIO_SERVO_MAX_ADJ) adj = -AUDIO_SERVO_MAX_ADJ;
    int32_t rate = (int32_t)nominal + adj;
    return (uint32_t)(rate < 1 ? 1 : rate);
}

#endif  // AUDIO_SERVO_H

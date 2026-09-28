// joy_compare.h: the CoCo joystick comparator (PIZERO-13). Pure, host-tested
// in test/test_gamepad.
//
// There is no ADC on the CoCo. The ROM writes a trial value to the 6-bit DAC
// (PIA1 port A bits 2-7), picks a port with PIA0 CB2 and an axis with CA2, and
// reads PIA0 PA7: high when the stick is at or past the trial value. JOYSTK
// binary-searches that bit. As upstream XRoar (and the Fruit Jam port), the
// threshold is ((dac & 0xFC) | 2) << 8 against a 16-bit axis, so the ROM's own
// search extracts the 6 bits it wants.

#ifndef JOY_COMPARE_H
#define JOY_COMPARE_H

#include <stdint.h>
#include <stdbool.h>

static inline bool joy_comparator_high(uint16_t axis, uint8_t pia1_a) {
    return (unsigned)axis >= (unsigned)(((pia1_a & 0xFC) | 2) << 8);
}

#endif  // JOY_COMPARE_H

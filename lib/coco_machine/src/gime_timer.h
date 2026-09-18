// gime_timer.h — CoCo 3 / GIME programmable timer and a minimal interrupt
// controller, in the real register layout (PIZERO-62).
//
// Why the authentic layout rather than our own: the point is that SOME
// unmodified CoCo 3 software runs on this CoCo 2 base. A device of our own
// invention would run none. $FF90-$FF95 is unused on a CoCo 2, so decoding it
// costs the base machine nothing.
//
// Header-only and free of machine state so the arithmetic can be tested on the
// host (PIZERO-109); coco_machine.cpp owns the instance and the wiring to the
// 6809's IRQ/FIRQ lines and the event queue.
//
// WHAT THIS DOES NOT DO: no MMU, no GIME video, no 512 KB. See
// docs/coco3-feasibility.md for why those are out of reach on this board. The
// sweet spot here is interrupt-driven music and timing code that installs its
// own handler and stays in VDG-compatible video.

#ifndef GIME_TIMER_H
#define GIME_TIMER_H

#include <stdbool.h>
#include <stdint.h>

// Registers, GIME names.
#define GIME_REG_INIT0    0xFF90
#define GIME_REG_INIT1    0xFF91
#define GIME_REG_IRQENR   0xFF92
#define GIME_REG_FIRQENR  0xFF93
#define GIME_REG_TMRH     0xFF94   // timer reload, bits 11-8
#define GIME_REG_TMRL     0xFF95   // timer reload, bits 7-0

// Interrupt source bits in IRQENR/FIRQENR. Only the timer is implemented; the
// others are decoded so a guest's read-modify-write of the enable registers
// does not lose bits, but nothing ever raises them.
#define GIME_INT_TMR      0x20     // bit 5, timer
#define GIME_INT_HBORD    0x10
#define GIME_INT_VBORD    0x08
#define GIME_INT_EI2      0x04
#define GIME_INT_EI1      0x02
#define GIME_INT_EI0      0x01

// INIT1 bit 5 selects the timer's clock source.
#define GIME_INIT1_TINS   0x20

// Event ticks per timer tick. The machine's event clock is 14,318,180 Hz
// (16 ticks per 0.895 MHz CPU cycle), and both GIME clocks divide it exactly:
//   fast:       3.579545 MHz  -> 14318180 / 3579545   = 4
//   horizontal: 15.734 kHz    -> 14318180 / 15734.264 = 910
#define GIME_TICKS_FAST   4u
#define GIME_TICKS_SLOW   910u

// Period bias. The 1986 GIME reloads at count+1, the 1987 part at count+2.
// UNVERIFIED against silicon by us; it matters for the tempo of music written
// against one revision, so it is a build option rather than a silent choice.
#ifndef GIME_PERIOD_BIAS
#define GIME_PERIOD_BIAS  2
#endif

// A pathologically small reload on the fast clock would fire every few CPU
// cycles and starve core 0 (PIZERO-119 left ~4 ms of headroom, not 16). Clamp
// the interval and count how often we had to: a guest that trips this is
// running differently from real hardware and we want to know rather than
// quietly lose frames.
#ifndef GIME_MIN_INTERVAL_TICKS
#define GIME_MIN_INTERVAL_TICKS 256u    // 16 CPU cycles
#endif

typedef struct {
    uint16_t reload;        // 12-bit reload value
    uint8_t  init0;
    uint8_t  init1;
    uint8_t  irq_enable;    // $FF92
    uint8_t  firq_enable;   // $FF93
    uint8_t  irq_status;    // latched, cleared by reading $FF92
    uint8_t  firq_status;   // latched, cleared by reading $FF93
    uint32_t clamped;       // times the interval hit GIME_MIN_INTERVAL_TICKS
} gime_timer_t;

static inline void gime_timer_reset(gime_timer_t *t) {
    t->reload = 0;
    t->init0 = t->init1 = 0;
    t->irq_enable = t->firq_enable = 0;
    t->irq_status = t->firq_status = 0;
    t->clamped = 0;
}

static inline bool gime_timer_owns(uint16_t addr) {
    return addr >= GIME_REG_INIT0 && addr <= GIME_REG_TMRL;
}

// Interval between firings, in event ticks. Zero means the timer is stopped: a
// reload of 0 disables it on real hardware, and so does having neither IRQ nor
// FIRQ enabled for the timer, which lets us skip queueing the event at all.
static inline uint32_t gime_timer_interval(gime_timer_t *t) {
    if (t->reload == 0) return 0;
    if (!((t->irq_enable | t->firq_enable) & GIME_INT_TMR)) return 0;
    uint32_t per_tick = (t->init1 & GIME_INIT1_TINS) ? GIME_TICKS_FAST : GIME_TICKS_SLOW;
    uint32_t ticks = ((uint32_t)t->reload + GIME_PERIOD_BIAS) * per_tick;
    if (ticks < GIME_MIN_INTERVAL_TICKS) {
        t->clamped++;
        ticks = GIME_MIN_INTERVAL_TICKS;
    }
    return ticks;
}

// Returns true if the write should (re)start the countdown.
static inline bool gime_timer_write(gime_timer_t *t, uint16_t addr, uint8_t val) {
    switch (addr) {
    case GIME_REG_INIT0:
        t->init0 = val;
        return false;
    case GIME_REG_INIT1:
        // Changing the clock source changes the interval, so restart: the
        // alternative is a first period of the wrong length.
        t->init1 = val;
        return true;
    case GIME_REG_IRQENR:
        t->irq_enable = (uint8_t)(val & 0x3F);
        return true;    // enabling the timer source has to start it
    case GIME_REG_FIRQENR:
        t->firq_enable = (uint8_t)(val & 0x3F);
        return true;
    case GIME_REG_TMRH:
        t->reload = (uint16_t)(((uint16_t)(val & 0x0F) << 8) | (t->reload & 0x00FF));
        return true;
    case GIME_REG_TMRL:
        t->reload = (uint16_t)((t->reload & 0x0F00) | val);
        return true;
    default:
        return false;
    }
}

// Reads of the enable registers return the LATCHED status and clear it, which
// is how a GIME interrupt handler acknowledges. Returns false if the address
// is not one this device answers.
static inline bool gime_timer_read(gime_timer_t *t, uint16_t addr, uint8_t *out) {
    switch (addr) {
    case GIME_REG_IRQENR:
        *out = t->irq_status;
        t->irq_status = 0;
        return true;
    case GIME_REG_FIRQENR:
        *out = t->firq_status;
        t->firq_status = 0;
        return true;
    case GIME_REG_INIT0:
    case GIME_REG_INIT1:
    case GIME_REG_TMRH:
    case GIME_REG_TMRL:
        *out = 0;       // write-only on real hardware
        return true;
    default:
        return false;
    }
}

// Latch the timer source into whichever status registers have it enabled.
// Returns the lines to assert: bit 0 = IRQ, bit 1 = FIRQ.
#define GIME_LINE_IRQ   0x01
#define GIME_LINE_FIRQ  0x02

static inline uint8_t gime_timer_fire(gime_timer_t *t) {
    uint8_t lines = 0;
    if (t->irq_enable & GIME_INT_TMR) {
        t->irq_status |= GIME_INT_TMR;
        lines |= GIME_LINE_IRQ;
    }
    if (t->firq_enable & GIME_INT_TMR) {
        t->firq_status |= GIME_INT_TMR;
        lines |= GIME_LINE_FIRQ;
    }
    return lines;
}

// Which lines are currently asserted, for the machine to OR into the 6809's
// inputs. An unacknowledged status keeps its line high, as on real hardware.
static inline uint8_t gime_timer_lines(const gime_timer_t *t) {
    uint8_t lines = 0;
    if (t->irq_status & t->irq_enable) lines |= GIME_LINE_IRQ;
    if (t->firq_status & t->firq_enable) lines |= GIME_LINE_FIRQ;
    return lines;
}

#endif  // GIME_TIMER_H

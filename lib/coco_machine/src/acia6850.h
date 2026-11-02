// acia6850.h: the MIDI Pak's Motorola 6850 ACIA (PIZERO-287).
//
// Pure logic, host-tested (test/test_acia6850): no bus, no USB. The machine
// maps it at $FF6E (control write / status read) and $FF6F (transmit /
// receive data), passes every access the current event tick, and services
// it from an event at the tick acia_next_due gives. The host side moves
// bytes through two small queues: what the CoCo sent (acia_tx_pop) and what
// arrives for it (acia_rx_push).
//
// What the software on the board does, from disassembling it (2026-11-01,
// see the issue): Lyra 2.85 and Lester Hands' CoCo MIDI 3 both write $03
// (master reset) then $15 to the control register: divide by 16, 8 data
// bits, no parity, 1 stop bit, interrupts off. Both send by polling status
// bit 1 (TDRE) and writing $FF6F. CoCo MIDI 3 records with $95 (the same,
// receive interrupt on), its FIRQ handler checking status bits 2 and 5
// ($24) before reading $FF6F. At divide by 16 the pak's clock must be
// 500 kHz for MIDI's 31,250 baud, so that is the clock here.
//
// Timing: a byte takes 10 bit times (start, 8 data, stop). Transmit is
// double-buffered as on the chip: a byte written while the shift register
// is idle starts at once (TDRE stays 1); one written while it is busy waits
// in TDR (TDRE 0) until the shift register is free. Receive delivers at most
// one byte per byte time, so software sees a real MIDI rate, and a byte
// that lands while RDRF is still set is lost and sets OVRN, as on the chip.

#ifndef ACIA6850_H
#define ACIA6850_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ACIA_TICK_RATE 14318180u    // XRoar's event ticks per second
#define ACIA_CLOCK     500000u      // the pak's ACIA clock: /16 = 31,250 baud
#define ACIA_TXQ       64           // bytes the CoCo sent, waiting for USB
#define ACIA_RXQ       128          // bytes from USB, waiting for the CoCo

// Status register bits.
#define ACIA_RDRF 0x01
#define ACIA_TDRE 0x02
#define ACIA_DCD  0x04
#define ACIA_CTS  0x08
#define ACIA_FE   0x10
#define ACIA_OVRN 0x20
#define ACIA_PE   0x40
#define ACIA_IRQ  0x80

struct acia6850 {
    uint8_t  cr;            // control register as last written (not a reset)
    bool     in_reset;      // master reset (CR1-0 = 11), as at power-on
    bool     rdrf, ovrn;
    uint8_t  rdr;
    bool     tdr_full;
    uint8_t  tdr;
    bool     shifting;      // a byte is going out
    uint32_t shift_end;     // tick the byte in the shift register is done
    uint32_t rx_next;       // earliest tick the next received byte may land
    uint32_t tpb;           // ticks per 10-bit byte at the current divide
    uint8_t  txq[ACIA_TXQ];
    uint16_t txh, txt;      // head (next out), tail (next in)
    uint8_t  rxq[ACIA_RXQ];
    uint16_t rxh, rxt;
    uint32_t tx_dropped, rx_dropped;
    bool     changed;       // state moved since the host last looked (acia_take_changed)
};

static inline int32_t acia_delta(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

// Ticks wrap (2^32 of them is 5 minutes at 14.3 MHz) and a signed difference
// only orders times within 2.5 minutes of each other. A real "not before"
// time is never more than one byte time ahead of now, so one further ahead
// (or long past, which looks the same once wrapped) is stale: due now. Found
// on hardware 2026-11-01: rx_next started at 0, and after 150 s of uptime 0
// read as the future, so received bytes never landed.
static inline uint32_t acia_due(uint32_t when, uint32_t now, uint32_t tpb) {
    int32_t ahead = acia_delta(when, now);
    return (ahead > 0 && (uint32_t)ahead <= tpb) ? when : now;
}

static inline uint32_t acia_ticks_per_byte(uint8_t cr) {
    unsigned div = (cr & 3) == 0 ? 1 : (cr & 3) == 1 ? 16 : 64;
    return (uint32_t)((10ull * ACIA_TICK_RATE * div) / ACIA_CLOCK);
}

static inline void acia_init(struct acia6850 *a) {
    memset(a, 0, sizeof *a);
    a->in_reset = true;
    a->tpb = acia_ticks_per_byte(0x15);
}

static inline bool acia_txq_put(struct acia6850 *a, uint8_t b) {
    uint16_t n = (uint16_t)((a->txt + 1) % ACIA_TXQ);
    if (n == a->txh) { a->tx_dropped++; return false; }
    a->txq[a->txt] = b; a->txt = n;
    return true;
}

// A byte the CoCo sent, for the host to forward. Returns false when none.
static inline bool acia_tx_pop(struct acia6850 *a, uint8_t *b) {
    if (a->txh == a->txt) return false;
    *b = a->txq[a->txh]; a->txh = (uint16_t)((a->txh + 1) % ACIA_TXQ);
    return true;
}

// A byte arriving for the CoCo. Dropped while the chip is held in reset (no
// software is listening) or when the queue is full.
static inline bool acia_rx_push(struct acia6850 *a, uint8_t b) {
    uint16_t n = (uint16_t)((a->rxt + 1) % ACIA_RXQ);
    if (a->in_reset || n == a->rxh) { a->rx_dropped++; return false; }
    a->rxq[a->rxt] = b; a->rxt = n;
    a->changed = true;
    return true;
}

static inline bool acia_rx_waiting(const struct acia6850 *a) { return a->rxh != a->rxt; }

// Bring the chip up to `now`: finish bytes going out, start the one waiting
// in TDR, and land at most one received byte.
static inline void acia_service(struct acia6850 *a, uint32_t now) {
    if (a->shifting) a->shift_end = acia_due(a->shift_end, now, a->tpb);
    while (a->shifting && acia_delta(now, a->shift_end) >= 0) {
        if (a->tdr_full) {
            acia_txq_put(a, a->tdr);
            a->tdr_full = false;
            a->shift_end += a->tpb;           // back to back, at the line rate
        } else {
            a->shifting = false;
        }
        a->changed = true;
    }
    if (!a->in_reset && acia_rx_waiting(a) && acia_delta(now, acia_due(a->rx_next, now, a->tpb)) >= 0) {
        uint8_t b = a->rxq[a->rxh]; a->rxh = (uint16_t)((a->rxh + 1) % ACIA_RXQ);
        if (a->rdrf) a->ovrn = true;          // the new byte is lost
        else { a->rdr = b; a->rdrf = true; }
        a->rx_next = now + a->tpb;
        a->changed = true;
    }
}

// When the chip next needs servicing, if ever: *at is set and true returned.
static inline bool acia_next_due(const struct acia6850 *a, uint32_t now, uint32_t *at) {
    bool any = false;
    uint32_t t = 0;
    if (a->shifting) { t = acia_due(a->shift_end, now, a->tpb); any = true; }
    if (!a->in_reset && acia_rx_waiting(a)) {
        uint32_t r = acia_due(a->rx_next, now, a->tpb);
        if (!any || acia_delta(r, t) < 0) t = r;
        any = true;
    }
    *at = t;
    return any;
}

// True once after anything that can move the IRQ line or the next due time,
// so a program polling the status register (Lyra and CoCo MIDI 3 spin on it)
// does not make the host touch its event queue on every read.
static inline bool acia_take_changed(struct acia6850 *a) {
    bool c = a->changed;
    a->changed = false;
    return c;
}

// The chip's IRQ output (active: the line to the CART pin is pulled low).
static inline bool acia_irq(const struct acia6850 *a) {
    if (a->in_reset) return false;
    bool rx = (a->cr & 0x80) && (a->rdrf || a->ovrn);
    bool tx = ((a->cr >> 5) & 3) == 1 && !a->tdr_full;
    return rx || tx;
}

static inline uint8_t acia_status(const struct acia6850 *a) {
    uint8_t s = 0;
    if (a->rdrf) s |= ACIA_RDRF;
    if (!a->in_reset && !a->tdr_full) s |= ACIA_TDRE;
    if (a->ovrn) s |= ACIA_OVRN;
    if (acia_irq(a)) s |= ACIA_IRQ;
    return s;                                 // DCD, CTS, FE, PE: always 0 (tied off on the pak)
}

// reg 0 = $FF6E, reg 1 = $FF6F.
static inline uint8_t acia_read(struct acia6850 *a, unsigned reg, uint32_t now) {
    acia_service(a, now);
    if (!(reg & 1)) return acia_status(a);    // a status read changes nothing itself
    a->rdrf = false;                          // reading the data clears RDRF and OVRN
    a->ovrn = false;
    a->changed = true;
    return a->rdr;
}

static inline void acia_write(struct acia6850 *a, unsigned reg, uint8_t v, uint32_t now) {
    acia_service(a, now);
    a->changed = true;
    if (!(reg & 1)) {
        if ((v & 3) == 3) {                   // master reset
            a->in_reset = true;
            a->rdrf = a->ovrn = a->tdr_full = a->shifting = false;
            a->rxh = a->rxt = 0;
        } else {
            a->in_reset = false;
            a->cr = v;
            a->tpb = acia_ticks_per_byte(v);
        }
        return;
    }
    if (a->in_reset) return;
    if (!a->shifting) {                       // idle: straight into the shift register
        acia_txq_put(a, v);
        a->shifting = true;
        a->shift_end = now + a->tpb;
    } else {                                  // busy: waits in TDR (a second write overwrites it)
        a->tdr = v;
        a->tdr_full = true;
    }
}

#endif  // ACIA6850_H

// pio_usb_encode.h: the TX encoder, four bits at a time (PIZERO-322, this
// project).
//
// The transmitter's program takes one 2-bit symbol per USB bit time, the
// address it jumps to: SE0 0, K 1, COMP 2 (release the bus), J 3. A byte of
// encoded data holds four symbols, the first in its top bits. NRZI: a 0 bit
// toggles the line, a 1 keeps it; after six 1s in a row a 0 is stuffed.
//
// Upstream's pio_usb_ll_encode_tx_data does this one bit at a time with a
// read-modify-write of the output byte per bit, about 100 us for a 196-byte
// isochronous audio packet, every 1 ms, in the frame interrupt on the
// emulator's core. Four bits with no stuffing inside them make exactly four
// symbols, one output byte, which depends only on the line level they start
// at: one table lookup. Stuffing can only fall inside a nibble when the run
// of 1s carried in plus the nibble's own leading 1s reaches six; that nibble
// goes bit by bit, as does the end of packet. The output is the same bytes
// (test/test_usb_encode checks it against upstream's).
//
// Pure, no hardware: pio_usb.c includes it and so does the host test.

#ifndef PIO_USB_ENCODE_H
#define PIO_USB_ENCODE_H

#include <stdint.h>

// Where the tables and the one out-of-line helper go: pio_usb.c puts both in
// RAM, as the frame interrupt must not wait on flash (left alone, the
// compiler sees the tables are never written and moves them to flash).
#ifndef PIO_USB_ENC_FUNC
#define PIO_USB_ENC_FUNC(name) name
#endif
#ifndef PIO_USB_ENC_DATA
#define PIO_USB_ENC_DATA
#endif

enum { PIO_USB_ENC_SE0 = 0, PIO_USB_ENC_K = 1, PIO_USB_ENC_COMP = 2, PIO_USB_ENC_J = 3 };

// A nibble's four symbols starting from line level 1 (K). From level 0 every
// level is the other one, so every symbol flips J <-> K: XOR 0xAA.
static const uint8_t PIO_USB_ENC_DATA pio_usb_enc_sym[16] = {
    0xdd, 0x77, 0xf7, 0x5d, 0xd7, 0x7d, 0xfd, 0x57,
    0xdf, 0x75, 0xf5, 0x5f, 0xd5, 0x7f, 0xff, 0x55,
};
// Its 1s at the low end (sent first), and at the high end (sent last).
static const uint8_t PIO_USB_ENC_DATA pio_usb_enc_lead1[16] = {
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
};
static const uint8_t PIO_USB_ENC_DATA pio_usb_enc_trail1[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 4,
};
// 1 when it has an odd number of 0s, which each toggle the line. A table,
// not __builtin_parity: that is a libgcc call, in flash.
static const uint8_t PIO_USB_ENC_DATA pio_usb_enc_flip[16] = {
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
};

struct pio_usb_enc {
    uint8_t *out;
    uint32_t acc;     // symbols not yet written, the newest in the low bits
    unsigned na;      // how many (0..3)
    unsigned level;   // the line: 1 K, 0 J
    unsigned run;     // 1s in a row so far (0..5)
};

static inline void pio_usb_enc_sym1(struct pio_usb_enc *e, unsigned s) {
    e->acc = (e->acc << 2) | s;
    if (++e->na == 4) {
        *e->out++ = (uint8_t)e->acc;
        e->na = 0;
    }
}

static inline void pio_usb_enc_bit(struct pio_usb_enc *e, unsigned bit) {
    if (!bit) {
        e->level ^= 1;
        e->run = 0;
    } else {
        e->run++;
    }
    pio_usb_enc_sym1(e, e->level ? PIO_USB_ENC_K : PIO_USB_ENC_J);
    if (e->run == 6) {                       // stuff a 0
        e->level ^= 1;
        e->run = 0;
        pio_usb_enc_sym1(e, e->level ? PIO_USB_ENC_K : PIO_USB_ENC_J);
    }
}

// A nibble a stuffed bit may fall inside: bit by bit. Out of line, so the
// rare path is one copy (inlined at every call it cost about 8 KB of RAM).
static void __attribute__((noinline)) PIO_USB_ENC_FUNC(pio_usb_enc_slow)(struct pio_usb_enc *e, unsigned n) {
    for (int b = 0; b < 4; b++) pio_usb_enc_bit(e, (n >> b) & 1);
}

static inline void pio_usb_enc_nibble(struct pio_usb_enc *e, unsigned n) {
    if (e->run + pio_usb_enc_lead1[n] >= 6) {
        pio_usb_enc_slow(e, n);
        return;
    }
    uint8_t sym = pio_usb_enc_sym[n] ^ (e->level ? 0x00 : 0xAA);
    e->acc = (e->acc << 8) | sym;
    *e->out++ = (uint8_t)(e->acc >> (2 * e->na));
    // The level after: one toggle per 0 bit.
    e->level ^= pio_usb_enc_flip[n];
    e->run = (n == 0xF) ? e->run + 4 : pio_usb_enc_trail1[n];
}

// As pio_usb_ll_encode_tx_data: the packet's symbols, then SE0 and COMP (the
// end of packet), then at least one K and on to the end of a byte (a whole
// byte of K when COMP ends one). Returns the bytes written.
static inline uint16_t pio_usb_encode_fast(const uint8_t *buffer, uint16_t buffer_len,
                                           uint8_t *encoded_data) {
    struct pio_usb_enc e = { encoded_data, 0, 0, 1, 0 };
    for (uint16_t i = 0; i < buffer_len; i++) {
        unsigned b = buffer[i];
        pio_usb_enc_nibble(&e, b & 0xF);
        pio_usb_enc_nibble(&e, b >> 4);
    }
    pio_usb_enc_sym1(&e, PIO_USB_ENC_SE0);
    pio_usb_enc_sym1(&e, PIO_USB_ENC_COMP);
    do pio_usb_enc_sym1(&e, PIO_USB_ENC_K);   // at least one, as upstream's do-while
    while (e.na);
    return (uint16_t)(e.out - encoded_data);
}

#endif

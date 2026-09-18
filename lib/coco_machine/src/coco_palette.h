// coco_palette.h — the writable 16-entry palette register file, in the CoCo 3
// GIME layout at $FFB0-$FFBF (PIZERO-55 part 1 / PIZERO-85).
//
// A real MC6847 has no palette registers at all; its colours are fixed in
// silicon. This port already looks every pixel up in a 16-entry table on its
// way to RGB565 (src/coco_boot.cpp), so making that table writable gives a
// CoCo 1/2-class machine CoCo 3-class palette control for no per-pixel cost.
//
// Header-only and machine-free so the conversion can be tested on the host
// (PIZERO-109). One register file, shared: PIZERO-55 owns it, PIZERO-85 binds
// to it. Do not build a second one.
//
// TRANSPARENCY RULE: the table powers on holding the stock VDG colours, so a
// game that never touches $FFB0 looks exactly as it did before this existed.
//
// FORMAT, taken from XRoar rather than from memory (~/github/xroar
// src/coco3.c:611-613, the cross-test target this port is compared against):
// a GIME palette byte is six bits, two per channel, interleaved
//   bit5 R1, bit4 G1, bit3 B1, bit2 R0, bit1 G0, bit0 B0
// and the four intensity levels are NOT linear (src/coco3.c:69):
//   0.000, 0.460, 0.736, 0.920
// Using a linear 0/85/170/255 ramp instead would make every mid-tone wrong
// against both real hardware and desktop XRoar.

#ifndef COCO_PALETTE_H
#define COCO_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#define COCO_PAL_BASE   0xFFB0u
#define COCO_PAL_LAST   0xFFBFu
#define COCO_PAL_COUNT  16

// XRoar's rgb_intensity_map, scaled to 8 bits: 0.000, 0.460, 0.736, 0.920.
static const uint8_t coco_pal_intensity[4] = { 0, 117, 188, 235 };

static inline uint16_t coco_pal_rgb565(uint8_t r8, uint8_t g8, uint8_t b8) {
    return (uint16_t)(((uint16_t)(r8 >> 3) << 11) |
                      ((uint16_t)(g8 >> 2) << 5) |
                      (uint16_t)(b8 >> 3));
}

// One GIME palette byte (6 bits) to RGB565.
static inline uint16_t coco_pal_gime_to_rgb565(uint8_t v) {
    uint8_t r = coco_pal_intensity[((v >> 4) & 2) | ((v >> 2) & 1)];
    uint8_t g = coco_pal_intensity[((v >> 3) & 2) | ((v >> 1) & 1)];
    uint8_t b = coco_pal_intensity[((v >> 2) & 2) | ((v >> 0) & 1)];
    return coco_pal_rgb565(r, g, b);
}

typedef struct {
    uint16_t rgb565[COCO_PAL_COUNT];    // what the blit reads, every frame
    uint8_t  gime[COCO_PAL_COUNT];      // last value written to each register
    bool     written;                   // has a guest touched the registers?
} coco_palette_t;

static inline bool coco_pal_owns(uint16_t addr) {
    return addr >= COCO_PAL_BASE && addr <= COCO_PAL_LAST;
}

// Load the factory table (the stock VDG colours) and forget any guest writes.
static inline void coco_pal_reset(coco_palette_t *p, const uint16_t *factory) {
    for (int i = 0; i < COCO_PAL_COUNT; i++) {
        p->rgb565[i] = factory[i];
        p->gime[i] = 0;
    }
    p->written = false;
}

// A guest write to $FFB0-$FFBF. Only the low six bits exist on real hardware,
// and XRoar masks them the same way (tcc1014.c:811).
static inline void coco_pal_write(coco_palette_t *p, uint16_t addr, uint8_t val) {
    uint8_t idx = (uint8_t)(addr & 0x0F);
    uint8_t six = (uint8_t)(val & 0x3F);
    p->gime[idx] = six;
    p->rgb565[idx] = coco_pal_gime_to_rgb565(six);
    p->written = true;
}

// Reads return the register, with the top two bits undriven as on the GIME.
static inline uint8_t coco_pal_read(const coco_palette_t *p, uint16_t addr) {
    return (uint8_t)(0xC0 | p->gime[addr & 0x0F]);
}

// Direct RGB565 access, for the BIOS editor and .pal profiles later
// (PIZERO-55 parts 2 and 3), which are not limited to the GIME's 64 colours.
static inline void coco_pal_set_rgb565(coco_palette_t *p, uint8_t idx, uint16_t rgb) {
    p->rgb565[idx & 0x0F] = rgb;
    p->written = true;
}

static inline uint16_t coco_pal_get_rgb565(const coco_palette_t *p, uint8_t idx) {
    return p->rgb565[idx & 0x0F];
}

#endif  // COCO_PALETTE_H

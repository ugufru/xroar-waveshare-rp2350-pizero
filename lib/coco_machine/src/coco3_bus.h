// coco3_bus.h: the CoCo 3's side of the GIME (PIZERO-133), host-tested.
//
// On a CoCo 3 the GIME is the bus: the 6809's every cycle goes to
// tcc1014_mem_cycle (lib/xroar_core/src/tcc1014), which decodes the address
// through its MMU and calls the machine back with the result in S, RAS and Z.
// This header is that call-back's portable half, the parts both the firmware
// (coco_machine.cpp) and the host boot test (test/test_coco3_boot) need:
//
//   * the 128 KB of RAM, as two 64 KB halves (so neither has to be one
//     allocation), and the 32 KB Super Extended Color BASIC ROM
//   * moving the byte for a RAM, ROM or cartridge-ROM cycle; the PIAs and
//     the cartridge I/O are left to the caller, which has its own side
//     effects for them (interrupt lines, the DAC, the disk controller)
//   * the GIME's video fetch
//   * turning the GIME's scanlines into the 320x240 RGB565 framebuffer
//
// Upstream's version of all this is src/coco3.c (read_byte, write_byte,
// fetch_vram, create_ram), which is desktop glue and is not vendored.
//
// MEMORY: a 128 KB machine sees physical addresses modulo 128 KB, as
// upstream does with RAM_ORG(17, 9, 0): the GIME's 19-bit Z wraps.
//
// VIDEO: the GIME hands over each finished scanline as 912 six-bit colours
// (one per 14.318 MHz pixel). The 640 pixels around the active area go to
// the framebuffer: all of them with VIDEO_640 (PIZERO-134), else halved to
// 320 columns, where a 640-wide mode (80 columns) loses every other pixel. The
// active area is centred vertically: 192 lines start at row 24, as the
// CoCo 2 picture does, and 225 lines at row 7.
//
// PIZERO-213: a row is only rewritten when its pixels changed. Writing the
// framebuffer that core 1 is scanning out costs 3.5-7 ms a frame on the
// board, so each row keeps a hash of the GIME pixels it last showed, and a
// row whose hash matches is left alone. Anything else that draws into the
// framebuffer (the overlay) must call coco3_bus_invalidate afterwards.

#ifndef COCO3_BUS_H
#define COCO3_BUS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "coco_palette.h"       // coco_pal_gime_to_rgb565

#define COCO3_ROM_SIZE   32768u
#define COCO3_FB_H       240

// PIZERO-134: VIDEO_640 keeps all 640 GIME pixels a line, one byte each (the
// 6-bit colour, which the scanout looks up in a palette): the same 153,600
// bytes as the 320-wide RGB565 framebuffer it replaces.
#ifdef VIDEO_640
#define COCO3_FB_W       640
typedef uint8_t coco3_pix_t;
#else
#define COCO3_FB_W       320
typedef uint16_t coco3_pix_t;
#endif

// What the caller still has to do for this cycle.
enum { C3_BUS_DONE = 0, C3_BUS_PIA0, C3_BUS_PIA1, C3_BUS_CARTIO };

struct coco3_bus {
    uint8_t *ram_lo;              // physical $00000-$0FFFF
    uint8_t *ram_hi;              // physical $10000-$1FFFF
    const uint8_t *rom;           // COCO3_ROM_SIZE bytes
    const uint8_t *cart;          // cartridge ROM, NULL for none
    uint16_t cart_mask;           // its size - 1

    // Video
    coco3_pix_t *fb;              // COCO3_FB_W x COCO3_FB_H, NULL for none
    uint16_t rgb[64];             // GIME colour to RGB565
    int line;                     // scanlines since the frame-sync pulse rose
    int act_x, act_y, act_w, act_h;   // the GIME's last set_active_area
    uint32_t row_sum[COCO3_FB_H];      // hash of what each row shows; 0 = redraw
};

// Every row is drawn again at its next scanline.
static inline void coco3_bus_invalidate(struct coco3_bus *b) {
    memset(b->row_sum, 0, sizeof b->row_sum);
}

static inline void coco3_bus_init(struct coco3_bus *b, uint8_t *lo, uint8_t *hi,
                                  const uint8_t *rom, coco3_pix_t *fb) {
    b->ram_lo = lo;
    b->ram_hi = hi;
    b->rom = rom;
    b->cart = 0;
    b->cart_mask = 0;
    b->fb = fb;
    for (int i = 0; i < 64; i++) b->rgb[i] = coco_pal_gime_to_rgb565((uint8_t)i);
    b->line = 0;
    b->act_x = 239; b->act_y = 39; b->act_w = 512; b->act_h = 192;   // 32 columns
    coco3_bus_invalidate(b);
}

static inline uint8_t *coco3_ram_at(const struct coco3_bus *b, uint32_t Z) {
    Z &= 0x1FFFFu;
    return ((Z & 0x10000u) ? b->ram_hi : b->ram_lo) + (Z & 0xFFFFu);
}

// One bus cycle after tcc1014_mem_cycle has decoded it. Moves the byte for
// RAM, ROM and cartridge ROM; says what is left for the caller.
static inline int coco3_bus_cycle(const struct coco3_bus *b, unsigned S, bool RAS,
                                  uint32_t Z, bool RnW, uint16_t A, uint8_t *D) {
    int left = C3_BUS_DONE;
    switch (S) {
    case 0:                                    // internal ROM
        if (RnW) *D = b->rom[A & 0x7FFF];
        break;
    case 1:                                    // cartridge ROM (CTS)
        if (RnW && b->cart) *D = b->cart[A & b->cart_mask];
        break;
    case 2:                                    // the PIAs
        left = (A & 0x20) ? C3_BUS_PIA1 : C3_BUS_PIA0;
        break;
    case 6:                                    // cartridge I/O (SCS)
        left = C3_BUS_CARTIO;
        break;
    default:
        break;
    }
    if (RAS) {
        uint8_t *p = coco3_ram_at(b, Z);
        if (RnW) *D = *p;
        else     *p = *D;
    }
    return left;
}

// PIZERO-213: where the 6809 core may read and write one 8 KB logical page
// itself (coco_fastbus.h), given the GIME's decode of it (S as
// tcc1014_decode_page returns it, and the bank): the same bytes
// coco3_bus_cycle would move, or NULL for a page that must take the full
// cycle. Internal ROM is addressed by the logical address, as in
// coco3_bus_cycle; a cartridge smaller than a page, or none, is left slow.
static inline void coco3_bus_page(const struct coco3_bus *b, unsigned page, unsigned S,
                                  unsigned bank, const uint8_t **rd, uint8_t **wr) {
    *rd = 0;
    *wr = 0;
    switch (S) {
    case 0:
        *rd = b->rom + ((page << 13) & 0x7FFFu);
        break;
    case 1:
        if (b->cart && b->cart_mask >= 0x1FFFu) *rd = b->cart + ((page << 13) & b->cart_mask);
        break;
    case 7: {
        uint8_t *p = coco3_ram_at(b, (uint32_t)bank << 13);
        *rd = p;
        *wr = p;
        break;
    }
    default:
        break;
    }
}

// The GIME's video fetch: 16 bits at an even physical address.
static inline uint16_t coco3_bus_fetch_vram(const struct coco3_bus *b, uint32_t A) {
    const uint8_t *p = coco3_ram_at(b, A & ~1u);
    return (uint16_t)((p[0] << 8) | p[1]);
}

// True when the area moved, so the rows the lines land on changed.
static inline bool coco3_bus_set_active_area(struct coco3_bus *b, int x, int y, int w, int h) {
    if (h > 225) h = 225;                      // LPF "infinite": show what fits
    bool moved = x != b->act_x || y != b->act_y || w != b->act_w || h != b->act_h;
    b->act_x = x; b->act_y = y; b->act_w = w; b->act_h = h;
    if (moved) coco3_bus_invalidate(b);
    return moved;
}

static inline void coco3_bus_frame_sync(struct coco3_bus *b, bool level) {
    if (level) b->line = 0;
}

// The framebuffer row a scanline lands on, or -1.
static inline int coco3_bus_row(const struct coco3_bus *b, int line) {
    int row = line - b->act_y + (COCO3_FB_H - b->act_h) / 2;
    return (row >= 0 && row < COCO3_FB_H) ? row : -1;
}

// The first of the 640 GIME pixels shown: centred on the active area.
static inline int coco3_bus_first_pixel(const struct coco3_bus *b) {
    return b->act_x + b->act_w / 2 - 320;
}

// A hash of n bytes of GIME pixels (n a multiple of 4), never 0. Read a
// word at a time: much cheaper than the copy it saves.
static inline uint32_t coco3_bus_line_sum(const uint8_t *src, int n) {
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; i += 4) {
        uint32_t w;
        memcpy(&w, src + i, 4);
        h ^= w;
        h = ((h << 5) | (h >> 27)) * 0x9E3779B1u;
    }
    return h | 1u;
}

// PIZERO-330: a line the GIME hands over undecoded (npixels TCC1014_PORT_JOB):
// where its COCO3_FB_W pixels go, NULL for none. The caller decodes it there
// (tcc1014_port_decode), now or on the other core. The row's sum is cleared,
// as what it will show is not hashed here.
static inline coco3_pix_t *coco3_bus_job_dst(struct coco3_bus *b) {
    int row = coco3_bus_row(b, b->line++);
    if (row < 0 || !b->fb) return NULL;
    b->row_sum[row] = 0;
    return b->fb + row * COCO3_FB_W;
}

// One scanline from the GIME. npixels says which form: COCO3_FB_W (its
// port_render form, PIZERO-218: the shown pixels, already reduced to it) or
// a whole scanline, of which the 640 around the active area are taken here.
static inline void coco3_bus_render_line(struct coco3_bus *b, const uint8_t *data,
                                         unsigned npixels) {
    int row = coco3_bus_row(b, b->line++);
    if (row < 0 || !b->fb || !data) return;
    coco3_pix_t *dst = b->fb + row * COCO3_FB_W;
    const uint8_t *src = data;
    if (npixels != COCO3_FB_W) {
        src = data + coco3_bus_first_pixel(b);
        uint32_t sum = coco3_bus_line_sum(src, 640);
        if (b->row_sum[row] == sum) return;
        b->row_sum[row] = sum;
#ifdef VIDEO_640
        for (int x = 0; x < 640; x++) dst[x] = src[x] & 63;
#else
        for (int x = 0; x < COCO3_FB_W; x++) dst[x] = b->rgb[src[2 * x] & 63];
#endif
        return;
    }
    uint32_t sum = coco3_bus_line_sum(src, COCO3_FB_W);
    if (b->row_sum[row] == sum) return;
    b->row_sum[row] = sum;
#ifdef VIDEO_640
    // The palette lookup happens at scanout; the GIME's colours are 6-bit.
    memcpy(dst, src, COCO3_FB_W);
#else
    for (int x = 0; x < COCO3_FB_W; x++) dst[x] = b->rgb[src[x] & 63];
#endif
}

#endif

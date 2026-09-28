// png_write.h: a streaming PNG writer for screenshots (PIZERO-165).
//
// Pure logic, host-tested (test/test_png_write). Writes an 8-bit indexed PNG
// a row at a time through a sink callback, so nothing larger than one row is
// ever held in memory. The pixel data is zlib-wrapped deflate with STORED
// (uncompressed) blocks: no compression library, a few hundred bytes of code,
// and any PNG reader opens it. A 320x240 screen comes to about 77 KB.
//
// The screen never has more than a couple of dozen colors (the 16-entry
// palette plus the card's), so the caller builds a palette while scanning and
// hands the writer palette indexes.

#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*png_sink_fn)(void *ctx, const uint8_t *p, size_t n);
// Fill `out` with row `y` as palette indexes (w bytes).
typedef void (*png_row_fn)(void *ctx, int y, uint8_t *out);

// CRC-32 (the PNG and zlib polynomial) a nibble at a time: a 16-entry table
// instead of the usual 1 KB, fast enough for a screenshot.
static inline uint32_t png_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ T[crc & 15];
        crc = (crc >> 4) ^ T[crc & 15];
    }
    return crc;
}

struct png_out {
    png_sink_fn sink;
    void       *ctx;
    uint32_t    crc;           // running CRC of the current chunk
    uint32_t    adler_a, adler_b;
    uint32_t    block_left;    // bytes left in the current stored block
    uint32_t    raw_left;      // raw (filtered) bytes still to come
    size_t      written;
};

static inline void png_emit(struct png_out *o, const uint8_t *p, size_t n) {
    o->sink(o->ctx, p, n);
    o->crc = png_crc_update(o->crc, p, n);
    o->written += n;
}

static inline void png_be32(struct png_out *o, uint32_t v, bool in_crc) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    if (in_crc) png_emit(o, b, 4);
    else { o->sink(o->ctx, b, 4); o->written += 4; }
}

static inline void png_chunk_begin(struct png_out *o, uint32_t len, const char type[4]) {
    png_be32(o, len, false);
    o->crc = 0xFFFFFFFFu;
    png_emit(o, (const uint8_t *)type, 4);
}

static inline void png_chunk_end(struct png_out *o) {
    png_be32(o, o->crc ^ 0xFFFFFFFFu, false);
}

// Raw pixel bytes into the IDAT: split into stored blocks of at most 65535
// bytes, each with its 5-byte header, keeping the Adler-32 as we go.
static inline void png_raw(struct png_out *o, const uint8_t *p, size_t n) {
    while (n) {
        if (o->block_left == 0) {
            uint32_t len = o->raw_left < 65535u ? o->raw_left : 65535u;
            uint8_t h[5] = { (uint8_t)(len == o->raw_left ? 1 : 0),
                             (uint8_t)len, (uint8_t)(len >> 8),
                             (uint8_t)~len, (uint8_t)(~len >> 8) };
            png_emit(o, h, 5);
            o->block_left = len;
        }
        size_t k = n < o->block_left ? n : o->block_left;
        png_emit(o, p, k);
        for (size_t i = 0; i < k; i++) {
            o->adler_a = (o->adler_a + p[i]) % 65521u;
            o->adler_b = (o->adler_b + o->adler_a) % 65521u;
        }
        o->block_left -= (uint32_t)k;
        o->raw_left -= (uint32_t)k;
        p += k; n -= k;
    }
}

// Write a w x h indexed PNG with `npal` palette entries (RGB, 3 bytes each).
// `row` is scratch of at least w bytes. Returns the bytes written.
static inline size_t png_write_indexed(png_sink_fn sink, void *ctx, int w, int h,
                                       const uint8_t *pal_rgb, int npal,
                                       png_row_fn get_row, void *row_ctx, uint8_t *row) {
    struct png_out o = { sink, ctx, 0, 1, 0, 0, 0, 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    sink(ctx, sig, 8);
    o.written = 8;

    png_chunk_begin(&o, 13, "IHDR");
    png_be32(&o, (uint32_t)w, true);
    png_be32(&o, (uint32_t)h, true);
    const uint8_t ihdr[5] = { 8, 3, 0, 0, 0 };   // 8-bit, indexed, deflate, no filter set, no interlace
    png_emit(&o, ihdr, 5);
    png_chunk_end(&o);

    png_chunk_begin(&o, (uint32_t)npal * 3u, "PLTE");
    png_emit(&o, pal_rgb, (size_t)npal * 3u);
    png_chunk_end(&o);

    uint32_t raw = (uint32_t)h * (uint32_t)(w + 1);          // a filter byte per row
    uint32_t blocks = raw ? (raw + 65534u) / 65535u : 1u;
    png_chunk_begin(&o, 2u + blocks * 5u + raw + 4u, "IDAT");
    const uint8_t zhdr[2] = { 0x78, 0x01 };                   // deflate, 32 KB window, no dict
    png_emit(&o, zhdr, 2);
    o.raw_left = raw;
    for (int y = 0; y < h; y++) {
        const uint8_t filter = 0;                             // "None"
        png_raw(&o, &filter, 1);
        get_row(row_ctx, y, row);
        png_raw(&o, row, (size_t)w);
    }
    png_be32(&o, (o.adler_b << 16) | o.adler_a, true);
    png_chunk_end(&o);

    png_chunk_begin(&o, 0, "IEND");
    png_chunk_end(&o);
    return o.written;
}

// RGB565 to the RGB888 a PNG palette holds, replicating the top bits so pure
// white stays 255.
static inline void png_rgb565_to_888(uint16_t c, uint8_t out[3]) {
    uint8_t r = (uint8_t)(c >> 11), g = (uint8_t)((c >> 5) & 63), b = (uint8_t)(c & 31);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

// The palette index of RGB565 color `c`, adding it if new. -1 when full.
static inline int png_palette_index(uint16_t *pal, int *n, int max, uint16_t c) {
    for (int i = 0; i < *n; i++) if (pal[i] == c) return i;
    if (*n >= max) return -1;
    pal[*n] = c;
    return (*n)++;
}

#endif  // PNG_WRITE_H

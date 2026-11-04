// dmk.h: DMK disk images, the sector lookup (PIZERO-320).
//
// A DMK stores each track as the controller sees it, so where a sector sits
// is found by reading the track, not worked out from its number as with a
// JVC .DSK. Pure logic, host-tested (test/test_dmk); the file reads are the
// caller's, through a callback, like rsdos_file.h.
//
// Layout, as upstream XRoar reads it (src/vdisk.c vdisk_load_dmk):
//   16-byte header: [0] non-zero = write back disabled; [1] tracks;
//   [2..3] track length, LITTLE-endian, 0x0CC0..0x2940; [4] flags (bit 4
//   single-sided, bit 6 single density, bit 7 mixed density);
//   [11] $00 or $FF overrides write-protect.
//   Then tracks in order, side 0 then side 1 of each cylinder, each
//   track_len bytes: 64 little-endian IDAM pointers, then the raw track.
// An IDAM pointer: bit 15 double density, low 14 bits the offset from the
// track's start (pointers included) of the ID mark $FE; 0 ends the list.
// After the mark: track, side, sector, size code, CRC (2). The data mark
// ($FB, or $F8 deleted) follows within 43 bytes, then the data and its CRC.
//
// This port reads double-density 256-byte sectors only (what Disk BASIC and
// OS-9 write). A single-density sector, which a DMK stores with every byte
// doubled, or another size, is left out of the map and reads as not found.

#ifndef DMK_H
#define DMK_H

#include <stdint.h>

#define DMK_HEADER      16
#define DMK_IDAMS       64
#define DMK_TRACK_MIN   0x0CC0
#define DMK_TRACK_MAX   0x2940
#define DMK_IDAM_DD     0x8000
#define DMK_IDAM_OFF    0x3FFF
#define DMK_ID_LEN      7          // $FE, track, side, sector, size, CRC hi, CRC lo
#define DMK_DAM_WINDOW  43         // bytes after the ID searched for the data mark
#define DMK_MAP_MAX     24         // sectors kept per track (Disk BASIC has 18)
#define DMK_MAP_F8      0x8000     // in a map offset: the data mark is $F8

struct dmk_geom {
    uint16_t track_len;
    uint8_t  tracks;
    uint8_t  sides;
    uint8_t  protect;              // header says do not write
};

// One track's sectors: id[i] is a sector number, off[i] the offset of its
// first data byte from the track's start (DMK_MAP_F8 set for a $F8 mark).
struct dmk_map {
    uint8_t  track, side;          // which track this is; valid only if n_ok
    uint8_t  n_ok;                 // 1 once built
    uint8_t  n;
    uint8_t  id[DMK_MAP_MAX];
    uint16_t off[DMK_MAP_MAX];
};

enum { DMK_OK = 0, DMK_SHORT = 1, DMK_BAD_LENGTH = 2, DMK_NO_TRACKS = 3 };

// Parse the header. file_size must hold at least the first track; a short
// last track is allowed (XRoar zero-fills it), reads past the end fail.
static inline int dmk_parse_header(const uint8_t h[DMK_HEADER], uint32_t file_size,
                                   struct dmk_geom *g) {
    if (file_size < DMK_HEADER) return DMK_SHORT;
    unsigned len = (unsigned)h[2] | ((unsigned)h[3] << 8);
    if (len < DMK_TRACK_MIN || len > DMK_TRACK_MAX) return DMK_BAD_LENGTH;
    if (h[1] == 0) return DMK_NO_TRACKS;
    if (file_size < DMK_HEADER + (uint32_t)len) return DMK_SHORT;
    g->track_len = (uint16_t)len;
    g->tracks = h[1];
    g->sides = (h[4] & 0x10) ? 1 : 2;
    // Writes go straight into the file here, so XRoar's "no write back"
    // (byte 0) can only mean protected, as can byte 11 = $FF.
    g->protect = h[0] != 0 || h[11] == 0xFF;
    return DMK_OK;
}

// Does this look like a DMK header? For a file named something else.
static inline bool dmk_sniff(const uint8_t h[DMK_HEADER], uint32_t file_size) {
    struct dmk_geom g;
    if (dmk_parse_header(h, file_size, &g) != DMK_OK) return false;
    for (int i = 5; i < 11; i++) if (h[i]) return false;     // reserved, zero
    if (h[0] != 0x00 && h[0] != 0xFF) return false;
    uint32_t body = file_size - DMK_HEADER;
    return body % g.track_len == 0 && body / g.track_len == (uint32_t)g.tracks * g.sides;
}

// Where a track starts in the file.
static inline uint32_t dmk_track_start(const struct dmk_geom *g, unsigned track, unsigned side) {
    return DMK_HEADER + ((uint32_t)track * g->sides + side) * g->track_len;
}

// CRC-16-CCITT (poly $1021), as the WD279x computes it.
static inline uint16_t dmk_crc_byte(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

// The CRC a double-density data field stores: over A1 A1 A1, the mark and
// the data.
static inline uint16_t dmk_data_crc(uint8_t mark, const uint8_t *data, unsigned n) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; i++) crc = dmk_crc_byte(crc, 0xA1);
    crc = dmk_crc_byte(crc, mark);
    for (unsigned i = 0; i < n; i++) crc = dmk_crc_byte(crc, data[i]);
    return crc;
}

// Given the bytes from an ID mark on (win, n of them), find its data field.
// Returns the offset of the first data byte from the ID mark, or -1 when it
// is not a double-density 256-byte sector with a data mark in reach.
static inline int dmk_find_data(const uint8_t *win, unsigned n, uint8_t *sector, uint8_t *mark) {
    if (n < DMK_ID_LEN + 1 || win[0] != 0xFE) return -1;
    if (win[4] != 1) return -1;                           // size code 1 = 256 bytes
    unsigned end = DMK_ID_LEN + DMK_DAM_WINDOW;
    if (end > n) end = n;
    for (unsigned j = DMK_ID_LEN; j < end; j++) {
        if (win[j] == 0xFB || win[j] == 0xF8) {
            *sector = win[3];
            *mark = win[j];
            return (int)j + 1;
        }
    }
    return -1;
}

// Reads n bytes at an absolute file offset; returns how many it read.
typedef unsigned (*dmk_read_fn)(void *ctx, uint32_t off, uint8_t *buf, unsigned n);

// Build the map of one track. Returns the number of sectors found, or -1 if
// the track is off the disk or its pointers cannot be read.
static inline int dmk_build_map(const struct dmk_geom *g, unsigned track, unsigned side,
                                dmk_read_fn rd, void *ctx, struct dmk_map *m) {
    m->n_ok = 0;
    m->n = 0;
    if (track >= g->tracks || side >= g->sides) return -1;
    uint32_t base = dmk_track_start(g, track, side);
    uint8_t ptrs[DMK_IDAMS * 2];
    if (rd(ctx, base, ptrs, sizeof ptrs) != sizeof ptrs) return -1;
    uint8_t win[DMK_ID_LEN + DMK_DAM_WINDOW];
    for (unsigned i = 0; i < DMK_IDAMS && m->n < DMK_MAP_MAX; i++) {
        uint16_t p = (uint16_t)(ptrs[2 * i] | (ptrs[2 * i + 1] << 8));
        if (p == 0) break;
        if (!(p & DMK_IDAM_DD)) continue;                 // single density: not read here
        unsigned at = p & DMK_IDAM_OFF;
        if (at < DMK_IDAMS * 2 || at >= g->track_len) continue;
        unsigned want = sizeof win;
        if (at + want > g->track_len) want = g->track_len - at;
        unsigned got = rd(ctx, base + at, win, want);
        uint8_t sec, mark;
        int d = dmk_find_data(win, got, &sec, &mark);
        if (d < 0) continue;
        unsigned data = at + (unsigned)d;
        if (data + 256 + 2 > g->track_len) continue;      // data runs off the track
        bool dup = false;
        for (unsigned k = 0; k < m->n; k++) if (m->id[k] == sec) dup = true;
        if (dup) continue;                                // the first one wins, as on a drive
        m->id[m->n] = sec;
        m->off[m->n] = (uint16_t)(data | (mark == 0xF8 ? DMK_MAP_F8 : 0));
        m->n++;
    }
    m->track = (uint8_t)track;
    m->side = (uint8_t)side;
    m->n_ok = 1;
    return m->n;
}

// A sector's data offset in the file, or 0 when the map does not have it.
// *mark is the data mark it was written with.
static inline uint32_t dmk_map_find(const struct dmk_geom *g, const struct dmk_map *m,
                                    unsigned sector, uint8_t *mark) {
    for (unsigned k = 0; k < m->n; k++) {
        if (m->id[k] != sector) continue;
        if (mark) *mark = (m->off[k] & DMK_MAP_F8) ? 0xF8 : 0xFB;
        return dmk_track_start(g, m->track, m->side) + (m->off[k] & DMK_IDAM_OFF);
    }
    return 0;
}

#endif

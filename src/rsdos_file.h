// rsdos_file.h: reading one file out of a Disk BASIC disk (PIZERO-309).
//
// Pure logic, host-tested (test/test_rsdos_file); the sector reads are the
// caller's, through a callback, so the board reads a mounted .DSK through
// coco_boot_disk_read_sector (the path the FDC uses) and the tests read an
// image built in memory.
//
// The layout (Disk BASIC 1.1, confirmed in disk11.rom; see
// docs/directory-drives-proposal.md):
//   * 35 tracks of 18 sectors of 256 bytes, sectors numbered from 1
//   * track 17 is the directory: sector 2 the FAT, sectors 3-11 the entries
//   * the rest is 68 granules of 9 sectors, two to a track, tracks 0-16 then
//     18-34: granule g is track g/2 (plus one from 17 on), sectors 1-9 for an
//     even g and 10-18 for an odd one
//   * FAT byte for a granule: the next granule ($00-$43), or $C0 + the
//     number of sectors used in this last one (1-9); $FF is free
//   * a directory entry is 32 bytes: name (8), extension (3), type, ASCII
//     flag ($FF ASCII, $00 binary), first granule, then at 14-15 the bytes
//     used in the last sector (big-endian); first byte $00 deleted, $FF the
//     end of the directory

#ifndef RSDOS_FILE_H
#define RSDOS_FILE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RSDOS_GRANULES     68
#define RSDOS_GRAN_SECTORS  9
#define RSDOS_FAT_TRACK    17
#define RSDOS_FAT_SECTOR    2

enum { RSDOS_OK = 0, RSDOS_TOO_BIG = 1, RSDOS_BROKEN = 2, RSDOS_READ_ERROR = 3 };

// Where granule g starts.
static inline void rsdos_granule_ts(int g, unsigned *track, unsigned *sector) {
    unsigned t = (unsigned)g / 2;
    if (t >= RSDOS_FAT_TRACK) t++;
    *track = t;
    *sector = (g & 1) ? 10u : 1u;
}

// The bytes used in the last sector, from a directory entry. Disk BASIC
// never writes 0 for a file that has a last sector; read 0 as a full one.
static inline unsigned rsdos_last_bytes(const uint8_t *entry) {
    unsigned n = ((unsigned)entry[14] << 8) | entry[15];
    return (n == 0 || n > 256) ? 256u : n;
}

// The file's length from its chain, without reading it. RSDOS_BROKEN for a
// chain that loops, runs off the disk or ends on a free granule.
static inline int rsdos_file_size(const uint8_t fat[RSDOS_GRANULES], const uint8_t *entry, uint32_t *size) {
    int g = entry[13];
    uint32_t sectors = 0;
    for (int steps = 0; steps < RSDOS_GRANULES; steps++) {
        if (g < 0 || g >= RSDOS_GRANULES) return RSDOS_BROKEN;
        uint8_t v = fat[g];
        if (v >= 0xC0 && v <= 0xC9) {
            unsigned used = v & 0x0F;
            if (used == 0) { *size = sectors * 256u; return RSDOS_OK; }   // an empty last granule
            *size = (sectors + used - 1) * 256u + rsdos_last_bytes(entry);
            return RSDOS_OK;
        }
        if (v >= RSDOS_GRANULES) return RSDOS_BROKEN;
        sectors += RSDOS_GRAN_SECTORS;
        g = v;
    }
    return RSDOS_BROKEN;
}

// Read one 256-byte sector; return 0 on success.
typedef int (*rsdos_read_fn)(void *ctx, unsigned track, unsigned sector, uint8_t out[256]);

// Read the whole file into out (at most max bytes). sec is a 256-byte
// scratch sector. RSDOS_TOO_BIG, with *len the true size, if it does not fit.
static inline int rsdos_read_file(rsdos_read_fn rd, void *ctx, const uint8_t fat[RSDOS_GRANULES],
                                  const uint8_t *entry, uint8_t sec[256],
                                  char *out, uint32_t max, uint32_t *len) {
    uint32_t size;
    int rc = rsdos_file_size(fat, entry, &size);
    *len = size;
    if (rc != RSDOS_OK) return rc;
    if (size > max) return RSDOS_TOO_BIG;
    uint32_t k = 0;
    int g = entry[13];
    while (k < size) {
        unsigned t, s;
        rsdos_granule_ts(g, &t, &s);
        for (int i = 0; i < RSDOS_GRAN_SECTORS && k < size; i++) {
            if (rd(ctx, t, s + (unsigned)i, sec) != 0) return RSDOS_READ_ERROR;
            uint32_t n = size - k < 256u ? size - k : 256u;
            memcpy(out + k, sec, n);
            k += n;
        }
        uint8_t v = fat[g];
        if (v >= 0xC0) break;
        g = v;
    }
    return RSDOS_OK;
}

// A directory entry's name as Disk BASIC writes it with a drive:
// "NAME.EXT:d", or "NAME:d" for a blank extension. Returns false for an
// entry that is not a file (deleted, never used, or no name). out needs 15.
static inline bool rsdos_entry_name(const uint8_t *entry, unsigned drive, char *out, size_t out_sz) {
    if (out_sz < 15 || entry[0] == 0x00 || entry[0] == 0xFF) return false;
    int nl = 8, el = 3, k = 0;
    while (nl > 0 && entry[nl - 1] == ' ') nl--;
    while (el > 0 && entry[8 + el - 1] == ' ') el--;
    if (nl == 0) return false;
    for (int i = 0; i < nl; i++) {
        char c = (char)entry[i];
        out[k++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    if (el) {
        out[k++] = '.';
        for (int i = 0; i < el; i++) {
            char c = (char)entry[8 + i];
            out[k++] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
    }
    out[k++] = ':';
    out[k++] = (char)('0' + (drive & 3));
    out[k] = '\0';
    return true;
}

// Is this entry a text file (Disk BASIC's ASCII flag)?
static inline bool rsdos_entry_is_ascii(const uint8_t *entry) {
    return entry[0] != 0x00 && entry[0] != 0xFF && entry[12] == 0xFF;
}

#endif  // RSDOS_FILE_H

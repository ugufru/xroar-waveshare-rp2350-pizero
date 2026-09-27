// rsdos_dir.h: find the program to run on a Disk BASIC disk (PIZERO-81d).
//
// ENTER on a disk in the F12 overlay cold-boots with that disk in drive 0 and
// types the command that starts its first program, the way the Fruit Jam port
// does (FRUITJAM-72, src/coco/coco_main.cpp dsk_first_program). Pure logic,
// host-tested; the sector reads are the caller's.
//
// RSDOS directory: track 17, sectors 3-11, eight 32-byte entries per sector.
// Entry: [0..7] name, space-padded; [8..10] extension; [11] file type;
// [12] ASCII flag. First byte $FF ends the directory, $00 marks a deleted
// entry. Types: 0 BASIC, 1 BASIC data, 2 machine language, 3 text. Only 0 and
// 2 can be run, so a data file that happens to be first cannot win.

#ifndef RSDOS_DIR_H
#define RSDOS_DIR_H

#include <stdint.h>
#include <stdio.h>

#define RSDOS_DIR_TRACK      17
#define RSDOS_DIR_FIRST_SEC  3
#define RSDOS_DIR_LAST_SEC   11

enum { RSDOS_BASIC = 0, RSDOS_ML = 2 };

// Scan one directory sector. Returns 1 with name/type filled when it holds a
// runnable program, -1 when the directory ends in it, 0 to keep looking.
static inline int rsdos_scan_sector(const uint8_t sec[256], char name[9], int *type) {
    for (int e = 0; e < 8; e++) {
        const uint8_t *d = sec + e * 32;
        if (d[0] == 0xFF) return -1;
        if (d[0] == 0x00) continue;
        if (d[11] != RSDOS_BASIC && d[11] != RSDOS_ML) continue;
        int n = 0;
        for (int i = 0; i < 8 && d[i] != ' '; i++) {
            char c = (char)d[i];
            if (c < 0x21 || c > 0x5F) break;       // not something we can type
            name[n++] = c;
        }
        name[n] = '\0';
        if (n == 0) continue;
        *type = d[11];
        return 1;
    }
    return 0;
}

// The command that starts it, ready for the autotype queue ('\r' = ENTER).
// A bare name resolves on Disk BASIC's default drive, which is drive 0: the
// reason the disk goes into drive 0 before the cold boot.
static inline int rsdos_run_command(const char *name, int type, char *out, size_t out_sz) {
    return type == RSDOS_BASIC ? snprintf(out, out_sz, "RUN\"%s\"\r", name)
                               : snprintf(out, out_sz, "LOADM\"%s\":EXEC\r", name);
}

#endif  // RSDOS_DIR_H

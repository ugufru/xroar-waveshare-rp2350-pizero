/*
 * coco_boot.cpp: front-end glue between the XRoar core and the AMOLED panel.
 *
 *   * coco_boot_load_rom_from_sd() reads /coco/extbas11.rom and
 *     /coco/bas12.rom from the mounted SD card and concatenates them
 *     into a 16 KB image (ECB low half, BASIC high half, so that
 *     ROM[A & 0x3FFF] resolves correctly for A in $8000-$BFFF and
 *     for the reset vector at $FFFE).
 *
 *   * coco_boot_blit_vdg() takes the live 256x192 palette-index buffer
 *     from coco_machine and writes it 1x, centred, into the landscape
 *     448x368 framebuffer. Rotation to the panel-native 368x448 portrait
 *     is inlined here (see src/main.cpp::plot() for the convention).
 *
 *   * coco_boot_blit_vdg_src() is the same blit with the source pointer
 *     passed in directly, used by main.cpp before XRoar is wired up
 *     so the blitter can be tested in isolation.
 */

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "ff.h"
#include "f_util.h"
#include "coco_machine.h"
#include "hot.h"
}

#include "coco_boot.h"
#include "dsk_catalog.h"        // PIZERO-81a, host-tested
#include "settings.h"           // PIZERO-145, host-tested
#include "text_lines.h"         // PIZERO-294: CR, LF or CRLF line ends, host-tested
#include "decb_files.h"         // PIZERO-289: decb_is_text, host-tested
#include "rsdos_file.h"         // PIZERO-309: files inside a .DSK, host-tested
#include "rsdos_dir.h"          // the directory track and sectors
#include "dmk.h"                // PIZERO-320: DMK images, host-tested

#define COCO_NDRIVE 4           // drives 0-3 (PIZERO-114)
#include "png_write.h"          // PIZERO-165, host-tested
#include "hardware/watchdog.h"   // PIZERO-165: fed while a screenshot writes
#include <time.h>
#include "pico/aon_timer.h"      // PIZERO-201: the clock

// Panel native: must match src/main.cpp.
#define LCD_W   368
#define LCD_H   448

// Logical landscape: must match src/main.cpp.
#define SCREEN_W  448  // = LCD_H
#define SCREEN_H  368  // = LCD_W

// Full-screen anamorphic blit: source 256x192 stretched to 448x368 landscape
// via nearest-neighbor lookup tables. Horizontal ratio 1.75, vertical ratio
// ~1.917, close to the real CoCo's wide NTSC pixel aspect.
//
// Two tables built once at boot:
//   g_cx_byte_for_py[]: panel row py (= landscape sx) -> source byte column
//     (cx >> 1) in 0..127, since vdg_buffer is nibble-packed (2 indices/byte).
//   g_nibble_shift_for_py[]: 0 if cx is even (low nibble), 4 if odd (high).
//   g_src_off_for_px[]: panel column px -> byte offset into the source row
//     for the matching cy (= ymap[LCD_W-1-px] * (COCO_VDG_W/2)).
//
// Hot loop is panel-row-major: writes to fb are contiguous (one full panel
// row of 368 RGB565 pixels per iteration), source reads are stride-(COCO_VDG_W/2)
// down a single column of the VDG buffer (whole VDG fits in cache anyway).

static uint8_t  g_cx_byte_for_py[LCD_H];       //  448 entries
static uint8_t  g_nibble_shift_for_py[LCD_H];  //  448 entries
static uint16_t g_src_off_for_px[LCD_W];       //  368 entries × 2 = 736 B
static bool     g_maps_ready = false;

static void build_maps() {
    for (int py = 0; py < LCD_H; py++) {
        // py == landscape sx; map to 0..COCO_VDG_W-1, then split into
        // byte-column and nibble parity for the packed buffer.
        int cx = (py * COCO_VDG_W) / LCD_H;
        g_cx_byte_for_py[py]      = (uint8_t)(cx >> 1);
        g_nibble_shift_for_py[py] = (uint8_t)((cx & 1) ? 4 : 0);
    }
    for (int px = 0; px < LCD_W; px++) {
        int sy = (LCD_W - 1) - px;                // landscape sy in 0..LCD_W-1
        int cy = (sy * COCO_VDG_H) / LCD_W;       // 0..191
        g_src_off_for_px[px] = (uint16_t)(cy * (COCO_VDG_W / 2));
    }
    g_maps_ready = true;
}

// VDG palette (12 entries; indices 12..15 padded black). Each value is
// stored byte-swapped: the SH8601 over QSPI consumes MSB-first, so the
// framebuffer holds wire-order bytes. See AMOLED-21.
#define BS(v) (uint16_t)(((uint16_t)(v) >> 8) | ((uint16_t)(v) << 8))
static const uint16_t g_vdg_rgb565[16] = {
    BS(0x07E0),  // 0  VDG_GREEN
    BS(0xFFE0),  // 1  VDG_YELLOW
    BS(0x001F),  // 2  VDG_BLUE
    BS(0xF800),  // 3  VDG_RED
    BS(0xFFFF),  // 4  VDG_WHITE
    BS(0x07FF),  // 5  VDG_CYAN
    BS(0xF81F),  // 6  VDG_MAGENTA
    BS(0xFC00),  // 7  VDG_ORANGE
    BS(0x0000),  // 8  VDG_BLACK
    BS(0x0320),  // 9  VDG_DARK_GREEN
    BS(0x8200),  // 10 VDG_DARK_ORANGE
    BS(0xFCA0),  // 11 VDG_BRIGHT_ORANGE
    BS(0x0000), BS(0x0000), BS(0x0000), BS(0x0000),
};
#undef BS

// PIZERO-319: the FIL (about 600 bytes) is on the heap, not the stack: a
// machine switch reads the ROMs from loop(), and with this on the stack core
// 0 reached 3,880 of its 4,096 bytes, 216 short of core 1's stack below it.
static size_t read_rom_file(const char *path, uint8_t *out, size_t want) {
    FIL *f = (FIL *)malloc(sizeof *f);
    if (!f) {
        Serial.printf("[rom] %s: no memory to open it\n", path);
        return 0;
    }
    FRESULT fr = f_open(f, path, FA_READ);
    if (fr != FR_OK) {
        free(f);
        Serial.printf("[rom] open %s failed: %s (%d)\n", path, FRESULT_str(fr), fr);
        return 0;
    }
    UINT br = 0;
    fr = f_read(f, out, want, &br);
    f_close(f);
    free(f);
    if (fr != FR_OK) {
        Serial.printf("[rom] read %s failed: %s (%d)\n", path, FRESULT_str(fr), fr);
        return 0;
    }
    Serial.printf("[rom] %s: %u bytes\n", path, (unsigned)br);
    return br;
}

// AMOLED-58: path resolver. Try /coco/<subdir>/<name> first (typed
// layout the user is encouraged to use), then /coco/<name> (flat
// fallback for users who just dump everything in the root). Both are
// case-insensitive thanks to FatFs. If `name` doesn't already have a
// dot in it, also try with the conventional extension for the subdir
// appended (so "@DIRECT ORBIT" finds "ORBIT.BIN").
// AMOLED-58 resolver: tries the typed subdir, then flat /coco/.
// Subdir naming: ROMs live in "roms" (plural); bins/disks in "bin"/"dsk"
// (singular). All types also fall back to flat /coco/ so a card that
// just dumps everything in the root still boots.
extern "C" bool coco_boot_resolve(const char *subdir, const char *name,
                                  char *out, size_t out_sz) {
    if (!name || !*name || !out || out_sz < 32) return false;
    FILINFO fi;

    auto try_path = [&](const char *p) -> bool {
        return f_stat(p, &fi) == FR_OK;
    };

    // Candidate subdirs in priority order. The caller passes the logical
    // type ("rom"/"bin"/"dsk"); ROMs map to the plural folder "roms".
    const char *sub_candidates[2] = { nullptr, nullptr };
    int nsub = 0;
    if (subdir && *subdir) {
        sub_candidates[nsub++] = (!strcmp(subdir, "rom")) ? "roms" : subdir;
    }
    sub_candidates[nsub++] = "";  // flat /coco/<name>

    // First pass: literal `name`.
    for (int i = 0; i < nsub; i++) {
        const char *s = sub_candidates[i];
        if (*s) snprintf(out, out_sz, "0:/coco/%s/%s", s, name);
        else    snprintf(out, out_sz, "0:/coco/%s",    name);
        if (try_path(out)) return true;
    }

    // Second pass: append conventional extension if `name` has no dot.
    if (!strchr(name, '.')) {
        const char *ext = nullptr;
        if (subdir) {
            if      (!strcmp(subdir, "bin")) ext = "BIN";
            else if (!strcmp(subdir, "dsk")) ext = "DSK";
            else if (!strcmp(subdir, "rom")) ext = "ROM";
            else if (!strcmp(subdir, "cart")) ext = "CCC";
        }
        if (ext) {
            for (int i = 0; i < nsub; i++) {
                const char *s = sub_candidates[i];
                if (*s) snprintf(out, out_sz, "0:/coco/%s/%s.%s", s, name, ext);
                else    snprintf(out, out_sz, "0:/coco/%s.%s",    name, ext);
                if (try_path(out)) return true;
            }
        }
    }
    return false;
}

// AMOLED-58: find the first .dsk (alphabetical) in /coco/dsk/ or /coco/.
static bool scan_dir_for_dsk(const char *dir, char *best_name, size_t name_sz) {
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return false;
    FILINFO fi;
    bool found = false;
    best_name[0] = '\0';
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        if (fi.fname[0] == '.') continue;   // skip dotfiles incl. macOS ._* AppleDouble
        const char *ext = strrchr(fi.fname, '.');
        if (!ext || (strcasecmp(ext, ".dsk") != 0 && strcasecmp(ext, ".dmk") != 0)) continue;
        if (!found || strcasecmp(fi.fname, best_name) < 0) {
            strncpy(best_name, fi.fname, name_sz - 1);
            best_name[name_sz - 1] = '\0';
            found = true;
        }
    }
    f_closedir(&d);
    return found;
}

extern "C" bool coco_boot_find_default_dsk(char *out, size_t out_sz) {
    char best[64];
    if (scan_dir_for_dsk("0:/coco/dsk", best, sizeof(best))) {
        snprintf(out, out_sz, "0:/coco/dsk/%s", best);
        return true;
    }
    if (scan_dir_for_dsk("0:/coco", best, sizeof(best))) {
        snprintf(out, out_sz, "0:/coco/%s", best);
        return true;
    }
    return false;
}

// PIZERO-81a/136: the list the F12 overlay shows, of one kind at a time
// (disks, programs or cartridges). The rules (what counts, order, cap,
// duplicates) are in dsk_catalog.h and tested on the host; this is only the
// FatFs walk. The kind's folder first, then /coco, so a name in both is
// listed from the kind's folder, as coco_boot_resolve would find it.
static struct dsk_catalog g_dsk_cat;

static void cat_scan_dir(const char *dir, uint8_t which) {
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;
        dsk_cat_add(&g_dsk_cat, fi.fname, (fi.fattrib & AM_DIR) != 0, which);
    }
    f_closedir(&d);
}

// PIZERO-296: the FILES list's walk of /coco and its folders. Everything it
// needs is in one heap block, taken only for the scan. It runs inside the USB
// key handler, already deep on core 0's 4 KB stack, and a FIL (with its
// 512-byte sector buffer), a FILINFO and a 512-byte sample on the stack
// overflowed it into core 1's: a hard fault on core 1 at a stack address.
#define TEXT_SUBS_MAX 16
// Folders one level down are scanned too only with FILES_SCAN_FOLDERS=1: the
// walk opens every file it might list, and with a card's log and game folders
// that made F11 slow to come up (user, 2026-11-01). Kept for later, PIZERO-308.
#ifndef FILES_SCAN_FOLDERS
#define FILES_SCAN_FOLDERS 0
#endif
struct text_scan_ws {
    DIR     d;
    FILINFO fi;
    FIL     f;
    uint8_t head[512];
    char    dir[48];
    char    path[DSK_NAME_MAX + 16];
    char    subs[TEXT_SUBS_MAX][32];
};

// Is w->path text, by its first 512 bytes (decb_is_text)?
static bool file_is_text(struct text_scan_ws *w) {
    if (f_open(&w->f, w->path, FA_READ) != FR_OK) return false;
    UINT br = 0;
    bool ok = f_read(&w->f, w->head, sizeof w->head, &br) == FR_OK && decb_is_text(w->head, (int)br);
    f_close(&w->f);
    return ok;
}

// Walk /coco/<sub>. At the top level (top) the folders found are noted in
// w->subs for a second pass, so only one directory is ever open at a time.
// The rules are dsk_catalog.h's.
static int text_scan_dir(struct text_scan_ws *w, const char *sub, bool top) {
    snprintf(w->dir, sizeof w->dir, sub[0] ? "0:/coco/%s" : "0:/coco", sub);
    if (f_opendir(&w->d, w->dir) != FR_OK) return 0;
    int nsub = 0;
    while (f_readdir(&w->d, &w->fi) == FR_OK && w->fi.fname[0]) {
        const char *name = w->fi.fname;
        if (w->fi.fattrib & (AM_HID | AM_SYS)) continue;
        if (w->fi.fattrib & AM_DIR) {
            if (top && nsub < TEXT_SUBS_MAX && strlen(name) < 32 && !text_cat_skip_dir(name))
                snprintf(w->subs[nsub++], 32, "%s", name);
            continue;
        }
        if (text_cat_skip(sub, name)) continue;
        if (strlen(sub) + 1 + strlen(name) >= DSK_NAME_MAX) {   // counted, not opened
            text_cat_add(&g_dsk_cat, sub, name);
            continue;
        }
        snprintf(w->path, sizeof w->path, "%s/%s", w->dir, name);
        if (file_is_text(w)) text_cat_add(&g_dsk_cat, sub, name);
        watchdog_update();
    }
    f_closedir(&w->d);
    return nsub;
}

extern "C" int coco_boot_rescan(int kind) {
    dsk_cat_clear(&g_dsk_cat, kind);
    if (kind == CAT_TEXT) {
        struct text_scan_ws *w = (struct text_scan_ws *)malloc(sizeof *w);
        if (!w) { Serial.print("[files] no memory to scan\r\n"); return 0; }
        int nsub = text_scan_dir(w, "", FILES_SCAN_FOLDERS != 0);
        for (int i = 0; i < nsub; i++) text_scan_dir(w, w->subs[i], false);
        dsk_cat_sort(&g_dsk_cat);
        int on_card = g_dsk_cat.n;
        // PIZERO-309: then the text files on each mounted disk, in directory
        // order, after the card's (so appended after the sort). The same
        // image in two drives is listed once.
        for (unsigned d = 0; d < COCO_NDRIVE; d++) {
            const char *p = coco_boot_drive_path(d);
            bool seen = !p;
            for (unsigned e = 0; e < d && !seen; e++) {
                const char *q = coco_boot_drive_path(e);
                seen = q && !strcasecmp(p, q);
            }
            if (seen) continue;
            for (unsigned s = RSDOS_DIR_FIRST_SEC; s <= RSDOS_DIR_LAST_SEC; s++) {
                if (coco_boot_disk_read_sector(d, RSDOS_DIR_TRACK, s, w->head) != 0) break;
                bool end = false;
                for (int k = 0; k < 8 && !end; k++) {
                    const uint8_t *ent = w->head + k * 32;
                    if (ent[0] == 0xFF) { end = true; break; }
                    char name[16];
                    if (rsdos_entry_is_ascii(ent) && rsdos_entry_name(ent, d, name, sizeof name))
                        text_cat_add_disk(&g_dsk_cat, d, name);
                }
                if (end) break;
            }
        }
        free(w);
        Serial.printf("[files] %d text file(s) on the card, %d on disks, %d name(s) too long, %d over the cap\r\n",
                      on_card, g_dsk_cat.n - on_card, g_dsk_cat.skipped_long, g_dsk_cat.skipped_full);
        return g_dsk_cat.n;
    }
    // PIZERO-312: the disks in the drives first, so a full card can never
    // push a mounted disk out of the list.
    if (kind == CAT_DSK)
        for (unsigned d = 0; d < COCO_NDRIVE; d++) dsk_cat_add_path(&g_dsk_cat, coco_boot_drive_path(d));
    cat_scan_dir(cat_kind_dir(kind), DSK_DIR_DSK);
    cat_scan_dir("0:/coco", DSK_DIR_ROOT);
    dsk_cat_sort(&g_dsk_cat);
    return g_dsk_cat.n;
}

extern "C" const struct dsk_catalog *coco_boot_dsk_catalog(void) {
    return &g_dsk_cat;
}

// PIZERO-145: read /coco/settings.txt into *out, starting from the defaults.
// Every bad line is reported on serial and skipped; nothing here is fatal.
// Returns false (with defaults in *out) when there is no file.
extern "C" bool coco_boot_load_settings(struct coco_settings *out) {
    return coco_boot_load_settings_for(nullptr, out, nullptr);
}

// PIZERO-294: files are read a line at a time through text_lines.h, so a
// line may end in CR (the CoCo), LF (the Mac) or CRLF (Windows). f_gets ends
// a line at LF only, so a CR file read as one line.
static int fil_read(void *ctx, char *buf, int n) {
    UINT br = 0;
    return f_read((FIL *)ctx, buf, (UINT)n, &br) == FR_OK ? (int)br : 0;
}

// PIZERO-319: working memory on the heap (about 900 bytes), as
// read_rom_file: a save in F11 runs this from the USB key handler.
static bool apply_settings_file(const char *path, struct coco_settings *out, bool quiet) {
    struct { FIL f; struct tl_reader rd; char line[96], name[32]; } *w = (decltype(w))malloc(sizeof *w);
    if (!w) {
        Serial.printf("[settings] %s: no memory to read it\r\n", path);
        return false;
    }
    FIL &f = w->f;
    char *line = w->line, *name = w->name;
    if (f_open(&f, path, FA_READ) != FR_OK) { free(w); return false; }
    int n = 0;
    struct tl_reader &rd = w->rd;
    tl_reader_init(&rd, fil_read, &f);
    while (tl_gets(&rd, line, sizeof w->line)) {
        n++;
        name[0] = '\0';
        int r = settings_parse_line(out, line, name, sizeof w->name);
        if (quiet) continue;
        if (r == SET_UNKNOWN)   Serial.printf("[settings] %s line %d: unknown setting '%s', ignored\r\n", path, n, name);
        if (r == SET_BAD_VALUE) Serial.printf("[settings] %s line %d: bad value for '%s', ignored\r\n", path, n, name);
        if (r == SET_SYNTAX)    Serial.printf("[settings] %s line %d: expected 'name = value', ignored\r\n", path, n);
    }
    f_close(&f);
    free(w);
    return true;
}

// PIZERO-154: apply one settings-format file on top of *out, as it stands.
// Returns false when there is no such file (and *out is unchanged).
extern "C" bool coco_boot_apply_settings_file(const char *path, struct coco_settings *out) {
    return apply_settings_file(path, out, false);
}

// PIZERO-242: the settings in force, in layers: the machine profile, then
// settings.txt, then the game's own file (game_file, or NULL). The machine
// is the last machine line of the two files, so it is found first, quietly.
// *game_found says whether game_file existed. Returns whether settings.txt did.
extern "C" bool coco_boot_load_settings_for(const char *game_file, struct coco_settings *out,
                                            bool *game_found) {
    static struct coco_settings t;
    settings_defaults(&t);
    apply_settings_file("0:/coco/settings.txt", &t, true);
    if (game_file) apply_settings_file(game_file, &t, true);
    settings_defaults(out);
    settings_profile(out, t.machine);
    bool have = apply_settings_file("0:/coco/settings.txt", out, false);
    bool game = game_file && apply_settings_file(game_file, out, false);
    if (game_found) *game_found = game;
    return have;
}

// PIZERO-201: the clock. The RP2350 has no RTC and this board no backup
// cell; the power manager's always-on timer runs while powered and through
// a watchdog reboot, and the SD library stamps files from it (get_fattime).
// At power-on it is seeded from the newest file time on the card, which the
// Mac wrote with real time, so the date is right to within "when the card
// was last touched" and the board's own files keep it monotonic.
static char g_clock_src[24] = "none";

static time_t fat_to_time(const FILINFO *fi) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = (fi->fdate >> 9) + 80;
    t.tm_mon  = ((fi->fdate >> 5) & 15) - 1;
    t.tm_mday = fi->fdate & 31;
    t.tm_hour = fi->ftime >> 11;
    t.tm_min  = (fi->ftime >> 5) & 63;
    t.tm_sec  = (fi->ftime & 31) * 2;
    return mktime(&t);
}

extern "C" void coco_boot_clock_init(void) {
    // A running timer with a real date is a reboot, not a power-on: keep it,
    // unless a file on the card is newer than it says, which proves it wrong
    // (the core may also start the timer itself at boot with no date in it).
    struct timespec now = { 0, 0 };
    bool running = aon_timer_is_running();
    if (running) aon_timer_get_time(&now);
    // The newest file time anywhere the Mac writes: the disks, programs,
    // cartridges and ROMs it copied carry its clock, and the board's own
    // files carry this clock once it has been set.
    time_t best = 0;
    char best_name[80] = "";
    const char *dirs[] = { "0:/coco", "0:/coco/dsk", "0:/coco/bin", "0:/coco/cart", "0:/coco/roms", "0:/coco/log" };
    FILINFO fi;
    DIR d;
    for (const char *dir : dirs) {
        if (f_opendir(&d, dir) != FR_OK) continue;
        time_t dir_best = 0; char dir_name[40] = "";
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            if (fi.fattrib & AM_DIR) continue;
            time_t t = fat_to_time(&fi);
            if (t > dir_best) { dir_best = t; snprintf(dir_name, sizeof dir_name, "%s", fi.fname); }
            if (t > best) { best = t; snprintf(best_name, sizeof best_name, "%s/%s", dir, fi.fname); }
        }
        f_closedir(&d);
        if (dir_best) {
            char txt[40]; struct tm bt; localtime_r(&dir_best, &bt);
            strftime(txt, sizeof txt, "%Y-%m-%d %H:%M:%S", &bt);
            Serial.printf("[clock]   newest in %s: %s %s\r\n", dir, dir_name, txt);
        }
    }
    {   // PIZERO-201: name the newest file, so a wrong clock can be traced to it
        char txt[40]; struct tm bt; localtime_r(&best, &bt);
        strftime(txt, sizeof txt, "%Y-%m-%d %H:%M:%S", &bt);
        Serial.printf("[clock] newest file on the card: %s, %s\r\n", best_name[0] ? best_name : "none", txt);
    }
    if (running && now.tv_sec >= 1700000000 && now.tv_sec >= best) {
        strcpy(g_clock_src, "kept");
        char txt[40]; struct tm t; localtime_r(&now.tv_sec, &t);
        strftime(txt, sizeof txt, "%Y-%m-%d %H:%M:%S", &t);
        Serial.printf("[clock] kept across the reboot: %s\r\n", txt);
        return;
    }
    struct tm t;
    memset(&t, 0, sizeof t);
    if (best < 1700000000) {                                 // before 2023-11: no usable file time
        // 2000-01-01 is below the threshold, so a later boot that finds a
        // real file time still takes it, and files written meanwhile are
        // never mistaken for a seed.
        t.tm_year = 2000 - 1900; t.tm_mday = 1;
        strcpy(g_clock_src, "unset");
    } else {
        best += 60;                                          // later than the newest file
        localtime_r(&best, &t);
        strcpy(g_clock_src, "card");
    }
    if (running) aon_timer_set_time_calendar(&t); else aon_timer_start_calendar(&t);
    char txt[40];
    strftime(txt, sizeof txt, "%Y-%m-%d %H:%M:%S", &t);
    if (best < 1700000000) Serial.printf("[clock] no file time on the card: %s, uptime only\r\n", txt);
    else Serial.printf("[clock] set from the card's newest file: %s (approximate)\r\n", txt);
}

extern "C" bool coco_boot_clock_text(char *out, size_t n, char *src, size_t src_n) {
    if (src) snprintf(src, src_n, "%s", g_clock_src);
    struct tm t;
    if (!aon_timer_is_running() || !aon_timer_get_time_calendar(&t)) { snprintf(out, n, "no clock"); return false; }
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}

// PIZERO-146: text files the on-screen editor opens and saves. Load reads up
// to max bytes; it returns false (with *len = 0) when there is no file.
// PIZERO-296: a file longer than max is refused, not cut short. Truncating
// it let a save (the editor, F8, the volume keys) write back only the first
// max bytes and lose the rest.
extern "C" int coco_boot_read_text(const char *path, char *buf, uint32_t max, uint32_t *len) {
    *len = 0;
    FIL f;
    FRESULT fo = f_open(&f, path, FA_READ);
    if (fo == FR_NO_FILE || fo == FR_NO_PATH) return COCO_TEXT_MISSING;
    if (fo != FR_OK) return COCO_TEXT_ERROR;
    if (f_size(&f) > max) {
        Serial.printf("[text] %s: %lu bytes, over the %lu limit, not read\r\n", path,
                      (unsigned long)f_size(&f), (unsigned long)max);
        f_close(&f);
        return COCO_TEXT_TOO_BIG;
    }
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, max, &br);
    f_close(&f);
    if (fr != FR_OK) return COCO_TEXT_ERROR;
    *len = br;
    return COCO_TEXT_OK;
}

extern "C" bool coco_boot_load_text(const char *path, char *buf, uint32_t max, uint32_t *len) {
    return coco_boot_read_text(path, buf, max, len) == COCO_TEXT_OK;
}

// PIZERO-309: read the text file `name` ("README.TXT:1", as listed) from the
// disk in drive d into buf, following its granule chain through the FDC's
// own sector read. COCO_TEXT_OK, _TOO_BIG (*len the true size), _MISSING or
// _ERROR. Working memory on the heap: this runs from the USB key handler.
static int dsk_read_cb(void *ctx, unsigned t, unsigned s, uint8_t out[256]) {
    return coco_boot_disk_read_sector(*(unsigned *)ctx, t, s, out);
}

extern "C" int coco_boot_dsk_read_text(unsigned d, const char *name, char *buf, uint32_t max, uint32_t *len) {
    *len = 0;
    if (d >= COCO_NDRIVE || !coco_boot_drive_path(d)) return COCO_TEXT_MISSING;
    struct { uint8_t sec[256]; uint8_t fat[256]; uint8_t entry[32]; } *w = (decltype(w))malloc(sizeof *w);
    if (!w) return COCO_TEXT_ERROR;
    int rc = COCO_TEXT_MISSING;
    if (coco_boot_disk_read_sector(d, RSDOS_FAT_TRACK, RSDOS_FAT_SECTOR, w->fat) != 0) rc = COCO_TEXT_ERROR;
    for (unsigned s = RSDOS_DIR_FIRST_SEC; s <= RSDOS_DIR_LAST_SEC && rc == COCO_TEXT_MISSING; s++) {
        if (coco_boot_disk_read_sector(d, RSDOS_DIR_TRACK, s, w->sec) != 0) { rc = COCO_TEXT_ERROR; break; }
        for (int k = 0; k < 8; k++) {
            const uint8_t *ent = w->sec + k * 32;
            if (ent[0] == 0xFF) { s = RSDOS_DIR_LAST_SEC; break; }
            char n[16];
            if (!rsdos_entry_name(ent, d, n, sizeof n) || strcmp(n, name)) continue;
            memcpy(w->entry, ent, 32);
            unsigned drive = d;
            int r = rsdos_read_file(dsk_read_cb, &drive, w->fat, w->entry, w->sec, buf, max, len);
            rc = r == RSDOS_OK ? COCO_TEXT_OK : r == RSDOS_TOO_BIG ? COCO_TEXT_TOO_BIG : COCO_TEXT_ERROR;
            break;
        }
    }
    free(w);
    return rc;
}

// PIZERO-296: can the editor open this file without changing what it does
// not show? Too big (over max) or CR line ends (the editor would join its
// lines and save them back as LF, until PIZERO-295) say no. *size is set
// when the file exists.
extern "C" int coco_boot_text_check(const char *path, uint32_t max, uint32_t *size) {
    *size = 0;
    // On the heap, not the stack: this runs from the USB key handler too.
    struct { FIL f; char head[512]; } *w = (decltype(w))malloc(sizeof *w);
    if (!w) return COCO_TEXT_ERROR;
    int rc = COCO_TEXT_OK;
    FRESULT fo = f_open(&w->f, path, FA_READ);
    if (fo == FR_NO_FILE || fo == FR_NO_PATH) rc = COCO_TEXT_MISSING;
    else if (fo != FR_OK) rc = COCO_TEXT_ERROR;
    else {
        *size = (uint32_t)f_size(&w->f);
        UINT br = 0;
        FRESULT fr = f_read(&w->f, w->head, sizeof w->head, &br);
        f_close(&w->f);
        if (fr != FR_OK) rc = COCO_TEXT_ERROR;
        else if (*size > max) rc = COCO_TEXT_TOO_BIG;
        else if (tl_convention(w->head, (int)br) == TL_CR) rc = COCO_TEXT_CR;
    }
    free(w);
    return rc;
}

// Save safely: write the whole text to PATH.tmp, then swap it in for PATH.
// A power cut during the write leaves the old file untouched; one between
// removing the old file and the rename leaves only the .tmp, which
// coco_boot_recover_text() finishes at the next boot.
static void tmp_path_for(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s.tmp", path);
}

// PIZERO-295: eol is what each LF in buf is written as ("\r" for a CoCo
// file, "\r\n" for Windows), or NULL to write buf as it is. The FIL and the
// chunk buffer are on the heap: saves run from the USB key handler, where
// core 0's stack is already deep (PIZERO-296's crash).
extern "C" bool coco_boot_save_text_eol(const char *path, const char *buf, uint32_t len, const char *eol) {
    char tmp[96];
    tmp_path_for(path, tmp, sizeof tmp);
    struct { FIL f; char out[256]; } *w = (decltype(w))malloc(sizeof *w);
    if (!w) { Serial.printf("[save] %s: no memory\r\n", path); return false; }
    FRESULT fr = f_open(&w->f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { Serial.printf("[save] %s: open failed (%d)\r\n", tmp, fr); free(w); return false; }
    uint32_t written = 0;
    bool ok = true;
    for (int pos = 0; ok && pos < (int)len;) {
        int n = tl_out_chunk(buf, (int)len, &pos, eol, w->out, (int)sizeof w->out);
        UINT bw = 0;
        ok = f_write(&w->f, w->out, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
        written += bw;
    }
    FRESULT fc = f_close(&w->f);              // close flushes to the card
    free(w);
    if (!ok || fc != FR_OK) {
        Serial.printf("[save] %s: write failed (%d, %lu bytes written)\r\n", tmp, fc, (unsigned long)written);
        f_unlink(tmp);
        return false;
    }
    len = written;
    fr = f_unlink(path);
    if (fr != FR_OK && fr != FR_NO_FILE) { Serial.printf("[save] %s: remove old failed (%d)\r\n", path, fr); return false; }
    fr = f_rename(tmp, path);
    if (fr != FR_OK) { Serial.printf("[save] %s: rename failed (%d)\r\n", path, fr); return false; }
    Serial.printf("[save] %s: %lu bytes%s\r\n", path, (unsigned long)len,
                  eol && eol[0] == '\r' ? (eol[1] ? " (CRLF)" : " (CR)") : "");
    return true;
}

extern "C" bool coco_boot_save_text(const char *path, const char *buf, uint32_t len) {
    return coco_boot_save_text_eol(path, buf, len, nullptr);
}

// PIZERO-165: save the screen as the next /coco/screendumps/SCRnnnn.PNG.
// Written straight through FatFS a 512-byte buffer at a time, so it needs no
// big buffer; the caller pauses the machine while it runs (about 77 KB).
#define SHOT_DIR "0:/coco/screendumps"
struct shot_ctx {
    FIL      f;
    uint8_t  buf[512];
    uint16_t used;
    bool     failed;
    const uint16_t *fb;
    const uint8_t  *fb8;               // PIZERO-134: palette indices, rows saved twice
    int      w;
    uint16_t pal[256];
    int      npal;
};

static void shot_flush(struct shot_ctx *c) {
    if (!c->used || c->failed) { c->used = 0; return; }
    UINT bw = 0;
    if (f_write(&c->f, c->buf, c->used, &bw) != FR_OK || bw != c->used) c->failed = true;
    c->used = 0;
    watchdog_update();                         // a slow card must not look like a hang
}

static void shot_sink(void *ctx, const uint8_t *p, size_t n) {
    struct shot_ctx *c = (struct shot_ctx *)ctx;
    while (n) {
        size_t k = sizeof c->buf - c->used;
        if (k > n) k = n;
        memcpy(c->buf + c->used, p, k);
        c->used = (uint16_t)(c->used + k);
        p += k; n -= k;
        if (c->used == sizeof c->buf) shot_flush(c);
    }
}

static void shot_row(void *ctx, int y, uint8_t *out) {
    struct shot_ctx *c = (struct shot_ctx *)ctx;
    if (c->fb8) { memcpy(out, c->fb8 + (size_t)(y / 2) * (size_t)c->w, (size_t)c->w); return; }
    const uint16_t *px = c->fb + (size_t)y * (size_t)c->w;
    for (int x = 0; x < c->w; x++) {
        int i = png_palette_index(c->pal, &c->npal, 256, px[x]);
        out[x] = (uint8_t)(i < 0 ? 0 : i);
    }
}

static bool shot_save(const uint16_t *fb, const uint8_t *fb8, const uint16_t *pal565,
                      int w, int h, char *name, size_t name_sz) {
    FRESULT fr = f_mkdir(SHOT_DIR);
    if (fr != FR_OK && fr != FR_EXIST) { Serial.printf("[shot] %s: cannot create (%d)\r\n", SHOT_DIR, fr); return false; }
    char path[64];
    FILINFO fi;
    unsigned k;
    for (k = 1; k <= 9999; k++) {
        snprintf(path, sizeof path, SHOT_DIR "/SCR%04u.PNG", k);
        if (f_stat(path, &fi) != FR_OK) break;
    }
    if (k > 9999) { Serial.printf("[shot] %s is full\r\n", SHOT_DIR); return false; }
    // The FIL and buffers (about 2 KB) come from the heap only while saving:
    // the double-buffered builds have no static RAM to spare.
    struct shot_ctx *cp = (struct shot_ctx *)malloc(sizeof *cp);
    uint8_t *row = (uint8_t *)malloc((size_t)w + 256 * 3);
    if (!cp || !row) { free(cp); free(row); Serial.printf("[shot] no memory\r\n"); return false; }
    struct shot_ctx &c = *cp;
    uint8_t *pal_rgb = row + w;
    // The palette first, so the PLTE chunk can go ahead of the pixels.
    c.npal = 0;
    if (fb8) {
        // The indices are the PNG's own palette, so rows go out as they are.
        c.npal = 256;
        for (int i = 0; i < 256; i++) png_rgb565_to_888(pal565[i], &pal_rgb[i * 3]);
    } else {
        for (int i = 0; i < w * h; i++) png_palette_index(c.pal, &c.npal, 256, fb[i]);
        for (int i = 0; i < c.npal; i++) png_rgb565_to_888(c.pal[i], &pal_rgb[i * 3]);
    }
    fr = f_open(&c.f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        Serial.printf("[shot] %s: open failed (%d)\r\n", path, fr);
        free(cp); free(row);
        return false;
    }
    c.used = 0; c.failed = false; c.fb = fb; c.fb8 = fb8; c.w = w;
    uint32_t t0 = millis();
    size_t n = png_write_indexed(shot_sink, &c, w, h, pal_rgb, c.npal ? c.npal : 1,
                                 shot_row, &c, row);
    shot_flush(&c);
    FRESULT fc = f_close(&c.f);
    bool ok = !c.failed && fc == FR_OK;
    unsigned npal = (unsigned)c.npal;
    free(cp); free(row);
    if (!ok) {
        Serial.printf("[shot] %s: write failed\r\n", path);
        f_unlink(path);
        return false;
    }
    Serial.printf("[shot] %s: %lu bytes, %u colors, %lu ms\r\n", path, (unsigned long)n,
                  npal, (unsigned long)(millis() - t0));
    if (name) snprintf(name, name_sz, "%s", path);
    return true;
}

extern "C" bool coco_boot_screenshot(const uint16_t *fb, int w, int h, char *name, size_t name_sz) {
    return shot_save(fb, nullptr, nullptr, w, h, name, name_sz);
}

extern "C" bool coco_boot_screenshot8(const uint8_t *fb, const uint16_t *pal565, int w, int h,
                                      char *name, size_t name_sz) {
    return shot_save(nullptr, fb, pal565, w, 2 * h, name, name_sz);
}

// Finish a save a power cut interrupted: PATH gone, PATH.tmp present.
extern "C" void coco_boot_recover_text(const char *path) {
    char tmp[96];
    tmp_path_for(path, tmp, sizeof tmp);
    FILINFO fi;
    if (f_stat(path, &fi) == FR_OK || f_stat(tmp, &fi) != FR_OK) return;
    if (f_rename(tmp, path) == FR_OK)
        Serial.printf("[save] recovered %s from an interrupted save\r\n", path);
}

// PIZERO-92: the caller needs to tell a MISSING ROM from a DAMAGED one, because
// the advice differs and "NO ROM FOUND" while the file is sitting on the card
// sends someone hunting for something they already have.
static struct coco_rom_status g_rom_status;

extern "C" const struct coco_rom_status *coco_boot_rom_status(void) {
    return &g_rom_status;
}

extern "C" bool coco_boot_load_rom_from_sd(uint8_t *rom16k) {
    memset(rom16k, 0xFF, 16384);
    char path[80];
    size_t n_ecb = 0, n_bas = 0;
    g_rom_status = (struct coco_rom_status){};
    g_rom_status.bas_found = coco_boot_resolve("rom", "bas12.rom", path, sizeof(path));
    if (g_rom_status.bas_found)
        n_bas = read_rom_file(path, &rom16k[0x2000], 8192);
    g_rom_status.bas_bytes = (uint32_t)n_bas;
    g_rom_status.ecb_found = coco_boot_resolve("rom", "extbas11.rom", path, sizeof(path));
    if (g_rom_status.ecb_found)
        n_ecb = read_rom_file(path, &rom16k[0x0000], 8192);
    g_rom_status.ecb_bytes = (uint32_t)n_ecb;
    if (n_bas != 8192) {
        Serial.printf("[rom] bas12.rom %s (looked in /coco/roms/ then /coco/)\\r\\n",
                      g_rom_status.bas_found ? "is the wrong size" : "is required");
        return false;
    }
    if (n_ecb != 8192) {
        Serial.println("[rom] extbas11.rom not found in /coco/roms/ or /coco/, "
                       "running Color BASIC only (no Extended/Disk BASIC)");
        memset(&rom16k[0x0000], 0xFF, 0x2000);
    }
    Serial.printf("[rom] reset vector -> $%02X%02X\n",
                  rom16k[0x3FFE], rom16k[0x3FFF]);
    return true;
}

// PIZERO-133: coco3.rom, from /coco/roms/ then /coco/, exactly 32 KB.
extern "C" bool coco_boot_load_coco3_rom(uint8_t *rom32k) {
    char path[80];
    g_rom_status = (struct coco_rom_status){};
    g_rom_status.c3_found = coco_boot_resolve("rom", "coco3.rom", path, sizeof(path));
    if (g_rom_status.c3_found)
        g_rom_status.c3_bytes = (uint32_t)read_rom_file(path, rom32k, 32768);
    if (g_rom_status.c3_bytes != 32768) {
        Serial.printf("[rom] coco3.rom %s (looked in /coco/roms/ then /coco/)\r\n",
                      g_rom_status.c3_found ? "is the wrong size" : "is required");
        return false;
    }
    Serial.printf("[rom] coco3.rom, reset vector -> $%02X%02X\r\n",
                  rom32k[0x7FFE], rom32k[0x7FFF]);
    return true;
}

// AMOLED-58: cart loader takes a bare filename so autorun.txt can
// pick an alternate cart (e.g. a game .ccc) via @CART.
// PIZERO-142: where a cartridge name lives, in the loader's search order.
extern "C" bool coco_boot_resolve_cart(const char *name, char *out, size_t out_sz) {
    return coco_boot_resolve("cart", name, out, out_sz) ||
           coco_boot_resolve("rom", name, out, out_sz);
}

// PIZERO-142: read exactly len bytes of a file (a banked cart's image).
extern "C" bool coco_boot_load_file(const char *path, uint8_t *buf, uint32_t len) {
    return read_rom_file(path, buf, len) == len;
}

// PIZERO-136/139: load a plain cartridge from an exact path: any size the
// machine maps directly (2, 4, 8 or 16 KB) that fits the buffer. Cartridges
// (.ccc) live in /coco/cart; coco_boot_resolve_cart finds one by name. The
// buffer is only touched once the size is known to be good, so a refused
// file leaves the installed cartridge intact.
extern "C" bool coco_boot_load_cart_path(const char *path, uint8_t *buf,
                                         uint32_t max, uint32_t *len) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK || !cat_cart_size_ok((unsigned long)fi.fsize, max)) {
        Serial.printf("[cart] %s: not a 2/4/8/16 KB cart that fits %lu bytes\n",
                      path, (unsigned long)max);
        return false;
    }
    size_t n = read_rom_file(path, buf, (size_t)fi.fsize);
    if (n != fi.fsize) return false;
    if (len) *len = (uint32_t)n;
    return true;
}

// AMOLED-58: autorun.txt parser. Tiny state-machine: skips blanks and
// '#' comments, recognises '@DIRECTIVE arg' lines, treats the rest as
// autotype. See AUTORUN.md for the spec.
static char *trim_leading(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
}

static bool prefix_eq(const char *line, const char *kw, const char **arg_out) {
    size_t kl = strlen(kw);
    if (strncasecmp(line, kw, kl) != 0) return false;
    if (line[kl] != ' ' && line[kl] != '\t') return false;
    const char *p = line + kl;
    while (*p == ' ' || *p == '\t') p++;
    *arg_out = p;
    return true;
}

extern "C" bool coco_boot_load_autorun(struct coco_autorun *out) {
    memset(out, 0, sizeof(*out));
    FIL f;
    if (f_open(&f, "0:/coco/autorun.txt", FA_READ) != FR_OK) return false;

    char line[260];
    size_t at_used = 0;
    struct tl_reader rd;                         // PIZERO-294
    tl_reader_init(&rd, fil_read, &f);
    while (tl_gets(&rd, line, sizeof line)) {
        char *p = trim_leading(line);
        trim_trailing(p);
        if (*p == '\0' || *p == '#') continue;
        if (*p == '@') {
            char *body = p + 1;
            const char *arg;
            if (prefix_eq(body, "DISK", &arg)) {
                // PIZERO-183: drive 0 is the remembered drive now.
                Serial.printf("[autorun] @DISK is no longer used, ignored: %s\n", arg);
            } else if (prefix_eq(body, "CART", &arg)) {
                strncpy(out->cart_name, arg, sizeof(out->cart_name) - 1);
            } else if (prefix_eq(body, "DIRECT", &arg)) {
                strncpy(out->direct_name, arg, sizeof(out->direct_name) - 1);
            } else {
                Serial.printf("[autorun] unknown directive: %s\n", body);
            }
            continue;
        }
        // Autotype line: append + '\r'. Truncate if buffer is full.
        size_t ll = strlen(p);
        if (at_used + ll + 1 < sizeof(out->autotype)) {
            memcpy(&out->autotype[at_used], p, ll);
            at_used += ll;
            out->autotype[at_used++] = '\r';
            out->autotype[at_used] = '\0';
        } else {
            Serial.println("[autorun] autotype buffer full, truncating");
        }
    }
    f_close(&f);
    Serial.printf("[autorun] parsed: cart='%s' direct='%s' autotype=%u bytes\n",
                  out->cart_name, out->direct_name,
                  (unsigned)at_used);
    return true;
}

// Disk images, one per drive (PIZERO-114). CoCo standard: 35 tracks,
// 18 sectors/track, 256 bytes/sector, single-sided = 161 280 bytes.
// Sector numbers from BASIC/DECB are 1-based; track is 0-based.
//
// The data stays on the card and is read and written a sector at a time:
// there is no PSRAM here and one image is larger than all free RAM. Both
// directions run synchronously on core 0 inside the FDC command, while the
// emulated CPU is halted waiting on the drive (PIZERO-66); core 1 only scans
// out video, so nothing else touches the card. A write changes no FAT chain
// (the image is already full size), so a power cut mid-write can lose one
// sector but not the filesystem. Mounting and ejecting happen from the F12
// overlay while the machine is paused, so they never race a transfer.
// (COCO_NDRIVE, the four drives, is defined at the top: the FILES scan uses it too.)
static FIL  g_dsk_file[COCO_NDRIVE];
static bool g_dsk_open[COCO_NDRIVE];
// Per-drive flags, one byte each (pizero_wavmeas has no RAM to spare).
#define DSK_RW    0x01                    // opened writable (else the file refused)
#define DSK_WP    0x02                    // the user's write-protect tab
#define DSK_DIRTY 0x04                    // written since the last f_sync
#define DSK_DMK   0x08                    // a DMK image (PIZERO-320), not a JVC .DSK
static uint8_t g_dsk_flags[COCO_NDRIVE];
#define g_dsk_rw(d)    ((g_dsk_flags[d] & DSK_RW) != 0)
#define g_dsk_wp(d)    ((g_dsk_flags[d] & DSK_WP) != 0)
#define g_dsk_dirty(d) ((g_dsk_flags[d] & DSK_DIRTY) != 0)
#define g_dsk_dmk(d)   ((g_dsk_flags[d] & DSK_DMK) != 0)
static char g_dsk_path[COCO_NDRIVE][96];

// PIZERO-320: a DMK drive's geometry, and the sectors of the track it last
// read (about 80 bytes a drive), rebuilt from the track's ID fields when
// the head moves. Side 0 only: Disk BASIC is single-sided.
static struct dmk_geom g_dmk_geom[COCO_NDRIVE];
static struct dmk_map  g_dmk_map[COCO_NDRIVE];

static unsigned dmk_fat_read(void *ctx, uint32_t off, uint8_t *buf, unsigned n) {
    FIL *f = (FIL *)ctx;
    UINT br = 0;
    if (f_lseek(f, (FSIZE_t)off) != FR_OK || f_read(f, buf, n, &br) != FR_OK) return 0;
    return (unsigned)br;
}

static bool path_is_dmk(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && !strcasecmp(ext, ".dmk");
}

// PIZERO-66: write timing, printed when the drive syncs, so the worst case
// over a SAVE is on the serial log. The ~90 ms audio ring is the budget.
static struct { uint32_t n, us_max, us_total; } g_dskw_stat[COCO_NDRIVE];
static uint32_t g_dskw_last_ms;           // when the last sector was written

// PIZERO-183: /coco/drives.txt, one line per drive, "N = path" ("N =" for
// an empty drive; "N ro = path" for a write-protected one, PIZERO-66).
// Written by the board, so it is plain and fixed; read back leniently.
// g_drives_saved is what the file holds, so an unchanged state is never
// rewritten; g_drives_quiet holds saving off during the restore.
#define DRIVES_PATH "0:/coco/drives.txt"
static uint32_t g_drives_saved_hash;      // FNV-1a of the file's text (PIZERO-201: a copy was 416 bytes)
static bool g_drives_quiet = false;
static int  g_drives_eol = TL_NONE;       // PIZERO-294: the line ends the file had (LF if new)

static uint32_t text_hash(const char *t) {
    uint32_t h = 2166136261u;
    for (; *t; t++) { h ^= (uint8_t)*t; h *= 16777619u; }
    return h;
}

static void drives_save(void) {
    if (g_drives_quiet) return;
    // PIZERO-324: the text on the heap, not the stack: the F12 list mounts,
    // ejects and renames from inside the USB key handler.
    const size_t cap = COCO_NDRIVE * 104;
    char *text = (char *)malloc(cap);
    if (!text) { Serial.print("[dsk] drives not saved: no memory\n"); return; }
    int k = 0;
    for (unsigned d = 0; d < COCO_NDRIVE; d++)
        k += snprintf(text + k, cap - k, "%u%s = %s%s", d,
                      (g_dsk_open[d] && g_dsk_wp(d)) ? " ro" : "",
                      g_dsk_open[d] ? g_dsk_path[d] : "", tl_eol(g_drives_eol));
    if (text_hash(text) != g_drives_saved_hash && coco_boot_save_text(DRIVES_PATH, text, (uint32_t)k)) {
        g_drives_saved_hash = text_hash(text);
        Serial.print("[dsk] drives saved\n");
    }
    free(text);
}

// Push a drive's pending writes to the card, timed and logged with the
// write statistics gathered since the last sync.
static void dsk_sync(unsigned d) {
    if (!g_dsk_open[d] || !g_dsk_dirty(d)) return;
    uint32_t t0 = micros();
    FRESULT fr = f_sync(&g_dsk_file[d]);
    uint32_t us = micros() - t0;
    watchdog_update();
    g_dsk_flags[d] &= (uint8_t)~DSK_DIRTY;
    Serial.printf("[dsk] drive %u synced: %lu writes, max %lu us, mean %lu us, sync %lu us%s\n",
                  d, (unsigned long)g_dskw_stat[d].n, (unsigned long)g_dskw_stat[d].us_max,
                  (unsigned long)(g_dskw_stat[d].n ? g_dskw_stat[d].us_total / g_dskw_stat[d].n : 0),
                  (unsigned long)us, fr == FR_OK ? "" : " FAILED");
    memset(&g_dskw_stat[d], 0, sizeof g_dskw_stat[d]);
}

extern "C" void coco_boot_flush_drives(void) {
    for (unsigned d = 0; d < COCO_NDRIVE; d++) dsk_sync(d);
}

// Once a frame from the main loop: a drive left dirty for 1.5 s syncs even
// if the guest never turned the motor off (or we never saw it do so).
extern "C" void coco_boot_disk_tick(void) {
    if ((uint32_t)(millis() - g_dskw_last_ms) < 1500) return;
    for (unsigned d = 0; d < COCO_NDRIVE; d++)
        if (g_dsk_dirty(d)) { Serial.printf("[dsk] drive %u idle 1.5 s\n", d); dsk_sync(d); }
}

static void eject_quiet(unsigned drive) {
    if (g_dsk_open[drive]) {
        dsk_sync(drive);
        f_close(&g_dsk_file[drive]);
        Serial.printf("[dsk] drive %u ejected %s\n", drive, g_dsk_path[drive]);
    }
    g_dsk_open[drive] = false;
    g_dsk_flags[drive] = 0;
    g_dsk_path[drive][0] = '\0';
}

extern "C" void coco_boot_eject_drive(unsigned drive) {
    if (drive >= COCO_NDRIVE) return;
    eject_quiet(drive);
    drives_save();
}

extern "C" bool coco_boot_mount_drive(unsigned drive, const char *path) {
    if (drive >= COCO_NDRIVE || !path) return false;
    eject_quiet(drive);
    if (strlen(path) >= sizeof g_dsk_path[drive]) { drives_save(); return false; }
    // Writable when the file allows it; a read-only file (its attribute set
    // on a PC, say) still mounts, and the FDC reports it write-protected.
    FRESULT fr = f_open(&g_dsk_file[drive], path, FA_READ | FA_WRITE);
    bool rw = (fr == FR_OK);
    if (!rw) fr = f_open(&g_dsk_file[drive], path, FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[dsk] drive %u open %s failed: %s (%d)\n",
                      drive, path, FRESULT_str(fr), fr);
        drives_save();
        return false;
    }
    // PIZERO-320: a .DMK, or any image whose header reads as one (DMKs are
    // often passed around named .DSK), finds its sectors by track scan.
    FIL *f = &g_dsk_file[drive];
    uint32_t size = (uint32_t)f_size(f);
    uint8_t hdr[DMK_HEADER];
    bool dmk = false;
    if (dmk_fat_read(f, 0, hdr, sizeof hdr) == sizeof hdr) {
        bool named = path_is_dmk(path);
        if (named || dmk_sniff(hdr, size)) {
            int pr = dmk_parse_header(hdr, size, &g_dmk_geom[drive]);
            if (pr != DMK_OK) {
                Serial.printf("[dsk] drive %u: %s is not a DMK this reads (%d)\n", drive, path, pr);
                f_close(f);
                drives_save();
                return false;
            }
            dmk = true;
            g_dmk_map[drive].n_ok = 0;
            if (g_dmk_geom[drive].protect) rw = false;   // the header says read-only
        }
    }
    g_dsk_open[drive] = true;
    g_dsk_flags[drive] = (uint8_t)((rw ? DSK_RW : 0) | (dmk ? DSK_DMK : 0));
    strcpy(g_dsk_path[drive], path);
    if (dmk)
        Serial.printf("[dsk] drive %u mounted %s (DMK, %u tracks, %u side%s, track %u bytes%s)\n",
                      drive, path, g_dmk_geom[drive].tracks, g_dmk_geom[drive].sides,
                      g_dmk_geom[drive].sides == 1 ? "" : "s", g_dmk_geom[drive].track_len,
                      rw ? "" : ", read-only");
    else
        Serial.printf("[dsk] drive %u mounted %s (%lu bytes%s)\n",
                      drive, path, (unsigned long)size, rw ? "" : ", read-only file");
    drives_save();
    return true;
}

extern "C" bool coco_boot_drive_protected(unsigned drive) {
    return drive < COCO_NDRIVE && g_dsk_open[drive] && (g_dsk_wp(drive) || !g_dsk_rw(drive));
}

extern "C" bool coco_boot_set_drive_protected(unsigned drive, bool on) {
    if (drive >= COCO_NDRIVE || !g_dsk_open[drive]) return false;
    if (!g_dsk_rw(drive)) return false;            // the file decides, not the tab
    if (g_dsk_wp(drive) != on) {
        if (on) g_dsk_flags[drive] |= DSK_WP; else g_dsk_flags[drive] &= (uint8_t)~DSK_WP;
        if (on) dsk_sync(drive);
        drives_save();
    }
    return true;
}

extern "C" bool coco_boot_restore_drives(char *path0, size_t path0_sz) {
    coco_boot_recover_text(DRIVES_PATH);
    char text[COCO_NDRIVE * 104];
    uint32_t n = 0;
    if (!coco_boot_load_text(DRIVES_PATH, text, sizeof text - 1, &n)) return false;
    text[n] = '\0';
    // What the file says is what is saved, even where a disk is now missing,
    // so the next real change rewrites it rather than a failed mount here.
    g_drives_saved_hash = text_hash(text);
    g_drives_eol = tl_convention(text, (int)n);   // PIZERO-294: kept when rewritten
    g_drives_quiet = true;
    for (int pos = 0, ls, ll; pos < (int)n;) {
        pos = tl_next(text, (int)n, pos, &ls, &ll);   // CR, LF or CRLF
        char *line = text + ls;
        line[ll] = '\0';                               // the line end, or the final NUL
        char *eq = strchr(line, '=');
        if (!eq || line[0] < '0' || line[0] > '3') continue;
        unsigned d = (unsigned)(line[0] - '0');
        bool ro = strstr(line, "ro") && strstr(line, "ro") < eq;   // "N ro = path"
        char *p = eq + 1;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = '\0';
        if (!*p) continue;
        if (!coco_boot_mount_drive(d, p))
            Serial.printf("[dsk] drive %u: remembered %s is gone\n", d, p);
        else if (ro)
            coco_boot_set_drive_protected(d, true);
    }
    g_drives_quiet = false;
    const char *p0 = coco_boot_drive_path(0);
    if (p0) snprintf(path0, path0_sz, "%s", p0);
    return p0 != nullptr;
}

extern "C" const char *coco_boot_drive_path(unsigned drive) {
    return (drive < COCO_NDRIVE && g_dsk_open[drive]) ? g_dsk_path[drive] : nullptr;
}

// Boot: the default disk goes to drive 0 when nothing is remembered.
extern "C" bool coco_boot_attach_dsk(const char *path) {
    return coco_boot_mount_drive(0, path);
}

// PIZERO-193: a new blank disk, /coco/dsk/NAME.DSK. 35 tracks x 18 sectors
// x 256 bytes of $FF is exactly what DSKINI leaves: every FAT granule free,
// every directory entry unused. Written as .tmp and renamed, so a power cut
// never leaves a short image under the real name. The machine is paused
// under the overlay, so the ~160 KB write may take its time; the watchdog
// is fed per block. Returns 0, or COCO_NEWDSK_EXISTS / COCO_NEWDSK_FAILED.
extern "C" int coco_boot_create_blank_dsk(const char *base, char *name, size_t name_sz) {
    const char *dir = cat_kind_dir(CAT_DSK);
    FRESULT fr = f_mkdir(dir);
    if (fr != FR_OK && fr != FR_EXIST) {
        Serial.printf("[dsk] new: mkdir %s failed (%d)\n", dir, fr);
        return COCO_NEWDSK_FAILED;
    }
    // PIZERO-324: working memory on the heap (about 1.4 KB), only while the
    // disk is written: this runs inside the F12 key handler, under the USB
    // task, and on the stack it took core 0 to about 3,800 of its 4,096.
    struct { FIL f; FILINFO fi; char path[96], tmp[100]; uint8_t blank[512]; } *w =
        (decltype(w))malloc(sizeof *w);
    if (!w) { Serial.printf("[dsk] new: no memory\n"); return COCO_NEWDSK_FAILED; }
    char *path = w->path, *tmp = w->tmp;
    uint8_t *blank = w->blank;
    FIL &f = w->f;
    snprintf(name, name_sz, "%s.DSK", base);
    snprintf(path, sizeof w->path, "%s/%s", dir, name);
    if (f_stat(path, &w->fi) == FR_OK) { free(w); return COCO_NEWDSK_EXISTS; }
    snprintf(tmp, sizeof w->tmp, "%s.tmp", path);
    fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        Serial.printf("[dsk] new: open %s failed (%d)\n", tmp, fr);
        free(w);
        return COCO_NEWDSK_FAILED;
    }
    memset(blank, 0xFF, sizeof w->blank);
    uint32_t t0 = micros();
    bool ok = true;
    for (int i = 0; i < 161280 / 512 && ok; i++) {
        UINT bw = 0;
        ok = f_write(&f, blank, sizeof w->blank, &bw) == FR_OK && bw == sizeof w->blank;
        watchdog_update();
    }
    if (f_close(&f) != FR_OK) ok = false;
    if (ok) ok = f_rename(tmp, path) == FR_OK;
    if (!ok) {
        Serial.printf("[dsk] new: writing %s failed\n", tmp);
        f_unlink(tmp);
        free(w);
        return COCO_NEWDSK_FAILED;
    }
    Serial.printf("[dsk] new: %s (%lu us)\n", path, (unsigned long)(micros() - t0));
    free(w);
    return 0;
}

// PIZERO-196: rename a disk in place, in its own directory, keeping its
// extension (.DSK, or .DMK since PIZERO-320).
// A disk in a drive is ejected from every drive holding it, renamed, and
// put back with its write-protect tab, so the handle and drives.txt stay
// right. Returns 0, COCO_NEWDSK_EXISTS or COCO_NEWDSK_FAILED.
extern "C" int coco_boot_rename_dsk(const char *old_path, const char *base,
                                    char *name, size_t name_sz) {
    const char *slash = strrchr(old_path, '/');
    if (!slash) return COCO_NEWDSK_FAILED;
    char path[96];
    snprintf(name, name_sz, "%s.%s", base, path_is_dmk(old_path) ? "DMK" : "DSK");
    int len = snprintf(path, sizeof path, "%.*s/%s", (int)(slash - old_path), old_path, name);
    if (len <= 0 || (size_t)len >= sizeof path) return COCO_NEWDSK_FAILED;
    if (!strcmp(path, old_path)) return 0;
    FILINFO fi;
    if (strcasecmp(path, old_path) && f_stat(path, &fi) == FR_OK) return COCO_NEWDSK_EXISTS;
    bool held[COCO_NDRIVE], wp[COCO_NDRIVE];
    for (unsigned d = 0; d < COCO_NDRIVE; d++) {
        held[d] = g_dsk_open[d] && !strcmp(g_dsk_path[d], old_path);
        wp[d] = held[d] && g_dsk_wp(d);
        if (held[d]) eject_quiet(d);
    }
    FRESULT fr = f_rename(old_path, path);
    const char *back = (fr == FR_OK) ? path : old_path;
    if (fr != FR_OK) Serial.printf("[dsk] rename %s -> %s failed (%d)\n", old_path, path, fr);
    else Serial.printf("[dsk] renamed %s -> %s\n", old_path, path);
    g_drives_quiet = true;
    for (unsigned d = 0; d < COCO_NDRIVE; d++)
        if (held[d] && coco_boot_mount_drive(d, back) && wp[d]) coco_boot_set_drive_protected(d, true);
    g_drives_quiet = false;
    drives_save();
    return fr == FR_OK ? 0 : COCO_NEWDSK_FAILED;
}

// PIZERO-320: a DMK sector's data offset, from the track's map (rebuilt when
// the track changes), or -1 when the track has no such sector.
static int32_t dmk_offset(const char *what, unsigned drive, unsigned track,
                          unsigned sector, int *rc, uint8_t *mark) {
    struct dmk_map *m = &g_dmk_map[drive];
    if (!m->n_ok || m->track != track) {
        int n = dmk_build_map(&g_dmk_geom[drive], track, 0, dmk_fat_read, &g_dsk_file[drive], m);
        if (n < 0)
            Serial.printf("[dsk] dmk d=%u t=%u: off the disk or unreadable\n", drive, track);
        else if (n == 0)
            Serial.printf("[dsk] dmk d=%u t=%u: no 256-byte DD sectors\n", drive, track);
    }
    uint32_t off = m->n_ok ? dmk_map_find(&g_dmk_geom[drive], m, sector, mark) : 0;
    if (!off) {
        Serial.printf("[dsk] reject %s d=%u t=%u s=%u (not on the DMK track)\n", what, drive, track, sector);
        *rc = 1;
        return -1;
    }
    *rc = 0;
    return (int32_t)off;
}

// A sector's offset in the image, or -1 when the drive is empty or the
// address is off the 35-track, 18-sector, single-sided disk (PIZERO-68
// covers more). *rc is the FDC's answer in that case. For a DMK, *mark
// (when asked for) is the data mark the sector has.
static int32_t dsk_offset(const char *what, unsigned drive, unsigned track,
                          unsigned sector, int *rc, uint8_t *mark = nullptr) {
    if (drive >= COCO_NDRIVE || !g_dsk_open[drive]) {
        *rc = COCO_DISK_NOT_READY;           // empty drive, as a real one reports
        return -1;
    }
    if (g_dsk_dmk(drive)) return dmk_offset(what, drive, track, sector, rc, mark);
    if (track > 34 || sector < 1 || sector > 18) {
        Serial.printf("[dsk] reject %s d=%u t=%u s=%u (range)\n", what, drive, track, sector);
        *rc = 1;
        return -1;
    }
    *rc = 0;
    return (int32_t)(((uint32_t)track * 18 + (sector - 1)) * 256);
}

// PIZERO-66: the FDC's write. Probed with in256 == NULL when the command
// arrives, then given the sector. Synchronous: measured, not deferred (see
// the note above the drive table; PIZERO-65 is the queue if the numbers
// ever say so). A write of over 50 ms gets its own line.
extern "C" int coco_boot_disk_write_sector(unsigned drive, unsigned track,
                                           unsigned sector, const uint8_t *in256) {
    int rc;
    uint8_t mark = 0xFB;
    int32_t off = dsk_offset("write", drive, track, sector, &rc, &mark);
    if (off < 0) return rc;
    if (g_dsk_wp(drive) || !g_dsk_rw(drive)) return COCO_DISK_WRITE_PROTECT;
    if (!in256) return 0;                    // the probe: this drive takes writes
    FIL *f = &g_dsk_file[drive];
    uint32_t t0 = micros();
    UINT bw = 0;
    FRESULT fr = FR_OK;
    if (g_dsk_dmk(drive) && mark != 0xFB) {
        // PIZERO-320: Write Sector puts down a normal $FB mark, so a deleted
        // sector stops being one; the cached map is rebuilt to say so.
        static const uint8_t fb = 0xFB;
        UINT b1 = 0;
        fr = f_lseek(f, (FSIZE_t)(off - 1));
        if (fr == FR_OK) fr = f_write(f, &fb, 1, &b1);
        g_dmk_map[drive].n_ok = 0;
    }
    if (fr == FR_OK) fr = f_lseek(f, (FSIZE_t)off);
    if (fr == FR_OK) fr = f_write(f, in256, 256, &bw);
    if (fr == FR_OK && bw == 256 && g_dsk_dmk(drive)) {
        // The data field's CRC, right after it, or a real controller (and
        // XRoar) would read the sector back as a CRC error.
        uint16_t crc = dmk_data_crc(0xFB, in256, 256);
        uint8_t c[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };
        UINT b2 = 0;
        fr = f_write(f, c, 2, &b2);
        if (fr == FR_OK && b2 != 2) bw = 0;
    }
    uint32_t us = micros() - t0;
    watchdog_update();                       // a slow card must not look like a hang
    g_dsk_flags[drive] |= DSK_DIRTY;
    g_dskw_last_ms = millis();
    if (g_dskw_stat[drive].n < 3)            // the first few of a burst, for the log
        Serial.printf("[dsk] write d=%u t=%u s=%u: %lu us\n", drive, track, sector, (unsigned long)us);
    g_dskw_stat[drive].n++;
    g_dskw_stat[drive].us_total += us;
    if (us > g_dskw_stat[drive].us_max) g_dskw_stat[drive].us_max = us;
    if (fr != FR_OK || bw != 256) {
        Serial.printf("[dsk] write d=%u t=%u s=%u failed: fr=%d bw=%u\n",
                      drive, track, sector, fr, (unsigned)bw);
        return 1;
    }
    if (us > 50000)
        Serial.printf("[dsk] slow write d=%u t=%u s=%u: %lu us\n", drive, track, sector,
                      (unsigned long)us);
    return 0;
}

extern "C" int coco_boot_disk_read_sector(unsigned drive, unsigned track,
                                          unsigned sector, uint8_t *out256) {
    int rc;
    int32_t off = dsk_offset("read", drive, track, sector, &rc);
    if (off < 0) return rc;
    FIL *f = &g_dsk_file[drive];
    FRESULT fr = f_lseek(f, (FSIZE_t)off);
    if (fr != FR_OK) {
        Serial.printf("[dsk] seek d=%u t=%u s=%u failed: %d\n", drive, track, sector, fr);
        return 1;
    }
    UINT br = 0;
    fr = f_read(f, out256, 256, &br);
    if (fr != FR_OK || br != 256) {
        Serial.printf("[dsk] read d=%u t=%u s=%u failed: fr=%d br=%u\n",
                      drive, track, sector, fr, (unsigned)br);
        return 1;
    }
#ifdef FDC_TRACE
    // PIZERO-194: a checksum per sector read, to compare with the image.
    { uint32_t sum = 0; for (unsigned i = 0; i < 256; i++) sum += out256[i];
      FDC_TRACE_PORT.printf("[dsk] rd d=%u t=%u s=%u sum=%05lu\r\n", drive, track, sector, (unsigned long)sum); }
    // PIZERO-190: the boot track, for disassembling what DOS runs.
    if (track == 34) {
        for (unsigned i = 0; i < 256; i += 32) {
            FDC_TRACE_PORT.printf("[t34] %02u %02x ", sector, i);
            for (unsigned j = 0; j < 32; j++) FDC_TRACE_PORT.printf("%02x", out256[i + j]);
            FDC_TRACE_PORT.print("\r\n");
        }
    }
#endif
    return 0;
}

// AMOLED-26 step 1: parse a CoCo DECB LOADM .bin file straight off the SD,
// walking the segment chain.
//
// LOADM format:
//   [1 byte: type] [2 bytes BE: len] [2 bytes BE: addr] [len bytes: data]
//   type = $00 → data segment, then another segment follows
//   type = $FF → end marker, the two "addr" bytes are the entry vector,
//                len bytes (usually 0) are ignored
//
// `cb` is called once per data segment with (addr, src_buf, len), caller
// decides whether to write into emulator RAM or just diagnose. Returns the
// entry vector via *entry_out, or returns false on malformed file / I/O.
typedef void (*coco_loadm_seg_cb)(uint16_t addr, const uint8_t *data, uint16_t len, void *ctx);

extern "C" bool coco_boot_parse_loadm(const char *path,
                                      coco_loadm_seg_cb cb, void *ctx,
                                      uint16_t *entry_out) {
    // PIZERO-324: the FIL and buffer on the heap (about 800 bytes), not the
    // stack: the F12 launch check runs this inside the USB key handler.
    struct { FIL f; uint8_t buf[256]; } *w = (decltype(w))malloc(sizeof *w);
    if (!w) { Serial.println("[loadm] no memory"); return false; }
    FIL &f = w->f;
    uint8_t *buf = w->buf;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[loadm] open %s failed: %s (%d)\n",
                      path, FRESULT_str(fr), fr);
        free(w);
        return false;
    }
    bool ok = false;
    while (true) {
        uint8_t hdr[5];
        UINT br = 0;
        if (f_read(&f, hdr, 5, &br) != FR_OK || br != 5) {
            Serial.println("[loadm] short header");
            break;
        }
        uint8_t type = hdr[0];
        uint16_t len  = ((uint16_t)hdr[1] << 8) | hdr[2];
        uint16_t addr = ((uint16_t)hdr[3] << 8) | hdr[4];
        if (type == 0xFF) {
            // End marker: addr is the entry vector.
            if (entry_out) *entry_out = addr;
            Serial.printf("[loadm] END entry=$%04X\n", addr);
            ok = true;
            break;
        }
        if (type != 0x00) {
            Serial.printf("[loadm] bad type $%02X at file offset %lu\n",
                          type, (unsigned long)f_tell(&f) - 5);
            break;
        }
        // Read segment data in chunks (segment can be >256 bytes).
        Serial.printf("[loadm] SEG addr=$%04X len=%u\n", addr, len);
        uint16_t left = len;
        uint16_t curr = addr;
        while (left > 0) {
            UINT want = (left > sizeof w->buf) ? sizeof w->buf : left;
            if (f_read(&f, buf, want, &br) != FR_OK || br != want) {
                Serial.println("[loadm] short read");
                goto done;
            }
            if (cb) cb(curr, buf, br, ctx);
            curr += br;
            left -= br;
        }
    }
done:
    f_close(&f);
    free(w);
    return ok;
}

extern "C" void HOT_FUNC(coco_boot_blit_vdg_src)(const uint8_t *src, uint16_t *fb) {
    if (!g_maps_ready) build_maps();
    for (int py = 0; py < LCD_H; py++) {
        const uint8_t *scol = &src[g_cx_byte_for_py[py]];
        const int shift = g_nibble_shift_for_py[py];
        uint16_t *frow = &fb[py * LCD_W];
        for (int px = 0; px < LCD_W; px++) {
            frow[px] = g_vdg_rgb565[(scol[g_src_off_for_px[px]] >> shift) & 0x0F];
        }
    }
}

extern "C" void coco_boot_blit_vdg(uint16_t *fb) {
    coco_boot_blit_vdg_src(coco_machine_get_vdg_buffer(), fb);
}

// AMOLED-54 experiment: 1:1 blit into a 192×256 panel-window framebuffer.
// CoCo native 256×192 → centered in landscape 448×368 at offset (96, 88).
// In panel coords that's window (88, 96)..(280, 352) = 192 wide × 256 tall.
// Layout: small_fb[wpy * 192 + wpx], panel row-major.
//
//   landscape sx = 96 + wpy            (so coco_x = wpy)
//   landscape sy = 279 - wpx           (so coco_y = 191 - wpx)
//
// Reads jump rows in the packed VDG buffer (whole buffer fits in cache
// so it doesn't hurt). Writes are sequential per row.
extern "C" void HOT_FUNC(coco_boot_blit_vdg_1to1_src)(const uint8_t *src, uint16_t *small_fb) {
    for (int wpy = 0; wpy < 256; wpy++) {
        int coco_x = wpy;
        unsigned cx_byte  = (unsigned)coco_x >> 1;
        unsigned cx_shift = (coco_x & 1) ? 4 : 0;
        uint16_t *frow = &small_fb[wpy * 192];
        // wpx 0..191 → coco_y 191..0. Walk the VDG column upward instead
        // so the inner loop's source pointer moves contiguously.
        const uint8_t *scol = &src[coco_x >> 1] + (191 * 128);
        for (int wpx = 0; wpx < 192; wpx++) {
            uint8_t byte = *scol;
            scol -= 128;
            frow[wpx] = g_vdg_rgb565[(byte >> cx_shift) & 0x0F];
        }
        (void)cx_byte;  // expressed via pointer math above
    }
}

extern "C" void coco_boot_blit_vdg_1to1(uint16_t *small_fb) {
    coco_boot_blit_vdg_1to1_src(coco_machine_get_vdg_buffer(), small_fb);
}

// PIZERO-07: blit into a 320x240 RGB565 framebuffer for libdvi (which scales
// it 2x to 640x480). Unlike the AMOLED path this is NATIVE RGB565 byte order
// (libdvi consumes it straight, no wire byte-swap) and NO rotation. The CoCo's
// 256x192 is centered at offset (32, 24), leaving a black border that the 2x
// doubling renders as the README's 512x384-in-640x480 image.
#define PIZERO_FB_W 320
#define PIZERO_FB_H 240
#define PIZERO_X0   ((PIZERO_FB_W - COCO_VDG_W) / 2)  // 32
#define PIZERO_Y0   ((PIZERO_FB_H - COCO_VDG_H) / 2)  // 24

// PIZERO-55/85: the palette is the writable register file owned by
// coco_machine (it is what sees the guest's writes to $FFB0-$FFBF). The blit
// hoists the pointer once per frame rather than reloading a global per pixel.

// Only the active 256x192 region is written each frame. The black border is
// painted once by the caller (memset of g_fb at init) and never changes, so
// re-clearing it every frame would be ~27K wasted pixel writes per frame.
extern "C" void HOT_FUNC(coco_boot_blit_vdg_pizero_src)(const uint8_t *src, uint16_t *fb) {
    const uint16_t *pal = coco_machine_palette();
    for (int cy = 0; cy < COCO_VDG_H; cy++) {
        const uint8_t *srow = &src[cy * (COCO_VDG_W / 2)];
        uint16_t *frow = &fb[(PIZERO_Y0 + cy) * PIZERO_FB_W + PIZERO_X0];
        for (int cx = 0; cx < COCO_VDG_W; cx += 2) {
            uint8_t byte = srow[cx >> 1];           // two pixels packed per byte
            frow[cx]     = pal[byte & 0x0F];        // even = low nibble
            frow[cx + 1] = pal[(byte >> 4) & 0x0F]; // odd  = high nibble
        }
    }
}

// PIZERO-135: a CoCo 2 on the 640-wide framebuffer. The palette (writable,
// PIZERO-55) is copied into the scanout's slots each frame, and each pixel's
// index is written twice: the same picture as the RGB565 blit.
extern "C" void HOT_FUNC(coco_boot_blit_vdg_pizero8)(uint8_t *fb, uint16_t *pal565, int base) {
    const uint16_t *pal = coco_machine_palette();
    for (int i = 0; i < 16; i++) pal565[base + i] = pal[i];
    const uint8_t *src = coco_machine_get_vdg_buffer();
    // Two packed source pixels make four output bytes, one 32-bit store.
    uint32_t quad[256];
    for (int b = 0; b < 256; b++) {
        uint32_t lo = (uint32_t)(base + (b & 0x0F)), hi = (uint32_t)(base + (b >> 4));
        quad[b] = lo | lo << 8 | hi << 16 | hi << 24;
    }
    for (int cy = 0; cy < COCO_VDG_H; cy++) {
        const uint8_t *srow = &src[cy * (COCO_VDG_W / 2)];
        uint32_t *frow = (uint32_t *)&fb[(PIZERO_Y0 + cy) * (2 * PIZERO_FB_W) + 2 * PIZERO_X0];
        for (int i = 0; i < COCO_VDG_W / 2; i++) frow[i] = quad[srow[i]];
    }
}

// ---------------------------------------------------------------------------
// PIZERO-92: a 32x16 text card, drawn in the machine's own 8x12 font and
// palette so a diagnostic looks like it came from the CoCo rather than from
// some other device.
//
// Why this exists: every failure before coco_machine_init leaves libdvi
// running and the framebuffer black, and a monitor that syncs to a black
// picture is indistinguishable from a dead unit. We ship no ROMs, so EVERY
// unit fails that way on first power-up by design.
//
// 512 bytes of character grid, not a 24 KB VDG buffer: the card is rendered
// straight into the framebuffer a glyph row at a time. PIZERO-81b can reuse
// this for the launcher; it is the same 32x16 card with different content.

extern "C" const uint8_t font_6847t1[];   // 128 glyphs x 12 rows
extern "C" const uint8_t font_6847t2[];   // PIZERO-166: the card draws with ours

#include "text_card.h"          // card_code / card_center_col, host-tested

// Each cell: a glyph index into font_6847t2 (bits 0-6) and CARD_INVERSE.
static uint8_t g_card[CARD_ROWS][CARD_COLS];

extern "C" void coco_boot_card_clear(void) {
    memset(g_card, CARD_SPACE, sizeof g_card);
}

extern "C" void coco_boot_card_text(int col, int row, const char *s) {
    if (!s || row < 0 || row >= CARD_ROWS) return;
    for (int c = col; c < CARD_COLS && *s; c++, s++)
        if (c >= 0) g_card[row][c] = card_code(*s);
}

// PIZERO-81b: inverse video for a whole row (the overlay's selection and
// title bars), as the 6847 draws it: paper-colored glyphs on an ink bar.
// Bit 7 of a card cell is free, since glyph indexes are $00-$7F.
#define CARD_INVERSE 0x80
extern "C" void coco_boot_card_invert_row(int row) {
    if (row < 0 || row >= CARD_ROWS) return;
    for (int c = 0; c < CARD_COLS; c++) g_card[row][c] |= CARD_INVERSE;
}

// PIZERO-146: the editor's cursor, one cell in inverse.
extern "C" void coco_boot_card_invert_cell(int col, int row) {
    if (row < 0 || row >= CARD_ROWS || col < 0 || col >= CARD_COLS) return;
    g_card[row][col] ^= CARD_INVERSE;
}

extern "C" void coco_boot_card_center(int row, const char *s) {
    if (!s) return;
    int len = 0;
    while (s[len]) len++;
    coco_boot_card_text(card_center_col(len), row, s);
}

// Render the card into the framebuffer, ink on paper. PIZERO-256: in the
// factory colors, green on black, whatever a program or a color setting has
// done to the machine's palette (it used the live palette, so a recolored
// green could leave the overlay unreadable). The machine's palette is not
// touched, so its colors are as they were when the card goes away.
#define CARD_INK   0x07E0      // the VDG's green
#define CARD_PAPER 0x0000      // black
// Draw `s` wrapped into the card from `row` down, at most `max_rows` rows.
// Returns the row after the last one used, so a caller can stack blocks.
extern "C" int coco_boot_card_wrap(int col, int row, int width, int max_rows,
                                   const char *s) {
    if (width <= 0 || width > CARD_COLS - col) width = CARD_COLS - col;
    int r = row;
    while (s && *s && r < CARD_ROWS && (r - row) < max_rows) {
        const char *next = s;
        int len = card_wrap_next(s, width, &next);
        for (int i = 0; i < len && (col + i) < CARD_COLS; i++)
            g_card[r][col + i] = card_code(s[i]);
        s = next;
        r++;
    }
    return r;
}

extern "C" void coco_boot_card_present(uint16_t *fb) {
    const uint16_t ink = CARD_INK;
    const uint16_t paper = CARD_PAPER;
    for (int r = 0; r < CARD_ROWS; r++) {
        for (int sub = 0; sub < 12; sub++) {
            int y = r * 12 + sub;
            if (y >= COCO_VDG_H) break;
            uint16_t *frow = &fb[(PIZERO_Y0 + y) * PIZERO_FB_W + PIZERO_X0];
            for (int c = 0; c < CARD_COLS; c++) {
                uint8_t cell = g_card[r][c];
                uint8_t glyph = font_6847t2[(cell & 0x7F) * 12 + sub];
                uint16_t fg = (cell & CARD_INVERSE) ? paper : ink;
                uint16_t bg = (cell & CARD_INVERSE) ? ink : paper;
                uint16_t *px = &frow[c * 8];
                for (int b = 0; b < 8; b++)
                    px[b] = (glyph & (0x80 >> b)) ? fg : bg;
            }
        }
    }
}

extern "C" void coco_boot_card_present8(uint8_t *fb, uint16_t *pal565, int base) {
    pal565[base] = CARD_INK;          // the card's own two slots
    pal565[base + 1] = CARD_PAPER;
    const uint8_t ink = (uint8_t)base, paper = (uint8_t)(base + 1);
    for (int r = 0; r < CARD_ROWS; r++) {
        for (int sub = 0; sub < 12; sub++) {
            int y = r * 12 + sub;
            if (y >= COCO_VDG_H) break;
            uint8_t *frow = &fb[(PIZERO_Y0 + y) * (2 * PIZERO_FB_W) + 2 * PIZERO_X0];
            for (int c = 0; c < CARD_COLS; c++) {
                uint8_t cell = g_card[r][c];
                uint8_t glyph = font_6847t2[(cell & 0x7F) * 12 + sub];
                uint8_t fg = (cell & CARD_INVERSE) ? paper : ink;
                uint8_t bg = (cell & CARD_INVERSE) ? ink : paper;
                uint8_t *px = &frow[c * 16];
                for (int b = 0; b < 8; b++)
                    px[2 * b] = px[2 * b + 1] = (glyph & (0x80 >> b)) ? fg : bg;
            }
        }
    }
}

extern "C" void coco_boot_blit_vdg_pizero(uint16_t *fb) {
    coco_boot_blit_vdg_pizero_src(coco_machine_get_vdg_buffer(), fb);
}

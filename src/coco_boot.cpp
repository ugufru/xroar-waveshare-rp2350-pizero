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
#include "png_write.h"          // PIZERO-165, host-tested
#include "hardware/watchdog.h"   // PIZERO-165: fed while a screenshot writes

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

static size_t read_rom_file(const char *path, uint8_t *out, size_t want) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[rom] open %s failed: %s (%d)\n", path, FRESULT_str(fr), fr);
        return 0;
    }
    UINT br = 0;
    fr = f_read(&f, out, want, &br);
    f_close(&f);
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
        if (!ext || (strcasecmp(ext, ".dsk") != 0)) continue;
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

extern "C" int coco_boot_rescan(int kind) {
    dsk_cat_clear(&g_dsk_cat, kind);
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
    settings_defaults(out);
    return coco_boot_apply_settings_file("0:/coco/settings.txt", out);
}

// PIZERO-154: apply one settings-format file on top of *out, as it stands:
// settings.txt over the defaults, or a game's NAME.TXT over settings.txt.
// Returns false when there is no such file (and *out is unchanged).
extern "C" bool coco_boot_apply_settings_file(const char *path, struct coco_settings *out) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    char line[96], name[32];
    int n = 0;
    while (f_gets(line, sizeof line, &f)) {
        n++;
        name[0] = '\0';
        int r = settings_parse_line(out, line, name, sizeof name);
        if (r == SET_UNKNOWN)   Serial.printf("[settings] %s line %d: unknown setting '%s', ignored\r\n", path, n, name);
        if (r == SET_BAD_VALUE) Serial.printf("[settings] %s line %d: bad value for '%s', ignored\r\n", path, n, name);
        if (r == SET_SYNTAX)    Serial.printf("[settings] %s line %d: expected 'name = value', ignored\r\n", path, n);
    }
    f_close(&f);
    return true;
}

// PIZERO-146: text files the on-screen editor opens and saves. Load reads up
// to max bytes; it returns false (with *len = 0) when there is no file.
extern "C" bool coco_boot_load_text(const char *path, char *buf, uint32_t max, uint32_t *len) {
    *len = 0;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, max, &br);
    f_close(&f);
    if (fr != FR_OK) return false;
    *len = br;
    return true;
}

// Save safely: write the whole text to PATH.tmp, then swap it in for PATH.
// A power cut during the write leaves the old file untouched; one between
// removing the old file and the rename leaves only the .tmp, which
// coco_boot_recover_text() finishes at the next boot.
static void tmp_path_for(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s.tmp", path);
}

extern "C" bool coco_boot_save_text(const char *path, const char *buf, uint32_t len) {
    char tmp[96];
    tmp_path_for(path, tmp, sizeof tmp);
    FIL f;
    FRESULT fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { Serial.printf("[save] %s: open failed (%d)\r\n", tmp, fr); return false; }
    UINT bw = 0;
    fr = f_write(&f, buf, len, &bw);
    FRESULT fc = f_close(&f);                 // close flushes to the card
    if (fr != FR_OK || fc != FR_OK || bw != len) {
        Serial.printf("[save] %s: write failed (%d/%d, %u of %lu)\r\n", tmp, fr, fc,
                      (unsigned)bw, (unsigned long)len);
        f_unlink(tmp);
        return false;
    }
    fr = f_unlink(path);
    if (fr != FR_OK && fr != FR_NO_FILE) { Serial.printf("[save] %s: remove old failed (%d)\r\n", path, fr); return false; }
    fr = f_rename(tmp, path);
    if (fr != FR_OK) { Serial.printf("[save] %s: rename failed (%d)\r\n", path, fr); return false; }
    Serial.printf("[save] %s: %lu bytes\r\n", path, (unsigned long)len);
    return true;
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
    const uint16_t *px = c->fb + (size_t)y * (size_t)c->w;
    for (int x = 0; x < c->w; x++) {
        int i = png_palette_index(c->pal, &c->npal, 256, px[x]);
        out[x] = (uint8_t)(i < 0 ? 0 : i);
    }
}

extern "C" bool coco_boot_screenshot(const uint16_t *fb, int w, int h, char *name, size_t name_sz) {
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
    for (int i = 0; i < w * h; i++) png_palette_index(c.pal, &c.npal, 256, fb[i]);
    for (int i = 0; i < c.npal; i++) png_rgb565_to_888(c.pal[i], &pal_rgb[i * 3]);
    fr = f_open(&c.f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        Serial.printf("[shot] %s: open failed (%d)\r\n", path, fr);
        free(cp); free(row);
        return false;
    }
    c.used = 0; c.failed = false; c.fb = fb; c.w = w;
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
    while (f_gets(line, sizeof(line), &f)) {
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
#define COCO_NDRIVE 4
static FIL  g_dsk_file[COCO_NDRIVE];
static bool g_dsk_open[COCO_NDRIVE];
// Per-drive flags, one byte each (pizero_wavmeas has no RAM to spare).
#define DSK_RW    0x01                    // opened writable (else the file refused)
#define DSK_WP    0x02                    // the user's write-protect tab
#define DSK_DIRTY 0x04                    // written since the last f_sync
static uint8_t g_dsk_flags[COCO_NDRIVE];
#define g_dsk_rw(d)    ((g_dsk_flags[d] & DSK_RW) != 0)
#define g_dsk_wp(d)    ((g_dsk_flags[d] & DSK_WP) != 0)
#define g_dsk_dirty(d) ((g_dsk_flags[d] & DSK_DIRTY) != 0)
static char g_dsk_path[COCO_NDRIVE][96];

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
static char g_drives_saved[COCO_NDRIVE * 104];
static bool g_drives_quiet = false;

static void drives_save(void) {
    if (g_drives_quiet) return;
    char text[sizeof g_drives_saved];
    int k = 0;
    for (unsigned d = 0; d < COCO_NDRIVE; d++)
        k += snprintf(text + k, sizeof text - k, "%u%s = %s\n", d,
                      (g_dsk_open[d] && g_dsk_wp(d)) ? " ro" : "",
                      g_dsk_open[d] ? g_dsk_path[d] : "");
    if (!strcmp(text, g_drives_saved)) return;
    if (coco_boot_save_text(DRIVES_PATH, text, (uint32_t)k)) {
        strcpy(g_drives_saved, text);
        Serial.print("[dsk] drives saved\n");
    }
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
    g_dsk_open[drive] = true;
    g_dsk_flags[drive] = rw ? DSK_RW : 0;
    strcpy(g_dsk_path[drive], path);
    Serial.printf("[dsk] drive %u mounted %s (%lu bytes%s)\n",
                  drive, path, (unsigned long)f_size(&g_dsk_file[drive]),
                  rw ? "" : ", read-only file");
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
    char text[sizeof g_drives_saved];
    uint32_t n = 0;
    if (!coco_boot_load_text(DRIVES_PATH, text, sizeof text - 1, &n)) return false;
    text[n] = '\0';
    // What the file says is what is saved, even where a disk is now missing,
    // so the next real change rewrites it rather than a failed mount here.
    snprintf(g_drives_saved, sizeof g_drives_saved, "%s", text);
    g_drives_quiet = true;
    for (char *line = text, *nl; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = '\0';
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
    char path[96], tmp[100];
    FILINFO fi;
    snprintf(name, name_sz, "%s.DSK", base);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (f_stat(path, &fi) == FR_OK) return COCO_NEWDSK_EXISTS;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FIL f;
    fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { Serial.printf("[dsk] new: open %s failed (%d)\n", tmp, fr); return COCO_NEWDSK_FAILED; }
    uint8_t blank[512];                     // on the stack: pizero_wavmeas has no spare RAM
    memset(blank, 0xFF, sizeof blank);
    uint32_t t0 = micros();
    bool ok = true;
    for (int i = 0; i < 161280 / 512 && ok; i++) {
        UINT bw = 0;
        ok = f_write(&f, blank, sizeof blank, &bw) == FR_OK && bw == sizeof blank;
        watchdog_update();
    }
    if (f_close(&f) != FR_OK) ok = false;
    if (ok) ok = f_rename(tmp, path) == FR_OK;
    if (!ok) {
        Serial.printf("[dsk] new: writing %s failed\n", tmp);
        f_unlink(tmp);
        return COCO_NEWDSK_FAILED;
    }
    Serial.printf("[dsk] new: %s (%lu us)\n", path, (unsigned long)(micros() - t0));
    return 0;
}

// PIZERO-196: rename a disk in place, in its own directory, keeping .DSK.
// A disk in a drive is ejected from every drive holding it, renamed, and
// put back with its write-protect tab, so the handle and drives.txt stay
// right. Returns 0, COCO_NEWDSK_EXISTS or COCO_NEWDSK_FAILED.
extern "C" int coco_boot_rename_dsk(const char *old_path, const char *base,
                                    char *name, size_t name_sz) {
    const char *slash = strrchr(old_path, '/');
    if (!slash) return COCO_NEWDSK_FAILED;
    char path[96];
    snprintf(name, name_sz, "%s.DSK", base);
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

// A sector's offset in the image, or -1 when the drive is empty or the
// address is off the 35-track, 18-sector, single-sided disk (PIZERO-68
// covers more). *rc is the FDC's answer in that case.
static int32_t dsk_offset(const char *what, unsigned drive, unsigned track,
                          unsigned sector, int *rc) {
    if (drive >= COCO_NDRIVE || !g_dsk_open[drive]) {
        *rc = COCO_DISK_NOT_READY;           // empty drive, as a real one reports
        return -1;
    }
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
    int32_t off = dsk_offset("write", drive, track, sector, &rc);
    if (off < 0) return rc;
    if (g_dsk_wp(drive) || !g_dsk_rw(drive)) return COCO_DISK_WRITE_PROTECT;
    if (!in256) return 0;                    // the probe: this drive takes writes
    FIL *f = &g_dsk_file[drive];
    uint32_t t0 = micros();
    UINT bw = 0;
    FRESULT fr = f_lseek(f, (FSIZE_t)off);
    if (fr == FR_OK) fr = f_write(f, in256, 256, &bw);
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
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[loadm] open %s failed: %s (%d)\n",
                      path, FRESULT_str(fr), fr);
        return false;
    }
    bool ok = false;
    uint8_t buf[256];
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
            UINT want = (left > sizeof(buf)) ? sizeof(buf) : left;
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

// Render the card into the framebuffer, ink on paper, using the live palette
// so a recolored machine recolors the diagnostic too.
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
    const uint16_t *pal = coco_machine_palette();
    const uint16_t ink = pal[0];      // VDG green
    const uint16_t paper = pal[8];    // black
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

extern "C" void coco_boot_blit_vdg_pizero(uint16_t *fb) {
    coco_boot_blit_vdg_pizero_src(coco_machine_get_vdg_buffer(), fb);
}

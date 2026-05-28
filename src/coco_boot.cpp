/*
 * coco_boot.cpp — front-end glue between the XRoar core and the AMOLED panel.
 *
 *   * coco_boot_load_rom_from_sd() reads /coco/extbas11.rom and
 *     /coco/bas12.rom from the mounted SD card and concatenates them
 *     into a 16 KB image (ECB low half, BASIC high half — so that
 *     ROM[A & 0x3FFF] resolves correctly for A in $8000-$BFFF and
 *     for the reset vector at $FFFE).
 *
 *   * coco_boot_blit_vdg() takes the live 256x192 palette-index buffer
 *     from coco_machine and writes it 1x, centred, into the landscape
 *     448x368 framebuffer. Rotation to the panel-native 368x448 portrait
 *     is inlined here (see src/main.cpp::plot() for the convention).
 *
 *   * coco_boot_blit_vdg_src() is the same blit with the source pointer
 *     passed in directly — used by main.cpp before XRoar is wired up
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

// Panel native — must match src/main.cpp.
#define LCD_W   368
#define LCD_H   448

// Logical landscape — must match src/main.cpp.
#define SCREEN_W  448  // = LCD_H
#define SCREEN_H  368  // = LCD_W

// Full-screen anamorphic blit: source 256x192 stretched to 448x368 landscape
// via nearest-neighbor lookup tables. Horizontal ratio 1.75, vertical ratio
// ~1.917 — close to the real CoCo's wide NTSC pixel aspect.
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
// stored byte-swapped — the SH8601 over QSPI consumes MSB-first, so the
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

extern "C" bool coco_boot_load_rom_from_sd(uint8_t *rom16k) {
    memset(rom16k, 0xFF, 16384);
    char path[80];
    size_t n_ecb = 0, n_bas = 0;
    if (coco_boot_resolve("rom", "extbas11.rom", path, sizeof(path)))
        n_ecb = read_rom_file(path, &rom16k[0x0000], 8192);
    if (coco_boot_resolve("rom", "bas12.rom", path, sizeof(path)))
        n_bas = read_rom_file(path, &rom16k[0x2000], 8192);
    if (n_bas != 8192) {
        Serial.println("[rom] bas12.rom is required (looked in /coco/roms/ then /coco/)");
        return false;
    }
    if (n_ecb != 8192) {
        Serial.println("[rom] extbas11.rom not found in /coco/roms/ or /coco/ "
                       "— running Color BASIC only (no Extended/Disk BASIC)");
        memset(&rom16k[0x0000], 0xFF, 0x2000);
    }
    Serial.printf("[rom] reset vector -> $%02X%02X\n",
                  rom16k[0x3FFE], rom16k[0x3FFF]);
    return true;
}

// AMOLED-58: cart loader takes a bare filename so autorun.txt can
// pick an alternate cart (e.g. a game .ccc) via @CART.
extern "C" bool coco_boot_load_cart_named(const char *name, uint8_t *cart8k) {
    memset(cart8k, 0xFF, 8192);
    char path[80];
    if (!coco_boot_resolve("rom", name, path, sizeof(path))) {
        Serial.printf("[cart] %s not found\n", name);
        return false;
    }
    size_t n = read_rom_file(path, cart8k, 8192);
    if (n != 8192) {
        Serial.printf("[cart] %s load failed (got %u bytes)\n",
                      name, (unsigned)n);
        return false;
    }
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
                strncpy(out->disk_name, arg, sizeof(out->disk_name) - 1);
            } else if (prefix_eq(body, "CART", &arg)) {
                strncpy(out->cart_name, arg, sizeof(out->cart_name) - 1);
            } else if (prefix_eq(body, "DIRECT", &arg)) {
                strncpy(out->direct_name, arg, sizeof(out->direct_name) - 1);
            } else {
                Serial.printf("[autorun] unknown directive: %s\n", body);
            }
            continue;
        }
        // Autotype line — append + '\r'. Truncate if buffer is full.
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
    Serial.printf("[autorun] parsed: cart='%s' disk='%s' direct='%s' autotype=%u bytes\n",
                  out->cart_name, out->disk_name, out->direct_name,
                  (unsigned)at_used);
    return true;
}

// JVC disk image kept open at /coco/demos.dsk. CoCo standard: 35 tracks,
// 18 sectors/track, 256 bytes/sector, single-sided = 161 280 bytes.
// Sector numbers from BASIC/DECB are 1-based; track is 0-based.
static FIL  g_dsk_file;
static bool g_dsk_open = false;

extern "C" bool coco_boot_attach_dsk(const char *path) {
    if (g_dsk_open) { f_close(&g_dsk_file); g_dsk_open = false; }
    FRESULT fr = f_open(&g_dsk_file, path, FA_READ);
    if (fr != FR_OK) {
        Serial.printf("[dsk] open %s failed: %s (%d)\n",
                      path, FRESULT_str(fr), fr);
        return false;
    }
    g_dsk_open = true;
    Serial.printf("[dsk] attached %s (%lu bytes)\n",
                  path, (unsigned long)f_size(&g_dsk_file));
    return true;
}

extern "C" int coco_boot_disk_read_sector(unsigned drive, unsigned track,
                                          unsigned sector, uint8_t *out256) {
    if (drive != 0 || !g_dsk_open) {
        Serial.printf("[dsk] reject drive=%u open=%d\n", drive, (int)g_dsk_open);
        return 1;
    }
    if (track > 34 || sector < 1 || sector > 18) {
        Serial.printf("[dsk] reject t=%u s=%u (range)\n", track, sector);
        return 1;
    }
    const uint32_t off = ((uint32_t)track * 18 + (sector - 1)) * 256;
    FRESULT fr = f_lseek(&g_dsk_file, off);
    if (fr != FR_OK) {
        Serial.printf("[dsk] seek t=%u s=%u failed: %d\n", track, sector, fr);
        return 1;
    }
    UINT br = 0;
    fr = f_read(&g_dsk_file, out256, 256, &br);
    if (fr != FR_OK || br != 256) {
        Serial.printf("[dsk] read t=%u s=%u failed: fr=%d br=%u\n", track, sector, fr, (unsigned)br);
        return 1;
    }
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
// `cb` is called once per data segment with (addr, src_buf, len) — caller
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

// Native (non-byte-swapped) twin of g_vdg_rgb565.
static const uint16_t g_vdg_rgb565_native[16] = {
    0x07E0, 0xFFE0, 0x001F, 0xF800, 0xFFFF, 0x07FF, 0xF81F, 0xFC00,
    0x0000, 0x0320, 0x8200, 0xFCA0, 0x0000, 0x0000, 0x0000, 0x0000,
};

extern "C" void HOT_FUNC(coco_boot_blit_vdg_pizero_src)(const uint8_t *src, uint16_t *fb) {
    // Top border rows (above the CoCo image): paint black once per frame.
    for (int y = 0; y < PIZERO_Y0; y++)
        for (int x = 0; x < PIZERO_FB_W; x++) fb[y * PIZERO_FB_W + x] = 0;
    for (int cy = 0; cy < COCO_VDG_H; cy++) {
        const uint8_t *srow = &src[cy * (COCO_VDG_W / 2)];
        uint16_t *frow = &fb[(PIZERO_Y0 + cy) * PIZERO_FB_W];
        for (int x = 0; x < PIZERO_X0; x++) frow[x] = 0;                 // left border
        for (int cx = 0; cx < COCO_VDG_W; cx++) {
            uint8_t byte = srow[cx >> 1];
            int shift = (cx & 1) ? 4 : 0;
            frow[PIZERO_X0 + cx] = g_vdg_rgb565_native[(byte >> shift) & 0x0F];
        }
        for (int x = PIZERO_X0 + COCO_VDG_W; x < PIZERO_FB_W; x++) frow[x] = 0; // right border
    }
    for (int y = PIZERO_Y0 + COCO_VDG_H; y < PIZERO_FB_H; y++)
        for (int x = 0; x < PIZERO_FB_W; x++) fb[y * PIZERO_FB_W + x] = 0;
}

extern "C" void coco_boot_blit_vdg_pizero(uint16_t *fb) {
    coco_boot_blit_vdg_pizero_src(coco_machine_get_vdg_buffer(), fb);
}

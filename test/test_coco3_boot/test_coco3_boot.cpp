// PIZERO-133: boot a 128 KB CoCo 3 on the host.
//
// The firmware's 6809 core and PIAs with the vendored GIME (tcc1014.c) and
// coco3_bus.h, running Super Extended Color BASIC from a real coco3.rom
// (32 KB; the repo ships no ROMs). Passes when BASIC has printed its banner
// and OK on the 32-column screen.
//
// PIZERO-213: the boot runs with the firmware's MMU fast path, and a second
// test boots with and without it and checks the two machines end up the same:
// time, CPU registers and all 128 KB of RAM.
//
//   COCO_ROM_DIR=~/Desktop/coco/roms pio test -e native -f test_coco3_boot
//
// COCO3_DUMP=<file> also writes the last frame as raw RGB565 (320x240,
// little-endian) for a look at the picture.

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c3host.h"
#include "../../lib/coco_machine/src/coco3_bus.h"

void setUp(void) {}
void tearDown(void) {}

#define FRAME_TICKS (912 * 263)           // one 60 Hz field in 14.318 MHz ticks

static uint8_t g_rom[32768];

static bool load_rom(void) {
    const char *dir = getenv("COCO_ROM_DIR");
    if (!dir || !dir[0]) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/coco3.rom", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(g_rom, 1, sizeof g_rom, f);
    fclose(f);
    return n == sizeof g_rom;
}

// The 32-column screen as text. BASIC keeps it at logical $0400 in the
// 64 KB its MMU maps to the top of physical RAM ($70000, which a 128 KB
// machine sees at $10000).
static void screen_row(int row, char out[33]) {
    for (int c = 0; c < 32; c++) {
        uint8_t v = c3_phys(0x10400 + row * 32 + c) & 0x7F;
        char ch = (char)(v & 0x3F);
        if (ch < 0x20) ch += 0x40;      // the VDG's @A-Z[\]^_ block
        out[c] = ch;
    }
    out[32] = '\0';
}

static bool screen_has(const char *s) {
    char row[33];
    for (int r = 0; r < 16; r++) {
        screen_row(r, row);
        if (strstr(row, s)) return true;
    }
    return false;
}

static void dump(void) {
    const char *path = getenv("COCO3_DUMP");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(c3_fb(), 2, 320 * 240, f);
    fclose(f);
}

static void test_boots_to_ok(void) {
    if (!load_rom()) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with coco3.rom (32 KB)");
    TEST_ASSERT_TRUE(c3_init(g_rom, 1));
    for (int f = 0; f < 180; f++) c3_run_ticks(FRAME_TICKS);   // 3 s
    char row[33];
    for (int r = 0; r < 16; r++) { screen_row(r, row); printf("  |%s|\n", row); }
    printf("  pc=%04x\n", c3_pc());
    dump();
    TEST_ASSERT_TRUE_MESSAGE(screen_has("EXTENDED COLOR BASIC"), "no banner on the screen");
    TEST_ASSERT_TRUE_MESSAGE(screen_has("OK"), "no OK prompt");
}

// Boot for `frames` fields and capture the machine: time, registers, RAM.
static void boot(int fast, int frames, struct c3_state *st, uint8_t *ram) {
    TEST_ASSERT_TRUE(c3_init(g_rom, fast));
    for (int f = 0; f < frames; f++) c3_run_ticks(FRAME_TICKS);
    c3_state(st);
    for (uint32_t z = 0; z < 0x20000; z++) ram[z] = c3_phys(z);
}

static void test_fast_path_matches(void) {
    if (!load_rom()) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with coco3.rom (32 KB)");
    static uint8_t ram_slow[0x20000], ram_fast[0x20000];
    struct c3_state slow, fastst;
    // Every frame boundary is a check: the first difference names its field.
    for (int frames = 1; frames <= 180; frames = frames < 8 ? frames + 1 : frames * 2) {
        boot(0, frames, &slow, ram_slow);
        boot(1, frames, &fastst, ram_fast);
        char msg[96];
        snprintf(msg, sizeof msg, "after %d fields: slow pc=%04x fast pc=%04x", frames, slow.pc, fastst.pc);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(slow.tick, fastst.tick, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(slow.pc, fastst.pc, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(slow.s, fastst.s, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(slow.u, fastst.u, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(slow.x, fastst.x, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(slow.y, fastst.y, msg);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(slow.cc, fastst.cc, msg);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ram_slow, ram_fast, sizeof ram_slow, msg);
    }
}

// After BASIC is up: switch to 1.79 MHz, then keep mapping bank $30 into
// logical $4000-$5FFF, filling it, and mapping the default bank back, with
// BASIC's 60 Hz interrupt running throughout. Loaded at logical $3000
// (physical $13000 in BASIC's default map); counts its passes at $3080.
static const uint8_t k_remap[] = {
    0xB7, 0xFF, 0xD9,        // 3000 STA $FFD9      1.79 MHz
    0x86, 0x30,              // 3003 LDA #$30       loop:
    0xB7, 0xFF, 0xA2,        // 3005 STA $FFA2      bank $30 at $4000
    0x8E, 0x40, 0x00,        // 3008 LDX #$4000
    0xE7, 0x80,              // 300B STB ,X+        fill:
    0x8C, 0x60, 0x00,        // 300D CMPX #$6000
    0x26, 0xF9,              // 3010 BNE fill
    0x5C,                    // 3012 INCB
    0x86, 0x3A,              // 3013 LDA #$3A
    0xB7, 0xFF, 0xA2,        // 3015 STA $FFA2      the default bank back
    0x7C, 0x30, 0x80,        // 3018 INC $3080
    0x20, 0xE6,              // 301B BRA loop
};

static void remap_run(int fast, struct c3_state *st, uint8_t *ram) {
    boot(fast, 180, st, ram);
    for (unsigned i = 0; i < sizeof k_remap; i++) c3_poke(0x13000 + i, k_remap[i]);
    c3_poke(0x13080, 0);
    c3_set_pc(0x3000);
    for (int f = 0; f < 30; f++) c3_run_ticks(FRAME_TICKS);
    c3_state(st);
    for (uint32_t z = 0; z < 0x20000; z++) ram[z] = c3_phys(z);
}

static void test_fast_path_remap(void) {
    if (!load_rom()) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with coco3.rom (32 KB)");
    static uint8_t ram_slow[0x20000], ram_fast[0x20000];
    struct c3_state slow, fastst;
    remap_run(0, &slow, ram_slow);
    remap_run(1, &fastst, ram_fast);
    printf("  passes=%u (slow) %u (fast), bank $30 byte 0 = %02x\n",
           ram_slow[0x13080], ram_fast[0x13080], ram_fast[0x00000]);
    TEST_ASSERT_TRUE_MESSAGE(ram_slow[0x13080] > 2, "the program did not run");
    TEST_ASSERT_EQUAL_UINT32(slow.tick, fastst.tick);
    TEST_ASSERT_EQUAL_HEX16(slow.pc, fastst.pc);
    TEST_ASSERT_EQUAL_HEX16(slow.s, fastst.s);
    TEST_ASSERT_EQUAL_HEX16(slow.x, fastst.x);
    TEST_ASSERT_EQUAL_HEX8(slow.cc, fastst.cc);
    TEST_ASSERT_EQUAL_MEMORY(ram_slow, ram_fast, sizeof ram_slow);
}

// PIZERO-213: a row is only rewritten when its GIME pixels change, and
// coco3_bus_invalidate makes every row draw again.
static void test_row_skip(void) {
    static uint8_t lo[65536], hi[65536], rom[COCO3_ROM_SIZE], line[912];
    static uint16_t fb[COCO3_FB_W * COCO3_FB_H];
    static struct coco3_bus b;
    coco3_bus_init(&b, lo, hi, rom, fb);
    int first = coco3_bus_first_pixel(&b);
    int y = b.act_y;                              // lands on row (240 - 192) / 2
    int row = coco3_bus_row(&b, y);
    TEST_ASSERT_EQUAL_INT(24, row);
    uint16_t *r = fb + row * COCO3_FB_W;

    memset(line, 0x12, sizeof line);
    b.line = y; coco3_bus_render_line(&b, line, 912);
    TEST_ASSERT_EQUAL_HEX16(b.rgb[0x12], r[0]);

    r[0] = 0xBEEF;                                // drawn over (the overlay)
    b.line = y; coco3_bus_render_line(&b, line, 912);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, r[0]);        // same pixels: left alone

    coco3_bus_invalidate(&b);
    b.line = y; coco3_bus_render_line(&b, line, 912);
    TEST_ASSERT_EQUAL_HEX16(b.rgb[0x12], r[0]);   // redrawn

    line[first + 2 * 100] = 0x2D;                 // one shown pixel changes
    b.line = y; coco3_bus_render_line(&b, line, 912);
    TEST_ASSERT_EQUAL_HEX16(b.rgb[0x2D], r[100]);
}

// PIZERO-218: the GIME's port renderer must draw what upstream's renderer
// plus the halving sink drew. Boot, then optionally run a program that
// masks interrupts and makes some register writes (a video mode) over
// pseudo-random VRAM at physical 0, change part of VRAM, and compare the
// two framebuffers.
static uint16_t g_fb_ref[320 * 240];

struct reg_write { uint16_t a; uint8_t v; };

static void render_run(int port, const struct reg_write *w, int nw) {
    TEST_ASSERT_TRUE(c3_init(g_rom, 1));
    c3_set_port_render(port);
    for (int f = 0; f < 180; f++) c3_run_ticks(FRAME_TICKS);
    if (!nw) return;
    uint8_t prog[256];
    unsigned n = 0;
    prog[n++] = 0x1A; prog[n++] = 0x50;                       // ORCC #$50
    for (int i = 0; i < nw; i++) {                            // LDA #v / STA a
        prog[n++] = 0x86; prog[n++] = w[i].v;
        prog[n++] = 0xB7; prog[n++] = w[i].a >> 8; prog[n++] = w[i].a & 0xFF;
    }
    prog[n++] = 0x20; prog[n++] = 0xFE;                       // BRA *
    for (unsigned i = 0; i < n; i++) c3_poke(0x13000 + i, prog[i]);
    uint32_t x = 12345;
    for (uint32_t z = 0; z < 0x8000; z++) { x = x * 1103515245u + 12345u; c3_poke(z, (uint8_t)(x >> 16)); }
    c3_set_pc(0x3000);
    for (int f = 0; f < 6; f++) c3_run_ticks(FRAME_TICKS);
    // Change part of VRAM: the port renderer skips unchanged lines, so the
    // changed ones must still be drawn.
    for (uint32_t z = 0x800; z < 0x2000; z++) { x = x * 1103515245u + 12345u; c3_poke(z, (uint8_t)(x >> 16)); }
    for (int f = 0; f < 2; f++) c3_run_ticks(FRAME_TICKS);
}

static void check_port_render(const char *what, const struct reg_write *w, int nw) {
    render_run(0, w, nw);
    memcpy(g_fb_ref, c3_fb(), sizeof g_fb_ref);
    static uint16_t fb2[320 * 240];
    render_run(2, w, nw);                       // through fetch_vram
    memcpy(fb2, c3_fb(), sizeof fb2);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(g_fb_ref, fb2, sizeof fb2, what);
    render_run(3, w, nw);                       // PIZERO-330: as jobs
    memcpy(fb2, c3_fb(), sizeof fb2);
    {
        int nj = 0, bj = -1;
        for (int i = 0; i < 320 * 240; i++) if (fb2[i] != g_fb_ref[i]) { if (bj < 0) bj = i; nj++; }
        char m[128];
        snprintf(m, sizeof m, "%s as jobs: %d pixels differ, first at x=%d y=%d", what, nj, bj % 320, bj / 320);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, nj, m);
    }
    render_run(1, w, nw);
    const uint16_t *fb = c3_fb();
    int bad = -1, n = 0;
    for (int i = 0; i < 320 * 240; i++) if (fb[i] != g_fb_ref[i]) { if (bad < 0) bad = i; n++; }
    int ncol = 0;                               // the mode took: count its colours
    static uint16_t seen[64];
    for (int i = 0; i < 320 * 240 && ncol < 64; i++) {
        int k = 0;
        while (k < ncol && seen[k] != fb[i]) k++;
        if (k == ncol) seen[ncol++] = fb[i];
    }
    printf("  %-36s %2d colours, row 120 starts %04x %04x %04x\n", what, ncol, fb[120 * 320 + 40], fb[120 * 320 + 41], fb[120 * 320 + 42]);
    char msg[128];
    snprintf(msg, sizeof msg, "%s: %d pixels differ, first at x=%d y=%d", what, n, bad % 320, bad / 320);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, msg);
}

// A CoCo 3 mode: INIT0 for CoCo 3 video, VMODE, VRES, VRAM at physical 0.
#define C3MODE(vm, vr) { { 0xFF90, 0x4C }, { 0xFF98, vm }, { 0xFF99, vr }, { 0xFF9D, 0 }, { 0xFF9E, 0 } }
// A VDG-compatible mode: INIT0 with COCO set, $FF22 (the VDG mode bits),
// VRAM at physical 0 plus the SAM's $0400, and SAM V (clear bits via the
// even address, set via the odd).
#define VDGMODE(ff22, v) { { 0xFF90, 0xCC }, { 0xFF22, ff22 }, { 0xFF9D, 0 }, { 0xFF9E, 0 }, \
    { (uint16_t)(0xFFC0 + ((v) & 1)), 0 }, { (uint16_t)(0xFFC2 + (((v) >> 1) & 1)), 0 }, \
    { (uint16_t)(0xFFC4 + (((v) >> 2) & 1)), 0 } }

static void test_port_render(void) {
    if (!load_rom()) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with coco3.rom (32 KB)");
    check_port_render("BASIC's 32-column screen", NULL, 0);
    static const struct { const char *what; struct reg_write w[7]; } modes[] = {
        { "40-column text with attributes", C3MODE(0x03, 0x15) },
        { "40-column text",                 C3MODE(0x03, 0x14) },
        { "320x192, 16 colours",            C3MODE(0x80, 0x1E) },
        { "320x192, 4 colours",             C3MODE(0x80, 0x15) },
        { "256x192, 16 colours",            C3MODE(0x80, 0x0E) },
        { "256x192, 2 colours",             C3MODE(0x80, 0x08) },
        { "640x192, 2 colours (halved)",    C3MODE(0x80, 0x1C) },
        { "128x192, 2 colours",             C3MODE(0x80, 0x00) },
        { "VDG alpha and SG4, CSS 0",       VDGMODE(0x00, 0) },
        { "VDG alpha and SG4, CSS 1",       VDGMODE(0x08, 0) },
        { "VDG CG1",                        VDGMODE(0x80, 1) },
        { "VDG RG1",                        VDGMODE(0x90, 1) },
        { "VDG CG2",                        VDGMODE(0xA0, 2) },
        { "VDG RG3, CSS 1",                 VDGMODE(0xD8, 4) },
        { "VDG CG6",                        VDGMODE(0xE0, 6) },
        { "VDG RG6, CSS 1",                 VDGMODE(0xF8, 6) },
    };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++)
        check_port_render(modes[i].what, modes[i].w, modes[i].w[5].a ? 7 : 5);
}

// PIZERO-214: type at BASIC through the keyboard matrix. Each character is
// held for 3 fields and released for 3, slow enough for BASIC's debounce,
// and ENTER is followed by 20.
// '\n' is ENTER, '|' too (for COCO3_TYPE on a command line), and '~' waits
// a second, as in autorun.txt.
static bool key_of(char ch, int *col, int *row, bool *shift) {
    static const char *rows[] = { "@ABCDEFG", "HIJKLMNO", "PQRSTUVW", "XYZ\x01\x02\x03\x04 ",
                                  "01234567", "89:;,-./" };
    static const char *shifted[] = { "", "", "", "", "0!\"#$%&'", "()*+<=>?" };
    *shift = false;
    if (ch == '\n' || ch == '|') { *col = 0; *row = 6; return true; }
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 8; c++) {
            if (rows[r][c] == ch) { *col = c; *row = r; return true; }
            if (shifted[r][0] && shifted[r][c] == ch && !(r == 4 && c == 0)) {
                *col = c; *row = r; *shift = true; return true;
            }
        }
    return false;
}

static void type(const char *s) {
    for (; *s; s++) {
        if (*s == '~') {                         // as autorun.txt: wait a second
            for (int f = 0; f < 60; f++) c3_run_ticks(FRAME_TICKS);
            continue;
        }
        int col, row; bool shift;
        if (!key_of(*s, &col, &row, &shift)) continue;
        if (shift) c3_key(7, 6, 1);
        c3_key(col, row, 1);
        for (int f = 0; f < 3; f++) c3_run_ticks(FRAME_TICKS);
        c3_key(col, row, 0);
        if (shift) c3_key(7, 6, 0);
        // After ENTER, BASIC stores or runs the line and misses keys meanwhile.
        static const char *gap = getenv("COCO3_KEYGAP");   // fields between keys
        int idle = (row == 6 && col == 0) ? 20 : (gap ? atoi(gap) : 3);
        for (int f = 0; f < idle; f++) c3_run_ticks(FRAME_TICKS);
    }
}

static void print_screen(void) {
    char row[33];
    for (int r = 0; r < 16; r++) { screen_row(r, row); printf("  |%s|\n", row); }
}

static void test_typing(void) {
    if (!load_rom()) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with coco3.rom (32 KB)");
    TEST_ASSERT_TRUE(c3_init(g_rom, 1));
    c3_set_port_render(1);
    for (int f = 0; f < 180; f++) c3_run_ticks(FRAME_TICKS);
    type("PRINT 6*7+(1<2)\n");
    for (int f = 0; f < 30; f++) c3_run_ticks(FRAME_TICKS);
    print_screen();
    TEST_ASSERT_TRUE_MESSAGE(screen_has(" 41 "), "PRINT 6*7+(1<2) did not show 41");
    // COCO3_TYPE: type more (| is ENTER), run COCO3_FRAMES fields (default
    // 120), then COCO3_DUMP has the picture.
    const char *more = getenv("COCO3_TYPE");
    if (more && more[0]) {
        type(more);
        const char *nf = getenv("COCO3_FRAMES");
        int frames = nf ? atoi(nf) : 120;
        for (int f = 0; f < frames; f++) c3_run_ticks(FRAME_TICKS);
        print_screen();
        printf("  gime ff90=%02x ff91=%02x ff92=%02x ff93=%02x ff94=%02x ff95=%02x ff98=%02x ff99=%02x\n",
               c3_gime_reg(0), c3_gime_reg(1), c3_gime_reg(2), c3_gime_reg(3), c3_gime_reg(4),
               c3_gime_reg(5), c3_gime_reg(8), c3_gime_reg(9));
        printf("  palette");
        for (int i = 0; i < 16; i++) printf(" %d", c3_gime_reg(16 + i));
        printf("\n");
        dump();
        const char *rp = getenv("COCO3_RAM");    // all 128 KB, physical order
        FILE *f = rp && rp[0] ? fopen(rp, "wb") : NULL;
        if (f) { for (uint32_t z = 0; z < 0x20000; z++) fputc(c3_phys(z), f); fclose(f); }
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_row_skip);
    RUN_TEST(test_boots_to_ok);
    RUN_TEST(test_fast_path_matches);
    RUN_TEST(test_fast_path_remap);
    RUN_TEST(test_port_render);
    RUN_TEST(test_typing);
    return UNITY_END();
}

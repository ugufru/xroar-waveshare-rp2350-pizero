// test_mc6809_lockstep.cpp: the 6809 correctness harness (PIZERO-129).
//
// Two copies of the 6809 core run in one binary: the frozen reference
// (test/mc6809_reference, today's core) and the live one (lib/xroar_core,
// the one PIZERO-131 will optimise). Both execute the same program from the
// same 64 KB, one instruction per step, and after every step the registers
// and every bus cycle of that instruction (address, R/W, data, dummy cycles
// included) must match. The first difference fails the test with the
// scenario, the step, the PC and the cycle.
//
// Inputs: seeded random memory (every byte is an opcode sooner or later,
// over all three pages and every addressing mode) with a random schedule of
// NMI, FIRQ, IRQ and HALT; a sweep of all 256 indexed postbytes; a sweep of
// the page 2 and page 3 opcodes; the interrupt instructions; and, when
// COCO_ROM_DIR names a folder with bas12.rom and extbas11.rom, a real Color
// BASIC boot for a few million instructions (the repo ships no ROMs, so
// that one is skipped otherwise). Two self-checks prove the comparator sees
// a one-flag and a one-cycle change.
//
//   pio test -e native -f test_mc6809_lockstep
//   COCO_ROM_DIR=~/Desktop/coco/roms pio test -e native -f test_mc6809_lockstep

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lockstep_core.h"

void setUp(void) {}
void tearDown(void) {}

// - - - a tiny PRNG, so every run is the same run - - - - - - - - - - - - -
static uint32_t g_rng;
static uint32_t rnd(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}

// - - - the pair - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void *g_ref, *g_work;
static char g_msg[400];

static void pair_create(void) {
    if (!g_ref)  g_ref  = ls_ref.create();
    if (!g_work) g_work = ls_work.create();
}

static void pair_fill(const uint8_t *image) {
    memcpy(ls_ref.mem, image, 65536);
    memcpy(ls_work.mem, image, 65536);
}

static void pair_reset(void) {
    *ls_ref.inject_extra_cycle_at = *ls_work.inject_extra_cycle_at = 0;
    *ls_ref.rom_base = *ls_work.rom_base = 0;
    ls_ref.reset(g_ref);
    ls_work.reset(g_work);
    // A 6809 reset leaves most registers as they were, so a scenario's
    // leftovers (a self-check's flipped flag, say) would follow it into the
    // next one. Start the pair from one register file.
    struct ls_regs r;
    ls_ref.get_regs(g_ref, &r);
    ls_work.set_regs(g_work, &r);
}

static void pair_lines(int halt, int nmi, int firq, int irq) {
    ls_ref.set_lines(g_ref, halt, nmi, firq, irq);
    ls_work.set_lines(g_work, halt, nmi, firq, irq);
}

// One step of both cores, then the comparison. Returns NULL when they
// agree, else the message describing the first difference.
static const char *pair_step(const char *scenario, unsigned step, unsigned cap) {
    struct ls_regs before;
    ls_ref.get_regs(g_ref, &before);
    ls_ref.step(g_ref, cap);
    ls_work.step(g_work, cap);
    unsigned nr = *ls_ref.nbus, nw = *ls_work.nbus;
    unsigned n = nr < nw ? nr : nw;
    if (n > LS_BUS_MAX) n = LS_BUS_MAX;
    for (unsigned i = 0; i < n; i++) {
        const struct ls_bus *r = &ls_ref.bus[i], *w = &ls_work.bus[i];
        if (r->a != w->a || r->rnw != w->rnw || r->d != w->d) {
            snprintf(g_msg, sizeof g_msg,
                     "%s: step %u at pc=%04X, bus cycle %u of %u/%u: reference %c %04X=%02X, working %c %04X=%02X",
                     scenario, step, before.pc, i, nr, nw,
                     r->rnw ? 'R' : 'W', r->a, r->d, w->rnw ? 'R' : 'W', w->a, w->d);
            return g_msg;
        }
    }
    if (nr != nw) {
        snprintf(g_msg, sizeof g_msg, "%s: step %u at pc=%04X: reference took %u bus cycles, working %u",
                 scenario, step, before.pc, nr, nw);
        return g_msg;
    }
    struct ls_regs r, w;
    ls_ref.get_regs(g_ref, &r);
    ls_work.get_regs(g_work, &w);
    if (memcmp(&r, &w, sizeof r) != 0) {
        snprintf(g_msg, sizeof g_msg,
                 "%s: step %u at pc=%04X, registers after it: reference cc=%02X a=%02X b=%02X dp=%02X x=%04X y=%04X u=%04X s=%04X pc=%04X nmi_armed=%u latches=%u%u%u;"
                 " working cc=%02X a=%02X b=%02X dp=%02X x=%04X y=%04X u=%04X s=%04X pc=%04X nmi_armed=%u latches=%u%u%u",
                 scenario, step, before.pc,
                 r.cc, r.a, r.b, r.dp, r.x, r.y, r.u, r.s, r.pc, r.nmi_armed, r.nmi_latch, r.firq_latch, r.irq_latch,
                 w.cc, w.a, w.b, w.dp, w.x, w.y, w.u, w.s, w.pc, w.nmi_armed, w.nmi_latch, w.firq_latch, w.irq_latch);
        return g_msg;
    }
    return NULL;
}

// Run `steps` steps with the random interrupt schedule the seed gives.
// Lines are set before a step and both cores see the same levels.
static const char *run_scenario(const char *scenario, unsigned steps, unsigned cap, bool interrupts) {
    int halt = 0, nmi = 0, firq = 0, irq = 0, hold = 0;
    for (unsigned s = 0; s < steps; s++) {
        if (interrupts) {
            if (hold > 0) {
                hold--;
                if (hold == 0) { halt = nmi = firq = irq = 0; pair_lines(0, 0, 0, 0); }
            } else if ((rnd() % 61) == 0) {
                switch (rnd() % 7) {
                case 0: nmi = 1;  hold = 1; break;                 // a pulse
                case 1: firq = 1; hold = 1 + rnd() % 8; break;     // held a few instructions
                case 2: irq = 1;  hold = 1 + rnd() % 8; break;
                case 3: halt = 1; hold = 1 + rnd() % 4; break;     // dummy cycles while halted
                case 4: firq = irq = 1; hold = 1 + rnd() % 4; break;
                default: irq = 1; hold = 20 + rnd() % 40; break;   // long enough to wake a SYNC
                }
                pair_lines(halt, nmi, firq, irq);
            }
        }
        const char *d = pair_step(scenario, s, cap);
        if (d) return d;
    }
    return NULL;
}

static uint8_t g_image[65536];

// - - - the scenarios - - - - - - - - - - - - - - - - - - - - - - - - - -

static void test_random_streams(void) {
    pair_create();
    for (unsigned seed = 1; seed <= 24; seed++) {
        g_rng = seed * 2654435761u;
        for (unsigned i = 0; i < 65536; i++) g_image[i] = (uint8_t)rnd();
        pair_fill(g_image);
        pair_reset();
        char name[40];
        snprintf(name, sizeof name, "random seed %u", seed);
        const char *d = run_scenario(name, 15000, 200, true);
        if (d) TEST_FAIL_MESSAGE(d);
        TEST_ASSERT_TRUE_MESSAGE(*ls_ref.cycles > 15000, "a seed ran almost nothing");
    }
}

// Every indexed postbyte, through LEAX, LDA and STA, with random operand
// bytes after it (a postbyte that takes none makes them the next opcodes,
// which is fine: both cores see the same bytes).
static void test_indexed_postbyte_sweep(void) {
    pair_create();
    g_rng = 0x1DEA;
    for (unsigned i = 0; i < 65536; i++) g_image[i] = (uint8_t)rnd();
    unsigned p = 0x1000;
    for (unsigned pb = 0; pb < 256; pb++) {
        g_image[p++] = 0x30; g_image[p++] = (uint8_t)pb; g_image[p++] = (uint8_t)rnd(); g_image[p++] = (uint8_t)rnd();
        g_image[p++] = 0xA6; g_image[p++] = (uint8_t)pb; g_image[p++] = (uint8_t)rnd(); g_image[p++] = (uint8_t)rnd();
        g_image[p++] = 0xA7; g_image[p++] = (uint8_t)pb; g_image[p++] = (uint8_t)rnd(); g_image[p++] = (uint8_t)rnd();
    }
    g_image[p++] = 0x7E; g_image[p++] = 0x10; g_image[p++] = 0x00;     // JMP $1000
    g_image[0xFFFE] = 0x10; g_image[0xFFFF] = 0x00;
    pair_fill(g_image);
    pair_reset();
    const char *d = run_scenario("indexed postbyte sweep", 6000, 200, false);
    if (d) TEST_FAIL_MESSAGE(d);
}

// Page 2 and page 3: every second byte, with zero operands, and the
// vectors pointing back at the sweep so SWI2 and SWI3 return to it.
static void test_page2_page3_sweep(void) {
    pair_create();
    memset(g_image, 0x12, sizeof g_image);                              // NOP everywhere else
    unsigned p = 0x2000;
    for (unsigned op = 0; op < 256; op++) {
        g_image[p++] = 0x10; g_image[p++] = (uint8_t)op; g_image[p++] = 0x40; g_image[p++] = 0x00;
        g_image[p++] = 0x11; g_image[p++] = (uint8_t)op; g_image[p++] = 0x40; g_image[p++] = 0x00;
    }
    g_image[p++] = 0x7E; g_image[p++] = 0x20; g_image[p++] = 0x00;
    for (unsigned v = 0xFFF0; v < 0xFFFE; v += 2) { g_image[v] = 0x30; g_image[v + 1] = 0x00; }
    g_image[0x3000] = 0x3B;                                              // RTI for every interrupt
    g_image[0xFFFE] = 0x20; g_image[0xFFFF] = 0x00;
    pair_fill(g_image);
    pair_reset();
    const char *d = run_scenario("page 2/3 opcode sweep", 6000, 200, true);
    if (d) TEST_FAIL_MESSAGE(d);
}

// SYNC, CWAI, the three SWIs and RTI, with the lines driven on a schedule.
static void test_interrupt_instructions(void) {
    pair_create();
    memset(g_image, 0x12, sizeof g_image);
    static const uint8_t prog[] = {
        0x10, 0xCE, 0x7F, 0x00,     // LDS #$7F00 (arms NMI)
        0x1C, 0xAF,                 // ANDCC #$AF: IRQ and FIRQ on
        0x13,                       // SYNC
        0x3C, 0xEF,                 // CWAI #$EF
        0x3F,                       // SWI
        0x10, 0x3F,                 // SWI2
        0x11, 0x3F,                 // SWI3
        0x86, 0x55, 0x97, 0x10,     // LDA #$55; STA <$10
        0x20, 0xF2,                 // BRA back to SYNC
    };
    memcpy(&g_image[0x0100], prog, sizeof prog);
    g_image[0x0200] = 0x3B;                                              // RTI
    g_image[0x0210] = 0x4C; g_image[0x0211] = 0x3B;                      // INCA; RTI (FIRQ)
    for (unsigned v = 0xFFF2; v < 0xFFFE; v += 2) { g_image[v] = 0x02; g_image[v + 1] = 0x00; }
    g_image[0xFFF6] = 0x02; g_image[0xFFF7] = 0x10;
    g_image[0xFFFE] = 0x01; g_image[0xFFFF] = 0x00;
    pair_fill(g_image);
    pair_reset();
    for (unsigned s = 0; s < 4000; s++) {
        pair_lines(0, (s % 13) == 5, (s % 11) == 3 || (s % 11) == 4, (s % 7) == 2 || (s % 7) == 3);
        const char *d = pair_step("interrupt instructions", s, 200);
        if (d) TEST_FAIL_MESSAGE(d);
    }
}

// - - - the comparator must see a one-flag and a one-cycle change - - - -

static void test_comparator_sees_one_flag(void) {
    pair_create();
    g_rng = 77;
    for (unsigned i = 0; i < 65536; i++) g_image[i] = (uint8_t)rnd();
    pair_fill(g_image);
    pair_reset();
    const char *d = run_scenario("flag self-check", 500, 200, false);
    TEST_ASSERT_NULL_MESSAGE(d, d);
    struct ls_regs r;
    ls_work.get_regs(g_work, &r);
    r.cc ^= 0x01;                                                       // one flag, C
    ls_work.set_regs(g_work, &r);
    d = NULL;
    for (unsigned s = 0; s < 50 && !d; s++) d = pair_step("flag self-check", 500 + s, 200);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "a flipped carry went unnoticed for 50 instructions");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(d, "registers after it") ? d : (strstr(d, "bus cycle") ? d : NULL), d);
}

static void test_comparator_sees_one_cycle(void) {
    pair_create();
    g_rng = 78;
    for (unsigned i = 0; i < 65536; i++) g_image[i] = (uint8_t)rnd();
    pair_fill(g_image);
    pair_reset();
    const char *d = run_scenario("cycle self-check", 500, 200, false);
    TEST_ASSERT_NULL_MESSAGE(d, d);
    *ls_work.inject_extra_cycle_at = *ls_work.cycles + 3;              // one dummy cycle too many
    d = NULL;
    for (unsigned s = 0; s < 50 && !d; s++) d = pair_step("cycle self-check", 500 + s, 200);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "an extra bus cycle went unnoticed for 50 instructions");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(d, "bus cycle"), d);
}

// - - - a real ROM, when one is to hand - - - - - - - - - - - - - - - - -

static bool load_rom(const char *dir, const char *name, unsigned at, unsigned size) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(&g_image[at], 1, size, f);
    fclose(f);
    return n == size;
}

static void test_color_basic_boot(void) {
    const char *dir = getenv("COCO_ROM_DIR");
    if (!dir || !dir[0]) TEST_IGNORE_MESSAGE("set COCO_ROM_DIR to a folder with bas12.rom and extbas11.rom");
    pair_create();
    memset(g_image, 0, sizeof g_image);
    if (!load_rom(dir, "bas12.rom", 0xA000, 8192)) TEST_IGNORE_MESSAGE("no bas12.rom in COCO_ROM_DIR");
    load_rom(dir, "extbas11.rom", 0x8000, 8192);                        // optional
    memcpy(&g_image[0xFFF0], &g_image[0xBFF0], 16);                      // the SAM maps the vectors from ROM
    pair_fill(g_image);
    pair_reset();
    *ls_ref.rom_base = *ls_work.rom_base = 0x8000;
    // BASIC's 60 Hz interrupt, roughly: a pulse every 2000 instructions.
    for (unsigned s = 0; s < 3000000; s++) {
        pair_lines(0, 0, 0, (s % 2000) == 0);
        const char *d = pair_step("Color BASIC boot", s, 200);
        if (d) TEST_FAIL_MESSAGE(d);
    }
    TEST_ASSERT_TRUE(*ls_ref.cycles > 3000000);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_random_streams);
    RUN_TEST(test_indexed_postbyte_sweep);
    RUN_TEST(test_page2_page3_sweep);
    RUN_TEST(test_interrupt_instructions);
    RUN_TEST(test_comparator_sees_one_flag);
    RUN_TEST(test_comparator_sees_one_cycle);
    RUN_TEST(test_color_basic_boot);
    return UNITY_END();
}

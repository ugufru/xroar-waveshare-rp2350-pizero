// c3host.c: a bare 128 KB CoCo 3 on the host (PIZERO-133).
//
// The firmware's own CPU core, PIAs and the vendored GIME, wired the way
// upstream's src/coco3.c wires them, with coco3_bus.h doing what the
// firmware's machine does with the GIME's decode. No sound, disk or
// cartridge: enough to boot Super Extended Color BASIC and look at it.
// PIZERO-214: and a keyboard matrix, so a test can type at BASIC.
//
// PIZERO-213: c3_init's fast flag turns on the firmware's fast path, the
// 6809 core reading and writing whole MMU pages itself (coco_fastbus.h), so
// a run with it can be compared with a run without.

#define COCO_FASTBUS_PAGES 1     // as -DCOCO3 sets it in the firmware

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "delegate.h"
#include "events.h"
#include "part.h"
#include "xroar.h"
#include "mc6809/mc6809.h"
#include "mc6821.h"
#include "tcc1014/tcc1014.h"
#include "coco_fastbus.h"
#include "../../lib/coco_machine/src/coco3_bus.h"
// stdbool's bool macro would turn the delegate type names below into
// delegate_void__Bool (see lib/xroar_core/include/hot.h); this file says _Bool.
#undef bool

#include "c3host.h"

extern const struct partdb_entry mc6809_part, mc6821_part, tcc1014_1986_part, tcc1014_1987_part;

void *part_new(size_t psize) { return calloc(1, psize); }   // part.c, without its partdb

struct coco_fastbus coco_fastbus;          // empty pages: every cycle takes the hook

static struct MC6809 *cpu;
static struct MC6821 *pia0, *pia1;
static struct TCC1014 *gime;
static struct coco3_bus bus;
static uint8_t ram[2][65536];
static uint8_t rom[COCO3_ROM_SIZE];
static uint16_t fb[COCO3_FB_W * COCO3_FB_H];
static event_ticks run_end;                // event tick at which this run stops
static int fast;                           // PIZERO-213: the fast path is on
static uint8_t keys[8];                    // PIZERO-214: rows held, per column
static uint8_t gime_regs[32];              // $FF90-$FF9F, $FFB0-$FFBF as last written

static struct part *make(const struct partdb_entry *e, void *options) {
    struct part *p = e->funcs->allocate();
    *p = (struct part){0};
    p->partdb = e;
    if (e->funcs->initialise) e->funcs->initialise(p, options);
    if (e->funcs->finish && !e->funcs->finish(p)) return NULL;
    return p;
}

// The core calls these by name (mc6809_common.c).
void coco_mem_cycle(void *sptr, _Bool RnW, uint16_t A) {
    (void)sptr;
    tcc1014_mem_cycle(gime, RnW, A);
}
void coco_mem_nvma(void *sptr) {
    (void)sptr;
    tcc1014_mem_cycle(gime, 1, 0xFFFF);
}
static void cycle_tail(int ncycles);

// A cycle the core did itself that ends at or after the deadline: the
// GIME's edge detect and the call-back's tail, without the decode or the
// bus, as the firmware does (PIZERO-220).
void coco_mem_tick(void *sptr, _Bool RnW, uint16_t A) {
    (void)sptr; (void)RnW; (void)A;
    if (!fast) abort();                    // no pages, so never reached
    tcc1014_poll_lines(gime);
    gime->IL1 = (PIA_VALUE_A(pia0) | 0x80) != 0xff;
    cycle_tail(coco_fastbus.ticks);
}

// The pages as the GIME's decode stands now (the firmware's fastbus_sync).
static void map_pages(void) {
    coco_fastbus.ram_top = coco_fastbus.rom_top = coco_fastbus.cart_top = 0;
    if (!fast) {
        coco_fastbus.page_top = 0;
        return;
    }
    for (unsigned p = 0; p < 8; p++) {
        unsigned bank;
        unsigned S = tcc1014_decode_page(gime, p, &bank);
        coco3_bus_page(&bus, p, S, bank, &coco_fastbus.rd[p], &coco_fastbus.wr[p]);
    }
    coco_fastbus.rom = rom + 0x4000;
    coco_fastbus.ticks = coco_fastbus.rom_ticks = (uint16_t)tcc1014_cycle_ticks(gime);
    coco_fastbus.page_top = 0xFE00;
}

// The next tick a cycle must take the full path: the next event, or the
// end of the run.
static void set_deadline(void) {
    event_ticks dl = run_end;
    struct event *head = MACHINE_EVENT_LIST->events;
    if (head && event_tick_delta(head->at_tick, dl) < 0) dl = head->at_tick;
    coco_fastbus.deadline = fast ? dl : event_current_tick;
}

// The GIME's call-back for every cycle, as upstream's cpu_cycle.
static void cpu_cycle(void *sptr, int ncycles, _Bool RnW, uint16_t A) {
    (void)sptr;
    event_run_queue(MACHINE_EVENT_LIST, ncycles);
    if (event_tick_delta(event_current_tick, run_end) >= 0) cpu->running = 0;
    switch (coco3_bus_cycle(&bus, gime->S, gime->RAS, gime->Z, RnW, A, &cpu->D)) {
    case C3_BUS_PIA0:
        if (RnW) cpu->D = mc6821_read(pia0, A); else mc6821_write(pia0, A, cpu->D);
        break;
    case C3_BUS_PIA1:
        if (RnW) cpu->D = mc6821_read(pia1, A); else mc6821_write(pia1, A, cpu->D);
        break;
    default:
        break;
    }
    if (!RnW && (A & 0xFFF0) == 0xFF90) gime_regs[A & 15] = cpu->D;
    if (!RnW && (A & 0xFFF0) == 0xFFB0) gime_regs[16 + (A & 15)] = cpu->D & 0x3F;
    if (!RnW && ((A & 0xFFFE) == 0xFF90 || (A & 0xFFF0) == 0xFFA0 || (A & 0xFFE0) == 0xFFC0))
        map_pages();
    gime->IL1 = (PIA_VALUE_A(pia0) | 0x80) != 0xff;
    // The interrupt lines as this cycle leaves them, as the firmware's
    // mem_cycle_tail does. Upstream sets them at the start of the next cycle,
    // which is the same for a machine that takes every cycle here, but the
    // fast path's cycles in between would keep a stale line.
    MC6809_IRQ_SET(cpu, pia0->a.irq || pia0->b.irq || gime->IRQ);
    MC6809_FIRQ_SET(cpu, pia1->a.irq || pia1->b.irq || gime->FIRQ);
    set_deadline();
}

// The call-back's tail for a cycle the core did itself (coco_mem_tick).
static void cycle_tail(int ncycles) {
    event_run_queue(MACHINE_EVENT_LIST, ncycles);
    if (event_tick_delta(event_current_tick, run_end) >= 0) cpu->running = 0;
    MC6809_IRQ_SET(cpu, pia0->a.irq || pia0->b.irq || gime->IRQ);
    MC6809_FIRQ_SET(cpu, pia1->a.irq || pia1->b.irq || gime->FIRQ);
    set_deadline();
}

// PIZERO-214: the matrix as upstream's keyboard does it: a column PIA0 B
// drives low pulls its held keys' rows low on PIA0 A.
static void kbd_preread(void *sptr) {
    (void)sptr;
    uint8_t cols = PIA_VALUE_B(pia0), rows = 0xff;
    for (int c = 0; c < 8; c++)
        if (!(cols & (1 << c))) rows &= ~keys[c];
    pia0->a.in_sink = rows;
}

static uint16_t fetch_vram(void *sptr, uint32_t A) { (void)sptr; return coco3_bus_fetch_vram(&bus, A); }
static void gime_hs(void *sptr, _Bool level) { (void)sptr; mc6821_set_cx1(&pia0->a, level); }
static void gime_fs(void *sptr, _Bool level) {
    (void)sptr;
    mc6821_set_cx1(&pia0->b, level);
    coco3_bus_frame_sync(&bus, level);
}
static void render_line(void *sptr, unsigned burst, unsigned npixels, const uint8_t *data) {
    (void)sptr; (void)burst;
    if (npixels == TCC1014_PORT_JOB) {
        // PIZERO-330: a line handed over undecoded; decode it as the port's
        // other core would, then on as a drawn line.
        static uint8_t line[TCC1014_PORT_W + 16];
        tcc1014_port_decode((const struct tcc1014_port_job *)data, line);
        coco3_bus_render_line(&bus, line, TCC1014_PORT_W);
        return;
    }
    coco3_bus_render_line(&bus, data, npixels);
}
static void set_active_area(void *sptr, int x, int y, int w, int h) {
    (void)sptr;
    if (coco3_bus_set_active_area(&bus, x, y, w, h)) gime->port_redraw = 1;
}

int c3_init(const uint8_t *rom32k, int fast_path) {
    fast = fast_path;
    memset(&coco_fastbus, 0, sizeof coco_fastbus);
    memcpy(rom, rom32k, sizeof rom);
    memset(ram, 0, sizeof ram);
    machine_event_list_global = event_list_new();
    event_current_tick = 0;
    coco3_bus_init(&bus, ram[0], ram[1], rom, fb);

    cpu  = (struct MC6809 *)make(&mc6809_part, NULL);
    pia0 = (struct MC6821 *)make(&mc6821_part, NULL);
    pia1 = (struct MC6821 *)make(&mc6821_part, NULL);
    gime = (struct TCC1014 *)make(&tcc1014_1987_part, NULL);
    if (!cpu || !pia0 || !pia1 || !gime) return 0;

    gime->cpu_cycle = DELEGATE_AS3(void, int, bool, uint16, cpu_cycle, NULL);
    gime->fetch_vram = DELEGATE_AS1(uint16, uint32, fetch_vram, NULL);
    gime->signal_hs = DELEGATE_AS1(void, bool, gime_hs, NULL);
    gime->signal_fs = DELEGATE_AS1(void, bool, gime_fs, NULL);
    gime->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp, render_line, NULL);
    gime->set_active_area = DELEGATE_AS4(void, int, int, int, int, set_active_area, NULL);
    gime->CPUD = &cpu->D;

    // As upstream's coco3_finish: nothing on the ports, serial in held low.
    pia0->b.in_source = 0;
    pia1->b.in_source = 0;
    pia0->a.in_sink = pia0->b.in_sink = 0xff;
    pia1->a.in_sink = pia1->b.in_sink = 0xff;
    pia1->b.in_sink &= ~(1 << 0);
    memset(keys, 0, sizeof keys);
    memset(gime_regs, 0, sizeof gime_regs);
    pia0->a.data_preread = DELEGATE_AS0(void, kbd_preread, NULL);

    tcc1014_notify_mode(gime);
    mc6821_reset(pia0);
    mc6821_reset(pia1);
    tcc1014_reset(gime);
    map_pages();
    cpu->reset(cpu);
    return 1;
}

void c3_run_ticks(int32_t ticks) {
    run_end = event_current_tick + (event_ticks)ticks;
    while (event_tick_delta(event_current_tick, run_end) < 0) {
        set_deadline();
        cpu->running = 1;
        cpu->run(cpu);
    }
}

void c3_state(struct c3_state *st) {
    st->tick = event_current_tick;
    st->pc = cpu->reg_pc;
    st->s = cpu->reg_s;
    st->u = cpu->reg_u;
    st->x = cpu->reg_x;
    st->y = cpu->reg_y;
    st->cc = cpu->reg_cc;
}

uint16_t c3_pc(void) { return cpu->reg_pc; }
const uint16_t *c3_fb(void) { return fb; }

uint8_t c3_phys(uint32_t Z) { return *coco3_ram_at(&bus, Z); }
void c3_poke(uint32_t Z, uint8_t v) { *coco3_ram_at(&bus, Z) = v; }
void c3_set_pc(uint16_t pc) { cpu->reg_pc = pc; }
void c3_key(int col, int row, int down) {
    if (down) keys[col & 7] |= (uint8_t)(1 << row);
    else keys[col & 7] &= (uint8_t)~(1 << row);
}
uint8_t c3_gime_reg(int r) { return gime_regs[r & 31]; }
void c3_set_port_render(int on) {
    gime->port_render = on;
    gime->port_vram[0] = on == 1 || on == 3 ? ram[0] : NULL;   // 2: through fetch_vram
    gime->port_vram[1] = on == 1 || on == 3 ? ram[1] : NULL;
    gime->port_defer = on == 3;                                // PIZERO-330: as jobs
}

// lockstep_core.inc: the wrapper body, included at the end of core_ref.c
// and core_work.c after the core itself, with LS_PREFIX set. Everything the
// core calls (coco_mem_cycle, renamed per core) and everything the test
// calls lives here, in the core's own translation unit, so the static
// functions of the core (mc6809_allocate, mc6809_initialise) are reachable
// and struct MC6809 is the one this core was compiled with.

#include <string.h>
#include "lockstep_core.h"

#define LS_CAT2(a, b) a##_##b
#define LS_CAT(a, b) LS_CAT2(a, b)
#define LS(name) LS_CAT(LS_PREFIX, name)

static uint8_t  LS(mem)[65536];
static struct ls_bus LS(bus)[LS_BUS_MAX];
static unsigned LS(nbus);
static unsigned LS(cycles);
static unsigned LS(cap);
static unsigned LS(step_cycles);
static unsigned LS(inject_at);
static uint16_t LS(rom_base);

// The core's bus cycle. Reads come from memory, writes go to it (unless in
// ROM), and every cycle is recorded. The step ends when the cap is hit, so
// a SYNC or HALT wait cannot run away.
void LS(mem_cycle)(void *sptr, _Bool RnW, uint16_t A) {
    struct MC6809 *cpu = (struct MC6809 *)sptr;
    if (RnW) cpu->D = LS(mem)[A];
    else if (!LS(rom_base) || A < LS(rom_base)) LS(mem)[A] = cpu->D;
    if (LS(nbus) < LS_BUS_MAX) {
        LS(bus)[LS(nbus)].a = A;
        LS(bus)[LS(nbus)].rnw = RnW;
        LS(bus)[LS(nbus)].d = cpu->D;
    }
    LS(nbus)++;
    LS(cycles)++;
    if (LS(inject_at) && LS(cycles) == LS(inject_at)) {
        LS(inject_at) = 0;
        LS(mem_cycle)(sptr, 1, 0xFFFF);       // one dummy cycle too many
        LS(step_cycles)--;                    // outside the cap, so a capped wait still shows it
    }
    if (++LS(step_cycles) >= LS(cap)) cpu->running = 0;
}

static void LS(posthook)(void *sptr) {
    struct MC6809 *cpu = (struct MC6809 *)sptr;
    cpu->running = 0;                         // one instruction per step
}

static void *LS(create)(void) {
    struct part *p = mc6809_allocate();
    struct MC6809 *cpu = (struct MC6809 *)p;
    cpu->mem_cycle.sptr = cpu;               // the core calls LS(mem_cycle) directly
    cpu->debug_cpu.instruction_posthook = DELEGATE_AS0(void, LS(posthook), cpu);
    mc6809_initialise(p, NULL);
    return cpu;
}

static void LS(reset)(void *vcpu) {
    struct MC6809 *cpu = (struct MC6809 *)vcpu;
    cpu->reset(cpu);
    LS(cycles) = 0;
    LS(nbus) = 0;
}

static void LS(step)(void *vcpu, unsigned cycle_cap) {
    struct MC6809 *cpu = (struct MC6809 *)vcpu;
    LS(nbus) = 0;
    LS(step_cycles) = 0;
    LS(cap) = cycle_cap ? cycle_cap : 1;
    cpu->running = 1;
    cpu->run(cpu);
}

static void LS(get_regs)(void *vcpu, struct ls_regs *o) {
    struct MC6809 *cpu = (struct MC6809 *)vcpu;
    o->cc = cpu->reg_cc; o->dp = cpu->reg_dp;
    o->a = MC6809_REG_A(cpu); o->b = MC6809_REG_B(cpu);
    o->x = cpu->reg_x; o->y = cpu->reg_y; o->u = cpu->reg_u; o->s = cpu->reg_s; o->pc = cpu->reg_pc;
    o->nmi_armed = cpu->nmi_armed;
    o->nmi_latch = cpu->nmi_latch; o->firq_latch = cpu->firq_latch; o->irq_latch = cpu->irq_latch;
}

static void LS(set_regs)(void *vcpu, const struct ls_regs *i) {
    struct MC6809 *cpu = (struct MC6809 *)vcpu;
    cpu->reg_cc = i->cc; cpu->reg_dp = i->dp;
    MC6809_REG_A(cpu) = i->a; MC6809_REG_B(cpu) = i->b;
    cpu->reg_x = i->x; cpu->reg_y = i->y; cpu->reg_u = i->u; cpu->reg_s = i->s; cpu->reg_pc = i->pc;
    cpu->nmi_armed = i->nmi_armed;
    cpu->nmi_latch = i->nmi_latch; cpu->firq_latch = i->firq_latch; cpu->irq_latch = i->irq_latch;
}

static void LS(set_lines)(void *vcpu, int halt, int nmi, int firq, int irq) {
    struct MC6809 *cpu = (struct MC6809 *)vcpu;
    cpu->halt = halt; cpu->nmi = nmi; cpu->firq = firq; cpu->irq = irq;
}

const struct ls_core LS_TABLE = {
    LS_NAME, LS(mem), LS(bus), &LS(nbus), &LS(cycles),
    LS(create), LS(reset), LS(step), LS(get_regs), LS(set_regs), LS(set_lines),
    &LS(inject_at), &LS(rom_base),
};

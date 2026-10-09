// lockstep_core.h: one 6809 core behind a prefix, for the lockstep test
// (PIZERO-129). core_ref.c and core_work.c each compile a whole core (the
// frozen reference and the live one) in their own translation unit, with
// every external symbol renamed, and expose it through this table. The test
// never touches struct MC6809 itself, so the two cores may differ in layout.

#ifndef LOCKSTEP_CORE_H
#define LOCKSTEP_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The registers at an instruction boundary. `state` and `page` are left
// out on purpose: an optimised core may restructure its dispatch, and the
// architectural registers plus the interrupt latches are what must match.
struct ls_regs {
    uint8_t  cc, dp, a, b;
    uint16_t x, y, u, s, pc;
    uint8_t  nmi_armed, nmi_latch, firq_latch, irq_latch;
};

// One bus cycle: the address, read (1) or write (0), and the data byte on
// the bus. A dummy cycle is a read of $FFFF, as the core makes it.
struct ls_bus {
    uint16_t a;
    uint8_t  rnw;
    uint8_t  d;
};

#define LS_BUS_MAX 4096        // cycles one step may record (a step is one instruction, or the cap)

struct ls_core {
    const char *name;
    uint8_t   *mem;            // its 64 KB
    struct ls_bus *bus;        // this step's cycles
    unsigned  *nbus;           // how many
    unsigned  *cycles;         // bus cycles since reset
    void *(*create)(void);     // allocate and reset
    void  (*reset)(void *cpu);
    void  (*step)(void *cpu, unsigned cycle_cap);   // one instruction, or cycle_cap bus cycles
    void  (*get_regs)(void *cpu, struct ls_regs *out);
    void  (*set_regs)(void *cpu, const struct ls_regs *in);   // for the comparator self-check
    void  (*set_lines)(void *cpu, int halt, int nmi, int firq, int irq);
    // Self-check: record one extra dummy cycle when the cycle count reaches
    // this value (0 = never). Proves the comparator sees a one-cycle change.
    unsigned *inject_extra_cycle_at;
    // Reads at or above this address come from ROM, writes there are
    // dropped (0 = all RAM). For the optional ROM boot.
    uint16_t *rom_base;
};

extern const struct ls_core ls_ref;
extern const struct ls_core ls_work;

#ifdef __cplusplus
}
#endif
#endif

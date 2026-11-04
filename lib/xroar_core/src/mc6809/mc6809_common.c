/** \file
 *
 *  \brief Motorola MC6809-compatible common functions.
 *
 *  \copyright Copyright 2003-2021 Ciaran Anscomb
 *
 *  \licenseblock This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 *  XRoar is free software; you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  See COPYING.GPL for redistribution conditions.
 *
 *  \endlicenseblock
 *
 *  This file is included directly into other source files.  It provides
 *  functions common across 6809 ISA CPUs.
 */

/*
 * Register handling macros
 */

#define REG_CC (cpu->reg_cc)
#define REG_A (MC6809_REG_A(cpu))
#define REG_B (MC6809_REG_B(cpu))
#define REG_D (cpu->reg_d)
#define REG_DP (cpu->reg_dp)
#define REG_X (cpu->reg_x)
#define REG_Y (cpu->reg_y)
#define REG_U (cpu->reg_u)
#define REG_S (cpu->reg_s)
#define REG_PC (cpu->reg_pc)

// Condition code register macros

#define CC_E (0x80)
#define CC_F (0x40)
#define CC_H (0x20)
#define CC_I (0x10)
#define CC_N (0x08)
#define CC_Z (0x04)
#define CC_V (0x02)
#define CC_C (0x01)

/*
 * Memory interface
 */

// AMOLED-56: devirtualize the mem_cycle delegate. cpu->mem_cycle is set
// once at init to coco_mem_cycle and never reassigned. The DELEGATE_CALL
// macro expands to an indirect function-pointer call through the cpu
// struct — opaque to the compiler without LTO (which our toolchain
// lacks). Calling coco_mem_cycle directly lets the compiler inline it
// straight into the 6809 hot loop.
extern void coco_mem_cycle(void *sptr, _Bool RnW, uint16_t A);
// PIZERO-131: the dummy cycle's own entry. Same bus cycle as a read of
// $FFFF, without the address decode.
extern void coco_mem_nvma(void *sptr);

#include "cpu_prof.h"   // PIZERO-209: empty unless CPU_PROF
#include "coco_fastbus.h"  // PIZERO-131: RAM and ROM without the decode

// PIZERO-131: RAM, BASIC ROM and cartridge ROM are read here, and RAM
// written, without coco_mem_cycle's address decode. The cycle then only
// advances time unless something is due, when coco_mem_tick does the
// per-cycle work. I/O and the SAM registers still take the full hook.
// PIZERO-132: `ticks` is the cycle's length, which on a CoCo 2 at address-
// dependent speed differs between RAM and ROM.
#define COCO_TICK(rnw, a, ticks) do { \
		if (!coco_fastbus_advance(ticks)) coco_mem_tick(cpu->mem_cycle.sptr, (rnw), (a)); \
	} while (0)

#ifdef COCO_FASTBUS_PAGES
// PIZERO-322: what is not in the page table. Out of line, so each inlined
// read or write keeps the one call it had (the core runs from RAM, and a
// check at every site cost 8 KB): on a CoCo 3 $FE00-$FEFF and the vectors
// come from memory here, the rest takes the full hook.
static void __attribute__((noinline)) HOT_FUNC(slow_read)(struct MC6809 *cpu, uint16_t a) {
	const uint8_t *p;
	if (a >= 0xfe00 && (p = coco_fastbus_hi_rd(a))) {
		cpu->D = *p;
		COCO_TICK(1, a, coco_fastbus.ticks);
	} else {
		coco_mem_cycle(cpu->mem_cycle.sptr, 1, a);
	}
}

static void __attribute__((noinline)) HOT_FUNC(slow_write)(struct MC6809 *cpu, uint16_t a, uint8_t d) {
	uint8_t *p;
	if ((a & 0xff00) == 0xfe00 && (p = coco_fastbus.fe_wr)) {
		p[a & 0xff] = d;
		COCO_TICK(0, a, coco_fastbus.ticks);
	} else {
		coco_mem_cycle(cpu->mem_cycle.sptr, 0, a);
	}
}
#endif

static CPU_PROF_INLINE uint8_t fetch_byte_notrace(struct MC6809 *cpu, uint16_t a) {
	CPU_PROF_BUS_BEGIN();
	cpu->nmi_latch |= (cpu->nmi_armed && cpu->nmi);
	cpu->firq_latch = cpu->firq;
	cpu->irq_latch = cpu->irq;
#ifdef COCO_FASTBUS_PAGES
	const uint8_t *p;
	if (a < coco_fastbus.page_top && (p = coco_fastbus.rd[a >> 13])) {
		cpu->D = p[a & 0x1fff];
		COCO_TICK(1, a, coco_fastbus.ticks);
	} else {
		slow_read(cpu, a);             // PIZERO-322: $FExx and the vectors first
	}
#else
	if (a < coco_fastbus.ram_top) {
		cpu->D = coco_fastbus.ram[a];
		COCO_TICK(1, a, coco_fastbus.ticks);
	} else if (a < coco_fastbus.rom_top) {
		cpu->D = coco_fastbus.rom[a & 0x3fff];
		COCO_TICK(1, a, coco_fastbus.rom_ticks);
	} else if (a < coco_fastbus.cart_top) {
		cpu->D = coco_fastbus.cart[a & coco_fastbus.cart_mask];
		COCO_TICK(1, a, coco_fastbus.rom_ticks);
	} else {
		coco_mem_cycle(cpu->mem_cycle.sptr, 1, a);
	}
#endif
	CPU_PROF_BUS_END(a);
	return cpu->D;
}

static uint16_t HOT_FUNC(fetch_word_notrace)(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte_notrace(cpu, a);
	return (v << 8) | fetch_byte_notrace(cpu, a+1);
}

static CPU_PROF_INLINE uint8_t fetch_byte(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte_notrace(cpu, a);
#ifdef TRACE
	cpu->trace_bytes[cpu->trace_nbytes++] = v;
	++cpu->trace_next_pc;
#endif
	return v;
}

static uint16_t HOT_FUNC(fetch_word)(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte(cpu, a);
	return (v << 8) | fetch_byte(cpu, a + 1);
}

static CPU_PROF_INLINE void store_byte(struct MC6809 *cpu, uint16_t a, uint8_t d) {
	CPU_PROF_BUS_BEGIN();
	cpu->nmi_latch |= (cpu->nmi_armed && cpu->nmi);
	cpu->firq_latch = cpu->firq;
	cpu->irq_latch = cpu->irq;
	cpu->D = d;
#ifdef COCO_FASTBUS_PAGES
	uint8_t *p;
	if (a < coco_fastbus.page_top && (p = coco_fastbus.wr[a >> 13])) {
		p[a & 0x1fff] = d;
		COCO_TICK(0, a, coco_fastbus.ticks);
	} else {
		slow_write(cpu, a, d);         // PIZERO-322: the $FExx page first
	}
#else
	if (a < coco_fastbus.ram_top) {
		coco_fastbus.ram[a] = d;
		COCO_TICK(0, a, coco_fastbus.ticks);
	} else {
		coco_mem_cycle(cpu->mem_cycle.sptr, 0, a);  // AMOLED-56
	}
#endif
	CPU_PROF_BUS_END(a);
}

#define peek_byte(c,a) ((void)fetch_byte_notrace(c,a))

// A dummy cycle samples the interrupt lines like any other.
static CPU_PROF_INLINE void nvma_cycle(struct MC6809 *cpu) {
	CPU_PROF_BUS_BEGIN();
	cpu->nmi_latch |= (cpu->nmi_armed && cpu->nmi);
	cpu->firq_latch = cpu->firq;
	cpu->irq_latch = cpu->irq;
	if (coco_fastbus_advance(coco_fastbus.rom_ticks))
		cpu->D = coco_fastbus.rom[0x3fff];   // what a read of $FFFF finds
	else
		coco_mem_nvma(cpu->mem_cycle.sptr);
	CPU_PROF_BUS_END(0xffff);
}

#define NVMA_CYCLE (nvma_cycle(cpu))

/*
 * Stack operations
 */

static void HOT_FUNC(push_s_byte)(struct MC6809 *cpu, uint8_t v) {
	store_byte(cpu, --REG_S, v);
}

static void HOT_FUNC(push_s_word)(struct MC6809 *cpu, uint16_t v) {
	store_byte(cpu, --REG_S, v);
	store_byte(cpu, --REG_S, v >> 8);
}

static uint8_t HOT_FUNC(pull_s_byte)(struct MC6809 *cpu) {
	return fetch_byte_notrace(cpu, REG_S++);
}

static uint16_t HOT_FUNC(pull_s_word)(struct MC6809 *cpu) {
	unsigned v = fetch_byte_notrace(cpu, REG_S++);
	return (v << 8) | fetch_byte_notrace(cpu, REG_S++);
}

static void HOT_FUNC(push_u_byte)(struct MC6809 *cpu, uint8_t v) {
	store_byte(cpu, --REG_U, v);
}

static void HOT_FUNC(push_u_word)(struct MC6809 *cpu, uint16_t v) {
	store_byte(cpu, --REG_U, v);
	store_byte(cpu, --REG_U, v >> 8);
}

static uint8_t HOT_FUNC(pull_u_byte)(struct MC6809 *cpu) {
	return fetch_byte_notrace(cpu, REG_U++);
}

static uint16_t HOT_FUNC(pull_u_word)(struct MC6809 *cpu) {
	unsigned v = fetch_byte_notrace(cpu, REG_U++);
	return (v << 8) | fetch_byte_notrace(cpu, REG_U++);
}

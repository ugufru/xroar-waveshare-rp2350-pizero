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

static uint8_t fetch_byte_notrace(struct MC6809 *cpu, uint16_t a) {
	cpu->nmi_latch |= (cpu->nmi_armed && cpu->nmi);
	cpu->firq_latch = cpu->firq;
	cpu->irq_latch = cpu->irq;
	coco_mem_cycle(cpu->mem_cycle.sptr, 1, a);
	return cpu->D;
}

static uint16_t fetch_word_notrace(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte_notrace(cpu, a);
	return (v << 8) | fetch_byte_notrace(cpu, a+1);
}

static uint8_t fetch_byte(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte_notrace(cpu, a);
#ifdef TRACE
	cpu->trace_bytes[cpu->trace_nbytes++] = v;
	++cpu->trace_next_pc;
#endif
	return v;
}

static uint16_t fetch_word(struct MC6809 *cpu, uint16_t a) {
	unsigned v = fetch_byte(cpu, a);
	return (v << 8) | fetch_byte(cpu, a + 1);
}

static void store_byte(struct MC6809 *cpu, uint16_t a, uint8_t d) {
	cpu->nmi_latch |= (cpu->nmi_armed && cpu->nmi);
	cpu->firq_latch = cpu->firq;
	cpu->irq_latch = cpu->irq;
	cpu->D = d;
	coco_mem_cycle(cpu->mem_cycle.sptr, 0, a);  // AMOLED-56
}

#define peek_byte(c,a) ((void)fetch_byte_notrace(c,a))
#define NVMA_CYCLE (peek_byte(cpu, 0xffff))

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

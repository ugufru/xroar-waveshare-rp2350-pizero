// pc_sample.cpp: PIZERO-331, the profiling build's PC sampler for core 0.
//
// Core 0's SysTick fires about 250 times a second (a period that is not a
// multiple of the frame or the line, so it lands all over both) and records
// the address core 0 was running. Flash addresses go into a buffer the
// once-a-second print empties ([pcs] lines); scratchpad tools map them to
// functions with nm. Together with the XIP cache's own hit counters this says
// how much of core 0's time runs from flash, which code it is, and how often
// the cache misses.
//
// Only in CPU_PROF builds: it takes SysTick from the core's getCycleCount
// (unused here; the profiler counts with DWT CYCCNT).

#ifdef CPU_PROF

#include <Arduino.h>
#include "hardware/exception.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/scb.h"
#include "hardware/regs/xip.h"
#include "hardware/regs/addressmap.h"

#define PCS_MAX 256
#define PCS_RELOAD 1000003u      // about 252 Hz at 252 MHz

static volatile uint32_t pcs_buf[PCS_MAX];
static volatile uint32_t pcs_n, pcs_flash, pcs_ram, pcs_rom, pcs_lost;

extern "C" void __not_in_flash_func(pcs_note)(const uint32_t *frame) {
    uint32_t pc = frame[6];
    if (pc >= 0x10000000u && pc < 0x11000000u) {
        pcs_flash++;
        if (pcs_n < PCS_MAX) pcs_buf[pcs_n++] = pc;
        else pcs_lost++;
    } else if (pc < 0x00008000u) {
        pcs_rom++;
    } else {
        pcs_ram++;
    }
}

extern "C" __attribute__((naked, section(".time_critical.pcs_isr"))) void pcs_isr(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b pcs_note\n");
}

void pc_sample_start(void) {
    // Straight into the RAM vector table: the SDK's setters hard_assert that
    // the slot still holds its default, and the core's getCycleCount has it.
    ((exception_handler_t *)scb_hw->vtor)[SYSTICK_EXCEPTION] = (exception_handler_t)pcs_isr;
    __asm volatile("dsb; isb" ::: "memory");
    // SysTick at the lowest priority: at the highest (to sample time inside
    // interrupts too) the board froze in its frame pacing and the freeze
    // watchdog rebooted it, about once a minute. So time in interrupts is
    // not sampled: a sample falls just after the interrupt returns.
    scb_hw->shpr[2] = (scb_hw->shpr[2] & 0x00FFFFFFu) | 0xF0000000u;
    systick_hw->csr = 0;
    systick_hw->rvr = PCS_RELOAD - 1;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7;       // processor clock, interrupt, enable
    // The XIP cache counters count from here.
    *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET) = 0;
    *(volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET) = 0;
}

// Called once a second from the profiler's print.
void pc_sample_print(void (*out)(const char *fmt, ...)) {
    volatile uint32_t *hit = (volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET);
    volatile uint32_t *acc = (volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET);
    uint32_t h = *hit, a = *acc;
    *hit = 0; *acc = 0;
    uint32_t n = pcs_n, fl = pcs_flash, ram = pcs_ram, rom = pcs_rom, lost = pcs_lost;
    uint32_t total = fl + ram + rom;
    // The core's own SysTick handler goes back in after pc_sample_start (the
    // vector table is shared and core 1's start-up installs it): take it back.
    exception_handler_t *vt = (exception_handler_t *)scb_hw->vtor;
    if (vt[SYSTICK_EXCEPTION] != (exception_handler_t)pcs_isr) {
        vt[SYSTICK_EXCEPTION] = (exception_handler_t)pcs_isr;
        __asm volatile("dsb; isb" ::: "memory");
        out("[pcs] SysTick handler taken back\r\n");
    }
    out("[pcs] samples=%lu flash=%lu ram=%lu rom=%lu lost=%lu | xip acc=%lu/s miss=%lu/s hit=%lu.%02lu%%\r\n",
        (unsigned long)total, (unsigned long)fl, (unsigned long)ram, (unsigned long)rom, (unsigned long)lost,
        (unsigned long)a, (unsigned long)(a - h),
        (unsigned long)(a ? (uint64_t)h * 100 / a : 0),
        (unsigned long)(a ? (uint64_t)h * 10000 / a % 100 : 0));
    char line[16 + 9 * 32];
    for (uint32_t i = 0; i < n; i += 32) {
        int p = snprintf(line, sizeof line, "[pcf]");
        for (uint32_t j = i; j < n && j < i + 32; j++)
            p += snprintf(line + p, sizeof line - p, " %lx", (unsigned long)pcs_buf[j]);
        out("%s\r\n", line);
    }
    pcs_n = 0; pcs_flash = pcs_ram = pcs_rom = pcs_lost = 0;
}

#endif

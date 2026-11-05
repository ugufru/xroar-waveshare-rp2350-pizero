/** \file
 *
 *  \brief Where the time per emulated 6809 cycle goes (PIZERO-209).
 *
 *  Counted with the Cortex-M33's own cycle counter (DWT CYCCNT), which
 *  resolves single host clocks; micros() cannot see a 185-clock cycle.
 *  Only in the CPU_PROF builds (envs pizero_cpuprof, pizero_cpuprof_hook);
 *  every macro is empty otherwise, so the product core is unchanged.
 *
 *  CPU_PROF=1 times the bus path (interrupt latch stores, the call and
 *  coco_mem_cycle) as one window and counts cycles, dummy cycles and
 *  instructions. CPU_PROF=2 also splits the hook into its sections.
 */

#ifndef XROAR_CPU_PROF_H_
#define XROAR_CPU_PROF_H_

#include <stdint.h>

#ifdef CPU_PROF

// The ARMv8-M architectural addresses, not the SDK's m33_hw: its headers
// bring in stdbool.h, which breaks the core's token-pasted delegate names.
#define CPU_PROF_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define CPU_PROF_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define CPU_PROF_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define CPU_PROF_DEMCR_TRCENA     (1u << 24)
#define CPU_PROF_DWT_CYCCNTENA    (1u << 0)

#ifdef __cplusplus
extern "C" {
#endif

struct cpu_prof {
	uint32_t run_clk;     // whole of the CPU run, per frame
	uint32_t bus_clk;     // latch stores + call + coco_mem_cycle
	uint32_t bus_n;       // bus cycles: one per emulated 6809 cycle
	uint32_t nvma_n;      // of which dummy cycles (address $FFFF)
	uint32_t insn_n;      // opcode fetches (a page prefix counts again)
	uint32_t sec_clk[4];  // CPU_PROF=2: decode, data, events+audio, irq+budget
	uint32_t slow_n;      // PIZERO-220: CoCo 3 cycles through the GIME (tick, cycle, dummy)
	uint32_t slow_clk;    //   and their clocks, events included
	uint32_t ev_n;        // event_run_queue calls from the per-cycle tail
	uint32_t ev_clk;      //   and their clocks, rendering included
	uint32_t line_n;      // CoCo 3 scanlines handed to the sink
	uint32_t line_clk;    //   and their clocks (GIME render to the sink)
	uint32_t cb_bus_clk;  // coco3_cpu_cycle before its tail
	uint32_t cb_tail_clk; // coco3_cpu_cycle's tail, events included
	uint32_t samp_n;      // audio samples finished
	uint32_t samp_clk;    //   and their clocks after the integrator (filter, ring)
	uint32_t catch_clk;   // audio_catch_up with something pending, samples included
	uint32_t wf_run;      // the worst frame this second: its run clocks,
	uint32_t wf_slow;     //   slow-path clocks,
	uint32_t wf_ev;       //   event clocks,
	uint32_t wf_lines;    //   lines drawn,
	uint32_t wf_sink;     //   sink clocks,
	uint32_t wf_cyc;      //   and bus cycles
	uint32_t pr_gather;   // GIME port renderer: gather and hash, per drawn line
	uint32_t pr_decode;   //   and decode
	uint32_t pr_n;        //   lines decoded
	// PIZERO-322: the CoCo 3's cycles through the GIME by address
	// (CPU_PROF_C3_* below), the fast cycles that only met the deadline,
	// and the fast-path page table's rebuilds.
	uint32_t c3_cls[11];
	uint32_t c3_page[8];  //   classes 0 and 1 by logical page
	uint32_t c3_tick_n;
	uint32_t c3_sync_n;
	// PIZERO-322: each event handler's dispatches and clocks (the first
	// CPU_PROF_EVF handlers seen; the rest add to the last slot), and the
	// audio catch-ups that had something to do.
	void *evf[8];
	uint32_t evf_n[8];
	uint32_t evf_clk[8];
	uint32_t catch_n;
	uint32_t catch_skip_n;   // CoCo 3 slow-path reads that left the catch-up
	// PIZERO-334: the CoCo 3's PIA reads alone, with the events and audio
	// catch-up that ran inside them taken out, and of them PIA0's pre-read
	// (the keyboard rows and the joystick comparator).
	uint32_t pia_rd_n, pia_rd_clk, pia_rd_pre;
	uint32_t pr_mode;     // PIZERO-322: the last decoded line's mode (see tcc1014.c)
	uint32_t pr_bytes;    //   and its VRAM bytes
	uint32_t pr_general;  //   lines through the general (per-byte mode test) loop
};
#define CPU_PROF_EVF 8

enum {
	CPU_PROF_C3_RD = 0,   // below $FE00, a page with no fast read
	CPU_PROF_C3_WR,       // below $FE00, a page with no fast write
	CPU_PROF_C3_FE,       // $FE00-$FEFF
	CPU_PROF_C3_PIA,      // $FF00-$FF3F
	CPU_PROF_C3_CART,     // $FF40-$FF8F (disk, cartridge, sound paks)
	CPU_PROF_C3_GIME,     // $FF90-$FF9F
	CPU_PROF_C3_MMU,      // $FFA0-$FFAF
	CPU_PROF_C3_PAL,      // $FFB0-$FFBF
	CPU_PROF_C3_SAM,      // $FFC0-$FFDF
	CPU_PROF_C3_VEC,      // $FFE0-$FFFE
	CPU_PROF_C3_FFFF,     // $FFFF (dummy cycles that came this way)
};

extern struct cpu_prof g_cpu_prof;

static inline uint32_t cpu_prof_now(void) {
	return CPU_PROF_DWT_CYCCNT;
}

// PIZERO-322: charge one event dispatch to its handler.
static inline void cpu_prof_ev_note(void *fn, uint32_t clk) {
	unsigned i = 0;
	while (i < CPU_PROF_EVF - 1 && g_cpu_prof.evf[i] && g_cpu_prof.evf[i] != fn) i++;
	if (!g_cpu_prof.evf[i]) g_cpu_prof.evf[i] = fn;
	g_cpu_prof.evf_n[i]++;
	g_cpu_prof.evf_clk[i] += clk;
}

#ifdef __cplusplus
}
#endif

#define CPU_PROF_BUS_BEGIN() uint32_t cpu_prof_t0 = cpu_prof_now()
#define CPU_PROF_BUS_END(a) do { \
		g_cpu_prof.bus_clk += cpu_prof_now() - cpu_prof_t0; \
		g_cpu_prof.bus_n++; \
		if ((a) == 0xffff) g_cpu_prof.nvma_n++; \
	} while (0)
#define CPU_PROF_INSN() (g_cpu_prof.insn_n++)
// The timing makes the bus helpers too big for the compiler to inline, and
// out of line they land in flash: force them back so the layout matches the
// product build, where they are inlined into the RAM-resident run loop.
#define CPU_PROF_INLINE __attribute__((always_inline)) inline

#else

#define CPU_PROF_BUS_BEGIN() do {} while (0)
#define CPU_PROF_BUS_END(a) do {} while (0)
#define CPU_PROF_INSN() do {} while (0)
// PIZERO-131: the bus helpers now hold the RAM/ROM fast path, which made
// them too big for the compiler to inline, and out of line they landed in
// flash. Forced inline in every build.
#define CPU_PROF_INLINE __attribute__((always_inline)) inline

#endif

#endif

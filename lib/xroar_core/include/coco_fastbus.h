/** \file
 *
 *  \brief The memory windows the 6809 core may access directly (PIZERO-131).
 *
 *  Most bus cycles are plain RAM or ROM. The core moves those bytes itself
 *  and then calls coco_mem_tick for the per-cycle work (time, events, audio,
 *  interrupt lines, budget); only the rest goes through coco_mem_cycle's
 *  address decode. The machine owns this struct and keeps it current when
 *  the SAM's all-RAM bit or the cartridge changes.
 *
 *  Reads below ram_top come from ram[A], then below rom_top from
 *  rom[A & $3FFF], then below cart_top from cart[A & cart_mask]; writes
 *  below ram_top go to ram[A]. Everything else uses coco_mem_cycle. A window
 *  is empty when its top is not above the one before it.
 *
 *  Step (c): most cycles need nothing from the tail either. The machine sets
 *  deadline to the earliest event tick at which something is due (the next
 *  event, the end of the frame's budget, or now when the interrupt lines need
 *  refreshing; PIZERO-220 dropped the next audio sample boundary). A cycle
 *  that ends before it only advances event_current_tick; the audio for those
 *  cycles, every sample they crossed, is caught up exactly the next time the
 *  tail runs.
 */

#ifndef XROAR_COCO_FASTBUS_H_
#define XROAR_COCO_FASTBUS_H_

/*  PIZERO-213, the CoCo 3 (COCO_FASTBUS_PAGES, implied by COCO3): the GIME's
 *  MMU maps each 8 KB logical page anywhere in physical RAM, or to ROM, so
 *  flat windows cannot describe it. The core instead looks the page up:
 *  below page_top, a read of A comes from rd[A >> 13][A & $1FFF] when that
 *  page's rd is set, and a write goes to wr[A >> 13][A & $1FFF] when its wr
 *  is set; everything else uses coco_mem_cycle. A bus cycle is ticks event
 *  ticks long (8 at 1.79 MHz). The machine rebuilds the table after every
 *  write that can change the map: the MMU registers, $FF90/$FF91, the SAM
 *  subset and the cartridge. rom is still what a dummy cycle reads:
 *  rom[$3FFF] is the byte at $FFFF.
 */
#if defined(COCO3) && !defined(COCO_FASTBUS_PAGES)
#define COCO_FASTBUS_PAGES 1
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct coco_fastbus {
	uint8_t *ram;
	const uint8_t *rom;
	const uint8_t *cart;
	uint16_t ram_top;
	uint16_t rom_top;
	uint16_t cart_top;
	uint16_t cart_mask;
	uint32_t deadline;
	// Event ticks per bus cycle. PIZERO-132: on a CoCo 2 the SAM's rate
	// bits decide it: ticks for a RAM cycle, rom_ticks for a ROM or
	// cartridge cycle and a dummy cycle (16 at normal speed, 8 fast; only
	// address-dependent speed, POKE 65495, makes them differ).
	uint16_t ticks;
	uint16_t rom_ticks;
#ifdef COCO_FASTBUS_PAGES
	const uint8_t *rd[8];
	uint8_t *wr[8];
	uint16_t page_top;
	// PIZERO-322: above page_top, the CoCo 3's $FE00-$FEFF (the constant
	// page, or the rest of page 7) and its vectors $FFE0-$FFFF (internal
	// ROM, reads only). Each points at the byte for the range's first
	// address, or is NULL for the full hook. The I/O between stays slow.
	const uint8_t *fe_rd;
	uint8_t *fe_wr;
	const uint8_t *vec_rd;
#endif
};

extern struct coco_fastbus coco_fastbus;
extern uint32_t event_current_tick;   // events.h

// One bus cycle `ticks` long that ends before the deadline: advance time and
// report 1. Otherwise leave time alone and report 0: call the tail.
static inline int coco_fastbus_advance(uint32_t ticks) {
	uint32_t t = event_current_tick + ticks;
	if ((int32_t)(t - coco_fastbus.deadline) >= 0)
		return 0;
	event_current_tick = t;
	return 1;
}

#ifdef COCO_FASTBUS_PAGES
// PIZERO-322: where a read at or above page_top may come from directly.
static inline const uint8_t *coco_fastbus_hi_rd(uint16_t a) {
	if (a < 0xff00)
		return coco_fastbus.fe_rd ? coco_fastbus.fe_rd + (a - 0xfe00) : 0;
	if (a >= 0xffe0)
		return coco_fastbus.vec_rd ? coco_fastbus.vec_rd + (a - 0xffe0) : 0;
	return 0;
}
#endif

// One bus cycle whose data the core has already moved. A and RnW are what
// was on the bus; the machine ignores them, the lockstep harness records them.
void coco_mem_tick(void *sptr, _Bool RnW, uint16_t A);

#ifdef __cplusplus
}
#endif

#endif

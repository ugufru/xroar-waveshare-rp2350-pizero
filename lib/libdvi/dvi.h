#ifndef _DVI_H
#define _DVI_H

#ifdef __cplusplus
extern "C" {
#endif

#define N_TMDS_LANES 3
#define TMDS_SYNC_LANE 0 // blue!

#include "pico/util/queue.h"

#include "dvi_config_defs.h"
#include "dvi_timing.h"
#include "dvi_serialiser.h"
#include "util_queue_u32_inline.h"

typedef void (*dvi_callback_t)(void);

struct dvi_inst {
	// Config ---
	const struct dvi_timing *timing;
	struct dvi_lane_dma_cfg dma_cfg[N_TMDS_LANES];
	struct dvi_timing_state timing_state;
	struct dvi_serialiser_cfg ser_cfg;
	// Called in the DMA IRQ once per scanline -- careful with the run time!
	dvi_callback_t scanline_callback;

	// PIZERO-30: called in the DMA IRQ on each vertical-blank (no-sync) line,
	// before that line's DMA list is loaded. Lets the HDMI audio engine repoint
	// the data-island read_addrs per line. Runs in IRQ context -- keep it CHEAP
	// (no TERC4/BCH encoding here; that disrupts scanout -- see Option A).
	dvi_callback_t vblank_callback;

	// PIZERO-30: called in the DMA IRQ on each ACTIVE scanline, BEFORE the
	// active DMA list is loaded. Lets the engine repoint the back-porch /
	// blanking blocks at a per-line audio-island buffer so audio is distributed
	// across the visible frame. Pointer stores only -- no encoding in the IRQ.
	dvi_callback_t active_line_callback;

	// State ---
	struct dvi_scanline_dma_list dma_list_vblank_sync;
	struct dvi_scanline_dma_list dma_list_vblank_nosync;
	struct dvi_scanline_dma_list dma_list_active;
	struct dvi_scanline_dma_list dma_list_error;

	// After a TMDS buffer has been enqueue via a control block for the last
	// time, two IRQs must go by before freeing. The first indicates the control
	// block for this buf has been loaded, and the second occurs some time after
	// the actual data DMA transfer has completed.
	uint32_t *tmds_buf_release_next;
	uint32_t *tmds_buf_release;
	// Remember how far behind the source is on TMDS scanlines, so we can output
	// solid colour until they catch up (rather than dying spectacularly)
	uint late_scanline_ctr;

	// Encoded scanlines:
	queue_t q_tmds_valid;
	queue_t q_tmds_free;

	// Either scanline buffers or frame buffers:
	queue_t q_colour_valid;
	queue_t q_colour_free;

};

// Set up data structures and hardware for DVI.
void dvi_init(struct dvi_inst *inst, uint spinlock_tmds_queue, uint spinlock_colour_queue);

// Call this after calling dvi_init(). DVI DMA interrupts will be routed to
// whichever core called this function. Registers an exclusive IRQ handler.
void dvi_register_irqs_this_core(struct dvi_inst *inst, uint irq_num);

// Start actually wiggling TMDS pairs. Call this once you have initialised the
// DVI, have registered the IRQs, and are producing rendered scanlines.
void dvi_start(struct dvi_inst *inst);

// TMDS encode worker function: core enters and doesn't leave, but still
// responds to IRQs. Repeatedly pop a scanline buffer from q_colour_valid,
// TMDS encode it, and pass it to the tmds valid queue.
void dvi_scanbuf_main_8bpp(struct dvi_inst *inst);
void dvi_scanbuf_main_16bpp(struct dvi_inst *inst);

// Same as above, but each q_colour_valid entry is a framebuffer
void dvi_framebuf_main_8bpp(struct dvi_inst *inst);
void dvi_framebuf_main_16bpp(struct dvi_inst *inst);

// PIZERO addition: encode a fixed RGB565 framebuffer forever, ignoring the
// colour queues entirely. Row stride is timing h_active/DVI_SYMBOLS_PER_WORD
// (320 for the 320x240->640x480 doubled mode). The producer core rewrites the
// framebuffer asynchronously; this never starves (tearing instead of dropouts).
void dvi_static_framebuf_main_16bpp(struct dvi_inst *inst, const uint16_t * volatile *framebuf_ptr);

#ifdef __cplusplus
}
#endif

#endif

/** \file
 *
 *  \brief Motorola MC6847 Video Display Generator (VDG).
 *
 *  \copyright Copyright 2003-2026 Ciaran Anscomb
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
 */

#include "top-config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "array.h"
#include "delegate.h"
#include "xalloc.h"

#include "events.h"
#include "hot.h"
#include "logging.h"
#include "machine.h"
#include "mc6847/font-6847.h"
#include "mc6847/font-6847t1.h"
#include "mc6847/mc6847.h"
#include "part.h"
#include "xroar.h"

// AMOLED-34 / AMOLED-22: 32-byte RG-mode byte→16-pixel lookup. Two flavours
// share the same table: plain fg/bg (used in alpha-as-RG text mode) and
// NTSC-artifact colours (used in true PMODE 4 / RG6 graphics). Switched by
// the externally-set g_artifact_active flag.
static uint8_t lut_rg32[256][16] __attribute__((section(".time_critical.lut_rg32")));
static uint8_t lut_rg32_last_fg = 0xFF, lut_rg32_last_bg = 0xFF;
static _Bool   lut_rg32_last_artifact = 0;
static _Bool   lut_rg32_last_css      = 0;
extern _Bool   g_artifact_active;
extern _Bool   g_artifact_css;

static void rebuild_lut_rg32(uint8_t fg, uint8_t bg) {
	if (g_artifact_active) {
		// CoCo PMODE 4 NTSC artifact pairs. Each pair of source bits
		// (taken MSB-first within the byte) maps to one of 4 colours;
		// each output pixel-pair covers 4 destination positions because
		// the artifact "pixel" is twice as wide as the raw VDG bit.
		// CSS bit swaps the two phase-dependent middle colours.
		const uint8_t col_01 = g_artifact_css ? 7 /*ORANGE*/ : 2 /*BLUE*/;
		const uint8_t col_10 = g_artifact_css ? 2 /*BLUE  */ : 7 /*ORANGE*/;
		for (int b = 0; b < 256; b++) {
			for (int pair = 0; pair < 4; pair++) {
				unsigned bits = (b >> (6 - pair * 2)) & 3;
				uint8_t c = (bits == 0) ? 8 /*BLACK*/ :
					    (bits == 3) ? 4 /*WHITE*/ :
					    (bits == 1) ? col_01 : col_10;
				lut_rg32[b][pair*4 + 0] = c;
				lut_rg32[b][pair*4 + 1] = c;
				lut_rg32[b][pair*4 + 2] = c;
				lut_rg32[b][pair*4 + 3] = c;
			}
		}
	} else {
		for (int b = 0; b < 256; b++) {
			for (int bit = 0; bit < 8; bit++) {
				uint8_t c = (b & (0x80 >> bit)) ? fg : bg;
				lut_rg32[b][bit*2 + 0] = c;
				lut_rg32[b][bit*2 + 1] = c;
			}
		}
	}
	lut_rg32_last_fg = fg;
	lut_rg32_last_bg = bg;
	lut_rg32_last_artifact = g_artifact_active;
	lut_rg32_last_css      = g_artifact_css;
}

// Convert VDG timings (measured in quarter-VDG-cycles) to event ticks:
#define EVENT_VDG_TIME(c) EVENT_TICKS_14M31818((c))

static const unsigned GM_nLPR[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };

// How video data is interpreted by the VDG.  As soon as mode changes take
// effect (which on a Dragon or CoCo typically happens partway through a byte),
// this changes immediately, so the rest of the byte is rendered differently.

enum vdg_render_mode {
	VDG_RENDER_SG,
	VDG_RENDER_CG,
	VDG_RENDER_RG,
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct MC6847_private {
	struct MC6847 public;

	/* Control lines */
	unsigned GM;
	_Bool nA_S;
	_Bool nA_G;
	_Bool EXT;
	_Bool CSS, CSSa, CSSb;
	_Bool inverted_text;

	/* Timing */
	struct event hs_fall_event;
	struct event hs_rise_event;
	event_ticks scanline_start;
	unsigned beam_pos;
	unsigned scanline;
	_Bool paused;

	// Address
	uint16_t A;

	/* Data */
	uint8_t vram_g_data;
	uint8_t vram_sg_data;

	/* Output */
	int frame;  // frameskip counter

	/* Internal state */
	_Bool is_32byte;
	_Bool GM0;
	unsigned nLPR;
	uint8_t s_fg_colour;
	uint8_t s_bg_colour;
	uint8_t fg_colour;
	uint8_t bg_colour;
	uint8_t cg_colours;
	uint8_t border_colour;
	uint8_t bright_orange;
	int vram_bit;
	unsigned render_mode;

	// Set when pixel_data[] contains only the current border colour
	_Bool have_border_only;

	/* Unsafe warning: pixel_data[] needs to be 8 elements longer than a
	 * full scanline, for the mid-scanline 32 -> 16 byte mode switch case
	 * where many extra pixels are emitted.  8 is the maximum number of
	 * elements rendered in render_scanline() between index checks. */
	uint8_t pixel_data[VDG_LINE_DURATION+8];

	unsigned burst;

	uint16_t vram[42];
	unsigned vram_index;
	unsigned vram_nbytes;

	/* Counters */
	unsigned lborder_remaining;
	unsigned vram_remaining;
	unsigned rborder_remaining;

	/* 6847T1 state */
	_Bool is_t1;
	_Bool inverse_text;
	_Bool text_border;
	uint8_t text_border_colour;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void do_hs_fall(void *);
static void do_hs_rise(void *);

static void render_scanline(struct MC6847_private *vdg);

// Canonify scanline numbers:
#define SCANLINE(s) ((s) % VDG_FRAME_DURATION)

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// MC6847 part creation

static struct part *mc6847_allocate(void);
static void mc6847_initialise(struct part *p, void *options);
static _Bool mc6847_finish(struct part *p);
static void mc6847_free(struct part *p);

static const struct partdb_entry_funcs mc6847_funcs = {
	.allocate = mc6847_allocate,
	.initialise = mc6847_initialise,
	.finish = mc6847_finish,
	.free = mc6847_free,
};

const struct partdb_entry mc6847_part = { .name = "MC6847", .description = "Motorola | MC6847 VDG", .funcs = &mc6847_funcs };
const struct partdb_entry mc6847t1_part = { .name = "MC6847T1", .description = "Motorola | MC6847T1 VDG", .funcs = &mc6847_funcs };

static struct part *mc6847_allocate(void) {
	struct MC6847_private *vdg = part_new(sizeof(*vdg));
	struct part *p = &vdg->public.part;

	*vdg = (struct MC6847_private){0};

	vdg->nLPR = 12;
	vdg->beam_pos = VDG_LEFT_BORDER_START;
	vdg->public.signal_hs = DELEGATE_DEFAULT1(void, bool);
	vdg->public.signal_fs = DELEGATE_DEFAULT1(void, bool);
	vdg->public.fetch_data = DELEGATE_DEFAULT3(void, uint16, int, uint16p);
	event_init(&vdg->hs_fall_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_hs_fall, vdg));
	event_init(&vdg->hs_rise_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_hs_rise, vdg));

	return p;
}

static void mc6847_initialise(struct part *p, void *options) {
	struct MC6847_private *vdg = (struct MC6847_private *)p;
	vdg->is_t1 = options && (strcmp((char *)options, "6847T1") == 0);
}

static _Bool mc6847_finish(struct part *p) {
	struct MC6847_private *vdg = (struct MC6847_private *)p;

	if (vdg->hs_fall_event.next == &vdg->hs_fall_event)
		event_queue(&vdg->hs_fall_event);
	if (vdg->hs_rise_event.next == &vdg->hs_rise_event)
		event_queue(&vdg->hs_rise_event);

	// AMOLED-55: is_t1 always true — fold these to T1 values.
	vdg->bright_orange = VDG_ORANGE;
	vdg->inverse_text = (vdg->GM & 2);
	vdg->text_border = !vdg->inverse_text && (vdg->GM & 4);
	vdg->text_border_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;

	return 1;
}

static void mc6847_free(struct part *p) {
	struct MC6847_private *vdg = (struct MC6847_private *)p;
	event_dequeue(&vdg->hs_fall_event);
	event_dequeue(&vdg->hs_rise_event);
}

void mc6847_pause(struct MC6847 *vdgp) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;

	event_dequeue(&vdg->hs_fall_event);
	event_dequeue(&vdg->hs_rise_event);
	vdg->paused = 1;
}

void mc6847_unpause(struct MC6847 *vdgp) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;

	vdg->scanline_start = event_current_tick;
	event_set_abs(&vdg->hs_rise_event, vdg->scanline_start + EVENT_VDG_TIME(VDG_HS_RISING_EDGE));
	event_set_abs(&vdg->hs_fall_event, vdg->scanline_start + EVENT_VDG_TIME(VDG_LINE_DURATION));
	event_queue(&vdg->hs_fall_event);
	event_queue(&vdg->hs_rise_event);
	vdg->paused = 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void HOT_FUNC(do_hs_fall)(void *data) {
	struct MC6847_private *vdg = data;
	// Finish rendering previous scanline
	if (vdg->frame == 0) {
		if (vdg->scanline < VDG_ACTIVE_AREA_START) {
			if (!vdg->have_border_only) {
				memset(vdg->pixel_data + VDG_LEFT_BORDER_START, vdg->border_colour, VDG_tAVB);
				vdg->have_border_only = 1;
			}
		} else if (vdg->scanline >= VDG_ACTIVE_AREA_START && vdg->scanline < VDG_ACTIVE_AREA_END) {
			vdg->have_border_only = 0;
			render_scanline(vdg);
			vdg->public.row++;
			if (vdg->public.row > 11)
				vdg->public.row = 0;
			if ((vdg->public.row % vdg->nLPR) == 0)
				vdg->A += vdg->is_32byte ? 32 : 16;
			vdg->beam_pos = VDG_LEFT_BORDER_START;
		} else if (vdg->scanline >= VDG_ACTIVE_AREA_END) {
			if (!vdg->have_border_only) {
				memset(vdg->pixel_data + VDG_LEFT_BORDER_START, vdg->border_colour, VDG_tAVB);
				vdg->have_border_only = 1;
			}
		}
	}
	DELEGATE_CALL(vdg->public.render_line, vdg->burst, VDG_LINE_DURATION, vdg->pixel_data);

	vdg->scanline_start = vdg->hs_fall_event.at_tick;
	// Next HS rise and fall
	event_set_abs(&vdg->hs_rise_event, vdg->scanline_start + EVENT_VDG_TIME(VDG_HS_RISING_EDGE));
	event_set_abs(&vdg->hs_fall_event, vdg->scanline_start + EVENT_VDG_TIME(VDG_LINE_DURATION));

	vdg->scanline = SCANLINE(vdg->scanline + 1);

	// HS falling edge.
	DELEGATE_CALL(vdg->public.signal_hs, 0);

	vdg->vram_nbytes = 0;
	vdg->vram_index = 0;
	vdg->vram_bit = 0;
	vdg->lborder_remaining = VDG_tLB;
	vdg->vram_remaining = vdg->is_32byte ? 32 : 16;
	vdg->rborder_remaining = VDG_tRB;
	vdg->burst = !(vdg->nA_G && vdg->CSSa && vdg->GM0);

	if (vdg->scanline == VDG_ACTIVE_AREA_START) {
		vdg->public.row = 0;
	}

	if (vdg->scanline == VDG_ACTIVE_AREA_END) {
		// FS falling edge
		DELEGATE_CALL(vdg->public.signal_fs, 0);
		vdg->A = 0;
	}

	if (vdg->scanline == VDG_VBLANK_START) {
		// FS rising edge
		DELEGATE_CALL(vdg->public.signal_fs, 1);
	}

	if (vdg->paused)
		return;

	event_queue(&vdg->hs_rise_event);
	event_queue(&vdg->hs_fall_event);
}

static void HOT_FUNC(do_hs_rise)(void *data) {
	struct MC6847_private *vdg = data;
	// HS rising edge.
	DELEGATE_CALL(vdg->public.signal_hs, 1);
}

// Renders current scanline up to the current time.

static void HOT_FUNC(render_scanline)(struct MC6847_private *vdg) {
#ifdef SUPPRESS_RENDER_SCANLINE
	// AMOLED-57 phase-1 probe: skip the pixel pipeline to measure the
	// cost we'd lift off core 0. Screen will be black/garbage while this
	// is enabled; that's expected. (The mc6847 state machine in do_hs_fall
	// still advances scanline counter, row counter, A pointer, and fires
	// signal_hs / signal_fs — which is what we want to keep.)
	(void)vdg;
	return;
#endif
	// Calculate where we are in the scanline, and queue video data up to
	// this point in time.

	unsigned beam_to = (event_current_tick - vdg->scanline_start) / EVENT_VDG_TIME(1);
	if (vdg->is_32byte && beam_to >= (VDG_tHBNK + 16)) {
		unsigned nbytes = (beam_to - VDG_tHBNK) >> 4;
		if (nbytes > 42)
			nbytes = 42;
		if (nbytes > vdg->vram_nbytes) {
			unsigned nfetch = nbytes - vdg->vram_nbytes;
			DELEGATE_CALL(vdg->public.fetch_data, vdg->A + vdg->vram_nbytes, nfetch, vdg->vram + vdg->vram_nbytes);
			vdg->vram_nbytes = nbytes;
		}
	} else if (!vdg->is_32byte && beam_to >= (VDG_tHBNK + 32)) {
		unsigned nbytes = (beam_to - VDG_tHBNK) >> 5;
		if (nbytes > 22)
			nbytes = 22;
		if (nbytes > vdg->vram_nbytes) {
			unsigned nfetch = nbytes - vdg->vram_nbytes;
			DELEGATE_CALL(vdg->public.fetch_data, vdg->A + vdg->vram_nbytes, nfetch, vdg->vram + vdg->vram_nbytes);
			vdg->vram_nbytes = nbytes;
		}
	}

	if (beam_to < VDG_LEFT_BORDER_START)
		return;
	if (vdg->beam_pos >= beam_to)
		return;
	uint8_t *pixel = vdg->pixel_data + vdg->beam_pos;

	// Render left border in full pixels.

	while (vdg->lborder_remaining > 0) {
		*(pixel++) = vdg->border_colour;
		*(pixel++) = vdg->border_colour;
		vdg->beam_pos += 2;
		if ((vdg->beam_pos & 15) == 0) {
			vdg->CSSa = vdg->CSS;
		}
		vdg->lborder_remaining -= 2;
		if (vdg->beam_pos >= beam_to)
			return;
	}

	// Active area.

	while (vdg->vram_remaining > 0) {

		if (vdg->vram_bit == 0) {
			// Byte boundary.  This is where we fetch new data,
			// including shifting in new values for CSS.  Per-byte
			// flags are processed and data is formatted for bitmap
			// graphics (vram_g_data) and semigraphics
			// (vram_sg_data).

			uint16_t vdata = vdg->vram[vdg->vram_index++];
			vdg->vram_g_data = vdata & 0xff;
			vdg->vram_bit = 8;
			// AMOLED-55: is_t1 always true in this build — non-T1 branch dropped.
			vdg->nA_S = vdata & 0x80;
			vdg->EXT = vdata & 0x400;

			vdg->CSSb = vdg->CSSa;
			vdg->CSSa = vdg->CSS;
			vdg->cg_colours = !vdg->CSSb ? VDG_GREEN : VDG_WHITE;
			vdg->text_border_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;

			if (!vdg->nA_G && !vdg->nA_S) {
				// AMOLED-55: is_t1 always true — non-T1 branch (font_6847) dropped.
				_Bool INV = vdg->EXT || (vdata & 0x40);
				INV ^= vdg->inverse_text;
				if (!vdg->EXT)
					vdg->vram_g_data |= 0x40;
				vdg->vram_g_data = font_6847t1[(vdg->vram_g_data&0x7f)*12 + vdg->public.row];
				if ((unsigned)INV ^ (unsigned)vdg->inverted_text)
					vdg->vram_g_data = ~vdg->vram_g_data;
			}

			if (!vdg->nA_G && vdg->nA_S) {
				vdg->vram_sg_data = vdg->vram_g_data;
				// AMOLED-55: is_t1 always true → (is_t1 || !EXT) always true.
				// Non-T1 SG6-mode branch dropped (the `else` block below the
				// closing brace of the dead path's set of fields).
				if (vdg->public.row < 6)
					vdg->vram_sg_data >>= 2;
				vdg->s_fg_colour = (vdg->vram_g_data >> 4) & 7;
				if (0) {  // AMOLED-55: dead branch, kept for diff clarity
					if (vdg->public.row < 4)
						vdg->vram_sg_data >>= 4;
					else if (vdg->public.row < 8)
						vdg->vram_sg_data >>= 2;
					vdg->s_fg_colour = vdg->cg_colours + ((vdg->vram_g_data >> 6) & 3);
				}
				vdg->s_bg_colour = !vdg->nA_G ? VDG_BLACK : VDG_GREEN;
				vdg->vram_sg_data = ((vdg->vram_sg_data & 2) ? 0xf0 : 0) | ((vdg->vram_sg_data & 1) ? 0x0f : 0);
			}

			if (!vdg->nA_G) {
				vdg->render_mode = !vdg->nA_S ? VDG_RENDER_RG : VDG_RENDER_SG;
				vdg->fg_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;
				vdg->bg_colour = !vdg->CSSb ? VDG_DARK_GREEN : VDG_DARK_ORANGE;
			} else {
				vdg->render_mode = vdg->GM0 ? VDG_RENDER_RG : VDG_RENDER_CG;
				vdg->fg_colour = !vdg->CSSb ? VDG_GREEN : VDG_WHITE;
				vdg->bg_colour = !vdg->CSSb ? VDG_DARK_GREEN : VDG_BLACK;
			}
		}

		// AMOLED-34 fast path: full-byte RG render in 32-byte mode
		// (PMODE 4 graphics + text-as-RG alpha). Avoids the per-2-bit
		// switch/dispatch loop entirely. Only safe when we're at the
		// start of a byte (vram_bit==8) AND there's room for all 16
		// output pixels before beam_to.
		if (vdg->vram_bit == 8 && vdg->render_mode == VDG_RENDER_RG &&
		    vdg->is_32byte && vdg->beam_pos + 16 <= beam_to) {
			if (vdg->fg_colour != lut_rg32_last_fg ||
			    vdg->bg_colour != lut_rg32_last_bg ||
			    g_artifact_active != lut_rg32_last_artifact ||
			    g_artifact_css    != lut_rg32_last_css) {
				rebuild_lut_rg32(vdg->fg_colour, vdg->bg_colour);
			}
			memcpy(pixel, lut_rg32[vdg->vram_g_data], 16);
			pixel += 16;
			vdg->beam_pos += 16;
			vdg->vram_remaining--;
			vdg->vram_bit = 0;
			vdg->vram_g_data = 0;
			vdg->vram_sg_data = 0;
			if (vdg->beam_pos >= beam_to)
				return;
			continue;
		}

		// Output is rendered for two bits of input data at a time.
		// This limits where mode changes can take effect, possibly a
		// little too much (2 bits can be 4 pixels in 16-byte modes).

		// Interpret data according to mode.  Note that a switch to
		// semigraphics mode can only occur on byte boundaries (ie,
		// processed above), which means a switch to text mode mid-byte
		// always renders the rest of the byte as bitmap graphics.

		uint8_t c0, c1;
		switch (vdg->render_mode) {
		case VDG_RENDER_SG: default:
			c0 = (vdg->vram_sg_data&0x80) ? vdg->s_fg_colour : vdg->s_bg_colour;
			c1 = (vdg->vram_sg_data&0x40) ? vdg->s_fg_colour : vdg->s_bg_colour;
			break;
		case VDG_RENDER_CG:
			c0 = c1 = vdg->cg_colours + ((vdg->vram_g_data & 0xc0) >> 6);
			break;
		case VDG_RENDER_RG:
			c0 = (vdg->vram_g_data&0x80) ? vdg->fg_colour : vdg->bg_colour;
			c1 = (vdg->vram_g_data&0x40) ? vdg->fg_colour : vdg->bg_colour;
			break;
		}

		if (vdg->is_32byte) {
			*(pixel++) = c0;
			*(pixel++) = c0;
			*(pixel++) = c1;
			*(pixel++) = c1;
			vdg->beam_pos += 4;
		} else {
			*(pixel++) = c0;
			*(pixel++) = c0;
			*(pixel++) = c0;
			*(pixel++) = c0;
			*(pixel++) = c1;
			*(pixel++) = c1;
			*(pixel++) = c1;
			*(pixel++) = c1;
			vdg->beam_pos += 8;
		}

		vdg->vram_bit -= 2;
		if (vdg->vram_bit == 0) {
			vdg->vram_remaining--;
		}
		vdg->vram_g_data <<= 2;
		vdg->vram_sg_data <<= 2;
		if (vdg->beam_pos >= beam_to)
			return;
	}

	// Render right border in full pixels (as with left border).

	while (vdg->rborder_remaining > 0) {
		if (vdg->beam_pos == VDG_RIGHT_BORDER_START) {
			vdg->CSSb = vdg->CSSa;
			vdg->text_border_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;
		}
		vdg->border_colour = vdg->nA_G ? vdg->cg_colours : (vdg->text_border ? vdg->text_border_colour : VDG_BLACK);
		*(pixel++) = vdg->border_colour;
		*(pixel++) = vdg->border_colour;
		vdg->beam_pos += 2;
		if ((vdg->beam_pos & 15) == 0) {
			vdg->CSSa = vdg->CSS;
		}
		vdg->rborder_remaining -= 2;
		if (vdg->beam_pos >= beam_to)
			return;
	}

	// If a program switches to 32 bytes per line mid-scanline, the whole
	// scanline might not have been rendered:

	while (vdg->beam_pos < VDG_RIGHT_BORDER_END) {
		*(pixel++) = VDG_BLACK;
		*(pixel++) = VDG_BLACK;
		vdg->beam_pos += 2;
	}

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void mc6847_reset(struct MC6847 *vdgp) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;
	memset(vdg->pixel_data, 0, sizeof(vdg->pixel_data));
	vdg->beam_pos = VDG_LEFT_BORDER_START;
	vdg->scanline = 0;
	vdg->public.row = 0;
	vdg->scanline_start = event_current_tick;
	event_queue_dt(&vdg->hs_fall_event, EVENT_VDG_TIME(VDG_LINE_DURATION));
	mc6847_set_mode(vdgp, 0);
	vdg->vram_index = 0;
	vdg->vram_bit = 0;
	vdg->lborder_remaining = VDG_tLB;
	vdg->vram_remaining = vdg->is_32byte ? 32 : 16;
	vdg->rborder_remaining = VDG_tRB;
}

void mc6847_set_inverted_text(struct MC6847 *vdgp, _Bool invert) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;
	vdg->inverted_text = invert;
}

// Render scanline up to current time
void mc6847_update(void *sptr) {
	struct MC6847_private *vdg = sptr;
	if (vdg->scanline >= VDG_ACTIVE_AREA_START && vdg->scanline < VDG_ACTIVE_AREA_END) {
		render_scanline(vdg);
	}
}

void mc6847_set_mode(struct MC6847 *vdgp, unsigned mode) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;

	// Render scanline so far before changing modes
	if (vdg->scanline >= VDG_ACTIVE_AREA_START && vdg->scanline < VDG_ACTIVE_AREA_END) {
		render_scanline(vdg);
	}

	// New mode information
	vdg->GM = (mode >> 4) & 7;
	vdg->GM0 = vdg->GM & 1;
	vdg->CSS = mode & 0x08;
	_Bool new_nA_G = mode & 0x80;
	vdg->nLPR = new_nA_G ? GM_nLPR[vdg->GM] : 12;

	// AMOLED-55: is_t1 always true.
	vdg->inverse_text = (vdg->GM & 2);
	vdg->text_border = !vdg->inverse_text && (vdg->GM & 4);
	vdg->text_border_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;

	// Transition between alpha/semigraphics and graphics has side-effects.
	// Border colour may change, row preset may occur, rest of byte may be
	// rendered differently.

	if (!new_nA_G) {
		// Alpha/semigraphics mode
		if (vdg->nA_G) {
			// Previously in graphics mode
			vdg->public.row = 0;  // row preset
			vdg->render_mode = VDG_RENDER_RG;
			if (vdg->nA_S) {
				vdg->vram_g_data = 0x3f;
				vdg->fg_colour = VDG_GREEN;
				vdg->bg_colour = VDG_DARK_GREEN;
			} else {
				vdg->fg_colour = !vdg->CSSb ? VDG_GREEN : vdg->bright_orange;
				vdg->bg_colour = !vdg->CSSb ? VDG_DARK_GREEN : VDG_DARK_ORANGE;
			}
		}
		vdg->border_colour = vdg->text_border ? vdg->text_border_colour : VDG_BLACK;
	} else {
		// Graphics mode
		if (!vdg->nA_G) {
			// Previously in alpha/semigraphics mode
			vdg->border_colour = vdg->cg_colours;
			vdg->fg_colour = !vdg->CSSb ? VDG_GREEN : VDG_WHITE;
			vdg->bg_colour = !vdg->CSSb ? VDG_DARK_GREEN : VDG_BLACK;
		}
		vdg->render_mode = vdg->GM0 ? VDG_RENDER_RG : VDG_RENDER_CG;
	}
	vdg->nA_G = new_nA_G;

	vdg->is_32byte = !vdg->nA_G || !(vdg->GM == 0 || (vdg->GM0 && vdg->GM != 7));
}

/** \file
 *
 *  \brief TCC1014 (GIME) support.
 *
 *  \copyright Copyright 2019-2024 Ciaran Anscomb
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

#ifndef XROAR_TCC1014_TCC1014_H
#define XROAR_TCC1014_TCC1014_H

#include <stdint.h>

#include "delegate.h"

// Virtually all timings vary by GIME model selected, however the line length
// remains constant (measured in pixels (1/14.31818µs);

#define TCC1014_tSL      (912)

// GIME palette indices.  These names reflect the usual use of the palette
// entry in VDG compatibility mode.

enum tcc1014_colour {
	TCC1014_GREEN, TCC1014_YELLOW, TCC1014_BLUE, TCC1014_RED,
	TCC1014_WHITE, TCC1014_CYAN, TCC1014_MAGENTA, TCC1014_ORANGE,
	TCC1014_RGCSS0_0, TCC1014_RGCSS0_1, TCC1014_RGCSS1_0, TCC1014_RGCSS1_1,
	TCC1014_DARK_GREEN, TCC1014_BRIGHT_GREEN, TCC1014_DARK_ORANGE, TCC1014_BRIGHT_ORANGE
};

struct TCC1014 {
	struct part part;

	unsigned S;
	uint32_t Z;
	_Bool RAS;

	_Bool FIRQ;
	_Bool IRQ;

	_Bool IL0, IL1, IL2;

	uint8_t *CPUD;

	// Delegates to notify on signal edges.
	DELEGATE_T1(void, bool) signal_hs;
	DELEGATE_T1(void, bool) signal_fs;

	DELEGATE_T3(void, int, bool, uint16) cpu_cycle;
	DELEGATE_T1(uint16, uint32) fetch_vram;

	// Report geometry
	//
	//     int x, y;  // top-left of active area
	//     int w, h;  // size of active area
	//
	// When video mode changes, GIME will report the new active area.  This
	// should allow a video module to centre it within its display area.

	DELEGATE_T4(void, int, int, int, int) set_active_area;

	// Render line
	//
	//     unsigned burst;       // burst index for this line
	//     unsigned npixels;     // no. pixels in scanline
	//     const uint8_t *data;  // palettised data, NULL for dummy line
	//
	// GIME will set 'burst' to 0 (normal burst) or 1 (inverted burst).

	DELEGATE_T3(void, unsigned, unsigned, uint8cp) render_line;

	// RP2350 port addition (PIZERO-218): when set, render_line is called
	// with TCC1014_PORT_W pixels instead of a whole scanline: the 640 GIME
	// pixels around the active area at half horizontal resolution, each a
	// 6-bit colour. The active area is drawn in one go with the state at its
	// start (or at the first register change inside it), not pixel by pixel.
	_Bool port_render;
	// Where the port_render renderer reads video RAM directly: physical
	// $00000-$0FFFF and $10000-$1FFFF of a 128 KB machine (addresses wrap at
	// 128 KB). NULL: it uses fetch_vram. It passes render_line NULL for an
	// active line whose inputs match the same scanline's last frame; setting
	// port_redraw makes every line draw again (after the port drew over them).
	const uint8_t *port_vram[2];
	_Bool port_redraw;
};

// PIZERO-134: TCC1014_PORT_FULL draws the 640 pixels at full resolution
// instead of halving them, for a 640-wide display (80 columns).
#ifdef TCC1014_PORT_FULL
#define TCC1014_PORT_W 640
#else
#define TCC1014_PORT_W 320
#endif
#define TCC1014_PORT_LINES 320

void tcc1014_reset(struct TCC1014 *gimep);
void tcc1014_mem_cycle(void *sptr, _Bool RnW, uint16_t A);

unsigned tcc1014_decode(struct TCC1014 *, uint16_t A);
unsigned tcc1014_decode_page(struct TCC1014 *, unsigned page, unsigned *bank);   // PIZERO-213
_Bool tcc1014_mc3(struct TCC1014 *);   // PIZERO-322: $FExx is the constant RAM page
unsigned tcc1014_cycle_ticks(struct TCC1014 *);                                   // PIZERO-213
void tcc1014_poll_lines(struct TCC1014 *);                                         // PIZERO-220
void tcc1014_set_sam_register(struct TCC1014 *gimep, unsigned val);

void tcc1014_set_inverted_text(struct TCC1014 *gimep, _Bool);
void tcc1014_notify_mode(struct TCC1014 *gimep);
void tcc1014_set_composite(struct TCC1014 *, _Bool);

#endif

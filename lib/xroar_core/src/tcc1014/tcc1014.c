/** \file
 *
 *  \brief TCC1014 (GIME) support.
 *
 *  \copyright Copyright 2019-2025 Ciaran Anscomb
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
 *  \par Sources
 *  Sock's GIME register reference [sockgime]
 *  https://www.6809.org.uk/sock/gime.html
 */

#include "top-config.h"

// Comment this out for debugging
#define GIME_DEBUG(...)

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "array.h"
#include "xalloc.h"

#include "delegate.h"
#include "events.h"
#include "logging.h"
#include "machine.h"
#include "part.h"
#include "tcc1014/font-gime.h"
#include "tcc1014/tcc1014.h"
#include "xroar.h"
#include "cpu_prof.h"   // PIZERO-220: empty unless CPU_PROF

#ifndef GIME_DEBUG
#define HAVE_GIME_DEBUG
#define GIME_DEBUG(...) LOG_DEBUG(__VA_ARGS__)
#define GIME_MOD_DEBUG(l,...) LOG_MOD_DEBUG(l, "tcc1014", __VA_ARGS__)
#else
#define GIME_MOD_DEBUG(l,...)
#endif

enum vdg_render_mode {
	TCC1014_RENDER_SG,
	TCC1014_RENDER_CG,
	TCC1014_RENDER_RG,
	TCC1014_RENDER_RG2,
};

// GIME variant constants
//
// The horizontal timings vary significantly between the '86 and '87 GIMEs.
//
// There is also a different minimum timer value between the two.

struct tcc1014_variant {
	// The GIME timer cannot actually count down from 1.  Times are offset
	// by 2 ('86 GIME) or by 1 ('87 GIME) [sockgime]
	int timer_offset;

	unsigned tHS;  // horizontal sync pulse
	unsigned tBP[2][2];   // back porch, low/high-res, txt/gfx
	unsigned tLB[2][2][2];   // left border, low/high-res, 32/40-byte, txt/gfx
	unsigned tAA[2];  // 32/40-byte
	// tRB = 912 - tHS - tBP - tLB - tAA - tFP
	unsigned tFP[2][2];  // front porch, low/high-res, txt/gfx

	// tHS_RB = tHS + tBP + tLB + tAA.  Offset to the right border, which
	// is when the HBORD interrupt is triggered.

	// tHS_VB is the offset to the point at which VSync falls or VBORD
	// interrupt is triggered if they occur on the current scanline.
	unsigned tHS_VB[2][2][2];   // low/high-res, 32/40-byte, txt/gfx
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct TCC1014_private {
	struct TCC1014 public;

	// GIME variant constants
	const struct tcc1014_variant *variant;

	// There are four horizontal timing points at which interesting things
	// happen:
	//
	// 1. HSync fall provides an IRQ edge to the PIAs.  It is also the
	// point at which the timer in line rate decrements.  Everything else
	// on the line is measured relative to this.
	//
	// 2. HSync rise.
	//
	// 3. Vertical border.  The point at which the VBORD interrupt would
	// fire, or FSync would rise or fall if such an event occurred on the
	// current scanline.  This may have been intended to be coincident with
	// the start of the left border, but you can see on a scope that there
	// is a slight offset, which varies by GIME model.
	//
	// 4. Horizontal border.  The point at which the HBORD interrupt will
	// fire.  Occurs at the end of the active area (or where that would be
	// for non active scanlines).

	struct event hs_fall_event;
	struct event hs_rise_event;
	struct event vb_irq_event;
	struct event hb_irq_event;

	event_ticks scanline_start;
	unsigned scanline;

	// The timer can be in one of two modes.
	//
	// If TINS=0, the timer decrements at line rate, and is handled in the
	// HS fall event handler (timer interrupts being coincident with HS
	// fall verified on scope).
	//
	// If TINS=1, it instead decrements at 1/8 the GIME clock rate, which
	// is 1/4 the pixel clock used for all our timings.  In this case, we
	// schedule an event.

	struct {
		struct event update_event;
		int counter;
	} timer;

	// Output
	int frame;  // frameskip counter

	// A real GIME emits two sets of signals: composite video and RGB.  It
	// generates very different signals for each from video data.  As we
	// don't want this code to have to constantly generate two sets of
	// output, we need to know which is desired.  Calling
	// tcc1014_set_composite() sets or clears this flag:
	_Bool want_composite;

	// $FF22: PIA1B video control lines
	// XXX there may be a need for latch propagation as with the VDG, but
	// for now assume that VDG-compatible modes are simulated in a basic
	// fashion.
	struct {
		_Bool ddr;     // snooped data direction register
		unsigned pdr;  // snooped peripheral data register
	} PIA1B_shadow;

	struct {
		_Bool GnA;
		_Bool GM1;
		_Bool GM0;
		_Bool CSS;
	} VDG;

	// $FF90: Initialisation register 0 - INIT0
	// $FF91: Initialisation register 1 - INIT1
	// $FF92: Interrupt request enabled register - IRQENR
	// $FF93: Fast interrupt request enabled register - FIRQENR
	// $FF94: Timer register MSB
	// $FF95: Timer register LSB
	// $FF98: Video mode register - VMODE
	// $FF99: Video resolution register - VRES
	// $FF99: Video resolution register - VRES
	// $FF9A: Border colour register - BRDR
	// $FF9B: Disto bank select - VBANK
	// $FF9C: Vertical scroll register - VSC
	// $FF9D: Vertical offset register MSB
	// $FF9E: Vertical offset register LSB
	// $FF9F: Horizontal offset register
	uint8_t registers[16];

	// $FF90: Initialisation register 0 - INIT0
	_Bool COCO;  // 1=Color Computer Compatible
	_Bool MMUEN;  // 1=MMU Enabled (COCO = 0)
	_Bool MC3;  // 1=RAM at $FExx is constant
	_Bool MC2;  // 1=$FF4x external; 0=internal
	_Bool MC1;  // ROM map control
	_Bool MC0;  // ROM map control

	// $FF91: Initialisation register 1 - INIT1
	_Bool TINS;  // Timer source: 1=3.58MHz, 0=15.7kHz
	unsigned TR;  // MMU task select 0=task 1, 8=task 2

	// $FF98: Video mode register - VMODE
	_Bool BP;  // 1=Graphics; 0=Text
	_Bool BPI;  // 1=Phase invert
	unsigned burstn;  // 0=Monochrome, 1=Normal, 2=180°
	_Bool MOCH;  // 1=Monochrome, 0=Colour
	_Bool H50;  // 1=50Hz video; 0=60Hz video
	unsigned LPR;  // Lines Per Row: 1, 2, 8, 9, 10, 11 or 65535 (=infinite)

	// $FF99: Video resolution register - VRES
	unsigned LPF;  // Lines Per Field: 192, 199, 65535 (=infinite), 225
	unsigned HRES;  // Bytes Per Row: 16, 20, 32, 40, 64, 80, 128, 160
	unsigned CRES;  // Bits Per Pixel: 1, 2, 4, 0

	// $FF9A: Border colour register - BRDR
	uint8_t BRDR;

	// $FF9C: Vertical scroll register - VSC
	unsigned VSC;

	// $FF9D: Vertical offset register MSB
	// $FF9E: Vertical offset register LSB
	uint32_t Y;

	// $FF9F: Horizontal offset register
	_Bool HVEN;  // 1=Horizontal virtual screen enable (256 bytes per row)
	unsigned X;  // Horizontal offset

	// $FFA0-$FFA7: MMU bank registers (task one)
	// $FFA8-$FFAF: MMU bank registers (task two)
	uint8_t mmu_bank[16];

	// $FFB0-$FFBF: Colour palette registers
	uint8_t palette_reg[16];

	// $FFC0-$FFC5: SAM clear/set VDG mode
	// $FFC6-$FFD3: SAM clear/set VDG display offset
	// $FFD8/$FFD9: Clear/set MPU rate
	// $FFDE/$FFDF: Clear/set map type
	uint16_t SAM_register;

	// $FFC0-$FFC5: SAM clear/set VDG mode
	uint8_t SAM_V;

	// $FFC6-$FFD3: SAM clear/set VDG display offset
	uint16_t SAM_F;

	// $FFD8/$FFD9: Clear/set MPU rate
	_Bool R1;

	// $FFDE/$FFDF: Clear/set map type
	_Bool TY;

	unsigned irq_state;
	unsigned firq_state;
	unsigned IL0_state;
	unsigned IL1_state;
	unsigned IL2_state;

	// Flags
	_Bool inverted_text;

	// Video address
	uint32_t B;  // Current VRAM address
	unsigned row;
	unsigned rowmask;
	_Bool row_advance;
	unsigned Xoff;

	// Video resolution
	unsigned BPR;  // bytes per row
	unsigned row_stride;  // may be different from BPR
	unsigned resolution;  // horizontal resolution

	// Horizontal timing in pixels (1/14.31818µs)
	struct {
		unsigned tHS_LB;  // start of left border (offset from HS fall)
		unsigned tHS_AA;  // start of active area (offset from HS fall)
		unsigned tHS_RB;  // start of right border (offset from HS fall)
		unsigned tHS_FP;  // start of front porch (offset from HS fall)

		unsigned npixels;  // number of pixels rendered so far
	} horizontal;

	// Vertical timing in scanlines
	struct {
		unsigned lF;   // lines per field: 314 (50Hz) or 263 (60Hz)
		unsigned lTB;  // lines of top border
		unsigned lAA;  // lines of active area

		_Bool sync;         // in sync state
		_Bool active_area;  // in active area
		unsigned lcount;    // number of scanlines rendered in current state
	} vertical;

	uint8_t border_colour;

	// Internal state
	_Bool blink;
	_Bool have_vdata_cache;
	uint8_t vdata_cache;

	// Unsafe warning: pixel_data[] *may* need to be 16 elements longer
	// than a full scanline.  16 is the maximum number of elements rendered
	// in render_scanline() between index checks.
	uint8_t pixel_data[TCC1014_tSL+16];

	// RP2350 port addition (PIZERO-218): see port_render in tcc1014.h.
	uint8_t port_line[TCC1014_PORT_W + 16];
	_Bool port_done;  // this scanline's port_line is drawn
	_Bool port_skip;  // ... and unchanged since last frame: render_line gets NULL
	uint32_t port_sum[TCC1014_PORT_LINES];  // per scanline: hash of its inputs, 0 = draw
	// PIZERO-330: the active line's gathered bytes and mode; decoded here
	// into port_line, or handed to render_line when port_defer is set.
	struct tcc1014_port_job port_job;
	_Bool port_job_ready;  // ... handed over at the line's end
};

// RP2350 port (PIZERO-133): serialisation removed, as for the other
// vendored parts; this port has no snapshots.

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// GIME interrupt flags
#define INT_TMR   (0x20)
#define INT_HBORD (0x10)
#define INT_VBORD (0x08)
#define INT_EI2   (0x04)
#define INT_EI1   (0x02)
#define INT_EI0   (0x01)

// Signal interrupt as IRQ or FIRQ according to enable bits
#define SET_INTERRUPT(g,v) do { \
		(g)->irq_state |= ((v) & (g)->registers[2]); \
		(g)->firq_state |= ((v) & (g)->registers[3]); \
		(g)->public.IRQ = ((g)->registers[0] & 0x20) ? ((g)->irq_state & 0x3f) : 0; \
		(g)->public.FIRQ = ((g)->registers[0] & 0x10) ? ((g)->firq_state & 0x3f) : 0; \
	} while (0)

// Lines of top border.  Varies by mode and 50Hz/60Hz selection.  The
// transition to "infinite" lines is handled specially.  Measured.
static const unsigned VRES_LPF_lTB[2][4] = {
	{ 36, 34, 65535, 19 },  // 60Hz
	{ 63, 59, 65535, 46 },  // 50Hz
};

// Lines of active area
static const unsigned VRES_LPF_lAA[4] = { 192, 199, 65535, 225 };

// Bytes per row
static const unsigned VRES_HRES_BPR[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
static const unsigned VRES_HRES_BPR_TEXT[8] = { 32, 40, 32, 40, 64, 80, 64, 80 };

// I imagine lines per rows counting in GIME modes to work by maintaining row
// as a 4-bit counter and having selected bits ANDed together to flag reset.
// Special cases: 0 always resets, 16 never resets.
static const unsigned LPR_rowmask[8] = { 0, 1, 2, 8, 9, 10, 11, 16 };
static const unsigned SAM_V_rowmask[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };
static const unsigned VSC_rowmask[16] = { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 4, 3, 2, 1, 12 };

// GIME variant constants
static const struct tcc1014_variant tcc1014_variant[2] = {
	// '86 GIME
	{
		.timer_offset = 2,
		.tHS =  80,
		.tBP = {
			{  61,  57 },  // back porch, low-res, txt/gfx
			{  63,  63 },  // back porch, high-res, txt/gfx
		},
		.tLB = {
			{
				{ 106, 110 },  // left border, low-res, 32-byte, txt/gfx
				{  44,  48 },  // left border, low-res, 40-byte, txt/gfx
			},
			{
				{ 104, 104 },  // left border, high-res, 32-byte, txt/gfx
				{  42,  42 },  // left border, high-res, 40-byte, txt/gfx
			},
		},
		.tAA = { 512, 640 },  // active area, 32/40-byte
		.tFP = {
			{  27,  31 },  // front porch, low-res, txt/gfx
			{  25,  25 },  // front porch, high-res, txt/gfx
		},
		.tHS_VB = {
			{
				{ 225, 221 },  // t(VB-HS), low-res, 32-byte, txt/gfx
				{ 161, 157 },  // t(VB-HS), low-res, 40-byte, txt/gfx
			},
			{
				{ 227, 227 },  // t(VB-HS), high-res, 32-byte, txt/gfx
				{ 163, 163 },  // t(VB-HS), high-res, 40-byte, txt/gfx
			},
		},
	},

	// '87 GIME
	{
		.timer_offset = 1,
		.tHS =  72,
		.tBP = {
			{  61,  57 },  // back porch, low-res, txt/gfx
			{  63,  63 },  // back porch, high-res, txt/gfx
		},
		.tLB = {
			{
				{ 106, 110 },  // left border, low-res, 32-byte, txt/gfx
				{  42,  48 },  // left border, low-res, 40-byte, txt/gfx
			},
			{
				{ 106, 106 },  // left border, high-res, 32-byte, txt/gfx
				{  42,  42 },  // left border, high-res, 40-byte, txt/gfx
			},
		},
		.tAA = { 512, 640 },  // active area, 32/40-byte
		.tFP = {
			{  35,  39 },  // front porch, low-res, txt/gfx
			{  33,  33 },  // front porch, high-res, txt/gfx
		},
		.tHS_VB = {
			{
				{ 137, 133 },  // t(VB-HS), low-res, 32-byte, txt/gfx
				{ 137, 133 },  // t(VB-HS), low-res, 40-byte, txt/gfx
			},
			{
				{ 139, 139 },  // t(VB-HS), high-res, 32-byte, txt/gfx
				{ 139, 139 },  // t(VB-HS), high-res, 40-byte, txt/gfx
			},
		},
	},
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// GIME register writes
static void tcc1014_set_register(struct TCC1014_private *gime, unsigned reg, unsigned val);

// Horizontal timing points
static void do_hs_fall(void *);
static void do_hs_rise(void *);
static void do_hb_irq(void *);
static void do_vb_irq(void *);
static void do_update_timer(void *);

// Render scanline to specified point in time
static void render_scanline(struct TCC1014_private *gime, event_ticks t);

// Timer handling
static void schedule_timer(struct TCC1014_private *, event_ticks t);
static void update_timer(struct TCC1014_private *, event_ticks t);
static void do_update_timer(void *);

// Update state from register contents
static void update_from_gime_registers(struct TCC1014_private *gime);
static void update_from_sam_register(struct TCC1014_private *gime);

#ifdef HAVE_GIME_DEBUG
static inline unsigned l_dt(struct TCC1014_private *gime) { return event_current_tick - gime->scanline_start; }
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// TCC1014/GIME part creation

static struct part *tcc1014_allocate(void);
static void tcc1014_initialise(struct part *p, void *options);
static _Bool tcc1014_finish(struct part *p);
static void tcc1014_free(struct part *p);

static _Bool tcc1014_is_a(struct part *p, const char *name);

static const struct partdb_entry_funcs tcc1014_funcs = {
        .allocate = tcc1014_allocate,
        .initialise = tcc1014_initialise,
        .finish = tcc1014_finish,
        .free = tcc1014_free,

	.is_a = tcc1014_is_a,
};

const struct partdb_entry tcc1014_1986_part = { .name = "TCC1014-1986", .description = "Tandy, VLSI | TCC1014 ACVC (GIME) (1986)", .funcs = &tcc1014_funcs };
const struct partdb_entry tcc1014_1987_part = { .name = "TCC1014-1987", .description = "Tandy, VLSI | TCC1014 ACVC (GIME) (1987)", .funcs = &tcc1014_funcs };

static struct part *tcc1014_allocate(void) {
	struct TCC1014_private *gime = part_new(sizeof(*gime));
	struct part *p = &gime->public.part;

	*gime = (struct TCC1014_private){0};

	gime->B = 0x60400;
	gime->horizontal.npixels = 0;
	gime->public.cpu_cycle = DELEGATE_DEFAULT3(void, int, bool, uint16);
	gime->public.fetch_vram = DELEGATE_DEFAULT1(uint16, uint32);
	gime->public.signal_hs = DELEGATE_DEFAULT1(void, bool);
	gime->public.signal_fs = DELEGATE_DEFAULT1(void, bool);
	event_init(&gime->hs_fall_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_hs_fall, gime));
	event_init(&gime->hs_rise_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_hs_rise, gime));
	event_init(&gime->hb_irq_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_hb_irq, gime));
	event_init(&gime->vb_irq_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_vb_irq, gime));
	event_init(&gime->timer.update_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, do_update_timer, gime));

	return p;
}

static void tcc1014_initialise(struct part *p, void *options) {
	(void)p;
	(void)options;
}

static _Bool tcc1014_finish(struct part *p) {
	struct TCC1014_private *gime = (struct TCC1014_private *)p;

	_Bool is_1987 = (strcmp(p->partdb->name, "TCC1014-1987") == 0);
	gime->variant = &tcc1014_variant[is_1987];

	if (gime->hs_fall_event.next == &gime->hs_fall_event)
		event_queue(&gime->hs_fall_event);
	if (gime->hs_rise_event.next == &gime->hs_rise_event)
		event_queue(&gime->hs_rise_event);
	if (gime->vb_irq_event.next == &gime->vb_irq_event)
		event_queue(&gime->vb_irq_event);
	if (gime->hb_irq_event.next == &gime->hb_irq_event)
		event_queue(&gime->hb_irq_event);
	if (gime->timer.update_event.next == &gime->timer.update_event)
		event_queue(&gime->timer.update_event);

	update_from_sam_register(gime);

	for (int i = 0; i < 16; i++) {
		tcc1014_set_register(gime, i, gime->registers[i]);
	}

	return 1;
}

void tcc1014_free(struct part *p) {
	struct TCC1014_private *gime = (struct TCC1014_private *)p;
	event_dequeue(&gime->timer.update_event);
	event_dequeue(&gime->hb_irq_event);
	event_dequeue(&gime->vb_irq_event);
	event_dequeue(&gime->hs_rise_event);
	event_dequeue(&gime->hs_fall_event);
}

static _Bool tcc1014_is_a(struct part *p, const char *name) {
	(void)p;
	return strcmp(name, "TCC1014") == 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void tcc1014_reset(struct TCC1014 *gimep) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;

	for (int i = 0; i < 16; i++) {
		tcc1014_set_register(gime, i, 0);
		gime->palette_reg[i] = 0;
	}
	tcc1014_set_sam_register(gimep, 0);

	// Offset our timings slightly to be out of phase with the CPU.
	event_ticks t = event_current_tick + 10;

	memset(gime->pixel_data, 0, sizeof(gime->pixel_data));
	gime->horizontal.npixels = 0;
	gime->frame = 0;
	gime->scanline = 0;
	gime->vertical.sync = 1;
	gime->vertical.lcount = 0;
	gime->row = 0;
	gime->scanline_start = t;
	gime->PIA1B_shadow.pdr = 0;
	event_queue_abs(&gime->hs_fall_event, t + TCC1014_tSL);
	update_from_gime_registers(gime);
	//gime->vram_bit = 0;
	gime->have_vdata_cache = 0;
}

// Port addition (PIZERO-220): what tcc1014_mem_cycle does for a cycle whose
// data the CPU has already moved (a RAM or ROM page of the port's fast path):
// only the edge detect on IL2..0. The caller does the rest of its call-back.
void tcc1014_poll_lines(struct TCC1014 *gimep) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	if (gimep->IL2 && !gime->IL2_state) {
		SET_INTERRUPT(gime, INT_EI2);
	}
	gime->IL2_state = gimep->IL2 ? INT_EI2 : 0;
	if (gimep->IL1 && !gime->IL1_state) {
		SET_INTERRUPT(gime, INT_EI1);
	}
	gime->IL1_state = gimep->IL1 ? INT_EI1 : 0;
	if (gimep->IL0 && !gime->IL0_state) {
		SET_INTERRUPT(gime, INT_EI0);
	}
	gime->IL0_state = gimep->IL0 ? INT_EI0 : 0;
}

void tcc1014_mem_cycle(void *sptr, _Bool RnW, uint16_t A) {
	struct TCC1014 *gimep = sptr;
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;

	// Edge detect IL2..0
	if (gimep->IL2 && !gime->IL2_state) {
		SET_INTERRUPT(gime, INT_EI2);
	}
	gime->IL2_state = gimep->IL2 ? INT_EI2 : 0;
	if (gimep->IL1 && !gime->IL1_state) {
		SET_INTERRUPT(gime, INT_EI1);
	}
	gime->IL1_state = gimep->IL1 ? INT_EI1 : 0;
	if (gimep->IL0 && !gime->IL0_state) {
		SET_INTERRUPT(gime, INT_EI0);
	}
	gime->IL0_state = gimep->IL0 ? INT_EI0 : 0;

	gimep->S = 7;
	gimep->RAS = 0;

	// Address decoding

	if (A < 0xff00) {
		_Bool use_mmu = gime->MMUEN;

		if (A >= 0xfe00) {
			if (gime->MC3) {
				gimep->RAS = 1;
				use_mmu = 0;
			}
		}

		unsigned bank = use_mmu ? gime->mmu_bank[gime->TR | (A >> 13)]
		                        : (0x38 | (A >> 13));

		if (!gime->TY && bank >= 0x3c) {
			if (!gime->MC1) {
				gimep->S = (bank >= 0x3e) ? 1 : 0;
			} else {
				gimep->S = gime->MC0 ? 1 : 0;
			}
		} else {
			gimep->RAS = 1;
		}

		gimep->Z = (bank << 13) | (A & 0x1fff);

	} else if (A < 0xff40) {
		if ((A & 0x10) == 0) {
			gimep->S = 2;
			if (A == 0xff22 && !RnW) {
				// GIME snoops writes to $FF22
				if (gime->PIA1B_shadow.ddr) {
					gime->PIA1B_shadow.pdr = *gimep->CPUD & 0xf8;
					update_from_gime_registers(gime);
				}
			} else if (A == 0xff23 && !RnW) {
				// GIME snoops the data direction register too
				gime->PIA1B_shadow.ddr = *gimep->CPUD & 0x04;
			}
		}

	} else if (A < 0xff60) {
		if (gime->MC2 || A >= 0xff50) {
			gimep->S = 6;
		}

	} else if (A < 0xff90) {
		// NOP

	} else if (A < 0xffa0) {
		if (!RnW) {
			tcc1014_set_register(gime, A & 15, *gimep->CPUD);
		} else {
			// Contrary to my earlier understanding, _none_ of the
			// other registers in this region are readable.  Just
			// the two IRQ status/acknowledge registers:
			if (A == 0xff92) {
				*gimep->CPUD = (*gimep->CPUD & ~0x3f) | gime->irq_state;
				gime->irq_state = 0;
				gime->public.IRQ = 0;
			} else if (A == 0xff93) {
				*gimep->CPUD = (*gimep->CPUD & ~0x3f) | gime->firq_state;
				gime->firq_state = 0;
				gime->public.FIRQ = 0;
			}
		}

	} else if (A < 0xffb0) {
		if (!RnW) {
			gime->mmu_bank[A & 15] = *gimep->CPUD & 0x3f;
		} else {
			*gimep->CPUD = (*gimep->CPUD & ~0x3f) | gime->mmu_bank[A & 15];
		}

	} else if (A < 0xffc0) {
		if (!RnW) {
			render_scanline(gime, event_current_tick);
			gime->palette_reg[A & 15] = *gimep->CPUD & 0x3f;
			GIME_MOD_DEBUG(3, "PALETTE: %d=%02x\n", A & 15, gime->palette_reg[A & 15]);
		} else {
			*gimep->CPUD = (*gimep->CPUD & ~0x3f) | gime->palette_reg[A & 15];
		}

	} else if (A < 0xffe0) {
		if (!RnW) {
			unsigned b = 1 << ((A >> 1) & 0x0f);
			if (A & 1) {
				gime->SAM_register |= b;
			} else {
				gime->SAM_register &= ~b;
			}
			update_from_sam_register(gime);
		}

	} else {
		gimep->S = 0;
	}

	int ncycles = gime->R1 ? 8 : 16;
	DELEGATE_CALL(gimep->cpu_cycle, ncycles, RnW, A);

}

// Just the address decode from tcc1014_mem_cycle().  Used to verify that a
// breakpoint refers to ROM.  Unlike SAM equivalent, RnW doesn't affect the
// result.

unsigned tcc1014_decode(struct TCC1014 *gimep, uint16_t A) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	if (A < 0xff00) {
		_Bool use_mmu = gime->MMUEN;

		if (A >= 0xfe00) {
			if (gime->MC3) {
				use_mmu = 0;
			}
		}

		unsigned bank = use_mmu ? gime->mmu_bank[gime->TR | (A >> 13)]
		                        : (0x38 | (A >> 13));

		if (!gime->TY && bank >= 0x3c) {
			if (!gime->MC1) {
				return (bank >= 0x3e) ? 1 : 0;
			} else {
				return gime->MC0 ? 1 : 0;
			}
		}
	} else if (A < 0xff40) {
		if ((A & 0x10) == 0) {
			return 2;
		}
	} else if (A < 0xff60) {
		if (gime->MC2 || A >= 0xff50) {
			return 6;
		}
	} else if (A >= 0xffe0) {
		return 0;
	}
	return 7;
}

// PIZERO-213 (port addition): the decode of one 8 KB logical page below
// $FE00, for the port's fast path, which maps whole pages instead of
// decoding every cycle. Returns S as tcc1014_mem_cycle sets it (0 internal
// ROM, 1 cartridge ROM, 7 RAM) and the page's bank, so RAM is physical
// (bank << 13) onwards.

unsigned tcc1014_decode_page(struct TCC1014 *gimep, unsigned page, unsigned *bank) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	page &= 7;
	unsigned b = gime->MMUEN ? gime->mmu_bank[gime->TR | page] : (0x38 | page);
	*bank = b;
	if (!gime->TY && b >= 0x3c) {
		if (!gime->MC1) {
			return (b >= 0x3e) ? 1 : 0;
		}
		return gime->MC0 ? 1 : 0;
	}
	return 7;
}

// PIZERO-322 (port addition): INIT0's MC3, so the port's fast path can
// serve $FE00-$FEFF: with it set they are RAM at bank $3F whatever the MMU
// says (tcc1014_mem_cycle sets RAS, and RAM wins over a ROM decode).
_Bool tcc1014_mc3(struct TCC1014 *gimep) {
	return ((struct TCC1014_private *)gimep)->MC3;
}

// PIZERO-213 (port addition): event ticks per CPU cycle, as tcc1014_mem_cycle
// passes to cpu_cycle: 8 at 1.79 MHz, 16 at 0.89 MHz.

unsigned tcc1014_cycle_ticks(struct TCC1014 *gimep) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	return gime->R1 ? 8 : 16;
}

void tcc1014_set_sam_register(struct TCC1014 *gimep, unsigned val) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	gime->SAM_register = val;
	update_from_sam_register(gime);
}

void tcc1014_set_inverted_text(struct TCC1014 *gimep, _Bool value) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	gime->inverted_text = value;
}

// Called from coco3.c during finish(), after all the appropriate delegates
// have been configured.

void tcc1014_notify_mode(struct TCC1014 *gimep) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;

	unsigned HR2 = gime->COCO ? 0 : (gime->HRES >> 2) & 1;  // 0=low res, 1=high res
	unsigned HR0 = gime->COCO ? 0 : (gime->HRES & 1);  // 0=512px, 1=640px mode
	unsigned BP = gime->BP;  // 0=txt, 1=gfx

	unsigned tHS = gime->variant->tHS;
	unsigned tBP = gime->variant->tBP[HR2][BP];
	unsigned tLB = gime->variant->tLB[HR2][HR0][BP];
	unsigned tAA = gime->variant->tAA[HR0];
	unsigned tFP = gime->variant->tFP[HR2][BP];
	unsigned tRB = TCC1014_tSL - tHS - tBP - tLB - tAA - tFP;

	unsigned tHS_LB = tHS + tBP;

	gime->horizontal.tHS_LB = tHS_LB;
	gime->horizontal.tHS_AA = gime->horizontal.tHS_LB + tLB;
	gime->horizontal.tHS_RB = gime->horizontal.tHS_AA + tAA;
	gime->horizontal.tHS_FP = gime->horizontal.tHS_RB + tRB;
	gime->horizontal.npixels = gime->horizontal.tHS_LB;

	DELEGATE_SAFE_CALL(gime->public.set_active_area, gime->horizontal.tHS_AA, gime->vertical.lTB + 3, tAA, gime->vertical.lAA);
}

void tcc1014_set_composite(struct TCC1014 *gimep, _Bool value) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	gime->want_composite = value;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// GIME register writes

static void tcc1014_set_register(struct TCC1014_private *gime, unsigned reg, unsigned val) {
	render_scanline(gime, event_current_tick);
	reg &= 15;
	unsigned changed = gime->registers[reg] ^ val;
	gime->registers[reg] = val;
	switch (reg) {
	case 0:
		gime->COCO = val & 0x80;
		gime->MMUEN = val & 0x40;
		gime->MC3 = val & 0x08;
		gime->MC2 = val & 0x04;
		gime->MC1 = val & 0x02;
		gime->MC0 = val & 0x01;
		gime->public.IRQ = (val & 0x20) ? gime->irq_state : 0;
		gime->public.FIRQ = (val & 0x10) ? gime->firq_state : 0;
		GIME_MOD_DEBUG(2, "INIT0 (%-3u+%-3u): COCO=%d MMUEN=%d IEN=%d FEN=%d MC3=%d MC2=%d MC1/0=%d\n", gime->scanline, l_dt(gime), (val>>7)&1, (val>>6)&1, (val>>5)&1, (val>>4)&1, (val>>3)&1,(val>>2)&1,val&3);
		update_from_gime_registers(gime);
		break;

	case 1:
		gime->TINS = val & 0x20;
		gime->TR = (val & 0x01) ? 8 : 0;
		GIME_MOD_DEBUG(2, "INIT1 (%-3u+%-3u): MTYP=%d TINS=%d TR=%d\n", gime->scanline, l_dt(gime), (val>>6)&1, (val>>5)&1, val&1);
		if (changed & 0x20) {
			schedule_timer(gime, event_current_tick);
		}
		break;

	case 2:
		GIME_MOD_DEBUG(2, "IRQ   (%-3u+%-3u): TMR=%d HBORD=%d VBORD=%d SER=%d KBD=%d CART=%d\n", gime->scanline, l_dt(gime), (val>>5)&1, (val>>4)&1, (val>>3)&1, (val>>2)&1, (val>>1)&1, val&1);
		gime->irq_state &= val;
		gime->public.IRQ &= val;
		{
			unsigned set_int = ((gime->timer.counter == 0) ? INT_TMR : 0) | gime->IL2_state | gime->IL1_state | gime->IL0_state;
			gime->irq_state |= (set_int & changed & val);
			gime->public.IRQ = (gime->registers[0] & 0x20) ? (gime->irq_state & 0x3f) : 0;
		}
		break;

	case 3:
		GIME_MOD_DEBUG(2, "FIRQ  (%-3u+%-3u): TMR=%d HBORD=%d VBORD=%d SER=%d KBD=%d CART=%d\n", gime->scanline, l_dt(gime), (val>>5)&1, (val>>4)&1, (val>>3)&1, (val>>2)&1, (val>>1)&1, val&1);
		gime->firq_state &= val;
		gime->public.FIRQ &= val;
		{
			unsigned set_int = ((gime->timer.counter == 0) ? INT_TMR : 0) | gime->IL2_state | gime->IL1_state | gime->IL0_state;
			gime->firq_state |= (set_int & changed & val);
			gime->public.FIRQ = (gime->registers[0] & 0x10) ? (gime->firq_state & 0x3f) : 0;
		}
		break;

	case 4:
		// Timer MSB
		schedule_timer(gime, event_current_tick);
		GIME_MOD_DEBUG(2, "TMRH  (%-3u+%-3u): TIMER=%d\n", gime->scanline, l_dt(gime), ((val&0xf)<<8)|gime->registers[5]);
		break;

	case 5:
		// Timer LSB
		GIME_MOD_DEBUG(2, "TMRL  (%-3u+%-3u): TIMER=%d\n", gime->scanline, l_dt(gime), ((gime->registers[4]&0xf)<<8)|val);
		break;

	case 8:
		gime->BP = val & 0x80;
		gime->BPI = val & 0x20;
		gime->MOCH = val & 0x10;
		gime->H50 = val & 0x08;
		gime->LPR = val & 7;
		GIME_MOD_DEBUG(2, "VMODE (%-3u+%-3u): BP=%d BPI=%d MOCH=%d H50=%d (l=%d) LPR=%d (mask=%x)\n", gime->scanline, l_dt(gime), (val&0x80)?1:0, (val&0x20)?1:0, (val&0x10)?1:0, (val&8)?1:0, gime->vertical.lF, val&7, LPR_rowmask[gime->LPR]);
		gime->burstn = gime->MOCH ? 0 : (gime->BPI ? 2 : 1);
		update_from_gime_registers(gime);
		break;

	case 9:
		gime->LPF = (val >> 5) & 3;
		gime->HRES = (val >> 2) & 7;
		gime->CRES = val & 3;
		GIME_MOD_DEBUG(2, "VRES  (%-3u+%-3u): LPF=%d (lTB=%d lAA=%d) HRES=%d CRES=%d\n", gime->scanline, l_dt(gime), (val>>5)&3, VRES_LPF_lTB[gime->H50][gime->LPF], VRES_LPF_lAA[gime->LPF], (val>>2)&7, val&3);
		update_from_gime_registers(gime);
		break;

	case 0xa:
		gime->BRDR = val & 0x3f;
		GIME_MOD_DEBUG(2, "BRDR  (%-3u+%-3u): BRDR=%d\n", gime->scanline, l_dt(gime), gime->BRDR);
		update_from_gime_registers(gime);
		break;

	case 0xc:
		gime->VSC = val & 15;
		GIME_MOD_DEBUG(2, "VSC   (%-3u+%-3u): VSC=%d\n", gime->scanline, l_dt(gime), val&15);
		update_from_gime_registers(gime);
		break;

	case 0xd:
		gime->Y = (val << 11) | (gime->registers[0xe] << 3);
		GIME_MOD_DEBUG(2, "VOFFh (%-3u+%-3u): VOFF=%05x\n", gime->scanline, l_dt(gime), (val<<11)|(gime->registers[0xe]<<3));
		break;

	case 0xe:
		gime->Y = (gime->registers[0xd] << 11) | (val << 3);
		GIME_MOD_DEBUG(2, "VOFFl (%-3u+%-3u): VOFF=%05x\n", gime->scanline, l_dt(gime), (gime->registers[0xd]<<11)|(val<<3));
		break;

	case 0xf:
		gime->HVEN = val & 0x80;
		gime->X = (val & 0x7f) << 1;
		GIME_MOD_DEBUG(2, "HOFF  (%-3u+%-3u): HVEN=%d X=%d\n", gime->scanline, l_dt(gime), gime->HVEN, gime->X);
		update_from_gime_registers(gime);
		break;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Horizontal timing points

static void do_hs_fall(void *sptr) {
	struct TCC1014_private *gime = sptr;

	event_ticks t = gime->hs_fall_event.at_tick;

	// Finish rendering previous scanline
	render_scanline(gime, t);

	if (gime->public.port_render) {
		// RP2350 port (PIZERO-218): a line outside the active area is all
		// border, or black in vertical sync.
		if (gime->port_job_ready) {
			// PIZERO-330: the port decodes it.
			DELEGATE_CALL(gime->public.render_line, gime->burstn, TCC1014_PORT_JOB,
			              (const uint8_t *)&gime->port_job);
		} else {
			if (!gime->port_done)
				memset(gime->port_line, gime->vertical.sync ? 0 : gime->border_colour, TCC1014_PORT_W);
			DELEGATE_CALL(gime->public.render_line, gime->burstn, TCC1014_PORT_W,
			              gime->port_done && gime->port_skip ? NULL : gime->port_line);
		}
		gime->port_done = 0;
		gime->port_skip = 0;
		gime->port_job_ready = 0;
	} else {
		DELEGATE_CALL(gime->public.render_line, gime->burstn, TCC1014_tSL, gime->pixel_data);
	}

	// HS falling edge.
	DELEGATE_CALL(gime->public.signal_hs, 0);

	// Timer, if at line rate.
	if (!gime->TINS && gime->timer.counter > 0) {
		// TINS=0: 15.7kHz
		gime->timer.counter--;
		if (gime->timer.counter <= 0) {
			update_timer(gime, t);
		}
	}

	// Next scanline

	gime->scanline++;
	gime->scanline_start = t;

	unsigned HR2 = gime->COCO ? 0 : (gime->HRES >> 2) & 1;  // 0=low res, 1=high res
	unsigned HR0 = gime->COCO ? 0 : (gime->HRES & 1);  // 0=512px, 1=640px mode
	unsigned BP = gime->BP;  // 0=txt, 1=gfx

	unsigned tHS_VB = gime->variant->tHS_VB[HR2][HR0][BP];

	//gime->vram_bit = 0;
	gime->have_vdata_cache = 0;

	event_queue_abs(&gime->hs_rise_event, t + gime->variant->tHS);
	event_queue_abs(&gime->hs_fall_event, t + TCC1014_tSL);
	event_queue_abs(&gime->vb_irq_event, t + tHS_VB);
}

static void do_hs_rise(void *sptr) {
	struct TCC1014_private *gime = sptr;

	GIME_MOD_DEBUG(3, "HS ^^ (%-3u+%-3u)\n", gime->scanline, l_dt(gime));

	// HS rising edge
	DELEGATE_CALL(gime->public.signal_hs, 1);
}

static void do_hb_irq(void *sptr) {
	struct TCC1014_private *gime = sptr;

#ifdef HAVE_GIME_DEBUG
	unsigned old_row = gime->row;
	unsigned old_B = gime->B;
	GIME_MOD_DEBUG(3, "HBORD (%-3u+%-3u) ", gime->scanline, l_dt(gime));
#endif

	render_scanline(gime, gime->hb_irq_event.at_tick);

	gime->row_advance = 0;
	if (gime->vertical.active_area) {
		gime->row = (gime->row + 1) & 15;
		if ((gime->row & gime->rowmask) == gime->rowmask) {
			gime->row_advance = 1;
		}
	}

	GIME_DEBUG(3, "%05x->%05x  %2d->%2d\n", old_B, gime->B, old_row, gime->row);

	// Horizontal border interrupt
	SET_INTERRUPT(gime, INT_HBORD);

}

// This timing point occurs near the beginning of the left border.  It's the
// point at which vertical signals occur (FS fall/rise, vertical border
// interrupt).

static void do_vb_irq(void *sptr) {
	struct TCC1014_private *gime = sptr;

#ifdef HAVE_GIME_DEBUG
	unsigned old_row = gime->row;
	unsigned old_B = gime->B;
	GIME_MOD_DEBUG(3, "VBORD (%-3u+%-3u) ", gime->scanline, l_dt(gime));
#endif

	if (gime->vertical.active_area) {
		if ((gime->row & gime->rowmask) == gime->rowmask) {
			gime->row = 0;
			gime->row_advance = 1;
		}

		// XXX this is a bodge to make BOINK work.  it _could_ be how
		// real hardware behaves (LPR=7 being a special case), but i've
		// not explored that yet.
		if (!gime->COCO && gime->LPR == 7) {
			gime->row_advance = 0;
		}

		if (gime->row_advance) {
			gime->B += gime->row_stride;
		}
	}

	gime->Xoff = gime->X;
	if (gime->COCO || gime->BP) {
		gime->row_stride = gime->HVEN ? 256 : gime->BPR;
	} else {
		gime->row_stride = gime->HVEN ? 256 : (gime->BPR << (gime->CRES & 1));
	}

	// XXX Only changing border colour here makes certain demos more
	// stable, but is this really the only place it is recognised?

	if (gime->COCO) {
		if (!gime->VDG.GnA) {
			_Bool GM2 = gime->PIA1B_shadow.pdr & 0x40;
			_Bool text_border = !gime->VDG.GM1 && GM2;
			unsigned text_border_colour = gime->VDG.CSS ? 0x26 : 0x12;
			gime->border_colour = text_border ? text_border_colour : 0;
		} else {
			unsigned c = gime->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
			gime->border_colour = gime->palette_reg[c];
		}
	} else {
		gime->border_colour = gime->BRDR;
	}

	unsigned HR2 = gime->COCO ? 0 : (gime->HRES >> 2) & 1;  // 0=low res, 1=high res
	unsigned HR0 = gime->COCO ? 0 : (gime->HRES & 1);  // 0=512px, 1=640px mode
	unsigned BP = gime->BP;  // 0=txt, 1=gfx

	unsigned tHS = gime->variant->tHS;
	unsigned tBP = gime->variant->tBP[HR2][BP];
	unsigned tLB = gime->variant->tLB[HR2][HR0][BP];
	unsigned tAA = gime->variant->tAA[HR0];
	unsigned tFP = gime->variant->tFP[HR2][BP];
	unsigned tRB = TCC1014_tSL - tHS - tBP - tLB - tAA - tFP;

	unsigned tHS_LB = tHS + tBP;
	unsigned tHS_RB = tHS + tBP + tLB + tAA;

	gime->horizontal.tHS_LB = tHS_LB;
	gime->horizontal.tHS_AA = gime->horizontal.tHS_LB + tLB;
	gime->horizontal.tHS_RB = gime->horizontal.tHS_AA + tAA;
	gime->horizontal.tHS_FP = gime->horizontal.tHS_RB + tRB;
	gime->horizontal.npixels = gime->horizontal.tHS_LB;

	event_queue_abs(&gime->hb_irq_event, gime->scanline_start + tHS_RB);

	gime->vertical.lcount++;

	if (gime->scanline >= gime->vertical.lF) {
		// FS falling edge
		DELEGATE_CALL(gime->public.signal_fs, 0);
		memset(gime->pixel_data, 0, sizeof(gime->pixel_data));
		// lAA must be latched near the beginning of the frame
		gime->vertical.lAA = gime->COCO ? 192 : VRES_LPF_lAA[gime->LPF];
		gime->vertical.lF = gime->H50 ? 314 : 263;
		if (gime->COCO) {
			gime->vertical.lTB = gime->H50 ? 63 : 36;
		} else {
			gime->vertical.lTB = VRES_LPF_lTB[gime->H50][gime->LPF];
		}
		gime->vertical.sync = 1;
		gime->vertical.lcount = 0;
		gime->scanline = 0;

	} else if (gime->vertical.sync) {
		// Sync (4 lines) and blanking (3 lines)
		if (gime->vertical.lcount == 4) {
			// FS rising edge
			DELEGATE_CALL(gime->public.signal_fs, 1);
		} else if (gime->vertical.lcount >= 7) {
			// Done with sync
			if (gime->LPF == 7) {
				DELEGATE_SAFE_CALL(gime->public.set_active_area, gime->horizontal.tHS_AA, 39, tAA, 192);
			} else {
				DELEGATE_SAFE_CALL(gime->public.set_active_area, gime->horizontal.tHS_AA, gime->vertical.lTB + 3, tAA, gime->vertical.lAA);
			}
			if (gime->COCO) {
				gime->B = (gime->Y & 0x701ff) | (gime->SAM_F << 9);
			} else {
				gime->B = gime->Y;
			}
			memset(gime->pixel_data + gime->horizontal.tHS_LB, gime->border_colour, 888 - gime->horizontal.tHS_LB);
			gime->vertical.sync = 0;
			gime->vertical.lcount = 0;
		}

	} else if (!gime->vertical.active_area) {
		// Border
		if (gime->vertical.lcount >= gime->vertical.lTB) {
			if (!gime->COCO) {
				gime->row = gime->VSC;
				if ((gime->row & gime->rowmask) == gime->rowmask) {
					gime->row = 0;
				}
			} else {
				gime->row = 0;
			}
			gime->vertical.active_area = 1;
			gime->vertical.lcount = 0;
		} else {
			memset(gime->pixel_data + gime->horizontal.tHS_LB, gime->border_colour, 888 - gime->horizontal.tHS_LB);
		}

	} else {
		// Active area
		if (gime->vertical.lcount >= gime->vertical.lAA) {
			// Vertical border interrupt
			SET_INTERRUPT(gime, INT_VBORD);
			memset(gime->pixel_data + gime->horizontal.tHS_LB, gime->border_colour, 888 - gime->horizontal.tHS_LB);
			gime->vertical.lTB = 65535;  // continue to end of frame
			gime->vertical.active_area = 0;
			gime->vertical.lcount = 0;
		}
	}

	GIME_DEBUG(3, "%05x->%05x  %2d->%2d\n", old_B, gime->B, old_row, gime->row);
}

static uint8_t fetch_byte_vram(struct TCC1014_private *gime) {
	// Fetch 16 bits at once.  16-colour 16 byte-per-row graphics modes
	// "lose" the lower 8 bits (done here by clearing vdata_cache).
	uint8_t r;
	if (gime->have_vdata_cache) {
		r = gime->vdata_cache;
		gime->have_vdata_cache = 0;
	} else {
		// X offset appears to be dynamically added to current video address
		uint16_t data = DELEGATE_CALL(gime->public.fetch_vram, gime->B + (gime->Xoff & 0xff));
		gime->Xoff += 2;
		r = data >> 8;
		gime->vdata_cache = data;
		gime->have_vdata_cache = 1;
	}
	return r;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Render scanline to specified point in time.
//
// Called at the end of a scanline, or before a change in state that would
// affect how things are rendered on the current scanline.
//
// Only renders active area scanlines.  Top & bottom border are treated
// specially, and just memset() the border colour.

static void port_render_scanline(struct TCC1014_private *gime, event_ticks t);

static void render_scanline(struct TCC1014_private *gime, event_ticks t) {
	if (gime->public.port_render) {
		port_render_scanline(gime, t);
		return;
	}
	unsigned beam_to = t - gime->scanline_start;

	// Don't bother if not in active area or frame skipping
	if (!gime->vertical.active_area || gime->frame != 0)
		return;

	// Don't start rendering until left border
	if (beam_to < gime->horizontal.tHS_LB)
		return;

	if (gime->horizontal.npixels >= beam_to)
		return;

	uint8_t *pixel = gime->pixel_data + gime->horizontal.npixels;

	// Left border
	while (gime->horizontal.npixels < gime->horizontal.tHS_AA) {
		*(pixel++) = gime->border_colour;
		*(pixel++) = gime->border_colour;
		gime->horizontal.npixels += 2;
		if (gime->horizontal.npixels >= beam_to)
			return;
	}

	// Active area
	while (gime->horizontal.npixels < gime->horizontal.tHS_RB) {
		enum vdg_render_mode render_mode;
		uint_fast8_t gdata;
		uint_fast8_t fg_colour = 0;
		uint_fast8_t bg_colour = 0;
		uint_fast8_t cg_colours = 0;

		// VRAM fetch and interpretation based on mode
		if (gime->COCO) {
			// VDG compatible mode
			uint_fast8_t vdata = fetch_byte_vram(gime);
			unsigned font_row = gime->row & 0x0f;
			_Bool SnA = vdata & 0x80;
			if (gime->VDG.GnA) {
				// Graphics mode
				gdata = vdata;
				fg_colour = gime->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
				bg_colour = gime->VDG.CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0;
				cg_colours = !gime->VDG.CSS ? TCC1014_GREEN : TCC1014_WHITE;
				render_mode = gime->VDG.GM0 ? TCC1014_RENDER_RG : TCC1014_RENDER_CG;
				if (gime->VDG.GM0) {
					if (gime->resolution || (gime->PIA1B_shadow.pdr & 0x70) == 0x70) {
						render_mode = TCC1014_RENDER_RG;
					} else {
						render_mode = TCC1014_RENDER_RG2;
					}
				} else {
					render_mode = TCC1014_RENDER_CG;
				}
			} else {
				if (SnA) {
					// Semigraphics
					if (font_row < 6) {
						gdata = vdata >> 2;
					} else {
						gdata = vdata;
					}
					fg_colour = (vdata >> 4) & 7;
					bg_colour = TCC1014_RGCSS0_0;
					render_mode = TCC1014_RENDER_SG;
				} else {
					// Alphanumeric
					_Bool INV = vdata & 0x40;
					INV ^= gime->VDG.GM1;  // 6847T1-compatible invert flag
					uint_fast8_t c = vdata & 0x7f;
					if (c < 0x20) {
						c |= (gime->VDG.GM0 ? 0x60 : 0x40);
						INV ^= gime->VDG.GM0;
					} else if (c >= 0x60) {
						c ^= 0x40;
					}
					gdata = font_gime[c*12+font_row];

					// Handle UI-specified inverse text mode:
					if (INV ^ gime->inverted_text)
						gdata = ~gdata;
					fg_colour = gime->VDG.CSS ? TCC1014_BRIGHT_ORANGE : TCC1014_BRIGHT_GREEN;
					bg_colour = gime->VDG.CSS ? TCC1014_DARK_ORANGE : TCC1014_DARK_GREEN;
					render_mode = TCC1014_RENDER_RG;
				}
			}

		} else {
			// CoCo 3 mode
			uint_fast8_t vdata = fetch_byte_vram(gime);
			unsigned font_row = (gime->row + 1) & 0x0f;
			if (font_row > 11) {
				font_row = 0;
			}
			if (gime->BP) {
				// CoCo 3 graphics
				gdata = vdata;
				// 16 colour, 16 byte-per-row modes zero the second
				// half of the data
				if (gime->HRES == 0 && gime->CRES >= 2) {
					gime->vdata_cache = 0;
				}
			} else {
				// CoCo 3 text
				int c = vdata & 0x7f;
				gdata = font_gime[c*12+font_row];
				if (gime->CRES & 1) {
					uint_fast8_t attr = fetch_byte_vram(gime);
					fg_colour = 8 | ((attr >> 3) & 7);
					bg_colour = attr & 7;
					if ((attr & 0x80) && gime->blink)
						fg_colour = bg_colour;
					if ((attr & 0x40) && ((font_row & gime->rowmask) == gime->rowmask))
						gdata = 0xff;
				} else {
					fg_colour = 1;
					bg_colour = 0;
				}
			}
			render_mode = TCC1014_RENDER_RG;
		}

		// Consider 4 bits at a time, twice.
		for (int i = 2; i; --i) {
			uint_fast8_t c0, c1, c2, c3;

			if (gime->COCO) {
				// VDG compatible mode
				switch (render_mode) {
				case TCC1014_RENDER_SG: default:
					c0 = c1 = c2 = c3 = gime->palette_reg[(gdata&0x02) ? fg_colour : bg_colour];
					gdata <<= 1;
					break;
				case TCC1014_RENDER_CG:
					c0 = c1 = gime->palette_reg[cg_colours + ((gdata >> 6) & 3)];
					c2 = c3 = gime->palette_reg[cg_colours + ((gdata >> 4) & 3)];
					gdata <<= 4;
					break;
				case TCC1014_RENDER_RG:
					c0 = gime->palette_reg[(gdata&0x80) ? fg_colour : bg_colour];
					c1 = gime->palette_reg[(gdata&0x40) ? fg_colour : bg_colour];
					c2 = gime->palette_reg[(gdata&0x20) ? fg_colour : bg_colour];
					c3 = gime->palette_reg[(gdata&0x10) ? fg_colour : bg_colour];
					gdata <<= 4;
					break;
				case TCC1014_RENDER_RG2:
					c0 = c1 = gime->palette_reg[(gdata&0x40) ? fg_colour : bg_colour];
					c2 = c3 = gime->palette_reg[(gdata&0x10) ? fg_colour : bg_colour];
					gdata <<= 4;
					break;
				}

			} else {
				// CoCo 3 modes

				// With the "monochrome" bit set, the grey at
				// that intensity is emitted - but only for
				// composite, so we need to know if that's what
				// the user is viewing.
				uint_fast8_t cmask = (gime->MOCH && gime->want_composite) ? 0x30 : 0x3f;

				if (gime->BP) {
					switch (gime->CRES) {
					case 0: default:
						c0 = gime->palette_reg[(gdata>>7)&1] & cmask;
						c1 = gime->palette_reg[(gdata>>6)&1] & cmask;
						c2 = gime->palette_reg[(gdata>>5)&1] & cmask;
						c3 = gime->palette_reg[(gdata>>4)&1] & cmask;
						break;

					case 1:
						c0 = c1 = gime->palette_reg[(gdata>>6)&3] & cmask;
						c2 = c3 = gime->palette_reg[(gdata>>4)&3] & cmask;
						break;

					case 2: case 3:
						c0 = c1 = c2 = c3 = gime->palette_reg[(gdata>>4)&15] & cmask;
						break;
					}

				} else {
					c0 = gime->palette_reg[(gdata&0x80)?fg_colour:bg_colour] & cmask;
					c1 = gime->palette_reg[(gdata&0x40)?fg_colour:bg_colour] & cmask;
					c2 = gime->palette_reg[(gdata&0x20)?fg_colour:bg_colour] & cmask;
					c3 = gime->palette_reg[(gdata&0x10)?fg_colour:bg_colour] & cmask;
				}
				gdata <<= 4;
			}

			// Render appropriate number of pixels
			switch (gime->resolution) {
			case 0:
				*(pixel) = c0;
				*(pixel+1) = c0;
				*(pixel+2) = c0;
				*(pixel+3) = c0;
				*(pixel+4) = c1;
				*(pixel+5) = c1;
				*(pixel+6) = c1;
				*(pixel+7) = c1;
				*(pixel+8) = c2;
				*(pixel+9) = c2;
				*(pixel+10) = c2;
				*(pixel+11) = c2;
				*(pixel+12) = c3;
				*(pixel+13) = c3;
				*(pixel+14) = c3;
				*(pixel+15) = c3;
				pixel += 16;
				gime->horizontal.npixels += 16;
				break;

			case 1:
				*(pixel) = c0;
				*(pixel+1) = c0;
				*(pixel+2) = c1;
				*(pixel+3) = c1;
				*(pixel+4) = c2;
				*(pixel+5) = c2;
				*(pixel+6) = c3;
				*(pixel+7) = c3;
				pixel += 8;
				gime->horizontal.npixels += 8;
				break;

			case 2:
				*(pixel) = c0;
				*(pixel+1) = c1;
				*(pixel+2) = c2;
				*(pixel+3) = c3;
				pixel += 4;
				gime->horizontal.npixels += 4;
				break;

			case 3:
				*(pixel) = c0;
				*(pixel+1) = c2;
				pixel += 2;
				gime->horizontal.npixels += 2;
				break;
			}
		}

		if (gime->horizontal.npixels >= beam_to)
			return;
	}

	// Right border
	while (gime->horizontal.npixels < gime->horizontal.tHS_FP) {
		*(pixel++) = gime->border_colour;
		*(pixel++) = gime->border_colour;
		gime->horizontal.npixels += 2;
		if (gime->horizontal.npixels >= beam_to)
			return;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// RP2350 port addition (PIZERO-218): the active area of a scanline straight
// into the TCC1014_PORT_W pixels the port shows, the 640 GIME pixels centred
// on it at half resolution. The VRAM fetches and the colour decode are
// render_scanline's; only the output differs: each 4-bit group writes 8, 4,
// 2 or 1 pixels by resolution (render_scanline's 16, 8, 4 or 2, halved),
// keeping the first of each pair as the port's sink always did. The whole
// active area is drawn the first time the beam is found past its start, so
// a change of state inside it shows from the next line, not mid-line.
//
// For speed it first gathers the line's VRAM bytes (straight from port_vram
// when the port gives it, else through fetch_vram), then hashes them with
// every piece of state the decode reads. A line whose hash matches the same
// scanline's last frame is not decoded at all, and render_line gets NULL:
// the port keeps what it drew. All mode state is copied to locals, as the
// byte stores below would otherwise make the compiler reload it each pixel.

#define PORT_MAX_BYTES 192

// Byte i of port_mask[g] is 0xFF when bit 7-i of g is set.
#define PORT_B(g, b) ((((g) >> (7 - (b))) & 1) ? (0xffull << (8 * (b))) : 0)
#define PORT_M1(g) (PORT_B(g,0) | PORT_B(g,1) | PORT_B(g,2) | PORT_B(g,3) | \
                    PORT_B(g,4) | PORT_B(g,5) | PORT_B(g,6) | PORT_B(g,7))
#define PORT_M4(g) PORT_M1(g), PORT_M1((g)+1), PORT_M1((g)+2), PORT_M1((g)+3)
#define PORT_M16(g) PORT_M4(g), PORT_M4((g)+4), PORT_M4((g)+8), PORT_M4((g)+12)
#define PORT_M64(g) PORT_M16(g), PORT_M16((g)+16), PORT_M16((g)+32), PORT_M16((g)+48)
static const uint64_t port_mask[256] = {
	PORT_M64(0), PORT_M64(64), PORT_M64(128), PORT_M64(192)
};

// Eight pixels: fg where the mask is set, bg elsewhere. In 32-bit halves:
// a 64-bit multiply is a library call on the M33.
static inline uint64_t port_sel(uint64_t m, uint8_t fg, uint8_t bg) {
	uint32_t f = fg * 0x01010101u, b = bg * 0x01010101u, d = f ^ b;
	uint32_t lo = b ^ (d & (uint32_t)m), hi = b ^ (d & (uint32_t)(m >> 32));
	return lo | (uint64_t)hi << 32;
}

// Eight pixels from four colours, each shown twice.
static inline uint64_t port_pairs(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
	return (uint64_t)(a * 0x0101u | b * 0x01010000u) | (uint64_t)(c * 0x0101u | d * 0x01010000u) << 32;
}

// Four pixels to eight, each shown twice.
static inline uint64_t port_double(uint32_t x) {
	uint32_t lo = (x & 0xff) * 0x0101u | ((x >> 8) & 0xff) * 0x01010000u;
	uint32_t hi = ((x >> 16) & 0xff) * 0x0101u | (x >> 24) * 0x01010000u;
	return lo | (uint64_t)hi << 32;
}

// Eight pixels from two colours, each shown four times.
static inline uint64_t port_quads(uint8_t a, uint8_t b) {
	return (uint64_t)(a * 0x01010101u) | (uint64_t)(b * 0x01010101u) << 32;
}

// A byte's eight pixels out at a resolution (0 shows each twice, 3 keeps
// every fourth), as the decode loop's switch does. PIZERO-220: always called
// with a constant resolution, so each loop below gets its own.
static inline __attribute__((always_inline)) uint8_t *port_emit(uint8_t *pixel, uint64_t cols,
                                                                 const unsigned res) {
#ifdef TCC1014_PORT_FULL
	// PIZERO-134: every pixel render_scanline would (0 shows each four
	// times, 3 keeps every other).
	switch (res) {
	case 0: {
		uint64_t a = port_quads((uint8_t)cols, (uint8_t)(cols >> 8));
		uint64_t b = port_quads((uint8_t)(cols >> 16), (uint8_t)(cols >> 24));
		uint64_t c = port_quads((uint8_t)(cols >> 32), (uint8_t)(cols >> 40));
		uint64_t d = port_quads((uint8_t)(cols >> 48), (uint8_t)(cols >> 56));
		memcpy(pixel, &a, 8);
		memcpy(pixel + 8, &b, 8);
		memcpy(pixel + 16, &c, 8);
		memcpy(pixel + 24, &d, 8);
		return pixel + 32;
	}
	case 1: {
		uint64_t a = port_double((uint32_t)cols), b = port_double((uint32_t)(cols >> 32));
		memcpy(pixel, &a, 8);
		memcpy(pixel + 8, &b, 8);
		return pixel + 16;
	}
	case 2:
		memcpy(pixel, &cols, 8);
		return pixel + 8;
	default:
		pixel[0] = cols; pixel[1] = cols >> 16;
		pixel[2] = cols >> 32; pixel[3] = cols >> 48;
		return pixel + 4;
	}
#endif
	switch (res) {
	case 0: {
		uint64_t a = port_double((uint32_t)cols), b = port_double((uint32_t)(cols >> 32));
		memcpy(pixel, &a, 8);
		memcpy(pixel + 8, &b, 8);
		return pixel + 16;
	}
	case 1:
		memcpy(pixel, &cols, 8);
		return pixel + 8;
	case 2:
		pixel[0] = cols; pixel[1] = cols >> 16;
		pixel[2] = cols >> 32; pixel[3] = cols >> 48;
		return pixel + 4;
	default:
		pixel[0] = cols; pixel[1] = cols >> 32;
		return pixel + 2;
	}
}

// PIZERO-220: the decode loop for one mode, once per resolution, so neither
// is tested per byte. COLS is an expression of vdata.
#define PORT_LOOP(COLS) do { \
		switch (resolution) { \
		case 0: for (unsigned n = nbytes; n; --n) { uint_fast8_t vdata = *src++; pixel = port_emit(pixel, (COLS), 0); } break; \
		case 1: for (unsigned n = nbytes; n; --n) { uint_fast8_t vdata = *src++; pixel = port_emit(pixel, (COLS), 1); } break; \
		case 2: for (unsigned n = nbytes; n; --n) { uint_fast8_t vdata = *src++; pixel = port_emit(pixel, (COLS), 2); } break; \
		case 3: for (unsigned n = nbytes; n; --n) { uint_fast8_t vdata = *src++; pixel = port_emit(pixel, (COLS), 3); } break; \
		} \
	} while (0)

static inline uint32_t port_mix(uint32_t h, uint32_t w) {
	h ^= w;
	return ((h << 5) | (h >> 27)) * 0x9E3779B1u;
}

static void port_render_scanline(struct TCC1014_private *gime, event_ticks t) {
	if (gime->port_done || !gime->vertical.active_area)
		return;
	unsigned beam_to = t - gime->scanline_start;
	if (beam_to <= gime->horizontal.tHS_AA)
		return;
	gime->port_done = 1;
	gime->port_skip = 0;
#ifdef CPU_PROF
	uint32_t pr_t0 = cpu_prof_now();
#endif

	unsigned tAA = gime->horizontal.tHS_RB - gime->horizontal.tHS_AA;
	if (tAA > 640)
		tAA = 640;

	// The mode, as the decode will read it.
	const _Bool COCO = gime->COCO;
	const _Bool GnA = gime->VDG.GnA, GM1 = gime->VDG.GM1, GM0 = gime->VDG.GM0, CSS = gime->VDG.CSS;
	const _Bool BP = gime->BP;
	const unsigned CRES = gime->CRES;
	const unsigned resolution = gime->resolution;
	const _Bool rg_full = resolution || (gime->PIA1B_shadow.pdr & 0x70) == 0x70;
	const uint_fast8_t cmask = (gime->MOCH && gime->want_composite) ? 0x30 : 0x3f;
	const _Bool INVT = gime->inverted_text;
	const _Bool blink = gime->blink;
	const unsigned rowmask = gime->rowmask;
	const uint8_t border = gime->border_colour;
	unsigned font_row;
	if (COCO) {
		font_row = gime->row & 0x0f;
	} else {
		font_row = (gime->row + 1) & 0x0f;
		if (font_row > 11)
			font_row = 0;
	}
	const _Bool attr = !COCO && !BP && (CRES & 1);
	const _Bool lose_low = !COCO && BP && gime->HRES == 0 && CRES >= 2;
	uint8_t pal[16];
	memcpy(pal, gime->palette_reg, 16);

	// Gather: render_scanline's fetches, in its order, with its cache.
	unsigned out_per_byte = 16u >> resolution;  // two 4-bit groups
	unsigned nbytes = (tAA / 2) / out_per_byte;
	unsigned nfetch = attr ? 2 * nbytes : nbytes;
	if (nfetch > PORT_MAX_BYTES)
		nfetch = PORT_MAX_BYTES, nbytes = attr ? nfetch / 2 : nfetch;
	uint8_t *buf = gime->port_job.bytes;   // PORT_MAX_BYTES + 4
	{
		_Bool have = gime->have_vdata_cache;
		uint8_t cache = gime->vdata_cache;
		uint32_t B = gime->B;
		unsigned Xoff = gime->Xoff;
		const uint8_t *lo = gime->public.port_vram[0], *hi = gime->public.port_vram[1];
		unsigned k = 0;
		// The usual case in one copy: the line starts on a fresh 16-bit
		// word, nothing is lost, and its bytes neither wrap the horizontal
		// offset nor cross a 64 KB half. Words are even and so is nfetch,
		// so it ends as the loop would, with the last byte in the cache.
		uint32_t A0 = (B + (Xoff & 0xff)) & 0x1ffff;
		if (lo && !have && !lose_low && !(A0 & 1) && !(nfetch & 1)
		    && (Xoff & 0xff) + nfetch <= 256 && (A0 & 0xffff) + nfetch <= 0x10000) {
			memcpy(buf, ((A0 & 0x10000) ? hi : lo) + (A0 & 0xffff), nfetch);
			Xoff += nfetch;
			cache = buf[nfetch - 1];
			k = nfetch;
		}
		while (k < nfetch) {
			for (int j = attr ? 2 : 1; j; --j) {
				uint8_t r;
				if (have) {
					r = cache;
					have = 0;
				} else {
					uint32_t A = B + (Xoff & 0xff);
					uint16_t data;
					if (lo) {
						uint32_t Z = A & 0x1fffe;
						const uint8_t *p = ((Z & 0x10000) ? hi : lo) + (Z & 0xffff);
						data = (p[0] << 8) | p[1];
					} else {
						data = DELEGATE_CALL(gime->public.fetch_vram, A);
					}
					Xoff += 2;
					r = data >> 8;
					cache = data;
					have = 1;
				}
				buf[k++] = r;
				if (lose_low && j == 1)
					cache = 0;
			}
		}
		gime->have_vdata_cache = have;
		gime->vdata_cache = cache;
		gime->Xoff = Xoff;
	}

	// Skip a line whose inputs have not changed since it was last drawn.
	if (gime->public.port_redraw) {
		memset(gime->port_sum, 0, sizeof(gime->port_sum));
		gime->public.port_redraw = 0;
	}
	uint32_t h = 0x811C9DC5u;
	h = port_mix(h, COCO | GnA << 1 | GM1 << 2 | GM0 << 3 | CSS << 4 | rg_full << 5 | BP << 6
	             | (cmask == 0x30) << 7 | INVT << 8 | blink << 9 | resolution << 10 | CRES << 12
	             | lose_low << 14 | (uint32_t)border << 16 | (uint32_t)(tAA >> 1) << 22);
	h = port_mix(h, font_row | rowmask << 8);
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, pal + i, 4);
		h = port_mix(h, w);
	}
	buf[nfetch] = buf[nfetch + 1] = buf[nfetch + 2] = 0;
	for (unsigned i = 0; i < nfetch; i += 4) {
		uint32_t w;
		memcpy(&w, buf + i, 4);
		h = port_mix(h, w);
	}
	h |= 1;
	unsigned sl = gime->scanline;
	if (sl < TCC1014_PORT_LINES) {
		if (gime->port_sum[sl] == h) {
			gime->port_skip = 1;
			return;
		}
		gime->port_sum[sl] = h;
	}
	// PIZERO-330: the decode's inputs, for tcc1014_port_decode here or on
	// the port's other core.
	struct tcc1014_port_job *j = &gime->port_job;
	memcpy(j->pal, pal, 16);
	j->nbytes = nbytes;
	j->side = (640 - tAA) / (1280 / TCC1014_PORT_W);
	j->flags = (COCO ? TCC1014_PJ_COCO : 0) | (GnA ? TCC1014_PJ_GNA : 0) | (GM1 ? TCC1014_PJ_GM1 : 0)
	         | (GM0 ? TCC1014_PJ_GM0 : 0) | (CSS ? TCC1014_PJ_CSS : 0) | (BP ? TCC1014_PJ_BP : 0)
	         | (rg_full ? TCC1014_PJ_RG_FULL : 0) | (INVT ? TCC1014_PJ_INVT : 0)
	         | (blink ? TCC1014_PJ_BLINK : 0) | (attr ? TCC1014_PJ_ATTR : 0);
	j->CRES = CRES;
	j->resolution = resolution;
	j->cmask = cmask;
	j->border = border;
	j->font_row = font_row;
	j->rowmask = rowmask;
#ifdef CPU_PROF
	uint32_t pr_t1 = cpu_prof_now();
	g_cpu_prof.pr_gather += pr_t1 - pr_t0;
#endif
	if (gime->public.port_defer) {
		gime->port_job_ready = 1;
	} else {
		tcc1014_port_decode(j, gime->port_line);
	}
#ifdef CPU_PROF
	g_cpu_prof.pr_decode += cpu_prof_now() - pr_t1;
	g_cpu_prof.pr_n++;
	// PIZERO-322: which mode the drawn lines are in
	g_cpu_prof.pr_mode = COCO | GnA << 1 | GM1 << 2 | GM0 << 3 | CSS << 4 | BP << 5 | attr << 6
	                     | CRES << 8 | resolution << 12;
	g_cpu_prof.pr_bytes = nbytes;
	if (!(!COCO && BP)) g_cpu_prof.pr_general++;
#endif
}

// PIZERO-330: the decode, from a job alone, so it can run on another core.
void tcc1014_port_decode(const struct tcc1014_port_job *j, uint8_t *pixel) {
	uint8_t *const line = pixel;
	const unsigned f = j->flags;
	const _Bool COCO = f & TCC1014_PJ_COCO, GnA = f & TCC1014_PJ_GNA, GM1 = f & TCC1014_PJ_GM1;
	const _Bool GM0 = f & TCC1014_PJ_GM0, CSS = f & TCC1014_PJ_CSS, BP = f & TCC1014_PJ_BP;
	const _Bool rg_full = f & TCC1014_PJ_RG_FULL, INVT = f & TCC1014_PJ_INVT;
	const _Bool blink = f & TCC1014_PJ_BLINK, attr = f & TCC1014_PJ_ATTR;
	const unsigned CRES = j->CRES, resolution = j->resolution;
	const uint_fast8_t cmask = j->cmask;
	const uint8_t border = j->border;
	const unsigned font_row = j->font_row, rowmask = j->rowmask;
	const unsigned nbytes = j->nbytes;
	uint8_t pal[16];
	memcpy(pal, j->pal, 16);

	// Decode. Each VRAM byte becomes eight colours in one 64-bit word, the
	// first pixel in the low byte: render_scanline's c0-c3 for its two 4-bit
	// groups. Two-colour patterns pick foreground or background through
	// port_mask (byte i is 0xFF when bit 7-i is set); the four- and
	// sixteen-colour ones look up pairs and quads. Then the word is emitted
	// by resolution.
	memset(pixel, border, j->side);
	pixel += j->side;
	const uint8_t *src = j->bytes;

	if (!COCO && BP) {
		// PIZERO-220: the bitplane modes, the ones a game redraws whole,
		// each in its own loop. The general loop below tests the mode for
		// every byte, about 49 clocks a byte on HSCREEN 2.
		uint8_t pm[16];
		for (int i = 0; i < 16; i++)
			pm[i] = pal[i] & cmask;
		switch (CRES) {
		case 0: default: {
			const uint8_t fg = pm[1], bg = pm[0];
			PORT_LOOP(port_sel(port_mask[vdata], fg, bg));
			break;
		}
		case 1:
			PORT_LOOP(port_pairs(pm[(vdata >> 6) & 3], pm[(vdata >> 4) & 3],
			                     pm[(vdata >> 2) & 3], pm[vdata & 3]));
			break;
		case 2: case 3:
#ifdef TCC1014_PORT_FULL
			// PIZERO-322: 320 x 16 colours, the highest resolution, which
			// keeps two of each colour's four: each byte is its two pixels
			// doubled, one 32-bit store from a table of the 16 colours
			// already doubled (about 27 clocks a byte through port_quads
			// and port_emit).
			if (resolution == 3) {
				uint16_t p2[16];
				for (int i = 0; i < 16; i++)
					p2[i] = (uint16_t)(pm[i] * 0x0101u);
				for (unsigned n = nbytes; n; --n) {
					uint_fast8_t vdata = *src++;
					uint32_t w = p2[vdata >> 4] | (uint32_t)p2[vdata & 15] << 16;
					memcpy(pixel, &w, 4);
					pixel += 4;
				}
				break;
			}
#endif
			PORT_LOOP(port_quads(pm[vdata >> 4], pm[vdata & 15]));
			break;
		}
	} else for (unsigned n = nbytes; n; --n) {
		uint_fast8_t vdata = *src++;
		uint64_t cols;

		if (COCO) {
			if (GnA) {
				if (GM0) {
					unsigned g = vdata;
					if (!rg_full)
						g = ((g & 0x40) ? 0xc0 : 0) | ((g & 0x10) ? 0x30 : 0) | ((g & 0x04) ? 0x0c : 0) | ((g & 0x01) ? 0x03 : 0);
					cols = port_sel(port_mask[g],
					                pal[CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1],
					                pal[CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0]);
				} else {
					unsigned cg = !CSS ? TCC1014_GREEN : TCC1014_WHITE;
					cols = port_pairs(pal[cg + ((vdata >> 6) & 3)], pal[cg + ((vdata >> 4) & 3)],
					                  pal[cg + ((vdata >> 2) & 3)], pal[cg + (vdata & 3)]);
				}
			} else if (vdata & 0x80) {
				unsigned g = (font_row < 6) ? (vdata >> 2) : vdata;
				unsigned m = ((g & 2) ? 0xf0 : 0) | ((g & 1) ? 0x0f : 0);
				cols = port_sel(port_mask[m], pal[(vdata >> 4) & 7], pal[TCC1014_RGCSS0_0]);
			} else {
				_Bool INV = vdata & 0x40;
				INV ^= GM1;
				uint_fast8_t c = vdata & 0x7f;
				if (c < 0x20) {
					c |= (GM0 ? 0x60 : 0x40);
					INV ^= GM0;
				} else if (c >= 0x60) {
					c ^= 0x40;
				}
				unsigned g = font_gime[c*12+font_row];
				if (INV ^ INVT)
					g = ~g & 0xff;
				cols = port_sel(port_mask[g],
				                pal[CSS ? TCC1014_BRIGHT_ORANGE : TCC1014_BRIGHT_GREEN],
				                pal[CSS ? TCC1014_DARK_ORANGE : TCC1014_DARK_GREEN]);
			}
		} else if (BP) {
			switch (CRES) {
			case 0: default:
				cols = port_sel(port_mask[vdata], pal[1] & cmask, pal[0] & cmask);
				break;
			case 1:
				cols = port_pairs(pal[(vdata >> 6) & 3] & cmask, pal[(vdata >> 4) & 3] & cmask,
				                  pal[(vdata >> 2) & 3] & cmask, pal[vdata & 3] & cmask);
				break;
			case 2: case 3:
				cols = (uint64_t)((pal[(vdata >> 4) & 15] & cmask) * 0x01010101u)
				     | (uint64_t)((pal[vdata & 15] & cmask) * 0x01010101u) << 32;
				break;
			}
		} else {
			unsigned g = font_gime[(vdata & 0x7f)*12+font_row];
			unsigned fgi = 1, bgi = 0;
			if (attr) {
				uint_fast8_t a = *src++;
				fgi = 8 | ((a >> 3) & 7);
				bgi = a & 7;
				if ((a & 0x80) && blink)
					fgi = bgi;
				if ((a & 0x40) && ((font_row & rowmask) == rowmask))
					g = 0xff;
			}
			cols = port_sel(port_mask[g], pal[fgi] & cmask, pal[bgi] & cmask);
		}

#ifdef TCC1014_PORT_FULL
		pixel = port_emit(pixel, cols, resolution);
#else
		switch (resolution) {
		case 0: {
			uint32_t lo = (uint32_t)cols, hi = (uint32_t)(cols >> 32);
			uint64_t a = port_double(lo), b = port_double(hi);
			memcpy(pixel, &a, 8);
			memcpy(pixel + 8, &b, 8);
			pixel += 16;
			break;
		}
		case 1:
			memcpy(pixel, &cols, 8);
			pixel += 8;
			break;
		case 2:
			pixel[0] = cols; pixel[1] = cols >> 16;
			pixel[2] = cols >> 32; pixel[3] = cols >> 48;
			pixel += 4;
			break;
		case 3:
			pixel[0] = cols; pixel[1] = cols >> 32;
			pixel += 2;
			break;
		}
#endif
	}

	memset(pixel, border, TCC1014_PORT_W - (pixel - line));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Timer handling

static void schedule_timer(struct TCC1014_private *gime, event_ticks t) {
	unsigned timer_reset = ((gime->registers[4] & 0x0f) << 8) | gime->registers[5];
	gime->timer.counter = timer_reset ? (timer_reset + gime->variant->timer_offset) : 0;
	if (gime->TINS && timer_reset > 0) {
		// TINS=1: 3.58MHz
		event_queue_abs(&gime->timer.update_event, t + (gime->timer.counter << 2));
	} else {
		event_dequeue(&gime->timer.update_event);
	}
}

static void update_timer(struct TCC1014_private *gime, event_ticks t) {
	SET_INTERRUPT(gime, INT_TMR);
	gime->blink = !gime->blink;
	schedule_timer(gime, t);
}

static void do_update_timer(void *sptr) {
	struct TCC1014_private *gime = sptr;
	update_timer(gime, gime->timer.update_event.at_tick);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Interpret GIME registers

static void update_from_gime_registers(struct TCC1014_private *gime) {
	// Render scanline so far before changing modes
	render_scanline(gime, event_current_tick);

	// Decode VDG-compatible mode setting
	gime->VDG.GnA = gime->PIA1B_shadow.pdr & 0x80;
	gime->VDG.GM1 = gime->PIA1B_shadow.pdr & 0x20;
	gime->VDG.GM0 = gime->PIA1B_shadow.pdr & 0x10;
	gime->VDG.CSS = gime->PIA1B_shadow.pdr & 0x08;

	if (gime->COCO) {
		// VDG compatible mode

		// Bytes per row, render resolution
		if (!gime->VDG.GnA || !(gime->SAM_V & 1)) {
			gime->BPR = 32;
			gime->resolution = 1;
		} else {
			gime->BPR = 16;
			gime->resolution = 0;
		}

		// Line counts
		gime->vertical.lTB = gime->H50 ? 63 : 36;
		gime->rowmask = gime->VDG.GnA ? SAM_V_rowmask[gime->SAM_V] : VSC_rowmask[gime->VSC];
	} else {
		// CoCo 3 modes

		// Bytes per row, render resolution
		if (gime->BP) {
			gime->BPR = VRES_HRES_BPR[gime->HRES];
			gime->resolution = gime->HRES >> 1;
		} else {
			gime->BPR = VRES_HRES_BPR_TEXT[gime->HRES];
			gime->resolution = (gime->HRES & 4) ? 2 : 1;
		}

		// Line counts
		gime->vertical.lTB = VRES_LPF_lTB[gime->H50][gime->LPF];
		gime->rowmask = LPR_rowmask[gime->LPR];
	}
}

// Interpret SAM compatibility register

static void update_from_sam_register(struct TCC1014_private *gime) {
	gime->TY = gime->SAM_register & 0x8000;
	gime->R1 = gime->SAM_register & 0x1000;
	gime->SAM_F = (gime->SAM_register >> 3) & 0x7f;
	gime->SAM_V = gime->SAM_register & 0x7;
	update_from_gime_registers(gime);
}

/*
 * coco_machine.cpp: see coco_machine.h.
 *
 * Owns:
 *   * 64 KB RAM in PSRAM
 *   * 16 KB ROM pointer (caller-owned, lives in flash)
 *   * MC6809, MC6883, two MC6821s, MC6847: all via the XRoar part system
 *   * The machine event list (assigned to xroar.h's global pointer)
 *   * 256x192 VDG palette-index buffer
 *
 * Bus dispatch follows the real CoCo: SAM decode picks the I/O region
 * for reads, raw address picks the device for writes (since the real
 * chip uses RAS0/nWE alongside S=7 for RAM writes). SAM register
 * writes at $FFC0-$FFDF are handled by SAM->mem_cycle which we call
 * for the cycle-count side effect.
 */

#include <Arduino.h>
#include "vdg_pack.h"
#include "cart_gmc.h"          // PIZERO-142, host-tested
// PIZERO-248: the sound chips' render path in RAM (see psg_ay8910.h).
#define SOUND_HOT __attribute__((section(".time_critical.sound")))
#define SOUND_TABLE __attribute__((section(".data.sound_tables")))
#include "csg_sn76489.h"       // PIZERO-143, host-tested
#include "psg_ay8910.h"        // PIZERO-149, host-tested
#include "joy_compare.h"       // PIZERO-13, host-tested
#include "coco_palette.h"
#include "audio_servo.h"
#include "audio_integ.h"       // PIZERO-131, host-tested
#include "audio_lowpass.h"     // PIZERO-212, host-tested
#include "coco_sound.h"        // PIZERO-253, host-tested
#include "coco3_bus.h"         // PIZERO-133, host-tested
#ifdef GIME_TIMER
#include "gime_timer.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern "C" {
#include "coco_machine.h"
#include "hot.h"
#include "rp_pico_alloc.h"
#include "part.h"
#include "mc6809/mc6809.h"
#include "mc6883.h"
#include "mc6821.h"
#include "mc6847/mc6847.h"
#include "events.h"
#include "delegate.h"
#include "xroar.h"
#include "cpu_prof.h"        // PIZERO-209: empty unless CPU_PROF
#include "coco_fastbus.h"     // PIZERO-131: RAM and ROM read by the core itself
#include "acia6850.h"         // PIZERO-287: the MIDI Pak
#include "tcc1014/tcc1014.h"   // PIZERO-133: the CoCo 3's GIME
}

struct CocoMachine {
    uint8_t *ram = nullptr;
    const uint8_t *rom = nullptr;
    size_t rom_len = 0;
    const uint8_t *cart_rom = nullptr;   // at $C000, NULL = no cart
    uint16_t cart_mask = 0x1FFF;         // PIZERO-139: size - 1, so a small cart repeats
    // PIZERO-142: a bank-switched cart (over 16 KB). cart_rom then points at
    // the selected bank inside cart_base, so reads cost nothing extra.
    const uint8_t *cart_base = nullptr;
    uint32_t cart_len = 0;
    bool     cart_banked = false;
    uint32_t csg_writes = 0;             // bytes written to the SN76489 (PIZERO-143)
    int32_t cart_toggle_remaining = 0;   // 6809 cycles until next CART pulse
    bool    cart_cb1_level = true;
    bool    cart_autostart = false;      // PIZERO-191: this cart drives CART

    // Minimal WD279x-ish FDC state, enough for DECB sector reads.
    // Registers map at $FF48 (status/cmd), $FF49 (track), $FF4A (sector),
    // $FF4B (data). $FF40 is the RSDOS latch (drive/density/halt).
    uint8_t fdc_status = 0x04;   // TRACK00 set when track==0
    uint8_t fdc_track  = 0;
    uint8_t fdc_sector = 0;
    uint8_t fdc_data   = 0;
    uint8_t fdc_command = 0;
    uint8_t fdc_latch  = 0;      // $FF40 mirror
    uint8_t fdc_buf[256];
    int     fdc_buf_pos = 0;
    int     fdc_buf_len = 0;
    unsigned fdc_drive = 0;
    bool    fdc_busy = false;
    bool    fdc_writing = false;  // PIZERO-66: a Write Sector is collecting bytes
    bool    fdc_type1 = true;     // PIZERO-320: status is Type I's (TRACK00, INDEX)
    bool    fdc_intrq = false;    // PIZERO-320: INTRQ, held until status is read
    coco_disk_read_sector_fn fdc_read_cb = nullptr;
    coco_disk_write_sector_fn fdc_write_cb = nullptr;
    coco_disk_idle_fn fdc_idle_cb = nullptr;

    // AMOLED-33: PIA IRQ outputs only change at well-defined moments, not on
    // every memory access. The dirty flag lets coco_mem_cycle skip the
    // per-cycle re-evaluation (4 loads + 2 ORs + 2 stores) when nothing
    // could have changed.
    bool pia_irq_dirty = true;

    // AMOLED-32: mirror of the SAM TY bit (all-RAM mode). DECB writes
    // $FFDF at boot to flip this on; we shadow it so our fast-path address
    // decode doesn't need to reach into the opaque MC6883_private struct.
    bool sam_ty = false;

    // AMOLED-26: shadow of the SAM F register (VDG display-memory base
    // address). Bits 9-15 are settable via $FFC6/C7..$FFD2/D3 (clear/set
    // each bit by writing the even/odd address). Used by coco_vdg_fetch
    // to read from the right framebuffer for graphics modes like RG6.
    uint16_t sam_f = 0;
    uint8_t  sam_v = 0;          // PIZERO-140: SAM VDG mode V2-V0, sets row geometry
    uint8_t  sam_r = 0;          // PIZERO-132: SAM MPU rate R1-R0 (POKE 65495/65497)


    struct MC6809 *cpu = nullptr;
    struct MC6883 *sam = nullptr;
    struct MC6821 *pia0 = nullptr;
    struct MC6821 *pia1 = nullptr;
    struct MC6847 *vdg = nullptr;

    uint32_t total_mem_cycles = 0;
    uint32_t render_lines = 0;

    // Nibble-packed: 2 palette indices per byte. Low nibble = even cx,
    // high nibble = odd cx. See coco_vdg_render / coco_boot_blit_vdg_src.
    // alignas(4): the alpha and RG6 renderers write it a 32-bit word at a
    // time (8 packed pixels per store), which was already assumed before
    // PIZERO-119 and is now relied on by three paths. Make it explicit.
#ifdef COCO3
    // PIZERO-133: a CoCo 3 draws straight into the framebuffer, and needs
    // these 24 KB for its second 64 KB of RAM. PIZERO-135: a build that can
    // be either machine takes them from the heap only as a CoCo 2.
    uint8_t *vdg_buffer;
#else
    alignas(4) uint8_t vdg_buffer[COCO_VDG_H * (COCO_VDG_W / 2)];
#endif
};
static_assert((COCO_VDG_W & 1) == 0, "COCO_VDG_W must be even for nibble packing");
#define COCO_VDG_BUF_BYTES (COCO_VDG_H * (COCO_VDG_W / 2))

static CocoMachine g_m;

// PIZERO-133: CoCo 3 mode, chosen by giving coco_machine_init a 32 KB ROM.
// The GIME is then the bus: the 6809's cycles go to tcc1014_mem_cycle, which
// decodes them through its MMU and calls coco3_cpu_cycle with the result.
// coco3_bus.h holds the portable half (RAM, ROM, video fetch, scanlines).
// Only the CoCo 3 build (-DCOCO3, env pizero_coco3) carries it; elsewhere
// g_c3 is a constant false and the compiler drops every CoCo 3 branch.
#ifdef COCO3
static bool g_c3 = false;
#else
static const bool g_c3 = false;
#endif
static struct TCC1014 *g_gime3 = nullptr;
static struct coco3_bus g_c3bus;

// PIZERO-55/85: the palette register file. A real 6847 has no palette at all,
// but this port already looks every pixel up in a 16-entry table on its way to
// RGB565, so making that table writable is free per pixel and gives the guest
// CoCo 3-style control through the GIME registers at $FFB0-$FFBF.
//
// Factory = the stock VDG colours, so a machine that never touches $FFB0 looks
// exactly as it always did. Indices 12-15 are unused by the 6847.
static const uint16_t g_pal_factory[COCO_PAL_COUNT] = {
    0x07E0, 0xFFE0, 0x001F, 0xF800, 0xFFFF, 0x07FF, 0xF81F, 0xFC00,
    0x0000, 0x0320, 0x8200, 0xFCA0, 0x0000, 0x0000, 0x0000, 0x0000,
};
// PIZERO-145: the palette a reset (and the cold reset) restores. Starts as
// the factory colors; color_ lines in settings.txt override entries.
static uint16_t g_pal_default[COCO_PAL_COUNT] = {
    0x07E0, 0xFFE0, 0x001F, 0xF800, 0xFFFF, 0x07FF, 0xF81F, 0xFC00,
    0x0000, 0x0320, 0x8200, 0xFCA0, 0x0000, 0x0000, 0x0000, 0x0000,
};
static coco_palette_t g_pal = {
    { 0x07E0, 0xFFE0, 0x001F, 0xF800, 0xFFFF, 0x07FF, 0xF81F, 0xFC00,
      0x0000, 0x0320, 0x8200, 0xFCA0, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0 }, false
};

// The blit reads this every frame, so a write previews live with no repaint
// plumbing. Note the render LUTs (g_alpha_lut, the SG4 and RG6 tables) store
// palette INDICES rather than colours, so a palette write needs no rebuild of
// them -- the wrinkle PIZERO-26 warned about does not apply to this port.
extern "C" const uint16_t *coco_machine_palette(void) { return g_pal.rgb565; }

extern "C" void coco_machine_palette_reset(void) {
    coco_pal_reset(&g_pal, g_pal_default);
}

// PIZERO-145: set the default palette. NULL restores the factory colors;
// otherwise entries whose bit is set in `mask` take the given color and the
// rest are factory. The live palette follows at once.
extern "C" void coco_machine_palette_set_default(const uint16_t *rgb565, uint16_t mask) {
    for (int i = 0; i < COCO_PAL_COUNT; i++)
        g_pal_default[i] = (rgb565 && (mask & (1u << i))) ? rgb565[i] : g_pal_factory[i];
    coco_machine_palette_reset();
}

extern "C" void coco_machine_palette_set(uint8_t idx, uint16_t rgb565) {
    coco_pal_set_rgb565(&g_pal, idx, rgb565);
}

extern "C" uint16_t coco_machine_palette_get(uint8_t idx) {
    return coco_pal_get_rgb565(&g_pal, idx);
}

extern "C" bool coco_machine_palette_written(void) { return g_pal.written; }

#ifdef GIME_TIMER
// PIZERO-62: the CoCo 3 timer in its real register block, which is unused on a
// CoCo 2. Scheduling rides the existing event queue, so an idle timer costs
// nothing: the memory-access fast path already skips the queue when nothing is
// due, and a stopped timer is simply not queued.
static gime_timer_t  g_gime;
static struct event  g_gime_event;
static bool          g_gime_event_ready = false;

static void gime_timer_restart(void);

static void gime_timer_expired(void *) {
    uint8_t lines = gime_timer_fire(&g_gime);
    if (lines) g_m.pia_irq_dirty = true;   // force the IRQ/FIRQ re-OR below
    gime_timer_restart();                  // auto-reload, as the GIME does
}

static void gime_timer_restart(void) {
    if (!g_gime_event_ready) {
        event_init(&g_gime_event, MACHINE_EVENT_LIST,
                   DELEGATE_AS0(void, gime_timer_expired, NULL));
        g_gime_event_ready = true;
    }
    if (event_queued(&g_gime_event)) event_dequeue(&g_gime_event);
    uint32_t dt = gime_timer_interval(&g_gime);
    if (dt) event_queue_dt(&g_gime_event, (int)dt);
}
#endif

// - - - bus -------------------------------------------------------

// Keyboard matrix state. Per xroar upstream (coco3.c / dragon.c) the CoCo's
// PIA0 wiring is: PA = ROWS (read by CPU), PB = COLUMNS (written by CPU).
// dkbd_matrix_point's (row, col) follows the same convention. We store one
// byte per dkbd-column, each bit a dkbd-row: 1=not pressed, 0=pressed.
static uint8_t g_kb_col_row_mask[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
// PIZERO-163: one layer per key source (USB keyboard, serial or autotype,
// gamepad), so one source releasing a key cannot drop another's hold.
// g_kb_col_row_mask above is their AND, kept current on every change, so the
// PIA read below costs the same as before.
static uint8_t g_kb_src[COCO_KEY_SOURCES][8];
static bool    g_kb_src_ready;

static void kb_src_init(void) {
    memset(g_kb_src, 0xFF, sizeof g_kb_src);
    g_kb_src_ready = true;
}

static void kb_combine(uint8_t col) {
    uint8_t m = 0xFF;
    for (int s = 0; s < COCO_KEY_SOURCES; s++) m &= g_kb_src[s][col];
    g_kb_col_row_mask[col] = m;
}

// AMOLED-22 globals consumed by mc6847.c's render_scanline LUT rebuild.
// g_artifact_active is set by coco_pia1b_postwrite to indicate the VDG is
// truly in PMODE 4 / RG6 (¬A/G + GM2 + GM1 + GM0). When non-zero, the LUT
// emits NTSC artifact colours instead of plain fg/bg pixel duplicates.
extern "C" _Bool g_artifact_active = 0;
// PIZERO-145: runtime switches, set from /coco/settings.txt.
static uint8_t g_artifact_mode =           // 0 off (mono), 1 on, 2 swapped phase
#ifdef ARTIFACT_PHASE_LEGACY
    2;
#else
    1;
#endif
static bool g_gime_pal_on = true;           // $FFB0-$FFBF palette registers
static bool g_gime_timer_on = true;         // $FF90-$FF95 timer and interrupts
extern "C" _Bool g_artifact_css    = 0;
// PIZERO-166: the font set (VDG_FONT_* in vdg_pack.h), and the T1's EXT line
// (PIA1 PB bit 4), which switches the T1 and T2 to true lower case.
static int  g_font = VDG_FONT_T2;
static bool g_vdg_ext = false;
static bool g_lowercase = false;              // the lowercase setting (T2 only)

// AMOLED-26: PIA1 PB bits 7..3 are the VDG mode lines (¬A/G, GM2, GM1, GM0,
// CSS). Demos write PIA1 PB to switch to PMODE 4 / RG6. Match upstream
// dragon/coco wiring: extract the value, mirror GM0 into the INT/EXT bit,
// hand to mc6847_set_mode. Fired from PIA1 PB postwrite.
// PIZERO-166 diagnostic: writes to PIA1 port B and the last value, for [vdg].
static uint32_t g_pb_writes = 0;
static uint8_t  g_pb_last = 0;
extern "C" void coco_machine_pia1b_trace(uint32_t *writes, uint8_t *last, _Bool *ext) {
    *writes = g_pb_writes; *last = g_pb_last;
    *ext = vdg_lower_case(g_vdg_ext, g_font, g_lowercase);   // lower case in effect
}

extern "C" void coco_pia1b_postwrite(void *sptr) {
    (void)sptr;
    if (!g_m.pia1 || !g_m.vdg) return;
    g_pb_writes++;
    g_pb_last = (uint8_t)(g_m.pia1->b.out_source & g_m.pia1->b.out_sink);
    unsigned pb = (g_m.pia1->b.out_source & g_m.pia1->b.out_sink) & 0xf8;
    unsigned vmode = pb | ((pb & 0x10) << 4);   // GM0 -> INT/EXT
    mc6847_set_mode(g_m.vdg, vmode);
    // AMOLED-22: PMODE 4 / RG6 = ¬A/G + GM2 + GM1 + GM0 (PB bits 7,6,5,4)
    // all set. CSS (PB bit 3) chooses the colour pair.
    g_artifact_active = ((pb & 0xF0) == 0xF0);
    g_artifact_css    = (pb & 0x08) != 0;
    g_vdg_ext         = (pb & 0x10) != 0;   // PIZERO-166: T1 EXT (lower case) = GM0
}

// PIZERO-13: two joysticks, ported from the Fruit Jam port (FRUITJAM-18).
// Port 0 = RIGHT joystick (fire on PIA0 PA0), port 1 = LEFT (PA1), as XRoar
// numbers them. Axis 0 = X (0 left), 1 = Y (0 up); 16-bit, because the
// comparator is (joy_compare.h). Centered and released until a pad reports.
static uint16_t g_joy_axis[2][2] = { { 32767, 32767 }, { 32767, 32767 } };
static uint8_t  g_joy_fire = 0;      // bit0 = port 0 (PA0), bit1 = port 1 (PA1)
                                     // PIZERO-215: bits 2-3 the second buttons (PA2, PA3)

extern "C" void coco_machine_set_joystick_axis(int port, int axis, uint16_t value) {
    if ((unsigned)port > 1 || (unsigned)axis > 1) return;
    g_joy_axis[port][axis] = value;
}

extern "C" void coco_machine_set_joystick_fire(int port, _Bool pressed) {
    if ((unsigned)port > 1) return;
    if (pressed) g_joy_fire |=  (uint8_t)(1u << port);
    else         g_joy_fire &= (uint8_t)~(1u << port);
}

extern "C" void coco_machine_set_joystick_fire2(int port, _Bool pressed) {
    if ((unsigned)port > 1) return;
    if (pressed) g_joy_fire |=  (uint8_t)(4u << port);
    else         g_joy_fire &= (uint8_t)~(4u << port);
}

extern "C" void coco_machine_release_all_joysticks(void) {
    g_joy_axis[0][0] = g_joy_axis[0][1] = 32767;
    g_joy_axis[1][0] = g_joy_axis[1][1] = 32767;
    g_joy_fire = 0;
}

static void HOT_FUNC(audio_comparator_read)(void);   // PIZERO-253, with the audio below

extern "C" void HOT_FUNC(coco_pia0_preread_a)(void *sptr) {
    (void)sptr;
    if (!g_m.pia0) return;
    audio_comparator_read();
    uint8_t col_sel = PIA_VALUE_B(g_m.pia0);
    uint8_t rows = 0xFF;
    for (int c = 0; c < 8; c++) {
        if (!(col_sel & (1u << c))) {
            rows &= g_kb_col_row_mask[c];
        }
    }
    // Fire buttons pull PA0/PA1 low whichever column is strobed (upstream:
    // row_sink &= ~(buttons & 3)); on a CoCo 3 the second buttons PA2/PA3
    // too (upstream coco3.c: buttons & 15).
    rows &= (uint8_t)~(g_joy_fire & (g_c3 ? 15 : 3));
    // Comparator on PA7: CB2 picks the port, CA2 the axis (two separate
    // selects, unlike the sound mux, which reads them as one 2-bit code).
    if (g_m.pia1) {
        unsigned port = PIA_VALUE_CB2(g_m.pia0) ? 1u : 0u;
        unsigned axis = PIA_VALUE_CA2(g_m.pia0) ? 1u : 0u;
        if (joy_comparator_high(g_joy_axis[port][axis], (uint8_t)PIA_VALUE_A(g_m.pia1)))
            rows |= 0x80;
        else
            rows &= 0x7F;
    }
    g_m.pia0->a.in_sink = rows;
}

// Per upstream xroar/src/dkbd.c, CoCo's PIA0 row layout is rotated relative
// to the raw dkbd scancode bits: row = (raw_row + 4) % 6 for rows 0-5, with
// row 6 (control keys) unchanged. col = dscan & 7 directly.
static inline void dscan_to_row_col(uint8_t dscan, uint8_t *row, uint8_t *col) {
    uint8_t raw_row = (dscan >> 3) & 7;
    *col = dscan & 7;
    *row = (raw_row == 6) ? 6 : (uint8_t)((raw_row + 4) % 6);
}

extern "C" void coco_machine_press_key_src(int src, uint8_t dscan) {
    if (dscan >= 0x40 || (unsigned)src >= COCO_KEY_SOURCES) return;
    if (!g_kb_src_ready) kb_src_init();
    uint8_t row, col;
    dscan_to_row_col(dscan, &row, &col);
    g_kb_src[src][col] &= (uint8_t)~(1u << row);
    kb_combine(col);
}

extern "C" void coco_machine_release_key_src(int src, uint8_t dscan) {
    if (dscan >= 0x40 || (unsigned)src >= COCO_KEY_SOURCES) return;
    if (!g_kb_src_ready) kb_src_init();
    uint8_t row, col;
    dscan_to_row_col(dscan, &row, &col);
    g_kb_src[src][col] |= (uint8_t)(1u << row);
    kb_combine(col);
}

extern "C" void coco_machine_release_all_keys_src(int src) {
    if ((unsigned)src >= COCO_KEY_SOURCES) return;
    if (!g_kb_src_ready) kb_src_init();
    memset(g_kb_src[src], 0xFF, sizeof g_kb_src[src]);
    for (uint8_t c = 0; c < 8; c++) kb_combine(c);
}

// The original calls are the USB keyboard's layer.
extern "C" void coco_machine_press_key(uint8_t dscan) {
    coco_machine_press_key_src(COCO_KEYS_USB, dscan);
}

extern "C" void coco_machine_release_key(uint8_t dscan) {
    coco_machine_release_key_src(COCO_KEYS_USB, dscan);
}

// Every source: reset, and the overlay opening.
extern "C" void coco_machine_release_all_keys(void) {
    kb_src_init();
    memset(g_kb_col_row_mask, 0xFF, sizeof(g_kb_col_row_mask));
}


// - - - minimal FDC -----------------------------------------------
//
// Emulates just the WD279x register-level behaviour DECB actually uses
// for sector reads:
//   $FF40: RSDOS control latch: bit 4 = single/double density,
//           bits 0-3 = drive select, bit 5 = drive precomp/motor (we
//           don't model timing), bit 7 = HALT enable (we ignore, our
//           reads complete synchronously, so DECB's HALT wait just
//           passes through immediately).
//   $FF48: read: status. write: command.
//   $FF49: track register.
//   $FF4A: sector register.
//   $FF4B: data register.
//
// All operations complete instantly (no DRQ/INTRQ delay). DECB polls
// status looking for !BUSY + bytes available, and we satisfy that on
// the first poll after a READ_SECTOR command.

// PIZERO-320: the RS-DOS controller wires INTRQ to NMI whenever $FF40 bit 5
// (double density) is set, as upstream's rsdos.c does (latch_write and
// set_intrq); bit 7 is HALT enable only. INTRQ is held until the status
// register is read or a new command is written, and turning double density
// on with INTRQ pending raises NMI then. Disk BASIC sets both bits for its
// transfers, so it sees the same NMIs as before; a loader that runs the
// controller itself with HALT off ($29: DD, motor, drive 0) and waits on NMI
// after a Restore now gets it. (Before, NMI needed bit 7.)
static void fdc_signal_intrq(void) {
    g_m.fdc_intrq = true;
    if (g_m.cpu && (g_m.fdc_latch & 0x20)) MC6809_NMI_SET(g_m.cpu, 1);
}

static void fdc_clear_intrq(void) {
    if (!g_m.fdc_intrq) return;
    g_m.fdc_intrq = false;
    if (g_m.cpu) MC6809_NMI_SET(g_m.cpu, 0);
}

// PIZERO-66: the status a disk callback's return value maps to. After a
// Type II command bit 7 is NOT READY, bit 6 WRITE PROTECT, bit 5 WRITE FAULT
// (on a write), bit 4 RECORD NOT FOUND.
static uint8_t fdc_status_for(int rc, bool writing) {
    if (rc == 0) return 0x00;
    if (rc == COCO_DISK_NOT_READY) return 0x80;
    if (rc == COCO_DISK_WRITE_PROTECT) return 0x40;
    return writing ? 0x20 : 0x10;
}

static void fdc_handle_command(uint8_t cmd) {
    g_m.fdc_command = cmd;
    g_m.fdc_writing = false;
    uint8_t top = cmd >> 4;
    g_m.fdc_type1 = (top <= 0x7 || top == 0xD);
    fdc_clear_intrq();                       // a new command drops INTRQ
    // Type IV: Force Interrupt
    if (top == 0xD) {
        g_m.fdc_busy = false;
        g_m.fdc_status = (g_m.fdc_track == 0) ? 0x04 : 0x00;
        g_m.fdc_buf_len = 0;
        if (cmd & 0x08) fdc_signal_intrq();  // $D8: interrupt now
        return;                              // $D0: no interrupt
    }
    // Type I: Restore, Seek, Step, StepIn, StepOut.
    if (top <= 0x7) {
        switch (top) {
        case 0x0: g_m.fdc_track = 0; break;             // Restore
        case 0x1: g_m.fdc_track = g_m.fdc_data; break;  // Seek
        case 0x2: case 0x3: /* Step in current direction */ break;
        case 0x4: case 0x5: g_m.fdc_track++; break;     // Step In
        case 0x6: case 0x7:
            if (g_m.fdc_track) g_m.fdc_track--;
            break;
        }
        g_m.fdc_busy = false;
        g_m.fdc_status = (g_m.fdc_track == 0) ? 0x04 : 0x00;
        fdc_signal_intrq();
        return;
    }
    // Type II: Read Sector (top 0x8/0x9) / Write Sector (0xA/0xB).
    if (top == 0x8 || top == 0x9) {
        g_m.fdc_buf_pos = 0;
        g_m.fdc_buf_len = 0;
        if (!g_m.fdc_read_cb) {
            g_m.fdc_status = 0x10;
            g_m.fdc_busy = false;
            fdc_signal_intrq();
            return;
        }
        int rc = g_m.fdc_read_cb(g_m.fdc_drive, g_m.fdc_track,
                                 g_m.fdc_sector, g_m.fdc_buf);
        if (rc != 0) {
            // PIZERO-114: an empty drive reports NOT READY, as a WD279x does
            // with no disk; anything else is RECORD NOT FOUND.
            g_m.fdc_status = (rc == COCO_DISK_NOT_READY) ? 0x80 : 0x10;
            g_m.fdc_busy = false;
            fdc_signal_intrq();
            return;
        }
        g_m.fdc_buf_len = 256;
        g_m.fdc_data = g_m.fdc_buf[0];
        g_m.fdc_status = 0x03;   // BUSY | DRQ
        g_m.fdc_busy = true;
        // NMI fires after the last byte is consumed, see fdc_io_read.
        return;
    }
    if (top == 0xA || top == 0xB) {
        // PIZERO-66: Write Sector. The writer is asked first whether the
        // drive will take a write (as the WD2797 samples write-protect at
        // command start); then DRQ stays up while the CPU writes 256 bytes
        // to $FF4B, and the sector goes to the writer on the last one (see
        // fdc_io_write). No writer: every disk is write-protected.
        int rc = g_m.fdc_write_cb
                     ? g_m.fdc_write_cb(g_m.fdc_drive, g_m.fdc_track, g_m.fdc_sector, nullptr)
                     : COCO_DISK_WRITE_PROTECT;
        if (rc != 0) {
            g_m.fdc_status = fdc_status_for(rc, true);
            g_m.fdc_busy = false;
            fdc_signal_intrq();
            return;
        }
        g_m.fdc_buf_pos = 0;
        g_m.fdc_buf_len = 256;
        g_m.fdc_writing = true;
        g_m.fdc_status = 0x03;   // BUSY | DRQ
        g_m.fdc_busy = true;
        return;
    }
    // Type III: stubs.
    g_m.fdc_status = 0x10;
    g_m.fdc_busy = false;
    fdc_signal_intrq();
}

// PIZERO-320: the index hole, once a revolution while the motor runs, as
// upstream's vdrive.c times it: a 6,272-byte track at 32 us a byte (300 rpm,
// about 200 ms) with the pulse 1/100 of that. In Type I status it is bit 1,
// and loaders that drive the controller themselves (with NMI off) wait on
// it after a Restore; Disk BASIC never looks.
#define FDC_REV_TICKS   ((event_ticks)EVENT_US(6272u * 32u))
#define FDC_INDEX_TICKS ((event_ticks)(FDC_REV_TICKS / 100))
static uint8_t fdc_status_now(void) {
    fdc_clear_intrq();                       // reading status drops INTRQ
    uint8_t st = g_m.fdc_status;
    if (g_m.fdc_type1 && !g_m.fdc_busy) {
        st &= (uint8_t)~0x06;
        if (g_m.fdc_track == 0) st |= 0x04;
        if ((g_m.fdc_latch & 0x08) && (event_current_tick % FDC_REV_TICKS) < FDC_INDEX_TICKS)
            st |= 0x02;
    }
#ifdef FDC_TRACE
    // PIZERO-320: is the guest polling status, and on what?
    { static uint32_t n = 0;
      if ((++n & 0xFFFF) == 0)
          FDC_TRACE_PORT.printf("[fdc] status reads=%lu st=%02x type1=%u latch=%02x\r\n",
                                (unsigned long)n, st, (unsigned)g_m.fdc_type1, g_m.fdc_latch); }
#endif
    return st;
}

static uint8_t fdc_io_read(uint16_t A) {
    switch (A & 0xF) {
    case 0x8: return fdc_status_now();
    case 0x9: return g_m.fdc_track;
    case 0xA: return g_m.fdc_sector;
    case 0xB: {
        uint8_t v = g_m.fdc_data;
        if (g_m.fdc_busy && g_m.fdc_buf_pos < g_m.fdc_buf_len - 1) {
            g_m.fdc_buf_pos++;
            g_m.fdc_data = g_m.fdc_buf[g_m.fdc_buf_pos];
            g_m.fdc_status = 0x03;  // BUSY + DRQ
        } else if (g_m.fdc_busy) {
            // last byte just delivered: clear BUSY + DRQ and fire NMI to wake
            // DECB's SYNC waiting for FDC completion. PIZERO-191: no TRACK00
            // here: after a Type II command bit 2 is LOST DATA, and setting
            // it on track 0 failed every NitrOS-9 read of LSN 0.
            g_m.fdc_busy = false;
            g_m.fdc_status = 0x00;
            fdc_signal_intrq();
        }
        return v;
    }
    default: return 0xFF;
    }
}

static void fdc_io_write(uint16_t A, uint8_t D) {
    switch (A & 0xF) {
    case 0x0: case 0x1: case 0x2: case 0x3:
    case 0x4: case 0x5: case 0x6: case 0x7:
        // $FF40-$FF47: RSDOS latch (drive sel / density / halt enable).
#ifdef FDC_TRACE
        if (D != g_m.fdc_latch)
            FDC_TRACE_PORT.printf("[fdc] latch=%02x pc=%04x\r\n", D, g_m.cpu ? g_m.cpu->reg_pc : 0);
#endif
        // PIZERO-66: motor off is when the host syncs the card.
        if ((g_m.fdc_latch & 0x08) && !(D & 0x08) && g_m.fdc_idle_cb) g_m.fdc_idle_cb();
        g_m.fdc_latch = D;
        if ((D & 0x20) && g_m.fdc_intrq && g_m.cpu) MC6809_NMI_SET(g_m.cpu, 1);   // PIZERO-320
        // PIZERO-190: bits 0-2 select drives 0-2, else bit 6 drive 3, else
        // drive 0, as upstream rsdos.c does. Bit 3 is MOTOR ON, not a drive.
        if (D & 0x01)      g_m.fdc_drive = 0;
        else if (D & 0x02) g_m.fdc_drive = 1;
        else if (D & 0x04) g_m.fdc_drive = 2;
        else if (D & 0x40) g_m.fdc_drive = 3;
        else               g_m.fdc_drive = 0;
        return;
    case 0x8:
        fdc_handle_command(D);
#ifdef FDC_TRACE
        FDC_TRACE_PORT.printf("[fdc] cmd=%02x d=%u t=%u s=%u -> st=%02x pc=%04x\r\n",
                      D, g_m.fdc_drive, g_m.fdc_track, g_m.fdc_sector,
                      g_m.fdc_status, g_m.cpu ? g_m.cpu->reg_pc : 0);
#endif
        return;
    case 0x9: g_m.fdc_track  = D;    return;
    case 0xA: g_m.fdc_sector = D;    return;
    case 0xB:
        g_m.fdc_data = D;
        if (g_m.fdc_busy && g_m.fdc_writing) {
            // PIZERO-66: the mirror of the read drain in fdc_io_read.
            g_m.fdc_buf[g_m.fdc_buf_pos++] = D;
            if (g_m.fdc_buf_pos < g_m.fdc_buf_len) {
                g_m.fdc_status = 0x03;  // BUSY + DRQ: next byte please
                return;
            }
            int rc = g_m.fdc_write_cb
                         ? g_m.fdc_write_cb(g_m.fdc_drive, g_m.fdc_track, g_m.fdc_sector, g_m.fdc_buf)
                         : COCO_DISK_WRITE_PROTECT;
            g_m.fdc_status = fdc_status_for(rc, true);
            g_m.fdc_busy = false;
            g_m.fdc_writing = false;
#ifdef FDC_TRACE
            FDC_TRACE_PORT.printf("[fdc] wrote d=%u t=%u s=%u -> st=%02x\r\n",
                                  g_m.fdc_drive, g_m.fdc_track, g_m.fdc_sector, g_m.fdc_status);
#endif
            fdc_signal_intrq();
        }
        return;
    default:  return;
    }
}

#ifdef FDC_TRACE
// PIZERO-190: the last jumps taken (from -> to, a repeat counted rather
// than stored), dumped when NitrOS-9's boot runs away (PIZERO-191): armed
// once the guest enters all-RAM mode (SAM TY, which REL sets first), fired
// the first time the CPU then executes in $8000-$EDFF, where no NitrOS-9
// Level 1 code lives. A
// runaway slides through memory without jumping, so the ring still holds
// the jumps that led to it.
#define PC_RING 256
struct pc_jump { uint16_t from, to, s, n; };
static struct pc_jump g_pc_ring[PC_RING];
static unsigned g_pc_head = 0;
static uint16_t g_pc_prev = 0;
static bool g_pc_armed = false, g_pc_dumped = false;
static void pc_trace_dump(const char *why, unsigned last);
static void pc_trace_hook(void *sptr) {
    struct MC6809 *cpu = (struct MC6809 *)sptr;
    uint16_t pc = cpu->reg_pc;
    if ((uint16_t)(pc - g_pc_prev) > 5) {             // not the next instruction
        struct pc_jump *last = &g_pc_ring[(g_pc_head + PC_RING - 1) % PC_RING];
        if (last->from == g_pc_prev && last->to == pc) {
            if (last->n < 0xFFFF) last->n++;
        } else {
            g_pc_ring[g_pc_head] = (struct pc_jump){ g_pc_prev, pc, cpu->reg_s, 1 };
            g_pc_head = (g_pc_head + 1) % PC_RING;
        }
    }
    if (pc == 0xA027 || pc == 0xA00E)     // Color BASIC reset entry (PIZERO-191)
        FDC_TRACE_PORT.printf("[pctrace] reset path pc=%04x from=%04x rstflg=%02x rstvec=%02x%02x\r\n",
                              pc, g_pc_prev, g_m.ram[0x71], g_m.ram[0x72], g_m.ram[0x73]);
    g_pc_prev = pc;
    if (g_m.sam_ty) g_pc_armed = true;
    if (g_pc_armed && !g_pc_dumped && pc >= 0x8000 && pc < 0xEE00) {
        g_pc_dumped = true;
        pc_trace_dump("runaway into $8000-$EDFF", PC_RING);
    }
}

static void pc_trace_dump(const char *why, unsigned last) {
    struct MC6809 *cpu = g_m.cpu;
    {
        FDC_TRACE_PORT.printf("[pctrace] %s; jumps oldest first (from>to s xN):\r\n", why);
        for (unsigned i = PC_RING - last; i < PC_RING; i++) {
            const struct pc_jump *j = &g_pc_ring[(g_pc_head + i) % PC_RING];
            if (!j->n) continue;
            FDC_TRACE_PORT.printf("%04x>%04x s%04x x%u%s", j->from, j->to, j->s, j->n,
                          (i % 6 == 5) ? "\r\n" : "  ");
        }
        FDC_TRACE_PORT.printf("\r\n[pctrace] cc=%02x a=%02x b=%02x x=%04x y=%04x u=%04x dp=%02x\r\n",
                      cpu->reg_cc, MC6809_REG_A(cpu), MC6809_REG_B(cpu),
                      cpu->reg_x, cpu->reg_y, cpu->reg_u, cpu->reg_dp);
        // Which source holds IRQ/FIRQ up.
        FDC_TRACE_PORT.printf("[pctrace] pia0 a cr=%02x irq=%u rx=%02x | b cr=%02x irq=%u rx=%02x | "
                      "pia1 a cr=%02x irq=%u | b cr=%02x irq=%u",
                      g_m.pia0->a.control_register, g_m.pia0->a.irq, g_m.pia0->a.irq1_received,
                      g_m.pia0->b.control_register, g_m.pia0->b.irq, g_m.pia0->b.irq1_received,
                      g_m.pia1->a.control_register, g_m.pia1->a.irq,
                      g_m.pia1->b.control_register, g_m.pia1->b.irq);
#ifdef GIME_TIMER
        FDC_TRACE_PORT.printf(" | gime lines=%02x", gime_timer_lines(&g_gime));
#endif
        FDC_TRACE_PORT.print("\r\n");
    }
}
#endif

// PIZERO-190: where the guest is, for locating a hang from the console.
extern "C" uint16_t coco_machine_cpu_pc(void) {
    return g_m.cpu ? g_m.cpu->reg_pc : 0;
}

#ifdef FDC_TRACE
// PIZERO-191: one row of the 32x16 text screen as ASCII, for reading the
// guest's screen from the console. VDG codes fold to 6 bits; semigraphics
// show as '#'.
extern "C" void coco_machine_screen_row(unsigned row, char out[33]) {
    const uint16_t base = g_m.sam_f ? g_m.sam_f : 0x0400;
    for (unsigned x = 0; x < 32; x++) {
        uint8_t c = g_m.ram[(uint16_t)(base + row * 32 + x)];
        uint8_t v = c & 0x3F;
        out[x] = (c & 0x80) ? '#' : (char)(v < 0x20 ? v + 0x40 : v);
    }
    out[32] = '\0';
}
#endif

// PIZERO-191: PIA0 control register B, whose bit 0 enables the 60 Hz IRQ.
extern "C" uint8_t coco_machine_pia0_crb(void) {
    return g_m.pia0 ? g_m.pia0->b.control_register : 0;
}

extern "C" void coco_machine_install_disk_reader(coco_disk_read_sector_fn fn) {
    g_m.fdc_read_cb = fn;
}

extern "C" void coco_machine_install_disk_writer(coco_disk_write_sector_fn fn) {
    g_m.fdc_write_cb = fn;
}

extern "C" void coco_machine_install_disk_idle(coco_disk_idle_fn fn) {
    g_m.fdc_idle_cb = fn;
}

extern "C" unsigned coco_machine_fdc_drive(void) {
    return g_m.fdc_drive;
}

extern "C" void coco_machine_loadm_write(uint16_t addr, const uint8_t *src, uint16_t len) {
    if (!g_m.ram) return;
    if (g_c3) {
        // PIZERO-133: through the map BASIC sets up, logical $0000-$FFFF on
        // physical $70000-$7FFFF. A program that remaps first is not handled.
        for (uint16_t i = 0; i < len; i++)
            *coco3_ram_at(&g_c3bus, 0x70000u + (uint16_t)(addr + i)) = src[i];
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        g_m.ram[(uint16_t)(addr + i)] = src[i];
    }
}

extern "C" void coco_machine_jump(uint16_t entry) {
    if (!g_m.cpu) return;
    g_m.cpu->reg_pc = entry;
    // After mc6809_reset() the CPU's state-machine is parked in
    // mc6809_state_reset, so the first cycle would otherwise read the
    // reset vector from $FFFE and overwrite our PC. Skip past that to
    // next_instruction so it picks up reg_pc as-is.
    g_m.cpu->state = mc6809_state_next_instruction;
    // Disable IRQ/FIRQ so the demo isn't whisked into BAS ROM's IRQ
    // handler before it has a chance to set up its own vectors.
    g_m.cpu->reg_cc |= 0x50;  // CC_I (bit 4) | CC_F (bit 6)
}

// AMOLED-32: declared in mc6883.c.
extern "C" int mc6883_mem_cycle(void *sptr, _Bool RnW, uint16_t A);

// PIZERO-209: where the time per emulated cycle goes, from the M33's cycle
// counter (cpu_prof.h). This replaces AMOLED-52's micros() profiler, which
// could not resolve a single cycle. CPU_PROF=2 splits the hook into its
// sections; CPU_PROF=1 leaves the hook untouched.
#if defined(CPU_PROF) && CPU_PROF >= 2
#define PROFILE_MEM_CYCLE 1
#else
#define PROFILE_MEM_CYCLE 0
#endif
#ifdef CPU_PROF
extern "C" { struct cpu_prof g_cpu_prof; }
#endif

// PIZERO-18 audio: event-driven resampler hooks (defined in the audio section).
static inline void audio_update_level(void);
static inline void audio_pia0_written(void);   // PIZERO-253
static inline void audio_pia1_written(void);
// Forced inline (PIZERO-131): with two callers, the hook and the dummy-cycle
// entry, the compiler made it a real function and put it in flash.
static inline __attribute__((always_inline)) void audio_integrate(uint32_t ticks);
// PIZERO-131 step (c): see coco_fastbus.h and the audio section.
static inline __attribute__((always_inline)) void audio_catch_up(void);
static inline __attribute__((always_inline)) void fastbus_deadline(void);
static uint32_t g_budget_end;   // event tick at which this run's budget is spent

// PIZERO-143: the Games Master Cartridge's SN76489. Written from the bus
// (odd $FF40-$FF5F with a banked cart in), rendered at each audio sample in
// audio_integrate, both on core 0, so a write lands between samples exactly
// where the program made it. Idle, and costing nothing, until first written.
static struct sn76489 g_csg;
static bool     g_csg_active = false;
static uint32_t g_csg_last_write = 0;
static uint32_t g_csg_dropped = 0;       // writes inside the chip's ready gap

static void csg_reset(void) {
    sn_reset(&g_csg);
    g_csg_active = false;
}

// PIZERO-149: the AY-3-8910 bank, shared by the CoCo PSG, Symphony-12 and
// Deluxe CoCo front-ends (psg_ay8910.h). Same timing as the SN76489: written
// from the bus, rendered at each audio sample, idle until first written.
static struct ay_bank g_ayb;

// PIZERO-148: the Orchestra-90CC, two 8-bit DACs, left at $FF7A and right at
// $FF7B (write only), as XRoar's orch90.c. They play from the card's own
// jacks, not through the multiplexer. Free addresses on both machines (the
// CoCo 2 decodes nothing at $FF60-$FFBF, the CoCo 3 has no RAM in its I/O
// page), so it can stay on.
static bool g_orch_on = true;
static inline bool orch_owns(uint16_t a) { return g_orch_on && (a & 0xFFFE) == 0xFF7A; }
static void orch_write(uint16_t a, uint8_t d);

// PIZERO-287: the MIDI Pak, a 6850 ACIA at $FF6E (control / status) and
// $FF6F (data), free on both machines like the Orchestra-90. The chip is
// acia6850.h; here it gets the bus, an event at the tick it next needs
// servicing (so received bytes land at MIDI's own rate and raise their
// interrupt on time), and the CART line. Its IRQ pulls CART low, which is
// PIA1 CB1: CoCo MIDI 3 takes it as FIRQ, falling edge. The host side moves
// bytes with coco_machine_midi_tx_pop / _rx_push.
static struct acia6850 g_acia;
static bool g_acia_on = true;
static struct event g_acia_event;
static bool g_acia_event_ready = false;
static bool g_cart_line = true;              // what PIA1 CB1 was last given
static inline bool acia_owns(uint16_t a) { return g_acia_on && (a & 0xFFFE) == 0xFF6E; }
static void acia_after(void);
// Out of line and in flash: the bus functions above run from RAM, and an
// access to $FF6E is rare enough not to need it (inlined, it cost 1.1 KB of RAM).
static uint8_t __attribute__((noinline)) acia_bus_read(uint16_t A);
static void    __attribute__((noinline)) acia_bus_write(uint16_t A, uint8_t D);

// Available all the time by default, not only with a GMC in (see
// csg_on_latch_mirror in cart_gmc.h); the settings page will switch it.
static bool g_csg_always = true;

// A byte from the bus to the chip, dropped if it comes inside the chip's
// 32-clock ready gap, as XRoar does.
static void csg_bus_write(uint8_t d) {
    g_m.csg_writes++;
    uint32_t now = (uint32_t)event_current_tick;
    if (sn_ready(g_csg_last_write, now)) {
        sn_write(&g_csg, d);
        g_csg_last_write = now;
        g_csg_active = true;
    } else {
        g_csg_dropped++;
    }
}

// PIZERO-131: the windows the 6809 core reads and writes itself (see
// coco_fastbus.h). Zero tops until coco_machine_init, so every access goes
// through coco_mem_cycle until RAM and ROM exist. fastbus_sync must follow
// every change to RAM, ROM, the cartridge pointer or the SAM's TY bit.
extern "C" { struct coco_fastbus coco_fastbus; }
static const uint8_t k_open_bus = 0xFF;   // no cartridge: reads float high

static inline void fastbus_sync(void) {
#ifdef COCO_FASTBUS_PAGES
    if (g_c3) {
        // PIZERO-213: map each 8 KB logical page through the GIME's MMU as
        // its decode stands now. coco3_cpu_cycle calls this after every
        // write that can change the map.
#ifdef CPU_PROF
        g_cpu_prof.c3_sync_n++;                      // PIZERO-322
#endif
        g_c3bus.cart = g_m.cart_rom;
        g_c3bus.cart_mask = g_m.cart_mask;
        coco_fastbus.ram_top = coco_fastbus.rom_top = coco_fastbus.cart_top = 0;
        coco_fastbus.fe_rd = coco_fastbus.vec_rd = nullptr;
        coco_fastbus.fe_wr = nullptr;
        if (!g_gime3) { coco_fastbus.page_top = 0; return; }
        for (unsigned p = 0; p < 8; p++) {
            unsigned bank;
            unsigned S = tcc1014_decode_page(g_gime3, p, &bank);
            coco3_bus_page(&g_c3bus, p, S, bank, &coco_fastbus.rd[p], &coco_fastbus.wr[p]);
        }
        coco_fastbus.rom = g_c3bus.rom + 0x4000;     // a dummy cycle reads $FFFF
        coco_fastbus.ticks = coco_fastbus.rom_ticks = (uint16_t)tcc1014_cycle_ticks(g_gime3);
        coco_fastbus.page_top = 0xFE00;              // $FE00 up: MC3 and I/O
        // PIZERO-322: $FE00-$FEFF as tcc1014_mem_cycle decodes it: with MC3
        // the RAM at bank $3F (RAS, whatever the MMU and ROM say), without
        // it the rest of page 7. The vectors read the internal ROM by their
        // logical address, as coco3_bus_cycle's S = 0 does.
        if (tcc1014_mc3(g_gime3)) {
            uint8_t *fe = coco3_ram_at(&g_c3bus, (0x3Fu << 13) | 0x1E00u);
            coco_fastbus.fe_rd = fe;
            coco_fastbus.fe_wr = fe;
        } else {
            coco_fastbus.fe_rd = coco_fastbus.rd[7] ? coco_fastbus.rd[7] + 0x1E00 : nullptr;
            coco_fastbus.fe_wr = coco_fastbus.wr[7] ? coco_fastbus.wr[7] + 0x1E00 : nullptr;
        }
        coco_fastbus.vec_rd = g_c3bus.rom ? g_c3bus.rom + 0x7FE0 : nullptr;
        return;
    }
#endif
    coco_fastbus.ram = g_m.ram;
    coco_fastbus.rom = g_m.rom;
    coco_fastbus.cart = g_m.cart_rom ? g_m.cart_rom : &k_open_bus;
    coco_fastbus.cart_mask = g_m.cart_rom ? g_m.cart_mask : 0;
    if (!g_m.ram || !g_m.rom) {
        coco_fastbus.ram_top = coco_fastbus.rom_top = coco_fastbus.cart_top = 0;
    } else if (g_m.sam_ty) {
        // All-RAM mode: everything below the I/O page is RAM.
        coco_fastbus.ram_top = coco_fastbus.rom_top = coco_fastbus.cart_top = 0xFF00;
    } else {
        coco_fastbus.ram_top  = 0x8000;
        coco_fastbus.rom_top  = 0xC000;
        coco_fastbus.cart_top = 0xFF00;
    }
    // PIZERO-132: what the SAM's rate makes a cycle cost, as sam_ticks says.
    // RAM (all of it below $FF00 in all-RAM mode) is fast only at full speed.
    coco_fastbus.ticks     = (g_m.sam_r & 2) ? 8 : 16;
    coco_fastbus.rom_ticks = g_m.sam_r ? 8 : 16;
#ifdef COCO_FASTBUS_PAGES
    // A CoCo 2 in a paged build: the same windows as pages.
    for (unsigned p = 0; p < 8; p++) {
        uint16_t a = (uint16_t)(p << 13);
        const uint8_t *rd = nullptr;
        uint8_t *wr = nullptr;
        if (a < coco_fastbus.ram_top) rd = wr = coco_fastbus.ram + a;
        // A page has one cost, so at address-dependent speed ROM and
        // cartridge pages take the full hook (PIZERO-132).
        else if (coco_fastbus.rom_ticks != coco_fastbus.ticks) rd = nullptr;
        else if (a < coco_fastbus.rom_top) rd = coco_fastbus.rom + (a & 0x3FFF);
        else if (a < coco_fastbus.cart_top && coco_fastbus.cart_mask >= 0x1FFF)
            rd = coco_fastbus.cart + (a & coco_fastbus.cart_mask);
        coco_fastbus.rd[p] = rd;
        coco_fastbus.wr[p] = wr;
    }
    coco_fastbus.page_top = coco_fastbus.cart_top ? 0xFF00 : 0;
    coco_fastbus.fe_rd = coco_fastbus.vec_rd = nullptr;   // PIZERO-322: CoCo 3 only
    coco_fastbus.fe_wr = nullptr;
#endif
}

// A write to the cartridge's I/O window, $FF40-$FF5F: a bank-switched
// cart's bank register or its SN76489, else the disk controller.
static inline void cart_io_write(uint16_t A, uint8_t D) {
    if (g_m.cart_banked) {
        if (gmc_is_bank_register(A)) {                           // PIZERO-142
            g_m.cart_rom = g_m.cart_base + gmc_bank_offset(D, g_m.cart_len);
            fastbus_sync();                                      // PIZERO-131
        } else csg_bus_write(D);                                 // PIZERO-143
    } else if (g_csg_always && csg_on_latch_mirror(A)) {
        csg_bus_write(D);                    // PIZERO-143: $FF41 with Disk BASIC in
    } else {
        fdc_io_write(A, D);
    }
}

// What every bus cycle does after its data has moved: advance time (and run
// any event now due), integrate the audio, refresh the CPU's interrupt lines
// if a PIA may have changed them, and spend the frame's cycle budget. Shared
// by coco_mem_cycle and the dummy-cycle entry coco_mem_nvma (PIZERO-131).
// pt0 is the profile timestamp, unused unless CPU_PROF=2.
static inline __attribute__((always_inline)) void mem_cycle_tail(int ncycles, uint32_t pt0) {
    (void)pt0;
    // AMOLED-30 fast path: ~96% of memory cycles have no event due, so
    // skip the function-call overhead and just advance the tick counter.
    // Only enter event_run_queue when something is actually pending.
    event_ticks new_tick = event_current_tick + ncycles;
    struct event *head = machine_event_list_global->events;
    if (head && event_tick_delta(new_tick, head->at_tick) >= 0) {
#ifdef CPU_PROF
        uint32_t et0 = cpu_prof_now();
        event_run_queue(machine_event_list_global, ncycles);
        g_cpu_prof.ev_clk += cpu_prof_now() - et0;
        g_cpu_prof.ev_n++;
#else
        event_run_queue(machine_event_list_global, ncycles);
#endif
        // VDG event callbacks may toggle PIA CB1 inputs; assume any actual
        // dispatch could have shifted PIA IRQ outputs.
        g_m.pia_irq_dirty = true;
    } else {
        event_current_tick = new_tick;
    }

    // PIZERO-18: integrate the DAC level over this access and emit 32 kHz
    // samples on tick boundaries (band-limited resampling at the source).
    audio_integrate((uint32_t)ncycles);

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = cpu_prof_now(); g_cpu_prof.sec_clk[2] += pt1 - pt0; pt0 = pt1; }
#endif

    if (g_c3) {
        // PIZERO-133: the GIME's own IRQ and FIRQ join the PIAs'. They change
        // in its events and on register accesses, so refresh every cycle.
        MC6809_IRQ_SET(g_m.cpu,  g_m.pia0->a.irq || g_m.pia0->b.irq || g_gime3->IRQ);
        MC6809_FIRQ_SET(g_m.cpu, g_m.pia1->a.irq || g_m.pia1->b.irq || g_gime3->FIRQ);
        g_m.pia_irq_dirty = false;
    } else if (g_m.pia_irq_dirty) {
#ifdef GIME_TIMER
        // PIZERO-62: the timer shares the 6809's two lines with the PIAs, as
        // it does on a CoCo 3. An unacknowledged status holds its line high.
        uint8_t gl = gime_timer_lines(&g_gime);
        MC6809_IRQ_SET(g_m.cpu,  g_m.pia0->a.irq || g_m.pia0->b.irq || (gl & GIME_LINE_IRQ));
        MC6809_FIRQ_SET(g_m.cpu, g_m.pia1->a.irq || g_m.pia1->b.irq || (gl & GIME_LINE_FIRQ));
#else
        MC6809_IRQ_SET(g_m.cpu,  g_m.pia0->a.irq || g_m.pia0->b.irq);
        MC6809_FIRQ_SET(g_m.cpu, g_m.pia1->a.irq || g_m.pia1->b.irq);
#endif
        g_m.pia_irq_dirty = false;
    }

    // The budget is spent once time reaches its end (PIZERO-131: by tick,
    // so cycles that skip the tail still count against it).
    if (event_tick_delta(event_current_tick, g_budget_end) >= 0) g_m.cpu->running = 0;

    fastbus_deadline();

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = cpu_prof_now(); g_cpu_prof.sec_clk[3] += pt1 - pt0; }
#endif
}

// PIZERO-133: a CoCo 3 cycle goes through the GIME, which calls back
// coco3_cpu_cycle. PIZERO-220: timed in the profiler build.
static inline __attribute__((always_inline)) void c3_slow(_Bool RnW, uint16_t A) {
#ifdef CPU_PROF
    {   // PIZERO-322: which addresses take the slow path
        unsigned c;
        if (A < 0xFE00) { c = RnW ? CPU_PROF_C3_RD : CPU_PROF_C3_WR; g_cpu_prof.c3_page[A >> 13]++; }
        else if (A < 0xFF00) c = CPU_PROF_C3_FE;
        else if (A < 0xFF40) c = CPU_PROF_C3_PIA;
        else if (A < 0xFF90) c = CPU_PROF_C3_CART;
        else if (A < 0xFFA0) c = CPU_PROF_C3_GIME;
        else if (A < 0xFFB0) c = CPU_PROF_C3_MMU;
        else if (A < 0xFFC0) c = CPU_PROF_C3_PAL;
        else if (A < 0xFFE0) c = CPU_PROF_C3_SAM;
        else if (A < 0xFFFF) c = CPU_PROF_C3_VEC;
        else c = CPU_PROF_C3_FFFF;
        g_cpu_prof.c3_cls[c]++;
    }
    uint32_t t0 = cpu_prof_now();
    tcc1014_mem_cycle(g_gime3, RnW, A);
    g_cpu_prof.slow_clk += cpu_prof_now() - t0;
    g_cpu_prof.slow_n++;
#else
    tcc1014_mem_cycle(g_gime3, RnW, A);
#endif
}

// PIZERO-132: a CoCo 2 bus cycle's length in event ticks, from the SAM's
// MPU rate R (mc6883.c's fast_cycle): 8 when fast, 16 when slow. Full speed
// (R1 set, POKE 65497) makes every cycle fast; address-dependent speed
// (R = 1, POKE 65495) all but RAM and PIA0 ($FF00-$FF1F). The real SAM's
// 15, 17 and 25 tick cycles where the rate changes are not modelled: they
// even out to within a few percent.
static inline __attribute__((always_inline)) int sam_ticks(bool ram_or_pia0) {
    return ((g_m.sam_r & 2) || (g_m.sam_r && !ram_or_pia0)) ? 8 : 16;
}

extern "C" void HOT_FUNC(coco_mem_cycle)(void *sptr, _Bool RnW, uint16_t A) {
    (void)sptr;
    if (g_c3) { c3_slow(RnW, A); return; }   // PIZERO-133
    audio_catch_up();     // before a PIA1 write can change the level
    g_m.total_mem_cycles++;

#if PROFILE_MEM_CYCLE
    uint32_t pt0 = cpu_prof_now();
#endif

    // AMOLED-32 fast-path SAM decode. The generic mc6883_mem_cycle handles
    // Dragon32/64/CoCo1/2/3/MC10 + the '785 variant + fast/slow cycle state
    // machine. We're CoCo 2 only and don't care about cycle-accurate fast
    // mode: flat 16-tick slow cycle per access matches default speed.
    // PIZERO-132: the SAM's rate is honoured since (sam_ticks).
    //
    // AMOLED-53 branch order: the overwhelming common case is A < 0xFF00
    // (RAM / ROM / cart ROM, i.e. every instruction fetch and almost every
    // data access). Putting that first keeps the branch predictor happy.
    // SAM register writes ($FFC0-$FFDF) are rare but have full side effects
    // (V/F/M/P/R/TY + vdg_update delegate); punt to xroar.
    int ncycles;
    unsigned S;
    if (A < 0xFF00) {
        // Bulk of memory: RAM / ROM / cart ROM. With TY=1 (all-RAM mode)
        // everything below FF00 is RAM.
        bool ram = !(A & 0x8000) || g_m.sam_ty;
        if (ram)                          S = RnW ? 0 : 7;
        else if (A < 0xA000)              S = RnW ? 1 : 7;
        else if (A < 0xC000)              S = RnW ? 2 : 7;
        else                              S = RnW ? 3 : 7;
        g_m.sam->S = S;
        ncycles = sam_ticks(ram);
    } else if ((A & 0xFFE0) == 0xFFC0 && !RnW) {
        // SAM register write: slow path. Its length is set by the rate
        // before the write, as on the SAM.
        ncycles = sam_ticks(false);
        mc6883_mem_cycle(g_m.sam, RnW, A);
        S = g_m.sam->S;
        // PIZERO-132: shadow the rate, R0 at $FFD6/7 and R1 at $FFD8/9.
        if (A >= 0xFFD6 && A <= 0xFFD9) {
            unsigned bit = (A - 0xFFD6) >> 1;
            if (A & 1) g_m.sam_r |=  (uint8_t)(1u << bit);
            else       g_m.sam_r &= (uint8_t)~(1u << bit);
            fastbus_sync();
        }
        // Shadow the TY bit for our fast decode.
        if (A == 0xFFDE) { g_m.sam_ty = false; fastbus_sync(); }
        else if (A == 0xFFDF) { g_m.sam_ty = true; fastbus_sync(); }
        // PIZERO-140: shadow the V register (VDG address mode, $FFC0-$FFC5):
        // it, not the VDG mode, decides row stride and line repeats.
        if (A <= 0xFFC5) {
            unsigned bit = (A - 0xFFC0) >> 1;
            if (A & 1) g_m.sam_v |=  (uint8_t)(1u << bit);
            else       g_m.sam_v &= (uint8_t)~(1u << bit);
        }
        // Shadow the F register (display address base) for coco_vdg_fetch.
        // $FFC6/C7 → F bit 9, $FFC8/C9 → bit 10, …, $FFD2/D3 → bit 15.
        if (A >= 0xFFC6 && A <= 0xFFD3) {
            unsigned bit_idx = ((A >> 1) & 0xf) + 6;
            if (A & 1) g_m.sam_f |=  (1u << bit_idx);
            else       g_m.sam_f &= ~(1u << bit_idx);
        }
    } else {
        // FF00-FFFF I/O area. Map by 32-byte windows.
        unsigned io = (A >> 5) & 7;
        switch (io) {
            case 0: S = 4; break;  // PIA0
            case 1: S = 5; break;  // PIA1
            case 2: S = 6; break;  // cart I/O (FDC)
            case 7: S = 2; break;  // FFE0-FFFF: vectors in BAS ROM
            default: S = 7; break; // FF60-FFBF: open bus
        }
        g_m.sam->S = S;
        ncycles = sam_ticks(io == 0);
    }

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = cpu_prof_now(); g_cpu_prof.sec_clk[0] += pt1 - pt0; pt0 = pt1; }
#endif

    // AMOLED-53 IRQ-dirty tightening: per the 6821 datasheet only writes
    // to CRA/CRB (A0=1) and reads of PADR/PBDR (A0=0) can change the IRQ
    // output. The other two combinations (CRA/CRB read, PADR/PBDR write)
    // cannot: skip the dirty mark in those cases so the IRQ-OR + macro
    // dispatch at the bottom runs less often.
    //
    // Now use SAM->S (set by mem_cycle) to route data on the bus.
    if (RnW && ayb_owns(&g_ayb, A)) {
        g_m.cpu->D = ayb_read(&g_ayb, A);                // PIZERO-149
    } else if (RnW && acia_owns(A)) {
        g_m.cpu->D = acia_bus_read(A);                   // PIZERO-287
    } else if (RnW) {
        switch (g_m.sam->S) {
        case 0:  g_m.cpu->D = g_m.ram[A]; break;
        case 1:
        case 2:  g_m.cpu->D = g_m.rom[A & 0x3FFF]; break;
        case 3:  // Cartridge ROM, $C000-$FEFF. A cart smaller than the window
                 // repeats through it, as real ones do (PIZERO-139).
            g_m.cpu->D = g_m.cart_rom ? g_m.cart_rom[A & g_m.cart_mask] : 0xFF;
            break;
        case 4:  g_m.cpu->D = mc6821_read(g_m.pia0, A);
                 if ((A & 1) == 0) g_m.pia_irq_dirty = true;  // PADR/PBDR read may clear IRQ
                 break;
        case 5:  g_m.cpu->D = mc6821_read(g_m.pia1, A);
                 if ((A & 1) == 0) g_m.pia_irq_dirty = true;
                 break;
        case 6:  g_m.cpu->D = g_m.cart_banked ? 0xFF : fdc_io_read(A); break;
        default:
#ifdef GIME_PALETTE
                 if (g_gime_pal_on && coco_pal_owns(A)) { g_m.cpu->D = coco_pal_read(&g_pal, A); break; }
#endif
#ifdef GIME_TIMER
                 { uint8_t gv;
                   if (g_gime_timer_on && gime_timer_owns(A) && gime_timer_read(&g_gime, A, &gv)) {
                       g_m.cpu->D = gv;
                       // Reading the enable registers acknowledges, which can
                       // drop the line.
                       g_m.pia_irq_dirty = true;
                       break;
                   } }
#endif
                 g_m.cpu->D = 0xFF; break;
        }
    } else {
        // Real CoCo writes route by raw address: S=7 covers SAM regs +
        // any RAM-region write (RAS/nWE handles RAM in parallel).
#ifdef FDC_TRACE
        // PIZERO-190: GIME and SAM writes, which a CoCo 3 build makes.
        { static unsigned n = 0;
          if (A >= 0xFF90 && A < 0xFFE0 && n < 200) {
              n++;
              FDC_TRACE_PORT.printf("[fdc] io %04x=%02x pc=%04x\r\n", A, g_m.cpu->D, g_m.cpu->reg_pc);
          }
          // PIZERO-191: PIA0 control writes ($FF01/$FF03 and mirrors).
          static unsigned m = 0;
          if ((A & 0xFFE1) == 0xFF01 && m < 60) {
              m++;
              FDC_TRACE_PORT.printf("[fdc] pia0 %04x=%02x pc=%04x\r\n", A, g_m.cpu->D, g_m.cpu->reg_pc);
          } }
#endif
        if (ayb_owns(&g_ayb, A)) {
            ayb_write(&g_ayb, A, g_m.cpu->D);           // PIZERO-149
        } else if (orch_owns(A)) {
            orch_write(A, g_m.cpu->D);                  // PIZERO-148
        } else if (acia_owns(A)) {
            acia_bus_write(A, g_m.cpu->D);              // PIZERO-287
        } else if ((A & 0xFFE0) == 0xFF00) {
            mc6821_write(g_m.pia0, A, g_m.cpu->D);
            if (A & 1) g_m.pia_irq_dirty = true;       // CRA/CRB write may change IRQ enable
            audio_pia0_written();                      // PIZERO-253: the sound source
        } else if ((A & 0xFFE0) == 0xFF20) {
            mc6821_write(g_m.pia1, A, g_m.cpu->D);
            if (A & 1) g_m.pia_irq_dirty = true;
            audio_pia1_written();   // DAC / single-bit / enable may have changed (PIZERO-253)
        }
#ifdef GIME_PALETTE
        else if (g_gime_pal_on && coco_pal_owns(A)) coco_pal_write(&g_pal, A, g_m.cpu->D);
#endif
#ifdef GIME_TIMER
        else if (g_gime_timer_on && gime_timer_owns(A)) {
            if (gime_timer_write(&g_gime, A, g_m.cpu->D)) gime_timer_restart();
            g_m.pia_irq_dirty = true;
        }
#endif
        else if ((A & 0xFFE0) == 0xFF40) cart_io_write(A, g_m.cpu->D);   // cart I/O
        else if (A < 0x8000)             g_m.ram[A] = g_m.cpu->D;
        else if (g_m.sam_ty && A < 0xFF00) g_m.ram[A] = g_m.cpu->D;  // all-RAM mode
        // ROM and other I/O writes ignored
    }

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = cpu_prof_now(); g_cpu_prof.sec_clk[1] += pt1 - pt0; pt0 = pt1; }
#else
    uint32_t pt0 = 0;
#endif
    mem_cycle_tail(ncycles, pt0);
}

#ifdef COCO3
// PIZERO-220: a CoCo 3 RAM or ROM cycle the core has already done, at or
// past the deadline: what coco3_cpu_cycle does after the GIME's decode,
// less the bus (no PIA, cartridge I/O or remapping write gets here).
static inline __attribute__((always_inline)) void coco3_tick(void) {
#ifdef CPU_PROF
    uint32_t t0 = cpu_prof_now();
#endif
    tcc1014_poll_lines(g_gime3);
    audio_catch_up();
    g_m.total_mem_cycles++;
    g_gime3->IL1 = (PIA_VALUE_A(g_m.pia0) | 0x80) != 0xff;
    mem_cycle_tail(coco_fastbus.ticks, 0);
#ifdef CPU_PROF
    g_cpu_prof.slow_clk += cpu_prof_now() - t0;
    g_cpu_prof.slow_n++;
    g_cpu_prof.c3_tick_n++;               // PIZERO-322
#endif
}
#else
static inline void coco3_tick(void) {}
#endif

// PIZERO-131: a RAM or ROM cycle the core has already done itself
// (coco_fastbus.h): only the per-cycle tail is left.
extern "C" void HOT_FUNC(coco_mem_tick)(void *sptr, _Bool RnW, uint16_t A) {
    (void)sptr; (void)RnW; (void)A;
    // PIZERO-220: on a CoCo 3 the core has already moved the byte, so of the
    // GIME's cycle only its interrupt edge detect is left (PIZERO-213 ran the
    // whole cycle through it again, decode and bus included).
    if (g_c3) { coco3_tick(); return; }
    audio_catch_up();
    g_m.total_mem_cycles++;
#if PROFILE_MEM_CYCLE
    uint32_t pt0 = cpu_prof_now();
#else
    uint32_t pt0 = 0;
#endif
    // PIZERO-132: the window the core used decides the length.
    mem_cycle_tail(A < coco_fastbus.ram_top ? coco_fastbus.ticks : coco_fastbus.rom_ticks, pt0);
}

// PIZERO-131: a dummy cycle (VMA low, address $FFFF). The full decode always
// lands on the same answer, the vector window of BASIC ROM, so set what it
// would set (S = 2, D = the ROM byte at $FFFF) and go straight to the
// per-cycle tail: about a third of all cycles skip the decode and the data
// switch. The CPU's interrupt latches are still sampled by the caller.
extern "C" void HOT_FUNC(coco_mem_nvma)(void *sptr) {
    // PIZERO-220: a CoCo 3 dummy cycle reads what the fast path's does.
    if (g_c3) { g_m.cpu->D = coco_fastbus.rom[0x3FFF]; coco3_tick(); return; }
    (void)sptr;
    audio_catch_up();
    g_m.total_mem_cycles++;
    g_m.sam->S = 2;
    g_m.cpu->D = g_m.rom[0xFFFF & 0x3FFF];
#if PROFILE_MEM_CYCLE
    uint32_t pt0 = cpu_prof_now();
#else
    uint32_t pt0 = 0;
#endif
    mem_cycle_tail(coco_fastbus.rom_ticks, pt0);        // PIZERO-132
}

// VDG sync signals on the real CoCo go to PIA0 control inputs:
//   FS (frame sync, ~60 Hz) -> PIA0 CB1 -> generates IRQ for BASIC timer
//   HS (horizontal sync)    -> PIA0 CA1
extern "C" void HOT_FUNC(coco_vdg_signal_fs)(void *sptr, _Bool level) {
    (void)sptr;
    // Drive the SAM raster counter as well as PIA0 CB1. Without the SAM
    // FS pulse the V-counter never resets, so the VDG fetches the wrong
    // window of video RAM.
    if (g_m.sam)  g_m.sam->vdg_fsync(g_m.sam, level);
    if (g_m.pia0) mc6821_set_cx1(&g_m.pia0->b, level);
}

extern "C" void HOT_FUNC(coco_vdg_signal_hs)(void *sptr, _Bool level) {
    (void)sptr;
    // SAM also needs HS to advance the row counter every scanline.
    if (g_m.sam)  g_m.sam->vdg_hsync(g_m.sam, level);
    if (g_m.pia0) mc6821_set_cx1(&g_m.pia0->a, level);
}

// - - - VDG callbacks ---------------------------------------------

extern "C" void HOT_FUNC(coco_vdg_fetch)(void *sptr, uint16_t A, int nwords,
                               uint16_t *dest) {
    (void)sptr;
    // AMOLED-26: use the SAM's F register (display base) we shadow on
    // every $FFC6-$FFD3 write. Falls back to $0400 if F is uninitialised
    // (no write has happened), matches DECB text-mode default.
    const uint16_t base = g_m.sam_f ? g_m.sam_f : 0x0400;
    for (int i = 0; i < nwords; i++) {
        uint16_t a = (base + A + i) & 0xFFFF;
        uint8_t v = g_m.ram[a];
        uint16_t D = (uint16_t)v;
        D |= (uint16_t)((v & 0xC0) << 2);
        dest[i] = D;
    }
}

extern "C" void HOT_FUNC(coco_vdg_render)(void *sptr, unsigned burst, unsigned npixels,
                                          const uint8_t *data) {
    (void)sptr; (void)burst; (void)npixels;
#ifdef SUPPRESS_RENDER_SCANLINE
    // vdg_buffer is produced whole-frame by coco_machine_render_frame()
    // instead of per-scanline here. xroar's render_scanline is suppressed,
    // so the pixel_data this callback would pack is stale: skip it entirely.
    //
    // NOTE (PIZERO-83, 2026-08-17): the original AMOLED-57 comment said
    // "core 1 owns vdg_buffer now". That is NOT true on this port. Here
    // core 1 runs only libdvi scanout (core1_main, src/main.cpp:200, which
    // never returns); coco_machine_render_frame is called from core 0's
    // loop() in the WP_RENDER phase. There is no cross-core race to avoid:
    // the buffer has a single writer on core 0.
    (void)data;
    return;
#endif
    // render_line fires once per scanline including borders. Wrap to the
    // VDG's per-frame scanline count so each new frame starts re-capturing
    // active rows 0..191, without the wrap, the row index races off into
    // the millions and every call_idx falls outside the active window.
    int scanline = g_m.render_lines++ % VDG_FRAME_DURATION;
    if (!data) return;

    int row = scanline - VDG_ACTIVE_AREA_START;
    if (row < 0 || row >= COCO_VDG_H) return;

    const uint8_t *p = data + VDG_ACTIVE_LINE_START;
    uint8_t *dst = &g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
    for (int x = 0; x < COCO_VDG_W; x += 2) {
        uint8_t lo = p[x * 2] & 0x0F;
        uint8_t hi = p[(x + 1) * 2] & 0x0F;
        dst[x >> 1] = lo | (hi << 4);
    }
}

// - - - CoCo 3 (PIZERO-133) -----------------------------------------
#ifdef COCO3

// The GIME's call-back for every cycle, after its decode: move the byte,
// give the PIAs and the cartridge I/O their side effects as the CoCo 2 path
// does, then the shared per-cycle tail. Upstream's equivalent is cpu_cycle
// in src/coco3.c.
static void HOT_FUNC(coco3_cpu_cycle)(void *sptr, int ncycles, bool RnW, uint16_t A) {
    (void)sptr;
#ifdef CPU_PROF
    uint32_t cb0 = cpu_prof_now();
#endif
    audio_catch_up();     // before a PIA1 write can change the level
    g_m.total_mem_cycles++;
    uint8_t *D = &g_m.cpu->D;
    // PIZERO-149: the AY cartridges' registers. All are in the I/O page,
    // which has no RAM behind it, so the bus has nothing else to move.
    if (ayb_owns(&g_ayb, A)) {
        if (RnW) *D = ayb_read(&g_ayb, A);
        else     ayb_write(&g_ayb, A, *D);
    } else if (!RnW && orch_owns(A)) {
        orch_write(A, *D);                              // PIZERO-148
    } else if (acia_owns(A)) {                          // PIZERO-287
        if (RnW) *D = acia_bus_read(A);
        else     acia_bus_write(A, *D);
    } else
    switch (coco3_bus_cycle(&g_c3bus, g_gime3->S, g_gime3->RAS, g_gime3->Z, RnW, A, D)) {
    case C3_BUS_PIA0:
        if (RnW) *D = mc6821_read(g_m.pia0, A);
        else   { mc6821_write(g_m.pia0, A, *D); audio_pia0_written(); }   // PIZERO-253
        break;
    case C3_BUS_PIA1:
        if (RnW) *D = mc6821_read(g_m.pia1, A);
        else   { mc6821_write(g_m.pia1, A, *D); audio_pia1_written(); }
        break;
    case C3_BUS_CARTIO:
        if (RnW) *D = g_m.cart_banked ? 0xFF : fdc_io_read(A);
        else     cart_io_write(A, *D);
        break;
    default:
        break;
    }
    // PIZERO-213: a write that can move the MMU map (the registers at
    // $FFA0-$FFAF, INIT0/INIT1 at $FF90/$FF91, the SAM subset) remaps the
    // fast path's pages. The GIME has already taken it.
    if (!RnW && ((A & 0xFFFE) == 0xFF90 || (A & 0xFFF0) == 0xFFA0 || (A & 0xFFE0) == 0xFFC0))
        fastbus_sync();
    // The keyboard interrupt: any row pulled low (upstream does this every
    // cycle; the GIME edge-detects it at the start of the next). PIZERO-213:
    // now only on the cycles that take the full path or the tail, at least
    // once per event (PIZERO-220: every scanline).
    g_gime3->IL1 = (PIA_VALUE_A(g_m.pia0) | 0x80) != 0xff;
#ifdef CPU_PROF
    uint32_t cb1 = cpu_prof_now();
    mem_cycle_tail(ncycles, 0);
    g_cpu_prof.cb_bus_clk += cb1 - cb0;
    g_cpu_prof.cb_tail_clk += cpu_prof_now() - cb1;
#else
    mem_cycle_tail(ncycles, 0);
#endif
}

static uint16_t HOT_FUNC(coco3_fetch_vram)(void *sptr, uint32_t A) {
    (void)sptr;
    return coco3_bus_fetch_vram(&g_c3bus, A);
}
static void coco3_signal_hs(void *sptr, bool level) {
    (void)sptr;
    mc6821_set_cx1(&g_m.pia0->a, level);
}
static void coco3_signal_fs(void *sptr, bool level) {
    (void)sptr;
    mc6821_set_cx1(&g_m.pia0->b, level);
    coco3_bus_frame_sync(&g_c3bus, level);
}
static void HOT_FUNC(coco3_render_line)(void *sptr, unsigned burst, unsigned npixels,
                                        const uint8_t *data) {
    (void)sptr; (void)burst;
#ifdef CPU_PROF
    uint32_t lt0 = cpu_prof_now();
    coco3_bus_render_line(&g_c3bus, data, npixels);
    g_cpu_prof.line_clk += cpu_prof_now() - lt0;
    g_cpu_prof.line_n += data != nullptr;
#else
    coco3_bus_render_line(&g_c3bus, data, npixels);
#endif
}
static void coco3_set_active_area(void *sptr, int x, int y, int w, int h) {
    (void)sptr;
    // PIZERO-218: lines land on other rows now; draw them all again.
    if (coco3_bus_set_active_area(&g_c3bus, x, y, w, h)) coco_machine_video_invalidate();
}

// Where the CoCo 3 draws: the framebuffer core 1 scans out (coco3_pix_t,
// PIZERO-134). The GIME writes each scanline as it finishes, so nothing is
// rendered or blitted between frames.
extern "C" void coco_machine_set_framebuffer(void *fb) {
    g_c3bus.fb = (coco3_pix_t *)fb;
    coco_machine_video_invalidate();
}

// PIZERO-213: something else drew over the picture (the overlay); every row
// is drawn again as its scanline comes round.
extern "C" void coco_machine_video_invalidate(void) {
    coco3_bus_invalidate(&g_c3bus);
    if (g_gime3) g_gime3->port_redraw = true;        // PIZERO-218: it skips unchanged lines too
}

extern "C" _Bool coco_machine_is_coco3(void) { return g_c3; }

#ifdef COCO3
// PIZERO-319: one 64 KB block, taken from the heap at the first machine
// and kept for good, so switching machine never needs the heap to have
// 64 KB in one piece (a booted CoCo 2 leaves it split). The CoCo 3 uses it
// as its second 64 KB; a CoCo 2 keeps its VDG buffer at the start and lends
// the rest to a bank-switched cartridge (coco_machine_cart_room).
#define ARENA_BYTES (64u * 1024u)
static uint8_t *g_arena = nullptr;
static uint8_t *arena(void) {
    if (!g_arena) g_arena = (uint8_t *)malloc(ARENA_BYTES);
    return g_arena;
}
extern "C" uint8_t *coco_machine_cart_room(uint32_t *len) {
    *len = 0;
    if (g_c3 || !g_arena || !g_m.cpu) return nullptr;
    *len = ARENA_BYTES - COCO_VDG_BUF_BYTES;
    return g_arena + COCO_VDG_BUF_BYTES;
}
#endif

static _Bool coco3_init(const uint8_t *rom) {
    g_m.rom = rom;
    g_m.rom_len = COCO3_ROM_SIZE;
    // 128 KB: the 64 KB pool the CoCo 2 uses, and 64 KB more from the heap.
    uint8_t *lo = (uint8_t *)rp_mem_malloc(64 * 1024);
    uint8_t *hi = arena();                           // PIZERO-319
    if (!lo || !hi) return 0;
    memset(lo, 0, 64 * 1024);
    memset(hi, 0, 64 * 1024);
    g_m.ram = lo;
    coco3_bus_init(&g_c3bus, lo, hi, rom, nullptr);
    g_c3 = true;
    fastbus_sync();

    machine_event_list_global = event_list_new();
    g_acia_event_ready = false;                  // PIZERO-287: its event was on the old list
    acia_init(&g_acia); g_cart_line = true;
    if (!machine_event_list_global) return 0;
    event_current_tick = 0;

    struct part *p = part_create("MC6809", NULL);
    if (!p) return 0;
    g_m.cpu = (struct MC6809 *)p;

    p = part_create("MC6821", NULL);
    if (!p) return 0;
    g_m.pia0 = (struct MC6821 *)p;
    mc6821_reset(g_m.pia0);
    g_m.pia0->a.data_preread = DELEGATE_AS0(void, coco_pia0_preread_a, NULL);

    p = part_create("MC6821", NULL);
    if (!p) return 0;
    g_m.pia1 = (struct MC6821 *)p;
    mc6821_reset(g_m.pia1);

    // As upstream's coco3_finish: nothing driving the ports, and the serial
    // input (PIA1 PB0) held low.
    g_m.pia0->b.in_source = 0;
    g_m.pia1->b.in_source = 0;
    g_m.pia0->a.in_sink = g_m.pia0->b.in_sink = 0xFF;
    g_m.pia1->a.in_sink = g_m.pia1->b.in_sink = 0xFF;
    g_m.pia1->b.in_sink &= (uint8_t)~(1u << 0);

    p = part_create("TCC1014-1987", NULL);
    if (!p) return 0;
    g_gime3 = (struct TCC1014 *)p;
    g_gime3->cpu_cycle = DELEGATE_AS3(void, int, bool, uint16, coco3_cpu_cycle, NULL);
    g_gime3->fetch_vram = DELEGATE_AS1(uint16, uint32, coco3_fetch_vram, NULL);
    g_gime3->signal_hs = DELEGATE_AS1(void, bool, coco3_signal_hs, NULL);
    g_gime3->signal_fs = DELEGATE_AS1(void, bool, coco3_signal_fs, NULL);
    g_gime3->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp, coco3_render_line, NULL);
    g_gime3->set_active_area = DELEGATE_AS4(void, int, int, int, int, coco3_set_active_area, NULL);
    g_gime3->CPUD = &g_m.cpu->D;
    g_gime3->port_render = true;                     // PIZERO-218: TCC1014_PORT_W pixels a line
    g_gime3->port_vram[0] = lo;
    g_gime3->port_vram[1] = hi;
    tcc1014_notify_mode(g_gime3);
    tcc1014_reset(g_gime3);
    fastbus_sync();                                  // PIZERO-213: the GIME's map
    g_m.fdc_intrq = false;                       // PIZERO-320: no NMI left over
    g_m.cpu->reset(g_m.cpu);
    return 1;
}
#else
extern "C" void coco_machine_set_framebuffer(void *fb) { (void)fb; }
extern "C" void coco_machine_video_invalidate(void) {}
extern "C" _Bool coco_machine_is_coco3(void) { return 0; }
extern "C" uint8_t *coco_machine_cart_room(uint32_t *len) { *len = 0; return nullptr; }   // PIZERO-319
#endif

// - - - init / run ------------------------------------------------

extern "C" _Bool coco_machine_init(const uint8_t *rom, size_t rom_len) {
#ifdef COCO3
    if (rom_len == COCO3_ROM_SIZE) return coco3_init(rom);    // PIZERO-133
#endif
    if (rom_len != 16384) return 0;
    g_m.rom = rom;
    g_m.rom_len = rom_len;

    g_m.ram = (uint8_t *)rp_mem_malloc(64 * 1024);
    if (!g_m.ram) return 0;
    memset(g_m.ram, 0, 64 * 1024);
#ifdef COCO3
    g_m.vdg_buffer = arena();                 // PIZERO-319: the start of the 64 KB block
    if (!g_m.vdg_buffer) return 0;
#endif
    memset(g_m.vdg_buffer, 0, COCO_VDG_BUF_BYTES);
    fastbus_sync();

    // Event list MUST exist before any part_create() that uses MACHINE_EVENT_LIST.
    machine_event_list_global = event_list_new();
    g_acia_event_ready = false;                  // PIZERO-287: its event was on the old list
    acia_init(&g_acia); g_cart_line = true;
    if (!machine_event_list_global) return 0;
    event_current_tick = 0;

    struct part *p;

    p = part_create("MC6809", NULL);
    if (!p) return 0;
    g_m.cpu = (struct MC6809 *)p;
    g_m.cpu->mem_cycle = DELEGATE_AS2(void, bool, uint16, coco_mem_cycle, g_m.cpu);
#ifdef FDC_TRACE
    g_m.cpu->debug_cpu.instruction_hook = DELEGATE_AS0(void, pc_trace_hook, g_m.cpu);
#endif

    p = part_create("SN74LS783", NULL);
    if (!p) return 0;
    g_m.sam = (struct MC6883 *)p;
    g_m.sam->reset(g_m.sam);

    p = part_create("MC6821", NULL);
    if (!p) return 0;
    g_m.pia0 = (struct MC6821 *)p;
    mc6821_reset(g_m.pia0);
    g_m.pia0->b.in_source = 0xFF;
    g_m.pia0->a.in_source = 0xFF;
    g_m.pia0->a.in_sink = 0xFF;
    g_m.pia0->a.data_preread = DELEGATE_AS0(void, coco_pia0_preread_a, NULL);

    p = part_create("MC6821", NULL);
    if (!p) return 0;
    g_m.pia1 = (struct MC6821 *)p;
    mc6821_reset(g_m.pia1);
    g_m.pia1->b.in_source = 0xFF;
    // AMOLED-26: VDG mode bits (¬A/G, GM2, GM1, GM0, CSS) live on PIA1
    // port B output. Hook the postwrite so VDG follows mode changes.
    g_m.pia1->b.data_postwrite = DELEGATE_AS0(void, coco_pia1b_postwrite, NULL);

    // AMOLED-38: select 6847T1 variant. CoCo 2 late boards (and most
    // emulator targets) ship the T1, it adds lowercase via the inverse
    // bit, mildly different ALPHA-mode border colour, and a few
    // text-mode features. With is_t1=true the non-T1 font path in
    // mc6847.c is dead at runtime, letting us drop font-6847.c.
    p = part_create("MC6847", (void *)"6847T1");
    if (!p) return 0;
    g_m.vdg = (struct MC6847 *)p;
    g_m.vdg->fetch_data  = DELEGATE_AS3(void, uint16, int, uint16p,
                                        coco_vdg_fetch, g_m.vdg);
    g_m.vdg->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp,
                                        coco_vdg_render, g_m.vdg);
    g_m.vdg->signal_fs   = DELEGATE_AS1(void, bool, coco_vdg_signal_fs, g_m.vdg);
    g_m.vdg->signal_hs   = DELEGATE_AS1(void, bool, coco_vdg_signal_hs, g_m.vdg);
    mc6847_reset(g_m.vdg);
    mc6847_set_mode(g_m.vdg, 0);
    mc6847_unpause(g_m.vdg);

    return 1;
}

// PIZERO-166: the original 6847 font (font-6847.c) is now built for the
// font = classic setting; it used to be a 1-byte link stub here.

// - - - audio (PIZERO-18) -----------------------------------------
//
// This stripped port vendors no XRoar sound module, so we synthesise the CoCo
// audio stream here. The 6-bit DAC sits on PIA1 port A bits 2..7 and the
// single-bit sound on PIA1 port B bit 1. We sample the effective pin state at
// a fixed COCO_AUDIO_RATE by slicing the CPU run into sample-sized cycle
// chunks (below), which yields correct PITCH because emulation time advances
// by exactly the right number of cycles between samples. Output lands in a
// small overwrite ring drained by whatever sink is active (WAV-over-CDC today;
// DVI data islands later). CPU clock = EVENT_TICK_RATE/16 = 14318180/16 =
// 894886.25 Hz; the Bresenham uses the rates x4 so the divisor is exact and
// pitch does not drift.
#define COCO_AUDIO_RATE      48000u   // PIZERO-30: 48 kHz native (ACR CTS now correct)
#define COCO_AUDIO_CPUCLK_X4 3579545u                 // (14318180/16)*4, exact
#define COCO_AUDIO_RATE_X4   (COCO_AUDIO_RATE * 4u)    // 128000
// PIZERO-39/38: deepened to absorb the bursty producer (a whole emulated frame
// of ~924 samples is pushed during coco_machine_run_cycles, then drained steadily
// by the core-1 IRQ). 8192 samples = ~170 ms = 16 KB (cheap now the streaming
// build freed ~126 KB). Too-shallow caused overflow-skips -> pitch up + distortion.
// PIZERO-250: a sample is a left and right pair now, for the Symphony-12 in
// stereo. Everything else puts the same value on both sides.
// PIZERO-262: 4096 pairs (16 KB, as the mono ring was). At 8192 pairs the
// ring's 32 KB came out of the heap the CoCo 3 needs for its second 64 KB,
// and it no longer started. Half full is about 43 ms, over five frames'
// bursts; the servo (PIZERO-121) aims at half of whatever the size is.
#define AUDIO_RING_SAMPLES   4096u                     // pow2

static int16_t           g_audio_ring[AUDIO_RING_SAMPLES][2];
static volatile uint32_t g_audio_w = 0;                // monotonic write index
static volatile uint32_t g_audio_r = 0;                // monotonic read index
static bool              g_audio_primed = false;       // reader: wait for ~half-full before draining
// PIZERO-121: the producer's working rate, nudged once a frame to hold the ring
// near half full. Nominal is COCO_AUDIO_RATE; the emulated CoCo's clock and the
// pixel clock are independent and disagree by ~0.02%, so without this the ring
// drifts to a wall and either repeats or discards samples forever.
static uint32_t          g_aud_rate = COCO_AUDIO_RATE;
static uint32_t          g_aud_servo_steps = 0;
static volatile uint32_t g_audio_skips = 0;            // reader: overflow catch-ups (diagnostic)
// PIZERO-118: an underrun count alone cannot say whether the machine had
// nothing to play or whether delivery failed. These two say which: `produced`
// is how many samples the source actually made, `tone` how many of them were
// away from the DAC's resting level, i.e. audible.
static volatile uint32_t g_audio_produced = 0;
static volatile uint32_t g_audio_tone = 0;
static int16_t           g_audio_prev = 0;
// Sound is CHANGE, not level: the DAC rests well away from zero, so an
// absolute threshold counts a silent machine as audible (measured on hardware
// 2026-09-17: tone == prod exactly). Count a sample only when it moves.
#define AUDIO_TONE_EPS 64                              // sample-to-sample step that counts as sound
static uint32_t          g_audio_err = 0;              // Bresenham accumulator

// Event-driven, band-limited audio resampler (integrate-and-dump). Rather than
// point-sampling the DAC at 32 kHz (which aliases the square wave's edges into
// inharmonic "fuzz" -- the edges snap to the sample grid), we INTEGRATE the DAC
// level over each 32 kHz sample period using the exact event-tick timestamps.
// g_dac_level is updated the instant the CPU writes the DAC (PIA1 PA/PB) and
// audio_integrate() accumulates level*ticks per memory access; at each sample
// boundary (Bresenham on the 14.3181 MHz tick clock) we emit the time-average.
// This captures edge timing to ~1 memory access (~16 ticks) instead of a whole
// 447-tick sample, removing the jitter aliasing at the source. The
// audio_lowpass setting (PIZERO-212) then softens the DAC's hard steps.
static int32_t  g_dac_level = 0;                       // current DAC+1bit level (set on PIA1 write)
static struct audio_integ g_aud;                       // the sample being integrated (audio_integ.h)
static struct audio_lp g_lp;                           // PIZERO-212: the output low-pass (audio_lowpass.h)
static struct audio_dc g_dc;                           // PIZERO-223: AC coupling, ahead of it
// PIZERO-250: the right channel's filters. While both sides carry the same
// sound only the left's run and the right copies its output; the right's
// state is taken from the left when they part, and they rejoin once both
// filters have settled to the same state, so neither edge clicks.
static struct audio_lp g_lp_r;
static struct audio_dc g_dc_r;
static bool g_aud_split;

// Recompute the cached audio level from PIA1 (called right after a PIA1 write).
// Output gain. The 6-bit DAC swings 64 steps; a square wave's RMS == its peak,
// so this is loud even well below full scale. AUDIO_DAC_GAIN sets the per-step
// amplitude: 480 -> ~47% FS, 240 -> ~24%, 120 -> ~12%, 60 -> ~6% FS (-18 dB from
// 480). Quieter = more sink headroom / less fatiguing harshness (does NOT change
// the off-spec delivery distortion). Lower to soften, raise for louder.
// PIZERO-223: 180, three times the old 60 (+9.5 dB). SN76489 chords at 60
// peaked near -24 dBFS, so TVs were turned right up and muddied them; the DC
// blocker (audio_dc_run) frees the headroom the offset used to take.
// PIZERO-230: 360, doubled again (+6 dB). Measured off the HDMI stream, a full
// 0-63 swing at 180 put a square wave near -15 dBFS. At 360 it is 22,680 p-p:
// about -9 dBFS for a square, and about -3 dBFS for the worst AC-coupled case
// (narrow pulses push one side out to nearly the whole swing, plus the
// low-pass overshoot). Volumes above 10 may clip on such waveforms.
#define AUDIO_DAC_GAIN   360
#define AUDIO_GAIN_X     6         // the same factor for the SN76489
// PIZERO-253: the line is worked out in millivolts (coco_sound.h), where the
// DAC's 63 steps span 4,500 mV, so AUDIO_DAC_GAIN a step is 5.04 a mV; Q10.
#define AUDIO_MV_GAIN_Q10 (AUDIO_DAC_GAIN * 63 * 1024 / 4500)
// PIZERO-145: the gain is the volume setting, 0-15; 10 is AUDIO_DAC_GAIN.
static int32_t g_mv_gain = AUDIO_MV_GAIN_Q10;
static int32_t g_volume = 10;

// PIZERO-253: the sound path (coco_sound.h). g_snd_mode is the
// sound_sources setting. g_dac6 is the DAC value heard, which is the
// register's except while a joystick probe is being taken back
// (joystick_quiet, below). g_line_mv is the line's level, which it holds
// when nothing drives it; g_cart_q10 the cartridge line's share (0 when
// the multiplexer has another source).
static uint8_t g_snd_mode = SND_ALL;
static uint8_t g_dac6 = 0;
static int32_t g_line_mv = 0;
static int32_t g_cart_q10 = 1024;

// PIZERO-148: the Orchestra-90's two DACs. Full scale is XRoar's, the whole
// 4,700 mV of the line, so a DAC swings a little more than the CoCo's. The
// left level rides with the line in the integrator; the right goes in as
// its difference from the left (audio_integ_run2), so a sample is the
// exact average of each.
static uint8_t g_orch_raw[2] = { 0, 0 };
static int32_t g_orch_l = 0, g_orch_d = 0;
static uint32_t g_orch_writes = 0;       // for telemetry

static inline void orch_levels(void) {
    int32_t l = (int32_t)g_orch_raw[0] * 4700 / 255 * g_mv_gain >> 10;
    int32_t r = (int32_t)g_orch_raw[1] * 4700 / 255 * g_mv_gain >> 10;
    g_orch_l = l;
    g_orch_d = r - l;
}

static void orch_write(uint16_t a, uint8_t d) {
    g_orch_raw[a & 1] = d;                // $FF7A left, $FF7B right
    g_orch_writes++;
    orch_levels();
}

static inline void audio_update_level(void) {
    if (!g_m.pia1 || !g_m.pia0) return;
    struct snd_inputs in;
    in.dac6 = g_dac6;
    in.sbs_output = (g_m.pia1->b.direction_register & 2) != 0;     // PB1
    in.sbs_high = in.sbs_output && (g_m.pia1->b.output_register & 2);
    in.enable = PIA_VALUE_CB2(g_m.pia1) != 0;
    in.source = (uint8_t)((PIA_VALUE_CB2(g_m.pia0) ? 2 : 0) | (PIA_VALUE_CA2(g_m.pia0) ? 1 : 0));
    g_line_mv = snd_line_mv(&in, g_snd_mode, g_line_mv, &g_cart_q10);
    g_dac_level = (g_line_mv * g_mv_gain) >> 10;
}

// PIZERO-253: joystick_quiet (snd_probe_* in coco_sound.h): a joystick
// read's trial DAC values are taken back out of the sound. The sample
// before the one being integrated is kept back for it (g_held_*).
static bool     g_joy_quiet = true;
static struct snd_probe g_probe;
static uint8_t  g_probe_dac6;           // what was heard before the trial
static int32_t  g_probe_mv;
static uint32_t g_probes;               // trials taken back, for telemetry
static bool     g_held;                 // the sample kept back, before the filters
static int32_t  g_held_l, g_held_r;
static uint32_t g_held_tk;

// A write to PIA1: the DAC (and the single-bit output, and the enable) may
// have changed. A change of the DAC alone may be a joystick trial.
static inline void audio_pia1_written(void) {
    uint8_t dac6 = (uint8_t)((PIA_VALUE_A(g_m.pia1) >> 2) & 0x3F);
    int32_t level = g_dac_level, mv = g_line_mv;
    uint8_t before = g_dac6;
    g_dac6 = dac6;
    audio_update_level();
    g_probe.armed = false;
    if (g_joy_quiet && dac6 != before && g_dac_level != level) {
        snd_probe_write(&g_probe, (uint32_t)event_current_tick, level);
        g_probe_dac6 = before;
        g_probe_mv = mv;
    }
}

// A write to PIA0: the multiplexer's source may have changed.
static inline void audio_pia0_written(void) {
    g_probe.armed = false;
    audio_update_level();
}


static inline int audio_clamp(int s) {
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}

// PIZERO-248: once a sample, so in RAM; left alone it went to flash.
static void HOT_FUNC(audio_emit)(int s, int sr) {
    // PIZERO-212: the audio_lowpass setting. Clamped after it, since a
    // band-limited edge overshoots. (It replaces PIZERO-30's fixed 2-pole IIR,
    // which was built out because it dulled the tone at the default level.)
    const bool same = s == sr;
    if (!same && !g_aud_split) {
        g_dc_r = g_dc;
        g_lp_r = g_lp;
        g_aud_split = true;
    }
    s = audio_dc_run(&g_dc, s);   // PIZERO-223: AC coupled, as the CoCo's output is
    s = audio_clamp(audio_lp_run(&g_lp, s));
    if (g_aud_split) {
        sr = audio_dc_run(&g_dc_r, sr);
        sr = audio_clamp(audio_lp_run(&g_lp_r, sr));
        if (same && g_dc.y1 == g_dc_r.y1 && g_lp.x1 == g_lp_r.x1 && g_lp.x2 == g_lp_r.x2
            && g_lp.y1 == g_lp_r.y1 && g_lp.y2 == g_lp_r.y2 && g_lp.err == g_lp_r.err)
            g_aud_split = false;   // x1 already agrees: both just took the same input
    } else {
        sr = s;
    }
    // PIZERO-39: single-producer/single-consumer. The writer (core 0) owns ONLY
    // g_audio_w; the reader (which may be the core-1 DMA IRQ in the streaming
    // build) owns g_audio_r and handles overflow on its side. The __dmb publishes
    // the sample BEFORE the index so a cross-core reader never sees w advance past
    // a slot that hasn't been written yet.
    uint32_t w = g_audio_w;
    g_audio_ring[w & (AUDIO_RING_SAMPLES - 1)][0] = (int16_t)s;
    g_audio_ring[w & (AUDIO_RING_SAMPLES - 1)][1] = (int16_t)sr;
    __dmb();
    g_audio_w = w + 1;
    g_audio_produced++;
    { int m = (s + sr) / 2, d = m - (int)g_audio_prev;
      if (d > AUDIO_TONE_EPS || d < -AUDIO_TONE_EPS) g_audio_tone++;
      g_audio_prev = (int16_t)m; }
}

// Called per memory access with the access duration in event ticks. Integrates
// the (piecewise-constant) DAC level and emits 32 kHz samples on tick boundaries.
// When a sample boundary falls inside this access we split EXACTLY at it (the
// ticks past the boundary carry to the next sample) so each emitted sample is
// the true time-average over its period -- no edge jitter, no residual aliasing.
// One finished sample from the integrator, tk ticks long.
// s is the line plus the Orchestra-90's left; s2 its right less its left.
static inline __attribute__((always_inline)) void audio_sample(int s, int s2, uint32_t tk) {
#ifdef CPU_PROF
    uint32_t t0 = cpu_prof_now();
#endif
    // PIZERO-253: the cartridge's sound line (the SN76489, and the AY chips
    // that come back through it) is heard as the multiplexer allows.
    int32_t line = 0;
    if (g_csg_active) line = sn_render(&g_csg, tk);                                // PIZERO-143/145/223
    int sr = s + s2;                                                                // PIZERO-148
    if (g_ayb.active) {                                                             // PIZERO-149/250
        int32_t ln, l, r;
        ayb_render_routed(&g_ayb, tk, g_snd_mode != SND_ALL, &ln, &l, &r);
        line += ln;
        s += l * g_volume * AUDIO_GAIN_X / 10;
        sr += r * g_volume * AUDIO_GAIN_X / 10;
    }
    if (line) {
        line = ((line * g_volume * AUDIO_GAIN_X / 10) * g_cart_q10) >> 10;
        s += line;
        sr += line;
    }
    // Kept back one sample, so a joystick trial that began in it can still
    // be taken out (audio_comparator_read).
    if (g_held) audio_emit(g_held_l, g_held_r);
    g_held = true;
    g_held_l = s;
    g_held_r = sr;
    g_held_tk = tk;
#ifdef CPU_PROF
    g_cpu_prof.samp_clk += cpu_prof_now() - t0;
    g_cpu_prof.samp_n++;
#endif
}

// A read of PIA0 port A (the comparator). If a DAC write came just before,
// take its sound back: the audio is caught up to this cycle already, so
// the span since the write is in the sample being built (g_aud) and, past
// its start, in the sample kept back.
static void HOT_FUNC(audio_comparator_read)(void) {
    if (!snd_probe_read(&g_probe, (uint32_t)event_current_tick, g_dac_level, &g_aud.acc, g_aud.tk,
                        g_held, &g_held_l, &g_held_r, g_held_tk))
        return;
    g_dac6 = g_probe_dac6;
    g_line_mv = g_probe_mv;
    g_dac_level = g_probe.level;
    g_probes++;
}

static inline __attribute__((always_inline)) void audio_integrate(uint32_t ticks) {
    int s, s2 = 0; uint32_t tk;
    if (audio_integ_step2(&g_aud, g_dac_level + g_orch_l, g_orch_d, ticks, g_aud_rate, &s, &s2, &tk))
        audio_sample(s, s2, tk);
}

// PIZERO-131 step (c). Cycles that end before coco_fastbus.deadline only
// advance event_current_tick. g_aud_tick is the tick the audio has been
// integrated up to; the cycles since are whole accesses (16 ticks, or the
// CoCo 3's coco_fastbus.ticks) at one DAC level (a level change comes
// through coco_mem_cycle, which catches up first). PIZERO-220: they may
// cross any number of sample boundaries, and audio_integ_run emits each
// sample exactly as audio_integrate on every access would have.
static uint32_t g_aud_tick;

// The catch-up's work, in one place: inlined into every bus entry, its
// sample loop (and the filter in it) cost 4 KB of RAM.
static void __attribute__((noinline)) HOT_FUNC(audio_catch_up_run)(uint32_t pend);

static inline __attribute__((always_inline)) void audio_catch_up(void) {
    uint32_t pend = event_current_tick - g_aud_tick;
    if (pend) audio_catch_up_run(pend);
}

static void HOT_FUNC(audio_catch_up_run)(uint32_t pend) {
    {
#ifdef CPU_PROF
        uint32_t ct0 = cpu_prof_now();
#endif
        // PIZERO-132: a CoCo 2 at address-dependent speed mixes 8 and 16
        // tick cycles in one run. At one level the integrator gives the same
        // samples however the ticks are split, so take them 8 at a time.
        const uint32_t t = g_c3 ? coco_fastbus.ticks : 8;
        audio_integ_run2(&g_aud, g_dac_level + g_orch_l, g_orch_d, pend / t, t, g_aud_rate, audio_sample);
        if (pend % t) audio_integrate(pend % t);   // never expected: keep time exact
        g_m.total_mem_cycles += pend / coco_fastbus.ticks;
        g_aud_tick = event_current_tick;
#ifdef CPU_PROF
        g_cpu_prof.catch_clk += cpu_prof_now() - ct0;
#endif
    }
}

// The earliest tick at which a cycle needs the tail: the next event, or the
// end of the budget. Called at the end of every tail, with the audio caught
// up to now. PIZERO-220: no longer the next sample boundary as well; the
// catch-up emits the samples a run of cycles crossed (about 800 a frame of
// tails saved at 1.79 MHz, where events still come every scanline).
static inline __attribute__((always_inline)) void fastbus_deadline(void) {
    uint32_t now = event_current_tick;
    g_aud_tick = now;
    uint32_t dl = g_budget_end;
    struct event *head = machine_event_list_global->events;
    if (head && event_tick_delta(head->at_tick, dl) < 0) dl = head->at_tick;
    coco_fastbus.deadline = dl;
}

// Drain up to `max` mono int16 samples; returns count read. PIZERO-39: this is
// the SPSC consumer and may run in the core-1 DMA IRQ (streaming build), so it
// (a) owns g_audio_r exclusively, (b) catches up reader-side if the producer has
// lapped it (>RING behind), (c) pairs the writer's publish __dmb, and (d) lives
// in RAM (.time_critical) so an IRQ call never stalls on XIP flash. PICO_NO_HARDWARE
// (=0, set by the arduino-pico core) makes __not_in_flash_func a no-op here, so we
// apply the section attribute explicitly (see docs/BUILD.md §4a, dvi_data_island.c).
#ifdef DVI_DI_HOST_TEST
#define COCO_RAMFUNC
#else
#define COCO_RAMFUNC __attribute__((section(".time_critical.coco_audio")))
#endif
// Up to `max` left and right pairs into lr (2 * max values).
extern "C" size_t COCO_RAMFUNC coco_machine_audio_read_lr(int16_t *lr, size_t max) {
    uint32_t w = g_audio_w;
    __dmb();                                           // read index before data
    uint32_t r = g_audio_r;
    uint32_t avail = w - r;
    if (avail > AUDIO_RING_SAMPLES) {                  // producer lapped us: skip ahead
        r = w - AUDIO_RING_SAMPLES;
        avail = AUDIO_RING_SAMPLES;
        g_audio_skips++;
    }
    // Prime: don't start draining until ~half full, so the bursty producer and
    // steady consumer settle around mid-ring instead of chattering the limits.
    if (!g_audio_primed) {
        if (avail < AUDIO_RING_SAMPLES / 2) return 0;  // caller holds last (silence) meanwhile
        g_audio_primed = true;
    }
    size_t n = 0;
    while (n < max && r != w) {
        lr[2 * n] = g_audio_ring[r & (AUDIO_RING_SAMPLES - 1)][0];
        lr[2 * n + 1] = g_audio_ring[r & (AUDIO_RING_SAMPLES - 1)][1];
        n++;
        r++;
    }
    g_audio_r = r;
    return n;
}

// The same, mixed to mono (the WAV dump and the non-streaming builds).
extern "C" size_t coco_machine_audio_read(int16_t *dst, size_t max) {
    int16_t lr[2 * 64];
    size_t n = 0;
    while (n < max) {
        size_t want = max - n < 64 ? max - n : 64;
        size_t got = coco_machine_audio_read_lr(lr, want);
        for (size_t i = 0; i < got; i++) dst[n + i] = (int16_t)((lr[2 * i] + lr[2 * i + 1]) / 2);
        n += got;
        if (got < want) break;
    }
    return n;
}

extern "C" uint32_t coco_machine_audio_rate(void) { return COCO_AUDIO_RATE; }

// PIZERO-121: what the servo is actually asking the producer for. Reported in
// the telemetry so a drifting clock is visible as a rate, not inferred from
// skips after the fact.
extern "C" uint32_t coco_machine_audio_rate_now(void) { return g_aud_rate; }

// Diagnostic: current ring fill (samples) and cumulative overflow-skip count.
extern "C" void coco_machine_audio_stats(uint32_t *fill, uint32_t *skips) {
    if (fill)  *fill  = g_audio_w - g_audio_r;
    if (skips) *skips = g_audio_skips;
}

// PIZERO-118: cumulative samples emitted by the source, and how many of those
// were audible. Paired with the consumer's underrun count, these separate
// "the machine was silent" from "the stream was short", which an underrun
// count on its own cannot do.
extern "C" void coco_machine_audio_counters(uint32_t *produced, uint32_t *tone) {
    if (produced) *produced = g_audio_produced;
    if (tone)     *tone     = g_audio_tone;
}

// Audio is event-driven from the memory-access path; run the whole budget in one
// cpu->run. (The VDG already drives field-sync at 60 Hz, so we do NOT regenerate
// it here -- doing so double-drove PIA0 CB1.)
static inline void run_cpu_with_audio(uint32_t cycles) {
    // PIZERO-131: the budget is an end tick, and the audio is caught up at
    // the end of every run, so the servo's rate change between runs applies
    // from here on, as before.
    g_budget_end = event_current_tick + cycles * 16u;
    g_aud_tick = event_current_tick;
    fastbus_deadline();
    // Interrupt lines changed between runs (the cart autostart toggle, a
    // key) are refreshed by the first cycle's tail, as before.
    if (g_m.pia_irq_dirty) coco_fastbus.deadline = event_current_tick;
    g_m.cpu->running = 1;
    g_m.cpu->run(g_m.cpu);
    audio_catch_up();
}

// PIZERO-287: the CART line, PIA1 CB1. It is wired-OR on the real machine:
// a cartridge's autostart square wave and the MIDI Pak's IRQ both pull it
// low. Only a change is passed to the PIA, so its edge detection sees the
// same edges a real one would.
static void cart_line_update(void) {
    bool level = (g_m.cart_rom && g_m.cart_autostart) ? g_m.cart_cb1_level : true;
    if (acia_irq(&g_acia)) level = false;
    if (level != g_cart_line && g_m.pia1) {
        g_cart_line = level;
        mc6821_set_cx1(&g_m.pia1->b, level);
        g_m.pia_irq_dirty = true;
    }
}

static void acia_expired(void *) {
    acia_service(&g_acia, (uint32_t)event_current_tick);
    acia_take_changed(&g_acia);
    acia_after();                         // always: this event has just been used up
}

// After every access and service: the CART line, and the next event.
static void acia_after(void) {
    cart_line_update();
    if (!g_acia_event_ready) {
        event_init(&g_acia_event, MACHINE_EVENT_LIST, DELEGATE_AS0(void, acia_expired, NULL));
        g_acia_event_ready = true;
    }
    if (event_queued(&g_acia_event)) event_dequeue(&g_acia_event);
    uint32_t at;
    if (acia_next_due(&g_acia, (uint32_t)event_current_tick, &at))
        event_queue_abs(&g_acia_event, (event_ticks)at);
}

static uint8_t acia_bus_read(uint16_t A) {
    uint8_t v = acia_read(&g_acia, A & 1, (uint32_t)event_current_tick);
    if (acia_take_changed(&g_acia)) acia_after();   // a status poll that found nothing new is free
    return v;
}

static void acia_bus_write(uint16_t A, uint8_t D) {
    acia_write(&g_acia, A & 1, D, (uint32_t)event_current_tick);
    acia_after();
}

static void acia_power_on(void) {
    if (g_acia_event_ready && event_queued(&g_acia_event)) event_dequeue(&g_acia_event);
    acia_init(&g_acia);
    cart_line_update();
}

// Settings are applied again whenever they are re-read; only a change resets
// the chip, since a reset holds TDRE low and a program waiting to send (Lyra
// polls TDRE) would wait for ever.
extern "C" void coco_machine_set_midi_pak(bool on) {
    if (on == g_acia_on) return;
    g_acia_on = on;
    acia_power_on();
}

extern "C" bool coco_machine_midi_tx_pop(uint8_t *b) { return acia_tx_pop(&g_acia, b); }

// A byte from the host for the CoCo; the event takes it from there.
extern "C" bool coco_machine_midi_rx_push(uint8_t b) {
    if (!g_acia_on) return false;
    bool ok = acia_rx_push(&g_acia, b);
    if (ok && g_m.cpu) acia_after();
    return ok;
}

// True once software has set the chip up (out of master reset): the host
// then runs frames in slices so bytes leave close to when they were written.
extern "C" bool coco_machine_midi_active(void) { return g_acia_on && !g_acia.in_reset; }

extern "C" void coco_machine_midi_counters(uint32_t *tx_dropped, uint32_t *rx_dropped) {
    *tx_dropped = g_acia.tx_dropped; *rx_dropped = g_acia.rx_dropped;
}

extern "C" void coco_machine_run_cycles(uint32_t cycles) {
    if (!g_m.cpu) return;

    // Cartridge autoboot: real CoCo cart-FIRQ carts pulse PIA1 CB1 from the
    // Q clock to nudge BASIC into the FIRQ handler that JMPs $C000. We
    // approximate per upstream xroar by toggling every ~100 ms (~88 950
    // CPU cycles at 0.895 MHz).
    if (g_m.cart_rom && g_m.cart_autostart && g_m.pia1) {
        g_m.cart_toggle_remaining -= (int32_t)cycles;
        while (g_m.cart_toggle_remaining <= 0) {
            g_m.cart_cb1_level = !g_m.cart_cb1_level;
            cart_line_update();                  // PIZERO-287: shared with the MIDI Pak
            g_m.cart_toggle_remaining += 88950;
        }
    }

    // PIZERO-31's fix forced PIA0 CRB bit 0 (the 60 Hz IRQ enable) on every
    // frame. That overrode every program that turns the IRQ off, and crashed
    // NitrOS-9: its REL clears $FF03, enters all-RAM mode, and the forced IRQ
    // then vectored into Disk BASIC's handler, now RAM (PIZERO-191). The IRQ
    // was off because of the Disk BASIC autostart above, now fixed at its
    // source, so the guest owns the bit again.

    // PIZERO-121: one servo step per frame. Only after the reader has primed,
    // because the ring is legitimately empty before that and correcting on it
    // would drive the rate to its clamp for no reason.
    if (g_audio_primed) {
        g_aud_rate = audio_servo_rate(COCO_AUDIO_RATE,
                                      g_audio_w - g_audio_r, AUDIO_RING_SAMPLES);
        g_aud_servo_steps++;
    }

#ifdef CPU_PROF
    struct cpu_prof before = g_cpu_prof;
    uint32_t prof_t = cpu_prof_now();
    run_cpu_with_audio(cycles);
    uint32_t run = cpu_prof_now() - prof_t;
    g_cpu_prof.run_clk += run;
    if (run > g_cpu_prof.wf_run) {               // PIZERO-220: the worst frame
        g_cpu_prof.wf_run = run;
        g_cpu_prof.wf_slow = g_cpu_prof.slow_clk - before.slow_clk;
        g_cpu_prof.wf_ev = g_cpu_prof.ev_clk - before.ev_clk;
        g_cpu_prof.wf_lines = g_cpu_prof.line_n - before.line_n;
        g_cpu_prof.wf_sink = g_cpu_prof.line_clk - before.line_clk;
        g_cpu_prof.wf_cyc = g_cpu_prof.bus_n - before.bus_n;
    }
#else
    run_cpu_with_audio(cycles);
#endif
}

#ifdef CPU_PROF
// PIZERO-209: the cost of one bus timing window, measured once with the same
// macros: 'in' is what a window adds to bus_clk, 'full' what it adds to the
// run as a whole (the two reads plus the counting).
static uint32_t g_prof_window_in, g_prof_window_full;

static void __attribute__((noinline)) cpu_prof_calibrate(void) {
    struct cpu_prof save = g_cpu_prof;
    const uint32_t n = 4096;
    uint32_t t = cpu_prof_now();
    for (uint32_t i = 0; i < n; i++) {
        CPU_PROF_BUS_BEGIN();
        __asm volatile ("" ::: "memory");
        CPU_PROF_BUS_END(0);
    }
    g_prof_window_full = (cpu_prof_now() - t) / n;
    g_prof_window_in = g_cpu_prof.bus_clk / n;
    g_cpu_prof = save;
}

extern "C" void coco_machine_cpu_prof(struct cpu_prof *out, uint32_t *window_in, uint32_t *window_full) {
    static bool started;
    if (!started) {
        // Trace must be on before the cycle counter counts.
        CPU_PROF_DEMCR |= CPU_PROF_DEMCR_TRCENA;
        CPU_PROF_DWT_CTRL |= CPU_PROF_DWT_CYCCNTENA;
        cpu_prof_calibrate();
        started = true;
        memset(&g_cpu_prof, 0, sizeof g_cpu_prof);
    }
    *out = g_cpu_prof;
    *window_in = g_prof_window_in;
    *window_full = g_prof_window_full;
    memset(&g_cpu_prof, 0, sizeof g_cpu_prof);
}
#endif

extern "C" void coco_machine_install_cart(const uint8_t *rom8k) {
    coco_machine_install_cart_sized(rom8k, 8192);
}

extern "C" void coco_machine_install_cart_sized(const uint8_t *rom, uint32_t len) {
    csg_reset();                         // PIZERO-143: the chip leaves with its cart
    g_m.cart_banked = false;
    g_m.cart_base = rom;
    g_m.cart_len = len;
    g_m.cart_rom = rom;
    g_m.cart_mask = (uint16_t)((len >= 2048 && len <= 16384) ? len - 1 : 0x1FFF);
    fastbus_sync();
    g_m.cart_toggle_remaining = 88950;
#ifdef GIME_TIMER
    gime_timer_reset(&g_gime);          // PIZERO-62: stopped until a guest programs it
#endif
    g_m.cart_cb1_level = true;
    // PIZERO-191: a disk controller does not wire CART, so Disk BASIC ("DK"
    // at $C000, the mark Extended BASIC looks for) must not autostart, as
    // upstream's RS-DOS cart does not. Pulsing it fired BASIC's autostart
    // FIRQ ($A0F6) after the cold start: that re-initialises the PIAs,
    // turning the 60 Hz IRQ off that Extended BASIC had just turned on, and
    // restarts Disk BASIC at $C000 without turning it back on (PIZERO-31).
    g_m.cart_autostart = rom && len >= 2 && !(rom[0] == 'D' && rom[1] == 'K');
}

// PIZERO-142: a bank-switched cartridge of len bytes (see cart_gmc.h),
// starting in bank 0. The image must stay valid while installed.
extern "C" void coco_machine_install_cart_banked(const uint8_t *rom, uint32_t len) {
    coco_machine_install_cart_sized(rom, 16384);
    g_m.cart_banked = true;
    g_m.cart_base = rom;
    g_m.cart_len = len;
    g_m.csg_writes = 0;
    g_csg_dropped = 0;
}

extern "C" uint32_t coco_machine_cart_csg_writes(void) { return g_m.csg_writes; }
extern "C" uint32_t coco_machine_cart_csg_dropped(void) { return g_csg_dropped; }
extern "C" void coco_machine_csg_always(bool on) {   // PIZERO-143/145: the sn76489 setting
    g_csg_always = on;
    if (!on && !g_m.cart_banked) csg_reset();
}

// PIZERO-149: which AY cartridges answer. A change silences the chips.
extern "C" void coco_machine_set_ay_carts(bool psg, bool sym12, bool deluxe, bool sym12_stereo) {
    uint8_t mask = (uint8_t)((psg ? AYB_PSG : 0) | (sym12 ? AYB_SYM12 : 0) | (deluxe ? AYB_DELUXE : 0));
    if (mask != g_ayb.enabled) { g_ayb.enabled = mask; ayb_reset(&g_ayb); }
    g_ayb.stereo = sym12_stereo;                     // PIZERO-250: takes effect at once
}
extern "C" uint32_t coco_machine_ay_writes(void) { return g_ayb.writes; }
extern "C" uint8_t coco_machine_ay_active(void) { return g_ayb.active; }

// PIZERO-145: the rest of the settings.txt switches.
extern "C" void coco_machine_set_volume(int v) {
    if (v < 0) v = 0; else if (v > 15) v = 15;
    g_volume = v;
    g_mv_gain = AUDIO_MV_GAIN_Q10 * v / 10;
    audio_update_level();
    orch_levels();                        // PIZERO-148
}

// PIZERO-253: the sound_sources and joystick_quiet settings.
extern "C" void coco_machine_set_sound_sources(bool all) {
    g_snd_mode = all ? SND_ALL : SND_SWITCHED;
    g_probe.armed = false;
    audio_update_level();
}
extern "C" void coco_machine_set_joystick_quiet(bool on) {
    g_joy_quiet = on;
    g_probe.armed = false;
}
extern "C" uint32_t coco_machine_joystick_probes(void) { return g_probes; }

// PIZERO-148: the orchestra90 setting. Off, the DACs fall silent.
extern "C" void coco_machine_set_orchestra90(bool on) {
    g_orch_on = on;
    if (!on) { g_orch_raw[0] = g_orch_raw[1] = 0; orch_levels(); }
}
extern "C" uint32_t coco_machine_orch_writes(void) { return g_orch_writes; }

// PIZERO-212: the output low-pass cutoff in Hz, 0 for off. Coefficients are
// for the nominal rate; the servo moves the real one by under 0.3%.
extern "C" void coco_machine_set_lowpass(int hz) {
    audio_lp_set(&g_lp, hz > 0 ? (uint32_t)hz : 0u, COCO_AUDIO_RATE);
    g_lp_r = g_lp;
}

extern "C" void coco_machine_set_artifact(int mode) {
    g_artifact_mode = (uint8_t)(mode >= 0 && mode <= 2 ? mode : 1);
}

// Off: the registers vanish from the bus and the palette returns to the
// default, so a program that recolored the screen cannot leave it so.
extern "C" void coco_machine_set_gime_palette(bool on) {
    g_gime_pal_on = on;
    if (!on) coco_machine_palette_reset();
}

// Off: the registers vanish from the bus and the timer stops.
extern "C" void coco_machine_set_gime_timer(bool on) {
    g_gime_timer_on = on;
#ifdef GIME_TIMER
    if (!on) { gime_timer_reset(&g_gime); g_m.pia_irq_dirty = true; }
#endif
}

// PIZERO-81d: a power-on restart without a power cycle, for launching from
// the F12 overlay. Ported from the Fruit Jam port's coco_machine_cold_reset
// (FRUITJAM-69), whose reasoning holds here too:
//   * RAM is zeroed, so Color BASIC's warm-start flag cannot match and the
//     ROM takes the cold path: banner, RAM clear, no old program left.
//   * The SAM must be reset. A program that left it in all-RAM mode (TY,
//     $FFDF) would otherwise map $A000-$BFFF to the RAM just zeroed; the
//     reset vector still comes from ROM, so the CPU would jump into ROM
//     space and execute zeros, forever. The real SAM's RESET clears this.
//   * The PIAs are left alone: they do not gate ROM visibility, BASIC
//     programs them during init, and mc6821_reset() would clobber the
//     in_source/preread hookups coco_machine_init installs after it.
// The cartridge, the disk reader and the mounted drives are kept: the
// caller has just chosen them. Our own additions go back to power-on state
// too: the palette, the GIME timer and the FDC's registers.
extern "C" void coco_machine_cold_reset(void) {
    if (!g_m.cpu) return;
    coco_machine_release_all_keys();
    if (g_c3) {
        // PIZERO-133: the GIME's reset clears INIT0, so the MMU is off and
        // the ROM back at $8000-$FFFF, as at power-on.
        memset(g_c3bus.ram_lo, 0, 64 * 1024);
        memset(g_c3bus.ram_hi, 0, 64 * 1024);
        g_m.fdc_status = 0x04;
        g_m.fdc_track = g_m.fdc_sector = g_m.fdc_data = g_m.fdc_command = 0;
        g_m.fdc_latch = 0;
        g_m.fdc_drive = 0;
        g_m.fdc_busy = false;
        g_m.fdc_writing = false;
        g_m.fdc_buf_pos = g_m.fdc_buf_len = 0;
        if (g_m.cart_banked) g_m.cart_rom = g_m.cart_base;
        fastbus_sync();
        csg_reset();
        ayb_reset(&g_ayb);                           // PIZERO-149
        acia_power_on();                             // PIZERO-287
        tcc1014_reset(g_gime3);
        fastbus_sync();                              // PIZERO-213: the map after reset
        g_m.fdc_intrq = false;                       // PIZERO-320: no NMI left over
        g_m.cpu->reset(g_m.cpu);
        return;
    }
    memset(g_m.ram, 0, 64 * 1024);
    g_m.sam->reset(g_m.sam);
    g_m.sam_ty = false;
    g_m.sam_f  = 0;
    g_m.sam_v  = 0;
    g_m.sam_r  = 0;     // PIZERO-132: RESET clears the rate as well
    coco_machine_palette_reset();
#ifdef GIME_TIMER
    gime_timer_reset(&g_gime);
#endif
    g_m.fdc_status = 0x04;
    g_m.fdc_track = g_m.fdc_sector = g_m.fdc_data = g_m.fdc_command = 0;
    g_m.fdc_latch = 0;
    g_m.fdc_drive = 0;
    g_m.fdc_busy = false;
    g_m.fdc_writing = false;
    g_m.fdc_buf_pos = g_m.fdc_buf_len = 0;
    g_m.cart_toggle_remaining = 88950;
    g_m.cart_cb1_level = true;
    if (g_m.cart_banked) g_m.cart_rom = g_m.cart_base;   // PIZERO-142: bank 0
    fastbus_sync();                                       // PIZERO-131: TY and the bank
    csg_reset();                                          // PIZERO-143: silent at power-on
    ayb_reset(&g_ayb);                                    // PIZERO-149
    acia_power_on();                                      // PIZERO-287
    g_m.fdc_intrq = false;                       // PIZERO-320: no NMI left over
    g_m.cpu->reset(g_m.cpu);
}

// PIZERO-314: take the machine down, so coco_machine_init can build the
// other one without restarting the board. Every part is freed (each takes
// its own events off the list), then the list, then the RAM, so the heap
// gets back the CoCo 3's 64 KB or the CoCo 2's VDG buffer in one piece.
// The settings this file holds (font, palette, sound chips, MIDI Pak and
// the rest) are kept: the caller applies its settings again after init.
// The cartridge and the disk callbacks are dropped; the caller installs
// them again, as at boot.
extern "C" void coco_machine_shutdown(void) {
    if (!g_m.cpu) return;
    coco_machine_release_all_keys();
#ifdef GIME_TIMER
    if (g_gime_event_ready && event_queued(&g_gime_event)) event_dequeue(&g_gime_event);
    g_gime_event_ready = false;
    gime_timer_reset(&g_gime);
#endif
    if (g_acia_event_ready && event_queued(&g_acia_event)) event_dequeue(&g_acia_event);
    g_acia_event_ready = false;
    if (g_gime3) { part_free(&g_gime3->part); g_gime3 = nullptr; }
    if (g_m.vdg) part_free(&g_m.vdg->part);
    if (g_m.sam) part_free(&g_m.sam->part);
    part_free(&g_m.pia1->part);
    part_free(&g_m.pia0->part);
    part_free(&g_m.cpu->debug_cpu.part);
    free(machine_event_list_global);
    machine_event_list_global = nullptr;
#ifdef COCO3
    if (g_c3) {                     // PIZERO-319: its second 64 KB is the kept block
        g_c3bus = (struct coco3_bus){};
        g_c3 = false;
    }
#endif
    rp_mem_free(g_m.ram);
    g_m = CocoMachine{};
#ifdef COCO3
    g_m.vdg_buffer = nullptr;
#endif
    csg_reset();
    ayb_reset(&g_ayb);
    acia_init(&g_acia);
    g_cart_line = true;
}

extern "C" const uint8_t *coco_machine_get_vdg_buffer(void) {
    return g_m.vdg_buffer;
}

// Whole-frame VDG renderer. Reads PIA1 PB mode bits + SAM F register +
// CoCo RAM and generates one frame's worth of palette indices into
// g_m.vdg_buffer, replacing xroar's per-scanline render path (suppressed
// by SUPPRESS_RENDER_SCANLINE). Called from core 0's loop(), see the
// WP_RENDER phase marker in src/main.cpp.
//
// Mode coverage (dispatch in coco_machine_render_frame below):
//   * ALPHA  (PB7=0): font_6847t1 glyph via the g_alpha_lut fast
//                               path; SG4 semigraphics handled per-byte
//                               inside render_alpha_frame (ch & 0x80)
//   * RG6    (PB7=1, GM=7): render_rg6_frame, incl. the NTSC-artifact
//                               colour path (PIZERO-43 sets the phase)
//   * GM 0-6 (PB7=1): render_graphics_frame: RG = 1 bit/pixel
//                               fg/bg, CG = 2 bits/pixel (cg_base + value),
//                               with horizontal replication and nLPR row
//                               repetition
//
// Not implemented: SG6. (The original AMOLED-57 comment here listed SG4 and
// CG1-6 as deferred "show black until implemented", that went stale when
// render_graphics_frame landed; corrected PIZERO-83, 2026-08-17.)
//
// Palette indices match g_vdg_rgb565[] in coco_boot.cpp:
//   0=GREEN  4=WHITE  8=BLACK  etc.  Indices 12-15 are unused by the 6847
//   (see PIZERO-55 / PIZERO-85 for making this LUT programmable).

extern "C" const uint8_t font_6847t1[];  // 128 chars × 12 rows = 1.5 KB
extern "C" const uint8_t font_6847[];    // PIZERO-166: 64 chars, font = classic
extern "C" const uint8_t font_6847t2[];  // PIZERO-166: ours, font = 6847t2

// Palette indices: match g_vdg_rgb565[] in coco_boot.cpp.
#define PAL_GREEN       0
#define PAL_YELLOW      1
#define PAL_BLUE        2
#define PAL_RED         3
#define PAL_WHITE       4
#define PAL_CYAN        5
#define PAL_MAGENTA     6
#define PAL_ORANGE      7
#define PAL_BLACK       8
// The tests build the same tables from vdg_pack.h, so the two sets of palette
// constants must not drift apart (PIZERO-109).
static_assert(PAL_GREEN == VDG_PAL_GREEN && PAL_WHITE == VDG_PAL_WHITE &&
              PAL_BLUE == VDG_PAL_BLUE && PAL_ORANGE == VDG_PAL_ORANGE,
              "vdg_pack.h palette indices must match coco_machine.cpp");
#define PAL_DARK_GREEN  9
// PIZERO-140: the GM 0-6 renderer in vdg_pack.h draws RG backgrounds in these.
static_assert(PAL_BLACK == VDG_PAL_BLACK && PAL_DARK_GREEN == VDG_PAL_DARK_GREEN,
              "vdg_pack.h palette indices must match coco_machine.cpp");

// Lines-per-row for each graphics GM (vertical replication) is VDG_GM_NLPR
// in vdg_pack.h. GM bit0 also selects RG (1 bit/pixel, 2 colours) vs CG
// (2 bits/pixel, 4).

static inline void put2(uint8_t *dst, int px, uint8_t color) {
    // Pack into vdg_buffer (low nibble = even px, high nibble = odd px).
    int byte_idx = px >> 1;
    if (px & 1) dst[byte_idx] = (dst[byte_idx] & 0x0F) | (color << 4);
    else        dst[byte_idx] = (dst[byte_idx] & 0xF0) | color;
}

// Fast path for alpha glyphs: precomputed expansion of an 8-bit font row into
// the 4 packed vdg_buffer bytes (8 nibbles), for the two BASIC colourings.
// nibble[N] (pixel N, MSB-first) = bit set ? ink : paper; packed little-endian
// so one 32-bit store replaces 8 per-pixel read-modify-writes (put2).
//   [0] = normal  green-on-black (ink=GREEN, paper=BLACK)
//   [1] = inverse black-on-green (ink=BLACK, paper=GREEN)
static uint32_t g_alpha_lut[2][256];
static bool     g_alpha_lut_ready = false;

// PIZERO-109: the table builders live in vdg_pack.h so the native tests can
// exercise the same code the firmware runs, rather than a copy of it.
static void build_alpha_lut() {
    vdg_build_alpha_table(g_alpha_lut);
    g_alpha_lut_ready = true;
}

static vdg_sg4_table_t g_sg4_lut;
static bool            g_sg4_lut_ready = false;

static void build_sg4_lut() {
    vdg_build_sg4_table(g_sg4_lut);
    g_sg4_lut_ready = true;
}

// PIZERO-166: glyph and colour pair for each of the 128 alpha codes, for the
// current font set and EXT line, rebuilt only when either changes.
static uint8_t g_alpha_glyph[128];
static uint8_t g_alpha_pair[128];
static int     g_alpha_map_key = -1;

static void build_alpha_map(int key) {
    for (int ch = 0; ch < 128; ch++)
        g_alpha_glyph[ch] = vdg_alpha_glyph((uint8_t)ch, key & 1, key >> 1, &g_alpha_pair[ch]);
    g_alpha_map_key = key;
}

extern "C" void coco_machine_set_font(int font) {
    if (font < VDG_FONT_CLASSIC || font > VDG_FONT_T2) font = VDG_FONT_T2;
    g_font = font;
}

extern "C" void coco_machine_set_lowercase(_Bool on) { g_lowercase = on; }

static void HOT_FUNC(render_alpha_frame)(uint16_t base) {
    if (!g_alpha_lut_ready) build_alpha_lut();
    if (!g_sg4_lut_ready) build_sg4_lut();
    int key = (g_font << 1) | (vdg_lower_case(g_vdg_ext, g_font, g_lowercase) ? 1 : 0);
    if (key != g_alpha_map_key) build_alpha_map(key);
    const uint8_t *font = g_font == VDG_FONT_CLASSIC ? font_6847
                        : g_font == VDG_FONT_T1      ? font_6847t1 : font_6847t2;
    // 32 chars wide × 16 text rows × 12 pixel rows = 192 lines.
    // Each glyph is 8 px wide → 32 chars × 8 = 256 px per line.
    for (int text_row = 0; text_row < 16; text_row++) {
        for (int sub_row = 0; sub_row < 12; sub_row++) {
            int row = text_row * 12 + sub_row;
            if (row >= COCO_VDG_H) return;
            uint8_t *dst = &g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
            const uint8_t *chrow = &g_m.ram[(base + text_row * 32) & 0xFFFF];
            for (int col = 0; col < 32; col++) {
                uint8_t ch = chrow[col];
                if (ch & 0x80) {
                    // SG4 semigraphics: 2×2 colour block in this cell.
                    // PIZERO-119: one packed store, not 8 read-modify-writes.
                    uint8_t sg = (sub_row < 6) ? (ch >> 2) : ch;
                    *(uint32_t *)(dst + col * 4) = g_sg4_lut[(ch >> 4) & 7][sg & 3];
                } else {
                    // Alpha: the font set picks the glyph and colour pair
                    // (PIZERO-166, vdg_alpha_glyph in vdg_pack.h).
                    uint8_t glyph = font[g_alpha_glyph[ch] * 12 + sub_row];
                    *(uint32_t *)(dst + col * 4) = g_alpha_lut[g_alpha_pair[ch]][glyph];
                }
            }
        }
    }
}

// PIZERO-119: the same trick the alpha path already uses, applied to RG6.
// One source byte becomes 8 packed pixels = one 32-bit store, instead of 8 or
// 16 read-modify-writes through put2. At 32 bytes x 192 rows that is 6,144
// stores a frame where the artifact path was doing 49,152 RMWs.
static uint32_t g_rg6_lut[256];        // monochrome: bit set -> white
static uint32_t g_rg6a_lut[256];       // artifact: bit PAIRS -> four colours
static bool     g_rg6_lut_ready = false;
static uint8_t  g_rg6a_key = 0xFF;     // c01<<4|c10 the artifact table was built for

static void build_rg6_lut() {
    vdg_build_rg6_mono_table(g_rg6_lut);
    g_rg6_lut_ready = true;
}

static void build_rg6a_lut(uint8_t c01, uint8_t c10) {
    vdg_build_rg6_artifact_table(g_rg6a_lut, c01, c10);
    g_rg6a_key = (uint8_t)((c01 << 4) | c10);
}

static void HOT_FUNC(render_rg6_frame)(uint16_t base) {
    // 32 bytes × 8 bits = 256 pixels per scanline × 192 scanlines. PIZERO-140:
    // rows advance and repeat as the SAM says (e.g. V4 doubles each row).
    int stride, lines;
    vdg_gfx_geometry(7, g_m.sam_v, &stride, &lines);
    if (g_artifact_active && g_artifact_mode != 0) {
        // AMOLED-22 NTSC artifact colours: PMODE 4 (RG6, PB[7:4]=1111)
        // produces colour from adjacent bit PAIRS. CSS picks the pair.
        //   00→BLACK  01→c01  10→c10  11→WHITE
        // PIZERO-43: the artifact red/blue PHASE is arbitrary on real hardware
        // (power-on dependent). Default to the orientation Space Warp assumes;
        // build -DARTIFACT_PHASE_LEGACY for the previous (opposite) phase.
        // PIZERO-145: the phase is the artifact_colors setting ('swapped').
        const bool swap = (g_artifact_mode == 2);
        const uint8_t c01 = (g_artifact_css != swap) ? PAL_BLUE   : PAL_ORANGE;
        const uint8_t c10 = (g_artifact_css != swap) ? PAL_ORANGE : PAL_BLUE;
        if (g_rg6a_key != (uint8_t)((c01 << 4) | c10)) build_rg6a_lut(c01, c10);
        for (int row = 0; row < COCO_VDG_H; row++) {
            const uint8_t *src = &g_m.ram[(base + (row / lines) * stride) & 0xFFFF];
            uint32_t *dst = (uint32_t *)&g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
            for (int byte = 0; byte < 32; byte++)
                dst[byte] = g_rg6a_lut[src[byte]];
        }
        return;
    }
    // Plain monochrome RG6 (white on black).
    if (!g_rg6_lut_ready) build_rg6_lut();
    for (int row = 0; row < COCO_VDG_H; row++) {
        const uint8_t *src = &g_m.ram[(base + (row / lines) * stride) & 0xFFFF];
        uint32_t *dst = (uint32_t *)&g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
        for (int byte = 0; byte < 32; byte++)
            dst[byte] = g_rg6_lut[src[byte]];
    }
}

// AMOLED-60: general color/resolution-graphics renderer for GM 0-6.
// (RG6/GM7 is handled by render_rg6_frame for its NTSC-artifact path.)
// PIZERO-140: table-driven, in vdg_pack.h. Each data byte becomes its run of
// packed pixels in one lookup instead of a per-pixel loop; the host test
// checks every mode frame-for-frame against the per-pixel rule. Popcorn
// (GM5) took ~7.5 ms a frame here before and ran at 50 fps.
static struct vdg_gm_lut g_gm_lut = { -1, {{0}} };
static void HOT_FUNC(render_graphics_frame)(uint16_t base, uint8_t gm, bool css) {
    int stride, lines;
    vdg_gfx_geometry(gm, g_m.sam_v, &stride, &lines);
    vdg_render_gm(&g_gm_lut, g_m.ram, base, gm, css, stride, lines, g_m.vdg_buffer);
}

extern "C" void HOT_FUNC(coco_machine_render_frame)(void) {
    if (!g_m.pia1 || g_c3) return;     // PIZERO-133: the GIME draws as it goes
    const uint8_t pb = (g_m.pia1->b.out_source & g_m.pia1->b.out_sink) & 0xFF;
    const uint16_t base = g_m.sam_f ? g_m.sam_f : 0x0400;
    const uint8_t gm = (pb >> 4) & 7;
    const bool    css = (pb & 0x08) != 0;
    if (pb & 0x80) {
        // Graphics. GM=7 (RG6/PMODE 4) has its own NTSC-artifact path.
        if (gm == 7) render_rg6_frame(base);
        else         render_graphics_frame(base, gm, css);
    } else {
        // Alpha + semigraphics (SG4 handled per-byte inside).
        render_alpha_frame(base);
    }
}

// PIZERO-119: which VDG mode is on screen, as the raw PIA1-B mode bits
// (bit 7 = graphics, bits 6-4 = GM, bit 3 = CSS). Measurements are hard to
// read without it: the three render paths differ by an order of magnitude in
// cost, so "render took 4.8 ms" means nothing until you know which one ran.
extern "C" uint8_t coco_machine_sam_v(void) { return g_m.sam_v; }   // PIZERO-140

extern "C" uint8_t coco_machine_vdg_mode_bits(void) {
    if (!g_m.pia1) return 0;
    return (uint8_t)((g_m.pia1->b.out_source & g_m.pia1->b.out_sink) & 0xF8);
}

extern "C" uint16_t coco_machine_get_pc(void) {
    return g_m.cpu ? g_m.cpu->reg_pc : 0;
}

extern "C" uint32_t coco_machine_get_total_mem_cycles(void) {
    return g_m.total_mem_cycles;
}

extern "C" uint32_t coco_machine_get_render_lines(void) {
    return g_m.render_lines;
}

// PIZERO-315: as BASIC sees it at power-on. On a CoCo 3 that is the MMU's
// task 0 at reset, blocks $38-$3F, physical $70000 up (in 128 KB, its upper
// 64 KB), where the 32-column screen at $0400 lives.
extern "C" const uint8_t *coco_machine_peek_ram(uint16_t addr) {
#ifdef COCO3
    if (g_c3) return coco3_ram_at(&g_c3bus, 0x70000u + addr);
#endif
    return &g_m.ram[addr];
}

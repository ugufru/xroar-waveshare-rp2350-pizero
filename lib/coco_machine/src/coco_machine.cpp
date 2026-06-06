/*
 * coco_machine.cpp — see coco_machine.h.
 *
 * Owns:
 *   * 64 KB RAM in PSRAM
 *   * 16 KB ROM pointer (caller-owned, lives in flash)
 *   * MC6809, MC6883, two MC6821s, MC6847 — all via the XRoar part system
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
}

struct CocoMachine {
    uint8_t *ram = nullptr;
    const uint8_t *rom = nullptr;
    size_t rom_len = 0;
    const uint8_t *cart_rom = nullptr;   // 8 KB at $C000, NULL = no cart
    int32_t cart_toggle_remaining = 0;   // 6809 cycles until next CART pulse
    bool    cart_cb1_level = true;

    // Minimal WD279x-ish FDC state — enough for DECB sector reads.
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
    coco_disk_read_sector_fn fdc_read_cb = nullptr;

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


    struct MC6809 *cpu = nullptr;
    struct MC6883 *sam = nullptr;
    struct MC6821 *pia0 = nullptr;
    struct MC6821 *pia1 = nullptr;
    struct MC6847 *vdg = nullptr;

    int32_t cycles_remaining = 0;
    uint32_t total_mem_cycles = 0;
    uint32_t render_lines = 0;

    // Nibble-packed: 2 palette indices per byte. Low nibble = even cx,
    // high nibble = odd cx. See coco_vdg_render / coco_boot_blit_vdg_src.
    uint8_t vdg_buffer[COCO_VDG_H * (COCO_VDG_W / 2)];
};
static_assert((COCO_VDG_W & 1) == 0, "COCO_VDG_W must be even for nibble packing");

static CocoMachine g_m;

// - - - bus -------------------------------------------------------

// Keyboard matrix state. Per xroar upstream (coco3.c / dragon.c) the CoCo's
// PIA0 wiring is: PA = ROWS (read by CPU), PB = COLUMNS (written by CPU).
// dkbd_matrix_point's (row, col) follows the same convention. We store one
// byte per dkbd-column, each bit a dkbd-row: 1=not pressed, 0=pressed.
static uint8_t g_kb_col_row_mask[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// AMOLED-22 globals consumed by mc6847.c's render_scanline LUT rebuild.
// g_artifact_active is set by coco_pia1b_postwrite to indicate the VDG is
// truly in PMODE 4 / RG6 (¬A/G + GM2 + GM1 + GM0). When non-zero, the LUT
// emits NTSC artifact colours instead of plain fg/bg pixel duplicates.
extern "C" _Bool g_artifact_active = 0;
extern "C" _Bool g_artifact_css    = 0;

// AMOLED-26: PIA1 PB bits 7..3 are the VDG mode lines (¬A/G, GM2, GM1, GM0,
// CSS). Demos write PIA1 PB to switch to PMODE 4 / RG6. Match upstream
// dragon/coco wiring: extract the value, mirror GM0 into the INT/EXT bit,
// hand to mc6847_set_mode. Fired from PIA1 PB postwrite.
extern "C" void coco_pia1b_postwrite(void *sptr) {
    (void)sptr;
    if (!g_m.pia1 || !g_m.vdg) return;
    unsigned pb = (g_m.pia1->b.out_source & g_m.pia1->b.out_sink) & 0xf8;
    unsigned vmode = pb | ((pb & 0x10) << 4);   // GM0 -> INT/EXT
    mc6847_set_mode(g_m.vdg, vmode);
    // AMOLED-22: PMODE 4 / RG6 = ¬A/G + GM2 + GM1 + GM0 (PB bits 7,6,5,4)
    // all set. CSS (PB bit 3) chooses the colour pair.
    g_artifact_active = ((pb & 0xF0) == 0xF0);
    g_artifact_css    = (pb & 0x08) != 0;
}

extern "C" void HOT_FUNC(coco_pia0_preread_a)(void *sptr) {
    (void)sptr;
    if (!g_m.pia0) return;
    uint8_t col_sel = PIA_VALUE_B(g_m.pia0);
    uint8_t rows = 0xFF;
    for (int c = 0; c < 8; c++) {
        if (!(col_sel & (1u << c))) {
            rows &= g_kb_col_row_mask[c];
        }
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

extern "C" void coco_machine_press_key(uint8_t dscan) {
    if (dscan >= 0x40) return;
    uint8_t row, col;
    dscan_to_row_col(dscan, &row, &col);
    g_kb_col_row_mask[col] &= ~(1u << row);
}

extern "C" void coco_machine_release_key(uint8_t dscan) {
    if (dscan >= 0x40) return;
    uint8_t row, col;
    dscan_to_row_col(dscan, &row, &col);
    g_kb_col_row_mask[col] |= (1u << row);
}

extern "C" void coco_machine_release_all_keys(void) {
    memset(g_kb_col_row_mask, 0xFF, sizeof(g_kb_col_row_mask));
}


// - - - minimal FDC -----------------------------------------------
//
// Emulates just the WD279x register-level behaviour DECB actually uses
// for sector reads:
//   $FF40 — RSDOS control latch: bit 4 = single/double density,
//           bits 0-3 = drive select, bit 5 = drive precomp/motor (we
//           don't model timing), bit 7 = HALT enable (we ignore — our
//           reads complete synchronously, so DECB's HALT wait just
//           passes through immediately).
//   $FF48 — read: status. write: command.
//   $FF49 — track register.
//   $FF4A — sector register.
//   $FF4B — data register.
//
// All operations complete instantly (no DRQ/INTRQ delay). DECB polls
// status looking for !BUSY + bytes available, and we satisfy that on
// the first poll after a READ_SECTOR command.

// Real CoCo wires FDC INTRQ through an NMI-enable flip-flop set by writes
// to $FF40 with bit 7 (HALT enable). DECB sets this before each FDC command
// and SYNCs the CPU waiting for the resulting NMI. So whenever an FDC
// command completes we must pulse NMI on the 6809 — but only if HALT was
// armed in $FF40.
static void fdc_signal_intrq(void) {
    if (g_m.cpu && (g_m.fdc_latch & 0x80)) {
        MC6809_NMI_SET(g_m.cpu, 1);
    }
}

static void fdc_handle_command(uint8_t cmd) {
    g_m.fdc_command = cmd;
    uint8_t top = cmd >> 4;
    // Type IV — Force Interrupt
    if (top == 0xD) {
        g_m.fdc_busy = false;
        g_m.fdc_status = (g_m.fdc_track == 0) ? 0x04 : 0x00;
        g_m.fdc_buf_len = 0;
        return;  // Force Interrupt does not raise NMI on its own.
    }
    // Type I — Restore, Seek, Step, StepIn, StepOut.
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
    // Type II — Read Sector (top 0x8/0x9) / Write Sector (0xA/0xB).
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
            g_m.fdc_status = 0x10;
            g_m.fdc_busy = false;
            fdc_signal_intrq();
            return;
        }
        g_m.fdc_buf_len = 256;
        g_m.fdc_data = g_m.fdc_buf[0];
        g_m.fdc_status = 0x03;   // BUSY | DRQ
        g_m.fdc_busy = true;
        // NMI fires after the last byte is consumed — see fdc_io_read.
        return;
    }
    if (top == 0xA || top == 0xB) {
        g_m.fdc_status = 0x40;
        g_m.fdc_busy = false;
        fdc_signal_intrq();
        return;
    }
    // Type III — stubs.
    g_m.fdc_status = 0x10;
    g_m.fdc_busy = false;
    fdc_signal_intrq();
}

static uint8_t fdc_io_read(uint16_t A) {
    switch (A & 0xF) {
    case 0x8: return g_m.fdc_status;
    case 0x9: return g_m.fdc_track;
    case 0xA: return g_m.fdc_sector;
    case 0xB: {
        uint8_t v = g_m.fdc_data;
        if (g_m.fdc_busy && g_m.fdc_buf_pos < g_m.fdc_buf_len - 1) {
            g_m.fdc_buf_pos++;
            g_m.fdc_data = g_m.fdc_buf[g_m.fdc_buf_pos];
            g_m.fdc_status = 0x03;  // BUSY + DRQ
        } else if (g_m.fdc_busy) {
            // last byte just delivered — clear BUSY + DRQ, set TRACK00 if t0,
            // and fire NMI to wake DECB's SYNC waiting for FDC completion.
            g_m.fdc_busy = false;
            g_m.fdc_status = (g_m.fdc_track == 0) ? 0x04 : 0x00;
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
        g_m.fdc_latch = D;
        // Bits 0-3 select drive 0..3 (one-hot).
        for (unsigned i = 0; i < 4; i++) {
            if (D & (1u << i)) { g_m.fdc_drive = i; break; }
        }
        return;
    case 0x8: fdc_handle_command(D); return;
    case 0x9: g_m.fdc_track  = D;    return;
    case 0xA: g_m.fdc_sector = D;    return;
    case 0xB: g_m.fdc_data   = D;    return;
    default:  return;
    }
}

extern "C" void coco_machine_install_disk_reader(coco_disk_read_sector_fn fn) {
    g_m.fdc_read_cb = fn;
}

extern "C" void coco_machine_loadm_write(uint16_t addr, const uint8_t *src, uint16_t len) {
    if (!g_m.ram) return;
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

// AMOLED-52: per-region profiler for coco_mem_cycle. Set to 1 to capture
// where the hot path spends its time, then revert to 0 for production.
// Overhead is ~5-10% measured frame time (4 micros() calls per mem cycle).
#define PROFILE_MEM_CYCLE 0

#if PROFILE_MEM_CYCLE
static uint64_t prof_sam_us       = 0;
static uint64_t prof_data_us      = 0;
static uint64_t prof_events_us    = 0;
static uint64_t prof_irq_us       = 0;
static uint64_t prof_calls        = 0;
static uint32_t prof_last_log_ms  = 0;
#endif

// PIZERO-18 audio: event-driven resampler hooks (defined in the audio section).
static inline void audio_update_level(void);
static inline void audio_integrate(uint32_t ticks);

extern "C" void HOT_FUNC(coco_mem_cycle)(void *sptr, _Bool RnW, uint16_t A) {
    (void)sptr;
    g_m.total_mem_cycles++;

#if PROFILE_MEM_CYCLE
    uint32_t pt0 = micros();
#endif

    // AMOLED-32 fast-path SAM decode. The generic mc6883_mem_cycle handles
    // Dragon32/64/CoCo1/2/3/MC10 + the '785 variant + fast/slow cycle state
    // machine. We're CoCo 2 only and don't care about cycle-accurate fast
    // mode — flat 16-tick slow cycle per access matches default speed.
    //
    // AMOLED-53 branch order: the overwhelming common case is A < 0xFF00
    // (RAM / ROM / cart ROM — i.e. every instruction fetch and almost every
    // data access). Putting that first keeps the branch predictor happy.
    // SAM register writes ($FFC0-$FFDF) are rare but have full side effects
    // (V/F/M/P/R/TY + vdg_update delegate); punt to xroar.
    int ncycles;
    unsigned S;
    if (A < 0xFF00) {
        // Bulk of memory: RAM / ROM / cart ROM. With TY=1 (all-RAM mode)
        // everything below FF00 is RAM.
        if (!(A & 0x8000) || g_m.sam_ty) S = RnW ? 0 : 7;
        else if (A < 0xA000)              S = RnW ? 1 : 7;
        else if (A < 0xC000)              S = RnW ? 2 : 7;
        else                              S = RnW ? 3 : 7;
        g_m.sam->S = S;
        ncycles = 16;
    } else if ((A & 0xFFE0) == 0xFFC0 && !RnW) {
        // SAM register write — slow path.
        ncycles = mc6883_mem_cycle(g_m.sam, RnW, A);
        S = g_m.sam->S;
        // Shadow the TY bit for our fast decode.
        if (A == 0xFFDE) g_m.sam_ty = false;
        else if (A == 0xFFDF) g_m.sam_ty = true;
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
        ncycles = 16;
    }

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = micros(); prof_sam_us += (uint32_t)(pt1 - pt0); pt0 = pt1; }
#endif

    // AMOLED-53 IRQ-dirty tightening: per the 6821 datasheet only writes
    // to CRA/CRB (A0=1) and reads of PADR/PBDR (A0=0) can change the IRQ
    // output. The other two combinations (CRA/CRB read, PADR/PBDR write)
    // cannot — skip the dirty mark in those cases so the IRQ-OR + macro
    // dispatch at the bottom runs less often.
    //
    // Now use SAM->S (set by mem_cycle) to route data on the bus.
    if (RnW) {
        switch (g_m.sam->S) {
        case 0:  g_m.cpu->D = g_m.ram[A]; break;
        case 1:
        case 2:  g_m.cpu->D = g_m.rom[A & 0x3FFF]; break;
        case 3:  // Cartridge ROM at $C000-$DFFF (mirrored to $E000-$FDFF)
            g_m.cpu->D = g_m.cart_rom ? g_m.cart_rom[A & 0x1FFF] : 0xFF;
            break;
        case 4:  g_m.cpu->D = mc6821_read(g_m.pia0, A);
                 if ((A & 1) == 0) g_m.pia_irq_dirty = true;  // PADR/PBDR read may clear IRQ
                 break;
        case 5:  g_m.cpu->D = mc6821_read(g_m.pia1, A);
                 if ((A & 1) == 0) g_m.pia_irq_dirty = true;
                 break;
        case 6:  g_m.cpu->D = fdc_io_read(A); break;
        default: g_m.cpu->D = 0xFF; break;
        }
    } else {
        // Real CoCo writes route by raw address — S=7 covers SAM regs +
        // any RAM-region write (RAS/nWE handles RAM in parallel).
        if ((A & 0xFFE0) == 0xFF00) {
            mc6821_write(g_m.pia0, A, g_m.cpu->D);
            if (A & 1) g_m.pia_irq_dirty = true;       // CRA/CRB write may change IRQ enable
        } else if ((A & 0xFFE0) == 0xFF20) {
            mc6821_write(g_m.pia1, A, g_m.cpu->D);
            if (A & 1) g_m.pia_irq_dirty = true;
            audio_update_level();   // DAC / single-bit may have changed -> recache
        }
        else if ((A & 0xFFE0) == 0xFF40) fdc_io_write(A, g_m.cpu->D);  // cart I/O
        else if (A < 0x8000)             g_m.ram[A] = g_m.cpu->D;
        else if (g_m.sam_ty && A < 0xFF00) g_m.ram[A] = g_m.cpu->D;  // all-RAM mode
        // ROM and other I/O writes ignored
    }

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = micros(); prof_data_us += (uint32_t)(pt1 - pt0); pt0 = pt1; }
#endif

    // AMOLED-30 fast path: ~96% of memory cycles have no event due, so
    // skip the function-call overhead and just advance the tick counter.
    // Only enter event_run_queue when something is actually pending.
    event_ticks new_tick = event_current_tick + ncycles;
    struct event *head = machine_event_list_global->events;
    if (head && event_tick_delta(new_tick, head->at_tick) >= 0) {
        event_run_queue(machine_event_list_global, ncycles);
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
    { uint32_t pt1 = micros(); prof_events_us += (uint32_t)(pt1 - pt0); pt0 = pt1; }
#endif

    if (g_m.pia_irq_dirty) {
        MC6809_IRQ_SET(g_m.cpu,  g_m.pia0->a.irq || g_m.pia0->b.irq);
        MC6809_FIRQ_SET(g_m.cpu, g_m.pia1->a.irq || g_m.pia1->b.irq);
        g_m.pia_irq_dirty = false;
    }

    g_m.cycles_remaining -= ncycles;
    if (g_m.cycles_remaining <= 0) g_m.cpu->running = 0;

#if PROFILE_MEM_CYCLE
    { uint32_t pt1 = micros(); prof_irq_us += (uint32_t)(pt1 - pt0); }
    prof_calls++;
#endif
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
    // (no write has happened) — matches DECB text-mode default.
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
    // AMOLED-57: core 1 owns vdg_buffer now (coco_machine_render_frame).
    // xroar's render_scanline is suppressed, so the pixel_data this
    // callback would pack is stale — skip it entirely to avoid racing
    // core 1's renderer.
    (void)data;
    return;
#endif
    // render_line fires once per scanline including borders. Wrap to the
    // VDG's per-frame scanline count so each new frame starts re-capturing
    // active rows 0..191 — without the wrap, the row index races off into
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

// - - - init / run ------------------------------------------------

extern "C" _Bool coco_machine_init(const uint8_t *rom, size_t rom_len) {
    if (rom_len != 16384) return 0;
    g_m.rom = rom;
    g_m.rom_len = rom_len;

    g_m.ram = (uint8_t *)rp_mem_malloc(64 * 1024);
    if (!g_m.ram) return 0;
    memset(g_m.ram, 0, 64 * 1024);
    memset(g_m.vdg_buffer, 0, sizeof(g_m.vdg_buffer));

    // Event list MUST exist before any part_create() that uses MACHINE_EVENT_LIST.
    machine_event_list_global = event_list_new();
    if (!machine_event_list_global) return 0;
    event_current_tick = 0;

    struct part *p;

    p = part_create("MC6809", NULL);
    if (!p) return 0;
    g_m.cpu = (struct MC6809 *)p;
    g_m.cpu->mem_cycle = DELEGATE_AS2(void, bool, uint16, coco_mem_cycle, g_m.cpu);

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
    // emulator targets) ship the T1 — it adds lowercase via the inverse
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

// AMOLED-38: link-time stub for the unused original-6847 font. The
// !is_t1 branch in mc6847.c:427 still references font_6847[] even though
// it's dead at runtime (we always pick the T1 variant). We exclude the
// real 1.5 KB font-6847.c from the build and provide this 1-byte
// placeholder so the linker is satisfied. Reaching this array would mean
// is_t1 got flipped to false — that's a configuration bug, not a data
// lookup, so a single zero is fine.
extern "C" const uint8_t font_6847[1] = { 0 };

// - - - audio (PIZERO-18) -----------------------------------------
//
// This stripped port vendors no XRoar sound module, so we synthesise the CoCo
// audio stream here. The 6-bit DAC sits on PIA1 port A bits 2..7 and the
// single-bit sound on PIA1 port B bit 1. We sample the effective pin state at
// a fixed COCO_AUDIO_RATE by slicing the CPU run into sample-sized cycle
// chunks (below), which yields correct PITCH because emulation time advances
// by exactly the right number of cycles between samples. Output lands in a
// small overwrite ring drained by whatever sink is active (WAV-over-CDC today;
// HDMI data islands later). CPU clock = EVENT_TICK_RATE/16 = 14318180/16 =
// 894886.25 Hz; the Bresenham uses the rates x4 so the divisor is exact and
// pitch does not drift.
#define COCO_AUDIO_RATE      48000u   // PIZERO-30: 48 kHz native (ACR CTS now correct)
#define COCO_AUDIO_CPUCLK_X4 3579545u                 // (14318180/16)*4, exact
#define COCO_AUDIO_RATE_X4   (COCO_AUDIO_RATE * 4u)    // 128000
#define AUDIO_RING_SAMPLES   2048u                     // pow2; ~64 ms @ 32 kHz, 4 KB

static int16_t           g_audio_ring[AUDIO_RING_SAMPLES];
static volatile uint32_t g_audio_w = 0;                // monotonic write index
static volatile uint32_t g_audio_r = 0;                // monotonic read index
static uint32_t          g_audio_err = 0;              // Bresenham accumulator

// Event-driven, band-limited audio resampler (integrate-and-dump). Rather than
// point-sampling the DAC at 32 kHz (which aliases the square wave's edges into
// inharmonic "fuzz" -- the edges snap to the sample grid), we INTEGRATE the DAC
// level over each 32 kHz sample period using the exact event-tick timestamps.
// g_dac_level is updated the instant the CPU writes the DAC (PIA1 PA/PB) and
// audio_integrate() accumulates level*ticks per memory access; at each sample
// boundary (Bresenham on the 14.3181 MHz tick clock) we emit the time-average.
// This captures edge timing to ~1 memory access (~16 ticks) instead of a whole
// 447-tick sample, removing the jitter aliasing at the source. A gentle 2-pole
// IIR then matches the OEM CoCo's TV-bandwidth sound.
#define EVENT_TICK_HZ        14318180u                 // event ticks per second (16/CPU cycle)
static int32_t  g_dac_level = 0;                       // current DAC+1bit level (set on PIA1 write)
static int32_t  g_aud_acc   = 0;                       // integral of level*ticks this sample
static uint32_t g_aud_tk    = 0;                       // ticks accumulated this sample
static uint32_t g_aud_terr  = 0;                       // Bresenham accumulator (ticks*RATE)
static int32_t  g_lp1 = 0, g_lp2 = 0;                  // 2-pole TV-bandwidth LPF state

// Recompute the cached audio level from PIA1 (called right after a PIA1 write).
// Output gain. The 6-bit DAC swings 64 steps; a square wave's RMS == its peak,
// so this is loud even well below full scale. AUDIO_DAC_GAIN sets the per-step
// amplitude (was 480 -> ~47% FS and harsh). 240 -> ~24% FS (-6 dB); lower this
// to make it quieter, raise it for louder.
#define AUDIO_DAC_GAIN   240
static inline void audio_update_level(void) {
    if (!g_m.pia1) return;
    int dac6 = (PIA_VALUE_A(g_m.pia1) >> 2) & 0x3F;    // 6-bit DAC, 0..63
    // NB: the real CoCo single-bit sound is PIA1 CB2, not PB1 -- the old PB1 tap
    // was bogus and only injected a constant DC offset, so it's dropped.
    g_dac_level = (dac6 - 32) * AUDIO_DAC_GAIN;        // ~ -7680..+7440, centered
}

static inline void audio_emit(int s) {
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    // PIZERO-30: output LPF BYPASSED by default -- the integrate-and-dump already
    // band-limits the square at the source, so the extra IIR was just dulling the
    // tone vs desktop xroar. Build with -DAUDIO_OUTPUT_LPF to re-enable it.
#ifdef AUDIO_OUTPUT_LPF
    g_lp1 += (((int32_t)s    - g_lp1) * 14) >> 4;
    g_lp2 += (((int32_t)g_lp1 - g_lp2) * 14) >> 4;
    s = (int)g_lp2;
#else
    (void)g_lp1; (void)g_lp2;
#endif
    uint32_t w = g_audio_w;
    g_audio_ring[w & (AUDIO_RING_SAMPLES - 1)] = (int16_t)s;
    g_audio_w = w + 1;
    if (g_audio_w - g_audio_r > AUDIO_RING_SAMPLES)    // overwrite-on-full
        g_audio_r = g_audio_w - AUDIO_RING_SAMPLES;
}

// Called per memory access with the access duration in event ticks. Integrates
// the (piecewise-constant) DAC level and emits 32 kHz samples on tick boundaries.
// When a sample boundary falls inside this access we split EXACTLY at it (the
// ticks past the boundary carry to the next sample) so each emitted sample is
// the true time-average over its period -- no edge jitter, no residual aliasing.
static inline void audio_integrate(uint32_t ticks) {
    g_aud_acc  += g_dac_level * (int32_t)ticks;
    g_aud_tk   += ticks;
    g_aud_terr += ticks * COCO_AUDIO_RATE;
    if (g_aud_terr >= EVENT_TICK_HZ) {
        g_aud_terr -= EVENT_TICK_HZ;
        uint32_t over_tk  = g_aud_terr / COCO_AUDIO_RATE;     // ticks of this access past the boundary
        int32_t  over_val = g_dac_level * (int32_t)over_tk;   // ...belong to the next sample
        int32_t  acc = g_aud_acc - over_val;
        uint32_t tk  = g_aud_tk  - over_tk;
        int s = tk ? (int)(acc / (int32_t)tk) : g_dac_level;
        audio_emit(s);
        g_aud_acc = over_val;
        g_aud_tk  = over_tk;
    }
}

// Drain up to `max` mono int16 samples; returns count read.
extern "C" size_t coco_machine_audio_read(int16_t *dst, size_t max) {
    size_t n = 0;
    while (n < max && g_audio_r != g_audio_w) {
        dst[n++] = g_audio_ring[g_audio_r & (AUDIO_RING_SAMPLES - 1)];
        g_audio_r++;
    }
    return n;
}

extern "C" uint32_t coco_machine_audio_rate(void) { return COCO_AUDIO_RATE; }

// Audio is event-driven from the memory-access path; run the whole budget in one
// cpu->run. (The VDG already drives field-sync at 60 Hz, so we do NOT regenerate
// it here -- doing so double-drove PIA0 CB1.)
static inline void run_cpu_with_audio(uint32_t cycles) {
    g_m.cycles_remaining = (int32_t)cycles * 16;
    g_m.cpu->running = 1;
    g_m.cpu->run(g_m.cpu);
}

extern "C" void coco_machine_run_cycles(uint32_t cycles) {
    if (!g_m.cpu) return;

    // Cartridge autoboot: real CoCo cart-FIRQ carts pulse PIA1 CB1 from the
    // Q clock to nudge BASIC into the FIRQ handler that JMPs $C000. We
    // approximate per upstream xroar by toggling every ~100 ms (~88 950
    // CPU cycles at 0.895 MHz).
    if (g_m.cart_rom && g_m.pia1) {
        g_m.cart_toggle_remaining -= (int32_t)cycles;
        while (g_m.cart_toggle_remaining <= 0) {
            g_m.cart_cb1_level = !g_m.cart_cb1_level;
            mc6821_set_cx1(&g_m.pia1->b, g_m.cart_cb1_level);
            g_m.pia_irq_dirty = true;
            g_m.cart_toggle_remaining += 88950;
        }
    }

    // PIZERO-30: keep the 60 Hz field-sync timer IRQ alive. The VDG drives FS ->
    // PIA0 CB1, but this port's autorun DIRECT-jumps into programs that can leave
    // PIA0 CB1's interrupt DISABLED (CRB bit 0 = 0); nothing re-enables it, so
    // BASIC's TIMER freezes and PLAY / cursor-blink / SOUND-duration hang forever.
    // On real hardware the FS line is wired and BASIC keeps this on, so force the
    // enable bit each frame. (Touches only bit 0 -- CB2/mux and edge bits intact.)
    if (g_m.pia0) g_m.pia0->b.control_register |= 0x01;

    run_cpu_with_audio(cycles);

#if PROFILE_MEM_CYCLE
    // AMOLED-52: per-second per-region breakdown.
    uint32_t now_ms = millis();
    if (now_ms - prof_last_log_ms >= 1000) {
        uint64_t total = prof_sam_us + prof_data_us + prof_events_us + prof_irq_us;
        if (total > 0 && prof_calls > 0) {
            Serial.printf("[prof] calls=%lu  sam=%lu (%lu%%)  data=%lu (%lu%%)  events=%lu (%lu%%)  irq=%lu (%lu%%)  total=%lu us\n",
                (unsigned long)prof_calls,
                (unsigned long)prof_sam_us,    (unsigned long)((prof_sam_us    * 100) / total),
                (unsigned long)prof_data_us,   (unsigned long)((prof_data_us   * 100) / total),
                (unsigned long)prof_events_us, (unsigned long)((prof_events_us * 100) / total),
                (unsigned long)prof_irq_us,    (unsigned long)((prof_irq_us    * 100) / total),
                (unsigned long)total);
        }
        prof_sam_us = prof_data_us = prof_events_us = prof_irq_us = 0;
        prof_calls = 0;
        prof_last_log_ms = now_ms;
    }
#endif
}

extern "C" void coco_machine_install_cart(const uint8_t *rom8k) {
    g_m.cart_rom = rom8k;
    g_m.cart_toggle_remaining = 88950;
    g_m.cart_cb1_level = true;
}

extern "C" const uint8_t *coco_machine_get_vdg_buffer(void) {
    return g_m.vdg_buffer;
}

// AMOLED-57 phase 2: core-1 VDG renderer. Reads PIA1 PB mode bits +
// SAM F register + CoCo RAM, generates one frame's worth of palette
// indices into g_m.vdg_buffer. Called from loop1 in place of core 0's
// per-scanline render path (which is suppressed by SUPPRESS_RENDER_SCANLINE).
//
// MVP modes:
//   * ALPHA  (PB7 = 0)         — character glyph via font_6847t1, green-on-black
//   * RG6    (PB7=1, GM=111)   — 1-bit per pixel, white-on-black
//
// Deferred: SG4/SG6 (semigraphics), CG1-6 (color graphics), artifact LUT
// for RG6 (AMOLED-22). Those modes show black until implemented.
//
// Palette indices match g_vdg_rgb565[] in coco_boot.cpp:
//   0=GREEN  4=WHITE  8=BLACK  etc.

extern "C" const uint8_t font_6847t1[];  // 128 chars × 12 rows = 1.5 KB

// Palette indices — match g_vdg_rgb565[] in coco_boot.cpp.
#define PAL_GREEN       0
#define PAL_YELLOW      1
#define PAL_BLUE        2
#define PAL_RED         3
#define PAL_WHITE       4
#define PAL_CYAN        5
#define PAL_MAGENTA     6
#define PAL_ORANGE      7
#define PAL_BLACK       8
#define PAL_DARK_GREEN  9

// Lines-per-row for each graphics GM (vertical replication). GM bit0
// also selects RG (1 bit/pixel, 2 colours) vs CG (2 bits/pixel, 4).
static const uint8_t GM_nLPR[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };

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

static void build_alpha_lut() {
    const uint8_t combos[2][2] = { { PAL_GREEN, PAL_BLACK }, { PAL_BLACK, PAL_GREEN } };
    for (int c = 0; c < 2; c++) {
        uint8_t ink = combos[c][0], paper = combos[c][1];
        for (int g = 0; g < 256; g++) {
            uint32_t word = 0;
            for (int bit = 0; bit < 8; bit++) {
                uint8_t color = (g & (0x80 >> bit)) ? ink : paper;
                int byte = bit >> 1, shift = (bit & 1) ? 4 : 0;   // low nibble = even px
                word |= (uint32_t)color << (byte * 8 + shift);
            }
            g_alpha_lut[c][g] = word;
        }
    }
    g_alpha_lut_ready = true;
}

static void HOT_FUNC(render_alpha_frame)(uint16_t base) {
    if (!g_alpha_lut_ready) build_alpha_lut();
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
                    uint8_t color = (ch >> 4) & 7;
                    uint8_t sg = (sub_row < 6) ? (ch >> 2) : ch;
                    uint8_t left  = (sg & 2) ? color : PAL_BLACK;
                    uint8_t right = (sg & 1) ? color : PAL_BLACK;
                    int basepx = col * 8;
                    for (int bit = 0; bit < 8; bit++)
                        put2(dst, basepx + bit, (bit < 4) ? left : right);
                } else {
                    // Alpha: 6-bit screen code → T1 font glyph $40-$7F.
                    // Bit 6 = inverse (the iconic black-on-green BASIC look).
                    uint8_t glyph = font_6847t1[(((ch & 0x3F) | 0x40)) * 12 + sub_row];
                    *(uint32_t *)(dst + col * 4) = g_alpha_lut[(ch >> 6) & 1][glyph];
                }
            }
        }
    }
}

static void HOT_FUNC(render_rg6_frame)(uint16_t base) {
    // 32 bytes × 8 bits = 256 pixels per scanline × 192 scanlines.
    if (g_artifact_active) {
        // AMOLED-22 NTSC artifact colours: PMODE 4 (RG6, PB[7:4]=1111)
        // produces colour from adjacent bit PAIRS. CSS picks the pair.
        //   00→BLACK  01→c01  10→c10  11→WHITE
        const uint8_t c01 = g_artifact_css ? PAL_ORANGE : PAL_BLUE;
        const uint8_t c10 = g_artifact_css ? PAL_BLUE  : PAL_ORANGE;
        for (int row = 0; row < COCO_VDG_H; row++) {
            const uint8_t *src = &g_m.ram[(base + row * 32) & 0xFFFF];
            uint8_t *dst = &g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
            int px = 0;
            for (int byte = 0; byte < 32; byte++) {
                uint8_t b = src[byte];
                for (int pair = 0; pair < 4; pair++) {
                    uint8_t bits = (b >> 6) & 3;  // leftmost two bits
                    b <<= 2;
                    uint8_t color = (bits == 0) ? PAL_BLACK
                                  : (bits == 1) ? c01
                                  : (bits == 2) ? c10
                                  : PAL_WHITE;
                    put2(dst, px++, color);   // each colour-clock spans
                    put2(dst, px++, color);   // two mono pixels
                }
            }
        }
        return;
    }
    // Plain monochrome RG6 (white on black).
    for (int row = 0; row < COCO_VDG_H; row++) {
        const uint8_t *src = &g_m.ram[(base + row * 32) & 0xFFFF];
        uint8_t *dst = &g_m.vdg_buffer[row * (COCO_VDG_W / 2)];
        for (int byte = 0; byte < 32; byte++) {
            uint8_t b = src[byte];
            for (int bit = 0; bit < 8; bit++) {
                int px = byte * 8 + bit;
                uint8_t color = (b & (0x80 >> bit)) ? PAL_WHITE : PAL_BLACK;
                put2(dst, px, color);
            }
        }
    }
}

// AMOLED-60: general color/resolution-graphics renderer for GM 0-6.
// (RG6/GM7 is handled by render_rg6_frame for its NTSC-artifact path.)
// Reads bytes_per_row bytes per data row, expands to 256 logical pixels
// with horizontal replication, and repeats each data row nLPR display
// scanlines. RG = 1 bit/pixel fg/bg; CG = 2 bits/pixel, colour =
// cg_base + value.
static void HOT_FUNC(render_graphics_frame)(uint16_t base, uint8_t gm, bool css) {
    const bool is_32       = (gm == 2 || gm == 4 || gm == 6);
    const int  bytes_per_row = is_32 ? 32 : 16;
    const int  nlpr        = GM_nLPR[gm];
    const bool rg          = gm & 1;
    const int  data_rows   = COCO_VDG_H / nlpr;          // 64/96/192

    const uint8_t cg_base = css ? PAL_WHITE : PAL_GREEN; // CG colour set
    const uint8_t fg = css ? PAL_WHITE : PAL_GREEN;      // RG foreground
    const uint8_t bg = css ? PAL_BLACK : PAL_DARK_GREEN; // RG background

    // Source pixels produced per row before horizontal replication:
    //   RG → 8 per byte, CG → 4 cells per byte.
    const int src_px = rg ? bytes_per_row * 8 : bytes_per_row * 4;
    const int hrep   = COCO_VDG_W / src_px;              // 1/2/4

    static uint8_t rowbuf[COCO_VDG_W];
    for (int drow = 0; drow < data_rows; drow++) {
        const uint8_t *p = &g_m.ram[(base + drow * bytes_per_row) & 0xFFFF];
        int px = 0;
        for (int byte = 0; byte < bytes_per_row; byte++) {
            uint8_t b = p[byte];
            if (rg) {
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t c = (b & (0x80 >> bit)) ? fg : bg;
                    for (int r = 0; r < hrep; r++) rowbuf[px++] = c;
                }
            } else {
                for (int cell = 0; cell < 4; cell++) {
                    uint8_t c = cg_base + ((b >> 6) & 3);
                    b <<= 2;
                    for (int r = 0; r < hrep; r++) rowbuf[px++] = c;
                }
            }
        }
        for (int rep = 0; rep < nlpr; rep++) {
            int disp = drow * nlpr + rep;
            if (disp >= COCO_VDG_H) break;
            uint8_t *dst = &g_m.vdg_buffer[disp * (COCO_VDG_W / 2)];
            for (int x = 0; x < COCO_VDG_W; x += 2)
                dst[x >> 1] = rowbuf[x] | (rowbuf[x + 1] << 4);
        }
    }
}

extern "C" void HOT_FUNC(coco_machine_render_frame)(void) {
    if (!g_m.pia1) return;
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

extern "C" uint16_t coco_machine_get_pc(void) {
    return g_m.cpu ? g_m.cpu->reg_pc : 0;
}

extern "C" uint32_t coco_machine_get_total_mem_cycles(void) {
    return g_m.total_mem_cycles;
}

extern "C" uint32_t coco_machine_get_render_lines(void) {
    return g_m.render_lines;
}

extern "C" const uint8_t *coco_machine_peek_ram(uint16_t addr) {
    return &g_m.ram[addr];
}

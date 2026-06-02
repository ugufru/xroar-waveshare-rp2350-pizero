/*
 * coco_machine — the integration glue that turns the vendored XRoar
 * modules (mc6809, mc6883, mc6821, mc6847) into a runnable CoCo 1/2.
 *
 * Public surface is small on purpose: init, advance time by N CPU
 * cycles, read out the VDG palette buffer. Display, keyboard, audio,
 * media and SD wiring all live above this layer.
 */
#ifndef COCO_MACHINE_H_
#define COCO_MACHINE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COCO_VDG_W 256
#define COCO_VDG_H 192

/* Initialise the machine with a 16 KB ROM image (Color BASIC or
 * Color BASIC + Extended Color BASIC packed into the upper bank).
 * rom_len must be 16384. Returns true on success. */
_Bool coco_machine_init(const uint8_t *rom, size_t rom_len);

/* Install an 8 KB cartridge ROM at $C000-$DFFF (mirror to $E000-$FDFF).
 * Pass NULL to remove. Setting a cart also schedules the periodic CART
 * FIRQ pulse so autobooting carts (DECB) jump to $C000 on next reset. */
void coco_machine_install_cart(const uint8_t *rom8k);

/* Floppy disk callback. drive/track/sector identify the requested sector.
 * Write 256 bytes into `out256`. Return 0 on success, non-zero on error
 * (record not found). Sector numbers are 1-based per WD2797 convention. */
typedef int (*coco_disk_read_sector_fn)(unsigned drive, unsigned track,
                                        unsigned sector, uint8_t *out256);

/* Install (or remove with NULL) the disk-read callback used by the
 * cartridge-side FDC at $FF48-$FF4B. */
void coco_machine_install_disk_reader(coco_disk_read_sector_fn fn);

/* Directly write `len` bytes from `src` into emulator RAM starting at
 * `addr`. Bypasses the 6809 / SAM / TY logic — used by AMOLED-26 to
 * direct-load LOADM .bin segments without going through DECB. Wraps
 * around at 64 KB. */
void coco_machine_loadm_write(uint16_t addr, const uint8_t *src, uint16_t len);

/* Set the 6809's PC register to `entry`. Used after coco_machine_loadm_write
 * to simulate an EXEC. Be careful — the CPU might be mid-instruction; this
 * is intended to be called only when the machine is paused. */
void coco_machine_jump(uint16_t entry);


/* Run the CPU for approximately cycles 6809 cycles (1 cycle = 16
 * 14.31818 MHz ticks on slow cycle, less on fast). The VDG event
 * queue is pumped as a side-effect of CPU memory cycles. */
void coco_machine_run_cycles(uint32_t cycles);

/* Audio (PIZERO-18). As the machine runs, the CoCo 6-bit DAC + single-bit
 * sound are sampled at coco_machine_audio_rate() Hz (mono, signed 16-bit)
 * into an internal ring. Drain up to `max` samples into `dst`; returns the
 * number read (0 if none pending). Samples are overwritten oldest-first if a
 * sink falls behind, so a slow/absent consumer never blocks emulation. */
size_t coco_machine_audio_read(int16_t *dst, size_t max);
uint32_t coco_machine_audio_rate(void);

/* Pointer to the current VDG buffer — COCO_VDG_W * COCO_VDG_H bytes,
 * one palette index per pixel. Stable for the lifetime of the
 * machine; callers should not free or modify it. */
const uint8_t *coco_machine_get_vdg_buffer(void);

/* AMOLED-57 phase 2: regenerate vdg_buffer from current CoCo state
 * (PIA1 PB mode bits + SAM F register + RAM). Called from core 1
 * each frame in place of core 0's render_scanline, which we suppress
 * via SUPPRESS_RENDER_SCANLINE in this build. */
void coco_machine_render_frame(void);

/* Diagnostic accessors — useful for smoke tests. */
uint16_t coco_machine_get_pc(void);
uint32_t coco_machine_get_total_mem_cycles(void);
uint32_t coco_machine_get_render_lines(void);
const uint8_t *coco_machine_peek_ram(uint16_t addr);

/* Keyboard injection — press/release simulated keys by DSCAN_* code
 * (see lib/xroar_core/src/dkbd.h). The PIA0 port A pre-read hook resolves
 * the active row(s) from PIA0 port B at read time and pulls down the
 * matching column bits. All keys released initially. */
void coco_machine_press_key(uint8_t dscan);
void coco_machine_release_key(uint8_t dscan);
void coco_machine_release_all_keys(void);

#ifdef __cplusplus
}
#endif

#endif

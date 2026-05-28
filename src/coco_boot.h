// AMOLED-58: shared declarations for the SD boot helpers.
//
// All public coco_boot_* APIs that were previously just extern'd into
// main.cpp now live in this header, plus the typed-subdir resolver and
// autorun.txt loader. See AUTORUN.md for the file format spec.

#ifndef COCO_BOOT_H_
#define COCO_BOOT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Path resolver: try /coco/<subdir>/<name> first, then /coco/<name>.
// On success, fills `out` with the full FatFs path (prefixed "0:").
// `out_sz` must be ≥ 80. Returns true if a file was found.
bool coco_boot_resolve(const char *subdir, const char *name,
                       char *out, size_t out_sz);

// Find the first *.dsk in /coco/dsk/ then /coco/ alphabetically.
// Fills `out` with the full path on success.
bool coco_boot_find_default_dsk(char *out, size_t out_sz);

// Parsed autorun.txt. Bare-name fields (cart_name etc.) are "" if the
// corresponding directive was absent. `autotype` is a concatenation of
// non-comment non-directive lines, each terminated with '\r'.
struct coco_autorun {
    char cart_name[64];
    char disk_name[64];
    char direct_name[64];
    char autotype[1024];
};

// Read /coco/autorun.txt. Returns true if the file existed (whether or
// not it had valid content); false if the file is missing — caller
// should then proceed with defaults.
bool coco_boot_load_autorun(struct coco_autorun *out);

// System-ROM loader. Tries /coco/rom/{extbas11,bas12}.rom first, then
// /coco/{extbas11,bas12}.rom.
bool coco_boot_load_rom_from_sd(uint8_t *rom16k);

// Cart ROM loader. `name` is a bare filename; tried under /coco/rom/
// then /coco/. Pass "disk11.rom" for default Disk BASIC.
bool coco_boot_load_cart_named(const char *name, uint8_t *cart8k);

// Disk attach. `path` is the full resolved FatFs path (use
// coco_boot_resolve("dsk", name, ...) to build it).
bool coco_boot_attach_dsk(const char *path);

// Sector read for the WD2797 emulator (set up in coco_machine).
int coco_boot_disk_read_sector(unsigned drive, unsigned track,
                               unsigned sector, uint8_t *out256);

// LOADM .bin parser (AMOLED-26 direct-load path). `path` is full
// FatFs path.
typedef void (*coco_loadm_seg_cb)(uint16_t addr, const uint8_t *data,
                                  uint16_t len, void *ctx);
bool coco_boot_parse_loadm(const char *path,
                           coco_loadm_seg_cb cb, void *ctx,
                           uint16_t *entry_out);

// VDG blit helpers (unchanged, here for completeness so main.cpp only
// needs one header for coco_boot stuff).
void coco_boot_blit_vdg(uint16_t *fb);
void coco_boot_blit_vdg_src(const uint8_t *src, uint16_t *fb);
void coco_boot_blit_vdg_1to1(uint16_t *small_fb);

// PIZERO-07: native-RGB565, no-rotation blit centering CoCo 256x192 into a
// 320x240 framebuffer for libdvi (2x scaled to 640x480). See coco_boot.cpp.
void coco_boot_blit_vdg_pizero(uint16_t *fb);
void coco_boot_blit_vdg_pizero_src(const uint8_t *src, uint16_t *fb);

#ifdef __cplusplus
}
#endif

#endif

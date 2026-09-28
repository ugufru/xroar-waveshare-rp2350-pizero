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

// PIZERO-81a/136: rebuild the list the F12 overlay shows, for one kind
// (enum cat_kind in dsk_catalog.h). Returns the number of entries listed.
struct dsk_catalog;
int coco_boot_rescan(int kind);
const struct dsk_catalog *coco_boot_dsk_catalog(void);

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
// Cartridge loading (PIZERO-136/139/142): resolve_cart finds a name in
// /coco/cart, /coco/roms, then /coco; load_cart_path takes any 2/4/8/16 KB
// image no bigger than `max` (*len gets the size); load_file reads exactly
// len bytes, for a bank-switched image.
bool coco_boot_load_cart_path(const char *path, uint8_t *buf, uint32_t max, uint32_t *len);
bool coco_boot_resolve_cart(const char *name, char *out, size_t out_sz);   // PIZERO-142
bool coco_boot_load_file(const char *path, uint8_t *buf, uint32_t len);        // PIZERO-142

// PIZERO-145: /coco/settings.txt (format in settings.h). Bad lines are
// reported on serial and never fail the boot.
struct coco_settings;
bool coco_boot_load_settings(struct coco_settings *out);
// PIZERO-154: apply a settings-format file on top of *out (a game's NAME.TXT).
bool coco_boot_apply_settings_file(const char *path, struct coco_settings *out);

// PIZERO-146: text files for the on-screen editor. save_text writes PATH.tmp
// and swaps it in, so a power cut never leaves a half-written file;
// recover_text finishes a swap a power cut interrupted.
bool coco_boot_load_text(const char *path, char *buf, uint32_t max, uint32_t *len);
bool coco_boot_save_text(const char *path, const char *buf, uint32_t len);
void coco_boot_recover_text(const char *path);

// Disk attach. `path` is the full resolved FatFs path (use
// coco_boot_resolve("dsk", name, ...) to build it).
bool coco_boot_attach_dsk(const char *path);

// PIZERO-114: four drives. Mount replaces whatever is in the drive; eject
// empties it. drive_path is the mounted path, or NULL for an empty drive.
bool coco_boot_mount_drive(unsigned drive, const char *path);
void coco_boot_eject_drive(unsigned drive);
const char *coco_boot_drive_path(unsigned drive);

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

/* PIZERO-92: a 32x16 text card in the machine's own font and palette, for
 * diagnostics that must be readable on a TV with no serial console. Costs 512
 * bytes of character grid; renders straight into the framebuffer. */
void coco_boot_card_clear(void);
void coco_boot_card_text(int col, int row, const char *s);
void coco_boot_card_center(int row, const char *s);
void coco_boot_card_present(uint16_t *fb);
void coco_boot_card_invert_row(int row);   // PIZERO-81b: selection bar
void coco_boot_card_invert_cell(int col, int row);   // PIZERO-146: editor cursor
int  coco_boot_card_wrap(int col, int row, int width, int max_rows, const char *s);

/* PIZERO-92: what the last ROM load actually found, so a diagnostic can tell a
 * MISSING file from a DAMAGED one. Valid after coco_boot_load_rom_from_sd. */
struct coco_rom_status {
    bool     bas_found;      /* bas12.rom resolved on the card */
    uint32_t bas_bytes;      /* bytes read; 8192 is correct */
    bool     ecb_found;      /* extbas11.rom resolved */
    uint32_t ecb_bytes;
};
const struct coco_rom_status *coco_boot_rom_status(void);

#ifdef __cplusplus
}
#endif

#endif
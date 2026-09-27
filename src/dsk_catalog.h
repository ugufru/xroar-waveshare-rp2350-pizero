// dsk_catalog.h — the list of disk images the F12 overlay offers (PIZERO-81a).
//
// Pure logic, no FatFs: which directory entries count as a disk image, how
// the list is ordered and capped, and how an entry turns back into a path.
// The directory walk itself is in coco_boot.cpp (coco_boot_rescan_dsk); this
// half is split out so it can be tested on the host, like text_card.h.
//
// Rules, most of them learned by the Fruit Jam port (src/coco/coco_main.cpp
// scan_dsk_dir):
//   * only *.dsk, case-insensitive; directories and dotfiles skipped, which
//     includes the macOS "._NAME.DSK" AppleDouble files that look like disks
//   * a name too long to store is SKIPPED, not truncated, because a truncated
//     name cannot be opened again (FRUITJAM-103); the count is kept so the
//     overlay can say so rather than silently hide a file
//   * /coco/dsk is searched before /coco, matching coco_boot_resolve, and a
//     name found in both is listed once, from /coco/dsk
//   * sorted case-insensitively, capped at DSK_CAT_MAX

#ifndef DSK_CATALOG_H
#define DSK_CATALOG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define DSK_CAT_MAX      128
#define DSK_NAME_MAX     64     // bytes including the terminator

enum { DSK_DIR_DSK = 0, DSK_DIR_ROOT = 1 };   // /coco/dsk, /coco

struct dsk_entry {
    char    name[DSK_NAME_MAX];
    uint8_t dir;                 // DSK_DIR_*
};

struct dsk_catalog {
    struct dsk_entry e[DSK_CAT_MAX];
    int n;
    int skipped_long;            // names that did not fit DSK_NAME_MAX
    int skipped_full;            // images beyond DSK_CAT_MAX
};

static inline void dsk_cat_clear(struct dsk_catalog *c) {
    c->n = 0;
    c->skipped_long = 0;
    c->skipped_full = 0;
}

// Does this directory entry name a disk image? Length is judged separately
// by dsk_cat_add, so an over-long image is counted rather than ignored.
static inline bool dsk_cat_is_image(const char *fname, bool is_dir) {
    if (!fname || !fname[0] || is_dir) return false;
    if (fname[0] == '.') return false;
    const char *ext = strrchr(fname, '.');
    return ext && ext != fname && strcasecmp(ext, ".dsk") == 0;
}

static inline int dsk_cat_find_name(const struct dsk_catalog *c, const char *name) {
    for (int i = 0; i < c->n; i++)
        if (strcasecmp(c->e[i].name, name) == 0) return i;
    return -1;
}

// Add one directory entry. Returns true if it was listed. Call for /coco/dsk
// before /coco so the duplicate rule keeps the /coco/dsk copy.
static inline bool dsk_cat_add(struct dsk_catalog *c, const char *fname,
                               bool is_dir, uint8_t dir) {
    if (!dsk_cat_is_image(fname, is_dir)) return false;
    if (strlen(fname) >= DSK_NAME_MAX) { c->skipped_long++; return false; }
    if (dsk_cat_find_name(c, fname) >= 0) return false;
    if (c->n >= DSK_CAT_MAX) { c->skipped_full++; return false; }
    struct dsk_entry *e = &c->e[c->n++];
    strcpy(e->name, fname);
    e->dir = dir;
    return true;
}

// Insertion sort: the list is at most 128 short names, filled once per scan.
static inline void dsk_cat_sort(struct dsk_catalog *c) {
    for (int i = 1; i < c->n; i++) {
        struct dsk_entry t = c->e[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(c->e[j].name, t.name) > 0) {
            c->e[j + 1] = c->e[j];
            j--;
        }
        c->e[j + 1] = t;
    }
}

// The FatFs path for entry i. Returns false if it does not fit.
static inline bool dsk_cat_path(const struct dsk_catalog *c, int i,
                                char *out, size_t out_sz) {
    if (i < 0 || i >= c->n || !out) return false;
    int len = snprintf(out, out_sz, c->e[i].dir == DSK_DIR_DSK
                                        ? "0:/coco/dsk/%s" : "0:/coco/%s",
                       c->e[i].name);
    return len > 0 && (size_t)len < out_sz;
}

// The entry whose path this is, or -1. Lets the overlay put its cursor on
// whatever is already mounted, including a disk autorun.txt chose at boot.
static inline int dsk_cat_find_path(const struct dsk_catalog *c, const char *path) {
    char buf[DSK_NAME_MAX + 16];
    for (int i = 0; i < c->n; i++)
        if (dsk_cat_path(c, i, buf, sizeof buf) && strcasecmp(buf, path) == 0)
            return i;
    return -1;
}

// The name as the overlay shows it: every entry is a .dsk, so the extension
// is dropped to leave more of the name on a 32-column screen. Clipped to
// `width` characters.
static inline void dsk_cat_display_name(const char *name, char *out, int width) {
    int len = (int)strlen(name);
    const char *ext = strrchr(name, '.');
    if (ext && strcasecmp(ext, ".dsk") == 0) len = (int)(ext - name);
    if (len > width) len = width;
    memcpy(out, name, (size_t)len);
    out[len] = '\0';
}

#endif  // DSK_CATALOG_H

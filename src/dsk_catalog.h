// dsk_catalog.h: the lists the F12 overlay offers (PIZERO-81a, PIZERO-136).
//
// One list at a time, of one kind: disk images (DSK), machine-language
// programs (BIN) or cartridge ROMs (CART). Left/right in the overlay switch
// kind and rescan, so only one list is ever held in RAM.
//
// Pure logic, no FatFs: which directory entries count as a disk image, how
// the list is ordered and capped, and how an entry turns back into a path.
// The directory walk itself is in coco_boot.cpp (coco_boot_rescan_dsk); this
// half is split out so it can be tested on the host, like text_card.h.
//
// Rules, most of them learned by the Fruit Jam port (src/coco/coco_main.cpp
// scan_dsk_dir):
//   * by kind: *.dsk; *.bin; *.ccc (the cartridge extension; .rom is left
//     to the machine's own ROMs). Case-insensitive; directories and dotfiles
//     skipped, which includes the macOS "._NAME.DSK" AppleDouble files
//   * a name too long to store is SKIPPED, not truncated, because a truncated
//     name cannot be opened again (FRUITJAM-103); the count is kept so the
//     overlay can say so rather than silently hide a file
//   * the kind's own folder (/coco/dsk, /coco/bin, /coco/cart) is searched
//     before /coco, matching coco_boot_resolve, and a name found in both is
//     listed once, from the kind's folder
//   * sorted case-insensitively, capped at DSK_CAT_MAX

#ifndef DSK_CATALOG_H
#define DSK_CATALOG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// 128 entries (~8 KB). The double-buffered firmware builds (no
// AV_DATA_ISLAND or FB_SINGLE) carry two framebuffers and have no RAM to
// spare, so they list 64. Decided here, in the one header every user
// includes, so the struct is the same size in every translation unit.
#ifndef DSK_CAT_MAX
#if defined(ARDUINO) && !defined(AV_DATA_ISLAND) && !defined(FB_SINGLE)
#define DSK_CAT_MAX      64
#else
#define DSK_CAT_MAX      128
#endif
#endif
#define DSK_NAME_MAX     64     // bytes including the terminator

enum cat_kind { CAT_DSK = 0, CAT_BIN, CAT_CART, CAT_KINDS };
// PIZERO-296: the FILES list, every text file under /coco. Not one of the
// launcher kinds (it sits where the overlay's FILES list does, after them).
// Its names are paths relative to /coco ("README.md", "dsk/ORBIT.TXT"),
// always with dir DSK_DIR_ROOT, so dsk_cat_path gives "0:/coco/<name>".
#define CAT_TEXT CAT_KINDS
enum { DSK_DIR_DSK = 0, DSK_DIR_ROOT = 1 };   // the kind's folder, then /coco
// PIZERO-309: in the FILES list, a file inside the disk in drive d has dir
// DSK_DIR_DRIVE0 + d and a name like "README.TXT:1". It has no card path.
#define DSK_DIR_DRIVE0 2

// The kind's own folder; dir DSK_DIR_ROOT is always /coco.
static inline const char *cat_kind_dir(int kind) {
    return kind == CAT_BIN ? "0:/coco/bin" : kind == CAT_CART ? "0:/coco/cart"
                                                              : "0:/coco/dsk";
}

struct dsk_entry {
    char    name[DSK_NAME_MAX];
    uint8_t dir;                 // DSK_DIR_*
};

struct dsk_catalog {
    int kind;                    // enum cat_kind
    struct dsk_entry e[DSK_CAT_MAX];
    int n;
    int skipped_long;            // names that did not fit DSK_NAME_MAX
    int skipped_full;            // images beyond DSK_CAT_MAX
};

static inline void dsk_cat_clear(struct dsk_catalog *c, int kind) {
    c->kind = kind;
    c->n = 0;
    c->skipped_long = 0;
    c->skipped_full = 0;
}

// Does this directory entry belong in a list of this kind? Length is judged
// separately by dsk_cat_add, so an over-long name is counted, not ignored.
static inline bool cat_accepts(int kind, const char *fname, bool is_dir) {
    if (!fname || !fname[0] || is_dir) return false;
    if (fname[0] == '.') return false;
    const char *ext = strrchr(fname, '.');
    if (!ext || ext == fname) return false;
    switch (kind) {
    case CAT_DSK: return strcasecmp(ext, ".dsk") == 0;
    case CAT_BIN: return strcasecmp(ext, ".bin") == 0;
    case CAT_CART: return strcasecmp(ext, ".ccc") == 0;
    default: return false;
    }
}

static inline bool dsk_cat_is_image(const char *fname, bool is_dir) {
    return cat_accepts(CAT_DSK, fname, is_dir);
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
    if (!cat_accepts(c->kind, fname, is_dir)) return false;
    if (strlen(fname) >= DSK_NAME_MAX) { c->skipped_long++; return false; }
    if (dsk_cat_find_name(c, fname) >= 0) return false;
    if (c->n >= DSK_CAT_MAX) { c->skipped_full++; return false; }
    struct dsk_entry *e = &c->e[c->n++];
    strcpy(e->name, fname);
    e->dir = dir;
    return true;
}

// Order: case-insensitive by name. In the FILES list (PIZERO-296) the files
// in /coco itself come before those in its folders.
static inline int dsk_cat_cmp(int kind, const char *a, const char *b) {
    if (kind == CAT_TEXT) {
        int sa = strchr(a, '/') != NULL, sb = strchr(b, '/') != NULL;
        if (sa != sb) return sa - sb;
    }
    return strcasecmp(a, b);
}

// PIZERO-312: add the image a drive holds, by its path, before the folders
// are scanned, so a mounted disk is always in the list however full the card
// is. Only paths in the kind's folder or /coco itself can be listed (the
// list keeps a name and which of those two folders); any other is left out,
// as before. The scan that follows skips it as a duplicate.
static inline bool dsk_cat_add_path(struct dsk_catalog *c, const char *path) {
    if (!path) return false;
    const char *dir = cat_kind_dir(c->kind);
    size_t dl = strlen(dir), rl = strlen("0:/coco");
    if (!strncasecmp(path, dir, dl) && path[dl] == '/' && !strchr(path + dl + 1, '/'))
        return dsk_cat_add(c, path + dl + 1, false, DSK_DIR_DSK);
    if (!strncasecmp(path, "0:/coco", rl) && path[rl] == '/' && !strchr(path + rl + 1, '/'))
        return dsk_cat_add(c, path + rl + 1, false, DSK_DIR_ROOT);
    return false;
}

// Insertion sort: the list is at most 128 short names, filled once per scan.
static inline void dsk_cat_sort(struct dsk_catalog *c) {
    for (int i = 1; i < c->n; i++) {
        struct dsk_entry t = c->e[i];
        int j = i - 1;
        while (j >= 0 && dsk_cat_cmp(c->kind, c->e[j].name, t.name) > 0) {
            c->e[j + 1] = c->e[j];
            j--;
        }
        c->e[j + 1] = t;
    }
}

// ---- PIZERO-296: the FILES list ----
//
// Every text file in /coco and its folders one level down, judged by content
// (decb_files.h's decb_is_text, read by the caller). This half decides which
// names are even worth opening, and how they are listed.

// Files the FILES list leaves out by name: the two it always lists first
// (settings.txt, autorun.txt), drives.txt (the board rewrites it whenever a
// drive changes, so a hand edit would be lost until PIZERO-293 re-reads it),
// a save's leftover .tmp, and extensions that are never text, so the scan
// does not open every disk image to find out.
static inline bool text_cat_skip(const char *sub, const char *leaf) {
    if (!leaf || !leaf[0] || leaf[0] == '.') return true;
    if (!sub || !sub[0]) {
        if (!strcasecmp(leaf, "settings.txt") || !strcasecmp(leaf, "autorun.txt") ||
            !strcasecmp(leaf, "drives.txt")) return true;
    }
    const char *ext = strrchr(leaf, '.');
    if (!ext || ext == leaf) return false;
    static const char *const binary[] = { ".tmp", ".dsk", ".bin", ".ccc", ".rom", ".png",
                                          ".zip", ".cas", ".wav", ".pal", ".fnt", ".uf2" };
    for (unsigned i = 0; i < sizeof binary / sizeof binary[0]; i++)
        if (!strcasecmp(ext, binary[i])) return true;
    return false;
}

// Folders under /coco the scan does not enter: the ROMs and screen dumps
// are never text.
static inline bool text_cat_skip_dir(const char *name) {
    return !name || !name[0] || name[0] == '.' || !strcasecmp(name, "roms") ||
           !strcasecmp(name, "screendumps");
}

// Add a text file found in /coco/<sub> (sub "" for /coco itself). The caller
// has already checked text_cat_skip and the content. A path too long to
// store is counted, not truncated, as in dsk_cat_add.
static inline bool text_cat_add(struct dsk_catalog *c, const char *sub, const char *leaf) {
    char rel[DSK_NAME_MAX + 16];
    int len = (sub && sub[0]) ? snprintf(rel, sizeof rel, "%s/%s", sub, leaf)
                              : snprintf(rel, sizeof rel, "%s", leaf);
    if (len < 0 || len >= DSK_NAME_MAX) { c->skipped_long++; return false; }
    if (dsk_cat_find_name(c, rel) >= 0) return false;
    if (c->n >= DSK_CAT_MAX) { c->skipped_full++; return false; }
    struct dsk_entry *e = &c->e[c->n++];
    strcpy(e->name, rel);
    e->dir = DSK_DIR_ROOT;
    return true;
}

// PIZERO-309: a text file inside the disk in drive d, listed as Disk BASIC
// names it ("README.TXT:1").
static inline bool text_cat_add_disk(struct dsk_catalog *c, unsigned d, const char *name) {
    if (strlen(name) >= DSK_NAME_MAX) { c->skipped_long++; return false; }
    if (c->n >= DSK_CAT_MAX) { c->skipped_full++; return false; }
    struct dsk_entry *e = &c->e[c->n++];
    strcpy(e->name, name);
    e->dir = (uint8_t)(DSK_DIR_DRIVE0 + d);
    return true;
}

// The drive whose disk holds entry i, or -1 for a file on the card.
static inline int dsk_cat_drive(const struct dsk_catalog *c, int i) {
    return (i >= 0 && i < c->n && c->e[i].dir >= DSK_DIR_DRIVE0) ? c->e[i].dir - DSK_DIR_DRIVE0 : -1;
}

// The file's own name, after its folder: what the editor's title shows.
static inline const char *text_cat_leaf(const char *rel) {
    const char *slash = strrchr(rel, '/');
    return slash ? slash + 1 : rel;
}

// The FatFs path for entry i. Returns false if it does not fit.
static inline bool dsk_cat_path(const struct dsk_catalog *c, int i,
                                char *out, size_t out_sz) {
    if (i < 0 || i >= c->n || !out) return false;
    if (c->e[i].dir >= DSK_DIR_DRIVE0) return false;   // inside a disk: no card path
    int len = snprintf(out, out_sz, "%s/%s",
                       c->e[i].dir == DSK_DIR_DSK ? cat_kind_dir(c->kind) : "0:/coco",
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

// The name as the overlay shows it: the list's kind already says what the
// files are, so the extension is dropped to leave more of the name on a
// 32-column screen. Clipped to `width` characters.
static inline void dsk_cat_display_name(const char *name, char *out, int width) {
    int len = (int)strlen(name);
    const char *ext = strrchr(name, '.');
    if (ext && ext != name) len = (int)(ext - name);
    if (len > width) len = width;
    memcpy(out, name, (size_t)len);
    out[len] = '\0';
}

// PIZERO-139: a cartridge image the machine can map: 2, 4, 8 or 16 KB, and
// no bigger than the build's cartridge buffer.
static inline bool cat_cart_size_ok(unsigned long size, unsigned long max) {
    return (size == 2048 || size == 4096 || size == 8192 || size == 16384) && size <= max;
}

#endif  // DSK_CATALOG_H

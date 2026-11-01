// decb_files.h: Disk BASIC's file rules, for files that live on the card
// (PIZERO-289). The foundation for directory drives (PIZERO-290, 292),
// unpacking and packing disks (PIZERO-297), and the FILES list's "is this
// text" test (PIZERO-296). Design: docs/directory-drives-proposal.md.
//
// Pure logic, no FatFs, host-tested (test/test_decb_files).
//
// THE 8.3 RULE. A host file is seen only if its name is up to 8 characters,
// optionally a dot and up to 3 more. Everything else is ignored: names
// starting with a dot (no name part), longer names, and directories (the
// caller skips those). Case is folded, since Disk BASIC is upper case and
// FAT cannot hold two names that differ only in case. Characters must be
// ones both sides can hold: Disk BASIC's filename parser (disk11.rom $C938)
// refuses $00, '.', '/', ':' and $FF in a name, and a FAT short name refuses
// space and " * + , / : ; < = > ? [ \ ] |. Letters, digits and
// ! # $ % & ' ( ) - @ ^ _ ` { } ~ pass both.
//
// TYPE FROM CONTENT, since there are no sidecar files. A directory entry
// carries a file type (0 BASIC, 1 data, 2 machine code, 3 editor source)
// and an ASCII flag, and they matter: Disk BASIC raises ?FM ERROR when the
// type does not suit the command, and LOAD re-tokenizes an ASCII program.
//   * first byte $FF               tokenized BASIC  (type 0, binary)
//   * a valid LOADM preamble        machine code     (type 2, binary)
//   * printable text               ASCII            (flag $FF): type 0 for
//                                  .BAS, 3 for .ASM, else 1
//   * anything else                data, binary     (type 1); type 2 if .BIN
// The extension breaks the ties: an ASCII .BAS is a BASIC program, a .BIN
// that does not parse is still offered to LOADM.

#ifndef DECB_FILES_H
#define DECB_FILES_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { DECB_T_BASIC = 0, DECB_T_DATA = 1, DECB_T_ML = 2, DECB_T_TEXT = 3 };
#define DECB_ASCII  0xFF
#define DECB_BINARY 0x00

// ---- names ----

// A character both Disk BASIC and a FAT short name can hold.
static inline bool decb_name_char_ok(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    return c && strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

static inline char decb_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Split a host file name into Disk BASIC's 8-byte name and 3-byte extension,
// upper case and space-padded (not NUL-terminated). Returns false, and
// leaves the fields alone, if the name does not follow the 8.3 rule.
static inline bool decb_from_host(const char *host, char name8[8], char ext3[3]) {
    if (!host || !host[0] || host[0] == '.') return false;
    const char *dot = strchr(host, '.');
    size_t nl = dot ? (size_t)(dot - host) : strlen(host);
    size_t el = dot ? strlen(dot + 1) : 0;
    if (nl < 1 || nl > 8 || el > 3) return false;
    if (dot && (el == 0 || strchr(dot + 1, '.'))) return false;   // "A." or a second dot
    for (size_t i = 0; i < nl; i++) if (!decb_name_char_ok((unsigned char)host[i])) return false;
    for (size_t i = 0; i < el; i++) if (!decb_name_char_ok((unsigned char)dot[1 + i])) return false;
    for (size_t i = 0; i < 8; i++) name8[i] = i < nl ? decb_upper(host[i]) : ' ';
    for (size_t i = 0; i < 3; i++) ext3[i] = i < el ? decb_upper(dot[1 + i]) : ' ';
    return true;
}

// Does this host name follow the 8.3 rule?
static inline bool decb_is_83(const char *host) {
    char n[8], e[3];
    return decb_from_host(host, n, e);
}

// Disk BASIC's name and extension (as in a directory entry) as a host file
// name: "NAME.EXT", or "NAME" when the extension is blank. Returns false for
// a name the card cannot hold (an empty name, a space inside it, or a
// character outside decb_name_char_ok), so a guest SAVE of one fails with
// an error rather than making a bad host file. out needs 13 bytes.
static inline bool decb_to_host(const char name8[8], const char ext3[3], char *out, size_t out_sz) {
    if (out_sz < 13) return false;
    int nl = 8, el = 3;
    while (nl > 0 && name8[nl - 1] == ' ') nl--;
    while (el > 0 && ext3[el - 1] == ' ') el--;
    if (nl == 0) return false;
    for (int i = 0; i < nl; i++) if (!decb_name_char_ok((unsigned char)name8[i])) return false;
    for (int i = 0; i < el; i++) if (!decb_name_char_ok((unsigned char)ext3[i])) return false;
    int k = 0;
    for (int i = 0; i < nl; i++) out[k++] = name8[i];
    if (el) { out[k++] = '.'; for (int i = 0; i < el; i++) out[k++] = ext3[i]; }
    out[k] = '\0';
    return true;
}

// ---- content ----

// Is this the start of a text file? Tabs, CR, LF, form feed and printable
// ASCII, plus well-formed UTF-8 (a README with a curly quote is still text).
// NUL, other control bytes, $FF and broken UTF-8 make it binary. A
// multi-byte character cut off by the end of the sample does not count
// against it. An empty file is text.
static inline bool decb_is_text(const uint8_t *b, int n) {
    for (int i = 0; i < n;) {
        uint8_t c = b[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || (c >= 0x20 && c < 0x7F)) { i++; continue; }
        int more = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if (more < 0) return false;
        for (int k = 1; k <= more; k++) {
            if (i + k >= n) return true;                 // cut off by the sample
            if ((b[i + k] & 0xC0) != 0x80) return false;
        }
        i += 1 + more;
    }
    return true;
}

// A LOADM file: segments of $00, length (2), load address (2), data; then
// $FF, $00 $00, exec address (2). head is the file's first bytes (at least
// 5), tail its last 5, size its length. Checks the first segment fits and
// the postamble is there.
static inline bool decb_is_loadm(const uint8_t *head, int head_n, const uint8_t *tail, uint32_t size) {
    if (head_n < 5 || size < 10 || head[0] != 0x00) return false;
    uint32_t len = ((uint32_t)head[1] << 8) | head[2];
    if (len == 0 || 5 + len + 5 > size) return false;
    return tail && tail[0] == 0xFF && tail[1] == 0x00 && tail[2] == 0x00;
}

static inline bool decb_ext_is(const char ext3[3], const char *e) {
    for (int i = 0; i < 3; i++) if (ext3[i] != (e[i] ? e[i] : ' ')) return false;
    return true;
}

// The type and ASCII flag a directory entry should carry for this file.
// head: the first head_n bytes (512 is plenty); tail: the last 5 (or NULL
// if the file is shorter than 10); ext3: the Disk BASIC extension.
static inline void decb_sniff(const uint8_t *head, int head_n, const uint8_t *tail,
                              uint32_t size, const char ext3[3],
                              uint8_t *type, uint8_t *ascii) {
    if (size > 0 && head_n > 0 && head[0] == 0xFF) {             // tokenized BASIC
        *type = DECB_T_BASIC; *ascii = DECB_BINARY; return;
    }
    if (decb_is_loadm(head, head_n, tail, size)) {
        *type = DECB_T_ML; *ascii = DECB_BINARY; return;
    }
    if (decb_is_text(head, head_n)) {
        *ascii = DECB_ASCII;
        *type = decb_ext_is(ext3, "BAS") ? DECB_T_BASIC
              : decb_ext_is(ext3, "ASM") ? DECB_T_TEXT : DECB_T_DATA;
        return;
    }
    *ascii = DECB_BINARY;
    *type = decb_ext_is(ext3, "BIN") ? DECB_T_ML : DECB_T_DATA;
}

#endif  // DECB_FILES_H

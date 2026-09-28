// settings.h: /coco/settings.txt and /coco/pal/*.pal (PIZERO-145).
//
// There is no settings GUI: the file is the source of truth, edited as text
// (on a PC, or on screen once PIZERO-146 lands). This header is the pure
// half, host-tested: the settings, their defaults, and one-line parsers for
// both files. Reading the files is coco_boot.cpp's; applying the values is
// main.cpp's.
//
// settings.txt, one per line, names and values case-insensitive:
//
//   # comment lines start with '#'
//   sn76489           = on | off              SN76489 at $FF41 without a GMC
//   volume            = 0-15                  10 is the old fixed level
//   artifact_colours  = on | off | swapped    PMODE 4 colour, or plain mono
//   gime_palette      = on | off              CoCo 3-style palette at $FFB0
//   gime_timer        = on | off              CoCo 3-style timer at $FF90
//   run_skips_autorun = on | off              RUN goes straight to BASIC
//   serial_keyboard   = on | off              type into the CoCo over USB serial
//   palette           = factory | NAME        loads /coco/pal/NAME.pal
//
// Anything after the value is ignored if it starts with '#', so a line can
// carry a comment. A missing line keeps the default; an unknown name or a
// bad value is reported and ignored, never fatal.
//
// NAME.pal, one per line: "N = #RRGGBB" for palette index N (0-15). Indices
// not listed keep their factory colour; '#' at the start of a line is a
// comment. Index order is the VDG's: 0 green, 1 yellow, 2 blue, 3 red,
// 4 white, 5 cyan, 6 magenta, 7 orange, 8 black, 9 dark green, 10 dark
// orange, 11 bright orange.

#ifndef SETTINGS_H
#define SETTINGS_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { ART_OFF = 0, ART_ON = 1, ART_SWAPPED = 2 };

#define SETTINGS_NAME_MAX 24          // a palette name, without .pal

struct coco_settings {
    bool    sn76489;
    uint8_t volume;                   // 0-15
    uint8_t artifact;                 // ART_*
    bool    gime_palette;
    bool    gime_timer;
    bool    run_skips_autorun;
    bool    serial_keyboard;
    char    palette[SETTINGS_NAME_MAX + 1];   // "" = factory
};

static inline void settings_defaults(struct coco_settings *s) {
    memset(s, 0, sizeof *s);
    s->sn76489 = true;
    s->volume = 10;
    s->artifact = ART_ON;
    s->gime_palette = true;
    s->gime_timer = true;
    s->run_skips_autorun = true;
    s->serial_keyboard = true;
    s->palette[0] = '\0';
}

// Parse results. SET_OK covers blank and comment lines too.
enum { SET_OK = 0, SET_UNKNOWN = 1, SET_BAD_VALUE = 2, SET_SYNTAX = 3 };

// Copy the next whitespace-delimited token from *p into out, lower-cased.
static inline int settings_token(const char **p, char *out, size_t n, char stop) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    size_t k = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != stop) {
        if (k + 1 < n) out[k++] = (char)tolower((unsigned char)*s);
        s++;
    }
    out[k] = '\0';
    *p = s;
    return (int)k;
}

// Split "name = value [# comment]" into lower-cased name and value. Returns
// false for a blank or comment line (both empty), and sets *syntax when the
// line has content but no '=' or no value.
static inline bool settings_split(const char *line, char *name, size_t nn,
                                  char *value, size_t vn, bool *syntax) {
    *syntax = false;
    name[0] = value[0] = '\0';
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '#') return false;
    settings_token(&p, name, nn, '=');
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') { *syntax = true; return false; }
    p++;
    if (settings_token(&p, value, vn, '\0') == 0) { *syntax = true; return false; }
    while (*p == ' ' || *p == '\t') p++;
    if (*p && *p != '#' && *p != '\r' && *p != '\n') { *syntax = true; return false; }
    return true;
}

static inline bool settings_bool(const char *v, bool *out) {
    if (!strcmp(v, "on")  || !strcmp(v, "yes") || !strcmp(v, "1")) { *out = true;  return true; }
    if (!strcmp(v, "off") || !strcmp(v, "no")  || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

// A palette name: letters, digits, '_' and '-' only, so it can only ever
// name a file inside /coco/pal.
static inline bool settings_palette_name_ok(const char *v) {
    size_t n = strlen(v);
    if (n == 0 || n > SETTINGS_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)v[i]) && v[i] != '_' && v[i] != '-') return false;
    return true;
}

// One line of settings.txt. On failure, *name_out (if given) holds the
// setting's name for the report.
static inline int settings_parse_line(struct coco_settings *s, const char *line,
                                      char *name_out, size_t name_sz) {
    char name[32], v[40];
    bool syntax;
    if (!settings_split(line, name, sizeof name, v, sizeof v, &syntax))
        return syntax ? SET_SYNTAX : SET_OK;
    if (name_out) snprintf(name_out, name_sz, "%s", name);
    bool b;
    if (!strcmp(name, "sn76489"))           { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->sn76489 = b; }
    else if (!strcmp(name, "gime_palette")) { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->gime_palette = b; }
    else if (!strcmp(name, "gime_timer"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->gime_timer = b; }
    else if (!strcmp(name, "run_skips_autorun")) { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->run_skips_autorun = b; }
    else if (!strcmp(name, "serial_keyboard"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->serial_keyboard = b; }
    else if (!strcmp(name, "volume")) {
        char *end; long n = strtol(v, &end, 10);
        if (*end || n < 0 || n > 15) return SET_BAD_VALUE;
        s->volume = (uint8_t)n;
    } else if (!strcmp(name, "artifact_colours") || !strcmp(name, "artifact_colors")) {
        if (!strcmp(v, "on"))           s->artifact = ART_ON;
        else if (!strcmp(v, "off"))     s->artifact = ART_OFF;
        else if (!strcmp(v, "swapped")) s->artifact = ART_SWAPPED;
        else return SET_BAD_VALUE;
    } else if (!strcmp(name, "palette")) {
        if (!strcmp(v, "factory")) s->palette[0] = '\0';
        else if (settings_palette_name_ok(v)) snprintf(s->palette, sizeof s->palette, "%s", v);
        else return SET_BAD_VALUE;
    } else {
        return SET_UNKNOWN;
    }
    return SET_OK;
}

// #RRGGBB to the screen's RGB565, rounding each channel.
static inline uint16_t settings_rgb565(uint32_t rgb) {
    unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5)
                      | ((b * 31 + 127) / 255));
}

// One line of a .pal file: "N = #RRGGBB". Returns SET_OK with *idx = -1
// for a blank or comment line.
static inline int pal_parse_line(const char *line, int *idx, uint32_t *rgb) {
    char name[16], v[16];
    bool syntax;
    *idx = -1;
    if (!settings_split(line, name, sizeof name, v, sizeof v, &syntax))
        return syntax ? SET_SYNTAX : SET_OK;
    char *end;
    long n = strtol(name, &end, 10);
    if (*end || end == name || n < 0 || n > 15) return SET_UNKNOWN;
    if (v[0] != '#' || strlen(v) != 7) return SET_BAD_VALUE;
    unsigned long c = strtoul(v + 1, &end, 16);
    if (*end) return SET_BAD_VALUE;
    *idx = (int)n;
    *rgb = (uint32_t)c;
    return SET_OK;
}

#endif  // SETTINGS_H

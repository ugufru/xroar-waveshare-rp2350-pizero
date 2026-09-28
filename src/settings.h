// settings.h: /coco/settings.txt (PIZERO-145).
//
// There is no settings GUI: the file is the source of truth, edited as text
// (on a PC, or on screen once PIZERO-146 lands). This header is the pure
// half, host-tested: the settings, their defaults, and the one-line parser.
// Reading the file is coco_boot.cpp's; applying the values is main.cpp's.
//
// settings.txt, one per line, names and values case-insensitive:
//
//   # comment lines start with '#'
//   sn76489           = on | off              SN76489 at $FF41 without a GMC
//   volume            = 0-15                  10 is the old fixed level
//   artifact_colors   = on | off | swapped    PMODE 4 color, or plain mono
//   gime_palette      = on | off              CoCo 3-style palette at $FFB0
//   gime_timer        = on | off              CoCo 3-style timer at $FF90
//   run_skips_autorun = on | off              RUN goes straight to BASIC
//   serial_keyboard   = on | off              type into the CoCo over USB serial
//   color_green       = #RRGGBB               override one palette color
//
// There is one color_ setting per 6847 color: green, yellow, blue, red,
// white, cyan, magenta, orange, black, dark_green, dark_orange and
// bright_orange. A color without a line keeps its default. No palette files
// or named styles: overriding colors here is the whole mechanism.
//
// Anything after the value is ignored if it starts with '#', so a line can
// carry a comment. A missing line keeps the default; an unknown name or a
// bad value is reported and ignored, never fatal.

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

struct coco_settings {
    bool    sn76489;
    uint8_t volume;                   // 0-15
    uint8_t artifact;                 // ART_*
    bool    gime_palette;
    bool    gime_timer;
    bool    run_skips_autorun;
    bool    serial_keyboard;
    uint16_t color[16];               // RGB565 overrides, by palette index
    uint16_t color_set;               // bit i: color[i] overrides the default
};

// The 6847 colors by palette index, as the color_ settings name them.
static const char *const SETTINGS_COLOR_NAMES[12] = {
    "green", "yellow", "blue", "red", "white", "cyan", "magenta", "orange",
    "black", "dark_green", "dark_orange", "bright_orange"
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

// #RRGGBB to the screen's RGB565, rounding each channel.
static inline uint16_t settings_rgb565(uint32_t rgb) {
    unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5)
                      | ((b * 31 + 127) / 255));
}

// "#RRGGBB" as a 24-bit color.
static inline bool settings_hex_color(const char *v, uint32_t *rgb) {
    if (v[0] != '#' || strlen(v) != 7) return false;
    char *end;
    unsigned long c = strtoul(v + 1, &end, 16);
    if (*end) return false;
    *rgb = (uint32_t)c;
    return true;
}

// The palette index a color_ setting names, or -1.
static inline int settings_color_index(const char *name) {
    if (strncmp(name, "color_", 6)) return -1;
    for (int i = 0; i < 12; i++)
        if (!strcmp(name + 6, SETTINGS_COLOR_NAMES[i])) return i;
    return -1;
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
    } else if (!strcmp(name, "artifact_colors")) {
        if (!strcmp(v, "on"))           s->artifact = ART_ON;
        else if (!strcmp(v, "off"))     s->artifact = ART_OFF;
        else if (!strcmp(v, "swapped")) s->artifact = ART_SWAPPED;
        else return SET_BAD_VALUE;
    } else if (settings_color_index(name) >= 0) {
        uint32_t rgb;
        if (!settings_hex_color(v, &rgb)) return SET_BAD_VALUE;
        int i = settings_color_index(name);
        s->color[i] = settings_rgb565(rgb);
        s->color_set |= (uint16_t)(1u << i);
    } else {
        return SET_UNKNOWN;
    }
    return SET_OK;
}

// PIZERO-154: a game's own settings file: the game's path with its extension
// replaced by .TXT, beside it ('0:/coco/bin/ORBIT.BIN' -> '0:/coco/bin/ORBIT.TXT').
// Refused (false) for a name that would be the machine's own settings.txt or
// autorun.txt, for a path with no extension, or if it does not fit.
static inline bool settings_game_path(const char *game, char *out, size_t n) {
    const char *slash = strrchr(game, '/');
    const char *base = slash ? slash + 1 : game;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return false;
    size_t stem = (size_t)(dot - base);
    if ((stem == 8 && !strncasecmp(base, "settings", 8)) ||
        (stem == 7 && !strncasecmp(base, "autorun", 7))) return false;
    size_t keep = (size_t)(dot - game);
    if (keep + 5 > n) return false;
    memcpy(out, game, keep);
    memcpy(out + keep, ".TXT", 5);
    return true;
}

// PIZERO-146: the text the on-screen editor starts from when there is no
// settings.txt: every setting at its default, with the colors commented out,
// so the file documents itself. Built from settings_defaults, so it cannot
// drift from them. Returns the length written.
static inline int settings_template(char *out, size_t n) {
    struct coco_settings d;
    settings_defaults(&d);
    int k = snprintf(out, n,
        "# COCO ZERO SETTINGS. SEE SETTINGS.MD\n"
        "sn76489 = %s\n"
        "volume = %u\n"
        "artifact_colors = %s\n"
        "gime_palette = %s\n"
        "gime_timer = %s\n"
        "run_skips_autorun = %s\n"
        "serial_keyboard = %s\n"
        "# color_green = #00FF00\n"
        "# color_dark_green = #006500\n",
        d.sn76489 ? "on" : "off", d.volume,
        d.artifact == ART_OFF ? "off" : d.artifact == ART_SWAPPED ? "swapped" : "on",
        d.gime_palette ? "on" : "off", d.gime_timer ? "on" : "off",
        d.run_skips_autorun ? "on" : "off", d.serial_keyboard ? "on" : "off");
    return (k < 0) ? 0 : (k >= (int)n ? (int)n - 1 : k);
}

#endif  // SETTINGS_H

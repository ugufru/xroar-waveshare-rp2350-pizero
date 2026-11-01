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
//   coco_psg          = on | off              PIZERO-149: CoCo PSG's AY at $FF5D-$FF5F
//   symphony12        = on | off              PIZERO-149: Symphony-12's four AYs at $FF60
//   symphony12_stereo = on | off              PIZERO-250: its chips 1-2 left, 3-4 right
//   deluxe_psg        = on | off              PIZERO-149: Deluxe CoCo's AY at $FF38
//   orchestra90       = on | off              PIZERO-148: Orchestra-90CC's DACs at $FF7A/$FF7B
//   usb_audio         = on | off              PIZERO-255: a USB headphone DAC at boot takes the sound
//   usb_audio_db      = -60-0                 PIZERO-255: that DAC's own level, set when it starts
//   sound_sources     = all | switched        PIZERO-253: every source at once, or the multiplexer
//   joystick_quiet    = on | off              PIZERO-253: joystick reads not heard
//   volume            = 0-15                  10 is the old fixed level
//   audio_lowpass     = 1000-20000 | on | off PIZERO-212: output low-pass cutoff, Hz
//   artifact_colors   = on | off | swapped    PMODE 4 color, or plain mono
//   gime_palette      = on | off              CoCo 3-style palette at $FFB0
//   gime_timer        = on | off              CoCo 3-style timer at $FF90
//   autorun           = on | off              run autorun.txt at power-on
//   reset_button      = basic | autorun       RUN (reset) skips autorun, or not
//   serial_keyboard   = on | off              type into the CoCo over USB serial
//   soak_log          = on | off              PIZERO-201: telemetry to /coco/log every minute
//   joystick_swap     = on | off              pad's right stick drives JOYSTK(0)/(1)
//   font              = classic | 6847t1 | 6847t2   text font set (6847t2 default)
//   lowercase         = on | off              6847t2: true lower case always
//   key_repeat        = on | off              USB keyboard auto-repeat
//   key_repeat_delay  = 100-2000              milliseconds before it starts
//   key_repeat_rate   = 1-30                  repeats a second (12 at most in effect)
//   dpad              = joystick | arrows     D-pad moves the stick, or presses arrows
//   pad_<button>      = fire | fire_right | fire2 | fire2_right | none | a CoCo key
//                       (PIZERO-164; fire2 and fire2_right PIZERO-215)
//                       buttons: bottom right left top l1 r1 l2 r2
//                                select start l3 r3 (Home opens the overlay)
//                       keys: a-z 0-9 @ : ; , - . / space enter clear
//                             break shift up down left right
//   video_encoder     = hardware | software   RP2350 TMDS encoder, or libdvi's tables
//   machine           = cocozero | coco2 | coco3   PIZERO-135/242: which CoCo the board is
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

#include "gamepad.h"        // PIZERO-164: pad buttons (struct pad_map)
#include "key_translate.h"  // PIZERO-164: CoCo key names
#include "text_lines.h"     // PIZERO-294: CR, LF or CRLF line ends

enum { ART_OFF = 0, ART_ON = 1, ART_SWAPPED = 2 };
// PIZERO-242: the machine profiles. cocozero, the default, is a CoCo 2 with
// this project's extras; coco2 and coco3 are the stock machines.
enum { MACH_COCOZERO = 0, MACH_COCO2 = 1, MACH_COCO3 = 2 };
static const char *const SETTINGS_MACHINE_NAMES[3] = { "cocozero", "coco2", "coco3" };
// PIZERO-166: font sets, the same numbers as VDG_FONT_* in vdg_pack.h.
enum { FONT_CLASSIC = 0, FONT_6847T1 = 1, FONT_6847T2 = 2 };

struct coco_settings {
    bool    sn76489;
    bool    coco_psg;                 // PIZERO-149: the AY cartridges
    bool    symphony12;
    bool    symphony12_stereo;        // PIZERO-250: the card's stereo jumper
    bool    deluxe_psg;
    bool    orchestra90;              // PIZERO-148
    bool    usb_audio;                // PIZERO-255
    int8_t  usb_audio_db;             // PIZERO-255: -60 to 0
    bool    sound_all;                // PIZERO-253: sound_sources = all (else switched)
    bool    joystick_quiet;           // PIZERO-253
    uint8_t volume;                   // 0-15
    uint16_t lowpass_hz;              // PIZERO-212: 0 off, else 1000-20000
    uint8_t artifact;                 // ART_*
    bool    gime_palette;
    bool    gime_timer;
    bool    autorun;                  // PIZERO-182: run autorun.txt at all
    bool    reset_to_basic;           // PIZERO-182: reset_button = basic
    bool    serial_keyboard;
    bool    soak_log;                 // PIZERO-201: write telemetry to the card
    bool    joystick_swap;            // PIZERO-160: pad sticks to the other ports
    uint8_t font;                     // PIZERO-166: FONT_*
    bool    lowercase;                // PIZERO-166: 6847t2 holds true lower case on
    bool    key_repeat;               // PIZERO-167: USB keyboard auto-repeat
    uint16_t key_repeat_delay;        // ms
    uint8_t key_repeat_rate;          // per second
    struct pad_map pad;               // PIZERO-164: what each pad button does
    bool    video_hw_encode;          // PIZERO-175: video_encoder = hardware
    uint8_t machine;                  // PIZERO-135/242: MACH_*
    uint16_t color[16];               // RGB565 overrides, by palette index
    uint16_t color_set;               // bit i: color[i] overrides the default
};

// The 6847 colors by palette index, as the color_ settings name them.
static const char *const SETTINGS_COLOR_NAMES[12] = {
    "green", "yellow", "blue", "red", "white", "cyan", "magenta", "orange",
    "black", "dark_green", "dark_orange", "bright_orange"
};

static inline bool settings_is_coco3(const struct coco_settings *s) {
    return s->machine == MACH_COCO3;
}

// PIZERO-242: a profile's starting point for the settings that describe the
// emulated hardware. Only cocozero adds anything: the GIME-style palette and
// timer, the SN76489 without a cartridge (a Games Master cartridge brings it
// on any machine), the AY cartridges' chips (PIZERO-149: the CoCo PSG and
// the Symphony-12; the Deluxe CoCo's stays off, its addresses being PIA1's
// mirror on a CoCo 2), the Orchestra-90's DACs (PIZERO-148), the 6847T2 font with true lower case, and auto-repeat
// (PIZERO-247: the stock machines' BASIC never repeated a key), every sound
// source heard at once and joystick reads kept out of the sound (PIZERO-253:
// the stock machines switch their sources through the multiplexer, and a
// program that reads the joystick with the sound on is heard doing it).
// settings.txt, then a game's own file, go on top, so any of it can be
// changed.
static inline void settings_profile(struct coco_settings *s, uint8_t machine) {
    bool zero = machine == MACH_COCOZERO;
    s->machine = machine;
    s->sn76489 = zero;
    s->coco_psg = zero;
    s->symphony12 = zero;
    s->gime_palette = zero;
    s->gime_timer = zero;
    s->font = zero ? FONT_6847T2 : FONT_CLASSIC;
    s->lowercase = zero;
    s->key_repeat = zero;
    s->sound_all = zero;
    s->joystick_quiet = zero;
    s->orchestra90 = zero;
}

static inline void settings_defaults(struct coco_settings *s) {
    memset(s, 0, sizeof *s);
    s->sn76489 = true;
    s->symphony12_stereo = true;      // PIZERO-250: a cartridge's jumper, not the machine's
    s->volume = 10;
    s->lowpass_hz = 8000;             // improvements are on by default
    s->artifact = ART_ON;
    s->gime_palette = true;
    s->gime_timer = true;
    s->autorun = true;
    s->reset_to_basic = true;
    s->serial_keyboard = true;
    s->soak_log = false;              // a diagnostic, so off
    s->font = FONT_6847T2;
    s->lowercase = true;              // improvements are on by default
    s->key_repeat = true;
    s->key_repeat_delay = 500;
    s->key_repeat_rate = 10;
    s->orchestra90 = true;            // PIZERO-148
    s->usb_audio = true;              // PIZERO-255: plugging a DAC in means wanting it
    s->usb_audio_db = -10;            // PIZERO-255: TV-level sound is hot at a DAC's 0 dB
    s->sound_all = true;              // PIZERO-253: improvements are on by default
    s->joystick_quiet = true;
    pad_map_defaults(&s->pad);
    s->video_hw_encode = true;
    settings_profile(s, MACH_COCOZERO);
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
    else if (!strcmp(name, "coco_psg"))     { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->coco_psg = b; }
    else if (!strcmp(name, "symphony12"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->symphony12 = b; }
    else if (!strcmp(name, "symphony12_stereo")) { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->symphony12_stereo = b; }
    else if (!strcmp(name, "sound_sources")) {
        if (!strcmp(v, "all"))           s->sound_all = true;
        else if (!strcmp(v, "switched")) s->sound_all = false;
        else return SET_BAD_VALUE;
    }
    else if (!strcmp(name, "usb_audio"))         { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->usb_audio = b; }
    else if (!strcmp(name, "usb_audio_db")) {
        char *e;
        long n = strtol(v, &e, 10);
        if (*e || e == v || n < -60 || n > 0) return SET_BAD_VALUE;
        s->usb_audio_db = (int8_t)n;
    }
    else if (!strcmp(name, "orchestra90"))       { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->orchestra90 = b; }
    else if (!strcmp(name, "joystick_quiet"))    { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->joystick_quiet = b; }
    else if (!strcmp(name, "deluxe_psg"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->deluxe_psg = b; }
    else if (!strcmp(name, "gime_palette")) { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->gime_palette = b; }
    else if (!strcmp(name, "gime_timer"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->gime_timer = b; }
    else if (!strcmp(name, "autorun"))           { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->autorun = b; }
    else if (!strcmp(name, "reset_button")) {
        if (!strcmp(v, "basic"))         s->reset_to_basic = true;
        else if (!strcmp(v, "autorun"))  s->reset_to_basic = false;
        else return SET_BAD_VALUE;
    }
    else if (!strcmp(name, "serial_keyboard"))   { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->serial_keyboard = b; }
    else if (!strcmp(name, "soak_log"))          { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->soak_log = b; }
    else if (!strcmp(name, "joystick_swap"))     { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->joystick_swap = b; }
    else if (!strcmp(name, "lowercase"))         { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->lowercase = b; }
    else if (!strcmp(name, "key_repeat"))        { if (!settings_bool(v, &b)) return SET_BAD_VALUE; s->key_repeat = b; }
    else if (!strcmp(name, "key_repeat_delay")) {
        char *end; long n = strtol(v, &end, 10);
        if (*end || n < 100 || n > 2000) return SET_BAD_VALUE;
        s->key_repeat_delay = (uint16_t)n;
    } else if (!strcmp(name, "key_repeat_rate")) {
        char *end; long n = strtol(v, &end, 10);
        if (*end || n < 1 || n > 30) return SET_BAD_VALUE;
        s->key_repeat_rate = (uint8_t)n;
    }
    else if (!strcmp(name, "volume")) {
        char *end; long n = strtol(v, &end, 10);
        if (*end || n < 0 || n > 15) return SET_BAD_VALUE;
        s->volume = (uint8_t)n;
    } else if (!strcmp(name, "audio_lowpass")) {
        if (settings_bool(v, &b)) { s->lowpass_hz = b ? 8000 : 0; return SET_OK; }   // on: the default
        char *end; long n = strtol(v, &end, 10);
        if (*end || n < 1000 || n > 20000) return SET_BAD_VALUE;
        s->lowpass_hz = (uint16_t)n;
    } else if (!strcmp(name, "font")) {
        if (!strcmp(v, "classic"))     s->font = FONT_CLASSIC;
        else if (!strcmp(v, "6847t1")) s->font = FONT_6847T1;
        else if (!strcmp(v, "6847t2")) s->font = FONT_6847T2;
        else return SET_BAD_VALUE;
    } else if (!strcmp(name, "dpad")) {
        if (!strcmp(v, "joystick"))    s->pad.dpad_arrows = false;
        else if (!strcmp(v, "arrows")) s->pad.dpad_arrows = true;
        else return SET_BAD_VALUE;
    } else if (!strncmp(name, "pad_", 4)) {
        int bi = -1;
        for (int i = 0; i < PAD_BUTTONS; i++)
            if (PAD_BUTTON_NAMES[i] && !strcmp(name + 4, PAD_BUTTON_NAMES[i])) bi = i;
        if (bi < 0) return SET_UNKNOWN;
        uint8_t a;
        if (!strcmp(v, "fire"))            a = PAD_ACT_FIRE;
        else if (!strcmp(v, "fire_right")) a = PAD_ACT_FIRE_RIGHT;
        else if (!strcmp(v, "fire2"))      a = PAD_ACT_FIRE2;
        else if (!strcmp(v, "fire2_right")) a = PAD_ACT_FIRE2_RIGHT;
        else if (!strcmp(v, "none"))       a = PAD_ACT_NONE;
        else if ((a = kt_key_by_name(v)) == K_INVALID) return SET_BAD_VALUE;
        s->pad.act[bi] = a;
    } else if (!strcmp(name, "video_encoder")) {
        if (!strcmp(v, "hardware"))      s->video_hw_encode = true;
        else if (!strcmp(v, "software")) s->video_hw_encode = false;
        else return SET_BAD_VALUE;
    } else if (!strcmp(name, "machine")) {                       // PIZERO-135/242
        int m = -1;
        for (int i = 0; i < 3; i++) if (!strcmp(v, SETTINGS_MACHINE_NAMES[i])) m = i;
        if (m < 0) return SET_BAD_VALUE;
        s->machine = (uint8_t)m;
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

// PIZERO-186: set one setting in a settings file's text, keeping the rest as
// written. The last line with that name is the one that counts, so it is the
// one replaced ("name = value", any comment on it dropped); with none, the
// line is added at the end. buf holds len bytes and has room for cap.
// Returns the new length, or -1 if it would not fit.
// PIZERO-294: lines may end in CR, LF or CRLF, and an added line ends the
// way the file's lines already do (LF for a new file).
static inline int settings_set_line(char *buf, int len, int cap,
                                    const char *name, const char *value) {
    char line[128], n[32], v[40];
    int at = -1, at_end = -1;
    for (int pos = 0; pos < len;) {
        int b, l0;
        pos = tl_next(buf, len, pos, &b, &l0);
        int l = l0 < (int)sizeof line - 1 ? l0 : (int)sizeof line - 1;
        memcpy(line, buf + b, (size_t)l);
        line[l] = '\0';
        bool syntax;
        if (settings_split(line, n, sizeof n, v, sizeof v, &syntax) && !strcmp(n, name)) {
            at = b; at_end = b + l0;
        }
    }
    char repl[96];
    int rl = snprintf(repl, sizeof repl, "%s = %s", name, value);
    if (rl < 0 || rl >= (int)sizeof repl) return -1;
    if (at < 0) {                                    // append, on a line of its own
        const char *eol = tl_eol(tl_convention(buf, len));
        int el = (int)strlen(eol);
        bool open_end = len > 0 && !tl_is_eol(buf[len - 1]);
        int need = len + rl + el + (open_end ? el : 0);
        if (need > cap) return -1;
        if (open_end) { memcpy(buf + len, eol, (size_t)el); len += el; }
        memcpy(buf + len, repl, (size_t)rl);
        len += rl;
        memcpy(buf + len, eol, (size_t)el);
        len += el;
        return len;
    }
    int old = at_end - at, nl = len - old + rl;
    if (nl > cap) return -1;
    memmove(buf + at + rl, buf + at_end, (size_t)(len - at_end));
    memcpy(buf + at, repl, (size_t)rl);
    return nl;
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

// PIZERO-181: a settings file as the on-screen editor shows it: every
// comment line first, in its order, then a blank line, then the settings
// sorted by name (case ignored; lines with the same name keep their order,
// so the one that wins still comes last). Blank lines are dropped and '\r'
// removed; each line is otherwise kept as written. Writes at most cap bytes
// and returns the length. No index arrays, so it is safe on a small stack:
// the sort picks the next line by scanning, fine for a 4 KB file.
static inline int settings_tidy_line(const char *in, int n, int pos, int *start, int *len) {
    int s0, l0;
    int next = tl_next(in, n, pos, &s0, &l0);      // PIZERO-294: CR, LF or CRLF
    int b = s0, l = s0 + l0;
    while (b < l && (in[b] == ' ' || in[b] == '\t')) b++;
    while (l > b && (in[l - 1] == ' ' || in[l - 1] == '\t')) l--;
    *start = b; *len = l - b;
    return next;                                    // the next line's start
}

// Compare two settings lines by name (up to '=' or a space), case ignored.
static inline int settings_tidy_cmp(const char *a, int al, const char *b, int bl) {
    for (int i = 0;; i++) {
        int ca = (i < al && a[i] != '=' && a[i] != ' ' && a[i] != '\t') ? tolower((unsigned char)a[i]) : 0;
        int cb = (i < bl && b[i] != '=' && b[i] != ' ' && b[i] != '\t') ? tolower((unsigned char)b[i]) : 0;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static inline int settings_tidy(const char *in, int n, char *out, int cap) {
    int k = 0, ncomment = 0, nsetting = 0, b, l;
#define TIDY_PUT(p, m) do { for (int q = 0; q < (m) && k < cap; q++) out[k++] = (p)[q]; \
                            if (k < cap) out[k++] = '\n'; } while (0)
    for (int pos = 0; pos < n;) {
        pos = settings_tidy_line(in, n, pos, &b, &l);
        if (l && in[b] == '#') { TIDY_PUT(in + b, l); ncomment++; }
        else if (l) nsetting++;
    }
    if (ncomment && nsetting && k < cap) out[k++] = '\n';
    // Emit settings in (name, position) order: each time, the smallest line
    // that sorts after the one emitted last.
    int last_b = -1, last_l = 0;
    for (int done = 0; done < nsetting; done++) {
        int best_b = -1, best_l = 0;
        for (int pos = 0; pos < n;) {
            pos = settings_tidy_line(in, n, pos, &b, &l);
            if (!l || in[b] == '#') continue;
            if (last_b >= 0) {
                int c = settings_tidy_cmp(in + b, l, in + last_b, last_l);
                if (c < 0 || (c == 0 && b <= last_b)) continue;      // already emitted
            }
            if (best_b < 0) { best_b = b; best_l = l; continue; }
            int c = settings_tidy_cmp(in + b, l, in + best_b, best_l);
            if (c < 0 || (c == 0 && b < best_b)) { best_b = b; best_l = l; }
        }
        if (best_b < 0) break;
        TIDY_PUT(in + best_b, best_l);
        last_b = best_b; last_l = best_l;
    }
#undef TIDY_PUT
    return k;
}

// PIZERO-146: the text the on-screen editor starts from when there is no
// settings.txt: every setting at its default, with the colors and the
// settings the machine profile sets (PIZERO-242) commented out, so the file
// documents itself and a game's machine line is not overridden by it. Built from settings_defaults, so it cannot
// drift from them. Returns the length written.
static inline int settings_template(char *out, size_t n) {
    struct coco_settings d;
    settings_defaults(&d);
    int k = snprintf(out, n,
        "# COCO ZERO SETTINGS. SEE SETTINGS.MD\n"
        "machine = %s\n"
        "# THE MACHINE SETS THESE. A LINE HERE\n"
        "# WOULD HOLD FOR EVERY GAME:\n"
        "# sn76489 = %s\n"
        "# coco_psg = %s\n"
        "# symphony12 = %s\n"
        "# orchestra90 = %s\n"
        "# gime_palette = %s\n"
        "# gime_timer = %s\n"
        "# font = %s\n"
        "# lowercase = %s\n"
        "# key_repeat = %s\n"
        "# sound_sources = %s\n"
        "# joystick_quiet = %s\n"
        "symphony12_stereo = %s\n"
        "deluxe_psg = %s\n"
        "usb_audio = %s\n"
        "usb_audio_db = %d\n"
        "volume = %u\n"
        "audio_lowpass = %u\n"
        "artifact_colors = %s\n"
        "autorun = %s\n"
        "reset_button = %s\n"
        "serial_keyboard = %s\n"
        "joystick_swap = %s\n"
        "key_repeat_delay = %u\n"
        "key_repeat_rate = %u\n"
        "video_encoder = %s\n"
        "soak_log = %s\n"
        "# color_green = #00FF00\n"
        "# color_dark_green = #006500\n",
        SETTINGS_MACHINE_NAMES[d.machine],
        d.sn76489 ? "on" : "off", d.coco_psg ? "on" : "off", d.symphony12 ? "on" : "off",
        d.orchestra90 ? "on" : "off",
        d.gime_palette ? "on" : "off", d.gime_timer ? "on" : "off",
        d.font == FONT_CLASSIC ? "classic" : d.font == FONT_6847T1 ? "6847t1" : "6847t2",
        d.lowercase ? "on" : "off", d.key_repeat ? "on" : "off",
        d.sound_all ? "all" : "switched", d.joystick_quiet ? "on" : "off",
        d.symphony12_stereo ? "on" : "off", d.deluxe_psg ? "on" : "off", d.usb_audio ? "on" : "off", (int)d.usb_audio_db, d.volume, (unsigned)d.lowpass_hz,
        d.artifact == ART_OFF ? "off" : d.artifact == ART_SWAPPED ? "swapped" : "on",
        d.autorun ? "on" : "off", d.reset_to_basic ? "basic" : "autorun", d.serial_keyboard ? "on" : "off",
        d.joystick_swap ? "on" : "off",
        (unsigned)d.key_repeat_delay, (unsigned)d.key_repeat_rate,
        d.video_hw_encode ? "hardware" : "software", d.soak_log ? "on" : "off");
    if (k < 0 || k >= (int)n) return (k < 0) ? 0 : (int)n - 1;
    // PIZERO-164: the pad, every button at its default.
    k += snprintf(out + k, n - (size_t)k, "dpad = %s\n", d.pad.dpad_arrows ? "arrows" : "joystick");
    for (int b = 0; b < PAD_BUTTONS && k < (int)n; b++) {
        if (!PAD_BUTTON_NAMES[b]) continue;       // Home: the overlay's button
        uint8_t a = d.pad.act[b];
        char key[16] = "none";                    // longest is "fire2_right"
        if (a == PAD_ACT_FIRE) strcpy(key, "fire");
        else if (a == PAD_ACT_FIRE_RIGHT) strcpy(key, "fire_right");
        else if (a == PAD_ACT_FIRE2) strcpy(key, "fire2");
        else if (a == PAD_ACT_FIRE2_RIGHT) strcpy(key, "fire2_right");
        else if (a < 0x40) kt_key_name(a, key);
        k += snprintf(out + k, n - (size_t)k, "pad_%s = %s\n", PAD_BUTTON_NAMES[b], key);
    }
    return (k < 0) ? 0 : (k >= (int)n ? (int)n - 1 : k);
}

#endif  // SETTINGS_H

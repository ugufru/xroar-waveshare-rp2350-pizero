// psg_ay8910.h: the General Instrument AY-3-8910 (and Yamaha YM2149) sound
// chip, and the CoCo cartridges that carry it (PIZERO-149). Pure integer
// logic, host-tested.
//
// The chip follows the AY-3-8910 data sheet, with XRoar's model
// (~/github/xroar/src/ay891x.c) and MAME's (src/devices/sound/ay8910.cpp)
// as references:
//   * the chip steps at its clock / 8 ("chip ticks")
//   * 3 tone channels: a 12-bit period TP (0 counts as 1); the square output
//     toggles every TP ticks, so f = clock / (16 * TP)
//   * 1 noise generator: a 17-bit shift register (feedback bit 0 xor bit 3)
//     stepped every 2 * NP ticks (5-bit NP, 0 counts as 1), f = clock / (16 * NP)
//   * mixer (R7, bits active low): a channel is on when (tone or tone off) and
//     (noise or noise off). A channel with both off is steadily on at its
//     level, which is how AY sample playback works; XRoar's model makes that
//     silent instead
//   * 4-bit level per channel, about 3 dB a step (each step 1/sqrt 2), 0 off;
//     bit 4 hands the channel to the envelope
//   * envelope: 16 steps, one every 2 * EP ticks (16-bit EP, 0 counts as 1);
//     R13's CONT, ATT, ALT and HOLD bits give the data sheet's ten shapes,
//     and writing R13 restarts it
//   * bus: latch a register number (0-15), then read or write it. Reads give
//     the bits the register has (R1/3/5 4 bits, R6 5, R8-10 5, R13 4); the
//     I/O ports (R14, R15) are not wired here and read $FF
// It starts silent: every level 0.
//
// The cartridges, sharing one bank of four chips (PIZERO-149):
//   * CoCo PSG (a YM2149): $FF5E latches a register, $FF5F reads and writes
//     it, $FF5D bit 0 picks the clock (0 = 2 MHz, 1 = 1 MHz). Chip 0.
//   * Symphony-12 (Speech Systems, four AY-3-8910s): a 6821 PIA at
//     $FF60-$FF63. Port A is the chips' data bus; port B carries two bits per
//     chip (bits 1-0 chip 0 ... bits 7-6 chip 3): 11 latch, 10 write, 01 read.
//     A port A write acts with port B as it stands, a port B write with port A
//     as it stands, as MAME's coco_sym12.cpp does. Clocked from the cartridge
//     port, 14.31818 MHz / 16. Chips 0-3.
// (The Deluxe CoCo's AY at $FF38-$FF39 was here until PIZERO-342 dropped it:
// an unreleased machine, with next to no software.)
// Each sets its chips' clock when the program uses it, so the pitch is right
// for whichever card the program was written for.
//
// Output is averaged over each audio sample, the same integrate-and-average
// the DAC and the SN76489 use, so a tone above the sample rate does not alias.

#ifndef PSG_AY8910_H
#define PSG_AY8910_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// PIZERO-248: the render path's functions and tables go in RAM on the board
// (coco_machine.cpp defines SOUND_HOT as a RAM section). Left to the
// compiler, ay_tick came out as a function in flash, and the level table
// was read from flash too, both thousands of times a frame.
#ifndef SOUND_HOT
#define SOUND_HOT
#define SOUND_TABLE
#endif

#define AY_EVENT_HZ      14318180u        // the machine's event ticks per second
#define AY_CHIPS         4

#define AY_CLK_PSG_2MHZ  2000000u
#define AY_CLK_PSG_1MHZ  1000000u
#define AY_CLK_SYM12     (AY_EVENT_HZ / 16)

// Level of one channel at each 4-bit amplitude: full scale 1260, each step
// down 1/sqrt 2. One chip's three channels at full match the DAC's loudest,
// as the SN76489's four do (csg_sn76489.h). In RAM on the board (PIZERO-248).
static const uint16_t AY_LEVEL[16] SOUND_TABLE = {
       0,   10,   14,   20,   28,   39,   56,   79,
     111,  158,  223,  315,  445,  630,  891, 1260
};

// Bits each register keeps.
static const uint8_t AY_REG_MASK[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF
};

struct ay8910 {
    uint8_t  reg[16];
    uint8_t  addr;               // latched register number
    uint16_t tone_count[3];
    uint8_t  tone_out[3];
    uint8_t  noise_count;
    uint8_t  noise_half;         // the noise generator runs at half the tone rate
    uint8_t  noise_out;
    uint32_t lfsr;
    uint16_t env_count;
    uint8_t  env_half;
    uint8_t  env_pos;            // 0-15 within a ramp
    uint8_t  env_level;          // 0-15
    bool     env_att, env_alt, env_hold, env_cont, env_holding;
    uint32_t clock_hz;
    uint32_t frac;               // event ticks not yet turned into chip ticks
    int32_t  last;               // last output level
    // PIZERO-248: chip ticks run so far, and the tick each part (tones 0-2,
    // noise, envelope) has been brought up to. A part no one can hear is
    // left behind and caught up exactly when a register write might make
    // it heard (ay_sync).
    uint32_t now;
    uint32_t since[5];
    // PIZERO-248: what the last render found, kept while no register is
    // written: the heard parts, the output, the ticks before a heard part
    // changes, and the ticks the heard parts are owed (taken in ay_flush).
    bool     valid;
    uint8_t  parts;
    int32_t  out;
    uint32_t quiet;
    uint32_t pending;
};

static inline void ay_set_clock(struct ay8910 *c, uint32_t hz) { c->clock_hz = hz; }

static inline void ay_reset(struct ay8910 *c, uint32_t clock_hz) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 3; i++) c->tone_count[i] = 1;
    c->noise_count = 1;
    c->env_count = 1;
    c->lfsr = 1;
    c->env_level = 15;           // shape 0 starts at the top
    c->clock_hz = clock_hz;
}

static inline unsigned ay_tone_period(const struct ay8910 *c, int ch) {
    unsigned tp = ((unsigned)(c->reg[ch * 2 + 1] & 0x0F) << 8) | c->reg[ch * 2];
    return tp ? tp : 1;
}

static inline void ay_env_set_level(struct ay8910 *c) {
    c->env_level = c->env_att ? c->env_pos : (uint8_t)(15 - c->env_pos);
}

static inline void ay_latch(struct ay8910 *c, uint8_t d) { c->addr = d & 0x0F; }

static inline uint8_t ay_read(const struct ay8910 *c) {
    return c->addr >= 14 ? 0xFF : c->reg[c->addr];
}

// One envelope step: the next level of the ramp, or what the shape does at
// its end.
static inline SOUND_HOT void ay_env_step(struct ay8910 *c) {
    if (c->env_holding) return;
    if (++c->env_pos < 16) { ay_env_set_level(c); return; }
    if (!c->env_cont) {                      // shapes 0-7: one ramp, then 0
        c->env_holding = true;
        c->env_level = 0;
    } else if (c->env_hold) {                // stop at the end, or the other end
        c->env_holding = true;
        bool top = c->env_att != c->env_alt;
        c->env_level = top ? 15 : 0;
    } else {                                 // repeat, turning round if ALT
        if (c->env_alt) c->env_att = !c->env_att;
        c->env_pos = 0;
        ay_env_set_level(c);
    }
}

// The summed output level of the chip as it stands.
static inline SOUND_HOT int32_t ay_mix(const struct ay8910 *c) {
    uint8_t mix = c->reg[7];
    int32_t out = 0;
    for (int ch = 0; ch < 3; ch++) {
        bool on = (c->tone_out[ch] | ((mix >> ch) & 1)) & (c->noise_out | ((mix >> (ch + 3)) & 1));
        if (!on) continue;
        uint8_t a = c->reg[8 + ch];
        out += AY_LEVEL[(a & 0x10) ? c->env_level : (a & 0x0F)];
    }
    return out;
}

static inline SOUND_HOT void ay_noise_step(struct ay8910 *c) {
    uint32_t fb = (c->lfsr ^ (c->lfsr >> 3)) & 1;
    c->lfsr = (c->lfsr >> 1) | (fb << 16);
    c->noise_out = c->lfsr & 1;
}

// The parts of the chip, as bits of a mask: tones 0-2, noise, envelope.
enum { AY_PART_NOISE = 1u << 3, AY_PART_ENV = 1u << 4, AY_PARTS = 0x1F };

// One chip tick (clock / 8) for the parts in `parts`; the summed output.
static inline SOUND_HOT int32_t ay_tick_parts(struct ay8910 *c, uint32_t parts) {
    for (int ch = 0; ch < 3; ch++) {
        if ((parts & (1u << ch)) && --c->tone_count[ch] == 0) {
            c->tone_count[ch] = (uint16_t)ay_tone_period(c, ch);
            c->tone_out[ch] ^= 1;
        }
    }
    if ((parts & AY_PART_NOISE) && (c->noise_half ^= 1) == 0 && --c->noise_count == 0) {
        unsigned np = c->reg[6] & 0x1F;
        c->noise_count = (uint8_t)(np ? np : 1);
        ay_noise_step(c);
    }
    if ((parts & AY_PART_ENV) && (c->env_half ^= 1) == 0 && --c->env_count == 0) {
        unsigned ep = ((unsigned)c->reg[12] << 8) | c->reg[11];
        c->env_count = (uint16_t)(ep ? ep : 1);
        ay_env_step(c);
    }
    c->now++;
    return ay_mix(c);
}

// A counter that reloads from `period` each time it runs out, moved on by
// k ticks. Returns how many times it ran out.
static inline SOUND_HOT uint32_t ay_count_skip(uint32_t *count, uint32_t period, uint32_t k) {
    if (k < *count) { *count -= k; return 0; }
    k -= *count;
    *count = period - k % period;
    return 1 + k / period;
}

// The noise and envelope generators count every other tick: the count goes
// down by the ticks that land on the counting half.
static inline SOUND_HOT uint32_t ay_half_skip(uint8_t *half, uint32_t *count,
                                              uint32_t period, uint32_t k) {
    uint32_t d = (k + *half) >> 1;
    *half ^= (uint8_t)(k & 1);
    return ay_count_skip(count, period, d);
}

// Give the heard parts the ticks the quick path of ay_render owes them.
static inline SOUND_HOT void ay_flush(struct ay8910 *c);

// Bring every part left behind up to `now`, exactly as ticking it would
// have. A tone costs a division or two; the noise and envelope take one
// step per time they ran out, and over a run of catch-ups those add up to
// no more than ticking them would have taken.
static inline SOUND_HOT void ay_sync(struct ay8910 *c) {
    ay_flush(c);
    for (int ch = 0; ch < 3; ch++) {
        uint32_t k = c->now - c->since[ch];
        if (!k) continue;
        uint32_t cnt = c->tone_count[ch];
        if (ay_count_skip(&cnt, ay_tone_period(c, ch), k) & 1) c->tone_out[ch] ^= 1;
        c->tone_count[ch] = (uint16_t)cnt;
        c->since[ch] = c->now;
    }
    uint32_t k = c->now - c->since[3];
    if (k) {
        uint32_t np = c->reg[6] & 0x1F, cnt = c->noise_count;
        uint32_t steps = ay_half_skip(&c->noise_half, &cnt, np ? np : 1, k);
        c->noise_count = (uint8_t)cnt;
        while (steps--) ay_noise_step(c);
        c->since[3] = c->now;
    }
    k = c->now - c->since[4];
    if (k) {
        uint32_t ep = ((uint32_t)c->reg[12] << 8) | c->reg[11], cnt = c->env_count;
        uint32_t steps = ay_half_skip(&c->env_half, &cnt, ep ? ep : 1, k);
        c->env_count = (uint16_t)cnt;
        while (steps-- && !c->env_holding) ay_env_step(c);
        c->since[4] = c->now;
    }
}

// Advance one chip tick, every part, and return the summed output level.
static inline SOUND_HOT int32_t ay_tick(struct ay8910 *c) {
    ay_sync(c);
    int32_t out = ay_tick_parts(c, AY_PARTS);
    for (int i = 0; i < 5; i++) c->since[i] = c->now;
    c->valid = false;
    return out;
}

// The parts that can be heard: a tone on in the mixer of a channel with a
// level above 0 (or under the envelope), the noise if such a channel takes
// it, the envelope if a channel is under it and it is still moving. Only
// these can change the output, so only these are run.
static inline SOUND_HOT uint32_t ay_heard(const struct ay8910 *c) {
    uint32_t parts = 0, mix = c->reg[7];
    for (int ch = 0; ch < 3; ch++) {
        uint8_t a = c->reg[8 + ch];
        if (!(a & 0x1F)) continue;
        if (!((mix >> ch) & 1)) parts |= 1u << ch;
        if (!((mix >> (ch + 3)) & 1)) parts |= AY_PART_NOISE;
        if (a & 0x10) parts |= AY_PART_ENV;
    }
    if (c->env_holding) parts &= ~(uint32_t)AY_PART_ENV;
    return parts;
}

// The ticks until a heard part next changes; at least 1.
static inline SOUND_HOT uint32_t ay_quiet_ticks(const struct ay8910 *c, uint32_t parts) {
    uint32_t q = 0xFFFFFFFFu;
    for (int ch = 0; ch < 3; ch++)
        if ((parts & (1u << ch)) && c->tone_count[ch] < q) q = c->tone_count[ch];
    if (parts & AY_PART_NOISE) {
        uint32_t t = 2u * c->noise_count - c->noise_half;
        if (t < q) q = t;
    }
    if (parts & AY_PART_ENV) {
        uint32_t t = 2u * c->env_count - c->env_half;
        if (t < q) q = t;
    }
    return q;
}

// k ticks for the heard parts, fewer than ay_quiet_ticks: nothing runs out.
static inline SOUND_HOT void ay_advance(struct ay8910 *c, uint32_t parts, uint32_t k) {
    for (int ch = 0; ch < 3; ch++)
        if (parts & (1u << ch)) c->tone_count[ch] = (uint16_t)(c->tone_count[ch] - k);
    if (parts & AY_PART_NOISE) {
        c->noise_count = (uint8_t)(c->noise_count - ((k + c->noise_half) >> 1));
        c->noise_half ^= (uint8_t)(k & 1);
    }
    if (parts & AY_PART_ENV) {
        c->env_count = (uint16_t)(c->env_count - ((k + c->env_half) >> 1));
        c->env_half ^= (uint8_t)(k & 1);
    }
    c->now += k;
}

static inline SOUND_HOT void ay_flush(struct ay8910 *c) {
    if (!c->pending) return;
    ay_advance(c, c->parts, c->pending);
    c->pending = 0;
    for (int i = 0; i < 5; i++)
        if (c->parts & (1u << i)) c->since[i] = c->now;
}

// The chip's average output over `event_ticks` of machine time: one audio
// sample's worth. 32-bit arithmetic only: the RP2350 divides 32 bits in
// hardware but 64 in a library call, and that call, once per chip per
// sample, cost about 2 ms a frame per chip (PIZERO-149). A sample is about
// 300 event ticks; the clamp keeps the product in range for any caller.
// PIZERO-248: only the heard parts run (ay_heard), and the ticks in which
// none of them changes are taken in one go. A sample in which nothing heard
// changes, most of them, only counts down what the last render found and
// returns its output. Stepping every part every tick cost about 400 CPU
// clocks a chip a sample on the board, 1.3 ms a frame a chip.
static inline SOUND_HOT int32_t ay_render(struct ay8910 *c, uint32_t event_ticks) {
    if (event_ticks > 16000) event_ticks = 16000;
    uint32_t f = c->frac + event_ticks * (c->clock_hz >> 3);
    uint32_t n = f / AY_EVENT_HZ;
    c->frac = f - n * AY_EVENT_HZ;
    if (n == 0) return c->last;
    if (c->valid && n < c->quiet) {              // nothing heard changes
        c->quiet -= n;
        c->pending += n;
        return c->last = c->out;
    }
    ay_flush(c);
    uint32_t parts = ay_heard(c);
    int32_t out = ay_mix(c), sum = 0;
    for (uint32_t left = n;;) {
        uint32_t q = ay_quiet_ticks(c, parts);
        if (q > left) { ay_advance(c, parts, left); sum += out * (int32_t)left; break; }
        ay_advance(c, parts, q - 1);
        sum += out * (int32_t)(q - 1);
        out = ay_tick_parts(c, parts);
        sum += out;
        if ((parts & AY_PART_ENV) && c->env_holding) {   // it stopped: leave it
            c->since[4] = c->now;
            parts &= ~(uint32_t)AY_PART_ENV;
        }
        if ((left -= q) == 0) break;
    }
    for (int i = 0; i < 5; i++)
        if (parts & (1u << i)) c->since[i] = c->now;
    c->parts = (uint8_t)parts;
    c->out = out;
    c->quiet = ay_quiet_ticks(c, parts);
    c->valid = true;
    c->last = sum / (int32_t)n;
    return c->last;
}

// A register write. Every part is caught up first, as the write may make
// one heard (or change a period it was being counted with).
static inline void ay_write(struct ay8910 *c, uint8_t d) {
    ay_sync(c);
    c->valid = false;
    unsigned r = c->addr;
    c->reg[r] = d & AY_REG_MASK[r];
    if (r == 13) {                           // a new shape restarts the envelope
        c->env_cont = (d & 8) != 0;
        c->env_att  = (d & 4) != 0;
        c->env_alt  = (d & 2) != 0;
        c->env_hold = (d & 1) != 0;
        c->env_pos = 0;
        c->env_holding = false;
        c->env_count = 1;
        c->env_half = 0;
        ay_env_set_level(c);
    }
}

// - - - the cartridges - - - - - - - - - - - - - - - - - - - - - - - - - -

enum { AYB_PSG = 1, AYB_SYM12 = 2 };   // which front-ends answer

struct ay_bank {
    struct ay8910 chip[AY_CHIPS];
    uint8_t  enabled;            // AYB_* the settings allow
    uint8_t  active;             // bit per chip: written since reset, so rendered
    uint8_t  psg_ctrl;           // CoCo PSG $FF5D
    uint8_t  ora, ddra, cra;     // Symphony-12's PIA
    uint8_t  orb, ddrb, crb;
    uint8_t  stereo;             // PIZERO-250: the Symphony-12's stereo jumper
    uint8_t  sym12;              // bit per chip: last written by the Symphony-12
    uint32_t writes;             // register writes, for telemetry
};

static inline void ayb_reset(struct ay_bank *b) {
    uint8_t en = b->enabled, st = b->stereo;
    memset(b, 0, sizeof *b);
    b->enabled = en;
    b->stereo = st;
    for (int i = 0; i < AY_CHIPS; i++) ay_reset(&b->chip[i], AY_CLK_PSG_2MHZ);
}

static inline void ayb_chip_write(struct ay_bank *b, int k, uint8_t d) {
    ay_write(&b->chip[k], d);
    b->active |= (uint8_t)(1u << k);
    b->writes++;
}

// Symphony-12: chips act on what the PIA drives (outputs only; an input bit
// reads as 0, so a port left as input does nothing).
static inline void ayb_sym12_bus(struct ay_bank *b) {
    uint8_t ctl = b->orb & b->ddrb;
    uint8_t data = b->ora & b->ddra;
    for (int k = 0; k < AY_CHIPS; k++) {
        unsigned f = (ctl >> (2 * k)) & 3;
        if (f == 3) ay_latch(&b->chip[k], data);
        else if (f == 2) {
            ayb_chip_write(b, k, data);
            b->sym12 |= (uint8_t)(1u << k);
        }
    }
}

static inline uint8_t ayb_sym12_read_a(const struct ay_bank *b) {
    uint8_t ctl = b->orb & b->ddrb, d = 0;
    for (int k = 0; k < AY_CHIPS; k++)
        if (((ctl >> (2 * k)) & 3) == 1) d |= ay_read(&b->chip[k]);
    // Bits driven as outputs read back what the PIA drives.
    return (uint8_t)((d & ~b->ddra) | (b->ora & b->ddra));
}

static inline void ayb_sym12_clock(struct ay_bank *b) {
    for (int k = 0; k < AY_CHIPS; k++) ay_set_clock(&b->chip[k], AY_CLK_SYM12);
}

// True when `a` is an address some enabled front-end answers. One compare
// covers the span, so the bus pays little for addresses outside it.
static inline bool ayb_owns(const struct ay_bank *b, uint16_t a) {
    if ((uint16_t)(a - 0xFF5D) > (0xFF63 - 0xFF5D) || !b->enabled) return false;
    if (a <= 0xFF5F) return b->enabled & AYB_PSG;
    return b->enabled & AYB_SYM12;
}

static inline void ayb_write(struct ay_bank *b, uint16_t a, uint8_t d) {
    switch (a) {
    case 0xFF5D:
        b->psg_ctrl = d;
        ay_set_clock(&b->chip[0], (d & 1) ? AY_CLK_PSG_1MHZ : AY_CLK_PSG_2MHZ);
        break;
    case 0xFF5E: ay_latch(&b->chip[0], d); break;
    case 0xFF5F:
        ay_set_clock(&b->chip[0], (b->psg_ctrl & 1) ? AY_CLK_PSG_1MHZ : AY_CLK_PSG_2MHZ);
        ayb_chip_write(b, 0, d);
        b->sym12 &= (uint8_t)~1u;
        break;
    case 0xFF60:
        if (b->cra & 4) b->ora = d; else b->ddra = d;
        ayb_sym12_clock(b);
        ayb_sym12_bus(b);
        break;
    case 0xFF61: b->cra = d & 0x3F; break;
    case 0xFF62:
        if (b->crb & 4) b->orb = d; else b->ddrb = d;
        ayb_sym12_clock(b);
        ayb_sym12_bus(b);
        break;
    case 0xFF63: b->crb = d & 0x3F; break;
    default: break;
    }
}

static inline uint8_t ayb_read(const struct ay_bank *b, uint16_t a) {
    switch (a) {
    case 0xFF5F: return ay_read(&b->chip[0]);
    case 0xFF60: return (b->cra & 4) ? ayb_sym12_read_a(b) : b->ddra;
    case 0xFF61: return b->cra;
    case 0xFF62: return (b->crb & 4) ? (uint8_t)(b->orb & b->ddrb) : b->ddrb;
    case 0xFF63: return b->crb;
    default:     return 0xFF;   // $FF5D is write-only
    }
}

// Every chip in use, mixed: one chip at its own level, more scaled down so
// four at full are 1.6 times one rather than four times. A chip with all
// three levels at 0 is silent whatever its tones do, so it is not run.
//
// A side of the mix: n chips scaled so four alike are 1.6 times one.
static inline int32_t ayb_scale(int32_t sum, int n) {
    return n <= 1 ? sum : sum * 2 / (n + 1);
}

// PIZERO-250: with the stereo jumper on, a chip the Symphony-12 last wrote
// plays on one side only, chips 1 and 2 left and 3 and 4 right, as MAME
// routes them. A chip the CoCo PSG wrote stays in the centre.
// Each side is mixed like the mono bank, by the chips on that side, so one
// Symphony-12 chip on its own is as loud as a CoCo PSG.
//
// PIZERO-253: where each chip's sound goes when the CoCo's multiplexer is
// modelled (`routed`). The CoCo PSG's chip, and the Symphony-12 in mono,
// come back through the cartridge's sound line, which the multiplexer
// switches: *line. The Symphony-12 in stereo plays from its own jacks, so it
// does not pass the multiplexer: *left and *right. Unrouted (every source heard at once)
// everything is in *left and *right and *line is 0.
static inline SOUND_HOT void ayb_render_routed(struct ay_bank *b, uint32_t event_ticks, bool routed,
                                               int32_t *line, int32_t *left, int32_t *right) {
    int32_t sc = 0, sl = 0, sr = 0, sm = 0;
    int nc = 0, nl = 0, nr = 0, nm = 0;
    const uint8_t panned = b->stereo ? b->sym12 : 0;
    for (int k = 0; k < AY_CHIPS; k++) {
        if (!(b->active & (1u << k))) continue;
        const uint8_t *r = b->chip[k].reg;
        if (!(r[8] | r[9] | r[10])) continue;
        int32_t v = ay_render(&b->chip[k], event_ticks);
        if (panned & (1u << k)) {
            if (k < AY_CHIPS / 2) { sl += v; nl++; } else { sr += v; nr++; }
        } else if (routed) {
            sm += v; nm++;
        } else {
            sc += v; nc++;
        }
    }
    *line = ayb_scale(sm, nm);
    *left = ayb_scale(sc + sl, nc + nl);
    *right = ayb_scale(sc + sr, nc + nr);
}

static inline SOUND_HOT void ayb_render_lr(struct ay_bank *b, uint32_t event_ticks,
                                           int32_t *left, int32_t *right) {
    int32_t line;
    ayb_render_routed(b, event_ticks, false, &line, left, right);
}

// The mono mix, which is what the bank gives with the jumper off.
static inline int32_t ayb_render(struct ay_bank *b, uint32_t event_ticks) {
    int32_t l, r;
    ayb_render_lr(b, event_ticks, &l, &r);
    return (l + r) / 2;
}

#endif  // PSG_AY8910_H

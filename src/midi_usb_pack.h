// midi_usb_pack.h: the bytes the CoCo writes to the MIDI Pak, as USB-MIDI
// event packets (PIZERO-287). Pure logic, host-tested (test/test_midi_usb_pack).
//
// A USB-MIDI packet is 4 bytes: cable number and Code Index Number (CIN),
// then up to 3 MIDI bytes. The CoCo writes a plain MIDI byte stream, one
// byte at a time, so this follows MIDI's own rules to cut it into messages:
//   * channel messages: running status (a data byte with no status repeats
//     the last channel status); 2 data bytes, or 1 for program change ($Cx)
//     and channel pressure ($Dx)
//   * system common: $F1 and $F3 take 1 data byte, $F2 takes 2, $F6 none
//   * SysEx ($F0 ... $F7): carried 3 bytes a packet (CIN 4), its end in
//     CIN 5, 6 or 7 with the last 1 to 3 bytes
//   * real time ($F8-$FF): a packet of its own at once, even mid-message
//   * a status byte arriving before a message is complete starts the new
//     one; the unfinished one is dropped, never sent garbled
// Found on hardware 2026-11-01: the library's own stream parser, given a
// lone $90 left by a program that stopped mid-message, sent "90 10 3C" when
// the next note came.

#ifndef MIDI_USB_PACK_H
#define MIDI_USB_PACK_H

#include <stdbool.h>
#include <stdint.h>

struct midi_pack {
    uint8_t status;      // running status (channel) or the common message being built
    uint8_t need;        // data bytes the current message wants
    uint8_t have;        // data bytes collected
    uint8_t data[3];     // for SysEx: up to 3 bytes waiting for a packet
    bool    sysex;
};

static inline void midi_pack_init(struct midi_pack *m) {
    m->status = 0; m->need = 0; m->have = 0; m->sysex = false;
}

// Feed one byte. Returns true and fills out[4] when a packet is ready.
// (Real-time bytes and channel messages never need two packets from one byte;
// a SysEx data byte can complete a 3-byte chunk, which also fits.)
static inline bool midi_pack_byte(struct midi_pack *m, uint8_t b, uint8_t out[4]) {
    if (b >= 0xF8) {                                  // real time: alone, at once
        out[0] = 0x0F; out[1] = b; out[2] = 0; out[3] = 0;
        return true;
    }
    if (m->sysex) {
        if (b == 0xF7) {                              // end: 1 to 3 bytes in the last packet
            m->sysex = false;
            m->data[m->have++] = 0xF7;
            out[0] = (uint8_t)(m->have == 1 ? 0x05 : m->have == 2 ? 0x06 : 0x07);
            out[1] = m->data[0];
            out[2] = m->have > 1 ? m->data[1] : 0;
            out[3] = m->have > 2 ? m->data[2] : 0;
            m->have = 0; m->status = 0;
            return true;
        }
        if (b & 0x80) {                               // another status ends SysEx unfinished: drop it
            m->sysex = false; m->have = 0;
        } else {
            m->data[m->have++] = b;
            if (m->have == 3) {
                out[0] = 0x04; out[1] = m->data[0]; out[2] = m->data[1]; out[3] = m->data[2];
                m->have = 0;
                return true;
            }
            return false;
        }
    }
    if (b == 0xF0) {                                  // SysEx starts
        m->sysex = true; m->data[0] = 0xF0; m->have = 1; m->status = 0;
        return false;
    }
    if (b & 0x80) {                                   // a status byte: a new message
        m->have = 0;
        if (b >= 0xF0) {                              // system common (not SysEx)
            m->status = b;
            m->need = (b == 0xF2) ? 2 : (b == 0xF1 || b == 0xF3) ? 1 : 0;
            if (m->need == 0) {                       // $F6 tune request (or an undefined one)
                m->status = 0;
                if (b == 0xF7) return false;          // a stray end of SysEx: nothing to send
                out[0] = 0x05; out[1] = b; out[2] = 0; out[3] = 0;
                return true;
            }
            return false;
        }
        m->status = b;
        m->need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
        return false;
    }
    if (!m->status) return false;                     // data with nothing to belong to
    m->data[m->have++] = b;
    if (m->have < m->need) return false;
    if (m->status >= 0xF0) {
        out[0] = (uint8_t)(m->need == 1 ? 0x02 : 0x03);
    } else {
        out[0] = (uint8_t)(m->status >> 4);           // CIN = the channel message's high nibble
    }
    out[1] = m->status;
    out[2] = m->data[0];
    out[3] = m->need > 1 ? m->data[1] : 0;
    m->have = 0;
    if (m->status >= 0xF0) m->status = 0;             // system common has no running status
    return true;
}

// A USB-MIDI packet back to MIDI bytes: how many of out[0..2] are real, by
// the Code Index Number (USB MIDI 1.0, table 4-1). Reserved and cable
// events give 0.
static inline int midi_unpack(const uint8_t pk[4], uint8_t out[3]) {
    static const uint8_t len[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    int n = len[pk[0] & 0x0F];
    for (int i = 0; i < n; i++) out[i] = pk[1 + i];
    return n;
}

#endif  // MIDI_USB_PACK_H

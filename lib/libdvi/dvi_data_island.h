// SPDX-License-Identifier: MIT
// HDMI data-island packet encoder (PIZERO-30 / PIZERO-27 M1).
// Ported from shuichitakano/pico_lib (MIT, © Shuichi Takano); see LICENSE /
// PROVENANCE.md in this directory.
//
// Pure data transformation — no Pico SDK / hardware dependencies — so it builds
// and is unit-tested on the host (see test/di_test.c) before it touches the
// timing-critical libdvi DMA path in M2 (PIZERO-28). Ported from the
// same-library reference (shuichitakano/pico_lib dvi/data_packet.cpp), with the
// BCH table generated from the HDMI ECC polynomial (LSB-first, 0x83) rather than
// transcribed.
//
// An HDMI data island is 32 pixels. Lane 0 bit 2 carries the 32-bit packet
// header (3 data bytes + 8-bit BCH parity); lanes 1 & 2 carry four 8-byte
// subpackets (7 data bytes + 8-bit BCH parity each). TERC4 maps each 4-bit
// nibble to a 10-bit symbol.

#ifndef DVI_DATA_ISLAND_H
#define DVI_DATA_ISLAND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t header[4];        // [0..2] data, [3] BCH parity
	uint8_t subpacket[4][8];  // [i][0..6] data, [i][7] BCH parity
} dvi_data_packet_t;

// TERC4 4-bit -> 10-bit symbol table. Guard-band is a fixed 10-bit symbol.
extern const uint16_t dvi_terc4_syms[16];
#define DVI_DI_GUARDBAND_SYM 0x133u   // 0b0100110011

// HDMI sampling-frequency codes (Audio InfoFrame SF field / matches CEA-861).
#define DVI_AUDIO_SF_32K   1
#define DVI_AUDIO_SF_44K1  2
#define DVI_AUDIO_SF_48K   3
// Sample-size codes (SS field): 1 = 16-bit.
#define DVI_AUDIO_SS_16    1

// One-time: build the BCH-8 parity table.
void dvi_di_init(void);

// Fill header[3] and subpacket[i][7] with BCH parity. Call after a builder.
void dvi_di_compute_parity(dvi_data_packet_t *pkt);

// Packet builders. Each clears the packet then sets its fields; the caller then
// calls dvi_di_compute_parity().
void dvi_di_set_null(dvi_data_packet_t *pkt);
void dvi_di_set_acr(dvi_data_packet_t *pkt, uint32_t cts, uint32_t n);
void dvi_di_set_audio_infoframe(dvi_data_packet_t *pkt, uint8_t channels,
                                uint8_t sf_code, uint8_t ss_code);
void dvi_di_set_avi_infoframe(dvi_data_packet_t *pkt, uint8_t vic);
void dvi_di_set_gcp(dvi_data_packet_t *pkt, bool set_avmute);
// Pack up to 4 stereo frames (interleaved L,R signed 16-bit) into one Audio
// Sample Packet. frame_ctr is the running IEC frame counter (for B-bit / channel
// status framing). NOTE: the IEC 60958 bit layout is finalised/verified against
// the monitor in M4 (PIZERO-30); host tests cover structure + parity only.
void dvi_di_set_audio_samples(dvi_data_packet_t *pkt, const int16_t *lr,
                              int nframes, uint32_t frame_ctr);

// TERC4-encode a packet to per-lane symbol words: 2 symbols per 32-bit word,
// low 10 bits = earlier pixel. dst0 = lane 0 (header), dst1/dst2 = data lanes;
// each receives 16 words (32 pixels). hv carries the data-island-period control
// bits (hsync/vsync); first_packet marks the first packet after a guard band.
void dvi_di_encode_header(uint32_t *dst0, const dvi_data_packet_t *pkt,
                          int hv, bool first_packet);
void dvi_di_encode_subpacket(uint32_t *dst1, uint32_t *dst2,
                             const dvi_data_packet_t *pkt);

// Pack two TERC4 nibbles into one word (low symbol = earlier pixel).
static inline uint32_t dvi_di_terc4x2(unsigned a, unsigned b) {
	return (uint32_t)dvi_terc4_syms[a & 15] | ((uint32_t)dvi_terc4_syms[b & 15] << 10);
}

#ifdef __cplusplus
}
#endif
#endif

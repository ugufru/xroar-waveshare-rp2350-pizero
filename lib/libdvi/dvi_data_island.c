// HDMI data-island packet encoder — see dvi_data_island.h.
// Ported from shuichitakano/pico_lib dvi/data_packet.cpp (same library lineage
// as our vendored libdvi). BCH table generated from the HDMI ECC polynomial.

#include "dvi_data_island.h"
#include <string.h>

// TERC4: 4-bit -> 10-bit symbols (HDMI 1.4 spec / reference, verbatim).
const uint16_t dvi_terc4_syms[16] = {
	0x29c, 0x263, 0x2e4, 0x2e2,   // 0b1010011100 0b1001100011 0b1011100100 0b1011100010
	0x171, 0x11e, 0x18e, 0x13c,   // 0b0101110001 0b0100011110 0b0110001110 0b0100111100
	0x2cc, 0x139, 0x19c, 0x2c6,   // 0b1011001100 0b0100111001 0b0110011100 0b1011000110
	0x28e, 0x271, 0x163, 0x2c3,   // 0b1010001110 0b1001110001 0b0101100011 0b1011000011
};

// BCH-8 parity table: LSB-first, polynomial 0x83 (the HDMI ECC generator
// x^8+x^7+x^6+x^4+x^2+1). Verified to reproduce the reference table's known
// bytes (0x00,0xd9,0xb5,0x6c,...). Generated once by dvi_di_init().
static uint8_t bch_table[256];
static bool    bch_ready = false;

void dvi_di_init(void) {
	for (int n = 0; n < 256; ++n) {
		int c = n;
		for (int i = 0; i < 8; ++i)
			c = (c & 1) ? ((c >> 1) ^ 0x83) : (c >> 1);
		bch_table[n] = (uint8_t)c;
	}
	bch_ready = true;
}

static uint8_t bch(const uint8_t *p, int n) {
	uint8_t v = 0;
	for (int i = 0; i < n; ++i)
		v = bch_table[p[i] ^ v];
	return v;
}

void dvi_di_compute_parity(dvi_data_packet_t *pkt) {
	pkt->header[3] = bch(pkt->header, 3);
	for (int i = 0; i < 4; ++i)
		pkt->subpacket[i][7] = bch(pkt->subpacket[i], 7);
}

void dvi_di_set_null(dvi_data_packet_t *pkt) {
	memset(pkt, 0, sizeof(*pkt));   // Null packet: type 0, all zero.
}

// HDMI Audio Clock Regeneration: header type 1; each of the 4 subpackets carries
// the same 20-bit CTS and N values (SB0 reserved=0).
void dvi_di_set_acr(dvi_data_packet_t *pkt, uint32_t cts, uint32_t n) {
	memset(pkt, 0, sizeof(*pkt));
	pkt->header[0] = 0x01;
	for (int i = 0; i < 4; ++i) {
		pkt->subpacket[i][0] = 0;
		pkt->subpacket[i][1] = (uint8_t)(cts >> 16) & 0x0f;
		pkt->subpacket[i][2] = (uint8_t)(cts >> 8);
		pkt->subpacket[i][3] = (uint8_t)(cts);
		pkt->subpacket[i][4] = (uint8_t)(n >> 16) & 0x0f;
		pkt->subpacket[i][5] = (uint8_t)(n >> 8);
		pkt->subpacket[i][6] = (uint8_t)(n);
	}
}

// Lay an InfoFrame (type/version/length header + payload bytes PB1..PBlen) into
// a packet, computing the CEA checksum into PB0. Payload occupies the four
// subpackets, 7 bytes each: PB0..6 -> sp[0], PB7..13 -> sp[1], etc.
static void set_infoframe(dvi_data_packet_t *pkt, uint8_t type, uint8_t ver,
                          uint8_t len, const uint8_t *pb /* pb[1..len] */) {
	memset(pkt, 0, sizeof(*pkt));
	pkt->header[0] = type;
	pkt->header[1] = ver;
	pkt->header[2] = len;
	uint8_t sum = type + ver + len;
	for (int k = 1; k <= len; ++k) sum += pb[k];
	uint8_t pb0 = (uint8_t)(0x100 - sum);   // checksum so total & 0xff == 0
	// Scatter PB0..PBlen across the subpackets (7 bytes each).
	for (int k = 0; k <= len; ++k) {
		uint8_t v = (k == 0) ? pb0 : pb[k];
		pkt->subpacket[k / 7][k % 7] = v;
	}
}

// Minimal AVI InfoFrame (type 0x82, v2, len 13). RGB, no special colorimetry.
void dvi_di_set_avi_infoframe(dvi_data_packet_t *pkt, uint8_t vic) {
	uint8_t pb[14] = {0};
	pb[1] = 0x00;   // Y=0 (RGB), no active-format info
	pb[2] = 0x08;   // R=8 (active portion = no data), no aspect override
	pb[3] = 0x00;
	pb[4] = vic;    // Video Identification Code (0 = unknown / not a CEA mode)
	pb[5] = 0x00;   // pixel repetition = none
	set_infoframe(pkt, 0x82, 2, 13, pb);
}

// Audio InfoFrame (type 0x84, v1, len 10). channels = count-1 (0 => 2ch refer).
void dvi_di_set_audio_infoframe(dvi_data_packet_t *pkt, uint8_t channels,
                                uint8_t sf_code, uint8_t ss_code) {
	uint8_t pb[11] = {0};
	pb[1] = (uint8_t)(channels & 0x07);                 // CC (CT=0: refer to stream)
	pb[2] = (uint8_t)(((sf_code & 0x07) << 2) | (ss_code & 0x03));
	pb[3] = 0x00;                                       // format
	pb[4] = 0x00;                                       // CA: channel allocation
	pb[5] = 0x00;                                       // LFE/level shift/downmix
	set_infoframe(pkt, 0x84, 1, 10, pb);
}

// General Control Packet (type 0x03): AV-mute control + color-depth (24-bit).
void dvi_di_set_gcp(dvi_data_packet_t *pkt, bool set_avmute) {
	memset(pkt, 0, sizeof(*pkt));
	pkt->header[0] = 0x03;
	pkt->subpacket[0][0] = set_avmute ? 0x01 : 0x10;    // SET_AVMUTE / CLR_AVMUTE
	pkt->subpacket[0][1] = 0x00;                        // CD=0 (24-bit), PP=0
}

// Audio Sample Packet (type 0x02). nframes (1..4) IEC 60958 stereo frames.
// header[1] = sample_present (one bit per subpacket); header[2] = sample_flat=0
// + B-bit position. Per HDMI, 16-bit PCM occupies bits 8..23 of the 24-bit IEC
// word. VUCP: validity=0, user=0, channel-status bit from frame_ctr framing,
// parity over bits. (Exact IEC bit positions confirmed on the sink in M4.)
// Even parity of a byte (XOR-reduce of its 8 bits).
static inline int iec_par(unsigned b) { b ^= b >> 4; b ^= b >> 2; b ^= b >> 1; return b & 1; }

// IEC 60958 consumer channel-status bit for frame index f (0..191). Minimal
// block: byte 0 = 0x00 (consumer, linear PCM, no copyright), byte 3 = 0x03
// (sampling frequency = 32 kHz, per AES3 consumer code 3). All other bytes 0.
// The C bit of frame f is bit f of this 192-bit block; byte 3 = bits 24..31, so
// 0x03 means frames 24 and 25 carry a 1, everything else 0. Declaring 32 kHz
// here keeps the channel status consistent with the ACR + Audio InfoFrame (a
// rate conflict makes sinks resample to garbage).
static inline int iec_cs_bit(uint32_t f) { return (f == 24u || f == 25u) ? 1 : 0; }

void dvi_di_set_audio_samples(dvi_data_packet_t *pkt, const int16_t *lr,
                              int nframes, uint32_t frame_ctr) {
	memset(pkt, 0, sizeof(*pkt));
	if (nframes < 1) nframes = 1;
	if (nframes > 4) nframes = 4;
	const unsigned B = (frame_ctr % 192u == 0) ? 1u : 0u;   // 192-frame IEC block start
	pkt->header[0] = 0x02;                                   // Audio Sample Packet
	pkt->header[1] = (uint8_t)((1u << nframes) - 1);         // layout=0 (2ch) | sample_present
	pkt->header[2] = (uint8_t)(B << 4);                      // B.0 preamble bit (block start)
	for (int i = 0; i < nframes; ++i) {
		uint16_t l = (uint16_t)lr[2 * i + 0];
		uint16_t r = (uint16_t)lr[2 * i + 1];
		uint8_t *d = pkt->subpacket[i];
		// IEC 60958 subframe: 16-bit sample left-justified in the 24-bit field,
		// little-endian bytes [0..2] (left) / [3..5] (right): d0=field[7:0]=0,
		// d1=sample[7:0], d2=sample[15:8].
		d[0] = 0; d[1] = (uint8_t)l; d[2] = (uint8_t)(l >> 8);
		d[3] = 0; d[4] = (uint8_t)r; d[5] = (uint8_t)(r >> 8);
		// V=0 (VALID -- IEC 60958: 0 = suitable for D/A), U=0, C from the channel-
		// status block. Parity P makes subframe slots 4..31 even.
		const unsigned V = 0, U = 0;
		const unsigned C = (unsigned)iec_cs_bit((frame_ctr + (uint32_t)i) % 192u);
		const unsigned vuc = V ^ U ^ C;
		int pl = iec_par(d[0]) ^ iec_par(d[1]) ^ iec_par(d[2]) ^ (int)vuc;
		int pr = iec_par(d[3]) ^ iec_par(d[4]) ^ iec_par(d[5]) ^ (int)vuc;
		// Byte 6: [V U C P] for left in bits 0..3, right in bits 4..7.
		d[6] = (uint8_t)((V << 0) | (U << 1) | (C << 2) | ((unsigned)pl << 3) |
		                 (V << 4) | (U << 5) | (C << 6) | ((unsigned)pr << 7));
	}
}

// ---- TERC4 island layout (ported verbatim from the reference) --------------

// Lane 0: 32-bit header, 1 bit/pixel in nibble bit 2, plus the hv control bits.
void dvi_di_encode_header(uint32_t *dst, const dvi_data_packet_t *pkt,
                          int hv, bool first_packet) {
	int hv1 = hv | 8;
	if (!first_packet) hv = hv1;
	for (int i = 0; i < 4; ++i) {
		unsigned h = pkt->header[i];
		dst[0] = dvi_di_terc4x2(((h << 2) & 4) | hv,  ((h << 1) & 4) | hv1);
		dst[1] = dvi_di_terc4x2((h & 4) | hv1,        ((h >> 1) & 4) | hv1);
		dst[2] = dvi_di_terc4x2(((h >> 2) & 4) | hv1, ((h >> 3) & 4) | hv1);
		dst[3] = dvi_di_terc4x2(((h >> 4) & 4) | hv1, ((h >> 5) & 4) | hv1);
		dst += 4;
		hv = hv1;
	}
}

// Lanes 1 & 2: four subpackets, bit-permuted so each pixel's nibble spans the
// four subpackets (reference bit-twiddle).
void dvi_di_encode_subpacket(uint32_t *dst1, uint32_t *dst2,
                             const dvi_data_packet_t *pkt) {
	for (int i = 0; i < 8; ++i) {
		uint32_t v = ((uint32_t)pkt->subpacket[0][i] << 0)  |
		             ((uint32_t)pkt->subpacket[1][i] << 8)  |
		             ((uint32_t)pkt->subpacket[2][i] << 16) |
		             ((uint32_t)pkt->subpacket[3][i] << 24);
		uint32_t t = (v ^ (v >> 7)) & 0x00aa00aa; v = v ^ t ^ (t << 7);
		t = (v ^ (v >> 14)) & 0x0000cccc;          v = v ^ t ^ (t << 14);
		dst1[0] = dvi_di_terc4x2((v >> 0) & 15,  (v >> 16) & 15);
		dst1[1] = dvi_di_terc4x2((v >> 4) & 15,  (v >> 20) & 15);
		dst2[0] = dvi_di_terc4x2((v >> 8) & 15,  (v >> 24) & 15);
		dst2[1] = dvi_di_terc4x2((v >> 12) & 15, (v >> 28) & 15);
		dst1 += 2;
		dst2 += 2;
	}
}

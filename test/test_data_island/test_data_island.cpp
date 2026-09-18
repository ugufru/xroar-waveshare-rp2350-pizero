// PIZERO-109: host tests for the HDMI data-island packet builders.
//
// These are the packets the sink uses to decide how to interpret our stream:
// ACR tells it how to reconstruct the audio clock, the InfoFrames describe the
// video and audio format, and every packet carries BCH parity the sink checks.
// A wrong field here is invisible in a code review and shows up as a monitor
// that mutes, buzzes, or refuses the mode.
//
// The .c is compiled straight into the test via its own DVI_DI_HOST_TEST path,
// so this exercises the firmware's encoder rather than a re-implementation.
//
//   pio test -e native

#include <unity.h>

#define DVI_DI_HOST_TEST 1
#include "../../lib/libdvi/dvi_data_island.c"

// --- ACR ------------------------------------------------------------------
// The numbers PIZERO-32 turns on: the sink recovers the audio clock from
// CTS/N against the pixel clock it ASSUMES we are using. CTS=25176 with
// N=6144 is what made pitch match desktop XRoar.

static void test_acr_layout_is_20_bit_cts_and_n(void) {
    dvi_data_packet_t pkt;
    dvi_di_set_acr(&pkt, 25176, 6144);
    TEST_ASSERT_EQUAL_HEX8(0x01, pkt.header[0]);          // ACR packet type
    for (int i = 0; i < 4; i++) {                          // repeated in all 4
        TEST_ASSERT_EQUAL_HEX8(0x00, pkt.subpacket[i][0]);
        TEST_ASSERT_EQUAL_HEX8((25176 >> 16) & 0x0F, pkt.subpacket[i][1]);
        TEST_ASSERT_EQUAL_HEX8((25176 >> 8) & 0xFF, pkt.subpacket[i][2]);
        TEST_ASSERT_EQUAL_HEX8(25176 & 0xFF, pkt.subpacket[i][3]);
        TEST_ASSERT_EQUAL_HEX8((6144 >> 16) & 0x0F, pkt.subpacket[i][4]);
        TEST_ASSERT_EQUAL_HEX8((6144 >> 8) & 0xFF, pkt.subpacket[i][5]);
        TEST_ASSERT_EQUAL_HEX8(6144 & 0xFF, pkt.subpacket[i][6]);
    }
}

static void test_acr_keeps_the_top_nibble_of_a_20_bit_value(void) {
    // CTS and N are 20-bit fields. A value above 16 bits must not lose its
    // top nibble, which a uint8_t cast without the mask would do silently.
    dvi_data_packet_t pkt;
    dvi_di_set_acr(&pkt, 0xFDCBA, 0xF1234);
    TEST_ASSERT_EQUAL_HEX8(0x0F, pkt.subpacket[0][1]);     // CTS[19:16]
    TEST_ASSERT_EQUAL_HEX8(0xDC, pkt.subpacket[0][2]);     // CTS[15:8]
    TEST_ASSERT_EQUAL_HEX8(0xBA, pkt.subpacket[0][3]);     // CTS[7:0]
    TEST_ASSERT_EQUAL_HEX8(0x0F, pkt.subpacket[0][4]);     // N[19:16]
    TEST_ASSERT_EQUAL_HEX8(0x12, pkt.subpacket[0][5]);     // N[15:8]
    TEST_ASSERT_EQUAL_HEX8(0x34, pkt.subpacket[0][6]);     // N[7:0]

    // And a value that overflows 20 bits must lose the excess, not smear it
    // into the next field.
    dvi_di_set_acr(&pkt, 0x1FFFFF, 0);
    TEST_ASSERT_EQUAL_HEX8(0x0F, pkt.subpacket[0][1]);
}

// --- InfoFrames -----------------------------------------------------------
// The spec rule: the checksum makes the whole InfoFrame sum to zero mod 256.
// A sink that checks it drops the frame if we get it wrong.

static uint8_t infoframe_sum(const dvi_data_packet_t *pkt, int len) {
    uint8_t sum = (uint8_t)(pkt->header[0] + pkt->header[1] + pkt->header[2]);
    for (int k = 0; k <= len; k++) sum = (uint8_t)(sum + pkt->subpacket[k / 7][k % 7]);
    return sum;
}

static void test_audio_infoframe_header_and_checksum(void) {
    dvi_data_packet_t pkt;
    dvi_di_set_audio_infoframe(&pkt, 2, DVI_AUDIO_SF_48K, DVI_AUDIO_SS_16);
    TEST_ASSERT_EQUAL_HEX8(0x84, pkt.header[0]);          // audio InfoFrame
    TEST_ASSERT_EQUAL_HEX8(0x01, pkt.header[1]);          // version 1
    TEST_ASSERT_EQUAL_HEX8(10, pkt.header[2]);            // length 10
    TEST_ASSERT_EQUAL_HEX8(0x00, infoframe_sum(&pkt, 10));
}

static void test_audio_infoframe_sample_rate_and_size_fields(void) {
    dvi_data_packet_t pkt;
    dvi_di_set_audio_infoframe(&pkt, 2, DVI_AUDIO_SF_48K, DVI_AUDIO_SS_16);
    TEST_ASSERT_EQUAL_HEX8(2, pkt.subpacket[0][1] & 0x07);             // 2 channels
    TEST_ASSERT_EQUAL_HEX8(DVI_AUDIO_SF_48K, (pkt.subpacket[0][2] >> 2) & 0x07);
    TEST_ASSERT_EQUAL_HEX8(DVI_AUDIO_SS_16, pkt.subpacket[0][2] & 0x03);

    dvi_di_set_audio_infoframe(&pkt, 2, DVI_AUDIO_SF_32K, DVI_AUDIO_SS_16);
    TEST_ASSERT_EQUAL_HEX8(DVI_AUDIO_SF_32K, (pkt.subpacket[0][2] >> 2) & 0x07);
    TEST_ASSERT_EQUAL_HEX8(0x00, infoframe_sum(&pkt, 10));             // still valid
}

static void test_avi_infoframe_header_and_checksum(void) {
    dvi_data_packet_t pkt;
    dvi_di_set_avi_infoframe(&pkt, 1);                    // VIC 1 = 640x480p60
    TEST_ASSERT_EQUAL_HEX8(0x82, pkt.header[0]);
    TEST_ASSERT_EQUAL_HEX8(2, pkt.header[1]);             // version 2
    TEST_ASSERT_EQUAL_HEX8(13, pkt.header[2]);            // length 13
    TEST_ASSERT_EQUAL_HEX8(1, pkt.subpacket[0][4]);       // VIC lands in PB4
    TEST_ASSERT_EQUAL_HEX8(0x00, infoframe_sum(&pkt, 13));
}

static void test_infoframe_payload_spills_into_the_next_subpacket(void) {
    // PB0..PB13 do not fit in one 7-byte subpacket; PB7 onwards must land in
    // the second. Getting the scatter wrong corrupts every long InfoFrame.
    dvi_data_packet_t pkt;
    dvi_di_set_avi_infoframe(&pkt, 0x2A);
    TEST_ASSERT_EQUAL_HEX8(0x2A, pkt.subpacket[0][4]);
    for (int k = 7; k <= 13; k++)
        TEST_ASSERT_EQUAL_HEX8(0x00, pkt.subpacket[k / 7][k % 7]);
    TEST_ASSERT_EQUAL_HEX8(0x00, infoframe_sum(&pkt, 13));
}

// --- General Control Packet ----------------------------------------------

static void test_gcp_mute_bits(void) {
    dvi_data_packet_t pkt;
    dvi_di_set_gcp(&pkt, true);
    TEST_ASSERT_EQUAL_HEX8(0x03, pkt.header[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, pkt.subpacket[0][0]);    // SET_AVMUTE
    dvi_di_set_gcp(&pkt, false);
    TEST_ASSERT_EQUAL_HEX8(0x10, pkt.subpacket[0][0]);    // CLR_AVMUTE
}

// --- audio sample packets -------------------------------------------------

static void test_audio_sample_packet_header(void) {
    dvi_data_packet_t pkt;
    const int16_t lr[8] = { 1, -1, 2, -2, 3, -3, 4, -4 };
    dvi_di_set_audio_samples(&pkt, lr, 3, 5);
    TEST_ASSERT_EQUAL_HEX8(0x02, pkt.header[0]);          // audio sample packet
    TEST_ASSERT_EQUAL_HEX8(0x07, pkt.header[1]);          // 3 frames present
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt.header[2]);          // not a block start
}

static void test_audio_sample_packet_marks_iec_block_start(void) {
    // Bit B.0 must be set on frame 0 of every 192-frame IEC block, and only
    // there; a sink uses it to find channel-status boundaries.
    dvi_data_packet_t pkt;
    const int16_t lr[2] = { 0, 0 };
    dvi_di_set_audio_samples(&pkt, lr, 1, 0);
    TEST_ASSERT_EQUAL_HEX8(0x10, pkt.header[2]);
    dvi_di_set_audio_samples(&pkt, lr, 1, 192);
    TEST_ASSERT_EQUAL_HEX8(0x10, pkt.header[2]);
    dvi_di_set_audio_samples(&pkt, lr, 1, 191);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt.header[2]);
}

static void test_audio_samples_are_little_endian_16_bit(void) {
    dvi_data_packet_t pkt;
    const int16_t lr[2] = { (int16_t)0x1234, (int16_t)0xABCD };
    dvi_di_set_audio_samples(&pkt, lr, 1, 1);
    TEST_ASSERT_EQUAL_HEX8(0x34, pkt.subpacket[0][1]);
    TEST_ASSERT_EQUAL_HEX8(0x12, pkt.subpacket[0][2]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, pkt.subpacket[0][4]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, pkt.subpacket[0][5]);
}

static void test_audio_sample_frame_count_is_clamped(void) {
    dvi_data_packet_t pkt;
    const int16_t lr[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    dvi_di_set_audio_samples(&pkt, lr, 9, 1);             // more than 4 frames
    TEST_ASSERT_EQUAL_HEX8(0x0F, pkt.header[1]);          // clamped to 4
    dvi_di_set_audio_samples(&pkt, lr, 0, 1);             // fewer than 1
    TEST_ASSERT_EQUAL_HEX8(0x01, pkt.header[1]);
}

// --- BCH parity -----------------------------------------------------------

static void test_parity_of_all_zero_data_is_zero(void) {
    dvi_data_packet_t pkt;
    memset(&pkt, 0, sizeof pkt);
    dvi_di_init();
    dvi_di_compute_parity(&pkt);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt.header[3]);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_HEX8(0x00, pkt.subpacket[i][7]);
}

static void test_parity_changes_with_every_header_byte(void) {
    dvi_di_init();
    dvi_data_packet_t base;
    memset(&base, 0, sizeof base);
    dvi_di_compute_parity(&base);
    for (int byte = 0; byte < 3; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            dvi_data_packet_t pkt;
            memset(&pkt, 0, sizeof pkt);
            pkt.header[byte] = (uint8_t)(1u << bit);
            dvi_di_compute_parity(&pkt);
            TEST_ASSERT_NOT_EQUAL(base.header[3], pkt.header[3]);
        }
    }
}

static void test_parity_is_deterministic_and_covers_real_packets(void) {
    dvi_di_init();
    dvi_data_packet_t a, b;
    dvi_di_set_acr(&a, 25176, 6144);
    dvi_di_compute_parity(&a);
    dvi_di_set_acr(&b, 25176, 6144);
    dvi_di_compute_parity(&b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a.header, b.header, 4);
    TEST_ASSERT_EQUAL_MEMORY(a.subpacket, b.subpacket, sizeof a.subpacket);
    dvi_di_set_acr(&b, 24000, 6144);                       // the old, wrong CTS
    dvi_di_compute_parity(&b);
    TEST_ASSERT_TRUE(memcmp(a.subpacket, b.subpacket, sizeof a.subpacket) != 0);
}

// --- TERC4 ----------------------------------------------------------------

static void test_terc4_symbols_are_distinct_10_bit_values(void) {
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_TRUE(dvi_terc4_syms[i] < 0x400);
        for (int j = i + 1; j < 16; j++)
            TEST_ASSERT_NOT_EQUAL(dvi_terc4_syms[i], dvi_terc4_syms[j]);
    }
}

static void test_terc4_pairs_pack_two_symbols_per_word(void) {
    // The DMA feeds 2 symbols per 32-bit word (DVI_SYMBOLS_PER_WORD=2), low
    // symbol first. This is what dvi_setup_active_audio_line writes.
    uint32_t w = dvi_di_terc4x2(0, 1);
    TEST_ASSERT_EQUAL_HEX32(dvi_terc4_syms[0] | ((uint32_t)dvi_terc4_syms[1] << 10), w);
}

static void test_guard_band_symbol_is_not_a_data_symbol(void) {
    for (int i = 0; i < 16; i++)
        TEST_ASSERT_NOT_EQUAL(DVI_DI_GUARDBAND_SYM, dvi_terc4_syms[i]);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_acr_layout_is_20_bit_cts_and_n);
    RUN_TEST(test_acr_keeps_the_top_nibble_of_a_20_bit_value);
    RUN_TEST(test_audio_infoframe_header_and_checksum);
    RUN_TEST(test_audio_infoframe_sample_rate_and_size_fields);
    RUN_TEST(test_avi_infoframe_header_and_checksum);
    RUN_TEST(test_infoframe_payload_spills_into_the_next_subpacket);
    RUN_TEST(test_gcp_mute_bits);
    RUN_TEST(test_audio_sample_packet_header);
    RUN_TEST(test_audio_sample_packet_marks_iec_block_start);
    RUN_TEST(test_audio_samples_are_little_endian_16_bit);
    RUN_TEST(test_audio_sample_frame_count_is_clamped);
    RUN_TEST(test_parity_of_all_zero_data_is_zero);
    RUN_TEST(test_parity_changes_with_every_header_byte);
    RUN_TEST(test_parity_is_deterministic_and_covers_real_packets);
    RUN_TEST(test_terc4_symbols_are_distinct_10_bit_values);
    RUN_TEST(test_terc4_pairs_pack_two_symbols_per_word);
    RUN_TEST(test_guard_band_symbol_is_not_a_data_symbol);
    return UNITY_END();
}

// test_usb_encode.cpp: the four-bits-at-a-time USB TX encoder (PIZERO-322)
// against upstream Pico-PIO-USB's bit-at-a-time one, byte for byte.
//
// The reference below is pio_usb_ll_encode_tx_data as it was in
// lib/pico_pio_usb/src/pio_usb.c (upstream 0.7.2 with this project's
// 16-bit lengths, PIZERO-255), copied unchanged but for its name.

#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../lib/pico_pio_usb/src/pio_usb_encode.h"

void setUp(void) {}
void tearDown(void) {}

enum {
    PIO_USB_TX_ENCODED_DATA_SE0 = 0,
    PIO_USB_TX_ENCODED_DATA_K = 1,
    PIO_USB_TX_ENCODED_DATA_COMP = 2,
    PIO_USB_TX_ENCODED_DATA_J = 3,
};

static uint16_t ref_encode(uint8_t const *buffer, uint16_t buffer_len, uint8_t *encoded_data) {
  uint16_t bit_idx = 0;
  int current_state = 1;
  int bit_stuffing = 6;
  for (int idx = 0; idx < buffer_len; idx++) {
    uint8_t data_byte = buffer[idx];
    for (int b = 0; b < 8; b++) {
      uint16_t byte_idx = bit_idx >> 2;
      encoded_data[byte_idx] <<= 2;
      if (data_byte & (1 << b)) {
        if (current_state) {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_K;
        } else {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_J;
        }
        bit_stuffing--;
      } else {
        if (current_state) {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_J;
          current_state = 0;
        } else {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_K;
          current_state = 1;
        }
        bit_stuffing = 6;
      }

      bit_idx++;

      if (bit_stuffing == 0) {
        byte_idx = bit_idx >> 2;
        encoded_data[byte_idx] <<= 2;

        if (current_state) {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_J;
          current_state = 0;
        } else {
          encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_K;
          current_state = 1;
        }
        bit_stuffing = 6;
        bit_idx++;
      }
    }
  }

  uint16_t byte_idx = bit_idx >> 2;
  encoded_data[byte_idx] <<= 2;
  encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_SE0;
  bit_idx++;

  byte_idx = bit_idx >> 2;
  encoded_data[byte_idx] <<= 2;
  encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_COMP;
  bit_idx++;

  // terminate buffers with K
  do {
    byte_idx = bit_idx >> 2;
    encoded_data[byte_idx] <<= 2;
    encoded_data[byte_idx] |= PIO_USB_TX_ENCODED_DATA_K;
    bit_idx++;
  } while (bit_idx & 0x03);

  byte_idx = bit_idx >> 2;
  return byte_idx;
}

#define MAXLEN 300
#define ENC_CAP ((MAXLEN) * 2 * 7 / 6 + 8)

static int g_checked;

static void check(const uint8_t *buf, uint16_t len, const char *what) {
    uint8_t a[ENC_CAP], b[ENC_CAP];
    memset(a, 0x5A, sizeof a);   // the reference shifts old bits out of each byte
    memset(b, 0xA5, sizeof b);
    uint16_t na = ref_encode(buf, len, a);
    uint16_t nb = pio_usb_encode_fast(buf, len, b);
    char msg[96];
    snprintf(msg, sizeof msg, "%s: length %u", what, (unsigned)len);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(na, nb, msg);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(a, b, na, msg);
    g_checked++;
}

static uint32_t g_rng = 1;
static uint8_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (uint8_t)g_rng;
}

static void test_tables_match_the_bit_rules(void) {
    for (unsigned n = 0; n < 16; n++) {
        unsigned lv = 1, sym = 0;
        for (int i = 0; i < 4; i++) {
            if (!((n >> i) & 1)) lv ^= 1;
            sym = (sym << 2) | (lv ? PIO_USB_ENC_K : PIO_USB_ENC_J);
        }
        TEST_ASSERT_EQUAL_HEX8(sym, pio_usb_enc_sym[n]);
    }
}

static void test_fixed_packets(void) {
    uint8_t buf[MAXLEN];
    for (uint16_t len = 0; len <= 200; len++) {
        memset(buf, 0x00, len); check(buf, len, "all 0x00");
        memset(buf, 0xFF, len); check(buf, len, "all 0xFF");
        memset(buf, 0x7F, len); check(buf, len, "all 0x7F");
        memset(buf, 0xFE, len); check(buf, len, "all 0xFE");
        for (uint16_t i = 0; i < len; i++) buf[i] = (i & 1) ? 0xFF : 0x3F;
        check(buf, len, "ones runs across bytes");
    }
}

static void test_every_byte_after_every_run(void) {
    // A byte of n trailing 1s (0..7 ones at the top) sets the run carried
    // into the next byte; follow it with every byte value.
    uint8_t buf[4];
    for (int ones = 0; ones <= 8; ones++) {
        uint8_t lead = (uint8_t)(ones ? (0xFF << (8 - ones)) : 0);
        for (int v = 0; v < 256; v++) {
            buf[0] = 0x80;  // the SYNC pattern's last bit
            buf[1] = lead;
            buf[2] = (uint8_t)v;
            buf[3] = (uint8_t)~v;
            check(buf, 4, "run then byte");
        }
    }
}

static void test_random_packets(void) {
    uint8_t buf[MAXLEN];
    for (int k = 0; k < 20000; k++) {
        uint16_t len = (uint16_t)(rnd() | ((rnd() & 1) << 8));
        if (len > MAXLEN) len = MAXLEN;
        int dense = rnd() & 3;   // sometimes mostly 1s, where stuffing lives
        for (uint16_t i = 0; i < len; i++) buf[i] = dense == 0 ? (uint8_t)(rnd() | rnd()) : rnd();
        check(buf, len, "random");
    }
}

static void test_an_audio_packet(void) {
    // A 196-byte isochronous packet as the frame handler builds it: SYNC,
    // DATA0, 48 stereo 16-bit samples, CRC.
    uint8_t buf[196];
    buf[0] = 0x80; buf[1] = 0xC3;
    for (int i = 0; i < 96; i++) {
        int16_t s = (int16_t)(i * 683 - 32768);
        buf[2 + 2 * i] = (uint8_t)s; buf[3 + 2 * i] = (uint8_t)(s >> 8);
    }
    buf[194] = 0x12; buf[195] = 0x34;
    check(buf, sizeof buf, "audio packet");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_tables_match_the_bit_rules);
    RUN_TEST(test_fixed_packets);
    RUN_TEST(test_every_byte_after_every_run);
    RUN_TEST(test_random_packets);
    RUN_TEST(test_an_audio_packet);
    printf("  %d packets matched\n", g_checked);
    return UNITY_END();
}

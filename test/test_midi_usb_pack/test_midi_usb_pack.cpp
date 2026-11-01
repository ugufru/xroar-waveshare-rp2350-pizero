// PIZERO-287: host tests for midi_usb_pack.h: the MIDI Pak's byte stream cut
// into USB-MIDI packets.
//
//   pio test -e native -f test_midi_usb_pack

#include <unity.h>
#include <string.h>

#include "../../src/midi_usb_pack.h"

void setUp(void) {}
void tearDown(void) {}

static struct midi_pack m;
static uint8_t pk[16][4];
static int npk;

static void feed(const uint8_t *b, int n) {
    npk = 0;
    for (int i = 0; i < n; i++) {
        uint8_t out[4];
        if (midi_pack_byte(&m, b[i], out)) memcpy(pk[npk++], out, 4);
    }
}

static void packet_is(int i, uint8_t c, uint8_t a, uint8_t b, uint8_t d) {
    uint8_t want[4] = { c, a, b, d };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pk[i], 4);
}

static void test_note_on_and_running_status(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0x90, 0x3C, 0x64, 0x40, 0x50, 0x80, 0x3C, 0x00 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(3, npk);
    packet_is(0, 0x09, 0x90, 0x3C, 0x64);
    packet_is(1, 0x09, 0x90, 0x40, 0x50);         // running status
    packet_is(2, 0x08, 0x80, 0x3C, 0x00);
}

static void test_two_byte_channel_messages(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0xC2, 0x05, 0x07, 0xD0, 0x40 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(3, npk);
    packet_is(0, 0x0C, 0xC2, 0x05, 0x00);
    packet_is(1, 0x0C, 0xC2, 0x07, 0x00);         // running status, one data byte
    packet_is(2, 0x0D, 0xD0, 0x40, 0x00);
}

// The case found on hardware: a lone status left by a program that stopped,
// then a whole note. The unfinished message is dropped, the note is clean.
static void test_unfinished_message_is_dropped(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0x90, 0x90, 0x3C, 0x64 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(1, npk);
    packet_is(0, 0x09, 0x90, 0x3C, 0x64);
    midi_pack_init(&m);
    const uint8_t t[] = { 0x90, 0x3C, 0x80, 0x3C, 0x00 };   // half a note-on, then a note-off
    feed(t, sizeof t);
    TEST_ASSERT_EQUAL_INT(1, npk);
    packet_is(0, 0x08, 0x80, 0x3C, 0x00);
}

static void test_real_time_goes_out_at_once_mid_message(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0x90, 0x3C, 0xF8, 0x64 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(2, npk);
    packet_is(0, 0x0F, 0xF8, 0x00, 0x00);
    packet_is(1, 0x09, 0x90, 0x3C, 0x64);
}

static void test_system_common(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0xF2, 0x10, 0x20, 0xF3, 0x05, 0xF6 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(3, npk);
    packet_is(0, 0x03, 0xF2, 0x10, 0x20);
    packet_is(1, 0x02, 0xF3, 0x05, 0x00);
    packet_is(2, 0x05, 0xF6, 0x00, 0x00);
    const uint8_t t[] = { 0x10 };                 // no running status after system common
    feed(t, sizeof t);
    TEST_ASSERT_EQUAL_INT(0, npk);
}

static void test_sysex_of_every_length(void) {
    midi_pack_init(&m);
    const uint8_t a[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };   // 6 bytes: 3 + 3
    feed(a, sizeof a);
    TEST_ASSERT_EQUAL_INT(2, npk);
    packet_is(0, 0x04, 0xF0, 0x7E, 0x7F);
    packet_is(1, 0x07, 0x09, 0x01, 0xF7);
    const uint8_t b[] = { 0xF0, 0x43, 0x10, 0x4C, 0xF7 };         // 5: 3 + 2
    feed(b, sizeof b);
    TEST_ASSERT_EQUAL_INT(2, npk);
    packet_is(1, 0x06, 0x4C, 0xF7, 0x00);
    const uint8_t c[] = { 0xF0, 0x43, 0x10, 0xF7 };               // 4: 3 + 1
    feed(c, sizeof c);
    TEST_ASSERT_EQUAL_INT(2, npk);
    packet_is(1, 0x05, 0xF7, 0x00, 0x00);
    const uint8_t d[] = { 0xF0, 0x7E, 0x90, 0x3C, 0x64 };         // SysEx cut off by a note
    feed(d, sizeof d);
    TEST_ASSERT_EQUAL_INT(1, npk);
    packet_is(0, 0x09, 0x90, 0x3C, 0x64);
}

static void test_stray_data_is_ignored(void) {
    midi_pack_init(&m);
    const uint8_t s[] = { 0x3C, 0x64, 0xF7 };
    feed(s, sizeof s);
    TEST_ASSERT_EQUAL_INT(0, npk);
}

static void test_unpack_by_code_index(void) {
    uint8_t o[3];
    const uint8_t note[4] = { 0x09, 0x90, 0x40, 0x7F }, pc[4] = { 0x0C, 0xC0, 0x05, 0x00 };
    const uint8_t clk[4] = { 0x0F, 0xF8, 0, 0 }, sx[4] = { 0x06, 0x4C, 0xF7, 0 }, junk[4] = { 0x00, 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT(3, midi_unpack(note, o)); TEST_ASSERT_EQUAL_HEX8(0x7F, o[2]);
    TEST_ASSERT_EQUAL_INT(2, midi_unpack(pc, o));   TEST_ASSERT_EQUAL_HEX8(0x05, o[1]);
    TEST_ASSERT_EQUAL_INT(1, midi_unpack(clk, o));  TEST_ASSERT_EQUAL_HEX8(0xF8, o[0]);
    TEST_ASSERT_EQUAL_INT(2, midi_unpack(sx, o));   TEST_ASSERT_EQUAL_HEX8(0xF7, o[1]);
    TEST_ASSERT_EQUAL_INT(0, midi_unpack(junk, o));
    // A packet cut by midi_pack_byte comes back as the same bytes.
    midi_pack_init(&m);
    const uint8_t s[] = { 0x90, 0x3C, 0x64 };
    feed(s, 3);
    TEST_ASSERT_EQUAL_INT(3, midi_unpack(pk[0], o));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s, o, 3);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_note_on_and_running_status);
    RUN_TEST(test_two_byte_channel_messages);
    RUN_TEST(test_unfinished_message_is_dropped);
    RUN_TEST(test_real_time_goes_out_at_once_mid_message);
    RUN_TEST(test_system_common);
    RUN_TEST(test_sysex_of_every_length);
    RUN_TEST(test_stray_data_is_ignored);
    RUN_TEST(test_unpack_by_code_index);
    return UNITY_END();
}

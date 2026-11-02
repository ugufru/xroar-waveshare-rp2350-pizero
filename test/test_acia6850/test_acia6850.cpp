// PIZERO-287: host tests for the MIDI Pak's 6850 ACIA (acia6850.h), driven
// the way Lyra 2.85 and CoCo MIDI 3 drive it (from their disassembly).
//
//   pio test -e native -f test_acia6850

#include <unity.h>

#include "../../lib/coco_machine/src/acia6850.h"

void setUp(void) {}
void tearDown(void) {}

static struct acia6850 a;
static uint32_t now;

static void boot(uint8_t cr) {
    acia_init(&a);
    now = 1000;
    acia_write(&a, 0, 0x03, now);              // master reset, as both programs do
    acia_write(&a, 0, cr, now);
}

static void test_byte_time_is_midi_rate(void) {
    // 10 bits at 31,250 baud = 320 us = 4,581.8 ticks of 14.318 MHz.
    TEST_ASSERT_EQUAL_UINT32(4581, acia_ticks_per_byte(0x15));
    TEST_ASSERT_EQUAL_UINT32(286, acia_ticks_per_byte(0x14));    // divide by 1
    TEST_ASSERT_EQUAL_UINT32(18327, acia_ticks_per_byte(0x16));  // divide by 64
}

static void test_reset_holds_tdre_low_until_configured(void) {
    acia_init(&a);
    TEST_ASSERT_EQUAL_HEX8(0x00, acia_read(&a, 0, 0));
    acia_write(&a, 1, 0x90, 0);                // ignored in reset
    uint8_t b;
    TEST_ASSERT_FALSE(acia_tx_pop(&a, &b));
    acia_write(&a, 0, 0x15, 0);
    TEST_ASSERT_EQUAL_HEX8(ACIA_TDRE, acia_read(&a, 0, 0));
}

// Lyra's send loop: wait for TDRE, write the byte. A note-on (3 bytes) goes
// out at the line rate: the first straight away, then one per byte time.
static void test_polled_send_is_paced_and_ordered(void) {
    boot(0x15);
    const uint8_t note[3] = { 0x90, 0x3C, 0x64 };
    uint32_t start = now;
    for (int i = 0; i < 3; i++) {
        int spins = 0;
        while (!(acia_read(&a, 0, now) & ACIA_TDRE)) { now += 40; spins++; TEST_ASSERT_TRUE(spins < 1000); }
        acia_write(&a, 1, note[i], now);
    }
    // Bytes 1 and 2 went in without waiting (shift register, then TDR); byte
    // 3 waited for the first to finish.
    TEST_ASSERT_TRUE(now - start >= acia_ticks_per_byte(0x15));
    acia_service(&a, now + 3 * a.tpb);
    uint8_t b;
    for (int i = 0; i < 3; i++) { TEST_ASSERT_TRUE(acia_tx_pop(&a, &b)); TEST_ASSERT_EQUAL_HEX8(note[i], b); }
    TEST_ASSERT_FALSE(acia_tx_pop(&a, &b));
}

static void test_receive_one_byte_per_byte_time(void) {
    boot(0x15);
    acia_rx_push(&a, 0x90); acia_rx_push(&a, 0x40); acia_rx_push(&a, 0x7F);
    acia_service(&a, now);
    TEST_ASSERT_EQUAL_HEX8(ACIA_RDRF | ACIA_TDRE, acia_read(&a, 0, now));
    TEST_ASSERT_EQUAL_HEX8(0x90, acia_read(&a, 1, now));
    TEST_ASSERT_EQUAL_HEX8(ACIA_TDRE, acia_read(&a, 0, now + 10));   // not yet
    uint32_t due;
    TEST_ASSERT_TRUE(acia_next_due(&a, now, &due));
    TEST_ASSERT_EQUAL_UINT32(now + a.tpb, due);
    TEST_ASSERT_EQUAL_HEX8(0x40, acia_read(&a, 1, now + a.tpb));
}

static void test_overrun_when_software_is_slow(void) {
    boot(0x15);
    acia_rx_push(&a, 0x11); acia_rx_push(&a, 0x22);
    acia_service(&a, now);
    acia_service(&a, now + a.tpb);             // second lands with the first unread
    uint8_t s = acia_read(&a, 0, now + a.tpb);
    TEST_ASSERT_TRUE(s & ACIA_OVRN);
    TEST_ASSERT_EQUAL_HEX8(0x11, acia_read(&a, 1, now + a.tpb));   // the first is kept
    TEST_ASSERT_FALSE(acia_read(&a, 0, now + a.tpb) & (ACIA_OVRN | ACIA_RDRF));
}

// CoCo MIDI 3's recording setup: $95 turns the receive interrupt on. The IRQ
// follows RDRF, and reading the data drops it.
static void test_receive_interrupt(void) {
    boot(0x95);
    TEST_ASSERT_FALSE(acia_irq(&a));
    acia_rx_push(&a, 0x90);
    acia_service(&a, now);
    TEST_ASSERT_TRUE(acia_irq(&a));
    TEST_ASSERT_TRUE(acia_read(&a, 0, now) & ACIA_IRQ);
    TEST_ASSERT_TRUE((acia_read(&a, 0, now) & 0x24) == 0);       // the handler's error check passes
    acia_read(&a, 1, now);
    TEST_ASSERT_FALSE(acia_irq(&a));
    boot(0x15);                                // the same byte with interrupts off: no IRQ
    acia_rx_push(&a, 0x90);
    acia_service(&a, now);
    TEST_ASSERT_FALSE(acia_irq(&a));
}

static void test_master_reset_drops_pending_input(void) {
    boot(0x15);
    acia_rx_push(&a, 0x90);
    acia_write(&a, 0, 0x03, now);
    TEST_ASSERT_FALSE(acia_rx_waiting(&a));
    TEST_ASSERT_FALSE(acia_rx_push(&a, 0x90));  // nobody listening
    uint32_t due;
    TEST_ASSERT_FALSE(acia_next_due(&a, now, &due));
}

// Found on hardware: after 150 s of uptime the tick count passes 2^31 and a
// stale "not before" of 0 looked like the future, so no byte ever landed.
static void test_ticks_past_the_halfway_wrap(void) {
    acia_init(&a);
    now = 0x90000000u;                          // 2.7 minutes of uptime
    acia_write(&a, 0, 0x03, now);
    acia_write(&a, 0, 0x15, now);
    acia_rx_push(&a, 0x90);
    uint32_t due;
    TEST_ASSERT_TRUE(acia_next_due(&a, now, &due));
    TEST_ASSERT_EQUAL_UINT32(now, due);
    TEST_ASSERT_EQUAL_HEX8(0x90, acia_read(&a, 1, now));
    // A byte sent, then nothing for longer than the wrap: the next send still goes.
    acia_write(&a, 1, 0x80, now);
    now += 0x80000010u;
    acia_write(&a, 1, 0x81, now);
    acia_write(&a, 1, 0x82, now);
    acia_service(&a, now + 2 * a.tpb);
    uint8_t b;
    TEST_ASSERT_TRUE(acia_tx_pop(&a, &b)); TEST_ASSERT_EQUAL_HEX8(0x80, b);
    TEST_ASSERT_TRUE(acia_tx_pop(&a, &b)); TEST_ASSERT_EQUAL_HEX8(0x81, b);
    TEST_ASSERT_TRUE(acia_tx_pop(&a, &b)); TEST_ASSERT_EQUAL_HEX8(0x82, b);
}

// A status poll that finds nothing new reports no change, so the host can
// skip its event queue; anything that lands or is sent does.
static void test_status_polls_are_not_changes(void) {
    boot(0x15);
    acia_take_changed(&a);
    for (int i = 0; i < 100; i++) acia_read(&a, 0, now + i);
    TEST_ASSERT_FALSE(acia_take_changed(&a));
    acia_rx_push(&a, 0x90);
    TEST_ASSERT_TRUE(acia_take_changed(&a));
    acia_read(&a, 0, now + 200);                 // the byte lands during this poll
    TEST_ASSERT_TRUE(acia_take_changed(&a));
    acia_read(&a, 1, now + 200);
    TEST_ASSERT_TRUE(acia_take_changed(&a));
    acia_write(&a, 1, 0x40, now + 300);
    TEST_ASSERT_TRUE(acia_take_changed(&a));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_byte_time_is_midi_rate);
    RUN_TEST(test_reset_holds_tdre_low_until_configured);
    RUN_TEST(test_polled_send_is_paced_and_ordered);
    RUN_TEST(test_receive_one_byte_per_byte_time);
    RUN_TEST(test_overrun_when_software_is_slow);
    RUN_TEST(test_receive_interrupt);
    RUN_TEST(test_master_reset_drops_pending_input);
    RUN_TEST(test_ticks_past_the_halfway_wrap);
    RUN_TEST(test_status_polls_are_not_changes);
    return UNITY_END();
}

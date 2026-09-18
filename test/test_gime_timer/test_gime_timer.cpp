// PIZERO-62 / PIZERO-109: host tests for the GIME timer's register behaviour
// and interval arithmetic.
//
// The tempo of any music written against this timer comes straight out of
// gime_timer_interval(), so the arithmetic is worth pinning before it ever
// runs on hardware.
//
//   pio test -e native

#include <unity.h>

#include "../../lib/coco_machine/src/gime_timer.h"

static gime_timer_t t;

void setUp(void) { gime_timer_reset(&t); }
void tearDown(void) {}

// --- interval arithmetic --------------------------------------------------

static void test_stopped_until_a_reload_and_an_enable_exist(void) {
    TEST_ASSERT_EQUAL_UINT32(0, gime_timer_interval(&t));
    gime_timer_write(&t, GIME_REG_TMRL, 100);
    TEST_ASSERT_EQUAL_UINT32(0, gime_timer_interval(&t));   // no source enabled
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    TEST_ASSERT_NOT_EQUAL(0, gime_timer_interval(&t));
}

static void test_reload_zero_stops_the_timer(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_TMRL, 0);
    gime_timer_write(&t, GIME_REG_TMRH, 0);
    TEST_ASSERT_EQUAL_UINT32(0, gime_timer_interval(&t));
}

static void test_slow_clock_is_the_horizontal_rate(void) {
    // TINS clear = 15.734 kHz. A reload of 1 is (1 + bias) horizontal periods.
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_TMRL, 1);
    TEST_ASSERT_EQUAL_UINT32((1 + GIME_PERIOD_BIAS) * GIME_TICKS_SLOW,
                             gime_timer_interval(&t));
}

static void test_fast_clock_is_four_event_ticks_per_count(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_INIT1, GIME_INIT1_TINS);
    gime_timer_write(&t, GIME_REG_TMRL, 100);
    TEST_ASSERT_EQUAL_UINT32((100 + GIME_PERIOD_BIAS) * GIME_TICKS_FAST,
                             gime_timer_interval(&t));
}

static void test_a_musical_interval_lands_where_it_should(void) {
    // 3.579545 MHz / (reload + bias) should give the guest's intended rate.
    // Reload 3579 on the fast clock is about 1 kHz; check the event-tick
    // interval corresponds, within the tick quantum.
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_INIT1, GIME_INIT1_TINS);
    gime_timer_write(&t, GIME_REG_TMRH, 0x0D);      // 0xDFB = 3579
    gime_timer_write(&t, GIME_REG_TMRL, 0xFB);
    TEST_ASSERT_EQUAL_UINT16(3579, t.reload);
    uint32_t ticks = gime_timer_interval(&t);
    double hz = 14318180.0 / (double)ticks;
    TEST_ASSERT_TRUE(hz > 995.0 && hz < 1001.0);
}

static void test_reload_is_twelve_bits(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_TMRH, 0xFF);      // only the low nibble counts
    gime_timer_write(&t, GIME_REG_TMRL, 0xFF);
    TEST_ASSERT_EQUAL_UINT16(0x0FFF, t.reload);
}

static void test_the_two_reload_bytes_do_not_disturb_each_other(void) {
    gime_timer_write(&t, GIME_REG_TMRL, 0x34);
    gime_timer_write(&t, GIME_REG_TMRH, 0x02);
    TEST_ASSERT_EQUAL_UINT16(0x0234, t.reload);
    gime_timer_write(&t, GIME_REG_TMRL, 0x99);
    TEST_ASSERT_EQUAL_UINT16(0x0299, t.reload);
}

static void test_tiny_reload_is_clamped_and_counted(void) {
    // A reload of 1 on the fast clock would fire every 12 event ticks, well
    // under a CPU cycle's worth of work. It must clamp, and say that it did.
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_INIT1, GIME_INIT1_TINS);
    gime_timer_write(&t, GIME_REG_TMRL, 1);
    TEST_ASSERT_EQUAL_UINT32(GIME_MIN_INTERVAL_TICKS, gime_timer_interval(&t));
    TEST_ASSERT_EQUAL_UINT32(1, t.clamped);
}

// --- interrupt routing ----------------------------------------------------

static void test_fire_routes_to_irq_firq_or_both(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    TEST_ASSERT_EQUAL_UINT8(GIME_LINE_IRQ, gime_timer_fire(&t));

    gime_timer_reset(&t);
    gime_timer_write(&t, GIME_REG_FIRQENR, GIME_INT_TMR);
    TEST_ASSERT_EQUAL_UINT8(GIME_LINE_FIRQ, gime_timer_fire(&t));

    gime_timer_reset(&t);
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_FIRQENR, GIME_INT_TMR);
    TEST_ASSERT_EQUAL_UINT8(GIME_LINE_IRQ | GIME_LINE_FIRQ, gime_timer_fire(&t));
}

static void test_a_disabled_timer_fires_nothing(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_VBORD);   // some other source
    TEST_ASSERT_EQUAL_UINT8(0, gime_timer_fire(&t));
    TEST_ASSERT_EQUAL_UINT8(0, gime_timer_lines(&t));
}

static void test_status_is_read_to_clear_and_holds_the_line_meanwhile(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_fire(&t);
    TEST_ASSERT_EQUAL_UINT8(GIME_LINE_IRQ, gime_timer_lines(&t));   // still asserted

    uint8_t v = 0xAA;
    TEST_ASSERT_TRUE(gime_timer_read(&t, GIME_REG_IRQENR, &v));
    TEST_ASSERT_EQUAL_UINT8(GIME_INT_TMR, v);                       // source reported
    TEST_ASSERT_EQUAL_UINT8(0, gime_timer_lines(&t));               // and released
    TEST_ASSERT_TRUE(gime_timer_read(&t, GIME_REG_IRQENR, &v));
    TEST_ASSERT_EQUAL_UINT8(0, v);                                  // stays clear
}

static void test_irq_and_firq_status_are_independent(void) {
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR);
    gime_timer_write(&t, GIME_REG_FIRQENR, GIME_INT_TMR);
    gime_timer_fire(&t);
    uint8_t v;
    gime_timer_read(&t, GIME_REG_IRQENR, &v);
    TEST_ASSERT_EQUAL_UINT8(GIME_LINE_FIRQ, gime_timer_lines(&t));  // FIRQ still up
}

// --- decode ---------------------------------------------------------------

static void test_only_the_gime_block_is_claimed(void) {
    TEST_ASSERT_TRUE(gime_timer_owns(0xFF90));
    TEST_ASSERT_TRUE(gime_timer_owns(0xFF95));
    TEST_ASSERT_FALSE(gime_timer_owns(0xFF8F));
    TEST_ASSERT_FALSE(gime_timer_owns(0xFF96));
    // The CoCo 2's own I/O must not be touched: PIAs, SAM, cartridge.
    TEST_ASSERT_FALSE(gime_timer_owns(0xFF00));
    TEST_ASSERT_FALSE(gime_timer_owns(0xFF22));
    TEST_ASSERT_FALSE(gime_timer_owns(0xFF40));
    TEST_ASSERT_FALSE(gime_timer_owns(0xFFC6));
}

static void test_enable_registers_keep_unimplemented_source_bits(void) {
    // A guest read-modify-writing the enable register must not lose bits for
    // sources we do not raise, or its own bookkeeping breaks.
    gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR | GIME_INT_VBORD | GIME_INT_EI1);
    TEST_ASSERT_EQUAL_UINT8(GIME_INT_TMR | GIME_INT_VBORD | GIME_INT_EI1, t.irq_enable);
}

static void test_writes_that_change_timing_ask_for_a_restart(void) {
    TEST_ASSERT_TRUE(gime_timer_write(&t, GIME_REG_TMRL, 10));
    TEST_ASSERT_TRUE(gime_timer_write(&t, GIME_REG_TMRH, 1));
    TEST_ASSERT_TRUE(gime_timer_write(&t, GIME_REG_INIT1, GIME_INIT1_TINS));
    TEST_ASSERT_TRUE(gime_timer_write(&t, GIME_REG_IRQENR, GIME_INT_TMR));
    TEST_ASSERT_FALSE(gime_timer_write(&t, GIME_REG_INIT0, 0x44));   // no timing effect
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stopped_until_a_reload_and_an_enable_exist);
    RUN_TEST(test_reload_zero_stops_the_timer);
    RUN_TEST(test_slow_clock_is_the_horizontal_rate);
    RUN_TEST(test_fast_clock_is_four_event_ticks_per_count);
    RUN_TEST(test_a_musical_interval_lands_where_it_should);
    RUN_TEST(test_reload_is_twelve_bits);
    RUN_TEST(test_the_two_reload_bytes_do_not_disturb_each_other);
    RUN_TEST(test_tiny_reload_is_clamped_and_counted);
    RUN_TEST(test_fire_routes_to_irq_firq_or_both);
    RUN_TEST(test_a_disabled_timer_fires_nothing);
    RUN_TEST(test_status_is_read_to_clear_and_holds_the_line_meanwhile);
    RUN_TEST(test_irq_and_firq_status_are_independent);
    RUN_TEST(test_only_the_gime_block_is_claimed);
    RUN_TEST(test_enable_registers_keep_unimplemented_source_bits);
    RUN_TEST(test_writes_that_change_timing_ask_for_a_restart);
    return UNITY_END();
}

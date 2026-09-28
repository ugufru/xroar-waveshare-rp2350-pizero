// PIZERO-145: host tests for /coco/settings.txt parsing.
//
// The file is edited by hand, on a PC or on the CoCo Zero's own screen, so
// every way a person writes a line has to land on the right setting or be
// reported, never silently misread.
//
//   pio test -e native

#include <unity.h>

#include "../../src/settings.h"

void setUp(void) {}
void tearDown(void) {}

static struct coco_settings s;

static int line(const char *l) { return settings_parse_line(&s, l, NULL, 0); }

static void test_defaults_match_todays_behaviour(void) {
    settings_defaults(&s);
    TEST_ASSERT_TRUE(s.sn76489);
    TEST_ASSERT_EQUAL_UINT8(10, s.volume);
    TEST_ASSERT_EQUAL_UINT8(ART_ON, s.artifact);
    TEST_ASSERT_TRUE(s.gime_palette);
    TEST_ASSERT_TRUE(s.gime_timer);
    TEST_ASSERT_TRUE(s.run_skips_autorun);
    TEST_ASSERT_TRUE(s.serial_keyboard);
    TEST_ASSERT_EQUAL_UINT16(0, s.color_set);                        // default palette
}

static void test_every_setting(void) {
    settings_defaults(&s);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("sn76489 = off"));            TEST_ASSERT_FALSE(s.sn76489);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("volume = 3"));               TEST_ASSERT_EQUAL_UINT8(3, s.volume);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("artifact_colors = swapped")); TEST_ASSERT_EQUAL_UINT8(ART_SWAPPED, s.artifact);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("artifact_colors = off"));    TEST_ASSERT_EQUAL_UINT8(ART_OFF, s.artifact);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("gime_palette = off"));       TEST_ASSERT_FALSE(s.gime_palette);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("gime_timer = no"));          TEST_ASSERT_FALSE(s.gime_timer);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("run_skips_autorun = 0"));    TEST_ASSERT_FALSE(s.run_skips_autorun);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("serial_keyboard = off"));    TEST_ASSERT_FALSE(s.serial_keyboard);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("color_green = #1ED01E"));
    TEST_ASSERT_EQUAL_UINT16(0x0001, s.color_set);
    TEST_ASSERT_EQUAL_HEX16(settings_rgb565(0x1ED01E), s.color[0]);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("color_dark_green = #0a3c0a  # text background"));
    TEST_ASSERT_EQUAL_UINT16(0x0201, s.color_set);                   // entries 0 and 9
    TEST_ASSERT_EQUAL_INT(SET_OK, line("COLOR_BRIGHT_ORANGE = #FFA500"));
    TEST_ASSERT_TRUE(s.color_set & (1u << 11));
}

static void test_case_spacing_and_comments(void) {
    settings_defaults(&s);
    // The on-screen editor shows upper case only, so upper case must work.
    TEST_ASSERT_EQUAL_INT(SET_OK, line("SN76489=OFF"));              TEST_ASSERT_FALSE(s.sn76489);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("   Volume   =   12   "));    TEST_ASSERT_EQUAL_UINT8(12, s.volume);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("volume = 7   # quieter"));   TEST_ASSERT_EQUAL_UINT8(7, s.volume);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("volume = 5\r\n"));           TEST_ASSERT_EQUAL_UINT8(5, s.volume);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("# a whole comment line"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line(""));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("   \r\n"));
    TEST_ASSERT_EQUAL_UINT8(5, s.volume);                            // untouched by those
}

static void test_bad_lines_are_reported_and_change_nothing(void) {
    settings_defaults(&s);
    char name[32] = "";
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, settings_parse_line(&s, "turbo = on", name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("turbo", name);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("volume = 16"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("volume = loud"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("sn76489 = maybe"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("artifact_colors = blue"));
    // American spellings only: one way to write each setting.
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("artifact_colours = off"));
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("colour_green = #00FF00"));
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("color_purple = #800080"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("color_red = FF0000"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("color_red = #FF00"));
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("color_red = #GG0000"));
    TEST_ASSERT_EQUAL_INT(SET_SYNTAX, line("volume 5"));
    TEST_ASSERT_EQUAL_INT(SET_SYNTAX, line("volume ="));
    TEST_ASSERT_EQUAL_INT(SET_SYNTAX, line("volume = 5 6"));
    struct coco_settings d; settings_defaults(&d);
    TEST_ASSERT_EQUAL_MEMORY(&d, &s, sizeof s);                      // nothing changed
}

static void test_rgb565_rounding(void) {
    TEST_ASSERT_EQUAL_HEX16(0x0000, settings_rgb565(0x000000));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, settings_rgb565(0xFFFFFF));
    TEST_ASSERT_EQUAL_HEX16(0xF800, settings_rgb565(0xFF0000));
    TEST_ASSERT_EQUAL_HEX16(0x07E0, settings_rgb565(0x00FF00));   // the factory VDG green
    TEST_ASSERT_EQUAL_HEX16(0x001F, settings_rgb565(0x0000FF));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_todays_behaviour);
    RUN_TEST(test_every_setting);
    RUN_TEST(test_case_spacing_and_comments);
    RUN_TEST(test_bad_lines_are_reported_and_change_nothing);
    RUN_TEST(test_rgb565_rounding);
    return UNITY_END();
}

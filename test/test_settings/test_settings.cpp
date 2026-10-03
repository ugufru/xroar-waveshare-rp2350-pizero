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
    TEST_ASSERT_TRUE(s.autorun);
    TEST_ASSERT_TRUE(s.reset_to_basic);
    TEST_ASSERT_TRUE(s.serial_keyboard);
    TEST_ASSERT_FALSE(s.joystick_swap);             // PIZERO-160: off by default
    TEST_ASSERT_EQUAL_UINT8(FONT_6847T2, s.font);   // PIZERO-166: ours by default
    TEST_ASSERT_TRUE(s.lowercase);                  // PIZERO-166: improvements default on
    TEST_ASSERT_TRUE(s.key_repeat);                 // PIZERO-167
    TEST_ASSERT_EQUAL_UINT16(500, s.key_repeat_delay);
    TEST_ASSERT_EQUAL_UINT8(10, s.key_repeat_rate);
    TEST_ASSERT_TRUE(s.video_hw_encode);            // PIZERO-175: the RP2350's encoder
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
    TEST_ASSERT_EQUAL_INT(SET_OK, line("reset_button = autorun"));   TEST_ASSERT_FALSE(s.reset_to_basic);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("reset_button = BASIC"));     TEST_ASSERT_TRUE(s.reset_to_basic);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("reset_button = on"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("autorun = off"));            TEST_ASSERT_FALSE(s.autorun);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("autorun = maybe"));
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("run_skips_autorun = off"));   // PIZERO-182: the old name is gone
    TEST_ASSERT_EQUAL_INT(SET_OK, line("serial_keyboard = off"));    TEST_ASSERT_FALSE(s.serial_keyboard);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("joystick_swap = on"));       TEST_ASSERT_TRUE(s.joystick_swap);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("joystick_swap = sideways"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("font = classic"));  TEST_ASSERT_EQUAL_UINT8(FONT_CLASSIC, s.font);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("FONT = 6847T1"));   TEST_ASSERT_EQUAL_UINT8(FONT_6847T1, s.font);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("font = 6847t2"));   TEST_ASSERT_EQUAL_UINT8(FONT_6847T2, s.font);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("font = comic"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("lowercase = off")); TEST_ASSERT_FALSE(s.lowercase);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("key_repeat = off")); TEST_ASSERT_FALSE(s.key_repeat);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("key_repeat_delay = 250")); TEST_ASSERT_EQUAL_UINT16(250, s.key_repeat_delay);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("key_repeat_delay = 50"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("key_repeat_rate = 5")); TEST_ASSERT_EQUAL_UINT8(5, s.key_repeat_rate);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("key_repeat_rate = 0"));
    TEST_ASSERT_EQUAL_INT(SET_OK, line("video_encoder = software")); TEST_ASSERT_FALSE(s.video_hw_encode);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("video_encoder = Hardware")); TEST_ASSERT_TRUE(s.video_hw_encode);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("video_encoder = on"));
    // PIZERO-164: pad buttons and the D-pad.
    TEST_ASSERT_EQUAL_UINT8(K_ENTER, s.pad.act[PAD_B_START]);          // default
    TEST_ASSERT_EQUAL_INT(SET_OK, line("pad_start = S"));      TEST_ASSERT_EQUAL_UINT8(K_S, s.pad.act[PAD_B_START]);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("pad_l2 = fire"));      TEST_ASSERT_EQUAL_UINT8(PAD_ACT_FIRE, s.pad.act[PAD_B_L2]);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("pad_select = break")); TEST_ASSERT_EQUAL_UINT8(K_BREAK, s.pad.act[PAD_B_SELECT]);
    TEST_ASSERT_EQUAL_INT(SET_OK, line("pad_top = none"));     TEST_ASSERT_EQUAL_UINT8(PAD_ACT_NONE, s.pad.act[PAD_B_TOP]);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("pad_start = tab"));
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("pad_turbo = fire"));
    TEST_ASSERT_EQUAL_INT(SET_UNKNOWN, line("pad_home = s"));   // PIZERO-169: Home is the overlay's
    TEST_ASSERT_EQUAL_INT(SET_OK, line("dpad = arrows"));      TEST_ASSERT_TRUE(s.pad.dpad_arrows);
    TEST_ASSERT_EQUAL_INT(SET_BAD_VALUE, line("dpad = mouse"));
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

// PIZERO-181: the editor shows a settings file comments first, then sorted.
static void tidy_is(const char *in, const char *want) {
    char out[512];
    int n = settings_tidy(in, (int)strlen(in), out, sizeof out);
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void test_tidy_puts_comments_first_and_sorts_settings(void) {
    tidy_is("volume = 8\r\n# mine\n\n  font = classic   # old look\nartifact_colors = off\n# end\n",
            "# mine\n# end\n\nartifact_colors = off\nfont = classic   # old look\nvolume = 8\n");
}

static void test_tidy_keeps_repeated_names_in_order(void) {
    // The last of the same name wins when parsed, so it must stay last.
    tidy_is("volume = 8\nFONT = classic\nvolume = 3\n", "FONT = classic\nvolume = 8\nvolume = 3\n");
}

static void test_tidy_sorts_by_name_not_by_line(void) {
    // key_repeat before key_repeat_delay, though '_' sorts after ' ' and '='.
    tidy_is("key_repeat_delay = 500\nkey_repeat = on\n", "key_repeat = on\nkey_repeat_delay = 500\n");
}

static void test_tidy_edge_cases(void) {
    tidy_is("", "");
    tidy_is("\n\n  \n", "");
    tidy_is("# only a comment", "# only a comment\n");
    tidy_is("volume = 8", "volume = 8\n");
    tidy_is("oops no equals\nvolume = 8\n", "oops no equals\nvolume = 8\n");
}

static void test_tidy_is_stable_and_keeps_the_meaning(void) {
    // Tidying twice changes nothing, and the tidied template parses back to
    // the same settings as the template itself.
    char tmpl[1024], once[1024], twice[1024];
    int n = settings_template(tmpl, sizeof tmpl);
    int n1 = settings_tidy(tmpl, n, once, sizeof once);
    int n2 = settings_tidy(once, n1, twice, sizeof twice);
    TEST_ASSERT_EQUAL_INT(n1, n2);
    TEST_ASSERT_EQUAL_MEMORY(once, twice, n1);
    TEST_ASSERT_EQUAL_INT(n, n1 - 1);                    // only the blank line added
    once[n1] = 0;
    settings_defaults(&s);
    s.volume = 3;                                        // so the tidied text must reset it
    for (char *line = once, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        *nl = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(SET_OK, settings_parse_line(&s, line, NULL, 0), line);
    }
    struct coco_settings d; settings_defaults(&d);
    TEST_ASSERT_EQUAL_MEMORY(&d, &s, sizeof s);
}

// PIZERO-186: F8 writes one setting into a file, keeping the rest.
static void set_is(const char *in, const char *name, const char *value, const char *want) {
    char buf[256];
    int n = (int)strlen(in);
    memcpy(buf, in, (size_t)n);
    int m = settings_set_line(buf, n, sizeof buf, name, value);
    TEST_ASSERT_TRUE(m >= 0);
    buf[m] = 0;
    TEST_ASSERT_EQUAL_STRING(want, buf);
}

static void test_set_line_replaces_adds_and_keeps_the_rest(void) {
    set_is("# mine\nvolume = 8\nartifact_colors = on   # old\nfont = classic\n", "artifact_colors", "swapped",
           "# mine\nvolume = 8\nartifact_colors = swapped\nfont = classic\n");
    set_is("volume = 8\n", "artifact_colors", "off", "volume = 8\nartifact_colors = off\n");
    set_is("volume = 8", "artifact_colors", "off", "volume = 8\nartifact_colors = off\n");
    set_is("", "artifact_colors", "on", "artifact_colors = on\n");
    // The last of two is the one that counts, so it is the one changed;
    // a commented-out line is not a setting.
    set_is("ARTIFACT_COLORS = on\n# artifact_colors = off\nartifact_colors = off\n", "artifact_colors", "swapped",
           "ARTIFACT_COLORS = on\n# artifact_colors = off\nartifact_colors = swapped\n");
    char tiny[8] = "x = 1\n";
    TEST_ASSERT_EQUAL_INT(-1, settings_set_line(tiny, 6, 8, "artifact_colors", "on"));
}

static void test_the_template_parses_back_to_the_defaults(void) {
    // The editor offers this when there is no file; saving it unchanged
    // must leave every setting exactly at its default, with no errors.
    char tmpl[1024];                                  // the overlay's buffer size
    int n = settings_template(tmpl, sizeof tmpl);
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof tmpl);
    settings_defaults(&s);
    s.volume = 3; s.sn76489 = false;                  // so the template must reset them
    char *line = tmpl;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(SET_OK, settings_parse_line(&s, line, NULL, 0), line);
        if (!nl) break;
        line = nl + 1;
    }
    struct coco_settings d; settings_defaults(&d);
    TEST_ASSERT_EQUAL_MEMORY(&d, &s, sizeof s);
}

static void test_game_settings_path(void) {
    char p[96];
    TEST_ASSERT_TRUE(settings_game_path("0:/coco/bin/ORBIT.BIN", p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("0:/coco/bin/ORBIT.TXT", p);
    TEST_ASSERT_TRUE(settings_game_path("0:/coco/cart/Popcorn (1981).ccc", p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("0:/coco/cart/Popcorn (1981).TXT", p);
    TEST_ASSERT_TRUE(settings_game_path("0:/coco/dsk/space.warp.dsk", p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("0:/coco/dsk/space.warp.TXT", p);   // last dot only
    // The machine's own files are never a game's settings file.
    TEST_ASSERT_FALSE(settings_game_path("0:/coco/SETTINGS.DSK", p, sizeof p));
    TEST_ASSERT_FALSE(settings_game_path("0:/coco/autorun.bin", p, sizeof p));
    TEST_ASSERT_TRUE(settings_game_path("0:/coco/SETTINGSX.DSK", p, sizeof p));  // only exact
    TEST_ASSERT_FALSE(settings_game_path("0:/coco/NOEXT", p, sizeof p));
    TEST_ASSERT_FALSE(settings_game_path("0:/coco/dsk/ORBIT.BIN", p, 12));      // too long
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_todays_behaviour);
    RUN_TEST(test_every_setting);
    RUN_TEST(test_case_spacing_and_comments);
    RUN_TEST(test_bad_lines_are_reported_and_change_nothing);
    RUN_TEST(test_rgb565_rounding);
    RUN_TEST(test_the_template_parses_back_to_the_defaults);
    RUN_TEST(test_set_line_replaces_adds_and_keeps_the_rest);
    RUN_TEST(test_tidy_puts_comments_first_and_sorts_settings);
    RUN_TEST(test_tidy_keeps_repeated_names_in_order);
    RUN_TEST(test_tidy_sorts_by_name_not_by_line);
    RUN_TEST(test_tidy_edge_cases);
    RUN_TEST(test_tidy_is_stable_and_keeps_the_meaning);
    RUN_TEST(test_game_settings_path);
    return UNITY_END();
}

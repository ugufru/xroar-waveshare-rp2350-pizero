// PIZERO-81d: host tests for finding the program to run on a Disk BASIC disk.
//
// Getting this wrong types RUN"SOMETHING" at a program that is not there, or
// runs a data file, so the rules are pinned here rather than on hardware.
//
//   pio test -e native

#include <unity.h>

#include <cstring>

#include "../../src/rsdos_dir.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t sec[256];

static void entry(int slot, const char *name8, const char *ext3, uint8_t type) {
    uint8_t *d = sec + slot * 32;
    memset(d, ' ', 11);
    memcpy(d, name8, strlen(name8));
    memcpy(d + 8, ext3, 3);
    d[11] = type;
}

static void blank(void) { memset(sec, 0, sizeof sec); }

static void test_first_basic_program_wins(void) {
    blank();
    entry(0, "HELLO", "BAS", RSDOS_BASIC);
    entry(1, "GAME", "BIN", RSDOS_ML);
    char name[9]; int type = -1;
    TEST_ASSERT_EQUAL_INT(1, rsdos_scan_sector(sec, name, &type));
    TEST_ASSERT_EQUAL_STRING("HELLO", name);
    TEST_ASSERT_EQUAL_INT(RSDOS_BASIC, type);
}

static void test_data_and_text_files_are_skipped(void) {
    blank();
    entry(0, "SCORES", "DAT", 1);
    entry(1, "README", "TXT", 3);
    entry(2, "GAME", "BIN", RSDOS_ML);
    char name[9]; int type = -1;
    TEST_ASSERT_EQUAL_INT(1, rsdos_scan_sector(sec, name, &type));
    TEST_ASSERT_EQUAL_STRING("GAME", name);
    TEST_ASSERT_EQUAL_INT(RSDOS_ML, type);
}

static void test_deleted_entries_are_skipped_and_ff_ends(void) {
    blank();
    entry(0, "GONE", "BAS", RSDOS_BASIC);
    sec[0] = 0x00;                            // deleted
    sec[32] = 0xFF;                           // end of directory
    char name[9]; int type = -1;
    TEST_ASSERT_EQUAL_INT(-1, rsdos_scan_sector(sec, name, &type));
}

static void test_a_sector_with_nothing_runnable_says_keep_looking(void) {
    blank();
    entry(0, "DATA", "DAT", 1);
    char name[9]; int type = -1;
    TEST_ASSERT_EQUAL_INT(0, rsdos_scan_sector(sec, name, &type));
}

static void test_full_eight_character_name(void) {
    blank();
    entry(0, "ABCDEFGH", "BAS", RSDOS_BASIC);
    char name[9]; int type = -1;
    TEST_ASSERT_EQUAL_INT(1, rsdos_scan_sector(sec, name, &type));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGH", name);
}

static void test_commands(void) {
    char out[32];
    rsdos_run_command("HELLO", RSDOS_BASIC, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("RUN\"HELLO\"\r", out);
    rsdos_run_command("GAME", RSDOS_ML, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("LOADM\"GAME\":EXEC\r", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_basic_program_wins);
    RUN_TEST(test_data_and_text_files_are_skipped);
    RUN_TEST(test_deleted_entries_are_skipped_and_ff_ends);
    RUN_TEST(test_a_sector_with_nothing_runnable_says_keep_looking);
    RUN_TEST(test_full_eight_character_name);
    RUN_TEST(test_commands);
    return UNITY_END();
}

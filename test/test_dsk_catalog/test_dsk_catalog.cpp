// PIZERO-81a: host tests for the disk-image catalogue behind the F12 overlay.
//
// The list is the whole interface for choosing a disk, so a file that is
// silently missing, listed twice, or listed under a path that cannot be
// opened again is a real bug, not a cosmetic one.
//
//   pio test -e native

#include <unity.h>

#include <cstring>
#include <string>

#include "../../src/dsk_catalog.h"

void setUp(void) {}
void tearDown(void) {}

static struct dsk_catalog cat;

static void test_only_dsk_files_count(void) {
    TEST_ASSERT_TRUE(dsk_cat_is_image("GAME.DSK", false));
    TEST_ASSERT_TRUE(dsk_cat_is_image("game.dsk", false));
    TEST_ASSERT_TRUE(dsk_cat_is_image("Mixed.Dsk", false));
    TEST_ASSERT_FALSE(dsk_cat_is_image("GAME.BIN", false));
    TEST_ASSERT_FALSE(dsk_cat_is_image("GAME.DSK.TXT", false));
    TEST_ASSERT_FALSE(dsk_cat_is_image("DSK", false));
    TEST_ASSERT_FALSE(dsk_cat_is_image("", false));
}

static void test_directories_and_dotfiles_are_skipped(void) {
    TEST_ASSERT_FALSE(dsk_cat_is_image("FOLDER.DSK", true));
    // macOS writes an AppleDouble "._" twin next to every file it touches,
    // and it ends in .dsk. Listing it offers a disk that is not a disk.
    TEST_ASSERT_FALSE(dsk_cat_is_image("._GAME.DSK", false));
    TEST_ASSERT_FALSE(dsk_cat_is_image(".DSK", false));
}

static void test_list_is_sorted_case_insensitively(void) {
    dsk_cat_clear(&cat);
    dsk_cat_add(&cat, "zaxxon.dsk", false, DSK_DIR_DSK);
    dsk_cat_add(&cat, "Arcade.DSK", false, DSK_DIR_DSK);
    dsk_cat_add(&cat, "MEGABUG.DSK", false, DSK_DIR_DSK);
    dsk_cat_sort(&cat);
    TEST_ASSERT_EQUAL_INT(3, cat.n);
    TEST_ASSERT_EQUAL_STRING("Arcade.DSK", cat.e[0].name);
    TEST_ASSERT_EQUAL_STRING("MEGABUG.DSK", cat.e[1].name);
    TEST_ASSERT_EQUAL_STRING("zaxxon.dsk", cat.e[2].name);
}

static void test_a_name_in_both_folders_is_listed_once_from_dsk(void) {
    dsk_cat_clear(&cat);
    dsk_cat_add(&cat, "GAME.DSK", false, DSK_DIR_DSK);
    TEST_ASSERT_FALSE(dsk_cat_add(&cat, "game.dsk", false, DSK_DIR_ROOT));
    TEST_ASSERT_EQUAL_INT(1, cat.n);
    TEST_ASSERT_EQUAL_UINT8(DSK_DIR_DSK, cat.e[0].dir);
}

static void test_long_names_are_counted_not_truncated(void) {
    dsk_cat_clear(&cat);
    std::string longname(DSK_NAME_MAX - 4, 'A');   // + ".DSK" = DSK_NAME_MAX
    longname += ".DSK";
    TEST_ASSERT_FALSE(dsk_cat_add(&cat, longname.c_str(), false, DSK_DIR_DSK));
    TEST_ASSERT_EQUAL_INT(0, cat.n);
    TEST_ASSERT_EQUAL_INT(1, cat.skipped_long);

    std::string fits(DSK_NAME_MAX - 5, 'B');       // one shorter: fits exactly
    fits += ".DSK";
    TEST_ASSERT_TRUE(dsk_cat_add(&cat, fits.c_str(), false, DSK_DIR_DSK));
    TEST_ASSERT_EQUAL_STRING(fits.c_str(), cat.e[0].name);
}

static void test_the_cap_counts_what_it_drops(void) {
    dsk_cat_clear(&cat);
    char name[16];
    for (int i = 0; i < DSK_CAT_MAX + 5; i++) {
        snprintf(name, sizeof name, "D%03d.DSK", i);
        dsk_cat_add(&cat, name, false, DSK_DIR_DSK);
    }
    TEST_ASSERT_EQUAL_INT(DSK_CAT_MAX, cat.n);
    TEST_ASSERT_EQUAL_INT(5, cat.skipped_full);
}

static void test_paths_round_trip(void) {
    dsk_cat_clear(&cat);
    dsk_cat_add(&cat, "IN_DSK.DSK", false, DSK_DIR_DSK);
    dsk_cat_add(&cat, "IN_ROOT.DSK", false, DSK_DIR_ROOT);
    char p[96];
    TEST_ASSERT_TRUE(dsk_cat_path(&cat, 0, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("0:/coco/dsk/IN_DSK.DSK", p);
    TEST_ASSERT_TRUE(dsk_cat_path(&cat, 1, p, sizeof p));
    TEST_ASSERT_EQUAL_STRING("0:/coco/IN_ROOT.DSK", p);
    // The overlay finds the row for whatever autorun.txt mounted at boot,
    // whose path may differ in case from the directory listing.
    TEST_ASSERT_EQUAL_INT(1, dsk_cat_find_path(&cat, "0:/coco/in_root.dsk"));
    TEST_ASSERT_EQUAL_INT(-1, dsk_cat_find_path(&cat, "0:/coco/dsk/IN_ROOT.DSK"));
    TEST_ASSERT_FALSE(dsk_cat_path(&cat, 2, p, sizeof p));
    TEST_ASSERT_FALSE(dsk_cat_path(&cat, 0, p, 8));        // does not fit
}

static void test_display_name_drops_the_extension_and_clips(void) {
    char out[32];
    dsk_cat_display_name("GAMES.DSK", out, 27);
    TEST_ASSERT_EQUAL_STRING("GAMES", out);
    dsk_cat_display_name("mixed.Dsk", out, 27);
    TEST_ASSERT_EQUAL_STRING("mixed", out);
    dsk_cat_display_name("A_VERY_LONG_DISK_IMAGE_NAME_INDEED.DSK", out, 10);
    TEST_ASSERT_EQUAL_STRING("A_VERY_LON", out);
    dsk_cat_display_name("NOEXT", out, 27);
    TEST_ASSERT_EQUAL_STRING("NOEXT", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_only_dsk_files_count);
    RUN_TEST(test_directories_and_dotfiles_are_skipped);
    RUN_TEST(test_list_is_sorted_case_insensitively);
    RUN_TEST(test_a_name_in_both_folders_is_listed_once_from_dsk);
    RUN_TEST(test_long_names_are_counted_not_truncated);
    RUN_TEST(test_the_cap_counts_what_it_drops);
    RUN_TEST(test_paths_round_trip);
    RUN_TEST(test_display_name_drops_the_extension_and_clips);
    return UNITY_END();
}

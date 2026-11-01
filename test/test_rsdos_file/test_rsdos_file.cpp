// PIZERO-309: host tests for rsdos_file.h, reading a file out of a Disk
// BASIC disk built in memory.
//
//   pio test -e native -f test_rsdos_file

#include <unity.h>
#include <string.h>

#include "../../src/rsdos_file.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t disk[35 * 18 * 256];

static uint8_t *sector(unsigned t, unsigned s) { return disk + ((t * 18) + (s - 1)) * 256; }

static int rd(void *, unsigned t, unsigned s, uint8_t out[256]) {
    if (t >= 35 || s < 1 || s > 18) return 1;
    memcpy(out, sector(t, s), 256);
    return 0;
}

static void test_granule_geometry(void) {
    unsigned t, s;
    rsdos_granule_ts(0, &t, &s);  TEST_ASSERT_EQUAL_UINT(0, t);  TEST_ASSERT_EQUAL_UINT(1, s);
    rsdos_granule_ts(1, &t, &s);  TEST_ASSERT_EQUAL_UINT(0, t);  TEST_ASSERT_EQUAL_UINT(10, s);
    rsdos_granule_ts(33, &t, &s); TEST_ASSERT_EQUAL_UINT(16, t); TEST_ASSERT_EQUAL_UINT(10, s);
    rsdos_granule_ts(34, &t, &s); TEST_ASSERT_EQUAL_UINT(18, t); TEST_ASSERT_EQUAL_UINT(1, s);   // skips 17
    rsdos_granule_ts(67, &t, &s); TEST_ASSERT_EQUAL_UINT(34, t); TEST_ASSERT_EQUAL_UINT(10, s);
}

// A file of `len` bytes, all `fill`, in granules g0 then g1, the last using
// `used` sectors; its entry at directory slot 0.
static uint8_t fat[RSDOS_GRANULES];
static uint8_t entry[32];

static void make_two_granule_file(uint32_t len) {
    memset(disk, 0xFF, sizeof disk);
    memset(fat, 0xFF, sizeof fat);
    fat[5] = 40;                                    // 5 -> 40
    uint32_t in_last = len - 9 * 256;
    unsigned used = (in_last + 255) / 256;
    fat[40] = (uint8_t)(0xC0 + used);
    unsigned last = in_last - (used - 1) * 256;
    memset(entry, ' ', 11);
    memcpy(entry, "README  TXT", 11);
    entry[11] = 1; entry[12] = 0xFF; entry[13] = 5;
    entry[14] = (uint8_t)(last >> 8); entry[15] = (uint8_t)last;
    // Content: byte i is 'A' + i % 26, so order and position are checked.
    uint32_t i = 0;
    int g = 5;
    while (i < len) {
        unsigned t, s;
        rsdos_granule_ts(g, &t, &s);
        for (int k = 0; k < 9 && i < len; k++)
            for (int b = 0; b < 256 && i < len; b++, i++) sector(t, s + k)[b] = (uint8_t)('A' + i % 26);
        g = 40;
    }
}

static void test_size_and_read_follow_the_chain(void) {
    const uint32_t len = 9 * 256 + 2 * 256 + 100;   // a full granule, then 3 sectors
    make_two_granule_file(len);
    uint32_t size = 0;
    TEST_ASSERT_EQUAL_INT(RSDOS_OK, rsdos_file_size(fat, entry, &size));
    TEST_ASSERT_EQUAL_UINT32(len, size);
    static char out[8192];
    uint8_t sec[256];
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(RSDOS_OK, rsdos_read_file(rd, nullptr, fat, entry, sec, out, sizeof out, &got));
    TEST_ASSERT_EQUAL_UINT32(len, got);
    for (uint32_t i = 0; i < len; i++) TEST_ASSERT_EQUAL_CHAR('A' + i % 26, out[i]);
}

static void test_too_big_is_refused_with_its_size(void) {
    make_two_granule_file(9 * 256 + 300);
    static char out[1024];
    uint8_t sec[256];
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(RSDOS_TOO_BIG, rsdos_read_file(rd, nullptr, fat, entry, sec, out, sizeof out, &got));
    TEST_ASSERT_EQUAL_UINT32(9 * 256 + 300, got);
}

static void test_broken_chains(void) {
    make_two_granule_file(9 * 256 + 10);
    uint32_t size;
    fat[40] = 5;                                    // loops 5 -> 40 -> 5
    TEST_ASSERT_EQUAL_INT(RSDOS_BROKEN, rsdos_file_size(fat, entry, &size));
    fat[40] = 0xFF;                                 // runs onto a free granule
    TEST_ASSERT_EQUAL_INT(RSDOS_BROKEN, rsdos_file_size(fat, entry, &size));
    entry[13] = 80;                                 // starts off the disk
    TEST_ASSERT_EQUAL_INT(RSDOS_BROKEN, rsdos_file_size(fat, entry, &size));
}

static void test_entry_names_and_flags(void) {
    uint8_t e[32];
    char name[15];
    memset(e, ' ', 11); memcpy(e, "README  TXT", 11); e[12] = 0xFF;
    TEST_ASSERT_TRUE(rsdos_entry_name(e, 1, name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("README.TXT:1", name);
    TEST_ASSERT_TRUE(rsdos_entry_is_ascii(e));
    memset(e, ' ', 11); memcpy(e, "LICENSE ", 8);
    TEST_ASSERT_TRUE(rsdos_entry_name(e, 0, name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("LICENSE:0", name);
    e[12] = 0x00;
    TEST_ASSERT_FALSE(rsdos_entry_is_ascii(e));
    e[0] = 0x00;                                    // deleted
    TEST_ASSERT_FALSE(rsdos_entry_name(e, 0, name, sizeof name));
    e[0] = 0xFF;                                    // never used
    TEST_ASSERT_FALSE(rsdos_entry_name(e, 0, name, sizeof name));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_granule_geometry);
    RUN_TEST(test_size_and_read_follow_the_chain);
    RUN_TEST(test_too_big_is_refused_with_its_size);
    RUN_TEST(test_broken_chains);
    RUN_TEST(test_entry_names_and_flags);
    return UNITY_END();
}

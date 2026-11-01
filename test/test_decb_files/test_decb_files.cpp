// PIZERO-289: host tests for decb_files.h, Disk BASIC's file rules for
// files on the card: the 8.3 rule, and the file type from the content.
//
//   pio test -e native -f test_decb_files

#include <unity.h>
#include <string.h>

#include "../../src/decb_files.h"

void setUp(void) {}
void tearDown(void) {}

static void test_83_names_that_pass(void) {
    const char *ok[] = { "ORBIT.BAS", "orbit.bas", "LICENSE", "README.md", "A", "A.B",
                         "12345678.123", "MY-GAME.BIN", "X_1.DAT", "{}~!.$%&" };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
        TEST_ASSERT_TRUE_MESSAGE(decb_is_83(ok[i]), ok[i]);
}

static void test_83_names_that_are_ignored(void) {
    const char *no[] = { "", ".gitignore", ".git", "CHANGELOG.md", "123456789", "A.BASX",
                         "A.", "A.B.C", "MY GAME.BAS", "A*B", "A+B.BAS", "A:B", "A/B",
                         "caf\xc3\xa9.txt", "A;1", "[X].BAS" };
    for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++)
        TEST_ASSERT_FALSE_MESSAGE(decb_is_83(no[i]), no[i]);
}

static void test_from_host_pads_and_folds(void) {
    char n[8], e[3];
    TEST_ASSERT_TRUE(decb_from_host("orbit.bas", n, e));
    TEST_ASSERT_EQUAL_MEMORY("ORBIT   ", n, 8);
    TEST_ASSERT_EQUAL_MEMORY("BAS", e, 3);
    TEST_ASSERT_TRUE(decb_from_host("LICENSE", n, e));
    TEST_ASSERT_EQUAL_MEMORY("LICENSE ", n, 8);
    TEST_ASSERT_EQUAL_MEMORY("   ", e, 3);
    TEST_ASSERT_TRUE(decb_from_host("README.md", n, e));
    TEST_ASSERT_EQUAL_MEMORY("README  ", n, 8);
    TEST_ASSERT_EQUAL_MEMORY("MD ", e, 3);
}

static void test_to_host_and_back(void) {
    char out[13];
    TEST_ASSERT_TRUE(decb_to_host("ORBIT   ", "BAS", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ORBIT.BAS", out);
    TEST_ASSERT_TRUE(decb_to_host("LICENSE ", "   ", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("LICENSE", out);
    TEST_ASSERT_TRUE(decb_to_host("12345678", "123", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("12345678.123", out);
    // Names Disk BASIC can make but the card cannot hold are refused.
    TEST_ASSERT_FALSE(decb_to_host("A*B     ", "BAS", out, sizeof out));
    TEST_ASSERT_FALSE(decb_to_host("MY GAME ", "BAS", out, sizeof out));
    TEST_ASSERT_FALSE(decb_to_host("        ", "BAS", out, sizeof out));
    TEST_ASSERT_FALSE(decb_to_host("A?      ", "   ", out, sizeof out));
    TEST_ASSERT_FALSE(decb_to_host("ORBIT   ", "BAS", out, 12));     // no room
    // Round trip: every 8.3 name comes back as itself, upper case.
    char n[8], e[3];
    TEST_ASSERT_TRUE(decb_from_host("my-game.bin", n, e));
    TEST_ASSERT_TRUE(decb_to_host(n, e, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("MY-GAME.BIN", out);
}

static void test_text_detection(void) {
    const char *t1 = "10 PRINT \"HELLO\"\r20 GOTO 10\r";
    TEST_ASSERT_TRUE(decb_is_text((const uint8_t *)t1, (int)strlen(t1)));
    const char *t2 = "# Title\n\nIt\xe2\x80\x99s a README\twith tabs.\n";   // a curly quote
    TEST_ASSERT_TRUE(decb_is_text((const uint8_t *)t2, (int)strlen(t2)));
    TEST_ASSERT_TRUE(decb_is_text((const uint8_t *)"", 0));
    const uint8_t cut[] = { 'o', 'k', 0xE2, 0x80 };                 // cut off by the sample
    TEST_ASSERT_TRUE(decb_is_text(cut, sizeof cut));
    const uint8_t nul[] = { 'a', 0x00, 'b' };
    TEST_ASSERT_FALSE(decb_is_text(nul, sizeof nul));
    const uint8_t ff[] = { 0xFF, 0x00, 0x10 };
    TEST_ASSERT_FALSE(decb_is_text(ff, sizeof ff));
    const uint8_t bad[] = { 'a', 0xC3, 'b' };                       // broken UTF-8
    TEST_ASSERT_FALSE(decb_is_text(bad, sizeof bad));
    const uint8_t latin1[] = { 'c', 'a', 'f', 0xE9 , ' '};          // Latin-1, not UTF-8
    TEST_ASSERT_FALSE(decb_is_text(latin1, sizeof latin1));
    const uint8_t ctl[] = { 'a', 0x07, 'b' };
    TEST_ASSERT_FALSE(decb_is_text(ctl, sizeof ctl));
}

static void sniff(const uint8_t *data, uint32_t size, const char *ext, uint8_t want_type, uint8_t want_ascii) {
    char e3[3] = { ' ', ' ', ' ' };
    for (int i = 0; i < 3 && ext[i]; i++) e3[i] = ext[i];
    uint8_t type = 99, ascii = 99;
    decb_sniff(data, size < 512 ? (int)size : 512, size >= 10 ? data + size - 5 : NULL, size, e3, &type, &ascii);
    TEST_ASSERT_EQUAL_UINT8(want_type, type);
    TEST_ASSERT_EQUAL_UINT8(want_ascii, ascii);
}

static void test_sniff_types(void) {
    // Tokenized BASIC: $FF, length, program.
    const uint8_t tok[] = { 0xFF, 0x00, 0x0A, 0x26, 0x09, 0x00, 0x0A, 0x87, 0x20, 0x31, 0x00, 0x00, 0x00 };
    sniff(tok, sizeof tok, "BAS", DECB_T_BASIC, DECB_BINARY);
    sniff(tok, sizeof tok, "DAT", DECB_T_BASIC, DECB_BINARY);       // content wins
    // LOADM: one 3-byte segment at $0E00, exec $0E00.
    const uint8_t ml[] = { 0x00, 0x00, 0x03, 0x0E, 0x00, 0x86, 0x41, 0x39, 0xFF, 0x00, 0x00, 0x0E, 0x00 };
    sniff(ml, sizeof ml, "BIN", DECB_T_ML, DECB_BINARY);
    sniff(ml, sizeof ml, "", DECB_T_ML, DECB_BINARY);
    // ASCII: a BASIC listing, assembler source, a README, a licence.
    const char *bas = "10 PRINT \"HI\"\r";
    sniff((const uint8_t *)bas, (uint32_t)strlen(bas), "BAS", DECB_T_BASIC, DECB_ASCII);
    sniff((const uint8_t *)bas, (uint32_t)strlen(bas), "ASM", DECB_T_TEXT, DECB_ASCII);
    sniff((const uint8_t *)bas, (uint32_t)strlen(bas), "MD", DECB_T_DATA, DECB_ASCII);
    sniff((const uint8_t *)bas, (uint32_t)strlen(bas), "", DECB_T_DATA, DECB_ASCII);
    // Binary that is neither: data, or offered to LOADM if it says .BIN.
    const uint8_t blob[] = { 0x01, 0x02, 0x00, 0x7F, 0x80, 0x90, 0x00, 0x01, 0x02, 0x03, 0x04 };
    sniff(blob, sizeof blob, "DAT", DECB_T_DATA, DECB_BINARY);
    sniff(blob, sizeof blob, "BIN", DECB_T_ML, DECB_BINARY);
    // A preamble whose segment runs past the end is not LOADM.
    const uint8_t badml[] = { 0x00, 0x40, 0x00, 0x0E, 0x00, 0x86, 0x41, 0x39, 0xFF, 0x00, 0x00, 0x0E, 0x00 };
    sniff(badml, sizeof badml, "DAT", DECB_T_DATA, DECB_BINARY);
    // Empty file: text, data.
    sniff((const uint8_t *)"", 0, "DAT", DECB_T_DATA, DECB_ASCII);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_83_names_that_pass);
    RUN_TEST(test_83_names_that_are_ignored);
    RUN_TEST(test_from_host_pads_and_folds);
    RUN_TEST(test_to_host_and_back);
    RUN_TEST(test_text_detection);
    RUN_TEST(test_sniff_types);
    return UNITY_END();
}

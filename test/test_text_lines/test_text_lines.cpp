// PIZERO-294: host tests for text_lines.h. A line ends at CR, LF or CRLF,
// so a file written on the CoCo, the Mac or Windows reads the same.
//
//   pio test -e native -f test_text_lines

#include <unity.h>
#include <string.h>

#include "../../src/text_lines.h"

void setUp(void) {}
void tearDown(void) {}

// Split a buffer with tl_next and join the lines with '|'.
static const char *split(const char *b) {
    static char out[256];
    int n = (int)strlen(b), k = 0, s, l;
    for (int pos = 0; pos < n;) {
        pos = tl_next(b, n, pos, &s, &l);
        if (k) out[k++] = '|';
        memcpy(out + k, b + s, (size_t)l);
        k += l;
    }
    out[k] = '\0';
    return out;
}

static void test_next_splits_every_convention(void) {
    TEST_ASSERT_EQUAL_STRING("a|b|c", split("a\nb\nc\n"));
    TEST_ASSERT_EQUAL_STRING("a|b|c", split("a\r\nb\r\nc\r\n"));
    TEST_ASSERT_EQUAL_STRING("a|b|c", split("a\rb\rc\r"));
    TEST_ASSERT_EQUAL_STRING("a|b|c", split("a\nb\rc"));      // mixed, no last line end
}

static void test_next_keeps_blank_lines(void) {
    TEST_ASSERT_EQUAL_STRING("a||b", split("a\n\nb\n"));
    TEST_ASSERT_EQUAL_STRING("a||b", split("a\r\rb\r"));
    TEST_ASSERT_EQUAL_STRING("a||b", split("a\r\n\r\nb\r\n"));
    TEST_ASSERT_EQUAL_STRING("a||b", split("a\n\rb"));          // LF then CR is two ends
}

static void test_convention_is_the_first_line_end(void) {
    TEST_ASSERT_EQUAL_INT(TL_LF,   tl_convention("a\nb\r", 4));
    TEST_ASSERT_EQUAL_INT(TL_CR,   tl_convention("a\rb\n", 4));
    TEST_ASSERT_EQUAL_INT(TL_CRLF, tl_convention("a\r\nb", 4));
    TEST_ASSERT_EQUAL_INT(TL_CR,   tl_convention("a\r", 2));    // CR last in the buffer
    TEST_ASSERT_EQUAL_INT(TL_NONE, tl_convention("abc", 3));
    TEST_ASSERT_EQUAL_INT(TL_NONE, tl_convention("", 0));
    TEST_ASSERT_EQUAL_STRING("\r", tl_eol(TL_CR));
    TEST_ASSERT_EQUAL_STRING("\r\n", tl_eol(TL_CRLF));
    TEST_ASSERT_EQUAL_STRING("\n", tl_eol(TL_LF));
    TEST_ASSERT_EQUAL_STRING("\n", tl_eol(TL_NONE));
}

// ---- the streaming reader, fed a few bytes at a time so a CRLF straddles
// the refill boundary ----

struct src { const char *s; int n, pos, chunk; };
static int src_read(void *ctx, char *buf, int n) {
    struct src *p = (struct src *)ctx;
    int k = p->n - p->pos;
    if (k > n) k = n;
    if (k > p->chunk) k = p->chunk;
    memcpy(buf, p->s + p->pos, (size_t)k);
    p->pos += k;
    return k;
}

static const char *gets_all(const char *text, int chunk, int cap) {
    static char out[512];
    struct src p = { text, (int)strlen(text), 0, chunk };
    struct tl_reader r;
    tl_reader_init(&r, src_read, &p);
    char line[64];
    int k = 0;
    while (tl_gets(&r, line, cap)) {
        if (k) out[k++] = '|';
        int l = (int)strlen(line);
        memcpy(out + k, line, (size_t)l);
        k += l;
    }
    out[k] = '\0';
    return out;
}

static void test_gets_every_convention_any_chunking(void) {
    for (int chunk = 1; chunk <= 7; chunk++) {
        TEST_ASSERT_EQUAL_STRING("one|two|three", gets_all("one\ntwo\nthree\n", chunk, 64));
        TEST_ASSERT_EQUAL_STRING("one|two|three", gets_all("one\r\ntwo\r\nthree\r\n", chunk, 64));
        TEST_ASSERT_EQUAL_STRING("one|two|three", gets_all("one\rtwo\rthree\r", chunk, 64));
        TEST_ASSERT_EQUAL_STRING("one||three", gets_all("one\r\n\r\nthree", chunk, 64));
    }
}

static void test_gets_last_line_without_an_end(void) {
    TEST_ASSERT_EQUAL_STRING("a|b", gets_all("a\nb", 64, 64));
    TEST_ASSERT_EQUAL_STRING("", gets_all("", 64, 64));
}

// A line longer than the buffer is cut, and its tail is NOT a second line.
static void test_gets_cuts_a_long_line_once(void) {
    TEST_ASSERT_EQUAL_STRING("abcd|next", gets_all("abcdefghij\nnext\n", 3, 5));
    TEST_ASSERT_EQUAL_STRING("abcd|next", gets_all("abcdefghij\rnext\r", 3, 5));
}

// PIZERO-309: CR and CRLF become LF for the read-only view.
static void test_to_lf(void) {
    char a[] = "10 PRINT\r20 END\r";
    int n = tl_to_lf(a, (int)strlen(a)); a[n] = 0;
    TEST_ASSERT_EQUAL_STRING("10 PRINT\n20 END\n", a);
    char b[] = "x\r\ny\r\rz\n";
    n = tl_to_lf(b, (int)strlen(b)); b[n] = 0;
    TEST_ASSERT_EQUAL_STRING("x\ny\n\nz\n", b);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_next_splits_every_convention);
    RUN_TEST(test_next_keeps_blank_lines);
    RUN_TEST(test_convention_is_the_first_line_end);
    RUN_TEST(test_gets_every_convention_any_chunking);
    RUN_TEST(test_gets_last_line_without_an_end);
    RUN_TEST(test_gets_cuts_a_long_line_once);
    RUN_TEST(test_to_lf);
    return UNITY_END();
}

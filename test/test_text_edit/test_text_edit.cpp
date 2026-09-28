// PIZERO-146: host tests for the on-screen text editor.
//
// This edits the machine's own settings file, so a lost character, a
// cursor that lands in the wrong place, or an ESC that throws away work
// is a real bug. The buffer and the key handling are both pinned here.
//
//   pio test -e native

#include <unity.h>

#include <cstring>

#include "../../src/text_edit_keys.h"
#include "../../src/text_card.h"
#include "../../src/file_templates.h"

void setUp(void) {}
void tearDown(void) {}

static struct text_edit t;
static struct tek_state k;
static uint32_t frame;

static void load(const char *s) { ted_load(&t, s, (int)strlen(s)); tek_init(&k); frame = 100; }

static void text_is(const char *s) {
    TEST_ASSERT_EQUAL_INT((int)strlen(s), t.len);
    TEST_ASSERT_EQUAL_MEMORY(s, t.buf, t.len ? t.len : 1);
}

// Press one key (with modifiers) and release it.
static uint8_t key(uint8_t code, uint8_t mods = 0) {
    uint8_t c[6] = { code, 0, 0, 0, 0, 0 }, none[6] = { 0 };
    uint8_t a = tek_report(&k, &t, mods, c, frame++);
    tek_report(&k, &t, 0, none, frame++);
    return a;
}

static void type(const char *s) {
    for (; *s; s++) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') key((uint8_t)(0x04 + ch - 'a'));
        else if (ch >= 'A' && ch <= 'Z') key((uint8_t)(0x04 + ch - 'A'), 0x02);
        else if (ch == ' ') key(0x2C);
        else if (ch == '=') key(0x2E);
        else if (ch == '_') key(0x2D, 0x02);
        else if (ch == '#') key(0x20, 0x02);
        else if (ch >= '1' && ch <= '9') key((uint8_t)(0x1E + ch - '1'));
        else if (ch == '0') key(0x27);
        else if (ch == '\n') key(0x28);
    }
}

// ---- the buffer -----------------------------------------------------------

static void test_load_drops_carriage_returns(void) {
    load("a = 1\r\nb = 2\r\n");
    text_is("a = 1\nb = 2\n");
    TEST_ASSERT_FALSE(t.dirty);
    TEST_ASSERT_EQUAL_INT(3, ted_lines(&t));
}

static void test_typing_inserts_at_the_cursor(void) {
    load("");
    type("volume = 12");
    text_is("volume = 12");
    TEST_ASSERT_TRUE(t.dirty);
    key(0x4A);                               // Home
    type("# ");
    text_is("# volume = 12");
}

static void test_shift_gives_upper_case_and_symbols(void) {
    load("");
    type("SN_A #");
    text_is("SN_A #");
    TEST_ASSERT_EQUAL_CHAR('!', tek_ascii(0x1E, true));
    TEST_ASSERT_EQUAL_CHAR('1', tek_ascii(0x1E, false));
    TEST_ASSERT_EQUAL_CHAR(':', tek_ascii(0x33, true));
    TEST_ASSERT_EQUAL_CHAR(0, tek_ascii(0x3A, false));   // F1: not printable
}

static void test_enter_backspace_delete(void) {
    load("ab");
    key(0x4F);                               // Right: between a and b
    key(0x28);                               // Enter splits the line
    text_is("a\nb");
    key(0x2A);                               // Backspace joins it again
    text_is("ab");
    key(0x4C);                               // Delete removes the b
    text_is("a");
    key(0x4A); key(0x2A);                    // Backspace at the start: nothing
    text_is("a");
}

static void test_up_down_keep_the_column(void) {
    load("long line here\nab\nanother long line");
    key(0x4D);                               // End of line 1: column 14
    key(0x51);                               // Down onto the short line
    TEST_ASSERT_EQUAL_INT(1, ted_line_of(&t, t.cur));
    TEST_ASSERT_EQUAL_INT(2, ted_col_of(&t, t.cur));     // clamped to its end
    key(0x51);                               // Down again: back to column 14
    TEST_ASSERT_EQUAL_INT(2, ted_line_of(&t, t.cur));
    TEST_ASSERT_EQUAL_INT(14, ted_col_of(&t, t.cur));
    key(0x52); key(0x52); key(0x52);         // Up past the top stays on line 0
    TEST_ASSERT_EQUAL_INT(0, ted_line_of(&t, t.cur));
}

static void test_the_view_follows_the_cursor(void) {
    char big[600] = "";
    for (int i = 0; i < 40; i++) strcat(big, "line\n");
    load(big);
    for (int i = 0; i < 20; i++) key(0x51);  // Down 20 lines
    TEST_ASSERT_EQUAL_INT(20, ted_line_of(&t, t.cur));
    TEST_ASSERT_EQUAL_INT(20 - TEK_ROWS + 1, t.top);   // cursor on the last row
    key(0x4B);                               // PgUp
    TEST_ASSERT_EQUAL_INT(20 - TEK_ROWS, ted_line_of(&t, t.cur));
    // A long line scrolls sideways.
    load("0123456789012345678901234567890123456789");
    key(0x4D);                               // End: column 40
    TEST_ASSERT_EQUAL_INT(40 - TEK_COLS + 1, t.left);
    char row[40];
    ted_row(&t, 0, t.left, TEK_COLS, row);
    TEST_ASSERT_EQUAL_STRING("9012345678901234567890123456789", row);
}

static void test_a_full_buffer_refuses_and_says_so(void) {
    static char full[TED_MAX + 1];
    memset(full, 'x', TED_MAX); full[TED_MAX] = 0;
    load(full);
    TEST_ASSERT_EQUAL_INT(TED_MAX, t.len);
    TEST_ASSERT_FALSE(ted_insert(&t, 'y'));
    TEST_ASSERT_TRUE(t.full);
    TEST_ASSERT_EQUAL_INT(TED_MAX, t.len);
}

// ---- the keys -------------------------------------------------------------

static void test_ctrl_s_saves(void) {
    load("a");
    TEST_ASSERT_EQUAL_UINT8(TEK_SAVE, key(0x16, 0x01));  // left Ctrl
    TEST_ASSERT_EQUAL_UINT8(TEK_SAVE, key(0x16, 0x10));  // right Ctrl
    text_is("a");                                        // no 's' typed
}

static void test_esc_protects_unsaved_changes(void) {
    load("a");
    TEST_ASSERT_EQUAL_UINT8(TEK_CANCEL, key(0x29));      // unchanged: leaves at once
    load("a");
    type("b");
    TEST_ASSERT_EQUAL_UINT8(TEK_WARN_UNSAVED, key(0x29));
    TEST_ASSERT_EQUAL_UINT8(TEK_CANCEL, key(0x29));      // second ESC discards
    load("a");
    type("b");
    key(0x29);                                           // warns
    type("c");                                           // any other key disarms
    TEST_ASSERT_EQUAL_UINT8(TEK_WARN_UNSAVED, key(0x29));
}

static void test_a_held_key_repeats_and_stops(void) {
    load("");
    uint8_t c[6] = { 0x04, 0, 0, 0, 0, 0 }, none[6] = { 0 };
    tek_report(&k, &t, 0, c, frame);                     // 'a' goes down
    for (int i = 1; i < TEK_REPEAT_DELAY; i++) tek_tick(&k, &t, frame + i);
    text_is("a");                                        // no repeat yet
    tek_tick(&k, &t, frame + TEK_REPEAT_DELAY);
    text_is("aa");
    tek_tick(&k, &t, frame + TEK_REPEAT_DELAY + TEK_REPEAT_RATE);
    text_is("aaa");
    tek_report(&k, &t, 0, none, frame + 50);             // released
    for (int i = 0; i < 100; i++) tek_tick(&k, &t, frame + 60 + i);
    text_is("aaa");
}

static void test_held_key_is_not_retyped_by_the_next_report(void) {
    // Pressing b while a is still held: only b is new.
    load("");
    uint8_t a[6] = { 0x04, 0, 0, 0, 0, 0 }, ab[6] = { 0x04, 0x05, 0, 0, 0, 0 };
    tek_report(&k, &t, 0, a, frame);
    tek_report(&k, &t, 0, ab, frame + 1);
    text_is("ab");
}

static void test_editor_words_fit_and_print(void) {
    const char *all[] = { TEK_HINT, TEK_MSG_SAVED, TEK_MSG_FAILED, TEK_MSG_WARN,
                          TEK_MSG_FULL, TEK_MSG_NEW, TEK_MSG_NOMEM,
                          TEK_MSG_APPLIED, TEK_MSG_NEXT, TEK_MSG_GAME };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(all[i]) <= CARD_COLS, all[i]);
        for (const char *p = all[i]; *p; p++)
            TEST_ASSERT_TRUE_MESSAGE(card_printable(*p), all[i]);
    }
}

static void all_comments_that_fit(const char *p) {
    int lines = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        TEST_ASSERT_EQUAL_CHAR('#', p[0]);
        TEST_ASSERT_TRUE(len <= CARD_COLS);
        lines++;
        if (!nl) break;
        p = nl + 1;
    }
    TEST_ASSERT_TRUE(lines > 5);
}

static void test_autorun_template_is_all_comments_that_fit(void) {
    // Saved unchanged, the template must do nothing at the next power-on:
    // every line a comment. And every line fits the screen.
    all_comments_that_fit(AUTORUN_TEMPLATE);
}

static void test_game_template_is_all_comments_that_fit(void) {
    all_comments_that_fit(GAME_TEMPLATE);             // PIZERO-154
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_load_drops_carriage_returns);
    RUN_TEST(test_typing_inserts_at_the_cursor);
    RUN_TEST(test_shift_gives_upper_case_and_symbols);
    RUN_TEST(test_enter_backspace_delete);
    RUN_TEST(test_up_down_keep_the_column);
    RUN_TEST(test_the_view_follows_the_cursor);
    RUN_TEST(test_a_full_buffer_refuses_and_says_so);
    RUN_TEST(test_ctrl_s_saves);
    RUN_TEST(test_esc_protects_unsaved_changes);
    RUN_TEST(test_a_held_key_repeats_and_stops);
    RUN_TEST(test_held_key_is_not_retyped_by_the_next_report);
    RUN_TEST(test_editor_words_fit_and_print);
    RUN_TEST(test_autorun_template_is_all_comments_that_fit);
    RUN_TEST(test_game_template_is_all_comments_that_fit);
    return UNITY_END();
}

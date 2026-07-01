/*
 * Native host test for Plover formatting engine.
 * Build: cc -I../src -o test_formatter test_formatter.c ../src/formatter.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "formatter.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ_STR(actual, expected, msg) do { \
    tests_run++; \
    if (strcmp((actual), (expected)) == 0) { \
        tests_passed++; \
    } else { \
        printf("FAIL [%s]: got \"%s\", expected \"%s\"\n", msg, actual, expected); \
    } \
} while (0)

#define ASSERT_EQ_INT(actual, expected, msg) do { \
    tests_run++; \
    if ((actual) == (expected)) { \
        tests_passed++; \
    } else { \
        printf("FAIL [%s]: got %d, expected %d\n", msg, (int)(actual), (int)(expected)); \
    } \
} while (0)

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if ((cond)) { \
        tests_passed++; \
    } else { \
        printf("FAIL [%s]\n", msg); \
    } \
} while (0)

/* Helper: process and return output text */
static struct steno_fmt_result proc(struct steno_fmt_state *s, const char *t)
{
    struct steno_fmt_result r;
    steno_fmt_process(s, t, &r);
    return r;
}

static void test_raw_text(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    /* First word: no space before */
    struct steno_fmt_result r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, "hello", "raw: first word");
    ASSERT_EQ_INT(r.len, 5, "raw: first word len");

    /* Second word: space before */
    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " world", "raw: second word with space");
    ASSERT_EQ_INT(r.len, 6, "raw: second word len");
}

static void test_attach(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "walk");
    struct steno_fmt_result r = proc(&s, "{^}");
    ASSERT_TRUE(r.is_command_only || r.len == 0, "attach: no output");

    r = proc(&s, "ing");
    ASSERT_EQ_STR(r.text, "ing", "attach: no space after {^}");
}

static void test_suffix_attach(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "walk");
    struct steno_fmt_result r = proc(&s, "{^ing}");
    ASSERT_EQ_STR(r.text, "ing", "suffix: attached");
    ASSERT_EQ_INT(r.len, 3, "suffix: len");
}

static void test_prefix_attach(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r = proc(&s, "{pre^}");
    ASSERT_EQ_STR(r.text, "pre", "prefix: text");

    r = proc(&s, "fix");
    ASSERT_EQ_STR(r.text, "fix", "prefix: next word attached");
}

static void test_capitalize_next(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{-|}");
    struct steno_fmt_result r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, "Hello", "cap_next: first word capitalized");

    /* Verify cap_next is one-shot */
    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " world", "cap_next: one-shot reset");
}

static void test_punctuation(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "hello");
    struct steno_fmt_result r = proc(&s, "{.}");
    ASSERT_EQ_STR(r.text, ".", "period: attached");

    /* Next word should be capitalized */
    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " World", "period: cap next");
}

static void test_comma(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "hello");
    struct steno_fmt_result r = proc(&s, "{,}");
    ASSERT_EQ_STR(r.text, ",", "comma: attached");

    /* Comma does NOT capitalize next */
    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " world", "comma: no cap next");
}

static void test_sentence_flow(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r;
    r = proc(&s, "I");
    ASSERT_EQ_STR(r.text, "I", "sentence: I");

    r = proc(&s, "{.}");
    ASSERT_EQ_STR(r.text, ".", "sentence: period");

    r = proc(&s, "the");
    ASSERT_EQ_STR(r.text, " The", "sentence: The after period");
}

static void test_mode_caps(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{MODE:CAPS}");
    struct steno_fmt_result r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, "HELLO", "mode_caps: uppercase");

    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " WORLD", "mode_caps: persists");

    proc(&s, "{MODE:RESET}");
    r = proc(&s, "test");
    ASSERT_EQ_STR(r.text, " test", "mode_reset: normal");
}

static void test_mode_title(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{MODE:TITLE}");
    struct steno_fmt_result r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, "Hello", "mode_title: capitalize");

    proc(&s, "{MODE:RESET}");
}

static void test_mode_lower(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{MODE:LOWER}");
    struct steno_fmt_result r = proc(&s, "HELLO");
    ASSERT_EQ_STR(r.text, "hello", "mode_lower: lowercase");

    proc(&s, "{MODE:RESET}");
}

static void test_fingerspelling(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r;
    r = proc(&s, "{&a}");
    ASSERT_EQ_STR(r.text, "a", "finger: a");

    r = proc(&s, "{&b}");
    ASSERT_EQ_STR(r.text, "b", "finger: b glued");

    r = proc(&s, "{&c}");
    ASSERT_EQ_STR(r.text, "c", "finger: c glued");

    /* Non-fingerspelling should get space */
    r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, " hello", "finger: break with space");
}

static void test_combined_commands(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "test");

    /* {^}{-|} — suppress space + capitalize */
    struct steno_fmt_result r = proc(&s, "{^}{-|}");
    ASSERT_TRUE(r.len == 0 || r.is_command_only, "combined: no text");

    r = proc(&s, "word");
    ASSERT_EQ_STR(r.text, "Word", "combined: attached + capitalized");
}

static void test_undo(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r = proc(&s, "{*}");
    ASSERT_TRUE(r.is_undo, "undo: flag set");
    ASSERT_TRUE(r.is_command_only, "undo: command only");
}

static void test_key_combo(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r = proc(&s, "{#Return}");
    ASSERT_TRUE(r.is_command_only, "key_combo: command only");
}

static void test_literal_braces(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r = proc(&s, "\\{");
    ASSERT_EQ_STR(r.text, "{", "literal: left brace");

    r = proc(&s, "\\}");
    ASSERT_EQ_STR(r.text, " }", "literal: right brace with space");
}

static void test_uppercase_next(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{<}");
    struct steno_fmt_result r = proc(&s, "hello");
    ASSERT_EQ_STR(r.text, "HELLO", "upper_next: entire word");

    /* One-shot */
    r = proc(&s, "world");
    ASSERT_EQ_STR(r.text, " world", "upper_next: one-shot");
}

static void test_lowercase_next(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    proc(&s, "{>}");
    struct steno_fmt_result r = proc(&s, "HELLO");
    ASSERT_EQ_STR(r.text, "hello", "lower_next: entire word");
}

static void test_empty_translation(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    struct steno_fmt_result r = proc(&s, "");
    ASSERT_TRUE(r.is_command_only, "empty: command only");
    ASSERT_EQ_INT(r.len, 0, "empty: no text");
}

static void test_prefix_then_suffix(void)
{
    struct steno_fmt_state s;
    steno_fmt_init(&s);

    /* {re^} then {^ed} */
    proc(&s, "{re^}");
    struct steno_fmt_result r = proc(&s, "{^ed}");
    ASSERT_EQ_STR(r.text, "ed", "pre+suf: attached");
}

int main(void)
{
    test_raw_text();
    test_attach();
    test_suffix_attach();
    test_prefix_attach();
    test_capitalize_next();
    test_punctuation();
    test_comma();
    test_sentence_flow();
    test_mode_caps();
    test_mode_title();
    test_mode_lower();
    test_fingerspelling();
    test_combined_commands();
    test_undo();
    test_key_combo();
    test_literal_braces();
    test_uppercase_next();
    test_lowercase_next();
    test_empty_translation();
    test_prefix_then_suffix();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    if (tests_passed == tests_run) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    return 1;
}

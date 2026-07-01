/*
 * Test undo ring buffer.
 * Build: cc -I../src -o test_undo test_undo.c ../src/undo.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "undo.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL [%s:%d]: %s\n", __func__, __LINE__, msg); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* 1. Init → count=0, pop returns NULL */
static void test_init(void)
{
    struct stroke_history h;
    steno_undo_init(&h);
    ASSERT(steno_undo_count(&h) == 0, "count should be 0 after init");
    ASSERT(steno_undo_pop(&h) == NULL, "pop on empty should return NULL");
    ASSERT(steno_undo_peek(&h) == NULL, "peek on empty should return NULL");
}

/* 2. Push one → count=1, peek returns it */
static void test_push_one(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    uint32_t strokes[] = {0xABCD};
    steno_undo_push(&h, strokes, 1, 5, 1, 0x0F);

    ASSERT(steno_undo_count(&h) == 1, "count should be 1");

    const struct stroke_history_entry *e = steno_undo_peek(&h);
    ASSERT(e != NULL, "peek should not be NULL");
    ASSERT(e->strokes[0] == 0xABCD, "stroke data mismatch");
    ASSERT(e->stroke_count == 1, "stroke_count mismatch");
    ASSERT(e->output_len == 5, "output_len mismatch");
    ASSERT(e->space_before == 1, "space_before mismatch");
    ASSERT(e->fmt_flags == 0x0F, "fmt_flags mismatch");
}

/* 3. Push and pop → entry matches */
static void test_push_pop(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    uint32_t strokes[] = {0x100, 0x200};
    steno_undo_push(&h, strokes, 2, 7, 0, 0x03);

    struct stroke_history_entry *e = steno_undo_pop(&h);
    ASSERT(e != NULL, "pop should return entry");
    ASSERT(e->strokes[0] == 0x100, "stroke[0] mismatch");
    ASSERT(e->strokes[1] == 0x200, "stroke[1] mismatch");
    ASSERT(e->stroke_count == 2, "stroke_count mismatch");
    ASSERT(e->output_len == 7, "output_len mismatch");
    ASSERT(e->space_before == 0, "space_before mismatch");
    ASSERT(e->fmt_flags == 0x03, "fmt_flags mismatch");
    ASSERT(steno_undo_count(&h) == 0, "count should be 0 after pop");
}

/* 4. Overflow: push SIZE+1 → count stays at SIZE */
static void test_overflow(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    for (int i = 0; i < CONFIG_STENO_HISTORY_SIZE + 1; i++) {
        uint32_t s = (uint32_t)i;
        steno_undo_push(&h, &s, 1, (uint8_t)(i & 0xFF), 0, 0);
    }

    ASSERT(steno_undo_count(&h) == CONFIG_STENO_HISTORY_SIZE,
           "count should cap at SIZE");

    /* Most recent should be SIZE (last pushed) */
    const struct stroke_history_entry *e = steno_undo_peek(&h);
    ASSERT(e != NULL, "peek should not be NULL");
    ASSERT(e->strokes[0] == (uint32_t)CONFIG_STENO_HISTORY_SIZE,
           "most recent entry should be last pushed");

    /* Oldest (entry 0) should be gone; entry 1 should be oldest */
    /* Pop all and check last one is entry 1 */
    struct stroke_history_entry *last = NULL;
    for (int i = 0; i < CONFIG_STENO_HISTORY_SIZE; i++) {
        last = steno_undo_pop(&h);
        ASSERT(last != NULL, "pop should succeed");
    }
    /* last popped = oldest = entry index 1 */
    ASSERT(last->strokes[0] == 1, "oldest entry should be index 1 (0 was overwritten)");
    ASSERT(steno_undo_count(&h) == 0, "count should be 0 after popping all");
}

/* 5. Pop all → count=0 */
static void test_pop_all(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    for (int i = 0; i < 10; i++) {
        uint32_t s = (uint32_t)i;
        steno_undo_push(&h, &s, 1, 1, 0, 0);
    }

    for (int i = 0; i < 10; i++) {
        ASSERT(steno_undo_pop(&h) != NULL, "pop should succeed");
    }
    ASSERT(steno_undo_count(&h) == 0, "count should be 0");
    ASSERT(steno_undo_pop(&h) == NULL, "pop on empty should be NULL");
}

/* 6. Push/pop cycle: push 5, pop 3, push 2, pop 4 → LIFO order */
static void test_push_pop_cycle(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    /* Push 5: values 10,11,12,13,14 */
    for (int i = 0; i < 5; i++) {
        uint32_t s = (uint32_t)(10 + i);
        steno_undo_push(&h, &s, 1, (uint8_t)(10 + i), 0, 0);
    }
    ASSERT(steno_undo_count(&h) == 5, "count should be 5");

    /* Pop 3: should get 14, 13, 12 */
    struct stroke_history_entry *e;
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 14, "should pop 14");
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 13, "should pop 13");
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 12, "should pop 12");
    ASSERT(steno_undo_count(&h) == 2, "count should be 2");

    /* Push 2: values 20, 21 */
    for (int i = 0; i < 2; i++) {
        uint32_t s = (uint32_t)(20 + i);
        steno_undo_push(&h, &s, 1, (uint8_t)(20 + i), 0, 0);
    }
    ASSERT(steno_undo_count(&h) == 4, "count should be 4");

    /* Pop 4: should get 21, 20, 11, 10 */
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 21, "should pop 21");
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 20, "should pop 20");
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 11, "should pop 11");
    e = steno_undo_pop(&h);
    ASSERT(e->strokes[0] == 10, "should pop 10");
    ASSERT(steno_undo_count(&h) == 0, "count should be 0");
}

/* 7. Verify multi-stroke data preserved */
static void test_data_preservation(void)
{
    struct stroke_history h;
    steno_undo_init(&h);

    uint32_t strokes[] = {0xDEAD, 0xBEEF, 0xCAFE};
    steno_undo_push(&h, strokes, 3, 42, 1, 0x7A);

    struct stroke_history_entry *e = steno_undo_pop(&h);
    ASSERT(e != NULL, "pop should return entry");
    ASSERT(e->stroke_count == 3, "stroke_count should be 3");
    ASSERT(e->strokes[0] == 0xDEAD, "strokes[0] mismatch");
    ASSERT(e->strokes[1] == 0xBEEF, "strokes[1] mismatch");
    ASSERT(e->strokes[2] == 0xCAFE, "strokes[2] mismatch");
    ASSERT(e->output_len == 42, "output_len mismatch");
    ASSERT(e->space_before == 1, "space_before mismatch");
    ASSERT(e->fmt_flags == 0x7A, "fmt_flags mismatch");
}

int main(void)
{
    printf("Running undo tests...\n\n");

    test_init();
    test_push_one();
    test_push_pop();
    test_overflow();
    test_pop_all();
    test_push_pop_cycle();
    test_data_preservation();

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}

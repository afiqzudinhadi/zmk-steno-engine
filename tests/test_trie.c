/*
 * Native test for trie.c — run on host, not on target.
 * Build: cc -I../src -o test_trie test_trie.c ../src/trie.c
 * Run:   ./test_trie /tmp/steno_test.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trie.h"

static uint8_t *load_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <steno_dict.bin>\n", argv[0]);
        return 1;
    }

    size_t len;
    uint8_t *data = load_file(argv[1], &len);
    if (!data) {
        printf("Failed to load %s\n", argv[1]);
        return 1;
    }

    int ret = steno_trie_init(data, len);
    CHECK(ret == 0, "trie_init succeeds");

    /* S → "is" (S- = bit 0 = 0x00000001) */
    uint32_t s_stroke = 0x00000001;
    const char *r = steno_trie_lookup(&s_stroke, 1);
    CHECK(r != NULL && strcmp(r, "is") == 0, "S → 'is'");

    /* T → "it" (T- = bit 1 = 0x00000002) */
    uint32_t t_stroke = 0x00000002;
    r = steno_trie_lookup(&t_stroke, 1);
    CHECK(r != NULL && strcmp(r, "it") == 0, "T → 'it'");

    /* -T → "the" (-T = bit 18 = 0x00040000) */
    uint32_t t_right = 0x00040000;
    r = steno_trie_lookup(&t_right, 1);
    CHECK(r != NULL && strcmp(r, "the") == 0, "-T → 'the'");

    /* TEFT → "test" (T- | -E | -F | -T = 0x02|0x400|0x1000|0x40000) */
    uint32_t teft = 0x00041402;
    r = steno_trie_lookup(&teft, 1);
    CHECK(r != NULL && strcmp(r, "test") == 0, "TEFT → 'test'");

    /* KO/PHAOURD → "computer" (multi-stroke) */
    uint32_t ko = 0x00000104;       /* K | O */
    uint32_t phaourd = 0x001029A8;  /* P | H | A | O | U | R | -D */
    uint32_t multi[2] = {ko, phaourd};
    r = steno_trie_lookup(multi, 2);
    CHECK(r != NULL && strcmp(r, "computer") == 0, "KO/PHAOURD → 'computer'");

    /* Non-existent stroke */
    uint32_t nonsense = 0x003FFFFF;
    r = steno_trie_lookup(&nonsense, 1);
    CHECK(r == NULL, "nonsense stroke → NULL");

    /* has_prefix: KO should be prefix of KO/PHAOURD */
    CHECK(steno_trie_has_prefix(&ko, 1) == true, "KO is prefix");

    /* has_prefix: S is NOT prefix of anything multi-stroke */
    CHECK(steno_trie_has_prefix(&s_stroke, 1) == false, "S is not prefix");

    /* NULL/zero args */
    CHECK(steno_trie_lookup(NULL, 1) == NULL, "NULL strokes → NULL");
    CHECK(steno_trie_lookup(&s_stroke, 0) == NULL, "0 count → NULL");
    CHECK(steno_trie_has_prefix(NULL, 1) == false, "NULL prefix → false");

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    free(data);
    return tests_passed == tests_run ? 0 : 1;
}

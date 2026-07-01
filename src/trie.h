#ifndef STENO_TRIE_H
#define STENO_TRIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STENO_DICT_MAGIC 0x4F4E5453 /* "STNO" */

struct steno_dict_header {
    uint32_t magic;
    uint16_t version;
    uint8_t max_strokes;
    uint8_t _pad;
    uint32_t entry_count;
    uint32_t strings_offset;
} __attribute__((packed));

int steno_trie_init(const uint8_t *data, size_t len);

const char *steno_trie_lookup(const uint32_t *strokes, uint8_t count);

bool steno_trie_has_prefix(const uint32_t *strokes, uint8_t count);

#endif

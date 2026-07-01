/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

#ifndef SPLIT_CACHE_H
#define SPLIT_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPLIT_CACHE_KEY_SIZE   24    /* max 8 strokes * 3 bytes */
#define SPLIT_CACHE_VALUE_SIZE 128   /* max translation length */

struct cache_entry {
    uint32_t key_hash;
    uint8_t stroke_count;
    uint32_t strokes[8];
    char translation[SPLIT_CACHE_VALUE_SIZE];
    bool has_prefix;
    bool valid;
    uint32_t access_count;
};

struct split_cache {
    struct cache_entry entries[CONFIG_STENO_SPLIT_CACHE_SIZE];
    uint32_t access_counter;
    uint32_t hits;
    uint32_t misses;
};

void split_cache_init(struct split_cache *cache);
bool split_cache_lookup(struct split_cache *cache, const uint32_t *strokes,
                        uint8_t count, char *result, size_t result_size,
                        bool *has_prefix);
void split_cache_insert(struct split_cache *cache, const uint32_t *strokes,
                        uint8_t count, const char *translation, bool has_prefix);
void split_cache_invalidate(struct split_cache *cache);

#endif /* SPLIT_CACHE_H */

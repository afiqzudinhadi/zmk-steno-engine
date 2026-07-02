/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

#include <string.h>
#include "split_cache.h"

/* FNV-1a hash over stroke bytes */
static uint32_t hash_strokes(const uint32_t *strokes, uint8_t count)
{
    uint32_t hash = 2166136261u; /* FNV offset basis */

    for (uint8_t i = 0; i < count; i++) {
        uint32_t s = strokes[i];
        for (int b = 0; b < 4; b++) {
            hash ^= (s & 0xFF);
            hash *= 16777619u; /* FNV prime */
            s >>= 8;
        }
    }

    return hash;
}

static bool strokes_match(const struct cache_entry *entry,
                          const uint32_t *strokes, uint8_t count)
{
    if (entry->stroke_count != count) {
        return false;
    }
    return memcmp(entry->strokes, strokes, count * sizeof(uint32_t)) == 0;
}

void split_cache_init(struct split_cache *cache)
{
    memset(cache->entries, 0,
           sizeof(struct cache_entry) * CONFIG_STENO_SPLIT_CACHE_SIZE);
    cache->access_counter = 0;
    cache->hits = 0;
    cache->misses = 0;
}

bool split_cache_lookup(struct split_cache *cache, const uint32_t *strokes,
                        uint8_t count, char *result, size_t result_size,
                        bool *has_prefix)
{
    uint32_t h = hash_strokes(strokes, count);

    for (int i = 0; i < CONFIG_STENO_SPLIT_CACHE_SIZE; i++) {
        struct cache_entry *e = &cache->entries[i];

        if (!e->valid) {
            continue;
        }

        if (e->key_hash == h && strokes_match(e, strokes, count)) {
            /* Hit */
            cache->access_counter++;
            e->access_count = cache->access_counter;
            cache->hits++;

            if (has_prefix) {
                *has_prefix = e->has_prefix;
            }

            if (result && result_size > 0) {
                size_t len = strlen(e->translation);
                if (len >= result_size) {
                    len = result_size - 1;
                }
                memcpy(result, e->translation, len);
                result[len] = '\0';
            }

            return true;
        }
    }

    cache->misses++;
    return false;
}

void split_cache_insert(struct split_cache *cache, const uint32_t *strokes,
                        uint8_t count, const char *translation, bool has_prefix)
{
    if (count == 0 || count > 8) {
        return;
    }

    uint32_t h = hash_strokes(strokes, count);

    /* Check if already present → update */
    for (int i = 0; i < CONFIG_STENO_SPLIT_CACHE_SIZE; i++) {
        struct cache_entry *e = &cache->entries[i];
        if (e->valid && e->key_hash == h && strokes_match(e, strokes, count)) {
            /* Update existing entry */
            if (translation) {
                size_t len = strlen(translation);
                if (len >= SPLIT_CACHE_VALUE_SIZE) {
                    len = SPLIT_CACHE_VALUE_SIZE - 1;
                }
                memcpy(e->translation, translation, len);
                e->translation[len] = '\0';
            }
            e->has_prefix = has_prefix;
            cache->access_counter++;
            e->access_count = cache->access_counter;
            return;
        }
    }

    /* Find empty slot or LRU victim */
    int target = -1;
    uint32_t min_access = UINT32_MAX;

    for (int i = 0; i < CONFIG_STENO_SPLIT_CACHE_SIZE; i++) {
        if (!cache->entries[i].valid) {
            target = i;
            break;
        }
        if (cache->entries[i].access_count < min_access) {
            min_access = cache->entries[i].access_count;
            target = i;
        }
    }

    if (target < 0) {
        target = 0; /* fallback: should never happen if cache size > 0 */
    }

    struct cache_entry *e = &cache->entries[target];
    e->key_hash = h;
    e->stroke_count = count;
    memcpy(e->strokes, strokes, count * sizeof(uint32_t));

    if (translation) {
        size_t len = strlen(translation);
        if (len >= SPLIT_CACHE_VALUE_SIZE) {
            len = SPLIT_CACHE_VALUE_SIZE - 1;
        }
        memcpy(e->translation, translation, len);
        e->translation[len] = '\0';
    } else {
        e->translation[0] = '\0';
    }

    e->has_prefix = has_prefix;
    e->valid = true;
    cache->access_counter++;
    e->access_count = cache->access_counter;
}

void split_cache_invalidate(struct split_cache *cache)
{
    for (int i = 0; i < CONFIG_STENO_SPLIT_CACHE_SIZE; i++) {
        cache->entries[i].valid = false;
    }
    cache->access_counter = 0;
    cache->hits = 0;
    cache->misses = 0;
}

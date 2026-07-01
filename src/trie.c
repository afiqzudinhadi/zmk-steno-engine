#include "trie.h"
#include <string.h>

static const uint8_t *dict_data;
static const struct steno_dict_header *dict_hdr;
static const uint8_t *entry_array;
static const char *string_table;
static size_t entry_stride;

int steno_trie_init(const uint8_t *data, size_t len)
{
    if (!data || len < sizeof(struct steno_dict_header)) {
        return -1;
    }

    dict_hdr = (const struct steno_dict_header *)data;

    if (dict_hdr->magic != STENO_DICT_MAGIC) {
        return -2;
    }
    if (dict_hdr->version != 1) {
        return -3;
    }
    if (dict_hdr->max_strokes == 0 || dict_hdr->max_strokes > 16) {
        return -4;
    }

    entry_stride = (size_t)dict_hdr->max_strokes * 4 + 4;
    size_t entries_end = sizeof(struct steno_dict_header) +
                         (size_t)dict_hdr->entry_count * entry_stride;

    if (entries_end > len || dict_hdr->strings_offset > len) {
        return -5;
    }

    dict_data = data;
    entry_array = data + sizeof(struct steno_dict_header);
    string_table = (const char *)(data + dict_hdr->strings_offset);

    return 0;
}

static const uint8_t *get_entry(uint32_t idx)
{
    return entry_array + (size_t)idx * entry_stride;
}

static int cmp_strokes(const uint32_t *a, uint8_t a_count,
                       const uint8_t *entry_bytes, uint8_t max_s)
{
    const uint32_t *b = (const uint32_t *)entry_bytes;

    uint8_t b_count = 0;
    for (uint8_t i = 0; i < max_s; i++) {
        if (b[i] != 0) {
            b_count = i + 1;
        }
    }

    uint8_t min_count = a_count < b_count ? a_count : b_count;
    for (uint8_t i = 0; i < min_count; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    if (a_count < b_count) return -1;
    if (a_count > b_count) return 1;
    return 0;
}

const char *steno_trie_lookup(const uint32_t *strokes, uint8_t count)
{
    if (!dict_hdr || !strokes || count == 0 || count > dict_hdr->max_strokes) {
        return NULL;
    }

    uint32_t lo = 0;
    uint32_t hi = dict_hdr->entry_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *entry = get_entry(mid);
        int c = cmp_strokes(strokes, count, entry, dict_hdr->max_strokes);
        if (c == 0) {
            uint32_t str_off;
            memcpy(&str_off, entry + (size_t)dict_hdr->max_strokes * 4, 4);
            return string_table + str_off;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return NULL;
}

bool steno_trie_has_prefix(const uint32_t *strokes, uint8_t count)
{
    if (!dict_hdr || !strokes || count == 0 || count >= dict_hdr->max_strokes) {
        return false;
    }

    uint32_t lo = 0;
    uint32_t hi = dict_hdr->entry_count;
    uint32_t first_ge = hi;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *entry = get_entry(mid);
        const uint32_t *e_strokes = (const uint32_t *)entry;

        int cmp = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (e_strokes[i] < strokes[i]) { cmp = -1; break; }
            if (e_strokes[i] > strokes[i]) { cmp = 1; break; }
        }

        if (cmp >= 0) {
            first_ge = mid;
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    for (uint32_t idx = first_ge; idx < dict_hdr->entry_count; idx++) {
        const uint8_t *entry = get_entry(idx);
        const uint32_t *e_strokes = (const uint32_t *)entry;

        bool prefix_match = true;
        for (uint8_t i = 0; i < count; i++) {
            if (e_strokes[i] != strokes[i]) {
                prefix_match = false;
                break;
            }
        }
        if (!prefix_match) {
            return false;
        }

        if (e_strokes[count] != 0) {
            return true;
        }
    }

    return false;
}

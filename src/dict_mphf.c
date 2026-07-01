/**
 * MPHF dictionary lookup engine — implementation.
 *
 * All data is read directly from flash. No heap allocation.
 * Bit-packed fields read via inline bit extraction.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "dict_mphf.h"
#include <string.h>

/* ─── FNV-1a 32-bit hash ─── */

static uint32_t fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

/**
 * Hash key bytes with a seed (prepend seed as LE u32).
 * Equivalent to: fnv1a_32(pack('<I', seed) + key_bytes)
 */
static uint32_t hash_key(const uint8_t *key, size_t key_len, uint32_t seed)
{
    uint32_t h = 0x811c9dc5u;

    /* Hash the seed bytes first (little-endian u32) */
    uint8_t seed_bytes[4];
    seed_bytes[0] = (uint8_t)(seed);
    seed_bytes[1] = (uint8_t)(seed >> 8);
    seed_bytes[2] = (uint8_t)(seed >> 16);
    seed_bytes[3] = (uint8_t)(seed >> 24);

    for (int i = 0; i < 4; i++) {
        h ^= seed_bytes[i];
        h *= 0x01000193u;
    }

    /* Then hash the key bytes */
    for (size_t i = 0; i < key_len; i++) {
        h ^= key[i];
        h *= 0x01000193u;
    }

    return h;
}

/* ─── Bit-packed field reading ─── */

/**
 * Read n_bits from a bit-packed array starting at bit position bit_pos.
 * Bits are packed LSB-first within each byte.
 */
static uint32_t read_bits(const uint8_t *data, uint32_t bit_pos, uint8_t n_bits)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < n_bits; i++) {
        uint32_t byte_idx = (bit_pos + i) / 8;
        uint8_t  bit_idx  = (bit_pos + i) % 8;
        if (data[byte_idx] & (1u << bit_idx)) {
            value |= (1u << i);
        }
    }
    return value;
}

/* ─── Alignment helper ─── */

static inline uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/* ─── Init ─── */

int dict_mphf_init(struct dict_mphf *dict, const void *data, size_t len)
{
    if (!dict || !data) {
        return -1;
    }

    if (len < sizeof(struct dict_mphf_header)) {
        return -2;
    }

    const struct dict_mphf_header *hdr = (const struct dict_mphf_header *)data;

    if (hdr->magic != DICT_MPHF_MAGIC) {
        return -3;
    }

    if (hdr->version != DICT_MPHF_VERSION) {
        return -4;
    }

    dict->header = hdr;

    const uint8_t *base = (const uint8_t *)data;
    uint32_t offset = sizeof(struct dict_mphf_header);

    /* Displacements section */
    dict->displacements = base + offset;
    uint32_t disp_bits_total = (uint32_t)hdr->bucket_count * hdr->disp_bits;
    dict->disp_section_len = align4((disp_bits_total + 7) / 8);
    offset += dict->disp_section_len;

    /* Values section */
    dict->values = base + offset;
    uint32_t val_bits_total = (uint32_t)hdr->entry_count * hdr->value_bits;
    dict->val_section_len = align4((val_bits_total + 7) / 8);
    offset += dict->val_section_len;

    /* Fingerprints section */
    dict->fingerprints = base + offset;
    dict->fp_section_len = align4(hdr->entry_count);
    offset += dict->fp_section_len;

    /* String offsets section (u24 LE, 3 bytes each) */
    dict->string_offsets = base + offset;
    offset += hdr->unique_count * 3;

    /* String data section */
    dict->string_data = (const char *)(base + offset);

    /*
     * To find string_data length, scan for end of last string.
     * But we don't strictly need it for lookups — strings are
     * null-terminated and we just follow offsets.
     *
     * For prefix_table, we need to know where string_data ends.
     * Compute from total file size minus prefix table size.
     */

    /* Prefix table is at the end of the file */
    uint32_t prefix_bytes = (uint32_t)hdr->prefix_count * 4;
    if (len >= prefix_bytes) {
        dict->prefix_table = (const uint32_t *)(base + len - prefix_bytes);
    } else {
        dict->prefix_table = NULL;
    }

    return 0;
}

/* ─── Lookup ─── */

const char *dict_mphf_lookup(const struct dict_mphf *dict,
                             const uint32_t *strokes, uint8_t count)
{
    if (!dict || !dict->header || !strokes || count == 0) {
        return NULL;
    }

    const struct dict_mphf_header *hdr = dict->header;

    /* Build key bytes: each stroke as LE u32, concatenated */
    uint8_t key_buf[32];  /* max 8 strokes × 4 bytes */
    size_t key_len = (size_t)count * 4;
    if (key_len > sizeof(key_buf)) {
        return NULL;
    }

    for (uint8_t i = 0; i < count; i++) {
        key_buf[i * 4 + 0] = (uint8_t)(strokes[i]);
        key_buf[i * 4 + 1] = (uint8_t)(strokes[i] >> 8);
        key_buf[i * 4 + 2] = (uint8_t)(strokes[i] >> 16);
        key_buf[i * 4 + 3] = (uint8_t)(strokes[i] >> 24);
    }

    /* MPHF lookup */
    uint32_t bucket = hash_key(key_buf, key_len, 0) % hdr->bucket_count;
    uint32_t d = read_bits(dict->displacements,
                           bucket * (uint32_t)hdr->disp_bits,
                           hdr->disp_bits);
    uint32_t slot = hash_key(key_buf, key_len, d + 1) % hdr->entry_count;

    /* Fingerprint check */
    uint8_t expected_fp = (uint8_t)(fnv1a_32(key_buf, key_len) & 0xFF);
    if (dict->fingerprints[slot] != expected_fp) {
        return NULL;
    }

    /* Read value ID from bit-packed array */
    uint32_t val_id = read_bits(dict->values,
                                slot * (uint32_t)hdr->value_bits,
                                hdr->value_bits);
    if (val_id >= hdr->unique_count) {
        return NULL;
    }

    /* Resolve string (u24 LE offset) */
    const uint8_t *off_ptr = dict->string_offsets + val_id * 3;
    uint32_t str_offset = (uint32_t)off_ptr[0]
                        | ((uint32_t)off_ptr[1] << 8)
                        | ((uint32_t)off_ptr[2] << 16);
    return dict->string_data + str_offset;
}

/* ─── has_prefix ─── */

bool dict_mphf_has_prefix(const struct dict_mphf *dict, uint32_t stroke)
{
    if (!dict || !dict->header || !dict->prefix_table ||
        dict->header->prefix_count == 0) {
        return false;
    }

    /* Binary search in sorted prefix table */
    uint32_t lo = 0;
    uint32_t hi = dict->header->prefix_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t val = dict->prefix_table[mid];
        if (val == stroke) {
            return true;
        } else if (val < stroke) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return false;
}

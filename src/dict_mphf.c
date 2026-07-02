/**
 * MPHF dictionary lookup engine — implementation.
 *
 * All data is read directly from flash. No heap allocation.
 * String table is block-compressed with zlib; decompressed on-demand
 * into a static 4KB buffer.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "dict_mphf.h"
#include <string.h>

#ifdef __ZEPHYR__
#include <zephyr/sys/crc.h>
#endif

/* ─── Minimal inflate for non-Zephyr (native tests) ─── */

#ifndef __ZEPHYR__
#include <stdlib.h>

/* Use zlib on host for native tests */
#ifdef HAS_ZLIB
#include <zlib.h>
static int block_inflate(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    uLongf out_len = dst_cap;
    int ret = uncompress(dst, &out_len, src, src_len);
    if (ret == Z_OK) {
        *dst_len = out_len;
        return 0;
    }
    return -1;
}
#else
/* Minimal tinf inflate — bundled for host-only testing.
 * On Zephyr, we use the kernel's built-in zlib. */
#include "tinf/tinf.h"
static int block_inflate(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    unsigned int out_len = dst_cap;
    /* Skip 2-byte zlib header, strip 4-byte adler32 checksum */
    if (src_len < 6) return -1;
    int ret = tinf_uncompress(dst, &out_len, src + 2, src_len - 6);
    if (ret == 0) {
        *dst_len = out_len;
        return 0;
    }
    return -1;
}
#endif /* HAS_ZLIB */

#else /* __ZEPHYR__ */

#include <zephyr/sys/util.h>

/* Zephyr built-in zlib decompression */
#if __has_include(<zephyr/lib/zlib/zlib.h>)
#include <zephyr/lib/zlib/zlib.h>
#elif __has_include(<zlib.h>)
#include <zlib.h>
#endif

static int block_inflate(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    /* Try Zephyr's tinycrypt/miniz or fall back to raw copy */
#if defined(CONFIG_ZLIB)
    uLongf out_len = dst_cap;
    int ret = uncompress(dst, &out_len, src, src_len);
    if (ret == Z_OK) {
        *dst_len = out_len;
        return 0;
    }
    return -1;
#else
    /* No zlib available — strings must be uncompressed (flags bit 0 clear) */
    (void)src; (void)src_len; (void)dst; (void)dst_cap; (void)dst_len;
    return -1;
#endif
}

#endif /* __ZEPHYR__ */


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

static uint32_t hash_key(const uint8_t *key, size_t key_len, uint32_t seed)
{
    uint32_t h = 0x811c9dc5u;
    uint8_t seed_bytes[4];
    seed_bytes[0] = (uint8_t)(seed);
    seed_bytes[1] = (uint8_t)(seed >> 8);
    seed_bytes[2] = (uint8_t)(seed >> 16);
    seed_bytes[3] = (uint8_t)(seed >> 24);
    for (int i = 0; i < 4; i++) {
        h ^= seed_bytes[i];
        h *= 0x01000193u;
    }
    for (size_t i = 0; i < key_len; i++) {
        h ^= key[i];
        h *= 0x01000193u;
    }
    return h;
}

/* ─── Bit-packed field reading ─── */

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

static inline uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/* ─── Init ─── */

int dict_mphf_init(struct dict_mphf *dict, const void *data, size_t len)
{
    if (!dict || !data) return -1;
    if (len < sizeof(struct dict_mphf_header)) return -2;

    const struct dict_mphf_header *hdr = (const struct dict_mphf_header *)data;
    if (hdr->magic != DICT_MPHF_MAGIC) return -3;
    if (hdr->version != DICT_MPHF_VERSION) return -4;

    dict->header = hdr;
    const uint8_t *base = (const uint8_t *)data;
    uint32_t offset = sizeof(struct dict_mphf_header);

    /* Displacements */
    dict->displacements = base + offset;
    uint32_t disp_bits_total = (uint32_t)hdr->bucket_count * hdr->disp_bits;
    dict->disp_section_len = align4((disp_bits_total + 7) / 8);
    offset += dict->disp_section_len;

    /* Values */
    dict->values = base + offset;
    uint32_t val_bits_total = (uint32_t)hdr->entry_count * hdr->value_bits;
    dict->val_section_len = align4((val_bits_total + 7) / 8);
    offset += dict->val_section_len;

    /* Fingerprints */
    dict->fingerprints = base + offset;
    dict->fp_section_len = align4(hdr->entry_count);
    offset += dict->fp_section_len;

    /* String offsets (u24 LE, 3 bytes each) */
    dict->string_offsets = base + offset;
    offset += hdr->unique_count * 3;

    /* String data section */
    dict->str_data_start = base + offset;

    if (hdr->flags & DICT_MPHF_FLAG_COMPRESSED) {
        /* Block directory: u16 block_count + u32[] offsets */
        dict->block_count = dict->str_data_start[0] |
                           ((uint16_t)dict->str_data_start[1] << 8);
        dict->block_dir = (const uint32_t *)(dict->str_data_start + 2);
        dict->blocks_start = dict->str_data_start + 2 + dict->block_count * 4;
    } else {
        dict->block_count = 0;
        dict->block_dir = NULL;
        dict->blocks_start = dict->str_data_start;
    }

    /* Block size from header (0 = legacy default 4096) */
    dict->blk_size = hdr->block_size ? hdr->block_size : DICT_MPHF_BLOCK_SIZE;

    /* Prefix table at end */
    uint32_t prefix_bytes = (uint32_t)hdr->prefix_count * 4;
    if (len >= prefix_bytes) {
        dict->prefix_table = (const uint32_t *)(base + len - prefix_bytes);
    } else {
        dict->prefix_table = NULL;
    }

    return 0;
}

/* ─── String decompression ─── */

static const char *resolve_string(const struct dict_mphf *dict, uint32_t val_id)
{
    const uint8_t *off_ptr = dict->string_offsets + val_id * 3;
    uint32_t str_offset = (uint32_t)off_ptr[0]
                        | ((uint32_t)off_ptr[1] << 8)
                        | ((uint32_t)off_ptr[2] << 16);

    if (!(dict->header->flags & DICT_MPHF_FLAG_COMPRESSED)) {
        return (const char *)(dict->str_data_start + str_offset);
    }

    /* Block-compressed: decompress the right block */
    uint32_t block_idx = str_offset / dict->blk_size;
    uint32_t in_block_off = str_offset % dict->blk_size;

    if (block_idx >= dict->block_count) {
        return NULL;
    }

    /* Get compressed block bounds */
    uint32_t blk_start = dict->block_dir[block_idx];
    uint32_t blk_end;
    if (block_idx + 1 < dict->block_count) {
        blk_end = dict->block_dir[block_idx + 1];
    } else {
        /* Last block: extends to prefix_table or end of file */
        blk_end = (const uint8_t *)dict->prefix_table - dict->blocks_start;
    }

    const uint8_t *compressed = dict->blocks_start + blk_start;
    uint32_t compressed_len = blk_end - blk_start;

    static uint8_t decomp_buf[DICT_MPHF_BLOCK_SIZE];
    static uint32_t cached_block = UINT32_MAX;
    static size_t cached_len;

    if (cached_block != block_idx) {
        size_t out_len;
        if (block_inflate(compressed, compressed_len,
                          decomp_buf, sizeof(decomp_buf), &out_len) != 0) {
            return NULL;
        }
        cached_block = block_idx;
        cached_len = out_len;
    }

    if (in_block_off >= cached_len) {
        return NULL;
    }

    return (const char *)(decomp_buf + in_block_off);
}

/* ─── Lookup ─── */

const char *dict_mphf_lookup(const struct dict_mphf *dict,
                             const uint32_t *strokes, uint8_t count)
{
    if (!dict || !dict->header || !strokes || count == 0) {
        return NULL;
    }

    const struct dict_mphf_header *hdr = dict->header;

    uint8_t key_buf[32];
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

    uint32_t bucket = hash_key(key_buf, key_len, 0) % hdr->bucket_count;
    uint32_t d = read_bits(dict->displacements,
                           bucket * (uint32_t)hdr->disp_bits,
                           hdr->disp_bits);
    uint32_t slot = hash_key(key_buf, key_len, d + 1) % hdr->entry_count;

    uint8_t expected_fp = (uint8_t)(fnv1a_32(key_buf, key_len) & 0xFF);
    if (dict->fingerprints[slot] != expected_fp) {
        return NULL;
    }

    uint32_t val_id = read_bits(dict->values,
                                slot * (uint32_t)hdr->value_bits,
                                hdr->value_bits);
    if (val_id >= hdr->unique_count) {
        return NULL;
    }

    return resolve_string(dict, val_id);
}

/* ─── has_prefix ─── */

bool dict_mphf_has_prefix(const struct dict_mphf *dict, uint32_t stroke)
{
    if (!dict || !dict->header || !dict->prefix_table ||
        dict->header->prefix_count == 0) {
        return false;
    }

    uint32_t lo = 0;
    uint32_t hi = dict->header->prefix_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t val = dict->prefix_table[mid];
        if (val == stroke) return true;
        if (val < stroke) lo = mid + 1;
        else hi = mid;
    }

    return false;
}

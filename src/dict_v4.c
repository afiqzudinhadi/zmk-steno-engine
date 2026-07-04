/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

/*
 * Dictionary binary format v4 decoder — implementation.
 *
 * All data is read directly from flash. No heap allocation on the
 * Zephyr path; string blocks are inflated on demand into a static
 * two-slot LRU cache (16 KB per slot, the compile-time block bound).
 */

#include "dict_v4.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define DICT_V4_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ─── Raw-deflate block inflate (wbits = -15) ─── */

#include "inflate.h"

static int block_inflate_raw(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    int ret = steno_inflate(src, src_len, dst, dst_cap);
    if (ret < 0) {
        return -1;
    }
    *dst_len = (size_t)ret;
    return 0;
}

/* ─── Byte helpers (blob fields may be unaligned) ─── */

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ─── FNV-1a 32-bit hash (identical to v2) ─── */

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

/* ─── LSB-first bit-packed field reading (MEMBERSHIP/VALIDX/FP) ─── */

static uint32_t read_bits_lsb(const uint8_t *data, uint32_t bit_pos, uint8_t n_bits)
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

/* ─── MSB-first bit reading (DISP Huffman stream only) ─── */

static inline uint32_t read_bit_msb(const uint8_t *data, uint32_t bit_pos)
{
    return (data[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1u;
}

/* ─── Canonical Huffman (DISP displacement classes) ─── */

static int huff_build(struct dict_v4 *d)
{
    const uint8_t *code_len = d->disp_code_len;
    uint8_t max_len = 0;

    memset(d->huff_count, 0, sizeof(d->huff_count));

    for (int c = 0; c < DICT_V4_DISP_CLASSES; c++) {
        uint8_t l = code_len[c];
        if (l == 0) {
            continue;
        }
        if (l > DICT_V4_DISP_CLASSES) {
            return -EBADMSG;
        }
        d->huff_count[l]++;
        if (l > max_len) {
            max_len = l;
        }
    }
    if (max_len == 0) {
        return -EBADMSG;
    }

    /* Symbol list in canonical (code_len, class) order */
    uint8_t idx = 0;
    for (uint8_t l = 1; l <= max_len; l++) {
        d->huff_sym_base[l] = idx;
        for (uint8_t c = 0; c < DICT_V4_DISP_CLASSES; c++) {
            if (code_len[c] == l) {
                d->huff_symbols[idx++] = c;
            }
        }
    }

    /* First canonical code per length; codes assigned numerically
     * increasing, MSB-first prefix codes. */
    uint32_t code = 0;
    for (uint8_t l = 1; l <= max_len; l++) {
        d->huff_first_code[l] = code;
        code += d->huff_count[l];
        if (code > (1u << l)) {
            return -EBADMSG;    /* over-subscribed code space */
        }
        code <<= 1;
    }

    d->huff_max_len = max_len;
    return 0;
}

/* Decode one displacement value at *bit_pos, advancing it. */
static int huff_decode_one(const struct dict_v4 *d, uint32_t *bit_pos, uint32_t *v_out)
{
    uint32_t pos = *bit_pos;
    uint32_t code = 0;
    uint8_t sym = 0;
    uint8_t found = 0;

    for (uint8_t len = 1; len <= d->huff_max_len; len++) {
        if (pos >= d->disp_stream_bits) {
            return -EBADMSG;
        }
        code = (code << 1) | read_bit_msb(d->disp_stream, pos++);
        if (d->huff_count[len] != 0 &&
            code >= d->huff_first_code[len] &&
            code - d->huff_first_code[len] < d->huff_count[len]) {
            sym = d->huff_symbols[d->huff_sym_base[len] +
                                  (code - d->huff_first_code[len])];
            found = 1;
            break;
        }
    }
    if (!found) {
        return -EBADMSG;
    }

    uint32_t v;
    if (sym <= 1) {
        v = sym;
    } else {
        uint32_t extra = 0;
        for (uint8_t i = 0; i < sym - 1; i++) {
            if (pos >= d->disp_stream_bits) {
                return -EBADMSG;
            }
            extra = (extra << 1) | read_bit_msb(d->disp_stream, pos++);
        }
        v = (1u << (sym - 1)) | extra;
    }

    *bit_pos = pos;
    *v_out = v;
    return 0;
}

/* Random access: start at skip[b >> 8], decode (b & 255) values, next
 * value is bucket b's displacement. */
static int disp_decode(const struct dict_v4 *d, uint32_t bucket, uint32_t *v_out)
{
    uint32_t skip_idx = bucket >> 8;

    if (skip_idx >= d->disp_skip_count) {
        return -EBADMSG;
    }

    uint32_t bit_pos = get_le32(d->disp_skip + skip_idx * 4);
    uint32_t v = 0;

    for (uint32_t i = 0; i <= (bucket & 255u); i++) {
        int ret = huff_decode_one(d, &bit_pos, &v);
        if (ret != 0) {
            return ret;
        }
    }

    *v_out = v;
    return 0;
}

/* ─── CONFLICTS: binary search on sorted 5-byte records ─── */

static int conflict_lookup(const struct dict_v4 *d, uint32_t slot, uint32_t *string_id)
{
    uint32_t lo = 0;
    uint32_t hi = d->conflict_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *rec = d->conflicts + mid * DICT_V4_CONFLICT_REC_SIZE;
        uint64_t v = (uint64_t)rec[0]
                   | ((uint64_t)rec[1] << 8)
                   | ((uint64_t)rec[2] << 16)
                   | ((uint64_t)rec[3] << 24)
                   | ((uint64_t)rec[4] << 32);
        uint32_t rec_slot = (uint32_t)(v & 0x3FFFFu);

        if (rec_slot == slot) {
            *string_id = (uint32_t)(v >> 18);
            return 0;
        }
        if (rec_slot < slot) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -ENOENT;
}

/* ─── Init ─── */

static int init_disp(struct dict_v4 *d, const uint8_t *sec, uint32_t sec_len)
{
    uint32_t fixed = DICT_V4_DISP_CLASSES + 4;

    if (sec_len < fixed) {
        return -EBADMSG;
    }

    uint32_t skip_count = get_le32(sec + DICT_V4_DISP_CLASSES);
    uint32_t expected = (d->header->bucket_count + 255u) / 256u;

    if (skip_count != expected || sec_len < fixed + skip_count * 4) {
        return -EBADMSG;
    }

    d->disp_code_len = sec;
    d->disp_skip = sec + fixed;
    d->disp_skip_count = skip_count;
    d->disp_stream = sec + fixed + skip_count * 4;
    d->disp_stream_bits = (sec_len - fixed - skip_count * 4) * 8;

    return huff_build(d);
}

int dict_v4_init(struct dict_v4 *d, const void *blob, size_t len)
{
    if (!d || !blob) {
        return -EINVAL;
    }

    memset(d, 0, sizeof(*d));

    if (len < sizeof(struct dict_v4_header)) {
        return -EBADMSG;
    }

    const struct dict_v4_header *hdr = (const struct dict_v4_header *)blob;
    if (hdr->magic != DICT_V4_MAGIC || hdr->version != DICT_V4_VERSION) {
        return -EBADMSG;
    }

    const uint8_t *base = (const uint8_t *)blob;
    uint32_t sc = hdr->section_count;
    uint32_t dir_end = sizeof(struct dict_v4_header) +
                       sc * sizeof(struct dict_v4_section);

    if (len < dir_end) {
        return -EBADMSG;
    }

    d->header = hdr;

    for (uint32_t i = 0; i < sc; i++) {
        const struct dict_v4_section *ent = (const struct dict_v4_section *)
            (base + sizeof(struct dict_v4_header) + i * sizeof(*ent));
        const uint8_t *sec = base + ent->offset;
        uint32_t sec_len = ent->len;
        int ret;

        if (ent->offset < dir_end || (ent->offset & 3u) != 0 ||
            ent->offset > len || sec_len > len - ent->offset) {
            return -EBADMSG;
        }

        switch (ent->type) {
        case DICT_V4_SEC_DISP:
            ret = init_disp(d, sec, sec_len);
            if (ret != 0) {
                return ret;
            }
            break;

        case DICT_V4_SEC_MEMBERSHIP:
            if ((uint64_t)sec_len * 8 < (uint64_t)hdr->n * 2) {
                return -EBADMSG;
            }
            d->membership = sec;
            break;

        case DICT_V4_SEC_VALIDX: {
            uint32_t count = (uint32_t)(((uint64_t)sec_len * 8) /
                                        DICT_V4_VALIDX_BITS);
            if (ent->param > hdr->n || count > hdr->n - ent->param) {
                return -EBADMSG;
            }
            d->validx = sec;
            d->validx_start_slot = ent->param;
            d->validx_slot_count = count;
            break;
        }

        case DICT_V4_SEC_CONFLICTS:
            if (ent->param != hdr->conflict_count ||
                sec_len < ent->param * DICT_V4_CONFLICT_REC_SIZE) {
                return -EBADMSG;
            }
            d->conflicts = sec;
            d->conflict_count = ent->param;
            break;

        case DICT_V4_SEC_FP:
            if ((uint64_t)sec_len * 8 < (uint64_t)hdr->n * hdr->fp_bits) {
                return -EBADMSG;
            }
            d->fp = sec;
            break;

        case DICT_V4_SEC_STRDIR: {
            if (sec_len < 4) {
                return -EBADMSG;
            }
            uint32_t block_count = get_le32(sec);
            if (block_count != ent->param ||
                sec_len < 4 + (uint64_t)block_count * 8) {
                return -EBADMSG;
            }
            d->strdir_entries = sec + 4;
            d->strdir_block_count = block_count;
            break;
        }

        case DICT_V4_SEC_STRINGS:
            d->strings = sec;
            d->strings_len = sec_len;
            break;

        default:
            /* Unknown section: ignore for forward compatibility */
            break;
        }
    }

    return 0;
}

/* ─── Lookup (decision path, left half) ─── */

int dict_v4_lookup(const struct dict_v4 *d, const uint32_t *strokes, uint8_t count,
                   uint8_t active_dict, uint32_t *slot, uint32_t *string_id)
{
    if (!d || !d->header || !strokes || count == 0 || !slot || active_dict > 1) {
        return -EINVAL;
    }
    if (!d->disp_code_len || !d->membership || !d->fp || !d->validx) {
        return -ENOTSUP;    /* this half lacks the decision sections */
    }

    const struct dict_v4_header *hdr = d->header;

    if (!(hdr->dicts_mask & (1u << active_dict))) {
        return DICT_V4_MISS;
    }
    if (count > hdr->max_entry_strokes || count > DICT_V4_MAX_KEY_STROKES) {
        return DICT_V4_MISS;
    }

    uint8_t key_buf[DICT_V4_MAX_KEY_STROKES * 4];
    size_t key_len = (size_t)count * 4;

    for (uint8_t i = 0; i < count; i++) {
        key_buf[i * 4 + 0] = (uint8_t)(strokes[i]);
        key_buf[i * 4 + 1] = (uint8_t)(strokes[i] >> 8);
        key_buf[i * 4 + 2] = (uint8_t)(strokes[i] >> 16);
        key_buf[i * 4 + 3] = (uint8_t)(strokes[i] >> 24);
    }

    uint32_t bucket = hash_key(key_buf, key_len, 0) % hdr->bucket_count;
    uint32_t v;
    int ret = disp_decode(d, bucket, &v);
    if (ret != 0) {
        return ret;
    }

    uint32_t s;
    if (v >= hdr->d_threshold) {
        s = v - hdr->d_threshold;   /* direct slot index */
        if (s >= hdr->n) {
            return -EBADMSG;
        }
    } else {
        s = hash_key(key_buf, key_len, v + 1) % hdr->n;
    }

    uint8_t expected_fp = (uint8_t)(fnv1a_32(key_buf, key_len) &
                                    ((1u << hdr->fp_bits) - 1u));
    if (read_bits_lsb(d->fp, s * hdr->fp_bits, hdr->fp_bits) != expected_fp) {
        return DICT_V4_MISS;
    }

    uint32_t m = read_bits_lsb(d->membership, s * 2, 2);
    if (!(m & (1u << active_dict))) {
        return DICT_V4_MISS;
    }

    *slot = s;

    if (s < d->validx_start_slot ||
        s - d->validx_start_slot >= d->validx_slot_count) {
        return DICT_V4_FOUND_REMOTE;
    }

    uint32_t sid = read_bits_lsb(d->validx,
                                 (s - d->validx_start_slot) * DICT_V4_VALIDX_BITS,
                                 DICT_V4_VALIDX_BITS);

    /* Main value index holds plover's string id for both-membership
     * conflicts; substitute lapwing's from the conflict table. */
    if (active_dict == DICT_V4_DICT_LAPWING && m == 3 && d->conflicts) {
        uint32_t conflict_sid;
        if (conflict_lookup(d, s, &conflict_sid) == 0) {
            sid = conflict_sid;
        }
    }

    if (sid >= hdr->string_count) {
        return -EBADMSG;
    }
    if (string_id) {
        *string_id = sid;
    }
    return DICT_V4_FOUND_LOCAL;
}

/* ─── String path (right half or host test) ─── */

/* Static two-slot LRU block cache. 16 KB per slot: the compiler caps
 * each front-coded block's uncompressed size at 16384 bytes. */
struct dict_v4_block_slot {
    const struct dict_v4 *owner;
    uint32_t block_idx;
    uint32_t len;
    uint32_t tick;
    uint8_t  buf[DICT_V4_BLOCK_BUF_SIZE];
};

static struct dict_v4_block_slot block_cache[2];
static uint32_t block_cache_tick;

static uint32_t strdir_comp_off(const struct dict_v4 *d, uint32_t block)
{
    return get_le32(d->strdir_entries + block * 8);
}

static uint32_t strdir_first_sid(const struct dict_v4 *d, uint32_t block)
{
    return get_le32(d->strdir_entries + block * 8 + 4);
}

static int block_get(const struct dict_v4 *d, uint32_t block,
                     const uint8_t **buf, uint32_t *buf_len)
{
    struct dict_v4_block_slot *victim = &block_cache[0];

    for (size_t i = 0; i < DICT_V4_ARRAY_LEN(block_cache); i++) {
        struct dict_v4_block_slot *slot = &block_cache[i];

        if (slot->owner == d && slot->block_idx == block) {
            slot->tick = ++block_cache_tick;
            *buf = slot->buf;
            *buf_len = slot->len;
            return 0;
        }
        if (slot->tick < victim->tick) {
            victim = slot;
        }
    }

    uint32_t comp_off = strdir_comp_off(d, block);
    uint32_t comp_end = (block + 1 < d->strdir_block_count)
                      ? strdir_comp_off(d, block + 1)
                      : d->strings_len;

    if (comp_end < comp_off || comp_end > d->strings_len) {
        return -EBADMSG;
    }

    size_t out_len;
    victim->owner = NULL;
    if (block_inflate_raw(d->strings + comp_off, comp_end - comp_off,
                          victim->buf, sizeof(victim->buf), &out_len) != 0) {
        return -EIO;
    }

    victim->owner = d;
    victim->block_idx = block;
    victim->len = (uint32_t)out_len;
    victim->tick = ++block_cache_tick;

    *buf = victim->buf;
    *buf_len = victim->len;
    return 0;
}

int dict_v4_string_by_id_ctx(const struct dict_v4 *d, uint32_t string_id,
                             char *out, size_t out_size)
{
    if (!d || !d->header || !out || out_size == 0) {
        return -EINVAL;
    }
    if (!d->strdir_entries || !d->strings) {
        return -ENOTSUP;    /* this half lacks the string sections */
    }
    if (string_id >= d->header->string_count || d->strdir_block_count == 0) {
        return -ENOENT;
    }

    /* Binary search: last block with first_string_id <= string_id */
    uint32_t lo = 0;
    uint32_t hi = d->strdir_block_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strdir_first_sid(d, mid) <= string_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return -EBADMSG;
    }
    uint32_t block = lo - 1;

    const uint8_t *buf;
    uint32_t buf_len;
    int ret = block_get(d, block, &buf, &buf_len);
    if (ret != 0) {
        return ret;
    }

    /* Walk the front-coded chain, maintaining the previous string
     * in place: the shared prefix bytes are already correct. */
    static uint8_t prev[DICT_V4_MAX_TEXT + 1];
    uint32_t steps = string_id - strdir_first_sid(d, block);
    uint32_t pos = 0;
    uint32_t prev_len = 0;

    for (uint32_t i = 0; i <= steps; i++) {
        if (pos >= buf_len) {
            return -EBADMSG;
        }
        uint32_t prefix = buf[pos++];
        if (prefix > prev_len || (i == 0 && prefix != 0)) {
            return -EBADMSG;
        }
        uint32_t cur_len = prefix;
        while (pos < buf_len && buf[pos] != 0) {
            if (cur_len >= DICT_V4_MAX_TEXT) {
                return -EBADMSG;
            }
            prev[cur_len++] = buf[pos++];
        }
        if (pos >= buf_len) {
            return -EBADMSG;    /* missing terminator */
        }
        pos++;                  /* skip 0x00 */
        prev_len = cur_len;
    }

    if (prev_len + 1 > out_size) {
        return -ENOSPC;
    }
    memcpy(out, prev, prev_len);
    out[prev_len] = '\0';
    return (int)prev_len;
}

int dict_v4_resolve_slot_ctx(const struct dict_v4 *d, uint32_t slot, uint8_t dict,
                             char *out, size_t out_size)
{
    if (!d || !d->header || !out || out_size == 0 || dict > 1) {
        return -EINVAL;
    }
    if (!d->validx) {
        return -ENOTSUP;
    }
    if (slot < d->validx_start_slot ||
        slot - d->validx_start_slot >= d->validx_slot_count) {
        return -ENOENT;
    }

    uint32_t sid = read_bits_lsb(d->validx,
                                 (slot - d->validx_start_slot) *
                                 DICT_V4_VALIDX_BITS,
                                 DICT_V4_VALIDX_BITS);

    if (dict == DICT_V4_DICT_LAPWING && d->conflicts) {
        uint32_t conflict_sid;
        if (conflict_lookup(d, slot, &conflict_sid) == 0) {
            sid = conflict_sid;
        }
    }

    if (sid >= d->header->string_count) {
        return -EBADMSG;
    }
    return dict_v4_string_by_id_ctx(d, sid, out, out_size);
}

uint8_t dict_v4_max_strokes(const struct dict_v4 *d)
{
    if (!d || !d->header) {
        return 0;
    }
    return d->header->max_entry_strokes;
}

/* ─── Zephyr singleton wrappers (embedded blob) ─── */

#ifdef __ZEPHYR__

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dict_v4, CONFIG_STENO_SPLIT_LOG_LEVEL);

#ifdef CONFIG_STENO_SPLIT_DICT
/* BLE transport for the sections living on the other half (split_dict.c) */
extern int split_dict_get_string(uint32_t string_id, char *out, size_t out_size);
extern int split_dict_resolve(uint32_t slot, uint8_t dict, char *out, size_t out_size);
#endif

extern const uint8_t steno_dict_start[];
extern const uint8_t steno_dict_end[];

static struct dict_v4 dict_singleton;
static bool dict_singleton_ready;
static uint8_t active_dict_id;

static int singleton_init(void)
{
    if (dict_singleton_ready) {
        return 0;
    }

    size_t blob_len = (size_t)(steno_dict_end - steno_dict_start);
    int ret = dict_v4_init(&dict_singleton, steno_dict_start, blob_len);
    if (ret != 0) {
        return ret;
    }

    active_dict_id = (dict_singleton.header->dicts_mask &
                      (1u << DICT_V4_DICT_PLOVER))
                   ? DICT_V4_DICT_PLOVER : DICT_V4_DICT_LAPWING;
    dict_singleton_ready = true;
    return 0;
}

int dict_v4_string_by_id(uint32_t string_id, char *out, size_t out_size)
{
    int ret = singleton_init();
    if (ret != 0) {
        return ret;
    }
    return dict_v4_string_by_id_ctx(&dict_singleton, string_id, out, out_size);
}

int dict_v4_resolve_slot(uint32_t slot, uint8_t dict, char *out, size_t out_size)
{
    int ret = singleton_init();
    if (ret != 0) {
        return ret;
    }
    return dict_v4_resolve_slot_ctx(&dict_singleton, slot, dict, out, out_size);
}

int steno_dict_lookup(const uint32_t *strokes, uint8_t count,
                      char *out, size_t out_size)
{
    int ret = singleton_init();
    if (ret != 0) {
        return ret;
    }

    uint32_t slot = 0;
    uint32_t string_id = 0;

    ret = dict_v4_lookup(&dict_singleton, strokes, count, active_dict_id,
                         &slot, &string_id);

    LOG_DBG("lookup n=%u s0=0x%06x dict=%u -> %d slot=%u sid=%u",
            count, strokes[0], active_dict_id, ret, slot, string_id);

    if (ret == DICT_V4_FOUND_LOCAL) {
        if (dict_singleton.strdir_entries && dict_singleton.strings) {
            return dict_v4_string_by_id_ctx(&dict_singleton, string_id,
                                            out, out_size);
        }
#ifdef CONFIG_STENO_SPLIT_DICT
        ret = split_dict_get_string(string_id, out, out_size);
        LOG_DBG("get_string(%u) -> %d", string_id, ret);
        return ret;
#else
        return -ENOTSUP;
#endif
    }

    if (ret == DICT_V4_FOUND_REMOTE) {
#ifdef CONFIG_STENO_SPLIT_DICT
        ret = split_dict_resolve(slot, active_dict_id, out, out_size);
        LOG_DBG("resolve(%u) -> %d", slot, ret);
        return ret;
#else
        return -ENOTSUP;
#endif
    }

    if (ret == DICT_V4_MISS) {
        return -ENOENT;
    }
    return ret;
}

uint8_t steno_dict_max_strokes(void)
{
    if (singleton_init() != 0) {
        return 0;
    }
    return dict_v4_max_strokes(&dict_singleton);
}

void steno_dict_set_active(uint8_t dict_id)
{
    if (singleton_init() != 0) {
        return;
    }
    if (dict_id > 1 ||
        !(dict_singleton.header->dicts_mask & (1u << dict_id))) {
        return;
    }
    active_dict_id = dict_id;
}

#endif /* __ZEPHYR__ */

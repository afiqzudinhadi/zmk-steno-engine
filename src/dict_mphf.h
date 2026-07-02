/**
 * MPHF (Minimal Perfect Hash Function) dictionary lookup engine.
 *
 * Binary format v3: CHD MPHF with spare slots + bit-packed
 * displacements/values + fingerprinted verification +
 * block-compressed string table.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#ifndef DICT_MPHF_H
#define DICT_MPHF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DICT_MPHF_MAGIC   0x4F4E5453  /* "STNO" */
#define DICT_MPHF_VERSION 3

#define DICT_MPHF_FLAG_COMPRESSED 0x0001

struct dict_mphf_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t slot_count;
    uint32_t bucket_count;
    uint32_t unique_count;
    uint8_t  value_bits;
    uint8_t  disp_bits;
    uint16_t prefix_count;
    uint32_t block_size;
    uint32_t entry_count;
} __attribute__((packed));

_Static_assert(sizeof(struct dict_mphf_header) == 32, "header must be 32 bytes");

struct dict_mphf {
    const struct dict_mphf_header *header;
    const uint8_t  *displacements;
    const uint8_t  *values;
    const uint8_t  *fingerprints;
    const uint8_t  *string_offsets;
    const uint8_t  *str_data_start;    /* start of string data section */
    const uint32_t *prefix_table;
    uint32_t        disp_section_len;
    uint32_t        val_section_len;
    uint32_t        fp_section_len;
    uint16_t        block_count;
    const uint32_t *block_dir;         /* block offset directory */
    const uint8_t  *blocks_start;      /* start of compressed blocks */
    uint32_t        str_data_len;      /* total string data section length */
    uint32_t        blk_size;          /* actual block size from header */
};

int dict_mphf_init(struct dict_mphf *dict, const void *data, size_t len);

const char *dict_mphf_lookup(const struct dict_mphf *dict,
                             const uint32_t *strokes, uint8_t count);

bool dict_mphf_has_prefix(const struct dict_mphf *dict, uint32_t stroke);

static inline uint32_t dict_mphf_count(const struct dict_mphf *dict)
{
    return dict->header->entry_count;
}

static inline uint32_t dict_mphf_slot_count(const struct dict_mphf *dict)
{
    return dict->header->slot_count;
}

#endif /* DICT_MPHF_H */

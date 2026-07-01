/**
 * MPHF (Minimal Perfect Hash Function) dictionary lookup engine.
 *
 * Binary format v2: CHD MPHF + bit-packed displacements/values +
 * fingerprinted verification + deduped string table.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#ifndef DICT_MPHF_H
#define DICT_MPHF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DICT_MPHF_MAGIC   0x4F4E5453  /* "STNO" */
#define DICT_MPHF_VERSION 2

/**
 * On-flash header (32 bytes). All fields little-endian.
 *
 * Layout:
 *   [0..3]   magic        u32
 *   [4..5]   version      u16
 *   [6..7]   flags        u16
 *   [8..11]  entry_count  u32
 *   [12..15] bucket_count u32
 *   [16..19] unique_count u32
 *   [20]     value_bits   u8
 *   [21]     disp_bits    u8
 *   [22..23] prefix_count u16
 *   [24..27] reserved0    u32
 *   [28..31] reserved1    u32
 */
struct dict_mphf_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t bucket_count;
    uint32_t unique_count;
    uint8_t  value_bits;
    uint8_t  disp_bits;
    uint16_t prefix_count;
    uint32_t reserved0;
    uint32_t reserved1;
} __attribute__((packed));

_Static_assert(sizeof(struct dict_mphf_header) == 32, "header must be 32 bytes");

/**
 * Runtime dictionary handle. Points into flash-resident data.
 * All pointers are into the compiled binary blob — no heap allocation.
 */
struct dict_mphf {
    const struct dict_mphf_header *header;
    const uint8_t  *displacements;   /* bit-packed, bucket_count entries */
    const uint8_t  *values;          /* bit-packed, entry_count entries  */
    const uint8_t  *fingerprints;    /* 1 byte per entry                */
    const uint8_t  *string_offsets;  /* unique_count offsets (u24 LE)    */
    const char     *string_data;     /* null-terminated UTF-8 strings   */
    const uint32_t *prefix_table;    /* sorted first-strokes, prefix_count entries */
    uint32_t        disp_section_len;
    uint32_t        val_section_len;
    uint32_t        fp_section_len;
};

/**
 * Initialize dictionary handle from a compiled binary blob.
 *
 * @param dict   Handle to initialize
 * @param data   Pointer to compiled binary (typically flash-mapped)
 * @param len    Length of binary in bytes
 * @return 0 on success, negative on error
 *   -1: NULL pointer
 *   -2: too short
 *   -3: bad magic
 *   -4: version mismatch
 */
int dict_mphf_init(struct dict_mphf *dict, const void *data, size_t len);

/**
 * Look up a stroke sequence in the dictionary.
 *
 * @param dict    Initialized dictionary handle
 * @param strokes Array of stroke values (each is a 23-bit steno chord)
 * @param count   Number of strokes in sequence
 * @return Pointer to null-terminated translation string in flash,
 *         or NULL if not found / fingerprint mismatch
 */
const char *dict_mphf_lookup(const struct dict_mphf *dict,
                             const uint32_t *strokes, uint8_t count);

/**
 * Check if a stroke could be the prefix of a multi-stroke entry.
 *
 * Binary search on sorted prefix table of first-strokes from
 * multi-stroke entries.
 *
 * @param dict   Initialized dictionary handle
 * @param stroke Single stroke value to check
 * @return true if stroke appears as first stroke of any multi-stroke entry
 */
bool dict_mphf_has_prefix(const struct dict_mphf *dict, uint32_t stroke);

/**
 * Get number of entries in the dictionary.
 */
static inline uint32_t dict_mphf_count(const struct dict_mphf *dict)
{
    return dict->header->entry_count;
}

#endif /* DICT_MPHF_H */

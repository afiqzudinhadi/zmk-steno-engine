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
 * Dictionary binary format v4 — "Union Split-Section" decoder.
 *
 * Byte layout per docs/FORMAT_V4.md. One union CHD MPHF over both
 * source dicts (plover = 0, lapwing = 1), sections split across the
 * two keyboard halves:
 *
 *   LEFT:  DISP, MEMBERSHIP, FP, CONFLICTS, VALIDX slice [0, k)
 *   RIGHT: STRDIR, STRINGS, CONFLICTS, VALIDX slice [k, n)
 *
 * The decision path (dict_v4_lookup) runs entirely on the left half;
 * the string path (dict_v4_string_by_id_ctx / dict_v4_resolve_slot_ctx)
 * runs on whichever context holds the needed sections.
 */

#ifndef DICT_V4_H
#define DICT_V4_H

#include <stdint.h>
#include <stddef.h>

#define DICT_V4_MAGIC   0x344E5453u /* "STN4" */
#define DICT_V4_VERSION 4

/* Section types (FORMAT_V4.md section 4) */
#define DICT_V4_SEC_DISP       1
#define DICT_V4_SEC_MEMBERSHIP 2
#define DICT_V4_SEC_VALIDX     3
#define DICT_V4_SEC_CONFLICTS  4
#define DICT_V4_SEC_FP         5
#define DICT_V4_SEC_STRDIR     6
#define DICT_V4_SEC_STRINGS    7

/* Source dictionary ids */
#define DICT_V4_DICT_PLOVER  0
#define DICT_V4_DICT_LAPWING 1

/* Displacement Huffman classes: class = bit_length(v), v <= 2^18-ish */
#define DICT_V4_DISP_CLASSES 19

/* VALIDX fixed field width */
#define DICT_V4_VALIDX_BITS 17

/* CONFLICTS record: u40le = slot (18 bits) | string_id << 18 */
#define DICT_V4_CONFLICT_REC_SIZE 5

/* Every translation is <= 255 bytes (compile-time enforced) */
#define DICT_V4_MAX_TEXT 255

/* FC string block decode buffer bound (FORMAT_V4.md section 4.7) */
#define DICT_V4_BLOCK_BUF_SIZE 16384

/* Local key buffer bound; header max_entry_strokes is the real limit */
#define DICT_V4_MAX_KEY_STROKES 16

/* dict_v4_lookup results (>= 0); negative values are -errno */
#define DICT_V4_MISS         0
#define DICT_V4_FOUND_LOCAL  1  /* string id known, in *string_id */
#define DICT_V4_FOUND_REMOTE 2  /* validx slice remote, caller RESOLVEs slot */

struct dict_v4_header {
    uint32_t magic;
    uint16_t version;
    uint16_t section_count;
    uint32_t n;                 /* union key count */
    uint32_t bucket_count;
    uint32_t string_count;
    uint32_t conflict_count;
    uint32_t d_threshold;       /* displacement escape threshold */
    uint8_t  fp_bits;           /* 4 */
    uint8_t  dicts_mask;        /* bit0 plover, bit1 lapwing */
    uint8_t  max_entry_strokes;
    uint8_t  reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct dict_v4_header) == 32, "header must be 32 bytes");

struct dict_v4_section {
    uint8_t  type;
    uint8_t  flags;
    uint16_t rsvd;
    uint32_t offset;            /* relative to blob start */
    uint32_t len;
    uint32_t param;             /* VALIDX: start_slot; STRDIR: block_count;
                                 * CONFLICTS: record count */
} __attribute__((packed));

_Static_assert(sizeof(struct dict_v4_section) == 16, "dir entry must be 16 bytes");

struct dict_v4 {
    const struct dict_v4_header *header;

    /* DISP (NULL pointers = section absent from this half) */
    const uint8_t *disp_code_len;    /* u8[19] canonical code lengths */
    const uint8_t *disp_skip;        /* u32 LE entries, unaligned-safe reads */
    uint32_t       disp_skip_count;
    const uint8_t *disp_stream;      /* MSB-first Huffman bitstream */
    uint32_t       disp_stream_bits;

    /* Canonical Huffman decode tables, built at init from code lengths.
     * Indexed by code length (1..huff_max_len). */
    uint8_t  huff_max_len;
    uint8_t  huff_count[DICT_V4_DISP_CLASSES + 1];
    uint32_t huff_first_code[DICT_V4_DISP_CLASSES + 1];
    uint8_t  huff_sym_base[DICT_V4_DISP_CLASSES + 1];
    uint8_t  huff_symbols[DICT_V4_DISP_CLASSES];

    /* Fixed-width LSB-first packed sections */
    const uint8_t *membership;       /* 2 bits/slot */
    const uint8_t *fp;               /* fp_bits/slot */
    const uint8_t *validx;           /* 17 bits/slot slice */
    uint32_t       validx_start_slot;
    uint32_t       validx_slot_count;

    /* CONFLICTS: sorted 5-byte records */
    const uint8_t *conflicts;
    uint32_t       conflict_count;

    /* String table */
    const uint8_t *strdir_entries;   /* {u32 comp_off; u32 first_string_id}[] */
    uint32_t       strdir_block_count;
    const uint8_t *strings;
    uint32_t       strings_len;
};

/* Parse a half blob in place; no copies. Returns 0 or -errno. */
int dict_v4_init(struct dict_v4 *d, const void *blob, size_t len);

/*
 * Decision path (left half). Returns DICT_V4_FOUND_LOCAL (string id
 * known, in *string_id), DICT_V4_FOUND_REMOTE (validx slice remote,
 * caller must RESOLVE *slot), DICT_V4_MISS, or -errno.
 * *slot is set on both FOUND results.
 */
int dict_v4_lookup(const struct dict_v4 *d, const uint32_t *strokes, uint8_t count,
                   uint8_t active_dict, uint32_t *slot, uint32_t *string_id);

/*
 * String path (right half or host test). Both return the translation
 * length (>= 0, `out` NUL-terminated) or -errno.
 */
int dict_v4_string_by_id_ctx(const struct dict_v4 *d, uint32_t string_id,
                             char *out, size_t out_size);
int dict_v4_resolve_slot_ctx(const struct dict_v4 *d, uint32_t slot, uint8_t dict,
                             char *out, size_t out_size);

uint8_t dict_v4_max_strokes(const struct dict_v4 *d);

#ifdef __ZEPHYR__
/*
 * Singleton wrappers over the blob embedded by dict_embed.S
 * (steno_dict_start/steno_dict_end): left half gets steno_v4_left.bin,
 * right half steno_v4_right.bin. Lazily initialized on first use.
 */
int dict_v4_string_by_id(uint32_t string_id, char *out, size_t out_size);
int dict_v4_resolve_slot(uint32_t slot, uint8_t dict, char *out, size_t out_size);

/* Dictionary dispatcher consumed by behavior_steno.c */
int steno_dict_lookup(const uint32_t *strokes, uint8_t count,
                      char *out, size_t out_size);
uint8_t steno_dict_max_strokes(void);
void steno_dict_set_active(uint8_t dict_id);
#endif /* __ZEPHYR__ */

#endif /* DICT_V4_H */

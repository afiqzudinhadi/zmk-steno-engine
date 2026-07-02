/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

#ifndef SPLIT_DICT_H
#define SPLIT_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/bluetooth/uuid.h>

/* Custom 128-bit UUID base for steno GATT service
 * Base: 7374656e-6f00-4000-8000-000000000000 */
#define STENO_UUID_BASE \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000000))

#define STENO_UUID_SERVICE \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000001))

#define STENO_UUID_DICT_QUERY \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000002))

#define STENO_UUID_DICT_PREFIX \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000003))

#define STENO_UUID_DICT_BATCH \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000004))

/* Message types */
enum steno_msg_type {
    STENO_MSG_QUERY   = 0x01,
    STENO_MSG_PREFIX  = 0x02,
    STENO_MSG_BATCH   = 0x03,
    STENO_MSG_RESPONSE = 0x80,
};

/* Status codes */
enum steno_status {
    STENO_STATUS_FOUND       = 0,
    STENO_STATUS_NOT_FOUND   = 1,
    STENO_STATUS_PREFIX_ONLY = 2,
    STENO_STATUS_ERROR       = 3,
};

/* Packet structures */
struct steno_query_pkt {
    uint8_t msg_type;
    uint8_t seq;
    uint8_t stroke_count;
    uint8_t strokes[];        /* 3 bytes per stroke (24-bit packed) */
} __packed;

struct steno_response_pkt {
    uint8_t msg_type;
    uint8_t seq;
    uint8_t status;
    uint16_t data_len;
    uint8_t data[];           /* translation string (UTF-8, not null-terminated) */
} __packed;

struct steno_batch_query_pkt {
    uint8_t msg_type;
    uint8_t seq;
    uint8_t query_count;
    uint8_t queries[];        /* packed steno_query_pkt entries (without msg_type/seq) */
} __packed;

/* Batch result entry */
struct steno_batch_result {
    uint8_t status;
    char translation[128];
    uint16_t translation_len;
    bool has_prefix;
};

/* API */
int split_dict_init(void);
int split_dict_lookup(const uint32_t *strokes, uint8_t count,
                      char *result, size_t result_size);
bool split_dict_has_prefix(const uint32_t *strokes, uint8_t count);
int split_dict_batch_lookup(const uint32_t **stroke_seqs, const uint8_t *counts,
                            uint8_t num_queries, struct steno_batch_result *results);

/* GATT service registration */
int split_dict_gatt_register(void);

#endif /* SPLIT_DICT_H */

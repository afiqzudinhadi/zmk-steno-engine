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

#define STENO_UUID_DICT_REQ \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000002))

/* BLE protocol v4 message types (FORMAT_V4.md section 7) */
enum steno_msg_type {
    STENO_MSG_GET_STRING = 0x10,
    STENO_MSG_RESOLVE    = 0x11,
    STENO_MSG_RESPONSE   = 0x80,
};

/* Response status codes */
enum steno_status {
    STENO_STATUS_OK        = 0,
    STENO_STATUS_NOT_FOUND = 1,
    STENO_STATUS_ERROR     = 2,
};

/* Format v4 guarantees every translation <= 255 bytes */
#define SPLIT_DICT_MAX_TEXT 255

/* Packet structures; all multi-byte fields little-endian on the wire */
struct steno_get_string_pkt {
    uint8_t msg_type;         /* STENO_MSG_GET_STRING */
    uint8_t seq;
    uint32_t string_id;       /* LE */
} __attribute__((packed));

struct steno_resolve_pkt {
    uint8_t msg_type;         /* STENO_MSG_RESOLVE */
    uint8_t seq;
    uint32_t slot;            /* LE */
    uint8_t dict;             /* 0 = plover, 1 = lapwing */
} __attribute__((packed));

struct steno_response_pkt {
    uint8_t msg_type;         /* STENO_MSG_RESPONSE */
    uint8_t seq;
    uint8_t status;           /* enum steno_status */
    uint16_t len;             /* LE, byte length of text[] */
    char text[];              /* translation (UTF-8, not null-terminated) */
} __attribute__((packed));

/* Central-side API */
int split_dict_init(void);
int split_dict_get_string(uint32_t string_id, char *out, size_t out_size);
int split_dict_resolve(uint32_t slot, uint8_t dict, char *out, size_t out_size);

/* GATT service registration */
int split_dict_gatt_register(void);

#endif /* SPLIT_DICT_H */

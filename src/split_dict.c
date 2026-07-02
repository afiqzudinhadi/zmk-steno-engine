/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "split_dict.h"
#include "split_cache.h"

LOG_MODULE_REGISTER(split_dict, CONFIG_STENO_SPLIT_LOG_LEVEL);

/* Peripheral-side v4 dict resolvers, implemented in dict_v4.c */
extern int dict_v4_string_by_id(uint32_t string_id, char *out, size_t out_size);
extern int dict_v4_resolve_slot(uint32_t slot, uint8_t dict, char *out, size_t out_size);

/* Semaphore for blocking on BLE response */
static K_SEM_DEFINE(response_sem, 0, 1);

/* Current pending response state (central side) */
static uint8_t pending_seq;
static uint8_t rx_buf[sizeof(struct steno_response_pkt) + SPLIT_DICT_MAX_TEXT];
static uint16_t rx_len;
static uint8_t seq_counter;

/* Notify buffer (peripheral side) */
static uint8_t notify_buf[sizeof(struct steno_response_pkt) + SPLIT_DICT_MAX_TEXT];

/* Cache instance */
static struct split_cache dict_cache;

/* --- Cache key synthesis ---
 *
 * The LRU cache keys on stroke sequences (uint32_t[]). Protocol v4 requests
 * are keyed instead by string_id or (slot, dict); synthesize a two-word
 * pseudo-stroke key with a tag word that can never collide with a real
 * 23-bit stroke bitmask. */

#define CACHE_TAG_GET_STRING 0x80000010u
#define CACHE_TAG_RESOLVE    0x80000011u

static void make_string_key(uint32_t string_id, uint32_t key[2])
{
    key[0] = CACHE_TAG_GET_STRING;
    key[1] = string_id;
}

static void make_resolve_key(uint32_t slot, uint8_t dict, uint32_t key[2])
{
    key[0] = CACHE_TAG_RESOLVE | ((uint32_t)dict << 8);
    key[1] = slot;
}

/* --- GATT Write Callbacks (peripheral side handlers) --- */

static void send_response(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          uint8_t seq, int ret, const char *text)
{
    struct steno_response_pkt *resp = (struct steno_response_pkt *)notify_buf;
    uint16_t tlen = 0;

    resp->msg_type = STENO_MSG_RESPONSE;
    resp->seq = seq;

    if (ret >= 0) {
        resp->status = STENO_STATUS_OK;
        tlen = (uint16_t)strlen(text);
        memcpy(resp->text, text, tlen);
    } else if (ret == -ENOENT) {
        resp->status = STENO_STATUS_NOT_FOUND;
    } else {
        resp->status = STENO_STATUS_ERROR;
    }

    resp->len = sys_cpu_to_le16(tlen);

    bt_gatt_notify(conn, attr, notify_buf, sizeof(*resp) + tlen);
}

static ssize_t handle_get_string(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len)
{
    const struct steno_get_string_pkt *pkt = buf;
    char text[SPLIT_DICT_MAX_TEXT + 1];

    if (len < sizeof(*pkt)) {
        LOG_WRN("GET_STRING pkt too short: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint32_t string_id = sys_le32_to_cpu(pkt->string_id);

    int ret = dict_v4_string_by_id(string_id, text, sizeof(text));

    send_response(conn, attr, pkt->seq, ret, text);

    return len;
}

static ssize_t handle_resolve(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len)
{
    const struct steno_resolve_pkt *pkt = buf;
    char text[SPLIT_DICT_MAX_TEXT + 1];

    if (len < sizeof(*pkt)) {
        LOG_WRN("RESOLVE pkt too short: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint32_t slot = sys_le32_to_cpu(pkt->slot);

    int ret = dict_v4_resolve_slot(slot, pkt->dict, text, sizeof(text));

    send_response(conn, attr, pkt->seq, ret, text);

    return len;
}

static ssize_t dict_req_write_cb(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len,
                                 uint16_t offset, uint8_t flags)
{
    const uint8_t *bytes = buf;

    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    switch (bytes[0]) {
    case STENO_MSG_GET_STRING:
        return handle_get_string(conn, attr, buf, len);
    case STENO_MSG_RESOLVE:
        return handle_resolve(conn, attr, buf, len);
    default:
        LOG_WRN("Unknown msg type: 0x%02x", bytes[0]);
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
}

/* --- Notification callback (central side) --- */

static uint8_t notify_cb(struct bt_conn *conn,
                         struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length)
{
    if (!data) {
        LOG_DBG("Notification unsubscribed");
        return BT_GATT_ITER_STOP;
    }

    const struct steno_response_pkt *resp = data;

    if (length < sizeof(struct steno_response_pkt)) {
        LOG_WRN("Response too short");
        return BT_GATT_ITER_CONTINUE;
    }

    if (resp->msg_type == STENO_MSG_RESPONSE && resp->seq == pending_seq) {
        if (length > sizeof(rx_buf)) {
            length = sizeof(rx_buf);
        }
        memcpy(rx_buf, data, length);
        rx_len = length;
        k_sem_give(&response_sem);
    }

    return BT_GATT_ITER_CONTINUE;
}

/* --- GATT Service Definition --- */

BT_GATT_SERVICE_DEFINE(steno_dict_svc,
    BT_GATT_PRIMARY_SERVICE(STENO_UUID_SERVICE),

    /* Dict request characteristic: write + notify */
    BT_GATT_CHARACTERISTIC(STENO_UUID_DICT_REQ,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, dict_req_write_cb, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* --- Central-side API --- */

/* Connection handle for GATT writes (set externally or via connection cb) */
static struct bt_conn *split_conn;
static struct bt_gatt_subscribe_params subscribe_params;

/* Send one request packet, block for its RESPONSE, copy text into out.
 * Returns text length on success, -ENOENT on NOT_FOUND, negative errno
 * otherwise. out must hold SPLIT_DICT_MAX_TEXT + 1 bytes. */
static int split_request(const void *pkt, uint16_t pkt_len, uint8_t seq, char *out)
{
    int err;

    if (!split_conn) {
        LOG_ERR("No split connection");
        return -ENOTCONN;
    }

    pending_seq = seq;
    k_sem_reset(&response_sem);

    err = bt_gatt_write_without_response(split_conn, 0, pkt, pkt_len, false);
    if (err) {
        LOG_ERR("GATT write failed: %d", err);
        return err;
    }

    err = k_sem_take(&response_sem, K_MSEC(CONFIG_STENO_SPLIT_TIMEOUT_MS));
    if (err) {
        LOG_WRN("Response timeout");
        return -ETIMEDOUT;
    }

    const struct steno_response_pkt *resp = (const struct steno_response_pkt *)rx_buf;

    if (resp->status == STENO_STATUS_NOT_FOUND) {
        return -ENOENT;
    }
    if (resp->status != STENO_STATUS_OK) {
        return -EIO;
    }

    uint16_t tlen = sys_le16_to_cpu(resp->len);
    uint16_t avail = rx_len - sizeof(struct steno_response_pkt);

    if (tlen > avail) {
        LOG_WRN("Response truncated: len %u, got %u", tlen, avail);
        return -EIO;
    }

    memcpy(out, resp->text, tlen);
    out[tlen] = '\0';

    return tlen;
}

/* Copy full text into caller buffer, truncating if needed */
static int copy_out(const char *text, uint16_t tlen, char *out, size_t out_size)
{
    size_t copy_len = tlen;

    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }
    memcpy(out, text, copy_len);
    out[copy_len] = '\0';

    return (int)copy_len;
}

int split_dict_get_string(uint32_t string_id, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -EINVAL;
    }

    uint32_t key[2];
    make_string_key(string_id, key);

    if (split_cache_lookup(&dict_cache, key, 2, out, out_size, NULL)) {
        LOG_DBG("Cache hit for string %u", string_id);
        return strlen(out);
    }

    struct steno_get_string_pkt pkt = {
        .msg_type = STENO_MSG_GET_STRING,
        .seq = seq_counter++,
        .string_id = sys_cpu_to_le32(string_id),
    };

    char text[SPLIT_DICT_MAX_TEXT + 1];
    int ret = split_request(&pkt, sizeof(pkt), pkt.seq, text);
    if (ret < 0) {
        return ret;
    }

    /* Cache only entries that fit the cache value slot untruncated */
    if ((size_t)ret < SPLIT_CACHE_VALUE_SIZE) {
        split_cache_insert(&dict_cache, key, 2, text, false);
    }

    return copy_out(text, ret, out, out_size);
}

int split_dict_resolve(uint32_t slot, uint8_t dict, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return -EINVAL;
    }

    uint32_t key[2];
    make_resolve_key(slot, dict, key);

    if (split_cache_lookup(&dict_cache, key, 2, out, out_size, NULL)) {
        LOG_DBG("Cache hit for slot %u dict %u", slot, dict);
        return strlen(out);
    }

    struct steno_resolve_pkt pkt = {
        .msg_type = STENO_MSG_RESOLVE,
        .seq = seq_counter++,
        .slot = sys_cpu_to_le32(slot),
        .dict = dict,
    };

    char text[SPLIT_DICT_MAX_TEXT + 1];
    int ret = split_request(&pkt, sizeof(pkt), pkt.seq, text);
    if (ret < 0) {
        return ret;
    }

    /* Cache only entries that fit the cache value slot untruncated */
    if ((size_t)ret < SPLIT_CACHE_VALUE_SIZE) {
        split_cache_insert(&dict_cache, key, 2, text, false);
    }

    return copy_out(text, ret, out, out_size);
}

int split_dict_init(void)
{
    split_cache_init(&dict_cache);
    seq_counter = 0;
    split_conn = NULL;
    subscribe_params.notify = notify_cb;
    subscribe_params.value = BT_GATT_CCC_NOTIFY;

    LOG_INF("Split dict initialized (protocol v4)");
    return 0;
}

int split_dict_gatt_register(void)
{
    /* GATT service registered statically via BT_GATT_SERVICE_DEFINE */
    LOG_INF("Split dict GATT service registered");
    return 0;
}

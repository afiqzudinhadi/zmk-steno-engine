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
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/att.h>
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

#ifndef CONFIG_STENO_DEBUG_NO_GATT
BT_GATT_SERVICE_DEFINE(steno_dict_svc,
    BT_GATT_PRIMARY_SERVICE(STENO_UUID_SERVICE),

    /* Dict request characteristic: write (with or without response) + notify */
    BT_GATT_CHARACTERISTIC(STENO_UUID_DICT_REQ,
                           BT_GATT_CHRC_WRITE |
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP |
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, dict_req_write_cb, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);
#endif /* !CONFIG_STENO_DEBUG_NO_GATT */

/* --- Central-side client: connection tracking + GATT discovery --- */

static struct bt_conn *split_conn;
static uint16_t req_value_handle;
static struct bt_gatt_subscribe_params subscribe_params;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct bt_gatt_discover_params discover_params;

/* Static UUID storage: discovery is asynchronous, the filter UUID must
 * outlive the initiating call. */
static struct bt_uuid_128 uuid_steno_svc = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
    0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000001));
static struct bt_uuid_128 uuid_steno_req = BT_UUID_INIT_128(BT_UUID_128_ENCODE(
    0x7374656e, 0x6f00, 0x4000, 0x8000, 0x000000000002));

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;

    if (!attr) {
        LOG_WRN("Steno dict discovery: attribute not found (type %u)",
                params->type);
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        /* Service found → discover its request characteristic */
        discover_params.uuid = &uuid_steno_req.uuid;
        discover_params.start_handle = attr->handle + 1;
        discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, &discover_params);
        if (err) {
            LOG_ERR("Char discovery failed: %d", err);
        }
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        req_value_handle = bt_gatt_attr_value_handle(attr);

        /* CCC descriptor follows the value attribute */
        discover_params.uuid = BT_UUID_GATT_CCC;
        discover_params.start_handle = req_value_handle + 1;
        discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

        err = bt_gatt_discover(conn, &discover_params);
        if (err) {
            LOG_ERR("CCC discovery failed: %d", err);
        }
        return BT_GATT_ITER_STOP;
    }

    if (params->type == BT_GATT_DISCOVER_DESCRIPTOR) {
        subscribe_params.notify = notify_cb;
        subscribe_params.value = BT_GATT_CCC_NOTIFY;
        subscribe_params.value_handle = req_value_handle;
        subscribe_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &subscribe_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe failed: %d", err);
        } else {
            LOG_INF("Steno dict channel ready (handle 0x%04x)",
                    req_value_handle);
        }
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

static void start_discovery(struct bt_conn *conn)
{
    memset(&discover_params, 0, sizeof(discover_params));
    discover_params.uuid = &uuid_steno_svc.uuid;
    discover_params.func = discover_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        LOG_ERR("Steno dict service discovery failed: %d", err);
    }
}

static void split_connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info;

    if (err || bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    /* The connection this half initiated = the link to the peripheral
     * half. Host links have the peripheral role. */
    if (info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    if (split_conn) {
        bt_conn_unref(split_conn);
    }
    split_conn = bt_conn_ref(conn);
    req_value_handle = 0;

    LOG_INF("Split peripheral connected, discovering steno dict service");
    start_discovery(conn);
}

static void split_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (conn != split_conn) {
        return;
    }
    bt_conn_unref(split_conn);
    split_conn = NULL;
    req_value_handle = 0;
    LOG_INF("Split peripheral disconnected (reason %u)", reason);
}

BT_CONN_CB_DEFINE(steno_split_conn_cb) = {
    .connected = split_connected,
    .disconnected = split_disconnected,
};

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

/* Send one request packet, block for its RESPONSE, copy text into out.
 * Returns text length on success, -ENOENT on NOT_FOUND, negative errno
 * otherwise. out must hold SPLIT_DICT_MAX_TEXT + 1 bytes. */
static int split_request(const void *pkt, uint16_t pkt_len, uint8_t seq, char *out)
{
    int err;

    if (!split_conn || req_value_handle == 0) {
        LOG_WRN("Steno dict channel not ready");
        return -ENOTCONN;
    }

    pending_seq = seq;
    k_sem_reset(&response_sem);

    err = bt_gatt_write_without_response(split_conn, req_value_handle,
                                         pkt, pkt_len, false);
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

    LOG_INF("Split dict initialized (protocol v4)");
    return 0;
}

int split_dict_gatt_register(void)
{
    /* GATT service registered statically via BT_GATT_SERVICE_DEFINE */
    LOG_INF("Split dict GATT service registered");
    return 0;
}

static int split_dict_sys_init(void)
{
    return split_dict_init();
}

SYS_INIT(split_dict_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

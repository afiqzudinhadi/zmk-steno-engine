/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * https://polyformproject.org/licenses/noncommercial/1.0.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "split_dict.h"
#include "split_cache.h"
#include "dict_mphf.h"

LOG_MODULE_REGISTER(split_dict, CONFIG_STENO_SPLIT_LOG_LEVEL);

/* Semaphore for blocking on BLE response */
static K_SEM_DEFINE(response_sem, 0, 1);

/* Current pending response state */
static uint8_t pending_seq;
static uint8_t response_buf[256];
static uint16_t response_len;
static uint8_t seq_counter;

/* Cache instance */
static struct split_cache dict_cache;

/* Local MPHF dict for peripheral-side GATT lookups */
extern const uint8_t _steno_dict_start[];
extern const uint8_t _steno_dict_end[];
static struct dict_mphf peripheral_mphf;
static bool peripheral_dict_ready;

/* --- Helpers --- */

static void encode_strokes(const uint32_t *strokes, uint8_t count, uint8_t *out)
{
    for (uint8_t i = 0; i < count; i++) {
        out[i * 3 + 0] = (strokes[i] >> 16) & 0xFF;
        out[i * 3 + 1] = (strokes[i] >> 8) & 0xFF;
        out[i * 3 + 2] = strokes[i] & 0xFF;
    }
}

static void decode_strokes(const uint8_t *in, uint8_t count, uint32_t *strokes)
{
    for (uint8_t i = 0; i < count; i++) {
        strokes[i] = ((uint32_t)in[i * 3 + 0] << 16) |
                     ((uint32_t)in[i * 3 + 1] << 8) |
                     (uint32_t)in[i * 3 + 2];
    }
}

/* --- GATT Write Callbacks (peripheral side handlers) --- */

static ssize_t dict_query_write_cb(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len,
                                   uint16_t offset, uint8_t flags)
{
    const struct steno_query_pkt *pkt = buf;

    if (len < sizeof(struct steno_query_pkt)) {
        LOG_WRN("Query pkt too short: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t stroke_count = pkt->stroke_count;
    uint16_t expected = sizeof(struct steno_query_pkt) + stroke_count * 3;

    if (len < expected) {
        LOG_WRN("Query pkt truncated: got %u, need %u", len, expected);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint32_t strokes[8];
    if (stroke_count > 8) {
        stroke_count = 8;
    }
    decode_strokes(pkt->strokes, stroke_count, strokes);

    /* Build response */
    struct steno_response_pkt *resp = (struct steno_response_pkt *)response_buf;
    resp->msg_type = STENO_MSG_RESPONSE;
    resp->seq = pkt->seq;

    const char *translation = NULL;
    if (peripheral_dict_ready) {
        translation = dict_mphf_lookup(&peripheral_mphf, strokes, stroke_count);
    }

    if (translation) {
        uint16_t tlen = (uint16_t)strlen(translation);
        resp->status = STENO_STATUS_FOUND;
        resp->data_len = tlen;
        memcpy(resp->data, translation, tlen);
        response_len = sizeof(struct steno_response_pkt) + tlen;
    } else {
        resp->status = STENO_STATUS_NOT_FOUND;
        resp->data_len = 0;
        response_len = sizeof(struct steno_response_pkt);
    }

    /* Notify central with response */
    bt_gatt_notify(conn, attr, response_buf, response_len);

    return len;
}

static ssize_t dict_prefix_write_cb(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    const struct steno_query_pkt *pkt = buf;

    if (len < sizeof(struct steno_query_pkt)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t stroke_count = pkt->stroke_count;
    if (stroke_count > 8) {
        stroke_count = 8;
    }

    uint32_t strokes[8];
    decode_strokes(pkt->strokes, stroke_count, strokes);

    struct steno_response_pkt *resp = (struct steno_response_pkt *)response_buf;
    resp->msg_type = STENO_MSG_RESPONSE;
    resp->seq = pkt->seq;
    resp->data_len = 0;

    if (peripheral_dict_ready &&
        stroke_count == 1 &&
        dict_mphf_has_prefix(&peripheral_mphf, strokes[0])) {
        resp->status = STENO_STATUS_PREFIX_ONLY;
    } else {
        resp->status = STENO_STATUS_NOT_FOUND;
    }

    response_len = sizeof(struct steno_response_pkt);
    bt_gatt_notify(conn, attr, response_buf, response_len);

    return len;
}

static ssize_t dict_batch_write_cb(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len,
                                   uint16_t offset, uint8_t flags)
{
    const struct steno_batch_query_pkt *pkt = buf;

    if (len < sizeof(struct steno_batch_query_pkt)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    LOG_DBG("Batch query: %u queries", pkt->query_count);

    /* Process each sub-query packed in queries[] */
    uint16_t pos = 0;
    const uint8_t *data = pkt->queries;
    uint16_t data_len = len - sizeof(struct steno_batch_query_pkt);

    for (uint8_t q = 0; q < pkt->query_count && pos < data_len; q++) {
        if (pos >= data_len) {
            break;
        }
        uint8_t stroke_count = data[pos];
        pos++;

        if (stroke_count > 8) {
            stroke_count = 8;
        }
        if (pos + stroke_count * 3 > data_len) {
            break;
        }

        uint32_t strokes[8];
        decode_strokes(&data[pos], stroke_count, strokes);
        pos += stroke_count * 3;

        /* Lookup and send individual response per query */
        struct steno_response_pkt *resp = (struct steno_response_pkt *)response_buf;
        resp->msg_type = STENO_MSG_RESPONSE;
        resp->seq = pkt->seq;

        const char *translation = NULL;
        if (peripheral_dict_ready) {
            translation = dict_mphf_lookup(&peripheral_mphf, strokes, stroke_count);
        }

        if (translation) {
            uint16_t tlen = (uint16_t)strlen(translation);
            resp->status = STENO_STATUS_FOUND;
            resp->data_len = tlen;
            memcpy(resp->data, translation, tlen);
            response_len = sizeof(struct steno_response_pkt) + tlen;
        } else {
            resp->status = STENO_STATUS_NOT_FOUND;
            resp->data_len = 0;
            response_len = sizeof(struct steno_response_pkt);
        }

        bt_gatt_notify(conn, attr, response_buf, response_len);
    }

    return len;
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

    if (resp->seq == pending_seq) {
        memcpy(response_buf, data, length);
        response_len = length;
        k_sem_give(&response_sem);
    }

    return BT_GATT_ITER_CONTINUE;
}

/* --- GATT Service Definition --- */

BT_GATT_SERVICE_DEFINE(steno_dict_svc,
    BT_GATT_PRIMARY_SERVICE(STENO_UUID_SERVICE),

    /* Dict Query characteristic: write + notify */
    BT_GATT_CHARACTERISTIC(STENO_UUID_DICT_QUERY,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, dict_query_write_cb, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Dict Prefix characteristic: write + notify */
    BT_GATT_CHARACTERISTIC(STENO_UUID_DICT_PREFIX,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, dict_prefix_write_cb, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Dict Batch characteristic: write + notify */
    BT_GATT_CHARACTERISTIC(STENO_UUID_DICT_BATCH,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL, dict_batch_write_cb, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* --- Central-side API --- */

/* Connection handle for GATT writes (set externally or via connection cb) */
static struct bt_conn *split_conn;
static struct bt_gatt_subscribe_params subscribe_params;

int split_dict_lookup(const uint32_t *strokes, uint8_t count,
                      char *result, size_t result_size)
{
    if (count == 0 || count > 8) {
        return -EINVAL;
    }

    /* Check cache first */
    bool has_prefix;
    if (split_cache_lookup(&dict_cache, strokes, count, result, result_size, &has_prefix)) {
        LOG_DBG("Cache hit for %u strokes", count);
        return strlen(result);
    }

    if (!split_conn) {
        LOG_ERR("No split connection");
        return -ENOTCONN;
    }

    /* Build query packet */
    uint8_t pkt_buf[sizeof(struct steno_query_pkt) + 8 * 3];
    struct steno_query_pkt *pkt = (struct steno_query_pkt *)pkt_buf;

    pkt->msg_type = STENO_MSG_QUERY;
    pkt->seq = seq_counter++;
    pkt->stroke_count = count;
    encode_strokes(strokes, count, pkt->strokes);

    pending_seq = pkt->seq;
    k_sem_reset(&response_sem);

    uint16_t pkt_len = sizeof(struct steno_query_pkt) + count * 3;

    /* Send via GATT write */
    int err = bt_gatt_write_without_response(split_conn, 0, pkt_buf, pkt_len, false);
    if (err) {
        LOG_ERR("GATT write failed: %d", err);
        return err;
    }

    /* Wait for response */
    err = k_sem_take(&response_sem, K_MSEC(CONFIG_STENO_SPLIT_TIMEOUT_MS));
    if (err) {
        LOG_WRN("Response timeout");
        return -ETIMEDOUT;
    }

    /* Decode response */
    const struct steno_response_pkt *resp = (const struct steno_response_pkt *)response_buf;

    if (resp->status == STENO_STATUS_FOUND) {
        uint16_t copy_len = resp->data_len;
        if (copy_len >= result_size) {
            copy_len = result_size - 1;
        }
        memcpy(result, resp->data, copy_len);
        result[copy_len] = '\0';

        /* Cache the result */
        split_cache_insert(&dict_cache, strokes, count, result, false);

        return copy_len;
    }

    return -ENOENT;
}

bool split_dict_has_prefix(const uint32_t *strokes, uint8_t count)
{
    if (count == 0 || count > 8) {
        return false;
    }

    /* Check cache */
    bool has_prefix;
    char dummy[1];
    if (split_cache_lookup(&dict_cache, strokes, count, dummy, sizeof(dummy), &has_prefix)) {
        return has_prefix;
    }

    if (!split_conn) {
        return false;
    }

    uint8_t pkt_buf[sizeof(struct steno_query_pkt) + 8 * 3];
    struct steno_query_pkt *pkt = (struct steno_query_pkt *)pkt_buf;

    pkt->msg_type = STENO_MSG_PREFIX;
    pkt->seq = seq_counter++;
    pkt->stroke_count = count;
    encode_strokes(strokes, count, pkt->strokes);

    pending_seq = pkt->seq;
    k_sem_reset(&response_sem);

    uint16_t pkt_len = sizeof(struct steno_query_pkt) + count * 3;

    int err = bt_gatt_write_without_response(split_conn, 0, pkt_buf, pkt_len, false);
    if (err) {
        return false;
    }

    err = k_sem_take(&response_sem, K_MSEC(CONFIG_STENO_SPLIT_TIMEOUT_MS));
    if (err) {
        return false;
    }

    const struct steno_response_pkt *resp = (const struct steno_response_pkt *)response_buf;
    return resp->status == STENO_STATUS_PREFIX_ONLY;
}

int split_dict_batch_lookup(const uint32_t **stroke_seqs, const uint8_t *counts,
                            uint8_t num_queries, struct steno_batch_result *results)
{
    if (num_queries == 0 || !split_conn) {
        return -EINVAL;
    }

    /* Build batch packet */
    uint8_t pkt_buf[256];
    struct steno_batch_query_pkt *pkt = (struct steno_batch_query_pkt *)pkt_buf;

    pkt->msg_type = STENO_MSG_BATCH;
    pkt->seq = seq_counter++;
    pkt->query_count = num_queries;

    uint16_t pos = 0;
    for (uint8_t q = 0; q < num_queries; q++) {
        uint8_t cnt = counts[q];
        if (cnt > 8) {
            cnt = 8;
        }
        pkt->queries[pos] = cnt;
        pos++;
        encode_strokes(stroke_seqs[q], cnt, &pkt->queries[pos]);
        pos += cnt * 3;
    }

    uint16_t pkt_len = sizeof(struct steno_batch_query_pkt) + pos;

    pending_seq = pkt->seq;
    k_sem_reset(&response_sem);

    int err = bt_gatt_write_without_response(split_conn, 0, pkt_buf, pkt_len, false);
    if (err) {
        return err;
    }

    /* Collect responses for each query */
    for (uint8_t q = 0; q < num_queries; q++) {
        err = k_sem_take(&response_sem, K_MSEC(CONFIG_STENO_SPLIT_TIMEOUT_MS));
        if (err) {
            results[q].status = STENO_STATUS_ERROR;
            continue;
        }

        const struct steno_response_pkt *resp =
            (const struct steno_response_pkt *)response_buf;

        results[q].status = resp->status;
        results[q].has_prefix = (resp->status == STENO_STATUS_PREFIX_ONLY);

        if (resp->status == STENO_STATUS_FOUND && resp->data_len > 0) {
            uint16_t copy_len = resp->data_len;
            if (copy_len >= sizeof(results[q].translation)) {
                copy_len = sizeof(results[q].translation) - 1;
            }
            memcpy(results[q].translation, resp->data, copy_len);
            results[q].translation[copy_len] = '\0';
            results[q].translation_len = copy_len;
        } else {
            results[q].translation[0] = '\0';
            results[q].translation_len = 0;
        }
    }

    return 0;
}

int split_dict_init(void)
{
    split_cache_init(&dict_cache);
    seq_counter = 0;
    split_conn = NULL;

    /* Init peripheral-side MPHF dict for GATT lookups */
    size_t dict_size = _steno_dict_end - _steno_dict_start;
    if (dict_size > 4) {
        int ret = dict_mphf_init(&peripheral_mphf, _steno_dict_start, dict_size);
        if (ret == 0) {
            peripheral_dict_ready = true;
            LOG_INF("Peripheral partition loaded (%u bytes)", (unsigned)dict_size);
        } else {
            LOG_ERR("Peripheral partition init failed: %d", ret);
        }
    }

    LOG_INF("Split dict initialized");
    return 0;
}

int split_dict_gatt_register(void)
{
    /* GATT service registered statically via BT_GATT_SERVICE_DEFINE */
    LOG_INF("Split dict GATT service registered");
    return 0;
}

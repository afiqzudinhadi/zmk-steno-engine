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
 * Host round-trip test for the format v4 decoder.
 *
 * Loads the two half blobs produced by tools/compile_v4.py plus a
 * vector file of (dict_id, strokes, expected translation) records,
 * then drives the real split decision path: every lookup runs on the
 * LEFT context; the string is fetched from the RIGHT context via
 * string_by_id (FOUND_LOCAL) or resolve_slot (FOUND_REMOTE) and
 * compared byte-exact. Unknown keys must MISS, modulo the 4-bit
 * fingerprint false-accept rate (asserted < 10%).
 *
 *   test_dict_v4 <left.bin> <right.bin> <vectors.bin>
 *
 * Vector file layout: u32 magic 0x56543456, u32 count, then records
 * { u8 dict_id; u8 stroke_count; u8 known; u8 rsvd;
 *   u32 strokes[stroke_count]; u16 t_len; u8 translation[t_len]; }.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dict_v4.h"

#define VECTORS_MAGIC 0x56543456u
#define MAX_MISMATCH_PRINT 10
#define FALSE_ACCEPT_LIMIT_PCT 10.0

static uint8_t *load_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)len;
    return buf;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void print_strokes(const uint32_t *strokes, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        fprintf(stderr, "%s0x%06x", i ? "/" : "", strokes[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <left.bin> <right.bin> <vectors.bin>\n",
                argv[0]);
        return 2;
    }

    size_t left_len, right_len, vec_len;
    uint8_t *left_blob = load_file(argv[1], &left_len);
    uint8_t *right_blob = load_file(argv[2], &right_len);
    uint8_t *vec = load_file(argv[3], &vec_len);
    if (!left_blob || !right_blob || !vec) {
        return 2;
    }

    struct dict_v4 left, right;
    int ret = dict_v4_init(&left, left_blob, left_len);
    if (ret != 0) {
        fprintf(stderr, "FAIL: dict_v4_init(left) = %d\n", ret);
        return 1;
    }
    ret = dict_v4_init(&right, right_blob, right_len);
    if (ret != 0) {
        fprintf(stderr, "FAIL: dict_v4_init(right) = %d\n", ret);
        return 1;
    }

    if (left.header->n != right.header->n ||
        left.header->string_count != right.header->string_count ||
        left.header->conflict_count != right.header->conflict_count) {
        fprintf(stderr, "FAIL: left/right header mismatch\n");
        return 1;
    }
    if (left.validx_start_slot != 0 ||
        left.validx_slot_count != right.validx_start_slot ||
        right.validx_start_slot + right.validx_slot_count != left.header->n) {
        fprintf(stderr, "FAIL: VALIDX slices do not cover [0, n) "
                "(left [%u, %u), right [%u, %u))\n",
                left.validx_start_slot,
                left.validx_start_slot + left.validx_slot_count,
                right.validx_start_slot,
                right.validx_start_slot + right.validx_slot_count);
        return 1;
    }
    if (dict_v4_max_strokes(&left) == 0) {
        fprintf(stderr, "FAIL: max_strokes = 0\n");
        return 1;
    }

    if (vec_len < 8 || get_le32(vec) != VECTORS_MAGIC) {
        fprintf(stderr, "FAIL: bad vector file magic\n");
        return 2;
    }
    uint32_t vec_count = get_le32(vec + 4);

    uint32_t known_checked = 0, unknown_checked = 0;
    uint32_t mismatches = 0, false_accepts = 0;
    uint32_t local_hits = 0, remote_hits = 0;
    size_t pos = 8;

    for (uint32_t rec = 0; rec < vec_count; rec++) {
        if (pos + 4 > vec_len) {
            fprintf(stderr, "FAIL: truncated vector file (record %u)\n", rec);
            return 2;
        }
        uint8_t dict_id = vec[pos];
        uint8_t stroke_count = vec[pos + 1];
        uint8_t known = vec[pos + 2];
        pos += 4;

        if (stroke_count == 0 || stroke_count > DICT_V4_MAX_KEY_STROKES ||
            pos + (size_t)stroke_count * 4 + 2 > vec_len) {
            fprintf(stderr, "FAIL: bad vector record %u\n", rec);
            return 2;
        }

        uint32_t strokes[DICT_V4_MAX_KEY_STROKES];
        for (uint8_t i = 0; i < stroke_count; i++) {
            strokes[i] = get_le32(vec + pos + (size_t)i * 4);
        }
        pos += (size_t)stroke_count * 4;

        uint16_t t_len = get_le16(vec + pos);
        pos += 2;
        if (pos + t_len > vec_len) {
            fprintf(stderr, "FAIL: truncated translation (record %u)\n", rec);
            return 2;
        }
        const uint8_t *expected = vec + pos;
        pos += t_len;

        /* Decision path always on the LEFT context */
        uint32_t slot = 0, string_id = 0;
        ret = dict_v4_lookup(&left, strokes, stroke_count, dict_id,
                             &slot, &string_id);

        char text[DICT_V4_MAX_TEXT + 1];
        int text_len = -1;

        /* String path on the RIGHT context */
        if (ret == DICT_V4_FOUND_LOCAL) {
            local_hits++;
            text_len = dict_v4_string_by_id_ctx(&right, string_id,
                                                text, sizeof(text));
        } else if (ret == DICT_V4_FOUND_REMOTE) {
            remote_hits++;
            text_len = dict_v4_resolve_slot_ctx(&right, slot, dict_id,
                                                text, sizeof(text));
        }

        if (known) {
            known_checked++;
            int ok = (ret == DICT_V4_FOUND_LOCAL ||
                      ret == DICT_V4_FOUND_REMOTE) &&
                     text_len == (int)t_len &&
                     memcmp(text, expected, t_len) == 0;
            if (!ok) {
                mismatches++;
                if (mismatches <= MAX_MISMATCH_PRINT) {
                    fprintf(stderr, "MISMATCH dict=%u strokes=", dict_id);
                    print_strokes(strokes, stroke_count);
                    fprintf(stderr, " lookup=%d text_len=%d expected=%.*s",
                            ret, text_len, (int)t_len, (const char *)expected);
                    if (text_len >= 0) {
                        fprintf(stderr, " got=%.*s", text_len, text);
                    }
                    fprintf(stderr, "\n");
                }
            }
        } else {
            unknown_checked++;
            if (ret == DICT_V4_FOUND_LOCAL || ret == DICT_V4_FOUND_REMOTE) {
                /* Fingerprint false accept: string fetch must still
                 * succeed (no crash, valid id), the text is garbage. */
                false_accepts++;
                if (text_len < 0) {
                    mismatches++;
                    if (mismatches <= MAX_MISMATCH_PRINT) {
                        fprintf(stderr, "BAD FALSE-ACCEPT dict=%u lookup=%d "
                                "string fetch=%d strokes=",
                                dict_id, ret, text_len);
                        print_strokes(strokes, stroke_count);
                        fprintf(stderr, "\n");
                    }
                }
            } else if (ret != DICT_V4_MISS) {
                mismatches++;
                if (mismatches <= MAX_MISMATCH_PRINT) {
                    fprintf(stderr, "BAD MISS ret=%d strokes=", ret);
                    print_strokes(strokes, stroke_count);
                    fprintf(stderr, "\n");
                }
            }
        }
    }

    double fa_pct = unknown_checked
                  ? 100.0 * false_accepts / unknown_checked : 0.0;

    printf("vectors: %u (known %u, unknown %u)\n",
           vec_count, known_checked, unknown_checked);
    printf("hits: local %u, remote %u; mismatches %u\n",
           local_hits, remote_hits, mismatches);
    printf("false accepts: %u / %u (%.2f%%)\n",
           false_accepts, unknown_checked, fa_pct);

    if (mismatches != 0) {
        printf("FAIL: %u mismatches\n", mismatches);
        return 1;
    }
    if (fa_pct >= FALSE_ACCEPT_LIMIT_PCT) {
        printf("FAIL: false-accept rate %.2f%% >= %.1f%%\n",
               fa_pct, FALSE_ACCEPT_LIMIT_PCT);
        return 1;
    }

    printf("PASS\n");

    free(left_blob);
    free(right_blob);
    free(vec);
    return 0;
}

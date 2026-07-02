/*
 * Copyright (c) 2024 Afiq Zudin Hadi
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "undo.h"
#include <string.h>

void steno_undo_init(struct stroke_history *hist)
{
    memset(hist, 0, sizeof(*hist));
}

void steno_undo_push(struct stroke_history *hist,
                     const uint32_t *strokes, uint8_t stroke_count,
                     uint8_t output_len, uint8_t space_before,
                     uint8_t fmt_flags)
{
    struct stroke_history_entry *e = &hist->entries[hist->head];

    uint8_t n = stroke_count;
    if (n > STENO_MAX_MULTI_STROKE) {
        n = STENO_MAX_MULTI_STROKE;
    }

    memcpy(e->strokes, strokes, n * sizeof(uint32_t));
    e->stroke_count = n;
    e->output_len = output_len;
    e->space_before = space_before;
    e->fmt_flags = fmt_flags;

    hist->head = (hist->head + 1) % CONFIG_STENO_HISTORY_SIZE;

    if (hist->count < CONFIG_STENO_HISTORY_SIZE) {
        hist->count++;
    }
}

struct stroke_history_entry *steno_undo_pop(struct stroke_history *hist)
{
    if (hist->count == 0) {
        return NULL;
    }

    hist->head = (hist->head + CONFIG_STENO_HISTORY_SIZE - 1) % CONFIG_STENO_HISTORY_SIZE;
    hist->count--;

    return &hist->entries[hist->head];
}

const struct stroke_history_entry *steno_undo_peek(const struct stroke_history *hist)
{
    if (hist->count == 0) {
        return NULL;
    }

    uint16_t idx = (hist->head + CONFIG_STENO_HISTORY_SIZE - 1) % CONFIG_STENO_HISTORY_SIZE;
    return &hist->entries[idx];
}

const struct stroke_history_entry *steno_undo_peek_at(const struct stroke_history *hist,
                                                      uint16_t back)
{
    if (back >= hist->count) {
        return NULL;
    }

    uint16_t idx = (hist->head + CONFIG_STENO_HISTORY_SIZE - 1 - back) % CONFIG_STENO_HISTORY_SIZE;
    return &hist->entries[idx];
}

uint16_t steno_undo_count(const struct stroke_history *hist)
{
    return hist->count;
}

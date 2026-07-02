/*
 * Copyright (c) 2024 Afiq Zudin Hadi
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#define DT_DRV_COMPAT zmk_behavior_steno_engine

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <string.h>

#include "output.h"
#include "undo.h"
#include "formatter.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Dictionary dispatcher — implemented by the active dict backend
 * (dict_v4 / split). Exact-match lookup of `count` strokes: returns
 * the translation length (>= 0, `out` NUL-terminated) on a hit, or a
 * negative value on miss/error. steno_dict_max_strokes() reports the
 * longest entry in the loaded dictionary (0 if none loaded).
 */
extern int steno_dict_lookup(const uint32_t *strokes, uint8_t count,
                             char *out, size_t out_size);
extern uint8_t steno_dict_max_strokes(void);

/*
 * Runtime dictionary toggle (0 = plover, 1 = lapwing), implemented by
 * the dict backend.
 *
 * TODO: wire steno_dict_set_active() to a dedicated chord or behavior
 * parameter once a binding is assigned; no new DTS binding yet.
 */
extern void steno_dict_set_active(uint8_t dict_id);

#define STENO_BIT_STAR 9

struct steno_state {
    uint32_t current_chord;
    uint8_t  keys_held;
};

static struct steno_state state;
static struct steno_fmt_state fmt_state;
static struct stroke_history undo_history;

/* ── formatter state snapshots (packed into history fmt_flags) ─── */

static uint8_t fmt_pack(const struct steno_fmt_state *s)
{
    return (uint8_t)((s->space_pending  ? 0x01 : 0) |
                     (s->cap_next       ? 0x02 : 0) |
                     (s->upper_next     ? 0x04 : 0) |
                     (s->lower_next     ? 0x08 : 0) |
                     (s->suppress_space ? 0x10 : 0) |
                     (s->glue           ? 0x20 : 0) |
                     (((uint8_t)s->mode & 0x03) << 6));
}

static void fmt_unpack(uint8_t flags, struct steno_fmt_state *s)
{
    s->space_pending  = (flags & 0x01) != 0;
    s->cap_next       = (flags & 0x02) != 0;
    s->upper_next     = (flags & 0x04) != 0;
    s->lower_next     = (flags & 0x08) != 0;
    s->suppress_space = (flags & 0x10) != 0;
    s->glue           = (flags & 0x20) != 0;
    s->mode           = (enum steno_fmt_mode)((flags >> 6) & 0x03);
}

/* ── raw steno rendering (total-miss fallback) ──────────────────── */

/* '#' + 22 keys + '-' + NUL */
#define STENO_RAW_MAX 26

/* Bit layout per parse_stroke (tools/compile_mphf.py, FORMAT_V4.md) */
static const char steno_key_char[22] = {
    'S', 'T', 'K', 'P', 'W', 'H', 'R',                /* bits 0-6: left  */
    'A', 'O', '*', 'E', 'U',                          /* bits 7-11: mid  */
    'F', 'R', 'P', 'B', 'L', 'G', 'T', 'S', 'D', 'Z', /* bits 12-21: right */
};

#define STENO_MASK_MIDDLE 0x00000F80U /* A O * E U — implicit hyphen */
#define STENO_MASK_NUM    0x00400000U /* # */
#define STENO_FIRST_RIGHT 12

static void stroke_to_steno(uint32_t stroke, char out[STENO_RAW_MAX])
{
    uint8_t len = 0;
    bool implicit = (stroke & STENO_MASK_MIDDLE) != 0;

    if (stroke & STENO_MASK_NUM) {
        out[len++] = '#';
    }
    for (uint8_t bit = 0; bit < 22; bit++) {
        if (!(stroke & (1U << bit))) {
            continue;
        }
        if (bit >= STENO_FIRST_RIGHT && !implicit) {
            out[len++] = '-';
            implicit = true;
        }
        out[len++] = steno_key_char[bit];
    }
    out[len] = '\0';
}

/* ── emission ───────────────────────────────────────────────────── */

static void emit_formatted(const char *translation,
                           const uint32_t *strokes, uint8_t stroke_count)
{
    /* Snapshot formatter state BEFORE this translation mutates it, so
     * undo/retrace can restore it. */
    uint8_t snap = fmt_pack(&fmt_state);

    struct steno_fmt_result result;
    steno_fmt_process(&fmt_state, translation, &result);
    if (result.backspaces > 0) {
        steno_output_backspace(result.backspaces);
    }
    if (result.len > 0) {
        steno_output_send(result.text, result.len);
    }
    if (!result.is_command_only) {
        uint16_t out_chars = (uint16_t)result.len + result.backspaces;
        if (out_chars > 255) {
            out_chars = 255;
        }
        steno_undo_push(&undo_history, strokes, stroke_count,
                        (uint8_t)out_chars, 0, snap);
    }
}

static void emit_raw_stroke(uint32_t stroke)
{
    char raw[STENO_RAW_MAX];
    stroke_to_steno(stroke, raw);
    emit_formatted(raw, &stroke, 1);
}

/*
 * Greedy longest-match over a contiguous stroke span. Used to
 * re-translate strokes orphaned by a retrace. No further retrace:
 * matches are confined to the span.
 */
static void translate_span(const uint32_t *strokes, uint8_t count,
                           uint8_t max_win)
{
    uint8_t pos = 0;

    while (pos < count) {
        uint8_t span = count - pos;
        char text[STENO_FMT_MAX_OUTPUT];
        uint8_t match_len = 0;

        if (span > max_win) {
            span = max_win;
        }
        for (uint8_t l = span; l >= 1; l--) {
            if (steno_dict_lookup(&strokes[pos], l, text, sizeof(text)) >= 0) {
                match_len = l;
                break;
            }
        }
        if (match_len == 0) {
            emit_raw_stroke(strokes[pos]);
            pos++;
        } else {
            emit_formatted(text, &strokes[pos], match_len);
            pos += match_len;
        }
    }
}

/* ── sliding longest-match translation with retrace ─────────────── */

static void translate_stroke(uint32_t stroke)
{
    uint8_t max_win = steno_dict_max_strokes();

    if (max_win < 1) {
        max_win = 1;
    } else if (max_win > STENO_MAX_MULTI_STROKE) {
        max_win = STENO_MAX_MULTI_STROKE;
    }

    /*
     * Build the lookup window: win[max_win - 1] is the new stroke,
     * earlier strokes (pulled from translation history, newest first)
     * fill leftward. avail = previous strokes actually gathered.
     */
    uint32_t win[STENO_MAX_MULTI_STROKE];
    uint8_t avail = 0;

    win[max_win - 1] = stroke;
    for (uint16_t i = 0; avail < max_win - 1; i++) {
        const struct stroke_history_entry *e =
            steno_undo_peek_at(&undo_history, i);
        if (!e) {
            break;
        }
        for (uint8_t k = e->stroke_count; k > 0 && avail < max_win - 1; k--) {
            avail++;
            win[max_win - 1 - avail] = e->strokes[k - 1];
        }
    }

    /* Longest match wins: L = avail + 1 down to 1 */
    char text[STENO_FMT_MAX_OUTPUT];
    uint8_t match_len = 0;

    for (uint8_t l = avail + 1; l >= 1; l--) {
        if (steno_dict_lookup(&win[max_win - l], l, text, sizeof(text)) >= 0) {
            match_len = l;
            break;
        }
    }

    if (match_len == 0) {
        /* Total miss → raw steno chars for this stroke */
        emit_raw_stroke(stroke);
        return;
    }

    if (match_len == 1) {
        emit_formatted(text, &stroke, 1);
        return;
    }

    /*
     * Retrace: the match swallows strokes already emitted by earlier
     * translations. Pop those, erase their output, restore the
     * formatter state that preceded the oldest popped entry, then
     * re-emit.
     */
    uint8_t needed = match_len - 1;
    uint8_t covered = 0;
    uint16_t erase = 0;
    uint8_t snap = fmt_pack(&fmt_state);
    uint32_t orphan[STENO_MAX_MULTI_STROKE];
    uint8_t orphan_count = 0;

    while (covered < needed) {
        struct stroke_history_entry *e = steno_undo_pop(&undo_history);
        if (!e) {
            /* Cannot happen: avail came from the same entries */
            break;
        }
        covered += e->stroke_count;
        erase += e->output_len;
        snap = e->fmt_flags;
        if (covered > needed) {
            /*
             * Oldest popped entry straddles the window boundary: its
             * leading strokes fall outside the new match and must be
             * re-translated. Copy before pushes reuse the ring slot.
             */
            orphan_count = covered - needed;
            memcpy(orphan, e->strokes, orphan_count * sizeof(uint32_t));
        }
    }

    if (erase > 0) {
        steno_output_backspace(erase);
    }
    fmt_unpack(snap, &fmt_state);

    if (orphan_count > 0) {
        translate_span(orphan, orphan_count, max_win);
    }

    emit_formatted(text, &win[max_win - match_len], match_len);
}

/* ── chord assembly ─────────────────────────────────────────────── */

static void process_chord(void)
{
    uint32_t stroke = state.current_chord;

    state.current_chord = 0;
    if (stroke == 0) {
        return;
    }

    /* Star-only stroke → undo last translation */
    if (stroke == (1U << STENO_BIT_STAR)) {
        struct stroke_history_entry *ue = steno_undo_pop(&undo_history);
        if (ue) {
            if (ue->output_len > 0) {
                steno_output_backspace(ue->output_len);
            }
            fmt_unpack(ue->fmt_flags, &fmt_state);
        }
        return;
    }

    translate_stroke(stroke);
}

static int on_steno_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event)
{
    uint32_t key_index = binding->param1;
    if (key_index > 35) {
        return -EINVAL;
    }
    state.current_chord |= (1U << key_index);
    state.keys_held++;

    LOG_DBG("Key %u pressed, chord=0x%06X held=%u",
            key_index, state.current_chord, state.keys_held);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_steno_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    if (state.keys_held > 0) {
        state.keys_held--;
    }
    if (state.keys_held == 0 && state.current_chord != 0) {
        process_chord();
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_steno_init(const struct device *dev)
{
    ARG_UNUSED(dev);

    state.current_chord = 0;
    state.keys_held = 0;

    steno_fmt_init(&fmt_state);
    steno_undo_init(&undo_history);

    LOG_INF("Steno engine initialized");
    return 0;
}

static const struct behavior_driver_api steno_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_steno_binding_pressed,
    .binding_released = on_steno_binding_released,
};

#define STENO_INST(n)                                      \
    BEHAVIOR_DT_INST_DEFINE(n,                             \
                            behavior_steno_init,           \
                            NULL,                          \
                            NULL,                          \
                            NULL,                          \
                            POST_KERNEL,                   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                            &steno_driver_api);

DT_INST_FOREACH_STATUS_OKAY(STENO_INST)

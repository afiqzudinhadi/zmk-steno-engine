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

#include "output.h"
#include "undo.h"
#include "formatter.h"

#if IS_ENABLED(CONFIG_STENO_DICT_MPHF)
#include "dict_mphf.h"
#else
#include "trie.h"
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

extern const uint8_t _steno_dict_start[];
extern const uint8_t _steno_dict_end[];

#if IS_ENABLED(CONFIG_STENO_DICT_MPHF)
static struct dict_mphf mphf_dict;
#endif

#define STENO_MAX_MULTI    8
#define STENO_MULTI_TIMEOUT_MS CONFIG_STENO_MULTI_STROKE_TIMEOUT_MS

static inline const char *dict_lookup(const uint32_t *strokes, uint8_t count)
{
#if IS_ENABLED(CONFIG_STENO_DICT_MPHF)
    return dict_mphf_lookup(&mphf_dict, strokes, count);
#else
    return steno_trie_lookup(strokes, count);
#endif
}

static inline bool dict_has_prefix(const uint32_t *strokes, uint8_t count)
{
#if IS_ENABLED(CONFIG_STENO_DICT_MPHF)
    return (count == 1) ? dict_mphf_has_prefix(&mphf_dict, strokes[0]) : false;
#else
    return steno_trie_has_prefix(strokes, count);
#endif
}

struct steno_state {
    uint32_t current_chord;
    uint8_t  keys_held;
    uint32_t pending_strokes[STENO_MAX_MULTI];
    uint8_t  stroke_count;
    struct k_work_delayable multi_timeout;
};

static struct steno_state state;
static struct steno_fmt_state fmt_state;
static struct stroke_history undo_history;
static bool dict_ready;

static void flush_strokes(void);
static void multi_timeout_handler(struct k_work *work);

static void emit_formatted(const char *translation,
                           const uint32_t *strokes, uint8_t stroke_count)
{
    struct steno_fmt_result result;
    steno_fmt_process(&fmt_state, translation, &result);
    if (result.backspaces > 0) {
        steno_output_backspace(result.backspaces);
    }
    if (result.len > 0) {
        steno_output_send(result.text, result.len);
    }
    if (!result.is_command_only) {
        steno_undo_push(&undo_history, strokes, stroke_count,
                        result.len + result.backspaces, 0, 0);
    }
}

static void process_chord(void)
{
    if (state.current_chord == 0) {
        return;
    }

    /* Star-only stroke (bit 9) → undo */
    if (state.current_chord == (1U << 9)) {
        state.current_chord = 0;
        struct stroke_history_entry *ue = steno_undo_pop(&undo_history);
        if (ue) {
            steno_output_backspace(ue->output_len);
        }
        return;
    }

    k_work_cancel_delayable(&state.multi_timeout);

    if (state.stroke_count < STENO_MAX_MULTI) {
        state.pending_strokes[state.stroke_count] = state.current_chord;
        state.stroke_count++;
    } else {
        flush_strokes();
        state.pending_strokes[0] = state.current_chord;
        state.stroke_count = 1;
    }
    state.current_chord = 0;

    if (!dict_ready) {
        LOG_WRN("steno dict not ready, flushing");
        flush_strokes();
        return;
    }

    const char *translation = dict_lookup(
        state.pending_strokes, state.stroke_count);
    LOG_INF("steno lookup %u strokes → %s", state.stroke_count,
            translation ? translation : "(null)");

    if (translation) {
        emit_formatted(translation, state.pending_strokes, state.stroke_count);
        state.stroke_count = 0;
        return;
    }

    if (dict_has_prefix(state.pending_strokes, state.stroke_count)) {
        k_work_schedule(&state.multi_timeout,
                        K_MSEC(STENO_MULTI_TIMEOUT_MS));
        return;
    }

    if (state.stroke_count > 1) {
        uint32_t last = state.pending_strokes[state.stroke_count - 1];
        state.stroke_count--;

        const char *partial = dict_lookup(
            state.pending_strokes, state.stroke_count);
        if (partial) {
            emit_formatted(partial, state.pending_strokes, state.stroke_count);
        }

        state.pending_strokes[0] = last;
        state.stroke_count = 1;

        const char *rest = dict_lookup(&last, 1);
        if (rest) {
            emit_formatted(rest, &last, 1);
            state.stroke_count = 0;
        }
        return;
    }

    flush_strokes();
}

static void flush_strokes(void)
{
    LOG_DBG("Flushing %u untranslated strokes", state.stroke_count);
    state.stroke_count = 0;
}

static void multi_timeout_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (state.stroke_count == 0) {
        return;
    }

    const char *translation = dict_lookup(
        state.pending_strokes, state.stroke_count);

    if (translation) {
        emit_formatted(translation, state.pending_strokes, state.stroke_count);
    }
    state.stroke_count = 0;
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

    LOG_INF("steno press key=%u chord=0x%06X held=%u",
            key_index, state.current_chord, state.keys_held);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_steno_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    if (state.keys_held > 0) {
        state.keys_held--;
    }

    LOG_INF("steno release held=%u chord=0x%06X", state.keys_held, state.current_chord);

    if (state.keys_held == 0 && state.current_chord != 0) {
        LOG_INF("steno all-up → process chord 0x%06X", state.current_chord);
        process_chord();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_steno_init(const struct device *dev)
{
    ARG_UNUSED(dev);

    state.current_chord = 0;
    state.keys_held = 0;
    state.stroke_count = 0;

    steno_fmt_init(&fmt_state);
    steno_undo_init(&undo_history);
    k_work_init_delayable(&state.multi_timeout, multi_timeout_handler);

    size_t dict_size = _steno_dict_end - _steno_dict_start;
    if (dict_size > 4) {
        int ret;
#if IS_ENABLED(CONFIG_STENO_DICT_MPHF)
        ret = dict_mphf_init(&mphf_dict, _steno_dict_start, dict_size);
#else
        ret = steno_trie_init(_steno_dict_start, dict_size);
#endif
        if (ret == 0) {
            dict_ready = true;
            LOG_INF("Steno dict loaded (%u bytes)", (unsigned)dict_size);
        } else {
            LOG_ERR("Steno dict init failed: %d", ret);
        }
    } else {
        LOG_WRN("No steno dict embedded");
    }

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

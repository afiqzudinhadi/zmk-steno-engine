/*
 * Copyright (c) 2024 Afiq Zudin Hadi
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Plover formatting engine — parses translation strings containing
 * {commands} and produces clean output with spacing/capitalization.
 * No malloc — all stack/static.
 */

#include "formatter.h"
#include <string.h>

/* ── helpers ────────────────────────────────────────────────────── */

static inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static inline bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static inline char to_upper(char c) { return is_lower(c) ? (char)(c - 32) : c; }
static inline char to_lower(char c) { return is_upper(c) ? (char)(c + 32) : c; }

static bool str_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool starts_with(const char *s, size_t slen, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (slen < plen) return false;
    return str_eq(s, prefix, plen);
}

/* ── output buffer append ───────────────────────────────────────── */

static void emit_char(struct steno_fmt_result *r, char c)
{
    if (r->len < STENO_FMT_MAX_OUTPUT - 1) {
        r->text[r->len++] = c;
    }
}

/* ── apply capitalization/mode transforms to a text segment ───── */

static void emit_text_transformed(struct steno_fmt_state *state,
                                  struct steno_fmt_result *r,
                                  const char *text, size_t len)
{
    if (len == 0) return;

    for (size_t i = 0; i < len; i++) {
        char c = text[i];

        /* Mode transforms */
        if (state->mode == STENO_MODE_CAPS) {
            c = to_upper(c);
        } else if (state->mode == STENO_MODE_LOWER) {
            c = to_lower(c);
        } else if (state->mode == STENO_MODE_TITLE) {
            /* Capitalize first letter of each word */
            if (i == 0 || (i > 0 && text[i - 1] == ' ')) {
                c = to_upper(c);
            }
        }

        /* One-shot transforms (first char only) */
        if (i == 0) {
            if (state->cap_next) {
                c = to_upper(c);
                state->cap_next = false;
            }
            if (state->upper_next) {
                /* uppercase entire word — handled below */
            }
            if (state->lower_next) {
                /* lowercase entire word — handled below */
            }
        }

        /* upper_next: entire word */
        if (state->upper_next) {
            c = to_upper(c);
        }

        /* lower_next: entire word */
        if (state->lower_next) {
            c = to_lower(c);
        }

        emit_char(r, c);
    }

    state->upper_next = false;
    state->lower_next = false;
}

/* ── prepend space if needed ────────────────────────────────────── */

static void maybe_space(struct steno_fmt_state *state,
                        struct steno_fmt_result *r)
{
    if (state->space_pending && !state->suppress_space) {
        emit_char(r, ' ');
    }
    state->suppress_space = false;
}

/* ── process a single {…} command ──────────────────────────────── */

static void process_command(struct steno_fmt_state *state,
                            struct steno_fmt_result *r,
                            const char *cmd, size_t cmd_len)
{
    /* {^} — suppress space */
    if (cmd_len == 1 && cmd[0] == '^') {
        state->suppress_space = true;
        return;
    }

    /* {^suffix} — attach suffix */
    if (cmd_len > 1 && cmd[0] == '^') {
        state->suppress_space = true;
        maybe_space(state, r);
        emit_text_transformed(state, r, cmd + 1, cmd_len - 1);
        state->space_pending = true;
        state->glue = false;
        return;
    }

    /* {prefix^} — attach prefix */
    if (cmd_len > 1 && cmd[cmd_len - 1] == '^') {
        maybe_space(state, r);
        emit_text_transformed(state, r, cmd, cmd_len - 1);
        state->suppress_space = true;
        state->space_pending = false;
        state->glue = false;
        return;
    }

    /* {-|} — capitalize next */
    if (cmd_len == 2 && cmd[0] == '-' && cmd[1] == '|') {
        state->cap_next = true;
        return;
    }

    /* {~|} — carry capitalize */
    if (cmd_len == 2 && cmd[0] == '~' && cmd[1] == '|') {
        state->cap_next = true;
        return;
    }

    /* {*-|} — retro capitalize (backspace + re-emit) */
    if (cmd_len == 3 && cmd[0] == '*' && cmd[1] == '-' && cmd[2] == '|') {
        /* Signal retro-capitalize — simplified: set flag, caller handles */
        r->backspaces = 1;
        return;
    }

    /* {*!} — retro delete space */
    if (cmd_len == 2 && cmd[0] == '*' && cmd[1] == '!') {
        r->backspaces = 1;
        return;
    }

    /* {*?} — retro insert space */
    if (cmd_len == 2 && cmd[0] == '*' && cmd[1] == '?') {
        /* Would need undo context; simplified signal */
        return;
    }

    /* {*} — undo */
    if (cmd_len == 1 && cmd[0] == '*') {
        r->is_undo = true;
        r->is_command_only = true;
        return;
    }

    /* {.} {,} {?} {!} {;} {:} — punctuation: attach to prev, cap next for sentence-enders */
    if (cmd_len == 1 && (cmd[0] == '.' || cmd[0] == ',' ||
                         cmd[0] == '?' || cmd[0] == '!' ||
                         cmd[0] == ';' || cmd[0] == ':')) {
        state->suppress_space = true;
        maybe_space(state, r);
        emit_char(r, cmd[0]);
        state->space_pending = true;
        state->suppress_space = false;
        /* Sentence-ending punctuation capitalizes next */
        if (cmd[0] == '.' || cmd[0] == '?' || cmd[0] == '!') {
            state->cap_next = true;
        }
        state->glue = false;
        return;
    }

    /* {#...} — key combo (emit nothing for now, could extend) */
    if (cmd_len > 0 && cmd[0] == '#') {
        r->is_command_only = true;
        return;
    }

    /* {&letter} — fingerspelling */
    if (cmd_len >= 2 && cmd[0] == '&') {
        /* Glue: no space between glue strokes */
        if (state->glue) {
            state->suppress_space = true;
        }
        maybe_space(state, r);
        emit_text_transformed(state, r, cmd + 1, cmd_len - 1);
        state->space_pending = true;
        state->glue = true;
        state->suppress_space = false;
        return;
    }

    /* {&} — bare glue */
    if (cmd_len == 1 && cmd[0] == '&') {
        state->glue = true;
        state->suppress_space = true;
        return;
    }

    /* {MODE:CAPS} {MODE:TITLE} {MODE:LOWER} {MODE:RESET} */
    if (starts_with(cmd, cmd_len, "MODE:")) {
        const char *mode_str = cmd + 5;
        size_t mode_len = cmd_len - 5;
        if (mode_len == 4 && str_eq(mode_str, "CAPS", 4)) {
            state->mode = STENO_MODE_CAPS;
        } else if (mode_len == 5 && str_eq(mode_str, "TITLE", 5)) {
            state->mode = STENO_MODE_TITLE;
        } else if (mode_len == 5 && str_eq(mode_str, "LOWER", 5)) {
            state->mode = STENO_MODE_LOWER;
        } else if (mode_len == 5 && str_eq(mode_str, "RESET", 5)) {
            state->mode = STENO_MODE_NORMAL;
        }
        return;
    }

    /* {<} — uppercase next word */
    if (cmd_len == 1 && cmd[0] == '<') {
        state->upper_next = true;
        return;
    }

    /* {>} — lowercase next word */
    if (cmd_len == 1 && cmd[0] == '>') {
        state->lower_next = true;
        return;
    }
}

/* ── main entry point ──────────────────────────────────────────── */

void steno_fmt_init(struct steno_fmt_state *state)
{
    memset(state, 0, sizeof(*state));
    state->space_pending = false;
    state->cap_next = false;
    state->upper_next = false;
    state->lower_next = false;
    state->suppress_space = false;
    state->glue = false;
    state->mode = STENO_MODE_NORMAL;
}

void steno_fmt_process(struct steno_fmt_state *state,
                       const char *translation,
                       struct steno_fmt_result *result)
{
    memset(result, 0, sizeof(*result));

    if (!translation || !translation[0]) {
        result->is_command_only = true;
        return;
    }

    size_t tlen = strlen(translation);
    const char *p = translation;
    const char *end = translation + tlen;
    bool emitted_text = false;
    bool had_command = false;
    bool this_glue = false;

    while (p < end) {
        /* Escaped braces: \{ \} */
        if (p[0] == '\\' && p + 1 < end && (p[1] == '{' || p[1] == '}')) {
            maybe_space(state, result);
            emit_text_transformed(state, result, p + 1, 1);
            state->space_pending = true;
            state->glue = false;
            emitted_text = true;
            p += 2;
            continue;
        }

        /* Command: {…} */
        if (p[0] == '{') {
            const char *close = p + 1;
            while (close < end && *close != '}') {
                close++;
            }
            if (close < end) {
                const char *cmd = p + 1;
                size_t cmd_len = (size_t)(close - cmd);
                process_command(state, result, cmd, cmd_len);
                had_command = true;
                if (state->glue) this_glue = true;
                p = close + 1;
                continue;
            }
        }

        /* Literal text segment — find end (next '{' or '\' or end) */
        const char *seg_start = p;
        while (p < end && p[0] != '{' &&
               !(p[0] == '\\' && p + 1 < end && (p[1] == '{' || p[1] == '}'))) {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len > 0) {
            if (!this_glue) {
                maybe_space(state, result);
            }
            emit_text_transformed(state, result, seg_start, seg_len);
            state->space_pending = true;
            if (!this_glue) {
                state->glue = false;
            }
            emitted_text = true;
        }
    }

    result->text[result->len] = '\0';
    result->is_command_only = !emitted_text && had_command && result->len == 0;
}

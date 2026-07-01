#ifndef STENO_FORMATTER_H
#define STENO_FORMATTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Max output length from a single translation */
#define STENO_FMT_MAX_OUTPUT 128

/* Formatting mode */
enum steno_fmt_mode {
    STENO_MODE_NORMAL = 0,
    STENO_MODE_CAPS,
    STENO_MODE_TITLE,
    STENO_MODE_LOWER,
};

/* State persists between translations */
struct steno_fmt_state {
    bool space_pending;      /* insert space before next word */
    bool cap_next;           /* capitalize next word */
    bool upper_next;         /* uppercase next word */
    bool lower_next;         /* lowercase next word */
    bool suppress_space;     /* {^} suppress next space */
    bool glue;               /* in fingerspelling/glue sequence */
    enum steno_fmt_mode mode;
};

/* Result of formatting one translation */
struct steno_fmt_result {
    char text[STENO_FMT_MAX_OUTPUT];  /* output text to emit */
    uint8_t len;                       /* length of text */
    uint8_t backspaces;                /* backspaces to send BEFORE text */
    bool is_command_only;              /* true if no text output (pure command) */
    bool is_undo;                      /* true if this is an undo command */
};

void steno_fmt_init(struct steno_fmt_state *state);

/* Format a raw translation string. Updates state, fills result. */
void steno_fmt_process(struct steno_fmt_state *state,
                       const char *translation,
                       struct steno_fmt_result *result);

#endif

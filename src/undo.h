#ifndef STENO_UNDO_H
#define STENO_UNDO_H

#include <stdbool.h>
#include <stdint.h>

#ifndef CONFIG_STENO_HISTORY_SIZE
#define CONFIG_STENO_HISTORY_SIZE 100
#endif

#define STENO_MAX_MULTI_STROKE 8

struct stroke_history_entry {
    uint32_t strokes[STENO_MAX_MULTI_STROKE];
    uint8_t stroke_count;
    uint8_t output_len;       /* chars emitted (for backspace count) */
    uint8_t space_before;     /* 1 if space was prepended */
    uint8_t fmt_flags;        /* formatter state snapshot for restore */
};

struct stroke_history {
    struct stroke_history_entry entries[CONFIG_STENO_HISTORY_SIZE];
    uint16_t head;            /* next write position */
    uint16_t count;           /* entries in buffer */
};

void steno_undo_init(struct stroke_history *hist);

/* Push a new entry after successful output */
void steno_undo_push(struct stroke_history *hist,
                     const uint32_t *strokes, uint8_t stroke_count,
                     uint8_t output_len, uint8_t space_before,
                     uint8_t fmt_flags);

/* Pop most recent entry for undo. Returns NULL if empty. */
struct stroke_history_entry *steno_undo_pop(struct stroke_history *hist);

/* Peek at most recent entry without removing. Returns NULL if empty. */
const struct stroke_history_entry *steno_undo_peek(const struct stroke_history *hist);

/* Get current count */
uint16_t steno_undo_count(const struct stroke_history *hist);

#endif

#ifndef STENO_OUTPUT_H
#define STENO_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

void steno_output_send(const char *text, size_t len);

void steno_output_backspace(int count);

#endif

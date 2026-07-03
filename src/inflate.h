/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Minimal raw DEFLATE (RFC 1951) decompressor. Decompress-only, no
 * dynamic allocation. Used to inflate the dictionary string blocks
 * (raw deflate streams, zlib wbits=-15).
 */

#ifndef STENO_INFLATE_H
#define STENO_INFLATE_H

#include <stdint.h>
#include <stddef.h>

/*
 * Inflate a raw DEFLATE stream.
 * Returns the number of bytes written to dst (>= 0), or a negative
 * value on malformed input / dst overflow.
 */
int steno_inflate(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t dst_cap);

#endif /* STENO_INFLATE_H */

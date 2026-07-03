/*
 * Copyright (c) 2024 zmk-steno-engine contributors
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Minimal raw DEFLATE (RFC 1951) decompressor.
 */

#include "inflate.h"
#include <string.h>

#define MAX_BITS   15
#define MAX_LCODES 288
#define MAX_DCODES 30
#define MAX_CODES  (MAX_LCODES + MAX_DCODES)

struct bitstream {
    const uint8_t *src;
    size_t src_len;
    size_t pos;          /* byte position */
    uint32_t bitbuf;
    int bitcnt;
};

struct huffman {
    uint16_t count[MAX_BITS + 1]; /* codes per bit length */
    uint16_t symbol[MAX_LCODES];  /* symbols in canonical order */
};

static int bits(struct bitstream *s, int need, uint32_t *out)
{
    while (s->bitcnt < need) {
        if (s->pos >= s->src_len) {
            return -1;
        }
        s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    *out = s->bitbuf & ((1u << need) - 1);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return 0;
}

static int decode(struct bitstream *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= MAX_BITS; len++) {
        uint32_t bit;
        if (bits(s, 1, &bit) < 0) {
            return -1;
        }
        code |= (int)bit;
        int count = h->count[len];
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int construct(struct huffman *h, const uint8_t *lengths, int n)
{
    int offs[MAX_BITS + 1];

    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }
    if (h->count[0] == n) {
        return 0; /* no codes at all — legal, decode() will just fail */
    }

    int left = 1;
    for (int len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return -1; /* over-subscribed */
        }
    }

    offs[1] = 0;
    for (int len = 1; len < MAX_BITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    return 0;
}

static int codes(struct bitstream *s,
                 const struct huffman *lencode, const struct huffman *distcode,
                 uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    static const uint16_t lens_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t lens_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) {
            return -1;
        }
        if (sym < 256) {
            if (*dst_len >= dst_cap) {
                return -2;
            }
            dst[(*dst_len)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0; /* end of block */
        } else {
            sym -= 257;
            if (sym >= 29) {
                return -1;
            }
            uint32_t extra;
            if (bits(s, lens_extra[sym], &extra) < 0 && lens_extra[sym] > 0) {
                return -1;
            }
            if (lens_extra[sym] == 0) {
                extra = 0;
            }
            uint32_t length = lens_base[sym] + extra;

            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) {
                return -1;
            }
            uint32_t dextra = 0;
            if (dist_extra[dsym] > 0 && bits(s, dist_extra[dsym], &dextra) < 0) {
                return -1;
            }
            uint32_t dist = dist_base[dsym] + dextra;

            if (dist > *dst_len) {
                return -1; /* distance beyond output start */
            }
            if (*dst_len + length > dst_cap) {
                return -2;
            }
            uint8_t *out = dst + *dst_len;
            const uint8_t *from = out - dist;
            for (uint32_t i = 0; i < length; i++) {
                out[i] = from[i];
            }
            *dst_len += length;
        }
    }
}

static int fixed_tables(struct huffman *lencode, struct huffman *distcode)
{
    uint8_t lengths[MAX_LCODES];

    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    if (construct(lencode, lengths, 288) < 0) {
        return -1;
    }

    for (int i = 0; i < 30; i++) lengths[i] = 5;
    return construct(distcode, lengths, 30);
}

static int dynamic_tables(struct bitstream *s,
                          struct huffman *lencode, struct huffman *distcode)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t lengths[MAX_CODES];
    uint32_t hlit, hdist, hclen;

    if (bits(s, 5, &hlit) < 0 || bits(s, 5, &hdist) < 0 ||
        bits(s, 4, &hclen) < 0) {
        return -1;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > MAX_LCODES || hdist > MAX_DCODES) {
        return -1;
    }

    memset(lengths, 0, 19);
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v;
        if (bits(s, 3, &v) < 0) {
            return -1;
        }
        lengths[order[i]] = (uint8_t)v;
    }

    struct huffman clcode;
    if (construct(&clcode, lengths, 19) < 0) {
        return -1;
    }

    uint32_t idx = 0;
    while (idx < hlit + hdist) {
        int sym = decode(s, &clcode);
        if (sym < 0) {
            return -1;
        }
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else {
            uint8_t repeat_val = 0;
            uint32_t repeat, v;
            if (sym == 16) {
                if (idx == 0 || bits(s, 2, &v) < 0) {
                    return -1;
                }
                repeat_val = lengths[idx - 1];
                repeat = 3 + v;
            } else if (sym == 17) {
                if (bits(s, 3, &v) < 0) {
                    return -1;
                }
                repeat = 3 + v;
            } else {
                if (bits(s, 7, &v) < 0) {
                    return -1;
                }
                repeat = 11 + v;
            }
            if (idx + repeat > hlit + hdist) {
                return -1;
            }
            while (repeat--) {
                lengths[idx++] = repeat_val;
            }
        }
    }

    if (lengths[256] == 0) {
        return -1; /* end-of-block code must exist */
    }
    if (construct(lencode, lengths, (int)hlit) < 0) {
        return -1;
    }
    return construct(distcode, lengths + hlit, (int)hdist);
}

int steno_inflate(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t dst_cap)
{
    struct bitstream s = { .src = src, .src_len = src_len };
    size_t dst_len = 0;
    uint32_t last, type;

    do {
        if (bits(&s, 1, &last) < 0 || bits(&s, 2, &type) < 0) {
            return -1;
        }

        if (type == 0) {
            /* stored block: discard remaining bits, read LEN/NLEN */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > s.src_len) {
                return -1;
            }
            uint32_t len = s.src[s.pos] | ((uint32_t)s.src[s.pos + 1] << 8);
            uint32_t nlen = s.src[s.pos + 2] | ((uint32_t)s.src[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.pos + len > s.src_len) {
                return -1;
            }
            if (dst_len + len > dst_cap) {
                return -2;
            }
            memcpy(dst + dst_len, src + s.pos, len);
            dst_len += len;
            s.pos += len;
        } else if (type == 1 || type == 2) {
            struct huffman lencode, distcode;
            int ret = (type == 1) ? fixed_tables(&lencode, &distcode)
                                  : dynamic_tables(&s, &lencode, &distcode);
            if (ret < 0) {
                return -1;
            }
            ret = codes(&s, &lencode, &distcode, dst, dst_cap, &dst_len);
            if (ret < 0) {
                return ret;
            }
        } else {
            return -1;
        }
    } while (!last);

    return (int)dst_len;
}

/*
 * Copyright (c) 2024 Afiq Zudin Hadi
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Standard steno key layout. Each key = bit position in uint32_t chord.
 * Bit assignments match the compiler's STENO_KEYS bitmask exactly.
 *
 * Layout (standard steno order):
 *   S  T  K  P  W  H  R  A  O  *  E  U  F  R  P  B  L  G  T  S  D  Z  #
 *   0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20 21 22
 */

#ifndef DT_BINDINGS_ZMK_STENO_KEYS_H_
#define DT_BINDINGS_ZMK_STENO_KEYS_H_

/* Left hand consonants */
#define STENO_SL     0   /* S- (left) */
#define STENO_TL     1   /* T- */
#define STENO_KL     2   /* K- */
#define STENO_PL     3   /* P- */
#define STENO_WL     4   /* W- */
#define STENO_HL     5   /* H- */
#define STENO_RL     6   /* R- (left) */

/* Vowels */
#define STENO_A      7   /* A */
#define STENO_O      8   /* O */

/* Center */
#define STENO_STAR   9   /* * */

/* Vowels (right) */
#define STENO_E     10   /* E */
#define STENO_U     11   /* U */

/* Right hand consonants */
#define STENO_FR    12   /* -F */
#define STENO_RR    13   /* -R (right) */
#define STENO_PR    14   /* -P (right) */
#define STENO_BR    15   /* -B */
#define STENO_LR    16   /* -L */
#define STENO_GR    17   /* -G */
#define STENO_TR    18   /* -T (right) */
#define STENO_SR    19   /* -S (right) */
#define STENO_DR    20   /* -D */
#define STENO_ZR    21   /* -Z */

/* Number bar */
#define STENO_NUM   22   /* # */

#define STENO_KEY_COUNT 23

#endif /* DT_BINDINGS_ZMK_STENO_KEYS_H_ */

#!/usr/bin/env python3
# Copyright (c) 2024 zmk-steno-engine contributors
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Licensed under the PolyForm Noncommercial License 1.0.0;
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# https://polyformproject.org/licenses/noncommercial/1.0.0
"""Steno dictionary v4 "Union Split-Section" compiler.

Implements docs/FORMAT_V4.md byte-exactly:

- Union CHD MPHF (displacement escape, D_THRESHOLD=32768) over both dicts.
- Canonical-Huffman-coded displacements (MSB-first) with a skip table.
- MEMBERSHIP / VALIDX / FP packed LSB-first; VALIDX split across the two
  half blobs at slot k (smallest k such that the right blob fits).
- Shared string table: sorted unique translations, front-coded 16KB blocks,
  raw deflate (wbits=-15, level 9).
- Mandatory verification: cold-reopen both blobs and run the full decoder
  path for every entry of both source dicts, byte-exact, plus 10,000
  random unknown-key probes.

Entries never get trimmed. Anything that does not fit is a hard error.
"""

import argparse
import heapq
import json
import math
import os
import random
import struct
import sys
import zlib
from collections import Counter, defaultdict

try:
    import numpy as np
except ImportError:
    np = None

# ─── Format constants (FORMAT_V4.md) ───

MAGIC = 0x344E5453          # "STN4"
VERSION = 4
D_THRESHOLD = 32768
SINGLETON_TRIES = 1000
FP_BITS = 4
VALIDX_BITS = 17
FC_BLOCK_BOUND = 16384
# FORMAT_V4.md section 1: spec'd 8, but longer entries exist (plover has 10
# entries up to 11 strokes) and the spec mandates "raise max, never drop".
MAX_ENTRY_STROKES = 11
MAX_TRANSLATION_BYTES = 255
MAX_N = 1 << 18             # slot must fit 18 bits
MAX_STRING_ID = 1 << 17     # string_id must fit 17 bits
DISP_CLASSES = 19

SEC_DISP = 1
SEC_MEMBERSHIP = 2
SEC_VALIDX = 3
SEC_CONFLICTS = 4
SEC_FP = 5
SEC_STRDIR = 6
SEC_STRINGS = 7

SEC_NAMES = {
    SEC_DISP: 'DISP',
    SEC_MEMBERSHIP: 'MEMBERSHIP',
    SEC_VALIDX: 'VALIDX',
    SEC_CONFLICTS: 'CONFLICTS',
    SEC_FP: 'FP',
    SEC_STRDIR: 'STRDIR',
    SEC_STRINGS: 'STRINGS',
}

FNV_PRIME = 0x01000193
FNV_OFFSET = 0x811c9dc5
MASK32 = 0xFFFFFFFF

DICT_PLOVER = 0
DICT_LAPWING = 1

# ─── Steno stroke parsing (identical to compile_mphf.py) ───

STENO_KEYS = {
    '#': 0x00400000,
    'S-': 0x00000001, 'T-': 0x00000002, 'K-': 0x00000004,
    'P-': 0x00000008, 'W-': 0x00000010, 'H-': 0x00000020,
    'R-': 0x00000040, 'A-': 0x00000080, 'O-': 0x00000100,
    '*': 0x00000200, '-E': 0x00000400, '-U': 0x00000800,
    '-F': 0x00001000, '-R': 0x00002000, '-P': 0x00004000,
    '-B': 0x00008000, '-L': 0x00010000, '-G': 0x00020000,
    '-T': 0x00040000, '-S': 0x00080000, '-D': 0x00100000,
    '-Z': 0x00200000,
}

IMPLICIT_HYPHEN = set('AOEU*')


def parse_stroke(s):
    result = 0
    if '#' in s:
        result |= STENO_KEYS['#']
        s = s.replace('#', '')
    has_hyphen = '-' in s
    s_clean = s.replace('-', '')
    if not has_hyphen and not any(c in IMPLICIT_HYPHEN for c in s_clean):
        for c in s_clean:
            key = c + '-'
            if key in STENO_KEYS:
                result |= STENO_KEYS[key]
        return result
    if has_hyphen:
        hyphen_pos = s.index('-')
        for i, c in enumerate(s):
            if c == '-':
                continue
            if c in 'AO':
                result |= STENO_KEYS[c + '-']
            elif c in 'EU':
                result |= STENO_KEYS['-' + c]
            elif c == '*':
                result |= STENO_KEYS['*']
            elif i < hyphen_pos and (c + '-') in STENO_KEYS:
                result |= STENO_KEYS[c + '-']
            elif i > hyphen_pos and ('-' + c) in STENO_KEYS:
                result |= STENO_KEYS['-' + c]
    else:
        past_vowels = False
        for c in s_clean:
            if c in 'AO':
                result |= STENO_KEYS[c + '-']
                past_vowels = True
            elif c in 'EU':
                result |= STENO_KEYS['-' + c]
                past_vowels = True
            elif c == '*':
                result |= STENO_KEYS['*']
                past_vowels = True
            elif not past_vowels and (c + '-') in STENO_KEYS:
                result |= STENO_KEYS[c + '-']
            elif past_vowels and ('-' + c) in STENO_KEYS:
                result |= STENO_KEYS['-' + c]
            elif (c + '-') in STENO_KEYS:
                result |= STENO_KEYS[c + '-']
    return result


def encode_key(stroke_str):
    """Parse stroke string → (strokes tuple, key_bytes: u32 LE per stroke)."""
    parts = stroke_str.split('/')
    strokes = tuple(parse_stroke(s) for s in parts)
    key_bytes = b''.join(struct.pack('<I', s) for s in strokes)
    return strokes, key_bytes


# ─── Hashing (identical to compile_mphf.py) ───

def fnv1a_32(data):
    """FNV-1a 32-bit hash."""
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK32
    return h


def hash_key(key_bytes, seed):
    """Hash key with seed by prepending seed bytes."""
    return fnv1a_32(struct.pack('<I', seed) + key_bytes)


# ─── Bit packing ───

class BitWriter:
    """LSB-first bit writer (v2-compatible)."""

    def __init__(self):
        self.data = bytearray()
        self.bit_pos = 0

    def write_bits(self, value, n_bits):
        for i in range(n_bits):
            if self.bit_pos % 8 == 0:
                self.data.append(0)
            if value & (1 << i):
                self.data[-1] |= (1 << (self.bit_pos % 8))
            self.bit_pos += 1


class MSBWriter:
    """MSB-first bit writer (DISP Huffman stream only)."""

    def __init__(self):
        self.data = bytearray()
        self.bit_pos = 0

    def write_bits(self, value, n_bits):
        for i in range(n_bits - 1, -1, -1):
            if self.bit_pos % 8 == 0:
                self.data.append(0)
            if (value >> i) & 1:
                self.data[-1] |= (0x80 >> (self.bit_pos % 8))
            self.bit_pos += 1


class MSBReader:
    """MSB-first bit reader (DISP Huffman stream only)."""

    def __init__(self, data):
        self.data = data
        self.bit_pos = 0

    def read_bit(self):
        b = (self.data[self.bit_pos >> 3] >> (7 - (self.bit_pos & 7))) & 1
        self.bit_pos += 1
        return b

    def read_bits(self, n_bits):
        v = 0
        for _ in range(n_bits):
            v = (v << 1) | self.read_bit()
        return v


def pack_lsb(values, bits):
    """Pack fixed-width values LSB-first. Returns exactly ceil(m*bits/8) bytes."""
    m = len(values)
    n_bytes = (m * bits + 7) // 8
    if m == 0:
        return b''
    if np is not None:
        v = np.asarray(values, dtype=np.uint64)
        offs = np.arange(m, dtype=np.uint64) * np.uint64(bits)
        byte_idx = (offs >> np.uint64(3)).astype(np.int64)
        shift = offs & np.uint64(7)
        shifted = v << shift
        buf = np.zeros(n_bytes + 8, dtype=np.uint8)
        span = (bits + 14) // 8
        for b in range(span):
            np.bitwise_or.at(
                buf, byte_idx + b,
                ((shifted >> np.uint64(8 * b)) & np.uint64(0xFF)).astype(np.uint8))
        return bytes(buf[:n_bytes])
    w = BitWriter()
    for v in values:
        w.write_bits(v, bits)
    return bytes(w.data[:n_bytes])


def read_lsb_at(section, idx, bits):
    """Scalar LSB-first fixed-width read (decoder reference path)."""
    off = idx * bits
    byte_idx = off >> 3
    shift = off & 7
    window = section[byte_idx:byte_idx + 4] + b'\x00' * 4
    acc = int.from_bytes(window[:4], 'little')
    return (acc >> shift) & ((1 << bits) - 1)


def read_lsb_many(section, idxs, bits):
    """Vectorized LSB-first fixed-width reads; same bit math as read_lsb_at."""
    if np is None or not hasattr(idxs, 'dtype'):
        return [read_lsb_at(section, int(i), bits) for i in idxs]
    if len(idxs) == 0:
        return np.zeros(0, dtype=np.int64)
    buf = np.frombuffer(bytes(section) + b'\x00' * 8, dtype=np.uint8)
    offs = idxs.astype(np.uint64) * np.uint64(bits)
    byte_idx = (offs >> np.uint64(3)).astype(np.int64)
    shift = offs & np.uint64(7)
    acc = np.zeros(len(idxs), dtype=np.uint64)
    span = (bits + 14) // 8
    for b in range(span):
        acc |= buf[byte_idx + b].astype(np.uint64) << np.uint64(8 * b)
    return ((acc >> shift) & np.uint64((1 << bits) - 1)).astype(np.int64)


# ─── Vectorized FNV-1a helpers ───

def _fold_mat(h, mat):
    for j in range(mat.shape[1]):
        h = ((h ^ mat[:, j]) * FNV_PRIME) & MASK32
    return h


def _fold_seeds(seeds):
    h = np.full(seeds.shape, FNV_OFFSET, dtype=np.uint64)
    for shift in (0, 8, 16, 24):
        h = ((h ^ ((seeds >> np.uint64(shift)) & np.uint64(0xFF))) * FNV_PRIME) & MASK32
    return h


def _group_by_len(keys, idxs=None):
    by_len = defaultdict(list)
    if idxs is None:
        for i, kb in enumerate(keys):
            by_len[len(kb)].append(i)
    else:
        for i in idxs:
            by_len[len(keys[i])].append(int(i))
    return by_len


def _key_matrix(keys, idxs, length):
    return np.frombuffer(b''.join(keys[i] for i in idxs), dtype=np.uint8) \
             .reshape(len(idxs), length).astype(np.uint64)


def hash0_all(keys):
    """hash_key(kb, 0) for all keys."""
    if np is None:
        return [hash_key(kb, 0) for kb in keys]
    out = np.zeros(len(keys), dtype=np.uint64)
    for length, idxs in _group_by_len(keys).items():
        ia = np.array(idxs, dtype=np.int64)
        h = np.full(len(idxs), FNV_OFFSET, dtype=np.uint64)
        for _ in range(4):  # seed 0 → four 0x00 bytes
            h = (h * FNV_PRIME) & MASK32
        out[ia] = _fold_mat(h, _key_matrix(keys, idxs, length))
    return out


def fp_all(keys):
    """fnv1a(kb) & 0xF for all keys."""
    if np is None:
        return [fnv1a_32(kb) & 0xF for kb in keys]
    out = np.zeros(len(keys), dtype=np.int64)
    for length, idxs in _group_by_len(keys).items():
        ia = np.array(idxs, dtype=np.int64)
        h = np.full(len(idxs), FNV_OFFSET, dtype=np.uint64)
        h = _fold_mat(h, _key_matrix(keys, idxs, length))
        out[ia] = (h & np.uint64(0xF)).astype(np.int64)
    return out


def compute_slots(keys, disp, bucket_count, n):
    """Decoder slot computation for a list of keys against a disp array."""
    if np is None:
        slots = []
        for kb in keys:
            b = hash_key(kb, 0) % bucket_count
            d = disp[b]
            if d >= D_THRESHOLD:
                slots.append(d - D_THRESHOLD)
            else:
                slots.append(hash_key(kb, d + 1) % n)
        return slots
    disp_np = np.asarray(disp, dtype=np.int64)
    h0 = hash0_all(keys)
    bucket = (h0 % np.uint64(bucket_count)).astype(np.int64)
    d = disp_np[bucket]
    slot = np.empty(len(keys), dtype=np.int64)
    direct = d >= D_THRESHOLD
    slot[direct] = d[direct] - D_THRESHOLD
    nd = np.nonzero(~direct)[0]
    for length, idxs in _group_by_len(keys, nd).items():
        ia = np.array(idxs, dtype=np.int64)
        seeds = (d[ia] + 1).astype(np.uint64)
        h = _fold_seeds(seeds)
        h = _fold_mat(h, _key_matrix(keys, idxs, length))
        slot[ia] = (h % np.uint64(n)).astype(np.int64)
    return slot


# ─── Dictionary loading ───

def load_dict(path, name, quiet=False):
    """Load a Plover-format JSON dict.

    Returns (raw_entries, kb_map, offenders) where:
      raw_entries = [(stroke_str, key_bytes)] for every JSON entry
      kb_map      = {key_bytes: (translation_bytes, stroke_str, n_strokes)},
                    FIRST occurrence wins for duplicate key bytes
      offenders   = list of hard-error strings (too many strokes / too long)
    """
    with open(path) as f:
        raw = json.load(f)
    raw_entries = []
    kb_map = {}
    offenders = []
    for stroke_str, translation in raw.items():
        strokes, kb = encode_key(stroke_str)
        raw_entries.append((stroke_str, kb))
        if len(strokes) > MAX_ENTRY_STROKES:
            offenders.append(f"{name}: '{stroke_str}' has {len(strokes)} strokes "
                             f"(max {MAX_ENTRY_STROKES})")
        tb = translation.encode('utf-8')
        if len(tb) > MAX_TRANSLATION_BYTES:
            offenders.append(f"{name}: '{stroke_str}' translation is {len(tb)} bytes "
                             f"(max {MAX_TRANSLATION_BYTES})")
        if kb in kb_map:
            prev_tb, prev_stroke, _ = kb_map[kb]
            if not quiet:
                print(f"  dup key ({name}): '{stroke_str}' → "
                      f"{translation!r} loses to '{prev_stroke}' → "
                      f"{prev_tb.decode('utf-8', 'replace')!r} (first wins)",
                      file=sys.stderr)
            continue
        kb_map[kb] = (tb, stroke_str, len(strokes))
    return raw_entries, kb_map, offenders


# ─── CHD MPHF construction ───

def build_chd_np(keys, bucket_count, n):
    """Numpy-vectorized CHD with displacement escape.

    Multi-key buckets search d in [0, D_THRESHOLD).
    Single-key buckets search d in [0, SINGLETON_TRIES), then direct-place
    into the lowest free slot (disp = D_THRESHOLD + slot).
    Returns (disp list, slot_of_key list, stats dict); hard error on failure.
    """
    h0 = hash0_all(keys)
    seed_hashes = _fold_seeds(np.arange(1, D_THRESHOLD + 1, dtype=np.uint64))

    bucket_of = (h0 % np.uint64(bucket_count)).astype(np.int64)
    order = np.argsort(bucket_of, kind='stable')
    sorted_buckets = bucket_of[order]
    uniq, starts = np.unique(sorted_buckets, return_index=True)
    groups = []
    for gi in range(len(uniq)):
        s = starts[gi]
        e = starts[gi + 1] if gi + 1 < len(uniq) else n
        groups.append((int(uniq[gi]), order[s:e]))
    groups.sort(key=lambda g: -len(g[1]))

    free = np.ones(n, dtype=bool)
    slot_of_key = np.full(n, -1, dtype=np.int64)
    disp = np.zeros(bucket_count, dtype=np.int64)
    max_hash_disp = 0
    direct_count = 0
    free_ptr = 0

    def chunks_multi():
        yield (0, 512)
        s = 512
        while s < D_THRESHOLD:
            e = min(s + 8192, D_THRESHOLD)
            yield (s, e)
            s = e

    for bucket_id, members in groups:
        m = len(members)
        kbs = [keys[i] for i in members]
        if m == 1:
            kb = kbs[0]
            h = seed_hashes[:SINGLETON_TRIES].copy()
            for b in kb:
                h = ((h ^ b) * FNV_PRIME) & MASK32
            slots = h % np.uint64(n)
            hit = np.nonzero(free[slots])[0]
            if hit.size:
                d = int(hit[0])
                slot = int(slots[hit[0]])
                free[slot] = False
                slot_of_key[members[0]] = slot
                disp[bucket_id] = d
                if d > max_hash_disp:
                    max_hash_disp = d
            else:
                while not free[free_ptr]:
                    free_ptr += 1
                slot = free_ptr
                free[slot] = False
                slot_of_key[members[0]] = slot
                disp[bucket_id] = D_THRESHOLD + slot
                direct_count += 1
            continue

        placed = False
        for cs, ce in chunks_multi():
            base = seed_hashes[cs:ce]
            slot_rows = []
            for kb in kbs:
                h = base.copy()
                for b in kb:
                    h = ((h ^ b) * FNV_PRIME) & MASK32
                slot_rows.append(h % np.uint64(n))
            slots = np.stack(slot_rows)
            ok = free[slots].all(axis=0)
            for i in range(m):
                for j in range(i + 1, m):
                    ok &= slots[i] != slots[j]
            hit = np.nonzero(ok)[0]
            if hit.size:
                d = cs + int(hit[0])
                chosen = slots[:, hit[0]]
                free[chosen] = False
                for i, mi in enumerate(members):
                    slot_of_key[mi] = int(chosen[i])
                disp[bucket_id] = d
                if d > max_hash_disp:
                    max_hash_disp = d
                placed = True
                break
        if not placed:
            print(f"FATAL: CHD bucket {bucket_id} ({m} keys) found no displacement "
                  f"in [0, {D_THRESHOLD})", file=sys.stderr)
            sys.exit(1)

    if not (slot_of_key >= 0).all():
        print("FATAL: CHD left keys unplaced", file=sys.stderr)
        sys.exit(1)
    stats = {'max_hash_disp': int(max_hash_disp), 'direct_count': int(direct_count)}
    return [int(v) for v in disp], [int(s) for s in slot_of_key], stats


def build_chd_py(keys, bucket_count, n):
    """Pure-python CHD fallback (same algorithm, no numpy)."""
    buckets = defaultdict(list)
    for idx, kb in enumerate(keys):
        buckets[hash_key(kb, 0) % bucket_count].append(idx)
    groups = sorted(buckets.items(), key=lambda g: -len(g[1]))

    free = [True] * n
    slot_of_key = [-1] * n
    disp = [0] * bucket_count
    max_hash_disp = 0
    direct_count = 0
    free_ptr = 0

    for bucket_id, members in groups:
        m = len(members)
        kbs = [keys[i] for i in members]
        if m == 1:
            kb = kbs[0]
            placed = False
            for d in range(SINGLETON_TRIES):
                slot = hash_key(kb, d + 1) % n
                if free[slot]:
                    free[slot] = False
                    slot_of_key[members[0]] = slot
                    disp[bucket_id] = d
                    max_hash_disp = max(max_hash_disp, d)
                    placed = True
                    break
            if not placed:
                while not free[free_ptr]:
                    free_ptr += 1
                free[free_ptr] = False
                slot_of_key[members[0]] = free_ptr
                disp[bucket_id] = D_THRESHOLD + free_ptr
                direct_count += 1
            continue

        placed = False
        for d in range(D_THRESHOLD):
            slots = []
            seen = set()
            ok = True
            for kb in kbs:
                slot = hash_key(kb, d + 1) % n
                if not free[slot] or slot in seen:
                    ok = False
                    break
                seen.add(slot)
                slots.append(slot)
            if not ok:
                continue
            for i, mi in enumerate(members):
                free[slots[i]] = False
                slot_of_key[mi] = slots[i]
            disp[bucket_id] = d
            max_hash_disp = max(max_hash_disp, d)
            placed = True
            break
        if not placed:
            print(f"FATAL: CHD bucket {bucket_id} ({m} keys) found no displacement "
                  f"in [0, {D_THRESHOLD})", file=sys.stderr)
            sys.exit(1)

    if any(s < 0 for s in slot_of_key):
        print("FATAL: CHD left keys unplaced", file=sys.stderr)
        sys.exit(1)
    stats = {'max_hash_disp': max_hash_disp, 'direct_count': direct_count}
    return disp, slot_of_key, stats


def build_chd(keys, bucket_count, n):
    if np is not None:
        return build_chd_np(keys, bucket_count, n)
    return build_chd_py(keys, bucket_count, n)


# ─── Canonical Huffman (DISP classes) ───

def huffman_lengths(freq):
    """freq: {symbol: count} → {symbol: code length}."""
    items = sorted(freq.items())
    if len(items) == 1:
        return {items[0][0]: 1}
    heap = []
    uid = 0
    for sym, count in items:
        heapq.heappush(heap, (count, uid, ('leaf', sym)))
        uid += 1
    while len(heap) > 1:
        c1, _, t1 = heapq.heappop(heap)
        c2, _, t2 = heapq.heappop(heap)
        heapq.heappush(heap, (c1 + c2, uid, ('node', t1, t2)))
        uid += 1
    lengths = {}
    stack = [(heap[0][2], 0)]
    while stack:
        node, depth = stack.pop()
        if node[0] == 'leaf':
            lengths[node[1]] = depth
        else:
            stack.append((node[1], depth + 1))
            stack.append((node[2], depth + 1))
    return lengths


def canonical_codes(lengths):
    """Canonical codes in (code_len, symbol) order, numerically increasing,
    MSB-first. Returns {symbol: (code, length)}."""
    syms = sorted(lengths, key=lambda s: (lengths[s], s))
    codes = {}
    code = 0
    prev_len = None
    for s in syms:
        length = lengths[s]
        if prev_len is not None:
            code = (code + 1) << (length - prev_len)
        codes[s] = (code, length)
        prev_len = length
    return codes


def disp_class(v):
    return v.bit_length()


def build_disp_section(disp, bucket_count):
    """DISP section: code_len[19], skip table, MSB-first Huffman stream."""
    classes = [disp_class(v) for v in disp]
    max_class = max(classes)
    if max_class >= DISP_CLASSES:
        print(f"FATAL: displacement class {max_class} out of range "
              f"(max {DISP_CLASSES - 1})", file=sys.stderr)
        sys.exit(1)
    lengths = huffman_lengths(Counter(classes))
    codes = canonical_codes(lengths)

    w = MSBWriter()
    skips = []
    for i, v in enumerate(disp):
        if i % 256 == 0:
            skips.append(w.bit_pos)
        c = classes[i]
        code, length = codes[c]
        w.write_bits(code, length)
        if c >= 2:
            w.write_bits(v - (1 << (c - 1)), c - 1)

    assert len(skips) == (bucket_count + 255) // 256
    code_len_bytes = bytes(lengths.get(c, 0) for c in range(DISP_CLASSES))
    section = (code_len_bytes
               + struct.pack('<I', len(skips))
               + b''.join(struct.pack('<I', s) for s in skips)
               + bytes(w.data))
    return section


def parse_disp_section(section, bucket_count):
    """Decode DISP: returns (disp list, skips, stream, codemap).
    Verifies the skip table against sequentially decoded bit offsets."""
    code_len = section[:DISP_CLASSES]
    (skip_count,) = struct.unpack_from('<I', section, DISP_CLASSES)
    skips = list(struct.unpack_from(f'<{skip_count}I', section, DISP_CLASSES + 4))
    stream = section[DISP_CLASSES + 4 + 4 * skip_count:]

    lengths = {c: code_len[c] for c in range(DISP_CLASSES) if code_len[c]}
    codes = canonical_codes(lengths)
    codemap = {(length, code): sym for sym, (code, length) in codes.items()}

    r = MSBReader(stream)
    disp = []
    offsets = []
    for _ in range(bucket_count):
        offsets.append(r.bit_pos)
        disp.append(decode_disp_value(r, codemap))

    expected_skips = (bucket_count + 255) // 256
    if skip_count != expected_skips:
        raise ValueError(f"DISP skip_count {skip_count} != {expected_skips}")
    for i in range(skip_count):
        if skips[i] != offsets[i * 256]:
            raise ValueError(f"DISP skip[{i}] = {skips[i]} != "
                             f"decoded offset {offsets[i * 256]}")
    return disp, skips, stream, codemap


def decode_disp_value(reader, codemap):
    code = 0
    length = 0
    while True:
        code = (code << 1) | reader.read_bit()
        length += 1
        sym = codemap.get((length, code))
        if sym is not None:
            break
        if length > 24:
            raise ValueError("DISP Huffman decode ran away")
    if sym <= 1:
        return sym
    return (1 << (sym - 1)) | reader.read_bits(sym - 1)


def disp_random_access(stream, codemap, skips, bucket):
    """Random access decode of bucket's displacement via the skip table."""
    r = MSBReader(stream)
    r.bit_pos = skips[bucket >> 8]
    for _ in range(bucket & 255):
        decode_disp_value(r, codemap)
    return decode_disp_value(r, codemap)


# ─── String table (front-coded deflate blocks) ───

def common_prefix_len(a, b):
    m = min(len(a), len(b), 255)
    i = 0
    while i < m and a[i] == b[i]:
        i += 1
    return i


def build_string_sections(strings):
    """strings: sorted unique translation bytes.
    Returns (strdir, strings_section, block_count, raw_fc_bytes)."""
    blocks = []          # (first_string_id, raw_fc_bytes)
    cur = bytearray()
    cur_first = 0
    prev = None
    for sid, s in enumerate(strings):
        if prev is None:
            enc = bytes([0]) + s + b'\x00'
        else:
            p = common_prefix_len(prev, s)
            enc = bytes([p]) + s[p:] + b'\x00'
            if len(cur) + len(enc) > FC_BLOCK_BOUND:
                blocks.append((cur_first, bytes(cur)))
                cur = bytearray()
                cur_first = sid
                prev = None
                enc = bytes([0]) + s + b'\x00'
        cur += enc
        prev = s
    if cur:
        blocks.append((cur_first, bytes(cur)))

    comp_blocks = []
    raw_total = 0
    for first_id, raw in blocks:
        assert len(raw) <= FC_BLOCK_BOUND
        raw_total += len(raw)
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        comp_blocks.append(co.compress(raw) + co.flush())

    strdir = struct.pack('<I', len(blocks))
    off = 0
    for (first_id, _), comp in zip(blocks, comp_blocks):
        strdir += struct.pack('<II', off, first_id)
        off += len(comp)
    return strdir, b''.join(comp_blocks), len(blocks), raw_total


def fc_decode_block(raw):
    """Walk a front-coded block, returning the list of strings."""
    out = []
    prev = b''
    i = 0
    n = len(raw)
    while i < n:
        plen = raw[i]
        i += 1
        j = raw.index(0, i)
        prev = prev[:plen] + raw[i:j]
        out.append(prev)
        i = j + 1
    return out


class StringTable:
    """Decoder-path string lookup: STRDIR binary search → inflate → FC walk."""

    def __init__(self, strdir, strings_section, block_count_param):
        (self.block_count,) = struct.unpack_from('<I', strdir, 0)
        if self.block_count != block_count_param:
            raise ValueError("STRDIR block_count != directory param")
        self.entries = []
        for i in range(self.block_count):
            comp_off, first_id = struct.unpack_from('<II', strdir, 4 + 8 * i)
            self.entries.append((comp_off, first_id))
        self.strings_section = strings_section
        self._cache = {}

    def _block(self, idx):
        if idx in self._cache:
            return self._cache[idx]
        comp_off, _ = self.entries[idx]
        if idx + 1 < self.block_count:
            comp_end = self.entries[idx + 1][0]
        else:
            comp_end = len(self.strings_section)
        raw = zlib.decompress(self.strings_section[comp_off:comp_end], -15)
        decoded = fc_decode_block(raw)
        self._cache[idx] = decoded
        return decoded

    def get(self, string_id):
        lo, hi = 0, self.block_count - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.entries[mid][1] <= string_id:
                lo = mid
            else:
                hi = mid - 1
        block = self._block(lo)
        return block[string_id - self.entries[lo][1]]


# ─── Blob assembly ───

def pad4(data):
    return data + b'\x00' * ((-len(data)) % 4)


def blob_total_size(section_lens):
    off = 32 + 16 * len(section_lens)
    for length in section_lens:
        off += (-off) % 4
        off += length
    return off


def assemble_blob(sections, n, bucket_count, string_count, conflict_count,
                  dicts_mask, max_entry_strokes):
    """sections: list of (type, data, param). Returns blob bytes."""
    sc = len(sections)
    header = struct.pack('<IHHIIIIIBBBB',
                         MAGIC, VERSION, sc,
                         n, bucket_count, string_count, conflict_count,
                         D_THRESHOLD, FP_BITS, dicts_mask,
                         max_entry_strokes, 0)
    assert len(header) == 32
    directory = b''
    payload = bytearray()
    off = 32 + 16 * sc
    for sec_type, data, param in sections:
        pad = (-off) % 4
        payload += b'\x00' * pad
        off += pad
        directory += struct.pack('<BBHIII', sec_type, 0, 0, off, len(data), param)
        payload += data
        off += len(data)
    return header + directory + bytes(payload)


def parse_blob(path):
    """Cold-reopen a half blob. Returns (header dict, [(type, data, param)])."""
    with open(path, 'rb') as f:
        data = f.read()
    (magic, version, sc, n, bucket_count, string_count, conflict_count,
     d_threshold, fp_bits, dicts_mask, max_strokes, _rsvd) = \
        struct.unpack_from('<IHHIIIIIBBBB', data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError(f"{path}: bad magic/version")
    header = {
        'n': n, 'bucket_count': bucket_count, 'string_count': string_count,
        'conflict_count': conflict_count, 'd_threshold': d_threshold,
        'fp_bits': fp_bits, 'dicts_mask': dicts_mask,
        'max_entry_strokes': max_strokes, 'section_count': sc,
        'total_bytes': len(data),
    }
    sections = []
    for i in range(sc):
        sec_type, _flags, _rsvd2, off, length, param = \
            struct.unpack_from('<BBHIII', data, 32 + 16 * i)
        sections.append((sec_type, data[off:off + length], param))
    return header, sections


def validx_len_bytes(slot_count):
    return (slot_count * VALIDX_BITS + 7) // 8


# ─── Verification (FORMAT_V4.md section 8) ───

def verify_blobs(left_path, right_path, dict_maps, raw_entries, dicts_mask):
    """Cold-reopen both blobs; full decoder-path lookup of every entry of
    both source dicts, byte-exact; 10k random unknown-key probes.
    Returns result dict; raises/exits on any mismatch."""
    lh, lsecs = parse_blob(left_path)
    rh, rsecs = parse_blob(right_path)
    failures = 0

    for field in ('n', 'bucket_count', 'string_count', 'conflict_count',
                  'd_threshold', 'fp_bits', 'dicts_mask', 'max_entry_strokes'):
        if lh[field] != rh[field]:
            print(f"VERIFY FAIL: header field {field} differs between halves",
                  file=sys.stderr)
            failures += 1
    n = lh['n']
    bucket_count = lh['bucket_count']

    def section(secs, sec_type):
        for t, data, param in secs:
            if t == sec_type:
                return data, param
        raise ValueError(f"missing section type {sec_type}")

    disp_sec, _ = section(lsecs, SEC_DISP)
    memb_sec, _ = section(lsecs, SEC_MEMBERSHIP)
    fp_sec, _ = section(lsecs, SEC_FP)
    lconf_sec, lconf_count = section(lsecs, SEC_CONFLICTS)
    lvalidx_sec, lvalidx_start = section(lsecs, SEC_VALIDX)
    strdir_sec, strdir_blocks = section(rsecs, SEC_STRDIR)
    strings_sec, _ = section(rsecs, SEC_STRINGS)
    rconf_sec, rconf_count = section(rsecs, SEC_CONFLICTS)
    rvalidx_sec, rvalidx_start = section(rsecs, SEC_VALIDX)

    # DISP: sequential decode + skip-table verification
    disp, skips, stream, codemap = parse_disp_section(disp_sec, bucket_count)
    rng = random.Random(0x5444)
    for _ in range(500):
        b = rng.randrange(bucket_count)
        if disp_random_access(stream, codemap, skips, b) != disp[b]:
            print(f"VERIFY FAIL: DISP random access mismatch at bucket {b}",
                  file=sys.stderr)
            failures += 1

    # CONFLICTS: identical in both halves, sorted ascending by slot
    if lconf_sec != rconf_sec or lconf_count != rconf_count:
        print("VERIFY FAIL: CONFLICTS differ between halves", file=sys.stderr)
        failures += 1
    conflict_map = {}
    prev_slot = -1
    for i in range(lconf_count):
        rec = int.from_bytes(lconf_sec[5 * i:5 * i + 5], 'little')
        slot = rec & (MAX_N - 1)
        sid = rec >> 18
        if slot <= prev_slot:
            print(f"VERIFY FAIL: CONFLICTS not sorted at record {i}",
                  file=sys.stderr)
            failures += 1
        prev_slot = slot
        conflict_map[slot] = sid

    strtab = StringTable(strdir_sec, strings_sec, strdir_blocks)

    k = rvalidx_start
    if lvalidx_start != 0:
        print("VERIFY FAIL: left VALIDX start_slot != 0", file=sys.stderr)
        failures += 1
    left_k = len(lvalidx_sec) * 8 // VALIDX_BITS
    if left_k != k:
        print(f"VERIFY FAIL: left VALIDX holds {left_k} slots, right starts at {k}",
              file=sys.stderr)
        failures += 1

    # Full decoder path for every unique key of each dict
    key_results = {}   # dict_id -> {kb: (ok, decoded_bytes)}
    for dict_id, kb_map in dict_maps.items():
        keys = list(kb_map)
        expected = [kb_map[kb][0] for kb in keys]
        slots = compute_slots(keys, disp, bucket_count, n)
        fp_expect = fp_all(keys)
        if np is not None:
            slots_np = slots if hasattr(slots, 'dtype') else np.asarray(slots)
            fp_read = read_lsb_many(fp_sec, slots_np, FP_BITS)
            memb_read = read_lsb_many(memb_sec, slots_np, 2)
            sid_read = np.empty(len(keys), dtype=np.int64)
            left_mask = slots_np < k
            sid_read[left_mask] = read_lsb_many(
                lvalidx_sec, slots_np[left_mask], VALIDX_BITS)
            sid_read[~left_mask] = read_lsb_many(
                rvalidx_sec, slots_np[~left_mask] - k, VALIDX_BITS)
        else:
            fp_read = [read_lsb_at(fp_sec, s, FP_BITS) for s in slots]
            memb_read = [read_lsb_at(memb_sec, s, 2) for s in slots]
            sid_read = [read_lsb_at(lvalidx_sec, s, VALIDX_BITS) if s < k
                        else read_lsb_at(rvalidx_sec, s - k, VALIDX_BITS)
                        for s in slots]

        results = {}
        for i, kb in enumerate(keys):
            slot = int(slots[i])
            ok = True
            decoded = None
            if not (0 <= slot < n):
                ok = False
            elif int(fp_read[i]) != int(fp_expect[i]):
                ok = False
            elif not (int(memb_read[i]) & (1 << dict_id)):
                ok = False
            else:
                sid = int(sid_read[i])
                if dict_id == DICT_LAPWING and int(memb_read[i]) == 3:
                    sid = conflict_map.get(slot, sid)
                decoded = strtab.get(sid)
                if decoded != expected[i]:
                    ok = False
            if not ok:
                failures += 1
                if failures <= 20:
                    print(f"VERIFY FAIL: dict {dict_id} key "
                          f"'{kb_map[kb][1]}' slot {slot}: decoded "
                          f"{decoded!r}, expected {expected[i]!r}",
                          file=sys.stderr)
            results[kb] = ok
        key_results[dict_id] = results

    # Every raw entry of both source dicts (dup-losers resolve to first-wins)
    entries_checked = 0
    for dict_id, entries in raw_entries.items():
        results = key_results[dict_id]
        for _stroke_str, kb in entries:
            entries_checked += 1
            if not results[kb]:
                failures += 1

    # 10k random unknown-key probes
    union = set()
    for kb_map in dict_maps.values():
        union.update(kb_map)
    rng = random.Random(0x5634)
    probes = []
    while len(probes) < 10000:
        ns = rng.choice((1, 1, 2, 3))
        kb = b''.join(struct.pack('<I', rng.randrange(1, 1 << 23))
                      for _ in range(ns))
        if kb not in union:
            probes.append(kb)
    pslots = compute_slots(probes, disp, bucket_count, n)
    pfp = fp_all(probes)
    if np is not None:
        pslots_np = pslots if hasattr(pslots, 'dtype') else np.asarray(pslots)
        pfp_read = read_lsb_many(fp_sec, pslots_np, FP_BITS)
        pmemb = read_lsb_many(memb_sec, pslots_np, 2)
    else:
        pfp_read = [read_lsb_at(fp_sec, s, FP_BITS) for s in pslots]
        pmemb = [read_lsb_at(memb_sec, s, 2) for s in pslots]
    active_dicts = [d for d in (DICT_PLOVER, DICT_LAPWING) if dicts_mask & (1 << d)]
    fp_pass = 0
    accepts = 0
    for i in range(len(probes)):
        slot = int(pslots[i])
        if not (0 <= slot < n):
            print(f"VERIFY FAIL: probe slot {slot} out of range", file=sys.stderr)
            failures += 1
            continue
        if int(pfp_read[i]) == int(pfp[i]):
            fp_pass += 1
            a = active_dicts[i % len(active_dicts)]
            if int(pmemb[i]) & (1 << a):
                accepts += 1

    return {
        'entries_checked': entries_checked,
        'failures': failures,
        'probes': len(probes),
        'probe_fp_pass': fp_pass,
        'probe_accepts': accepts,
        'false_accept_pct': 100.0 * accepts / len(probes),
        'fp_pass_pct': 100.0 * fp_pass / len(probes),
        'left_header': lh,
        'right_header': rh,
    }


# ─── Main ───

def main():
    parser = argparse.ArgumentParser(
        description='Compile steno dictionaries to v4 split-section blobs')
    parser.add_argument('--plover', help='Plover JSON dictionary (dict id 0)')
    parser.add_argument('--lapwing', help='Lapwing JSON dictionary (dict id 1)')
    parser.add_argument('--out-dir', required=True, help='Output directory')
    parser.add_argument('--left-size', type=int, required=True,
                        help='Left half flash budget in bytes')
    parser.add_argument('--right-size', type=int, required=True,
                        help='Right half flash budget in bytes')
    parser.add_argument('--dicts', choices=['both', 'plover', 'lapwing'],
                        default='both', help='Which dicts to compile in')
    args = parser.parse_args()

    want_plover = args.dicts in ('both', 'plover')
    want_lapwing = args.dicts in ('both', 'lapwing')
    if want_plover and not args.plover:
        parser.error('--plover required for --dicts ' + args.dicts)
    if want_lapwing and not args.lapwing:
        parser.error('--lapwing required for --dicts ' + args.dicts)
    dicts_mask = (1 if want_plover else 0) | (2 if want_lapwing else 0)

    dict_maps = {}
    raw_entries = {}
    offenders = []
    if want_plover:
        print(f"Loading plover: {args.plover}", file=sys.stderr)
        entries, kb_map, off = load_dict(args.plover, 'plover')
        raw_entries[DICT_PLOVER] = entries
        dict_maps[DICT_PLOVER] = kb_map
        offenders += off
        print(f"  plover: {len(entries)} entries, {len(kb_map)} unique keys",
              file=sys.stderr)
    if want_lapwing:
        print(f"Loading lapwing: {args.lapwing}", file=sys.stderr)
        entries, kb_map, off = load_dict(args.lapwing, 'lapwing')
        raw_entries[DICT_LAPWING] = entries
        dict_maps[DICT_LAPWING] = kb_map
        offenders += off
        print(f"  lapwing: {len(entries)} entries, {len(kb_map)} unique keys",
              file=sys.stderr)

    if offenders:
        print("FATAL: compile-time limits violated:", file=sys.stderr)
        for o in offenders:
            print(f"  {o}", file=sys.stderr)
        sys.exit(1)

    plover_map = dict_maps.get(DICT_PLOVER, {})
    lapwing_map = dict_maps.get(DICT_LAPWING, {})

    # Union key set (insertion order: plover first, then new lapwing keys)
    union = {}
    for kb in plover_map:
        union[kb] = None
    for kb in lapwing_map:
        union[kb] = None
    keys = list(union)
    n = len(keys)
    if n >= MAX_N:
        print(f"FATAL: union key count {n} >= {MAX_N} (slot must fit 18 bits)",
              file=sys.stderr)
        sys.exit(1)
    max_entry_strokes = max(
        max((v[2] for v in m.values()), default=0) for m in dict_maps.values())
    print(f"Union: n={n}, max_entry_strokes={max_entry_strokes}", file=sys.stderr)

    # Shared string table: sorted unique translations (UTF-8 byte order)
    strings = sorted({v[0] for m in dict_maps.values() for v in m.values()})
    string_count = len(strings)
    if string_count > MAX_STRING_ID:
        print(f"FATAL: {string_count} unique strings > {MAX_STRING_ID} "
              f"(string_id must fit {VALIDX_BITS} bits)", file=sys.stderr)
        sys.exit(1)
    string_id = {s: i for i, s in enumerate(strings)}
    print(f"Strings: {string_count} unique", file=sys.stderr)

    # CHD MPHF
    bucket_count = n // 4
    print(f"Building CHD: n={n}, buckets={bucket_count}, "
          f"D_THRESHOLD={D_THRESHOLD}", file=sys.stderr)
    disp, slot_of_key, chd_stats = build_chd(keys, bucket_count, n)
    print(f"  CHD ok: max_hash_disp={chd_stats['max_hash_disp']}, "
          f"direct={chd_stats['direct_count']}", file=sys.stderr)

    # Per-slot tables
    fps = fp_all(keys)
    memb_slot = [0] * n
    val_slot = [0] * n
    fp_slot = [0] * n
    conflicts = []
    for i, kb in enumerate(keys):
        slot = slot_of_key[i]
        m = ((1 if kb in plover_map else 0)
             | (2 if kb in lapwing_map else 0))
        memb_slot[slot] = m
        fp_slot[slot] = int(fps[i])
        if kb in plover_map:
            sid = string_id[plover_map[kb][0]]
        else:
            sid = string_id[lapwing_map[kb][0]]
        val_slot[slot] = sid
        if m == 3 and plover_map[kb][0] != lapwing_map[kb][0]:
            conflicts.append((slot, string_id[lapwing_map[kb][0]]))
    conflicts.sort()
    conflict_count = len(conflicts)
    print(f"Membership: both={sum(1 for m in memb_slot if m == 3)}, "
          f"conflicts={conflict_count}", file=sys.stderr)

    # Sections
    disp_sec = build_disp_section(disp, bucket_count)
    memb_sec = pad4(pack_lsb(memb_slot, 2))
    fp_sec = pad4(pack_lsb(fp_slot, FP_BITS))
    conf_sec = b''.join((slot | (sid << 18)).to_bytes(5, 'little')
                        for slot, sid in conflicts)
    strdir_sec, strings_sec, block_count, raw_fc_bytes = \
        build_string_sections(strings)
    print(f"Section sizes: DISP={len(disp_sec)} MEMBERSHIP={len(memb_sec)} "
          f"FP={len(fp_sec)} CONFLICTS={len(conf_sec)} "
          f"STRDIR={len(strdir_sec)} STRINGS={len(strings_sec)} "
          f"(FC raw={raw_fc_bytes}, {block_count} blocks) "
          f"VALIDX total={validx_len_bytes(n)}", file=sys.stderr)

    # Placement: k = smallest such that the right blob fits --right-size
    right_fixed = [len(strdir_sec), len(strings_sec), len(conf_sec)]

    def right_size(k):
        return blob_total_size(right_fixed + [validx_len_bytes(n - k)])

    def left_size(k):
        return blob_total_size([len(disp_sec), len(memb_sec), len(fp_sec),
                                len(conf_sec), validx_len_bytes(k)])

    if right_size(n) > args.right_size:
        print(f"FATAL: right blob without any VALIDX is {right_size(n)} bytes "
              f"> budget {args.right_size}. Nothing may be trimmed.",
              file=sys.stderr)
        sys.exit(1)
    lo, hi = 0, n
    while lo < hi:
        mid = (lo + hi) // 2
        if right_size(mid) <= args.right_size:
            hi = mid
        else:
            lo = mid + 1
    k = lo
    lsz, rsz = left_size(k), right_size(k)
    print(f"Placement: k={k} → left={lsz} (budget {args.left_size}), "
          f"right={rsz} (budget {args.right_size})", file=sys.stderr)
    if lsz > args.left_size:
        print(f"FATAL: left blob {lsz} bytes exceeds budget {args.left_size} "
              f"(right={rsz}/{args.right_size}, k={k}, n={n}, "
              f"left VALIDX={validx_len_bytes(k)}, "
              f"right VALIDX={validx_len_bytes(n - k)}). "
              f"Nothing may be trimmed.", file=sys.stderr)
        print("RESULT_JSON: " + json.dumps({
            'ok': False, 'k': k, 'n': n,
            'left_bytes': lsz, 'right_bytes': rsz,
            'left_budget': args.left_size, 'right_budget': args.right_size,
            'sections': {
                'DISP': len(disp_sec), 'MEMBERSHIP': len(memb_sec),
                'FP': len(fp_sec), 'CONFLICTS': len(conf_sec),
                'STRDIR': len(strdir_sec), 'STRINGS': len(strings_sec),
                'VALIDX_LEFT': validx_len_bytes(k),
                'VALIDX_RIGHT': validx_len_bytes(n - k),
            },
        }))
        sys.exit(1)

    validx_left = pack_lsb(val_slot[:k], VALIDX_BITS)
    validx_right = pack_lsb(val_slot[k:], VALIDX_BITS)
    assert len(validx_left) == validx_len_bytes(k)
    assert len(validx_right) == validx_len_bytes(n - k)

    left_sections = [
        (SEC_DISP, disp_sec, 0),
        (SEC_MEMBERSHIP, memb_sec, 0),
        (SEC_FP, fp_sec, 0),
        (SEC_CONFLICTS, conf_sec, conflict_count),
        (SEC_VALIDX, validx_left, 0),
    ]
    right_sections = [
        (SEC_STRDIR, strdir_sec, block_count),
        (SEC_STRINGS, strings_sec, 0),
        (SEC_CONFLICTS, conf_sec, conflict_count),
        (SEC_VALIDX, validx_right, k),
    ]

    left_blob = assemble_blob(left_sections, n, bucket_count, string_count,
                              conflict_count, dicts_mask, max_entry_strokes)
    right_blob = assemble_blob(right_sections, n, bucket_count, string_count,
                               conflict_count, dicts_mask, max_entry_strokes)
    assert len(left_blob) == lsz and len(right_blob) == rsz

    os.makedirs(args.out_dir, exist_ok=True)
    left_path = os.path.join(args.out_dir, 'steno_v4_left.bin')
    right_path = os.path.join(args.out_dir, 'steno_v4_right.bin')
    with open(left_path, 'wb') as f:
        f.write(left_blob)
    with open(right_path, 'wb') as f:
        f.write(right_blob)
    print(f"Wrote {left_path} ({len(left_blob)} bytes)", file=sys.stderr)
    print(f"Wrote {right_path} ({len(right_blob)} bytes)", file=sys.stderr)

    # Mandatory verification (cold reopen)
    print("Verifying (cold reopen, full decode path, every entry)...",
          file=sys.stderr)
    vres = verify_blobs(left_path, right_path, dict_maps, raw_entries,
                        dicts_mask)
    print(f"  entries checked: {vres['entries_checked']}, "
          f"failures: {vres['failures']}", file=sys.stderr)
    print(f"  unknown probes: {vres['probes']}, "
          f"false-accept {vres['false_accept_pct']:.2f}% "
          f"(fp-only pass {vres['fp_pass_pct']:.2f}%)", file=sys.stderr)
    if vres['failures']:
        print(f"VERIFICATION FAILED: {vres['failures']} failures",
              file=sys.stderr)
        sys.exit(1)

    def section_report(sections, half):
        out = []
        for sec_type, data, param in sections:
            out.append({'name': SEC_NAMES[sec_type], 'bytes': len(data),
                        'param': param, 'half': half})
        return out

    manifest = {
        'format_version': VERSION,
        'n': n,
        'bucket_count': bucket_count,
        'string_count': string_count,
        'conflict_count': conflict_count,
        'k_split_slot': k,
        'max_entry_strokes': max_entry_strokes,
        'dicts_mask': dicts_mask,
        'd_threshold': D_THRESHOLD,
        'fp_bits': FP_BITS,
        'chd': chd_stats,
        'strings': {
            'block_count': block_count,
            'raw_fc_bytes': raw_fc_bytes,
            'compressed_bytes': len(strings_sec),
        },
        'left': {
            'file': 'steno_v4_left.bin',
            'bytes': len(left_blob),
            'budget': args.left_size,
            'sections': section_report(left_sections, 'left'),
        },
        'right': {
            'file': 'steno_v4_right.bin',
            'bytes': len(right_blob),
            'budget': args.right_size,
            'sections': section_report(right_sections, 'right'),
        },
        'verification': {
            'entries_checked': vres['entries_checked'],
            'failures': vres['failures'],
            'unknown_probes': vres['probes'],
            'false_accept_pct': vres['false_accept_pct'],
            'fp_only_pass_pct': vres['fp_pass_pct'],
        },
    }
    manifest_path = os.path.join(args.out_dir, 'manifest.json')
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)
    print(f"Wrote {manifest_path}", file=sys.stderr)

    for half in ('left', 'right'):
        print(f"{half}: {manifest[half]['bytes']} bytes "
              f"(budget {manifest[half]['budget']})")
        for s in manifest[half]['sections']:
            print(f"  {s['name']:<11} {s['bytes']:>8} bytes (param {s['param']})")
    print("RESULT_JSON: " + json.dumps({'ok': True, 'manifest': manifest}))


if __name__ == '__main__':
    main()

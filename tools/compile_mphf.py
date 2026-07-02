#!/usr/bin/env python3
"""CHD MPHF dictionary compiler for steno engine.

Reads Plover JSON dictionaries and produces a compact binary for
embedded use (nRF52840, 462KB flash budget).
"""

import json
import struct
import math
import argparse
import sys
import os
import zlib
from collections import defaultdict

# ─── Steno stroke parsing ───

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


# ─── Hashing ───

def fnv1a_32(data: bytes) -> int:
    """FNV-1a 32-bit hash."""
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def hash_key(key_bytes: bytes, seed: int) -> int:
    """Hash key with seed by prepending seed bytes."""
    return fnv1a_32(struct.pack('<I', seed) + key_bytes)


# ─── Bit packing ───

class BitWriter:
    def __init__(self):
        self.data = bytearray()
        self.bit_pos = 0  # total bits written

    def write_bits(self, value, n_bits):
        """Write n_bits of value (LSB first)."""
        for i in range(n_bits):
            if self.bit_pos % 8 == 0:
                self.data.append(0)
            if value & (1 << i):
                self.data[-1] |= (1 << (self.bit_pos % 8))
            self.bit_pos += 1

    def pad_to_alignment(self, alignment=4):
        """Pad to byte alignment."""
        while len(self.data) % alignment != 0:
            self.data.append(0)
        self.bit_pos = len(self.data) * 8

    def to_bytes(self):
        return bytes(self.data)


class BitReader:
    def __init__(self, data):
        self.data = data
        self.bit_pos = 0

    def read_bits(self, n_bits):
        value = 0
        for i in range(n_bits):
            byte_idx = self.bit_pos // 8
            bit_idx = self.bit_pos % 8
            if self.data[byte_idx] & (1 << bit_idx):
                value |= (1 << i)
            self.bit_pos += 1
        return value


# ─── Key encoding ───

def encode_key(stroke_str):
    """Parse stroke string → key_bytes (each stroke as u32 LE, concatenated)."""
    parts = stroke_str.split('/')
    strokes = tuple(parse_stroke(s) for s in parts)
    key_bytes = b''.join(struct.pack('<I', s) for s in strokes)
    return strokes, key_bytes


# ─── Importance scoring ───

def score_entry(stroke_str, translation):
    """Lower score = more important = keep first."""
    n_strokes = stroke_str.count('/') + 1
    has_format = '{' in translation
    return (n_strokes, has_format, len(translation), stroke_str)


# ─── CHD MPHF construction ───

def build_chd(keys_and_bytes, entry_count):
    """
    Build CHD MPHF.

    keys_and_bytes: list of (index, key_bytes) for each entry
    entry_count: total number of entries

    Returns: (displacements, slot_to_entry_idx, max_displacement)
        displacements[bucket] = d value
        slot_to_entry_idx[slot] = index into keys_and_bytes, or -1 if empty
    """
    bucket_count = max(entry_count // 4, 16)

    # Assign keys to buckets
    buckets = defaultdict(list)
    for idx, (_, kb) in enumerate(keys_and_bytes):
        b = hash_key(kb, 0) % bucket_count
        buckets[b].append(idx)

    # Sort buckets by size descending
    sorted_buckets = sorted(buckets.items(), key=lambda x: len(x[1]), reverse=True)

    displacements = [0] * bucket_count
    occupied = set()
    slot_to_entry = [-1] * entry_count
    max_disp = 0

    for bucket_id, members in sorted_buckets:
        if not members:
            continue

        member_key_bytes = [(m, keys_and_bytes[m][1]) for m in members]

        placed = False
        for d in range(1 << 20):
            slots = []
            collision = False
            seen = set()

            for _, kb in member_key_bytes:
                slot = hash_key(kb, d + 1) % entry_count
                if slot in occupied or slot in seen:
                    collision = True
                    break
                seen.add(slot)
                slots.append(slot)

            if collision:
                continue

            # Place all members
            for i, (m, _) in enumerate(member_key_bytes):
                occupied.add(slots[i])
                slot_to_entry[slots[i]] = m

            displacements[bucket_id] = d
            if d > max_disp:
                max_disp = d
            placed = True
            break

        if not placed:
            print(f"FATAL: bucket {bucket_id} with {len(members)} keys failed after 1048576 tries",
                  file=sys.stderr)
            return None, None, None

    return displacements, slot_to_entry, max_disp


# ─── Compilation ───

def compile_mphf(entries, max_size=None, block_size=4096):
    """
    entries: list of (stroke_str, translation) from JSON dict
    max_size: max output size in bytes (default: 462*1024 = 473088)
    block_size: zlib compression block size (smaller = better ratio, more blocks)

    Returns: bytes (the compiled binary) or None if can't fit
    """
    if max_size is None:
        max_size = 462 * 1024

    # Sort by importance for potential trimming
    entries_scored = sorted(entries, key=lambda e: score_entry(e[0], e[1]))

    # Parse all keys, dedup by key_bytes (last wins for same key)
    seen_keys = {}
    for stroke_str, translation in entries_scored:
        strokes, key_bytes = encode_key(stroke_str)
        if key_bytes in seen_keys:
            prev = seen_keys[key_bytes]
            print(f"  Dedup: '{stroke_str}'→'{translation}' collides with "
                  f"'{prev[2]}'→'{prev[1]}', keeping first", file=sys.stderr)
            continue
        entry = (key_bytes, translation, stroke_str, strokes)
        seen_keys[key_bytes] = entry

    keys_and_bytes = list(seen_keys.values())

    entry_count = len(keys_and_bytes)
    if entry_count == 0:
        return None

    # Build deduped string table
    translations = [kb[1] for kb in keys_and_bytes]
    unique_translations = sorted(set(translations))
    trans_to_id = {t: i for i, t in enumerate(unique_translations)}
    unique_count = len(unique_translations)

    # String table (block-compressed)
    string_data_raw = b''
    string_offsets = []
    for t in unique_translations:
        string_offsets.append(len(string_data_raw))
        string_data_raw += t.encode('utf-8') + b'\x00'

    compressed_blocks = []
    for i in range(0, len(string_data_raw), block_size):
        block = string_data_raw[i:i + block_size]
        compressed_blocks.append(zlib.compress(block, 9))

    # Prefix table
    prefix_strokes = set()
    for kb, trans, stroke_str, strokes in keys_and_bytes:
        if len(strokes) > 1:
            prefix_strokes.add(strokes[0])
    prefix_list = sorted(prefix_strokes)

    print(f"  Building CHD MPHF: {entry_count} entries...",
          file=sys.stderr)

    # Build CHD
    chd_input = [(i, keys_and_bytes[i][0]) for i in range(entry_count)]
    displacements, slot_to_entry, max_disp = build_chd(chd_input, entry_count)

    if displacements is None:
        return None

    bucket_count = len(displacements)

    # Compute actual bit widths
    disp_bits = max(1, math.ceil(math.log2(max(max_disp + 1, 2))))
    value_bits = max(1, math.ceil(math.log2(max(unique_count, 2))))
    prefix_count = len(prefix_list)

    print(f"  Buckets: {bucket_count}", file=sys.stderr)
    print(f"  Max displacement: {max_disp}, disp_bits: {disp_bits}", file=sys.stderr)
    print(f"  Unique translations: {unique_count}, value_bits: {value_bits}", file=sys.stderr)
    print(f"  Prefix entries: {prefix_count}", file=sys.stderr)

    # ─── Build binary ───

    # Displacements section
    disp_writer = BitWriter()
    for d in displacements:
        disp_writer.write_bits(d, disp_bits)
    disp_writer.pad_to_alignment(4)
    disp_section = disp_writer.to_bytes()

    # Values section: slot → value_id
    val_writer = BitWriter()
    fingerprints = bytearray(entry_count)
    for slot in range(entry_count):
        entry_idx = slot_to_entry[slot]
        if entry_idx >= 0:
            kb, trans, stroke_str, strokes = keys_and_bytes[entry_idx]
            val_id = trans_to_id[trans]
            val_writer.write_bits(val_id, value_bits)
            fingerprints[slot] = fnv1a_32(kb) & 0xFF
        else:
            val_writer.write_bits(0, value_bits)
            fingerprints[slot] = 0
    val_writer.pad_to_alignment(4)
    val_section = val_writer.to_bytes()

    # Fingerprints section
    fp_section = bytes(fingerprints)
    # Pad to 4-byte boundary
    while len(fp_section) % 4 != 0:
        fp_section += b'\x00'

    # String offsets section (u24 packed LE — 3 bytes each, into raw/uncompressed table)
    str_offsets_section = b''.join(struct.pack('<I', off)[:3] for off in string_offsets)

    # String data section (block-compressed)
    block_dir = struct.pack('<H', len(compressed_blocks))
    block_offset = 0
    for blk in compressed_blocks:
        block_dir += struct.pack('<I', block_offset)
        block_offset += len(blk)
    str_data_section = block_dir + b''.join(compressed_blocks)

    # Prefix table section
    prefix_section = b''.join(struct.pack('<I', s) for s in prefix_list)

    # Header (32 bytes):
    #   magic: u32, version: u16, flags: u16,
    #   entry_count: u32, bucket_count: u32, unique_count: u32,
    #   value_bits: u8, disp_bits: u8, prefix_count: u16,
    #   block_size: u32, reserved1: u32
    header = struct.pack('<IHHIIIBBHii',
        0x4F4E5453,     # magic "STNO"
        2,              # version
        0x0001,         # flags: bit 0 = block-compressed strings
        entry_count,    # entry_count
        bucket_count,   # bucket_count
        unique_count,   # unique_count
        value_bits,     # value_bits
        disp_bits,      # disp_bits
        prefix_count,   # prefix_count
        block_size,     # block_size
        0,              # reserved1
    )
    assert len(header) == 32, f"Header is {len(header)} bytes, expected 32"

    binary = header + disp_section + val_section + fp_section + str_offsets_section + str_data_section + prefix_section

    # ─── Verification ───
    print(f"  Verifying all {entry_count} entries...", file=sys.stderr)
    errors = 0
    for entry_idx in range(entry_count):
        kb, trans, stroke_str, strokes = keys_and_bytes[entry_idx]

        # Lookup through MPHF
        bucket = hash_key(kb, 0) % bucket_count

        # Read displacement
        disp_reader = BitReader(disp_section)
        disp_reader.bit_pos = bucket * disp_bits
        d = disp_reader.read_bits(disp_bits)

        slot = hash_key(kb, d + 1) % entry_count

        # Check fingerprint
        expected_fp = fnv1a_32(kb) & 0xFF
        if fingerprints[slot] != expected_fp:
            print(f"  VERIFY FAIL: fingerprint mismatch for '{stroke_str}' at slot {slot}: "
                  f"got {fingerprints[slot]}, expected {expected_fp}", file=sys.stderr)
            errors += 1
            continue

        # Check value
        val_reader = BitReader(val_section)
        val_reader.bit_pos = slot * value_bits
        val_id = val_reader.read_bits(value_bits)

        # Resolve string from compressed table
        off_bytes = str_offsets_section[val_id * 3:(val_id + 1) * 3]
        str_off = off_bytes[0] | (off_bytes[1] << 8) | (off_bytes[2] << 16)
        block_idx = str_off // block_size
        in_block_off = str_off % block_size
        raw_block = zlib.decompress(compressed_blocks[block_idx])
        if b'\x00' in raw_block[in_block_off:]:
            end = raw_block.index(b'\x00', in_block_off)
            resolved = raw_block[in_block_off:end].decode('utf-8')
        elif block_idx + 1 < len(compressed_blocks):
            part1 = raw_block[in_block_off:]
            next_block = zlib.decompress(compressed_blocks[block_idx + 1])
            end = next_block.index(b'\x00')
            resolved = (part1 + next_block[:end]).decode('utf-8')
        else:
            resolved = raw_block[in_block_off:].decode('utf-8')

        if resolved != trans:
            print(f"  VERIFY FAIL: value mismatch for '{stroke_str}': "
                  f"got '{resolved}', expected '{trans}'", file=sys.stderr)
            errors += 1

    if errors:
        print(f"  VERIFICATION FAILED: {errors} errors", file=sys.stderr)
        return None

    print(f"  Verification passed: all {entry_count} entries OK", file=sys.stderr)

    # Check final size — hard error, never trim
    if max_size and len(binary) > max_size:
        print(f"FATAL: output {len(binary)} bytes exceeds budget {max_size}. "
              f"Increase budget or split differently.", file=sys.stderr)
        return None

    return binary, {
        'entry_count': entry_count,
        'bucket_count': bucket_count,
        'unique_count': unique_count,
        'value_bits': value_bits,
        'disp_bits': disp_bits,
        'max_displacement': max_disp,
        'prefix_count': prefix_count,
        'block_size': block_size,
        'disp_section_bytes': len(disp_section),
        'val_section_bytes': len(val_section),
        'fp_section_bytes': len(fp_section),
        'str_offsets_bytes': len(str_offsets_section),
        'str_data_bytes': len(str_data_section),
        'str_data_raw_bytes': len(string_data_raw),
        'prefix_section_bytes': len(prefix_section),
        'total_bytes': len(binary),
    }


def print_stats(stats):
    """Print size breakdown statistics."""
    print(f"Entries: {stats['entry_count']}")
    print(f"Block size: {stats.get('block_size', 4096)} bytes")
    print(f"MPHF displacements: {stats['disp_section_bytes']/1024:.1f} KB "
          f"({stats['bucket_count']} buckets, {stats['disp_bits']} bits each)")
    print(f"Value array: {stats['val_section_bytes']/1024:.1f} KB "
          f"({stats['entry_count']} entries, {stats['value_bits']} bits each)")
    print(f"Fingerprints: {stats['fp_section_bytes']/1024:.1f} KB")
    print(f"String offsets: {stats['str_offsets_bytes']/1024:.1f} KB "
          f"({stats['unique_count']} unique x 3 bytes)")
    print(f"String data: {stats['str_data_bytes']/1024:.1f} KB"
          f" (compressed, {stats.get('str_data_raw_bytes', 0)/1024:.1f} KB raw)")
    print(f"Prefix table: {stats['prefix_section_bytes']/1024:.1f} KB "
          f"({stats['prefix_count']} entries x 4 bytes)")
    print(f"Total: {stats['total_bytes']/1024:.1f} KB")


def partition_entries(entries, left_budget, right_budget):
    """Partition dict entries by importance into left (central) and right (peripheral).

    Left gets highest-importance entries first (most common single-stroke words).
    Right gets remaining entries. Uses empirical ~4 bytes/entry from MPHF benchmarks
    to estimate how many entries each budget can hold.

    Returns (left_entries, right_entries).
    """
    sorted_entries = sorted(entries, key=lambda e: score_entry(e[0], e[1]))

    # ~4 bytes/entry average from 583KB / 147K entries benchmark
    # Use 4.5 for safety margin (smaller partitions have higher per-entry overhead)
    est_bytes_per_entry = 4.5
    left_max = int(left_budget / est_bytes_per_entry)
    left_max = max(1, min(left_max, len(sorted_entries) - 1))

    left_entries = sorted_entries[:left_max]
    right_entries = sorted_entries[left_max:]

    return left_entries, right_entries


def main():
    parser = argparse.ArgumentParser(description='Compile steno dictionary to MPHF binary format')
    parser.add_argument('input', help='Input JSON dictionary (Plover format)')
    parser.add_argument('output', help='Output binary file')
    parser.add_argument('--max-size', type=int, default=462*1024,
                       help='Maximum output size in bytes (default: 473088 = 462KB)')
    parser.add_argument('--stats', action='store_true',
                       help='Print size breakdown statistics')
    parser.add_argument('--verify', action='store_true', default=True,
                       help='Verify compiled dict (default: true)')
    parser.add_argument('--split-part', choices=['left', 'right'],
                       help='Build one partition of a split dict (left=central, right=peripheral)')
    parser.add_argument('--left-size', type=int, default=153600,
                       help='Left partition flash budget in bytes (default: 153600 = 150KB)')
    parser.add_argument('--right-size', type=int, default=512000,
                       help='Right partition flash budget in bytes (default: 512000 = 500KB)')
    parser.add_argument('--block-size', type=int, default=4096,
                       help='Zlib compression block size (default: 4096, try 2048/1024 for tighter packing)')
    args = parser.parse_args()

    # Load dictionary
    with open(args.input) as f:
        raw_dict = json.load(f)

    print(f"Loaded {len(raw_dict)} entries from {args.input}", file=sys.stderr)

    entries = list(raw_dict.items())

    # Split-partition mode
    if args.split_part:
        left_entries, right_entries = partition_entries(
            entries, args.left_size, args.right_size)

        if args.split_part == 'left':
            print(f"Split partition: LEFT (central) — {len(left_entries)} entries, "
                  f"budget {args.left_size} bytes", file=sys.stderr)
            entries = left_entries
            max_size = args.left_size
        else:
            print(f"Split partition: RIGHT (peripheral) — {len(right_entries)} entries, "
                  f"budget {args.right_size} bytes", file=sys.stderr)
            entries = right_entries
            max_size = args.right_size
    else:
        max_size = args.max_size

    result = compile_mphf(entries, max_size=max_size,
                          block_size=args.block_size)

    if result is None:
        print("Compilation failed", file=sys.stderr)
        sys.exit(1)

    binary, stats = result

    with open(args.output, 'wb') as f:
        f.write(binary)

    print(f"Wrote {len(binary)} bytes to {args.output}", file=sys.stderr)

    if args.stats or args.split_part:
        print()
        if args.split_part:
            print(f"=== {args.split_part.upper()} partition ===")
        print_stats(stats)


if __name__ == '__main__':
    main()

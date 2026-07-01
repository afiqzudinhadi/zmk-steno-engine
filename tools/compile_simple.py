#!/usr/bin/env python3
"""Simple flat-format steno dictionary compiler.

Outputs a binary format optimized for binary search on embedded targets.
No compression — just sorted entries with fixed-width keys.

Format:
  Header (16 bytes):
    magic:           u32 = 0x4F4E5453 ("STNO")
    version:         u16 = 1
    max_strokes:     u8  (max stroke count per entry)
    pad:             u8
    entry_count:     u32
    strings_offset:  u32

  Entry array (sorted by stroke tuple, fixed width):
    Each entry = max_strokes * 4 + 4 bytes:
      strokes[max_strokes]: u32 LE (unused slots = 0)
      string_offset:        u32 LE (into string table)

  String table:
    Null-terminated UTF-8 strings, concatenated
"""

import argparse
import json
import struct
import sys
import os

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


def compile_dict(json_path, max_entries=None):
    with open(json_path) as f:
        raw = json.load(f)

    entries = []
    for stroke_str, translation in raw.items():
        strokes = tuple(parse_stroke(s) for s in stroke_str.split('/'))
        entries.append((strokes, translation))

    if max_entries and len(entries) > max_entries:
        single = [(s, t) for s, t in entries if len(s) == 1]
        multi = [(s, t) for s, t in entries if len(s) > 1]
        multi.sort(key=lambda x: (len(x[1]), len(x[0])))
        remaining = max_entries - len(single)
        entries = single + multi[:max(0, remaining)]

    entries.sort(key=lambda x: x[0])
    max_strokes = max(len(s) for s, _ in entries)

    string_table = bytearray()
    string_offsets = {}
    for _, translation in entries:
        if translation not in string_offsets:
            string_offsets[translation] = len(string_table)
            string_table.extend(translation.encode('utf-8'))
            string_table.append(0)

    entry_size = max_strokes * 4 + 4
    header_size = 16
    entries_size = len(entries) * entry_size
    strings_offset = header_size + entries_size

    header = struct.pack('<IHBBII',
                         0x4F4E5453,  # "STNO"
                         1,           # version
                         max_strokes,
                         0,           # pad
                         len(entries),
                         strings_offset)

    entry_data = bytearray()
    for strokes, translation in entries:
        padded = list(strokes) + [0] * (max_strokes - len(strokes))
        for s in padded:
            entry_data.extend(struct.pack('<I', s))
        entry_data.extend(struct.pack('<I', string_offsets[translation]))

    binary = header + bytes(entry_data) + bytes(string_table)
    return binary, len(entries), max_strokes, len(string_table)


def main():
    parser = argparse.ArgumentParser(description='Compile steno dict to flat binary')
    parser.add_argument('input', help='JSON dictionary path')
    parser.add_argument('-o', '--output', default='steno_dict.bin')
    parser.add_argument('--max-entries', type=int, default=None)
    parser.add_argument('--stats', action='store_true')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: {args.input} not found")
        sys.exit(1)

    binary, n_entries, max_strokes, str_size = compile_dict(args.input, args.max_entries)

    with open(args.output, 'wb') as f:
        f.write(binary)

    if args.stats:
        entry_size = max_strokes * 4 + 4
        print(f"Entries:       {n_entries}")
        print(f"Max strokes:   {max_strokes}")
        print(f"Entry size:    {entry_size} bytes")
        print(f"Entry array:   {n_entries * entry_size} bytes")
        print(f"String table:  {str_size} bytes")
        print(f"Total:         {len(binary)} bytes ({len(binary)/1024:.1f} KB)")

    print(f"Written {len(binary)} bytes to {args.output}")


if __name__ == '__main__':
    main()

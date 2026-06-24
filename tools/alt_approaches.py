#!/usr/bin/env python3
"""Alternative compression approaches beyond DAWG/trie.

1. Compressed page table (sorted entries, zlib pages, binary search)
2. Rule-based reduction (phonetic rules + exception table)
3. MPHF + compressed blob (no random access, decompress per-lookup)
4. Two-level hash (first stroke → bucket → scan)
5. Full compressed blob with LRU page cache
6. Hybrid combos
"""

import json
import struct
import sys
import zlib
import math
import os
from collections import Counter, defaultdict

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
        elif has_hyphen:
            if s.index(c) < s.index('-'):
                result |= STENO_KEYS.get(c + '-', 0)
            else:
                result |= STENO_KEYS.get('-' + c, 0)
        else:
            if (c + '-') in STENO_KEYS:
                result |= STENO_KEYS[c + '-']
    return result


def stroke_to_bytes(stroke_val):
    """Encode stroke as 3 bytes (23 bits used)."""
    return struct.pack('<I', stroke_val)[:3]


# ─── Approach 1: Compressed Page Table ───

def test_compressed_pages(entries, page_sizes=[64, 128, 256, 512]):
    """
    Sort entries, group into pages, delta-encode + zlib each page.
    Binary search on page index for lookup.
    """
    print("=" * 60)
    print("APPROACH 1: COMPRESSED PAGE TABLE")
    print("=" * 60)

    # Build string table (deduped, sorted)
    all_translations = sorted(set(v for _, v in entries))
    trans_to_id = {t: i for i, t in enumerate(all_translations)}

    raw_strings = b'\x00'.join(t.encode('utf-8') for t in all_translations)
    str_compressed = len(zlib.compress(raw_strings, 9))

    # Block-compressed strings for random access
    str_blocks = []
    for i in range(0, len(raw_strings), 4096):
        str_blocks.append(zlib.compress(raw_strings[i:i+4096], 9))
    str_block_total = sum(len(b) for b in str_blocks) + len(str_blocks) * 4

    print(f"  String table (block 4KB): {str_block_total/1024:.1f} KB")
    print(f"  String table (full zlib): {str_compressed/1024:.1f} KB")
    print()

    for page_size in page_sizes:
        n_pages = math.ceil(len(entries) / page_size)

        # Serialize each page: delta-coded stroke sequences + translation IDs
        page_data = []
        page_index = []  # (first_key, compressed_offset)

        total_compressed = 0

        for p in range(n_pages):
            start = p * page_size
            end = min(start + page_size, len(entries))
            page_entries = entries[start:end]

            # First key for index
            first_key = page_entries[0][0]

            # Serialize page
            buf = bytearray()
            prev_strokes = ()

            for strokes, translation in page_entries:
                tid = trans_to_id[translation]

                # Delta from previous: shared prefix length + new strokes + tid
                shared = 0
                for i in range(min(len(strokes), len(prev_strokes))):
                    if strokes[i] == prev_strokes[i]:
                        shared += 1
                    else:
                        break

                # Encode: shared_len(1) + n_new(1) + new_strokes(3 each) + tid(2)
                n_new = len(strokes) - shared
                buf.append(shared)
                buf.append(n_new)
                for s in strokes[shared:]:
                    buf.extend(stroke_to_bytes(s))
                buf.extend(struct.pack('<H', tid & 0xFFFF))

                prev_strokes = strokes

            raw_page = bytes(buf)
            compressed_page = zlib.compress(raw_page, 9)

            page_index.append((first_key, total_compressed, len(compressed_page)))
            total_compressed += len(compressed_page)
            page_data.append(compressed_page)

        # Page index size: per page = first_key (variable) + offset(3) + size(2)
        avg_key_len = sum(len(k) * 3 for k, _, _ in page_index) / len(page_index)
        index_entry_size = avg_key_len + 5
        index_total = int(n_pages * index_entry_size)

        # Simpler: first_key_hash(4) + offset(3) + size(2) = 9 bytes per page
        index_simple = n_pages * 9

        total = total_compressed + index_simple + str_block_total
        total_fullzlib = total_compressed + index_simple + str_compressed

        print(f"  Page size={page_size:4d}: {n_pages:4d} pages, "
              f"pages={total_compressed/1024:.1f}KB "
              f"idx={index_simple/1024:.1f}KB "
              f"str={str_block_total/1024:.1f}KB "
              f"TOTAL={total/1024:.1f}KB "
              f"(w/full zlib str: {total_fullzlib/1024:.1f}KB)")

    print()
    return total_compressed, index_simple, str_block_total


# ─── Approach 2: Two-Level Hash Table ───

def test_two_level_hash(entries):
    """
    Level 1: hash(first_stroke) → bucket
    Level 2: within bucket, linear scan of entries
    Each bucket independently compressed.
    """
    print("=" * 60)
    print("APPROACH 2: TWO-LEVEL HASH TABLE")
    print("=" * 60)

    # Group by first stroke
    buckets = defaultdict(list)
    for strokes, translation in entries:
        buckets[strokes[0]].append((strokes, translation))

    print(f"  Unique first strokes (buckets): {len(buckets)}")

    bucket_sizes = [len(v) for v in buckets.values()]
    print(f"  Bucket size: min={min(bucket_sizes)} max={max(bucket_sizes)} "
          f"avg={sum(bucket_sizes)/len(bucket_sizes):.1f} "
          f"median={sorted(bucket_sizes)[len(bucket_sizes)//2]}")

    # Build string table
    all_translations = sorted(set(v for _, v in entries))
    trans_to_id = {t: i for i, t in enumerate(all_translations)}
    raw_strings = b'\x00'.join(t.encode('utf-8') for t in all_translations)
    str_blocks = []
    for i in range(0, len(raw_strings), 4096):
        str_blocks.append(zlib.compress(raw_strings[i:i+4096], 9))
    str_total = sum(len(b) for b in str_blocks) + len(str_blocks) * 4

    # Compress each bucket
    total_bucket_compressed = 0
    for first_stroke, bucket_entries in buckets.items():
        buf = bytearray()
        for strokes, translation in sorted(bucket_entries):
            tid = trans_to_id[translation]
            # remaining strokes after first
            remaining = strokes[1:]
            buf.append(len(remaining))
            for s in remaining:
                buf.extend(stroke_to_bytes(s))
            buf.extend(struct.pack('<H', tid & 0xFFFF))

        compressed = zlib.compress(bytes(buf), 9)
        total_bucket_compressed += len(compressed)

    # Bucket index: first_stroke(3) + offset(3) + size(2) = 8 bytes each
    bucket_index = len(buckets) * 8

    total = total_bucket_compressed + bucket_index + str_total
    print(f"  Buckets compressed: {total_bucket_compressed/1024:.1f} KB")
    print(f"  Bucket index: {bucket_index/1024:.1f} KB")
    print(f"  String table: {str_total/1024:.1f} KB")
    print(f"  TOTAL: {total/1024:.1f} KB")

    # With full zlib strings
    str_zlib = len(zlib.compress(raw_strings, 9))
    total_zlib = total_bucket_compressed + bucket_index + str_zlib
    print(f"  TOTAL (full zlib str): {total_zlib/1024:.1f} KB")
    print()
    return total


# ─── Approach 3: Full zlib blob with page cache ───

def test_full_compressed(entries):
    """
    Store everything as one zlib blob. At runtime, decompress
    needed sections into RAM page cache. LRU eviction.
    """
    print("=" * 60)
    print("APPROACH 3: FULL COMPRESSED BLOB + PAGE CACHE")
    print("=" * 60)

    all_translations = sorted(set(v for _, v in entries))
    trans_to_id = {t: i for i, t in enumerate(all_translations)}

    # Serialize all entries sorted
    buf = bytearray()
    prev_strokes = ()
    for strokes, translation in entries:
        tid = trans_to_id[translation]
        shared = 0
        for i in range(min(len(strokes), len(prev_strokes))):
            if strokes[i] == prev_strokes[i]:
                shared += 1
            else:
                break
        n_new = len(strokes) - shared
        buf.append(shared)
        buf.append(n_new)
        for s in strokes[shared:]:
            buf.extend(stroke_to_bytes(s))
        buf.extend(struct.pack('<H', tid & 0xFFFF))
        prev_strokes = strokes

    raw_entries = bytes(buf)

    # String table
    raw_strings = b'\x00'.join(t.encode('utf-8') for t in all_translations)

    # Full zlib
    entries_zlib = len(zlib.compress(raw_entries, 9))
    strings_zlib = len(zlib.compress(raw_strings, 9))

    # zstd-like (try different zlib levels and strategies)
    entries_zlib_best = len(zlib.compress(raw_entries, 9))

    print(f"  Raw entries: {len(raw_entries)/1024:.1f} KB")
    print(f"  Raw strings: {len(raw_strings)/1024:.1f} KB")
    print(f"  Entries zlib: {entries_zlib/1024:.1f} KB")
    print(f"  Strings zlib: {strings_zlib/1024:.1f} KB")
    print(f"  TOTAL (full zlib): {(entries_zlib + strings_zlib)/1024:.1f} KB")
    print()
    print(f"  Runtime: decompress ~4KB page per lookup (~1-5ms)")
    print(f"  RAM cache: 8 pages × 4KB = 32KB for hot entries")
    print()

    # Sectored version: split into 4KB sectors, independently compressed
    # Allows decompressing only the needed sector
    sector_size = 4096
    sectors = []
    for i in range(0, len(raw_entries), sector_size):
        sector = raw_entries[i:i+sector_size]
        sectors.append(zlib.compress(sector, 9))

    sectors_total = sum(len(s) for s in sectors)
    sector_index = len(sectors) * 8  # offset(4) + first_entry_stroke_hash(4)

    str_sectors = []
    for i in range(0, len(raw_strings), sector_size):
        str_sectors.append(zlib.compress(raw_strings[i:i+sector_size], 9))
    str_sectors_total = sum(len(s) for s in str_sectors)
    str_sector_index = len(str_sectors) * 4

    sectored_total = sectors_total + sector_index + str_sectors_total + str_sector_index

    print(f"  Sectored (4KB, random access):")
    print(f"    Entry sectors: {sectors_total/1024:.1f} KB ({len(sectors)} sectors)")
    print(f"    String sectors: {str_sectors_total/1024:.1f} KB ({len(str_sectors)} sectors)")
    print(f"    Indices: {(sector_index + str_sector_index)/1024:.1f} KB")
    print(f"    TOTAL: {sectored_total/1024:.1f} KB")
    print()

    return entries_zlib + strings_zlib, sectored_total


# ─── Approach 4: Phonetic rule analysis ───

def test_rule_reduction(raw_dict):
    """
    Analyze how many entries could be generated by phonetic rules.
    Steno maps sounds → letters. Regular words follow patterns.
    """
    print("=" * 60)
    print("APPROACH 4: PHONETIC RULE ANALYSIS")
    print("=" * 60)

    # Steno phonetic mappings (left hand → initial consonants)
    LEFT_MAP = {
        0x00000001: 's',          # S
        0x00000002: 't',          # T
        0x00000004: 'k',          # K
        0x00000002|0x00000004: 'c',  # TK → d → but also 'c' context dependent
        0x00000008: 'p',          # P
        0x00000010: 'w',          # W
        0x00000020: 'h',          # H
        0x00000040: 'r',          # R
    }

    # Count single-stroke entries where output is a simple word
    single_stroke = {k: v for k, v in raw_dict.items() if '/' not in k}
    multi_stroke = {k: v for k, v in raw_dict.items() if '/' in k}

    # Categorize translations
    simple_words = 0  # single word, lowercase, no formatting
    formatted = 0     # contains { } formatting
    phrases = 0       # multiple words
    other = 0

    for translation in raw_dict.values():
        if '{' in translation:
            formatted += 1
        elif ' ' in translation.strip():
            phrases += 1
        elif translation.strip().replace("'", "").replace("-", "").isalpha():
            simple_words += 1
        else:
            other += 1

    print(f"  Translation categories:")
    print(f"    Simple words: {simple_words} ({simple_words/len(raw_dict)*100:.1f}%)")
    print(f"    Phrases:      {phrases} ({phrases/len(raw_dict)*100:.1f}%)")
    print(f"    Formatted:    {formatted} ({formatted/len(raw_dict)*100:.1f}%)")
    print(f"    Other:        {other} ({other/len(raw_dict)*100:.1f}%)")
    print()

    # Suffix/prefix analysis
    # Many multi-stroke entries = base word + suffix stroke
    # If we store base words + suffix rules, we eliminate many entries

    # Check: how many multi-stroke translations are base_word + common_suffix?
    single_translations = set(raw_dict[k] for k in single_stroke)

    common_suffixes = ['ing', 'ed', 'er', 'est', 'ly', 'ment', 'ness', 'tion',
                       'sion', 'able', 'ible', 'ful', 'less', 'ous', 'ive',
                       'al', 'ial', 's', 'es', "'s", 'ry', 'ary', 'ity',
                       'ize', 'ise', 'en', 'ence', 'ance', 'ent', 'ant',
                       'ion', 'or', 'ist', 'ism', 'ical', 'ically']

    derivable = 0
    derivable_by_suffix = Counter()

    for k, v in multi_stroke.items():
        v_clean = v.strip().lower()
        for suffix in common_suffixes:
            if v_clean.endswith(suffix):
                base = v_clean[:-len(suffix)]
                # Check variants of base in single-stroke dict
                if base in single_translations or (base + 'e') in single_translations:
                    derivable += 1
                    derivable_by_suffix[suffix] += 1
                    break

    print(f"  Multi-stroke entries derivable from single + suffix:")
    print(f"    Derivable: {derivable} of {len(multi_stroke)} ({derivable/len(multi_stroke)*100:.1f}%)")
    for suffix, count in derivable_by_suffix.most_common(10):
        print(f"      -{suffix}: {count}")

    remaining = len(raw_dict) - derivable
    print()
    print(f"  If suffix rules eliminate {derivable} entries:")
    print(f"    Remaining: {remaining} entries")
    print(f"    Reduction: {derivable/len(raw_dict)*100:.1f}%")
    print()

    # Word frequency: many entries are for rare words
    # English has ~3000 core words covering 95% of text
    # How many Plover entries map to these core words?

    # Rough: count unique simple output words
    word_freq = Counter()
    for v in raw_dict.values():
        v_clean = v.strip().lower()
        if v_clean.isalpha():
            word_freq[v_clean] += 1

    # Top N words cover what % of entries?
    total_word_entries = sum(word_freq.values())
    cumulative = 0
    for n, (word, count) in enumerate(word_freq.most_common()):
        cumulative += count
        if n + 1 in (1000, 3000, 5000, 10000, 20000):
            print(f"  Top {n+1:5d} words cover {cumulative/total_word_entries*100:.1f}% "
                  f"of word entries ({cumulative}/{total_word_entries})")

    print()
    return remaining


# ─── Approach 5: Multi-strategy hybrid ───

def test_hybrid(entries, raw_dict):
    """
    Best of all approaches combined:
    1. Phonetic rules for regular derivations (suffix combos)
    2. Two-level hash for remaining entries
    3. Block-compressed strings
    4. LRU cache for hot pages
    """
    print("=" * 60)
    print("APPROACH 5: MULTI-STRATEGY HYBRID")
    print("=" * 60)

    # Strategy 1: Suffix rules (from approach 4)
    single_stroke = {k: v for k, v in raw_dict.items() if '/' not in k}
    multi_stroke = {k: v for k, v in raw_dict.items() if '/' in k}
    single_translations = set(raw_dict[k] for k in single_stroke)

    common_suffixes = ['ing', 'ed', 'er', 'est', 'ly', 'ment', 'ness', 'tion',
                       'sion', 'able', 'ible', 'ful', 'less', 'ous', 'ive',
                       'al', 'ial', 's', 'es', "'s"]

    suffix_derivable = set()
    for k, v in multi_stroke.items():
        v_clean = v.strip().lower()
        for suffix in common_suffixes:
            if v_clean.endswith(suffix):
                base = v_clean[:-len(suffix)]
                if base in single_translations or (base + 'e') in single_translations:
                    suffix_derivable.add(k)
                    break

    # Remaining after suffix rules
    remaining_dict = {k: v for k, v in raw_dict.items() if k not in suffix_derivable}
    remaining_entries = []
    for stroke_str in sorted(remaining_dict.keys()):
        strokes = tuple(parse_stroke(s) for s in stroke_str.split('/'))
        remaining_entries.append((strokes, remaining_dict[stroke_str]))
    remaining_entries.sort(key=lambda x: x[0])

    n_remaining = len(remaining_entries)

    # Strategy 2: Compressed page table for remaining
    all_trans = sorted(set(v for _, v in remaining_entries))
    trans_to_id = {t: i for i, t in enumerate(all_trans)}
    raw_strings = b'\x00'.join(t.encode('utf-8') for t in all_trans)

    # String table (block-compressed)
    str_blocks = []
    for i in range(0, len(raw_strings), 4096):
        str_blocks.append(zlib.compress(raw_strings[i:i+4096], 9))
    str_total = sum(len(b) for b in str_blocks) + len(str_blocks) * 4

    # Entries as compressed pages
    page_size = 128
    n_pages = math.ceil(n_remaining / page_size)
    total_pages_compressed = 0

    for p in range(n_pages):
        start = p * page_size
        end = min(start + page_size, n_remaining)
        page = remaining_entries[start:end]

        buf = bytearray()
        prev_strokes = ()
        for strokes, translation in page:
            tid = trans_to_id[translation]
            shared = 0
            for i in range(min(len(strokes), len(prev_strokes))):
                if strokes[i] == prev_strokes[i]:
                    shared += 1
                else:
                    break
            n_new = len(strokes) - shared
            buf.append(shared)
            buf.append(n_new)
            for s in strokes[shared:]:
                buf.extend(stroke_to_bytes(s))
            buf.extend(struct.pack('<H', tid & 0xFFFF))
            prev_strokes = strokes

        total_pages_compressed += len(zlib.compress(bytes(buf), 9))

    page_index = n_pages * 9

    # Suffix rule table: store which suffix strokes map to which suffixes
    # ~20 rules × ~8 bytes = ~160 bytes
    suffix_rules_size = len(common_suffixes) * 8

    # Base word lookup: need to know single-stroke → translation mapping
    # This is a subset of the full dict, already included in remaining_entries
    # (single-stroke entries are NOT removed by suffix rules)

    total_hybrid = total_pages_compressed + page_index + str_total + suffix_rules_size

    print(f"  Suffix rules remove: {len(suffix_derivable)} entries")
    print(f"  Remaining entries: {n_remaining}")
    print(f"  Components:")
    print(f"    Suffix rules: {suffix_rules_size/1024:.2f} KB")
    print(f"    Entry pages: {total_pages_compressed/1024:.1f} KB ({n_pages} pages)")
    print(f"    Page index: {page_index/1024:.1f} KB")
    print(f"    String table: {str_total/1024:.1f} KB")
    print(f"  TOTAL: {total_hybrid/1024:.1f} KB")
    print()

    # Full zlib strings variant
    str_zlib = len(zlib.compress(raw_strings, 9))
    total_hybrid_zlib = total_pages_compressed + page_index + str_zlib + suffix_rules_size
    print(f"  TOTAL (full zlib str): {total_hybrid_zlib/1024:.1f} KB")
    print(f"    (needs ~4KB RAM for decompressing string blocks)")
    print()

    return total_hybrid, total_hybrid_zlib


def main():
    dict_path = '/tmp/plover-main.json'
    with open(dict_path) as f:
        raw_dict = json.load(f)

    parsed = []
    for stroke_str in sorted(raw_dict.keys()):
        strokes = tuple(parse_stroke(s) for s in stroke_str.split('/'))
        parsed.append((strokes, raw_dict[stroke_str]))
    parsed.sort(key=lambda x: x[0])

    print(f"Plover: {len(raw_dict)} entries")
    print()

    # Run all approaches
    test_compressed_pages(parsed)
    test_two_level_hash(parsed)
    full_zlib, sectored = test_full_compressed(parsed)
    remaining = test_rule_reduction(raw_dict)
    hybrid, hybrid_zlib = test_hybrid(parsed, raw_dict)

    # Summary
    print("=" * 60)
    print("FINAL SUMMARY — ALL APPROACHES")
    print("=" * 60)
    print(f"  Target: 300 KB")
    print()

    results = [
        ("Compressed pages (128/pg)", None),  # printed inline
        ("Two-level hash", None),
        ("Full zlib blob", full_zlib),
        ("Sectored (4KB)", sectored),
        ("Hybrid (suffix rules + pages)", hybrid),
        ("Hybrid (suffix + full zlib)", hybrid_zlib),
    ]

    for name, size in results:
        if size:
            kb = size / 1024
            marker = "  ✓ FITS!" if kb <= 300 else f"  ({kb-300:+.0f} KB over)"
            print(f"  {name:40s}: {kb:8.1f} KB{marker}")


if __name__ == '__main__':
    main()

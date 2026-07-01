#!/usr/bin/env python3
"""Tests for dict_compiler.py — DAWG dictionary compiler."""

import json
import os
import struct
import sys
import tempfile

import pytest

# Ensure tools/ is importable
sys.path.insert(0, os.path.dirname(__file__))

from dict_compiler import (
    STENO_KEYS,
    FLAG_SPLIT_STORAGE,
    HEADER_SIZE,
    MAGIC,
    VERSION,
    build_dawg,
    build_string_table,
    compile_dictionary,
    compute_skip_counts,
    dawg_lookup_index,
    decompress_string_table,
    deserialize_edges,
    deserialize_value_array,
    get_dawg_traversal_order,
    lookup_string,
    parse_header,
    parse_stroke,
    parse_stroke_string,
    serialize_edges,
    serialize_header,
    serialize_value_array,
    trim_entries,
    verify_compilation,
)


# ─── Small test dictionaries ───

SMALL_DICT = {
    "S": "is",
    "T": "it",
    "K": "can",
    "W": "with",
    "H": "had",
    "R": "are",
    "TPHO": "no",
    "STPH": "then",
    "KAT": "cat",
    "TKOG": "dog",
}

MULTI_STROKE_DICT = {
    "S": "is",
    "T": "it",
    "KPA/HROL": "{}{-|}",
    "TPHO/WUPB": "no one",
    "K": "can",
}

MEDIUM_DICT = {
    "S": "is",
    "T": "it",
    "K": "can",
    "W": "with",
    "H": "had",
    "R": "are",
    "A": "a",
    "O": "oh",
    "E": "he",
    "U": "you",
    "TPHO": "no",
    "STPH": "then",
    "KAT": "cat",
    "TKOG": "dog",
    "HOUS": "house",
    "TPHAEUPL": "name",
    "HROS": "also",
    "TKPWRAET": "great",
    "SKEL": "school",
    "PLAS": "place",
}


def _make_temp_dict(d):
    """Write dict to temp JSON file, return path."""
    fd, path = tempfile.mkstemp(suffix='.json')
    with os.fdopen(fd, 'w') as f:
        json.dump(d, f)
    return path


def _parse_and_sort(d):
    """Parse dict entries and sort by stroke tuple."""
    entries = []
    for stroke_str, translation in d.items():
        strokes = parse_stroke_string(stroke_str)
        entries.append((strokes, translation))
    entries.sort(key=lambda x: x[0])
    return entries


# ─── Tests ───

class TestParseStroke:
    """Test stroke parsing."""

    def test_left_side_stph(self):
        """STPH → S + T + P + H left side bits."""
        val = parse_stroke("STPH")
        expected = STENO_KEYS['S-'] | STENO_KEYS['T-'] | STENO_KEYS['P-'] | STENO_KEYS['H-']
        assert val == expected

    def test_right_side_eurb(self):
        """EURB → E + U + R + B right side bits."""
        val = parse_stroke("EURB")
        expected = STENO_KEYS['-E'] | STENO_KEYS['-U'] | STENO_KEYS['-R'] | STENO_KEYS['-B']
        assert val == expected

    def test_single_s(self):
        """S → just S bit."""
        val = parse_stroke("S")
        assert val == STENO_KEYS['S-']

    def test_number_bar(self):
        """#STPH → number + S + T + P + H."""
        val = parse_stroke("#STPH")
        expected = (STENO_KEYS['#'] | STENO_KEYS['S-'] | STENO_KEYS['T-'] |
                    STENO_KEYS['P-'] | STENO_KEYS['H-'])
        assert val == expected

    def test_vowels(self):
        """AO → A + O vowel bits."""
        val = parse_stroke("AO")
        expected = STENO_KEYS['A-'] | STENO_KEYS['O-']
        assert val == expected

    def test_full_stroke(self):
        """STKPWHR → all left consonants."""
        val = parse_stroke("STKPWHR")
        expected = (STENO_KEYS['S-'] | STENO_KEYS['T-'] | STENO_KEYS['K-'] |
                    STENO_KEYS['P-'] | STENO_KEYS['W-'] | STENO_KEYS['H-'] |
                    STENO_KEYS['R-'])
        assert val == expected

    def test_star(self):
        """*E → star + E."""
        val = parse_stroke("*E")
        expected = STENO_KEYS['*'] | STENO_KEYS['-E']
        assert val == expected

    def test_multi_stroke_parse(self):
        """KPA/HROL parses to two stroke bitmasks."""
        strokes = parse_stroke_string("KPA/HROL")
        assert len(strokes) == 2
        # First stroke: K + P + A
        assert strokes[0] == (STENO_KEYS['K-'] | STENO_KEYS['P-'] | STENO_KEYS['A-'])


class TestBuildDawgSmall:
    """Test DAWG construction with small dictionary."""

    def test_node_compression(self):
        """DAWG should have fewer nodes than a plain trie (compression happening)."""
        entries = _parse_and_sort(SMALL_DICT)
        root, node_count, edge_count = build_dawg(entries)
        # With 10 entries, DAWG should have fewer nodes than
        # total path length (which would be ~15+ for a trie)
        assert node_count < 15
        assert node_count > 0
        assert edge_count > 0

    def test_all_entries_reachable(self):
        """All entries should be reachable via traversal."""
        entries = _parse_and_sort(SMALL_DICT)
        root, node_count, edge_count = build_dawg(entries)

        # Check each entry can be traversed
        for strokes, _trans in entries:
            node = root
            for stroke in strokes:
                assert stroke in node.edges, f"Missing edge for stroke in {strokes}"
                node = node.edges[stroke]
            assert node.final, f"Node not final for {strokes}"


class TestDawgLookup:
    """Test DAWG lookup via skip-count traversal."""

    def test_all_lookups_correct(self):
        """All entries should have unique sequential skip-count indices."""
        entries = _parse_and_sort(SMALL_DICT)
        root, _, _ = build_dawg(entries)
        skip_cache = compute_skip_counts(root)

        indices = []
        for strokes, _ in entries:
            idx = dawg_lookup_index(root, strokes, skip_cache)
            assert idx >= 0, f"Lookup failed for {strokes}"
            indices.append(idx)

        # All indices should be unique
        assert len(set(indices)) == len(indices), "Duplicate indices found"

        # Indices should be 0..n-1
        assert sorted(indices) == list(range(len(entries)))

    def test_missing_entry_returns_neg(self):
        """Looking up a non-existent stroke should return -1."""
        entries = _parse_and_sort(SMALL_DICT)
        root, _, _ = build_dawg(entries)
        skip_cache = compute_skip_counts(root)

        # A stroke not in the dict
        fake_strokes = (0xDEAD,)
        idx = dawg_lookup_index(root, fake_strokes, skip_cache)
        assert idx == -1

    def test_traversal_order_matches(self):
        """Traversal order should match sorted entries."""
        entries = _parse_and_sort(SMALL_DICT)
        root, _, _ = build_dawg(entries)

        traversal = get_dawg_traversal_order(root)
        entry_paths = [strokes for strokes, _ in entries]

        assert traversal == entry_paths


class TestBinaryRoundTrip:
    """Test compile → binary → deserialize → verify."""

    def test_small_dict_round_trip(self):
        """Compile small dict, deserialize, verify all lookups."""
        path = _make_temp_dict(SMALL_DICT)
        try:
            binary, stats, root, skip_cache, translations, parsed = \
                compile_dictionary(path, max_entries=100)

            assert stats['entry_count'] == len(SMALL_DICT)
            assert stats['total_size'] == len(binary)
            assert stats['total_size'] > HEADER_SIZE

            # Verify header
            header = parse_header(binary)
            assert header['magic'] == MAGIC
            assert header['version'] == VERSION
            assert header['entry_count'] == len(SMALL_DICT)

            # Verify round-trip
            correct, wrong, missing = verify_compilation(binary, parsed, root, skip_cache)
            assert correct == len(SMALL_DICT)
            assert wrong == 0
            assert missing == 0
        finally:
            os.unlink(path)

    def test_medium_dict_round_trip(self):
        """Medium dict round-trip."""
        path = _make_temp_dict(MEDIUM_DICT)
        try:
            binary, stats, root, skip_cache, translations, parsed = \
                compile_dictionary(path, max_entries=100)

            correct, wrong, missing = verify_compilation(binary, parsed, root, skip_cache)
            assert correct == len(MEDIUM_DICT)
            assert wrong == 0
            assert missing == 0
        finally:
            os.unlink(path)

    def test_edge_serialization_round_trip(self):
        """Edge bit-packing round-trip."""
        entries = _parse_and_sort(SMALL_DICT)
        root, node_count, edge_count = build_dawg(entries)

        edge_bytes, node_id_map, skip_cache = serialize_edges(root, node_count, edge_count)
        edges = deserialize_edges(edge_bytes, edge_count)

        assert len(edges) == edge_count
        # Each edge should have valid fields
        for stroke, target, skip, is_last in edges:
            assert 0 <= stroke <= 0xFFFF
            assert 0 <= target <= 0xFFFF
            assert 0 <= skip <= 0x1FFFF

    def test_string_table_round_trip(self):
        """String table compress/decompress round-trip."""
        translations = list(SMALL_DICT.values())
        table_bytes, offsets, raw_size = build_string_table(translations)
        raw = decompress_string_table(table_bytes)

        for i, trans in enumerate(translations):
            recovered = lookup_string(raw, offsets[i])
            assert recovered == trans, f"Mismatch at {i}: '{trans}' vs '{recovered}'"


class TestEntryTrimming:
    """Test entry trimming logic."""

    def test_trim_keeps_single_stroke(self):
        """With max_entries < total, single-stroke entries kept preferentially."""
        d = {}
        # 10 unique single-stroke entries (no duplicates)
        single_strokes = ["S", "T", "K", "W", "H", "A", "O", "-F", "-B", "-G"]
        for i, k in enumerate(single_strokes):
            d[k] = f"word_{i}"

        # 40 unique multi-stroke entries using distinct second strokes
        left = ["S", "T", "K", "P", "W", "H", "R", "SK", "TK", "PH"]
        right = ["-F", "-R", "-P", "-B"]
        idx = 0
        for lk in left:
            for rk in right:
                d[f"KAT/{lk}{rk}"] = f"long_translation_{idx}"
                idx += 1

        # Total = 50 entries
        entries = list(d.items())
        assert len(entries) == 50, f"Expected 50, got {len(entries)}"

        trimmed = trim_entries(entries, 30)
        assert len(trimmed) == 30

        # Count single vs multi in result
        single_count = sum(1 for s, _ in trimmed if '/' not in s)
        multi_count = sum(1 for s, _ in trimmed if '/' in s)

        # All 10 single-stroke entries should be kept
        assert single_count == 10
        assert multi_count == 20

    def test_no_trim_when_under_limit(self):
        """No trimming when entries < max_entries."""
        entries = list(SMALL_DICT.items())
        trimmed = trim_entries(entries, 1000)
        assert len(trimmed) == len(entries)

    def test_trim_multi_stroke_by_length(self):
        """Multi-stroke entries trimmed by translation length (shorter kept)."""
        d = {"S": "is"}  # 1 single-stroke
        # Add multi-stroke with varying translation lengths
        d["KAT/S"] = "ab"           # short
        d["KAT/T"] = "abcdefghij"   # long
        d["KAT/K"] = "abc"          # medium

        entries = list(d.items())
        trimmed = trim_entries(entries, 3)

        # Should keep: single("S"), then shortest multi-stroke
        assert len(trimmed) == 3
        trans = [t for _, t in trimmed]
        assert "is" in trans  # single stroke kept
        assert "ab" in trans  # shortest multi kept
        assert "abc" in trans  # medium kept
        assert "abcdefghij" not in trans  # longest dropped


class TestSplitStorageFlag:
    """Test --split-storage flag."""

    def test_flag_set_in_header(self):
        """split_storage flag should set bit 0 in header flags."""
        path = _make_temp_dict(SMALL_DICT)
        try:
            binary, stats, _, _, _, _ = compile_dictionary(
                path, max_entries=100, split_storage=True)

            header = parse_header(binary)
            assert header['flags'] & FLAG_SPLIT_STORAGE != 0
            assert stats['split_storage'] is True
        finally:
            os.unlink(path)

    def test_flag_not_set_by_default(self):
        """split_storage flag should NOT be set by default."""
        path = _make_temp_dict(SMALL_DICT)
        try:
            binary, stats, _, _, _, _ = compile_dictionary(
                path, max_entries=100, split_storage=False)

            header = parse_header(binary)
            assert header['flags'] & FLAG_SPLIT_STORAGE == 0
            assert stats['split_storage'] is False
        finally:
            os.unlink(path)


class TestMultiStroke:
    """Test multi-stroke entry handling."""

    def test_multi_stroke_compile_and_lookup(self):
        """Multi-stroke entries (e.g. KPA/HROL) should compile and look up correctly."""
        path = _make_temp_dict(MULTI_STROKE_DICT)
        try:
            binary, stats, root, skip_cache, translations, parsed = \
                compile_dictionary(path, max_entries=100)

            correct, wrong, missing = verify_compilation(binary, parsed, root, skip_cache)
            assert correct == len(MULTI_STROKE_DICT)
            assert wrong == 0
            assert missing == 0
        finally:
            os.unlink(path)

    def test_multi_stroke_traversal(self):
        """Multi-stroke entries should appear in correct traversal order."""
        entries = _parse_and_sort(MULTI_STROKE_DICT)
        root, _, _ = build_dawg(entries)
        skip_cache = compute_skip_counts(root)

        for strokes, _ in entries:
            idx = dawg_lookup_index(root, strokes, skip_cache)
            assert idx >= 0, f"Multi-stroke lookup failed: {strokes}"

    def test_kpa_hrol_specific(self):
        """KPA/HROL → {}{-|} specifically."""
        d = {"KPA/HROL": "{}{-|}"}
        entries = _parse_and_sort(d)
        root, _, _ = build_dawg(entries)
        skip_cache = compute_skip_counts(root)

        strokes = parse_stroke_string("KPA/HROL")
        idx = dawg_lookup_index(root, strokes, skip_cache)
        assert idx == 0  # only entry → index 0


class TestHeaderSerialization:
    """Test header pack/unpack."""

    def test_header_size(self):
        """Header should be exactly 32 bytes."""
        header = serialize_header(0, 100, 50, 200, 1000, 500, 800)
        assert len(header) == HEADER_SIZE

    def test_header_round_trip(self):
        """Header fields should survive pack/unpack."""
        header = serialize_header(
            flags=FLAG_SPLIT_STORAGE,
            entry_count=12345,
            node_count=6789,
            edge_count=11111,
            string_table_offset=99999,
            string_table_size=55555,
            value_array_offset=44444,
        )
        parsed = parse_header(header)
        assert parsed['magic'] == MAGIC
        assert parsed['version'] == VERSION
        assert parsed['flags'] == FLAG_SPLIT_STORAGE
        assert parsed['entry_count'] == 12345
        assert parsed['node_count'] == 6789
        assert parsed['edge_count'] == 11111
        assert parsed['string_table_offset'] == 99999
        assert parsed['string_table_size'] == 55555
        assert parsed['value_array_offset'] == 44444


if __name__ == '__main__':
    pytest.main([__file__, '-v'])

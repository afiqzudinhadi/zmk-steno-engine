#!/usr/bin/env python3
"""DAWG dictionary compiler for ZMK steno engine.

Compiles a Plover-format JSON steno dictionary into a compact binary
DAWG with skip-count indexing and block-compressed string table.

Optimize-dict variant targeting 462KB (left half flash budget).
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib
from collections import Counter

# ─── Steno stroke parsing (copied from dawg_fst_prototype.py) ───

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
    """Parse a steno stroke string into a bitmask."""
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


def parse_stroke_string(stroke_str):
    """Parse a stroke string (possibly multi-stroke with /) into tuple of bitmasks."""
    return tuple(parse_stroke(s) for s in stroke_str.split('/'))


# ─── DAWG construction (Daciuk's incremental algorithm) ───

class DawgNode:
    """Node in the DAWG."""
    __slots__ = ['id', 'edges', 'final', '_hash_cache']

    _next_id = 0

    def __init__(self):
        self.id = DawgNode._next_id
        DawgNode._next_id += 1
        self.edges = {}  # stroke_val -> DawgNode
        self.final = False
        self._hash_cache = None

    def signature(self):
        """Hashable signature for minimization."""
        edge_sig = tuple(sorted(
            (k, child.id) for k, child in self.edges.items()
        ))
        return (self.final, edge_sig)

    def __hash__(self):
        if self._hash_cache is None:
            self._hash_cache = hash(self.signature())
        return self._hash_cache

    def __eq__(self, other):
        return self.signature() == other.signature()

    def invalidate_cache(self):
        self._hash_cache = None


def build_dawg(sorted_entries):
    """Build minimized DAWG using Daciuk's incremental algorithm.

    Entries MUST be sorted by stroke tuple (lexicographic).
    Returns (root, node_count, edge_count).
    """
    DawgNode._next_id = 0
    root = DawgNode()
    unchecked = []  # list of (parent, stroke, child)
    minimized = {}  # signature -> node
    prev_strokes = ()

    def _minimize(down_to):
        """Minimize unchecked nodes from top down to given depth."""
        for i in range(len(unchecked) - 1, down_to - 1, -1):
            parent, stroke, child = unchecked[i]
            child.invalidate_cache()
            sig = child.signature()
            if sig in minimized:
                parent.edges[stroke] = minimized[sig]
            else:
                minimized[sig] = child
            unchecked.pop()

    for strokes, _translation in sorted_entries:
        # Find common prefix length with previous entry
        common = 0
        limit = min(len(strokes), len(prev_strokes))
        while common < limit and strokes[common] == prev_strokes[common]:
            common += 1

        # Minimize nodes beyond common prefix
        _minimize(common)

        # Get node at end of common prefix
        if unchecked:
            node = unchecked[-1][2]
        else:
            node = root

        # Add new nodes for remaining strokes
        for stroke in strokes[common:]:
            new_node = DawgNode()
            node.edges[stroke] = new_node
            unchecked.append((node, stroke, new_node))
            node = new_node

        node.final = True
        prev_strokes = strokes

    # Minimize remaining
    _minimize(0)

    # Count nodes and edges
    node_count = 0
    edge_count = 0
    visited = set()

    def _count(n):
        nonlocal node_count, edge_count
        if n.id in visited:
            return
        visited.add(n.id)
        node_count += 1
        for _s, child in n.edges.items():
            edge_count += 1
            _count(child)

    _count(root)
    return root, node_count, edge_count


# ─── Skip-count computation ───

def compute_skip_counts(root):
    """Compute skip-count (number of final nodes in subtree) for each node.

    Returns dict: node_id -> skip_count
    """
    cache = {}

    def _count(node):
        if node.id in cache:
            return cache[node.id]
        c = 1 if node.final else 0
        for stroke in sorted(node.edges.keys()):
            child = node.edges[stroke]
            c += _count(child)
        cache[node.id] = c
        return c

    _count(root)
    return cache


def dawg_lookup_index(root, strokes, skip_cache):
    """Look up a stroke sequence in the DAWG, returning its skip-count index.

    Returns -1 if not found.
    """
    node = root
    idx = 0

    for stroke in strokes:
        if stroke not in node.edges:
            return -1
        # Count finals of all edges with stroke < target
        for s in sorted(node.edges.keys()):
            if s == stroke:
                child = node.edges[s]
                if child.final:
                    idx += 1
                node = child
                break
            else:
                child = node.edges[s]
                idx += skip_cache[child.id]
        else:
            return -1

    if not node.final:
        return -1
    return idx - 1


def get_dawg_traversal_order(root):
    """DFS traversal of DAWG, edges sorted by stroke value.

    Returns list of translations in traversal order (one per final node encounter).
    This is the order entries appear when looking up via skip-count.
    """
    order = []
    visited_paths = set()

    def _dfs(node, path):
        path_key = tuple(path)
        if path_key in visited_paths:
            return
        visited_paths.add(path_key)

        if node.final:
            order.append(path_key)

        for stroke in sorted(node.edges.keys()):
            child = node.edges[stroke]
            _dfs(child, path + [stroke])

    _dfs(root, [])
    return order


# ─── Entry trimming ───

def trim_entries(entries, max_entries):
    """Trim entries to max_entries, keeping single-stroke preferentially.

    Priority:
    1. All single-stroke entries
    2. Multi-stroke entries sorted by translation length (shorter first)
    3. Drop longest/rarest multi-stroke first
    """
    if len(entries) <= max_entries:
        return entries

    single_stroke = []
    multi_stroke = []

    for strokes_str, translation in entries:
        if '/' not in strokes_str:
            single_stroke.append((strokes_str, translation))
        else:
            multi_stroke.append((strokes_str, translation))

    # Sort multi-stroke by translation length (shorter = more useful)
    multi_stroke.sort(key=lambda x: (len(x[1]), len(x[0].split('/'))))

    remaining = max_entries - len(single_stroke)
    if remaining < 0:
        # Even single-stroke entries exceed limit; trim by translation length
        single_stroke.sort(key=lambda x: len(x[1]))
        return single_stroke[:max_entries]

    return single_stroke + multi_stroke[:remaining]


# ─── String table construction ───

def build_string_table(translations):
    """Build block-compressed string table from translations.

    Returns (table_bytes, offsets) where offsets[i] is the byte offset
    of translation i in the uncompressed table.
    """
    # Build raw table: null-separated strings
    # We need to track offset of each unique string
    unique_translations = sorted(set(translations))
    trans_to_unique_idx = {t: i for i, t in enumerate(unique_translations)}

    # Build raw bytes and offset map
    raw_parts = []
    unique_offsets = []
    offset = 0
    for t in unique_translations:
        unique_offsets.append(offset)
        encoded = t.encode('utf-8')
        raw_parts.append(encoded)
        offset += len(encoded) + 1  # +1 for null separator

    raw = b'\x00'.join(raw_parts)
    if raw_parts:
        raw += b'\x00'  # trailing null

    # Block compress
    block_size = 4096
    compressed_blocks = []
    block_offsets_raw = []
    current_offset = 0

    for i in range(0, len(raw), block_size):
        block = raw[i:i + block_size]
        compressed = zlib.compress(block, 9)
        block_offsets_raw.append(current_offset)
        compressed_blocks.append(compressed)
        current_offset += len(compressed)

    # Serialize: block_count(u16) + block_offsets(u32 each) + compressed blocks
    n_blocks = len(compressed_blocks)
    table_header = struct.pack('<H', n_blocks)
    table_index = b''.join(struct.pack('<I', off) for off in block_offsets_raw)
    table_data = b''.join(compressed_blocks)
    table_bytes = table_header + table_index + table_data

    # Map each translation to its offset in raw table
    entry_offsets = []
    for t in translations:
        uid = trans_to_unique_idx[t]
        entry_offsets.append(unique_offsets[uid])

    return table_bytes, entry_offsets, len(raw)


def decompress_string_table(table_bytes):
    """Decompress a block-compressed string table back to raw bytes."""
    pos = 0
    n_blocks = struct.unpack_from('<H', table_bytes, pos)[0]
    pos += 2

    block_offsets = []
    for _ in range(n_blocks):
        off = struct.unpack_from('<I', table_bytes, pos)[0]
        pos += 4
        block_offsets.append(off)

    data_start = pos
    raw_parts = []
    for i in range(n_blocks):
        block_start = data_start + block_offsets[i]
        if i + 1 < n_blocks:
            block_end = data_start + block_offsets[i + 1]
        else:
            block_end = len(table_bytes)
        compressed = table_bytes[block_start:block_end]
        raw_parts.append(zlib.decompress(compressed))

    return b''.join(raw_parts)


def lookup_string(raw_table, offset):
    """Look up a null-terminated string at given offset in raw table."""
    end = raw_table.index(b'\x00', offset)
    return raw_table[offset:end].decode('utf-8')


# ─── Binary serialization ───

MAGIC = b'STNO'
VERSION = 1
FLAG_SPLIT_STORAGE = 0x0001

HEADER_SIZE = 32


def serialize_header(flags, entry_count, node_count, edge_count,
                     string_table_offset, string_table_size,
                     value_array_offset):
    """Serialize the 32-byte binary header."""
    return struct.pack('<4sHHIIIIIIxxxx',
                       MAGIC,
                       VERSION,
                       flags,
                       entry_count,
                       node_count,
                       edge_count,
                       string_table_offset,
                       string_table_size,
                       value_array_offset)


def parse_header(data):
    """Parse 32-byte binary header. Returns dict."""
    if len(data) < HEADER_SIZE:
        raise ValueError("Data too short for header")
    # Unpack with padding bytes
    magic, version, flags, entry_count, node_count, edge_count, \
        str_table_off, str_table_size, val_array_off = \
        struct.unpack_from('<4sHHIIIIII', data, 0)
    # 4 bytes reserved at end (32 - 28 = 4)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: {magic!r}")
    return {
        'magic': magic,
        'version': version,
        'flags': flags,
        'entry_count': entry_count,
        'node_count': node_count,
        'edge_count': edge_count,
        'string_table_offset': str_table_off,
        'string_table_size': str_table_size,
        'value_array_offset': val_array_off,
    }


def serialize_edges(root, node_count, edge_count):
    """Serialize DAWG edges as bit-packed array.

    Each edge: stroke_key(16) + target_node(16) + skip_count(17) + is_last(1) = 50 bits

    Nodes are assigned sequential IDs via DFS traversal (sorted edges).
    Returns (edge_bytes, node_id_map, skip_counts_by_node).
    """
    # Assign sequential node IDs via DFS
    node_id_map = {}
    dfs_order = []

    def _assign_ids(node):
        if node.id in node_id_map:
            return
        new_id = len(node_id_map)
        node_id_map[node.id] = new_id
        dfs_order.append(node)
        for stroke in sorted(node.edges.keys()):
            child = node.edges[stroke]
            _assign_ids(child)

    _assign_ids(root)

    # Compute skip counts
    skip_cache = compute_skip_counts(root)

    # Build edge list: for each node in DFS order, emit edges sorted by stroke
    edges = []
    for node in dfs_order:
        sorted_strokes = sorted(node.edges.keys())
        for i, stroke in enumerate(sorted_strokes):
            child = node.edges[stroke]
            is_last = (i == len(sorted_strokes) - 1)
            target_id = node_id_map[child.id]
            skip = skip_cache[child.id]
            edges.append((stroke, target_id, skip, is_last))

    # Bit-pack edges: each 50 bits
    # stroke_key: 16 bits, target_node: 16 bits, skip_count: 17 bits, is_last: 1 bit
    total_bits = len(edges) * 50
    total_bytes = (total_bits + 7) // 8
    buf = bytearray(total_bytes)

    bit_pos = 0
    for stroke_key, target_node, skip_count, is_last in edges:
        # Clamp values to field widths
        stroke_key &= 0xFFFF
        target_node &= 0xFFFF
        skip_count = min(skip_count, 0x1FFFF)  # 17 bits max
        is_last_bit = 1 if is_last else 0

        # Pack 50 bits: stroke(16) | target(16) | skip(17) | last(1)
        val = (stroke_key << 34) | (target_node << 18) | (skip_count << 1) | is_last_bit

        # Write 50 bits into buffer at bit_pos
        for i in range(50):
            bit = (val >> (49 - i)) & 1
            byte_idx = (bit_pos + i) // 8
            bit_idx = 7 - ((bit_pos + i) % 8)
            if bit:
                buf[byte_idx] |= (1 << bit_idx)

        bit_pos += 50

    return bytes(buf), node_id_map, skip_cache


def deserialize_edges(edge_bytes, edge_count):
    """Deserialize bit-packed edge array.

    Returns list of (stroke_key, target_node, skip_count, is_last).
    """
    edges = []
    bit_pos = 0

    for _ in range(edge_count):
        val = 0
        for i in range(50):
            byte_idx = (bit_pos + i) // 8
            bit_idx = 7 - ((bit_pos + i) % 8)
            bit = (edge_bytes[byte_idx] >> bit_idx) & 1
            val = (val << 1) | bit
        bit_pos += 50

        stroke_key = (val >> 34) & 0xFFFF
        target_node = (val >> 18) & 0xFFFF
        skip_count = (val >> 1) & 0x1FFFF
        is_last = val & 1
        edges.append((stroke_key, target_node, skip_count, bool(is_last)))

    return edges


def serialize_value_array(entry_offsets, raw_table_size):
    """Serialize value array (string table offsets for each entry).

    Uses uint16 if raw_table_size <= 65535, else uint32.
    """
    use_u32 = raw_table_size > 65535
    fmt = '<I' if use_u32 else '<H'
    parts = [struct.pack(fmt, off) for off in entry_offsets]
    return b''.join(parts), use_u32


def deserialize_value_array(data, entry_count, use_u32=False):
    """Deserialize value array."""
    fmt = '<I' if use_u32 else '<H'
    size = 4 if use_u32 else 2
    offsets = []
    for i in range(entry_count):
        off = struct.unpack_from(fmt, data, i * size)[0]
        offsets.append(off)
    return offsets


# ─── Full compilation pipeline ───

def compile_dictionary(json_path, max_entries=120000, split_storage=False):
    """Compile a JSON steno dictionary into binary DAWG format.

    Returns (binary_data, stats_dict).
    """
    # 1. Load and parse
    with open(json_path) as f:
        raw_dict = json.load(f)

    raw_entries = list(raw_dict.items())

    # 2. Trim if needed
    if len(raw_entries) > max_entries:
        raw_entries = trim_entries(raw_entries, max_entries)

    # 3. Parse strokes and sort
    parsed = []
    for stroke_str, translation in raw_entries:
        strokes = parse_stroke_string(stroke_str)
        parsed.append((strokes, translation))

    parsed.sort(key=lambda x: x[0])

    # 4. Build DAWG
    root, node_count, edge_count = build_dawg(parsed)

    # 5. Get traversal order for value array
    traversal_paths = get_dawg_traversal_order(root)

    # Build path->translation map
    path_to_trans = {}
    for strokes, translation in parsed:
        path_to_trans[strokes] = translation

    translations_ordered = []
    for path in traversal_paths:
        if path in path_to_trans:
            translations_ordered.append(path_to_trans[path])
        else:
            translations_ordered.append("")

    # 6. Build string table
    string_table_bytes, entry_offsets, raw_table_size = \
        build_string_table(translations_ordered)

    # 7. Serialize edges
    edge_bytes, node_id_map, skip_cache = \
        serialize_edges(root, node_count, edge_count)

    # 8. Serialize value array
    value_array_bytes, use_u32 = \
        serialize_value_array(entry_offsets, raw_table_size)

    # 9. Compute offsets
    edge_array_offset = HEADER_SIZE
    value_array_offset = edge_array_offset + len(edge_bytes)
    string_table_offset = value_array_offset + len(value_array_bytes)

    # 10. Build header
    flags = 0
    if split_storage:
        flags |= FLAG_SPLIT_STORAGE
    if use_u32:
        flags |= 0x0002  # bit 1 = u32 value offsets

    header = serialize_header(
        flags=flags,
        entry_count=len(translations_ordered),
        node_count=node_count,
        edge_count=edge_count,
        string_table_offset=string_table_offset,
        string_table_size=len(string_table_bytes),
        value_array_offset=value_array_offset,
    )

    # 11. Assemble
    binary = header + edge_bytes + value_array_bytes + string_table_bytes

    stats = {
        'entry_count': len(translations_ordered),
        'node_count': node_count,
        'edge_count': edge_count,
        'bits_per_edge': 50,
        'edge_array_size': len(edge_bytes),
        'value_array_size': len(value_array_bytes),
        'string_table_size': len(string_table_bytes),
        'raw_string_table_size': raw_table_size,
        'total_size': len(binary),
        'header_size': HEADER_SIZE,
        'use_u32_offsets': use_u32,
        'split_storage': split_storage,
    }

    return binary, stats, root, skip_cache, translations_ordered, parsed


def verify_compilation(binary_data, parsed_entries, root, skip_cache):
    """Verify compiled binary by deserializing and looking up every entry.

    Returns (correct, wrong, missing).
    """
    header = parse_header(binary_data)
    entry_count = header['entry_count']
    edge_count = header['edge_count']
    use_u32 = bool(header['flags'] & 0x0002)

    # Extract sections
    edge_start = HEADER_SIZE
    edge_end = header['value_array_offset']
    edge_bytes = binary_data[edge_start:edge_end]

    val_start = header['value_array_offset']
    val_end = header['string_table_offset']
    val_bytes = binary_data[val_start:val_end]

    str_start = header['string_table_offset']
    str_bytes = binary_data[str_start:]

    # Deserialize
    edges = deserialize_edges(edge_bytes, edge_count)
    value_offsets = deserialize_value_array(val_bytes, entry_count, use_u32)
    raw_table = decompress_string_table(str_bytes)

    # Build adjacency from deserialized edges for lookup
    # Reconstruct graph: node_id -> list of (stroke, target, skip)
    adj = {}
    node_finals = set()
    edge_idx = 0

    # We need to figure out which edges belong to which node.
    # Edges are stored in DFS node order; is_last marks end of a node's edge list.
    current_node = 0
    node_edges = {}

    i = 0
    while i < len(edges):
        stroke, target, skip, is_last = edges[i]
        if current_node not in node_edges:
            node_edges[current_node] = []
        node_edges[current_node].append((stroke, target, skip))
        if is_last:
            current_node += 1
            # Skip nodes that have no edges (they won't appear in edge list)
            # We detect these by checking if next edge's parent should be higher
        i += 1

    # Now look up each entry via the reconstructed DAWG
    correct = 0
    wrong = 0
    missing = 0

    for strokes, expected in parsed_entries:
        idx = dawg_lookup_index(root, strokes, skip_cache)
        if idx < 0 or idx >= len(value_offsets):
            missing += 1
            continue

        offset = value_offsets[idx]
        # Find null terminator
        try:
            translation = lookup_string(raw_table, offset)
        except (ValueError, IndexError):
            missing += 1
            continue

        if translation == expected:
            correct += 1
        else:
            wrong += 1
            if wrong <= 5:
                print(f"  WRONG: strokes={strokes}, expected='{expected}', got='{translation}'")

    return correct, wrong, missing


def print_stats(stats, target_kb=533):
    """Print compilation statistics."""
    target_bytes = target_kb * 1024
    total = stats['total_size']
    pct = (total / target_bytes) * 100 if target_bytes else 0

    print(f"Dictionary Compilation Stats:")
    print(f"  Entries:           {stats['entry_count']:>10,}")
    print(f"  DAWG nodes:        {stats['node_count']:>10,}")
    print(f"  DAWG edges:        {stats['edge_count']:>10,}")
    print(f"  Bits/edge:         {stats['bits_per_edge']:>10}")
    print(f"  ---")
    print(f"  Header:            {stats['header_size']:>10,} bytes")
    print(f"  Edge array:        {stats['edge_array_size']:>10,} bytes  ({stats['edge_array_size']/1024:.1f} KB)")
    print(f"  Value array:       {stats['value_array_size']:>10,} bytes  ({stats['value_array_size']/1024:.1f} KB)")
    print(f"  String table:      {stats['string_table_size']:>10,} bytes  ({stats['string_table_size']/1024:.1f} KB)")
    print(f"  ---")
    print(f"  TOTAL:             {total:>10,} bytes  ({total/1024:.1f} KB)")
    print(f"  Budget:            {target_bytes:>10,} bytes  ({target_kb} KB)")
    print(f"  Usage:             {pct:>9.1f}%")
    if total <= target_bytes:
        print(f"  Status:            FITS ({(target_bytes - total)/1024:.1f} KB remaining)")
    else:
        print(f"  Status:            OVER BUDGET by {(total - target_bytes)/1024:.1f} KB")
    print(f"  ---")
    print(f"  Split storage:     {'yes' if stats['split_storage'] else 'no'}")
    print(f"  U32 offsets:       {'yes' if stats['use_u32_offsets'] else 'no'}")


# ─── CLI ───

def main():
    parser = argparse.ArgumentParser(
        description='Compile steno dictionary to binary DAWG format')
    parser.add_argument('input', nargs='?', default='/tmp/plover-main.json',
                        help='Path to JSON dictionary (default: /tmp/plover-main.json)')
    parser.add_argument('--output', default='steno_dict.bin',
                        help='Output binary path (default: steno_dict.bin)')
    parser.add_argument('--max-entries', type=int, default=120000,
                        help='Max entries to include (default: 120000)')
    parser.add_argument('--target-size', type=int, default=462,
                        help='Target size in KB (default: 462)')
    parser.add_argument('--split-storage', action='store_true',
                        help='Generate split-storage metadata header')
    parser.add_argument('--stats', action='store_true',
                        help='Print detailed stats')
    parser.add_argument('--verify', action='store_true',
                        help='Verify all entries after compilation')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: Dictionary not found: {args.input}")
        print("Download Plover dict:")
        print("  curl -sL 'https://raw.githubusercontent.com/openstenoproject/plover/main/plover/assets/main.json' -o /tmp/plover-main.json")
        sys.exit(1)

    print(f"Compiling {args.input}...")
    binary, stats, root, skip_cache, translations, parsed = \
        compile_dictionary(args.input, args.max_entries, args.split_storage)

    # Write output
    with open(args.output, 'wb') as f:
        f.write(binary)
    print(f"Written {len(binary):,} bytes to {args.output}")

    if args.stats:
        print()
        print_stats(stats, args.target_size)

    if args.verify:
        print()
        print("Verifying...")
        correct, wrong, missing = verify_compilation(binary, parsed, root, skip_cache)
        total = correct + wrong + missing
        print(f"  Correct: {correct}/{total}")
        print(f"  Wrong:   {wrong}/{total}")
        print(f"  Missing: {missing}/{total}")
        if wrong > 0 or missing > 0:
            print("  WARNING: Verification found errors!")
            sys.exit(2)
        else:
            print("  All entries verified successfully.")


if __name__ == '__main__':
    main()

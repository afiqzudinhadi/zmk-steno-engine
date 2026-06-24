#!/usr/bin/env python3
"""Prototype: DAWG with FST-style edge outputs.

DAWG handles key structure (22K nodes, 87.8% suffix sharing).
Outputs encoded directly on edges — no separate value index.

When traversing stroke sequence, accumulate output fragments from edges.
Final node's output = concatenation of all edge outputs along path.

String dedup: outputs reference into block-compressed string table.
"""

import json
import struct
import sys
import zlib
import math
import os
from collections import Counter, defaultdict

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


# ─── DAWG with outputs ───

class DawgNode:
    next_id = 0
    def __init__(self):
        self.id = DawgNode.next_id
        DawgNode.next_id += 1
        self.edges = {}      # stroke_val -> (DawgNode, output_str)
        self.final = False
        self.final_output = ""  # remaining output at final state

    def signature(self):
        """Signature for minimization — includes outputs."""
        edge_sig = tuple(sorted(
            (k, child.id, out) for k, (child, out) in self.edges.items()
        ))
        return (self.final, self.final_output if self.final else "", edge_sig)

    def __hash__(self):
        return hash(self.signature())

    def __eq__(self, other):
        return self.signature() == other.signature()


def build_dawg_fst(entries):
    """
    Build DAWG with FST-style outputs.

    For each entry (strokes, translation):
    - Walk trie path for strokes
    - Attach output to FIRST edge (FST convention: push output left)
    - At final node, store remaining output

    Then minimize: merge nodes with identical futures (including outputs).

    This is a simplified FST construction — not fully optimal but
    captures most of the savings.
    """
    DawgNode.next_id = 0

    # Phase 1: Build trie with outputs
    root = DawgNode()

    for strokes, translation in entries:
        node = root
        for i, stroke in enumerate(strokes):
            if stroke not in node.edges:
                new_node = DawgNode()
                node.edges[stroke] = (new_node, "")
                node = new_node
            else:
                node = node.edges[stroke][0]
        node.final = True
        node.final_output = translation

    # Phase 2: Push outputs to edges (left-push)
    # For each node, find common prefix of all outputs reachable,
    # push that prefix to the incoming edge, strip from descendants.
    # This enables more suffix sharing.

    def push_outputs(node, depth=0):
        """Push common output prefixes toward the root."""
        if not node.edges:
            return

        # First recurse into children
        for stroke, (child, out) in list(node.edges.items()):
            push_outputs(child, depth + 1)

        # For each child, collect all outputs reachable from it
        for stroke, (child, edge_out) in list(node.edges.items()):
            if child.final and not child.edges:
                # Leaf: output = edge_out + final_output
                full_out = edge_out + child.final_output
                node.edges[stroke] = (child, full_out)
                child.final_output = ""

    push_outputs(root)

    # Phase 3: Minimize (merge identical subtrees including outputs)
    minimized = {}

    def minimize_node(node, visited=None):
        if visited is None:
            visited = set()
        if node.id in visited:
            return node
        visited.add(node.id)

        # First minimize children
        for stroke, (child, out) in list(node.edges.items()):
            minimized_child = minimize_node(child, visited)
            node.edges[stroke] = (minimized_child, out)

        # Check if we've seen an equivalent node
        sig = node.signature()
        if sig in minimized:
            return minimized[sig]
        minimized[sig] = node
        return node

    root = minimize_node(root)

    return root


def measure_dawg_fst(root):
    """Measure the DAWG-FST structure."""
    nodes = set()
    edges = 0
    total_output_bytes = 0
    output_lengths = []
    unique_outputs = set()

    def visit(node):
        nonlocal edges, total_output_bytes
        if node.id in nodes:
            return
        nodes.add(node.id)
        for stroke, (child, output) in node.edges.items():
            edges += 1
            out_bytes = output.encode('utf-8')
            total_output_bytes += len(out_bytes)
            output_lengths.append(len(out_bytes))
            if output:
                unique_outputs.add(output)
            visit(child)
        if node.final and node.final_output:
            total_output_bytes += len(node.final_output.encode('utf-8'))
            output_lengths.append(len(node.final_output.encode('utf-8')))
            unique_outputs.add(node.final_output)

    visit(root)
    return {
        'nodes': len(nodes),
        'edges': edges,
        'total_output_bytes': total_output_bytes,
        'unique_outputs': len(unique_outputs),
        'avg_output_len': sum(output_lengths) / max(len(output_lengths), 1),
        'output_lengths': output_lengths,
    }


def verify_dawg_fst(root, entries):
    """Verify lookups return correct translations."""
    correct = 0
    wrong = 0
    missing = 0

    for strokes, expected in entries:
        node = root
        output = ""
        found = True
        for stroke in strokes:
            if stroke in node.edges:
                child, edge_out = node.edges[stroke]
                output += edge_out
                node = child
            else:
                found = False
                break

        if found and node.final:
            output += node.final_output
            if output == expected:
                correct += 1
            else:
                wrong += 1
                if wrong <= 5:
                    print(f"  WRONG: expected '{expected}', got '{output}'")
        else:
            missing += 1
            if missing <= 5:
                print(f"  MISSING: {strokes} -> '{expected}'")

    return correct, wrong, missing


def estimate_binary_size(stats, n_unique_strokes):
    """Estimate binary encoding size."""
    n_nodes = stats['nodes']
    n_edges = stats['edges']

    cbits = max(1, math.ceil(math.log2(max(n_unique_strokes, 2))))
    abits = max(1, math.ceil(math.log2(max(n_nodes, 2))))

    # Edge encoding: stroke_key + target_node + output_ref
    # output_ref: index into output string table
    unique_outputs = stats['unique_outputs']
    obits = max(1, math.ceil(math.log2(max(unique_outputs + 1, 2))))  # +1 for "no output"

    bits_per_edge = cbits + abits + obits + 1  # +1 for last-edge flag
    total_edge_bits = n_edges * bits_per_edge

    # Node overhead: 1 bit for is_final, 1 bit for has_final_output
    node_bits = n_nodes * 2

    # Final outputs: nodes with final_output need an output reference
    # Approximate: ~50% of final nodes have output
    final_output_bits = n_nodes * obits // 4  # rough

    structure_bits = total_edge_bits + node_bits + final_output_bits
    structure_bytes = (structure_bits + 7) // 8

    return {
        'cbits': cbits,
        'abits': abits,
        'obits': obits,
        'bits_per_edge': bits_per_edge,
        'structure_bytes': structure_bytes,
    }


def build_output_string_table(root):
    """Collect all unique output strings and build compressed table."""
    outputs = set()

    def visit(node, visited=None):
        if visited is None:
            visited = set()
        if node.id in visited:
            return
        visited.add(node.id)
        for stroke, (child, output) in node.edges.items():
            if output:
                outputs.add(output)
            visit(child, visited)
        if node.final and node.final_output:
            outputs.add(node.final_output)

    visit(root)

    sorted_outputs = sorted(outputs)

    # Raw
    raw = b'\x00'.join(o.encode('utf-8') for o in sorted_outputs)
    raw_size = len(raw)

    # Block compressed
    block_size = 4096
    blocks = []
    for i in range(0, len(raw), block_size):
        block = raw[i:i+block_size]
        blocks.append(zlib.compress(block, 9))
    compressed_size = sum(len(b) for b in blocks)
    index_size = len(blocks) * 4

    # Full zlib
    full_zlib_size = len(zlib.compress(raw, 9))

    return {
        'unique_count': len(sorted_outputs),
        'raw_size': raw_size,
        'block_compressed': compressed_size + index_size,
        'full_zlib': full_zlib_size,
    }


# ─── Alternative: DAWG keys + implicit index + compressed values ───

def build_dawg_implicit_index(entries):
    """
    Standard DAWG (no outputs on edges) but with implicit indexing.

    DAWG traversal counts reachable final nodes → gives entry index.
    Entry index maps to value via simple array lookup.

    Values stored as: entry_index → string_table_offset
    String table block-compressed.

    Key difference from benchmark.py: here we build actual DAWG
    and measure REAL node count, then compute skip-count based size.
    """
    DawgNode.next_id = 0
    root = DawgNode()
    unchecked = []
    minimized = {}
    prev_strokes = []

    def minimize(down_to):
        for i in range(len(unchecked) - 1, down_to - 1, -1):
            parent, stroke, child = unchecked[i]
            sig = (child.final, tuple(sorted((k, v.id) for k, (v, _) in child.edges.items())))
            if sig in minimized:
                existing = minimized[sig]
                parent.edges[stroke] = (existing, "")
            else:
                minimized[sig] = child
            unchecked.pop()

    for strokes, translation in entries:
        common = 0
        for i in range(min(len(strokes), len(prev_strokes))):
            if strokes[i] != prev_strokes[i]:
                break
            common += 1
        else:
            common = min(len(strokes), len(prev_strokes))

        minimize(common)

        if unchecked:
            node = unchecked[-1][2]
        else:
            node = root

        for stroke in strokes[common:]:
            new_node = DawgNode()
            node.edges[stroke] = (new_node, "")
            unchecked.append((node, stroke, new_node))
            node = new_node

        node.final = True
        prev_strokes = strokes

    minimize(0)

    # Count reachable finals for skip-count indexing
    def count_finals(node, cache=None):
        if cache is None:
            cache = {}
        if node.id in cache:
            return cache[node.id]
        c = 1 if node.final else 0
        for stroke in sorted(node.edges.keys()):
            child, _ = node.edges[stroke]
            c += count_finals(child, cache)
        cache[node.id] = c
        return c

    count_finals(root)

    return root


def verify_implicit_index(root, entries):
    """Verify skip-count indexing gives correct sequential indices."""

    def lookup_index(node, strokes):
        """Return the skip-count index for a stroke sequence."""
        idx = 0
        for stroke in strokes:
            # Count finals of all children with stroke < target
            for s in sorted(node.edges.keys()):
                if s == stroke:
                    child, _ = node.edges[s]
                    if child.final:
                        # This child's final state comes before its children
                        pass
                    node = child
                    if node.final:
                        idx += 1  # count this final state
                    break
                else:
                    child, _ = node.edges[s]
                    idx += count_subtree_finals(child)
            else:
                return -1
        return idx - 1 if node.final else -1

    def count_subtree_finals(node, cache={}):
        if node.id in cache:
            return cache[node.id]
        c = 1 if node.final else 0
        for s in sorted(node.edges.keys()):
            child, _ = node.edges[s]
            c += count_subtree_finals(child, cache)
        cache[node.id] = c
        return c

    # Verify first 100 entries get sequential indices
    correct = 0
    for expected_idx, (strokes, translation) in enumerate(entries[:100]):
        got_idx = lookup_index(root, strokes)
        if got_idx == expected_idx:
            correct += 1

    return correct


# ─── Main ───

def main():
    dict_path = '/tmp/plover-main.json'
    if not os.path.exists(dict_path):
        print("ERROR: Download Plover dict first:")
        print("  curl -sL 'https://raw.githubusercontent.com/openstenoproject/plover/main/plover/assets/main.json' -o /tmp/plover-main.json")
        sys.exit(1)

    with open(dict_path) as f:
        raw_dict = json.load(f)

    # Parse and sort
    parsed = []
    unique_stroke_vals = set()
    for stroke_str in sorted(raw_dict.keys()):
        strokes = tuple(parse_stroke(s) for s in stroke_str.split('/'))
        for s in strokes:
            unique_stroke_vals.add(s)
        parsed.append((strokes, raw_dict[stroke_str]))
    parsed.sort(key=lambda x: x[0])

    n_entries = len(parsed)
    n_unique_strokes = len(unique_stroke_vals)
    translations = [v for _, v in parsed]

    print(f"Entries: {n_entries}, Unique strokes: {n_unique_strokes}")
    print()

    # ─── Approach A: DAWG-FST (outputs on edges) ───
    print("=" * 60)
    print("APPROACH A: DAWG-FST (outputs on edges)")
    print("=" * 60)

    print("Building DAWG-FST...")
    root_fst = build_dawg_fst(parsed)

    stats = measure_dawg_fst(root_fst)
    print(f"  Nodes: {stats['nodes']}")
    print(f"  Edges: {stats['edges']}")
    print(f"  Unique output strings: {stats['unique_outputs']}")
    print(f"  Total output bytes (on edges): {stats['total_output_bytes']}")
    print(f"  Avg output length: {stats['avg_output_len']:.1f} bytes")

    # Verify correctness
    print("  Verifying lookups...")
    correct, wrong, missing = verify_dawg_fst(root_fst, parsed[:1000])
    print(f"  Verification (first 1000): {correct} correct, {wrong} wrong, {missing} missing")

    # Output string table
    str_table = build_output_string_table(root_fst)
    print(f"  Output string table:")
    print(f"    Unique strings: {str_table['unique_count']}")
    print(f"    Raw: {str_table['raw_size']/1024:.1f} KB")
    print(f"    Block-compressed: {str_table['block_compressed']/1024:.1f} KB")
    print(f"    Full zlib: {str_table['full_zlib']/1024:.1f} KB")

    # Binary size estimate
    bin_est = estimate_binary_size(stats, n_unique_strokes)
    print(f"  Binary encoding:")
    print(f"    cbits={bin_est['cbits']} abits={bin_est['abits']} obits={bin_est['obits']}")
    print(f"    Bits/edge: {bin_est['bits_per_edge']}")
    print(f"    Structure: {bin_est['structure_bytes']/1024:.1f} KB")

    total_a = bin_est['structure_bytes'] + str_table['block_compressed']
    print(f"  TOTAL: {total_a/1024:.1f} KB")
    print()

    # ─── Approach B: DAWG + implicit skip-count index ───
    print("=" * 60)
    print("APPROACH B: DAWG + skip-count index (no value array)")
    print("=" * 60)

    print("Building standard DAWG...")
    root_std = build_dawg_implicit_index(parsed)

    stats_std = measure_dawg_fst(root_std)  # reuse measurement fn
    print(f"  Nodes: {stats_std['nodes']}")
    print(f"  Edges: {stats_std['edges']}")

    # DAWG structure: edges need stroke + target + skip_count
    cbits = max(1, math.ceil(math.log2(max(n_unique_strokes, 2))))
    abits = max(1, math.ceil(math.log2(max(stats_std['nodes'], 2))))
    skipbits = max(1, math.ceil(math.log2(max(n_entries, 2))))

    # Compact: last-edge flag saves storing child count
    bits_per_edge = cbits + abits + skipbits + 1  # stroke + target + skip + last_edge
    structure_bits = stats_std['edges'] * bits_per_edge + stats_std['nodes'] * 1  # is_final per node
    structure_bytes = (structure_bits + 7) // 8

    print(f"  cbits={cbits} abits={abits} skipbits={skipbits}")
    print(f"  Bits/edge: {bits_per_edge}")
    print(f"  Structure: {structure_bytes/1024:.1f} KB")

    # Values: skip-count gives index → look up in ordered value array
    # Value array: translations in DAWG traversal order
    # Need: string table + offset array (index → string table position)
    unique_trans = sorted(set(translations))
    raw_strings = b'\x00'.join(t.encode('utf-8') for t in unique_trans)

    # Dedup: entry → unique_string_id
    trans_to_id = {t: i for i, t in enumerate(unique_trans)}
    dedup_array = [trans_to_id[t] for t in translations]
    dedup_bits = math.ceil(math.log2(len(unique_trans)))
    dedup_bytes = (n_entries * dedup_bits + 7) // 8

    # String table block-compressed
    blocks = []
    for i in range(0, len(raw_strings), 4096):
        blocks.append(zlib.compress(raw_strings[i:i+4096], 9))
    str_compressed = sum(len(b) for b in blocks) + len(blocks) * 4

    total_b = structure_bytes + dedup_bytes + str_compressed
    print(f"  Value dedup array: {dedup_bytes/1024:.1f} KB ({dedup_bits} bits/entry)")
    print(f"  String table: {str_compressed/1024:.1f} KB")
    print(f"  TOTAL: {total_b/1024:.1f} KB")
    print()

    # ─── Approach C: DAWG + skip-count + NO dedup array ───
    # Instead of dedup array, store string offset directly in DAWG final nodes
    print("=" * 60)
    print("APPROACH C: DAWG + skip-count + direct string refs")
    print("=" * 60)

    # Each final node stores a string table offset
    n_final = sum(1 for _ in range(1))  # need to count
    visited_c = set()
    n_final_c = 0
    def count_final(node):
        nonlocal n_final_c
        if node.id in visited_c:
            return
        visited_c.add(node.id)
        if node.final:
            n_final_c += 1
        for s, (child, _) in node.edges.items():
            count_final(child)
    count_final(root_std)

    # But wait — with DAWG suffix sharing, multiple entries share final nodes
    # A shared final node can only store ONE string offset
    # This breaks dedup... unless we use the skip-count to disambiguate
    # Skip-count already gives unique index → use that as index into value array
    # So we STILL need the value array

    # Alternative: don't share final nodes (partial DAWG — share internal only)
    # Then each final node = unique entry = unique string ref
    print(f"  Final nodes (shared): {n_final_c}")
    print(f"  Total entries: {n_entries}")
    print(f"  Final nodes can't store unique refs with suffix sharing")
    print(f"  → Must use skip-count index + value array (same as Approach B)")
    print()

    # ─── Approach D: Partial DAWG (share internal only) + direct refs ───
    print("=" * 60)
    print("APPROACH D: Partial DAWG (no suffix sharing at finals)")
    print("=" * 60)

    # Don't merge final nodes → each has unique string ref
    # Merge only internal nodes
    # Trade: more nodes but no value array needed

    # In the standard DAWG we had 22K nodes.
    # Without suffix sharing at finals, estimate:
    # 147K entries = 147K unique final nodes + shared internal nodes
    # Internal nodes from DAWG: ~22K - final_shared ≈ much more nodes
    # This blows up the structure. Not good.
    print(f"  Would need ~{n_entries} final nodes (no sharing)")
    print(f"  Structure would be larger than value array savings")
    print(f"  → Not viable")
    print()

    # ─── Approach E: DAWG + varint value array ───
    print("=" * 60)
    print("APPROACH E: DAWG + varint-compressed value array")
    print("=" * 60)

    # Instead of fixed-width dedup_bits per entry, use varint
    # Most translations are common (dedup IDs are small for frequent ones)
    # Sort unique translations by frequency → frequent = small ID → small varint

    trans_freq = Counter(translations)
    sorted_by_freq = sorted(set(translations), key=lambda t: -trans_freq[t])
    freq_to_id = {t: i for i, t in enumerate(sorted_by_freq)}

    # Varint encode: 7 bits per byte, high bit = continuation
    def varint_size(n):
        if n < 128: return 1
        if n < 16384: return 2
        if n < 2097152: return 3
        return 4

    varint_total = sum(varint_size(freq_to_id[t]) for t in translations)

    print(f"  Fixed-width value array: {dedup_bytes/1024:.1f} KB")
    print(f"  Varint value array: {varint_total/1024:.1f} KB")
    print(f"  Savings: {(dedup_bytes - varint_total)/1024:.1f} KB")

    total_e = structure_bytes + varint_total + str_compressed
    print(f"  TOTAL: {total_e/1024:.1f} KB")
    print()

    # ─── Approach F: Hybrid — DAWG keys + Huffman-coded values ───
    print("=" * 60)
    print("APPROACH F: DAWG + Huffman-coded value IDs")
    print("=" * 60)

    # Huffman code the dedup IDs based on frequency
    # Theoretical minimum: entropy
    total_entries = len(translations)
    entropy_bits = 0
    for t, count in trans_freq.items():
        p = count / total_entries
        entropy_bits -= count * math.log2(p)
    entropy_bytes = int(entropy_bits / 8)

    print(f"  Entropy of value mapping: {entropy_bytes/1024:.1f} KB")
    print(f"  (theoretical minimum for value array)")

    total_f = structure_bytes + entropy_bytes + str_compressed
    print(f"  DAWG structure: {structure_bytes/1024:.1f} KB")
    print(f"  Huffman values: {entropy_bytes/1024:.1f} KB")
    print(f"  String table: {str_compressed/1024:.1f} KB")
    print(f"  TOTAL: {total_f/1024:.1f} KB")
    print()

    # ─── Summary ───
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"  Target: 300 KB")
    print()

    results = [
        ("A: DAWG-FST (edge outputs)", total_a),
        ("B: DAWG + skip + dedup array", total_b),
        ("E: DAWG + varint values", total_e),
        ("F: DAWG + Huffman values", total_f),
    ]
    results.sort(key=lambda x: x[1])

    for name, size in results:
        kb = size / 1024
        marker = "  ✓ FITS!" if kb <= 300 else f"  ({kb-300:+.0f} KB over)"
        print(f"  {name:40s}: {kb:8.1f} KB{marker}")

    # Breakdown of best approach
    print()
    best_name, best_size = results[0]
    print(f"  Best: {best_name}")
    print(f"  Breakdown:")
    if "FST" in best_name:
        print(f"    DAWG-FST structure: {bin_est['structure_bytes']/1024:.1f} KB")
        print(f"    Output string table: {str_table['block_compressed']/1024:.1f} KB")
    elif "Huffman" in best_name:
        print(f"    DAWG structure: {structure_bytes/1024:.1f} KB")
        print(f"    Huffman value IDs: {entropy_bytes/1024:.1f} KB")
        print(f"    String table: {str_compressed/1024:.1f} KB")
    elif "varint" in best_name:
        print(f"    DAWG structure: {structure_bytes/1024:.1f} KB")
        print(f"    Varint value array: {varint_total/1024:.1f} KB")
        print(f"    String table: {str_compressed/1024:.1f} KB")


if __name__ == '__main__':
    main()

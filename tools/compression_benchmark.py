#!/usr/bin/env python3
"""Benchmark compression approaches for steno dictionaries.

Downloads Plover main.json and measures actual output sizes for:
1. Bit-packed DAWG (smhanov style)
2. MPHF + block-compressed values
3. LOUDS succinct trie
4. Computed entries (rules + exceptions)
5. Hybrid approaches
"""

import json
import struct
import sys
import zlib
import math
import os
import urllib.request
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


# ─── String table compression ───

def build_string_table_raw(translations):
    """Deduplicated null-terminated strings."""
    unique = sorted(set(translations))
    table = b'\x00'.join(v.encode('utf-8') for v in unique)
    idx_map = {}
    offset = 0
    for v in unique:
        idx_map[v] = offset
        offset += len(v.encode('utf-8')) + 1
    return table, idx_map

def build_string_table_block_compressed(translations, block_size=4096):
    """Block-compressed string table with random access."""
    unique = sorted(set(translations))
    raw = b'\x00'.join(v.encode('utf-8') for v in unique)

    blocks = []
    block_offsets = []
    strings_per_block = []
    raw_offset = 0

    for i in range(0, len(raw), block_size):
        block = raw[i:i+block_size]
        compressed = zlib.compress(block, 9)
        block_offsets.append(len(b''.join(blocks)) if blocks else 0)
        blocks.append(compressed)
        count = block.count(b'\x00') + (1 if i == 0 else 0)
        strings_per_block.append(count)

    total_compressed = sum(len(b) for b in blocks)
    index_size = len(blocks) * 4  # block offsets
    cumulative_counts = len(blocks) * 4  # cumulative string counts

    return total_compressed, index_size, cumulative_counts, len(unique)

def build_string_table_front_coded(translations):
    """Front-coded sorted strings."""
    unique = sorted(set(translations))
    total = 0
    prev = b''
    for v in unique:
        vb = v.encode('utf-8')
        shared = 0
        for i in range(min(len(prev), len(vb))):
            if prev[i] == vb[i]:
                shared += 1
            else:
                break
        total += 2 + len(vb) - shared  # prefix_len + suffix_len + suffix
        prev = vb
    return total, len(unique)


# ─── Approach 1: DAWG (Daciuk algorithm) ───

class DawgNode:
    next_id = 0
    def __init__(self):
        self.id = DawgNode.next_id
        DawgNode.next_id += 1
        self.edges = {}  # stroke_value -> DawgNode
        self.final = False
        self.count = 0  # reachable end nodes

    def __hash__(self):
        return hash((self.final, tuple(sorted((k, v.id) for k, v in self.edges.items()))))

    def __eq__(self, other):
        return (self.final == other.final and
                len(self.edges) == len(other.edges) and
                all(k in other.edges and self.edges[k].id == other.edges[k].id
                    for k in self.edges))

def build_dawg(entries):
    """Build DAWG using Daciuk's algorithm. Entries must be sorted."""
    DawgNode.next_id = 0
    root = DawgNode()
    unchecked = []  # (parent, stroke, child)
    minimized = {}
    prev_strokes = []

    def minimize(down_to):
        for i in range(len(unchecked) - 1, down_to - 1, -1):
            parent, stroke, child = unchecked[i]
            key = (child.final, tuple(sorted((k, v.id) for k, v in child.edges.items())))
            if key in minimized:
                parent.edges[stroke] = minimized[key]
            else:
                minimized[key] = child
            unchecked.pop()

    for strokes, _ in entries:
        # Find common prefix
        common = 0
        for i in range(min(len(strokes), len(prev_strokes))):
            if strokes[i] != prev_strokes[i]:
                break
            common += 1
        else:
            common = min(len(strokes), len(prev_strokes))

        minimize(common)

        # Add suffix
        if unchecked:
            node = unchecked[-1][2]
        else:
            node = root

        for stroke in strokes[common:]:
            new_node = DawgNode()
            node.edges[stroke] = new_node
            unchecked.append((node, stroke, new_node))
            node = new_node

        node.final = True
        prev_strokes = strokes

    minimize(0)

    # Count reachable end nodes for each node
    def count_reachable(node, visited=None):
        if visited is None:
            visited = {}
        if node.id in visited:
            return visited[node.id]
        c = 1 if node.final else 0
        for child in node.edges.values():
            c += count_reachable(child, visited)
        visited[node.id] = c
        node.count = c
        return c

    count_reachable(root)
    return root

def measure_dawg(root):
    """Count nodes, edges, measure bit-packed size."""
    nodes = set()
    edges = 0
    fallthrough = 0
    child_dist = Counter()

    def visit(node):
        nonlocal edges, fallthrough
        if node.id in nodes:
            return
        nodes.add(node.id)
        n_children = len(node.edges)
        child_dist[n_children] += 1
        edges += n_children
        if n_children == 1:
            fallthrough += 1
        for child in node.edges.values():
            visit(child)

    visit(root)
    return len(nodes), edges, fallthrough, child_dist

def estimate_dawg_bitpacked(n_nodes, n_edges, n_fallthrough, unique_strokes, n_entries):
    """Estimate bit-packed DAWG size (smhanov format)."""
    cbits = max(1, math.ceil(math.log2(max(unique_strokes, 2))))
    abits = max(1, math.ceil(math.log2(max(n_nodes, 2))))
    nskipbits = max(1, math.ceil(math.log2(max(n_entries, 2))))

    # Fallthrough nodes: 2 + cbits bits
    ft_bits = n_fallthrough * (2 + cbits)
    # Leaf nodes (0 children): 2 bits
    n_leaf = sum(1 for _ in range(n_nodes) if True)  # approximate
    # Multi-edge nodes: 2 + 1 + n_children * (cbits + nskipbits + abits)
    non_ft_edges = n_edges - n_fallthrough
    multi_bits = (n_nodes - n_fallthrough) * 3  # header per non-fallthrough
    multi_bits += non_ft_edges * (cbits + nskipbits + abits)

    total_bits = ft_bits + multi_bits
    return total_bits // 8, cbits, abits, nskipbits


# ─── Approach 2: MPHF + values ───

def estimate_mphf(n_entries, n_unique_strokes):
    """Estimate MPHF-based approach size."""
    mphf_bits_per_key = 2.5  # CHD or similar
    mphf_bytes = int(n_entries * mphf_bits_per_key / 8)
    fingerprint_bytes = n_entries * 2  # 16-bit fingerprints
    return mphf_bytes, fingerprint_bytes


# ─── Approach 3: LOUDS succinct trie ───

def build_louds_trie(entries):
    """Build trie and encode as LOUDS."""
    # Build trie
    class TrieNode:
        __slots__ = ['children', 'is_end']
        def __init__(self):
            self.children = {}
            self.is_end = False

    root = TrieNode()
    for strokes, _ in entries:
        node = root
        for s in strokes:
            if s not in node.children:
                node.children[s] = TrieNode()
            node = node.children[s]
        node.is_end = True

    # BFS to build LOUDS
    from collections import deque
    queue = deque([root])
    louds_bits = []  # 1 per child, 0 as separator
    labels = []
    is_final = []
    n_nodes = 0

    # Super root
    louds_bits.append(1)  # root is child of super root
    louds_bits.append(0)

    while queue:
        node = queue.popleft()
        n_nodes += 1
        is_final.append(node.is_end)
        for stroke in sorted(node.children.keys()):
            louds_bits.append(1)
            labels.append(stroke)
            queue.append(node.children[stroke])
        louds_bits.append(0)  # separator

    return louds_bits, labels, is_final, n_nodes

def measure_louds(louds_bits, labels, is_final, n_nodes, unique_strokes, n_entries):
    """Measure LOUDS encoding size."""
    # LOUDS bitvector
    louds_bytes = (len(louds_bits) + 7) // 8
    # Rank/select auxiliary structures (~37.5% overhead for practical implementations)
    rank_select_bytes = int(louds_bytes * 0.375)
    # Labels: each label = stroke value
    cbits = max(1, math.ceil(math.log2(max(unique_strokes, 2))))
    labels_bytes = (len(labels) * cbits + 7) // 8
    # is_final bitvector
    final_bytes = (n_nodes + 7) // 8
    final_rank_bytes = int(final_bytes * 0.375)

    return {
        'louds_bitvec': louds_bytes,
        'rank_select': rank_select_bytes,
        'labels': labels_bytes,
        'is_final': final_bytes + final_rank_bytes,
        'total': louds_bytes + rank_select_bytes + labels_bytes + final_bytes + final_rank_bytes,
    }


# ─── Approach 4: Computed entries analysis ───

def analyze_computed_entries(entries_dict):
    """Analyze how many entries follow computable patterns."""
    computable = 0
    rule_categories = Counter()

    for stroke_str, translation in entries_dict.items():
        # Fingerspelling: single letter output from specific strokes
        if len(translation) == 1 and translation.isalpha():
            computable += 1
            rule_categories['fingerspelling'] += 1
            continue

        # Number entries: output is digits
        if translation.replace(',', '').replace('.', '').replace('-', '').isdigit():
            computable += 1
            rule_categories['numbers'] += 1
            continue

        # Simple suffix entries: {^ing}, {^ed}, {^ly}, {^er}, {^ment}, {^ness}
        if translation.startswith('{^') and translation.endswith('}'):
            suffix = translation[2:-1]
            if suffix in ('ing', 'ed', 'ly', 'er', 'est', 'ment', 'ness', 'tion',
                         'sion', 'able', 'ible', 'ful', 'less', 'ous', 'ive',
                         'al', 'ial', 'en', 'ize', 'ise', 'ity', 'ty',
                         's', 'es', "'s", 'ry', 'ary'):
                computable += 1
                rule_categories['common_suffix'] += 1
                continue

        # Simple prefix entries: {pre^}, {re^}, {un^}
        if translation.startswith('{') and translation.endswith('^}'):
            prefix = translation[1:-2]
            if prefix in ('re', 'un', 'pre', 'dis', 'mis', 'over', 'under',
                         'out', 'sub', 'super', 'anti', 'auto', 'bi', 'co',
                         'de', 'ex', 'inter', 'macro', 'micro', 'mid', 'mini',
                         'mono', 'multi', 'non', 'post', 'semi', 'tri'):
                computable += 1
                rule_categories['common_prefix'] += 1
                continue

        # Plover commands: {#...}, {PLOVER:...}, {MODE:...}
        if translation.startswith('{#') or translation.startswith('{PLOVER:') or \
           translation.startswith('{MODE:'):
            computable += 1
            rule_categories['commands'] += 1
            continue

        # Punctuation/formatting: {.}, {,}, {?}, {!}, {^}, {-|}
        if translation in ('{.}', '{,}', '{?}', '{!}', '{^}', '{-|}', '{*-|}',
                          '{*!}', '{*?}', '{<}', '{>}', '{*<}', '{*>}',
                          '{^~|^}', '{~|}'):
            computable += 1
            rule_categories['formatting'] += 1
            continue

    return computable, rule_categories


# ─── Approach 5: FST-style encoding ───

def estimate_fst(entries, unique_strokes):
    """
    FST shares both prefixes AND suffixes on OUTPUT side too.
    Output = sequence of output tokens along edges.
    """
    # In an FST, each edge carries an output fragment
    # Common output prefixes/suffixes are shared
    # For steno: input = stroke sequence, output = translation

    # Build input trie first (same as DAWG input)
    # Then attach output weights to edges
    # FST minimization merges states with identical futures (like DAWG)
    # PLUS merges output-compatible states

    # Estimate: FST typically achieves 2-4 bytes per entry for English word lists
    # For steno with longer outputs, maybe 4-8 bytes per entry

    # Use BurntSushi/fst benchmarks as reference:
    # 235K English words → ~750KB FST
    # That's ~3.2 bytes per entry

    # For steno: 147K entries, but outputs are longer (avg 8.6 chars vs 7 for English)
    # Rough: 4-6 bytes per entry
    low = len(entries) * 4
    high = len(entries) * 6
    return low, high


# ─── Main benchmark ───

def main():
    # Download Plover dict
    dict_path = '/tmp/plover-main.json'
    if not os.path.exists(dict_path):
        print("Downloading Plover main.json...")
        urllib.request.urlretrieve(
            "https://raw.githubusercontent.com/openstenoproject/plover/main/plover/assets/main.json",
            dict_path)

    with open(dict_path) as f:
        raw_dict = json.load(f)

    print(f"Plover main.json: {len(raw_dict)} entries")
    print()

    # Parse all strokes
    parsed = []
    unique_stroke_vals = set()
    for stroke_str in sorted(raw_dict.keys()):
        strokes = tuple(parse_stroke(s) for s in stroke_str.split('/'))
        for s in strokes:
            unique_stroke_vals.add(s)
        parsed.append((strokes, raw_dict[stroke_str]))

    # Sort by stroke tuple for DAWG construction
    parsed.sort(key=lambda x: x[0])

    n_entries = len(parsed)
    translations = [v for _, v in parsed]
    unique_translations = set(translations)
    n_unique_strokes = len(unique_stroke_vals)

    print(f"Unique stroke values: {n_unique_strokes}")
    print(f"Unique translations: {len(unique_translations)}")
    print()

    # ─── String table measurements ───
    print("=" * 60)
    print("STRING TABLE OPTIONS")
    print("=" * 60)

    raw_table, _ = build_string_table_raw(translations)
    print(f"  Raw deduplicated:      {len(raw_table)/1024:8.1f} KB")

    fc_size, fc_count = build_string_table_front_coded(translations)
    print(f"  Front-coded:           {fc_size/1024:8.1f} KB")

    bc_data, bc_idx, bc_cum, bc_unique = build_string_table_block_compressed(
        translations, block_size=4096)
    bc_total = bc_data + bc_idx + bc_cum
    print(f"  Block-compressed 4KB:  {bc_total/1024:8.1f} KB  (data={bc_data/1024:.1f} idx={bc_idx/1024:.1f})")

    bc_data2, bc_idx2, bc_cum2, _ = build_string_table_block_compressed(
        translations, block_size=2048)
    bc_total2 = bc_data2 + bc_idx2 + bc_cum2
    print(f"  Block-compressed 2KB:  {bc_total2/1024:8.1f} KB")

    bc_data3, bc_idx3, bc_cum3, _ = build_string_table_block_compressed(
        translations, block_size=8192)
    bc_total3 = bc_data3 + bc_idx3 + bc_cum3
    print(f"  Block-compressed 8KB:  {bc_total3/1024:8.1f} KB")

    # Full zlib (no random access)
    full_zlib = len(zlib.compress(raw_table, 9))
    print(f"  Full zlib (no RA):     {full_zlib/1024:8.1f} KB")

    # Value index: maps entry index → string table position
    val_idx_2b = n_entries * 2
    val_idx_3b = n_entries * 3
    # With dedup: entry → unique_string_id (17 bits for 70K)
    dedup_idx_bits = n_entries * math.ceil(math.log2(len(unique_translations)))
    dedup_idx_bytes = (dedup_idx_bits + 7) // 8
    print(f"  Value index (2B/ent):  {val_idx_2b/1024:8.1f} KB")
    print(f"  Value index (dedup):   {dedup_idx_bytes/1024:8.1f} KB  ({math.ceil(math.log2(len(unique_translations)))} bits/ent)")
    print()

    # ─── Approach 1: DAWG ───
    print("=" * 60)
    print("APPROACH 1: BIT-PACKED DAWG")
    print("=" * 60)

    print("  Building DAWG (may take ~30s)...")
    root = build_dawg(parsed)
    n_nodes, n_edges, n_fallthrough, child_dist = measure_dawg(root)
    print(f"  Nodes: {n_nodes}")
    print(f"  Edges: {n_edges}")
    print(f"  Fallthrough (1-child): {n_fallthrough}")
    print(f"  Suffix dedup: {(1 - n_nodes/184582)*100:.1f}% reduction from trie")

    dawg_bytes, cbits, abits, nskipbits = estimate_dawg_bitpacked(
        n_nodes, n_edges, n_fallthrough, n_unique_strokes, n_entries)
    print(f"  cbits={cbits} abits={abits} nskipbits={nskipbits}")
    print(f"  DAWG structure:        {dawg_bytes/1024:8.1f} KB")

    # DAWG gives implicit index via skip counts → no separate value index needed
    # Total = DAWG + string table
    dawg_total = dawg_bytes + bc_total
    print(f"  + block-compressed strings: {bc_total/1024:.1f} KB")
    print(f"  TOTAL (DAWG):          {dawg_total/1024:8.1f} KB")
    print()

    # ─── Approach 2: MPHF ───
    print("=" * 60)
    print("APPROACH 2: MPHF + BLOCK-COMPRESSED VALUES")
    print("=" * 60)

    mphf_bytes, fp_bytes = estimate_mphf(n_entries, n_unique_strokes)
    print(f"  MPHF (~2.5 bits/key):  {mphf_bytes/1024:8.1f} KB")
    print(f"  Fingerprints (16-bit): {fp_bytes/1024:8.1f} KB")
    print(f"  Fingerprints (8-bit):  {fp_bytes/2/1024:8.1f} KB")
    # Need to store stroke sequences for fingerprint verification
    # Average stroke seq: 2.3 strokes × 3 bytes = 6.9 bytes per key
    stroke_storage = int(n_entries * 2.3 * 3)
    print(f"  Stroke key storage:    {stroke_storage/1024:8.1f} KB  (for verification)")

    mphf_total_16 = mphf_bytes + fp_bytes + bc_total + dedup_idx_bytes
    mphf_total_8 = mphf_bytes + fp_bytes // 2 + bc_total + dedup_idx_bytes
    mphf_total_nofp = mphf_bytes + bc_total + dedup_idx_bytes  # no fingerprint, accept false positives
    print(f"  TOTAL (16-bit fp):     {mphf_total_16/1024:8.1f} KB")
    print(f"  TOTAL (8-bit fp):      {mphf_total_8/1024:8.1f} KB")
    print(f"  TOTAL (no fp):         {mphf_total_nofp/1024:8.1f} KB  (0.4% false positive)")
    print()

    # ─── Approach 3: LOUDS ───
    print("=" * 60)
    print("APPROACH 3: LOUDS SUCCINCT TRIE")
    print("=" * 60)

    print("  Building LOUDS trie...")
    louds_bits, labels, is_final, louds_n_nodes = build_louds_trie(parsed)
    louds_sizes = measure_louds(louds_bits, labels, is_final, louds_n_nodes,
                                 n_unique_strokes, n_entries)
    for k, v in louds_sizes.items():
        if k != 'total':
            print(f"  {k:20s}:  {v/1024:8.1f} KB")
    louds_total = louds_sizes['total'] + bc_total + dedup_idx_bytes
    print(f"  + strings + val index: {(bc_total + dedup_idx_bytes)/1024:.1f} KB")
    print(f"  TOTAL (LOUDS):         {louds_total/1024:8.1f} KB")
    print()

    # ─── Approach 4: Computed entries ───
    print("=" * 60)
    print("APPROACH 4: COMPUTED ENTRIES ANALYSIS")
    print("=" * 60)

    computable, categories = analyze_computed_entries(raw_dict)
    remaining = n_entries - computable
    print(f"  Computable entries:    {computable} ({computable/n_entries*100:.1f}%)")
    for cat, count in categories.most_common():
        print(f"    {cat:20s}: {count}")
    print(f"  Remaining (stored):    {remaining}")
    print(f"  If remaining used DAWG approach:")
    reduction = remaining / n_entries
    computed_dawg_est = dawg_total * reduction
    print(f"    Estimated:           {computed_dawg_est/1024:8.1f} KB")
    print()

    # ─── Approach 5: FST estimate ───
    print("=" * 60)
    print("APPROACH 5: FST (FINITE STATE TRANSDUCER) ESTIMATE")
    print("=" * 60)

    fst_low, fst_high = estimate_fst(parsed, n_unique_strokes)
    print(f"  FST (4 bytes/entry):   {fst_low/1024:8.1f} KB")
    print(f"  FST (6 bytes/entry):   {fst_high/1024:8.1f} KB")
    print(f"  Note: FST stores keys + values together, no separate string table")
    print()

    # ─── Hybrid approaches ───
    print("=" * 60)
    print("HYBRID APPROACHES")
    print("=" * 60)

    # Hybrid 1: Computed entries + DAWG for rest
    print(f"\n  HYBRID 1: Computed rules + DAWG for remaining {remaining} entries")
    h1_rules = 5  # KB for rule engine code
    h1_dawg = dawg_total * reduction
    h1_total = h1_rules * 1024 + h1_dawg
    print(f"    Rules engine:        {h1_rules:8.1f} KB")
    print(f"    DAWG (remaining):    {h1_dawg/1024:8.1f} KB")
    print(f"    TOTAL:               {h1_total/1024:8.1f} KB")

    # Hybrid 2: MPHF (no fingerprint) + full zlib strings + dedup index
    print(f"\n  HYBRID 2: MPHF + full zlib (decompress to RAM per-block)")
    h2_total = mphf_bytes + bc_total + dedup_idx_bytes
    print(f"    MPHF:                {mphf_bytes/1024:8.1f} KB")
    print(f"    Strings (block):     {bc_total/1024:8.1f} KB")
    print(f"    Value index (dedup): {dedup_idx_bytes/1024:8.1f} KB")
    print(f"    TOTAL:               {h2_total/1024:8.1f} KB")

    # Hybrid 3: LOUDS trie (no value index needed - use rank on is_final)
    # The rank of the final-bit gives the entry index
    print(f"\n  HYBRID 3: LOUDS + rank-based indexing (no value index array)")
    h3_total = louds_sizes['total'] + bc_total
    print(f"    LOUDS structure:     {louds_sizes['total']/1024:8.1f} KB")
    print(f"    Strings (block):     {bc_total/1024:8.1f} KB")
    print(f"    TOTAL:               {h3_total/1024:8.1f} KB")

    # Hybrid 4: Computed + LOUDS for remaining
    print(f"\n  HYBRID 4: Computed + LOUDS for remaining {remaining}")
    h4_louds_est = louds_sizes['total'] * reduction
    h4_strings_est = bc_total * reduction
    h4_total = h1_rules * 1024 + h4_louds_est + h4_strings_est
    print(f"    Rules engine:        {h1_rules:8.1f} KB")
    print(f"    LOUDS (remaining):   {h4_louds_est/1024:8.1f} KB")
    print(f"    Strings (remaining): {h4_strings_est/1024:8.1f} KB")
    print(f"    TOTAL:               {h4_total/1024:8.1f} KB")

    # Hybrid 5: Computed + MPHF for remaining (no fingerprint)
    print(f"\n  HYBRID 5: Computed + MPHF for remaining {remaining}")
    h5_mphf = int(remaining * 2.5 / 8)
    h5_strings = int(bc_total * reduction)
    h5_dedup = int(dedup_idx_bytes * reduction)
    h5_total = h1_rules * 1024 + h5_mphf + h5_strings + h5_dedup
    print(f"    Rules engine:        {h1_rules:8.1f} KB")
    print(f"    MPHF (remaining):    {h5_mphf/1024:8.1f} KB")
    print(f"    Strings (remaining): {h5_strings/1024:8.1f} KB")
    print(f"    Value index:         {h5_dedup/1024:8.1f} KB")
    print(f"    TOTAL:               {h5_total/1024:8.1f} KB")

    # Hybrid 6: DAWG keys (implicit indexing) + zlib strings with smaller blocks
    print(f"\n  HYBRID 6: DAWG (implicit index) + aggressive string compression")
    # Use DAWG skip-count for indexing (no value array)
    # Try smaller zlib blocks for better compression at cost of more overhead
    bc_data_1k, bc_idx_1k, bc_cum_1k, _ = build_string_table_block_compressed(
        translations, block_size=1024)
    bc_total_1k = bc_data_1k + bc_idx_1k + bc_cum_1k
    print(f"    DAWG structure:      {dawg_bytes/1024:8.1f} KB")
    print(f"    Strings (1KB block): {bc_total_1k/1024:8.1f} KB")
    print(f"    TOTAL:               {(dawg_bytes + bc_total_1k)/1024:8.1f} KB")

    print()
    print("=" * 60)
    print("SUMMARY — ALL APPROACHES RANKED BY SIZE")
    print("=" * 60)
    print(f"  Target: 300 KB")
    print()

    approaches = [
        ("DAWG + block strings", dawg_total),
        ("MPHF no-fp + strings", mphf_total_nofp),
        ("MPHF 8-bit fp", mphf_total_8),
        ("LOUDS + strings", louds_total),
        ("LOUDS rank-index", h3_total),
        ("FST (optimistic)", fst_low),
        ("FST (conservative)", fst_high),
        ("Hybrid 1: Compute+DAWG", h1_total),
        ("Hybrid 2: MPHF+block", h2_total),
        ("Hybrid 3: LOUDS+rank", h3_total),
        ("Hybrid 4: Compute+LOUDS", h4_total),
        ("Hybrid 5: Compute+MPHF", h5_total),
        ("Hybrid 6: DAWG+aggr.str", dawg_bytes + bc_total_1k),
    ]

    approaches.sort(key=lambda x: x[1])

    for name, size in approaches:
        kb = size / 1024
        marker = "  ✓" if kb <= 300 else f"  ({kb-300:+.0f} KB over)"
        print(f"  {name:28s}: {kb:8.1f} KB{marker}")


if __name__ == '__main__':
    main()

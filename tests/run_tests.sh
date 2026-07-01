#!/bin/bash
# Run MPHF dictionary tests: compile test dict, build C test, execute.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"

echo "=== Step 1: Create test dictionary JSON ==="
cat > "$BUILD_DIR/test_dict.json" << 'DICTEOF'
{
  "S": "is",
  "T": "it",
  "THE": "the",
  "KAT": "cat",
  "TK": "did",
  "SKP": "and",
  "TPOR": "for",
  "STO": "so",
  "HAOEU": "hi",
  "TKOGS": "dogs",
  "S/T": "{.}",
  "PHAO*EUP/HRAOEUPB": "my line"
}
DICTEOF

echo "=== Step 2: Compile test dictionary ==="
python3 "$ROOT_DIR/tools/compile_mphf.py" \
    "$BUILD_DIR/test_dict.json" \
    "$BUILD_DIR/test_dict.bin" \
    --stats

echo ""
echo "=== Step 3: Build C test binary ==="
cc -O2 -Wall -Wextra -I"$ROOT_DIR/src" \
    -o "$BUILD_DIR/test_mphf" \
    "$SCRIPT_DIR/test_mphf.c" \
    "$ROOT_DIR/src/dict_mphf.c"

echo ""
echo "=== Step 4: Run tests ==="
"$BUILD_DIR/test_mphf" "$BUILD_DIR/test_dict.bin"

echo ""
echo "=== Step 5: Run with larger dict (if Plover available) ==="
PLOVER="/tmp/plover-main.json"
if [ -f "$PLOVER" ]; then
    echo "Compiling 1000-entry subset..."
    python3 "$ROOT_DIR/tools/compile_mphf.py" \
        "$PLOVER" \
        "$BUILD_DIR/plover_1k.bin" \
        --max-entries 1000 \
        --stats
    echo ""
    echo "Binary size: $(wc -c < "$BUILD_DIR/plover_1k.bin") bytes"
else
    echo "Plover dict not found at $PLOVER, skipping large dict test"
    echo "Download: curl -sL 'https://raw.githubusercontent.com/openstenoproject/plover/main/plover/assets/main.json' -o /tmp/plover-main.json"
fi

echo ""
echo "=== Done ==="

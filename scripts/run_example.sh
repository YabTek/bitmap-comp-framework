#!/usr/bin/env bash
# Example end-to-end run: build, generate a query file, benchmark every backend
# on the committed example dataset with verification on, print the CSVs.
#
# This is a smoke test of the pipeline, not an experiment: one small dataset of
# a single kind answers none of the project's questions. Use it to check that
# the framework works and to see what the output looks like, then write your own
# sweep for Task 2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"

[ -x "$BUILD/bitmap_bench" ] || "$ROOT/scripts/build.sh"

DATA="$ROOT/data/example"
OUT="${EXAMPLE_OUTPUT_DIR:-$ROOT/results/example}"
QUERIES="$OUT/queries.txt"
NUM_BITMAPS=$(sed -n 's/^num_bitmaps=//p' "$DATA/metadata.txt")

mkdir -p "$OUT"
"$BUILD/generate_queries" \
  --out="$QUERIES" --num-bitmaps="$NUM_BITMAPS" --num-queries=200 \
  --min-width=2 --max-width=4 --seed=43

status=0
for BACKEND in raw wah ewah roaring concise plwah val sbh bbc; do
  echo "==> $BACKEND"
  if ! "$BUILD/bitmap_bench" \
        --backend="$BACKEND" --dataset="$DATA" --queries="$QUERIES" \
        --out="$OUT/$BACKEND.csv" --repetitions=5 --warmup=1 --verify=1; then
    echo "    FAILED"
    status=1
  fi
done

echo
echo "CSVs in $OUT"
exit "$status"

#!/usr/bin/env bash
# Configure and build in Release mode into build/.
#
# Always benchmark a Release build: a Debug build is one to two orders of
# magnitude slower and does not slow the backends down by the same factor, so
# Debug measurements are meaningless for comparing representations.
#
# Env:
#   BUILD_DIR      build tree location (default: <root>/build)
#   BENCH_NATIVE=1 add -march=native (faster, but binaries are not portable
#                  between machines -- do not mix such results with others)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake not found on PATH" >&2
  exit 1
fi

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DBENCH_NATIVE=OFF -DBENCH_SANITIZERS=OFF -DBUILD_TESTING=ON)
if [ "${BENCH_NATIVE:-0}" != "0" ]; then
  CMAKE_ARGS+=(-DBENCH_NATIVE=ON)
fi

cmake -S "$ROOT" -B "$BUILD" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" --config Release --parallel
ctest --test-dir "$BUILD" -C Release --output-on-failure

echo
echo "built into $BUILD:"
for exe in bitmap_bench bitmap_inspect generate_queries generate_dataset; do
  if [ -x "$BUILD/$exe" ]; then echo "  $exe"; fi
done

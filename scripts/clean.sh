#!/usr/bin/env bash
# Default: remove only the default build tree. --outputs also removes queries/
# and results/. Dataset directories are never removed by this helper.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
  "") dirs=(build) ;;
  --outputs) dirs=(build queries results) ;;
  --help|-h) echo "Usage: scripts/clean.sh [--outputs]"; exit 0 ;;
  *) echo "Unknown option: $1" >&2; exit 1 ;;
esac
if [ "$#" -gt 1 ]; then echo "Too many arguments" >&2; exit 1; fi
for name in "${dirs[@]}"; do
  if [ -e "$ROOT/$name" ]; then
    rm -rf "${ROOT:?}/$name"
    echo "removed $name/"
  fi
done

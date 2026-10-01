#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root_dir"
mode="all"
case "${1:-}" in
  "") ;;
  --static) mode="static" ;;
  --firmware) mode="firmware" ;;
  *) echo "usage: $0 [--static|--firmware]" >&2; exit 2 ;;
esac

if [[ "$mode" != firmware ]]; then
  python3 -m py_compile tools/*.py
  python3 -m unittest discover -s tests -v
fi

if [[ "$mode" != static ]]; then
  command -v idf.py >/dev/null || {
    echo "idf.py not found; source ESP-IDF 5.5.3/export.sh first" >&2
    exit 1
  }
  python3 tools/pack_music.py
  idf.py build
fi

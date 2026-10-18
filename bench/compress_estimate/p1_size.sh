#!/usr/bin/env bash
# P1 size measurement: re-save a splayed store compressed and list per-column
# file sizes and generations next to the generation-0 originals.
#   usage: p1_size.sh [SRC] [OUT] [RAYFORCE]
#   SRC       generation-0 splayed store (default /skull/clickbench/rfsplayed)
#   OUT       directory for the compressed copy, removed first
#             (default <repo>/build/p1_size/hits)
#   RAYFORCE  release binary (default ./rayforce)
set -euo pipefail
SRC=${1:-/skull/clickbench/rfsplayed}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${2:-$HERE/../../build/p1_size/hits}
BIN=${3:-./rayforce}
rm -rf "$OUT"
"$BIN" "$HERE/p1_size.rfl" -- --src "$SRC" --out "$OUT" >&2
cur() { if [ -f "$1/.current" ]; then echo "$1/$(cat "$1/.current")"; else echo "$1"; fi; }
S=$(cur "$SRC"); D=$(cur "$OUT")
printf 'column\tgen0_bytes\tp1_bytes\tp1_gen\n'
for f in "$S"/*; do
  n=$(basename "$f"); [ -f "$D/$n" ] || continue
  g=$(od -An -tu1 -j17 -N1 "$D/$n" | tr -d ' ')
  printf '%s\t%s\t%s\t%s\n' "$n" "$(stat -c %s "$f")" "$(stat -c %s "$D/$n")" "$g"
done
printf 'sym\t%s\t%s\t-\n' "$(stat -c %s "$SRC/.sym")" "$(stat -c %s "$OUT/.sym")"

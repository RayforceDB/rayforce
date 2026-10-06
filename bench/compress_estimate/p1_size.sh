#!/usr/bin/env bash
# P1 size measurement: re-save the 10M store compressed and list per-column
# file sizes and generations next to the generation-0 originals.
set -euo pipefail
SRC=/skull/clickbench/rfsplayed
OUT=/home/hetoku/data/tmp/p1_size/hits
rm -rf /home/hetoku/data/tmp/p1_size
./rayforce bench/compress_estimate/p1_size.rfl >&2
cur() { if [ -f "$1/.current" ]; then echo "$1/$(cat "$1/.current")"; else echo "$1"; fi; }
S=$(cur "$SRC"); D=$(cur "$OUT")
printf 'column\tgen0_bytes\tp1_bytes\tp1_gen\n'
for f in "$S"/*; do
  n=$(basename "$f"); [ -f "$D/$n" ] || continue
  g=$(od -An -tu1 -j17 -N1 "$D/$n" | tr -d ' ')
  printf '%s\t%s\t%s\t%s\n' "$n" "$(stat -c %s "$f")" "$(stat -c %s "$D/$n")" "$g"
done
printf 'sym\t%s\t%s\t-\n' "$(stat -c %s "$SRC/.sym")" "$(stat -c %s "$OUT/.sym")"

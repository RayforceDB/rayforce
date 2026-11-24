#!/bin/sh
# usage: bench/affine_null/run.sh OUTDIR "CORES..." ROUNDS BINARY...
# Runs bench.rfl with each binary at each core count, ROUNDS times,
# rotating the binary order each round, then prints the median table
# (first binary is the baseline the percentages compare against).
set -e
out=$1; cores=$2; rounds=$3; shift 3
here=$(dirname "$0")
mkdir -p "$out"
r=1
while [ "$r" -le "$rounds" ]; do
  for c in $cores; do
    i=0
    for b in "$@"; do
      "$b" -c "$c" "$here/bench.rfl" > "$out/b$i.c$c.r$r.txt"
      i=$((i + 1))
    done
  done
  r=$((r + 1))
done
python3 "$here/summarize.py" "$out" "$@"

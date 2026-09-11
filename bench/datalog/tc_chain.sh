#!/usr/bin/env bash
# usage: tc_chain.sh [linear|nonlinear] [BIN]
set -u
MODE=${1:-linear}; BIN=${2:-./rayforce}
HERE=$(cd "$(dirname "$0")" && pwd)
for N in 64 128 256 512 1024; do
  f=$(mktemp /tmp/tc_XXXX.rfl); "$HERE/gen_chain.sh" $N $MODE > "$f"
  want=$(( N * (N + 1) / 2 )); best=999999
  for rep in 1 2 3; do
    s=$(date +%s%N); out=$(RAYFORCE_CORES=2 "$BIN" "$f" 2>&1 | tail -1); e=$(date +%s%N)
    ms=$(( (e - s) / 1000000 )); [ $ms -lt $best ] && best=$ms
  done
  ok=$([ "$out" = "$want" ] && echo ok || echo "WRONG($out)")
  printf "%s N=%5d rows=%8d min_ms=%7d %s\n" "$MODE" $N $want $best "$ok"
  rm -f "$f"
done

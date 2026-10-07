#!/usr/bin/env bash
# PI measurement (plan: compressed-columns PI, Task 1).
#   conv  : convert the 10M CSV twice (today's converter / upgrade disabled),
#           recording wall time, peak RSS, store bytes and per-column bytes
#   warm  : q41, q42 on both stores, warm, alternated (5 rounds x 10 runs)
#   gen2  : informational generation-2 store (no-hash converter, compress true)
#   warm3 : q41, q42 on hash, nohash, gen2, alternated
#   cold  : q41, q42 on both stores, one fresh process per run, page cache
#           evicted before each run, per file, with POSIX_FADV_DONTNEED
#           through dd iflag=nocache (no privileged operation)
# Output: tab-separated lines appended to $W/pi.tsv.
set -euo pipefail
W=${W:-/home/hetoku/data/tmp/pi_bench}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
CSV=/skull/clickbench/hits_h.csv
SCHEMA=/home/hetoku/data/work/ClickBench/rayforce/schema.rfl
QUERIES=/home/hetoku/data/work/ClickBench/rayforce/queries.rfl
BIN_H=$W/rayforce_hash
BIN_N=$W/rayforce_nohash
OUT=$W/pi.tsv
mkdir -p "$W"

query_n() {   # print the n-th (1-based) paren-balanced expression of queries.rfl
  awk -v want="$1" '
    /^[[:space:]]*$/ { next }
    { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
      for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
      if (d == 0) { n++; if (n == want) { printf "%s", q; exit } q = "" } }' "$QUERIES"
}

evict() {     # $1: store dir; per-file page-cache eviction, echoes the mode
  sync
  find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \;
  echo fadvise
}

case "${1:?usage: pi_measure.sh build|conv|warm|cold|gen2|warm3}" in
build)
  avail=$(df -B1 --output=avail "$W" | tail -1)
  [ "$avail" -ge $((13 * 1024 * 1024 * 1024)) ] || { echo "need 13 GB free in $W, have $avail" >&2; exit 2; }
  # both binaries from detached copies of the same commit (HEAD): BIN_H as
  # committed, BIN_N with the converter's hash upgrade check forced to 0.
  # The BIN_N patch targets commit 2dcb78ae / trees before PI Task 3 (which
  # removed ray_csv_hash_upgrade_check); on a later tree there is nothing to
  # disable, the patch is skipped and BIN_N equals BIN_H.
  SHA=$(git -C "$REPO" rev-parse --short HEAD)
  T=${PI_TREES:-$W}
  for t in h n; do
    rm -rf "$T/pi-$t"
    git -C "$REPO" worktree add --detach "$T/pi-$t" HEAD >/dev/null
  done
  python3 - "$T/pi-n/src/io/csv.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
sig = "int ray_csv_hash_upgrade_check(int8_t type, int64_t len,\n                               const void* index_payload) {\n"
if s.count(sig) == 0:
    print("pi_measure: no hash upgrade check in this tree (post PI Task 3): BIN_N not patched", file=sys.stderr)
    sys.exit(0)
assert s.count(sig) == 1, "signature found more than once"
s = s.replace(sig, sig + "    (void)type; (void)len; (void)index_payload;\n    return 0;   /* PI measurement only: upgrade disabled */\n")
open(p, "w").write(s)
PY
  make -C "$T/pi-h" release >/dev/null && cp "$T/pi-h/rayforce" "$BIN_H"
  make -C "$T/pi-n" release >/dev/null && cp "$T/pi-n/rayforce" "$BIN_N"
  for t in h n; do git -C "$REPO" worktree remove --force "$T/pi-$t"; done
  printf 'meta\tbuild\tsha\t%s\n' "$SHA" >> "$OUT"
  echo "built $BIN_H $BIN_N from $SHA"
  ;;
conv)
  for v in hash nohash; do
    bin=$BIN_H; [ "$v" = nohash ] && bin=$BIN_N
    dir=$W/conv_$v
    rm -rf "$dir"
    s=$(mktemp)
    cat "$SCHEMA" > "$s"
    printf '\n(do (.csv.splayed hit-types "%s" "%s") null)\n' "$CSV" "$dir" >> "$s"
    /usr/bin/time -f "%e %M" -o "$W/time_$v" "$bin" "$s" >/dev/null
    rm -f "$s"
    read -r secs rss_kb < "$W/time_$v"
    printf 'conv\t%s\twall_s\t%s\n' "$v" "$secs" >> "$OUT"
    printf 'conv\t%s\tpeak_rss_kb\t%s\n' "$v" "$rss_kb" >> "$OUT"
    printf 'conv\t%s\tstore_bytes\t%s\n' "$v" "$(du -sb "$dir" | cut -f1)" >> "$OUT"
    d=$dir; [ -f "$dir/.current" ] && d="$dir/$(cat "$dir/.current")"
    for c in WatchID ClientEventTime FUniqID RefererHash URLHash UserID; do
      printf 'conv\t%s\tbytes_%s\t%s\n' "$v" "$c" "$(stat -c %s "$d/$c")" >> "$OUT"
    done
  done
  ;;
warm)
  for qn in 41 42; do
    q=$(query_n "$qn")
    for round in 1 2 3 4 5; do
      for v in hash nohash; do
        s=$(mktemp)
        printf '(set hits (.db.splayed.get "%s"))\n(do %s null)\n' "$W/conv_$v" "$q" > "$s"
        for i in $(seq 1 10); do printf '(println (timeit %s))\n' "$q" >> "$s"; done
        "$BIN_H" "$s" | while read -r ms; do printf 'warm\t%s\tq%s\t%s\n' "$v" "$qn" "$ms"; done >> "$OUT"
        rm -f "$s"
      done
    done
  done
  ;;
cold)
  for qn in 41 42; do
    q=$(query_n "$qn")
    for trial in 1 2 3 4 5; do
      for v in hash nohash; do
        mode=$(evict "$W/conv_$v")
        s=$(mktemp)
        printf '(println (timeit (set hits (.db.splayed.get "%s"))))\n(println (timeit %s))\n' "$W/conv_$v" "$q" > "$s"
        mapfile -t r < <("$BIN_H" "$s")
        rm -f "$s"
        printf 'cold_load\t%s\tq%s\t%s\n' "$v" "$qn" "${r[0]}" >> "$OUT"
        printf 'cold\t%s\tq%s\t%s\n' "$v" "$qn" "${r[1]}" >> "$OUT"
        printf 'evict\t%s\tq%s\t%s\n' "$v" "$qn" "$mode" >> "$OUT"
      done
    done
  done
  ;;
gen2)
  # informational (ruling C1): generation-2 store from the no-hash converter
  avail=$(df -B1 --output=avail "$W" | tail -1)
  [ "$avail" -ge $((4 * 1024 * 1024 * 1024)) ] || { echo "need 4 GB free for conv_gen2, have $avail" >&2; exit 2; }
  dir=$W/conv_gen2
  rm -rf "$dir"
  s=$(mktemp)
  cat "$SCHEMA" > "$s"
  printf '\n(do (.csv.splayed {compress: true} hit-types "%s" "%s") null)\n' "$CSV" "$dir" >> "$s"
  /usr/bin/time -f "%e %M" -o "$W/time_gen2" "$BIN_N" "$s" >/dev/null
  rm -f "$s"
  read -r secs rss_kb < "$W/time_gen2"
  printf 'conv\tgen2\twall_s\t%s\n' "$secs" >> "$OUT"
  printf 'conv\tgen2\tpeak_rss_kb\t%s\n' "$rss_kb" >> "$OUT"
  printf 'conv\tgen2\tstore_bytes\t%s\n' "$(du -sb "$dir" | cut -f1)" >> "$OUT"
  d=$dir; [ -f "$dir/.current" ] && d="$dir/$(cat "$dir/.current")"
  for c in WatchID ClientEventTime FUniqID RefererHash URLHash UserID; do
    printf 'conv\tgen2\tbytes_%s\t%s\n' "$c" "$(stat -c %s "$d/$c")" >> "$OUT"
  done
  ;;
warm3)
  # informational: q41/q42 on hash, nohash and gen2, alternated (kind warm3)
  for qn in 41 42; do
    q=$(query_n "$qn")
    for round in 1 2 3 4 5; do
      for v in hash nohash gen2; do
        s=$(mktemp)
        printf '(set hits (.db.splayed.get "%s"))\n(do %s null)\n' "$W/conv_$v" "$q" > "$s"
        for i in $(seq 1 10); do printf '(println (timeit %s))\n' "$q" >> "$s"; done
        "$BIN_H" "$s" | while read -r ms; do printf 'warm3\t%s\tq%s\t%s\n' "$v" "$qn" "$ms"; done >> "$OUT"
        rm -f "$s"
      done
    done
  done
  ;;
esac

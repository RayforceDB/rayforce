#!/usr/bin/env bash
# P4 gate (plan: compressed-columns P4, Task 10): index queries on the 10M
# store, generation 0 against generation 2, both carrying explicit HASH
# indexes on UserID, RefererHash and URLHash.  Helpers are p2_bench.sh's.
#   prep  : HEAD's release build copied to $W/bin/p4; g0 / g2 copies of the
#           splayed store with the three hashes (.db.splayed.set {compress})
#   warm  : ROUNDS rounds, g0 / g2 alternated (order swapped each round); a
#           process loads the store and runs the query 6 times (timeit, ms):
#           run 1 materializes (generation 2) and builds nothing, runs 2..6
#           are the warm runs
#   first : first index use, a fresh process with the store's files evicted
#           from the page cache per file (dd iflag=nocache), 3 trials
#   ident : every query's full answer, g0 against g2 (g2 twice), warm process
#           and first-touch process (cache evicted), md5 raw and sorted
#   uses  : .idx.uses after each query, per variant (which index answered)
#   clean : delete the copies
#   kinds : the index kind of each copy's three columns (part of prep)
#   all   : prep warm first ident uses clean
# The load average is checked before every round and trial: below 1.5 or wait.
# Output: $W/p4.tsv ; the verdict is printed by p4_summary.py (never typed).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
W=${W:-$REPO/build/p4_bench}
QUERIES=
USAGE="usage: p4_bench.sh [-w WORKDIR] -q QUERIES <stage>\n  -w WORKDIR  scratch directory (default $REPO/build/p4_bench)\n  -q QUERIES  the ClickBench rayforce queries.rfl\n"
while getopts "w:q:" o; do
  case $o in
    w) W=$OPTARG ;;
    q) QUERIES=$OPTARG ;;
    *) printf "%b" "$USAGE" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ -n "$QUERIES" ] || { printf "%b" "$USAGE" >&2; exit 2; }
SRC=/skull/clickbench/rfsplayed
ROUNDS=${ROUNDS:-9}
OUT=$W/p4.tsv
BIN=$W/bin/p4
mkdir -p "$W/g" "$W/bin"

query_n() {     # the n-th paren-balanced form of queries.rfl
  awk -v want="$1" '
    /^[[:space:]]*$/ { next }
    { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
      for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
      if (d == 0) { n++; if (n == want) { printf "%s", q; exit } q = "" } }' "$QUERIES"
}
# q1: ClickBench q20 (the UserID point lookup, projecting: a count over a
# select is answered before the index arm).  q2 / q3: the RefererHash /
# URLHash constants of q41 / q42 as bare projecting point lookups.  q4 / q5:
# the whole q41 / q42 forms.
QD=$W/g/q; mkdir -p "$QD"
query_n 20 > "$QD/1.rfl"
grep -q '(== UserID 435090932899640449)' "$QD/1.rfl" || { echo "queries.rfl form 20 is not the UserID lookup" >&2; exit 2; }
printf '(select {RefererHash: RefererHash from: hits where: (== RefererHash 3594120000172545465)})\n' > "$QD/2.rfl"
printf '(select {URLHash: URLHash from: hits where: (== URLHash 2868770270353813622)})\n' > "$QD/3.rfl"
query_n 41 > "$QD/4.rfl"; query_n 42 > "$QD/5.rfl"
grep -q '3594120000172545465' "$QD/4.rfl" && grep -q '2868770270353813622' "$QD/5.rfl" \
  || { echo "queries.rfl forms 41 / 42 do not carry the hash constants" >&2; exit 2; }
NQ=5

guard() {   # $1: GB free needed in $W
  local avail; avail=$(df -B1 --output=avail "$W" | tail -1)
  [ "$avail" -ge $(($1 * 1024 * 1024 * 1024)) ] || { echo "disk guard: need $1 GB in $W, have $avail" >&2; exit 2; }
}
rel_guard() {   # a release build carries no sanitizer runtime
  [ -x "$1" ] || { echo "$1 missing: run prep" >&2; exit 2; }
  if grep -aq "__asan_init" "$1"; then echo "$1 is an ASan build" >&2; exit 2; fi
}
loadwait() {    # wait until the 1-minute load average is below 1.5; logs waits
  local t0; t0=$(date +%s)
  until awk '{exit !($1 < 1.5)}' /proc/loadavg; do sleep 20; done
  [ $(( $(date +%s) - t0 )) -lt 20 ] || printf 'loadwait\t%s\t%s\t%s\n' "$1" "$(date +%T)" "$(( $(date +%s) - t0 ))" >> "$OUT"
}
loadline() { printf '(set hits (.db.splayed.get "%s/"))\n' "$1"; }
evict() {       # dir: per-file page-cache eviction
  sync
  find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \;
}
answer() {      # dir q file: the full result of query q written to file; a result that is
                # not a table (an error: every query here is a select) leaves file.err
  local s c; s=$(mktemp -p "$W/g"); c=$(mktemp -p "$W/g" --suffix=.csv); rm -f "$c"
  { loadline "$1"; printf '(set r %s)\n' "$(cat "$QD/$2.rfl")"
    printf '(if (== (type r) (quote TABLE)) (.csv.write r "%s") (println r))\n' "$c"; } > "$s"
  "$BIN" "$s" > "$3.stdout" 2>&1 || true; rm -f "$s"
  rm -f "$3.err"
  if [ -f "$c" ]; then mv "$c" "$3"; else cp "$3.stdout" "$3"; : > "$3.err"; fi
  rm -f "$3.stdout"
}
# An error output hashes as ERR-<file>: never equal to the other side's.
h_raw()  { if [ -f "$1.err" ]; then echo "ERR-$(basename "$1")"; else md5sum < "$1" | cut -c1-12; fi; }
h_sort() { if [ -f "$1.err" ]; then echo "ERR-$(basename "$1")"; return; fi
           { head -n 1 "$1"; tail -n +2 "$1" | LC_ALL=C sort; } | md5sum | cut -c1-12; }
rows()   { echo $(( $(wc -l < "$1") - 1 )); }

prep() {
  guard 11
  ( cd "$REPO" && make release >/dev/null )
  cp "$REPO/rayforce" "$BIN"; rel_guard "$BIN"
  printf 'meta\tp4\t%s\t%s\n' "$(git -C "$REPO" rev-parse --short HEAD)" "$(sha256sum "$BIN" | cut -c1-12)" >> "$OUT"
  rm -rf "$W/g0" "$W/g2"
  local s; s=$(mktemp -p "$W/g")
  cat > "$s" <<EOF
(set t (.db.splayed.get "$SRC"))
(set t (update {UserID: (.idx.hash UserID) RefererHash: (.idx.hash RefererHash) URLHash: (.idx.hash URLHash) from: t}))
(println (map (fn [c] (at (.idx.info (at t c)) 'kind)) ['UserID 'RefererHash 'URLHash]))
(.db.splayed.set "$W/g0/" t {compress: false})
(.db.splayed.set "$W/g2/" t {compress: true})
EOF
  local o; o=$("$BIN" "$s" 2>&1); rm -f "$s"
  printf 'srckind\t%s\n' "$(printf '%s\n' "$o" | grep -m1 hash || true)" >> "$OUT"
  for v in g0 g2; do printf 'size\t%s\t%s\n' "$v" "$(du -sb "$W/$v" | cut -f1)" >> "$OUT"; done
  kinds
}

kinds() {       # the index kind each copy's columns carry once loaded (and materialized)
  local v s o
  for v in g0 g2; do
    s=$(mktemp -p "$W/g")
    { loadline "$W/$v"; printf "(println (map (fn [c] (at (.idx.info (at hits c)) 'kind)) ['UserID 'RefererHash 'URLHash]))\n"; } > "$s"
    o=$("$BIN" "$s" 2>&1) || true; rm -f "$s"
    printf 'kind\t%s\t%s\n' "$v" "$(printf '%s\n' "$o" | tail -1)" >> "$OUT"
  done
}

warm() {
  rel_guard "$BIN"
  local round q v order s r
  for round in $(seq 1 "$ROUNDS"); do
    loadwait "warm$round"
    if [ $((round % 2)) = 1 ]; then order="g0 g2"; else order="g2 g0"; fi
    for q in $(seq 1 $NQ); do
      for v in $order; do
        s=$(mktemp -p "$W/g")
        { loadline "$W/$v"; for i in 1 2 3 4 5 6; do printf '(println (timeit %s))\n' "$(cat "$QD/$q.rfl")"; done; } > "$s"
        r=$("$BIN" "$s" 2>/dev/null | tail -6 | tr '\n' '\t') || true; rm -f "$s"
        printf 'm\t%s\tq%s\t%s\t%s\n' "$v" "$q" "$round" "${r%$'\t'}" >> "$OUT"
      done
    done
    echo "warm round $round done $(date +%T)" >&2
  done
}

first() {
  rel_guard "$BIN"
  local trial q v order s r
  for trial in 1 2 3; do
    loadwait "first$trial"
    if [ $((trial % 2)) = 1 ]; then order="g0 g2"; else order="g2 g0"; fi
    for q in $(seq 1 $NQ); do
      for v in $order; do
        evict "$W/$v"
        s=$(mktemp -p "$W/g")
        { loadline "$W/$v"; printf '(println (timeit %s))\n' "$(cat "$QD/$q.rfl")"; } > "$s"
        r=$("$BIN" "$s" 2>/dev/null | tail -1) || true; rm -f "$s"
        printf 'f\t%s\tq%s\t%s\t%s\n' "$v" "$q" "$trial" "$r" >> "$OUT"
      done
    done
    echo "first trial $trial done $(date +%T)" >&2
  done
}

ident() {       # warm answers (g2 twice) and first-touch answers (cache evicted)
  rel_guard "$BIN"
  local q ad=$W/ans fa fb fb2 ord
  rm -rf "$ad"; mkdir -p "$ad"
  for q in $(seq 1 $NQ); do
    if grep -Eq '(asc|desc):' "$QD/$q.rfl"; then ord=ordered; else ord=unordered; fi
    fa=$ad/q$q.g0; fb=$ad/q$q.g2; fb2=$ad/q$q.g2b
    answer "$W/g0" "$q" "$fa"; answer "$W/g2" "$q" "$fb"; answer "$W/g2" "$q" "$fb2"
    printf 'ident\twarm\tq%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$q" \
      "$(h_raw "$fa")" "$(h_raw "$fb")" "$(h_raw "$fb2")" \
      "$(h_sort "$fa")" "$(h_sort "$fb")" "$(h_sort "$fb2")" "$ord" "$(rows "$fa")" >> "$OUT"
    fa=$ad/q$q.g0f; fb=$ad/q$q.g2f
    evict "$W/g0"; answer "$W/g0" "$q" "$fa"
    evict "$W/g2"; answer "$W/g2" "$q" "$fb"
    printf 'ident\tfirst\tq%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$q" \
      "$(h_raw "$fa")" "$(h_raw "$fb")" "$(h_raw "$fb")" \
      "$(h_sort "$fa")" "$(h_sort "$fb")" "$(h_sort "$fb")" "$ord" "$(rows "$fa")" >> "$OUT"
  done
  echo "ident done $(date +%T)" >&2
}

uses() {        # which index each query consulted, and what it left materialized
  rel_guard "$BIN"
  local q v s
  for q in $(seq 1 $NQ); do
    for v in g0 g2; do
      s=$(mktemp -p "$W/g")
      { loadline "$W/$v"; printf '(do (.idx.uses hits) null)\n(do %s null)\n(println (.idx.uses hits))\n' "$(cat "$QD/$q.rfl")"
        printf '(println (at (cols hits) (where (raze (map (fn [c] (at (.idx.info hits c) (quote materialized))) (cols hits))))))\n'; } > "$s"
      printf 'uses\t%s\tq%s\t%s\n' "$v" "$q" "$("$BIN" "$s" 2>&1 | tail -2 | tr '\n' '\t' | sed 's/\t$//')" >> "$OUT"
      rm -f "$s"
    done
  done
}

clean() { rm -rf "$W/g0" "$W/g2" "$W/ans"; }

[ $# -gt 0 ] || { echo "usage: p4_bench.sh prep|kinds|warm|first|ident|uses|clean|all ..." >&2; exit 2; }
for stage in "$@"; do     # stages run in the order given
  case "$stage" in
    prep)  prep ;;
    kinds) kinds ;;
    warm)  warm ;;
    first) first ;;
    ident) ident ;;
    uses)  uses ;;
    clean) clean ;;
    all)   rm -f "$OUT"; prep; warm; first; ident; uses; clean ;;
    *) echo "unknown stage $stage" >&2; exit 2 ;;
  esac
done

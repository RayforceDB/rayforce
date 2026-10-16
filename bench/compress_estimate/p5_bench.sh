#!/usr/bin/env bash
# P5 measurement (plan: compressed-columns P5, Task 8, pre-flight D9).
# The ClickBench store has no float column, so the float store is derived
# from it: five F64 columns and one F32 computed from real integer columns
# of the 10M .db.splayed.get store, saved g0 / g2, measured, deleted.
#   prep : release binary copied, store derived and saved (save times recorded);
#          the TSV is started afresh, so a stale pass never mixes with this one
#   enc  : per-column encoding mix and sizes of g2; per-vector ALP / ALP_RD
#          exception counts and DICT code children (p5_vecscan.py, O1)
#   warm : every query, ROUNDS alternated rounds (order swapped), one process
#          per round and variant: run 1 = first, runs 2..6 -> warm median
#   cold : 3 trials per variant, the store's files evicted first (fadvise)
#   ident: the full answer of every query, g0 vs g2 (g0 run twice, so float
#          sum drift (#744) is told apart from a g2 difference); raw and sorted
#   wall : per-call wall time of every query, g0 / g2, WALLS (5) alternated rounds
#   savet: the writer cost (O5): SAVES alternated rounds of the g0 and g2
#          saves of the derived table, with this binary and with BASE's
#          (`git archive`), each into a scratch store deleted after the round
#   clean: delete the stores
# usage: p5_bench.sh prep | enc | warm | cold | ident | wall | savet | clean | all
set -euo pipefail
W=${W:-/home/hetoku/data/tmp/p5_bench}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SRC=/skull/clickbench/rfsplayed
ROUNDS=${ROUNDS:-9}
SAVES=${SAVES:-3}
WALLS=${WALLS:-5}
BASE=${BASE:-81c04acb}
OUT=$W/p5.tsv
BIN=$W/bin/p5
BB=$W/bin/base
mkdir -p "$W/g" "$W/bin"
QS=(
  '(sum (at t (quote price)))'
  '(avg (at t (quote sendt)))'
  '(min (at t (quote ratio)))'
  '(max (at t (quote f32)))'
  '(count (select {from: t where: (> price 12.5)}))'
  '(count (select {from: t where: (== level 213.0)}))'
  '(count (select {from: t where: (< ratio 0.25)}))'
  '(count (select {from: t where: (and (>= sendt 0.5) (< sendt 1.5))}))'
  '(select {s: (sum price) c: (count price) from: t by: refresh})'
  '(select {p: price from: t desc: price take: 10})'
)
guard() { local a; a=$(df -B1 --output=avail "$W" | tail -1)
  [ "$a" -ge $(($1 * 1024 * 1024 * 1024)) ] || { echo "disk guard: need $1 GB in $W, have $a" >&2; exit 2; }; }
rel_guard() { [ -x "$1" ] || { echo "$1 missing: run prep" >&2; exit 2; }
  if grep -aq "__asan_init" "$1"; then echo "$1 is an ASan build" >&2; exit 2; fi; }
calm() { while ! awk '{exit !($1 < 1.5)}' /proc/loadavg; do printf 'wait\t%s\t%s\n' "$(date +%T)" "$(cut -d' ' -f1 /proc/loadavg)" >> "$OUT"; sleep 30; done; }
evict() { sync; find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \; ; }
loadline() { printf '(set t (.db.splayed.get "%s/%s/"))\n' "$W" "$1"; }
derive() {   # the derived float table, bound to t (pre-flight D9)
  cat <<EOF
(set h (.db.splayed.get "$SRC"))
(set t (select {price: (/ (as 'F64 ResolutionWidth) 100.0)
                ratio: (/ (as 'F64 ClientIP) 4294967296.0)
                sendt: (/ (as 'F64 SendTiming) 1000.0)
                level: (as 'F64 RegionID)
                ts:    (/ (as 'F64 EventTime) 1000000.0)
                f32:   (as 'F32 (/ (as 'F64 ResolutionWidth) 8.0))
                refresh: IsRefresh from: h}))
EOF
}
h_raw()  { md5sum < "$1" | cut -c1-12; }
h_sort() { { head -n 1 "$1"; tail -n +2 "$1" | LC_ALL=C sort; } | md5sum | cut -c1-12; }
answer() {   # variant q file
  local s c; s=$(mktemp -p "$W/g"); c=$W/g/ans.csv; rm -f "$c"
  # a scalar goes through .csv.write too (a one-cell table): println keeps
  # 6 significant digits, which would hide a float-sum difference
  { loadline "$1"; printf '(set r %s)\n(.csv.write (if (== (type r) (quote TABLE)) r (table [r] (list (enlist r)))) "%s")\n' "$2" "$c"; } > "$s"
  "$BIN" "$s" > "$3.err" 2>&1 || true; rm -f "$s"
  # no csv: a FAIL marker, never the error text (the same error on g0 and
  # g2 would hash the same and read as "same")
  if [ -f "$c" ]; then mv "$c" "$3"; rm -f "$3.err"; else { echo FAIL; cat "$3.err"; } > "$3"; rm -f "$3.err"; fi
}
h_or_fail() { if [ "$(head -n 1 "$2")" = FAIL ]; then echo FAIL; else "$1" "$2"; fi; }
case "${1:?usage: p5_bench.sh prep|enc|warm|cold|ident|wall|savet|clean|all}" in
prep)
  guard 4
  ( cd "$REPO" && make release >/dev/null ); cp "$REPO/rayforce" "$BIN"; rel_guard "$BIN"
  : > "$OUT"
  printf 'nq\t%s\n' "${#QS[@]}" >> "$OUT"
  printf 'meta\tp5\t%s\t%s\n' "$(git -C "$REPO" rev-parse --short HEAD)" "$(sha256sum "$BIN" | cut -c1-12)" >> "$OUT"
  rm -rf "$W/g0" "$W/g2"
  s=$(mktemp -p "$W/g")
  { derive; printf '(println (timeit (.db.splayed.set "%s/g0/" t {compress: false})))\n(println (timeit (.db.splayed.set "%s/g2/" t {compress: true})))\n' "$W" "$W"; } > "$s"
  "$BIN" "$s" | tail -2 | paste -sd '\t' | awk '{printf "save\tg0\t%s\nsave\tg2\t%s\n", $1, $2}' >> "$OUT"; rm -f "$s"
  for v in g0 g2; do printf 'size\t%s\t%s\n' "$v" "$(du -sb "$W/$v" | cut -f1)" >> "$OUT"; done
  ;;
enc)
  s=$(mktemp -p "$W/g")
  { loadline g2; printf '(map (fn [c] (println (format "col\\t%%\\t%%\\t%%\\t%%" c (at (.idx.info t c) (quote encoded_bytes)) (at (.idx.info t c) (quote raw_bytes)) (at (.idx.info t c) (quote enc))))) (cols t))\nnull\n'; } > "$s"
  "$BIN" "$s" | grep '^col' >> "$OUT"; rm -f "$s"
  python3 "$HERE/p5_vecscan.py" "$W/g2" >> "$OUT"
  ;;
warm)
  rel_guard "$BIN"
  for round in $(seq 1 "$ROUNDS"); do
    calm
    if [ $((round % 2)) = 1 ]; then order="g0 g2"; else order="g2 g0"; fi
    for qn in "${!QS[@]}"; do q=${QS[$qn]}
      for v in $order; do
        s=$(mktemp -p "$W/g")
        { loadline "$v"; for k in 1 2 3 4 5 6; do printf '(println (timeit %s))\n' "$q"; done; } > "$s"
        r=$("$BIN" "$s" 2>/dev/null | tail -6 | paste -sd '\t') || true; rm -f "$s"
        printf 'm\t%s\tq%s\t%s\t%s\n' "$v" "$qn" "$round" "$r" >> "$OUT"
      done
    done
    echo "warm round $round done" >&2
  done
  ;;
cold)
  rel_guard "$BIN"
  for qn in "${!QS[@]}"; do q=${QS[$qn]}
    for trial in 1 2 3; do for v in g0 g2; do
      calm; evict "$W/$v"
      s=$(mktemp -p "$W/g")
      { loadline "$v"; printf '(println (timeit %s))\n' "$q"; } > "$s"
      r=$("$BIN" "$s" 2>/dev/null | tail -1) || true; rm -f "$s"
      printf 'c\t%s\tq%s\t%s\t%s\n' "$v" "$qn" "$trial" "$r" >> "$OUT"
    done; done
  done
  ;;
ident)
  for qn in "${!QS[@]}"; do q=${QS[$qn]}
    a=$W/g/out_g0; b=$W/g/out_g2; a2=$W/g/out_g0b
    answer g0 "$q" "$a"; answer g2 "$q" "$b"; answer g0 "$q" "$a2"
    printf 'ident\tq%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$qn" "$(h_or_fail h_raw "$a")" "$(h_or_fail h_raw "$b")" "$(h_or_fail h_raw "$a2")" \
      "$(h_or_fail h_sort "$a")" "$(h_or_fail h_sort "$b")" "$(h_or_fail h_sort "$a2")" >> "$OUT"
    printf 'answer\tq%s\tg0\t%s\n' "$qn" "$(head -c 300 "$a" | tr '\n\t' '  ')" >> "$OUT"
    printf 'answer\tq%s\tg2\t%s\n' "$qn" "$(head -c 300 "$b" | tr '\n\t' '  ')" >> "$OUT"
  done
  ;;
savet)
  rel_guard "$BIN"; guard 4
  if [ ! -x "$BB" ]; then
    rm -rf "$W/base_src"; mkdir -p "$W/base_src"
    ( cd "$REPO" && git archive "$BASE" ) | tar -x -C "$W/base_src"
    ( cd "$W/base_src" && make release >/dev/null )
    cp "$W/base_src/rayforce" "$BB"; rm -rf "$W/base_src"
  fi
  rel_guard "$BB"
  printf 'meta\tbase\t%s\t%s\n' "$BASE" "$(sha256sum "$BB" | cut -c1-12)" >> "$OUT"
  for round in $(seq 1 "$SAVES"); do
    if [ $((round % 2)) = 1 ]; then order="p5 base"; else order="base p5"; fi
    for b in $order; do
      if [ "$b" = p5 ]; then x=$BIN; else x=$BB; fi
      calm; rm -rf "$W/s0" "$W/s2"
      s=$(mktemp -p "$W/g")
      { derive; printf '(println (timeit (.db.splayed.set "%s/s0/" t {compress: false})))\n(println (timeit (.db.splayed.set "%s/s2/" t {compress: true})))\n' "$W" "$W"; } > "$s"
      r=$("$x" "$s" | tail -2 | paste -sd '\t'); rm -f "$s"
      printf 'savet\t%s\t%s\t%s\t%s\t%s\n' "$b" "$round" "$r" "$(du -sb "$W/s0" | cut -f1)" "$(du -sb "$W/s2" | cut -f1)" >> "$OUT"
      rm -rf "$W/s0" "$W/s2"
    done
  done
  ;;
wall)   # per-call wall time, (T(1 + K) - T(1)) / K over K top-level calls: (timeit q) on a
        # plain generation-0 vector reports the reduction as ~0 ms (q0, q1), so the warm
        # ratio of those queries is not a scan-to-scan ratio; this one is
  rel_guard "$BIN"; K=100
  for round in $(seq 1 "$WALLS"); do
    calm
    if [ $((round % 2)) = 1 ]; then order="g0 g2"; else order="g2 g0"; fi
    for qn in "${!QS[@]}"; do q=${QS[$qn]}
      for v in $order; do
        t=()
        for n in 1 $((K + 1)); do
          s=$(mktemp -p "$W/g")
          { loadline "$v"; for k in $(seq 1 $n); do printf '(set r %s)\n' "$q"; done; } > "$s"
          a=$(date +%s%N); "$BIN" "$s" >/dev/null 2>&1 || true; b=$(date +%s%N); rm -f "$s"
          t+=($(( (b - a) / 1000 )))
        done
        printf 'wall\t%s\tq%s\t%s\t%s\n' "$v" "$qn" "$round" "$(awk -v a="${t[0]}" -v b="${t[1]}" -v k=$K 'BEGIN{printf "%.3f", (b - a) / k / 1000.0}')" >> "$OUT"
      done
    done
  done
  ;;
clean) rm -rf "$W/g0" "$W/g2" "$W/s0" "$W/s2" "$W/g" ;;
all) "$0" prep; "$0" enc; "$0" ident; "$0" warm; "$0" cold; "$0" wall; "$0" savet; "$0" clean ;;
esac

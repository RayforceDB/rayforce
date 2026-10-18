#!/usr/bin/env bash
# P2 gate (plan: compressed-columns P2, Task 17), 10M ClickBench, all 43 queries.
# Stages run one load kind at a time (disk: ~13-16 GB free): create, measure, delete.
#   prep    : HEAD's release build copied to $W/bin/p2; the pre-P2 base (BASE)
#             built from `git archive` and copied to $W/bin/base
#   splayed : g0 / g2 copies of the splayed store (.db.splayed.set {compress})
#             arm A  g0 vs g2 (p2 binary); arm C0 g0, base vs p2 binary
#   parted  : p0 / p2 conversions of the CSV (.csv.parted, 1M rows per partition)
#             arm B  p0 vs p2 (p2 binary); arm C1 p0, base vs p2 binary;
#             arm R  p0 made ragged (the last partition without its column
#                    tail from AdvEngineID on), base vs p2 binary
# A measurement process loads the store and runs the query 6 times (timeit, ms):
#   first = run 1 (fresh process, warm page cache), 3-run = runs 1..3,
#   warm = median of runs 2..6.  ROUNDS rounds, the two variants alternated
#   (order swapped each round).  Cold (arms A, B): the store's files evicted
#   from the page cache per file (dd iflag=nocache: POSIX_FADV_DONTNEED, no
#   privileged operation), then run 1 in a fresh process; the .sym is evicted
#   with the store, so SYM queries' cold times include the domain faults.
# Answers: every query's full result is written (a table by .csv.write, any
#   other value by println) and compared byte for byte, raw and with the lines
#   sorted (header kept first).  A query without an explicit order (no asc: /
#   desc:) is compared sorted; a raw-only difference is reported as an order
#   difference.  Variant b runs twice to separate nondeterminism from a diff.
# Output: $W/p2.tsv ; the verdict is printed by p2_summary.py (never typed).
# usage: p2_bench.sh [-w WORKDIR] -s SCHEMA -q QUERIES prep | splayed | parted | all | verdict
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
W=${W:-$REPO/build/p2_bench}
SCHEMA=
QUERIES=
USAGE="usage: p2_bench.sh [-w WORKDIR] -s SCHEMA -q QUERIES <stage>\n  -w WORKDIR  scratch directory (default $REPO/build/p2_bench)\n  -s SCHEMA   the ClickBench rayforce schema.rfl\n  -q QUERIES  the ClickBench rayforce queries.rfl\n"
while getopts "w:s:q:" o; do
  case $o in
    w) W=$OPTARG ;;
    s) SCHEMA=$OPTARG ;;
    q) QUERIES=$OPTARG ;;
    *) printf "%b" "$USAGE" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ -n "$SCHEMA" ] || { printf "%b" "$USAGE" >&2; exit 2; }
[ -n "$QUERIES" ] || { printf "%b" "$USAGE" >&2; exit 2; }
SRC=/skull/clickbench/rfsplayed
CSV=/skull/clickbench/hits_h.csv
BASE=${BASE:-80a28e49}
ROUNDS=${ROUNDS:-5}
NQ=43
OUT=$W/p2.tsv
P2=$W/bin/p2
BB=$W/bin/base
mkdir -p "$W/g" "$W/bin"

query_n() {
  awk -v want="$1" '
    /^[[:space:]]*$/ { next }
    { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
      for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
      if (d == 0) { n++; if (n == want) { printf "%s", q; exit } q = "" } }' "$QUERIES"
}
QD=$W/g/q; mkdir -p "$QD"
for n in $(seq 1 $NQ); do query_n "$n" > "$QD/$n.rfl"; done
[ "$(cat "$QD/$NQ.rfl" | wc -c)" -gt 0 ] || { echo "queries.rfl has fewer than $NQ forms" >&2; exit 2; }

guard() {   # $1: GB free needed in $W
  local avail; avail=$(df -B1 --output=avail "$W" | tail -1)
  [ "$avail" -ge $(($1 * 1024 * 1024 * 1024)) ] || { echo "disk guard: need $1 GB in $W, have $avail" >&2; exit 2; }
}
rel_guard() {   # a release build carries no sanitizer runtime
  [ -x "$1" ] || { echo "$1 missing: run prep" >&2; exit 2; }
  if grep -aq "__asan_init" "$1"; then echo "$1 is an ASan build" >&2; exit 2; fi
}
loadline() {    # kind dir
  if [ "$1" = parted ]; then printf '(set hits (.db.parted.get "%s" (quote hits)))\n' "$2"
  else printf '(set hits (.db.splayed.get "%s/"))\n' "$2"; fi
}
evict() {       # dir: per-file page-cache eviction, echoes the mode
  sync
  find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \;
  echo fadvise
}
size() { printf 'size\t%s\t%s\t%s\n' "$1" "$2" "$(du -sb "$3" | cut -f1)" >> "$OUT"; }

measure() {     # arm kind dirA binA labA dirB binB labB
  local arm=$1 kind=$2 round q v d b lab s r order
  for round in $(seq 1 "$ROUNDS"); do
    for q in $(seq 1 $NQ); do
      if [ $((round % 2)) = 1 ]; then order="a b"; else order="b a"; fi
      for v in $order; do
        if [ "$v" = a ]; then d=$3; b=$4; lab=$5; else d=$6; b=$7; lab=$8; fi
        s=$(mktemp -p "$W/g")
        { loadline "$kind" "$d"; for i in 1 2 3 4 5 6; do printf '(println (timeit %s))\n' "$(cat "$QD/$q.rfl")"; done; } > "$s"
        r=$("$b" "$s" 2>/dev/null | tail -6 | tr '\n' '\t') || true; rm -f "$s"
        printf 'm\t%s\t%s\tq%s\t%s\t%s\n' "$arm" "$lab" "$q" "$round" "${r%$'\t'}" >> "$OUT"
      done
    done
    echo "measure $arm round $round done" >&2
  done
}

cold() {        # arm kind dirA labA dirB labB (P2 binary)
  local arm=$1 kind=$2 trial q v d lab s r mode
  for q in $(seq 1 $NQ); do
    for trial in 1 2 3; do
      for v in a b; do
        if [ "$v" = a ]; then d=$3; lab=$4; else d=$5; lab=$6; fi
        mode=$(evict "$d")
        s=$(mktemp -p "$W/g")
        { loadline "$kind" "$d"; printf '(println (timeit %s))\n' "$(cat "$QD/$q.rfl")"; } > "$s"
        r=$("$P2" "$s" 2>/dev/null | tail -1) || true; rm -f "$s"
        printf 'c\t%s\t%s\tq%s\t%s\t%s\t%s\n' "$arm" "$lab" "$q" "$trial" "$r" "$mode" >> "$OUT"
      done
    done
  done
  echo "cold $arm done" >&2
}

mat() {         # arm kind dir (generation 2, P2 binary): the columns each query leaves materialized
  local arm=$1 q s m
  for q in $(seq 1 $NQ); do
    s=$(mktemp -p "$W/g")
    { loadline "$2" "$3"; printf '(do %s null)\n' "$(cat "$QD/$q.rfl")"
      printf '(println (at (cols hits) (where (raze (map (fn [c] (at (.idx.info hits c) (quote materialized))) (cols hits))))))\n'; } > "$s"
    m=$("$P2" "$s" 2>/dev/null | tail -1) || true; rm -f "$s"
    printf 'mat\t%s\tq%s\t%s\n' "$arm" "$q" "$m" >> "$OUT"
  done
}

enc() {         # arm kind dir: every column's encoding mix (records which keys are DELTA)
  local arm=$1 s
  s=$(mktemp -p "$W/g")
  { loadline "$2" "$3"; printf '(map (fn [c] (println (list c (at (.idx.info hits c) (quote enc))))) (cols hits))\nnull\n'; } > "$s"
  "$P2" "$s" 2>/dev/null | while IFS= read -r line; do printf 'enc\t%s\t%s\n' "$arm" "$line" >> "$OUT"; done || true
  rm -f "$s"
}

mem() {         # arm kind dirA labA dirB labB [skip]: peak RSS of one process running all 43 queries;
                # with skip (query numbers), also of one running all but those (row memx)
  local arm=$1 v d lab s q
  for v in a b; do
    if [ "$v" = a ]; then d=$3; lab=$4; else d=$5; lab=$6; fi
    s=$(mktemp -p "$W/g")
    { loadline "$2" "$d"; for q in $(seq 1 $NQ); do printf '(do %s null)\n' "$(cat "$QD/$q.rfl")"; done; } > "$s"
    /usr/bin/time -f "%M" -o "$W/g/mem" "$P2" "$s" >/dev/null 2>&1 || true; rm -f "$s"
    printf 'mem\t%s\t%s\t%s\n' "$arm" "$lab" "$(cat "$W/g/mem")" >> "$OUT"
    [ -n "${7:-}" ] || continue
    s=$(mktemp -p "$W/g")
    { loadline "$2" "$d"
      for q in $(seq 1 $NQ); do
        case " $7 " in *" $q "*) continue ;; esac
        printf '(do %s null)\n' "$(cat "$QD/$q.rfl")"
      done; } > "$s"
    /usr/bin/time -f "%M" -o "$W/g/mem" "$P2" "$s" >/dev/null 2>&1 || true; rm -f "$s"
    printf 'memx\t%s\t%s\t%s\t%s\n' "$arm" "$lab" "$(cat "$W/g/mem")" "$(printf 'q%s,' $7 | sed 's/,$//')" >> "$OUT"
  done
}

answer() {      # kind dir bin q file: the full result of query q written to file
  local s c; s=$(mktemp -p "$W/g"); c=$(mktemp -p "$W/g" --suffix=.csv); rm -f "$c"
  { loadline "$1" "$2"; printf '(set r %s)\n' "$(cat "$QD/$4.rfl")"
    printf '(if (== (type r) (quote TABLE)) (.csv.write r "%s") (println r))\n' "$c"; } > "$s"
  "$3" "$s" > "$5.stdout" 2>&1 || true; rm -f "$s"
  if [ -f "$c" ]; then mv "$c" "$5"; else cp "$5.stdout" "$5"; fi
  rm -f "$5.stdout"
}
h_raw()  { md5sum < "$1" | cut -c1-12; }
h_sort() { { head -n 1 "$1"; tail -n +2 "$1" | LC_ALL=C sort; } | md5sum | cut -c1-12; }

ident() {       # arm kind dirA binA dirB binB: full answers of every query, a vs b, b twice
  local arm=$1 q ord ad fa fb fb2
  ad=$W/ans/$arm; rm -rf "$ad"; mkdir -p "$ad"
  for q in $(seq 1 $NQ); do
    if grep -Eq '(asc|desc):' "$QD/$q.rfl"; then ord=ordered; else ord=unordered; fi
    fa=$ad/q$q.a; fb=$ad/q$q.b; fb2=$ad/q$q.b2
    answer "$2" "$3" "$4" "$q" "$fa"
    answer "$2" "$5" "$6" "$q" "$fb"
    answer "$2" "$5" "$6" "$q" "$fb2"   # separates nondeterminism from a real diff
    printf 'ident\t%s\tq%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$q" \
      "$(h_raw "$fa")" "$(h_raw "$fb")" "$(h_raw "$fb2")" \
      "$(h_sort "$fa")" "$(h_sort "$fb")" "$(h_sort "$fb2")" "$ord" >> "$OUT"
  done
  echo "ident $arm done" >&2
}

prep() {
  ( cd "$REPO" && make release >/dev/null )
  cp "$REPO/rayforce" "$P2"; rel_guard "$P2"
  rm -rf "$W/base_src"; mkdir -p "$W/base_src"
  ( cd "$REPO" && git archive "$BASE" ) | tar -x -C "$W/base_src"
  ( cd "$W/base_src" && make release >/dev/null )
  cp "$W/base_src/rayforce" "$BB"; rm -rf "$W/base_src"; rel_guard "$BB"
  printf 'meta\tp2\t%s\t%s\n' "$(git -C "$REPO" rev-parse --short HEAD)" "$(sha256sum "$P2" | cut -c1-12)" >> "$OUT"
  printf 'meta\tbase\t%s\t%s\n' "$BASE" "$(sha256sum "$BB" | cut -c1-12)" >> "$OUT"
}

stage_splayed() {
  rel_guard "$P2"; rel_guard "$BB"; guard 11
  rm -rf "$W/g0" "$W/g2"
  local s; s=$(mktemp -p "$W/g")
  printf '(set t (.db.splayed.get "%s"))\n(.db.splayed.set "%s/g0/" t {compress: false})\n(.db.splayed.set "%s/g2/" t {compress: true})\n' "$SRC" "$W" "$W" > "$s"
  "$P2" "$s" >/dev/null; rm -f "$s"
  size A g0 "$W/g0"; size A g2 "$W/g2"
  enc A splayed "$W/g2"
  mat A splayed "$W/g2"
  measure A splayed "$W/g0" "$P2" g0 "$W/g2" "$P2" g2
  cold A splayed "$W/g0" g0 "$W/g2" g2
  mem A splayed "$W/g0" g0 "$W/g2" g2 "41 42"   # memx: without p3_summary's D2 entries (index queries)
  ident A splayed "$W/g0" "$P2" "$W/g2" "$P2"
  measure C0 splayed "$W/g0" "$BB" base "$W/g0" "$P2" p2
  ident C0 splayed "$W/g0" "$BB" "$W/g0" "$P2"
  rm -rf "$W/g0" "$W/g2"
}

convert_parted() {   # dir compress(true|false)
  local s; s=$(mktemp -p "$W/g")
  cat "$SCHEMA" > "$s"
  printf '\n(do (.csv.parted {compress: %s} hit-types "%s" 1000000 "%s/" (quote hits)) null)\n' "$2" "$CSV" "$1" >> "$s"
  "$P2" "$s" >/dev/null; rm -f "$s"
}

make_ragged() {      # dir: the last partition rewritten without its column tail from AdvEngineID on (generation 0)
  # The parted loader associates columns by index and accepts a partition
  # with fewer columns only as a missing tail (src/store/part.c), so the last
  # partition keeps the columns before AdvEngineID (index 40 of 105; it lacks
  # AdvEngineID, DontCountHits, IsLink, IsDownload, WindowClientWidth,
  # RefererHash, URLHash and the rest; ResolutionWidth, index 20, stays).
  local last s; last=$(find "$1" -mindepth 1 -maxdepth 1 -type d ! -name '.*' | sort -n | tail -1)
  s=$(mktemp -p "$W/g")
  printf '(set t (.db.splayed.get "%s/hits/" "%s/.sym"))\n(set cs (take (cols t) (first (where (== (cols t) %sAdvEngineID)))))\n(.db.splayed.set "%s/hits_r/" (table cs (map (fn [c] (at t c)) cs)) "%s/.sym" {compress: false})\n' \
    "$last" "$1" "'" "$last" "$1" > "$s"
  "$BB" "$s" >/dev/null; rm -f "$s"          # the base binary: plain column values, no lazy `at`
  rm -rf "$last/hits"; mv "$last/hits_r" "$last/hits"
  printf 'ragged\t%s\t%s\n' "$(basename "$last")" "$(ls "$last/hits" | grep -vc '^\.')" >> "$OUT"
}

stage_parted() {
  rel_guard "$P2"; rel_guard "$BB"; guard 11
  rm -rf "$W/p0" "$W/p2"
  convert_parted "$W/p0" false
  convert_parted "$W/p2" true
  size B p0 "$W/p0"; size B p2 "$W/p2"
  enc B parted "$W/p2"
  mat B parted "$W/p2"
  measure B parted "$W/p0" "$P2" p0 "$W/p2" "$P2" p2
  cold B parted "$W/p0" p0 "$W/p2" p2
  mem B parted "$W/p0" p0 "$W/p2" p2
  ident B parted "$W/p0" "$P2" "$W/p2" "$P2"
  measure C1 parted "$W/p0" "$BB" base "$W/p0" "$P2" p2
  ident C1 parted "$W/p0" "$BB" "$W/p0" "$P2"
  rm -rf "$W/p2"
  make_ragged "$W/p0"
  measure R parted "$W/p0" "$BB" base "$W/p0" "$P2" p2
  ident R parted "$W/p0" "$BB" "$W/p0" "$P2"
  rm -rf "$W/p0"
}

case "${1:?usage: p2_bench.sh prep|splayed|parted|all|verdict}" in
  prep)    prep ;;
  splayed) stage_splayed ;;
  parted)  stage_parted ;;
  all)     rm -f "$OUT"; prep; stage_splayed; stage_parted ;;
  verdict) python3 "$HERE/p2_summary.py" "$OUT" ;;
esac

#!/usr/bin/env bash
# PI gate (plan: compressed-columns PI, Task 5): re-measure after the index
# policy change, over ALL 43 ClickBench queries, per load kind.
#   BIN_H : head binary (release, built from a committed tree)
#   BIN_D : baseline binary (dev), run on a store ITS converter wrote
#           (with the persisted HASH indexes)
# Load kinds: gen0   splayed generation 0     (head store vs dev store)
#             gen2   splayed {compress:true}  (head store vs the dev gen-0 store:
#                                              dev has no generation 2)
#             parted .csv.parted store        (head store vs dev store)
# Per (kind, variant, query, round) one FRESH process loads the store through
# .db.splayed.get / .db.parted.get and runs the query 6 times (timeit, ms):
#   first run  = run 1 (fresh process, warm page cache)
#   3-run total= runs 1..3 (what the ClickBench harness records)
#   warm       = median of runs 2..6 in the process; best of 5 rounds,
#                head and dev alternated (order swapped each round)
# Stores are converted, measured and deleted one at a time (disk guard).
# Output: $W/gate.tsv ; the verdict is printed by pi_gate.py (never typed).
# usage: BIN_H=... BIN_D=... pi_gate.sh [-w WORKDIR] -s SCHEMA -q QUERIES run|ident|all|verdict
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
W=${W:-$REPO/build/pi_bench}
SCHEMA=
QUERIES=
USAGE="usage: pi_gate.sh [-w WORKDIR] -s SCHEMA -q QUERIES <stage>\n  -w WORKDIR  scratch directory (default $REPO/build/pi_bench)\n  -s SCHEMA   the ClickBench rayforce schema.rfl\n  -q QUERIES  the ClickBench rayforce queries.rfl\n"
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
CSV=/skull/clickbench/hits_h.csv
BIN_H=${BIN_H:?BIN_H}
BIN_D=${BIN_D:?BIN_D}
OUT=$W/gate.tsv
ROUNDS=${ROUNDS:-5}
NQ=43
QS=${QS:-$(seq 1 43)}       # confirmation runs: QS="16 20 39" ROUNDS=15 OUT_FILE=file.tsv
OUT=${OUT_FILE:-$OUT}
mkdir -p "$W/g"

query_n() {
  awk -v want="$1" '
    /^[[:space:]]*$/ { next }
    { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
      for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
      if (d == 0) { n++; if (n == want) { printf "%s", q; exit } q = "" } }' "$QUERIES"
}
# cache the 43 query texts once
QD=$W/g/q; mkdir -p "$QD"
for n in $(seq 1 $NQ); do query_n "$n" > "$QD/$n.rfl"; done

guard() {  # F8: >= 7 GB free before each conversion
  local avail; avail=$(df -B1 --output=avail "$W" | tail -1)
  [ "$avail" -ge $((7 * 1024 * 1024 * 1024)) ] || { echo "disk guard: $avail free" >&2; exit 2; }
}

convert() {  # bin kind dir
  local bin=$1 kind=$2 dir=$3 s; guard
  rm -rf "$dir"; s=$(mktemp -p "$W/g")
  cat "$SCHEMA" > "$s"
  case $kind in
    gen0)   printf '\n(do (.csv.splayed hit-types "%s" "%s") null)\n' "$CSV" "$dir" >> "$s" ;;
    gen2)   printf '\n(do (.csv.splayed {compress: true} hit-types "%s" "%s") null)\n' "$CSV" "$dir" >> "$s" ;;
    parted) printf '\n(do (.csv.parted hit-types "%s" 1000000 "%s/" (quote hits)) null)\n' "$CSV" "$dir" >> "$s" ;;
  esac
  /usr/bin/time -f "%e %M" -o "$s.t" "$bin" "$s" >/dev/null
  rm -f "$s"
  echo "conv $kind $(basename "$bin") $(cat "$s.t") bytes=$(du -sb "$dir" | cut -f1)" >&2
  printf 'conv\t%s\t%s\t%s\t%s\n' "$kind" "$(basename "$dir")" "$(cut -d' ' -f1 "$s.t")" "$(du -sb "$dir" | cut -f1)" >> "$OUT"
  rm -f "$s.t"
}

loadline() {  # kind dir
  if [ "$1" = parted ]; then printf '(set hits (.db.parted.get "%s" (quote hits)))\n' "$2"
  else printf '(set hits (.db.splayed.get "%s"))\n' "$2"; fi
}

clause1() {  # bin dir label : count of HASH-indexed columns of a splayed store
  local s; s=$(mktemp -p "$W/g")
  printf '(set t (.db.splayed.get "%s"))\n(println (sum (map (fn [c] (if (.idx.has? (at t c)) (if (== (at (.idx.info (at t c)) (quote kind)) (quote hash)) 1 0) 0)) (cols t))))\n' "$2" > "$s"
  local n; n=$("$1" "$s" | tail -1); rm -f "$s"
  printf 'clause1\t%s\t%s\t%s\n' "$3" "$(basename "$1")" "$n" >> "$OUT"
  echo "clause1 $3: $n HASH columns" >&2
}

measure() {  # kind dirH dirD
  local kind=$1 dh=$2 dd=$3 round q v bin d s order
  for round in $(seq 1 "$ROUNDS"); do
    for q in $QS; do
      if [ $((round % 2)) = 1 ]; then order="h d"; else order="d h"; fi
      for v in $order; do
        if [ $v = h ]; then bin=$BIN_H; d=$dh; else bin=$BIN_D; d=$dd; fi
        s=$(mktemp -p "$W/g")
        { loadline "$kind" "$d"; for i in 1 2 3 4 5 6; do printf '(println (timeit %s))\n' "$(cat "$QD/$q.rfl")"; done; } > "$s"
        r=$("$bin" "$s" 2>&1 | tail -6 | tr '\n' '\t'); rm -f "$s"
        printf 'm\t%s\t%s\tq%s\t%s\t%s\n' "$kind" "$v" "$q" "$round" "${r%$'\t'}" >> "$OUT"
      done
    done
    echo "measure $kind round $round done" >&2
  done
}

ident() {  # kind dirH dirD : result text of every query, head vs baseline
  local kind=$1 q s
  for q in $QS; do
    for v in h d; do
      if [ $v = h ]; then bin=$BIN_H; d=$2; else bin=$BIN_D; d=$3; fi
      s=$(mktemp -p "$W/g")
      { loadline "$kind" "$d"; printf '(println %s)\n' "$(cat "$QD/$q.rfl")"; } > "$s"
      "$bin" "$s" 2>&1 | md5sum | cut -c1-12 > "$W/g/id_$v"; rm -f "$s"
    done
    # repeat head once against itself to separate tie-order noise from a real diff
    s=$(mktemp -p "$W/g")
    { loadline "$kind" "$2"; printf '(println %s)\n' "$(cat "$QD/$q.rfl")"; } > "$s"
    "$BIN_H" "$s" 2>&1 | md5sum | cut -c1-12 > "$W/g/id_h2"; rm -f "$s"
    printf 'ident\t%s\tq%s\t%s\t%s\t%s\n' "$kind" "$q" "$(cat "$W/g/id_h")" "$(cat "$W/g/id_d")" "$(cat "$W/g/id_h2")" >> "$OUT"
  done
}

stage() {  # kind: convert both, measure, delete head store (+ dev store unless kept)
  local kind=$1
  case $kind in
    gen0)
      convert "$BIN_D" gen0 "$W/g/dev_gen0"; clause1 "$BIN_D" "$W/g/dev_gen0" dev_gen0
      convert "$BIN_H" gen0 "$W/g/head_gen0"; clause1 "$BIN_H" "$W/g/head_gen0" head_gen0
      measure gen0 "$W/g/head_gen0" "$W/g/dev_gen0"; ident gen0 "$W/g/head_gen0" "$W/g/dev_gen0"
      rm -rf "$W/g/head_gen0" ;;
    gen2)   # dev gen-0 store is still on disk from the gen0 stage
      convert "$BIN_H" gen2 "$W/g/head_gen2"; clause1 "$BIN_H" "$W/g/head_gen2" head_gen2
      measure gen2 "$W/g/head_gen2" "$W/g/dev_gen0"; ident gen2 "$W/g/head_gen2" "$W/g/dev_gen0"
      rm -rf "$W/g/head_gen2" "$W/g/dev_gen0" ;;
    parted)
      convert "$BIN_D" parted "$W/g/dev_parted"
      convert "$BIN_H" parted "$W/g/head_parted"
      measure parted "$W/g/head_parted" "$W/g/dev_parted"; ident parted "$W/g/head_parted" "$W/g/dev_parted"
      rm -rf "$W/g/head_parted" "$W/g/dev_parted" ;;
  esac
}

case "${1:?usage: pi_gate.sh run KIND|all|verdict}" in
  run) stage "${2:?kind}" ;;
  all) rm -f "$OUT"; printf 'meta\thead\t%s\n' "$(sha256sum "$BIN_H" | cut -c1-12)" >> "$OUT"
       printf 'meta\tdev\t%s\n' "$(sha256sum "$BIN_D" | cut -c1-12)" >> "$OUT"
       stage gen0; stage gen2; stage parted ;;
  verdict) python3 "$HERE/pi_gate.py" "$OUT" ;;
esac

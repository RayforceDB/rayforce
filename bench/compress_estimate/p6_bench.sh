#!/usr/bin/env bash
# P6 gate (plan: compressed-columns P6, Task 8, pre-flight D14, rulings R2 / R3),
# rerun as the P6b gate (P6b Task 8, D18): the persisted STR DICT.
# The ClickBench store has no STR column, so the STR store is derived from it:
# the 25 columns the 43 ClickBench queries read, the five text ones (URL,
# Title, Referer, SearchPhrase, MobilePhoneModel) cast SYM -> STR, from the
# 10M .db.splayed.get store; saved g0 / g2, measured, deleted.  Copied from
# p5_bench.sh (calm, guard, rel_guard, alternated rounds, per-file eviction,
# ident g0 twice, the wall stage for #856, completeness and FAIL rows).
#   prep : release binary copied; the store's size and its DICT estimated,
#          guarded (estimate + 4 GB), g0 saved and its size printed, guarded
#          again (DICT estimate + 3 GB), g2 saved (save times recorded), at
#          least 0.5 GB left after it; the DICT region bytes per STR column
#          from the file header; p6_dictcheck built from `make lib` and run
#          (dict / dictgate rows: the persisted DICT); the TSV started afresh
#   enc  : per-column encoding mix and sizes of g2
#   mat  : the columns each query leaves materialized on g2 (Rulings 6-2 / 7-1)
#   warm : every query, ROUNDS alternated rounds (order swapped), one process
#          per round and variant: run 1 = first, runs 2..6 -> warm median
#   cold : 3 trials per variant, the store's files evicted first (fadvise)
#   ident: the full answer of every query, g0 vs g2 (g0 run twice); raw and sorted
#   wall : per-call wall time, (T(1 + K) - T(1)) / K, WALLS alternated rounds;
#          K per query from its warm g0 median (about 2 s of calls, 3..100)
#   o5   : arm C base vs p6 binary on g0, all 43 queries (I8 on the STR
#          store, D18); arm D the STR group-by queries (P3 carry), base binary
#          on its own generation-2 save (bg2, RAW / CONST STR) vs the p6 binary
#          on g2.  bg2 is derived and saved by the base binary after g0 is
#          deleted (one extra store at a time), at least 0.3 GB left after it
#   i8   : base vs p6 binary on the SYM 10M store read in place from $SRC
#          (no copy, no disk), all 43 queries (I8, D18)
#   clean: delete the stores
#   savet: the writer cost (O2): SAVES alternated rounds of the g0 and g2
#          saves, this binary and BASE's, each into a scratch store deleted
#          after the round (run after clean: needs the room of g0 + g2)
#   parted: R2, the parted STR arm: p0 / p2 built from the derived table, ten
#          1M-row partitions saved one by one, each store under the guard;
#          a refusal is recorded as a `skip` row; warm / ident / mat on it
# usage: p6_bench.sh [-w WORKDIR] -q QUERIES prep | enc | mat | warm | cold | ident | wall | o5 | i8 | clean | savet | parted | all
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
W=${W:-$REPO/build/p6_bench}
QUERIES=
USAGE="usage: p6_bench.sh [-w WORKDIR] -q QUERIES <stage>\n  -w WORKDIR  scratch directory (default $REPO/build/p6_bench)\n  -q QUERIES  the ClickBench rayforce queries.rfl\n"
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
SAVES=${SAVES:-3}
WALLS=${WALLS:-5}
PROUNDS=${PROUNDS:-5}
BASE=${BASE:-9802ba80}
NQ=43
OUT=$W/p6.tsv
BIN=$W/bin/p6
BB=$W/bin/base
DC=$W/bin/p6_dictcheck
mkdir -p "$W/g" "$W/bin"
TEXT="URL Title Referer SearchPhrase MobilePhoneModel"
COLS="WatchID EventTime EventDate CounterID ClientIP RegionID UserID URL Title Referer IsRefresh ResolutionWidth
      MobilePhone MobilePhoneModel SearchEngineID SearchPhrase AdvEngineID IsLink IsDownload DontCountHits
      TraficSourceID URLHash RefererHash WindowClientWidth WindowClientHeight"
# the STR group-by queries (a STR key or a STR count-distinct): O5
O5Q="6 11 12 13 14 15 17 18 19 22 23 29 34 35 37 38 39 40"
query_n() {
  awk -v want="$1" '
    /^[[:space:]]*$/ { next }
    { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
      for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
      if (d == 0) { n++; if (n == want) { printf "%s", q; exit } q = "" } }' "$QUERIES"
}
QD=$W/g/q; mkdir -p "$QD"
for n in $(seq 1 $NQ); do query_n "$n" > "$QD/$n.rfl"; done
[ "$(wc -c < "$QD/$NQ.rfl")" -gt 0 ] || { echo "queries.rfl has fewer than $NQ forms" >&2; exit 2; }
Q() { cat "$QD/$1.rfl"; }
avail() { df -B1 --output=avail "$W" | tail -1; }
guard() { local a; a=$(avail)   # $1: bytes needed in $W
  printf 'disk\t%s\tneed\t%s\tavail\t%s\n' "${2:-guard}" "$1" "$a" >> "$OUT"
  [ "$a" -ge "$1" ] || { echo "disk guard (${2:-guard}): need $1 B in $W, have $a" >&2; return 2; }; }
GB=$((1024 * 1024 * 1024))
rel_guard() { [ -x "$1" ] || { echo "$1 missing: run prep" >&2; exit 2; }
  if grep -aq "__asan_init" "$1"; then echo "$1 is an ASan build" >&2; exit 2; fi; }
calm() { while ! awk '{exit !($1 < 1.5)}' /proc/loadavg; do printf 'wait\t%s\t%s\n' "$(date +%T)" "$(cut -d' ' -f1 /proc/loadavg)" >> "$OUT"; sleep 30; done; }
evict() { sync; find "$1" -type f -exec dd if={} iflag=nocache count=0 status=none \; ; }
peak() { local u; u=$(du -sb "$W" | cut -f1); printf 'peak\t%s\t%s\t%s\n' "$1" "$u" "$(avail)" >> "$OUT"; }
loadline() { if [ "${2:-}" = parted ]; then printf '(set hits (.db.parted.get "%s/%s" (quote hits)))\n' "$W" "$1"
             elif [ "${2:-}" = sym ]; then printf '(set hits (.db.splayed.get "%s/"))\n' "$1"
             else printf '(set hits (.db.splayed.get "%s/%s/"))\n' "$W" "$1"; fi; }
sel() {      # the select of the derived columns from table $1
  local c e=""
  for c in $COLS; do
    case " $TEXT " in *" $c "*) e="$e $c: (as 'STR $c)" ;; *) e="$e $c: $c" ;; esac
  done
  printf '(select {%s from: %s})' "$e" "$1"
}
derive() {   # the derived STR table, bound to t (pre-flight D14, R3)
  printf '(set h (.db.splayed.get "%s"))\n(set t %s)\n' "$SRC" "$(sel h)"
}
estimate() {   # bytes: the numeric source files + per STR column 16 B descriptors,
               # its pool (at most its string bytes) and the gen-0 DICT (8 B / row)
  local s c num=0 str
  for c in $COLS; do case " $TEXT " in *" $c "*) ;; *) num=$((num + $(stat -c %s "$SRC/$c"))) ;; esac; done
  s=$(mktemp -p "$W/g")
  { derive; printf '(println (sum (map (fn [c] (+ (* 24 (count t)) (sum (strlen (at t c))))) [%s])))\n' "$(printf "'%s " $TEXT)"; } > "$s"
  str=$("$BIN" "$s" | tail -1); rm -f "$s"
  echo $((num + str))
}
estimate_dict() {   # bytes: per text column 4 B / row + 12 B / distinct + the distinct strings' bytes
  local s; s=$(mktemp -p "$W/g")
  { printf '(set h (.db.splayed.get "%s"))\n' "$SRC"
    printf '(println (sum (map (fn [c] (+ (+ (+ (* 4 (count h)) (* 12 (count (distinct (at h c))))) (sum (strlen (distinct (at h c))))) 1024)) [%s])))\n' "$(printf "'%s " $TEXT)"; } > "$s"
  "$BIN" "$s" | tail -1; rm -f "$s"
}
h_raw()  { md5sum < "$1" | cut -c1-12; }
h_sort() { { head -n 1 "$1"; tail -n +2 "$1" | LC_ALL=C sort; } | md5sum | cut -c1-12; }
answer() {   # store q file [kind]: a table by .csv.write, a scalar as a one-cell
             # table (println keeps 6 digits), anything else (a list) by println
  local s c; s=$(mktemp -p "$W/g"); c=$W/g/ans.csv; rm -f "$c"
  { loadline "$1" "${4:-}"; printf '(set r %s)\n(.csv.write (if (== (type r) (quote TABLE)) r (table [r] (list (enlist r)))) "%s")\n' "$(Q "$2")" "$c"; } > "$s"
  "$BIN" "$s" > "$3.err" 2>&1 || true
  if [ ! -f "$c" ]; then
    { loadline "$1" "${4:-}"; printf '(set r %s)\n(println r)\n' "$(Q "$2")"; } > "$s"
    if "$BIN" "$s" > "$3.out" 2>>"$3.err"; then mv "$3.out" "$c"; else rm -f "$3.out"; fi
  fi
  rm -f "$s"
  # no answer: a FAIL marker with the error text after it (the hash is FAIL)
  if [ -f "$c" ]; then mv "$c" "$3"; rm -f "$3.err"; else { echo FAIL; head -c 300 "$3.err"; } > "$3"; rm -f "$3.err"; fi
}
h_or_fail() { if [ "$(head -n 1 "$2")" = FAIL ]; then echo FAIL; else "$1" "$2"; fi; }
base_bin() {
  [ -x "$BB" ] && return 0
  rm -rf "$W/base_src"; mkdir -p "$W/base_src"
  ( cd "$REPO" && git archive "$BASE" ) | tar -x -C "$W/base_src"
  ( cd "$W/base_src" && make release >/dev/null )
  cp "$W/base_src/rayforce" "$BB"; rm -rf "$W/base_src"
  rel_guard "$BB"
  printf 'meta\tbase\t%s\t%s\n' "$BASE" "$(sha256sum "$BB" | cut -c1-12)" >> "$OUT"
}
# warm rows: m <label> q<n> <round> <6 times> (or FAIL); arm-tagged for o5 / parted
warm_run() {   # tag binA storeA labA binB storeB labB rounds queries [kind]
  local tag=$1 rounds=$8 qs=$9 kind=${10:-} round qn v x d lab order s o r
  for round in $(seq 1 "$rounds"); do
    calm
    if [ $((round % 2)) = 1 ]; then order="a b"; else order="b a"; fi
    for qn in $qs; do
      for v in $order; do
        if [ "$v" = a ]; then x=$2; d=$3; lab=$4; else x=$5; d=$6; lab=$7; fi
        s=$(mktemp -p "$W/g")
        { loadline "$d" "$kind"; for k in 1 2 3 4 5 6; do printf '(println (timeit %s))\n' "$(Q "$qn")"; done; } > "$s"
        # an erroring run (non-zero exit: a script stops at its first error)
        # is a FAIL row, which the summary counts as a failure
        if o=$("$x" "$s" 2>/dev/null); then r=$(printf '%s\n' "$o" | tail -6 | paste -sd '\t'); else r=FAIL; fi; rm -f "$s"
        printf '%s\t%s\tq%s\t%s\t%s\n' "$tag" "$lab" "$qn" "$round" "$r" >> "$OUT"
      done
    done
    echo "$tag round $round done" >&2
  done
}
mat_run() {   # tag store [kind]
  local qn s m
  for qn in $(seq 1 $NQ); do
    s=$(mktemp -p "$W/g")
    { loadline "$2" "${3:-}"; printf '(do %s null)\n' "$(Q "$qn")"
      printf '(println (at (cols hits) (where (raze (map (fn [c] (at (.idx.info hits c) (quote materialized))) (cols hits))))))\n'; } > "$s"
    m=$("$BIN" "$s" 2>/dev/null | tail -1) || m=FAIL; rm -f "$s"
    printf '%s\tq%s\t%s\n' "$1" "$qn" "$m" >> "$OUT"
  done
}
ident_run() {   # tag storeA storeB [kind]: a vs b, a twice
  local qn a b a2
  for qn in $(seq 1 $NQ); do
    a=$W/g/out_a; b=$W/g/out_b; a2=$W/g/out_a2
    answer "$2" "$qn" "$a" "${4:-}"; answer "$3" "$qn" "$b" "${4:-}"; answer "$2" "$qn" "$a2" "${4:-}"
    printf '%s\tq%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$qn" "$(h_or_fail h_raw "$a")" "$(h_or_fail h_raw "$b")" "$(h_or_fail h_raw "$a2")" \
      "$(h_or_fail h_sort "$a")" "$(h_or_fail h_sort "$b")" "$(h_or_fail h_sort "$a2")" >> "$OUT"
    printf 'answer\t%s\tq%s\ta\t%s\n' "$1" "$qn" "$(head -c 300 "$a" | tr '\n\t' '  ')" >> "$OUT"
    printf 'answer\t%s\tq%s\tb\t%s\n' "$1" "$qn" "$(head -c 300 "$b" | tr '\n\t' '  ')" >> "$OUT"
  done
}
save_one() {   # bin dir compress: derive in a fresh process, save, print the ms
  local s r; s=$(mktemp -p "$W/g")
  { derive; printf '(println (timeit (.db.splayed.set "%s/" t {compress: %s})))\n' "$2" "$3"; } > "$s"
  r=$("$1" "$s" | tail -1); rm -f "$s"; echo "$r"
}
case "${1:?usage: p6_bench.sh prep|enc|mat|warm|cold|ident|wall|o5|i8|clean|savet|parted|all}" in
prep)
  ( cd "$REPO" && make release >/dev/null ); cp "$REPO/rayforce" "$BIN"; rel_guard "$BIN"
  : > "$OUT"
  printf 'nq\t%s\n' "$NQ" >> "$OUT"
  printf 'meta\tp6\t%s\t%s\n' "$(git -C "$REPO" rev-parse --short HEAD)" "$(sha256sum "$BIN" | cut -c1-12)" >> "$OUT"
  rm -rf "$W/g0" "$W/g2"
  est=$(estimate); printf 'est\tg0\t%s\n' "$est" >> "$OUT"; echo "estimated g0: $est B" >&2
  estd=$(estimate_dict); printf 'est\tdict\t%s\n' "$estd" >> "$OUT"; echo "estimated DICT: $estd B" >&2
  [ -n "$estd" ] && [ "$estd" -gt 0 ] || { echo "DICT estimate failed" >&2; exit 2; }
  guard $((est + 4 * GB)) "prep g0"
  printf 'save\tg0\t%s\n' "$(save_one "$BIN" "$W/g0" false)" >> "$OUT"
  printf 'size\tg0\t%s\n' "$(du -sb "$W/g0" | cut -f1)" >> "$OUT"; echo "g0: $(du -sb "$W/g0" | cut -f1) B" >&2
  guard $((estd + 3 * GB)) "prep g2"
  printf 'save\tg2\t%s\n' "$(save_one "$BIN" "$W/g2" true)" >> "$OUT"
  [ "$(avail)" -ge $((GB / 2)) ] || { rm -rf "$W/g2"; printf 'skip\tprep\tfree below 0.5 GB after g2\n' >> "$OUT"; exit 2; }
  printf 'size\tg2\t%s\n' "$(du -sb "$W/g2" | cut -f1)" >> "$OUT"
  for c in $TEXT; do
    f=$W/g2/$c
    io=$(od -An -t u8 -j $((32 + 32)) -N 8 "$f" | tr -d ' '); eo=$(od -An -t u8 -j $((32 + 56)) -N 8 "$f" | tr -d ' ')
    printf 'dictsize\t%s\t%s\n' "$c" "$((io ? eo - io : 0))" >> "$OUT"
  done
  for c in $COLS; do for v in g0 g2; do
    printf 'colsize\t%s\t%s\t%s\n' "$v" "$c" "$(stat -c %s "$W/$v/$c")" >> "$OUT"
  done; done
  peak prep
  ( cd "$REPO" && make lib >/dev/null &&
    cc -O2 -std=c17 -Wall -Wextra -Werror -Iinclude -Isrc bench/compress_estimate/p6_dictcheck.c librayforce.a \
       -lm -lpthread -o "$DC" )
  "$DC" "$W/g0" "$W/g2" | grep '^dict' >> "$OUT"
  ;;
enc)
  s=$(mktemp -p "$W/g")
  { loadline g2; printf '(map (fn [c] (println (format "col\\t%%\\t%%\\t%%\\t%%" c (at (.idx.info hits c) (quote encoded_bytes)) (at (.idx.info hits c) (quote raw_bytes)) (at (.idx.info hits c) (quote enc))))) (cols hits))\nnull\n'; } > "$s"
  "$BIN" "$s" | grep '^col' >> "$OUT"; rm -f "$s"
  ;;
mat) rel_guard "$BIN"; mat_run mat g2 ;;
warm) rel_guard "$BIN"; warm_run m "$BIN" g0 g0 "$BIN" g2 g2 "$ROUNDS" "$(seq 1 $NQ)" ;;
cold)
  rel_guard "$BIN"
  for qn in $(seq 1 $NQ); do
    for trial in 1 2 3; do for v in g0 g2; do
      calm; evict "$W/$v"
      s=$(mktemp -p "$W/g")
      { loadline "$v"; printf '(println (timeit %s))\n' "$(Q "$qn")"; } > "$s"
      if o=$("$BIN" "$s" 2>/dev/null); then r=$(printf '%s\n' "$o" | tail -1); else r=FAIL; fi; rm -f "$s"
      printf 'c\t%s\tq%s\t%s\t%s\n' "$v" "$qn" "$trial" "$r" >> "$OUT"
    done; done
  done
  ;;
ident) rel_guard "$BIN"; ident_run ident g0 g2 ;;
wall)   # per-call wall time over K top-level calls: (timeit q) on a plain
        # generation-0 vector can report a reduction as ~0 ms (#856); this cannot
  rel_guard "$BIN"
  python3 - "$OUT" > "$W/g/wallk" <<'EOF'
import collections, statistics, sys, math
w = collections.defaultdict(list)
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    f = line.rstrip("\n").split("\t")
    if f[0] == "m" and f[1] == "g0":
        try: t = [float(x) for x in f[4:10]]
        except ValueError: continue
        if len(t) == 6: w[f[2]].append(statistics.median(t[1:]))
for i in range(1, 44):
    m = statistics.median(w[f"q{i}"]) if w[f"q{i}"] else 0.0
    print(i, max(3, min(100, math.ceil(2000.0 / max(m, 0.02)))))
EOF
  for round in $(seq 1 "$WALLS"); do
    calm
    if [ $((round % 2)) = 1 ]; then order="g0 g2"; else order="g2 g0"; fi
    while read -r qn K; do
      for v in $order; do
        t=(); ok=1
        for n in 1 $((K + 1)); do
          s=$(mktemp -p "$W/g")
          { loadline "$v"; for k in $(seq 1 $n); do printf '(set r %s)\n' "$(Q "$qn")"; done; } > "$s"
          # an erroring run stops early and would time as fast: fail it, never time it
          a=$(date +%s%N); "$BIN" "$s" >/dev/null 2>&1 < /dev/null || ok=0; b=$(date +%s%N); rm -f "$s"
          t+=($(( (b - a) / 1000 )))
        done
        if [ "$ok" = 1 ]; then w=$(awk -v a="${t[0]}" -v b="${t[1]}" -v k="$K" 'BEGIN{printf "%.3f", (b - a) / k / 1000.0}'); else w=FAIL; fi
        printf 'wall\t%s\tq%s\t%s\t%s\t%s\n' "$v" "$qn" "$round" "$w" "$K" >> "$OUT"
      done
    done < "$W/g/wallk"
    echo "wall round $round done" >&2
  done
  ;;
o5)
  rel_guard "$BIN"; base_bin
  warm_run o5c "$BB" g0 base "$BIN" g0 p6 "$PROUNDS" "$(seq 1 $NQ)"
  rm -rf "$W/g0"; rm -rf "$W/bg2"
  # one store (g0 deleted first): bg2 / g0 was 0.883 in P6 Task 8, so 0.9 g0 + 0.5 GB
  if guard $(( $(awk -F'\t' '$1=="size" && $2=="g0"{print $3}' "$OUT" | tail -1) * 9 / 10 + GB / 2 )) "o5 bg2"; then
    printf 'save\tbg2\t%s\n' "$(save_one "$BB" "$W/bg2" true)" >> "$OUT"
    if [ "$(avail)" -ge $((GB * 3 / 10)) ]; then
      printf 'size\tbg2\t%s\n' "$(du -sb "$W/bg2" | cut -f1)" >> "$OUT"; peak o5
      warm_run o5d "$BB" bg2 base "$BIN" g2 p6 "$PROUNDS" "$O5Q"
    else printf 'skip\to5d\tfree below 0.3 GB after bg2\n' >> "$OUT"; fi
  else printf 'skip\to5d\tguard\n' >> "$OUT"; fi
  rm -rf "$W/bg2"
  ;;
i8)   # I8 on the SYM store (D18): read in place from $SRC, nothing written
  rel_guard "$BIN"; base_bin
  warm_run i8 "$BB" "$SRC" base "$BIN" "$SRC" p6 "$PROUNDS" "$(seq 1 $NQ)" sym
  ;;
clean) rm -rf "$W/g0" "$W/g2" "$W/bg2" "$W/s0" "$W/s2" "$W/p0" "$W/p2" ;;
savet)
  rel_guard "$BIN"; base_bin
  est=$(awk -F'\t' '$1=="est" && $2=="g0"{print $3}' "$OUT" | tail -1)
  estd=$(awk -F'\t' '$1=="est" && $2=="dict"{print $3}' "$OUT" | tail -1)
  for round in $(seq 1 "$SAVES"); do
    if [ $((round % 2)) = 1 ]; then order="p6 base"; else order="base p6"; fi
    for b in $order; do
      if [ "$b" = p6 ]; then x=$BIN; else x=$BB; fi
      calm; rm -rf "$W/s0" "$W/s2"
      guard $((est + estd + GB)) "savet" || exit 2   # s0 and s2 one after the other
      r0=$(save_one "$x" "$W/s0" false); z0=$(du -sb "$W/s0" | cut -f1); rm -rf "$W/s0"
      r2=$(save_one "$x" "$W/s2" true);  z2=$(du -sb "$W/s2" | cut -f1); rm -rf "$W/s2"
      printf 'savet\t%s\t%s\t%s\t%s\t%s\t%s\n' "$b" "$round" "$r0" "$r2" "$z0" "$z2" >> "$OUT"
    done
  done
  ;;
parted)   # R2: one store at a time under the guard; a refusal skips the arm
  rel_guard "$BIN"
  est=$(awk -F'\t' '$1=="est" && $2=="g0"{print $3}' "$OUT" | tail -1)
  estd=$(awk -F'\t' '$1=="est" && $2=="dict"{print $3}' "$OUT" | tail -1)
  rm -rf "$W/p0" "$W/p2"
  for v in p0 p2; do
    if [ "$v" = p0 ]; then need=$((est + 4 * GB)); z=false; else need=$((estd + 3 * GB)); z=true; fi
    if ! guard "$need" "parted $v"; then printf 'skip\tparted\tguard %s\n' "$v" >> "$OUT"; rm -rf "$W/p0" "$W/p2"; exit 0; fi
    s=$(mktemp -p "$W/g")
    # each partition is cast from its own slice of the source: a slice of a
    # derived STR column would keep (and a generation-0 save would write)
    # the whole column's pool
    { printf '(set h (.db.splayed.get "%s"))\n' "$SRC"
      printf '(do (map (fn [i] (do (set d (if (< i 9) (take (drop h (* i 1000000)) 1000000) (drop h (* i 1000000)))) (.db.splayed.set (format "%s/%s/%%/hits/" i) %s {compress: %s}))) (til 10)) null)\n' "$W" "$v" "$(sel d)" "$z"; } > "$s"
    "$BIN" "$s" >/dev/null; rm -f "$s"
    printf 'size\t%s\t%s\n' "$v" "$(du -sb "$W/$v" | cut -f1)" >> "$OUT"
  done
  peak parted
  s=$(mktemp -p "$W/g")
  { loadline p2 parted; printf '(map (fn [c] (println (format "pcol\\t%%\\t%%" c (at (.idx.info hits c) (quote enc))))) (cols hits))\nnull\n'; } > "$s"
  "$BIN" "$s" | grep '^pcol' >> "$OUT" || true; rm -f "$s"
  mat_run pmat p2 parted
  ident_run pident p0 p2 parted
  warm_run pm "$BIN" p0 p0 "$BIN" p2 p2 "$PROUNDS" "$(seq 1 $NQ)" parted
  rm -rf "$W/p0" "$W/p2"
  ;;
all) for st in prep enc mat ident warm cold wall o5 i8 clean savet parted; do "$0" -w "$W" -q "$QUERIES" "$st"; done ;;
esac

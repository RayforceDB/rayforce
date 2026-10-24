#!/bin/bash
# Per-query persisted-index use for the ClickBench queries.
# usage: index_use.sh DB TABLE [QUERIES]
#   DB: splayed table dir (has .d, directly or via its .current generation).
#   TABLE: table name (kept for symmetry with compress_estimate; unused).
#   Output: one line per query, "query_no<TAB>dict", the dict being what
#   .idx.uses returned for that query ({} = no index consulted).
# Index sites never consult parted segment indexes, so a parted root is
# rejected (warning, exit 2) rather than reported as empty dicts.
set -euo pipefail
DB=${1:?usage: index_use.sh DB TABLE [QUERIES]}
QUERIES=${3:-../ClickBench/rayforce/queries.rfl}
BIN=${RAY_BIN:-./rayforce}
DB=${DB%/}
splayed=0
if [ -f "$DB/.d" ]; then splayed=1
elif [ -f "$DB/.current" ]; then
  gen=$(tr -d '\n' < "$DB/.current")
  [ -f "$DB/$gen/.d" ] && splayed=1
fi
if [ "$splayed" != 1 ]; then
  echo "warning: $DB is not a splayed table directory (no .d); index use is only meaningful on a splayed table" >&2
  exit 2
fi
LOAD="(set hits (.db.splayed.get \"$DB/\"))"
n=0
# one query per paren-balanced expression, as run.sh splits them
awk '
  /^[[:space:]]*$/ { next }
  { q = q $0 "\n"; p = $0; gsub(/[^()]/, "", p)
    for (i = 1; i <= length(p); i++) { c = substr(p, i, 1); if (c == "(") d++; else d-- }
    if (d == 0) { printf "%s%c", q, 0; q = "" } }
' "$QUERIES" | while IFS= read -r -d '' q; do
  n=$((n + 1))
  script=$(mktemp)
  printf '(do %s null)\n(do (.idx.uses hits) null)\n(do %s null)\n(println (.idx.uses hits))\n' "$LOAD" "$q" > "$script"
  out=$("$BIN" "$script" 2>&1 | tail -1) || out="error"
  rm -f "$script"
  printf '%d\t%s\n' "$n" "$out"
done

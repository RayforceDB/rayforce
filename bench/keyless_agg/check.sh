#!/bin/sh
# Instruction-count bounds for the keyless aggregates of a plain column
# (#1628).  usage: bench/keyless_agg/check.sh [BINARY]   (default ./rayforce)
#
# Each shape runs the setup alone and the setup plus CALLS calls under
# callgrind, which counts the executed instructions exactly and needs no
# hardware counters; the difference per call and row is checked against the
# bound in bounds.txt (per row of the table, selected or not).  A release
# build is assumed.  The bounds sit well
# above the measured cost (a typed loop per column type, under 1 to 6
# instructions a row with gcc 11, gcc 12 and clang) and far below a per-row
# type dispatch or the generic per-row aggregate path (11 to 140 a row), so
# either coming back fails the check.
set -e
bin=${1:-./rayforce}
here=$(cd "$(dirname "$0")" && pwd)
command -v valgrind >/dev/null 2>&1 || { echo "keyless_agg: valgrind not found" >&2; exit 2; }
n=1000000
calls=3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
setup="(set n $n)
(set v (% (* (til n) 7919) 1000003))
(set T (table [v fv w] (list v (as 'F64 v) (% (til n) 10))))"
icount() {   # $1: rfl body after the setup
    printf '%s\n%s\n(exit 0)\n' "$setup" "$1" > "$tmp/q.rfl"
    valgrind --tool=callgrind --callgrind-out-file="$tmp/cg.out" "$bin" -c 1 "$tmp/q.rfl" \
        > /dev/null 2> "$tmp/cg.err"
    sed -n 's/.*Collected : \([0-9]*\).*/\1/p' "$tmp/cg.err"
}
base=$(icount "0")
fail=0
while IFS='|' read -r agg bound; do
    case "$agg" in ''|'#'*) continue ;; esac
    total=$(icount "(set f (fn [] (select {s: $agg from: T})))
(times $calls (f))")
    per=$(awk -v t="$total" -v b="$base" -v c="$calls" -v n="$n" 'BEGIN { printf "%.2f", (t - b) / c / n }')
    ok=$(awk -v p="$per" -v m="$bound" 'BEGIN { print (p <= m) ? "ok" : "FAIL" }')
    printf '%-26s %6s instructions/row (bound %s) %s\n' "$agg" "$per" "$bound" "$ok"
    [ "$ok" = ok ] || fail=1
done < "$here/bounds.txt"
exit $fail

#!/usr/bin/env bash
# Round-trip benchmark on the 10M-row table from examples/rfl/table.rfl.
#
#   1. ./rayforce bench/csv_table_gen.rfl  → writes /tmp/table_bench.csv
#   2. .csv.read in rayforce2 / read-csv in rayforce1 / read_csv in DuckDB,
#      best of N runs each.  Wall-clock from /usr/bin/time -f "%e".
#
# Usage:  bash bench/bench_csv_table.sh [runs]
#         (default runs = 3)
#
# Optional env: RAY1=/path/to/rayforce  (rayforce1 binary, default ../rayforce)
#               DUCKDB=/path/to/duckdb  (default `which duckdb`)
#
set -euo pipefail

cd "$(dirname "$0")/.."

RUNS="${1:-3}"
CSV=/tmp/table_bench.csv
RAY2=./rayforce
RAY1="${RAY1:-/home/hetoku/data/work/rayforce/rayforce}"
DUCKDB="${DUCKDB:-$(command -v duckdb || true)}"

# ── Step 1: build (release) and generate the CSV once ────────────────
if [[ ! -x $RAY2 || $RAY2 -ot src/io/csv.c || $RAY2 -ot src/core/numparse.c ]]; then
    make release > /dev/null
fi
if [[ ! -f $CSV || -n "${REGEN:-}" ]]; then
    echo "[gen] generating $CSV …"
    $RAY2 bench/csv_table_gen.rfl
fi
echo "[csv] $(ls -lh $CSV | awk '{print $5}')  $(wc -l < $CSV) lines"

# ── Step 2: time each reader ─────────────────────────────────────────
bench() {
    local label="$1"; shift
    local best="999999"
    for _ in $(seq "$RUNS"); do
        cat $CSV > /dev/null  # warm page cache
        # /usr/bin/time -f "%e" prints elapsed wall seconds on stderr
        local t
        t=$(/usr/bin/time -f "%e" "$@" 2>&1 >/dev/null | tail -1)
        if awk -v a="$t" -v b="$best" 'BEGIN{exit !(a+0 < b+0)}'; then best="$t"; fi
    done
    printf "  %-12s  %6s s\n" "$label" "$best"
}

echo
echo "── best of $RUNS runs (wall-clock) ──"
bench "rayforce2"  "$RAY2" bench/csv_table_read_r2.rfl

if [[ -x $RAY1 ]]; then
    bench "rayforce1"  "$RAY1" bench/csv_table_read_r1.rfl
else
    echo "  rayforce1     [skip — $RAY1 not found]"
fi

if [[ -n "$DUCKDB" && -x $DUCKDB ]]; then
    bench "duckdb"   "$DUCKDB" -c "COPY (SELECT * FROM read_csv('$CSV', header=true)) TO '/dev/null' (FORMAT CSV);"
else
    echo "  duckdb        [skip — duckdb not in PATH]"
fi
echo

#!/bin/bash
# Compare CSV loading: Teide vs DuckDB
# Usage: ./bench/bench_csv_vs_duckdb.sh

set -e
CSV_DIR="/tmp/teide_bench_csvs"
mkdir -p "$CSV_DIR"

# Generate test CSVs
echo "Generating test CSVs..."

python3 -c "
import csv, sys
configs = [
    ('100k_100sym', 100000, 100),
    ('1m_100sym', 1000000, 100),
    ('100k_100ksym', 100000, 100000),
    ('1m_100ksym', 1000000, 100000),
    ('1m_1msym', 1000000, 1000000),
    ('1m_numeric', 1000000, 0),
]
for name, nrows, nuniq in configs:
    path = f'$CSV_DIR/{name}.csv'
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        if nuniq > 0:
            w.writerow(['id','val','sym'])
            for i in range(nrows):
                w.writerow([i, round(i*0.1, 2), f'sym_{i % nuniq}'])
        else:
            w.writerow(['id','val'])
            for i in range(nrows):
                w.writerow([i, round(i*0.1, 2)])
    sz = __import__('os').path.getsize(path)
    print(f'  {name}: {nrows} rows, {sz/1024/1024:.1f} MB')
"

echo ""
echo "=== DuckDB CSV Load Times ==="
echo ""

for f in "$CSV_DIR"/*.csv; do
    name=$(basename "$f" .csv)
    # Warm up page cache
    cat "$f" > /dev/null
    # Time DuckDB load (best of 3)
    best=""
    for i in 1 2 3; do
        t=$(duckdb -c ".timer on" -c "CREATE TABLE t AS SELECT * FROM read_csv('$f');" -c "SELECT count(*) FROM t;" -c "DROP TABLE t;" 2>&1 | grep -oP 'Run Time.*?(\d+\.\d+)s' | grep -oP '\d+\.\d+' | tail -1)
        if [ -z "$best" ] || [ "$(echo "$t < $best" | bc -l)" = "1" ]; then
            best=$t
        fi
    done
    printf "  %-24s  %8s ms\n" "$name" "$(echo "$best * 1000" | bc -l | xargs printf '%.1f')"
done

echo ""
echo "=== Teide CSV Load Times ==="
echo ""

# Build teide bench if needed
if [ ! -f build_release/bench_csv_detail ]; then
    cmake -B build_release -DCMAKE_BUILD_TYPE=Release -DTEIDE_BENCH=ON > /dev/null 2>&1
    cmake --build build_release -j$(nproc) > /dev/null 2>&1
fi

# Write a C program that loads specific files
cat > /tmp/teide_bench_files.c << 'CEOF'
#define _POSIX_C_SOURCE 199309L
#include <teide/td.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void bench(const char* label, const char* path) {
    double best = 1e18;
    for (int r = 0; r < 3; r++) {
        td_heap_init();
        td_sym_init();
        double start = now_ns();
        td_t* t = td_read_csv(path);
        double elapsed = now_ns() - start;
        if (t && !TD_IS_ERR(t)) td_release(t);
        td_sym_destroy();
        td_heap_destroy();
        if (elapsed < best) best = elapsed;
    }
    printf("  %-24s  %8.1f ms\n", label, best / 1e6);
}

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        bench(argv[i], argv[i+1]);
    }
    return 0;
}
CEOF

gcc -O2 -o /tmp/teide_bench_files /tmp/teide_bench_files.c \
    -I include -I src -Lbuild_release -lteide_static -lm -lpthread 2>/dev/null \
|| cc -O2 -o /tmp/teide_bench_files /tmp/teide_bench_files.c \
    build_release/libteide_static.a -I include -I src -lm -lpthread

/tmp/teide_bench_files \
    "100k_100sym" "$CSV_DIR/100k_100sym.csv" \
    "1m_100sym" "$CSV_DIR/1m_100sym.csv" \
    "100k_100ksym" "$CSV_DIR/100k_100ksym.csv" \
    "1m_100ksym" "$CSV_DIR/1m_100ksym.csv" \
    "1m_1msym" "$CSV_DIR/1m_1msym.csv" \
    "1m_numeric" "$CSV_DIR/1m_numeric.csv"

echo ""
echo "=== Cleanup ==="
rm -rf "$CSV_DIR" /tmp/teide_bench_files /tmp/teide_bench_files.c
echo "Done."

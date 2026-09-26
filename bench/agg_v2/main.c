/* Aggregation-engine A/B perf microbench (wide group-by shapes).
 *
 * Generates one grouped table with a fixed-seed PRNG (identical data on
 * every run / every branch) and times the group-by shapes that the old
 * hand-tuned "rowform" group operators targeted.  On THIS branch (feat/
 * agg-engine-phase0) OP_GROUP routes through the v2 composable aggregation
 * engine (ray_agg_engine_v2 default true); on master the same C API drives
 * the rowform operators.  The bench is API-identical on both branches (it
 * never touches the v2 knob — it measures each branch's default path).
 *
 * For each shape we print median / min exec ms over M timed reps plus a
 * checksum so the A/B can confirm both binaries computed the same answer.
 *
 *   Q7  max(v1), min(v2)               by id3                 (MAXMIN rowform)
 *   Q10 sum(v3), count                 by id1..id6 (6 keys)   (SUM_COUNT)
 *   Q9  pearson(v1,v2)                 by id2,id4             (PEARSON)
 *   Q6  median(v3), stddev(v3)         by id4,id5             (MEDIAN_STDDEV)
 *   Q8  top(v3,2)                      by id6 (HIGH card)     (TOPK)
 *   S1  sum(v1)                        by id1  (low card)
 *   S2  sum(v1)                        by id6  (HIGH card)
 *
 * High-reduction always-scatter probe (adversarial): hold the group COUNT
 * fixed and vary ONLY key DENSITY to isolate the radix "scatter all N rows"
 * tax for shapes with very few groups but keys too wide for the dense cap.
 *   R1d sum(v1),count by rk1d  (64 groups, key=i%64        → DENSE)
 *   R1s sum(v1),count by rk1s  (64 groups, key=(i%64)*1e8  → RADIX, range>cap)
 *   R2d sum(v1),count by rk2d  (4096 groups, key=i%4096     → DENSE)
 *   R2s sum(v1),count by rk2s  (4096 groups, key=(i%4096)*1e8→ RADIX, range>cap)
 * R*d and R*s produce identical group counts & aggregate results; only the
 * key VALUES (hence the chosen strategy) differ.  sparse/dense time = the tax.
 */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "table/sym.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#endif

/* ---------- timing ---------- */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}
static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double median(double* arr, int n) {
    qsort(arr, (size_t)n, sizeof(double), cmp_double);
    if (n % 2 == 1) return arr[n / 2];
    return (arr[n / 2 - 1] + arr[n / 2]) * 0.5;
}
static double vmin(const double* arr, int n) {
    double m = arr[0];
    for (int i = 1; i < n; i++) if (arr[i] < m) m = arr[i];
    return m;
}
static long max_rss_kb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (long)(pmc.PeakWorkingSetSize / 1024);
#else
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
#endif
}

/* ---------- deterministic PRNG (splitmix64) ---------- */
static uint64_t g_rng;
static uint64_t rng_next(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static int64_t rng_range(int64_t n) { return (int64_t)(rng_next() % (uint64_t)n); }

/* ---------- table config ---------- */
#ifndef NROWS
#  define NROWS 10000000L
#endif
#define CARD_LOW   100L      /* id1,id2,id3,id4,id5: ~100 distinct */
#define CARD_HIGH  1000000L  /* id6: ~1e6 distinct  */
#define V1_RANGE   100L
#define V2_RANGE   100L

static ray_t* g_tbl = NULL;
/* keep interned sym ids for id1..id3 so SYM columns share a small vocab */
static int64_t g_sym_id[CARD_LOW];

static ray_t* build_sym_col(int64_t* gen) {
    /* RAY_SYM W64 = int64 array of intern ids; fill + set len. */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, NROWS);
    if (RAY_IS_ERR(v)) { fprintf(stderr, "sym_vec_new OOM\n"); abort(); }
    int64_t* d = (int64_t*)ray_data(v);
    memcpy(d, gen, (size_t)NROWS * sizeof(int64_t));
    v->len = NROWS;
    return v;
}
static ray_t* build_i64_col(int64_t* gen) {
    ray_t* v = ray_vec_from_raw(RAY_I64, gen, NROWS);
    if (RAY_IS_ERR(v)) { fprintf(stderr, "vec_from_raw I64 OOM\n"); abort(); }
    return v;
}
static ray_t* build_f64_col(double* gen) {
    ray_t* v = ray_vec_from_raw(RAY_F64, gen, NROWS);
    if (RAY_IS_ERR(v)) { fprintf(stderr, "vec_from_raw F64 OOM\n"); abort(); }
    return v;
}

static void add_col(const char* name, ray_t* col) {
    g_tbl = ray_table_add_col(g_tbl, ray_sym_intern(name, strlen(name)), col);
    ray_release(col);
}

static void build_table(void) {
    /* intern a 100-entry vocab "id001".."id100" for the SYM columns */
    for (int64_t i = 0; i < CARD_LOW; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "id%03lld", (long long)(i + 1));
        g_sym_id[i] = ray_sym_intern(buf, strlen(buf));
    }

    int64_t* s1 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* s2 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* s3 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* i4 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* i5 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* i6 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* w1 = malloc((size_t)NROWS * sizeof(int64_t));
    int64_t* w2 = malloc((size_t)NROWS * sizeof(int64_t));
    double*  w3 = malloc((size_t)NROWS * sizeof(double));
    /* High-reduction probe keys: same group COUNT, different key density. */
    int64_t* r1d = malloc((size_t)NROWS * sizeof(int64_t));  /* i%64        */
    int64_t* r1s = malloc((size_t)NROWS * sizeof(int64_t));  /* (i%64)*1e8  */
    int64_t* r2d = malloc((size_t)NROWS * sizeof(int64_t));  /* i%4096      */
    int64_t* r2s = malloc((size_t)NROWS * sizeof(int64_t));  /* (i%4096)*1e8*/
    if (!s1||!s2||!s3||!i4||!i5||!i6||!w1||!w2||!w3||!r1d||!r1s||!r2d||!r2s) {
        fprintf(stderr,"OOM gen\n"); abort();
    }

    g_rng = 0xD1CE5EEDULL;  /* fixed seed */
    for (int64_t r = 0; r < NROWS; r++) {
        s1[r] = g_sym_id[rng_range(CARD_LOW)];
        s2[r] = g_sym_id[rng_range(CARD_LOW)];
        s3[r] = g_sym_id[rng_range(CARD_LOW)];
        i4[r] = 1 + rng_range(CARD_LOW);       /* ~100 distinct */
        i5[r] = 1 + rng_range(CARD_LOW);       /* ~100 distinct */
        i6[r] = 1 + rng_range(CARD_HIGH);      /* ~1e6 distinct */
        w1[r] = 1 + rng_range(V1_RANGE);
        w2[r] = 1 + rng_range(V2_RANGE);
        w3[r] = (double)rng_range(100000000LL) * 1e-3;  /* ~[0,1e5) */
        /* deterministic dense/sparse key pairs (no PRNG: row-derived) */
        int64_t g64   = r % 64;
        int64_t g4096 = r % 4096;
        r1d[r] = g64;                          /* range 0..63    → dense  */
        r1s[r] = g64   * 100000001LL;          /* range 0..~6.3e9 → radix */
        r2d[r] = g4096;                        /* range 0..4095  → dense  */
        r2s[r] = g4096 * 100000001LL;          /* range 0..~4.1e11→ radix */
    }

    g_tbl = ray_table_new(13);
    add_col("id1", build_sym_col(s1));
    add_col("id2", build_sym_col(s2));
    add_col("id3", build_sym_col(s3));
    add_col("id4", build_i64_col(i4));
    add_col("id5", build_i64_col(i5));
    add_col("id6", build_i64_col(i6));
    add_col("v1",  build_i64_col(w1));
    add_col("v2",  build_i64_col(w2));
    add_col("v3",  build_f64_col(w3));
    add_col("rk1d", build_i64_col(r1d));
    add_col("rk1s", build_i64_col(r1s));
    add_col("rk2d", build_i64_col(r2d));
    add_col("rk2s", build_i64_col(r2s));

    free(s1);free(s2);free(s3);free(i4);free(i5);free(i6);free(w1);free(w2);free(w3);
    free(r1d);free(r1s);free(r2d);free(r2s);
}

/* ---------- checksum: fold a result table into a single double ---------
 * Sums numeric cells of every non-LIST column (I64/F64) plus folds row
 * count.  For LIST columns (Q8 top output) we descend one level and sum
 * the numeric cells of each cell-vector.  This makes the checksum sensitive
 * to the actual aggregate values, not just shape. */
static double fold_numeric_vec(ray_t* col) {
    double acc = 0.0;
    int64_t n = ray_len(col);
    switch (ray_type(col)) {
        case RAY_I64: { int64_t* d = ray_data(col); for (int64_t i=0;i<n;i++) acc += (double)d[i]; } break;
        case RAY_F64: { double*  d = ray_data(col); for (int64_t i=0;i<n;i++) if (!isnan(d[i])) acc += d[i]; } break;
        case RAY_F32: { float*   d = ray_data(col); for (int64_t i=0;i<n;i++) if (!isnan(d[i])) acc += (double)d[i]; } break;
        case RAY_I32: { int32_t* d = ray_data(col); for (int64_t i=0;i<n;i++) acc += (double)d[i]; } break;
        case RAY_SYM: { int64_t* d = ray_data(col); /* W64 assumed */ for (int64_t i=0;i<n;i++) acc += (double)d[i]; } break;
        case RAY_LIST: {
            for (int64_t i=0;i<n;i++) {
                ray_t* cell = ray_list_get(col, i);
                if (cell && !RAY_IS_ERR(cell)) acc += fold_numeric_vec(cell);
            }
        } break;
        default: break;  /* ignore other types */
    }
    return acc;
}
static double checksum_table(ray_t* t) {
    double acc = (double)ray_table_nrows(t);
    /* iterate columns by index */
    for (int64_t c = 0; ; c++) {
        ray_t* col = ray_table_get_col_idx(t, c);
        if (!col || RAY_IS_ERR(col)) break;
        acc += fold_numeric_vec(col);
    }
    return acc;
}

/* ---------- shapes ---------- */
typedef struct { const char* name; const char* note; } shape_meta_t;

typedef ray_op_t* (*build_fn)(ray_graph_t* g);

static ray_op_t* q7_maxmin(ray_graph_t* g) {     /* max(v1),min(v2) by id3 */
    ray_op_t* keys[] = { ray_scan(g, "id3") };
    uint16_t  ops[]  = { OP_MAX, OP_MIN };
    ray_op_t* ins[]  = { ray_scan(g,"v1"), ray_scan(g,"v2") };
    return ray_group(g, keys, 1, ops, ins, 2);
}
static ray_op_t* q10_sumcount(ray_graph_t* g) {  /* sum(v3),count by id1..id6 */
    ray_op_t* keys[] = { ray_scan(g,"id1"), ray_scan(g,"id2"), ray_scan(g,"id3"),
                         ray_scan(g,"id4"), ray_scan(g,"id5"), ray_scan(g,"id6") };
    uint16_t  ops[]  = { OP_SUM, OP_COUNT };
    ray_op_t* ins[]  = { ray_scan(g,"v3"), ray_scan(g,"v3") };
    return ray_group(g, keys, 6, ops, ins, 2);
}
static ray_op_t* q9_pearson(ray_graph_t* g) {    /* pearson(v1,v2) by id2,id4 */
    ray_op_t* keys[] = { ray_scan(g,"id2"), ray_scan(g,"id4") };
    uint16_t  ops[]  = { OP_PEARSON_CORR };
    ray_op_t* ins[]  = { ray_scan(g,"v1") };
    ray_op_t* ins2[] = { ray_scan(g,"v2") };
    return ray_group2(g, keys, 2, ops, ins, ins2, 1);
}
static ray_op_t* q6_medstd(ray_graph_t* g) {     /* median(v3),stddev(v3) by id4,id5 */
    ray_op_t* keys[] = { ray_scan(g,"id4"), ray_scan(g,"id5") };
    uint16_t  ops[]  = { OP_MEDIAN, OP_STDDEV };
    ray_op_t* ins[]  = { ray_scan(g,"v3"), ray_scan(g,"v3") };
    return ray_group(g, keys, 2, ops, ins, 2);
}
static ray_op_t* q8_top2(ray_graph_t* g) {       /* top(v3,2) by id6 (HIGH card) */
    ray_op_t* keys[] = { ray_scan(g,"id6") };
    uint16_t  ops[]  = { OP_TOP_N };
    ray_op_t* ins[]  = { ray_scan(g,"v3") };
    int64_t   kk[]   = { 2 };
    return ray_group3(g, keys, 1, ops, ins, NULL, kk, 1);
}
static ray_op_t* s1_sum_id1(ray_graph_t* g) {    /* sum(v1) by id1 (low card) */
    ray_op_t* keys[] = { ray_scan(g,"id1") };
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { ray_scan(g,"v1") };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* s2_sum_id6(ray_graph_t* g) {    /* sum(v1) by id6 (HIGH card) */
    ray_op_t* keys[] = { ray_scan(g,"id6") };
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { ray_scan(g,"v1") };
    return ray_group(g, keys, 1, ops, ins, 1);
}

/* High-reduction always-scatter probe: sum(v1),count by a fixed-group-count key.
 * R1d/R2d use compact keys (→ DENSE); R1s/R2s use the SAME group count but
 * sparse keys whose range exceeds DENSE_MAX_SLOTS (→ RADIX, scatters all N). */
static ray_op_t* r_sumcount(ray_graph_t* g, const char* keycol) {
    ray_op_t* keys[] = { ray_scan(g, keycol) };
    uint16_t  ops[]  = { OP_SUM, OP_COUNT };
    ray_op_t* ins[]  = { ray_scan(g,"v1"), ray_scan(g,"v1") };
    return ray_group(g, keys, 1, ops, ins, 2);
}
static ray_op_t* r1_dense(ray_graph_t* g) { return r_sumcount(g, "rk1d"); }
static ray_op_t* r1_sparse(ray_graph_t* g){ return r_sumcount(g, "rk1s"); }
static ray_op_t* r2_dense(ray_graph_t* g) { return r_sumcount(g, "rk2d"); }
static ray_op_t* r2_sparse(ray_graph_t* g){ return r_sumcount(g, "rk2s"); }

#define N_SHAPES 11
static const shape_meta_t SHAPES[N_SHAPES] = {
    {"Q7  max,min by id3",        "MAXMIN"},
    {"Q10 sum,count by id1..6",   "SUM_COUNT 6key"},
    {"Q9  pearson by id2,id4",    "PEARSON"},
    {"Q6  median,stddev by id4,5","MEDIAN_STDDEV"},
    {"Q8  top(v3,2) by id6",      "TOPK high-card"},
    {"S1  sum(v1) by id1",        "low-card"},
    {"S2  sum(v1) by id6",        "high-card"},
    {"R1d sum,count 64g dense",   "64 groups DENSE"},
    {"R1s sum,count 64g sparse",  "64 groups RADIX"},
    {"R2d sum,count 4096g dense", "4096 groups DENSE"},
    {"R2s sum,count 4096g sparse","4096 groups RADIX"},
};
static const build_fn BUILDERS[N_SHAPES] = {
    q7_maxmin, q10_sumcount, q9_pearson, q6_medstd, q8_top2, s1_sum_id1, s2_sum_id6,
    r1_dense, r1_sparse, r2_dense, r2_sparse,
};

#define N_WARM 2
#define N_REP  9

static double run_one(int shape, double* exec_ms_out /*may be NULL*/, double* checksum_out /*may be NULL*/) {
    ray_graph_t* g = ray_graph_new(g_tbl);
    ray_op_t* root = BUILDERS[shape](g);
    root = ray_optimize(g, root);

    double t0 = now_ms();
    ray_t* res = ray_execute(g, root);
    double dt = now_ms() - t0;

    if (RAY_IS_ERR(res)) {
        fprintf(stderr, "shape %d (%s) ERROR\n", shape, SHAPES[shape].name);
        abort();
    }
    if (checksum_out) *checksum_out = checksum_table(res);
    if (exec_ms_out)  *exec_ms_out  = dt;

    ray_release(res);
    ray_graph_free(g);
    return dt;
}

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();

    printf("=== bench-agg-v2 (agg engine A/B) ===\n");
    printf("rows=%ld  warmup=%d  timed=%d  seed=0xD1CE5EED\n",
           (long)NROWS, N_WARM, N_REP);
    printf("Building table...\n");
    double bt0 = now_ms();
    build_table();
    printf("Table built: %lld rows, 9 cols in %.0f ms\n\n",
           (long long)ray_table_nrows(g_tbl), now_ms() - bt0);

    double exec[N_SHAPES][N_REP];
    double checksum[N_SHAPES];
    int64_t outrows[N_SHAPES];

    for (int s = 0; s < N_SHAPES; s++) {
        for (int w = 0; w < N_WARM; w++) run_one(s, NULL, NULL);
        for (int r = 0; r < N_REP; r++) {
            double cs;
            run_one(s, &exec[s][r], &cs);
            checksum[s] = cs;
        }
        /* capture output row count once */
        ray_graph_t* g = ray_graph_new(g_tbl);
        ray_op_t* root = ray_optimize(g, BUILDERS[s](g));
        ray_t* res = ray_execute(g, root);
        outrows[s] = ray_table_nrows(res);
        ray_release(res); ray_graph_free(g);
        printf("  [%d/%d] %-28s done\n", s+1, N_SHAPES, SHAPES[s].name);
    }

    printf("\n%-28s %12s %12s %10s %16s\n",
           "shape", "median_ms", "min_ms", "out_rows", "checksum");
    printf("%-28s %12s %12s %10s %16s\n",
           "----------------------------", "------------", "------------",
           "----------", "----------------");
    for (int s = 0; s < N_SHAPES; s++) {
        double med = median(exec[s], N_REP);
        double mn  = vmin(exec[s], N_REP);
        printf("%-28s %12.3f %12.3f %10lld %16.6g\n",
               SHAPES[s].name, med, mn, (long long)outrows[s], checksum[s]);
    }

    printf("\npeak RSS: %ld KB (%.1f MB)\n", max_rss_kb(), max_rss_kb()/1024.0);

    printf("\n--- raw per-rep exec_ms ---\n");
    for (int s = 0; s < N_SHAPES; s++) {
        printf("%-28s", SHAPES[s].name);
        for (int r = 0; r < N_REP; r++) printf(" %7.1f", exec[s][r]);
        printf("\n");
    }

    ray_release(g_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}

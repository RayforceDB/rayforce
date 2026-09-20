/* Null-free join key perf gate (#597).
 *
 * The join tests every key cell for null on every row: once per key column
 * in hash_row_keys (build and probe, plus the prefetch lookahead) and twice
 * per key column per hash-chain step in join_keys_eq, across both the count
 * and the fill pass.  ray_vec_is_null is out-of-line (no LTO), so each test
 * is a call.  A reporter profiled 13.28% of a service's samples there, on a
 * join keyed by two SYM columns that structurally never hold a null.
 *
 * join_keys_nullfree proves once per join that no key column can hold a
 * null and the loops drop the call.  This measures what that is worth.
 *
 * Cases (all two-key SYM joins, mirroring the reported venue+instrument
 * book shape):
 *   SYM2         right=500K (unique venue+instrument pairs), left=4M
 *                drawn from those pairs, both key columns null-free, so
 *                each left row matches exactly one right row — the shape
 *                of a book join, not a fan-out.  The fast path must fire.
 *   SYM2-NULL    identical, except one left venue cell is the SYM null.
 *                The fast path must NOT fire; both sides must time alike,
 *                bounding what the proof scan itself costs.
 *   I64          right=500K, left=4M, single I64 key, HAS_NULLS clear.
 *                Fast path fires via the attrs bit rather than a scan.
 *
 * Mechanism: ray_join_nullfree_keys must advance on SYM2 and I64 and must
 * not advance on SYM2-NULL.  ray_join_force_null_checks supplies the
 * null-aware baseline in the same binary.
 *
 * Timing: CLOCK_MONOTONIC around ray_execute only.  Tables built once
 * outside the timed loop; graph rebuilt per rep; sides interleaved per rep
 * so drift hits both equally.
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
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static double medianN(double arr[], int n) {
    double tmp[64];
    memcpy(tmp, arr, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    return tmp[n / 2];
}
static double minN(double arr[], int n) {
    double m = arr[0];
    for (int i = 1; i < n; i++) if (arr[i] < m) m = arr[i];
    return m;
}

/* ---------- SYM column over a vocabulary of `vocab` interned symbols ------
 * Cell i takes vocabulary entry (i * stride) % vocab.  null_at, when >= 0,
 * is written as SYM id 0 — the canonical SYM null, which HAS_NULLS does not
 * track, so only a payload scan can see it. */
static ray_t* make_sym_col(const char* prefix, int64_t n, int64_t vocab,
                           int64_t stride, int64_t null_at) {
    ray_t* col = ray_sym_vec_new(RAY_SYM_W64, n);
    if (!col || RAY_IS_ERR(col)) { fprintf(stderr, "make_sym_col: alloc\n"); abort(); }
    col->len = n;

    int64_t* ids = (int64_t*)malloc((size_t)vocab * sizeof(int64_t));
    if (!ids) { fprintf(stderr, "make_sym_col: OOM vocab\n"); abort(); }
    for (int64_t v = 0; v < vocab; v++) {
        char b[32];
        int m = snprintf(b, sizeof(b), "%s%lld", prefix, (long long)v);
        ids[v] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++)
        ray_write_sym(ray_data(col), i, (uint64_t)ids[(i * stride) % vocab],
                      RAY_SYM, col->attrs);
    if (null_at >= 0 && null_at < n)
        ray_write_sym(ray_data(col), null_at, 0, RAY_SYM, col->attrs);
    free(ids);
    return col;
}

static ray_t* make_i64_col(int64_t n, int64_t mod) {
    int64_t* v = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!v) { fprintf(stderr, "make_i64_col: OOM\n"); abort(); }
    for (int64_t i = 0; i < n; i++) v[i] = i % mod;
    ray_t* col = ray_vec_from_raw(RAY_I64, v, n);
    free(v);
    if (!col || RAY_IS_ERR(col)) { fprintf(stderr, "make_i64_col: from_raw\n"); abort(); }
    return col;
}

static ray_t* table_of(const char* const* names, ray_t* const* cols, int64_t ncols) {
    ray_t* tbl = ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++)
        tbl = ray_table_add_col(tbl, ray_sym_intern(names[c], strlen(names[c])), cols[c]);
    if (!tbl || RAY_IS_ERR(tbl)) { fprintf(stderr, "table_of: add_col\n"); abort(); }
    return tbl;
}

/* ---------- one inner-join rep ---------- */
static double run_join_rep(ray_t* lt, const char* const* lkeys,
                           ray_t* rt, const char* const* rkeys,
                           uint32_t n_keys, int64_t* rows_out) {
    ray_graph_t* g = ray_graph_new(lt);
    if (!g) { fprintf(stderr, "run_join_rep: graph alloc\n"); abort(); }

    ray_op_t* lt_node = ray_const_table(g, lt);
    ray_op_t* rt_node = ray_const_table(g, rt);
    ray_op_t* lk_arr[4];
    ray_op_t* rk_arr[4];
    for (uint32_t k = 0; k < n_keys; k++) {
        lk_arr[k] = ray_scan(g, lkeys[k]);
        rk_arr[k] = ray_scan(g, rkeys[k]);
        if (!lk_arr[k] || !rk_arr[k]) { fprintf(stderr, "run_join_rep: key node\n"); abort(); }
    }
    if (!lt_node || !rt_node) { fprintf(stderr, "run_join_rep: node alloc\n"); abort(); }

    ray_op_t* jn = ray_join(g, lt_node, lk_arr, rt_node, rk_arr, n_keys, 0);
    if (!jn) { fprintf(stderr, "run_join_rep: join node\n"); abort(); }
    jn = ray_optimize(g, jn);

    double t0 = now_ms();
    ray_t* result = ray_execute(g, jn);
    double t1 = now_ms();

    if (!result || RAY_IS_ERR(result)) {
        fprintf(stderr, "run_join_rep: execute returned error\n"); abort();
    }
    if (rows_out) *rows_out = ray_table_nrows(result);
    ray_release(result);
    ray_graph_free(g);
    return t1 - t0;
}

#define NREPS 11

typedef struct {
    const char* name;
    double fast_ms[NREPS];    /* knob off — null-free proof allowed */
    double base_ms[NREPS];    /* knob on  — null-aware loops (pre-#597) */
    int64_t rows_out;
} case_result_t;

static void run_case(const char* name,
                     ray_t* lt, const char* const* lkeys,
                     ray_t* rt, const char* const* rkeys,
                     uint32_t n_keys, bool expect_fast,
                     case_result_t* cr) {
    cr->name = name;
    cr->rows_out = -1;

    printf("Running case %-12s (%d reps)...\n", name, NREPS);
    fflush(stdout);

    uint64_t nf_before = ray_join_nullfree_keys;

    for (int rep = 0; rep < NREPS; rep++) {
        ray_join_force_null_checks = false;
        int64_t rows_f = -1;
        cr->fast_ms[rep] = run_join_rep(lt, lkeys, rt, rkeys, n_keys, &rows_f);

        ray_join_force_null_checks = true;
        int64_t rows_b = -1;
        cr->base_ms[rep] = run_join_rep(lt, lkeys, rt, rkeys, n_keys, &rows_b);
        ray_join_force_null_checks = false;

        if (rows_f != rows_b) {
            fprintf(stderr,
                "CORRECTNESS FAILURE case %s rep %d: fast=%lld rows, baseline=%lld rows\n",
                name, rep, (long long)rows_f, (long long)rows_b);
            abort();
        }
        cr->rows_out = rows_f;
    }

    bool fired = ray_join_nullfree_keys > nf_before;
    if (expect_fast != fired) {
        fprintf(stderr,
            "MECHANISM FAILURE case %s: expected null-free path %s "
            "(before=%llu after=%llu)\n",
            name, expect_fast ? "to fire" : "NOT to fire",
            (unsigned long long)nf_before,
            (unsigned long long)ray_join_nullfree_keys);
        abort();
    }

    printf("  nullfree counter: before=%llu after=%llu fired=%s  rows=%lld\n",
           (unsigned long long)nf_before,
           (unsigned long long)ray_join_nullfree_keys,
           fired ? "YES" : "NO",
           (long long)cr->rows_out);
    fflush(stdout);
}

static void report(const case_result_t* cr) {
    double fmed = medianN((double*)cr->fast_ms, NREPS);
    double bmed = medianN((double*)cr->base_ms, NREPS);
    double fmin = minN((double*)cr->fast_ms, NREPS);
    double bmin = minN((double*)cr->base_ms, NREPS);
    printf("%-12s  median %8.2f -> %8.2f ms  (%+6.1f%%)   min %8.2f -> %8.2f ms  (%+6.1f%%)\n",
           cr->name, bmed, fmed, 100.0 * (fmed - bmed) / bmed,
           bmin, fmin, 100.0 * (fmin - bmin) / bmin);
}

int main(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_join_force_null_checks = false;

    printf("=== bench-join-nullfree (#597) ===\n");
    printf("NREPS=%d  RAY_PARALLEL_THRESHOLD=%d\n\n",
           NREPS, (int)RAY_PARALLEL_THRESHOLD);
    fflush(stdout);

    const int64_t nl = 4000000L;   /* probe side */
    const int64_t nr =  500000L;   /* build side */

    /* ---- SYM2: two null-free SYM key columns ---- */
    printf("Building SYM2 tables (left=%lld, right=%lld)...\n",
           (long long)nl, (long long)nr);
    fflush(stdout);
    {
        /* Right: instrument unique per row, venue spread over 64 — 500K
         * distinct (venue, instrument) pairs.  Left: the same pairs cycled,
         * so every left row has exactly one match. */
        ray_t* rv = make_sym_col("venue", nr, nr, 1, -1);
        ray_t* ri = make_sym_col("inst",  nr, nr, 1, -1);
        ray_t* lv = make_sym_col("venue", nl, nr, 1, -1);
        ray_t* li = make_sym_col("inst",  nl, nr, 1, -1);

        /* SYM2-NULL shares the build side and differs only in one left cell. */
        ray_t* lv_null = make_sym_col("venue", nl, nr, 1, nl / 2);
        ray_t* li_null = make_sym_col("inst",  nl, nr, 1, -1);

        static const char* const lnames[] = { "lvenue", "linst" };
        static const char* const rnames[] = { "rvenue", "rinst" };
        static const char* const lkeys[]  = { "lvenue", "linst" };
        static const char* const rkeys[]  = { "rvenue", "rinst" };

        ray_t* lcols[2] = { lv, li };
        ray_t* rcols[2] = { rv, ri };
        ray_t* lcols_n[2] = { lv_null, li_null };
        ray_t* lt = table_of(lnames, lcols, 2);
        ray_t* rt = table_of(rnames, rcols, 2);
        ray_t* lt_null = table_of(lnames, lcols_n, 2);
        ray_release(lv); ray_release(li); ray_release(rv); ray_release(ri);
        ray_release(lv_null); ray_release(li_null);

        case_result_t cr_sym2, cr_sym2n;
        run_case("SYM2",      lt,      lkeys, rt, rkeys, 2, true,  &cr_sym2);
        run_case("SYM2-NULL", lt_null, lkeys, rt, rkeys, 2, false, &cr_sym2n);

        printf("\n--- results (baseline = forced null-aware loops) ---\n");
        report(&cr_sym2);
        report(&cr_sym2n);

        ray_release(lt); ray_release(rt); ray_release(lt_null);
    }

    /* ---- I64: single key, proof is the HAS_NULLS bit ---- */
    printf("\nBuilding I64 tables (left=%lld, right=%lld)...\n",
           (long long)nl, (long long)nr);
    fflush(stdout);
    {
        ray_t* lc = make_i64_col(nl, nr);
        ray_t* rc = make_i64_col(nr, nr);
        static const char* const lnames[] = { "lk" };
        static const char* const rnames[] = { "rk" };
        static const char* const lkeys[]  = { "lk" };
        static const char* const rkeys[]  = { "rk" };
        ray_t* lcols[1] = { lc };
        ray_t* rcols[1] = { rc };
        ray_t* lt = table_of(lnames, lcols, 1);
        ray_t* rt = table_of(rnames, rcols, 1);
        ray_release(lc); ray_release(rc);

        case_result_t cr_i64;
        run_case("I64", lt, lkeys, rt, rkeys, 1, true, &cr_i64);
        printf("\n--- results (baseline = forced null-aware loops) ---\n");
        report(&cr_i64);

        ray_release(lt); ray_release(rt);
    }

    printf("\n(negative %% = faster with the null-free path)\n");
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}

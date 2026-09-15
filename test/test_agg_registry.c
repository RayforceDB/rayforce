/* test/test_agg_registry.c — differential: vtable accumulator ≡ current ray_*_fn.
 * Phase 0 oracle = the existing reduction functions. */
#include "test.h"
#include <rayforce.h>
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "lang/eval.h"
#include "lang/internal.h"  /* topk_take_vec — top_n/bot_n differential oracle */
#include "mem/heap.h"
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>

static ray_t* vec_i64(const int64_t* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    memcpy(ray_data(v), xs, (size_t)n * sizeof(int64_t));
    return v;
}

/* Run a vtable accumulator over the whole vector as a single group (gid 0). */
static ray_t* run_single_group(const agg_vtable_t* vt, ray_t* col) {
    void* state = calloc(1, vt->state_size);
    vt->init(state);
    uint32_t* gids = calloc((size_t)col->len, sizeof(uint32_t));  /* all zero */
    ray_valid_t valid = { ray_data(col), col->type,
                          (col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch(state, vt->state_size, gids, ray_data(col), &valid, col->len, NULL);
    ray_t* out = vt->finalize(state, NULL, 0);
    if (vt->destroy) vt->destroy(state);  /* buffered accumulators own heap state */
    free(gids); free(state);
    return out;
}

static test_result_t test_sum_i64_matches_reduction(void) {
    /* ray_sum_fn(RAY_I64) reduces via the parallel morsel/DAG executor,
     * which needs the heap + runtime up — mirror the other exec tests. */
    ray_heap_init();
    (void)ray_sym_init();

    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    const agg_vtable_t* vt = agg_resolve(OP_SUM, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    /* ray_sum_fn on an I64 vector returns a lazy DAG wrapper — force it to a
     * concrete scalar atom before comparing. */
    ray_t* want = ray_lazy_materialize(ray_sum_fn(col));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_FALSE(RAY_IS_ERR(want));
    TEST_ASSERT_EQ_I(got->i64, want->i64);
    ray_release(got); ray_release(want); ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_minmax_count_i64_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t xs[] = { 10, 20, 30, -5, 7 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_MIN, ray_min_fn }, { OP_MAX, ray_max_fn },
    };
    for (size_t c = 0; c < 2; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        /* ray_min_fn/ray_max_fn on an I64 vector return a lazy DAG wrapper —
         * force to a concrete scalar atom before comparing. */
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_I(got->i64, want->i64);
        ray_release(got); ray_release(want);
    }
    const agg_vtable_t* vt = agg_resolve(OP_COUNT, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    TEST_ASSERT_EQ_I(got->i64, 5);     /* 5 live rows, no nulls */
    ray_release(got); ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static ray_t* vec_f64(const double* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n; memcpy(ray_data(v), xs, (size_t)n * sizeof(double));
    return v;
}

static test_result_t test_f64_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double xs[] = { 1.5, 2.0, -3.25, 10.0 };
    ray_t* col = vec_f64(xs, 4);
    TEST_ASSERT_NOT_NULL(col);
    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_SUM, ray_sum_fn }, { OP_MIN, ray_min_fn },
        { OP_MAX, ray_max_fn }, { OP_AVG, ray_avg_fn },
    };
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-12);
        ray_release(got); ray_release(want);
    }
    ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_nulls_match_reduction(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 live values, 2 sentinels; mark the column HAS_NULLS so the current
     * reductions take their null-skipping path. */
    int64_t xs[] = { 10, NULL_I64, 20, NULL_I64, 30 };
    ray_t* col = vec_i64(xs, 5);
    TEST_ASSERT_NOT_NULL(col);
    col->attrs |= RAY_ATTR_HAS_NULLS;

    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_SUM, ray_sum_fn }, { OP_MIN, ray_min_fn }, { OP_MAX, ray_max_fn },
    };
    for (size_t c = 0; c < 3; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_I(got->i64, want->i64);   /* sentinels skipped identically */
        ray_release(got); ray_release(want);
    }
    /* COUNT counts all input rows, including nulls, on every route. */
    const agg_vtable_t* vt = agg_resolve(OP_COUNT, RAY_I64);
    TEST_ASSERT_NOT_NULL(vt);
    ray_t* got = run_single_group(vt, col);
    TEST_ASSERT_EQ_I(got->i64, 5);
    ray_release(got); ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Run a BINARY vtable accumulator over two columns as a single group (gid 0). */
static ray_t* run_single_group_bin(const agg_vtable_t* vt, ray_t* x, ray_t* y) {
    int64_t len = x->len;
    void* state = calloc(1, vt->state_size);
    vt->init(state);
    uint32_t* gids = calloc((size_t)len, sizeof(uint32_t));  /* all zero */
    ray_valid_t vx = { ray_data(x), x->type, (x->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    ray_valid_t vy = { ray_data(y), y->type, (y->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch2(state, vt->state_size, gids,
                      ray_data(x), ray_data(y), &vx, &vy, len, NULL);
    ray_t* out = vt->finalize(state, NULL, 0);
    free(gids); free(state);
    return out;
}

static test_result_t test_pearson_signed_r(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const agg_vtable_t* vt = agg_resolve(OP_PEARSON_CORR, RAY_F64);
    TEST_ASSERT_NOT_NULL(vt);

    /* Perfect positive correlation → r = 1.0. */
    {
        double xs[] = { 1, 2, 3, 4 }, ys[] = { 2, 4, 6, 8 };
        ray_t* x = vec_f64(xs, 4); ray_t* y = vec_f64(ys, 4);
        ray_t* got = run_single_group_bin(vt, x, y);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_EQ_F(got->f64, 1.0, 1e-6);
        ray_release(got); ray_release(x); ray_release(y);
    }
    /* Perfect anti-correlation → r = -1.0 (confirms SIGNED r, not r²). */
    {
        double xs[] = { 1, 2, 3, 4 }, ys[] = { 4, 3, 2, 1 };
        ray_t* x = vec_f64(xs, 4); ray_t* y = vec_f64(ys, 4);
        ray_t* got = run_single_group_bin(vt, x, y);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_EQ_F(got->f64, -1.0, 1e-6);
        ray_release(got); ray_release(x); ray_release(y);
    }
    /* Mixed: x={1,2,3,4,5}, y={2,1,4,3,5}.
     * sx=15,sy=15,sxx=55,syy=55,sxy=2+2+12+12+25=53,n=5.
     * num=5*53-225=40, dx=5*55-225=50, dy=50, r=40/sqrt(2500)=40/50=0.8. */
    {
        double xs[] = { 1, 2, 3, 4, 5 }, ys[] = { 2, 1, 4, 3, 5 };
        ray_t* x = vec_f64(xs, 5); ray_t* y = vec_f64(ys, 5);
        ray_t* got = run_single_group_bin(vt, x, y);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_EQ_F(got->f64, 0.8, 1e-6);
        ray_release(got); ray_release(x); ray_release(y);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_truth_reducers_v2(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const agg_vtable_t* all_vt = agg_resolve(OP_ALL, RAY_I64);
    const agg_vtable_t* any_vt = agg_resolve(OP_ANY, RAY_I64);
    TEST_ASSERT_NOT_NULL(all_vt);
    TEST_ASSERT_NOT_NULL(any_vt);

    {
        int64_t xs[] = { 1, NULL_I64, 2 };
        ray_t* col = vec_i64(xs, 3);
        TEST_ASSERT_NOT_NULL(col);
        col->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* got = run_single_group(all_vt, col);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, -RAY_BOOL);
        TEST_ASSERT_EQ_I(got->b8, 1);
        ray_release(got); ray_release(col);
    }
    {
        int64_t xs[] = { 1, 0, 2 };
        ray_t* col = vec_i64(xs, 3);
        TEST_ASSERT_NOT_NULL(col);
        ray_t* got = run_single_group(all_vt, col);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, -RAY_BOOL);
        TEST_ASSERT_EQ_I(got->b8, 0);
        ray_release(got); ray_release(col);
    }
    {
        int64_t xs[] = { 0, NULL_I64, 0 };
        ray_t* col = vec_i64(xs, 3);
        TEST_ASSERT_NOT_NULL(col);
        col->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* got = run_single_group(any_vt, col);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, -RAY_BOOL);
        TEST_ASSERT_EQ_I(got->b8, 0);
        ray_release(got); ray_release(col);
    }
    {
        int64_t xs[] = { 0, NULL_I64, 2 };
        ray_t* col = vec_i64(xs, 3);
        TEST_ASSERT_NOT_NULL(col);
        col->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* got = run_single_group(any_vt, col);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, -RAY_BOOL);
        TEST_ASSERT_EQ_I(got->b8, 1);
        ray_release(got); ray_release(col);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_pairwise_stats_v2(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double xs[] = { 1, 3, 2, 4 };
    double ys[] = { 2, 6, 4, 8 };
    ray_t* x = vec_f64(xs, 4);
    ray_t* y = vec_f64(ys, 4);
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_NOT_NULL(y);

    struct { uint16_t op; double want; } cases[] = {
        { OP_COV, 2.5 },
        { OP_SCOV, 10.0 / 3.0 },
        { OP_WSUM, 60.0 },
        { OP_WAVG, 6.0 },
    };
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group_bin(vt, x, y);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, -RAY_F64);
        TEST_ASSERT_EQ_F(got->f64, cases[c].want, 1e-9);
        ray_release(got);
    }

    ray_release(x); ray_release(y);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_variance_matches_oracle(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I64 col: pop var=4, pop stddev=2, sample var=32/7, sample stddev=sqrt(32/7). */
    int64_t xs[] = { 2, 4, 4, 4, 5, 5, 7, 9 };
    ray_t* col = vec_i64(xs, 8);
    TEST_ASSERT_NOT_NULL(col);
    struct { uint16_t op; ray_t* (*ref)(ray_t*); } cases[] = {
        { OP_VAR, ray_var_fn }, { OP_VAR_POP, ray_var_pop_fn },
        { OP_STDDEV, ray_stddev_fn }, { OP_STDDEV_POP, ray_stddev_pop_fn },
    };
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(cases[c].ref(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-9);   /* v2 ≡ old oracle */
        ray_release(got); ray_release(want);
    }
    ray_release(col);

    /* F64 case: differential against the same oracles. */
    double fs[] = { 1.5, 2.0, -3.25, 10.0, 4.0 };
    ray_t* fcol = vec_f64(fs, 5);
    TEST_ASSERT_NOT_NULL(fcol);
    for (size_t c = 0; c < 4; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, fcol);
        ray_t* want = ray_lazy_materialize(cases[c].ref(fcol));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-9);
        ray_release(got); ray_release(want);
    }
    ray_release(fcol);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_median(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I64 odd, I64 even, F64 — differential vs ray_med_fn (F64 output). */
    {
        int64_t xs[] = { 1, 3, 2, 5, 4 };  /* odd → 3.0 */
        ray_t* col = vec_i64(xs, 5);
        const agg_vtable_t* vt = agg_resolve(OP_MEDIAN, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(ray_med_fn(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, 3.0, 1e-9);
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-9);
        ray_release(got); ray_release(want); ray_release(col);
    }
    {
        int64_t xs[] = { 1, 2, 3, 4 };  /* even → 2.5 */
        ray_t* col = vec_i64(xs, 4);
        const agg_vtable_t* vt = agg_resolve(OP_MEDIAN, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(ray_med_fn(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, 2.5, 1e-9);
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-9);
        ray_release(got); ray_release(want); ray_release(col);
    }
    {
        double fs[] = { 1.5, 2.5, 0.5 };  /* → 1.5 */
        ray_t* col = vec_f64(fs, 3);
        const agg_vtable_t* vt = agg_resolve(OP_MEDIAN, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* got = run_single_group(vt, col);
        ray_t* want = ray_lazy_materialize(ray_med_fn(col));
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        TEST_ASSERT_EQ_F(got->f64, 1.5, 1e-9);
        TEST_ASSERT_EQ_F(got->f64, want->f64, 1e-9);
        ray_release(got); ray_release(want); ray_release(col);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Run a vtable accumulator over a vector as a single group (gid 0), passing K
 * to finalize.  For LIST-returning buffered accumulators (top_n/bot_n). */
static ray_t* run_single_group_k(const agg_vtable_t* vt, ray_t* col, int64_t k) {
    void* state = calloc(1, vt->state_size);
    vt->init(state);
    uint32_t* gids = calloc((size_t)col->len, sizeof(uint32_t));  /* all zero */
    ray_valid_t valid = { ray_data(col), col->type,
                          (col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch(state, vt->state_size, gids, ray_data(col), &valid, col->len, NULL);
    ray_t* out = vt->finalize(state, NULL, k);
    if (vt->destroy) vt->destroy(state);  /* buffered accumulators own heap state */
    free(gids); free(state);
    return out;
}

static test_result_t test_topn_botn_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t xs[] = { 30, 10, 20, 40 };
    ray_t* col = vec_i64(xs, 4);
    TEST_ASSERT_NOT_NULL(col);

    struct { uint16_t op; uint8_t desc; int64_t k; int64_t expect_len; } cases[] = {
        { OP_TOP_N, 1, 2,  2 },   /* [40 30] */
        { OP_BOT_N, 0, 2,  2 },   /* [10 20] */
        { OP_TOP_N, 1, 10, 4 },   /* K>len → [40 30 20 10] */
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        TEST_ASSERT_EQ_I(vt->out_type, RAY_LIST);
        ray_t* got = run_single_group_k(vt, col, cases[c].k);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, RAY_I64);
        TEST_ASSERT_EQ_I(got->len, cases[c].expect_len);
        /* Differential vs topk_take_vec on a fresh copy. The K>=len path returns
         * a LAZY handle over `copy`; materialize consumes it (releasing copy), so
         * we don't release copy separately in that case. */
        ray_t* copy = vec_i64(xs, 4);
        ray_t* want = topk_take_vec(copy, cases[c].k, cases[c].desc);
        TEST_ASSERT_NOT_NULL(want);
        TEST_ASSERT_FALSE(RAY_IS_ERR(want));
        bool want_was_lazy = ray_is_lazy(want);
        if (want_was_lazy) want = ray_lazy_materialize(want);
        TEST_ASSERT_EQ_I(got->len, want->len);
        const int64_t* g = ray_data(got);
        const int64_t* w = ray_data(want);
        for (int64_t i = 0; i < got->len; i++)
            TEST_ASSERT_EQ_I(g[i], w[i]);
        ray_release(got); ray_release(want);
        if (!want_was_lazy) ray_release(copy);
    }
    ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_topn_botn_f64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double fs[] = { 3.5, 1.5, 2.5, 4.5 };
    ray_t* col = vec_f64(fs, 4);
    TEST_ASSERT_NOT_NULL(col);

    /* TOP_N K=2 → [4.5 3.5] ; BOT_N K=2 → [1.5 2.5]. */
    struct { uint16_t op; uint8_t desc; int64_t k; } cases[] = {
        { OP_TOP_N, 1, 2 }, { OP_BOT_N, 0, 2 },
    };
    for (size_t c = 0; c < 2; c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_F64);
        TEST_ASSERT_NOT_NULL(vt);
        TEST_ASSERT_EQ_I(vt->out_type, RAY_LIST);
        ray_t* got = run_single_group_k(vt, col, cases[c].k);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_FALSE(RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, RAY_F64);
        TEST_ASSERT_EQ_I(got->len, 2);
        ray_t* copy = vec_f64(fs, 4);
        ray_t* want = topk_take_vec(copy, cases[c].k, cases[c].desc);
        TEST_ASSERT_NOT_NULL(want);
        bool want_was_lazy = ray_is_lazy(want);
        if (want_was_lazy) want = ray_lazy_materialize(want);
        TEST_ASSERT_EQ_I(got->len, want->len);
        const double* g = ray_data(got);
        const double* w = ray_data(want);
        for (int64_t i = 0; i < got->len; i++)
            TEST_ASSERT_EQ_F(g[i], w[i], 1e-12);
        ray_release(got); ray_release(want);
        if (!want_was_lazy) ray_release(copy);
    }
    ray_release(col);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t agg_registry_entries[] = {
    { "agg_registry/sum_i64_matches_reduction", test_sum_i64_matches_reduction, NULL, NULL },
    { "agg_registry/minmax_count_i64_match", test_minmax_count_i64_match, NULL, NULL },
    { "agg_registry/f64_match", test_f64_match, NULL, NULL },
    { "agg_registry/nulls_match_reduction", test_nulls_match_reduction, NULL, NULL },
    { "agg_registry/variance_matches_oracle", test_variance_matches_oracle, NULL, NULL },
    { "agg_registry/pearson_signed_r", test_pearson_signed_r, NULL, NULL },
    { "agg_registry/truth_reducers_v2", test_truth_reducers_v2, NULL, NULL },
    { "agg_registry/pairwise_stats_v2", test_pairwise_stats_v2, NULL, NULL },
    { "agg_registry/median", test_median, NULL, NULL },
    { "agg_registry/topn_botn_i64", test_topn_botn_i64, NULL, NULL },
    { "agg_registry/topn_botn_f64", test_topn_botn_f64, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

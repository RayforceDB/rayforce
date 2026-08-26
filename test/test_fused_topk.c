/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * test_fused_topk.c — coverage for src/ops/fused_topk.c.
 *
 * Tests target:
 *   Chunk 9  — fpk_cmp multi-key + DESC + nulls + SYM-string compare
 *              (fused_topk.c:122-187)
 *   Chunk 10 — fpk_par_fn heap-reject-fast path (fused_topk.c:232-236)
 *   Chunk 11 — ray_fused_topk_select gate rejections (fused_topk.c:266-298)
 *
 * The C-level path calls ray_fused_topk_select directly with a parsed
 * where_expr (constructed via ray_parse).  ray_parse requires a runtime
 * (parser maintains the symbol table); each test uses a runtime
 * setup/teardown pair so symbols are interned consistently.
 *
 * Per the brief, every fused-path test includes a non-trivial WHERE
 * (planner gate also requires `where:` to route into fused_topk).
 */

#include "test.h"
#include <rayforce.h>
#include "lang/parse.h"
#include "lang/eval.h"
#include "ops/fused_topk.h"
#include "table/sym.h"
#include <string.h>

/* __RUNTIME is internal test plumbing; runtime API declarations come from
 * <rayforce.h>. */
extern ray_runtime_t* __RUNTIME;

/* ─── Setup / Teardown ─────────────────────────────────────────────── */

static void topk_setup(void)    { ray_runtime_create(0, NULL); }
static void topk_teardown(void) { ray_runtime_destroy(__RUNTIME); }

/* ─── Helpers ──────────────────────────────────────────────────────── */

/* Build an I64-keyed table with N rows: g[i] = i % 4, v[i] = i. */
static ray_t* make_i64_table(int64_t N) {
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    int64_t* vd = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { gd[i] = i % 4; vd[i] = i; }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);
    return tbl;
}

/* Build a multi-column table with diverse types for testing key/output
 * combinations.  Returns table with columns:
 *   id   I64  : 0..N-1
 *   k    I64  : (i * 7) % 100  -- non-monotone numeric key
 *   k2   I32  : i % 5
 *   sel  I64  : i               -- predicate column
 */
static ray_t* make_multi_col_table(int64_t N) {
    ray_t* idc = ray_vec_new(RAY_I64, N); idc->len = N;
    ray_t* kc  = ray_vec_new(RAY_I64, N); kc->len  = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int64_t* idd = (int64_t*)ray_data(idc);
    int64_t* kd  = (int64_t*)ray_data(kc);
    int32_t* k2d = (int32_t*)ray_data(k2c);
    int64_t* sd  = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        idd[i] = i;
        kd[i]  = (i * 7) % 100;
        k2d[i] = (int32_t)(i % 5);
        sd[i]  = i;
    }
    int64_t s_id  = ray_sym_intern("id",  2);
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_k2  = ray_sym_intern("k2",  2);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, s_id,  idc); ray_release(idc);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_k2,  k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_sel, sc);  ray_release(sc);
    return tbl;
}

/* ─── Tests ────────────────────────────────────────────────────────── */

/* Chunk 10: heap-reject-fast path.  With N=10000 rows and K=8, after
 * the first K rows fill the heap the next ~9992 rows must each compare
 * against heap[0] (the current worst).  ASC order on a random-ish key
 * makes most subsequent rows worse than the heap root, so the
 * `fpk_cmp(...) >= 0` branch (line 233) fires repeatedly. */
static test_result_t test_topk_heap_reject_fast(void) {
    int64_t N = 10000;
    ray_t* tbl = make_multi_col_table(N);
    TEST_ASSERT_NOT_NULL(tbl);

    /* WHERE sel >= 0 — non-trivial WHERE that admits all rows. */
    ray_t* where_expr = ray_parse("(>= sel 0)");
    TEST_ASSERT_NOT_NULL(where_expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(where_expr));

    int64_t s_k  = ray_sym_intern("k",  1);
    int64_t s_id = ray_sym_intern("id", 2);
    int64_t sort_keys[1] = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[2] = { s_id, s_k };
    int64_t k_pick = 8;

    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* Result rows should have the K smallest k values; since k = (i*7)%100,
     * the smallest k=0 occurs at rows i where 7i ≡ 0 mod 100 → i ∈ {0,
     * 100, 200, ...}.  K=8 with stable tie-break (lower id first) → top
     * 8 should all have k=0 with ids = {0, 100, 200, 300, 400, 500, 600, 700}. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    for (int64_t i = 0; i < k_pick; i++) {
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k_col))[i], 0);
    }

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Chunk 9 part 1: multi-key + DESC.  Two sort keys (k2 ASC, k DESC) —
 * ties on first key broken by second key in opposite direction.  This
 * walks both `if (cmp != 0) return ks->desc ? -cmp : cmp;` (line 166)
 * and the multi-key `for` loop (line 124-167). */
static test_result_t test_topk_multi_key_desc(void) {
    int64_t N = 200;
    ray_t* tbl = make_multi_col_table(N);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_k2  = ray_sym_intern("k2",  2);
    int64_t s_id  = ray_sym_intern("id",  2);
    /* k2 ASC, k DESC — primary asc, secondary desc. */
    int64_t sort_keys[2]  = { s_k2, s_k };
    uint8_t sort_descs[2] = { 0, 1 };
    int64_t out_syms[3]   = { s_id, s_k, s_k2 };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 2, k_pick,
                                       out_syms, NULL, 3);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* The K smallest are at k2=0; among those, k DESC → rows with
     * largest k first.  k2=0 occurs at i where i % 5 == 0, i.e. i ∈
     * {0,5,10,15,...,195}.  Their k values = (i*7) % 100.  With i
     * stepping by 5, k = (5*7*j) % 100 = (35j) % 100 for j=0..39.
     * Largest k value among 40 entries: j=11 → 35*11 % 100 = 85.
     * j=27 → 945%100 = 45.  j=15 → 525%100 = 25.  Compute the max. */
    ray_t* k2_col = ray_table_get_col(res, s_k2);
    ray_t* k_col  = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k2_col);
    TEST_ASSERT_NOT_NULL(k_col);
    /* All result rows must have k2 == 0. */
    for (int64_t i = 0; i < k_pick; i++) {
        TEST_ASSERT_EQ_I((int64_t)((int32_t*)ray_data(k2_col))[i], 0);
    }
    /* k values must be in non-increasing order (DESC). */
    int64_t prev = INT64_MAX;
    for (int64_t i = 0; i < k_pick; i++) {
        int64_t cur = ((int64_t*)ray_data(k_col))[i];
        TEST_ASSERT_TRUE(cur <= prev);
        prev = cur;
    }

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Chunk 9 part 2: SYM key — exercises the SYM branch of fpk_cmp
 * (lines 140-148) including ray_str_cmp.  Use SYM_W32 width per
 * brief constraint. */
static test_result_t test_topk_sym_key(void) {
    int64_t N = 100;
    /* SYM column with 5 distinct symbols spelled differently. */
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, N);
    sc->len = N;
    int64_t s_alpha = ray_sym_intern("alpha", 5);
    int64_t s_beta  = ray_sym_intern("beta",  4);
    int64_t s_gamma = ray_sym_intern("gamma", 5);
    int64_t s_delta = ray_sym_intern("delta", 5);
    int64_t s_epsi  = ray_sym_intern("epsilon", 7);
    int64_t syms[5] = { s_alpha, s_beta, s_gamma, s_delta, s_epsi };
    int32_t* d = (int32_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) d[i] = (int32_t)syms[i % 5];

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_name = ray_sym_intern("name", 4);
    int64_t s_sel  = ray_sym_intern("sel",  3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_name, sc);   ray_release(sc);
    tbl = ray_table_add_col(tbl, s_sel,  selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_name };
    uint8_t sort_descs[1] = { 0 };  /* ASC by name string */
    int64_t out_syms[1]   = { s_name };
    int64_t k_pick = 4;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* Top 4 ASC by string: "alpha", "beta", "delta", "epsilon" —
     * "alpha" first (with 20 ties broken by source row index). */
    ray_t* name_col = ray_table_get_col(res, s_name);
    TEST_ASSERT_NOT_NULL(name_col);
    /* Lexicographic order: alpha < beta < delta < epsilon < gamma.
     * The K=4 smallest ALL fit "alpha" in the first slot since alpha
     * has 20 occurrences in the table (rows 0,5,10,...,95).  The
     * next 3 should be 4 more "alpha" rows — actually all top 4 must
     * be "alpha" because there are 20 alpha rows. */
    int32_t got = ((int32_t*)ray_data(name_col))[0];
    TEST_ASSERT_EQ_I((int64_t)got, s_alpha);

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Chunk 9 part 3: nulls in sort key — exercises the has_nulls leg.
 * A null is the smallest value: ASC puts nulls FIRST, DESC puts them LAST. */
static test_result_t test_topk_null_sort_key_asc(void) {
    int64_t N = 20;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* kd = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) kd[i] = i;
    /* Mark rows {0, 5, 10} as null. */
    ray_vec_set_null(kc, 0, true);
    ray_vec_set_null(kc, 5, true);
    ray_vec_set_null(kc, 10, true);

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);   ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC, NULLS LAST */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* ASC, NULLS FIRST: the three null rows are the three smallest, in
     * source order, and the two smallest non-nulls follow: 1, 2. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    for (int64_t i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(ray_vec_is_null(k_col, i));
    int64_t expected[2] = {1, 2};
    for (int64_t i = 0; i < 2; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(k_col, 3 + i));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k_col))[3 + i], expected[i]);
    }

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_null_sort_key_desc(void) {
    int64_t N = 20;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* kd = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) kd[i] = i;
    ray_vec_set_null(kc, 0, true);
    ray_vec_set_null(kc, 5, true);
    ray_vec_set_null(kc, 10, true);

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);   ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 1 };  /* DESC, so NULLS LAST */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* DESC, NULLS LAST: a null is the smallest value, so it never enters a
     * DESC top-5 while five larger values exist.  All five are the highest
     * non-nulls: 19, 18, 17, 16, 15. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    for (int64_t i = 0; i < 5; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(k_col, i));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k_col))[i], 19 - i);
    }

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* DESC top-k that has to reach past the non-nulls.  With nulls last, a
 * null enters the result only when there are fewer than k non-null rows —
 * the back-fill leg of the bounded heap, which no other test exercises:
 * every DESC case above has more non-nulls than k, so the null branch is
 * only ever taken to REJECT. */
static test_result_t test_topk_null_desc_backfill(void) {
    int64_t N = 20;
    ray_t* kc = ray_vec_new(RAY_I64, N); kc->len = N;
    int64_t* kd = (int64_t*)ray_data(kc);
    for (int64_t i = 0; i < N; i++) kd[i] = i;
    /* Only rows 3, 11 and 17 are non-null: 3 values for a k of 5. */
    for (int64_t i = 0; i < N; i++)
        if (i != 3 && i != 11 && i != 17) ray_vec_set_null(kc, i, true);

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 1 };  /* DESC, so NULLS LAST */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    /* 17, 11, 3, then two nulls in source order. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    int64_t expect[3] = {17, 11, 3};
    for (int64_t i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(k_col, i));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k_col))[i], expect[i]);
    }
    TEST_ASSERT_TRUE(ray_vec_is_null(k_col, 3));
    TEST_ASSERT_TRUE(ray_vec_is_null(k_col, 4));

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Chunk 11: gate rejections.  Each call below must return NULL because
 * the inputs fail one of the runtime gates in ray_fused_topk_select.
 * The C-API shape lets us hit gates the planner already filters out
 * (where ray-level callers never arrive). */

static test_result_t test_topk_gate_null_tbl(void) {
    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { 1 };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { 1 };
    /* Pass NULL table → first-line gate. */
    ray_t* res = ray_fused_topk_select(NULL, where_expr,
                                       sort_keys, sort_descs, 1, 5,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr);
    PASS();
}

static test_result_t test_topk_gate_k_too_large(void) {
    int64_t N = 100;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    /* k > FPK_MAX_K (8192). */
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 9000,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_zero_sort_keys(void) {
    int64_t N = 100;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    /* n_sort_keys == 0. */
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 0, 5,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_k_ge_nrows(void) {
    int64_t N = 5;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    /* k (10) >= nrows (5). */
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 10,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gathers_f64_out_col_type(void) {
    /* Output columns use the shared gather helper, so F64 projections
     * are safe even though the sort key still uses the fixed-width
     * comparator gate. */
    int64_t N = 50;
    ray_t* fc = ray_vec_new(RAY_F64, N); fc->len = N;
    double* fd = (double*)ray_data(fc);
    for (int64_t i = 0; i < N; i++) fd[i] = (double)i;
    ray_t* sc = ray_vec_new(RAY_I64, N); sc->len = N;
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;
    int64_t s_f   = ray_sym_intern("f",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_f,   fc); ray_release(fc);
    tbl = ray_table_add_col(tbl, s_sel, sc); ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_sel };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_sel, s_f };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 5,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 5);
    ray_t* out_f = ray_table_get_col(res, s_f);
    TEST_ASSERT_NOT_NULL(out_f);
    TEST_ASSERT_EQ_I(out_f->type, RAY_F64);
    double* out_fd = (double*)ray_data(out_f);
    TEST_ASSERT_FMT(out_fd[0] == 0.0, "expected first F64 output to be 0");
    TEST_ASSERT_FMT(out_fd[4] == 4.0, "expected fifth F64 output to be 4");

    ray_release(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_unsupported_sort_key_type(void) {
    /* F64 sort key → rejected at the sort-key gate. */
    int64_t N = 50;
    ray_t* fc = ray_vec_new(RAY_F64, N); fc->len = N;
    double* fd = (double*)ray_data(fc);
    for (int64_t i = 0; i < N; i++) fd[i] = (double)i;
    ray_t* sc = ray_vec_new(RAY_I64, N); sc->len = N;
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;
    int64_t s_f   = ray_sym_intern("f",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_f,   fc); ray_release(fc);
    tbl = ray_table_add_col(tbl, s_sel, sc); ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_f };  /* F64 → reject */
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_sel };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 5,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);

    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_n_out_zero(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    /* n_out == 0 — first-line gate. */
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 5,
                                       NULL, NULL, 0);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_too_many_sort_keys(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    /* FPK_MAX_KEYS == 16; pass 17 keys to trip the gate. */
    int64_t sort_keys[17];
    uint8_t sort_descs[17];
    for (int i = 0; i < 17; i++) { sort_keys[i] = s_v; sort_descs[i] = 0; }
    int64_t out_syms[1] = { s_v };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 17, 5,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_gate_negative_k(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    /* k <= 0 first-line gate. */
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, -3,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    /* k = 0 also rejected. */
    res = ray_fused_topk_select(tbl, where_expr,
                                sort_keys, sort_descs, 1, 0,
                                out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* End-to-end fused topk select with simple I64 sort key.  Ensures the
 * happy path through fpk_par_fn → final merge → materialize works for
 * the smallest non-trivial input.  Also exercises the n_out=multi
 * gather path (lines 412-440) including ray_vec_set_null short-circuit
 * (no nulls in source). */
static test_result_t test_topk_basic_i64_asc(void) {
    int64_t N = 100;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_v, s_g };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Smallest 3: v = 0, 1, 2. */
    ray_t* v_col = ray_table_get_col(res, s_v);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[i], i);

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

static test_result_t test_topk_in_pred(void) {
    int64_t N = 100;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(in g [1 3])");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_v, s_g };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 4,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);
    ray_t* v_col = ray_table_get_col(res, s_v);
    ray_t* g_col = ray_table_get_col(res, s_g);
    TEST_ASSERT_NOT_NULL(v_col);
    TEST_ASSERT_NOT_NULL(g_col);
    int64_t expect_v[] = {1, 3, 5, 7};
    int64_t expect_g[] = {1, 3, 1, 3};
    for (int64_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[i], expect_v[i]);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(g_col))[i], expect_g[i]);
    }

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Aliased output columns — alias array non-NULL.  Exercises line 414
 * `int64_t alias = out_alias_syms ? out_alias_syms[c] : cs;`. */
static test_result_t test_topk_aliased_out(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t s_alias = ray_sym_intern("vv", 2);
    int64_t sort_keys[1]   = { s_v };
    uint8_t sort_descs[1]  = { 0 };
    int64_t out_syms[1]    = { s_v };
    int64_t out_aliases[1] = { s_alias };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 3,
                                       out_syms, out_aliases, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Result column must be published under the alias name. */
    ray_t* col = ray_table_get_col(res, s_alias);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_I(col->len, 3);

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Source nulls propagated to result.  Exercises lines 432-437. */
static test_result_t test_topk_propagates_nulls(void) {
    int64_t N = 20;
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    int64_t* vd = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { gd[i] = i % 4; vd[i] = i; }
    /* Mark some g rows as null — these will be carried into the
     * result via the null propagation block. */
    ray_vec_set_null(gc, 1, true);
    ray_vec_set_null(gc, 2, true);
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_v, s_g };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Top 5 by v ASC = rows 0..4.  g[1] and g[2] are null; result
     * positions 1 and 2 must be marked null. */
    ray_t* g_col = ray_table_get_col(res, s_g);
    TEST_ASSERT_NOT_NULL(g_col);
    TEST_ASSERT_FALSE(ray_vec_is_null(g_col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(g_col, 1));
    TEST_ASSERT_TRUE(ray_vec_is_null(g_col, 2));
    TEST_ASSERT_FALSE(ray_vec_is_null(g_col, 3));
    TEST_ASSERT_FALSE(ray_vec_is_null(g_col, 4));

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Multi-key path that crosses the 64-bit composite key boundary —
 * three I32 keys at offsets 0/32/64 (96 bits total).  Exercises the
 * inner mk-style key-resolution path in fpk_cmp via a tall enough
 * table that fpk_par_fn uses multiple workers and the global merge
 * runs.  (Note: fused_topk does not pack composite keys; it loops
 * over n_keys per compare, so this hits the multi-key for-loop in
 * fpk_cmp.) */
static test_result_t test_topk_three_keys(void) {
    int64_t N = 1000;
    ray_t* k1c = ray_vec_new(RAY_I32, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* k3c = ray_vec_new(RAY_I32, N); k3c->len = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int32_t* k1 = (int32_t*)ray_data(k1c);
    int32_t* k2 = (int32_t*)ray_data(k2c);
    int32_t* k3 = (int32_t*)ray_data(k3c);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        k1[i] = (int32_t)(i % 7);
        k2[i] = (int32_t)(i % 11);
        k3[i] = (int32_t)(i % 13);
        sd[i] = i;
    }
    int64_t s_k1  = ray_sym_intern("k1",  2);
    int64_t s_k2  = ray_sym_intern("k2",  2);
    int64_t s_k3  = ray_sym_intern("k3",  2);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, s_k1,  k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2,  k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_k3,  k3c); ray_release(k3c);
    tbl = ray_table_add_col(tbl, s_sel, sc);  ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[3]  = { s_k1, s_k2, s_k3 };
    uint8_t sort_descs[3] = { 0, 1, 0 };  /* mixed asc/desc */
    int64_t out_syms[1]   = { s_sel };
    int64_t k_pick = 10;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 3, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);

    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* ─── Branch-coverage tests ───────────────────────────────────────── */

/* fpk_cmp esz==2 leg (I16 sort key, lines 158-160). */
static test_result_t test_topk_i16_sort_key(void) {
    int64_t N = 50;
    ray_t* kc  = ray_vec_new(RAY_I16, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int16_t* kd = (int16_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        kd[i] = (int16_t)(N - 1 - i);  /* descending values */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Smallest 3 I16 values: 0, 1, 2. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(k_col))[i], i);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* fpk_cmp esz==2 leg DESC — both comparison directions exercised. */
static test_result_t test_topk_i16_sort_key_desc(void) {
    int64_t N = 50;
    ray_t* kc  = ray_vec_new(RAY_I16, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int16_t* kd = (int16_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) { kd[i] = (int16_t)i; sd[i] = i; }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 1 };  /* DESC */
    int64_t out_syms[1]   = { s_k };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Largest 3 DESC: 49, 48, 47. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(k_col))[i], (int64_t)(N - 1 - i));
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* fpk_cmp esz==1 leg (U8 sort key, lines 162-165). */
static test_result_t test_topk_u8_sort_key(void) {
    int64_t N = 50;
    ray_t* kc  = ray_vec_new(RAY_U8, N);  kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    uint8_t* kd = (uint8_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        kd[i] = (uint8_t)((i * 7) % 50);  /* scattered values */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 4;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Verify ASC order on output. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_NOT_NULL(k_col);
    uint8_t prev = 0;
    for (int64_t i = 0; i < k_pick; i++) {
        uint8_t cur = ((uint8_t*)ray_data(k_col))[i];
        TEST_ASSERT_TRUE(cur >= prev);
        prev = cur;
    }
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* fpk_cmp esz==1 leg DESC (U8 sort key). */
static test_result_t test_topk_u8_sort_key_desc(void) {
    int64_t N = 30;
    ray_t* kc  = ray_vec_new(RAY_U8, N);  kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    uint8_t* kd = (uint8_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) { kd[i] = (uint8_t)i; sd[i] = i; }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 1 };  /* DESC */
    int64_t out_syms[1]   = { s_k };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Largest 3 DESC: 29, 28, 27. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I((int64_t)((uint8_t*)ray_data(k_col))[i], (int64_t)(N - 1 - i));
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* BOOL sort key — type gate acceptance + esz==1 unsigned compare path.
 * BOOL column values are 0/1 stored as uint8_t. */
static test_result_t test_topk_bool_sort_key(void) {
    int64_t N = 20;
    ray_t* kc  = ray_vec_new(RAY_BOOL, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N);  sc->len  = N;
    uint8_t* kd = (uint8_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        kd[i] = (uint8_t)(i % 2);  /* alternating 0/1 */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC: false (0) before true (1) */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 4;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* ASC: first K=4 are the false values (k==0), rows 0,2,4,6. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I((int64_t)((uint8_t*)ray_data(k_col))[i], 0);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* DATE sort key — temporal type gate (esz==4 via I32 path). */
static test_result_t test_topk_date_sort_key(void) {
    int64_t N = 30;
    ray_t* kc  = ray_vec_new(RAY_DATE, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N);  sc->len  = N;
    int32_t* kd = (int32_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    /* DATE stored as days-since-epoch (I32). Use distinct descending. */
    for (int64_t i = 0; i < N; i++) {
        kd[i] = (int32_t)(20000 - (int32_t)i);  /* large to small */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Smallest 3 date values: 19971, 19972, 19973. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    TEST_ASSERT_EQ_I((int64_t)((int32_t*)ray_data(k_col))[0], 20000 - (N - 1));
    TEST_ASSERT_EQ_I((int64_t)((int32_t*)ray_data(k_col))[1], 20000 - (N - 2));
    TEST_ASSERT_EQ_I((int64_t)((int32_t*)ray_data(k_col))[2], 20000 - (N - 3));
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* TIME sort key — temporal type gate (esz==4 via I32 path). */
static test_result_t test_topk_time_sort_key(void) {
    int64_t N = 20;
    ray_t* kc  = ray_vec_new(RAY_TIME, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N);  sc->len  = N;
    int32_t* kd = (int32_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        kd[i] = (int32_t)((i * 3600) % 86400);  /* various times */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 1 };  /* DESC */
    int64_t out_syms[1]   = { s_k };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* DESC: largest time values first. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    int32_t prev = INT32_MAX;
    for (int64_t i = 0; i < k_pick; i++) {
        int32_t cur = ((int32_t*)ray_data(k_col))[i];
        TEST_ASSERT_TRUE(cur <= prev);
        prev = cur;
    }
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* TIMESTAMP sort key — temporal type gate (esz==8 via I64 path). */
static test_result_t test_topk_timestamp_sort_key(void) {
    int64_t N = 30;
    ray_t* kc  = ray_vec_new(RAY_TIMESTAMP, N); kc->len = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N);       sc->len = N;
    int64_t* kd = (int64_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        kd[i] = 1700000000000LL + (int64_t)(i * 86400000LL);  /* millis */
        sd[i] = i;
    }
    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Smallest 3 timestamps: rows 0,1,2. */
    ray_t* k_col = ray_table_get_col(res, s_k);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k_col))[i],
                         1700000000000LL + i * 86400000LL);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* SYM sort key DESC — exercises the `ks->desc ? -cmp : cmp` path with
 * cmp from ray_str_cmp on SYM strings in the opposite direction. */
static test_result_t test_topk_sym_key_desc(void) {
    int64_t N = 50;
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W32, N);
    sc->len = N;
    int64_t s_alpha = ray_sym_intern("alpha", 5);
    int64_t s_beta  = ray_sym_intern("beta",  4);
    int64_t s_gamma = ray_sym_intern("gamma", 5);
    int64_t s_delta = ray_sym_intern("delta", 5);
    int64_t s_epsi  = ray_sym_intern("epsilon", 7);
    int64_t syms[5] = { s_alpha, s_beta, s_gamma, s_delta, s_epsi };
    int32_t* d = (int32_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) d[i] = (int32_t)syms[i % 5];

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_name = ray_sym_intern("name", 4);
    int64_t s_sel  = ray_sym_intern("sel",  3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_name, sc);   ray_release(sc);
    tbl = ray_table_add_col(tbl, s_sel,  selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_name };
    uint8_t sort_descs[1] = { 1 };  /* DESC by name string */
    int64_t out_syms[1]   = { s_name };
    int64_t k_pick = 4;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* DESC: gamma > epsilon > delta > beta > alpha.
     * Top 4 DESC = gamma(×10), but K=4 so first 4 gamma rows. */
    ray_t* name_col = ray_table_get_col(res, s_name);
    TEST_ASSERT_NOT_NULL(name_col);
    int32_t got = ((int32_t*)ray_data(name_col))[0];
    TEST_ASSERT_EQ_I((int64_t)got, s_gamma);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Both-null tie on a sort key — line 137 `a_null && b_null` continue,
 * then break tie on second key or row index. */
static test_result_t test_topk_both_nulls_tie(void) {
    int64_t N = 20;
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I64, N); k2c->len = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int64_t* k1d = (int64_t*)ray_data(k1c);
    int64_t* k2d = (int64_t*)ray_data(k2c);
    int64_t* sd  = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        k1d[i] = i / 5;   /* 0,0,0,0,0, 1,1,1,1,1, 2,2,2,2,2, 3,3,3,3,3 */
        k2d[i] = N - i;   /* tie-breaker: descending */
        sd[i]  = i;
    }
    /* Mark several rows null on k1 — including pairs so both-null
     * branches fire during comparison. */
    ray_vec_set_null(k1c, 0, true);
    ray_vec_set_null(k1c, 1, true);
    ray_vec_set_null(k1c, 5, true);
    ray_vec_set_null(k1c, 10, true);

    int64_t s_k1  = ray_sym_intern("k1",  2);
    int64_t s_k2  = ray_sym_intern("k2",  2);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1,  k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2,  k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_sel, sc);  ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    /* Sort by k1 ASC (a null is smallest, so nulls lead), then k2 ASC. */
    int64_t sort_keys[2]  = { s_k1, s_k2 };
    uint8_t sort_descs[2] = { 0, 0 };
    int64_t out_syms[2]   = { s_k1, s_k2 };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 2, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* (k1 ASC, k2 ASC).  The four k1-nulls are the smallest and lead; two
     * nulls tie on k1, so k2 orders them: rows 10(k2=10), 5(k2=15),
     * 1(k2=19), 0(k2=20).  The fifth slot is the smallest k2 of the
     * smallest non-null k1 group (k1=0: rows 2,3,4) — row 4, k2=16. */
    ray_t* k2_col = ray_table_get_col(res, s_k2);
    TEST_ASSERT_NOT_NULL(k2_col);
    int64_t expect_k2[] = {10, 15, 19, 20, 16};
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(k2_col))[i], expect_k2[i]);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Selective WHERE filtering most rows.  K close to the number of
 * surviving rows.  Exercises !bits[r] branch heavily and the case
 * where fewer rows pass than K (global_n < k after merge). */
static test_result_t test_topk_selective_where(void) {
    int64_t N = 100;
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    int64_t* vd = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { gd[i] = i % 10; vd[i] = i; }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    /* WHERE g == 7 — only 10 out of 100 rows pass. */
    ray_t* where_expr = ray_parse("(== g 7)");
    TEST_ASSERT_NOT_NULL(where_expr);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_v, s_g };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Rows matching g==7: v=7,17,27,37,... Top 3 ASC = 7,17,27. */
    ray_t* v_col = ray_table_get_col(res, s_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[0], 7);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[1], 17);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[2], 27);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Fewer-than-K passing rows — the global merge's global_n stays
 * below k so the final heapify-then-sift path is not taken; instead
 * the linear accumulate path fills global_idx[0..global_n-1] and
 * fpk_sort_final sorts that short array. */
static test_result_t test_topk_fewer_than_k_passing(void) {
    int64_t N = 50;
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    int64_t* vd = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { gd[i] = i % 10; vd[i] = i; }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    /* WHERE g == 7 — only 5 rows pass. K=10 > 5 surviving. */
    ray_t* where_expr = ray_parse("(== g 7)");
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    int64_t k_pick = 10;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Only 5 rows pass, so result has 5, not 10. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 5);
    ray_t* v_col = ray_table_get_col(res, s_v);
    int64_t expect[] = {7, 17, 27, 37, 47};
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[i], expect[i]);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* Multi-key with nulls on first key AND ties — exercises both-null
 * continue (line 137) propagating to second key comparison. */
static test_result_t test_topk_multi_key_null_tiebreak(void) {
    int64_t N = 20;
    ray_t* k1c = ray_vec_new(RAY_I64, N); k1c->len = N;
    ray_t* k2c = ray_vec_new(RAY_I32, N); k2c->len = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int64_t* k1d = (int64_t*)ray_data(k1c);
    int32_t* k2d = (int32_t*)ray_data(k2c);
    int64_t* sd  = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) {
        k1d[i] = i / 4;   /* groups of 4 */
        k2d[i] = (int32_t)(i % 4);
        sd[i]  = i;
    }
    /* Null k1 at rows 0,1 (both in group k1=0) and rows 8,9 (group k1=2). */
    ray_vec_set_null(k1c, 0, true);
    ray_vec_set_null(k1c, 1, true);
    ray_vec_set_null(k1c, 8, true);
    ray_vec_set_null(k1c, 9, true);

    int64_t s_k1  = ray_sym_intern("k1",  2);
    int64_t s_k2  = ray_sym_intern("k2",  2);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1,  k1c); ray_release(k1c);
    tbl = ray_table_add_col(tbl, s_k2,  k2c); ray_release(k2c);
    tbl = ray_table_add_col(tbl, s_sel, sc);  ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    /* k1 ASC (a null is smallest, so nulls lead), k2 DESC for tie-break. */
    int64_t sort_keys[2]  = { s_k1, s_k2 };
    uint8_t sort_descs[2] = { 0, 1 };
    int64_t out_syms[3]   = { s_k1, s_k2, s_sel };
    int64_t k_pick = 6;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 2, k_pick,
                                       out_syms, NULL, 3);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* The four k1-nulls lead.  They tie on k1, so k2 DESC orders them and
     * the row index breaks the remaining ties: row 1(k2=1), row 9(k2=1),
     * row 0(k2=0), row 8(k2=0).  Then the smallest non-null k1 group
     * (k1=0, rows 2,3) by k2 DESC: row 3(k2=3), row 2(k2=2). */
    ray_t* sel_col = ray_table_get_col(res, s_sel);
    int64_t expect_sel[] = {1, 9, 0, 8, 3, 2};
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(sel_col))[i], expect_sel[i]);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* I64 DESC single key — cover the desc branch for esz==8 path (line 167
 * `ks->desc ? -cmp : cmp` with esz==8).  Existing tests use ASC for I64. */
static test_result_t test_topk_i64_desc(void) {
    int64_t N = 100;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 1 };  /* DESC */
    int64_t out_syms[1]   = { s_v };
    int64_t k_pick = 4;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Top 4 DESC: 99, 98, 97, 96. */
    ray_t* v_col = ray_table_get_col(res, s_v);
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[i], N - 1 - i);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* I16 sort key with nulls — exercises the null-aware leg (lines 134-139)
 * in combination with the esz==2 compare path. */
static test_result_t test_topk_i16_with_nulls(void) {
    int64_t N = 20;
    ray_t* kc  = ray_vec_new(RAY_I16, N); kc->len  = N;
    ray_t* sc  = ray_vec_new(RAY_I64, N); sc->len  = N;
    int16_t* kd = (int16_t*)ray_data(kc);
    int64_t* sd = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) { kd[i] = (int16_t)i; sd[i] = i; }
    ray_vec_set_null(kc, 0, true);
    ray_vec_set_null(kc, 5, true);

    int64_t s_k   = ray_sym_intern("k",   1);
    int64_t s_sel = ray_sym_intern("sel", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k,   kc);  ray_release(kc);
    tbl = ray_table_add_col(tbl, s_sel, sc);   ray_release(sc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_k };
    uint8_t sort_descs[1] = { 0 };  /* ASC, so NULLS FIRST */
    int64_t out_syms[2]   = { s_k, s_sel };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* I16 ASC: the two nulls (rows 0 and 5) lead in source order, then
     * the smallest non-nulls 1, 2, 3. */
    ray_t* k_col  = ray_table_get_col(res, s_k);
    TEST_ASSERT_TRUE(ray_vec_is_null(k_col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(k_col, 1));
    int16_t expect[] = {1, 2, 3};
    for (int64_t i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(k_col, 2 + i));
        TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(k_col))[2 + i], expect[i]);
    }
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* AND predicate — multi-clause WHERE exercises the compound-pred path
 * in fp_compile_pred through fused_topk. */
static test_result_t test_topk_and_pred(void) {
    int64_t N = 100;
    ray_t* gc = ray_vec_new(RAY_I64, N); gc->len = N;
    ray_t* vc = ray_vec_new(RAY_I64, N); vc->len = N;
    int64_t* gd = (int64_t*)ray_data(gc);
    int64_t* vd = (int64_t*)ray_data(vc);
    for (int64_t i = 0; i < N; i++) { gd[i] = i % 10; vd[i] = i; }
    int64_t s_g = ray_sym_intern("g", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_g, gc); ray_release(gc);
    tbl = ray_table_add_col(tbl, s_v, vc); ray_release(vc);

    /* WHERE g >= 5 AND g < 8 — rows where g in {5,6,7}. */
    ray_t* where_expr = ray_parse("(and (>= g 5) (< g 8))");
    TEST_ASSERT_NOT_NULL(where_expr);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[2]   = { s_v, s_g };
    int64_t k_pick = 5;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 2);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* Smallest v where g in {5,6,7}: v=5,6,7,15,16. */
    ray_t* v_col = ray_table_get_col(res, s_v);
    int64_t expect_v[] = {5, 6, 7, 15, 16};
    for (int64_t i = 0; i < k_pick; i++)
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[i], expect_v[i]);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* gate: sort key column not found in table (line 288). */
static test_result_t test_topk_gate_sort_key_missing(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v       = ray_sym_intern("v",     1);
    int64_t s_missing = ray_sym_intern("noexist", 7);
    int64_t sort_keys[1]  = { s_missing };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_v };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 3,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* gate: output column not found in table (line 275). */
static test_result_t test_topk_gate_out_col_missing(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v       = ray_sym_intern("v",     1);
    int64_t s_missing = ray_sym_intern("noexist", 7);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { s_missing };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 3,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* K=1 edge case — minimum heap size; sift_down is a no-op, but
 * the heap-reject-fast path (fpk_cmp >= 0 continue) fires on every
 * row after the first. */
static test_result_t test_topk_k_equals_one(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[1]   = { s_v };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 1,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    ray_t* v_col = ray_table_get_col(res, s_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[0], 0);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* K=1 DESC — minimum heap descending. */
static test_result_t test_topk_k_one_desc(void) {
    int64_t N = 50;
    ray_t* tbl = make_i64_table(N);
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t s_v = ray_sym_intern("v", 1);
    int64_t sort_keys[1]  = { s_v };
    uint8_t sort_descs[1] = { 1 };  /* DESC */
    int64_t out_syms[1]   = { s_v };
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, 1,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    ray_t* v_col = ray_table_get_col(res, s_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v_col))[0], N - 1);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* SYM key with W64 width — exercises read_by_esz esz=8 through SYM path
 * (line 142).  W8 is skipped because sym IDs from ray_sym_intern may
 * exceed 255 after other tests have interned symbols, making truncation
 * unavoidable.  W64 is a safe large-width alternative. */
static test_result_t test_topk_sym_key_w64(void) {
    int64_t N = 30;
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W64, N);
    sc->len = N;
    int64_t s_aa = ray_sym_intern("aa", 2);
    int64_t s_bb = ray_sym_intern("bb", 2);
    int64_t s_cc = ray_sym_intern("cc", 2);
    int64_t syms[3] = { s_aa, s_bb, s_cc };
    int64_t* d = (int64_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) d[i] = syms[i % 3];

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_name = ray_sym_intern("name", 4);
    int64_t s_sel  = ray_sym_intern("sel",  3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_name, sc);   ray_release(sc);
    tbl = ray_table_add_col(tbl, s_sel,  selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_name };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[1]   = { s_name };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* ASC: "aa" < "bb" < "cc".  With 10 "aa" rows, top 3 = all "aa". */
    ray_t* name_col = ray_table_get_col(res, s_name);
    int64_t got = ((int64_t*)ray_data(name_col))[0];
    TEST_ASSERT_EQ_I(got, s_aa);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* SYM key with W16 width — exercises read_by_esz esz=2 through SYM path. */
static test_result_t test_topk_sym_key_w16(void) {
    int64_t N = 30;
    ray_t* sc = ray_sym_vec_new(RAY_SYM_W16, N);
    sc->len = N;
    int64_t s_xx = ray_sym_intern("xx", 2);
    int64_t s_yy = ray_sym_intern("yy", 2);
    int64_t syms[2] = { s_xx, s_yy };
    uint16_t* d = (uint16_t*)ray_data(sc);
    for (int64_t i = 0; i < N; i++) d[i] = (uint16_t)syms[i % 2];

    ray_t* selc = ray_vec_new(RAY_I64, N); selc->len = N;
    int64_t* sd = (int64_t*)ray_data(selc);
    for (int64_t i = 0; i < N; i++) sd[i] = i;

    int64_t s_name = ray_sym_intern("name", 4);
    int64_t s_sel  = ray_sym_intern("sel",  3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_name, sc);   ray_release(sc);
    tbl = ray_table_add_col(tbl, s_sel,  selc); ray_release(selc);

    ray_t* where_expr = ray_parse("(>= sel 0)");
    int64_t sort_keys[1]  = { s_name };
    uint8_t sort_descs[1] = { 0 };  /* ASC */
    int64_t out_syms[1]   = { s_name };
    int64_t k_pick = 3;
    ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                       sort_keys, sort_descs, 1, k_pick,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), k_pick);
    /* ASC: "xx" < "yy".  15 "xx" rows, top 3 = all "xx". */
    ray_t* name_col = ray_table_get_col(res, s_name);
    uint16_t got = ((uint16_t*)ray_data(name_col))[0];
    TEST_ASSERT_EQ_I((int64_t)got, s_xx);
    ray_release(res); ray_release(where_expr); ray_release(tbl);
    PASS();
}

/* gate: tbl is not a table (RAY_TABLE) — passes a vec instead. */
static test_result_t test_topk_gate_not_table(void) {
    ray_t* not_a_table = ray_vec_new(RAY_I64, 10);
    not_a_table->len = 10;
    ray_t* where_expr = ray_parse("(>= v 0)");
    int64_t sort_keys[1]  = { 1 };
    uint8_t sort_descs[1] = { 0 };
    int64_t out_syms[1]   = { 1 };
    ray_t* res = ray_fused_topk_select(not_a_table, where_expr,
                                       sort_keys, sort_descs, 1, 5,
                                       out_syms, NULL, 1);
    TEST_ASSERT_NULL(res);
    ray_release(where_expr); ray_release(not_a_table);
    PASS();
}

/* ─── Entry table ──────────────────────────────────────────────────── */

const test_entry_t fused_topk_entries[] = {
    /* Chunk 10 — heap-reject-fast path */
    { "fused_topk/heap_reject_fast",          test_topk_heap_reject_fast,          topk_setup, topk_teardown },
    /* Chunk 9 — multi-key compare paths */
    { "fused_topk/multi_key_desc",            test_topk_multi_key_desc,            topk_setup, topk_teardown },
    { "fused_topk/sym_key_w32",               test_topk_sym_key,                   topk_setup, topk_teardown },
    { "fused_topk/null_sort_key_asc",         test_topk_null_sort_key_asc,         topk_setup, topk_teardown },
    { "fused_topk/null_sort_key_desc",        test_topk_null_sort_key_desc,        topk_setup, topk_teardown },
    { "fused_topk/null_desc_backfill",        test_topk_null_desc_backfill,        topk_setup, topk_teardown },
    /* Chunk 11 — gate rejections */
    { "fused_topk/gate_null_tbl",             test_topk_gate_null_tbl,             topk_setup, topk_teardown },
    { "fused_topk/gate_k_too_large",          test_topk_gate_k_too_large,          topk_setup, topk_teardown },
    { "fused_topk/gate_zero_sort_keys",       test_topk_gate_zero_sort_keys,       topk_setup, topk_teardown },
    { "fused_topk/gate_k_ge_nrows",           test_topk_gate_k_ge_nrows,           topk_setup, topk_teardown },
    { "fused_topk/gather_f64_out_col",        test_topk_gathers_f64_out_col_type,       topk_setup, topk_teardown },
    { "fused_topk/gate_unsupported_sort_key", test_topk_gate_unsupported_sort_key_type, topk_setup, topk_teardown },
    { "fused_topk/gate_n_out_zero",           test_topk_gate_n_out_zero,           topk_setup, topk_teardown },
    { "fused_topk/gate_too_many_sort_keys",   test_topk_gate_too_many_sort_keys,   topk_setup, topk_teardown },
    { "fused_topk/gate_negative_k",           test_topk_gate_negative_k,           topk_setup, topk_teardown },
    /* Happy paths */
    { "fused_topk/basic_i64_asc",             test_topk_basic_i64_asc,             topk_setup, topk_teardown },
    { "fused_topk/in_pred",                   test_topk_in_pred,                   topk_setup, topk_teardown },
    { "fused_topk/aliased_out",               test_topk_aliased_out,               topk_setup, topk_teardown },
    { "fused_topk/propagates_nulls",        test_topk_propagates_nulls,        topk_setup, topk_teardown },
    { "fused_topk/three_keys",                test_topk_three_keys,                topk_setup, topk_teardown },
    /* Branch-coverage: esz==2 I16 key */
    { "fused_topk/i16_sort_key",             test_topk_i16_sort_key,              topk_setup, topk_teardown },
    { "fused_topk/i16_sort_key_desc",        test_topk_i16_sort_key_desc,         topk_setup, topk_teardown },
    /* Branch-coverage: esz==1 U8/BOOL keys */
    { "fused_topk/u8_sort_key",              test_topk_u8_sort_key,               topk_setup, topk_teardown },
    { "fused_topk/u8_sort_key_desc",         test_topk_u8_sort_key_desc,          topk_setup, topk_teardown },
    { "fused_topk/bool_sort_key",            test_topk_bool_sort_key,             topk_setup, topk_teardown },
    /* Branch-coverage: temporal type gates */
    { "fused_topk/date_sort_key",            test_topk_date_sort_key,             topk_setup, topk_teardown },
    { "fused_topk/time_sort_key",            test_topk_time_sort_key,             topk_setup, topk_teardown },
    { "fused_topk/timestamp_sort_key",       test_topk_timestamp_sort_key,        topk_setup, topk_teardown },
    /* Branch-coverage: SYM DESC */
    { "fused_topk/sym_key_desc",             test_topk_sym_key_desc,              topk_setup, topk_teardown },
    /* Branch-coverage: both-null tie */
    { "fused_topk/both_nulls_tie",           test_topk_both_nulls_tie,            topk_setup, topk_teardown },
    /* Branch-coverage: selective WHERE + fewer-than-K */
    { "fused_topk/selective_where",          test_topk_selective_where,           topk_setup, topk_teardown },
    { "fused_topk/fewer_than_k_passing",     test_topk_fewer_than_k_passing,      topk_setup, topk_teardown },
    /* Branch-coverage: multi-key null tiebreak */
    { "fused_topk/multi_key_null_tiebreak",  test_topk_multi_key_null_tiebreak,   topk_setup, topk_teardown },
    /* Branch-coverage: I64 DESC */
    { "fused_topk/i64_desc",                 test_topk_i64_desc,                  topk_setup, topk_teardown },
    /* Branch-coverage: I16 with nulls */
    { "fused_topk/i16_with_nulls",           test_topk_i16_with_nulls,            topk_setup, topk_teardown },
    /* Branch-coverage: AND predicate */
    { "fused_topk/and_pred",                 test_topk_and_pred,                  topk_setup, topk_teardown },
    /* Branch-coverage: gate rejections */
    { "fused_topk/gate_sort_key_missing",    test_topk_gate_sort_key_missing,     topk_setup, topk_teardown },
    { "fused_topk/gate_out_col_missing",     test_topk_gate_out_col_missing,      topk_setup, topk_teardown },
    /* Branch-coverage: K=1 edge */
    { "fused_topk/k_equals_one",             test_topk_k_equals_one,              topk_setup, topk_teardown },
    { "fused_topk/k_one_desc",               test_topk_k_one_desc,               topk_setup, topk_teardown },
    /* Branch-coverage: SYM widths W16/W64 */
    { "fused_topk/sym_key_w64",             test_topk_sym_key_w64,               topk_setup, topk_teardown },
    { "fused_topk/sym_key_w16",             test_topk_sym_key_w16,               topk_setup, topk_teardown },
    /* Branch-coverage: non-table tbl gate */
    { "fused_topk/gate_not_table",          test_topk_gate_not_table,            topk_setup, topk_teardown },
    { NULL, NULL, NULL, NULL },
};

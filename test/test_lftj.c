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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "store/csr.h"
#include "ops/lftj.h"
#include <string.h>

/* Helper: build CSR relation from edge arrays */
static ray_rel_t* make_rel(int64_t* src, int64_t* dst, int64_t n,
                           int64_t n_nodes) {
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src, n);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst, n);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                       n_nodes, n_nodes, true);
    ray_release(edges);
    return rel;
}

/* Helper: set up enumeration context output buffers after build_plan */
static void init_enum_output(lftj_enum_ctx_t* ctx, int64_t** col_ptrs) {
    int64_t cap = 64;
    ctx->col_data  = col_ptrs;
    ctx->out_count = 0;
    ctx->out_cap   = cap;
    ctx->oom       = false;
    for (uint8_t v = 0; v < ctx->n_vars; v++) {
        ray_t* h = ray_alloc((size_t)cap * sizeof(int64_t));
        ctx->buf_hdrs[v] = h;
        col_ptrs[v] = (int64_t*)ray_data(h);
    }
}

/* Triangle graph: 0-1, 0-2, 1-2 (bidirectional) */
static test_result_t test_lftj_triangle(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Bidirectional triangle: 0↔1, 0↔2, 1↔2 */
    int64_t src[] = {0, 0, 1, 1, 2, 2};
    int64_t dst[] = {1, 2, 0, 2, 0, 1};
    ray_rel_t* rel = make_rel(src, dst, 6, 3);
    TEST_ASSERT_NOT_NULL(rel);

    /* Find triangles: (a,b,c) where a→b, a→c, b→c */
    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel, rel, rel };
    bool ok = lftj_build_default_plan(&ctx, rels, 3, 3);
    TEST_ASSERT_TRUE(ok);

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    /* One triangle: (0,1,2) in all 6 orderings → 6 results */
    TEST_ASSERT_TRUE(ctx.out_count == 6);

    /* Cleanup output buffers */
    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_lftj_no_results(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Linear graph: 0→1→2 (no triangles) */
    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel, rel, rel };
    bool ok = lftj_build_default_plan(&ctx, rels, 3, 3);
    TEST_ASSERT_TRUE(ok);

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    TEST_ASSERT_TRUE(ctx.out_count == 0);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_leapfrog_search(void) {
    ray_heap_init();

    /* Two sorted arrays, find intersection */
    int64_t a_data[] = {1, 3, 5, 7, 9};
    int64_t b_data[] = {2, 3, 6, 7, 10};

    ray_lftj_iter_t a = { .targets = a_data, .start = 0, .end = 5, .pos = 0 };
    ray_lftj_iter_t b = { .targets = b_data, .start = 0, .end = 5, .pos = 0 };

    ray_lftj_iter_t* iters[] = { &a, &b };
    int64_t val;
    bool found = leapfrog_search(iters, 2, &val);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQ_I(val, 3);

    /* Advance both iterators past 3 and find second intersection (7) */
    a.pos = 3;  /* points to 7 in a_data */
    b.pos = 3;  /* points to 7 in b_data */
    found = leapfrog_search(iters, 2, &val);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQ_I(val, 7);

    /* Advance past 7 -- no more intersections */
    a.pos = 4;  /* points to 9 */
    b.pos = 4;  /* points to 10 */
    found = leapfrog_search(iters, 2, &val);
    TEST_ASSERT_FALSE(found);

    ray_heap_destroy();
    PASS();
}

/* Test lftj_grow_output: start with cap=2, run triangle to force realloc */
static test_result_t test_lftj_grow_output(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Bidirectional triangle: 0↔1, 0↔2, 1↔2 — produces 6 result tuples */
    int64_t src[] = {0, 0, 1, 1, 2, 2};
    int64_t dst[] = {1, 2, 0, 2, 0, 1};
    ray_rel_t* rel = make_rel(src, dst, 6, 3);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel, rel, rel };
    bool ok = lftj_build_default_plan(&ctx, rels, 3, 3);
    TEST_ASSERT_TRUE(ok);

    /* Start with cap=2, which forces lftj_grow_output to trigger */
    int64_t cap = 2;
    int64_t* col_ptrs[LFTJ_MAX_VARS];
    ctx.col_data  = col_ptrs;
    ctx.out_count = 0;
    ctx.out_cap   = cap;
    ctx.oom       = false;
    for (uint8_t v = 0; v < ctx.n_vars; v++) {
        ray_t* h = ray_alloc((size_t)cap * sizeof(int64_t));
        ctx.buf_hdrs[v] = h;
        col_ptrs[v] = (int64_t*)ray_data(h);
    }

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    /* Triangle gives 6 results — requires at least one grow */
    TEST_ASSERT_TRUE(ctx.out_count == 6);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_plan: sv > dv path (rev CSR binding) */
static test_result_t test_lftj_build_plan_rev_binding(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0→1 */
    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 2);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel };
    /* src_var=1, dst_var=0 forces sv > dv → rev CSR binding */
    uint8_t sv[1] = {1};
    uint8_t dv[1] = {0};
    bool ok = lftj_build_plan(&ctx, rels, 1, 2, sv, dv);
    TEST_ASSERT_TRUE(ok);
    /* var_plans[1] should have a binding (rev CSR, bound_var=0) */
    TEST_ASSERT_TRUE(ctx.var_plans[1].n_bindings == 1);
    TEST_ASSERT_TRUE(ctx.var_plans[1].bindings[0].csr == &rel->rev);
    TEST_ASSERT_TRUE(ctx.var_plans[1].bindings[0].bound_var == 0);

    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_plan: self-loop is skipped (sv == dv) */
static test_result_t test_lftj_build_plan_self_loop(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 2);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel };
    /* sv == dv → self-loop, should be skipped (still returns true) */
    uint8_t sv[1] = {0};
    uint8_t dv[1] = {0};
    bool ok = lftj_build_plan(&ctx, rels, 1, 2, sv, dv);
    TEST_ASSERT_TRUE(ok);
    /* No bindings were added */
    TEST_ASSERT_TRUE(ctx.var_plans[0].n_bindings == 0);
    TEST_ASSERT_TRUE(ctx.var_plans[1].n_bindings == 0);

    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_plan: variable out of bounds → returns false */
static test_result_t test_lftj_build_plan_oob_var(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 3);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel };
    /* dv=5 >= n_vars=2 → should return false */
    uint8_t sv[1] = {0};
    uint8_t dv[1] = {5};
    bool ok = lftj_build_plan(&ctx, rels, 1, 2, sv, dv);
    TEST_ASSERT_FALSE(ok);

    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_plan: n_vars > LFTJ_MAX_VARS → returns false */
static test_result_t test_lftj_build_plan_too_many_vars(void) {
    ray_heap_init();
    (void)ray_sym_init();

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* n_vars = LFTJ_MAX_VARS + 1 triggers the guard */
    uint8_t sv[1] = {0};
    uint8_t dv[1] = {1};
    bool ok = lftj_build_plan(&ctx, NULL, 0, LFTJ_MAX_VARS + 1, sv, dv);
    TEST_ASSERT_FALSE(ok);

    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_default_plan: n_vars=2 path */
static test_result_t test_lftj_default_plan_2vars(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Edge: 0→1 */
    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 2);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel };
    /* n_vars=2, n_rels=1 → 2-var path */
    bool ok = lftj_build_default_plan(&ctx, rels, 1, 2);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ_I(ctx.n_vars, 2);

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    /* Should find (0,1) */
    TEST_ASSERT_EQ_I(ctx.out_count, 1);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_default_plan: fallback returns false (unrecognized pattern) */
static test_result_t test_lftj_default_plan_fallback_false(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 2);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* n_vars=5, n_rels=2: neither 3-triangle, 2-var, 4-clique, nor chain (chain needs n_rels==n_vars-1==4) */
    ray_rel_t* rels[] = { rel, rel };
    bool ok = lftj_build_default_plan(&ctx, rels, 2, 5);
    TEST_ASSERT_FALSE(ok);

    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_enumerate: non-root variable with 0 bindings → early return */
static test_result_t test_lftj_enumerate_nonroot_no_bindings(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Edge: 0→1 */
    int64_t src[] = {0};
    int64_t dst[] = {1};
    ray_rel_t* rel = make_rel(src, dst, 1, 2);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Manually build a plan where var 0 has a binding but var 1 has none */
    ctx.n_vars = 2;
    ctx.var_plans[0].n_bindings = 0; /* root, iterated via the all-nodes path */
    ctx.var_plans[1].n_bindings = 0; /* non-root with no bindings → should early-return */

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    /* Put n_nodes so root has something to iterate over;
     * we seed var_plans[0] with a CSR reference only for n_nodes lookup */
    ctx.var_plans[0].n_bindings = 1;
    ctx.var_plans[0].bindings[0].csr = &rel->fwd;
    ctx.var_plans[0].bindings[0].bound_var = 0;
    /* Now clear it back so var 0 has 0 bindings at enumerate time */
    ctx.var_plans[0].n_bindings = 0;

    /* With no CSRs to scan for n_nodes, lftj_enumerate will see n_nodes=0 and return */
    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    TEST_ASSERT_EQ_I(ctx.out_count, 0);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_enumerate: non-root var with 0 bindings (depth > 0) via custom plan */
static test_result_t test_lftj_enumerate_depth1_no_bindings(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0→1→2, n_nodes=3 */
    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 3 variables: var0 has n_bindings=0 (root), var1 has a binding, var2 has n_bindings=0 (non-root, no binding) */
    ctx.n_vars = 3;
    ctx.var_plans[0].n_bindings = 0;
    ctx.var_plans[1].n_bindings = 1;
    ctx.var_plans[1].bindings[0].csr = &rel->fwd;
    ctx.var_plans[1].bindings[0].bound_var = 0;
    ctx.var_plans[2].n_bindings = 0; /* non-root with no bindings */

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    /* var2 has no bindings and depth != 0 → early return, no results emitted */
    TEST_ASSERT_FALSE(ctx.oom);
    TEST_ASSERT_EQ_I(ctx.out_count, 0);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_default_plan: 4-clique plan with actual enumeration */
static test_result_t test_lftj_4clique(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Complete graph on 4 nodes: 0,1,2,3 all connected bidirectionally */
    int64_t src[] = {0,0,0, 1,1, 2, 1,2,3, 2,3, 3};
    int64_t dst[] = {1,2,3, 2,3, 3, 0,0,0, 1,1, 2};
    ray_rel_t* rel = make_rel(src, dst, 12, 4);
    TEST_ASSERT_NOT_NULL(rel);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[6];
    for (int i = 0; i < 6; i++) rels[i] = rel;

    bool ok = lftj_build_default_plan(&ctx, rels, 6, 4);
    TEST_ASSERT_TRUE(ok);

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    /* Should find at least some 4-clique tuples */
    TEST_ASSERT_TRUE(ctx.out_count > 0);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test lftj_build_default_plan: chain fallback (n_vars=3, n_rels=2) */
static test_result_t test_lftj_chain_pattern(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Chain: 0→1→2 */
    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel0 = make_rel(src,     dst,     1, 3);  /* rel for 0→1 */
    ray_rel_t* rel1 = make_rel(src + 1, dst + 1, 1, 3);  /* rel for 1→2 */
    TEST_ASSERT_NOT_NULL(rel0);
    TEST_ASSERT_NOT_NULL(rel1);

    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ray_rel_t* rels[] = { rel0, rel1 };
    /* n_vars=3, n_rels=2 → chain fallback */
    bool ok = lftj_build_default_plan(&ctx, rels, 2, 3);
    TEST_ASSERT_TRUE(ok);

    int64_t* col_ptrs[LFTJ_MAX_VARS];
    init_enum_output(&ctx, col_ptrs);

    lftj_enumerate(&ctx, 0);
    TEST_ASSERT_FALSE(ctx.oom);
    TEST_ASSERT_EQ_I(ctx.out_count, 1);

    for (uint8_t i = 0; i < ctx.n_vars; i++) {
        if (ctx.buf_hdrs[i]) ray_free(ctx.buf_hdrs[i]);
    }
    ray_rel_free(rel0);
    ray_rel_free(rel1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Test leapfrog_search: k=0 returns false */
static test_result_t test_leapfrog_search_k0(void) {
    ray_heap_init();
    int64_t val;
    bool found = leapfrog_search(NULL, 0, &val);
    TEST_ASSERT_FALSE(found);
    ray_heap_destroy();
    PASS();
}

/* Test leapfrog_search: single iterator */
static test_result_t test_leapfrog_search_single(void) {
    ray_heap_init();

    int64_t data[] = {5, 10, 15};
    ray_lftj_iter_t it = { .targets = data, .start = 0, .end = 3, .pos = 0 };
    ray_lftj_iter_t* iters[] = { &it };
    int64_t val;
    bool found = leapfrog_search(iters, 1, &val);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQ_I(val, 5);

    ray_heap_destroy();
    PASS();
}

const test_entry_t lftj_entries[] = {
    { "lftj/triangle", test_lftj_triangle, NULL, NULL },
    { "lftj/no_results", test_lftj_no_results, NULL, NULL },
    { "lftj/leapfrog_search", test_leapfrog_search, NULL, NULL },
    { "lftj/grow_output", test_lftj_grow_output, NULL, NULL },
    { "lftj/build_plan_rev_binding", test_lftj_build_plan_rev_binding, NULL, NULL },
    { "lftj/build_plan_self_loop", test_lftj_build_plan_self_loop, NULL, NULL },
    { "lftj/build_plan_oob_var", test_lftj_build_plan_oob_var, NULL, NULL },
    { "lftj/build_plan_too_many_vars", test_lftj_build_plan_too_many_vars, NULL, NULL },
    { "lftj/default_plan_2vars", test_lftj_default_plan_2vars, NULL, NULL },
    { "lftj/default_plan_fallback_false", test_lftj_default_plan_fallback_false, NULL, NULL },
    { "lftj/enumerate_nonroot_no_bindings", test_lftj_enumerate_nonroot_no_bindings, NULL, NULL },
    { "lftj/enumerate_depth1_no_bindings", test_lftj_enumerate_depth1_no_bindings, NULL, NULL },
    { "lftj/4clique", test_lftj_4clique, NULL, NULL },
    { "lftj/chain_pattern", test_lftj_chain_pattern, NULL, NULL },
    { "lftj/leapfrog_search_k0", test_leapfrog_search_k0, NULL, NULL },
    { "lftj/leapfrog_search_single", test_leapfrog_search_single, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};



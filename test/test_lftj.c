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

const test_entry_t lftj_entries[] = {
    { "lftj/triangle", test_lftj_triangle, NULL, NULL },
    { "lftj/no_results", test_lftj_no_results, NULL, NULL },
    { "lftj/leapfrog_search", test_leapfrog_search, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};



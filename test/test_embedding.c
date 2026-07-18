/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * Tests for Rayfall embedding / vector-similarity / HNSW builtins.
 * Names: cos-dist, l2-dist, inner-prod, plus norm, knn,
 * hnsw-build, ann, hnsw-save, hnsw-load, hnsw-info, hnsw-free.
 */

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "lang/internal.h"
#include "lang/format.h"
#include "store/hnsw.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

/* Small epsilon for F64 comparisons. */
#define EPS 1e-6

static void emb_setup(void) {
    ray_runtime_create(0, NULL);
}

static void emb_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* Evaluate a Rayfall expression and return its F64 atom value.  Scalar
 * return means we can't use the early-return FAIL macros; on failure we
 * call ray_test_fatal(), which longjmp's out of the current test.  The
 * runner catches the jump and reports a normal FAIL, so subsequent
 * tests still run — only the offending test is marked failed. */
static double eval_f64(const char* expr) {
    ray_t* r = ray_eval_str(expr);
    if (!r || RAY_IS_ERR(r) || r->type != -RAY_F64) {
        /* Capture formatted error into a local buffer, reclaim the ray_t
         * error object (ray_release is a no-op on errors), then longjmp
         * via ray_test_fatal — must free before the noreturn call. */
        ray_t*      fs = r ? ray_fmt(r, 0) : NULL;
        char em[256]; size_t fl = fs ? ray_str_len(fs) : 5;
        if (fl > sizeof em - 1) fl = sizeof em - 1;
        if (fs) { memcpy(em, ray_str_ptr(fs), fl); ray_release(fs); }
        else    { memcpy(em, "<nil>", 5); fl = 5; }
        em[fl] = '\0';
        if (r && RAY_IS_ERR(r)) ray_error_free(r);
        ray_test_fatal("eval_f64 failed: %s  -> %s", expr, em);
    }
    double v = r->f64;
    ray_release(r);
    return v;
}

static int64_t eval_i64(const char* expr) {
    ray_t* r = ray_eval_str(expr);
    if (!r || RAY_IS_ERR(r)
        || (r->type != -RAY_I64 && r->type != -RAY_I32 && r->type != -RAY_I16)) {
        ray_t*      fs = r ? ray_fmt(r, 0) : NULL;
        char em[256]; size_t fl = fs ? ray_str_len(fs) : 5;
        if (fl > sizeof em - 1) fl = sizeof em - 1;
        if (fs) { memcpy(em, ray_str_ptr(fs), fl); ray_release(fs); }
        else    { memcpy(em, "<nil>", 5); fl = 5; }
        em[fl] = '\0';
        if (r && RAY_IS_ERR(r)) ray_error_free(r);
        ray_test_fatal("eval_i64 failed: %s  -> %s", expr, em);
    }
    int64_t v;
    switch (r->type) {
        case -RAY_I64: v = r->i64; break;
        case -RAY_I32: v = r->i32; break;
        default:       v = r->i16; break;
    }
    ray_release(r);
    return v;
}

/* ============ Direct metric builtins — scalar results ============ */

static test_result_t test_cos_dist_scalar(void) {
    /* Orthogonal vectors: cos(90°) = 0 → distance 1. */
    TEST_ASSERT_EQ_F(eval_f64("(cos-dist [1.0 0.0] [0.0 1.0])"), 1.0, 1e-6);
    /* Parallel identical: cos(0°) = 1 → distance 0. */
    TEST_ASSERT_EQ_F(eval_f64("(cos-dist [1.0 1.0] [1.0 1.0])"), 0.0, 1e-6);
    /* Opposite: cos(180°) = -1 → distance 2. */
    TEST_ASSERT_EQ_F(eval_f64("(cos-dist [1.0 0.0] [-1.0 0.0])"), 2.0, 1e-6);
    /* 45°: cos = √2/2 → distance 1 - √2/2 ≈ 0.2929. */
    double v = eval_f64("(cos-dist [1.0 0.0] [1.0 1.0])");
    TEST_ASSERT_EQ_F(v, 1.0 - 1.0/sqrt(2.0), 1e-6);
    PASS();
}

static test_result_t test_l2_dist_scalar(void) {
    TEST_ASSERT_EQ_F(eval_f64("(l2-dist [0.0 0.0] [3.0 4.0])"), 5.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(l2-dist [1.0 1.0] [1.0 1.0])"), 0.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(l2-dist [0.0 0.0 0.0] [1.0 2.0 2.0])"), 3.0, 1e-6);
    PASS();
}

static test_result_t test_inner_prod_scalar(void) {
    TEST_ASSERT_EQ_F(eval_f64("(inner-prod [1.0 2.0 3.0] [4.0 5.0 6.0])"), 32.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(inner-prod [1.0 0.0] [0.0 1.0])"), 0.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(inner-prod [2.0 3.0] [-1.0 -1.0])"), -5.0, 1e-6);
    PASS();
}

static test_result_t test_norm_scalar(void) {
    TEST_ASSERT_EQ_F(eval_f64("(norm [3.0 4.0])"), 5.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(norm [0.0 0.0])"), 0.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(norm [1.0 2.0 2.0])"), 3.0, 1e-6);
    PASS();
}

/* ============ Errors on malformed inputs ============ */

static test_result_t test_metric_length_mismatch(void) {
    ray_t* r = ray_eval_str("(cos-dist [1.0 2.0 3.0] [1.0 2.0])");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

static test_result_t test_metric_type_error(void) {
    /* String isn't a numeric vec. */
    ray_t* r = ray_eval_str("(l2-dist \"hello\" [1.0 2.0])");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ============ LIST-column metrics — vector results ============ */

static test_result_t test_cos_dist_list(void) {
    /* Query [1,0,0] against rows [1,0,0], [0,1,0], [1,1,0]: distances 0, 1, 1-1/√2. */
    double d0 = eval_f64("(at (cos-dist (list [1.0 0.0 0.0] [0.0 1.0 0.0] [1.0 1.0 0.0]) [1.0 0.0 0.0]) 0)");
    double d1 = eval_f64("(at (cos-dist (list [1.0 0.0 0.0] [0.0 1.0 0.0] [1.0 1.0 0.0]) [1.0 0.0 0.0]) 1)");
    double d2 = eval_f64("(at (cos-dist (list [1.0 0.0 0.0] [0.0 1.0 0.0] [1.0 1.0 0.0]) [1.0 0.0 0.0]) 2)");
    TEST_ASSERT_EQ_F(d0, 0.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, 1.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 1.0 - 1.0/sqrt(2.0), 1e-6);
    PASS();
}

static test_result_t test_l2_dist_list(void) {
    double d0 = eval_f64("(at (l2-dist (list [0.0 0.0] [3.0 4.0] [6.0 8.0]) [0.0 0.0]) 0)");
    double d1 = eval_f64("(at (l2-dist (list [0.0 0.0] [3.0 4.0] [6.0 8.0]) [0.0 0.0]) 1)");
    double d2 = eval_f64("(at (l2-dist (list [0.0 0.0] [3.0 4.0] [6.0 8.0]) [0.0 0.0]) 2)");
    TEST_ASSERT_EQ_F(d0, 0.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, 5.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 10.0, 1e-6);
    PASS();
}

static test_result_t test_inner_prod_list(void) {
    double d0 = eval_f64("(at (inner-prod (list [1.0 0.0] [0.0 1.0] [1.0 1.0]) [1.0 2.0]) 0)");
    double d1 = eval_f64("(at (inner-prod (list [1.0 0.0] [0.0 1.0] [1.0 1.0]) [1.0 2.0]) 1)");
    double d2 = eval_f64("(at (inner-prod (list [1.0 0.0] [0.0 1.0] [1.0 1.0]) [1.0 2.0]) 2)");
    TEST_ASSERT_EQ_F(d0, 1.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, 2.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 3.0, 1e-6);
    PASS();
}

static test_result_t test_norm_list(void) {
    double d0 = eval_f64("(at (norm (list [3.0 4.0] [5.0 12.0] [8.0 15.0])) 0)");
    double d1 = eval_f64("(at (norm (list [3.0 4.0] [5.0 12.0] [8.0 15.0])) 1)");
    double d2 = eval_f64("(at (norm (list [3.0 4.0] [5.0 12.0] [8.0 15.0])) 2)");
    TEST_ASSERT_EQ_F(d0, 5.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, 13.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 17.0, 1e-6);
    PASS();
}

/* ============ Brute-force knn — correctness per metric ============ */

/* Helper: common 5-row test column built in-script. */
#define FIVE_VECS "(list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0] [1.0 1.0 0.0] [1.0 0.0 1.0])"

static test_result_t test_knn_cosine(void) {
    /* Top-3 nearest to [1,0,0] by cosine distance: row 0 (dist 0), then rows 3 and 4 (both 1-1/√2). */
    TEST_ASSERT_EQ_I(eval_i64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'cosine) '_rowid) 0)"), 0);
    double d0 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'cosine) '_dist) 0)");
    TEST_ASSERT_EQ_F(d0, 0.0, 1e-6);
    double d1 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'cosine) '_dist) 1)");
    TEST_ASSERT_EQ_F(d1, 1.0 - 1.0/sqrt(2.0), 1e-6);
    double d2 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'cosine) '_dist) 2)");
    TEST_ASSERT_EQ_F(d2, 1.0 - 1.0/sqrt(2.0), 1e-6);
    PASS();
}

static test_result_t test_knn_l2(void) {
    TEST_ASSERT_EQ_I(eval_i64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'l2) '_rowid) 0)"), 0);
    double d0 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'l2) '_dist) 0)");
    double d1 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'l2) '_dist) 1)");
    double d2 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'l2) '_dist) 2)");
    TEST_ASSERT_EQ_F(d0, 0.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, 1.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 1.0, 1e-6);
    PASS();
}

static test_result_t test_knn_ip(void) {
    /* Inner-product metric sorts by -dot; all vecs that match with dot=1 tie at -1. */
    double d0 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'ip) '_dist) 0)");
    double d1 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'ip) '_dist) 1)");
    double d2 = eval_f64("(at (at (knn " FIVE_VECS " [1.0 0.0 0.0] 3 'ip) '_dist) 2)");
    TEST_ASSERT_EQ_F(d0, -1.0, 1e-6);
    TEST_ASSERT_EQ_F(d1, -1.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, -1.0, 1e-6);
    PASS();
}

/* ============ HNSW lifecycle ============ */

static test_result_t test_hnsw_info_and_metric(void) {
    /* Build with 'l2 and verify hnsw-info reports dim, metric, n-nodes. */
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'l2 4 50))");
    TEST_ASSERT_EQ_I(eval_i64("(at (hnsw-info __idx) 'dim)"), 3);
    TEST_ASSERT_EQ_I(eval_i64("(at (hnsw-info __idx) 'nrows)"), 5);
    TEST_ASSERT_EQ_I(eval_i64("(at (hnsw-info __idx) 'M)"), 4);
    TEST_ASSERT_EQ_I(eval_i64("(at (hnsw-info __idx) 'efc)"), 50);
    /* metric is a SYM atom; verify via equality with 'l2. */
    ray_t* r = ray_eval_str("(== (at (hnsw-info __idx) 'metric) 'l2)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(r->b8, 1);
    ray_release(r);
    ray_eval_str("(hnsw-free __idx)");
    PASS();
}

static test_result_t test_ann_topk(void) {
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'cosine 8 100))");
    /* Row 0 must be the closest (identity). */
    TEST_ASSERT_EQ_I(eval_i64("(at (at (ann __idx [1.0 0.0 0.0] 1) '_rowid) 0)"), 0);
    TEST_ASSERT_EQ_F(eval_f64("(at (at (ann __idx [1.0 0.0 0.0] 1) '_dist) 0)"), 0.0, 1e-6);
    ray_eval_str("(hnsw-free __idx)");
    PASS();
}

/* Build two indexes with the same data, one per metric (l2, cosine), verify
 * that the row-0-self query gives distance 0 in both. */
static test_result_t test_ann_each_metric(void) {
    ray_eval_str("(set __idx_l2 (hnsw-build " FIVE_VECS " 'l2 8 100))");
    ray_eval_str("(set __idx_co (hnsw-build " FIVE_VECS " 'cosine 8 100))");
    TEST_ASSERT_EQ_F(eval_f64("(at (at (ann __idx_l2 [1.0 0.0 0.0] 1) '_dist) 0)"), 0.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(at (at (ann __idx_co [1.0 0.0 0.0] 1) '_dist) 0)"), 0.0, 1e-6);
    ray_eval_str("(hnsw-free __idx_l2)");
    ray_eval_str("(hnsw-free __idx_co)");
    PASS();
}

/* Recall check — build a random 200×16 index, measure recall@5 vs exact knn. */
static test_result_t test_ann_recall(void) {
    /* Use C random with fixed seed for reproducibility. */
    srand(42);
    const int N = 200, D = 16, K = 5, QUERIES = 10;

    /* Build an RAY_LIST of RAY_F32 vectors. */
    ray_t* list = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(list);
    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_new(RAY_F32, D);
        v->len = D;
        float* d = (float*)ray_data(v);
        for (int j = 0; j < D; j++) d[j] = (float)rand() / (float)RAND_MAX - 0.5f;
        list = ray_list_append(list, v);
        ray_release(v);
    }
    TEST_ASSERT_EQ_I(list->len, N);

    /* Bind list as a global variable so eval_str can reach it. */
    int64_t sym = ray_sym_intern("__recall_list", 13);
    TEST_ASSERT_EQ_I(ray_env_set(sym, list), RAY_OK);
    ray_release(list);

    ray_eval_str("(set __recall_idx (hnsw-build __recall_list 'l2 16 100))");

    int hits = 0, total = 0;
    for (int q = 0; q < QUERIES; q++) {
        /* Pick row q as the query (so exact match is guaranteed at position 0). */
        char expr[256];
        snprintf(expr, sizeof(expr),
                 "(at (at (ann __recall_idx (at __recall_list %d) %d) '_rowid) 0)", q, K);
        int64_t top_ann = eval_i64(expr);

        snprintf(expr, sizeof(expr),
                 "(at (at (knn __recall_list (at __recall_list %d) %d 'l2) '_rowid) 0)", q, K);
        int64_t top_exact = eval_i64(expr);

        if (top_ann == top_exact) hits++;
        total++;
    }
    ray_eval_str("(hnsw-free __recall_idx)");
    /* Self-query: every top-1 must be the row itself. */
    TEST_ASSERT_EQ_I(hits, total);
    PASS();
}

/* Review §2.9 #4/#5 — HNSW back-link saturation must keep the M closest
 * neighbors instead of silently dropping every back-link once a list is full.
 *
 * With a small M (=4) and many clustered vectors, back-link lists saturate
 * frequently during build.  Before the fix, prune was a dead no-op (M_keep ==
 * M_max), so a full list dropped the new back-link → degraded connectivity.
 * After the fix the saturated list keeps the M closest, so self-queries must
 * still resolve to the node itself (recall@1 == 1.0). */
static test_result_t test_ann_recall_saturated(void) {
    srand(1234);
    const int N = 150, D = 8, K = 5, QUERIES = 40;

    ray_t* list = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(list);
    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_new(RAY_F32, D);
        v->len = D;
        float* d = (float*)ray_data(v);
        /* Spread vectors so duplicates are unlikely (each row is a distinct
         * nearest neighbor of itself), but dense enough that the small-M
         * neighbor lists saturate and exercise the back-link replacement. */
        for (int j = 0; j < D; j++)
            d[j] = (float)rand() / (float)RAND_MAX - 0.5f;
        list = ray_list_append(list, v);
        ray_release(v);
    }
    int64_t sym = ray_sym_intern("__sat_list", 10);
    TEST_ASSERT_EQ_I(ray_env_set(sym, list), RAY_OK);
    ray_release(list);

    /* Small M (=4) forces back-link saturation during build.  With the
     * "keep M closest" replacement in place the graph stays well connected,
     * so self-queries resolve to the node itself with high recall.  Before
     * the fix, saturated lists silently dropped every back-link, fragmenting
     * the graph and degrading recall@1. */
    ray_eval_str("(set __sat_idx (hnsw-build __sat_list 'l2 4 80))");

    int hits = 0, total = 0;
    for (int q = 0; q < QUERIES; q++) {
        char expr[256];
        snprintf(expr, sizeof(expr),
                 "(at (at (ann __sat_idx (at __sat_list %d) %d) '_rowid) 0)", q, K);
        int64_t top_ann = eval_i64(expr);
        if (top_ann == q) hits++;
        total++;
    }
    ray_eval_str("(hnsw-free __sat_idx)");
    /* Self-query recall@1: the fix keeps this high even at M=4.  Assert a
     * conservative floor (90%) that the fixed connectivity comfortably meets
     * but the silent-drop regression would fall below. */
    TEST_ASSERT_TRUE(hits * 100 >= total * 90);
    PASS();
}

/* ============ Persistence round-trip ============ */

static test_result_t test_hnsw_save_load(void) {
    const char* dir = "/tmp/ray_hnsw_test_idx";
    /* Build and save in 'ip metric. */
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'ip 4 50))");
    char expr[512];
    snprintf(expr, sizeof(expr), "(hnsw-save __idx \"%s\")", dir);
    ray_t* saved = ray_eval_str(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(saved));
    ray_eval_str("(hnsw-free __idx)");

    /* Load from disk, confirm metric still 'ip, then query. */
    snprintf(expr, sizeof(expr), "(set __idx2 (hnsw-load \"%s\"))", dir);
    ray_t* loaded = ray_eval_str(expr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* metric_eq = ray_eval_str("(== (at (hnsw-info __idx2) 'metric) 'ip)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(metric_eq));
    TEST_ASSERT_EQ_I(metric_eq->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(metric_eq->b8, 1);
    ray_release(metric_eq);

    /* ann on a known vector must return an inner-product-optimal row.
     * Under 'ip, query [1,0,0] ties IP=1 across rows 0=[1,0,0], 3=[1,1,0]
     * and 4=[1,0,1], so the approximate top-1 is any of {0,3,4} depending on
     * graph connectivity — assert membership, not a specific (unstable) row.
     * (Re-baselined from a hard-coded 0, which pinned a build-order tie that
     * the HNSW saturation/back-link fix legitimately perturbs.) */
    int64_t ip_top = eval_i64("(at (at (ann __idx2 [1.0 0.0 0.0] 1) '_rowid) 0)");
    TEST_ASSERT_TRUE(ip_top == 0 || ip_top == 3 || ip_top == 4);
    ray_eval_str("(hnsw-free __idx2)");
    PASS();
}

/* ============ Handle lifecycle — double-free returns type error ============ */

static test_result_t test_hnsw_free_twice(void) {
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'cosine 4 50))");
    ray_t* r1 = ray_eval_str("(hnsw-free __idx)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    /* Second free: handle attr is cleared, so unwrap fails → type error. */
    ray_t* r2 = ray_eval_str("(hnsw-free __idx)");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    PASS();
}

/* Rebinding a handle variable must not leak the previously-bound index.
 * ASan would report the leak on runtime teardown if rc→0 cleanup missed it. */
static test_result_t test_hnsw_rebind_no_leak(void) {
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'cosine 4 50))");
    /* Rebind: the old handle's rc hits zero, ray_free runs, the rc-cleanup
     * hook in src/mem/heap.c frees the underlying ray_hnsw_t. */
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'l2 4 50))");
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'ip 4 50))");
    ray_eval_str("(hnsw-free __idx)");
    PASS();
}

/* Handle produced inline (never bound) must not leak after the enclosing
 * expression completes. */
static test_result_t test_hnsw_inline_no_leak(void) {
    ray_t* r = ray_eval_str(
        "(at (at (ann (hnsw-build " FIVE_VECS " 'l2 4 50) [1.0 0.0 0.0] 1) '_rowid) 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* Handle left bound to a variable at runtime teardown must not leak.
 * ASan reports any leaked ray_hnsw_t at process exit. */
static test_result_t test_hnsw_teardown_no_leak(void) {
    ray_eval_str("(set __leak_idx (hnsw-build " FIVE_VECS " 'cosine 4 50))");
    PASS();
}

/* Exercise the generic copy path directly.  ray_alloc_copy must deep-clone
 * the underlying ray_hnsw_t so the copy is a full, independent owner with
 * the same index contents — and freeing either does not disturb the other. */
static test_result_t test_hnsw_handle_cow(void) {
    ray_eval_str("(set __idx (hnsw-build " FIVE_VECS " 'cosine 4 50))");
    ray_t* h = ray_env_get(ray_sym_intern("__idx", 5));
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQ_I(h->type, -RAY_I64);
    TEST_ASSERT_TRUE(h->attrs & RAY_ATTR_HNSW);
    int64_t orig_ptr = h->i64;
    TEST_ASSERT((orig_ptr) != (0), "orig_ptr != 0");

    /* Copy via the same path ray_cow uses.  The copy must OWN its own
     * ray_hnsw_t (deep-cloned from the source), not alias the original. */
    ray_t* copy = ray_alloc_copy(h);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQ_I(copy->type, -RAY_I64);
    TEST_ASSERT_TRUE(copy->attrs & RAY_ATTR_HNSW);
    TEST_ASSERT((copy->i64) != (0), "copy->i64 != 0");
    TEST_ASSERT((copy->i64) != (orig_ptr), "copy->i64 != orig_ptr");

    /* Both indexes should answer the same query — same data, independent memory. */
    ray_hnsw_t* src = (ray_hnsw_t*)(uintptr_t)orig_ptr;
    ray_hnsw_t* dup = (ray_hnsw_t*)(uintptr_t)copy->i64;
    TEST_ASSERT_EQ_I(dup->n_nodes, src->n_nodes);
    TEST_ASSERT_EQ_I(dup->dim, src->dim);
    TEST_ASSERT_EQ_I(dup->metric, src->metric);

    float q[3] = {1.0f, 0.0f, 0.0f};
    int64_t ids_src[1], ids_dup[1];
    double  ds_src[1], ds_dup[1];
    TEST_ASSERT_EQ_I(ray_hnsw_search(src, q, 3, 1, 50, ids_src, ds_src), 1);
    TEST_ASSERT_EQ_I(ray_hnsw_search(dup, q, 3, 1, 50, ids_dup, ds_dup), 1);
    TEST_ASSERT_EQ_I(ids_src[0], ids_dup[0]);
    TEST_ASSERT_EQ_F(ds_src[0], ds_dup[0], 1e-6);

    /* Releasing the copy frees the cloned index but leaves the original intact. */
    ray_release(copy);
    TEST_ASSERT_TRUE(h->attrs & RAY_ATTR_HNSW);
    TEST_ASSERT_EQ_I(h->i64, orig_ptr);
    TEST_ASSERT_EQ_I(eval_i64("(at (hnsw-info __idx) 'nrows)"), 5);

    ray_eval_str("(hnsw-free __idx)");
    PASS();
}

/* ============ select ... nearest ... take — Pass 2 integration ============ */

/* Helper: build a 5-row test table with id / score / emb columns.  Runs
 * in the Rayfall env, then returns nothing.  The subsequent eval_* calls
 * reference `__docs` by name. */
static void build_docs(void) {
    ray_eval_str(
        "(set __docs (table [id score emb] "
        "  (list [0 1 2 3 4] "
        "        [0.9 0.2 0.8 0.1 0.7] "
        "        (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0] "
        "              [1.0 1.0 0.0] [1.0 0.0 1.0]))))");
}

static test_result_t test_select_nearest_knn(void) {
    build_docs();
    /* Top-3 nearest to [1,0,0] by cosine over full table.  Expect row 0
     * first (exact match, dist 0).  `select ... nearest` returns source
     * columns projected at the matching rowids; use the user's id column. */
    TEST_ASSERT_EQ_I(eval_i64(
        "(at (at (select {from: __docs nearest: (knn emb [1.0 0.0 0.0]) take: 3}) 'id) 0)"), 0);
    /* `_dist` is only in the output when the user projects it explicitly. */
    TEST_ASSERT_EQ_F(eval_f64(
        "(at (at (select {id: id d: _dist from: __docs "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 3}) 'd) 0)"), 0.0, 1e-6);
    PASS();
}

static test_result_t test_select_nearest_ann(void) {
    build_docs();
    ray_eval_str("(set __idx (hnsw-build (at __docs 'emb) 'cosine))");
    /* Top-1 must be exact match (row 0, id=0, dist=0). */
    TEST_ASSERT_EQ_I(eval_i64(
        "(at (at (select {from: __docs nearest: (ann __idx [1.0 0.0 0.0]) take: 1}) 'id) 0)"), 0);
    TEST_ASSERT_EQ_F(eval_f64(
        "(at (at (select {id: id d: _dist from: __docs "
        "                 nearest: (ann __idx [1.0 0.0 0.0]) take: 1}) 'd) 0)"), 0.0, 1e-6);
    ray_eval_str("(hnsw-free __idx)");
    PASS();
}

static test_result_t test_select_nearest_filter(void) {
    build_docs();
    /* Filter keeps rows {0, 2, 4} (score > 0.5).  Top-2 nearest to
     * [1,0,0]: id=0 (exact), id=4 (cos=1/√2, dist≈0.293).  id=3 is
     * excluded because its score=0.1. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs where: (> score 0.5) "
        "         nearest: (knn emb [1.0 0.0 0.0]) take: 2})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);
    ray_release(r);

    TEST_ASSERT_EQ_I(eval_i64(
        "(at (at (select {from: __docs where: (> score 0.5) "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 2}) 'id) 0)"), 0);
    TEST_ASSERT_EQ_I(eval_i64(
        "(at (at (select {from: __docs where: (> score 0.5) "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 2}) 'id) 1)"), 4);
    PASS();
}

static test_result_t test_select_nearest_empty_filter(void) {
    build_docs();
    /* Filter rejects every row (score > 100) → empty result with schema. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs where: (> score 100.0) "
        "         nearest: (knn emb [1.0 0.0 0.0]) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);
    /* Schema preserved: id, score, emb (3 source columns).  `_dist` is
     * only present when the user projects it explicitly. */
    TEST_ASSERT_EQ_I(ray_table_ncols(r), 3);
    ray_release(r);
    PASS();
}

static test_result_t test_select_nearest_take_default(void) {
    build_docs();
    /* No take: default k=10, but table only has 5 rows → 5 results. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs nearest: (knn emb [1.0 0.0 0.0])})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 5);
    ray_release(r);
    PASS();
}

static test_result_t test_select_nearest_asc_rejected(void) {
    build_docs();
    /* nearest + asc on same select must error — the two orderings are
     * mutually exclusive. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs "
        "         nearest: (knn emb [1.0 0.0 0.0]) "
        "         asc: score take: 2})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* `_dist` must NOT leak into the output schema when the user didn't
 * project it.  (select {from: t nearest: ...}) must be shape-compatible
 * with (select {from: t}). */
static test_result_t test_select_nearest_implicit_no_dist(void) {
    build_docs();
    ray_t* r = ray_eval_str(
        "(select {from: __docs nearest: (knn emb [1.0 0.0 0.0]) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    /* Must match source schema: id, score, emb — no _dist. */
    TEST_ASSERT_EQ_I(ray_table_ncols(r), 3);
    /* Look up `_dist` explicitly: should fail (column absent). */
    int64_t dist_sym = ray_sym_intern("_dist", 5);
    TEST_ASSERT_EQ_PTR(ray_table_get_col(r, dist_sym), NULL);
    ray_release(r);
    PASS();
}

/* STR columns in the source must project correctly through the rerank
 * gather path (previously errored with "nyi" and mis-copied DATE/TIME). */
static test_result_t test_select_nearest_str_col(void) {
    ray_eval_str(
        "(set __tdocs (table [id title emb] "
        "  (list [0 1 2 3 4] "
        "        (list \"alpha\" \"beta\" \"gamma\" \"delta\" \"epsilon\") "
        "        (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0] "
        "              [1.0 1.0 0.0] [1.0 0.0 1.0]))))");
    /* Top-1 by cosine to [1,0,0] is row 0 ("alpha"). */
    ray_t* r = ray_eval_str(
        "(at (at (select {id: id title: title from: __tdocs "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 1}) 'title) 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* STR atom. */
    TEST_ASSERT_EQ_I(r->type, -RAY_STR);
    TEST_ASSERT_EQ_I((int)ray_str_len(r), 5);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(r), "alpha");
    ray_release(r);
    PASS();
}

/* SYM columns round-trip correctly through the rerank gather path.
 * (An earlier revision passed the ray_write_sym args in the wrong order,
 * producing garbage sym ids.) */
static test_result_t test_select_nearest_sym_col(void) {
    /* A SYM column holding the 5 tickers; each test sym_id is global. */
    ray_eval_str(
        "(set __sdocs (table [id tag emb] "
        "  (list [0 1 2 3 4] "
        "        [alpha beta gamma delta epsilon] "
        "        (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0] "
        "              [1.0 1.0 0.0] [1.0 0.0 1.0]))))");
    /* Top-1 cosine to [1,0,0] → row 0 → tag `alpha`. */
    ray_t* r = ray_eval_str(
        "(at (at (select {id: id tag: tag from: __sdocs "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 1}) 'tag) 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_SYM);
    int64_t expected = ray_sym_intern("alpha", 5);
    TEST_ASSERT_EQ_I(r->i64, expected);
    ray_release(r);
    PASS();
}

/* Empty source table (0 rows) must not crash the oversample or heap
 * allocation — returns a well-shaped 0-row result. */
static test_result_t test_select_nearest_empty_source(void) {
    /* Build a 3-column empty table with an empty list for `emb`. */
    ray_eval_str(
        "(set __empty (table [id emb] (list [] (list))))");
    ray_t* r = ray_eval_str(
        "(select {from: __empty nearest: (knn emb [1.0 0.0 0.0]) take: 5})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);
    TEST_ASSERT_EQ_I(ray_table_ncols(r), 2);  /* id, emb — no _dist */
    ray_release(r);
    PASS();
}

/* Inline HNSW handle in `nearest (ann ...)`: the handle has rc=1 at eval
 * time.  If the select parser releases it before ray_execute, the rc→0
 * hook frees the underlying index and the DAG exec uses a dangling
 * pointer — a use-after-free that ASan catches.  The handle must be kept
 * alive through ray_execute. */
static test_result_t test_select_nearest_ann_inline_handle(void) {
    build_docs();
    /* `(hnsw-build ...)` is evaluated inline and never bound to a var —
     * its rc is 1 at the moment `select` sees it.  Previously this
     * freed the index mid-DAG; the fix retains the handle through exec. */
    ray_t* r = ray_eval_str(
        "(select {id: id d: _dist from: __docs "
        "         nearest: (ann (hnsw-build (at __docs 'emb) 'cosine) "
        "                       [1.0 0.0 0.0]) "
        "         take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    /* Row 0 is the exact match for [1,0,0]. */
    ray_t* id_col = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(id_col))[0], 0);
    ray_release(r);
    PASS();
}

/* Invalid handle payload: attr set but pointer zeroed (e.g. after
 * hnsw-free).  Must return a clean type error rather than dereferencing
 * NULL. */
static test_result_t test_select_nearest_ann_freed_handle(void) {
    build_docs();
    ray_eval_str("(set __idx (hnsw-build (at __docs 'emb)))");
    ray_eval_str("(hnsw-free __idx)");
    /* __idx now has attr cleared AND i64=0.  The attr-cleared state is
     * caught by the handle-type check; ensure a hypothetical attr-still-set
     * but i64=0 state also errors cleanly.  We hit the same error path via
     * the type check — just assert the query errors rather than crashes. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs nearest: (ann __idx [1.0 0.0 0.0]) take: 1})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* Projection compile failure must not leak the retained inline HNSW handle
 * or the owned query buffer.  ASan catches leaks/UAF at runtime teardown. */
static test_result_t test_select_nearest_ann_projection_error(void) {
    build_docs();
    /* `(hnsw-build (at __docs 'emb))` is inline and rc=1.  The projection
     * uses an unknown function name that compile_expr_dag cannot lower —
     * the error path must release the nearest handle and free the query
     * buffer before returning. */
    ray_t* r = ray_eval_str(
        "(select {bad: (no-such-function id) "
        "         from: __docs "
        "         nearest: (ann (hnsw-build (at __docs 'emb) 'cosine) "
        "                       [1.0 0.0 0.0]) "
        "         take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* If we got here with no ASan complaint, the handle/query cleanup
     * worked.  A subsequent ordinary query must still run fine. */
    TEST_ASSERT_EQ_I(eval_i64(
        "(at (at (select {id: id from: __docs "
        "                 nearest: (knn emb [1.0 0.0 0.0]) take: 1}) 'id) 0)"), 0);
    PASS();
}

/* Filter-aware iterative scan: a modestly selective filter (~50% pass rate).
 *
 * The prior oversample+refilter path pulled K × oversample_factor candidates
 * without filter awareness, then dropped non-matching ones — risking
 * undershoot for any filter with local rejections.  The iterative scan
 * pushes the predicate into the beam so rejected candidates don't consume
 * result slots.
 *
 * We verify:
 *  - full K rows returned,
 *  - every returned row passes the filter,
 *  - ordering is ascending by _dist.
 *
 * (Very-low-selectivity filters are a separate domain — HNSW beam is
 * locality-bounded by design; the standard remedy is
 * `iterative_scan_max_tuples` / multi-entry restart.) */
static test_result_t test_select_nearest_iterative_selective(void) {
    srand(42);
    const int N = 300, D = 16, K = 10;

    ray_t* vlist = ray_list_new(N);
    ray_t* scores = ray_vec_new(RAY_F64, N);
    TEST_ASSERT_NOT_NULL(vlist);
    TEST_ASSERT_NOT_NULL(scores);
    scores->len = N;
    double* sdata = (double*)ray_data(scores);

    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_new(RAY_F32, D);
        v->len = D;
        float* d = (float*)ray_data(v);
        for (int j = 0; j < D; j++) d[j] = (float)rand() / (float)RAND_MAX - 0.5f;
        vlist = ray_list_append(vlist, v);
        ray_release(v);
        /* ~50% pass the filter — enough density that iterative scan can
         * reliably fill K via graph traversal. */
        sdata[i] = (double)rand() / (double)RAND_MAX;
    }

    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__it_list", 9), vlist), RAY_OK);
    ray_release(vlist);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__it_scores", 11), scores), RAY_OK);
    ray_release(scores);

    ray_eval_str(
        "(set __it_tbl (table [id score emb] "
        "  (list (til 300) __it_scores __it_list)))");
    ray_eval_str("(set __it_idx (hnsw-build __it_list 'l2 16 100))");

    ray_t* r = ray_eval_str(
        "(select {id: id score: score d: _dist from: __it_tbl "
        "         where: (> score 0.5) "
        "         nearest: (ann __it_idx (at __it_list 0)) "
        "         take: 10})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    /* Must return K rows — iterative scan fills from graph neighborhood. */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), K);

    /* Every returned row passes the filter (score > 0.5). */
    ray_t* sc_col = ray_table_get_col(r, ray_sym_intern("score", 5));
    TEST_ASSERT_NOT_NULL(sc_col);
    TEST_ASSERT_EQ_I(sc_col->type, RAY_F64);
    double* sc = (double*)ray_data(sc_col);
    for (int64_t i = 0; i < ray_table_nrows(r); i++)
        TEST_ASSERT_TRUE(sc[i] > 0.5);

    /* Distances ascending. */
    ray_t* d_col = ray_table_get_col(r, ray_sym_intern("d", 1));
    TEST_ASSERT_NOT_NULL(d_col);
    double* dd = (double*)ray_data(d_col);
    for (int64_t i = 1; i < ray_table_nrows(r); i++)
        TEST_ASSERT_TRUE(dd[i] >= dd[i - 1]);

    ray_release(r);
    ray_eval_str("(hnsw-free __it_idx)");
    PASS();
}

static test_result_t test_select_nearest_recall(void) {
    /* 200×16 random vectors, self-query each one: ANN top-1 via select must
     * match brute-force top-1 via select for every query (recall@1 = 1.0). */
    srand(17);
    const int N = 200, D = 16;
    ray_t* list = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(list);
    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_new(RAY_F32, D);
        v->len = D;
        float* d = (float*)ray_data(v);
        for (int j = 0; j < D; j++) d[j] = (float)rand() / (float)RAND_MAX - 0.5f;
        list = ray_list_append(list, v);
        ray_release(v);
    }
    int64_t list_sym = ray_sym_intern("__rv_list", 9);
    TEST_ASSERT_EQ_I(ray_env_set(list_sym, list), RAY_OK);
    ray_release(list);

    /* Build a table with row-id + emb. */
    ray_eval_str(
        "(set __rv (table [id emb] "
        "  (list (til 200) __rv_list)))");
    ray_eval_str("(set __rv_idx (hnsw-build __rv_list 'l2 16 100))");

    int hits = 0;
    for (int q = 0; q < 20; q++) {
        char expr[512];
        snprintf(expr, sizeof(expr),
            "(at (at (select {from: __rv "
            "                 nearest: (ann __rv_idx (at __rv_list %d)) "
            "                 take: 1}) 'id) 0)", q);
        int64_t ann_id = eval_i64(expr);

        snprintf(expr, sizeof(expr),
            "(at (at (select {from: __rv "
            "                 nearest: (knn emb (at __rv_list %d) 'l2) "
            "                 take: 1}) 'id) 0)", q);
        int64_t knn_id = eval_i64(expr);
        if (ann_id == knn_id && ann_id == q) hits++;
    }
    ray_eval_str("(hnsw-free __rv_idx)");
    TEST_ASSERT_EQ_I(hits, 20);
    PASS();
}

/* ============ Direct C-API coverage helpers ============ */

/* ray_hnsw_dim: the only public accessor that no existing test exercises. */
static test_result_t test_hnsw_dim_accessor(void) {
    /* Build a small 3-dim index via the C API directly. */
    float vecs[5 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 1.0f,
    };
    ray_hnsw_t* idx = ray_hnsw_build(vecs, 5, 3, RAY_HNSW_L2, 4, 50);
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_EQ_I(ray_hnsw_dim(idx), 3);
    /* NULL guard. */
    TEST_ASSERT_EQ_I(ray_hnsw_dim(NULL), 0);
    ray_hnsw_free(idx);
    PASS();
}

/* ray_hnsw_search_filter with accept=NULL falls through to plain search. */
static test_result_t test_hnsw_search_filter_null_accept(void) {
    float vecs[5 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 1.0f,
    };
    ray_hnsw_t* idx = ray_hnsw_build(vecs, 5, 3, RAY_HNSW_L2, 4, 50);
    TEST_ASSERT_NOT_NULL(idx);

    float q[3] = {1.0f, 0.0f, 0.0f};
    int64_t ids[3];
    double  dists[3];
    /* accept=NULL: delegates to ray_hnsw_search — must still return results. */
    int64_t n = ray_hnsw_search_filter(idx, q, 3, 3, 50, NULL, NULL, ids, dists);
    TEST_ASSERT_EQ_I(n, 3);
    TEST_ASSERT_EQ_I(ids[0], 0);
    TEST_ASSERT_EQ_F(dists[0], 0.0, 1e-6);

    ray_hnsw_free(idx);
    PASS();
}

/* ray_hnsw_mmap: exercised via the C API directly (not exposed as a builtin). */
static test_result_t test_hnsw_mmap_load(void) {
    const char* dir = "/tmp/ray_hnsw_mmap_test";
    /* Build & save a small index. */
    float vecs[5 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 1.0f,
    };
    ray_hnsw_t* idx = ray_hnsw_build(vecs, 5, 3, RAY_HNSW_COSINE, 4, 50);
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_EQ_I(ray_hnsw_save(idx, dir), RAY_OK);
    ray_hnsw_free(idx);

    /* Load via mmap path — currently both paths read into memory,
     * but the call itself is the untouched region. */
    ray_hnsw_t* loaded = ray_hnsw_mmap(dir);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQ_I(ray_hnsw_dim(loaded), 3);

    float q[3] = {1.0f, 0.0f, 0.0f};
    int64_t ids[1];
    double dists[1];
    TEST_ASSERT_EQ_I(ray_hnsw_search(loaded, q, 3, 1, 50, ids, dists), 1);
    TEST_ASSERT_EQ_I(ids[0], 0);

    ray_hnsw_free(loaded);
    PASS();
}

/* Regression: ray_hnsw_build must reject dimensions whose
 * n_nodes * dim * sizeof(float) overflows size_t, before it copies the source
 * vectors.  Values are chosen so the byte count wraps to 16: without the guard
 * the copy under-allocates and memcpy over-reads the 1-element source (ASan
 * traps it); with the guard the call returns NULL and the source is never
 * touched.  (2^62 + 2) * 2 * sizeof(float) == 2^65 + 16 == 16 (mod 2^64). */
static test_result_t test_hnsw_build_overflow_rejected(void) {
    float dummy[1] = { 0.0f };
    ray_hnsw_t* idx = ray_hnsw_build(dummy, ((int64_t)1 << 62) + 2, 2,
                                     RAY_HNSW_L2, 4, 50);
    TEST_ASSERT_NULL(idx);
    PASS();
}

/* Directly exercises the vectors-block overflow guard that hnsw_load_impl
 * applies to an untrusted header (factored into ray_hnsw_vec_size_valid).  A
 * crafted header can make n_nodes * dim * sizeof(float) wrap size_t, under-
 * allocating the vectors buffer that the following fread overruns; the guard
 * must reject it.  Unit-tested at this layer rather than through ray_hnsw_load
 * because a header patched to overflow is refused earlier — the huge
 * node-level read fails first — so a full-load test cannot distinguish this
 * path (with or without the guard the load returns NULL). */
static test_result_t test_hnsw_vec_size_valid_guard(void) {
    /* Ordinary dimensions fit. */
    TEST_ASSERT_TRUE(ray_hnsw_vec_size_valid(100000, 1536));
    /* Non-positive dims are rejected. */
    TEST_ASSERT_FALSE(ray_hnsw_vec_size_valid(0, 4));
    TEST_ASSERT_FALSE(ray_hnsw_vec_size_valid(4, 0));
    TEST_ASSERT_FALSE(ray_hnsw_vec_size_valid(-1, 4));
    /* n_nodes * dim * sizeof(float) overflows size_t → rejected. */
    TEST_ASSERT_FALSE(ray_hnsw_vec_size_valid((int64_t)1 << 40, 1 << 25));
    /* Boundary: the largest element count whose * sizeof(float) still fits,
     * then one dim past it. */
    int64_t max_elems = (int64_t)(SIZE_MAX / sizeof(float));
    TEST_ASSERT_TRUE(ray_hnsw_vec_size_valid(max_elems, 1));
    TEST_ASSERT_FALSE(ray_hnsw_vec_size_valid(max_elems, 2));
    PASS();
}

/* Trigger the maxheap_sift_down / results-replacement path in hnsw_search_layer.
 *
 * The replacement branch (lines 342-344) fires when:
 *   res_sz >= ef  AND  d < results[0].dist
 *
 * Strategy: build a large index with ef_construction=1.  During construction,
 * hnsw_search_layer is called with ef=1.  After the first candidate fills
 * the result heap (res_sz=1=ef), any neighbor that is closer than that one
 * result must enter via the sift-down replacement path.
 *
 * ef_construction=1 is intentionally aggressive (low quality index) but
 * fully legal — the test just verifies the code path is reached.
 * We additionally search with ef=1 to hit the same path at query time. */
static test_result_t test_hnsw_search_sift_down(void) {
    /* 200 random 4-D vectors — large enough that construction visits many
     * neighbors and the result-replacement branch fires at least once. */
    srand(99);
    const int N = 200, D = 4;
    float vecs[200 * 4];
    for (int i = 0; i < N * D; i++)
        vecs[i] = (float)rand() / (float)RAND_MAX - 0.5f;

    /* ef_construction=1: result heap saturates after 1 entry during build,
     * triggering sift_down whenever a better candidate arrives. */
    ray_hnsw_t* idx = ray_hnsw_build(vecs, N, D, RAY_HNSW_L2, 8, 1);
    TEST_ASSERT_NOT_NULL(idx);

    /* Also search with ef=1 so the same path fires at query time. */
    float q[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int64_t ids[1];
    double dists[1];
    int64_t n = ray_hnsw_search(idx, q, D, 1, 1, ids, dists);
    TEST_ASSERT_TRUE(n >= 1);

    ray_hnsw_free(idx);
    PASS();
}

/* ============ Suite table ============ */

/* ─── rerank coverage (S7) ───────────────────────────────── */

static test_result_t test_knn_i32_source_vecs(void) {
    const int N = 4, D = 3;
    ray_t* vlist = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(vlist);
    int32_t raw[3];
    /* row 0: [1,0,0], row 1: [0,1,0], row 2: [0,0,1], row 3: [1,1,0] */
    int32_t rows[4][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0}};
    for (int i = 0; i < N; i++) {
        raw[0] = rows[i][0]; raw[1] = rows[i][1]; raw[2] = rows[i][2];
        ray_t* v = ray_vec_from_raw(RAY_I32, raw, D);
        TEST_ASSERT_NOT_NULL(v);
        vlist = ray_list_append(vlist, v);
        ray_release(v);
    }
    TEST_ASSERT_EQ_I(vlist->len, N);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__i32vlist", 10), vlist), RAY_OK);
    ray_release(vlist);

    ray_eval_str(
        "(set __i32docs (table [id emb] "
        "  (list [0 1 2 3] __i32vlist)))");

    /* KNN over I32 vecs — rr_at_f64 RAY_I32 arm fires for each element. */
    ray_t* r = ray_eval_str(
        "(select {from: __i32docs nearest: (knn emb [1.0 0.0 0.0] 'l2) take: 2})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);
    /* Row 0 (id=0, [1,0,0]) is the exact match. */
    ray_t* id_col = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(id_col))[0], 0);
    ray_release(r);
    PASS();
}

/* rr_at_f64: I64 source vectors — hits the RAY_I64 arm. */
static test_result_t test_knn_i64_source_vecs(void) {
    const int N = 4, D = 3;
    ray_t* vlist = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(vlist);
    int64_t rows[4][3] = {{2,0,0},{0,2,0},{0,0,2},{2,2,0}};
    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_from_raw(RAY_I64, rows[i], D);
        TEST_ASSERT_NOT_NULL(v);
        vlist = ray_list_append(vlist, v);
        ray_release(v);
    }
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__i64vlist", 10), vlist), RAY_OK);
    ray_release(vlist);

    ray_eval_str(
        "(set __i64docs (table [id emb] "
        "  (list [0 1 2 3] __i64vlist)))");

    /* Query with a float [1,0,0]: row 0 ([2,0,0]) is closest by L2. */
    ray_t* r = ray_eval_str(
        "(select {from: __i64docs nearest: (knn emb [1.0 0.0 0.0] 'l2) take: 1})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 1);
    ray_t* id_col = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(id_col))[0], 0);
    ray_release(r);
    PASS();
}

/* exec_ann_rerank: empty source table — hits the src_rows==0 branch.
 * We need an empty table with an HNSW index built from the same embedding
 * shape, but the source table being passed to exec_ann_rerank has 0 rows. */
static test_result_t test_ann_rerank_empty_source(void) {
    /* Build an index from a small non-empty list so the index is valid,
     * then create a 0-row table (matching dim=3) for the select. */
    ray_eval_str("(set __ann_idx (hnsw-build "
                 "(list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0]) 'cosine 4 50))");

    /* Empty source: 0-row table with emb LIST column. */
    ray_eval_str(
        "(set __ann_empty (table [id emb] (list [] (list))))");

    ray_t* r = ray_eval_str(
        "(select {from: __ann_empty "
        "         nearest: (ann __ann_idx [1.0 0.0 0.0]) "
        "         take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);
    /* Source has 2 cols (id, emb); _dist not projected → schema unchanged. */
    TEST_ASSERT_EQ_I(ray_table_ncols(r), 2);
    ray_release(r);
    ray_eval_str("(hnsw-free __ann_idx)");
    PASS();
}

/* exec_ann_rerank: filter rejects all rows — hits accepted_count==0 branch. */
static test_result_t test_ann_rerank_filter_rejects_all(void) {
    ray_eval_str(
        "(set __afdocs (table [id score emb] "
        "  (list [0 1 2] "
        "        [0.1 0.2 0.3] "
        "        (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0]))))");
    ray_eval_str("(set __af_idx (hnsw-build (at __afdocs 'emb) 'cosine 4 50))");

    /* Filter rejects every row (score > 100) → accepted_count = 0. */
    ray_t* r = ray_eval_str(
        "(select {from: __afdocs where: (> score 100.0) "
        "         nearest: (ann __af_idx [1.0 0.0 0.0]) "
        "         take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);
    /* Source has 3 cols (id, score, emb); _dist not projected. */
    TEST_ASSERT_EQ_I(ray_table_ncols(r), 3);
    ray_release(r);
    ray_eval_str("(hnsw-free __af_idx)");
    PASS();
}

/* exec_knn_rerank identity scan: non-numeric row triggers type error.
 * The identity scan (no filter) iterates all rows; if any element is
 * not a numeric vector, exec_knn_rerank returns a type error. */
static test_result_t test_knn_rerank_identity_scan_bad_row(void) {
    /* Build a LIST where one element is a BOOL vector (not numeric). */
    ray_t* vlist = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(vlist);
    /* Good rows. */
    float r0[3] = {1.0f, 0.0f, 0.0f};
    float r1[3] = {0.0f, 1.0f, 0.0f};
    ray_t* v0 = ray_vec_from_raw(RAY_F32, r0, 3);
    ray_t* v1 = ray_vec_from_raw(RAY_F32, r1, 3);
    /* Bad row: BOOL vector — rr_is_numeric returns false. */
    uint8_t bdata[3] = {1, 0, 1};
    ray_t* vbad = ray_vec_from_raw(RAY_BOOL, bdata, 3);
    TEST_ASSERT_NOT_NULL(v0); TEST_ASSERT_NOT_NULL(v1); TEST_ASSERT_NOT_NULL(vbad);
    vlist = ray_list_append(vlist, v0); ray_release(v0);
    vlist = ray_list_append(vlist, v1); ray_release(v1);
    vlist = ray_list_append(vlist, vbad); ray_release(vbad);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__badvlist", 10), vlist), RAY_OK);
    ray_release(vlist);

    ray_eval_str(
        "(set __baddocs (table [id emb] "
        "  (list [0 1 2] __badvlist)))");

    /* Identity scan (no where clause) — should hit the type error in the else branch. */
    ray_t* r = ray_eval_str(
        "(select {from: __baddocs nearest: (knn emb [1.0 0.0 0.0]) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* exec_knn_rerank filtered scan: non-numeric row triggers type error.
 * The filtered scan (accepted rowid list) iterates selected rows;
 * if any accepted element is not a numeric vector, type error is returned. */
static test_result_t test_knn_rerank_filtered_scan_bad_row(void) {
    /* Build a LIST where the first row (which passes the filter) is bad. */
    ray_t* vlist2 = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(vlist2);
    uint8_t bdata2[3] = {0, 1, 0};
    ray_t* vbad2 = ray_vec_from_raw(RAY_BOOL, bdata2, 3);
    float r1[3] = {0.0f, 1.0f, 0.0f};
    float r2[3] = {1.0f, 0.0f, 0.0f};
    ray_t* vg1 = ray_vec_from_raw(RAY_F32, r1, 3);
    ray_t* vg2 = ray_vec_from_raw(RAY_F32, r2, 3);
    TEST_ASSERT_NOT_NULL(vbad2); TEST_ASSERT_NOT_NULL(vg1); TEST_ASSERT_NOT_NULL(vg2);
    vlist2 = ray_list_append(vlist2, vbad2); ray_release(vbad2);
    vlist2 = ray_list_append(vlist2, vg1);  ray_release(vg1);
    vlist2 = ray_list_append(vlist2, vg2);  ray_release(vg2);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__badvlist2", 11), vlist2), RAY_OK);
    ray_release(vlist2);

    /* score col: row 0 has score=1.0 (passes > 0.5), rows 1,2 have score=0.0. */
    ray_eval_str(
        "(set __baddocs2 (table [id score emb] "
        "  (list [0 1 2] [1.0 0.0 0.0] __badvlist2)))");

    /* Filter keeps row 0 (score > 0.5); row 0's emb is a BOOL vec → type error. */
    ray_t* r = ray_eval_str(
        "(select {from: __baddocs2 where: (> score 0.5) "
        "         nearest: (knn emb [1.0 0.0 0.0]) take: 2})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* gather_rows_with_dist: nullable column — hits src_has_nulls branch (lines 200-204).
 * Build a table with a nullable I64 id column (some nulls) and an emb LIST,
 * then run select nearest knn so gather_rows_with_dist encounters a column
 * with RAY_ATTR_HAS_NULLS set. */
static test_result_t test_knn_rerank_nullable_col_gather(void) {
    /* id col with a null at row 1: [0, 0N, 2, 3, 4] */
    ray_eval_str(
        "(set __nulldocs (table [id emb] "
        "  (list [0 0Nl 2 3 4] "
        "        (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0] "
        "              [1.0 1.0 0.0] [1.0 0.0 1.0]))))");

    /* Verify the id column actually has nulls (sanity check). */
    ray_t* tbl = ray_env_get(ray_sym_intern("__nulldocs", 10));
    TEST_ASSERT_NOT_NULL(tbl);
    ray_t* id_col = ray_table_get_col(tbl, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_TRUE((id_col->attrs & RAY_ATTR_HAS_NULLS) != 0);

    /* KNN query toward [0,1,0] (row 1 — the null row) — it must be selected.
     * This forces ray_vec_set_null to be called in gather_rows_with_dist. */
    ray_t* r = ray_eval_str(
        "(select {from: __nulldocs nearest: (knn emb [0.0 1.0 0.0] 'l2) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    /* Check that row 1 (the null-id row) is included in the results. */
    ray_t* id_col2 = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col2);
    /* At least one result should be null (the id for the nearest row). */
    bool found_null = false;
    for (int64_t i = 0; i < ray_table_nrows(r); i++) {
        if (ray_vec_is_null(id_col2, i)) { found_null = true; break; }
    }
    TEST_ASSERT_TRUE(found_null);
    ray_release(r);
    PASS();
}

/* rr_row_dist cosine with zero denom — hits the `denom <= 0.0` false branch.
 * A zero query vector has q_norm=0, making denom=0*anything=0 → sim=0.0. */
static test_result_t test_knn_rerank_zero_query_cosine(void) {
    build_docs();
    /* Zero query vector: q_norm=0 → denom=0 in cosine distance → sim=0. */
    ray_t* r = ray_eval_str(
        "(select {from: __docs nearest: (knn emb [0.0 0.0 0.0] 'cosine) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    /* All rows get distance 1.0 (1 - 0.0 = 1.0); any 3 rows are valid. */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    ray_release(r);
    PASS();
}

/* rr_heap_insert: right-child wins in the percolate-down loop.
 * With k=3 and vectors at distances [3.0, 2.0, 2.5, 1.0] (L2 from origin),
 * inserting 1.0 replaces the root (3.0); left child is 2.0, right child is 2.5
 * → right child wins (best=r), covering line 303's true branch. */
static test_result_t test_knn_rerank_heap_right_child(void) {
    /* Vectors at L2 distances from [0,0]: [3,0]=3.0, [2,0]=2.0, [2.5,0]=2.5, [1,0]=1.0. */
    ray_eval_str(
        "(set __heapdocs (table [id emb] "
        "  (list [0 1 2 3] "
        "        (list [3.0 0.0] [2.0 0.0] [2.5 0.0] [1.0 0.0]))))");

    /* Take top-3 nearest to [0,0] by L2 → heap fill order triggers right-child win. */
    ray_t* r = ray_eval_str(
        "(select {from: __heapdocs nearest: (knn emb [0.0 0.0] 'l2) take: 3})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    /* Closest 3 by ascending distance: id=3 (d=1.0), id=1 (d=2.0), id=2 (d=2.5). */
    ray_t* id_col = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(id_col))[0], 3);
    ray_release(r);
    PASS();
}

/* exec_knn_rerank F64 source vectors — hits the RAY_F64 arm in rr_at_f64. */
static test_result_t test_knn_f64_source_vecs(void) {
    const int N = 3, D = 3;
    ray_t* vlist = ray_list_new(N);
    TEST_ASSERT_NOT_NULL(vlist);
    double rows[3][3] = {{1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0}};
    for (int i = 0; i < N; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F64, rows[i], D);
        TEST_ASSERT_NOT_NULL(v);
        vlist = ray_list_append(vlist, v);
        ray_release(v);
    }
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("__f64vlist", 10), vlist), RAY_OK);
    ray_release(vlist);

    ray_eval_str(
        "(set __f64docs (table [id emb] "
        "  (list [0 1 2] __f64vlist)))");

    ray_t* r = ray_eval_str(
        "(select {from: __f64docs nearest: (knn emb [1.0 0.0 0.0] 'cosine) take: 1})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 1);
    ray_t* id_col = ray_table_get_col(r, ray_sym_intern("id", 2));
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(id_col))[0], 0);
    ray_release(r);
    PASS();
}

/* ============ Branch-coverage tests (S8) ============ */

/* vec_binary_metric NULL-arg guards: !a and !b. */
static test_result_t test_cos_dist_null_args(void) {
    /* NULL first arg → type error. */
    ray_t* r = ray_cos_dist_fn(NULL, NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* NULL only first arg. */
    float f[2] = {1.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 2);
    TEST_ASSERT_NOT_NULL(v);
    r = ray_cos_dist_fn(NULL, v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    r = ray_cos_dist_fn(v, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(v);

    /* Same for l2-dist and inner-prod. */
    r = ray_l2_dist_fn(NULL, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    r = ray_inner_prod_fn(NULL, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ray_norm_fn with NULL input → type error. */
static test_result_t test_norm_null(void) {
    ray_t* r = ray_norm_fn(NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ray_knn_fn direct C-API: rank errors (too few / too many args). */
static test_result_t test_knn_rank_errors(void) {
    float f[3] = {1.0f, 0.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 3);
    TEST_ASSERT_NOT_NULL(v);
    ray_t* col = ray_list_new(1);
    col = ray_list_append(col, v);

    /* Too few: n=2 (need 3-4). */
    ray_t* args2[2] = { col, v };
    ray_t* r = ray_knn_fn(args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Too many: n=5 (max 4). */
    ray_t* katom = make_i64(1);
    ray_t* args5[5] = { col, v, katom, NULL, NULL };
    r = ray_knn_fn(args5, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    ray_release(col);
    ray_release(katom);
    PASS();
}

/* ray_knn_fn: col is NULL or not a list → type error. */
static test_result_t test_knn_col_type_errors(void) {
    float f[3] = {1.0f, 0.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 3);
    ray_t* katom = make_i64(1);

    /* col is NULL. */
    ray_t* args_null[3] = { NULL, v, katom };
    ray_t* r = ray_knn_fn(args_null, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* col is not a list (a scalar). */
    ray_t* scalar = make_i64(42);
    ray_t* args_scalar[3] = { scalar, v, katom };
    r = ray_knn_fn(args_scalar, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    ray_release(katom);
    ray_release(scalar);
    PASS();
}

/* ray_knn_fn: query not numeric → type error. */
static test_result_t test_knn_query_type_error(void) {
    float f[3] = {1.0f, 0.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 3);
    ray_t* col = ray_list_new(1);
    col = ray_list_append(col, v);
    ray_t* katom = make_i64(1);

    /* query is a scalar, not a vec. */
    ray_t* bad_query = make_i64(5);
    ray_t* args[3] = { col, bad_query, katom };
    ray_t* r = ray_knn_fn(args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    ray_release(col);
    ray_release(katom);
    ray_release(bad_query);
    PASS();
}

/* ray_knn_fn: k atom is not int → type error. */
static test_result_t test_knn_k_type_error(void) {
    float f[3] = {1.0f, 0.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 3);
    ray_t* col = ray_list_new(1);
    col = ray_list_append(col, v);

    /* k is a float, not int. */
    ray_t* kfloat = make_f64(1.5);
    ray_t* args[3] = { col, v, kfloat };
    ray_t* r = ray_knn_fn(args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    ray_release(col);
    ray_release(kfloat);
    PASS();
}

/* ray_hnsw_build_fn direct C-API: rank errors. */
static test_result_t test_hnsw_build_rank_errors(void) {
    /* n=0: too few. */
    ray_t* r = ray_hnsw_build_fn(NULL, 0);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* n=5: too many. */
    ray_t* args[5] = { NULL, NULL, NULL, NULL, NULL };
    r = ray_hnsw_build_fn(args, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ray_hnsw_build_fn: col is NULL → type error. */
static test_result_t test_hnsw_build_col_null(void) {
    ray_t* args[1] = { NULL };
    ray_t* r = ray_hnsw_build_fn(args, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ray_ann_fn rank errors. */
static test_result_t test_ann_rank_errors(void) {
    ray_t* r = ray_ann_fn(NULL, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_t* args[5] = { NULL, NULL, NULL, NULL, NULL };
    r = ray_ann_fn(args, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* ray_ann_fn: handle is not HNSW → type error. */
static test_result_t test_ann_handle_type_error(void) {
    ray_t* bad = make_i64(42);
    float f[3] = {1.0f, 0.0f, 0.0f};
    ray_t* v = ray_vec_from_raw(RAY_F32, f, 3);
    ray_t* katom = make_i64(1);
    ray_t* args[3] = { bad, v, katom };
    ray_t* r = ray_ann_fn(args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(bad);
    ray_release(v);
    ray_release(katom);
    PASS();
}

/* ray_ann_fn: query not numeric → type error. */
static test_result_t test_ann_query_type_error(void) {
    /* Build a real index to get past the handle check. */
    float vecs[3 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    ray_t* col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F32, vecs + i * 3, 3);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    ray_t* build_args[2];
    build_args[0] = col;
    int64_t cos_sym = ray_sym_intern("l2", 2);
    ray_t* metric_atom = ray_sym(cos_sym);
    build_args[1] = metric_atom;
    ray_t* handle = ray_hnsw_build_fn(build_args, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(handle));

    /* Query with a non-numeric (scalar I64). */
    ray_t* bad_query = make_i64(42);
    ray_t* katom = make_i64(1);
    ray_t* ann_args[3] = { handle, bad_query, katom };
    ray_t* r = ray_ann_fn(ann_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Query with wrong dimension. */
    float f2[2] = {1.0f, 0.0f};
    ray_t* short_v = ray_vec_from_raw(RAY_F32, f2, 2);
    ann_args[1] = short_v;
    r = ray_ann_fn(ann_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* k is not int → type error. */
    float f3[3] = {1.0f, 0.0f, 0.0f};
    ray_t* good_q = ray_vec_from_raw(RAY_F32, f3, 3);
    ray_t* kfloat = make_f64(1.5);
    ray_t* ann_args2[3] = { handle, good_q, kfloat };
    r = ray_ann_fn(ann_args2, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* k <= 0 → domain error. */
    ray_t* k0 = make_i64(0);
    ray_t* ann_args3[3] = { handle, good_q, k0 };
    r = ray_ann_fn(ann_args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_hnsw_free_fn(handle);
    ray_release(col);
    ray_release(metric_atom);
    ray_release(handle);
    ray_release(bad_query);
    ray_release(katom);
    ray_release(short_v);
    ray_release(good_q);
    ray_release(kfloat);
    ray_release(k0);
    PASS();
}

/* ray_hnsw_free_fn: non-handle → type error. */
static test_result_t test_hnsw_free_type_errors(void) {
    ray_t* r = ray_hnsw_free_fn(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_t* s = make_i64(42);
    r = ray_hnsw_free_fn(s);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(s);
    PASS();
}

/* ray_hnsw_save_fn: various error paths. */
static test_result_t test_hnsw_save_type_errors(void) {
    /* NULL handle → type error. */
    ray_t* path = ray_eval_str("\"some_path\"");
    ray_t* r = ray_hnsw_save_fn(NULL, path);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Non-handle → type error. */
    ray_t* scalar = make_i64(42);
    r = ray_hnsw_save_fn(scalar, path);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Valid handle, NULL path → type error. */
    float vecs[3] = {1.0f, 0.0f, 0.0f};
    ray_t* col = ray_list_new(1);
    ray_t* v = ray_vec_from_raw(RAY_F32, vecs, 3);
    col = ray_list_append(col, v);
    ray_t* build_args[1] = { col };
    ray_t* handle = ray_hnsw_build_fn(build_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(handle));

    r = ray_hnsw_save_fn(handle, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* Valid handle, non-string path → type error. */
    r = ray_hnsw_save_fn(handle, scalar);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_hnsw_free_fn(handle);
    ray_release(v);
    ray_release(col);
    ray_release(handle);
    ray_release(path);
    ray_release(scalar);
    PASS();
}

/* ray_hnsw_load_fn: various error paths. */
static test_result_t test_hnsw_load_type_errors(void) {
    /* NULL → type error. */
    ray_t* r = ray_hnsw_load_fn(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* Non-string → type error. */
    ray_t* scalar = make_i64(42);
    r = ray_hnsw_load_fn(scalar);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(scalar);
    PASS();
}

/* ray_hnsw_info_fn: non-handle → type error. */
static test_result_t test_hnsw_info_type_errors(void) {
    ray_t* r = ray_hnsw_info_fn(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_t* scalar = make_i64(42);
    r = ray_hnsw_info_fn(scalar);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(scalar);
    PASS();
}

/* knn with k as I32 and I16 atoms (atom_to_i64 non-I64 branches). */
static test_result_t test_knn_k_i32_i16(void) {
    /* Build col via eval. */
    ray_eval_str("(set __k_test_col (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0]))");
    /* k as I32 (1i suffix). */
    TEST_ASSERT_EQ_I(eval_i64("(first (at (knn __k_test_col [1.0 0.0 0.0] 1i 'l2) '_rowid))"), 0);
    /* k as I16 (1h suffix). */
    TEST_ASSERT_EQ_I(eval_i64("(first (at (knn __k_test_col [1.0 0.0 0.0] 1h 'l2) '_rowid))"), 0);
    PASS();
}

/* hnsw-build with I32/I64/F64 source vecs (rayvec_to_floats non-F32 path). */
static test_result_t test_hnsw_build_non_f32_source(void) {
    /* F64 vectors → rayvec_to_floats takes the loop path. */
    double f64_vecs[3][3] = {{1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0}};
    ray_t* col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F64, f64_vecs[i], 3);
        TEST_ASSERT_NOT_NULL(v);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    ray_t* build_args[1] = { col };
    ray_t* handle = ray_hnsw_build_fn(build_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(handle));
    ray_hnsw_free_fn(handle);
    ray_release(handle);
    ray_release(col);

    /* I32 vectors. */
    int32_t i32_vecs[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_I32, i32_vecs[i], 3);
        TEST_ASSERT_NOT_NULL(v);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    build_args[0] = col;
    handle = ray_hnsw_build_fn(build_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(handle));
    ray_hnsw_free_fn(handle);
    ray_release(handle);
    ray_release(col);

    /* I64 vectors. */
    int64_t i64_vecs[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_I64, i64_vecs[i], 3);
        TEST_ASSERT_NOT_NULL(v);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    build_args[0] = col;
    handle = ray_hnsw_build_fn(build_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(handle));
    ray_hnsw_free_fn(handle);
    ray_release(handle);
    ray_release(col);
    PASS();
}

/* hnsw-build M out-of-range defaults: M=0 and M=999. */
static test_result_t test_hnsw_build_m_out_of_range(void) {
    float vecs[3 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    ray_t* col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F32, vecs + i * 3, 3);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    int64_t l2_sym = ray_sym_intern("l2", 2);
    ray_t* metric_atom = ray_sym(l2_sym);

    /* M=0 → out-of-range, keeps default. */
    ray_t* m0 = make_i64(0);
    ray_t* args3[3] = { col, metric_atom, m0 };
    ray_t* h = ray_hnsw_build_fn(args3, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    ray_hnsw_free_fn(h);
    ray_release(h);

    /* M=999 → out-of-range (> 512), keeps default. */
    ray_t* m999 = make_i64(999);
    args3[2] = m999;
    h = ray_hnsw_build_fn(args3, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    ray_hnsw_free_fn(h);
    ray_release(h);

    /* M is not int → type error. */
    ray_t* mfloat = make_f64(8.0);
    args3[2] = mfloat;
    ray_t* r = ray_hnsw_build_fn(args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(col);
    ray_release(metric_atom);
    ray_release(m0);
    ray_release(m999);
    ray_release(mfloat);
    PASS();
}

/* hnsw-build ef_c out-of-range defaults: ef_c=0 and ef_c=9999. */
static test_result_t test_hnsw_build_efc_out_of_range(void) {
    float vecs[3 * 3] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    ray_t* col = ray_list_new(3);
    for (int i = 0; i < 3; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F32, vecs + i * 3, 3);
        col = ray_list_append(col, v);
        ray_release(v);
    }
    int64_t l2_sym = ray_sym_intern("l2", 2);
    ray_t* metric_atom = ray_sym(l2_sym);
    ray_t* m8 = make_i64(8);

    /* ef_c=0 → out-of-range. */
    ray_t* ef0 = make_i64(0);
    ray_t* args4[4] = { col, metric_atom, m8, ef0 };
    ray_t* h = ray_hnsw_build_fn(args4, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    ray_hnsw_free_fn(h);
    ray_release(h);

    /* ef_c=9999 → out-of-range (> 4096). */
    ray_t* ef9999 = make_i64(9999);
    args4[3] = ef9999;
    h = ray_hnsw_build_fn(args4, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    ray_hnsw_free_fn(h);
    ray_release(h);

    /* ef_c is not int → type error. */
    ray_t* effloat = make_f64(50.0);
    args4[3] = effloat;
    ray_t* r = ray_hnsw_build_fn(args4, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(col);
    ray_release(metric_atom);
    ray_release(m8);
    ray_release(ef0);
    ray_release(ef9999);
    ray_release(effloat);
    PASS();
}

/* list_vec_validate return 3: mixed-dimension vectors in LIST. */
static test_result_t test_list_validate_dim_mismatch(void) {
    /* Build LIST with dim-mismatch: [1,0,0] and [1,0]. */
    ray_t* r = ray_eval_str("(cos-dist (list [1.0 0.0 0.0] [1.0 0.0]) [1.0 0.0 0.0])");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* Same for knn. */
    r = ray_eval_str("(knn (list [1.0 0.0 0.0] [1.0 0.0]) [1.0 0.0 0.0] 1)");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* Same for hnsw-build. */
    r = ray_eval_str("(hnsw-build (list [1.0 0.0 0.0] [1.0 0.0]))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* list_vec_validate return 2: first element non-numeric. */
static test_result_t test_list_validate_first_bad(void) {
    ray_t* r = ray_eval_str("(cos-dist (list (as 'BOOL [true false]) [1.0 0.0]) [1.0 0.0])");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    /* norm on list with bad first element. */
    r = ray_eval_str("(norm (list (as 'BOOL [true false]) [1.0 0.0]))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* knn empty column with zero-dim query → empty result table. */
static test_result_t test_knn_empty_column(void) {
    ray_t* r = ray_eval_str("(count (knn (list) (as 'F64 []) 1))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    PASS();
}

/* cos-dist with zero-denom: zero row vector → denom == 0, sim=0, dist=1. */
static test_result_t test_cos_dist_zero_denom(void) {
    /* Zero row vector. */
    TEST_ASSERT_EQ_F(eval_f64("(cos-dist [0.0 0.0] [1.0 0.0])"), 1.0, 1e-6);
    /* Both zero vectors. */
    TEST_ASSERT_EQ_F(eval_f64("(cos-dist [0.0 0.0] [0.0 0.0])"), 1.0, 1e-6);
    /* LIST with zero row. */
    TEST_ASSERT_EQ_F(eval_f64("(at (cos-dist (list [0.0 0.0] [1.0 0.0]) [1.0 0.0]) 0)"), 1.0, 1e-6);
    PASS();
}

/* knn with IP metric: row_score dispatches through RAY_HNSW_IP → negated dot. */
static test_result_t test_knn_ip_negated_distance(void) {
    double d = eval_f64("(first (at (knn (list [1.0 0.0 0.0] [0.0 1.0 0.0]) [1.0 0.0 0.0] 1 'ip) '_dist))");
    TEST_ASSERT_EQ_F(d, -1.0, 1e-6);
    PASS();
}

/* vec_binary_metric scalar path: a->len <= 0 → length error. */
static test_result_t test_metric_scalar_empty_vec(void) {
    ray_t* r = ray_eval_str("(cos-dist (as 'F32 []) (as 'F32 []))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    r = ray_eval_str("(l2-dist (as 'F32 []) (as 'F32 []))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    r = ray_eval_str("(inner-prod (as 'F32 []) (as 'F32 []))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* hnsw-build on empty list → dim <= 0 → length error. */
static test_result_t test_hnsw_build_empty_list(void) {
    ray_t* r = ray_eval_str("(hnsw-build (list))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    PASS();
}

/* knn: k > nrows clamping and insertion sort on ascending output. */
static test_result_t test_knn_k_clamp_and_sort(void) {
    /* 3 rows, k=100 → clamped to 3. */
    TEST_ASSERT_EQ_I(eval_i64("(count (knn (list [1.0 0.0] [0.0 1.0] [1.0 1.0]) [1.0 0.0] 100 'l2))"), 3);
    /* Verify ascending sort. */
    double d0 = eval_f64("(first (at (knn (list [5.0 0.0] [3.0 0.0] [1.0 0.0]) [0.0 0.0] 3 'l2) '_dist))");
    double d2 = eval_f64("(last (at (knn (list [5.0 0.0] [3.0 0.0] [1.0 0.0]) [0.0 0.0] 3 'l2) '_dist))");
    TEST_ASSERT_EQ_F(d0, 1.0, 1e-6);
    TEST_ASSERT_EQ_F(d2, 5.0, 1e-6);
    PASS();
}

/* ann: ef_s out-of-range keeps computed ef. */
static test_result_t test_ann_efs_out_of_range(void) {
    ray_eval_str("(set __efs_idx (hnsw-build (list [1.0 0.0 0.0] [0.0 1.0 0.0] [0.0 0.0 1.0]) 'l2))");
    /* ef_s=0 → keeps default. */
    TEST_ASSERT_EQ_I(eval_i64("(count (ann __efs_idx [1.0 0.0 0.0] 2 0))"), 2);
    /* ef_s=9999 → v > 4096, keeps computed ef. */
    TEST_ASSERT_EQ_I(eval_i64("(count (ann __efs_idx [1.0 0.0 0.0] 2 9999))"), 2);
    /* ef_s as non-int → type error. */
    ray_t* r = ray_eval_str("(ann __efs_idx [1.0 0.0 0.0] 2 1.5)");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_eval_str("(hnsw-free __efs_idx)");
    PASS();
}

/* hnsw-info for each metric to cover the switch. */
static test_result_t test_hnsw_info_all_metrics(void) {
    /* Cosine → "cosine" */
    ray_eval_str("(set __info_cos (hnsw-build (list [1.0 0.0 0.0] [0.0 1.0 0.0]) 'cosine))");
    ray_t* r = ray_eval_str("(== (at (hnsw-info __info_cos) 'metric) 'cosine)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(r->b8, 1);
    ray_release(r);
    ray_eval_str("(hnsw-free __info_cos)");

    /* L2 → "l2" */
    ray_eval_str("(set __info_l2 (hnsw-build (list [1.0 0.0 0.0] [0.0 1.0 0.0]) 'l2))");
    r = ray_eval_str("(== (at (hnsw-info __info_l2) 'metric) 'l2)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(r->b8, 1);
    ray_release(r);
    ray_eval_str("(hnsw-free __info_l2)");

    /* IP → "ip" */
    ray_eval_str("(set __info_ip (hnsw-build (list [1.0 0.0 0.0] [0.0 1.0 0.0]) 'ip))");
    r = ray_eval_str("(== (at (hnsw-info __info_ip) 'metric) 'ip)");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(r->b8, 1);
    ray_release(r);
    ray_eval_str("(hnsw-free __info_ip)");
    PASS();
}

/* knn heap sift patterns: exercises both heap-up break and sift-down paths
 * with a variety of k values and distance orderings. */
static test_result_t test_knn_heap_patterns(void) {
    /* 6 rows at distances 6,5,4,3,2,1 from origin. k=3.
     * First 3 fill heap: [6,5,4] (max-heap root=6).
     * Row 3 (d=3): 3 < 6 → replace root, sift-down.
     * Row 4 (d=2): 2 < 5 (new root after sift) → replace, sift-down.
     * Row 5 (d=1): 1 < 4 (new root) → replace, sift-down.
     * Final sorted: 1,2,3. */
    TEST_ASSERT_EQ_F(eval_f64("(first (at (knn (list [6.0 0.0] [5.0 0.0] [4.0 0.0] [3.0 0.0] [2.0 0.0] [1.0 0.0]) [0.0 0.0] 3 'l2) '_dist))"), 1.0, 1e-6);
    TEST_ASSERT_EQ_F(eval_f64("(last  (at (knn (list [6.0 0.0] [5.0 0.0] [4.0 0.0] [3.0 0.0] [2.0 0.0] [1.0 0.0]) [0.0 0.0] 3 'l2) '_dist))"), 3.0, 1e-6);

    /* k=1: first entry fills, subsequent replacements exercise sift-down
     * in the degenerate single-element heap (best == j, breaks immediately). */
    TEST_ASSERT_EQ_F(eval_f64("(first (at (knn (list [6.0 0.0] [5.0 0.0] [1.0 0.0]) [0.0 0.0] 1 'l2) '_dist))"), 1.0, 1e-6);

    /* All ties: heap fills but no replacements needed (d >= heap[0].d fails). */
    TEST_ASSERT_EQ_F(eval_f64("(first (at (knn (list [1.0 0.0] [0.0 1.0] [-1.0 0.0] [0.0 -1.0]) [0.0 0.0] 4 'l2) '_dist))"), 1.0, 1e-6);
    PASS();
}

/* ============ Suite table ============ */

const test_entry_t embedding_entries[] = {
    { "embedding/cos_dist_scalar", test_cos_dist_scalar, emb_setup, emb_teardown },
    { "embedding/l2_dist_scalar", test_l2_dist_scalar, emb_setup, emb_teardown },
    { "embedding/inner_prod_scalar", test_inner_prod_scalar, emb_setup, emb_teardown },
    { "embedding/norm_scalar", test_norm_scalar, emb_setup, emb_teardown },
    { "embedding/metric_length_err", test_metric_length_mismatch, emb_setup, emb_teardown },
    { "embedding/metric_type_err", test_metric_type_error, emb_setup, emb_teardown },
    { "embedding/cos_dist_list", test_cos_dist_list, emb_setup, emb_teardown },
    { "embedding/l2_dist_list", test_l2_dist_list, emb_setup, emb_teardown },
    { "embedding/inner_prod_list", test_inner_prod_list, emb_setup, emb_teardown },
    { "embedding/norm_list", test_norm_list, emb_setup, emb_teardown },
    { "embedding/knn_cosine", test_knn_cosine, emb_setup, emb_teardown },
    { "embedding/knn_l2", test_knn_l2, emb_setup, emb_teardown },
    { "embedding/knn_ip", test_knn_ip, emb_setup, emb_teardown },
    { "embedding/hnsw_info_metric", test_hnsw_info_and_metric, emb_setup, emb_teardown },
    { "embedding/ann_topk", test_ann_topk, emb_setup, emb_teardown },
    { "embedding/ann_each_metric", test_ann_each_metric, emb_setup, emb_teardown },
    { "embedding/ann_recall", test_ann_recall, emb_setup, emb_teardown },
    { "embedding/ann_recall_saturated", test_ann_recall_saturated, emb_setup, emb_teardown },
    { "embedding/hnsw_save_load", test_hnsw_save_load, emb_setup, emb_teardown },
    { "embedding/hnsw_free_twice", test_hnsw_free_twice, emb_setup, emb_teardown },
    { "embedding/hnsw_rebind_no_leak", test_hnsw_rebind_no_leak, emb_setup, emb_teardown },
    { "embedding/hnsw_inline_no_leak", test_hnsw_inline_no_leak, emb_setup, emb_teardown },
    { "embedding/hnsw_teardown_no_leak", test_hnsw_teardown_no_leak, emb_setup, emb_teardown },
    { "embedding/hnsw_handle_cow", test_hnsw_handle_cow, emb_setup, emb_teardown },
    { "embedding/select_nearest_knn", test_select_nearest_knn, emb_setup, emb_teardown },
    { "embedding/select_nearest_ann", test_select_nearest_ann, emb_setup, emb_teardown },
    { "embedding/select_nearest_filter", test_select_nearest_filter, emb_setup, emb_teardown },
    { "embedding/select_nearest_empty_filter", test_select_nearest_empty_filter, emb_setup, emb_teardown },
    { "embedding/select_nearest_take_default", test_select_nearest_take_default, emb_setup, emb_teardown },
    { "embedding/select_nearest_asc_rejected", test_select_nearest_asc_rejected, emb_setup, emb_teardown },
    { "embedding/select_nearest_implicit_no_dist", test_select_nearest_implicit_no_dist, emb_setup, emb_teardown },
    { "embedding/select_nearest_empty_source", test_select_nearest_empty_source, emb_setup, emb_teardown },
    { "embedding/select_nearest_str_col", test_select_nearest_str_col, emb_setup, emb_teardown },
    { "embedding/select_nearest_sym_col", test_select_nearest_sym_col, emb_setup, emb_teardown },
    { "embedding/select_nearest_ann_inline_handle", test_select_nearest_ann_inline_handle, emb_setup, emb_teardown },
    { "embedding/select_nearest_ann_freed_handle", test_select_nearest_ann_freed_handle, emb_setup, emb_teardown },
    { "embedding/select_nearest_ann_projection_error", test_select_nearest_ann_projection_error, emb_setup, emb_teardown },
    { "embedding/select_nearest_iterative_selective", test_select_nearest_iterative_selective, emb_setup, emb_teardown },
    { "embedding/select_nearest_recall", test_select_nearest_recall, emb_setup, emb_teardown },
    { "embedding/hnsw_dim_accessor", test_hnsw_dim_accessor, emb_setup, emb_teardown },
    { "embedding/hnsw_search_filter_null_accept", test_hnsw_search_filter_null_accept, emb_setup, emb_teardown },
    { "embedding/hnsw_mmap_load", test_hnsw_mmap_load, emb_setup, emb_teardown },
    { "embedding/hnsw_build_overflow_rejected", test_hnsw_build_overflow_rejected, emb_setup, emb_teardown },
    { "embedding/hnsw_vec_size_valid_guard", test_hnsw_vec_size_valid_guard, emb_setup, emb_teardown },
    { "embedding/hnsw_search_sift_down", test_hnsw_search_sift_down, emb_setup, emb_teardown },

    /* rerank coverage (S7) */
    { "embedding/rerank_knn_i32_source", test_knn_i32_source_vecs, emb_setup, emb_teardown },
    { "embedding/rerank_knn_i64_source", test_knn_i64_source_vecs, emb_setup, emb_teardown },
    { "embedding/rerank_knn_f64_source", test_knn_f64_source_vecs, emb_setup, emb_teardown },
    { "embedding/rerank_ann_empty_source", test_ann_rerank_empty_source, emb_setup, emb_teardown },
    { "embedding/rerank_ann_filter_rejects_all", test_ann_rerank_filter_rejects_all, emb_setup, emb_teardown },
    { "embedding/rerank_knn_identity_scan_bad_row", test_knn_rerank_identity_scan_bad_row, emb_setup, emb_teardown },
    { "embedding/rerank_knn_filtered_scan_bad_row", test_knn_rerank_filtered_scan_bad_row, emb_setup, emb_teardown },
    { "embedding/rerank_knn_nullable_col_gather", test_knn_rerank_nullable_col_gather, emb_setup, emb_teardown },
    { "embedding/rerank_knn_heap_right_child", test_knn_rerank_heap_right_child, emb_setup, emb_teardown },
    { "embedding/rerank_knn_zero_query_cosine", test_knn_rerank_zero_query_cosine, emb_setup, emb_teardown },

    /* branch coverage (S8) */
    { "embedding/cos_dist_null_args", test_cos_dist_null_args, emb_setup, emb_teardown },
    { "embedding/norm_null", test_norm_null, emb_setup, emb_teardown },
    { "embedding/knn_rank_errors", test_knn_rank_errors, emb_setup, emb_teardown },
    { "embedding/knn_col_type_errors", test_knn_col_type_errors, emb_setup, emb_teardown },
    { "embedding/knn_query_type_error", test_knn_query_type_error, emb_setup, emb_teardown },
    { "embedding/knn_k_type_error", test_knn_k_type_error, emb_setup, emb_teardown },
    { "embedding/hnsw_build_rank_errors", test_hnsw_build_rank_errors, emb_setup, emb_teardown },
    { "embedding/hnsw_build_col_null", test_hnsw_build_col_null, emb_setup, emb_teardown },
    { "embedding/ann_rank_errors", test_ann_rank_errors, emb_setup, emb_teardown },
    { "embedding/ann_handle_type_error", test_ann_handle_type_error, emb_setup, emb_teardown },
    { "embedding/ann_query_type_error", test_ann_query_type_error, emb_setup, emb_teardown },
    { "embedding/hnsw_free_type_errors", test_hnsw_free_type_errors, emb_setup, emb_teardown },
    { "embedding/hnsw_save_type_errors", test_hnsw_save_type_errors, emb_setup, emb_teardown },
    { "embedding/hnsw_load_type_errors", test_hnsw_load_type_errors, emb_setup, emb_teardown },
    { "embedding/hnsw_info_type_errors", test_hnsw_info_type_errors, emb_setup, emb_teardown },
    { "embedding/knn_k_i32_i16", test_knn_k_i32_i16, emb_setup, emb_teardown },
    { "embedding/hnsw_build_non_f32_source", test_hnsw_build_non_f32_source, emb_setup, emb_teardown },
    { "embedding/hnsw_build_m_out_of_range", test_hnsw_build_m_out_of_range, emb_setup, emb_teardown },
    { "embedding/hnsw_build_efc_out_of_range", test_hnsw_build_efc_out_of_range, emb_setup, emb_teardown },
    { "embedding/list_validate_dim_mismatch", test_list_validate_dim_mismatch, emb_setup, emb_teardown },
    { "embedding/list_validate_first_bad", test_list_validate_first_bad, emb_setup, emb_teardown },
    { "embedding/knn_empty_column", test_knn_empty_column, emb_setup, emb_teardown },
    { "embedding/cos_dist_zero_denom", test_cos_dist_zero_denom, emb_setup, emb_teardown },
    { "embedding/knn_ip_negated_distance", test_knn_ip_negated_distance, emb_setup, emb_teardown },
    { "embedding/metric_scalar_empty_vec", test_metric_scalar_empty_vec, emb_setup, emb_teardown },
    { "embedding/hnsw_build_empty_list", test_hnsw_build_empty_list, emb_setup, emb_teardown },
    { "embedding/knn_k_clamp_and_sort", test_knn_k_clamp_and_sort, emb_setup, emb_teardown },
    { "embedding/ann_efs_out_of_range", test_ann_efs_out_of_range, emb_setup, emb_teardown },
    { "embedding/hnsw_info_all_metrics", test_hnsw_info_all_metrics, emb_setup, emb_teardown },
    { "embedding/knn_heap_patterns", test_knn_heap_patterns, emb_setup, emb_teardown },

    { NULL, NULL, NULL, NULL },
};



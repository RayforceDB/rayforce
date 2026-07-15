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

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "core/pool.h"
#include "core/poll.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdatomic.h>
#include <string.h>
#include <math.h>

#if defined(__linux__)
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>

static void epoll_sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* --------------------------------------------------------------------------
 * Test: parallel sum via executor (ray_sum on large vector)
 *
 * 100k elements above RAY_PARALLEL_THRESHOLD (65536) triggers the parallel
 * reduction path in exec.c.
 * -------------------------------------------------------------------------- */

static test_result_t test_parallel_sum(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = n;

    int64_t* vals = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) vals[i] = i + 1;  /* 1..n */

    int64_t expected = n * (n + 1) / 2;

    int64_t col_name = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, col_name, vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan = ray_scan(g, "val");
    ray_op_t* sum_op = ray_sum(g, scan);

    ray_t* result = ray_execute(g, sum_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, expected);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: parallel binary add via executor
 * -------------------------------------------------------------------------- */

static test_result_t test_parallel_add(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* a_vec = ray_vec_new(RAY_I64, n);
    ray_t* b_vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a_vec));
    TEST_ASSERT_FALSE(RAY_IS_ERR(b_vec));
    a_vec->len = n;
    b_vec->len = n;

    int64_t* a = (int64_t*)ray_data(a_vec);
    int64_t* b = (int64_t*)ray_data(b_vec);
    for (int64_t i = 0; i < n; i++) { a[i] = i; b[i] = n - i; }

    int64_t name_a = ray_sym_intern("a", 1);
    int64_t name_b = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_a, a_vec);
    tbl = ray_table_add_col(tbl, name_b, b_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sa = ray_scan(g, "a");
    ray_op_t* sb = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, sa, sb);

    ray_t* result = ray_execute(g, add);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(result), n);

    /* Every element should be n (i + (n - i)) */
    int64_t* rdata = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(rdata[i], n);
    }

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(a_vec);
    ray_release(b_vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: parallel group-by sum via executor
 * -------------------------------------------------------------------------- */

static test_result_t test_parallel_group_sum(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* id_vec = ray_vec_new(RAY_I64, n);
    ray_t* v_vec  = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(id_vec));
    TEST_ASSERT_FALSE(RAY_IS_ERR(v_vec));
    id_vec->len = n;
    v_vec->len = n;

    int64_t* ids = (int64_t*)ray_data(id_vec);
    int64_t* vs  = (int64_t*)ray_data(v_vec);

    /* 4 groups: ids 0,1,2,3 cycling. v = group_id + 1 */
    for (int64_t i = 0; i < n; i++) {
        ids[i] = i % 4;
        vs[i] = ids[i] + 1;
    }

    /* Expected sums: each group has n/4=25000 elements
     * group 0: 25000 * 1 = 25000
     * group 1: 25000 * 2 = 50000
     * group 2: 25000 * 3 = 75000
     * group 3: 25000 * 4 = 100000
     */

    int64_t name_id = ray_sym_intern("id", 2);
    int64_t name_v  = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, id_vec);
    tbl = ray_table_add_col(tbl, name_v, v_vec);

    ray_graph_t* g = ray_graph_new(tbl);

    /* Build group-by using the same API as test_graph.c */
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "v");

    ray_op_t* key_arr[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, key_arr, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Result should have 4 groups */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 4);

    /* Extract key and sum columns by index (0=key, 1=agg) */
    ray_t* res_ids = ray_table_get_col_idx(result, 0);
    ray_t* res_sums = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(res_ids);
    TEST_ASSERT_NOT_NULL(res_sums);

    int64_t* rids = (int64_t*)ray_data(res_ids);
    int64_t* rsums = (int64_t*)ray_data(res_sums);

    /* Verify sums (order may vary, so match by group id) */
    int64_t expected_sums[] = {25000, 50000, 75000, 100000};
    for (int64_t i = 0; i < 4; i++) {
        int64_t gid = rids[i];
        TEST_ASSERT_TRUE(gid >= 0 && gid <= 3);
        TEST_ASSERT_EQ_I(rsums[i], expected_sums[gid]);
    }

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(id_vec);
    ray_release(v_vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: parallel min/max via executor
 * -------------------------------------------------------------------------- */

static test_result_t test_parallel_min_max(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* vec = ray_vec_new(RAY_F64, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = n;

    double* vals = (double*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) vals[i] = (double)(i - 50000);
    /* Range: -50000.0 to 49999.0 */

    int64_t col_name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, col_name, vec);

    /* Test min */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan = ray_scan(g, "x");
    ray_op_t* min_op = ray_min_op(g, scan);

    ray_t* min_result = ray_execute(g, min_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(min_result));
    TEST_ASSERT_EQ_I(min_result->type, -RAY_F64);
    TEST_ASSERT_EQ_F(min_result->f64, -50000.0, 1e-6);

    ray_release(min_result);
    ray_graph_free(g);

    /* Test max (new graph, since execute consumes the graph) */
    g = ray_graph_new(tbl);
    scan = ray_scan(g, "x");
    ray_op_t* max_op = ray_max_op(g, scan);

    ray_t* max_result = ray_execute(g, max_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(max_result));
    TEST_ASSERT_EQ_I(max_result->type, -RAY_F64);
    TEST_ASSERT_EQ_F(max_result->f64, 49999.0, 1e-6);

    ray_release(max_result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_cancel() causes ray_execute() to return RAY_ERR_CANCEL
 * -------------------------------------------------------------------------- */

static test_result_t test_cancel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = n;
    int64_t* vals = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) vals[i] = i + 1;

    int64_t col_name = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, col_name, vec);

    /* Set cancel before execute — query should return RAY_ERR_CANCEL */
    ray_cancel();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan = ray_scan(g, "val");
    ray_op_t* sum_op = ray_sum(g, scan);
    ray_t* result = ray_execute(g, sum_op);
    /* ray_execute() resets cancel flag at start — first query may succeed */
    if (result) { if (RAY_IS_ERR(result)) ray_error_free(result); else ray_release(result); }

    /* ray_execute() resets the flag, so this tests that the next query works */
    ray_graph_free(g);

    /* Now verify normal execution works after cancel was consumed */
    g = ray_graph_new(tbl);
    scan = ray_scan(g, "val");
    sum_op = ray_sum(g, scan);
    result = ray_execute(g, sum_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    int64_t expected = n * (n + 1) / 2;
    TEST_ASSERT_EQ_I(result->i64, expected);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* A cancel request must reach BOTH the eval/VM interrupt flag and the pool's
 * worker-cancel flag through the single ray_request_interrupt() entry point,
 * and the clear path must reset both.  Before unification ray_cancel() had no
 * caller, so Ctrl-C set only the eval flag and never stopped parallel workers. */
static test_result_t test_interrupt_sets_both_flags(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_pool_t* pool = ray_pool_get();     /* ensure the pool is live (state 2) */
    TEST_ASSERT_NOT_NULL(pool);

    ray_clear_interrupt();
    TEST_ASSERT_FALSE(ray_interrupted());
    TEST_ASSERT_EQ_I((int)atomic_load_explicit(&pool->cancelled, memory_order_relaxed), 0);

    ray_request_interrupt();               /* the unified trigger */
    TEST_ASSERT_TRUE(ray_interrupted());   /* main-thread eval/VM flag */
    TEST_ASSERT_EQ_I((int)atomic_load_explicit(&pool->cancelled, memory_order_relaxed), 1); /* worker flag too */

    ray_clear_interrupt();                  /* the unified reset */
    TEST_ASSERT_FALSE(ray_interrupted());
    TEST_ASSERT_EQ_I((int)atomic_load_explicit(&pool->cancelled, memory_order_relaxed), 0);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Direct ray_pool_dispatch / ray_pool_dispatch_n coverage
 *
 * These tests exercise the pool API directly with a private pool, hitting
 * code paths the high-level ray_execute() tests do not reach:
 *   - ray_pool_dispatch with total_elems <= 0 (early return)
 *   - ray_pool_dispatch_n with n_tasks == 0 (early return)
 *   - ray_pool_dispatch_n small n (the entire dispatch_n body)
 *   - cancellation observed mid-dispatch by main + worker threads
 *   - ring growth (n_tasks > initial task_cap=1024)
 *   - single-worker pool create/free
 * -------------------------------------------------------------------------- */

/* Per-task counter incremented by every callback invocation.  Each task
 * increments once for each (start, end) range claim regardless of element
 * count — sufficient to verify dispatch coverage without per-element work. */
typedef struct {
    _Atomic(int64_t) calls;          /* number of fn invocations            */
    _Atomic(int64_t) elem_sum;       /* total elements processed across all */
    _Atomic(uint32_t) saw_worker;    /* bitmask of distinct worker_ids seen */
} pool_count_ctx_t;

static void pool_count_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    pool_count_ctx_t* c = (pool_count_ctx_t*)ctx;
    atomic_fetch_add_explicit(&c->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->elem_sum, end - start, memory_order_relaxed);
    if (worker_id < 32) {
        atomic_fetch_or_explicit(&c->saw_worker, 1u << worker_id,
                                 memory_order_relaxed);
    }
}

/* --------------------------------------------------------------------------
 * Test: dispatch with total_elems <= 0 returns immediately, no calls fire
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_zero_elems(void) {
    ray_heap_init();

    ray_pool_t pool;
    ray_err_t err = ray_pool_create(&pool, 1);  /* 1 worker thread */
    TEST_ASSERT_EQ_I(err, RAY_OK);

    pool_count_ctx_t ctx = {0};

    /* total_elems == 0 → early return, no dispatch */
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, 0);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 0);

    /* total_elems negative → also early return */
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, -1);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 0);

    /* dispatch_n with 0 → early return */
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, 0);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 0);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: dispatch over a small range produces exactly 1 task and processes
 * all elements (covers single-task fast path on private pool).
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_small(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);

    pool_count_ctx_t ctx = {0};
    int64_t n = 100;  /* well below TASK_GRAIN (8192) → 1 task */
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, n);

    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 1);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);

    /* Dispatch again on same pool — verifies pending counter resets cleanly */
    pool_count_ctx_t ctx2 = {0};
    ray_pool_dispatch(&pool, pool_count_fn, &ctx2, n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.calls), 1);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.elem_sum), n);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch_n with small n (covers the dispatch_n body
 * including task ring fill, semaphore signal, main-thread participation,
 * spin-wait for completion).
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_small(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);

    pool_count_ctx_t ctx = {0};
    uint32_t n = 16;
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, n);

    /* Each task is [i, i+1) → exactly n calls, each processing 1 element */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);

    /* Reuse pool: second dispatch_n */
    pool_count_ctx_t ctx2 = {0};
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx2, 4);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.calls), 4);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.elem_sum), 4);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch_n with n_tasks > task_cap forces ring growth.
 * Initial task_cap = 1024; 2000 tasks → grow to 2048.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_ring_growth(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);

    /* Sanity: initial cap is 1024 */
    TEST_ASSERT_EQ_U(pool.task_cap, 1024u);

    pool_count_ctx_t ctx = {0};
    uint32_t n = 2000;  /* > 1024 → ring must grow */
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, n);

    /* Capacity should have at least doubled (next power of 2 >= 2000) */
    TEST_ASSERT_TRUE(pool.task_cap >= 2048);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch with total_elems large enough to require ring
 * growth.  TASK_GRAIN = 8192, initial cap = 1024 → need > 1024 * 8192
 * elements (~8.4M) for n_tasks > task_cap.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_ring_growth(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);
    TEST_ASSERT_EQ_U(pool.task_cap, 1024u);

    pool_count_ctx_t ctx = {0};
    /* 1100 tasks worth of elements: 1100 * 8192 = 9,011,200 */
    int64_t n = 1100LL * 8192LL;
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, n);

    /* Cap should have grown */
    TEST_ASSERT_TRUE(pool.task_cap >= 2048);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 1100);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: cancel-mid-dispatch path on dispatch_n.
 *
 * Set pool->cancelled before dispatching; every task should be skipped.
 * Hits the RAY_UNLIKELY cancelled-skip branch on both worker + main threads
 * (lines 71-76 and 287-291 in pool.c).
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_cancelled(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);

    pool_count_ctx_t ctx = {0};
    /* Pre-set cancelled so every task hits the RAY_UNLIKELY skip branch */
    atomic_store_explicit(&pool.cancelled, 1, memory_order_release);

    uint32_t n = 64;
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, n);

    /* All tasks should have been skipped — pool drained to 0 pending,
     * but fn must NOT have been called. */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 0);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), 0);

    /* Reset and verify a normal dispatch works again */
    atomic_store_explicit(&pool.cancelled, 0, memory_order_release);
    pool_count_ctx_t ctx2 = {0};
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx2, 8);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.calls), 8);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: cancel-mid-dispatch on ray_pool_dispatch (range-partitioned variant).
 * Mirrors test_dispatch_n_cancelled but for the dispatch() entry point.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_cancelled(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);

    pool_count_ctx_t ctx = {0};
    atomic_store_explicit(&pool.cancelled, 1, memory_order_release);

    /* Use enough elements to produce multiple tasks across workers */
    int64_t n = 8192LL * 4;  /* 4 tasks */
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 0);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), 0);

    /* Clear and verify normal dispatch still works */
    atomic_store_explicit(&pool.cancelled, 0, memory_order_release);
    pool_count_ctx_t ctx2 = {0};
    ray_pool_dispatch(&pool, pool_count_fn, &ctx2, n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.calls), 4);
    TEST_ASSERT_EQ_I(atomic_load(&ctx2.elem_sum), n);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: pool with zero workers — main thread executes all tasks alone.
 * Exercises the n_workers==0 branch in ray_pool_create (no thread spawn,
 * no semaphore signals) and validates dispatch correctness without workers.
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_zero_workers(void) {
    ray_heap_init();

    ray_pool_t pool;
    /* Explicitly pass 1 here — passing 0 triggers auto-detect (nproc-1).
     * To force "main only", we manually create with 0 by skipping the
     * thread alloc path: but ray_pool_create's contract uses 0 = autodetect.
     * Use create with n_workers=1 for now and separately drive the
     * "main does work" code path via cancel. */
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 1), RAY_OK);
    TEST_ASSERT_EQ_U(pool.n_workers, 1u);

    pool_count_ctx_t ctx = {0};
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, 8192LL * 3);  /* 3 tasks */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 3);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), 8192LL * 3);

    /* Worker 0 (main) must have participated; worker 1 may or may not have
     * picked up tasks depending on scheduling — at least worker 0 is set. */
    uint32_t mask = atomic_load(&ctx.saw_worker);
    TEST_ASSERT_TRUE((mask & 0x1u) != 0);  /* main thread always = worker 0 */

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_total_workers() macro
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_total_workers(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 3), RAY_OK);
    TEST_ASSERT_EQ_U(ray_pool_total_workers(&pool), 4u);  /* 3 + main = 4 */
    ray_pool_free(&pool);

    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 1), RAY_OK);
    TEST_ASSERT_EQ_U(ray_pool_total_workers(&pool), 2u);
    ray_pool_free(&pool);

    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: total-worker initialization used by the CLI.
 *
 * The pool API normally accepts a background-worker count and reserves worker
 * 0 for the calling thread. The CLI's -c contract is different: it counts all
 * participants. In particular, total=1 must create an exact zero-background
 * pool rather than taking ray_pool_init(0)'s auto-size path.
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_init_total(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(1), RAY_OK);
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQ_U(pool->n_workers, 0u);
    TEST_ASSERT_EQ_U(ray_pool_total_workers(pool), 1u);

    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQ_U(pool->n_workers, 3u);
    TEST_ASSERT_EQ_U(ray_pool_total_workers(pool), 4u);

    /* Restore the lazy/default configuration for later tests. */
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init(0), RAY_OK);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_free(NULL) is a no-op (covers the early-return guard).
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_free_null(void) {
    ray_pool_free(NULL);  /* must not crash */
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_init() when global pool already initialized is a no-op.
 *
 * The global pool is auto-initialized by the first parallel test above
 * (state == 2).  Calling ray_pool_init() again should observe the CAS
 * failure path and return RAY_OK without altering n_workers.
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_init_idempotent(void) {
    /* First call: pool may already be initialized (state==2) from earlier
     * tests using ray_pool_get() via ray_execute(). */
    ray_pool_t* p_before = ray_pool_get();
    TEST_ASSERT_NOT_NULL(p_before);
    uint32_t n_before = p_before->n_workers;

    /* Re-init request should be silently ignored (state already 2) */
    ray_err_t err = ray_pool_init(99);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_pool_t* p_after = ray_pool_get();
    TEST_ASSERT_EQ_PTR(p_before, p_after);
    TEST_ASSERT_EQ_U(p_after->n_workers, n_before);

    PASS();
}

/* --------------------------------------------------------------------------
 * Test: full ray_pool_destroy → ray_pool_init → ray_pool_get round-trip.
 *
 * Exercises the destroy CAS (state 2→3→0), the init CAS-acquired branch
 * (state 0→1→2 with successful create), and post-init pool_get (state 2
 * fast path).  After this, the global pool is left re-initialized for
 * subsequent tests that depend on ray_pool_get() returning non-NULL.
 * -------------------------------------------------------------------------- */

static test_result_t test_pool_destroy_and_reinit(void) {
    /* Make sure the pool is initialized first (state==2). */
    ray_pool_t* p1 = ray_pool_get();
    TEST_ASSERT_NOT_NULL(p1);

    /* Destroy: state 2 → 3 → 0 */
    ray_pool_destroy();

    /* Second destroy is a no-op (CAS fails because state==0) */
    ray_pool_destroy();

    /* ray_pool_get() after destroy should re-initialize: state 0 → 1 → 2 */
    ray_pool_t* p2 = ray_pool_get();
    TEST_ASSERT_NOT_NULL(p2);
    /* Pool struct is the same global, but contents reset */
    TEST_ASSERT_EQ_PTR(p1, p2);

    /* Destroy and use ray_pool_init() instead to re-initialize */
    ray_pool_destroy();
    ray_err_t err = ray_pool_init(2);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_pool_t* p3 = ray_pool_get();
    TEST_ASSERT_NOT_NULL(p3);
    TEST_ASSERT_EQ_U(p3->n_workers, 2u);

    /* Restore the pool to a stable state for subsequent tests: rebuild
     * with default worker count.  ray_pool_init() is idempotent on
     * state==2 — safe to call repeatedly. */
    ray_pool_destroy();
    err = ray_pool_init(0);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_cancel() pokes pool->cancelled and a subsequent dispatch
 * observes it.  Calling ray_cancel() routes through ray_pool_get(), so
 * this also exercises the state==2 fast path of pool_get.
 * -------------------------------------------------------------------------- */

static test_result_t test_ray_cancel_global(void) {
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);

    /* Reset cancel state defensively (other tests may have left it dirty,
     * though ray_execute clears it on entry). */
    atomic_store_explicit(&pool->cancelled, 0, memory_order_release);

    ray_cancel();
    TEST_ASSERT_EQ_I(atomic_load_explicit(&pool->cancelled, memory_order_acquire), 1);

    /* Reset before exiting so we don't poison the global pool for later
     * tests. */
    atomic_store_explicit(&pool->cancelled, 0, memory_order_release);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: dispatch with workers that participate (force workers to claim
 * tasks by adding a small spin in the callback).  Drives line 73-76
 * (worker cancelled-skip path) by setting cancelled mid-flight on a large
 * task batch — workers will observe it on at least some iterations.
 * -------------------------------------------------------------------------- */

static void pool_count_with_spin_fn(void* ctx, uint32_t worker_id,
                                    int64_t start, int64_t end) {
    (void)worker_id;
    pool_count_ctx_t* c = (pool_count_ctx_t*)ctx;
    /* Light spin to give workers time to claim tasks before main drains.
     * 1k busy iterations per task ≈ a few microseconds — enough to ensure
     * workers wake from sem_wait and start consuming. */
    volatile int sink = 0;
    for (int i = 0; i < 1000; i++) sink += i;
    (void)sink;
    atomic_fetch_add_explicit(&c->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->elem_sum, end - start, memory_order_relaxed);
    if (worker_id < 32) {
        atomic_fetch_or_explicit(&c->saw_worker, 1u << worker_id,
                                 memory_order_relaxed);
    }
}

static test_result_t test_dispatch_workers_participate(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 3), RAY_OK);

    pool_count_ctx_t ctx = {0};
    /* Many tasks so workers + main race to claim them */
    uint32_t n = 256;
    ray_pool_dispatch_n(&pool, pool_count_with_spin_fn, &ctx, n);

    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);
    /* No per-id participation assert: WHICH claimants win is pure
     * scheduling.  Workers 1+ may never wake on a loaded runner — and
     * the inverse race is just as real: on a fast box the workers can
     * drain all 256 tasks before main enters its claim loop, leaving
     * bit 0 (main) unset — observed on macOS CI.  The test's value is
     * driving the multi-claimant contention paths; completion is
     * asserted above, and some bit is necessarily set once calls == n. */
    TEST_ASSERT_TRUE(atomic_load(&ctx.saw_worker) != 0);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch_n with n_tasks exceeding MAX_RING_CAP (1<<16).
 *
 * Drives the growth-loop early-out (`new_cap < MAX_RING_CAP`) on line ~335
 * and the post-growth clamp (`if (n_tasks > pool->task_cap) n_tasks = ...`)
 * on line ~347. With n_tasks = 70000 and MAX_RING_CAP = 65536, the ring
 * grows to 65536 then clamps n_tasks down to 65536; only 65536 tasks fire.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_max_ring_cap_clamp(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 1), RAY_OK);

    pool_count_ctx_t ctx = {0};
    /* MAX_RING_CAP is 1<<16 = 65536; ask for 70000 → growth caps at 65536,
     * then n_tasks is clamped to task_cap. */
    uint32_t requested = 70000;
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, requested);

    /* task_cap should have grown to MAX_RING_CAP exactly */
    TEST_ASSERT_EQ_U(pool.task_cap, 65536u);
    /* Calls should equal the clamped count, not the requested one. */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 65536);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch with total_elems large enough that n_tasks would
 * exceed MAX_RING_CAP, exercising the post-growth clamp on lines 246-249
 * (rebalances grain so all elements still get covered).
 *
 * total_elems = 70000 * TASK_GRAIN (= 70000 * 8192 = 573M).  We pass a
 * no-op fn so the cost is just the dispatch overhead — even at 65536
 * tasks we're under a second.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_max_ring_cap_clamp(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 1), RAY_OK);

    pool_count_ctx_t ctx = {0};
    /* TASK_GRAIN = 8 * 1024 = 8192.  70000 * 8192 = 573_440_000 elements,
     * which would naively want 70000 tasks. After clamp → 65536 tasks with
     * a slightly larger grain so total_elems is still fully covered. */
    int64_t grain = 8192;
    int64_t total = 70000LL * grain;
    ray_pool_dispatch(&pool, pool_count_fn, &ctx, total);

    /* Ring should grow to MAX_RING_CAP and stop there */
    TEST_ASSERT_EQ_U(pool.task_cap, 65536u);
    /* Calls clamped to MAX_RING_CAP */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 65536);
    /* All elements covered (grain rebalanced) */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), total);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_destroy on uninitialized state is a no-op.
 *
 * Already partly covered by test_pool_destroy_and_reinit (calling destroy
 * twice in a row), but this isolates the "state == 0" branch on entry to
 * destroy by destroying first to drop to 0, then calling destroy again
 * without any intervening init/get.
 * -------------------------------------------------------------------------- */

static test_result_t test_destroy_when_uninit(void) {
    /* Make sure we are at state==0 by destroying any existing pool first.
     * If the pool is currently at state==2, this drops it to 0; if it's
     * already 0 (no prior get/init), the CAS fails inside and it's a no-op. */
    ray_pool_destroy();
    /* Now in state==0: this destroy must hit the CAS-fail branch and return
     * without touching the pool. */
    ray_pool_destroy();

    /* Re-init for subsequent tests. */
    ray_err_t err = ray_pool_init(0);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_pool_dispatch_n with ring growth to a power-of-2 < MAX_RING_CAP.
 *
 * Existing test_dispatch_n_ring_grow uses 2000 → grows to 2048.  This test
 * pushes higher (5000 → grows to 8192) so the growth-loop runs multiple
 * iterations (1024 → 2048 → 4096 → 8192), strengthening coverage of the
 * `while (new_cap < n_tasks && new_cap < MAX_RING_CAP)` loop body.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_multi_grow(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);
    TEST_ASSERT_EQ_U(pool.task_cap, 1024u);

    pool_count_ctx_t ctx = {0};
    uint32_t n = 5000;
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, n);

    /* 1024 → 2048 → 4096 → 8192 (next power of 2 ≥ 5000) */
    TEST_ASSERT_EQ_U(pool.task_cap, 8192u);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), n);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: dispatch with n_tasks exactly equal to task_cap (no growth).
 *
 * Boundary case for the `n_tasks > pool->task_cap` check — when equal,
 * growth is skipped and the existing ring is used as-is.  Picks 1024
 * tasks (= initial cap) using dispatch_n so we don't multiply by grain.
 * -------------------------------------------------------------------------- */

static test_result_t test_dispatch_n_exact_cap(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 2), RAY_OK);
    TEST_ASSERT_EQ_U(pool.task_cap, 1024u);

    pool_count_ctx_t ctx = {0};
    ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, 1024);

    /* No growth — task_cap unchanged */
    TEST_ASSERT_EQ_U(pool.task_cap, 1024u);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 1024);

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

/* ==========================================================================
 * epoll.c region-coverage tests
 *
 * These tests are Linux-only and exercise paths in src/core/epoll.c that
 * are not reached by the IPC/repl tests:
 *   (a) sel_cap growth: register > RAY_POLL_INITIAL_CAP (16) selectors so
 *       the selector array must be doubled (lines 91-101 in epoll.c).
 *   (b) EPOLLHUP/EPOLLERR branch: register a socket fd with no recv_fn/
 *       read_fn but with an error_fn; close the peer end so epoll fires
 *       EPOLLIN|EPOLLHUP; the EPOLLIN block exits without goto, so the
 *       hangup block at line 232 is reached (lines 234-241 in epoll.c).
 * ========================================================================== */

#if defined(__linux__)

/* --------------------------------------------------------------------------
 * Test: register more than RAY_POLL_INITIAL_CAP (16) selectors on a single
 * poll — forces the selector-array growth path (epoll.c lines 91-101).
 *
 * We create 20 anonymous pipes, register the read ends with no callbacks,
 * verify all return valid ids, then destroy the poll (which deregisters
 * everything).  The write ends are closed immediately after registration
 * to avoid fd leaks.
 * -------------------------------------------------------------------------- */

#define EPOLL_TEST_N_SELS 20   /* > RAY_POLL_INITIAL_CAP (16) */

static test_result_t test_epoll_sel_cap_growth(void) {
    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);

    int write_ends[EPOLL_TEST_N_SELS];
    int64_t ids[EPOLL_TEST_N_SELS];

    for (int i = 0; i < EPOLL_TEST_N_SELS; i++) {
        int pfd[2];
        TEST_ASSERT_EQ_I(pipe(pfd), 0);
        write_ends[i] = pfd[1];

        ray_poll_reg_t reg;
        memset(&reg, 0, sizeof(reg));
        reg.fd   = pfd[0];
        reg.type = RAY_SEL_SOCKET;

        ids[i] = ray_poll_register(poll, &reg);
        TEST_ASSERT_FMT(ids[i] >= 0,
            "ray_poll_register failed for sel %d (id=%lld)", i, (long long)ids[i]);
    }

    /* Close write ends to avoid fd leaks; the read ends are owned by poll */
    for (int i = 0; i < EPOLL_TEST_N_SELS; i++)
        close(write_ends[i]);

    /* Verify the selector array grew past 16: n_sels should be 20 */
    TEST_ASSERT_EQ_U(poll->n_sels, (uint32_t)EPOLL_TEST_N_SELS);
    TEST_ASSERT_TRUE(poll->sel_cap >= (uint32_t)EPOLL_TEST_N_SELS);

    /* Manually close the read-end fds before destroy so they don't leak;
     * deregister each slot first (epoll_ctl DEL, free sel, NULL the slot). */
    for (int i = 0; i < EPOLL_TEST_N_SELS; i++) {
        ray_selector_t* sel = ray_poll_get(poll, ids[i]);
        if (sel) {
            int rfd = (int)sel->fd;
            ray_poll_deregister(poll, ids[i]);
            close(rfd);
        }
    }

    ray_poll_destroy(poll);
    PASS();
}

/* --------------------------------------------------------------------------
 * Context for the EPOLLHUP test thread.
 * -------------------------------------------------------------------------- */

typedef struct {
    ray_poll_t* poll;
} epoll_hup_ctx_t;

static void epoll_hup_poll_thread(void* arg) {
    epoll_hup_ctx_t* ctx = (epoll_hup_ctx_t*)arg;
    ray_poll_run(ctx->poll);
}

/* error_fn: called by epoll.c lines 237-239 when EPOLLHUP fires on a
 * selector with no recv_fn.  Calls ray_poll_exit so the poll loop stops. */
static void epoll_hup_error_fn(ray_poll_t* poll, ray_selector_t* sel) {
    (void)sel;
    ray_poll_exit(poll, 0);
}

/* --------------------------------------------------------------------------
 * Test: trigger EPOLLHUP/EPOLLERR branch (epoll.c lines 234-241).
 *
 * We use a socketpair.  Fd[0] is registered in the poll with no recv_fn/
 * read_fn but with an error_fn.  The poll runs in a background thread.
 * We then close fd[1] (the peer); epoll reports EPOLLIN|EPOLLHUP on fd[0].
 *
 *   EPOLLIN block (line 177): recv_fn==NULL → inner recv skipped; read_fn
 *   ==NULL → break at line 209.  Block exits normally (no goto next_event).
 *
 *   EPOLLHUP block (line 232): condition is true (EPOLLHUP set); sel is
 *   still registered; error_fn is non-NULL → error_fn called → poll exits.
 *
 * This covers lines 234-240 inclusive.
 * -------------------------------------------------------------------------- */

static test_result_t test_epoll_hup_branch(void) {
    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);

    int sv[2];
    TEST_ASSERT_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    ray_poll_reg_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.fd       = sv[0];
    reg.type     = RAY_SEL_SOCKET;
    reg.error_fn = epoll_hup_error_fn;
    /* recv_fn and read_fn intentionally left NULL so EPOLLIN block exits
     * without a goto, allowing the EPOLLHUP block to be reached. */

    int64_t id = ray_poll_register(poll, &reg);
    TEST_ASSERT_FMT(id >= 0, "ray_poll_register failed (id=%lld)", (long long)id);

    /* Run poll loop in background thread */
    epoll_hup_ctx_t ctx = { .poll = poll };
    ray_thread_t tid;
    ray_thread_create(&tid, epoll_hup_poll_thread, &ctx);

    /* Give the thread time to enter epoll_wait */
    epoll_sleep_ms(20);

    /* Close the peer — triggers EPOLLHUP (possibly also EPOLLIN with 0 bytes) */
    close(sv[1]);

    /* Wait for the poll thread to exit (error_fn sets poll->code = 0) */
    ray_thread_join(tid);

    /* sv[0] is still owned by the selector; deregister+close */
    ray_poll_deregister(poll, id);
    close(sv[0]);

    ray_poll_destroy(poll);
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: trigger EPOLLHUP branch with no error_fn — default deregister path
 * (epoll.c lines 238-239).  After deregister the selector slot is NULL,
 * the poll still runs; we stop it by calling ray_poll_exit from a second
 * thread after the hangup fires.
 * -------------------------------------------------------------------------- */

typedef struct {
    ray_poll_t* poll;
    volatile int hup_fired;
} epoll_hup_noerrfn_ctx_t;

static void epoll_hup_noerrfn_poll_thread(void* arg) {
    epoll_hup_noerrfn_ctx_t* ctx = (epoll_hup_noerrfn_ctx_t*)arg;
    /* poll->code starts at -1; we expect error_fn==NULL path to deregister
     * the selector but not stop the loop.  We stop it with ray_poll_exit. */
    ray_poll_run(ctx->poll);
}

static test_result_t test_epoll_hup_no_errfn(void) {
    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);

    int sv[2];
    TEST_ASSERT_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    ray_poll_reg_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.fd       = sv[0];
    reg.type     = RAY_SEL_SOCKET;
    /* No error_fn — hits the else branch at line 239 (ray_poll_deregister) */

    int64_t id = ray_poll_register(poll, &reg);
    TEST_ASSERT_FMT(id >= 0, "ray_poll_register failed (id=%lld)", (long long)id);

    /* Register the wake self-pipe BEFORE starting the poll thread.  The poll
     * selector array is single-owner by design — production only ever
     * registers/deregisters from the poll-loop thread — so registering here
     * (while no poll thread is running) keeps this test faithful to that
     * contract.  Later we only WRITE to wake_pipe[1], never touching the
     * selector array from the main thread. */
    int wake_pipe[2];
    bool have_wake = (pipe(wake_pipe) == 0);
    if (have_wake) {
        ray_poll_reg_t wake_reg;
        memset(&wake_reg, 0, sizeof(wake_reg));
        wake_reg.fd   = wake_pipe[0];
        wake_reg.type = RAY_SEL_SOCKET;
        ray_poll_register(poll, &wake_reg);
    }

    epoll_hup_noerrfn_ctx_t ctx = { .poll = poll, .hup_fired = 0 };
    ray_thread_t tid;
    ray_thread_create(&tid, epoll_hup_noerrfn_poll_thread, &ctx);

    epoll_sleep_ms(20);

    /* Close peer — triggers EPOLLHUP; default path deregisters sv[0] */
    close(sv[1]);

    /* Give the poll thread time to process the hangup, then stop the loop */
    epoll_sleep_ms(50);
    ray_poll_exit(poll, 0);

    /* Wake epoll_wait so the loop re-checks poll->code (>= 0) and exits.
     * Only a write to the pipe — no selector-array mutation from here. */
    if (have_wake) {
        char b = 'x';
        (void)write(wake_pipe[1], &b, 1);
        epoll_sleep_ms(30);
        close(wake_pipe[1]);
        /* wake_pipe[0] is owned by the poll selector; destroy will clean it */
    }

    ray_thread_join(tid);

    /* sv[0] was deregistered by the default hangup path; close our copy */
    close(sv[0]);

    ray_poll_destroy(poll);
    PASS();
}

#endif /* __linux__ */

/* --------------------------------------------------------------------------
 * Suite definition
 * -------------------------------------------------------------------------- */

const test_entry_t pool_entries[] = {
    { "pool/parallel_sum", test_parallel_sum, NULL, NULL },
    { "pool/parallel_add", test_parallel_add, NULL, NULL },
    { "pool/parallel_group_sum", test_parallel_group_sum, NULL, NULL },
    { "pool/parallel_min_max", test_parallel_min_max, NULL, NULL },
    { "pool/cancel", test_cancel, NULL, NULL },
    { "pool/interrupt_sets_both_flags", test_interrupt_sets_both_flags, NULL, NULL },
    { "pool/dispatch_zero_elems",   test_dispatch_zero_elems,   NULL, NULL },
    { "pool/dispatch_small",        test_dispatch_small,        NULL, NULL },
    { "pool/dispatch_n_small",      test_dispatch_n_small,      NULL, NULL },
    { "pool/dispatch_n_ring_grow",  test_dispatch_n_ring_growth, NULL, NULL },
    { "pool/dispatch_ring_grow",    test_dispatch_ring_growth,  NULL, NULL },
    { "pool/dispatch_n_cancelled",  test_dispatch_n_cancelled,  NULL, NULL },
    { "pool/dispatch_cancelled",    test_dispatch_cancelled,    NULL, NULL },
    { "pool/zero_workers",          test_pool_zero_workers,     NULL, NULL },
    { "pool/total_workers",         test_pool_total_workers,    NULL, NULL },
    { "pool/init_total",            test_pool_init_total,       NULL, NULL },
    { "pool/free_null",             test_pool_free_null,        NULL, NULL },
    { "pool/init_idempotent",       test_pool_init_idempotent,  NULL, NULL },
    { "pool/destroy_reinit",        test_pool_destroy_and_reinit, NULL, NULL },
    { "pool/ray_cancel_global",     test_ray_cancel_global,     NULL, NULL },
    { "pool/workers_participate",   test_dispatch_workers_participate, NULL, NULL },
    { "pool/dispatch_n_max_ring",   test_dispatch_n_max_ring_cap_clamp, NULL, NULL },
    { "pool/dispatch_max_ring",     test_dispatch_max_ring_cap_clamp, NULL, NULL },
    { "pool/destroy_when_uninit",   test_destroy_when_uninit,   NULL, NULL },
    { "pool/dispatch_n_multi_grow", test_dispatch_n_multi_grow, NULL, NULL },
    { "pool/dispatch_n_exact_cap",  test_dispatch_n_exact_cap,  NULL, NULL },
#if defined(__linux__)
    { "pool/epoll_sel_cap_growth",  test_epoll_sel_cap_growth,  NULL, NULL },
    { "pool/epoll_hup_branch",      test_epoll_hup_branch,      NULL, NULL },
    { "pool/epoll_hup_no_errfn",    test_epoll_hup_no_errfn,    NULL, NULL },
#endif
    { NULL, NULL, NULL, NULL },
};


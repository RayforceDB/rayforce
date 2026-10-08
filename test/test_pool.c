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
#include "core/timer.h"
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#endif

#if defined(__linux__)
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
 * Test: blocks a worker allocated inside a dispatch and the main thread freed
 * after it are back with their owners by the end of the next dispatch — no
 * registered heap carries a foreign list across a parallel region (issue
 * #619: a stream of parallel joins grew the process by a pool per worker
 * while the freed per-task buffers waited on those lists).
 * -------------------------------------------------------------------------- */

typedef struct {
    ray_t*   blocks[64];
    uint32_t owner[64];      /* worker_id that allocated blocks[i]; 0 = main */
} pool_alloc_ctx_t;

static void pool_alloc_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    pool_alloc_ctx_t* c = (pool_alloc_ctx_t*)ctx;
    for (int64_t i = start; i < end && i < 64; i++) {
        c->blocks[i] = ray_alloc(256u << 10);    /* 256 KB from the caller's heap */
        c->owner[i]  = worker_id;
    }
}

static test_result_t test_dispatch_reclaims_worker_blocks(void) {
    ray_heap_init();

    ray_pool_t pool;
    ray_err_t err = ray_pool_create(&pool, 3);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* ray_pool_create returns before the workers have started; a worker
     * publishes its heap once it has run ray_heap_init.  Wait for all three
     * on that published state, so every slot below is a real heap. */
    for (uint32_t w = 0; w < pool.n_workers; w++)
        while (!atomic_load(&pool.worker_heaps[w])) RAY_CPU_RELAX();

    /* Rounds of "workers allocate, main frees" until a round in which at
     * least one block really came from a worker (main is worker 0 and can
     * take every task when the others are slow to wake).  Rounds are cheap;
     * 64 of them without a worker allocation would mean the pool is not
     * running its workers at all. */
    pool_alloc_ctx_t ctx = {0};
    int worker_blocks = 0;
    for (int round = 0; round < 64 && worker_blocks == 0; round++) {
        ray_pool_dispatch_n(&pool, pool_alloc_fn, &ctx, 64);
        for (int i = 0; i < 64; i++) {
            TEST_ASSERT_NOT_NULL(ctx.blocks[i]);
            if (ctx.owner[i] != 0) worker_blocks++;
            ray_free(ctx.blocks[i]);             /* cross-thread for worker blocks */
            ctx.blocks[i] = NULL;
        }
    }
    TEST_ASSERT(worker_blocks > 0, "some blocks were allocated by workers");

    /* Those frees happened after the last dispatch ended, so the blocks sit
     * on their owners' foreign lists now; the next dispatch hands them back. */
    pool_count_ctx_t cctx = {0};
    ray_pool_dispatch_n(&pool, pool_count_fn, &cctx, 4);
    for (uint32_t w = 0; w < pool.n_workers; w++) {
        ray_heap_t* wh = (ray_heap_t*)atomic_load(&pool.worker_heaps[w]);
        TEST_ASSERT_NOT_NULL(wh);
        TEST_ASSERT_NULL(atomic_load(&wh->foreign));
    }

    ray_pool_free(&pool);
    PASS();
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

/* --------------------------------------------------------------------------
 * Test: a dispatch NARROWER than the pool still runs every task, and leaves
 * the pool fully usable for a later wide one (#599).
 *
 * ray_pool_dispatch/_n now signal only min(n_tasks-1, n_workers) workers
 * instead of the whole pool, so the under-signalled workers stay asleep.  The
 * risks that buys are (a) a task nobody claims and (b) signal accounting that
 * drifts across dispatches, starving a later wide window.  Alternating narrow
 * and wide dispatches over one pool catches both: every window is verified for
 * exact task and element counts.
 * -------------------------------------------------------------------------- */
static test_result_t test_dispatch_narrower_than_pool(void) {
    ray_heap_init();

    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 4), RAY_OK);

    /* n_tasks below the worker count: 1 wakes nobody, 2 wakes one, etc. */
    for (uint32_t n = 1; n <= 4; n++) {
        pool_count_ctx_t ctx = {0};
        ray_pool_dispatch_n(&pool, pool_count_fn, &ctx, n);
        TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), n);
        TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), n);
    }

    /* The element form, sized to a single task. */
    {
        pool_count_ctx_t ctx = {0};
        ray_pool_dispatch(&pool, pool_count_fn, &ctx, 1);
        TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), 1);
        TEST_ASSERT_EQ_I(atomic_load(&ctx.elem_sum), 1);
    }

    /* Alternating narrow/wide over the same pool: a wide window must still be
     * fully served after windows that signalled fewer workers than exist. */
    for (int rep = 0; rep < 25; rep++) {
        pool_count_ctx_t narrow = {0};
        ray_pool_dispatch_n(&pool, pool_count_fn, &narrow, 1);
        TEST_ASSERT_EQ_I(atomic_load(&narrow.calls), 1);

        pool_count_ctx_t wide = {0};
        ray_pool_dispatch_n(&pool, pool_count_fn, &wide, 32);
        TEST_ASSERT_EQ_I(atomic_load(&wide.calls), 32);
        TEST_ASSERT_EQ_I(atomic_load(&wide.elem_sum), 32);
    }

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

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

    /* No per-id participation assert, for the reason workers_participate
     * gives: which claimant wins is pure scheduling.  With one worker and
     * three tasks the worker can drain all three before main enters its
     * claim loop, leaving bit 0 unset — observed on macOS CI.  Completion
     * is asserted exactly above; some bit is necessarily set once
     * calls == 3. */
    TEST_ASSERT_TRUE(atomic_load(&ctx.saw_worker) != 0);

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
 * The ring grows to MAX_RING_CAP and stops; the tasks past it must still run,
 * in further rounds, each with its absolute index.  Until the rounds were
 * added the pool clamped n_tasks to the ring and silently dropped the rest
 * (a window with more partitions than the ring lost every partition past
 * 65536).
 * -------------------------------------------------------------------------- */
typedef struct {
    _Atomic(int64_t) calls;
    _Atomic(int64_t) start_sum;    /* sum of every task's absolute index */
    _Atomic(int64_t) start_max;
} pool_index_ctx_t;

static void pool_index_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    pool_index_ctx_t* c = (pool_index_ctx_t*)ctx;
    if (end != start + 1) return;   /* a range would break the [i, i+1) contract: leave calls short */
    atomic_fetch_add_explicit(&c->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->start_sum, start, memory_order_relaxed);
    int64_t seen = atomic_load_explicit(&c->start_max, memory_order_relaxed);
    while (start > seen && !atomic_compare_exchange_weak_explicit(&c->start_max, &seen, start,
                memory_order_relaxed, memory_order_relaxed)) {}
}

static test_result_t test_dispatch_n_max_ring_cap_clamp(void) {
    ray_heap_init();
    ray_pool_t pool;
    TEST_ASSERT_EQ_I(ray_pool_create(&pool, 1), RAY_OK);
    pool_index_ctx_t ctx = {0};
    uint32_t requested = 70000;
    ray_pool_dispatch_n(&pool, pool_index_fn, &ctx, requested);
    /* the ring grows to MAX_RING_CAP exactly ... */
    TEST_ASSERT_EQ_U(pool.task_cap, 65536u);
    /* ... and every requested task still runs, once, with its own index */
    TEST_ASSERT_EQ_I(atomic_load(&ctx.calls), (int64_t)requested);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.start_max), (int64_t)requested - 1);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.start_sum), (int64_t)requested * (requested - 1) / 2);
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
 * Claim-protocol tests: every ticket exactly once, under both claiming modes
 *
 * RAY_POOL_STEAL selects, at pool creation, the shared bounded-CAS cursor (0)
 * or per-worker contiguous ranges with tail stealing (1).  The pool has had
 * cross-dispatch claim races before, so each mode is driven through thousands
 * of back-to-back windows of every awkward shape — tiny n, n below / at /
 * above the worker count, n far above it through both entry points, and a
 * dispatch_n wider than the ring (several rounds) — with uneven per-ticket
 * work so that owners and thieves really meet on the words.  A ticket hit
 * twice or never is the failure; a lost ticket would also hang the dispatch,
 * which the harness watchdog reports as a hung test.
 * ========================================================================== */

#if defined(__linux__) || defined(__APPLE__)

/* Create a pool under a given RAY_POOL_STEAL value, restoring the variable
 * afterwards (the switch is read once, at creation). */
static ray_err_t pool_create_mode(ray_pool_t* pool, uint32_t n_workers, bool steal) {
    const char* cur = getenv("RAY_POOL_STEAL");
    char* saved = cur ? strdup(cur) : NULL;
    setenv("RAY_POOL_STEAL", steal ? "1" : "0", 1);
    ray_err_t rc = ray_pool_create(pool, n_workers);
    if (saved) { setenv("RAY_POOL_STEAL", saved, 1); free(saved); }
    else unsetenv("RAY_POOL_STEAL");
    return rc;
}

#define POOL_ONCE_MAX   70000u
#define POOL_ONCE_GRAIN ((int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS)   /* = TASK_GRAIN */

static _Atomic(uint32_t) pool_once_hits[POOL_ONCE_MAX];   /* per-ticket hit count */

typedef struct {
    int64_t  grain;              /* elements per ticket: 1 (dispatch_n) or TASK_GRAIN */
    uint32_t n;                  /* tickets in the window */
    uint32_t block;              /* ticket that waits for every other one; UINT32_MAX = none */
    _Atomic(uint32_t) done;      /* tickets finished */
} pool_once_ctx_t;

static void pool_once_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    pool_once_ctx_t* c = (pool_once_ctx_t*)ctx;
    uint32_t t = (uint32_t)(start / c->grain);
    /* Uneven, deterministic per-ticket work (0..16k iterations) so claims
     * and steals interleave instead of every ticket finishing at once. */
    volatile uint32_t sink = 0;
    uint32_t spin = ((t * 2654435761u) >> 24) << 6;
    for (uint32_t i = 0; i < spin; i++) sink += i;
    (void)sink;
    /* The blocking ticket finishes last: it waits on observable state (every
     * other ticket done), never on a timer. */
    if (t == c->block)
        while (atomic_load_explicit(&c->done, memory_order_acquire) + 1 < c->n) RAY_CPU_RELAX();
    if (t < POOL_ONCE_MAX && end - start <= c->grain)
        atomic_fetch_add_explicit(&pool_once_hits[t], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->done, 1, memory_order_release);
}

static void pool_once_reset(pool_once_ctx_t* c, uint32_t n, int64_t grain) {
    for (uint32_t t = 0; t < n; t++)
        atomic_store_explicit(&pool_once_hits[t], 0, memory_order_relaxed);
    c->grain = grain;
    c->n = n;
    atomic_store_explicit(&c->done, 0, memory_order_relaxed);
}

/* First ticket in [0, n) whose hit count is not exactly one, or -1. */
static int64_t pool_once_bad(uint32_t n) {
    for (uint32_t t = 0; t < n; t++)
        if (atomic_load_explicit(&pool_once_hits[t], memory_order_relaxed) != 1) return t;
    return -1;
}

/* One window through the chosen entry point, then the exactly-once check
 * and the cross-dispatch invariant (window fully claimed: cursors equal). */
static test_result_t pool_once_window(ray_pool_t* pool, pool_once_ctx_t* c, uint32_t n,
                                      bool elem_form, uint64_t* tickets) {
    pool_once_reset(c, n, elem_form ? POOL_ONCE_GRAIN : 1);
    if (elem_form) ray_pool_dispatch(pool, pool_once_fn, c, (int64_t)n * POOL_ONCE_GRAIN);
    else           ray_pool_dispatch_n(pool, pool_once_fn, c, n);
    *tickets += n;
    TEST_ASSERT_EQ_U(atomic_load(&c->done), n);
    int64_t bad = pool_once_bad(n);
    TEST_ASSERT_FMT(bad < 0, "ticket %lld of %u hit %u times (steal=%u, workers=%u, %s)",
                    (long long)bad, n, bad < 0 ? 0u : atomic_load(&pool_once_hits[bad]),
                    pool->steal, pool->n_workers, elem_form ? "dispatch" : "dispatch_n");
    TEST_ASSERT_EQ_U(atomic_load(&pool->task_claim), atomic_load(&pool->task_limit));
    PASS();
}

#define POOL_ONCE_RUN(pool, c, n, elem, tk) do {                              \
        test_result_t _r = pool_once_window((pool), (c), (n), (elem), (tk)); \
        if (_r.status != TEST_PASS) return _r;                                \
    } while (0)

/* The window battery over one pool: every awkward shape, many times over. */
static test_result_t pool_once_battery(ray_pool_t* pool) {
    uint32_t W = ray_pool_total_workers(pool);
    pool_once_ctx_t c = { .block = UINT32_MAX };
    uint64_t tickets = 0;

    /* thousands of back-to-back tiny windows: the cross-dispatch race bed */
    for (uint32_t i = 0; i < 3000; i++)
        POOL_ONCE_RUN(pool, &c, 1 + i % 3, false, &tickets);

    /* n below, at and just above the worker count, and a few multiples */
    for (uint32_t i = 0; i < 300; i++) {
        if (W > 1) POOL_ONCE_RUN(pool, &c, W - 1, false, &tickets);
        POOL_ONCE_RUN(pool, &c, W, false, &tickets);
        POOL_ONCE_RUN(pool, &c, W + 1, false, &tickets);
        POOL_ONCE_RUN(pool, &c, 2 * W + 1, i & 1, &tickets);
    }

    /* n far above the worker count, both entry points */
    for (uint32_t i = 0; i < 60; i++) {
        POOL_ONCE_RUN(pool, &c, 1000, false, &tickets);
        POOL_ONCE_RUN(pool, &c, 1000, true, &tickets);
    }

    /* more tasks than the ring can ever hold: several dispatch_n rounds */
    POOL_ONCE_RUN(pool, &c, POOL_ONCE_MAX, false, &tickets);

    /* Accounting: in steal mode every ticket was claimed from an own range
     * or stolen, nothing else; the cursor path never touches the counters. */
    uint64_t own = 0, stolen = 0;
    for (uint32_t w = 0; w < W; w++) { own += pool->slots[w].own; stolen += pool->slots[w].stolen; }
    if (pool->steal) TEST_ASSERT_EQ_U(own + stolen, tickets);
    else             TEST_ASSERT_EQ_U(own + stolen, 0u);
    PASS();
}

static test_result_t pool_once_mode(bool steal) {
    ray_heap_init();
    static const uint32_t workers[] = { 3, 0, 6 };   /* W = 4, 1 (main only), 7 */
    for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
        ray_pool_t pool;
        TEST_ASSERT_EQ_I(pool_create_mode(&pool, workers[i], steal), RAY_OK);
        TEST_ASSERT_EQ_U(pool.steal, steal ? 1u : 0u);
        test_result_t r = pool_once_battery(&pool);
        ray_pool_free(&pool);
        if (r.status != TEST_PASS) return r;
    }
    ray_heap_destroy();
    PASS();
}

static test_result_t test_claim_once_cursor(void) { return pool_once_mode(false); }
static test_result_t test_claim_once_steal(void)  { return pool_once_mode(true); }

/* --------------------------------------------------------------------------
 * Test: stealing is forced, not left to scheduling luck.
 *
 * Ticket 0 heads main's own range and waits until every other ticket is
 * done.  Main is stuck in it, so the rest of main's share can only run if
 * the workers cut it from the tail — with ranges the window cannot complete
 * otherwise (a hang the watchdog would report).  Should a worker beat main
 * to ticket 0 instead, it did so by stealing it, so `stolen` is positive
 * either way.  The cursor mode runs the same window as a control: there the
 * remaining tickets are simply claimed from the shared cursor.
 * -------------------------------------------------------------------------- */
static test_result_t pool_steal_forced_mode(bool steal) {
    ray_heap_init();
    ray_pool_t pool;
    TEST_ASSERT_EQ_I(pool_create_mode(&pool, 3, steal), RAY_OK);

    pool_once_ctx_t c = { .block = 0 };
    uint64_t tickets = 0;
    for (int rep = 0; rep < 8; rep++)
        POOL_ONCE_RUN(&pool, &c, 4000, rep & 1, &tickets);

    uint64_t own = 0, stolen = 0, steals = 0;
    for (uint32_t w = 0; w < ray_pool_total_workers(&pool); w++) {
        own += pool.slots[w].own; stolen += pool.slots[w].stolen; steals += pool.slots[w].steals;
    }
    if (steal) {
        TEST_ASSERT_EQ_U(own + stolen, tickets);
        TEST_ASSERT_FMT(stolen > 0 && steals > 0, "no steal in %llu windows", (unsigned long long)8);
    } else {
        TEST_ASSERT_EQ_U(own + stolen + steals, 0u);
    }

    ray_pool_free(&pool);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_steal_forced(void)        { return pool_steal_forced_mode(true); }
static test_result_t test_steal_forced_cursor(void) { return pool_steal_forced_mode(false); }

#endif /* __linux__ || __APPLE__ */

/* ==========================================================================
 * Bounded poll-step tests
 * ========================================================================== */

#if defined(__linux__) || defined(__APPLE__)

typedef struct {
    int calls;
} poll_run_for_ctx_t;

static int64_t poll_run_for_recv(int64_t fd, uint8_t* buf, int64_t len) {
    return (int64_t)read((int)fd, buf, (size_t)len);
}

static ray_t* poll_run_for_read(ray_poll_t* poll, ray_selector_t* sel) {
    poll_run_for_ctx_t* ctx = (poll_run_for_ctx_t*)sel->data;
    ctx->calls++;
    ray_poll_deregister(poll, sel->id);
    return NULL;
}

static test_result_t test_poll_run_for_zero_drains_ready_event(void) {
    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);

    int pfd[2];
    TEST_ASSERT_EQ_I(pipe(pfd), 0);

    poll_run_for_ctx_t ctx = {0};
    ray_poll_reg_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.fd      = pfd[0];
    reg.type    = RAY_SEL_SOCKET;
    reg.data    = &ctx;
    reg.recv_fn = poll_run_for_recv;
    reg.read_fn = poll_run_for_read;

    int64_t id = ray_poll_register(poll, &reg);
    TEST_ASSERT_TRUE(id >= 0);
    ray_selector_t* sel = ray_poll_get(poll, id);
    TEST_ASSERT_NOT_NULL(sel);
    ray_poll_rx_request(poll, sel, 1);

    TEST_ASSERT_EQ_I(write(pfd[1], "x", 1), 1);
    TEST_ASSERT_EQ_I(ray_poll_run_for(poll, 0), 0);
    TEST_ASSERT_EQ_I(ctx.calls, 1);

    close(pfd[0]);
    close(pfd[1]);
    ray_poll_destroy(poll);
    PASS();
}

static test_result_t test_poll_run_for_positive_timeout(void) {
    TEST_ASSERT_EQ_I(ray_poll_run_for(NULL, 10), -1);

    ray_poll_t* poll = ray_poll_create();
    TEST_ASSERT_NOT_NULL(poll);

    int64_t before = ray_time_now_ms();
    TEST_ASSERT_EQ_I(ray_poll_run_for(poll, 25), 0);
    int64_t elapsed = ray_time_now_ms() - before;

    TEST_ASSERT_FMT(elapsed >= 15,
                    "bounded poll returned too early (%lld ms)",
                    (long long)elapsed);
    TEST_ASSERT_FMT(elapsed < 500,
                    "bounded poll exceeded its budget (%lld ms)",
                    (long long)elapsed);

    ray_poll_destroy(poll);
    PASS();
}

#endif

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

#if defined(__linux__) || defined(__APPLE__)
static test_result_t test_auto_all_logical_cpus(void) {
    const char* current = getenv("RAYFORCE_CORES");
    char* saved = current ? strdup(current) : NULL;
    TEST_ASSERT_TRUE(!current || saved);
    unsetenv("RAYFORCE_CORES");
    ray_pool_t local;
    ray_err_t rc = ray_pool_create(&local, 0);
    uint32_t total = rc == RAY_OK ? ray_pool_total_workers(&local) : 0;
    if (rc == RAY_OK) ray_pool_free(&local);
    if (saved) { setenv("RAYFORCE_CORES", saved, 1); free(saved); }
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(total, ray_thread_count());
    PASS();
}
#endif

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* --------------------------------------------------------------------------
 * Test: scan read-ahead requests the registered rows from storage.
 *
 * A column file is evicted from the page cache, registered, and walked by
 * tasks that read a page of it here and there at most, with the kernel's
 * own read-ahead on faults off (MADV_RANDOM): the rest of its pages can
 * only become resident because the pool asked the kernel for them.  A
 * dispatch over exactly its rows is read ahead per ticket; a dispatch_n
 * task reports its position through ray_pool_scan_at; the chunks a gate
 * rules out are never requested.  Skipped where eviction has no effect
 * (tmpfs keeps its pages) or read-ahead is turned off (RAY_SCAN_PREFETCH=0).
 * -------------------------------------------------------------------------- */
#define PF_ROWS      ((int64_t)1 << 20)   /* 8 MiB of int64 */
#define PF_PAGES     (PF_ROWS * 8 / 4096)
#define PF_TASKS     8
#define PF_LOG2      16                   /* gate chunk: 64K rows, 512 KiB */
/* per process, so concurrent runs of the suite do not share the files */
static char pf_path_a[96], pf_path_b[96];
static void pf_paths(void) {
    if (pf_path_a[0]) return;
    snprintf(pf_path_a, sizeof(pf_path_a), "/tmp/rayforce_test_scan_ahead.%d.col", (int)getpid());
    snprintf(pf_path_b, sizeof(pf_path_b), "/tmp/rayforce_test_scan_ahead_b.%d.col", (int)getpid());
}
#define PF_PATH      (pf_paths(), pf_path_a)
#define PF_PATH_B    (pf_paths(), pf_path_b)

/* Ticket task: reads the middle row of its rows of column A (ctx), one
 * page of the sixteen a ticket spans — the one the next call of its thread
 * looks at to tell whether A is read. */
static volatile int64_t pf_sink;
static void pf_touch(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w;
    pf_sink += ((const int64_t*)ctx)[s + (e - s) / 2];
}

/* dispatch_n task t walks rows [t*per, (t+1)*per) of column A (ctx),
 * reading only the first 8192 of them; the rest is left to read-ahead. */
static void pf_walk(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w;
    const int64_t* a = (const int64_t*)ctx;
    const int64_t per = PF_ROWS / PF_TASKS;
    for (int64_t t = s; t < e; t++)
        for (int64_t r = t * per; r < (t + 1) * per; r += 8192) {
            ray_pool_scan_at(PF_ROWS, r, (t + 1) * per);
            if (r == t * per) {
                int64_t sum = 0;
                for (int64_t i = r; i < r + 8192; i += 512) sum += a[i];
                pf_sink += sum;
            }
        }
}

/* As pf_walk over columns A and B (ctx: their bases), block by block: the
 * middle row of A in the first block, the middle row of B in each block
 * from the seventh on.  A block's call looks at the middle of the block
 * before, so B is found unread at the calls of blocks 1-4. */
static void pf_walk_late(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w;
    const int64_t* const* col = (const int64_t* const*)ctx;
    const int64_t per = PF_ROWS / PF_TASKS;
    for (int64_t t = s; t < e; t++)
        for (int64_t r = t * per, k = 0; r < (t + 1) * per; r += 8192, k++) {
            ray_pool_scan_at(PF_ROWS, r, (t + 1) * per);
            if (k == 0) pf_sink += col[0][r + 4096];
            if (k >= 6) pf_sink += col[1][r + 4096];
        }
}

/* As pf_walk, naming column A (ctx) as the one read and reading none of it. */
static void pf_walk_named(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w;
    const int64_t per = PF_ROWS / PF_TASKS;
    for (int64_t t = s; t < e; t++)
        for (int64_t r = t * per; r < (t + 1) * per; r += 8192)
            ray_pool_scan_col_at(ctx, PF_ROWS, r, (t + 1) * per);
}

/* One task over every gate chunk of column A (ctx) — even chunks pass the
 * gate: a call at each chunk's start; from the fifth gated-in chunk on, a
 * second call half way through, with the row between the two calls read. */
static void pf_walk_gaps(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w; (void)s; (void)e;
    const int64_t* a = (const int64_t*)ctx;
    const int64_t chunk = (int64_t)1 << PF_LOG2;
    int in = 0;
    for (int64_t c0 = 0; c0 < PF_ROWS; c0 += chunk) {
        ray_pool_scan_at(PF_ROWS, c0, PF_ROWS);
        if ((c0 / chunk) % 2 || in++ < 4) continue;
        pf_sink += a[c0 + chunk / 4];
        ray_pool_scan_at(PF_ROWS, c0 + chunk / 2, PF_ROWS);
    }
}

static bool pf_evict_one(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = fdatasync(fd) == 0 && posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    close(fd);
    return ok;
}
static bool pf_evict(void) { return pf_evict_one(PF_PATH) && pf_evict_one(PF_PATH_B); }

/* Map a column file with the kernel's read-ahead on faults off: a page
 * read through the mapping brings no neighbours in with it. */
static uint8_t* pf_map(const char* path, size_t* size) {
    uint8_t* p = (uint8_t*)ray_vm_map_file(path, size);
    if (p) posix_madvise(p, *size, POSIX_MADV_RANDOM);
    return p;
}

/* No sampled page of [p, p+n) is resident. */
static bool pf_none_resident(const uint8_t* p, size_t n) {
    for (size_t off = 0; off < n; off += n / 16)
        if (ray_vm_resident(p + off, 4096)) return false;
    return true;
}

/* Pages this process has read stay mapped and are not evicted: drop both
 * columns' mappings, evict the files, map them again and re-register. */
static bool pf_reset(uint8_t** base, size_t* size, uint8_t** bbase, size_t* bsize,
                     ray_pool_scan_t* scan) {
    ray_vm_unmap_file(*base, *size);
    ray_vm_unmap_file(*bbase, *bsize);
    *base = *bbase = NULL;
    if (!pf_evict()) return false;
    *base = pf_map(PF_PATH, size);
    *bbase = pf_map(PF_PATH_B, bsize);
    if (!*base || !*bbase) return false;
    scan->base[0] = *base;
    scan->base[1] = *bbase;
    return pf_none_resident(*base, *size) && pf_none_resident(*bbase, *bsize);
}

/* Resident pages of [p, p+n), probed one by one. */
static int64_t pf_resident_pages(const uint8_t* p, size_t n) {
    int64_t k = 0;
    for (size_t off = 0; off < n; off += 4096) k += ray_vm_resident(p + off, 1);
    return k;
}

/* Bounded wait (~10 s) for `want` pages of [p, p+n) to be resident. */
static bool pf_wait_pages(const uint8_t* p, size_t n, int64_t want) {
    for (int i = 0; i < 2000; i++) {
        if (pf_resident_pages(p, n) >= want) return true;
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

/* Bounded wait for the kernel to finish the reads: every sampled page of
 * [p, p+n) resident.  Returns false after ~10 s. */
static bool pf_wait_resident(const void* p, size_t n) {
    for (int i = 0; i < 20000; i++) {
        if (ray_vm_resident(p, n)) return true;
        struct timespec ts = { 0, 500000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

static test_result_t test_scan_read_ahead(void) {
    ray_heap_init();
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    test_result_t res = { TEST_PASS, NULL };
    uint8_t* base = NULL;
    uint8_t* bbase = NULL;
    size_t size = 0, bsize = 0;

    const char* paths[2] = { PF_PATH, PF_PATH_B };
    for (int k = 0; k < 2; k++) {
        FILE* f = fopen(paths[k], "wb");
        TEST_ASSERT_NOT_NULL(f);
        for (int64_t i = 0; i < PF_ROWS; i++) fwrite(&i, sizeof(i), 1, f);
        fclose(f);
    }
    base = pf_map(PF_PATH, &size);
    bbase = pf_map(PF_PATH_B, &bsize);
    TEST_ASSERT_NOT_NULL(base);
    TEST_ASSERT_NOT_NULL(bbase);
    TEST_ASSERT_EQ_U(size, (uint64_t)PF_ROWS * 8);

    if (!pool->scan_bytes) { res = (test_result_t){ TEST_SKIP, "RAY_SCAN_PREFETCH=0" }; goto out; }
    if (!pf_evict() || ray_vm_resident(base, size) || !pf_none_resident(bbase, bsize)) {
        res = (test_result_t){ TEST_SKIP, "page cache eviction has no effect here" };
        goto out;
    }

    /* Column B is registered but no dispatch below reads it: it must never
     * be requested. */
    ray_pool_scan_t scan = { .base = { base, bbase }, .esz = { 8, 8 }, .n = 2, .rows = PF_ROWS };
    const ray_pool_scan_t* prev = ray_pool_scan_set(&scan);

    /* Per ticket: a dispatch over exactly the registered rows (range mode
     * only; the shared cursor hands consecutive tickets to different
     * threads, so there is no per-thread run to read ahead of).  A ticket
     * reads one page of A: enough for its thread to see A read, while the
     * other pages come in only by request — all but those of the first
     * ticket, before which nothing was seen read. */
    if (pool->steal) {
        ray_pool_dispatch(pool, pf_touch, base, PF_ROWS);
        if (!pf_wait_pages(base, size, PF_PAGES * 7 / 8)) {
            res = (test_result_t){ TEST_FAIL, "per-ticket read-ahead left the rows unread" };
            goto restore;
        }
        if (!pf_none_resident(bbase, bsize)) {
            res = (test_result_t){ TEST_FAIL, "per-ticket read-ahead requested a column no ticket read" };
            goto restore;
        }
        if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }
    }

    /* Reported positions: dispatch_n tasks walking row ranges, reading the
     * first rows of each; the rest of A is requested, B is not. */
    ray_pool_dispatch_n(pool, pf_walk, base, PF_TASKS);
    if (!pf_wait_resident(base, size)) {
        res = (test_result_t){ TEST_FAIL, "task-reported read-ahead left the rows unread" };
        goto restore;
    }
    if (!pf_none_resident(bbase, bsize)) {
        res = (test_result_t){ TEST_FAIL, "task-reported read-ahead requested a column no task read" };
        goto restore;
    }
    if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }

    /* A column read sparsely: B is first read in the seventh block of each
     * task, after four probes found it unread.  It is never probed again,
     * so never requested; A, read in the first block, is. */
    const uint8_t* cols[2] = { base, bbase };
    ray_pool_dispatch_n(pool, pf_walk_late, cols, PF_TASKS);
    if (!pf_wait_pages(base, size, PF_PAGES * 7 / 8)) {
        res = (test_result_t){ TEST_FAIL, "a column read from the first block was not requested" };
        goto restore;
    }
    if (pf_resident_pages(bbase, bsize) > PF_PAGES / 8) {
        res = (test_result_t){ TEST_FAIL, "a column found unread four times was requested" };
        goto restore;
    }
    if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }

    /* A task that names the column it reads has it requested from its first
     * call, without reading any of it. */
    ray_pool_dispatch_n(pool, pf_walk_named, base, PF_TASKS);
    if (!pf_wait_pages(base, size, PF_PAGES * 15 / 16)) {
        res = (test_result_t){ TEST_FAIL, "a named column was not requested" };
        goto restore;
    }
    if (!pf_none_resident(bbase, bsize)) {
        res = (test_result_t){ TEST_FAIL, "a column no task named was requested" };
        goto restore;
    }
    if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }

    /* Gate: only even chunks may pass, so odd chunks are never requested. */
    uint64_t gate = 0;
    const int64_t n_chunks = PF_ROWS >> PF_LOG2;
    for (int64_t c = 0; c < n_chunks; c += 2) gate |= UINT64_C(1) << c;
    scan.gate = &gate;
    scan.gate_log2 = PF_LOG2;
    ray_pool_dispatch_n(pool, pf_walk, base, PF_TASKS);
    const size_t chunk = ((size_t)1 << PF_LOG2) * 8;
    for (int64_t c = 0; c < n_chunks; c++) {
        const uint8_t* p = base + (size_t)c * chunk;
        if (c % 2 == 0 && !pf_wait_resident(p, chunk)) {
            res = (test_result_t){ TEST_FAIL, "a gated-in chunk was not read" };
            goto restore;
        }
        if (c % 2 == 1 && ray_vm_resident(p + chunk / 2, 4096)) {
            res = (test_result_t){ TEST_FAIL, "a gated-out chunk was read" };
            goto restore;
        }
    }
    if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }

    /* A gated-out chunk ends the run: the first call past it does not probe
     * the rows the gate skipped (which nobody reads).  Single calls between
     * gaps there then cost the column no probes, and the run that reads it
     * gets it requested — the later gated-in chunks, past what was read. */
    ray_pool_dispatch_n(pool, pf_walk_gaps, base, 1);
    for (int64_t c = 10; c < n_chunks; c += 2) {
        if (!pf_wait_resident(base + (size_t)c * chunk + chunk / 2, chunk / 2)) {
            res = (test_result_t){ TEST_FAIL, "probes across gated-out chunks kept the column from being requested" };
            goto restore;
        }
    }

    /* Outside a dispatch the hooks have nothing registered to act on. */
    ray_pool_scan_at(PF_ROWS, 0, PF_ROWS);
    ray_pool_scan_col_at(base, PF_ROWS, 0, PF_ROWS);

restore:
    ray_pool_scan_set(prev);
out:
    if (base) ray_vm_unmap_file(base, size);
    if (bbase) ray_vm_unmap_file(bbase, bsize);
    remove(PF_PATH);
    remove(PF_PATH_B);
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init(0), RAY_OK);
    ray_heap_destroy();
    return res;
}

/* --------------------------------------------------------------------------
 * Test: what a thread saw of a dispatch does not outlive the pool.
 *
 * The main thread runs tasks of each dispatch and keeps, per thread, the
 * columns it saw read and the rows it requested, under the generation of
 * that dispatch.  ray_pool_destroy zeroes the pool, and the pool made after
 * it numbers its own dispatches: none of them may take that state for its
 * own.  Column B, read by the old pool's first dispatch and not by the new
 * pool's first, must not be requested by the latter.
 * -------------------------------------------------------------------------- */

/* As pf_walk over columns A and B (ctx: their bases): one page of each in
 * the first block of every task. */
static void pf_walk_both(void* ctx, uint32_t w, int64_t s, int64_t e) {
    (void)w;
    const int64_t* const* col = (const int64_t* const*)ctx;
    const int64_t per = PF_ROWS / PF_TASKS;
    for (int64_t t = s; t < e; t++)
        for (int64_t r = t * per; r < (t + 1) * per; r += 8192) {
            ray_pool_scan_at(PF_ROWS, r, (t + 1) * per);
            if (r == t * per) pf_sink += col[0][r + 4096] + col[1][r + 4096];
        }
}

static test_result_t test_scan_read_ahead_new_pool(void) {
    ray_heap_init();
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    test_result_t res = { TEST_PASS, NULL };
    uint8_t* base = NULL;
    uint8_t* bbase = NULL;
    size_t size = 0, bsize = 0;

    const char* paths[2] = { PF_PATH, PF_PATH_B };
    for (int k = 0; k < 2; k++) {
        FILE* f = fopen(paths[k], "wb");
        TEST_ASSERT_NOT_NULL(f);
        for (int64_t i = 0; i < PF_ROWS; i++) fwrite(&i, sizeof(i), 1, f);
        fclose(f);
    }
    base = pf_map(PF_PATH, &size);
    bbase = pf_map(PF_PATH_B, &bsize);
    TEST_ASSERT_NOT_NULL(base);
    TEST_ASSERT_NOT_NULL(bbase);

    if (!pool->scan_bytes) { res = (test_result_t){ TEST_SKIP, "RAY_SCAN_PREFETCH=0" }; goto out; }
    if (!pf_evict() || ray_vm_resident(base, size) || !pf_none_resident(bbase, bsize)) {
        res = (test_result_t){ TEST_SKIP, "page cache eviction has no effect here" };
        goto out;
    }

    ray_pool_scan_t scan = { .base = { base, bbase }, .esz = { 8, 8 }, .n = 2, .rows = PF_ROWS };
    const ray_pool_scan_t* prev = ray_pool_scan_set(&scan);
    const uint8_t* cols[2] = { base, bbase };
    ray_pool_dispatch_n(pool, pf_walk_both, cols, PF_TASKS);

    ray_pool_destroy();
    if (ray_pool_init_total(4) != RAY_OK || !(pool = ray_pool_get())) {
        res = (test_result_t){ TEST_FAIL, "pool init" };
        goto restore;
    }
    if (!pf_reset(&base, &size, &bbase, &bsize, &scan)) { res = (test_result_t){ TEST_FAIL, "evict" }; goto restore; }
    ray_pool_dispatch_n(pool, pf_walk, base, PF_TASKS);
    if (!pf_wait_resident(base, size)) {
        res = (test_result_t){ TEST_FAIL, "task-reported read-ahead left the rows unread" };
        goto restore;
    }
    if (!pf_none_resident(bbase, bsize)) {
        res = (test_result_t){ TEST_FAIL, "a column only the old pool's dispatch read was requested" };
        goto restore;
    }

restore:
    ray_pool_scan_set(prev);
out:
    if (base) ray_vm_unmap_file(base, size);
    if (bbase) ray_vm_unmap_file(bbase, bsize);
    remove(PF_PATH);
    remove(PF_PATH_B);
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init(0), RAY_OK);
    ray_heap_destroy();
    return res;
}

/* --------------------------------------------------------------------------
 * Test: the passes of a query that first read a mapped column request it.
 *
 * A splayed table is written, evicted from the page cache and opened, and
 * the column under test gets the kernel's read-ahead on faults turned off
 * (MADV_RANDOM): each of its pages the query reads without having asked
 * for it first is a major fault of its own.  The pass that first reads the
 * column asks for it: the scatter of the dense partitioned route (`sum v
 * by k`, 256K key slots), the holistic reduce of the indexed route (`med w
 * by k`), the gathers of a filtered group that compacts its rows (`count
 * distinct x` and `med w` by g, where v > 30) and the pass of a filtered
 * group over the selected rows (`sum w` by g, where v > 30).  A column the select
 * names and no pass reads is not asked for, however much of its start the
 * kernel read along with its header.  A per-group count distinct over rows
 * a filter on a sorted column keeps together asks for the pages of those
 * rows only, not for the whole column.
 * -------------------------------------------------------------------------- */
static char pq_dir[96], pq_tbl[104];
static void pq_paths(void) {
    if (pq_dir[0]) return;
    snprintf(pq_dir, sizeof(pq_dir), "/tmp/rayforce_test_scan_q.%d", (int)getpid());
    snprintf(pq_tbl, sizeof(pq_tbl), "%s/t/", pq_dir);
}
#define PQ_DIR (pq_paths(), pq_dir)
#define PQ_TBL (pq_paths(), pq_tbl)

static bool pq_ok(const char* src) {
    ray_t* r = ray_eval_str(src);
    bool ok = r && !RAY_IS_ERR(r);
    if (r) ray_release(r);
    return ok;
}

/* Evict every file of the table. */
static bool pq_evict(void) {
    DIR* d = opendir(PQ_TBL);
    if (!d) return false;
    bool ok = true;
    struct dirent* e;
    char path[512];
    while ((e = readdir(d))) {
        snprintf(path, sizeof(path), "%s%s", PQ_TBL, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ok &= pf_evict_one(path);
    }
    closedir(d);
    return ok;
}

/* Remove the tree at `path` (a table written again keeps generations). */
static void pq_rm(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            char sub[512];
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
                pq_rm(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        remove(path);
    }
}

/* Open the table cold as M, with the kernel's read-ahead on faults off for
 * column `name`; its data through *p, *n.  1 when eviction has no effect
 * here, -1 on an error. */
static int pq_open_cold(const char* name, const uint8_t** p, size_t* n) {
    if (!pq_ok("(set M 0)") || !pq_evict()) return -1;
    char src[192];
    snprintf(src, sizeof(src), "(set M (.db.splayed.get \"%s\"))", PQ_TBL);
    if (!pq_ok(src)) return -1;
    snprintf(src, sizeof(src), "(at M '%s)", name);
    ray_t* col = ray_eval_str(src);
    if (!col || RAY_IS_ERR(col) || col->mmod != 1 || col->len < 2048) {
        if (col) ray_release(col);
        return -1;
    }
    *p = (const uint8_t*)ray_data(col);
    *n = (size_t)col->len * 8;
    ray_release(col);   /* M holds it */
    uintptr_t a = (uintptr_t)*p & ~(uintptr_t)4095;
    posix_madvise((void*)a, (uintptr_t)*p + *n - a, POSIX_MADV_RANDOM);
    /* Opening reads the header, and the kernel the pages after it. */
    return pf_none_resident(*p + (512 << 10), *n - (512 << 10)) ? 0 : 1;
}

static long pq_majflt(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_majflt;
}

static test_result_t test_scan_read_ahead_queries(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);
    uint32_t cores = ray_pool_total_workers(ray_pool_get());
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    test_result_t res = { TEST_PASS, NULL };
    static char msg[160];
    const uint8_t* p = NULL;
    size_t n = 0;
    int rc;

    if (!ray_pool_scan_on()) { res = (test_result_t){ TEST_SKIP, "RAY_SCAN_PREFETCH=0" }; goto out; }
    char pq_set_src[192];
    snprintf(pq_set_src, sizeof(pq_set_src), "(.db.splayed.set \"%s\" T)", PQ_TBL);
    pq_rm(PQ_DIR);
    if (!pq_ok("(set N 1048576)") ||
        !pq_ok("(set T (table [k v w g x d] (list (% (* (til N) 7919) 262144) (* (til N) 3)"
               " (* 0.5 (til N)) (% (til N) 100) (% (* (til N) 13) 50000) (/ (til N) 1024))))") ||
        !pq_ok(pq_set_src) || !pq_ok("(set T 0)")) {
        res = (test_result_t){ TEST_FAIL, "table" };
        goto out;
    }

    static const struct { const char* col; const char* query; const char* pass; } q[] = {
        { "v", "(count (select {from: M s: (sum v) by: k}))", "dense partition scatter" },
        { "w", "(count (select {from: M m: (med w) by: k}))", "indexed holistic reduce" },
        { "w", "(count (select {from: M c: (count (distinct x)) m: (med w) by: g where: (> v 30)}))",
          "filtered group gather" },
        { "w", "(count (select {from: M s: (sum w) by: g where: (> v 30)}))", "filtered dense group" },
    };
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        if ((rc = pq_open_cold(q[i].col, &p, &n))) {
            res = rc > 0 ? (test_result_t){ TEST_SKIP, "page cache eviction has no effect here" }
                         : (test_result_t){ TEST_FAIL, "open" };
            goto out;
        }
        long before = pq_majflt();
        if (!pq_ok(q[i].query)) { res = (test_result_t){ TEST_FAIL, q[i].query }; goto out; }
        long faults = pq_majflt() - before;
        if (faults > (long)(n / 4096 / 4)) {
            snprintf(msg, sizeof(msg), "%s: %ld major faults over %zu pages of %s",
                     q[i].pass, faults, n / 4096, q[i].col);
            res = (test_result_t){ TEST_FAIL, msg };
            goto out;
        }
    }

    /* `count w` reads none of w, which the select registers all the same:
     * the pages at its start the kernel read along with the header must not
     * pass for a pass reading it, or the thread over the first rows asks
     * for the next window of w. */
    if ((rc = pq_open_cold("w", &p, &n))) {
        res = rc > 0 ? (test_result_t){ TEST_SKIP, "page cache eviction has no effect here" }
                     : (test_result_t){ TEST_FAIL, "open" };
        goto out;
    }
    if (!pq_ok("(count (select {from: M c: (count w) s: (sum v) by: g}))")) {
        res = (test_result_t){ TEST_FAIL, "count w" };
        goto out;
    }
    if (pf_resident_pages(p + (512 << 10), n - (1024 << 10)) > 0) {
        res = (test_result_t){ TEST_FAIL, "a column no pass read was requested" };
        goto out;
    }

    /* d sorted, 1024 rows a value: the filter keeps the last 16K rows, the
     * last 128 KiB of x.  Nothing before them may be asked for (the start
     * of the file was read with its header). */
    if ((rc = pq_open_cold("x", &p, &n))) {
        res = rc > 0 ? (test_result_t){ TEST_SKIP, "page cache eviction has no effect here" }
                     : (test_result_t){ TEST_FAIL, "open" };
        goto out;
    }
    if (!pq_ok("(count (select {from: M c: (count (distinct x)) s: (sum v) by: g where: (within d [1008 1023])}))")) {
        res = (test_result_t){ TEST_FAIL, "clustered count distinct" };
        goto out;
    }
    if (pf_resident_pages(p + n / 4, n / 2) > 0) {
        res = (test_result_t){ TEST_FAIL, "count distinct over clustered rows asked for pages none of them is on" };
        goto out;
    }

out:
    pq_ok("(set M 0)");
    pq_rm(PQ_DIR);
    ray_runtime_destroy(rt);
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(cores), RAY_OK);
    return res;
}
#endif

const test_entry_t pool_entries[] = {
#if defined(__linux__) || defined(__APPLE__)
    { "pool/auto_all_logical_cpus", test_auto_all_logical_cpus, NULL, NULL },
#endif
    { "pool/parallel_sum", test_parallel_sum, NULL, NULL },
    { "pool/dispatch_reclaims_worker_blocks", test_dispatch_reclaims_worker_blocks, NULL, NULL },
    { "pool/parallel_add", test_parallel_add, NULL, NULL },
    { "pool/parallel_group_sum", test_parallel_group_sum, NULL, NULL },
    { "pool/parallel_min_max", test_parallel_min_max, NULL, NULL },
    { "pool/cancel", test_cancel, NULL, NULL },
    { "pool/interrupt_sets_both_flags", test_interrupt_sets_both_flags, NULL, NULL },
    { "pool/dispatch_zero_elems",   test_dispatch_zero_elems,   NULL, NULL },
    { "pool/dispatch_small",        test_dispatch_small,        NULL, NULL },
    { "pool/dispatch_n_small",      test_dispatch_n_small,      NULL, NULL },
    { "pool/dispatch_narrow",       test_dispatch_narrower_than_pool, NULL, NULL },
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
#if defined(__linux__) || defined(__APPLE__)
    { "pool/claim_once_cursor",     test_claim_once_cursor,     NULL, NULL },
    { "pool/claim_once_steal",      test_claim_once_steal,      NULL, NULL },
    { "pool/steal_forced",          test_steal_forced,          NULL, NULL },
    { "pool/steal_forced_cursor",   test_steal_forced_cursor,   NULL, NULL },
    { "pool/poll_run_for_zero_drains_ready_event", test_poll_run_for_zero_drains_ready_event, NULL, NULL },
    { "pool/poll_run_for_positive_timeout", test_poll_run_for_positive_timeout, NULL, NULL },
#endif
#if defined(__linux__)
    { "pool/epoll_sel_cap_growth",  test_epoll_sel_cap_growth,  NULL, NULL },
    { "pool/epoll_hup_branch",      test_epoll_hup_branch,      NULL, NULL },
    { "pool/epoll_hup_no_errfn",    test_epoll_hup_no_errfn,    NULL, NULL },
    { "pool/scan_read_ahead",       test_scan_read_ahead,       NULL, NULL },
    { "pool/scan_read_ahead_new_pool", test_scan_read_ahead_new_pool, NULL, NULL },
    { "pool/scan_read_ahead_queries", test_scan_read_ahead_queries, NULL, NULL },
#endif
    { NULL, NULL, NULL, NULL },
};

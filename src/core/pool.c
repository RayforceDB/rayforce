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

#include "core/pool.h"
#include "mem/cow.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include <string.h>
#include <sched.h>

/* Task granularity: RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS elements per task */
#define TASK_GRAIN  ((int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS)

/* Maximum ring capacity (power of 2) */
#define MAX_RING_CAP  (1u << 16)

/* --------------------------------------------------------------------------
 * Worker thread entry
 * -------------------------------------------------------------------------- */

typedef struct {
    ray_pool_t* pool;
    uint32_t   worker_id;   /* 1-based (0 = main thread) */
} worker_ctx_t;

static void worker_loop(void* arg) {
    worker_ctx_t wctx = *(worker_ctx_t*)arg;
    ray_sys_free(arg);

    ray_pool_t* pool = wctx.pool;

    /* Each worker thread gets its own heap */
    ray_heap_init();
    ray_rc_sync = true;  /* workers always use atomic refcounting */

    for (;;) {
        ray_sem_wait(&pool->work_ready);

        if (atomic_load_explicit(&pool->shutdown, memory_order_acquire))
            break;

        /* Claim and execute tasks until ring is drained */
        for (;;) {
            uint32_t idx = atomic_fetch_add_explicit(&pool->task_tail, 1,
                                                     memory_order_acq_rel);
            if (idx >= atomic_load_explicit(&pool->task_count,
                                            memory_order_acquire))
                break;

            /* Skip execution if query was cancelled */
            if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                  memory_order_relaxed))) {
                atomic_fetch_sub_explicit(&pool->pending, 1,
                                          memory_order_acq_rel);
                continue;
            }

            ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
            t->fn(t->ctx, wctx.worker_id, t->start, t->end);

            atomic_fetch_sub_explicit(&pool->pending, 1,
                                      memory_order_acq_rel);
        }

        /* No ray_heap_gc() here — removing worker GC between dispatch rounds
         * ensures main can safely modify worker heaps in ray_parallel_end().
         * Eager madvise in heap_coalesce already releases pages on free. */
    }

    ray_heap_destroy();
}

/* --------------------------------------------------------------------------
 * ray_pool_create
 * -------------------------------------------------------------------------- */

ray_err_t ray_pool_create(ray_pool_t* pool, uint32_t n_workers) {
    /* conc-L7: memset zeroes all fields including the `cancelled` atomic,
     * which resets any cancellation state from a prior pool instance. */
    memset(pool, 0, sizeof(*pool));
    /* H3: Re-initialize atomic fields after memset — memset produces a
     * valid zero bit pattern on all supported platforms, but C11 requires
     * atomic_init for well-defined atomic semantics. */
    atomic_init(&pool->shutdown, 0);
    atomic_init(&pool->task_tail, 0);
    atomic_init(&pool->task_count, 0);
    atomic_init(&pool->pending, 0);
    atomic_init(&pool->cancelled, 0);

    if (n_workers == 0) {
        uint32_t ncpu = ray_thread_count();
        n_workers = (ncpu > 1) ? ncpu - 1 : 0;
    }

    pool->n_workers = n_workers;
    atomic_store_explicit(&pool->shutdown, 0, memory_order_relaxed);

    /* Allocate task ring */
    pool->task_cap = 1024;
    if (pool->task_cap < MAX_RING_CAP) {
        /* Will grow if needed in dispatch */
    }
    pool->tasks = (ray_pool_task_t*)ray_sys_alloc(pool->task_cap * sizeof(ray_pool_task_t));
    if (!pool->tasks) return RAY_ERR_OOM;

    pool->task_head = 0;
    atomic_store_explicit(&pool->task_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->task_count, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->pending, 0, memory_order_relaxed);

    ray_err_t err = ray_sem_init(&pool->work_ready, 0);
    if (err != RAY_OK) {
        ray_sys_free(pool->tasks);
        return err;
    }

    /* Spawn worker threads */
    if (n_workers > 0) {
        pool->threads = (ray_thread_t*)ray_sys_alloc(n_workers * sizeof(ray_thread_t));
        if (!pool->threads) {
            ray_sem_destroy(&pool->work_ready);
            ray_sys_free(pool->tasks);
            return RAY_ERR_OOM;
        }

        for (uint32_t i = 0; i < n_workers; i++) {
            worker_ctx_t* wctx = (worker_ctx_t*)ray_sys_alloc(sizeof(worker_ctx_t));
            if (!wctx) {
                /* Partial cleanup: shut down already-started threads */
                atomic_store_explicit(&pool->shutdown, 1, memory_order_release);
                for (uint32_t j = 0; j < i; j++) {
                    ray_sem_signal(&pool->work_ready);
                }
                for (uint32_t j = 0; j < i; j++) {
                    ray_thread_join(pool->threads[j]);
                }
                ray_sys_free(pool->threads);
                ray_sem_destroy(&pool->work_ready);
                ray_sys_free(pool->tasks);
                return RAY_ERR_OOM;
            }
            wctx->pool = pool;
            wctx->worker_id = i + 1;  /* 0 = main thread */

            err = ray_thread_create(&pool->threads[i], worker_loop, wctx);
            if (err != RAY_OK) {
                ray_sys_free(wctx);
                atomic_store_explicit(&pool->shutdown, 1, memory_order_release);
                for (uint32_t j = 0; j < i; j++) {
                    ray_sem_signal(&pool->work_ready);
                }
                for (uint32_t j = 0; j < i; j++) {
                    ray_thread_join(pool->threads[j]);
                }
                ray_sys_free(pool->threads);
                ray_sem_destroy(&pool->work_ready);
                ray_sys_free(pool->tasks);
                return err;
            }
        }
    }

    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_pool_free
 * -------------------------------------------------------------------------- */

void ray_pool_free(ray_pool_t* pool) {
    if (!pool) return;

    /* Signal shutdown and wake all workers */
    atomic_store_explicit(&pool->shutdown, 1, memory_order_release);
    for (uint32_t i = 0; i < pool->n_workers; i++) {
        ray_sem_signal(&pool->work_ready);
    }

    /* Join all worker threads */
    for (uint32_t i = 0; i < pool->n_workers; i++) {
        ray_thread_join(pool->threads[i]);
    }

    ray_sys_free(pool->threads);
    ray_sem_destroy(&pool->work_ready);
    ray_sys_free(pool->tasks);
    memset(pool, 0, sizeof(*pool));
}

/* --------------------------------------------------------------------------
 * ray_pool_dispatch
 * -------------------------------------------------------------------------- */

/* M2: Caller (ray_execute) must reset pool->cancelled before dispatching.
 * The cancelled flag is per-query state; failing to clear it causes all
 * subsequent dispatches to skip task execution. */
void ray_pool_dispatch(ray_pool_t* pool, ray_pool_fn fn, void* ctx,
                      int64_t total_elems) {
    if (total_elems <= 0) return;

    /* Calculate number of tasks.
     * Overflow guard: total_elems + grain - 1 could wrap for extreme values. */
    int64_t grain = TASK_GRAIN;
    if (RAY_UNLIKELY(total_elems > INT64_MAX - grain + 1))
        total_elems = INT64_MAX - grain + 1;
    uint32_t n_tasks = (uint32_t)((total_elems + grain - 1) / grain);

    /* conc-L6: Ring growth is safe without synchronization because dispatch is
     * single-producer: only the main thread (the dispatch caller) writes to
     * task_head, tasks[], and task_cap. Workers only read via task_tail after
     * the publish fence (task_count store-release). */
    if (n_tasks > pool->task_cap) {
        uint32_t new_cap = pool->task_cap;
        while (new_cap < n_tasks && new_cap < MAX_RING_CAP) new_cap *= 2;
        if (new_cap > pool->task_cap) {
            ray_pool_task_t* new_tasks = (ray_pool_task_t*)ray_sys_realloc(
                pool->tasks, new_cap * sizeof(ray_pool_task_t));
            if (new_tasks) {
                pool->tasks = new_tasks;
                pool->task_cap = new_cap;
            }
        }
    }

    /* Clamp n_tasks to task_cap to prevent ring overflow */
    if (n_tasks > pool->task_cap) {
        n_tasks = pool->task_cap;
        grain = (total_elems + n_tasks - 1) / n_tasks;
    }

    /* Fill task ring */
    for (uint32_t i = 0; i < n_tasks; i++) {
        int64_t start = (int64_t)i * grain;
        int64_t end = start + grain;
        if (end > total_elems) end = total_elems;

        uint32_t slot = i & (pool->task_cap - 1);
        pool->tasks[slot].fn = fn;
        pool->tasks[slot].ctx = ctx;
        pool->tasks[slot].start = start;
        pool->tasks[slot].end = end;
    }

    pool->task_head = n_tasks;
    atomic_store_explicit(&pool->task_count, n_tasks, memory_order_release);
    atomic_store_explicit(&pool->task_tail, 0, memory_order_release);
    atomic_store_explicit(&pool->pending, n_tasks, memory_order_release);

    /* Mark parallel region: workers are about to run, cross-heap
     * freelist modification is unsafe until spin-wait completes. */
    atomic_store_explicit(&ray_parallel_flag, 1, memory_order_release);

    /* Main thread enters atomic refcount mode during parallel dispatch */
    ray_rc_sync = true;

    /* Wake worker threads */
    for (uint32_t i = 0; i < pool->n_workers; i++) {
        ray_sem_signal(&pool->work_ready);
    }

    /* Main thread participates as worker 0 */
    for (;;) {
        uint32_t idx = atomic_fetch_add_explicit(&pool->task_tail, 1,
                                                 memory_order_acq_rel);
        if (idx >= n_tasks) break;

        if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                              memory_order_relaxed))) {
            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
            continue;
        }

        ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
        t->fn(t->ctx, 0, t->start, t->end);

        atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
    }

    /* Spin-wait for workers to finish remaining tasks.
     * No semaphore — avoids surplus-signal bug between consecutive dispatches. */
    {
        unsigned spin_count = 0;
        while (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
            if (++spin_count % 1024 == 0) sched_yield();
        }
    }

    /* All tasks done, workers heading to sem_wait (no GC in loop).
     * Safe for main to modify worker heaps between dispatches. */
    atomic_store_explicit(&ray_parallel_flag, 0, memory_order_release);

    /* Memory fence ensures all worker RC operations are visible before
     * main thread switches to non-atomic refcounting.  Workers may still
     * be between pending-- and sem_wait. */
    atomic_thread_fence(memory_order_seq_cst);
    ray_rc_sync = false;
}

/* --------------------------------------------------------------------------
 * ray_pool_dispatch_n — dispatch exactly n_tasks tasks, each [i, i+1)
 * -------------------------------------------------------------------------- */

void ray_pool_dispatch_n(ray_pool_t* pool, ray_pool_fn fn, void* ctx,
                         uint32_t n_tasks) {
    if (n_tasks == 0) return;

    /* Grow ring if needed */
    if (n_tasks > pool->task_cap) {
        uint32_t new_cap = pool->task_cap;
        while (new_cap < n_tasks && new_cap < MAX_RING_CAP) new_cap *= 2;
        if (new_cap > pool->task_cap) {
            ray_pool_task_t* new_tasks = (ray_pool_task_t*)ray_sys_realloc(
                pool->tasks, new_cap * sizeof(ray_pool_task_t));
            if (new_tasks) {
                pool->tasks = new_tasks;
                pool->task_cap = new_cap;
            }
        }
    }

    /* Clamp n_tasks to task_cap to prevent ring overflow */
    if (n_tasks > pool->task_cap) n_tasks = pool->task_cap;

    /* Fill task ring: one task per partition */
    for (uint32_t i = 0; i < n_tasks; i++) {
        uint32_t slot = i & (pool->task_cap - 1);
        pool->tasks[slot].fn = fn;
        pool->tasks[slot].ctx = ctx;
        pool->tasks[slot].start = (int64_t)i;
        pool->tasks[slot].end = (int64_t)i + 1;
    }

    pool->task_head = n_tasks;
    atomic_store_explicit(&pool->task_count, n_tasks, memory_order_release);
    atomic_store_explicit(&pool->task_tail, 0, memory_order_release);
    atomic_store_explicit(&pool->pending, n_tasks, memory_order_release);

    atomic_store_explicit(&ray_parallel_flag, 1, memory_order_release);
    ray_rc_sync = true;

    /* Wake worker threads */
    for (uint32_t i = 0; i < pool->n_workers; i++) {
        ray_sem_signal(&pool->work_ready);
    }

    /* Main thread participates as worker 0 */
    for (;;) {
        uint32_t idx = atomic_fetch_add_explicit(&pool->task_tail, 1,
                                                 memory_order_acq_rel);
        if (idx >= n_tasks) break;

        if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                              memory_order_relaxed))) {
            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
            continue;
        }

        ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
        t->fn(t->ctx, 0, t->start, t->end);

        atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
    }

    /* Spin-wait for workers to finish remaining tasks */
    {
        unsigned spin_count = 0;
        while (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
            if (++spin_count % 1024 == 0) sched_yield();
        }
    }

    atomic_store_explicit(&ray_parallel_flag, 0, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    ray_rc_sync = false;
}

/* --------------------------------------------------------------------------
 * Global pool singleton (lazy init)
 * -------------------------------------------------------------------------- */

/* L4: Global singleton; not destroyed at program exit (OS reclaims resources).
 * May cause ASan leak reports — suppress via LSAN_OPTIONS=detect_leaks=0 or
 * an explicit ray_pool_destroy() call before exit. */
static ray_pool_t  g_pool;
static _Atomic(uint32_t) g_pool_init_state = 0;  /* 0=uninit, 1=initializing, 2=ready */

ray_pool_t* ray_pool_get(void) {
    uint32_t state = atomic_load_explicit(&g_pool_init_state, memory_order_acquire);
    if (state == 2) return &g_pool;
    if (state == 0) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_pool_init_state, &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            ray_err_t err = ray_pool_create(&g_pool, 0);
            if (err == RAY_OK) {
                atomic_store_explicit(&g_pool_init_state, 2, memory_order_release);
                return &g_pool;
            }
            /* Failed — allow retry */
            atomic_store_explicit(&g_pool_init_state, 0, memory_order_release);
            return NULL;
        }
    }
    /* Spin while another thread initializes or destroys.
     * M7: state==3 means the pool is being destroyed — treat as unavailable
     * and wait for it to return to state 0 (then return NULL), or become
     * state 2 if re-initialized by another thread. */
    {
        unsigned spin_count = 0;
        for (;;) {
            uint32_t s = atomic_load_explicit(&g_pool_init_state, memory_order_acquire);
            if (s == 2) return &g_pool;
            if (s == 0) return NULL;  /* init failed, not started, or destroy completed */
            /* s == 1: still initializing, s == 3: destroying — spin */
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
            if (++spin_count % 1024 == 0) sched_yield();
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API wrappers (declared in rayforce.h)
 * -------------------------------------------------------------------------- */

/* conc-L4: If ray_pool_init() is called when the pool is already initialized
 * (state==2), the n_workers parameter is silently ignored and the existing
 * pool configuration is preserved. This is by design — the pool is a
 * singleton and reconfiguration requires ray_pool_destroy() + ray_pool_init(). */
ray_err_t ray_pool_init(uint32_t n_workers) {
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_pool_init_state, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        /* Another thread is currently initializing (state==1); spin until ready */
        if (expected == 1) {
            while (atomic_load_explicit(&g_pool_init_state, memory_order_acquire) == 1) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
#endif
            }
        }
        return RAY_OK;  /* already initialized or completed during our spin */
    }
    ray_err_t err = ray_pool_create(&g_pool, n_workers);
    if (err == RAY_OK) {
        atomic_store_explicit(&g_pool_init_state, 2, memory_order_release);
    } else {
        atomic_store_explicit(&g_pool_init_state, 0, memory_order_release);
    }
    return err;
}

void ray_pool_destroy(void) {
    uint32_t expected = 2;
    if (!atomic_compare_exchange_strong_explicit(&g_pool_init_state, &expected, 3,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
        return;  /* not ready, or another thread is already destroying */
    ray_pool_free(&g_pool);
    atomic_store_explicit(&g_pool_init_state, 0, memory_order_release);
}

void ray_cancel(void) {
    ray_pool_t* pool = ray_pool_get();
    if (pool)
        atomic_store_explicit(&pool->cancelled, 1, memory_order_release);
}

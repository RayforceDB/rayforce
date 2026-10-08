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

#ifndef RAY_POOL_H
#define RAY_POOL_H

/*
 * pool.h -- Persistent thread pool for parallel morsel dispatch.
 *
 * Workers sleep on a semaphore and wake when ray_pool_dispatch() submits tasks.
 * The main thread participates as worker 0 (no thread spawned for it).
 * Each worker initializes its own thread-local heap via ray_heap_init().
 */

#include "core/platform.h"
#include "ops/ops.h"

/* Callback: process elements [start, end) with the given worker_id */
typedef void (*ray_pool_fn)(void* ctx, uint32_t worker_id, int64_t start, int64_t end);

/* A single work item in the task ring */
typedef struct {
    ray_pool_fn  fn;
    void*       ctx;
    int64_t     start;
    int64_t     end;
} ray_pool_task_t;

/* Per-worker claim slot of the range+steal scheduler (RAY_POOL_STEAL=1).
 *
 * `range` holds the worker's unclaimed tickets as head (low 32 bits) and
 * tail (high 32 bits) in ONE word, so the owner's head claim and a thief's
 * tail cut are both a CAS against the whole pair and can never both win the
 * same ticket.  The halves are absolute ticket numbers mod 2^32 (never
 * window-relative offsets): a stale CAS that happens to match a later
 * window's word then takes real, unclaimed tickets of THAT window, with the
 * right ring slots (slot = low bits of the ticket, task_cap <= 2^16), so the
 * ABA across windows is benign — the same reasoning that keeps task_claim
 * monotonic.  The three counters are owner-written statistics (printed at
 * destroy when RAY_POOL_TRACE is set).  One cache line per worker: the
 * owner CASes `range` every ticket, thieves touch it only when they steal. */
typedef struct {
    _Alignas(64) _Atomic(uint64_t) range;  /* head:32 | tail:32, absolute tickets */
    uint64_t own;      /* tickets run out of the worker's own share */
    uint64_t stolen;   /* tickets run that were cut from other workers' tails */
    uint64_t steals;   /* successful steal operations */
} ray_pool_slot_t;

/* Thread pool */
struct ray_pool {
    ray_thread_t*       threads;       /* worker thread handles [n_workers] */
    uint32_t           n_workers;     /* number of background threads (nproc - 1) */
    _Atomic(uint32_t)  shutdown;

    /* Heap of each worker thread [n_workers] (a ray_heap_t*), published by
     * the worker once ray_heap_init has run and cleared before it exits.
     * The dispatcher reads them at the end of every parallel region to hand
     * each worker the blocks other threads freed to it (ray_heap_reclaim_
     * worker).  Only these heaps: a worker parked on the semaphore is the
     * one thread known to touch nothing of its own until the next dispatch,
     * which is what makes draining its list from here sound.  NULL until
     * the worker is up. */
    _Atomic(void*)*     worker_heaps;

    /* SPMC task ring (single producer = main, multi consumer = workers + main).
     *
     * Claiming uses two MONOTONIC 64-bit cursors that are never reset, which
     * eliminates the cross-dispatch reset race: a worker that wakes late on a
     * surplus semaphore signal can no longer claim a half-republished slot,
     * because there is no republish — each dispatch carves a fresh window
     * [base, base+n) off the high-water mark.  A ticket maps to ring slot
     * (ticket & (task_cap-1)); consecutive dispatches' windows never alias a
     * live slot because the prior window is fully claimed before the next
     * publishes.  Workers claim via a bounded CAS (never overshoot task_limit)
     * so the cursor stays exact across dispatches; 64-bit so it never wraps. */
    ray_pool_task_t*    tasks;         /* ring buffer [task_cap] */
    uint32_t           task_cap;      /* power of 2 */
    _Atomic(uint64_t)  task_claim;    /* next ticket to claim (bounded CAS) */
    _Atomic(uint64_t)  task_limit;    /* published end of current window */

    /* Range+steal scheduler (RAY_POOL_STEAL, read once at creation; 1 by
     * default).  Each window [base, base+n) is split into one contiguous
     * ticket range per worker (main included), kept in slots[w].range; a
     * worker claims from its head and, once empty, halves the tail of the
     * other workers' ranges in a fixed victim order.  task_limit stays the
     * monotonic high-water mark the window is carved from; task_claim is
     * advanced with it only as bookkeeping.  `win_state` is odd while the
     * dispatcher is writing the range words of a window and even once they
     * are all out — a thief that cut a chunk while the window was still
     * being published runs it privately instead of installing it in its
     * own slot, which the dispatcher may be about to overwrite with that
     * worker's share.  0 = shared cursor, the pre-range behaviour. */
    uint32_t           steal;
    uint32_t           trace;         /* RAY_POOL_TRACE: histogram at destroy */
    ray_pool_slot_t*    slots;         /* [n_workers+1], 64-byte aligned */
    void*              slots_raw;     /* the allocation behind `slots` */
    _Atomic(uint64_t)  win_state;     /* 2*windows + (publishing ? 1 : 0) */

    /* Barrier */
    _Atomic(uint32_t)  pending;       /* decremented by each task completion */
    ray_sem_t           work_ready;    /* workers sleep here */

    /* Query cancellation — set by ray_cancel(), checked per-morsel */
    _Atomic(uint32_t)  cancelled;
};

/* Total workers = n_workers + 1 (main thread is worker 0) */
#define ray_pool_total_workers(p) ((p)->n_workers + 1)

/* Maximum number of tasks per ray_pool_dispatch call (ring capacity ceiling).
 * Workloads that would require more tasks must either fall back to a serial
 * path or be restructured — the dispatcher clamps n_tasks and re-derives a
 * non-morsel-aligned grain, which breaks slot math in morsel-sensitive callers
 * (e.g. exec_pred_to_selection).  Value must stay in sync with MAX_RING_CAP
 * in pool.c. */
#define RAY_POOL_MAX_TASKS  (1u << 16)

/* Initial task-ring capacity (power of 2).  ray_pool_dispatch_n callers that
 * must never trigger ring growth (its clamp-on-grow-failure silently DROPS
 * tasks) cap their task count at this value — one shared constant instead of
 * a hardcoded 1024 at every site. */
#define RAY_POOL_INIT_TASKS 1024u

/* True when a data-parallel dispatch is both worthwhile AND safe: a live
 * pool with background workers (at -c 1 the pool exists with n_workers==0),
 * at least min_elems elements to amortize task overhead, and NOT already
 * inside an in-flight dispatch — ray_pool_dispatch/_n are single-producer,
 * so a nested dispatch from a worker thread would corrupt the task ring
 * (ray_parallel_flag is raised for the duration of a dispatch). */
static inline bool ray_pool_par_dispatch_ok(ray_pool_t* p, int64_t n,
                                            int64_t min_elems) {
    return p && p->n_workers > 0 && n >= min_elems &&
           atomic_load_explicit(&ray_parallel_flag, memory_order_relaxed) == 0;
}

/* Initialize pool with n_workers background threads.
 * Pass 0 to auto-detect (nproc - 1). */
ray_err_t ray_pool_create(ray_pool_t* pool, uint32_t n_workers);

/* Shutdown and free all resources */
void ray_pool_free(ray_pool_t* pool);

/* ORDER CONTRACT.  The tasks of one window may run in ANY order and on any
 * worker: concurrently, a later task before an earlier one, and one worker
 * may run tasks from anywhere in the window (with ranges+stealing a worker
 * walks its own slice in order and then takes chunks cut from the END of
 * other workers' slices; with the shared cursor tasks start in index order
 * but still finish in any order).  Callers may rely only on (a) each task
 * running exactly once, (b) a task's identity — its [start, end) or index —
 * which is what results must be placed by, and (c) everything being done
 * when the dispatch returns.  Anything that stops early ("first N", a
 * cutoff, a bound) must be sound whatever the order: derive it from row
 * positions or values, never from "the tasks before this one have run"
 * (see ray_fused_take_select in fused_topk.c for the pattern). */

/* Dispatch fn over [0, total_elems) partitioned into morsel-sized tasks.
 * Blocks until all tasks complete. Main thread participates as worker 0. */
void ray_pool_dispatch(ray_pool_t* pool, ray_pool_fn fn, void* ctx, int64_t total_elems);

/* Dispatch exactly n_tasks tasks, each with range [i, i+1).
 * Used for partition-parallel workloads where each task is one partition. */
void ray_pool_dispatch_n(ray_pool_t* pool, ray_pool_fn fn, void* ctx, uint32_t n_tasks);

/* Global pool lifecycle (lazy singleton) */
ray_pool_t* ray_pool_get(void);

/* Public pool init/destroy (moved from rayforce.h) */
ray_err_t ray_pool_init(uint32_t n_workers);
/* Initialize by total execution participants, including the main thread.
 * Pass 0 to auto-detect. Used by the CLI, where -c is a total-core count. */
ray_err_t ray_pool_init_total(uint32_t total_workers);
void     ray_pool_destroy(void);

/* Call fn on the heap of every worker of the live singleton pool; nothing
 * when no pool is running (this never starts one).  For heap maintenance on
 * the dispatcher's thread with ray_parallel_flag clear: a parked worker
 * allocates and frees nothing until the next dispatch, so its heap may be
 * worked on from there — which holds for no other thread's heap. */
struct ray_heap;
void ray_pool_each_worker_heap(void (*fn)(struct ray_heap* h, void* ctx), void* ctx);

#endif /* RAY_POOL_H */

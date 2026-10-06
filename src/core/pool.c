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
#include "core/platform.h"   /* RAY_CPU_RELAX */
#include "core/qstats.h"     /* per-worker query-stats slab */
#include "mem/cow.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>

/* Per-worker query-statistics slab (see core/qstats.h).  Zero-initialised;
 * disabled (mode == 0) until a profiling/progress consumer turns it on. */
ray_qstats_t g_qstats;

/* Task granularity: RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS elements per task */
#define TASK_GRAIN  ((int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS)

/* Maximum ring capacity (power of 2) — mirrors RAY_POOL_MAX_TASKS in pool.h */
#define MAX_RING_CAP  RAY_POOL_MAX_TASKS

/* --------------------------------------------------------------------------
 * Worker thread entry
 * -------------------------------------------------------------------------- */

typedef struct {
    ray_pool_t* pool;
    uint32_t   worker_id;   /* 1-based (0 = main thread) */
} worker_ctx_t;

/* Claim the next ticket in the current window via a bounded CAS.  Returns true
 * and sets *out_idx when a ticket in [.., task_limit) was claimed; false when
 * the window is exhausted.  The CAS never advances task_claim past task_limit,
 * so the cursor stays EXACT across dispatches (no fetch_add overshoot) — this
 * is what lets the next dispatch carve its window off the high-water mark
 * without a reset, closing the cross-dispatch claim race.  The acquire on
 * task_limit pairs with the publishing store-release so a claimer that sees
 * the new window also sees the freshly-filled ring slots and `pending`. */
static inline bool pool_claim(ray_pool_t* pool, uint64_t* out_idx) {
    uint64_t cur = atomic_load_explicit(&pool->task_claim, memory_order_relaxed);
    for (;;) {
        uint64_t lim = atomic_load_explicit(&pool->task_limit, memory_order_acquire);
        if (cur >= lim) return false;
        if (atomic_compare_exchange_weak_explicit(&pool->task_claim, &cur, cur + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            *out_idx = cur;
            return true;
        }
        /* CAS failed: `cur` now holds the current value — retry. */
    }
}

/* --------------------------------------------------------------------------
 * Range + steal claiming (pool->steal, RAY_POOL_STEAL=1 — the default)
 *
 * The dispatcher splits every window [base, base+n) into W contiguous ticket
 * ranges, one per worker (main is worker 0), and stores each as a head:tail
 * word in slots[w].range (see ray_pool_slot_t for the encoding and why the
 * halves are absolute tickets).  Owner and thieves agree through that one
 * word only:
 *
 *   owner  CAS (h, t) -> (h+1, t)        claims ticket h, keeps running in order
 *   thief  CAS (h, t) -> (h, t-k)        takes [t-k, t), k = ceil((t-h)/2) >= 1
 *
 * Both compare the whole pair, so when owner and thief race on the last
 * ticket exactly one CAS lands and the other sees an empty range.  Half-
 * at-least-one guarantees a range nobody runs — a worker that was not woken
 * (only n-1 helpers are signalled), is still starting, or is simply slow —
 * is drained entirely by thieves.
 *
 * The word is stored without a CAS in two places only, both when it is
 * provably empty and uncontended: by the dispatcher at window open (pending
 * == 0 means every range is head == tail, and a thief never CASes an empty
 * word), and by a thief installing the chunk it has just cut into its own
 * slot (nobody but the owner adds to an owner's word).  Those two can race
 * with each other: a worker still scanning for victims when the next window
 * opens may cut a chunk before the dispatcher has reached ITS slot, and the
 * dispatcher's share store would then overwrite the installed chunk — or
 * the install would overwrite the share.  win_state closes it: it is odd
 * from before the first range store until after the last one; a thief that
 * reads it odd after its cut runs the chunk privately (no install), and a
 * thief that reads it even has, by that acquire, seen its own share land.
 * A chunk pins its window (pending > 0), so the parity it reads can only be
 * that of the window the chunk came from.
 *
 * Publish order: ring fill, pending, range words (release each), then the
 * even win_state.  A claim CAS acquires the word, so whoever runs a ticket
 * — however late it woke — sees the ring slots and pending of that window.
 * Nothing is ever reset, so the cross-dispatch reset race of pool_claim's
 * predecessor cannot reappear here either.
 * -------------------------------------------------------------------------- */

#define RANGE_PACK(h, t)  (((uint64_t)(uint32_t)(t) << 32) | (uint32_t)(h))
#define RANGE_HEAD(w)     ((uint32_t)(w))
#define RANGE_TAIL(w)     ((uint32_t)((w) >> 32))

/* Run one claimed ticket (or skip it when cancelled) and retire it. */
static inline void pool_exec(ray_pool_t* pool, uint32_t worker_id, uint32_t ticket) {
    if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled, memory_order_relaxed))) {
        atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
        return;
    }
    ray_pool_task_t* t = &pool->tasks[ticket & (pool->task_cap - 1)];
    int64_t _qs_t0; uint32_t _qs_m = ray_qstats_task_begin(&_qs_t0);
    t->fn(t->ctx, worker_id, t->start, t->end);
    ray_qstats_task_end(_qs_m, worker_id, t->end - t->start, _qs_t0);
    atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
}

/* Owner claim: the head of own range, in order.  False when it is empty. */
static inline bool pool_range_take(ray_pool_slot_t* s, uint32_t* out) {
    uint64_t cur = atomic_load_explicit(&s->range, memory_order_acquire);
    for (;;) {
        uint32_t h = RANGE_HEAD(cur), t = RANGE_TAIL(cur);
        if (h == t) return false;
        if (atomic_compare_exchange_weak_explicit(&s->range, &cur, RANGE_PACK(h + 1, t),
                memory_order_acq_rel, memory_order_acquire)) {
            *out = h;
            return true;
        }
        /* CAS failed: a thief cut the tail — `cur` is current, retry. */
    }
}

/* Victim order hook: the k-th victim (k in [1, W)) of worker w.  Plain
 * round-robin from w+1 for now; a NUMA-aware order (same-node workers
 * first, then the rest) plugs in here without touching the protocol. */
static inline uint32_t pool_victim(const ray_pool_t* pool, uint32_t w, uint32_t k) {
    uint32_t W = ray_pool_total_workers(pool);
    uint32_t v = w + k;
    return v >= W ? v - W : v;
}

/* Cut the upper half (at least one ticket) off the first non-empty victim
 * range in victim order.  Two passes: a chunk that is between its cut and
 * its install is invisible for a moment, and a worker that goes to sleep on
 * that moment would leave the thief alone with it. */
static bool pool_steal(ray_pool_t* pool, uint32_t w, uint32_t* c0, uint32_t* c1) {
    uint32_t W = ray_pool_total_workers(pool);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t k = 1; k < W; k++) {
            ray_pool_slot_t* v = &pool->slots[pool_victim(pool, w, k)];
            uint64_t cur = atomic_load_explicit(&v->range, memory_order_relaxed);
            for (;;) {
                uint32_t h = RANGE_HEAD(cur), t = RANGE_TAIL(cur);
                uint32_t rem = t - h;
                if (rem == 0) break;
                uint32_t take = (rem + 1) >> 1;
                if (atomic_compare_exchange_weak_explicit(&v->range, &cur,
                        RANGE_PACK(h, t - take), memory_order_acq_rel,
                        memory_order_relaxed)) {
                    *c0 = t - take;
                    *c1 = t;
                    return true;
                }
            }
        }
        RAY_CPU_RELAX();
    }
    return false;
}

/* Drain own range, then steal until a full victim scan finds nothing.
 * `prog` is the main thread's progress pump (false on workers).  Tickets
 * are attributed when they run: those inside the chunk last installed here
 * (or run privately) count as stolen, the rest came with the share — so
 * own + stolen over all slots is exactly the tickets executed. */
/* --------------------------------------------------------------------------
 * Scan read-ahead
 *
 * A mapped column is read on first touch, one page fault at a time per
 * worker (plus the kernel's own read-ahead), so a scan over storage with a
 * high per-request latency runs at a couple of requests in flight per
 * worker whatever the storage could serve in parallel.  A query registers
 * the mapped columns it reads (ray_pool_scan_set); while one of its
 * dispatches runs, each thread keeps the next `scan_bytes` of every
 * registered column, from the row it is at, requested (MADV_WILLNEED,
 * window by window), topping the request up once half of it is used.
 *
 * The row comes from the ticket for a dispatch over exactly the registered
 * rows (ticket base+i holds rows [i*grain, (i+1)*grain)), and from the task
 * itself (ray_pool_scan_at) for dispatch_n tasks that walk a row range.
 * Rows of zone chunks the query's filter rules out (scan->gate) are never
 * requested.  RAY_SCAN_PREFETCH sets the window in KiB (default 4096, 0
 * turns read-ahead off); it is read when the pool is created.
 * -------------------------------------------------------------------------- */
static RAY_TLS const ray_pool_scan_t* t_scan;

/* What this thread has requested for the current dispatch: rows
 * [lo[c], hi[c]) of column c.  Stale (gen differs) at a new dispatch. */
typedef struct {
    uint64_t gen;
    int64_t  lo[RAY_POOL_SCAN_MAX];
    int64_t  hi[RAY_POOL_SCAN_MAX];
} pool_scan_tls_t;
static RAY_TLS pool_scan_tls_t t_pf;

const ray_pool_scan_t* ray_pool_scan_set(const ray_pool_scan_t* scan) {
    const ray_pool_scan_t* prev = t_scan;
    t_scan = scan;
    return prev;
}

const ray_pool_scan_t* ray_pool_scan_get(void) { return t_scan; }

/* Requested so far (all pools), for the RAY_POOL_TRACE summary. */
static _Atomic(uint64_t) pool_scan_req_bytes, pool_scan_req_ranges;

static void pool_scan_willneed(const ray_pool_scan_t* s, uint32_t c, int64_t a, int64_t b) {
    size_t bytes = (size_t)(b - a) * s->esz[c];
    void* p = (void*)(s->base[c] + (size_t)a * s->esz[c]);
    /* Rows an earlier dispatch of the query requested are in the page cache
     * already (or on their way in): asking again only walks those pages. */
    if (ray_vm_resident(p, bytes)) return;
    ray_vm_advise_willneed(p, bytes);
    atomic_fetch_add_explicit(&pool_scan_req_bytes, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&pool_scan_req_ranges, 1, memory_order_relaxed);
}

/* Request rows [a, b) of column c, skipping the chunks the gate rules out. */
static void pool_scan_request(const ray_pool_scan_t* s, uint32_t c, int64_t a, int64_t b) {
    if (!s->gate) { pool_scan_willneed(s, c, a, b); return; }
    uint8_t lg = s->gate_log2;
    int64_t run = -1;
    for (int64_t ch = a >> lg; (ch << lg) < b; ch++) {
        int64_t c0 = ch << lg;
        if (c0 < a) c0 = a;
        if ((s->gate[ch >> 6] >> (ch & 63)) & 1) {
            if (run < 0) run = c0;
        } else if (run >= 0) {
            pool_scan_willneed(s, c, run, c0);
            run = -1;
        }
    }
    if (run >= 0) pool_scan_willneed(s, c, run, b);
}

/* This thread is about to read row r and goes on towards `end`. */
static void pool_scan_ahead(ray_pool_t* pool, int64_t r, int64_t end) {
    const ray_pool_scan_t* s = pool->scan;
    if (end > s->rows) end = s->rows;
    if (r < 0 || r >= end) return;
    pool_scan_tls_t* t = &t_pf;
    if (t->gen != pool->scan_gen) {
        t->gen = pool->scan_gen;
        for (uint32_t c = 0; c < s->n; c++) t->lo[c] = t->hi[c] = r;
    }
    for (uint32_t c = 0; c < s->n; c++) {
        /* A jump outside what was requested starts a new run. */
        if (r < t->lo[c] || r > t->hi[c]) t->lo[c] = t->hi[c] = r;
        int64_t win = (int64_t)(pool->scan_bytes / s->esz[c]);
        if (win < 1) win = 1;
        if (t->hi[c] - r > win / 2) continue;
        int64_t want = end - r > win ? r + win : end;
        if (want <= t->hi[c]) continue;
        pool_scan_request(s, c, t->hi[c], want);
        t->hi[c] = want;
    }
}

static inline void pool_scan_ticket(ray_pool_t* pool, uint32_t tk) {
    if (!pool->scan_auto) return;
    pool_scan_ahead(pool, (int64_t)(uint32_t)(tk - (uint32_t)pool->scan_base) * pool->scan_grain,
                    pool->scan->rows);
}

static void pool_run_steal(ray_pool_t* pool, uint32_t w, bool prog) {
    ray_pool_slot_t* s = &pool->slots[w];
    uint32_t ch0 = 0, ch1 = 0;    /* installed chunk [ch0, ch1), empty at entry */
    for (;;) {
        uint32_t tk;
        while (pool_range_take(s, &tk)) {
            if ((uint32_t)(tk - ch0) < (uint32_t)(ch1 - ch0)) s->stolen++; else s->own++;
            pool_scan_ticket(pool, tk);
            pool_exec(pool, w, tk);
            if (prog) ray_progress_pump();
        }

        uint32_t c0, c1;
        if (!pool_steal(pool, w, &c0, &c1)) return;
        s->steals++;

        /* Install the chunk as own range so others can halve it in turn —
         * unless the window is still being published (odd win_state: the
         * dispatcher may be about to store this worker's share here) or
         * own range is not empty (that share already landed after the scan
         * started).  win_state is read FIRST: its acquire is what makes a
         * share the dispatcher stored visible to the own-range load, so an
         * even value and an empty word together mean no share is coming.
         * Either way the chunk is run privately, in order, and the loop
         * comes back to own range afterwards. */
        if (!(atomic_load_explicit(&pool->win_state, memory_order_acquire) & 1)) {
            uint64_t mine = atomic_load_explicit(&s->range, memory_order_acquire);
            if (RANGE_HEAD(mine) == RANGE_TAIL(mine)) {
                ch0 = c0; ch1 = c1;
                atomic_store_explicit(&s->range, RANGE_PACK(c0, c1), memory_order_release);
                continue;
            }
        }
        s->stolen += c1 - c0;
        for (; c0 != c1; c0++) {
            pool_scan_ticket(pool, c0);
            pool_exec(pool, w, c0);
            if (prog) ray_progress_pump();
        }
    }
}

/* Split tickets [base, base+n) into W contiguous ranges — the first n % W
 * workers take one ticket more — and publish them.  Caller has filled the
 * ring and stored `pending`; see the ordering note above. */
static void pool_publish_ranges(ray_pool_t* pool, uint64_t base, uint32_t n) {
    uint32_t W = ray_pool_total_workers(pool);
    uint32_t q = n / W, r = n % W;
    atomic_fetch_add_explicit(&pool->win_state, 1, memory_order_release);   /* odd */
    uint64_t at = base;
    for (uint32_t w = 0; w < W; w++) {
        uint64_t len = q + (w < r ? 1u : 0u);
        atomic_store_explicit(&pool->slots[w].range,
                              RANGE_PACK((uint32_t)at, (uint32_t)(at + len)),
                              memory_order_release);
        at += len;
    }
    atomic_fetch_add_explicit(&pool->win_state, 1, memory_order_release);   /* even */
}

/* Main thread in steal mode: own range, steal, and keep stealing while it
 * waits — a worker that was not woken has its range drained from here, so
 * completion never depends on a signal count.  Progress is pumped as in the
 * cursor path: per ticket, and once per 1024 spins. */
static void pool_main_run_steal(ray_pool_t* pool, bool prog) {
    unsigned spin_count = 0;
    for (;;) {
        pool_run_steal(pool, 0, prog);
        if (atomic_load_explicit(&pool->pending, memory_order_acquire) == 0) break;
        for (int i = 0; i < 16; i++) {
            RAY_CPU_RELAX();
            if (++spin_count % 1024 == 0) { sched_yield(); if (prog) ray_progress_pump(); }
        }
    }
}

/* RAY_POOL_TRACE: how the tickets split between own ranges and steals. */
static void pool_trace_dump(const ray_pool_t* pool) {
    uint32_t W = ray_pool_total_workers(pool);
    uint64_t own = 0, stolen = 0, steals = 0;
    for (uint32_t w = 0; w < W; w++) {
        own += pool->slots[w].own;
        stolen += pool->slots[w].stolen;
        steals += pool->slots[w].steals;
    }
    fprintf(stderr, "pool: steal=%u workers=%u windows=%llu tickets=%llu own=%llu stolen=%llu steals=%llu\n",
            pool->steal, W,
            (unsigned long long)(atomic_load_explicit(&pool->win_state, memory_order_relaxed) / 2),
            (unsigned long long)(own + stolen), (unsigned long long)own,
            (unsigned long long)stolen, (unsigned long long)steals);
    fprintf(stderr, "pool: scan read-ahead %llu KiB per column, requested %llu MiB in %llu ranges\n",
            (unsigned long long)(pool->scan_bytes >> 10),
            (unsigned long long)(atomic_load_explicit(&pool_scan_req_bytes, memory_order_relaxed) >> 20),
            (unsigned long long)atomic_load_explicit(&pool_scan_req_ranges, memory_order_relaxed));
    for (uint32_t w = 0; w < W; w++) {
        const ray_pool_slot_t* s = &pool->slots[w];
        fprintf(stderr, "pool:  w%-3u own %12llu  stolen %12llu  steals %8llu\n", w,
                (unsigned long long)s->own, (unsigned long long)s->stolen,
                (unsigned long long)s->steals);
    }
}

static void worker_loop(void* arg) {
    worker_ctx_t wctx = *(worker_ctx_t*)arg;
    ray_sys_free(arg);

    ray_pool_t* pool = wctx.pool;

    /* Each worker thread gets its own heap — a fresh one, or one abandoned
     * by an earlier worker (pools and slab caches intact). */
    ray_heap_init();
    ray_rc_sync = true;  /* workers always use atomic refcounting */
    /* Publish the heap for the dispatcher's post-dispatch reclaim (release
     * pairs with its acquire: the heap is fully initialised when seen). */
    atomic_store_explicit(&pool->worker_heaps[wctx.worker_id - 1], ray_tl_heap,
                          memory_order_release);

    for (;;) {
        ray_sem_wait(&pool->work_ready);

        if (atomic_load_explicit(&pool->shutdown, memory_order_acquire))
            break;

        if (pool->steal) {
            pool_run_steal(pool, wctx.worker_id, false);
            continue;
        }

        /* Claim and execute tasks until the window is drained */
        uint64_t idx;
        while (pool_claim(pool, &idx)) {
            /* Skip execution if query was cancelled */
            if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                  memory_order_relaxed))) {
                atomic_fetch_sub_explicit(&pool->pending, 1,
                                          memory_order_acq_rel);
                continue;
            }

            ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
            int64_t _qs_t0; uint32_t _qs_m = ray_qstats_task_begin(&_qs_t0);
            t->fn(t->ctx, wctx.worker_id, t->start, t->end);
            ray_qstats_task_end(_qs_m, wctx.worker_id, t->end - t->start, _qs_t0);

            atomic_fetch_sub_explicit(&pool->pending, 1,
                                      memory_order_acq_rel);
        }

        /* No ray_heap_gc() here — a worker that neither allocates nor frees
         * between its last pending-- and sem_wait is what lets the
         * dispatcher drain worker foreign lists (ray_heap_reclaim_workers)
         * and the idle decay walk worker heaps once the flag is clear. */
    }

    /* Abandon, do not destroy.  Another thread may still hold — and later
     * free — a block that came out of this heap, and the cross-thread free
     * path resolves the owning heap through the registry without a lock, so
     * a heap that could be unregistered and munmapped underneath that lookup
     * would be a use-after-free.  The heap stays registered with its pools
     * and is adopted by the next worker thread that starts. */
    atomic_store_explicit(&pool->worker_heaps[wctx.worker_id - 1], NULL,
                          memory_order_release);
    ray_heap_abandon();
}

/* End of a parallel region: hand every worker the blocks other threads
 * freed to it since the last dispatch, so the next round reuses them
 * instead of cutting fresh pool space (issue #619).  Workers only — they
 * are parked on the semaphore and touch nothing of their own until the
 * next dispatch; any other registered heap may belong to a live thread.
 * The dispatcher's own list is drained too, on its own thread. */
static void pool_reclaim_worker_heaps(ray_pool_t* pool) {
    for (uint32_t i = 0; i < pool->n_workers; i++) {
        ray_heap_t* h = (ray_heap_t*)atomic_load_explicit(&pool->worker_heaps[i],
                                                          memory_order_acquire);
        if (h) ray_heap_reclaim_worker(h);
    }
    ray_heap_flush_foreign();
}

/* --------------------------------------------------------------------------
 * ray_pool_create
 * -------------------------------------------------------------------------- */

static ray_err_t ray_pool_create_impl(ray_pool_t* pool, uint32_t n_workers,
                                      bool auto_size) {
    /* conc-L7: memset zeroes all fields including the `cancelled` atomic,
     * which resets any cancellation state from a prior pool instance. */
    memset(pool, 0, sizeof(*pool));
    /* H3: Re-initialize atomic fields after memset — memset produces a
     * valid zero bit pattern on all supported platforms, but C11 requires
     * atomic_init for well-defined atomic semantics. */
    atomic_init(&pool->shutdown, 0);
    atomic_init(&pool->task_claim, 0);
    atomic_init(&pool->task_limit, 0);
    atomic_init(&pool->pending, 0);
    atomic_init(&pool->cancelled, 0);
    atomic_init(&pool->win_state, 0);

    /* Claiming mode and tracing, read once here so one binary can be A/B'd:
     * RAY_POOL_STEAL=0 keeps the shared cursor, anything else (or unset)
     * selects per-worker ranges with stealing. */
    {
        const char* e = getenv("RAY_POOL_STEAL");
        pool->steal = (e && *e) ? (strtol(e, NULL, 10) != 0) : 1;
        e = getenv("RAY_SCAN_PREFETCH");
        long kib = (e && *e) ? strtol(e, NULL, 10) : 4096;
        pool->scan_bytes = kib > 0 ? (size_t)(kib > (1L << 20) ? (1L << 20) : kib) << 10 : 0;
        e = getenv("RAY_POOL_TRACE");
        pool->trace = (e && *e && strtol(e, NULL, 10) != 0);
    }

    if (auto_size) {
        /* Auto-size to ncpu-1. The RAYFORCE_CORES env var overrides this
         * default worker count — the test harness sets it (see the Makefile
         * `test` target) so neither the in-process runtime nor the server
         * children it spawns via .sys.exec each create ncpu-1 threads on a
         * many-core box. An explicit positive -c uses ray_pool_init_total()
         * with exact sizing and bypasses this entirely; a non-test run with
         * the var unset keeps the historical ncpu-1 default. */
        const char* env = getenv("RAYFORCE_CORES");
        if (env && *env) {
            long v = strtol(env, NULL, 10);
            n_workers = (v > 0) ? (uint32_t)v : 0;
        } else {
            /* Default to every online logical CPU. Individual operations may
             * bound their task count to their workload or memory budget. */
            uint32_t ncpu = ray_thread_count();
            n_workers = (ncpu > 1) ? ncpu - 1 : 0;
        }
    }

    pool->n_workers = n_workers;
    atomic_store_explicit(&pool->shutdown, 0, memory_order_relaxed);

    /* Allocate task ring */
    _Static_assert(RAY_POOL_INIT_TASKS <= RAY_POOL_MAX_TASKS,
                   "initial ring capacity must fit the ring ceiling");
    pool->task_cap = RAY_POOL_INIT_TASKS;
    if (pool->task_cap < MAX_RING_CAP) {
        /* Will grow if needed in dispatch */
    }
    pool->tasks = (ray_pool_task_t*)ray_sys_alloc(pool->task_cap * sizeof(ray_pool_task_t));
    if (!pool->tasks) return RAY_ERR_OOM;

    /* One claim slot per worker, main included, each on its own cache line
     * (ray_sys_alloc only guarantees its header alignment, hence the manual
     * round-up).  Allocated in both modes so the trace and tests can read
     * the counters regardless of the switch. */
    {
        size_t W = (size_t)n_workers + 1;
        pool->slots_raw = ray_sys_alloc(W * sizeof(ray_pool_slot_t) + 64);
        if (!pool->slots_raw) {
            ray_sys_free(pool->tasks);
            return RAY_ERR_OOM;
        }
        pool->slots = (ray_pool_slot_t*)(((uintptr_t)pool->slots_raw + 63) & ~(uintptr_t)63);
        for (size_t w = 0; w < W; w++) {
            atomic_init(&pool->slots[w].range, 0);
            pool->slots[w].own = pool->slots[w].stolen = pool->slots[w].steals = 0;
        }
    }

    atomic_store_explicit(&pool->task_claim, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->task_limit, 0, memory_order_relaxed);
    atomic_store_explicit(&pool->pending, 0, memory_order_relaxed);

    ray_err_t err = ray_sem_init(&pool->work_ready, 0);
    if (err != RAY_OK) {
        ray_sys_free(pool->slots_raw);
        ray_sys_free(pool->tasks);
        return err;
    }

    /* Spawn worker threads */
    if (n_workers > 0) {
        pool->threads = (ray_thread_t*)ray_sys_alloc(n_workers * sizeof(ray_thread_t));
        if (!pool->threads) {
            ray_sem_destroy(&pool->work_ready);
            ray_sys_free(pool->slots_raw);
            ray_sys_free(pool->tasks);
            return RAY_ERR_OOM;
        }
        pool->worker_heaps = ray_sys_alloc(n_workers * sizeof(*pool->worker_heaps));
        if (!pool->worker_heaps) {
            ray_sys_free(pool->threads);
            ray_sem_destroy(&pool->work_ready);
            ray_sys_free(pool->slots_raw);
            ray_sys_free(pool->tasks);
            return RAY_ERR_OOM;
        }
        for (uint32_t i = 0; i < n_workers; i++)
            atomic_store_explicit(&pool->worker_heaps[i], NULL, memory_order_relaxed);

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
                ray_sys_free(pool->worker_heaps);
                ray_sys_free(pool->threads);
                ray_sem_destroy(&pool->work_ready);
                ray_sys_free(pool->slots_raw);
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
                ray_sys_free(pool->worker_heaps);
                ray_sys_free(pool->threads);
                ray_sem_destroy(&pool->work_ready);
                ray_sys_free(pool->slots_raw);
                ray_sys_free(pool->tasks);
                return err;
            }
        }
    }

    return RAY_OK;
}

ray_err_t ray_pool_create(ray_pool_t* pool, uint32_t n_workers) {
    return ray_pool_create_impl(pool, n_workers, n_workers == 0);
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

    if (pool->trace && pool->slots) pool_trace_dump(pool);

    ray_sys_free(pool->worker_heaps);
    ray_sys_free(pool->threads);
    ray_sem_destroy(&pool->work_ready);
    ray_sys_free(pool->slots_raw);
    ray_sys_free(pool->tasks);
    memset(pool, 0, sizeof(*pool));
}

/* Arm read-ahead for the window about to be published: the caller's
 * registration, requested per ticket when the window covers exactly its
 * rows (`total` elements, `grain` per ticket from `base`). */
static void pool_scan_begin(ray_pool_t* pool, int64_t total, uint64_t base, int64_t grain) {
    const ray_pool_scan_t* sc = (pool->scan_bytes && t_scan && t_scan->n) ? t_scan : NULL;
    pool->scan_gen++;
    pool->scan = sc;
    pool->scan_auto = sc && pool->steal && grain > 0 && sc->rows == total;
    pool->scan_base = base;
    pool->scan_grain = grain;
}

/* The window has drained: nothing reads the registration any more. */
static inline void pool_scan_end(ray_pool_t* pool) {
    pool->scan = NULL;
    pool->scan_auto = false;
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
     * single-producer: only the main thread (the dispatch caller) writes
     * tasks[] and task_cap, and only while the prior window is fully claimed
     * (no task pending).  Workers read tasks[]/task_cap only after acquiring
     * the new task_limit (store-release), which orders the growth before any
     * claim — so a late worker never reads a stale tasks pointer or task_cap. */
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

    /* Carve a fresh window [base, base+n_tasks) off the monotonic high-water
     * mark.  The prior dispatch is fully claimed, so task_claim == task_limit
     * == base here (in steal mode: every slots[].range is head == tail and
     * the cursors were advanced to base at publish); the window's slots
     * never alias a still-live slot. */
    uint64_t base = atomic_load_explicit(&pool->task_limit, memory_order_relaxed);

    /* Fill task ring for tickets [base, base+n_tasks) */
    for (uint32_t i = 0; i < n_tasks; i++) {
        int64_t start = (int64_t)i * grain;
        int64_t end = start + grain;
        if (end > total_elems) end = total_elems;

        uint32_t slot = (uint32_t)((base + i) & (pool->task_cap - 1));
        pool->tasks[slot].fn = fn;
        pool->tasks[slot].ctx = ctx;
        pool->tasks[slot].start = start;
        pool->tasks[slot].end = end;
    }

    /* pending must be visible before the window opens; the task_limit
     * store-release publishes the ring fill AND pending to claimers.  In
     * steal mode the range words are the publication (release each) and
     * the cursors only record the high-water mark. */
    atomic_store_explicit(&pool->pending, n_tasks, memory_order_relaxed);
    /* Scan read-ahead for this window, published with the ranges. */
    pool_scan_begin(pool, total_elems, base, grain);
    if (pool->steal) {
        atomic_store_explicit(&pool->task_claim, base + n_tasks, memory_order_relaxed);
        atomic_store_explicit(&pool->task_limit, base + n_tasks, memory_order_relaxed);
        pool_publish_ranges(pool, base, n_tasks);
    } else {
        atomic_store_explicit(&pool->task_limit, base + n_tasks, memory_order_release);
    }

    /* Mark parallel region: workers are about to run, cross-heap
     * freelist modification is unsafe until spin-wait completes. */
    atomic_store_explicit(&ray_parallel_flag, 1, memory_order_release);

    /* Main thread enters atomic refcount mode during parallel dispatch */
    ray_rc_sync = true;

    /* Progress: when a callback is listening (PROGRESS bit armed), baseline the
     * row counter for this dispatch so the main thread can pump a 0→100% bar
     * from its claim/spin path.  One relaxed load; skipped entirely otherwise. */
    const bool prog = ray_qstats_mode() & RAY_QS_PROGRESS;
    if (prog) ray_progress_dispatch_begin((uint64_t)total_elems);

    /* Wake worker threads — only as many as have something to claim.  Main
     * participates as worker 0, so at most n_tasks-1 helpers can be useful;
     * signalling the whole pool woke threads that raced to an already-drained
     * window and went straight back to sleep, which is pure overhead on any
     * dispatch narrower than the machine (#599).  Signalling FEWER is safe:
     * completion is governed by the `pending` spin-wait below, never by the
     * signal count, so an unsignalled worker simply stays asleep. */
    uint32_t wake = n_tasks > 0 ? n_tasks - 1 : 0;
    if (wake > pool->n_workers) wake = pool->n_workers;
    for (uint32_t i = 0; i < wake; i++) {
        ray_sem_signal(&pool->work_ready);
    }

    /* Main thread participates as worker 0 */
    if (pool->steal) {
        pool_main_run_steal(pool, prog);
    } else {
        uint64_t idx;
        while (pool_claim(pool, &idx)) {
            if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                  memory_order_relaxed))) {
                atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
                continue;
            }

            ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
            int64_t _qs_t0; uint32_t _qs_m = ray_qstats_task_begin(&_qs_t0);
            t->fn(t->ctx, 0, t->start, t->end);
            ray_qstats_task_end(_qs_m, 0, t->end - t->start, _qs_t0);

            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
            if (prog) ray_progress_pump();
        }

        /* Spin-wait for workers to finish remaining tasks.
         * No semaphore — avoids surplus-signal bug between consecutive dispatches. */
        unsigned spin_count = 0;
        while (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0) {
            RAY_CPU_RELAX();
            if (++spin_count % 1024 == 0) { sched_yield(); if (prog) ray_progress_pump(); }
        }
    }

    pool_scan_end(pool);

    /* All tasks done, workers heading to sem_wait (no GC in loop).
     * Safe for main to modify worker heaps between dispatches. */
    atomic_store_explicit(&ray_parallel_flag, 0, memory_order_release);

    /* Memory fence ensures all worker RC operations are visible before
     * main thread switches to non-atomic refcounting.  Workers may still
     * be between pending-- and sem_wait. */
    atomic_thread_fence(memory_order_seq_cst);
    ray_rc_sync = false;

    pool_reclaim_worker_heaps(pool);
}

/* One round of ray_pool_dispatch_n: tasks [first, first+n_tasks), each handed
 * to fn as [i, i+1) with its ABSOLUTE index, n_tasks <= task_cap. */
static void dispatch_n_round(ray_pool_t* pool, ray_pool_fn fn, void* ctx,
                             uint32_t first, uint32_t n_tasks) {
    /* Carve a fresh window [base, base+n_tasks) off the monotonic high-water
     * mark (the prior dispatch is fully claimed, so base == task_claim). */
    uint64_t base = atomic_load_explicit(&pool->task_limit, memory_order_relaxed);

    /* Fill task ring: one task per partition, tickets [base, base+n_tasks) */
    for (uint32_t i = 0; i < n_tasks; i++) {
        uint32_t slot = (uint32_t)((base + i) & (pool->task_cap - 1));
        pool->tasks[slot].fn = fn;
        pool->tasks[slot].ctx = ctx;
        pool->tasks[slot].start = (int64_t)(first + i);
        pool->tasks[slot].end = (int64_t)(first + i) + 1;
    }

    atomic_store_explicit(&pool->pending, n_tasks, memory_order_relaxed);
    pool_scan_begin(pool, -1, base, 0);   /* tasks are not row ranges */
    if (pool->steal) {
        atomic_store_explicit(&pool->task_claim, base + n_tasks, memory_order_relaxed);
        atomic_store_explicit(&pool->task_limit, base + n_tasks, memory_order_relaxed);
        pool_publish_ranges(pool, base, n_tasks);
    } else {
        atomic_store_explicit(&pool->task_limit, base + n_tasks, memory_order_release);
    }

    atomic_store_explicit(&ray_parallel_flag, 1, memory_order_release);
    ray_rc_sync = true;

    /* Progress: baseline this dispatch (one unit per task/partition). */
    const bool prog = ray_qstats_mode() & RAY_QS_PROGRESS;
    if (prog) ray_progress_dispatch_begin((uint64_t)n_tasks);

    /* Wake worker threads — only as many as have something to claim.  Main
     * participates as worker 0, so at most n_tasks-1 helpers can be useful;
     * signalling the whole pool woke threads that raced to an already-drained
     * window and went straight back to sleep, which is pure overhead on any
     * dispatch narrower than the machine (#599).  Signalling FEWER is safe:
     * completion is governed by the `pending` spin-wait below, never by the
     * signal count, so an unsignalled worker simply stays asleep. */
    uint32_t wake = n_tasks > 0 ? n_tasks - 1 : 0;
    if (wake > pool->n_workers) wake = pool->n_workers;
    for (uint32_t i = 0; i < wake; i++) {
        ray_sem_signal(&pool->work_ready);
    }

    /* Main thread participates as worker 0 */
    if (pool->steal) {
        pool_main_run_steal(pool, prog);
    } else {
        uint64_t idx;
        while (pool_claim(pool, &idx)) {
            if (RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                  memory_order_relaxed))) {
                atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
                continue;
            }

            ray_pool_task_t* t = &pool->tasks[idx & (pool->task_cap - 1)];
            int64_t _qs_t0; uint32_t _qs_m = ray_qstats_task_begin(&_qs_t0);
            t->fn(t->ctx, 0, t->start, t->end);
            ray_qstats_task_end(_qs_m, 0, t->end - t->start, _qs_t0);

            atomic_fetch_sub_explicit(&pool->pending, 1, memory_order_acq_rel);
            if (prog) ray_progress_pump();
        }

        /* Spin-wait for workers to finish remaining tasks */
        unsigned spin_count = 0;
        while (atomic_load_explicit(&pool->pending, memory_order_acquire) > 0) {
            RAY_CPU_RELAX();
            if (++spin_count % 1024 == 0) { sched_yield(); if (prog) ray_progress_pump(); }
        }
    }

    pool_scan_end(pool);
    atomic_store_explicit(&ray_parallel_flag, 0, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    ray_rc_sync = false;
    pool_reclaim_worker_heaps(pool);
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

    /* The ring holds task_cap tasks at a time.  Anything past that runs in
     * further rounds rather than being dropped: a window with more
     * partitions than the ring, or a join with more morsels, used to lose
     * every task past the cap silently. */
    for (uint32_t first = 0; first < n_tasks; ) {
        uint32_t batch = n_tasks - first;
        if (batch > pool->task_cap) batch = pool->task_cap;
        dispatch_n_round(pool, fn, ctx, first, batch);
        first += batch;
    }
}

/* --------------------------------------------------------------------------
 * Global pool singleton (lazy init)
 * -------------------------------------------------------------------------- */

/* L4: Global singleton; not destroyed at program exit (OS reclaims resources).
 * May cause ASan leak reports — suppress via LSAN_OPTIONS=detect_leaks=0 or
 * an explicit ray_pool_destroy() call before exit. */
static ray_pool_t  g_pool;
static _Atomic(uint32_t) g_pool_init_state = 0;  /* 0=uninit, 1=initializing, 2=ready */

/* RAY_POOL_TRACE on the singleton: the CLI never destroys it, so the
 * histogram is printed from an atexit hook if the pool is still live then
 * (ray_pool_free prints it itself otherwise).  Workers may still be parked
 * at that point; they are idle, so the counters are quiescent. */
static void pool_trace_atexit(void) {
    if (atomic_load_explicit(&g_pool_init_state, memory_order_acquire) == 2)
        pool_trace_dump(&g_pool);
}

static void pool_trace_arm(void) {
    static bool armed = false;
    if (!armed && g_pool.trace) { armed = true; atexit(pool_trace_atexit); }
}

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
                pool_trace_arm();
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
            RAY_CPU_RELAX();
            if (++spin_count % 1024 == 0) sched_yield();
        }
    }
}

/* Read-ahead from inside a task of the singleton's open dispatch (the only
 * pool the executor's kernels run on).  Outside a dispatch `scan` is NULL. */
void ray_pool_scan_at(int64_t nrows, int64_t r, int64_t end) {
    ray_pool_t* pool = &g_pool;
    const ray_pool_scan_t* s = pool->scan;
    if (!s || s->rows != nrows) return;
    pool_scan_ahead(pool, r, end);
}

/* --------------------------------------------------------------------------
 * Public API wrappers (declared in rayforce.h)
 * -------------------------------------------------------------------------- */

/* conc-L4: If an initializer is called when the pool is already initialized
 * (state==2), its requested size is silently ignored and the existing pool
 * configuration is preserved. This is by design — the pool is a singleton
 * and reconfiguration requires ray_pool_destroy() followed by initialization. */
static ray_err_t ray_pool_init_impl(uint32_t n_workers, bool auto_size) {
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
    ray_err_t err = ray_pool_create_impl(&g_pool, n_workers, auto_size);
    if (err == RAY_OK) {
        pool_trace_arm();
        atomic_store_explicit(&g_pool_init_state, 2, memory_order_release);
    } else {
        atomic_store_explicit(&g_pool_init_state, 0, memory_order_release);
    }
    return err;
}

ray_err_t ray_pool_init(uint32_t n_workers) {
    return ray_pool_init_impl(n_workers, n_workers == 0);
}

ray_err_t ray_pool_init_total(uint32_t total_workers) {
    if (total_workers == 0)
        return ray_pool_init_impl(0, true);
    return ray_pool_init_impl(total_workers - 1, false);
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

/* Async-signal-safe: set the running query's worker-cancel flag.  Touches the
 * pool only when it is already live (state 2) — it must NOT run ray_pool_get's
 * lazy-init path (malloc/thread create) from a signal handler, and if no pool
 * exists there are no workers to stop.  Reached from ray_request_interrupt(),
 * including the SIGINT handler. */
void ray_cancel(void) {
    if (atomic_load_explicit(&g_pool_init_state, memory_order_acquire) == 2)
        atomic_store_explicit(&g_pool.cancelled, 1, memory_order_release);
}

/* Clear the worker-cancel flag at a query boundary — mirror of ray_cancel,
 * equally signal-safe.  ray_execute also resets it before each parallel
 * dispatch; this keeps the flag in sync when a query never dispatches. */
void ray_cancel_reset(void) {
    if (atomic_load_explicit(&g_pool_init_state, memory_order_acquire) == 2)
        atomic_store_explicit(&g_pool.cancelled, 0, memory_order_release);
}

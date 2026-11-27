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

#include "lang/eval.h"
#include "core/runtime.h"
#include "core/platform.h"   /* ray_stack_short: the depth guard */
#include "lang/internal.h"
#include "app/repl.h"
#include "lang/env.h"
#include "lang/sig.h"
#include "lang/inspect.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "core/types.h"
#include "ops/ops.h"
#include "ops/walkmemo.h"
#include "ops/temporal.h"
#include "ops/datalog.h"
#include "ops/idxop.h"
#include "io/json.h"
#include "ops/linkop.h"
#include "table/sym.h"
#include "vec/vec.h"
#include "core/profile.h"
#include "core/qstats.h"   /* per-worker parallelism counters for eval-path spans */
#include "core/qmeasure.h" /* shared per-query worker lifecycle */
#include "core/qlog.h"     /* ray_qlog_enabled — arm capture for query logging */
#include "lang/callprof.h" /* per-call-site profiler */
#include "store/serde.h"   /* ray_serde_size — result footprint for spans */
#include "table/sym.h"
#include "store/col.h"    /* ray_col_want_rows — gather read-ahead */
#include "mem/heap.h"
#include "core/pool.h"
#include "mem/sys.h"
#include "lang/env.h"      /* ray_fn_name — stable builtin label for spans */
/* store/serde.h, store/splay.h, store/part.h moved to system.c */
/* ray_lang_print, ray_cast_fn, etc. moved to ops/builtins.c */
/* ray_error() is declared in <rayforce.h> (included via eval.h) */

#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* Row provenance of the value the most recent ray_eval returned: 1 derived
 * from the active query's rows (a query frame's column binding, a helper's
 * source column, a row-wise builtin over one, a compiled helper's result),
 * 0 not, -1 unknown (an interpreted closure's result).  Read by the caller
 * right after the ray_eval that produced it. */
static _Thread_local int8_t g_eval_rows = -1;

/* Tri-state combination: a row operand makes the result rows; otherwise an
 * unknown operand makes it unknown. */
static inline int8_t rows_join(int8_t a, int8_t b) {
    if (a == 1 || b == 1) return 1;
    if (a < 0 || b < 0) return -1;
    return 0;
}
/* The VM's encoding of the same: 0 no, 1 rows, 2 unknown. */
static inline uint8_t vrows_join(uint8_t a, uint8_t b) {
    if (a == 1 || b == 1) return 1;
    if (a == 2 || b == 2) return 2;
    return 0;
}
static inline uint8_t vrows_from(int8_t r) { return r < 0 ? 2 : (uint8_t)r; }
static inline int8_t  vrows_to(uint8_t r)  { return r == 2 ? -1 : (int8_t)r; }

/* The provenance of the value the last ray_eval returned, in the VM's
 * encoding (0 no, 1 rows, 2 unknown): what a local bound to it keeps, as
 * `let` keeps it.  For a caller that evaluates a query's arguments itself
 * (query.c rw_eval_call). */
uint8_t ray_eval_last_rows(void) { return vrows_from(g_eval_rows); }

/* The same in the tree walker's encoding (1 rows, 0 not, -1 unknown), and
 * its setter: for a special form that builds its value from several of its
 * operands, so the value's provenance is theirs joined, not the last one's
 * (cmp.c `and` / `or`). */
int8_t ray_eval_rows(void) { return g_eval_rows; }
void ray_eval_set_rows(int8_t rows) { g_eval_rows = rows; }

#include <math.h>
#include <signal.h>
#include <time.h>

/* Maximum recursion depth for ray_eval() to prevent stack overflow.
 * The depth counter lives on the per-thread VM (__VM->eval_depth).  The
 * evaluation also stops, with the same error, once the thread's stack is
 * down to its reserve (ray_stack_short, read every fourth level): what a
 * level costs depends on the build (over 11 KB a frame under ASan at -O0),
 * and a secondary thread's stack can be far smaller than the main one's. */
#define RAY_EVAL_MAX_DEPTH 512

typedef struct {
    ray_t* vec;  /* owned until this evaluation root finishes */
    const void* data;
    int64_t len;
    int8_t type;
    uint8_t attrs;
    int64_t sum_i;
    int64_t lo, hi;  /* integer vector bounds; lo > hi for empty */
} affine_sum_cache_entry_t;

#define AFFINE_SUM_CACHE_N 16
static _Thread_local affine_sum_cache_entry_t affine_sum_cache[AFFINE_SUM_CACHE_N];
static _Thread_local uint8_t affine_sum_cache_n = 0;

static void affine_sum_cache_clear(void) {
    /* Detach before releasing: completed roots must not leave borrowed
     * identities that can match a later allocation at the same address. */
    ray_t* owners[AFFINE_SUM_CACHE_N];
    uint8_t n = affine_sum_cache_n;
    for (uint8_t i = 0; i < n; i++) owners[i] = affine_sum_cache[i].vec;
    affine_sum_cache_n = 0;
    for (uint8_t i = 0; i < n; i++) ray_release(owners[i]);
}

/* Eval-context nfo (source locations) and the error trace (list of
 * [span_i64, filename, fn_name, source] frames) live on the per-thread
 * VM — __VM->nfo / __VM->trace — so they survive across bytecode
 * invocations and stay one-per-thread, per the runtime/VM state model. */

/* Cancel state (#1623).  Two cancels, each addressed to its own target:
 *
 *  - the PROCESS cancel: the SIGINT handler, ray_request_interrupt from
 *    any thread, the test hooks.  It is the user's Ctrl-C, for the local
 *    evaluation, or for the sync request a client thread is waiting on,
 *    which forwards it to its server and consumes it (ray_take_interrupt);
 *  - the REQUEST cancel: the SIGURG handler, when a client's cancel byte
 *    arrives for the IPC request the server is evaluating.  It belongs to
 *    that request only, so it is honoured only on the thread evaluating
 *    it (and by the pool workers running that thread's dispatch).
 *
 * In a process that is an IPC server on one thread and a client on
 * another, neither can take or clear the other's cancel: a client never
 * consumes the server's request cancel, and a server request starting
 * clears only its own cancel, and the process cancel only while no sync
 * send waits for it.  While a sync send waits, the process cancel is that
 * client's to forward, so the request being evaluated does not act on it
 * either.  In separate processes (the supported setup for a client and a
 * server) nothing changes.
 *
 * Both live in one word, so a checkpoint still costs one relaxed load and
 * a branch; only a pending cancel takes the slow path that asks whether it
 * is addressed to the calling thread.  Every access is a lock-free atomic:
 * the signal handlers set the bits and read the request owner. */
#define CANCEL_PROCESS 1u
#define CANCEL_REQUEST 2u
static _Atomic unsigned g_eval_interrupted = 0;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "the interrupt flag is set from signal handlers: it must be lock-free");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
               "the request owner is read in a signal handler: it must be lock-free");

/* A thread's identity: the address of a thread-local, unique among live
 * threads and never NULL. */
static _Thread_local char t_cancel_self;
#define CANCEL_SELF() ((const void*)&t_cancel_self)
/* The thread evaluating the IPC request the request cancel is for (NULL
 * when none is), the thread whose pool dispatch is in flight (NULL when
 * none is), and how many sync sends are waiting for a reply. */
static _Atomic(const void*) g_cancel_req_thread = NULL;
static _Atomic(const void*) g_cancel_dispatcher = NULL;
static _Atomic int          g_cancel_waiters = 0;

/* Whether the calling thread works for the request being evaluated: it is
 * the thread evaluating it, or a pool worker running that thread's
 * dispatch. */
static bool cancel_on_request_thread(void) {
    const void* rq = atomic_load_explicit(&g_cancel_req_thread, memory_order_acquire);
    if (!rq) return false;
    if (rq == CANCEL_SELF()) return true;
    return ray_pool_on_worker() &&
           atomic_load_explicit(&g_cancel_dispatcher, memory_order_acquire) == rq;
}

/* Whether the cancels pending in `w` are addressed to the calling thread. */
static bool cancel_applies(unsigned w) {
    bool req = cancel_on_request_thread();
    if ((w & CANCEL_REQUEST) && req) return true;
    if (w & CANCEL_PROCESS)
        return !(req && atomic_load_explicit(&g_cancel_waiters, memory_order_acquire) > 0);
    return false;
}

/* The checkpoint's slow path, taken only while some cancel is pending. */
__attribute__((noinline, cold)) static bool cancel_applies_now(void) {
    return cancel_applies(atomic_load_explicit(&g_eval_interrupted, memory_order_acquire));
}

/* The hot-path read: a plain load on every target, ordering nothing; a
 * checkpoint only needs to see the flag eventually. */
#define EVAL_INTERRUPTED() \
    (RAY_UNLIKELY(atomic_load_explicit(&g_eval_interrupted, memory_order_relaxed) != 0) && \
     cancel_applies_now())

/* A cancel request must reach BOTH the main-thread eval/VM loops (this flag)
 * and any in-flight parallel workers (the pool's cancel flag, via ray_cancel).
 * Unifying them here means every trigger — the SIGINT handler today, an IPC
 * cancel message later — stops the whole query, not just the serial half.
 * ray_cancel/ray_cancel_reset are async-signal-safe, so this stays callable
 * from the signal handler.  Zero hot-path cost: only runs when a cancel is
 * actually requested.  The pool is left alone while a sync send waits and
 * a request is being evaluated: the cancel is the client's then, and the
 * dispatch in flight would be the request's. */
void ray_request_interrupt(void) {
    atomic_fetch_or_explicit(&g_eval_interrupted, CANCEL_PROCESS, memory_order_release);
    if (!(atomic_load_explicit(&g_cancel_req_thread, memory_order_acquire) &&
          atomic_load_explicit(&g_cancel_waiters, memory_order_acquire) > 0))
        ray_cancel();
}
/* Clears the process cancel.  The pool's flag is reset with it unless a
 * request cancel is pending, which may be what set it. */
void ray_clear_interrupt(void) {
    unsigned old = atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_PROCESS,
                                             memory_order_acq_rel);
    if (!(old & CANCEL_REQUEST)) ray_cancel_reset();
}
bool ray_interrupted(void) {
    unsigned w = atomic_load_explicit(&g_eval_interrupted, memory_order_acquire);
    return w && cancel_applies(w);
}
#ifdef DEBUG
/* The block poll of serial row loops (ops/internal.h) with its test seam:
 * DEBUG only; release polls ray_interrupted() directly. */
static _Atomic(uint32_t) g_cancel_poll_at;
void ray_test_cancel_at_poll(uint32_t nth) {
    atomic_store_explicit(&g_cancel_poll_at, nth, memory_order_relaxed);
}
bool ray_cancel_poll(void) {
    if (RAY_UNLIKELY(atomic_load_explicit(&g_cancel_poll_at, memory_order_relaxed)) &&
        atomic_fetch_sub_explicit(&g_cancel_poll_at, 1, memory_order_relaxed) == 1)
        ray_request_interrupt();
    return ray_interrupted();
}
#endif
/* Consume a pending process cancel: true, and the bit cleared, when one was
 * set; false otherwise.  One atomic read-modify-write, so a second request
 * that lands between a look and a clear is not swallowed with the first:
 * it stays set for the next take.  The one place a cancel is consumed (the
 * IPC client forwarding a Ctrl-C to its server).  It never touches the
 * request cancel: that one is the server's. */
bool ray_take_interrupt(void) {
    unsigned old = atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_PROCESS,
                                             memory_order_acq_rel);
    if (!(old & CANCEL_PROCESS)) return false;
    if (!(old & CANCEL_REQUEST)) ray_cancel_reset();
    return true;
}
bool ray_interrupt_pending(void) {
    return (atomic_load_explicit(&g_eval_interrupted, memory_order_acquire) & CANCEL_PROCESS) != 0;
}

/* ── The request cancel (src/core/ipc.c) ── */

/* Async-signal-safe: cancel the request being evaluated.  The pool's flag
 * is set only when the dispatch in flight is that request's. */
void ray_cancel_request(void) {
    atomic_fetch_or_explicit(&g_eval_interrupted, CANCEL_REQUEST, memory_order_release);
    const void* rq = atomic_load_explicit(&g_cancel_req_thread, memory_order_acquire);
    if (rq && atomic_load_explicit(&g_cancel_dispatcher, memory_order_acquire) == rq)
        ray_cancel();
}
bool ray_request_cancelled(void) {
    return (atomic_load_explicit(&g_eval_interrupted, memory_order_acquire) & CANCEL_REQUEST) &&
           cancel_on_request_thread();
}

/* A request starts on the calling thread: it owns the request cancel, which
 * starts clear (a stale one would fail it spuriously), and so does the
 * pool's flag.  The process cancel is cleared too, as before, but only
 * while no sync send waits: a waiting client's Ctrl-C is not this
 * request's to erase.  *save keeps the cancel of an outer request this one
 * runs nested in. */
void ray_cancel_request_begin(ray_cancel_save_t* save) {
    save->thread = atomic_load_explicit(&g_cancel_req_thread, memory_order_acquire);
    unsigned old = atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_REQUEST,
                                             memory_order_acq_rel);
    save->cancelled = (old & CANCEL_REQUEST) != 0;
    atomic_store_explicit(&g_cancel_req_thread, CANCEL_SELF(), memory_order_release);
    if (atomic_load_explicit(&g_cancel_waiters, memory_order_acquire) == 0)
        atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_PROCESS, memory_order_acq_rel);
    ray_cancel_reset();
}
/* The request has returned: its cancel ends with it, and an outer
 * request's comes back.  A pool flag the request cancel set is reset unless
 * a process cancel is pending, which may want it. */
void ray_cancel_request_end(const ray_cancel_save_t* save) {
    unsigned old;
    if (save->cancelled)
        old = atomic_fetch_or_explicit(&g_eval_interrupted, CANCEL_REQUEST, memory_order_acq_rel);
    else
        old = atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_REQUEST, memory_order_acq_rel);
    atomic_store_explicit(&g_cancel_req_thread, save->thread, memory_order_release);
    if ((old & CANCEL_REQUEST) && !save->cancelled && !(old & CANCEL_PROCESS))
        ray_cancel_reset();
}

/* A sync send starts (+1) or stops (-1) waiting for its reply. */
void ray_cancel_waiter(int delta) {
    atomic_fetch_add_explicit(&g_cancel_waiters, delta, memory_order_acq_rel);
}

/* The calling thread opens (or, with false, closes) a pool dispatch.
 * Opening one when a cancel addressed to this thread is pending marks the
 * pool cancelled, and one pending for another thread only is not this
 * dispatch's: the pool's flag is cleared for it. */
void ray_cancel_dispatch(bool open) {
    if (!open) {
        atomic_store_explicit(&g_cancel_dispatcher, NULL, memory_order_release);
        return;
    }
    atomic_store_explicit(&g_cancel_dispatcher, CANCEL_SELF(), memory_order_release);
    unsigned w = atomic_load_explicit(&g_eval_interrupted, memory_order_acquire);
    if (!w) return;
    if (cancel_applies(w)) ray_cancel();
    else ray_cancel_reset();
}

/* Test hook: no request owner, no waiter, no cancel. */
void ray_cancel_state_reset_for_test(void) {
    atomic_store_explicit(&g_cancel_req_thread, NULL, memory_order_release);
    atomic_store_explicit(&g_cancel_dispatcher, NULL, memory_order_release);
    atomic_store_explicit(&g_cancel_waiters, 0, memory_order_release);
    atomic_fetch_and_explicit(&g_eval_interrupted, ~CANCEL_REQUEST, memory_order_acq_rel);
}

/* Legacy internal names — thin wrappers kept for existing callers. */
void ray_eval_request_interrupt(void) { ray_request_interrupt(); }
void ray_eval_clear_interrupt(void)   { ray_clear_interrupt(); }
int  ray_eval_is_interrupted(void)    { return ray_interrupted(); }
int  ray_eval_depth(void)             { return __VM ? (int)__VM->eval_depth : 0; }

ray_t* ray_eval_get_nfo(void) { return __VM ? __VM->nfo : NULL; }
void   ray_eval_set_nfo(ray_t* nfo) { if (__VM) __VM->nfo = nfo; }

ray_t* ray_get_error_trace(void) { return __VM ? __VM->trace : NULL; }
void   ray_clear_error_trace(void) {
    if (__VM && __VM->trace) { ray_release(__VM->trace); __VM->trace = NULL; }
}

/* A (raise x) value is consumed by `try`.  An uncaught one stays in
 * __VM->raise_val until the next raise; clearing it at every top-level
 * boundary keeps a value from one evaluation from being reported against
 * an error in a later one. */
void ray_raise_clear(void) {
    if (__VM && __VM->raise_val) { ray_release(__VM->raise_val); __VM->raise_val = NULL; }
}

/* Render the pending uncaught raise value into buf (cap > 0, always
 * NUL-terminated) for an error printer.  Returns false when nothing is
 * pending.  Only error printers call this, so `raise` itself stays O(1),
 * and the cost here is bounded: a string is copied up to cap, a table is
 * summarised as "<table RxC>" (as println does), and anything else is
 * formatted in the REPL's display-limited mode.  Text cut at cap ends in
 * "..". */
bool ray_raise_render(char* buf, size_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = '\0';
    ray_t* val = __VM ? __VM->raise_val : NULL;
    if (!val) return false;
    if (val->type == RAY_TABLE) {
        snprintf(buf, cap, "<table %" PRId64 "x%" PRId64 ">",
                 ray_table_nrows(val), ray_table_ncols(val));
        return true;
    }
    ray_t* text = NULL;
    const char* p;
    size_t n;
    if (val->type == -RAY_STR) {
        p = ray_str_ptr(val);
        n = ray_str_len(val);
    } else {
        text = ray_fmt(val, 1);
        if (!text || RAY_IS_ERR(text)) { if (text) ray_error_free(text); return false; }
        p = ray_str_ptr(text);
        n = ray_str_len(text);
    }
    if (n >= cap) {
        /* Keep what fits, leaving room for ".." and the NUL. */
        n = cap > 3 ? cap - 3 : cap - 1;
        memcpy(buf, p, n);
        if (cap > 3) { buf[n++] = '.'; buf[n++] = '.'; }
    } else {
        memcpy(buf, p, n);
    }
    buf[n] = '\0';
    if (text) ray_release(text);
    return true;
}

/* ══════════════════════════════════════════
 * Restricted-mode check
 * ══════════════════════════════════════════ */

/* The flag alone; the frames a request may bind in are recorded by
 * ray_eval_restrict_enter, which is how a request is evaluated. */
void ray_eval_set_restricted(bool on) { if (__VM) __VM->restricted = on; }
bool ray_eval_get_restricted(void)    { return __VM && __VM->restricted; }

/* A request is evaluated in the restriction of the connection it came on.
 * It may arrive while server code is inside a frame (a request a peer
 * pushes back during a sync send), so the frames already open are recorded
 * as not the request's own: env.c refuses a restricted binding into them
 * as it does into the global frame.  Restores both on leave. */
void ray_eval_restrict_enter(bool on, ray_restrict_save_t* s) {
    s->restricted = __VM && __VM->restricted;
    s->base = __VM ? __VM->restricted_base : 0;
    if (!__VM) return;
    __VM->restricted = on;
    __VM->restricted_base = __VM->scope_depth;
}
void ray_eval_restrict_leave(const ray_restrict_save_t* s) {
    if (!__VM) return;
    __VM->restricted = s->restricted;
    __VM->restricted_base = s->base;
}

/* The lift log of the request in progress on this thread (see eval.h),
 * NULL when none: then every trusted call runs lifted. */
static _Thread_local ray_lift_log_t* g_lift_log;

void ray_eval_lift_record(ray_lift_log_t* log) {
    *log = (ray_lift_log_t){ .ids = log->inline_ids, .cap = 8, .prev = g_lift_log };
    g_lift_log = log;
}

void ray_eval_lift_check(ray_lift_log_t* log, const int64_t* ids, int64_t n) {
    *log = (ray_lift_log_t){ .check = true, .ids = (int64_t*)ids, .n = n, .prev = g_lift_log };
    g_lift_log = log;
}

void ray_eval_lift_done(ray_lift_log_t* log) {
    g_lift_log = log->prev;
    if (!log->check && log->ids != log->inline_ids) ray_sys_free(log->ids);
    log->ids = log->inline_ids;
}

static uint64_t trust_hash(uint64_t h, const void* bytes, int64_t n) {
    const uint8_t* p = (const uint8_t*)bytes;
    for (int64_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(0x100000001b3); }
    return h;
}

/* The identity `lambda` has when trusted under `name` (-1: as a value),
 * or 0 when a part of it cannot be serialized: it then has none, since two
 * lambdas differing only in that part would share one. */
static uint64_t trust_identity(ray_t* lambda, int64_t name) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    ray_t* ns = name >= 0 ? ray_sym_str(name) : NULL;   /* borrowed */
    if (ns) h = trust_hash(h, ray_str_ptr(ns), (int64_t)ray_str_len(ns));
    h = trust_hash(h, "", 1);   /* the name ends here */
    /* The captures too: closures of one factory differ only in them. */
    ray_t* parts[3] = { LAMBDA_PARAMS(lambda), LAMBDA_BODY(lambda), LAMBDA_CLOSURE(lambda) };
    /* Without the snapshot's trust bits, whoever asks: a captured trusted
     * lambda hashes the same live and while a snapshot loads. */
    bool keep = ray_serde_keep_marks(false);
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        ray_t* bytes = parts[i] ? ray_ser(parts[i]) : NULL;
        if (bytes && !RAY_IS_ERR(bytes)) {
            h = trust_hash(h, ray_data(bytes), bytes->len);
            ray_release(bytes);
        } else if (parts[i]) {
            if (bytes) ray_error_free(bytes);
            ok = false;
        }
        h = trust_hash(h, &i, sizeof i);   /* the part ends here */
    }
    ray_serde_keep_marks(keep);
    if (!ok) return 0;
    /* Never 0, which means untrusted; bit 0 is "trusted by name". */
    return (h << 2) | 2 | (name >= 0 ? 1 : 0);
}

bool ray_eval_lambda_trust(ray_t* lambda, int64_t name) {
    LAMBDA_TRUSTED(lambda) = trust_identity(lambda, name);
    return LAMBDA_TRUSTED(lambda) != 0;
}

/* Whether a trusted call of restricted code may run lifted, given the
 * request's lift log: recording keeps its identity (and refuses the lift
 * when it cannot, so that replay, which will find no record, agrees);
 * checking lets it run lifted only as the next call recorded live. */
static bool lift_permitted(ray_t* lambda) {
    ray_lift_log_t* log = g_lift_log;
    if (!log) return true;
    int64_t id = (int64_t)LAMBDA_TRUSTED(lambda);
    if (log->check) {
        if (log->next < log->n && log->ids[log->next] == id) { log->next++; return true; }
        return false;
    }
    if (log->n == log->cap) {
        int64_t cap = log->cap * 2;
        int64_t* ids = (int64_t*)ray_sys_alloc((size_t)cap * sizeof(int64_t));
        if (!ids) { log->failed = true; return false; }
        memcpy(ids, log->ids, (size_t)log->n * sizeof(int64_t));
        if (log->ids != log->inline_ids) ray_sys_free(log->ids);
        log->ids = ids;
        log->cap = cap;
    }
    log->ids[log->n++] = id;
    return true;
}

bool ray_fn_is_local_mutator(const ray_t* fn) {
    if (!fn || fn->type != RAY_VARY) return false;
    ray_vary_fn f = (ray_vary_fn)(uintptr_t)fn->i64;
    return f == ray_insert || f == ray_insert_fn || f == ray_alter_fn;
}

/* The head `hd` of a call, which evaluated to the mutator `head`, is the
 * literal insert or alter or a global alias of one: an unquoted symbol bound
 * in no local frame, whose global binding is that mutator.  Any other head
 * (a parameter, a let, a dotted head, a computed head) makes the mutator an
 * ordinary callee, as compile_call treats it. */
static bool mutator_head_is_global(const ray_t* hd, const ray_t* head) {
    if (!hd || hd->type != -RAY_SYM || (hd->attrs & ATTR_QUOTED)) return false;
    return !ray_env_get_local(hd->i64) && ray_env_get_global(hd->i64) == head;
}

static inline bool fn_is_restricted(ray_t* fn_obj) {
    return __VM->restricted && (fn_obj->attrs & RAY_FN_RESTRICTED);
}

/* A lambda the server marked with .sys.trust, called by restricted code:
 * its body runs with the restriction lifted, isolated from the caller's
 * scope frames and query.  call_lambda_impl lifts and isolates it, and
 * puts both back, around a call it runs to completion in a C frame of its
 * own, so they are back however the body ended (an error, a raise, a
 * return); the VM never pushes such a call as a frame of its own
 * executor. */
static inline bool lambda_lifts(ray_t* lambda) {
    return LAMBDA_TRUSTED(lambda) && __VM->restricted;
}

static inline ray_binary_fn unary_binary_overload(ray_unary_fn fn) {
    if (fn == ray_min_fn) return ray_min2_fn;
    if (fn == ray_max_fn) return ray_max2_fn;
    return NULL;
}

/* Builtins that map over a vector element by element in some argument but
 * are not registered atomic (they do not auto-map over lists): the argument
 * positions that are row-shaped (bit i: argument i+1; these builtins take
 * at most three).  An argument a builtin consumes whole -- in's set,
 * within's bounds, bin's breaks, at's source -- is not listed.  like's
 * pattern is row-shaped: a string column matches row by row (#1615).  The conformance test (lang/builtin_row_shape) measures every
 * entry, and every pure builtin's undeclared positions. */
static const struct { void* fn; uint8_t nargs; uint8_t mask; } g_row_args[] = {
    { (void*)ray_not_fn,             1, 1 }, { (void*)ray_nil_fn,             1, 1 },
    { (void*)ray_sin_fn,             1, 1 }, { (void*)ray_cos_fn,             1, 1 },
    { (void*)ray_tan_fn,             1, 1 }, { (void*)ray_asin_fn,            1, 1 },
    { (void*)ray_acos_fn,            1, 1 }, { (void*)ray_atan_fn,            1, 1 },
    { (void*)ray_reciprocal_fn,      1, 1 }, { (void*)ray_signum_fn,          1, 1 },
    { (void*)ray_extract_ss_fn,      1, 1 }, { (void*)ray_extract_minute_fn,  1, 1 },
    { (void*)ray_extract_hh_fn,      1, 1 }, { (void*)ray_extract_dd_fn,      1, 1 },
    { (void*)ray_extract_mm_fn,      1, 1 }, { (void*)ray_extract_yyyy_fn,    1, 1 },
    { (void*)ray_extract_dow_fn,     1, 1 }, { (void*)ray_extract_doy_fn,     1, 1 },
    { (void*)ray_sym_name_fn,        1, 1 }, { (void*)ray_strlen_fn,          1, 1 },
    { (void*)ray_cast_fn,            2, 2 }, { (void*)ray_xbar_fn,            2, 3 },
    { (void*)ray_in_eval_fn,         2, 1 }, { (void*)ray_not_in_fn,          2, 1 },
    { (void*)ray_like_fn,            2, 3 }, { (void*)ray_str_find_fn,        2, 1 },
    { (void*)ray_within_fn,          2, 1 },
    { (void*)ray_upper_fn,           1, 1 }, { (void*)ray_lower_fn,           1, 1 },
    { (void*)ray_trim_fn,            1, 1 }, { (void*)ray_substr_fn,          3, 1 },
    { (void*)ray_replace_fn,         3, 1 },
    /* a timestamp vector's dates and times, a truncation's values, a text
     * vector split row by row, each text's tokens (#1755) */
    { (void*)ray_date_clock_fn,      1, 1 }, { (void*)ray_time_clock_fn,      1, 1 },
    { (void*)ray_trunc_fn,           2, 2 }, { (void*)ray_split_fn,           2, 1 },
    { (void*)ray_emb_tokens_fn,      2, 2 },
};

/* Positions that do map element by element but a query reads whole by
 * decision, each with its reason.  Not row-shaped: ray_fn_row_arg leaves
 * them out; the conformance test measures that each still maps, so the
 * list cannot go stale or grow unseen. */
static const struct { void* fn; uint8_t nargs; uint8_t mask; const char* why; } g_row_args_whole[] = {
    /* lookups: neither side of a lookup is sliced unless it is a column */
    { (void*)ray_at_fn,     2, 2, "at's index is a lookup: read whole, as its source" },
    { (void*)ray_find_fn,   2, 2, "find's needles are a lookup: read whole, as its list" },
    { (void*)ray_bin_fn,    2, 2, "bin's values are a lookup: read whole, as its breaks" },
    { (void*)ray_binr_fn,   2, 2, "binr's values are a lookup: read whole, as its breaks" },
    { (void*)ray_raze_fn,   1, 1, "raze is the identity on a flat vector; on a list it changes length" },
    { (void*)ray_concat_fn, 2, 3, "a query concatenates a bound collection operand as a collection" },
};

bool ray_fn_row_arg_whole(ray_t* fn, int64_t nargs, int64_t pos, const char** why) {
    if (why) *why = NULL;
    if (!fn || fn->type < RAY_UNARY || fn->type > RAY_VARY || pos < 1 || pos > nargs)
        return false;
    void* f = (void*)(uintptr_t)fn->i64;
    for (size_t i = 0; i < sizeof g_row_args_whole / sizeof g_row_args_whole[0]; i++)
        if (g_row_args_whole[i].fn == f && g_row_args_whole[i].nargs == nargs &&
            ((g_row_args_whole[i].mask >> (pos - 1)) & 1)) {
            if (why) *why = g_row_args_whole[i].why;
            return true;
        }
    return false;
}

int64_t ray_fn_row_arity(ray_t* fn) {
    if (!fn) return 0;
    if (fn->type == RAY_UNARY) return 1;
    if (fn->type == RAY_BINARY) return 2;
    if (fn->type != RAY_VARY) return 0;
    void* f = (void*)(uintptr_t)fn->i64;
    if (f == (void*)ray_and_vary_fn || f == (void*)ray_or_vary_fn || f == (void*)ray_cond_fn)
        return 2;
    for (size_t i = 0; i < sizeof g_row_args / sizeof g_row_args[0]; i++)
        if (g_row_args[i].fn == f) return g_row_args[i].nargs;
    return 0;
}

bool ray_fn_row_arg(ray_t* fn, int64_t nargs, int64_t pos) {
    if (!fn || nargs < 1 || pos < 1 || pos > nargs) return false;
    void* f = (void*)(uintptr_t)fn->i64;
    switch (fn->type) {
    case RAY_UNARY:
        /* (min a b) / (max a b): the binary overload maps elementwise */
        if (nargs == 2 && unary_binary_overload((ray_unary_fn)f)) return true;
        if (nargs != 1) return false;
        if (fn->attrs & RAY_FN_ATOMIC) return true;
        break;
    case RAY_BINARY:
        if (nargs != 2) return false;
        if (fn->attrs & RAY_FN_ATOMIC) return true;
        if (pos == 1 && (fn->attrs & RAY_FN_LEFT_ATOMIC)) return true;
        if (pos == 2 && (fn->attrs & RAY_FN_RIGHT_ATOMIC)) return true;
        break;
    case RAY_VARY:
        /* and / or / if test and pick element by element, every operand
         * however many: an if as a query evaluates one whose test reads
         * rows (outside a query a vector test is one truth value) */
        if (f == (void*)ray_and_vary_fn || f == (void*)ray_or_vary_fn ||
            f == (void*)ray_cond_fn) return nargs >= 2;
        if (fn->attrs & RAY_FN_ATOMIC) return true;
        break;
    default:
        return false;
    }
    /* a declared builtin: pos <= nargs <= 3, inside its mask */
    for (size_t i = 0; i < sizeof g_row_args / sizeof g_row_args[0]; i++)
        if (g_row_args[i].fn == f && g_row_args[i].nargs == nargs)
            return (g_row_args[i].mask >> (pos - 1)) & 1;
    return false;
}

/* A LIST whose elements are all atoms of one numeric or temporal type, as
 * a vector of that type (the list is released); any other list as it is. */
static ray_t* list_collapse_uniform(ray_t* list) {
    if (!list || RAY_IS_ERR(list) || list->type != RAY_LIST || ray_len(list) == 0)
        return list;
    ray_t** e = (ray_t**)ray_data(list);
    int8_t t = e[0] ? e[0]->type : 0;
    switch (t) {
    case -RAY_BOOL: case -RAY_U8: case -RAY_I16: case -RAY_I32: case -RAY_I64:
    case -RAY_TIMESPAN: case -RAY_F32: case -RAY_F64: case -RAY_DATE: case -RAY_TIME: case -RAY_TIMESTAMP:
        break;
    default:
        return list;
    }
    int64_t n = ray_len(list);
    for (int64_t i = 1; i < n; i++)
        if (!e[i] || e[i]->type != t) return list;
    ray_t* vec = ray_vec_new((int8_t)-t, n);
    if (!vec || RAY_IS_ERR(vec)) return list;
    vec->len = n;
    for (int64_t i = 0; i < n; i++) store_typed_elem(vec, i, e[i]);
    ray_release(list);
    return vec;
}

/* (min a b) / (max a b) over collections.  Two collections must have the
 * same length, an error named for the call; and over a LIST the results,
 * when they share one type, make a vector of it. */
static ray_t* minmax_map(ray_binary_fn f2, ray_t* a, ray_t* b) {
    if (is_collection(a) && is_collection(b) && ray_len(a) != ray_len(b))
        return ray_error("length", "%s: operand lengths must match, got %lld and %lld",
                         f2 == ray_min2_fn ? "min" : "max",
                         (long long)ray_len(a), (long long)ray_len(b));
    /* a TIMESPAN against numbers: one vector kernel, one TIMESPAN result */
    if (ray_minmax_span_number(a, b)) return f2(a, b);
    return list_collapse_uniform(atomic_map_binary(f2, a, b));
}

static ray_t* materialize_owned_args(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        if (!args[i] || !ray_is_lazy(args[i])) continue;

        args[i] = ray_lazy_materialize(args[i]); /* consumes owned ref */
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i] ? args[i] : ray_error("type", NULL);
            args[i] = NULL;
            for (int64_t j = 0; j < n; j++) {
                if (args[j]) ray_release(args[j]);
                args[j] = NULL;
            }
            return err;
        }
    }
    return NULL;
}

static ray_t* materialize_owned_value(ray_t* x) {
    if (x && ray_is_lazy(x))
        x = ray_lazy_materialize(x);
    return x;
}

static int affine_sum_cache_lookup(ray_t* vec, affine_sum_cache_entry_t* out) {
    const void* data = ray_data(vec);
    for (uint8_t i = 0; i < affine_sum_cache_n; i++) {
        affine_sum_cache_entry_t* e = &affine_sum_cache[i];
        if (e->vec == vec && e->data == data && e->len == vec->len &&
            e->type == vec->type && e->attrs == vec->attrs) {
            *out = *e;
            return 1;
        }
    }
    return 0;
}

static void affine_sum_cache_store(ray_t* vec, const affine_sum_cache_entry_t* in) {
    /* Arena values cannot acquire an independent owner via ray_retain. */
    if (affine_sum_cache_n >= AFFINE_SUM_CACHE_N || (vec->attrs & RAY_ATTR_ARENA)) return;
    ray_retain(vec);
    affine_sum_cache[affine_sum_cache_n++] = *in;
}

/* Can evaluating `e` leave a name bound to another value?  False only when
 * every call in it is of a builtin that runs no code it is handed: an
 * operator with an opcode, an atomic or aggregate builtin, `and`, `or`,
 * `if` or `select`.  Anything else, a lambda, or a builtin that may call a
 * function value (map, fold, apply, ...) however that value was reached,
 * may run a `set`. */
static bool expr_rebinds_nothing(ray_t* e, int depth) {
    if (!e) return true;
    if (depth > 64) return false;
    if (e->type == RAY_DICT) {                           /* a query dict: its values */
        ray_t* vals = ray_dict_vals(e);
        if (!vals || vals->type != RAY_LIST) return true;
        ray_t** vv = (ray_t**)ray_data(vals);
        for (int64_t i = 0; i < ray_len(vals); i++)
            if (!expr_rebinds_nothing(vv[i], depth + 1)) return false;
        return true;
    }
    if (e->type != RAY_LIST) {                           /* a name or a literal */
        /* A dotted name that no binding or container path answers is
         * resolved by the evaluator as a method call on the container
         * (`x.eval` runs eval over x), so it may run anything: decline it.
         * ray_env_get probes bindings and containers only, never a method. */
        if (e->type == -RAY_SYM && !(e->attrs & ATTR_QUOTED) &&
            ray_sym_is_dotted(e->i64) && !ray_env_get(e->i64)) return false;
        return true;
    }
    int64_t n = ray_len(e);
    if (n == 0) return true;
    ray_t** el = (ray_t**)ray_data(e);
    ray_t* h = el[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & ATTR_QUOTED)) return false;
    ray_t* fn = ray_env_get(h->i64);
    if (!fn || (fn->attrs & RAY_FN_RESTRICTED)) return false;
    if (fn->type != RAY_UNARY && fn->type != RAY_BINARY && fn->type != RAY_VARY)
        return false;
    bool pure = RAY_FN_OPCODE(fn) != 0 ||
                (fn->attrs & (RAY_FN_ATOMIC | RAY_FN_LEFT_ATOMIC |
                              RAY_FN_RIGHT_ATOMIC | RAY_FN_AGGR));
    if (!pure && fn->type == RAY_VARY) {
        ray_vary_fn vf = (ray_vary_fn)(uintptr_t)fn->i64;
        pure = vf == ray_and_vary_fn || vf == ray_or_vary_fn ||
               vf == ray_cond_fn || vf == ray_select;
    }
    if (!pure) return false;
    for (int64_t i = 1; i < n; i++)
        if (!expr_rebinds_nothing(el[i], depth + 1)) return false;
    return true;
}

/* The count of (take x n) when it is known before x is evaluated: n an
 * integer literal, or a name bound to an integer atom that evaluating x
 * cannot rebind (expr_rebinds_nothing), above zero.  A dotted name that no
 * binding or container path answers is a method call, so it is not read
 * here.  An owned atom, else NULL (x first, then n, as any call). */
static ray_t* take_count_arg(ray_t* e, ray_t* x) {
    if (!e) return NULL;
    ray_t* v = NULL;
    if (e->type == -RAY_I64 || e->type == -RAY_I32 || e->type == -RAY_I16) {
        v = e;
        ray_retain(v);
    } else if (e->type == -RAY_SYM && !(e->attrs & ATTR_QUOTED)) {
        if (ray_sym_is_dotted(e->i64) && !ray_env_get(e->i64)) return NULL;
        if (!expr_rebinds_nothing(x, 0)) return NULL;
        v = ray_eval(e);
        if (!v) return NULL;
        if (RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    } else {
        return NULL;
    }
    if ((v->type == -RAY_I64 || v->type == -RAY_I32 || v->type == -RAY_I16) &&
        !RAY_IS_NULL(v) && as_i64(v) > 0)
        return v;
    ray_release(v);
    return NULL;
}

/* One partial per task, not per worker: tasks may be stolen. */
typedef struct { uint64_t sum; int64_t lo, hi; } affine_part_t;
typedef struct {
    const int64_t* data;
    int64_t len, grain;
    affine_part_t* parts;
} affine_scan_t;

static void affine_part_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    affine_scan_t* s = ctx;
    int64_t off = start * s->grain, n = s->len - off;
    if (n > s->grain) n = s->grain;
    const int64_t* data = s->data + off;
    affine_part_t p = { 0, INT64_MAX, INT64_MIN };
    for (int64_t i = 0; i < n; i++) {
        int64_t x = data[i];
        p.sum += (uint64_t)x;
        if (x < p.lo) p.lo = x;
        if (x > p.hi) p.hi = x;
    }
    s->parts[start] = p;
}

static bool affine_i64_scan(affine_sum_cache_entry_t* ce) {
    affine_part_t parts[64];
    affine_scan_t s = { ce->data, ce->len, ce->len, parts };
    ray_pool_t* pool = ce->len >= RAY_PARALLEL_THRESHOLD ? ray_pool_get() : NULL;
    uint32_t nt = 1;
    if (ray_pool_par_dispatch_ok(pool, ce->len, RAY_PARALLEL_THRESHOLD)) {
        nt = ray_pool_total_workers(pool);
        if (nt > sizeof(parts)/sizeof(parts[0])) nt = sizeof(parts)/sizeof(parts[0]);
        s.grain = 1 + (ce->len - 1) / nt;
        nt = (uint32_t)(1 + (ce->len - 1) / s.grain);
        ray_pool_dispatch_n(pool, affine_part_fn, &s, nt);
        if (ray_interrupted() || atomic_load_explicit(&pool->cancelled, memory_order_acquire))
            return false;
    } else {
        affine_part_fn(&s, 0, 0, 1);
    }
    affine_part_t total = parts[0];
    for (uint32_t i = 1; i < nt; i++) {
        total.sum += parts[i].sum;
        if (parts[i].lo < total.lo) total.lo = parts[i].lo;
        if (parts[i].hi > total.hi) total.hi = parts[i].hi;
    }
    ce->sum_i = (int64_t)total.sum;
    ce->lo = total.lo;
    ce->hi = total.hi;
    return true;
}

/* Borrow the ordinary addition's evaluated operands; only a syntactic
 * integer literal qualifies. NULL declines without consuming either ref. */
static bool affine_literal(ray_t* x) {
    return (x->type == -RAY_I64 || x->type == -RAY_I32 ||
            x->type == -RAY_I16 || x->type == -RAY_U8) && !RAY_ATOM_IS_NULL(x);
}

static ray_t* try_sum_affine(ray_t* vec, ray_t* literal) {
    if (vec->type != RAY_I64 || ray_vec_may_have_nulls(vec) ||
        ray_arith_type(vec->type, -literal->type) != RAY_I64)
        return NULL;
    affine_sum_cache_entry_t ce;
    if (!affine_sum_cache_lookup(vec, &ce)) {
        memset(&ce, 0, sizeof(ce));
        ce.vec = vec;
        ce.data = ray_data(vec);
        ce.len = vec->len;
        ce.type = vec->type;
        ce.attrs = vec->attrs;
        if (!ray_idx_int_bounds(vec, &ce.lo, &ce.hi)) {
            if (!affine_i64_scan(&ce)) {
                return ray_error("cancel", "interrupted");
            }
        } else {
            ray_t* sum = ray_sum_fn(vec);
            if (!sum || RAY_IS_ERR(sum)) {
                return sum ? sum : ray_error("type", NULL);
            }
            sum = materialize_owned_value(sum);
            if (!sum || RAY_IS_ERR(sum)) {
                return sum ? sum : ray_error("type", NULL);
            }
            ce.sum_i = sum->i64;
            ray_release(sum);
        }
        affine_sum_cache_store(vec, &ce);
    }

    {
        /* Only MIN - c (mod 2^64) adds to the null sentinel. Ordinary
         * wrap is safe, but sum must skip rows that produce MIN. */
        int64_t c = as_i64(literal);
        int64_t hit = (int64_t)((uint64_t)INT64_MIN - (uint64_t)c);
        if (ce.lo <= hit && hit <= ce.hi) {
            int64_t total = 0;
            for (int64_t i = 0; i < vec->len; i++) {
                int64_t x = ray_vec_get_i64(vec, i);
                if (x != hit) total = wrap_add_i64(total, wrap_add_i64(x, c));
            }
            return make_i64(total);
        }
    }
    int64_t n = vec->len;
    int64_t out = wrap_add_i64(ce.sum_i, wrap_mul_i64(as_i64(literal), n));
    return make_i64(out);
}

/* ══════════════════════════════════════════
 * Error handling: try / raise
 * ══════════════════════════════════════════ */

/* The pending (raise x) value lives on the per-thread VM
 * (__VM->raise_val) — it must survive the unwind across any number of
 * bytecode invocations on this thread's stack. */

/* The marker ray_return_fn raises: an error whose code is "return".  The
 * value travels in __VM->return_val; ray_take_return turns both back into
 * the value at a lambda's call boundary. */
static inline bool is_return_marker(ray_t* e) {
    return e && RAY_IS_ERR(e) && e->slen == 6 && memcmp(e->sdata, "return", 6) == 0;
}
static ray_t* ray_take_return(ray_t* marker) {
    ray_error_free(marker);
    ray_t* val = __VM->return_val;
    __VM->return_val = NULL;
    if (!val) { val = RAY_NULL_OBJ; ray_retain(val); }
    return val;
}

/* (raise value) — raise an error with the given value */
ray_t* ray_raise_fn(ray_t* val) {
    if (__VM->raise_val) ray_release(__VM->raise_val);
    ray_retain(val);
    __VM->raise_val = val;
    return ray_error("domain", NULL);
}

/* The errors both engines raise for the same mistake, so that their text,
 * which a `try` handler reads as msg, doesn't depend on whether the code
 * was compiled. */
static ray_t* undefined_name_error(int64_t sym) {
    ray_t* s = ray_sym_str(sym);   /* borrowed */
    if (!s) return ray_error("name", NULL);
    return ray_error("name", "'%.*s' undefined", (int)ray_str_len(s), ray_str_ptr(s));
}

static ray_t* head_not_callable_error(int8_t type) {
    return ray_error("type", "head is not callable, got %s", ray_type_name(type));
}

/* (.sys.trust f), (.sys.trust 'name), (.sys.trust ['a 'b]): mark lambdas
 * trusted.  A call to one from restricted code (-U) runs its body, and
 * whatever the body calls, with the restriction lifted and isolated from
 * the caller's scope frames and query (lambda_lifts).
 * The builtin is restricted itself, so only unrestricted code, a boot
 * script, can mark.  The mark lives on the lambda object: ser/de and IPC
 * do not carry it (a journal snapshot does, ray_serde_keep_marks), a
 * closure the body makes does not get it, and a lambda a request builds
 * has none.  A name means the global bound to it, as at
 * the top level, never a local of the caller, and is not dotted; each
 * must hold a lambda, or none is marked.  Returns its
 * argument. */
ray_t* ray_sys_trust_fn(ray_t* x) {
    if (x->type == RAY_LAMBDA) {
        /* Already trusted under a name: it keeps that identity. */
        if (!(LAMBDA_TRUSTED(x) & 1)) {
            uint64_t ident = trust_identity(x, -1);
            if (!ident) return ray_error("domain", ".sys.trust: the lambda cannot be serialized, so it has no identity");
            LAMBDA_TRUSTED(x) = ident;
        }
        ray_retain(x);
        return x;
    }
    if (x->type != -RAY_SYM && x->type != RAY_SYM)
        return ray_error("type", ".sys.trust: expected a lambda or the names of lambdas, got %s",
                         ray_type_name(x->type));
    int64_t n = x->type == -RAY_SYM ? 1 : x->len;
    /* Look the names up with every frame hidden, as at the top level. */
    int32_t scope_base = ray_env_isolate_begin();
    ray_t* err = NULL;
    uint64_t ident_buf[16];
    uint64_t* idents = n <= 16 ? ident_buf : (uint64_t*)ray_sys_alloc((size_t)n * sizeof(uint64_t));
    if (!idents) { ray_env_isolate_end(scope_base); return ray_error("oom", NULL); }
    for (int pass = 0; pass < 2 && !err; pass++) {
        for (int64_t i = 0; i < n; i++) {
            int64_t id = x->type == -RAY_SYM ? x->i64
                                             : ray_read_sym(ray_data(x), i, RAY_SYM, x->attrs);
            ray_t* fn = ray_env_get(id);   /* borrowed */
            if (pass == 1) { LAMBDA_TRUSTED(fn) = idents[i]; continue; }
            if (!fn) { err = undefined_name_error(id); break; }
            if (fn->type != RAY_LAMBDA) {
                ray_t* s = ray_sym_str(id);   /* borrowed */
                err = ray_error("type", ".sys.trust: '%.*s' is %s, not a lambda",
                                s ? (int)ray_str_len(s) : 0, s ? ray_str_ptr(s) : "",
                                ray_type_name(fn->type));
                break;
            }
            /* One object, one name: its identity includes the name, and
             * recovery gives each binding the identity of its own name, so
             * a second name would not replay. */
            /* A dotted name's lambda sits in a dict, which a snapshot loads
             * without the name: its identity would not survive recovery.
             * Such a lambda is trusted as a value. */
            if (ray_sym_is_dotted(id)) {
                ray_t* s = ray_sym_str(id);   /* borrowed */
                err = ray_error("domain", ".sys.trust: '%.*s' is a dotted name; trust the lambda as a value",
                                s ? (int)ray_str_len(s) : 0, s ? ray_str_ptr(s) : "");
                break;
            }
            idents[i] = trust_identity(fn, id);
            if (!idents[i]) {
                ray_t* s = ray_sym_str(id);   /* borrowed */
                err = ray_error("domain", ".sys.trust: the lambda '%.*s' holds cannot be serialized, so it has no identity",
                                s ? (int)ray_str_len(s) : 0, s ? ray_str_ptr(s) : "");
                break;
            }
            bool second = (LAMBDA_TRUSTED(fn) & 1) && LAMBDA_TRUSTED(fn) != idents[i];
            for (int64_t k = 0; k < i && !second; k++) {
                int64_t prev = ray_read_sym(ray_data(x), k, RAY_SYM, x->attrs);
                second = prev != id && ray_env_get(prev) == fn;
            }
            if (second) {
                ray_t* s = ray_sym_str(id);   /* borrowed */
                err = ray_error("domain", ".sys.trust: the lambda '%.*s' holds is trusted under another name",
                                s ? (int)ray_str_len(s) : 0, s ? ray_str_ptr(s) : "");
                break;
            }
        }
    }
    ray_env_isolate_end(scope_base);
    if (idents != ident_buf) ray_sys_free(idents);
    if (err) return err;
    ray_retain(x);
    return x;
}

/* Dispose of a construction temporary that was not handed on: an error is
 * freed (errors are not refcounted, so ray_release would leave it
 * allocated), anything else released. */
static void try_drop(ray_t* v) {
    if (RAY_IS_ERR(v)) ray_error_free(v);
    else ray_release(v);
}

/* The value a `try` handler receives for an error the engine raised
 * itself (a (raise x) hands it x): the dict
 * {code: 'type msg: "cannot add i64 and sym"}, the code and message the
 * REPL prints for the error, msg "" when it carries none.  An error from a
 * builtin called with the wrong arguments also has `usage`, the call shape,
 * "(take <collection: ...> <count: ...>)": a field of its own, so that the
 * message is what the builtin wrote and nobody matches on a suffix.  Called before
 * the error is freed or the handler runs, since msg is usually
 * ray_error_msg(), the thread's last error message.  null, which takes no
 * allocation, if the dict cannot be built. */
static ray_t* try_error_value(const char* code, const char* msg, const char* usage) {
    if (!code) code = "error";
    if (!msg) msg = "";
    if (usage && !*usage) usage = NULL;
    int nkeys = usage ? 3 : 2;
    /* The keys' ids hold for the symbol table's lifetime: one lookup each
     * per table, not per catch.  An epoch of 0 is no table yet. */
    static _Thread_local uint64_t key_epoch;
    static _Thread_local int64_t kc, km, ku;
    ray_t *sm, *su = NULL, *sc = NULL, *keys = NULL, *vals = NULL, *d;
    int64_t kv;
    /* The message first: a failure below makes an error of its own,
     * which overwrites the thread's last message. */
    sm = ray_str(msg, strlen(msg));
    if (RAY_IS_ERR(sm)) goto fail;
    if (usage) {
        su = ray_str(usage, strlen(usage));
        if (RAY_IS_ERR(su)) { su = NULL; goto fail; }
    }
    uint64_t epoch = ray_sym_epoch();
    if (key_epoch != epoch || !epoch) {
        kc = ray_sym_intern("code", 4);
        km = ray_sym_intern("msg", 3);
        ku = ray_sym_intern("usage", 5);
        if (kc < 0 || km < 0 || ku < 0) { key_epoch = 0; goto fail; }
        key_epoch = epoch;
    }
    kv = ray_sym_intern(code, strlen(code));
    if (kv < 0) goto fail;
    sc = ray_sym(kv);
    if (RAY_IS_ERR(sc)) goto fail;
    keys = ray_sym_vec_new(RAY_SYM_W64, nkeys);
    if (RAY_IS_ERR(keys)) goto fail;
    vals = ray_list_new(nkeys);
    if (RAY_IS_ERR(vals)) goto fail;
    /* Filled in place, vals taking sc and sm over: nothing between the
     * allocations above and the dict's own can fail. */
    ((int64_t*)ray_data(keys))[0] = kc;
    ((int64_t*)ray_data(keys))[1] = km;
    ((ray_t**)ray_data(vals))[0] = sc;
    ((ray_t**)ray_data(vals))[1] = sm;   /* not-a-table */
    if (usage) {
        ((int64_t*)ray_data(keys))[2] = ku;
        ((ray_t**)ray_data(vals))[2] = su;
        su = NULL;   /* vals owns it now */
    }
    keys->len = nkeys;
    vals->len = nkeys;
    d = ray_dict_new(keys, vals);   /* consumes keys and vals, failing too */
    if (!RAY_IS_ERR(d)) return d;
    ray_error_free(d);
    return RAY_NULL_OBJ;
fail:
    try_drop(sm);
    try_drop(su);
    try_drop(sc);
    try_drop(keys);
    try_drop(vals);
    return RAY_NULL_OBJ;
}

#ifdef DEBUG
ray_t* ray_try_error_value_for_test(const char* code, const char* msg) {
    return try_error_value(code, msg, NULL);
}
#endif

/* True when a `try` handler expression is a plain value: not a name or a
 * symbol (either can resolve to a function), not a call, not a function.
 * Such a handler is the result as it stands, so the error value it would
 * be handed is never read and need not be built.  Shared with the
 * compiler, which gives it a trap that pushes none (OP_TRAPV). */
bool ray_try_handler_is_value(ray_t* h) {
    if (!h || RAY_IS_ERR(h)) return false;
    return h->type != -RAY_SYM && h->type != RAY_LIST &&
           h->type != RAY_LAMBDA && h->type != RAY_UNARY;
}

/* Dispatch a `try` handler value against the error value.  A *callable*
 * handler (a lambda or a unary builtin) is invoked with the error and its
 * result returned; ANY other value is returned as-is as a fallback result.
 *
 * The fallback form exists because Rayfall lambdas do not capture closures,
 * so a handler lambda `(fn [e] outer)` cannot see an outer binding.  Passing
 * the value directly — evaluated in the current scope — is the only way to
 * surface an outer variable from the failure branch:
 *     ((fn [data] (try (raise "x") data)) 123)  ->  123
 *
 * Borrows both `handler` and `err_val`; returns a new owned ref. */
static ray_t* call_lambda_impl(ray_t* lambda, ray_t** call_args, int64_t argc,
                               bool query, uint64_t row_args, bool named,
                               int8_t* out_rows);

ray_t* ray_try_handle(ray_t* handler, ray_t* err_val) {
    if (handler->type == RAY_LAMBDA || handler->type == RAY_UNARY)
        return call_fn1(handler, err_val);   /* borrows err_val */
    ray_retain(handler);
    return handler;                          /* fallback value */
}

/* ray_try_handle that also reports the handler value's row provenance:
 * a lambda handler's result flag, -1 (unknown) for a builtin, and
 * `fallback_rows` for a plain fallback value. */
static ray_t* try_handle_rows(ray_t* handler, ray_t* err_val, int8_t fallback_rows, int8_t* rows) {
    *rows = -1;
    if (handler->type == RAY_LAMBDA) {
        ray_t* args[1] = { err_val };
        return call_lambda_impl(handler, args, 1, ray_active_query_table() != NULL, 0, true, rows);
    }
    if (handler->type == RAY_UNARY) return call_fn1(handler, err_val);
    *rows = fallback_rows;
    ray_retain(handler);
    return handler;
}

/* (try expr handler) — evaluate expr; on error, dispatch the (evaluated)
 * handler via ray_try_handle (callable → invoked with the error value;
 * otherwise returned as a fallback value).
 * Special form: receives unevaluated args. */
ray_t* ray_try_fn(ray_t* expr, ray_t* handler_expr) {
    return ray_try_finish(ray_eval(expr), handler_expr);
}

/* The rest of `(try expr handler)` once `expr` has been evaluated to
 * `result`: the value itself, or the handler's answer to its error.  A
 * query's per-row pass evaluates `expr` its own way and finishes here, so
 * the two agree on what is caught and how (query.c rw_eval_try). */
ray_t* ray_try_finish(ray_t* result, ray_t* handler_expr) {
    if (!RAY_IS_ERR(result)) return result;
    if (is_return_marker(result)) return result;   /* a `return` is not an error */

    /* Get error value (set by raise, or the engine error's code and
     * message, which a plain-value handler never reads) */
    ray_t* err_val = __VM->raise_val;
    __VM->raise_val = NULL;
    if (!err_val)
        err_val = ray_try_handler_is_value(handler_expr) ? RAY_NULL_OBJ
                : try_error_value(ray_err_code(result), ray_err_msg(result), ray_error_usage());
    /* The caught error itself goes no further: errors are not refcounted,
     * so ray_release would leave it allocated.  Nor does the trace its
     * unwind recorded. */
    ray_error_free(result);
    ray_clear_error_trace();

    /* Evaluate handler expression (in the current scope) */
    ray_t* handler = ray_eval(handler_expr);
    if (RAY_IS_ERR(handler)) {
        ray_release(err_val);
        return handler;
    }

    /* A lambda handler reports its result's provenance; a fallback value
     * keeps the one its expression had (g_eval_rows after evaluating it). */
    int8_t handler_rows = -1;
    ray_t* handler_result = try_handle_rows(handler, err_val, g_eval_rows, &handler_rows);
    ray_release(err_val);
    ray_release(handler);
    g_eval_rows = handler_rows;
    return handler_result;
}

/* ══════════════════════════════════════════
 * FN_ATOMIC auto-mapping helpers
 * ══════════════════════════════════════════ */

/* Convert a typed vector to a boxed list.  If already a list, retains
 * and returns it directly.  Caller owns the returned object. */
ray_t* to_boxed_list(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (x->type == RAY_LIST) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "to_boxed_list: expected a vector or list, got %s", ray_type_name(x->type));

    int64_t len = ray_len(x);
    ray_t* list = ray_alloc(len * sizeof(ray_t*));
    if (!list) return ray_error("oom", NULL);
    list->type = RAY_LIST;
    list->len = len;
    ray_t** dst = (ray_t**)ray_data(list);

    for (int64_t i = 0; i < len; i++) {
        int alloc = 0;
        dst[i] = collection_elem(x, i, &alloc);
        if (RAY_IS_ERR(dst[i])) {
            /* Trim len to the initialized prefix so the list free
             * releases exactly dst[0..i) — leaving len at the full
             * length would re-release them and then touch the
             * uninitialized tail. */
            ray_t* err = dst[i];
            list->len = i;
            ray_release(list);
            return err;
        }
        /* collection_elem always allocates for typed vecs, so ownership transfers */
    }
    return list;
}

/* Unbox a typed vector argument to a boxed list for use in builtins.
 * Sets *_bx to the allocated boxed list (caller must release) or NULL.
 * Returns the (possibly converted) argument, or an error. */
ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx) {
    *_bx = NULL;
    if (x && !RAY_IS_ERR(x) && ray_is_vec(x)) {
        *_bx = to_boxed_list(x);
        return *_bx;
    }
    return x;
}

/* Construct a zero-valued owned atom matching the element type of a
 * vector (typed or RAY_LIST).  Used only for empty-collection type
 * probing by the atomic-map helpers: it lets us invoke a binary or
 * unary `fn` with a representative scalar so the result's output
 * type is observable even when the input has no elements.
 *
 * Symbol / string / GUID columns must produce atoms of their own
 * element type — falling back to i64(0) for those would make, e.g.,
 * `(== empty_sym_col 'foo)` probe an integer comparison and return
 * I64 instead of the BOOL a non-empty input would yield.  Unknown
 * element types still fall back to ray_i64(0). */
static ray_t* zero_atom_for_elem_type(ray_t* coll) {
    if (!coll) return ray_i64(0);
    if (coll->type == RAY_LIST) return ray_i64(0);
    switch (coll->type) {
        case RAY_I64:       return ray_i64(0);
        case RAY_I32:       return ray_i32(0);
        case RAY_I16:       return ray_i16(0);
        case RAY_U8:        return ray_u8(0);
        case RAY_BOOL:      return make_bool(0);
        case RAY_F64:       return make_f64(0.0);
        case RAY_F32:       return ray_f32(0.0f);
        case RAY_DATE:      return ray_date(0);
        case RAY_TIME:      return ray_time(0);
        case RAY_TIMESPAN: return ray_timespan(0);
        case RAY_TIMESTAMP: return ray_timestamp(0);
        case RAY_SYM:       return ray_sym(0);
        case RAY_STR:       return ray_str("", 0);
        case RAY_GUID: {
            static const uint8_t zero_guid[16] = {0};
            return ray_guid(zero_guid);
        }
        default:            return ray_i64(0);
    }
}

static bool str_cells_repeat(ray_t* const* cells, int64_t n);

/* The scalar fill/min/max results for wide cells have a common type, but the
 * generic atomic mapper boxes them.  Collapse only homogeneous wide results;
 * mixed-type LIST operands keep their existing boxed shape. */
static ray_t* atomic_collapse_wide(ray_t* list) {
    if (!list || RAY_IS_ERR(list) || list->type != RAY_LIST || list->len == 0)
        return list;
    ray_t** cells = (ray_t**)ray_data(list);
    int8_t t = cells[0] ? cells[0]->type : 0;
    if (t != -RAY_SYM && t != -RAY_STR && t != -RAY_GUID) return list;
    for (int64_t i = 1; i < list->len; i++)
        if (!cells[i] || cells[i]->type != t) return list;

    if (t == -RAY_STR && str_cells_repeat(cells, list->len)) {
        ray_release(list);
        return ray_error("limit", "a string vector repeats shared strings beyond 1 GiB of extra data");
    }
    ray_t* vec = ray_vec_new((int8_t)-t, t == -RAY_STR ? 0 : list->len);
    if (!vec || RAY_IS_ERR(vec)) {
        ray_release(list);
        return vec ? vec : ray_error("oom", NULL);
    }
    if (t == -RAY_STR) {
        for (int64_t i = 0; i < list->len; i++) {
            ray_t* next = ray_str_vec_append(vec, ray_str_ptr(cells[i]), ray_str_len(cells[i]));
            if (!next || RAY_IS_ERR(next)) {
                ray_release(vec);
                ray_release(list);
                return next ? next : ray_error("oom", NULL);
            }
            vec = next;
        }
    } else {
        vec->len = list->len;
        for (int64_t i = 0; i < list->len; i++)
            if (store_typed_elem(vec, i, cells[i]) != 0) {
                ray_release(vec);
                return list;
            }
    }
    ray_release(list);
    return vec;
}

/* ── fill / dyadic min / max over a fixed-width vector ─────────────────
 *
 * (fill r v), (min a b) and (max a b) have no DAG opcode, so the mapper
 * below built one atom per cell: an allocation, a scalar call and a
 * release each, ~50 ns a row (1.2 s over a 24M-row column, as long as the
 * join that produced it).  These kernels answer the shapes the mapper's
 * probe types the same way it would, cell by cell in the result's width,
 * and decline (NULL) every other shape, which keeps its boxed route:
 *
 *   fill   an int-family or F64 vector with a non-null replacement of
 *          the vector's type (one of another type is cast to it first,
 *          fill_other_type);
 *   min/max  int-family cells (I64 I32 I16 DATE TIME TIMESTAMP) against
 *          int-family cells of the same or a promotable type, or F64
 *          against F64 / an integer, as a vector against an atom or two
 *          vectors of one length.
 *
 * Null is the payload sentinel, as the atom predicates read it
 * (ray_atom_is_null_fn), whatever the vector's HAS_NULLS flag says; a null
 * written to the result sets that flag, as store_typed_elem did.  min/max
 * order null lowest (ray_lte_fn / ray_gte_fn): min of a null and a value
 * is the null, max is the value.  DATE cells compare as their saturating
 * nanosecond conversion does (temporal_as_ns): days beyond the TIMESTAMP
 * range all order alike. */
static inline bool fastmap_int_type(int8_t t) {
    return t == RAY_I64 || t == RAY_I32 || t == RAY_I16 ||
           t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP;
}

/* cell i as int64 with its null state */
static inline int64_t fastmap_read_i64(const void* d, int8_t t, int64_t i, bool* null) {
    switch (t) {
    case RAY_I64: case RAY_TIMESTAMP: {
        int64_t v = ((const int64_t*)d)[i]; *null = v == NULL_I64; return v;
    }
    case RAY_I32: case RAY_DATE: case RAY_TIME: {
        int32_t v = ((const int32_t*)d)[i]; *null = v == NULL_I32; return v;
    }
    default: {
        int16_t v = ((const int16_t*)d)[i]; *null = v == NULL_I16; return v;
    }
    }
}

static inline void fastmap_write_i64(void* d, int8_t t, int64_t i, int64_t v, bool null) {
    switch (t) {
    case RAY_I64: case RAY_TIMESTAMP: ((int64_t*)d)[i] = null ? NULL_I64 : v; break;
    case RAY_I32: case RAY_DATE: case RAY_TIME: ((int32_t*)d)[i] = null ? NULL_I32 : (int32_t)v; break;
    default: ((int16_t*)d)[i] = null ? NULL_I16 : (int16_t)v; break;
    }
}

/* DATE days as temporal_as_ns orders them: saturated past +-106751 days */
#define FASTMAP_DATE_SAT 106751
/* Past +-106751 days temporal_as_ns saturates: all such days compare equal,
 * and strictly beyond every day inside the range — one step out, not on it. */
static inline int64_t fastmap_date_key(int64_t days) {
    return days > FASTMAP_DATE_SAT ? FASTMAP_DATE_SAT + 1
         : days < -FASTMAP_DATE_SAT ? -FASTMAP_DATE_SAT - 1 : days;
}

static ray_t* fastmap_fill(ray_t* r, ray_t* v) {
    if (!r || !v || r->type >= 0 || !ray_is_vec(v) || v->len == 0) return NULL;
    if (RAY_ATOM_IS_NULL(r)) return NULL;
    int8_t vt = v->type, rt = (int8_t)-r->type;
    /* another type is cast to the vector's first (fill_other_type) */
    if (rt != vt) return NULL;
    int64_t n = v->len;
    const void* src = ray_data(v);
    if (fastmap_int_type(vt)) {
        int64_t rv = elem_as_i64(r);
        ray_t* out = ray_vec_new(vt, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;
        void* dst = ray_data(out);
        switch (vt) {
        case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* s = (const int64_t*)src; int64_t* d = (int64_t*)dst;
            for (int64_t i = 0; i < n; i++) d[i] = s[i] == NULL_I64 ? rv : s[i];
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* s = (const int32_t*)src; int32_t* d = (int32_t*)dst;
            int32_t r32 = (int32_t)rv;
            for (int64_t i = 0; i < n; i++) d[i] = s[i] == NULL_I32 ? r32 : s[i];
            break;
        }
        default: {
            const int16_t* s = (const int16_t*)src; int16_t* d = (int16_t*)dst;
            int16_t r16 = (int16_t)rv;
            for (int64_t i = 0; i < n; i++) d[i] = s[i] == NULL_I16 ? r16 : s[i];
            break;
        }
        }
        return out;
    }
    if (vt == RAY_F64) {
        const double* s = (const double*)src;
        double rv = r->f64;
        ray_t* out = ray_vec_new(RAY_F64, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;
        double* d = (double*)ray_data(out);
        for (int64_t i = 0; i < n; i++) d[i] = s[i] != s[i] ? rv : s[i];
        return out;
    }
    return NULL;
}

/* one operand of min/max: a vector's cells or an atom broadcast */
typedef struct {
    const void* data;      /* vector cells, or NULL for an atom */
    int8_t      type;      /* cell / atom type (positive) */
    int64_t     ai;        /* the atom as int64 (int family) */
    double      af;        /* the atom as double (F64 result) */
    bool        anull;
} fastmap_side_t;

static bool fastmap_side(ray_t* x, fastmap_side_t* s) {
    if (!x) return false;
    if (x->type < 0) {
        s->type = (int8_t)-x->type;
        s->data = NULL;
        s->anull = RAY_ATOM_IS_NULL(x);
        if (fastmap_int_type(s->type)) { s->ai = elem_as_i64(x); s->af = (double)s->ai; }
        else if (s->type == RAY_F64) { s->af = x->f64; s->ai = 0; }
        else return false;
        return true;
    }
    if (!ray_is_vec(x)) return false;
    s->type = x->type;
    if (!fastmap_int_type(s->type) && s->type != RAY_F64) return false;
    s->data = ray_data(x);
    s->ai = 0; s->af = 0; s->anull = false;
    return true;
}

static inline int64_t fastmap_side_i64(const fastmap_side_t* s, int64_t i, bool* null) {
    if (!s->data) { *null = s->anull; return s->ai; }
    return fastmap_read_i64(s->data, s->type, i, null);
}

static inline double fastmap_side_f64(const fastmap_side_t* s, int64_t i, bool* null) {
    if (!s->data) { *null = s->anull; return s->af; }
    if (s->type == RAY_F64) { double v = ((const double*)s->data)[i]; *null = v != v; return v; }
    int64_t v = fastmap_read_i64(s->data, s->type, i, null);
    return (double)v;
}

static ray_t* fastmap_minmax(bool is_min, ray_t* a, ray_t* b) {
    fastmap_side_t sa, sb;
    if (!fastmap_side(a, &sa) || !fastmap_side(b, &sb)) return NULL;
    if (!sa.data && !sb.data) return NULL;
    if (sa.data && sb.data && a->len != b->len) return NULL;
    int64_t n = sa.data ? a->len : b->len;
    if (n == 0) return NULL;
    bool any_null = false;
    if (fastmap_int_type(sa.type) && fastmap_int_type(sb.type)) {
        int8_t ot = sa.type;
        if (sa.type != sb.type) {
            ot = ray_num_type(sa.type, sb.type);   /* 0 for a temporal against another type */
            if (!ot) return NULL;
        }
        bool date = ot == RAY_DATE;
        ray_t* out = ray_vec_new(ot, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;
        void* dst = ray_data(out);
        for (int64_t i = 0; i < n; i++) {
            bool na, nb;
            int64_t x = fastmap_side_i64(&sa, i, &na);
            int64_t y = fastmap_side_i64(&sb, i, &nb);
            /* null orders lowest; a saturated date orders like its bound */
            int64_t key_x = na ? INT64_MIN : date ? fastmap_date_key(x) : x;
            int64_t key_y = nb ? INT64_MIN : date ? fastmap_date_key(y) : y;
            bool pick_a = is_min ? key_x <= key_y : key_x >= key_y;
            bool nv = pick_a ? na : nb;
            fastmap_write_i64(dst, ot, i, pick_a ? x : y, nv);
            any_null |= nv;
        }
        if (any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
        return out;
    }
    /* F64 against F64 or an integer (I64 I32 I16): an F64 result */
    bool a_ok = sa.type == RAY_F64 || sa.type == RAY_I64 || sa.type == RAY_I32 || sa.type == RAY_I16;
    bool b_ok = sb.type == RAY_F64 || sb.type == RAY_I64 || sb.type == RAY_I32 || sb.type == RAY_I16;
    if (!a_ok || !b_ok) return NULL;
    ray_t* out = ray_vec_new(RAY_F64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    double* d = (double*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        bool na, nb;
        double x = fastmap_side_f64(&sa, i, &na);
        double y = fastmap_side_f64(&sb, i, &nb);
        bool pick_a, nv;
        if (is_min) { pick_a = na || (!nb && x <= y); nv = na || nb; }
        else        { pick_a = !na && (nb || x >= y); nv = na && nb; }
        d[i] = nv ? NULL_F64 : pick_a ? x : y;
        any_null |= nv;
    }
    if (any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* Map a binary function element-wise over collections.
 * Both args can be collections (zip-map) or one scalar (broadcast).
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
static ray_t* atomic_map_binary_parted(ray_binary_fn fn, uint16_t dag_opcode,
                                       ray_t* left, ray_t* right) {
    int left_parted = left && !RAY_IS_ERR(left) && RAY_IS_PARTED(left->type);
    int right_parted = right && !RAY_IS_ERR(right) && RAY_IS_PARTED(right->type);
    int64_t nseg = left_parted ? left->len : right->len;
    if (left_parted && right_parted && right->len < nseg) nseg = right->len;

    ray_t* out = ray_alloc((size_t)nseg * sizeof(ray_t*));
    if (!out) return ray_error("oom", NULL);
    out->type = RAY_LIST;
    out->len = 0;
    out->attrs = 0;
    memset(out->aux, 0, sizeof(out->aux));
    ray_t** dst = (ray_t**)ray_data(out);
    ray_t** lsegs = left_parted ? (ray_t**)ray_data(left) : NULL;
    ray_t** rsegs = right_parted ? (ray_t**)ray_data(right) : NULL;
    int8_t out_base = 0;

    for (int64_t s = 0; s < nseg; s++) {
        ray_t* l = left_parted ? lsegs[s] : left;
        ray_t* r = right_parted ? rsegs[s] : right;
        if (!l || !r) {
            ray_release(out);
            return ray_error("type", NULL);
        }
        ray_t* seg = atomic_map_binary_op(fn, dag_opcode, l, r);
        if (!seg || RAY_IS_ERR(seg)) {
            ray_release(out);
            return seg ? seg : ray_error("domain", NULL);
        }
        if (!ray_is_vec(seg)) {
            int8_t st = seg->type;
            ray_release(seg);
            ray_release(out);
            return ray_error("type", "parted map: segment result must be a vector, got %s", ray_type_name(st));
        }
        if (s == 0) out_base = seg->type;
        if (seg->type != out_base) {
            int8_t st = seg->type;
            ray_release(seg);
            ray_release(out);
            return ray_error("type", "parted map: segment result types must match, got %s and %s", ray_type_name(out_base), ray_type_name(st));
        }
        dst[s] = seg;
        out->len = s + 1;
    }
    out->type = RAY_PARTED_BASE + out_base;
    return out;
}

/* Map a comparison opcode and a three-way compare result to the BOOL the
 * scalar cmp fns produce. */
static inline bool cmp_apply(uint16_t opc, int c) {
    switch (opc) {
    case OP_EQ: return c == 0;
    case OP_NE: return c != 0;
    case OP_LT: return c < 0;
    case OP_LE: return c <= 0;
    case OP_GT: return c > 0;
    default:    return c >= 0;   /* OP_GE */
    }
}

/* Union-rank LUTs for SYM ordering compares: sort the domain vocabularies
 * (one or two) with the canonical string comparator (memcmp over the
 * common prefix, then length — build_enum_rank / char_str_cmp order) and
 * assign EQUAL strings EQUAL ranks, so per-element ordering reduces to an
 * integer rank compare that pool workers can run without touching the
 * domains.  Returns 0 on success; fills rankL (size dl) and, when rdom
 * differs, rankR (size dr). */
typedef struct { const char* p; uint32_t l; int64_t id; int from_r; } symrank_ent_t;

static int sym_union_ranks(struct ray_sym_domain_s* ldom, int64_t dl,
                           struct ray_sym_domain_s* rdom, int64_t dr,
                           int64_t* rankL, int64_t* rankR) {
    int64_t total = dl + ((rdom && rankR) ? dr : 0);
    if (total <= 0) return 0;
    ray_t* ents_hdr = ray_alloc(total * (int64_t)sizeof(symrank_ent_t));
    ray_t* tmp_hdr  = ray_alloc(total * (int64_t)sizeof(symrank_ent_t));
    if (!ents_hdr || !tmp_hdr) {
        if (ents_hdr) ray_release(ents_hdr);
        if (tmp_hdr)  ray_release(tmp_hdr);
        return -1;
    }
    symrank_ent_t* e   = (symrank_ent_t*)ray_data(ents_hdr);
    symrank_ent_t* tmp = (symrank_ent_t*)ray_data(tmp_hdr);
    int64_t n = 0;
    for (int64_t i = 0; i < dl; i++) {
        ray_t* str = ray_sym_domain_str(ldom, i);
        e[n].p = str ? ray_str_ptr(str) : NULL;
        e[n].l = str ? (uint32_t)ray_str_len(str) : 0;
        e[n].id = i; e[n].from_r = 0; n++;
    }
    if (rdom && rankR) {
        for (int64_t i = 0; i < dr; i++) {
            ray_t* str = ray_sym_domain_str(rdom, i);
            e[n].p = str ? ray_str_ptr(str) : NULL;
            e[n].l = str ? (uint32_t)ray_str_len(str) : 0;
            e[n].id = i; e[n].from_r = 1; n++;
        }
    }
    /* Bottom-up merge sort by (string) — no comparator context needed. */
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * width) {
            int64_t mid = lo + width;    if (mid > n) mid = n;
            int64_t hi  = lo + 2 * width; if (hi > n) hi = n;
            int64_t a = lo, b = mid, t = lo;
            while (a < mid && b < hi) {
                uint32_t ml = e[a].l < e[b].l ? e[a].l : e[b].l;
                int c = ml ? memcmp(e[a].p, e[b].p, ml) : 0;
                if (c == 0) c = (e[a].l > e[b].l) - (e[a].l < e[b].l);
                tmp[t++] = (c <= 0) ? e[a++] : e[b++];
            }
            while (a < mid) tmp[t++] = e[a++];
            while (b < hi)  tmp[t++] = e[b++];
            for (int64_t cpy = lo; cpy < hi; cpy++) e[cpy] = tmp[cpy];
        }
    }
    /* Equal strings share a rank so cross-domain equality holds. */
    int64_t rank = 0;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) {
            uint32_t ml = e[i].l < e[i-1].l ? e[i].l : e[i-1].l;
            int c = ml ? memcmp(e[i].p, e[i-1].p, ml) : 0;
            if (c == 0) c = (e[i].l > e[i-1].l) - (e[i].l < e[i-1].l);
            if (c != 0) rank++;
        }
        if (e[i].from_r) rankR[e[i].id] = rank;
        else             rankL[e[i].id] = rank;
    }
    ray_release(ents_hdr);
    ray_release(tmp_hdr);
    return 0;
}

/* Pool worker: SYM ordering compare via rank LUTs (vec-vec) or a verdict
 * LUT (vec-vs-atom: verdict[id] = three-way cmp of the id's string against
 * the atom, orientation already folded in).  Pure integer reads — no
 * domain access from workers. */
typedef struct {
    const void* ld; uint8_t lattrs;
    const void* rd; uint8_t rattrs;      /* NULL for vec-vs-atom */
    const int64_t* rankL;
    const int64_t* rankR;
    const int8_t*  verdict;              /* vec-vs-atom path */
    bool*    obuf;
    uint16_t opc;
} symord_ctx_t;

static void symord_cmp_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    symord_ctx_t* c = (symord_ctx_t*)raw;
    if (!c->rd) {
        for (int64_t i = start; i < end; i++) {
            int64_t lid = ray_read_sym(c->ld, i, RAY_SYM, c->lattrs);
            c->obuf[i] = cmp_apply(c->opc, (int)c->verdict[lid]);
        }
        return;
    }
    for (int64_t i = start; i < end; i++) {
        int64_t la = c->rankL[ray_read_sym(c->ld, i, RAY_SYM, c->lattrs)];
        int64_t rb = c->rankR[ray_read_sym(c->rd, i, RAY_SYM, c->rattrs)];
        c->obuf[i] = cmp_apply(c->opc, (la > rb) - (la < rb));
    }
}

/* Pool worker: STR compare — vec-vec or vec-vs-atom, canonical bytes
 * comparator, no boxing.  ray_str_vec_get is a read-only view. */
typedef struct {
    ray_t*      lv;                       /* NULL when left is the atom */
    ray_t*      rv;                       /* NULL when right is the atom */
    const char* ap; size_t al;            /* the atom side, when present */
    bool*       obuf;
    uint16_t    opc;
} strcmp_ctx_t;

static void strvec_cmp_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    strcmp_ctx_t* c = (strcmp_ctx_t*)raw;
    for (int64_t i = start; i < end; i++) {
        const char *lp, *rp; size_t ll, rl;
        if (c->lv) { lp = ray_str_vec_get(c->lv, i, &ll); }
        else       { lp = c->ap; ll = c->al; }
        if (c->rv) { rp = ray_str_vec_get(c->rv, i, &rl); }
        else       { rp = c->ap; rl = c->al; }
        size_t ml = ll < rl ? ll : rl;
        int cv = ml ? memcmp(lp, rp, ml) : 0;
        if (cv == 0) cv = (ll > rl) - (ll < rl);
        c->obuf[i] = cmp_apply(c->opc, cv);
    }
}

/* Pool worker: GUID compare — vec-vec or vec-vs-atom over 16-byte cells.
 * The atom side has stride 0, so one loop serves every shape.  Equality
 * compares the two 8-byte halves; ordering is the memcmp the scalar cmp
 * fns apply to two guid atoms. */
typedef struct {
    const uint8_t* lp; int64_t lstride;
    const uint8_t* rp; int64_t rstride;
    bool*    obuf;
    uint16_t opc;
} guidcmp_ctx_t;

static void guidvec_cmp_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    guidcmp_ctx_t* c = (guidcmp_ctx_t*)raw;
    const uint8_t* lp = c->lp;
    const uint8_t* rp = c->rp;
    int64_t ls = c->lstride, rs = c->rstride;
    if (c->opc == OP_EQ || c->opc == OP_NE) {
        bool inv = c->opc == OP_NE;
        for (int64_t i = start; i < end; i++) {
            uint64_t l0, l1, r0, r1;
            memcpy(&l0, lp + i * ls, 8); memcpy(&l1, lp + i * ls + 8, 8);
            memcpy(&r0, rp + i * rs, 8); memcpy(&r1, rp + i * rs + 8, 8);
            c->obuf[i] = ((l0 == r0) & (l1 == r1)) ^ inv;
        }
        return;
    }
    for (int64_t i = start; i < end; i++)
        c->obuf[i] = cmp_apply(c->opc, memcmp(lp + i * ls, rp + i * rs, 16));
}

/* Pool worker for the R7/R8 SYM compare fast paths.  rd == NULL means the
 * right side is a scalar target id (R7); otherwise element-wise vec-vec
 * (R8), optionally translating left ids through a vocabulary LUT when the
 * domains differ.  Canonical SYM null is raw id 0, so the same value compare
 * handles it without a separate bitmap. */
typedef struct {
    const void* ld;
    const void* rd;         /* NULL for the vec-vs-atom shape */
    uint8_t     lattrs;
    uint8_t     rattrs;
    int64_t     target;     /* vec-vs-atom: right id in the LEFT domain */
    const int64_t* lut;     /* vec-vec cross-domain: left pos -> right pos */
    int64_t     lcount;
    bool*       obuf;
    bool        invert;
} symcmp_ctx_t;

static void symcmp_eq_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    symcmp_ctx_t* c = (symcmp_ctx_t*)raw;
    const void* ld = c->ld;
    uint8_t la = c->lattrs;
    bool inv = c->invert;
    bool* obuf = c->obuf;
    if (!c->rd) {
        /* vec vs scalar: width-specialized loops (R7). */
        uint8_t w = (uint8_t)(la & RAY_SYM_W_MASK);
        int64_t target = c->target;
        if (w == RAY_SYM_W8) {
            const uint8_t* d = (const uint8_t*)ld;
            uint8_t t8 = (uint8_t)target;
            for (int64_t i = start; i < end; i++) obuf[i] = (d[i] == t8) ^ inv;
        } else if (w == RAY_SYM_W16) {
            const uint16_t* d = (const uint16_t*)ld;
            uint16_t t16 = (uint16_t)target;
            for (int64_t i = start; i < end; i++) obuf[i] = (d[i] == t16) ^ inv;
        } else if (w == RAY_SYM_W32) {
            const uint32_t* d = (const uint32_t*)ld;
            uint32_t t32 = (uint32_t)target;
            for (int64_t i = start; i < end; i++) obuf[i] = (d[i] == t32) ^ inv;
        } else {
            const int64_t* d = (const int64_t*)ld;
            for (int64_t i = start; i < end; i++) obuf[i] = (d[i] == c->target) ^ inv;
        }
        return;
    }
    const void* rd = c->rd;
    uint8_t ra = c->rattrs;
    if (!c->lut) {
        for (int64_t i = start; i < end; i++)
            obuf[i] = (ray_read_sym(ld, i, RAY_SYM, la) ==
                       ray_read_sym(rd, i, RAY_SYM, ra)) ^ inv;
    } else {
        const int64_t* lut = c->lut;
        int64_t lcount = c->lcount;
        for (int64_t i = start; i < end; i++) {
            int64_t lid = ray_read_sym(ld, i, RAY_SYM, la);
            int64_t tl = (lid >= 0 && lid < lcount) ? lut[lid] : -1;
            int64_t rid = ray_read_sym(rd, i, RAY_SYM, ra);
            obuf[i] = (tl >= 0 && tl == rid) ^ inv;
        }
    }
}

/* Run the SYM compare worker over [0,n): pool-parallel above the shared
 * morsel threshold, serial otherwise. */
static void symcmp_eq_run(symcmp_ctx_t* ctx, int64_t n) {
    ray_pool_t* pool = ray_pool_get();
    if (pool && n >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, symcmp_eq_fn, ctx, n);
    else
        symcmp_eq_fn(ctx, 0, 0, n);
}

/* B8 comparison (== <> < <= > >=) of two B8 vectors, or a B8 vector and a B8
 * atom.  A byte is truthy when non-zero, as in and / or / not.  The typed DAG
 * path does not take BOOL, so without this the comparison fell to the boxed
 * per-element loop (a BOOL atom allocated per row).  `b` is NULL when the
 * right operand is the atom `bv`; the caller mirrors the operator when the
 * atom is on the left. */
typedef struct {
    const uint8_t* a;
    const uint8_t* b;
    uint8_t*       out;
    uint8_t        bv;
    uint16_t       op;
} boolcmp_ctx_t;

static void boolcmp_fn(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    const boolcmp_ctx_t* c = (const boolcmp_ctx_t*)vctx;
    const uint8_t* a = c->a;
    const uint8_t* b = c->b;
    uint8_t* o = c->out;
    uint8_t bv = c->bv;
#define BOOLCMP_CASE(OPC, CMP)                                                     \
    case OPC:                                                                      \
        if (b) for (int64_t i = lo; i < hi; i++)                                   \
                   o[i] = (uint8_t)((a[i] != 0) CMP (b[i] != 0));                  \
        else   for (int64_t i = lo; i < hi; i++)                                   \
                   o[i] = (uint8_t)((a[i] != 0) CMP bv);                           \
        break;
    switch (c->op) {
        BOOLCMP_CASE(OP_EQ, ==)
        BOOLCMP_CASE(OP_NE, !=)
        BOOLCMP_CASE(OP_LT, <)
        BOOLCMP_CASE(OP_LE, <=)
        BOOLCMP_CASE(OP_GT, >)
        BOOLCMP_CASE(OP_GE, >=)
    default: break;
    }
#undef BOOLCMP_CASE
}

/* The operator that gives the same answer with its operands swapped. */
static uint16_t boolcmp_mirror(uint16_t op) {
    switch (op) {
    case OP_LT: return OP_GT;
    case OP_LE: return OP_GE;
    case OP_GT: return OP_LT;
    case OP_GE: return OP_LE;
    default:    return op;
    }
}

/* NULL when the operands are not a B8 vector against a B8 vector or a non-null
 * B8 atom; otherwise the BOOL result vector (possibly an error). */
static ray_t* bool_cmp_fast(uint16_t op, ray_t* left, ray_t* right) {
    bool lv = ray_is_vec(left)  && left->type  == RAY_BOOL;
    bool rv = ray_is_vec(right) && right->type == RAY_BOOL;
    bool la = left->type  == -RAY_BOOL && !RAY_ATOM_IS_NULL(left);
    bool ra = right->type == -RAY_BOOL && !RAY_ATOM_IS_NULL(right);
    if (!((lv && rv) || (lv && ra) || (la && rv))) return NULL;
    ray_t* vec  = lv ? left : right;
    int64_t n = vec->len;
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    boolcmp_ctx_t c = { .out = (uint8_t*)ray_data(out), .b = NULL, .bv = 0, .op = op };
    if (lv && rv) {
        c.a = (const uint8_t*)ray_data(left);
        c.b = (const uint8_t*)ray_data(right);
    } else if (lv) {
        c.a = (const uint8_t*)ray_data(left);
        c.bv = right->b8 != 0;
    } else {
        c.a = (const uint8_t*)ray_data(right);
        c.bv = left->b8 != 0;
        c.op = boolcmp_mirror(op);
    }
    ray_pool_t* pool = ray_pool_get();
    if (pool && n >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, boolcmp_fn, &c, n);
    else
        boolcmp_fn(&c, 0, 0, n);
    out->len = n;
    return out;
}

/* ---- Temporal +/- integer (#1302) ----
 *
 * DATE / TIME / TIMESTAMP plus or minus a B8 / U8 / I16 / I32 / I64 operand,
 * either order for +, and int - TIME (the builtin's one int-minus-temporal
 * pair).  ray_add_fn / ray_sub_fn answer each cell as the temporal count
 * plus the integer, wrapped to the temporal lane (ray_date / ray_time keep
 * the low 32 bits, TIMESTAMP wraps in 64), and a null on either side is the
 * temporal null; a result that wraps onto the sentinel reads as null too.
 * The DAG below cannot carry that (promote() types DATE + I64 as I64 and
 * the narrow lane is lost), so the pair ran the boxed per-element loop at
 * ~27 ns a cell.  Here it is one typed pass over the raw counts: the same
 * wrap, the same nulls, the temporal type.  (A query lowers the same pairs
 * to the DAG itself: dag_temporal_int_arith, src/ops/query.c.) */
enum { TI_ADD, TI_SUB, TI_RSUB };          /* t + x, t - x, x - t */
enum { TI_VV, TI_VA, TI_AV };              /* t vec x vec, t vec x atom, t atom x vec */

typedef struct {
    const void* t;  const void* x;  void* out;
    int64_t tc, xc;                        /* the atom side's value */
    int8_t  tw, xt;                        /* temporal lane bytes, int type */
    uint8_t op, shape;
    _Atomic(uint8_t) has_null;             /* some cell is the sentinel */
} tempint_ctx_t;

static inline __attribute__((always_inline))
void tempint_span(tempint_ctx_t* c, int64_t lo, int64_t hi,
                  const int tw, const int8_t xt, const int op, const int shape) {
    const int32_t* t32 = (const int32_t*)c->t;
    const int64_t* t64 = (const int64_t*)c->t;
    bool any = false;
    for (int64_t j = lo; j < hi; j++) {
        int64_t tv = shape == TI_AV ? c->tc : tw == 4 ? (int64_t)t32[j] : t64[j];
        int64_t xv;
        bool xn = false;
        if (shape == TI_VA) xv = c->xc;
        else switch (xt) {
        case RAY_BOOL: case RAY_U8: xv = ((const uint8_t*)c->x)[j]; break;
        case RAY_I16: xv = ((const int16_t*)c->x)[j]; xn = xv == NULL_I16; break;
        case RAY_I32: xv = ((const int32_t*)c->x)[j]; xn = xv == NULL_I32; break;
        default:      xv = ((const int64_t*)c->x)[j]; xn = xv == NULL_I64; break;
        }
        if (tw == 4) {
            bool tn = shape != TI_AV && tv == NULL_I32;
            uint32_t a = (uint32_t)tv, b = (uint32_t)xv;
            uint32_t r = op == TI_ADD ? a + b : op == TI_SUB ? a - b : b - a;
            int32_t o = (tn || xn) ? NULL_I32 : (int32_t)r;
            ((int32_t*)c->out)[j] = o;
            any |= o == NULL_I32;
        } else {
            bool tn = shape != TI_AV && tv == NULL_I64;
            uint64_t a = (uint64_t)tv, b = (uint64_t)xv;
            uint64_t r = op == TI_ADD ? a + b : op == TI_SUB ? a - b : b - a;
            int64_t o = (tn || xn) ? NULL_I64 : (int64_t)r;
            ((int64_t*)c->out)[j] = o;
            any |= o == NULL_I64;
        }
    }
    /* a wrap onto the sentinel is a null too, as the boxed store reads it */
    if (any) atomic_store_explicit(&c->has_null, 1, memory_order_relaxed);
}

#define TEMPINT_SHAPES(TW, XT, OP)                                             \
    switch (c->shape) {                                                        \
    case TI_VV: tempint_span(c, lo, hi, TW, XT, OP, TI_VV); break;             \
    case TI_VA: tempint_span(c, lo, hi, TW, XT, OP, TI_VA); break;             \
    default:    tempint_span(c, lo, hi, TW, XT, OP, TI_AV); break;             \
    }
#define TEMPINT_OPS(TW, XT)                                                    \
    switch (c->op) {                                                           \
    case TI_ADD: TEMPINT_SHAPES(TW, XT, TI_ADD) break;                         \
    case TI_SUB: TEMPINT_SHAPES(TW, XT, TI_SUB) break;                         \
    default:     TEMPINT_SHAPES(TW, XT, TI_RSUB) break;                        \
    }
/* an atom offset is read once (xc): one instantiation serves every type */
#define TEMPINT_XTYPES(TW)                                                     \
    switch (c->shape == TI_VA ? RAY_I64 : c->xt) {                             \
    case RAY_BOOL: case RAY_U8: TEMPINT_OPS(TW, RAY_U8) break;                 \
    case RAY_I16: TEMPINT_OPS(TW, RAY_I16) break;                              \
    case RAY_I32: TEMPINT_OPS(TW, RAY_I32) break;                              \
    default:      TEMPINT_OPS(TW, RAY_I64) break;                              \
    }

static void tempint_fn(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    tempint_ctx_t* c = (tempint_ctx_t*)vctx;
    if (c->tw == 4) { TEMPINT_XTYPES(4) }
    else            { TEMPINT_XTYPES(8) }
}
#undef TEMPINT_XTYPES
#undef TEMPINT_OPS
#undef TEMPINT_SHAPES

_Atomic(uint64_t) ray_tempint_routes;

static bool tempint_is_int(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 || t == RAY_I64;
}

/* NULL when the operands are not a temporal-vs-integer +/- pair the typed
 * pass answers (the caller then takes its usual route); otherwise the
 * result vector of type `out_type` (the probe's), or an error.  Plain
 * vectors and atoms only: parted, LIST and the rest keep their paths. */
static ray_t* tempint_arith(uint16_t dag_opcode, ray_t* left, ray_t* right,
                            int8_t out_type, int64_t len) {
    if (dag_opcode != OP_ADD && dag_opcode != OP_SUB) return NULL;
    if (out_type != RAY_DATE && out_type != RAY_TIME && out_type != RAY_TIMESTAMP)
        return NULL;
    bool lvec = ray_is_vec(left), rvec = ray_is_vec(right);
    if (!lvec && !ray_is_atom(left)) return NULL;
    if (!rvec && !ray_is_atom(right)) return NULL;
    if (!lvec && !rvec) return NULL;
    int8_t lt = lvec ? left->type : (int8_t)-left->type;
    int8_t rt = rvec ? right->type : (int8_t)-right->type;
    ray_t *tv, *xv;
    uint8_t op;
    if (lt == out_type && tempint_is_int(rt)) {
        tv = left; xv = right; op = dag_opcode == OP_ADD ? TI_ADD : TI_SUB;
    } else if (rt == out_type && tempint_is_int(lt) &&
               (dag_opcode == OP_ADD || out_type == RAY_TIME)) {
        tv = right; xv = left; op = dag_opcode == OP_ADD ? TI_ADD : TI_RSUB;
    } else {
        return NULL;
    }
    bool tvec = tv == left ? lvec : rvec;
    bool xvec = xv == left ? lvec : rvec;
    ray_t* out = ray_vec_new(out_type, len);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = len;
    atomic_fetch_add_explicit(&ray_tempint_routes, 1, memory_order_relaxed);
    int tw = out_type == RAY_TIMESTAMP ? 8 : 4;
    /* A null atom is a null for every cell, as the builtin answers it. */
    if ((!tvec && RAY_ATOM_IS_NULL(tv)) || (!xvec && RAY_ATOM_IS_NULL(xv))) {
        if (tw == 4) { int32_t* d = (int32_t*)ray_data(out); for (int64_t j = 0; j < len; j++) d[j] = NULL_I32; }
        else         { int64_t* d = (int64_t*)ray_data(out); for (int64_t j = 0; j < len; j++) d[j] = NULL_I64; }
        out->attrs |= RAY_ATTR_HAS_NULLS;
        return out;
    }
    tempint_ctx_t c = {
        .t = tvec ? ray_data(tv) : NULL, .x = xvec ? ray_data(xv) : NULL,
        .out = ray_data(out),
        .tc = tvec ? 0 : tv->i64, .xc = xvec ? 0 : as_i64(xv),
        .tw = (int8_t)tw, .xt = xvec ? xv->type : (int8_t)-xv->type,
        .op = op, .shape = tvec && xvec ? TI_VV : tvec ? TI_VA : TI_AV,
    };
    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD) {
        ray_pool_dispatch(pool, tempint_fn, &c, len);
        if (ray_interrupted() ||
            atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
            ray_release(out);
            return ray_error("cancel", "interrupted");
        }
    } else {
        tempint_fn(&c, 0, 0, len);
    }
    if (atomic_load_explicit(&c.has_null, memory_order_relaxed))
        out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* An element-wise op over a value that holds one list at several positions
 * maps that list once (#1627): the result for a shared list (and the
 * other operand, and the op) is kept for the rest of the outermost map and
 * reused, so the result shares as the operand does and the work is linear
 * in the distinct lists, not in the paths through them.  The memo holds a
 * reference to each kept result and drops it when the outermost map ends. */
static _Thread_local ray_walk_memo_t g_map_memo;
static inline bool map_memo_wanted(const ray_t* x) {
    return x && RAY_UNLIKELY(ray_wm_reused(x)) && !RAY_IS_ERR(x) &&
           (x->type == RAY_LIST || x->type == RAY_DICT || x->type == RAY_TABLE);
}
typedef struct { ray_binary_fn fn2; ray_unary_fn fn1; uint16_t op; ray_t* l; ray_t* r; } map_ctx_t;

static inline __attribute__((always_inline)) ray_t* atomic_map_binary_op_c(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right);
static inline __attribute__((always_inline)) ray_t* atomic_map_unary_c(ray_unary_fn fn, ray_t* arg);
static ray_t* map_binary_run(void* p) {
    map_ctx_t* c = p;
    return atomic_map_binary_op_c(c->fn2, c->op, c->l, c->r);
}
static ray_t* map_unary_run(void* p) {
    map_ctx_t* c = p;
    return atomic_map_unary_c(c->fn1, c->l);
}
/* The shortest typed vector a builtin's vector kernel runs on from the
 * per-element maps below (#1756).  The kernel's graph costs about 4 800
 * instructions to build and run whatever the length, the boxed map about
 * 720 per element, so they cross at six or seven elements. */
#define MAP_KERNEL_MIN_LEN 8

__attribute__((noinline))
static ray_t* atomic_map_binary_op_memo(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
    /* user-space pointers are below 2^48: the opcode fits above fn */
    uint64_t tag = (uint64_t)(uintptr_t)fn ^ ((uint64_t)dag_opcode << 48);
    map_ctx_t c = { fn, NULL, dag_opcode, left, right };
    return ray_wm_result(&g_map_memo, left, right, tag, map_binary_run, &c);
}
/* Let go of a vector a fast path allocated and will not answer with: a
 * failed ray_vec_new is an owned error, freed (ray_release leaves errors
 * alone), so falling back to the slow path keeps nothing. */
static inline void map_drop_vec(ray_t* v) {
    if (!v) return;
    if (RAY_IS_ERR(v)) ray_error_free(v); else ray_release(v);
}

/* A pair of cells the binary map read with collection_elem, one of them an
 * error: the error the map answers, with what it read released.  An error
 * read from a typed vector is the map's own (the oom of the cell's atom, or
 * the type error of a cell it cannot read) and is the answer; a LIST holds
 * its cells, so an error there is the list's, not the map's, and the
 * answer is a type error as before. */
static ray_t* map_cell_error(ray_t* left, int left_coll, ray_t* a, int la,
                             ray_t* right, int right_coll, ray_t* b, int ra) {
    bool own_a = left_coll && left->type != RAY_LIST && RAY_IS_ERR(a);
    bool own_b = right_coll && right->type != RAY_LIST && RAY_IS_ERR(b);
    if (la && !RAY_IS_ERR(a)) ray_release(a);
    if (ra && !RAY_IS_ERR(b)) ray_release(b);
    if (own_a && own_b) ray_error_free(b);
    if (own_a) return a;
    if (own_b) return b;
    return ray_error("type", NULL);
}

/* Whether a cell's result is a number a typed numeric or temporal vector
 * holds as itself: store_typed_elem reads any other atom (a GUID, symbol or
 * string) through its integer field. */
static inline bool map_cell_is_number(ray_t* x) {
    switch (x->type) {
    case -RAY_BOOL: case -RAY_U8: case -RAY_I16: case -RAY_I32: case -RAY_I64:
    case -RAY_F32: case -RAY_F64: case -RAY_DATE: case -RAY_TIME:
    case -RAY_TIMESTAMP: case -RAY_TIMESPAN:
        return true;
    default:
        return false;
    }
}

/* The numeric and temporal types: a value of one casts to any other. */
static inline bool fill_num_type(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F32: case RAY_F64: case RAY_DATE: case RAY_TIME:
    case RAY_TIMESTAMP: case RAY_TIMESPAN:
        return true;
    default:
        return false;
    }
}

/* fill_cast_replacement's answer for a cast `as` refuses (a TIMESPAN and a
 * DATE, TIME or TIMESTAMP): not an object, never released. */
static ray_t g_fill_cast_refused;

/* fill's replacement cast to the type of the vector it fills, when both are
 * numeric or temporal and of different types: (as 'T replacement).  NULL
 * when there is nothing to cast, &g_fill_cast_refused when `as` refuses the
 * cast; an allocation failure is its error. */
static ray_t* fill_cast_replacement(ray_t* r, ray_t* v) {
    if (!r || !v || RAY_IS_ERR(r) || RAY_IS_ERR(v) || !ray_is_vec(v)) return NULL;
    int8_t vt = v->type;
    int8_t rt = ray_is_vec(r) ? r->type : r->type < 0 ? (int8_t)-r->type : 0;
    if (rt == vt || !fill_num_type(vt) || !fill_num_type(rt)) return NULL;
    const char* tn = ray_type_name(vt);
    ray_t* tsym = ray_sym(ray_sym_intern(tn, strlen(tn)));
    if (!tsym || RAY_IS_ERR(tsym)) return tsym ? tsym : ray_error("oom", NULL);
    ray_t* c = ray_cast_fn(tsym, r);
    ray_release(tsym);
    if (!c) return NULL;
    if (RAY_IS_ERR(c)) {
        if (strcmp(ray_err_code(c), "oom") == 0) return c;
        ray_error_free(c);
        ray_error_clear();
        return &g_fill_cast_refused;
    }
    int8_t ct = ray_is_vec(c) ? c->type : (int8_t)-c->type;
    if (ct != vt) { ray_release(c); return &g_fill_cast_refused; }
    return c;
}

/* whether fill's replacement and vector are of different types: the cheap
 * test before fill_other_type */
static inline bool fill_types_differ(ray_t* r, ray_t* v) {
    /* an atom of the vector's type has the negated tag, a vector its tag */
    return r->type != -v->type && r->type != v->type;
}

/* fill over a vector with a replacement of another type (the rule above
 * atomic_map_binary_op_c's call); NULL when the general path answers. */
static __attribute__((noinline)) ray_t* fill_other_type(uint16_t dag_opcode, ray_t* left, ray_t* right) {
    ray_binary_fn fn = ray_fill_fn;

    ray_t* cast = fill_cast_replacement(left, right);
    if (cast == &g_fill_cast_refused) {
        /* the LIST of each cell, null or not first: the vector's cells
         * mapped as a list's.  With no null nothing is replaced, and
         * the vector answers as itself below. */
        bool any_null = false;
        for (int64_t i = 0; i < right->len && !any_null; i++)
            any_null = ray_vec_is_null(right, i);
        if (any_null) {
            ray_t* cells = ray_list_new(right->len);
            for (int64_t i = 0; i < right->len && cells && !RAY_IS_ERR(cells); i++) {
                int a = 0;
                ray_t* e = collection_elem(right, i, &a);
                if (RAY_IS_ERR(e)) { ray_release(cells); return e; }
                cells = ray_list_append(cells, e);
                if (a) ray_release(e);
            }
            if (!cells || RAY_IS_ERR(cells)) return cells ? cells : ray_error("oom", NULL);
            ray_t* r = atomic_map_binary_op(fn, dag_opcode, left, cells);
            ray_release(cells);
            return r;
        }
        return NULL;
    }
    if (cast) {
        if (RAY_IS_ERR(cast)) return cast;
        ray_t* r = atomic_map_binary_op(fn, dag_opcode, cast, right);
        ray_release(cast);
        return r;
    }
    return NULL;
}


ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
    if (RAY_UNLIKELY(map_memo_wanted(left) || map_memo_wanted(right)))
        return atomic_map_binary_op_memo(fn, dag_opcode, left, right);
    return atomic_map_binary_op_c(fn, dag_opcode, left, right);
}
static inline __attribute__((always_inline)) ray_t* atomic_map_binary_op_c(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
    if (fn == ray_eq_fn && left && right && !RAY_IS_ERR(left) && !RAY_IS_ERR(right) &&
        (left->type == RAY_DICT || right->type == RAY_DICT ||
                            left->type == RAY_TABLE || right->type == RAY_TABLE))
        return fn(left, right);
    if ((left && !RAY_IS_ERR(left) && RAY_IS_PARTED(left->type)) ||
        (right && !RAY_IS_ERR(right) && RAY_IS_PARTED(right->type)))
        return atomic_map_binary_parted(fn, dag_opcode, left, right);

    /* fill of a numeric or temporal vector with a replacement of another
     * such type: the replacement is cast to the vector's type, as (as 'T r)
     * would, so the result is the vector's type whichever cell comes first
     * (#1778).  A replacement the cast refuses keeps the LIST of each cell
     * (#1777). */
    /* fill / dyadic min / max over a fixed-width vector: one typed pass
     * (fastmap_fill, fastmap_minmax); NULL keeps the shape on the mapper */
    if (fn == ray_fill_fn || fn == ray_min2_fn || fn == ray_max2_fn) {
        ray_t* r = fn == ray_fill_fn ? fastmap_fill(left, right)
                                     : fastmap_minmax(fn == ray_min2_fn, left, right);
        if (r) return r;
        if (fn == ray_fill_fn && fill_types_differ(left, right)) {
            r = fill_other_type(dag_opcode, left, right);
            if (r) return r;
        }
    }

    bool duration_binary = fn == ray_add_fn || fn == ray_sub_fn || fn == ray_mul_fn ||
        fn == ray_div_fn || fn == ray_idiv_fn || fn == ray_mod_fn || fn == ray_xbar_fn ||
        fn == ray_eq_fn || fn == ray_neq_fn || fn == ray_lt_fn || fn == ray_lte_fn ||
        fn == ray_gt_fn || fn == ray_gte_fn;
    if (duration_binary && left->type != RAY_LIST && right->type != RAY_LIST &&
        (ray_has_timespan(left) || ray_has_timespan(right) ||
         (fn == ray_sub_fn && ray_point_diff(left, right))))
        return fn(left, right);

    int left_coll = is_collection(left);
    int right_coll = is_collection(right);

    if (!left_coll && !right_coll) return fn(left, right);
    /* A list's cells are mapped through here again (atomic_map_unary). */
    if ((left->type == RAY_LIST || right->type == RAY_LIST) && RAY_UNLIKELY(ray_stack_short()))
        return ray_error("limit", "value nested too deep for this thread's stack");

    int64_t len;
    if (left_coll && right_coll) {
        /* Both operands are vectors: lengths MUST match.  Silently
         * truncating to the shorter (the prior behaviour) hid a real
         * error and diverged from the compiled/VM path, which already
         * rejects the mismatch (src/ops/expr.c).  A 1-element vector
         * does NOT broadcast (only a true atom does — see the
         * !left_coll/!right_coll branch). */
        if (ray_len(left) != ray_len(right)) {
            const char* opn = dag_opcode ? ray_opcode_name(dag_opcode) : NULL;
            return ray_error("length", "%s: operand lengths must match, got %lld and %lld",
                             opn ? opn : "binary op",
                             (long long)ray_len(left), (long long)ray_len(right));
        }
        len = ray_len(left);
    } else {
        len = left_coll ? ray_len(left) : ray_len(right);
    }

    if (len == 0) {
        /* Empty collection — no first element to probe, so fabricate a
         * zero-valued atom of each operand's element type and run `fn`
         * on it to learn the output type.  Without this the result was
         * hardcoded to I64 and lost the semantics of type-preserving
         * ops (e.g. `(xbar empty_TIME_col 10000)` returned an I64 empty
         * vector instead of a TIME one). */
        ray_t* la = left_coll  ? zero_atom_for_elem_type(left)  : left;
        ray_t* ra = right_coll ? zero_atom_for_elem_type(right) : right;
        ray_t* probe = (la && ra && !RAY_IS_ERR(la) && !RAY_IS_ERR(ra))
                       ? fn(la, ra) : NULL;
        /* The probe and the fabricated operands are ours.  A failed
         * probe — `(/ empty_DATE 2)`: ray_div_fn rejects the fabricated
         * DATE — or a failed fabrication is freed and its message
         * cleared; the empty result keeps its I64 fallback type.  A
         * caller's own operand (not fabricated) is not touched. */
        bool failed = (probe && RAY_IS_ERR(probe)) ||
                      (left_coll  && la && RAY_IS_ERR(la)) ||
                      (right_coll && ra && RAY_IS_ERR(ra));
        if (left_coll  && la) { if (RAY_IS_ERR(la)) ray_error_free(la); else ray_release(la); }
        if (right_coll && ra) { if (RAY_IS_ERR(ra)) ray_error_free(ra); else ray_release(ra); }
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        if (probe) { if (RAY_IS_ERR(probe)) ray_error_free(probe); else ray_release(probe); }
        if (failed) ray_error_clear();
        return ray_vec_new(RAY_I64, 0);
    }

    /* Probe first element to determine output type */
    int la0 = 0, ra0 = 0;
    ray_t* a0 = left_coll  ? collection_elem(left, 0, &la0)  : left;
    ray_t* b0 = right_coll ? collection_elem(right, 0, &ra0) : right;
    ray_t* e0;
    if (RAY_IS_ERR(a0) || RAY_IS_ERR(b0))
        return map_cell_error(left, left_coll, a0, la0, right, right_coll, b0, ra0);
    if (is_collection(a0) || is_collection(b0)) {
        e0 = atomic_map_binary(fn, a0, b0);
    } else {
        e0 = fn(a0, b0);
    }
    if (la0) ray_release(a0);
    if (ra0) ray_release(b0);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = -(e0->type);  /* atom type (-RAY_I64) → vector type (RAY_I64) */

    /* If either input is a boxed list (mixed types), always use boxed list output
     * to preserve type heterogeneity */
    int force_boxed = (left_coll && left->type == RAY_LIST) ||
                      (right_coll && right->type == RAY_LIST);

    /* The probe's type is the result type: the builtins take it from the
     * promotion table (ray_num_type, core/types.h), the same for either
     * operand order and for atoms as for vectors (#972), and the DAG below
     * plans the same one.  The one adjustment left is for a left vector
     * outside the numeric table (temporal, TIMESPAN, ...): an I32 probe
     * answers I64 there, as it always has (DATE - DATE days, (min span 5i)). */
    if (out_type == RAY_I32 && left_coll && ray_is_vec(left) &&
        !ray_num_type(left->type, left->type) && !RAY_ATOM_IS_NULL(e0))
        out_type = RAY_I64;

    /* `div` is registered without its opcode (the query compiler maps the
     * name itself); here, past the checks whose messages name the opcode,
     * it is what lets a typed vector reach the floor-division kernel
     * instead of the boxed per-element loop (#1756), from the length where
     * the kernel is the cheaper (MAP_KERNEL_MIN_LEN).  A B8 operand has no
     * kernel there (the DAG path takes no BOOL) and would only pay for
     * the attempt, so it keeps the boxed loop as it was */
    if (RAY_UNLIKELY(dag_opcode == 0) && fn == ray_idiv_fn && len >= MAP_KERNEL_MIN_LEN &&
        left->type != RAY_BOOL && left->type != -RAY_BOOL &&
        right->type != RAY_BOOL && right->type != -RAY_BOOL) dag_opcode = OP_IDIV;

    if (!force_boxed) {
        ray_t* r = tempint_arith(dag_opcode, left, right, out_type, len);
        if (r) { ray_release(e0); return r; }
    }

    /* DAG executor — for F64 and comparisons */
    if (!force_boxed && dag_opcode > 0) {
        int is_idiv = (dag_opcode == OP_MOD);
        int is_cmp  = (dag_opcode >= OP_EQ && dag_opcode <= OP_GE);
        int is_pow  = (dag_opcode == OP_POW);

        /* Classify operands: numeric/temporal vectors or scalars */
        int8_t lt = left_coll ? left->type : -(left->type);
        int8_t rt = right_coll ? right->type : -(right->type);
        #define IS_NUM_TYPE(t) ((t)==RAY_I64||(t)==RAY_F64||(t)==RAY_F32||(t)==RAY_I32||(t)==RAY_I16|| \
                                (t)==RAY_U8||(t)==RAY_DATE||(t)==RAY_TIME||(t)==RAY_TIMESTAMP)
        #define IS_POW_TYPE(t) ((t)==RAY_BOOL||(t)==RAY_U8||(t)==RAY_I16||(t)==RAY_I32|| \
                                (t)==RAY_I64||(t)==RAY_F64)
        int l_num_vec = left_coll && ray_is_vec(left) && (is_pow ? IS_POW_TYPE(lt) : IS_NUM_TYPE(lt));
        int r_num_vec = right_coll && ray_is_vec(right) && (is_pow ? IS_POW_TYPE(rt) : IS_NUM_TYPE(rt));
        int l_num_scalar = !left_coll && (is_pow ? IS_POW_TYPE(lt) : IS_NUM_TYPE(lt));
        int r_num_scalar = !right_coll && (is_pow ? IS_POW_TYPE(rt) : IS_NUM_TYPE(rt));
        #undef IS_POW_TYPE
        #undef IS_NUM_TYPE

        int can_dag = (l_num_vec || r_num_vec) &&
                      (l_num_vec || l_num_scalar) && (r_num_vec || r_num_scalar);
        /* Null scalar atoms lose their null bit in DAG constants — use slow path */
        if (l_num_scalar && RAY_ATOM_IS_NULL(left)) can_dag = 0;
        if (r_num_scalar && RAY_ATOM_IS_NULL(right)) can_dag = 0;
        /* Null-bearing vectors route through the DAG too: the null model is
         * sentinel-based end to end (bitmap decommissioned), and expr.c's
         * kernels are sentinel-aware (null-fusion) — the old bail predated
         * that and silently degraded every nullable-vector op to the boxed
         * per-element loop. */

        /* Div/mod: only I64×I64 (executor has floor-div semantics for I64) */
        if (is_idiv && !(lt == RAY_I64 && rt == RAY_I64)) can_dag = 0;
        /* Comparisons of different numeric types run through the DAG as well:
         * the result is a BOOL, so there is no type tag to lose, and the
         * kernels compare integer-family operands as int64 and anything with a
         * float as double, as ray_eq_fn and friends do.  A temporal operand of
         * a different type is still refused just below.  (Same-type-only
         * sent (== u8vec 7), an integer literal against a narrower or float
         * vector, to the boxed per-element loop: 24 - 37 ms per million rows.) */
        /* Cross-type temporal: DAG promote() loses type tag (int+TIMESTAMP→I64 not TIMESTAMP).
         * POW rejects temporals entirely; its scalar builtin treats power as
         * a numeric-only operation and returns F64. */
        {   int lt_temp = (lt==RAY_DATE||lt==RAY_TIME||lt==RAY_TIMESTAMP);
            int rt_temp = (rt==RAY_DATE||rt==RAY_TIME||rt==RAY_TIMESTAMP);
            if (is_pow && (lt_temp || rt_temp)) can_dag = 0;
            else if ((lt_temp || rt_temp) && lt != rt) can_dag = 0;
        }

        if (can_dag) {
                ray_graph_t* g = ray_graph_new(NULL);
                if (g) {
                    /* A scalar on the left of a comparison with a vector is
                     * the mirrored comparison with the scalar on the right
                     * ((< 7 v) is (> v 7)), the shape the typed kernels
                     * broadcast.  Null-as-minimum is symmetric, so the
                     * answer is the same. */
                    ray_t* dl = left;
                    ray_t* dr = right;
                    int dl_scalar = l_num_scalar, dr_scalar = r_num_scalar;
                    uint16_t dop = dag_opcode;
                    if (is_cmp && l_num_scalar && r_num_vec) {
                        dl = right; dr = left;
                        dl_scalar = r_num_scalar; dr_scalar = l_num_scalar;
                        switch (dag_opcode) {
                        case OP_LT: dop = OP_GT; break;
                        case OP_LE: dop = OP_GE; break;
                        case OP_GT: dop = OP_LT; break;
                        case OP_GE: dop = OP_LE; break;
                        default: break;
                        }
                    }
                    /* Build left operand node */
                    ray_op_t* lop = NULL;
                    if (dl_scalar) {
                        if (dl->type == -RAY_F64)
                            lop = ray_const_f64(g, dl->f64);
                        /* The exact-width scalar-left kernel checks the literal's
                         * runtime tag, not just the DAG node's out_type. */
                        else if (dl->type == -RAY_F32 ||
                                 ((dop == OP_ADD || dop == OP_SUB || dop == OP_MUL) &&
                                  (lt == RAY_I16 || lt == RAY_I32) && lt == rt))
                            lop = ray_const_atom(g, dl);
                        else {
                            int64_t sv = as_i64(dl);
                            lop = ray_const_i64(g, sv);
                            if (lop) lop->out_type = -(dl->type);
                        }
                    } else {
                        lop = ray_const_vec(g, dl);
                    }
                    ray_op_t* rop = NULL;
                    if (dr_scalar) {
                        if (dr->type == -RAY_F64)
                            rop = ray_const_f64(g, dr->f64);
                        else if (dr->type == -RAY_F32)
                            rop = ray_const_atom(g, dr);
                        else {
                            int64_t sv = as_i64(dr);
                            rop = ray_const_i64(g, sv);
                            if (rop) rop->out_type = -(dr->type);
                        }
                    } else {
                        rop = ray_const_vec(g, dr);
                    }
                    if (lop && rop) {
                        ray_op_t* root = ray_binop(g, dop, lop, rop);
                        if (root) {
                            if (is_idiv) root->out_type = RAY_I64;
                            ray_t* result = ray_execute(g, root);
                            ray_graph_free(g);
                            /* a cancelled query stays cancelled: the
                             * per-element loop below is not its answer */
                            if (result && RAY_IS_ERR(result)) {
                                if (strcmp(ray_err_code(result), "cancel") == 0) { ray_release(e0); return result; }
                                ray_error_free(result);
                                result = NULL;
                            }
                            if (result && !RAY_IS_ERR(result)) {
                                /* Restore temporal type tag if promote() collapsed it */
                                if (ray_is_vec(result) && result->type != out_type &&
                                    ray_elem_size(result->type) == ray_elem_size(out_type))
                                    result->type = out_type;
                                ray_release(e0);
                                return result;
                            }
                        } else { ray_graph_free(g); }
                    } else { ray_graph_free(g); }
                }
            }
        }
    /* B8 comparisons: both operands B8 (the DAG path above takes no BOOL). */
    if (!force_boxed && dag_opcode >= OP_EQ && dag_opcode <= OP_GE && out_type == RAY_BOOL) {
        ray_t* r = bool_cmp_fast(dag_opcode, left, right);
        if (r) { ray_release(e0); return r; }
    }
    /* R7 fast path: (== or !=) of SYM-vec against a SYM atom.
     *
     * The DAG path above doesn't handle SYM (IS_NUM_TYPE excludes it),
     * so without this, ray_neq_fn / ray_eq_fn fan out to one allocation
     * per row in the slow loop.  At 5M rows the per-element bool atom
     * thrash dominates: `(!= URL nu)` standalone takes 113 ms when the
     * raw work is one i64 lookup + N width-truncated cmpneq.
     *
     * Handles either operand order; output is RAY_BOOL.  Canonical empty SYM
     * uses id 0, so value comparison also preserves empty/null equality. */
    if (!force_boxed && (dag_opcode == OP_EQ || dag_opcode == OP_NE) &&
        out_type == RAY_BOOL) {
        int l_is_sym_vec = left_coll  && ray_is_vec(left)  && left->type  == RAY_SYM;
        int r_is_sym_vec = right_coll && ray_is_vec(right) && right->type == RAY_SYM;
        int l_is_sym_atom = !left_coll  && left  && left->type  == -RAY_SYM;
        int r_is_sym_atom = !right_coll && right && right->type == -RAY_SYM;
        if ((l_is_sym_vec && r_is_sym_atom) || (r_is_sym_vec && l_is_sym_atom)) {
            ray_t* vv  = l_is_sym_vec ? left  : right;
            ray_t* atom = l_is_sym_vec ? right : left;
            int64_t n = vv->len;

            ray_t* out = ray_vec_new(RAY_BOOL, n);
            if (out && !RAY_IS_ERR(out)) {
                out->len = n;
                bool*    obuf = (bool*)ray_data(out);
                const void* src = ray_data(vv);
                uint8_t va = vv->attrs;
                int64_t target = atom->i64;
                bool    invert = (dag_opcode == OP_NE);

                /* sym-domain Phase 2: the loops below compare RAW cell
                 * ids, which are positions in the COLUMN's domain.  The
                 * atom carries a runtime id — re-express it in the
                 * column's domain once before the loops.  Runtime-domain
                 * columns keep the raw id (byte-identical fast path).
                 * Literal absent from the domain ⇒ equals no cell: ==
                 * all-false, != all-true. */
                int target_absent = 0;
                struct ray_sym_domain_s* dom = ray_sym_vec_domain(vv);
                if (dom != ray_sym_runtime_domain()) {
                    ray_t* ts = ray_sym_str(atom->i64);
                    int64_t pos = ts ? ray_sym_domain_find(dom,
                                          ray_str_ptr(ts),
                                          ray_str_len(ts))
                                     : -1;
                    if (pos >= 0) target = pos;
                    else          target_absent = 1;
                }

                if (target_absent) {
                    bool fill = invert; /* != absent → true; == absent → false */
                    for (int64_t i = 0; i < n; i++) obuf[i] = fill;
                } else {
                    /* Canonical null is raw id 0, so the compare remains a
                     * tight per-width raw-id loop, pool-parallel above the
                     * shared morsel threshold (symcmp_eq_fn). */
                    symcmp_ctx_t sctx = {
                        .ld = src, .rd = NULL, .lattrs = va, .rattrs = 0,
                        .target = target, .lut = NULL, .lcount = 0,
                        .obuf = obuf, .invert = invert,
                    };
                    symcmp_eq_run(&sctx, n);
                }
                ray_release(e0);
                return out;
            }
            map_drop_vec(out);   /* a failed ray_vec_new is an owned error */
            /* Fall through to slow path on allocation failure. */
        }
    }

    /* R8 fast path: (== or !=) of SYM-vec against SYM-vec.
     *
     * Same rationale as R7 — the DAG path excludes SYM, so without this a
     * vector-vector symbol compare fans out to one boxed compare per row
     * (measured 53 ms for 596k rows when the raw work is an id compare).
     * Same-domain vectors (including both-runtime, the common case)
     * compare raw cell ids width-aware.  Cross-domain pairs translate the
     * LEFT vocabulary into the RIGHT domain once — positions, not rows —
     * then compare in the right id space; an untranslatable left id
     * matches nothing.  Canonical null is raw id 0 and needs no side bitmap. */
    if (!force_boxed && (dag_opcode == OP_EQ || dag_opcode == OP_NE) &&
        out_type == RAY_BOOL &&
        left_coll && right_coll && ray_is_vec(left) && ray_is_vec(right) &&
        left->type == RAY_SYM && right->type == RAY_SYM) {
        int64_t n = left->len;  /* lengths verified equal above */
        struct ray_sym_domain_s* ldom = ray_sym_vec_domain(left);
        struct ray_sym_domain_s* rdom = ray_sym_vec_domain(right);
        int64_t lcount = (ldom == rdom) ? 0 : ray_sym_domain_count(ldom);
        if (ldom == rdom || (lcount >= 0 && lcount <= (int64_t)1 << 22)) {
            ray_t* out = ray_vec_new(RAY_BOOL, n);
            ray_t* lut_hdr = NULL;
            int64_t* lut = NULL;
            if (ldom != rdom && lcount > 0 && out && !RAY_IS_ERR(out)) {
                lut_hdr = ray_alloc(lcount * (int64_t)sizeof(int64_t));
                if (lut_hdr) {
                    lut = (int64_t*)ray_data(lut_hdr);
                    for (int64_t p = 0; p < lcount; p++) {
                        ray_t* sstr = ray_sym_domain_str(ldom, p);
                        lut[p] = sstr ? ray_sym_domain_find(rdom,
                                                            ray_str_ptr(sstr),
                                                            ray_str_len(sstr))
                                      : -1;
                    }
                }
            }
            if (out && !RAY_IS_ERR(out) && (ldom == rdom || lcount == 0 || lut)) {
                out->len = n;
                bool* obuf = (bool*)ray_data(out);
                const void* ld = ray_data(left);
                const void* rd = ray_data(right);
                uint8_t lattrs = left->attrs, rattrs = right->attrs;
                bool invert = (dag_opcode == OP_NE);
                symcmp_ctx_t sctx = {
                    .ld = ld, .rd = rd, .lattrs = lattrs, .rattrs = rattrs,
                    .target = 0,
                    .lut = (ldom == rdom) ? NULL : lut,
                    .lcount = lcount,
                    .obuf = obuf, .invert = invert,
                };
                symcmp_eq_run(&sctx, n);
                if (lut_hdr) ray_release(lut_hdr);
                ray_release(e0);
                return out;
            }
            if (lut_hdr) ray_release(lut_hdr);
            map_drop_vec(out);   /* a failed ray_vec_new is an owned error */
            /* Fall through to slow path on allocation failure. */
        }
    }

    /* R9 fast path: SYM ordering compares (< <= > >=), vec-vec and
     * vec-vs-atom.  Lexicographic order (the char_str_cmp / build_enum_rank
     * comparator) is precomputed once over the domain VOCABULARY — a rank
     * LUT for vec-vec (union-ranked across two domains so equal strings
     * rank equal), a three-way verdict LUT for vec-vs-atom — then the row
     * loop is pure integer reads, pool-parallel; canonical null is id 0. */
    if (!force_boxed && out_type == RAY_BOOL &&
        (dag_opcode == OP_LT || dag_opcode == OP_LE ||
         dag_opcode == OP_GT || dag_opcode == OP_GE)) {
        int l_sym_vec = left_coll  && ray_is_vec(left)  && left->type  == RAY_SYM;
        int r_sym_vec = right_coll && ray_is_vec(right) && right->type == RAY_SYM;
        int l_sym_atom = !left_coll  && left  && left->type  == -RAY_SYM;
        int r_sym_atom = !right_coll && right && right->type == -RAY_SYM;

        if (l_sym_vec && r_sym_vec) {
            struct ray_sym_domain_s* ldom = ray_sym_vec_domain(left);
            struct ray_sym_domain_s* rdom = ray_sym_vec_domain(right);
            int64_t dl = ray_sym_domain_count(ldom);
            int64_t dr = (rdom == ldom) ? 0 : ray_sym_domain_count(rdom);
            if (dl >= 0 && dl + dr <= (int64_t)1 << 22) {
                int64_t n = left->len;
                ray_t* out = ray_vec_new(RAY_BOOL, n);
                ray_t* rl_hdr = (dl > 0)
                    ? ray_alloc(dl * (int64_t)sizeof(int64_t)) : NULL;
                ray_t* rr_hdr = (rdom != ldom && dr > 0)
                    ? ray_alloc(dr * (int64_t)sizeof(int64_t)) : NULL;
                int64_t* rankL = rl_hdr ? (int64_t*)ray_data(rl_hdr) : NULL;
                int64_t* rankR = rr_hdr ? (int64_t*)ray_data(rr_hdr) : NULL;
                bool built = out && !RAY_IS_ERR(out) && (dl == 0 || rankL) &&
                             (rdom == ldom || dr == 0 || rankR) &&
                    sym_union_ranks(ldom, dl,
                                    (rdom == ldom) ? NULL : rdom, dr,
                                    rankL, rankR) == 0;
                if (built) {
                    out->len = n;
                    symord_ctx_t sctx = {
                        .ld = ray_data(left),  .lattrs = left->attrs,
                        .rd = ray_data(right), .rattrs = right->attrs,
                        .rankL = rankL,
                        .rankR = (rdom == ldom) ? rankL : rankR,
                        .verdict = NULL,
                        .obuf = (bool*)ray_data(out), .opc = dag_opcode,
                    };
                    ray_pool_t* pool = ray_pool_get();
                    if (pool && n >= RAY_PARALLEL_THRESHOLD)
                        ray_pool_dispatch(pool, symord_cmp_fn, &sctx, n);
                    else
                        symord_cmp_fn(&sctx, 0, 0, n);
                    if (rl_hdr) ray_release(rl_hdr);
                    if (rr_hdr) ray_release(rr_hdr);
                    ray_release(e0);
                    return out;
                }
                if (rl_hdr) ray_release(rl_hdr);
                if (rr_hdr) ray_release(rr_hdr);
                map_drop_vec(out);   /* a failed ray_vec_new is an owned error */
                /* fall through on OOM */
            }
        } else if ((l_sym_vec && r_sym_atom) || (r_sym_vec && l_sym_atom)) {
            ray_t* vv   = l_sym_vec ? left  : right;
            ray_t* atom = l_sym_vec ? right : left;
            struct ray_sym_domain_s* dom = ray_sym_vec_domain(vv);
            int64_t d = ray_sym_domain_count(dom);
            ray_t* as = ray_sym_str(atom->i64);
            if (as && d > 0 && d <= (int64_t)1 << 22) {
                const char* ap = ray_str_ptr(as);
                uint32_t    al = (uint32_t)ray_str_len(as);
                int64_t n = vv->len;
                ray_t* out = ray_vec_new(RAY_BOOL, n);
                ray_t* vd_hdr = ray_alloc(d);
                if (out && !RAY_IS_ERR(out) && vd_hdr) {
                    int8_t* verdict = (int8_t*)ray_data(vd_hdr);
                    for (int64_t idp = 0; idp < d; idp++) {
                        ray_t* es = ray_sym_domain_str(dom, idp);
                        const char* ep = es ? ray_str_ptr(es) : NULL;
                        uint32_t    el = es ? (uint32_t)ray_str_len(es) : 0;
                        uint32_t ml = el < al ? el : al;
                        int cv = ml ? memcmp(ep, ap, ml) : 0;
                        if (cv == 0) cv = (el > al) - (el < al);
                        /* fold orientation: verdict is cmp(LEFT, RIGHT) */
                        if (!l_sym_vec) cv = -cv;
                        verdict[idp] = (int8_t)((cv > 0) - (cv < 0));
                    }
                    out->len = n;
                    symord_ctx_t sctx = {
                        .ld = ray_data(vv), .lattrs = vv->attrs,
                        .rd = NULL, .rattrs = 0,
                        .rankL = NULL, .rankR = NULL,
                        .verdict = verdict,
                        .obuf = (bool*)ray_data(out), .opc = dag_opcode,
                    };
                    ray_pool_t* pool = ray_pool_get();
                    if (pool && n >= RAY_PARALLEL_THRESHOLD)
                        ray_pool_dispatch(pool, symord_cmp_fn, &sctx, n);
                    else
                        symord_cmp_fn(&sctx, 0, 0, n);
                    ray_release(vd_hdr);
                    ray_release(e0);
                    return out;
                }
                if (vd_hdr) ray_release(vd_hdr);
                map_drop_vec(out);   /* a failed ray_vec_new is an owned error */
            }
        }
    }

    /* R10 fast path: STR compares (== != < <= > >=), vec-vec and
     * vec-vs-atom — canonical bytes comparator per element, pool-parallel,
     * no boxing.  Gated off null-bearing operands: null compare semantics
     * (null matches nothing, null==null is true) belong to the scalar fns. */
    if (!force_boxed && out_type == RAY_BOOL &&
        (dag_opcode == OP_EQ || dag_opcode == OP_NE ||
         dag_opcode == OP_LT || dag_opcode == OP_LE ||
         dag_opcode == OP_GT || dag_opcode == OP_GE)) {
        int l_str_vec = left_coll  && ray_is_vec(left)  && left->type  == RAY_STR &&
                        !ray_vec_has_nulls(left);
        int r_str_vec = right_coll && ray_is_vec(right) && right->type == RAY_STR &&
                        !ray_vec_has_nulls(right);
        int l_str_atom = !left_coll  && left  && left->type  == -RAY_STR &&
                         !RAY_ATOM_IS_NULL(left);
        int r_str_atom = !right_coll && right && right->type == -RAY_STR &&
                         !RAY_ATOM_IS_NULL(right);
        int vv_shape = l_str_vec && r_str_vec;
        int va_shape = (l_str_vec && r_str_atom) || (r_str_vec && l_str_atom);
        if (vv_shape || va_shape) {
            int64_t n = l_str_vec ? left->len : right->len;
            ray_t* out = ray_vec_new(RAY_BOOL, n);
            if (out && !RAY_IS_ERR(out)) {
                out->len = n;
                ray_t* atom = vv_shape ? NULL : (l_str_vec ? right : left);
                strcmp_ctx_t sctx = {
                    .lv = (vv_shape || l_str_vec) ? left  : NULL,
                    .rv = (vv_shape || r_str_vec) ? right : NULL,
                    .ap = atom ? ray_str_ptr(atom) : NULL,
                    .al = atom ? ray_str_len(atom) : 0,
                    .obuf = (bool*)ray_data(out), .opc = dag_opcode,
                };
                if (sctx.lv && !ray_is_vec(sctx.lv)) sctx.lv = NULL;
                if (sctx.rv && !ray_is_vec(sctx.rv)) sctx.rv = NULL;
                ray_pool_t* pool = ray_pool_get();
                if (pool && n >= RAY_PARALLEL_THRESHOLD)
                    ray_pool_dispatch(pool, strvec_cmp_fn, &sctx, n);
                else
                    strvec_cmp_fn(&sctx, 0, 0, n);
                ray_release(e0);
                return out;
            }
            map_drop_vec(out);   /* a failed ray_vec_new is an owned error */
        }
    }

    /* R11 fast path: GUID compares (== != < <= > >=), vec-vec and
     * vec-vs-atom.  The DAG path declines GUID, so without this every cell
     * of both operands was boxed into an atom for the scalar fn: ~7 ms for
     * 140k rows against ~0.2 ms for SYM.  A null guid is all-zero bytes, so
     * comparing the bytes gives the scalar fns' answers for nulls too:
     * null == null, and a null below every other value. */
    if (!force_boxed && out_type == RAY_BOOL &&
        dag_opcode >= OP_EQ && dag_opcode <= OP_GE) {
        int l_guid_vec  = left_coll  && left->type  == RAY_GUID;
        int r_guid_vec  = right_coll && right->type == RAY_GUID;
        int l_guid_atom = !left_coll  && left->type  == -RAY_GUID;
        int r_guid_atom = !right_coll && right->type == -RAY_GUID;
        if ((l_guid_vec || l_guid_atom) && (r_guid_vec || r_guid_atom)) {
            static const uint8_t null_guid[16] = {0};
            ray_t* out = ray_vec_new(RAY_BOOL, len);
            /* The slow path would ask for the same vector: hand back this
             * failure rather than retry it there. */
            if (!out || RAY_IS_ERR(out)) {
                ray_release(e0);
                return out ? out : ray_error("oom", NULL);
            }
            out->len = len;
            guidcmp_ctx_t gctx = {
                .lp = l_guid_vec ? (const uint8_t*)ray_data(left)
                    : left->obj  ? (const uint8_t*)ray_data(left->obj) : null_guid,
                .lstride = l_guid_vec ? 16 : 0,
                .rp = r_guid_vec ? (const uint8_t*)ray_data(right)
                    : right->obj ? (const uint8_t*)ray_data(right->obj) : null_guid,
                .rstride = r_guid_vec ? 16 : 0,
                .obuf = (bool*)ray_data(out), .opc = dag_opcode,
            };
            ray_pool_t* pool = ray_pool_get();
            if (pool && len >= RAY_PARALLEL_THRESHOLD) {
                ray_pool_dispatch(pool, guidvec_cmp_fn, &gctx, len);
                /* A cancel skips the tasks not yet started: no answer. */
                if (ray_interrupted() ||
                    atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
                    ray_release(out);
                    ray_release(e0);
                    return ray_error("cancel", "interrupted");
                }
            } else {
                guidvec_cmp_fn(&gctx, 0, 0, len);
            }
            ray_release(e0);
            return out;
        }
    }

    /* SLOW PATH: per-element scalar loop (fallback for mixed types, temporal, etc.) */
    if (!force_boxed &&
        (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_F32 || out_type == RAY_I32 ||
         out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
         out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP || out_type == RAY_TIMESPAN)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);

        for (int64_t i = 1; i < len; i++) {
            if ((i & 0xFFFF) == 0 && EVAL_INTERRUPTED()) { ray_release(vec); ray_release(e0); return ray_error("cancel", "interrupted"); }
            int la = 0, ra = 0;
            ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
            ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
            if (RAY_IS_ERR(a) || RAY_IS_ERR(b)) {
                ray_release(vec);
                ray_release(e0);
                return map_cell_error(left, left_coll, a, la, right, right_coll, b, ra);
            }
            ray_t* elem = fn(a, b);
            if (la) ray_release(a);
            if (ra) ray_release(b);
            if (RAY_IS_ERR(elem)) { ray_release(vec); ray_release(e0); return elem; }
            if (RAY_UNLIKELY(!map_cell_is_number(elem))) {
                /* A cell whose result is not a number (a GUID, symbol or
                 * string fill keeps, a nested result): the typed vector
                 * would hold it through the integer field, the GUID's
                 * pointer, the symbol's id or the string's bytes as a
                 * number.  The result is the LIST of the cells, as it is
                 * when that cell comes first. */
                ray_release(elem);
                ray_release(vec);
                goto boxed_cells;
            }
            store_typed_elem(vec, i, elem);
            ray_release(elem);
        }
        ray_release(e0);
        return vec;
    }
boxed_cells:;

    /* Determine scalar int type for list+scalar coercion.
     * When a boxed list is combined with a scalar, integer results
     * are coerced to the scalar's integer type (integer-promotion convention). */
    int8_t scalar_int_type = 0;
    if (force_boxed) {
        ray_t* scalar = (!left_coll) ? left : (!right_coll ? right : NULL);
        if (scalar && ray_is_atom(scalar)) {
            int8_t st = scalar->type;
            if (st == -RAY_I16 || st == -RAY_I32 || st == -RAY_I64 || st == -RAY_U8)
                scalar_int_type = st;
        }
    }

    /* Coerce an integer atom to the scalar's integer type */
    #define COERCE_TO_SCALAR(elem) do { \
        if (scalar_int_type && ray_is_atom(elem) && elem->type != scalar_int_type && \
            elem->type != -RAY_F64 && elem->type != -RAY_BOOL && is_numeric(elem)) { \
            int64_t _v = as_i64(elem); \
            ray_t* _coerced = make_typed_int(scalar_int_type, _v); \
            ray_release(elem); \
            elem = _coerced; \
        } \
    } while(0)

    /* Fallback: boxed list for non-numeric output or mixed-type input */
    COERCE_TO_SCALAR(e0);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;  /* first element already computed */

    for (int64_t i = 1; i < len; i++) {
        if ((i & 0xFFFF) == 0 && EVAL_INTERRUPTED()) {
            result->len = i;                 /* the filled prefix only */
            ray_release(result);
            return ray_error("cancel", "interrupted");
        }
        int la = 0, ra = 0;
        ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
        ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
        ray_t* elem;
        if (RAY_IS_ERR(a) || RAY_IS_ERR(b)) {
            elem = map_cell_error(left, left_coll, a, la, right, right_coll, b, ra);
            la = ra = 0;   /* released with the error */
        } else if (is_collection(a) || is_collection(b)) {
            /* Recursive auto-map when list element is itself a collection */
            elem = atomic_map_binary(fn, a, b);
        } else {
            elem = fn(a, b);
        }
        if (la) ray_release(a);
        if (ra) ray_release(b);
        if (RAY_IS_ERR(elem)) {
            /* Only out[0..i) are initialized.  Trim len so the list free
             * releases exactly the filled prefix — with len still at the
             * full length it would re-release those children after this
             * path already had (double free) and then "release" the
             * uninitialized garbage pointers in out[i..len). */
            result->len = i;
            ray_release(result);
            return elem;
        }
        COERCE_TO_SCALAR(elem);
        out[i] = elem;
    }
    #undef COERCE_TO_SCALAR
    if (!force_boxed && (fn == ray_fill_fn || fn == ray_min2_fn || fn == ray_max2_fn))
        return atomic_collapse_wide(result);
    return ray_list_collapse_bool(result);
}

ray_t* ray_list_collapse_bool(ray_t* list) {
    if (!list || RAY_IS_ERR(list) || list->type != RAY_LIST) return list;
    int64_t n = ray_len(list);
    ray_t** e = (ray_t**)ray_data(list);
    for (int64_t i = 0; i < n; i++)
        if (!e[i] || e[i]->type != -RAY_BOOL) return list;
    ray_t* vec = ray_vec_new(RAY_BOOL, n);
    /* a B8 vector it could not allocate is the call's oom: the list of
     * cells is not the answer (and the failure is an owned error) */
    if (RAY_IS_ERR(vec)) { ray_release(list); return vec; }
    uint8_t* d = (uint8_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) d[i] = e[i]->b8 ? 1 : 0;
    vec->len = n;
    ray_release(list);
    return vec;
}

/* One element of a collection mapped by an atomic unary function.  A nested
 * collection (the rows of a list of vectors) is mapped in turn, as the binary
 * atomic path does, instead of being handed whole to a function that only
 * takes atoms. */
static ray_t* atomic_unary_elem(ray_unary_fn fn, ray_t* e) {
    return is_collection(e) ? atomic_map_unary(fn, e) : fn(e);
}

/* Map a unary function element-wise over a collection.
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise;
 * a nested collection maps to a collection of the same shape. */
typedef struct { ray_t* x; int64_t i; } nest_frame_t;
static _Thread_local ray_walk_memo_t g_nest_memo;
bool ray_nests_deeper_than(ray_t* x, int64_t limit) {
    ray_walk_memo_t* m = NULL;   /* the thread-local memo's address only when wanted */
    bool own = false;
    if (!x || RAY_IS_ERR(x) || (x->type != RAY_LIST && x->type != RAY_DICT)) return limit < 1;
    nest_frame_t local[64];
    nest_frame_t* st = local;
    int64_t cap = 64, top = 0;
    bool deeper = false;
    st[top++] = (nest_frame_t){ x, 0 };
    while (top > 0 && !deeper) {
        nest_frame_t* f = &st[top - 1];
        ray_t* c;
        if (f->x->type == RAY_DICT) {               /* its keys, then its values */
            if (f->i >= 2) { top--; continue; }
            c = f->i == 0 ? ray_dict_keys(f->x) : ray_dict_vals(f->x);
        } else {
            if (f->i >= f->x->len) { top--; continue; }
            c = ((ray_t**)ray_data(f->x))[f->i];
        }
        f->i++;
        if (!c || RAY_IS_ERR(c) || (c->type != RAY_LIST && c->type != RAY_DICT)) {
            if (top + 1 > limit) deeper = true;
            continue;
        }
        if (top + 1 > limit) { deeper = true; break; }
        /* A shared container already walked from this depth or deeper
         * holds nothing deeper from here (#1627): not walked again. */
        if (RAY_UNLIKELY(ray_wm_reused(c))) {
            if (!m) m = &g_nest_memo;
            if (!m->active) own = ray_wm_begin(m);
            uint64_t at;
            if (m->active && ray_wm_get(m, c, NULL, 0, &at) && at >= (uint64_t)(top + 1)) continue;
            if (m->active) ray_wm_put(m, c, NULL, 0, (uint64_t)(top + 1));
        }
        if (top == cap) {
            nest_frame_t* grown = (nest_frame_t*)ray_alloc_raw((size_t)cap * 2 * sizeof *grown);
            if (!grown) { deeper = true; break; }   /* cannot measure it: refuse */
            memcpy(grown, st, (size_t)top * sizeof *grown);
            if (st != local) ray_free_raw(st);
            st = grown;
            cap *= 2;
        }
        st[top++] = (nest_frame_t){ c, 0 };
    }
    if (st != local) ray_free_raw(st);
    if (own) ray_wm_end(m);
    return deeper;
}

__attribute__((noinline)) static ray_t* atomic_map_unary_memo(ray_unary_fn fn, ray_t* arg) {
    uint64_t tag = (uint64_t)(uintptr_t)fn | (UINT64_C(1) << 63);   /* apart from a binary op's */
    map_ctx_t c = { NULL, fn, 0, arg, NULL };
    return ray_wm_result(&g_map_memo, arg, NULL, tag, map_unary_run, &c);
}
ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg) {
    if (RAY_UNLIKELY(map_memo_wanted(arg))) return atomic_map_unary_memo(fn, arg);
    return atomic_map_unary_c(fn, arg);
}
/* A copy of an integer-family vector (B8, U8, I16, I32, I64) as the boxed
 * map builds it: the same cells, and HAS_NULLS when a cell is the type's
 * null sentinel (store_typed_elem flags each null it stores).  An oom is
 * returned as the error it is. */
static ray_t* int_vec_copy(ray_t* v, int64_t len) {
    ray_t* r = ray_vec_new(v->type, len);
    /* an allocation failure is the boxed map's answer too (its own
     * ray_vec_new): the owned oom goes back to the caller */
    if (RAY_IS_ERR(r)) return r;
    r->len = len;
    const void* src = ray_data(v);
    memcpy(ray_data(r), src, (size_t)len * ray_elem_size(v->type));
    bool nulls = false;
    switch (v->type) {
    case RAY_I16: { const int16_t* p = src; for (int64_t i = 0; i < len; i++) nulls |= p[i] == NULL_I16; break; }
    case RAY_I32: { const int32_t* p = src; for (int64_t i = 0; i < len; i++) nulls |= p[i] == NULL_I32; break; }
    case RAY_I64: { const int64_t* p = src; for (int64_t i = 0; i < len; i++) nulls |= p[i] == NULL_I64; break; }
    default: break;   /* B8 and U8 hold no null */
    }
    if (nulls) r->attrs |= RAY_ATTR_HAS_NULLS;
    return r;
}

/* A unary atomic builtin over a typed vector through the vector kernel the
 * query path runs (#1756): the boxed map below costs two allocations and
 * two frees per element around a one-instruction operation.  Taken only for
 * the builtins with a kernel, and its result kept only when it is what the
 * boxed map gives: a vector of the first cell's result type (`out_type`)
 * and the input's length.  Anything else (a type whose kernel answers
 * another type, a kernel's error other than a cancel, a graph that could
 * not be built) is dropped, freed, and the boxed map answers as before.
 * The integer floor / ceil copy's oom is returned, as the boxed map's own
 * allocation would return it.  NULL when not taken. */
__attribute__((noinline)) static ray_t* unary_kernel_vec(ray_unary_fn fn, ray_t* v, int8_t out_type, int64_t len) {
    ray_op_t* (*mk)(ray_graph_t*, ray_op_t*) =
        fn == ray_neg_fn   ? ray_neg       :
        fn == ray_abs_fn   ? ray_abs       :
        fn == ray_floor_fn ? ray_floor_op  :
        fn == ray_ceil_fn  ? ray_ceil_op   :
        fn == ray_round_fn ? ray_round_op  :
        fn == ray_sqrt_fn  ? ray_sqrt_op   :
        fn == ray_log_fn   ? ray_log_op    :
        fn == ray_exp_fn   ? ray_exp_op    : NULL;
    if (!mk) return NULL;
    /* Numbers only.  A temporal atom's answer depends on its value (`neg` of
     * a null TIMESTAMP is a null, of any other a type error), so the first
     * cell's type does not settle a temporal vector's: the boxed map keeps
     * them, and every other type the kernels do not read */
    if (v->type != RAY_BOOL && v->type != RAY_U8 && v->type != RAY_I16 &&
        v->type != RAY_I32 && v->type != RAY_I64 && v->type != RAY_F32 &&
        v->type != RAY_F64) return NULL;
    /* floor and ceil of an integer are the integer (ray_floor_fn hands the
     * atom back, a null included): the vector's copy, its nulls flagged as
     * the boxed map's stores flag them.  No kernel, no cast. */
    if ((fn == ray_floor_fn || fn == ray_ceil_fn) &&
        v->type != RAY_F64 && v->type != RAY_F32)
        return out_type == v->type ? int_vec_copy(v, len) : NULL;
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return NULL;
    ray_op_t* in = ray_const_vec(g, v);
    /* round answers a float for an integer cell (as it does for an atom):
     * the kernel keeps the integer type, so an integer vector is cast to
     * F64 first — rounding an exact integer changes nothing */
    if (in && fn == ray_round_fn && out_type == RAY_F64 && v->type != RAY_F64)
        in = ray_cast(g, in, RAY_F64);
    ray_op_t* root = in ? mk(g, in) : NULL;
    ray_t* r = root ? ray_execute(g, root) : NULL;
    ray_graph_free(g);
    if (!r) return NULL;
    if (RAY_IS_ERR(r)) {
        /* a cancelled query stays cancelled: the boxed map is not its answer */
        if (strcmp(ray_err_code(r), "cancel") == 0) return r;
        ray_error_free(r);
        ray_error_clear();
        return NULL;
    }
    if (!ray_is_vec(r) || r->type != out_type || r->len != len) { ray_release(r); return NULL; }
    return r;
}

/* The builtin over a collection's first cell: its result's type is the
 * map's (atomic_map_unary_c below) */
static inline __attribute__((always_inline)) ray_t* atomic_unary_first(ray_unary_fn fn, ray_t* arg) {
    int alloc0 = 0;
    ray_t* e0_in = collection_elem(arg, 0, &alloc0);
    ray_t* e0 = RAY_IS_ERR(e0_in) ? e0_in : atomic_unary_elem(fn, e0_in);
    if (alloc0) ray_release(e0_in);
    return e0;
}

static inline __attribute__((always_inline)) ray_t* atomic_map_unary_c(ray_unary_fn fn, ray_t* arg) {
    if (!is_collection(arg)) return fn(arg);
    /* A list's cells are mapped through here again: one C frame per level
     * of nesting, and a value can be nested arbitrarily deep at run time
     * (#1591).  Stop at the thread's stack reserve, as the evaluator does. */
    if (arg->type == RAY_LIST && RAY_UNLIKELY(ray_stack_short()))
        return ray_error("limit", "value nested too deep for this thread's stack");

    int64_t len = ray_len(arg);
    ray_t* e0;
    int8_t out_type;

    /* One test sends both the empty collection and a vector long enough
     * for its builtin's vector kernel (MAP_KERNEL_MIN_LEN) off the path of
     * a short one, so a short call pays nothing for the kernel */
    if (RAY_UNLIKELY((uint64_t)len - 1 >= (uint64_t)MAP_KERNEL_MIN_LEN - 1)) {
        if (len == 0) {
            /* Empty — fabricate a zero atom of the element type and run
             * `fn` to learn the output type; fall back to I64 if the
             * probe can't resolve a typed atom.  The probe is ours: a
             * failed one (or a failed fabricated operand) is freed and its
             * message cleared, since only the type is wanted from it. */
            ray_t* z = zero_atom_for_elem_type(arg);
            ray_t* probe = (z && !RAY_IS_ERR(z)) ? fn(z) : NULL;
            bool failed = (z && RAY_IS_ERR(z)) || (probe && RAY_IS_ERR(probe));
            if (z) { if (RAY_IS_ERR(z)) ray_error_free(z); else ray_release(z); }
            if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
                int8_t t = (int8_t)(-probe->type);
                ray_release(probe);
                return ray_vec_new(t, 0);
            }
            if (probe) { if (RAY_IS_ERR(probe)) ray_error_free(probe); else ray_release(probe); }
            if (failed) ray_error_clear();
            return ray_vec_new(RAY_I64, 0);
        }
        e0 = atomic_unary_first(fn, arg);
        if (RAY_IS_ERR(e0)) return e0;
        out_type = -(e0->type);
        /* A typed vector: its builtin's vector kernel, when it has one */
        if (arg->type != RAY_LIST && ray_is_vec(arg)) {
            ray_t* r = unary_kernel_vec(fn, arg, out_type, len);
            if (r) { ray_release(e0); return r; }
        }
    } else {
        /* Probe first element to determine output type */
        e0 = atomic_unary_first(fn, arg);
        if (RAY_IS_ERR(e0)) return e0;
        out_type = -(e0->type);
    }

    /* Try typed vector path for numeric/bool/temporal output */
    if (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_F32 || out_type == RAY_I32 ||
        out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
        out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP || out_type == RAY_TIMESPAN) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);
        ray_release(e0);
        /* A boxed list's cells may answer different types ((neg (list 1
         * 2.5)) is an I64 and an F64): stored into the first one's vector
         * the others would be converted, 2.5 truncated to 2 (#1749).  The
         * result is then the boxed list of the cells' own results, what
         * (map f x) gives.  A typed vector's cells share one type: its loop
         * is the one without the test (unswitched, so it pays nothing). */
#define MAP_UNARY_TYPED_LOOP(CHECK_MIXED)                                       \
        for (int64_t i = 1; i < len; i++) {                                     \
            int alloc = 0;                                                      \
            ray_t* e = collection_elem(arg, i, &alloc);                         \
            ray_t* elem = RAY_IS_ERR(e) ? e : atomic_unary_elem(fn, e);         \
            if (alloc) ray_release(e);                                          \
            if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }            \
            if (elem->type >= 0 || (CHECK_MIXED && elem->type != -out_type)) {  \
                /* A nested collection after an atom, or a cell of another     \
                 * type: the typed vector cannot hold it.  Start over as a     \
                 * boxed list. */                                               \
                ray_release(elem);                                              \
                ray_release(vec);                                               \
                int alloc_r = 0;                                                \
                ray_t* e_r = collection_elem(arg, 0, &alloc_r);                 \
                e0 = RAY_IS_ERR(e_r) ? e_r : atomic_unary_elem(fn, e_r);        \
                if (alloc_r) ray_release(e_r);                                  \
                if (RAY_IS_ERR(e0)) return e0;                                  \
                goto boxed;                                                     \
            }                                                                   \
            store_typed_elem(vec, i, elem);                                     \
            ray_release(elem);                                                  \
        }
        if (arg->type == RAY_LIST) { MAP_UNARY_TYPED_LOOP(1) }
        else                       { MAP_UNARY_TYPED_LOOP(0) }
#undef MAP_UNARY_TYPED_LOOP
        return vec;
    }

boxed:;
    /* Fallback: boxed list for non-numeric output */
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;

    for (int64_t i = 1; i < len; i++) {
        int alloc = 0;
        ray_t* e = collection_elem(arg, i, &alloc);
        ray_t* elem = RAY_IS_ERR(e) ? e : atomic_unary_elem(fn, e);
        if (alloc) ray_release(e);
        if (RAY_IS_ERR(elem)) {
            /* Trim to the initialized prefix — see atomic_map_binary_op:
             * the list free releases len children, so leaving len at the
             * full length double-freed out[0..i) and released garbage
             * pointers from the uninitialized tail. */
            result->len = i;
            ray_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* ══════════════════════════════════════════
 * Higher-order functions: map, pmap, fold, scan, filter, apply
 * ══════════════════════════════════════════ */

/* Calling a variadic builtin or a special form with values (map, fold, scan,
 * apply, ... hand their function its arguments already evaluated).
 *
 * An ordinary variadic builtin takes values: the same call the evaluator
 * makes for (f x y).  A special form takes its argument expressions and
 * evaluates them itself, so handed values it would evaluate them again (a
 * boxed list value would run as a call); `and` and `or` are the exceptions,
 * because the evaluator's own element-wise binary forms, ray_and_fn and
 * ray_or_fn, give the same answer for values that are already evaluated, and
 * a single value is their identity. */
static bool fn_is_vary(const ray_t* fn, ray_vary_fn f) {
    return fn->type == RAY_VARY && (ray_vary_fn)(uintptr_t)fn->i64 == f;
}

static ray_t* special_form_value_error(const ray_t* fn) {
    return ray_error("type", "call: '%s' is a special form: it takes its arguments unevaluated, "
                             "so it cannot be applied to values", ray_fn_name(fn));
}

/* An ordinary variadic builtin applied to `n` borrowed values. */
static ray_t* call_vary_values(ray_t* fn, ray_t* const* args, int64_t n) {
    if (fn->attrs & RAY_FN_SPECIAL_FORM) return special_form_value_error(fn);
    ray_t* own[2];
    for (int64_t i = 0; i < n; i++) { ray_retain(args[i]); own[i] = args[i]; }
    if (!(fn->attrs & RAY_FN_LAZY_AWARE)) {
        ray_t* err = materialize_owned_args(own, n);   /* releases them all on error */
        if (err) return err;
    }
    ray_t* r = ((ray_vary_fn)(uintptr_t)fn->i64)(own, n);
    for (int64_t i = 0; i < n; i++) ray_release(own[i]);
    return ray_sig_note(ray_fn_name(fn), r);
}

/* Helper: call a function object with 1 arg, returning result.
 * Handles UNARY, BINARY, LAMBDA types. Does not release fn or arg. */
ray_t* call_fn1(ray_t* fn, ray_t* arg) {
    if (fn_is_restricted(fn)) return ray_error("access", "restricted");
    if (fn->type == RAY_UNARY) {
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        ray_t* a = arg;
        bool owned = false;
        if (!(fn->attrs & RAY_FN_LAZY_AWARE) && ray_is_lazy(a)) {
            ray_retain(a);               /* give materialise its own ref; arg is borrowed */
            a = ray_lazy_materialize(a); /* consumes the retain we just added */
            if (!a || RAY_IS_ERR(a)) return a ? a : ray_error("type", NULL);
            owned = true;
        }
        ray_t* r;
        if ((fn->attrs & RAY_FN_ATOMIC) && is_collection(a))
            r = atomic_map_unary(f, a);
        else
            r = f(a);
        if (owned) ray_release(a);
        return ray_sig_note(ray_fn_name(fn), r);
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[1] = { arg };
        return call_lambda(fn, args, 1);
    }
    if (fn->type == RAY_BINARY)
        return ray_sig_note(ray_fn_name(fn),
                            ray_error("arity", "call: '%s' expects 2 arguments, got 1", ray_fn_name(fn)));
    if (fn->type == RAY_VARY) {
        if (fn_is_vary(fn, ray_and_vary_fn) || fn_is_vary(fn, ray_or_vary_fn)) {
            /* (and x) is (or x) is x */
            ray_t* a = arg;
            ray_retain(a);
            if (ray_is_lazy(a)) a = ray_lazy_materialize(a);   /* consumes our ref */
            return a ? a : ray_error("type", NULL);
        }
        return call_vary_values(fn, &arg, 1);
    }
    return ray_error("type", "call: expected a callable function, got %s", ray_type_name(fn->type));
}

/* Helper: call a function object with 2 args. Does not release fn or args. */
ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b) {
    if (fn_is_restricted(fn)) return ray_error("access", "restricted");
    ray_binary_fn bin = NULL;
    if (fn->type == RAY_BINARY)                  bin = (ray_binary_fn)(uintptr_t)fn->i64;
    else if (fn_is_vary(fn, ray_and_vary_fn))    bin = ray_and_fn;
    else if (fn_is_vary(fn, ray_or_vary_fn))     bin = ray_or_fn;
    if (bin) {
        ray_t* la = a;
        ray_t* lb = b;
        bool owned_a = false, owned_b = false;
        if (!(fn->attrs & RAY_FN_LAZY_AWARE)) {
            if (ray_is_lazy(la)) {
                ray_retain(la);               /* give materialise its own ref; a is borrowed */
                la = ray_lazy_materialize(la); /* consumes the retain we just added */
                if (!la || RAY_IS_ERR(la)) return la ? la : ray_error("type", NULL);
                owned_a = true;
            }
            if (ray_is_lazy(lb)) {
                ray_retain(lb);               /* give materialise its own ref; b is borrowed */
                lb = ray_lazy_materialize(lb); /* consumes the retain we just added */
                if (!lb || RAY_IS_ERR(lb)) {
                    if (owned_a) ray_release(la);
                    return lb ? lb : ray_error("type", NULL);
                }
                owned_b = true;
            }
        }
        ray_t* r;
        if ((fn->attrs & RAY_FN_ATOMIC) && (is_collection(la) || is_collection(lb)))
            r = atomic_map_binary(bin, la, lb);
        else
            r = bin(la, lb);
        if (owned_a) ray_release(la);
        if (owned_b) ray_release(lb);
        return ray_sig_note(ray_fn_name(fn), r);
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[2] = { a, b };
        return call_lambda(fn, args, 2);
    }
    if (fn->type == RAY_UNARY) {
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        ray_binary_fn f2 = unary_binary_overload(f);
        if (f2) {
            ray_t* la = a;
            ray_t* lb = b;
            bool owned_a = false, owned_b = false;
            if (ray_is_lazy(la)) {
                ray_retain(la);
                la = ray_lazy_materialize(la);
                if (!la || RAY_IS_ERR(la)) return la ? la : ray_error("type", NULL);
                owned_a = true;
            }
            if (ray_is_lazy(lb)) {
                ray_retain(lb);
                lb = ray_lazy_materialize(lb);
                if (!lb || RAY_IS_ERR(lb)) {
                    if (owned_a) ray_release(la);
                    return lb ? lb : ray_error("type", NULL);
                }
                owned_b = true;
            }
            ray_t* out = (is_collection(la) || is_collection(lb))
                       ? minmax_map(f2, la, lb) : f2(la, lb);
            if (owned_a) ray_release(la);
            if (owned_b) ray_release(lb);
            return out;
        }
        /* Legacy helper behavior for unary functions without an overload. */
        return f(a);
    }
    if (fn->type == RAY_VARY) {
        ray_t* args[2] = { a, b };
        return call_vary_values(fn, args, 2);
    }
    return ray_error("type", "call: expected a callable function, got %s", ray_type_name(fn->type));
}


/* ══════════════════════════════════════════
 * Sorting builtins
 * ══════════════════════════════════════════ */

/* Parallel fixed-width index gather for gather_by_idx: value memcpy loops
 * split across the worker pool.  Null-bit propagation stays serial (bit
 * writes within a shared word would race across range boundaries).
 *
 * gather_by_idx is a LEAF utility with 35+ call sites; unlike op-level
 * dispatch points it cannot know its execution context — the shared
 * ray_pool_par_dispatch_ok reentrancy check (core/pool.h) is what makes
 * dispatching from here safe. */
static inline bool gbi_par_ok(ray_pool_t* p, int64_t n) {
    return ray_pool_par_dispatch_ok(p, n, RAY_PARALLEL_THRESHOLD);
}

typedef struct {
    const char*    src;
    char*          dst;
    const int64_t* idx;
    uint8_t        esz;
    int64_t        rows;      /* src's rows when each task reads ahead (gbi_want); 0 otherwise */
    int64_t        row_end;   /* one past the last row idx lists */
} gbi_ctx_t;

static void gbi_fill_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gbi_ctx_t* c = (gbi_ctx_t*)ctxp;
    const char* src = c->src;
    char* dst = c->dst;
    const int64_t* idx = c->idx;
    /* the task's rows may span more than one read-ahead window: report
     * them as the copy goes */
    for (int64_t lo = start; lo < end; lo += 1024) {
        int64_t hi = end - lo < 1024 ? end : lo + 1024;
        if (c->rows) ray_pool_scan_col_at(src, c->rows, idx[lo], c->row_end);
        switch (c->esz) {
        case 8: for (int64_t i = lo; i < hi; i++) memcpy(dst + i*8, src + idx[i]*8, 8); break;
        case 4: for (int64_t i = lo; i < hi; i++) memcpy(dst + i*4, src + idx[i]*4, 4); break;
        case 2: for (int64_t i = lo; i < hi; i++) memcpy(dst + i*2, src + idx[i]*2, 2); break;
        case 1: for (int64_t i = lo; i < hi; i++) dst[i] = src[idx[i]]; break;
        case 16: for (int64_t i = lo; i < hi; i++) memcpy(dst + i*16, src + idx[i]*16, 16); break;
        default: for (int64_t i = lo; i < hi; i++) memcpy(dst + i*c->esz, src + idx[i]*c->esz, c->esz); break;
        }
    }
}

/* Reading a cold mapped column at scattered rows waits on one page fault
 * at a time.  When `idx` lists rows of a column registered for scan
 * read-ahead (core/pool.c) in order, with about a row per page of their
 * stretch or more, each task of the parallel gather keeps the rows past its
 * own requested, paced with the gather; else the pages of the rows are
 * requested up front (ray_col_want_rows). */
static void gbi_want(gbi_ctx_t* gc, ray_t* vec, const int64_t* idx, int64_t n, bool par) {
    if (!ray_pool_scan_on()) return;
    int64_t lo = n > 0 ? idx[0] : -1, hi = n > 0 ? idx[n - 1] : -1;
    if (par && lo >= 0 && lo <= hi && hi < vec->len &&
        (hi - lo + 1) * gc->esz / 4096 <= n && ray_pool_scan_holds(gc->src, vec->len)) {
        gc->rows = vec->len;
        gc->row_end = hi + 1;
        return;
    }
    ray_col_want_rows(vec, idx, n);
}

/* STR gather in two parallel passes.  A STR cell is a 16-byte descriptor
 * (inline up to 12 bytes, else an offset into the vector's immutable pool),
 * so the tasks first count the pooled bytes of their rows, a prefix sum over
 * the tasks places each task's region of the output pool, then every task
 * copies its descriptors and pooled bytes into disjoint ranges.  The
 * per-cell ray_str_vec_set it replaces (a copy-on-write check and a pool
 * growth per cell, serial) was the whole cost of compacting a STR key over
 * millions of rows.  NULL when the pooled bytes pass the u32 offsets, the
 * per-cell path then reports it. */
typedef struct {
    const ray_str_t* src;
    const char*      spool;
    const int64_t*   idx;
    ray_str_t*       dst;
    char*            dpool;
    int64_t          n;
    uint32_t         tasks;
    uint64_t*        bytes;     /* [tasks + 1]: pooled bytes per task, then their prefix */
    _Atomic(bool)    empty;     /* a zero-length cell: the output is marked HAS_NULLS */
} gbi_str_ctx_t;

static void gbi_str_count_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gbi_str_ctx_t* c = (gbi_str_ctx_t*)ctxp;
    bool empty = false;
    for (int64_t t = start; t < end; t++) {
        int64_t lo = c->n * t / c->tasks, hi = c->n * (t + 1) / c->tasks;
        uint64_t b = 0;
        for (int64_t i = lo; i < hi; i++) {
            uint32_t l = c->src[c->idx[i]].len;
            if (l > RAY_STR_INLINE_MAX) b += l;
            else if (l == 0) empty = true;
        }
        c->bytes[t] = b;
    }
    if (empty) atomic_store_explicit(&c->empty, true, memory_order_relaxed);
}

static void gbi_str_fill_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    const gbi_str_ctx_t* c = (const gbi_str_ctx_t*)ctxp;
    for (int64_t t = start; t < end; t++) {
        int64_t lo = c->n * t / c->tasks, hi = c->n * (t + 1) / c->tasks;
        uint64_t off = c->bytes[t];
        for (int64_t i = lo; i < hi; i++) {
            const ray_str_t* s = &c->src[c->idx[i]];
            ray_str_t* d = &c->dst[i];
            if (ray_str_is_inline(s)) { *d = *s; continue; }
            memcpy(c->dpool + off, c->spool + s->pool_off, s->len);
            *d = *s;                      /* len, prefix and the content hash */
            d->pool_off = (uint32_t)off;
            off += s->len;
        }
    }
}

static ray_t* gbi_str(ray_t* vec, const int64_t* idx, int64_t n) {
    ray_t* owner = (vec->attrs & RAY_ATTR_SLICE) ? vec->slice_parent : vec;
    int64_t base = (vec->attrs & RAY_ATTR_SLICE) ? vec->slice_offset : 0;
    gbi_str_ctx_t c;
    memset(&c, 0, sizeof c);
    c.src = (const ray_str_t*)ray_data(owner) + base;
    c.spool = owner->str_pool ? (const char*)ray_data(owner->str_pool) : NULL;
    c.idx = idx;
    c.n = n;
    atomic_store_explicit(&c.empty, false, memory_order_relaxed);
    ray_pool_t* gpool = ray_pool_get();
    bool par = n > 0 && gbi_par_ok(gpool, n);
    /* tasks of at least a morsel: the prefix sum below is over them */
    c.tasks = par ? ray_pool_total_workers(gpool) * 4 : 1;
    if (c.tasks > RAY_POOL_INIT_TASKS) c.tasks = RAY_POOL_INIT_TASKS;
    while (c.tasks > 1 && n / c.tasks < RAY_MORSEL_ELEMS) c.tasks /= 2;
    ray_t* bytes_hdr = ray_alloc(((size_t)c.tasks + 1) * sizeof(uint64_t));
    if (!bytes_hdr || RAY_IS_ERR(bytes_hdr)) return bytes_hdr ? bytes_hdr : ray_error("oom", NULL);
    c.bytes = (uint64_t*)ray_data(bytes_hdr);
    ray_t* result = ray_vec_new(RAY_STR, n);
    if (!result || RAY_IS_ERR(result)) { ray_free(bytes_hdr); return result ? result : ray_error("oom", NULL); }
    result->len = n;
    if (n > 0) {
        if (c.tasks > 1) ray_pool_dispatch_n(gpool, gbi_str_count_fn, &c, c.tasks);
        else gbi_str_count_fn(&c, 0, 0, 1);
        uint64_t total = 0;
        for (uint32_t t = 0; t < c.tasks; t++) { uint64_t b = c.bytes[t]; c.bytes[t] = total; total += b; }
        c.bytes[c.tasks] = total;
        if (total > UINT32_MAX) { ray_release(result); ray_free(bytes_hdr); return NULL; }
        if (total > 0) {
            ray_t* sp = ray_alloc((size_t)total);
            if (!sp || RAY_IS_ERR(sp)) { ray_release(result); ray_free(bytes_hdr); return ray_error("oom", NULL); }
            sp->type = RAY_U8;
            sp->len = (int64_t)total;
            result->str_pool = sp;
            c.dpool = (char*)ray_data(sp);
        }
        c.dst = (ray_str_t*)ray_data(result);
        if (c.tasks > 1) ray_pool_dispatch_n(gpool, gbi_str_fill_fn, &c, c.tasks);
        else gbi_str_fill_fn(&c, 0, 0, 1);
        /* the pool skips the tasks of a cancelled query: their cells are unwritten */
        if (c.tasks > 1 && (ray_interrupted() ||
                            atomic_load_explicit(&gpool->cancelled, memory_order_acquire))) {
            ray_release(result); ray_free(bytes_hdr);
            return ray_error("cancel", NULL);
        }
        if (atomic_load_explicit(&c.empty, memory_order_relaxed)) result->attrs |= RAY_ATTR_HAS_NULLS;
    }
    ray_free(bytes_hdr);
    return result;
}

/* Reorder vector elements by an index array */
ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n) {
    int8_t type = vec->type;

    /* Check nulls once — resolve through slices */
    bool has_nulls = ray_vec_may_have_nulls(vec);

    if (type == RAY_STR) {
        ray_t* result = gbi_str(vec, idx, n);
        if (result) return result;
        /* the pooled bytes pass UINT32_MAX: the per-cell path's error */
        result = ray_vec_new(type, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (has_nulls && ray_vec_is_null(vec, idx[i])) {
                result = ray_str_vec_set(result, i, "", 0);
                ray_vec_set_null(result, i, true);
            } else {
                size_t slen;
                const char* s = ray_str_vec_get(vec, idx[i], &slen);
                result = ray_str_vec_set(result, i, s ? s : "", s ? slen : 0);
            }
        }
        return result;
    }

    /* RAY_SYM: use adaptive width, create with matching width */
    if (type == RAY_SYM) {
        uint8_t w = vec->attrs & RAY_SYM_W_MASK;
        ray_t* result = ray_sym_vec_new(w, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t esz = (uint8_t)RAY_SYM_ELEM(w);
        gbi_ctx_t gc = { .src = (const char*)ray_data(vec),
                         .dst = (char*)ray_data(result),
                         .idx = idx, .esz = esz };
        ray_pool_t* gpool = ray_pool_get();
        bool par = gbi_par_ok(gpool, n);
        gbi_want(&gc, vec, idx, n, par);   /* scattered rows of a cold mapped column */
        if (par)
            ray_pool_dispatch(gpool, gbi_fill_fn, &gc, n);
        else
            gbi_fill_fn(&gc, 0, 0, n);
        if (has_nulls) {
            for (int64_t i = 0; i < n; i++)
                if (ray_vec_is_null(vec, idx[i]))
                    ray_vec_set_null(result, i, true);
        }
        /* Output-vec rule (Task-3 review): the gather raw-copies cell ids
         * from a single source vec, so the output must resolve over the
         * source's dictionary.  No-op while every domain is the runtime
         * singleton. */
        ray_sym_vec_adopt_domain(result, vec);
        return result;
    }

    /* LIST: pointer gather with retain */
    if (type == RAY_LIST) {
        ray_t* result = ray_alloc(n * sizeof(ray_t*));
        if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
        result->type = type;
        result->len = n;
        ray_t** src_ptrs = (ray_t**)ray_data(vec);
        ray_t** dst_ptrs = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < n; i++) {
            dst_ptrs[i] = src_ptrs[idx[i]];
            if (dst_ptrs[i]) ray_retain(dst_ptrs[i]);
        }
        return result;
    }

    ray_t* result = ray_vec_new(type, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t esz = ray_type_sizes[type];
    /* Typed gather — compiler constant esz enables vectorization, alias-safe;
     * pool-parallel over disjoint output ranges for large gathers. */
    gbi_ctx_t gc = { .src = (const char*)ray_data(vec),
                     .dst = (char*)ray_data(result),
                     .idx = idx, .esz = esz };
    ray_pool_t* gpool = ray_pool_get();
    bool par = gbi_par_ok(gpool, n);
    gbi_want(&gc, vec, idx, n, par);   /* scattered rows of a cold mapped column */
    if (par)
        ray_pool_dispatch(gpool, gbi_fill_fn, &gc, n);
    else
        gbi_fill_fn(&gc, 0, 0, n);

    /* Propagate null bitmap */
    if (has_nulls) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null(vec, idx[i]))
                ray_vec_set_null(result, i, true);
    }

    return result;
}

/* ══════════════════════════════════════════
 * Table construction and access
 * ══════════════════════════════════════════ */

/* (list v1 v2 ...) — package args into a list */
ray_t* ray_list_fn(ray_t** args, int64_t n) {
    ray_t* result = ray_alloc(n * sizeof(ray_t*));
    if (!result) return ray_error("oom", NULL);
    result->type = RAY_LIST;
    result->len = n;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        ray_retain(args[i]);
        out[i] = args[i];
    }
    return result;
}

static bool table_widens_atom(int8_t t) {
    return t <= -RAY_BOOL && t >= -RAY_F64;
}

/* (table [col_names] (list col1 col2 ...)) — build a RAY_TABLE */
/* Do the string atoms of a would-be string column, some held by several
 * cells, copy more than RAY_DUP_BUDGET bytes of text again (#1627)?  A
 * table's string column and a mapped result collapsed to a string vector
 * (atomic_collapse_wide) both ask. */
static bool str_cells_repeat(ray_t* const* cells, int64_t n) {
    return ray_str_cells_repeat_over(cells, n);
}

ray_t* ray_table_fn(ray_t* names, ray_t* cols) {
    ray_t *_bxn = NULL, *_bxc = NULL;
    names = unbox_vec_arg(names, &_bxn);
    if (RAY_IS_ERR(names)) return names;
    cols = unbox_vec_arg(cols, &_bxc);
    if (RAY_IS_ERR(cols)) { if (_bxn) ray_release(_bxn); return cols; }
    if (!is_list(names) || !is_list(cols)) { int8_t nt = names->type, ct = cols->type; if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", "table: names and columns must be lists, got %s and %s", ray_type_name(nt), ray_type_name(ct)); }
    int64_t ncols = ray_len(names);
    if (ray_len(cols) != ncols) { int64_t ncols_got = ray_len(cols); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", "table: column count must match name count %lld, got %lld", (long long)ncols, (long long)ncols_got); }

    ray_t** name_elems = (ray_t**)ray_data(names);
    ray_t** col_elems = (ray_t**)ray_data(cols);
    int64_t expected_rows = -1;

    ray_t* tbl = ray_table_new(ncols);
    if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }

    for (int64_t i = 0; i < ncols; i++) {
        if (name_elems[i]->type != -RAY_SYM)
            { int8_t nt = name_elems[i]->type; ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", "table: column name must be a symbol, got %s", ray_type_name(nt)); }
        int64_t name_id = name_elems[i]->i64;

        /* Convert Rayfall list (or typed vec) to typed column vector */
        ray_t* col_src = col_elems[i];

        /* Single atom → wrap in a 1-element vector.  A process-local
         * handle is wrapped in a 1-cell LIST instead: an I64 cell would
         * keep its pointer as a plain integer and drop what marks it. */
        ray_t* atom_wrap = NULL;
        if (ray_is_handle_atom(col_src)) {
            ray_t* one = ray_list_new(1);
            if (RAY_IS_ERR(one)) { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return one; }
            ray_t* prev = one;
            one = ray_list_append(one, col_src);
            if (RAY_IS_ERR(one)) { ray_release(prev); ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return one; }
            if (expected_rows < 0) expected_rows = 1;
            else if (expected_rows != 1)
                { ray_release(one); ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", "table: all columns must have %lld rows, got 1", (long long)expected_rows); }
            tbl = ray_table_add_col(tbl, name_id, one);
            ray_release(one);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }
        if (ray_is_atom(col_src) && col_src->type != -RAY_SYM) {
            int8_t atype = -col_src->type;
            if (atype == RAY_GUID) {
                atom_wrap = ray_vec_new(RAY_GUID, 1);
                if (!RAY_IS_ERR(atom_wrap) && col_src->obj)
                    memcpy(ray_data(atom_wrap), ray_data(col_src->obj), 16);
                if (!RAY_IS_ERR(atom_wrap)) atom_wrap->len = 1;
            } else if (atype == RAY_TIMESTAMP || atype == RAY_I64 || atype == RAY_SYM) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int64_t*)ray_data(atom_wrap))[0] = col_src->i64; atom_wrap->len = 1; }
            } else if (atype == RAY_F64) {
                atom_wrap = ray_vec_new(RAY_F64, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((double*)ray_data(atom_wrap))[0] = col_src->f64; atom_wrap->len = 1; }
            } else if (atype == RAY_DATE || atype == RAY_TIME || atype == RAY_I32) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int32_t*)ray_data(atom_wrap))[0] = col_src->i32; atom_wrap->len = 1; }
            } else if (atype == RAY_BOOL) {
                atom_wrap = ray_vec_new(RAY_BOOL, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((uint8_t*)ray_data(atom_wrap))[0] = col_src->b8; atom_wrap->len = 1; }
            }
            if (atom_wrap && !RAY_IS_ERR(atom_wrap)) {
                /* A lone typed-null atom wrapped into a 1-element column
                 * carries its sentinel; flag it.  SYM is no-null by design. */
                if (RAY_ATOM_IS_NULL(col_src) && atom_wrap->type != RAY_SYM)
                    atom_wrap->attrs |= RAY_ATTR_HAS_NULLS;
                col_src = atom_wrap;
            }
        }

        /* If the column is already a typed vector, use it directly */
        if (ray_is_vec(col_src)) {
            /* A wrapped atom is owned by this function: the table takes its
             * own reference below, and ours is dropped with the call. */
            bool wrapped = atom_wrap && !RAY_IS_ERR(atom_wrap);
            int64_t nrows = ray_len(col_src);
            if (expected_rows < 0) expected_rows = nrows;
            else if (nrows != expected_rows)
                { if (wrapped) ray_release(atom_wrap); ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", "table: all columns must have %lld rows, got %lld", (long long)expected_rows, (long long)nrows); }
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (wrapped) ray_release(atom_wrap);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        if (!is_list(col_src))
            { int8_t ct = col_src->type; ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", "table: column must be a vector or list, got %s", ray_type_name(ct)); }
        int64_t nrows = ray_len(col_src);

        /* Validate all columns have consistent row count */
        if (expected_rows < 0) expected_rows = nrows;
        else if (nrows != expected_rows)
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", "table: all columns must have %lld rows, got %lld", (long long)expected_rows, (long long)nrows); }

        /* Empty generic list → typeless empty column: keep it as a RAY_LIST
         * so its storage type is adopted from the first inserted value,
         * rather than defaulting to I64. */
        if (nrows == 0) {
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        ray_t** row_elems = (ray_t**)ray_data(col_src);

        /* If the LIST contains non-atom values (e.g. nested vectors for an
         * embedding column), store the LIST as the column directly rather
         * than trying to build a typed vector from non-atomic elements. */
        if (nrows > 0 && row_elems[0] && !ray_is_atom(row_elems[0])) {
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        int8_t first = row_elems[0]->type;
        bool numeric = table_widens_atom(first);
        int8_t col_type = -first;
        if (numeric) {
            for (int64_t j = 1; j < nrows; j++) {
                int8_t t = row_elems[j]->type;
                if (table_widens_atom(t)) col_type = ray_num_type(col_type, -t);
            }
        } else if (col_type != RAY_BOOL && col_type != RAY_SYM && col_type != RAY_STR &&
                   col_type != RAY_GUID && col_type != RAY_TIMESTAMP && col_type != RAY_DATE &&
                   col_type != RAY_TIME && col_type != RAY_TIMESPAN) {
            col_type = RAY_I64; /* unsupported first atom: report element mismatch below */
        }

        /* A string column copies each cell's text: a string atom held by
         * many cells is written out again for each (#1627).  Up to 1 GiB of
         * such copies, as for every writer that writes a value out in full. */
        if (col_type == RAY_STR && str_cells_repeat(row_elems, nrows))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc);
              return ray_error("limit", "table: a string column repeats shared strings beyond 1 GiB of extra data"); }

        ray_t* col_vec = ray_vec_new(col_type, nrows);
        if (RAY_IS_ERR(col_vec))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }

        if (numeric) col_vec->len = nrows; /* fixed-width cells are written in place */

        /* ray_vec_append copies the atom payload raw and never sets HAS_NULLS,
         * so track typed null atoms and flip the column flag after the loop. */
        bool col_has_nulls = false;
        /* A process-local handle met while filling: the column stays the
         * source LIST, since a typed I64 cell would turn the handle into
         * its pointer as a plain integer (#802). */
        bool has_handle = false;

        for (int64_t j = 0; j < nrows; j++) {
            /* Promotion preserves logical nulls, not the source sentinel's
             * numeric value. Publish the column flag after filling it. */
            bool cell_is_null = RAY_ATOM_IS_NULL(row_elems[j]);
            if (cell_is_null && col_type != RAY_BOOL && col_type != RAY_U8) col_has_nulls = true;
            if (col_type == RAY_STR) {
                if (row_elems[j]->type != -RAY_STR) {
                    int8_t et = row_elems[j]->type;
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", "table: string column element must be a string, got %s", ray_type_name(et));
                }
                const char *sptr = ray_str_ptr(row_elems[j]);
                size_t slen = ray_str_len(row_elems[j]);
                col_vec = ray_str_vec_append(col_vec, sptr, slen);
            } else if (col_type == RAY_GUID) {
                if (row_elems[j]->type != -RAY_GUID || !row_elems[j]->obj) {
                    int8_t et = row_elems[j]->type;
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", "table: guid column element must be a guid, got %s", ray_type_name(et));
                }
                col_vec = ray_vec_append(col_vec, ray_data(row_elems[j]->obj));
            } else {
                ray_t* cell = row_elems[j];
                if (numeric ? !table_widens_atom(cell->type) : cell->type != -col_type) {
                    int8_t et = row_elems[j]->type;
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", "table: %s column element type mismatch, got %s", ray_type_name(col_type), ray_type_name(et));
                }
                if (numeric) {
                    if (RAY_UNLIKELY(cell->attrs & RAY_HANDLE_ATTRS) && cell->type == -RAY_I64)
                        { has_handle = true; break; }
                    if (cell_is_null && (col_type == RAY_BOOL || col_type == RAY_U8))
                        ((uint8_t*)ray_data(col_vec))[j] = 0;
                    else if (cell_is_null) ray_vec_set_null(col_vec, j, true);
                    else if (col_type == RAY_F64) ((double*)ray_data(col_vec))[j] = (cell->type == -RAY_F64 ? cell->f64 : as_f64(cell));
                    else if (cell->type == -RAY_BOOL && col_type != RAY_BOOL) {
                        ray_t n = { .type = -RAY_I64, .i64 = cell->b8 ? 1 : 0 };
                        store_typed_elem(col_vec, j, &n);
                    } else store_typed_elem(col_vec, j, cell);
                } else {
                    const void* payload = col_type == RAY_BOOL ? (void*)&cell->b8 : (void*)&cell->i64;
                    col_vec = ray_vec_append(col_vec, payload);
                }
            }
            if (RAY_IS_ERR(col_vec))
                { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }
        }

        if (RAY_UNLIKELY(has_handle)) {
            ray_release(col_vec);
            col_vec = col_src;
            ray_retain(col_vec);
        } else if (col_has_nulls && col_vec->type != RAY_SYM)
            col_vec->attrs |= RAY_ATTR_HAS_NULLS;

        tbl = ray_table_add_col(tbl, name_id, col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
    }

    if (_bxn) ray_release(_bxn);
    if (_bxc) ray_release(_bxc);
    return tbl;
}

/* (key dict/table) — return keys vector */
ray_t* ray_key_fn(ray_t* x) {
    if (x->type == RAY_DICT) {
        ray_t* keys = ray_dict_keys(x);
        if (!keys) return ray_error("type", NULL);
        ray_retain(keys);
        return keys;
    }
    if (x->type != RAY_TABLE) return ray_error("type", "key: expected a dict or table, got %s", ray_type_name(x->type));
    int64_t ncols = ray_table_ncols(x);
    ray_t* vec = ray_vec_new(RAY_SYM, ncols);
    if (RAY_IS_ERR(vec)) return vec;
    vec->len = ncols;
    int64_t* out = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < ncols; i++)
        out[i] = ray_table_col_name(x, i);
    return vec;
}

/* (value dict/table) — extract values */
ray_t* ray_value_fn(ray_t* x) {
    /* Table: return list of column vectors */
    if (x->type == RAY_TABLE) {
        /* Table cols slot is a RAY_LIST already — return a fresh copy. */
        int64_t ncols = ray_table_ncols(x);
        ray_t* result = ray_list_new(ncols);
        if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
        for (int64_t i = 0; i < ncols; i++) {
            ray_t* c = ray_table_col_idx(x, i);
            if (c && RAY_IS_ERR(c)) { ray_release(result); return c; }
            result = ray_list_append(result, c);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    if (x->type != RAY_DICT) return ray_error("type", "value: expected a dict or table, got %s", ray_type_name(x->type));
    ray_t* vals = ray_dict_vals(x);
    if (!vals) return ray_error("type", NULL);
    ray_retain(vals);
    return vals;
}



/* ray_lang_print, fmt_interpolate, ray_println_fn, ray_show_fn, ray_format_fn,
 * ray_resolve_fn, ray_timeit_fn, ray_exit_fn, resolve_type_name,
 * ray_read_csv_fn, ray_write_csv_fn, cast_match, ray_cast_fn, ray_type_fn,
 * ray_read_file_fn, ray_load_file_fn, ray_write_file_fn
 * moved to ops/builtins.c */

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
ray_t* ray_set_fn(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", "set: name must be a symbol, got %s", ray_type_name(name_obj->type));
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set(name_obj->i64, val);
    if (err != RAY_OK) {
        ray_release(val);
        return ray_error(ray_err_code_str(err), NULL);
    }
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
ray_t* ray_let_fn(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", "let: name must be a symbol, got %s", ray_type_name(name_obj->type));
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    uint8_t rows = vrows_from(g_eval_rows);   /* the local keeps its value's provenance */
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set_local_rows(name_obj->i64, val, rows);
    if (err != RAY_OK) {
        ray_release(val);
        return err == RAY_ERR_ACCESS ? ray_error("access", "restricted")
                                     : ray_error(ray_err_code_str(err), NULL);
    }
    return val;
}

/* A `while` condition inside a query that is a vector as long as the rows
 * being evaluated (the group's under a per-group evaluation, the active
 * table's otherwise; ray_query_rows_len) (#1630, #1647).  This is the
 * condition of a compiled `while`'s jump (vm_cond_test; a `times` count is
 * normalized to an atom first, and `if` chooses per row for a boolean
 * vector, ray_cond_rows) or of the tree walker's (ray_while_fn).  Tested
 * whole, any non-empty vector is true, so a loop over the rows ran on the
 * first row's say, and one false on every row never ended: a server could
 * be held by one query.
 *
 * Every such vector is tested, whatever computed it.  Row provenance is not
 * consulted: a value computed from the rows is reported as such only where
 * each route that builds one says so (an `if`'s result took its arms'
 * provenance, `and`'s its last operand's), and one route that does not
 * would loop forever.  A BOOL vector true on every row is true and one
 * false on every row false (no rows: false, there being no row to loop
 * for); one true on some rows and false on others, or a vector of another
 * type, is an error, since a loop cannot take a different number of passes
 * for each row.  *truth is set to 1 or 0; a vector of another length, or
 * an atom, leaves it at -1, tested whole as outside a query.  NULL unless
 * an error. */
static __attribute__((noinline)) ray_t* cond_rows_test(ray_t* cond, int* truth) {
    *truth = -1;
    if (!(ray_is_vec(cond) || cond->type == RAY_LIST)) return NULL;
    int64_t len = ray_query_rows_len();
    if (len < 0 || len != ray_len(cond)) return NULL;
    if (len == 0) { *truth = 0; return NULL; }
    if (cond->type != RAY_BOOL)
        return ray_error("nyi", "while: its condition is a vector of %s over the query's rows, "
                         "and a loop tests it whole, not row by row; "
                         "see Iteration: while in language/control-flow.md",
                         ray_type_name(cond->type));
    const uint8_t* b = (const uint8_t*)ray_data(cond);
    bool first = b[0] != 0;
    /* Eight cells a word: a BOOL cell is 0 or 1, so a word of cells all
     * like the first is 0 or 0x0101...; any other word is read a cell at a
     * time, which also accepts a true cell stored as another non-zero. */
    const uint64_t want = first ? 0x0101010101010101ULL : 0;
    bool mixed = false;
    int64_t k = 0;
    for (; k + 8 <= len && !mixed; k += 8) {
        uint64_t w;
        memcpy(&w, b + k, sizeof w);
        if (w == want) continue;
        for (int j = 0; j < 8; j++) mixed |= (b[k + j] != 0) != first;
    }
    for (; k < len && !mixed; k++) mixed = (b[k] != 0) != first;
    if (mixed)
        return ray_error("nyi", "while: its condition differs between the query's rows, "
                         "and a loop tests it whole, not row by row; "
                         "see Iteration: while in language/control-flow.md");
    *truth = first ? 1 : 0;
    return NULL;
}

/* The bytecode jump's test of a vector condition (op_jmpf, the jumps other than
 * `if`'s, which takes OP_JMPFV): counted toward rows_cond, and inside a query a
 * condition over its rows goes through cond_rows_test.  Consumes `cond`; an
 * error goes to *err. */
static __attribute__((noinline)) int vm_cond_test(ray_t* cond, ray_t** err) {
    __VM->rows_cond++;   /* a vector tested whole */
    int truth = -1;
    ray_t* mixed = cond_rows_test(cond, &truth);
    if (mixed) { ray_release(cond); *err = mixed; return 0; }
    int t = truth >= 0 ? truth : is_truthy(cond);
    ray_release(cond);
    return t;
}

/* (if c1 r1 c2 r2 ... else?) — conditional chain.  Receives unevaluated
 * args.  Conditions are tried in order and the result paired with the
 * first truthy one is evaluated; an odd trailing arg is the fallback.  No
 * fallback: return 0. */
/* Elementwise choice for a boolean-vector condition: `mask` picks per row
 * between `then_v` and `else_v`, both already evaluated and materialized,
 * none consumed.  The arms are vectors of the mask's length or atoms that
 * broadcast.  The result type follows the promotion `if` has inside a
 * query, because the choice runs through the same OP_IF kernel. */
ray_t* ray_cond_rows(ray_t* mask, ray_t* then_v, ray_t* else_v) {
    int64_t len = mask->len;
    ray_t* arms[2] = { then_v, else_v };
    for (int k = 0; k < 2; k++) {
        ray_t* a = arms[k];
        if (ray_is_atom(a)) continue;
        if (!ray_is_vec(a))
            return ray_error("type", "if: an arm of a vector condition must be a vector or an atom, got %s",
                             ray_type_name(a->type));
        if (a->len != len)
            return ray_error("length", "if: condition has %lld rows, %s arm has %lld",
                             (long long)len, k ? "else" : "then", (long long)a->len);
    }
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* ops[3] = { ray_const_vec(g, mask), NULL, NULL };
    for (int k = 0; k < 2 && ops[0]; k++)
        ops[k + 1] = ray_is_atom(arms[k]) ? ray_const_atom(g, arms[k])
                                          : ray_const_vec(g, arms[k]);
    ray_op_t* root = (ops[0] && ops[1] && ops[2]) ? ray_if(g, ops[0], ops[1], ops[2]) : NULL;
    if (!root) {
        ray_t* e = g->compile_err;
        g->compile_err = NULL;
        ray_graph_free(g);
        return e ? e : ray_error("type", "if: cannot choose between %s and %s rows",
                                 ray_type_name(then_v->type), ray_type_name(else_v->type));
    }
    ray_t* r = ray_execute(g, root);
    ray_graph_free(g);
    return r ? r : ray_error("oom", NULL);
}

ray_t* ray_cond_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", "if: expected at least 2 args (cond then), got %lld", (long long)n);
    int64_t i = 0;
    for (; i + 1 < n; i += 2) {
        ray_t* cond = ray_eval(args[i]);
        if (RAY_IS_ERR(cond)) return cond;
        /* Materialize lazy handles before testing truthiness */
        if (ray_is_lazy(cond))
            cond = ray_lazy_materialize(cond);
        if (RAY_IS_ERR(cond)) return cond;
        if (cond->type == RAY_BOOL) {
            /* A boolean vector chooses per row, so both the then arm and
             * the rest of the chain are evaluated, then merged.  It still
             * counts as a vector condition for the opaque by: key, whose
             * row-by-row route is unchanged. */
            __VM->rows_cond++;
            if (i + 2 >= n) {
                ray_release(cond);
                return ray_error("domain", "if: a boolean-vector condition needs an else arm");
            }
            ray_t* tv = ray_eval(args[i + 1]);
            if (tv && !RAY_IS_ERR(tv) && ray_is_lazy(tv)) tv = ray_lazy_materialize(tv);
            if (!tv || RAY_IS_ERR(tv)) { ray_release(cond); return tv ? tv : ray_error("oom", NULL); }
            ray_t* ev = (n - i - 2 == 1) ? ray_eval(args[i + 2])
                                            : ray_cond_fn(args + i + 2, n - i - 2);
            if (ev && !RAY_IS_ERR(ev) && ray_is_lazy(ev)) ev = ray_lazy_materialize(ev);
            ray_t* out = (!ev || RAY_IS_ERR(ev)) ? (ev ? ev : ray_error("oom", NULL))
                                                 : ray_cond_rows(cond, tv, ev);
            ray_release(cond);
            ray_release(tv);
            if (ev && !RAY_IS_ERR(ev)) ray_release(ev);
            return out;
        }
        /* A non-boolean vector condition is still tested whole: the
         * expression may have needed row-by-row evaluation (see the
         * opaque by: key). */
        if (!ray_is_atom(cond)) __VM->rows_cond++;
        int truthy = is_truthy(cond);
        ray_release(cond);
        if (truthy) return ray_eval(args[i + 1]);
    }
    if (i < n) return ray_eval(args[i]);
    /* No else branch: return 0 */
    return make_i64(0);
}

/* (do expr1 expr2 ...) — evaluate in sequence, return last. Pushes local scope. */
ray_t* ray_do_fn(ray_t** args, int64_t n) {
    if (n == 0) return make_i64(0);
    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
    ray_t* result = NULL;
    for (int64_t i = 0; i < n; i++) {
        if (result) {
            ray_release(result);
        }
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) {
            ray_env_pop_scope();
            return result;
        }
    }
    ray_env_pop_scope();
    return result;
}

/* (while cond body...) — iterate while cond is truthy.  Receives
 * unevaluated args.  Always returns null: it is a statement form run for
 * effect, and a never-taken loop has no last value to report.
 *
 * Zero body expressions is legal — a condition with side effects is then
 * the whole loop, which is the shape a "repeat until done" drain wants
 * (issue 588): there is no sequence to iterate, so folding over a range
 * was only ever scaffolding.
 *
 * Unlike ray_do_fn this pushes NO scope around the body.  `let` binds in
 * the top frame only (env_bind_local), so a per-iteration frame would
 * discard loop-carried `let` state here while the compiled path — whose
 * `let` writes a function-level bytecode slot — kept it, and the two
 * evaluators would disagree on the same source.  A caller wanting a fresh
 * frame per pass writes (while cond (do ...)), which composes.
 *
 * No interrupt check is needed: the condition goes through ray_eval on
 * every pass, whose entry guard raises `cancel` when a Ctrl-C has landed.
 * The compiled form is emitted by the bytecode compiler — see compile.c. */
ray_t* ray_while_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "while: expected at least 1 arg (cond), got %lld", (long long)n);
    for (;;) {
        ray_t* cond = ray_eval(args[0]);
        if (RAY_IS_ERR(cond)) return cond;
        /* Materialize lazy handles before testing truthiness — the
         * truthiness belongs to the value, not to the non-NULL handle
         * that happens to contain it (same rule as ray_cond_fn). */
        if (ray_is_lazy(cond))
            cond = ray_lazy_materialize(cond);
        if (RAY_IS_ERR(cond)) return cond;
        /* Inside a query a condition over its rows is tested as the
         * compiled `while`'s is (cond_rows_test): tested whole, a
         * non-empty vector false on every row stayed true and the loop
         * never ended (#1647). */
        int truth = -1;
        if (!ray_is_atom(cond)) {
            ray_t* mixed = cond_rows_test(cond, &truth);
            if (mixed) { ray_release(cond); return mixed; }
        }
        int truthy = truth >= 0 ? truth : is_truthy(cond);
        ray_release(cond);
        if (!truthy) return RAY_NULL_OBJ;
        for (int64_t i = 1; i < n; i++) {
            ray_t* val = ray_eval(args[i]);
            if (RAY_IS_ERR(val)) return val;
            ray_release(val);
        }
    }
}

/* Read a loop count from an evaluated atom.  Integers only: floats are
 * rejected rather than truncated, because as_i64 on an F64 reads the bit
 * pattern, and a silent (times 1.5 ...) meaning "once" hides a mistake
 * either way.  Returns 0 on success, or fills *err. */
static int loop_count(ray_t* x, int64_t* out, const char* who, ray_t** err) {
    if (ray_is_atom(x)) {
        switch (x->type) {
        case -RAY_I64: case -RAY_I32: case -RAY_I16: case -RAY_U8:
            *out = as_i64(x);
            return 0;
        default: break;
        }
    }
    *err = ray_error("type", "%s: count must be an integer, got %s", who, ray_type_name(x->type));
    return -1;
}

/* The compiled `times` loop drives its counter through these two, held
 * as constant-pool objects rather than bound in the env — so they carry
 * no name a user could write, and the loop's arithmetic cannot be
 * changed out from under it by a `(set - ...)` style override.
 *
 * Normalizing once on entry (type-check, and clamp a negative bound to
 * zero) is what lets the loop test the raw counter for truthiness: 0 is
 * falsy, every other count is truthy, so no comparison call is needed
 * per pass and a negative bound cannot run away. */
ray_t* ray_times_norm_fn(ray_t* x) {
    int64_t reps = 0;
    ray_t* err = NULL;
    if (loop_count(x, &reps, "times", &err)) return err;
    return make_i64(reps < 0 ? 0 : reps);
}

ray_t* ray_times_dec_fn(ray_t* x) {
    return make_i64(as_i64(x) - 1);
}

/* The index of the pass about to run, from the normalized bound and the
 * passes still to go (counter > 0 when this is called). */
ray_t* ray_times_idx_fn(ray_t* lim, ray_t* ctr) {
    return make_i64(as_i64(lim) - as_i64(ctr));
}

/* If `ast` is the binding of an indexed `times`, as the parser builds it
 * from [name count], fill the name's sym and the count expression. */
bool ray_times_binding(ray_t* ast, int64_t* name, ray_t** count) {
    if (!ast || ast->type != RAY_LIST || ast->len != 3) return false;
    ray_t** e = (ray_t**)ray_data(ast);
    if (!e[0] || e[0]->type != -RAY_SYM || (e[0]->attrs & ATTR_QUOTED) ||
        e[0]->i64 != ray_sym_intern(RAY_TIMES_BIND, sizeof(RAY_TIMES_BIND) - 1))
        return false;
    if (!e[1] || e[1]->type != -RAY_SYM) return false;
    *name = e[1]->i64;
    *count = e[2];
    return true;
}

/* (times n body...) — evaluate the body exactly n times.  Receives
 * unevaluated args.  Always returns null, like `while`.
 *
 * (times [i n] body...) also binds i to 0, 1, ..., n-1, one per pass.
 * The name is bound like a `let`, in the current frame, and stays bound
 * after the loop to the last index (untouched when no pass runs).  The
 * index is assigned at the top of each pass from the loop's own counter,
 * so a body that rebinds i changes it for the rest of that pass only.
 *
 * `n` is evaluated ONCE, up front: the count is a bound fixed on entry,
 * so a body that mutates whatever produced it cannot change how many
 * passes remain.  A count of zero or less runs the body zero times
 * rather than trapping — a bound computed as empty is a no-op, not an
 * error.
 *
 * Pushes no scope, for the reason given on ray_while_fn: `let` binds in
 * the top frame only, so a per-pass frame would discard loop-carried
 * state here while the compiled path kept it.
 *
 * The compiled form is emitted by the bytecode compiler — see compile.c. */
ray_t* ray_times_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "times: expected at least 1 arg (count), got %lld", (long long)n);
    int64_t name = -1;
    ray_t* cexpr = args[0];
    if (ray_times_binding(args[0], &name, &cexpr) &&
        ray_sym_is_reserved(name) && !ray_sym_is_ipc_hook(name))
        return ray_error(ray_err_code_str(RAY_ERR_RESERVED), NULL);
    ray_t* cnt = ray_eval(cexpr);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (ray_is_lazy(cnt))
        cnt = ray_lazy_materialize(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    int64_t reps = 0;
    ray_t* err = NULL;
    int bad = loop_count(cnt, &reps, "times", &err);
    ray_release(cnt);
    if (bad) return err;
    for (int64_t r = 0; r < reps; r++) {
        if (name >= 0) {
            ray_t* idx = make_i64(r);
            if (RAY_IS_ERR(idx)) return idx;
            ray_err_t e = ray_env_set_local_rows(name, idx, 0);
            ray_release(idx);
            if (e == RAY_ERR_ACCESS) return ray_error("access", "restricted");
            if (e != RAY_OK) return ray_error(ray_err_code_str(e), NULL);
        }
        for (int64_t i = 1; i < n; i++) {
            ray_t* val = ray_eval(args[i]);
            if (RAY_IS_ERR(val)) return val;
            ray_release(val);
        }
    }
    return RAY_NULL_OBJ;
}

/* ══════════════════════════════════════════
 * Lambda functions
 * ══════════════════════════════════════════ */

/* (fn [params...] body...) — create a lambda object.
 * Stores params list and body expressions in data area. */
/* Sym ID of "self" for call_lambda's frame.  It belongs to the current sym
 * table: runtime destroy tears the table down and the next runtime can give
 * "self" a different ID, so ray_lang_destroy resets it, as ray_compile_reset
 * does for the compiler's special-form IDs.  A stale ID bound the lambda under
 * a name that was no longer "self", shadowing whatever variable owned it. */
static int64_t g_call_self_sym = -1;

/* Closures made by interpreted code (the top level, a lambda that could not
 * compile, a literal inside a special form the compiler hands to the tree
 * walker) share one compiled template per literal and capture set, as
 * closures made by compiled code share theirs (OP_MKCLOSURE): the body's
 * bytecode depends only on which captured names it mentions.  The entry
 * retains the literal's parameter and body nodes, so their addresses stay
 * its own while it lives; a literal is matched by them and by its capture
 * names.  A lambda that is query text (LAMBDA_CAPQ) stays interpreted and
 * never comes here. */
enum { CLOSURE_TMPL_SLOTS = 256, CLOSURE_TMPL_MAX_BODY = 8 };
typedef struct {
    ray_t*  params;    /* the literal's parameter node */
    ray_t*  body[CLOSURE_TMPL_MAX_BODY];   /* every body node: a literal built
                       * for eval can share first and last nodes with another
                       * and differ in the middle */
    int64_t nbody;
    ray_t*  mentioned; /* the symbols the body mentions (I64 vector) */
    bool    dynamic;   /* the body uses eval or resolve, or a mutator with a
                        * computed target: every capture counts */
    bool    failed;    /* the body did not compile: its closures stay interpreted */
    ray_t*  names;     /* capture names (SYM vector) or NULL */
    ray_t*  tmpl;      /* the compiled template, or NULL when `failed` */
    int64_t bytes;     /* what the literal's text holds (ray_closure_mentioned_syms) */
    ray_t*  deps;      /* a definition's entry (nothing captured, builtins
                        * baked on its first call): the call heads its code
                        * depends on (ray_compile_definition); NULL for a
                        * closure's */
} closure_tmpl_t;
/* The entries keep their literals' text alive after every closure made
 * from them is gone, so the table is bounded by bytes: a literal holding
 * more than CLOSURE_TMPL_MAX_BYTES is not kept, and older entries make
 * room once the table holds CLOSURE_TMPL_BUDGET. */
enum { CLOSURE_TMPL_MAX_BYTES = RAY_CLOSURE_TMPL_MAX_BYTES, CLOSURE_TMPL_BUDGET = 1024 * 1024 };
static _Thread_local closure_tmpl_t g_closure_tmpl[CLOSURE_TMPL_SLOTS];
static _Thread_local uint32_t g_closure_tmpl_next;
static _Thread_local uint64_t g_closure_tmpl_epoch;
static _Thread_local int64_t g_closure_tmpl_bytes;   /* sum of the entries' bytes */
static _Thread_local uint32_t g_closure_tmpl_made;   /* entries made since the last
                                                      * request-end sweep */

static void closure_tmpl_drop(closure_tmpl_t* e) {
    g_closure_tmpl_bytes -= e->bytes;
    if (e->params) ray_release(e->params);
    for (int64_t i = 0; i < e->nbody && i < CLOSURE_TMPL_MAX_BODY; i++) if (e->body[i]) ray_release(e->body[i]);
    if (e->mentioned) ray_release(e->mentioned);
    if (e->names) ray_release(e->names);
    if (e->tmpl) ray_release(e->tmpl);
    if (e->deps) ray_release(e->deps);
    memset(e, 0, sizeof *e);
}

/* The runtime is going away with everything the entries retain: release
 * them (called by the thread destroying it, before the heap goes). */
static void closure_tmpl_reset(void) {
    for (int i = 0; i < CLOSURE_TMPL_SLOTS; i++) closure_tmpl_drop(&g_closure_tmpl[i]);
    g_closure_tmpl_next = 0;
    g_closure_tmpl_epoch = 0;
    g_closure_tmpl_bytes = 0;
    g_closure_tmpl_made = 0;
}

/* Another thread's table after the runtime it filled it under was
 * destroyed and a new one created: its objects are gone with that
 * runtime's heap, so the entries are forgotten, not released. */
static void closure_tmpl_forget(void) {
    memset(g_closure_tmpl, 0, sizeof g_closure_tmpl);
    g_closure_tmpl_next = 0;
    g_closure_tmpl_bytes = 0;
    g_closure_tmpl_made = 0;
}

/* An entry is dead once the table alone holds its literal's parameter
 * node (the entry and its template each hold one): the text the literal
 * was read from is gone, a finished ray_eval_str request or a function
 * since rebound, so the literal cannot be evaluated again and the entry
 * cannot be hit.  It would still pin its template, and through it the
 * nfo of that text: a request's whole source and a span record per node,
 * which the entry's bytes do not count.  Dead entries go before a new
 * entry is made and when a top-level request ends (ray_eval_request_end),
 * so they never pile up behind live ones. */
static void closure_tmpl_sweep(void) {
    if (g_closure_tmpl_epoch != ray_sym_epoch()) return;   /* not this runtime's */
    for (int i = 0; i < CLOSURE_TMPL_SLOTS; i++) {
        closure_tmpl_t* e = &g_closure_tmpl[i];
        if (!e->params) continue;
        uint32_t held = 1 + (e->tmpl && LAMBDA_PARAMS(e->tmpl) == e->params);
        if (ray_atomic_load(&e->params->rc) <= held) closure_tmpl_drop(e);
    }
}

/* A top-level request (a ray_eval_str string, a REPL line, a script file,
 * an IPC message) has ended and released its text: the entries its
 * literals made are dead unless a function that outlives it holds them.
 * A request that made no entry leaves nothing to sweep, so it pays for
 * no scan of the table.  An entry another request's rebinding left dead
 * goes at the next sweep, which the next new entry makes in any case.
 * Inside an evaluation (a nested load or string eval) the enclosing
 * request still holds its text, and its own end sweeps. */
void ray_eval_request_end(void) {
    if (!__VM || __VM->eval_depth > 0 || !g_closure_tmpl_made) return;
    g_closure_tmpl_made = 0;
    closure_tmpl_sweep();
}

/* Is any of a literal's body nodes recorded in `nfo`? */
static bool nfo_records_literal(ray_t* nfo, ray_t* body) {
    if (!nfo) return false;
    ray_t** bn = (ray_t**)ray_data(body);
    for (int64_t k = 0; k < body->len; k++)
        if (ray_nfo_get(nfo, bn[k]).id) return true;
    return false;
}

/* The nfo a literal's nodes were recorded in, for an entry that outlives
 * the request evaluating it: that request's when it parsed them (the
 * lambda's own nfo), else the function the tree walker is running the
 * literal in (bound as `self`), else none.  A long-lived function's inner
 * literal first evaluated by a large request would otherwise pin that
 * request's source and span records for as long as the entry lives,
 * though they record none of its nodes.  Looked up once per new entry,
 * next to a compile that looks up every node. */
static ray_t* literal_nfo(ray_t* lambda) {
    ray_t* body = LAMBDA_BODY(lambda);
    ray_t* own = LAMBDA_NFO(lambda);
    if (nfo_records_literal(own, body)) return own;
    if (g_call_self_sym < 0) g_call_self_sym = ray_sym_intern("self", 4);
    ray_t* self = ray_env_get_local(g_call_self_sym);
    if (self && self != lambda && self->type == RAY_LAMBDA && LAMBDA_NFO(self) != own &&
        nfo_records_literal(LAMBDA_NFO(self), body))
        return LAMBDA_NFO(self);
    return NULL;
}

/* Give `lambda` the nfo `nfo` (NULL for none) in place of its own. */
static void lambda_set_nfo(ray_t* lambda, ray_t* nfo) {
    if (LAMBDA_NFO(lambda) == nfo) return;
    if (nfo) ray_retain(nfo);
    if (LAMBDA_NFO(lambda)) ray_release(LAMBDA_NFO(lambda));
    LAMBDA_NFO(lambda) = nfo;
}

/* Does `closure`'s capture set, filtered by the entry's mentioned symbols,
 * spell the entry's names?  Walks the dict keys once, allocating nothing. */
static bool closure_entry_names_match(const closure_tmpl_t* e, ray_t* closure) {
    int64_t want = e->names ? e->names->len : 0;
    if (!closure) return want == 0;
    ray_t* keys = ray_dict_keys(closure);
    if (!keys) return want == 0;
    const int64_t* ment = e->mentioned ? (const int64_t*)ray_data(e->mentioned) : NULL;
    int64_t nment = e->mentioned ? e->mentioned->len : 0;
    int64_t at = 0;
    for (int64_t i = 0; i < keys->len; i++) {
        int64_t id = ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs);
        if (id == g_call_self_sym) continue;
        bool counts = e->dynamic;
        for (int64_t k = 0; !counts && k < nment; k++) counts = ment[k] == id;
        if (!counts) continue;
        if (at >= want || ray_read_sym(ray_data(e->names), at, RAY_SYM, e->names->attrs) != id) return false;
        at++;
    }
    return at == want;
}

static void closure_share_from(ray_t* lambda, ray_t* tmpl) {
    if (LAMBDA_BC(tmpl))     { ray_retain(LAMBDA_BC(tmpl));     LAMBDA_BC(lambda)     = LAMBDA_BC(tmpl); }
    if (LAMBDA_CONSTS(tmpl)) { ray_retain(LAMBDA_CONSTS(tmpl)); LAMBDA_CONSTS(lambda) = LAMBDA_CONSTS(tmpl); }
    if (LAMBDA_DBG(tmpl))    { ray_retain(LAMBDA_DBG(tmpl));    LAMBDA_DBG(lambda)    = LAMBDA_DBG(tmpl); }
    /* The spans in the shared debug table are the template's nfo's: an
     * error in this lambda reports them against that text, and the lambda
     * does not hold the text of whichever request evaluated it. */
    lambda_set_nfo(lambda, LAMBDA_NFO(tmpl));
    LAMBDA_NLOCALS(lambda) = LAMBDA_NLOCALS(tmpl);
    lambda->attrs |= RAY_FN_COMPILED;
}

static void closure_share_template(ray_t* lambda) {
    ray_t* body = LAMBDA_BODY(lambda);
    ray_t* params = LAMBDA_PARAMS(lambda);
    /* A lambda with nothing captured is a definition: it compiles on its
     * first call and bakes the builtins it names then, so a function
     * rebound between its evaluation and that call, or a dict it reaches
     * a function through, is the one it calls.  A shared template would
     * have baked them when the literal was evaluated. */
    if (!LAMBDA_CLOSURE(lambda)) return;
    if (!body || body->type != RAY_LIST || body->len == 0 || !params) return;
    if (body->len > CLOSURE_TMPL_MAX_BODY) return;   /* compiles when called */
    if (g_closure_tmpl_epoch != ray_sym_epoch()) {
        if (g_closure_tmpl_epoch) closure_tmpl_forget();
        g_closure_tmpl_epoch = ray_sym_epoch();
    }
    ray_t** bn = (ray_t**)ray_data(body);
    /* The literal's nodes first: a known literal's mentioned symbols are
     * in its entries, so the capture names cost a pass over the dict keys
     * rather than a walk of the body. */
    if (g_call_self_sym < 0) g_call_self_sym = ray_sym_intern("self", 4);
    for (int i = 0; i < CLOSURE_TMPL_SLOTS; i++) {
        closure_tmpl_t* e = &g_closure_tmpl[i];
        if ((!e->tmpl && !e->failed) || e->deps || e->params != params || e->nbody != body->len) continue;
        bool same = true;
        for (int64_t k = 0; k < body->len && same; k++) same = e->body[k] == bn[k];
        if (same && closure_entry_names_match(e, LAMBDA_CLOSURE(lambda))) {
            if (e->failed) LAMBDA_NLOCALS(lambda) = -1;   /* known not to compile */
            else closure_share_from(lambda, e->tmpl);
            return;
        }
    }
    /* A new literal, or a known one with another capture set. */
    bool dynamic = false;
    int64_t weight = 0;
    ray_t* mentioned = ray_closure_mentioned_syms(body, &dynamic, &weight);
    if (!mentioned) return;
    if (weight > CLOSURE_TMPL_MAX_BYTES) {
        /* A literal built around a large constant is not kept: the table
         * would hold the constant alive after its closures are gone. */
        ray_release(mentioned);
        return;
    }
    ray_t* names = ray_closure_names_from(LAMBDA_CLOSURE(lambda), mentioned, dynamic);
    ray_t* tmpl = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    if (!tmpl || RAY_IS_ERR(tmpl)) { if (names) ray_release(names); ray_release(mentioned); return; }
    tmpl->type = RAY_LAMBDA;
    tmpl->attrs = 0;
    tmpl->len = 0;
    memset(ray_data(tmpl), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    ray_retain(params); LAMBDA_PARAMS(tmpl) = params;
    ray_retain(body);   LAMBDA_BODY(tmpl) = body;
    ray_t* nfo = literal_nfo(lambda);
    if (nfo) { ray_retain(nfo); LAMBDA_NFO(tmpl) = nfo; }
    /* A closure body: its free names resolve when it runs, as the tree
     * walker resolves them, so one template serves every closure of the
     * literal whatever is rebound in between. */
    bool compiled = ray_compile_with_captures(tmpl, names, true);
    if (!compiled) {
        /* Not compilable: the closure stays interpreted, and the entry
         * remembers that so later closures from the literal skip the
         * attempt. */
        ray_release(tmpl);
        tmpl = NULL;
        LAMBDA_NLOCALS(lambda) = -1;
    }
    closure_tmpl_sweep();
    closure_tmpl_t* e = &g_closure_tmpl[g_closure_tmpl_next++ % CLOSURE_TMPL_SLOTS];
    closure_tmpl_drop(e);
    /* Make room within the budget, oldest entries first. */
    for (uint32_t k = 0; k < CLOSURE_TMPL_SLOTS && g_closure_tmpl_bytes + weight > CLOSURE_TMPL_BUDGET; k++)
        closure_tmpl_drop(&g_closure_tmpl[(g_closure_tmpl_next + k) % CLOSURE_TMPL_SLOTS]);
    ray_retain(params); e->params = params;
    for (int64_t k = 0; k < body->len; k++) { ray_retain(bn[k]); e->body[k] = bn[k]; }
    e->nbody = body->len;
    e->mentioned = mentioned;   /* owned */
    e->dynamic = dynamic;
    e->failed = !compiled;
    e->bytes = weight;
    g_closure_tmpl_bytes += weight;
    e->names = names;   /* owned, may be NULL */
    e->tmpl = tmpl;     /* owned, NULL when failed */
    g_closure_tmpl_made++;
    if (compiled) closure_share_from(lambda, tmpl);
}

/* A lambda with nothing captured compiles on its first call and bakes the
 * builtins it names then.  Another lambda of the same literal reuses that
 * bytecode when, at its own first call, every call head the code was
 * shaped by still names the builtin it named (or still names none):
 * compiling afresh would produce the same code.  The table bounds what
 * it keeps as for closures. */
static bool definition_deps_hold(ray_t* deps) {
    const int64_t* d = (const int64_t*)ray_data(deps);
    for (int64_t i = 0; i + 1 < deps->len; i += 2) {
        ray_t* fn = ray_env_get_compiled(d[i]);
        bool builtin = fn && (fn->type == RAY_UNARY || fn->type == RAY_BINARY || fn->type == RAY_VARY);
        if ((builtin ? (int64_t)(uintptr_t)fn : 0) != d[i + 1]) return false;
    }
    return true;
}

static void definition_compile(ray_t* lambda) {
    ray_t* body = LAMBDA_BODY(lambda);
    ray_t* params = LAMBDA_PARAMS(lambda);
    if (!body || body->type != RAY_LIST || body->len == 0 || !params ||
        body->len > CLOSURE_TMPL_MAX_BODY) { ray_compile(lambda); return; }
    if (g_closure_tmpl_epoch != ray_sym_epoch()) {
        if (g_closure_tmpl_epoch) closure_tmpl_forget();
        g_closure_tmpl_epoch = ray_sym_epoch();
    }
    ray_t** bn = (ray_t**)ray_data(body);
    for (int i = 0; i < CLOSURE_TMPL_SLOTS; i++) {
        closure_tmpl_t* e = &g_closure_tmpl[i];
        if (!e->deps || e->params != params || e->nbody != body->len) continue;
        bool same = true;
        for (int64_t k = 0; k < body->len && same; k++) same = e->body[k] == bn[k];
        if (!same) continue;
        if (definition_deps_hold(e->deps)) { closure_share_from(lambda, e->tmpl); return; }
        closure_tmpl_drop(e);   /* a head was rebound: this literal compiles afresh */
        break;
    }
    bool dynamic = false;
    int64_t weight = 0;
    ray_t* mentioned = ray_closure_mentioned_syms(body, &dynamic, &weight);
    if (!mentioned || weight > CLOSURE_TMPL_MAX_BYTES) {
        if (mentioned) ray_release(mentioned);
        ray_compile(lambda);
        return;
    }
    /* The entry keeps this lambda as its template, nfo and all. */
    lambda_set_nfo(lambda, literal_nfo(lambda));
    ray_t* deps = NULL;
    if (!ray_compile_definition(lambda, &deps)) { ray_release(mentioned); return; }
    closure_tmpl_sweep();
    closure_tmpl_t* e = &g_closure_tmpl[g_closure_tmpl_next++ % CLOSURE_TMPL_SLOTS];
    closure_tmpl_drop(e);
    for (uint32_t k = 0; k < CLOSURE_TMPL_SLOTS && g_closure_tmpl_bytes + weight > CLOSURE_TMPL_BUDGET; k++)
        closure_tmpl_drop(&g_closure_tmpl[(g_closure_tmpl_next + k) % CLOSURE_TMPL_SLOTS]);
    ray_retain(params); e->params = params;
    for (int64_t k = 0; k < body->len; k++) { ray_retain(bn[k]); e->body[k] = bn[k]; }
    e->nbody = body->len;
    e->mentioned = mentioned;   /* owned */
    e->bytes = weight;
    g_closure_tmpl_bytes += weight;
    ray_retain(lambda); e->tmpl = lambda;   /* its bytecode is the template's */
    e->deps = deps;             /* owned */
    g_closure_tmpl_made++;
}

/* Compile a lambda on its first call (see lambda_wants_compile). */
static void lambda_compile(ray_t* lambda) {
    if (LAMBDA_CLOSURE(lambda)) ray_compile(lambda);
    else definition_compile(lambda);
}

ray_t* ray_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", "fn: expected at least 2 args (params body), got %lld", (long long)n);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    ray_t* params_list = args[0];

    /* Reject lambda parameters named under the reserved `.` namespace.
     * Even though the bytecode VM resolves them to slot indices rather
     * than env entries, a user-defined fn with `.sys.gc` as a parameter
     * would silently override the builtin inside the body via the
     * compile-time name→slot map — that counts as shadowing and is
     * disallowed for the same reason `(let .sys.gc ...)` is.  Lambda
     * param lists are SYM *vectors* (not RAY_LISTs): `[a b c]` of all
     * syms is stored as a flat i64 sym-id array. */
    if (params_list) {
        int64_t nparams = ray_len(params_list);
        if (params_list->type == RAY_SYM) {
            /* name-ids: stays global — param lists are parser-built
             * runtime SYM vecs of BINDING names, not data cells. */
            int64_t* ids = (int64_t*)ray_data(params_list);
            for (int64_t i = 0; i < nparams; i++)
                if (ray_sym_is_reserved(ids[i]))
                    return ray_error("reserve",
                        "lambda parameter '%s' is in the reserved namespace",
                        ray_str_ptr(ray_sym_str(ids[i])));
        } else if (params_list->type == RAY_LIST) {
            ray_t** pelems = (ray_t**)ray_data(params_list);
            for (int64_t i = 0; i < nparams; i++) {
                ray_t* p = pelems[i];
                if (p && p->type == -RAY_SYM && ray_sym_is_reserved(p->i64))
                    return ray_error("reserve",
                        "lambda parameter '%s' is in the reserved namespace",
                        ray_str_ptr(ray_sym_str(p->i64)));
            }
        }
    }

    ray_t* caprows = NULL;
    uint32_t capq = 0;
    ray_t* closure = ray_env_capture_locals_rows(&caprows, &capq);
    if (closure && RAY_IS_ERR(closure)) return closure;
    /* A lambda made inside a query is query text: it keeps the query's
     * literal rule by running on the tree walker, so it is never compiled.
     * The serial of the query frame marks it. */
    if (!capq) capq = ray_env_query_serial();

    /* Create lambda object (layout in eval.h): params, body, bytecode,
     * constants, n_locals, nfo, dbg, closure, capture row flags, query. */
    ray_t* lambda = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    if (!lambda) { ray_release(closure); if (caprows) ray_release(caprows); return ray_error("oom", NULL); }
    lambda->type = RAY_LAMBDA;
    lambda->attrs = 0;
    lambda->len = 0;
    memset(ray_data(lambda), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    LAMBDA_CAPROWS(lambda) = caprows;   /* owned */
    LAMBDA_CAPQ(lambda) = capq;

    /* Store params list */
    ray_retain(params_list);
    LAMBDA_PARAMS(lambda) = params_list;

    /* Build body list: wrap body expressions in a RAY_LIST */
    int64_t body_count = n - 1;
    ray_t* body = ray_alloc(body_count * sizeof(ray_t*));
    if (!body) {
        ray_release(lambda);
        ray_release(closure);
        return ray_error("oom", NULL);
    }
    body->type = RAY_LIST;
    body->len = body_count;
    ray_t** body_elems = (ray_t**)ray_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        ray_retain(args[i + 1]);
        body_elems[i] = args[i + 1];
    }
    LAMBDA_BODY(lambda) = body;

    /* Clear compiled slots */
    LAMBDA_BC(lambda) = NULL;
    LAMBDA_CONSTS(lambda) = NULL;
    LAMBDA_NLOCALS(lambda) = 0;

    /* Attach source location info from current eval context */
    if (__VM->nfo) {
        LAMBDA_NFO(lambda) = __VM->nfo;
        ray_retain(__VM->nfo);
    } else {
        LAMBDA_NFO(lambda) = NULL;
    }
    LAMBDA_DBG(lambda) = NULL;
    LAMBDA_CLOSURE(lambda) = closure;

    /* Share the literal's compiled template unless the lambda is query text
     * or has a closure the compiler cannot bind (the compiler's own gate). */
    if (!capq && ray_closure_shape_ok(closure))
        closure_share_template(lambda);

    return lambda;
}

/* A frame element that failed to allocate is left out (NULL), and its
 * error object freed: frames are released as ordinary lists, and a release
 * does not free an error object. */
static ray_t* frame_elem(ray_t* v) {
    if (v && RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    return v;
}

/* Build a [span_i64, filename, fn_name, source] frame from a resolved span
 * and append it to __VM->trace.  Shared by the bytecode and eval paths, and
 * by the parser (ray_error_frame_add), which names its frame and adds the
 * failing byte's offset as a fifth element (see ray_error_frame_add).
 * Best effort: a frame that cannot be built is dropped.  The error being
 * traced owns its kind and message, which nothing here can change; the
 * thread's usage hint, which an allocation failure here would clear, is
 * put back. */
static void append_error_frame_named(ray_t* nfo, ray_span_t span, const char* name,
                                     int64_t offset) {
    if (span.id == 0 || !__VM) return;

    const char* saved_usage = ray_error_usage();

    int64_t n = offset >= 0 ? 5 : 4;
    ray_t* frame = ray_alloc((size_t)n * sizeof(ray_t*));
    if (!frame || RAY_IS_ERR(frame)) goto restore;
    frame->type = RAY_LIST;
    frame->len = n;
    ray_t** fe = (ray_t**)ray_data(frame);

    fe[0] = frame_elem(ray_i64(span.id));
    if (nfo && NFO_FILENAME(nfo)) {
        fe[1] = NFO_FILENAME(nfo);
        ray_retain(fe[1]);
    } else {
        fe[1] = frame_elem(ray_str("<unknown>", 9));
    }
    fe[2] = name ? frame_elem(ray_str(name, strlen(name))) : NULL;
    if (nfo && NFO_SOURCE(nfo)) {
        fe[3] = NFO_SOURCE(nfo);
        ray_retain(fe[3]);
    } else {
        fe[3] = frame_elem(ray_str("", 0));
    }
    if (n == 5) fe[4] = frame_elem(ray_i64(offset));
    if (!fe[0]) { ray_release(frame); goto restore; }

    if (!__VM->trace) {
        __VM->trace = ray_alloc(sizeof(ray_t*));
        if (!__VM->trace || RAY_IS_ERR(__VM->trace)) {
            __VM->trace = NULL;
            ray_release(frame);
            goto restore;
        }
        __VM->trace->type = RAY_LIST;
        __VM->trace->len = 1;
        ((ray_t**)ray_data(__VM->trace))[0] = frame;
    } else {
        /* The trace is the VM's own (never shared), so a failed append
         * leaves it as it was. */
        ray_t* t = ray_list_append(__VM->trace, frame);
        if (t && !RAY_IS_ERR(t)) __VM->trace = t;
        else ray_error_free(t);
        ray_release(frame);
    }

restore:
    ray_error_usage_set(saved_usage);
}

static void append_error_frame(ray_t* nfo, ray_span_t span) {
    append_error_frame_named(nfo, span, NULL, -1);
}

void ray_error_frame_add(ray_t* nfo, ray_span_t span, const char* name, int64_t offset) {
    append_error_frame_named(nfo, span, name, offset);
}

/* Build a single error trace frame from a lambda's debug/nfo info at the given
 * bytecode IP. */
static void add_error_frame(ray_t* fn, int32_t ip) {
    if (!fn || fn->type != RAY_LAMBDA) return;
    ray_t* dbg = LAMBDA_DBG(fn);
    ray_t* nfo = LAMBDA_NFO(fn);
    if (!dbg && !nfo) return;

    ray_span_t span = {0};
    if (dbg) span = ray_bc_dbg_get(dbg, ip);
    append_error_frame(nfo, span);
}

/* Add error frame from eval context (nfo + AST node) for call-site errors. */
static void add_eval_error_frame(ray_t* nfo, ray_t* node) {
    if (!nfo || !node) return;
    append_error_frame(nfo, ray_nfo_get(nfo, node));
}

/* Execute compiled bytecode for a lambda. */
static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc);
static ray_t* vm_exec_counted(ray_t* lambda, ray_t** call_args, int64_t argc,
                              uint64_t row_args, uint8_t* out_rows);
static ray_t* vm_exec_rows(ray_t* lambda, ray_t** call_args, int64_t argc,
                           uint64_t row_args, uint8_t* out_rows);


/* Inside a helper called from a query (a call frame above the query's
 * frames).  Literal symbols keep bytecode's meaning in a helper called by
 * name; a lambda written inline in the query is query text and follows the
 * column rule. */
bool ray_eval_query_helper_active(void) {
    return ray_env_in_query_helper(NULL);
}

bool ray_eval_query_helper_literals(void) {
    bool named = false;
    return ray_env_in_query_helper(&named) && named;
}

/* Ordinary calls use bytecode; query fallback interprets the retained AST
 * so concat can distinguish column parameters from bound collections. */
/* Whether a call should compile the lambda first.  A lambda made inside a
 * query (LAMBDA_CAPQ set and no bytecode) is query text and keeps the
 * query's literal rule on the tree walker, so it never compiles; a closure
 * made by compiled code (OP_MKCLOSURE) is born compiled, sharing its
 * literal's bytecode.  Everything else compiles on its first call. */
static inline bool lambda_wants_compile(ray_t* lambda) {
    /* NLOCALS -1 marks a body that did not compile: it stays on the tree
     * walker without another attempt per call. */
    return !LAMBDA_IS_COMPILED(lambda) && !LAMBDA_CAPQ(lambda) && LAMBDA_NLOCALS(lambda) != -1;
}

static ray_t* call_lambda_impl(ray_t* lambda, ray_t** call_args, int64_t argc,
                               bool query, uint64_t row_args, bool named,
                               int8_t* out_rows) {
    if (out_rows) *out_rows = -1;
    if (RAY_UNLIKELY(lambda_lifts(lambda)) && lift_permitted(lambda)) {
        /* Isolated as well as lifted: the body sees its arguments, its
         * captures and the globals, as it would called at the top level.
         * The caller's locals, eval windows and query stay hidden, so a
         * restricted caller cannot rebind a name the body reads. */
        ray_query_state_t query_state;
        ray_query_suspend(&query_state);
        int32_t scope_base = ray_env_isolate_begin();
        bool restricted = __VM->restricted;
        __VM->restricted = false;
        ray_t* result = call_lambda_impl(lambda, call_args, argc, false, 0, false, NULL);
        __VM->restricted = restricted;
        ray_env_isolate_end(scope_base);
        ray_query_resume(&query_state);
        return result;
    }
    /* A lambda called inside a named helper's body is that body's text. */
    if (query && !named) named = ray_eval_query_helper_literals();
    /* Lazy compilation (see lambda_wants_compile); a closure's captures
     * become locals of its body (OP_BINDCAPS). */
    if (lambda_wants_compile(lambda))
        lambda_compile(lambda);
    if (LAMBDA_IS_COMPILED(lambda)) {
        if (!query) return vm_exec(lambda, call_args, argc);
        /* Row provenance rides on the VM stack.  The call frame hides the
         * query's aliases and locals; source columns stay visible through
         * its table, then the globals, as in a body the planner inlines. */
        if (ray_env_push_call_scope(ray_query_helper_table(), named) != RAY_OK)
            return ray_error("oom", NULL);
        uint8_t result_rows = 0;
        ray_t* result = vm_exec_counted(lambda, call_args, argc, row_args, &result_rows);
        ray_env_pop_scope();
        if (out_rows) *out_rows = vrows_to(result_rows);
        return result;
    }

    /* Fallback: tree-walking interpreter */
    ray_t* params_list = LAMBDA_PARAMS(lambda);
    ray_t* body = LAMBDA_BODY(lambda);

    int64_t param_count = ray_len(params_list);

    if (argc != param_count)
        return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);

    bool has_closure = LAMBDA_CLOSURE(lambda) != NULL;
    /* A query helper's call frame caches source columns on demand, above
     * its table_from.  It sits below the frame that binds self, the
     * parameters and the body's lets, so none of those is ever taken for
     * a column: the captures' frame for a closure, an empty call frame
     * otherwise, as a compiled helper keeps its locals in VM slots. */
    bool call_frame = has_closure || query;
    if (has_closure && ray_env_push_capture(LAMBDA_CLOSURE(lambda), LAMBDA_CAPROWS(lambda),
                                            LAMBDA_CAPQ(lambda)) != RAY_OK)
        return ray_error("oom", NULL);
    if (query && has_closure) ray_env_mark_call_scope(ray_query_helper_table(), named);
    if (query && !has_closure &&
        ray_env_push_call_scope(ray_query_helper_table(), named) != RAY_OK)
        return ray_error("oom", NULL);
    if (ray_env_push_scope() != RAY_OK) {
        if (call_frame) ray_env_pop_scope();
        return ray_error("oom", NULL);
    }
    /* Closures made inside the body capture down to this call's frames
     * (its captures or columns, and its parameters), never the caller's
     * locals. */
    ray_env_mark_lexical_scope(call_frame ? 1 : 0);

    /* Bind 'self' to the current lambda for recursion */
    if (g_call_self_sym < 0) g_call_self_sym = ray_sym_intern("self", 4);
    ray_env_set_local(g_call_self_sym, lambda);

    int64_t* param_ids = (int64_t*)ray_data(params_list);
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        ray_t* arg = call_args[i];
        if (arg && ray_is_lazy(arg)) {
            /* A lazy handle is single-use: bind the concrete value, as the
             * bytecode VM does when it loads a lazy parameter. */
            ray_retain(arg);
            arg = ray_lazy_materialize(arg);
            if (!arg || RAY_IS_ERR(arg)) {
                ray_env_pop_scope();
                if (call_frame) ray_env_pop_scope();
                return arg ? arg : ray_error("type", NULL);
            }
        }
        /* Parameters carry the arguments' row provenance into the body. */
        uint8_t rows = query && i < 64 ? (uint8_t)((row_args >> i) & 1) : 0;
        (void)ray_env_set_local_rows(param_ids[i], arg, rows);
        if (arg != call_args[i]) ray_release(arg);
    }

    int64_t body_count = ray_len(body);
    ray_t** body_exprs = (ray_t**)ray_data(body);
    ray_t* result = NULL;
    __VM->lambda_depth++;
    for (int64_t i = 0; i < body_count; i++) {
        if (result) ray_release(result);
        result = ray_eval(body_exprs[i]);
        if (RAY_IS_ERR(result)) {
            if (is_return_marker(result)) {
                /* (return x) somewhere in this statement: the body ends here
                 * with x, as it does when compiled. */
                result = ray_take_return(result);
                g_eval_rows = -1;
                break;
            }
            __VM->lambda_depth--;
            ray_env_pop_scope();
            if (call_frame) ray_env_pop_scope();
            return result;
        }
    }
    __VM->lambda_depth--;
    if (out_rows) *out_rows = g_eval_rows;   /* the body's last value's provenance */

    ray_env_pop_scope();
    if (call_frame) ray_env_pop_scope();
    return result;
}

/* Compile `lambda` now if its first call would (lambda_wants_compile):
 * pmap does so on the calling thread before workers call it, so no two
 * threads compile one lambda. */
void ray_lambda_ready(ray_t* lambda) {
    if (lambda && lambda->type == RAY_LAMBDA && lambda_wants_compile(lambda)) lambda_compile(lambda);
}

/* Does `e`, a lambda body, name nothing but the lambda's parameters (as
 * data, never called) and global unary or binary builtins that are neither
 * special forms nor restricted nor eval?  Such a body reads no column, calls
 * no function it was given and reaches no name at run time, so it needs no
 * query call frame.  Anything else (a let, an if, a variadic or higher-order
 * builtin, a helper, an unbound name, a quoted form) may. */
static bool lambda_body_plain(ray_t* e, ray_t* params, int depth) {
    if (!e) return true;
    if (depth > 64) return false;
    if (e->type == -RAY_SYM) {
        if (e->attrs & ATTR_QUOTED) return true;   /* in a helper, the symbol itself */
        for (int64_t i = 0; i < ray_len(params); i++)
            if (((int64_t*)ray_data(params))[i] == e->i64) return true;
        ray_t* g = ray_env_get_global(e->i64);
        if (!g || (g->type != RAY_UNARY && g->type != RAY_BINARY)) return false;
        if (g->attrs & (RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED)) return false;
        return !(g->type == RAY_UNARY && (ray_unary_fn)(uintptr_t)g->i64 == ray_eval_builtin_fn);
    }
    if (e->type == RAY_LIST) {
        if (e->attrs & ATTR_QUOTED) return false;
        ray_t** el = (ray_t**)ray_data(e);
        if (ray_len(e) > 0 && el[0] && el[0]->type == -RAY_SYM && !(el[0]->attrs & ATTR_QUOTED))
            for (int64_t i = 0; i < ray_len(params); i++)
                if (((int64_t*)ray_data(params))[i] == el[0]->i64) return false;   /* a parameter called */
        for (int64_t i = 0; i < ray_len(e); i++)
            if (!lambda_body_plain(el[i], params, depth + 1)) return false;
        return true;
    }
    if (e->type < 0) return true;                    /* a non-symbol atom */
    return ray_is_vec(e) && e->type != RAY_LIST;      /* a literal vector */
}

/* Does `e` name a column of `tbl` (a builtin's name may be one: date,
 * count, first)?  A source column shadows the builtin in a helper. */
static bool lambda_body_names_col(ray_t* e, ray_t* tbl, int depth) {
    if (!e || depth > 64) return depth > 64;
    if (e->type == -RAY_SYM) {
        if (e->attrs & ATTR_QUOTED) return false;
        for (int64_t i = 0, n = ray_table_ncols(tbl); i < n; i++)
            if (ray_table_col_name(tbl, i) == e->i64) return true;
        return false;
    }
    if (e->type == RAY_LIST)
        for (int64_t i = 0; i < ray_len(e); i++)
            if (lambda_body_names_col(((ray_t**)ray_data(e))[i], tbl, depth + 1)) return true;
    return false;
}

/* Can a call of the closure-free `lambda` inside the query on `tbl` skip
 * the query's call frame: its body names only parameters and first-order
 * builtins (lambda_body_plain, cached for the current global epoch, since
 * a global rebound may change the answer), none of them a column of `tbl`
 * (cached for the last table checked)? */
static inline uint32_t lambda_query_epoch(void) {
    return (uint32_t)atomic_load_explicit(&g_global_epoch, memory_order_relaxed) & 0x3fffffffu;
}
static inline uint32_t lambda_query_tag(const ray_t* tbl) {
    return ((uint32_t)((uintptr_t)tbl >> 4) << 2) | 2u;
}
static __attribute__((noinline)) bool lambda_query_plain(ray_t* lambda, ray_t* tbl) {
    if (LAMBDA_CLOSURE(lambda) || !tbl || tbl->type != RAY_TABLE) return false;
    uint32_t ge = lambda_query_epoch();
    uint32_t c = LAMBDA_QPLAIN(lambda);
    if (!(c & 2u) || (c >> 2) != ge) {
        bool plain = LAMBDA_PARAMS(lambda) && LAMBDA_PARAMS(lambda)->type == RAY_SYM &&
                     lambda_body_plain(LAMBDA_BODY(lambda), LAMBDA_PARAMS(lambda), 0);
        c = LAMBDA_QPLAIN(lambda) = (ge << 2) | 2u | (plain ? 1u : 0u);
        LAMBDA_QTBL(lambda) = 0;
    }
    if (!(c & 1u)) return false;
    uint32_t tag = lambda_query_tag(tbl);
    uint32_t t = LAMBDA_QTBL(lambda);
    if ((t & ~1u) == tag) return t & 1u;
    bool ok = !lambda_body_names_col(LAMBDA_BODY(lambda), tbl, 0);
    LAMBDA_QTBL(lambda) = tag | (ok ? 1u : 0u);
    return ok;
}

ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc) {
    if (RAY_UNLIKELY(lambda_lifts(lambda)))
        return call_lambda_impl(lambda, call_args, argc, false, 0, false, NULL);
    /* A helper a builtin calls inside a query, (map f ..), (fold g ..),
     * however it was reached (a name, (at (env 0) ..)), sees the source
     * columns as the direct call (f a) does: through the query's call
     * frame (#1762).  The arguments are the builtin's elements, not the
     * query's rows.  A lambda made inside the query is query text and
     * keeps its own path, which already sees the columns; a body that can
     * reach no column (lambda_query_plain) needs no frame. */
    if (RAY_UNLIKELY(g_active_query_table != NULL) && !LAMBDA_CAPQ(lambda)) {
        ray_t* tbl = g_query_source_table ? g_query_source_table : g_active_query_table;
        /* The cached answer first, inline: a plain body checked against
         * this table under this global epoch (lambda_query_plain). */
        bool plain = LAMBDA_QPLAIN(lambda) == ((lambda_query_epoch() << 2) | 3u) &&
                     LAMBDA_QTBL(lambda) == (lambda_query_tag(tbl) | 1u);
        if (!plain && !lambda_query_plain(lambda, tbl))
            return call_lambda_impl(lambda, call_args, argc, true, 0, true, NULL);
    }
    if (lambda_wants_compile(lambda)) lambda_compile(lambda);
    if (LAMBDA_IS_COMPILED(lambda)) return vm_exec(lambda, call_args, argc);
    return call_lambda_impl(lambda, call_args, argc, false, 0, false, NULL);
}

/* ══════════════════════════════════════════
 * Stack-based VM executor (computed goto, frame-based)
 * ══════════════════════════════════════════ */

/* Shared thread-local with runtime.c (declared extern in core/runtime.h).
 * Defining it locally here would shadow runtime.c's symbol, leaving
 * ray_error_msg() reading a NULL pointer on any thread that ran an eval
 * without going through ray_runtime_create — which is every tokio worker
 * thread in ray-exomem. */
/* __VM (per-thread VM) comes from core/runtime.h — the executor below
 * allocates a private ray_exec_t per invocation and leaves __VM alone. */

/* A backstop count of compiled bodies open on this stack (vm_reentry):
 * ray_stack_short stops a body recursing through a builtin's callback at
 * the thread's stack reserve, but where the platform gives no bound (the
 * main thread under `ulimit -s unlimited`, whose reported stack runs to
 * the next mapping) nothing would, until memory ran out.  An optimized
 * build reaches about 1,700 levels on an 8 MB stack, so the count never
 * binds first there. */
#define RAY_VM_REENTRY_MAX 10000
static ray_t* vm_exec_counted(ray_t* lambda, ray_t** call_args, int64_t argc,
                              uint64_t row_args, uint8_t* out_rows) {
    if (RAY_UNLIKELY(__VM->vm_reentry >= RAY_VM_REENTRY_MAX))
        return ray_error("limit", "stack overflow");
    __VM->vm_reentry++;
    ray_t* r = vm_exec_rows(lambda, call_args, argc, row_args, out_rows);
    __VM->vm_reentry--;
    return r;
}

static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc) {
    return vm_exec_counted(lambda, call_args, argc, 0, NULL);
}

/* An element-wise builtin (RAY_FN_ROWWISE) called inside a query with an
 * argument that is a column's rows (row flag 1, a vector or list) is
 * applied once per row: each such argument gives that row's cell, any
 * other argument is passed whole, and the row results, strings, form a
 * string vector (a list when they are not all strings; an empty string
 * vector over no rows).  So update, by:
 * outputs and keys, and a whole-column projection answer row by row as the
 * per-row projection does (#1594).  Not while a per-row evaluation is in
 * progress, where a column is already its cell; not outside a query.
 * `fn_type` is the builtin's type and `fnp` its function; `rows` the
 * arguments' flags in the VM encoding.  NULL when it does not apply. */
/* Is the builtin function pointer `fp` first or last, whose query reading
 * skips nulls (ray_query_edge_value, #1673)? */
static inline bool vm_fn_is_edge(int64_t fp) {
    return (void*)(uintptr_t)fp == (void*)ray_first_fn || (void*)(uintptr_t)fp == (void*)ray_last_fn;
}

#define ROWWISE_MAX_ARGS 64   /* a builtin call takes at most 64 arguments */
static ray_t* rowwise_apply(int8_t fn_type, void* fnp, ray_t** args, const uint8_t* rows,
                            int64_t argc) {
    /* The per-row arrays below hold one slot per argument: more than they
     * hold is refused, never written past (callers refuse such a call
     * before it gets here). */
    if (argc > ROWWISE_MAX_ARGS)
        return ray_error("limit", "row-wise call: at most %d arguments, got %lld",
                         ROWWISE_MAX_ARGS, (long long)argc);
    if (argc < 1 || !ray_active_query_table() || ray_query_row_active()) return NULL;
    int64_t nrows = -1;
    bool map[ROWWISE_MAX_ARGS] = { false };
    for (int64_t j = 0; j < argc; j++) {
        map[j] = rows[j] == 1 && args[j] && !RAY_IS_ERR(args[j]) && !ray_is_atom(args[j]) &&
                 (ray_is_vec(args[j]) || args[j]->type == RAY_LIST);
        if (!map[j]) continue;
        if (nrows < 0) nrows = ray_len(args[j]);
        else if (ray_len(args[j]) != nrows)
            return ray_error("length", "row-wise call: arguments of %lld and %lld rows",
                             (long long)nrows, (long long)ray_len(args[j]));
    }
    if (nrows < 0) return NULL;
    ray_t* out = ray_list_new(nrows > 0 ? nrows : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    bool all_str = true;
    for (int64_t i = 0; i < nrows; i++) {
        ray_t* cargs[ROWWISE_MAX_ARGS] = { NULL };
        int alloc[ROWWISE_MAX_ARGS] = { 0 };
        for (int64_t j = 0; j < argc; j++) {
            alloc[j] = 0;
            cargs[j] = map[j] ? collection_elem(args[j], i, &alloc[j]) : args[j];
        }
        ray_t* r = fn_type == RAY_UNARY ? ((ray_unary_fn)fnp)(cargs[0])
                 : fn_type == RAY_BINARY ? ((ray_binary_fn)fnp)(cargs[0], cargs[1])
                 : ((ray_vary_fn)fnp)(cargs, argc);
        for (int64_t j = 0; j < argc; j++) if (alloc[j]) ray_release(cargs[j]);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("oom", NULL); }
        all_str = all_str && r->type == -RAY_STR;
        out = ray_list_append(out, r);
        ray_release(r);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    if (nrows == 0) {   /* no rows: no string, the marked builtins render text */
        ray_release(out);
        return ray_vec_new(RAY_STR, 0);
    }
    if (!all_str) return out;
    ray_t* sv = ray_vec_new(RAY_STR, nrows);
    ray_t** cells = (ray_t**)ray_data(out);
    for (int64_t i = 0; i < nrows && sv && !RAY_IS_ERR(sv); i++)
        sv = ray_str_vec_append(sv, ray_str_ptr(cells[i]), ray_str_len(cells[i]));
    ray_release(out);
    return sv ? sv : ray_error("oom", NULL);
}

/* Whether a builtin keeps row provenance: the planner lowers it row by
 * row, so its result over a column-derived operand is column-derived.
 * Cached by object for one symbol-table epoch: the builtin objects live
 * as long as the runtime, and a re-init frees and reallocates them. */
static bool vm_fn_rowwise(const ray_t* fn_obj) {
    static _Thread_local struct { const ray_t* fn; bool rowwise; } cache[64];
    static _Thread_local uint64_t epoch;
    if (epoch != ray_sym_epoch() || !epoch) {
        memset(cache, 0, sizeof cache);
        epoch = ray_sym_epoch();
    }
    size_t h = ((uintptr_t)fn_obj >> 4) & 63;
    if (cache[h].fn == fn_obj) return cache[h].rowwise;
    const char* name = ray_fn_name(fn_obj);
    bool rowwise = ray_query_rowwise_builtin(ray_sym_intern(name, strlen(name)));
    cache[h].fn = fn_obj;
    cache[h].rowwise = rowwise;
    return rowwise;
}

/* Control forms return one of their operands, so the operand's provenance
 * is the form's; every other special form builds a new value. */
static bool form_inherits_rows(const ray_t* head) {
    static const char* const forms[] = { "if", "do", "let", "set", "and", "or", "try" };
    const char* name = ray_fn_name(head);
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++)
        if (strcmp(name, forms[i]) == 0) return true;
    return false;
}

static bool vm_text_value(const ray_t* v) {
    return v->type == RAY_SYM || v->type == RAY_STR || v->type == -RAY_SYM || v->type == -RAY_STR;
}

/* A unary builtin applied by compiled code to `arg` (borrowed). */
static inline ray_t* vm_apply_unary_raw(ray_t* fn_obj, ray_t* arg) {
    ray_unary_fn fn = (ray_unary_fn)(uintptr_t)fn_obj->i64;
    if (RAY_UNLIKELY(RAY_IS_NULL(arg)))
        return (fn == (ray_unary_fn)ray_nil_fn || fn == (ray_unary_fn)ray_type_fn)
               ? fn(arg) : ray_error("type", NULL);
    if ((fn_obj->attrs & RAY_FN_ATOMIC) && arg->type >= 0)
        return atomic_map_unary(fn, arg);
    return fn(arg);
}

static inline ray_t* vm_apply_unary(ray_t* fn_obj, ray_t* arg) {
    return ray_sig_note(ray_fn_name(fn_obj), vm_apply_unary_raw(fn_obj, arg));
}

/* A binary builtin applied by compiled code.  Consumes `left` and `right`;
 * `left_rows`/`right_rows` are their row-provenance flags and `*out_rows`
 * receives the result's.  Returns an owned result or error. */
static ray_t* vm_call_binary_raw(ray_t* fn_obj, ray_t* left, ray_t* right,
                                 uint8_t left_rows, uint8_t right_rows, uint8_t* out_rows) {
    ray_binary_fn fn = (ray_binary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    *out_rows = 0;
    /* Materialise lazy args for non-lazy-aware fns; ray_lazy_materialize
     * consumes the owned ref it is given. */
    if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {
        if (left && ray_is_lazy(left)) {
            left = ray_lazy_materialize(left);
            if (!left || RAY_IS_ERR(left)) { ray_release(right); return left ? left : ray_error("type", NULL); }
        }
        if (right && ray_is_lazy(right)) {
            right = ray_lazy_materialize(right);
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right ? right : ray_error("type", NULL); }
        }
    }
    RAY_ASSERT_VALUE(left); RAY_ASSERT_VALUE(right);
    if (RAY_UNLIKELY(RAY_IS_NULL(left) || RAY_IS_NULL(right))) {
        result = (fn == ray_identical_fn || fn == (ray_binary_fn)ray_eq_fn || fn == (ray_binary_fn)ray_neq_fn)
                 ? fn(left, right) : ray_error("type", NULL);
    } else if (fn == ray_concat_fn && (left_rows == 1 || right_rows == 1) &&
               vm_text_value(left) && vm_text_value(right) &&
               (left_rows == 1 || ray_is_atom(left)) && (right_rows == 1 || ray_is_atom(right))) {
        /* Query text concat: a column's cells joined row by row, as the
         * planner does for the query's own expressions.  Without row data
         * among the operands, a call inside a function keeps ordinary
         * collection semantics; a bound vector never has row provenance. */
        result = ray_concat_text_fn(left, right,
                                    (left->type == -RAY_STR && left_rows == 1) ||
                                    (right->type == -RAY_STR && right_rows == 1));
        *out_rows = 1;
    /* Fast path: atoms have negative type — skip collection check entirely.
     * Only call is_collection when at least one arg has type >= 0 (vector/list). */
    } else {
        uint8_t joined = vrows_join(left_rows, right_rows);
        if ((fn_obj->attrs & RAY_FN_ATOMIC) && (left->type >= 0 || right->type >= 0))
            result = atomic_map_binary_op(fn, RAY_FN_OPCODE(fn_obj), left, right);
        else
            result = fn(left, right);
        *out_rows = joined && vm_fn_rowwise(fn_obj) ? joined : 0;
    }
    ray_release(left);
    ray_release(right);
    return result;
}

static ray_t* vm_call_binary(ray_t* fn_obj, ray_t* left, ray_t* right,
                             uint8_t left_rows, uint8_t right_rows, uint8_t* out_rows) {
    return ray_sig_note(ray_fn_name(fn_obj),
                        vm_call_binary_raw(fn_obj, left, right, left_rows, right_rows, out_rows));
}

/* The calls above, timed by the call profiler under the running lambda
 * `self` and the call's offset `ip`.  Out of line: the dispatch loop pays
 * only the test of the profiler flag. */
static __attribute__((noinline))
ray_t* vm_prof_unary(ray_t* fn_obj, ray_t* arg, ray_t* self, int32_t ip) {
    int32_t frame = ray_callprof_enter_vm(self, ip);
    ray_t* result = vm_apply_unary(fn_obj, arg);
    ray_callprof_exit(frame);
    return result;
}

static __attribute__((noinline))
ray_t* vm_prof_binary(ray_t* fn_obj, ray_t* left, ray_t* right, uint8_t left_rows,
                      uint8_t right_rows, uint8_t* out_rows, ray_t* self, int32_t ip) {
    int32_t frame = ray_callprof_enter_vm(self, ip);
    ray_t* result = vm_call_binary(fn_obj, left, right, left_rows, right_rows, out_rows);
    ray_callprof_exit(frame);
    return result;
}

static __attribute__((noinline))
ray_t* vm_prof_vary(ray_vary_fn fn, ray_t** args, int64_t n, ray_t* self, int32_t ip) {
    int32_t frame = ray_callprof_enter_vm(self, ip);
    ray_t* result = fn(args, n);
    ray_callprof_exit(frame);
    return result;
}

static __attribute__((noinline))
ray_t* vm_prof_eval(ray_t* ast, ray_t* self, int32_t ip) {
    int32_t frame = ray_callprof_enter_vm(self, ip);
    ray_t* result = ray_eval(ast);
    ray_callprof_exit(frame);
    return result;
}

/* `row_args` bit i: parameter i is derived from the rows of the active
 * query.  The flag rides on vm.rows beside every stack slot: loads and
 * stores copy it, a builtin the planner lowers row by row keeps it,
 * anything else clears it, and `concat` consults it to choose between
 * text and collection semantics.  `out_rows` receives the result's flag.
 *
 * The dispatch loop starts on a page boundary: where its handlers fall
 * relative to fetch and branch-predictor boundaries moved its speed when
 * code in an unrelated object grew or shrank.  How much depends on the
 * machine: on one box an 896-byte shift cost 5-10% on VM-bound loops; a
 * second measurement saw under 2% on the same loops and 4-6% on others. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((aligned(4096)))
#endif
static ray_t* vm_exec_rows(ray_t* lambda, ray_t** call_args, int64_t argc,
                           uint64_t row_args, uint8_t* out_rows) {
    /* Computed goto dispatch table */
    static void *dispatch[OP__COUNT] = {
        [OP_RET]        = &&op_ret,
        [OP_JMP]        = &&op_jmp,
        [OP_JMPF]       = &&op_jmpf,
        [OP_LOADCONST]  = &&op_loadconst,
        [OP_LOADENV]    = &&op_loadenv,
        [OP_STOREENV]   = &&op_storeenv,
        [OP_POP]        = &&op_pop,
        [OP_RESOLVE]    = &&op_resolve,
        [OP_CALL1]      = &&op_call1,
        [OP_CALL2]      = &&op_call2,
        [OP_CALLN]      = &&op_calln,
        [OP_CALLF]      = &&op_callf,
        [OP_CALLS]      = &&op_calls,
        [OP_CALLD]      = &&op_calld,
        [OP_DUP]        = &&op_dup,
        [OP_LOADCONST_W] = &&op_loadconst_w,
        [OP_RESOLVE_W]  = &&op_resolve_w,
        [OP_TRAP]       = &&op_trap,
        [OP_TRAPV]      = &&op_trapv,
        [OP_TRAP_END]   = &&op_trap_end,
        [OP_STOREGLOBAL]   = &&op_storeglobal,
        [OP_STOREGLOBAL_W] = &&op_storeglobal_w,
        [OP_SCOPE_BEGIN]   = &&op_scope_begin,
        [OP_SCOPE_END]     = &&op_scope_end,
        [OP_TRYH]          = &&op_tryh,
        [OP_FORCE]         = &&op_force,
        [OP_BINDCAPS]      = &&op_bindcaps,
        [OP_LOADSELF]      = &&op_loadself,
        [OP_MKCLOSURE]     = &&op_mkclosure,
        [OP_LOADENVG]      = &&op_loadenvg,
        [OP_SCOPE_EVAL]    = &&op_scope_eval,
        [OP_CALL1R]        = &&op_call1r,
        [OP_CALL2R]        = &&op_call2r,
        [OP_CALLNR]        = &&op_callnr,
        [OP_PATHGET]       = &&op_pathget,
        [OP_JMPFV]         = &&op_jmpfv,
        [OP_IFTREE]        = &&op_iftree,
    };

    /* Arity check before allocating VM state */
    {
        int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
        if (argc != param_count)
            return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);
    }

    if (RAY_UNLIKELY(!__VM))
        return ray_error("state", "no VM bound to this thread");

    /* Calls between compiled bodies stay on this frame (the VM's own
     * return stack, bounded by VM_STACK_SIZE), but a builtin that calls a
     * lambda back (map, fold, a sort key...) re-enters here on the C stack,
     * a frame per level with no count of its own: a body recursing through
     * one stops at the thread's stack reserve, as the evaluator does. */
    if (RAY_UNLIKELY(ray_stack_short()))
        return ray_error("limit", "stack overflow");

    /* Scope-stack baseline: OP_SCOPE_BEGIN windows that error out before
     * their OP_SCOPE_END leave materialized frames behind — the error
     * paths unwind back to this depth. */
    int32_t scope_base = ray_env_scope_depth();

    ray_t *vm_block = ray_alloc(sizeof(ray_exec_t));
    if (!vm_block || RAY_IS_ERR(vm_block)) return ray_error("oom", NULL);
    ray_exec_t *vmp = (ray_exec_t *)ray_data(vm_block);
    /* Only the header needs clearing: every stack, return and trap slot is
     * written before it is read, and the locals are set below.  Zeroing
     * the whole block cost more than a small call's body. */
    vmp->sp = 0; vmp->fp = 0; vmp->rp = 0; vmp->tp = 0; vmp->fn = NULL;
    __VM->lambda_depth++;

#define vm (*vmp)

    /* Set up initial frame */
    vm.fn = lambda;
    ray_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;

    /* Bind parameters into local slots; the other locals start empty */
    int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        ray_retain(call_args[i]);
        vm.ps[i] = call_args[i];
        vm.rows[i] = i < 64 ? (uint8_t)((row_args >> i) & 1) : 0;
    }
    for (int32_t i = (int32_t)(param_count < argc ? param_count : argc); i < n_locals; i++) {
        vm.ps[i] = NULL;
        vm.rows[i] = 0;
    }

    uint8_t *code = (uint8_t *)ray_data(LAMBDA_BC(lambda));
    ray_t **cpool = (ray_t **)ray_data(LAMBDA_CONSTS(lambda));
    int32_t ip = 0;
    ray_t *vm_err_obj = NULL;

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    do { if (vm.sp >= VM_STACK_SIZE) goto vm_error_limit; vm.rows[vm.sp] = 0; vm.ps[vm.sp++] = (v); } while(0)
#define PUSHR(v,r) do { if (vm.sp >= VM_STACK_SIZE) goto vm_error_limit; vm.rows[vm.sp] = (uint8_t)(r); vm.ps[vm.sp++] = (v); } while(0)
#define POP()      (vm.ps[--vm.sp])
#define POPPED_ROWS() (vm.rows[vm.sp])   /* flag of the value POP() just returned */
#define PEEK()     (vm.ps[vm.sp - 1])
#define LOCAL(s)   (vm.ps[vm.fp + (s)])
#define LROWS(s)   (vm.rows[vm.fp + (s)])

    DISPATCH();

op_loadconst: {
    uint8_t idx = code[ip++];
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadconst_w: {
    uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
    ip += 2;
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadenv: {
    uint8_t slot = code[ip++];
    ray_t *val = LOCAL(slot);
    if (val && ray_is_lazy(val)) {
        /* Force a lazy local to a concrete, reusable value on first read,
         * storing it back into the slot.  A lazy handle is single-use
         * (materialization consumes its deferred graph), so a second read
         * would otherwise see a dead handle — e.g. a lazy first/last bound
         * to a lambda PARAM (which, unlike `let`, is not forced at bind)
         * and then used twice: `((fn [v] (if (> v 0) v 0)) (first xs))`. */
        val = ray_lazy_materialize(val);   /* consumes the slot's ref */
        if (!val || RAY_IS_ERR(val)) {
            vm_err_obj = val ? val : ray_error("type", NULL);
            LOCAL(slot) = NULL;            /* ref already consumed */
            goto vm_error;
        }
        LOCAL(slot) = val;                 /* slot now owns the concrete */
    }
    if (val) ray_retain(val);
    else val = make_i64(0);
    PUSHR(val, LROWS(slot));
    DISPATCH();
}

op_storeenv: {
    uint8_t slot = code[ip++];
    ray_t *val = POP();
    uint8_t val_rows = POPPED_ROWS();
    if (LOCAL(slot)) ray_release(LOCAL(slot));
    LOCAL(slot) = val;
    LROWS(slot) = val_rows;
    DISPATCH();
}

op_pop: {
    if (vm.sp > vm.fp + n_locals) {
        ray_t *val = POP();
        if (val) ray_release(val);
    }
    DISPATCH();
}

op_dup: {
    ray_t *val = PEEK();
    ray_retain(val);
    PUSHR(val, vm.rows[vm.sp - 1]);
    DISPATCH();
}

op_resolve: {
    uint8_t idx = code[ip++];
    ray_t *name_obj = cpool[idx];
    uint8_t val_rows = 0;   /* set when a query helper reads a source column */
    {
        /* The answer cached for this name while nothing it was read from
         * has moved (ray_env_resolve_compiled). */
        ray_resolve_cache_t *e = &g_resolve_cache[ray_resolve_cache_slot(name_obj->i64)];
        if (e->sym == name_obj->i64 && e->fe == g_frame_epoch && e->val &&
            e->ge == atomic_load_explicit(&g_global_epoch, memory_order_relaxed)) {
            ray_retain(e->val);
            PUSHR(e->val, e->rows);
            DISPATCH();
        }
    }
    ray_t *val = ray_env_resolve_compiled(name_obj->i64, &val_rows);
    if (!val) { vm_err_obj = undefined_name_error(name_obj->i64); goto vm_error; }
    /* env_resolve returns an owned ref (rc >= 1); no extra retain needed.
     * It can also return a real error (e.g. nyi from a parted-target link
     * deref inside the dotted walker) — surface that as a VM error rather
     * than pushing it onto the stack as if it were a normal value. */
    if (RAY_IS_ERR(val)) { vm_err_obj = val; goto vm_error; }
    PUSHR(val, val_rows);
    DISPATCH();
}

op_resolve_w: {
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *name_obj = cpool[idx];
    uint8_t val_rows = 0;
    ray_t *val = ray_env_resolve_compiled(name_obj->i64, &val_rows);
    if (!val) { vm_err_obj = undefined_name_error(name_obj->i64); goto vm_error; }
    if (RAY_IS_ERR(val)) { vm_err_obj = val; goto vm_error; }
    PUSHR(val, val_rows);
    DISPATCH();
}

op_storeglobal: {
    uint8_t idx = code[ip++];
    ray_t *name_obj = cpool[idx];
    /* `set` returns its value, so the result must stay on the stack;
     * PEEK rather than POP.  ray_env_set retains its own ref. */
    if (RAY_UNLIKELY(__VM->restricted)) {
        ray_release(POP());
        vm_err_obj = ray_error("access", "restricted");
        goto vm_error;
    }
    ray_t *val = PEEK();
    ray_err_t err = ray_env_set(name_obj->i64, val);
    if (err != RAY_OK) {
        ray_release(POP());
        vm_err_obj = ray_error(ray_err_code_str(err), NULL);
        goto vm_error;
    }
    DISPATCH();
}

op_storeglobal_w: {
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *name_obj = cpool[idx];
    if (RAY_UNLIKELY(__VM->restricted)) {
        ray_release(POP());
        vm_err_obj = ray_error("access", "restricted");
        goto vm_error;
    }
    ray_t *val = PEEK();
    ray_err_t err = ray_env_set(name_obj->i64, val);
    if (err != RAY_OK) {
        ray_release(POP());
        vm_err_obj = ray_error(ray_err_code_str(err), NULL);
        goto vm_error;
    }
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    if (offset < 0 && EVAL_INTERRUPTED()) goto vm_error_cancel;
    DISPATCH();
}

op_jmpfv: {
    /* An `if` condition: a boolean vector chooses per row (OP_IFTREE, the
     * second offset) and stays on the stack; anything else is OP_JMPF. */
    int16_t off_false = (int16_t)((code[ip] << 8) | code[ip + 1]);
    int16_t off_vec = (int16_t)((code[ip + 2] << 8) | code[ip + 3]);
    ip += 4;
    ray_t *cond = PEEK();
    if (cond && cond->type == RAY_BOOL) { ip += off_vec; DISPATCH(); }
    cond = POP();
    if (cond && !ray_is_atom(cond)) __VM->rows_cond++;   /* a vector tested whole */
    int truthy = is_truthy(cond);
    ray_release(cond);
    if (!truthy) ip += off_false;
    DISPATCH();
}

op_iftree: {
    uint16_t site = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    uint8_t start = code[ip + 2];
    int16_t offset = (int16_t)((code[ip + 3] << 8) | code[ip + 4]);
    ip += 5;
    ray_t *pool = LAMBDA_CONSTS(vm.fn);
    ray_t *table = ((ray_t **)ray_data(pool))[pool->len - 1];
    ray_t **te = (ray_t **)ray_data(table);
    ray_t *form = te[2 * (size_t)site];
    ray_t *syms = te[2 * (size_t)site + 1];
    ray_t **fe = (ray_t **)ray_data(form);
    int64_t nargs = form->len - start;   /* the chain: its condition and the rest */
    ray_t *targs_stack[64];
    ray_t **targs = targs_stack;
    ray_t *targs_obj = NULL;
    if (nargs > 64) {
        targs_obj = ray_alloc((size_t)nargs * sizeof(ray_t *));
        if (!targs_obj || RAY_IS_ERR(targs_obj)) { vm_err_obj = ray_error("oom", NULL); goto vm_error; }
        targs = (ray_t **)ray_data(targs_obj);
    }
    ray_t *cond = POP();
    targs[0] = cond;
    for (int64_t i = 1; i < nargs; i++) targs[i] = fe[start + i];
    /* The window binds the locals for the arms, as OP_SCOPE_EVAL does. */
    if (ray_env_scope_bind((const int64_t*)ray_data(syms), (int32_t)syms->len,
                           &vm.ps[vm.fp], &vm.rows[vm.fp], vm.fn) != RAY_OK) {
        ray_release(cond);
        if (targs_obj) ray_release(targs_obj);
        goto vm_error_limit;
    }
    ray_env_mark_eval_window();
    ray_t *res = ray_cond_fn(targs, nargs);
    ray_release(cond);
    if (targs_obj) ray_release(targs_obj);
    if (!res || RAY_IS_ERR(res)) { vm_err_obj = res ? res : ray_error("oom", NULL); goto vm_error; }
    ray_t *before[256];
    int64_t nsync = syms->len < 256 ? syms->len : 256;
    for (int64_t i = 0; i < nsync; i++) before[i] = LOCAL(i);
    ray_env_scope_sync((const int64_t*)ray_data(syms), (int32_t)syms->len, &vm.ps[vm.fp]);
    for (int64_t i = 0; i < nsync; i++) if (LOCAL(i) != before[i]) LROWS(i) = 2;
    PUSHR(res, 0);
    ip += offset;
    DISPATCH();
}

op_jmpf: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *cond = POP();
    if (cond && !ray_is_atom(cond)) {
        /* a vector tested whole, or a condition over a query's rows
         * (vm_cond_test, out of line) */
        ray_t* err = NULL;
        int t = vm_cond_test(cond, &err);   /* consumes cond */
        if (err) { vm_err_obj = err; goto vm_error; }
        if (!t) ip += offset;
        DISPATCH();
    }
    int truthy = is_truthy(cond);
    ray_release(cond);
    if (!truthy) ip += offset;
    DISPATCH();
}

/* The builtin call handlers, as macros: each opcode's handler and its
 * profiled twin (the *_prof labels below, reached when the call profiler
 * is on) differ only in how they apply the builtin.  The twin's entry is
 * the handler's first test, so the hot handler keeps its own registers. */
#define VM_CALL1(APPLY) {                                                   \
    ray_t *arg = POP();                                                     \
    uint8_t arg_rows = POPPED_ROWS();                                       \
    ray_t *fn_obj = POP();                                                  \
    if (RAY_UNLIKELY(fn_is_restricted(fn_obj))) {                           \
        ray_release(arg);                                                   \
        ray_release(fn_obj);                                                \
        vm_err_obj = ray_error("access", "restricted");                     \
        goto vm_error;                                                      \
    }                                                                       \
    /* Materialise lazy args for non-lazy-aware fns.  arg is an owned ref   \
     * (POPped from VM stack); ray_lazy_materialize consumes it. */         \
    if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE) && arg && ray_is_lazy(arg)) {  \
        arg = ray_lazy_materialize(arg);                                    \
        if (!arg || RAY_IS_ERR(arg)) {                                      \
            ray_release(fn_obj);                                            \
            vm_err_obj = arg ? arg : ray_error("type", NULL);               \
            goto vm_error;                                                  \
        }                                                                   \
    }                                                                       \
    RAY_ASSERT_VALUE(arg);                                                  \
    ray_t *result = RAY_UNLIKELY(fn_obj->type == RAY_UNARY &&               \
                                 (fn_obj->attrs & RAY_FN_ROWWISE))          \
        ? rowwise_apply(RAY_UNARY, (void*)(uintptr_t)fn_obj->i64, &arg,     \
                        &arg_rows, 1) : NULL;   /* per row in a query */    \
    bool rowwise_done = result != NULL;                                     \
    if (!result && arg_rows == 1 && fn_obj->type == RAY_UNARY &&            \
        vm_fn_is_edge(fn_obj->i64))   /* first / last (#1673) */            \
        result = ray_query_edge_value((void*)(uintptr_t)fn_obj->i64, arg,   \
                                      arg_rows);                            \
    if (!result) result = APPLY;                                            \
    uint8_t result_rows = rowwise_done ? 1                                  \
        : arg_rows && vm_fn_rowwise(fn_obj) ? arg_rows : 0;                 \
    ray_release(arg);                                                       \
    ray_release(fn_obj);                                                    \
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }        \
    PUSHR(result, result_rows);                                             \
    DISPATCH();                                                             \
}

#define VM_CALL2(APPLY) {                                                   \
    ray_t *right = POP();                                                   \
    uint8_t right_rows = POPPED_ROWS();                                     \
    ray_t *left = POP();                                                    \
    uint8_t left_rows = POPPED_ROWS();                                      \
    ray_t *fn_obj = POP();                                                  \
    if (RAY_UNLIKELY(fn_is_restricted(fn_obj))) {                           \
        ray_release(left);                                                  \
        ray_release(right);                                                 \
        ray_release(fn_obj);                                                \
        vm_err_obj = ray_error("access", "restricted");                     \
        goto vm_error;                                                      \
    }                                                                       \
    uint8_t result_rows = 0;                                                \
    ray_t *result = APPLY;                                                  \
    ray_release(fn_obj);                                                    \
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }        \
    PUSHR(result, result_rows);                                             \
    DISPATCH();                                                             \
}

#define VM_CALLN(APPLY) {                                                   \
    uint8_t n = code[ip++];                                                 \
    if (n > 64) goto vm_error;                                              \
    ray_t *fn_args[64];                                                     \
    uint8_t args_rows = 0;                                                  \
    uint8_t argn_rows[64];                                                  \
    for (int32_t i = n - 1; i >= 0; i--) {                                  \
        fn_args[i] = POP();                                                 \
        argn_rows[i] = POPPED_ROWS();                                       \
        args_rows = vrows_join(args_rows, argn_rows[i]);                    \
    }                                                                       \
    ray_t *fn_obj = POP();                                                  \
    if (RAY_UNLIKELY(fn_is_restricted(fn_obj))) {                           \
        for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);            \
        ray_release(fn_obj);                                                \
        vm_err_obj = ray_error("access", "restricted");                     \
        goto vm_error;                                                      \
    }                                                                       \
    ray_vary_fn fn = (ray_vary_fn)(uintptr_t)fn_obj->i64;                   \
    if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {                             \
        ray_t* err = materialize_owned_args(fn_args, n);                    \
        if (err) { ray_release(fn_obj); vm_err_obj = err; goto vm_error; }  \
    }                                                                       \
    ray_t *result = RAY_UNLIKELY(fn_obj->attrs & RAY_FN_ROWWISE)            \
        ? rowwise_apply(RAY_VARY, (void*)fn, fn_args, argn_rows, n) : NULL; \
    bool rowwise_done = result != NULL;   /* per row in a query (#1594) */  \
    if (!result) result = ray_sig_note(ray_fn_name(fn_obj), APPLY);        \
    uint8_t result_rows = rowwise_done ? 1                                  \
        : args_rows && vm_fn_rowwise(fn_obj) ? args_rows : 0;               \
    for (int32_t i = 0; i < n; i++)                                         \
        ray_release(fn_args[i]);                                            \
    ray_release(fn_obj);                                                    \
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }        \
    PUSHR(result, result_rows);                                             \
    DISPATCH();                                                             \
}

op_call1r:
    /* A builtin a closure body resolved when it ran: OP_CALL1 while the
     * name still holds a unary builtin, the general call (which reads
     * the argument count operand) otherwise, the generic null included,
     * since the general call applies the tree walker's null rule. */
    if (RAY_UNLIKELY(!vm.ps[vm.sp - 2] || vm.ps[vm.sp - 2]->type != RAY_UNARY ||
                     RAY_IS_NULL(vm.ps[vm.sp - 1])))
        goto op_callf;
    ip++;
op_call1:
    if (RAY_UNLIKELY(ray_callprof_on())) goto op_call1_prof;
    VM_CALL1(vm_apply_unary(fn_obj, arg))

op_call2r:
    /* OP_CALL2 for a resolved binary builtin, with the same rows in and
     * out as a baked call; the general call when the name holds anything
     * else. */
    if (RAY_UNLIKELY(!vm.ps[vm.sp - 3] || vm.ps[vm.sp - 3]->type != RAY_BINARY)) goto op_callf;
    ip++;
    if (RAY_UNLIKELY(ray_callprof_on())) goto op_call2_prof;
    VM_CALL2(vm_call_binary(fn_obj, left, right, left_rows, right_rows, &result_rows))

op_call2:
    if (RAY_UNLIKELY(ray_callprof_on())) goto op_call2_prof;
    VM_CALL2(vm_call_binary(fn_obj, left, right, left_rows, right_rows, &result_rows))

op_callnr: {
    /* OP_CALLN for a resolved variadic builtin; the general call when the
     * name holds anything else. */
    uint8_t nr = code[ip];
    if (RAY_UNLIKELY(nr > 64 || !vm.ps[vm.sp - 1 - nr] || vm.ps[vm.sp - 1 - nr]->type != RAY_VARY)) goto op_callf;
    goto op_calln;
}

op_calln:
    if (RAY_UNLIKELY(ray_callprof_on())) goto op_calln_prof;
    VM_CALLN(fn(fn_args, n))

op_pathget: {
    /* A dotted name whose head is a local: the head was loaded as the name
     * alone loads (compile_dotted_local); walk the rest. */
    uint16_t pidx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *phead = POP();
    ray_t *pname = cpool[pidx];
    ray_t *pv = ray_env_path_get(phead, pname->i64);
    ray_release(phead);
    if (RAY_UNLIKELY(!pv)) {
        vm_err_obj = undefined_name_error(pname->i64);
        goto vm_error;
    }
    if (RAY_IS_ERR(pv)) { vm_err_obj = pv; goto vm_error; }
    PUSHR(pv, 0);
    DISPATCH();
}

op_callf: {
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    uint8_t arg_rows[64];
    uint8_t args_any = 0;
    uint64_t args_bits = 0;
    for (int32_t i = n - 1; i >= 0; i--) {
        fn_args[i] = POP();
        arg_rows[i] = POPPED_ROWS();
        args_any = vrows_join(args_any, arg_rows[i]);
        if (arg_rows[i] == 1) args_bits |= UINT64_C(1) << i;
    }
    ray_t *fn_obj = POP();

    /* Compiled lambda: push frame, switch to callee bytecode.  A lambda
     * made inside a query stays on the tree walker (see call_lambda_impl),
     * and a trusted one called by restricted code goes there too, to run
     * lifted in a frame of its own (lambda_lifts). */
    if (fn_obj->type == RAY_LAMBDA && !lambda_lifts(fn_obj)) {
        if (lambda_wants_compile(fn_obj))
            lambda_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* All checks before any VM state mutation.
             * Stack limits take priority over arity (safety first). */
            int64_t pcnt = ray_len(LAMBDA_PARAMS(fn_obj));
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            if (vm.rp >= VM_STACK_SIZE ||
                vm.sp + callee_locals >= VM_STACK_SIZE) {
                for (int32_t i = 0; i < n; i++)
                    if (fn_args[i]) ray_release(fn_args[i]);
                ray_release(fn_obj);
                goto vm_error_limit;
            }
            if (n != pcnt) {
                for (int32_t i = 0; i < n; i++)
                    if (fn_args[i]) ray_release(fn_args[i]);
                ray_release(fn_obj);
                vm_err_obj = ray_error("arity", "expected %" PRId64 " args, got %d", pcnt, n);
                goto vm_error;
            }

            /* Push return frame */
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            vm.sp += callee_locals;
            n_locals = callee_locals;

            /* Bind parameters */
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++) {
                LOCAL(i) = fn_args[i];  /* transfer ownership from args */
                LROWS(i) = arg_rows[i];
            }
            for (int32_t i = (int32_t)bind; i < callee_locals; i++) {
                LOCAL(i) = NULL;
                LROWS(i) = 0;
            }
            for (int64_t i = bind; i < n; i++)
                ray_release(fn_args[i]);  /* excess args */

            /* Check for Ctrl-C interrupt on each compiled call */
            if (EVAL_INTERRUPTED()) goto vm_error_cancel;

            /* Switch to callee bytecode */
            code = (uint8_t *)ray_data(LAMBDA_BC(fn_obj));
            cpool = (ray_t **)ray_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Non-lambda or uncompiled: dispatch by type */
    {
        ray_t *result;
        int8_t lambda_rows = -1;
        bool rowwise_done = false;   /* an element-wise builtin applied per row (#1594) */
        int32_t prof_frame = -1;
        if (RAY_UNLIKELY(ray_callprof_on())) prof_frame = ray_callprof_enter_vm(vm.fn, ip - 1);
        switch (fn_obj->type) {
        case RAY_UNARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            {
            ray_unary_fn unary = (ray_unary_fn)(uintptr_t)fn_obj->i64;
            ray_binary_fn binary = unary_binary_overload(unary);
            if (n == 2 && binary) {
                for (int32_t i = 0; i < 2; i++) {
                    if (fn_args[i] && ray_is_lazy(fn_args[i])) {
                        fn_args[i] = ray_lazy_materialize(fn_args[i]);
                        if (!fn_args[i] || RAY_IS_ERR(fn_args[i])) {
                            result = fn_args[i] ? fn_args[i] : ray_error("type", NULL);
                            fn_args[i] = NULL;
                            ray_release(fn_args[1 - i]);
                            goto unary_done;
                        }
                    }
                }
                result = (is_collection(fn_args[0]) || is_collection(fn_args[1]))
                       ? minmax_map(binary, fn_args[0], fn_args[1])
                       : binary(fn_args[0], fn_args[1]);
                ray_release(fn_args[0]);
                ray_release(fn_args[1]);
                goto unary_done;
            }
            if (n != 1) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_sig_arity(ray_fn_name(fn_obj), "1", n); break; }
            /* fn_args[0] is an owned ref (POPped from VM stack); materialise
             * consumes it — no extra ray_release after the call. */
            if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE) && fn_args[0] && ray_is_lazy(fn_args[0])) {
                fn_args[0] = ray_lazy_materialize(fn_args[0]); /* consumes owned ref */
                if (!fn_args[0] || RAY_IS_ERR(fn_args[0])) { result = fn_args[0] ? fn_args[0] : ray_error("type", NULL); fn_args[0] = NULL; break; }
            }
            if (RAY_UNLIKELY(fn_obj->attrs & RAY_FN_ROWWISE) &&
                (result = rowwise_apply(RAY_UNARY, (void*)(uintptr_t)unary, fn_args, arg_rows, 1)) != NULL) {
                ray_release(fn_args[0]);
                rowwise_done = true;
                break;
            }
            /* first / last of the query's rows skip nulls (#1673) */
            if (arg_rows[0] == 1 && vm_fn_is_edge((int64_t)(uintptr_t)unary) &&
                (result = ray_query_edge_value((void*)(uintptr_t)unary, fn_args[0], arg_rows[0])) != NULL) {
                ray_release(fn_args[0]);
                break;
            }
            /* The generic null: only nil?, type and ser take it, as on the
             * tree walker and in OP_CALL1. */
            if (RAY_UNLIKELY(RAY_IS_NULL(fn_args[0])))
                result = (unary == (ray_unary_fn)ray_nil_fn || unary == (ray_unary_fn)ray_type_fn ||
                          unary == (ray_unary_fn)ray_ser_fn) ? unary(fn_args[0]) : ray_error("type", NULL);
            else if ((fn_obj->attrs & RAY_FN_ATOMIC) && fn_args[0] && fn_args[0]->type >= 0)
                result = atomic_map_unary(unary, fn_args[0]);
            else
                result = unary(fn_args[0]);
            ray_release(fn_args[0]);
unary_done:
            break;
            }
        case RAY_BINARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            if (n > 2 && n <= 64 && (ray_binary_fn)(uintptr_t)fn_obj->i64 == (ray_binary_fn)ray_concat_fn &&
                ray_active_query_table()) {
                /* A query's concat takes several operands: compiled code
                 * joins them left to right with the binary step, as the
                 * interpreter does (#1592). */
                ray_t* acc = fn_args[0];
                uint8_t acc_rows = (uint8_t)(args_bits & 1);
                fn_args[0] = NULL;
                for (int32_t i = 1; i < n; i++) {
                    uint8_t step_rows = 0;
                    ray_t* next = fn_args[i];
                    fn_args[i] = NULL;
                    if (!acc || RAY_IS_ERR(acc)) { ray_release(next); continue; }
                    acc = vm_call_binary(fn_obj, acc, next, acc_rows,
                                         (uint8_t)((args_bits >> i) & 1), &step_rows);
                    acc_rows = step_rows ? 1 : 0;
                }
                result = acc;
                lambda_rows = acc_rows;
                break;
            }
            if (n != 2) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_sig_arity(ray_fn_name(fn_obj), "2", n); break; }
            /* fn_args[0/1] are owned refs (POPped from VM stack); materialise
             * consumes them — no extra ray_release after each call. */
            if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {
                if (fn_args[0] && ray_is_lazy(fn_args[0])) {
                    fn_args[0] = ray_lazy_materialize(fn_args[0]); /* consumes owned ref */
                    if (!fn_args[0] || RAY_IS_ERR(fn_args[0])) { result = fn_args[0] ? fn_args[0] : ray_error("type", NULL); fn_args[0] = NULL; ray_release(fn_args[1]); fn_args[1] = NULL; break; }
                }
                if (fn_args[1] && ray_is_lazy(fn_args[1])) {
                    fn_args[1] = ray_lazy_materialize(fn_args[1]); /* consumes owned ref */
                    if (!fn_args[1] || RAY_IS_ERR(fn_args[1])) { result = fn_args[1] ? fn_args[1] : ray_error("type", NULL); ray_release(fn_args[0]); fn_args[0] = NULL; fn_args[1] = NULL; break; }
                }
            }
            /* The same path as OP_CALL2: the elementwise map for an atomic
             * builtin, the query text rule for concat, the row provenance
             * join.  A closure body reaches here for every builtin it names,
             * since it resolves them when it runs. */
            {
                uint8_t bin_rows = 0;
                result = vm_call_binary(fn_obj, fn_args[0], fn_args[1],
                                        (uint8_t)(args_bits & 1), (uint8_t)((args_bits >> 1) & 1), &bin_rows);
                fn_args[0] = fn_args[1] = NULL;   /* consumed */
                lambda_rows = bin_rows ? 1 : 0;   /* reported below as the result's flag */
            }
            break;
        case RAY_VARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {
                result = materialize_owned_args(fn_args, n);
                if (result) break;
            }
            result = RAY_UNLIKELY(fn_obj->attrs & RAY_FN_ROWWISE)
                ? rowwise_apply(RAY_VARY, (void*)(uintptr_t)fn_obj->i64, fn_args, arg_rows, n) : NULL;
            rowwise_done = result != NULL;
            if (!result) result = ((ray_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        case RAY_LAMBDA: {
            /* An uncompiled lambda (a closure): interpreted, with the
             * arguments' row provenance when a query is active.  A trusted
             * lambda under restriction also comes here, compiled or not. */
            bool in_query = ray_active_query_table() != NULL;
            result = call_lambda_impl(fn_obj, fn_args, n, in_query, args_bits, true, &lambda_rows);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        }
        default:
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            result = head_not_callable_error(fn_obj->type);
            break;
        }
        if (RAY_UNLIKELY(prof_frame >= 0)) ray_callprof_exit(prof_frame);
        uint8_t result_rows = rowwise_done ? 1
            : fn_obj->type == RAY_LAMBDA ? vrows_from(lambda_rows)
            : fn_obj->type == RAY_BINARY ? (uint8_t)(lambda_rows == 1)
            : args_any && (fn_obj->type == RAY_UNARY || fn_obj->type == RAY_VARY) &&
              vm_fn_rowwise(fn_obj) ? args_any : 0;
        ray_release(fn_obj);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSHR(result, result_rows);
        DISPATCH();
    }
}

op_calls: {
    /* Self-recursive call — lean path matching rayforce 1.
     * No fn object on stack. Args are already at sp-argc..sp.
     * Push return frame, set fp so args become locals, extend for extra locals. */
    uint8_t argc = code[ip++];

    /* Stack overflow guard */
    if (RAY_UNLIKELY(vm.rp >= VM_STACK_SIZE)) goto vm_error_limit;
    if (RAY_UNLIKELY(vm.sp + n_locals >= VM_STACK_SIZE)) goto vm_error_limit;

    /* Push return frame (fn=NULL signals self-call to OP_RET) */
    vm.rs[vm.rp++] = (vm_ctx_t){ .fn = NULL, .fp = vm.fp, .ip = ip };

    /* Args on stack become the new frame's first locals.
     * Compiler guarantees argc == param count, so argc <= n_locals. */
    vm.fp = vm.sp - argc;

    /* Extend stack for extra locals beyond params (let bindings etc.) */
    for (int32_t i = argc; i < n_locals; i++) {
        vm.rows[vm.sp] = 0;
        vm.ps[vm.sp++] = NULL;
    }

    ip = 0;
    DISPATCH();
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via ray_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        ray_t *ast = POP();
        ray_t *result = RAY_UNLIKELY(ray_callprof_on()) ? vm_prof_eval(ast, vm.fn, ip - 1) : ray_eval(ast);
        ray_release(ast);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSHR(result, vrows_from(g_eval_rows));
        DISPATCH();
    }
    /* n>0: build call list and eval */
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    ray_t *call_list = ray_alloc((n + 1) * sizeof(ray_t *));
    if (!call_list || RAY_IS_ERR(call_list)) {
        for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
        ray_release(fn_obj);
        goto vm_error;
    }
    call_list->type = RAY_LIST;
    call_list->len = n + 1;
    ray_t **elems = (ray_t **)ray_data(call_list);
    elems[0] = fn_obj;
    for (int32_t i = 0; i < n; i++)
        elems[i + 1] = fn_args[i];

    ray_t *result = RAY_UNLIKELY(ray_callprof_on()) ? vm_prof_eval(call_list, vm.fn, ip - 1)
                                                    : ray_eval(call_list);
    ray_release(call_list);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSHR(result, vrows_from(g_eval_rows));
    DISPATCH();
}

op_ret: {
    ray_t *result;
    uint8_t result_rows = 0;
    bool from_stack = (vm.sp > vm.fp + n_locals);
    if (from_stack) {
        result = POP();
        result_rows = POPPED_ROWS();
        ray_retain(result);  /* prevent free during cleanup if aliased in locals */
    } else {
        result = RAY_NULL_OBJ;
    }

    /* Clean up current frame — release all locals and leftover stack slots */
    while (vm.sp > vm.fp) {
        ray_t *v = vm.ps[--vm.sp];
        if (v) ray_release(v);
    }
    /* Undo protective retain — POP's reference is the caller's ownership */
    if (from_stack) ray_release(result);

    if (vm.rp == 0) {
        /* Top-level return */
        if (out_rows) *out_rows = result_rows;
        ray_release(vm.fn);
        __VM->lambda_depth--;
#undef vm
        ray_free(vm_block);
        return result;  /* caller owns the POP'd reference */
#define vm (*vmp)
    }

    /* Pop return frame */
    vm.rp--;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    if (vm.rs[vm.rp].fn) {
        /* Normal call: restore caller's function */
        ray_release(vm.fn);
        vm.fn = vm.rs[vm.rp].fn;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        n_locals = LAMBDA_NLOCALS(vm.fn);
    }
    /* Self-call (fn==NULL): vm.fn/code/cpool/n_locals are already correct */
    PUSHR(result, result_rows);
    DISPATCH();
}

op_trapv:   /* the handler is a plain value: it reads no error value */
op_trap: {
    bool value = code[ip - 1] == OP_TRAPV;
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    if (vm.tp >= VM_TRAP_SIZE) goto vm_error_limit;
    vm.ts[vm.tp++] = (vm_trap_t){
        .rp = vm.rp, .sp = vm.sp, .handler_ip = ip + offset,
        .fn = vm.fn, .fp = vm.fp, .n_locals = n_locals,
        .scope_depth = ray_env_scope_depth(), .value = value
    };
    ray_retain(vm.fn);
    DISPATCH();
}

op_trap_end: {
    if (vm.tp > 0) {
        vm.tp--;
        ray_release(vm.ts[vm.tp].fn);
    }
    DISPATCH();
}

op_tryh: {
    /* Stack: [.., handler, err_val] (err_val on top).  A callable handler
     * is invoked with the error; any other value is the fallback result. */
    ray_t* err_val = POP();
    ray_t* handler = POP();
    uint8_t handler_rows = POPPED_ROWS();
    int8_t result_rows = -1;
    ray_t* result = try_handle_rows(handler, err_val, vrows_to(handler_rows), &result_rows);  /* borrows both */
    ray_release(err_val);
    ray_release(handler);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSHR(result, vrows_from(result_rows));
    DISPATCH();
}

op_bindcaps: {
    /* The closure's captures become local slots (the compiled closure's
     * prologue): cpool[idx] holds (name, slot) pairs; each name is looked
     * up in the closure dict.  A name the dict lacks (a `let` that had not
     * run when the closure was made) leaves its slot empty, so a read of
     * it falls back to the global, as on the tree walker.  The capture's
     * row flag applies while the query frame the closure was made under
     * is the innermost one. */
    uint8_t idx = code[ip++];
    ray_t *pairs = cpool[idx];
    ray_t *cap = LAMBDA_CLOSURE(vm.fn);
    ray_t *ckeys = cap && cap->type == RAY_DICT ? ray_dict_keys(cap) : NULL;
    ray_t *cvals = cap && cap->type == RAY_DICT ? ray_dict_vals(cap) : NULL;
    if (!pairs || pairs->type != RAY_I64) goto vm_error_name;
    if (cap && (!ckeys || !cvals || ckeys->type != RAY_SYM || cvals->type != RAY_LIST || ckeys->len != cvals->len))
        goto vm_error_name;
    int64_t ncap = ckeys ? ckeys->len : 0;
    ray_t *flags = LAMBDA_CAPROWS(vm.fn);
    bool live = flags && flags->len == ncap && LAMBDA_CAPQ(vm.fn) &&
                LAMBDA_CAPQ(vm.fn) == ray_env_query_serial();
    const int64_t *pv = (const int64_t*)ray_data(pairs);
    for (int64_t i = 0, guess = 0; i + 1 < pairs->len; i += 2, guess++) {
        int64_t name = pv[i], slot = pv[i + 1];
        int64_t at = -1;
        if (guess < ncap && ray_read_sym(ray_data(ckeys), guess, RAY_SYM, ckeys->attrs) == name) at = guess;
        for (int64_t k = 0; at < 0 && k < ncap; k++)
            if (ray_read_sym(ray_data(ckeys), k, RAY_SYM, ckeys->attrs) == name) at = k;
        if (LOCAL(slot)) { ray_release(LOCAL(slot)); LOCAL(slot) = NULL; }
        LROWS(slot) = 0;
        if (at < 0) continue;
        ray_t *val = ((ray_t**)ray_data(cvals))[at];
        if (!val) continue;
        ray_retain(val);
        LOCAL(slot) = val;
        LROWS(slot) = live ? ((uint8_t*)ray_data(flags))[at] : 0;
    }
    DISPATCH();
}

op_mkclosure: {
    /* Stack: [.., template, names, slots].  A closure of the template:
     * it shares the template's bytecode, constants, debug map and locals
     * count, and captures the current frame's slots named by `names`, with
     * their row flags, which apply while the innermost query frame is the
     * one of now.  A slot still empty (its `let` has not run) is not
     * captured, as the tree walker would not bind it either; a lazy slot
     * is materialized first, into the slot too: a lazy handle is single-use
     * (materializing frees its graph in place), and the dict would hold a
     * dead handle after the closure's first call.  Rayfall text does not
     * reach here with one (call sites materialize lazy arguments), but a
     * caller of the C API can. */
    ray_t *slots = POP();
    ray_t *names = POP();
    ray_t *tmpl  = POP();
    if (!tmpl || tmpl->type != RAY_LAMBDA || !names || names->type != RAY_SYM ||
        !slots || slots->type != RAY_I64 || names->len != slots->len) {
        ray_release(slots); ray_release(names); ray_release(tmpl);
        goto vm_error;
    }
    int64_t ncap = names->len;
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, ncap > 0 ? ncap : 1);
    ray_t *vals = ray_list_new(ncap > 0 ? ncap : 1);
    ray_t *flags = ray_vec_new(RAY_U8, ncap > 0 ? ncap : 1);
    ray_t *lam = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals) || !flags || RAY_IS_ERR(flags) || !lam || RAY_IS_ERR(lam)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        if (flags && !RAY_IS_ERR(flags)) ray_release(flags);
        if (lam && !RAY_IS_ERR(lam)) { lam->type = -RAY_I64; ray_release(lam); }
        ray_release(slots); ray_release(names); ray_release(tmpl);
        vm_err_obj = ray_error("oom", NULL);
        goto vm_error;
    }
    keys->len = 0; flags->len = 0;
    bool any_flag = false;
    for (int64_t i = 0; i < ncap; i++) {
        int64_t slot = ((const int64_t*)ray_data(slots))[i];
        if (slot < 0 || slot >= n_locals) continue;
        ray_t *v = LOCAL(slot);
        if (!v) continue;
        if (ray_is_lazy(v)) {
            v = ray_lazy_materialize(v);   /* consumes the slot's ref */
            LOCAL(slot) = v && !RAY_IS_ERR(v) ? v : NULL;
            if (!v || RAY_IS_ERR(v)) {
                ray_release(keys); ray_release(vals); ray_release(flags);
                lam->type = -RAY_I64; ray_release(lam);
                ray_release(slots); ray_release(names); ray_release(tmpl);
                vm_err_obj = v ? v : ray_error("type", NULL);
                goto vm_error;
            }
        }
        ((int64_t*)ray_data(keys))[keys->len++] = ray_read_sym(ray_data(names), i, RAY_SYM, names->attrs);
        vals = ray_list_append(vals, v);
        uint8_t r = LROWS(slot) == 1 ? 1 : 0;
        ((uint8_t*)ray_data(flags))[flags->len++] = r;
        if (r) any_flag = true;
    }
    lam->type = RAY_LAMBDA;
    lam->attrs = tmpl->attrs;
    lam->len = 0;
    memset(ray_data(lam), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    ray_retain(LAMBDA_PARAMS(tmpl)); LAMBDA_PARAMS(lam) = LAMBDA_PARAMS(tmpl);
    ray_retain(LAMBDA_BODY(tmpl));   LAMBDA_BODY(lam)   = LAMBDA_BODY(tmpl);
    if (LAMBDA_BC(tmpl))     { ray_retain(LAMBDA_BC(tmpl));     LAMBDA_BC(lam)     = LAMBDA_BC(tmpl); }
    if (LAMBDA_CONSTS(tmpl)) { ray_retain(LAMBDA_CONSTS(tmpl)); LAMBDA_CONSTS(lam) = LAMBDA_CONSTS(tmpl); }
    if (LAMBDA_NFO(tmpl))    { ray_retain(LAMBDA_NFO(tmpl));    LAMBDA_NFO(lam)    = LAMBDA_NFO(tmpl); }
    if (LAMBDA_DBG(tmpl))    { ray_retain(LAMBDA_DBG(tmpl));    LAMBDA_DBG(lam)    = LAMBDA_DBG(tmpl); }
    LAMBDA_NLOCALS(lam) = LAMBDA_NLOCALS(tmpl);
    if (keys->len > 0) {
        LAMBDA_CLOSURE(lam) = ray_dict_new(keys, vals);   /* consumes both */
        if (!LAMBDA_CLOSURE(lam) || RAY_IS_ERR(LAMBDA_CLOSURE(lam))) {
            LAMBDA_CLOSURE(lam) = NULL; ray_release(flags); ray_release(lam);
            ray_release(slots); ray_release(names); ray_release(tmpl);
            vm_err_obj = ray_error("oom", NULL);
            goto vm_error;
        }
    } else { ray_release(keys); ray_release(vals); }
    if (any_flag) { LAMBDA_CAPROWS(lam) = flags; LAMBDA_CAPQ(lam) = ray_env_query_serial(); }
    else ray_release(flags);
    ray_release(slots); ray_release(names); ray_release(tmpl);
    PUSHR(lam, 0);
    DISPATCH();
}

op_loadenvg: {
    /* A `let` slot that may still be empty: the local when bound, else
     * the global of that name (a name error when there is none), as the
     * tree walker resolves a name whose `let` has not run. */
    uint8_t slot = code[ip++];
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *val = LOCAL(slot);
    if (val) {
        if (ray_is_lazy(val)) {
            val = ray_lazy_materialize(val);
            if (!val || RAY_IS_ERR(val)) { vm_err_obj = val ? val : ray_error("type", NULL); LOCAL(slot) = NULL; goto vm_error; }
            LOCAL(slot) = val;
        }
        ray_retain(val);
        PUSHR(val, LROWS(slot));
        DISPATCH();
    }
    ray_t *name_obj = cpool[idx];
    uint8_t val_rows = 0;
    val = ray_env_resolve_compiled(name_obj->i64, &val_rows);
    if (!val) { vm_err_obj = undefined_name_error(name_obj->i64); goto vm_error; }
    if (RAY_IS_ERR(val)) { vm_err_obj = val; goto vm_error; }
    PUSHR(val, val_rows);
    DISPATCH();
}

op_loadself: {
    ray_retain(vm.fn);
    PUSHR(vm.fn, 0);
    DISPATCH();
}

op_force: {
    /* Materialize a lazy TOS so a let-bound local holds a concrete,
     * reusable value (a lazy handle is single-use). */
    ray_t* v = POP();
    uint8_t v_rows = POPPED_ROWS();
    if (v && ray_is_lazy(v)) {
        v = ray_lazy_materialize(v);   /* consumes; concrete or error */
        if (!v || RAY_IS_ERR(v)) { vm_err_obj = v ? v : ray_error("type", NULL); goto vm_error; }
    }
    PUSHR(v, v_rows);
    DISPATCH();
}

op_scope_eval:
op_scope_begin: {
    /* Stack: [.., syms_vec] — materialize the live locals (slot i holds
     * the value of syms[i]) plus self into a fresh scope frame so the
     * OP_CALLD that follows can resolve them via the tree walker. */
    bool eval_window = code[ip - 1] == OP_SCOPE_EVAL;
    ray_t *syms = POP();
    /* A lazy handle is single-use, so every other way of reaching a local
     * forces it (op_loadenv, op_loadenvg, a closure's capture, the tree
     * walker's lambda call).  The frame's slots are bound as they are, and a
     * lazy-aware call inside the window (count, first, distinct, ...) appends
     * to the handle in place and then consumes it: the slot would be left
     * holding a dead handle, and the next read of that local, in the window or
     * after it, fails with "lazy handle already materialized" (#1860).  Force
     * the lazy locals into their slots first, as op_loadenv does for one. */
    for (int64_t i = 0; i < syms->len; i++) {
        ray_t *lv = LOCAL(i);
        if (((const int64_t*)ray_data(syms))[i] < 0 || !lv || !ray_is_lazy(lv)) continue;
        lv = ray_lazy_materialize(lv);   /* consumes the slot's ref */
        if (!lv || RAY_IS_ERR(lv)) {
            vm_err_obj = lv ? lv : ray_error("type", NULL);
            LOCAL(i) = NULL;             /* ref already consumed */
            ray_release(syms);
            goto vm_error;
        }
        LOCAL(i) = lv;
    }
    /* The locals' row provenance travels with the bindings, so the tree
     * walker inside the window sees it as it sees any local's. */
    ray_err_t err = ray_env_scope_bind((const int64_t*)ray_data(syms),
                                       (int32_t)syms->len,
                                       &vm.ps[vm.fp], &vm.rows[vm.fp], vm.fn);
    ray_release(syms);
    if (err != RAY_OK) goto vm_error_limit;
    /* An eval window serves the evaluated text: a lambda that text calls
     * resolves its free names past it (ray_env_resolve_compiled). */
    if (eval_window) ray_env_mark_eval_window();
    /* A closure's captures sit in local slots (bound by the prologue), so
     * the window binds them with the other locals, and nothing from the
     * caller's frames can stand in for one. */
    DISPATCH();
}

op_scope_end: {
    /* Stack: [.., calld_result, syms_vec] — sync the frame back into the
     * slots and pop it; the dynamic eval's result stays on the stack. */
    ray_t *syms = POP();
    /* A local rebound inside the window has unknown provenance: clear its
     * flag, as for any value the VM did not derive itself. */
    ray_t *before[256];   /* a frame has at most 256 slots (compile.c add_local) */
    int64_t nsync = syms->len < 256 ? syms->len : 256;
    for (int64_t i = 0; i < nsync; i++) before[i] = LOCAL(i);
    ray_env_scope_sync((const int64_t*)ray_data(syms),
                       (int32_t)syms->len, &vm.ps[vm.fp]);
    for (int64_t i = 0; i < nsync; i++) if (LOCAL(i) != before[i]) LROWS(i) = 2;
    ray_release(syms);
    DISPATCH();
}

/* The call handlers' profiled twins (the call profiler is on).  `ip`
 * is inside the call instruction, which is all the site key needs. */
op_call1_prof:
    VM_CALL1(vm_prof_unary(fn_obj, arg, vm.fn, ip - 1))
op_call2_prof:
    VM_CALL2(vm_prof_binary(fn_obj, left, right, left_rows, right_rows, &result_rows, vm.fn, ip - 1))
op_calln_prof:
    VM_CALLN(vm_prof_vary(fn, fn_args, n, vm.fn, ip - 1))
#undef VM_CALL1
#undef VM_CALL2
#undef VM_CALLN

    const char *vm_err_str = "domain";
    const char *vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error_limit:
    vm_err_str = "limit";
    vm_err_detail = "stack overflow";
    goto vm_error_cleanup;

vm_error_cancel:
    vm_err_str = "cancel";
    vm_err_detail = "interrupted";
    goto vm_error_cleanup;

vm_error_name:
    vm_err_str = "name";
    vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error:
    vm_err_str = "domain";
    vm_err_detail = NULL;

vm_error_cleanup: {
    /* A `return` evaluated by the tree walker inside this frame (a window
     * around a special form) arrives as the return marker: it is this
     * frame's return, not an error.  Traps armed in this frame and the
     * window's scope frames go with it. */
    if (vm_err_obj && is_return_marker(vm_err_obj)) {
        ray_t *rv = ray_take_return(vm_err_obj);
        vm_err_obj = NULL;
        while (vm.tp > 0 && vm.ts[vm.tp - 1].rp >= vm.rp) {
            vm.tp--;
            ray_release(vm.ts[vm.tp].fn);
        }
        while (ray_env_scope_depth() > scope_base)
            ray_env_pop_scope();
        PUSHR(rv, 0);
        goto op_ret;
    }
    /* Check for trap frame */
    if (vm.tp > 0) {
        vm.tp--;
        vm_trap_t trap = vm.ts[vm.tp];

        /* Clean up return frames above trap point */
        while (vm.rp > trap.rp) {
            vm.rp--;
            if (vm.rs[vm.rp].fn) ray_release(vm.rs[vm.rp].fn);
        }

        /* Clean up stack above trap point */
        while (vm.sp > trap.sp) {
            ray_t *v = vm.ps[--vm.sp];
            if (v) ray_release(v);
        }

        /* Unwind scope frames materialized after the trap was armed
         * (an error inside an OP_SCOPE_BEGIN..END window skips its
         * OP_SCOPE_END) */
        while (ray_env_scope_depth() > trap.scope_depth)
            ray_env_pop_scope();

        /* Get error value.  If __VM->raise_val is set, a user (raise x)
         * just ran — its value is the real payload the handler must
         * see.  Otherwise the handler gets the error's code and message,
         * as ray_try_fn gives it: vm_err_obj's when there is one, else
         * the VM's own (vm_err_str, vm_err_detail), the error an untrapped
         * exit would make.  A trap armed by OP_TRAPV, whose handler is a
         * plain value, takes none.  vm_err_obj, the error itself (the
         * sentinel ray_raise_fn returns, or a builtin's or the VM's own),
         * goes no further: errors are not refcounted, so ray_release would
         * leave it allocated. */
        ray_t *err_val = __VM->raise_val;
        __VM->raise_val = NULL;
        if (trap.value) {
            ray_release(err_val);
            err_val = NULL;
        } else if (!err_val)
            err_val = vm_err_obj && RAY_IS_ERR(vm_err_obj)
                ? try_error_value(ray_err_code(vm_err_obj), ray_err_msg(vm_err_obj), ray_error_usage())
                : try_error_value(vm_err_str, vm_err_detail, NULL);
        if (vm_err_obj) {
            if (RAY_IS_ERR(vm_err_obj)) ray_error_free(vm_err_obj);
            else ray_release(vm_err_obj);
        }
        vm_err_obj = NULL;
        ray_clear_error_trace();   /* frames an inner call's unwind recorded */

        /* Restore context and push error value */
        ray_release(vm.fn);
        vm.fn = trap.fn;  /* takes ownership from trap frame */
        vm.fp = trap.fp;
        n_locals = trap.n_locals;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        ip = trap.handler_ip;
        if (!trap.value) PUSH(err_val);
        DISPATCH();
    }

    /* No trap frame — regular error cleanup */

    /* Unwind any scope frames this exec materialized and didn't end */
    while (ray_env_scope_depth() > scope_base)
        ray_env_pop_scope();

    /* Build error trace: current frame + callers from return stack */
    add_error_frame(vm.fn, ip > 0 ? ip - 1 : 0);
    for (int32_t i = vm.rp - 1; i >= 0; i--) {
        if (vm.rs[i].fn)
            add_error_frame(vm.rs[i].fn, vm.rs[i].ip > 0 ? vm.rs[i].ip - 1 : 0);
    }

    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) ray_release(vm.ps[i]);
    ray_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        if (vm.rs[i].fn) ray_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        ray_release(vm.ts[i].fn);
    __VM->lambda_depth--;
#undef vm
    ray_free(vm_block);
    if (vm_err_obj)
        return vm_err_obj;
    if (vm_err_detail)
        return ray_error(vm_err_str, "%s", vm_err_detail);
    return ray_error(vm_err_str, NULL);
}

#undef DISPATCH
#undef PUSH
#undef PUSHR
#undef POP
#undef POPPED_ROWS
#undef PEEK
#undef LOCAL
#undef LROWS
#undef vm
}


/* ray_enlist_fn, ray_dict_fn, ray_nil_fn, ray_where_fn, ray_group_indices_fn,
 * ray_concat_fn, ray_raze_fn, ray_within_fn
 * moved to ops/builtins.c */

/* ══════════════════════════════════════════
 * Builtin registration
 * ══════════════════════════════════════════ */

/* Bind `obj` under `name` in the global env.  For reserved-namespace
 * names like `.sys.gc`:
 *
 *   - `.sys` itself is a RAY_DICT in the env (keys SYM vec + vals
 *     LIST).  Typing `.sys` at the REPL returns the whole dict for
 *     introspection.
 *   - `.sys.gc` is ALSO bound flat in the env, pointing at the same
 *     function object.  This keeps direct lookup O(1), surfaces the
 *     full name to `ray_env_lookup_prefix` (so tab completion and
 *     REPL highlighting continue to see every reserved builtin),
 *     and lets error messages cite the fully-qualified name.
 *
 * The two bindings are created at startup and kept in sync — writes
 * to any `.`-prefixed name are refused by ray_env_set, so user code
 * can't drift them apart.  Only 2-level namespaces are in use; the
 * assert below guards against silent breakage if that changes. */
/* Builtin registration runs once at startup with no caller to report to, so
 * a failure is fatal.  It is checked explicitly and never inside assert():
 * an -DNDEBUG build compiles an assert away, work and all — the binds below
 * once lived inside one, and such a build started with no builtins (#652). */
static void reg_check(bool ok, const char* name, const char* what) {
    if (ok) return;
    fprintf(stderr, "rayforce: cannot register builtin %s: %s\n",
            name ? name : "(namespace)", what);
    abort();
}

/* Get-or-create a child dict by key on `parent`.  ray_dict_get
 * returns an owned ref (or NULL if missing), so we either reuse it
 * (after type-checking) or build a fresh subdict and upsert it.
 * Returns the (possibly-COWd) parent; the child is handed back via
 * `*out_child`, owned by the caller. */
static ray_t* dict_get_or_create_subdict(ray_t* parent, ray_t* key,
                                         ray_t** out_child) {
    ray_t* existing = ray_dict_get(parent, key);
    if (existing && !RAY_IS_ERR(existing) && existing->type == RAY_DICT) {
        *out_child = existing;
        return parent;
    }
    if (existing) ray_release(existing);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vals = ray_list_new(4);
    reg_check(keys && !RAY_IS_ERR(keys) && vals && !RAY_IS_ERR(vals),
              NULL, "out of memory");
    ray_t* child = ray_dict_new(keys, vals);
    reg_check(child && !RAY_IS_ERR(child), NULL, "out of memory");
    ray_retain(child);  /* caller retains; dict_upsert below also retains */
    parent = ray_dict_upsert(parent, key, child);
    *out_child = child;
    return parent;
}

static void reg_bind(const char* name, ray_t* obj) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    if (name[0] == '.' && ray_sym_is_dotted(sym)) {
        const int64_t* segs;
        int nsegs = ray_sym_segs(sym, &segs);
        assert(nsegs >= 2 && "reg_bind: dotted reserved name must have ≥ 2 segments");

        int64_t root_sym = segs[0];      /* e.g. `.sys` or `.db` */
        int64_t leaf_sym = segs[nsegs-1];/* leaf action sym */

        /* 1. Get-or-create the root dict bound at `.<ns>`. */
        ray_t* root = ray_env_get(root_sym);
        if (root) {
            ray_retain(root);
        } else {
            ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
            ray_t* vals = ray_list_new(4);
            reg_check(keys && !RAY_IS_ERR(keys) && vals && !RAY_IS_ERR(vals),
                      name, "out of memory");
            root = ray_dict_new(keys, vals);
            reg_check(root && !RAY_IS_ERR(root), name, "out of memory");
        }

        /* 2. For each intermediate segment, descend into (or create)
         *    a sub-dict.  Two-level names skip this loop entirely
         *    and fall through to the leaf upsert below.  After the
         *    walk, `cur` points at the dict that should hold the
         *    leaf; `chain[]` records the parents we still need to
         *    write back so a COW upsert at the deepest level
         *    propagates upward through every parent. */
        enum { MAX_DEPTH = 4 };
        ray_t* chain[MAX_DEPTH] = { root };
        int64_t chain_keys[MAX_DEPTH] = { 0 };
        int chain_len = 1;
        ray_t* cur = root;
        for (int i = 1; i < nsegs - 1; i++) {
            assert(chain_len < MAX_DEPTH);
            ray_t* mid_key = ray_sym(segs[i]);
            ray_t* child = NULL;
            cur = dict_get_or_create_subdict(cur, mid_key, &child);
            ray_release(mid_key);
            chain[chain_len - 1] = cur;
            chain_keys[chain_len - 1] = segs[i];
            chain[chain_len++] = child;
            cur = child;
        }

        /* 3. Upsert the leaf into the deepest dict, then walk back up
         *    re-upserting any COWd parents into their parents. */
        ray_t* leaf_key = ray_sym(leaf_sym);
        ray_t* deepest = ray_dict_upsert(cur, leaf_key, obj);
        ray_release(leaf_key);
        chain[chain_len - 1] = deepest;
        for (int i = chain_len - 1; i > 0; i--) {
            ray_t* parent_key = ray_sym(chain_keys[i - 1]);
            chain[i - 1] = ray_dict_upsert(chain[i - 1], parent_key, chain[i]);
            ray_release(parent_key);
            ray_release(chain[i]);  /* dict_upsert retained */
        }

        /* 4. Bind the (possibly-COWd) root and the flat fully-qualified
         *    name so ray_env_lookup_prefix (REPL completion / syntax
         *    highlighting) enumerates every reserved builtin by name.
         *    ray_env_bind_flat skips the dotted-walk so this doesn't
         *    re-upsert into the same dict we just built. */
        reg_check(ray_env_bind(root_sym, chain[0]) == RAY_OK, name,
                  "binding its namespace root failed");
        ray_release(chain[0]);
        reg_check(ray_env_bind_flat(sym, obj) == RAY_OK, name, "binding failed");
        return;
    }
    reg_check(ray_env_bind(sym, obj) == RAY_OK, name, "binding failed");
}

static void register_binary(const char* name, uint8_t attrs, ray_binary_fn fn) {
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

/* Register binary with a DAG opcode for vectorized execution */
static void register_binary_op(const char* name, uint8_t attrs, ray_binary_fn fn, uint16_t opcode) {
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    RAY_FN_SET_OPCODE(obj, opcode);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_unary(const char* name, uint8_t attrs, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_unary_op(const char* name, uint8_t attrs, ray_unary_fn fn, uint16_t opcode) {
    ray_t* obj = ray_fn_unary(name, attrs, fn);
    RAY_FN_SET_OPCODE(obj, opcode);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_vary(const char* name, uint8_t attrs, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

static void ray_register_builtins(void) {
    register_binary_op("+",   RAY_FN_ATOMIC, ray_add_fn, OP_ADD);
    register_binary_op("-",   RAY_FN_ATOMIC, ray_sub_fn, OP_SUB);
    register_binary_op("*",   RAY_FN_ATOMIC, ray_mul_fn, OP_MUL);
    register_binary_op("/",   RAY_FN_ATOMIC, ray_div_fn, OP_DIV);
    register_binary_op("%",   RAY_FN_ATOMIC, ray_mod_fn, OP_MOD);
    register_binary_op(">",   RAY_FN_ATOMIC, ray_gt_fn,  OP_GT);
    register_binary_op("<",   RAY_FN_ATOMIC, ray_lt_fn,  OP_LT);
    register_binary_op(">=",  RAY_FN_ATOMIC, ray_gte_fn,    OP_GE);
    register_binary_op("<=",  RAY_FN_ATOMIC, ray_lte_fn,    OP_LE);
    register_binary_op("==",  RAY_FN_ATOMIC, ray_eq_fn,  OP_EQ);
    register_binary_op("!=",  RAY_FN_ATOMIC, ray_neq_fn,    OP_NE);
    /* Special-form so args are passed unevaluated and the kernel can
     * short-circuit on the first determining scalar (matches v1 and the
     * Lisp/Clojure convention). */
    register_vary("and", RAY_FN_SPECIAL_FORM, ray_and_vary_fn);
    register_vary("or",  RAY_FN_SPECIAL_FORM, ray_or_vary_fn);
    register_unary_op("not",  RAY_FN_NONE,   ray_not_fn, OP_NOT);
    register_unary_op("neg",  RAY_FN_ATOMIC, ray_neg_fn, OP_NEG);
    register_unary("round",   RAY_FN_ATOMIC, ray_round_fn);
    register_unary_op("floor", RAY_FN_ATOMIC, ray_floor_fn, OP_FLOOR);
    register_unary_op("ceil",  RAY_FN_ATOMIC, ray_ceil_fn,  OP_CEIL);
    register_unary_op("abs",   RAY_FN_ATOMIC, ray_abs_fn,  OP_ABS);
    register_unary_op("sqrt",  RAY_FN_ATOMIC, ray_sqrt_fn, OP_SQRT);
    register_unary_op("log",   RAY_FN_ATOMIC, ray_log_fn,  OP_LOG);
    register_unary_op("exp",   RAY_FN_ATOMIC, ray_exp_fn,  OP_EXP);
    register_unary_op("sin",        RAY_FN_NONE, ray_sin_fn,        OP_SIN);
    register_unary_op("asin",       RAY_FN_NONE, ray_asin_fn,       OP_ASIN);
    register_unary_op("cos",        RAY_FN_NONE, ray_cos_fn,        OP_COS);
    register_unary_op("acos",       RAY_FN_NONE, ray_acos_fn,       OP_ACOS);
    register_unary_op("tan",        RAY_FN_NONE, ray_tan_fn,        OP_TAN);
    register_unary_op("atan",       RAY_FN_NONE, ray_atan_fn,       OP_ATAN);
    register_unary_op("reciprocal", RAY_FN_NONE, ray_reciprocal_fn, OP_RECIPROCAL);
    register_unary_op("signum",     RAY_FN_NONE, ray_signum_fn,     OP_SIGNUM);
    register_binary_op("pow", RAY_FN_ATOMIC, ray_pow_fn, OP_POW);
    /* Partial-sort top/bottom-N (topk_take_vec): O(N + K log K) via the
     * selection topk_indices_single runs, on the pool for long vectors (a
     * comparator heap for SYM/STR/GUID); falls back to full sort + take
     * when it declines (K >= N, unsupported types).  Per-group usage
     * works through the eval-level scatter. */
    register_binary("top", RAY_FN_AGGR, ray_top_fn);
    register_binary("bot", RAY_FN_AGGR, ray_bot_fn);
    /* pearson_corr: 2-input scalar reducer.  Marked AGGR + LAZY_AWARE so
     * the planner picks it up via is_streaming_aggr_binary_call and lowers
     * a `(pearson_corr x y)` reference inside `(select ... by ...)` to an
     * OP_PEARSON_CORR DAG node — single-pass vectorized hash-agg.  The
     * ray_pearson_corr_fn body remains the fallback for non-vectorizable
     * shapes (LIST inputs, eval-level scatter on unsupported key types). */
    /* NOT lazy-aware: ray_pearson_corr_fn has no lazy branch (no
     * lazy_append shell), so the flag made eval hand it raw lazy handles
     * that it rejected with a bogus "expects two vectors" type error.
     * Without the flag eval materializes lazy args before the call. */
    register_binary("pearson_corr", RAY_FN_AGGR, ray_pearson_corr_fn);
    register_binary("cov",  RAY_FN_AGGR, ray_cov_fn);
    register_binary("scov", RAY_FN_AGGR, ray_scov_fn);
    register_binary("wsum", RAY_FN_AGGR, ray_wsum_fn);
    register_binary("wavg", RAY_FN_AGGR, ray_wavg_fn);
    register_binary("quantile",   RAY_FN_AGGR, ray_quantile_fn);
    register_binary("percentile", RAY_FN_AGGR, ray_percentile_fn);

    /* Special forms */
    register_binary("set", RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_set_fn);
    register_binary("let", RAY_FN_SPECIAL_FORM, ray_let_fn);
    register_vary("if",    RAY_FN_SPECIAL_FORM, ray_cond_fn);
    register_vary("do",    RAY_FN_SPECIAL_FORM, ray_do_fn);
    register_vary("while", RAY_FN_SPECIAL_FORM, ray_while_fn);
    register_vary("times", RAY_FN_SPECIAL_FORM, ray_times_fn);
    register_vary("fn",    RAY_FN_SPECIAL_FORM, ray_fn);

    /* Aggregation builtins */
    /* RAY_FN_LAZY_AWARE: these fns have 'if (ray_is_lazy(x)) return ray_lazy_append(...)' shells
     * and must NOT have their args materialised by the dispatcher. */
    register_unary("sum",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_sum_fn);
    register_unary("prod",  RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_prod_fn);
    register_unary("all",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_all_fn);
    register_unary("any",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_any_fn);
    register_unary("count", RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_count_fn);
    register_unary("avg",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_avg_fn);
    register_unary("min",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_min_fn);
    register_unary("max",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_max_fn);
    register_unary("first", RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_first_fn);
    register_unary("last",  RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_last_fn);
    /* med/dev/stddev/var materialise inline (no ray_lazy_append shell) — not LAZY_AWARE */
    register_unary("med",   RAY_FN_AGGR, ray_med_fn);
    register_unary("mode",  RAY_FN_AGGR, ray_mode_fn);
    register_unary("dev",        RAY_FN_AGGR, ray_dev_fn);
    register_unary("stddev",     RAY_FN_AGGR, ray_stddev_fn);
    register_unary("dev_pop",    RAY_FN_AGGR, ray_stddev_pop_fn);
    register_unary("var",        RAY_FN_AGGR, ray_var_fn);
    register_unary("var_pop",    RAY_FN_AGGR, ray_var_pop_fn);

    /* Error handling */
    register_unary("raise", RAY_FN_NONE, ray_raise_fn);
    register_binary("try",  RAY_FN_SPECIAL_FORM, ray_try_fn);

    /* Higher-order functions */
    register_vary("map",    RAY_FN_NONE, ray_map_fn);
    register_vary("pmap",   RAY_FN_NONE, ray_pmap_fn);
    register_vary("fold",   RAY_FN_NONE, ray_fold_fn);
    register_vary("scan",   RAY_FN_NONE, ray_scan_fn);
    register_vary("prior",  RAY_FN_NONE, ray_prior_fn);
    register_binary("filter", RAY_FN_NONE, ray_filter_fn);
    register_vary("apply",  RAY_FN_NONE, ray_apply_fn);

    /* Collection operations */
    register_binary("identical", RAY_FN_NONE, ray_identical_fn);
    register_unary("distinct", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_distinct_fn);
    register_binary("in",      RAY_FN_NONE, ray_in_eval_fn);
    register_binary("not-in",  RAY_FN_NONE, ray_not_in_fn);
    register_binary("except",  RAY_FN_NONE, ray_except_fn);
    register_binary("union",   RAY_FN_NONE, ray_union_fn);
    register_binary("sect",    RAY_FN_NONE, ray_sect_fn);
    register_binary("take",    RAY_FN_NONE, ray_take_fn);
    register_binary("drop",    RAY_FN_NONE, ray_drop_fn);
    register_binary("rotate",  RAY_FN_NONE, ray_rotate_fn);
    register_binary("cut",     RAY_FN_NONE, ray_cut_fn);
    register_binary("cross",   RAY_FN_NONE, ray_cross_fn);
    register_binary("at",      RAY_FN_NONE, ray_at_fn);
    register_binary("find",    RAY_FN_NONE, ray_find_fn);
    register_binary("fill",    RAY_FN_ATOMIC, ray_fill_fn);
    register_unary("reverse",  RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_reverse_fn);
    register_unary("til",      RAY_FN_NONE, ray_til_fn);
    register_unary_op("lag",    RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_lag_fn,    OP_LAG);
    register_unary_op("lead",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_lead_fn,   OP_LEAD);
    register_unary_op("deltas", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_deltas_fn, OP_DELTAS);
    register_unary_op("ratios", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_ratios_fn, OP_RATIOS);
    register_unary_op("fills",  RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_fills_fn,  OP_FILLS);
    register_unary_op("sums",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_sums_fn,   OP_SUMS);
    register_unary_op("avgs",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_avgs_fn,   OP_AVGS);
    register_unary_op("mins",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mins_fn,   OP_MINS);
    register_unary_op("maxs",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_maxs_fn,   OP_MAXS);
    register_unary_op("prds",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_prds_fn,   OP_PRDS);
    register_unary_op("differ", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_differ_fn, OP_DIFFER);
    register_binary("msum",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_msum_fn);
    register_binary("mavg",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mavg_fn);
    register_binary("mmin",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mmin_fn);
    register_binary("mmax",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mmax_fn);
    register_binary("mcount", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mcount_fn);
    register_binary("mvar",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mvar_fn);
    register_binary("mdev",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_mdev_fn);

    /* Sorting operations */
    register_unary("asc",      RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_asc_fn);
    register_unary("desc",     RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_desc_fn);
    register_unary("iasc",     RAY_FN_NONE, ray_iasc_fn);
    register_unary("idesc",    RAY_FN_NONE, ray_idesc_fn);
    register_unary("rank",     RAY_FN_NONE, ray_rank_fn);
    register_binary("xasc",    RAY_FN_NONE, ray_xasc_fn);
    register_binary("xdesc",   RAY_FN_NONE, ray_xdesc_fn);

    /* Table operations */
    register_vary("list",      RAY_FN_NONE, ray_list_fn);
    register_binary("table",   RAY_FN_NONE, ray_table_fn);
    register_unary("key",      RAY_FN_NONE, ray_key_fn);
    register_unary("value",    RAY_FN_NONE, ray_value_fn);
    register_unary("cols",     RAY_FN_NONE, ray_cols_fn);
    register_binary("xcol",    RAY_FN_NONE, ray_xcol_fn);
    register_binary("xcols",   RAY_FN_NONE, ray_xcols_fn);
    register_binary("xkey",    RAY_FN_NONE, ray_xkey_fn);
    register_binary("xgroup",  RAY_FN_NONE, ray_xgroup_fn);
    register_unary("fkeys",    RAY_FN_NONE, ray_fkeys_fn);
    register_binary("union-all",      RAY_FN_NONE, ray_union_all_fn);
    /* table-distinct removed — distinct dispatches on type */

    /* Query operations */
    register_vary("select",    RAY_FN_SPECIAL_FORM, ray_select);
    register_vary("window",    RAY_FN_SPECIAL_FORM, ray_window_fn);
    register_vary("update",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_update);
    register_vary("insert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_insert);
    register_vary("upsert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_upsert);
    register_vary("delete",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_delete);
    /* xbar is registered NON-atomic so the call path lands in
     * ray_xbar_fn(VEC, scalar) directly.  ray_xbar_fn handles the
     * vector fast path itself (tight per-element loop, no per-atom
     * allocation) and recurses through atomic_map_binary for the rare
     * (collection, collection) zip case. */
    register_binary("xbar",    RAY_FN_NONE,   ray_xbar_fn);
    register_binary("trunc",   RAY_FN_NONE,   ray_trunc_fn);

    /* Join operations */
    register_vary("left-join",   RAY_FN_NONE, ray_left_join_fn);
    register_vary("inner-join",  RAY_FN_NONE, ray_inner_join_fn);
    register_vary("full-join",   RAY_FN_NONE, ray_full_join_fn);
    register_vary("anti-join",   RAY_FN_NONE, ray_anti_join_fn);
    register_vary("semi-mask",   RAY_FN_NONE, ray_semi_mask_fn);
    register_vary("anti-mask",   RAY_FN_NONE, ray_anti_mask_fn);
    register_vary("window-join", RAY_FN_SPECIAL_FORM, ray_window_join_fn);
    register_vary("window-join1", RAY_FN_SPECIAL_FORM, ray_window_join1_fn);
    /* Special form: asof-join inspects its operand ASTs (parted-select
     * detection) before evaluating them so a parted (select … from:PARTED)
     * pair can stream per-partition instead of flattening both histories. */
    register_vary("asof-join",   RAY_FN_SPECIAL_FORM, ray_asof_join_fn);

    /* I/O builtins */
    register_vary("println",    RAY_FN_NONE, ray_println_fn);
    register_vary("show",       RAY_FN_NONE, ray_show_fn);
    register_vary("format",     RAY_FN_ROWWISE, ray_format_fn);
    register_vary("repr",      RAY_FN_ROWWISE, ray_repr_fn);
    register_vary("read-value",  RAY_FN_NONE, ray_read_value_fn);
    register_vary("read-limits", RAY_FN_NONE, ray_read_limits_fn);
    register_vary(".json.parse", RAY_FN_NONE, ray_json_parse_fn);
    register_vary(".json.get", RAY_FN_NONE, ray_json_get_fn);
    register_unary(".json.format", RAY_FN_ROWWISE, ray_json_format_fn);
    register_vary(".csv.read",         RAY_FN_RESTRICTED, ray_read_csv_fn);
    register_vary(".csv.splayed",      RAY_FN_RESTRICTED, ray_read_csv_splayed_fn);
    register_vary(".csv.parted",       RAY_FN_RESTRICTED, ray_read_csv_parted_fn);
    register_vary(".csv.write",        RAY_FN_RESTRICTED, ray_write_csv_fn);
    register_vary(".parquet.scan",     RAY_FN_RESTRICTED, ray_parquet_scan_fn);
    register_unary(".parquet.meta",    RAY_FN_RESTRICTED, ray_parquet_metadata_fn);
    register_vary(".parquet.read",     RAY_FN_RESTRICTED, ray_parquet_read_fn);
    register_vary(".parquet.each",     RAY_FN_RESTRICTED, ray_parquet_each_fn);
    register_vary(".parquet.splayed",  RAY_FN_RESTRICTED, ray_parquet_splayed_fn);
    register_vary(".parquet.parted",   RAY_FN_RESTRICTED, ray_parquet_parted_fn);
    register_binary("as",       RAY_FN_NONE, ray_cast_fn);
    register_unary("type",      RAY_FN_NONE, ray_type_fn);
    register_unary("read",      RAY_FN_RESTRICTED, ray_read_file_fn);
    register_unary("read-bytes", RAY_FN_RESTRICTED, ray_read_bytes_fn);
    register_binary("write",    RAY_FN_RESTRICTED, ray_write_file_fn);
    register_binary("write-bytes", RAY_FN_RESTRICTED, ray_write_bytes_fn);
    register_unary("load",      RAY_FN_RESTRICTED, ray_load_file_fn);
    register_unary("exit",      RAY_FN_RESTRICTED, ray_exit_fn);
    register_vary("resolve",    RAY_FN_SPECIAL_FORM, ray_resolve_fn);
    register_vary("timeit",     RAY_FN_SPECIAL_FORM, ray_timeit_fn);

    /* Additional builtins (ported from rayforce) */
    register_vary("enlist",     RAY_FN_NONE, ray_enlist_fn);
    register_binary("dict",     RAY_FN_NONE, ray_dict_fn);
    register_unary("nil?",      RAY_FN_NONE, ray_nil_fn);
    register_unary("where",     RAY_FN_NONE, ray_where_fn);
    register_unary("group",     RAY_FN_NONE, ray_group_indices_fn);
    register_binary("concat",   RAY_FN_NONE, ray_concat_fn);
    register_unary("raze",      RAY_FN_NONE, ray_raze_fn);
    register_unary("ungroup",   RAY_FN_NONE, ray_ungroup_fn);
    register_binary("within",   RAY_FN_NONE, ray_within_fn);
    register_binary("div",      RAY_FN_ATOMIC, ray_idiv_fn);
    register_binary("rand",     RAY_FN_NONE, ray_rand_fn);
    register_binary("bin",      RAY_FN_NONE, ray_bin_fn);
    register_binary("binr",     RAY_FN_NONE, ray_binr_fn);
    register_vary("map-left",   RAY_FN_NONE, ray_map_left_fn);
    register_vary("map-right",  RAY_FN_NONE, ray_map_right_fn);

    /* String operations */
    register_binary("split",     RAY_FN_NONE, ray_split_fn);
    register_binary("str-find",  RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_str_find_fn);
    register_binary("str-join",  RAY_FN_NONE, ray_str_join_fn);
    register_unary ("upper",     RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_upper_fn);
    register_unary ("lower",     RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_lower_fn);
    register_unary ("trim",      RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_trim_fn);
    register_vary  ("substr",    RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_substr_fn);
    register_vary  ("replace",   RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_replace_fn);

    /* Serialization */
    register_unary("ser",        RAY_FN_NONE, ray_ser_fn);
    register_unary("de",         RAY_FN_NONE, ray_de_fn);

    /* Splayed / partitioned table I/O */
    /* Database storage — splayed and parted table I/O.  Kept under a
     * dedicated `.db.*` namespace so format-specific siblings stay
     * grouped (set/get per format) and there's room to grow
     * without polluting the top-level builtin namespace. */
    register_vary(".db.splayed.set",   RAY_FN_RESTRICTED, ray_set_splayed_fn);
    register_vary(".db.splayed.append", RAY_FN_RESTRICTED, ray_append_splayed_fn);
    register_vary(".db.splayed.get",   RAY_FN_NONE,       ray_get_splayed_fn);
    register_vary(".db.parted.get",    RAY_FN_NONE,       ray_get_parted_fn);
    register_vary(".db.parted.tables", RAY_FN_NONE,       ray_get_parted_tables_fn);
    register_vary(".db.parted.fill",   RAY_FN_RESTRICTED, ray_fill_parted_fn);

    /* GUID generation */
    register_unary("guid",       RAY_FN_NONE, ray_guid_fn);

    /* In-place mutation */
    register_vary("alter",       RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_alter_fn);

    /* Pattern matching */
    register_binary("like",      RAY_FN_NONE, ray_like_fn);

    /* Temporal clocks */
    register_unary("date",       RAY_FN_NONE, ray_date_clock_fn);
    register_unary("time",       RAY_FN_NONE, ray_time_clock_fn);
    register_unary("timestamp",  RAY_FN_NONE, ray_timestamp_clock_fn);

    /* Temporal field accessors: unary builtins that map 1:1 onto
     * ray_temporal_extract.  Registered here so `(ss ts)` / `(dd d)`
     * participate in the normal call machinery and `ts.ss` / `d.dd`
     * resolve through env_resolve's "is segment a callable" lookup
     * instead of a bespoke sym→field table. */
    register_unary("ss",         RAY_FN_NONE, ray_extract_ss_fn);
    register_unary("hh",         RAY_FN_NONE, ray_extract_hh_fn);
    register_unary("minute",     RAY_FN_NONE, ray_extract_minute_fn);
    register_unary("yyyy",       RAY_FN_NONE, ray_extract_yyyy_fn);
    register_unary("mm",         RAY_FN_NONE, ray_extract_mm_fn);
    register_unary("dd",         RAY_FN_NONE, ray_extract_dd_fn);
    register_unary("dow",        RAY_FN_NONE, ray_extract_dow_fn);
    register_unary("doy",        RAY_FN_NONE, ray_extract_doy_fn);

    /* Eval, parse, print, meta */
    register_unary("eval",       RAY_FN_NONE, ray_eval_builtin_fn);
    register_unary("parse",      RAY_FN_NONE, ray_parse_builtin_fn);
    register_vary("print",       RAY_FN_NONE, ray_print_fn);
    register_unary("meta",       RAY_FN_NONE, ray_meta_fn);

    /* System builtins — bound under the reserved `.sys.*` namespace so
     * user code can't shadow them and a glance at the name identifies
     * the category. */
    register_vary (".sys.gc",   RAY_FN_NONE,        ray_gc_fn);
    register_vary (".sys.exec", RAY_FN_RESTRICTED,  ray_system_fn);
    register_unary(".mem.objsize", RAY_FN_NONE,      ray_mem_objsize_fn);
    register_vary (".mem.ts",      RAY_FN_SPECIAL_FORM, ray_mem_ts_fn);
    /* Registry-dispatched system commands.  `.sys.cmd "name args"` is
     * the colon-command entry point; the per-command direct builtins below
     * skip the string parse for callers that already have a typed arg
     * in hand.  All share the table in lang/syscmd.c. */
    register_unary(".sys.cmd",    RAY_FN_RESTRICTED, ray_syscmd_string_dispatch_fn);
    register_vary (".sys.timeit", RAY_FN_NONE,       ray_sys_timeit_fn);
    register_unary(".sys.listen", RAY_FN_RESTRICTED, ray_sys_listen_fn);
    register_unary(".sys.trust",  RAY_FN_RESTRICTED, ray_sys_trust_fn);
    register_vary (".sys.env",    RAY_FN_NONE,       ray_sys_env_fn);
    register_vary (".sys.args",   RAY_FN_NONE,       ray_sys_args_fn);

    /* OS env / process interaction under `.os.*` */
    register_unary( ".os.getenv", RAY_FN_RESTRICTED,  ray_getenv_fn);
    register_binary(".os.setenv", RAY_FN_RESTRICTED,  ray_setenv_fn);

    /* Filesystem metadata under `.fs.*` (issue #36): size + listing.
     * Predicates (exists / is-file / is-dir) are reachable via `try` on
     * these or via shell fallback through `.sys.cmd`. */
    register_unary( ".fs.size",   RAY_FN_NONE,        ray_fs_size_fn);
    register_unary( ".fs.list",   RAY_FN_NONE,        ray_fs_list_fn);

    /* IPC client primitives under `.ipc.*` */
    register_vary(  ".ipc.open",  RAY_FN_RESTRICTED,  ray_hopen_fn);
    register_unary( ".ipc.close", RAY_FN_RESTRICTED,  ray_hclose_fn);
    register_vary(  ".ipc.send",  RAY_FN_RESTRICTED,  ray_hsend_fn);
    register_binary(".ipc.post",  RAY_FN_RESTRICTED,  ray_hpost_fn);
    /* Current connection handle inside any `.ipc.on.*` hook, -1 otherwise.
     * Variadic for the `(.ipc.handle)` / `(.ipc.handle 0)` convention. */
    register_vary(  ".ipc.handle", RAY_FN_NONE,       ray_ipc_handle_fn);
    /* Every open connection as a table: read-only, so allowed under -U. */
    register_vary(  ".ipc.handles", RAY_FN_NONE,      ray_ipc_handles_fn);
    /* True while evaluating inside an auto-journaled IPC request, where
     * the request itself is the journal entry and `.log.write` refuses. */
    register_vary(  ".ipc.auto_journal_eval", RAY_FN_NONE, ray_ipc_auto_journal_eval_fn);
    /* Transmit backlog policy: read, set the process default, or override
     * one connection.  Privileged — it decides when a peer is dropped. */
    register_vary(  ".ipc.txlimit", RAY_FN_RESTRICTED, ray_ipc_txlimit_fn);
    /* Transport policy: which faster road a connection may move to.
     * Privileged — it decides what this process offers its peers. */
    register_vary(  ".ipc.transport", RAY_FN_RESTRICTED, ray_ipc_transport_fn);

    /* Multicast/pub-sub over IPC.  Subscriptions are allowed under
     * restricted IPC; publishing stays privileged. */
    register_vary(  ".mc.sub",   RAY_FN_NONE,       ray_mc_sub_fn);
    register_unary( ".mc.unsub", RAY_FN_NONE,       ray_mc_unsub_fn);
    register_binary(".mc.pub",   RAY_FN_RESTRICTED, ray_mc_pub_fn);
    register_vary(  ".mc.stats", RAY_FN_NONE,       ray_mc_stats_fn);

    /* Remote-REPL session control under `.repl.*`.  Once .repl.connect
     * succeeds, the local REPL line-loop reroutes each subsequent input
     * through ray_ipc_send_verbose() until .repl.disconnect.  See
     * src/app/repl_remote.c for the state model. */
    register_unary(".repl.connect",    RAY_FN_RESTRICTED, ray_repl_connect_fn);
    register_vary( ".repl.disconnect", RAY_FN_RESTRICTED, ray_repl_disconnect_fn);
    /* Read-only namespace completion (issue #441): builtins + keywords +
     * user bindings whose name begins with the given prefix.  RAY_FN_NONE
     * so remote editor clients can query it over IPC. */
    register_unary(".repl.complete",   RAY_FN_NONE,       ray_repl_complete_fn);
    /* Read-only metadata of a named global for editor tooling (#947):
     * calls and evaluates nothing, names only, so allowed under -U. */
    register_unary(".repl.signature",  RAY_FN_NONE,       ray_repl_signature_fn);
    /* Where the function a named global holds was written (#946): server
     * file paths and loaded source, so refused under -U. */
    register_unary(".repl.definition", RAY_FN_RESTRICTED, ray_repl_definition_fn);
    /* Read-only members and schema of a named global path (#945): reads
     * headers only, never rows or decoded columns; allowed under -U. */
    register_vary(".repl.inspect",     RAY_FN_NONE,       ray_repl_inspect_fn);

    /* Transaction-log journaling under `.log.*` — the -l/-L feature.
     * The CLI flags -l <base> / -L <base> call ray_journal_open() at
     * startup; these builtins expose the same machinery to Rayfall code
     * for manual control (open from a script, snapshot on demand, etc). */
    register_vary(".log.open",     RAY_FN_RESTRICTED, ray_log_open_fn);
    register_vary(  ".log.status", RAY_FN_NONE,       ray_log_status_fn);
    register_unary(".log.write",   RAY_FN_RESTRICTED, ray_log_write_fn);
    register_unary(".log.replay",  RAY_FN_RESTRICTED, ray_log_replay_fn);
    register_unary(".log.replay_report", RAY_FN_RESTRICTED, ray_log_replay_report_fn);
    register_unary(".log.validate",RAY_FN_NONE,       ray_log_validate_fn);
    register_vary(".log.roll",     RAY_FN_RESTRICTED, ray_log_roll_fn);
    register_vary(".log.snapshot", RAY_FN_RESTRICTED, ray_log_snapshot_fn);
    register_vary(".log.sync",     RAY_FN_NONE,       ray_log_sync_fn);
    register_vary(".log.posts",    RAY_FN_RESTRICTED, ray_log_posts_fn);
    register_vary(".log.close",    RAY_FN_RESTRICTED, ray_log_close_fn);
    register_vary(".log.purge",    RAY_FN_RESTRICTED, ray_log_purge_fn);

    /* quote — special form (unevaluated argument) */
    register_vary("quote",       RAY_FN_SPECIAL_FORM, ray_quote_fn);

    /* return — early exit from compiled lambda (0 args → null, 1 arg → value) */
    register_vary("return",      RAY_FN_NONE, ray_return_fn);

    /* rc — reference count */
    register_unary("rc",         RAY_FN_NONE, ray_rc_fn);

    /* diverse — check if all elements unique */
    register_unary("diverse",    RAY_FN_NONE, ray_diverse_fn);

    /* get — dictionary/table lookup (alias for at) */
    register_binary("get",       RAY_FN_NONE, ray_get_fn);

    /* remove — remove key from dict */
    register_binary("remove",    RAY_FN_NONE, ray_remove_fn);

    /* row — single row from table */
    register_binary("row",       RAY_FN_NONE, ray_row_fn);

    /* .time.* — monotonic clock, scheduled timers and sleep */
    register_vary (".time.now",       RAY_FN_NONE,       ray_time_now_fn);
    register_vary (".time.timer.set", RAY_FN_RESTRICTED, ray_time_timer_set_fn);
    register_unary(".time.timer.del", RAY_FN_RESTRICTED, ray_time_timer_del_fn);
    /* Restricted: a sleep stops the whole single-threaded server, and a
     * restricted peer may not occupy it at will (as with .sys.exec). */
    register_unary(".time.sleep",     RAY_FN_RESTRICTED, ray_time_sleep_fn);

    /* env — list all global environment bindings */
    register_unary("env",        RAY_FN_NONE, ray_env_fn);

    /* Directional fold/scan variants */
    register_vary("fold-left",   RAY_FN_NONE, ray_fold_left_fn);
    register_vary("fold-while",  RAY_FN_NONE, ray_fold_while_fn);
    register_vary("fold-right",  RAY_FN_NONE, ray_fold_right_fn);
    register_vary("scan-left",   RAY_FN_NONE, ray_scan_left_fn);
    register_vary("scan-right",  RAY_FN_NONE, ray_scan_right_fn);

    /* del, modify, pivot remain top-level language primitives.
     * Runtime/heap introspection moves under `.sys.*`. */
    register_vary("del",          RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_del_fn);
    register_vary(".sys.build", RAY_FN_NONE, ray_internals_fn);
    register_vary(".sys.mem",   RAY_FN_NONE, ray_memstat_fn);
    register_unary(".idx.uses", RAY_FN_NONE, ray_idx_uses_fn);
    register_vary(".sys.prof",  RAY_FN_NONE, ray_prof_fn);
    register_vary(".sys.querylog",        RAY_FN_NONE,       ray_qlog_fn);
    register_vary(".sys.querylog.stats",  RAY_FN_NONE,       ray_qlog_stats_fn);
    register_vary(".sys.querylog.enable", RAY_FN_RESTRICTED, ray_qlog_enable_fn);
    register_vary(".sys.join.par.min",    RAY_FN_RESTRICTED, ray_join_par_min_fn);
    register_vary(".sys.profile",         RAY_FN_NONE,       ray_callprof_fn);
    register_vary(".sys.profile.enable",  RAY_FN_RESTRICTED, ray_callprof_enable_fn);
    register_vary(".sys.profile.reset",   RAY_FN_RESTRICTED, ray_callprof_reset_fn);
    register_vary("modify",     RAY_FN_RESTRICTED, ray_modify_fn);
    register_vary("pivot",      RAY_FN_NONE, ray_pivot_fn);
    register_vary(".sys.info",  RAY_FN_NONE, ray_sysinfo_fn);
    register_unary("sym-name",   RAY_FN_NONE, ray_sym_name_fn);
    register_binary("unify",     RAY_FN_NONE, ray_unify_fn);
    register_binary("xrank",     RAY_FN_NONE, ray_xrank_fn);

    /* EAV triple storage */
    register_vary("datoms",        RAY_FN_NONE, ray_datoms_fn);
    register_vary("assert-fact",   RAY_FN_NONE, ray_assert_fact_fn);
    register_vary("retract-fact",  RAY_FN_NONE, ray_retract_fact_fn);
    register_vary("scan-eav",      RAY_FN_NONE, ray_scan_eav_fn);
    register_vary("pull",          RAY_FN_NONE, ray_pull_fn);

    /* Datalog */
    register_vary("rule",         RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_rule_fn);
    register_vary("query",        RAY_FN_SPECIAL_FORM, ray_query_fn);

    /* Programmatic Datalog API */
    register_vary("dl-program",    RAY_FN_NONE, ray_dl_program_fn);
    register_vary("dl-add-edb",    RAY_FN_RESTRICTED, ray_dl_add_edb_fn);
    register_unary("dl-stratify",  RAY_FN_RESTRICTED, ray_dl_stratify_fn);
    register_unary("dl-eval",      RAY_FN_RESTRICTED, ray_dl_eval_fn);
    register_binary("dl-query",    RAY_FN_NONE, ray_dl_query_fn);
    register_binary("dl-provenance", RAY_FN_NONE, ray_dl_provenance_fn);
    register_unary("dl-free",      RAY_FN_RESTRICTED, ray_dl_free_fn);

    /* Vector similarity / embeddings / HNSW */
    register_binary(".emb.cos-dist",    RAY_FN_NONE, ray_cos_dist_fn);
    register_binary(".emb.inner-prod",  RAY_FN_NONE, ray_inner_prod_fn);
    register_binary(".emb.l2-dist",     RAY_FN_NONE, ray_l2_dist_fn);
    register_unary ("norm",        RAY_FN_NONE, ray_norm_fn);
    register_binary("mmu",         RAY_FN_NONE, ray_mmu_fn);
    register_binary("lsq",         RAY_FN_NONE, ray_lsq_fn);
    register_unary ("inv",         RAY_FN_NONE, ray_inv_fn);
    register_unary ("flip",        RAY_FN_NONE, ray_flip_mat_fn);
    register_unary ("gram",        RAY_FN_NONE, ray_gram_fn);
    register_vary  (".emb.knn",         RAY_FN_NONE, ray_knn_fn);
    register_vary  (".emb.hnsw.build",  RAY_FN_NONE, ray_hnsw_build_fn);
    register_vary  (".emb.ann",         RAY_FN_NONE, ray_ann_fn);
    register_unary (".emb.hnsw.free",   RAY_FN_RESTRICTED, ray_hnsw_free_fn);
    register_binary(".emb.hnsw.set",   RAY_FN_RESTRICTED, ray_hnsw_save_fn);
    register_unary (".emb.hnsw.get",   RAY_FN_RESTRICTED, ray_hnsw_load_fn);
    register_unary (".emb.hnsw.info",   RAY_FN_NONE, ray_hnsw_info_fn);
    register_unary (".emb.model.get",   RAY_FN_RESTRICTED, ray_emb_model_get_fn);
    register_unary (".emb.model.info",  RAY_FN_NONE, ray_emb_model_info_fn);
    register_binary(".emb.tokens",      RAY_FN_NONE, ray_emb_tokens_fn);
    register_vary  (".emb.encode",      RAY_FN_NONE, ray_emb_encode_fn);

    /* Per-vector accelerator indices (see src/ops/idxop.h) */
    register_unary (".idx.zone",   RAY_FN_NONE, ray_idx_zone_fn);
    register_unary (".idx.hash",   RAY_FN_NONE, ray_idx_hash_fn);
    register_unary (".idx.sort",   RAY_FN_NONE, ray_idx_sort_fn);
    register_unary (".idx.bloom",  RAY_FN_NONE, ray_idx_bloom_fn);
    register_unary (".idx.drop",   RAY_FN_NONE, ray_idx_drop_fn);
    register_unary (".idx.has?",   RAY_FN_NONE, ray_idx_has_fn);
    register_vary  (".idx.info",   RAY_FN_NONE, ray_idx_info_vary_fn);

    /* Semantic attributes (see src/ops/idxop.c: (.attr.*) family) */
    register_binary(".attr.set",   RAY_FN_NONE, ray_attr_set_fn);
    register_unary (".attr.get",   RAY_FN_NONE, ray_attr_get_fn);
    register_unary (".attr.drop",  RAY_FN_NONE, ray_attr_drop_fn);

    /* Linked columns (see src/ops/linkop.h) */
    register_binary(".col.link",   RAY_FN_NONE, ray_col_link_fn);
    register_unary (".col.unlink", RAY_FN_NONE, ray_col_unlink_fn);
    register_unary (".col.link?",  RAY_FN_NONE, ray_col_link_p_fn);
    register_unary (".col.target", RAY_FN_NONE, ray_col_target_fn);

    /* Graph algorithms (see src/ops/graph_builtin.c, src/ops/traverse.c).
     * Build a CSR handle from an edge table, then dispatch any of the 18
     * algorithms in src/ops/traverse.c.  All wrappers are vary-arity so
     * the underlying algorithm's optional parameters surface uniformly.
     * Lifecycle handles auto-free on rc→0 (see RAY_ATTR_GRAPH branch in
     * src/mem/heap.c). */
    register_vary (".graph.build",         RAY_FN_NONE, ray_graph_build_fn);
    register_unary(".graph.free",          RAY_FN_RESTRICTED, ray_graph_free_fn);
    register_unary(".graph.info",          RAY_FN_NONE, ray_graph_info_fn);
    register_vary (".graph.pagerank",      RAY_FN_NONE, ray_graph_pagerank_fn);
    register_vary (".graph.connected",     RAY_FN_NONE, ray_graph_connected_fn);
    register_vary (".graph.dijkstra",      RAY_FN_NONE, ray_graph_dijkstra_fn);
    register_vary (".graph.louvain",       RAY_FN_NONE, ray_graph_louvain_fn);
    register_vary (".graph.degree",        RAY_FN_NONE, ray_graph_degree_fn);
    register_vary (".graph.topsort",       RAY_FN_NONE, ray_graph_topsort_fn);
    register_vary (".graph.dfs",           RAY_FN_NONE, ray_graph_dfs_fn);
    register_vary (".graph.cluster",       RAY_FN_NONE, ray_graph_cluster_fn);
    register_vary (".graph.betweenness",   RAY_FN_NONE, ray_graph_betweenness_fn);
    register_vary (".graph.closeness",     RAY_FN_NONE, ray_graph_closeness_fn);
    register_vary (".graph.mst",           RAY_FN_NONE, ray_graph_mst_fn);
    register_vary (".graph.random-walk",   RAY_FN_NONE, ray_graph_random_walk_fn);
    register_vary (".graph.k-shortest",    RAY_FN_NONE, ray_graph_k_shortest_fn);
    register_vary (".graph.shortest-path", RAY_FN_NONE, ray_graph_shortest_path_fn);
    register_vary (".graph.expand",        RAY_FN_NONE, ray_graph_expand_fn);
    register_vary (".graph.var-expand",    RAY_FN_NONE, ray_graph_var_expand_fn);
    /* NOT lazy-aware: ray_strlen_fn has no lazy branch and there is no
     * OP_STRLEN chain opcode — the flag made eval pass raw lazy handles
     * that fell through to the eager paths and errored.  Without the
     * flag eval materializes lazy args before the call. */
    register_unary("strlen",               RAY_FN_NONE, ray_strlen_fn);
}

/* ══════════════════════════════════════════
 * Runtime lifecycle
 * ══════════════════════════════════════════ */

ray_err_t ray_lang_init(void) {
    ray_err_t err = ray_env_init();
    if (err != RAY_OK) return err;
    ray_register_builtins();
    return RAY_OK;
}

void ray_lang_destroy(void) {
    if (__VM && __VM->raise_val) { ray_release(__VM->raise_val); __VM->raise_val = NULL; }
    closure_tmpl_reset();
    ray_callprof_destroy();
    /* Reset global Datalog rule storage */
    ray_dl_reset_rules();
    ray_env_destroy();
    ray_compile_reset();
    g_call_self_sym = -1;
}

/* ══════════════════════════════════════════
 * Tree-walking evaluator
 * ══════════════════════════════════════════ */

/* Fill a profiler span's payload — memory (sys_cur) and parallelism (qstats)
 * — from the live counters.  Mirrors exec_node's capture so direct-builtin
 * ops (arithmetic, aggregation, generators that run OUTSIDE the DAG) report
 * the same columns as DAG operators.  `result` is NULL at span START (no
 * row count to size yet). */
static inline void eval_span_payload(ray_prof_span_t* s, ray_t* result) {
    if (!s) return;
    int64_t cur = 0; ray_sys_get_stat(&cur, NULL);
    s->sys_cur = cur;
    s->qs_rows = ray_qstats_sum_rows();
    uint64_t sum = 0; uint32_t used = 0;
    ray_query_workers_snapshot(&used, &sum);
    s->qs_busy_ns = sum;
    s->qs_workers = used;
    if (result && !RAY_IS_ERR(result) && !RAY_IS_NULL(result)) {
        /* ray_len on a scalar atom returns the stored VALUE (the len field
         * aliases i64), not an element count — so a `(sum …)` result would
         * masquerade as a billion-row output.  A scalar is one row. */
        s->rows_out  = (result->type == RAY_TABLE) ? ray_table_nrows(result)
                     : ray_is_atom(result)         ? 1
                     :                                (int64_t)ray_len(result);
    }
}

/* Only the direct argument of ordinary SUM gets a fusion request. Nested
 * evaluations (including special forms) enter through ray_eval as usual. */
static ray_t* eval_impl(ray_t* obj, bool* summed);

ray_t* ray_eval(ray_t* obj) {
    return eval_impl(obj, NULL);
}

static ray_t* eval_impl(ray_t* obj, bool* summed) {

    if (!obj || RAY_IS_ERR(obj)) return obj;

    /* All eval-context state (depth, scope, restricted, nfo/trace) lives
     * on the per-thread VM — a thread must bind one before evaluating. */
    if (RAY_UNLIKELY(!__VM))
        return ray_error("state", "no VM bound to this thread");

    if (__VM->eval_depth == 0) {
        affine_sum_cache_clear();
        /* Scope the parallelism counters to THIS top-level query.  The
         * slots are process-cumulative otherwise, so a serial op would
         * inherit the worker count of whatever ran before it (e.g. table
         * construction), reading as "28 workers, 0 busy".  Resetting at the
         * query boundary — and setting PROF mode here rather than only in
         * ray_execute — also means non-DAG paths (the select special form,
         * direct builtins) get their pool dispatches measured, and keeps
         * capture strictly off (mode 0) when the profiler is disabled. */
        bool capture  = g_ray_profile.active || ray_qlog_enabled();
        bool progress = ray_progress_active();
        ray_query_workers_begin(capture, progress);
    }

    /* Check for external interrupt (e.g. Ctrl-C from REPL) */
    if (EVAL_INTERRUPTED()) return ray_error("cancel", "interrupted");

    /* The stack is read every fourth level: four levels are far below
     * the reserve in any build, and the hot path skips the read. */
    int32_t depth_now = ++__VM->eval_depth;
    if (RAY_UNLIKELY(depth_now > RAY_EVAL_MAX_DEPTH ||
                     ((depth_now & 3) == 0 && ray_stack_short()))) {
        __VM->eval_depth--;
        return ray_error("limit", "eval depth exceeded");
    }

    ray_t* ret;
    int8_t rows = -1;   /* provenance of ret; published as g_eval_rows at out: */

    /* Direct-builtin profiling span: opened just before the head-dispatch
     * switch below (for UNARY/BINARY/VARY builtins), closed at `out:`.
     * Extends the profiler's coverage to ops that run outside the DAG —
     * generators (`til`), arithmetic (`+`), aggregation (`count`/`sum`) —
     * which otherwise leave the whole query opaque in a single `eval` tick.
     * NULL/-1 on every path that never reaches the switch (atoms, errors). */
    ray_prof_span_t* ev_span = NULL;
    const char*      ev_name = NULL;
    int64_t          ev_idx  = -1;
    int32_t          cp_frame = -1;   /* call profiler frame (lang/callprof.h) */

    /* Atoms: return themselves (retain) */
    if (ray_is_atom(obj)) {
        /* Name reference: resolve from env (unflagged default; ATTR_QUOTED clear) */
        if (obj->type == -RAY_SYM && !(obj->attrs & ATTR_QUOTED)) {
            /* Check for null keyword — compare by string, not cached sym_id,
             * because sym table may be reinitialized between test runs */
            {
                ray_t* name_str = ray_sym_str(obj->i64);
                if (name_str && ray_str_len(name_str) == 4 &&
                    memcmp(ray_str_ptr(name_str), "null", 4) == 0) {
                    ray_release(name_str);
                    ret = RAY_NULL_OBJ; goto out;
                }
                if (name_str) ray_release(name_str);
            }

            uint8_t val_rows = 0;   /* a query's column binding, or a helper's */
            ray_t* val = ray_env_resolve_rows(obj->i64, &val_rows);
            if (!val) {
                ret = undefined_name_error(obj->i64);
                goto out;
            }
            /* env_resolve may also return a real error (e.g. nyi from a
             * parted-target link deref inside the dotted walker) — surface
             * it directly rather than treating it as a found value. */
            if (RAY_IS_ERR(val)) { ret = val; goto out; }
            /* env_resolve hands back an owned ref; no extra retain. */
            rows = vrows_to(val_rows);
            ret = val; goto out;
        }
        /* Literal symbol (ATTR_QUOTED set).  Inside a query, a literal that
         * names a from-table COLUMN resolves to that column (B3 Part 2).  We
         * consult ONLY the active query table's column set — never the env
         * scope chain — so a literal never captures a lambda/let local, and
         * the rule fires only while a query is active (ray_active_query_table
         * is NULL otherwise).  A literal naming no column returns itself.
         * Named helper bodies keep bytecode's literal semantics; an inline
         * lambda is query text, and a nested query re-enables its own
         * column-name interpretation. */
        if (obj->type == -RAY_SYM && !ray_eval_query_helper_literals()) {
            /* The column — or, during a per-row evaluation, its cell in the
             * current row (ray_active_query_literal). */
            ray_t* v = ray_active_query_literal(obj->i64);
            if (v) { ret = v; rows = 1; goto out; }
        }
        ray_retain(obj);
        ret = obj; rows = 0; goto out;
    }

    /* Non-list vectors (incl. RAY_DICT/RAY_TABLE): return themselves —
     * dict literals are self-evaluating; values stay unevaluated.  Use
     * the (dict ...) builtin for evaluated construction. */
    if (obj->type != RAY_LIST) { ray_retain(obj); ret = obj; rows = 0; goto out; }

    /* Empty list */
    if (ray_len(obj) == 0) { ray_retain(obj); ret = obj; rows = 0; goto out; }

    /* List: evaluate first element, dispatch by type */
    ray_t** elems = (ray_t**)ray_data(obj);
    int literal_fallback = (obj->attrs & RAY_EVAL_LITERAL_FALLBACK) != 0;
    ray_t* head = ray_eval(elems[0]);
    if (RAY_IS_ERR(head)) {
        const char* code = ray_err_code(head);
        if (literal_fallback && code && strcmp(code, "name") == 0) {
            ray_error_free(head);
            ray_retain(obj);
            ret = obj; goto out;
        }
        ret = head; goto out;
    }

    /* A symbol in functional position names the function to apply.  A name
     * head (e.g. from `(quote f)`) already resolved during head-eval above;
     * this handles a literal-symbol head (e.g. from `(list 'f 1)`), which
     * would otherwise fall through to the `type` error below. */
    if (head->type == -RAY_SYM) {
        ray_t* fn = ray_env_resolve(head->i64);
        if (!fn) {
            if (literal_fallback) {
                ray_release(head);
                ray_retain(obj);
                ret = obj; goto out;
            }
            ret = undefined_name_error(head->i64);
            ray_release(head);
            goto out;
        }
        /* env_resolve may also surface a real error (e.g. nyi from a
         * dotted-target deref) — propagate it directly. */
        if (RAY_IS_ERR(fn)) {
            const char* code = ray_err_code(fn);
            if (literal_fallback && code && strcmp(code, "name") == 0) {
                ray_release(head);
                ray_error_free(fn);
                ray_retain(obj);
                ret = obj; goto out;
            }
            ray_release(head); ret = fn; goto out;
        }
        ray_release(head);
        head = fn;   /* env_resolve hands back an owned ref; no extra retain. */
    }

    int64_t n = ray_len(obj);

    /* Open a profiling span around the builtin invocation.  Gated on the
     * profiler flag FIRST so the disabled path is a single predicted branch
     * per call — nothing else runs (pillar-4 zero-cost-off).  ray_fn_name
     * reads the label inline from the fn object's aux bytes — stable for
     * as long as `head` lives, but NOT beyond: a builtin's env slot can be
     * rebound (e.g. `(set + ...)`), dropping the object's refcount to zero
     * and freeing those bytes.  The span outlives this call (read later by
     * `.sys.prof`, possibly after such a rebind), so the raw aux pointer
     * must not be stored directly.  Intern it instead: the sym table's
     * arena never frees or relocates an interned string once allocated, so
     * ray_str_ptr(ray_sym_str(...)) is valid for the rest of the process,
     * independent of `head`'s lifetime. */
    if (g_ray_profile.active &&
        (head->type == RAY_UNARY || head->type == RAY_BINARY ||
         head->type == RAY_VARY)) {
        const char* raw_name = ray_fn_name(head);
        ray_t* name_atom = ray_sym_str(ray_sym_intern(raw_name, strlen(raw_name)));
        ev_name = name_atom ? ray_str_ptr(name_atom) : raw_name;
        ev_span = ray_profile_span_start(ev_name);
        if (ev_span) {
            ev_idx = (int64_t)g_ray_profile.n - 1;   /* index of this START */
            eval_span_payload(ev_span, NULL);
        }
    }

    /* The call profiler times top-level calls by their parsed list. */
    if (RAY_UNLIKELY(ray_callprof_on())) cp_frame = ray_callprof_enter_node(obj);

    switch (head->type) {
        case RAY_UNARY: {
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ray_unary_fn fn = (ray_unary_fn)(uintptr_t)head->i64;
            ray_binary_fn fn2 = unary_binary_overload(fn);
            /* Row provenance follows a builtin the planner lowers row by row. */
            bool rowwise = ray_active_query_table() && vm_fn_rowwise(head);
            if (n == 3 && fn2) {
                ray_t* left = ray_eval(elems[1]);
                int8_t left_rows = g_eval_rows;
                if (!left || RAY_IS_ERR(left)) { ray_release(head); ret = left ? left : ray_error("type", NULL); goto out; }
                ray_t* right = ray_eval(elems[2]);
                int8_t right_rows = g_eval_rows;
                rows = rowwise ? rows_join(left_rows, right_rows) : 0;
                ray_release(head);
                if (!right || RAY_IS_ERR(right)) { ray_release(left); ret = right ? right : ray_error("type", NULL); goto out; }
                if (ray_is_lazy(left)) {
                    left = ray_lazy_materialize(left);
                    if (!left || RAY_IS_ERR(left)) { ray_release(right); ret = left ? left : ray_error("type", NULL); goto out; }
                }
                if (ray_is_lazy(right)) {
                    right = ray_lazy_materialize(right);
                    if (!right || RAY_IS_ERR(right)) { ray_release(left); ret = right ? right : ray_error("type", NULL); goto out; }
                }
                ret = (is_collection(left) || is_collection(right))
                    ? minmax_map(fn2, left, right) : fn2(left, right);
                ray_release(left);
                ray_release(right);
                goto out;
            }
            if (n != 2) {
                ray_t* arity_err = ray_sig_arity(ray_fn_name(head), "1", (int)(n-1));
                ray_release(head); ret = arity_err; goto out;
            }
            char nm[16];
            memcpy(nm, ray_fn_name(head), 14); nm[14] = '\0';   /* inline name: 13 characters and a NUL */
            uint8_t fn_attrs = head->attrs;
            if (fn == (ray_unary_fn)ray_count_fn) {
                int handled = 0;
                ray_t* fast = ray_try_count_select_expr(elems[1], &handled);
                if (handled) {
                    ray_release(head);
                    ret = fast ? fast : ray_error("type", NULL);
                    rows = 0; goto out;
                }
            }
            if (fn == (ray_unary_fn)ray_first_fn) {
                /* (first (select ...)): the select's first row alone */
                int handled = 0;
                ray_t* top = ray_try_select_head_expr(elems[1], 1, &handled);
                if (handled) {
                    ray_release(head);
                    ret = top && !RAY_IS_ERR(top) ? ray_first_fn(top) : top;
                    if (top && !RAY_IS_ERR(top)) ray_release(top);
                    rows = 0; goto out;
                }
            }
            bool fused = false;
            ray_t* arg = eval_impl(elems[1], fn == (ray_unary_fn)ray_sum_fn ? &fused : NULL);
            if (fused) { ray_release(head); ret = arg; rows = 0; goto out; }
            int8_t arg_rows = g_eval_rows;
            rows = rowwise ? g_eval_rows : 0;
            ray_release(head);
            if (arg && RAY_IS_ERR(arg)) { ret = arg; goto out; }
            /* Materialise lazy arg for non-lazy-aware fns.
             * arg is an owned ref (returned from ray_eval); materialise
             * consumes it — no extra ray_release needed after the call. */
            if (!(fn_attrs & RAY_FN_LAZY_AWARE) && arg && ray_is_lazy(arg)) {
                arg = ray_lazy_materialize(arg); /* consumes owned ref */
                if (!arg || RAY_IS_ERR(arg)) { ret = arg ? arg : ray_error("type", NULL); goto out; }
            }
            ray_t* result;
            RAY_ASSERT_VALUE(arg);
            if (RAY_UNLIKELY(fn_attrs & RAY_FN_ROWWISE)) {   /* applied per row in a query (#1594) */
                uint8_t r1 = vrows_from(arg_rows);
                ray_t* rw = rowwise_apply(RAY_UNARY, (void*)(uintptr_t)fn, &arg, &r1, 1);
                if (rw) {
                    if (arg) ray_release(arg);
                    rows = RAY_IS_ERR(rw) ? 0 : 1;
                    ret = rw; goto out;
                }
            }
            /* first / last of the query's rows skip nulls (#1673) */
            ray_t* edge = vm_fn_is_edge((int64_t)(uintptr_t)fn)
                ? ray_query_edge_value((void*)(uintptr_t)fn, arg, vrows_from(arg_rows)) : NULL;
            if (edge) {
                if (arg) ray_release(arg);
                ret = edge; goto out;
            }
            if (RAY_IS_NULL(arg)) {
                /* Only nil?/type/ser safely handle null */
                result = (fn == (ray_unary_fn)ray_nil_fn || fn == (ray_unary_fn)ray_type_fn ||
                          fn == (ray_unary_fn)ray_ser_fn) ? fn(arg) : ray_error("type", NULL);
            } else if ((fn_attrs & RAY_FN_ATOMIC) && is_collection(arg))
                result = atomic_map_unary(fn, arg);
            else
                result = fn(arg);
            result = ray_sig_note(nm, result);
            if (arg) ray_release(arg);
            ret = result; goto out;
        }
        case RAY_BINARY: {
            if (n > 3 && (ray_binary_fn)(uintptr_t)head->i64 == (ray_binary_fn)ray_concat_fn &&
                !(head->attrs & RAY_FN_SPECIAL_FORM) && ray_active_query_table()) {
                /* A query's concat takes up to 255 operands (OP_CONCAT).  Where
                 * the interpreter evaluates it, a by: projection or a fallback,
                 * join them left to right, each step the binary text concat
                 * below: (concat a b c) is (concat (concat a b) c) (#1592). */
                if (n - 1 > 255) {
                    ray_release(head);
                    ret = ray_error("arity", "concat: expected 2 to 255 arguments, got %lld", (long long)(n - 1));
                    goto out;
                }
                ray_t* nested = ray_list_new(3);
                nested = ray_list_append(nested, elems[0]);
                nested = ray_list_append(nested, elems[1]);
                nested = ray_list_append(nested, elems[2]);
                for (int64_t i = 3; i < n && nested && !RAY_IS_ERR(nested); i++) {
                    ray_t* outer = ray_list_new(3);
                    outer = ray_list_append(outer, elems[0]);
                    outer = ray_list_append(outer, nested);
                    outer = ray_list_append(outer, elems[i]);
                    ray_release(nested);
                    nested = outer;
                }
                ray_release(head);
                if (!nested || RAY_IS_ERR(nested)) { ret = nested ? nested : ray_error("oom", NULL); goto out; }
                ret = ray_eval(nested);
                rows = g_eval_rows;
                ray_release(nested);
                goto out;
            }
            if (n != 3) {
                ray_t* arity_err = ray_sig_arity(ray_fn_name(head), "2", (int)(n-1));
                ray_release(head); ret = arity_err; goto out;
            }
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            char nm[16];
            memcpy(nm, ray_fn_name(head), 14); nm[14] = '\0';   /* inline name: 13 characters and a NUL */
            ray_binary_fn fn = (ray_binary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            if (fn_attrs & RAY_FN_SPECIAL_FORM) {
                bool inherits = form_inherits_rows(head);
                ray_release(head);
                ret = ray_sig_note(nm, fn(elems[1], elems[2]));
                rows = inherits ? g_eval_rows : 0;
                goto out;
            }
            if (fn == (ray_binary_fn)ray_take_fn && ray_select_form_whole(elems[1])) {
                /* (take (select ...) n), n > 0 a literal or a name the
                 * select cannot rebind: the select's first n rows alone; a select of fewer rows is
                 * its whole result, which take then repeats as for any table.  The shape is
                 * checked first, so no other take reads its count early. */
                ray_t* nv = take_count_arg(elems[2], elems[1]);
                int handled = 0;
                ray_t* top = nv ? ray_try_select_head_expr(elems[1], as_i64(nv), &handled) : NULL;
                if (handled) {
                    ray_release(head);
                    if (top && !RAY_IS_ERR(top) && top->type == RAY_TABLE &&
                        ray_table_nrows(top) < as_i64(nv)) {
                        ret = ray_take_fn(top, nv);
                        ray_release(top);
                    } else {
                        ret = top;
                    }
                    ray_release(nv);
                    rows = 0; goto out;
                }
                if (nv) ray_release(nv);
            }
            bool rowwise = ray_active_query_table() && vm_fn_rowwise(head);
            ray_t* left = ray_eval(elems[1]);
            int8_t left_rows = g_eval_rows;
            if (left && RAY_IS_ERR(left)) {
                ray_release(head);
                ret = left; goto out;
            }
            ray_t* right = ray_eval(elems[2]);
            int8_t right_rows = g_eval_rows;
            if (right && RAY_IS_ERR(right)) {
                ray_release(head); if (left) ray_release(left);
                ret = right; goto out;
            }
            /* Materialise lazy args for non-lazy-aware fns.
             * left/right are owned refs (returned from ray_eval); materialise
             * consumes them — no extra ray_release needed after each call. */
            if (!(fn_attrs & RAY_FN_LAZY_AWARE)) {
                if (left && ray_is_lazy(left)) {
                    left = ray_lazy_materialize(left); /* consumes owned ref */
                    if (!left || RAY_IS_ERR(left)) {
                        ray_release(head); if (right) ray_release(right);
                        ret = left ? left : ray_error("type", NULL); goto out;
                    }
                }
                if (right && ray_is_lazy(right)) {
                    right = ray_lazy_materialize(right); /* consumes owned ref */
                    if (!right || RAY_IS_ERR(right)) {
                        ray_release(head); if (left) ray_release(left);
                        ret = right ? right : ray_error("type", NULL); goto out;
                    }
                }
            }
            /* If either arg is NULL/void, only == and != can handle it */
            RAY_ASSERT_VALUE(left); RAY_ASSERT_VALUE(right);
            if (RAY_IS_NULL(left) || RAY_IS_NULL(right)) {
                if (fn == ray_identical_fn || fn == (ray_binary_fn)ray_eq_fn || fn == (ray_binary_fn)ray_neq_fn) {
                    ray_release(head);
                    ray_t* result = fn(left, right);
                    ray_release(left);
                    ray_release(right);
                    ret = result; goto out;
                }
                int8_t lt = left->type, rt = right->type;
                ray_release(head);
                ray_release(left);
                ray_release(right);
                ret = ray_error("type", "binary op: null operand not supported, got %s and %s", ray_type_name(lt), ray_type_name(rt)); goto out;
            }
            uint16_t fn_opcode = RAY_FN_OPCODE(head);
            ray_release(head);
            ray_t* result;
            rows = rowwise ? rows_join(left_rows, right_rows) : 0;
            if (summed && fn == ray_add_fn) {
                result = affine_literal(elems[1]) ? try_sum_affine(right, left) :
                         affine_literal(elems[2]) ? try_sum_affine(left, right) : NULL;
                if (result) {
                    *summed = true;
                    ray_release(left);
                    ray_release(right);
                    ret = result; goto out;
                }
            }
            if ((fn_attrs & RAY_FN_ATOMIC) && (is_collection(left) || is_collection(right)))
                result = atomic_map_binary_op(fn, fn_opcode, left, right);
            else if (fn == ray_concat_fn && ray_active_query_table() &&
                     (left->type == RAY_SYM || left->type == RAY_STR ||
                      left->type == -RAY_SYM || left->type == -RAY_STR) &&
                     (right->type == RAY_SYM || right->type == RAY_STR ||
                      right->type == -RAY_SYM || right->type == -RAY_STR)) {
                /* Row by row, a column's cell is an atom: it still carries
                 * the column's row provenance, and a STR cell its type.  An
                 * interpreted helper's result has unknown provenance; its
                 * syntax decides.  In the query's own expression two scalar
                 * text operands join; inside a function a call without row
                 * data among its operands keeps collection semantics. */
                bool lr = left_rows >= 0 ? left_rows > 0 : ray_query_expr_reads_rows(elems[1]);
                bool rr = right_rows >= 0 ? right_rows > 0 : ray_query_expr_reads_rows(elems[2]);
                bool scalar_ok = !ray_eval_query_helper_active() || lr || rr;
                if (scalar_ok && (ray_is_atom(left) || lr) && (ray_is_atom(right) || rr)) {
                    result = ray_concat_text_fn(left, right,
                                                (left->type == -RAY_STR && lr) ||
                                                (right->type == -RAY_STR && rr));
                    rows = lr || rr;
                } else {
                    result = fn(left, right);
                    rows = 0;
                }
            }
            else
                result = fn(left, right);
            result = ray_sig_note(nm, result);
            ray_release(left);
            ray_release(right);
            ret = result; goto out;
        }
        case RAY_VARY: {
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ray_vary_fn fn = (ray_vary_fn)(uintptr_t)head->i64;
            char nm[16];
            memcpy(nm, ray_fn_name(head), 14); nm[14] = '\0';   /* inline name: 13 characters and a NUL */
            /* insert or alter reached through any head but the literal name
             * or a global alias: an ordinary call, its operands evaluated
             * first, and its quoted target is no binding of this function. */
            bool callee_mutator = (head->attrs & RAY_FN_SPECIAL_FORM) &&
                                  ray_fn_is_local_mutator(head) &&
                                  !mutator_head_is_global(elems[0], head);
            if ((head->attrs & RAY_FN_SPECIAL_FORM) && !callee_mutator) {
                /* A control form returns one of its operands; any other
                 * form (a query, a table op) builds a new value. */
                bool inherits = form_inherits_rows(head);
                ray_release(head);
                ret = ray_sig_note(nm, fn(elems + 1, n - 1));
                rows = inherits ? g_eval_rows : 0;
                goto out;
            }
            int64_t argc = n - 1;
            /* A printed LIST can contain arbitrarily many cells.  Keep the
             * usual stack path and arity guard for other variadic calls. */
            if (argc > 64 && fn != ray_list_fn) { ray_release(head); ret = ray_error("domain", "call: too many args, max 64, got %lld", (long long)argc); goto out; }
            ray_t* stack_args[64];
            ray_t** args = stack_args;
            if (argc > 64) {
                if ((uint64_t)argc > SIZE_MAX / sizeof(ray_t*)) { ray_release(head); ret = ray_error("limit", NULL); goto out; }
                args = ray_alloc_raw((size_t)argc * sizeof(ray_t*));
                if (!args) { ray_release(head); ret = ray_error("oom", NULL); goto out; }
            }
            int8_t any_rows = 0;
            uint8_t argv_rows[64];   /* each argument's flag, for a row-wise builtin */
            for (int64_t i = 0; i < argc; i++) {
                /* .json.get's type, a quoted symbol after the path, names a
                 * type, never a column a query would read it as */
                ray_t* ai = elems[i + 1];
                if (i == 2 && fn == ray_json_get_fn && ai && ai->type == -RAY_SYM &&
                    (ai->attrs & ATTR_QUOTED)) {
                    ray_retain(ai);
                    args[i] = ai;
                    argv_rows[i] = 0;
                    continue;
                }
                args[i] = ray_eval(ai);
                any_rows = rows_join(any_rows, g_eval_rows);
                if (i < 64) argv_rows[i] = vrows_from(g_eval_rows);
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    if (args != stack_args) ray_free_raw(args);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            rows = fn == ray_return_fn ? any_rows
                 : any_rows && ray_active_query_table() && vm_fn_rowwise(head) ? any_rows : 0;
            if (!(head->attrs & RAY_FN_LAZY_AWARE)) {
                ray_t* err = materialize_owned_args(args, argc);
                if (err) { if (args != stack_args) ray_free_raw(args); ray_release(head); ret = err; goto out; }
            }
            uint8_t vary_attrs = head->attrs;
            ray_release(head);
            if (callee_mutator) ray_env_mutator_callee_arm();
            ray_t* result = NULL;
            if (RAY_UNLIKELY(vary_attrs & RAY_FN_ROWWISE) && argc <= 64) {   /* per row in a query (#1594) */
                result = rowwise_apply(RAY_VARY, (void*)(uintptr_t)fn, args, argv_rows, argc);
                if (result) rows = RAY_IS_ERR(result) ? 0 : 1;
            }
            if (!result) result = ray_sig_note(nm, fn(args, argc));
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            if (args != stack_args) ray_free_raw(args);
            ret = result; goto out;
        }
        case RAY_LAMBDA: {
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = ray_error("domain", "call: too many args, max 64, got %lld", (long long)argc); goto out; }
            ray_t* args[64];
            int8_t arg_rows[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                arg_rows[i] = g_eval_rows;
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            bool query = ray_active_query_table() != NULL;
            uint64_t row_args = 0;
            /* The arguments' evaluation says whether they are row data; only
             * an interpreted helper's result needs the syntactic analysis. */
            if (query) for (int64_t i = 0; i < argc; i++) {
                bool r = arg_rows[i] >= 0 ? arg_rows[i] > 0 : ray_query_expr_reads_rows(elems[i + 1]);
                if (r) row_args |= UINT64_C(1) << i;
            }
            bool named = elems[0]->type == -RAY_SYM && !(elems[0]->attrs & ATTR_QUOTED);
            int8_t result_rows = -1;
            ray_t* result = call_lambda_impl(head, args, argc, query, row_args, named, &result_rows);
            rows = result_rows;
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ray_release(head);
            if (RAY_IS_ERR(result))
                add_eval_error_frame(__VM->nfo, obj);
            ret = result; goto out;
        }
        default: {
            int8_t head_type = head->type;
            ray_release(head);
            if (obj->attrs & RAY_EVAL_LITERAL_FALLBACK) {
                ray_retain(obj);
                ret = obj; goto out;
            }
            ret = head_not_callable_error(head_type); goto out;
        }
    }

out:
    if (RAY_UNLIKELY(cp_frame >= 0)) ray_callprof_exit(cp_frame);
    /* Close the direct-builtin span opened before the dispatch switch.
     * ev_span is non-NULL only when profiling is active and we opened one,
     * so error/atom paths (which never reach the switch) skip this. */
    if (ev_span) {
        ray_prof_span_t* ep = ray_profile_span_end(ev_name);
        if (ep) {
            eval_span_payload(ep, ret);
            /* Noise gate: retract a trivial LEAF span so scalar ops
             * (`(+ 1 2)`, `(count 'x)`) don't flood the log.  A leaf has
             * its START and END adjacent (no child spans appended between);
             * keep it only if it moved bulk data (>1 row) or took real time
             * (≥50µs).  Ops with nested children are always kept — they
             * carry the tree structure. */
            if (ev_idx >= 0 && (int64_t)g_ray_profile.n == ev_idx + 2) {
                ray_prof_span_t* sp = &g_ray_profile.spans[ev_idx];
                int64_t dur_ns = ep->ts - sp->ts;
                if (ep->rows_out <= 1 && dur_ns < 50000)
                    g_ray_profile.n = (uint32_t)ev_idx;   /* drop START+END */
            }
        }
    }
    g_eval_rows = rows;
    __VM->eval_depth--;
    /* End-of-top-level-expression cleanup hook. Every path that
     * entered ray_eval — REPL, IPC, ray_eval_str, file mode — exits
     * through here; firing ray_progress_end exactly when the depth
     * returns to 0 guarantees the progress bar is cleared no matter
     * which builtin drove the update (including ray_group_indices_fn etc.
     * that bypass ray_execute). */
    if (__VM->eval_depth == 0) {
        /* Release pins before a final progress callback can start more work,
         * and before a caller is allowed to destroy its runtime. */
        affine_sum_cache_clear();
        /* a pool worker's own VM (pmap, #1248) ends no progress: the bar
         * is the dispatching thread's */
        if (!ray_pool_on_worker()) ray_progress_end();
    }
    return ret;
}

ray_t* ray_eval_str(const char* source) {
    ray_clear_error_trace();
    ray_raise_clear();
    ray_t* nfo = ray_nfo_create(RAY_NFO_REPL_NAME, strlen(RAY_NFO_REPL_NAME), source, strlen(source));
    ray_t* parsed = ray_parse_with_nfo(source, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); return parsed; }

    ray_t* prev_nfo = __VM->nfo;
    __VM->nfo = nfo;
    ray_t* result = ray_eval(parsed);
    __VM->nfo = prev_nfo;

    ray_release(parsed);
    ray_release(nfo);
    ray_eval_request_end();   /* the request's literals are gone with its text */
    /* ray_eval_str is the public top-level boundary — materialise any lazy
     * chain before returning to callers who expect a concrete value. */
    if (ray_is_lazy(result))
        result = ray_lazy_materialize(result);
    return result;
}

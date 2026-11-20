/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Call profiler: wall time per Rayfall call site.
 *
 *   While on, every builtin call compiled code makes (keyed by the lambda's
 *   bytecode and the call's offset) and every call top-level code makes
 *   (keyed by the parsed expression) is timed and aggregated into one row
 *   per site: calls, self and total wall time, and the pool workers' busy
 *   time spent under the call.  (.sys.profile) reads the rows as a table.
 *
 *   Off, a call site pays one predicted-not-taken test of g_ray_callprof.
 *   On, the main thread's VM is profiled; calls on other threads that own a
 *   VM (IPC hook threads, embedders) run unprofiled.  Pool workers run no
 *   Rayfall: their time reaches the profile as busy time of the call that
 *   dispatched them, which blocks until they finish.
 */

#ifndef RAY_CALLPROF_H
#define RAY_CALLPROF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <rayforce.h>

/* Distinct sites kept; calls at further sites fold into one `<other>` row. */
#define RAY_CALLPROF_SITES_MAX 4096
/* Nesting depth timed; calls deeper than this run untimed (their time stays
 * in the enclosing call's self time). */
#define RAY_CALLPROF_DEPTH_MAX 256

extern _Atomic(bool) g_ray_callprof;

static inline bool ray_callprof_on(void) {
    return atomic_load_explicit(&g_ray_callprof, memory_order_relaxed);
}

/* Open a frame for a call made by compiled code: `fn` is the running
 * lambda, `ip` an offset inside the call instruction.  Returns the frame
 * to pass to ray_callprof_exit, or -1 when the call is not timed. */
int32_t ray_callprof_enter_vm(ray_t* fn, int32_t ip);

/* The same for a call the tree walker makes at top level: `node` is the
 * call's parsed list, its span read from the running eval's source info.
 * Nodes without a span (built by C rather than parsed) are not timed. */
int32_t ray_callprof_enter_node(ray_t* node);

/* Close a frame opened above.  Always called, flag on or off, so a call
 * that switches profiling off (or resets it) still unwinds its frame. */
void ray_callprof_exit(int32_t frame);

void   ray_callprof_set_enabled(bool on);
void   ray_callprof_reset(void);
ray_t* ray_callprof_table(void);
/* Thread check for the control builtins: profiling state is the main VM's. */
bool   ray_callprof_owner(void);
/* Frames open: 0 whenever no timed call is running (tests). */
int32_t ray_callprof_depth(void);
/* Switch off and free everything (runtime teardown). */
void   ray_callprof_destroy(void);

#endif /* RAY_CALLPROF_H */

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
#ifndef RAY_RUNTIME_H
#define RAY_RUNTIME_H

#include <rayforce.h>

/* ===== Error Info (per-VM, ephemeral) ===== */

typedef struct {
    char msg[256];
} ray_err_info_t;

/* ===== Scope Frame ===== */

#define RAY_SCOPE_CAP  64
#define RAY_FRAME_CAP  64
#define RAY_SCOPE_LEXICAL 0
#define RAY_SCOPE_QUERY   1

/* One lexical or synthetic query scope frame.  keys/vals start out pointing
 * at the inline arrays and move to heap blocks if the frame grows past
 * RAY_FRAME_CAP (see env.c).  Self-referential, so a frame must not be
 * relocated while live. */
typedef struct {
    int64_t  keys_inline[RAY_FRAME_CAP];
    ray_t*   vals_inline[RAY_FRAME_CAP];
    int64_t* keys;     /* -> keys_inline, or heap once grown */
    ray_t**  vals;     /* -> vals_inline, or heap once grown */
    int32_t  cap;
    int32_t  count;
    uint8_t  kind;
} ray_scope_frame_t;

/* ===== Per-thread VM =====
 *
 * THE per-thread state object — one per thread, stable for the thread's
 * lifetime, published through `__VM`.  Process-wide state lives on
 * ray_runtime_t; everything thread-scoped lives here.  Bytecode
 * execution state is NOT here: the interpreter allocates a private
 * ray_exec_t (lang/eval.h) per invocation and never touches __VM.
 *
 * Always initialize through ray_vm_init() — plain memset(0) leaves
 * ipc_handle at 0, which is a VALID connection handle; the sentinel
 * for "no dispatch on this stack" is -1. */
typedef struct {
    /* ── hot: touched on every eval step / scope lookup.  Keep this
     * block within the first 32 bytes — ray_sys_alloc data starts at
     * page+32, so offsets 0..31 share one cache line. */
    int32_t          eval_depth;
    int32_t          scope_depth;
    bool             restricted; /* -U connection on this stack */
    int32_t          id;
    ray_t           *nfo;        /* source-info of the eval in progress (borrowed) */
    void            *heap;
    /* ── warm: per-dispatch context (core/ipc.c saves/restores) */
    int64_t          ipc_handle; /* connection handle of the hook/eval on this stack; -1 = none */
    void            *ipc_poll;   /* opaque ray_poll_t* that dispatched it; NULL = none */
    /* ── projection-pushdown hint (query eval): a consuming select publishes the
     * column syms it references before evaluating its `from:`; the first nested
     * `select {by:}` distinct consumes them and drops the rest.  Consume-once;
     * NULL/0/false defaults (memset) mean inactive. */
    const int64_t   *proj_keep;
    int              proj_keep_n;
    int32_t          proj_depth;  /* eval_depth at publish; consumed only at +1 */
    bool             proj_active;
    /* ── filter-compaction keep-set: a select about to execute its WHERE-filter
     * DAG publishes the column syms its (provably keys-only distinct) downstream
     * will read, so the top-level filter finalization (ray_execute_inner) gathers
     * only those columns instead of the whole table.  Scoped to one ray_execute
     * call via save/restore; consumed only at the publishing eval_depth.
     * NULL/0 (memset) = carry all columns (unchanged behaviour). */
    const int64_t   *filt_keep;
    int              filt_keep_n;
    int32_t          filt_depth;  /* eval_depth at publish; consumed only at == */
    /* ── group-by DA-eligibility hint: exec_group publishes each group key's
     * KNOWN cardinality (dict n_distinct today; extensible to other key types
     * with a cheaply-known TIGHT slot-span bound) so exec_group_v2's direct-
     * array check can reject an infeasible composite without a min/max prescan.
     * Consume-once per group; cleared at exec_group entry. memset-zero = none. */
    int64_t          grp_card_sym[8];
    int64_t          grp_card_val[8];
    uint8_t          grp_card_n;
    /* ── cold: error paths only */
    ray_t           *trace;      /* error trace list (owned) */
    ray_t           *raise_val;  /* pending (raise x) value (owned) */
    ray_err_info_t   err;
    /* ── big: lexical scope stack, own cache lines */
    ray_scope_frame_t scope_stack[RAY_SCOPE_CAP];
} ray_vm_t;

/* Zero the VM and set the non-zero defaults (ipc_handle = -1). */
void ray_vm_init(ray_vm_t* vm, int32_t id);

/* ===== Runtime ===== */

typedef struct ray_runtime_s {
    ray_vm_t       **vms;
    int32_t          n_vms;
    int64_t          total_ram;    /* total physical RAM, bytes */
    void            *poll;         /* opaque ray_poll_t* — see ray_runtime_(set|get)_poll */
    void            *sys_args;     /* opaque ray_t* dict — see ray_runtime_(set|get)_sys_args */
} ray_runtime_t;

/* Global runtime + per-thread VM */
extern ray_runtime_t *__RUNTIME;
extern _Thread_local ray_vm_t *__VM;

/* Lifecycle */
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void           ray_runtime_destroy(ray_runtime_t* rt);

/* Main event-loop accessors.  The host (main.c) registers the poll it
 * created; runtime-level builtins read it back through these to avoid
 * pulling poll.h into runtime.h. */
void  ray_runtime_set_poll(void* poll);
void* ray_runtime_get_poll(void);

/* Application arguments.  The host (main.c) builds a dict from argv via
 * ray_build_sys_args and registers it here; the `.sys.args` builtin reads
 * it back.  set takes ownership (released in ray_runtime_destroy). */
void   ray_runtime_set_sys_args(void* dict);
void*  ray_runtime_get_sys_args(void);
ray_t* ray_build_sys_args(int argc, char** argv);

/* Persistent-consumer lifecycle: load the sym table from `sym_path` (if
 * present) before builtins register, so user-interned IDs keep the same
 * slots across process restarts.  The _err variant surfaces the load
 * result via `out_sym_err` (RAY_OK / RAY_ERR_CORRUPT / I/O errors) so
 * callers can decide recovery policy; the plain variant discards it. */
ray_runtime_t* ray_runtime_create_with_sym(const char* sym_path);
ray_runtime_t* ray_runtime_create_with_sym_err(const char* sym_path,
                                               ray_err_t* out_sym_err);

/* Error API — allocates ray_t with type=RAY_ERROR, sets __VM->err.msg */
ray_t* ray_error(const char* code, const char* fmt, ...);
/* Read error code from a RAY_ERROR object (returns pointer to sdata) */
const char* ray_err_code(ray_t* err);
/* ray_error_free() is published in include/rayforce.h */

/* Read VM error detail message (NULL if empty) */
const char* ray_error_msg(void);

/* Clear VM error detail */
void ray_error_clear(void);

#endif /* RAY_RUNTIME_H */

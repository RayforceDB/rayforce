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

#include "lang/env.h"
#include "core/runtime.h"
#include "core/platform.h"   /* RAY_CPU_RELAX */
#include "table/sym.h"
#include "table/dict.h"
#include "ops/temporal.h"
#include "ops/linkop.h"
#include "mem/sys.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---- Function constructors ---- */

/* Builtin name stored inline in aux[2..15] (max 13 chars + null).
 * Bytes 0-1 reserved for DAG opcode (any type, not just binary). */
static void fn_set_name(ray_t* obj, const char* name) {
    memset(obj->aux, 0, 16);
    size_t len = strlen(name);
    if (len > 13) len = 13;
    memcpy(obj->aux + 2, name, len);
}

ray_t* ray_fn_unary(const char* name, uint8_t fn_attrs, ray_unary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_UNARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

ray_t* ray_fn_binary(const char* name, uint8_t fn_attrs, ray_binary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_BINARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

ray_t* ray_fn_vary(const char* name, uint8_t fn_attrs, ray_vary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_VARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

/* ---- Global environment ---- */

/* Spinlock protecting g_env mutations in ray_env_set */
static _Atomic(int) g_env_lock = 0;
static inline void env_lock(void) {
    while (atomic_exchange_explicit(&g_env_lock, 1, memory_order_acquire)) {
        RAY_CPU_RELAX();
    }
}
static inline void env_unlock(void) {
    atomic_store_explicit(&g_env_lock, 0, memory_order_release);
}

#define ENV_CAP 1024

static struct {
    int64_t keys[ENV_CAP];
    ray_t*   vals[ENV_CAP];
    /* Per-slot flag: 1 iff this binding was last written by user code
     * (ray_env_set / ray_env_set_local-promoted-to-global), 0 if the
     * latest writer was builtin registration (ray_env_bind / _flat).
     * Powers ray_env_list_user, which the journal snapshot uses to
     * pick which globals to dump to <base>.qdb.  A user `(set + 42)`
     * over a builtin flips the slot to user=1 so the override is
     * preserved across snapshot/restore. */
    uint8_t  user[ENV_CAP];
    int32_t count;
} g_env;

/* ---- Local scope stack ---- */

/* The scope stack lives on the per-thread VM (core/runtime.h) — one
 * per thread, stable across bytecode invocations.  ray_scope_frame_t
 * and the caps (RAY_SCOPE_CAP / RAY_FRAME_CAP) are defined there.
 * Callers reach env state through eval, whose entry guard rejects
 * threads with no bound VM, so plain __VM derefs are safe below. */

int32_t ray_env_scope_depth(void) { return __VM ? __VM->scope_depth : 0; }
int32_t ray_env_global_count(void) { return g_env.count; }

/* Query scopes are synthetic and must not eclipse a caller's lexical
 * parameter/let binding.  Other query frames are deliberately ignored so
 * an inner query can bind a same-named column over an outer query column.
 * Query-local callers push a query frame before probing, so scanning every
 * frame still excludes the binding currently being installed. */
bool ray_env_has_lexical_local(int64_t sym_id) {
    if (!__VM) return false;
    for (int32_t d = __VM->scope_depth - 1; d >= 0; d--) {
        ray_scope_frame_t* f = &__VM->scope_stack[d];
        if (f->kind == RAY_SCOPE_QUERY) continue;
        for (int32_t i = 0; i < f->count; i++)
            if (f->keys[i] == sym_id) return true;
    }
    return false;
}

ray_err_t ray_env_set_query_local(int64_t sym_id, ray_t* val) {
    return ray_env_has_lexical_local(sym_id) ? RAY_OK
                                              : ray_env_set_local(sym_id, val);
}

ray_t* ray_env_capture_locals(void) {
    if (!__VM || __VM->scope_depth == 0) return NULL;
    int64_t capacity = 0;
    for (int32_t d = 0; d < __VM->scope_depth; d++)
        capacity += __VM->scope_stack[d].count;
    if (capacity == 0) return NULL;

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, capacity);
    ray_t* vals = ray_list_new(capacity);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return ray_error("oom", NULL);
    }

    int64_t* key_ids = (int64_t*)ray_data(keys);
    /* Top-to-bottom flattening preserves ordinary lexical lookup: the first
     * occurrence of a name is the value visible at closure creation. */
    for (int32_t d = __VM->scope_depth - 1; d >= 0; d--) {
        ray_scope_frame_t* f = &__VM->scope_stack[d];
        for (int32_t i = 0; i < f->count; i++) {
            if (!f->vals[i]) continue;
            bool seen = false;
            for (int64_t k = 0; k < keys->len; k++)
                if (key_ids[k] == f->keys[i]) { seen = true; break; }
            if (seen) continue;
            key_ids[keys->len++] = f->keys[i];
            vals = ray_list_append(vals, f->vals[i]);
            if (!vals || RAY_IS_ERR(vals)) {
                ray_release(keys);
                return vals ? vals : ray_error("oom", NULL);
            }
        }
    }
    if (keys->len == 0) {
        ray_release(keys);
        ray_release(vals);
        return NULL;
    }
    return ray_dict_new(keys, vals);
}

ray_err_t ray_env_push_capture(ray_t* capture) {
    if (ray_env_push_scope() != RAY_OK) return RAY_ERR_OOM;
    if (!capture) return RAY_OK;
    if (capture->type != RAY_DICT) {
        ray_env_pop_scope();
        return RAY_ERR_TYPE;
    }
    ray_t* keys = ray_dict_keys(capture);
    ray_t* vals = ray_dict_vals(capture);
    if (!keys || keys->type != RAY_SYM || !vals || vals->type != RAY_LIST ||
        keys->len != vals->len) {
        ray_env_pop_scope();
        return RAY_ERR_TYPE;
    }
    ray_t** value_items = (ray_t**)ray_data(vals);
    for (int64_t i = 0; i < keys->len; i++) {
        int64_t sym = ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs);
        ray_err_t err = ray_env_set_local(sym, value_items[i]);
        if (err != RAY_OK) {
            ray_env_pop_scope();
            return err;
        }
    }
    return RAY_OK;
}

/* The five connection-hook sym ids carved out of the reserved-name reject.
 * Populated lazily on first probe; idempotent — ray_sym_intern is content-
 * keyed, so repeated calls return the same id.  The carve-out applies ONLY
 * to these five names; everything else under `.ipc.*` (open/send/close/
 * handle) stays unsettable, as do all `.sys.*` / `.os.*` / etc.
 * Lambda parameters with these names remain forbidden — that's pure
 * shadowing with no legitimate use. */
static int64_t g_ipc_hook_syms[5] = {0};
static bool    g_ipc_hook_syms_ready = false;

/* Cached `self` sym id for ray_env_scope_bind — interned once per sym
 * table generation; invalidated alongside the hook syms below. */
static int64_t g_self_sym = -1;

static void ipc_hook_syms_ensure(void) {
    if (g_ipc_hook_syms_ready) return;
    g_ipc_hook_syms[0] = ray_sym_intern(".ipc.on.open",  strlen(".ipc.on.open"));
    g_ipc_hook_syms[1] = ray_sym_intern(".ipc.on.close", strlen(".ipc.on.close"));
    g_ipc_hook_syms[2] = ray_sym_intern(".ipc.on.sync",  strlen(".ipc.on.sync"));
    g_ipc_hook_syms[3] = ray_sym_intern(".ipc.on.async", strlen(".ipc.on.async"));
    g_ipc_hook_syms[4] = ray_sym_intern(".ipc.on.auth",  strlen(".ipc.on.auth"));
    g_ipc_hook_syms_ready = true;
}

bool ray_sym_is_ipc_hook(int64_t sym_id) {
    ipc_hook_syms_ensure();
    for (int i = 0; i < 5; i++)
        if (g_ipc_hook_syms[i] == sym_id) return true;
    return false;
}

int64_t ray_sym_ipc_hook(int idx) {
    if (idx < 0 || idx >= 5) return -1;
    ipc_hook_syms_ensure();
    return g_ipc_hook_syms[idx];
}

ray_err_t ray_env_init(void) {
    memset(&g_env, 0, sizeof(g_env));
    __VM->scope_depth = 0;
    /* The IPC hook sym IDs are interned lazily on first probe via
     * `ipc_hook_syms_ensure`.  We deliberately do NOT pre-intern here:
     * single-char operator names (`+`, `-`, `*`, …) are registered just
     * after `ray_env_init` returns, and they need the low sym-ID slots.
     * Several call sites (notably `resolve` on tables of small ints)
     * depend on those low IDs corresponding to operator chars so the
     * "slen < 2 / operator-char" guard rejects them.  Stealing IDs 1-5
     * for long dotted names would silently promote `[1 2 3]` columns to
     * SYM.  Lazy interning runs after all builtin registration, so the
     * hook syms land safely past the operator block. */
    return RAY_OK;
}

void ray_env_destroy(void) {
    /* Pop any remaining scopes */
    while (__VM->scope_depth > 0) ray_env_pop_scope();
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.vals[i]) ray_release(g_env.vals[i]);
    }
    memset(&g_env, 0, sizeof(g_env));
    /* Invalidate the IPC hook sym cache: the runtime destroy path tears
     * down the sym table too, so the cached sym IDs in `g_ipc_hook_syms`
     * are stale by the time the next runtime spins up.  Reset the flag
     * so the next ray_env_init re-interns fresh IDs from the new sym
     * table. */
    g_ipc_hook_syms_ready = false;
    g_self_sym = -1;
}

/* Flat (non-dotted) lookup — scope stack top-down, then global env.
 * Returns NULL if not bound.  Always used as the head-segment resolver
 * for dotted paths, and as the fast path for plain names. */
static ray_t* env_lookup_flat(int64_t sym_id) {
    for (int32_t d = __VM->scope_depth - 1; d >= 0; d--) {
        ray_scope_frame_t* f = &__VM->scope_stack[d];
        for (int32_t i = 0; i < f->count; i++) {
            if (f->keys[i] == sym_id) return f->vals[i];
        }
    }
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) return g_env.vals[i];
    }
    return NULL;
}

ray_t* ray_env_get_local(int64_t sym_id) {
    if (!__VM) return NULL;
    for (int32_t d = __VM->scope_depth - 1; d >= 0; d--) {
        ray_scope_frame_t* f = &__VM->scope_stack[d];
        for (int32_t i = 0; i < f->count; i++)
            if (f->keys[i] == sym_id) return f->vals[i];
    }
    return NULL;
}

ray_t* ray_env_get(int64_t sym_id) {
    /* Flat lookup first — covers every non-dotted name AND every
     * reserved builtin like `.sys.gc` which is bound both flat (for
     * O(1) resolution + prefix enumeration) and inside the `.sys`
     * namespace dict (for REPL introspection). */
    ray_t* flat = env_lookup_flat(sym_id);
    if (flat) return flat;
    if (!ray_sym_is_dotted(sym_id)) return NULL;

    /* Dotted walk: head resolves via scope+global, rest are sym-keyed
     * container probes — dicts probe the keys SYM vec and read the
     * matching slot from the vals LIST, tables look up by schema sym
     * id, anything else is surfaced as "undefined" (NULL).  Missing
     * intermediate keys also return NULL so the evaluator's name-error
     * reporting stays consistent with plain names.  Returning env-owned
     * pointers (never fresh allocations) keeps the caller's retain/release
     * balance correct. */
    const int64_t* segs;
    int n = ray_sym_segs(sym_id, &segs);
    if (n < 2) return NULL;  /* defensive — dotted bit without segments */

    ray_t* v = env_lookup_flat(segs[0]);
    for (int i = 1; v && i < n; i++) {
        v = ray_container_probe_sym(v, segs[i]);
    }
    return v;
}

/* Owned-ref variant.  Always returns rc>=1 on success; caller must
 * release.  Additionally handles temporal field extraction in the dotted
 * walk (e.g. `date.dd`, `ts.hh`) — when the next container-probe step
 * would fail and the current value is a RAY_DATE / RAY_TIME /
 * RAY_TIMESTAMP vector or atom, we try mapping the segment sym to a
 * RAY_EXTRACT_* field and call ray_temporal_extract, which allocates a
 * fresh result.  Those fresh allocations are exactly why this function
 * has a different retain contract from ray_env_get. */
ray_t* ray_env_resolve(int64_t sym_id) {
    /* Flat lookup first — short-circuits dotted reserved builtins
     * (`.sys.gc`, `.os.getenv`, …) that are additionally bound flat
     * alongside their namespace dict.  Non-dotted names take the
     * same path. */
    ray_t* flat = env_lookup_flat(sym_id);
    if (flat) { ray_retain(flat); return flat; }
    if (!ray_sym_is_dotted(sym_id)) return NULL;

    const int64_t* segs;
    int n = ray_sym_segs(sym_id, &segs);
    if (n < 2) return NULL;

    /* `v` is either a borrowed env/container pointer (fresh=false) or a
     * fresh temporal-extract result (fresh=true).  When switching between
     * the two we must release the previous fresh value to avoid leaks. */
    ray_t* v = env_lookup_flat(segs[0]);
    bool   fresh = false;

    for (int i = 1; v && i < n; i++) {
        ray_t* next = NULL;
        bool   next_fresh = false;
        /* Linked column: deref segs[i] as a target field name (returns
         * a fresh owning result, columns the same length as v).  Errors
         * from ray_link_deref (e.g. "nyi: target table has a parted
         * column") must be surfaced to the caller — silently downgrading
         * to NULL would convert a real wrong-answer-bug guard into a
         * confusing "name undefined" message. */
        if (ray_link_has(v)) {
            next = ray_link_deref(v, segs[i]);
            if (next && RAY_IS_ERR(next)) {
                if (fresh) ray_release(v);
                return next;
            }
            next_fresh = (next != NULL);
        }
        if (!next) next = ray_container_probe_sym(v, segs[i]);
        if (next) {
            if (fresh) ray_release(v);
            v = next;
            fresh = next_fresh;
            continue;
        }

        /* Container probe miss — try method dispatch: look up the
         * segment as a callable in env, and if it's a unary function,
         * apply it to the current value.  This makes `ts.ss`, `d.dd`,
         * or any future `x.some_fn` work the same way, with the
         * segment resolution going through the normal function
         * registration path instead of a bespoke table.
         *
         * Walk both scope and global env looking for a RAY_UNARY
         * binding — a local non-callable (e.g. a column named `ss`
         * pushed into scope by the select fallback) must not shadow
         * the globally-registered accessor function. */
        ray_t* fn = NULL;
        for (int32_t d = __VM->scope_depth - 1; d >= 0 && !fn; d--) {
            ray_scope_frame_t* f = &__VM->scope_stack[d];
            for (int32_t k = 0; k < f->count; k++) {
                if (f->keys[k] == segs[i] && f->vals[k]
                    && f->vals[k]->type == RAY_UNARY) {
                    fn = f->vals[k];
                    break;
                }
            }
        }
        if (!fn) {
            for (int32_t k = 0; k < g_env.count; k++) {
                if (g_env.keys[k] == segs[i] && g_env.vals[k]
                    && g_env.vals[k]->type == RAY_UNARY) {
                    fn = g_env.vals[k];
                    break;
                }
            }
        }
        if (fn) {
            ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
            ray_t* r = f(v);
            if (fresh) ray_release(v);
            if (!r || RAY_IS_ERR(r)) return NULL;
            v = r;
            fresh = true;
            continue;
        }

        /* Nothing matched — propagate "undefined". */
        if (fresh) ray_release(v);
        return NULL;
    }

    if (!v) return NULL;
    if (!fresh) ray_retain(v);   /* hand back an owned ref */
    return v;
}

/* Flat-binding helpers: mutate a specific scope (global or top frame) by
 * sym_id.  Used by both the simple and dotted set paths.  Passing val=NULL
 * means "delete" — if a slot exists, release its value and compact the
 * slot out of the array (no-op if the slot doesn't exist).  This matches
 * ray_del_fn's contract via ray_env_set(sym, NULL) and also covers the
 * cascade-up case in env_set_dotted where every dict in a dotted path was
 * emptied by the delete. */
static ray_err_t env_bind_global_impl(int64_t sym_id, ray_t* val, int is_user) {
    env_lock();
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) {
            if (val == NULL) {
                if (g_env.vals[i]) ray_release(g_env.vals[i]);
                for (int32_t j = i; j + 1 < g_env.count; j++) {
                    g_env.keys[j] = g_env.keys[j + 1];
                    g_env.vals[j] = g_env.vals[j + 1];
                    g_env.user[j] = g_env.user[j + 1];
                }
                g_env.count--;
                env_unlock();
                return RAY_OK;
            }
            if (g_env.vals[i]) ray_release(g_env.vals[i]);
            ray_retain(val);
            g_env.vals[i] = val;
            /* User write upgrades a builtin slot to user-defined, so a
             * (set + 42) override survives snapshot/restore.  A builtin
             * re-bind (e.g. theoretical hot reload) leaves the existing
             * flag alone — once user, always user, until the slot is
             * deleted. */
            if (is_user) g_env.user[i] = 1;
            env_unlock();
            return RAY_OK;
        }
    }
    if (val == NULL) {   /* deleting an absent binding: no-op */
        env_unlock();
        return RAY_OK;
    }
    if (g_env.count >= ENV_CAP) {
        env_unlock();
        return RAY_ERR_OOM;
    }
    g_env.keys[g_env.count] = sym_id;
    ray_retain(val);
    g_env.vals[g_env.count] = val;
    g_env.user[g_env.count] = is_user ? 1 : 0;
    g_env.count++;
    env_unlock();
    return RAY_OK;
}

/* Function-pointer-shaped wrapper used by env_set_dotted's bind_fn
 * indirection — preserves the existing signature. */
static ray_err_t env_bind_global(int64_t sym_id, ray_t* val) {
    return env_bind_global_impl(sym_id, val, 0);
}

/* User-flagged sibling: identical except the slot is marked user=1.
 * Used by ray_env_set and the dotted-set path it drives. */
static ray_err_t env_bind_global_user(int64_t sym_id, ray_t* val) {
    return env_bind_global_impl(sym_id, val, 1);
}

static ray_err_t env_bind_local(int64_t sym_id, ray_t* val) {
    ray_scope_frame_t* f = &__VM->scope_stack[__VM->scope_depth - 1];
    for (int32_t i = 0; i < f->count; i++) {
        if (f->keys[i] == sym_id) {
            if (val == NULL) {
                if (f->vals[i]) ray_release(f->vals[i]);
                for (int32_t j = i; j + 1 < f->count; j++) {
                    f->keys[j] = f->keys[j + 1];
                    f->vals[j] = f->vals[j + 1];
                }
                f->count--;
                return RAY_OK;
            }
            if (f->vals[i]) ray_release(f->vals[i]);
            ray_retain(val);
            f->vals[i] = val;
            return RAY_OK;
        }
    }
    if (val == NULL) return RAY_OK;
    if (f->count >= f->cap) {
        int32_t new_cap = f->cap * 2;
        int64_t* nk = (int64_t*)ray_sys_alloc(sizeof(int64_t) * (size_t)new_cap);
        ray_t**  nv = (ray_t**)ray_sys_alloc(sizeof(ray_t*)  * (size_t)new_cap);
        if (!nk || !nv) {
            if (nk) ray_sys_free(nk);
            if (nv) ray_sys_free(nv);
            return RAY_ERR_OOM;
        }
        memcpy(nk, f->keys, sizeof(int64_t) * (size_t)f->count);
        memcpy(nv, f->vals, sizeof(ray_t*)  * (size_t)f->count);
        if (f->keys != f->keys_inline) ray_sys_free(f->keys);
        if (f->vals != f->vals_inline) ray_sys_free(f->vals);
        f->keys = nk;
        f->vals = nv;
        f->cap = new_cap;
    }
    f->keys[f->count] = sym_id;
    ray_retain(val);
    f->vals[f->count] = val;
    f->count++;
    return RAY_OK;
}

/* Dotted-path write.  base_lookup(head_sym) returns the current binding in
 * the scope we are writing to (global or local frame), or NULL.  bind_fn
 * rebinds the new top-level dict in that same scope.  Walks the existing
 * chain (if any) for intermediate dicts, then COW-rebuilds bottom-up using
 * dict_upsert.  Auto-creates missing intermediates as empty dicts. */
static ray_err_t env_set_dotted(int64_t sym_id, ray_t* val,
                                ray_t* (*base_lookup)(int64_t),
                                ray_err_t (*bind_fn)(int64_t, ray_t*)) {
    const int64_t* segs;
    int n = ray_sym_segs(sym_id, &segs);
    if (n < 2) return RAY_ERR_TYPE;   /* dotted flag without segments */

    /* Walk existing chain to the deepest parent that still exists.  Record
     * each level's dict pointer (borrowed) so we can rebuild upward.  Any
     * non-dict intermediate is an error. */
    ray_t* parents[256];
    parents[0] = base_lookup(segs[0]);
    if (parents[0] && parents[0]->type != RAY_DICT)
        return RAY_ERR_TYPE;

    /* parents[i] is the dict at path prefix segs[0..i].  If an intermediate
     * key is missing, parents[i+1..n-2] are NULL and ray_dict_upsert will
     * create fresh dicts on the way back up. */
    for (int i = 1; i < n - 1; i++) {
        if (!parents[i - 1]) { parents[i] = NULL; continue; }
        ray_t* child = ray_dict_probe_sym_borrowed(parents[i - 1], segs[i]);
        if (child && child->type != RAY_DICT)
            return RAY_ERR_TYPE;
        parents[i] = child;
    }

    /* Delete path: (del ns.x) lowers to ray_env_set(sym_id, NULL).  The
     * non-dotted path removes the env slot; the dotted path must actually
     * remove the key from the leaf dict and rebuild the chain — otherwise
     * the user would see a zombie entry like {:x NULL} instead of the
     * key being gone.  No-op cleanly if any part of the path is missing.
     * If the leaf-removal empties the containing dict, we must not rebind
     * {} upward — that would leave a stale empty namespace.  Instead
     * cascade up: at each level, if `cur` is empty, delete that key from
     * its parent instead of upserting it.  If the cascade reaches the
     * head with an empty dict, we rebind the head to NULL (env_bind_*
     * treats NULL as "remove the slot"). */
    int start_i;
    ray_t* cur;
    bool deleting = (val == NULL);
    if (deleting) {
        ray_t* leaf_parent = parents[n - 2];
        if (!leaf_parent) return RAY_OK;
        if (!ray_dict_probe_sym_borrowed(leaf_parent, segs[n - 1])) return RAY_OK;
        ray_retain(leaf_parent);
        ray_t* k = ray_sym(segs[n - 1]);
        cur = ray_dict_remove(leaf_parent, k);
        ray_release(k);
        if (!cur || RAY_IS_ERR(cur)) return RAY_ERR_OOM;
        start_i = n - 2;   /* rebuild from the parent of the deleted key up */
    } else {
        ray_retain(val);
        cur = val;
        start_i = n - 1;
    }

    /* Build new chain bottom-up.  ray_dict_upsert consumes its `dict` arg,
     * so we retain parents before passing.  Missing-parent levels are
     * created from a fresh empty dict.  On failure we release cur and bail
     * — parents are env-owned borrowed refs. */
    for (int i = start_i; i >= 1; i--) {
        ray_t* parent = parents[i - 1];

        if (deleting && cur && cur->type == RAY_DICT && ray_dict_len(cur) == 0) {
            /* Cascade: the rebuilt child became empty, so remove the key
             * at this level rather than storing {}.  If parent is absent
             * too, nothing more to do. */
            ray_release(cur);
            if (!parent) { cur = NULL; break; }
            ray_retain(parent);
            ray_t* k = ray_sym(segs[i]);
            cur = ray_dict_remove(parent, k);
            ray_release(k);
            if (!cur || RAY_IS_ERR(cur)) return RAY_ERR_OOM;
            continue;
        }

        ray_t* dict_in;
        if (parent) {
            ray_retain(parent);
            dict_in = parent;
        } else {
            ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 1);
            ray_t* vals = ray_list_new(1);
            dict_in = ray_dict_new(keys, vals);
            if (!dict_in || RAY_IS_ERR(dict_in)) { ray_release(cur); return RAY_ERR_OOM; }
        }
        ray_t* k = ray_sym(segs[i]);
        ray_t* next = ray_dict_upsert(dict_in, k, cur);
        ray_release(k);
        ray_release(cur);
        if (!next || RAY_IS_ERR(next)) return RAY_ERR_OOM;
        cur = next;
    }

    /* If cascade reduced the head-level dict to empty (or propagated up
     * past a missing parent), rebind the head as NULL so the stale empty
     * namespace disappears from introspection and from future lookups. */
    ray_t* to_bind = cur;
    if (deleting && cur && cur->type == RAY_DICT && ray_dict_len(cur) == 0) {
        to_bind = NULL;
    }
    ray_err_t err = bind_fn(segs[0], to_bind);
    if (cur) ray_release(cur);
    return err;
}

/* Scope-specific base lookups used by env_set_dotted. */
static ray_t* lookup_global(int64_t sym_id) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) return g_env.vals[i];
    }
    return NULL;
}

static ray_t* lookup_top_frame(int64_t sym_id) {
    if (__VM->scope_depth <= 0) return NULL;
    ray_scope_frame_t* f = &__VM->scope_stack[__VM->scope_depth - 1];
    for (int32_t i = 0; i < f->count; i++) {
        if (f->keys[i] == sym_id) return f->vals[i];
    }
    return NULL;
}

/* A sym belongs to the reserved system namespace if its name starts with
 * a dot (e.g. `.sys.gc`, `.os.getenv`).  The leading segment is the
 * category tag; builtin registration populates these via ray_env_bind
 * and every user-level binder refuses such names so the system
 * bindings can't be shadowed in any scope. */
bool ray_sym_is_reserved(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return false;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    return n > 0 && p && p[0] == '.';
}

ray_err_t ray_env_bind(int64_t sym_id, ray_t* val) {
    if (ray_sym_is_dotted(sym_id)) {
        return env_set_dotted(sym_id, val, lookup_global, env_bind_global);
    }
    return env_bind_global(sym_id, val);
}

ray_err_t ray_env_bind_flat(int64_t sym_id, ray_t* val) {
    return env_bind_global(sym_id, val);
}

ray_err_t ray_env_set(int64_t sym_id, ray_t* val) {
    /* Reserved-namespace gate, with a narrow carve-out for the five IPC
     * connection hooks.  Everything else under `.ipc.*` and all other
     * dotted system namespaces remain unsettable. */
    if (ray_sym_is_reserved(sym_id) && !ray_sym_is_ipc_hook(sym_id))
        return RAY_ERR_RESERVED;
    /* Same machinery as ray_env_bind, but routes through the user-flagged
     * binder so the journal snapshot can pick this slot.  Without this
     * flip, env_bind_global would also be reached via ray_env_bind below
     * and the slot would carry user=0 — leaving it out of <base>.qdb. */
    if (ray_sym_is_dotted(sym_id))
        return env_set_dotted(sym_id, val, lookup_global, env_bind_global_user);
    return env_bind_global_user(sym_id, val);
}

static ray_err_t env_push_scope(uint8_t kind) {
    if (__VM->scope_depth >= RAY_SCOPE_CAP) return RAY_ERR_OOM;
    ray_scope_frame_t* f = &__VM->scope_stack[__VM->scope_depth];
    f->keys = f->keys_inline;
    f->vals = f->vals_inline;
    f->cap = RAY_FRAME_CAP;
    f->count = 0;
    f->kind = kind;
    __VM->scope_depth++;
    return RAY_OK;
}

ray_err_t ray_env_push_scope(void) {
    return env_push_scope(RAY_SCOPE_LEXICAL);
}

ray_err_t ray_env_push_query_scope(void) {
    return env_push_scope(RAY_SCOPE_QUERY);
}

void ray_env_pop_scope(void) {
    if (__VM->scope_depth <= 0) return;
    __VM->scope_depth--;
    ray_scope_frame_t* f = &__VM->scope_stack[__VM->scope_depth];
    for (int32_t i = 0; i < f->count; i++) {
        if (f->vals[i]) ray_release(f->vals[i]);
    }
    if (f->keys != f->keys_inline) ray_sys_free(f->keys);
    if (f->vals != f->vals_inline) ray_sys_free(f->vals);
    f->keys = f->keys_inline;
    f->vals = f->vals_inline;
    f->cap = RAY_FRAME_CAP;
    f->count = 0;
    f->kind = RAY_SCOPE_LEXICAL;
}

/* Materialize compiled-lambda locals into a fresh scope frame — the
 * compiled counterpart of call_lambda's tree-walk frame.  A runtime
 * special form re-evaluates its argument ASTs via ray_eval, which
 * resolves names through scope frames + globals and cannot see the
 * caller's bytecode slots; this bridges the two.  NULL slots are
 * let-locals whose initializer hasn't executed yet: in the tree walker
 * they wouldn't be bound at this point either, so they're skipped. */
ray_err_t ray_env_scope_bind(const int64_t* syms, int32_t n, ray_t** slots,
                             ray_t* self_obj) {
    ray_err_t err = ray_env_push_scope();
    if (err != RAY_OK) return err;
    if (self_obj) {
        if (g_self_sym < 0) g_self_sym = ray_sym_intern("self", 4);
        (void)ray_env_set_local(g_self_sym, self_obj);
    }
    for (int32_t i = 0; i < n; i++) {
        if (!slots[i]) continue;
        ray_err_t e = ray_env_set_local(syms[i], slots[i]);
        if (e != RAY_OK && e != RAY_ERR_RESERVED) {
            ray_env_pop_scope();
            return e;
        }
    }
    return RAY_OK;
}

/* Sync the materialized frame back into the slots and pop it.  Only
 * syms from the compiler's table sync — a `let` inside the dynamic
 * eval that introduced a name the compiler never saw has no slot to
 * land in (the tree walker would keep it in the lambda frame; compiled
 * code drops it with this frame — names must be introduced lexically
 * for the compiler to allocate a slot, same as before). */
void ray_env_scope_sync(const int64_t* syms, int32_t n, ray_t** slots) {
    if (__VM->scope_depth > 0) {
        ray_scope_frame_t* f = &__VM->scope_stack[__VM->scope_depth - 1];
        for (int32_t i = 0; i < n; i++) {
            for (int32_t k = 0; k < f->count; k++) {
                if (f->keys[k] != syms[i]) continue;
                if (f->vals[k] != slots[i]) {
                    ray_retain(f->vals[k]);
                    if (slots[i]) ray_release(slots[i]);
                    slots[i] = f->vals[k];
                }
                break;
            }
        }
    }
    ray_env_pop_scope();
}

/* ---- Iteration ---- */

int32_t ray_env_list(int64_t* sym_ids, ray_t** vals, int32_t max_entries) {
    int32_t n = g_env.count < max_entries ? g_env.count : max_entries;
    for (int32_t i = 0; i < n; i++) {
        sym_ids[i] = g_env.keys[i];
        vals[i] = g_env.vals[i];
    }
    return n;
}

int32_t ray_env_list_user(int64_t* sym_ids, ray_t** vals, int32_t max_entries) {
    int32_t out = 0;
    for (int32_t i = 0; i < g_env.count && out < max_entries; i++) {
        if (!g_env.user[i]) continue;
        sym_ids[out] = g_env.keys[i];
        vals[out]    = g_env.vals[i];
        out++;
    }
    return out;
}

/* ---- Prefix lookup ---- */

static const char* s_keywords[] = {
    "def", "do", "false", "fn", "if", "let", "set", "true", NULL
};

/* Compare helper for qsort on const char* */
static int cmp_str_ptr(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

bool ray_env_has_name(const char* name, int64_t len) {
    if (!name || len <= 0) return false;
    for (int32_t i = 0; i < g_env.count; i++) {
        ray_t* s = ray_sym_str(g_env.keys[i]);
        if (!s) continue;
        const char* n = ray_str_ptr(s);
        if (!n) continue;
        if ((int64_t)strlen(n) == len && memcmp(n, name, (size_t)len) == 0)
            return true;
    }
    for (const char** kw = s_keywords; *kw; kw++) {
        if ((int64_t)strlen(*kw) == len && memcmp(*kw, name, (size_t)len) == 0)
            return true;
    }
    return false;
}

int64_t ray_env_lookup_prefix(const char* prefix, int64_t len,
                              const char** results, int64_t max_results) {
    int64_t count = 0;

    /* Scan global env keys */
    for (int32_t i = 0; i < g_env.count && count < max_results; i++) {
        ray_t* s = ray_sym_str(g_env.keys[i]);
        if (!s) continue;
        const char* name = ray_str_ptr(s);
        if (!name) continue;
        int64_t nlen = (int64_t)strlen(name);
        if (nlen >= len && strncmp(name, prefix, (size_t)len) == 0) {
            /* Deduplicate against what we already have */
            int dup = 0;
            for (int64_t j = 0; j < count; j++) {
                if (strcmp(results[j], name) == 0) { dup = 1; break; }
            }
            if (!dup) results[count++] = name;
        }
    }

    /* Scan static keyword list */
    for (const char** kw = s_keywords; *kw && count < max_results; kw++) {
        int64_t klen = (int64_t)strlen(*kw);
        if (klen >= len && strncmp(*kw, prefix, (size_t)len) == 0) {
            int dup = 0;
            for (int64_t j = 0; j < count; j++) {
                if (strcmp(results[j], *kw) == 0) { dup = 1; break; }
            }
            if (!dup) results[count++] = *kw;
        }
    }

    /* Sort alphabetically */
    if (count > 1) {
        qsort((void*)results, (size_t)count, sizeof(const char*), cmp_str_ptr);
    }
    return count;
}

ray_err_t ray_env_set_local(int64_t sym_id, ray_t* val) {
    /* Reserved names (.sys.*, .os.*, .csv.*, .ipc.*) can only be
     * populated by builtin registration (ray_env_bind).  Refuse at
     * every user-reachable binding path so `(let .sys.gc 99)` or a
     * lambda parameter named `.sys.gc` cannot shadow the builtin.
     * Narrow carve-out: the five `.ipc.on.*` connection-hook syms are
     * user-settable here too, matching the global `ray_env_set` path. */
    if (ray_sym_is_reserved(sym_id) && !ray_sym_is_ipc_hook(sym_id))
        return RAY_ERR_RESERVED;
    if (__VM->scope_depth <= 0) return ray_env_set(sym_id, val);
    if (ray_sym_is_dotted(sym_id)) {
        return env_set_dotted(sym_id, val, lookup_top_frame, env_bind_local);
    }
    return env_bind_local(sym_id, val);
}

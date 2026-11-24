/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_OPS_WALKMEMO_H
#define RAY_OPS_WALKMEMO_H

/* Walks over a value that reach one container more than once (#1627).
 *
 * A value can hold one child at several positions on every level: each
 * level holds the previous one twice, and a value of n containers has 2^n
 * paths.  A walk that descends into every element then does work
 * exponential in the depth, though the value is small.  A walk whose
 * answer for a container depends only on that container (a hash, flags,
 * the result of an element-wise op, whether two values are equal) keeps
 * that answer in a memo the first time and reuses it after, so its work
 * is linear in the distinct containers.
 *
 * Only a container whose reference count is not 1 can be reached twice,
 * so only such a container is looked up or kept: a walk over unshared
 * values pays one compare per container and never touches the memo.
 *
 * Each walk kind has its own thread-local memo.  The first frame of a
 * walk that meets a shared container begins it (ray_wm_begin returns true
 * to that frame only), and that frame ends it on its way out, so a memo
 * lives for one walk.  Entries are keyed by one or two pointers and a
 * tag, and carry a 64-bit answer.  A memo that cannot grow (out of
 * memory) stops keeping new answers: the walk stays correct, only no
 * longer bounded. */

#include <rayforce.h>
#include "core/platform.h"   /* RAY_LIKELY */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uintptr_t a, b;        /* a == 0: an empty slot */
    uint64_t  tag;
    uint64_t  val;
} ray_wm_slot_t;

typedef struct {
    ray_wm_slot_t* slots;  /* allocated on the first entry, freed by ray_wm_end */
    uint64_t       cap;    /* a power of two, or 0 */
    uint64_t       n;
    bool           active;
    bool           full;   /* an entry could not be kept */
    const void*    skip;   /* the container the memo path is computing: the walk
                            * takes it into its own body, not back to the memo */
} ray_walk_memo_t;

/* May x be reached more than once in a walk: a container whose reference
 * count is not exactly one. */
static inline bool ray_wm_shared(const ray_t* x) {
    return x->rc != 1;
}

/* Does the list, dict or table x hold a list, dict or table: only such a
 * container can make a walk repeat work beyond its own size (one of
 * atoms and vectors costs no more to walk again than it holds, and its
 * parents are what the memo keeps). */
bool ray_wm_nested(const ray_t* x);

/* Is x worth keeping in a memo: shared, and holding containers.  A result
 * or a key set shares its cells with its source (each cell's count is 2),
 * so the count alone would keep every flat cell of a large value.  A list,
 * the usual such cell, is looked at here; a dict or table out of line. */
static inline bool ray_wm_container_type(int8_t t) {
    return t == RAY_LIST || t == RAY_DICT || t == RAY_TABLE;
}
static inline bool ray_wm_list_nested(const ray_t* l) {
    ray_t* const* cells = (ray_t* const*)ray_data((ray_t*)l);
    for (int64_t i = 0; i < l->len; i++) {
        const ray_t* c = cells[i];
        if (c && ray_wm_container_type(c->type)) return true;
    }
    return false;
}
static inline bool ray_wm_reused(const ray_t* x) {
    if (x->type < 0 || RAY_LIKELY(x->rc == 1)) return false;   /* an atom, or held once */
    if (x->type == RAY_LIST) return ray_wm_list_nested(x);
    if (x->type == RAY_DICT) {   /* its keys and values: slots 0 and 1 */
        ray_t* const* kv = (ray_t* const*)ray_data((ray_t*)x);
        for (int i = 0; i < 2; i++) {
            const ray_t* p = kv[i];
            if (!p || !ray_wm_container_type(p->type)) continue;
            if (p->type != RAY_LIST || ray_wm_list_nested(p)) return true;
        }
        return false;
    }
    return x->type == RAY_TABLE && ray_wm_nested(x);
}

/* Start a walk on m.  True for the frame that started it (which must call
 * ray_wm_end), false when a walk is already under way on this thread. */
bool ray_wm_begin(ray_walk_memo_t* m);
void ray_wm_end(ray_walk_memo_t* m);

bool ray_wm_get(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag, uint64_t* val);
/* False when the entry could not be kept (out of memory). */
bool ray_wm_put(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag, uint64_t val);

/* An operation that maps a value's parts through itself again (an
 * element-wise op, `in`, `not`): the result for the shared operand (a, b,
 * tag) is computed once by f(ctx) for the outermost call and reused, a
 * new reference each time.  a and b are values (b may be NULL): the memo
 * holds a reference to each kept result and to its operands, and drops
 * them all when the outermost call ends.  An error is not kept. */
ray_t* ray_wm_result(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag,
                     ray_t* (*f)(void* ctx), void* ctx);

/* Writers that write a value out in full (ser, the IPC reply and the
 * journal, .json.format, formatting and printing, a list column's save)
 * cannot share what the value shares: each time they meet a shared part
 * again they write it out again.  They keep a per-operation budget of
 * duplicated output: a value may repeat shared parts up to RAY_DUP_BUDGET
 * bytes of extra output (1 GiB).  What is charged is what the writer
 * really writes while it is inside a part it already wrote earlier in the
 * same walk: its text, its bytes, its separators, for every type.  Writing
 * a part the first time is never charged.
 *
 * ray_dup_t follows one writer's walk.  The writer counts the bytes it has
 * written so far (`out`), calls ray_dup_enter before writing each part and
 * ray_dup_leave after, and stops with `limit` once ray_dup_over says the
 * budget is passed.  Only a part that can be met twice is remembered: a
 * list, dict, table or vector, or a string atom, whose reference count is
 * not 1.  Inside a repeat nothing more is remembered or looked up: all of
 * it is being written again, and all of it is counted. */
#define RAY_DUP_BUDGET (ray_dup_budget)
extern uint64_t ray_dup_budget;   /* 1 GiB; tests lower it (ray_dup_budget_set) */
/* Set the budget, for tests that would otherwise need gigabytes; returns
 * the previous one. */
uint64_t ray_dup_budget_set(uint64_t bytes);

/* A memo is read and written only inside its walk: checked in debug
 * builds (release builds keep assert() live, and this is a hot path). */
#ifdef DEBUG
#define RAY_WM_ASSERT(c) assert(c)
#else
#define RAY_WM_ASSERT(c) ((void)0)
#endif

typedef struct {
    ray_walk_memo_t m;
    uint64_t        dup;     /* bytes written inside repeats that have closed */
    uint64_t        start;   /* `out` when the open repeat began */
    bool            in;      /* a repeat is open */
    bool            lb_done; /* the lower-bound walk has run */
    bool            tables_opaque;  /* the writer writes a table as one cell (print) */
    const ray_t*    root;    /* the value being written, for the lower bound */
} ray_dup_t;

void ray_dup_begin(ray_dup_t* t);
/* For a text writer that writes `root` in full: the first time a repeat
 * is met, a lower bound of the text written again for the whole value is
 * taken (ray_value_dup_cells_over); past the budget the writer is refused
 * before it writes the repeat.  A value without sharing never meets a
 * repeat and never pays for the walk. */
void ray_dup_root(ray_dup_t* t, const ray_t* root, bool tables_opaque);

/* A lower bound, in bytes, of what a text writer writes again for x's
 * repeated parts: each list, dict or table one and its parts, each vector
 * its cells (a symbol or string vector also its texts), a string atom its
 * length, any other atom one (every cell writes at least a byte, or a
 * separator).  With tables_opaque a table counts one.  True when it
 * passes RAY_DUP_BUDGET; linear in the distinct parts, and it stops there. */
bool ray_value_dup_cells_over(const ray_t* x, bool tables_opaque);
void ray_dup_end(ray_dup_t* t);
bool ray_dup_enter_slow(ray_dup_t* t, const ray_t* x, uint64_t out);

/* Before writing x: true when x opens a repeat (pass it to ray_dup_leave). */
static inline bool ray_dup_enter(ray_dup_t* t, const ray_t* x, uint64_t out) {
    if (t->in || !x || x->rc == 1) return false;
    if (x->type >= 0 ? x->type >= RAY_LAMBDA : x->type != -RAY_STR) return false;
    return ray_dup_enter_slow(t, x, out);
}
/* Must the writer track x: a repeat is open (everything is counted, and the
 * budget checked), or x is a part that may be met again.  When false the
 * writer writes x with no bookkeeping. */
static inline bool ray_dup_watch(const ray_dup_t* t, const ray_t* x) {
    return t->in || (x && x->rc != 1 &&
                     (x->type >= 0 ? x->type < RAY_LAMBDA : x->type == -RAY_STR));
}
static inline void ray_dup_leave(ray_dup_t* t, bool opened, uint64_t out) {
    if (!opened) return;
    uint64_t n = out - t->start;
    t->dup = t->dup > UINT64_MAX - n ? UINT64_MAX : t->dup + n;
    t->in = false;
}
/* Has the walk written more than the budget out again (`out` as above)? */
static inline bool ray_dup_over(const ray_dup_t* t, uint64_t out) {
    uint64_t open = t->in ? out - t->start : 0;
    return t->dup > RAY_DUP_BUDGET || open > RAY_DUP_BUDGET - t->dup;
}

#endif /* RAY_OPS_WALKMEMO_H */

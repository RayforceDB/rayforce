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

#include "ops/walkmemo.h"
#include "ops/hash.h"
#include "table/table.h"   /* ray_table_slot_idx */
#include "core/types.h"    /* ray_elem_size */
#include "table/sym.h"     /* ray_sym_vec_cell */
#include <string.h>

static inline bool wm_container(const ray_t* c) {
    return c && !RAY_IS_ERR(c) && (c->type == RAY_LIST || c->type == RAY_DICT || c->type == RAY_TABLE);
}
static bool wm_list_nested(const ray_t* l) {
    ray_t* const* cells = (ray_t* const*)ray_data((ray_t*)l);
    for (int64_t i = 0; i < l->len; i++)
        if (wm_container(cells[i])) return true;
    return false;
}
static bool wm_part_nested(const ray_t* p) {
    if (!wm_container(p)) return false;
    return p->type == RAY_LIST ? wm_list_nested(p) : true;
}
bool ray_wm_nested(const ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return false;
    if (x->type == RAY_LIST) return wm_list_nested(x);
    if (x->type == RAY_DICT)
        return wm_part_nested(ray_dict_keys((ray_t*)x)) || wm_part_nested(ray_dict_vals((ray_t*)x));
    if (x->type == RAY_TABLE) {
        for (int64_t c = 0; c < ray_table_ncols((ray_t*)x); c++)
            if (wm_part_nested(ray_table_slot_idx((ray_t*)x, c))) return true;
    }
    return false;
}

bool ray_wm_begin(ray_walk_memo_t* m) {
    if (m->active) return false;
    /* A memo lives for one walk: one left with entries was used outside a
     * walk (get/put on a closed memo).  Never reuse what it holds. */
    RAY_WM_ASSERT(!m->slots && "walk memo used outside ray_wm_begin/ray_wm_end");
    if (RAY_UNLIKELY(m->slots != NULL)) ray_wm_end(m);
    m->active = true;
    m->full = false;
    m->n = 0;
    return true;
}

void ray_wm_end(ray_walk_memo_t* m) {
    if (m->slots) ray_free_raw(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->n = 0;
    m->active = false;
}

static inline uint64_t wm_hash(uintptr_t a, uintptr_t b, uint64_t tag) {
    return ray_hash_combine(ray_hash_combine(ray_hash_i64((int64_t)a), ray_hash_i64((int64_t)b)),
                            ray_hash_i64((int64_t)tag));
}

static ray_wm_slot_t* wm_find(ray_wm_slot_t* slots, uint64_t cap, uintptr_t a, uintptr_t b,
                              uint64_t tag) {
    uint64_t i = wm_hash(a, b, tag) & (cap - 1);
    for (;;) {
        ray_wm_slot_t* s = &slots[i];
        if (!s->a || (s->a == a && s->b == b && s->tag == tag)) return s;
        i = (i + 1) & (cap - 1);
    }
}

bool ray_wm_get(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag, uint64_t* val) {
    RAY_WM_ASSERT(m->active && "walk memo read outside its walk");
    if (!m->n) return false;
    ray_wm_slot_t* s = wm_find(m->slots, m->cap, (uintptr_t)a, (uintptr_t)b, tag);
    if (!s->a) return false;
    *val = s->val;
    return true;
}

static bool wm_grow(ray_walk_memo_t* m) {
    uint64_t cap = m->cap ? m->cap * 2 : 64;
    ray_wm_slot_t* slots = ray_calloc_raw((size_t)cap * sizeof *slots);
    if (!slots) return false;
    for (uint64_t i = 0; i < m->cap; i++) {
        ray_wm_slot_t* o = &m->slots[i];
        if (o->a) *wm_find(slots, cap, o->a, o->b, o->tag) = *o;
    }
    if (m->slots) ray_free_raw(m->slots);
    m->slots = slots;
    m->cap = cap;
    return true;
}

bool ray_wm_put(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag, uint64_t val) {
    RAY_WM_ASSERT(m->active && "walk memo written outside its walk");
    if (RAY_UNLIKELY(!m->active)) return false;   /* keep nothing a walk will not end */
    if (!a) return false;
    if ((m->n + 1) * 2 > m->cap && !wm_grow(m)) { m->full = true; return false; }
    ray_wm_slot_t* s = wm_find(m->slots, m->cap, (uintptr_t)a, (uintptr_t)b, tag);
    if (!s->a) m->n++;
    *s = (ray_wm_slot_t){ (uintptr_t)a, (uintptr_t)b, tag, val };
    return true;
}


/* ===== ray_dup_t ===== */

uint64_t ray_dup_budget = (uint64_t)1 << 30;
uint64_t ray_dup_budget_set(uint64_t bytes) {
    uint64_t was = ray_dup_budget;
    ray_dup_budget = bytes;
    return was;
}

void ray_dup_begin(ray_dup_t* t) {
    memset(t, 0, sizeof *t);
    t->m.active = true;
}

void ray_dup_end(ray_dup_t* t) {
    ray_wm_end(&t->m);
}

void ray_dup_root(ray_dup_t* t, const ray_t* root, bool tables_opaque) {
    t->root = root;
    t->tables_opaque = tables_opaque;
}

bool ray_dup_enter_slow(ray_dup_t* t, const ray_t* x, uint64_t out) {
    if (!t->m.active) return false;   /* a writer that does not track (a zeroed one) */
    uint64_t v;
    if (ray_wm_get(&t->m, x, NULL, 0, &v)) {   /* written before: a repeat */
        t->in = true;
        t->start = out;
        if (!t->lb_done && t->root) {   /* the first repeat: bound the whole value once */
            t->lb_done = true;
            if (ray_value_dup_cells_over(t->root, t->tables_opaque)) t->dup = UINT64_MAX;
        }
        return true;
    }
    ray_wm_put(&t->m, x, NULL, 0, 1);
    return false;
}

/* ===== ray_value_dup_cells_over ===== */

static inline bool lb_container(const ray_t* x, bool tables_opaque) {
    return x && !RAY_IS_ERR(x) &&
           (x->type == RAY_LIST || x->type == RAY_DICT || (x->type == RAY_TABLE && !tables_opaque));
}

static ray_t* lb_part(const ray_t* x, int64_t i, bool* more) {
    *more = true;
    if (x->type == RAY_LIST) {
        if (i >= x->len) { *more = false; return NULL; }
        return ((ray_t**)ray_data((ray_t*)x))[i];
    }
    if (x->type == RAY_DICT) {
        if (i >= 2) { *more = false; return NULL; }
        return i == 0 ? ray_dict_keys((ray_t*)x) : ray_dict_vals((ray_t*)x);
    }
    if (i >= ray_table_ncols((ray_t*)x)) { *more = false; return NULL; }
    return ray_table_slot_idx((ray_t*)x, i);
}

static inline uint64_t lb_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/* A part that is not walked: a vector its cells (a symbol or string vector
 * also its texts), a string atom its length, any other atom or a table
 * written as one cell one. */
static uint64_t lb_leaf(const ray_t* p) {
    if (!p || RAY_IS_ERR(p)) return 1;
    if (p->type == -RAY_STR) { size_t n = ray_str_len((ray_t*)p); return n ? n : 1; }
    if (p->type < 0 || p->type >= RAY_LAMBDA || p->type == RAY_TABLE) return 1;
    uint64_t n = p->len > 0 ? (uint64_t)p->len : 1;
    if (p->type == RAY_SYM) {
        for (int64_t i = 0; i < p->len; i++) {
            ray_t* s = ray_sym_vec_cell((ray_t*)p, i);
            if (s) n = lb_add(n, ray_str_len(s));
        }
    } else if (p->type == RAY_STR) {
        for (int64_t i = 0; i < p->len; i++) {
            size_t l = 0;
            if (ray_str_vec_get((ray_t*)p, i, &l)) n = lb_add(n, l);
        }
    }
    return n;
}

typedef struct { const ray_t* x; int64_t i; uint64_t acc; } lb_frame_t;

static inline bool lb_tracked(const ray_t* c) {
    return c && !RAY_IS_ERR(c) && c->rc != 1 &&
           (c->type >= 0 ? c->type < RAY_LAMBDA : c->type == -RAY_STR);
}

bool ray_value_dup_cells_over(const ray_t* x, bool tables_opaque) {
    if (!lb_container(x, tables_opaque)) return false;
    ray_walk_memo_t m;
    memset(&m, 0, sizeof m);
    m.active = true;
    lb_frame_t local[32];
    lb_frame_t* st = local;
    int64_t cap = 32, top = 0;
    uint64_t lb = 0;
    bool over = false;
    st[top++] = (lb_frame_t){ x, 0, 1 };
    while (top > 0 && !over) {
        lb_frame_t* f = &st[top - 1];
        bool more;
        ray_t* c = lb_part(f->x, f->i++, &more);
        if (!more) {
            uint64_t v = f->acc;
            if (lb_tracked(f->x)) ray_wm_put(&m, f->x, NULL, 0, v);
            top--;
            if (top > 0) st[top - 1].acc = lb_add(st[top - 1].acc, v);
            continue;
        }
        uint64_t v;
        if (lb_tracked(c) && ray_wm_get(&m, c, NULL, 0, &v)) {   /* written again */
            lb = lb_add(lb, v);
            f->acc = lb_add(f->acc, v);
            over = lb > RAY_DUP_BUDGET;
            continue;
        }
        if (!lb_container(c, tables_opaque)) {
            uint64_t w = lb_leaf(c);
            if (lb_tracked(c)) ray_wm_put(&m, c, NULL, 0, w);
            f->acc = lb_add(f->acc, w);
            continue;
        }
        if (top == cap) {
            lb_frame_t* grown = ray_alloc_raw((size_t)cap * 2 * sizeof *grown);
            if (!grown) break;   /* cannot bound it here: the exact count still applies */
            memcpy(grown, st, (size_t)top * sizeof *grown);
            if (st != local) ray_free_raw(st);
            st = grown;
            cap *= 2;
        }
        st[top++] = (lb_frame_t){ c, 0, 1 };
    }
    if (st != local) ray_free_raw(st);
    ray_wm_end(&m);
    return over;
}

/* ===== ray_wm_result ===== */

/* A kept result's entry holds a reference to its result and to its
 * operands, so no operand's address is reused by another value while the
 * memo lives. */
static void wm_release_all(ray_walk_memo_t* m) {
    for (uint64_t i = 0; i < m->cap; i++) {
        ray_wm_slot_t* s = &m->slots[i];
        if (!s->a) continue;
        ray_release((ray_t*)(uintptr_t)s->val);
        ray_release((ray_t*)s->a);
        if (s->b) ray_release((ray_t*)s->b);
    }
}

ray_t* ray_wm_result(ray_walk_memo_t* m, const void* a, const void* b, uint64_t tag,
                     ray_t* (*f)(void* ctx), void* ctx) {
    if (!m->active) {   /* the outermost call: nothing to reuse yet, and it does not recur */
        ray_wm_begin(m);
        ray_t* r = f(ctx);
        wm_release_all(m);
        ray_wm_end(m);
        return r;
    }
    uint64_t v;
    ray_t* r;
    if (ray_wm_get(m, a, b, tag, &v)) {
        r = (ray_t*)(uintptr_t)v;
        ray_retain(r);
    } else {
        r = f(ctx);
        if (r && !RAY_IS_ERR(r) && ray_wm_put(m, a, b, tag, (uint64_t)(uintptr_t)r)) {
            ray_retain(r);
            ray_retain((ray_t*)a);
            if (b) ray_retain((ray_t*)b);
        }
    }
    return r;
}

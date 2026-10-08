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

/* Ordered symbol predicates. Scratch follows the ids used by this scan,
 * never the process-wide vocabulary. Each worker owns its maps and ranks. */
#include "ops/internal.h"
#include "ops/hash.h"
#include "table/domain.h"
#include "table/sym.h"
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t id, value; } sym_order_slot_t;
typedef struct {
    int64_t base;
    int64_t* dense;
    sym_order_slot_t* slots;
    size_t cap, used;
} sym_order_map_t;

static void sym_order_map_free(sym_order_map_t* m) {
    ray_free_raw(m->dense);
    ray_free_raw(m->slots);
}

static bool sym_order_map_grow(sym_order_map_t* m) {
    size_t cap = m->cap ? m->cap * 2 : 16;
    if (cap < m->cap || cap > SIZE_MAX / sizeof(sym_order_slot_t)) return false;
    sym_order_slot_t* slots = ray_calloc_raw(cap * sizeof(*slots));
    if (!slots) return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->slots[i].value) continue;
        size_t h = ray_hash_i64(m->slots[i].id) & (cap - 1);
        while (slots[h].value) h = (h + 1) & (cap - 1);
        slots[h] = m->slots[i];
    }
    ray_free_raw(m->slots);
    m->slots = slots; m->cap = cap;
    return true;
}

/* A zero value denotes an unseen id. Callers immediately fill new slots.
 * Dense storage is bounded by both the rows and a small absolute budget;
 * widely separated ids use a growing hash map, even in a huge domain. */
static int64_t* sym_order_slot(sym_order_map_t* m, int64_t id) {
    if (m->dense) return &m->dense[id - m->base];
    size_t h = ray_hash_i64(id) & (m->cap - 1);
    while (m->slots[h].value) {
        if (m->slots[h].id == id) return &m->slots[h].value;
        h = (h + 1) & (m->cap - 1);
    }
    if (m->used >= m->cap / 2) {
        if (!sym_order_map_grow(m)) return NULL;
        h = ray_hash_i64(id) & (m->cap - 1);
        while (m->slots[h].value) h = (h + 1) & (m->cap - 1);
    }
    m->slots[h].id = id;
    m->used++;
    return &m->slots[h].value;
}

static bool sym_order_map_init(sym_order_map_t* m, ray_t* v,
                                int64_t start, int64_t end) {
    const void* data = ray_data(v);
    int64_t lo = INT64_MAX, hi = 0;
#define SYM_BOUNDS(T) do { \
    const T* p = data; \
    for (int64_t i = start; i < end; i++) { \
        int64_t id = p[i]; \
        if (id < lo) lo = id; \
        if (id > hi) hi = id; \
    } \
} while (0)
    switch (v->attrs & RAY_SYM_W_MASK) {
    case RAY_SYM_W8:  SYM_BOUNDS(uint8_t); break;
    case RAY_SYM_W16: SYM_BOUNDS(uint16_t); break;
    case RAY_SYM_W32: SYM_BOUNDS(uint32_t); break;
    default:         SYM_BOUNDS(int64_t); break;
    }
#undef SYM_BOUNDS
    uint64_t span = (uint64_t)hi - (uint64_t)lo + 1;
    if (span <= 65536 && span <= (uint64_t)(end - start) * 4 + 32) {
        m->dense = ray_calloc_raw((size_t)span * sizeof(int64_t));
        m->base = lo;
        return m->dense != NULL;
    }
    return sym_order_map_grow(m);
}

static int sym_order_bytes(const char* a, size_t al, const char* b, size_t bl) {
    size_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;
    return c ? c : (al > bl) - (al < bl);
}

static bool sym_order_apply(uint16_t op, int c) {
    switch (op) {
    case OP_LT: return c < 0;
    case OP_LE: return c <= 0;
    case OP_GT: return c > 0;
    default:    return c >= 0;
    }
}

typedef struct {
    const char* bytes;
    size_t len;
    int64_t id;
    int side;
} sym_order_entry_t;

static int sym_order_entry_cmp(const void* a, const void* b) {
    const sym_order_entry_t* l = a;
    const sym_order_entry_t* r = b;
    return sym_order_bytes(l->bytes, l->len, r->bytes, r->len);
}

typedef struct {
    ray_t* left;
    ray_t* right;                 /* NULL for column/literal */
    const char* literal;
    size_t literal_len;
    bool reverse;
    uint16_t opcode;
    int64_t nrows;
    uint32_t ntasks;
    bool* out;
    int failed;
} sym_order_ctx_t;

static void sym_order_task(void* raw, uint32_t wid, int64_t task, int64_t task_end) {
    (void)wid; (void)task_end;
    sym_order_ctx_t* c = raw;
    /* dispatch_n gives one contiguous range to each task. Rank setup is
     * once per task, not once per morsel, and needs no shared mutable cache. */
    int64_t chunk = c->nrows / c->ntasks;
    int64_t start = task * chunk;
    int64_t end = task + 1 == c->ntasks ? c->nrows : start + chunk;
    sym_order_map_t maps[2] = {{0}, {0}};
    sym_order_entry_t* entries = NULL;
    size_t used = 0, cap = 0;
    ray_t* cols[] = {c->left, c->right};
    if (!sym_order_map_init(&maps[0], cols[0], start, end)) goto oom;
    if (!c->right) {
        ray_sym_domain_t* dom = ray_sym_vec_domain(c->left);
        const void* data = ray_data(c->left);
        for (int64_t i = start; i < end; i++) {
            int64_t id = ray_read_sym(data, i, RAY_SYM, c->left->attrs);
            int64_t* verdict = sym_order_slot(&maps[0], id);
            if (!verdict) goto oom;
            if (!*verdict) {
                ray_t* s = ray_sym_domain_str(dom, id);
                if (!s) goto oom;
                int cmp = sym_order_bytes(ray_str_ptr(s), ray_str_len(s),
                                           c->literal, c->literal_len);
                if (c->reverse) cmp = -((cmp > 0) - (cmp < 0));
                *verdict = 1 + sym_order_apply(c->opcode, cmp);
            }
            c->out[i] = *verdict == 2;
        }
    } else {
        if (!sym_order_map_init(&maps[1], cols[1], start, end)) goto oom;
        /* Collect distinct ids present in these rows. An unrelated symbol
         * is never resolved or sorted. Cross-domain equal strings receive
         * the same rank when the two used vocabularies are sorted together. */
        for (int side = 0; side < 2; side++) {
            ray_t* v = cols[side];
            const void* data = ray_data(v);
            ray_sym_domain_t* dom = ray_sym_vec_domain(v);
            for (int64_t i = start; i < end; i++) {
                int64_t id = ray_read_sym(data, i, RAY_SYM, v->attrs);
                int64_t* seen = sym_order_slot(&maps[side], id);
                if (!seen) goto oom;
                if (*seen) continue;
                if (used == cap) {
                    size_t next = cap ? cap * 2 : 16;
                    if (next < cap || next > SIZE_MAX / sizeof(*entries)) goto oom;
                    void* p = ray_realloc_raw(entries, next * sizeof(*entries));
                    if (!p) goto oom;
                    entries = p; cap = next;
                }
                ray_t* s = ray_sym_domain_str(dom, id);
                if (!s) goto oom;
                entries[used++] = (sym_order_entry_t){ray_str_ptr(s), ray_str_len(s), id, side};
                *seen = 1;
            }
        }
        qsort(entries, used, sizeof(*entries), sym_order_entry_cmp);
        int64_t rank = 1;
        for (size_t j = 0; j < used; j++) {
            if (j && sym_order_entry_cmp(&entries[j - 1], &entries[j])) rank++;
            *sym_order_slot(&maps[entries[j].side], entries[j].id) = rank;
        }
        const void* ld = ray_data(c->left);
        const void* rd = ray_data(c->right);
        for (int64_t i = start; i < end; i++) {
            int64_t l = *sym_order_slot(&maps[0], ray_read_sym(ld, i, RAY_SYM, c->left->attrs));
            int64_t r = *sym_order_slot(&maps[1], ray_read_sym(rd, i, RAY_SYM, c->right->attrs));
            c->out[i] = sym_order_apply(c->opcode, (l > r) - (l < r));
        }
    }
    goto done;
oom:
    __atomic_store_n(&c->failed, 1, __ATOMIC_RELAXED);
done:
    ray_free_raw(entries);
    sym_order_map_free(&maps[0]); sym_order_map_free(&maps[1]);
}

/* Borrow scalar bytes in their original domain. No interning, including
 * length-one FILE-domain vectors used as query scalar broadcasts. */
static bool sym_order_scalar(ray_t* v, const char** bytes, size_t* len) {
    ray_t* s = v;
    if (v->type == -RAY_SYM) s = ray_sym_str(v->i64);
    else if (v->type == RAY_SYM) s = ray_sym_vec_cell(v, 0);
    else if (v->type == RAY_STR) {
        *bytes = ray_str_vec_get(v, 0, len);
        return true;
    }
    if (!s) return false;
    *bytes = ray_str_ptr(s); *len = ray_str_len(s);
    return true;
}

ray_t* exec_sym_order(uint16_t opcode, ray_t* lhs, ray_t* rhs,
                      bool l_scalar, bool r_scalar) {
    if ((!l_scalar && lhs->type != RAY_SYM) ||
        (!r_scalar && rhs->type != RAY_SYM))
        return ray_error("type", "symbol ordering requires symbol columns or string/symbol scalars");
    int64_t n = !l_scalar ? lhs->len : !r_scalar ? rhs->len : 1;
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = n;
    if (!n) return out;
    sym_order_ctx_t c = {.opcode = opcode, .nrows = n, .ntasks = 1, .out = ray_data(out)};
    if (l_scalar && r_scalar) {
        const char *l, *r; size_t ll, rl;
        if (!sym_order_scalar(lhs, &l, &ll) || !sym_order_scalar(rhs, &r, &rl)) {
            ray_release(out);
            return ray_error("oom", "symbol ordering scalar");
        }
        c.out[0] = sym_order_apply(opcode, sym_order_bytes(l, ll, r, rl));
        return out;
    }
    c.left = l_scalar ? rhs : lhs;
    if (l_scalar || r_scalar) {
        c.reverse = l_scalar;
        if (!sym_order_scalar(l_scalar ? lhs : rhs, &c.literal, &c.literal_len)) {
            ray_release(out);
            return ray_error("oom", "symbol ordering scalar");
        }
    } else c.right = rhs;
    ray_pool_t* pool = ray_pool_get();
    if (pool && n >= RAY_PARALLEL_THRESHOLD) {
        c.ntasks = ray_pool_total_workers(pool);
        if (c.ntasks > (uint64_t)n) c.ntasks = (uint32_t)n;
        ray_pool_dispatch_n(pool, sym_order_task, &c, c.ntasks);
    } else sym_order_task(&c, 0, 0, 1);
    if (c.failed) { ray_release(out); return ray_error("oom", "symbol ordering scratch"); }
    return out;
}

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

#include "ops/colbind.h"
#include "table/table.h"
#include <stdatomic.h>
#include <string.h>

static _Atomic int64_t g_bind_fail_at;      /* test hook: fail the nth allocation */
void ray_graph_bind_test_fail_alloc(int64_t nth) { atomic_store(&g_bind_fail_at, nth); }
static bool bind_alloc_fails(void) {
    int64_t v = atomic_load(&g_bind_fail_at);
    while (v > 0)
        if (atomic_compare_exchange_weak(&g_bind_fail_at, &v, v - 1)) return v == 1;
    return false;
}

typedef struct { ray_t* tbl; int64_t sym; ray_col_src_t src; } colbind_ent_t;

typedef struct ray_col_binds_s {
    colbind_ent_t** e;     /* one allocation per entry: stable addresses */
    uint32_t n, cap;
} ray_col_binds_t;

ray_col_src_t* ray_graph_bind_col(ray_graph_t* g, ray_t* tbl, int64_t sym, ray_t** err) {
    *err = NULL;
    if (!g || !tbl) return NULL;
    ray_col_binds_t* b = g->binds;
    /* A hit must still describe the column the table holds now: a table
     * released and reallocated at the same address (a temporary sub-table)
     * or a column replaced in place must not return the old binding. */
    ray_t* cur = NULL;
    bool   cur_set = false;
    for (uint32_t i = 0; b && i < b->n; i++) {
        if (b->e[i]->tbl != tbl || b->e[i]->sym != sym) continue;
        if (!cur_set) {
            cur = ray_table_get_col_enc(tbl, sym);
            if (!cur) cur = ray_table_col(tbl, sym);          /* plain slot: no decode */
            cur_set = true;
        }
        if (b->e[i]->src.obj == cur) return &b->e[i]->src;
    }
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, sym, &m)) return NULL;          /* absent */
    if (bind_alloc_fails()) { *err = ray_error("oom", "column bind"); return NULL; }
    if (!b) {
        b = ray_calloc_raw(sizeof *b);
        if (!b) { *err = ray_error("oom", "column bind"); return NULL; }
        g->binds = b;
    }
    if (b->n == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 8;
        colbind_ent_t** ne = ray_realloc_raw(b->e, nc * sizeof *ne);
        if (!ne) { *err = ray_error("oom", "column bind"); return NULL; }
        b->e = ne; b->cap = nc;
    }
    colbind_ent_t* x = ray_calloc_raw(sizeof *x);
    if (!x) { *err = ray_error("oom", "column bind"); return NULL; }
    ray_t* e = ray_table_col_src(tbl, sym, &x->src);              /* no decode */
    if (e || !x->src.obj) { ray_free_raw(x); *err = e; return NULL; }
    x->tbl = tbl; x->sym = sym;
    b->e[b->n++] = x;
    return &x->src;
}

bool ray_col_bind_declines(ray_t* tbl, int64_t sym) {
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, sym, &m)) return false;
    if (m.type == RAY_MAPCOMMON) return true;
    if (m.enc) return (m.enc->attrs & RAY_ATTR_SLICE) != 0;
    return RAY_IS_PARTED(m.type) && (m.attrs & RAY_ATTR_SLICE) != 0;
}

void ray_graph_binds_free(ray_graph_t* g) {
    ray_col_binds_t* b = g ? g->binds : NULL;
    if (!b) return;
    for (uint32_t i = 0; i < b->n; i++) { ray_col_src_release(&b->e[i]->src); ray_free_raw(b->e[i]); }
    ray_free_raw(b->e);
    ray_free_raw(b);
    g->binds = NULL;
}

uint32_t ray_graph_binds_count(const ray_graph_t* g) {
    return (g && g->binds) ? g->binds->n : 0;
}

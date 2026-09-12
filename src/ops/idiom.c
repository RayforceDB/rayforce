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

#include "vec/vec.h"
#include "idiom.h"
#include "opt.h"
#include "mem/sys.h"
#include "mem/heap.h"
#include <string.h>

/* Local accessors mirroring ops/internal.h.  idiom.c includes "opt.h" (which
 * provides a non-static find_ext); including internal.h here would redefine
 * find_ext as static inline.  So we copy the id->node accessors locally, the
 * same way opt.c does. */
static inline ray_op_t* op_node(ray_graph_t* g, uint32_t id) {
    return id == RAY_OP_NONE ? NULL : &g->nodes[id];
}
static inline ray_op_t* op_child(ray_graph_t* g, const ray_op_t* op, int i) {
    return op_node(g, op->in_id[i]);
}

#define RAY_IDIOM_OPCODE_CAP 128
#define RAY_IDIOM_MAX_ROWS    64

/* ---------------------------------------------------------------------------
 * Rewrite functions — one per idiom row.
 * ---------------------------------------------------------------------------
 */

/* (count (distinct v)) → OP_COUNT_DISTINCT(v)
 *
 * node     = OP_COUNT
 * node's input[0] = OP_DISTINCT
 * Returns  OP_COUNT_DISTINCT(distinct's input[0])
 */
static ray_op_t* rw_count_distinct(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* distinct = op_child(g, node, 0);
    if (!distinct || !op_child(g, distinct, 0)) return NULL;
    ray_op_t* src = op_child(g, distinct, 0);

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_COUNT_DISTINCT;
    repl->arity     = 1;
    repl->in_id[0]  = src ? src->id : RAY_OP_NONE;
    repl->out_type  = RAY_I64;
    repl->est_rows  = 1;
    return repl;
}

/* (count (X v)) → (count v)  for any X that preserves cardinality
 * (asc, desc, reverse).  We wire OP_COUNT directly over X's input,
 * letting the orphaned sort/reverse node be swept by pass_dce.
 */
static ray_op_t* rw_count_passthrough(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* inner = op_child(g, node, 0);
    if (!inner || !op_child(g, inner, 0)) return NULL;
    ray_op_t* src = op_child(g, inner, 0);

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_COUNT;
    repl->arity     = 1;
    repl->in_id[0]  = src ? src->id : RAY_OP_NONE;
    repl->out_type  = RAY_I64;
    repl->est_rows  = 1;
    return repl;
}

static ray_t* scan_source_col(ray_graph_t* g, ray_op_t* src) {
    if (!g || !src || src->opcode != OP_SCAN) return NULL;

    ray_op_ext_t* ext = find_ext(g, src->id);
    if (!ext) return NULL;

    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, src->pad, sizeof(uint16_t));

    ray_t* tbl = NULL;
    if (stored_table_id > 0) {
        uint16_t table_id = (uint16_t)(stored_table_id - 1);
        if (g->tables && table_id < g->n_tables)
            tbl = g->tables[table_id];
    } else {
        tbl = g->table;
    }

    if (!tbl) return NULL;
    return ray_table_get_col(tbl, ext->sym);
}

/* True only when the input vector to (asc …) is statically known to
   have no nulls. Walks one node — the input to the asc — and reads
   its out_attrs (if tracked) or out_type+constness. Returns false on
   uncertainty (safe default — slow path runs). */
static bool pre_no_nulls_on_asc_input(ray_graph_t* g, ray_op_t* node) {
    /* node = OP_FIRST (or OP_LAST); node's input[0] = OP_ASC;
       inspect the OP_ASC's input[0] (the source vector). */
    ray_op_t* asc = op_child(g, node, 0);
    if (!asc || !op_child(g, asc, 0)) return false;
    ray_op_t* src = op_child(g, asc, 0);

    /* OP_CONST: read the literal from the ext data and check its attrs.
       For other opcodes (computed inputs), bail — false negative is fine. */
    if (src->opcode == OP_CONST) {
        ray_op_ext_t* ext = find_ext(g, src->id);
        if (!ext || !ext->literal) return false;
        ray_t* lit = ext->literal;
        /* Only safe to rewrite if the literal is a vector with no nulls. */
        if (!ray_is_vec(lit)) return false;
        return !ray_vec_has_nulls(lit);
    }

    if (src->opcode == OP_SCAN) {
        ray_t* col = scan_source_col(g, src);
        if (!col) return false;
        if (!ray_is_vec(col) && !RAY_IS_PARTED(col->type)) return false;
        return !ray_vec_has_nulls(col);
    }

    return false;
}

/* (first (asc v)) → OP_MIN(v) — safe only when v is null-free.
 * xasc puts nulls first, so first(asc(null-bearing)) = null;
 * OP_MIN skips nulls and returns smallest non-null.  The precondition
 * pre_no_nulls_on_asc_input ensures we only rewrite when null-free. */
static ray_op_t* rw_first_asc_to_min(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* asc = op_child(g, node, 0);
    if (!asc || !op_child(g, asc, 0)) return NULL;
    ray_op_t* src = op_child(g, asc, 0);

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_MIN;
    repl->arity     = 1;
    repl->in_id[0]  = src ? src->id : RAY_OP_NONE;
    repl->out_type  = src->out_type;
    repl->est_rows  = 1;
    return repl;
}

/* (last (asc v)) → OP_MAX(v) — same null-free precondition.
 * xasc puts nulls first, so last(asc(null-free)) = largest element;
 * OP_MAX also returns the largest non-null element — semantics match. */
static ray_op_t* rw_last_asc_to_max(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* asc = op_child(g, node, 0);
    if (!asc || !op_child(g, asc, 0)) return NULL;
    ray_op_t* src = op_child(g, asc, 0);

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_MAX;
    repl->arity     = 1;
    repl->in_id[0]  = src ? src->id : RAY_OP_NONE;
    repl->out_type  = src->out_type;
    repl->est_rows  = 1;
    return repl;
}

/* ---------------------------------------------------------------------------
 * Idiom table — one row per pattern.
 * ---------------------------------------------------------------------------
 */
const ray_idiom_t ray_idioms[] = {
    { OP_COUNT, OP_DISTINCT, NULL,                      rw_count_distinct,    "count(distinct) -> count_distinct" },
    { OP_COUNT, OP_ASC,      NULL,                      rw_count_passthrough, "count(asc) -> count"               },
    { OP_COUNT, OP_DESC,     NULL,                      rw_count_passthrough, "count(desc) -> count"              },
    { OP_COUNT, OP_REVERSE,  NULL,                      rw_count_passthrough, "count(reverse) -> count"           },
    { OP_FIRST, OP_ASC,      pre_no_nulls_on_asc_input, rw_first_asc_to_min,  "first(asc) -> min  [no-nulls]"     },
    { OP_LAST,  OP_ASC,      pre_no_nulls_on_asc_input, rw_last_asc_to_max,   "last(asc) -> max   [no-nulls]"     },
};
const int ray_idioms_count = (int)(sizeof(ray_idioms) / sizeof(ray_idioms[0]));

_Static_assert(sizeof(ray_idioms) / sizeof(ray_idioms[0]) <= RAY_IDIOM_MAX_ROWS,
               "idiom row count exceeds dispatch index capacity");

static int8_t first_idiom[RAY_IDIOM_OPCODE_CAP];
static int8_t next_idiom [RAY_IDIOM_MAX_ROWS];
static bool   index_built;

static void build_index(void) {
    if (index_built) return;
    memset(first_idiom, -1, sizeof(first_idiom));
    memset(next_idiom,  -1, sizeof(next_idiom));
    for (int i = 0; i < ray_idioms_count; i++) {
        uint16_t op = ray_idioms[i].root_op;
        if (op >= RAY_IDIOM_OPCODE_CAP) continue;
        next_idiom[i]   = first_idiom[op];
        first_idiom[op] = (int8_t)i;
    }
    index_built = true;
}

static bool is_ext_root(uint16_t opcode) {
    return opcode == OP_GROUP || opcode == OP_SORT || opcode == OP_JOIN ||
           opcode == OP_WINDOW || opcode == OP_WINDOW_JOIN || opcode == OP_SELECT;
}

/* Try one rewrite at `node`.  Returns the replacement when the rewrite
 * fires, else NULL.  Caller redirects consumers and marks the old node
 * dead — having the helper return the replacement also lets the pass
 * track when the *root* was rewritten so the caller's root pointer can
 * be bumped to the replacement. */
static ray_op_t* try_rewrite(ray_graph_t* g, ray_op_t* node) {
    if (!node || (node->flags & OP_FLAG_DEAD)) return NULL;
    if (is_ext_root(node->opcode)) return NULL;
    if (node->opcode >= RAY_IDIOM_OPCODE_CAP) return NULL;

    int idx = first_idiom[node->opcode];
    while (idx >= 0) {
        const ray_idiom_t* row = &ray_idioms[idx];
        ray_op_t* c0 = op_child(g, node, 0);
        if (c0 && c0->opcode == row->child0_op) {
            if (!row->pre || row->pre(g, node)) {
                ray_op_t* repl = row->rewrite(g, node);
                if (repl) {
                    /* UINT32_MAX sentinels: no nodes to skip during redirect */
                    redirect_consumers(g, node->id, repl, UINT32_MAX, UINT32_MAX);
                    node->flags |= OP_FLAG_DEAD;
                    return repl;  /* first-match-wins */
                }
            }
        }
        idx = next_idiom[idx];
    }
    return NULL;
}

ray_op_t* ray_idiom_pass(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root || g->node_count == 0) return root;
    build_index();

    /* Iterative post-order walk: children rewritten before parents so
       a parent match sees the latest shape of its children. Two-stack
       pattern — push roots onto stack1, drain into stack2 (reverse),
       pop stack2 to get post-order. */
    uint32_t nc = g->node_count;
    if (nc > UINT32_MAX / 4) return root;  /* overflow guard, mirrors expr.c */

    uint32_t cap = nc * 2;
    uint32_t stk1_local[256], stk2_local[256];
    uint32_t* stk1 = cap <= 256 ? stk1_local : (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
    uint32_t* stk2 = cap <= 256 ? stk2_local : (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
    if (!stk1 || !stk2) {
        if (stk1 && stk1 != stk1_local) ray_sys_free(stk1);
        if (stk2 && stk2 != stk2_local) ray_sys_free(stk2);
        return root;
    }

    /* Visited-bit guard against re-entry on shared subgraphs. */
    uint8_t visited_local[256];
    uint8_t* visited = nc <= 256 ? visited_local : (uint8_t*)ray_sys_alloc(nc);
    if (!visited) {
        if (stk1 != stk1_local) ray_sys_free(stk1);
        if (stk2 != stk2_local) ray_sys_free(stk2);
        return root;
    }
    memset(visited, 0, nc);

    int sp1 = 0, sp2 = 0;
    stk1[sp1++] = root->id;
    while (sp1 > 0) {
        uint32_t nid = stk1[--sp1];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = 1;
        stk2[sp2++] = nid;

        ray_op_t* n = &g->nodes[nid];
        if (n->flags & OP_FLAG_DEAD) continue;
        for (int i = 0; i < n->arity && i < 2; i++) {
            ray_op_t* ci = op_child(g, n, i);
            if (ci && sp1 < (int)cap)
                stk1[sp1++] = ci->id;
        }
    }

    /* Post-order: pop stk2 from top, call try_rewrite.  Track whether
     * the root itself was rewritten — caller needs the new pointer to
     * avoid executing the dead node. */
    uint32_t root_id = root->id;
    while (sp2 > 0) {
        uint32_t nid = stk2[--sp2];
        ray_op_t* repl = try_rewrite(g, &g->nodes[nid]);
        if (repl && nid == root_id) {
            root = repl;
            root_id = repl->id;
        }
    }

    if (visited != visited_local) ray_sys_free(visited);
    if (stk1 != stk1_local) ray_sys_free(stk1);
    if (stk2 != stk2_local) ray_sys_free(stk2);
    return root;
}

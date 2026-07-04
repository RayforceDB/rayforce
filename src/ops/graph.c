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

#include "graph.h"
#include "internal.h"   /* EXT_TRAIL, graph_alloc_ext_node_ex */
#include "store/csr.h"
#include "store/hnsw.h"
#include "mem/sys.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Graph allocation helpers
 * -------------------------------------------------------------------------- */

#define GRAPH_INIT_CAP 4096

/* Graph edges are stable uint32 node ids (indices into g->nodes), so a
   realloc of g->nodes needs no pointer fixups — the former graph_fix_ptr /
   graph_fixup_ptrs / graph_fixup_ext_ptrs machinery is gone entirely. */

/* L3: node_count is uint32_t — theoretical overflow at 2^32 nodes is
   unreachable in practice (would require ~128 GB for the nodes array). */
ray_op_t* graph_alloc_node(ray_graph_t* g) {
    if (g->node_count >= g->node_cap) {
        /* H2: Overflow guard — if node_cap is already > UINT32_MAX/2,
           doubling would wrap around to a smaller value. */
        if (g->node_cap > UINT32_MAX / 2) return NULL;
        uint32_t new_cap = g->node_cap * 2;
        ray_op_t* new_nodes = (ray_op_t*)ray_sys_realloc(g->nodes,
                                                      new_cap * sizeof(ray_op_t));
        if (!new_nodes) return NULL;
        g->nodes = new_nodes;
        g->node_cap = new_cap;
    }
    ray_op_t* n = &g->nodes[g->node_count];
    memset(n, 0, sizeof(ray_op_t));
    n->id = g->node_count;
    n->in_id[0] = n->in_id[1] = RAY_OP_NONE;
    g->node_count++;
    return n;
}

ray_op_ext_t* graph_alloc_ext_node_ex(ray_graph_t* g, size_t extra) {
    /* Extended nodes are 64 bytes; extra bytes appended for inline arrays */
    ray_op_ext_t* ext = (ray_op_ext_t*)ray_sys_alloc(sizeof(ray_op_ext_t) + extra);
    if (!ext) return NULL;
    memset(ext, 0, sizeof(ray_op_ext_t) + extra);
    ext->base.in_id[0] = ext->base.in_id[1] = RAY_OP_NONE;
    ext->third_in = RAY_OP_NONE;

    /* Also add a placeholder in the nodes array for ID tracking */
    if (g->node_count >= g->node_cap) {
        if (g->node_cap > UINT32_MAX / 2) { ray_sys_free(ext); return NULL; }
        uint32_t new_cap = g->node_cap * 2;
        ray_op_t* new_nodes = (ray_op_t*)ray_sys_realloc(g->nodes,
                                                      new_cap * sizeof(ray_op_t));
        if (!new_nodes) { ray_sys_free(ext); return NULL; }
        g->nodes = new_nodes;
        g->node_cap = new_cap;
    }
    ext->base.id = g->node_count;
    /* H4: Do NOT copy ext->base to nodes[] here — the caller fills in
       fields first and then syncs via g->nodes[ext->base.id] = ext->base. */
    memset(&g->nodes[g->node_count], 0, sizeof(ray_op_t));
    g->nodes[g->node_count].id = g->node_count;
    g->nodes[g->node_count].in_id[0] = g->nodes[g->node_count].in_id[1] = RAY_OP_NONE;
    g->node_count++;

    /* Track ext node for cleanup */
    if (g->ext_count >= g->ext_cap) {
        if (g->ext_cap > UINT32_MAX / 2) { g->node_count--; ray_sys_free(ext); return NULL; }
        uint32_t new_cap = g->ext_cap == 0 ? 16 : g->ext_cap * 2;
        ray_op_ext_t** new_exts = (ray_op_ext_t**)ray_sys_realloc(g->ext_nodes,
                                                               new_cap * sizeof(ray_op_ext_t*));
        if (!new_exts) { g->node_count--; ray_sys_free(ext); return NULL; }
        g->ext_nodes = new_exts;
        g->ext_cap = new_cap;
    }
    g->ext_nodes[g->ext_count++] = ext;

    return ext;
}

static ray_op_ext_t* graph_alloc_ext_node(ray_graph_t* g) {
    return graph_alloc_ext_node_ex(g, 0);
}

/* EXT_TRAIL is now declared in ops/internal.h so other op modules can
 * embed inline arrays the same way. */

/* --------------------------------------------------------------------------
 * ray_graph_new / ray_graph_free
 * -------------------------------------------------------------------------- */

ray_graph_t* ray_graph_new(ray_t* tbl) {
    ray_graph_t* g = (ray_graph_t*)ray_sys_alloc(sizeof(ray_graph_t));
    if (!g) return NULL;

    g->nodes = (ray_op_t*)ray_sys_alloc(GRAPH_INIT_CAP * sizeof(ray_op_t));
    if (!g->nodes) { ray_sys_free(g); return NULL; }
    g->node_cap = GRAPH_INIT_CAP;
    g->node_count = 0;
    g->table = tbl;
    if (tbl) ray_retain(tbl);

    g->tables = NULL;
    g->n_tables = 0;

    g->ext_nodes = NULL;
    g->ext_count = 0;
    g->ext_cap = 0;
    g->selection = NULL;
    g->sg_col = NULL;
    g->sg_slices_hdr = NULL;
    g->sg_nslices = 0;

    g->cexpr_env_top = 0;  /* compile-time lambda/let env, initially empty */

    return g;
}

void ray_graph_free(ray_graph_t* g) {
    if (!g) return;

    /* Unconsumed slice-group hint (error paths / shapes that never
     * reached exec_group). */
    if (g->sg_col)        { ray_release(g->sg_col); g->sg_col = NULL; }
    if (g->sg_slices_hdr) { ray_free(g->sg_slices_hdr); g->sg_slices_hdr = NULL; }

    /* M6: Release OP_CONST literal values before freeing ext nodes */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        ray_op_ext_t* ext = g->ext_nodes[i];
        if (ext && g->nodes[ext->base.id].opcode == OP_CONST && ext->literal) {
            ray_release(ext->literal);
        }
        /* Release runtime-built SIP bitmaps on graph traversal nodes */
        if (ext) {
            uint16_t oc = g->nodes[ext->base.id].opcode;
            if ((oc == OP_EXPAND || oc == OP_VAR_EXPAND || oc == OP_SHORTEST_PATH)
                && ext->graph.sip_sel) {
                ray_release((ray_t*)ext->graph.sip_sel);
            }
            if (oc == OP_ASTAR && ext->graph.node_props) {
                ray_release((ray_t*)ext->graph.node_props);
            }
        }
    }
    /* Free seg_mask bitmaps (shared across ext nodes — deduplicate) */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        ray_op_ext_t* ext = g->ext_nodes[i];
        if (ext && ext->seg_mask) {
            uint64_t* mask = ext->seg_mask;
            ext->seg_mask = NULL;
            /* Clear same pointer from other ext nodes */
            for (uint32_t j = i + 1; j < g->ext_count; j++) {
                if (g->ext_nodes[j] && g->ext_nodes[j]->seg_mask == mask)
                    g->ext_nodes[j]->seg_mask = NULL;
            }
            ray_sys_free(mask);
        }
    }
    /* Free extended nodes */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        ray_sys_free(g->ext_nodes[i]);
    }
    ray_sys_free(g->ext_nodes);

    ray_sys_free(g->nodes);
    if (g->table) ray_release(g->table);

    /* Release table registry */
    if (g->tables) {
        for (uint16_t i = 0; i < g->n_tables; i++) {
            if (g->tables[i]) ray_release(g->tables[i]);
        }
        ray_sys_free(g->tables);
    }

    if (g->selection) ray_release(g->selection);
    ray_sys_free(g);
}

/* --------------------------------------------------------------------------
 * Source ops
 * -------------------------------------------------------------------------- */

ray_op_t* ray_scan(ray_graph_t* g, const char* col_name) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_SCAN;
    ext->base.arity = 0;

    /* Intern the column name to get symbol ID */
    int64_t sym_id = ray_sym_intern(col_name, strlen(col_name));
    ext->sym = sym_id;

    /* Infer output type from the bound table */
    if (g->table) {
        ray_t* col = ray_table_get_col(g->table, sym_id);
        if (col) {
            ext->base.out_type = col->type;
            ext->base.est_rows = (uint32_t)col->len;
        }
    }

    /* Update the nodes array with the filled base */
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_f64(ray_graph_t* g, double val) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = RAY_F64;
    ext->literal = ray_f64(val);
    /* L4: null/error check on allocation result */
    if (!ext->literal || RAY_IS_ERR(ext->literal)) ext->literal = NULL;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_i64(ray_graph_t* g, int64_t val) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = RAY_I64;
    ext->literal = ray_i64(val);
    /* L4: null/error check on allocation result */
    if (!ext->literal || RAY_IS_ERR(ext->literal)) ext->literal = NULL;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_bool(ray_graph_t* g, bool val) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = RAY_BOOL;
    ext->literal = ray_bool(val);
    /* L4: null/error check on allocation result */
    if (!ext->literal || RAY_IS_ERR(ext->literal)) ext->literal = NULL;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_str(ray_graph_t* g, const char* s, size_t len) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = RAY_SYM;   /* string constants resolve to SYM at exec time */
    ext->literal = ray_str(s, len);
    /* L4: null/error check on allocation result */
    if (!ext->literal || RAY_IS_ERR(ext->literal)) ext->literal = NULL;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_vec(ray_graph_t* g, ray_t* vec) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = vec->type;
    ext->base.est_rows = (uint32_t)vec->len;
    ext->literal = vec;
    ray_retain(vec);

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* Generic const-atom constructor.  Handles any scalar atom type
 * (RAY_SYM, RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_GUID, RAY_NULL,
 * and any other ray_t* used as an immediate literal).  The executor
 * OP_CONST handler just returns ext->literal, so the same retain/
 * store mechanism as ray_const_vec works for atoms too. */
ray_op_t* ray_const_atom(ray_graph_t* g, ray_t* atom) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    /* Atom types are stored negated (-RAY_I64 etc); normalise to the
     * positive type tag so downstream passes (promote(), fold_binary_const's
     * out_type switch) see the same values used by ray_const_i64 /
     * ray_const_f64 / ray_const_bool / ray_const_vec.  Without this, the
     * I32/DATE/TIME arm of fold_binary_const (opt.c:454) is dead for any
     * atom-typed const built via this constructor (e.g. DATE/TIME literals
     * from rfl). */
    ext->base.out_type = atom->type < 0 ? (int8_t)(-(int)atom->type) : atom->type;
    ext->base.est_rows = 1;
    ext->literal = atom;
    ray_retain(atom);

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_const_table(ray_graph_t* g, ray_t* tbl) {
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.out_type = RAY_TABLE;
    ext->literal = tbl;
    ray_retain(tbl);

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Helper: create unary/binary node
 * -------------------------------------------------------------------------- */

static ray_op_t* make_unary(ray_graph_t* g, uint16_t opcode, ray_op_t* a, int8_t out_type) {
    /* Save ID before alloc — realloc may invalidate the pointer */
    uint32_t a_id = a->id;
    uint32_t est = a->est_rows;
    ray_op_t* n = graph_alloc_node(g);
    if (!n) return NULL;
    a = &g->nodes[a_id];  /* re-resolve after potential realloc */

    n->opcode = opcode;
    n->arity = 1;
    n->in_id[0] = a->id;
    n->out_type = out_type;
    n->est_rows = est;
    return n;
}

static ray_op_t* make_binary(ray_graph_t* g, uint16_t opcode, ray_op_t* a, ray_op_t* b, int8_t out_type) {
    /* Save IDs before alloc — realloc may invalidate the pointers */
    uint32_t a_id = a->id;
    uint32_t b_id = b->id;
    uint32_t est = a->est_rows > b->est_rows ? a->est_rows : b->est_rows;
    ray_op_t* n = graph_alloc_node(g);
    if (!n) return NULL;
    a = &g->nodes[a_id];  /* re-resolve after potential realloc */
    b = &g->nodes[b_id];

    n->opcode = opcode;
    n->arity = 2;
    n->in_id[0] = a->id;
    n->in_id[1] = b->id;
    n->out_type = out_type;
    n->est_rows = est;
    return n;
}

/* Type promotion: BOOL < U8 < I16 < I32 < I64 < F64.
 * RAY_STR is its own type class — not promotable to numeric types. */
static int8_t promote(int8_t a, int8_t b) {
    if (a == RAY_STR || b == RAY_STR) return RAY_STR;
    if (a == RAY_F64 || b == RAY_F64) return RAY_F64;
    if (a == RAY_I64 || b == RAY_I64 || a == RAY_SYM || b == RAY_SYM ||
        a == RAY_TIMESTAMP || b == RAY_TIMESTAMP) return RAY_I64;
    if (a == RAY_I32 || b == RAY_I32 ||
        a == RAY_DATE || b == RAY_DATE || a == RAY_TIME || b == RAY_TIME) return RAY_I32;
    if (a == RAY_I16 || b == RAY_I16) return RAY_I16;
    if (a == RAY_U8 || b == RAY_U8) return RAY_U8;
    return RAY_BOOL;
}

/* --------------------------------------------------------------------------
 * Unary element-wise ops
 * -------------------------------------------------------------------------- */

ray_op_t* ray_neg(ray_graph_t* g, ray_op_t* a)     { return make_unary(g, OP_NEG, a, a->out_type); }
ray_op_t* ray_abs(ray_graph_t* g, ray_op_t* a)     { return make_unary(g, OP_ABS, a, a->out_type); }
ray_op_t* ray_not(ray_graph_t* g, ray_op_t* a)     { return make_unary(g, OP_NOT, a, RAY_BOOL); }
ray_op_t* ray_sqrt_op(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_SQRT, a, RAY_F64); }
ray_op_t* ray_log_op(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_LOG, a, RAY_F64); }
ray_op_t* ray_exp_op(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_EXP, a, RAY_F64); }
ray_op_t* ray_ceil_op(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_CEIL, a, a->out_type); }
ray_op_t* ray_floor_op(ray_graph_t* g, ray_op_t* a){ return make_unary(g, OP_FLOOR, a, a->out_type); }
ray_op_t* ray_round_op(ray_graph_t* g, ray_op_t* a){ return make_unary(g, OP_ROUND, a, a->out_type); }
ray_op_t* ray_isnull(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_ISNULL, a, RAY_BOOL); }

ray_op_t* ray_cast(ray_graph_t* g, ray_op_t* a, int8_t target_type) {
    return make_unary(g, OP_CAST, a, target_type);
}

/* --------------------------------------------------------------------------
 * Binary element-wise ops
 * -------------------------------------------------------------------------- */

/* Generic binary op constructor — opcode-driven, no switch/case needed by caller */
ray_op_t* ray_binop(ray_graph_t* g, uint16_t opcode, ray_op_t* a, ray_op_t* b) {
    int8_t out;
    switch (opcode) {
    case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        out = RAY_BOOL; break;
    case OP_DIV:
        out = RAY_F64; break;
    default:
        out = promote(a->out_type, b->out_type); break;
    }
    return make_binary(g, opcode, a, b, out);
}

ray_op_t* ray_add(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_ADD, a, b, promote(a->out_type, b->out_type)); }
ray_op_t* ray_sub(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_SUB, a, b, promote(a->out_type, b->out_type)); }
ray_op_t* ray_mul(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_MUL, a, b, promote(a->out_type, b->out_type)); }
ray_op_t* ray_div(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_DIV, a, b, RAY_F64); }
ray_op_t* ray_idiv(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_IDIV, a, b, RAY_I64); }
ray_op_t* ray_mod(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_MOD, a, b, promote(a->out_type, b->out_type)); }

ray_op_t* ray_eq(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_EQ, a, b, RAY_BOOL); }
ray_op_t* ray_ne(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_NE, a, b, RAY_BOOL); }
ray_op_t* ray_lt(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_LT, a, b, RAY_BOOL); }
ray_op_t* ray_le(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_LE, a, b, RAY_BOOL); }
ray_op_t* ray_gt(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_GT, a, b, RAY_BOOL); }
ray_op_t* ray_ge(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_GE, a, b, RAY_BOOL); }
ray_op_t* ray_and(ray_graph_t* g, ray_op_t* a, ray_op_t* b){ return make_binary(g, OP_AND, a, b, RAY_BOOL); }
ray_op_t* ray_or(ray_graph_t* g, ray_op_t* a, ray_op_t* b) { return make_binary(g, OP_OR, a, b, RAY_BOOL); }
ray_op_t* ray_min2(ray_graph_t* g, ray_op_t* a, ray_op_t* b){ return make_binary(g, OP_MIN2, a, b, promote(a->out_type, b->out_type)); }
ray_op_t* ray_max2(ray_graph_t* g, ray_op_t* a, ray_op_t* b){ return make_binary(g, OP_MAX2, a, b, promote(a->out_type, b->out_type)); }
ray_op_t* ray_in(ray_graph_t* g, ray_op_t* col, ray_op_t* set){ return make_binary(g, OP_IN, col, set, RAY_BOOL); }
ray_op_t* ray_not_in(ray_graph_t* g, ray_op_t* col, ray_op_t* set){ return make_binary(g, OP_NOT_IN, col, set, RAY_BOOL); }

ray_op_t* ray_if(ray_graph_t* g, ray_op_t* cond, ray_op_t* then_val, ray_op_t* else_val) {
    /* 3-input node: cond, then, else — needs ext node */
    uint32_t cond_id = cond->id;
    uint32_t then_id = then_val->id;
    uint32_t else_id = else_val->id;
    int8_t out_type = promote(then_val->out_type, else_val->out_type);
    /* IF preserves string types: promote() handles RAY_STR (wins over SYM);
     * SYM override only applies when neither side is RAY_STR */
    if (out_type != RAY_STR &&
        (then_val->out_type == RAY_SYM || else_val->out_type == RAY_SYM))
        out_type = RAY_SYM;
    uint32_t est = cond->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    /* Re-resolve after potential realloc (else_val stored as index, not pointer) */
    cond = &g->nodes[cond_id];
    then_val = &g->nodes[then_id];

    ext->base.opcode = OP_IF;
    ext->base.arity = 2;  /* inputs[0]=cond, inputs[1]=then; else via ext */
    ext->base.in_id[0] = cond->id;
    ext->base.in_id[1] = then_val->id;
    ext->base.out_type = out_type;
    ext->base.est_rows = est;
    /* Store else_val as a node ID in the dedicated third_in field. */
    ext->third_in = else_id;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_like(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern) {
    return make_binary(g, OP_LIKE, input, pattern, RAY_BOOL);
}

ray_op_t* ray_ilike(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern) {
    return make_binary(g, OP_ILIKE, input, pattern, RAY_BOOL);
}

/* String ops */
ray_op_t* ray_upper(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_UPPER, a, a->out_type == RAY_STR ? RAY_STR : RAY_SYM); }
ray_op_t* ray_lower(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_LOWER, a, a->out_type == RAY_STR ? RAY_STR : RAY_SYM); }
ray_op_t* ray_strlen(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_STRLEN, a, RAY_I64); }
ray_op_t* ray_trim_op(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_TRIM, a, a->out_type == RAY_STR ? RAY_STR : RAY_SYM); }

ray_op_t* ray_substr(ray_graph_t* g, ray_op_t* str, ray_op_t* start, ray_op_t* len) {
    /* 3-input: str=inputs[0], start=inputs[1], len stored via literal field */
    uint32_t s_id = str->id;
    uint32_t st_id = start->id;
    uint32_t l_id = len->id;
    uint32_t est = str->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    str   = &g->nodes[s_id];
    start = &g->nodes[st_id];

    ext->base.opcode = OP_SUBSTR;
    ext->base.arity = 2;
    ext->base.in_id[0] = str->id;
    ext->base.in_id[1] = start->id;
    ext->base.out_type = (str->out_type == RAY_STR) ? RAY_STR : RAY_SYM;
    ext->base.est_rows = est;
    ext->third_in = l_id;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_replace(ray_graph_t* g, ray_op_t* str, ray_op_t* from, ray_op_t* to) {
    /* 3-input: str=inputs[0], from=inputs[1], to stored via literal field */
    uint32_t s_id = str->id;
    uint32_t f_id = from->id;
    uint32_t t_id = to->id;
    uint32_t est = str->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    str  = &g->nodes[s_id];
    from = &g->nodes[f_id];

    ext->base.opcode = OP_REPLACE;
    ext->base.arity = 2;
    ext->base.in_id[0] = str->id;
    ext->base.in_id[1] = from->id;
    ext->base.out_type = (str->out_type == RAY_STR) ? RAY_STR : RAY_SYM;
    ext->base.est_rows = est;
    ext->third_in = t_id;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_concat(ray_graph_t* g, ray_op_t** args, int n) {
    /* Variadic: first 2 in inputs[], rest in trailing IDs */
    if (!args || n < 2) return NULL;
    /* M4: Guard VLA upper bound */
    if (n > 256) return NULL;
    size_t n_args = (size_t)n;
    if (n_args > (SIZE_MAX / sizeof(uint32_t))) return NULL;
    size_t extra = (n > 2) ? (size_t)(n - 2) * sizeof(uint32_t) : 0;

    /* Save IDs before alloc (n is small — bounded by function arity) */
    uint32_t ids[n];
    for (int i = 0; i < n; i++) ids[i] = args[i]->id;
    uint32_t est = args[0]->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, extra);
    if (!ext) return NULL;

    ext->base.opcode = OP_CONCAT;
    ext->base.arity = 2;
    ext->base.in_id[0] = ids[0];
    ext->base.in_id[1] = ids[1];
    /* RAY_STR if any input is RAY_STR, else RAY_SYM */
    int8_t out_type = RAY_SYM;
    for (int i = 0; i < n; i++) {
        if (args[i]->out_type == RAY_STR) { out_type = RAY_STR; break; }
    }
    ext->base.out_type = out_type;
    ext->base.est_rows = est;
    ext->sym = n; /* total arg count stored in sym field */

    /* Extra args in trailing bytes */
    uint32_t* trail = (uint32_t*)EXT_TRAIL(ext);
    for (int i = 2; i < n; i++) trail[i - 2] = ids[i];

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Reduction ops
 * -------------------------------------------------------------------------- */

ray_op_t* ray_sum(ray_graph_t* g, ray_op_t* a)    { return make_unary(g, OP_SUM, a, a->out_type == RAY_F64 ? RAY_F64 : RAY_I64); }
ray_op_t* ray_prod(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_PROD, a, a->out_type == RAY_F64 ? RAY_F64 : RAY_I64); }
ray_op_t* ray_min_op(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_MIN, a, a->out_type); }
ray_op_t* ray_max_op(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_MAX, a, a->out_type); }
ray_op_t* ray_count(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_COUNT, a, RAY_I64); }
ray_op_t* ray_avg(ray_graph_t* g, ray_op_t* a)    { return make_unary(g, OP_AVG, a, RAY_F64); }
ray_op_t* ray_first(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_FIRST, a, a->out_type); }
ray_op_t* ray_last(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_LAST, a, a->out_type); }
ray_op_t* ray_count_distinct(ray_graph_t* g, ray_op_t* a) { return make_unary(g, OP_COUNT_DISTINCT, a, RAY_I64); }
ray_op_t* ray_distinct_op(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_DISTINCT, a, a->out_type); }
ray_op_t* ray_asc_op(ray_graph_t* g, ray_op_t* a)       { return make_unary(g, OP_ASC, a, a->out_type); }
ray_op_t* ray_desc_op(ray_graph_t* g, ray_op_t* a)      { return make_unary(g, OP_DESC, a, a->out_type); }
ray_op_t* ray_reverse_op(ray_graph_t* g, ray_op_t* a)   { return make_unary(g, OP_REVERSE, a, a->out_type); }
ray_op_t* ray_stddev(ray_graph_t* g, ray_op_t* a)     { return make_unary(g, OP_STDDEV, a, RAY_F64); }
ray_op_t* ray_stddev_pop(ray_graph_t* g, ray_op_t* a)  { return make_unary(g, OP_STDDEV_POP, a, RAY_F64); }
ray_op_t* ray_var(ray_graph_t* g, ray_op_t* a)         { return make_unary(g, OP_VAR, a, RAY_F64); }
ray_op_t* ray_var_pop(ray_graph_t* g, ray_op_t* a)     { return make_unary(g, OP_VAR_POP, a, RAY_F64); }

/* Exact median per group. Runtime forks to a separate bucket-scatter +
 * quickselect path (see ray_median_per_group) — it can't fit the
 * fixed-size row-layout HT because per-group buffer size is variable. */
ray_op_t* ray_median(ray_graph_t* g, ray_op_t* a) {
    return make_unary(g, OP_MEDIAN, a, RAY_F64);
}

/* --------------------------------------------------------------------------
 * Structural ops
 * -------------------------------------------------------------------------- */

ray_op_t* ray_filter(ray_graph_t* g, ray_op_t* input, ray_op_t* predicate) {
    uint32_t input_id = input->id;
    uint32_t pred_id = predicate->id;
    uint32_t est = input->est_rows / 2;  /* estimate: 50% selectivity */

    ray_op_t* n = graph_alloc_node(g);
    if (!n) return NULL;

    input = &g->nodes[input_id];
    predicate = &g->nodes[pred_id];

    n->opcode = OP_FILTER;
    n->arity = 2;
    n->in_id[0] = input->id;
    n->in_id[1] = predicate->id;
    n->out_type = input->out_type;
    n->est_rows = est;
    return n;
}

ray_op_t* ray_sort_op(ray_graph_t* g, ray_op_t* table_node,
                     ray_op_t** keys, uint8_t* descs, uint8_t* nulls_first,
                     uint32_t n_cols) {
    uint32_t table_id = table_node->id;
    ray_t* ids_hdr = NULL;
    uint32_t* key_ids = (uint32_t*)scratch_alloc(&ids_hdr,
                            (size_t)(n_cols ? n_cols : 1) * sizeof(uint32_t));
    if (!key_ids) return NULL;
    for (uint32_t i = 0; i < n_cols; i++) key_ids[i] = keys[i]->id;

    size_t keys_sz = (size_t)n_cols * sizeof(ray_op_t*);
    size_t descs_sz = (size_t)n_cols;
    size_t nf_sz = (size_t)n_cols;
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, keys_sz + descs_sz + nf_sz);
    if (!ext) { scratch_free(ids_hdr); return NULL; }

    table_node = &g->nodes[table_id];

    ext->base.opcode = OP_SORT;
    ext->base.arity = 1;
    ext->base.in_id[0] = table_node->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = table_node->est_rows;

    /* Arrays embedded in trailing space — freed with ext node */
    char* trail = EXT_TRAIL(ext);
    ext->sort.columns = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_cols; i++)
        ext->sort.columns[i] = key_ids[i];
    ext->sort.desc = (uint8_t*)(trail + keys_sz);
    memcpy(ext->sort.desc, descs, descs_sz);
    ext->sort.nulls_first = (uint8_t*)(trail + keys_sz + descs_sz);
    if (nulls_first) {
        memcpy(ext->sort.nulls_first, nulls_first, nf_sz);
    } else {
        /* Default: NULLS LAST for ASC, NULLS FIRST for DESC */
        for (uint32_t i = 0; i < n_cols; i++)
            ext->sort.nulls_first[i] = descs[i] ? 1 : 0;
    }
    ext->sort.n_cols = n_cols;

    scratch_free(ids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* Shared impl for ray_group / ray_group2 / ray_group3.  agg_ins2 NULL →
 * no binary aggs; otherwise must be the same length as agg_ins (NULL
 * slots for unary aggs, non-NULL for OP_PEARSON_CORR slots).  agg_k NULL
 * → no scalar params; otherwise length n_aggs (0 in slots without). */
static ray_op_t* ray_group_impl(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys,
                                uint16_t* agg_ops, ray_op_t** agg_ins,
                                ray_op_t** agg_ins2, const int64_t* agg_k,
                                uint32_t n_aggs) {
    /* One carve for all three id arrays (key_ids/agg_ids/agg_ids2 are all
     * uint32_t, so a single flat buffer sliced by offset suffices). */
    ray_t* ids_hdr = NULL;
    size_t ids_elems = (size_t)n_keys + (size_t)n_aggs * 2;
    uint32_t* ids_buf = (uint32_t*)scratch_alloc(&ids_hdr,
                            (ids_elems ? ids_elems : 1) * sizeof(uint32_t));
    if (!ids_buf) return NULL;
    uint32_t* key_ids  = ids_buf;
    uint32_t* agg_ids  = ids_buf + n_keys;
    uint32_t* agg_ids2 = ids_buf + n_keys + n_aggs;  /* parallel to agg_ids; 0 when no second input */
    bool has_ins2 = false;
    bool has_k = false;
    for (uint32_t i = 0; i < n_keys; i++) key_ids[i] = keys[i]->id;
    for (uint32_t i = 0; i < n_aggs; i++) {
        agg_ids[i]  = agg_ins[i]->id;
        agg_ids2[i] = RAY_OP_NONE;
        if (agg_ins2 && agg_ins2[i]) {
            agg_ids2[i] = agg_ins2[i]->id;
            has_ins2 = true;
        }
        if (agg_k && agg_k[i] != 0) has_k = true;
    }

    size_t keys_sz = (size_t)n_keys * sizeof(ray_op_t*);
    size_t ops_sz  = (size_t)n_aggs * sizeof(uint16_t);
    size_t ins_sz  = (size_t)n_aggs * sizeof(ray_op_t*);
    size_t ins2_sz = has_ins2 ? ins_sz : 0;
    size_t k_sz    = has_k ? (size_t)n_aggs * sizeof(int64_t) : 0;
    /* Align ops after keys (pointer-sized), ins after ops, ins2 after ins. */
    size_t ops_off  = keys_sz;
    size_t ins_off  = ops_off + ops_sz;
    /* Round ins_off up to pointer alignment */
    ins_off = (ins_off + sizeof(ray_op_t*) - 1) & ~(sizeof(ray_op_t*) - 1);
    size_t ins2_off = ins_off + ins_sz;
    size_t k_off    = ins2_off + ins2_sz;
    /* Round k_off up to int64 alignment */
    k_off = (k_off + sizeof(int64_t) - 1) & ~(sizeof(int64_t) - 1);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, k_off + k_sz);
    if (!ext) { scratch_free(ids_hdr); return NULL; }

    ext->base.opcode = OP_GROUP;
    ext->base.arity = 0;
    ext->base.out_type = RAY_TABLE;
    if (n_keys > 0 && keys[0])
        ext->base.est_rows = g->nodes[key_ids[0]].est_rows / 10;  /* rough estimate */
    ext->base.in_id[0] = n_keys > 0 ? key_ids[0] : RAY_OP_NONE;

    /* Arrays embedded in trailing space — freed with ext node */
    char* trail = EXT_TRAIL(ext);
    ext->keys = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_keys; i++)
        ext->keys[i] = key_ids[i];
    ext->agg_ops = (uint16_t*)(trail + ops_off);
    if (ops_sz > 0) memcpy(ext->agg_ops, agg_ops, ops_sz);
    ext->agg_ins = (uint32_t*)(trail + ins_off);
    for (uint32_t i = 0; i < n_aggs; i++)
        ext->agg_ins[i] = agg_ids[i];
    if (has_ins2) {
        ext->agg_ins2 = (uint32_t*)(trail + ins2_off);
        for (uint32_t i = 0; i < n_aggs; i++)
            ext->agg_ins2[i] = agg_ids2[i] != RAY_OP_NONE ? agg_ids2[i] : RAY_OP_NONE;
    } else {
        ext->agg_ins2 = NULL;
    }
    if (has_k) {
        ext->agg_k = (int64_t*)(trail + k_off);
        for (uint32_t i = 0; i < n_aggs; i++)
            ext->agg_k[i] = agg_k ? agg_k[i] : 0;
    } else {
        ext->agg_k = NULL;
    }
    ext->n_keys = n_keys;
    ext->n_aggs = n_aggs;

    scratch_free(ids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_group(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys,
                   uint16_t* agg_ops, ray_op_t** agg_ins, uint32_t n_aggs) {
    return ray_group_impl(g, keys, n_keys, agg_ops, agg_ins, NULL, NULL, n_aggs);
}

ray_op_t* ray_group2(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys,
                     uint16_t* agg_ops, ray_op_t** agg_ins,
                     ray_op_t** agg_ins2, uint32_t n_aggs) {
    return ray_group_impl(g, keys, n_keys, agg_ops, agg_ins, agg_ins2, NULL, n_aggs);
}

ray_op_t* ray_group3(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys,
                     uint16_t* agg_ops, ray_op_t** agg_ins,
                     ray_op_t** agg_ins2, const int64_t* agg_k,
                     uint32_t n_aggs) {
    return ray_group_impl(g, keys, n_keys, agg_ops, agg_ins, agg_ins2, agg_k, n_aggs);
}

ray_op_t* ray_distinct(ray_graph_t* g, ray_op_t** keys, uint32_t n_keys) {
    return ray_group(g, keys, n_keys, NULL, NULL, 0);
}

ray_op_t* ray_pivot_op(ray_graph_t* g,
                       ray_op_t** index_cols, uint32_t n_index,
                       ray_op_t* pivot_col,
                       ray_op_t* value_col,
                       uint16_t agg_op) {
    ray_t* idx_hdr = NULL;
    uint32_t* idx_ids = (uint32_t*)scratch_alloc(&idx_hdr,
                            (size_t)(n_index ? n_index : 1) * sizeof(uint32_t));
    if (!idx_ids) return NULL;
    for (uint32_t i = 0; i < n_index; i++) idx_ids[i] = index_cols[i]->id;
    uint32_t pcol_id = pivot_col->id;
    uint32_t vcol_id = value_col->id;

    size_t idx_sz = (size_t)n_index * sizeof(ray_op_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, idx_sz);
    if (!ext) { scratch_free(idx_hdr); return NULL; }

    ext->base.opcode = OP_PIVOT;
    ext->base.arity = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = 0; /* unknown until execution */

    char* trail = EXT_TRAIL(ext);
    ext->pivot.index_cols = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_index; i++)
        ext->pivot.index_cols[i] = idx_ids[i];
    ext->pivot.pivot_col = pcol_id;
    ext->pivot.value_col = vcol_id;
    ext->pivot.agg_op = agg_op;
    ext->pivot.n_index = n_index;

    scratch_free(idx_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_join(ray_graph_t* g,
                  ray_op_t* left_table, ray_op_t** left_keys,
                  ray_op_t* right_table, ray_op_t** right_keys,
                  uint32_t n_keys, uint8_t join_type) {
    uint32_t left_table_id = left_table->id;
    uint32_t right_table_id = right_table->id;
    ray_t* keyids_hdr = NULL;
    size_t keyids_elems = (size_t)n_keys * 2;
    uint32_t* keyids_buf = (uint32_t*)scratch_alloc(&keyids_hdr,
                            (keyids_elems ? keyids_elems : 1) * sizeof(uint32_t));
    if (!keyids_buf) return NULL;
    uint32_t* lkey_ids = keyids_buf;
    uint32_t* rkey_ids = keyids_buf + n_keys;
    for (uint32_t i = 0; i < n_keys; i++) {
        lkey_ids[i] = left_keys[i]->id;
        rkey_ids[i] = right_keys[i]->id;
    }

    size_t keys_sz = (size_t)n_keys * sizeof(ray_op_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, keys_sz * 2);
    if (!ext) { scratch_free(keyids_hdr); return NULL; }

    left_table = &g->nodes[left_table_id];
    right_table = &g->nodes[right_table_id];

    ext->base.opcode = OP_JOIN;
    ext->base.arity = 2;
    ext->base.in_id[0] = left_table->id;
    ext->base.in_id[1] = right_table->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = left_table->est_rows;

    /* Arrays embedded in trailing space — freed with ext node */
    char* trail = EXT_TRAIL(ext);
    ext->join.left_keys = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_keys; i++)
        ext->join.left_keys[i] = lkey_ids[i];
    ext->join.right_keys = (uint32_t*)(trail + (size_t)n_keys * sizeof(ray_op_t*));
    for (uint32_t i = 0; i < n_keys; i++)
        ext->join.right_keys[i] = rkey_ids[i];
    ext->join.n_join_keys = n_keys;
    ext->join.join_type = join_type;

    scratch_free(keyids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_antijoin(ray_graph_t* g,
                      ray_op_t* left_table, ray_op_t** left_keys,
                      ray_op_t* right_table, ray_op_t** right_keys,
                      uint32_t n_keys) {
    uint32_t left_table_id = left_table->id;
    uint32_t right_table_id = right_table->id;
    ray_t* keyids_hdr = NULL;
    size_t keyids_elems = (size_t)n_keys * 2;
    uint32_t* keyids_buf = (uint32_t*)scratch_alloc(&keyids_hdr,
                            (keyids_elems ? keyids_elems : 1) * sizeof(uint32_t));
    if (!keyids_buf) return NULL;
    uint32_t* lkey_ids = keyids_buf;
    uint32_t* rkey_ids = keyids_buf + n_keys;
    for (uint32_t i = 0; i < n_keys; i++) {
        lkey_ids[i] = left_keys[i]->id;
        rkey_ids[i] = right_keys[i]->id;
    }

    size_t keys_sz = (size_t)n_keys * sizeof(ray_op_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, keys_sz * 2);
    if (!ext) { scratch_free(keyids_hdr); return NULL; }

    left_table = &g->nodes[left_table_id];
    right_table = &g->nodes[right_table_id];

    ext->base.opcode = OP_ANTIJOIN;
    ext->base.arity = 2;
    ext->base.in_id[0] = left_table->id;
    ext->base.in_id[1] = right_table->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = left_table->est_rows;

    char* trail = EXT_TRAIL(ext);
    ext->join.left_keys = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_keys; i++)
        ext->join.left_keys[i] = lkey_ids[i];
    ext->join.right_keys = (uint32_t*)(trail + (size_t)n_keys * sizeof(ray_op_t*));
    for (uint32_t i = 0; i < n_keys; i++)
        ext->join.right_keys[i] = rkey_ids[i];
    ext->join.n_join_keys = n_keys;
    ext->join.join_type = 3;  /* anti */

    scratch_free(keyids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_asof_join(ray_graph_t* g,
                       ray_op_t* left_table, ray_op_t* right_table,
                       ray_op_t* time_key,
                       ray_op_t** eq_keys, uint32_t n_eq_keys,
                       uint8_t join_type) {
    uint32_t left_id  = left_table->id;
    uint32_t right_id = right_table->id;
    uint32_t time_id  = time_key->id;
    ray_t* eq_hdr = NULL;
    uint32_t* eq_ids = (uint32_t*)scratch_alloc(&eq_hdr,
                            (size_t)(n_eq_keys ? n_eq_keys : 1) * sizeof(uint32_t));
    if (!eq_ids) return NULL;
    for (uint32_t i = 0; i < n_eq_keys; i++) eq_ids[i] = eq_keys[i]->id;

    /* Trailing: [eq_keys: n_eq_keys * ptr] */
    size_t keys_sz = (size_t)n_eq_keys * sizeof(ray_op_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, keys_sz);
    if (!ext) { scratch_free(eq_hdr); return NULL; }

    left_table  = &g->nodes[left_id];
    right_table = &g->nodes[right_id];

    ext->base.opcode  = OP_WINDOW_JOIN;
    ext->base.arity   = 2;
    ext->base.in_id[0] = left_table->id;
    ext->base.in_id[1] = right_table->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = left_table->est_rows;

    ext->asof.time_key   = time_id;
    ext->asof.n_eq_keys  = n_eq_keys;
    ext->asof.join_type  = join_type;
    ext->asof.eq_keys    = (uint32_t*)EXT_TRAIL(ext);
    for (uint32_t i = 0; i < n_eq_keys; i++)
        ext->asof.eq_keys[i] = eq_ids[i];

    scratch_free(eq_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_window_op(ray_graph_t* g, ray_op_t* table_node,
                       ray_op_t** part_keys, uint32_t n_part,
                       ray_op_t** order_keys, uint8_t* order_descs, uint32_t n_order,
                       uint8_t* func_kinds, ray_op_t** func_inputs,
                       int64_t* func_params, uint32_t n_funcs,
                       uint8_t frame_type, uint8_t frame_start, uint8_t frame_end,
                       int64_t frame_start_n, int64_t frame_end_n) {
    /* One carve for all three id arrays (all uint32_t; sliced by offset). */
    ray_t* ids_hdr = NULL;
    size_t ids_elems = (size_t)n_part + (size_t)n_order + (size_t)n_funcs;
    uint32_t* ids_buf = (uint32_t*)scratch_alloc(&ids_hdr,
                            (ids_elems ? ids_elems : 1) * sizeof(uint32_t));
    if (!ids_buf) return NULL;
    uint32_t* part_ids  = ids_buf;
    uint32_t* order_ids = ids_buf + n_part;
    uint32_t* func_ids  = ids_buf + n_part + n_order;
    for (uint32_t i = 0; i < n_part; i++) part_ids[i] = part_keys[i]->id;
    for (uint32_t i = 0; i < n_order; i++) order_ids[i] = order_keys[i]->id;
    for (uint32_t i = 0; i < n_funcs; i++) func_ids[i] = func_inputs[i]->id;

    /* Trailing layout:
     *   [part_keys:   n_part * ptr]
     *   [order_keys:  n_order * ptr]
     *   [order_descs: n_order * 1B]
     *   [padding to ptr alignment]
     *   [func_inputs: n_funcs * ptr]
     *   [func_kinds:  n_funcs * 1B]
     *   [padding to 8B alignment]
     *   [func_params: n_funcs * 8B]
     */
    size_t pk_sz    = (size_t)n_part  * sizeof(ray_op_t*);
    size_t ok_sz    = (size_t)n_order * sizeof(ray_op_t*);
    size_t od_sz    = (size_t)n_order;
    size_t od_end   = pk_sz + ok_sz + od_sz;
    size_t fi_off   = (od_end + sizeof(ray_op_t*) - 1) & ~(sizeof(ray_op_t*) - 1);
    size_t fi_sz    = (size_t)n_funcs * sizeof(ray_op_t*);
    size_t fk_off   = fi_off + fi_sz;
    size_t fk_sz    = (size_t)n_funcs;
    size_t fp_off   = (fk_off + fk_sz + 7) & ~(size_t)7;
    size_t fp_sz    = (size_t)n_funcs * sizeof(int64_t);
    size_t total    = fp_off + fp_sz;

    /* Save IDs before alloc — realloc may invalidate pointers */
    uint32_t table_id = table_node->id;
    uint32_t est   = table_node->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, total);
    if (!ext) { scratch_free(ids_hdr); return NULL; }

    /* Re-resolve table_node after potential realloc */
    table_node = &g->nodes[table_id];

    ext->base.opcode   = OP_WINDOW;
    ext->base.arity    = 1;
    ext->base.in_id[0] = table_node->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = est;  /* window preserves row count */

    /* Fill trailing arrays */
    char* trail = EXT_TRAIL(ext);
    ext->window.part_keys = (uint32_t*)trail;
    for (uint32_t i = 0; i < n_part; i++)
        ext->window.part_keys[i] = part_ids[i];

    ext->window.order_keys = (uint32_t*)(trail + pk_sz);
    for (uint32_t i = 0; i < n_order; i++)
        ext->window.order_keys[i] = order_ids[i];

    ext->window.order_descs = (uint8_t*)(trail + pk_sz + ok_sz);
    if (n_order) memcpy(ext->window.order_descs, order_descs, od_sz);

    ext->window.func_inputs = (uint32_t*)(trail + fi_off);
    for (uint32_t i = 0; i < n_funcs; i++)
        ext->window.func_inputs[i] = func_ids[i];

    ext->window.func_kinds = (uint8_t*)(trail + fk_off);
    if (n_funcs) memcpy(ext->window.func_kinds, func_kinds, fk_sz);

    ext->window.func_params = (int64_t*)(trail + fp_off);
    if (n_funcs) memcpy(ext->window.func_params, func_params, fp_sz);

    ext->window.n_part_keys   = n_part;
    ext->window.n_order_keys  = n_order;
    ext->window.n_funcs       = n_funcs;
    ext->window.frame_type    = frame_type;
    ext->window.frame_start   = frame_start;
    ext->window.frame_end     = frame_end;
    ext->window.frame_start_n = frame_start_n;
    ext->window.frame_end_n   = frame_end_n;

    scratch_free(ids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_select_op(ray_graph_t* g, ray_op_t* input,
                        ray_op_t** cols, uint32_t n_cols) {
    uint32_t input_id = input->id;
    ray_t* ids_hdr = NULL;
    uint32_t* col_ids = (uint32_t*)scratch_alloc(&ids_hdr,
                            (size_t)(n_cols ? n_cols : 1) * sizeof(uint32_t));
    if (!col_ids) return NULL;
    for (uint32_t i = 0; i < n_cols; i++) col_ids[i] = cols[i]->id;

    size_t cols_sz = (size_t)n_cols * sizeof(ray_op_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, cols_sz);
    if (!ext) { scratch_free(ids_hdr); return NULL; }

    input = &g->nodes[input_id];

    ext->base.opcode = OP_SELECT;
    ext->base.arity = 1;
    ext->base.in_id[0] = input->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = input->est_rows;

    /* Array embedded in trailing space — freed with ext node */
    ext->sort.columns = (uint32_t*)EXT_TRAIL(ext);
    for (uint32_t i = 0; i < n_cols; i++)
        ext->sort.columns[i] = col_ids[i];
    ext->sort.n_cols = n_cols;

    scratch_free(ids_hdr);
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* L6: When n (stored as ext->sym) is 0, HEAD produces an empty result
   with the same schema as the input. */
ray_op_t* ray_head(ray_graph_t* g, ray_op_t* input, int64_t n) {
    uint32_t input_id = input->id;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    input = &g->nodes[input_id];

    ext->base.opcode = OP_HEAD;
    ext->base.arity = 1;
    ext->base.in_id[0] = input->id;
    ext->base.out_type = input->out_type;
    ext->base.est_rows = (uint32_t)n;
    ext->sym = n;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_tail(ray_graph_t* g, ray_op_t* input, int64_t n) {
    uint32_t input_id = input->id;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    input = &g->nodes[input_id];

    ext->base.opcode = OP_TAIL;
    ext->base.arity = 1;
    ext->base.in_id[0] = input->id;
    ext->base.out_type = input->out_type;
    ext->base.est_rows = (uint32_t)n;
    ext->sym = n;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_extract(ray_graph_t* g, ray_op_t* col, int64_t field) {
    uint32_t col_id = col->id;
    uint32_t est = col->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    col = &g->nodes[col_id];  /* re-resolve after potential realloc */

    ext->base.opcode = OP_EXTRACT;
    ext->base.arity = 1;
    ext->base.in_id[0] = col->id;
    ext->base.out_type = RAY_I64;
    ext->base.est_rows = est;
    ext->sym = field;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_date_trunc(ray_graph_t* g, ray_op_t* col, int64_t field) {
    uint32_t col_id = col->id;
    uint32_t est = col->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    col = &g->nodes[col_id];  /* re-resolve after potential realloc */

    ext->base.opcode = OP_DATE_TRUNC;
    ext->base.arity = 1;
    ext->base.in_id[0] = col->id;
    ext->base.out_type = RAY_TIMESTAMP;  /* returns timestamp (microseconds) */
    ext->base.est_rows = est;
    ext->sym = field;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Multi-table support
 * -------------------------------------------------------------------------- */

uint16_t ray_graph_add_table(ray_graph_t* g, ray_t* table) {
    uint16_t id = g->n_tables;
    uint16_t new_cap = id + 1;

    ray_t** new_tables = (ray_t**)ray_sys_realloc(g->tables,
                                                (size_t)new_cap * sizeof(ray_t*));
    if (!new_tables) return UINT16_MAX;  /* error sentinel */
    g->tables = new_tables;
    g->tables[id] = table;
    ray_retain(table);
    g->n_tables = new_cap;

    return id;
}

ray_op_t* ray_scan_table(ray_graph_t* g, uint16_t table_id, const char* col_name) {
    if (table_id >= g->n_tables || !g->tables[table_id]) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode = OP_SCAN;
    ext->base.arity = 0;

    int64_t sym_id = ray_sym_intern(col_name, strlen(col_name));
    ext->sym = sym_id;

    /* Store table_id+1 in pad[0..1] as uint16_t (0 = default g->table) */
    uint16_t stored_id = table_id + 1;
    memcpy(ext->base.pad, &stored_id, sizeof(uint16_t));

    /* Infer output type from the specified table */
    ray_t* tbl = g->tables[table_id];
    if (tbl) {
        ray_t* col = ray_table_get_col(tbl, sym_id);
        if (col) {
            ext->base.out_type = col->type;
            ext->base.est_rows = (uint32_t)col->len;
        }
    }

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Graph traversal DAG builders
 * -------------------------------------------------------------------------- */

ray_op_t* ray_expand(ray_graph_t* g, ray_op_t* src_nodes,
                    ray_rel_t* rel, uint8_t direction) {
    uint32_t src_id = src_nodes->id;
    uint32_t est = src_nodes->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    src_nodes = &g->nodes[src_id];

    ext->base.opcode = OP_EXPAND;
    ext->base.arity = 1;
    ext->base.in_id[0] = src_nodes->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = est * 10;  /* rough estimate: 10x fan-out */
    ext->graph.rel = rel;
    ext->graph.direction = direction;
    ext->graph.min_depth = 1;
    ext->graph.max_depth = 1;
    ext->graph.path_tracking = 0;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_var_expand(ray_graph_t* g, ray_op_t* start_nodes,
                        ray_rel_t* rel, uint8_t direction,
                        uint8_t min_depth, uint8_t max_depth,
                        bool track_path) {
    uint32_t src_id = start_nodes->id;
    uint32_t est = start_nodes->est_rows;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    start_nodes = &g->nodes[src_id];

    ext->base.opcode = OP_VAR_EXPAND;
    ext->base.arity = 1;
    ext->base.in_id[0] = start_nodes->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = est * 100;  /* rough estimate */
    ext->graph.rel = rel;
    ext->graph.direction = direction;
    ext->graph.min_depth = min_depth;
    ext->graph.max_depth = max_depth;
    ext->graph.path_tracking = track_path ? 1 : 0;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_shortest_path(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                           ray_rel_t* rel, uint8_t max_depth) {
    uint32_t src_id = src->id;
    uint32_t dst_id = dst->id;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    src = &g->nodes[src_id];
    dst = &g->nodes[dst_id];

    ext->base.opcode = OP_SHORTEST_PATH;
    ext->base.arity = 2;
    ext->base.in_id[0] = src->id;
    ext->base.in_id[1] = dst->id;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = max_depth;
    ext->graph.rel = rel;
    ext->graph.direction = 0;  /* forward by default */
    ext->graph.min_depth = 0;
    ext->graph.max_depth = max_depth;
    ext->graph.path_tracking = 0;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Graph algorithm builders
 * -------------------------------------------------------------------------- */

ray_op_t* ray_pagerank(ray_graph_t* g, ray_rel_t* rel,
                      uint16_t max_iter, double damping) {
    if (!g || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode   = OP_PAGERANK;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel      = rel;
    ext->graph.max_iter  = max_iter;
    ext->graph.damping   = damping;
    ext->graph.direction = 0;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_connected_comp(ray_graph_t* g, ray_rel_t* rel) {
    if (!g || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode   = OP_CONNECTED_COMP;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel     = rel;
    ext->graph.direction = 2;  /* both directions for undirected */

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_dijkstra(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                      ray_rel_t* rel, const char* weight_col,
                      uint8_t max_depth) {
    if (!g || !src || !rel || !weight_col) return NULL;

    /* Save IDs before alloc — realloc may invalidate the pointers */
    uint32_t src_id = src->id;
    uint32_t dst_id = dst ? dst->id : 0;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    src = &g->nodes[src_id];
    if (dst) dst = &g->nodes[dst_id];

    ext->base.opcode    = OP_DIJKSTRA;
    ext->base.arity     = dst ? 2 : 1;
    ext->base.in_id[0] = src->id;
    ext->base.in_id[1] = dst ? dst->id : RAY_OP_NONE;
    ext->base.out_type  = RAY_TABLE;
    ext->base.est_rows  = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel       = rel;
    ext->graph.direction = 0;
    ext->graph.max_depth = max_depth;
    ext->graph.weight_col_sym = ray_sym_intern(weight_col, (int64_t)strlen(weight_col));

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_louvain(ray_graph_t* g, ray_rel_t* rel, uint16_t max_iter) {
    if (!g || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode   = OP_LOUVAIN;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel      = rel;
    ext->graph.max_iter  = max_iter > 0 ? max_iter : 100;
    ext->graph.direction = 2;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_degree_cent(ray_graph_t* g, ray_rel_t* rel) {
    if (!g || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode   = OP_DEGREE_CENT;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel     = rel;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_topsort(ray_graph_t* g, ray_rel_t* rel) {
    if (!g || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    ext->base.opcode   = OP_TOPSORT;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel     = rel;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_dfs(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel, uint8_t max_depth) {
    if (!g || !src || !rel) return NULL;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    uint32_t src_id = src->id;
    src = &g->nodes[src_id];

    ext->base.opcode     = OP_DFS;
    ext->base.arity      = 1;
    ext->base.in_id[0]   = src->id;
    ext->base.out_type   = RAY_TABLE;
    ext->base.est_rows   = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel       = rel;
    ext->graph.direction = 0;
    ext->graph.max_depth = max_depth;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_wco_join(ray_graph_t* g,
                      ray_rel_t** rels, uint8_t n_rels,
                      uint8_t n_vars) {
    size_t extra = (size_t)n_rels * sizeof(ray_rel_t*);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, extra);
    if (!ext) return NULL;

    ext->base.opcode = OP_WCO_JOIN;
    ext->base.arity = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = 1000;  /* rough estimate */

    /* Copy rels array into trailing bytes */
    ray_rel_t** trail = (ray_rel_t**)EXT_TRAIL(ext);
    if (n_rels > 0) memcpy(trail, rels, (size_t)n_rels * sizeof(ray_rel_t*));
    ext->wco.rels = (void**)trail;
    ext->wco.n_rels = n_rels;
    ext->wco.n_vars = n_vars;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_cluster_coeff(ray_graph_t* g, ray_rel_t* rel) {
    if (!g || !rel) return NULL;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    ext->base.opcode   = OP_CLUSTER_COEFF;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel     = rel;
    ext->graph.direction = 2;
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_random_walk(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel,
                        uint16_t walk_length) {
    if (!g || !src || !rel) return NULL;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    uint32_t src_id = src->id;
    src = &g->nodes[src_id];
    ext->base.opcode    = OP_RANDOM_WALK;
    ext->base.arity     = 1;
    ext->base.in_id[0] = src->id;
    ext->base.out_type  = RAY_TABLE;
    ext->base.est_rows  = walk_length + 1;
    ext->graph.rel      = rel;
    ext->graph.max_iter = walk_length;
    ext->graph.direction = 0;
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_astar(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                  ray_rel_t* rel, const char* weight_col,
                  const char* lat_col, const char* lon_col,
                  ray_t* node_props, uint8_t max_depth) {
    if (!g || !src || !dst || !rel || !weight_col || !lat_col || !lon_col || !node_props)
        return NULL;

    /* Save IDs before alloc — realloc may invalidate the pointers */
    uint32_t src_id = src->id;
    uint32_t dst_id = dst->id;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    src = &g->nodes[src_id];
    dst = &g->nodes[dst_id];

    ext->base.opcode    = OP_ASTAR;
    ext->base.arity     = 2;
    ext->base.in_id[0] = src->id;
    ext->base.in_id[1] = dst->id;
    ext->base.out_type  = RAY_TABLE;
    ext->base.est_rows  = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel       = rel;
    ext->graph.direction = 0;
    ext->graph.max_depth = max_depth;
    ext->graph.weight_col_sym = ray_sym_intern(weight_col, (int64_t)strlen(weight_col));
    ext->graph.coord_col_syms[0] = ray_sym_intern(lat_col, (int64_t)strlen(lat_col));
    ext->graph.coord_col_syms[1] = ray_sym_intern(lon_col, (int64_t)strlen(lon_col));
    ext->graph.node_props = node_props;
    ray_retain(node_props);

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_k_shortest(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                       ray_rel_t* rel, const char* weight_col, uint16_t k) {
    if (!g || !src || !dst || !rel || !weight_col || k == 0) return NULL;

    /* Save IDs before alloc — realloc may invalidate the pointers */
    uint32_t src_id = src->id;
    uint32_t dst_id = dst->id;

    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;

    src = &g->nodes[src_id];
    dst = &g->nodes[dst_id];

    ext->base.opcode    = OP_K_SHORTEST;
    ext->base.arity     = 2;
    ext->base.in_id[0] = src->id;
    ext->base.in_id[1] = dst->id;
    ext->base.out_type  = RAY_TABLE;
    ext->base.est_rows  = (uint32_t)(k * rel->fwd.n_nodes);
    ext->graph.rel       = rel;
    ext->graph.direction = 0;
    ext->graph.max_iter  = k;
    ext->graph.weight_col_sym = ray_sym_intern(weight_col, (int64_t)strlen(weight_col));

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_betweenness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size) {
    if (!g || !rel) return NULL;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    ext->base.opcode   = OP_BETWEENNESS;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel       = rel;
    ext->graph.direction = 2;  /* undirected BFS */
    ext->graph.max_iter  = sample_size;  /* 0 = exact */
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_closeness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size) {
    if (!g || !rel) return NULL;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    ext->base.opcode   = OP_CLOSENESS;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)rel->fwd.n_nodes;
    ext->graph.rel       = rel;
    ext->graph.direction = 2;  /* undirected BFS */
    ext->graph.max_iter  = sample_size;  /* 0 = exact */
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_mst(ray_graph_t* g, ray_rel_t* rel, const char* weight_col) {
    if (!g || !rel || !weight_col) return NULL;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    ext->base.opcode   = OP_MST;
    ext->base.arity    = 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.est_rows = (uint32_t)(rel->fwd.n_nodes > 0 ? rel->fwd.n_nodes - 1 : 0);
    ext->graph.rel     = rel;
    ext->graph.direction = 2;
    ext->graph.weight_col_sym = ray_sym_intern(weight_col, (int64_t)strlen(weight_col));
    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_ann_rerank(ray_graph_t* g, ray_op_t* src,
                         ray_hnsw_t* idx, const float* query_vec,
                         int32_t dim, int64_t k, int32_t ef_search) {
    if (!g || !src || !idx || !query_vec || dim <= 0 || k <= 0) return NULL;

    uint32_t src_id = src->id;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    src = &g->nodes[src_id];

    ext->base.opcode     = OP_ANN_RERANK;
    ext->base.arity      = 1;
    ext->base.in_id[0]   = src->id;
    ext->base.out_type   = RAY_TABLE;
    ext->base.est_rows   = (uint32_t)k;
    ext->rerank.hnsw_idx  = idx;
    ext->rerank.col_sym   = 0;
    ext->rerank.query_vec = (float*)query_vec;
    ext->rerank.dim       = dim;
    ext->rerank.metric    = idx ? idx->metric : RAY_HNSW_COSINE;
    ext->rerank.k         = k;
    ext->rerank.ef_search = ef_search > 0 ? ef_search : HNSW_DEFAULT_EF_S;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

ray_op_t* ray_knn_rerank(ray_graph_t* g, ray_op_t* src,
                         int64_t col_sym, const float* query_vec,
                         int32_t dim, int64_t k, ray_hnsw_metric_t metric) {
    if (!g || !src || !query_vec || dim <= 0 || k <= 0 || col_sym <= 0) return NULL;

    uint32_t src_id = src->id;
    ray_op_ext_t* ext = graph_alloc_ext_node(g);
    if (!ext) return NULL;
    src = &g->nodes[src_id];

    ext->base.opcode     = OP_KNN_RERANK;
    ext->base.arity      = 1;
    ext->base.in_id[0]   = src->id;
    ext->base.out_type   = RAY_TABLE;
    ext->base.est_rows   = (uint32_t)k;
    ext->rerank.hnsw_idx  = NULL;
    ext->rerank.col_sym   = col_sym;
    ext->rerank.query_vec = (float*)query_vec;
    ext->rerank.dim       = dim;
    ext->rerank.metric    = (int32_t)metric;
    ext->rerank.k         = k;
    ext->rerank.ef_search = 0;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* --------------------------------------------------------------------------
 * Lazy DAG handles
 * -------------------------------------------------------------------------- */

ray_op_t* ray_graph_input_vec(ray_graph_t* g, ray_t* vec) {
    return ray_const_vec(g, vec);
}

ray_t* ray_lazy_wrap(ray_graph_t* g, ray_op_t* op) {
    ray_t* h = ray_alloc(0);
    if (!h) { ray_graph_free(g); return ray_error("oom", NULL); }
    h->type  = RAY_LAZY;
    h->attrs = 0;
    RAY_LAZY_GRAPH(h) = g;
    RAY_LAZY_OP(h)    = op;
    return h;
}

ray_t* ray_lazy_append(ray_t* lazy, uint16_t opcode) {
    ray_graph_t* g    = RAY_LAZY_GRAPH(lazy);
    ray_op_t*    prev = RAY_LAZY_OP(lazy);

    /* Determine output type based on opcode */
    int8_t out_type;
    switch (opcode) {
        case OP_COUNT:
        case OP_COUNT_DISTINCT:
            out_type = RAY_I64; break;
        case OP_AVG:
        case OP_STDDEV:
        case OP_STDDEV_POP:
        case OP_VAR:
        case OP_VAR_POP:
            out_type = RAY_F64; break;
        case OP_SUM:
        case OP_PROD:
            out_type = (prev->out_type == RAY_F64) ? RAY_F64 : RAY_I64; break;
        case OP_DISTINCT:
            out_type = prev->out_type; break;     /* distinct preserves type */
        case OP_ASC:
            out_type = prev->out_type; break;     /* asc preserves type */
        case OP_DESC:
            out_type = prev->out_type; break;     /* desc preserves type */
        case OP_REVERSE:
            out_type = prev->out_type; break;     /* reverse preserves type */
        default:
            out_type = prev->out_type; break;
    }

    ray_op_t* op = make_unary(g, opcode, prev, out_type);
    if (!op) return ray_error("oom", NULL);
    RAY_LAZY_OP(lazy) = op;
    /* The VM's op_call1 always releases arg after calling fn(arg).  Since
     * ray_lazy_append returns the SAME lazy object it received, the caller
     * would see rc drop to 0 without this retain.  Retain here so the
     * caller's release brings rc back to the pre-call level. */
    ray_retain(lazy);
    return lazy;
}

ray_t* ray_lazy_materialize(ray_t* val) {
    if (!ray_is_lazy(val)) return val;

    ray_graph_t* g  = RAY_LAZY_GRAPH(val);
    ray_op_t*    op = RAY_LAZY_OP(val);

    /* Run the full optimizer pipeline before execution. ray_optimize
       may return a different root (e.g. predicate pushdown). */
    op = ray_optimize(g, op);

    ray_t* result = ray_execute(g, op);

    ray_graph_free(g);
    /* Clear graph pointer before releasing to prevent double-free in
     * ray_release_owned_refs */
    RAY_LAZY_GRAPH(val) = NULL;
    ray_release(val);
    return result;
}

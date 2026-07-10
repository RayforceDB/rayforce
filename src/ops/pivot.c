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

#include "ops/internal.h"
#include "ops/hash.h"
#include "ops/idxop.h"
#include "ops/rowsel.h"

/* Resolved string atom (borrowed) of a SYM-scalar broadcast input
 * (atom -RAY_SYM, or a 1-elem RAY_SYM_W{8,16,32,64} vec used as
 * scalar).  Atoms are runtime-domain by design; a vec scalar is
 * CELL-DATA and resolves through its own domain.  Hides the atom/vec
 * dispatch: ray_t->i64 aliases ray_t->len, so reading `v->i64` on a
 * vec silently yields `len` (= 1) instead of the element value —
 * always go through these helpers for a then_v/else_v that may be
 * either atom or 1-elem vec. */
static inline ray_t* sym_scalar_str(ray_t* v) {
    return ray_is_atom(v) ? ray_sym_str(v->i64) : ray_sym_vec_cell(v, 0);
}

/* A SYM id taken from `col`'s cells, re-expressed in the RUNTIME domain
 * (fresh SYM output vecs built here are runtime-domain).  Fast path: a
 * runtime-domain column's ids ARE runtime ids — raw copy.  Otherwise a
 * lock-free read of the per-domain runtime-id LUT (pivot is sequential,
 * so the LUT's first-request vocabulary intern is safe here; mirrors
 * lang/internal.h sym_id_runtime — exact no-op while every domain is
 * the runtime singleton). */
static inline int64_t sym_id_runtime(ray_t* col, int64_t id) {
    struct ray_sym_domain_s* dom = ray_sym_vec_domain(col);
    if (dom == ray_sym_runtime_domain()) return id;
    const int64_t* lut = ray_sym_domain_runtime_lut(dom);
    if (!lut || id < 0 || id >= ray_sym_domain_count(dom)) return -1;
    return lut[id];
}

static inline int64_t sym_cell_runtime_id(ray_t* v, int64_t i) {
    return sym_id_runtime(v, ray_read_sym(ray_data(v), i, v->type, v->attrs));
}

static inline int64_t sym_scalar_runtime_id(ray_t* v) {
    return ray_is_atom(v) ? v->i64 : sym_cell_runtime_id(v, 0);
}

static inline ray_t* str_vec_pool_obj(ray_t* v) {
    if (!v || v->type != RAY_STR) return NULL;
    ray_t* owner = (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v;
    return owner ? owner->str_pool : NULL;
}

enum { IF_BRANCH_MAX_SCAN_SYMS = 64 };

typedef struct {
    int64_t syms[IF_BRANCH_MAX_SCAN_SYMS];
    int     n_syms;
} if_branch_plan_t;

static bool if_plan_add_sym(if_branch_plan_t* p, int64_t sym) {
    for (int i = 0; i < p->n_syms; i++)
        if (p->syms[i] == sym) return true;
    if (p->n_syms >= IF_BRANCH_MAX_SCAN_SYMS) return false;
    p->syms[p->n_syms++] = sym;
    return true;
}

static bool if_op_rowwise(uint16_t opc) {
    switch (opc) {
    case OP_SCAN: case OP_CONST:
    case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
    case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR: case OP_ROUND:
    case OP_SIN: case OP_ASIN: case OP_COS: case OP_ACOS:
    case OP_TAN: case OP_ATAN: case OP_RECIPROCAL: case OP_SIGNUM:
    case OP_ISNULL: case OP_CAST:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
    case OP_POW:
    case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_AND: case OP_OR:
    case OP_MIN2: case OP_MAX2: case OP_IF:
    case OP_LIKE: case OP_ILIKE: case OP_UPPER: case OP_LOWER:
    case OP_STRLEN: case OP_SUBSTR: case OP_REPLACE: case OP_TRIM:
    case OP_CONCAT: case OP_STR_FIND:
    case OP_EXTRACT: case OP_DATE_TRUNC:
        return true;
    default:
        return false;
    }
}

static bool if_collect_branch(ray_graph_t* g, ray_op_t* op,
                              if_branch_plan_t* plan, uint64_t* visited) {
    if (!g || !op) return false;
    uint32_t nid = op->id;
    if (nid < g->node_count) {
        uint64_t bit = (uint64_t)1 << (nid & 63);
        uint64_t* word = &visited[nid >> 6];
        if (*word & bit) return true;
        *word |= bit;
    }

    uint16_t opc = op->opcode;
    if (opc == OP_CONST) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        return ext && ext->literal && ray_is_atom(ext->literal);
    }
    if (opc == OP_SCAN) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (!ext) return false;
        uint16_t stored_table_id = 0;
        memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
        if (stored_table_id != 0) return false;
        return if_plan_add_sym(plan, ext->sym);
    }
    if (!if_op_rowwise(opc)) return false;

    for (uint8_t i = 0; i < op->arity && i < 2; i++) {
        if (op->in_id[i] == RAY_OP_NONE) continue;
        if (!if_collect_branch(g, op_child(g, op, i), plan, visited))
            return false;
    }

    ray_op_ext_t* ext = find_ext(g, op->id);
    if ((opc == OP_IF || opc == OP_SUBSTR || opc == OP_REPLACE) && ext) {
        if (ext->third_in != RAY_OP_NONE &&
            !if_collect_branch(g, op_node(g, ext->third_in), plan, visited))
            return false;
    } else if (opc == OP_CONCAT && ext) {
        int n_args = (int)ext->sym;
        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
        for (int i = 2; i < n_args; i++) {
            if (!if_collect_branch(g, op_node(g, trail[i - 2]), plan, visited))
                return false;
        }
    }
    return true;
}

static bool if_make_branch_plan(ray_graph_t* g, ray_op_t* root,
                                if_branch_plan_t* plan) {
    memset(plan, 0, sizeof(*plan));
    if (!g || !root || g->node_count == 0) return false;
    ray_t* hdr = NULL;
    size_t words = ((size_t)g->node_count + 63u) >> 6;
    uint64_t* visited = (uint64_t*)scratch_calloc(&hdr, words * sizeof(uint64_t));
    if (!visited) return false;
    bool ok = if_collect_branch(g, root, plan, visited);
    scratch_free(hdr);
    return ok;
}

static int64_t if_selected_true_count(const uint8_t* cond, int64_t nrows,
                                      ray_t* outer_sel) {
    int64_t total = 0;
    if (!outer_sel) {
        for (int64_t i = 0; i < nrows; i++) total += cond[i] ? 1 : 0;
        return total;
    }
    ray_rowsel_t* m = ray_rowsel_meta(outer_sel);
    const uint8_t* flags = ray_rowsel_flags(outer_sel);
    const uint32_t* offsets = ray_rowsel_offsets(outer_sel);
    const uint16_t* idx = ray_rowsel_idx(outer_sel);
    for (uint32_t seg = 0; seg < m->n_segs; seg++) {
        uint8_t f = flags[seg];
        if (f == RAY_SEL_NONE) continue;
        int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
        int64_t end = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        if (f == RAY_SEL_ALL) {
            for (int64_t r = base; r < end; r++) total += cond[r] ? 1 : 0;
        } else {
            uint32_t off = offsets[seg];
            uint32_t cnt = offsets[seg + 1] - off;
            for (uint32_t i = 0; i < cnt; i++) {
                int64_t r = base + idx[off + i];
                total += cond[r] ? 1 : 0;
            }
        }
    }
    return total;
}

static void if_fill_branch_ids(const uint8_t* cond, int64_t nrows,
                               ray_t* outer_sel, int64_t* true_ids,
                               int64_t* false_ids) {
    int64_t ti = 0, fi = 0;
#define IF_ADD_ROW(R) do { int64_t _r = (R); if (cond[_r]) true_ids[ti++] = _r; else false_ids[fi++] = _r; } while (0)
    if (!outer_sel) {
        for (int64_t r = 0; r < nrows; r++) IF_ADD_ROW(r);
    } else {
        ray_rowsel_t* m = ray_rowsel_meta(outer_sel);
        const uint8_t* flags = ray_rowsel_flags(outer_sel);
        const uint32_t* offsets = ray_rowsel_offsets(outer_sel);
        const uint16_t* idx = ray_rowsel_idx(outer_sel);
        for (uint32_t seg = 0; seg < m->n_segs; seg++) {
            uint8_t f = flags[seg];
            if (f == RAY_SEL_NONE) continue;
            int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
            int64_t end = base + RAY_MORSEL_ELEMS;
            if (end > nrows) end = nrows;
            if (f == RAY_SEL_ALL) {
                for (int64_t r = base; r < end; r++) IF_ADD_ROW(r);
            } else {
                uint32_t off = offsets[seg];
                uint32_t cnt = offsets[seg + 1] - off;
                for (uint32_t i = 0; i < cnt; i++) IF_ADD_ROW(base + idx[off + i]);
            }
        }
    }
#undef IF_ADD_ROW
}

static ray_t* if_eval_branch(ray_graph_t* g, ray_op_t* branch,
                             const if_branch_plan_t* plan,
                             int64_t* ids, int64_t count, int64_t nrows) {
    if (count <= 0) return NULL;
    ray_t* saved_table = g->table;
    ray_t* saved_sel = g->selection;
    ray_t* sub = NULL;
    ray_t* sel = NULL;

    if (plan->n_syms > 0 && count != nrows) {
        sel = ray_index_rowsel_from_ids(nrows, ids, count);
        if (!sel) return ray_error("oom", NULL);
        sub = sel_compact(g, saved_table, sel, plan->syms, plan->n_syms);
        ray_release(sel);
        if (!sub || RAY_IS_ERR(sub)) return sub ? sub : ray_error("oom", NULL);
    } else {
        sub = saved_table;
        ray_retain(sub);
    }

    g->table = sub;
    g->selection = NULL;
    ray_t* value = exec_node(g, branch);
    if (g->selection) {
        ray_release(g->selection);
        g->selection = NULL;
    }
    g->table = saved_table;
    g->selection = saved_sel;
    ray_release(sub);
    return value;
}

static inline bool if_value_scalar(ray_t* v) {
    return ray_is_atom(v) || (v->type > 0 && v->len == 1);
}

static int64_t if_value_index(ray_t* v, int64_t* ids, int64_t j,
                              int64_t count, int64_t nrows) {
    if (if_value_scalar(v)) return 0;
    if (v->len == count) return j;
    if (v->len == nrows) return ids[j];
    return -1;
}

static int64_t if_atom_i64(ray_t* v) {
    switch (-v->type) {
    case RAY_I64: case RAY_TIMESTAMP: return v->i64;
    case RAY_I32: case RAY_DATE: case RAY_TIME: return v->i32;
    case RAY_I16: return v->i16;
    case RAY_BOOL: case RAY_U8: return v->b8;
    case RAY_F64: case RAY_F32: return (int64_t)v->f64;
    default: return 0;
    }
}

static double if_atom_f64(ray_t* v) {
    return (v->type == -RAY_F64 || v->type == -RAY_F32)
         ? v->f64 : (double)if_atom_i64(v);
}

static int64_t if_vec_i64(ray_t* v, int64_t idx) {
    if (v->type == RAY_F64) return (int64_t)((double*)ray_data(v))[idx];
    if (v->type == RAY_F32) return (int64_t)((float*)ray_data(v))[idx];
    return read_col_i64(ray_data(v), idx, v->type, v->attrs);
}

static double if_vec_f64(ray_t* v, int64_t idx) {
    if (v->type == RAY_F64) return ((double*)ray_data(v))[idx];
    if (v->type == RAY_F32) return (double)((float*)ray_data(v))[idx];
    return (double)if_vec_i64(v, idx);
}

static int64_t if_scalar_i64(ray_t* v) {
    return ray_is_atom(v) ? if_atom_i64(v) : if_vec_i64(v, 0);
}

static double if_scalar_f64(ray_t* v) {
    return ray_is_atom(v) ? if_atom_f64(v) : if_vec_f64(v, 0);
}

static int8_t if_value_type(ray_t* v) {
    if (!v) return 0;
    if (ray_is_atom(v) && v->type == -RAY_STR) return RAY_SYM;
    int8_t t = ray_is_atom(v) ? (int8_t)(-(int)v->type) : v->type;
    return (t == RAY_F32) ? RAY_F64 : t;
}

static int8_t if_promote_type(int8_t a, int8_t b) {
    if (a == 0) return b;
    if (b == 0) return a;
    if (a == RAY_STR || b == RAY_STR) return RAY_STR;
    if (a == RAY_SYM || b == RAY_SYM) return RAY_SYM;
    if (a == RAY_F64 || b == RAY_F64 || a == RAY_F32 || b == RAY_F32)
        return RAY_F64;
    if (a == RAY_I64 || b == RAY_I64 ||
        a == RAY_TIMESTAMP || b == RAY_TIMESTAMP)
        return RAY_I64;
    if (a == RAY_I32 || b == RAY_I32 ||
        a == RAY_DATE || b == RAY_DATE || a == RAY_TIME || b == RAY_TIME)
        return RAY_I32;
    if (a == RAY_I16 || b == RAY_I16) return RAY_I16;
    if (a == RAY_U8 || b == RAY_U8) return RAY_U8;
    return RAY_BOOL;
}

static int8_t if_selected_type(ray_t* then_v, ray_t* else_v, int8_t fallback) {
    int8_t t = if_promote_type(if_value_type(then_v), if_value_type(else_v));
    return t ? t : fallback;
}

static bool if_scatter_numeric(ray_t* result, ray_t* value, int64_t* ids,
                               int64_t count, int64_t nrows, int8_t out_type) {
    if (!value) return true;
    for (int64_t j = 0; j < count; j++) {
        int64_t src = if_value_index(value, ids, j, count, nrows);
        if (src < 0) return false;
        int64_t dst = ids[j];
        if (out_type == RAY_F64) {
            double x = ray_is_atom(value) ? if_atom_f64(value)
                     : if_vec_f64(value, src);
            ((double*)ray_data(result))[dst] = x;
        } else if (out_type == RAY_I64 || out_type == RAY_TIMESTAMP) {
            int64_t x = ray_is_atom(value) ? if_atom_i64(value) : if_vec_i64(value, src);
            ((int64_t*)ray_data(result))[dst] = x;
        } else if (out_type == RAY_I32 || out_type == RAY_DATE || out_type == RAY_TIME) {
            int64_t x = ray_is_atom(value) ? if_atom_i64(value) : if_vec_i64(value, src);
            ((int32_t*)ray_data(result))[dst] = (int32_t)x;
        } else if (out_type == RAY_I16) {
            int64_t x = ray_is_atom(value) ? if_atom_i64(value) : if_vec_i64(value, src);
            ((int16_t*)ray_data(result))[dst] = (int16_t)x;
        } else {
            int64_t x = ray_is_atom(value) ? if_atom_i64(value) : if_vec_i64(value, src);
            ((uint8_t*)ray_data(result))[dst] = (uint8_t)x;
        }
    }
    return true;
}

static bool if_string_cell(ray_t* value, int64_t src,
                           const char** sp, size_t* sl) {
    if (value->type == -RAY_STR) {
        *sp = ray_str_ptr(value); *sl = ray_str_len(value); return true;
    }
    if (value->type == RAY_STR) {
        *sp = ray_str_vec_get(value, src, sl);
        if (!*sp) { *sp = ""; *sl = 0; }
        return true;
    }
    if (value->type == -RAY_SYM || RAY_IS_SYM(value->type)) {
        ray_t* s = ray_is_atom(value) ? sym_scalar_str(value)
                                      : ray_sym_vec_cell(value, src);
        *sp = s ? ray_str_ptr(s) : "";
        *sl = s ? ray_str_len(s) : 0;
        return true;
    }
    *sp = ""; *sl = 0;
    return true;
}

static ray_t* if_scatter_str(ray_t* result, ray_t* value, int64_t* ids,
                             int64_t count, int64_t nrows) {
    if (!value) return result;
    for (int64_t j = 0; j < count; j++) {
        int64_t src = if_value_index(value, ids, j, count, nrows);
        if (src < 0) { ray_release(result); return ray_error("length", "if: branch length mismatch"); }
        const char* sp = NULL;
        size_t sl = 0;
        if_string_cell(value, src, &sp, &sl);
        ray_t* next = ray_str_vec_set(result, ids[j], sp ? sp : "", sp ? sl : 0);
        if (!next || RAY_IS_ERR(next)) {
            ray_release(result);
            return next ? next : ray_error("oom", NULL);
        }
        result = next;
    }
    return result;
}

static int64_t if_sym_cell_value(ray_t* value, int64_t src) {
    if (value->type == -RAY_STR) return ray_sym_intern(ray_str_ptr(value), ray_str_len(value));
    if (value->type == RAY_STR) {
        size_t sl = 0;
        const char* sp = ray_str_vec_get(value, src, &sl);
        return ray_sym_intern(sp ? sp : "", sp ? sl : 0);
    }
    if (ray_is_atom(value)) return sym_scalar_runtime_id(value);
    return sym_cell_runtime_id(value, src);
}

static bool if_scatter_sym(ray_t* result, ray_t* value, int64_t* ids,
                           int64_t count, int64_t nrows) {
    if (!value) return true;
    int64_t* dst = (int64_t*)ray_data(result);
    for (int64_t j = 0; j < count; j++) {
        int64_t src = if_value_index(value, ids, j, count, nrows);
        if (src < 0) return false;
        dst[ids[j]] = if_sym_cell_value(value, src);
    }
    return true;
}

static bool if_lazy_supported_type(int8_t out_type) {
    return out_type == RAY_F64 || out_type == RAY_I64 ||
           out_type == RAY_I32 || out_type == RAY_I16 ||
           out_type == RAY_BOOL || out_type == RAY_U8 ||
           out_type == RAY_TIMESTAMP || out_type == RAY_TIME ||
           out_type == RAY_DATE || out_type == RAY_STR ||
           out_type == RAY_SYM;
}

static ray_t* exec_if_selected(ray_graph_t* g, ray_op_t* op, ray_t* cond_v) {
    if (!g || !g->table || !cond_v || cond_v->type != RAY_BOOL)
        return NULL;
    int64_t nrows = ray_table_nrows(g->table);
    if (cond_v->len != nrows) return NULL;

    if (!if_lazy_supported_type(op->out_type)) return NULL;

    ray_op_t* then_op = op_child(g, op, 1);
    ray_op_ext_t* ext = find_ext(g, op->id);
    ray_op_t* else_op = ext ? op_node(g, ext->third_in) : NULL;
    if (!then_op || !else_op) return NULL;

    if_branch_plan_t then_plan, else_plan;
    if (!if_make_branch_plan(g, then_op, &then_plan) ||
        !if_make_branch_plan(g, else_op, &else_plan))
        return NULL;

    ray_t* outer_sel = g->selection;
    if (outer_sel) {
        ray_rowsel_t* sm = ray_rowsel_meta(outer_sel);
        if (!sm || sm->nrows != nrows) return NULL;
    }

    int64_t selected = outer_sel ? ray_rowsel_meta(outer_sel)->total_pass : nrows;
    if (selected < 0 || selected > nrows) return NULL;

    uint8_t* cond = (uint8_t*)ray_data(cond_v);
    int64_t true_count = if_selected_true_count(cond, nrows, outer_sel);
    int64_t false_count = selected - true_count;
    if (true_count < 0 || false_count < 0) return NULL;

    ray_t* ids_hdr = NULL;
    int64_t* ids = NULL;
    if (selected > 0) {
        ids = (int64_t*)scratch_alloc(&ids_hdr, (size_t)selected * sizeof(int64_t));
        if (!ids) return ray_error("oom", NULL);
        if_fill_branch_ids(cond, nrows, outer_sel, ids, ids + true_count);
    }
    int64_t* true_ids = ids;
    int64_t* false_ids = ids ? ids + true_count : NULL;

    ray_t* then_v = if_eval_branch(g, then_op, &then_plan, true_ids, true_count, nrows);
    if (then_v && RAY_IS_ERR(then_v)) { scratch_free(ids_hdr); return then_v; }
    ray_t* else_v = if_eval_branch(g, else_op, &else_plan, false_ids, false_count, nrows);
    if (else_v && RAY_IS_ERR(else_v)) {
        if (then_v) ray_release(then_v);
        scratch_free(ids_hdr);
        return else_v;
    }

    int8_t out_type = if_selected_type(then_v, else_v, op->out_type);
    if (!if_lazy_supported_type(out_type)) {
        if (then_v) ray_release(then_v);
        if (else_v) ray_release(else_v);
        scratch_free(ids_hdr);
        return NULL;
    }

    ray_t* result = ray_vec_new(out_type, nrows);
    if (!result || RAY_IS_ERR(result)) {
        if (then_v) ray_release(then_v);
        if (else_v) ray_release(else_v);
        scratch_free(ids_hdr);
        return result;
    }
    result->len = nrows;
    if (out_type == RAY_STR)
        memset(ray_data(result), 0, (size_t)nrows * sizeof(ray_str_t));
    else
        memset(ray_data(result), 0, (size_t)nrows * ray_sym_elem_size(out_type, result->attrs));

    bool ok = true;
    if (out_type == RAY_STR) {
        result = if_scatter_str(result, then_v, true_ids, true_count, nrows);
        if (result && !RAY_IS_ERR(result))
            result = if_scatter_str(result, else_v, false_ids, false_count, nrows);
    } else if (out_type == RAY_SYM) {
        ok = if_scatter_sym(result, then_v, true_ids, true_count, nrows) &&
             if_scatter_sym(result, else_v, false_ids, false_count, nrows);
    } else {
        ok = if_scatter_numeric(result, then_v, true_ids, true_count, nrows, out_type) &&
             if_scatter_numeric(result, else_v, false_ids, false_count, nrows, out_type);
    }

    if (then_v) ray_release(then_v);
    if (else_v) ray_release(else_v);
    scratch_free(ids_hdr);

    if (!result || RAY_IS_ERR(result)) return result;
    if (!ok) {
        ray_release(result);
        return ray_error("length", "if: branch length mismatch");
    }
    return result;
}

/* ============================================================================
 * OP_IF: ternary select  result[i] = cond[i] ? then[i] : else[i]
 * ============================================================================ */

static ray_t* exec_if_eager(ray_graph_t* g, ray_op_t* op) {
    /* cond = inputs[0], then = inputs[1], else_id stored in ext->third_in */
    ray_t* cond_v = exec_node(g, op_child(g, op, 0));
    ray_t* then_v = exec_node(g, op_child(g, op, 1));

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t else_id = ext->third_in;
    ray_t* else_v = exec_node(g, op_node(g, else_id));

    if (!cond_v || RAY_IS_ERR(cond_v)) {
        if (then_v && !RAY_IS_ERR(then_v)) ray_release(then_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return cond_v;
    }
    if (!then_v || RAY_IS_ERR(then_v)) {
        ray_release(cond_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return then_v;
    }
    if (!else_v || RAY_IS_ERR(else_v)) {
        ray_release(cond_v); ray_release(then_v);
        return else_v;
    }

    int64_t len = cond_v->len;
    bool then_scalar = ray_is_atom(then_v) || (then_v->type > 0 && then_v->len == 1);
    bool else_scalar = ray_is_atom(else_v) || (else_v->type > 0 && else_v->len == 1);
    if (then_scalar && !else_scalar) len = else_v->len;
    if (!then_scalar) len = then_v->len;

    int8_t out_type = op->out_type;
    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(cond_v); ray_release(then_v); ray_release(else_v);
        return result;
    }
    result->len = len;

    uint8_t* cond_p = (uint8_t*)ray_data(cond_v);

    if (out_type == RAY_F64) {
        double t_scalar = then_scalar ? if_scalar_f64(then_v) : 0.0;
        double e_scalar = else_scalar ? if_scalar_f64(else_v) : 0.0;
        double* dst = (double*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            double tv = then_scalar ? t_scalar : if_vec_f64(then_v, i);
            double ev = else_scalar ? e_scalar : if_vec_f64(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_I64) {
        int64_t t_scalar = then_scalar ? if_scalar_i64(then_v) : 0;
        int64_t e_scalar = else_scalar ? if_scalar_i64(else_v) : 0;
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int64_t tv = then_scalar ? t_scalar : if_vec_i64(then_v, i);
            int64_t ev = else_scalar ? e_scalar : if_vec_i64(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_I32) {
        int32_t t_scalar = then_scalar ? (int32_t)if_scalar_i64(then_v) : 0;
        int32_t e_scalar = else_scalar ? (int32_t)if_scalar_i64(else_v) : 0;
        int32_t* dst = (int32_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int32_t tv = then_scalar ? t_scalar : (int32_t)if_vec_i64(then_v, i);
            int32_t ev = else_scalar ? e_scalar : (int32_t)if_vec_i64(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_STR) {
        if (!then_scalar && !else_scalar &&
            then_v->type == RAY_STR && else_v->type == RAY_STR &&
            len <= then_v->len && len <= else_v->len &&
            !(then_v->attrs & RAY_ATTR_HAS_NULLS) &&
            !(else_v->attrs & RAY_ATTR_HAS_NULLS)) {
            ray_t* then_pool = str_vec_pool_obj(then_v);
            ray_t* else_pool = str_vec_pool_obj(else_v);
            if (then_pool == else_pool || !then_pool || !else_pool) {
                ray_t* out_pool = then_pool ? then_pool : else_pool;
                if (out_pool && !RAY_IS_ERR(out_pool)) {
                    ray_retain(out_pool);
                    result->str_pool = out_pool;
                }
                const ray_str_t* t_desc = NULL;
                const ray_str_t* e_desc = NULL;
                const char* unused_pool = NULL;
                str_resolve(then_v, &t_desc, &unused_pool);
                str_resolve(else_v, &e_desc, &unused_pool);
                ray_str_t* dst = (ray_str_t*)ray_data(result);
                for (int64_t i = 0; i < len; i++)
                    dst[i] = cond_p[i] ? t_desc[i] : e_desc[i];
                ray_release(cond_v); ray_release(then_v); ray_release(else_v);
                return result;
            }
        }

        /* RAY_STR: resolve each side to string data and ray_str_vec_append.
         * Scalars may be -RAY_STR or RAY_SYM atoms. */
        result->len = 0; /* ray_str_vec_append manages len */
        for (int64_t i = 0; i < len; i++) {
            const char* sp;
            size_t sl;
            if (cond_p[i]) {
                if (then_scalar) {
                    if (then_v->type == -RAY_STR) {
                        sp = ray_str_ptr(then_v);
                        sl = ray_str_len(then_v);
                    } else if (then_v->type == RAY_STR) {
                        sp = ray_str_vec_get(then_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(then_v->type) || then_v->type == -RAY_SYM) {
                        ray_t* s = sym_scalar_str(then_v);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (then_v->type == RAY_STR) {
                    sp = ray_str_vec_get(then_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column: cell-data, resolve through its domain */
                    ray_t* sa = ray_sym_vec_cell(then_v, i);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            } else {
                if (else_scalar) {
                    if (else_v->type == -RAY_STR) {
                        sp = ray_str_ptr(else_v);
                        sl = ray_str_len(else_v);
                    } else if (else_v->type == RAY_STR) {
                        sp = ray_str_vec_get(else_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(else_v->type) || else_v->type == -RAY_SYM) {
                        ray_t* s = sym_scalar_str(else_v);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (else_v->type == RAY_STR) {
                    sp = ray_str_vec_get(else_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column: cell-data, resolve through its domain */
                    ray_t* sa = ray_sym_vec_cell(else_v, i);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            }
            result = ray_str_vec_append(result, sp, sl);
            if (RAY_IS_ERR(result)) break;
        }
    } else if (out_type == RAY_SYM) {
        /* SYM columns may have narrow widths (W8/W16/W32) — use ray_read_sym.
         * Scalars may be string atoms that need interning. Output is always W64.
         * The result is a fresh runtime-domain vec that can mix cells of
         * TWO source columns, so every cell id is re-expressed in the
         * runtime domain (raw-copy fast path while domains == runtime). */
        int64_t t_scalar = 0, e_scalar = 0;
        if (then_scalar) {
            if (then_v->type == -RAY_STR) {
                t_scalar = ray_sym_intern(ray_str_ptr(then_v), ray_str_len(then_v));
            } else {
                t_scalar = sym_scalar_runtime_id(then_v);
            }
        }
        if (else_scalar) {
            if (else_v->type == -RAY_STR) {
                e_scalar = ray_sym_intern(ray_str_ptr(else_v), ray_str_len(else_v));
            } else {
                e_scalar = sym_scalar_runtime_id(else_v);
            }
        }
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int64_t tv = then_scalar ? t_scalar : if_sym_cell_value(then_v, i);
            int64_t ev = else_scalar ? e_scalar : if_sym_cell_value(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_BOOL || out_type == RAY_U8) {
        uint8_t t_scalar = then_scalar ? (uint8_t)if_scalar_i64(then_v) : 0;
        uint8_t e_scalar = else_scalar ? (uint8_t)if_scalar_i64(else_v) : 0;
        uint8_t* dst = (uint8_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            uint8_t tv = then_scalar ? t_scalar : (uint8_t)if_vec_i64(then_v, i);
            uint8_t ev = else_scalar ? e_scalar : (uint8_t)if_vec_i64(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_TIMESTAMP || out_type == RAY_TIME || out_type == RAY_DATE) {
        /* TIMESTAMP is 8B like I64; DATE and TIME are 4B like I32 */
        if (out_type == RAY_TIMESTAMP) {
            int64_t t_scalar2 = then_scalar ? if_scalar_i64(then_v) : 0;
            int64_t e_scalar2 = else_scalar ? if_scalar_i64(else_v) : 0;
            int64_t* dst = (int64_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++) {
                int64_t tv = then_scalar ? t_scalar2 : if_vec_i64(then_v, i);
                int64_t ev = else_scalar ? e_scalar2 : if_vec_i64(else_v, i);
                dst[i] = cond_p[i] ? tv : ev;
            }
        } else {
            int32_t t_scalar2 = then_scalar ? (int32_t)if_scalar_i64(then_v) : 0;
            int32_t e_scalar2 = else_scalar ? (int32_t)if_scalar_i64(else_v) : 0;
            int32_t* dst = (int32_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++) {
                int32_t tv = then_scalar ? t_scalar2 : (int32_t)if_vec_i64(then_v, i);
                int32_t ev = else_scalar ? e_scalar2 : (int32_t)if_vec_i64(else_v, i);
                dst[i] = cond_p[i] ? tv : ev;
            }
        }
    } else if (out_type == RAY_I16) {
        int16_t t_scalar = then_scalar ? (int16_t)if_scalar_i64(then_v) : 0;
        int16_t e_scalar = else_scalar ? (int16_t)if_scalar_i64(else_v) : 0;
        int16_t* dst = (int16_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int16_t tv = then_scalar ? t_scalar : (int16_t)if_vec_i64(then_v, i);
            int16_t ev = else_scalar ? e_scalar : (int16_t)if_vec_i64(else_v, i);
            dst[i] = cond_p[i] ? tv : ev;
        }
    }

    ray_release(cond_v); ray_release(then_v); ray_release(else_v);
    return result;
}

ray_t* exec_if(ray_graph_t* g, ray_op_t* op) {
    ray_t* cond_v = exec_node(g, op_child(g, op, 0));
    if (!cond_v || RAY_IS_ERR(cond_v)) return cond_v;

    ray_t* selected = exec_if_selected(g, op, cond_v);
    if (selected) {
        ray_release(cond_v);
        return selected;
    }

    ray_release(cond_v);
    return exec_if_eager(g, op);
}

/* Fold the null-mask words [nullw, nullw+null_words) into a running hash.
 * Mirrors group.c's ght_hash_null_words (file-local there, so pivot.c
 * carries its own copy): an all-zero word contributes nothing, so the
 * single-word (<=64-key) case is byte-identical to the historical
 * `if (idx_nmask) combine` this replaces. */
static inline uint64_t pivot_hash_null_words(uint64_t h, const int64_t* nullw,
                                             uint32_t null_words) {
    for (uint32_t w = 0; w < null_words; w++)
        if (nullw[w]) h = ray_hash_combine(h, ray_hash_i64(nullw[w]));
    return h;
}

/* ============================================================================
 * exec_pivot — single-pass hash-aggregated pivot table
 *
 * Groups by (index_cols, pivot_col), aggregates value_col, then unstacks
 * pivot values into separate output columns.
 * ============================================================================ */

/* Segment count of a parted table = length of any parted column's segs array
 * (mirrors build_segment_table exec.c). */
static int64_t pivot_parted_seg_count(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && RAY_IS_PARTED(col->type)) return col->len;
    }
    return 0;
}

static bool pivot_table_has_parted_col(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON))
            return true;
    }
    return false;
}

ray_t* exec_pivot(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    /* Parted-input guard: exec_pivot fetches columns via ray_table_get_col and
     * indexes them by row up to ray_table_nrows.  A raw RAY_PARTED/MAPCOMMON
     * column (whose data is a tiny segs array, not nrows elements) would be
     * indexed out of bounds → OOB read / SIGSEGV.  Flatten (concat all
     * segments) and recompute on the flat table, mirroring exec_window /
     * join_with_parted_guard.  A flat table has no parted column, so the
     * recursive call falls straight through this guard (one level). */
    if (pivot_table_has_parted_col(tbl)) {
        int64_t nseg = pivot_parted_seg_count(tbl);
        ray_t* flat = NULL;
        for (int64_t s = 0; s < nseg; s++) {
            ray_t* seg = build_segment_table(tbl, (int32_t)s);
            if (!seg || RAY_IS_ERR(seg)) { if (flat) ray_release(flat); return seg ? seg : ray_error("oom", NULL); }
            if (!flat) flat = seg;
            else {
                ray_t* m = ray_result_merge(flat, seg);
                ray_release(flat); ray_release(seg);
                if (!m || RAY_IS_ERR(m)) return m ? m : ray_error("oom", NULL);
                flat = m;
            }
        }
        ray_t* r = exec_pivot(g, op, flat);
        ray_release(flat);
        return r;
    }

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint32_t n_idx  = ext->pivot.n_index;
    uint16_t agg_op = ext->pivot.agg_op;
    int64_t nrows   = ray_table_nrows(tbl);

    /* n_idx is USER-DATA-sized (the index-arg vector's length, tblop.c's
     * pivot_fn_impl), not AST-bounded — task 6 lifts both this function's
     * own former n_keys > 8 gate and tblop.c's caller-side check (the ght
     * layout below spills to a heap block for n_keys > 8, same mechanism
     * group.c's exec_group_run already relies on), so a caller can drive
     * n_idx into the millions.  A stack VLA at that size is a stack-clash
     * SIGSEGV waiting to happen; heap-carve instead.  One consolidated
     * block holds all six per-index/per-key arrays below (idx_vecs,
     * key_data, key_vecs — pointer-sized, placed first so every array
     * after the first stays 8-byte aligned — then idx_wide, key_types,
     * key_attrs, the byte-sized ones); freed on every exit from here to
     * pivot_cleanup below.  n_idx_cap floors n_idx at 1 (an index-less
     * pivot is legal — grouping by the pivot column alone), n_keys = n_idx
     * + 1 is already >= 1. */
    uint32_t n_idx_cap = n_idx ? n_idx : 1;
    uint32_t n_keys = n_idx + 1;
    size_t key_ptr_bytes  = (size_t)n_idx_cap * sizeof(ray_t*)   /* idx_vecs */
                          + (size_t)n_keys   * sizeof(void*)     /* key_data */
                          + (size_t)n_keys   * sizeof(ray_t*);   /* key_vecs */
    size_t key_byte_bytes = (size_t)n_idx_cap * sizeof(bool)     /* idx_wide */
                          + (size_t)n_keys   * sizeof(int8_t)    /* key_types */
                          + (size_t)n_keys   * sizeof(uint8_t);  /* key_attrs */
    ray_t* key_hdr = NULL;
    uint8_t* key_base = (uint8_t*)scratch_alloc(&key_hdr, key_ptr_bytes + key_byte_bytes);
    if (!key_base) return ray_error("oom", NULL);
    ray_t**  idx_vecs  = (ray_t**)key_base;
    void**   key_data  = (void**)(idx_vecs + n_idx_cap);
    ray_t**  key_vecs  = (ray_t**)(key_data + n_keys);
    bool*    idx_wide  = (bool*)(key_vecs + n_keys);
    int8_t*  key_types = (int8_t*)(idx_wide + n_idx_cap);
    uint8_t* key_attrs = (uint8_t*)(key_types + n_keys);
    memset(idx_wide, 0, (size_t)n_idx_cap * sizeof(bool));

    for (uint32_t i = 0; i < n_idx; i++) {
        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[i]);
        idx_vecs[i] = (ie && ie->base.opcode == OP_SCAN)
                     ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!idx_vecs[i]) { scratch_free(key_hdr); return ray_error("domain", "pivot: index column not found"); }
    }

    ray_op_ext_t* pe = find_ext(g, ext->pivot.pivot_col);
    ray_t* pcol = (pe && pe->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, pe->sym) : NULL;
    if (!pcol) { scratch_free(key_hdr); return ray_error("domain", "pivot: pivot column not found"); }

    ray_op_ext_t* ve = find_ext(g, ext->pivot.value_col);
    ray_t* vcol = (ve && ve->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, ve->sym) : NULL;
    if (!vcol) { scratch_free(key_hdr); return ray_error("domain", "pivot: value column not found"); }

    if (nrows == 0) { scratch_free(key_hdr); return ray_table_new(0); }

    /* Combined keys: index_cols + pivot_col.  No count cap — the ght
     * layout below (ght_compute_layout) spills to an owned heap block
     * whenever n_keys exceeds its GHT_INLINE (8) inline capacity; its own
     * key-stride budget (uint16 byte offsets) is the only remaining limit,
     * unreachable at any index-column count a real table could carry. */
    for (uint32_t k = 0; k < n_idx; k++)
        idx_wide[k] = (idx_vecs[k]->type == RAY_GUID);
    bool pvt_wide = (pcol->type == RAY_GUID);

    for (uint32_t k = 0; k < n_idx; k++) {
        key_data[k]  = ray_data(idx_vecs[k]);
        key_types[k] = idx_vecs[k]->type;
        key_attrs[k] = idx_vecs[k]->attrs;
        key_vecs[k]  = idx_vecs[k];
    }
    key_data[n_idx]  = ray_data(pcol);
    key_types[n_idx] = pcol->type;
    key_attrs[n_idx] = pcol->attrs;
    key_vecs[n_idx]  = pcol;

    /* Single agg input: value column */
    ray_t* agg_vecs[1] = { vcol };
    uint16_t agg_ops[1] = { agg_op };

    /* Compute need_flags for the agg op */
    uint8_t need_flags = GHT_NEED_SUM; /* always need sum (used for FIRST/LAST too) */
    if (agg_op == OP_MIN) need_flags |= GHT_NEED_MIN;
    if (agg_op == OP_MAX) need_flags |= GHT_NEED_MAX;

    /* n_keys/n_aggs are no longer capped: ght_compute_layout spills to an
     * owned heap block (ly.spill_hdr) whenever n_keys exceeds GHT_INLINE
     * (8) — the same path group.c's exec_group_run already exercises.
     * ght_layout_free(&ly) is therefore no longer always a no-op past this
     * point; every exit below (early return AND pivot_cleanup) must call
     * it exactly once so a spilled layout's heap block is never leaked. */
    ght_layout_t ly;
    if (!ght_compute_layout(&ly, n_keys, 1, agg_vecs, NULL,
                            need_flags, agg_ops, key_types)) {
        scratch_free(key_hdr);
        return ray_error("limit", "pivot: key stride budget exceeded");
    }

    /* Hash-aggregate all rows via the shared radix pipeline — parallel
     * across thread-pool workers for n_scan ≥ RAY_PARALLEL_THRESHOLD,
     * sequential single-HT for smaller inputs. */
    ray_progress_update("pivot", "hash-aggregate", 0, (uint64_t)nrows);
    pivot_ingest_t pg;
    if (!pivot_ingest_run(&pg, &ly, key_data, key_types, key_attrs,
                          key_vecs, agg_vecs, nrows)) {
        pivot_ingest_free(&pg);
        ght_layout_free(&ly);
        scratch_free(key_hdr);
        return ray_error("oom", NULL);
    }
    ray_progress_update("pivot", "dedupe", 0, (uint64_t)pg.total_grps);
    if (ray_interrupted()) {
        pivot_ingest_free(&pg);
        ght_layout_free(&ly);
        scratch_free(key_hdr);
        return ray_error("cancel", "interrupted");
    }
    uint32_t grp_count = pg.total_grps;
    if (grp_count == 0) {
        pivot_ingest_free(&pg);
        ght_layout_free(&ly);
        scratch_free(key_hdr);
        return ray_table_new(0);
    }

    /* Pass 2: Collect distinct pivot values and distinct index keys.
     * Each group row layout: [hash:8][key0:8]...[keyN-1:8][null_word0:8]
     * ...[null_wordM-1:8][accum...] where the keys region holds n_idx
     * index keys + 1 pivot key, followed by the ceil(n_keys/64) key-null
     * mask words written by group_rows_range (ly.null_words; Task 3). */

    /* SQL PIVOT treats a null pivot key as "no column" — drop those groups.
     * The pivot key is the LAST of the n_keys keys, at bit position n_idx —
     * word-indexed per the Task-3/4 convention (word k>>6, bit k&63; the
     * unsigned-shift idiom sidesteps UB at bit 63) since n_idx can now
     * exceed 63 (pivot's own admission gate lifts along with the ght-path
     * key gate this migration also lifts — the two are no longer
     * independent facts once BOTH caps are gone). */
    const uint32_t null_words     = ly.null_words;
    const uint32_t pvt_null_word  = n_idx >> 6;
    const int64_t  pvt_null_bit   = (int64_t)((uint64_t)1 << (n_idx & 63));

    /* Collect distinct pivot values */
    uint32_t pv_cap = 64, pv_count = 0;
    ray_t* pv_hdr = NULL;
    int64_t* pv_vals = (int64_t*)scratch_alloc(&pv_hdr, pv_cap * sizeof(int64_t));
    if (!pv_vals) { pivot_ingest_free(&pg); ght_layout_free(&ly); scratch_free(key_hdr); return ray_error("oom", NULL); }

    const char* pvt_base = pvt_wide ? (const char*)key_data[n_idx] : NULL;
    for (uint32_t _p = 0; _p < pg.n_parts; _p++) {
        group_ht_t* ph = &pg.part_hts[_p];
        uint32_t pcount = ph->grp_count;
        for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
            const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
            const int64_t* rkeys = (const int64_t*)(row + 8);
            if (rkeys[n_keys + pvt_null_word] & pvt_null_bit) continue;
            int64_t pval = rkeys[n_idx];
            bool found = false;
            for (uint32_t p = 0; p < pv_count; p++) {
                if (pvt_wide) {
                    if (memcmp(pvt_base + (size_t)pv_vals[p] * 16,
                               pvt_base + (size_t)pval * 16, 16) == 0) { found = true; break; }
                } else {
                    if (pv_vals[p] == pval) { found = true; break; }
                }
            }
            if (!found) {
                if (pv_count >= pv_cap) {
                    uint32_t new_cap = pv_cap * 2;
                    int64_t* new_pv = (int64_t*)scratch_realloc(&pv_hdr,
                        pv_cap * sizeof(int64_t), new_cap * sizeof(int64_t));
                    if (!new_pv) { pivot_ingest_free(&pg); ght_layout_free(&ly); scratch_free(key_hdr); return ray_error("oom", NULL); }
                    pv_vals = new_pv;
                    pv_cap = new_cap;
                }
                pv_vals[pv_count++] = pval;
            }
        }
    }

    /* Collect distinct index keys.
     * Flat append-only entry array + secondary open-addressed HT keyed by
     * the hash of (idx_keys + idx_null_words). The HT makes phase2 dedupe
     * O(grp_count) instead of the previous O(grp_count * ix_count)
     * linear scan which hung on large pivots.
     * Entry layout: [hash:8 | idx_keys:8*n_idx | idx_null_words:8*null_words].
     * Widened from a single trailing null word (Task-3 carryover finding):
     * a single word only ever compared/hashed/stored bits 0..63, silently
     * conflating two index tuples that agree on keys/nulls < 64 but differ
     * in a null bit >= 64 (index key 64+) into one dedupe entry — the
     * 65-index cell caught the clobber. */
    uint32_t ix_cap = 256, ix_count = 0;
    ray_t* ix_hdr = NULL;
    size_t ix_entry = 8 + (size_t)n_idx * 8 + (size_t)null_words * 8;
    char* ix_rows = (char*)scratch_alloc(&ix_hdr, ix_cap * ix_entry);
    if (!ix_rows) { scratch_free(pv_hdr); pivot_ingest_free(&pg); ght_layout_free(&ly); scratch_free(key_hdr); return ray_error("oom", NULL); }

    /* Secondary HT: hash slot -> ix_row index; empty = UINT32_MAX. */
    uint32_t ix_ht_cap = 256;
    while (ix_ht_cap < (uint32_t)grp_count * 2 && ix_ht_cap < (1u << 30)) ix_ht_cap <<= 1;
    ray_t* ix_ht_hdr = NULL;
    uint32_t* ix_ht = (uint32_t*)scratch_alloc(&ix_ht_hdr, ix_ht_cap * sizeof(uint32_t));
    if (!ix_ht) {
        scratch_free(ix_hdr); scratch_free(pv_hdr); pivot_ingest_free(&pg); ght_layout_free(&ly);
        scratch_free(key_hdr);
        return ray_error("oom", NULL);
    }
    memset(ix_ht, 0xFF, ix_ht_cap * sizeof(uint32_t));
    uint32_t ix_ht_mask = ix_ht_cap - 1;

    /* Map: group_id -> (ix_row, pv_idx) for result cell placement */
    ray_t* map_hdr = NULL;
    uint32_t* grp_ix  = (uint32_t*)scratch_alloc(&map_hdr, grp_count * 2 * sizeof(uint32_t));
    if (!grp_ix) {
        scratch_free(ix_ht_hdr); scratch_free(ix_hdr); scratch_free(pv_hdr);
        pivot_ingest_free(&pg); ght_layout_free(&ly);
        scratch_free(key_hdr);
        return ray_error("oom", NULL);
    }
    uint32_t* grp_pv = grp_ix + grp_count;

    for (uint32_t _p = 0; _p < pg.n_parts; _p++) {
        group_ht_t* ph = &pg.part_hts[_p];
        uint32_t pcount = ph->grp_count;
        uint32_t gi_base = pg.part_offsets[_p];
        /* Progress tick at each partition boundary — time-gated so
         * 256 small partitions do not spam the callback. */
        ray_progress_update(NULL, NULL, gi_base, (uint64_t)grp_count);
        for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
            uint32_t gi = gi_base + gi_local;
            const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
            const int64_t* keys = (const int64_t*)(row + 8);
            if (keys[n_keys + pvt_null_word] & pvt_null_bit) {
                grp_ix[gi] = UINT32_MAX;
                grp_pv[gi] = UINT32_MAX;
                continue;
            }
        /* Index-key null words: keys[n_keys .. n_keys+null_words) as-is.
         * The pivot key's own null bit (position n_idx, word pvt_null_word)
         * is guaranteed 0 here — the check just above already `continue`d
         * on it being set — so these words already carry exactly the
         * index-key null flags with no separate masking needed. */
        const int64_t* idx_nwords = keys + n_keys;

        /* Hash index keys only (exclude pivot key) + null words.
         * Wide keys (GUID) resolve actual bytes via key_data[k]. */
        uint64_t ih = 0;
        for (uint32_t k = 0; k < n_idx; k++) {
            uint64_t kh;
            if (idx_wide[k]) {
                const char* base = (const char*)key_data[k];
                kh = ray_hash_bytes(base + (size_t)keys[k] * 16, 16);
            } else if (key_types[k] == RAY_F64) {
                double kd;                          /* F64 key bits live in the i64 slot; */
                memcpy(&kd, &keys[k], sizeof kd);   /* memcpy bit-cast is aliasing-safe   */
                kh = ray_hash_f64(kd);
            } else {
                kh = ray_hash_i64(keys[k]);
            }
            ih = (k == 0) ? kh : ray_hash_combine(ih, kh);
        }
        ih = pivot_hash_null_words(ih, idx_nwords, null_words);

        /* Open-addressed HT probe. On match, reuse; else insert. */
        uint32_t ix_row = UINT32_MAX;
        uint32_t slot = (uint32_t)(ih & ix_ht_mask);
        for (;;) {
            uint32_t ent = ix_ht[slot];
            if (ent == UINT32_MAX) break; /* empty → insert below */
            const char* ix_entry_p = ix_rows + (size_t)ent * ix_entry;
            if (*(const uint64_t*)ix_entry_p == ih) {
                const int64_t* ekeys = (const int64_t*)(ix_entry_p + 8);
                bool eq = true;
                for (uint32_t k = 0; k < n_idx && eq; k++) {
                    if (idx_wide[k]) {
                        const char* base = (const char*)key_data[k];
                        eq = (memcmp(base + (size_t)ekeys[k] * 16,
                                      base + (size_t)keys[k] * 16, 16) == 0);
                    } else {
                        eq = (ekeys[k] == keys[k]);
                    }
                }
                if (eq) {
                    const int64_t* ent_nwords = (const int64_t*)(ix_entry_p + 8 + (size_t)n_idx * 8);
                    eq = (memcmp(ent_nwords, idx_nwords, (size_t)null_words * 8) == 0);
                }
                if (eq) { ix_row = ent; break; }
            }
            slot = (slot + 1) & ix_ht_mask;
        }
        if (ix_row == UINT32_MAX) {
            if (ix_count >= ix_cap) {
                uint32_t new_cap = ix_cap * 2;
                char* new_rows = (char*)scratch_realloc(&ix_hdr,
                    ix_cap * ix_entry, new_cap * ix_entry);
                if (!new_rows) {
                    scratch_free(map_hdr); scratch_free(ix_ht_hdr);
                    scratch_free(pv_hdr); pivot_ingest_free(&pg); ght_layout_free(&ly);
                    scratch_free(key_hdr);
                    return ray_error("oom", NULL);
                }
                ix_rows = new_rows;
                ix_cap = new_cap;
            }
            ix_row = ix_count++;
            char* dst = ix_rows + (size_t)ix_row * ix_entry;
            *(uint64_t*)dst = ih;
            memcpy(dst + 8, keys, (size_t)n_idx * 8);
            memcpy(dst + 8 + (size_t)n_idx * 8, idx_nwords, (size_t)null_words * 8);
            ix_ht[slot] = ix_row;
        }

        /* Find pivot column index. For wide pivot keys both slot values
         * are source row indices — resolve to actual bytes for compare,
         * otherwise duplicate GUID pivot values map to the wrong column. */
        int64_t pval = keys[n_idx];
        uint32_t pv_idx = UINT32_MAX;
        for (uint32_t p = 0; p < pv_count; p++) {
            if (pvt_wide) {
                if (memcmp(pvt_base + (size_t)pv_vals[p] * 16,
                           pvt_base + (size_t)pval * 16, 16) == 0) { pv_idx = p; break; }
            } else {
                if (pv_vals[p] == pval) { pv_idx = p; break; }
            }
        }

            grp_ix[gi] = ix_row;
            grp_pv[gi] = pv_idx;
        }
    }

    /* Pass 3: Build output table */
    ray_progress_update("pivot", "scatter", 0, (uint64_t)pv_count);
    bool val_is_f64 = vcol->type == RAY_F64;
    int8_t out_agg_type;
    switch (agg_op) {
        case OP_AVG:   out_agg_type = RAY_F64; break;
        case OP_COUNT: out_agg_type = RAY_I64; break;
        case OP_SUM:   out_agg_type = val_is_f64 ? RAY_F64 : RAY_I64; break;
        default:       out_agg_type = vcol->type; break;
    }

    int64_t out_ncols = (int64_t)n_idx + (int64_t)pv_count;
    ray_t* result = ray_table_new(out_ncols);
    if (!result || RAY_IS_ERR(result)) goto pivot_cleanup;

    /* Index columns */
    for (uint32_t k = 0; k < n_idx; k++) {
        ray_t* new_col = col_vec_new(idx_vecs[k], (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;
        uint8_t esz = col_esz(idx_vecs[k]);
        int8_t kt = idx_vecs[k]->type;
        const char* src_base = idx_wide[k] ? (const char*)key_data[k] : NULL;
        /* Word-indexed per the Task-3/4 convention: k is fixed in this outer
         * scope, so hoist its word/bit out of the per-row loop below (as
         * the seq emit loop in group.c does) rather than recomputing it
         * ix_count times. */
        const uint32_t null_kw  = k >> 6;
        const int64_t  null_kbit = (int64_t)((uint64_t)1 << (k & 63));
        for (uint32_t r = 0; r < ix_count; r++) {
            const char* ix_entry_p = ix_rows + r * ix_entry;
            int64_t kv = ((const int64_t*)(ix_entry_p + 8))[k];
            const int64_t* ent_nwords = (const int64_t*)(ix_entry_p + 8 + (size_t)n_idx * 8);
            if (ent_nwords[null_kw] & null_kbit) {
                ray_vec_set_null(new_col, (int64_t)r, true);
                /* Fill the correct-width sentinel. */
                switch (kt) {
                    case RAY_F64:
                        ((double*)ray_data(new_col))[r] = NULL_F64; break;
                    case RAY_I64: case RAY_TIMESTAMP:
                        ((int64_t*)ray_data(new_col))[r] = NULL_I64; break;
                    case RAY_I32: case RAY_DATE: case RAY_TIME:
                        ((int32_t*)ray_data(new_col))[r] = NULL_I32; break;
                    case RAY_I16:
                        ((int16_t*)ray_data(new_col))[r] = NULL_I16; break;
                    default: break;
                }
                continue;
            }
            if (idx_wide[k]) {
                /* kv is a source row index; copy the 16 raw bytes. */
                memcpy((char*)ray_data(new_col) + (size_t)r * esz,
                       src_base + (size_t)kv * 16, 16);
            } else if (kt == RAY_F64) {
                memcpy((char*)ray_data(new_col) + (size_t)r * esz, &kv, 8);
            } else {
                write_col_i64(ray_data(new_col), (int64_t)r, kv, kt, new_col->attrs);
            }
        }
        if (idx_vecs[k]->type == RAY_STR)
            col_propagate_str_pool(new_col, idx_vecs[k]);
        /* Output-vec rule (Task-2 review): a SYM index column raw-copies
         * cell ids from its source, so it must resolve over the source's
         * dictionary.  No-op while every domain is the runtime singleton. */
        if (new_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, idx_vecs[k]);

        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[k]);
        result = ray_table_add_col(result, ie->sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }

    /* Value columns — one per distinct pivot value */
    {
    int8_t s = ly.agg_val_slot[0]; /* single agg input -> slot 0 */
    for (uint32_t p = 0; p < pv_count; p++) {
        ray_t* new_col = (out_agg_type == vcol->type)
                        ? col_vec_new(vcol, (int64_t)ix_count)
                        : ray_vec_new(out_agg_type, (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;

        /* Initialize missing cells with the type-correct NULL sentinel —
         * a pivot cell with no source row is "no data", which must stay
         * distinguishable from a real 0 (review 2.10).  Present cells get
         * overwritten in the scatter loop below; whatever remains is null.
         * par_finalize_nulls (after scatter) flips HAS_NULLS if any
         * sentinel survives.  Non-sentinel types (SYM/STR/BOOL/U8/GUID)
         * fall back to zero-fill: SYM id 0 is the SYM null already; the
         * others carry no null sentinel. */
        switch (new_col->type) {
            case RAY_F64: {
                double* d = (double*)ray_data(new_col);
                for (int64_t r = 0; r < (int64_t)ix_count; r++) d[r] = NULL_F64;
                break;
            }
            case RAY_F32: {
                float* d = (float*)ray_data(new_col);
                for (int64_t r = 0; r < (int64_t)ix_count; r++) d[r] = NULL_F32;
                break;
            }
            case RAY_I64: case RAY_TIMESTAMP: {
                int64_t* d = (int64_t*)ray_data(new_col);
                for (int64_t r = 0; r < (int64_t)ix_count; r++) d[r] = NULL_I64;
                break;
            }
            case RAY_I32: case RAY_DATE: case RAY_TIME: {
                int32_t* d = (int32_t*)ray_data(new_col);
                for (int64_t r = 0; r < (int64_t)ix_count; r++) d[r] = NULL_I32;
                break;
            }
            case RAY_I16: {
                int16_t* d = (int16_t*)ray_data(new_col);
                for (int64_t r = 0; r < (int64_t)ix_count; r++) d[r] = NULL_I16;
                break;
            }
            default:
                memset(ray_data(new_col), 0, (size_t)ix_count * (size_t)col_esz(new_col));
                break;
        }

        for (uint32_t _pp = 0; _pp < pg.n_parts; _pp++) {
            group_ht_t* ph = &pg.part_hts[_pp];
            uint32_t pcount = ph->grp_count;
            uint32_t gi_base = pg.part_offsets[_pp];
            for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
                uint32_t gi = gi_base + gi_local;
                if (grp_pv[gi] != p) continue;
                uint32_t r = grp_ix[gi];
                const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
                int64_t cnt = *(const int64_t*)(const void*)row;
                /* nn = per-slot non-null count (nullable value column) or the
                 * group row count (null-free — byte-identical to before). */
                int64_t nn = ly.off_nn
                    ? ((const int64_t*)(const void*)(row + ly.off_nn))[s] : cnt;

            if (out_agg_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    case OP_AVG:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s) / nn
                                       : (double)ROW_RD_I64(row, ly.off_sum, s) / nn;
                        break;
                    case OP_MIN:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_min, s)
                                       : (double)ROW_RD_I64(row, ly.off_min, s);
                        break;
                    case OP_MAX:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_max, s)
                                       : (double)ROW_RD_I64(row, ly.off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    default: v = 0.0; break;
                }
                /* Single-null float model: canonicalize non-finite (avg
                 * division, sum overflow) to NULL_F64; HAS_NULLS set by the
                 * scan below before new_col is added to the result. */
                ((double*)ray_data(new_col))[r] = ray_f64_fin(v);
            } else {
                int64_t v;
                int64_t int_null = agg_int_null_sentinel_for(out_agg_type);
                switch (agg_op) {
                    case OP_SUM:   v = ROW_RD_I64(row, ly.off_sum, s); break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = ROW_RD_I64(row, ly.off_min, s); break;
                    case OP_MAX:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = ROW_RD_I64(row, ly.off_max, s); break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, (int64_t)r, true); break; }
                        v = ROW_RD_I64(row, ly.off_sum, s); break;
                    default:       v = 0; break;
                }
                    write_col_i64(ray_data(new_col), (int64_t)r, v, out_agg_type, new_col->attrs);
                }
            }
        }

        /* Output-vec rule (sym-domain Phase 2): a SYM value column
         * (MIN/MAX/FIRST/LAST over a SYM input) carries raw cell ids
         * accumulated from `vcol` — it must resolve over vcol's
         * dictionary.  The memset-0 fill for missing cells is the SYM
         * null (id 0) in any domain.  No-op while every domain is the
         * runtime singleton. */
        if (new_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, vcol);

        /* Column name from pivot value — match pivot_val_to_sym semantics */
        int64_t pval = pv_vals[p];
        int64_t col_sym;
        if (pcol->type == RAY_SYM) {
            /* pivot value (cell-data) becomes a column NAME — names live
             * in the runtime table, so re-express the id there. */
            col_sym = sym_id_runtime(pcol, pval);
        } else if (pvt_wide) {
            /* GUID: format 16 bytes as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
             * pval is a source row index into pvt_base. */
            static const char hex[] = "0123456789abcdef";
            static const int groups[] = {4, 2, 2, 2, 6};
            char buf[37];
            const uint8_t* bytes = (const uint8_t*)pvt_base + (size_t)pval * 16;
            int pos = 0, bpos = 0;
            for (int g = 0; g < 5; g++) {
                if (g > 0) buf[bpos++] = '-';
                for (int j = 0; j < groups[g]; j++) {
                    buf[bpos++] = hex[bytes[pos] >> 4];
                    buf[bpos++] = hex[bytes[pos] & 0x0F];
                    pos++;
                }
            }
            col_sym = ray_sym_intern(buf, (size_t)bpos);
        } else {
            char buf[128];
            int len = 0;
            int8_t pt = key_types[n_idx];
            if (pt == RAY_F64) {
                double fv;
                memcpy(&fv, &pval, 8);
                fv = clear_neg_zero(fv);
                len = snprintf(buf, sizeof(buf), "%g", fv);
            } else if (pt == RAY_BOOL) {
                len = snprintf(buf, sizeof(buf), "%s", pval ? "true" : "false");
            } else if (pt == RAY_I64 || pt == RAY_I32 || pt == RAY_I16 ||
                       pt == RAY_DATE || pt == RAY_TIME || pt == RAY_TIMESTAMP) {
                len = snprintf(buf, sizeof(buf), "%ld", (long)pval);
            } else {
                len = snprintf(buf, sizeof(buf), "col%ld", (long)pval);
            }
            col_sym = ray_sym_intern(buf, (size_t)len);
        }

        /* Flip HAS_NULLS if any pivot cell carries the type-correct NULL
         * sentinel — either a missing cell (no source row) left as null by
         * the init above, or (F64) a value canonicalized to NULL_F64 by
         * ray_f64_fin (avg division, sum overflow).  par_finalize_nulls is
         * a no-op for non-sentinel types (SYM/STR/BOOL/U8/GUID). */
        par_finalize_nulls(new_col);
        result = ray_table_add_col(result, col_sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }
    }

pivot_cleanup:
    ght_layout_free(&ly);   /* no-op when inline (n_keys <= 8); frees the
                             * owned spill block otherwise (see above) */
    scratch_free(map_hdr);
    scratch_free(ix_ht_hdr);
    scratch_free(ix_hdr);
    scratch_free(pv_hdr);
    scratch_free(key_hdr); /* idx_vecs/idx_wide/key_data/key_types/key_attrs/
                             * key_vecs consolidated carve, alive since the
                             * top of this function (see key_hdr above) */
    pivot_ingest_free(&pg);
    return result;
}

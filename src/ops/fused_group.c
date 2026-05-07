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

#include "ops/fused_group.h"
#include "ops/fused_pred.h" /* fp_pred_t / fp_compile_pred / fp_eval_pred */
#include "lang/eval.h"      /* RAY_ATTR_NAME */
#include "core/pool.h"      /* ray_pool_get / ray_pool_dispatch */

#include <limits.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* Constructor — mirrors ray_group's trail layout (keys, agg_ops, agg_ins
 * embedded in trailing bytes), and stashes the predicate root in
 * base.inputs[0].  Reusing OP_GROUP's anonymous union variant means
 * exec_filtered_group reads ext->n_keys / ext->keys[] / ext->agg_ops /
 * ext->agg_ins exactly the way OP_GROUP does. */
ray_op_t* ray_filtered_group(ray_graph_t* g, ray_op_t* pred,
                             ray_op_t** keys, uint8_t n_keys,
                             uint16_t* agg_ops, ray_op_t** agg_ins,
                             uint8_t n_aggs)
{
    if (!g) return NULL;

    uint32_t pred_id = pred ? pred->id : 0;
    uint32_t key_ids[256];
    uint32_t agg_ids[256];
    for (uint8_t i = 0; i < n_keys; i++) key_ids[i] = keys[i]->id;
    for (uint8_t i = 0; i < n_aggs; i++) agg_ids[i] = agg_ins[i]->id;

    size_t keys_sz = (size_t)n_keys * sizeof(ray_op_t*);
    size_t ops_sz  = (size_t)n_aggs * sizeof(uint16_t);
    size_t ins_sz  = (size_t)n_aggs * sizeof(ray_op_t*);
    size_t ops_off = keys_sz;
    size_t ins_off = ops_off + ops_sz;
    /* Round ins_off up to pointer alignment */
    ins_off = (ins_off + sizeof(ray_op_t*) - 1) & ~(sizeof(ray_op_t*) - 1);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, ins_off + ins_sz);
    if (!ext) return NULL;

    ext->base.opcode = OP_FILTERED_GROUP;
    /* arity 1 when there's a real predicate, 0 for the const-true case
     * (used by no-WHERE group-by — the count1/multi exec path treats a
     * NULL inputs[0] as an empty fp_pred which evaluates to all-ones). */
    ext->base.arity = pred ? 1 : 0;
    ext->base.out_type = RAY_TABLE;
    ext->base.inputs[0] = pred ? &g->nodes[pred_id] : NULL;
    if (n_keys > 0 && keys[0])
        ext->base.est_rows = g->nodes[key_ids[0]].est_rows / 10;

    char* trail = EXT_TRAIL(ext);
    ext->keys = (ray_op_t**)trail;
    for (uint8_t i = 0; i < n_keys; i++)
        ext->keys[i] = &g->nodes[key_ids[i]];
    ext->agg_ops = (uint16_t*)(trail + ops_off);
    if (ops_sz > 0) memcpy(ext->agg_ops, agg_ops, ops_sz);
    ext->agg_ins = (ray_op_t**)(trail + ins_off);
    for (uint8_t i = 0; i < n_aggs; i++)
        ext->agg_ins[i] = &g->nodes[agg_ids[i]];
    ext->n_keys = n_keys;
    ext->n_aggs = n_aggs;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* Recognise a 2-character comparison operator: ==, !=, <=, >=.  Returns
 * the FP_* code or -1 on miss. */
static int fp_op_from_2char(const char* op, size_t len) {
    if (len != 2) return -1;
    if (op[1] == '=') {
        if (op[0] == '=') return 0;  /* FP_EQ */
        if (op[0] == '!') return 1;  /* FP_NE */
        if (op[0] == '<') return 3;  /* FP_LE */
        if (op[0] == '>') return 5;  /* FP_GE */
    }
    return -1;
}

/* Recognise a 1-character comparison operator: <, >. */
static int fp_op_from_1char(const char* op, size_t len) {
    if (len != 1) return -1;
    if (op[0] == '<') return 2;  /* FP_LT */
    if (op[0] == '>') return 4;  /* FP_GT */
    return -1;
}

/* Return 1 when an atom of `atom_type` (negative-typed RAY_*) is a
 * legal RHS for a comparison against a column of `col_type`.  The
 * fused per-row compare reads raw bit patterns from the column and
 * compares against an int64-decoded constant, so column ↔ atom must
 * agree on units.  In particular, mixing temporal units (DATE days
 * vs TIMESTAMP nanoseconds vs TIME microseconds) is rejected — let
 * the unfused engine handle the implicit conversion.  Atom types
 * here are the negative-typed (-RAY_*) atom encoding from values. */
static int fp_atom_col_compatible(int8_t atom_type, int8_t col_type) {
    switch (col_type) {
    case RAY_SYM:
        /* SYM compares against a symbol-id atom or a string literal
         * (string is intern-resolved to a sym id at compile time). */
        return atom_type == -RAY_SYM || atom_type == -RAY_STR;
    case RAY_DATE:
        return atom_type == -RAY_DATE;
    case RAY_TIME:
        return atom_type == -RAY_TIME;
    case RAY_TIMESTAMP:
        return atom_type == -RAY_TIMESTAMP;
    case RAY_BOOL:
    case RAY_U8:
    case RAY_I16:
    case RAY_I32:
    case RAY_I64:
        /* Any signed/unsigned integer literal; we still range-check
         * cval against the column width to fold out-of-range. */
        return atom_type == -RAY_BOOL || atom_type == -RAY_U8
            || atom_type == -RAY_I16  || atom_type == -RAY_I32
            || atom_type == -RAY_I64;
    default:
        return 0;
    }
}

/* Reject columns the fused per-row compare can't read safely.
 * Currently: any column carrying a non-empty nullmap (RAY_ATTR_HAS_NULLS).
 * The fused evaluator reads raw payload bytes — for nullable columns it
 * would compare the sentinel value rather than treating the slot as
 * null, producing a different result from the unfused null-aware
 * compare kernel.  Until fp_eval_cmp learns to skip nulls, gate fused
 * off here at compile so the planner falls back transparently. */
static int fp_col_supported(const ray_t* col) {
    if (!col) return 0;
    if (col->attrs & RAY_ATTR_HAS_NULLS) return 0;
    return 1;
}

/* Is `expr` a phase-3 simple comparison form (op col const)?  Validates
 * that the column exists in `tbl` and that ordering ops only target
 * non-SYM columns.  Returns the FP_* code on success, or -1 on miss. */
static int fp_check_simple_cmp(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return -1;
    if (ray_len(expr) != 3) return -1;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return -1;
    ray_t* op_sym = ray_sym_str(elems[0]->i64);
    if (!op_sym) return -1;
    size_t op_len = ray_str_len(op_sym);
    const char* op = ray_str_ptr(op_sym);
    int code = fp_op_from_2char(op, op_len);
    if (code < 0) code = fp_op_from_1char(op, op_len);
    if (code < 0) return -1;

    ray_t* lhs = elems[1];
    if (!lhs || lhs->type != -RAY_SYM || !(lhs->attrs & RAY_ATTR_NAME))
        return -1;
    ray_t* rhs = elems[2];
    if (!rhs || !ray_is_atom(rhs) || (rhs->attrs & RAY_ATTR_NAME))
        return -1;

    /* Resolve column type to gate ordering ops AND verify the column
     * is fused-supported (no nulls, supported type) AND the constant's
     * atom type is compatible with the column's storage class.  This
     * mirrors fp_compile_cmp exactly so the planner gate and executor
     * compile agree — divergence here means the executor returns
     * `nyi` on shapes the planner thought were ok. */
    if (tbl) {
        ray_t* col = ray_table_get_col(tbl, lhs->i64);
        if (!col) return -1;
        if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return -1;
        int8_t ct = col->type;
        int is_ord = (code >= 2);  /* LT/LE/GT/GE */
        if (is_ord && ct == RAY_SYM) return -1;
        /* F32/F64/STR not supported by phase-3 evaluator. */
        if (ct != RAY_SYM && ct != RAY_BOOL && ct != RAY_U8
            && ct != RAY_I16 && ct != RAY_I32 && ct != RAY_I64
            && ct != RAY_DATE && ct != RAY_TIME && ct != RAY_TIMESTAMP)
            return -1;
        if (!fp_col_supported(col)) return -1;
        if (!fp_atom_col_compatible(rhs->type, ct)) return -1;
    }
    return code;
}

/* Phase-3 supported shapes:
 *
 *   pred  = simple_cmp | (and simple_cmp simple_cmp …)
 *   simple_cmp = (op col const) where op ∈ {==, !=, <, <=, >, >=}
 *   ordering ops require col to be a numeric/temporal (non-SYM) column.
 *   AND fan-in is bounded by FP_PRED_MAX_CHILDREN (defined in fused_pred.h). */

int ray_fused_group_supported(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;

    /* Detect (and …). */
    ray_t* head_sym = ray_sym_str(elems[0]->i64);
    if (head_sym) {
        size_t hlen = ray_str_len(head_sym);
        const char* hstr = ray_str_ptr(head_sym);
        if (hlen == 3 && memcmp(hstr, "and", 3) == 0) {
            int64_t k = n - 1;
            if (k < 1 || k > FP_PRED_MAX_CHILDREN) return 0;
            for (int64_t i = 0; i < k; i++) {
                if (fp_check_simple_cmp(elems[i + 1], tbl) < 0) return 0;
            }
            return 1;
        }
    }
    /* Fall through: single simple cmp. */
    return fp_check_simple_cmp(expr, tbl) >= 0 ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-morsel predicate evaluator
 *
 * Phase 1 only handles a single comparison `(== col const)` / `(!= col const)`
 * against an SYM or numeric column.  The compiled state is built once at
 * exec entry (column resolution + constant decode) and reused for every
 * morsel.  fp_eval_cmp writes 0/1 into bits[0..n) for the corresponding
 * row range; OP_NE flips the result via XOR.
 * ──────────────────────────────────────────────────────────────────────── */

/* fp_op_t / fp_cmp_t / fp_pred_t / FP_PRED_MAX_CHILDREN are defined in
 * fused_pred.h so fused_topk.c (and any future fused-* operator) can
 * reuse the predicate evaluator without duplicating the types. */

/* Inner-loop generator: specialise on element type and operator so the
 * compiler can hoist the comparison and autovectorise the byte-store. */
#define FP_RUN(T, OP) do {                                                  \
    const T* d = (const T*)p->col_base + start;                              \
    T cv = (T)cval;                                                          \
    for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)(d[r] OP cv);         \
} while (0)

/* Fill bits[0..end-start) with 0/1 based on `cmp` over rows [start, end).
 * Per-(esz, op) specialisation; SYM supports only EQ/NE. */
void fp_eval_cmp(const fp_cmp_t* p, int64_t start, int64_t end,
                 uint8_t* bits)
{
    int64_t n = end - start;
    int64_t cval = p->cval;
    fp_op_t op = p->op;
    int8_t  ct = p->col_type;
    uint8_t esz = p->col_esz;

    /* Compile-time fold: out-of-range constant ⇒ all-true or all-false. */
    if (p->fold) {
        memset(bits, (p->fold == FP_FOLD_TRUE) ? 1 : 0, (size_t)n);
        return;
    }

    /* SYM low-card fold: const not in dict ⇒ EQ all-zero / NE all-one.
     * Ordering ops are rejected at compile for SYM, so unreachable here. */
    if (ct == RAY_SYM && !p->cval_in_dict) {
        memset(bits, (op == FP_NE) ? 1 : 0, (size_t)n);
        return;
    }

    if (esz == 1) {
        switch (op) {
        case FP_EQ: FP_RUN(uint8_t, ==); break;
        case FP_NE: FP_RUN(uint8_t, !=); break;
        case FP_LT: FP_RUN(uint8_t, < ); break;
        case FP_LE: FP_RUN(uint8_t, <=); break;
        case FP_GT: FP_RUN(uint8_t, > ); break;
        case FP_GE: FP_RUN(uint8_t, >=); break;
        }
        return;
    }
    if (esz == 2) {
        if (ct == RAY_SYM) {
            switch (op) {
            case FP_EQ: FP_RUN(uint16_t, ==); break;
            case FP_NE: FP_RUN(uint16_t, !=); break;
            default:    memset(bits, 0, (size_t)n); break;  /* unreachable */
            }
        } else {
            switch (op) {
            case FP_EQ: FP_RUN(int16_t, ==); break;
            case FP_NE: FP_RUN(int16_t, !=); break;
            case FP_LT: FP_RUN(int16_t, < ); break;
            case FP_LE: FP_RUN(int16_t, <=); break;
            case FP_GT: FP_RUN(int16_t, > ); break;
            case FP_GE: FP_RUN(int16_t, >=); break;
            }
        }
        return;
    }
    if (esz == 4) {
        if (ct == RAY_SYM) {
            switch (op) {
            case FP_EQ: FP_RUN(uint32_t, ==); break;
            case FP_NE: FP_RUN(uint32_t, !=); break;
            default:    memset(bits, 0, (size_t)n); break;  /* unreachable */
            }
        } else {
            switch (op) {
            case FP_EQ: FP_RUN(int32_t, ==); break;
            case FP_NE: FP_RUN(int32_t, !=); break;
            case FP_LT: FP_RUN(int32_t, < ); break;
            case FP_LE: FP_RUN(int32_t, <=); break;
            case FP_GT: FP_RUN(int32_t, > ); break;
            case FP_GE: FP_RUN(int32_t, >=); break;
            }
        }
        return;
    }
    /* Width 8: I64 / TIMESTAMP (or wide SYM, which we reject for ordering). */
    switch (op) {
    case FP_EQ: FP_RUN(int64_t, ==); break;
    case FP_NE: FP_RUN(int64_t, !=); break;
    case FP_LT: FP_RUN(int64_t, < ); break;
    case FP_LE: FP_RUN(int64_t, <=); break;
    case FP_GT: FP_RUN(int64_t, > ); break;
    case FP_GE: FP_RUN(int64_t, >=); break;
    }
}
#undef FP_RUN

/* Evaluate a (possibly ANDed) predicate over rows [start, end).  The
 * first child writes directly into bits[]; subsequent children eval into
 * a stack-resident tmp[] buffer and bitwise-AND into bits. */
void fp_eval_pred(const fp_pred_t* p, int64_t start, int64_t end,
                  uint8_t* bits)
{
    int64_t n = end - start;
    if (p->n_children == 0) {
        memset(bits, 1, (size_t)n);
        return;
    }
    fp_eval_cmp(&p->children[0], start, end, bits);
    if (p->n_children == 1) return;
    uint8_t tmp[RAY_MORSEL_ELEMS];
    for (uint8_t i = 1; i < p->n_children; i++) {
        fp_eval_cmp(&p->children[i], start, end, tmp);
        for (int64_t r = 0; r < n; r++) bits[r] &= tmp[r];
    }
}

/* Compile predicate DAG node (a comparison with OP_SCAN lhs and OP_CONST
 * rhs) against `tbl`.  Returns 0 on success, -1 if the shape can't be
 * handled (caller should bail and let the unfused path run). */
static int fp_compile_cmp(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                          fp_cmp_t* out)
{
    if (!pred_op || pred_op->arity != 2) return -1;
    out->fold = FP_FOLD_NONE;
    switch (pred_op->opcode) {
    case OP_EQ: out->op = FP_EQ; break;
    case OP_NE: out->op = FP_NE; break;
    case OP_LT: out->op = FP_LT; break;
    case OP_LE: out->op = FP_LE; break;
    case OP_GT: out->op = FP_GT; break;
    case OP_GE: out->op = FP_GE; break;
    default: return -1;
    }

    ray_op_t* lhs = pred_op->inputs[0];
    ray_op_t* rhs = pred_op->inputs[1];
    if (!lhs || !rhs) return -1;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return -1;

    ray_op_ext_t* lext = find_ext(g, lhs->id);
    ray_op_ext_t* rext = find_ext(g, rhs->id);
    if (!lext || !rext || !rext->literal) return -1;

    ray_t* col = ray_table_get_col(tbl, lext->sym);
    if (!col) return -1;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return -1;
    /* Ordering ops on SYM are meaningless (dict ID order != string order). */
    if (col->type == RAY_SYM && (out->op == FP_LT || out->op == FP_LE ||
                                 out->op == FP_GT || out->op == FP_GE))
        return -1;
    /* Reject nullable columns — fp_eval_cmp doesn't read the nullmap,
     * so a comparison against a stored sentinel slot would diverge from
     * the unfused null-aware kernel. */
    if (!fp_col_supported(col)) return -1;

    ray_t* cv = rext->literal;
    /* Atom type ↔ column class compatibility — block mixed-temporal
     * forms like `(== date_col timestamp_const)` whose raw-unit
     * comparison is meaningless. */
    if (!fp_atom_col_compatible(cv->type, col->type)) return -1;

    out->col_type  = col->type;
    out->col_attrs = col->attrs;
    out->col_esz   = ray_sym_elem_size(col->type, col->attrs);
    out->col_base  = ray_data(col);
    out->col_len   = col->len;

    if (out->col_type == RAY_SYM) {
        if (cv->type == -RAY_SYM) {
            out->cval = cv->i64;
            out->cval_in_dict = 1;
        } else if (cv->type == -RAY_STR) {
            int64_t did = ray_sym_find(ray_str_ptr(cv), ray_str_len(cv));
            out->cval = (did >= 0) ? did : 0;
            out->cval_in_dict = (did >= 0) ? 1 : 0;
        } else {
            return -1;
        }
    } else {
        /* Numeric / temporal: decode the atom into an int64 via the same
         * type-aware reader used elsewhere in the engine.  Atom type
         * has already been validated against col_type by
         * fp_atom_col_compatible above, so each branch knows the
         * stored unit matches the column's. */
        switch (cv->type) {
        case -RAY_I64:       case -RAY_TIMESTAMP: out->cval = cv->i64;            break;
        case -RAY_I32:       case -RAY_DATE:
        case -RAY_TIME:                            out->cval = (int64_t)cv->i32;  break;
        case -RAY_I16:                             out->cval = (int64_t)cv->i16;  break;
        case -RAY_BOOL:      case -RAY_U8:         out->cval = (int64_t)cv->b8;   break;
        default: return -1;
        }
        out->cval_in_dict = 1;
    }

    /* Range-check cval against the column's representable range and
     * pre-fold the comparison when the constant lies outside it.
     * Without this, the inner-loop cast `(T)cval` silently truncates and
     * yields wrong results (e.g. `u8_col == 300` matched value 44).
     *
     * For SYM, the storage IDs are unsigned and bounded by 2^(8*esz);
     * a global sym ID larger than that can't appear in this column.
     * For numeric, U8/BOOL is unsigned [0..255], I16/I32/I64 are signed. */
    int64_t v_min, v_max;
    int     is_unsigned;
    if (out->col_type == RAY_SYM) {
        is_unsigned = 1;
        v_min = 0;
        switch (out->col_esz) {
        case 1: v_max = 0xFFLL;       break;
        case 2: v_max = 0xFFFFLL;     break;
        case 4: v_max = 0xFFFFFFFFLL; break;
        default: v_max = INT64_MAX;   break;
        }
    } else {
        switch (out->col_esz) {
        case 1: is_unsigned = 1; v_min = 0;          v_max = 0xFFLL;     break; /* U8/BOOL */
        case 2: is_unsigned = 0; v_min = INT16_MIN;  v_max = INT16_MAX;  break;
        case 4: is_unsigned = 0; v_min = INT32_MIN;  v_max = INT32_MAX;  break;
        default: is_unsigned = 0; v_min = INT64_MIN; v_max = INT64_MAX;  break;
        }
    }
    (void)is_unsigned;

    if (out->cval < v_min || out->cval > v_max) {
        int below = (out->cval < v_min);
        switch (out->op) {
        case FP_EQ: out->fold = FP_FOLD_FALSE; break;
        case FP_NE: out->fold = FP_FOLD_TRUE;  break;
        case FP_LT: out->fold = below ? FP_FOLD_FALSE : FP_FOLD_TRUE;  break; /* col < below ⇒ false; col < above ⇒ true */
        case FP_LE: out->fold = below ? FP_FOLD_FALSE : FP_FOLD_TRUE;  break;
        case FP_GT: out->fold = below ? FP_FOLD_TRUE  : FP_FOLD_FALSE; break;
        case FP_GE: out->fold = below ? FP_FOLD_TRUE  : FP_FOLD_FALSE; break;
        }
    }
    return 0;
}

/* Walk the predicate DAG (an OP_AND tree of leaf comparisons) and collect
 * leaves into `out->children`.  Phase 3: balanced binary OP_AND emitted
 * by compile_expr_dag means we recurse on both inputs whenever we see an
 * OP_AND node.  Returns 0 on success, -1 if a leaf can't be compiled or
 * the fan-in exceeds FP_PRED_MAX_CHILDREN. */
static int fp_compile_pred_dag(ray_graph_t* g, ray_op_t* node, ray_t* tbl,
                               fp_pred_t* out)
{
    if (!node) return -1;
    if (node->opcode == OP_AND) {
        if (fp_compile_pred_dag(g, node->inputs[0], tbl, out) != 0) return -1;
        if (fp_compile_pred_dag(g, node->inputs[1], tbl, out) != 0) return -1;
        return 0;
    }
    if (out->n_children >= FP_PRED_MAX_CHILDREN) return -1;
    return fp_compile_cmp(g, node, tbl, &out->children[out->n_children++]);
}

int fp_compile_pred(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                    fp_pred_t* out)
{
    out->n_children = 0;
    /* No predicate → const-true.  fp_eval_pred memsets bits to 1
     * when n_children == 0, so the worker treats every row as a hit. */
    if (!pred_op) return 0;
    return fp_compile_pred_dag(g, pred_op, tbl, out);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Phase-2 parallel fused exec.
 *
 * Per-worker linear-probe hash shards, populated by ray_pool_dispatch
 * over morsel ranges.  Workers lazy-init their shard on first call,
 * grow it 2× when load reaches 0.5, and update it inline as they walk
 * the morsels.  After all workers finish, the main thread walks shard
 * occupants in series and merges them into a global HT before
 * materialising the 2-column [key, count] result table.
 * ──────────────────────────────────────────────────────────────────────── */

/* Per-worker shard capacity bounds.  INIT_CAP is the initial allocation
 * size; the shard grows by 2× whenever load factor exceeds 0.5.
 * MAX_CAP bounds the per-shard memory: 64 M slots ≈ 1 GB at one int64
 * key + state per slot (more for wide keys / multi-agg).  Group queries
 * with cardinalities approaching MAX_CAP × n_workers will hit OOM at
 * the shard grow and the fused exec returns oom; the OOM path triggers
 * the unfused fallback. */
#define FP_SHARD_INIT_CAP   1024ULL
#define FP_SHARD_MAX_CAP    (1ULL << 26)

/* Crossover row count below which the parallel combine (3-pass radix
 * scatter) loses to the serial walk because dispatch + scratch alloc
 * cost dominate.  Determined empirically; set high enough that
 * fixed-cost setup is amortised over enough work to pay back.  When
 * total_local < this, mk_combine_parallel returns 0 and the caller
 * runs the serial combine instead. */
#define FP_COMBINE_PAR_MIN  50000

typedef struct {
    int64_t* slots;       /* cap × 2 (occupied flag, key) */
    int64_t* counts;      /* cap */
    ray_t*   slots_hdr;   /* scratch headers for cleanup */
    ray_t*   counts_hdr;
    uint64_t cap;
    uint64_t mask;
    int64_t  n_filled;
} fp_shard_t;

typedef struct {
    fp_pred_t  pred;
    int8_t     kt;
    uint8_t    katt;
    uint8_t    kesz;
    const void* kbase;
    fp_shard_t* shards;     /* nw entries */
    uint64_t   init_cap;
    _Atomic(uint32_t) oom;  /* set by any worker on OOM; main bails */
} fp_par_ctx_t;

static int fp_shard_init(fp_shard_t* sh, uint64_t cap) {
    sh->slots  = (int64_t*)scratch_calloc(&sh->slots_hdr,
                                          (size_t)cap * 2 * sizeof(int64_t));
    sh->counts = (int64_t*)scratch_calloc(&sh->counts_hdr,
                                          (size_t)cap * sizeof(int64_t));
    if (!sh->slots || !sh->counts) {
        if (sh->slots_hdr)  { scratch_free(sh->slots_hdr);  sh->slots_hdr  = NULL; }
        if (sh->counts_hdr) { scratch_free(sh->counts_hdr); sh->counts_hdr = NULL; }
        sh->slots = NULL; sh->counts = NULL;
        return -1;
    }
    sh->cap  = cap;
    sh->mask = cap - 1;
    sh->n_filled = 0;
    return 0;
}

static void fp_shard_free(fp_shard_t* sh) {
    if (sh->slots_hdr)  { scratch_free(sh->slots_hdr);  sh->slots_hdr  = NULL; }
    if (sh->counts_hdr) { scratch_free(sh->counts_hdr); sh->counts_hdr = NULL; }
    sh->slots = NULL; sh->counts = NULL;
}

/* Double cap and rehash in place.  Returns -1 on OOM (shard left intact). */
static int fp_shard_grow(fp_shard_t* sh) {
    uint64_t new_cap = sh->cap * 2;
    if (new_cap > FP_SHARD_MAX_CAP) return -1;
    uint64_t new_mask = new_cap - 1;
    ray_t*   ns_hdr = NULL;
    ray_t*   nc_hdr = NULL;
    int64_t* ns = (int64_t*)scratch_calloc(&ns_hdr,
                                           (size_t)new_cap * 2 * sizeof(int64_t));
    int64_t* nc = (int64_t*)scratch_calloc(&nc_hdr,
                                           (size_t)new_cap * sizeof(int64_t));
    if (!ns || !nc) {
        if (ns_hdr) scratch_free(ns_hdr);
        if (nc_hdr) scratch_free(nc_hdr);
        return -1;
    }
    for (uint64_t s = 0; s < sh->cap; s++) {
        if (!sh->slots[s * 2]) continue;
        int64_t kv  = sh->slots[s * 2 + 1];
        int64_t cnt = sh->counts[s];
        uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t t = h & new_mask;
        while (ns[t * 2]) t = (t + 1) & new_mask;
        ns[t * 2]     = 1;
        ns[t * 2 + 1] = kv;
        nc[t]         = cnt;
    }
    scratch_free(sh->slots_hdr);
    scratch_free(sh->counts_hdr);
    sh->slots      = ns;
    sh->counts     = nc;
    sh->slots_hdr  = ns_hdr;
    sh->counts_hdr = nc_hdr;
    sh->cap        = new_cap;
    sh->mask       = new_mask;
    return 0;
}

/* Insert kv into shard, or bump count if already present.  Returns -1 on
 * grow OOM. */
static inline int fp_shard_upsert(fp_shard_t* sh, int64_t kv) {
    if (RAY_UNLIKELY(sh->n_filled * 2 >= (int64_t)sh->cap)) {
        if (fp_shard_grow(sh) != 0) return -1;
    }
    uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    uint64_t s = h & sh->mask;
    for (;;) {
        if (!sh->slots[s * 2]) {
            sh->slots[s * 2]     = 1;
            sh->slots[s * 2 + 1] = kv;
            sh->counts[s]        = 1;
            sh->n_filled++;
            return 0;
        }
        if (sh->slots[s * 2 + 1] == kv) { sh->counts[s]++; return 0; }
        s = (s + 1) & sh->mask;
    }
}

/* Worker: walk rows [start, end), eval predicate per-morsel, upsert
 * passing rows into per-worker shard. */
static void fp_par_fn(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    fp_par_ctx_t* c = (fp_par_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    fp_shard_t* sh = &c->shards[worker_id];
    if (!sh->slots) {
        if (fp_shard_init(sh, c->init_cap) != 0) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
    }
    int64_t row = start;
    while (row < end) {
        int64_t mend = row + RAY_MORSEL_ELEMS;
        if (mend > end) mend = end;
        int64_t mlen = mend - row;
        uint8_t bits[RAY_MORSEL_ELEMS];
        fp_eval_pred(&c->pred, row, mend, bits);
        for (int64_t r = 0; r < mlen; r++) {
            if (!bits[r]) continue;
            int64_t kv = read_by_esz(c->kbase, row + r, c->kesz);
            if (fp_shard_upsert(sh, kv) != 0) {
                atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
                return;
            }
        }
        row = mend;
    }
}

/* Parallel combine: 3-pass radix scatter.
 *
 *   Pass A (per shard, parallel): histogram slot counts per partition.
 *   Pass B (per shard, parallel): scatter (kv, cnt) into a flat packed
 *                                 buffer using the per-(shard, partition)
 *                                 cursors derived from the histogram.
 *   Pass C (per partition, parallel): dedup the partition's slice via a
 *                                     private linear-probe HT, pack the
 *                                     unique entries.
 *
 * The main thread concatenates per-partition outputs into the final
 * (key, count) columns.  Eliminates the serial 22-28 ms merge on Q13's
 * 700 K-group result. */
typedef struct {
    fp_shard_t*       shards;
    uint32_t          nw;
    uint32_t          p_bits;
    uint64_t          p_mask;
    int64_t           total_local;
    int64_t*          hist;            /* [nw * P]: per-shard per-partition counts */
    int64_t*          part_off;        /* [P + 1]: cumulative partition offsets */
    int64_t*          sw_cursor;       /* [nw * P]: scatter cursors */
    int64_t*          keys_buf;        /* [total_local]: packed keys */
    int64_t*          counts_buf;      /* [total_local]: packed counts */
    int64_t**         part_keys;       /* [P]: per-partition deduped keys */
    int64_t**         part_counts;     /* [P]: per-partition deduped counts */
    ray_t**           part_keys_hdr;
    ray_t**           part_counts_hdr;
    int64_t*          part_n;          /* [P]: deduped count */
    _Atomic(uint32_t) oom;
} fp_combine_par_ctx_t;

static void fp_combine_hist_fn(void* vctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    fp_combine_par_ctx_t* c = (fp_combine_par_ctx_t*)vctx;
    uint32_t w = (uint32_t)start;
    fp_shard_t* sh = &c->shards[w];
    int64_t* hist = c->hist + (size_t)w * (c->p_mask + 1);
    if (!sh->slots) return;
    for (uint64_t s = 0; s < sh->cap; s++) {
        if (!sh->slots[s * 2]) continue;
        int64_t kv = sh->slots[s * 2 + 1];
        uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t p = h & c->p_mask;
        hist[p]++;
    }
}

static void fp_combine_scat_fn(void* vctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    fp_combine_par_ctx_t* c = (fp_combine_par_ctx_t*)vctx;
    uint32_t w = (uint32_t)start;
    fp_shard_t* sh = &c->shards[w];
    int64_t* cur = c->sw_cursor + (size_t)w * (c->p_mask + 1);
    if (!sh->slots) return;
    for (uint64_t s = 0; s < sh->cap; s++) {
        if (!sh->slots[s * 2]) continue;
        int64_t kv  = sh->slots[s * 2 + 1];
        int64_t cnt = sh->counts[s];
        uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t p = h & c->p_mask;
        int64_t pos = cur[p]++;
        c->keys_buf[pos]   = kv;
        c->counts_buf[pos] = cnt;
    }
}

static void fp_combine_dedup_fn(void* vctx, uint32_t worker_id,
                                int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    fp_combine_par_ctx_t* c = (fp_combine_par_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    uint64_t p = (uint64_t)start;
    int64_t off  = c->part_off[p];
    int64_t pcnt = c->part_off[p + 1] - off;
    if (pcnt <= 0) {
        c->part_keys[p]   = NULL;
        c->part_counts[p] = NULL;
        c->part_n[p]      = 0;
        return;
    }

    uint64_t cap = 256;
    while (cap < (uint64_t)(pcnt * 2)) cap <<= 1;
    uint64_t mask = cap - 1;

    ray_t* slots_hdr = NULL;
    ray_t* cnts_hdr  = NULL;
    int64_t* slots = (int64_t*)scratch_calloc(&slots_hdr,
                                              (size_t)cap * 2 * sizeof(int64_t));
    int64_t* cnts  = (int64_t*)scratch_calloc(&cnts_hdr,
                                              (size_t)cap * sizeof(int64_t));
    if (!slots || !cnts) {
        if (slots_hdr) scratch_free(slots_hdr);
        if (cnts_hdr)  scratch_free(cnts_hdr);
        atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
        return;
    }

    int64_t n_filled = 0;
    const int64_t* k_in = c->keys_buf + off;
    const int64_t* v_in = c->counts_buf + off;
    for (int64_t i = 0; i < pcnt; i++) {
        int64_t kv  = k_in[i];
        int64_t cnt = v_in[i];
        uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t t = (h >> c->p_bits) & mask;
        for (;;) {
            if (!slots[t * 2]) {
                slots[t * 2]     = 1;
                slots[t * 2 + 1] = kv;
                cnts[t]          = cnt;
                n_filled++;
                break;
            }
            if (slots[t * 2 + 1] == kv) {
                cnts[t] += cnt;
                break;
            }
            t = (t + 1) & mask;
        }
    }

    /* Pack into per-partition output. */
    ray_t* k_hdr = NULL;
    ray_t* c_hdr = NULL;
    int64_t* k_out = (int64_t*)scratch_alloc(&k_hdr,
                                             (size_t)n_filled * sizeof(int64_t));
    int64_t* c_out = (int64_t*)scratch_alloc(&c_hdr,
                                             (size_t)n_filled * sizeof(int64_t));
    if (!k_out || !c_out) {
        if (k_hdr) scratch_free(k_hdr);
        if (c_hdr) scratch_free(c_hdr);
        scratch_free(slots_hdr); scratch_free(cnts_hdr);
        atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
        return;
    }
    int64_t out_idx = 0;
    for (uint64_t s = 0; s < cap; s++) {
        if (slots[s * 2]) {
            k_out[out_idx] = slots[s * 2 + 1];
            c_out[out_idx] = cnts[s];
            out_idx++;
        }
    }
    scratch_free(slots_hdr);
    scratch_free(cnts_hdr);

    c->part_keys[p]       = k_out;
    c->part_counts[p]     = c_out;
    c->part_keys_hdr[p]   = k_hdr;
    c->part_counts_hdr[p] = c_hdr;
    c->part_n[p]          = n_filled;
}

/* Combine: merge per-worker shards into a global linear-probe hash, then
 * materialize the [key, count] result columns.  Caller frees shards
 * afterwards. */
static ray_t* fp_combine_and_materialize(fp_shard_t* shards, uint32_t nw,
                                         int8_t kt, uint8_t katt,
                                         int64_t key_sym)
{
    /* Sum local fills to size the global HT. */
    int64_t total_local = 0;
    for (uint32_t w = 0; w < nw; w++) total_local += shards[w].n_filled;
    if (total_local == 0) {
        /* Empty result.  Build a 2-col table with 0 rows. */
        ray_t* k0 = (kt == RAY_SYM)
                  ? ray_sym_vec_new(katt & RAY_SYM_W_MASK, 0)
                  : ray_vec_new(kt, 0);
        ray_t* c0 = ray_vec_new(RAY_I64, 0);
        if (!k0 || !c0 || RAY_IS_ERR(k0) || RAY_IS_ERR(c0)) {
            if (k0 && !RAY_IS_ERR(k0)) ray_release(k0);
            if (c0 && !RAY_IS_ERR(c0)) ray_release(c0);
            return ray_error("oom", NULL);
        }
        k0->len = 0; c0->len = 0;
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) {
            ray_release(k0); ray_release(c0);
            return ray_error("oom", NULL);
        }
        int64_t cnt_sym = ray_sym_intern("count", 5);
        result = ray_table_add_col(result, key_sym, k0);
        result = ray_table_add_col(result, cnt_sym, c0);
        ray_release(k0); ray_release(c0);
        return result;
    }

    /* Parallel combine for high-cardinality results: 3-pass radix scatter.
     * Crossover at 50 K entries — below that, the serial walk has lower
     * overhead than the dispatch + scratch alloc cost. */
    ray_pool_t* cpool = ray_pool_get();
    if (cpool && total_local >= FP_COMBINE_PAR_MIN &&
        ray_pool_total_workers(cpool) >= 2 && nw <= 256)
    {
        uint32_t cnw = ray_pool_total_workers(cpool);
        uint32_t p_bits = 6;  /* 64 partitions */
        if (cnw < 8) p_bits = 4;
        uint64_t P = (uint64_t)1 << p_bits;
        uint64_t p_mask = P - 1;

        ray_t* hist_hdr = NULL;
        ray_t* off_hdr  = NULL;
        ray_t* cur_hdr  = NULL;
        ray_t* keys_hdr = NULL;
        ray_t* cnts_hdr = NULL;
        ray_t* pk_hdr   = NULL;
        ray_t* pc_hdr   = NULL;
        ray_t* pkh_hdr  = NULL;
        ray_t* pch_hdr  = NULL;
        ray_t* pn_hdr   = NULL;

        int64_t* hist = (int64_t*)scratch_calloc(&hist_hdr,
            (size_t)nw * (size_t)P * sizeof(int64_t));
        int64_t* part_off = (int64_t*)scratch_alloc(&off_hdr,
            (size_t)(P + 1) * sizeof(int64_t));
        int64_t* sw_cursor = (int64_t*)scratch_alloc(&cur_hdr,
            (size_t)nw * (size_t)P * sizeof(int64_t));
        int64_t* keys_buf = (int64_t*)scratch_alloc(&keys_hdr,
            (size_t)total_local * sizeof(int64_t));
        int64_t* counts_buf = (int64_t*)scratch_alloc(&cnts_hdr,
            (size_t)total_local * sizeof(int64_t));
        int64_t** part_keys = (int64_t**)scratch_calloc(&pk_hdr,
            (size_t)P * sizeof(int64_t*));
        int64_t** part_counts = (int64_t**)scratch_calloc(&pc_hdr,
            (size_t)P * sizeof(int64_t*));
        ray_t**   part_keys_hdr = (ray_t**)scratch_calloc(&pkh_hdr,
            (size_t)P * sizeof(ray_t*));
        ray_t**   part_counts_hdr = (ray_t**)scratch_calloc(&pch_hdr,
            (size_t)P * sizeof(ray_t*));
        int64_t*  part_n = (int64_t*)scratch_calloc(&pn_hdr,
            (size_t)P * sizeof(int64_t));

        if (hist && part_off && sw_cursor && keys_buf && counts_buf &&
            part_keys && part_counts && part_keys_hdr &&
            part_counts_hdr && part_n)
        {
            fp_combine_par_ctx_t pctx = {
                .shards          = shards,
                .nw              = nw,
                .p_bits          = p_bits,
                .p_mask          = p_mask,
                .total_local     = total_local,
                .hist            = hist,
                .part_off        = part_off,
                .sw_cursor       = sw_cursor,
                .keys_buf        = keys_buf,
                .counts_buf      = counts_buf,
                .part_keys       = part_keys,
                .part_counts     = part_counts,
                .part_keys_hdr   = part_keys_hdr,
                .part_counts_hdr = part_counts_hdr,
                .part_n          = part_n,
                .oom             = 0,
            };
            ray_pool_dispatch_n(cpool, fp_combine_hist_fn, &pctx, nw);

            /* Compute partition offsets + per-(shard,partition) cursors. */
            int64_t total = 0;
            for (uint64_t p = 0; p < P; p++) {
                part_off[p] = total;
                int64_t cum = total;
                for (uint32_t w = 0; w < nw; w++) {
                    int64_t cnt = hist[(size_t)w * P + p];
                    sw_cursor[(size_t)w * P + p] = cum;
                    cum += cnt;
                }
                total = cum;
            }
            part_off[P] = total;

            ray_pool_dispatch_n(cpool, fp_combine_scat_fn, &pctx, nw);
            ray_pool_dispatch_n(cpool, fp_combine_dedup_fn, &pctx, (uint32_t)P);

            if (!atomic_load_explicit(&pctx.oom, memory_order_relaxed)) {
                int64_t global_n = 0;
                for (uint64_t p = 0; p < P; p++) global_n += part_n[p];

                ray_t* k_out = (kt == RAY_SYM)
                             ? ray_sym_vec_new(katt & RAY_SYM_W_MASK, global_n)
                             : ray_vec_new(kt, global_n);
                ray_t* c_out = ray_vec_new(RAY_I64, global_n);
                if (!k_out || !c_out || RAY_IS_ERR(k_out) || RAY_IS_ERR(c_out)) {
                    if (k_out && !RAY_IS_ERR(k_out)) ray_release(k_out);
                    if (c_out && !RAY_IS_ERR(c_out)) ray_release(c_out);
                    for (uint64_t p = 0; p < P; p++) {
                        if (part_keys_hdr[p])   scratch_free(part_keys_hdr[p]);
                        if (part_counts_hdr[p]) scratch_free(part_counts_hdr[p]);
                    }
                    scratch_free(hist_hdr); scratch_free(off_hdr);
                    scratch_free(cur_hdr); scratch_free(keys_hdr);
                    scratch_free(cnts_hdr);
                    scratch_free(pk_hdr); scratch_free(pc_hdr);
                    scratch_free(pkh_hdr); scratch_free(pch_hdr);
                    scratch_free(pn_hdr);
                    return ray_error("oom", NULL);
                }
                k_out->len = global_n;
                c_out->len = global_n;
                void*    k_dst = ray_data(k_out);
                int64_t* c_dst = (int64_t*)ray_data(c_out);
                int64_t  gi    = 0;
                for (uint64_t p = 0; p < P; p++) {
                    int64_t  pn = part_n[p];
                    int64_t* pk = part_keys[p];
                    int64_t* pc = part_counts[p];
                    for (int64_t i = 0; i < pn; i++) {
                        write_col_i64(k_dst, gi, pk[i], kt, katt);
                        c_dst[gi] = pc[i];
                        gi++;
                    }
                    if (part_keys_hdr[p])   scratch_free(part_keys_hdr[p]);
                    if (part_counts_hdr[p]) scratch_free(part_counts_hdr[p]);
                }
                scratch_free(hist_hdr); scratch_free(off_hdr);
                scratch_free(cur_hdr); scratch_free(keys_hdr);
                scratch_free(cnts_hdr);
                scratch_free(pk_hdr); scratch_free(pc_hdr);
                scratch_free(pkh_hdr); scratch_free(pch_hdr);
                scratch_free(pn_hdr);

                ray_t* result = ray_table_new(2);
                if (!result || RAY_IS_ERR(result)) {
                    ray_release(k_out); ray_release(c_out);
                    return ray_error("oom", NULL);
                }
                int64_t cnt_sym = ray_sym_intern("count", 5);
                result = ray_table_add_col(result, key_sym, k_out);
                result = ray_table_add_col(result, cnt_sym, c_out);
                ray_release(k_out); ray_release(c_out);
                return result;
            }
            /* OOM during dedup pass — free per-partition outputs, fall through. */
            for (uint64_t p = 0; p < P; p++) {
                if (part_keys_hdr[p])   scratch_free(part_keys_hdr[p]);
                if (part_counts_hdr[p]) scratch_free(part_counts_hdr[p]);
            }
        }
        if (hist_hdr) scratch_free(hist_hdr);
        if (off_hdr)  scratch_free(off_hdr);
        if (cur_hdr)  scratch_free(cur_hdr);
        if (keys_hdr) scratch_free(keys_hdr);
        if (cnts_hdr) scratch_free(cnts_hdr);
        if (pk_hdr)   scratch_free(pk_hdr);
        if (pc_hdr)   scratch_free(pc_hdr);
        if (pkh_hdr)  scratch_free(pkh_hdr);
        if (pch_hdr)  scratch_free(pch_hdr);
        if (pn_hdr)   scratch_free(pn_hdr);
    }

    /* Global cap: 2 × total_local rounded up to power of 2.  This is an
     * upper bound (groups can collide across shards) so the global HT
     * never has to grow. */
    uint64_t gcap = 1024;
    while (gcap < (uint64_t)(total_local * 2) && gcap < FP_SHARD_MAX_CAP) gcap <<= 1;
    uint64_t gmask = gcap - 1;
    ray_t* gs_hdr = NULL;
    ray_t* gc_hdr = NULL;
    int64_t* gs = (int64_t*)scratch_calloc(&gs_hdr,
                                           (size_t)gcap * 2 * sizeof(int64_t));
    int64_t* gc = (int64_t*)scratch_calloc(&gc_hdr,
                                           (size_t)gcap * sizeof(int64_t));
    if (!gs || !gc) {
        if (gs_hdr) scratch_free(gs_hdr);
        if (gc_hdr) scratch_free(gc_hdr);
        return ray_error("oom", NULL);
    }
    int64_t global_n = 0;
    for (uint32_t w = 0; w < nw; w++) {
        fp_shard_t* sh = &shards[w];
        if (!sh->slots) continue;
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv  = sh->slots[s * 2 + 1];
            int64_t cnt = sh->counts[s];
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t t = h & gmask;
            for (;;) {
                if (!gs[t * 2]) {
                    gs[t * 2]     = 1;
                    gs[t * 2 + 1] = kv;
                    gc[t]         = cnt;
                    global_n++;
                    break;
                }
                if (gs[t * 2 + 1] == kv) { gc[t] += cnt; break; }
                t = (t + 1) & gmask;
            }
        }
    }

    /* Materialize. */
    ray_t* k_out = (kt == RAY_SYM)
                 ? ray_sym_vec_new(katt & RAY_SYM_W_MASK, global_n)
                 : ray_vec_new(kt, global_n);
    ray_t* c_out = ray_vec_new(RAY_I64, global_n);
    if (!k_out || !c_out || RAY_IS_ERR(k_out) || RAY_IS_ERR(c_out)) {
        if (k_out && !RAY_IS_ERR(k_out)) ray_release(k_out);
        if (c_out && !RAY_IS_ERR(c_out)) ray_release(c_out);
        scratch_free(gs_hdr); scratch_free(gc_hdr);
        return ray_error("oom", NULL);
    }
    k_out->len = global_n;
    c_out->len = global_n;
    void*    k_dst = ray_data(k_out);
    int64_t* c_dst = (int64_t*)ray_data(c_out);
    int64_t  gi    = 0;
    for (uint64_t s = 0; s < gcap; s++) {
        if (!gs[s * 2]) continue;
        int64_t kv = gs[s * 2 + 1];
        write_col_i64(k_dst, gi, kv, kt, katt);
        c_dst[gi] = gc[s];
        gi++;
    }
    scratch_free(gs_hdr);
    scratch_free(gc_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(k_out); ray_release(c_out);
        return ray_error("oom", NULL);
    }
    int64_t cnt_sym = ray_sym_intern("count", 5);
    result = ray_table_add_col(result, key_sym, k_out);
    result = ray_table_add_col(result, cnt_sym, c_out);
    ray_release(k_out);
    ray_release(c_out);
    return result;
}

/* Phase-3 fast path: single-key, single OP_COUNT.  Kept byte-for-byte
 * stable so all previously-fired queries (Q8/Q37/Q38/Q43) maintain their
 * cycle budget — this is the path that owns the parallel HT-shard wins. */
static ray_t* exec_filtered_group_count1(ray_graph_t* g, ray_op_ext_t* ext,
                                         ray_t* tbl)
{
    ray_op_t* pred_op = ext->base.inputs[0];
    fp_par_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (fp_compile_pred(g, pred_op, tbl, &ctx.pred) != 0)
        return ray_error("nyi", "fused_group: predicate shape unsupported");

    ray_op_t* key_op = ext->keys[0];
    if (!key_op || key_op->opcode != OP_SCAN)
        return ray_error("nyi", "fused_group: phase-2 needs SCAN key");
    ray_op_ext_t* kext = find_ext(g, key_op->id);
    if (!kext) return ray_error("nyi", NULL);
    ray_t* key_col = ray_table_get_col(tbl, kext->sym);
    if (!key_col) return ray_error("schema", NULL);
    if (RAY_IS_PARTED(key_col->type) || key_col->type == RAY_MAPCOMMON)
        return ray_error("nyi", "fused_group: phase-2 needs flat key column");

    int64_t nrows = key_col->len;
    ctx.kt    = key_col->type;
    ctx.katt  = key_col->attrs;
    ctx.kesz  = ray_sym_elem_size(ctx.kt, ctx.katt);
    ctx.kbase = ray_data(key_col);
    ctx.init_cap = FP_SHARD_INIT_CAP;
    atomic_store_explicit(&ctx.oom, 0, memory_order_relaxed);

    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
    ray_t* shards_hdr = NULL;
    ctx.shards = (fp_shard_t*)scratch_calloc(&shards_hdr,
                                             (size_t)nw * sizeof(fp_shard_t));
    if (!ctx.shards) return ray_error("oom", NULL);

    if (pool) {
        ray_pool_dispatch(pool, fp_par_fn, &ctx, nrows);
    } else {
        /* No pool: serial fallback. */
        fp_par_fn(&ctx, 0, 0, nrows);
    }

    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        for (uint32_t w = 0; w < nw; w++) fp_shard_free(&ctx.shards[w]);
        scratch_free(shards_hdr);
        return ray_error("oom", "fused_group: shard OOM");
    }

    ray_t* result = fp_combine_and_materialize(ctx.shards, nw,
                                               ctx.kt, ctx.katt, kext->sym);
    for (uint32_t w = 0; w < nw; w++) fp_shard_free(&ctx.shards[w]);
    scratch_free(shards_hdr);
    return result;
}


/* ═════════════════════════════════════════════════════════════════════════
 * Multi-agg / multi-key path
 *
 * Lives entirely separate from the count1 fast path above so that any
 * struct or codegen quirks here cannot regress the queries that fire
 * count1.  Triggered only when n_aggs > 1 OR n_keys > 1 OR the single
 * agg isn't COUNT.
 *
 * Supports:
 *   - Aggregates: COUNT, SUM, MIN, MAX, AVG (integer/temporal inputs)
 *   - Keys: 1..16 keys, total packed bytes ≤ 8 (composite int64 slot)
 *
 * F32/F64 inputs and count(distinct) bail to the unfused path.
 * ════════════════════════════════════════════════════════════════════════ */

#define FP_MAX_AGGS 8
#define FP_MAX_KEYS 16

typedef enum {
    MK_AGG_COUNT = 0,
    MK_AGG_SUM   = 1,
    MK_AGG_MIN   = 2,
    MK_AGG_MAX   = 3,
    MK_AGG_AVG   = 4,
} mk_agg_kind_t;

typedef struct {
    mk_agg_kind_t kind;
    int8_t        in_type;
    uint8_t       in_attrs;
    uint8_t       in_esz;
    /* 1 when in_type stores an unsigned narrow value (U8/BOOL); 0 for
     * signed widths (I16/I32/I64/DATE/TIME/TIMESTAMP).  Used to
     * sign-extend correctly in SUM/MIN/MAX/AVG so a stored -1 reads as
     * -1 and not 65535. */
    uint8_t       in_unsigned;
    const void*   in_base;
    uint8_t       state_off;
} mk_agg_t;

typedef struct {
    int8_t      type;
    uint8_t     attrs;
    uint8_t     esz;
    uint8_t     bit_off;
    const void* base;
    int64_t     sym;
} mk_key_t;

typedef struct {
    int64_t* slots;       /* [cap * 2]: (used, kv_lo) */
    int64_t* slots_hi;    /* [cap]: kv_hi — only allocated when ctx->wide */
    int64_t* state;
    ray_t*   slots_hdr;
    ray_t*   slots_hi_hdr;
    ray_t*   state_hdr;
    uint64_t cap;
    uint64_t mask;
    int64_t  n_filled;
} mk_shard_t;

typedef struct {
    fp_pred_t   pred;
    /* Hot key fast path for n_keys==1 — kbase/kesz live here so inner
     * loop reads them off cache line shared with pred. */
    const void* k0_base;
    int8_t      k0_type;
    uint8_t     k0_attrs;
    uint8_t     k0_esz;
    uint8_t     n_keys;
    uint8_t     n_aggs;
    uint8_t     total_state;
    uint8_t     wide;        /* 1 when total_bytes > 8 (uses kv_hi side array) */
    /* Cool fields (only touched once per dispatch or in cold paths). */
    mk_shard_t* shards;
    uint64_t    init_cap;
    _Atomic(uint32_t) oom;
    mk_key_t    keys[FP_MAX_KEYS];
    mk_agg_t    aggs[FP_MAX_AGGS];
} mk_par_ctx_t;

/* ─── Composite key compose ────────────────────────────────────────── */

static inline int64_t mk_compose_key(const mk_par_ctx_t* c, int64_t row) {
    if (c->n_keys == 1) {
        return read_by_esz(c->k0_base, row, c->k0_esz);
    }
    uint64_t composite = 0;
    for (uint8_t k = 0; k < c->n_keys; k++) {
        const mk_key_t* kk = &c->keys[k];
        uint64_t v = (uint64_t)read_by_esz(kk->base, row, kk->esz);
        if (kk->esz < 8) v &= ((1ULL << (kk->esz * 8)) - 1);
        composite |= v << kk->bit_off;
    }
    return (int64_t)composite;
}

/* Wide-key compose: pack each key into the lo/hi 64-bit halves.
 * A key with bit_off in [0,64) sits in lo; one with bit_off >= 64 sits
 * in hi at (bit_off - 64); a key that crosses the 64-bit boundary
 * (bit_off < 64 < bit_off + 8*esz) splits between lo and hi.  The
 * decompose path in mk_combine_and_materialize stitches the halves
 * back together using the same rules. */
static inline void mk_compose_key2(const mk_par_ctx_t* c, int64_t row,
                                   int64_t* out_lo, int64_t* out_hi)
{
    uint64_t lo = 0, hi = 0;
    for (uint8_t k = 0; k < c->n_keys; k++) {
        const mk_key_t* kk = &c->keys[k];
        uint64_t v = (uint64_t)read_by_esz(kk->base, row, kk->esz);
        if (kk->esz < 8) v &= ((1ULL << (kk->esz * 8)) - 1);
        uint8_t bit_off = kk->bit_off;
        uint8_t bit_end = (uint8_t)(bit_off + kk->esz * 8);
        if (bit_off >= 64) {
            hi |= v << (bit_off - 64);
        } else if (bit_end <= 64) {
            lo |= v << bit_off;
        } else {
            /* Spans the 64-bit boundary. */
            uint8_t lo_bits = (uint8_t)(64 - bit_off);
            uint64_t lo_mask = (lo_bits == 64) ? ~0ULL : ((1ULL << lo_bits) - 1);
            lo |= (v & lo_mask) << bit_off;
            hi |= v >> lo_bits;
        }
    }
    *out_lo = (int64_t)lo;
    *out_hi = (int64_t)hi;
}

/* Wide-key hash: 128-bit composite → 64-bit hash.  Mixes lo by Knuth's
 * golden-ratio constant (same as narrow path) and folds in hi via the
 * splitmix64 multiplier so different hi values land in different
 * partitions when the radix scatter inspects only the low bits. */
static inline uint64_t mk_hash_lo_hi(int64_t kv_lo, int64_t kv_hi) {
    uint64_t h  = (uint64_t)kv_lo * 0x9E3779B97F4A7C15ULL;
    uint64_t hh = (uint64_t)kv_hi * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 33;
    h ^= hh;
    h ^= h >> 33;
    return h;
}

/* ─── Per-row aggregator state init/accum ──────────────────────────── */

static inline void mk_state_init_row(int64_t* st, const mk_agg_t* aggs,
                                     uint8_t n_aggs, int64_t row)
{
    for (uint8_t i = 0; i < n_aggs; i++) {
        const mk_agg_t* a = &aggs[i];
        switch (a->kind) {
        case MK_AGG_COUNT:
            st[a->state_off] = 1;
            break;
        case MK_AGG_SUM:
        case MK_AGG_MIN:
        case MK_AGG_MAX: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            st[a->state_off] = v;
            break;
        }
        case MK_AGG_AVG: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            st[a->state_off    ] = v;
            st[a->state_off + 1] = 1;
            break;
        }
        }
    }
}

static inline void mk_state_accum_row(int64_t* st, const mk_agg_t* aggs,
                                      uint8_t n_aggs, int64_t row)
{
    for (uint8_t i = 0; i < n_aggs; i++) {
        const mk_agg_t* a = &aggs[i];
        switch (a->kind) {
        case MK_AGG_COUNT:
            st[a->state_off] += 1;
            break;
        case MK_AGG_SUM: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            st[a->state_off] += v;
            break;
        }
        case MK_AGG_MIN: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            if (v < st[a->state_off]) st[a->state_off] = v;
            break;
        }
        case MK_AGG_MAX: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            if (v > st[a->state_off]) st[a->state_off] = v;
            break;
        }
        case MK_AGG_AVG: {
            int64_t v = read_signed_by_esz(a->in_base, row, a->in_esz, a->in_unsigned);
            st[a->state_off    ] += v;
            st[a->state_off + 1] += 1;
            break;
        }
        }
    }
}

static inline void mk_state_merge(int64_t* dst, const int64_t* src,
                                  const mk_agg_t* aggs, uint8_t n_aggs)
{
    for (uint8_t i = 0; i < n_aggs; i++) {
        const mk_agg_t* a = &aggs[i];
        switch (a->kind) {
        case MK_AGG_COUNT:
        case MK_AGG_SUM:
            dst[a->state_off] += src[a->state_off]; break;
        case MK_AGG_MIN:
            if (src[a->state_off] < dst[a->state_off])
                dst[a->state_off] = src[a->state_off];
            break;
        case MK_AGG_MAX:
            if (src[a->state_off] > dst[a->state_off])
                dst[a->state_off] = src[a->state_off];
            break;
        case MK_AGG_AVG:
            dst[a->state_off    ] += src[a->state_off    ];
            dst[a->state_off + 1] += src[a->state_off + 1];
            break;
        }
    }
}

/* ─── Shard mgmt for the multi path ─────────────────────────────────── */

static int mk_shard_init(mk_shard_t* sh, uint64_t cap, uint8_t total_state,
                         uint8_t wide)
{
    sh->slots = (int64_t*)scratch_calloc(&sh->slots_hdr,
                                         (size_t)cap * 2 * sizeof(int64_t));
    sh->state = (int64_t*)scratch_calloc(&sh->state_hdr,
                                         (size_t)cap * total_state * sizeof(int64_t));
    if (wide) {
        sh->slots_hi = (int64_t*)scratch_calloc(&sh->slots_hi_hdr,
                                                (size_t)cap * sizeof(int64_t));
    } else {
        sh->slots_hi = NULL;
        sh->slots_hi_hdr = NULL;
    }
    if (!sh->slots || !sh->state || (wide && !sh->slots_hi)) {
        if (sh->slots_hdr)    { scratch_free(sh->slots_hdr);    sh->slots_hdr = NULL; }
        if (sh->state_hdr)    { scratch_free(sh->state_hdr);    sh->state_hdr = NULL; }
        if (sh->slots_hi_hdr) { scratch_free(sh->slots_hi_hdr); sh->slots_hi_hdr = NULL; }
        sh->slots = NULL; sh->state = NULL; sh->slots_hi = NULL;
        return -1;
    }
    sh->cap = cap; sh->mask = cap - 1; sh->n_filled = 0;
    return 0;
}

static void mk_shard_free(mk_shard_t* sh) {
    if (sh->slots_hdr)    { scratch_free(sh->slots_hdr);    sh->slots_hdr = NULL; }
    if (sh->slots_hi_hdr) { scratch_free(sh->slots_hi_hdr); sh->slots_hi_hdr = NULL; }
    if (sh->state_hdr)    { scratch_free(sh->state_hdr);    sh->state_hdr = NULL; }
    sh->slots = NULL; sh->slots_hi = NULL; sh->state = NULL;
}

static int mk_shard_grow(mk_shard_t* sh, uint8_t total_state, uint8_t wide) {
    uint64_t new_cap = sh->cap * 2;
    if (new_cap > FP_SHARD_MAX_CAP) return -1;
    uint64_t new_mask = new_cap - 1;
    ray_t* ns_hdr = NULL; ray_t* nst_hdr = NULL; ray_t* nshi_hdr = NULL;
    int64_t* ns   = (int64_t*)scratch_calloc(&ns_hdr,  (size_t)new_cap * 2 * sizeof(int64_t));
    int64_t* nst  = (int64_t*)scratch_calloc(&nst_hdr, (size_t)new_cap * total_state * sizeof(int64_t));
    int64_t* nshi = NULL;
    if (wide) {
        nshi = (int64_t*)scratch_calloc(&nshi_hdr,
                                        (size_t)new_cap * sizeof(int64_t));
    }
    if (!ns || !nst || (wide && !nshi)) {
        if (ns_hdr)   scratch_free(ns_hdr);
        if (nst_hdr)  scratch_free(nst_hdr);
        if (nshi_hdr) scratch_free(nshi_hdr);
        return -1;
    }
    if (wide) {
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv_lo = sh->slots[s * 2 + 1];
            int64_t kv_hi = sh->slots_hi[s];
            uint64_t h = mk_hash_lo_hi(kv_lo, kv_hi);
            uint64_t t = h & new_mask;
            while (ns[t * 2]) t = (t + 1) & new_mask;
            ns[t * 2] = 1; ns[t * 2 + 1] = kv_lo;
            nshi[t] = kv_hi;
            for (uint8_t k = 0; k < total_state; k++)
                nst[t * total_state + k] = sh->state[s * total_state + k];
        }
    } else {
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv = sh->slots[s * 2 + 1];
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t t = h & new_mask;
            while (ns[t * 2]) t = (t + 1) & new_mask;
            ns[t * 2] = 1; ns[t * 2 + 1] = kv;
            for (uint8_t k = 0; k < total_state; k++)
                nst[t * total_state + k] = sh->state[s * total_state + k];
        }
    }
    scratch_free(sh->slots_hdr);
    scratch_free(sh->state_hdr);
    if (sh->slots_hi_hdr) scratch_free(sh->slots_hi_hdr);
    sh->slots = ns; sh->state = nst; sh->slots_hi = nshi;
    sh->slots_hdr = ns_hdr; sh->state_hdr = nst_hdr; sh->slots_hi_hdr = nshi_hdr;
    sh->cap = new_cap; sh->mask = new_mask;
    return 0;
}

static inline int mk_shard_upsert(mk_shard_t* sh, const mk_par_ctx_t* c,
                                  int64_t kv, int64_t row)
{
    if (RAY_UNLIKELY(sh->n_filled * 2 >= (int64_t)sh->cap)) {
        if (mk_shard_grow(sh, c->total_state, c->wide) != 0) return -1;
    }
    uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    uint64_t s = h & sh->mask;
    for (;;) {
        if (!sh->slots[s * 2]) {
            sh->slots[s * 2] = 1; sh->slots[s * 2 + 1] = kv;
            mk_state_init_row(&sh->state[s * c->total_state],
                              c->aggs, c->n_aggs, row);
            sh->n_filled++;
            return 0;
        }
        if (sh->slots[s * 2 + 1] == kv) {
            mk_state_accum_row(&sh->state[s * c->total_state],
                               c->aggs, c->n_aggs, row);
            return 0;
        }
        s = (s + 1) & sh->mask;
    }
}

/* ─── Worker fn — DuckDB-style chunked vectorized aggregate update ───
 *
 * Per morsel we run two passes:
 *
 *   PASS 1 (probe): linear-probe the HT for every passing row.  On a
 *   new slot we initialize the per-agg state to a per-kind sentinel
 *   (0 for COUNT/SUM/AVG-sum, 0 for AVG-count, INT64_MAX for MIN,
 *   INT64_MIN for MAX) so the accumulate-only update logic in pass 2
 *   produces the correct first value without a separate "first row"
 *   branch.  Pass 1 fills slot_idx[i] (HT slot for the i-th passing row)
 *   and src_rows[i] (source row index) into stack-resident arrays.
 *
 *   PASS 2 (update): for each aggregate, run a tight per-agg loop over
 *   match_count entries.  No per-row switch dispatch — the kind switch
 *   is hoisted out of the loop, so each loop body is a single
 *   accumulate operation against state[slot_idx[i] * total + off].
 *
 * Mirrors duckdb's GroupedAggregateHashTable::AddChunk path: probe
 * first, then UpdateStates per aggregate.  Eliminates the O(rows × aggs)
 * branch dispatch the per-row update did. */
static void mk_par_fn(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    mk_par_ctx_t* c = (mk_par_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    mk_shard_t* sh = &c->shards[worker_id];
    uint8_t  wide        = c->wide;
    if (!sh->slots) {
        if (mk_shard_init(sh, c->init_cap, c->total_state, wide) != 0) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
    }

    uint8_t  total_state = c->total_state;
    uint8_t  n_aggs      = c->n_aggs;

    int64_t row = start;
    while (row < end) {
        int64_t mend = row + RAY_MORSEL_ELEMS;
        if (mend > end) mend = end;
        int64_t mlen = mend - row;
        uint8_t bits[RAY_MORSEL_ELEMS];
        fp_eval_pred(&c->pred, row, mend, bits);

        /* Count passing rows up-front so we can pre-grow the shard if a
         * worst-case (all new groups) chunk would push past load 0.5.
         * This guarantees pass 1 never grows mid-chunk, which would
         * invalidate the slot_idx[] we accumulate. */
        int match_count = 0;
        for (int64_t r = 0; r < mlen; r++) match_count += bits[r];
        if (match_count == 0) { row = mend; continue; }

        while (sh->n_filled + match_count > (int64_t)(sh->cap / 2)) {
            if (mk_shard_grow(sh, total_state, wide) != 0) {
                atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
                return;
            }
        }

        /* PASS 1: probe; record slot_idx and src_rows for each passing row.
         * Two body variants — narrow (kv fits in 64 bits) and wide
         * (kv needs lo+hi).  The branch on `wide` is hoisted out of the
         * per-row loop and is constant across the whole query, so the
         * predictor pins it on first iteration.  Narrow body is byte-
         * for-byte the prior implementation. */
        uint32_t slot_idx[RAY_MORSEL_ELEMS];
        int32_t  src_rows[RAY_MORSEL_ELEMS];   /* offset within morsel */
        int64_t  base_row = row;
        int64_t* slots = sh->slots;
        int64_t* slots_hi = sh->slots_hi;
        int64_t* state = sh->state;
        uint64_t mask  = sh->mask;
        int      mi = 0;
        if (!wide) {
            for (int64_t r = 0; r < mlen; r++) {
                if (!bits[r]) continue;
                int64_t source_row = base_row + r;
                int64_t kv = mk_compose_key(c, source_row);
                uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
                h ^= h >> 33;
                uint64_t s = h & mask;
                for (;;) {
                    if (!slots[s * 2]) {
                        slots[s * 2]     = 1;
                        slots[s * 2 + 1] = kv;
                        int64_t* st = &state[s * total_state];
                        for (uint8_t a = 0; a < n_aggs; a++) {
                            const mk_agg_t* ag = &c->aggs[a];
                            switch (ag->kind) {
                            case MK_AGG_COUNT:
                            case MK_AGG_SUM:
                                st[ag->state_off] = 0; break;
                            case MK_AGG_MIN:
                                st[ag->state_off] = INT64_MAX; break;
                            case MK_AGG_MAX:
                                st[ag->state_off] = INT64_MIN; break;
                            case MK_AGG_AVG:
                                st[ag->state_off    ] = 0;
                                st[ag->state_off + 1] = 0; break;
                            }
                        }
                        sh->n_filled++;
                        break;
                    }
                    if (slots[s * 2 + 1] == kv) break;
                    s = (s + 1) & mask;
                }
                slot_idx[mi] = (uint32_t)s;
                src_rows[mi] = (int32_t)r;
                mi++;
            }
        } else {
            for (int64_t r = 0; r < mlen; r++) {
                if (!bits[r]) continue;
                int64_t source_row = base_row + r;
                int64_t kv_lo, kv_hi;
                mk_compose_key2(c, source_row, &kv_lo, &kv_hi);
                uint64_t h = mk_hash_lo_hi(kv_lo, kv_hi);
                uint64_t s = h & mask;
                for (;;) {
                    if (!slots[s * 2]) {
                        slots[s * 2]     = 1;
                        slots[s * 2 + 1] = kv_lo;
                        slots_hi[s]      = kv_hi;
                        int64_t* st = &state[s * total_state];
                        for (uint8_t a = 0; a < n_aggs; a++) {
                            const mk_agg_t* ag = &c->aggs[a];
                            switch (ag->kind) {
                            case MK_AGG_COUNT:
                            case MK_AGG_SUM:
                                st[ag->state_off] = 0; break;
                            case MK_AGG_MIN:
                                st[ag->state_off] = INT64_MAX; break;
                            case MK_AGG_MAX:
                                st[ag->state_off] = INT64_MIN; break;
                            case MK_AGG_AVG:
                                st[ag->state_off    ] = 0;
                                st[ag->state_off + 1] = 0; break;
                            }
                        }
                        sh->n_filled++;
                        break;
                    }
                    if (slots[s * 2 + 1] == kv_lo && slots_hi[s] == kv_hi) break;
                    s = (s + 1) & mask;
                }
                slot_idx[mi] = (uint32_t)s;
                src_rows[mi] = (int32_t)r;
                mi++;
            }
        }

        /* PASS 2: per-agg vectorized update.  Each loop is branch-free
         * across rows; the kind switch is amortized over match_count. */
        for (uint8_t a = 0; a < n_aggs; a++) {
            const mk_agg_t* ag = &c->aggs[a];
            uint8_t off = ag->state_off;
            switch (ag->kind) {
            case MK_AGG_COUNT:
                for (int i = 0; i < match_count; i++)
                    state[slot_idx[i] * total_state + off]++;
                break;
            case MK_AGG_SUM: {
                const void* in_base = ag->in_base;
                uint8_t in_esz = ag->in_esz;
                int     in_uns = ag->in_unsigned;
                for (int i = 0; i < match_count; i++) {
                    int64_t v = read_signed_by_esz(in_base,
                                                   base_row + src_rows[i],
                                                   in_esz, in_uns);
                    state[slot_idx[i] * total_state + off] += v;
                }
                break;
            }
            case MK_AGG_MIN: {
                const void* in_base = ag->in_base;
                uint8_t in_esz = ag->in_esz;
                int     in_uns = ag->in_unsigned;
                for (int i = 0; i < match_count; i++) {
                    int64_t v = read_signed_by_esz(in_base,
                                                   base_row + src_rows[i],
                                                   in_esz, in_uns);
                    int64_t* p = &state[slot_idx[i] * total_state + off];
                    if (v < *p) *p = v;
                }
                break;
            }
            case MK_AGG_MAX: {
                const void* in_base = ag->in_base;
                uint8_t in_esz = ag->in_esz;
                int     in_uns = ag->in_unsigned;
                for (int i = 0; i < match_count; i++) {
                    int64_t v = read_signed_by_esz(in_base,
                                                   base_row + src_rows[i],
                                                   in_esz, in_uns);
                    int64_t* p = &state[slot_idx[i] * total_state + off];
                    if (v > *p) *p = v;
                }
                break;
            }
            case MK_AGG_AVG: {
                const void* in_base = ag->in_base;
                uint8_t in_esz = ag->in_esz;
                int     in_uns = ag->in_unsigned;
                for (int i = 0; i < match_count; i++) {
                    int64_t v = read_signed_by_esz(in_base,
                                                   base_row + src_rows[i],
                                                   in_esz, in_uns);
                    state[slot_idx[i] * total_state + off    ] += v;
                    state[slot_idx[i] * total_state + off + 1] += 1;
                }
                break;
            }
            }
        }

        row = mend;
    }
}

/* ─── Materialize ───────────────────────────────────────────────────── */

static ray_t* mk_materialize_agg(const mk_agg_t* a, const int64_t* gstate,
                                 const int64_t* gslots, uint64_t gcap,
                                 int64_t global_n, uint8_t total_state)
{
    int8_t out_type;
    bool   is_avg = (a->kind == MK_AGG_AVG);
    if (a->kind == MK_AGG_COUNT)      out_type = RAY_I64;
    else if (is_avg)                  out_type = RAY_F64;
    else if (a->kind == MK_AGG_SUM)   out_type = RAY_I64;
    else                              out_type = a->in_type;  /* MIN/MAX */

    ray_t* col = ray_vec_new(out_type, global_n);
    if (!col || RAY_IS_ERR(col)) return col;
    col->len = global_n;
    void* dst = ray_data(col);
    int64_t gi = 0;
    if (is_avg) {
        double* d = (double*)dst;
        for (uint64_t s = 0; s < gcap; s++) {
            if (!gslots[s * 2]) continue;
            int64_t sum = gstate[s * total_state + a->state_off    ];
            int64_t cnt = gstate[s * total_state + a->state_off + 1];
            d[gi++] = cnt ? (double)sum / (double)cnt : 0.0;
        }
    } else if (out_type == RAY_I64) {
        int64_t* d = (int64_t*)dst;
        for (uint64_t s = 0; s < gcap; s++) {
            if (!gslots[s * 2]) continue;
            d[gi++] = gstate[s * total_state + a->state_off];
        }
    } else {
        for (uint64_t s = 0; s < gcap; s++) {
            if (!gslots[s * 2]) continue;
            int64_t v = gstate[s * total_state + a->state_off];
            write_col_i64(dst, gi++, v, out_type, a->in_attrs);
        }
    }
    return col;
}

/* Parallel combine for the multi-agg/multi-key path.  Same 3-pass radix
 * scatter as count1: histogram per (shard, partition), scatter packed
 * (kv + state[]) to a flat buffer using per-(shard, partition) cursors,
 * dedup per partition.  Final materialize concatenates partition outputs.
 *
 * State per slot is `total_state` int64s, so the packed buffer width
 * is (1 + total_state) int64s per entry: [kv, state_0, state_1, ...]. */
typedef struct {
    mk_shard_t*       shards;
    uint32_t          nw;
    uint8_t           total_state;
    uint8_t           wide;            /* mirror of mk_par_ctx_t::wide */
    const mk_agg_t*   aggs;
    uint8_t           n_aggs;
    uint32_t          p_bits;
    uint64_t          p_mask;
    int64_t*          hist;            /* [nw * P] */
    int64_t*          part_off;        /* [P + 1] */
    int64_t*          sw_cursor;       /* [nw * P] */
    int64_t*          buf;             /* [total_local * stride], stride = (wide?2:1) + total_state */
    /* Per-partition deduped output: kv_lo array, optional kv_hi (wide), state array */
    int64_t**         part_keys;
    int64_t**         part_keys_hi;    /* [P]: NULL when narrow; per-part kv_hi[part_n[p]] */
    int64_t**         part_states;     /* [P]: per-partition state[part_n[p] * total_state] */
    ray_t**           part_keys_hdr;
    ray_t**           part_keys_hi_hdr;
    ray_t**           part_states_hdr;
    int64_t*          part_n;
    _Atomic(uint32_t) oom;
} mk_combine_par_ctx_t;

static void mk_combine_hist_fn(void* vctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    mk_combine_par_ctx_t* c = (mk_combine_par_ctx_t*)vctx;
    uint32_t w = (uint32_t)start;
    mk_shard_t* sh = &c->shards[w];
    int64_t* hist = c->hist + (size_t)w * (c->p_mask + 1);
    if (!sh->slots) return;
    uint8_t wide = c->wide;
    if (!wide) {
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv = sh->slots[s * 2 + 1];
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t p = h & c->p_mask;
            hist[p]++;
        }
    } else {
        const int64_t* slots_hi = sh->slots_hi;
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            uint64_t h = mk_hash_lo_hi(sh->slots[s * 2 + 1], slots_hi[s]);
            uint64_t p = h & c->p_mask;
            hist[p]++;
        }
    }
}

static void mk_combine_scat_fn(void* vctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    mk_combine_par_ctx_t* c = (mk_combine_par_ctx_t*)vctx;
    uint32_t w = (uint32_t)start;
    mk_shard_t* sh = &c->shards[w];
    int64_t* cur = c->sw_cursor + (size_t)w * (c->p_mask + 1);
    uint8_t total_state = c->total_state;
    uint8_t wide = c->wide;
    int64_t kv_off = wide ? 2 : 1;        /* state begins after kv_lo (+ kv_hi if wide) */
    int64_t stride = kv_off + total_state;
    if (!sh->slots) return;
    if (!wide) {
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv = sh->slots[s * 2 + 1];
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t p = h & c->p_mask;
            int64_t pos = cur[p]++;
            int64_t* dst = c->buf + pos * stride;
            dst[0] = kv;
            const int64_t* sst = &sh->state[s * total_state];
            for (uint8_t k = 0; k < total_state; k++) dst[1 + k] = sst[k];
        }
    } else {
        const int64_t* slots_hi = sh->slots_hi;
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv_lo = sh->slots[s * 2 + 1];
            int64_t kv_hi = slots_hi[s];
            uint64_t h = mk_hash_lo_hi(kv_lo, kv_hi);
            uint64_t p = h & c->p_mask;
            int64_t pos = cur[p]++;
            int64_t* dst = c->buf + pos * stride;
            dst[0] = kv_lo;
            dst[1] = kv_hi;
            const int64_t* sst = &sh->state[s * total_state];
            for (uint8_t k = 0; k < total_state; k++) dst[2 + k] = sst[k];
        }
    }
}

static void mk_combine_dedup_fn(void* vctx, uint32_t worker_id,
                                int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    mk_combine_par_ctx_t* c = (mk_combine_par_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    uint64_t p = (uint64_t)start;
    int64_t off  = c->part_off[p];
    int64_t pcnt = c->part_off[p + 1] - off;
    if (pcnt <= 0) {
        c->part_keys[p]    = NULL;
        if (c->part_keys_hi) c->part_keys_hi[p] = NULL;
        c->part_states[p]  = NULL;
        c->part_n[p]       = 0;
        return;
    }

    uint8_t total_state = c->total_state;
    uint8_t wide = c->wide;
    int64_t kv_off = wide ? 2 : 1;
    int64_t stride = kv_off + total_state;

    uint64_t cap = 256;
    while (cap < (uint64_t)(pcnt * 2)) cap <<= 1;
    uint64_t mask = cap - 1;

    ray_t* slots_hdr = NULL;
    ray_t* slots_hi_hdr = NULL;
    ray_t* state_hdr = NULL;
    int64_t* slots = (int64_t*)scratch_calloc(&slots_hdr,
                                              (size_t)cap * 2 * sizeof(int64_t));
    int64_t* slots_hi = wide
        ? (int64_t*)scratch_calloc(&slots_hi_hdr, (size_t)cap * sizeof(int64_t))
        : NULL;
    int64_t* state = (int64_t*)scratch_calloc(&state_hdr,
                                              (size_t)cap * total_state *
                                              sizeof(int64_t));
    if (!slots || !state || (wide && !slots_hi)) {
        if (slots_hdr)    scratch_free(slots_hdr);
        if (slots_hi_hdr) scratch_free(slots_hi_hdr);
        if (state_hdr)    scratch_free(state_hdr);
        atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
        return;
    }

    int64_t n_filled = 0;
    const int64_t* in = c->buf + off * stride;
    if (!wide) {
        for (int64_t i = 0; i < pcnt; i++) {
            const int64_t* ent = in + i * stride;
            int64_t kv = ent[0];
            const int64_t* sst = ent + 1;
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t t = (h >> c->p_bits) & mask;
            for (;;) {
                if (!slots[t * 2]) {
                    slots[t * 2]     = 1;
                    slots[t * 2 + 1] = kv;
                    int64_t* dst = &state[t * total_state];
                    for (uint8_t k = 0; k < total_state; k++) dst[k] = sst[k];
                    n_filled++;
                    break;
                }
                if (slots[t * 2 + 1] == kv) {
                    mk_state_merge(&state[t * total_state],
                                   sst, c->aggs, c->n_aggs);
                    break;
                }
                t = (t + 1) & mask;
            }
        }
    } else {
        for (int64_t i = 0; i < pcnt; i++) {
            const int64_t* ent = in + i * stride;
            int64_t kv_lo = ent[0];
            int64_t kv_hi = ent[1];
            const int64_t* sst = ent + 2;
            uint64_t h = mk_hash_lo_hi(kv_lo, kv_hi);
            uint64_t t = (h >> c->p_bits) & mask;
            for (;;) {
                if (!slots[t * 2]) {
                    slots[t * 2]     = 1;
                    slots[t * 2 + 1] = kv_lo;
                    slots_hi[t]      = kv_hi;
                    int64_t* dst = &state[t * total_state];
                    for (uint8_t k = 0; k < total_state; k++) dst[k] = sst[k];
                    n_filled++;
                    break;
                }
                if (slots[t * 2 + 1] == kv_lo && slots_hi[t] == kv_hi) {
                    mk_state_merge(&state[t * total_state],
                                   sst, c->aggs, c->n_aggs);
                    break;
                }
                t = (t + 1) & mask;
            }
        }
    }

    /* Pack per-partition output. */
    ray_t* k_hdr = NULL;
    ray_t* khi_hdr = NULL;
    ray_t* s_hdr = NULL;
    int64_t* k_out = (int64_t*)scratch_alloc(&k_hdr,
                                             (size_t)n_filled * sizeof(int64_t));
    int64_t* khi_out = wide
        ? (int64_t*)scratch_alloc(&khi_hdr, (size_t)n_filled * sizeof(int64_t))
        : NULL;
    int64_t* s_out = (int64_t*)scratch_alloc(&s_hdr,
                                             (size_t)n_filled * total_state *
                                             sizeof(int64_t));
    if (!k_out || !s_out || (wide && !khi_out)) {
        if (k_hdr)   scratch_free(k_hdr);
        if (khi_hdr) scratch_free(khi_hdr);
        if (s_hdr)   scratch_free(s_hdr);
        scratch_free(slots_hdr);
        if (slots_hi_hdr) scratch_free(slots_hi_hdr);
        scratch_free(state_hdr);
        atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
        return;
    }
    int64_t out_idx = 0;
    for (uint64_t s = 0; s < cap; s++) {
        if (slots[s * 2]) {
            k_out[out_idx] = slots[s * 2 + 1];
            if (wide) khi_out[out_idx] = slots_hi[s];
            int64_t* dst = &s_out[out_idx * total_state];
            const int64_t* src = &state[s * total_state];
            for (uint8_t k = 0; k < total_state; k++) dst[k] = src[k];
            out_idx++;
        }
    }
    scratch_free(slots_hdr);
    if (slots_hi_hdr) scratch_free(slots_hi_hdr);
    scratch_free(state_hdr);

    c->part_keys[p]          = k_out;
    if (c->part_keys_hi)     c->part_keys_hi[p] = khi_out;
    c->part_states[p]        = s_out;
    c->part_keys_hdr[p]      = k_hdr;
    if (c->part_keys_hi_hdr) c->part_keys_hi_hdr[p] = khi_hdr;
    c->part_states_hdr[p]    = s_hdr;
    c->part_n[p]             = n_filled;
}

/* Build a virtual gs/gst pair from per-partition packed outputs so the
 * existing materialize loop (key decompose + agg materialize) keeps
 * working unchanged.  We allocate the dense gs/gst arrays sized to
 * global_n with no empty slots and walk them in order.  When wide,
 * we additionally fill out_gs_hi (one int64 per slot) holding the
 * upper 64 bits of each composite key. */
static int mk_combine_parallel(mk_par_ctx_t* c, uint32_t nw,
                               int64_t** out_gs, ray_t** out_gs_hdr,
                               int64_t** out_gs_hi, ray_t** out_gs_hi_hdr,
                               int64_t** out_gst, ray_t** out_gst_hdr,
                               int64_t* out_gcap, int64_t* out_global_n)
{
    mk_shard_t* shards = c->shards;
    uint8_t total_state = c->total_state;
    uint8_t wide = c->wide;

    int64_t total_local = 0;
    for (uint32_t w = 0; w < nw; w++) total_local += shards[w].n_filled;

    if (total_local < FP_COMBINE_PAR_MIN) return 0;  /* not worth parallelising */

    ray_pool_t* pool = ray_pool_get();
    if (!pool || ray_pool_total_workers(pool) < 2) return 0;
    if (nw > 256) return 0;

    uint32_t cnw = ray_pool_total_workers(pool);
    uint32_t p_bits = (cnw < 8) ? 4 : 6;
    uint64_t P = (uint64_t)1 << p_bits;
    uint64_t p_mask = P - 1;

    ray_t* hist_hdr = NULL;
    ray_t* off_hdr  = NULL;
    ray_t* cur_hdr  = NULL;
    ray_t* buf_hdr  = NULL;
    ray_t* pk_hdr   = NULL;
    ray_t* pkhi_hdr = NULL;
    ray_t* ps_hdr   = NULL;
    ray_t* pkh_hdr  = NULL;
    ray_t* pkhh_hdr = NULL;
    ray_t* psh_hdr  = NULL;
    ray_t* pn_hdr   = NULL;

    int64_t kv_off = wide ? 2 : 1;
    int64_t stride = kv_off + total_state;
    int64_t* hist  = (int64_t*)scratch_calloc(&hist_hdr,
        (size_t)nw * (size_t)P * sizeof(int64_t));
    int64_t* part_off = (int64_t*)scratch_alloc(&off_hdr,
        (size_t)(P + 1) * sizeof(int64_t));
    int64_t* sw_cursor = (int64_t*)scratch_alloc(&cur_hdr,
        (size_t)nw * (size_t)P * sizeof(int64_t));
    int64_t* buf = (int64_t*)scratch_alloc(&buf_hdr,
        (size_t)total_local * (size_t)stride * sizeof(int64_t));
    int64_t** part_keys = (int64_t**)scratch_calloc(&pk_hdr,
        (size_t)P * sizeof(int64_t*));
    int64_t** part_keys_hi = wide
        ? (int64_t**)scratch_calloc(&pkhi_hdr, (size_t)P * sizeof(int64_t*))
        : NULL;
    int64_t** part_states = (int64_t**)scratch_calloc(&ps_hdr,
        (size_t)P * sizeof(int64_t*));
    ray_t**   part_keys_hdr = (ray_t**)scratch_calloc(&pkh_hdr,
        (size_t)P * sizeof(ray_t*));
    ray_t**   part_keys_hi_hdr = wide
        ? (ray_t**)scratch_calloc(&pkhh_hdr, (size_t)P * sizeof(ray_t*))
        : NULL;
    ray_t**   part_states_hdr = (ray_t**)scratch_calloc(&psh_hdr,
        (size_t)P * sizeof(ray_t*));
    int64_t*  part_n = (int64_t*)scratch_calloc(&pn_hdr,
        (size_t)P * sizeof(int64_t));

    if (!hist || !part_off || !sw_cursor || !buf || !part_keys ||
        !part_states || !part_keys_hdr || !part_states_hdr || !part_n
        || (wide && (!part_keys_hi || !part_keys_hi_hdr)))
    {
        if (hist_hdr)  scratch_free(hist_hdr);
        if (off_hdr)   scratch_free(off_hdr);
        if (cur_hdr)   scratch_free(cur_hdr);
        if (buf_hdr)   scratch_free(buf_hdr);
        if (pk_hdr)    scratch_free(pk_hdr);
        if (pkhi_hdr)  scratch_free(pkhi_hdr);
        if (ps_hdr)    scratch_free(ps_hdr);
        if (pkh_hdr)   scratch_free(pkh_hdr);
        if (pkhh_hdr)  scratch_free(pkhh_hdr);
        if (psh_hdr)   scratch_free(psh_hdr);
        if (pn_hdr)    scratch_free(pn_hdr);
        return 0;
    }

    mk_combine_par_ctx_t pctx = {
        .shards             = shards,
        .nw                 = nw,
        .total_state        = total_state,
        .wide               = wide,
        .aggs               = c->aggs,
        .n_aggs             = c->n_aggs,
        .p_bits             = p_bits,
        .p_mask             = p_mask,
        .hist               = hist,
        .part_off           = part_off,
        .sw_cursor          = sw_cursor,
        .buf                = buf,
        .part_keys          = part_keys,
        .part_keys_hi       = part_keys_hi,
        .part_states        = part_states,
        .part_keys_hdr      = part_keys_hdr,
        .part_keys_hi_hdr   = part_keys_hi_hdr,
        .part_states_hdr    = part_states_hdr,
        .part_n             = part_n,
        .oom                = 0,
    };

    ray_pool_dispatch_n(pool, mk_combine_hist_fn, &pctx, nw);

    int64_t total = 0;
    for (uint64_t p = 0; p < P; p++) {
        part_off[p] = total;
        int64_t cum = total;
        for (uint32_t w = 0; w < nw; w++) {
            int64_t cnt = hist[(size_t)w * P + p];
            sw_cursor[(size_t)w * P + p] = cum;
            cum += cnt;
        }
        total = cum;
    }
    part_off[P] = total;

    ray_pool_dispatch_n(pool, mk_combine_scat_fn, &pctx, nw);
    ray_pool_dispatch_n(pool, mk_combine_dedup_fn, &pctx, (uint32_t)P);

    int oom = atomic_load_explicit(&pctx.oom, memory_order_relaxed) ? 1 : 0;
    if (oom) {
        for (uint64_t p = 0; p < P; p++) {
            if (part_keys_hdr[p])    scratch_free(part_keys_hdr[p]);
            if (part_keys_hi_hdr && part_keys_hi_hdr[p])
                scratch_free(part_keys_hi_hdr[p]);
            if (part_states_hdr[p])  scratch_free(part_states_hdr[p]);
        }
        scratch_free(hist_hdr); scratch_free(off_hdr);
        scratch_free(cur_hdr); scratch_free(buf_hdr);
        scratch_free(pk_hdr); if (pkhi_hdr) scratch_free(pkhi_hdr);
        scratch_free(ps_hdr);
        scratch_free(pkh_hdr); if (pkhh_hdr) scratch_free(pkhh_hdr);
        scratch_free(psh_hdr);
        scratch_free(pn_hdr);
        return 0;
    }

    /* Concat per-partition outputs into a dense gs/gst pair so the
     * existing materialize loop in mk_combine_and_materialize works
     * unchanged.  gs is laid out as [used, kv_lo] per slot; when wide,
     * gs_hi[gi] holds the upper 64 bits.  Set every used flag to 1
     * and pack tightly (cap == global_n, no empty slots). */
    int64_t global_n = 0;
    for (uint64_t p = 0; p < P; p++) global_n += part_n[p];

    ray_t* gs_hdr = NULL;
    ray_t* gs_hi_hdr = NULL;
    ray_t* gst_hdr = NULL;
    int64_t* gs  = (int64_t*)scratch_calloc(&gs_hdr,
        (size_t)global_n * 2 * sizeof(int64_t));
    int64_t* gs_hi = wide
        ? (int64_t*)scratch_alloc(&gs_hi_hdr, (size_t)global_n * sizeof(int64_t))
        : NULL;
    int64_t* gst = (int64_t*)scratch_alloc(&gst_hdr,
        (size_t)global_n * total_state * sizeof(int64_t));
    if (!gs || !gst || (wide && !gs_hi)) {
        if (gs_hdr)    scratch_free(gs_hdr);
        if (gs_hi_hdr) scratch_free(gs_hi_hdr);
        if (gst_hdr)   scratch_free(gst_hdr);
        for (uint64_t p = 0; p < P; p++) {
            if (part_keys_hdr[p])    scratch_free(part_keys_hdr[p]);
            if (part_keys_hi_hdr && part_keys_hi_hdr[p])
                scratch_free(part_keys_hi_hdr[p]);
            if (part_states_hdr[p])  scratch_free(part_states_hdr[p]);
        }
        scratch_free(hist_hdr); scratch_free(off_hdr);
        scratch_free(cur_hdr); scratch_free(buf_hdr);
        scratch_free(pk_hdr); if (pkhi_hdr) scratch_free(pkhi_hdr);
        scratch_free(ps_hdr);
        scratch_free(pkh_hdr); if (pkhh_hdr) scratch_free(pkhh_hdr);
        scratch_free(psh_hdr);
        scratch_free(pn_hdr);
        return 0;
    }
    int64_t gi = 0;
    for (uint64_t p = 0; p < P; p++) {
        int64_t  pn = part_n[p];
        int64_t* pk = part_keys[p];
        int64_t* pkhi = part_keys_hi ? part_keys_hi[p] : NULL;
        int64_t* ps = part_states[p];
        for (int64_t i = 0; i < pn; i++) {
            gs[gi * 2]     = 1;
            gs[gi * 2 + 1] = pk[i];
            if (wide) gs_hi[gi] = pkhi[i];
            int64_t* dst = &gst[gi * total_state];
            const int64_t* src = &ps[i * total_state];
            for (uint8_t k = 0; k < total_state; k++) dst[k] = src[k];
            gi++;
        }
        if (part_keys_hdr[p])    scratch_free(part_keys_hdr[p]);
        if (part_keys_hi_hdr && part_keys_hi_hdr[p])
            scratch_free(part_keys_hi_hdr[p]);
        if (part_states_hdr[p])  scratch_free(part_states_hdr[p]);
    }

    scratch_free(hist_hdr); scratch_free(off_hdr);
    scratch_free(cur_hdr); scratch_free(buf_hdr);
    scratch_free(pk_hdr); if (pkhi_hdr) scratch_free(pkhi_hdr);
    scratch_free(ps_hdr);
    scratch_free(pkh_hdr); if (pkhh_hdr) scratch_free(pkhh_hdr);
    scratch_free(psh_hdr);
    scratch_free(pn_hdr);

    *out_gs       = gs;
    *out_gs_hdr   = gs_hdr;
    *out_gs_hi    = gs_hi;
    *out_gs_hi_hdr= gs_hi_hdr;
    *out_gst      = gst;
    *out_gst_hdr  = gst_hdr;
    *out_gcap     = global_n;
    *out_global_n = global_n;
    return 1;
}

static ray_t* mk_combine_and_materialize(mk_par_ctx_t* c, uint32_t nw,
                                         const uint16_t* agg_op_ids)
{
    mk_shard_t* shards = c->shards;
    uint8_t total_state = c->total_state;
    uint8_t n_aggs = c->n_aggs;
    uint8_t n_keys = c->n_keys;
    uint8_t wide   = c->wide;

    int64_t total_local = 0;
    for (uint32_t w = 0; w < nw; w++) total_local += shards[w].n_filled;

    /* Try parallel combine first.  On success, jump straight to the
     * materialize section with the already-built gs/gs_hi/gst arrays. */
    int64_t* gs    = NULL;
    int64_t* gs_hi = NULL;
    int64_t* gst   = NULL;
    ray_t*   gs_hdr    = NULL;
    ray_t*   gs_hi_hdr = NULL;
    ray_t*   gst_hdr   = NULL;
    int64_t  gcap     = 0;
    int64_t  global_n = 0;
    int parallel_ok = mk_combine_parallel(c, nw,
                                          &gs, &gs_hdr,
                                          &gs_hi, &gs_hi_hdr,
                                          &gst, &gst_hdr,
                                          &gcap, &global_n);
    if (parallel_ok) goto materialize;

    {
    uint64_t _gcap = 1024;
    while (_gcap < (uint64_t)(total_local * 2) && _gcap < FP_SHARD_MAX_CAP) _gcap <<= 1;
    gcap = (int64_t)_gcap;
    uint64_t gmask = (uint64_t)gcap - 1;
    gs  = (int64_t*)scratch_calloc(&gs_hdr,  (size_t)gcap * 2 * sizeof(int64_t));
    gst = (int64_t*)scratch_calloc(&gst_hdr, (size_t)gcap * total_state * sizeof(int64_t));
    if (wide) {
        gs_hi = (int64_t*)scratch_calloc(&gs_hi_hdr, (size_t)gcap * sizeof(int64_t));
    }
    if (!gs || !gst || (wide && !gs_hi)) {
        if (gs_hdr)    scratch_free(gs_hdr);
        if (gs_hi_hdr) scratch_free(gs_hi_hdr);
        if (gst_hdr)   scratch_free(gst_hdr);
        return ray_error("oom", NULL);
    }
    global_n = 0;
    if (!wide) {
        for (uint32_t w = 0; w < nw; w++) {
            mk_shard_t* sh = &shards[w];
            if (!sh->slots) continue;
            for (uint64_t s = 0; s < sh->cap; s++) {
                if (!sh->slots[s * 2]) continue;
                int64_t kv = sh->slots[s * 2 + 1];
                uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
                h ^= h >> 33;
                uint64_t t = h & gmask;
                for (;;) {
                    if (!gs[t * 2]) {
                        gs[t * 2] = 1; gs[t * 2 + 1] = kv;
                        for (uint8_t k = 0; k < total_state; k++)
                            gst[t * total_state + k] = sh->state[s * total_state + k];
                        global_n++;
                        break;
                    }
                    if (gs[t * 2 + 1] == kv) {
                        mk_state_merge(&gst[t * total_state],
                                       &sh->state[s * total_state],
                                       c->aggs, n_aggs);
                        break;
                    }
                    t = (t + 1) & gmask;
                }
            }
        }
    } else {
        for (uint32_t w = 0; w < nw; w++) {
            mk_shard_t* sh = &shards[w];
            if (!sh->slots) continue;
            const int64_t* slots_hi = sh->slots_hi;
            for (uint64_t s = 0; s < sh->cap; s++) {
                if (!sh->slots[s * 2]) continue;
                int64_t kv_lo = sh->slots[s * 2 + 1];
                int64_t kv_hi = slots_hi[s];
                uint64_t h = mk_hash_lo_hi(kv_lo, kv_hi);
                uint64_t t = h & gmask;
                for (;;) {
                    if (!gs[t * 2]) {
                        gs[t * 2] = 1; gs[t * 2 + 1] = kv_lo;
                        gs_hi[t] = kv_hi;
                        for (uint8_t k = 0; k < total_state; k++)
                            gst[t * total_state + k] = sh->state[s * total_state + k];
                        global_n++;
                        break;
                    }
                    if (gs[t * 2 + 1] == kv_lo && gs_hi[t] == kv_hi) {
                        mk_state_merge(&gst[t * total_state],
                                       &sh->state[s * total_state],
                                       c->aggs, n_aggs);
                        break;
                    }
                    t = (t + 1) & gmask;
                }
            }
        }
    }
    }

materialize:

    /* Build n_keys key columns by decomposing the composite. */
    ray_t* key_cols[FP_MAX_KEYS];
    for (uint8_t k = 0; k < n_keys; k++) key_cols[k] = NULL;
    int keys_ok = 1;
    for (uint8_t k = 0; k < n_keys; k++) {
        const mk_key_t* kk = &c->keys[k];
        ray_t* col = (kk->type == RAY_SYM)
                   ? ray_sym_vec_new(kk->attrs & RAY_SYM_W_MASK, global_n)
                   : ray_vec_new(kk->type, global_n);
        if (!col || RAY_IS_ERR(col)) { keys_ok = 0; break; }
        col->len = global_n;
        key_cols[k] = col;
    }
    if (!keys_ok) {
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_cols[k] && !RAY_IS_ERR(key_cols[k])) ray_release(key_cols[k]);
        scratch_free(gs_hdr); if (gs_hi_hdr) scratch_free(gs_hi_hdr);
        scratch_free(gst_hdr);
        return ray_error("oom", NULL);
    }
    int64_t gi = 0;
    for (int64_t s = 0; s < gcap; s++) {
        if (!gs[s * 2]) continue;
        uint64_t lo = (uint64_t)gs[s * 2 + 1];
        uint64_t hi = wide ? (uint64_t)gs_hi[s] : 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            const mk_key_t* kk = &c->keys[k];
            uint8_t bit_off = kk->bit_off;
            uint8_t bit_end = (uint8_t)(bit_off + kk->esz * 8);
            int64_t v;
            if (kk->esz >= 8 && bit_off == 0) {
                v = (int64_t)lo;
            } else if (kk->esz >= 8 && bit_off == 64) {
                v = (int64_t)hi;
            } else if (bit_end <= 64) {
                /* Fits entirely in lo half. */
                uint64_t mask = (kk->esz >= 8) ? ~0ULL
                                               : ((1ULL << (kk->esz * 8)) - 1);
                v = (int64_t)((lo >> bit_off) & mask);
            } else if (bit_off >= 64) {
                /* Fits entirely in hi half. */
                uint64_t mask = ((1ULL << (kk->esz * 8)) - 1);
                v = (int64_t)((hi >> (bit_off - 64)) & mask);
            } else {
                /* Spans the 64-bit boundary — stitch. */
                uint8_t lo_bits = (uint8_t)(64 - bit_off);
                uint64_t lo_mask = (lo_bits == 64) ? ~0ULL : ((1ULL << lo_bits) - 1);
                uint64_t lo_part = (lo >> bit_off) & lo_mask;
                uint64_t hi_part = hi & ((1ULL << (kk->esz * 8 - lo_bits)) - 1);
                v = (int64_t)(lo_part | (hi_part << lo_bits));
            }
            write_col_i64(ray_data(key_cols[k]), gi, v, kk->type, kk->attrs);
        }
        gi++;
    }

    /* Build agg columns. */
    ray_t* agg_cols[FP_MAX_AGGS];
    for (uint8_t i = 0; i < n_aggs; i++) agg_cols[i] = NULL;
    int build_ok = 1;
    for (uint8_t i = 0; i < n_aggs; i++) {
        agg_cols[i] = mk_materialize_agg(&c->aggs[i], gst, gs, gcap,
                                         global_n, total_state);
        if (!agg_cols[i] || RAY_IS_ERR(agg_cols[i])) { build_ok = 0; break; }
    }
    scratch_free(gs_hdr); if (gs_hi_hdr) scratch_free(gs_hi_hdr);
    scratch_free(gst_hdr);
    if (!build_ok) {
        for (uint8_t k = 0; k < n_keys; k++) ray_release(key_cols[k]);
        for (uint8_t i = 0; i < n_aggs; i++)
            if (agg_cols[i] && !RAY_IS_ERR(agg_cols[i])) ray_release(agg_cols[i]);
        return ray_error("oom", NULL);
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        for (uint8_t k = 0; k < n_keys; k++) ray_release(key_cols[k]);
        for (uint8_t i = 0; i < n_aggs; i++) ray_release(agg_cols[i]);
        return ray_error("oom", NULL);
    }
    for (uint8_t k = 0; k < n_keys; k++)
        result = ray_table_add_col(result, c->keys[k].sym, key_cols[k]);
    for (uint8_t i = 0; i < n_aggs; i++) {
        int64_t name;
        switch (agg_op_ids[i]) {
        case OP_COUNT: name = ray_sym_intern("count", 5); break;
        case OP_SUM:   name = ray_sym_intern("sum",   3); break;
        case OP_MIN:   name = ray_sym_intern("min",   3); break;
        case OP_MAX:   name = ray_sym_intern("max",   3); break;
        case OP_AVG:   name = ray_sym_intern("avg",   3); break;
        default:       name = ray_sym_intern("agg",   3); break;
        }
        result = ray_table_add_col(result, name, agg_cols[i]);
    }
    for (uint8_t k = 0; k < n_keys; k++) ray_release(key_cols[k]);
    for (uint8_t i = 0; i < n_aggs; i++) ray_release(agg_cols[i]);
    return result;
}

/* ─── Compile + exec ────────────────────────────────────────────────── */

static int mk_compile(ray_graph_t* g, ray_op_ext_t* ext, ray_t* tbl,
                      mk_par_ctx_t* ctx)
{
    if (ext->n_aggs == 0 || ext->n_aggs > FP_MAX_AGGS) return -1;
    if (ext->n_keys == 0 || ext->n_keys > FP_MAX_KEYS) return -1;
    /* Aggs */
    uint8_t state_off = 0;
    for (uint8_t i = 0; i < ext->n_aggs; i++) {
        mk_agg_t* a = &ctx->aggs[i];
        memset(a, 0, sizeof(*a));
        switch (ext->agg_ops[i]) {
        case OP_COUNT: a->kind = MK_AGG_COUNT; break;
        case OP_SUM:   a->kind = MK_AGG_SUM;   break;
        case OP_MIN:   a->kind = MK_AGG_MIN;   break;
        case OP_MAX:   a->kind = MK_AGG_MAX;   break;
        case OP_AVG:   a->kind = MK_AGG_AVG;   break;
        default: return -1;
        }
        a->state_off = state_off;
        state_off += (a->kind == MK_AGG_AVG) ? 2 : 1;
        if (a->kind == MK_AGG_COUNT) { a->in_type = -1; continue; }
        ray_op_t* in_op = ext->agg_ins[i];
        if (!in_op || in_op->opcode != OP_SCAN) return -1;
        ray_op_ext_t* iext = find_ext(g, in_op->id);
        if (!iext) return -1;
        ray_t* col = ray_table_get_col(tbl, iext->sym);
        if (!col) return -1;
        if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return -1;
        /* Aggregate inputs cannot carry nulls — the per-row read in
         * mk_state_init_row / mk_state_accum_row treats every slot as
         * a real value, so a stored sentinel for null would corrupt
         * SUM / MIN / MAX / AVG.  Bail to OP_GROUP, which has the
         * null-aware aggregate kernels. */
        if (col->attrs & RAY_ATTR_HAS_NULLS) return -1;
        int8_t ct = col->type;
        if (ct != RAY_BOOL && ct != RAY_U8 && ct != RAY_I16
            && ct != RAY_I32 && ct != RAY_I64
            && ct != RAY_DATE && ct != RAY_TIME && ct != RAY_TIMESTAMP)
            return -1;
        a->in_type = ct;
        a->in_attrs = col->attrs;
        a->in_esz = ray_sym_elem_size(ct, col->attrs);
        a->in_base = ray_data(col);
        a->in_unsigned = (ct == RAY_BOOL || ct == RAY_U8) ? 1 : 0;
    }
    ctx->total_state = state_off;
    ctx->n_aggs = ext->n_aggs;

    /* Keys.  Total composite width up to 16 bytes — narrow path uses an
     * 8-byte int64 slot, wide path adds a side-array kv_hi for bytes
     * 9..16.  Wider composites bail to the regular OP_GROUP path. */
    uint8_t bit_off = 0;
    int total_bytes = 0;
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key_op = ext->keys[k];
        if (!key_op || key_op->opcode != OP_SCAN) return -1;
        ray_op_ext_t* kext = find_ext(g, key_op->id);
        if (!kext) return -1;
        ray_t* col = ray_table_get_col(tbl, kext->sym);
        if (!col) return -1;
        if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return -1;
        /* Group keys cannot carry nulls — the composite key compose
         * reads raw bytes into the int64 slot, so a sentinel value for
         * null collides with a legitimate row that happens to hold the
         * same bit pattern.  Bail to OP_GROUP, which groups null keys
         * via a dedicated null bucket. */
        if (col->attrs & RAY_ATTR_HAS_NULLS) return -1;
        uint8_t esz = ray_sym_elem_size(col->type, col->attrs);
        total_bytes += esz;
        if (total_bytes > 16) return -1;
        ctx->keys[k].type    = col->type;
        ctx->keys[k].attrs   = col->attrs;
        ctx->keys[k].esz     = esz;
        ctx->keys[k].bit_off = bit_off;
        ctx->keys[k].base    = ray_data(col);
        ctx->keys[k].sym     = kext->sym;
        bit_off += (uint8_t)(esz * 8);
    }
    ctx->n_keys = ext->n_keys;
    ctx->wide   = (total_bytes > 8) ? 1 : 0;
    ctx->k0_type  = ctx->keys[0].type;
    ctx->k0_attrs = ctx->keys[0].attrs;
    ctx->k0_esz   = ctx->keys[0].esz;
    ctx->k0_base  = ctx->keys[0].base;
    return 0;
}

static ray_t* exec_filtered_group_multi(ray_graph_t* g, ray_op_ext_t* ext,
                                        ray_t* tbl)
{
    ray_op_t* pred_op = ext->base.inputs[0];
    mk_par_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (fp_compile_pred(g, pred_op, tbl, &ctx.pred) != 0)
        return ray_error("nyi", "fused_group: predicate shape unsupported");
    if (mk_compile(g, ext, tbl, &ctx) != 0)
        return ray_error("nyi", "fused_group: agg/key shape unsupported");

    int64_t nrows = -1;
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]->id);
        if (kext) {
            ray_t* col = ray_table_get_col(tbl, kext->sym);
            if (col && nrows < 0) { nrows = col->len; break; }
        }
    }
    if (nrows < 0) return ray_error("nyi", NULL);

    ctx.init_cap = FP_SHARD_INIT_CAP;
    atomic_store_explicit(&ctx.oom, 0, memory_order_relaxed);
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
    ray_t* shards_hdr = NULL;
    ctx.shards = (mk_shard_t*)scratch_calloc(&shards_hdr,
                                             (size_t)nw * sizeof(mk_shard_t));
    if (!ctx.shards) return ray_error("oom", NULL);

    if (pool) ray_pool_dispatch(pool, mk_par_fn, &ctx, nrows);
    else      mk_par_fn(&ctx, 0, 0, nrows);

    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        for (uint32_t w = 0; w < nw; w++) mk_shard_free(&ctx.shards[w]);
        scratch_free(shards_hdr);
        return ray_error("oom", "fused_group: shard OOM");
    }

    ray_t* result = mk_combine_and_materialize(&ctx, nw, ext->agg_ops);
    for (uint32_t w = 0; w < nw; w++) mk_shard_free(&ctx.shards[w]);
    scratch_free(shards_hdr);
    return result;
}

/* ─── Public entry: dispatcher ──────────────────────────────────────── */

/* Graceful fallback: build a freshly-constructed FILTER + GROUP
 * subgraph from the inputs already on the fused node and execute it
 * via the regular DAG path.  Defense-in-depth — the planner gate
 * should be tight enough that the fused exec never sees an
 * unsupported shape, but if a future change introduces a divergence
 * we degrade to a slower-but-correct result instead of surfacing a
 * user-visible "nyi" error.
 *
 * The original predicate / keys / agg-input ops are still present in
 * the graph (we don't tear down the OP_FILTERED_GROUP node), so the
 * unfused subgraph just references them. */
static ray_t* exec_filtered_group_fallback(ray_graph_t* g, ray_op_ext_t* ext) {
    if (!g || !ext) return ray_error("nyi", NULL);
    ray_op_t* root = ray_const_table(g, g->table);
    if (!root) return ray_error("oom", NULL);
    ray_op_t* pred = ext->base.inputs[0];
    if (pred) {
        root = ray_filter(g, root, pred);
        if (!root) return ray_error("oom", NULL);
    }
    root = ray_group(g, ext->keys, ext->n_keys,
                     ext->agg_ops, ext->agg_ins, ext->n_aggs);
    if (!root) return ray_error("oom", NULL);
    return ray_execute(g, root);
}

ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op) {
    if (!g || !op) return ray_error("nyi", NULL);
    ray_t* tbl = g->table;
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    /* count1 fast path: single key, single OP_COUNT.  Unchanged from
     * Phase 3 — guarantees zero regression on Q8/Q37/Q38/Q43.
     * If the fused exec rejects the shape (planner / executor gate
     * divergence), fall back to the unfused FILTER + GROUP subgraph. */
    ray_t* res;
    if (ext->n_keys == 1 && ext->n_aggs == 1
        && ext->agg_ops[0] == OP_COUNT)
    {
        res = exec_filtered_group_count1(g, ext, tbl);
    } else {
        res = exec_filtered_group_multi(g, ext, tbl);
    }
    if (res && RAY_IS_ERR(res)) {
        const char* code = ray_err_code(res);
        if (code && strcmp(code, "nyi") == 0) {
            ray_release(res);
            return exec_filtered_group_fallback(g, ext);
        }
    }
    return res;
}

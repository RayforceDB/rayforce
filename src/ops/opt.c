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

#if !defined(RAY_OS_WINDOWS) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "opt.h"
#include "core/profile.h"
#include "mem/sys.h"
#include "mem/heap.h"
#include <math.h>
#include <string.h>

/* Forward declaration — defined below, used by type inference and DCE passes. */
static ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id);

/* --------------------------------------------------------------------------
 * Optimizer passes (v1): Type Inference + Constant Folding + Fusion + DCE
 *
 * Per the spec's staged rollout:
 *   v1: Type Inference + Constant Folding + Fusion + DCE
 *   v2: Predicate/Projection Pushdown + CSE (future)
 *   v3: Op Reordering + Join Optimization (future)
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Pass 1: Type inference (bottom-up)
 *
 * Most type inference is done during graph construction (graph.c).
 * This pass validates and propagates any missing types.
 * -------------------------------------------------------------------------- */

static int8_t promote_type(int8_t a, int8_t b) {
    if (a == RAY_STR || b == RAY_STR) return RAY_STR;
    if (a == RAY_F64 || b == RAY_F64) return RAY_F64;
    /* Treat SYM/TIMESTAMP/DATE/TIME as integer-class types */
    if (a == RAY_I64 || b == RAY_I64 || a == RAY_SYM || b == RAY_SYM ||
        a == RAY_TIMESTAMP || b == RAY_TIMESTAMP) return RAY_I64;
    if (a == RAY_I32 || b == RAY_I32 ||
        a == RAY_DATE || b == RAY_DATE || a == RAY_TIME || b == RAY_TIME) return RAY_I32;
    if (a == RAY_I16 || b == RAY_I16) return RAY_I16;
    if (a == RAY_U8 || b == RAY_U8) return RAY_U8;
    return RAY_BOOL;
}

static void infer_type_for_node(ray_op_t* node) {
    if (node->out_type == 0 && node->opcode != OP_SCAN && node->opcode != OP_CONST) {
        /* Comparison and boolean ops always produce BOOL */
        if (node->opcode >= OP_EQ && node->opcode <= OP_GE) {
            node->out_type = RAY_BOOL;
            return;
        }
        if (node->opcode == OP_AND || node->opcode == OP_OR) {
            node->out_type = RAY_BOOL;
            return;
        }
        if (node->arity >= 2 && node->inputs[0] && node->inputs[1]) {
            node->out_type = promote_type(node->inputs[0]->out_type,
                                           node->inputs[1]->out_type);
        } else if (node->arity >= 1 && node->inputs[0]) {
            node->out_type = node->inputs[0]->out_type;
        }
    }
}

static void pass_type_inference(ray_graph_t* g, ray_op_t* root) {
    if (!root || root->flags & OP_FLAG_DEAD) return;

    /* Iterative post-order: collect nodes into an order array, then
       process in reverse (children before parents). */
    uint32_t nc = g->node_count;
    uint32_t stack_cap = nc * 2 + 64;  /* extra space for high fan-out nodes */
    uint32_t stack_local[256], order_local[256];
    bool visited_stack[256];
    uint32_t *stack = stack_cap <= 256 ? stack_local : (uint32_t*)ray_sys_alloc(stack_cap * sizeof(uint32_t));
    uint32_t *order = nc <= 256 ? order_local : (uint32_t*)ray_sys_alloc(nc * sizeof(uint32_t));
    bool* visited;
    if (nc <= 256) {
        visited = visited_stack;
    } else {
        visited = (bool*)ray_sys_alloc(nc * sizeof(bool));
    }
    if (!stack || !order || !visited) {
        { if (stack_cap > 256) ray_sys_free(stack); if (nc > 256) { ray_sys_free(order); ray_sys_free(visited); } }
        return;
    }
    memset(visited, 0, nc * sizeof(bool));

    int sp = 0, oc = 0;
    stack[sp++] = root->id;
    while (sp > 0 && oc < (int)nc) {
        uint32_t nid = stack[--sp];
        ray_op_t* n = &g->nodes[nid];
        if (!n || n->flags & OP_FLAG_DEAD) continue;
        if (visited[nid]) continue;
        visited[nid] = true;
        order[oc++] = nid;
        for (int i = 0; i < 2 && i < n->arity; i++) {
            if (n->inputs[i] && sp < (int)stack_cap)
                stack[sp++] = n->inputs[i]->id;
        }
        /* M3: Traverse ext node children so type inference reaches all
           referenced nodes (GROUP keys/aggs, SORT/PROJECT/SELECT columns,
           JOIN keys, WINDOW partition/order/func_inputs). */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint8_t k = 0; k < ext->n_keys; k++)
                        if (ext->keys[k] && !visited[ext->keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->keys[k]->id;
                    for (uint8_t a = 0; a < ext->n_aggs; a++)
                        if (ext->agg_ins[a] && !visited[ext->agg_ins[a]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->agg_ins[a]->id;
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint8_t k = 0; k < ext->sort.n_cols; k++)
                        if (ext->sort.columns[k] && !visited[ext->sort.columns[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->sort.columns[k]->id;
                    break;
                case OP_JOIN:
                    for (uint8_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (ext->join.left_keys[k] && !visited[ext->join.left_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.left_keys[k]->id;
                        if (ext->join.right_keys && ext->join.right_keys[k] &&
                            !visited[ext->join.right_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.right_keys[k]->id;
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (wj_ext->asof.time_key && !visited[wj_ext->asof.time_key->id] && sp < (int)stack_cap)
                            stack[sp++] = wj_ext->asof.time_key->id;
                        for (uint8_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (wj_ext->asof.eq_keys[k] && !visited[wj_ext->asof.eq_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.eq_keys[k]->id;
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint8_t k = 0; k < ext->window.n_part_keys; k++)
                        if (ext->window.part_keys[k] && !visited[ext->window.part_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.part_keys[k]->id;
                    for (uint8_t k = 0; k < ext->window.n_order_keys; k++)
                        if (ext->window.order_keys[k] && !visited[ext->window.order_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.order_keys[k]->id;
                    for (uint8_t f = 0; f < ext->window.n_funcs; f++)
                        if (ext->window.func_inputs[f] && !visited[ext->window.func_inputs[f]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.func_inputs[f]->id;
                    break;
                /* M3b: 3-input ops store third operand node ID in ext->literal */
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                    if (third_id < nc && !visited[third_id] && sp < (int)stack_cap)
                        stack[sp++] = third_id;
                    break;
                }
                /* M3c: OP_CONCAT trailing arg node IDs beyond inputs[0..1] */
                case OP_CONCAT:
                    if (ext->sym >= 2) {
                        int n_args = (int)ext->sym;
                        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                        for (int j = 2; j < n_args; j++) {
                            uint32_t arg_id = trail[j - 2];
                            if (arg_id < nc && !visited[arg_id] && sp < (int)stack_cap)
                                stack[sp++] = arg_id;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    /* Process in reverse order (children before parents) */
    for (int i = oc - 1; i >= 0; i--)
        infer_type_for_node(&g->nodes[order[i]]);

    { if (stack_cap > 256) ray_sys_free(stack); if (nc > 256) { ray_sys_free(order); ray_sys_free(visited); } }
}

/* --------------------------------------------------------------------------
 * Pass 2: Constant folding
 *
 * If all inputs to an element-wise op are OP_CONST, evaluate immediately
 * and replace the node with a new OP_CONST.
 * -------------------------------------------------------------------------- */

static bool is_const(ray_op_t* n) {
    return n && n->opcode == OP_CONST;
}

/* O(ext_count) per call; acceptable for typical graph sizes (tens to
   hundreds of nodes).  L2: intentional duplication to keep files
   self-contained — also present in fuse.c. */
static ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == node_id)
            return g->ext_nodes[i];
    }
    return NULL;
}

static bool track_ext_node(ray_graph_t* g, ray_op_ext_t* ext) {
    if (g->ext_count >= g->ext_cap) {
        if (g->ext_cap > UINT32_MAX / 2) return false;
        uint32_t new_cap = g->ext_cap == 0 ? 16 : g->ext_cap * 2;
        ray_op_ext_t** new_exts =
            (ray_op_ext_t**)ray_sys_realloc(g->ext_nodes, new_cap * sizeof(ray_op_ext_t*));
        if (!new_exts) return false;
        g->ext_nodes = new_exts;
        g->ext_cap = new_cap;
    }
    g->ext_nodes[g->ext_count++] = ext;
    return true;
}

static ray_op_ext_t* ensure_ext_node(ray_graph_t* g, uint32_t node_id) {
    ray_op_ext_t* ext = find_ext(g, node_id);
    if (ext) return ext;

    ext = (ray_op_ext_t*)ray_sys_alloc(sizeof(ray_op_ext_t));
    if (!ext) return NULL;
    /* M1: Zero-init to prevent use of uninitialized fields (literal,
       keys, agg_ins, etc.) before the caller populates them. */
    memset(ext, 0, sizeof(*ext));
    ext->base.id = node_id;
    if (!track_ext_node(g, ext)) {
        ray_sys_free(ext);
        return NULL;
    }
    return ext;
}

static bool atom_to_numeric(ray_t* v, double* out_f, int64_t* out_i, bool* is_f64) {
    if (!v || !ray_is_atom(v)) return false;
    if (RAY_ATOM_IS_NULL(v)) return false;
    switch (v->type) {
        case -RAY_F64:
            *out_f = v->f64;
            *out_i = (int64_t)v->f64;
            *is_f64 = true;
            return true;
        case -RAY_I64:
        case -RAY_SYM:
        case -RAY_DATE:
        case -RAY_TIME:
        case -RAY_TIMESTAMP:
            *out_i = v->i64;
            *out_f = (double)v->i64;
            *is_f64 = false;
            return true;
        case -RAY_I32:
            *out_i = (int64_t)v->i32;
            *out_f = (double)v->i32;
            *is_f64 = false;
            return true;
        case -RAY_I16:
            *out_i = (int64_t)v->i16;
            *out_f = (double)v->i16;
            *is_f64 = false;
            return true;
        case -RAY_U8:
        case -RAY_BOOL:
            *out_i = (int64_t)v->u8;
            *out_f = (double)v->u8;
            *is_f64 = false;
            return true;
        default:
            return false;
    }
}

static bool replace_with_const(ray_graph_t* g, ray_op_t* node, ray_t* literal) {
    /* H3: If the node already has an ext node (GROUP, SORT, JOIN, etc.),
       skip constant replacement — overwriting the ext union would clobber
       structural data.  Structural ops should never be constant-folded. */
    if (find_ext(g, node->id)) return false;

    ray_op_ext_t* ext = ensure_ext_node(g, node->id);
    if (!ext) return false;

    ext->base = *node;
    ext->base.opcode = OP_CONST;
    ext->base.arity = 0;
    ext->base.inputs[0] = NULL;
    ext->base.inputs[1] = NULL;
    ext->base.flags &= (uint8_t)~OP_FLAG_FUSED;
    ext->base.out_type = literal->type < 0 ? (int8_t)(-(int)literal->type) : literal->type;
    ext->literal = literal;

    *node = ext->base;
    g->nodes[node->id] = ext->base;
    return true;
}

static bool fold_unary_const(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* operand = node->inputs[0];
    if (!is_const(operand)) return false;

    ray_op_ext_t* oe = find_ext(g, operand->id);
    if (!oe || !oe->literal || !ray_is_atom(oe->literal)) return false;

    double vf = 0.0;
    int64_t vi = 0;
    bool is_f64 = false;
    if (!atom_to_numeric(oe->literal, &vf, &vi, &is_f64)) return false;

    ray_t* folded = NULL;
    switch (node->opcode) {
        case OP_NEG:
            if (is_f64) folded = ray_f64(-vf);
            else if (vi == INT64_MIN) return false;  /* -INT64_MIN overflows */
            else folded = ray_i64(-vi);
            break;
        case OP_ABS:
            if (is_f64)
                folded = ray_f64(fabs(vf));
            else if (vi == INT64_MIN) return false;  /* -INT64_MIN overflows */
            else folded = ray_i64(vi < 0 ? -vi : vi);
            break;
        case OP_NOT:
            folded = ray_bool(is_f64 ? vf == 0.0 : vi == 0);
            break;
        case OP_SQRT:
            folded = ray_f64(sqrt(is_f64 ? vf : (double)vi));
            break;
        case OP_LOG:
            folded = ray_f64(log(is_f64 ? vf : (double)vi));
            break;
        case OP_EXP:
            folded = ray_f64(exp(is_f64 ? vf : (double)vi));
            break;
        case OP_CEIL:
            folded = is_f64 ? ray_f64(ceil(vf)) : ray_i64(vi);
            break;
        case OP_FLOOR:
            folded = is_f64 ? ray_f64(floor(vf)) : ray_i64(vi);
            break;
        default:
            return false;
    }

    if (!folded || RAY_IS_ERR(folded)) return false;
    if (!replace_with_const(g, node, folded)) {
        ray_release(folded);
        return false;
    }
    return true;
}

static bool fold_binary_const(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* lhs = node->inputs[0];
    ray_op_t* rhs = node->inputs[1];
    if (!is_const(lhs) || !is_const(rhs)) return false;

    ray_op_ext_t* le = find_ext(g, lhs->id);
    ray_op_ext_t* re = find_ext(g, rhs->id);
    if (!le || !re || !le->literal || !re->literal) return false;
    if (!ray_is_atom(le->literal) || !ray_is_atom(re->literal)) return false;

    double lf = 0.0, rf = 0.0;
    int64_t li = 0, ri = 0;
    bool l_is_f64 = false, r_is_f64 = false;
    if (!atom_to_numeric(le->literal, &lf, &li, &l_is_f64)) return false;
    if (!atom_to_numeric(re->literal, &rf, &ri, &r_is_f64)) return false;

    ray_t* folded = NULL;
    switch (node->out_type) {
        case RAY_F64: {
            double lv = l_is_f64 ? lf : (double)li;
            double rv = r_is_f64 ? rf : (double)ri;
            double r = 0.0;
            switch (node->opcode) {
                case OP_ADD: r = lv + rv; break;
                case OP_SUB: r = lv - rv; break;
                case OP_MUL: r = lv * rv; break;
                case OP_DIV: r = lv / rv; break;  /* IEEE 754: ±Inf or NaN */
                case OP_MOD: r = fmod(lv, rv); break;  /* IEEE 754: NaN for rv==0 */
                case OP_MIN2: r = fmin(lv, rv); break;  /* NaN-propagating */
                case OP_MAX2: r = fmax(lv, rv); break;  /* NaN-propagating */
                default: return false;
            }
            folded = ray_f64(r);
            break;
        }
        case RAY_I64: {
            int64_t lv = l_is_f64 ? (int64_t)lf : li;
            int64_t rv = r_is_f64 ? (int64_t)rf : ri;
            int64_t r = 0;
            switch (node->opcode) {
                case OP_ADD: r = (int64_t)((uint64_t)lv + (uint64_t)rv); break;
                case OP_SUB: r = (int64_t)((uint64_t)lv - (uint64_t)rv); break;
                case OP_MUL: r = (int64_t)((uint64_t)lv * (uint64_t)rv); break;
                case OP_DIV:
                    r = (rv != 0 && !(lv == INT64_MIN && rv == -1)) ? lv / rv : 0;
                    break;
                case OP_MOD:
                    r = (rv != 0 && !(lv == INT64_MIN && rv == -1)) ? lv % rv : 0;
                    break;
                case OP_MIN2: r = lv < rv ? lv : rv; break;
                case OP_MAX2: r = lv > rv ? lv : rv; break;
                default: return false;
            }
            folded = ray_i64(r);
            break;
        }
        case RAY_BOOL: {
            /* NaN comparison follows IEEE 754; SQL NULL handled separately
               in executor. */
            double lv = l_is_f64 ? lf : (double)li;
            double rv = r_is_f64 ? rf : (double)ri;
            bool r = false;
            switch (node->opcode) {
                case OP_EQ:  r = lv == rv; break;
                case OP_NE:  r = lv != rv; break;
                case OP_LT:  r = lv < rv; break;
                case OP_LE:  r = lv <= rv; break;
                case OP_GT:  r = lv > rv; break;
                case OP_GE:  r = lv >= rv; break;
                case OP_AND: r = (lv != 0.0) && (rv != 0.0); break;
                case OP_OR:  r = (lv != 0.0) || (rv != 0.0); break;
                default: return false;
            }
            folded = ray_bool(r);
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            int32_t lv = (int32_t)(l_is_f64 ? (int64_t)lf : li);
            int32_t rv = (int32_t)(r_is_f64 ? (int64_t)rf : ri);
            int32_t r = 0;
            switch (node->opcode) {
                case OP_ADD: r = (int32_t)((uint32_t)lv + (uint32_t)rv); break;
                case OP_SUB: r = (int32_t)((uint32_t)lv - (uint32_t)rv); break;
                case OP_MUL: r = (int32_t)((uint32_t)lv * (uint32_t)rv); break;
                case OP_DIV:
                    r = (rv != 0 && !(lv == INT32_MIN && rv == -1)) ? lv / rv : 0;
                    break;
                case OP_MOD:
                    r = (rv != 0 && !(lv == INT32_MIN && rv == -1)) ? lv % rv : 0;
                    break;
                case OP_MIN2: r = lv < rv ? lv : rv; break;
                case OP_MAX2: r = lv > rv ? lv : rv; break;
                default: return false;
            }
            folded = ray_i32(r);
            break;
        }
        default:
            return false;
    }

    if (!folded || RAY_IS_ERR(folded)) return false;
    if (!replace_with_const(g, node, folded)) {
        ray_release(folded);
        return false;
    }
    return true;
}

static bool atom_to_bool(ray_t* v, bool* out) {
    double vf = 0.0;
    int64_t vi = 0;
    bool is_f64 = false;
    if (!atom_to_numeric(v, &vf, &vi, &is_f64)) return false;
    if (is_f64) {
        *out = vf != 0.0;
    } else {
        *out = vi != 0;
    }
    return true;
}

static bool fold_filter_const_predicate(ray_graph_t* g, ray_op_t* node) {
    if (node->opcode != OP_FILTER || node->arity != 2) return false;
    ray_op_t* pred = node->inputs[1];
    if (!is_const(pred)) return false;

    ray_op_ext_t* pred_ext = find_ext(g, pred->id);
    if (!pred_ext || !pred_ext->literal || !ray_is_atom(pred_ext->literal)) return false;

    bool keep_rows = false;
    if (!atom_to_bool(pred_ext->literal, &keep_rows)) return false;

    if (keep_rows) {
        node->opcode = OP_MATERIALIZE;
        node->arity = 1;
        node->inputs[1] = NULL;
        node->flags &= (uint8_t)~OP_FLAG_FUSED;
        g->nodes[node->id] = *node;
        return true;
    }

    ray_op_ext_t* ext = ensure_ext_node(g, node->id);
    if (!ext) return false;
    ext->base = *node;
    ext->base.opcode = OP_HEAD;
    ext->base.arity = 1;
    ext->base.inputs[1] = NULL;
    ext->base.est_rows = 0;
    ext->base.flags &= (uint8_t)~OP_FLAG_FUSED;
    ext->sym = 0;

    *node = ext->base;
    g->nodes[node->id] = ext->base;
    return true;
}

/* Fold reduction(OP_TIL(n)) → closed-form result.
 * sum(0..n-1) = n*(n-1)/2,  min(0..n-1) = 0,  max(0..n-1) = n-1,
 * count(0..n-1) = n,  avg(0..n-1) = (n-1)/2.0 */
static bool fold_reduction_til(ray_graph_t* g, ray_op_t* node) {
    if (node->arity != 1) return false;
    ray_op_t* input = node->inputs[0];
    if (!input || input->opcode != OP_TIL) return false;
    ray_op_ext_t* til_ext = find_ext(g, input->id);
    if (!til_ext || !til_ext->literal) return false;
    int64_t n = til_ext->literal->i64;
    if (n <= 0) return false;

    ray_t* folded = NULL;
    switch (node->opcode) {
        case OP_SUM:   folded = ray_i64((n * (n - 1)) / 2); break;
        case OP_MIN:   folded = ray_i64(0); break;
        case OP_MAX:   folded = ray_i64(n - 1); break;
        case OP_COUNT: folded = ray_i64(n); break;
        case OP_AVG:   folded = ray_f64((double)(n - 1) / 2.0); break;
        case OP_FIRST: folded = ray_i64(0); break;
        case OP_LAST:  folded = ray_i64(n - 1); break;
        default: return false;
    }
    if (!folded || RAY_IS_ERR(folded)) return false;
    if (!replace_with_const(g, node, folded)) { ray_release(folded); return false; }
    return true;
}

static void fold_node(ray_graph_t* g, ray_op_t* node) {
    /* Fold unary element-wise ops with constant input */
    if (node->arity == 1 && node->opcode >= OP_NEG && node->opcode <= OP_FLOOR) {
        (void)fold_unary_const(g, node);
    }
    /* Fold binary element-wise ops with two const inputs */
    if (node->arity == 2 && node->opcode >= OP_ADD && node->opcode <= OP_MAX2) {
        (void)fold_binary_const(g, node);
    }
    /* Fold reduction(til(n)) to closed-form */
    if (node->arity == 1 && node->opcode >= OP_SUM && node->opcode <= OP_LAST) {
        (void)fold_reduction_til(g, node);
    }
    /* FILTER with constant predicate can be reduced to pass-through/empty. */
    (void)fold_filter_const_predicate(g, node);
}

static void pass_constant_fold(ray_graph_t* g, ray_op_t* root) {
    if (!root || root->flags & OP_FLAG_DEAD) return;

    /* Iterative post-order: collect nodes, then process in reverse
       (children before parents). */
    uint32_t nc = g->node_count;
    uint32_t stack_cap = nc * 2 + 64;  /* extra space for high fan-out nodes */
    uint32_t stack_local[256], order_local[256];
    bool visited_stack[256];
    uint32_t *stack = stack_cap <= 256 ? stack_local : (uint32_t*)ray_sys_alloc(stack_cap * sizeof(uint32_t));
    uint32_t *order = nc <= 256 ? order_local : (uint32_t*)ray_sys_alloc(nc * sizeof(uint32_t));
    bool* visited;
    if (nc <= 256) {
        visited = visited_stack;
    } else {
        visited = (bool*)ray_sys_alloc(nc * sizeof(bool));
    }
    if (!stack || !order || !visited) {
        { if (stack_cap > 256) ray_sys_free(stack); if (nc > 256) { ray_sys_free(order); ray_sys_free(visited); } }
        return;
    }
    memset(visited, 0, nc * sizeof(bool));

    int sp = 0, oc = 0;
    stack[sp++] = root->id;
    while (sp > 0 && oc < (int)nc) {
        uint32_t nid = stack[--sp];
        ray_op_t* n = &g->nodes[nid];
        if (!n || n->flags & OP_FLAG_DEAD) continue;
        if (visited[nid]) continue;
        visited[nid] = true;
        order[oc++] = nid;
        for (int i = 0; i < 2 && i < n->arity; i++) {
            if (n->inputs[i] && sp < (int)stack_cap)
                stack[sp++] = n->inputs[i]->id;
        }
        /* H1: Traverse ext-node children so constant folding reaches all
           referenced nodes (GROUP keys/aggs, SORT/PROJECT/SELECT columns,
           JOIN keys, WINDOW partition/order/func_inputs). */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint8_t k = 0; k < ext->n_keys; k++)
                        if (ext->keys[k] && !visited[ext->keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->keys[k]->id;
                    for (uint8_t a = 0; a < ext->n_aggs; a++)
                        if (ext->agg_ins[a] && !visited[ext->agg_ins[a]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->agg_ins[a]->id;
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint8_t k = 0; k < ext->sort.n_cols; k++)
                        if (ext->sort.columns[k] && !visited[ext->sort.columns[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->sort.columns[k]->id;
                    break;
                case OP_JOIN:
                    for (uint8_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (ext->join.left_keys[k] && !visited[ext->join.left_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.left_keys[k]->id;
                        if (ext->join.right_keys && ext->join.right_keys[k] &&
                            !visited[ext->join.right_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.right_keys[k]->id;
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (wj_ext->asof.time_key && !visited[wj_ext->asof.time_key->id] && sp < (int)stack_cap)
                            stack[sp++] = wj_ext->asof.time_key->id;
                        for (uint8_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (wj_ext->asof.eq_keys[k] && !visited[wj_ext->asof.eq_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.eq_keys[k]->id;
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint8_t k = 0; k < ext->window.n_part_keys; k++)
                        if (ext->window.part_keys[k] && !visited[ext->window.part_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.part_keys[k]->id;
                    for (uint8_t k = 0; k < ext->window.n_order_keys; k++)
                        if (ext->window.order_keys[k] && !visited[ext->window.order_keys[k]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.order_keys[k]->id;
                    for (uint8_t f = 0; f < ext->window.n_funcs; f++)
                        if (ext->window.func_inputs[f] && !visited[ext->window.func_inputs[f]->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.func_inputs[f]->id;
                    break;
                /* H1b: 3-input ops store third operand node ID in ext->literal */
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                    if (third_id < nc && !visited[third_id] && sp < (int)stack_cap)
                        stack[sp++] = third_id;
                    break;
                }
                /* H1c: OP_CONCAT trailing arg node IDs beyond inputs[0..1] */
                case OP_CONCAT:
                    if (ext->sym >= 2) {
                        int n_args = (int)ext->sym;
                        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                        for (int j = 2; j < n_args; j++) {
                            uint32_t arg_id = trail[j - 2];
                            if (arg_id < nc && !visited[arg_id] && sp < (int)stack_cap)
                                stack[sp++] = arg_id;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    /* Process in reverse order (children before parents) */
    for (int i = oc - 1; i >= 0; i--)
        fold_node(g, &g->nodes[order[i]]);

    { if (stack_cap > 256) ray_sys_free(stack); if (nc > 256) { ray_sys_free(order); ray_sys_free(visited); } }
}

/* --------------------------------------------------------------------------
 * Pass 3: Dead code elimination
 *
 * Mark nodes unreachable from root as DEAD.
 * -------------------------------------------------------------------------- */

static void mark_live(ray_graph_t* g, ray_op_t* root, bool* live) {
    if (!root) return;

    uint32_t nc = g->node_count;
    if (nc > UINT32_MAX / 2) return;
    /* Worst case: each node can contribute up to ~N children (CONCAT trailing),
       but nc*2 is a safe upper bound for the stack. */
    uint32_t stack_cap = nc * 2;
    uint32_t stack_local[256];
    uint32_t *stack = stack_cap <= 256 ? stack_local : (uint32_t*)ray_sys_alloc(stack_cap * sizeof(uint32_t));
    if (!stack) return;
    int sp = 0;
    stack[sp++] = root->id;
    while (sp > 0) {
        uint32_t nid = stack[--sp];
        if (live[nid]) continue;
        live[nid] = true;
        ray_op_t* n = &g->nodes[nid];
        for (int i = 0; i < 2; i++) {
            if (n->inputs[i] && sp < (int)stack_cap)
                stack[sp++] = n->inputs[i]->id;
        }
        /* H4: 3-input ops (OP_IF, OP_SUBSTR, OP_REPLACE) store the third
           operand node ID as (uintptr_t)ext->literal. */
        if (n->opcode == OP_IF || n->opcode == OP_SUBSTR || n->opcode == OP_REPLACE) {
            ray_op_ext_t* ext = find_ext(g, nid);
            if (ext) {
                uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                if (third_id < nc && sp < (int)stack_cap)
                    stack[sp++] = third_id;
            }
        }
        /* H5: OP_CONCAT stores extra arg IDs (beyond inputs[0..1]) as
           uint32_t values in trailing bytes after the ext node.
           ext->sym holds the total arg count. */
        if (n->opcode == OP_CONCAT) {
            ray_op_ext_t* ext = find_ext(g, nid);
            /* M4: Guard against ext->sym < 2 — trailing uint32_t values
               only exist when there are more than 2 arguments. */
            if (ext && ext->sym >= 2) {
                int n_args = (int)ext->sym;
                uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                for (int i = 2; i < n_args; i++) {
                    uint32_t arg_id = trail[i - 2];
                    if (arg_id < nc && sp < (int)stack_cap)
                        stack[sp++] = arg_id;
                }
            }
        }
        /* H1: Traverse ext node children for structural ops so DCE does
           not incorrectly mark referenced nodes as dead. */
        if (n->opcode == OP_GROUP || n->opcode == OP_SORT ||
            n->opcode == OP_JOIN  || n->opcode == OP_ANTIJOIN ||
            n->opcode == OP_WINDOW_JOIN ||
            n->opcode == OP_WINDOW || n->opcode == OP_PIVOT ||
            n->opcode == OP_SELECT) {
            ray_op_ext_t* ext = find_ext(g, nid);
            if (ext) {
                switch (n->opcode) {
                    case OP_GROUP:
                        for (uint8_t k = 0; k < ext->n_keys; k++) {
                            if (ext->keys[k] && !live[ext->keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->keys[k]->id;
                        }
                        for (uint8_t a = 0; a < ext->n_aggs; a++) {
                            if (ext->agg_ins[a] && !live[ext->agg_ins[a]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->agg_ins[a]->id;
                        }
                        break;
                    case OP_SORT:
                    case OP_SELECT:
                        for (uint8_t k = 0; k < ext->sort.n_cols; k++) {
                            if (ext->sort.columns[k] && !live[ext->sort.columns[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->sort.columns[k]->id;
                        }
                        break;
                    case OP_JOIN:
                    case OP_ANTIJOIN:
                        for (uint8_t k = 0; k < ext->join.n_join_keys; k++) {
                            if (ext->join.left_keys[k] && !live[ext->join.left_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->join.left_keys[k]->id;
                            if (ext->join.right_keys && ext->join.right_keys[k] &&
                                !live[ext->join.right_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->join.right_keys[k]->id;
                        }
                        break;
                    case OP_PIVOT:
                        for (uint8_t k = 0; k < ext->pivot.n_index; k++) {
                            if (ext->pivot.index_cols[k] && !live[ext->pivot.index_cols[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->pivot.index_cols[k]->id;
                        }
                        if (ext->pivot.pivot_col && !live[ext->pivot.pivot_col->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->pivot.pivot_col->id;
                        if (ext->pivot.value_col && !live[ext->pivot.value_col->id] && sp < (int)stack_cap)
                            stack[sp++] = ext->pivot.value_col->id;
                        break;
                    case OP_WINDOW_JOIN: {
                        ray_op_ext_t* wj_ext = find_ext(g, n->id);
                        if (wj_ext) {
                            if (wj_ext->asof.time_key && !live[wj_ext->asof.time_key->id] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.time_key->id;
                            for (uint8_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                                if (wj_ext->asof.eq_keys[k] && !live[wj_ext->asof.eq_keys[k]->id] && sp < (int)stack_cap)
                                    stack[sp++] = wj_ext->asof.eq_keys[k]->id;
                            }
                        }
                        break;
                    }
                    case OP_WINDOW:
                        for (uint8_t k = 0; k < ext->window.n_part_keys; k++) {
                            if (ext->window.part_keys[k] && !live[ext->window.part_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.part_keys[k]->id;
                        }
                        for (uint8_t k = 0; k < ext->window.n_order_keys; k++) {
                            if (ext->window.order_keys[k] && !live[ext->window.order_keys[k]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.order_keys[k]->id;
                        }
                        for (uint8_t f = 0; f < ext->window.n_funcs; f++) {
                            if (ext->window.func_inputs[f] && !live[ext->window.func_inputs[f]->id] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.func_inputs[f]->id;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }
    if (stack_cap > 256) ray_sys_free(stack);
}

static void pass_dce(ray_graph_t* g, ray_op_t* root) {
    uint32_t nc = g->node_count;
    bool* live;
    bool live_stack[256];
    if (nc <= 256) {
        live = live_stack;
    } else {
        live = (bool*)ray_sys_alloc(nc * sizeof(bool));
        if (!live) return;
    }
    memset(live, 0, nc * sizeof(bool));

    mark_live(g, root, live);

    for (uint32_t i = 0; i < nc; i++) {
        if (!live[i]) {
            g->nodes[i].flags |= OP_FLAG_DEAD;
        }
    }
    if (nc > 256) ray_sys_free(live);
}

/* --------------------------------------------------------------------------
 * Pass: SIP (Sideways Information Passing)
 *
 * Bottom-up DAG walk. For each OP_EXPAND:
 *   1. Find downstream filter on target side
 *   2. Reverse-CSR: mark source nodes that have any passing target -> RAY_SEL
 *   3. Attach source_sel to upstream scan
 *
 * Currently a no-op placeholder — activated when graph ops are present.
 * -------------------------------------------------------------------------- */

/* Find downstream consumer of a node (first node that uses it as input) */
static ray_op_t* find_consumer(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->node_count; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        for (int j = 0; j < n->arity && j < 2; j++) {
            if (n->inputs[j] && n->inputs[j]->id == node_id)
                return n;
        }
    }
    return NULL;
}

/* Find upstream OP_SCAN that feeds into a node via input chain (iterative) */
static ray_op_t* find_upstream_scan(ray_graph_t* g, ray_op_t* node) {
    uint32_t limit = g ? g->node_count : 1024;
    for (uint32_t steps = 0; node && steps < limit; steps++) {
        if (node->opcode == OP_SCAN) return node;
        if (node->arity > 0 && node->inputs[0])
            node = node->inputs[0];
        else return NULL;
    }
    return NULL;
}

static void sip_pass(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return;

    uint32_t nc = g->node_count;

    /* Collect graph traversal nodes (bottom-up for chained SIP) */
    uint32_t expand_ids[64];
    uint32_t n_expands = 0;
    for (uint32_t i = 0; i < nc && n_expands < 64; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        if (n->opcode != OP_EXPAND && n->opcode != OP_VAR_EXPAND
            && n->opcode != OP_SHORTEST_PATH) continue;
        expand_ids[n_expands++] = i;
    }

    /* Process bottom-up (deepest expand first — process in reverse ID order
     * since deeper nodes in the pipeline tend to have higher IDs) */
    for (int ei = (int)n_expands - 1; ei >= 0; ei--) {
        ray_op_t* expand = &g->nodes[expand_ids[ei]];
        ray_op_ext_t* ext = find_ext(g, expand->id);
        if (!ext || !ext->graph.rel) continue;

        /* 1. Find downstream consumer — look for OP_FILTER on target side */
        ray_op_t* consumer = find_consumer(g, expand->id);
        if (!consumer) continue;

        /* If the consumer is OP_FILTER, we can extract a semijoin.
         * The filter's condition restricts which target nodes pass.
         * We reverse-propagate through the CSR to mark which source
         * nodes could produce any passing target. */
        if (consumer->opcode != OP_FILTER) continue;

        /* 2. Find the input scan to this expand (source side) */
        ray_op_t* src_scan = NULL;
        if (expand->arity > 0 && expand->inputs[0])
            src_scan = find_upstream_scan(g, expand->inputs[0]);

        if (!src_scan) continue;

        /* 3. Propagate backward: attach selection hint to the expand node.
         * The executor will use this to build a RAY_SEL bitmap at runtime
         * by evaluating the filter condition, reverse-CSR propagating,
         * and applying the resulting source-side selection.
         *
         * We store the filter node ID in the expand's ext pad bytes
         * so the executor can find the downstream filter for runtime SIP. */
        /* pad[2] = 1 signals the executor to build SIP bitmap at runtime.
         * Note: pad is only 3 bytes (pad[0..2]) — do NOT write uint16_t
         * at pad+2 as that overflows into the 'id' field at offset 8. */
        ext->base.pad[2] = 1;
    }
}

/* --------------------------------------------------------------------------
 * Pass: Factorized detection
 *
 * Detect OP_EXPAND → OP_GROUP patterns where factorized execution
 * avoids materializing the full cross-product.
 * -------------------------------------------------------------------------- */
static void factorize_pass(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return;

    uint32_t nc = g->node_count;
    for (uint32_t i = 0; i < nc; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        if (n->opcode != OP_EXPAND) continue;

        ray_op_ext_t* ext = find_ext(g, n->id);
        if (!ext || ext->graph.factorized) continue;  /* already set by SIP pass */

        /* Look for immediate OP_GROUP consumer with _src as group key */
        ray_op_t* consumer = find_consumer(g, n->id);
        if (!consumer || consumer->opcode != OP_GROUP) continue;

        ray_op_ext_t* grp_ext = find_ext(g, consumer->id);
        if (!grp_ext || grp_ext->n_keys != 1 || !grp_ext->keys[0]) continue;

        ray_op_ext_t* key_ext = find_ext(g, grp_ext->keys[0]->id);
        if (!key_ext || key_ext->base.opcode != OP_SCAN) continue;

        int64_t src_sym = ray_sym_intern("_src", 4);
        if (key_ext->sym == src_sym) {
            ext->graph.factorized = 1;
        }
    }
}

/* --------------------------------------------------------------------------
 * Pass: Filter reordering
 *
 * Reorder chained OP_FILTER nodes so cheapest predicates execute first.
 * Also splits AND trees into separate chained filters.
 * -------------------------------------------------------------------------- */

/* Allocate a new node in the graph (for use during optimization passes).
 * Same logic as graph_alloc_node in graph.c but local to opt.c. */
static ray_op_t* graph_alloc_node_opt(ray_graph_t* g) {
    if (g->node_count >= g->node_cap) {
        if (g->node_cap > UINT32_MAX / 2) return NULL;
        uint32_t new_cap = g->node_cap * 2;
        uintptr_t old_base = (uintptr_t)g->nodes;
        ray_op_t* new_nodes = (ray_op_t*)ray_sys_realloc(g->nodes,
                                                       new_cap * sizeof(ray_op_t));
        if (!new_nodes) return NULL;
        g->nodes = new_nodes;
        g->node_cap = new_cap;
        /* Fix up all input pointers after realloc */
        ptrdiff_t delta = (ptrdiff_t)((uintptr_t)g->nodes - old_base);
        if (delta != 0) {
            for (uint32_t i = 0; i < g->node_count; i++) {
                if (g->nodes[i].inputs[0])
                    g->nodes[i].inputs[0] = (ray_op_t*)((char*)g->nodes[i].inputs[0] + delta);
                if (g->nodes[i].inputs[1])
                    g->nodes[i].inputs[1] = (ray_op_t*)((char*)g->nodes[i].inputs[1] + delta);
            }
            /* Fix ext node input pointers */
            for (uint32_t i = 0; i < g->ext_count; i++) {
                if (g->ext_nodes[i]) {
                    if (g->ext_nodes[i]->base.inputs[0])
                        g->ext_nodes[i]->base.inputs[0] =
                            (ray_op_t*)((char*)g->ext_nodes[i]->base.inputs[0] + delta);
                    if (g->ext_nodes[i]->base.inputs[1])
                        g->ext_nodes[i]->base.inputs[1] =
                            (ray_op_t*)((char*)g->ext_nodes[i]->base.inputs[1] + delta);
                    /* Fix structural op column pointers */
                    switch (g->ext_nodes[i]->base.opcode) {
                        case OP_GROUP:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->n_keys; k++)
                                if (g->ext_nodes[i]->keys[k])
                                    g->ext_nodes[i]->keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->keys[k] + delta);
                            for (uint8_t a = 0; a < g->ext_nodes[i]->n_aggs; a++)
                                if (g->ext_nodes[i]->agg_ins[a])
                                    g->ext_nodes[i]->agg_ins[a] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->agg_ins[a] + delta);
                            break;
                        case OP_SORT:
                        case OP_SELECT:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->sort.n_cols; k++)
                                if (g->ext_nodes[i]->sort.columns[k])
                                    g->ext_nodes[i]->sort.columns[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->sort.columns[k] + delta);
                            break;
                        case OP_JOIN:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->join.n_join_keys; k++) {
                                if (g->ext_nodes[i]->join.left_keys[k])
                                    g->ext_nodes[i]->join.left_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->join.left_keys[k] + delta);
                                if (g->ext_nodes[i]->join.right_keys &&
                                    g->ext_nodes[i]->join.right_keys[k])
                                    g->ext_nodes[i]->join.right_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->join.right_keys[k] + delta);
                            }
                            break;
                        case OP_WINDOW_JOIN:
                            if (g->ext_nodes[i]->asof.time_key)
                                g->ext_nodes[i]->asof.time_key = (ray_op_t*)((char*)g->ext_nodes[i]->asof.time_key + delta);
                            for (uint8_t k = 0; k < g->ext_nodes[i]->asof.n_eq_keys; k++)
                                if (g->ext_nodes[i]->asof.eq_keys[k])
                                    g->ext_nodes[i]->asof.eq_keys[k] = (ray_op_t*)((char*)g->ext_nodes[i]->asof.eq_keys[k] + delta);
                            break;
                        case OP_ANTIJOIN:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->join.n_join_keys; k++) {
                                if (g->ext_nodes[i]->join.left_keys[k])
                                    g->ext_nodes[i]->join.left_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->join.left_keys[k] + delta);
                                if (g->ext_nodes[i]->join.right_keys &&
                                    g->ext_nodes[i]->join.right_keys[k])
                                    g->ext_nodes[i]->join.right_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->join.right_keys[k] + delta);
                            }
                            break;
                        case OP_PIVOT:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->pivot.n_index; k++)
                                if (g->ext_nodes[i]->pivot.index_cols[k])
                                    g->ext_nodes[i]->pivot.index_cols[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->pivot.index_cols[k] + delta);
                            if (g->ext_nodes[i]->pivot.pivot_col)
                                g->ext_nodes[i]->pivot.pivot_col =
                                    (ray_op_t*)((char*)g->ext_nodes[i]->pivot.pivot_col + delta);
                            if (g->ext_nodes[i]->pivot.value_col)
                                g->ext_nodes[i]->pivot.value_col =
                                    (ray_op_t*)((char*)g->ext_nodes[i]->pivot.value_col + delta);
                            break;
                        case OP_WINDOW:
                            for (uint8_t k = 0; k < g->ext_nodes[i]->window.n_part_keys; k++)
                                if (g->ext_nodes[i]->window.part_keys[k])
                                    g->ext_nodes[i]->window.part_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->window.part_keys[k] + delta);
                            for (uint8_t k = 0; k < g->ext_nodes[i]->window.n_order_keys; k++)
                                if (g->ext_nodes[i]->window.order_keys[k])
                                    g->ext_nodes[i]->window.order_keys[k] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->window.order_keys[k] + delta);
                            for (uint8_t f = 0; f < g->ext_nodes[i]->window.n_funcs; f++)
                                if (g->ext_nodes[i]->window.func_inputs[f])
                                    g->ext_nodes[i]->window.func_inputs[f] =
                                        (ray_op_t*)((char*)g->ext_nodes[i]->window.func_inputs[f] + delta);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
    ray_op_t* n = &g->nodes[g->node_count];
    memset(n, 0, sizeof(ray_op_t));
    n->id = g->node_count;
    g->node_count++;
    return n;
}

/* Count how many live nodes use node_id as an input.
 * Returns the consumer count (0 if unreferenced). */
static int count_node_consumers(ray_graph_t* g, uint32_t node_id) {
    int count = 0;
    uint32_t nc = g->node_count;
    for (uint32_t j = 0; j < nc; j++) {
        ray_op_t* c = &g->nodes[j];
        if (c->flags & OP_FLAG_DEAD) continue;
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->inputs[k] && c->inputs[k]->id == node_id) {
                count++;
                break;  /* count each consumer node once */
            }
        }
    }
    for (uint32_t j = 0; j < g->ext_count; j++) {
        if (!g->ext_nodes[j]) continue;
        ray_op_t* c = &g->ext_nodes[j]->base;
        if (c->flags & OP_FLAG_DEAD) continue;
        if (c->id < nc) continue;  /* already counted in nodes[] */
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->inputs[k] && c->inputs[k]->id == node_id) {
                count++;
                break;
            }
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * Pass: Predicate pushdown
 *
 * Move OP_FILTER nodes below PROJECT/SELECT, GROUP (key-only), JOIN
 * (one-sided), and EXPAND (source-only) to reduce rows flowing through
 * expensive operators.
 * -------------------------------------------------------------------------- */

/* Collect all OP_SCAN node IDs referenced by a predicate subtree.
 * Returns count on success, -1 if traversal was truncated (stack or result
 * overflow) — caller must treat -1 as "unknown" and skip optimisation. */
static int collect_pred_scans(ray_graph_t* g, ray_op_t* pred,
                              uint32_t* scan_ids, int max) {
    if (!pred || max <= 0) return 0;
    int n = 0;

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = pred->id;

    bool visited[4096];
    uint32_t nc = g->node_count;
    if (nc > 4096) return -1;  /* safety: skip for huge graphs */
    memset(visited, 0, nc * sizeof(bool));

    while (sp > 0) {
        uint32_t nid = stack[--sp];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = true;
        ray_op_t* node = &g->nodes[nid];
        if (node->flags & OP_FLAG_DEAD) continue;

        if (node->opcode == OP_SCAN) {
            if (n >= max) return -1;  /* result overflow */
            scan_ids[n++] = nid;
            continue;
        }
        for (int i = 0; i < node->arity && i < 2; i++) {
            if (node->inputs[i]) {
                if (sp >= 64) return -1;  /* stack overflow */
                stack[sp++] = node->inputs[i]->id;
            }
        }
        /* Walk ext-stored operands for multi-input ops */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (node->opcode) {
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                    if (third_id < nc && !visited[third_id]) {
                        if (sp >= 64) return -1;
                        stack[sp++] = third_id;
                    }
                    break;
                }
                case OP_CONCAT:
                    if (ext->sym >= 2) {
                        int n_args = (int)ext->sym;
                        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                        for (int j = 2; j < n_args; j++) {
                            uint32_t arg_id = trail[j - 2];
                            if (arg_id < nc && !visited[arg_id]) {
                                if (sp >= 64) return -1;
                                stack[sp++] = arg_id;
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return n;
}

/* Check if target_id is reachable from start by walking inputs.
 * Returns true if target_id is in the subgraph rooted at start. */
static bool is_reachable_from(ray_graph_t* g, ray_op_t* start, uint32_t target_id) {
    if (!start) return false;
    if (start->id == target_id) return true;

    uint32_t nc = g->node_count;
    if (nc > 4096) return false;

    bool visited[4096];
    memset(visited, 0, nc * sizeof(bool));

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = start->id;

    while (sp > 0) {
        uint32_t nid = stack[--sp];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = true;
        if (nid == target_id) return true;
        ray_op_t* node = &g->nodes[nid];
        if (node->flags & OP_FLAG_DEAD) continue;
        for (int i = 0; i < node->arity && i < 2; i++) {
            if (node->inputs[i] && sp < 64)
                stack[sp++] = node->inputs[i]->id;
        }
        /* Walk ext-stored operands for multi-input ops */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (node->opcode) {
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                    if (third_id < nc && !visited[third_id] && sp < 64)
                        stack[sp++] = third_id;
                    break;
                }
                case OP_CONCAT:
                    if (ext->sym >= 2) {
                        int n_args = (int)ext->sym;
                        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                        for (int j = 2; j < n_args; j++) {
                            uint32_t arg_id = trail[j - 2];
                            if (arg_id < nc && !visited[arg_id] && sp < 64)
                                stack[sp++] = arg_id;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return false;
}

/* Redirect all consumers of old_id to point to new_target instead.
 * Skips nodes with IDs skip_a and skip_b (the swapped pair).
 * Updates both g->nodes[] and g->ext_nodes[].base.inputs[]. */
static void redirect_consumers(ray_graph_t* g, uint32_t old_id,
                               ray_op_t* new_target,
                               uint32_t skip_a, uint32_t skip_b) {
    uint32_t nc = g->node_count;
    for (uint32_t j = 0; j < nc; j++) {
        ray_op_t* c = &g->nodes[j];
        if (c->flags & OP_FLAG_DEAD || j == skip_a || j == skip_b) continue;
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->inputs[k] && c->inputs[k]->id == old_id)
                c->inputs[k] = new_target;
        }
    }
    /* Also update ext_node heap copies to keep them in sync */
    for (uint32_t j = 0; j < g->ext_count; j++) {
        if (!g->ext_nodes[j]) continue;
        ray_op_t* c = &g->ext_nodes[j]->base;
        if (c->flags & OP_FLAG_DEAD) continue;
        if (c->id == skip_a || c->id == skip_b) continue;
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->inputs[k] && c->inputs[k]->id == old_id)
                c->inputs[k] = new_target;
        }
    }
}

static ray_op_t* pass_predicate_pushdown(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return root;

    /* Multiple iterations: pushdown may enable further pushdowns */
    for (int iter = 0; iter < 4; iter++) {
        bool changed = false;
        uint32_t nc = g->node_count;

        for (uint32_t i = 0; i < nc; i++) {
            ray_op_t* n = &g->nodes[i];
            if (n->flags & OP_FLAG_DEAD) continue;
            if (n->opcode != OP_FILTER || n->arity != 2) continue;

            ray_op_t* child = n->inputs[0];
            ray_op_t* pred  = n->inputs[1];
            if (!child || !pred) continue;

            /* Push past SELECT/ALIAS (only if child is single-consumer,
             * otherwise mutating child->inputs[0] would corrupt other branches) */
            if (child->opcode == OP_SELECT ||
                child->opcode == OP_ALIAS) {
                if (count_node_consumers(g, child->id) > 1) continue;
                /* Swap: FILTER(pred, SELECT(x)) -> SELECT(FILTER(pred, x)) */
                ray_op_t* proj_input = child->inputs[0];
                n->inputs[0] = proj_input;
                child->inputs[0] = n;
                redirect_consumers(g, n->id, child, child->id, n->id);
                if (n->id == root->id) root = child;
                changed = true;
                continue;
            }

            /* GROUP pushdown disabled: the executor's key/agg scans
             * bypass the filter, producing wrong results. Needs executor
             * support for filtered scan propagation before enabling. */

            /* Push past EXPAND (source-side predicates, single-consumer only) */
            if (child->opcode == OP_EXPAND) {
                if (count_node_consumers(g, child->id) > 1) continue;
                uint32_t scan_ids[64];
                int n_scans = collect_pred_scans(g, pred, scan_ids, 64);
                if (n_scans <= 0) continue;  /* 0 = no scans, -1 = truncated */

                /* All predicate scans must be reachable from the expand's
                 * source input (inputs[0]).  Walk the source subtree. */
                ray_op_t* expand_src_tree = child->inputs[0];
                bool all_source = true;
                for (int s = 0; s < n_scans; s++) {
                    if (!is_reachable_from(g, expand_src_tree, scan_ids[s])) {
                        all_source = false;
                        break;
                    }
                }
                if (!all_source) continue;

                /* Swap: FILTER(pred, EXPAND(src, rel)) -> EXPAND(FILTER(pred, src), rel) */
                ray_op_t* expand_src = child->inputs[0];
                n->inputs[0] = expand_src;
                child->inputs[0] = n;
                redirect_consumers(g, n->id, child, child->id, n->id);
                if (n->id == root->id) root = child;
                changed = true;
                continue;
            }
        }
        if (!changed) break;
    }
    return root;
}

/* Score a predicate subtree: lower = cheaper = execute first. */
static int filter_cost(ray_graph_t* g, ray_op_t* pred) {
    (void)g;
    if (!pred) return 99;
    int cost = 0;

    /* Constant comparison: one input is OP_CONST */
    bool has_const = false;
    for (int i = 0; i < pred->arity && i < 2; i++) {
        if (pred->inputs[i] && pred->inputs[i]->opcode == OP_CONST)
            has_const = true;
    }
    if (!has_const) cost += 4;  /* col-col comparison */

    /* Type width cost */
    int8_t t = pred->out_type;
    if (pred->arity >= 1 && pred->inputs[0])
        t = pred->inputs[0]->out_type;
    switch (t) {
        case RAY_BOOL: case RAY_U8:  cost += 0; break;
        case RAY_I16:               cost += 1; break;
        case RAY_I32:  case RAY_DATE: case RAY_TIME: cost += 2; break;
        default:                   cost += 3; break;  /* I64, F64, SYM, STR */
    }

    /* Comparison type cost */
    switch (pred->opcode) {
        case OP_EQ: case OP_NE:    cost += 0; break;
        case OP_LT: case OP_LE:
        case OP_GT: case OP_GE:    cost += 2; break;
        case OP_LIKE: case OP_ILIKE: cost += 4; break;
        default:                   cost += 1; break;
    }

    return cost;
}

/* Split FILTER(AND(a, b), input) into FILTER(a, FILTER(b, input)).
 * Returns the new outer filter node, or the original if no split. */
static ray_op_t* split_and_filter(ray_graph_t* g, ray_op_t* filter_node) {
    if (!filter_node || filter_node->opcode != OP_FILTER) return filter_node;
    if (filter_node->arity != 2) return filter_node;

    ray_op_t* pred = filter_node->inputs[1];
    if (!pred || pred->opcode != OP_AND || pred->arity != 2) return filter_node;

    ray_op_t* pred_a = pred->inputs[0];
    ray_op_t* pred_b = pred->inputs[1];
    ray_op_t* input  = filter_node->inputs[0];
    if (!pred_a || !pred_b || !input) return filter_node;

    /* Save IDs before potential realloc */
    uint32_t filter_id = filter_node->id;
    uint32_t pred_a_id = pred_a->id;
    uint32_t pred_b_id = pred_b->id;

    /* Allocate new outer filter first, before mutating existing nodes */
    ray_op_t* outer = graph_alloc_node_opt(g);
    if (!outer) return &g->nodes[filter_id];  /* OOM: leave unsplit */

    /* Re-fetch after potential realloc */
    filter_node = &g->nodes[filter_id];
    pred_a = &g->nodes[pred_a_id];
    pred_b = &g->nodes[pred_b_id];

    /* Rewrite: filter_node becomes FILTER(pred_a, input) */
    filter_node->inputs[1] = pred_a;

    outer->opcode = OP_FILTER;
    outer->arity = 2;
    outer->inputs[0] = filter_node;
    outer->inputs[1] = pred_b;
    outer->out_type = filter_node->out_type;
    outer->est_rows = filter_node->est_rows;

    return outer;
}

/* Collect a chain of OP_FILTER nodes. Returns count (max 64). */
static int collect_filter_chain(ray_op_t* top, ray_op_t** chain, int max) {
    int n = 0;
    ray_op_t* cur = top;
    while (cur && cur->opcode == OP_FILTER && n < max) {
        chain[n++] = cur;
        cur = cur->inputs[0];
    }
    return n;
}

static ray_op_t* pass_filter_reorder(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return root;

    uint32_t root_id = root->id;

    /* First pass: split AND predicates in filters.
     * Iterate until no more splits occur so nested ANDs like
     * AND(AND(a,b), c) are fully decomposed into individual filters. */
    for (int split_iter = 0; split_iter < 16; split_iter++) {
        bool split_changed = false;
        uint32_t nc = g->node_count;
        for (uint32_t i = 0; i < nc; i++) {
            ray_op_t* n = &g->nodes[i];
            if (n->flags & OP_FLAG_DEAD) continue;
            if (n->opcode != OP_FILTER) continue;
            if (n->arity != 2 || !n->inputs[1]) continue;
            if (n->inputs[1]->opcode != OP_AND) continue;

            /* Split AND and update consumers to point to new outer.
             * split_and_filter may realloc g->nodes, so re-fetch n afterwards. */
            uint32_t orig_id = i;
            ray_op_t* new_outer = split_and_filter(g, n);
            n = &g->nodes[orig_id];  /* re-fetch after potential realloc */
            if (new_outer->id != orig_id) {
                redirect_consumers(g, orig_id, new_outer, new_outer->id, orig_id);
                if (orig_id == root_id) root_id = new_outer->id;
                split_changed = true;
            }
        }
        if (!split_changed) break;
    }

    /* Second pass: reorder filter chains by cost.
     * Use insertion sort on chain arrays (chains are typically short). */
    uint32_t nc = g->node_count;  /* may have grown from splits */
    bool* visited = NULL;
    bool visited_stack[256];
    if (nc <= 256) {
        visited = visited_stack;
    } else {
        visited = (bool*)ray_sys_alloc(nc * sizeof(bool));
        if (!visited) return &g->nodes[root_id];
    }
    memset(visited, 0, nc * sizeof(bool));

    for (uint32_t i = 0; i < nc; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        if (n->opcode != OP_FILTER) continue;
        if (visited[i]) continue;

        /* Collect the filter chain starting at this node */
        ray_op_t* chain[64];
        int chain_len = collect_filter_chain(n, chain, 64);
        if (chain_len < 2) {
            for (int c = 0; c < chain_len; c++) visited[chain[c]->id] = true;
            continue;
        }

        /* Mark all as visited */
        for (int c = 0; c < chain_len; c++) visited[chain[c]->id] = true;

        /* Skip reordering if any filter in the chain has multiple consumers,
         * since swapping predicates would change semantics for other branches */
        bool has_shared = false;
        for (int c = 0; c < chain_len; c++) {
            if (count_node_consumers(g, chain[c]->id) > 1) {
                has_shared = true;
                break;
            }
        }
        if (has_shared) continue;

        /* Score each filter's predicate */
        int costs[64];
        for (int c = 0; c < chain_len; c++)
            costs[c] = filter_cost(g, chain[c]->inputs[1]);

        /* Insertion sort predicates by cost descending (stable: preserves
         * original order for equal costs). Expensive predicates go to
         * chain[0] (outer, runs last), cheap go to chain[N-1] (inner,
         * runs first). We swap predicates, not filter nodes. */
        for (int c = 1; c < chain_len; c++) {
            ray_op_t* pred = chain[c]->inputs[1];
            int cost = costs[c];
            int j = c - 1;
            while (j >= 0 && costs[j] < cost) {
                chain[j + 1]->inputs[1] = chain[j]->inputs[1];
                costs[j + 1] = costs[j];
                j--;
            }
            chain[j + 1]->inputs[1] = pred;
            costs[j + 1] = cost;
        }
    }

    if (nc > 256) ray_sys_free(visited);
    return &g->nodes[root_id];
}

/* --------------------------------------------------------------------------
 * Pass 7: Projection pushdown
 *
 * BFS from root collecting all reachable node IDs (following inputs and
 * ext-node children).  Any node not reachable is marked DEAD so the DCE
 * pass can clean it up.
 * -------------------------------------------------------------------------- */

static bool pass_projection_pushdown(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return false;
    uint32_t nc = g->node_count;

    bool live_stack[256];
    bool* live = nc <= 256 ? live_stack : (bool*)ray_sys_alloc(nc * sizeof(bool));
    uint32_t q_stack[256];
    uint32_t* q = nc <= 256 ? q_stack : (uint32_t*)ray_sys_alloc(nc * sizeof(uint32_t));
    if (!live || !q) { if (nc > 256) { ray_sys_free(live); ray_sys_free(q); } return false; }
    memset(live, 0, nc * sizeof(bool));

    /* BFS from root */
    int qh = 0, qt = 0;
    q[qt++] = root->id;
    live[root->id] = true;

    while (qh < qt) {
        uint32_t nid = q[qh++];
        ray_op_t* n = &g->nodes[nid];

        /* Follow standard inputs */
        for (int i = 0; i < 2 && i < n->arity; i++) {
            if (n->inputs[i] && !live[n->inputs[i]->id]) {
                live[n->inputs[i]->id] = true;
                if (qt < (int)nc) q[qt++] = n->inputs[i]->id;
            }
        }

        /* Follow ext node children (mirrors pass_type_inference traversal) */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint8_t k = 0; k < ext->n_keys; k++)
                        if (ext->keys[k] && !live[ext->keys[k]->id]) {
                            live[ext->keys[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->keys[k]->id;
                        }
                    for (uint8_t a = 0; a < ext->n_aggs; a++)
                        if (ext->agg_ins[a] && !live[ext->agg_ins[a]->id]) {
                            live[ext->agg_ins[a]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->agg_ins[a]->id;
                        }
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint8_t k = 0; k < ext->sort.n_cols; k++)
                        if (ext->sort.columns[k] && !live[ext->sort.columns[k]->id]) {
                            live[ext->sort.columns[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->sort.columns[k]->id;
                        }
                    break;
                case OP_JOIN:
                    for (uint8_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (ext->join.left_keys[k] && !live[ext->join.left_keys[k]->id]) {
                            live[ext->join.left_keys[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->join.left_keys[k]->id;
                        }
                        if (ext->join.right_keys && ext->join.right_keys[k] &&
                            !live[ext->join.right_keys[k]->id]) {
                            live[ext->join.right_keys[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->join.right_keys[k]->id;
                        }
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (wj_ext->asof.time_key && !live[wj_ext->asof.time_key->id]) {
                            live[wj_ext->asof.time_key->id] = true;
                            if (qt < (int)nc) q[qt++] = wj_ext->asof.time_key->id;
                        }
                        for (uint8_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (wj_ext->asof.eq_keys[k] && !live[wj_ext->asof.eq_keys[k]->id]) {
                                live[wj_ext->asof.eq_keys[k]->id] = true;
                                if (qt < (int)nc) q[qt++] = wj_ext->asof.eq_keys[k]->id;
                            }
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint8_t k = 0; k < ext->window.n_part_keys; k++)
                        if (ext->window.part_keys[k] && !live[ext->window.part_keys[k]->id]) {
                            live[ext->window.part_keys[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.part_keys[k]->id;
                        }
                    for (uint8_t k = 0; k < ext->window.n_order_keys; k++)
                        if (ext->window.order_keys[k] && !live[ext->window.order_keys[k]->id]) {
                            live[ext->window.order_keys[k]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.order_keys[k]->id;
                        }
                    for (uint8_t f = 0; f < ext->window.n_funcs; f++)
                        if (ext->window.func_inputs[f] && !live[ext->window.func_inputs[f]->id]) {
                            live[ext->window.func_inputs[f]->id] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.func_inputs[f]->id;
                        }
                    break;
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = (uint32_t)(uintptr_t)ext->literal;
                    if (third_id < nc && !live[third_id]) {
                        live[third_id] = true;
                        if (qt < (int)nc) q[qt++] = third_id;
                    }
                    break;
                }
                case OP_CONCAT:
                    if (ext->sym >= 2) {
                        int n_args = (int)ext->sym;
                        uint32_t* trail = (uint32_t*)((char*)(ext + 1));
                        for (int j = 2; j < n_args; j++) {
                            uint32_t arg_id = trail[j - 2];
                            if (arg_id < nc && !live[arg_id]) {
                                live[arg_id] = true;
                                if (qt < (int)nc) q[qt++] = arg_id;
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /* Mark unreachable nodes DEAD */
    for (uint32_t i = 0; i < nc; i++) {
        if (!live[i])
            g->nodes[i].flags |= OP_FLAG_DEAD;
    }

    if (nc > 256) { ray_sys_free(live); ray_sys_free(q); }
    return true;
}

/* --------------------------------------------------------------------------
 * Pass 8: Partition pruning
 *
 * Recognize FILTER(EQ(SCAN(mapcommon_col), CONST(val))) patterns and set
 * est_rows=1 to hint that most partitions can be skipped at execution time.
 * -------------------------------------------------------------------------- */

static void pass_partition_pruning(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return;
    (void)root;

    for (uint32_t i = 0; i < g->node_count; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        if (n->opcode != OP_FILTER || n->arity != 2) continue;

        ray_op_t* pred = n->inputs[1];
        if (!pred || pred->arity != 2) continue;

        uint16_t cmp_op = pred->opcode;
        if (cmp_op != OP_EQ && cmp_op != OP_NE &&
            cmp_op != OP_LT && cmp_op != OP_GT &&
            cmp_op != OP_LE && cmp_op != OP_GE &&
            cmp_op != OP_IN && cmp_op != OP_NOT_IN) continue;

        ray_op_t* lhs = pred->inputs[0];
        ray_op_t* rhs = pred->inputs[1];
        if (!lhs || !rhs) continue;

        ray_op_t* scan_node = NULL;
        ray_op_t* const_node = NULL;
        bool swapped = false;
        if (lhs->opcode == OP_SCAN && rhs->opcode == OP_CONST) {
            scan_node = lhs; const_node = rhs;
        } else if (rhs->opcode == OP_SCAN && lhs->opcode == OP_CONST) {
            scan_node = rhs; const_node = lhs; swapped = true;
        } else continue;

        if (scan_node->out_type != RAY_MAPCOMMON) continue;

        ray_op_ext_t* scan_ext = find_ext(g, scan_node->id);
        if (!scan_ext) continue;

        /* Resolve table */
        uint16_t stored_table_id = 0;
        memcpy(&stored_table_id, scan_ext->base.pad, sizeof(uint16_t));
        ray_t* tbl;
        if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables)
            tbl = g->tables[stored_table_id - 1];
        else
            tbl = g->table;
        if (!tbl) continue;

        ray_t* mc_col = ray_table_get_col(tbl, scan_ext->sym);
        if (!mc_col || mc_col->type != RAY_MAPCOMMON) continue;

        /* Extract constant value */
        ray_op_ext_t* const_ext = find_ext(g, const_node->id);
        if (!const_ext || !const_ext->literal) continue;
        ray_t* lit = const_ext->literal;

        /* Read partition keys from MAPCOMMON: [key_values, row_counts] */
        if (mc_col->len < 2) continue;
        ray_t** mc_ptrs = (ray_t**)ray_data(mc_col);
        ray_t* key_values = mc_ptrs[0];
        if (!key_values) continue;
        int64_t n_parts = key_values->len;
        if (n_parts <= 0) continue;

        /* Type-class check: partition keys and the literal must live in
         * the same value namespace, otherwise comparisons are nonsense.
         *   - SYM keys are interned IDs; they can only be compared to
         *     SYM set elements.
         *   - int-family keys (I16/I32/I64/DATE/TIME/TIMESTAMP/BOOL/U8)
         *     compare only to other int-family values.
         *   - mixing the two is always wrong at the raw-bits level,
         *     so skip pruning (the executor filter still runs). */
        int8_t pkey_t = key_values->type;
        int8_t lit_base = lit->type < 0 ? (int8_t)(-lit->type) : lit->type;
        bool pkey_is_sym = (pkey_t == RAY_SYM);
        bool lit_is_sym  = (lit_base == RAY_SYM);
        if (pkey_is_sym != lit_is_sym) {
            continue;
        }

        /* Allocate seg_mask bitmap */
        uint32_t n_words = (uint32_t)((n_parts + 63) / 64);
        uint64_t* mask = (uint64_t*)ray_sys_alloc(n_words * sizeof(uint64_t));
        if (!mask) continue;
        memset(mask, 0, n_words * sizeof(uint64_t));

        /* OP_IN / OP_NOT_IN expects a literal vector const on the RHS.
         * For the scalar ops, the const is a single atom or 1-elem vec. */
        bool is_in  = (cmp_op == OP_IN);
        bool is_nin = (cmp_op == OP_NOT_IN);

        /* For IN/NOT_IN the scan must be the LHS (col IN set), not
         * swapped — we never pruned on `const IN col_set` anyway. */
        if ((is_in || is_nin) && swapped) { ray_sys_free(mask); continue; }

        /* Extract constant(s) for comparison.  Scalar ops take one
         * value; IN ops take an array of values read from the vec
         * literal.  We normalize all values to int64_t (which covers
         * I64, TIMESTAMP, SYM interned IDs, and sign-extended I32/
         * DATE/TIME).  Atoms store the value in the header; vectors
         * store it in data. */
        int64_t const_val = 0;                 /* for scalar ops */
        int64_t set_stack[32];
        int64_t* set_vals = set_stack;         /* for IN/NOT_IN */
        int64_t set_len   = 0;
        ray_t*  set_heap  = NULL;

        int8_t lt = lit->type < 0 ? (int8_t)(-lit->type) : lit->type;
        bool narrow32 = (lt == RAY_I32 || lt == RAY_DATE || lt == RAY_TIME);
        bool wide64   = (lt == RAY_I64 || lt == RAY_TIMESTAMP || lt == RAY_SYM);
        if (!narrow32 && !wide64) {
            ray_sys_free(mask);
            continue;  /* unsupported type for partition pruning */
        }

        if (is_in || is_nin) {
            /* Literal must be a vector (ray_const_vec carries the vec
             * pointer unchanged in ext->literal). */
            if (lit->type <= 0) { ray_sys_free(mask); continue; }
            set_len = lit->len;
            if (set_len <= 0) {
                /* Empty set: for IN no partition can match → mask stays 0
                 * and we attach it below (skipping all segments).  For
                 * NOT_IN every partition passes → set all bits. */
                if (is_nin) {
                    for (int64_t p = 0; p < n_parts; p++)
                        mask[p / 64] |= (1ULL << (p % 64));
                }
                goto attach_mask;
            }
            if (set_len > 32) {
                set_heap = ray_alloc((size_t)set_len * sizeof(int64_t));
                if (!set_heap) { ray_sys_free(mask); continue; }
                set_vals = (int64_t*)ray_data(set_heap);
            }
            /* Read set elements — skip nulls in the literal so a null
             * sentinel can never match a partition key. */
            int64_t next = 0;
            bool set_has_nulls = (lit->attrs & RAY_ATTR_HAS_NULLS) != 0;
            for (int64_t i = 0; i < set_len; i++) {
                if (set_has_nulls && ray_vec_is_null(lit, i)) continue;
                if (narrow32) {
                    int32_t v32;
                    memcpy(&v32, (char*)ray_data(lit) + i * sizeof(int32_t), sizeof(int32_t));
                    set_vals[next++] = v32;
                } else {
                    int64_t v64;
                    memcpy(&v64, (char*)ray_data(lit) + i * sizeof(int64_t), sizeof(int64_t));
                    set_vals[next++] = v64;
                }
            }
            set_len = next;
            /* Also handle the degenerate case where all set elements
             * were null — treat like empty set. */
            if (set_len == 0) {
                if (is_nin) {
                    for (int64_t p = 0; p < n_parts; p++)
                        mask[p / 64] |= (1ULL << (p % 64));
                }
                if (set_heap) ray_free(set_heap);
                goto attach_mask;
            }
        } else {
            /* Scalar const path (EQ/NE/LT/GT/LE/GE). */
            if (wide64) {
                if (lit->type < 0) const_val = lit->i64;
                else memcpy(&const_val, ray_data(lit), sizeof(int64_t));
            } else {
                int32_t v32;
                if (lit->type < 0) v32 = lit->i32;
                else memcpy(&v32, ray_data(lit), sizeof(int32_t));
                const_val = v32;
            }
        }

        /* Effective comparison: if swapped, reverse direction
         * (IN/NOT_IN are never swapped — gated above). */
        uint16_t eff_op = cmp_op;
        if (swapped) {
            if (cmp_op == OP_LT) eff_op = OP_GT;
            else if (cmp_op == OP_GT) eff_op = OP_LT;
            else if (cmp_op == OP_LE) eff_op = OP_GE;
            else if (cmp_op == OP_GE) eff_op = OP_LE;
        }

        for (int64_t p = 0; p < n_parts; p++) {
            int64_t pkey = 0;
            if (key_values->type == RAY_DATE || key_values->type == RAY_I32 || key_values->type == RAY_TIME) {
                int32_t v32;
                memcpy(&v32, (char*)ray_data(key_values) + p * sizeof(int32_t), sizeof(int32_t));
                pkey = v32;
            } else {
                memcpy(&pkey, (char*)ray_data(key_values) + p * sizeof(int64_t), sizeof(int64_t));
            }

            bool pass = false;
            if (is_in || is_nin) {
                bool found = false;
                for (int64_t j = 0; j < set_len; j++) {
                    if (pkey == set_vals[j]) { found = true; break; }
                }
                pass = is_in ? found : !found;
            } else {
                switch (eff_op) {
                    case OP_EQ: pass = (pkey == const_val); break;
                    case OP_NE: pass = (pkey != const_val); break;
                    case OP_LT: pass = (pkey <  const_val); break;
                    case OP_GT: pass = (pkey >  const_val); break;
                    case OP_LE: pass = (pkey <= const_val); break;
                    case OP_GE: pass = (pkey >= const_val); break;
                    default: break;
                }
            }
            if (pass)
                mask[p / 64] |= (1ULL << (p % 64));
        }
        if (set_heap) ray_free(set_heap);
    attach_mask:;

        /* Attach seg_mask to OP_SCAN nodes reading parted columns from same table.
         * When !any_active the mask is all-zeros — attach it anyway so the
         * segment loop in ray_execute skips all segments and hits the
         * empty-table path instead of reading every partition. */
        bool mask_owned = false;
        for (uint32_t s = 0; s < g->node_count; s++) {
            ray_op_t* sn = &g->nodes[s];
            if (sn->flags & OP_FLAG_DEAD || sn->opcode != OP_SCAN) continue;
            if (sn == scan_node) continue;

            ray_op_ext_t* sn_ext = find_ext(g, sn->id);
            if (!sn_ext) continue;

            uint16_t sn_tid = 0;
            memcpy(&sn_tid, sn_ext->base.pad, sizeof(uint16_t));
            if (sn_tid != stored_table_id) continue;

            ray_t* sn_col = ray_table_get_col(tbl, sn_ext->sym);
            if (!sn_col || !RAY_IS_PARTED(sn_col->type)) continue;

            if (sn_ext->seg_mask) {
                /* AND with existing mask (conjunctive filters) */
                uint32_t exist_w = (uint32_t)((sn_ext->seg_mask_count + 63) / 64);
                uint32_t min_w = n_words < exist_w ? n_words : exist_w;
                for (uint32_t w = 0; w < min_w; w++)
                    sn_ext->seg_mask[w] &= mask[w];
                /* Zero out words beyond new mask (prune extra segments) */
                for (uint32_t w = min_w; w < exist_w; w++)
                    sn_ext->seg_mask[w] = 0;
                /* Tighten count to the smaller partition set */
                if (n_parts < sn_ext->seg_mask_count)
                    sn_ext->seg_mask_count = n_parts;
            } else {
                sn_ext->seg_mask = mask;
                sn_ext->seg_mask_count = n_parts;
                mask_owned = true;
            }
        }
        if (!mask_owned) ray_sys_free(mask);

        n->est_rows = 1;
    }
}

/* --------------------------------------------------------------------------
 * ray_optimize — run all passes in order, return (possibly updated) root
 * -------------------------------------------------------------------------- */

ray_op_t* ray_optimize(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return root;

    ray_profile_span_start("optimize");

    /* Pass 1: Type inference */
    pass_type_inference(g, root);
    ray_profile_tick("type inference");

    /* Pass 2: Constant folding */
    pass_constant_fold(g, root);
    ray_profile_tick("constant fold");

    /* Pass 3: SIP (graph-aware sideways information passing) */
    sip_pass(g, root);
    ray_profile_tick("SIP");

    /* Pass 4: Factorized detection (OP_EXPAND → OP_GROUP optimization) */
    factorize_pass(g, root);
    ray_profile_tick("factorize");

    /* Pass 5: Predicate pushdown (may change root) */
    root = pass_predicate_pushdown(g, root);
    ray_profile_tick("predicate pushdown");

    /* Pass 6: Filter reordering (split ANDs + reorder by cost, may change root) */
    root = pass_filter_reorder(g, root);
    ray_profile_tick("filter reorder");

    /* Pass 7: Projection pushdown (mark unreachable nodes dead) */
    bool proj_ok = pass_projection_pushdown(g, root);
    ray_profile_tick("projection pushdown");

    /* Pass 8: Partition pruning (set est_rows hints for mapcommon filters).
     * Only safe to run if projection pushdown completed: pruning walks all
     * nodes and would attach seg_masks to disconnected branches otherwise. */
    if (proj_ok)
        pass_partition_pruning(g, root);
    ray_profile_tick("partition pruning");

    /* Pass 9: Fusion */
    ray_fuse_pass(g, root);
    ray_profile_tick("fusion");

    /* Pass 10: DCE */
    pass_dce(g, root);
    ray_profile_tick("DCE");

    ray_profile_span_end("optimize");

    return root;
}

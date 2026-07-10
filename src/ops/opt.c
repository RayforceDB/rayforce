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
#include "idiom.h"
#include "core/profile.h"
#include "mem/sys.h"
#include "mem/heap.h"
#include <float.h>
#include <math.h>
#include <string.h>

/* Forward declaration — defined below, used by type inference and DCE passes. */
ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id);

/* Shared 32-byte node allocator (graph.c).  Forwarded by graph_alloc_node_opt;
 * node ids are realloc-stable so no pointer fixups are needed. */
ray_op_t* graph_alloc_node(ray_graph_t* g);

/* Node-id edge accessors (mirror src/ops/internal.h).  Local copies keep opt.c
 * self-contained — same rationale as the duplicated find_ext below.  Resolve a
 * stable uint32 node id (or input edge) to its ray_op_t*, NULL for RAY_OP_NONE. */
static inline ray_op_t* op_node(ray_graph_t* g, uint32_t id) {
    return id == RAY_OP_NONE ? NULL : &g->nodes[id];
}
static inline ray_op_t* op_child(ray_graph_t* g, const ray_op_t* op, int i) {
    return op_node(g, op->in_id[i]);
}

static inline double opt_f64_fin(double r) {
    return (__builtin_fabs(r) <= DBL_MAX) ? r : NULL_F64;
}

/* Test knob: disable the GROUP predicate-pushdown arm so the differential
 * harness and the perf gate can compare pushed vs unpushed plans. */
bool ray_opt_no_group_pushdown = false;

/* --------------------------------------------------------------------------
 * Optimizer passes: Type Inference + Constant Folding + DCE
 *
 * Expression fusion is handled executor-side by the expr.c bytecode VM.
 *
 *   v1: Type Inference + Constant Folding + DCE
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
    if (a == RAY_F32 || b == RAY_F32)
        return (a == RAY_F32 && b == RAY_F32) ? RAY_F32 : RAY_F64;
    /* Treat SYM/TIMESTAMP/DATE/TIME as integer-class types */
    if (a == RAY_I64 || b == RAY_I64 || a == RAY_SYM || b == RAY_SYM ||
        a == RAY_TIMESTAMP || b == RAY_TIMESTAMP) return RAY_I64;
    if (a == RAY_I32 || b == RAY_I32 ||
        a == RAY_DATE || b == RAY_DATE || a == RAY_TIME || b == RAY_TIME) return RAY_I32;
    if (a == RAY_I16 || b == RAY_I16) return RAY_I16;
    if (a == RAY_U8 || b == RAY_U8) return RAY_U8;
    return RAY_BOOL;
}

static void infer_type_for_node(ray_graph_t* g, ray_op_t* node) {
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
        ray_op_t* in0 = op_child(g, node, 0);
        ray_op_t* in1 = op_child(g, node, 1);
        if (node->arity >= 2 && in0 && in1) {
            node->out_type = promote_type(in0->out_type, in1->out_type);
        } else if (node->arity >= 1 && in0) {
            node->out_type = in0->out_type;
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
            if (op_child(g, n, i) && sp < (int)stack_cap)
                stack[sp++] = n->in_id[i];
        }
        /* M3: Traverse ext node children so type inference reaches all
           referenced nodes (GROUP keys/aggs, SORT/PROJECT/SELECT columns,
           JOIN keys, WINDOW partition/order/func_inputs). */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint32_t k = 0; k < ext->n_keys; k++)
                        if (op_node(g, ext->keys[k]) && !visited[ext->keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->keys[k];
                    for (uint32_t a = 0; a < ext->n_aggs; a++)
                        if (op_node(g, ext->agg_ins[a]) && !visited[ext->agg_ins[a]] && sp < (int)stack_cap)
                            stack[sp++] = ext->agg_ins[a];
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint32_t k = 0; k < ext->sort.n_cols; k++)
                        if (op_node(g, ext->sort.columns[k]) && !visited[ext->sort.columns[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->sort.columns[k];
                    break;
                case OP_JOIN:
                    for (uint32_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (op_node(g, ext->join.left_keys[k]) && !visited[ext->join.left_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.left_keys[k];
                        if (ext->join.right_keys && op_node(g, ext->join.right_keys[k]) &&
                            !visited[ext->join.right_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.right_keys[k];
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (op_node(g, wj_ext->asof.time_key) && !visited[wj_ext->asof.time_key] && sp < (int)stack_cap)
                            stack[sp++] = wj_ext->asof.time_key;
                        for (uint32_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (op_node(g, wj_ext->asof.eq_keys[k]) && !visited[wj_ext->asof.eq_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.eq_keys[k];
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint32_t k = 0; k < ext->window.n_part_keys; k++)
                        if (op_node(g, ext->window.part_keys[k]) && !visited[ext->window.part_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.part_keys[k];
                    for (uint32_t k = 0; k < ext->window.n_order_keys; k++)
                        if (op_node(g, ext->window.order_keys[k]) && !visited[ext->window.order_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.order_keys[k];
                    for (uint32_t f = 0; f < ext->window.n_funcs; f++)
                        if (op_node(g, ext->window.func_inputs[f]) && !visited[ext->window.func_inputs[f]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.func_inputs[f];
                    break;
                /* M3b: 3-input ops store third operand node ID in ext->third_in */
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = ext->third_in;
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
        infer_type_for_node(g, &g->nodes[order[i]]);

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
   self-contained — also present in expr.c. */
ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
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
        case -RAY_F32:
            *out_f = (double)(float)v->f64;
            *out_i = (int64_t)(float)v->f64;
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

static bool pow_atom_type_admitted(ray_t* v) {
    if (!v || !ray_is_atom(v)) return false;
    return v->type == -RAY_BOOL || v->type == -RAY_U8 ||
           v->type == -RAY_I16 || v->type == -RAY_I32 ||
           v->type == -RAY_I64 || v->type == -RAY_F32 ||
           v->type == -RAY_F64;
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
    ext->base.in_id[0] = RAY_OP_NONE;
    ext->base.in_id[1] = RAY_OP_NONE;
    ext->base.out_type = literal->type < 0 ? (int8_t)(-(int)literal->type) : literal->type;
    ext->literal = literal;

    *node = ext->base;
    g->nodes[node->id] = ext->base;
    return true;
}

static bool fold_unary_const(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* operand = op_child(g, node, 0);
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
            if (node->out_type == RAY_F32) folded = ray_f32((float)-vf);
            else if (is_f64) folded = ray_f64(-vf);
            else if (vi == INT64_MIN) return false;  /* -INT64_MIN overflows */
            else folded = ray_i64(-vi);
            break;
        case OP_ABS:
            if (node->out_type == RAY_F32)
                folded = ray_f32(fabsf((float)vf));
            else if (is_f64)
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
        case OP_SIN:
            folded = ray_f64(opt_f64_fin(sin(is_f64 ? vf : (double)vi)));
            break;
        case OP_ASIN:
            folded = ray_f64(opt_f64_fin(asin(is_f64 ? vf : (double)vi)));
            break;
        case OP_COS:
            folded = ray_f64(opt_f64_fin(cos(is_f64 ? vf : (double)vi)));
            break;
        case OP_ACOS:
            folded = ray_f64(opt_f64_fin(acos(is_f64 ? vf : (double)vi)));
            break;
        case OP_TAN:
            folded = ray_f64(opt_f64_fin(tan(is_f64 ? vf : (double)vi)));
            break;
        case OP_ATAN:
            folded = ray_f64(opt_f64_fin(atan(is_f64 ? vf : (double)vi)));
            break;
        case OP_RECIPROCAL: {
            double v = is_f64 ? vf : (double)vi;
            folded = v != 0.0 ? ray_f64(opt_f64_fin(1.0 / v)) : ray_typed_null(-RAY_F64);
            break;
        }
        case OP_SIGNUM: {
            double v = is_f64 ? vf : (double)vi;
            folded = ray_i64((v > 0.0) - (v < 0.0));
            break;
        }
        case OP_CEIL:
            folded = node->out_type == RAY_F32 ? ray_f32(ceilf((float)vf))
                   : is_f64 ? ray_f64(ceil(vf)) : ray_i64(vi);
            break;
        case OP_FLOOR:
            folded = node->out_type == RAY_F32 ? ray_f32(floorf((float)vf))
                   : is_f64 ? ray_f64(floor(vf)) : ray_i64(vi);
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
    ray_op_t* lhs = op_child(g, node, 0);
    ray_op_t* rhs = op_child(g, node, 1);
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
        case RAY_F32: {
            float lv = (float)(l_is_f64 ? lf : (double)li);
            float rv = (float)(r_is_f64 ? rf : (double)ri);
            float r = 0.0f;
            switch (node->opcode) {
                case OP_ADD: r = lv + rv; break;
                case OP_SUB: r = lv - rv; break;
                case OP_MUL: r = lv * rv; break;
                case OP_MOD:
                    if (rv == 0.0f) r = NULL_F32;
                    else {
                        r = fmodf(lv, rv);
                        if (r != 0.0f && ((r > 0.0f) != (rv > 0.0f))) r += rv;
                    }
                    break;
                case OP_MIN2: r = lv < rv ? lv : rv; break;
                case OP_MAX2: r = lv > rv ? lv : rv; break;
                default: return false;
            }
            folded = ray_f32(isfinite(r) ? r : NULL_F32);
            break;
        }
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
                case OP_POW:
                    if (!pow_atom_type_admitted(le->literal) ||
                        !pow_atom_type_admitted(re->literal))
                        return false;
                    r = (lv != lv || rv != rv) ? NULL_F64 : pow(lv, rv);
                    break;
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

/* Try to read an atom-bool value from a CONST node.  Returns false if the
   node is not a const, has no atom literal, or isn't bool-coercible. */
static bool const_node_bool(ray_graph_t* g, ray_op_t* n, bool* out) {
    if (!is_const(n)) return false;
    ray_op_ext_t* ext = find_ext(g, n->id);
    if (!ext || !ext->literal || !ray_is_atom(ext->literal)) return false;
    return atom_to_bool(ext->literal, out);
}

/* Partial fold for (and X c) / (or X c) where exactly one operand is a
   constant scalar bool.  Without this, executing AND/OR with mismatched
   vector + scalar shapes errors with "length".  Identities applied:
       (and X true)  = X         (and X false) = false
       (or  X true)  = true      (or  X false) = X
   Two-const cases are already handled by fold_binary_const. */
static bool simplify_and_or_const(ray_graph_t* g, ray_op_t* node) {
    if (node->opcode != OP_AND && node->opcode != OP_OR) return false;
    if (node->arity != 2) return false;
    ray_op_t* lhs = op_child(g, node, 0);
    ray_op_t* rhs = op_child(g, node, 1);
    if (!lhs || !rhs) return false;

    bool lhs_b = false, rhs_b = false;
    bool lhs_const = const_node_bool(g, lhs, &lhs_b);
    bool rhs_const = const_node_bool(g, rhs, &rhs_b);
    if (lhs_const == rhs_const) return false;  /* both-const handled elsewhere; neither: nothing to do */

    bool c_val   = lhs_const ? lhs_b : rhs_b;
    ray_op_t* X  = lhs_const ? rhs : lhs;

    bool is_and = node->opcode == OP_AND;
    /* Identity arm: AND-with-true / OR-with-false → reduces to X */
    bool identity = is_and ? c_val : !c_val;

    if (identity) {
        /* Replace AND/OR node in-place with an OP_ALIAS of the non-const arm. */
        if (find_ext(g, node->id)) return false;  /* shouldn't happen for AND/OR */
        node->opcode = OP_ALIAS;
        node->arity = 1;
        node->in_id[0] = X ? X->id : RAY_OP_NONE;
        node->in_id[1] = RAY_OP_NONE;
        node->out_type = RAY_BOOL;
        node->est_rows = X->est_rows;
        g->nodes[node->id] = *node;
        return true;
    }

    /* Dominant arm: AND-with-false → false; OR-with-true → true. */
    ray_t* lit = ray_bool(is_and ? false : true);
    if (!lit || RAY_IS_ERR(lit)) {
        if (lit) ray_release(lit);
        return false;
    }
    if (!replace_with_const(g, node, lit)) {
        ray_release(lit);
        return false;
    }
    return true;
}

static bool fold_filter_const_predicate(ray_graph_t* g, ray_op_t* node) {
    if (node->opcode != OP_FILTER || node->arity != 2) return false;
    ray_op_t* pred = op_child(g, node, 1);
    if (!is_const(pred)) return false;

    ray_op_ext_t* pred_ext = find_ext(g, pred->id);
    if (!pred_ext || !pred_ext->literal || !ray_is_atom(pred_ext->literal)) return false;

    bool keep_rows = false;
    if (!atom_to_bool(pred_ext->literal, &keep_rows)) return false;

    if (keep_rows) {
        node->opcode = OP_MATERIALIZE;
        node->arity = 1;
        node->in_id[1] = RAY_OP_NONE;
        g->nodes[node->id] = *node;
        return true;
    }

    ray_op_ext_t* ext = ensure_ext_node(g, node->id);
    if (!ext) return false;
    ext->base = *node;
    ext->base.opcode = OP_HEAD;
    ext->base.arity = 1;
    ext->base.in_id[1] = RAY_OP_NONE;
    ext->base.est_rows = 0;
    ext->sym = 0;

    *node = ext->base;
    g->nodes[node->id] = ext->base;
    return true;
}

static void fold_node(ray_graph_t* g, ray_op_t* node) {
    /* Fold unary element-wise ops with constant input */
    if (node->arity == 1 &&
        ((node->opcode >= OP_NEG && node->opcode <= OP_FLOOR) ||
         (node->opcode >= OP_SIN && node->opcode <= OP_SIGNUM))) {
        (void)fold_unary_const(g, node);
    }
    /* Fold binary element-wise ops with two const inputs */
    if (node->arity == 2 &&
        ((node->opcode >= OP_ADD && node->opcode <= OP_MAX2) ||
         node->opcode == OP_POW)) {
        (void)fold_binary_const(g, node);
    }
    /* Partial-fold AND/OR when one operand is a const scalar bool.  Must run
       before fold_filter_const_predicate so the FILTER pass sees the simpler
       form (and can finish the job if both arms collapsed to const). */
    (void)simplify_and_or_const(g, node);
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
            if (op_child(g, n, i) && sp < (int)stack_cap)
                stack[sp++] = n->in_id[i];
        }
        /* H1: Traverse ext-node children so constant folding reaches all
           referenced nodes (GROUP keys/aggs, SORT/PROJECT/SELECT columns,
           JOIN keys, WINDOW partition/order/func_inputs). */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint32_t k = 0; k < ext->n_keys; k++)
                        if (op_node(g, ext->keys[k]) && !visited[ext->keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->keys[k];
                    for (uint32_t a = 0; a < ext->n_aggs; a++)
                        if (op_node(g, ext->agg_ins[a]) && !visited[ext->agg_ins[a]] && sp < (int)stack_cap)
                            stack[sp++] = ext->agg_ins[a];
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint32_t k = 0; k < ext->sort.n_cols; k++)
                        if (op_node(g, ext->sort.columns[k]) && !visited[ext->sort.columns[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->sort.columns[k];
                    break;
                case OP_JOIN:
                    for (uint32_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (op_node(g, ext->join.left_keys[k]) && !visited[ext->join.left_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.left_keys[k];
                        if (ext->join.right_keys && op_node(g, ext->join.right_keys[k]) &&
                            !visited[ext->join.right_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->join.right_keys[k];
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (op_node(g, wj_ext->asof.time_key) && !visited[wj_ext->asof.time_key] && sp < (int)stack_cap)
                            stack[sp++] = wj_ext->asof.time_key;
                        for (uint32_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (op_node(g, wj_ext->asof.eq_keys[k]) && !visited[wj_ext->asof.eq_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.eq_keys[k];
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint32_t k = 0; k < ext->window.n_part_keys; k++)
                        if (op_node(g, ext->window.part_keys[k]) && !visited[ext->window.part_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.part_keys[k];
                    for (uint32_t k = 0; k < ext->window.n_order_keys; k++)
                        if (op_node(g, ext->window.order_keys[k]) && !visited[ext->window.order_keys[k]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.order_keys[k];
                    for (uint32_t f = 0; f < ext->window.n_funcs; f++)
                        if (op_node(g, ext->window.func_inputs[f]) && !visited[ext->window.func_inputs[f]] && sp < (int)stack_cap)
                            stack[sp++] = ext->window.func_inputs[f];
                    break;
                /* H1b: 3-input ops store third operand node ID in ext->third_in */
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = ext->third_in;
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
            if (op_child(g, n, i) && sp < (int)stack_cap)
                stack[sp++] = n->in_id[i];
        }
        /* H4: 3-input ops (OP_IF, OP_SUBSTR, OP_REPLACE) store the third
           operand node ID in ext->third_in. */
        if (n->opcode == OP_IF || n->opcode == OP_SUBSTR || n->opcode == OP_REPLACE) {
            ray_op_ext_t* ext = find_ext(g, nid);
            if (ext) {
                uint32_t third_id = ext->third_in;
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
                        for (uint32_t k = 0; k < ext->n_keys; k++) {
                            if (op_node(g, ext->keys[k]) && !live[ext->keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->keys[k];
                        }
                        for (uint32_t a = 0; a < ext->n_aggs; a++) {
                            if (op_node(g, ext->agg_ins[a]) && !live[ext->agg_ins[a]] && sp < (int)stack_cap)
                                stack[sp++] = ext->agg_ins[a];
                        }
                        break;
                    case OP_SORT:
                    case OP_SELECT:
                        for (uint32_t k = 0; k < ext->sort.n_cols; k++) {
                            if (op_node(g, ext->sort.columns[k]) && !live[ext->sort.columns[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->sort.columns[k];
                        }
                        break;
                    case OP_JOIN:
                    case OP_ANTIJOIN:
                        for (uint32_t k = 0; k < ext->join.n_join_keys; k++) {
                            if (op_node(g, ext->join.left_keys[k]) && !live[ext->join.left_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->join.left_keys[k];
                            if (ext->join.right_keys && op_node(g, ext->join.right_keys[k]) &&
                                !live[ext->join.right_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->join.right_keys[k];
                        }
                        break;
                    case OP_PIVOT:
                        for (uint32_t k = 0; k < ext->pivot.n_index; k++) {
                            if (op_node(g, ext->pivot.index_cols[k]) && !live[ext->pivot.index_cols[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->pivot.index_cols[k];
                        }
                        if (op_node(g, ext->pivot.pivot_col) && !live[ext->pivot.pivot_col] && sp < (int)stack_cap)
                            stack[sp++] = ext->pivot.pivot_col;
                        if (op_node(g, ext->pivot.value_col) && !live[ext->pivot.value_col] && sp < (int)stack_cap)
                            stack[sp++] = ext->pivot.value_col;
                        break;
                    case OP_WINDOW_JOIN: {
                        ray_op_ext_t* wj_ext = find_ext(g, n->id);
                        if (wj_ext) {
                            if (op_node(g, wj_ext->asof.time_key) && !live[wj_ext->asof.time_key] && sp < (int)stack_cap)
                                stack[sp++] = wj_ext->asof.time_key;
                            for (uint32_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                                if (op_node(g, wj_ext->asof.eq_keys[k]) && !live[wj_ext->asof.eq_keys[k]] && sp < (int)stack_cap)
                                    stack[sp++] = wj_ext->asof.eq_keys[k];
                            }
                        }
                        break;
                    }
                    case OP_WINDOW:
                        for (uint32_t k = 0; k < ext->window.n_part_keys; k++) {
                            if (op_node(g, ext->window.part_keys[k]) && !live[ext->window.part_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.part_keys[k];
                        }
                        for (uint32_t k = 0; k < ext->window.n_order_keys; k++) {
                            if (op_node(g, ext->window.order_keys[k]) && !live[ext->window.order_keys[k]] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.order_keys[k];
                        }
                        for (uint32_t f = 0; f < ext->window.n_funcs; f++) {
                            if (op_node(g, ext->window.func_inputs[f]) && !live[ext->window.func_inputs[f]] && sp < (int)stack_cap)
                                stack[sp++] = ext->window.func_inputs[f];
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
            if (n->in_id[j] == node_id)
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
        ray_op_t* in0 = op_child(g, node, 0);
        if (node->arity > 0 && in0)
            node = in0;
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
        ray_op_t* expand_in0 = op_child(g, expand, 0);
        if (expand->arity > 0 && expand_in0)
            src_scan = find_upstream_scan(g, expand_in0);

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
        if (!grp_ext || grp_ext->n_keys != 1 || !op_node(g, grp_ext->keys[0])) continue;

        ray_op_ext_t* key_ext = find_ext(g, grp_ext->keys[0]);
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
 * Forwards to graph.c's graph_alloc_node — node ids are realloc-stable, so
 * no pointer fixups are needed and there is no second copy of the logic. */
ray_op_t* graph_alloc_node_opt(ray_graph_t* g) {
    return graph_alloc_node(g);
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
            if (c->in_id[k] == node_id) {
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
            if (c->in_id[k] == node_id) {
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
            if (op_child(g, node, i)) {
                if (sp >= 64) return -1;  /* stack overflow */
                stack[sp++] = node->in_id[i];
            }
        }
        /* Walk ext-stored operands for multi-input ops */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (node->opcode) {
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = ext->third_in;
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
            if (op_child(g, node, i) && sp < 64)
                stack[sp++] = node->in_id[i];
        }
        /* Walk ext-stored operands for multi-input ops */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (node->opcode) {
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = ext->third_in;
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
 * Updates both g->nodes[] and g->ext_nodes[].base.in_id[]. */
void redirect_consumers(ray_graph_t* g, uint32_t old_id,
                        ray_op_t* new_target,
                        uint32_t skip_a, uint32_t skip_b) {
    uint32_t nc = g->node_count;
    for (uint32_t j = 0; j < nc; j++) {
        ray_op_t* c = &g->nodes[j];
        if (c->flags & OP_FLAG_DEAD || j == skip_a || j == skip_b) continue;
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->in_id[k] == old_id)
                c->in_id[k] = new_target ? new_target->id : RAY_OP_NONE;
        }
    }
    /* Also update ext_node heap copies to keep them in sync */
    for (uint32_t j = 0; j < g->ext_count; j++) {
        if (!g->ext_nodes[j]) continue;
        ray_op_t* c = &g->ext_nodes[j]->base;
        if (c->flags & OP_FLAG_DEAD) continue;
        if (c->id == skip_a || c->id == skip_b) continue;
        for (int k = 0; k < c->arity && k < 2; k++) {
            if (c->in_id[k] == old_id)
                c->in_id[k] = new_target ? new_target->id : RAY_OP_NONE;
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

            ray_op_t* child = op_child(g, n, 0);
            ray_op_t* pred  = op_child(g, n, 1);
            if (!child || !pred) continue;

            /* Push past SELECT/ALIAS (only if child is single-consumer,
             * otherwise mutating child->inputs[0] would corrupt other branches) */
            if (child->opcode == OP_SELECT ||
                child->opcode == OP_ALIAS) {
                if (count_node_consumers(g, child->id) > 1) continue;
                /* Swap: FILTER(pred, SELECT(x)) -> SELECT(FILTER(pred, x)) */
                ray_op_t* proj_input = op_child(g, child, 0);
                n->in_id[0] = proj_input ? proj_input->id : RAY_OP_NONE;
                child->in_id[0] = n->id;
                redirect_consumers(g, n->id, child, child->id, n->id);
                if (n->id == root->id) root = child;
                changed = true;
                continue;
            }

            /* Push past GROUP — legal only for HAVING predicates that
             * reference group KEY columns exclusively, each key a plain
             * OP_SCAN (key output column name == input column name, so the
             * predicate resolves identically below the group; every row of
             * a group shares its key value, so group-passes-HAVING iff
             * rows-pass-filter).  Agg-output references (v_sum etc.) match
             * no key sym and correctly refuse the push.  The executor
             * honours the interposed filter (exec.c OP_GROUP dispatch). */
            if (child->opcode == OP_GROUP && !ray_opt_no_group_pushdown) {
                if (!g->table) continue;
                if (count_node_consumers(g, child->id) > 1) continue;
                ray_op_ext_t* gext = find_ext(g, child->id);
                if (!gext || gext->n_keys == 0) continue;
                /* one pushed filter per GROUP (chained HAVING stays above) */
                ray_op_t* child_in0 = op_child(g, child, 0);
                if (child_in0 && child_in0->opcode == OP_FILTER)
                    continue;
                /* factorized-expand pipeline (key `_src`) reads the expand
                 * output, not g->table — never push into it */
                {
                    int64_t src_sym = ray_sym_intern("_src", 4);
                    bool is_fact = false;
                    for (uint32_t kk = 0; kk < gext->n_keys; kk++) {
                        ray_op_ext_t* ke = op_node(g, gext->keys[kk])
                            ? find_ext(g, gext->keys[kk]) : NULL;
                        if (ke && ke->base.opcode == OP_SCAN &&
                            ke->sym == src_sym) { is_fact = true; break; }
                    }
                    if (is_fact) continue;
                }
                uint32_t scan_ids[64];
                int n_scans = collect_pred_scans(g, pred, scan_ids, 64);
                if (n_scans <= 0) continue;
                bool keys_only = true;
                for (int s = 0; s < n_scans && keys_only; s++) {
                    ray_op_ext_t* sext = find_ext(g, scan_ids[s]);
                    if (!sext) { keys_only = false; break; }
                    bool matched = false;
                    for (uint32_t kk = 0; kk < gext->n_keys; kk++) {
                        ray_op_t* kop = op_node(g, gext->keys[kk]);
                        if (!kop || kop->opcode != OP_SCAN) continue;
                        ray_op_ext_t* kext = find_ext(g, gext->keys[kk]);
                        if (kext && kext->sym == sext->sym) { matched = true; break; }
                    }
                    if (!matched) keys_only = false;
                }
                if (!keys_only) continue;
                /* Rewire: FILTER's data input becomes a const-table source;
                 * GROUP's inputs[0] (normally the first key op — keys stay
                 * reachable via ext->keys) becomes the FILTER; consumers of
                 * FILTER consume GROUP.  Both dense and ext copies of
                 * GROUP's inputs[0] must be updated (graph.c:843 copies).
                 *
                 * REALLOC SAFETY: ray_const_table allocates a new node and
                 * may realloc g->nodes.  graph_fixup_ptrs patches all stored
                 * pointers inside g->nodes[] and ext nodes, but our local C
                 * variables n/child/pred are NOT fixed up.  Save node ids
                 * and re-derive pointers after the allocation. */
                uint32_t n_id     = n->id;
                uint32_t child_id = child->id;
                /* Save root->id before ray_const_table: that call may realloc
                 * g->nodes, invalidating the root pointer itself. */
                uint32_t root_id  = root->id;
                ray_op_t* tbl_node = ray_const_table(g, g->table);
                if (!tbl_node) continue;
                /* Re-derive pointers (realloc may have moved g->nodes) */
                n     = &g->nodes[n_id];
                child = &g->nodes[child_id];
                gext  = find_ext(g, child_id);
                if (!gext) continue;
                n->in_id[0] = tbl_node->id;
                /* Mark the interposed filter so pass_filter_reorder leaves
                 * it alone: GROUP's arity is 0, so redirect_consumers would
                 * never re-point GROUP->inputs[0] at a split-off outer
                 * filter — an AND-split below the group silently drops a
                 * conjunct.  (OP_FILTER is dense-only; no ext copy.) */
                n->flags |= OP_FLAG_PUSHED;
                child->in_id[0] = n->id;        /* dense copy */
                gext->base.in_id[0] = n->id;    /* ext copy in sync */
                redirect_consumers(g, n_id, child, child_id, n_id);
                if (n_id == root_id) root = child;
                changed = true;
                continue;
            }

            /* Push past EXPAND (source-side predicates, single-consumer only) */
            if (child->opcode == OP_EXPAND) {
                if (count_node_consumers(g, child->id) > 1) continue;
                uint32_t scan_ids[64];
                int n_scans = collect_pred_scans(g, pred, scan_ids, 64);
                if (n_scans <= 0) continue;  /* 0 = no scans, -1 = truncated */

                /* All predicate scans must be reachable from the expand's
                 * source input (inputs[0]).  Walk the source subtree. */
                ray_op_t* expand_src_tree = op_child(g, child, 0);
                bool all_source = true;
                for (int s = 0; s < n_scans; s++) {
                    if (!is_reachable_from(g, expand_src_tree, scan_ids[s])) {
                        all_source = false;
                        break;
                    }
                }
                if (!all_source) continue;

                /* Swap: FILTER(pred, EXPAND(src, rel)) -> EXPAND(FILTER(pred, src), rel) */
                ray_op_t* expand_src = op_child(g, child, 0);
                n->in_id[0] = expand_src ? expand_src->id : RAY_OP_NONE;
                child->in_id[0] = n->id;
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
    if (!pred) return 99;
    int cost = 0;

    /* Constant comparison: one input is OP_CONST */
    bool has_const = false;
    for (int i = 0; i < pred->arity && i < 2; i++) {
        ray_op_t* in = op_child(g, pred, i);
        if (in && in->opcode == OP_CONST)
            has_const = true;
    }
    if (!has_const) cost += 4;  /* col-col comparison */

    /* Type width cost */
    int8_t t = pred->out_type;
    ray_op_t* pin0 = op_child(g, pred, 0);
    if (pred->arity >= 1 && pin0)
        t = pin0->out_type;
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

/* Static selectivity rank for a filter predicate: lower = more selective =
 * should run earlier (inner).  filter_cost above orders by per-row eval cost
 * only, which runs a cheap but non-selective predicate (e.g. a BOOL/U8 flag
 * `== 0`, ~half the rows) before a super-selective one (e.g. an I64 hash
 * `== K`, a handful of rows), bloating every later filter's input and gather.
 * This estimates the pass-fraction from the predicate's static shape so the
 * reorder runs the most-selective predicate first.  Direction-only, not a
 * calibrated fraction (data-backed estimation is a separate change). */
/* Only a high-cardinality equality is a CONFIDENT static selectivity signal.
 * Range / != / LIKE / narrow-flag-equality have data-dependent selectivity we
 * cannot estimate statically, so they map to SEL_NEUTRAL and the composite key
 * defers their ordering to filter_cost (which already runs expensive predicates
 * like LIKE last). An earlier table that ranked LIKE/!= as distinct buckets
 * regressed LIKE-heavy queries by running the substring scan first. */
enum {
    SEL_EQ_WIDE = 0,   /* == / in on I64/F64/STR: inherently large value space */
    SEL_EQ_MED  = 2,   /* == / in on I32/DATE/TIME: usually selective */
    SEL_NEUTRAL = 5,   /* everything else: unknown/data-dependent — defer to filter_cost */
};

static int filter_selectivity_rank(ray_graph_t* g, ray_op_t* pred) {
    if (!pred) return SEL_NEUTRAL;

    /* Find the column operand (the non-const side) and whether a constant is
     * present at all.  Without a constant there is nothing to estimate. */
    bool has_const = false;
    int8_t col_type = pred->out_type;
    for (int i = 0; i < pred->arity && i < 2; i++) {
        ray_op_t* in = op_child(g, pred, i);
        if (!in) continue;
        if (in->opcode == OP_CONST) has_const = true;
        else col_type = in->out_type;
    }
    if (!has_const) return SEL_NEUTRAL;

    /* SYM is deliberately excluded from `wide`: it is dictionary-encoded, so
     * its cardinality is the domain size (a low-card categorical may store as
     * W8, a high-card one as W64).  That width/cardinality lives on the bound
     * column, which this pass cannot see — only the logical RAY_SYM type — so
     * assuming SYM equality is selective is unjustified.  Treat it as neutral
     * and let filter_cost order it (consistent with the conservative rule:
     * reorder only on a CONFIDENT static signal). */
    bool wide   = (col_type == RAY_I64 || col_type == RAY_F64 ||
                   col_type == RAY_STR);
    bool narrow = (col_type == RAY_BOOL || col_type == RAY_U8 ||
                   col_type == RAY_I16);

    switch (pred->opcode) {
        case OP_EQ:
        case OP_IN:
            if (wide)   return SEL_EQ_WIDE;     /* I64/F64/STR */
            if (narrow) return SEL_NEUTRAL;     /* BOOL/U8/I16 flags: non-selective */
            if (col_type == RAY_SYM) return SEL_NEUTRAL;  /* card unknown statically */
            return SEL_EQ_MED;                  /* I32/DATE/TIME and other mid types */
        default:
            return SEL_NEUTRAL;                 /* range, !=, like/ilike, col-col, unknown */
    }
}

/* Composite reorder key: selectivity dominates, filter_cost breaks ties.
 * K (100) exceeds any filter_cost (max ~11), so equal-selectivity predicates
 * fall back to the prior cheap-first ordering. */
static int filter_rank_key(ray_graph_t* g, ray_op_t* pred) {
    return filter_selectivity_rank(g, pred) * 100 + filter_cost(g, pred);
}

/* Split FILTER(AND(a, b), input) into FILTER(a, FILTER(b, input)).
 * Returns the new outer filter node, or the original if no split. */
static ray_op_t* split_and_filter(ray_graph_t* g, ray_op_t* filter_node) {
    if (!filter_node || filter_node->opcode != OP_FILTER) return filter_node;
    if (filter_node->arity != 2) return filter_node;

    ray_op_t* pred = op_child(g, filter_node, 1);
    if (!pred || pred->opcode != OP_AND || pred->arity != 2) return filter_node;

    ray_op_t* pred_a = op_child(g, pred, 0);
    ray_op_t* pred_b = op_child(g, pred, 1);
    ray_op_t* input  = op_child(g, filter_node, 0);
    if (!pred_a || !pred_b || !input) return filter_node;

    /* Don't split a FILTER whose child is a GROUP.  Splitting an AND here
     * produces FILTER(b, FILTER(a, GROUP)); exec's HAVING fusion swaps the
     * table to the GROUP output only for a FILTER whose DIRECT child is GROUP,
     * so the outer conjunct would evaluate against the base table (schema error
     * on an agg-output ref, row-count mismatch on a key ref).  Left intact, the
     * single FILTER(AND, GROUP) is handled correctly by the HAVING fusion. */
    if (input->opcode == OP_GROUP) return filter_node;

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
    filter_node->in_id[1] = pred_a->id;

    outer->opcode = OP_FILTER;
    outer->arity = 2;
    outer->in_id[0] = filter_node->id;
    outer->in_id[1] = pred_b->id;
    outer->out_type = filter_node->out_type;
    outer->est_rows = filter_node->est_rows;

    return outer;
}

/* Collect a chain of OP_FILTER nodes. Returns count (max 64).
 * Stops before PUSHED filters: a filter interposed below a GROUP must
 * never have its predicate reordered into (or out of) an above-group
 * chain.  (Pushed filters sit below GROUP with a const-table input, so
 * they can't normally appear inside an above-group chain — this is a
 * defensive stop.) */
static int collect_filter_chain(ray_graph_t* g, ray_op_t* top, ray_op_t** chain, int max) {
    int n = 0;
    ray_op_t* cur = top;
    while (cur && cur->opcode == OP_FILTER &&
           !(cur->flags & OP_FLAG_PUSHED) && n < max) {
        chain[n++] = cur;
        cur = op_child(g, cur, 0);
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
            /* Never split a PUSHED filter (interposed below a GROUP by
             * predicate pushdown).  Its AND evaluates as ONE fused BOOL
             * pass through the lazy-selection path; splitting it would
             * need a GROUP.inputs[0] redirection that redirect_consumers'
             * arity gate (GROUP arity is 0) doesn't perform — the outer
             * split filter would be orphaned and a conjunct silently
             * dropped — and would also cost two rowsel passes instead
             * of one. */
            if (n->flags & OP_FLAG_PUSHED) continue;
            ray_op_t* n_in1 = op_child(g, n, 1);
            if (n->arity != 2 || !n_in1) continue;
            if (n_in1->opcode != OP_AND) continue;

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
        /* PUSHED filters (below a GROUP) are not reorder candidates:
         * their single fused predicate must stay exactly where pass-6
         * put it (see split-loop comment above). */
        if (n->flags & OP_FLAG_PUSHED) continue;
        if (visited[i]) continue;

        /* Collect the filter chain starting at this node */
        ray_op_t* chain[64];
        int chain_len = collect_filter_chain(g, n, chain, 64);
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

        /* Score each filter's predicate: selectivity-primary, eval-cost tiebreak.
         * The insertion sort below is unchanged — it sorts these keys
         * descending, so least-selective lands outer (runs last) and
         * most-selective lands inner (runs first). */
        int costs[64];
        for (int c = 0; c < chain_len; c++)
            costs[c] = filter_rank_key(g, op_child(g, chain[c], 1));

        /* Insertion sort predicates by rank key descending (stable: preserves
         * original order for equal keys). Highest key — least selective, or
         * equally selective but costlier — goes to chain[0] (outer, runs
         * last); lowest key — most selective — goes to chain[N-1] (inner,
         * runs first). We swap predicates, not filter nodes. */
        for (int c = 1; c < chain_len; c++) {
            uint32_t pred_id = chain[c]->in_id[1];
            int cost = costs[c];
            int j = c - 1;
            while (j >= 0 && costs[j] < cost) {
                chain[j + 1]->in_id[1] = chain[j]->in_id[1];
                costs[j + 1] = costs[j];
                j--;
            }
            chain[j + 1]->in_id[1] = pred_id;
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
            if (op_child(g, n, i) && !live[n->in_id[i]]) {
                live[n->in_id[i]] = true;
                if (qt < (int)nc) q[qt++] = n->in_id[i];
            }
        }

        /* Follow ext node children (mirrors pass_type_inference traversal) */
        ray_op_ext_t* ext = find_ext(g, nid);
        if (ext) {
            switch (n->opcode) {
                case OP_GROUP:
                    for (uint32_t k = 0; k < ext->n_keys; k++)
                        if (op_node(g, ext->keys[k]) && !live[ext->keys[k]]) {
                            live[ext->keys[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->keys[k];
                        }
                    for (uint32_t a = 0; a < ext->n_aggs; a++)
                        if (op_node(g, ext->agg_ins[a]) && !live[ext->agg_ins[a]]) {
                            live[ext->agg_ins[a]] = true;
                            if (qt < (int)nc) q[qt++] = ext->agg_ins[a];
                        }
                    break;
                case OP_SORT:
                case OP_SELECT:
                    for (uint32_t k = 0; k < ext->sort.n_cols; k++)
                        if (op_node(g, ext->sort.columns[k]) && !live[ext->sort.columns[k]]) {
                            live[ext->sort.columns[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->sort.columns[k];
                        }
                    break;
                case OP_JOIN:
                    for (uint32_t k = 0; k < ext->join.n_join_keys; k++) {
                        if (op_node(g, ext->join.left_keys[k]) && !live[ext->join.left_keys[k]]) {
                            live[ext->join.left_keys[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->join.left_keys[k];
                        }
                        if (ext->join.right_keys && op_node(g, ext->join.right_keys[k]) &&
                            !live[ext->join.right_keys[k]]) {
                            live[ext->join.right_keys[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->join.right_keys[k];
                        }
                    }
                    break;
                case OP_WINDOW_JOIN: {
                    ray_op_ext_t* wj_ext = find_ext(g, n->id);
                    if (wj_ext) {
                        if (op_node(g, wj_ext->asof.time_key) && !live[wj_ext->asof.time_key]) {
                            live[wj_ext->asof.time_key] = true;
                            if (qt < (int)nc) q[qt++] = wj_ext->asof.time_key;
                        }
                        for (uint32_t k = 0; k < wj_ext->asof.n_eq_keys; k++) {
                            if (op_node(g, wj_ext->asof.eq_keys[k]) && !live[wj_ext->asof.eq_keys[k]]) {
                                live[wj_ext->asof.eq_keys[k]] = true;
                                if (qt < (int)nc) q[qt++] = wj_ext->asof.eq_keys[k];
                            }
                        }
                    }
                    break;
                }
                case OP_WINDOW:
                    for (uint32_t k = 0; k < ext->window.n_part_keys; k++)
                        if (op_node(g, ext->window.part_keys[k]) && !live[ext->window.part_keys[k]]) {
                            live[ext->window.part_keys[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.part_keys[k];
                        }
                    for (uint32_t k = 0; k < ext->window.n_order_keys; k++)
                        if (op_node(g, ext->window.order_keys[k]) && !live[ext->window.order_keys[k]]) {
                            live[ext->window.order_keys[k]] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.order_keys[k];
                        }
                    for (uint32_t f = 0; f < ext->window.n_funcs; f++)
                        if (op_node(g, ext->window.func_inputs[f]) && !live[ext->window.func_inputs[f]]) {
                            live[ext->window.func_inputs[f]] = true;
                            if (qt < (int)nc) q[qt++] = ext->window.func_inputs[f];
                        }
                    break;
                case OP_IF:
                case OP_SUBSTR:
                case OP_REPLACE: {
                    uint32_t third_id = ext->third_in;
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

        ray_op_t* pred = op_child(g, n, 1);
        if (!pred || pred->arity != 2) continue;

        uint16_t cmp_op = pred->opcode;
        if (cmp_op != OP_EQ && cmp_op != OP_NE &&
            cmp_op != OP_LT && cmp_op != OP_GT &&
            cmp_op != OP_LE && cmp_op != OP_GE &&
            cmp_op != OP_IN && cmp_op != OP_NOT_IN) continue;

        ray_op_t* lhs = op_child(g, pred, 0);
        ray_op_t* rhs = op_child(g, pred, 1);
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

    /* Pass 3: Idiom rewrite (may replace the root, e.g. count(distinct)
     * → count_distinct on a single-statement chain). */
    ray_profile_span_start("idiom");
    root = ray_idiom_pass(g, root);
    ray_profile_span_end("idiom");
    ray_profile_tick("idiom rewrite");

    /* Pass 4: SIP (graph-aware sideways information passing) */
    sip_pass(g, root);
    ray_profile_tick("sip");

    /* Pass 5: Factorized detection (OP_EXPAND → OP_GROUP optimization) */
    factorize_pass(g, root);
    ray_profile_tick("factorize");

    /* Pass 6: Predicate pushdown (may change root) */
    root = pass_predicate_pushdown(g, root);
    ray_profile_tick("predicate pushdown");

    /* Pass 7: Filter reordering (split ANDs + reorder by cost, may change root) */
    root = pass_filter_reorder(g, root);
    ray_profile_tick("filter reorder");

    /* Pass 8: Projection pushdown (mark unreachable nodes dead) */
    bool proj_ok = pass_projection_pushdown(g, root);
    ray_profile_tick("projection pushdown");

    /* Pass 9: Partition pruning (set est_rows hints for mapcommon filters).
     * Only safe to run if projection pushdown completed: pruning walks all
     * nodes and would attach seg_masks to disconnected branches otherwise. */
    if (proj_ok)
        pass_partition_pruning(g, root);
    ray_profile_tick("partition pruning");

    /* Pass 10: DCE */
    pass_dce(g, root);
    ray_profile_tick("dce");

    ray_profile_span_end("optimize");

    return root;
}

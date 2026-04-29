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

static bool atom_to_numeric(ray_t* atom, double* out_f, int64_t* out_i, bool* out_is_f64) {
    if (!atom || !ray_is_atom(atom)) return false;
    switch (atom->type) {
        case -RAY_F64:
            *out_f = atom->f64;
            *out_i = (int64_t)atom->f64;
            *out_is_f64 = true;
            return true;
        case -RAY_I64:
        case -RAY_SYM:
        case -RAY_DATE:
        case -RAY_TIME:
        case -RAY_TIMESTAMP:
            *out_i = atom->i64;
            *out_f = (double)atom->i64;
            *out_is_f64 = false;
            return true;
        case -RAY_I32:
            *out_i = (int64_t)atom->i32;
            *out_f = (double)atom->i32;
            *out_is_f64 = false;
            return true;
        case -RAY_I16:
            *out_i = (int64_t)atom->i16;
            *out_f = (double)atom->i16;
            *out_is_f64 = false;
            return true;
        case -RAY_U8:
        case -RAY_BOOL:
            *out_i = (int64_t)atom->u8;
            *out_f = (double)atom->u8;
            *out_is_f64 = false;
            return true;
        default:
            return false;
    }
}

/* Evaluate a numeric constant sub-expression from op graph.
 * Supports CONST and arithmetic trees over constant children. */
static bool eval_const_numeric_expr(ray_graph_t* g, ray_op_t* op,
                                    double* out_f, int64_t* out_i, bool* out_is_f64) {
    if (!g || !op || !out_f || !out_i || !out_is_f64) return false;

    if (op->opcode == OP_CONST) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (!ext || !ext->literal) return false;
        return atom_to_numeric(ext->literal, out_f, out_i, out_is_f64);
    }

    if ((op->opcode == OP_NEG || op->opcode == OP_ABS) && op->arity == 1 && op->inputs[0]) {
        double af = 0.0;
        int64_t ai = 0;
        bool a_is_f64 = false;
        if (!eval_const_numeric_expr(g, op->inputs[0], &af, &ai, &a_is_f64)) return false;
        if (a_is_f64 || op->out_type == RAY_F64) {
            double v = a_is_f64 ? af : (double)ai;
            double r = (op->opcode == OP_NEG) ? -v : fabs(v);
            *out_f = r;
            *out_i = (int64_t)r;
            *out_is_f64 = true;
            return true;
        }
        int64_t v = ai;
        /* Unsigned negation avoids UB on INT64_MIN */
        int64_t r = (op->opcode == OP_NEG)
                  ? (int64_t)(-(uint64_t)v)
                  : (v < 0 ? (int64_t)(-(uint64_t)v) : v);
        *out_i = r;
        *out_f = (double)r;
        *out_is_f64 = false;
        return true;
    }

    if (op->arity != 2 || !op->inputs[0] || !op->inputs[1]) return false;
    if (op->opcode < OP_ADD || op->opcode > OP_MAX2) return false;

    double lf = 0.0, rf = 0.0;
    int64_t li = 0, ri = 0;
    bool l_is_f64 = false, r_is_f64 = false;
    if (!eval_const_numeric_expr(g, op->inputs[0], &lf, &li, &l_is_f64)) return false;
    if (!eval_const_numeric_expr(g, op->inputs[1], &rf, &ri, &r_is_f64)) return false;

    if (op->out_type == RAY_F64 || l_is_f64 || r_is_f64 || op->opcode == OP_DIV) {
        double lv = l_is_f64 ? lf : (double)li;
        double rv = r_is_f64 ? rf : (double)ri;
        double r = 0.0;
        switch (op->opcode) {
            case OP_ADD: r = lv + rv; break;
            case OP_SUB: r = lv - rv; break;
            case OP_MUL: r = lv * rv; break;
            case OP_DIV: r = rv != 0.0 ? lv / rv : NAN; break;
            case OP_MOD: { if (rv != 0.0) { r = fmod(lv, rv); if (r && ((r > 0) != (rv > 0))) r += rv; } else { r = NAN; } } break;
            case OP_MIN2: r = lv < rv ? lv : rv; break;
            case OP_MAX2: r = lv > rv ? lv : rv; break;
            default: return false;
        }
        *out_f = r;
        *out_i = (int64_t)r;
        *out_is_f64 = true;
        return true;
    }

    int64_t r = 0;
    switch (op->opcode) {
        case OP_ADD: r = (int64_t)((uint64_t)li + (uint64_t)ri); break;
        case OP_SUB: r = (int64_t)((uint64_t)li - (uint64_t)ri); break;
        case OP_MUL: r = (int64_t)((uint64_t)li * (uint64_t)ri); break;
        case OP_DIV:
            if (ri==0) return false;
            r = li/ri; if ((li^ri)<0 && r*ri!=li) r--;
            break;
        case OP_MOD:
            if (ri==0) return false;
            r = li%ri; if (r && (r^ri)<0) r+=ri;
            break;
        case OP_MIN2: r = li < ri ? li : ri; break;
        case OP_MAX2: r = li > ri ? li : ri; break;
        default: return false;
    }
    *out_i = r;
    *out_f = (double)r;
    *out_is_f64 = false;
    return true;
}

static bool const_expr_to_i64(ray_graph_t* g, ray_op_t* op, int64_t* out) {
    if (!g || !op || !out) return false;
    double c_f = 0.0;
    int64_t c_i = 0;
    bool c_is_f64 = false;
    if (!eval_const_numeric_expr(g, op, &c_f, &c_i, &c_is_f64)) return false;
    if (!c_is_f64) {
        *out = c_i;
        return true;
    }
    if (!isfinite(c_f)) return false;
    double ip = 0.0;
    if (modf(c_f, &ip) != 0.0) return false;
    if (ip > (double)INT64_MAX || ip < (double)INT64_MIN) return false;
    *out = (int64_t)ip;
    return true;
}

static inline bool type_is_linear_i64_col(int8_t t) {
    return t == RAY_I64 || t == RAY_TIMESTAMP ||
           t == RAY_I32 || t == RAY_DATE || t == RAY_TIME || t == RAY_I16 ||
           t == RAY_U8 || t == RAY_BOOL || RAY_IS_SYM(t);
}

static bool linear_expr_add_term(linear_expr_i64_t* e, int64_t sym, int64_t coeff) {
    if (!e) return false;
    if (coeff == 0) return true;
    for (uint8_t i = 0; i < e->n_terms; i++) {
        if (e->syms[i] != sym) continue;
        int64_t next = e->coeff_i64[i] + coeff;
        if (next != 0) {
            e->coeff_i64[i] = next;
            return true;
        }
        for (uint8_t j = i + 1; j < e->n_terms; j++) {
            e->syms[j - 1] = e->syms[j];
            e->coeff_i64[j - 1] = e->coeff_i64[j];
        }
        e->n_terms--;
        return true;
    }
    if (e->n_terms >= AGG_LINEAR_MAX_TERMS) return false;
    e->syms[e->n_terms] = sym;
    e->coeff_i64[e->n_terms] = coeff;
    e->n_terms++;
    return true;
}

static void linear_expr_scale(linear_expr_i64_t* e, int64_t k) {
    if (!e || k == 1) return;
    e->bias_i64 *= k;
    for (uint8_t i = 0; i < e->n_terms; i++)
        e->coeff_i64[i] *= k;
}

static bool linear_expr_add_scaled(linear_expr_i64_t* dst, const linear_expr_i64_t* src, int64_t scale) {
    if (!dst || !src) return false;
    dst->bias_i64 += src->bias_i64 * scale;
    for (uint8_t i = 0; i < src->n_terms; i++) {
        if (!linear_expr_add_term(dst, src->syms[i], src->coeff_i64[i] * scale))
            return false;
    }
    return true;
}

/* Parse an expression tree into integer linear form:
 *   sum(coeff[i] * scan(sym[i])) + bias
 * Supports +, -, unary -, and multiplication by integer constants. */
static bool parse_linear_i64_expr(ray_graph_t* g, ray_op_t* op, linear_expr_i64_t* out) {
    if (!g || !op || !out) return false;
    memset(out, 0, sizeof(*out));

    int64_t c = 0;
    if (const_expr_to_i64(g, op, &c)) {
        out->bias_i64 = c;
        return true;
    }

    if (op->opcode == OP_SCAN) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (!ext || ext->base.opcode != OP_SCAN) return false;
        out->n_terms = 1;
        out->syms[0] = ext->sym;
        out->coeff_i64[0] = 1;
        return true;
    }

    if (op->opcode == OP_NEG && op->arity == 1 && op->inputs[0]) {
        linear_expr_i64_t inner;
        if (!parse_linear_i64_expr(g, op->inputs[0], &inner)) return false;
        linear_expr_scale(&inner, -1);
        *out = inner;
        return true;
    }

    if ((op->opcode == OP_ADD || op->opcode == OP_SUB) &&
        op->arity == 2 && op->inputs[0] && op->inputs[1]) {
        linear_expr_i64_t lhs;
        linear_expr_i64_t rhs;
        if (!parse_linear_i64_expr(g, op->inputs[0], &lhs)) return false;
        if (!parse_linear_i64_expr(g, op->inputs[1], &rhs)) return false;
        *out = lhs;
        return linear_expr_add_scaled(out, &rhs, op->opcode == OP_ADD ? 1 : -1);
    }

    if (op->opcode == OP_MUL && op->arity == 2 && op->inputs[0] && op->inputs[1]) {
        int64_t k = 0;
        linear_expr_i64_t side;
        if (const_expr_to_i64(g, op->inputs[0], &k) &&
            parse_linear_i64_expr(g, op->inputs[1], &side)) {
            linear_expr_scale(&side, k);
            *out = side;
            return true;
        }
        if (const_expr_to_i64(g, op->inputs[1], &k) &&
            parse_linear_i64_expr(g, op->inputs[0], &side)) {
            linear_expr_scale(&side, k);
            *out = side;
            return true;
        }
    }

    return false;
}

/* Detect SUM/AVG integer-linear inputs for scalar aggregate fast path.
 * Example: (v1 + 1) * 2, v1 + v2 + 1 */
bool try_linear_sumavg_input_i64(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                                        agg_linear_t* out_plan) {
    if (!g || !tbl || !input_op || !out_plan) return false;
    linear_expr_i64_t lin;
    if (!parse_linear_i64_expr(g, input_op, &lin)) return false;

    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->n_terms = lin.n_terms;
    out_plan->bias_i64 = lin.bias_i64;
    for (uint8_t i = 0; i < lin.n_terms; i++) {
        ray_t* col = ray_table_get_col(tbl, lin.syms[i]);
        if (!col || !type_is_linear_i64_col(col->type)) return false;
        out_plan->term_ptrs[i] = ray_data(col);
        out_plan->term_types[i] = col->type;
        out_plan->coeff_i64[i] = lin.coeff_i64[i];
    }
    out_plan->enabled = true;
    return true;
}

/* Detect SUM/AVG affine inputs of form (scan +/- const) and return scan vector
 * plus the additive bias so we can adjust results from (sum,count) directly. */
bool try_affine_sumavg_input(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                                    ray_t** out_vec, agg_affine_t* out_affine) {
    if (!g || !tbl || !input_op || !out_vec || !out_affine) return false;
    if (input_op->opcode != OP_ADD && input_op->opcode != OP_SUB) return false;
    if (input_op->arity != 2 || !input_op->inputs[0] || !input_op->inputs[1]) return false;

    ray_op_t* lhs = input_op->inputs[0];
    ray_op_t* rhs = input_op->inputs[1];
    ray_op_t* base_op = NULL;
    int sign = 1;
    double c_f = 0.0;
    int64_t c_i = 0;
    bool c_is_f64 = false;

    double lhs_f = 0.0, rhs_f = 0.0;
    int64_t lhs_i = 0, rhs_i = 0;
    bool lhs_is_f64 = false, rhs_is_f64 = false;
    bool lhs_const = eval_const_numeric_expr(g, lhs, &lhs_f, &lhs_i, &lhs_is_f64);
    bool rhs_const = eval_const_numeric_expr(g, rhs, &rhs_f, &rhs_i, &rhs_is_f64);

    if (input_op->opcode == OP_ADD) {
        if (lhs_const) {
            base_op = rhs;
            sign = 1;
            c_f = lhs_f;
            c_i = lhs_i;
            c_is_f64 = lhs_is_f64;
        } else if (rhs_const) {
            base_op = lhs;
            sign = 1;
            c_f = rhs_f;
            c_i = rhs_i;
            c_is_f64 = rhs_is_f64;
        }
    } else { /* OP_SUB */
        if (rhs_const) {
            base_op = lhs;
            sign = -1;
            c_f = rhs_f;
            c_i = rhs_i;
            c_is_f64 = rhs_is_f64;
        }
    }
    if (!base_op) return false;

    ray_op_ext_t* base_ext = find_ext(g, base_op->id);
    if (!base_ext || base_ext->base.opcode != OP_SCAN) return false;
    ray_t* base_vec = ray_table_get_col(tbl, base_ext->sym);
    if (!base_vec) return false;

    int8_t bt = base_vec->type;
    if (bt == RAY_F64) {
        out_affine->enabled = true;
        out_affine->bias_f64 = (double)sign * (c_is_f64 ? c_f : (double)c_i);
        out_affine->bias_i64 = (int64_t)out_affine->bias_f64;
        *out_vec = base_vec;
        return true;
    }

    if (bt == RAY_I64 || bt == RAY_TIMESTAMP ||
        bt == RAY_I32 || bt == RAY_I16 || bt == RAY_U8 || bt == RAY_BOOL ||
        RAY_IS_SYM(bt)) {
        int64_t c = 0;
        if (c_is_f64) {
            if (!isfinite(c_f)) return false;
            double ip = 0.0;
            if (modf(c_f, &ip) != 0.0) return false;
            if (ip > (double)INT64_MAX || ip < (double)INT64_MIN) return false;
            c = (int64_t)ip;
        } else {
            c = c_i;
        }
        out_affine->enabled = true;
        out_affine->bias_i64 = sign > 0 ? c : -c;
        out_affine->bias_f64 = (double)out_affine->bias_i64;
        *out_vec = base_vec;
        return true;
    }

    return false;
}

/* ============================================================================
 * Expression Compiler: morsel-batched fused evaluation
 *
 * Compiles an expression DAG (e.g. v1 + v2 * 3) into a flat instruction
 * array. Evaluates in morsel-sized chunks (1024 elements) with scratch
 * registers — never allocates full-length intermediate vectors.
 * ============================================================================ */

/* Is this opcode an element-wise op suitable for expression compilation? */
static inline bool expr_is_elementwise(uint16_t op) {
    return (op >= OP_NEG && op <= OP_CAST) || (op >= OP_ADD && op <= OP_MAX2);
}

/* Insert CAST instruction to promote register to target type */
static uint8_t expr_ensure_type(ray_expr_t* out, uint8_t src, int8_t target) {
    if (out->regs[src].type == target) return src;
    if (out->n_regs >= EXPR_MAX_REGS || out->n_ins >= EXPR_MAX_INS) return src;
    uint8_t r = out->n_regs;
    out->regs[r].kind = REG_SCRATCH;
    out->regs[r].type = target;
    out->n_regs++;
    out->n_scratch++;
    out->ins[out->n_ins++] = (expr_ins_t){
        .opcode = OP_CAST, .dst = r, .src1 = src, .src2 = 0xFF,
    };
    return r;
}

/* Compile expression DAG into flat instruction array.
 * Returns true on success. Only compiles element-wise subtrees. */
bool expr_compile(ray_graph_t* g, ray_t* tbl, ray_op_t* root, ray_expr_t* out) {
    memset(out, 0, sizeof(*out));
    if (!root || !g || !tbl) return false;
    if (root->opcode == OP_SCAN || root->opcode == OP_CONST) return false;
    if (!expr_is_elementwise(root->opcode)) return false;

    uint32_t nc = g->node_count;
    if (nc > 4096) return false; /* guard against stack overflow from VLA */
    uint8_t node_reg[nc];
    memset(node_reg, 0xFF, nc * sizeof(uint8_t));

    /* Post-order DFS with explicit stack */
    /* Depth limit 64 — expressions deeper than 64 levels fall back to non-fused path. */
    typedef struct { ray_op_t* node; uint8_t phase; } dfs_t;
    dfs_t dfs[64];
    int sp = 0;
    dfs[sp++] = (dfs_t){root, 0};

    while (sp > 0) {
        dfs_t* top = &dfs[sp - 1];
        ray_op_t* node = top->node;

        if (node->id < nc && node_reg[node->id] != 0xFF) { sp--; continue; }

        if (top->phase == 0) {
            top->phase = 1;
            for (int i = node->arity - 1; i >= 0; i--) {
                ray_op_t* ch = node->inputs[i];
                if (!ch) continue;
                if (ch->id < nc && node_reg[ch->id] != 0xFF) continue;
                if (sp >= 64) return false;
                dfs[sp++] = (dfs_t){ch, 0};
            }
        } else {
            sp--;
            uint8_t r = out->n_regs;
            if (r >= EXPR_MAX_REGS) return false;

            if (node->opcode == OP_SCAN) {
                ray_op_ext_t* ext = find_ext(g, node->id);
                if (!ext) return false;
                ray_t* col = ray_table_get_col(tbl, ext->sym);
                if (!col) return false;
                if (col->type == RAY_MAPCOMMON) return false;
                if (col->type == RAY_STR) return false; /* RAY_STR needs string comparison path */
                if (col->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) return false; /* nullable cols need bitmap-aware path */
                out->regs[r].kind = REG_SCAN;
                if (RAY_IS_PARTED(col->type)) {
                    int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                    out->regs[r].col_type = base;
                    out->regs[r].data = NULL; /* resolved per-segment */
                    out->regs[r].is_parted = true;
                    out->regs[r].parted_col = col;
                    out->regs[r].type = (base == RAY_F64) ? RAY_F64 : RAY_I64;
                    out->has_parted = true;
                } else {
                    out->regs[r].col_type = col->type;
                    out->regs[r].col_attrs = col->attrs;
                    out->regs[r].data = ray_data(col);
                    out->regs[r].is_parted = false;
                    out->regs[r].parted_col = NULL;
                    out->regs[r].type = (col->type == RAY_F64) ? RAY_F64 : RAY_I64;
                }
            } else if (node->opcode == OP_CONST) {
                ray_op_ext_t* ext = find_ext(g, node->id);
                if (!ext || !ext->literal) return false;
                if (RAY_ATOM_IS_NULL(ext->literal)) return false; /* null constants need bitmap-aware path */
                double cf; int64_t ci; bool is_f64;
                if (!atom_to_numeric(ext->literal, &cf, &ci, &is_f64)) {
                    /* Try resolving string constant to symbol intern ID —
                     * enables fused evaluation of SYM column comparisons
                     * (e.g. id2 = 'id080' compiles to integer EQ). */
                    if (ext->literal->type == -RAY_STR) {
                        const char* s = ray_str_ptr(ext->literal);
                        size_t slen = ray_str_len(ext->literal);
                        int64_t sid = ray_sym_find(s, slen);
                        if (sid < 0) return false;
                        ci = sid;
                        cf = (double)sid;
                        is_f64 = false;
                    } else {
                        return false;
                    }
                }
                out->regs[r].kind = REG_CONST;
                out->regs[r].type = is_f64 ? RAY_F64 : RAY_I64;
                out->regs[r].const_f64 = cf;
                out->regs[r].const_i64 = ci;
            } else if (expr_is_elementwise(node->opcode)) {
                if (!node->inputs[0]) return false;
                uint8_t s1 = node_reg[node->inputs[0]->id];
                if (s1 == 0xFF) return false;
                uint8_t s2 = 0xFF;
                if (node->arity >= 2 && node->inputs[1]) {
                    s2 = node_reg[node->inputs[1]->id];
                    if (s2 == 0xFF) return false;
                }

                int8_t t1 = out->regs[s1].type;
                int8_t t2 = (s2 != 0xFF) ? out->regs[s2].type : t1;
                uint16_t op = node->opcode;
                int8_t ot;

                /* Determine output type */
                if (op == OP_CAST)
                    ot = node->out_type;
                else if ((op >= OP_EQ && op <= OP_GE) ||
                    op == OP_AND || op == OP_OR || op == OP_NOT)
                    ot = RAY_BOOL;
                else if (t1 == RAY_F64 || t2 == RAY_F64 || op == OP_DIV ||
                         op == OP_SQRT || op == OP_LOG || op == OP_EXP)
                    ot = RAY_F64;
                else
                    ot = RAY_I64;

                /* Type promotion: ensure both sources match for the operation.
                 * Skip for OP_CAST — the instruction itself IS the conversion. */
                if (op == OP_CAST) {
                    /* No promotion needed; CAST handles the conversion */
                    r = out->n_regs;
                    if (r >= EXPR_MAX_REGS) return false;
                } else if (ot == RAY_F64 && s2 != 0xFF) {
                    /* Arithmetic with f64 output — promote i64 inputs to f64 */
                    s1 = expr_ensure_type(out, s1, RAY_F64);
                    s2 = expr_ensure_type(out, s2, RAY_F64);
                    r = out->n_regs; /* re-read after possible CAST inserts */
                    if (r >= EXPR_MAX_REGS) return false;
                } else if (ot == RAY_F64 && s2 == 0xFF) {
                    /* Unary f64 — promote input */
                    s1 = expr_ensure_type(out, s1, RAY_F64);
                    r = out->n_regs;
                    if (r >= EXPR_MAX_REGS) return false;
                } else if (ot == RAY_BOOL && s2 != 0xFF && t1 != t2) {
                    /* Comparison with mixed types — promote both to f64 */
                    int8_t pt = (t1 == RAY_F64 || t2 == RAY_F64) ? RAY_F64 : RAY_I64;
                    s1 = expr_ensure_type(out, s1, pt);
                    s2 = expr_ensure_type(out, s2, pt);
                    r = out->n_regs;
                    if (r >= EXPR_MAX_REGS) return false;
                }

                out->regs[r].kind = REG_SCRATCH;
                out->regs[r].type = ot;
                out->n_scratch++;

                if (out->n_ins >= EXPR_MAX_INS) return false;
                out->ins[out->n_ins++] = (expr_ins_t){
                    .opcode = (uint8_t)op, .dst = r, .src1 = s1, .src2 = s2,
                };
            } else {
                return false;
            }

            out->n_regs++;
            if (node->id < nc) node_reg[node->id] = r;
        }
    }

    if (out->n_regs == 0 || out->n_ins == 0) return false;
    out->out_reg = out->n_regs - 1;
    out->out_type = out->regs[out->out_reg].type;
    return true;
}

/* ---- Morsel-batched expression evaluator ---- */

/* Load SCAN column data into i64 scratch buffer with type conversion */
static void expr_load_i64(int64_t* dst, const void* data, int8_t col_type,
                          uint8_t col_attrs, int64_t start, int64_t n) {
    switch (col_type) {
        case RAY_I64: case RAY_TIMESTAMP:
            memcpy(dst, (const int64_t*)data + start, (size_t)n * 8);
            break;
        case RAY_SYM: {
            for (int64_t j = 0; j < n; j++)
                dst[j] = ray_read_sym(data, start + j, col_type, col_attrs);
        } break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* s = (const int32_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        case RAY_U8: case RAY_BOOL: {
            const uint8_t* s = (const uint8_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        case RAY_I16: {
            const int16_t* s = (const int16_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        default: memset(dst, 0, (size_t)n * 8); break;
    }
}

/* Load SCAN column data into f64 scratch buffer with type conversion */
static void expr_load_f64(double* dst, const void* data, int8_t col_type,
                          uint8_t col_attrs, int64_t start, int64_t n) {
    switch (col_type) {
        case RAY_F64:
            memcpy(dst, (const double*)data + start, (size_t)n * 8);
            break;
        case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* s = (const int64_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_SYM: {
            for (int64_t j = 0; j < n; j++)
                dst[j] = (double)ray_read_sym(data, start + j, col_type, col_attrs);
        } break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* s = (const int32_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_U8: case RAY_BOOL: {
            const uint8_t* s = (const uint8_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_I16: {
            const int16_t* s = (const int16_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        default: memset(dst, 0, (size_t)n * 8); break;
    }
}

/* Execute a binary instruction over n elements.
 * Switch is OUTSIDE the loop so each case auto-vectorizes. */
static void expr_exec_binary(uint8_t opcode, int8_t dt, void* dp,
                              int8_t t1, const void* ap,
                              int8_t t2, const void* bp, int64_t n) {
    (void)t2;
    if (dt == RAY_F64) {
        double* d = (double*)dp;
        const double* a = (const double*)ap;
        const double* b = (const double*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = a[j] + b[j]; break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = a[j] - b[j]; break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = a[j] * b[j]; break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) d[j] = b[j] != 0.0 ? a[j] / b[j] : NAN; break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) {
                if (b[j] == 0.0) { d[j] = NAN; continue; }
                double m = fmod(a[j], b[j]);
                d[j] = (m && ((m > 0) != (b[j] > 0))) ? m + b[j] : m;
            } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
            default: break;
        }
    } else if (dt == RAY_I64 || dt == RAY_TIMESTAMP) {
        int64_t* d = (int64_t*)dp;
        const int64_t* a = (const int64_t*)ap;
        const int64_t* b = (const int64_t*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] + (uint64_t)b[j]); break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] - (uint64_t)b[j]); break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] * (uint64_t)b[j]); break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==((int64_t)1<<63))) { d[j]=0; continue; }
                int64_t q = a[j]/b[j];
                if ((a[j]^b[j])<0 && q*b[j]!=a[j]) q--;
                d[j] = q;
            } break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==((int64_t)1<<63))) { d[j]=0; continue; }
                int64_t m = a[j]%b[j];
                if (m && (m^b[j])<0) m+=b[j];
                d[j] = m;
            } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
            default: break;
        }
    } else if (dt == RAY_I32 || dt == RAY_DATE || dt == RAY_TIME) {
        int32_t* d = (int32_t*)dp;
        const int32_t* a = (const int32_t*)ap;
        const int32_t* b = (const int32_t*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] + (uint32_t)b[j]); break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] - (uint32_t)b[j]); break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] * (uint32_t)b[j]); break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==((int32_t)1<<31))) { d[j]=0; continue; }
                int32_t q = a[j]/b[j];
                if ((a[j]^b[j])<0 && q*b[j]!=a[j]) q--;
                d[j] = q;
            } break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==((int32_t)1<<31))) { d[j]=0; continue; }
                int32_t m = a[j]%b[j];
                if (m && (m^b[j])<0) m+=b[j];
                d[j] = m;
            } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
            default: break;
        }
    } else if (dt == RAY_I16) {
        int16_t* d = (int16_t*)dp;
        const int16_t* a = (const int16_t*)ap;
        const int16_t* b = (const int16_t*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = (int16_t)((uint16_t)a[j] + (uint16_t)b[j]); break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = (int16_t)((uint16_t)a[j] - (uint16_t)b[j]); break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = (int16_t)((uint16_t)a[j] * (uint16_t)b[j]); break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) { d[j] = b[j] ? a[j] / b[j] : 0; } break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) { d[j] = b[j] ? a[j] % b[j] : 0; } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
            default: break;
        }
    } else if (dt == RAY_U8) {
        uint8_t* d2 = (uint8_t*)dp;
        const uint8_t* a2 = (const uint8_t*)ap;
        const uint8_t* b2 = (const uint8_t*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d2[j] = a2[j] + b2[j]; break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d2[j] = a2[j] - b2[j]; break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d2[j] = a2[j] * b2[j]; break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) { d2[j] = b2[j] ? a2[j] / b2[j] : 0; } break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) { d2[j] = b2[j] ? a2[j] % b2[j] : 0; } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d2[j] = a2[j] < b2[j] ? a2[j] : b2[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d2[j] = a2[j] > b2[j] ? a2[j] : b2[j]; break;
            default: break;
        }
    } else if (dt == RAY_BOOL) {
        uint8_t* d = (uint8_t*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            const double* b = (const double*)bp;
            /* Null-aware F64 comparisons: NaN is null sentinel.
             * null == null → true, null < non-null → true, non-null > null → true */
            #define F64_ISNAN(x) ((x) != (x))
            switch (opcode) {
                case OP_EQ: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 1 : (F64_ISNAN(a[j])||F64_ISNAN(b[j])) ? 0 : a[j]==b[j]; break;
                case OP_NE: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 0 : (F64_ISNAN(a[j])||F64_ISNAN(b[j])) ? 1 : a[j]!=b[j]; break;
                case OP_LT: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 0 : F64_ISNAN(a[j]) ? 1 : F64_ISNAN(b[j]) ? 0 : a[j]<b[j]; break;
                case OP_LE: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 1 : F64_ISNAN(a[j]) ? 1 : F64_ISNAN(b[j]) ? 0 : a[j]<=b[j]; break;
                case OP_GT: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 0 : F64_ISNAN(b[j]) ? 1 : F64_ISNAN(a[j]) ? 0 : a[j]>b[j]; break;
                case OP_GE: for (int64_t j = 0; j < n; j++) d[j] = (F64_ISNAN(a[j])&&F64_ISNAN(b[j])) ? 1 : F64_ISNAN(b[j]) ? 1 : F64_ISNAN(a[j]) ? 0 : a[j]>=b[j]; break;
                default: break;
            }
            #undef F64_ISNAN
        } else if (t1 == RAY_I64) {
            const int64_t* a = (const int64_t*)ap;
            const int64_t* b = (const int64_t*)bp;
            /* Plain comparison — null handling via bitmap post-pass.
             * Values at null positions are zero (from vector init), which
             * compares correctly for null-as-minimum semantics when both
             * input null bitmaps are propagated to the result. */
            switch (opcode) {
                case OP_EQ: for (int64_t j = 0; j < n; j++) d[j] = a[j]==b[j]; break;
                case OP_NE: for (int64_t j = 0; j < n; j++) d[j] = a[j]!=b[j]; break;
                case OP_LT: for (int64_t j = 0; j < n; j++) d[j] = a[j]<b[j]; break;
                case OP_LE: for (int64_t j = 0; j < n; j++) d[j] = a[j]<=b[j]; break;
                case OP_GT: for (int64_t j = 0; j < n; j++) d[j] = a[j]>b[j]; break;
                case OP_GE: for (int64_t j = 0; j < n; j++) d[j] = a[j]>=b[j]; break;
                default: break;
            }
        } else { /* both bool */
            const uint8_t* a = (const uint8_t*)ap;
            const uint8_t* b = (const uint8_t*)bp;
            switch (opcode) {
                case OP_AND: for (int64_t j = 0; j < n; j++) d[j] = a[j] && b[j]; break;
                case OP_OR:  for (int64_t j = 0; j < n; j++) d[j] = a[j] || b[j]; break;
                default: break;
            }
        }
    }
}

/* Execute a unary instruction over n elements */
static void expr_exec_unary(uint8_t opcode, int8_t dt, void* dp,
                             int8_t t1, const void* ap, int64_t n) {
    if (dt == RAY_F64) {
        double* d = (double*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            switch (opcode) {
                case OP_NEG:   for (int64_t j = 0; j < n; j++) d[j] = -a[j]; break;
                case OP_ABS:   for (int64_t j = 0; j < n; j++) d[j] = fabs(a[j]); break;
                case OP_SQRT:  for (int64_t j = 0; j < n; j++) d[j] = sqrt(a[j]); break;
                case OP_LOG:   for (int64_t j = 0; j < n; j++) d[j] = log(a[j]); break;
                case OP_EXP:   for (int64_t j = 0; j < n; j++) d[j] = exp(a[j]); break;
                case OP_CEIL:  for (int64_t j = 0; j < n; j++) d[j] = ceil(a[j]); break;
                case OP_FLOOR: for (int64_t j = 0; j < n; j++) d[j] = floor(a[j]); break;
                case OP_ROUND: for (int64_t j = 0; j < n; j++) d[j] = round(a[j]); break;
                default: break;
            }
        } else { /* CAST i64→f64 */
            const int64_t* a = (const int64_t*)ap;
            for (int64_t j = 0; j < n; j++) d[j] = (double)a[j];
        }
    } else if (dt == RAY_I64) {
        int64_t* d = (int64_t*)dp;
        if (t1 == RAY_I64) {
            const int64_t* a = (const int64_t*)ap;
            switch (opcode) {
                /* Unsigned negation avoids UB on INT64_MIN */
                case OP_NEG: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)(-(uint64_t)a[j]); break;
                case OP_ABS: for (int64_t j = 0; j < n; j++) d[j] = a[j] < 0 ? (int64_t)(-(uint64_t)a[j]) : a[j]; break;
                default: break;
            }
        } else { /* CAST f64→i64 — clamp to avoid out-of-range UB */
            const double* a = (const double*)ap;
            for (int64_t j = 0; j < n; j++)
                d[j] = (a[j] >= (double)INT64_MAX) ? INT64_MAX
                     : (a[j] <= (double)INT64_MIN) ? INT64_MIN
                     : (int64_t)a[j];
        }
    } else if (dt == RAY_BOOL) {
        uint8_t* d = (uint8_t*)dp;
        const uint8_t* a = (const uint8_t*)ap;
        switch (opcode) {
            case OP_NOT: for (int64_t j = 0; j < n; j++) d[j] = !a[j]; break;
            default: break;
        }
    }
}

/* Evaluate compiled expression for morsel [start, end).
 * scratch: array of EXPR_MAX_REGS buffers, each EXPR_MORSEL*8 bytes.
 * Returns pointer to output data (morsel-relative indexing). */
static void* expr_eval_morsel(const ray_expr_t* expr, void** scratch,
                               int64_t start, int64_t end) {
    int64_t n = end - start;
    if (n <= 0) return NULL;

    void* rptrs[EXPR_MAX_REGS];
    for (uint8_t r = 0; r < expr->n_regs; r++) {
        int8_t rt = expr->regs[r].type;
        int8_t ct = expr->regs[r].col_type;
        switch (expr->regs[r].kind) {
            case REG_SCAN: {
                /* Direct pointer if native type matches, else convert */
                uint8_t ca = expr->regs[r].col_attrs;
                if (rt == RAY_F64 && ct == RAY_F64) {
                    rptrs[r] = (double*)expr->regs[r].data + start;
                } else if (rt == RAY_I64 && (ct == RAY_I64 || ct == RAY_TIMESTAMP)) {
                    rptrs[r] = (int64_t*)expr->regs[r].data + start;
                } else if (rt == RAY_I64 && ct == RAY_SYM &&
                           (ca & RAY_SYM_W_MASK) == RAY_SYM_W64) {
                    rptrs[r] = (int64_t*)expr->regs[r].data + start;
                } else {
                    rptrs[r] = scratch[r];
                    if (rt == RAY_F64)
                        expr_load_f64(scratch[r], expr->regs[r].data, ct, ca, start, n);
                    else
                        expr_load_i64(scratch[r], expr->regs[r].data, ct, ca, start, n);
                }
            }
                break;
            case REG_CONST:
                rptrs[r] = scratch[r];
                if (rt == RAY_F64) {
                    double v = expr->regs[r].const_f64;
                    double* d = (double*)scratch[r];
                    for (int64_t j = 0; j < n; j++) d[j] = v;
                } else {
                    int64_t v = expr->regs[r].const_i64;
                    int64_t* d = (int64_t*)scratch[r];
                    for (int64_t j = 0; j < n; j++) d[j] = v;
                }
                break;
            default: /* REG_SCRATCH */
                rptrs[r] = scratch[r];
                break;
        }
    }

    for (uint8_t i = 0; i < expr->n_ins; i++) {
        const expr_ins_t* ins = &expr->ins[i];
        int8_t dt = expr->regs[ins->dst].type;
        if (ins->src2 != 0xFF) {
            expr_exec_binary(ins->opcode, dt, rptrs[ins->dst],
                             expr->regs[ins->src1].type, rptrs[ins->src1],
                             expr->regs[ins->src2].type, rptrs[ins->src2], n);
        } else {
            expr_exec_unary(ins->opcode, dt, rptrs[ins->dst],
                            expr->regs[ins->src1].type, rptrs[ins->src1], n);
        }
    }

    return rptrs[expr->out_reg];
}

/* Context for parallel full-vector expression evaluation */
typedef struct {
    const ray_expr_t* expr;
    void*  out_data;
    int8_t out_type;
} expr_full_ctx_t;

static void expr_full_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    expr_full_ctx_t* c = (expr_full_ctx_t*)ctx;
    const ray_expr_t* expr = c->expr;
    uint8_t esz = ray_elem_size(c->out_type);

    /* Per-worker scratch buffers (heap-allocated via arena, morsel-sized) */
    ray_t* scratch_hdr = NULL;
    char* scratch_mem = (char*)scratch_alloc(&scratch_hdr,
                            (size_t)EXPR_MAX_REGS * EXPR_MORSEL * 8);
    if (!scratch_mem) return;
    void* scratch[EXPR_MAX_REGS];
    for (uint8_t r = 0; r < expr->n_regs; r++)
        scratch[r] = scratch_mem + (size_t)r * EXPR_MORSEL * 8;

    for (int64_t ms = start; ms < end; ms += EXPR_MORSEL) {
        int64_t me = (ms + EXPR_MORSEL < end) ? ms + EXPR_MORSEL : end;
        void* result = expr_eval_morsel(expr, scratch, ms, me);
        if (result)
            memcpy((char*)c->out_data + ms * esz, result, (size_t)(me - ms) * esz);
    }
    scratch_free(scratch_hdr);
}

/* Post-pass for the fused unary path: |INT64_MIN| and -INT64_MIN don't fit in
 * i64 (signed-overflow; k/q convention surfaces this as typed null).  The
 * element-wise loop uses unsigned wrap, so any overflow position lands as
 * INT64_MIN in data.  Convert each such position to typed-null: zero data[i]
 * (preserve "null position is 0" invariant) and set the null bit.  Caller
 * must invoke single-threaded — after pool dispatch joins. */
static void mark_i64_overflow_as_null(ray_t* result, int64_t off, int64_t len) {
    int64_t* d = (int64_t*)ray_data(result) + off;
    for (int64_t i = 0; i < len; i++) {
        if (RAY_UNLIKELY(d[i] == INT64_MIN)) {
            d[i] = 0;
            ray_vec_set_null(result, off + i, true);
        }
    }
}

/* The fused unary path may produce INT64_MIN via signed-overflow only for
 * OP_NEG and OP_ABS over an i64 source (output type i64).  Detect those
 * shapes from the last instruction in the compiled expression. */
static bool expr_last_op_overflows_i64(const ray_expr_t* expr) {
    if (expr->out_type != RAY_I64 || expr->n_ins == 0) return false;
    const expr_ins_t* last = &expr->ins[expr->n_ins - 1];
    if (last->opcode != OP_NEG && last->opcode != OP_ABS) return false;
    if (last->src2 != 0xFF) return false; /* unary only */
    if (expr->regs[last->src1].type != RAY_I64) return false;
    if (expr->regs[last->dst].type != RAY_I64) return false;
    return true;
}

/* Evaluate compiled expression over parted (segmented) columns.
 * Iterates segments as outer loop, rebinds data pointers per segment,
 * then dispatches the existing morsel evaluator per segment. Zero copy. */
static ray_t* expr_eval_full_parted(const ray_expr_t* expr, int64_t nrows) {
    ray_t* out = ray_vec_new(expr->out_type, nrows);
    if (!out || RAY_IS_ERR(out)) {
        return out;
    }
    out->len = nrows;

    /* Find first parted register to get segment structure */
    ray_t* ref_parted = NULL;
    for (uint8_t r = 0; r < expr->n_regs; r++) {
        if (expr->regs[r].is_parted) {
            ref_parted = expr->regs[r].parted_col;
            break;
        }
    }
    if (!ref_parted) { ray_release(out); return ray_error("nyi", NULL); }

    int64_t n_segs = ref_parted->len;
    uint8_t esz = ray_elem_size(expr->out_type);
    ray_pool_t* pool = ray_pool_get();
    int64_t global_off = 0;

    for (int64_t s = 0; s < n_segs; s++) {
        /* Determine segment length from any non-NULL parted register */
        int64_t seg_len = 0;
        for (uint8_t r = 0; r < expr->n_regs; r++) {
            if (expr->regs[r].is_parted) {
                ray_t** segs = (ray_t**)ray_data(expr->regs[r].parted_col);
                if (segs[s]) { seg_len = segs[s]->len; break; }
            }
        }
        if (seg_len <= 0) continue;

        /* Stack-copy expr, rebind parted registers to this segment's data */
        ray_expr_t seg_expr = *expr;
        bool seg_ok = true;
        for (uint8_t r = 0; r < seg_expr.n_regs; r++) {
            if (seg_expr.regs[r].is_parted) {
                ray_t** segs = (ray_t**)ray_data(seg_expr.regs[r].parted_col);
                if (!segs[s]) { seg_ok = false; break; }
                seg_expr.regs[r].data = ray_data(segs[s]);
            }
        }
        if (!seg_ok) {
            memset((char*)ray_data(out) + global_off * esz, 0,
                   (size_t)seg_len * esz);
            global_off += seg_len;
            continue;
        }

        expr_full_ctx_t ctx = {
            .expr = &seg_expr,
            .out_data = (char*)ray_data(out) + global_off * esz,
            .out_type = expr->out_type,
        };
        if (pool && seg_len >= RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, expr_full_fn, &ctx, seg_len);
        else
            expr_full_fn(&ctx, 0, 0, seg_len);

        global_off += seg_len;
    }
    if (expr_last_op_overflows_i64(expr))
        mark_i64_overflow_as_null(out, 0, nrows);
    return out;
}

/* Evaluate compiled expression into a full-length output vector.
 * Replaces exec_node() for expression subtrees — no intermediate vectors. */
ray_t* expr_eval_full(const ray_expr_t* expr, int64_t nrows) {
    if (expr->has_parted)
        return expr_eval_full_parted(expr, nrows);

    ray_t* out = ray_vec_new(expr->out_type, nrows);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = nrows;

    expr_full_ctx_t ctx = {
        .expr = expr, .out_data = ray_data(out), .out_type = expr->out_type,
    };

    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, expr_full_fn, &ctx, nrows);
    else
        expr_full_fn(&ctx, 0, 0, nrows);

    if (expr_last_op_overflows_i64(expr))
        mark_i64_overflow_as_null(out, 0, nrows);
    return out;
}

/* ============================================================================
 * Null bitmap propagation for element-wise ops
 * ============================================================================ */

/* Resolve the raw null bitmap pointer and bit offset for a vector.
 * Returns NULL if the vector has no null bits, or if the inline nullmap
 * cannot cover the requested range (prevents overread). */
static const uint8_t* nullmap_bits(ray_t* v, int64_t* bit_offset, int64_t len) {
    ray_t* target = v;
    int64_t off = 0;
    if (v->attrs & RAY_ATTR_SLICE) {
        target = v->slice_parent;
        off = v->slice_offset;
    }
    if (!(target->attrs & RAY_ATTR_HAS_NULLS)) return NULL;
    int64_t resolved_off = 0, len_bits = 0;
    const uint8_t* bits = ray_vec_nullmap_bytes(target, &resolved_off, &len_bits);
    if (!bits) return NULL;
    *bit_offset = off + resolved_off;
    /* Caller assumes inline buffer means 128-bit coverage; reject ranges
     * that would overrun it just like the original guard. */
    if (len_bits == 128 && off + len > 128) return NULL;
    return bits;
}

/* Writable null bitmap pointer for freshly allocated (non-slice) dst vector.
 * Returns NULL if inline nullmap cannot cover dst->len (prevents overflow). */
static uint8_t* nullmap_bits_mut(ray_t* dst) {
    if (dst->attrs & RAY_ATTR_NULLMAP_EXT)
        return (uint8_t*)ray_data(dst->ext_nullmap);
    if (dst->type == RAY_STR) return NULL;
    if (dst->len > 128) return NULL; /* inline can only cover 128 bits */
    return dst->nullmap;
}

/* OR-merge null bitmap from src into dst. Fast byte-level path when possible,
 * element-level fallback for misaligned slices or RAY_STR without ext nullmap. */
static void propagate_nulls(ray_t* src, ray_t* dst, int64_t len) {
    int64_t src_off = 0;
    const uint8_t* sbits = nullmap_bits(src, &src_off, len);
    if (!sbits) goto slow; /* no accessible bitmap — use element path */

    /* Ensure dst has ext nullmap for large vectors */
    if (len > 128 && !(dst->attrs & RAY_ATTR_NULLMAP_EXT))
        ray_vec_set_null(dst, len - 1, false); /* force ext alloc */
    uint8_t* dbits = nullmap_bits_mut(dst);
    if (!dbits) goto slow; /* ext alloc failed or RAY_STR */

    /* Bulk OR — both bitmaps are byte-accessible and src is byte-aligned */
    if ((src_off % 8) == 0) {
        int64_t byte_start = src_off / 8;
        int64_t nbytes = (len + 7) / 8;
        for (int64_t b = 0; b < nbytes; b++)
            dbits[b] |= sbits[byte_start + b];
        dst->attrs |= RAY_ATTR_HAS_NULLS;
        return;
    }

slow:
    for (int64_t i = 0; i < len; i++) {
        if (ray_vec_is_null(src, i))
            ray_vec_set_null(dst, i, true);
    }
}

/* Returns true for arithmetic ops that should propagate nulls.
 * Comparisons (EQ..GE) and logical ops (AND/OR) produce false for null inputs. */
static bool op_propagates_null(uint16_t opc) {
    return opc < OP_EQ || opc > OP_OR;
}

/* Check if a scalar operand (atom or length-1 vector) is null.
 * Handles slices correctly via ray_vec_is_null delegation. */
static bool scalar_is_null(ray_t* x) {
    if (ray_is_atom(x)) return RAY_ATOM_IS_NULL(x);
    /* Length-1 vector — use ray_vec_is_null which handles slices */
    return ray_vec_is_null(x, 0);
}

/* Check if a vector might contain nulls (accounts for slices). */
static bool vec_may_have_nulls(ray_t* v) {
    return (v->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) != 0;
}

/* Resolve data pointer for a vector, accounting for slices.
 * For slices, returns the parent's data and adjusts *offset. */
static void* resolve_vec_data(ray_t* v, int64_t* offset) {
    if (v->attrs & RAY_ATTR_SLICE) {
        *offset += v->slice_offset;
        return ray_data(v->slice_parent);
    }
    return ray_data(v);
}

/* For comparisons: force result to false for any element where either input is null. */
/* Fix comparison results at null positions using null-as-minimum semantics.
 * null == null → true, null < x → true, x > null → true, etc. */
static void fix_null_comparisons(ray_t* lhs, ray_t* rhs, ray_t* result,
                                  bool l_scalar, bool r_scalar, int64_t len,
                                  uint16_t opcode) {
    uint8_t* dst = (uint8_t*)ray_data(result);
    bool ln_s = l_scalar && scalar_is_null(lhs);
    bool rn_s = r_scalar && scalar_is_null(rhs);
    bool l_has = !l_scalar && vec_may_have_nulls(lhs);
    bool r_has = !r_scalar && vec_may_have_nulls(rhs);
    if (!ln_s && !rn_s && !l_has && !r_has) return;

    for (int64_t i = 0; i < len; i++) {
        bool ln = ln_s || (l_has && ray_vec_is_null(lhs, i));
        bool rn = rn_s || (r_has && ray_vec_is_null(rhs, i));
        if (!ln && !rn) continue;
        /* Both null */
        if (ln && rn) {
            dst[i] = (opcode == OP_EQ || opcode == OP_LE || opcode == OP_GE) ? 1 : 0;
            continue;
        }
        /* Left null only (null = minimum) */
        if (ln) {
            dst[i] = (opcode == OP_LT || opcode == OP_LE || opcode == OP_NE) ? 1 : 0;
            continue;
        }
        /* Right null only */
        dst[i] = (opcode == OP_GT || opcode == OP_GE || opcode == OP_NE) ? 1 : 0;
    }
}

/* Set all elements in result as null (scalar null broadcast). */
static void set_all_null(ray_t* result, int64_t len) {
    if (len > 128 && !(result->attrs & RAY_ATTR_NULLMAP_EXT))
        ray_vec_set_null(result, len - 1, false); /* force ext alloc */
    uint8_t* dbits = nullmap_bits_mut(result);
    if (dbits) {
        memset(dbits, 0xFF, (size_t)((len + 7) / 8));
        result->attrs |= RAY_ATTR_HAS_NULLS;
    } else {
        for (int64_t i = 0; i < len; i++) ray_vec_set_null(result, i, true);
    }
}

/* Propagate null bitmaps for binary ops: null in either operand → null in result. */
static void propagate_nulls_binary(ray_t* lhs, ray_t* rhs, ray_t* result,
                                   bool l_scalar, bool r_scalar, int64_t len) {
    if (l_scalar && scalar_is_null(lhs)) {
        set_all_null(result, len);
    } else if (r_scalar && scalar_is_null(rhs)) {
        set_all_null(result, len);
    } else {
        if (!l_scalar && vec_may_have_nulls(lhs)) propagate_nulls(lhs, result, len);
        if (!r_scalar && vec_may_have_nulls(rhs)) propagate_nulls(rhs, result, len);
    }
}

/* ============================================================================
 * Element-wise execution
 * ============================================================================ */

ray_t* exec_elementwise_unary(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g;
    if (!input || RAY_IS_ERR(input)) return input;
    int64_t len = input->len;
    int8_t in_type = input->type;
    int8_t out_type = op->out_type;

    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = len;

    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t out_off = 0;

    while (ray_morsel_next(&m)) {
        int64_t n = m.morsel_len;
        void* dst = (char*)ray_data(result) + out_off * ray_elem_size(out_type);

        if (in_type == RAY_F64 || in_type == RAY_I64) {
            for (int64_t i = 0; i < n; i++) {
                if (in_type == RAY_F64) {
                    double v = ((double*)m.morsel_ptr)[i];
                    double r;
                    switch (op->opcode) {
                        case OP_NEG:   r = -v; break;
                        case OP_ABS:   r = fabs(v); break;
                        case OP_SQRT:  r = sqrt(v); break;
                        case OP_LOG:   r = log(v); break;
                        case OP_EXP:   r = exp(v); break;
                        case OP_CEIL:  r = ceil(v); break;
                        case OP_FLOOR: r = floor(v); break;
                        case OP_ROUND: r = round(v); break;
                        default:       r = v; break;
                    }
                    if (out_type == RAY_F64) ((double*)dst)[i] = r;
                    else if (out_type == RAY_I64) ((int64_t*)dst)[i] = (int64_t)r;
                } else {
                    int64_t v = ((int64_t*)m.morsel_ptr)[i];
                    if (out_type == RAY_I64) {
                        int64_t r;
                        switch (op->opcode) {
                            /* Unsigned negation avoids UB on INT64_MIN */
                            case OP_NEG: r = (int64_t)(-(uint64_t)v); break;
                            case OP_ABS: r = v < 0 ? (int64_t)(-(uint64_t)v) : v; break;
                            default:     r = v; break;
                        }
                        ((int64_t*)dst)[i] = r;
                    } else if (out_type == RAY_F64) {
                        double r;
                        switch (op->opcode) {
                            case OP_NEG:   r = -(double)v; break;
                            case OP_SQRT:  r = sqrt((double)v); break;
                            case OP_LOG:   r = log((double)v); break;
                            case OP_EXP:   r = exp((double)v); break;
                            default:       r = (double)v; break;
                        }
                        ((double*)dst)[i] = r;
                    } else if (out_type == RAY_BOOL) {
                        /* ISNULL: for non-null vecs, always false */
                        ((uint8_t*)dst)[i] = 0;
                    }
                }
            }
        } else if (in_type == RAY_BOOL && op->opcode == OP_NOT) {
            for (int64_t i = 0; i < n; i++) {
                ((uint8_t*)dst)[i] = !((uint8_t*)m.morsel_ptr)[i];
            }
        } else if (op->opcode == OP_CAST) {
            /* CAST from narrow integer types (I32/I16/U8/BOOL) to I64/F64 */
            for (int64_t i = 0; i < n; i++) {
                int64_t v = 0;
                if (in_type == RAY_I32 || in_type == RAY_DATE || in_type == RAY_TIME)
                    v = (int64_t)((int32_t*)m.morsel_ptr)[i];
                else if (in_type == RAY_I16)
                    v = (int64_t)((int16_t*)m.morsel_ptr)[i];
                else if (in_type == RAY_U8 || in_type == RAY_BOOL)
                    v = (int64_t)((uint8_t*)m.morsel_ptr)[i];
                if (out_type == RAY_I64)       ((int64_t*)dst)[i] = v;
                else if (out_type == RAY_F64)  ((double*)dst)[i] = (double)v;
            }
        }

        out_off += n;
    }

    /* Propagate null bitmap from input to result.
     * ISNULL is special: set output to 1 for null elements. */
    if (vec_may_have_nulls(input)) {
        if (op->opcode == OP_ISNULL) {
            for (int64_t i = 0; i < len; i++) {
                if (ray_vec_is_null(input, i))
                    ((uint8_t*)ray_data(result))[i] = 1;
            }
        } else {
            propagate_nulls(input, result, len);
        }
    }

    /* OP_NEG/OP_ABS over i64: |INT64_MIN| and -INT64_MIN don't fit — surface
     * as typed null (k/q convention).  Loop above used unsigned wrap, so
     * overflow positions land as INT64_MIN in data; convert them to null. */
    if (out_type == RAY_I64 && in_type == RAY_I64 &&
        (op->opcode == OP_NEG || op->opcode == OP_ABS))
        mark_i64_overflow_as_null(result, 0, len);

    return result;
}

/* Inner loop for binary element-wise string comparison over [start, end) */
static void binary_range_str(ray_op_t* op, ray_t* lhs, ray_t* rhs, ray_t* result,
                             bool l_scalar, bool r_scalar,
                             int64_t start, int64_t end) {
    uint8_t* dst = (uint8_t*)ray_data(result) + start;
    int64_t n = end - start;
    uint16_t opc = op->opcode;

    const ray_str_t* l_elems = NULL;
    const ray_str_t* r_elems = NULL;
    const char* l_pool = NULL;
    const char* r_pool = NULL;
    if (!l_scalar) { str_resolve(lhs, &l_elems, &l_pool); l_elems += start; }
    if (!r_scalar) { str_resolve(rhs, &r_elems, &r_pool); r_elems += start; }

    /* For scalar side, build a single ray_str_t */
    ray_str_t l_scalar_elem = {0}, r_scalar_elem = {0};
    const char* l_scalar_pool = NULL;
    const char* r_scalar_pool = NULL;
    if (l_scalar) {
        atom_to_str_t(lhs, &l_scalar_elem, &l_scalar_pool);
        l_elems = &l_scalar_elem;
    }
    if (r_scalar) {
        atom_to_str_t(rhs, &r_scalar_elem, &r_scalar_pool);
        r_elems = &r_scalar_elem;
    }

    for (int64_t i = 0; i < n; i++) {
        const ray_str_t* a = l_scalar ? l_elems : &l_elems[i];
        const ray_str_t* b = r_scalar ? r_elems : &r_elems[i];
        const char* pa = l_scalar ? l_scalar_pool : l_pool;
        const char* pb = r_scalar ? r_scalar_pool : r_pool;

        switch (opc) {
            case OP_EQ: dst[i] = ray_str_t_eq(a, pa, b, pb); break;
            case OP_NE: dst[i] = !ray_str_t_eq(a, pa, b, pb); break;
            case OP_LT: dst[i] = ray_str_t_cmp(a, pa, b, pb) < 0; break;
            case OP_LE: dst[i] = ray_str_t_cmp(a, pa, b, pb) <= 0; break;
            case OP_GT: dst[i] = ray_str_t_cmp(a, pa, b, pb) > 0; break;
            case OP_GE: dst[i] = ray_str_t_cmp(a, pa, b, pb) >= 0; break;
            default: dst[i] = 0; break;
        }
    }
}

/* Context for parallel RAY_STR binary dispatch */
typedef struct {
    ray_op_t* op;
    ray_t*    lhs;
    ray_t*    rhs;
    ray_t*    result;
    bool     l_scalar;
    bool     r_scalar;
} par_binary_str_ctx_t;

static void par_binary_str_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    par_binary_str_ctx_t* c = (par_binary_str_ctx_t*)ctx;
    binary_range_str(c->op, c->lhs, c->rhs, c->result,
                     c->l_scalar, c->r_scalar, start, end);
}

/* Inner loop for binary element-wise over a range [start, end) */
static void binary_range(ray_op_t* op, int8_t out_type,
                         ray_t* lhs, ray_t* rhs, ray_t* result,
                         bool l_scalar, bool r_scalar,
                         double l_f64, double r_f64,
                         int64_t l_i64, int64_t r_i64,
                         int64_t start, int64_t end) {
    uint8_t out_esz = ray_elem_size(out_type);
    void* dst = (char*)ray_data(result) + start * out_esz;
    int64_t n = end - start;

    /* Pointers into source data at offset start */
    double* lp_f64 = NULL; int64_t* lp_i64 = NULL; uint8_t* lp_bool = NULL;
    double* rp_f64 = NULL; int64_t* rp_i64 = NULL; uint8_t* rp_bool = NULL;

    int32_t* lp_i32 = NULL; uint32_t* lp_u32 = NULL; int16_t* lp_i16 = NULL;
    int32_t* rp_i32 = NULL; uint32_t* rp_u32 = NULL; int16_t* rp_i16 = NULL;

    /* VLA bound of zero is UB; guarantee >=1 slot.  The fill loops below
     * are bounded by n so extra slots are harmless. */
    int64_t _sym_buf_n = n ? n : 1;
    int64_t lsym_buf[_sym_buf_n], rsym_buf[_sym_buf_n]; /* stack VLA for narrow RAY_SYM (n<=1024) */
    if (!l_scalar) {
        int64_t l_off = start;
        void* l_data = resolve_vec_data(lhs, &l_off);
        void* lbase = (char*)l_data + l_off * ray_sym_elem_size(lhs->type, lhs->attrs);
        if (lhs->type == RAY_F64) lp_f64 = (double*)lbase;
        else if (lhs->type == RAY_I64 || lhs->type == RAY_TIMESTAMP) lp_i64 = (int64_t*)lbase;
        else if (RAY_IS_SYM(lhs->type)) {
            uint8_t w = lhs->attrs & RAY_SYM_W_MASK;
            if (w == RAY_SYM_W64) lp_i64 = (int64_t*)lbase;
            else if (w == RAY_SYM_W32) lp_u32 = (uint32_t*)lbase;
            else { for (int64_t j = 0; j < n; j++) lsym_buf[j] = ray_read_sym(l_data, l_off+j, lhs->type, lhs->attrs); lp_i64 = lsym_buf; }
        }
        else if (lhs->type == RAY_I32 || lhs->type == RAY_DATE || lhs->type == RAY_TIME) lp_i32 = (int32_t*)lbase;
        else if (lhs->type == RAY_I16) lp_i16 = (int16_t*)lbase;
        else if (lhs->type == RAY_BOOL || lhs->type == RAY_U8) lp_bool = (uint8_t*)lbase;
    }
    if (!r_scalar) {
        int64_t r_off = start;
        void* r_data = resolve_vec_data(rhs, &r_off);
        void* rbase = (char*)r_data + r_off * ray_sym_elem_size(rhs->type, rhs->attrs);
        if (rhs->type == RAY_F64) rp_f64 = (double*)rbase;
        else if (rhs->type == RAY_I64 || rhs->type == RAY_TIMESTAMP) rp_i64 = (int64_t*)rbase;
        else if (RAY_IS_SYM(rhs->type)) {
            uint8_t w = rhs->attrs & RAY_SYM_W_MASK;
            if (w == RAY_SYM_W64) rp_i64 = (int64_t*)rbase;
            else if (w == RAY_SYM_W32) rp_u32 = (uint32_t*)rbase;
            else { for (int64_t j = 0; j < n; j++) rsym_buf[j] = ray_read_sym(r_data, r_off+j, rhs->type, rhs->attrs); rp_i64 = rsym_buf; }
        }
        else if (rhs->type == RAY_I32 || rhs->type == RAY_DATE || rhs->type == RAY_TIME) rp_i32 = (int32_t*)rbase;
        else if (rhs->type == RAY_I16) rp_i16 = (int16_t*)rbase;
        else if (rhs->type == RAY_BOOL || rhs->type == RAY_U8) rp_bool = (uint8_t*)rbase;
    }

    for (int64_t i = 0; i < n; i++) {
        double lv, rv;
        if (lp_f64)       lv = lp_f64[i];
        else if (lp_i64)  lv = (double)lp_i64[i];
        else if (lp_i32)  lv = (double)lp_i32[i];
        else if (lp_u32)  lv = (double)lp_u32[i];
        else if (lp_i16)  lv = (double)lp_i16[i];
        else if (lp_bool) lv = (double)lp_bool[i];
        else if (l_scalar && (lhs->type == -RAY_F64 || lhs->type == RAY_F64)) lv = l_f64;
        else              lv = (double)l_i64;

        if (rp_f64)       rv = rp_f64[i];
        else if (rp_i64)  rv = (double)rp_i64[i];
        else if (rp_i32)  rv = (double)rp_i32[i];
        else if (rp_u32)  rv = (double)rp_u32[i];
        else if (rp_i16)  rv = (double)rp_i16[i];
        else if (rp_bool) rv = (double)rp_bool[i];
        else if (r_scalar && (rhs->type == -RAY_F64 || rhs->type == RAY_F64)) rv = r_f64;
        else              rv = (double)r_i64;

        if (out_type == RAY_F64) {
            double r;
            switch (op->opcode) {
                case OP_ADD: r = lv + rv; break;
                case OP_SUB: r = lv - rv; break;
                case OP_MUL: r = lv * rv; break;
                case OP_DIV: r = rv != 0.0 ? lv / rv : NAN; break;
                case OP_MOD: { if (rv != 0.0) { r = fmod(lv, rv); if (r && ((r > 0) != (rv > 0))) r += rv; } else { r = NAN; } } break;
                case OP_MIN2: r = lv < rv ? lv : rv; break;
                case OP_MAX2: r = lv > rv ? lv : rv; break;
                default: r = 0.0; break;
            }
            ((double*)dst)[i] = r;
        } else if (out_type == RAY_I64 || out_type == RAY_TIMESTAMP) {
            int64_t li = (int64_t)lv, ri = (int64_t)rv;
            int64_t r;
            switch (op->opcode) {
                case OP_ADD: r = (int64_t)((uint64_t)li + (uint64_t)ri); break;
                case OP_SUB: r = (int64_t)((uint64_t)li - (uint64_t)ri); break;
                case OP_MUL: r = (int64_t)((uint64_t)li * (uint64_t)ri); break;
                case OP_DIV:
                    if (ri==0 || (ri==-1 && li==((int64_t)1<<63))) { r = 0; }
                    else { r = li/ri; if ((li^ri)<0 && r*ri!=li) r--; }
                    break;
                case OP_MOD:
                    if (ri==0 || (ri==-1 && li==((int64_t)1<<63))) { r = 0; }
                    else { r = li%ri; if (r && (r^ri)<0) r+=ri; }
                    break;
                case OP_MIN2: r = li < ri ? li : ri; break;
                case OP_MAX2: r = li > ri ? li : ri; break;
                default: r = 0; break;
            }
            ((int64_t*)dst)[i] = r;
        } else if (out_type == RAY_I32 || out_type == RAY_DATE || out_type == RAY_TIME) {
            int32_t li = (int32_t)lv, ri = (int32_t)rv;
            int32_t r;
            switch (op->opcode) {
                case OP_ADD: r = (int32_t)((uint32_t)li + (uint32_t)ri); break;
                case OP_SUB: r = (int32_t)((uint32_t)li - (uint32_t)ri); break;
                case OP_MUL: r = (int32_t)((uint32_t)li * (uint32_t)ri); break;
                case OP_DIV:
                    if (ri==0 || (ri==-1 && li==((int32_t)1<<31))) { r = 0; }
                    else { r = li/ri; if ((li^ri)<0 && r*ri!=li) r--; }
                    break;
                case OP_MOD:
                    if (ri==0 || (ri==-1 && li==((int32_t)1<<31))) { r = 0; }
                    else { r = li%ri; if (r && (r^ri)<0) r+=ri; }
                    break;
                case OP_MIN2: r = li < ri ? li : ri; break;
                case OP_MAX2: r = li > ri ? li : ri; break;
                default: r = 0; break;
            }
            ((int32_t*)dst)[i] = r;
        } else if (out_type == RAY_I16) {
            int16_t li = (int16_t)lv, ri = (int16_t)rv;
            int16_t r;
            switch (op->opcode) {
                case OP_ADD: r = (int16_t)((uint16_t)li + (uint16_t)ri); break;
                case OP_SUB: r = (int16_t)((uint16_t)li - (uint16_t)ri); break;
                case OP_MUL: r = (int16_t)((uint16_t)li * (uint16_t)ri); break;
                case OP_DIV: r = ri ? li / ri : 0; break;
                case OP_MOD: r = ri ? li % ri : 0; break;
                case OP_MIN2: r = li < ri ? li : ri; break;
                case OP_MAX2: r = li > ri ? li : ri; break;
                default: r = 0; break;
            }
            ((int16_t*)dst)[i] = r;
        } else if (out_type == RAY_U8) {
            uint8_t li = (uint8_t)lv, ri = (uint8_t)rv;
            uint8_t r;
            switch (op->opcode) {
                case OP_ADD: r = li + ri; break;
                case OP_SUB: r = li - ri; break;
                case OP_MUL: r = li * ri; break;
                case OP_DIV: r = ri ? li / ri : 0; break;
                case OP_MOD: r = ri ? li % ri : 0; break;
                case OP_MIN2: r = li < ri ? li : ri; break;
                case OP_MAX2: r = li > ri ? li : ri; break;
                default: r = 0; break;
            }
            ((uint8_t*)dst)[i] = r;
        } else if (out_type == RAY_BOOL) {
            /* Read raw I64 values directly for null-aware comparison
             * when both operands are I64/I32-family (not F64). */
            int src_is_i64 = (lp_i64 || lp_i32 || lp_u32 || lp_i16 ||
                              (l_scalar && lhs->type != -RAY_F64 && lhs->type != RAY_F64)) &&
                             (rp_i64 || rp_i32 || rp_u32 || rp_i16 ||
                              (r_scalar && rhs->type != -RAY_F64 && rhs->type != RAY_F64));
            int64_t li64 = (int64_t)lv, ri64 = (int64_t)rv;
            uint8_t r;
            if (src_is_i64) {
                /* No sentinel nulls — fix_null_comparisons handles null positions */
                switch (op->opcode) {
                    case OP_EQ: r = li64==ri64; break;
                    case OP_NE: r = li64!=ri64; break;
                    case OP_LT: r = li64<ri64; break;
                    case OP_LE: r = li64<=ri64; break;
                    case OP_GT: r = li64>ri64; break;
                    case OP_GE: r = li64>=ri64; break;
                    case OP_AND: r = (uint8_t)lv && (uint8_t)rv; break;
                    case OP_OR:  r = (uint8_t)lv || (uint8_t)rv; break;
                    default: r = 0; break;
                }
            } else {
                /* Null-aware F64 comparisons: NaN is null sentinel */
                int ln = (lv != lv), rn = (rv != rv); /* NaN check */
                switch (op->opcode) {
                    case OP_EQ:  r = (ln&&rn) ? 1 : (ln||rn) ? 0 : lv==rv; break;
                    case OP_NE:  r = (ln&&rn) ? 0 : (ln||rn) ? 1 : lv!=rv; break;
                    case OP_LT:  r = (ln&&rn) ? 0 : ln ? 1 : rn ? 0 : lv<rv; break;
                    case OP_LE:  r = (ln&&rn) ? 1 : ln ? 1 : rn ? 0 : lv<=rv; break;
                    case OP_GT:  r = (ln&&rn) ? 0 : rn ? 1 : ln ? 0 : lv>rv; break;
                    case OP_GE:  r = (ln&&rn) ? 1 : rn ? 1 : ln ? 0 : lv>=rv; break;
                    case OP_AND: r = (uint8_t)lv && (uint8_t)rv; break;
                    case OP_OR:  r = (uint8_t)lv || (uint8_t)rv; break;
                    default: r = 0; break;
                }
            }
            ((uint8_t*)dst)[i] = r;
        }
    }
}

/* Context for parallel binary dispatch */
typedef struct {
    ray_op_t* op;
    int8_t   out_type;
    ray_t*    lhs;
    ray_t*    rhs;
    ray_t*    result;
    bool     l_scalar;
    bool     r_scalar;
    double   l_f64, r_f64;
    int64_t  l_i64, r_i64;
} par_binary_ctx_t;

static void par_binary_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    par_binary_ctx_t* c = (par_binary_ctx_t*)ctx;
    binary_range(c->op, c->out_type, c->lhs, c->rhs, c->result,
                 c->l_scalar, c->r_scalar,
                 c->l_f64, c->r_f64, c->l_i64, c->r_i64,
                 start, end);
}

ray_t* exec_elementwise_binary(ray_graph_t* g, ray_op_t* op, ray_t* lhs, ray_t* rhs) {
    (void)g;
    if (!lhs || RAY_IS_ERR(lhs)) return lhs;
    if (!rhs || RAY_IS_ERR(rhs)) return rhs;

    bool l_scalar = ray_is_atom(lhs) || (lhs->type > 0 && lhs->len == 1);
    bool r_scalar = ray_is_atom(rhs) || (rhs->type > 0 && rhs->len == 1);

    int64_t len = 1;
    if (!l_scalar && !r_scalar) {
        if (lhs->len != rhs->len) return ray_error("length", NULL);
        len = lhs->len;
    } else if (l_scalar && !r_scalar) {
        len = rhs->len;
    } else if (!l_scalar && r_scalar) {
        len = lhs->len;
    }

    int8_t out_type = op->out_type;
    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = len;

    /* RAY_STR comparison: use ray_str_t_eq / ray_str_t_cmp directly.
       Handles RAY_STR column vs RAY_STR column, or -RAY_STR scalar vs RAY_STR column. */
    {
        bool l_is_str = (!l_scalar && lhs->type == RAY_STR);
        bool r_is_str = (!r_scalar && rhs->type == RAY_STR);
        bool l_atom_str = (l_scalar && (lhs->type == -RAY_STR
                          || lhs->type == RAY_STR
                          || (RAY_IS_SYM(lhs->type) && ray_is_atom(lhs))));
        bool r_atom_str = (r_scalar && (rhs->type == -RAY_STR
                          || rhs->type == RAY_STR
                          || (RAY_IS_SYM(rhs->type) && ray_is_atom(rhs))));

        if (l_is_str || r_is_str || (l_atom_str && r_atom_str)) {
            /* RAY_STR only supports comparison ops — reject arithmetic */
            uint16_t opc = op->opcode;
            if (opc < OP_EQ || opc > OP_GE) { ray_release(result); return ray_error("type", NULL); }
            /* At least one side is a RAY_STR column — use string comparison path.
               The scalar side (if any) must be -RAY_STR or RAY_SYM atom.
               The non-scalar side must be RAY_STR. */
            if (l_scalar && !l_atom_str) { ray_release(result); return ray_error("type", NULL); }
            if (r_scalar && !r_atom_str) { ray_release(result); return ray_error("type", NULL); }
            if (!l_scalar && !l_is_str) { ray_release(result); return ray_error("type", NULL); }
            if (!r_scalar && !r_is_str) { ray_release(result); return ray_error("type", NULL); }

            ray_pool_t* pool = ray_pool_get();
            if (pool && len >= RAY_PARALLEL_THRESHOLD) {
                par_binary_str_ctx_t ctx = {
                    .op = op, .lhs = lhs, .rhs = rhs, .result = result,
                    .l_scalar = l_scalar, .r_scalar = r_scalar,
                };
                ray_pool_dispatch(pool, par_binary_str_fn, &ctx, len);
                fix_null_comparisons(lhs, rhs, result, l_scalar, r_scalar, len, op->opcode);
                return result;
            }
            binary_range_str(op, lhs, rhs, result, l_scalar, r_scalar, 0, len);
            fix_null_comparisons(lhs, rhs, result, l_scalar, r_scalar, len, op->opcode);
            return result;
        }
    }

    /* SYM vs STR comparison: resolve string constant to intern ID so we
       can compare numerically against SYM intern indices.
       ray_sym_find returns -1 if string not in table → no match. */
    bool str_resolved = false;
    int64_t resolved_sym_id = 0;
    if (r_scalar && rhs->type == -RAY_STR &&
        RAY_IS_SYM(lhs->type)) {
        const char* s = ray_str_ptr(rhs);
        size_t slen = ray_str_len(rhs);
        resolved_sym_id = ray_sym_find(s, slen);
        str_resolved = true;
    } else if (l_scalar && lhs->type == -RAY_STR &&
               RAY_IS_SYM(rhs->type)) {
        const char* s = ray_str_ptr(lhs);
        size_t slen = ray_str_len(lhs);
        resolved_sym_id = ray_sym_find(s, slen);
        str_resolved = true;
    }

    double l_f64_val = 0, r_f64_val = 0;
    int64_t l_i64_val = 0, r_i64_val = 0;
    if (l_scalar) {
        if (str_resolved && lhs->type == -RAY_STR)
            l_i64_val = resolved_sym_id;
        else if (ray_is_atom(lhs)) {
            if (lhs->type == -RAY_F64) l_f64_val = lhs->f64;
            else if (lhs->type == -RAY_I32 || lhs->type == -RAY_DATE || lhs->type == -RAY_TIME)
                l_i64_val = (int64_t)lhs->i32;
            else if (lhs->type == -RAY_I16) l_i64_val = (int64_t)lhs->i16;
            else if (lhs->type == -RAY_U8 || lhs->type == -RAY_BOOL) l_i64_val = (int64_t)lhs->u8;
            else l_i64_val = lhs->i64;
        } else {
            int8_t t = lhs->type;
            int64_t elem = 0;
            void* data = resolve_vec_data(lhs, &elem);
            if (t == RAY_F64) l_f64_val = ((double*)data)[elem];
            else l_i64_val = read_col_i64(data, elem, t, lhs->attrs);
        }
    }
    if (r_scalar) {
        if (str_resolved && rhs->type == -RAY_STR)
            r_i64_val = resolved_sym_id;
        else if (ray_is_atom(rhs)) {
            if (rhs->type == -RAY_F64) r_f64_val = rhs->f64;
            else if (rhs->type == -RAY_I32 || rhs->type == -RAY_DATE || rhs->type == -RAY_TIME)
                r_i64_val = (int64_t)rhs->i32;
            else if (rhs->type == -RAY_I16) r_i64_val = (int64_t)rhs->i16;
            else if (rhs->type == -RAY_U8 || rhs->type == -RAY_BOOL) r_i64_val = (int64_t)rhs->u8;
            else r_i64_val = rhs->i64;
        } else {
            int8_t t = rhs->type;
            int64_t elem = 0;
            void* data = resolve_vec_data(rhs, &elem);
            if (t == RAY_F64) r_f64_val = ((double*)data)[elem];
            else r_i64_val = read_col_i64(data, elem, t, rhs->attrs);
        }
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD) {
        par_binary_ctx_t ctx = {
            .op = op, .out_type = out_type,
            .lhs = lhs, .rhs = rhs, .result = result,
            .l_scalar = l_scalar, .r_scalar = r_scalar,
            .l_f64 = l_f64_val, .r_f64 = r_f64_val,
            .l_i64 = l_i64_val, .r_i64 = r_i64_val,
        };
        ray_pool_dispatch(pool, par_binary_fn, &ctx, len);
    } else {
        binary_range(op, out_type, lhs, rhs, result,
                     l_scalar, r_scalar,
                     l_f64_val, r_f64_val, l_i64_val, r_i64_val,
                     0, len);
    }

    /* Null propagation from inputs */
    if (op_propagates_null(op->opcode))
        propagate_nulls_binary(lhs, rhs, result, l_scalar, r_scalar, len);
    else
        fix_null_comparisons(lhs, rhs, result, l_scalar, r_scalar, len, op->opcode);

    /* Div/mod: mark zero-divisor positions as null.
     * The morsel loop writes 0 for b==0 but can't set bitmap nulls. */
    uint16_t opc = op->opcode;
    if (opc == OP_DIV || opc == OP_MOD) {
        if (!r_scalar) {
            int8_t rt = rhs->type;
            if (rt == RAY_I64 || rt == RAY_TIMESTAMP) {
                const int64_t* b = (const int64_t*)ray_data(rhs);
                for (int64_t i = 0; i < len; i++)
                    if (b[i] == 0) ray_vec_set_null(result, i, true);
            } else if (rt == RAY_I32 || rt == RAY_DATE || rt == RAY_TIME) {
                const int32_t* b = (const int32_t*)ray_data(rhs);
                for (int64_t i = 0; i < len; i++)
                    if (b[i] == 0) ray_vec_set_null(result, i, true);
            }
            /* F64 div-by-zero produces NaN which is handled by propagate_nulls */
        } else {
            /* Scalar divisor: check for zero using the correct type */
            bool is_zero = false;
            if (rhs->type == -RAY_F64 || rhs->type == RAY_F64)
                is_zero = (r_f64_val == 0.0);
            else
                is_zero = (r_i64_val == 0);
            if (is_zero) {
                for (int64_t i = 0; i < len; i++)
                    ray_vec_set_null(result, i, true);
            }
        }
    }

    return result;
}

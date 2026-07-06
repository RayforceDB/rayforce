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
#include "lang/internal.h"  /* sym_domain_rep (sym-domain Phase 2) */
#include "lang/format.h"    /* ray_type_name */
#include "ops/rowsel.h"
#include "ops/idxop.h"      /* RAY_IDX_CHUNK_ZONE per-morsel zone-skip */
#include <stdio.h>
#include <stdlib.h>

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

    if ((op->opcode == OP_NEG || op->opcode == OP_ABS) && op->arity == 1 && op_child(g, op, 0)) {
        double af = 0.0;
        int64_t ai = 0;
        bool a_is_f64 = false;
        if (!eval_const_numeric_expr(g, op_child(g, op, 0), &af, &ai, &a_is_f64)) return false;
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

    if (op->arity != 2 || !op_child(g, op, 0) || !op_child(g, op, 1)) return false;
    if ((op->opcode < OP_ADD || op->opcode > OP_MAX2) && op->opcode != OP_IDIV) return false;

    double lf = 0.0, rf = 0.0;
    int64_t li = 0, ri = 0;
    bool l_is_f64 = false, r_is_f64 = false;
    if (!eval_const_numeric_expr(g, op_child(g, op, 0), &lf, &li, &l_is_f64)) return false;
    if (!eval_const_numeric_expr(g, op_child(g, op, 1), &rf, &ri, &r_is_f64)) return false;

    if (op->out_type == RAY_F64 || l_is_f64 || r_is_f64 || op->opcode == OP_DIV) {
        double lv = l_is_f64 ? lf : (double)li;
        double rv = r_is_f64 ? rf : (double)ri;
        double r = 0.0;
        switch (op->opcode) {
            case OP_ADD: r = lv + rv; break;
            case OP_SUB: r = lv - rv; break;
            case OP_MUL: r = lv * rv; break;
            case OP_DIV: r = rv != 0.0 ? lv / rv : NAN; break;
            case OP_IDIV: r = rv != 0.0 ? floor(lv / rv) : NAN; break;
            case OP_MOD: { if (rv != 0.0) { r = fmod(lv, rv); if (r && ((r > 0) != (rv > 0))) r += rv; } else { r = NAN; } } break;
            case OP_MIN2: r = lv < rv ? lv : rv; break;
            case OP_MAX2: r = lv > rv ? lv : rv; break;
            default: return false;
        }
        /* Single-null float model: a folded constant that is non-finite
         * (div/mod by zero → NaN, overflow → ±Inf) canonicalizes to NULL_F64. */
        r = ray_f64_fin(r);
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
        case OP_IDIV:
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

    if (op->opcode == OP_NEG && op->arity == 1 && op_child(g, op, 0)) {
        linear_expr_i64_t inner;
        if (!parse_linear_i64_expr(g, op_child(g, op, 0), &inner)) return false;
        linear_expr_scale(&inner, -1);
        *out = inner;
        return true;
    }

    if ((op->opcode == OP_ADD || op->opcode == OP_SUB) &&
        op->arity == 2 && op_child(g, op, 0) && op_child(g, op, 1)) {
        linear_expr_i64_t lhs;
        linear_expr_i64_t rhs;
        if (!parse_linear_i64_expr(g, op_child(g, op, 0), &lhs)) return false;
        if (!parse_linear_i64_expr(g, op_child(g, op, 1), &rhs)) return false;
        *out = lhs;
        return linear_expr_add_scaled(out, &rhs, op->opcode == OP_ADD ? 1 : -1);
    }

    if (op->opcode == OP_MUL && op->arity == 2 && op_child(g, op, 0) && op_child(g, op, 1)) {
        int64_t k = 0;
        linear_expr_i64_t side;
        if (const_expr_to_i64(g, op_child(g, op, 0), &k) &&
            parse_linear_i64_expr(g, op_child(g, op, 1), &side)) {
            linear_expr_scale(&side, k);
            *out = side;
            return true;
        }
        if (const_expr_to_i64(g, op_child(g, op, 1), &k) &&
            parse_linear_i64_expr(g, op_child(g, op, 0), &side)) {
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
        /* scalar_sum_linear_i64_fn reads slots raw via scalar_i64_at;
         * any nullable term would poison the sum with NULL_I{16,32,64}
         * sentinels.  Refuse the fast plan and let the caller fall back
         * to the generic masked path. */
        if (col->attrs & RAY_ATTR_HAS_NULLS) return false;
        out_plan->term_ptrs[i] = ray_data(col);
        out_plan->term_types[i] = col->type;
        out_plan->coeff_i64[i] = lin.coeff_i64[i];
    }
    out_plan->enabled = true;
    return true;
}

/* Detect SUM/AVG inputs of form (scan a * scan b) with a FLOAT product:
 * both columns flat and null-free, at least one RAY_F64, the other F64 or
 * integer-family.  int x int stays on the materialized path — its overflow
 * semantics (expr marks i64 overflow as null) must not silently change.
 * The fused read promotes each side to double exactly like expr's
 * promote(), so the accumulated sum is bit-compatible. */
bool try_prod_sumavg_input_f64(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                               agg_prod_t* out) {
    if (!g || !tbl || !input_op || !out) return false;
    if (input_op->opcode != OP_MUL || input_op->arity != 2) return false;
    ray_op_t* lhs = op_child(g, input_op, 0);
    ray_op_t* rhs = op_child(g, input_op, 1);
    if (!lhs || !rhs) return false;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_SCAN) return false;
    ray_op_ext_t* lx = find_ext(g, lhs->id);
    ray_op_ext_t* rx = find_ext(g, rhs->id);
    if (!lx || !rx) return false;
    ray_t* ca = ray_table_get_col(tbl, lx->sym);
    ray_t* cb = ray_table_get_col(tbl, rx->sym);
    if (!ca || !cb || RAY_IS_ERR(ca) || RAY_IS_ERR(cb)) return false;
    if (RAY_IS_PARTED(ca->type) || ca->type == RAY_MAPCOMMON) return false;
    if (RAY_IS_PARTED(cb->type) || cb->type == RAY_MAPCOMMON) return false;
    if ((ca->attrs & RAY_ATTR_HAS_NULLS) || (cb->attrs & RAY_ATTR_HAS_NULLS))
        return false;
    #define PROD_OK_T(t) ((t) == RAY_F64 || (t) == RAY_I64 || (t) == RAY_I32 || \
                          (t) == RAY_I16 || (t) == RAY_U8  || (t) == RAY_BOOL)
    if (!PROD_OK_T(ca->type) || !PROD_OK_T(cb->type)) return false;
    #undef PROD_OK_T
    if (ca->type != RAY_F64 && cb->type != RAY_F64) return false;

    out->enabled = true;
    out->pa = ray_data(ca); out->ta = ca->type; out->aa = ca->attrs;
    out->pb = ray_data(cb); out->tb = cb->type; out->ab = cb->attrs;
    return true;
}

/* Detect SUM/AVG affine inputs of form (scan +/- const) and return scan vector
 * plus the additive bias so we can adjust results from (sum,count) directly. */
bool try_affine_sumavg_input(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                                    ray_t** out_vec, agg_affine_t* out_affine) {
    if (!g || !tbl || !input_op || !out_vec || !out_affine) return false;
    if (input_op->opcode != OP_ADD && input_op->opcode != OP_SUB) return false;
    if (input_op->arity != 2 || !op_child(g, input_op, 0) || !op_child(g, input_op, 1)) return false;

    ray_op_t* lhs = op_child(g, input_op, 0);
    ray_op_t* rhs = op_child(g, input_op, 1);
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

/* ── Phase-0 instrumentation: why did expr_compile bail? ──
 * Diagnostic counters, not synchronized — compiles run on the query
 * thread; benign races on parallel sessions are acceptable for stats. */
uint64_t ray_expr_bail_counts[EXPR_BAIL__N];
uint64_t ray_expr_compile_ok;
bool     ray_expr_disable; /* test knob: force the fallback path */

#define EXPR_BAIL(reason) do {                          \
    ray_expr_bail_counts[(reason)]++;                   \
    return false;                                       \
} while (0)

static void expr_stats_dump(void) {
    static const char* names[EXPR_BAIL__N] = {
        "root-shape", "graph-size", "depth", "regs", "ins",
        "mapcommon", "str", "nulls", "slice", "sym-domain",
        "const", "null-shape", "other",
    };
    fprintf(stderr, "expr_compile ok=%llu\n",
            (unsigned long long)ray_expr_compile_ok);
    for (int i = 0; i < EXPR_BAIL__N; i++)
        if (ray_expr_bail_counts[i])
            fprintf(stderr, "expr_compile bail %-10s %llu\n", names[i],
                    (unsigned long long)ray_expr_bail_counts[i]);
}

void ray_expr_stats_init(void) {
    if (getenv("RAY_EXPR_STATS")) atexit(expr_stats_dump);
}

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
    out->regs[r].nullable = out->regs[src].nullable;
    out->n_regs++;
    out->n_scratch++;
    out->ins[out->n_ins++] = (expr_ins_t){
        .opcode = OP_CAST, .dst = r, .src1 = src, .src2 = 0xFF,
        .null_aware = out->regs[src].nullable ? 1 : 0,
    };
    return r;
}

/* Which (opcode, dst-type, src1-type) shapes have null-aware kernel
 * variants?  Landing per-family:
 *   Task 5: CAST shapes + F64 arithmetic (IEEE-propagating, no variant needed)
 *   Task 6: I64 BOOL comparisons + AND/OR + ISNULL
 *   Task 7: I64 arithmetic (ADD/SUB/MUL/DIV/MOD/MIN2/MAX2/NEG/ABS) +
 *            F64 MIN2/MAX2 (non-IEEE-propagating, variant added) */
static bool expr_null_capable(uint8_t op, int8_t dt, int8_t t1) {
    if (op == OP_CAST) {
        if (dt == RAY_F64 && t1 != RAY_F64) return true;  /* i64→f64: NaN map */
        if (dt == RAY_I64 && t1 == RAY_F64) return true;  /* f64→i64: NaN→NULL_I64 */
        if (dt == RAY_I64 && t1 != RAY_F64) return true;  /* narrow widen: sentinel map */
        if (dt == RAY_BOOL) return true;                   /* BOOL CAST: unconditionally sentinel-hardened;
                                                            * ignores null_aware param (unlike other clauses)
                                                            * — sentinel handling is baked into the kernel */
        /* Task 8: narrowing demotion to I32/I16; null_aware arm writes
         * NULL_I32/NULL_I16 for NULL_I64/NaN inputs.
         * t1 is unconstrained here because scratch sources are exclusively
         * RAY_I64/RAY_F64 lanes (register-widening invariant: REG_CONST and
         * REG_SCAN always widen to I64 or F64), and the kernel handles both
         * source types explicitly in the I32 and I16 branches. */
        if (dt == RAY_I32 || dt == RAY_I16) return true;
    }
    /* I64 comparisons and AND/OR: null-aware kernel handles NULL_I64 sentinel inline. */
    if (dt == RAY_BOOL && t1 == RAY_I64 &&
        ((op >= OP_EQ && op <= OP_GE) || op == OP_AND || op == OP_OR))
        return true;
    /* ISNULL: sentinel read in both F64 (NaN) and I64 (NULL_I64) arms; always capable. */
    if (op == OP_ISNULL) return true;
    /* Task 7: I64 arithmetic and unary ops.
     * OP_ADD=20..OP_MOD=24, OP_MIN2=33, OP_MAX2=34 in the binary block.
     * OP_NEG=10, OP_ABS=11 in the unary block.
     * OP_IDIV=49 is NOT contiguous with OP_ADD..OP_MOD and has no I64 plain
     * case in exec_elementwise_binary, so it is excluded. */
    if (dt == RAY_I64 && t1 != RAY_F64 &&
        ((op >= OP_ADD && op <= OP_MOD) || op == OP_MIN2 || op == OP_MAX2 ||
         op == OP_NEG || op == OP_ABS))
        return true;
    /* Task 7: F64 MIN2/MAX2 — null-aware kernel variant added (NaN propagation). */
    if (dt == RAY_F64 && (op == OP_MIN2 || op == OP_MAX2)) return true;
    return false;
}

/* Compile expression DAG into flat instruction array.
 * Returns true on success. Only compiles element-wise subtrees. */
bool expr_compile(ray_graph_t* g, ray_t* tbl, ray_op_t* root, ray_expr_t* out) {
    memset(out, 0, sizeof(*out));
    if (ray_expr_disable) return false; /* uncounted: test knob */
    if (!root || !g || !tbl) EXPR_BAIL(EXPR_BAIL_ROOT);
    if (root->opcode == OP_SCAN || root->opcode == OP_CONST) EXPR_BAIL(EXPR_BAIL_ROOT);
    if (!expr_is_elementwise(root->opcode)) EXPR_BAIL(EXPR_BAIL_ROOT);

    uint32_t nc = g->node_count;
    if (nc > 4096) EXPR_BAIL(EXPR_BAIL_SIZE); /* guard against stack overflow from VLA */
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
                ray_op_t* ch = op_child(g, node, i);
                if (!ch) continue;
                if (ch->id < nc && node_reg[ch->id] != 0xFF) continue;
                if (sp >= 64) EXPR_BAIL(EXPR_BAIL_DEPTH);
                dfs[sp++] = (dfs_t){ch, 0};
            }
        } else {
            sp--;
            uint8_t r = out->n_regs;
            if (r >= EXPR_MAX_REGS) EXPR_BAIL(EXPR_BAIL_REGS);

            if (node->opcode == OP_SCAN) {
                ray_op_ext_t* ext = find_ext(g, node->id);
                if (!ext) EXPR_BAIL(EXPR_BAIL_OTHER);
                ray_t* col = ray_table_get_col(tbl, ext->sym);
                if (!col) EXPR_BAIL(EXPR_BAIL_OTHER);
                if (col->type == RAY_MAPCOMMON) EXPR_BAIL(EXPR_BAIL_MAPCOMMON);
                if (col->type == RAY_STR) EXPR_BAIL(EXPR_BAIL_STR); /* RAY_STR needs string comparison path */
                if (col->attrs & RAY_ATTR_SLICE)     EXPR_BAIL(EXPR_BAIL_SLICE);
                /* Length-1 columns used as scalar broadcasts are handled by the
                 * fallback's exec_elementwise_binary (which has vec/scalar routing).
                 * The fused evaluator reads exactly nrows elements from every scan
                 * column — a length-1 column paired with nrows>1 would overread. */
                if (!RAY_IS_PARTED(col->type)) {
                    int64_t nrows = ray_table_nrows(tbl);
                    if (col->len != nrows) EXPR_BAIL(EXPR_BAIL_OTHER); /* length-mismatch (scalar broadcast) */
                }
                /* sym-domain Phase 2: the fused program loads SYM cells
                 * as raw i64s and resolves STR literals via the GLOBAL
                 * intern table (ray_sym_find below) — only valid for
                 * runtime-domain columns.  FILE-domain columns bail to
                 * the non-fused executor, which is domain-aware.  No-op
                 * pre-flip (every domain is the runtime singleton). */
                {
                    ray_t* sym_rep = sym_domain_rep(col);
                    if (sym_rep && ray_sym_vec_domain(sym_rep) !=
                                       ray_sym_runtime_domain())
                        EXPR_BAIL(EXPR_BAIL_SYM_DOMAIN);
                }
                /* Determine whether any lane in this column may be null.
                 * For parted columns the wrapper attrs may not reflect
                 * individual segments — scan all segments. */
                bool col_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) != 0;
                if (RAY_IS_PARTED(col->type)) {
                    ray_t** segs = (ray_t**)ray_data(col);
                    for (int64_t s = 0; s < col->len; s++)
                        if (segs[s] && (segs[s]->attrs & RAY_ATTR_HAS_NULLS))
                            col_nulls = true;
                }
                /* Nullable SYM is out of scope: sym ids are indistinguishable
                 * from the null sentinel (id 0) in raw integer lanes. */
                if (col_nulls && (col->type == RAY_SYM ||
                    (RAY_IS_PARTED(col->type) &&
                     RAY_PARTED_BASETYPE(col->type) == RAY_SYM)))
                    EXPR_BAIL(EXPR_BAIL_NULLS);
                out->regs[r].kind = REG_SCAN;
                if (RAY_IS_PARTED(col->type)) {
                    int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                    out->regs[r].col_type = base;
                    out->regs[r].data = NULL; /* resolved per-segment */
                    out->regs[r].is_parted = true;
                    out->regs[r].col_obj = NULL; /* parted: per-segment, no zone-skip */
                    out->regs[r].parted_col = col;
                    out->regs[r].type = (base == RAY_F64) ? RAY_F64 : RAY_I64;
                    out->regs[r].nullable = col_nulls;
                    out->has_parted = true;
                } else {
                    out->regs[r].col_type = col->type;
                    out->regs[r].col_attrs = col->attrs;
                    out->regs[r].data = ray_data(col);
                    out->regs[r].is_parted = false;
                    out->regs[r].col_obj = col;
                    out->regs[r].parted_col = NULL;
                    out->regs[r].type = (col->type == RAY_F64) ? RAY_F64 : RAY_I64;
                    out->regs[r].nullable = col_nulls;
                }
            } else if (node->opcode == OP_CONST) {
                ray_op_ext_t* ext = find_ext(g, node->id);
                if (!ext || !ext->literal) EXPR_BAIL(EXPR_BAIL_CONST);
                if (RAY_ATOM_IS_NULL(ext->literal)) EXPR_BAIL(EXPR_BAIL_CONST); /* null constants need the null-aware path */
                double cf; int64_t ci; bool is_f64;
                if (!atom_to_numeric(ext->literal, &cf, &ci, &is_f64)) {
                    /* Try resolving string constant to symbol intern ID —
                     * enables fused evaluation of SYM column comparisons
                     * (e.g. id2 = 'id080' compiles to integer EQ). */
                    if (ext->literal->type == -RAY_STR) {
                        const char* s = ray_str_ptr(ext->literal);
                        size_t slen = ray_str_len(ext->literal);
                        int64_t sid = ray_sym_find(s, slen);
                        if (sid < 0) EXPR_BAIL(EXPR_BAIL_CONST);
                        ci = sid;
                        cf = (double)sid;
                        is_f64 = false;
                    } else {
                        EXPR_BAIL(EXPR_BAIL_CONST);
                    }
                }
                out->regs[r].kind = REG_CONST;
                out->regs[r].type = is_f64 ? RAY_F64 : RAY_I64;
                out->regs[r].const_f64 = cf;
                out->regs[r].const_i64 = ci;
            } else if (expr_is_elementwise(node->opcode)) {
                if (!op_child(g, node, 0)) EXPR_BAIL(EXPR_BAIL_OTHER);
                uint8_t s1 = node_reg[node->in_id[0]];
                if (s1 == 0xFF) EXPR_BAIL(EXPR_BAIL_OTHER);
                uint8_t s2 = 0xFF;
                if (node->arity >= 2 && op_child(g, node, 1)) {
                    s2 = node_reg[node->in_id[1]];
                    if (s2 == 0xFF) EXPR_BAIL(EXPR_BAIL_OTHER);
                }

                int8_t t1 = out->regs[s1].type;
                int8_t t2 = (s2 != 0xFF) ? out->regs[s2].type : t1;
                uint16_t op = node->opcode;
                int8_t ot;

                /* Determine output type */
                if (op == OP_CAST)
                    ot = node->out_type;
                else if ((op >= OP_EQ && op <= OP_GE) ||
                    op == OP_AND || op == OP_OR || op == OP_NOT ||
                    op == OP_ISNULL)
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
                    if (r >= EXPR_MAX_REGS) EXPR_BAIL(EXPR_BAIL_REGS);
                } else if (ot == RAY_F64 && s2 != 0xFF) {
                    /* Arithmetic with f64 output — promote i64 inputs to f64 */
                    s1 = expr_ensure_type(out, s1, RAY_F64);
                    s2 = expr_ensure_type(out, s2, RAY_F64);
                    r = out->n_regs; /* re-read after possible CAST inserts */
                    if (r >= EXPR_MAX_REGS) EXPR_BAIL(EXPR_BAIL_REGS);
                } else if (ot == RAY_F64 && s2 == 0xFF) {
                    /* Unary f64 — promote input */
                    s1 = expr_ensure_type(out, s1, RAY_F64);
                    r = out->n_regs;
                    if (r >= EXPR_MAX_REGS) EXPR_BAIL(EXPR_BAIL_REGS);
                } else if (ot == RAY_BOOL && s2 != 0xFF && t1 != t2) {
                    /* Comparison with mixed types — promote both to f64 */
                    int8_t pt = (t1 == RAY_F64 || t2 == RAY_F64) ? RAY_F64 : RAY_I64;
                    s1 = expr_ensure_type(out, s1, pt);
                    s2 = expr_ensure_type(out, s2, pt);
                    r = out->n_regs;
                    if (r >= EXPR_MAX_REGS) EXPR_BAIL(EXPR_BAIL_REGS);
                }

                /* Compute nullability from the FINAL (post-promotion) s1/s2.
                 * Inserted CASTs inherit nullable from their source (Step 3),
                 * so the promoted regs already carry the right flag. */
                bool in_null = out->regs[s1].nullable ||
                               (s2 != 0xFF && out->regs[s2].nullable);
                bool ins_null_aware = false;
                bool dst_nullable   = false;
                if (in_null) {
                    bool cmp    = (op >= OP_EQ && op <= OP_GE);
                    bool boolop = (op == OP_AND || op == OP_OR || op == OP_NOT);
                    if (cmp || boolop || op == OP_ISNULL) {
                        /* Definite-bool output.
                         * F64 comparisons (EQ/NE/LT/LE/GT/GE): already null-
                         *   aware in the F64 BOOL block of expr_exec_binary
                         *   (explicit NaN handling).  No kernel variant needed.
                         * F64 AND/OR/NOT: these kernels have no NaN handling
                         *   (NaN is truthy as a raw double), so op == OP_NOT is
                         *   also included intentionally.  Mark null_aware so
                         *   the choke fires → fallback.
                         * Integer lanes: need bitmap post-pass → null_aware. */
                        ins_null_aware = (out->regs[s1].type != RAY_F64) ||
                                         op == OP_ISNULL ||
                                         op == OP_AND || op == OP_OR || op == OP_NOT;
                        dst_nullable = false;
                    } else if (op == OP_CAST) {
                        dst_nullable   = (ot != RAY_BOOL && ot != RAY_U8);
                        ins_null_aware = true;
                    } else {
                        /* Arithmetic:
                         * F64 standard ops (ADD/SUB/MUL/DIV/MOD, NEG/ABS/SQRT/LOG/EXP/CEIL/FLOOR/ROUND): NaN propagates
                         *   via IEEE for free — no kernel variant needed.
                         * F64 MIN2/MAX2: NOT IEEE-propagating — mark null_aware;
                         *   null-aware kernel variant now live (Task 7).
                         * F64 AND/OR/NOT: mark null_aware → capability choke fires → fallback.
                         * Non-F64 arithmetic (ADD/SUB/MUL/DIV/MOD/MIN2/MAX2/NEG/ABS): I64
                         *   null-aware arithmetic variants live (Task 7). */
                        bool f64_ieee_propagates = (ot == RAY_F64) &&
                            (op == OP_ADD || op == OP_SUB || op == OP_MUL ||
                             op == OP_DIV || op == OP_MOD ||
                             op == OP_NEG || op == OP_ABS ||
                             op == OP_SQRT || op == OP_LOG || op == OP_EXP ||
                             op == OP_CEIL || op == OP_FLOOR || op == OP_ROUND);
                        ins_null_aware = !f64_ieee_propagates;
                        dst_nullable   = true;
                    }
                    if (ot == RAY_U8)
                        EXPR_BAIL(EXPR_BAIL_NULL_SHAPE); /* no U8 sentinel */
                }

                out->regs[r].kind = REG_SCRATCH;
                out->regs[r].type = ot;
                out->regs[r].nullable = dst_nullable;
                out->n_scratch++;

                if (out->n_ins >= EXPR_MAX_INS) EXPR_BAIL(EXPR_BAIL_INS);
                out->ins[out->n_ins++] = (expr_ins_t){
                    .opcode = (uint8_t)op, .dst = r, .src1 = s1, .src2 = s2,
                    .null_aware = ins_null_aware ? 1 : 0,
                };
            } else {
                EXPR_BAIL(EXPR_BAIL_OTHER);
            }

            out->n_regs++;
            if (node->id < nc) node_reg[node->id] = r;
        }
    }

    if (out->n_regs == 0 || out->n_ins == 0) EXPR_BAIL(EXPR_BAIL_OTHER);
    out->out_reg = out->n_regs - 1;
    out->out_type = out->regs[out->out_reg].type;

    /* Null-capability choke: a nullable program may only use instructions
     * with null-aware kernel variants. Bail (to the fallback) otherwise —
     * never produce wrong results. */
    for (uint8_t i = 0; i < out->n_ins; i++) {
        const expr_ins_t* in = &out->ins[i];
        if (in->null_aware &&
            !expr_null_capable(in->opcode, out->regs[in->dst].type,
                               out->regs[in->src1].type))
            EXPR_BAIL(EXPR_BAIL_NULL_SHAPE);
    }

    ray_expr_compile_ok++;
    return true;
}

/* ---- Morsel-batched expression evaluator ---- */

/* Load SCAN column data into i64 scratch buffer with type conversion.
 * has_nulls: if true, narrow sentinels are mapped to NULL_I64 during widen
 * so kernels only ever see the canonical NULL_I64 (INT64_MIN) sentinel. */
static void expr_load_i64(int64_t* dst, const void* data, int8_t col_type,
                          uint8_t col_attrs, bool has_nulls,
                          int64_t start, int64_t n) {
    switch (col_type) {
        case RAY_I64: case RAY_TIMESTAMP:
            /* NULL_I64 already canonical in place — direct copy */
            memcpy(dst, (const int64_t*)data + start, (size_t)n * 8);
            break;
        case RAY_SYM: {
            for (int64_t j = 0; j < n; j++)
                dst[j] = ray_read_sym(data, start + j, col_type, col_attrs);
        } break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* s = (const int32_t*)data + start;
            if (has_nulls)
                for (int64_t j = 0; j < n; j++)
                    dst[j] = (s[j] == NULL_I32) ? NULL_I64 : (int64_t)s[j];
            else
                for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        case RAY_U8: case RAY_BOOL: {
            /* U8/BOOL are non-nullable — no sentinel mapping needed */
            const uint8_t* s = (const uint8_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        case RAY_I16: {
            const int16_t* s = (const int16_t*)data + start;
            if (has_nulls)
                for (int64_t j = 0; j < n; j++)
                    dst[j] = (s[j] == NULL_I16) ? NULL_I64 : (int64_t)s[j];
            else
                for (int64_t j = 0; j < n; j++) dst[j] = s[j];
        } break;
        default: memset(dst, 0, (size_t)n * 8); break;
    }
}

/* Load SCAN column data into f64 scratch buffer with type conversion.
 * has_nulls: if true, integer sentinels are mapped to NULL_F64 (NaN) so the
 * f64 kernel receives the canonical NaN null and IEEE propagation works. */
static void expr_load_f64(double* dst, const void* data, int8_t col_type,
                          uint8_t col_attrs, bool has_nulls,
                          int64_t start, int64_t n) {
    switch (col_type) {
        case RAY_F64:
            /* NaN already canonical in place — direct copy */
            memcpy(dst, (const double*)data + start, (size_t)n * 8);
            break;
        case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* s = (const int64_t*)data + start;
            if (has_nulls)
                for (int64_t j = 0; j < n; j++)
                    dst[j] = (s[j] == NULL_I64) ? NULL_F64 : (double)s[j];
            else
                for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_SYM: {
            for (int64_t j = 0; j < n; j++)
                dst[j] = (double)ray_read_sym(data, start + j, col_type, col_attrs);
        } break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* s = (const int32_t*)data + start;
            if (has_nulls)
                for (int64_t j = 0; j < n; j++)
                    dst[j] = (s[j] == NULL_I32) ? NULL_F64 : (double)s[j];
            else
                for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_U8: case RAY_BOOL: {
            /* U8/BOOL are non-nullable — no sentinel mapping needed */
            const uint8_t* s = (const uint8_t*)data + start;
            for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        case RAY_I16: {
            const int16_t* s = (const int16_t*)data + start;
            if (has_nulls)
                for (int64_t j = 0; j < n; j++)
                    dst[j] = (s[j] == NULL_I16) ? NULL_F64 : (double)s[j];
            else
                for (int64_t j = 0; j < n; j++) dst[j] = (double)s[j];
        } break;
        default: memset(dst, 0, (size_t)n * 8); break;
    }
}

/* Execute a binary instruction over n elements.
 * Switch is OUTSIDE the loop so each case auto-vectorizes.
 * null_aware selects sentinel-checking kernel variants.
 *
 * Step 2 — fallback semantics extracted from exec_elementwise_binary
 * (exec.c → propagate_nulls_binary, fix_null_comparisons, zero-divisor pass):
 *
 * I64 arithmetic null truth table (null = NULL_I64 = INT64_MIN sentinel):
 *   ADD/SUB/MUL:   null(a) || null(b)  → NULL_I64
 *   DIV/MOD:       null(a) || null(b)  → NULL_I64  (propagate_nulls_binary first)
 *                  b==0 (non-null)     → NULL_I64  (zero-divisor post-pass)
 *                  b==-1 && a==INT64_MIN → NULL_I64 (overflow guard → 0 → zero-divisor pass marks null)
 *   MIN2/MAX2 i64: null(a) || null(b)  → NULL_I64  (op_propagates_null=true for MIN2/MAX2)
 *   NEG/ABS i64:   null(a)             → NULL_I64
 *                  result==INT64_MIN   → NULL_I64  (overflow, handled by mark_i64_overflow_as_null)
 *
 * F64 MIN2/MAX2 null truth table (null = NaN, IEEE min/max does NOT propagate NaN):
 *   null(a) || null(b) → NaN (propagate_nulls_binary fires after the loop)
 *   non-null inputs    → plain comparison
 *
 * AND/OR on I64-dst: task derivation forces BOOL dst for AND/OR since Task 6,
 *   so these I64-dst cases are unreachable for null_aware programs; left plain.
 */
static void expr_exec_binary(uint8_t opcode, uint8_t null_aware, int8_t dt, void* dp,
                              int8_t t1, const void* ap,
                              int8_t t2, const void* bp, int64_t n) {
    (void)t2;
    if (dt == RAY_F64) {
        double* d = (double*)dp;
        const double* a = (const double*)ap;
        const double* b = (const double*)bp;
        /* F64 MIN2/MAX2 null-aware variant: NaN (null sentinel) in either
         * operand → NaN output.  IEEE min/max comparisons return false for NaN
         * so we must check explicitly.  All other F64 ops rely on IEEE NaN
         * propagation and need no null_aware variant. */
        if (null_aware && (opcode == OP_MIN2 || opcode == OP_MAX2)) {
            #define F64_ISN(x) ((x) != (x))
            /* null in either operand → NULL_F64 (single-null model: write the
             * canonical sentinel, not a bare NAN).  min/max of finite values
             * is finite so the non-null branch needs no fin-canonicalization. */
            if (opcode == OP_MIN2)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (F64_ISN(a[j]) || F64_ISN(b[j])) ? NULL_F64
                          : (a[j] < b[j] ? a[j] : b[j]);
            else
                for (int64_t j = 0; j < n; j++)
                    d[j] = (F64_ISN(a[j]) || F64_ISN(b[j])) ? NULL_F64
                          : (a[j] > b[j] ? a[j] : b[j]);
            #undef F64_ISN
            return;
        }
        /* Single-null float model: canonicalize every non-finite F64 result
         * (NaN OR ±Inf, incl. overflow) to NULL_F64 via ray_f64_fin (branchless
         * compare+select — stays auto-vectorized).  DIV/MOD keep the explicit
         * divisor==0 guard (avoids the FPU NaN slow path) and wrap the value. */
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(a[j] + b[j]); break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(a[j] - b[j]); break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(a[j] * b[j]); break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) d[j] = b[j] != 0.0 ? ray_f64_fin(a[j] / b[j]) : NULL_F64; break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) {
                if (b[j] == 0.0) { d[j] = NULL_F64; continue; }
                double m = fmod(a[j], b[j]);
                m = (m && ((m > 0) != (b[j] > 0))) ? m + b[j] : m;
                d[j] = ray_f64_fin(m);
            } break;
            case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
            case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
            default: break;
        }
    } else if (dt == RAY_I64 || dt == RAY_TIMESTAMP) {
        int64_t* d = (int64_t*)dp;
        const int64_t* a = (const int64_t*)ap;
        const int64_t* b = (const int64_t*)bp;
        /* Null-aware I64 arithmetic: NULL_I64 = INT64_MIN sentinel.
         * Mirrors the fallback (propagate_nulls_binary + zero-divisor pass). */
        #define I64_ISN(x) ((x) == NULL_I64)
        if (null_aware) {
            switch (opcode) {
                case OP_ADD: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?NULL_I64:(int64_t)((uint64_t)a[j]+(uint64_t)b[j]); break;
                case OP_SUB: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?NULL_I64:(int64_t)((uint64_t)a[j]-(uint64_t)b[j]); break;
                case OP_MUL: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?NULL_I64:(int64_t)((uint64_t)a[j]*(uint64_t)b[j]); break;
                /* DIV/MOD: null check FIRST (mirrors fallback's propagate_nulls_binary before
                 * zero-divisor pass); then existing zero/overflow guards verbatim.
                 * b==0 (non-null) → NULL_I64 mirrors fallback's zero-divisor post-pass.
                 * b==-1 && a==INT64_MIN → overflow: direct NULL_I64 (vs fallback: loop writes 0
                 * then zero-divisor pass marks null — same observable result). */
                case OP_DIV: for (int64_t j=0;j<n;j++) {
                    if (I64_ISN(a[j])||I64_ISN(b[j])) { d[j]=NULL_I64; continue; }
                    if (b[j]==0||(b[j]==-1&&a[j]==INT64_MIN)) { d[j]=NULL_I64; continue; }
                    int64_t q=a[j]/b[j];
                    if ((a[j]^b[j])<0&&q*b[j]!=a[j]) q--;
                    d[j]=q;
                } break;
                case OP_MOD: for (int64_t j=0;j<n;j++) {
                    if (I64_ISN(a[j])||I64_ISN(b[j])) { d[j]=NULL_I64; continue; }
                    if (b[j]==0||(b[j]==-1&&a[j]==INT64_MIN)) { d[j]=NULL_I64; continue; }
                    int64_t m=a[j]%b[j];
                    if (m&&(m^b[j])<0) m+=b[j];
                    d[j]=m;
                } break;
                /* MIN2/MAX2: null in either → NULL_I64 (mirrors propagate_nulls_binary). */
                case OP_MIN2: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?NULL_I64:(a[j]<b[j]?a[j]:b[j]); break;
                case OP_MAX2: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?NULL_I64:(a[j]>b[j]?a[j]:b[j]); break;
                /* AND/OR on I64-dst are unreachable for null_aware programs (dst is BOOL
                 * since Task 6); guarded by expr_null_capable.
                 * Capability/kernel desync guard — must stay in sync with expr_null_capable. */
                default: memset(dp, 0, (size_t)n * 8); break;
            }
        } else {
            switch (opcode) {
                case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] + (uint64_t)b[j]); break;
                case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] - (uint64_t)b[j]); break;
                case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)((uint64_t)a[j] * (uint64_t)b[j]); break;
                case OP_DIV: for (int64_t j = 0; j < n; j++) {
                    if (b[j]==0 || (b[j]==-1 && a[j]==INT64_MIN)) { d[j]=0; continue; }
                    int64_t q = a[j]/b[j];
                    if ((a[j]^b[j])<0 && q*b[j]!=a[j]) q--;
                    d[j] = q;
                } break;
                case OP_MOD: for (int64_t j = 0; j < n; j++) {
                    if (b[j]==0 || (b[j]==-1 && a[j]==INT64_MIN)) { d[j]=0; continue; }
                    int64_t m = a[j]%b[j];
                    if (m && (m^b[j])<0) m+=b[j];
                    d[j] = m;
                } break;
                case OP_MIN2: for (int64_t j = 0; j < n; j++) d[j] = a[j] < b[j] ? a[j] : b[j]; break;
                case OP_MAX2: for (int64_t j = 0; j < n; j++) d[j] = a[j] > b[j] ? a[j] : b[j]; break;
                case OP_AND:  for (int64_t j = 0; j < n; j++) d[j] = (a[j] && b[j]) ? 1 : 0; break;
                case OP_OR:   for (int64_t j = 0; j < n; j++) d[j] = (a[j] || b[j]) ? 1 : 0; break;
                default: break;
            }
        }
        #undef I64_ISN
    } else if (dt == RAY_I32 || dt == RAY_DATE || dt == RAY_TIME) {
        int32_t* d = (int32_t*)dp;
        const int32_t* a = (const int32_t*)ap;
        const int32_t* b = (const int32_t*)bp;
        switch (opcode) {
            case OP_ADD: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] + (uint32_t)b[j]); break;
            case OP_SUB: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] - (uint32_t)b[j]); break;
            case OP_MUL: for (int64_t j = 0; j < n; j++) d[j] = (int32_t)((uint32_t)a[j] * (uint32_t)b[j]); break;
            case OP_DIV: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==INT32_MIN)) { d[j]=0; continue; }
                int32_t q = a[j]/b[j];
                if ((a[j]^b[j])<0 && q*b[j]!=a[j]) q--;
                d[j] = q;
            } break;
            case OP_MOD: for (int64_t j = 0; j < n; j++) {
                if (b[j]==0 || (b[j]==-1 && a[j]==INT32_MIN)) { d[j]=0; continue; }
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
            /* Null-aware I64 comparisons: NULL_I64 = INT64_MIN sentinel.
             * Truth table matches the fallback (fix_null_comparisons + op_propagates_null):
             *   null == null → 1;  null < non-null → 1;  non-null > null → 1
             *   null == non-null → 0;  non-null < null → 0
             *   null != null → 0;  null != non-null → 1;  non-null != null → 1
             *   AND/OR with any null operand → 0 (false) */
            #define I64_ISN(x) ((x) == NULL_I64)
            if (null_aware) {
                switch (opcode) {
                    case OP_EQ: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?1:(I64_ISN(a[j])||I64_ISN(b[j]))?0:a[j]==b[j]; break;
                    case OP_NE: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?0:(I64_ISN(a[j])||I64_ISN(b[j]))?1:a[j]!=b[j]; break;
                    case OP_LT: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?0:I64_ISN(a[j])?1:I64_ISN(b[j])?0:a[j]<b[j]; break;
                    case OP_LE: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?1:I64_ISN(a[j])?1:I64_ISN(b[j])?0:a[j]<=b[j]; break;
                    case OP_GT: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?0:I64_ISN(b[j])?1:I64_ISN(a[j])?0:a[j]>b[j]; break;
                    case OP_GE: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])&&I64_ISN(b[j]))?1:I64_ISN(b[j])?1:I64_ISN(a[j])?0:a[j]>=b[j]; break;
                    /* AND/OR: null on either side → false (0) */
                    case OP_AND: for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?0:(a[j]&&b[j])?1:0; break;
                    case OP_OR:  for (int64_t j=0;j<n;j++) d[j]=(I64_ISN(a[j])||I64_ISN(b[j]))?0:(a[j]||b[j])?1:0; break;
                    default: break;
                }
            } else {
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
                    /* BOOL cols are loaded as I64 abstract via expr_load_i64;
                     * AND/OR on such inputs lands here with dt=BOOL t1=t2=I64. */
                    case OP_AND: for (int64_t j = 0; j < n; j++) d[j] = (a[j] && b[j]) ? 1 : 0; break;
                    case OP_OR:  for (int64_t j = 0; j < n; j++) d[j] = (a[j] || b[j]) ? 1 : 0; break;
                    default: break;
                }
            }
            #undef I64_ISN
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

/* Execute a unary instruction over n elements.
 * null_aware: when true, kernels check for the type-correct null sentinel
 * and propagate it.  For CAST: maps type-specific sentinels across the type
 * boundary (NULL_I32/NULL_I16 → NULL_I64, NULL_I64 → NULL_F64, etc.).
 * For I64 NEG/ABS: null(a) → NULL_I64; non-null overflow still handled by
 * the mark_i64_overflow_as_null post-pass (INT64_MIN result → null). */
static void expr_exec_unary(uint8_t opcode, uint8_t null_aware, int8_t dt, void* dp,
                             int8_t t1, const void* ap, int64_t n) {
    if (dt == RAY_F64) {
        double* d = (double*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            switch (opcode) {
                /* NEG/ABS/CEIL/FLOOR/ROUND of a finite value are finite (and a
                 * NaN sentinel input stays NaN through them), so they need no
                 * fin-canonicalization.  SQRT/LOG/EXP can map a finite input to
                 * a non-finite (sqrt(<0)=NaN, log(0)=-Inf, exp(big)=Inf) →
                 * canonicalize to NULL_F64 (single-null float model). */
                case OP_NEG:   for (int64_t j = 0; j < n; j++) d[j] = -a[j]; break;
                case OP_ABS:   for (int64_t j = 0; j < n; j++) d[j] = fabs(a[j]); break;
                case OP_SQRT:  for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(sqrt(a[j])); break;
                case OP_LOG:   for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(log(a[j])); break;
                case OP_EXP:   for (int64_t j = 0; j < n; j++) d[j] = ray_f64_fin(exp(a[j])); break;
                case OP_CEIL:  for (int64_t j = 0; j < n; j++) d[j] = ceil(a[j]); break;
                case OP_FLOOR: for (int64_t j = 0; j < n; j++) d[j] = floor(a[j]); break;
                case OP_ROUND: for (int64_t j = 0; j < n; j++) d[j] = round(a[j]); break;
                /* OP_CAST F64→F64: same-buffer-issue as I64→I64 (see below). */
                case OP_CAST:  memcpy(d, a, (size_t)n * sizeof(double)); break;
                default: break;
            }
        } else { /* CAST i64→f64: with null_aware map NULL_I64 → NULL_F64 */
            const int64_t* a = (const int64_t*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] == NULL_I64) ? NULL_F64 : (double)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = (double)a[j];
        }
    } else if (dt == RAY_I64) {
        int64_t* d = (int64_t*)dp;
        if (t1 == RAY_I64) {
            const int64_t* a = (const int64_t*)ap;
            /* Null-aware NEG/ABS: null(a) → NULL_I64.  Overflow (result==INT64_MIN)
             * is handled by the mark_i64_overflow_as_null post-pass in expr_eval_full. */
            if (null_aware) {
                #define I64_ISN(x) ((x) == NULL_I64)
                switch (opcode) {
                    case OP_NEG: for (int64_t j=0;j<n;j++) d[j]=I64_ISN(a[j])?NULL_I64:(int64_t)(-(uint64_t)a[j]); break;
                    case OP_ABS: for (int64_t j=0;j<n;j++) d[j]=I64_ISN(a[j])?NULL_I64:(a[j]<0?(int64_t)(-(uint64_t)a[j]):a[j]); break;
                    case OP_CAST: memcpy(d, a, (size_t)n * sizeof(int64_t)); break;
                    default: break;
                }
                #undef I64_ISN
                return;
            }
            switch (opcode) {
                /* Unsigned negation avoids UB on INT64_MIN */
                case OP_NEG: for (int64_t j = 0; j < n; j++) d[j] = (int64_t)(-(uint64_t)a[j]); break;
                case OP_ABS: for (int64_t j = 0; j < n; j++) d[j] = a[j] < 0 ? (int64_t)(-(uint64_t)a[j]) : a[j]; break;
                /* OP_CAST I64→I64 is logically a no-op, but src and dst are
                 * separate scratch buffers: the dst slot must still receive
                 * the data.  SCAN U8/BOOL/I16/I32 columns get loaded into
                 * the I64 abstract via expr_load_i64; any subsequent
                 * `(as 'I64 col)` lands in this branch and would otherwise
                 * leave dst un-initialised. */
                case OP_CAST: memcpy(d, a, (size_t)n * sizeof(int64_t)); break;
                default: break;
            }
        } else if (t1 == RAY_BOOL) {
            /* CAST bool→i64 — BOOL scratch is 1 byte per elem (0/1). */
            const uint8_t* a = (const uint8_t*)ap;
            for (int64_t j = 0; j < n; j++) d[j] = a[j];
        } else if (t1 == RAY_I32 || t1 == RAY_DATE || t1 == RAY_TIME) {
            /* CAST i32→i64: with null_aware map NULL_I32 → NULL_I64. */
            const int32_t* a = (const int32_t*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] == NULL_I32) ? NULL_I64 : (int64_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = a[j];
        } else if (t1 == RAY_I16) {
            /* CAST i16→i64: with null_aware map NULL_I16 → NULL_I64. */
            const int16_t* a = (const int16_t*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] == NULL_I16) ? NULL_I64 : (int64_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = a[j];
        } else { /* CAST f64→i64: clamp + null_aware NaN → NULL_I64 */
            const double* a = (const double*)ap;
            /* Null-model invariant 16.4: INT64_MIN == NULL_I64 is the reserved
             * null sentinel and must NOT be produced as a real clamp value
             * (it would read back as null).  Clamp the negative-overflow
             * floor to INT64_MIN+1 — the most-negative *representable*
             * non-null i64 — so a saturated cast stays a real value. */
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] != a[j]) ? NULL_I64
                         : (a[j] >= (double)INT64_MAX) ? INT64_MAX
                         : (a[j] <= (double)INT64_MIN) ? (INT64_MIN + 1)
                         : (int64_t)a[j];
            else
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] >= (double)INT64_MAX) ? INT64_MAX
                         : (a[j] <= (double)INT64_MIN) ? (INT64_MIN + 1)
                         : (int64_t)a[j];
        }
    } else if (dt == RAY_BOOL) {
        uint8_t* d = (uint8_t*)dp;
        if (opcode == OP_ISNULL) {
            /* ISNULL: 1 where input is the type-correct null sentinel, else 0.
             * Null-free lanes → all false (correctly).  Works regardless of
             * null_aware since we always read the sentinel directly. */
            if (t1 == RAY_F64) {
                const double* a = (const double*)ap;
                for (int64_t j = 0; j < n; j++) d[j] = (a[j] != a[j]) ? 1 : 0;
            } else {
                const int64_t* a = (const int64_t*)ap;
                for (int64_t j = 0; j < n; j++) d[j] = (a[j] == NULL_I64) ? 1 : 0;
            }
        } else if (opcode == OP_CAST) {
            /* (as 'BOOL ...) — truthy semantics, but treat null sentinel
             * as false (BOOL is non-nullable, so we can't preserve null
             * structurally; a SQL-style "missing → not true" mapping is
             * the least-surprising convention).  For F64, NULL_F64 = NaN:
             * the IEEE `NaN != 0.0` is true, so add an explicit NaN check
             * (`a[j] == a[j]` is false iff NaN).  For I64, NULL_I64 =
             * INT64_MIN is a regular non-zero value, so skip it. */
            if (t1 == RAY_F64) {
                const double* a = (const double*)ap;
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] != 0.0 && a[j] == a[j]) ? 1 : 0;
            } else {
                const int64_t* a = (const int64_t*)ap;
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] != 0 && a[j] != NULL_I64) ? 1 : 0;
            }
        } else {
            const uint8_t* a = (const uint8_t*)ap;
            switch (opcode) {
                case OP_NOT: for (int64_t j = 0; j < n; j++) d[j] = !a[j]; break;
                default: break;
            }
        }
    } else if (dt == RAY_I32) {
        /* OP_CAST narrow output — src came from I64/F64 scratch (filled
         * by REG_CONST or REG_SCAN widening); truncate to int32_t.
         *
         * Fallback semantics (exec_elementwise_unary + propagate_nulls):
         *   non-null → plain (int32_t) truncation; null → propagate_nulls
         *   calls ray_vec_set_null which writes NULL_I32 (INT32_MIN) into
         *   the data cell.  ray_vec_is_null is sentinel-based, so the
         *   null_aware arm must write NULL_I32 inline for null inputs.
         *
         * No out-of-range clamping is applied deliberately.  The fallback
         * (exec_elementwise_unary) also performs bare truncation for out-of-
         * range doubles; this is pre-existing UB shared by both paths.  On
         * x86 the hardware F64→I32 conversion saturates to INT32_MIN for
         * out-of-range values, which happens to equal NULL_I32 — so both
         * the plain arm and this null_aware arm surface out-of-range inputs
         * as null in practice.  Clamping only this arm would diverge from
         * the fallback oracle without fixing the root UB.
         * Contrast: F64→I64 clamps explicitly (see `dt == RAY_I64` branch
         * above, ~line 1189) because INT64_MIN == NULL_I64 makes silent
         * mis-nulling observable, so that arm deserves the guard. */
        int32_t* d = (int32_t*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] != a[j]) ? NULL_I32 : (int32_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = (int32_t)a[j];
        } else {
            const int64_t* a = (const int64_t*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] == NULL_I64) ? NULL_I32 : (int32_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = (int32_t)a[j];
        }
    } else if (dt == RAY_I16) {
        /* Fallback semantics identical to I32: plain truncation for
         * non-null, NULL_I16 (INT16_MIN) sentinel written at null positions.
         *
         * No out-of-range clamping for the same reason as the I32 arm above:
         * the fallback performs bare truncation too (pre-existing UB shared
         * by both paths), and on x86 the saturating hardware conversion
         * coincidentally produces INT16_MIN == NULL_I16 for out-of-range
         * doubles — keeping both arms behaviorally consistent.  Clamping
         * only the null_aware arm would diverge from the fallback oracle. */
        int16_t* d = (int16_t*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] != a[j]) ? NULL_I16 : (int16_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = (int16_t)a[j];
        } else {
            const int64_t* a = (const int64_t*)ap;
            if (null_aware)
                for (int64_t j = 0; j < n; j++)
                    d[j] = (a[j] == NULL_I64) ? NULL_I16 : (int16_t)a[j];
            else
                for (int64_t j = 0; j < n; j++) d[j] = (int16_t)a[j];
        }
    } else if (dt == RAY_U8) {
        uint8_t* d = (uint8_t*)dp;
        if (t1 == RAY_F64) {
            const double* a = (const double*)ap;
            for (int64_t j = 0; j < n; j++) d[j] = (uint8_t)a[j];
        } else {
            const int64_t* a = (const int64_t*)ap;
            for (int64_t j = 0; j < n; j++) d[j] = (uint8_t)a[j];
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
                /* Direct pointer if native type matches, else convert.
                 * Nullable F64/I64 columns may take the direct path: their
                 * sentinels (NaN / NULL_I64) are already canonical in place.
                 * Narrow types always go through expr_load_* for sentinel
                 * normalization. */
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
                    bool hn = expr->regs[r].nullable;
                    if (rt == RAY_F64)
                        expr_load_f64(scratch[r], expr->regs[r].data, ct, ca, hn, start, n);
                    else
                        expr_load_i64(scratch[r], expr->regs[r].data, ct, ca, hn, start, n);
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
            expr_exec_binary(ins->opcode, ins->null_aware, dt, rptrs[ins->dst],
                             expr->regs[ins->src1].type, rptrs[ins->src1],
                             expr->regs[ins->src2].type, rptrs[ins->src2], n);
        } else {
            expr_exec_unary(ins->opcode, ins->null_aware, dt, rptrs[ins->dst],
                            expr->regs[ins->src1].type, rptrs[ins->src1], n);
        }
    }

    return rptrs[expr->out_reg];
}

/* ── Per-morsel chunk-zone skip for fused BOOL predicates ──────────────────
 *
 * A WHERE comparison (or AND/OR tree of comparisons) over chunk-zone-indexed
 * integer/temporal columns can frequently be decided for a whole morsel from
 * the per-chunk [min,max] extrema alone — without reading a single column
 * value.  This recovers the block-skip the deleted OP_FILTERED_GROUP operator
 * provided, but as a general property of the fused evaluator (every BOOL
 * expression benefits; nothing is benchmark-shaped).
 *
 * The decision is tri-state per register during a walk of the compiled
 * instruction list:  1 = all-pass, 0 = all-fail, mixed/none = undecided.
 * Each comparison's tri-state is proven by chunk extrema (mirrors the proven
 * fp_eval_cmp logic); three-valued AND/OR composition preserves soundness.
 * Any non-{compare,AND,OR} instruction, or a comparison whose operands aren't
 * (zone-column, integer-const), degrades that register to "mixed", so the
 * final answer is conservative by construction — a wrong "all-pass"/"all-fail"
 * is impossible. */
#define ZT_MIXED  (-1)   /* boolean, but value varies within the chunk */
#define ZT_NONE   (-2)   /* register is not a boolean we can reason about */

static inline int zt_and(int a, int b) {
    if (a == 0 || b == 0) return 0;          /* FALSE dominates AND */
    if (a == 1 && b == 1) return 1;
    return ZT_MIXED;
}
static inline int zt_or(int a, int b) {
    if (a == 1 || b == 1) return 1;          /* TRUE dominates OR */
    if (a == 0 && b == 0) return 0;
    return ZT_MIXED;
}
/* Mirror a comparison operator when the column is the RIGHT operand
 * (const OP col ≡ col swap(OP) const).  EQ/NE are symmetric. */
static inline uint16_t zone_swap_op(uint16_t op) {
    switch (op) {
    case OP_LT: return OP_GT;
    case OP_GT: return OP_LT;
    case OP_LE: return OP_GE;
    case OP_GE: return OP_LE;
    default:    return op;
    }
}

/* Decide one comparison (col cmp_op cval) over chunk `ch` from its int64
 * extrema.  cmp_op is normalized so the column is the left operand.  The
 * all-pass arm is gated on "no nulls in the chunk" (a NULL lane yields BOOL 0,
 * never 1); the all-fail arm needs no guard (NULL op const is never TRUE). */
static int zone_cmp_decision(const ray_index_t* ix, int64_t ch,
                             uint16_t cmp_op, int64_t cval) {
    const int64_t* mins = (const int64_t*)ray_data(ix->u.chunk_zone.mins);
    const int64_t* maxs = (const int64_t*)ray_data(ix->u.chunk_zone.maxs);
    int64_t cmin = mins[ch], cmax = maxs[ch];
    if (cmin > cmax) return ZT_MIXED;        /* all-null chunk: leave to kernel */
    const uint8_t* nb = (const uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    bool has_nulls = (nb[ch >> 3] >> (ch & 7)) & 1u;
    switch (cmp_op) {
    case OP_EQ:
        if (cval < cmin || cval > cmax)                 return 0;
        if (!has_nulls && cmin == cmax)                 return 1;
        break;
    case OP_NE:
        if (!has_nulls && (cval < cmin || cval > cmax)) return 1;
        if (cmin == cmax && cval == cmin)               return 0;
        break;
    case OP_LT:
        if (cmin >= cval)                               return 0;
        if (!has_nulls && cmax <  cval)                 return 1;
        break;
    case OP_LE:
        if (cmin >  cval)                               return 0;
        if (!has_nulls && cmax <= cval)                 return 1;
        break;
    case OP_GT:
        if (cmax <= cval)                               return 0;
        if (!has_nulls && cmin >  cval)                 return 1;
        break;
    case OP_GE:
        if (cmax <  cval)                               return 0;
        if (!has_nulls && cmin >= cval)                 return 1;
        break;
    default: break;
    }
    return ZT_MIXED;
}

/* Does `col` carry a fresh, integer/temporal chunk-zone index? */
static inline const ray_index_t* zone_fresh_index(const ray_t* col) {
    if (!col || !(col->attrs & RAY_ATTR_HAS_INDEX) || !col->index) return NULL;
    const ray_index_t* ix = ray_index_payload(col->index);
    if (ix->kind != RAY_IDX_CHUNK_ZONE ||
        ix->built_for_len != col->len ||
        ix->u.chunk_zone.is_f64) return NULL;
    return ix;
}

/* Cheap O(n_regs) pre-check: is any scan register zone-eligible?  Computed
 * once per evaluation so the per-morsel walk is skipped entirely otherwise. */
static bool expr_zone_eligible(const ray_expr_t* expr) {
    if (expr->out_type != RAY_BOOL) return false;
    for (uint8_t r = 0; r < expr->n_regs; r++)
        if (expr->regs[r].kind == REG_SCAN && zone_fresh_index(expr->regs[r].col_obj))
            return true;
    return false;
}

/* Decide morsel [ms, me) from chunk-zone extrema without reading column data.
 * Returns 1 (all-pass), 0 (all-fail), or -1 (undecided — evaluate normally). */
static int expr_zone_decide(const ray_expr_t* expr, int64_t ms, int64_t me) {
    int tri[EXPR_MAX_REGS];
    for (uint8_t r = 0; r < expr->n_regs; r++) tri[r] = ZT_NONE;

    for (uint8_t i = 0; i < expr->n_ins; i++) {
        const expr_ins_t* in = &expr->ins[i];
        uint16_t op = in->opcode;
        if (op >= OP_EQ && op <= OP_GE && in->src2 != 0xFF) {
            const ray_t* col = NULL; int64_t cval = 0; uint16_t cmp = op;
            uint8_t a = in->src1, b = in->src2;
            if (expr->regs[a].kind == REG_SCAN &&
                expr->regs[b].kind == REG_CONST && expr->regs[b].type == RAY_I64) {
                col = expr->regs[a].col_obj; cval = expr->regs[b].const_i64;
            } else if (expr->regs[b].kind == REG_SCAN &&
                       expr->regs[a].kind == REG_CONST && expr->regs[a].type == RAY_I64) {
                col = expr->regs[b].col_obj; cval = expr->regs[a].const_i64;
                cmp = zone_swap_op(op);
            }
            int d = ZT_MIXED;
            const ray_index_t* ix = zone_fresh_index(col);
            if (ix) {
                uint8_t log2 = ix->u.chunk_zone.chunk_log2;
                int64_t s_ch = ms >> log2, e_ch = (me - 1) >> log2;
                if (s_ch == e_ch && (uint32_t)s_ch < ix->u.chunk_zone.n_chunks)
                    d = zone_cmp_decision(ix, s_ch, cmp, cval);
            }
            tri[in->dst] = d;
        } else if (op == OP_AND && in->src2 != 0xFF) {
            tri[in->dst] = zt_and(tri[in->src1], tri[in->src2]);
        } else if (op == OP_OR && in->src2 != 0xFF) {
            tri[in->dst] = zt_or(tri[in->src1], tri[in->src2]);
        } else {
            tri[in->dst] = ZT_MIXED;            /* unanalyzable op */
        }
    }
    int res = tri[expr->out_reg];
    return (res == 0 || res == 1) ? res : -1;
}

/* Context for parallel full-vector expression evaluation.
 *
 * Two output modes:
 *   - Normal (sel_builders == NULL): each morsel's result is memcpy'd into
 *     out_data at its absolute offset (full materialized output vec).
 *   - Fused selection (sel_builders != NULL): out_type is RAY_BOOL and each
 *     morsel's bools are streamed via rowsel_emit_segment into THIS task's
 *     builder instead of a BOOL vec.  See exec_pred_to_selection.  Task
 *     boundaries are morsel/grain aligned, so the builder for an invocation
 *     is sel_builders[start / TASK_GRAIN] and the invocation's first global
 *     segment is start / RAY_MORSEL_ELEMS (subtracted to get builder-local
 *     seg indices). */
typedef struct {
    const ray_expr_t* expr;
    void*  out_data;
    int8_t out_type;
    rowsel_builder_t* sel_builders;    /* NULL = normal bool/vec output */
    uint32_t          n_sel_builders;  /* slot count (fused mode only)  */
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

    /* Chunk-zone skip is only possible for a BOOL predicate over a zone-
     * indexed column; compute the gate once, not per morsel. */
    bool zone = expr_zone_eligible(expr);

    /* Fused selection-output mode: resolve this task's builder + its first
     * global segment once.  Task boundaries are morsel-aligned because
     * TASK_GRAIN is a whole multiple of RAY_MORSEL_ELEMS (the serial path
     * passes start=0,end=nrows → slot 0). */
    rowsel_builder_t* sb = NULL;
    uint32_t local_base_seg = 0;
    if (c->sel_builders) {
        const int64_t grain = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
        _Static_assert((RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS) % RAY_MORSEL_ELEMS == 0,
                       "TASK_GRAIN must be a whole number of morsels");
        assert(start % grain == 0 &&
               "fused-sel: task start not grain/morsel aligned");
        uint32_t slot = (uint32_t)(start / grain);
        assert(slot < c->n_sel_builders && "fused-sel: builder slot OOB");
        sb = &c->sel_builders[slot];
        local_base_seg = (uint32_t)(start / RAY_MORSEL_ELEMS);
    }

    for (int64_t ms = start; ms < end; ms += EXPR_MORSEL) {
        int64_t me = (ms + EXPR_MORSEL < end) ? ms + EXPR_MORSEL : end;
        if (zone) {
            int d = expr_zone_decide(expr, ms, me);
            if (d >= 0) {  /* whole morsel decided from extrema — skip the read */
                if (sb) {
                    /* Emit a uniform ALL (d=1) / NONE (d=0) segment: memset a
                     * scratch bool buffer (unused on this branch — no eval ran)
                     * to d and let rowsel_emit_segment classify it. */
                    uint32_t lseg = (uint32_t)(ms / RAY_MORSEL_ELEMS) - local_base_seg;
                    memset(scratch[0], d, (size_t)(me - ms));
                    rowsel_emit_segment(sb, lseg, (const uint8_t*)scratch[0], me - ms);
                } else {
                    memset((char*)c->out_data + ms * esz, d, (size_t)(me - ms) * esz);
                }
                continue;
            }
        }
        void* result = expr_eval_morsel(expr, scratch, ms, me);
        if (sb) {
            /* Selection mode: emit this morsel's bools (or, on the rare
             * eval-NULL internal error, a none-pass segment so the builder's
             * strictly-ascending segment sequence stays gap-free). */
            uint32_t lseg = (uint32_t)(ms / RAY_MORSEL_ELEMS) - local_base_seg;
            if (!result) { memset(scratch[0], 0, (size_t)(me - ms)); result = scratch[0]; }
            rowsel_emit_segment(sb, lseg, (const uint8_t*)result, me - ms);
        } else if (result) {
            memcpy((char*)c->out_data + ms * esz, result, (size_t)(me - ms) * esz);
        }
    }
    scratch_free(scratch_hdr);
}

/* Post-pass for the fused unary path: |INT64_MIN| and -INT64_MIN don't fit in
 * i64 (signed-overflow; by convention this surfaces as typed null).  The
 * element-wise loop uses unsigned wrap, so any overflow position lands as
 * INT64_MIN in data.  Since INT64_MIN IS the canonical NULL_I64 sentinel,
 * the payload is already correct — we just flip HAS_NULLS via
 * ray_vec_set_null.  Caller must invoke single-threaded (after pool
 * dispatch joins). */
static void mark_i64_overflow_as_null(ray_t* result, int64_t off, int64_t len) {
    int64_t* d = (int64_t*)ray_data(result) + off;
    for (int64_t i = 0; i < len; i++) {
        if (RAY_UNLIKELY(d[i] == NULL_I64)) {
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

/* The fused binary path writes NULL_I64 for an i64 DIV/IDIV/MOD whose
 * divisor is zero (or the INT64_MIN/-1 overflow case) — null-model
 * invariant 16.4 requires HAS_NULLS set when that sentinel lands.  When the
 * output register is already marked `nullable` the conservative flag below
 * covers it; this detector handles the case where it is not, reusing the
 * mark_i64_overflow_as_null scan (which flips HAS_NULLS for any NULL_I64
 * lane).  Detect the shape from the last instruction. */
static bool expr_last_op_divmod_i64(const ray_expr_t* expr) {
    if (expr->out_type != RAY_I64 || expr->n_ins == 0) return false;
    const expr_ins_t* last = &expr->ins[expr->n_ins - 1];
    if (last->opcode != OP_DIV && last->opcode != OP_IDIV &&
        last->opcode != OP_MOD) return false;
    if (last->src2 == 0xFF) return false; /* binary only */
    if (expr->regs[last->dst].type != RAY_I64) return false;
    return true;
}

/* Single-null float model: the fused F64 kernels canonicalize any non-finite
 * result (overflow → ±Inf, div/mod-by-zero, sqrt(<0), log(≤0), exp(overflow))
 * to NULL_F64 in-buffer.  Detect when the last instruction is such an F64
 * producer so the caller runs the cheap post-scan that flips HAS_NULLS for any
 * 0Nf lane.  Used to set HAS_NULLS conservatively from the op shape (no
 * per-element scan — the scan is a full extra memory pass that regressed the
 * hot float kernels ~50%); see the call site.  Matches the fallback path's
 * shape-based flagging so VM ≡ fallback. */
static bool expr_last_op_produces_f64_null(const ray_expr_t* expr) {
    if (expr->out_type != RAY_F64 || expr->n_ins == 0) return false;
    const expr_ins_t* last = &expr->ins[expr->n_ins - 1];
    switch (last->opcode) {
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_DIV: case OP_IDIV: case OP_MOD:
        case OP_SQRT: case OP_LOG: case OP_EXP:
            return true;
        default:
            return false;  /* NEG/ABS/CEIL/FLOOR/ROUND/CAST/MIN2/MAX2: finite→finite */
    }
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
                /* SYM index width is PER SEGMENT (partitions saved at
                 * different vocabulary sizes legitimately differ) — the
                 * compile-time reg carries no width for parted scans. */
                seg_expr.regs[r].col_attrs = segs[s]->attrs;
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
    if (expr_last_op_overflows_i64(expr) || expr_last_op_divmod_i64(expr))
        mark_i64_overflow_as_null(out, 0, nrows);
    /* Single-null float model: flip HAS_NULLS PRECISELY if an F64 producer
     * canonicalized a non-finite result to NULL_F64 in-buffer.  Scan-based (not
     * conservative-by-shape) so a pure-finite result keeps HAS_NULLS unset —
     * critical because this fused output is often an input to the NEXT op /
     * aggregate, and a spurious HAS_NULLS would force that consumer onto the
     * slow null-aware path (measured: conservative flagging regressed chained
     * float kernels catastrophically by poisoning inputs).  This output is
     * produced once (post-join of all morsels) so the single pass here is not
     * the per-op hot loop; mirrors the i64-overflow mark above. */
    if (expr_last_op_produces_f64_null(expr))
        mark_f64_nonfinite_as_null(out, 0, nrows);
    /* Conservative "may contain nulls" — REQUIRED, not cosmetic: group.c
     * feeds this vec to aggregates whose check-free fast path is gated on
     * the attr; a missing attr with sentinel lanes = wrong aggregates. */
    if (expr->regs[expr->out_reg].nullable)
        out->attrs |= RAY_ATTR_HAS_NULLS;
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

    if (expr_last_op_overflows_i64(expr) || expr_last_op_divmod_i64(expr))
        mark_i64_overflow_as_null(out, 0, nrows);
    /* Single-null float model: flip HAS_NULLS PRECISELY if an F64 producer
     * canonicalized a non-finite result to NULL_F64 in-buffer.  Scan-based (not
     * conservative-by-shape) so a pure-finite result keeps HAS_NULLS unset —
     * critical because this fused output is often an input to the NEXT op /
     * aggregate, and a spurious HAS_NULLS would force that consumer onto the
     * slow null-aware path (measured: conservative flagging regressed chained
     * float kernels catastrophically by poisoning inputs).  This output is
     * produced once (post-join of all morsels) so the single pass here is not
     * the per-op hot loop; mirrors the i64-overflow mark above. */
    if (expr_last_op_produces_f64_null(expr))
        mark_f64_nonfinite_as_null(out, 0, nrows);
    /* Conservative "may contain nulls" — REQUIRED, not cosmetic: group.c
     * feeds this vec to aggregates whose check-free fast path is gated on
     * the attr; a missing attr with sentinel lanes = wrong aggregates. */
    if (expr->regs[expr->out_reg].nullable)
        out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* ============================================================================
 * Fused predicate → row selection
 *
 * Instead of materializing a whole-table BOOL vec (expr_eval_full) and then
 * scanning it with ray_rowsel_from_pred, stream each morsel's bools directly
 * into per-task rowsel builders and stitch them into a single selection.
 * Saves the BOOL allocation + the second full pass.  Strictly additive:
 * unsupported shapes return to the existing path.
 *
 * The per-task-builder scaffolding (allocate one builder per dispatched
 * grain task, dispatch a worker that streams each morsel's bools via
 * rowsel_emit_segment, stitch + apply the all-pass→NULL convention) is shared
 * by two producers: the expr-tree path (expr_full_fn, selection mode) and the
 * standalone predicate kernels (pred_sel_drive, used by exec_in_to_selection).
 * pred_sel_builders_alloc / _finish factor that scaffolding so both producers
 * stay byte-identical to ray_rowsel_from_pred over the same bools.
 * ============================================================================ */

/* Allocate + init one rowsel builder per dispatched grain task (parallel) or a
 * single builder over all segments (serial).  ray_pool_dispatch carves task i
 * as [i*grain, min((i+1)*grain, nrows)); grain is morsel-aligned so each task
 * owns a contiguous run of whole segments.  On return *parallel and *n_builders
 * describe the dispatch geometry.  Returns NULL when the table needs more tasks
 * than the ring holds (pool clamps n_tasks to RAY_POOL_MAX_TASKS and re-derives
 * a non-morsel-aligned grain, which would trip the worker's `start % grain == 0`
 * assert) or on OOM — caller falls back to the bool path. */
static rowsel_builder_t* pred_sel_builders_alloc(int64_t nrows, ray_pool_t* pool,
                                                 bool* parallel,
                                                 uint32_t* n_builders_out) {
    const int64_t grain  = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
    int64_t       n_segs = (nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    bool          par    = (pool && nrows >= RAY_PARALLEL_THRESHOLD);

    uint32_t n_builders = par ? (uint32_t)((nrows + grain - 1) / grain) : 1u;
    if (par && n_builders > RAY_POOL_MAX_TASKS) return NULL;

    rowsel_builder_t* builders =
        (rowsel_builder_t*)ray_alloc_raw((size_t)n_builders * sizeof(rowsel_builder_t));
    if (!builders) return NULL;

    if (par) {
        for (uint32_t i = 0; i < n_builders; i++) {
            int64_t s = (int64_t)i * grain;
            int64_t e = s + grain;
            if (e > nrows) e = nrows;
            uint32_t segs = (uint32_t)((e - s + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
            rowsel_builder_init(&builders[i], segs);
        }
    } else {
        rowsel_builder_init(&builders[0], (uint32_t)n_segs);
    }

    *parallel       = par;
    *n_builders_out = n_builders;
    return builders;
}

/* Stitch builders (global segment order = slot order) into one rowsel block,
 * free the builder array, and apply the all-pass→NULL convention (a selection
 * that keeps every row is "no selection", matching ray_rowsel_from_pred and
 * g->selection == NULL).  Returns NULL+*all_pass=1 for all-pass, NULL+*all_pass
 * unchanged on OOM, else the block. */
static ray_t* pred_sel_builders_finish(rowsel_builder_t* builders,
                                       uint32_t n_builders, int64_t nrows,
                                       bool* all_pass) {
    ray_t* block = rowsel_builder_finish(builders, n_builders, nrows);
    ray_free_raw(builders);
    if (!block) return NULL;  /* OOM — caller falls back to the bool path */

    ray_rowsel_t* m = ray_rowsel_meta(block);
    if (m->total_pass == nrows) {
        ray_rowsel_release(block);
        *all_pass = true;
        return NULL;
    }
    return block;
}

/* Generic selection-output driver for standalone predicate kernels.  Each
 * morsel [ms, me) is filled into a morsel-local bool scratch by `fill` (which
 * runs the kernel's element logic over global rows [ms, me)) then streamed via
 * rowsel_emit_segment — the same per-task-builder model as expr_full_fn's
 * selection mode, only the per-morsel COMPUTE differs.  Returns the same
 * tri-state as exec_pred_to_selection. */
typedef struct {
    pred_sel_fill_fn  fill;
    void*             fill_ctx;
    rowsel_builder_t* sel_builders;
    uint32_t          n_sel_builders;
} pred_sel_drive_ctx_t;

static void pred_sel_worker(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    pred_sel_drive_ctx_t* c = (pred_sel_drive_ctx_t*)vctx;

    ray_t* scratch_hdr = NULL;
    uint8_t* scratch = (uint8_t*)scratch_alloc(&scratch_hdr, RAY_MORSEL_ELEMS);
    if (!scratch) return;

    const int64_t grain = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
    _Static_assert((RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS) % RAY_MORSEL_ELEMS == 0,
                   "TASK_GRAIN must be a whole number of morsels");
    assert(start % grain == 0 && "pred-sel: task start not grain/morsel aligned");
    uint32_t slot = (uint32_t)(start / grain);
    assert(slot < c->n_sel_builders && "pred-sel: builder slot OOB");
    rowsel_builder_t* sb = &c->sel_builders[slot];
    uint32_t local_base_seg = (uint32_t)(start / RAY_MORSEL_ELEMS);

    for (int64_t ms = start; ms < end; ms += RAY_MORSEL_ELEMS) {
        int64_t me = (ms + RAY_MORSEL_ELEMS < end) ? ms + RAY_MORSEL_ELEMS : end;
        c->fill(c->fill_ctx, ms, me - ms, scratch);
        uint32_t lseg = (uint32_t)(ms / RAY_MORSEL_ELEMS) - local_base_seg;
        rowsel_emit_segment(sb, lseg, scratch, me - ms);
    }
    scratch_free(scratch_hdr);
}

ray_t* pred_sel_drive(int64_t nrows, pred_sel_fill_fn fill, void* fill_ctx,
                      bool* all_pass) {
    *all_pass = false;
    if (nrows <= 0) { *all_pass = true; return NULL; }

    ray_pool_t* pool = ray_pool_get();
    bool        parallel;
    uint32_t    n_builders;
    rowsel_builder_t* builders =
        pred_sel_builders_alloc(nrows, pool, &parallel, &n_builders);
    if (!builders) return NULL;

    pred_sel_drive_ctx_t ctx = {
        .fill           = fill,
        .fill_ctx       = fill_ctx,
        .sel_builders   = builders,
        .n_sel_builders = n_builders,
    };
    if (parallel)
        ray_pool_dispatch(pool, pred_sel_worker, &ctx, nrows);
    else
        pred_sel_worker(&ctx, 0, 0, nrows);

    return pred_sel_builders_finish(builders, n_builders, nrows, all_pass);
}

ray_t* exec_pred_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows,
                              bool* all_pass) {
    *all_pass = false;
    if (!g || !g->table || !pred) return NULL;          /* unsupported */
    if (nrows <= 0) { *all_pass = true; return NULL; }  /* empty = all-pass */

    /* Standalone predicate kernels that aren't expr-compilable but have a
     * selection-output worker.  `in`/`not-in` over a flat column + literal set
     * stream membership bools straight into the rowsel.  (`like` stays on the
     * bool fallback: its 4 input-shape paths + dict-LUT pipeline don't reduce
     * to a clean per-morsel fill.  `between` desugars to >=/<= AND and so
     * already rides the expr path below.) */
    if (pred->opcode == OP_IN || pred->opcode == OP_NOT_IN)
        return exec_in_to_selection(g, pred, nrows, all_pass);

    ray_expr_t ex;
    if (!expr_compile(g, g->table, pred, &ex)) return NULL;
    if (ex.out_type != RAY_BOOL || ex.has_parted) return NULL;

    ray_pool_t* pool     = ray_pool_get();
    bool        parallel;
    uint32_t    n_builders;
    rowsel_builder_t* builders =
        pred_sel_builders_alloc(nrows, pool, &parallel, &n_builders);
    if (!builders) return NULL;

    expr_full_ctx_t ctx = {
        .expr           = &ex,
        .out_data       = NULL,
        .out_type       = ex.out_type,
        .sel_builders   = builders,
        .n_sel_builders = n_builders,
    };
    if (parallel)
        ray_pool_dispatch(pool, expr_full_fn, &ctx, nrows);
    else
        expr_full_fn(&ctx, 0, 0, nrows);

    return pred_sel_builders_finish(builders, n_builders, nrows, all_pass);
}

/* ============================================================================
 * Null bitmap propagation for element-wise ops
 * ============================================================================ */

/* Propagate nulls from src into dst element-wise.  ray_vec_set_null
 * writes the type-correct sentinel, and ray_vec_is_null reads it back —
 * the per-element walk is required since there is no per-row bitmap to
 * bulk-OR. */
static void propagate_nulls(ray_t* src, ray_t* dst, int64_t len) {
    if (!(src->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE))) return;
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

    /* One-sided null fast path: only one side has nulls (the common
     * shape — vec col vs non-null scalar) and no scalar is null.  Scan
     * src elements via ray_vec_is_null (sentinel-based), set the
     * comparison's fill value per null cell. */
    if (!ln_s && !rn_s && (l_has ^ r_has)) {
        ray_t* src = l_has ? lhs : rhs;
        bool   src_left = l_has;
        uint8_t left_bits  = (opcode == OP_LT || opcode == OP_LE || opcode == OP_NE);
        uint8_t right_bits = (opcode == OP_GT || opcode == OP_GE || opcode == OP_NE);
        uint8_t fill = src_left ? left_bits : right_bits;
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null(src, i)) dst[i] = fill;
        }
        return;
    }

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

/* Set all elements in result as null (scalar null broadcast).
 * Writes the type-correct sentinel into every payload slot and sets
 * HAS_NULLS. */
static void set_all_null(ray_t* result, int64_t len) {
    result->attrs |= RAY_ATTR_HAS_NULLS;
    /* Sentinel payload fill — the sole source of truth. */
    switch (result->type) {
        case RAY_F64: {
            double* d = (double*)ray_data(result);
            for (int64_t i = 0; i < len; i++) d[i] = NULL_F64;
            break;
        }
        case RAY_I64: case RAY_TIMESTAMP: {
            int64_t* d = (int64_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++) d[i] = NULL_I64;
            break;
        }
        case RAY_F32: {
            float* d = (float*)ray_data(result);
            for (int64_t i = 0; i < len; i++) d[i] = NULL_F32;
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            int32_t* d = (int32_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++) d[i] = NULL_I32;
            break;
        }
        case RAY_I16: {
            int16_t* d = (int16_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++) d[i] = NULL_I16;
            break;
        }
        case RAY_STR: {
            ray_str_t* s = (ray_str_t*)ray_data(result);
            memset(s, 0, (size_t)len * sizeof(ray_str_t));
            break;
        }
        case RAY_GUID:
            memset(ray_data(result), 0, (size_t)len * 16);
            break;
        default: break;
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

/* Graph type-inference stamps elementwise nodes with the scanned
 * column's raw STORAGE tag — RAY_PARTED_* / RAY_MAPCOMMON propagate
 * verbatim through NEG/ABS/... and promote().  At exec time OP_SCAN
 * always hands the kernels a flat vector (segment tables in streaming
 * mode, the flatten cold path otherwise), so a storage-tagged out_type
 * would make ray_vec_new reject the allocation ("type" error).
 * Normalize to the runtime element type: parted → its base type,
 * mapcommon → the materialized operand's type. */
static inline int8_t ew_runtime_out_type(int8_t out_type, int8_t operand_type) {
    if (RAY_IS_PARTED(out_type)) return (int8_t)RAY_PARTED_BASETYPE(out_type);
    if (out_type == RAY_MAPCOMMON) return operand_type;
    return out_type;
}

ray_t* exec_elementwise_unary(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g;
    if (!input || RAY_IS_ERR(input)) return input;
    if (!ray_is_vec(input)) return ray_error("type", "expr eval: unary op expects a vector, got %s", ray_type_name(input->type));
    int64_t len = input->len;
    int8_t in_type = input->type;
    int8_t out_type = ew_runtime_out_type(op->out_type, in_type);

    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = len;

    /* Hoist in_type, out_type, and opcode dispatch entirely outside the
     * morsel loop so each inner loop is a tight, single-type kernel.
     * This allows autovectorisation and eliminates per-element branch predict slots. */
    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t out_off = 0;
    uint16_t opc = op->opcode;

    if (in_type == RAY_F64 && out_type == RAY_F64) {
        while (ray_morsel_next(&m)) {
            int64_t n = m.morsel_len;
            double* src = (double*)m.morsel_ptr;
            double* dst = (double*)ray_data(result) + out_off;
            switch (opc) {
                /* Single-null float model: SQRT/LOG/EXP can map finite→non-finite
                 * → canonicalize to NULL_F64 (HAS_NULLS via post-scan below).
                 * NEG/ABS/CEIL/FLOOR/ROUND of finite stay finite. */
                case OP_NEG:   for (int64_t i=0;i<n;i++) dst[i] = -src[i]; break;
                case OP_ABS:   for (int64_t i=0;i<n;i++) dst[i] = fabs(src[i]); break;
                case OP_SQRT:  for (int64_t i=0;i<n;i++) dst[i] = ray_f64_fin(sqrt(src[i])); break;
                case OP_LOG:   for (int64_t i=0;i<n;i++) dst[i] = ray_f64_fin(log(src[i])); break;
                case OP_EXP:   for (int64_t i=0;i<n;i++) dst[i] = ray_f64_fin(exp(src[i])); break;
                case OP_CEIL:  for (int64_t i=0;i<n;i++) dst[i] = ceil(src[i]); break;
                case OP_FLOOR: for (int64_t i=0;i<n;i++) dst[i] = floor(src[i]); break;
                case OP_ROUND: for (int64_t i=0;i<n;i++) dst[i] = round(src[i]); break;
                default:       for (int64_t i=0;i<n;i++) dst[i] = src[i]; break;
            }
            out_off += n;
        }
    } else if (in_type == RAY_F64 && out_type == RAY_I64) {
        while (ray_morsel_next(&m)) {
            int64_t n = m.morsel_len;
            double* src = (double*)m.morsel_ptr;
            int64_t* dst = (int64_t*)((char*)ray_data(result) + out_off * sizeof(int64_t));
            switch (opc) {
                case OP_NEG:   for (int64_t i=0;i<n;i++) dst[i] = (int64_t)(-src[i]); break;
                case OP_ABS:   for (int64_t i=0;i<n;i++) dst[i] = (int64_t)fabs(src[i]); break;
                case OP_SQRT:  for (int64_t i=0;i<n;i++) dst[i] = (int64_t)sqrt(src[i]); break;
                case OP_LOG:   for (int64_t i=0;i<n;i++) dst[i] = (int64_t)log(src[i]); break;
                case OP_EXP:   for (int64_t i=0;i<n;i++) dst[i] = (int64_t)exp(src[i]); break;
                case OP_CEIL:  for (int64_t i=0;i<n;i++) dst[i] = (int64_t)ceil(src[i]); break;
                case OP_FLOOR: for (int64_t i=0;i<n;i++) dst[i] = (int64_t)floor(src[i]); break;
                case OP_ROUND: for (int64_t i=0;i<n;i++) dst[i] = (int64_t)round(src[i]); break;
                default:       for (int64_t i=0;i<n;i++) dst[i] = (int64_t)src[i]; break;
            }
            out_off += n;
        }
    } else if (in_type == RAY_I64 && out_type == RAY_I64) {
        while (ray_morsel_next(&m)) {
            int64_t n = m.morsel_len;
            int64_t* src = (int64_t*)m.morsel_ptr;
            int64_t* dst = (int64_t*)((char*)ray_data(result) + out_off * sizeof(int64_t));
            switch (opc) {
                /* Unsigned arithmetic avoids UB on INT64_MIN */
                case OP_NEG: for (int64_t i=0;i<n;i++) dst[i]=(int64_t)(-(uint64_t)src[i]); break;
                case OP_ABS: for (int64_t i=0;i<n;i++) dst[i]=src[i]<0?(int64_t)(-(uint64_t)src[i]):src[i]; break;
                default:     for (int64_t i=0;i<n;i++) dst[i]=src[i]; break;
            }
            out_off += n;
        }
    } else if (in_type == RAY_I64 && out_type == RAY_F64) {
        while (ray_morsel_next(&m)) {
            int64_t n = m.morsel_len;
            int64_t* src = (int64_t*)m.morsel_ptr;
            double* dst = (double*)ray_data(result) + out_off;
            switch (opc) {
                case OP_NEG:  for (int64_t i=0;i<n;i++) dst[i]=-(double)src[i]; break;
                case OP_SQRT: for (int64_t i=0;i<n;i++) dst[i]=ray_f64_fin(sqrt((double)src[i])); break;
                case OP_LOG:  for (int64_t i=0;i<n;i++) dst[i]=ray_f64_fin(log((double)src[i])); break;
                case OP_EXP:  for (int64_t i=0;i<n;i++) dst[i]=ray_f64_fin(exp((double)src[i])); break;
                default:      for (int64_t i=0;i<n;i++) dst[i]=(double)src[i]; break;
            }
            out_off += n;
        }
    } else if (in_type == RAY_I64 && out_type == RAY_BOOL) {
        if (opc == OP_ISNULL) {
            /* ISNULL over a non-null vec: always false here; the
             * null-propagation pass at the end of the function sets
             * dst[i]=1 for null rows of the input. */
            while (ray_morsel_next(&m)) {
                int64_t n = m.morsel_len;
                uint8_t* dst = (uint8_t*)((char*)ray_data(result) + out_off);
                for (int64_t i = 0; i < n; i++) dst[i] = 0;
                out_off += n;
            }
        } else if (opc == OP_CAST) {
            /* (as 'BOOL i64_col) — truthy semantics; NULL_I64 = INT64_MIN
             * sentinel is non-zero but logically missing, so skip it. */
            while (ray_morsel_next(&m)) {
                int64_t n = m.morsel_len;
                int64_t* src = (int64_t*)m.morsel_ptr;
                uint8_t* dst = (uint8_t*)((char*)ray_data(result) + out_off);
                for (int64_t i = 0; i < n; i++)
                    dst[i] = (src[i] != 0 && src[i] != NULL_I64) ? 1 : 0;
                out_off += n;
            }
        }
    } else if (in_type == RAY_BOOL && opc == OP_NOT) {
        while (ray_morsel_next(&m)) {
            int64_t n = m.morsel_len;
            uint8_t* src = (uint8_t*)m.morsel_ptr;
            uint8_t* dst = (uint8_t*)((char*)ray_data(result) + out_off);
            for (int64_t i = 0; i < n; i++) dst[i] = !src[i];
            out_off += n;
        }
    } else if (opc == OP_CAST) {
        /* CAST from narrow integer types (I32/I16/U8/BOOL) to I64/F64.
         * in_type is loop-invariant; select the typed read outside the loop. */
        if (in_type == RAY_I32 || in_type == RAY_DATE || in_type == RAY_TIME) {
            if (out_type == RAY_I64) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int32_t* src = (int32_t*)m.morsel_ptr;
                    int64_t* dst = (int64_t*)((char*)ray_data(result) + out_off * sizeof(int64_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)src[i];
                    out_off += n;
                }
            } else {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int32_t* src = (int32_t*)m.morsel_ptr;
                    double* dst = (double*)ray_data(result) + out_off;
                    for (int64_t i = 0; i < n; i++) dst[i] = (double)src[i];
                    out_off += n;
                }
            }
        } else if (in_type == RAY_I16) {
            if (out_type == RAY_I64) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int16_t* src = (int16_t*)m.morsel_ptr;
                    int64_t* dst = (int64_t*)((char*)ray_data(result) + out_off * sizeof(int64_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)src[i];
                    out_off += n;
                }
            } else {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int16_t* src = (int16_t*)m.morsel_ptr;
                    double* dst = (double*)ray_data(result) + out_off;
                    for (int64_t i = 0; i < n; i++) dst[i] = (double)src[i];
                    out_off += n;
                }
            }
        } else if (in_type == RAY_U8 || in_type == RAY_BOOL) {
            if (out_type == RAY_I64) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    uint8_t* src = (uint8_t*)m.morsel_ptr;
                    int64_t* dst = (int64_t*)((char*)ray_data(result) + out_off * sizeof(int64_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)src[i];
                    out_off += n;
                }
            } else {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    uint8_t* src = (uint8_t*)m.morsel_ptr;
                    double* dst = (double*)ray_data(result) + out_off;
                    for (int64_t i = 0; i < n; i++) dst[i] = (double)src[i];
                    out_off += n;
                }
            }
        } else if (in_type == RAY_I64) {
            /* Narrowing I64 → I32/I16/U8/BOOL: truncate. */
            if (out_type == RAY_I32) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int64_t* src = (int64_t*)m.morsel_ptr;
                    int32_t* dst = (int32_t*)((char*)ray_data(result) + out_off * sizeof(int32_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int32_t)src[i];
                    out_off += n;
                }
            } else if (out_type == RAY_I16) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int64_t* src = (int64_t*)m.morsel_ptr;
                    int16_t* dst = (int16_t*)((char*)ray_data(result) + out_off * sizeof(int16_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int16_t)src[i];
                    out_off += n;
                }
            } else if (out_type == RAY_U8 || out_type == RAY_BOOL) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    int64_t* src = (int64_t*)m.morsel_ptr;
                    uint8_t* dst = (uint8_t*)((char*)ray_data(result) + out_off);
                    /* BOOL: collapse non-zero to 1; U8: low byte. */
                    if (out_type == RAY_BOOL)
                        for (int64_t i = 0; i < n; i++) dst[i] = src[i] ? 1 : 0;
                    else
                        for (int64_t i = 0; i < n; i++) dst[i] = (uint8_t)src[i];
                    out_off += n;
                }
            }
        } else if (in_type == RAY_F64) {
            /* Narrowing F64 → I32/I16/U8/BOOL: float truncation. */
            if (out_type == RAY_I32) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    double* src = (double*)m.morsel_ptr;
                    int32_t* dst = (int32_t*)((char*)ray_data(result) + out_off * sizeof(int32_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int32_t)src[i];
                    out_off += n;
                }
            } else if (out_type == RAY_I16) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    double* src = (double*)m.morsel_ptr;
                    int16_t* dst = (int16_t*)((char*)ray_data(result) + out_off * sizeof(int16_t));
                    for (int64_t i = 0; i < n; i++) dst[i] = (int16_t)src[i];
                    out_off += n;
                }
            } else if (out_type == RAY_U8 || out_type == RAY_BOOL) {
                while (ray_morsel_next(&m)) {
                    int64_t n = m.morsel_len;
                    double* src = (double*)m.morsel_ptr;
                    uint8_t* dst = (uint8_t*)((char*)ray_data(result) + out_off);
                    if (out_type == RAY_BOOL)
                        /* NaN (NULL_F64 sentinel) is "missing"; IEEE
                         * `NaN != 0.0` is true so add an explicit
                         * `src[i] == src[i]` to filter NaN to false. */
                        for (int64_t i = 0; i < n; i++)
                            dst[i] = (src[i] != 0.0 && src[i] == src[i]) ? 1 : 0;
                    else
                        for (int64_t i = 0; i < n; i++) dst[i] = (uint8_t)src[i];
                    out_off += n;
                }
            }
        }
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
     * as typed null (by convention).  Loop above used unsigned wrap, so
     * overflow positions land as INT64_MIN in data; convert them to null. */
    if (out_type == RAY_I64 && in_type == RAY_I64 &&
        (op->opcode == OP_NEG || op->opcode == OP_ABS))
        mark_i64_overflow_as_null(result, 0, len);

    /* Single-null float model: SQRT/LOG/EXP can map a finite input to a
     * non-finite result, canonicalized to NULL_F64 in-buffer.  Flip HAS_NULLS
     * PRECISELY (scan only when the shape can produce a 0Nf) so a pure-finite
     * result doesn't poison downstream consumers; precise also matches the
     * fused path (VM ≡ fallback).  These transcendental ops are not the hot
     * add/sub/mul/div perf-gate kernels, so the single pass is acceptable. */
    if (out_type == RAY_F64 &&
        (op->opcode == OP_SQRT || op->opcode == OP_LOG || op->opcode == OP_EXP))
        mark_f64_nonfinite_as_null(result, 0, len);

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

    /* Fast path: EQ/NE against the empty-string constant — a string equals ""
     * iff len==0.  Evaluate as a length check; no pointer deref or memcmp.
     * Results are byte-identical to the general STR_CMP_LOOP path. */
    if ((opc == OP_EQ || opc == OP_NE) &&
        ((l_scalar && !r_scalar && l_scalar_elem.len == 0) ||
         (r_scalar && !l_scalar && r_scalar_elem.len == 0))) {
        const ray_str_t* vec = l_scalar ? r_elems : l_elems;
        bool is_eq = (opc == OP_EQ);
        for (int64_t i = 0; i < n; i++)
            dst[i] = (uint8_t)((vec[i].len == 0) == is_eq);
        return;
    }

    /* Resolve final element pointers and pool pointers outside the loop.
     * For scalar sides the element pointer is fixed; for vector sides it
     * advances with i.  Use step_l / step_r (0 or 1) to avoid per-element
     * ternaries on l_scalar / r_scalar. */
    const ray_str_t* la = l_scalar ? l_elems            : l_elems;   /* base */
    const ray_str_t* lb = r_scalar ? r_elems            : r_elems;
    const char*      pa = l_scalar ? l_scalar_pool      : l_pool;
    const char*      pb = r_scalar ? r_scalar_pool      : r_pool;
    int step_l = l_scalar ? 0 : 1;
    int step_r = r_scalar ? 0 : 1;

    /* Hoist opc outside the loop — one tight loop per comparison opcode. */
#define STR_CMP_LOOP(expr)                                          \
    do {                                                            \
        for (int64_t i = 0; i < n; i++) {                          \
            const ray_str_t* a = la + i * step_l;                  \
            const ray_str_t* b = lb + i * step_r;                  \
            dst[i] = (uint8_t)(expr);                              \
        }                                                           \
    } while (0)

    switch (opc) {
        case OP_EQ: STR_CMP_LOOP(ray_str_t_eq(a, pa, b, pb));       break;
        case OP_NE: STR_CMP_LOOP(!ray_str_t_eq(a, pa, b, pb));      break;
        case OP_LT: STR_CMP_LOOP(ray_str_t_cmp(a, pa, b, pb) < 0);  break;
        case OP_LE: STR_CMP_LOOP(ray_str_t_cmp(a, pa, b, pb) <= 0); break;
        case OP_GT: STR_CMP_LOOP(ray_str_t_cmp(a, pa, b, pb) > 0);  break;
        case OP_GE: STR_CMP_LOOP(ray_str_t_cmp(a, pa, b, pb) >= 0); break;
        default: for (int64_t i = 0; i < n; i++) dst[i] = 0; break;
    }
#undef STR_CMP_LOOP
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
/* rdata is the result's data pointer, resolved ONCE by the caller on the
 * dispatching thread.  Workers must not call ray_data(result) themselves:
 * that reads result->attrs to test the slice bit, which races (per TSan)
 * with the relaxed-atomic OR of HAS_NULLS other workers perform below.
 * The pointer is stable for the whole op, so resolving it up front is
 * both correct and one fewer branch per morsel. */
static void binary_range(ray_op_t* op, int8_t out_type,
                         ray_t* lhs, ray_t* rhs, ray_t* result, void* rdata,
                         bool l_scalar, bool r_scalar,
                         double l_f64, double r_f64,
                         int64_t l_i64, int64_t r_i64,
                         int64_t start, int64_t end) {
    uint8_t out_esz = ray_elem_size(out_type);
    void* dst = (char*)rdata + start * out_esz;
    int64_t n = end - start;

    /* Fast path: integer-family column vs integer scalar → bool comparison.
     * The generic LV_READ/RV_READ macro chain below converts every operand
     * to double then back to int64, killing autovectorisation and burning
     * cycles on SYM-column-vs-resolved-sym-id comparisons (Q11's
     * (!= MobilePhoneModel "") = 15 ms vs ~1 ms on the typed-pointer
     * path).  Specialise on column width here so the inner loop reduces
     * to a typed pointer dereference + compare + store. */
    if (out_type == RAY_BOOL && !l_scalar && r_scalar &&
        (op->opcode == OP_EQ || op->opcode == OP_NE ||
         op->opcode == OP_LT || op->opcode == OP_LE ||
         op->opcode == OP_GT || op->opcode == OP_GE) &&
        (lhs->type == RAY_I64 || lhs->type == RAY_TIMESTAMP ||
         lhs->type == RAY_I32 || lhs->type == RAY_DATE   ||
         lhs->type == RAY_TIME || lhs->type == RAY_I16   ||
         lhs->type == RAY_BOOL || lhs->type == RAY_U8    ||
         RAY_IS_SYM(lhs->type)))
    {
        int64_t l_off = start;
        void* l_data = resolve_vec_data(lhs, &l_off);
        uint8_t l_esz = ray_sym_elem_size(lhs->type, lhs->attrs);
        const uint8_t* lbase = (const uint8_t*)l_data + l_off * l_esz;
        uint8_t* odst = (uint8_t*)dst;
        int64_t cv = r_i64;
        uint16_t opc = op->opcode;

        #define BR_FAST(T, READ_T)                                              \
            do {                                                                \
                const T* d = (const T*)lbase;                                   \
                T cvt = (T)cv;                                                  \
                switch (opc) {                                                  \
                case OP_EQ: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T == cvt); break; \
                case OP_NE: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T != cvt); break; \
                case OP_LT: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T <  cvt); break; \
                case OP_LE: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T <= cvt); break; \
                case OP_GT: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T >  cvt); break; \
                case OP_GE: for (int64_t i = 0; i < n; i++) odst[i] = (READ_T >= cvt); break; \
                }                                                               \
            } while (0)

        if (l_esz == 8) {
            if (RAY_IS_SYM(lhs->type)) {
                BR_FAST(int64_t, d[i]);
            } else if (lhs->type == RAY_I64 || lhs->type == RAY_TIMESTAMP) {
                BR_FAST(int64_t, d[i]);
            }
            return;
        } else if (l_esz == 4) {
            /* SYM W32 stores unsigned IDs; for EQ/NE the unsigned compare
             * gives the same result as the signed compare against r_i64
             * (truncated to 32-bit).  For ordering ops we keep the signed
             * compare (matches the previous generic-path semantics for
             * I32/DATE/TIME). */
            if (RAY_IS_SYM(lhs->type)) {
                if (opc == OP_EQ || opc == OP_NE) {
                    const uint32_t* d = (const uint32_t*)lbase;
                    uint32_t cvu = (uint32_t)cv;
                    if (opc == OP_EQ)
                        for (int64_t i = 0; i < n; i++) odst[i] = (d[i] == cvu);
                    else
                        for (int64_t i = 0; i < n; i++) odst[i] = (d[i] != cvu);
                    return;
                }
                BR_FAST(uint32_t, (int64_t)d[i]);
            } else {
                BR_FAST(int32_t, (int64_t)d[i]);
            }
            return;
        } else if (l_esz == 2) {
            if (RAY_IS_SYM(lhs->type)) {
                BR_FAST(uint16_t, (int64_t)d[i]);
            } else {
                BR_FAST(int16_t, (int64_t)d[i]);
            }
            return;
        } else if (l_esz == 1) {
            BR_FAST(uint8_t, (int64_t)d[i]);
            return;
        }
        #undef BR_FAST
    }

    /* Fast path: integer-vec vs integer-scalar arithmetic with
     * matching output type (e.g. (- ClientIP 1) → I32 from I32 col).
     * Same motivation as the BOOL fast path: the LV_READ macro chain
     * downcasts to double then back, killing autovec.  Specialise on
     * width here so the inner loop reduces to typed pointer
     * dereferences.  Only safe when lhs type == out type so no
     * narrowing or promotion is needed.  Hits Q36's `(- col const)`
     * derived columns hard (3 of them × 5 M rows). */
    if (!l_scalar && r_scalar &&
        (op->opcode == OP_ADD || op->opcode == OP_SUB ||
         op->opcode == OP_MUL || op->opcode == OP_MIN2 ||
         op->opcode == OP_MAX2) &&
        lhs->type == out_type &&
        (out_type == RAY_I64 || out_type == RAY_TIMESTAMP ||
         out_type == RAY_I32 || out_type == RAY_DATE || out_type == RAY_TIME ||
         out_type == RAY_I16))
    {
        int64_t l_off = start;
        void* l_data = resolve_vec_data(lhs, &l_off);
        uint8_t l_esz = ray_sym_elem_size(lhs->type, lhs->attrs);
        const uint8_t* lbase = (const uint8_t*)l_data + l_off * l_esz;
        int64_t cv = r_i64;
        uint16_t opc = op->opcode;

        #define BR_AR_FAST(T) do {                                          \
            const T* d = (const T*)lbase;                                   \
            T* odst = (T*)dst;                                              \
            T cvt = (T)cv;                                                  \
            switch (opc) {                                                  \
            case OP_ADD:  for (int64_t i = 0; i < n; i++) odst[i] = (T)((uint64_t)d[i] + (uint64_t)cvt); break; \
            case OP_SUB:  for (int64_t i = 0; i < n; i++) odst[i] = (T)((uint64_t)d[i] - (uint64_t)cvt); break; \
            case OP_MUL:  for (int64_t i = 0; i < n; i++) odst[i] = (T)((uint64_t)d[i] * (uint64_t)cvt); break; \
            case OP_MIN2: for (int64_t i = 0; i < n; i++) odst[i] = d[i] < cvt ? d[i] : cvt; break; \
            case OP_MAX2: for (int64_t i = 0; i < n; i++) odst[i] = d[i] > cvt ? d[i] : cvt; break; \
            }                                                               \
        } while (0)

        if (l_esz == 8) { BR_AR_FAST(int64_t); return; }
        if (l_esz == 4) { BR_AR_FAST(int32_t); return; }
        if (l_esz == 2) { BR_AR_FAST(int16_t); return; }
        #undef BR_AR_FAST
    }


    /* Pointers into source data at offset start */
    double* lp_f64 = NULL; int64_t* lp_i64 = NULL; uint8_t* lp_bool = NULL;
    double* rp_f64 = NULL; int64_t* rp_i64 = NULL; uint8_t* rp_bool = NULL;

    int32_t* lp_i32 = NULL; uint32_t* lp_u32 = NULL; int16_t* lp_i16 = NULL;
    int32_t* rp_i32 = NULL; uint32_t* rp_u32 = NULL; int16_t* rp_i16 = NULL;

    /* SYM index scratch — needed ONLY by narrow-width (W8/W16) or cross-domain
     * SYM columns.  Allocated lazily on the heap (freed at `done:`) rather than
     * as a stack VLA: the pool can hand a worker a morsel far larger than a page
     * (grain is total/task_cap once the ring saturates), so an n-sized stack
     * array would overflow the worker stack.  Non-SYM ops (the common case)
     * allocate nothing. */
    int64_t* lsym_buf = NULL;
    int64_t* rsym_buf = NULL;
    /* sym-domain Phase 2: SYM column vs SYM column across DIFFERENT
     * domains cannot compare raw indices — re-express the rhs cells in
     * the lhs's domain via the buffer path (absent → -1: equals no lhs
     * id).  Same-domain columns (the only pre-flip case) keep the
     * direct typed-pointer fast paths — byte-identical. */
    bool sym_xlate = !l_scalar && !r_scalar &&
                     RAY_IS_SYM(lhs->type) && RAY_IS_SYM(rhs->type) &&
                     ray_sym_vec_domain(lhs) != ray_sym_vec_domain(rhs);
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
            else {
                lsym_buf = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
                if (!lsym_buf) goto done;
                for (int64_t j = 0; j < n; j++) lsym_buf[j] = ray_read_sym(l_data, l_off+j, lhs->type, lhs->attrs);
                lp_i64 = lsym_buf;
            }
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
            if (sym_xlate) {
                /* cross-domain: translate every rhs cell (any width) */
                rsym_buf = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
                if (!rsym_buf) goto done;
                struct ray_sym_domain_s* ld = ray_sym_vec_domain(lhs);
                struct ray_sym_domain_s* rd = ray_sym_vec_domain(rhs);
                for (int64_t j = 0; j < n; j++) {
                    int64_t rid = ray_read_sym(r_data, r_off+j, rhs->type, rhs->attrs);
                    ray_t* s = ray_sym_domain_str(rd, rid);
                    rsym_buf[j] = s ? ray_sym_domain_find(ld, ray_str_ptr(s),
                                                          ray_str_len(s)) : -1;
                }
                rp_i64 = rsym_buf;
            }
            else if (w == RAY_SYM_W64) rp_i64 = (int64_t*)rbase;
            else if (w == RAY_SYM_W32) rp_u32 = (uint32_t*)rbase;
            else {
                rsym_buf = (int64_t*)ray_alloc_raw((size_t)n * sizeof(int64_t));
                if (!rsym_buf) goto done;
                for (int64_t j = 0; j < n; j++) rsym_buf[j] = ray_read_sym(r_data, r_off+j, rhs->type, rhs->attrs);
                rp_i64 = rsym_buf;
            }
        }
        else if (rhs->type == RAY_I32 || rhs->type == RAY_DATE || rhs->type == RAY_TIME) rp_i32 = (int32_t*)rbase;
        else if (rhs->type == RAY_I16) rp_i16 = (int16_t*)rbase;
        else if (rhs->type == RAY_BOOL || rhs->type == RAY_U8) rp_bool = (uint8_t*)rbase;
    }

    /* Resolve the lhs/rhs reader to a single decision made once before the loop.
     * Each of these yields a double for the given index. */
#define LV_READ(i)  (lp_f64 ? lp_f64[i] : lp_i64 ? (double)lp_i64[i] : lp_i32 ? (double)lp_i32[i] : lp_u32 ? (double)lp_u32[i] : lp_i16 ? (double)lp_i16[i] : lp_bool ? (double)lp_bool[i] : (l_scalar && (lhs->type == -RAY_F64 || lhs->type == RAY_F64)) ? l_f64 : (double)l_i64)
#define RV_READ(i)  (rp_f64 ? rp_f64[i] : rp_i64 ? (double)rp_i64[i] : rp_i32 ? (double)rp_i32[i] : rp_u32 ? (double)rp_u32[i] : rp_i16 ? (double)rp_i16[i] : rp_bool ? (double)rp_bool[i] : (r_scalar && (rhs->type == -RAY_F64 || rhs->type == RAY_F64)) ? r_f64 : (double)r_i64)

    /* Compute once: is lhs/rhs integer-family (not float)? Used by BOOL path. */
    int l_is_int = !(lp_f64 || (l_scalar && (lhs->type == -RAY_F64 || lhs->type == RAY_F64)));
    int r_is_int = !(rp_f64 || (r_scalar && (rhs->type == -RAY_F64 || rhs->type == RAY_F64)));
    int src_is_i64_all = l_is_int && r_is_int;

    /* Hoist out_type outside the loop. Each branch is a tight per-element kernel. */
    if (out_type == RAY_F64) {
        double* odst = (double*)dst;
        switch (op->opcode) {
            /* Single-null float model: canonicalize non-finite F64 results
             * (overflow → ±Inf, div/mod-by-zero → NaN) to NULL_F64.  Mirrors
             * the fused kernel so VM ≡ fallback bit-for-bit.  HAS_NULLS is set
             * conservatively from the op shape at the tail of the caller. */
            case OP_ADD:  for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=ray_f64_fin(lv+rv); } break;
            case OP_SUB:  for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=ray_f64_fin(lv-rv); } break;
            case OP_MUL:  for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=ray_f64_fin(lv*rv); } break;
            case OP_DIV:  for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=rv!=0.0?ray_f64_fin(lv/rv):NULL_F64; } break;
            case OP_IDIV: for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=rv!=0.0?ray_f64_fin(floor(lv/rv)):NULL_F64; } break;
            case OP_MOD:  for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); double r; if(rv!=0.0){r=fmod(lv,rv);if(r&&((r>0)!=(rv>0)))r+=rv; r=ray_f64_fin(r);}else r=NULL_F64; odst[i]=r; } break;
            case OP_MIN2: for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=lv<rv?lv:rv; } break;
            case OP_MAX2: for (int64_t i=0;i<n;i++) { double lv=LV_READ(i),rv=RV_READ(i); odst[i]=lv>rv?lv:rv; } break;
            default:      for (int64_t i=0;i<n;i++) odst[i]=0.0; break;
        }
        /* Single-null float model: PRECISE HAS_NULLS, zero extra memory pass.
         * The producing arithmetic ops (ADD/SUB/MUL/DIV/IDIV/MOD) may have
         * canonicalized an overflow/0-divisor result to NULL_F64; scan the
         * range JUST written (odst[0..n] is hot in L1/L2, so this is near-free
         * vs a cold post-scan) and atomically OR HAS_NULLS iff a 0Nf actually
         * exists.  Precise (no input poisoning) AND no separate pass — keeps
         * the hot float kernels within noise and matches the fused path. */
        if (op->opcode == OP_ADD || op->opcode == OP_SUB || op->opcode == OP_MUL ||
            op->opcode == OP_DIV || op->opcode == OP_IDIV || op->opcode == OP_MOD) {
            int any_nan = 0;
            for (int64_t i = 0; i < n; i++) any_nan |= (odst[i] != odst[i]);
            if (any_nan)
                __atomic_fetch_or(&result->attrs, (uint8_t)RAY_ATTR_HAS_NULLS, __ATOMIC_RELAXED);
        }
    } else if (out_type == RAY_I64 || out_type == RAY_TIMESTAMP) {
        int64_t* odst = (int64_t*)dst;
        switch (op->opcode) {
            case OP_ADD: for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=(int64_t)((uint64_t)li+(uint64_t)ri);}break;
            case OP_SUB: for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=(int64_t)((uint64_t)li-(uint64_t)ri);}break;
            case OP_MUL: for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=(int64_t)((uint64_t)li*(uint64_t)ri);}break;
            case OP_DIV: for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);int64_t r;if(ri==0||(ri==-1&&li==INT64_MIN)){r=0;}else{r=li/ri;if((li^ri)<0&&r*ri!=li)r--;}odst[i]=r;}break;
            case OP_IDIV:for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);odst[i]=rv!=0.0?(int64_t)floor(lv/rv):0;}break;
            case OP_MOD: for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);int64_t r;if(ri==0||(ri==-1&&li==INT64_MIN)){r=0;}else{r=li%ri;if(r&&(r^ri)<0)r+=ri;}odst[i]=r;}break;
            case OP_MIN2:for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li<ri?li:ri;}break;
            case OP_MAX2:for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li>ri?li:ri;}break;
            default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
        }
    } else if (out_type == RAY_I32 || out_type == RAY_DATE || out_type == RAY_TIME) {
        int32_t* odst = (int32_t*)dst;
        switch (op->opcode) {
            case OP_ADD: for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);odst[i]=(int32_t)((uint32_t)li+(uint32_t)ri);}break;
            case OP_SUB: for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);odst[i]=(int32_t)((uint32_t)li-(uint32_t)ri);}break;
            case OP_MUL: for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);odst[i]=(int32_t)((uint32_t)li*(uint32_t)ri);}break;
            /* OP_DIV omitted — ray_binop hard-codes F64 for OP_DIV, so
             * narrow-output OP_DIV is unreachable through any caller. */
            case OP_IDIV:for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);odst[i]=rv!=0.0?(int32_t)floor(lv/rv):0;}break;
            case OP_MOD: for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);int32_t r;if(ri==0||(ri==-1&&li==INT32_MIN)){r=0;}else{r=li%ri;if(r&&(r^ri)<0)r+=ri;}odst[i]=r;}break;
            case OP_MIN2:for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);odst[i]=li<ri?li:ri;}break;
            case OP_MAX2:for(int64_t i=0;i<n;i++){int32_t li=(int32_t)LV_READ(i),ri=(int32_t)RV_READ(i);odst[i]=li>ri?li:ri;}break;
            default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
        }
    } else if (out_type == RAY_I16) {
        int16_t* odst = (int16_t*)dst;
        switch (op->opcode) {
            case OP_ADD: for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=(int16_t)((uint16_t)li+(uint16_t)ri);}break;
            case OP_SUB: for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=(int16_t)((uint16_t)li-(uint16_t)ri);}break;
            case OP_MUL: for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=(int16_t)((uint16_t)li*(uint16_t)ri);}break;
            /* OP_DIV omitted — unreachable, see I32 arm. */
            case OP_IDIV:for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);odst[i]=rv!=0.0?(int16_t)floor(lv/rv):0;}break;
            case OP_MOD: for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=ri?li%ri:0;}break;
            case OP_MIN2:for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=li<ri?li:ri;}break;
            case OP_MAX2:for(int64_t i=0;i<n;i++){int16_t li=(int16_t)LV_READ(i),ri=(int16_t)RV_READ(i);odst[i]=li>ri?li:ri;}break;
            default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
        }
    } else if (out_type == RAY_U8) {
        uint8_t* odst = (uint8_t*)dst;
        switch (op->opcode) {
            case OP_ADD: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li+ri;}break;
            case OP_SUB: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li-ri;}break;
            case OP_MUL: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li*ri;}break;
            /* OP_DIV omitted — unreachable, see I32 arm. */
            case OP_IDIV:for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);odst[i]=rv!=0.0?(uint8_t)floor(lv/rv):0;}break;
            case OP_MOD: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=ri?li%ri:0;}break;
            case OP_MIN2:for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li<ri?li:ri;}break;
            case OP_MAX2:for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li>ri?li:ri;}break;
            default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
        }
    } else if (out_type == RAY_BOOL) {
        uint8_t* odst = (uint8_t*)dst;
        if (src_is_i64_all) {
            /* Integer-family operands: compare as int64 */
            switch (op->opcode) {
                case OP_EQ:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li==ri;}break;
                case OP_NE:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li!=ri;}break;
                case OP_LT:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li<ri;}break;
                case OP_LE:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li<=ri;}break;
                case OP_GT:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li>ri;}break;
                case OP_GE:  for(int64_t i=0;i<n;i++){int64_t li=(int64_t)LV_READ(i),ri=(int64_t)RV_READ(i);odst[i]=li>=ri;}break;
                case OP_AND: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li&&ri;}break;
                case OP_OR:  for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li||ri;}break;
                default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
            }
        } else {
            /* Float-family: NaN is null sentinel */
            switch (op->opcode) {
                case OP_EQ:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?1:(ln||rn)?0:lv==rv;}break;
                case OP_NE:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?0:(ln||rn)?1:lv!=rv;}break;
                case OP_LT:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?0:ln?1:rn?0:lv<rv;}break;
                case OP_LE:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?1:ln?1:rn?0:lv<=rv;}break;
                case OP_GT:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?0:rn?1:ln?0:lv>rv;}break;
                case OP_GE:  for(int64_t i=0;i<n;i++){double lv=LV_READ(i),rv=RV_READ(i);int ln=lv!=lv,rn=rv!=rv;odst[i]=(ln&&rn)?1:rn?1:ln?0:lv>=rv;}break;
                case OP_AND: for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li&&ri;}break;
                case OP_OR:  for(int64_t i=0;i<n;i++){uint8_t li=(uint8_t)LV_READ(i),ri=(uint8_t)RV_READ(i);odst[i]=li||ri;}break;
                default:     for(int64_t i=0;i<n;i++)odst[i]=0;break;
            }
        }
    }
#undef LV_READ
#undef RV_READ
done:
    if (lsym_buf) ray_free_raw(lsym_buf);
    if (rsym_buf) ray_free_raw(rsym_buf);
}

/* Context for parallel binary dispatch */
typedef struct {
    ray_op_t* op;
    int8_t   out_type;
    ray_t*    lhs;
    ray_t*    rhs;
    ray_t*    result;
    void*     result_data;   /* ray_data(result), resolved once by the dispatcher */
    bool     l_scalar;
    bool     r_scalar;
    double   l_f64, r_f64;
    int64_t  l_i64, r_i64;
    /* Optional rowsel — when set, skip morsel segments that the prior
     * WHERE conjuncts have already filtered out.  Output positions for
     * filtered rows are left untouched (the caller's
     * ray_rowsel_refine() never reads pred[r] for r ∉ existing rowsel).
     * Drops the per-conjunct work proportional to selection narrowing. */
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    uint32_t        sel_n_segs;
} par_binary_ctx_t;

static void par_binary_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    par_binary_ctx_t* c = (par_binary_ctx_t*)ctx;
    if (!c->sel_flg) {
        binary_range(c->op, c->out_type, c->lhs, c->rhs, c->result, c->result_data,
                     c->l_scalar, c->r_scalar,
                     c->l_f64, c->r_f64, c->l_i64, c->r_i64,
                     start, end);
        return;
    }
    /* Selection-aware: walk per-morsel and skip RAY_SEL_NONE segments.
     * For ALL/MIX run the dense binary_range over the whole segment;
     * partial-segment row-masking would cost more than the saved work
     * since binary_range is already a tight typed loop. */
    uint32_t seg_lo = (uint32_t)(start / RAY_MORSEL_ELEMS);
    uint32_t seg_hi = (uint32_t)((end + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    if (seg_hi > c->sel_n_segs) seg_hi = c->sel_n_segs;
    for (uint32_t seg = seg_lo; seg < seg_hi; seg++) {
        int64_t s_lo = (int64_t)seg * RAY_MORSEL_ELEMS;
        int64_t s_hi = s_lo + RAY_MORSEL_ELEMS;
        if (s_lo < start) s_lo = start;
        if (s_hi > end)   s_hi = end;
        if (c->sel_flg[seg] == RAY_SEL_NONE) continue;
        binary_range(c->op, c->out_type, c->lhs, c->rhs, c->result, c->result_data,
                     c->l_scalar, c->r_scalar,
                     c->l_f64, c->r_f64, c->l_i64, c->r_i64,
                     s_lo, s_hi);
    }
}

ray_t* exec_elementwise_binary(ray_graph_t* g, ray_op_t* op, ray_t* lhs, ray_t* rhs) {
    if (!lhs || RAY_IS_ERR(lhs)) return lhs;
    if (!rhs || RAY_IS_ERR(rhs)) return rhs;

    bool l_scalar = ray_is_atom(lhs) || (lhs->type > 0 && lhs->len == 1);
    bool r_scalar = ray_is_atom(rhs) || (rhs->type > 0 && rhs->len == 1);

    int64_t len = 1;
    if (!l_scalar && !r_scalar) {
        if (lhs->len != rhs->len) return ray_error("length", "expr eval: binary op operand lengths must match, got %lld and %lld", (long long)lhs->len, (long long)rhs->len);
        len = lhs->len;
    } else if (l_scalar && !r_scalar) {
        len = rhs->len;
    } else if (!l_scalar && r_scalar) {
        len = lhs->len;
    }

    int8_t out_type = ew_runtime_out_type(op->out_type,
                                          !l_scalar ? lhs->type
                                        : !r_scalar ? rhs->type
                                        : (int8_t)(lhs->type < 0 ? -lhs->type : lhs->type));
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
            if (opc < OP_EQ || opc > OP_GE) { ray_release(result); return ray_error("type", "expr eval: string operands support only comparison operators, got %s and %s", ray_type_name(lhs->type), ray_type_name(rhs->type)); }
            /* At least one side is a RAY_STR column — use string comparison path.
               The scalar side (if any) must be -RAY_STR or RAY_SYM atom.
               The non-scalar side must be RAY_STR. */
            if (l_scalar && !l_atom_str) { ray_release(result); return ray_error("type", "expr eval: string comparison left operand must be a str/sym scalar, got %s", ray_type_name(lhs->type)); }
            if (r_scalar && !r_atom_str) { ray_release(result); return ray_error("type", "expr eval: string comparison right operand must be a str/sym scalar, got %s", ray_type_name(rhs->type)); }
            if (!l_scalar && !l_is_str) { ray_release(result); return ray_error("type", "expr eval: string comparison left operand must be a str column, got %s", ray_type_name(lhs->type)); }
            if (!r_scalar && !r_is_str) { ray_release(result); return ray_error("type", "expr eval: string comparison right operand must be a str column, got %s", ray_type_name(rhs->type)); }

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

    /* Reject string atom in arithmetic context (only comparisons are valid). */
    {
        bool l_atom_str = (l_scalar && lhs->type == -RAY_STR);
        bool r_atom_str = (r_scalar && rhs->type == -RAY_STR);
        if (l_atom_str || r_atom_str) {
            uint16_t opc = op->opcode;
            bool is_cmp = (opc >= OP_EQ && opc <= OP_GE);
            if (!is_cmp && !RAY_IS_SYM(lhs->type) && !RAY_IS_SYM(rhs->type)) {
                ray_release(result);
                return ray_error("type", "expr eval: string atom is not valid in arithmetic, got %s and %s", ray_type_name(lhs->type), ray_type_name(rhs->type));
            }
        }
    }

    /* SYM vs STR comparison: resolve the string constant to a position
       in the SYM COLUMN's domain so we can compare numerically against
       its raw cell indices (sym-domain Phase 2; the runtime singleton
       delegates to ray_sym_find — exact no-op pre-flip).
       Lookup returns -1 if the string is absent → no match (truncated
       -1 can never equal a valid id at any width by ray_sym_dict_width
       construction). */
    bool str_resolved = false;
    int64_t resolved_sym_id = 0;
    if (r_scalar && rhs->type == -RAY_STR &&
        RAY_IS_SYM(lhs->type)) {
        const char* s = ray_str_ptr(rhs);
        size_t slen = ray_str_len(rhs);
        resolved_sym_id = ray_sym_vec_lookup(lhs, s, slen);
        str_resolved = true;
    } else if (l_scalar && lhs->type == -RAY_STR &&
               RAY_IS_SYM(rhs->type)) {
        const char* s = ray_str_ptr(lhs);
        size_t slen = ray_str_len(lhs);
        resolved_sym_id = ray_sym_vec_lookup(rhs, s, slen);
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

    /* sym-domain Phase 2: a SYM scalar (runtime-domain atom, or a 1-len
     * SYM vec with its own domain) compared against a SYM COLUMN is a
     * raw index compare against the column's cells, so the scalar must
     * be re-expressed in the COLUMN's domain.  Same-domain (the only
     * pre-flip case) keeps the raw id — byte-identical fast path.
     * Absent ⇒ -1: equals no cell (see the lookup note above). */
    if (!l_scalar && RAY_IS_SYM(lhs->type) && r_scalar &&
        (rhs->type == -RAY_SYM || RAY_IS_SYM(rhs->type))) {
        struct ray_sym_domain_s* cdom = ray_sym_vec_domain(lhs);
        struct ray_sym_domain_s* sdom = (rhs->type == -RAY_SYM)
            ? ray_sym_runtime_domain() : ray_sym_vec_domain(rhs);
        if (cdom != sdom) {
            ray_t* s = ray_sym_domain_str(sdom, r_i64_val);
            r_i64_val = s ? ray_sym_domain_find(cdom, ray_str_ptr(s),
                                                ray_str_len(s)) : -1;
        }
    } else if (!r_scalar && RAY_IS_SYM(rhs->type) && l_scalar &&
               (lhs->type == -RAY_SYM || RAY_IS_SYM(lhs->type))) {
        struct ray_sym_domain_s* cdom = ray_sym_vec_domain(rhs);
        struct ray_sym_domain_s* sdom = (lhs->type == -RAY_SYM)
            ? ray_sym_runtime_domain() : ray_sym_vec_domain(lhs);
        if (cdom != sdom) {
            ray_t* s = ray_sym_domain_str(sdom, l_i64_val);
            l_i64_val = s ? ray_sym_domain_find(cdom, ray_str_ptr(s),
                                                ray_str_len(s)) : -1;
        }
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD) {
        par_binary_ctx_t ctx = {
            .op = op, .out_type = out_type,
            .lhs = lhs, .rhs = rhs, .result = result,
            .result_data = ray_data(result),   /* resolve once on this thread */
            .l_scalar = l_scalar, .r_scalar = r_scalar,
            .l_f64 = l_f64_val, .r_f64 = r_f64_val,
            .l_i64 = l_i64_val, .r_i64 = r_i64_val,
            .sel_flg    = NULL,
            .sel_offs   = NULL,
            .sel_idx    = NULL,
            .sel_n_segs = 0,
        };
        if (g && g->selection) {
            ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
            if (sm->nrows == len) {
                ctx.sel_flg    = ray_rowsel_flags(g->selection);
                ctx.sel_offs   = ray_rowsel_offsets(g->selection);
                ctx.sel_idx    = ray_rowsel_idx(g->selection);
                ctx.sel_n_segs = sm->n_segs;
            }
        }
        ray_pool_dispatch(pool, par_binary_fn, &ctx, len);
    } else {
        binary_range(op, out_type, lhs, rhs, result, ray_data(result),
                     l_scalar, r_scalar,
                     l_f64_val, r_f64_val, l_i64_val, r_i64_val,
                     0, len);
    }

    /* Null propagation from inputs.  Skipped when str_resolved: we resolved
     * a string constant to an integer sym id and compared it by value against
     * a SYM column.  SYM columns carry no nulls (id 0 / the interned empty
     * string is a real value — see ray_sym_init / ray_vec_is_null), and the
     * resolved string atom must NOT be treated as null here.  Otherwise the
     * empty-string literal "" — for which RAY_ATOM_IS_NULL is true (slen==0,
     * obj==NULL) yet which resolves to the valid sym id 0 — would take the
     * null-comparison fill: `!= col ""` passing every row and `== col ""`
     * matching none, instead of selecting the empty-string rows by value
     * (which silently drops a `(!= symcol "")` WHERE predicate). */
    if (!str_resolved) {
        if (op_propagates_null(op->opcode))
            propagate_nulls_binary(lhs, rhs, result, l_scalar, r_scalar, len);
        else
            fix_null_comparisons(lhs, rhs, result, l_scalar, r_scalar, len, op->opcode);
    }

    /* Div/mod: mark zero-divisor positions as null.
     * The morsel loop writes 0 for b==0 but can't set bitmap nulls. */
    uint16_t opc = op->opcode;
    if (opc == OP_DIV || opc == OP_IDIV || opc == OP_MOD) {
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

    /* Single-null float model: HAS_NULLS for newly-produced 0Nf is set
     * PRECISELY inside binary_range's F64 block (hot-cache scan of the range it
     * just wrote, atomic-OR into result->attrs) — no separate cold pass, no
     * input poisoning.  Nothing to do here. */

    return result;
}

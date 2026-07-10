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

#include "lang/internal.h"
#include "ops/ops.h"

/* Arithmetic builtins (atom-only).
 * Vector dispatch goes through the DAG executor. */

typedef ray_op_t* (*arith_unary_ctor)(ray_graph_t*, ray_op_t*);

static bool arith_numeric_vec_type(int8_t t) {
    if (RAY_IS_PARTED(t)) t = (int8_t)RAY_PARTED_BASETYPE(t);
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 ||
           t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64;
}

static ray_t* arith_unary_dag_vec(ray_t* x, arith_unary_ctor ctor) {
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", "unary math: graph alloc failed");
    ray_op_t* in = ray_const_vec(g, x);
    ray_op_t* root = in ? ctor(g, in) : NULL;
    if (!root) {
        ray_graph_free(g);
        return ray_error("oom", "unary math: graph node alloc failed");
    }
    ray_t* r = ray_execute(g, root);
    ray_graph_free(g);
    return r ? r : ray_error("oom", "unary math: execution failed");
}

static ray_t* ray_f64_unary_math_fn(ray_t* x, ray_unary_fn self,
                                    arith_unary_ctor ctor,
                                    double (*fn)(double),
                                    const char* name) {
    if (ray_is_vec(x)) {
        if (!arith_numeric_vec_type(x->type))
            return ray_error("type", "%s: expects a numeric argument, got %s",
                             name, ray_type_name(x->type));
        return arith_unary_dag_vec(x, ctor);
    }
    if (x->type == RAY_LIST)
        return atomic_map_unary(self, x);
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (!is_numeric(x))
        return ray_error("type", "%s: expects a numeric argument, got %s",
                         name, ray_type_name(x->type));
    return make_f64(fn(as_f64(x)));
}

ray_t* ray_add_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_add_fn, OP_ADD, a, b);
    /* Vector fast path — only when at least one operand is a typed vector */

    /* Temporal + integer arithmetic (only int types, not float) */
    if (is_temporal(a) && is_numeric(b) &&
        b->type != -RAY_F64 && b->type != -RAY_F32) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);

        int64_t v = as_i64(b);
        if (a->type == -RAY_DATE)      return ray_date(a->i64 + v);
        if (a->type == -RAY_TIME)      return ray_time(a->i64 + v);
        if (a->type == -RAY_TIMESTAMP) return ray_timestamp(a->i64 + v);
    }
    if (is_numeric(a) && a->type != -RAY_F64 &&
        a->type != -RAY_F32 && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(b->type);

        int64_t v = as_i64(a);
        if (b->type == -RAY_DATE)      return ray_date(b->i64 + v);
        if (b->type == -RAY_TIME)      return ray_time(b->i64 + v);
        if (b->type == -RAY_TIMESTAMP) return ray_timestamp(b->i64 + v);
    }
    /* Reject float + temporal */
    if (((a->type == -RAY_F64 || a->type == -RAY_F32) && is_temporal(b)) ||
        (is_temporal(a) && (b->type == -RAY_F64 || b->type == -RAY_F32)))
        return ray_error("type", "add: temporal arithmetic requires integer offsets, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Reject null_numeric + temporal (for null floats etc) */
    if (is_numeric(a) && RAY_ATOM_IS_NULL(a) && is_temporal(b))
        return ray_error("type", "add: unsupported temporal operand combination, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (is_temporal(a) && is_numeric(b) && RAY_ATOM_IS_NULL(b))
        return ray_error("type", "add: unsupported temporal operand combination, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* DATE + TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(a->i64 * 86400000000000LL + b->i64 * 1000000LL);
    }
    if (a->type == -RAY_TIME && b->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(b->i64 * 86400000000000LL + a->i64 * 1000000LL);
    }
    /* TIME + TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(a->i64 + b->i64);
    }
    /* TIME + TIMESTAMP → TIMESTAMP (add ms as ns) */
    if (a->type == -RAY_TIME && b->type == -RAY_TIMESTAMP) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(b->i64 + a->i64 * 1000000LL);
    }
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(a->i64 + b->i64 * 1000000LL);
    }

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot add %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (both_f32_atoms(a, b)) return ray_f32((float)(as_f64(a) + as_f64(b)));
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) + as_i64(b));
}

ray_t* ray_sub_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_sub_fn, OP_SUB, a, b);

    /* Temporal - int null propagation (both operands) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);
    }
    if (is_numeric(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(b->type);
    }
    /* DATE - int → DATE */
    if (a->type == -RAY_DATE && is_numeric(b)) {
        return ray_date(a->i64 - as_i64(b));
    }
    /* DATE - DATE → i32 (days difference) */
    if (a->type == -RAY_DATE && b->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_I32);
        return ray_i32((int32_t)(a->i64 - b->i64));
    }
    /* DATE - TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(a->i64 * 86400000000000LL - b->i64 * 1000000LL);
    }
    /* TIME - int → TIME */
    if (a->type == -RAY_TIME && is_numeric(b)) {
        return ray_time(a->i64 - as_i64(b));
    }
    /* int - TIME → TIME (negative) */
    if (is_numeric(a) && b->type == -RAY_TIME) {
        return ray_time(as_i64(a) - b->i64);
    }
    /* TIME - TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(a->i64 - b->i64);
    }
    /* TIMESTAMP - int → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && is_numeric(b)) {
        return ray_timestamp(a->i64 - as_i64(b));
    }
    /* TIMESTAMP - TIME → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(a->i64 - b->i64 * 1000000LL);
    }
    /* TIMESTAMP - TIMESTAMP → int (nanos difference) */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIMESTAMP) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_I64);
        return make_i64(a->i64 - b->i64);
    }
    /* TIMESTAMP - DATE → error */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_DATE)
        return ray_error("type", "subtract: cannot subtract %s from %s",
                         ray_type_name(b->type), ray_type_name(a->type));

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot subtract %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) {
        double r = as_f64(a) - as_f64(b);
        if (r == 0.0) r = 0.0; /* normalize -0.0 to +0.0 */
        if (both_f32_atoms(a, b)) return ray_f32((float)r);
        return make_f64(r);
    }
    int8_t rt = promote_int_type_right(a, b);
    return make_typed_int(rt, as_i64(a) - as_i64(b));
}

ray_t* ray_mul_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_mul_fn, OP_MUL, a, b);

    /* int * TIME → TIME, TIME * int → TIME */
    if (is_numeric(a) && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(as_i64(a) * b->i64);
    }
    if (a->type == -RAY_TIME && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(a->i64 * as_i64(b));
    }
    /* TIME * TIME → error */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME)
        return ray_error("type", "multiply: cannot multiply %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot multiply %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (both_f32_atoms(a, b)) return ray_f32((float)(as_f64(a) * as_f64(b)));
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) * as_i64(b));
}

ray_t* ray_div_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_div_fn, OP_DIV, a, b);
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot divide %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
        return ray_typed_null(-RAY_F64);
    double bv = as_f64(b);
    if (bv == 0.0)
        return ray_typed_null(-RAY_F64);
    return make_f64(as_f64(a) / bv);
}

ray_t* ray_idiv_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot div %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
        return ray_typed_null(-RAY_I64);
    double bv = as_f64(b);
    if (bv == 0.0)
        return ray_typed_null(-RAY_I64);
    double q = floor(as_f64(a) / bv);
    if (q < (double)INT64_MIN || q > (double)INT64_MAX)
        return ray_typed_null(-RAY_I64);
    return make_i64((int64_t)q);
}

ray_t* ray_mod_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_mod_fn, OP_MOD, a, b);
    /* Temporal % numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);
        int64_t bv;
        if (b->type == -RAY_F64 || b->type == -RAY_F32) {
            double bvf = as_f64(b);
            if (bvf == 0.0)
                return ray_typed_null(a->type);
            bv = (int64_t)bvf;
        } else {
            bv = as_i64(b);
        }
        if (bv == 0)
            return ray_typed_null(a->type);

        int64_t av = a->i64;
        int64_t q = av / bv;
        if ((av ^ bv) < 0 && q * bv != av) q--;
        int64_t result = av - bv * q;
        if (a->type == -RAY_TIME)      return ray_time(result);
        if (a->type == -RAY_DATE)      return ray_date(result);
        return ray_timestamp(result);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot mod %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));

    /* u8: unsigned byte modulo, no null sentinel — mod by 0 returns 0 */
    if (b->type == -RAY_U8) {
        uint8_t bv = b->u8;
        if (bv == 0) return make_u8(0);
        return make_u8((uint8_t)((uint8_t)as_i64(a) % bv));
    }
    if (a->type == -RAY_U8) {
        /* a is u8 but b is not u8 — treat as integer, result follows b's type */
    }

    /* Null propagation and division by zero: null type follows RIGHT operand */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) {
        return null_for_promoted(a, b);
    }

    /* Float modulo: result = a - b * floor(a/b), type follows RIGHT or f64 */
    if (is_float_op(a, b)) {
        double av = as_f64(a), bv = as_f64(b);
        if (bv == 0.0) {
            return null_for_promoted(a, b);
        }
        double result = av - bv * floor(av / bv);
        /* Snap tiny residual to 0 */
        if (fabs(result) < 1e-12 || fabs(result - fabs(bv)) < 1e-12) result = bv > 0 ? 0.0 : -0.0;
        if (both_f32_atoms(a, b)) return ray_f32((float)result);
        if (b->type == -RAY_F64 || a->type == -RAY_F64 ||
            b->type == -RAY_F32 || a->type == -RAY_F32) return make_f64(result);
        if (b->type == -RAY_I32) return make_i32((int32_t)(int64_t)result);
        if (b->type == -RAY_I16) return make_i16((int16_t)(int64_t)result);
        return make_i64((int64_t)result);
    }

    /* Integer modulo: result = a - b * floor(a/b), sign follows b (divisor) */
    int64_t av = as_i64(a), bv = as_i64(b);
    if (bv == 0)
        return ray_typed_null(b->type);

    int64_t q = av / bv;
    if ((av ^ bv) < 0 && q * bv != av) q--;  /* floor division */
    int64_t result = av - bv * q;
    /* Result type follows RIGHT operand */
    if (b->type == -RAY_I32) return make_i32((int32_t)result);
    if (b->type == -RAY_I16) return make_i16((int16_t)result);
    if (b->type == -RAY_U8) return make_u8((uint8_t)result);
    return make_i64(result);
}

ray_t* ray_neg_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(-x->f64);
    if (x->type == -RAY_F32) return ray_f32((float)-x->f64);
    /* INT_MIN is the lone overflow case for signed negation: -INT_MIN
     * doesn't fit in the same width.  By convention, surface this
     * as a typed null of the same width — preserving type, avoiding UB,
     * and giving the caller a `nil?`-detectable signal that overflow
     * happened.  Consistent with how `(neg 0Ni) → 0Ni` propagates. */
    if (x->type == -RAY_I64) {
        if (RAY_UNLIKELY(x->i64 == INT64_MIN)) return ray_typed_null(-RAY_I64);
        return make_i64(-x->i64);
    }
    if (x->type == -RAY_I32) {
        if (RAY_UNLIKELY(x->i32 == INT32_MIN)) return ray_typed_null(-RAY_I32);
        return make_i32(-x->i32);
    }
    if (x->type == -RAY_I16) {
        if (RAY_UNLIKELY(x->i16 == INT16_MIN)) return ray_typed_null(-RAY_I16);
        return make_i16(-x->i16);
    }
    return ray_error("type", "negate: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* round: round to nearest integer (ties go away from zero), returns f64 */
ray_t* ray_round_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) {
        if (x->type == -RAY_F32) return ray_typed_null(-RAY_F32);
        return ray_typed_null(-RAY_F64);
    }
    if (x->type == -RAY_F64) return make_f64(round(x->f64));
    if (x->type == -RAY_F32) return ray_f32(roundf((float)x->f64));
    if (is_numeric(x)) return make_f64(round(as_f64(x)));
    return ray_error("type", "round: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* floor: round toward -inf, returns f64 for f64, identity for int */
ray_t* ray_floor_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(floor(x->f64));
    if (x->type == -RAY_F32) return ray_f32(floorf((float)x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", "floor: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* ceil: round toward +inf, returns f64 for f64, identity for int */
ray_t* ray_ceil_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(ceil(x->f64));
    if (x->type == -RAY_F32) return ray_f32(ceilf((float)x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", "ceil: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* abs: absolute value, preserves type.  INT_MIN has no representable
 * positive in the same width — return a typed null instead (same
 * convention as `neg`).  Stops `(abs -32768h) → -32768h` (negative
 * result from abs!) and `(abs INT_MIN)` UB simultaneously. */
ray_t* ray_abs_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(fabs(x->f64));
    if (x->type == -RAY_F32) return ray_f32(fabsf((float)x->f64));
    if (x->type == -RAY_I64) {
        if (RAY_UNLIKELY(x->i64 == INT64_MIN)) return ray_typed_null(-RAY_I64);
        return make_i64(x->i64 < 0 ? -x->i64 : x->i64);
    }
    if (x->type == -RAY_I32) {
        if (RAY_UNLIKELY(x->i32 == INT32_MIN)) return ray_typed_null(-RAY_I32);
        return make_i32(x->i32 < 0 ? -x->i32 : x->i32);
    }
    if (x->type == -RAY_I16) {
        if (RAY_UNLIKELY(x->i16 == INT16_MIN)) return ray_typed_null(-RAY_I16);
        return make_i16(x->i16 < 0 ? -x->i16 : x->i16);
    }
    return ray_error("type", "abs: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* sqrt: square root, returns f64 */
ray_t* ray_sqrt_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(sqrt(x->f64));
    if (is_numeric(x)) return make_f64(sqrt(as_f64(x)));
    return ray_error("type", "sqrt: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* log: natural logarithm, returns f64 */
ray_t* ray_log_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(log(x->f64));
    if (is_numeric(x)) return make_f64(log(as_f64(x)));
    return ray_error("type", "log: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* exp: e^x, returns f64 */
ray_t* ray_exp_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(exp(x->f64));
    if (is_numeric(x)) return make_f64(exp(as_f64(x)));
    return ray_error("type", "exp: expects a numeric argument, got %s", ray_type_name(x->type));
}

ray_t* ray_sin_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_sin_fn, ray_sin_op, sin, "sin");
}

ray_t* ray_asin_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_asin_fn, ray_asin_op, asin, "asin");
}

ray_t* ray_cos_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_cos_fn, ray_cos_op, cos, "cos");
}

ray_t* ray_acos_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_acos_fn, ray_acos_op, acos, "acos");
}

ray_t* ray_tan_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_tan_fn, ray_tan_op, tan, "tan");
}

ray_t* ray_atan_fn(ray_t* x) {
    return ray_f64_unary_math_fn(x, ray_atan_fn, ray_atan_op, atan, "atan");
}

ray_t* ray_reciprocal_fn(ray_t* x) {
    if (ray_is_vec(x)) {
        if (!arith_numeric_vec_type(x->type))
            return ray_error("type", "reciprocal: expects a numeric argument, got %s",
                             ray_type_name(x->type));
        return arith_unary_dag_vec(x, ray_reciprocal_op);
    }
    if (x->type == RAY_LIST)
        return atomic_map_unary(ray_reciprocal_fn, x);
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (!is_numeric(x))
        return ray_error("type", "reciprocal: expects a numeric argument, got %s",
                         ray_type_name(x->type));
    double v = as_f64(x);
    return v != 0.0 ? make_f64(1.0 / v) : ray_typed_null(-RAY_F64);
}

ray_t* ray_signum_fn(ray_t* x) {
    if (ray_is_vec(x)) {
        if (!arith_numeric_vec_type(x->type))
            return ray_error("type", "signum: expects a numeric argument, got %s",
                             ray_type_name(x->type));
        return arith_unary_dag_vec(x, ray_signum_op);
    }
    if (x->type == RAY_LIST)
        return atomic_map_unary(ray_signum_fn, x);
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
    if (!is_numeric(x))
        return ray_error("type", "signum: expects a numeric argument, got %s",
                         ray_type_name(x->type));
    double v = as_f64(x);
    return make_i64((v > 0.0) - (v < 0.0));
}

/* pow: x raised to y, returns f64.
 *
 * Atomic binary — broadcasts over numeric vectors via the same
 * RAY_FN_ATOMIC dispatch the other binary atomic ops use.  Result is
 * always F64; integer bases with integer exponents still go through
 * libm pow() so fractional exponents use standard floating-point
 * semantics (e.g. (pow 2 0.5) → 1.41…).
 *
 * Null propagation: either operand null → typed F64 null. */
ray_t* ray_pow_fn(ray_t* x, ray_t* y) {
    if (RAY_ATOM_IS_NULL(x) || RAY_ATOM_IS_NULL(y))
        return ray_typed_null(-RAY_F64);
    if (!is_numeric(x) || !is_numeric(y))
        return ray_error("type", "pow: expects numeric base and exponent, got %s and %s",
                         ray_type_name(x->type), ray_type_name(y->type));
    return make_f64(pow(as_f64(x), as_f64(y)));
}

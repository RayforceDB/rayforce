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

/* Arithmetic builtins (atom-only).
 * Vector dispatch goes through the DAG executor. */

ray_t* ray_add_fn(ray_t* a, ray_t* b) {
    /* Vector fast path — only when at least one operand is a typed vector */

    /* Temporal + integer arithmetic (only int types, not float) */
    if (is_temporal(a) && is_numeric(b) && b->type != -RAY_F64) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);

        int64_t v = as_i64(b);
        if (a->type == -RAY_DATE)      return ray_date(a->i64 + v);
        if (a->type == -RAY_TIME)      return ray_time(a->i64 + v);
        if (a->type == -RAY_TIMESTAMP) return ray_timestamp(a->i64 + v);
    }
    if (is_numeric(a) && a->type != -RAY_F64 && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(b->type);

        int64_t v = as_i64(a);
        if (b->type == -RAY_DATE)      return ray_date(b->i64 + v);
        if (b->type == -RAY_TIME)      return ray_time(b->i64 + v);
        if (b->type == -RAY_TIMESTAMP) return ray_timestamp(b->i64 + v);
    }
    /* Reject float + temporal */
    if ((a->type == -RAY_F64 && is_temporal(b)) || (is_temporal(a) && b->type == -RAY_F64))
        return ray_error("type", NULL);
    /* Reject null_numeric + temporal (for null floats etc) */
    if (is_numeric(a) && RAY_ATOM_IS_NULL(a) && is_temporal(b))
        return ray_error("type", NULL);
    if (is_temporal(a) && is_numeric(b) && RAY_ATOM_IS_NULL(b))
        return ray_error("type", NULL);
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
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) + as_i64(b));
}

ray_t* ray_sub_fn(ray_t* a, ray_t* b) {

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
        return ray_error("type", NULL);

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot subtract %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) {
        double r = as_f64(a) - as_f64(b);
        if (r == 0.0) r = 0.0; /* normalize -0.0 to +0.0 */
        return make_f64(r);
    }
    int8_t rt = promote_int_type_right(a, b);
    return make_typed_int(rt, as_i64(a) - as_i64(b));
}

ray_t* ray_mul_fn(ray_t* a, ray_t* b) {

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
        return ray_error("type", NULL);

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot multiply %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) * as_i64(b));
}

ray_t* ray_div_fn(ray_t* a, ray_t* b) {
    /* Temporal / numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(b) || RAY_ATOM_IS_NULL(a))
            return ray_typed_null(a->type);
        if (is_float_op(a, b)) {
            double bv = as_f64(b);
            if (bv == 0.0)
                return ray_typed_null(a->type);
            int64_t result = (int64_t)floor((double)a->i64 / bv);
            if (a->type == -RAY_TIME)      return ray_time(result);
            if (a->type == -RAY_DATE)      return ray_date(result);
            return ray_timestamp(result);
        }
        int64_t bv = as_i64(b);
        if (bv == 0)
            return ray_typed_null(a->type);
        int64_t av = a->i64;
        int64_t q = av / bv;
        if ((av ^ bv) < 0 && q * bv != av) q--;
        if (a->type == -RAY_TIME)      return ray_time(q);
        if (a->type == -RAY_DATE)      return ray_date(q);
        return ray_timestamp(q);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot divide %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* u8: unsigned byte division — div by 0 returns 0 */
    if (a->type == -RAY_U8) {
        uint8_t bv = (uint8_t)as_i64(b);
        if (bv == 0 || RAY_ATOM_IS_NULL(b)) return make_u8(0);
        if (RAY_ATOM_IS_NULL(a)) return make_u8(0);
        return make_u8((uint8_t)((uint8_t)as_i64(a) / bv));
    }
    /* Null propagation — null operand → typed null matching left operand type */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
        return ray_typed_null(a->type);

    /* Integer (floor) division — always returns integer.
     * Float operands are converted to i64 via floor(a/b). */
    if (is_float_op(a, b)) {
        double bv = as_f64(b);
        if (bv == 0.0)
            return ray_typed_null(a->type);
        double result = floor(as_f64(a) / bv);
        /* Return type matches LEFT operand */
        if (a->type == -RAY_F64) return make_f64(result);
        if (a->type == -RAY_I16) return make_i16((int16_t)(int64_t)result);
        if (a->type == -RAY_I32) return make_i32((int32_t)(int64_t)result);
        if (result >= (double)INT64_MIN && result <= (double)INT64_MAX)
            return make_i64((int64_t)result);
        return ray_typed_null(-RAY_I64);
    }
    int64_t bv = as_i64(b);
    if (bv == 0)
        return ray_typed_null(a->type);

    int64_t av = as_i64(a);
    /* Floor division (toward -inf) */
    int64_t q = av / bv;
    if ((av ^ bv) < 0 && q * bv != av) q--;
    /* Return type matches LEFT operand for i16/i32 */
    if (a->type == -RAY_I16) return make_i16((int16_t)q);
    if (a->type == -RAY_I32) return make_i32((int32_t)q);
    return make_i64(q);
}

ray_t* ray_mod_fn(ray_t* a, ray_t* b) {
    /* Temporal % numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);
        int64_t bv;
        if (b->type == -RAY_F64) {
            double bvf = b->f64;
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
        int8_t rt = (b->type == -RAY_F64 || a->type == -RAY_F64) ? -RAY_F64 : b->type;
        return ray_typed_null(rt);
    }

    /* Float modulo: result = a - b * floor(a/b), type follows RIGHT or f64 */
    if (is_float_op(a, b)) {
        double av = as_f64(a), bv = as_f64(b);
        if (bv == 0.0) {
            int8_t rt = (b->type == -RAY_F64 || a->type == -RAY_F64) ? -RAY_F64 : b->type;
            return ray_typed_null(rt);
        }
        double result = av - bv * floor(av / bv);
        /* Snap tiny residual to 0 */
        if (fabs(result) < 1e-12 || fabs(result - fabs(bv)) < 1e-12) result = bv > 0 ? 0.0 : -0.0;
        if (b->type == -RAY_F64 || a->type == -RAY_F64) return make_f64(result);
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
    /* INT_MIN is the lone overflow case for signed negation: -INT_MIN
     * doesn't fit in the same width.  Per k/q convention, surface this
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
    return ray_error("type", NULL);
}

/* round: round to nearest integer (ties go away from zero), returns f64 */
ray_t* ray_round_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(round(x->f64));
    if (is_numeric(x)) return make_f64(round(as_f64(x)));
    return ray_error("type", NULL);
}

/* floor: round toward -inf, returns f64 for f64, identity for int */
ray_t* ray_floor_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(floor(x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", NULL);
}

/* ceil: round toward +inf, returns f64 for f64, identity for int */
ray_t* ray_ceil_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(ceil(x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", NULL);
}

/* abs: absolute value, preserves type.  INT_MIN has no representable
 * positive in the same width — return a typed null instead (same
 * convention as `neg`).  Stops `(abs -32768h) → -32768h` (negative
 * result from abs!) and `(abs INT_MIN)` UB simultaneously. */
ray_t* ray_abs_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(fabs(x->f64));
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
    return ray_error("type", NULL);
}

/* sqrt: square root, returns f64 */
ray_t* ray_sqrt_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(sqrt(x->f64));
    if (is_numeric(x)) return make_f64(sqrt(as_f64(x)));
    return ray_error("type", NULL);
}

/* log: natural logarithm, returns f64 */
ray_t* ray_log_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(log(x->f64));
    if (is_numeric(x)) return make_f64(log(as_f64(x)));
    return ray_error("type", NULL);
}

/* exp: e^x, returns f64 */
ray_t* ray_exp_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(exp(x->f64));
    if (is_numeric(x)) return make_f64(exp(as_f64(x)));
    return ray_error("type", NULL);
}

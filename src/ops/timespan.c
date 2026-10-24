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
#include "core/pool.h"
#include "vec/vec.h"

/* Local selector; bucket evaluation does not allocate a DAG opcode. */
#define SPAN_BUCKET UINT16_MAX

static int8_t span_base(int8_t t) {
    if (t < 0) return (int8_t)-t;
    return RAY_IS_PARTED(t) ? (int8_t)RAY_PARTED_BASETYPE(t) : t;
}

bool ray_has_timespan(const ray_t* x) {
    return x && span_base(x->type) == RAY_TIMESPAN;
}

static bool span_number(int8_t t) { return t >= RAY_BOOL && t <= RAY_F64; }
static bool span_divisor(int8_t t) {
    return span_number(t) || t == RAY_TIMESPAN || t == RAY_TIME ||
           t == RAY_DATE || t == RAY_TIMESTAMP;
}

int8_t ray_timespan_binary_type(uint16_t op, int8_t left, int8_t right) {
    int8_t a = span_base(left), b = span_base(right);
    bool an = a == RAY_TIMESPAN, bn = b == RAY_TIMESPAN;
    if (op == OP_SUB && a == RAY_TIMESTAMP && b == RAY_TIMESTAMP) return RAY_TIMESPAN;
    if (!an && !bn) return 0;
    if (op == SPAN_BUCKET && span_divisor(a) && span_divisor(b))
        return a == RAY_F64 || a == RAY_F32 || b == RAY_F64 || b == RAY_F32
            ? RAY_F64 : span_number(a) ? RAY_I64 : a;

    if (op >= OP_EQ && op <= OP_GE)
        return ((an && (bn || span_number(b) || b == RAY_TIME || b == RAY_TIMESTAMP)) ||
                (bn && (span_number(a) || a == RAY_TIME || a == RAY_TIMESTAMP))) ? RAY_BOOL : -1;
    if (op == OP_ADD || op == OP_SUB) {
        if (a == RAY_DATE || b == RAY_DATE || a == RAY_TIMESTAMP || b == RAY_TIMESTAMP)
            return RAY_TIMESTAMP;
        if ((an || span_number(a) || a == RAY_TIME) &&
            (bn || span_number(b) || b == RAY_TIME))
            return a == RAY_F64 || b == RAY_F64 ? RAY_F64 : RAY_TIMESPAN;
    }
    if (op == OP_DIV && span_divisor(a) && span_divisor(b)) return RAY_F64;
    if (op == OP_IDIV && span_divisor(a) && span_divisor(b))
        return an ? RAY_TIMESPAN : (a == RAY_F32 ? RAY_F64 : a == RAY_BOOL || a == RAY_U8 || a == RAY_I16 ? RAY_I32 : a);
    if ((op == OP_MUL || op == OP_MOD) && an != bn &&
        (an ? span_number(b) : span_number(a)))
        return a == RAY_F64 || b == RAY_F64 ? RAY_F64 : RAY_TIMESPAN;
    if ((op == OP_MIN2 || op == OP_MAX2) && an && bn) return RAY_TIMESPAN;
    return -1;
}

static int64_t span_value(ray_t* x, uint16_t op) {
    if (op == OP_DIV || op == OP_IDIV || op == SPAN_BUCKET) {
        if (x->type == -RAY_DATE || x->type == -RAY_TIME) return x->i32;
    }
    if (x->type == -RAY_DATE) return wrap_mul_i64(x->i32, INT64_C(86400000000000));
    if (x->type == -RAY_TIME) return (int64_t)x->i32 * INT64_C(1000000);
    if (x->type == -RAY_F32 || x->type == -RAY_F64) return ray_cast_f64_to_i64_null(round(x->f64));
    return as_i64(x);
}

static ray_binary_fn span_fn(uint16_t op) {
    switch (op) {
    case OP_ADD: return ray_add_fn; case OP_SUB: return ray_sub_fn;
    case OP_MUL: return ray_mul_fn; case OP_DIV: return ray_div_fn;
    case OP_IDIV: return ray_idiv_fn; case OP_MOD: return ray_mod_fn;
    case OP_EQ: return ray_eq_fn; case OP_NE: return ray_neq_fn;
    case OP_LT: return ray_lt_fn; case OP_LE: return ray_lte_fn;
    case OP_GT: return ray_gt_fn; case OP_GE: return ray_gte_fn;
    case OP_MIN2: return ray_min2_fn; case OP_MAX2: return ray_max2_fn;
    case SPAN_BUCKET: return ray_xbar_fn;
    default: return NULL;
    }
}

typedef struct {
    int64_t i;
    double f;
    bool missing;
} span_result_t;

static span_result_t span_eval(uint16_t op, ray_t* a, ray_t* b, int8_t out) {
    bool na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    int64_t av = span_value(a, op), bv = span_value(b, op), value = 0;
    if (out == RAY_BOOL) {
        if (a->type == -RAY_TIMESTAMP && !na) {
            av %= INT64_C(86400000000000);
            if (av < 0) av += INT64_C(86400000000000);
        }
        if (b->type == -RAY_TIMESTAMP && !nb) {
            bv %= INT64_C(86400000000000);
            if (bv < 0) bv += INT64_C(86400000000000);
        }
        int cmp;
        if (na || nb) cmp = na == nb ? 0 : na ? -1 : 1;
        else if (a->type == -RAY_F64 || b->type == -RAY_F64 ||
                 a->type == -RAY_F32 || b->type == -RAY_F32) {
            double x = is_numeric(a) ? as_f64(a) : (double)av;
            double y = is_numeric(b) ? as_f64(b) : (double)bv;
            cmp = x < y ? -1 : x > y;
        } else cmp = av < bv ? -1 : av > bv;
        return (span_result_t){.i = (op == OP_EQ ? cmp == 0 : op == OP_NE ? cmp != 0 :
                        op == OP_LT ? cmp < 0 : op == OP_LE ? cmp <= 0 :
                        op == OP_GT ? cmp > 0 : cmp >= 0)};
    }
    if (op == OP_MAX2 && (na || nb) && na != nb)
        return (span_result_t){.i = na ? bv : av};
    if (na || nb) return (span_result_t){.missing = true};
    if (out == RAY_F64) {
        double x = is_numeric(a) ? as_f64(a) : (double)av;
        double y = is_numeric(b) ? as_f64(b) : (double)bv;
        double v = op == OP_ADD ? x + y : op == OP_SUB ? x - y :
                   op == OP_MUL ? x * y : op == OP_DIV ? x / y :
                   op == OP_IDIV ? floor(x / y) : op == SPAN_BUCKET ? y * floor(x / y) : x - y * floor(x / y);
        return (span_result_t){.f = __builtin_isfinite(v) ? v : NULL_F64};
    }
    switch (op) {
    case OP_ADD: value = wrap_add_i64(av, bv); break;
    case OP_SUB: value = (int64_t)((uint64_t)av - (uint64_t)bv); break;
    case OP_MUL: value = wrap_mul_i64(av, bv); break;
    case OP_IDIV: case OP_MOD: case SPAN_BUCKET:
        if (!bv) return (span_result_t){.missing = true};
        value = av / bv;
        if (av % bv && (av < 0) != (bv < 0)) value--;
        if (op == OP_MOD) {
            value = av % bv;
            if (value && (av < 0) != (bv < 0)) value += bv;
        }
        if (op == SPAN_BUCKET) value = wrap_mul_i64(value, bv);
        break;
    case OP_MIN2: value = av < bv ? av : bv; break;
    case OP_MAX2: value = av > bv ? av : bv; break;
    default: return (span_result_t){.missing = true};
    }
    return (span_result_t){.i = value};
}

static ray_t* span_atom(int8_t type, span_result_t value) {
    if (value.missing) return ray_typed_null(-type);
    switch (type) {
    case RAY_BOOL: return ray_bool(value.i != 0);
    case RAY_F64: return ray_f64(value.f);
    case RAY_TIMESPAN: return ray_timespan(value.i);
    case RAY_TIMESTAMP: return ray_timestamp(value.i);
    case RAY_TIME: return ray_time(value.i);
    case RAY_DATE: return ray_date(value.i);
    default: return make_typed_int(-type, value.i);
    }
}

static ray_t span_cell(ray_t* vector, int64_t row) {
    ray_t atom = {.type = (int8_t)-vector->type};
    const void* data = ray_data(vector);
    switch (vector->type) {
    case RAY_BOOL: atom.b8 = ((const uint8_t*)data)[row]; break;
    case RAY_U8: atom.u8 = ((const uint8_t*)data)[row]; break;
    case RAY_I16: atom.i16 = ((const int16_t*)data)[row]; break;
    case RAY_I32: case RAY_DATE: case RAY_TIME: atom.i32 = ((const int32_t*)data)[row]; break;
    case RAY_F32: atom.f64 = ((const float*)data)[row]; break;
    case RAY_F64: atom.f64 = ((const double*)data)[row]; break;
    default: atom.i64 = ((const int64_t*)data)[row]; break;
    }
    return atom;
}

ray_t* ray_timespan_binary(uint16_t op, ray_t* a, ray_t* b) {
    if (a->type == RAY_LIST || b->type == RAY_LIST ||
        RAY_IS_PARTED(a->type) || RAY_IS_PARTED(b->type)) {
        ray_binary_fn fn = span_fn(op);
        return fn ? atomic_map_binary(fn, a, b) : ray_error("type", "unsupported duration operation");
    }
    int8_t type = ray_timespan_binary_type(op, a->type, b->type);
    if (type <= 0) return ray_error("type", "duration operation is not defined for %s and %s",
                                   ray_type_name(a->type), ray_type_name(b->type));
    bool av = ray_is_vec(a), bv = ray_is_vec(b);
    if (!av && !bv) return span_atom(type, span_eval(op, a, b, type));
    if (av && bv && a->len != b->len)
        return ray_error("length", "duration operand lengths must match");
    int64_t n = av ? a->len : b->len;
    ray_t* result = ray_vec_new(type, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    void* data = ray_data(result);
    bool missing = false;
    ray_pool_t* pool = ray_pool_get();
    for (int64_t i = 0; i < n; i++) {
        if ((i & 65535) == 0 && (ray_interrupted() ||
            (pool && atomic_load_explicit(&pool->cancelled, memory_order_acquire)))) {
            ray_release(result);
            return ray_error("cancel", NULL);
        }
        ray_t left, right;
        if (av) left = span_cell(a, i);
        if (bv) right = span_cell(b, i);
        span_result_t value = span_eval(op, av ? &left : a, bv ? &right : b, type);
        if (type == RAY_F64) ((double*)data)[i] = value.missing ? NULL_F64 : value.f;
        else if (type == RAY_BOOL || type == RAY_U8)
            ((uint8_t*)data)[i] = value.missing ? 0 : (uint8_t)value.i;
        else if (type == RAY_I16)
            ((int16_t*)data)[i] = value.missing ? NULL_I16 : (int16_t)value.i;
        else if (type == RAY_I32 || type == RAY_DATE || type == RAY_TIME)
            ((int32_t*)data)[i] = value.missing ? NULL_I32 : (int32_t)value.i;
        else ((int64_t*)data)[i] = value.missing ? NULL_I64 : value.i;
        missing |= ray_raw_is_null(type, 0, data, i);
    }
    if (missing) result->attrs |= RAY_ATTR_HAS_NULLS;
    return result;
}

ray_t* ray_timespan_xbar(ray_t* values, ray_t* width) {
    return ray_timespan_binary(SPAN_BUCKET, values, width);
}

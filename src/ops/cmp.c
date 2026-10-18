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
#include "ops/ops.h"   /* RAY_LAZY, ray_is_lazy, ray_lazy_materialize */
#include "lang/format.h"   /* ray_type_name (error context) */

#include <assert.h>

/* Helper: compare char atom vs string atom.
 * Returns: -1 if no char/string pair, else memcmp-like result via *out. */
int char_str_cmp(ray_t* a, ray_t* b, int *out) {
    const char *ap, *bp;
    size_t al, bl;
    int a_cs = (a->type == -RAY_STR);
    int b_cs = (b->type == -RAY_STR);
    if (!a_cs || !b_cs) return -1;
    ap = ray_str_ptr(a); al = ray_str_len(a);
    bp = ray_str_ptr(b); bl = ray_str_len(b);
    size_t mn = al < bl ? al : bl;
    int c = memcmp(ap, bp, mn);
    if (c != 0) { *out = c; return 0; }
    *out = (al > bl) ? 1 : (al < bl) ? -1 : 0;
    return 0;
}

/* Lexicographic compare of two SYM atoms.  Fast path: equal interned
 * ids ⇒ identical text ⇒ 0, no global-table lookup.  Slow path: pull
 * the backing STR via ray_sym_str and delegate to ray_str_cmp, which
 * uses the 12-byte SSO inline path for short symbols.
 *
 * Invariant: any valid SYM atom resolves to its interned string.  A
 * NULL from ray_sym_str means corruption (uninitialised intern table,
 * out-of-range id, or evicted slot) — no defensible total order exists
 * in that state.  We assert and let the process abort rather than
 * fabricate an answer (returning 0 silently collapses distinct symbols;
 * returning ±1 by raw id invents a non-lexicographic ordering that
 * still lies about the contract).  Matches v1 behaviour, which also
 * trusts the invariant (and would SIGSEGV via strcmp(NULL,...) if it
 * broke). */
int sym_atom_cmp(ray_t* a, ray_t* b) {
    if (a->i64 == b->i64) return 0;
    ray_t* sa = ray_sym_str(a->i64);
    ray_t* sb = ray_sym_str(b->i64);
    assert(sa && sb && "sym_atom_cmp: corrupted intern table — "
                       "valid SYM atom must resolve to interned string");
    int r = ray_str_cmp(sa, sb);
    ray_release(sa);
    ray_release(sb);
    return r;
}

/* Both operands integer-family atoms (I64/I32/I16/U8): order them as
 * int64.  A double is exact only to 2^53, so every i64 above it — any
 * TIMESTAMP, a 64-bit hash, a packed key — would otherwise merge with its
 * neighbours under <, >, <=, >= (and the dyadic min/max built on them) while
 * == already told them apart (#524).  Mixed float/int and BOOL/char pairs
 * keep the double compare: their values are exact in a double. */
static inline int both_int_atoms(const ray_t* a, const ray_t* b) {
    int ia = a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16 || a->type == -RAY_U8;
    int ib = b->type == -RAY_I64 || b->type == -RAY_I32 || b->type == -RAY_I16 || b->type == -RAY_U8;
    return ia && ib;
}

/* Comparison */
ray_t* ray_gt_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c > 0 ? 1 : 0); }
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(sym_atom_cmp(a, b) > 0 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) > 0 ? 1 : 0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return make_bool(RAY_ATOM_IS_NULL(b) && !RAY_ATOM_IS_NULL(a) ? 1 : 0);
        return make_bool(temporal_as_ns(a) > temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(0);       /* null == null → not > */
    if (na) return make_bool(0);             /* null > X → false */
    if (nb) return make_bool(1);             /* X > null → true */
    if (both_int_atoms(a, b)) return make_bool(as_i64(a) > as_i64(b) ? 1 : 0);
    return make_bool(as_f64(a) > as_f64(b) ? 1 : 0);
}

ray_t* ray_lt_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c < 0 ? 1 : 0); }
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(sym_atom_cmp(a, b) < 0 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) < 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return make_bool(RAY_ATOM_IS_NULL(a) && !RAY_ATOM_IS_NULL(b) ? 1 : 0);
        return make_bool(temporal_as_ns(a) < temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(0);       /* null == null → not < */
    if (na) return make_bool(1);             /* null < X → true */
    if (nb) return make_bool(0);             /* X < null → false */
    if (both_int_atoms(a, b)) return make_bool(as_i64(a) < as_i64(b) ? 1 : 0);
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

ray_t* ray_gte_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c >= 0 ? 1 : 0); }
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(sym_atom_cmp(a, b) >= 0 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) >= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(a)) return make_bool(0);
        if (RAY_ATOM_IS_NULL(b)) return make_bool(1);
        return make_bool(temporal_as_ns(a) >= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(1);       /* null == null → >= true */
    if (na) return make_bool(0);             /* null >= X → false */
    if (nb) return make_bool(1);             /* X >= null → true */
    if (both_int_atoms(a, b)) return make_bool(as_i64(a) >= as_i64(b) ? 1 : 0);
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

ray_t* ray_lte_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c <= 0 ? 1 : 0); }
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(sym_atom_cmp(a, b) <= 0 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) <= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(a)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(b)) return make_bool(0);
        return make_bool(temporal_as_ns(a) <= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(1);       /* null == null → <= true */
    if (na) return make_bool(1);             /* null <= X → true */
    if (nb) return make_bool(0);             /* X <= null → false */
    if (both_int_atoms(a, b)) return make_bool(as_i64(a) <= as_i64(b) ? 1 : 0);
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

/* Rank of a numeric atom type in the promotion order min/max use, or -1. */
static int minmax_rank(int8_t t) {
    switch (t) {
    case -RAY_BOOL: return 0;
    case -RAY_U8:   return 1;
    case -RAY_I16:  return 2;
    case -RAY_I32:  return 3;
    case -RAY_I64:  return 4;
    case -RAY_F32:  return 5;
    case -RAY_F64:  return 6;
    default:        return -1;
    }
}

/* The atom type the result of (min a b) / (max a b) takes: the wider of
 * two numeric types (BOOL < U8 < I16 < I32 < I64 < F32 < F64), or
 * TIMESTAMP for a DATE against a TIMESTAMP, so an integer against a float
 * is a float and a boolean against an integer an integer.  0 when the
 * types already agree or no promotion applies. */
static int8_t minmax_type(ray_t* a, ray_t* b) {
    if (a->type == b->type) return 0;
    int ra = minmax_rank(a->type), rb = minmax_rank(b->type);
    if (ra >= 0 && rb >= 0) return ra > rb ? a->type : b->type;
    if ((a->type == -RAY_DATE && b->type == -RAY_TIMESTAMP) ||
        (a->type == -RAY_TIMESTAMP && b->type == -RAY_DATE))
        return -RAY_TIMESTAMP;
    return 0;
}

/* `x` as an atom of type `t` (from minmax_type); a null stays null. */
static ray_t* minmax_promote(ray_t* x, int8_t t) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(t);
    if (t == -RAY_TIMESTAMP) return ray_timestamp(temporal_as_ns(x));
    if (t == -RAY_F64) return make_f64(as_f64(x));
    if (t == -RAY_F32) return ray_f32((float)as_f64(x));
    int64_t v = x->type == -RAY_BOOL ? (int64_t)(x->b8 != 0) : as_i64(x);
    return make_typed_int(t, v);
}

/* Elementwise dyadic overloads for the aggregate names `min` and `max`.
 * Their vectorization is supplied by the evaluator, just like comparisons.
 * The result takes the promoted type of the two operands (minmax_type),
 * whichever of them is chosen. */
static ray_t* minmax_pick(ray_t* a, ray_t* b, ray_t* cmp) {
    if (!cmp || RAY_IS_ERR(cmp)) return cmp ? cmp : ray_error("oom", NULL);
    ray_t* out = cmp->b8 ? a : b;
    ray_release(cmp);
    int8_t t = minmax_type(a, b);
    if (t && out->type != t) return minmax_promote(out, t);
    ray_retain(out);
    return out;
}

ray_t* ray_min2_fn(ray_t* a, ray_t* b) { return minmax_pick(a, b, ray_lte_fn(a, b)); }
ray_t* ray_max2_fn(ray_t* a, ray_t* b) { return minmax_pick(a, b, ray_gte_fn(a, b)); }


ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    /* Handle null forms (RAY_NULL_OBJ, typed null atoms) */
    RAY_ASSERT_VALUE(a); RAY_ASSERT_VALUE(b);
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(1);
    if (na || nb) return make_bool(0);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c == 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 == b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 == b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0 ? 1 : 0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) == temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", "=: incomparable operand types, got %s and %s", ray_type_name(a->type), ray_type_name(b->type));
    if (is_float_op(a, b))
        return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) == as_i64(b) ? 1 : 0);
}

ray_t* ray_neq_fn(ray_t* a, ray_t* b) {
    /* Handle null forms (RAY_NULL_OBJ, typed null atoms) */
    RAY_ASSERT_VALUE(a); RAY_ASSERT_VALUE(b);
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(0);
    if (na || nb) return make_bool(1);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c != 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 != b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 != b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) != 0 ? 1 : 0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) != temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", "<>: incomparable operand types, got %s and %s", ray_type_name(a->type), ray_type_name(b->type));
    if (is_float_op(a, b))
        return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) != as_i64(b) ? 1 : 0);
}

/* Bool vector element-wise helpers to reduce duplication in and/or/not. */
/* op must be a BITWISE operator (& / |): the short-circuit forms compile
 * to a branch per element, which mispredicts on mixed masks (~10x).  The
 * != 0 normalization keeps non-canonical bytes truthy and vectorizes. */
#define BOOL_VEC_BINOP(a, b, op) do {                       \
    int64_t n = a->len < b->len ? a->len : b->len;        \
    ray_t* r = ray_vec_new(RAY_BOOL, n);                   \
    if (RAY_IS_ERR(r)) return r;                           \
    const uint8_t* restrict da = (const uint8_t*)ray_data(a); \
    const uint8_t* restrict db = (const uint8_t*)ray_data(b); \
    uint8_t* restrict dr = (uint8_t*)ray_data(r);          \
    for (int64_t i = 0; i < n; i++)                        \
        dr[i] = (uint8_t)((da[i] != 0) op (db[i] != 0));   \
    r->len = n;                                            \
    return r;                                              \
} while(0)

#define BOOL_VEC_SCALAR_L(vec, sv, op) do {                 \
    int64_t n = vec->len;                                  \
    ray_t* r = ray_vec_new(RAY_BOOL, n);                   \
    if (RAY_IS_ERR(r)) return r;                           \
    bool* dv = (bool*)ray_data(vec);                       \
    bool* dr = (bool*)ray_data(r);                         \
    for (int64_t i = 0; i < n; i++) dr[i] = dv[i] op sv;  \
    r->len = n;                                            \
    return r;                                              \
} while(0)

/* and/or with a collection operand that is not a BOOL vector — a LIST, or
 * a numeric or temporal vector: element by element, never one truth value
 * for the whole collection.  Each pair goes back
 * through `fn`, so a nested list recurses, and a result whose elements
 * are all booleans is a B8 vector.  An atom broadcasts; two collections
 * must have the same length. */
static ray_t* logic_map(ray_binary_fn fn, const char* name, ray_t* a, ray_t* b) {
    int ac = is_collection(a), bc = is_collection(b);
    if (ac && bc && ray_len(a) != ray_len(b))
        return ray_error("length", "%s: operand lengths must match, got %lld and %lld",
                         name, (long long)ray_len(a), (long long)ray_len(b));
    int64_t n = ac ? ray_len(a) : ray_len(b);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t i = 0; i < n; i++) {
        int la = 0, lb = 0;
        ray_t* ea = ac ? collection_elem(a, i, &la) : a;
        ray_t* eb = bc ? collection_elem(b, i, &lb) : b;
        ray_t* r = (RAY_IS_ERR(ea) || RAY_IS_ERR(eb))
                 ? ray_error("type", NULL) : fn(ea, eb);
        if (la) ray_release(ea);
        if (lb) ray_release(eb);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("oom", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    if (n == 0) { ray_release(out); return ray_vec_new(RAY_BOOL, 0); }
    return ray_list_collapse_bool(out);
}

static inline bool is_bool_vec(ray_t* x) {
    return ray_is_vec(x) && x->type == RAY_BOOL;
}

ray_t* ray_and_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (is_bool_vec(a) && is_bool_vec(b)) {
        if (a->len != b->len)
            return ray_error("length", "and: operand lengths must match, got %lld and %lld",
                             (long long)a->len, (long long)b->len);
        BOOL_VEC_BINOP(a, b, &);
    }
    /* Scalar broadcast: vec and scalar */
    if (is_bool_vec(a) && ray_is_atom(b))
        BOOL_VEC_SCALAR_L(a, is_truthy(b), &&);
    if (ray_is_atom(a) && is_bool_vec(b))
        BOOL_VEC_SCALAR_L(b, is_truthy(a), &&);
    if (is_collection(a) || is_collection(b))
        return logic_map(ray_and_fn, "and", a, b);
    return make_bool((is_truthy(a) && is_truthy(b)) ? 1 : 0);
}

ray_t* ray_or_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (is_bool_vec(a) && is_bool_vec(b)) {
        if (a->len != b->len)
            return ray_error("length", "or: operand lengths must match, got %lld and %lld",
                             (long long)a->len, (long long)b->len);
        BOOL_VEC_BINOP(a, b, |);
    }
    /* Scalar broadcast */
    if (is_bool_vec(a) && ray_is_atom(b))
        BOOL_VEC_SCALAR_L(a, is_truthy(b), ||);
    if (ray_is_atom(a) && is_bool_vec(b))
        BOOL_VEC_SCALAR_L(b, is_truthy(a), ||);
    if (is_collection(a) || is_collection(b))
        return logic_map(ray_or_fn, "or", a, b);
    return make_bool((is_truthy(a) || is_truthy(b)) ? 1 : 0);
}

/* Special-form variadic AND/OR with short-circuit (matches v1).
 *
 * `args` are UNEVALUATED AST nodes — registered with RAY_FN_SPECIAL_FORM
 * so the evaluator hands us raw forms rather than computed values.  We
 * call ray_eval per arg ourselves and stop as soon as the result is
 * determined: AND on first scalar falsy, OR on first scalar truthy.
 *
 * Mixed scalar+vector: when the running accumulator becomes a *scalar*
 * with the determining truth value, we return it immediately — same
 * shape as Lisp/Clojure where short-circuit yields the determinant.
 * If the accumulator is a vector we cannot short-circuit (subsequent
 * args may be vectors that still need element-wise combination), so we
 * fall through to ray_and_fn / ray_or_fn for that step. */
static ray_t* eval_and_short(ray_t* arg) {
    ray_t* v = ray_eval(arg);
    if (!v || RAY_IS_ERR(v)) return v;
    if (ray_is_lazy(v)) v = ray_lazy_materialize(v);
    return v;
}

ray_t* ray_and_vary_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "expected at least 1 arg, got %lld", (long long)n);
    ray_t* acc = eval_and_short(args[0]);
    if (!acc || RAY_IS_ERR(acc)) return acc;
    /* Single arg = identity: (and X) == X, (or X) == X — monoid identity
     * rule (Scheme/Haskell).  Enables programmatic AST construction like
     * `(cons 'and preds)` where preds may have length 1. */
    if (n == 1) return acc;
    /* Short-circuit only when the running result is a *scalar* falsy.
     * If acc is a vector, subsequent args still need element-wise
     * combination (so `(and vec false)` broadcasts to all-false vector
     * of acc's shape rather than a bare scalar). */
    if (ray_is_atom(acc) && !is_truthy(acc)) return acc;
    for (int64_t i = 1; i < n; i++) {
        ray_t* v = eval_and_short(args[i]);
        if (!v || RAY_IS_ERR(v)) { ray_release(acc); return v; }
        ray_t* next = ray_and_fn(acc, v);
        ray_release(acc);
        ray_release(v);
        if (!next || RAY_IS_ERR(next)) return next;
        acc = next;
        if (ray_is_atom(acc) && !is_truthy(acc)) return acc;
    }
    return acc;
}

ray_t* ray_or_vary_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "expected at least 1 arg, got %lld", (long long)n);
    ray_t* acc = eval_and_short(args[0]);
    if (!acc || RAY_IS_ERR(acc)) return acc;
    /* Single arg = identity — see ray_and_vary_fn for rationale. */
    if (n == 1) return acc;
    /* Short-circuit only on scalar truthy accumulator (see AND comment). */
    if (ray_is_atom(acc) && is_truthy(acc)) return acc;
    for (int64_t i = 1; i < n; i++) {
        ray_t* v = eval_and_short(args[i]);
        if (!v || RAY_IS_ERR(v)) { ray_release(acc); return v; }
        ray_t* next = ray_or_fn(acc, v);
        ray_release(acc);
        ray_release(v);
        if (!next || RAY_IS_ERR(next)) return next;
        acc = next;
        if (ray_is_atom(acc) && is_truthy(acc)) return acc;
    }
    return acc;
}

/* Unary */
ray_t* ray_not_fn(ray_t* x) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(x) && x->type == RAY_BOOL) {
        int64_t n = x->len;
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* src = (bool*)ray_data(x);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = !src[i];
        r->len = n;
        return r;
    }
    /* Element-wise for numeric and temporal vectors too, as the compiled
     * `not` is: true where the cell is zero or null.  It used to answer one
     * truth value for the whole vector, so a query evaluated through the
     * interpreter disagreed with the same query compiled. */
    if (ray_is_vec(x) && (x->type == RAY_U8 || x->type == RAY_I16 ||
                          x->type == RAY_I32 || x->type == RAY_I64 ||
                          x->type == RAY_F32 || x->type == RAY_F64 ||
                          x->type == RAY_DATE || x->type == RAY_TIME ||
                          x->type == RAY_TIMESTAMP)) {
        int64_t n = x->len;
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        uint8_t* dr = (uint8_t*)ray_data(r);
        const void* src = ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            bool z;
            switch (x->type) {
                case RAY_U8:  z = ((const uint8_t*)src)[i] == 0; break;
                case RAY_I16: z = ((const int16_t*)src)[i] == 0; break;
                case RAY_I32: case RAY_DATE: case RAY_TIME:
                              z = ((const int32_t*)src)[i] == 0; break;
                case RAY_F32: z = ((const float*)src)[i] == 0.0f; break;
                case RAY_F64: z = ((const double*)src)[i] == 0.0; break;
                default:      z = ((const int64_t*)src)[i] == 0; break;
            }
            dr[i] = z || ray_vec_is_null(x, i);
        }
        r->len = n;
        return r;
    }
    /* A LIST, element by element: a nested list recurses, and all-boolean
     * results make a B8 vector. */
    if (is_list(x)) {
        int64_t n = ray_len(x);
        if (n == 0) return ray_vec_new(RAY_BOOL, 0);
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = ray_not_fn(e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("oom", NULL); }
            out = ray_list_append(out, r);
            ray_release(r);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return ray_list_collapse_bool(out);
    }
    return make_bool(is_truthy(x) ? 0 : 1);
}

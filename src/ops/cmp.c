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
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

/* Check if comparable (numeric or temporal) */
int is_comparable(ray_t* x) {
    return is_numeric(x) || is_temporal(x);
}

ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    /* Handle all null forms (C NULL, RAY_NULL_OBJ, typed null atoms) */
    int na = (!a || RAY_ATOM_IS_NULL(a)), nb = (!b || RAY_ATOM_IS_NULL(b));
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
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) == as_i64(b) ? 1 : 0);
}

ray_t* ray_neq_fn(ray_t* a, ray_t* b) {
    /* Handle all null forms (C NULL, RAY_NULL_OBJ, typed null atoms) */
    int na = (!a || RAY_ATOM_IS_NULL(a)), nb = (!b || RAY_ATOM_IS_NULL(b));
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
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) != as_i64(b) ? 1 : 0);
}

/* Bool vector element-wise helpers to reduce duplication in and/or/not. */
#define BOOL_VEC_BINOP(a, b, op) do {                       \
    int64_t n = a->len < b->len ? a->len : b->len;        \
    ray_t* r = ray_vec_new(RAY_BOOL, n);                   \
    if (RAY_IS_ERR(r)) return r;                           \
    bool* da = (bool*)ray_data(a);                         \
    bool* db = (bool*)ray_data(b);                         \
    bool* dr = (bool*)ray_data(r);                         \
    for (int64_t i = 0; i < n; i++) dr[i] = da[i] op db[i]; \
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

ray_t* ray_and_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_vec(b) && b->type == RAY_BOOL)
        BOOL_VEC_BINOP(a, b, &&);
    /* Scalar broadcast: vec and scalar */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_atom(b))
        BOOL_VEC_SCALAR_L(a, is_truthy(b), &&);
    if (ray_is_atom(a) && ray_is_vec(b) && b->type == RAY_BOOL)
        BOOL_VEC_SCALAR_L(b, is_truthy(a), &&);
    return make_bool((is_truthy(a) && is_truthy(b)) ? 1 : 0);
}

ray_t* ray_or_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_vec(b) && b->type == RAY_BOOL)
        BOOL_VEC_BINOP(a, b, ||);
    /* Scalar broadcast */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_atom(b))
        BOOL_VEC_SCALAR_L(a, is_truthy(b), ||);
    if (ray_is_atom(a) && ray_is_vec(b) && b->type == RAY_BOOL)
        BOOL_VEC_SCALAR_L(b, is_truthy(a), ||);
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
    if (n < 2) return ray_error("arity", "expected at least 2 args, got %lld", (long long)n);
    ray_t* acc = eval_and_short(args[0]);
    if (!acc || RAY_IS_ERR(acc)) return acc;
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
    if (n < 2) return ray_error("arity", "expected at least 2 args, got %lld", (long long)n);
    ray_t* acc = eval_and_short(args[0]);
    if (!acc || RAY_IS_ERR(acc)) return acc;
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
    return make_bool(is_truthy(x) ? 0 : 1);
}

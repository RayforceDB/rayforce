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
#include "ops/internal.h" /* ray_table_col_value */
#include "lang/format.h"   /* ray_type_name (error context) */

#include <assert.h>
#include "ops/hash.h"

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
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_GT, a, b);
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
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_LT, a, b);
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
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_GE, a, b);
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
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_LE, a, b);
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

/* The atom type the result of (min a b) / (max a b) takes: the promotion
 * table's (ray_num_type, core/types.h), so an integer against a float is a
 * float, F32 against anything else F64, and a boolean against an integer an
 * integer; or TIMESTAMP for a DATE against a TIMESTAMP.  0 when the types
 * already agree or no promotion applies. */
static int8_t minmax_type(ray_t* a, ray_t* b) {
    if (a->type == b->type) return 0;
    int8_t t = ray_num_type((int8_t)-a->type, (int8_t)-b->type);
    if (t) return (int8_t)-t;
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


/* Use the language column accessor: stored PARTED and MAPCOMMON columns
 * need flattening before ordinary equality kernels can consume them. */
static ray_t* equality_column(ray_t* table, int64_t c) {
    return ray_table_col_value(table, ray_table_col_name(table, c));
}

/* LIST keys include xkey's composite rows. Hash recursively with the same
 * numeric coercion as atom_eq; uncommon container keys share a safe bucket. */
static uint64_t eq_key_hash(ray_t* x, bool durations) {
    if (RAY_ATOM_IS_NULL(x)) return 0;
    if (is_numeric(x)) return ray_hash_f64(durations ? round(as_f64(x)) : as_f64(x));
    if (x->type == -RAY_SYM) return ray_hash_i64(x->i64);
    if (x->type == -RAY_TIMESTAMP || x->type == -RAY_TIMESPAN)
        return durations ? ray_hash_f64((double)x->i64) : ray_hash_i64(x->i64);
    if (x->type == -RAY_TIME)
        return durations ? ray_hash_f64((double)((int64_t)x->i32 * 1000000)) : ray_hash_i64(x->i32);
    if (x->type == -RAY_DATE) return ray_hash_i64(x->i32);
    if (x->type == -RAY_GUID)
        return ray_hash_bytes(ray_data(x->obj), 16);
    if (x->type == -RAY_STR) return ray_hash_bytes(ray_str_ptr(x), ray_str_len(x));
    if (x->type == RAY_LIST) {
        uint64_t h = ray_hash_i64(x->len);
        ray_t** cells = ray_data(x);
        for (int64_t i = 0; i < x->len; i++)
            h = ray_hash_i64((int64_t)(h ^ eq_key_hash(cells[i], durations)));
        return h;
    }
    return 0;
}

/* Duration equality rounds numeric operands and converts TIME to ns.
 * Detect it before hashing so both key sets use that normalization. */
static bool eq_key_has_span(ray_t* x) {
    if (x->type == -RAY_TIMESPAN) return true;
    if (x->type == RAY_LIST) {
        ray_t** cells = ray_data(x);
        for (int64_t i = 0; i < x->len; i++)
            if (eq_key_has_span(cells[i])) return true;
    }
    return false;
}

static ray_t* eq_key_find(ray_t* keys, ray_t* probes) {
    if (keys->type != RAY_LIST) return ray_find_fn(keys, probes);
    bool durations = eq_key_has_span(keys) || eq_key_has_span(probes);
    int64_t cap = 8;
    while (cap / 2 < keys->len) {
        if (cap > INT64_MAX / 2) return ray_error("oom", NULL);
        cap *= 2;
    }
    ray_t* slots = ray_vec_new(RAY_I64, cap);
    if (!slots || RAY_IS_ERR(slots)) return slots;
    int64_t* index = ray_data(slots);
    for (int64_t i = 0; i < cap; i++) index[i] = -1;
    ray_t** data = ray_data(keys);
    for (int64_t i = 0; i < keys->len; i++) {
        uint64_t at = eq_key_hash(data[i], durations) & (cap - 1);
        while (index[at] >= 0 && !atom_eq(data[index[at]], data[i]))
            at = (at + 1) & (cap - 1);
        if (index[at] < 0) index[at] = i;
    }
    ray_t* out = ray_vec_new(RAY_I64, probes->len);
    if (!out || RAY_IS_ERR(out)) { ray_release(slots); return out; }
    out->len = probes->len;
    int64_t* result = ray_data(out);
    ray_t** needle = ray_data(probes);
    for (int64_t i = 0; i < probes->len; i++) {
        uint64_t at = eq_key_hash(needle[i], durations) & (cap - 1);
        while (index[at] >= 0 && !atom_eq(data[index[at]], needle[i]))
            at = (at + 1) & (cap - 1);
        result[i] = index[at] < 0 ? NULL_I64 : index[at];
    }
    ray_release(slots);
    return out;
}

/* Keep equality's container shape while delegating cells to the existing
 * atomic mapper, including numeric promotion and nested lists. */
static ray_t* eq_container(ray_t* a, ray_t* b) {
    bool tables = a->type == RAY_TABLE || b->type == RAY_TABLE;
    ray_t* shape = tables ? (a->type == RAY_TABLE ? a : b)
                          : (a->type == RAY_DICT ? a : b);
    ray_t* other = shape == a ? b : a;
    if (tables) {
        if (other->type != RAY_TABLE && !ray_is_atom(other))
            return ray_error("type", "==: table expects a table or atom");
        int64_t nc = ray_table_ncols(shape);
        if (other->type == RAY_TABLE) {
            if (nc != ray_table_ncols(other) || ray_table_nrows(shape) != ray_table_nrows(other))
                return ray_error("length", "==: table shapes differ");
            for (int64_t c = 0; c < nc; c++)
                if (ray_table_col_name(shape, c) != ray_table_col_name(other, c))
                    return ray_error("length", "==: table column names differ");
        }
        ray_t* out = ray_table_new(nc);
        if (!out || RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* x = equality_column(shape, c);
            ray_t* y = other->type == RAY_TABLE ? equality_column(other, c) : other;
            if (other->type != RAY_TABLE) ray_retain(y);
            if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) {
                ray_release(out);
                ray_t* err = RAY_IS_ERR(x) ? x : RAY_IS_ERR(y) ? y : ray_error("type", "==: unreadable column");
                if (x && x != err) ray_release(x);
                if (y && y != err) ray_release(y);
                return err;
            }
            ray_t* col = atomic_map_binary_op(ray_eq_fn, OP_EQ, x, y);
            ray_release(x); ray_release(y);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col; }
            out = ray_table_add_col(out, ray_table_col_name(shape, c), col);
            ray_release(col);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (other->type != RAY_DICT && !ray_is_atom(other))
        return ray_error("type", "==: dictionary expects a dictionary or atom");
    ray_t* keys = ray_dict_keys(shape);
    ray_t* vals = ray_dict_vals(shape);
    ray_t* more = other->type == RAY_DICT ? ray_dict_keys(other) : NULL;
    if (more && (!keys->len || !more->len)) {
        ray_t* union_keys = keys->len ? keys : more;
        ray_t* no = ray_vec_new(RAY_BOOL, union_keys->len);
        if (!no || RAY_IS_ERR(no)) return no;
        no->len = union_keys->len;
        memset(ray_data(no), 0, (size_t)no->len);
        ray_t* values = to_boxed_list(no);
        ray_release(no);
        ray_retain(union_keys);
        return ray_dict_new(union_keys, values);
    }
    if (more && keys->type != more->type)
        return ray_error("type", "==: dictionary key types must match");
    if (more) {
        ray_t* same = ray_identical_fn(keys, more);
        if (!same || RAY_IS_ERR(same)) return same;
        bool aligned = same->b8;
        ray_release(same);
        if (aligned) {
            ray_t* values = atomic_map_binary_op(ray_eq_fn, OP_EQ, vals, ray_dict_vals(other));
            if (!values || RAY_IS_ERR(values)) return values;
            if (values->type != RAY_LIST) {
                ray_t* boxed = to_boxed_list(values);
                ray_release(values);
                values = boxed;
            }
            ray_retain(keys);
            return ray_dict_new(keys, values);
        }
    }
    ray_t* matches = more ? eq_key_find(more, keys) : NULL;
    if (matches && RAY_IS_ERR(matches)) return matches;
    ray_t* reverse = more ? eq_key_find(keys, more) : NULL;
    if (reverse && RAY_IS_ERR(reverse)) { ray_release(matches); return reverse; }
    ray_t* result = ray_list_new(keys->len);
    ray_t* outkeys = keys;
    ray_retain(outkeys);
    if (!result || RAY_IS_ERR(result) || !outkeys || RAY_IS_ERR(outkeys)) {
        ray_release(matches); ray_release(reverse);
        ray_t* err = RAY_IS_ERR(result) ? result : RAY_IS_ERR(outkeys) ? outkeys : ray_error("oom", NULL);
        if (result != err) ray_release(result);
        if (outkeys != err) ray_release(outkeys);
        return err;
    }
    ray_t* err = NULL;
    for (int64_t i = 0; i < keys->len; i++) {
        int ov = 0, oy = 0;
        ray_t* x = collection_elem(vals, i, &ov);
        int64_t j = more ? ((int64_t*)ray_data(matches))[i] : 0;
        ray_t* y = more && j != NULL_I64 ? collection_elem(ray_dict_vals(other), j, &oy) : other;
        ray_t* value = RAY_IS_ERR(x) ? x : RAY_IS_ERR(y) ? y :
            more && j == NULL_I64 ? make_bool(0) : atomic_map_binary_op(ray_eq_fn, OP_EQ, x, y);
        if (ov && x != value) ray_release(x);
        if (oy && y != value) ray_release(y);
        if (!value || RAY_IS_ERR(value)) {
            err = value ? value : ray_error("oom", NULL);
            break;
        }
        result = ray_list_append(result, value);
        ray_release(value);
        if (RAY_IS_ERR(result) || RAY_IS_ERR(outkeys)) break;
    }
    for (int64_t i = 0; !err && !RAY_IS_ERR(result) && !RAY_IS_ERR(outkeys) && more && i < more->len; i++) {
        if (((int64_t*)ray_data(reverse))[i] != NULL_I64) continue;
        ray_t* no = make_bool(0);
        result = ray_list_append(result, no);
        ray_release(no);
    }
    if (!err && more) {
        ray_t* added = ray_vec_new(RAY_I64, more->len);
        if (!added || RAY_IS_ERR(added)) err = added ? added : ray_error("oom", NULL);
        else {
            int64_t* rows = ray_data(added);
            for (int64_t i = 0; i < more->len; i++)
                if (((int64_t*)ray_data(reverse))[i] == NULL_I64) rows[added->len++] = i;
            ray_t* extra = ray_at_fn(more, added);
            ray_release(added);
            if (!extra || RAY_IS_ERR(extra)) err = extra ? extra : ray_error("oom", NULL);
            else {
                ray_t* combined = ray_concat_fn(keys, extra);
                ray_release(extra);
                ray_release(outkeys);
                outkeys = combined;
            }
        }
    }
    ray_release(matches); ray_release(reverse);
    if (err) { ray_release(result); ray_release(outkeys); return err; }
    return ray_dict_new(outkeys, result);
}

ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    if (a->type == RAY_DICT || b->type == RAY_DICT || a->type == RAY_TABLE || b->type == RAY_TABLE)
        return eq_container(a, b);
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_EQ, a, b);
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
    if (ray_has_timespan(a) || ray_has_timespan(b)) return ray_timespan_binary(OP_NE, a, b);
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
 * determined: AND on first scalar falsy, OR on first scalar truthy.  A
 * multi-argument scalar result is always BOOL; the determining operand's
 * original type must not leak through the short-circuit path.
 *
 * Mixed scalar+vector: when the running accumulator becomes a *scalar*
 * with the determining truth value, we return its normalized boolean.
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
    if (ray_is_atom(acc) && !is_truthy(acc)) {
        if (acc->type == -RAY_BOOL) return acc;
        ray_release(acc);
        return make_bool(0);
    }
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
    if (ray_is_atom(acc) && is_truthy(acc)) {
        if (acc->type == -RAY_BOOL) return acc;
        ray_release(acc);
        return make_bool(1);
    }
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
                          x->type == RAY_TIMESTAMP || x->type == RAY_TIMESPAN)) {
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
    /* `not` has no meaning for the cells of a STR, SYM or GUID vector.  It
     * used to fall through to the atom rule and answer one boolean for the
     * whole vector; raise instead, as the compiled `not` does. */
    if (ray_is_vec(x) && (x->type == RAY_STR || x->type == RAY_SYM ||
                          x->type == RAY_GUID))
        return ray_error("type", "not: expects a numeric argument, got %s",
                         ray_type_name(x->type));
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

/* Whole-value equality deliberately does not use numeric promotion. */
ray_t* ray_identical_fn(ray_t* a, ray_t* b) {
    if (!a || !b) return ray_error("type", "identical: missing value");
    if (RAY_IS_ERR(a)) { ray_retain(a); return a; }
    if (RAY_IS_ERR(b)) { ray_retain(b); return b; }
    if (ray_is_lazy(a) || ray_is_lazy(b)) {
        ray_retain(a); ray_retain(b);
        ray_t* x = ray_is_lazy(a) ? ray_lazy_materialize(a) : a;
        ray_t* y = ray_is_lazy(b) ? ray_lazy_materialize(b) : b;
        if (!x || RAY_IS_ERR(x)) { ray_release(y); return x; }
        if (!y || RAY_IS_ERR(y)) { ray_release(x); return y; }
        ray_t* result = ray_identical_fn(x, y);
        ray_release(x); ray_release(y);
        return result;
    }
    if (a->type != b->type) return make_bool(0);
    if (a == b) return make_bool(1);
    if (a->type >= RAY_LAMBDA) {
        if (a->type == RAY_UNARY || a->type == RAY_BINARY || a->type == RAY_VARY)
            return make_bool(a->i64 == b->i64);
        return make_bool(0);
    }
    if (a->type == RAY_DICT) {
        ray_t* keys = ray_identical_fn(ray_dict_keys(a), ray_dict_keys(b));
        if (!keys || RAY_IS_ERR(keys) || !keys->b8) return keys;
        ray_release(keys);
        return ray_identical_fn(ray_dict_vals(a), ray_dict_vals(b));
    }
    if (a->type == RAY_TABLE) {
        if (ray_table_ncols(a) != ray_table_ncols(b) ||
            ray_table_nrows(a) != ray_table_nrows(b)) return make_bool(0);
        for (int64_t c = 0; c < ray_table_ncols(a); c++) {
            if (ray_table_col_name(a, c) != ray_table_col_name(b, c)) return make_bool(0);
            ray_t* x = equality_column(a, c);
            ray_t* y = equality_column(b, c);
            if (RAY_IS_ERR(x)) { if (y) ray_release(y); return x; }
            if (RAY_IS_ERR(y)) { if (x) ray_release(x); return y; }
            if (!x || !y) { ray_release(x); ray_release(y); return ray_error("type", "identical: unreadable column"); }
            ray_t* equal = ray_identical_fn(x, y);
            ray_release(x); ray_release(y);
            if (!equal || RAY_IS_ERR(equal) || !equal->b8) return equal;
            ray_release(equal);
        }
        return make_bool(1);
    }
    if (ray_is_vec(a)) {
        if (a->len != b->len) return make_bool(0);
        if (!ray_elem_size(a->type))
            return ray_error("type", "identical: unreadable vector type");
        const unsigned char* ad = ray_data(a);
        const unsigned char* bd = ray_data(b);
        bool nulls = ray_vec_may_have_nulls(a) || ray_vec_may_have_nulls(b);
        if (!nulls && a->type != RAY_SYM && a->type != RAY_STR &&
            a->type != RAY_F32 && a->type != RAY_F64)
            return make_bool(memcmp(ad, bd, (size_t)a->len * ray_elem_size(a->type)) == 0);
        for (int64_t i = 0; i < a->len; i++) {
            if (nulls) {
                bool an = ray_vec_is_null(a, i), bn = ray_vec_is_null(b, i);
                if (an != bn) return make_bool(0);
                if (an) continue;
            }
            bool same;
            if (a->type == RAY_F64) {
                const double* xa = ray_data(a), *xb = ray_data(b);
                double x = xa[i], y = xb[i];
                same = x == y || (isnan(x) && isnan(y));
            } else if (a->type == RAY_F32) {
                const float* xa = ray_data(a), *xb = ray_data(b);
                float x = xa[i], y = xb[i];
                same = x == y || (isnan(x) && isnan(y));
            } else if (a->type == RAY_SYM) {
                same = sym_cell_runtime_id(a, i) == sym_cell_runtime_id(b, i);
            } else if (a->type == RAY_STR) {
                size_t al, bl;
                const char* ap = ray_str_vec_get(a, i, &al);
                const char* bp = ray_str_vec_get(b, i, &bl);
                if (!ap || !bp) return ray_error("type", "identical: unreadable string");
                same = al == bl && memcmp(ap, bp, al) == 0;
            } else {
                size_t size = ray_elem_size(a->type);
                same = memcmp(ad + i * size, bd + i * size, size) == 0;
            }
            if (!same) return make_bool(0);
        }
        return make_bool(1);
    }
    if (a->type == RAY_LIST) {
        if (a->len != b->len) return make_bool(0);
        for (int64_t i = 0; i < a->len; i++) {
            int oa = 0, ob = 0;
            ray_t* x = collection_elem(a, i, &oa);
            ray_t* y = collection_elem(b, i, &ob);
            ray_t* equal = ray_identical_fn(x, y);
            if (oa) ray_release(x);
            if (ob) ray_release(y);
            if (!equal || RAY_IS_ERR(equal)) return equal;
            int yes = equal->b8;
            ray_release(equal);
            if (!yes) return make_bool(0);
        }
        return make_bool(1);
    }
    return ray_eq_fn(a, b);
}

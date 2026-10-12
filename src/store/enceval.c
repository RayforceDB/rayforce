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

#include "store/enceval.h"
#include <math.h>
#include <string.h>

static inline int cmp_one(int64_t x, uint16_t op, int64_t c) {
    switch (op) {
    case OP_EQ: return x == c; case OP_NE: return x != c;
    case OP_LT: return x < c;  case OP_LE: return x <= c;
    case OP_GT: return x > c;  case OP_GE: return x >= c;
    default:    return 0;
    }
}
/* the predicate comparator on doubles: -0.0 == +0.0; a NaN never reaches
 * here (it is the null, in the bitmap) */
static inline int cmp_one_f(double x, uint16_t op, double c) {
    switch (op) {
    case OP_EQ: return x == c; case OP_NE: return x != c;
    case OP_LT: return x < c;  case OP_LE: return x <= c;
    case OP_GT: return x > c;  case OP_GE: return x >= c;
    default:    return 0;
    }
}
static inline uint64_t lane(const ray_enc_lanes_t* l, int T, uint32_t i) {
    switch (T) { case 8: return l->b[i]; case 16: return l->h[i]; case 32: return l->s[i]; default: return l->q[i]; }
}
static inline bool is_float(int8_t t) { return t == RAY_F32 || t == RAY_F64; }
static inline bool is_cmp_op(uint16_t op) { return op >= OP_EQ && op <= OP_GE; }

void ray_enc_mask_nulls(const ray_enc_vview_t* v, uint8_t nr, uint8_t* bits) {
    if (!v->nullmap) return;
    for (int i = 0; i < v->n; i++)
        if ((v->nullmap[i >> 3] >> (i & 7)) & 1) bits[i] = nr;
}

int ray_enc_const_decision(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t nr) {
    if (v->enc != RAY_ENC_CONST || is_float(v->type) || !is_cmp_op(op)) return -1;
    if (v->type == RAY_SYM && op != OP_EQ && op != OP_NE) return -1;   /* never order ids (#738) */
    if (v->nullmap && v->n_nulls == v->n) return nr;          /* all null: every row is null_result */
    int r = cmp_one(ray_enc_logical(v->cbits, v->k), op, c);
    if (!v->nullmap || r == nr) return r;
    return -1;
}

int ray_enc_const_decision_f64(const ray_enc_vview_t* v, uint16_t op, double c, uint8_t nr) {
    if (v->enc != RAY_ENC_CONST || !is_float(v->type) || !is_cmp_op(op)) return -1;
    if (isnan(c)) return -1;                                  /* a NaN constant is the null constant: decline */
    if (v->nullmap && v->n_nulls == v->n) return nr;
    double x;
    if (v->type == RAY_F64) memcpy(&x, &v->cbits, 8);
    else { float f; uint32_t b = (uint32_t)v->cbits; memcpy(&f, &b, 4); x = f; }
    int r = cmp_one_f(x, op, c);
    if (!v->nullmap || r == nr) return r;
    return -1;
}

/* FOR_BP: x = logical(base) + u with 0 <= u <= umax and no wrap in logical
 * space (the view checked it on the actual residuals), so x op c <=> u op t
 * with t = c - logical(base), computed in 128 bits; t outside [0, umax]
 * decides every non-null row alike.  The residuals come from the lane
 * kernels at width T and compare as unsigned T-bit values. */
#define EVAL_RES(UT) do { const UT* u_ = (const UT*)res; const UT t_ = (UT)tt;               \
    switch (op) {                                                                             \
    case OP_EQ: for (int i = 0; i < v->n; i++) bits[i] = u_[i] == t_; break;                  \
    case OP_NE: for (int i = 0; i < v->n; i++) bits[i] = u_[i] != t_; break;                  \
    case OP_LT: for (int i = 0; i < v->n; i++) bits[i] = u_[i] <  t_; break;                  \
    case OP_LE: for (int i = 0; i < v->n; i++) bits[i] = u_[i] <= t_; break;                  \
    case OP_GT: for (int i = 0; i < v->n; i++) bits[i] = u_[i] >  t_; break;                  \
    case OP_GE: for (int i = 0; i < v->n; i++) bits[i] = u_[i] >= t_; break;                  \
    default:    memset(bits, 0, (size_t)v->n); break; } } while (0)

static void eval_for(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t* bits) {
    const uint64_t umax = v->width >= 64 ? ~0ull : ((1ull << v->width) - 1);
    /* one formula for every kind: the only unsigned 64-bit kind is SYM W64,
     * whose ids (and so its base) are below 2^32, where logical(base) is
     * the base itself; a negative c then lands in t < 0, which is right
     * for EQ / NE, the only operators SYM reaches */
    const __int128 t = (__int128)c - (__int128)ray_enc_logical(v->base, v->k);
    if (t < 0 || t > (__int128)umax) {
        memset(bits, cmp_one(t < 0 ? 1 : -1, op, 0), (size_t)v->n);   /* x > c, or x < c, for every row */
        return;
    }
    const uint64_t tt = (uint64_t)t;
    _Alignas(64) ray_enc_lanes_t lanes;                       /* the residuals, on the stack */
    const void* res = &lanes;
    ray_enccol_unpack_resid(v, &lanes);
    switch (v->k.T) {
    case 8:  EVAL_RES(uint8_t);  break;
    case 16: EVAL_RES(uint16_t); break;
    case 32: EVAL_RES(uint32_t); break;
    default: EVAL_RES(uint64_t); break;
    }
}
#undef EVAL_RES

/* DICT: VALUE_DICT is strictly ascending in native order (the view admits
 * signed integer kinds only); the passing codes form one interval [lo, hi)
 * or its complement (NE). */
static uint32_t lower_bound(const ray_enc_vview_t* v, int64_t c) {
    uint32_t lo = 0, hi = v->dict_n;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (ray_enc_logical(v->dict[mid], v->k) < c) lo = mid + 1; else hi = mid; }
    return lo;
}
static uint32_t upper_bound(const ray_enc_vview_t* v, int64_t c) {
    uint32_t lo = 0, hi = v->dict_n;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (ray_enc_logical(v->dict[mid], v->k) <= c) lo = mid + 1; else hi = mid; }
    return lo;
}
static void eval_dict(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t* bits) {
    const uint32_t lb = lower_bound(v, c), ub = upper_bound(v, c);
    uint32_t lo = 0, hi = 0; uint8_t neg = 0;
    switch (op) {
    case OP_EQ: lo = lb; hi = ub; break;
    case OP_NE: lo = lb; hi = ub; neg = 1; break;
    case OP_LT: lo = 0;  hi = lb; break;
    case OP_LE: lo = 0;  hi = ub; break;
    case OP_GT: lo = ub; hi = v->dict_n; break;
    case OP_GE: lo = lb; hi = v->dict_n; break;
    default: break;
    }
    const uint32_t span = hi - lo;
    for (int i = 0; i < v->n; i++)
        bits[i] = (uint8_t)((v->codes[i] - lo < span) ^ neg);   /* unsigned: one compare */
}

/* x in set: the set sorted ascending, distinct */
static bool in_set(const int64_t* set, int nset, int64_t x) {
    int lo = 0, hi = nset;
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (set[mid] < x) lo = mid + 1; else hi = mid; }
    return lo < nset && set[lo] == x;
}
/* RLE: each run's value decides its rows (CONST takes one memset), under
 * a comparison or a membership. */
typedef struct { bool member; uint16_t op; int64_t c; const int64_t* set; int nset; } run_pred_t;
static inline int run_pred(const run_pred_t* p, int64_t x) {
    return p->member ? in_set(p->set, p->nset, x) : cmp_one(x, p->op, p->c);
}
/* ray_enc_logical, inlined for the per-run loop */
static inline int64_t logical_of(uint64_t bits, ray_enc_kind_t k) {
    if (k.T == 64) return (int64_t)bits;
    const uint64_t m = (1ull << k.T) - 1;
    bits &= m;
    return k.is_signed && (bits >> (k.T - 1)) & 1 ? (int64_t)(bits | ~m) : (int64_t)bits;
}
/* bits[i, e) = b in 8-byte stores (runs are often a few rows: a memset
 * call per run costs more than the run).  A store may reach past e, never
 * past n: the next run, which starts at e, overwrites those bytes. */
static inline void fill_run(uint8_t* bits, uint32_t i, uint32_t e, uint32_t n, uint8_t b) {
    const uint64_t pat = 0x0101010101010101ull * b;
    for (; i + 8 <= e; i += 8) memcpy(bits + i, &pat, 8);
    if (i < e) {
        if (i + 8 <= n) memcpy(bits + i, &pat, 8);
        else for (; i < e; i++) bits[i] = b;
    }
}
static void eval_runs(const ray_enc_vview_t* v, const run_pred_t* p, uint8_t* bits) {
    uint32_t i = 0;
    for (uint32_t r = 0; r < v->n_runs; r++) {
        const uint8_t b = (uint8_t)run_pred(p, logical_of(lane(&v->run_vals, v->k.T, r), v->k));
        const uint32_t e = v->run_ends.h[r];
        fill_run(bits, i, e, (uint32_t)v->n, b);
        i = e;
    }
}

/* Some run of v holds the logical value c: its T-bit pattern compared
 * against the run values, one pass (no per-run fill), for equality's
 * all-fail case.  A value outside the kind's range is held by none. */
#define RUNS_HOLD(UT, F) do { const UT x_ = (UT)pat; bool hit_ = false;               \
        for (uint32_t r = 0; r < v->n_runs; r++) hit_ |= v->run_vals.F[r] == x_;      \
        return hit_; } while (0)
static bool runs_hold(const ray_enc_vview_t* v, int64_t c) {
    const int T = v->k.T;
    if (T < 64) {
        const int64_t lo = v->k.is_signed ? -((int64_t)1 << (T - 1)) : 0;
        const int64_t hi = v->k.is_signed ? ((int64_t)1 << (T - 1)) - 1 : (int64_t)((1ull << T) - 1);
        if (c < lo || c > hi) return false;
    } else if (!v->k.is_signed && c < 0) {
        return false;
    }
    const uint64_t pat = T == 64 ? (uint64_t)c : (uint64_t)c & ((1ull << T) - 1);
    switch (T) {
    case 8:  RUNS_HOLD(uint8_t, b);
    case 16: RUNS_HOLD(uint16_t, h);
    case 32: RUNS_HOLD(uint32_t, s);
    default: RUNS_HOLD(uint64_t, q);
    }
}
#undef RUNS_HOLD

bool ray_enc_eval_cmp(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t nr, uint8_t* bits) {
    if (is_float(v->type)) return false;                              /* floats: ray_enc_const_decision_f64 */
    if (!is_cmp_op(op)) return false;
    if (v->type == RAY_SYM && op != OP_EQ && op != OP_NE) return false;   /* never order ids (#738) */
    switch (v->enc) {
    case RAY_ENC_CONST:
        memset(bits, cmp_one(ray_enc_logical(v->cbits, v->k), op, c), (size_t)v->n);
        break;
    case RAY_ENC_FOR_BP: eval_for(v, op, c, bits); break;
    case RAY_ENC_RLE:
        if ((op == OP_EQ || op == OP_NE) && !runs_hold(v, c)) memset(bits, op == OP_NE, (size_t)v->n);
        else eval_runs(v, &(run_pred_t){ false, op, c, NULL, 0 }, bits);
        break;
    case RAY_ENC_DICT:   eval_dict(v, op, c, bits); break;
    default: return false;
    }
    ray_enc_mask_nulls(v, nr, bits);
    return true;
}

bool ray_enc_eval_in(const ray_enc_vview_t* v, const int64_t* set, int nset, uint8_t nr, uint8_t* bits) {
    if (is_float(v->type)) return false;
    if (nset < 0) nset = 0;
    switch (v->enc) {
    case RAY_ENC_CONST:
        memset(bits, in_set(set, nset, ray_enc_logical(v->cbits, v->k)), (size_t)v->n);
        break;
    case RAY_ENC_RLE: eval_runs(v, &(run_pred_t){ true, 0, 0, set, nset }, bits); break;
    case RAY_ENC_DICT: {
        /* the codes of the set's members: a bitmap over dict_n <= 2^16 codes (8 KB) */
        uint64_t pass[(1u << 16) / 64];
        memset(pass, 0, ((size_t)v->dict_n + 63) / 64 * 8);
        for (int s = 0; s < nset; s++) {
            const uint32_t lb = lower_bound(v, set[s]);
            if (lb < v->dict_n && ray_enc_logical(v->dict[lb], v->k) == set[s]) pass[lb >> 6] |= 1ull << (lb & 63);
        }
        for (int i = 0; i < v->n; i++) bits[i] = (uint8_t)((pass[v->codes[i] >> 6] >> (v->codes[i] & 63)) & 1);
        break;
    }
    case RAY_ENC_FOR_BP: {
        /* x = logical(base) + u with no wrap (the view's guarantee), so the
         * wrapping 64-bit sum is the logical value exactly */
        const uint64_t lb = (uint64_t)ray_enc_logical(v->base, v->k);
        _Alignas(64) ray_enc_lanes_t lanes;                   /* the residuals, on the stack */
        ray_enccol_unpack_resid(v, &lanes);
        for (int i = 0; i < v->n; i++)
            bits[i] = (uint8_t)in_set(set, nset, (int64_t)(lb + lane(&lanes, v->k.T, (uint32_t)i)));
        break;
    }
    default: return false;
    }
    ray_enc_mask_nulls(v, nr, bits);
    return true;
}

/* CONST: n_nonnull * logical(value) (0 when every row is null); FOR_BP:
 * n_nonnull * logical(base) + the non-null residuals (no wrap, the view's
 * guarantee; the writer fills null slots with the base, and the residual
 * sum skips them all the same); RLE without nulls: logical(value) * run
 * length per run. */
bool ray_enc_vec_sum(const ray_enc_vview_t* v, int64_t* hi, uint64_t* lo, int64_t* nn) {
    switch (v->type) {
    case RAY_I16: case RAY_I32: case RAY_I64: case RAY_TIME: break;
    case RAY_BOOL: case RAY_U8: if (v->n_nulls) return false; break;
    default: return false;
    }
    __int128 s = 0;
    const int64_t n_nonnull = (int64_t)v->n - v->n_nulls;
    switch (v->enc) {
    case RAY_ENC_CONST:
        if (n_nonnull) s = (__int128)n_nonnull * ray_enc_logical(v->cbits, v->k);
        break;
    case RAY_ENC_FOR_BP: {
        _Alignas(64) ray_enc_lanes_t u;                      /* the residuals, on the stack */
        ray_enccol_unpack_resid(v, &u);
        const uint8_t* nm = v->n_nulls ? v->nullmap : NULL;
        __int128 r = 0;
        if (v->k.T == 64) {
            if (nm) { for (int i = 0; i < v->n; i++) if (!((nm[i >> 3] >> (i & 7)) & 1)) r += u.q[i]; }
            else    { for (int i = 0; i < v->n; i++) r += u.q[i]; }
        } else {
            /* at most 1024 residuals below 2^32: the 64-bit total cannot wrap */
            uint64_t r64 = 0;
            for (int i = 0; i < v->n; i++) {
                uint64_t x = lane(&u, v->k.T, (uint32_t)i);
                if (nm) x &= (uint64_t)((nm[i >> 3] >> (i & 7)) & 1) - 1;   /* a null row adds 0 */
                r64 += x;
            }
            r = r64;
        }
        s = (__int128)n_nonnull * ray_enc_logical(v->base, v->k) + r;
        break;
    }
    case RAY_ENC_RLE: {
        if (v->n_nulls) return false;                        /* nullable RLE decodes */
        uint32_t prev = 0;
        for (uint32_t r = 0; r < v->n_runs; r++) {
            s += (__int128)ray_enc_logical(lane(&v->run_vals, v->k.T, r), v->k) * (int64_t)(v->run_ends.h[r] - prev);
            prev = v->run_ends.h[r];
        }
        break;
    }
    default: return false;
    }
    *hi = (int64_t)(s >> 64);
    *lo = (uint64_t)s;
    *nn = n_nonnull;
    return true;
}

/* Counts (the count fast path): the evaluators' rules, summed instead of
 * written out.  A view with a bitmap declines (its nulls would need the
 * mask); SYM ids count == / != only, never ordered (#738). */
/* FOR_BP: eval_for's threshold t = c - logical(base) over the residuals
 * (no wrap, the view's guarantee); t outside [0, umax] decides every row.
 * The residuals are compared as the lane kernels unpack them, never
 * stored (ray_enccol_count_resid: measured 12-16% faster than unpacking
 * into a lane buffer and counting it). */
static int64_t count_for(const ray_enc_vview_t* v, uint16_t op, int64_t c) {
    const uint64_t umax = v->width >= 64 ? ~0ull : ((1ull << v->width) - 1);
    const __int128 t = (__int128)c - (__int128)ray_enc_logical(v->base, v->k);
    if (t < 0 || t > (__int128)umax) return cmp_one(t < 0 ? 1 : -1, op, 0) ? v->n : 0;
    return ray_enccol_count_resid(v, op, (uint64_t)t);
}

bool ray_enc_count_cmp(const ray_enc_vview_t* v, uint16_t op, int64_t c, int64_t* count) {
    if (v->nullmap || is_float(v->type) || !is_cmp_op(op)) return false;
    if (v->type == RAY_SYM && op != OP_EQ && op != OP_NE) return false;   /* never order ids (#738) */
    int64_t n = 0;
    switch (v->enc) {
    case RAY_ENC_CONST:
        n = cmp_one(ray_enc_logical(v->cbits, v->k), op, c) ? v->n : 0;
        break;
    case RAY_ENC_RLE: {
        uint32_t prev = 0;
        for (uint32_t r = 0; r < v->n_runs; r++) {
            const uint32_t e = v->run_ends.h[r];
            if (cmp_one(logical_of(lane(&v->run_vals, v->k.T, r), v->k), op, c)) n += e - prev;
            prev = e;
        }
        break;
    }
    case RAY_ENC_FOR_BP: n = count_for(v, op, c); break;
    case RAY_ENC_DICT: {
        const uint32_t lb = lower_bound(v, c), ub = upper_bound(v, c);
        uint32_t lo = 0, hi = 0;
        switch (op) {
        case OP_EQ: case OP_NE: lo = lb; hi = ub; break;
        case OP_LT: hi = lb; break;
        case OP_LE: hi = ub; break;
        case OP_GT: lo = ub; hi = v->dict_n; break;
        default:    lo = lb; hi = v->dict_n; break;        /* OP_GE */
        }
        const uint32_t span = hi - lo;
        uint32_t a = 0;
        for (int i = 0; i < v->n; i++) a += v->codes[i] - lo < span;   /* unsigned: one compare */
        n = op == OP_NE ? v->n - (int64_t)a : (int64_t)a;
        break;
    }
    default: return false;
    }
    *count = n;
    return true;
}

bool ray_enc_runs(const ray_enc_vview_t* v, void* ctx, ray_enc_run_fn fn) {
    if ((v->enc != RAY_ENC_CONST && v->enc != RAY_ENC_RLE) || is_float(v->type)) return false;
    uint32_t prev = 0;
    for (uint32_t r = 0; r < v->n_runs; r++) {
        const uint32_t e = v->run_ends.h[r];
        fn(ctx, logical_of(lane(&v->run_vals, v->k.T, r), v->k), (int32_t)prev, (int32_t)e);
        prev = e;
    }
    return true;
}

#define VR_LOOP(ARR, N) do { for (int q_ = 0; q_ < (N); q_++) { if ((ARR)[q_] < lo_) lo_ = (ARR)[q_]; if ((ARR)[q_] > hi_) hi_ = (ARR)[q_]; } } while (0)
void ray_enc_view_range(const ray_enc_vview_t* v, const ray_enc_lanes_t* u, int64_t* mn, int64_t* mx) {
    if (v->enc == RAY_ENC_CONST || v->enc == RAY_ENC_RLE) {
        int64_t a = INT64_MAX, b = INT64_MIN;
        for (uint32_t q = 0; q < v->n_runs; q++) {
            const int64_t x = logical_of(lane(&v->run_vals, v->k.T, q), v->k);
            if (x < a) a = x;
            if (x > b) b = x;
        }
        *mn = a; *mx = b;
    } else if (v->enc == RAY_ENC_FOR_BP) {
        /* no wrap (the view's guarantee): base + the extreme residuals are
         * the extreme values */
        uint64_t lo_ = UINT64_MAX, hi_ = 0;
        switch (v->k.T) {
        case 8:  VR_LOOP(u->b, v->n); break;
        case 16: VR_LOOP(u->h, v->n); break;
        case 32: VR_LOOP(u->s, v->n); break;
        default: VR_LOOP(u->q, v->n); break;
        }
        const uint64_t lb = (uint64_t)ray_enc_logical(v->base, v->k);
        *mn = (int64_t)(lb + lo_); *mx = (int64_t)(lb + hi_);
    } else {                                                  /* DICT: entries ascending in native order */
        uint32_t lo_ = UINT32_MAX, hi_ = 0;
        VR_LOOP(v->codes, v->n);
        *mn = logical_of(v->dict[lo_], v->k); *mx = logical_of(v->dict[hi_], v->k);
    }
}
#undef VR_LOOP

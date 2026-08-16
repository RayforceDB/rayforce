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
#include "lang/internal.h"
#include "ops/temporal.h"
#include "lang/format.h"  /* ray_type_name (error context) */
#include "core/pool.h"    /* ray_pool_dispatch — parallel extract/truncate */
#include <time.h>

/* ============================================================================
 * ray_temporal_extract — standalone extract, usable outside the DAG.
 *
 * Mirrors exec_extract's scalar decomposition kernel but takes a ray_t*
 * input directly.  Vector input → RAY_I64 vector; atom input → RAY_I64
 * atom.  Returned ref is caller-owned.  Called from the env dotted-path
 * resolver so `date.dd` / `ts.hh` etc. work at runtime without building
 * a DAG.
 * ============================================================================ */

#define RTE_USEC_PER_SEC  1000000LL
#define RTE_USEC_PER_MIN  (60LL  * RTE_USEC_PER_SEC)
#define RTE_USEC_PER_HOUR (3600LL * RTE_USEC_PER_SEC)
#define RTE_USEC_PER_DAY  (86400LL * RTE_USEC_PER_SEC)

/* Decompose a single 'microseconds since 2000-01-01' value into a field. */
static int64_t rte_extract_one(int64_t us, int field) {
    if (field == RAY_EXTRACT_EPOCH) return us;
    if (field == RAY_EXTRACT_HOUR) {
        int64_t day_us = us % RTE_USEC_PER_DAY;
        if (day_us < 0) day_us += RTE_USEC_PER_DAY;
        return day_us / RTE_USEC_PER_HOUR;
    }
    if (field == RAY_EXTRACT_MINUTE) {
        int64_t day_us = us % RTE_USEC_PER_DAY;
        if (day_us < 0) day_us += RTE_USEC_PER_DAY;
        return (day_us % RTE_USEC_PER_HOUR) / RTE_USEC_PER_MIN;
    }
    if (field == RAY_EXTRACT_SECOND) {
        int64_t day_us = us % RTE_USEC_PER_DAY;
        if (day_us < 0) day_us += RTE_USEC_PER_DAY;
        return (day_us % RTE_USEC_PER_MIN) / RTE_USEC_PER_SEC;
    }

    /* Calendar fields: Hinnant civil_from_days. */
    int64_t days_since_2000 = us / RTE_USEC_PER_DAY;
    if (us < 0 && us % RTE_USEC_PER_DAY != 0) days_since_2000--;
    int64_t z = days_since_2000 + 10957 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
    uint64_t mp = (5*doy_mar + 2) / 153;
    uint64_t d = doy_mar - (153*mp + 2) / 5 + 1;
    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
    y += (mo <= 2);

    if (field == RAY_EXTRACT_YEAR)  return y;
    if (field == RAY_EXTRACT_MONTH) return (int64_t)mo;
    if (field == RAY_EXTRACT_DAY)   return (int64_t)d;
    if (field == RAY_EXTRACT_DOW) {
        return ((days_since_2000 % 7) + 7 + 5) % 7 + 1;
    }
    if (field == RAY_EXTRACT_DOY) {
        static const int dbm[13] = {
            0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
        };
        if (mo < 1 || mo > 12) return 0;
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int64_t doy_jan = dbm[mo] + (int64_t)d;
        if (mo > 2 && leap) doy_jan++;
        return doy_jan;
    }
    return 0;
}

/* Convert a raw slot value from the respective temporal type into
 * microseconds-since-2000 — the internal unit used by rte_extract_one's
 * Hinnant math.  DATE is stored as int32 days, TIME as int32 ms,
 * TIMESTAMP as int64 *nanoseconds* (matching io/csv.c's parse and the
 * rest of the runtime).  The previous version of this helper treated
 * TIMESTAMP as µs, which made (yyyy ts) decode to absurd years (26204
 * on 2024-03-15) — a 1000× unit mismatch. */
static inline bool rte_to_us_ck(int8_t type, int64_t raw, int64_t* out) {
    if (type == RAY_DATE || type == -RAY_DATE) {
        /* DATE is int32 days, so `raw` can be as large as INT32_MAX — a date
         * hundreds of millennia out.  day * µs-per-day then overflows int64
         * (UBSan).  Such a value is simply not representable in the µs domain
         * the extract/truncate math uses; report it so the caller emits a
         * null, consistent with how a null input is handled. */
        if (raw > INT64_MAX / RTE_USEC_PER_DAY || raw < INT64_MIN / RTE_USEC_PER_DAY)
            return false;
        *out = raw * RTE_USEC_PER_DAY;
        return true;
    }
    if (type == RAY_TIME || type == -RAY_TIME) { *out = raw * 1000LL; return true; }
    /* RAY_TIMESTAMP / -RAY_TIMESTAMP: ns → µs, floor toward -inf.  Done with
     * truncate-then-adjust rather than negating: the old `-((-raw)+999)/1000`
     * overflowed for `raw` within 999 of INT64_MIN (the low edge of the
     * representable range). */
    int64_t q = raw / 1000LL;
    if (raw % 1000LL != 0 && raw < 0) q--;
    *out = q;
    return true;
}

/* Extract one field from a raw temporal slot.  Returns false when the source
 * value is not representable in the µs extract domain (caller emits null). */
static inline bool rte_extract_elem(int8_t t, int64_t raw, int field, int64_t* out) {
    int64_t us;
    if (!rte_to_us_ck(t, raw, &us)) return false;
    *out = rte_extract_one(us, field);
    return true;
}

/* Truncate a raw temporal slot to a TIMESTAMP-ns bucket.  Returns false when
 * the source, or the bucketed result, is outside the int64 nanosecond range
 * (caller emits null): a value hundreds of millennia out cannot be a
 * TIMESTAMP.  The floor uses overflow-safe arithmetic and the ACTUAL bucketed
 * result is range-checked (mirroring exec_date_trunc), so a value whose
 * floored bucket is still representable — e.g. the low day boundary
 * 1707.09.23 — round-trips instead of being rejected by a pre-floor headroom
 * that disagreed with the DAG path. */
static inline bool rte_trunc_elem(int8_t t, int64_t raw, int64_t bucket, int64_t* out_ns) {
    int64_t us;
    if (!rte_to_us_ck(t, raw, &us)) return false;
    /* Floor `us` to the bucket boundary (toward -inf).  `us - r` truncates
     * toward zero (magnitude only shrinks, never overflows); a negative
     * remainder needs one more bucket subtracted to floor toward -inf — the
     * one step that can underflow (a DATE reaches `us` across the full µs
     * range), so guard it. */
    int64_t r = us % bucket;
    int64_t out_us = us - r;
    if (r < 0) {
        if (out_us < INT64_MIN + bucket) return false;
        out_us -= bucket;
    }
    /* Range-check the ACTUAL floored result against the int64 nanosecond
     * domain (out_us × 1000), matching exec_date_trunc — not a conservative
     * pre-floor headroom, which rejected representable boundary values. */
    if (out_us > INT64_MAX / 1000LL || out_us < INT64_MIN / 1000LL) return false;
    *out_ns = out_us * 1000LL;
    return true;
}

/* ----------------------------------------------------------------------------
 * Parallel driver for the two whole-column kernels below.
 *
 * Both loops are pure elementwise maps: row i of the output depends only on
 * row i of the input, so any partition of [0, len) is safe.  The ONE piece of
 * cross-row state is the null marking.  Nulls in this engine are *sentinels in
 * the payload* (see ray_vec_set_null_checked) — the per-row write is already
 * disjoint — but ray_vec_set_null also does a read-modify-write of the shared
 * `attrs` byte (RAY_ATTR_HAS_NULLS, and the SORTED clear inside
 * vec_drop_index_inplace).  That byte is what races, not the bitmap the
 * bitmap-based engines would have.
 *
 * So the workers never touch `attrs`: each writes the NULL_* sentinel into its
 * own rows and reports "I produced at least one null" through one atomic flag,
 * and the caller folds that into `result->attrs` once, single-threaded, after
 * the dispatch has joined.  Result is byte-identical to the serial version:
 * the destination is a fresh ray_vec_new (attrs == 0, no index, not a slice),
 * so ray_vec_set_null on it reduces to exactly "sentinel + HAS_NULLS".
 * -------------------------------------------------------------------------- */
typedef struct {
    ray_t*             input;
    const char*        base;
    int64_t*           out;
    int64_t            bucket;        /* truncate only */
    int                field;
    int8_t             t;
    bool               src_has_nulls;
    bool               in32;          /* RAY_DATE / RAY_TIME element is int32 */
    _Atomic(uint32_t)  any_null;
} rte_par_ctx_t;

#define RTE_RANGE_BODY(CALL)                                                  \
    do {                                                                      \
        if (c->in32) {                                                        \
            const int32_t* d32 = (const int32_t*)c->base;                     \
            if (c->src_has_nulls) {                                           \
                for (int64_t i = start; i < end; i++)                         \
                    if (ray_vec_is_null(c->input, i) ||                       \
                        !CALL((int64_t)d32[i])) {                             \
                        c->out[i] = NULL_I64; nulled = true;                  \
                    }                                                         \
            } else {                                                          \
                for (int64_t i = start; i < end; i++)                         \
                    if (!CALL((int64_t)d32[i])) {                             \
                        c->out[i] = NULL_I64; nulled = true;                  \
                    }                                                         \
            }                                                                 \
        } else {                                                              \
            const int64_t* d64 = (const int64_t*)c->base;                     \
            if (c->src_has_nulls) {                                           \
                for (int64_t i = start; i < end; i++)                         \
                    if (ray_vec_is_null(c->input, i) || !CALL(d64[i])) {      \
                        c->out[i] = NULL_I64; nulled = true;                  \
                    }                                                         \
            } else {                                                          \
                for (int64_t i = start; i < end; i++)                         \
                    if (!CALL(d64[i])) {                                      \
                        c->out[i] = NULL_I64; nulled = true;                  \
                    }                                                         \
            }                                                                 \
        }                                                                     \
        if (nulled)                                                           \
            atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);     \
    } while (0)

#define RTE_EXTRACT_CALL(RAW) rte_extract_elem(c->t, (RAW), c->field, &c->out[i])
#define RTE_TRUNC_CALL(RAW)   rte_trunc_elem(c->t, (RAW), c->bucket, &c->out[i])

static void rte_extract_range(rte_par_ctx_t* c, int64_t start, int64_t end) {
    bool nulled = false;
    RTE_RANGE_BODY(RTE_EXTRACT_CALL);
}

static void rte_trunc_range(rte_par_ctx_t* c, int64_t start, int64_t end) {
    bool nulled = false;
    RTE_RANGE_BODY(RTE_TRUNC_CALL);
}

#undef RTE_EXTRACT_CALL
#undef RTE_TRUNC_CALL
#undef RTE_RANGE_BODY

static void rte_extract_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    rte_extract_range((rte_par_ctx_t*)ctx, start, end);
}

static void rte_trunc_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    rte_trunc_range((rte_par_ctx_t*)ctx, start, end);
}

/* Run `range_fn` over [0, len), in parallel when the column is big enough to
 * amortize dispatch (the engine-wide RAY_PARALLEL_THRESHOLD, same gate
 * expr_eval_full uses — no new knob), then fold the null flag into `result`. */
static void rte_run(rte_par_ctx_t* c, ray_pool_fn task_fn,
                    void (*range_fn)(rte_par_ctx_t*, int64_t, int64_t),
                    ray_t* result, int64_t len) {
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, len, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, task_fn, c, len);
    else
        range_fn(c, 0, len);
    if (atomic_load_explicit(&c->any_null, memory_order_relaxed))
        result->attrs |= RAY_ATTR_HAS_NULLS;
}

ray_t* ray_temporal_extract(ray_t* input, int field) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* Atom input — extract single value as RAY_I64 atom.  A null input
     * atom produces a typed null output (0Nl): a garbage year/month/etc.
     * extracted from the null-sentinel bit pattern would be deeply
     * confusing when mixed into downstream arithmetic. */
    if (input->type < 0) {
        int8_t t = input->type;
        if (t != -RAY_DATE && t != -RAY_TIME && t != -RAY_TIMESTAMP)
            return ray_error("type", "extract: expected date/time/timestamp, got %s", ray_type_name(t));
        if (RAY_ATOM_IS_NULL(input)) return ray_typed_null(-RAY_I64);
        int64_t ev;
        if (!rte_extract_elem(t, input->i64, field, &ev)) return ray_typed_null(-RAY_I64);
        return ray_i64(ev);
    }

    /* Vector input. */
    int8_t t = input->type;
    if (t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP)
        return ray_error("type", "extract: expected date/time/timestamp, got %s", ray_type_name(t));

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = len;
    int64_t* out = (int64_t*)ray_data(result);

    /* Null-aware decomposition: any row flagged null in the source
     * becomes 0 in the data buffer and carries the null bit on the
     * output, so downstream ops treat it as 0Nl rather than the bogus
     * year/month/etc that would fall out of decomposing the null
     * sentinel's bit pattern. */
    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
    /* src_has_nulls and the 32-/64-bit element dispatch are hoisted into the
     * context so the inner body is a tight typed kernel with no per-element
     * branches; the row range is chunked over the pool for large columns. */
    rte_par_ctx_t c = {
        .input = input,
        .base = (const char*)ray_data(input),
        .out = out,
        .field = field,
        .t = t,
        .src_has_nulls = src_has_nulls,
        .in32 = (t == RAY_DATE || t == RAY_TIME),
        .any_null = 0,
    };
    rte_run(&c, rte_extract_fn, rte_extract_range, result, len);
    return result;
}

/* Sym name → RAY_EXTRACT_* field code.  Resolves by reading the interned
 * name string and matching against the documented segment names.  Used
 * by the env dotted-path resolver so `date_col.dd` works without a DAG. */
int ray_temporal_field_from_sym(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return -1;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    if (!p) return -1;

    if (n == 4 && memcmp(p, "yyyy",   4) == 0) return RAY_EXTRACT_YEAR;
    if (n == 2 && memcmp(p, "mm",     2) == 0) return RAY_EXTRACT_MONTH;
    if (n == 2 && memcmp(p, "dd",     2) == 0) return RAY_EXTRACT_DAY;
    if (n == 2 && memcmp(p, "hh",     2) == 0) return RAY_EXTRACT_HOUR;
    if (n == 6 && memcmp(p, "minute", 6) == 0) return RAY_EXTRACT_MINUTE;
    if (n == 2 && memcmp(p, "ss",     2) == 0) return RAY_EXTRACT_SECOND;
    if (n == 3 && memcmp(p, "dow",    3) == 0) return RAY_EXTRACT_DOW;
    if (n == 3 && memcmp(p, "doy",    3) == 0) return RAY_EXTRACT_DOY;

    return -1;
}

/* Eval-level unary builtins.  Each one is a thin wrapper around
 * ray_temporal_extract with the field bound, so they participate in the
 * regular function-call machinery: `(ss ts)`, `(yyyy d)`, etc. behave
 * like any other unary builtin and `ts.ss`, `d.yyyy` resolve through
 * env_resolve's standard container-then-callable dispatch. */
ray_t* ray_extract_ss_fn(ray_t* x)     { return ray_temporal_extract(x, RAY_EXTRACT_SECOND); }
ray_t* ray_extract_hh_fn(ray_t* x)     { return ray_temporal_extract(x, RAY_EXTRACT_HOUR); }
ray_t* ray_extract_minute_fn(ray_t* x) { return ray_temporal_extract(x, RAY_EXTRACT_MINUTE); }
ray_t* ray_extract_yyyy_fn(ray_t* x)   { return ray_temporal_extract(x, RAY_EXTRACT_YEAR); }
ray_t* ray_extract_mm_fn(ray_t* x)     { return ray_temporal_extract(x, RAY_EXTRACT_MONTH); }
ray_t* ray_extract_dd_fn(ray_t* x)     { return ray_temporal_extract(x, RAY_EXTRACT_DAY); }
ray_t* ray_extract_dow_fn(ray_t* x)    { return ray_temporal_extract(x, RAY_EXTRACT_DOW); }
ray_t* ray_extract_doy_fn(ray_t* x)    { return ray_temporal_extract(x, RAY_EXTRACT_DOY); }

int ray_temporal_trunc_from_sym(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return -1;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    if (!p) return -1;
    if (n == 4 && memcmp(p, "date",  4) == 0) return RAY_EXTRACT_DAY;
    if (n == 4 && memcmp(p, "time",  4) == 0) return RAY_EXTRACT_SECOND;
    if (n == 5 && memcmp(p, "month", 5) == 0) return RAY_EXTRACT_MONTH;
    if (n == 4 && memcmp(p, "hour",  4) == 0) return RAY_EXTRACT_HOUR;
    if (n == 4 && memcmp(p, "year",  4) == 0) return RAY_EXTRACT_YEAR;
    /* "minute" intentionally NOT added — it collides with the extract
     * binding ("minute" → RAY_EXTRACT_MINUTE in
     * ray_temporal_field_from_sym), which query.c tries first.  The
     * DATE_TRUNC_INNER MINUTE case remains unreachable; covering it
     * would need a distinct trunc syntax (e.g. (trunc 'minute ts)). */
    return -1;
}

ray_t* ray_temporal_truncate(ray_t* input, int kind) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* Atom input — produce a RAY_TIMESTAMP atom.  Null input → 0Np. */
    if (input->type < 0) {
        int8_t t = input->type;
        if (t != -RAY_DATE && t != -RAY_TIME && t != -RAY_TIMESTAMP)
            return ray_error("type", "truncate: expected date/time/timestamp, got %s", ray_type_name(t));
        if (RAY_ATOM_IS_NULL(input)) return ray_typed_null(-RAY_TIMESTAMP);
        int64_t bucket = (kind == RAY_EXTRACT_DAY)
            ? RTE_USEC_PER_DAY
            : RTE_USEC_PER_SEC;
        int64_t out_ns;
        if (!rte_trunc_elem(t, input->i64, bucket, &out_ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(out_ns);
    }

    /* Vector input. */
    int8_t t = input->type;
    if (t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP)
        return ray_error("type", "truncate: expected date/time/timestamp, got %s", ray_type_name(t));

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_TIMESTAMP, len);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = len;
    int64_t* out = (int64_t*)ray_data(result);

    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
    const char* base = (const char*)ray_data(input);
    int64_t bucket = (kind == RAY_EXTRACT_DAY)
        ? RTE_USEC_PER_DAY
        : RTE_USEC_PER_SEC;

    /* Same hoist-and-chunk shape as ray_temporal_extract above. */
    rte_par_ctx_t c = {
        .input = input,
        .base = base,
        .out = out,
        .bucket = bucket,
        .t = t,
        .src_has_nulls = src_has_nulls,
        .in32 = (t == RAY_DATE || t == RAY_TIME),
        .any_null = 0,
    };
    rte_run(&c, rte_trunc_fn, rte_trunc_range, result, len);
    return result;
}

/* ============================================================================
 * EXTRACT — date/time component extraction from temporal columns
 *
 * Input:  RAY_TIMESTAMP (i64 us since 2000-01-01), RAY_DATE (i32 days since
 *         2000-01-01), or RAY_TIME (i32 ms since midnight).
 * Output: i64 vector of extracted field values.
 *
 * Uses Howard Hinnant's civil_from_days algorithm (public domain) for
 * Gregorian calendar decomposition.
 * ============================================================================ */

/* Per-worker state for the DAG-level extract.  Same contract as
 * rte_par_ctx_t above: workers own disjoint row ranges, write NULL_I64
 * sentinels themselves, and report the HAS_NULLS decision through one
 * atomic flag the caller folds into `result` after the join. */
typedef struct {
    ray_t*             input;
    int64_t*           out;
    int64_t            field;
    int8_t             in_type;
    bool               src_has_nulls;
    bool               in32;
    _Atomic(uint32_t)  any_null;
} xtr_par_ctx_t;

#undef  USEC_PER_SEC
#define USEC_PER_SEC  1000000LL
#define USEC_PER_MIN  (60LL  * USEC_PER_SEC)
#define USEC_PER_HOUR (3600LL * USEC_PER_SEC)
#define USEC_PER_DAY  (86400LL * USEC_PER_SEC)

static void xtr_extract_range(xtr_par_ctx_t* c, int64_t start, int64_t end) {
    ray_t* input   = c->input;
    int64_t* out   = c->out;
    int64_t field  = c->field;
    int8_t in_type = c->in_type;
    bool nulled    = false;

    /* Macro to emit a tight inner loop body with loop-invariant branches
     * hoisted at compile time.  HAS_NULLS and IN32 are 0/1 constants so
     * the compiler eliminates dead branches via DCE.
     * IN32=1 → RAY_DATE/RAY_TIME (int32 element), IN32=0 → TIMESTAMP/I64 (int64). */
#define EXTRACT_INNER(HAS_NULLS, IN32)                                      \
    do {                                                                    \
        while (ray_morsel_next(&m)) {                                       \
            int64_t n = m.morsel_len;                                       \
            for (int64_t i = 0; i < n; i++) {                              \
                if (HAS_NULLS && ray_vec_is_null(input, off + i)) {        \
                    out[off + i] = NULL_I64;                                \
                    nulled = true;                                          \
                    continue;                                               \
                }                                                           \
                int64_t us;                                                 \
                if (IN32) {                                                 \
                    /* RAY_DATE: int32 days → µs; RAY_TIME: int32 ms → µs */ \
                    int32_t raw32 = ((const int32_t*)m.morsel_ptr)[i];     \
                    /* A DATE hundreds of millennia out (int32 days) would  \
                     * overflow days×µs-per-day; it is not representable in  \
                     * the µs domain, so emit a null like a null input. */   \
                    if (in_type == RAY_DATE &&                              \
                        ((int64_t)raw32 > INT64_MAX / USEC_PER_DAY ||       \
                         (int64_t)raw32 < INT64_MIN / USEC_PER_DAY)) {      \
                        out[off + i] = NULL_I64;                            \
                        nulled = true;                                      \
                        continue;                                           \
                    }                                                       \
                    us = (in_type == RAY_DATE)                              \
                         ? (int64_t)raw32 * USEC_PER_DAY                   \
                         : (int64_t)raw32 * 1000LL;                        \
                } else {                                                    \
                    /* RAY_TIMESTAMP: int64 nanoseconds → µs, floor toward   \
                     * -inf.  Truncate-then-adjust instead of negating: the  \
                     * old -((-ns)+999)/1000 overflowed near INT64_MIN. */    \
                    int64_t ns = ((const int64_t*)m.morsel_ptr)[i];        \
                    us = ns / 1000LL;                                       \
                    if (ns % 1000LL != 0 && ns < 0) us--;                  \
                }                                                           \
                if (field == RAY_EXTRACT_EPOCH) {                          \
                    out[off + i] = us;                                      \
                } else if (field == RAY_EXTRACT_HOUR) {                    \
                    int64_t day_us = us % USEC_PER_DAY;                    \
                    if (day_us < 0) day_us += USEC_PER_DAY;               \
                    out[off + i] = day_us / USEC_PER_HOUR;                 \
                } else if (field == RAY_EXTRACT_MINUTE) {                  \
                    int64_t day_us = us % USEC_PER_DAY;                    \
                    if (day_us < 0) day_us += USEC_PER_DAY;               \
                    out[off + i] = (day_us % USEC_PER_HOUR) / USEC_PER_MIN; \
                } else if (field == RAY_EXTRACT_SECOND) {                  \
                    int64_t day_us = us % USEC_PER_DAY;                    \
                    if (day_us < 0) day_us += USEC_PER_DAY;               \
                    out[off + i] = (day_us % USEC_PER_MIN) / USEC_PER_SEC; \
                } else {                                                    \
                    /* Calendar fields: YEAR, MONTH, DAY, DOW, DOY */      \
                    int64_t days_since_2000 = us / USEC_PER_DAY;           \
                    if (us < 0 && us % USEC_PER_DAY != 0) days_since_2000--; \
                    int64_t z = days_since_2000 + 10957 + 719468;          \
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;     \
                    uint64_t doe = (uint64_t)(z - era * 146097);           \
                    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; \
                    int64_t y = (int64_t)yoe + era * 400;                  \
                    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100); \
                    uint64_t mp = (5*doy_mar + 2) / 153;                   \
                    uint64_t d_cal = doy_mar - (153*mp + 2) / 5 + 1;      \
                    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;              \
                    y += (mo <= 2);                                         \
                    if (field == RAY_EXTRACT_YEAR) {                       \
                        out[off + i] = y;                                  \
                    } else if (field == RAY_EXTRACT_MONTH) {               \
                        out[off + i] = (int64_t)mo;                        \
                    } else if (field == RAY_EXTRACT_DAY) {                 \
                        out[off + i] = (int64_t)d_cal;                     \
                    } else if (field == RAY_EXTRACT_DOW) {                 \
                        out[off + i] = ((days_since_2000 % 7) + 7 + 5) % 7 + 1; \
                    } else if (field == RAY_EXTRACT_DOY) {                 \
                        static const int dbm[13] = {                       \
                            0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 \
                        };                                                  \
                        if (mo < 1 || mo > 12) { out[off + i] = 0; continue; } \
                        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)); \
                        int64_t doy_jan = dbm[mo] + (int64_t)d_cal;       \
                        if (mo > 2 && leap) doy_jan++;                     \
                        out[off + i] = doy_jan;                             \
                    } else {                                                \
                        out[off + i] = 0;                                  \
                    }                                                       \
                }                                                           \
            }                                                               \
            off += n;                                                       \
        }                                                                   \
    } while (0)

    ray_morsel_t m;
    ray_morsel_init_range(&m, input, start, end);
    int64_t off = start;

    /* Hoist src_has_nulls and in_type (32- vs 64-bit element) outside
     * all loops using the macro dispatch above. */
    const bool src_has_nulls = c->src_has_nulls;
    const bool in32 = c->in32;
    if (!src_has_nulls && !in32) EXTRACT_INNER(0, 0);
    else if (!src_has_nulls &&  in32) EXTRACT_INNER(0, 1);
    else if ( src_has_nulls && !in32) EXTRACT_INNER(1, 0);
    else                              EXTRACT_INNER(1, 1);

#undef EXTRACT_INNER

    if (nulled) atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

#undef USEC_PER_SEC
#undef USEC_PER_MIN
#undef USEC_PER_HOUR
#undef USEC_PER_DAY

static void xtr_extract_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    xtr_extract_range((xtr_par_ctx_t*)ctx, start, end);
}

ray_t* exec_extract(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));

    /* Preserve ray_morsel_init's one-time sequential-readahead hint for
     * mmap'd columns.  The per-worker ray_morsel_init_range deliberately
     * does not issue it — it would fire once per task instead of once per
     * column — so it is issued here, on the main thread, before dispatch. */
    ray_morsel_t warm;
    ray_morsel_init(&warm, input);

    xtr_par_ctx_t c = {
        .input = input,
        .out = (int64_t*)ray_data(result),
        .field = field,
        .in_type = in_type,
        .src_has_nulls = src_has_nulls,
        .in32 = (in_type == RAY_DATE || in_type == RAY_TIME),
        .any_null = 0,
    };

    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, len, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, xtr_extract_fn, &c, len);
    else
        xtr_extract_range(&c, 0, len);
    if (atomic_load_explicit(&c.any_null, memory_order_relaxed))
        result->attrs |= RAY_ATTR_HAS_NULLS;

    ray_release(input);
    return result;
}

/* ============================================================================
 * DATE_TRUNC — truncate temporal value to specified precision
 *
 * Input:  RAY_TIMESTAMP (i64 us since 2000-01-01), RAY_DATE (i32 days since
 *         2000-01-01), or RAY_TIME (i32 ms since midnight).
 * Output: RAY_TIMESTAMP (i64 us) — always returns microseconds since 2000-01-01.
 * Sub-day: modular arithmetic. Month/year: calendar decompose + recompose.
 * ============================================================================ */

/* Convert (year, month, day) to days since 2000-01-01 using the inverse of
 * Hinnant's civil_from_days. */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m > 2 ? (uint64_t)m - 3 : (uint64_t)m + 9) + 2) / 5 + (uint64_t)d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468 - 10957;
}

#define DT_USEC_PER_SEC  1000000LL
#define DT_USEC_PER_MIN  (60LL  * DT_USEC_PER_SEC)
#define DT_USEC_PER_HOUR (3600LL * DT_USEC_PER_SEC)
#define DT_USEC_PER_DAY  (86400LL * DT_USEC_PER_SEC)

static void xtr_trunc_range(xtr_par_ctx_t* c, int64_t start, int64_t end) {
    ray_t* input   = c->input;
    int64_t* out   = c->out;
    int64_t field  = c->field;
    int8_t in_type = c->in_type;
    bool nulled    = false;

    /* Macro to emit a tight inner loop body with HAS_NULLS and IN32 hoisted
     * as compile-time constants (DCE removes dead branches).
     * IN32=1 → RAY_DATE/RAY_TIME (int32 element), IN32=0 → TIMESTAMP (int64). */
#define DATE_TRUNC_INNER(HAS_NULLS, IN32)                                   \
    do {                                                                    \
        while (ray_morsel_next(&m)) {                                       \
            int64_t n = m.morsel_len;                                       \
            for (int64_t i = 0; i < n; i++) {                              \
                if (HAS_NULLS && ray_vec_is_null(input, off + i)) {        \
                    out[off + i] = NULL_I64;                                \
                    nulled = true;                        \
                    continue;                                               \
                }                                                           \
                int64_t us;                                                 \
                if (IN32) {                                                 \
                    int32_t raw32 = ((const int32_t*)m.morsel_ptr)[i];     \
                    /* Bound the DATE by the int64-NANOSECOND representable   \
                     * day range, not just the µs one: the YEAR/MONTH arms    \
                     * below re-multiply days_from_civil(...) — a day count   \
                     * floored DOWN to the period start, up to a year beyond  \
                     * `raw32` — by DT_USEC_PER_DAY, and the result is then    \
                     * ×1000 to nanoseconds.  Matches rte_trunc_elem; a DATE   \
                     * outside this range is not a TIMESTAMP, so null it. */   \
                    if (in_type == RAY_DATE &&                              \
                        ((int64_t)raw32 > INT64_MAX / 1000 / DT_USEC_PER_DAY || \
                         (int64_t)raw32 < INT64_MIN / 1000 / DT_USEC_PER_DAY)) { \
                        out[off + i] = NULL_I64;                            \
                        nulled = true;                    \
                        continue;                                           \
                    }                                                       \
                    us = (in_type == RAY_DATE)                              \
                         ? (int64_t)raw32 * DT_USEC_PER_DAY                \
                         : (int64_t)raw32 * 1000LL;                        \
                } else {                                                    \
                    /* ns → µs, floor toward -inf, overflow-free (the old    \
                     * -((-ns)+999)/1000 overflowed near INT64_MIN). */      \
                    int64_t ns = ((const int64_t*)m.morsel_ptr)[i];        \
                    us = ns / 1000LL;                                       \
                    if (ns % 1000LL != 0 && ns < 0) us--;                  \
                }                                                           \
                int64_t out_us;                                             \
                switch (field) {                                            \
                    case RAY_EXTRACT_SECOND: {                              \
                        int64_t r = us % DT_USEC_PER_SEC;                  \
                        out_us = us - r - (r < 0 ? DT_USEC_PER_SEC : 0);  \
                        break;                                              \
                    }                                                       \
                    case RAY_EXTRACT_MINUTE: {                              \
                        int64_t r = us % DT_USEC_PER_MIN;                  \
                        out_us = us - r - (r < 0 ? DT_USEC_PER_MIN : 0);  \
                        break;                                              \
                    }                                                       \
                    case RAY_EXTRACT_HOUR: {                                \
                        int64_t r = us % DT_USEC_PER_HOUR;                 \
                        out_us = us - r - (r < 0 ? DT_USEC_PER_HOUR : 0); \
                        break;                                              \
                    }                                                       \
                    case RAY_EXTRACT_DAY: {                                 \
                        int64_t r = us % DT_USEC_PER_DAY;                  \
                        out_us = us - r - (r < 0 ? DT_USEC_PER_DAY : 0);  \
                        break;                                              \
                    }                                                       \
                    case RAY_EXTRACT_MONTH: {                               \
                        int64_t days2k = us / DT_USEC_PER_DAY;             \
                        if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--; \
                        int64_t z = days2k + 10957 + 719468;               \
                        int64_t era = (z >= 0 ? z : z - 146096) / 146097; \
                        uint64_t doe = (uint64_t)(z - era * 146097);       \
                        uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; \
                        int64_t y = (int64_t)yoe + era * 400;              \
                        uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100); \
                        uint64_t mp = (5*doy_mar + 2) / 153;               \
                        uint64_t mo = mp < 10 ? mp + 3 : mp - 9;          \
                        y += (mo <= 2);                                     \
                        out_us = days_from_civil(y, (int64_t)mo, 1) * DT_USEC_PER_DAY; \
                        break;                                              \
                    }                                                       \
                    case RAY_EXTRACT_YEAR: {                                \
                        int64_t days2k = us / DT_USEC_PER_DAY;             \
                        if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--; \
                        int64_t z = days2k + 10957 + 719468;               \
                        int64_t era = (z >= 0 ? z : z - 146096) / 146097; \
                        uint64_t doe = (uint64_t)(z - era * 146097);       \
                        uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; \
                        int64_t y = (int64_t)yoe + era * 400;              \
                        uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100); \
                        uint64_t mp = (5*doy_mar + 2) / 153;               \
                        uint64_t mo = mp < 10 ? mp + 3 : mp - 9;          \
                        y += (mo <= 2);                                     \
                        out_us = days_from_civil(y, 1, 1) * DT_USEC_PER_DAY; \
                        break;                                              \
                    }                                                       \
                    default:                                                \
                        out_us = us;                                        \
                        break;                                              \
                }                                                           \
                /* Result must fit int64 nanoseconds (out_us × 1000). */    \
                if (out_us > INT64_MAX / 1000LL || out_us < INT64_MIN / 1000LL) { \
                    out[off + i] = NULL_I64;                                \
                    nulled = true;                        \
                    continue;                                               \
                }                                                           \
                out[off + i] = out_us * 1000LL; /* µs → ns for RAY_TIMESTAMP */ \
            }                                                               \
            off += n;                                                       \
        }                                                                   \
    } while (0)

    ray_morsel_t m;
    ray_morsel_init_range(&m, input, start, end);
    int64_t off = start;

    /* Hoist src_has_nulls and in_type dispatch outside all loops. */
    const bool src_has_nulls = c->src_has_nulls;
    const bool dt_in32 = c->in32;
    if (!src_has_nulls && !dt_in32) DATE_TRUNC_INNER(0, 0);
    else if (!src_has_nulls &&  dt_in32) DATE_TRUNC_INNER(0, 1);
    else if ( src_has_nulls && !dt_in32) DATE_TRUNC_INNER(1, 0);
    else                                 DATE_TRUNC_INNER(1, 1);

#undef DATE_TRUNC_INNER

    if (nulled) atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

#undef DT_USEC_PER_SEC
#undef DT_USEC_PER_MIN
#undef DT_USEC_PER_HOUR
#undef DT_USEC_PER_DAY

static void xtr_trunc_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    xtr_trunc_range((xtr_par_ctx_t*)ctx, start, end);
}

ray_t* exec_date_trunc(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_TIMESTAMP, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));

    /* Preserve ray_morsel_init's one-time sequential-readahead hint for
     * mmap'd columns.  The per-worker ray_morsel_init_range deliberately
     * does not issue it — it would fire once per task instead of once per
     * column — so it is issued here, on the main thread, before dispatch. */
    ray_morsel_t warm;
    ray_morsel_init(&warm, input);

    xtr_par_ctx_t c = {
        .input = input,
        .out = (int64_t*)ray_data(result),
        .field = field,
        .in_type = in_type,
        .src_has_nulls = src_has_nulls,
        .in32 = (in_type == RAY_DATE || in_type == RAY_TIME),
        .any_null = 0,
    };

    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, len, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, xtr_trunc_fn, &c, len);
    else
        xtr_trunc_range(&c, 0, len);
    if (atomic_load_explicit(&c.any_null, memory_order_relaxed))
        result->attrs |= RAY_ATTR_HAS_NULLS;

    ray_release(input);
    return result;
}

/* ── Builtins ── */

/* Helper: is the argument the symbol 'global? */
static bool is_global_arg(ray_t* arg) {
    if (arg && arg->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(arg->i64);
        if (s && ray_str_len(s) == 6 && memcmp(ray_str_ptr(s), "global", 6) == 0)
            return true;
    }
    return false;
}

/* Compute seconds since 2000.01.01 00:00:00 UTC (the rayforce epoch) */
static time_t ray_epoch_offset(void) {
    /* 2000-01-01 00:00:00 UTC = 946684800 seconds after 1970 epoch */
    return (time_t)946684800;
}

/* Current UTC wall-clock as a RAY_TIMESTAMP value (ns since 2000-01-01).
 * The one place the epoch conversion lives; internal callers (query log, …)
 * reuse it rather than re-deriving the offset. */
int64_t ray_timestamp_now_ns(void) {
    return ((int64_t)time(NULL) - (int64_t)ray_epoch_offset()) * 1000000000LL;
}

/* (date 'local) or (date 'global) — returns current date as DATE atom.
 * Overloaded: if arg is a DATE / TIME / TIMESTAMP value or vector,
 * returns `arg` truncated to the day boundary (RAY_TIMESTAMP result).
 * This lets `(date ts)` and `ts.date` both flow through the registered
 * unary builtin with no special-case detour. */
ray_t* ray_date_clock_fn(ray_t* arg) {
    if (arg) {
        int8_t t = arg->type < 0 ? (int8_t)-arg->type : arg->type;
        if (t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP)
            return ray_temporal_truncate(arg, RAY_EXTRACT_DAY);
    }
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "date: failed to get current time");

    /* Reconstruct midnight of today */
    struct tm day = *t;
    day.tm_hour = 0; day.tm_min = 0; day.tm_sec = 0; day.tm_isdst = -1;
    time_t day_time = mktime(&day);

    /* For UTC (global), mktime interprets as local — adjust via difference */
    if (!local) {
        /* Use a simpler approach: total days from epoch */
        int32_t days = (int32_t)((now - ray_epoch_offset()) / 86400);
        return ray_date((int64_t)days);
    }

    /* Local: days since the rayforce epoch, in local time sense */
    int32_t days = (int32_t)((day_time - ray_epoch_offset()) / 86400);
    return ray_date((int64_t)days);
}

/* (time 'local) or (time 'global) — returns current time as TIME atom.
 * Overloaded same way as ray_date_clock_fn: temporal argument ⇒
 * truncate to second boundary (RAY_TIMESTAMP); symbol / default ⇒ clock. */
ray_t* ray_time_clock_fn(ray_t* arg) {
    if (arg) {
        int8_t t = arg->type < 0 ? (int8_t)-arg->type : arg->type;
        if (t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP)
            return ray_temporal_truncate(arg, RAY_EXTRACT_SECOND);
    }
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "time: failed to get current time");

    int32_t ms = t->tm_hour * 3600000 + t->tm_min * 60000 + t->tm_sec * 1000;
    return ray_time((int64_t)ms);
}

/* (timestamp 'local) or (timestamp 'global) — returns current timestamp (ns since 2000.01.01) */
ray_t* ray_timestamp_clock_fn(ray_t* arg) {
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "timestamp: failed to get current time");

    if (!local)
        return ray_timestamp(ray_timestamp_now_ns());

    /* For local, compute offset from rayforce epoch in local terms */
    struct tm lt = *t;
    lt.tm_isdst = -1;
    int64_t secs = mktime(&lt) - ray_epoch_offset();
    return ray_timestamp(secs * 1000000000LL);
}

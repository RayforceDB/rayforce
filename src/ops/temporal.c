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
static inline int64_t rte_to_us(int8_t type, int64_t raw) {
    if (type == RAY_DATE || type == -RAY_DATE) return raw * RTE_USEC_PER_DAY;
    if (type == RAY_TIME || type == -RAY_TIME) return raw * 1000LL;
    /* RAY_TIMESTAMP / -RAY_TIMESTAMP: ns → µs (floor toward -inf). */
    return raw >= 0 ? raw / 1000LL
                    : -(((-raw) + 999LL) / 1000LL);
}

/* Inverse of rte_to_us for TIMESTAMP output paths (truncate). */
static inline int64_t rte_us_to_ts_raw(int64_t us) { return us * 1000LL; }

ray_t* ray_temporal_extract(ray_t* input, int field) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* Atom input — extract single value as RAY_I64 atom.  A null input
     * atom produces a typed null output (0Nl): a garbage year/month/etc.
     * extracted from the null-sentinel bit pattern would be deeply
     * confusing when mixed into downstream arithmetic. */
    if (input->type < 0) {
        int8_t t = input->type;
        if (t != -RAY_DATE && t != -RAY_TIME && t != -RAY_TIMESTAMP)
            return ray_error("type", NULL);
        if (RAY_ATOM_IS_NULL(input)) return ray_typed_null(-RAY_I64);
        int64_t raw = input->i64;
        int64_t us = rte_to_us(t, raw);
        return ray_i64(rte_extract_one(us, field));
    }

    /* Vector input. */
    int8_t t = input->type;
    if (t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP)
        return ray_error("type", NULL);

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
    const char* base = (const char*)ray_data(input);
    for (int64_t i = 0; i < len; i++) {
        if (src_has_nulls && ray_vec_is_null(input, i)) {
            out[i] = 0;
            ray_vec_set_null(result, i, true);
            continue;
        }
        int64_t raw;
        if (t == RAY_DATE)       raw = (int64_t)((const int32_t*)base)[i];
        else if (t == RAY_TIME)  raw = (int64_t)((const int32_t*)base)[i];
        else                     raw = ((const int64_t*)base)[i];
        out[i] = rte_extract_one(rte_to_us(t, raw), field);
    }
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
    if (n == 4 && memcmp(p, "date", 4) == 0) return RAY_EXTRACT_DAY;
    if (n == 4 && memcmp(p, "time", 4) == 0) return RAY_EXTRACT_SECOND;
    return -1;
}

ray_t* ray_temporal_truncate(ray_t* input, int kind) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* Atom input — produce a RAY_TIMESTAMP atom.  Null input → 0Np. */
    if (input->type < 0) {
        int8_t t = input->type;
        if (t != -RAY_DATE && t != -RAY_TIME && t != -RAY_TIMESTAMP)
            return ray_error("type", NULL);
        if (RAY_ATOM_IS_NULL(input)) return ray_typed_null(-RAY_TIMESTAMP);
        int64_t us = rte_to_us(t, input->i64);
        int64_t bucket = (kind == RAY_EXTRACT_DAY)
            ? RTE_USEC_PER_DAY
            : RTE_USEC_PER_SEC;
        int64_t r = us % bucket;
        int64_t out_us = us - r - (r < 0 ? bucket : 0);
        return ray_timestamp(rte_us_to_ts_raw(out_us));
    }

    /* Vector input. */
    int8_t t = input->type;
    if (t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP)
        return ray_error("type", NULL);

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

    for (int64_t i = 0; i < len; i++) {
        if (src_has_nulls && ray_vec_is_null(input, i)) {
            out[i] = 0;
            ray_vec_set_null(result, i, true);
            continue;
        }
        int64_t raw;
        if (t == RAY_DATE)       raw = (int64_t)((const int32_t*)base)[i];
        else if (t == RAY_TIME)  raw = (int64_t)((const int32_t*)base)[i];
        else                     raw = ((const int64_t*)base)[i];
        int64_t us = rte_to_us(t, raw);
        int64_t r = us % bucket;
        out[i] = rte_us_to_ts_raw(us - r - (r < 0 ? bucket : 0));
    }
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

ray_t* exec_extract(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    int64_t* out = (int64_t*)ray_data(result);

    #undef  USEC_PER_SEC
    #define USEC_PER_SEC  1000000LL
    #define USEC_PER_MIN  (60LL  * USEC_PER_SEC)
    #define USEC_PER_HOUR (3600LL * USEC_PER_SEC)
    #define USEC_PER_DAY  (86400LL * USEC_PER_SEC)

    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));

    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t off = 0;

    while (ray_morsel_next(&m)) {
        int64_t n = m.morsel_len;

        for (int64_t i = 0; i < n; i++) {
            /* Propagate nulls: decomposing a null-sentinel's raw bytes
             * would emit a bogus year / month / hour, so we zero the
             * output slot and set its null bit instead. */
            if (src_has_nulls && ray_vec_is_null(input, off + i)) {
                out[off + i] = 0;
                ray_vec_set_null(result, off + i, true);
                continue;
            }
            int64_t us;
            if (in_type == RAY_DATE) {
                /* int32 days since 2000-01-01 -> microseconds */
                int32_t d = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)d * USEC_PER_DAY;
            } else if (in_type == RAY_TIME) {
                /* int32 milliseconds since midnight -> microseconds */
                int32_t ms = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)ms * 1000LL;
            } else {
                /* RAY_TIMESTAMP: int64 *nanoseconds* since 2000 (matches
                 * io/csv parse and the rest of the runtime).  Convert to
                 * µs for the calendar/time decomposition below.  RAY_I64
                 * inputs flow through the same path; anything higher-
                 * resolution than µs loses its low three digits, which
                 * doesn't matter for calendar or clock field extraction. */
                int64_t ns = ((const int64_t*)m.morsel_ptr)[i];
                us = ns >= 0 ? ns / 1000LL
                             : -(((-ns) + 999LL) / 1000LL);
            }

            if (field == RAY_EXTRACT_EPOCH) {
                out[off + i] = us;
            } else if (field == RAY_EXTRACT_HOUR) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = day_us / USEC_PER_HOUR;
            } else if (field == RAY_EXTRACT_MINUTE) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = (day_us % USEC_PER_HOUR) / USEC_PER_MIN;
            } else if (field == RAY_EXTRACT_SECOND) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = (day_us % USEC_PER_MIN) / USEC_PER_SEC;
            } else {
                /* Calendar fields: YEAR, MONTH, DAY, DOW, DOY */
                /* Floor-divide microseconds to get day count */
                int64_t days_since_2000 = us / USEC_PER_DAY;
                if (us < 0 && us % USEC_PER_DAY != 0) days_since_2000--;

                /* Hinnant civil_from_days: shift to 0000-03-01 era-based epoch */
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

                if (field == RAY_EXTRACT_YEAR) {
                    out[off + i] = y;
                } else if (field == RAY_EXTRACT_MONTH) {
                    out[off + i] = (int64_t)mo;
                } else if (field == RAY_EXTRACT_DAY) {
                    out[off + i] = (int64_t)d;
                } else if (field == RAY_EXTRACT_DOW) {
                    /* ISO day of week: Mon=1 .. Sun=7
                     * 2000-01-01 was Saturday (ISO 6).
                     * Formula: ((days%7)+7+5)%7 + 1 */
                    out[off + i] = ((days_since_2000 % 7) + 7 + 5) % 7 + 1;
                } else if (field == RAY_EXTRACT_DOY) {
                    /* Day of year [1..366], January-based */
                    static const int dbm[13] = {
                        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
                    };
                    if (mo < 1 || mo > 12) { out[off + i] = 0; continue; }
                    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                    int64_t doy_jan = dbm[mo] + (int64_t)d;
                    if (mo > 2 && leap) doy_jan++;
                    out[off + i] = doy_jan;
                } else {
                    out[off + i] = 0;
                }
            }
        }
        off += n;
    }

    #undef USEC_PER_SEC
    #undef USEC_PER_MIN
    #undef USEC_PER_HOUR
    #undef USEC_PER_DAY

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

ray_t* exec_date_trunc(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_TIMESTAMP, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    int64_t* out = (int64_t*)ray_data(result);

    #define DT_USEC_PER_SEC  1000000LL
    #define DT_USEC_PER_MIN  (60LL  * DT_USEC_PER_SEC)
    #define DT_USEC_PER_HOUR (3600LL * DT_USEC_PER_SEC)
    #define DT_USEC_PER_DAY  (86400LL * DT_USEC_PER_SEC)

    /* Slice-aware HAS_NULLS check: slices don't carry HAS_NULLS on
     * themselves, so inspect the parent when input is a slice. */
    bool src_has_nulls =
        (input->attrs & RAY_ATTR_HAS_NULLS) ||
        ((input->attrs & RAY_ATTR_SLICE) && input->slice_parent &&
         (input->slice_parent->attrs & RAY_ATTR_HAS_NULLS));

    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t off = 0;

    while (ray_morsel_next(&m)) {
        int64_t n = m.morsel_len;

        for (int64_t i = 0; i < n; i++) {
            /* Null sentinels decode to garbage times; propagate the
             * null bit instead of emitting a bogus truncated value. */
            if (src_has_nulls && ray_vec_is_null(input, off + i)) {
                out[off + i] = 0;
                ray_vec_set_null(result, off + i, true);
                continue;
            }

            int64_t us;
            if (in_type == RAY_DATE) {
                int32_t d = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)d * DT_USEC_PER_DAY;
            } else if (in_type == RAY_TIME) {
                int32_t ms = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)ms * 1000LL;
            } else {
                /* RAY_TIMESTAMP: nanoseconds since 2000 → microseconds.
                 * Sub-microsecond precision is intentionally dropped —
                 * every DATE_TRUNC field truncates at second boundary
                 * or coarser. */
                int64_t ns = ((const int64_t*)m.morsel_ptr)[i];
                us = ns >= 0 ? ns / 1000LL
                             : -(((-ns) + 999LL) / 1000LL);
            }

            /* Truncation math below happens in µs; the final value is
             * scaled back to ns before storing, because the result
             * vector is RAY_TIMESTAMP and the rest of the runtime
             * expects ns. */
            int64_t out_us;
            switch (field) {
                case RAY_EXTRACT_SECOND: {
                    int64_t r = us % DT_USEC_PER_SEC;
                    out_us = us - r - (r < 0 ? DT_USEC_PER_SEC : 0);
                    break;
                }
                case RAY_EXTRACT_MINUTE: {
                    int64_t r = us % DT_USEC_PER_MIN;
                    out_us = us - r - (r < 0 ? DT_USEC_PER_MIN : 0);
                    break;
                }
                case RAY_EXTRACT_HOUR: {
                    int64_t r = us % DT_USEC_PER_HOUR;
                    out_us = us - r - (r < 0 ? DT_USEC_PER_HOUR : 0);
                    break;
                }
                case RAY_EXTRACT_DAY: {
                    int64_t r = us % DT_USEC_PER_DAY;
                    out_us = us - r - (r < 0 ? DT_USEC_PER_DAY : 0);
                    break;
                }
                case RAY_EXTRACT_MONTH: {
                    int64_t days2k = us / DT_USEC_PER_DAY;
                    if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--;
                    int64_t z = days2k + 10957 + 719468;
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                    uint64_t doe = (uint64_t)(z - era * 146097);
                    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int64_t y = (int64_t)yoe + era * 400;
                    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
                    uint64_t mp = (5*doy_mar + 2) / 153;
                    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
                    y += (mo <= 2);
                    out_us = days_from_civil(y, (int64_t)mo, 1) * DT_USEC_PER_DAY;
                    break;
                }
                case RAY_EXTRACT_YEAR: {
                    int64_t days2k = us / DT_USEC_PER_DAY;
                    if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--;
                    int64_t z = days2k + 10957 + 719468;
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                    uint64_t doe = (uint64_t)(z - era * 146097);
                    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int64_t y = (int64_t)yoe + era * 400;
                    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
                    uint64_t mp = (5*doy_mar + 2) / 153;
                    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
                    y += (mo <= 2);
                    out_us = days_from_civil(y, 1, 1) * DT_USEC_PER_DAY;
                    break;
                }
                default:
                    out_us = us;
                    break;
            }
            out[off + i] = out_us * 1000LL;  /* µs → ns for RAY_TIMESTAMP */
        }
        off += n;
    }

    #undef DT_USEC_PER_SEC
    #undef DT_USEC_PER_MIN
    #undef DT_USEC_PER_HOUR
    #undef DT_USEC_PER_DAY

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

    int64_t secs;
    if (!local) {
        secs = now - ray_epoch_offset();
    } else {
        /* For local, compute offset from rayforce epoch in local terms */
        struct tm lt = *t;
        lt.tm_isdst = -1;
        secs = mktime(&lt) - ray_epoch_offset();
    }

    int64_t nanos = secs * 1000000000LL;
    return ray_timestamp(nanos);
}

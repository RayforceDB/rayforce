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

/*
 * validate.c -- Null-model invariant validator (review item §2.1).
 *
 * Invariant 16.4: if a typed fixed-width column's payload contains a
 * type-correct *reserved* null sentinel, RAY_ATTR_HAS_NULLS MUST be set.
 *
 * Compiled only in DEBUG/ASAN builds (the whole TU collapses to nothing in
 * release via the #ifdef DEBUG guard below).  The release prototype in
 * vec.h expands ray_check_null_invariant(v) to ((void)0), so there is zero
 * cost on the hot path of a release binary.
 *
 * SCOPE — types whose null sentinel is TRULY RESERVED (unreachable as a
 * real value through normal ops), so a payload hit without HAS_NULLS is
 * unambiguously a producer bug:
 *   F64, F32        — any NaN bit pattern (x != x); the single-null float
 *                     model canonicalizes every non-finite to NULL_F64.
 *   I64, TIMESTAMP  — NULL_I64 (INT64_MIN).  64-bit arithmetic overflow is
 *                     guarded to null at every producer; INT64_MIN is never
 *                     a legitimate computed value.
 *
 * Deliberately NOT validated (return true / no-op):
 *   I16, I32 (+ DATE/TIME, which are I32-backed)
 *                   — DOCUMENTED EXCEPTION.  Narrow-int arithmetic wraps
 *                     modulo the type width (see
 *                     test/rfl/expr/narrow_binary.rfl): 32767h + 1h == -32768h
 *                     and 2147483647i + 1i == -2147483648i are REAL wrapped
 *                     values that happen to equal NULL_I16 / NULL_I32.
 *                     Enforcing 16.4 here would force those results to read
 *                     back as null, contradicting documented wrap semantics.
 *                     The sentinel is therefore NOT reserved for I16/I32, so
 *                     they are out of the mechanism's scope.  (Producers that
 *                     INTEND an I16/I32 null still set HAS_NULLS via
 *                     ray_vec_set_null; this only declines to *assert* it.)
 *   BOOL, U8        — non-nullable, no sentinel.
 *   SYM             — no-null by design: ray_vec_is_null() short-circuits to
 *                     false for SYM regardless of payload, and sym-id 0 is a
 *                     legitimate (interned empty-string) value.
 *   STR, GUID       — the "sentinel" is an empty string / 16 zero bytes,
 *                     which is a LEGITIMATE ordinary value.  ray_vec_is_null
 *                     only treats it as null when HAS_NULLS is already set,
 *                     so an empty string WITHOUT HAS_NULLS is valid, not a
 *                     violation.  Forcing the bit here would be wrong.
 *   slices          — don't own the payload and never carry HAS_NULLS (the
 *                     parent does); ray_vec_is_null walks to the parent.
 *   zero-length     — nothing to scan.
 *
 * LIST/TABLE/DICT are recursed into (cheaply) so column results inside a
 * table flow through the same check.
 *
 * SAMPLING — to bound DEBUG cost on huge columns we do not always scan
 * every element.  Up to VALIDATE_FULL_SCAN_N elements are scanned in full;
 * beyond that we scan the first VALIDATE_HEAD_N, then a strided sample of
 * VALIDATE_SAMPLE_N positions across the remainder.  Producer COVERAGE
 * (which producer set is exercised) matters more than scanning every byte:
 * a producer that writes a sentinel without HAS_NULLS overwhelmingly writes
 * it in the head or hits the stride.  Full coverage of small/medium columns
 * (the common test shape) is exact.
 */

#ifdef DEBUG

#include "vec.h"
#include "mem/heap.h"            /* RAY_ATTR_* */
#include "lang/format.h"         /* ray_type_name */
#include <stdio.h>
#include <stdlib.h>             /* abort */

/* Sampling thresholds (DEBUG cost bound). */
enum {
    VALIDATE_FULL_SCAN_N = 1 << 16,  /* <= 65536: scan every element        */
    VALIDATE_HEAD_N      = 4096,     /* large cols: scan this many up front */
    VALIDATE_SAMPLE_N    = 4096,     /* ... plus this many strided samples  */
};

/* Per-type reserved-sentinel test at a single payload index.  Mirrors
 * sentinel_is_null() in vec.c but restricted to the reserved-sentinel set
 * (no SYM/STR/GUID — see scope note above). */
static inline bool reserved_sentinel_at(const ray_t* v, const void* p, int64_t i) {
    switch (v->type) {
        case RAY_F64: { double x = ((const double*)p)[i]; return x != x; }
        case RAY_F32: { float  x = ((const float*)p)[i];  return x != x; }
        case RAY_I64:
        case RAY_TIMESTAMP: return ((const int64_t*)p)[i] == NULL_I64;
        /* I16/I32 (+ DATE/TIME) excluded — wraparound makes MIN a real value
         * (see header).  is_reserved_sentinel_type() rejects them upstream. */
        default:            return false;
    }
}

/* True for the reserved-sentinel types this validator enforces.  I16/I32
 * (and the I32-backed DATE/TIME) are intentionally absent — see the header
 * comment on the documented narrow-int wraparound exception. */
static inline bool is_reserved_sentinel_type(int8_t type) {
    switch (type) {
        case RAY_F64: case RAY_F32:
        case RAY_I64: case RAY_TIMESTAMP:
            return true;
        default:
            return false;
    }
}

static void report_violation(const ray_t* v, int64_t idx) {
    fprintf(stderr,
        "\n[null-invariant 16.4 VIOLATION] column type=%s (tag %d), len=%lld:\n"
        "  payload[%lld] is the type-correct null sentinel but "
        "RAY_ATTR_HAS_NULLS is NOT set.\n"
        "  A producer wrote a sentinel without setting HAS_NULLS — "
        "null-aware ops will treat it as a real value.\n"
        "  Fix: set RAY_ATTR_HAS_NULLS at the producer site that writes the "
        "sentinel.\n\n",
        ray_type_name(v->type), (int)v->type, (long long)v->len,
        (long long)idx);
    fflush(stderr);
    abort();
}

bool ray_check_null_invariant(const ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return true;

    /* Recurse into containers so per-column results inside a table/list/dict
     * are validated too.  LIST/TABLE/DICT all store ray_t* pointers in their
     * data region; len is the slot/element count. */
    if (v->type == RAY_LIST || v->type == RAY_TABLE || v->type == RAY_DICT) {
        ray_t** kids = (ray_t**)ray_data((ray_t*)v);
        for (int64_t i = 0; i < v->len; i++)
            if (kids[i]) (void)ray_check_null_invariant(kids[i]);
        return true;
    }

    /* Only positive-tag vectors carry a payload + HAS_NULLS. */
    if (!ray_is_vec(v)) return true;

    /* Slices don't own the payload and never carry HAS_NULLS — the parent
     * is the authority; skip (the parent is validated where it is produced). */
    if (v->attrs & RAY_ATTR_SLICE) return true;

    if (!is_reserved_sentinel_type(v->type)) return true;  /* no reserved sentinel */
    if (v->len == 0) return true;

    /* HAS_NULLS already set: the invariant is satisfied regardless of payload. */
    if (v->attrs & RAY_ATTR_HAS_NULLS) return true;

    const void* p = ray_data((ray_t*)v);
    const int64_t n = v->len;

    if (n <= VALIDATE_FULL_SCAN_N) {
        for (int64_t i = 0; i < n; i++)
            if (reserved_sentinel_at(v, p, i)) report_violation(v, i);
        return true;
    }

    /* Large column: head scan + strided sample (see sampling note). */
    for (int64_t i = 0; i < VALIDATE_HEAD_N; i++)
        if (reserved_sentinel_at(v, p, i)) report_violation(v, i);

    int64_t rem   = n - VALIDATE_HEAD_N;
    int64_t step  = rem / VALIDATE_SAMPLE_N;
    if (step < 1) step = 1;
    for (int64_t i = VALIDATE_HEAD_N; i < n; i += step)
        if (reserved_sentinel_at(v, p, i)) report_violation(v, i);

    return true;
}

#endif /* DEBUG */

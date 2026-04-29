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

#ifndef RAY_OPS_TEMPORAL_H
#define RAY_OPS_TEMPORAL_H

#include <rayforce.h>
#include "ops/ops.h"

/* Extract a calendar / clock field from a RAY_DATE, RAY_TIME, or
 * RAY_TIMESTAMP input (vector or atom).  `field` is one of the
 * RAY_EXTRACT_* codes from ops/ops.h.
 *
 * Vector input → RAY_I64 vector of the same length, each slot holding the
 * extracted value.  Atom input (type < 0) → RAY_I64 atom.  Returns an
 * error ray_t* if the input isn't a supported temporal type.  The
 * returned value is caller-owned (rc >= 1); caller must ray_release.
 * Does NOT consume the input's refcount. */
ray_t* ray_temporal_extract(ray_t* input, int field);

/* Map a sym_id (used as a dotted-name segment, e.g. `.dd`, `.yyyy`, `.mm`)
 * to a RAY_EXTRACT_* field code.  Returns -1 if the sym isn't a known
 * temporal field name.  Recognised segments:
 *     yyyy      → RAY_EXTRACT_YEAR
 *     mm        → RAY_EXTRACT_MONTH
 *     dd        → RAY_EXTRACT_DAY
 *     hh        → RAY_EXTRACT_HOUR
 *     minute    → RAY_EXTRACT_MINUTE
 *     ss        → RAY_EXTRACT_SECOND
 *     dow       → RAY_EXTRACT_DOW (ISO day-of-week 1..7, Mon=1)
 *     doy       → RAY_EXTRACT_DOY (day-of-year 1..366)
 *
 * `mm` is unambiguously MONTH — MINUTE spelling stays long-form
 * because a two-letter token can't serve both meanings in a uniform
 * dotted walk that has no container-type-dependent dispatch. */
int ray_temporal_field_from_sym(int64_t sym_id);

/* Truncate a temporal value/vector to day boundary (`kind == 0`) or to
 * time-of-day (`kind == 1`, i.e. microseconds within the current day).
 * Returns a freshly-allocated RAY_TIMESTAMP-typed ray_t* (caller-owned);
 * nulls in the input propagate to nulls in the output.  `kind` uses
 * RAY_EXTRACT_DAY (for `.date`) or RAY_EXTRACT_SECOND (for `.time`) so
 * the set of codes stays consistent with exec_date_trunc.  Returns
 * ray_error("type", ...) if input isn't a supported temporal type. */
ray_t* ray_temporal_truncate(ray_t* input, int kind);

/* Dotted-segment sym → truncate kind (see above).  Returns -1 if the
 * segment isn't one of the truncate-flavoured names (`date` / `time`). */
int ray_temporal_trunc_from_sym(int64_t sym_id);

/* Unary builtins: thin wrappers over ray_temporal_extract with the
 * field pre-bound.  Exposed so eval.c can register them alongside the
 * rest of the language's unary functions — `(ss ts)` and `ts.ss` then
 * dispatch through the normal call machinery. */
ray_t* ray_extract_ss_fn(ray_t* x);
ray_t* ray_extract_hh_fn(ray_t* x);
ray_t* ray_extract_minute_fn(ray_t* x);
ray_t* ray_extract_yyyy_fn(ray_t* x);
ray_t* ray_extract_mm_fn(ray_t* x);
ray_t* ray_extract_dd_fn(ray_t* x);
ray_t* ray_extract_dow_fn(ray_t* x);
ray_t* ray_extract_doy_fn(ray_t* x);

#endif /* RAY_OPS_TEMPORAL_H */

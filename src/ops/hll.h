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

#ifndef RAY_OPS_HLL_H
#define RAY_OPS_HLL_H

/**
 * Probabilistic cardinality sketch (HyperLogLog).
 *
 * Each sketch holds 2^P registers; each register stores the maximum
 * leading-zero count (rho) seen for any hash whose top P bits index
 * that register.  Cardinality is then read off the harmonic mean of
 * 2^reg over all registers, with bias correction for both ends of
 * the range.  Standard error ≈ 1.04 / sqrt(2^P).  P=14 → ≈ 0.8 %.
 *
 * Memory: 1 byte per register (8-bit reg holds rho up to 64+P, way
 * over the 6 bits a packed implementation would need; the extra few
 * KB buys a tighter hot loop).  At P=14 a sketch is 16 KB and lives
 * in L2 for the duration of one query.
 *
 * The sketch is mergeable element-wise (max), which is the property
 * the per-group / per-worker aggregation paths rely on: each worker
 * builds a local sketch and the planner merges them at finalisation.
 */

#include "rayforce.h"
#include "ops/hash.h"

/* Default precision: 14 (16384 registers, ~0.81 % std error, 16 KB). */
#define RAY_HLL_DEFAULT_P  14

typedef struct {
    uint8_t  p;        /* precision: register count = 1 << p */
    uint32_t m;        /* register count */
    uint8_t* regs;     /* [m] — 1 byte per register, holds rho count */
    ray_t*   _hdr;     /* scratch handle for regs */
} ray_hll_t;

/* Initialise an empty sketch with `p` precision bits.  Allocates regs
 * via scratch_alloc; the caller frees with ray_hll_free.  Returns 0 on
 * success, -1 on OOM. */
int  ray_hll_init(ray_hll_t* h, uint8_t p);

/* Free the regs allocation.  Safe on a zeroed (uninitialised) sketch. */
void ray_hll_free(ray_hll_t* h);

/* Zero all registers (clears the sketch — same effect as init with the
 * same p, but in-place; useful when reusing a sketch across calls). */
void ray_hll_reset(ray_hll_t* h);

/* Add a 64-bit hash to the sketch.  Caller is responsible for hashing
 * its value type before invoking — see ray_hash_i64 / ray_hash_bytes
 * in ops/hash.h.  Hot path; kept fully inline. */
static inline void ray_hll_add(ray_hll_t* h, uint64_t hash) {
    uint32_t idx = (uint32_t)(hash >> (64u - h->p));
    /* The low (64-p) bits hold the value we scan for the leading-zero
     * run.  Sentinel-bit at position (64-p-1) keeps the rho value in
     * [1, 64-p+1] without a branch on all-zero. */
    uint64_t rest = (hash << h->p) | (1ULL << (h->p - 1));
    uint8_t  rho  = (uint8_t)(__builtin_clzll(rest) + 1u);
    if (rho > h->regs[idx]) h->regs[idx] = rho;
}

/* Merge src into dst (element-wise max).  src and dst must share the
 * same precision p. */
void ray_hll_merge(ray_hll_t* dst, const ray_hll_t* src);

/* Estimate the unique-value count of all hashes added so far.  Uses
 * the standard HyperLogLog estimator with bias-corrected raw-mean for
 * the mid-range and linear counting (m * ln(m/V)) when many registers
 * are still zero (V = unused register count). */
int64_t ray_hll_estimate(const ray_hll_t* h);

/* Scalar approximate `count(distinct …)` over a vec, ~0.8 % standard
 * error.  Handles I64/I32/I16/I8/U8/BOOL/F64/DATE/TIME/TIMESTAMP/SYM/
 * STR.  Nulls are skipped (matches the SQL `count distinct` semantics).
 * Parallelised: each worker builds a private sketch over its row range
 * and the main thread merges them before extracting the estimate.
 * Wired into `exec_count_distinct` above an input-row threshold. */
ray_t* ray_count_distinct_approx(ray_t* x);

/* Per-group approximate `count(distinct …)` over a buffered row-index
 * layout: group g owns the row indices
 *   idx_buf[offsets[g] .. offsets[g] + counts[g]).
 * Parallelised across groups — one task per group, each task uses a
 * private stack-resident HLL so total memory is O(n_workers · 1<<p).
 * Callers holding a row_gid layout instead build idx_buf+offsets+counts
 * once and call this; there's a single per-group kernel.  Writes the
 * estimate to out[gid].  Returns 0 on success, -1 on unsupported type
 * (caller falls back to exact). */
int ray_count_distinct_approx_pg_buf(ray_t* src,
                                      const int64_t* idx_buf,
                                      const int64_t* offsets,
                                      const int64_t* counts,
                                      int64_t n_groups,
                                      uint8_t p, int64_t* out);

#endif /* RAY_OPS_HLL_H */

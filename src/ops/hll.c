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

#include "ops/hll.h"
#include "ops/internal.h"
#include "ops/ops.h"
#include "core/pool.h"
#include "table/sym.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>

int ray_hll_init(ray_hll_t* h, uint8_t p) {
    if (!h) return -1;
    if (p < 4) p = 4;            /* too small loses all accuracy */
    if (p > 18) p = 18;           /* 256 KB cap on register array */
    memset(h, 0, sizeof(*h));
    uint32_t m = 1u << p;
    h->p = p;
    h->m = m;
    h->regs = (uint8_t*)scratch_calloc(&h->_hdr, (size_t)m);
    if (!h->regs) return -1;
    return 0;
}

void ray_hll_free(ray_hll_t* h) {
    if (!h) return;
    if (h->_hdr) scratch_free(h->_hdr);
    h->regs = NULL;
    h->_hdr = NULL;
    h->m = 0;
    h->p = 0;
}

void ray_hll_reset(ray_hll_t* h) {
    if (h && h->regs) memset(h->regs, 0, (size_t)h->m);
}

void ray_hll_merge(ray_hll_t* dst, const ray_hll_t* src) {
    if (!dst || !src || !dst->regs || !src->regs) return;
    if (dst->m != src->m) return;     /* mismatched precision — caller bug */
    const uint8_t* s = src->regs;
    uint8_t*       d = dst->regs;
    uint32_t       m = dst->m;
    /* Branchless max — keeps the hot per-shard merge in vector regs.
     * The compiler usually auto-vectorises this to a packed-max sequence. */
    for (uint32_t i = 0; i < m; i++) {
        uint8_t a = d[i], b = s[i];
        d[i] = a > b ? a : b;
    }
}

/* HyperLogLog cardinality estimator (Flajolet, Fusy, Gandouet, Meunier 2007),
 * with the original raw-estimate / linear-counting hybrid switch.  Skips the
 * HLL++ small-range bias-correction tables because the linear-counting branch
 * already gives a clean estimate below E ≤ 2.5·m, which is where the raw
 * mean diverges from truth. */
int64_t ray_hll_estimate(const ray_hll_t* h) {
    if (!h || !h->regs) return 0;
    uint32_t m = h->m;
    if (m == 0) return 0;

    /* alpha_m correction constant from the paper.  m == 16 / 32 / 64 use
     * the closed-form values; everything else uses 0.7213 / (1 + 1.079/m). */
    double alpha_m;
    if      (m == 16) alpha_m = 0.673;
    else if (m == 32) alpha_m = 0.697;
    else if (m == 64) alpha_m = 0.709;
    else              alpha_m = 0.7213 / (1.0 + 1.079 / (double)m);

    /* Sum of 2^-reg[i].  Count zero registers for the linear-counting
     * fallback at small cardinalities (when V > 0 and E ≤ 2.5·m). */
    double   sum_inv  = 0.0;
    uint32_t n_zeros  = 0;
    for (uint32_t i = 0; i < m; i++) {
        uint8_t r = h->regs[i];
        sum_inv += ldexp(1.0, -(int)r);   /* 2^-r */
        n_zeros += (r == 0);
    }

    double raw = alpha_m * (double)m * (double)m / sum_inv;

    if (raw <= 2.5 * (double)m && n_zeros != 0) {
        /* Linear counting — much tighter than raw for small E. */
        raw = (double)m * log((double)m / (double)n_zeros);
    }
    /* Large-range bias-correction (the 2^32 upper-edge correction in the
     * original paper) is for 32-bit hashes only — we hash 64 bits, so the
     * raw value is already unbiased to ~2^57.  Skip. */

    if (raw < 0.0) raw = 0.0;
    return (int64_t)(raw + 0.5);
}

/* ---- Scalar approximate count-distinct aggregator ---------------------- */

typedef struct {
    const ray_t*  vec;
    int8_t        type;
    uint8_t       attrs;
    bool          has_nulls;
    ray_hll_t*    shards;          /* [n_workers] — one HLL per worker */
    uint8_t       p;
    uint32_t      n_workers;
    _Atomic(int)  oom;
} cda_scalar_ctx_t;

static void cda_scalar_fn(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    cda_scalar_ctx_t* c = (cda_scalar_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    ray_hll_t* sh = &c->shards[worker_id % c->n_workers];
    if (!sh->regs) {
        if (ray_hll_init(sh, c->p) != 0) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
    }
    const ray_t* v = c->vec;
    const void* base = ray_data((ray_t*)v);
    int8_t  t = c->type;
    bool    hn = c->has_nulls;
    const int64_t CHK = 65535;

    if (t == RAY_I64 || t == RAY_TIMESTAMP) {
        const int64_t* d = (const int64_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t v_i = d[r];
            if (hn && v_i == NULL_I64) continue;
            ray_hll_add(sh, ray_hash_i64(v_i));
        }
    } else if (t == RAY_I32 || t == RAY_DATE || t == RAY_TIME) {
        const int32_t* d = (const int32_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int32_t v_i = d[r];
            if (hn && v_i == NULL_I32) continue;
            ray_hll_add(sh, ray_hash_i64((int64_t)v_i));
        }
    } else if (t == RAY_I16) {
        const int16_t* d = (const int16_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int16_t v_i = d[r];
            if (hn && v_i == NULL_I16) continue;
            ray_hll_add(sh, ray_hash_i64((int64_t)v_i));
        }
    } else if (t == RAY_BOOL || t == RAY_U8) {
        const uint8_t* d = (const uint8_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            ray_hll_add(sh, ray_hash_i64((int64_t)d[r]));
        }
    } else if (t == RAY_F64) {
        const double* d = (const double*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            double v_f = d[r];
            if (v_f != v_f) continue;     /* NaN = null in F64 column */
            ray_hll_add(sh, ray_hash_f64(v_f));
        }
    } else if (RAY_IS_SYM(t)) {
        /* SYM is width-encoded — sym id 0 is the canonical empty-string
         * sentinel (treat as null), every other id is a real distinct
         * value, so hash the id directly. */
        uint8_t w = c->attrs & RAY_SYM_W_MASK;
        if (w == RAY_SYM_W64) {
            const int64_t* d = (const int64_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                int64_t v_i = d[r];
                if (v_i == 0) continue;
                ray_hll_add(sh, ray_hash_i64(v_i));
            }
        } else if (w == RAY_SYM_W32) {
            const uint32_t* d = (const uint32_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                uint32_t v_i = d[r];
                if (v_i == 0) continue;
                ray_hll_add(sh, ray_hash_i64((int64_t)v_i));
            }
        } else if (w == RAY_SYM_W16) {
            const uint16_t* d = (const uint16_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                uint16_t v_i = d[r];
                if (v_i == 0) continue;
                ray_hll_add(sh, ray_hash_i64((int64_t)v_i));
            }
        } else {
            const uint8_t* d = (const uint8_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                uint8_t v_i = d[r];
                if (v_i == 0) continue;
                ray_hll_add(sh, ray_hash_i64((int64_t)v_i));
            }
        }
    } else if (t == RAY_STR) {
        ray_t* vm = (ray_t*)v;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            size_t n = 0;
            const char* s = ray_str_vec_get(vm, r, &n);
            if (!s || n == 0) continue;
            ray_hll_add(sh, ray_hash_bytes(s, n));
        }
    }
    /* Unsupported types fall through silently — caller validates. */
}

ray_t* ray_count_distinct_approx(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) {
        /* Scalar atom — distinct count is 1 (or 0 if null). */
        if (ray_is_atom(x)) {
            if (RAY_ATOM_IS_NULL(x)) return ray_i64(0);
            return ray_i64(1);
        }
        return ray_error("type", "count_distinct_approx: vec expected");
    }
    int8_t t = x->type;
    /* Reject types we don't hash. */
    if (t != RAY_I64 && t != RAY_I32 && t != RAY_I16 && t != RAY_U8 &&
        t != RAY_BOOL && t != RAY_F64 && t != RAY_DATE && t != RAY_TIME &&
        t != RAY_TIMESTAMP && t != RAY_STR && !RAY_IS_SYM(t))
        return ray_error("type", "count_distinct_approx: unsupported element type");
    int64_t n = x->len;
    if (n == 0) return ray_i64(0);

    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = (pool && n >= RAY_PARALLEL_THRESHOLD)
                  ? ray_pool_total_workers(pool) : 1;

    ray_t* shards_hdr = NULL;
    ray_hll_t* shards = (ray_hll_t*)scratch_calloc(
        &shards_hdr, (size_t)nw * sizeof(ray_hll_t));
    if (!shards) return ray_error("oom", NULL);

    cda_scalar_ctx_t ctx = {
        .vec = x,
        .type = t,
        .attrs = x->attrs,
        .has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0,
        .shards = shards,
        .p = RAY_HLL_DEFAULT_P,
        .n_workers = nw,
        .oom = 0,
    };
    if (nw > 1) {
        ray_pool_dispatch(pool, cda_scalar_fn, &ctx, n);
    } else {
        cda_scalar_fn(&ctx, 0, 0, n);
    }
    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        for (uint32_t w = 0; w < nw; w++) ray_hll_free(&shards[w]);
        scratch_free(shards_hdr);
        return ray_error("oom", "count_distinct_approx: HLL alloc failed");
    }
    /* Merge per-worker shards into shard[0], then estimate. */
    for (uint32_t w = 1; w < nw; w++) {
        if (shards[w].regs)
            ray_hll_merge(&shards[0], &shards[w]);
    }
    int64_t est = shards[0].regs ? ray_hll_estimate(&shards[0]) : 0;
    for (uint32_t w = 0; w < nw; w++) ray_hll_free(&shards[w]);
    scratch_free(shards_hdr);
    return ray_i64(est);
}

/* ---- Per-group HLL --------------------------------------------------- */

typedef struct {
    const ray_t*   vec;
    int8_t         type;
    uint8_t        attrs;
    bool           has_nulls;
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* counts;       /* per-group length — offsets has only n_groups entries */
    uint8_t        p;
    uint32_t       m;
    int64_t*       out;
    _Atomic(int)   oom;
} cda_pg_buf_ctx_t;

static void cda_pg_buf_task(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    cda_pg_buf_ctx_t* c = (cda_pg_buf_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    const void* base = ray_data((ray_t*)c->vec);
    int8_t  t  = c->type;
    bool    hn = c->has_nulls;

    /* One private HLL per task (allocated on stack so we never touch
     * the shared scratch arena from a worker thread).  P≤14 → m≤16384,
     * fits comfortably in the default 8 MiB worker stack. */
    uint8_t regs[1u << 14];
    ray_hll_t sk = { .p = c->p, .m = c->m, .regs = regs, ._hdr = NULL };

    for (int64_t g = start; g < end; g++) {
        memset(regs, 0, c->m);
        int64_t s = c->offsets[g];
        int64_t e = s + c->counts[g];
        if (t == RAY_I64 || t == RAY_TIMESTAMP) {
            const int64_t* d = (const int64_t*)base;
            for (int64_t k = s; k < e; k++) {
                int64_t r = c->idx_buf[k];
                int64_t v = d[r];
                if (hn && v == NULL_I64) continue;
                ray_hll_add(&sk, ray_hash_i64(v));
            }
        } else if (t == RAY_I32 || t == RAY_DATE || t == RAY_TIME) {
            const int32_t* d = (const int32_t*)base;
            for (int64_t k = s; k < e; k++) {
                int64_t r = c->idx_buf[k];
                int32_t v = d[r];
                if (hn && v == NULL_I32) continue;
                ray_hll_add(&sk, ray_hash_i64((int64_t)v));
            }
        } else if (t == RAY_I16) {
            const int16_t* d = (const int16_t*)base;
            for (int64_t k = s; k < e; k++) {
                int64_t r = c->idx_buf[k];
                int16_t v = d[r];
                if (hn && v == NULL_I16) continue;
                ray_hll_add(&sk, ray_hash_i64((int64_t)v));
            }
        } else if (t == RAY_BOOL || t == RAY_U8) {
            const uint8_t* d = (const uint8_t*)base;
            for (int64_t k = s; k < e; k++) {
                int64_t r = c->idx_buf[k];
                ray_hll_add(&sk, ray_hash_i64((int64_t)d[r]));
            }
        } else if (t == RAY_F64) {
            const double* d = (const double*)base;
            for (int64_t k = s; k < e; k++) {
                int64_t r = c->idx_buf[k];
                double v = d[r];
                if (v != v) continue;
                ray_hll_add(&sk, ray_hash_f64(v));
            }
        } else if (RAY_IS_SYM(t)) {
            uint8_t w = c->attrs & RAY_SYM_W_MASK;
            if (w == RAY_SYM_W64) {
                const int64_t* d = (const int64_t*)base;
                for (int64_t k = s; k < e; k++) {
                    int64_t r = c->idx_buf[k];
                    int64_t v = d[r]; if (v == 0) continue;
                    ray_hll_add(&sk, ray_hash_i64(v));
                }
            } else if (w == RAY_SYM_W32) {
                const uint32_t* d = (const uint32_t*)base;
                for (int64_t k = s; k < e; k++) {
                    int64_t r = c->idx_buf[k];
                    uint32_t v = d[r]; if (v == 0) continue;
                    ray_hll_add(&sk, ray_hash_i64((int64_t)v));
                }
            } else if (w == RAY_SYM_W16) {
                const uint16_t* d = (const uint16_t*)base;
                for (int64_t k = s; k < e; k++) {
                    int64_t r = c->idx_buf[k];
                    uint16_t v = d[r]; if (v == 0) continue;
                    ray_hll_add(&sk, ray_hash_i64((int64_t)v));
                }
            } else {
                const uint8_t* d = (const uint8_t*)base;
                for (int64_t k = s; k < e; k++) {
                    int64_t r = c->idx_buf[k];
                    uint8_t v = d[r]; if (v == 0) continue;
                    ray_hll_add(&sk, ray_hash_i64((int64_t)v));
                }
            }
        }
        c->out[g] = ray_hll_estimate(&sk);
    }
}

int ray_count_distinct_approx_pg_buf(ray_t* src,
                                      const int64_t* idx_buf,
                                      const int64_t* offsets,
                                      const int64_t* counts,
                                      int64_t n_groups,
                                      uint8_t p, int64_t* out)
{
    if (!src || RAY_IS_ERR(src) || !idx_buf || !offsets || !counts || !out)
        return -1;
    int8_t t = src->type;
    bool hashable = (t == RAY_I64 || t == RAY_I32 || t == RAY_I16 ||
                      t == RAY_U8 || t == RAY_BOOL || t == RAY_F64 ||
                      t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP ||
                      RAY_IS_SYM(t));
    if (!hashable) return -1;
    if (n_groups <= 0) return 0;
    if (p < 4) p = 4;
    if (p > 14) p = 14;
    uint32_t m = 1u << p;

    cda_pg_buf_ctx_t ctx = {
        .vec = src,
        .type = t,
        .attrs = src->attrs,
        .has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0,
        .idx_buf = idx_buf,
        .offsets = offsets,
        .counts = counts,
        .p = p,
        .m = m,
        .out = out,
        .oom = 0,
    };
    ray_pool_t* pool = ray_pool_get();
    if (pool && ray_pool_total_workers(pool) >= 2 && n_groups >= 4) {
        ray_pool_dispatch_n(pool, cda_pg_buf_task, &ctx, (uint32_t)n_groups);
    } else {
        cda_pg_buf_task(&ctx, 0, 0, n_groups);
    }
    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) return -1;
    return 0;
}

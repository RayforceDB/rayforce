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

void ray_hll_init_sparse(ray_hll_t* h, uint8_t p,
                          uint32_t* sparse_buf, uint32_t sparse_cap,
                          uint8_t* dense_buf) {
    if (!h) return;
    if (p < 4) p = 4;
    if (p > 18) p = 18;
    memset(h, 0, sizeof(*h));
    h->p = p;
    h->m = 1u << p;
    /* Encode caller-owned dense buffer as a tagged pointer in _hdr —
     * low bit set ⇒ caller-owned (skip free), clear ⇒ scratch ray_t*.
     * promote_to_dense recovers it; ray_hll_free skips the scratch_free.
     * Stack allocations on x86-64 are at least 8-byte aligned for arrays
     * of this size, so the low bit is always free for tagging. */
    assert(((uintptr_t)dense_buf & 1u) == 0);
    uintptr_t tagged = (uintptr_t)dense_buf | (uintptr_t)1;
    h->_hdr = (ray_t*)tagged;
    h->sparse_keys = sparse_buf;
    h->sparse_count = 0;
    h->sparse_cap = sparse_cap;
}

/* Recover the caller-owned dense buffer (NULL if none).  Used by
 * promote_to_dense to install regs without a scratch alloc. */
static inline uint8_t* hll_caller_dense_buf(const ray_hll_t* h) {
    uintptr_t tagged = (uintptr_t)h->_hdr;
    if (!(tagged & 1)) return NULL;
    return (uint8_t*)(tagged & ~(uintptr_t)1);
}

void ray_hll_promote_to_dense(ray_hll_t* h) {
    if (!h || h->regs) return;       /* already dense */
    uint8_t* dense = hll_caller_dense_buf(h);
    if (!dense) {
        /* No caller buffer — fall back to scratch alloc.  Used by
         * merge paths that promote a sparse src whose owner is the
         * caller's stack but dst is heap-resident; we materialise a
         * fresh dense buffer through the scratch arena. */
        ray_t* hdr = NULL;
        dense = (uint8_t*)scratch_calloc(&hdr, (size_t)h->m);
        if (!dense) {
            /* OOM during promote.  Leave sparse; caller's estimate
             * will overflow into a small under-count.  This branch is
             * extremely rare (the dense buffer is 16 KB at P=14). */
            return;
        }
        h->_hdr = hdr;
    } else {
        /* Caller-owned: clear and install. */
        memset(dense, 0, (size_t)h->m);
        h->_hdr = NULL;  /* drop tagged pointer; no longer needed */
    }
    h->regs = dense;
    /* Replay sparse entries into dense (max). */
    uint32_t* sk = h->sparse_keys;
    uint32_t  n  = h->sparse_count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = sk[i];
        uint32_t idx = v >> 8;
        uint8_t  rho = (uint8_t)(v & 0xFF);
        if (rho > dense[idx]) dense[idx] = rho;
    }
    h->sparse_keys = NULL;
    h->sparse_count = 0;
    h->sparse_cap = 0;
}

void ray_hll_free(ray_hll_t* h) {
    if (!h) return;
    /* Only free if _hdr is a real scratch handle (low bit clear, non-NULL).
     * Tagged caller-owned buffers and NULL _hdr are both no-ops. */
    uintptr_t tagged = (uintptr_t)h->_hdr;
    if (h->_hdr && !(tagged & 1)) scratch_free(h->_hdr);
    h->regs = NULL;
    h->_hdr = NULL;
    h->sparse_keys = NULL;
    h->sparse_count = 0;
    h->sparse_cap = 0;
    h->m = 0;
    h->p = 0;
}

void ray_hll_reset(ray_hll_t* h) {
    if (!h) return;
    if (h->regs) {
        memset(h->regs, 0, (size_t)h->m);
        return;
    }
    if (h->sparse_keys) {
        /* Don't memset the sparse buffer — entries are only read up to
         * sparse_count, so clearing the count is enough. */
        h->sparse_count = 0;
    }
}

/* Merge a sparse src into a dense dst.  Each src entry contributes a
 * rho-update at its idx slot. */
static inline void hll_merge_sparse_into_dense(uint8_t* d,
                                               const uint32_t* sk,
                                               uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = sk[i];
        uint32_t idx = v >> 8;
        uint8_t  rho = (uint8_t)(v & 0xFF);
        if (rho > d[idx]) d[idx] = rho;
    }
}

void ray_hll_merge(ray_hll_t* dst, const ray_hll_t* src) {
    if (!dst || !src) return;
    if (dst->m != src->m) return;     /* mismatched precision — caller bug */
    /* Promote dst to dense first if needed (cheap: at most 256 entries).
     * dst's caller-owned dense buffer (if any) gets used; otherwise
     * promote_to_dense scratch-allocates. */
    if (!dst->regs) {
        ray_hll_promote_to_dense(dst);
        if (!dst->regs) return;       /* promote OOM — best-effort skip */
    }
    if (src->regs) {
        const uint8_t* s = src->regs;
        uint8_t*       d = dst->regs;
        uint32_t       m = dst->m;
        /* Branchless max — keeps the hot per-shard merge in vector regs.
         * The compiler usually auto-vectorises this to a packed-max sequence. */
        for (uint32_t i = 0; i < m; i++) {
            uint8_t a = d[i], b = s[i];
            d[i] = a > b ? a : b;
        }
    } else if (src->sparse_keys) {
        hll_merge_sparse_into_dense(dst->regs, src->sparse_keys,
                                    src->sparse_count);
    }
}

/* HyperLogLog cardinality estimator (Flajolet, Fusy, Gandouet, Meunier 2007),
 * with the original raw-estimate / linear-counting hybrid switch.  Skips the
 * HLL++ small-range bias-correction tables because the linear-counting branch
 * already gives a clean estimate below E ≤ 2.5·m, which is where the raw
 * mean diverges from truth. */
int64_t ray_hll_estimate(const ray_hll_t* h) {
    if (!h) return 0;
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
     * fallback at small cardinalities (when V > 0 and E ≤ 2.5·m).
     * Sparse mode: only iterate the entries (each rho>=1 by construction);
     * the remaining (m - sparse_count) registers contribute 2^0 = 1 each
     * and count as zero registers. */
    double   sum_inv  = 0.0;
    uint32_t n_zeros  = 0;
    if (h->regs) {
        for (uint32_t i = 0; i < m; i++) {
            uint8_t r = h->regs[i];
            sum_inv += ldexp(1.0, -(int)r);   /* 2^-r */
            n_zeros += (r == 0);
        }
    } else if (h->sparse_keys) {
        uint32_t n = h->sparse_count;
        const uint32_t* sk = h->sparse_keys;
        /* Each entry stores a unique register idx (linear-probe dedup
         * guarantees this).  Unset registers contribute 2^0 = 1.0 each
         * and count as zeros. */
        sum_inv = (double)(m - n);
        n_zeros = m - n;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t r = (uint8_t)(sk[i] & 0xFF);
            sum_inv += ldexp(1.0, -(int)r);
        }
    } else {
        /* Uninitialised — all m registers are conceptually zero. */
        sum_inv = (double)m;
        n_zeros = m;
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
        .has_nulls = ray_vec_may_have_nulls(x),
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
    /* Merge per-worker shards into the first INITIALIZED shard, then estimate.
     * Under dynamic worker scheduling a thread (including worker 0) may process
     * no chunk, leaving its shard zero-initialized (regs==NULL, m==0).  Merging
     * INTO such a shard fails ray_hll_merge's precision check (0 != m) and
     * silently skips every merge, yielding a spurious 0 estimate.  Pick the
     * first shard that actually saw data as the merge base. */
    uint32_t base = 0;
    while (base < nw && !shards[base].regs) base++;
    int64_t est = 0;
    if (base < nw) {
        for (uint32_t w = base + 1; w < nw; w++) {
            if (shards[w].regs)
                ray_hll_merge(&shards[base], &shards[w]);
        }
        est = ray_hll_estimate(&shards[base]);
    }
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
     * fits comfortably in the default 8 MiB worker stack.
     *
     * Sparse start: the sketch begins in sparse mode using sparse_buf
     * (256 entries, 1 KB).  Groups with few distinct values never touch
     * the dense register array; once the sparse cap is hit on a group,
     * promote_to_dense moves it into the stack regs[] buffer.  The
     * dense buffer is unconditionally allocated on the stack so the
     * promotion path is alloc-free. */
    uint8_t  regs[1u << 14];
    uint32_t sparse_buf[RAY_HLL_SPARSE_CAP];
    ray_hll_t sk;

    for (int64_t g = start; g < end; g++) {
        ray_hll_init_sparse(&sk, c->p, sparse_buf,
                            RAY_HLL_SPARSE_CAP, regs);
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
        .has_nulls = ray_vec_may_have_nulls(src),
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
        /* dispatch_n issues exactly n_groups tasks of [i, i+1), but the
         * task ring is hard-capped at 65536 so n_groups > 65536 would
         * silently drop trailing groups.  For high-cardinality grouping
         * use element-based dispatch — each worker gets a range of
         * groups, processes them serially, and reuses its stack sketch
         * across the range. */
        if (n_groups <= 65536) {
            ray_pool_dispatch_n(pool, cda_pg_buf_task, &ctx, (uint32_t)n_groups);
        } else {
            ray_pool_dispatch(pool, cda_pg_buf_task, &ctx, n_groups);
        }
    } else {
        cda_pg_buf_task(&ctx, 0, 0, n_groups);
    }
    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) return -1;
    return 0;
}

/* ---- Streaming per-group HLL ----------------------------------------- */

/* Streaming kernel layout
 * -----------------------
 * Each worker owns a contiguous *bank* of n_groups HLL sketches.  Memory
 * for a bank is one slab allocated up-front (sketches + sparse keys +
 * dense regs) so the per-row hot loop is alloc-free.  Each sketch starts
 * sparse; ray_hll_add transparently promotes to its caller-owned dense
 * buffer once the sparse cap is exceeded.
 *
 * After the streaming pass, banks are merged element-wise (max) into
 * bank[0] and the per-group estimates are written to out[gid].
 */

typedef struct {
    /* Per-worker bank base pointers.  Each bank holds n_groups sketches
     * whose `sparse_keys` / dense slots point into the per-worker pool. */
    ray_hll_t**      banks;          /* [n_workers] */
    /* Constant inputs. */
    const ray_t*     vec;
    const int64_t*   row_gid;
    int64_t          n_rows;
    int64_t          n_groups;
    int8_t           type;
    uint8_t          attrs;
    bool             has_nulls;
    uint8_t          p;
    uint32_t         m;
} cda_pg_stream_ctx_t;

/* Worker per-row body — picks up the bank for this worker, decodes the
 * column-type once into a local pointer, and updates bank[gid] for each
 * row in the assigned range. */
static void cda_pg_stream_task(void* raw, uint32_t worker_id,
                               int64_t start, int64_t end) {
    cda_pg_stream_ctx_t* c = (cda_pg_stream_ctx_t*)raw;
    ray_hll_t* bank = c->banks[worker_id];
    if (!bank) return;
    const void*    base    = ray_data((ray_t*)c->vec);
    const int64_t* row_gid = c->row_gid;
    int64_t        ng      = c->n_groups;
    int8_t         t       = c->type;
    bool           hn      = c->has_nulls;
    const int64_t  CHK     = 65535;

    if (t == RAY_I64 || t == RAY_TIMESTAMP) {
        const int64_t* d = (const int64_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= ng) continue;
            int64_t v = d[r];
            if (hn && v == NULL_I64) continue;
            ray_hll_add(&bank[gid], ray_hash_i64(v));
        }
    } else if (t == RAY_I32 || t == RAY_DATE || t == RAY_TIME) {
        const int32_t* d = (const int32_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= ng) continue;
            int32_t v = d[r];
            if (hn && v == NULL_I32) continue;
            ray_hll_add(&bank[gid], ray_hash_i64((int64_t)v));
        }
    } else if (t == RAY_I16) {
        const int16_t* d = (const int16_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= ng) continue;
            int16_t v = d[r];
            if (hn && v == NULL_I16) continue;
            ray_hll_add(&bank[gid], ray_hash_i64((int64_t)v));
        }
    } else if (t == RAY_BOOL || t == RAY_U8) {
        const uint8_t* d = (const uint8_t*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= ng) continue;
            ray_hll_add(&bank[gid], ray_hash_i64((int64_t)d[r]));
        }
    } else if (t == RAY_F64) {
        const double* d = (const double*)base;
        for (int64_t r = start; r < end; r++) {
            if (((r - start) & CHK) == 0 && ray_interrupted()) return;
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= ng) continue;
            double v = d[r];
            if (v != v) continue;
            ray_hll_add(&bank[gid], ray_hash_f64(v));
        }
    } else if (RAY_IS_SYM(t)) {
        uint8_t w = c->attrs & RAY_SYM_W_MASK;
        if (w == RAY_SYM_W64) {
            const int64_t* d = (const int64_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= ng) continue;
                int64_t v = d[r]; if (v == 0) continue;
                ray_hll_add(&bank[gid], ray_hash_i64(v));
            }
        } else if (w == RAY_SYM_W32) {
            const uint32_t* d = (const uint32_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= ng) continue;
                uint32_t v = d[r]; if (v == 0) continue;
                ray_hll_add(&bank[gid], ray_hash_i64((int64_t)v));
            }
        } else if (w == RAY_SYM_W16) {
            const uint16_t* d = (const uint16_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= ng) continue;
                uint16_t v = d[r]; if (v == 0) continue;
                ray_hll_add(&bank[gid], ray_hash_i64((int64_t)v));
            }
        } else {
            const uint8_t* d = (const uint8_t*)base;
            for (int64_t r = start; r < end; r++) {
                if (((r - start) & CHK) == 0 && ray_interrupted()) return;
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= ng) continue;
                uint8_t v = d[r]; if (v == 0) continue;
                ray_hll_add(&bank[gid], ray_hash_i64((int64_t)v));
            }
        }
    }
}

int ray_count_distinct_approx_pg_stream(ray_t* src,
                                         const int64_t* row_gid,
                                         int64_t n_rows,
                                         int64_t n_groups,
                                         uint8_t p, int64_t* out)
{
    if (!src || RAY_IS_ERR(src) || !row_gid || !out) return -1;
    if (n_rows <= 0 || n_groups <= 0) return -1;
    int8_t t = src->type;
    bool hashable = (t == RAY_I64 || t == RAY_I32 || t == RAY_I16 ||
                      t == RAY_U8 || t == RAY_BOOL || t == RAY_F64 ||
                      t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP ||
                      RAY_IS_SYM(t));
    if (!hashable) return -1;
    if (p < 4) p = 4;
    if (p > 14) p = 14;
    uint32_t m = 1u << p;

    /* Choose worker count from the existing parallel threshold; the pool
     * dispatcher partitions n_rows into morsels across n_workers + main. */
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = (pool && n_rows >= RAY_PARALLEL_THRESHOLD)
                  ? ray_pool_total_workers(pool) : 1;

    /* Allocate per-worker banks.  One slab per worker: sketches array,
     * then sparse-key pool (n_groups * RAY_HLL_SPARSE_CAP * 4 bytes),
     * then dense-regs pool (n_groups * m bytes).  Pre-allocating dense
     * means promotion in the hot loop is a memset + replay, alloc-free. */
    ray_t* banks_hdr = NULL;
    ray_hll_t** banks = (ray_hll_t**)scratch_calloc(
        &banks_hdr, (size_t)nw * sizeof(ray_hll_t*));
    if (!banks) return -1;

    /* Per-worker scratch headers, freed at end. */
    ray_t** slab_hdrs_array = NULL;
    ray_t* slab_hdrs_hdr = NULL;
    slab_hdrs_array = (ray_t**)scratch_calloc(
        &slab_hdrs_hdr, (size_t)nw * sizeof(ray_t*));
    if (!slab_hdrs_array) {
        scratch_free(banks_hdr);
        return -1;
    }

    size_t sketches_bytes = (size_t)n_groups * sizeof(ray_hll_t);
    size_t sparse_bytes   = (size_t)n_groups *
                             RAY_HLL_SPARSE_CAP * sizeof(uint32_t);
    size_t dense_bytes    = (size_t)n_groups * (size_t)m;
    size_t per_worker     = sketches_bytes + sparse_bytes + dense_bytes;

    bool oom = false;
    for (uint32_t w = 0; w < nw; w++) {
        ray_t* slab_hdr = NULL;
        uint8_t* slab = (uint8_t*)scratch_alloc(&slab_hdr, per_worker);
        if (!slab) { oom = true; break; }
        slab_hdrs_array[w] = slab_hdr;
        ray_hll_t* sketches = (ray_hll_t*)slab;
        uint32_t*  sparse   = (uint32_t*)(slab + sketches_bytes);
        uint8_t*   dense    = slab + sketches_bytes + sparse_bytes;
        /* Init each sketch sparse, pointed at its slice of the pools. */
        for (int64_t g = 0; g < n_groups; g++) {
            ray_hll_init_sparse(&sketches[g], p,
                                sparse + (size_t)g * RAY_HLL_SPARSE_CAP,
                                RAY_HLL_SPARSE_CAP,
                                dense + (size_t)g * m);
        }
        banks[w] = sketches;
    }
    if (oom) {
        for (uint32_t w = 0; w < nw; w++) {
            if (slab_hdrs_array[w]) scratch_free(slab_hdrs_array[w]);
        }
        scratch_free(slab_hdrs_hdr);
        scratch_free(banks_hdr);
        return -1;
    }

    cda_pg_stream_ctx_t ctx = {
        .banks    = banks,
        .vec      = src,
        .row_gid  = row_gid,
        .n_rows   = n_rows,
        .n_groups = n_groups,
        .type     = t,
        .attrs    = src->attrs,
        .has_nulls = ray_vec_may_have_nulls(src),
        .p        = p,
        .m        = m,
    };

    if (nw > 1) {
        ray_pool_dispatch(pool, cda_pg_stream_task, &ctx, n_rows);
    } else {
        cda_pg_stream_task(&ctx, 0, 0, n_rows);
    }

    /* Merge worker banks into bank[0], then estimate per group.
     *
     * Per gid: merge bank[1..nw-1][gid] into bank[0][gid].  ray_hll_merge
     * handles both (sparse|dense) × (sparse|dense) combinations and
     * promotes dst as needed.  After merge, bank[0][gid] estimate is the
     * answer.  We merge gid-by-gid (rather than worker-by-worker over all
     * gids) so a finished dst stays hot across estimation. */
    for (int64_t g = 0; g < n_groups; g++) {
        ray_hll_t* dst = &banks[0][g];
        for (uint32_t w = 1; w < nw; w++) {
            ray_hll_merge(dst, &banks[w][g]);
        }
        out[g] = ray_hll_estimate(dst);
    }

    /* Free per-worker slabs.  Caller-owned sparse + dense buffers were
     * not separately allocated, so ray_hll_free is a no-op on each
     * sketch (low-bit-tagged _hdr or NULL _hdr).  Promotion-time scratch
     * allocations (when promote_to_dense needed an arena dense buf — only
     * possible if the caller's tagged buf had been cleared, which doesn't
     * happen here since dense was provided up-front) are owned by the
     * sketch's _hdr; if any are present, ray_hll_free releases them. */
    for (uint32_t w = 0; w < nw; w++) {
        for (int64_t g = 0; g < n_groups; g++) ray_hll_free(&banks[w][g]);
        scratch_free(slab_hdrs_array[w]);
    }
    scratch_free(slab_hdrs_hdr);
    scratch_free(banks_hdr);
    return 0;
}

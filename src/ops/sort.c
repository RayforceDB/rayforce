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
#include "ops/ops.h"
#include "ops/idxop.h"
#include "mem/sys.h"

/* --------------------------------------------------------------------------
 * Sort comparator: compare two row indices across all sort keys.
 * Returns negative if a < b, positive if a > b, 0 if equal.
 * -------------------------------------------------------------------------- */
/* sort_cmp_ctx_t defined in exec_internal.h */

int sort_cmp(const sort_cmp_ctx_t* ctx, int64_t a, int64_t b) {
    for (uint32_t k = 0; k < ctx->n_sort; k++) {
        ray_t* col = ctx->vecs[k];
        if (!col) continue;
        int cmp = 0;
        int null_cmp = 0;
        int desc = ctx->desc ? ctx->desc[k] : 0;
        int nf = ctx->nulls_first ? ctx->nulls_first[k] : desc;

        /* Check null bitmap for both elements */
        int a_null = ray_vec_is_null(col, a);
        int b_null = ray_vec_is_null(col, b);
        if (a_null || b_null) {
            null_cmp = 1;
            if (a_null && b_null) cmp = 0;
            else if (a_null) cmp = nf ? -1 : 1;
            else cmp = nf ? 1 : -1;
        } else if (col->type == RAY_F64) {
            double va = ((double*)ray_data(col))[a];
            double vb = ((double*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_F32) {
            float va = ((float*)ray_data(col))[a];
            float vb = ((float*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
            int64_t va = ((int64_t*)ray_data(col))[a];
            int64_t vb = ((int64_t*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_I32) {
            int32_t va = ((int32_t*)ray_data(col))[a];
            int32_t vb = ((int32_t*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (RAY_IS_SYM(col->type)) {
            /* Lex compare materializes strings: both ids are CELL DATA
             * of this column, so they resolve through ITS domain
             * (sym-domain Phase 2; runtime singleton delegates to
             * ray_sym_str — exact no-op pre-flip). */
            struct ray_sym_domain_s* dom = ray_sym_vec_domain(col);
            int64_t va = ray_read_sym(ray_data(col), a, col->type, col->attrs);
            int64_t vb = ray_read_sym(ray_data(col), b, col->type, col->attrs);
            ray_t* sa = ray_sym_domain_str(dom, va);
            ray_t* sb = ray_sym_domain_str(dom, vb);
            if (sa && sb) cmp = ray_str_cmp(sa, sb);
        } else if (col->type == RAY_I16) {
            int16_t va = ((int16_t*)ray_data(col))[a];
            int16_t vb = ((int16_t*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
            uint8_t va = ((uint8_t*)ray_data(col))[a];
            uint8_t vb = ((uint8_t*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_DATE || col->type == RAY_TIME) {
            int32_t va = ((int32_t*)ray_data(col))[a];
            int32_t vb = ((int32_t*)ray_data(col))[b];
            if (va < vb) cmp = -1;
            else if (va > vb) cmp = 1;
        } else if (col->type == RAY_GUID) {
            const uint8_t* base = (const uint8_t*)ray_data(col);
            cmp = memcmp(base + a * 16, base + b * 16, 16);
        } else if (col->type == RAY_STR) {
            const ray_str_t* elems;
            const char* pool;
            str_resolve(col, &elems, &pool);
            cmp = ray_str_t_cmp(&elems[a], pool, &elems[b], pool);
        }

        if (desc && !null_cmp) cmp = -cmp;
        if (cmp != 0) return cmp;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Small-array sort: introsort on (key, idx) pairs.
 *
 * For arrays ≤ RADIX_SORT_THRESHOLD, a single-pass encode + comparison sort
 * beats multi-pass radix sort.  Uses quicksort with median-of-3 pivot and
 * heapsort fallback (introsort) to guarantee O(n log n) worst case.
 * -------------------------------------------------------------------------- */

/* RADIX_SORT_THRESHOLD, SMALL_POOL_THRESHOLD defined in exec_internal.h */

/* Total-order comparison of (key, idx) pairs.  Equal keys break ties on the
 * carried index, which callers seed as the original iota (radix_encode_fn does
 * idx[i]=i).  Because those indices are unique, this is a strict total order
 * with no true ties, so the comparison sorts below — quicksort included —
 * produce the UNIQUE (key,idx)-ascending permutation: equal keys retain their
 * original storage order (a STABLE sort).  This matches the ≤64 merge-sort path
 * and the >4096 packed-radix path (both stable), so all three size regimes now
 * resolve equal-key ties identically.  The streaming asof carry relies on this:
 * `(last col) by:eq` over a time-pre-sorted qtab must pick the same tied row the
 * asof kernel's rightmost-<= binary search would. */
static inline bool key_lt(uint64_t ka, int64_t ia, uint64_t kb, int64_t ib) {
    return ka < kb || (ka == kb && ia < ib);
}

static void key_sift_down(uint64_t* keys, int64_t* idx, int64_t n, int64_t i) {
    for (;;) {
        int64_t largest = i, l = 2*i+1, r = 2*i+2;
        if (l < n && key_lt(keys[largest], idx[largest], keys[l], idx[l])) largest = l;
        if (r < n && key_lt(keys[largest], idx[largest], keys[r], idx[r])) largest = r;
        if (largest == i) return;
        uint64_t tk = keys[i]; keys[i] = keys[largest]; keys[largest] = tk;
        int64_t  ti = idx[i];  idx[i]  = idx[largest];  idx[largest]  = ti;
        i = largest;
    }
}

static void key_heapsort(uint64_t* keys, int64_t* idx, int64_t n) {
    for (int64_t i = n/2 - 1; i >= 0; i--)
        key_sift_down(keys, idx, n, i);
    for (int64_t i = n - 1; i > 0; i--) {
        uint64_t tk = keys[0]; keys[0] = keys[i]; keys[i] = tk;
        int64_t  ti = idx[0];  idx[0]  = idx[i];  idx[i]  = ti;
        key_sift_down(keys, idx, i, 0);
    }
}

static void key_insertion_sort(uint64_t* keys, int64_t* idx, int64_t n) {
    for (int64_t i = 1; i < n; i++) {
        uint64_t kk = keys[i];
        int64_t  ii = idx[i];
        int64_t j = i - 1;
        while (j >= 0 && key_lt(kk, ii, keys[j], idx[j])) {
            keys[j+1] = keys[j];
            idx[j+1]  = idx[j];
            j--;
        }
        keys[j+1] = kk;
        idx[j+1]  = ii;
    }
}

static void key_introsort_impl(uint64_t* keys, int64_t* idx,
                                 int64_t n, int depth) {
    while (n > 32) {
        if (depth == 0) {
            key_heapsort(keys, idx, n);
            return;
        }
        depth--;

        /* Median-of-3 pivot */
        int64_t mid = n / 2;
        uint64_t a = keys[0], b = keys[mid], c = keys[n-1];
        int64_t pi;
        if (a < b) pi = (b < c) ? mid : (a < c ? n-1 : 0);
        else       pi = (a < c) ? 0   : (b < c ? n-1 : mid);

        /* Move pivot to end */
        uint64_t pk = keys[pi]; keys[pi] = keys[n-1]; keys[n-1] = pk;
        int64_t  pv = idx[pi];  idx[pi]  = idx[n-1];  idx[n-1]  = pv;

        /* Partition */
        int64_t lo = 0;
        for (int64_t i = 0; i < n - 1; i++) {
            if (key_lt(keys[i], idx[i], pk, pv)) {
                uint64_t tk = keys[i]; keys[i] = keys[lo]; keys[lo] = tk;
                int64_t  ti = idx[i];  idx[i]  = idx[lo];  idx[lo]  = ti;
                lo++;
            }
        }
        keys[n-1] = keys[lo]; keys[lo] = pk;
        idx[n-1]  = idx[lo];  idx[lo]  = pv;

        /* Recurse on smaller partition, iterate on larger */
        if (lo < n - 1 - lo) {
            key_introsort_impl(keys, idx, lo, depth);
            keys += lo + 1; idx += lo + 1; n -= lo + 1;
        } else {
            key_introsort_impl(keys + lo + 1, idx + lo + 1, n - lo - 1, depth);
            n = lo;
        }
    }
    key_insertion_sort(keys, idx, n);
}

/* Sort (key, idx) pairs in-place by key.  O(n log n) guaranteed. */
void key_introsort(uint64_t* keys, int64_t* idx, int64_t n) {
    if (n <= 1) return;
    int depth = 0;
    for (int64_t nn = n; nn > 1; nn >>= 1) depth++;
    depth *= 2;
    key_introsort_impl(keys, idx, n, depth);
}

/* --------------------------------------------------------------------------
 * Adaptive pre-sort detection.
 *
 * Scans encoded keys to detect already-sorted and nearly-sorted data.
 * Returns a sortedness metric: fraction of out-of-order pairs [0.0, 1.0].
 *   0.0 = perfectly sorted → skip sort entirely
 *   small = nearly sorted → prefer comparison-based sort (adaptive mergesort)
 *   large = random → use radix sort
 * -------------------------------------------------------------------------- */

typedef struct {
    const uint64_t* keys;
    int64_t*        pw_unsorted; /* per-worker out-of-order count */
} sortedness_ctx_t;

/* Each worker counts out-of-order pairs in [start, end).
 * Also checks the boundary: keys[start-1] vs keys[start] (for start > 0). */
static void sortedness_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    sortedness_ctx_t* c = (sortedness_ctx_t*)arg;
    const uint64_t* keys = c->keys;
    int64_t unsorted = 0;
    for (int64_t i = start + 1; i < end; i++) {
        if (keys[i] < keys[i - 1]) unsorted++;
    }
    c->pw_unsorted[wid] += unsorted;
}

/* Detect sortedness of encoded keys.  Returns fraction of out-of-order pairs.
 * If the result is 0.0, data is already sorted and sort can be skipped.
 * If < threshold (e.g. 0.05), comparison sort is faster than radix. */
double detect_sortedness(ray_pool_t* pool, const uint64_t* keys, int64_t n) {
    if (n <= 1) return 0.0;

    int64_t total_unsorted;
    if (pool && n > SMALL_POOL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        int64_t pw[nw];
        memset(pw, 0, (size_t)nw * sizeof(int64_t));
        sortedness_ctx_t ctx = { .keys = keys, .pw_unsorted = pw };
        ray_pool_dispatch(pool, sortedness_fn, &ctx, n);

        total_unsorted = 0;
        for (uint32_t t = 0; t < nw; t++)
            total_unsorted += pw[t];

        /* Check cross-task boundaries (each task starts at a TASK_GRAIN
         * boundary; the sortedness_fn only checks within [start+1, end)
         * so boundaries between adjacent tasks are missed). */
        int64_t grain = RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
        for (int64_t b = grain; b < n; b += grain) {
            if (keys[b] < keys[b - 1])
                total_unsorted++;
        }
    } else {
        total_unsorted = 0;
        for (int64_t i = 1; i < n; i++) {
            if (keys[i] < keys[i - 1]) total_unsorted++;
        }
    }

    return (double)total_unsorted / (double)(n - 1);
}

/* Threshold: if fewer than 5% of pairs are out of order, data is
 * "nearly sorted" and adaptive comparison sort beats radix. */
/* NEARLY_SORTED_FRAC, radix_key_bytes defined in exec_internal.h */

/* Scan encoded keys to compute actual significant byte count from data range.
 * Eliminates histogram passes for bytes that are uniform across all keys. */
typedef struct {
    const uint64_t* keys;
    uint64_t*       pw_or;   /* per-worker XOR-diff accumulator */
} key_range_ctx_t;

static void key_range_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    key_range_ctx_t* c = (key_range_ctx_t*)arg;
    const uint64_t* keys = c->keys;
    uint64_t local_or = c->pw_or[wid];
    uint64_t first = keys[start];
    for (int64_t i = start; i < end; i++)
        local_or |= keys[i] ^ first;
    c->pw_or[wid] = local_or;
}

uint8_t compute_key_nbytes(ray_pool_t* pool, const uint64_t* keys,
                            int64_t n, uint8_t type_max) {
    if (n <= 1) return 1;
    uint64_t diff;
    if (pool && n > SMALL_POOL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        uint64_t pw_or[nw];
        memset(pw_or, 0, nw * sizeof(uint64_t));
        key_range_ctx_t ctx = { .keys = keys, .pw_or = pw_or };
        ray_pool_dispatch(pool, key_range_fn, &ctx, n);
        diff = 0;
        for (uint32_t w = 0; w < nw; w++) diff |= pw_or[w];
        /* Also XOR the first element from different worker ranges to
         * catch cross-worker differences (workers' "first" may differ) */
        uint64_t first = keys[0];
        int64_t chunk = (n + nw - 1) / nw;
        for (uint32_t w = 1; w < nw; w++) {
            int64_t wstart = (int64_t)w * chunk;
            if (wstart < n) diff |= keys[wstart] ^ first;
        }
    } else {
        diff = 0;
        uint64_t first = keys[0];
        for (int64_t i = 1; i < n; i++)
            diff |= keys[i] ^ first;
    }
    uint8_t nb = 0;
    while (diff) { nb++; diff >>= 8; }
    if (nb < 1) nb = 1;
    return nb < type_max ? nb : type_max;
}

/* --------------------------------------------------------------------------
 * Parallel LSB radix sort (8-bit digits, 256 buckets)
 *
 * Used for single-key sorts on I64/F64/I32/SYM/TIMESTAMP columns,
 * and composite-key sorts where all keys are integer types with total
 * bit width <= 64.
 *
 * Three phases per byte:
 *   1. Parallel histogram — each task counts byte occurrences in its range
 *   2. Sequential prefix-sum — compute per-task scatter offsets
 *   3. Parallel scatter — write elements to sorted positions
 *
 * Byte-skip: after histogram, if all elements share the same byte value,
 * skip that pass entirely.  Critical for small-range integers where most
 * upper bytes are identical.
 * -------------------------------------------------------------------------- */

/* radix_pass_ctx_t defined in exec_internal.h */

/* Pass 1: histogram — each task counts byte values in its fixed range */
static void radix_hist_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    radix_pass_ctx_t* c = (radix_pass_ctx_t*)arg;
    int64_t task = start; /* dispatch_n: [task, task+1) */

    /* Zero histogram slice BEFORE early return — empty tasks must still
     * clear their slice so the prefix-sum sees zeros, not garbage. */
    uint32_t* h = c->hist + task * 256;
    memset(h, 0, 256 * sizeof(uint32_t));

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;

    const uint64_t* keys = c->keys;
    uint8_t shift = c->shift;
    for (int64_t i = lo; i < hi; i++)
        h[(keys[i] >> shift) & 0xFF]++;
}

/* Pass 3: scatter with software write-combining (SWC).
 * Buffers entries per bucket before flushing, converting random writes
 * into sequential bursts that are friendlier to the cache hierarchy. */
#define SWC_N 8  /* entries per bucket buffer; 8*8=64B per bucket = 32KB total */
static void radix_scatter_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    radix_pass_ctx_t* c = (radix_pass_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;

    int64_t* off = c->offsets + task * 256;
    const uint64_t* k_in = c->keys;
    const int64_t*  i_in = c->idx;
    uint64_t* k_out = c->keys_out;
    int64_t*  i_out = c->idx_out;
    uint8_t shift = c->shift;

    /* SWC buffers: separate key/idx arrays to match output layout */
    uint64_t kbuf[256][SWC_N];
    int64_t  ibuf[256][SWC_N];
    uint8_t  bcnt[256];
    memset(bcnt, 0, 256);

    for (int64_t i = lo; i < hi; i++) {
        uint8_t byte = (k_in[i] >> shift) & 0xFF;
        uint8_t bp = bcnt[byte];
        kbuf[byte][bp] = k_in[i];
        ibuf[byte][bp] = i_in[i];
        if (++bp == SWC_N) {
            int64_t pos = off[byte];
            memcpy(&k_out[pos], kbuf[byte], SWC_N * sizeof(uint64_t));
            memcpy(&i_out[pos], ibuf[byte], SWC_N * sizeof(int64_t));
            off[byte] = pos + SWC_N;
            bp = 0;
        }
        bcnt[byte] = bp;
    }

    /* Flush remaining entries */
    for (int b = 0; b < 256; b++) {
        int64_t pos = off[b];
        for (uint8_t j = 0; j < bcnt[b]; j++) {
            k_out[pos + j] = kbuf[b][j];
            i_out[pos + j] = ibuf[b][j];
        }
        off[b] = pos + bcnt[b];
    }
}
#undef SWC_N

/* Run radix sort on pre-encoded uint64_t keys + int64_t indices.
 * n_bytes limits the number of byte passes (1..8) based on key width.
 * Returns pointer to the final sorted index array (either `indices` or
 * `idx_tmp`).  Caller must keep both alive until done reading indices
 * (the result may point into idx_tmp if an odd number of passes executed).
 * If sorted_keys_out is non-NULL, stores the pointer to the final sorted
 * keys buffer (either `keys` or `keys_tmp`).
 * Returns NULL on failure. */
int64_t* radix_sort_run(ray_pool_t* pool,
                                uint64_t* keys, int64_t* indices,
                                uint64_t* keys_tmp, int64_t* idx_tmp,
                                int64_t n, uint8_t n_bytes,
                                uint64_t** sorted_keys_out) {
    uint32_t n_tasks = pool ? ray_pool_total_workers(pool) : 1;
    if (n_tasks < 1) n_tasks = 1;

    ray_t *hist_hdr = NULL, *off_hdr = NULL;
    uint32_t* hist = (uint32_t*)scratch_alloc(&hist_hdr,
                        (size_t)n_tasks * 256 * sizeof(uint32_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                        (size_t)n_tasks * 256 * sizeof(int64_t));
    if (!hist || !offsets) {
        scratch_free(hist_hdr); scratch_free(off_hdr);
        return NULL;
    }

    uint64_t* src_k = keys,     *dst_k = keys_tmp;
    int64_t*  src_i = indices,   *dst_i = idx_tmp;

    for (uint8_t bp = 0; bp < n_bytes; bp++) {
        uint8_t shift = bp * 8;

        radix_pass_ctx_t ctx = {
            .keys = src_k, .idx = src_i,
            .keys_out = dst_k, .idx_out = dst_i,
            .n = n, .shift = shift, .n_tasks = n_tasks,
            .hist = hist, .offsets = offsets,
        };

        /* Pass 1: parallel histogram */
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, radix_hist_fn, &ctx, n_tasks);
        else
            radix_hist_fn(&ctx, 0, 0, 1);

        /* Check uniformity via global histogram */
        bool uniform = false;
        for (int b = 0; b < 256; b++) {
            uint32_t total = 0;
            for (uint32_t t = 0; t < n_tasks; t++)
                total += hist[t * 256 + b];
            if (total == (uint32_t)n) { uniform = true; break; }
        }
        if (uniform) continue; /* all same byte — skip this pass */

        /* Pass 2: prefix sum → per-task scatter offsets */
        int64_t running = 0;
        for (int b = 0; b < 256; b++) {
            for (uint32_t t = 0; t < n_tasks; t++) {
                offsets[t * 256 + b] = running;
                running += hist[t * 256 + b];
            }
        }

        /* Pass 3: parallel scatter */
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, radix_scatter_fn, &ctx, n_tasks);
        else
            radix_scatter_fn(&ctx, 0, 0, 1);

        /* Swap double-buffer pointers */
        uint64_t* tk = src_k; src_k = dst_k; dst_k = tk;
        int64_t*  ti = src_i; src_i = dst_i; dst_i = ti;
    }

    scratch_free(hist_hdr);
    scratch_free(off_hdr);
    if (sorted_keys_out) *sorted_keys_out = src_k;
    return src_i;  /* pointer to final sorted indices */
}

/* ============================================================================
 * Packed radix sort — key+index in a single uint64_t
 *
 * When key_nbytes * 8 + index_bits ≤ 64, we pack the encoded key and the
 * row index into one uint64_t:
 *   packed[i] = encoded_key[i] | ((uint64_t)i << idx_shift)
 *
 * Radix sort then moves ONE 8-byte value per element per pass instead of
 * TWO 8-byte values (key + index).  This halves all memory traffic:
 *   - SWC buffer: 16KB instead of 32KB (fits better in L1)
 *   - Scatter writes: 8B instead of 16B per element
 *   - Total traffic per pass: n×8B instead of n×16B
 *
 * After sorting, indices are extracted: idx = packed >> idx_shift
 * ============================================================================ */

/* Packed scatter: single-array SWC scatter, no separate index array. */
#define PSWC_N 8
static void packed_scatter_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    radix_pass_ctx_t* c = (radix_pass_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;

    int64_t* off = c->offsets + task * 256;
    const uint64_t* in  = c->keys;
    uint64_t*       out = c->keys_out;
    uint8_t shift = c->shift;

    /* Single SWC buffer: 256 × 8 × 8B = 16KB — fits in L1 */
    uint64_t buf[256][PSWC_N];
    uint8_t  bcnt[256];
    memset(bcnt, 0, 256);

    for (int64_t i = lo; i < hi; i++) {
        uint8_t byte = (in[i] >> shift) & 0xFF;
        uint8_t bp = bcnt[byte];
        buf[byte][bp] = in[i];
        if (++bp == PSWC_N) {
            int64_t pos = off[byte];
            memcpy(&out[pos], buf[byte], PSWC_N * sizeof(uint64_t));
            off[byte] = pos + PSWC_N;
            bp = 0;
        }
        bcnt[byte] = bp;
    }

    /* Flush remaining entries */
    for (int b = 0; b < 256; b++) {
        int64_t pos = off[b];
        for (uint8_t j = 0; j < bcnt[b]; j++)
            out[pos + j] = buf[b][j];
        off[b] = pos + bcnt[b];
    }
}
#undef PSWC_N

/* Packed radix sort: sorts an array of packed (key|index) uint64_t values.
 * Sorts by bytes lo_byte to hi_byte-1 (the key bytes).
 * Returns pointer to final sorted array (data or tmp). */
uint64_t* packed_radix_sort_run(ray_pool_t* pool,
                                         uint64_t* data, uint64_t* tmp,
                                         int64_t n, uint8_t n_bytes) {
    uint32_t n_tasks = pool ? ray_pool_total_workers(pool) : 1;
    if (n_tasks < 1) n_tasks = 1;

    ray_t *hist_hdr = NULL, *off_hdr = NULL;
    uint32_t* hist = (uint32_t*)scratch_alloc(&hist_hdr,
                        (size_t)n_tasks * 256 * sizeof(uint32_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                        (size_t)n_tasks * 256 * sizeof(int64_t));
    if (!hist || !offsets) {
        scratch_free(hist_hdr); scratch_free(off_hdr);
        return NULL;
    }

    uint64_t* src = data, *dst = tmp;

    for (uint8_t bp = 0; bp < n_bytes; bp++) {
        uint8_t shift = bp * 8;

        /* Reuse radix_pass_ctx_t — only .keys and .keys_out are used
         * by radix_hist_fn and packed_scatter_fn. */
        radix_pass_ctx_t ctx = {
            .keys = src, .keys_out = dst,
            .n = n, .shift = shift, .n_tasks = n_tasks,
            .hist = hist, .offsets = offsets,
        };

        /* Pass 1: parallel histogram (reuses existing radix_hist_fn) */
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, radix_hist_fn, &ctx, n_tasks);
        else
            radix_hist_fn(&ctx, 0, 0, 1);

        /* Check uniformity */
        bool uniform = false;
        for (int b = 0; b < 256; b++) {
            uint32_t total = 0;
            for (uint32_t t = 0; t < n_tasks; t++)
                total += hist[t * 256 + b];
            if (total == (uint32_t)n) { uniform = true; break; }
        }
        if (uniform) continue;

        /* Pass 2: prefix sum */
        int64_t running = 0;
        for (int b = 0; b < 256; b++) {
            for (uint32_t t = 0; t < n_tasks; t++) {
                offsets[t * 256 + b] = running;
                running += hist[t * 256 + b];
            }
        }

        /* Pass 3: packed scatter (half the traffic of dual-array scatter) */
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, packed_scatter_fn, &ctx, n_tasks);
        else
            packed_scatter_fn(&ctx, 0, 0, 1);

        uint64_t* t2 = src; src = dst; dst = t2;
    }

    scratch_free(hist_hdr);
    scratch_free(off_hdr);
    return src;
}

/* Fused pack + sortedness detection for packed radix sort.
 * Packs keys[i] |= (i << key_bits) in-place while counting:
 *   - forward inversions (keys[i] < keys[i-1]) → unsorted
 *   - reverse inversions (keys[i] > keys[i-1]) → not_reverse
 * If unsorted==0: already sorted. If not_reverse==0: reverse-sorted. */
typedef struct {
    uint64_t* keys;
    uint8_t   key_bits;
    uint64_t  key_mask;       /* mask for significant key bytes */
    int64_t*  pw_unsorted;    /* count of forward inversions */
    int64_t*  pw_not_reverse; /* count of strict ascending pairs */
} packed_detect_ctx_t;

static void packed_detect_fn(void* arg, uint32_t wid,
                              int64_t start, int64_t end) {
    packed_detect_ctx_t* c = (packed_detect_ctx_t*)arg;
    uint64_t* k = c->keys;
    uint8_t kb = c->key_bits;
    uint64_t km = c->key_mask;
    int64_t unsorted = 0, not_rev = 0;
    uint64_t prev = (start > 0) ? (k[start - 1] & km) : 0;
    for (int64_t i = start; i < end; i++) {
        uint64_t cur = k[i] & km;  /* mask to significant bytes */
        if (i > start) {
            if (cur < prev) unsorted++;
            if (cur > prev) not_rev++;
        }
        /* Pack: significant key bits | (index << key_bits) */
        k[i] = cur | ((uint64_t)i << kb);
        prev = cur;
    }
    c->pw_unsorted[wid] += unsorted;
    c->pw_not_reverse[wid] += not_rev;
}

/* Parallel unpack: extract indices (and optionally sorted keys) from
 * packed values after packed radix sort. */
typedef struct {
    const uint64_t* sorted;
    int64_t*        indices;
    uint64_t*       keys_out;
    uint8_t         key_bits;
    uint64_t        idx_mask;
    uint64_t        key_mask;
    bool            extract_keys;
} packed_unpack_ctx_t;

static void packed_unpack_fn(void* arg, uint32_t wid,
                              int64_t start, int64_t end) {
    (void)wid;
    packed_unpack_ctx_t* c = (packed_unpack_ctx_t*)arg;
    for (int64_t i = start; i < end; i++) {
        uint64_t v = c->sorted[i];
        c->indices[i] = (int64_t)((v >> c->key_bits) & c->idx_mask);
        if (c->extract_keys)
            c->keys_out[i] = v & c->key_mask;
    }
}

/* ============================================================================
 * MSD+LSB hybrid radix sort
 *
 * First pass: MSD partition by the most significant non-uniform byte.
 * Creates up to 256 buckets, each small enough to fit in L2 cache.
 * Subsequent passes: LSB radix sort within each bucket (in-cache, fast).
 *
 * For 10M I64 values with 3 significant bytes:
 *   LSB: 3 full passes over 160MB (keys+indices) = ~960MB random traffic
 *   MSD+LSB: 1 full pass + 256 × 2 in-cache passes ≈ ~400MB random + ~5ms in-cache
 *
 * Cache behavior: after the first MSD partition, each bucket (10M/256 ≈ 39K
 * elements ≈ 625KB) fits in L2.  Subsequent passes operate entirely within
 * cache, making them effectively free compared to the first pass.
 * ============================================================================ */

/* Per-bucket LSB radix sort (non-parallel, for cache-resident data).
 * No SWC needed since data fits in L2/L1 cache. */
static int64_t* bucket_lsb_sort(uint64_t* keys, int64_t* idx,
                                  uint64_t* ktmp, int64_t* itmp,
                                  int64_t n, uint8_t n_bytes) {
    if (n <= 64) {
        key_introsort(keys, idx, n);
        return idx;
    }

    uint64_t* src_k = keys, *dst_k = ktmp;
    int64_t*  src_i = idx,  *dst_i = itmp;

    for (uint8_t bp = 0; bp < n_bytes; bp++) {
        uint8_t shift = bp * 8;

        uint32_t hist[256];
        memset(hist, 0, sizeof(hist));
        for (int64_t i = 0; i < n; i++)
            hist[(src_k[i] >> shift) & 0xFF]++;

        /* Check uniformity — skip this byte if all values share the same digit */
        bool uniform = false;
        for (int b = 0; b < 256; b++) {
            if (hist[b] == (uint32_t)n) { uniform = true; break; }
        }
        if (uniform) continue;

        /* Prefix sum */
        int64_t off[256];
        off[0] = 0;
        for (int b = 1; b < 256; b++)
            off[b] = off[b-1] + (int64_t)hist[b-1];

        /* Scatter (no SWC — data is cache-resident) */
        for (int64_t i = 0; i < n; i++) {
            uint8_t byte = (src_k[i] >> shift) & 0xFF;
            int64_t pos = off[byte]++;
            dst_k[pos] = src_k[i];
            dst_i[pos] = src_i[i];
        }

        uint64_t* tk = src_k; src_k = dst_k; dst_k = tk;
        int64_t*  ti = src_i; src_i = dst_i; dst_i = ti;
    }

    return src_i;
}

/* Context for parallel per-bucket sorting after MSD partition */
typedef struct {
    uint64_t*  data_k;          /* MSD output: partitioned keys */
    int64_t*   data_i;          /* MSD output: partitioned indices */
    uint64_t*  tmp_k;           /* scratch (MSD input buffer, now free) */
    int64_t*   tmp_i;
    int64_t    bucket_offsets[257]; /* prefix-sum of bucket sizes */
    uint8_t    n_bytes;            /* remaining bytes to sort per bucket */
} msd_bucket_ctx_t;

static void msd_bucket_sort_fn(void* arg, uint32_t wid,
                                 int64_t start, int64_t end) {
    (void)wid;
    msd_bucket_ctx_t* c = (msd_bucket_ctx_t*)arg;

    for (int64_t b = start; b < end; b++) {
        int64_t off = c->bucket_offsets[b];
        int64_t cnt = c->bucket_offsets[b + 1] - off;
        if (cnt <= 1) continue;

        int64_t* sorted = bucket_lsb_sort(
            c->data_k + off, c->data_i + off,
            c->tmp_k  + off, c->tmp_i  + off,
            cnt, c->n_bytes);

        /* Ensure result is in the canonical buffer (data_k/data_i).
         * bucket_lsb_sort may leave result in the scratch buffer if an
         * odd number of scatter passes executed. */
        if (sorted != c->data_i + off) {
            memcpy(c->data_k + off, c->tmp_k + off,
                   (size_t)cnt * sizeof(uint64_t));
            memcpy(c->data_i + off, c->tmp_i + off,
                   (size_t)cnt * sizeof(int64_t));
        }
    }
}

/* MSD+LSB hybrid radix sort.
 * Returns pointer to final sorted indices (always idx_tmp).
 * If sorted_keys_out is non-NULL, stores sorted keys pointer (always keys_tmp).
 * Falls back to LSB radix sort for small arrays or single-byte keys. */
int64_t* msd_radix_sort_run(ray_pool_t* pool,
                                     uint64_t* keys, int64_t* indices,
                                     uint64_t* keys_tmp, int64_t* idx_tmp,
                                     int64_t n, uint8_t n_bytes,
                                     uint64_t** sorted_keys_out) {
    /* MSD is beneficial when:
     * (1) Many significant bytes (≥4) — saving 1 of 4+ LSB passes is worth it.
     * (2) Data is large enough that full passes dominate over MSD overhead.
     * (3) Average bucket fits in L2 cache (~256KB = 16K elements × 16B).
     * For ≤3 byte keys, LSB radix with range-adaptive byte skip is already fast
     * and MSD adds partitioning + dispatch overhead without enough payoff. */
    /* MSD adds partitioning + dispatch overhead that only pays off for
     * very wide keys (≥6 bytes) where saving multiple LSB passes matters.
     * For typical data (≤5 bytes after range analysis), LSB with SWC is faster. */
    if (n_bytes <= 5 || n <= 1000000) {
        return radix_sort_run(pool, keys, indices, keys_tmp, idx_tmp,
                               n, n_bytes, sorted_keys_out);
    }

    uint32_t n_tasks = pool ? ray_pool_total_workers(pool) : 1;
    if (n_tasks < 1) n_tasks = 1;

    /* Allocate histogram and offsets for MSD pass */
    ray_t *hist_hdr = NULL, *off_hdr = NULL;
    uint32_t* hist = (uint32_t*)scratch_alloc(&hist_hdr,
                        (size_t)n_tasks * 256 * sizeof(uint32_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                        (size_t)n_tasks * 256 * sizeof(int64_t));
    if (!hist || !offsets) {
        scratch_free(hist_hdr); scratch_free(off_hdr);
        return radix_sort_run(pool, keys, indices, keys_tmp, idx_tmp,
                               n, n_bytes, sorted_keys_out);
    }

    /* MSD pass: partition by the most significant non-uniform byte */
    uint8_t msd_byte = n_bytes - 1;
    uint8_t shift = msd_byte * 8;

    radix_pass_ctx_t ctx = {
        .keys = keys, .idx = indices,
        .keys_out = keys_tmp, .idx_out = idx_tmp,
        .n = n, .shift = shift, .n_tasks = n_tasks,
        .hist = hist, .offsets = offsets,
    };

    /* Pass 1: parallel histogram */
    if (pool && n_tasks > 1)
        ray_pool_dispatch_n(pool, radix_hist_fn, &ctx, n_tasks);
    else
        radix_hist_fn(&ctx, 0, 0, 1);

    /* Check uniformity */
    bool uniform = false;
    for (int b = 0; b < 256; b++) {
        uint32_t total = 0;
        for (uint32_t t = 0; t < n_tasks; t++)
            total += hist[t * 256 + b];
        if (total == (uint32_t)n) { uniform = true; break; }
    }

    if (uniform) {
        /* All keys share the same MSB — skip this byte, try next */
        scratch_free(hist_hdr); scratch_free(off_hdr);
        return msd_radix_sort_run(pool, keys, indices, keys_tmp, idx_tmp,
                                    n, n_bytes - 1, sorted_keys_out);
    }

    /* Pass 2: prefix sum → per-task scatter offsets + bucket boundaries */
    int64_t bucket_offsets[257];
    {
        int64_t running = 0;
        for (int b = 0; b < 256; b++) {
            bucket_offsets[b] = running;
            for (uint32_t t = 0; t < n_tasks; t++) {
                offsets[t * 256 + b] = running;
                running += hist[t * 256 + b];
            }
        }
        bucket_offsets[256] = running;
    }

    /* Pass 3: parallel scatter with SWC */
    if (pool && n_tasks > 1)
        ray_pool_dispatch_n(pool, radix_scatter_fn, &ctx, n_tasks);
    else
        radix_scatter_fn(&ctx, 0, 0, 1);

    scratch_free(hist_hdr);
    scratch_free(off_hdr);

    /* Data is now in keys_tmp/idx_tmp, partitioned by MSB.
     * Sort each bucket independently using the remaining bytes.
     * Use keys/indices as scratch (MSD input, now free to reuse). */
    uint8_t remaining_bytes = msd_byte; /* bytes 0..msd_byte-1 */

    msd_bucket_ctx_t bctx = {
        .data_k = keys_tmp, .data_i = idx_tmp,
        .tmp_k  = keys,     .tmp_i  = indices,
        .n_bytes = remaining_bytes,
    };
    memcpy(bctx.bucket_offsets, bucket_offsets, sizeof(bucket_offsets));

    if (pool)
        ray_pool_dispatch_n(pool, msd_bucket_sort_fn, &bctx, 256);
    else
        msd_bucket_sort_fn(&bctx, 0, 0, 256);

    /* Result is always in keys_tmp/idx_tmp */
    if (sorted_keys_out) *sorted_keys_out = keys_tmp;
    return idx_tmp;
}

/* radix_encode_ctx_t defined in exec_internal.h */

void radix_encode_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    radix_encode_ctx_t* c = (radix_encode_ctx_t*)arg;

    /* Fused iota: initialize index array alongside key encoding */
    if (c->indices) {
        int64_t* idx = c->indices;
        for (int64_t i = start; i < end; i++) idx[i] = i;
    }

    if (c->n_keys <= 1) {
        /* Single-key fast path */
        switch (c->type) {
        case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* d = (const int64_t*)c->data;
            bool has_nulls = c->col && (c->col->attrs & RAY_ATTR_HAS_NULLS);
            bool nf = c->nulls_first;
            bool desc = c->desc;
            /* Null key: nf=true→sort first, nf=false→sort last.
             * For ASC  NULLS FIRST → e=0            (smallest)
             * For ASC  NULLS LAST  → e=UINT64_MAX   (largest)
             * For DESC NULLS FIRST → e=UINT64_MAX   (~e=0, smallest after flip)
             * For DESC NULLS LAST  → e=0            (~e=UINT64_MAX, largest after flip) */
            uint64_t null_e = (nf ^ desc) ? 0 : UINT64_MAX;
            /* Hoist has_nulls × desc into 4 specialised tight loops. */
            if (desc && has_nulls) {
                for (int64_t i = start; i < end; i++) {
                    if (ray_vec_is_null(c->col, i)) c->keys[i] = ~null_e;
                    else c->keys[i] = ~((uint64_t)d[i] ^ ((uint64_t)1 << 63));
                }
            } else if (desc) {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = ~((uint64_t)d[i] ^ ((uint64_t)1 << 63));
            } else if (has_nulls) {
                for (int64_t i = start; i < end; i++) {
                    if (ray_vec_is_null(c->col, i)) c->keys[i] = null_e;
                    else c->keys[i] = (uint64_t)d[i] ^ ((uint64_t)1 << 63);
                }
            } else {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = (uint64_t)d[i] ^ ((uint64_t)1 << 63);
            }
            break;
        }
        case RAY_F64: {
            const double* d = (const double*)c->data;
            bool nf   = c->nulls_first;
            bool desc = c->desc;
            /* NaN override: encode NaN so it sorts first or last.
             * For ASC  NULLS FIRST → e=0            (smallest key)
             * For ASC  NULLS LAST  → e=UINT64_MAX   (largest key)
             * For DESC NULLS FIRST → e=UINT64_MAX   (~e=0, smallest)
             * For DESC NULLS LAST  → e=0            (~e=UINT64_MAX, largest)
             * Pattern: e = (nf ^ desc) ? 0 : UINT64_MAX */
            uint64_t nan_e = (nf ^ desc) ? 0 : UINT64_MAX;
            for (int64_t i = start; i < end; i++) {
                uint64_t bits;
                memcpy(&bits, &d[i], 8);
                /* NaN: exponent all-1s (0x7FF) and mantissa non-zero */
                if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
                    (bits & 0x000FFFFFFFFFFFFFULL)) {
                    c->keys[i] = desc ? ~nan_e : nan_e;
                } else {
                    uint64_t mask = -(bits >> 63) | ((uint64_t)1 << 63);
                    uint64_t e = bits ^ mask;
                    c->keys[i] = desc ? ~e : e;
                }
            }
            break;
        }
        case RAY_F32: {
            const float* d = (const float*)c->data;
            bool nf   = c->nulls_first;
            bool desc = c->desc;
            uint32_t nan_e = (nf ^ desc) ? 0U : UINT32_MAX;
            for (int64_t i = start; i < end; i++) {
                uint32_t bits;
                memcpy(&bits, &d[i], 4);
                uint32_t e;
                if ((bits & 0x7F800000U) == 0x7F800000U &&
                    (bits & 0x007FFFFFU)) {
                    e = nan_e;
                } else {
                    uint32_t mask = -(bits >> 31) | ((uint32_t)1 << 31);
                    e = bits ^ mask;
                }
                c->keys[i] = desc ? (uint64_t)(~e) : (uint64_t)e;
            }
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* d = (const int32_t*)c->data;
            bool has_nulls = c->col && (c->col->attrs & RAY_ATTR_HAS_NULLS);
            bool nf = c->nulls_first;
            bool desc = c->desc;
            uint64_t null_e = (nf ^ desc) ? 0 : UINT64_MAX;
            /* Hoist has_nulls × desc into 4 specialised tight loops. */
            if (desc && has_nulls) {
                for (int64_t i = start; i < end; i++) {
                    if (ray_vec_is_null(c->col, i)) c->keys[i] = ~null_e;
                    else c->keys[i] = ~((uint64_t)((uint32_t)d[i] ^ ((uint32_t)1 << 31)));
                }
            } else if (desc) {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = ~((uint64_t)((uint32_t)d[i] ^ ((uint32_t)1 << 31)));
            } else if (has_nulls) {
                for (int64_t i = start; i < end; i++) {
                    if (ray_vec_is_null(c->col, i)) c->keys[i] = null_e;
                    else c->keys[i] = (uint64_t)((uint32_t)d[i] ^ ((uint32_t)1 << 31));
                }
            } else {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = (uint64_t)((uint32_t)d[i] ^ ((uint32_t)1 << 31));
            }
            break;
        }
        case RAY_SYM: {
            const uint32_t* rank = c->enum_rank;
            if (c->desc) {
                for (int64_t i = start; i < end; i++) {
                    uint32_t raw = (uint32_t)ray_read_sym(c->data, i, c->type, c->col_attrs);
                    c->keys[i] = ~(uint64_t)rank[raw];
                }
            } else {
                for (int64_t i = start; i < end; i++) {
                    uint32_t raw = (uint32_t)ray_read_sym(c->data, i, c->type, c->col_attrs);
                    c->keys[i] = (uint64_t)rank[raw];
                }
            }
            break;
        }
        case RAY_I16: {
            const int16_t* d = (const int16_t*)c->data;
            bool has_nulls = c->col && (c->col->attrs & RAY_ATTR_HAS_NULLS);
            bool nf = c->nulls_first;
            bool desc = c->desc;
            if (has_nulls) {
                /* Shift non-null encoded values up by 1 so the null
                 * sentinel sits outside the [1, 0x10000] data range and
                 * cannot tie with INT16_MIN or INT16_MAX.  Costs one
                 * extra radix-byte pass; sort_indices_ex bumps
                 * key_nbytes_max accordingly. */
                uint64_t null_e = (nf ^ desc) ? 0 : 0x10001ULL;
                if (desc) {
                    for (int64_t i = start; i < end; i++) {
                        if (ray_vec_is_null(c->col, i)) c->keys[i] = ~null_e;
                        else c->keys[i] = ~((uint64_t)((uint16_t)d[i] ^ ((uint16_t)1 << 15)) + 1);
                    }
                } else {
                    for (int64_t i = start; i < end; i++) {
                        if (ray_vec_is_null(c->col, i)) c->keys[i] = null_e;
                        else c->keys[i] = (uint64_t)((uint16_t)d[i] ^ ((uint16_t)1 << 15)) + 1;
                    }
                }
            } else if (desc) {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = ~((uint64_t)((uint16_t)d[i] ^ ((uint16_t)1 << 15)));
            } else {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = (uint64_t)((uint16_t)d[i] ^ ((uint16_t)1 << 15));
            }
            break;
        }
        case RAY_BOOL: case RAY_U8: {
            const uint8_t* d = (const uint8_t*)c->data;
            bool has_nulls = c->col && (c->col->attrs & RAY_ATTR_HAS_NULLS);
            bool nf = c->nulls_first;
            bool desc = c->desc;
            if (has_nulls) {
                /* Shift non-null encoded values up by 1; null sentinel
                 * lives at 0 (NF) or 0x101 (NL), beyond every U8/BOOL
                 * data value. */
                uint64_t null_e = (nf ^ desc) ? 0 : 0x101ULL;
                if (desc) {
                    for (int64_t i = start; i < end; i++) {
                        if (ray_vec_is_null(c->col, i)) c->keys[i] = ~null_e;
                        else c->keys[i] = ~((uint64_t)d[i] + 1);
                    }
                } else {
                    for (int64_t i = start; i < end; i++) {
                        if (ray_vec_is_null(c->col, i)) c->keys[i] = null_e;
                        else c->keys[i] = (uint64_t)d[i] + 1;
                    }
                }
            } else if (desc) {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = ~(uint64_t)d[i];
            } else {
                for (int64_t i = start; i < end; i++)
                    c->keys[i] = (uint64_t)d[i];
            }
            break;
        }
        }
    } else {
        /* Composite-key encoding */
        for (int64_t i = start; i < end; i++) {
            uint64_t composite = 0;
            for (uint32_t k = 0; k < c->n_keys; k++) {
                ray_t* col = c->vecs[k];
                int64_t val;
                if (c->enum_ranks[k]) {
                    uint32_t raw = (uint32_t)ray_read_sym(ray_data(col), i, col->type, col->attrs);
                    val = (int64_t)c->enum_ranks[k][raw];
                } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
                    val = ((const int64_t*)ray_data(col))[i];
                } else if (col->type == RAY_F64) {
                    uint64_t bits;
                    memcpy(&bits, &((const double*)ray_data(col))[i], 8);
                    uint64_t mask = -(bits >> 63) | ((uint64_t)1 << 63);
                    val = (int64_t)(bits ^ mask);
                } else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME) {
                    val = (int64_t)((const int32_t*)ray_data(col))[i];
                } else if (col->type == RAY_I16) {
                    val = (int64_t)((const int16_t*)ray_data(col))[i];
                } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
                    val = (int64_t)((const uint8_t*)ray_data(col))[i];
                } else {
                    val = 0;
                }
                uint64_t part = (uint64_t)val - (uint64_t)c->mins[k];
                if (c->descs[k]) part = (uint64_t)c->ranges[k] - part;
                composite |= part << c->bit_shifts[k];
            }
            c->keys[i] = composite;
        }
    }
}

/* ============================================================================
 * Adaptive string sort (single-key RAY_STR)
 *
 * Pipeline:
 *   1. Null partition — move nulls to sorted_idx[n_live..nrows).
 *   2. Probe — one linear pass over the non-null range computes
 *        • max_len                     (→ key width)
 *        • run_count / run_all_asc/desc (→ pre-sorted short-circuit)
 *        • card_estimate on the first 1024 rows via an exact hashset
 *                                      (future-facing; unused today)
 *      Every downstream decision is taken from these runtime numbers —
 *      nothing in this file branches on "we know the bench is str8".
 *   3. Single-run short-circuit — if the probe reports one monotone
 *      run across the entire non-null range, we're done: copy (or
 *      reverse, for DESC × ASC mismatch) and skip sorting entirely.
 *      This is the vergesort trivial case; the general multi-run
 *      merge path is scoped for a follow-up.
 *   4. Key materialization — pack each non-null string into a record
 *        struct { uint64_t parts[parts]; uint32_t row; uint32_t len; }
 *      where parts = min(4, ceil(max_len/8)) and each part holds 8
 *      bytes of the string byte-swapped into big-endian u64 form, so
 *      raw u64 comparison == lex comparison.  One sequential pass
 *      over the input, zero per-byte function calls downstream.
 *   5. American-Flag in-place MSD byte radix on the packed records.
 *      Top-level byte histogram → 256 buckets → one in-place swap
 *      pass → recurse.  Sub-base-case buckets (≤ 24) finish with
 *      insertion sort using the full multi-u64 comparator.  When
 *      recursion exhausts the packed prefix (depth == parts*8),
 *      ties fall through to a tail comparator that walks the
 *      original bytes via ray_str_t_cmp — the only place cold
 *      pool memory is touched during the sort proper.
 *   6. Scatter row indices back to sorted_idx.
 *   7. DESC reverses the non-null range; nulls-first rotates nulls
 *      to the front.
 *
 * Every threshold and resource allocation here is driven by runtime
 * numbers (n, max_len, worker count) or machine geometry (cache line,
 * pool workers) — never by assumptions about input shape.
 * ============================================================================ */

#define RAY_STRSORT_KEY_PARTS_MAX 4    /* 32-byte packed prefix cap */
#define RAY_STRSORT_BASE_CASE     24   /* small-bucket insertion-sort threshold */
#define RAY_STRSORT_PROBE_HEAD    1024 /* rows sampled for exact distinct count */

typedef struct {
    uint64_t parts[RAY_STRSORT_KEY_PARTS_MAX];
    uint32_t row;
    uint32_t len;
} ray_strkey_t;

/* Convert a native-endian u64 to big-endian so raw u64 comparison yields
 * lex order over the original byte layout.  On LE targets (everything we
 * build for today) this is a single bswap instruction. */
static inline uint64_t strkey_lex_u64(uint64_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

/* Load 8 bytes starting at src[offset], zero-padding past `len`, then
 * byte-swap into lex u64 form.  Returns 0 when offset ≥ len. */
static inline uint64_t strkey_load_part(const char* src, int64_t len, int offset) {
    int64_t remaining = len - offset;
    if (remaining <= 0) return 0;
    uint64_t raw = 0;
    int64_t take = remaining < 8 ? remaining : 8;
    memcpy(&raw, src + offset, (size_t)take);
    return strkey_lex_u64(raw);
}

/* Full-depth comparator.  Fast path: the packed parts.  Tail fallback:
 * only fires if both records have len > parts*8 and their packed
 * prefixes are equal — touches pool memory via ray_str_t_cmp only
 * at the base case, never during the radix partitioning loop. */
static int strkey_cmp(const ray_strkey_t* a, const ray_strkey_t* b,
                      int parts,
                      const ray_str_t* elems, const char* pool) {
    for (int p = 0; p < parts; p++) {
        if (a->parts[p] < b->parts[p]) return -1;
        if (a->parts[p] > b->parts[p]) return  1;
    }
    int64_t parts_bytes = (int64_t)parts * 8;
    /* Both strings fit inside the packed prefix — the only way their
     * parts can tie is if one is a zero-padded suffix of the other, in
     * which case the shorter one sorts first.  (Equal length means they
     * are actually equal and stability via row is handled by the caller.) */
    if ((int64_t)a->len <= parts_bytes && (int64_t)b->len <= parts_bytes) {
        return (int)a->len - (int)b->len;
    }
    /* Tail comparison on bytes [parts_bytes, len). */
    const ray_str_t* sa = &elems[a->row];
    const ray_str_t* sb = &elems[b->row];
    const char* pa = ray_str_t_ptr(sa, pool);
    const char* pb = ray_str_t_ptr(sb, pool);
    int64_t la = (int64_t)sa->len - parts_bytes; if (la < 0) la = 0;
    int64_t lb = (int64_t)sb->len - parts_bytes; if (lb < 0) lb = 0;
    int64_t m = la < lb ? la : lb;
    int r = m ? memcmp(pa + parts_bytes, pb + parts_bytes, (size_t)m) : 0;
    if (r != 0) return r;
    return (la > lb) - (la < lb);
}

static void strkey_insertion_sort(ray_strkey_t* a, int64_t n, int parts,
                                   const ray_str_t* elems, const char* pool) {
    for (int64_t i = 1; i < n; i++) {
        ray_strkey_t cur = a[i];
        int64_t j = i - 1;
        while (j >= 0 && strkey_cmp(&a[j], &cur, parts, elems, pool) > 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = cur;
    }
}

/* Extract the bp'th big-endian byte of the packed prefix. */
static inline uint8_t strkey_byte_at(const ray_strkey_t* k, int bp) {
    int part = bp >> 3;
    int shift = 56 - ((bp & 7) << 3);
    return (uint8_t)(k->parts[part] >> shift);
}

/* Cheap max-len probe — one sequential pass over the `len` field of each
 * live row's ray_str_t.  Reads only 4 bytes per row (the len), so at 10M
 * rows this is ~5ms bandwidth-bound.  Everything else the old probe
 * computed (monotonicity, distinct-count sample) is folded into the
 * parallel key-build pass below, where it's nearly free. */
static int strsort_probe_parts(const int64_t* indices, int64_t n_live,
                                const ray_str_t* elems) {
    int64_t max_len = 0;
    for (int64_t i = 0; i < n_live; i++) {
        int64_t l = (int64_t)elems[indices[i]].len;
        if (l > max_len) max_len = l;
    }
    int64_t pcalc = (max_len + 7) / 8;
    if (pcalc < 1) pcalc = 1;
    if (pcalc > RAY_STRSORT_KEY_PARTS_MAX) pcalc = RAY_STRSORT_KEY_PARTS_MAX;
    return (int)pcalc;
}

/* Parallel key materialization (morsel range). */
typedef struct {
    ray_strkey_t*    out;
    const int64_t*   indices;
    const ray_str_t* elems;
    const char*      pool;
    int              parts;
} strsort_build_ctx_t;

static void strsort_build_fn(void* vctx, uint32_t wid, int64_t s, int64_t e) {
    (void)wid;
    strsort_build_ctx_t* c = (strsort_build_ctx_t*)vctx;
    for (int64_t i = s; i < e; i++) {
        int64_t row = c->indices[i];
        const ray_str_t* str = &c->elems[row];
        c->out[i].row = (uint32_t)row;
        c->out[i].len = str->len;
        int64_t len = str->len;
        const char* src = len ? ray_str_t_ptr(str, c->pool) : NULL;
        for (int p = 0; p < RAY_STRSORT_KEY_PARTS_MAX; p++) {
            c->out[i].parts[p] = (p < c->parts)
                ? strkey_load_part(src, len, p * 8)
                : 0;
        }
    }
}

static void strsort_build_keys(ray_strkey_t* out, int64_t n_live,
                                const int64_t* indices,
                                const ray_str_t* elems, const char* pool,
                                int parts) {
    strsort_build_ctx_t c = { out, indices, elems, pool, parts };
    ray_pool_t* p = ray_pool_get();
    if (p && n_live >= RAY_PARALLEL_THRESHOLD) {
        ray_pool_dispatch(p, strsort_build_fn, &c, n_live);
    } else {
        strsort_build_fn(&c, 0, 0, n_live);
    }
}

/* Emit sorted row indices back to sorted_idx (parallel). */
typedef struct {
    int64_t*             out;
    const ray_strkey_t*  keys;
} strsort_emit_ctx_t;

static void strsort_emit_fn(void* vctx, uint32_t wid, int64_t s, int64_t e) {
    (void)wid;
    strsort_emit_ctx_t* c = (strsort_emit_ctx_t*)vctx;
    for (int64_t i = s; i < e; i++) c->out[i] = (int64_t)c->keys[i].row;
}

/* Packed-key lexicographic compare.  Fast path for run-detection and
 * insertion sort at the radix base case.  No pool access. */
static inline int strkey_cmp_packed(const ray_strkey_t* a,
                                    const ray_strkey_t* b, int parts) {
    for (int p = 0; p < parts; p++) {
        if (a->parts[p] < b->parts[p]) return -1;
        if (a->parts[p] > b->parts[p]) return  1;
    }
    return (int)a->len - (int)b->len;
}

/* Sequential run detection over packed keys, with early abort.
 * For random data the first inversion appears within a few elements
 * and the scan exits in O(1).  For fully sorted data it does one
 * linear pass over the packed key array (contiguous memory, ~10ms
 * sequential at 10M × 40B records — bandwidth bound).
 * Returns the detected direction: -1 = all descending, +1 = all
 * ascending, 0 = neither (or tail bytes remain to be sorted).
 *
 * IMPORTANT: when two adjacent packed keys tie AND either string is
 * longer than the packed window, we CANNOT declare a sorted run —
 * the tail bytes may impose ordering we haven't examined.  The
 * shortcut is safe only when every pair is either strictly ordered
 * by the packed key or both sides fit entirely inside the window. */
static int strsort_detect_runs(const ray_strkey_t* keys, int64_t n,
                                int parts, int parts_bytes) {
    if (n < 2) return 0;
    bool asc = true, desc = true;
    for (int64_t i = 1; i < n; i++) {
        int r = strkey_cmp_packed(&keys[i - 1], &keys[i], parts);
        if (r == 0) {
            if ((int64_t)keys[i - 1].len > parts_bytes ||
                (int64_t)keys[i].len     > parts_bytes) {
                /* Tail bytes unresolved — fall through to the real sort. */
                return 0;
            }
            /* Both fully fit in the packed prefix and their parts tie
             * → the strings are equal in the sorted order, which is
             * compatible with both ascending and descending runs. */
        } else if (r > 0) {
            asc = false;
        } else {
            desc = false;
        }
        if (!asc && !desc) return 0;
    }
    if (asc) return 1;
    if (desc) return -1;
    return 0;
}

/* Parallel top-level byte-0 partition: per-task histogram, global
 * prefix-sum, parallel scatter into a second contiguous buffer.
 * This is the same pattern as the numeric radix_sort_run up above,
 * adapted for 40-byte packed string keys. */
typedef struct {
    const ray_strkey_t* src;
    ray_strkey_t*       dst;
    int64_t             n;
    uint32_t            n_tasks;
    uint32_t*           hist;     /* [n_tasks × 256] */
    int64_t*            offsets;  /* [n_tasks × 256] */
} strsort_top_ctx_t;

static void strsort_top_hist_fn(void* vctx, uint32_t wid,
                                 int64_t start, int64_t end) {
    (void)wid; (void)end;
    strsort_top_ctx_t* c = (strsort_top_ctx_t*)vctx;
    int64_t task = start;
    uint32_t* h = c->hist + task * 256;
    memset(h, 0, 256 * sizeof(uint32_t));
    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;
    const ray_strkey_t* src = c->src;
    for (int64_t i = lo; i < hi; i++) {
        h[strkey_byte_at(&src[i], 0)]++;
    }
}

static void strsort_top_scatter_fn(void* vctx, uint32_t wid,
                                    int64_t start, int64_t end) {
    (void)wid; (void)end;
    strsort_top_ctx_t* c = (strsort_top_ctx_t*)vctx;
    int64_t task = start;
    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;
    int64_t* off = c->offsets + task * 256;
    const ray_strkey_t* src = c->src;
    ray_strkey_t* dst = c->dst;
    for (int64_t i = lo; i < hi; i++) {
        uint8_t b = strkey_byte_at(&src[i], 0);
        dst[off[b]++] = src[i];
    }
}

/* Bucket dispatch context: each task sorts one top-level bucket. */
typedef struct {
    ray_strkey_t*    keys;
    const int64_t*   starts;
    const int64_t*   counts;
    int              parts_bytes;
    int64_t          base_offset;
    const ray_str_t* elems;
    const char*      pool;
    int              parts;
    int              start_bp;  /* byte position to begin radix within bucket */
} strsort_bucket_ctx_t;

static void strsort_aflag(ray_strkey_t* keys, int64_t n, int bp,
                          int parts_bytes, int64_t base_offset,
                          const ray_str_t* elems, const char* pool,
                          int parts);

static void strsort_bucket_fn(void* vctx, uint32_t wid, int64_t s, int64_t e) {
    (void)wid;
    strsort_bucket_ctx_t* c = (strsort_bucket_ctx_t*)vctx;
    for (int64_t b = s; b < e; b++) {
        int64_t cnt = c->counts[b];
        if (cnt <= 1) continue;
        strsort_aflag(c->keys + c->starts[b], cnt, c->start_bp,
                      c->parts_bytes, c->base_offset,
                      c->elems, c->pool, c->parts);
    }
}

/* In-place quicksort by packed key `len` field.  Used as the
 * finalization step for buckets where every record's string ended
 * at or before the current base_offset — such records tied on the
 * packed prefix but still need to be ordered by length (shorter
 * strings sort before longer ones that extend them, per
 * ray_str_t_cmp).  Single-key integer quicksort with median-of-3
 * pivot; stack depth bounded via tail-recursion on the larger half.
 * Falls back to insertion sort for small ranges. */
static void strkey_qsort_by_len(ray_strkey_t* a, int64_t lo, int64_t hi) {
    while (hi - lo > 16) {
        int64_t mid = lo + (hi - lo) / 2;
        /* Median-of-3. */
        if (a[lo].len  > a[hi].len)  { ray_strkey_t t=a[lo];  a[lo]=a[hi];  a[hi]=t;  }
        if (a[mid].len > a[hi].len)  { ray_strkey_t t=a[mid]; a[mid]=a[hi]; a[hi]=t;  }
        if (a[lo].len  > a[mid].len) { ray_strkey_t t=a[lo];  a[lo]=a[mid]; a[mid]=t; }
        uint32_t pivot = a[mid].len;
        /* Hoare partition. */
        int64_t i = lo - 1, j = hi + 1;
        for (;;) {
            do { i++; } while (a[i].len < pivot);
            do { j--; } while (a[j].len > pivot);
            if (i >= j) break;
            ray_strkey_t t = a[i]; a[i] = a[j]; a[j] = t;
        }
        /* Recurse on smaller half, loop on the larger. */
        if (j - lo < hi - (j + 1)) {
            strkey_qsort_by_len(a, lo, j);
            lo = j + 1;
        } else {
            strkey_qsort_by_len(a, j + 1, hi);
            hi = j;
        }
    }
    /* Insertion sort base case. */
    for (int64_t i = lo + 1; i <= hi; i++) {
        ray_strkey_t cur = a[i];
        int64_t j = i - 1;
        while (j >= lo && a[j].len > cur.len) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = cur;
    }
}

/* Re-pack the next window of bytes for records whose previous window
 * tied on the full packed prefix.  `base_offset` is the byte position
 * in the original string that will become byte 0 of the new packed
 * prefix.  Returns true if any record still has bytes to contribute
 * past base_offset — false means every record's string ended at or
 * before base_offset.
 *
 * When this returns false the caller MUST NOT simply move on: strings
 * that ended before base_offset may still have differing lengths, and
 * ray_str_t_cmp sorts shorter-before-longer on tie.  We handle that
 * right here by sorting the bucket in place on `len` before returning,
 * so the caller can just stop recursing. */
static bool strsort_repack_window(ray_strkey_t* keys, int64_t n,
                                   int64_t base_offset,
                                   const ray_str_t* elems, const char* pool,
                                   int parts) {
    bool any_tail = false;
    /* Track min/max len alongside the repack so we can skip the
     * finalize-by-len step when every string in the bucket has the
     * same length — the very common case where the bucket is full
     * of identical strings (e.g. few_unique radix sub-bucket). */
    uint32_t min_len = UINT32_MAX;
    uint32_t max_len = 0;
    for (int64_t i = 0; i < n; i++) {
        const ray_str_t* s = &elems[keys[i].row];
        int64_t len = s->len;
        if (len > base_offset) any_tail = true;
        if ((uint32_t)len < min_len) min_len = (uint32_t)len;
        if ((uint32_t)len > max_len) max_len = (uint32_t)len;
        const char* src = len > 0 ? ray_str_t_ptr(s, pool) : NULL;
        for (int p = 0; p < parts; p++) {
            int64_t off = base_offset + (int64_t)p * 8;
            keys[i].parts[p] = (src && len > off)
                ? strkey_load_part(src, len, (int)off)
                : 0;
        }
    }
    if (!any_tail && n > 1 && min_len != max_len) {
        /* Every string ended at or before base_offset, they tied on
         * the zero-padded packed prefix, and at least two of them
         * differ in length.  A string of length 3 whose bytes match
         * a prefix of a length-5 string must sort before it (per
         * ray_str_t_cmp), so finalize the bucket by sorting on len.
         * When min_len == max_len every record is bitwise equal and
         * any order is valid — we skip the sort entirely. */
        strkey_qsort_by_len(keys, 0, n - 1);
    }
    return any_tail;
}

/* American Flag in-place MSD byte radix on keys[0..n) at byte position bp
 * within the current window.  All records share the same prefix from
 * byte 0 up to `base_offset + bp` of the original string.  When the
 * current window is exhausted (`bp >= parts_bytes`) we re-pack the next
 * window and continue — keeps worst case at O(total_bytes) even when
 * records share arbitrarily long common prefixes.
 *
 * parts_bytes = parts * 8 (cached).  base_offset tracks how many bytes
 * of the original string have already been consumed by earlier windows. */
static void strsort_aflag(ray_strkey_t* keys, int64_t n, int bp,
                          int parts_bytes, int64_t base_offset,
                          const ray_str_t* elems, const char* pool,
                          int parts) {
    /* Tail-recursive inline loop on the largest bucket to bound stack
     * depth independent of n. */
    for (;;) {
        if (n <= 1) return;
        if (n <= RAY_STRSORT_BASE_CASE) {
            /* Small bucket — finish with a bounded comparison sort.
             * strkey_cmp walks the original string bytes past the
             * current window when necessary, so long tails are fine
             * at this size. */
            strkey_insertion_sort(keys, n, parts, elems, pool);
            return;
        }
        if (bp >= parts_bytes) {
            /* Exhausted the packed prefix for this window with a big
             * bucket still to resolve.  Re-pack the next window and
             * restart the radix — keeps total work linear in string
             * bytes, never quadratic. */
            int64_t next_offset = base_offset + parts_bytes;
            if (!strsort_repack_window(keys, n, next_offset,
                                        elems, pool, parts)) {
                /* Every record's string ends at or before next_offset;
                 * they are all equal from here on, order preserved. */
                return;
            }
            base_offset = next_offset;
            bp = 0;
            continue;
        }

        int64_t counts[256] = {0};
        for (int64_t i = 0; i < n; i++) {
            counts[strkey_byte_at(&keys[i], bp)]++;
        }
        /* Fast path: all records share the same byte at this position.
         * Skip the partition pass and advance one byte deeper. */
        int uniq_b = -1;
        bool uniform = true;
        for (int b = 0; b < 256; b++) {
            if (counts[b] == 0) continue;
            if (uniq_b < 0) uniq_b = b;
            else { uniform = false; break; }
        }
        if (uniform) {
            bp++;
            continue;
        }

        int64_t starts[256];
        int64_t ends[256];
        {
            int64_t sum = 0;
            for (int b = 0; b < 256; b++) {
                starts[b] = sum;
                sum += counts[b];
                ends[b] = sum;
            }
        }

        /* In-place swap loop: classic American Flag.  For each bucket b,
         * drain records out of its slice whose current byte != b into
         * their correct destination, cycling until the bucket slice
         * contains only records that belong in b. */
        int64_t cursors[256];
        memcpy(cursors, starts, sizeof(cursors));
        for (int b = 0; b < 256; b++) {
            while (cursors[b] < ends[b]) {
                ray_strkey_t v = keys[cursors[b]];
                int bb = strkey_byte_at(&v, bp);
                while (bb != b) {
                    ray_strkey_t tmp = keys[cursors[bb]];
                    keys[cursors[bb]] = v;
                    cursors[bb]++;
                    v = tmp;
                    bb = strkey_byte_at(&v, bp);
                }
                keys[cursors[b]] = v;
                cursors[b]++;
            }
        }

        /* Find the largest bucket; recurse on the rest and loop on the
         * largest to keep stack shallow. */
        int big_b = 0;
        int64_t big_cnt = counts[0];
        for (int b = 1; b < 256; b++) {
            if (counts[b] > big_cnt) { big_cnt = counts[b]; big_b = b; }
        }
        for (int b = 0; b < 256; b++) {
            if (b == big_b) continue;
            int64_t cnt = counts[b];
            if (cnt > 1) {
                strsort_aflag(keys + starts[b], cnt, bp + 1,
                              parts_bytes, base_offset, elems, pool, parts);
            }
        }
        keys += starts[big_b];
        n = big_cnt;
        bp++;
    }
}

/* Top-level adaptive string sort.  Nulls partitioned first, then the
 * non-null range runs through probe → single-run short-circuit →
 * key materialization → American-Flag MSD → scatter row indices back.
 * Returns false on OOM (caller should fall back to comparison sort). */
static bool sort_str_msd_inplace(int64_t* sorted_idx, int64_t nrows,
                                 ray_t* col, bool desc, bool nulls_first) {
    if (nrows <= 0) return true;

    /* Initial iota — caller may or may not have already filled it. */
    for (int64_t i = 0; i < nrows; i++) sorted_idx[i] = i;

    /* Partition nulls to the tail.  Slice vecs inherit the null bitmap
     * from slice_parent, so check both attr slots — matches the
     * exec_sort post-sort propagation pattern. */
    int64_t null_count = 0;
    bool has_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) ||
                     ((col->attrs & RAY_ATTR_SLICE) && col->slice_parent &&
                      (col->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
    if (has_nulls) {
        int64_t w = 0;
        int64_t null_pos;
        for (int64_t i = 0; i < nrows; i++) {
            if (!ray_vec_is_null(col, i)) sorted_idx[w++] = i;
        }
        null_count = nrows - w;
        null_pos = w;
        for (int64_t i = 0; i < nrows; i++) {
            if (ray_vec_is_null(col, i)) sorted_idx[null_pos++] = i;
        }
    }
    int64_t n_live = nrows - null_count;

    if (n_live > 1) {
        const ray_str_t* elems;
        const char* pool;
        str_resolve(col, &elems, &pool);
        ray_pool_t* pool_p = ray_pool_get();
        bool go_parallel = (pool_p && n_live >= RAY_PARALLEL_THRESHOLD);

        /* --- Cheap max-len probe (one pass over len fields). ---
         * Chooses how many 8-byte parts to pack per key.  Everything
         * else (monotonicity, cardinality sampling) is folded into the
         * key-build / run-detection passes below. */
        int parts = strsort_probe_parts(sorted_idx, n_live, elems);
        int parts_bytes = parts * 8;

        /* --- Parallel key materialization. --- */
        ray_t* keys_hdr = NULL;
        ray_strkey_t* keys = (ray_strkey_t*)scratch_alloc(&keys_hdr,
                                (size_t)n_live * sizeof(ray_strkey_t));
        if (!keys) return false;
        strsort_build_keys(keys, n_live, sorted_idx, elems, pool, parts);

        /* --- Vergesort run detection on packed keys. ---
         * Early-aborts on the first inversion (so random input pays O(1)).
         * When the entire non-null range is a single monotone run we
         * skip the sort proper and emit row indices directly. */
        int run_dir = strsort_detect_runs(keys, n_live, parts, parts_bytes);
        bool want_asc = !desc;
        if (run_dir == 1 && want_asc) {
            /* Already ascending — emit as-is. */
            strsort_emit_ctx_t ectx = { sorted_idx, keys };
            if (go_parallel)
                ray_pool_dispatch(pool_p, strsort_emit_fn, &ectx, n_live);
            else
                strsort_emit_fn(&ectx, 0, 0, n_live);
        } else if (run_dir == -1 && !want_asc) {
            /* Already descending — emit as-is. */
            strsort_emit_ctx_t ectx = { sorted_idx, keys };
            if (go_parallel)
                ray_pool_dispatch(pool_p, strsort_emit_fn, &ectx, n_live);
            else
                strsort_emit_fn(&ectx, 0, 0, n_live);
        } else if (run_dir != 0) {
            /* Single run but wrong direction — emit row-indices reversed. */
            for (int64_t i = 0, j = n_live - 1; i < j; i++, j--) {
                ray_strkey_t t = keys[i]; keys[i] = keys[j]; keys[j] = t;
            }
            strsort_emit_ctx_t ectx = { sorted_idx, keys };
            if (go_parallel)
                ray_pool_dispatch(pool_p, strsort_emit_fn, &ectx, n_live);
            else
                strsort_emit_fn(&ectx, 0, 0, n_live);
        } else {
            /* --- Top-level byte-0 partition. ---
             * When parallel: per-task histograms, prefix-sum, parallel
             * scatter into a second contiguous buffer, pointer-swap
             * so `keys` holds the partitioned records.  When sequential:
             * single-pass American-Flag in-place swap loop. */
            ray_t* tmp_hdr = NULL;
            ray_strkey_t* keys_sorted = keys;  /* where the final data lands */

            if (!go_parallel || parts_bytes == 0) {
                strsort_aflag(keys, n_live, /*bp=*/0, parts_bytes,
                              /*base_offset=*/0, elems, pool, parts);
            } else {
                ray_strkey_t* tmp = (ray_strkey_t*)scratch_alloc(&tmp_hdr,
                                        (size_t)n_live * sizeof(ray_strkey_t));
                if (!tmp) {
                    /* Fall back to sequential sort on OOM. */
                    strsort_aflag(keys, n_live, /*bp=*/0, parts_bytes,
                                  /*base_offset=*/0, elems, pool, parts);
                } else {
                    uint32_t n_tasks = ray_pool_total_workers(pool_p);
                    if (n_tasks < 1) n_tasks = 1;

                    ray_t* hist_hdr = NULL;
                    ray_t* off_hdr  = NULL;
                    uint32_t* hist = (uint32_t*)scratch_alloc(&hist_hdr,
                                        (size_t)n_tasks * 256 * sizeof(uint32_t));
                    int64_t*  off  = (int64_t*)scratch_alloc(&off_hdr,
                                        (size_t)n_tasks * 256 * sizeof(int64_t));
                    if (!hist || !off) {
                        /* Free only the hist/off scratch we own here; tmp_hdr
                         * belongs to the outer cleanup block (line below) and
                         * MUST NOT be freed twice. */
                        scratch_free(hist_hdr); scratch_free(off_hdr);
                        strsort_aflag(keys, n_live, /*bp=*/0, parts_bytes,
                                      /*base_offset=*/0, elems, pool, parts);
                    } else {
                        strsort_top_ctx_t tctx = {
                            .src = keys, .dst = tmp, .n = n_live,
                            .n_tasks = n_tasks, .hist = hist, .offsets = off,
                        };

                        /* Pass 1: parallel histogram. */
                        ray_pool_dispatch_n(pool_p, strsort_top_hist_fn,
                                            &tctx, n_tasks);

                        /* Pass 2: sequential prefix-sum.  For each bucket
                         * b, the starting offset is the sum of all counts
                         * in earlier buckets plus all counts in earlier
                         * tasks for this bucket. */
                        int64_t bucket_counts[256];
                        int64_t bucket_starts[256];
                        int64_t sum = 0;
                        for (int b = 0; b < 256; b++) {
                            bucket_starts[b] = sum;
                            int64_t bc = 0;
                            for (uint32_t t = 0; t < n_tasks; t++) {
                                off[t * 256 + b] = sum + bc;
                                bc += hist[t * 256 + b];
                            }
                            bucket_counts[b] = bc;
                            sum += bc;
                        }

                        /* Pass 3: parallel scatter into tmp. */
                        ray_pool_dispatch_n(pool_p, strsort_top_scatter_fn,
                                            &tctx, n_tasks);

                        /* tmp now holds the records partitioned by byte 0. */
                        scratch_free(hist_hdr);
                        scratch_free(off_hdr);

                        /* Pass 4: parallel per-bucket recursive sort. */
                        strsort_bucket_ctx_t bctx = {
                            .keys        = tmp,
                            .starts      = bucket_starts,
                            .counts      = bucket_counts,
                            .parts_bytes = parts_bytes,
                            .base_offset = 0,
                            .elems       = elems,
                            .pool        = pool,
                            .parts       = parts,
                            .start_bp    = 1,
                        };
                        ray_pool_dispatch_n(pool_p, strsort_bucket_fn,
                                            &bctx, 256);

                        keys_sorted = tmp;
                    }
                }
            }

            /* Scatter row indices back (ASC order, parallel). */
            strsort_emit_ctx_t ectx = { sorted_idx, keys_sorted };
            if (go_parallel)
                ray_pool_dispatch(pool_p, strsort_emit_fn, &ectx, n_live);
            else
                strsort_emit_fn(&ectx, 0, 0, n_live);

            if (tmp_hdr) scratch_free(tmp_hdr);

            /* DESC reverses the sorted non-null range. */
            if (desc) {
                for (int64_t i = 0, j = n_live - 1; i < j; i++, j--) {
                    int64_t t = sorted_idx[i];
                    sorted_idx[i] = sorted_idx[j];
                    sorted_idx[j] = t;
                }
            }
        }

        scratch_free(keys_hdr);
    }

    /* If nulls should be first, rotate them to the front. */
    if (null_count > 0 && nulls_first) {
        /* Cheap rotation via three reverses:
         *   reverse [0, n_live); reverse [n_live, nrows); reverse [0, nrows)
         * Takes O(nrows) swaps, no extra memory. */
        int64_t a = 0, b = n_live - 1;
        while (a < b) { int64_t t = sorted_idx[a]; sorted_idx[a] = sorted_idx[b]; sorted_idx[b] = t; a++; b--; }
        a = n_live; b = nrows - 1;
        while (a < b) { int64_t t = sorted_idx[a]; sorted_idx[a] = sorted_idx[b]; sorted_idx[b] = t; a++; b--; }
        a = 0; b = nrows - 1;
        while (a < b) { int64_t t = sorted_idx[a]; sorted_idx[a] = sorted_idx[b]; sorted_idx[b] = t; a++; b--; }
    }

    return true;
}

/* Build SYM rank mapping: intern_id → sorted rank by string value.
 * Caller must scratch_free(*hdr_out) when done.
 * Returns pointer to rank array of size (max_id + 1), or NULL on error. */
/* Parallel max_id scan context */
typedef struct {
    const void* data;
    int8_t      type;
    uint8_t     attrs;
    uint32_t*   pw_max;  /* per-worker max */
} enum_max_ctx_t;

static void enum_max_fn(void* arg, uint32_t wid,
                         int64_t start, int64_t end) {
    enum_max_ctx_t* c = (enum_max_ctx_t*)arg;
    uint32_t local_max = c->pw_max[wid];
    for (int64_t i = start; i < end; i++) {
        uint32_t v = (uint32_t)ray_read_sym(c->data, i, c->type, c->attrs);
        if (v > local_max) local_max = v;
    }
    c->pw_max[wid] = local_max;
}

uint32_t* build_enum_rank(ray_t* col, int64_t nrows, ray_t** hdr_out) {
    *hdr_out = NULL;
    const void* data = ray_data(col);
    int8_t type = col->type;
    uint8_t attrs = col->attrs;

    /* Find max intern ID (parallel for large columns) */
    uint32_t max_id = 0;
    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows > 100000) {
        uint32_t nw = ray_pool_total_workers(pool);
        uint32_t pw_max[nw];
        memset(pw_max, 0, nw * sizeof(uint32_t));
        enum_max_ctx_t ectx = { .data = data, .type = type, .attrs = attrs, .pw_max = pw_max };
        ray_pool_dispatch(pool, enum_max_fn, &ectx, nrows);
        for (uint32_t w = 0; w < nw; w++)
            if (pw_max[w] > max_id) max_id = pw_max[w];
    } else {
        for (int64_t i = 0; i < nrows; i++) {
            uint32_t v = (uint32_t)ray_read_sym(data, i, type, attrs);
            if (v > max_id) max_id = v;
        }
    }

    if (max_id >= UINT32_MAX - 1) { *hdr_out = NULL; return NULL; }
    uint32_t n_ids = max_id + 1;
    uint64_t row_count = nrows > 0 ? (uint64_t)nrows : 0;
    uint64_t dense_budget = row_count > UINT64_MAX / 8u
                          ? UINT64_MAX
                          : row_count * 8u;
    if ((uint64_t)n_ids > 65536u && (uint64_t)n_ids > dense_budget)
        return NULL;

    /* Arena for temporaries (ids, ptrs, lens, tmp) — single reset at end */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Allocate array of intern IDs to sort */
    uint32_t* ids = (uint32_t*)ray_scratch_arena_push(&arena,
                        (size_t)n_ids * sizeof(uint32_t));
    if (!ids) { ray_scratch_arena_reset(&arena); *hdr_out = NULL; return NULL; }
    for (uint32_t i = 0; i < n_ids; i++) ids[i] = i;

    /* Pre-cache raw string pointers and lengths for fast comparison */
    const char** ptrs = (const char**)ray_scratch_arena_push(&arena,
                             (size_t)n_ids * sizeof(const char*));
    uint32_t* lens = (uint32_t*)ray_scratch_arena_push(&arena,
                         (size_t)n_ids * sizeof(uint32_t));
    if (!ptrs || !lens) {
        ray_scratch_arena_reset(&arena); *hdr_out = NULL; return NULL;
    }
    /* The ids being ranked are CELL DATA of `col`: resolve each through
     * the COLUMN's domain (sym-domain Phase 2).  Every consumer of the
     * rank LUT (radix encodes in sort.c/window.c) keeps reading raw
     * cell ids — only the id→string source changes.  Runtime singleton
     * delegates to ray_sym_str: exact no-op pre-flip. */
    struct ray_sym_domain_s* dom = ray_sym_vec_domain(col);
    for (uint32_t i = 0; i < n_ids; i++) {
        ray_t* s = ray_sym_domain_str(dom, (int64_t)i);
        if (s) {
            ptrs[i] = ray_str_ptr(s);
            lens[i] = (uint32_t)ray_str_len(s);
        } else {
            ptrs[i] = NULL;
            lens[i] = 0;
        }
    }

    /* Merge sort intern IDs by full string comparison.  For ≤100K SYM
     * values this completes in <1ms and correctly handles strings that
     * share long common prefixes (e.g. "id000000001"–"id000099999"). */
    {
        uint32_t* tmp = (uint32_t*)ray_scratch_arena_push(&arena,
                             (size_t)n_ids * sizeof(uint32_t));
        if (!tmp) { ray_scratch_arena_reset(&arena);
                    *hdr_out = NULL; return NULL; }

        /* Bottom-up merge sort */
        for (uint32_t width = 1; width < n_ids; width *= 2) {
            for (uint32_t i = 0; i < n_ids; i += 2 * width) {
                uint32_t lo = i;
                uint32_t mid = lo + width;
                if (mid > n_ids) mid = n_ids;
                uint32_t hi = lo + 2 * width;
                if (hi > n_ids) hi = n_ids;
                /* Merge ids[lo..mid) and ids[mid..hi) into tmp[lo..hi) */
                uint32_t a = lo, b = mid, k = lo;
                while (a < mid && b < hi) {
                    uint32_t ia = ids[a], ib = ids[b];
                    uint32_t la = lens[ia], lb = lens[ib];
                    uint32_t ml = la < lb ? la : lb;
                    int cmp = 0;
                    if (ml > 0) cmp = memcmp(ptrs[ia], ptrs[ib], ml);
                    if (cmp == 0) cmp = (la > lb) - (la < lb);
                    if (cmp <= 0) tmp[k++] = ids[a++];
                    else          tmp[k++] = ids[b++];
                }
                while (a < mid) tmp[k++] = ids[a++];
                while (b < hi)  tmp[k++] = ids[b++];
            }
            /* Swap ids and tmp */
            uint32_t* s = ids; ids = tmp; tmp = s;
        }
    }

    /* Build rank[intern_id] = sorted position (output — not arena'd) */
    ray_t* rank_hdr;
    uint32_t* rank = (uint32_t*)scratch_calloc(&rank_hdr,
                        (size_t)n_ids * sizeof(uint32_t));
    if (!rank) { ray_scratch_arena_reset(&arena); *hdr_out = NULL; return NULL; }

    for (uint32_t i = 0; i < n_ids; i++)
        rank[ids[i]] = i;

    ray_scratch_arena_reset(&arena);  /* free all temporaries at once */
    *hdr_out = rank_hdr;
    return rank;
}

/* Insertion sort for small arrays — used as base case for merge sort */
void sort_insertion(const sort_cmp_ctx_t* ctx, int64_t* arr, int64_t n) {
    for (int64_t i = 1; i < n; i++) {
        int64_t key = arr[i];
        int64_t j = i - 1;
        while (j >= 0 && sort_cmp(ctx, arr[j], key) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/* Single-threaded merge sort (recursive, with insertion sort base case) */
void sort_merge_recursive(const sort_cmp_ctx_t* ctx,
                                  int64_t* arr, int64_t* tmp, int64_t n) {
    if (n <= 64) {
        sort_insertion(ctx, arr, n);
        return;
    }
    int64_t mid = n / 2;
    sort_merge_recursive(ctx, arr, tmp, mid);
    sort_merge_recursive(ctx, arr + mid, tmp + mid, n - mid);

    /* Merge arr[0..mid) and arr[mid..n) into tmp, then copy back */
    int64_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (sort_cmp(ctx, arr[i], arr[j]) <= 0)
            tmp[k++] = arr[i++];
        else
            tmp[k++] = arr[j++];
    }
    while (i < mid) tmp[k++] = arr[i++];
    while (j < n) tmp[k++] = arr[j++];
    memcpy(arr, tmp, (size_t)n * sizeof(int64_t));
}

/* sort_phase1_ctx_t defined in exec_internal.h */

void sort_phase1_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    sort_phase1_ctx_t* ctx = (sort_phase1_ctx_t*)arg;
    for (int64_t chunk_idx = start; chunk_idx < end; chunk_idx++) {
        int64_t chunk_size = (ctx->nrows + ctx->n_chunks - 1) / ctx->n_chunks;
        int64_t lo = chunk_idx * chunk_size;
        int64_t hi = lo + chunk_size;
        if (hi > ctx->nrows) hi = ctx->nrows;
        if (lo >= hi) continue;
        sort_merge_recursive(ctx->cmp_ctx, ctx->indices + lo, ctx->tmp + lo, hi - lo);
    }
}

/* Merge two adjacent sorted runs: [lo..mid) and [mid..hi) from src into dst */
static void merge_runs(const sort_cmp_ctx_t* ctx,
                        const int64_t* src, int64_t* dst,
                        int64_t lo, int64_t mid, int64_t hi) {
    int64_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (sort_cmp(ctx, src[i], src[j]) <= 0)
            dst[k++] = src[i++];
        else
            dst[k++] = src[j++];
    }
    while (i < mid) dst[k++] = src[i++];
    while (j < hi) dst[k++] = src[j++];
}

/* sort_merge_ctx_t defined in exec_internal.h */

void sort_merge_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    sort_merge_ctx_t* ctx = (sort_merge_ctx_t*)arg;
    for (int64_t pair_idx = start; pair_idx < end; pair_idx++) {
        int64_t lo = pair_idx * 2 * ctx->run_size;
        int64_t mid = lo + ctx->run_size;
        int64_t hi = mid + ctx->run_size;
        if (mid > ctx->nrows) mid = ctx->nrows;
        if (hi > ctx->nrows) hi = ctx->nrows;
        if (lo >= ctx->nrows) continue;
        if (mid >= hi) {
            /* Only one run — copy directly */
            memcpy(ctx->dst + lo, ctx->src + lo, (size_t)(hi - lo) * sizeof(int64_t));
        } else {
            merge_runs(ctx->cmp_ctx, ctx->src, ctx->dst, lo, mid, hi);
        }
    }
}

/* --------------------------------------------------------------------------
 * Parallel multi-key min/max prescan for composite radix sort.
 * Each worker scans all n_keys columns over its row range, then the main
 * thread merges per-worker results.
 * -------------------------------------------------------------------------- */

/* MK_PRESCAN_MAX_KEYS, mk_prescan_ctx_t defined in exec_internal.h */

void mk_prescan_fn(void* arg, uint32_t wid,
                           int64_t start, int64_t end) {
    mk_prescan_ctx_t* c = (mk_prescan_ctx_t*)arg;
    uint32_t nk = c->n_keys;
    int64_t* my_mins = c->pw_mins + (int64_t)wid * nk;
    int64_t* my_maxs = c->pw_maxs + (int64_t)wid * nk;

    /* Initialize on first morsel, merge on subsequent */
    for (uint32_t k = 0; k < nk; k++) {
        if (my_mins[k] == INT64_MAX) {
            /* first morsel for this worker — will be set below */
        }
    }

    for (uint32_t k = 0; k < nk; k++) {
        ray_t* col = c->vecs[k];
        int64_t kmin = my_mins[k], kmax = my_maxs[k];

        if (c->enum_ranks[k]) {
            const void* cdata = ray_data(col);
            int8_t ctype = col->type;
            uint8_t cattrs = col->attrs;
            const uint32_t* ranks = c->enum_ranks[k];
            for (int64_t i = start; i < end; i++) {
                uint32_t raw = (uint32_t)ray_read_sym(cdata, i, ctype, cattrs);
                int64_t v = (int64_t)ranks[raw];
                if (v < kmin) kmin = v;
                if (v > kmax) kmax = v;
            }
        } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
            const int64_t* d = (const int64_t*)ray_data(col);
            for (int64_t i = start; i < end; i++) {
                if (d[i] < kmin) kmin = d[i];
                if (d[i] > kmax) kmax = d[i];
            }
        } else if (col->type == RAY_F64) {
            const double* d = (const double*)ray_data(col);
            for (int64_t i = start; i < end; i++) {
                uint64_t bits;
                memcpy(&bits, &d[i], 8);
                uint64_t mask = -(bits >> 63) | ((uint64_t)1 << 63);
                int64_t v = (int64_t)(bits ^ mask);
                if (v < kmin) kmin = v;
                if (v > kmax) kmax = v;
            }
        } else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME) {
            const int32_t* d = (const int32_t*)ray_data(col);
            for (int64_t i = start; i < end; i++) {
                int64_t v = (int64_t)d[i];
                if (v < kmin) kmin = v;
                if (v > kmax) kmax = v;
            }
        } else if (col->type == RAY_I16) {
            const int16_t* d = (const int16_t*)ray_data(col);
            for (int64_t i = start; i < end; i++) {
                int64_t v = (int64_t)d[i];
                if (v < kmin) kmin = v;
                if (v > kmax) kmax = v;
            }
        } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
            const uint8_t* d = (const uint8_t*)ray_data(col);
            for (int64_t i = start; i < end; i++) {
                int64_t v = (int64_t)d[i];
                if (v < kmin) kmin = v;
                if (v > kmax) kmax = v;
            }
        }

        my_mins[k] = kmin;
        my_maxs[k] = kmax;
    }
}

/* Decode sorted radix keys directly into a typed output vector.
 * Sequential writes — no random access. */
static void radix_decode_into(void* dst, int8_t type, const uint64_t* sorted_keys,
                               int64_t n, bool desc) {
    if (type == RAY_I64 || type == RAY_TIMESTAMP) {
        int64_t* d = (int64_t*)dst;
        if (desc)
            for (int64_t i = 0; i < n; i++)
                d[i] = (int64_t)(~sorted_keys[i] ^ ((uint64_t)1 << 63));
        else
            for (int64_t i = 0; i < n; i++)
                d[i] = (int64_t)(sorted_keys[i] ^ ((uint64_t)1 << 63));
    } else if (type == RAY_F64) {
        double* d = (double*)dst;
        for (int64_t i = 0; i < n; i++) {
            uint64_t k = desc ? ~sorted_keys[i] : sorted_keys[i];
            /* Inverse of encode: positive originals have MSB=1 in key (flip sign bit),
             * negative originals have MSB=0 in key (flip all bits). */
            uint64_t mask = (k >> 63) ? ((uint64_t)1 << 63) : ~(uint64_t)0;
            uint64_t bits = k ^ mask;
            memcpy(&d[i], &bits, 8);
        }
    } else if (type == RAY_F32) {
        float* d = (float*)dst;
        for (int64_t i = 0; i < n; i++) {
            uint32_t k = (uint32_t)sorted_keys[i];
            if (desc) k = ~k;
            uint32_t mask = (k >> 31) ? ((uint32_t)1 << 31) : ~(uint32_t)0;
            uint32_t bits = k ^ mask;
            memcpy(&d[i], &bits, 4);
        }
    } else if (type == RAY_I32 || type == RAY_DATE || type == RAY_TIME) {
        int32_t* d = (int32_t*)dst;
        if (desc)
            for (int64_t i = 0; i < n; i++)
                d[i] = (int32_t)((uint32_t)(~sorted_keys[i]) ^ ((uint32_t)1 << 31));
        else
            for (int64_t i = 0; i < n; i++)
                d[i] = (int32_t)((uint32_t)sorted_keys[i] ^ ((uint32_t)1 << 31));
    } else if (type == RAY_I16) {
        int16_t* d = (int16_t*)dst;
        if (desc)
            for (int64_t i = 0; i < n; i++)
                d[i] = (int16_t)((uint16_t)(~sorted_keys[i]) ^ ((uint16_t)1 << 15));
        else
            for (int64_t i = 0; i < n; i++)
                d[i] = (int16_t)((uint16_t)sorted_keys[i] ^ ((uint16_t)1 << 15));
    } else if (type == RAY_BOOL || type == RAY_U8) {
        uint8_t* d = (uint8_t*)dst;
        if (desc)
            for (int64_t i = 0; i < n; i++) d[i] = (uint8_t)(~sorted_keys[i]);
        else
            for (int64_t i = 0; i < n; i++) d[i] = (uint8_t)sorted_keys[i];
    }
}

/* Sort columns and return index array (extended: optionally returns sorted keys).
 * cols:        array of n_cols vectors (sort keys, most significant first)
 * descs:       array of n_cols flags (0=asc, 1=desc), or NULL for all-asc
 * nulls_first: array of n_cols flags (0=nulls last, 1=nulls first), or NULL
 *              for default convention (nulls last for asc, nulls first for desc)
 * n_cols:      number of sort key columns (composite radix encode is used
 *              up to 16 keys; wider sorts take the merge-sort fallback)
 * nrows:       number of rows in each column
 * sorted_keys_out: if non-NULL, receives sorted radix keys (caller frees keys_hdr_out)
 * keys_hdr_out:    if non-NULL, receives scratch header for sorted_keys_out
 * Returns:     ray_t* I64 vector of sorted indices (caller owns), or RAY_ERROR */
static ray_t* sort_indices_ex(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                               uint32_t n_cols, int64_t nrows,
                               uint64_t** sorted_keys_out, ray_t** keys_hdr_out) {
    if (n_cols == 0 || nrows <= 0)
        return ray_vec_new(RAY_I64, 0);
    /* No n_cols cap here: the composite-radix fast path below (`fits`)
     * uses a fixed-16-key encode context and gates itself on n_cols <= 16;
     * anything wider (or any key type it declines) falls through to the
     * generic comparator-based merge sort, which is n_cols-unbounded. */

    /* Allocate index array */
    ray_t* indices_hdr;
    int64_t* indices = (int64_t*)scratch_alloc(&indices_hdr,
                            (size_t)nrows * sizeof(int64_t));
    if (!indices) return ray_error("oom", NULL);
    bool iota_done = false;

    /* --- Radix sort fast path ------------------------------------------------
     * Try radix sort for integer/float/enum keys.  Falls back to merge sort
     * for unsupported types (SYM with arbitrary strings, mixed types, etc.). */
    bool radix_done = false;
    int64_t* sorted_idx = indices;  /* may point to itmp after radix sort */
    ray_t* radix_itmp_hdr = NULL;   /* kept alive until we copy out */
    ray_t* enum_rank_hdrs_hdr;
    ray_t** enum_rank_hdrs = (ray_t**)scratch_calloc(&enum_rank_hdrs_hdr,
                                 (size_t)n_cols * sizeof(ray_t*));
    if (!enum_rank_hdrs) { scratch_free(indices_hdr); return ray_error("oom", NULL); }

    if (nrows > 64) {
        /* RAY_STR single-key fast path — dedicated MSD byte-radix
         * sort.  Handles variable-width strings, nulls, and DESC
         * internally; skips the rest of sort_indices_ex on success. */
        if (n_cols == 1 && cols[0]->type == RAY_STR) {
            bool desc = descs ? descs[0] : 0;
            bool nf   = nulls_first ? nulls_first[0] : !desc;
            if (sort_str_msd_inplace(indices, nrows, cols[0], desc, nf)) {
                sorted_idx = indices;
                iota_done = true;
                radix_done = true;
                goto str_msd_done;
            }
            /* OOM — fall through to comparison merge sort. */
        }

        /* Check if all sort keys are radix-sortable types.
         * RAY_STR and RAY_GUID are accepted for multi-key sorts only:
         * they have no packed uint64 encoding, so the composite-radix
         * path can't fit them, but the rank-then-compose fallback handles
         * them via single-key sort_indices_ex recursion (which hits the
         * RAY_STR MSD byte-radix path for strings, or the merge-sort
         * path with the new RAY_GUID comparator for guids). */
        bool can_radix = true;
        bool has_wide_key = false;  /* RAY_STR or RAY_GUID — forces rank fallback */
        for (uint32_t k = 0; k < n_cols; k++) {
            if (!cols[k]) { can_radix = false; break; }
            int8_t t = cols[k]->type;
            if (t == RAY_STR || t == RAY_GUID) { has_wide_key = true; continue; }
            if (t == RAY_F32 && n_cols != 1) { can_radix = false; break; }
            if (t != RAY_I64 && t != RAY_F64 && t != RAY_F32 &&
                t != RAY_I32 && t != RAY_I16 &&
                t != RAY_BOOL && t != RAY_U8 && t != RAY_SYM &&
                t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP) {
                can_radix = false; break;
            }
        }
        /* Single-key wide types: RAY_STR has its own MSD fast path above;
         * single-key RAY_GUID falls through to merge sort with the new
         * comparator. In both cases the multi-key composite path is not
         * applicable, so disable the radix branch. */
        if (has_wide_key && n_cols == 1) can_radix = false;

        if (can_radix) {
            ray_pool_t* pool = ray_pool_get();

            /* Build SYM rank mappings (intern_id -> sorted rank by string) */
            ray_t* enum_ranks_hdr;
            uint32_t** enum_ranks = (uint32_t**)scratch_calloc(&enum_ranks_hdr,
                                         (size_t)n_cols * sizeof(uint32_t*));
            if (enum_ranks) {
                for (uint32_t k = 0; k < n_cols; k++) {
                    if (RAY_IS_SYM(cols[k]->type)) {
                        enum_ranks[k] = build_enum_rank(cols[k], nrows,
                                                         &enum_rank_hdrs[k]);
                        if (!enum_ranks[k]) { can_radix = false; break; }
                    }
                }
            } else {
                can_radix = false;
            }

            if (can_radix && n_cols == 1) {
                /* --- Single-key sort --- */
                uint8_t key_nbytes_max = radix_key_bytes(cols[0]->type);
                /* Narrow-int + has_nulls uses a +1-shifted encoding so
                 * the null sentinel sits one byte beyond the data
                 * range; reserve that extra byte for the radix pass. */
                if ((cols[0]->attrs & RAY_ATTR_HAS_NULLS) &&
                    (cols[0]->type == RAY_BOOL || cols[0]->type == RAY_U8 ||
                     cols[0]->type == RAY_I16) &&
                    key_nbytes_max < 8) {
                    key_nbytes_max++;
                }

                /* Skip pool for small arrays - dispatch overhead dominates */
                ray_pool_t* sk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;

                /* Encode keys (needed by all paths) */
                ray_t *keys_hdr;
                uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                    (size_t)nrows * sizeof(uint64_t));
                if (keys) {
                    bool desc = descs ? descs[0] : 0;
                    /* Null = minimum value.
                     * ASC → nulls first, DESC → nulls last. */
                    bool nf = nulls_first ? nulls_first[0] : !desc;
                    radix_encode_ctx_t enc = {
                        .keys = keys, .indices = indices,
                        .data = ray_data(cols[0]),
                        .col = cols[0],
                        .type = cols[0]->type,
                        .col_attrs = cols[0]->attrs,
                        .desc = desc,
                        .nulls_first = nf,
                        .enum_rank = enum_ranks[0], .n_keys = 1,
                    };
                    if (sk_pool)
                        ray_pool_dispatch(sk_pool, radix_encode_fn, &enc, nrows);
                    else
                        radix_encode_fn(&enc, 0, 0, nrows);
                    iota_done = true;

                    if (nrows <= RADIX_SORT_THRESHOLD) {
                        /* Introsort on encoded keys - faster than multi-pass
                         * radix for small arrays (avoids scatter overhead). */
                        key_introsort(keys, indices, nrows);
                        sorted_idx = indices;
                        radix_done = true;
                    } else {
                        /* Data-range-adaptive byte count: scan encoded keys
                         * to skip bytes that are uniform across all values,
                         * avoiding wasteful histogram passes. */
                        uint8_t key_nbytes = compute_key_nbytes(
                            sk_pool, keys, nrows, key_nbytes_max);

                        /* Try packed radix sort: pack key + index into one
                         * uint64_t to halve memory traffic per pass.
                         * Feasible when key_nbytes*8 + index_bits <= 64. */
                        uint8_t idx_bits = 0;
                        { int64_t nn = nrows; while (nn > 0) { idx_bits++; nn >>= 1; } }
                        bool use_packed = (key_nbytes <= 3
                                           && key_nbytes * 8 + idx_bits <= 64);

                        if (use_packed) {
                            uint8_t key_bits = key_nbytes * 8;
                            ray_t *ptmp_hdr;
                            uint64_t* ptmp = (uint64_t*)scratch_alloc(&ptmp_hdr,
                                                (size_t)nrows * sizeof(uint64_t));
                            if (ptmp) {
                                /* Fuse packing with sortedness + reverse detection */
                                uint32_t pd_nw = sk_pool ? ray_pool_total_workers(sk_pool) : 1;
                                int64_t pd_pw[pd_nw], pd_nr[pd_nw];
                                memset(pd_pw, 0, (size_t)pd_nw * sizeof(int64_t));
                                memset(pd_nr, 0, (size_t)pd_nw * sizeof(int64_t));
                                uint64_t key_mask_pd =
                                    (key_bits < 64) ? ((1ULL << key_bits) - 1) : ~0ULL;
                                packed_detect_ctx_t pd_ctx = {
                                    .keys = keys, .key_bits = key_bits,
                                    .key_mask = key_mask_pd,
                                    .pw_unsorted = pd_pw, .pw_not_reverse = pd_nr,
                                };

                                if (sk_pool)
                                    ray_pool_dispatch(sk_pool, packed_detect_fn, &pd_ctx, nrows);
                                else
                                    packed_detect_fn(&pd_ctx, 0, 0, nrows);

                                /* Aggregate sortedness results */
                                int64_t total_unsorted = 0, total_not_rev = 0;
                                for (uint32_t t = 0; t < pd_nw; t++) {
                                    total_unsorted += pd_pw[t];
                                    total_not_rev += pd_nr[t];
                                }
                                /* Check cross-task boundaries */
                                int64_t grain = RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
                                uint64_t key_mask_s =
                                    (key_bits < 64) ? ((1ULL << key_bits) - 1) : ~0ULL;
                                for (int64_t b = grain; b < nrows; b += grain) {
                                    uint64_t ka = keys[b-1] & key_mask_s;
                                    uint64_t kb2 = keys[b] & key_mask_s;
                                    if (kb2 < ka) total_unsorted++;
                                    if (kb2 > ka) total_not_rev++;
                                }

                                if (total_unsorted == 0) {
                                    /* Already sorted - identity permutation */
                                    sorted_idx = indices;
                                    radix_done = true;
                                } else if (total_not_rev == 0 && nrows > 1) {
                                    /* Reverse-sorted - reverse indices in O(n) */
                                    for (int64_t i = 0; i < nrows; i++)
                                        indices[i] = nrows - 1 - i;
                                    sorted_idx = indices;
                                    radix_done = true;
                                } else {
                                    /* Packed radix sort - half the memory traffic */
                                    uint64_t* sorted = packed_radix_sort_run(
                                        sk_pool, keys, ptmp, nrows, key_nbytes);

                                    if (sorted) {
                                        uint64_t idx_mask =
                                            (idx_bits < 64) ? ((1ULL << idx_bits) - 1) : ~0ULL;

                                        /* Packed path: keys are truncated to key_bits,
                                         * not full 64-bit encoded keys — can't decode. */
                                        packed_unpack_ctx_t up = {
                                            .sorted = sorted, .indices = indices,
                                            .keys_out = NULL,
                                            .key_bits = key_bits,
                                            .idx_mask = idx_mask, .key_mask = 0,
                                            .extract_keys = false,
                                        };
                                        if (sk_pool)
                                            ray_pool_dispatch(sk_pool, packed_unpack_fn, &up, nrows);
                                        else
                                            packed_unpack_fn(&up, 0, 0, nrows);

                                        sorted_idx = indices;
                                        radix_done = true;
                                    }
                                }
                            }
                            scratch_free(ptmp_hdr);
                        } else {
                            /* Non-packed path: detect sortedness first */
                            double us_frac2 = detect_sortedness(sk_pool, keys, nrows);
                            if (us_frac2 == 0.0) {
                                sorted_idx = indices;
                                radix_done = true;
                            }
                            /* Standard dual-array radix sort */
                            if (!radix_done) {
                                ray_t *ktmp_hdr, *itmp_hdr;
                                uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                                    (size_t)nrows * sizeof(uint64_t));
                                int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                                    (size_t)nrows * sizeof(int64_t));
                                if (ktmp && itmp) {
                                    bool want_sk = sorted_keys_out
                                        && !RAY_IS_SYM(cols[0]->type);
                                    uint64_t* sk_out = NULL;
                                    sorted_idx = msd_radix_sort_run(sk_pool, keys, indices,
                                                                     ktmp, itmp, nrows,
                                                                     key_nbytes,
                                                                     want_sk ? &sk_out : NULL);
                                    radix_done = (sorted_idx != NULL);
                                    if (radix_done && want_sk && sk_out) {
                                        *sorted_keys_out = sk_out;
                                        if (sk_out == ktmp) {
                                            *keys_hdr_out = ktmp_hdr;
                                            ktmp_hdr = NULL;
                                        } else {
                                            /* Even number of radix passes:
                                             * sorted keys ended up in the
                                             * original keys buffer. */
                                            *keys_hdr_out = keys_hdr;
                                            keys_hdr = NULL;
                                        }
                                    }
                                }
                                if (ktmp_hdr) scratch_free(ktmp_hdr);
                                if (sorted_idx != itmp) scratch_free(itmp_hdr);
                                else radix_itmp_hdr = itmp_hdr;
                            }
                        }
                    }
                }
                scratch_free(keys_hdr);

            } else if (can_radix && n_cols > 1) {
                /* --- Multi-key composite radix sort --- */
                ray_t* minmax_hdr;
                int64_t* minmax = (int64_t*)scratch_alloc(&minmax_hdr,
                                      2 * (size_t)n_cols * sizeof(int64_t));
              if (minmax) {
                int64_t* mins = minmax;
                int64_t* maxs = minmax + n_cols;
                /* Wider accumulator: up to 16 keys * 63 bits = 1008,
                 * which would wrap a uint8_t and let an oversized
                 * budget falsely pass the <=64 fits check. */
                uint16_t total_bits = 0;
                bool fits = true;

                ray_pool_t* mk_prescan_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;
                if (has_wide_key) {
                    /* RAY_STR / RAY_GUID can't be packed into a composite
                     * uint64 key. Force the rank-then-compose fallback. */
                    total_bits = UINT16_MAX;
                    fits = false;
                } else if (n_cols <= MK_PRESCAN_MAX_KEYS && mk_prescan_pool) {
                    uint32_t nw = ray_pool_total_workers(mk_prescan_pool);
                    size_t pw_count = (size_t)nw * n_cols;
                    int64_t pw_mins_stack[512], pw_maxs_stack[512];
                    ray_t *pw_mins_hdr = NULL, *pw_maxs_hdr = NULL;
                    int64_t* pw_mins = (pw_count <= 512)
                        ? pw_mins_stack
                        : (int64_t*)scratch_alloc(&pw_mins_hdr, pw_count * sizeof(int64_t));
                    int64_t* pw_maxs = (pw_count <= 512)
                        ? pw_maxs_stack
                        : (int64_t*)scratch_alloc(&pw_maxs_hdr, pw_count * sizeof(int64_t));
                    for (size_t i = 0; i < pw_count; i++) {
                        pw_mins[i] = INT64_MAX;
                        pw_maxs[i] = INT64_MIN;
                    }
                    mk_prescan_ctx_t pctx = {
                        .vecs = cols, .enum_ranks = enum_ranks,
                        .n_keys = n_cols, .nrows = nrows, .n_workers = nw,
                        .pw_mins = pw_mins, .pw_maxs = pw_maxs,
                    };
                    ray_pool_dispatch(mk_prescan_pool, mk_prescan_fn, &pctx, nrows);

                    /* Merge per-worker results */
                    for (uint32_t k = 0; k < n_cols; k++) {
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;
                        for (uint32_t w = 0; w < nw; w++) {
                            int64_t wmin = pw_mins[w * n_cols + k];
                            int64_t wmax = pw_maxs[w * n_cols + k];
                            if (wmin < kmin) kmin = wmin;
                            if (wmax > kmax) kmax = wmax;
                        }
                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits = (uint16_t)(total_bits + bits);
                    }
                    if (pw_mins_hdr) scratch_free(pw_mins_hdr);
                    if (pw_maxs_hdr) scratch_free(pw_maxs_hdr);
                } else {
                    /* Sequential fallback (no pool or too many keys) */
                    for (uint32_t k = 0; k < n_cols; k++) {
                        ray_t* col = cols[k];
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;

                        if (enum_ranks[k]) {
                            const void* cdata = ray_data(col);
                            int8_t ctype = col->type;
                            uint8_t cattrs = col->attrs;
                            for (int64_t i = 0; i < nrows; i++) {
                                uint32_t raw = (uint32_t)ray_read_sym(cdata, i, ctype, cattrs);
                                int64_t v = (int64_t)enum_ranks[k][raw];
                                if (v < kmin) kmin = v;
                                if (v > kmax) kmax = v;
                            }
                        } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
                            const int64_t* d = (const int64_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = d[i];
                                if (d[i] > kmax) kmax = d[i];
                            }
                        } else if (col->type == RAY_F64) {
                            const double* d = (const double*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                uint64_t bits;
                                memcpy(&bits, &d[i], 8);
                                uint64_t mask = -(bits >> 63) | ((uint64_t)1 << 63);
                                int64_t v = (int64_t)(bits ^ mask);
                                if (v < kmin) kmin = v;
                                if (v > kmax) kmax = v;
                            }
                        } else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME) {
                            const int32_t* d = (const int32_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_I16) {
                            const int16_t* d = (const int16_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
                            const uint8_t* d = (const uint8_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        }

                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits = (uint16_t)(total_bits + bits);
                    }
                }

                if (total_bits > 64) {
                    fits = false;
                    /* --- Rank-then-compose fallback ---
                     * The composite bit budget overflows because at least
                     * one key has a value range that doesn't fit (typical:
                     * F64 columns whose sign-flipped IEEE-754 encoding
                     * spans most of the 64-bit space).  Fall back to a
                     * rank-encoded composite: for each key, run a single-
                     * key sort to produce a dense rank in [0..K_k), then
                     * compose the ranks.  Bits per key shrinks from
                     * "data range" to "ceil(log2 distinct_count)", which
                     * always fits for n_cols * ceil(log2 nrows) <= 64. */
                    ray_t* rank_buf_hdr;
                    size_t rank_off_ranks = (size_t)n_cols * sizeof(ray_t*);
                    size_t rank_off_max = rank_off_ranks
                                          + (size_t)n_cols * sizeof(uint32_t*);
                    size_t rank_buf_bytes = rank_off_max
                                            + (size_t)n_cols * sizeof(uint32_t);
                    uint8_t* rank_buf = (uint8_t*)scratch_calloc(&rank_buf_hdr,
                                                                  rank_buf_bytes);
                    bool rank_ok = rank_buf != NULL;
                    ray_t** rank_hdrs = (ray_t**)rank_buf;
                    uint32_t** ranks = (uint32_t**)(rank_buf + rank_off_ranks);
                    uint32_t* rank_max = (uint32_t*)(rank_buf + rank_off_max);
                    for (uint32_t k = 0; k < n_cols && rank_ok; k++) {
                        uint8_t kdesc = descs ? descs[k] : 0;
                        uint8_t knf   = nulls_first ? nulls_first[k] : !kdesc;
                        ray_t* col_arg[1] = { cols[k] };
                        uint8_t desc_arg[1] = { kdesc };
                        uint8_t nf_arg[1]   = { knf };
                        ray_t* sk_idx = sort_indices_ex(col_arg, desc_arg,
                                                         nf_arg, 1, nrows,
                                                         NULL, NULL);
                        if (!sk_idx || RAY_IS_ERR(sk_idx)) { rank_ok = false; break; }
                        int64_t* sk_idx_data = (int64_t*)ray_data(sk_idx);
                        uint32_t* r = (uint32_t*)scratch_alloc(&rank_hdrs[k],
                                          (size_t)nrows * sizeof(uint32_t));
                        if (!r) { ray_release(sk_idx); rank_ok = false; break; }
                        ranks[k] = r;
                        /* Dense-rank tie detection must use the same null
                         * ordering as the sub-sort so that null/non-null
                         * pairs aren't treated as ties (and so that two
                         * nulls do collapse to the same rank). */
                        sort_cmp_ctx_t cctx = {
                            .vecs = col_arg, .desc = desc_arg,
                            .nulls_first = nf_arg, .n_sort = 1,
                        };
                        uint32_t cur = 0;
                        r[sk_idx_data[0]] = 0;
                        for (int64_t i = 1; i < nrows; i++) {
                            if (sort_cmp(&cctx, sk_idx_data[i-1], sk_idx_data[i]) != 0)
                                cur++;
                            r[sk_idx_data[i]] = cur;
                        }
                        rank_max[k] = cur;
                        ray_release(sk_idx);
                    }
                    if (rank_ok) {
                        ray_t* rank_bits_hdr;
                        uint8_t* rank_bits = (uint8_t*)scratch_alloc(&rank_bits_hdr,
                                                                      n_cols);
                      if (rank_bits) {
                        /* Accumulate in a wider type: up to 16 keys * 63
                         * bits each = 1008, which would wrap a uint8_t. */
                        uint16_t rank_total = 0;
                        for (uint32_t k = 0; k < n_cols; k++) {
                            uint8_t b = 1;
                            while (((uint64_t)1 << b) <= rank_max[k] && b < 64) b++;
                            rank_bits[k] = b;
                            rank_total = (uint16_t)(rank_total + b);
                        }
                        if (rank_total <= 64) {
                            ray_t* rshift_hdr;
                            uint8_t* rshift = (uint8_t*)scratch_alloc(&rshift_hdr,
                                                                       n_cols);
                          if (rshift) {
                            uint16_t accum = 0;
                            for (int k = n_cols - 1; k >= 0; k--) {
                                rshift[k] = (uint8_t)accum;
                                accum = (uint16_t)(accum + rank_bits[k]);
                            }
                            uint8_t rcomp_nbytes = (uint8_t)((rank_total + 7) / 8);
                            if (rcomp_nbytes < 1) rcomp_nbytes = 1;
                            ray_pool_t* rk_pool =
                                (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;
                            ray_t* rkeys_hdr;
                            uint64_t* rkeys = (uint64_t*)scratch_alloc(&rkeys_hdr,
                                                  (size_t)nrows * sizeof(uint64_t));
                            if (rkeys) {
                                for (int64_t i = 0; i < nrows; i++) {
                                    uint64_t composite = 0;
                                    for (uint32_t k = 0; k < n_cols; k++)
                                        composite |= ((uint64_t)ranks[k][i]) << rshift[k];
                                    rkeys[i] = composite;
                                    indices[i] = i;
                                }
                                iota_done = true;
                                if (nrows <= RADIX_SORT_THRESHOLD) {
                                    key_introsort(rkeys, indices, nrows);
                                    sorted_idx = indices;
                                    radix_done = true;
                                } else {
                                    ray_t *rktmp_hdr, *ritmp_hdr;
                                    uint64_t* rktmp = (uint64_t*)scratch_alloc(&rktmp_hdr,
                                                          (size_t)nrows * sizeof(uint64_t));
                                    int64_t* ritmp = (int64_t*)scratch_alloc(&ritmp_hdr,
                                                         (size_t)nrows * sizeof(int64_t));
                                    if (rktmp && ritmp) {
                                        sorted_idx = msd_radix_sort_run(
                                            rk_pool, rkeys, indices,
                                            rktmp, ritmp, nrows, rcomp_nbytes, NULL);
                                        radix_done = (sorted_idx != NULL);
                                    }
                                    if (rktmp_hdr) scratch_free(rktmp_hdr);
                                    if (sorted_idx != ritmp) {
                                        if (ritmp_hdr) scratch_free(ritmp_hdr);
                                    } else {
                                        radix_itmp_hdr = ritmp_hdr;
                                    }
                                }
                                scratch_free(rkeys_hdr);
                            }
                          } /* if (rshift) */
                            scratch_free(rshift_hdr);
                        }
                      } /* if (rank_bits) */
                        scratch_free(rank_bits_hdr);
                    }
                    if (rank_buf) {
                        for (uint32_t k = 0; k < n_cols; k++)
                            if (rank_hdrs[k]) scratch_free(rank_hdrs[k]);
                    }
                    scratch_free(rank_buf_hdr);
                }

                /* radix_encode_ctx_t (internal.h) carries fixed 16-key
                 * mins/ranges/bit_shifts/descs/enum_ranks arrays — this
                 * direct-encode path is only safe up to that width.  Wider
                 * composites (any n_cols above 16) fall through to the
                 * merge sort below rather than overrun those arrays. */
                if (fits && n_cols <= 16) {
                    /* Compute bit-shift for each key: primary key in MSBs */
                    uint8_t bit_shifts[n_cols];
                    uint8_t accum = 0;
                    for (int k = n_cols - 1; k >= 0; k--) {
                        bit_shifts[k] = accum;
                        uint64_t range = (uint64_t)(maxs[k] - mins[k]);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        accum += bits;
                    }

                    uint8_t comp_nbytes = (total_bits + 7) / 8;
                    if (comp_nbytes < 1) comp_nbytes = 1;
                    ray_pool_t* mk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;

                    {
                        /* Encode composite keys */
                        ray_t *keys_hdr;
                        uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                            (size_t)nrows * sizeof(uint64_t));
                        if (keys) {
                            radix_encode_ctx_t enc = {
                                .keys = keys, .indices = indices,
                                .n_keys = n_cols, .vecs = cols,
                            };
                            for (uint32_t k = 0; k < n_cols; k++) {
                                enc.mins[k] = mins[k];
                                enc.ranges[k] = maxs[k] - mins[k];
                                enc.bit_shifts[k] = bit_shifts[k];
                                enc.descs[k] = descs ? descs[k] : 0;
                                enc.enum_ranks[k] = enum_ranks[k];
                            }
                            if (mk_pool)
                                ray_pool_dispatch(mk_pool, radix_encode_fn, &enc, nrows);
                            else
                                radix_encode_fn(&enc, 0, 0, nrows);
                            iota_done = true;

                            /* Adaptive: detect sortedness */
                            double unsorted_frac = detect_sortedness(mk_pool, keys, nrows);

                            if (unsorted_frac == 0.0) {
                                /* Already sorted */
                                sorted_idx = indices;
                                radix_done = true;
                            } else if (nrows <= RADIX_SORT_THRESHOLD) {
                                /* Small arrays - introsort */
                                key_introsort(keys, indices, nrows);
                                sorted_idx = indices;
                                radix_done = true;
                            } else {
                                /* Radix sort with type-aware pass count */
                                ray_t *ktmp_hdr, *itmp_hdr;
                                uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                                    (size_t)nrows * sizeof(uint64_t));
                                int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                                    (size_t)nrows * sizeof(int64_t));
                                if (ktmp && itmp) {
                                    sorted_idx = msd_radix_sort_run(mk_pool, keys, indices,
                                                                     ktmp, itmp, nrows,
                                                                     comp_nbytes, NULL);
                                    radix_done = (sorted_idx != NULL);
                                }
                                scratch_free(ktmp_hdr);
                                if (sorted_idx != itmp) scratch_free(itmp_hdr);
                                else radix_itmp_hdr = itmp_hdr;
                            }
                        }
                        scratch_free(keys_hdr);
                    }
                }
              } /* if (minmax) */
                scratch_free(minmax_hdr);
            }
            scratch_free(enum_ranks_hdr);
        }
    }

    /* --- Merge sort fallback ------------------------------------------------ */
    if (!radix_done) {
        if (!iota_done)
            for (int64_t i = 0; i < nrows; i++) indices[i] = i;
        /* Null = minimum value.
         * ASC → nulls first (nf=1), DESC → nulls last (nf=0). */
        ray_t* default_nf_hdr = NULL;
        if (!nulls_first) {
            uint8_t* default_nf = (uint8_t*)scratch_alloc(&default_nf_hdr, n_cols);
            if (!default_nf) {
                for (uint32_t k = 0; k < n_cols; k++)
                    scratch_free(enum_rank_hdrs[k]);
                scratch_free(enum_rank_hdrs_hdr);
                scratch_free(indices_hdr);
                return ray_error("oom", NULL);
            }
            for (uint32_t k = 0; k < n_cols; k++)
                default_nf[k] = descs ? !descs[k] : 1;
            nulls_first = default_nf;
        }
        sort_cmp_ctx_t cmp_ctx = {
            .vecs = cols,
            .desc = descs,
            .nulls_first = nulls_first,
            .n_sort = n_cols,
        };

        if (nrows <= 64) {
            sort_insertion(&cmp_ctx, indices, nrows);
        } else {
            ray_pool_t* pool = ray_pool_get();
            uint32_t n_workers = pool ? ray_pool_total_workers(pool) : 1;

            ray_t* tmp_hdr;
            int64_t* tmp = (int64_t*)scratch_alloc(&tmp_hdr,
                                (size_t)nrows * sizeof(int64_t));
            if (!tmp) {
                for (uint32_t k = 0; k < n_cols; k++)
                    scratch_free(enum_rank_hdrs[k]);
                scratch_free(enum_rank_hdrs_hdr);
                scratch_free(default_nf_hdr);
                scratch_free(indices_hdr);
                return ray_error("oom", NULL);
            }

            uint32_t n_chunks = n_workers;
            if (pool && n_chunks > 1 && nrows > 1024) {
                sort_phase1_ctx_t p1ctx = {
                    .cmp_ctx = &cmp_ctx, .indices = indices, .tmp = tmp,
                    .nrows = nrows, .n_chunks = n_chunks,
                };
                ray_pool_dispatch_n(pool, sort_phase1_fn, &p1ctx, n_chunks);
            } else {
                n_chunks = 1;
                sort_merge_recursive(&cmp_ctx, indices, tmp, nrows);
            }

            if (n_chunks > 1) {
                int64_t chunk_size = (nrows + n_chunks - 1) / n_chunks;
                int64_t run_size = chunk_size;
                int64_t* src = indices;
                int64_t* dst = tmp;

                while (run_size < nrows) {
                    int64_t n_pairs = (nrows + 2 * run_size - 1) / (2 * run_size);
                    sort_merge_ctx_t mctx = {
                        .cmp_ctx = &cmp_ctx, .src = src, .dst = dst,
                        .nrows = nrows, .run_size = run_size,
                    };
                    if (pool && n_pairs > 1)
                        ray_pool_dispatch_n(pool, sort_merge_fn, &mctx,
                                            (uint32_t)n_pairs);
                    else
                        sort_merge_fn(&mctx, 0, 0, n_pairs);
                    int64_t* t = src; src = dst; dst = t;
                    run_size *= 2;
                }

                if (src != indices)
                    memcpy(indices, src, (size_t)nrows * sizeof(int64_t));
            }

            scratch_free(tmp_hdr);
        }
        scratch_free(default_nf_hdr);
    }

str_msd_done:;
    /* If sorted_keys_out was requested but never set, null it out */
    if (sorted_keys_out && !*sorted_keys_out) {
        *sorted_keys_out = NULL;
        if (keys_hdr_out) *keys_hdr_out = NULL;
    }

    /* Build result I64 vector containing sorted indices */
    ray_t* result = ray_vec_new(RAY_I64, nrows);
    if (!result || RAY_IS_ERR(result)) {
        if (sorted_keys_out && *sorted_keys_out && keys_hdr_out)
            scratch_free(*keys_hdr_out);
        for (uint32_t k = 0; k < n_cols; k++)
            scratch_free(enum_rank_hdrs[k]);
        scratch_free(enum_rank_hdrs_hdr);
        scratch_free(radix_itmp_hdr);
        scratch_free(indices_hdr);
        return result ? result : ray_error("oom", NULL);
    }
    result->len = nrows;

    /* Copy final sorted indices into the result vector.
     * sorted_idx may point to indices or itmp - either way, copy out. */
    memcpy(ray_data(result), sorted_idx, (size_t)nrows * sizeof(int64_t));

    /* Free all scratch allocations */
    for (uint32_t k = 0; k < n_cols; k++)
        scratch_free(enum_rank_hdrs[k]);
    scratch_free(enum_rank_hdrs_hdr);
    scratch_free(radix_itmp_hdr);
    scratch_free(indices_hdr);
    return result;
}

/* Total-order compare for the comparator top-k: sort_cmp first, then the
 * original row index ascending as the tie-break.  Row indices are unique, so
 * this is a strict total order — the bounded heap evicts, and the final sort
 * orders, equal-key rows by LOWER original position.  That makes topk_indices_cmp
 * a STABLE top-k identical to the full-sort path (key_lt), rather than leaving
 * equal-key ties in arbitrary heap order (which both selected the wrong tied
 * rows at the k boundary and ordered survivors nondeterministically). */
static inline int topk_cmp_pos(const sort_cmp_ctx_t* ctx, int64_t a, int64_t b) {
    int c = sort_cmp(ctx, a, b);
    if (c != 0) return c;
    return (a > b) - (a < b);
}

static void topk_cmp_sift_down(const sort_cmp_ctx_t* ctx, int64_t* heap,
                               int64_t n, int64_t root) {
    for (;;) {
        int64_t worst = root;
        int64_t l = 2 * root + 1;
        int64_t r = 2 * root + 2;
        if (l < n && topk_cmp_pos(ctx, heap[l], heap[worst]) > 0) worst = l;
        if (r < n && topk_cmp_pos(ctx, heap[r], heap[worst]) > 0) worst = r;
        if (worst == root) break;
        int64_t tmp = heap[root];
        heap[root] = heap[worst];
        heap[worst] = tmp;
        root = worst;
    }
}

/* Comparator-based top-K: works for any comparable sort key types and any
 * number of keys (1..n).  Used when the radix-encoded fast path is not
 * applicable.  O(n log K + K log K). */
static ray_t* topk_indices_cmp(ray_t** cols, uint8_t* descs, uint8_t* nfs,
                               uint8_t n_cols, int64_t nrows, int64_t k) {
    if (!cols || n_cols == 0 || k <= 0 || nrows <= 0 || k >= nrows) return NULL;
    for (uint8_t c = 0; c < n_cols; c++) if (!cols[c]) return NULL;

    ray_t* idx = ray_vec_new(RAY_I64, k);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
    idx->len = k;
    int64_t* heap = (int64_t*)ray_data(idx);
    for (int64_t i = 0; i < k; i++) heap[i] = i;

    sort_cmp_ctx_t ctx = {
        .vecs = cols,
        .desc = descs,
        .nulls_first = nfs,
        .n_sort = n_cols,
    };

    for (int64_t i = k / 2 - 1; i >= 0; i--)
        topk_cmp_sift_down(&ctx, heap, k, i);

    for (int64_t i = k; i < nrows; i++) {
        if (sort_cmp(&ctx, i, heap[0]) >= 0) continue;
        heap[0] = i;
        topk_cmp_sift_down(&ctx, heap, k, 0);
    }

    for (int64_t i = 1; i < k; i++) {
        int64_t v = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && topk_cmp_pos(&ctx, v, heap[j]) < 0) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = v;
    }

    return idx;
}

static ray_t* topk_indices_cmp_single(ray_t* col, uint8_t desc, uint8_t nf,
                                      int64_t nrows, int64_t k) {
    ray_t* cols[1] = { col };
    uint8_t descs[1] = { desc };
    uint8_t nfs[1] = { nf };
    return topk_indices_cmp(cols, descs, nfs, 1, nrows, k);
}

/* --------------------------------------------------------------------------
 * Top-K bounded-heap selection on a single sort key.
 *
 * Replaces a full O(n log n) sort + take-K with O(n log K + K log K) when
 * K << n.  At plan time, the apply_sort_take / projection paths detect
 * "single sort key + small atom take" and call this in lieu of OP_SORT +
 * OP_HEAD.  Multi-key, take-range, or take-K-near-n cases keep the
 * existing fused sort+limit path (which is already O(n log n) bounded
 * with K-row gather).
 *
 * Implementation: encode each row's key to a uint64 (same encoding
 * radix_encode_fn uses, so smaller key = earlier in ASC order, with DESC
 * already pre-flipped).  Maintain a max-heap of K (key, original_idx)
 * pairs; for each row r > K, if r's encoded key is smaller than the
 * heap-top key, replace the top and sift down.  After the scan, sort
 * the K (key, idx) pairs by key ascending — the result is the top-K
 * indices in the user's requested order.
 *
 * Supported types: I64, I32, I16, U8, BOOL, F64, DATE, TIME,
 * TIMESTAMP, plus SYM, STR, and GUID via a comparator heap.  Returns
 * NULL on any unsupported configuration so the caller's fallback path
 * handles it.
 * -------------------------------------------------------------------------- */
/* Max-heap ordering for the radix bounded top-k: the "greater" (worse)
 * candidate has the larger encoded key, or an equal key with the LARGER
 * original row index.  Carrying the row index as the tie-break makes the heap
 * root the (key, position)-worst element, so evicting it keeps the (key,
 * position)-smallest k — a STABLE top-k that retains the lower original
 * position on ties, matching the full-sort path (key_lt) and key_heapsort's
 * final ordering.  Without the index tie-break the root among equal keys was
 * arbitrary and eviction could drop the row that should have been kept. */
#define TOPK_PAIR_GT(ka, ia, kb, ib) \
    ((ka) > (kb) || ((ka) == (kb) && (ia) > (ib)))

static ray_t* topk_indices_single(ray_t* col, uint8_t desc, uint8_t nf,
                                  int64_t nrows, int64_t k) {
    if (!col || k <= 0 || nrows <= 0) return NULL;
    if (k >= nrows) return NULL; /* full sort is at least as good */

    int8_t type = col->type;
    /* Whitelist of types where radix_encode_fn produces an order-preserving
     * uint64 — exactly the cases topk can handle without a comparator. */
    bool ok = (type == RAY_I64 || type == RAY_TIMESTAMP ||
               type == RAY_F64 || type == RAY_F32 ||
               type == RAY_I32 || type == RAY_DATE || type == RAY_TIME ||
               type == RAY_SYM || type == RAY_STR || type == RAY_GUID ||
               type == RAY_I16 || type == RAY_BOOL || type == RAY_U8);
    if (!ok) return NULL;

    if (type == RAY_SYM || type == RAY_STR || type == RAY_GUID)
        return topk_indices_cmp_single(col, desc, nf, nrows, k);

    /* Encode all rows to a single uint64 key array. */
    ray_t* keys_hdr = NULL;
    uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
        (size_t)nrows * sizeof(uint64_t));
    if (!keys) return NULL;

    radix_encode_ctx_t enc = {
        .keys        = keys,
        .indices     = NULL,
        .data        = ray_data(col),
        .col         = col,
        .type        = type,
        .col_attrs   = col->attrs,
        .desc        = desc != 0,
        .nulls_first = nf != 0,
        .enum_rank   = NULL,
        .n_keys      = 1,
    };
    /* Single-threaded encode is plenty for the heap pass that follows;
     * radix_encode_fn handles the type/desc/nulls dispatch correctly. */
    radix_encode_fn(&enc, 0, 0, nrows);

    /* Max-heap of K (key, idx) pairs.  Stored in two parallel arrays
     * for cache locality on the comparison path. */
    ray_t* hk_hdr = NULL;
    ray_t* hi_hdr = NULL;
    uint64_t* hk = (uint64_t*)scratch_alloc(&hk_hdr, (size_t)k * sizeof(uint64_t));
    int64_t*  hi = (int64_t*)scratch_alloc(&hi_hdr, (size_t)k * sizeof(int64_t));
    if (!hk || !hi) {
        if (hk_hdr) scratch_free(hk_hdr);
        if (hi_hdr) scratch_free(hi_hdr);
        scratch_free(keys_hdr);
        return NULL;
    }

    /* Seed with the first K rows. */
    for (int64_t i = 0; i < k; i++) { hk[i] = keys[i]; hi[i] = i; }

    /* Heapify (build max-heap on (hk,hi) pairs). */
    for (int64_t i = k / 2 - 1; i >= 0; i--) {
        int64_t idx = i;
        for (;;) {
            int64_t largest = idx;
            int64_t l = 2 * idx + 1, r = 2 * idx + 2;
            if (l < k && TOPK_PAIR_GT(hk[l], hi[l], hk[largest], hi[largest])) largest = l;
            if (r < k && TOPK_PAIR_GT(hk[r], hi[r], hk[largest], hi[largest])) largest = r;
            if (largest == idx) break;
            uint64_t tk = hk[idx]; hk[idx] = hk[largest]; hk[largest] = tk;
            int64_t  ti = hi[idx]; hi[idx] = hi[largest]; hi[largest] = ti;
            idx = largest;
        }
    }

    /* Scan remaining rows, push when the new key is strictly smaller than the
     * heap-top key.  Scan indices increase monotonically and every heap slot
     * holds an index below the current one, so an equal-key row is always the
     * higher position and correctly skipped; only a strictly-smaller key
     * displaces the (key,position)-worst root.  Sift the new root down. */
    for (int64_t i = k; i < nrows; i++) {
        if (keys[i] >= hk[0]) continue;
        hk[0] = keys[i];
        hi[0] = i;
        int64_t idx = 0;
        for (;;) {
            int64_t largest = idx;
            int64_t l = 2 * idx + 1, r = 2 * idx + 2;
            if (l < k && TOPK_PAIR_GT(hk[l], hi[l], hk[largest], hi[largest])) largest = l;
            if (r < k && TOPK_PAIR_GT(hk[r], hi[r], hk[largest], hi[largest])) largest = r;
            if (largest == idx) break;
            uint64_t tk = hk[idx]; hk[idx] = hk[largest]; hk[largest] = tk;
            int64_t  ti = hi[idx]; hi[idx] = hi[largest]; hi[largest] = ti;
            idx = largest;
        }
    }

    /* The heap contains the K best (smallest key) rows but unsorted.
     * Sort by key ascending so the gather order matches a full sort. */
    key_heapsort(hk, hi, k);

    /* Build the result I64 vec of indices. */
    ray_t* result = ray_vec_new(RAY_I64, k);
    if (!result || RAY_IS_ERR(result)) {
        scratch_free(hk_hdr); scratch_free(hi_hdr);
        scratch_free(keys_hdr);
        return result ? result : ray_error("oom", NULL);
    }
    result->len = k;
    memcpy(ray_data(result), hi, (size_t)k * sizeof(int64_t));

    scratch_free(hk_hdr); scratch_free(hi_hdr);
    scratch_free(keys_hdr);
    return result;
}
#undef TOPK_PAIR_GT

/* True when any column of `tbl` is a raw parted wrapper (RAY_IS_PARTED)
 * or the virtual partition column (RAY_MAPCOMMON).  The raw sort/top-k
 * builtins drive their whole-table gather off row indices in [0, nrows)
 * (nrows = the true parted row count, tens of millions), but a parted
 * wrapper's storage is an array of segment pointers sized to the SEGMENT
 * count (tens/hundreds) — a shape the gather is not built for and which,
 * once any path indexes ray_data(col) at a row position, reads out of
 * bounds.  Reject such tables up front with a clean, intentional error
 * instead of letting them fall through to an incidental type error deep in
 * the gather (or a future OOB).  The select-clause path sorts a
 * materialized result table, so it never trips this guard. */
static bool table_has_parted_col(ray_t* tbl) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON))
            return true;
    }
    return false;
}

/* Gather K rows of `tbl` at the given indices and return a new table.
 * Used by both single-key and multi-key top-K paths.  Releases `idx`. */
static ray_t* topk_gather_rows(ray_t* tbl, ray_t* idx, int64_t k) {
    int64_t* idx_data = (int64_t*)ray_data(idx);
    int64_t ncols = ray_table_ncols(tbl);

    ray_t* result = ray_table_new(ncols);
    if (!result || RAY_IS_ERR(result)) { ray_release(idx); return result; }
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        int64_t name = ray_table_col_name(tbl, c);
        if (!src) continue;
        ray_t* dst;
        if (src->type == RAY_LIST) {
            dst = ray_list_new(k);
            if (!dst || RAY_IS_ERR(dst)) {
                ray_release(idx); ray_release(result);
                return dst ? dst : ray_error("oom", NULL);
            }
            ray_t** sp = (ray_t**)ray_data(src);
            ray_t** dp = (ray_t**)ray_data(dst);
            for (int64_t i = 0; i < k; i++) {
                dp[i] = sp[idx_data[i]];
                if (dp[i]) ray_retain(dp[i]);
            }
            dst->len = k;
        } else {
            dst = gather_by_idx(src, idx_data, k);
            if (!dst || RAY_IS_ERR(dst)) {
                ray_release(idx); ray_release(result);
                return dst ? dst : ray_error("oom", NULL);
            }
        }
        result = ray_table_add_col(result, name, dst);
        ray_release(dst);
        if (RAY_IS_ERR(result)) { ray_release(idx); return result; }
    }
    ray_release(idx);
    return result;
}

/* Public top-K gather: returns a new table of `k` rows of `tbl`, sorted by
 * `col` in the requested direction.  When the inputs don't match the
 * single-key fast-path (multi-key, unsupported type, etc.), returns NULL
 * so the caller can fall back to the full-sort path. */
ray_t* ray_topk_table(ray_t* tbl, ray_t* col, uint8_t desc, uint8_t nf,
                      int64_t k) {
    if (!tbl || tbl->type != RAY_TABLE || !col) return NULL;
    if (table_has_parted_col(tbl))
        return ray_error("nyi", "top/bot: parted table input not supported");
    int64_t nrows = ray_table_nrows(tbl);
    if (k <= 0 || nrows <= 0) return NULL;
    if (k >= nrows) return NULL;
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (src && src->type == RAY_LIST) return NULL;
    }

    ray_t* idx = topk_indices_single(col, desc, nf, nrows, k);
    if (!idx) return NULL;
    return topk_gather_rows(tbl, idx, k);
}

/* Multi-key top-K: comparator-based bounded heap across `n_keys` columns.
 * Falls back to a comparator heap (no radix encoding) since multi-key
 * radix encoding requires uniform-width packed keys.  Returns NULL when
 * the inputs aren't supported (n_keys==0, K>=nrows, LIST columns) so the
 * caller can fall back to a full sort.  Cost is O(n_rows * n_keys * log K
 * + K log K) in comparisons — wins decisively when K << n_rows even with
 * the per-compare overhead.  All key columns must come from the same
 * table; row indices are interpreted into each column at the same
 * position. */
ray_t* ray_topk_table_multi(ray_t* tbl, ray_t** key_cols, uint8_t* descs,
                            uint8_t* nfs, uint8_t n_keys, int64_t k) {
    if (!tbl || tbl->type != RAY_TABLE || !key_cols || n_keys == 0) return NULL;
    if (table_has_parted_col(tbl))
        return ray_error("nyi", "top/bot: parted table input not supported");
    int64_t nrows = ray_table_nrows(tbl);
    if (k <= 0 || nrows <= 0 || k >= nrows) return NULL;
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (src && src->type == RAY_LIST) return NULL;
    }
    for (uint8_t i = 0; i < n_keys; i++)
        if (!key_cols[i] || key_cols[i]->len < nrows) return NULL;

    ray_t* idx = topk_indices_cmp(key_cols, descs, nfs, n_keys, nrows, k);
    if (!idx) return NULL;
    if (RAY_IS_ERR(idx)) return idx;
    return topk_gather_rows(tbl, idx, k);
}

/* (top vec n)  / (bot vec n) — partial-sort first N largest/smallest
 * elements of a numeric vector, returning a typed vec of the same
 * type as the input (or LIST/SYM passthrough via the comparator
 * heap).  O(N log K) when K << len via the same bounded-heap path
 * that ray_topk_table uses; falls back to full-sort + take when the
 * heap path declines (k >= len, unsupported types). */
ray_t* topk_take_vec(ray_t* v, int64_t k, uint8_t desc) {
    if (!v) return ray_error("type", "top/bot: expected a vector, got null");
    if (ray_is_lazy(v)) v = ray_lazy_materialize(v);
    if (!ray_is_vec(v)) return ray_error("type", "top/bot expects a vector");
    int64_t len = ray_len(v);
    if (k <= 0) return ray_vec_new(v->type, 0);

    /* k >= len → just full-sort the input.  Doesn't lose perf vs the
     * heap path (k log k bookkeeping) since the heap path needs the
     * full sort anyway when k == len. */
    if (k >= len) {
        return desc ? ray_desc_fn(v) : ray_asc_fn(v);
    }

    /* Try the bounded-heap fast path.  Default nulls-last for ASC,
     * nulls-first for DESC (matches sort defaults so the gathered
     * non-null prefix is always K elements when nulls fit). */
    uint8_t nf = desc ? 1 : 0;
    ray_t* idx = topk_indices_single(v, desc, nf, len, k);
    if (idx && !RAY_IS_ERR(idx)) {
        const int64_t* idata = (const int64_t*)ray_data(idx);
        ray_t* out = gather_by_idx(v, (int64_t*)idata, k);
        ray_release(idx);
        if (out && !RAY_IS_ERR(out)) return out;
    } else if (idx && RAY_IS_ERR(idx)) {
        return idx;
    }

    /* Fallback: full sort then take for unsupported inputs. */
    ray_t* sorted = desc ? ray_desc_fn(v) : ray_asc_fn(v);
    if (!sorted || RAY_IS_ERR(sorted)) return sorted;
    /* asc/desc on a concrete vector returns a LAZY handle, and
     * ray_take_fn materializes lazy args at entry — which CONSUMES the
     * handle (ray_lazy_materialize releases its input).  Materialize
     * here instead so take borrows a concrete vector and our release
     * below drops the ref we actually own; releasing the handle after
     * take had consumed it was a double free that resurfaced as
     * cascading stale releases once the slab recycled the block. */
    if (ray_is_lazy(sorted)) sorted = ray_lazy_materialize(sorted);
    if (!sorted || RAY_IS_ERR(sorted)) return sorted;
    ray_t* k_atom = ray_i64(k);
    ray_t* out = ray_take_fn(sorted, k_atom);
    ray_release(sorted);
    ray_release(k_atom);
    return out;
}

ray_t* ray_top_fn(ray_t* v, ray_t* n_obj) {
    if (!is_numeric(n_obj)) return ray_error("type", "top: n must be integer");
    return topk_take_vec(v, as_i64(n_obj), /*desc=*/1);
}

ray_t* ray_bot_fn(ray_t* v, ray_t* n_obj) {
    if (!is_numeric(n_obj)) return ray_error("type", "bot: n must be integer");
    return topk_take_vec(v, as_i64(n_obj), /*desc=*/0);
}

ray_t* ray_sort_indices(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                        uint8_t n_cols, int64_t nrows) {
    return sort_indices_ex(cols, descs, nulls_first, n_cols, nrows, NULL, NULL);
}

ray_t* ray_sort(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                uint8_t n_cols, int64_t nrows) {
    if (n_cols == 1) {
        uint64_t* sorted_keys = NULL;
        ray_t* keys_hdr = NULL;
        ray_t* idx = sort_indices_ex(cols, descs, nulls_first, 1, nrows,
                                      &sorted_keys, &keys_hdr);
        if (!idx || RAY_IS_ERR(idx)) return idx;

        bool c0_shifted = (cols[0]->attrs & RAY_ATTR_HAS_NULLS) &&
                          (cols[0]->type == RAY_BOOL ||
                           cols[0]->type == RAY_U8 ||
                           cols[0]->type == RAY_I16);
        if (sorted_keys && !RAY_IS_SYM(cols[0]->type) && !c0_shifted) {
            /* Decode path: sequential writes, no random access */
            ray_t* result = ray_vec_new(cols[0]->type, nrows);
            if (!result || RAY_IS_ERR(result)) {
                ray_release(idx);
                if (keys_hdr) scratch_free(keys_hdr);
                return result ? result : ray_error("oom", NULL);
            }
            result->len = nrows;
            radix_decode_into(ray_data(result), cols[0]->type, sorted_keys,
                              nrows, descs ? descs[0] : 0);
            /* Propagate null bitmap using sorted indices */
            if (cols[0]->attrs & RAY_ATTR_HAS_NULLS) {
                int64_t* idx_data = (int64_t*)ray_data(idx);
                for (int64_t i = 0; i < nrows; i++)
                    if (ray_vec_is_null(cols[0], idx_data[i]))
                        ray_vec_set_null(result, i, true);
            }
            ray_release(idx);
            scratch_free(keys_hdr);
            return result;
        }

        /* Fallback: gather by index */
        if (keys_hdr) scratch_free(keys_hdr);
        ray_t* result = gather_by_idx(cols[0], (int64_t*)ray_data(idx), nrows);
        ray_release(idx);
        return result;
    }

    /* Multi-column: index sort + gather (decode only helps single-key) */
    ray_t* idx = ray_sort_indices(cols, descs, nulls_first, n_cols, nrows);
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_t* result = gather_by_idx(cols[0], (int64_t*)ray_data(idx), nrows);
    ray_release(idx);
    return result;
}

ray_t* exec_sort(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t limit) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols > 4096) return ray_error("nyi", NULL); /* stack safety */
    uint32_t n_sort = ext->sort.n_cols;

    /* ---- Top-K bounded-heap shortcut ----
     * Triggered by the SORT+HEAD fusion (HEAD passes limit > 0).  When
     * K is well below nrows (K << n) and every sort key is a direct
     * OP_SCAN of a column on `tbl`, run a heap-based partial selection
     * in O(n log K + K log K) instead of the full O(n log n) sort.
     * Single key → radix-encoded fast path; multi-key → comparator
     * heap (still O(n log K) in compares, big win when K << n).
     * Falls through to the full sort whenever the topk path returns
     * NULL (unsupported type, computed-key sort, etc.).  Bounded to
     * n_sort <= 16: the shortcut's key_cols/nfs scratch below is a
     * fixed-16 stack array; wider sorts skip straight to the general
     * sort_indices_ex path (itself n_cols-unbounded). */
    if (limit > 0 && n_sort >= 1 && n_sort <= 16 && limit < nrows && limit <= 8192 &&
        g && g->selection == NULL) {
        ray_t* key_cols[16];
        int    all_scan = 1;
        for (uint32_t k = 0; k < n_sort; k++) {
            ray_op_t* key_op = op_node(g, ext->sort.columns[k]);
            ray_op_ext_t* key_ext = find_ext(g, key_op->id);
            if (key_ext && key_ext->base.opcode == OP_SCAN) {
                key_cols[k] = ray_table_get_col(tbl, key_ext->sym);
                if (!key_cols[k]) { all_scan = 0; break; }
            } else {
                all_scan = 0;
                break;
            }
        }
        if (all_scan) {
            if (n_sort == 1) {
                uint8_t desc = ext->sort.desc ? ext->sort.desc[0] : 0;
                uint8_t nf   = ext->sort.nulls_first
                              ? ext->sort.nulls_first[0]
                              : !desc;
                ray_t* topk_res = ray_topk_table(tbl, key_cols[0], desc, nf, limit);
                if (topk_res && !RAY_IS_ERR(topk_res)) return topk_res;
                if (topk_res && RAY_IS_ERR(topk_res)) ray_release(topk_res);
            } else {
                /* Default nulls-first to !desc per-key when caller
                 * didn't supply a vector. */
                uint8_t nfs[16];
                for (uint32_t k = 0; k < n_sort; k++) {
                    uint8_t d = ext->sort.desc ? ext->sort.desc[k] : 0;
                    nfs[k] = ext->sort.nulls_first
                             ? ext->sort.nulls_first[k]
                             : !d;
                }
                ray_t* topk_res = ray_topk_table_multi(tbl, key_cols,
                    ext->sort.desc, nfs, n_sort, limit);
                if (topk_res && !RAY_IS_ERR(topk_res)) return topk_res;
                if (topk_res && RAY_IS_ERR(topk_res)) ray_release(topk_res);
            }
            /* topk_res == NULL → unsupported config, fall through. */
        }
    }

    /* Resolve sort key vectors.  n_sort is the (unbounded) widened sort
     * key count post-migration — carve the key-vector/owned-flag scratch
     * from the heap instead of two stack VLAs (stack-blow risk on worker
     * threads for wide sorts).  Single consolidated allocation: sort_vecs
     * (8-byte entries) first, sort_owned (1-byte entries) immediately
     * after; scratch_calloc zero-inits both, matching the prior memsets. */
    uint32_t n_sort_alloc = n_sort > 0 ? n_sort : 1;
    size_t sort_vecs_bytes = (size_t)n_sort_alloc * sizeof(ray_t*);
    ray_t* sort_hdr;
    char* sort_scratch = (char*)scratch_calloc(&sort_hdr,
        sort_vecs_bytes + (size_t)n_sort_alloc * sizeof(uint8_t));
    if (!sort_scratch) return ray_error("oom", NULL);
    ray_t** sort_vecs = (ray_t**)sort_scratch;
    uint8_t* sort_owned = (uint8_t*)(sort_scratch + sort_vecs_bytes);

    for (uint32_t k = 0; k < n_sort; k++) {
        ray_op_t* key_op = op_node(g, ext->sort.columns[k]);
        ray_op_ext_t* key_ext = find_ext(g, key_op->id);
        if (key_ext && key_ext->base.opcode == OP_SCAN) {
            sort_vecs[k] = ray_table_get_col(tbl, key_ext->sym);
        } else {
            ray_t* saved = g->table;
            g->table = tbl;
            sort_vecs[k] = exec_node(g, key_op);
            g->table = saved;
            sort_owned[k] = 1;
        }
        if (!sort_vecs[k] || RAY_IS_ERR(sort_vecs[k])) {
            ray_t* err = sort_vecs[k] ? sort_vecs[k] : ray_error("nyi", NULL);
            for (uint32_t j = 0; j < k; j++) {
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            }
            scratch_free(sort_hdr);
            return err;
        }
    }

    /* ---- Sort-index permutation reuse ----
     * When there is exactly one ascending sort key that is a direct column
     * scan and that column carries a fresh, null-free sort index, reuse the
     * pre-built ascending permutation instead of recomputing it.
     *
     * Restricted to ascending-only: reversing the asc perm for DESC would
     * swap tie-group positions (stable DESC sort keeps ties in iota order;
     * reversed ASC perm would put them in reverse-iota order — a visible
     * difference that would fail the differential).
     *
     * g->selection is always NULL here (exec.c compacts it before calling
     * exec_sort); the gate still checks defensively.
     *
     * FUTURE TRAP: lifting the null-free restriction (idx_fresh_nonull)
     * requires reconciling nulls placement — the index perm was built with
     * sort_indices_ex's default (nulls FIRST), while ray_sort_op defaults
     * to nulls LAST for ASC.  Reusing the perm for a null-bearing column
     * without that reconciliation silently misplaces the null block. */
    uint64_t* sorted_keys = NULL;
    ray_t* sorted_keys_hdr = NULL;
    ray_t* idx_vec;
    if (n_sort == 1 &&
        (!ext->sort.desc || !ext->sort.desc[0]) &&
        (!g || !g->selection) &&
        !sort_owned[0] && sort_vecs[0]) {
        ray_t* perm = ray_index_sort_perm_fresh(sort_vecs[0]);
        if (perm) {
            /* Index eligible and fresh: consult + hit. */
            ray_idx_consults[IDX_SITE_SORT]++;
            ray_idx_hits[IDX_SITE_SORT]++;
            ray_retain(perm);
            idx_vec = perm;
            goto sort_idx_ready;
        }
    }
    idx_vec = sort_indices_ex(sort_vecs, ext->sort.desc,
                              ext->sort.nulls_first, n_sort, nrows,
                              &sorted_keys, &sorted_keys_hdr);
sort_idx_ready:;
    if (!idx_vec || RAY_IS_ERR(idx_vec)) {
        if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
        for (uint32_t k = 0; k < n_sort; k++) {
            if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                ray_release(sort_vecs[k]);
        }
        scratch_free(sort_hdr);
        return idx_vec ? idx_vec : ray_error("oom", NULL);
    }
    int64_t* sorted_idx = (int64_t*)ray_data(idx_vec);

    /* Check cancellation before expensive gather phase */
    {
        ray_pool_t* cp = ray_pool_get();
        if (pool_cancelled(cp)) {
            if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
            for (uint32_t k = 0; k < n_sort; k++) {
                if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                    ray_release(sort_vecs[k]);
            }
            scratch_free(sort_hdr);
            ray_release(idx_vec);
            return ray_error("cancel", NULL);
        }
    }

    /* Materialize sorted result - fused multi-column gather.
     * When limit > 0, only gather the first `limit` rows (SORT+LIMIT fusion). */
    int64_t gather_rows = nrows;
    if (limit > 0 && limit < nrows) gather_rows = limit;

    ray_t* result = ray_table_new(ncols);
    if (!result || RAY_IS_ERR(result)) {
        if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
        for (uint32_t k = 0; k < n_sort; k++) {
            if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                ray_release(sort_vecs[k]);
        }
        scratch_free(sort_hdr);
        ray_release(idx_vec);
        return result;
    }

    /* Pre-allocate all output columns, then do a single fused gather pass */
    ray_pool_t* gather_pool = (gather_rows > RAY_PARALLEL_THRESHOLD) ? ray_pool_get() : NULL;
    ray_t* new_cols[ncols];
    int64_t col_names[ncols];
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        col_names[c] = ray_table_col_name(tbl, c);
        if (!col) { new_cols[c] = NULL; continue; }
        ray_t* nc;
        if (col->type == RAY_LIST) {
            /* LIST: element-wise gather with retain (not memcpy-safe) */
            nc = ray_list_new(gather_rows);
        } else {
            nc = col_vec_new(col, gather_rows);
            /* the gather below raw-copies SYM cell ids — the output
             * resolves over the source's dictionary (sym-domain Phase 2) */
            if (nc && !RAY_IS_ERR(nc) && col->type == RAY_SYM)
                ray_sym_vec_adopt_domain(nc, col);
        }
        if (!nc || RAY_IS_ERR(nc)) {
            for (int64_t j = 0; j < c; j++)
                if (new_cols[j]) ray_release(new_cols[j]);
            ray_release(result);
            if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
            for (uint32_t k = 0; k < n_sort; k++)
                if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                    ray_release(sort_vecs[k]);
            scratch_free(sort_hdr);
            ray_release(idx_vec);
            return nc ? nc : ray_error("oom", NULL);
        }
        if (col->type == RAY_LIST) {
            ray_t** src_ptrs = (ray_t**)ray_data(col);
            ray_t** dst_ptrs = (ray_t**)ray_data(nc);
            for (int64_t r = 0; r < gather_rows; r++) {
                dst_ptrs[r] = src_ptrs[sorted_idx[r]];
                if (dst_ptrs[r]) ray_retain(dst_ptrs[r]);
            }
        }
        nc->len = gather_rows;
        new_cols[c] = nc;
    }

    /* Decode-gather optimisation: decode the sort key column directly from
     * sorted radix keys (sequential writes) instead of random-access gather.
     * Only for single-key, non-SYM sorts where radix keys are available.
     * Narrow-int + has_nulls uses a +1-shifted encoding that radix_decode_into
     * doesn't invert, so fall back to gather there. */
    int8_t sk0_type = sort_vecs[0] ? sort_vecs[0]->type : 0;
    bool sk0_shifted = sort_vecs[0] &&
                       (sort_vecs[0]->attrs & RAY_ATTR_HAS_NULLS) &&
                       (sk0_type == RAY_BOOL || sk0_type == RAY_U8 ||
                        sk0_type == RAY_I16);
    int64_t sort_key_sym = -1;
    if (sorted_keys && n_sort == 1 && !RAY_IS_SYM(sk0_type) && !sk0_shifted) {
        ray_op_ext_t* key_ext = find_ext(g, ext->sort.columns[0]);
        if (key_ext && key_ext->base.opcode == OP_SCAN)
            sort_key_sym = key_ext->sym;
    }
    int64_t decode_col_idx = -1;
    if (sort_key_sym >= 0) {
        for (int64_t c = 0; c < ncols; c++) {
            if (col_names[c] == sort_key_sym && new_cols[c]) {
                decode_col_idx = c;
                break;
            }
        }
    }

    if (decode_col_idx >= 0) {
        radix_decode_into(ray_data(new_cols[decode_col_idx]),
                          sort_vecs[0]->type, sorted_keys,
                          gather_rows, ext->sort.desc ? ext->sort.desc[0] : 0);
    }

    /* Gather all columns using sorted indices, in batches of MGATHER_MAX_COLS.
     * LIST columns are skipped here — they were gathered with retain above. */
    for (int64_t base = 0; base < ncols; ) {
        char*   g_srcs[MGATHER_MAX_COLS];
        char*   g_dsts[MGATHER_MAX_COLS];
        uint8_t g_esz[MGATHER_MAX_COLS];
        int64_t g_nc = 0;
        for (; base < ncols && g_nc < MGATHER_MAX_COLS; base++) {
            if (!new_cols[base] || base == decode_col_idx) continue;
            ray_t* col = ray_table_get_col_idx(tbl, base);
            if (col->type == RAY_LIST) continue;
            g_srcs[g_nc] = (char*)ray_data(col);
            g_dsts[g_nc] = (char*)ray_data(new_cols[base]);
            g_esz[g_nc]  = col_esz(col);
            g_nc++;
        }
        if (g_nc == 0) continue;
        if (n_sort == 1)
            partitioned_gather(gather_pool, sorted_idx, gather_rows,
                               nrows, g_srcs, g_dsts, g_esz, g_nc);
        else {
            multi_gather_ctx_t mg = { .idx = sorted_idx, .ncols = g_nc };
            for (int64_t i = 0; i < g_nc; i++) {
                mg.srcs[i] = g_srcs[i];
                mg.dsts[i] = g_dsts[i];
                mg.esz[i]  = g_esz[i];
            }
            if (gather_pool)
                ray_pool_dispatch(gather_pool, multi_gather_fn, &mg,
                                 gather_rows);
            else
                multi_gather_fn(&mg, 0, 0, gather_rows);
        }
    }

    /* Propagate str_pool / null bitmaps from source columns */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        col_propagate_str_pool(new_cols[c], col);
        /* Gather null bits in sorted order */
        bool src_has_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) ||
                             ((col->attrs & RAY_ATTR_SLICE) && col->slice_parent &&
                              (col->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
        if (src_has_nulls) {
            for (int64_t r = 0; r < gather_rows; r++)
                if (ray_vec_is_null(col, sorted_idx[r]))
                    ray_vec_set_null(new_cols[c], r, true);
        }
    }

    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        result = ray_table_add_col(result, col_names[c], new_cols[c]);
        ray_release(new_cols[c]);
    }

    /* Free sorted radix keys scratch buffer */
    if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);

    /* Free expression-evaluated sort keys */
    for (uint32_t k = 0; k < n_sort; k++) {
        if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
            ray_release(sort_vecs[k]);
    }
    scratch_free(sort_hdr);

    ray_release(idx_vec);
    return result;
}

/* ── Builtins ── */

/* Shared eager kernel — called by the OP_ASC executor. */
ray_t* asc_vec_eager(ray_t* x) {
    int64_t n = ray_len(x);
    uint8_t desc = 0;
    return ray_sort(&x, &desc, NULL, 1, n);
}

/* (asc v) — sort vector ascending */
ray_t* ray_asc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;

    /* Extend an existing chain. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_ASC);

    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "asc expects a vector");
    int64_t n = ray_len(x);
    if (n <= 1) { ray_retain(x); return x; }

    /* Concrete vector: start a fresh chain. */
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = ray_asc_op(g, in);
    return ray_lazy_wrap(g, op);
}

/* Shared eager kernel — called by the OP_DESC executor. */
ray_t* desc_vec_eager(ray_t* x) {
    int64_t n = ray_len(x);
    uint8_t desc = 1;
    return ray_sort(&x, &desc, NULL, 1, n);
}

/* (desc v) — sort vector descending */
ray_t* ray_desc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;

    /* Extend an existing chain. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_DESC);

    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "desc expects a vector");
    int64_t n = ray_len(x);
    if (n <= 1) { ray_retain(x); return x; }

    /* Concrete vector: start a fresh chain. */
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = ray_desc_op(g, in);
    return ray_lazy_wrap(g, op);
}

/* (iasc v) — ascending sort indices */
ray_t* ray_iasc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "iasc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 0;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (idesc v) — descending sort indices */
ray_t* ray_idesc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "idesc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 1;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (rank v) — rank positions (inverse permutation of iasc) */
ray_t* ray_rank_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "rank expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 0;
    ray_t* idx = ray_sort_indices(&x, &desc, NULL, 1, n);
    if (RAY_IS_ERR(idx)) return idx;

    ray_t* result = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(result)) { ray_release(idx); return result; }
    result->len = n;

    int64_t* idx_data = (int64_t*)ray_data(idx);
    int64_t* rank_data = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < n; i++)
        rank_data[idx_data[i]] = i;

    ray_release(idx);
    return result;
}

/* Pool worker for the already-in-order detection scan: each task verifies
 * consecutive-row order over [start,end) INCLUDING the (start-1,start)
 * boundary pair, so chunk edges are covered.  The shared flag only ever
 * transitions 1 -> 0 (benign race) and doubles as a bail signal. */
typedef struct {
    ray_t**  key_cols;
    int64_t  n_keys;
    uint8_t  descending;
    volatile int* ordered;
} sorted_check_ctx_t;

static void sorted_check_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    sorted_check_ctx_t* c = (sorted_check_ctx_t*)raw;
    if (start < 1) start = 1;

    /* Specialized (SYM, i64-family) two-key loop — the canonical
     * (sym, time) shape.  The generic loop below pays a type-switch per
     * read and a volatile flag load per row; here adjacent sym ids
     * compare directly (equal ids on ~all rows of a sorted table — the
     * string comparison only runs at run boundaries), the secondary key
     * reads raw i64, and the bail flag is polled per 4096-row block. */
    if (!c->descending && c->n_keys == 2 &&
        c->key_cols[0]->type == RAY_SYM &&
        (c->key_cols[1]->type == RAY_I64 ||
         c->key_cols[1]->type == RAY_TIME ||
         c->key_cols[1]->type == RAY_TIMESTAMP) &&
        !(c->key_cols[1]->attrs & RAY_ATTR_HAS_NULLS)) {
        ray_t* sc = c->key_cols[0];
        const void* sd = ray_data(sc);
        uint8_t sattrs = sc->attrs;
        const int64_t* restrict tv = (const int64_t*)ray_data(c->key_cols[1]);
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(sc);
        for (int64_t b = start; b < end; b += 4096) {
            if (!*c->ordered) return;
            int64_t e = b + 4096 < end ? b + 4096 : end;
            for (int64_t i = b; i < e; i++) {
                int64_t ia = ray_read_sym(sd, i - 1, RAY_SYM, sattrs);
                int64_t ib = ray_read_sym(sd, i,     RAY_SYM, sattrs);
                if (RAY_LIKELY(ia == ib)) {
                    if (tv[i] < tv[i - 1]) { *c->ordered = 0; return; }
                } else {
                    ray_t* sa = ray_sym_domain_str(dom, ia);
                    ray_t* sb = ray_sym_domain_str(dom, ib);
                    uint32_t la = sa ? (uint32_t)ray_str_len(sa) : 0;
                    uint32_t lb = sb ? (uint32_t)ray_str_len(sb) : 0;
                    uint32_t ml = la < lb ? la : lb;
                    int cmp = ml ? memcmp(ray_str_ptr(sa), ray_str_ptr(sb), ml) : 0;
                    if (cmp == 0) cmp = (la > lb) - (la < lb);
                    if (cmp > 0) { *c->ordered = 0; return; }
                }
            }
        }
        return;
    }

    for (int64_t i = start; i < end; i++) {
        if (!*c->ordered) return;   /* another task already found a violation */
        int cmp = 0;
        for (int64_t k = 0; k < c->n_keys && cmp == 0; k++) {
            ray_t* col = c->key_cols[k];
            if (col->type == RAY_SYM) {
                int64_t ia = ray_read_sym(ray_data(col), i - 1, RAY_SYM, col->attrs);
                int64_t ib = ray_read_sym(ray_data(col), i,     RAY_SYM, col->attrs);
                if (ia != ib) {
                    struct ray_sym_domain_s* dom = ray_sym_vec_domain(col);
                    ray_t* sa = ray_sym_domain_str(dom, ia);
                    ray_t* sb = ray_sym_domain_str(dom, ib);
                    uint32_t la = sa ? (uint32_t)ray_str_len(sa) : 0;
                    uint32_t lb = sb ? (uint32_t)ray_str_len(sb) : 0;
                    uint32_t ml = la < lb ? la : lb;
                    if (ml > 0) cmp = memcmp(ray_str_ptr(sa), ray_str_ptr(sb), ml);
                    if (cmp == 0) cmp = (la > lb) - (la < lb);
                }
            } else {
                int64_t va = read_col_i64(ray_data(col), i - 1, col->type, col->attrs);
                int64_t vb = read_col_i64(ray_data(col), i,     col->type, col->attrs);
                cmp = (va > vb) - (va < vb);
            }
        }
        if (c->descending ? (cmp < 0) : (cmp > 0)) { *c->ordered = 0; return; }
    }
}

/* Public: are these key columns already in sorted order under the sort's
 * lexicographic ordering (single uniform direction across all keys)?  Only
 * null-free integer-family / SYM keys are decidable — anything else (float,
 * STR/LIST, or a HAS_NULLS column) returns false so the caller must NOT
 * assume sortedness (float NaN / null placement belong to the real sort).
 * O(nrows) with early bail on the first violation; a 0/1-row column is
 * trivially sorted.  Used by the parted ORDER BY streaming path in query.c
 * to verify each partition is internally sorted at O(partition), never a
 * full sort. */
bool ray_key_cols_sorted(ray_t** key_cols, int64_t n_keys, uint8_t descending,
                         int64_t nrows) {
    if (n_keys < 1) return true;
    /* Decidability gate runs UNCONDITIONALLY — a 0/1-row segment is trivially
     * "sorted", but an undecidable key type (float / STR / GUID) or a
     * null-bearing key must still return false so the caller declines rather
     * than silently treating an unorderable column as ordered. */
    for (int64_t k = 0; k < n_keys; k++) {
        int8_t t = key_cols[k]->type;
        if (key_cols[k]->attrs & RAY_ATTR_HAS_NULLS) return false;
        if (t != RAY_BOOL && t != RAY_U8 && t != RAY_I16 && t != RAY_I32 &&
            t != RAY_I64 && t != RAY_DATE && t != RAY_TIME &&
            t != RAY_TIMESTAMP && t != RAY_SYM)
            return false;
    }
    if (nrows < 2) return true;
    volatile int ordered = 1;
    sorted_check_ctx_t sctx = {
        .key_cols = key_cols, .n_keys = n_keys,
        .descending = descending, .ordered = &ordered,
    };
    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, sorted_check_fn, &sctx, nrows);
    else
        sorted_check_fn(&sctx, 0, 1, nrows);
    return ordered != 0;
}

static bool sort_part_key_type(int8_t t) {
    return t == RAY_SYM ||
           t == RAY_BOOL || t == RAY_U8  || t == RAY_I16 ||
           t == RAY_I32  || t == RAY_I64 || t == RAY_DATE ||
           t == RAY_TIME || t == RAY_TIMESTAMP;
}

static void sort_note_part_order(ray_t* col, int64_t order_sym) {
    if (!col || RAY_IS_ERR(col) || ray_index_kind(col) != RAY_IDX_PART)
        return;
    ray_index_payload(col->index)->u.part.order_sym = order_sym;
}

static ray_t* sort_stamp_part_col_owned(ray_t* col, int64_t order_sym) {
    if (!col || RAY_IS_ERR(col) || (col->attrs & RAY_ATTR_HAS_NULLS) ||
        !sort_part_key_type(col->type))
        return NULL;
    ray_t* w = col;
    ray_t* r = ray_index_attach_part(&w);
    if (!r || RAY_IS_ERR(r)) {
        if (r && RAY_IS_ERR(r)) ray_release(r);
        return NULL;
    }
    sort_note_part_order(w, order_sym);
    return w;
}

static ray_t* sort_stamp_ordered_table(ray_t* tbl, int64_t key_id,
                                       ray_t* key_col, int64_t order_sym) {
    if (!key_col || (key_col->attrs & RAY_ATTR_HAS_NULLS) ||
        !sort_part_key_type(key_col->type)) {
        ray_retain(tbl);
        return tbl;
    }

    ray_t* stamped = key_col;
    ray_retain(stamped);
    ray_t* r = ray_index_attach_part(&stamped);
    if (!r || RAY_IS_ERR(r)) {
        if (r && RAY_IS_ERR(r)) ray_release(r);
        ray_release(stamped);
        ray_retain(tbl);
        return tbl;
    }
    sort_note_part_order(stamped, order_sym);

    int64_t ncols = ray_table_ncols(tbl);
    ray_t* out = ray_table_new(ncols);
    if (!out || RAY_IS_ERR(out)) {
        ray_release(stamped);
        return out ? out : ray_error("oom", NULL);
    }
    for (int64_t c = 0; c < ncols; c++) {
        int64_t cn = ray_table_col_name(tbl, c);
        ray_t* col = (cn == key_id) ? stamped : ray_table_get_col_idx(tbl, c);
        out = ray_table_add_col(out, cn, col);
        if (!out || RAY_IS_ERR(out)) {
            ray_release(stamped);
            return out ? out : ray_error("oom", NULL);
        }
    }
    ray_release(stamped);
    return out;
}

/* Helper: resolve key symbols to table columns for xasc/xdesc */
ray_t* sort_table_by_keys(ray_t* tbl, ray_t* keys, uint8_t descending) {
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", "xasc/xdesc expects a table as first argument");
    if (table_has_parted_col(tbl))
        return ray_error("nyi", "xasc/xdesc: parted table input not supported");

    /* keys can be a SYM atom, a SYM vector, or a list of SYM atoms */
    int64_t n_keys;
    if (keys->type == -RAY_SYM) {
        n_keys = 1;
    } else if (keys->type == RAY_SYM) {
        n_keys = ray_len(keys);
    } else if (is_list(keys)) {
        n_keys = ray_len(keys);
    } else {
        return ray_error("type", "xasc/xdesc key must be a symbol or list of symbols");
    }

    if (n_keys == 0) { ray_retain(tbl); return tbl; }

    /* Exact-size carve for the key ids, resolved key columns and per-key
     * descending flags: one block, the two 8-byte arrays (key_ids,
     * key_cols) followed by the descs bytes.  Unbounded key count; freed
     * at every exit below. */
    ray_t* k_hdr;
    int64_t* key_ids = (int64_t*)scratch_alloc(&k_hdr,
        (size_t)n_keys * (sizeof(int64_t) + sizeof(ray_t*) + sizeof(uint8_t)));
    if (!key_ids) return ray_error("oom", NULL);
    ray_t** key_cols = (ray_t**)(key_ids + n_keys);
    uint8_t* descs   = (uint8_t*)(key_cols + n_keys);

    if (keys->type == -RAY_SYM) {
        /* Single symbol atom */
        key_ids[0] = keys->i64;
    } else if (keys->type == RAY_SYM) {
        /* SYM vector */
        int64_t* syms = (int64_t*)ray_data(keys);
        for (int64_t i = 0; i < n_keys; i++) key_ids[i] = syms[i];
    } else {
        /* List of symbol atoms */
        ray_t** elems = (ray_t**)ray_data(keys);
        for (int64_t i = 0; i < n_keys; i++) {
            if (elems[i]->type != -RAY_SYM) {
                scratch_free(k_hdr);
                return ray_error("type", "xasc/xdesc key must be a symbol");
            }
            key_ids[i] = elems[i]->i64;
        }
    }

    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 1) { scratch_free(k_hdr); ray_retain(tbl); return tbl; }

    /* Resolve key columns */
    for (int64_t i = 0; i < n_keys; i++) {
        key_cols[i] = ray_table_get_col(tbl, key_ids[i]);
        if (!key_cols[i]) {
            scratch_free(k_hdr);
            return ray_error("domain", "xasc/xdesc: key column not found in table");
        }
    }

    /* Build descs array */
    for (int64_t i = 0; i < n_keys; i++) descs[i] = descending;

    /* Already-in-order detection: one bail-early pass comparing consecutive
     * rows with EXACTLY the sort's ordering — integer family raw, SYM
     * lexicographic through the column's domain (build_enum_rank's
     * comparator: memcmp over the common prefix, then length).  A table
     * that already sits in the requested order — e.g. an in-query
     * (xasc t ['sym 'time]) re-asserting the store's layout — returns the
     * input retained: the sort, the whole-table gather and the null/attr
     * propagation all vanish.  Restricted to null-free integer-family and
     * SYM keys: null placement and float NaN ordering belong to the real
     * sort.  An unsorted input bails at its first violation, so the scan
     * costs a few comparisons, not O(n). */
    {
        bool detectable = true;
        for (int64_t k = 0; k < n_keys && detectable; k++) {
            int8_t t = key_cols[k]->type;
            if (key_cols[k]->attrs & RAY_ATTR_HAS_NULLS) detectable = false;
            else if (t != RAY_BOOL && t != RAY_U8 && t != RAY_I16 &&
                     t != RAY_I32 && t != RAY_I64 && t != RAY_DATE &&
                     t != RAY_TIME && t != RAY_TIMESTAMP && t != RAY_SYM)
                detectable = false;
        }
        if (detectable) {
            volatile int ordered = 1;
            sorted_check_ctx_t sctx = {
                .key_cols = key_cols, .n_keys = n_keys,
                .descending = descending, .ordered = &ordered,
            };
            ray_pool_t* pool = ray_pool_get();
            if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, sorted_check_fn, &sctx, nrows);
            else
                sorted_check_fn(&sctx, 0, 1, nrows);
            if (ordered) {
                ray_t* out = (descending || n_keys <= 1) ? NULL
                    : sort_stamp_ordered_table(tbl, key_ids[0], key_cols[0],
                                               n_keys > 1 ? key_ids[1] : -1);
                scratch_free(k_hdr);
                if (out) return out;
                ray_retain(tbl);
                return tbl;
            }
        }
    }

    uint64_t* sorted_keys = NULL;
    ray_t* sorted_keys_hdr = NULL;
    ray_t* idx = sort_indices_ex(key_cols, descs, NULL, n_keys, nrows,
                                 &sorted_keys, &sorted_keys_hdr);
    if (RAY_IS_ERR(idx)) {
        if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
        scratch_free(k_hdr);
        return idx;
    }

    int64_t* idx_data = (int64_t*)ray_data(idx);
    int64_t ncols = ray_table_ncols(tbl);

    /* Pre-allocate all output columns, then do a parallel multi-column
     * gather — same fast path exec_sort uses.  LIST columns are gathered
     * element-wise with retain; all other columns go through the
     * partitioned_gather / multi_gather_fn paths.  Null bits and
     * str_pool are propagated after the gather runs.
     *
     * Heap-allocate the per-column scratch arrays so the fast path
     * handles arbitrarily wide tables — avoids a VLA stack blow-up
     * and matches the pre-regression xasc behavior which supported
     * any column count via gather_by_idx. */
    ray_pool_t* gather_pool = (nrows > RAY_PARALLEL_THRESHOLD)
                              ? ray_pool_get() : NULL;

    ray_t* nc_hdr = NULL;
    ray_t** new_cols = (ray_t**)scratch_alloc(&nc_hdr,
                             (size_t)ncols * sizeof(ray_t*));
    ray_t* cn_hdr = NULL;
    int64_t* col_names = (int64_t*)scratch_alloc(&cn_hdr,
                             (size_t)ncols * sizeof(int64_t));
    if (!new_cols || !col_names) {
        if (nc_hdr) scratch_free(nc_hdr);
        if (cn_hdr) scratch_free(cn_hdr);
        if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
        scratch_free(k_hdr);
        ray_release(idx);
        return ray_error("oom", NULL);
    }
    for (int64_t c = 0; c < ncols; c++) new_cols[c] = NULL;

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        col_names[c] = ray_table_col_name(tbl, c);
        if (!col) continue;
        ray_t* nc;
        if (col->type == RAY_LIST)
            nc = ray_list_new(nrows);
        else {
            nc = col_vec_new(col, nrows);
            /* raw SYM cell-id gather — adopt the source's dictionary */
            if (nc && !RAY_IS_ERR(nc) && col->type == RAY_SYM)
                ray_sym_vec_adopt_domain(nc, col);
        }
        if (!nc || RAY_IS_ERR(nc)) {
            for (int64_t j = 0; j < c; j++)
                if (new_cols[j]) ray_release(new_cols[j]);
            scratch_free(nc_hdr);
            scratch_free(cn_hdr);
            if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
            scratch_free(k_hdr);
            ray_release(idx);
            return nc ? nc : ray_error("oom", NULL);
        }
        if (col->type == RAY_LIST) {
            ray_t** src_ptrs = (ray_t**)ray_data(col);
            ray_t** dst_ptrs = (ray_t**)ray_data(nc);
            for (int64_t r = 0; r < nrows; r++) {
                dst_ptrs[r] = src_ptrs[idx_data[r]];
                if (dst_ptrs[r]) ray_retain(dst_ptrs[r]);
            }
        }
        nc->len = nrows;
        new_cols[c] = nc;
    }

    /* Decode sort key column directly from sorted radix keys when
     * available — sequential write, much faster than random-access
     * gather.  Only for single-key sorts where sort_indices_ex
     * produced sorted_keys (non-packed path).  Narrow-int + has_nulls
     * uses a +1-shifted encoding that radix_decode_into doesn't invert,
     * so fall back to gather in that case. */
    int64_t decode_col_idx = -1;
    int8_t k0_type = key_cols[0]->type;
    bool k0_shifted = (key_cols[0]->attrs & RAY_ATTR_HAS_NULLS) &&
                      (k0_type == RAY_BOOL || k0_type == RAY_U8 ||
                       k0_type == RAY_I16);
    if (sorted_keys && n_keys == 1 && !RAY_IS_SYM(k0_type) && !k0_shifted) {
        for (int64_t c = 0; c < ncols; c++) {
            if (col_names[c] == key_ids[0] && new_cols[c]) {
                decode_col_idx = c;
                break;
            }
        }
    }
    if (decode_col_idx >= 0) {
        radix_decode_into(ray_data(new_cols[decode_col_idx]),
                          k0_type, sorted_keys,
                          nrows, descs[0]);
    }

    /* Gather remaining non-LIST, non-decode columns in batches.
     * Single-key sorts use the radix-partitioned gather; multi-key
     * fallback to the multi_gather pool dispatch. */
    for (int64_t base = 0; base < ncols; ) {
        char*   g_srcs[MGATHER_MAX_COLS];
        char*   g_dsts[MGATHER_MAX_COLS];
        uint8_t g_esz[MGATHER_MAX_COLS];
        int64_t g_nc = 0;
        for (; base < ncols && g_nc < MGATHER_MAX_COLS; base++) {
            if (!new_cols[base] || base == decode_col_idx) continue;
            ray_t* col = ray_table_get_col_idx(tbl, base);
            if (col->type == RAY_LIST) continue;
            g_srcs[g_nc] = (char*)ray_data(col);
            g_dsts[g_nc] = (char*)ray_data(new_cols[base]);
            g_esz[g_nc]  = col_esz(col);
            g_nc++;
        }
        if (g_nc == 0) continue;
        if (n_keys == 1)
            partitioned_gather(gather_pool, idx_data, nrows,
                               nrows, g_srcs, g_dsts, g_esz, g_nc);
        else {
            multi_gather_ctx_t mg = { .idx = idx_data, .ncols = g_nc };
            for (int64_t i = 0; i < g_nc; i++) {
                mg.srcs[i] = g_srcs[i];
                mg.dsts[i] = g_dsts[i];
                mg.esz[i]  = g_esz[i];
            }
            if (gather_pool)
                ray_pool_dispatch(gather_pool, multi_gather_fn, &mg, nrows);
            else
                multi_gather_fn(&mg, 0, 0, nrows);
        }
    }

    /* Propagate str_pool / null bitmaps from source columns.
     * Null propagation was the reason this function got rewritten in
     * commit 87981c8; do it explicitly here instead of relying on
     * gather_by_idx. */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        col_propagate_str_pool(new_cols[c], col);
        bool src_has_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) ||
                             ((col->attrs & RAY_ATTR_SLICE) && col->slice_parent &&
                              (col->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
        if (src_has_nulls) {
            for (int64_t r = 0; r < nrows; r++)
                if (ray_vec_is_null(col, idx_data[r]))
                    ray_vec_set_null(new_cols[c], r, true);
        }
    }

    /* Stamp the verified `sorted` marker on the PRIMARY key output column:
     * an ascending sort makes it non-descending by construction — exactly
     * the guarantee (.attr.set 'sorted) verifies with an O(n) scan, so the
     * marker still never lies.  Conservatively gated to null-free
     * integer-family keys: float NaN takes radix total order (not plain
     * `<=` order) and SYM sorts in id space, where the marker's comparison
     * semantics are not what downstream consumers (asof presort/index
     * paths) assume. */
    if (!descending &&
        !(key_cols[0]->attrs & RAY_ATTR_HAS_NULLS) &&
        (k0_type == RAY_BOOL || k0_type == RAY_U8  || k0_type == RAY_I16 ||
         k0_type == RAY_I32  || k0_type == RAY_I64 || k0_type == RAY_DATE ||
         k0_type == RAY_TIME || k0_type == RAY_TIMESTAMP)) {
        for (int64_t c = 0; c < ncols; c++) {
            if (col_names[c] == key_ids[0] && new_cols[c] &&
                !(new_cols[c]->attrs & RAY_ATTR_HAS_NULLS)) {
                new_cols[c]->attrs |= RAY_ATTR_SORTED;
                break;
            }
        }
    }
    if (!descending && n_keys > 1 && sort_part_key_type(k0_type)) {
        for (int64_t c = 0; c < ncols; c++) {
            if (col_names[c] == key_ids[0] && new_cols[c]) {
                ray_t* stamped = sort_stamp_part_col_owned(
                    new_cols[c], n_keys > 1 ? key_ids[1] : -1);
                if (stamped) new_cols[c] = stamped;
                break;
            }
        }
    }

    /* Assemble result table */
    ray_t* result = ray_table_new(ncols);
    if (!result || RAY_IS_ERR(result)) {
        for (int64_t c = 0; c < ncols; c++)
            if (new_cols[c]) ray_release(new_cols[c]);
        scratch_free(nc_hdr);
        scratch_free(cn_hdr);
        if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
        scratch_free(k_hdr);
        ray_release(idx);
        return result ? result : ray_error("oom", NULL);
    }
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        result = ray_table_add_col(result, col_names[c], new_cols[c]);
        ray_release(new_cols[c]);
    }

    scratch_free(nc_hdr);
    scratch_free(cn_hdr);
    if (sorted_keys_hdr) scratch_free(sorted_keys_hdr);
    scratch_free(k_hdr);
    ray_release(idx);
    return result;
}

/* (xasc tbl keys) — sort table ascending by key columns */
ray_t* ray_xasc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 0);
}

/* (xdesc tbl keys) — sort table descending by key columns */
ray_t* ray_xdesc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 1);
}

/* (xrank n vec) — cross-rank: assign each element to one of n groups
 * based on its sorted position.  Uses the same O(n log n) sort
 * infrastructure as `rank` / `xasc` (radix-or-merge inside
 * ray_sort_indices).  Replaces a per-element ray_vec_get-based
 * insertion sort that was both correctness-broken (the boxed elem
 * came back with type=0 so the comparison degenerated to 0.0 ≤ 0.0
 * → always true → all elements bucketed into group 0) and
 * algorithmically O(n^2). */
ray_t* ray_xrank_fn(ray_t* n_obj, ray_t* vec) {
    if (!is_numeric(n_obj))
        return ray_error("type", "xrank: first arg must be integer");
    if (!ray_is_vec(vec))
        return ray_error("type", "xrank: second arg must be a vector");

    int64_t n_groups = as_i64(n_obj);
    int64_t len = ray_len(vec);
    if (n_groups <= 0 || len == 0) return ray_vec_new(RAY_I64, 0);

    uint8_t desc = 0;
    ray_t* idx = ray_sort_indices(&vec, &desc, NULL, 1, len);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);

    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(idx); return result ? result : ray_error("oom", NULL); }
    result->len = len;
    const int64_t* idx_data = (const int64_t*)ray_data(idx);
    int64_t* out = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < len; i++)
        out[idx_data[i]] = i * n_groups / len;
    ray_release(idx);
    return result;
}

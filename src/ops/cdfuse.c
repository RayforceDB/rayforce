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

/* Fused grouped count-distinct (spec Part B). A histogram sizes one payload;
 * phase 1 scatters compact [pairhash][k][v][row] records into disjoint source/
 * partition slices; phase 2 walks each partition once with two
 * partition-local open-addressing tables — a (k,v) dedupe table and a k
 * table — producing PARTIAL per-key (distinct count, first_row) laid out in
 * key-hash buckets; phase 3 dispatches one task per bucket to merge those
 * partials into global totals, which are then ordered and emitted as
 * (k, distinct, first_row).  No intermediate pairs table, no second group
 * pipeline.
 *
 * Partitioning is by the PAIR hash fmix64(hash(k) ^ hash(v)), NOT by the
 * key hash.  Key-hash partitioning makes each key's ENTIRE row set land in
 * one partition, so a single skewed key (e.g. a region owning half the
 * table) serializes phase 2 behind one core — measured 35% SLOWER than the
 * unfused path on a 100M-row / 9K-key / heavy-skew query.  The pair hash is
 * uniform regardless of key skew, at the price of every partition seeing
 * (nearly) every key — hence the additive merge in phase 3 and the 1024
 * partition cap that bounds the merge input.
 *
 * The dedupe table's slot index uses pair-hash bits ABOVE the partition
 * bits (the agg_engine hash-bit-overlap lesson); the k table indexes on an
 * independent hash of k alone, which takes no part in partition selection. */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "rayforce.h"
#include "core/pool.h"
#include "core/profile.h"
#include "mem/heap.h"      /* ray_heap_anon_watermark */
#include "ops/internal.h"  /* scratch_*, read_col_i64 */
#include "ops/hash.h"      /* ray_hash_i64 */
#include "ops/cdfuse.h"
#include "ops/agg_engine.h" /* agg_group_card_sample — the route gate */
#include "ops/hll.h"        /* ray_count_distinct_approx — the gate's group count check */
#include "table/sym.h"     /* RAY_IS_SYM */
#include "store/col.h"    /* ray_col_cold — sampled-row read-ahead */
#include "store/colrange.h" /* an encoded column read by range */
#include "ops/rowsel.h"    /* ray_rowsel_* — the bounded kernel's selection */
#include "core/platform.h" /* ray_vm_advise_willneed */

/* Peak-footprint estimate used by the admission gate below: 24B/row for
 * phase 1's records plus up to 24B per (partition, key) pair in phase 2,
 * itself bounded by the row count — see the KNOWN MEMORY BOUND comment at
 * the phase-3 call site.  48B/row is that worst case (all-distinct input),
 * both phases' buffers alive simultaneously until cdf_free_all runs. */
#define CDF_BYTES_PER_ROW 48

/* ══════════════════════════════════════════
 * Shared helpers
 * ══════════════════════════════════════════ */

/* Avalanche finalizer — identical body to agg_radix_fmix64 (agg_engine.c,
 * static there).  Partition selection consumes the LOW log2(n_parts) bits of
 * the pair hash and the dedupe slot index the bits ABOVE them, so the raw
 * hash must be finalized for the shift-out split to hold. */
static inline uint64_t cdf_fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/* A disjoint source/partition slice of one query-owned record payload. */
typedef struct {
    char* buf;
    uint32_t n, cap;
} cdf_buf_t;

#define CDF_REC 24 /* [k 8][v 8][row 8]; the pair hash is recomputed by phase 2 */

/* Upper bound on partitions.  Phase 3's merge input is
 * n_parts × distinct-keys-per-partition and pair-hash partitioning lets every
 * partition see nearly every key, so the cap is what keeps the merge bounded;
 * 1024 still gives ample parallelism (and equals RAY_POOL_INIT_TASKS, so
 * ray_pool_dispatch_n never needs to grow its ring). */
#define CDF_MAX_PARTS 1024u

/* ══════════════════════════════════════════
 * Phase 1 — scatter
 * ══════════════════════════════════════════ */

typedef struct {
    const void* kdata;
    int8_t ktype;
    uint8_t kattrs;
    const void* vdata;
    int8_t vtype;
    uint8_t vattrs;
    uint32_t n_parts, nw;
    cdf_buf_t* bufs; /* [nw * n_parts] */
    int64_t nrows;
    bool dedup;
    _Atomic(int) oom;
    /* Rows completed in each pass. Logical source tasks are bounded by the
     * pool ring; cancellation must decline rather than return a short answer. */
    _Atomic(int64_t) rows_done;
} cdf_p1_ctx_t;

static int64_t cdf_read(const void* data, int64_t r, int8_t type, uint8_t attrs) {
    if (type == RAY_F32) {
        float v = ((const float*)data)[r];
        if (v != v) return INT64_C(0x7fc00000);
        if (v == 0) return 0;
        uint32_t bits; memcpy(&bits, &v, sizeof(bits)); return bits;
    }
    if (type == RAY_F64) {
        double v = ((const double*)data)[r];
        if (v != v) return INT64_C(0x7ff8000000000000);
        if (v == 0) return 0;
        int64_t bits; memcpy(&bits, &v, sizeof(bits)); return bits;
    }
    return read_col_i64(data, r, type, attrs);
}
/* Keep the pair hash asymmetric: hash(k)^hash(v) collapses correlated
 * k==v inputs into one partition. Both passes use the same exact transform. */
static inline uint64_t cdf_pair_hash(int64_t key, int64_t value) {
    return cdf_fmix64(ray_hash_i64(key) * 0x9E3779B97F4A7C15ULL ^ ray_hash_i64(value));
}
/* Bounded, exact source-local preaggregation. A collision only forgets an
 * earlier pair; the partition deduper still sees and reconciles extra copies.
 * Nothing persists across source tasks or queries. */
typedef struct { int64_t key, value; bool used; } cdf_recent_t;
static bool cdf_repeat(cdf_recent_t* recent, uint64_t hash, int64_t key, int64_t value) {
    cdf_recent_t* entry = &recent[hash & 63];
    if (entry->used && entry->key == key && entry->value == value) return true;
    entry->key = key; entry->value = value; entry->used = true;
    return false;
}
static inline int64_t cdf_sample_row(int64_t nrows, int i) {
    int64_t step = nrows / 1024;
    return i * step + ray_hash_i64(i) % step;
}
static bool cdf_sample_repetition(const cdf_p1_ctx_t* c) {
    if (c->nrows < 1024) return false;
    cdf_recent_t recent[64] = {{0}};
    int repeats = 0;
    for (int i = 0; i < 1024; i++) {
        int64_t row = cdf_sample_row(c->nrows, i);
        int64_t key = cdf_read(c->kdata, row, c->ktype, c->kattrs);
        int64_t value = cdf_read(c->vdata, row, c->vtype, c->vattrs);
        repeats += cdf_repeat(recent, cdf_pair_hash(key, value), key, value);
    }
    return repeats >= 128;
}
static void cdf_p1_hist(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; cdf_p1_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        cdf_buf_t* my = c->bufs + (size_t)task * c->n_parts;
        cdf_recent_t recent[64];
        if (c->dedup) memset(recent, 0, sizeof(recent));
        int64_t begin = c->nrows / c->nw * task;
        int64_t limit = task + 1 == c->nw ? c->nrows : c->nrows / c->nw * (task + 1);
        /* The first read of these rows: keep the next ones requested from
         * storage, block by block (scan read-ahead). */
        for (int64_t b = begin; b < limit; b += 8192) {
            int64_t e = limit - b > 8192 ? b + 8192 : limit;
            ray_pool_scan_at(c->nrows, b, limit);
            for (int64_t r = b; r < e; r++) {
                int64_t k = cdf_read(c->kdata, r, c->ktype, c->kattrs);
                int64_t v = cdf_read(c->vdata, r, c->vtype, c->vattrs);
                uint64_t hash = cdf_pair_hash(k, v);
                if (c->dedup && cdf_repeat(recent, hash, k, v)) continue;
                uint32_t part = hash & (c->n_parts - 1);
                if (my[part].cap == UINT32_MAX) { atomic_store(&c->oom, 1); return; }
                my[part].cap++;
            }
        }
        atomic_fetch_add_explicit(&c->rows_done, limit - begin, memory_order_relaxed);
    }
}
static void cdf_p1_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; cdf_p1_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        cdf_buf_t* my = c->bufs + (size_t)task * c->n_parts;
        cdf_recent_t recent[64];
        if (c->dedup) memset(recent, 0, sizeof(recent));
        int64_t begin = c->nrows / c->nw * task;
        int64_t limit = task + 1 == c->nw ? c->nrows : c->nrows / c->nw * (task + 1);
        for (int64_t r = begin; r < limit; r++) {
            int64_t k = cdf_read(c->kdata, r, c->ktype, c->kattrs);
            int64_t v = cdf_read(c->vdata, r, c->vtype, c->vattrs);
            uint64_t hash = cdf_pair_hash(k, v);
            if (c->dedup && cdf_repeat(recent, hash, k, v)) continue;
            uint32_t part = hash & (c->n_parts - 1);
            char* record = my[part].buf + (size_t)my[part].n++ * CDF_REC;
            ((int64_t*)record)[0] = k;
            ((int64_t*)record)[1] = v;
            ((int64_t*)record)[2] = r;
        }
        atomic_fetch_add_explicit(&c->rows_done, limit - begin, memory_order_relaxed);
    }
}

/* ══════════════════════════════════════════
 * Phase 2 — per-partition dedupe + count
 * ══════════════════════════════════════════ */

/* One group's PARTIAL result inside one partition: this partition's share of
 * the key's distinct-value count and the minimum row index it saw for the key.
 * Phase 3 sums the counts and takes the MIN of the firsts.  Kept as a
 * struct-of-three (not three parallel slabs pre-sized to the partition's
 * record count) so the output memory tracks the group count instead of the row
 * count — a 100M-row / 1K-group run pays 24 bytes per GROUP, not per ROW. */
typedef struct {
    int64_t key, cnt, first;
} cdf_grp_t;

/* Phase-3 merge buckets.  Phase 2 lays its partial triples out grouped by
 * merge bucket so phase 3 can dispatch one task per bucket: bucket m walks
 * bucket m of EVERY partition, and because the bucket is a function of the key
 * alone, keys are disjoint across buckets — no cross-task coordination. */
#define CDF_MERGE_PARTS 64

/* Merge bucket of a key, taken from bits 32..37 of fmix64(hash(k)).  The
 * phase-2 k table and the phase-3 per-bucket table both index on the LOW bits
 * of that same value, so the bucket selector is taken from the high half to
 * keep the two selectors independent (the partition itself came from the pair
 * hash, which shares no bits with this one). */
static inline uint32_t cdf_bucket(uint64_t kh) {
    return (uint32_t)((kh >> 32) & (CDF_MERGE_PARTS - 1));
}

typedef struct {
    cdf_grp_t* g; /* ng partial triples, laid out bucket by bucket */
    int64_t ng;
    int64_t boff[CDF_MERGE_PARTS + 1]; /* bucket m is g[boff[m], boff[m+1]) */
} cdf_part_t;

typedef struct {
    cdf_buf_t* bufs;
    cdf_part_t* parts;
    uint32_t nw, n_parts, part_bits;
    _Atomic(int) oom;
    /* Partitions actually processed.  A cancelled pool skips its tasks, so
     * the partitions left keep ng == 0 and their groups would silently
     * vanish from the result.  The wrapper compares this against n_parts
     * and declines (NULL) on mismatch. */
    _Atomic(int64_t) done;
} cdf_p2_ctx_t;

static void cdf_p2_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    cdf_p2_ctx_t* c = (cdf_p2_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    for (int64_t p = start; p < end; p++) {
        int64_t total = 0;
        for (uint32_t w = 0; w < c->nw; w++)
            total += c->bufs[(size_t)w * c->n_parts + p].n;
        c->parts[p].ng = 0;
        if (total == 0) {
            atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
            continue;
        }

        /* Both partition-local tables are sized to 2× the record count:
         * groups and distinct (k,v) pairs are each bounded by the record
         * count, so load stays ≤ 0.5 and linear probing always finds an
         * empty slot. */
        uint64_t dcap = 8;
        while (dcap < (uint64_t)total * 2) dcap <<= 1;
        uint64_t dmask = dcap - 1;

        /* (k,v) dedupe table: 16B key slot + int32 occupancy (an in-band
         * sentinel is unsafe — every int64 is a valid value). */
        ray_t* dh_hdr = NULL;
        int64_t* dkv = (int64_t*)scratch_alloc(
            &dh_hdr, (size_t)dcap * 2 * sizeof(int64_t) + (size_t)dcap * 4);
        if (!dkv) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int32_t* docc = (int32_t*)(dkv + (size_t)dcap * 2);
        memset(docc, 0xFF, (size_t)dcap * 4);

        /* k table: slots of (k, group index), both written on first insert. */
        ray_t* kh_hdr = NULL;
        int64_t* kk = (int64_t*)scratch_alloc(
            &kh_hdr, (size_t)dcap * sizeof(int64_t) + (size_t)dcap * 4);
        if (!kk) {
            scratch_free(dh_hdr);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int32_t* kidx = (int32_t*)(kk + dcap);
        memset(kidx, 0xFF, (size_t)dcap * 4);

        int64_t cap = total < 256 ? total : 256;
        cdf_grp_t* grps = (cdf_grp_t*)ray_alloc_raw((size_t)cap * sizeof(cdf_grp_t));
        if (!grps) {
            scratch_free(dh_hdr);
            scratch_free(kh_hdr);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int64_t ng = 0;
        int failed = 0;

        for (uint32_t w = 0; w < c->nw && !failed; w++) {
            cdf_buf_t* b = &c->bufs[(size_t)w * c->n_parts + p];
            const char* rec = b->buf;
            for (uint32_t i = 0; i < b->n; i++, rec += CDF_REC) {
                int64_t k = ((const int64_t*)rec)[0];
                int64_t v = ((const int64_t*)rec)[1];
                int64_t row = ((const int64_t*)rec)[2];
                uint64_t ph = cdf_pair_hash(k, v);

                /* k table probe for EVERY record: this partition's first_row
                 * must be the MIN over all its rows of the key, not only over
                 * rows that open a fresh (k,v) pair.  The slot index comes
                 * from a hash of k ALONE — recomputed here rather than stored,
                 * to keep the record at 24B — and that hash never selects a
                 * partition, so it needs no bit shift-out. */
                uint64_t t = cdf_fmix64(ray_hash_i64(k)) & dmask;
                int64_t gi = 0;
                for (;;) {
                    if (kidx[t] == -1) {
                        if (ng >= cap) {
                            /* Invariant: ng <= total — every group is opened
                             * by a distinct record of this partition, so the
                             * record count bounds the group count and `total`
                             * is a legitimate clamp.  The growth must never
                             * be a no-op while ng == cap (that would write one
                             * past the end), so nc is always > cap: clamp to
                             * total only while cap is still below it. */
                            int64_t nc = cap * 2;
                            if (nc > total && cap < total) nc = total;
                            cdf_grp_t* ngp = (cdf_grp_t*)ray_realloc_raw(
                                grps, (size_t)nc * sizeof(cdf_grp_t));
                            if (!ngp) { failed = 1; break; }
                            grps = ngp;
                            cap = nc;
                        }
                        if (ng > INT32_MAX) { failed = 1; break; }
                        kk[t] = k;
                        kidx[t] = (int32_t)ng;
                        grps[ng].key = k;
                        grps[ng].cnt = 0;
                        grps[ng].first = row;
                        gi = ng++;
                        break;
                    }
                    if (kk[t] == k) {
                        gi = kidx[t];
                        if (row < grps[gi].first) grps[gi].first = row;
                        break;
                    }
                    t = (t + 1) & dmask;
                }
                if (failed) break;

                /* Dedupe on the pair hash already in the record; slot bits
                 * above the partition bits. */
                uint64_t s = (ph >> c->part_bits) & dmask;
                for (;;) {
                    if (docc[s] == -1) {
                        dkv[s * 2] = k;
                        dkv[s * 2 + 1] = v;
                        docc[s] = 1;
                        grps[gi].cnt++;
                        break;
                    }
                    if (dkv[s * 2] == k && dkv[s * 2 + 1] == v) break;
                    s = (s + 1) & dmask;
                }
            }
        }

        scratch_free(dh_hdr);
        scratch_free(kh_hdr);
        if (failed) {
            ray_free_raw(grps);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }

        /* Re-lay the partials out bucket by bucket (counting sort over 64
         * buckets) so phase 3 reads each bucket as one contiguous run. */
        int64_t* boff = c->parts[p].boff;
        int64_t pos[CDF_MERGE_PARTS];
        memset(boff, 0, sizeof(int64_t) * (CDF_MERGE_PARTS + 1));
        for (int64_t i = 0; i < ng; i++)
            boff[cdf_bucket(cdf_fmix64(ray_hash_i64(grps[i].key))) + 1]++;
        for (uint32_t m = 0; m < CDF_MERGE_PARTS; m++) {
            boff[m + 1] += boff[m];
            pos[m] = boff[m];
        }
        cdf_grp_t* sorted = (cdf_grp_t*)ray_alloc_raw((size_t)ng * sizeof(cdf_grp_t));
        if (!sorted) {
            ray_free_raw(grps);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        for (int64_t i = 0; i < ng; i++)
            sorted[pos[cdf_bucket(cdf_fmix64(ray_hash_i64(grps[i].key)))]++] = grps[i];
        ray_free_raw(grps);

        c->parts[p].g = sorted;
        c->parts[p].ng = ng;
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
}

/* ══════════════════════════════════════════
 * Phase 3 — parallel key-bucketed merge
 * ══════════════════════════════════════════ */

typedef struct {
    cdf_grp_t* g;
    int64_t n;
} cdf_merge_t;

typedef struct {
    cdf_part_t* parts;
    uint32_t n_parts;
    cdf_merge_t* mg; /* [CDF_MERGE_PARTS] */
    _Atomic(int) oom;
    _Atomic(int64_t) done; /* same dropped-task guard as phases 1-2 */
} cdf_p3_ctx_t;

static void cdf_p3_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    cdf_p3_ctx_t* c = (cdf_p3_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    for (int64_t m = start; m < end; m++) {
        int64_t total = 0;
        for (uint32_t p = 0; p < c->n_parts; p++)
            total += c->parts[p].boff[m + 1] - c->parts[p].boff[m];
        c->mg[m].n = 0;
        if (total == 0) {
            atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
            continue;
        }

        /* Slot table sized to 2× the triple count — distinct keys in this
         * bucket are bounded by it, so load stays ≤ 0.5. */
        uint64_t scap = 8;
        while (scap < (uint64_t)total * 2) scap <<= 1;
        uint64_t smask = scap - 1;
        ray_t* s_hdr = NULL;
        int32_t* slot = (int32_t*)scratch_alloc(&s_hdr, (size_t)scap * sizeof(int32_t));
        if (!slot) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        memset(slot, 0xFF, (size_t)scap * sizeof(int32_t));

        int64_t cap = total < 256 ? total : 256, n = 0;
        cdf_grp_t* out = (cdf_grp_t*)ray_alloc_raw((size_t)cap * sizeof(cdf_grp_t));
        if (!out) {
            scratch_free(s_hdr);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int failed = 0;

        for (uint32_t p = 0; p < c->n_parts && !failed; p++) {
            const cdf_grp_t* src = c->parts[p].g;
            for (int64_t i = c->parts[p].boff[m]; i < c->parts[p].boff[m + 1]; i++) {
                int64_t key = src[i].key;
                uint64_t s = cdf_fmix64(ray_hash_i64(key)) & smask;
                while (slot[s] != -1 && out[slot[s]].key != key) s = (s + 1) & smask;
                if (slot[s] != -1) {
                    cdf_grp_t* dst = &out[slot[s]];
                    /* Counts ADD: a (k,v) pair hashes to exactly one phase-1
                     * partition, so partitions hold disjoint distinct sets for
                     * the key.  first_row is a MIN, which composes likewise. */
                    dst->cnt += src[i].cnt;
                    if (src[i].first < dst->first) dst->first = src[i].first;
                    continue;
                }
                if (n >= cap) {
                    int64_t nc = cap * 2;
                    if (nc > total && cap < total) nc = total;
                    cdf_grp_t* no = (cdf_grp_t*)ray_realloc_raw(
                        out, (size_t)nc * sizeof(cdf_grp_t));
                    if (!no) { failed = 1; break; }
                    out = no;
                    cap = nc;
                }
                if (n > INT32_MAX) { failed = 1; break; }
                out[n] = src[i];
                slot[s] = (int32_t)n;
                n++;
            }
        }

        scratch_free(s_hdr);
        if (failed) {
            ray_free_raw(out);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        c->mg[m].g = out;
        c->mg[m].n = n;
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
}

/* ══════════════════════════════════════════
 * Assembly
 * ══════════════════════════════════════════ */

/* Merged group.  first_row values are unique across merged groups — one entry
 * per distinct key, each carrying that key's global minimum row — so the
 * ordering sort is total and the comparator needs no tiebreak. */
static int cdf_grp_cmp(const void* a, const void* b) {
    int64_t x = ((const cdf_grp_t*)a)->first, y = ((const cdf_grp_t*)b)->first;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct {
    const cdf_grp_t* groups;
    const uint64_t* order;
    uint64_t mask;
    int64_t* keys;
    int64_t* counts;
    int64_t* firsts;
} cdf_emit_t;
static void cdf_emit(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; cdf_emit_t* c = raw;
    for (int64_t i = start; i < end; i++) {
        const cdf_grp_t* group = &c->groups[c->order ? c->order[i] & c->mask : (uint64_t)i];
        c->keys[i] = group->key; c->counts[i] = group->cnt; c->firsts[i] = group->first;
    }
}

/* Same sqrt-style sizing as agg_radix_part_count: at least one partition per
 * worker, and enough partitions that each holds ~sqrt(nrows) records — capped
 * at CDF_MAX_PARTS to bound phase 3's merge input. */
static uint32_t cdf_part_count(uint32_t nworkers, int64_t nrows) {
    uint32_t n = 1;
    uint64_t rows = nrows > 0 ? (uint64_t)nrows : 1;
    while ((n < nworkers || (uint64_t)n < rows / n + (rows % n != 0)) &&
           n < CDF_MAX_PARTS)
        n <<= 1;
    return n;
}

static int cdf_type_ok(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 ||
        t == RAY_I32 || t == RAY_I64 || t == RAY_DATE ||
        t == RAY_TIME || t == RAY_TIMESTAMP || t == RAY_F32 || t == RAY_F64 || RAY_IS_SYM(t);
}

/* Free the shared payload and phase-2 outputs on fallback and success. */
static void cdf_free_all(cdf_buf_t* bufs, char* records, cdf_part_t* parts,
                         uint32_t n_parts) {
    ray_free_raw(records);
    ray_free_raw(bufs);
    if (parts)
        for (uint32_t p = 0; p < n_parts; p++) ray_free_raw(parts[p].g);
    ray_free_raw(parts);
}

ray_t* ray_cd_fused(ray_t* key_col, ray_t* val_col, int64_t nrows) {
    if (!key_col || !val_col || nrows <= 0) return NULL;
    if (!ray_is_vec(key_col) || !ray_is_vec(val_col)) return NULL;
    if (!cdf_type_ok(key_col->type) || !cdf_type_ok(val_col->type)) return NULL;

    if (key_col->len < nrows || val_col->len < nrows) return NULL;
    if (nrows < CDF_MIN_ROWS) return NULL; /* small: existing path fine */

    ray_pool_t* pool = ray_pool_get();
    /* Self-contained dispatch guard (folds ray_pool_par_dispatch_ok in, not
     * just min-rows): live pool with background workers and not already
     * inside an in-flight dispatch, so any future caller — not just the
     * query.c rewrite, which currently duplicates this check — gets a safe
     * kernel.  The query.c call-site guard is left in place unchanged; this
     * makes it redundant there but load-bearing for other callers. */
    if (!ray_pool_par_dispatch_ok(pool, nrows, CDF_MIN_ROWS)) return NULL;

    /* Memory admission gate: peak footprint is ~CDF_BYTES_PER_ROW bytes/row
     * (see the constant's derivation above and the phase-3 KNOWN MEMORY
     * BOUND comment).  Decline rather than risk the OOM killer when that
     * estimate would exceed a quarter of the heap's memory budget (default:
     * physical RAM or the cgroup limit) — leaving headroom for the rest of the query
     * (source columns, other operators, concurrent work) sharing the same
     * budget.  No new env knob: this rides the existing watermark, which
     * ray_heap_set_anon_watermark already lets tests/embedders override.
     * Same "unknown RAM -> stay permissive" convention as heap.c's own
     * heap_anon_would_exceed: a caller that never ran ray_runtime_new (unit
     * tests calling ray_cd_fused directly, without the app's runtime/RAM
     * probe) sees ray_sys_total_ram() == 0, which must not read as a
     * zero-byte budget. */
    int64_t wm = ray_heap_mem_budget();
    if (wm > 0 && (double)nrows * CDF_BYTES_PER_ROW > (double)wm / 4.0)
        return NULL;

    if ((uint64_t)nrows > SIZE_MAX / CDF_REC) return NULL;
    uint32_t workers = ray_pool_total_workers(pool);
    uint32_t nw = workers * 4;
    if (nw > RAY_POOL_INIT_TASKS) nw = RAY_POOL_INIT_TASKS;
    uint32_t n_parts = cdf_part_count(workers, nrows);
    char* records = NULL;
    size_t nbuf = (size_t)nw * n_parts;
    cdf_buf_t* bufs = (cdf_buf_t*)ray_calloc_raw(nbuf * sizeof(cdf_buf_t));
    cdf_part_t* parts = (cdf_part_t*)ray_calloc_raw((size_t)n_parts * sizeof(cdf_part_t));
    if (!bufs || !parts) {
        cdf_free_all(bufs, records, parts, n_parts);
        return NULL;
    }

    cdf_p1_ctx_t p1 = {
        .kdata = ray_data(key_col), .ktype = key_col->type, .kattrs = key_col->attrs,
        .vdata = ray_data(val_col), .vtype = val_col->type, .vattrs = val_col->attrs,
        .n_parts = n_parts, .nw = nw, .bufs = bufs,
        .nrows = nrows,
        .oom = 0, .rows_done = 0,
    };
    if (nrows >= 1024) {
        bool kc = ray_col_cold(key_col), vc = ray_col_cold(val_col);
        for (int i = 0; (kc || vc) && i < 1024; i++) {
            if (kc) ray_col_want_row(key_col, cdf_sample_row(nrows, i));
            if (vc) ray_col_want_row(val_col, cdf_sample_row(nrows, i));
        }
    }
    p1.dedup = cdf_sample_repetition(&p1);
    if (p1.dedup) ray_profile_tick("count-distinct: source preaggregation");
    ray_profile_tick("count-distinct: prepared");
    ray_pool_dispatch_n(pool, cdf_p1_hist, &p1, nw);
    if (atomic_load(&p1.oom) || atomic_load(&p1.rows_done) != nrows) {
        cdf_free_all(bufs, records, parts, n_parts); return NULL;
    }
    int64_t record_count = 0;
    for (size_t b = 0; b < nbuf; b++) record_count += bufs[b].cap;
    records = ray_alloc_raw((size_t)record_count * CDF_REC);
    if (!records) { cdf_free_all(bufs, records, parts, n_parts); return NULL; }
    int64_t record_offset = 0;
    for (size_t b = 0; b < nbuf; b++) {
        bufs[b].buf = records + (size_t)record_offset * CDF_REC;
        record_offset += bufs[b].cap;
    }
    atomic_store(&p1.rows_done, 0);
    ray_profile_tick("count-distinct: histogram");
    ray_pool_dispatch_n(pool, cdf_p1_fn, &p1, nw);
    ray_profile_tick("count-distinct: scattered pairs");
    if (atomic_load_explicit(&p1.oom, memory_order_relaxed) ||
        atomic_load_explicit(&p1.rows_done, memory_order_relaxed) != nrows) {
        cdf_free_all(bufs, records, parts, n_parts);
        return NULL; /* fallback, not an error */
    }

    cdf_p2_ctx_t p2 = {
        .bufs = bufs, .parts = parts, .nw = nw, .n_parts = n_parts,
        .part_bits = (uint32_t)__builtin_ctz(n_parts), .oom = 0, .done = 0,
    };
    ray_pool_dispatch_n(pool, cdf_p2_fn, &p2, n_parts);
    ray_profile_tick("count-distinct: deduplicated pairs");
    if (atomic_load_explicit(&p2.oom, memory_order_relaxed) ||
        atomic_load_explicit(&p2.done, memory_order_relaxed) != (int64_t)n_parts) {
        cdf_free_all(bufs, records, parts, n_parts);
        return NULL;
    }

    /* Phase 3: merge the per-partition partials into global per-key totals, one
     * task per key bucket.  Keys are disjoint across buckets, so the tasks
     * never touch the same entry and need no coordination.
     *
     * KNOWN MEMORY BOUND: peak footprint scales with ROWS, not with groups —
     * phase 2's partial triples (24B per distinct (partition, key) pair, itself
     * bounded by the row count) coexist with phase 1's 24B-per-row records
     * until the cdf_free_all below, so a pathological all-distinct input peaks
     * near 48B/row.  Cardinality-aware admission (declining when the estimated
     * distinct-pair count would blow a budget) is future work. */
    cdf_merge_t mg[CDF_MERGE_PARTS];
    memset(mg, 0, sizeof(mg));
    cdf_p3_ctx_t p3 = {
        .parts = parts, .n_parts = n_parts, .mg = mg, .oom = 0, .done = 0,
    };
    ray_pool_dispatch_n(pool, cdf_p3_fn, &p3, CDF_MERGE_PARTS);
    ray_profile_tick("count-distinct: merged counts");
    if (atomic_load_explicit(&p3.oom, memory_order_relaxed) ||
        atomic_load_explicit(&p3.done, memory_order_relaxed) != CDF_MERGE_PARTS) {
        for (uint32_t m = 0; m < CDF_MERGE_PARTS; m++) ray_free_raw(mg[m].g);
        cdf_free_all(bufs, records, parts, n_parts);
        return NULL;
    }
    cdf_free_all(bufs, records, parts, n_parts);

    int64_t ng = 0;
    for (uint32_t m = 0; m < CDF_MERGE_PARTS; m++) ng += mg[m].n;
    cdf_grp_t* merged = (cdf_grp_t*)ray_alloc_raw((size_t)(ng > 0 ? ng : 1) *
                                                  sizeof(cdf_grp_t));
    if (!merged) {
        for (uint32_t m = 0; m < CDF_MERGE_PARTS; m++) ray_free_raw(mg[m].g);
        return NULL;
    }
    int64_t mo = 0;
    for (uint32_t m = 0; m < CDF_MERGE_PARTS; m++) {
        if (mg[m].n)
            memcpy(merged + mo, mg[m].g, (size_t)mg[m].n * sizeof(cdf_grp_t));
        mo += mg[m].n;
        ray_free_raw(mg[m].g);
    }
    /* Restore stable first-seen order with integer radix sorting. The packed
     * row/order key replaces a serial comparison sort of 24-byte records.
     * Free the unused sort buffer before allocating outputs: peak remains
     * at most 56 bytes per group, within the existing row-based admission. */
    uint64_t* order = NULL;
    uint64_t mask = 0;
    if (ng >= RADIX_SORT_THRESHOLD && nrows > 1) {
        unsigned bits = 64 - __builtin_clzll((uint64_t)ng - 1);
        unsigned row_bits = 64 - __builtin_clzll((uint64_t)nrows - 1);
        if (bits + row_bits <= 64) {
            uint64_t* a = ray_alloc_raw((size_t)ng * sizeof(uint64_t));
            uint64_t* b = ray_alloc_raw((size_t)ng * sizeof(uint64_t));
            if (a && b) {
                mask = (UINT64_C(1) << bits) - 1;
                for (int64_t i = 0; i < ng; i++) a[i] = ((uint64_t)merged[i].first << bits) | (uint64_t)i;
                order = packed_radix_sort_run(pool, a, b, ng, (uint8_t)((bits + row_bits + 7) / 8));
            }
            if (a != order) ray_free_raw(a);
            if (b != order) ray_free_raw(b);
        }
    }
    if (atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
        ray_free_raw(order); ray_free_raw(merged); return NULL;
    }
    if (!order) qsort(merged, (size_t)ng, sizeof(cdf_grp_t), cdf_grp_cmp);
    ray_profile_tick("count-distinct: ordered groups");

    ray_t* keys = ray_vec_new(RAY_I64, ng);
    ray_t* cnts = ray_vec_new(RAY_I64, ng);
    ray_t* firsts = ray_vec_new(RAY_I64, ng);
    ray_t* tbl = ray_table_new(3);
    if (!keys || RAY_IS_ERR(keys) || !cnts || RAY_IS_ERR(cnts) ||
        !firsts || RAY_IS_ERR(firsts) || !tbl || RAY_IS_ERR(tbl)) {
        ray_release(keys); ray_release(cnts); ray_release(firsts); ray_release(tbl);
        ray_free_raw(order); ray_free_raw(merged);
        return NULL;
    }
    keys->len = cnts->len = firsts->len = ng;
    cdf_emit_t emit = {merged, order, mask, ray_data(keys), ray_data(cnts), ray_data(firsts)};
    ray_pool_dispatch(pool, cdf_emit, &emit, ng);
    ray_free_raw(order); ray_free_raw(merged);
    if (atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
        ray_release(keys); ray_release(cnts); ray_release(firsts); ray_release(tbl); return NULL;
    }

    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), keys);
    tbl = ray_table_add_col(tbl, ray_sym_intern("u", 1), cnts);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_first", 6), firsts);
    ray_release(keys);
    ray_release(cnts);
    ray_release(firsts);
    if (!tbl || RAY_IS_ERR(tbl)) {
        ray_release(tbl);
        return NULL;
    }
    return tbl;
}

/* The scatter path serves a distinct count beside the group by K: the
 * post-group row→group map and a per-group dedup over the slices (the
 * global-hash kernel past 50 K groups).  Up to CD_SCATTER_MAX_GROUPS groups
 * it is the route whenever the rewrite's (K, X) grouping would not be
 * dense; past CD_SCATTER_MANY_GROUPS its map and kernel fall behind the
 * rewrite's radix grouping whatever that grouping dedups; between the two
 * the tuple count decides. */
#define CD_SCATTER_MAX_GROUPS  (256 * 1024)
#define CD_SCATTER_MANY_GROUPS (1024 * 1024)

/* The scatter's per-group dedup is serial over a group: its critical path
 * is the largest group's rows, where the rewrite's (K, X) pass spreads
 * every row over the workers.  The rewrite is the route once the largest
 * group holds more than this many workers' shares of the rows: at 28
 * workers a key whose largest value held half the rows took 180 ms on the
 * scatter against 136 on the rewrite, one holding 8% of them 107 against
 * 138, uniform groups 89 against 140 (12 M rows surviving a filter over
 * 100 M; the issue's 45 groups hold 2% each). */
#define CD_SCATTER_SKEW 8

/* Whether the two-pass rewrite (group by (K, X), then count per K; query.c)
 * beats grouping by K and counting distinct X over each group's slice — the
 * path a `(count (distinct X))` takes beside other aggregates, and the one
 * the planner falls back to when this returns false (#756).
 *
 * The rewrite wins when its (K, X) grouping is dense: the packed key space
 * — the product of the keys' spans and X's — fits the row count, so the
 * dedup is a direct-index pass.  Past that it is a radix grouping that
 * materializes and orders every (K, X) tuple — 1.8 GB and 3x the time of
 * the slice dedup at 10 M rows with 45 groups of 3 M distinct values —
 * unless the groups are so many that the scatter's own machinery is the
 * slower one: at 2 M and 5 M groups the rewrite took 1.3x to 2x less time,
 * at 750 K groups whose (K, X) tuples were as many as the rows the scatter
 * took 1.3x less time and 1.9x less memory, and at 750 K groups whose
 * tuples were a sixth of the rows the rewrite took 1.3x less time; below
 * 256 K groups the scatter is the route (#756).  Few groups do not favour
 * the rewrite: at 2 to 9 groups of 3 M to 6 M values on 28 workers the
 * slice dedup took the same time or less and 40% of the memory, and the
 * issue's 45 groups on 46 workers would otherwise have taken the rewrite at
 * 3x the time.  A group holding many workers' shares of the rows takes
 * the rewrite (CD_SCATTER_SKEW).  The group, value and tuple counts and
 * the largest group's rows are sampled estimates (agg_group_card_sample)
 * over the `nrows` rows of `rows` when a `where:` selected them — a filter
 * that drops a dominant value hides the groups and the shares of what
 * remains — else over the table; half a millisecond at 10 M rows.  A
 * skewed or clustered key the sample cannot exhaust estimates high,
 * towards the rewrite. */
/* The sampled count of the groups is a lower bound for a key the sample
 * reads mostly as singletons — six million search phrases over thirteen
 * million rows, most on one row and a few on thousands, read as a few
 * hundred thousand (the Chao1 takes the phrases seen twice for the whole
 * tail's repeats).  Before the slices are chosen on such a count, the key
 * columns are counted approximately in full (ray_count_distinct_approx,
 * parallel, a few milliseconds at 100 M rows): a column past the many-groups
 * mark makes the tuple past it.  Over the whole table, not the selection —
 * an upper bound that errs towards the rewrite. */
static bool cd_many_groups_in_full(ray_t** K_cols, int n_K) {
    for (int j = 0; j < n_K; j++) {
        ray_t* k = K_cols[j];
        /* the count reads the column whole, a chunk per thread: a mapped
         * column not in memory is asked for ahead as one range, else each
         * thread waits on its chunk's pages in turn */
        if (ray_pool_scan_on() && ray_col_cold(k))
            ray_pool_want(ray_data(k), (size_t)k->len * col_esz(k));
        ray_t* c = ray_count_distinct_approx(k);
        if (!c) return false;
        bool many = !RAY_IS_ERR(c) && c->type == -RAY_I64 && c->i64 > CD_SCATTER_MANY_GROUPS;
        if (RAY_IS_ERR(c)) ray_error_free(c); else ray_release(c);
        if (many) return true;
    }
    return false;
}

bool ray_cd_two_pass_preferred(ray_t** K_cols, int n_K, ray_t* X_col,
                               const int64_t* rows, int64_t nrows, int64_t population,
                               int64_t workers) {
    if (n_K < 1 || n_K > 15) return true;
    const int64_t pop = population > nrows ? population : nrows;
    int64_t k_est[15], k_card = 0, k_max = 0, x_est = 0, t_est = 0;
    if (!agg_group_card_sample(K_cols, (uint32_t)n_K, (uint32_t)n_K, rows, nrows, population,
                               k_est, &k_card, &k_max, NULL))
        return true;                      /* K unbounded (a key the estimate does not cover) */
    if (!agg_group_card_sample(&X_col, 1, 1, rows, nrows, population, &x_est, NULL, NULL, NULL))
        return false;                     /* X unbounded: (K, X) is radix; the slices dedup */
    const int64_t nrows_list = nrows;
    nrows = pop;                          /* the rows the estimates describe */
    /* the packed (K, X) key space: the keys' spans times X's */
    int64_t k_span = 1;
    for (int j = 0; j < n_K; j++) {
        if (k_span > nrows / k_est[j]) { k_span = nrows + 1; break; }
        k_span *= k_est[j];
    }
    if (k_span <= nrows / x_est) return true;        /* dense: a direct-index dedup */
    /* a dominant group: the scatter would dedup it on one worker */
    if (workers > 1 && k_max > nrows / workers * CD_SCATTER_SKEW) return true;
    if (k_card > CD_SCATTER_MANY_GROUPS) return true;
    /* the slices on a count the sample cannot vouch for (more sampled
     * singletons than the Chao1's repeats explain: any count past the
     * sample's size): a full approximate count may say many groups */
    if (k_card > AGG_EST_SAMPLE_ROWS && cd_many_groups_in_full(K_cols, n_K)) return true;
    if (k_card <= CD_SCATTER_MAX_GROUPS) return false;
    /* a radix (K, X) pass over 256 K to 1 M groups: the rewrite wins when
     * that pass dedups, the scatter when the tuples are nearly the rows */
    ray_t* cols[16];
    for (int j = 0; j < n_K; j++) cols[j] = K_cols[j];
    cols[n_K] = X_col;
    if (!agg_group_card_sample(cols, (uint32_t)n_K + 1, (uint32_t)n_K, rows,
                               population > 0 ? nrows_list : nrows, population,
                               NULL, NULL, NULL, &t_est))
        return true;
    return t_est < nrows - nrows / 4;
}

/* ══════════════════════════════════════════
 * Bounded grouped count-distinct
 * ══════════════════════════════════════════
 *
 * The per-group slices (count_distinct_per_group_buf, query.c) take a row
 * map (8 B a row), a row index ordered by group (8 B a row) and a hash table
 * per group sized by its rows, and gather the values through the index: on a
 * 100M-row table that is 1.6 GB of row-sized scratch read at random, which a
 * small machine spills to disk and rewrites several times over.  The global
 * hash past 50 K groups scatters 16 B a row besides the row map.
 *
 * Here the K and X columns are read in order, a chunk at a time.  Phase A
 * (a task per run of CDB_TASK_ROWS rows) maps each row to its group, hashes
 * the (group, value) pair, drops a pair the task saw a moment ago and sorts
 * the rest by partition (the hash's top bits) into the chunk's record
 * buffer.  Phase B (a task per partition) adds the chunk's records of its
 * partition to that partition's set — open addressing over (value, group+1)
 * slots, 6 to 12 bytes each.  The sets hold each distinct pair once; the
 * record buffer, the staging and the per-worker counts are a few tens of MB
 * whatever the row count.  The input is read once, the sets are written in
 * place: no structure grows with the rows.
 *
 * The sets are what grows with the distinct pairs.  A set grows when phase
 * B fills it to load 3/4 (its task stops there; the main thread gives it a
 * block sized by the share of new pairs among the records so far, and phase
 * B goes on), so a set holds what its pairs need, not its raw records.  The
 * sets are sized against a budget — three quarters of the room under the
 * anon watermark, counting the blocks the heap can reuse, and no less than
 * a sixteenth of the watermark or a quarter of the slices' row buffers —
 * and when growing them would pass it, the partitions at the top of the
 * round's range are freed and left to a further round, which reads the
 * input again and keeps only their pairs.  Partitions are disjoint in
 * pairs, so a partition is counted in exactly one round; how many
 * partitions a round takes is set from the bytes per partition the previous
 * one ended with.  The first trim projects the round's sets over the rows
 * left: past CDB_MAX_ROUNDS budgets' worth the budget rises to a third of
 * that (up to the slices' row buffers), rather than re-reading the input
 * once a partition.
 *
 * Every set block is allocated and freed by the calling thread (phase B
 * only fills blocks the main thread handed it), so nothing is freed into a
 * worker's foreign list. */

#define CDB_TASK_ROWS   32768   /* rows per phase-A task (at most), a multiple of RAY_MORSEL_ELEMS */
#define CDB_TASK_ROWS_MIN 4096  /* the fewest, under a small budget */
#define CDB_MAX_ROUNDS  3       /* passes over the input before the sets may pass the budget */
#define CDB_CACHE       256     /* recent-pair cache entries per task */
#define CDB_SET_MIN_BYTES 1024  /* first block of a partition set */
#define CDB_MAX_PARTS   1024u
#define CDB_LOCAL_CNT   (INT64_C(4) << 20)  /* per-worker count cells before atomics */
#define CDB_MIN_BUDGET  (INT64_C(4) << 20)  /* and a sixteenth of the watermark */
#define CDB_RA_BYTES    (INT64_C(48) << 20) /* input requested ahead of the scan */
#define CDB_SKEW        4       /* workers' shares of the rows a dominant group holds */
#define CDB_SAMPLE_ROWS (INT64_C(1) << 26) /* rows from which the route samples */

typedef struct { int64_t x; uint32_t g; uint32_t h; } cdb_rec_t;
typedef struct { int64_t x; uint64_t g; } cdb_recent_t;

typedef struct {
    const void* data;   /* NULL: every row reads `fill`, unless `src` */
    int64_t     lo, n;  /* global rows [lo, lo + n) */
    int8_t      type;
    uint8_t     attrs, esz;
    bool        cold;
    int64_t     fill;
    ray_col_src_t* src; /* an encoded column read by range (its cdb_col_t's own) */
} cdb_seg_t;

typedef struct {
    cdb_seg_t* seg;
    int64_t    nseg, rows;
    bool       narrow;  /* every cell is a 32-bit pattern (see cdb_col_open) */
    bool       bound;   /* own is bound: a single-file encoded column */
    ray_col_src_t own;
} cdb_col_t;

typedef struct {
    void*    blk;       /* cap values, then cap tags (group + 1, 0 = empty) */
    void*    old;       /* the previous slots, rehashed into blk by phase B */
    uint64_t cap, ocap;
    uint64_t bytes;     /* blk's buddy block: a power of two, header included */
    uint64_t obytes;    /* old's */
    int64_t  n;
    /* phase B over one chunk: the records of the partition (inc), those
     * taken (seen) and the new pairs among them (added), and where it
     * stopped when the set filled (task rt, record ri) */
    int64_t  inc, seen, added, ri;
    uint32_t rt;
    bool     fresh;     /* blk not initialised yet */
    bool     full;      /* stopped at load 3/4: to grow and go on */
} cdb_set_t;

typedef struct {
    cdb_col_t       key, val;
    const int64_t*  row_gid;
    const int64_t*  kkeys;      /* key -> group + 1 */
    const uint32_t* ktag;
    uint64_t        kmask;
    int64_t         n_groups, nrows;
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    int64_t         sel_nsegs;
    const int64_t*  sel_pre;    /* a row map over the kept rows: rank of each segment's first kept row */
    uint64_t        vmask;
    uint32_t        P, pshift, lo, hi, T, T_c;
    int64_t         S;          /* rows per phase-A task */
    int64_t         chunk_r0;
    cdb_rec_t*      recs;       /* [T * S] */
    cdb_rec_t*      stage;      /* [W * S] */
    uint16_t*       spart;      /* [W * S] */
    const uint32_t* plist;      /* phase B: the partitions of its tasks */
    int64_t*        toff;       /* [T * (P + 1)] */
    cdb_set_t*      sets;       /* [P] */
    int             var;        /* slot layout, see CDB_SET_FNS */
    int64_t*        cnt;        /* [W * n_groups], or [n_groups] when shared */
    bool            cnt_shared;
    bool            one_group;  /* no key: every row is group 0 */
    ray_scan_fail_t fail;       /* a range read's failure (encoded columns) */
    _Atomic(int64_t) done;
} cdb_ctx_t;

/* A slot of a set of `cap` slots from 32 hash bits (multiply-shift): any
 * capacity, so a set fills its buddy block (cdb_set_bytes). */
static inline uint64_t cdb_slot(uint32_t h, uint64_t cap) {
    return ((uint64_t)h * cap) >> 32;
}

static inline uint64_t cdb_hash(int64_t x, uint64_t g) {
    return cdf_fmix64((uint64_t)x * 0x9E3779B97F4A7C15ULL + g * 0xC2B2AE3D27D4EB4FULL);
}

static bool cdb_type_ok(int8_t t, bool is_val) {
    switch (t) {
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_SYM:
        return true;
    case RAY_F64:
        return is_val;
    default:
        return false;
    }
}

/* A cell as a key: integers widened with their sign, SYM ids and BOOL/U8
 * without, F64 through canon_f64_key — what the other count-distinct
 * kernels key a value by, nulls included (the sentinels are ordinary
 * integers, a SYM null is id 0, every NaN folds to one). */
static inline int64_t cdb_cell(const void* d, int64_t r, int8_t t, uint8_t attrs) {
    if (t == RAY_F64) return canon_f64_key(((const double*)d)[r]);
    return read_col_i64(d, r, t, attrs);
}

/* What the rows of a partition without the column read as: the type's
 * null, keyed as a stored one (BOOL/U8 have none: 0, as parted_fill_nulls
 * writes). */
static int64_t cdb_null_cell(int8_t t) {
    switch (t) {
    case RAY_I16: return NULL_I16;
    case RAY_I32: case RAY_DATE: case RAY_TIME: return NULL_I32;
    case RAY_I64: case RAY_TIMESTAMP: return NULL_I64;
    case RAY_F64: return canon_f64_key(NULL_F64);
    default: return 0;
    }
}

static void cdb_col_close(cdb_col_t* c) {
    ray_free_raw(c->seg);
    c->seg = NULL;
    if (c->bound) ray_col_src_release(&c->own);
    c->bound = false;
}

/* The segments of a flat, parted or MAPCOMMON column.  `narrow`: the low 32
 * bits of every cell identify it — the signed family (I16, I32, DATE, TIME)
 * whose cells are 32-bit integers widened, or the unsigned one (BOOL, U8,
 * SYM) while every cell is below 2^32 — so a set stores 4 bytes a value.
 * 1 when served, 0 when not (a type, a segment of another type), -1 when
 * memory ran out. */
static int cdb_col_open(cdb_col_t* c, ray_t* col, ray_t* tbl, bool is_val, ray_t** err) {
    memset(c, 0, sizeof(*c));
    *err = NULL;
    if (!col || RAY_IS_ERR(col)) return 0;
    int8_t base;
    int64_t n;
    if (col->type == RAY_ENCCOL) {
        /* a single-file generation-2 column, read a vector at a time by
         * the tasks (never decoded whole); a parted one is not served */
        ray_col_meta_t m;
        if (!ray_col_meta_of(col, &m) || RAY_IS_PARTED(m.type) || !cdb_type_ok(m.type, is_val)) return 0;
        ray_t* be = ray_col_src_bind(col, &c->own);
        if (be) { *err = be; return -1; }
        c->bound = true;
        base = c->own.type;
        n = 1;
        c->seg = (cdb_seg_t*)ray_calloc_raw(sizeof(cdb_seg_t));
        if (!c->seg) { cdb_col_close(c); *err = ray_error("oom", NULL); return -1; }
        c->seg[0].n = c->own.len;
        c->seg[0].type = base;
        c->seg[0].attrs = c->own.attrs;
        c->seg[0].esz = ray_col_src_esz(&c->own);
        c->seg[0].src = &c->own;
    } else if (col->type == RAY_MAPCOMMON) {
        ray_t** mp = (ray_t**)ray_data(col);
        ray_t* kv = mp[0];
        ray_t* rc = mp[1];
        if (!kv || !rc || RAY_IS_ERR(kv) || RAY_IS_ERR(rc) || rc->type != RAY_I64 ||
            !ray_is_vec(kv) || kv->len < rc->len || !cdb_type_ok(kv->type, is_val))
            return 0;
        base = kv->type;
        n = rc->len;
        c->seg = (cdb_seg_t*)ray_calloc_raw((size_t)(n > 0 ? n : 1) * sizeof(cdb_seg_t));
        if (!c->seg) { *err = ray_error("oom", NULL); return -1; }
        const int64_t* counts = (const int64_t*)ray_data(rc);
        for (int64_t p = 0; p < n; p++) {
            c->seg[p].n = counts[p] > 0 ? counts[p] : 0;
            c->seg[p].type = base;
            c->seg[p].fill = cdb_cell(ray_data(kv), p, kv->type, kv->attrs);
        }
    } else if (RAY_IS_PARTED(col->type)) {
        base = (int8_t)RAY_PARTED_BASETYPE(col->type);
        if (!cdb_type_ok(base, is_val)) return 0;
        ray_t** segs = (ray_t**)ray_data(col);
        int64_t ncnt = 0;
        const int64_t* cnt = parted_table_counts(tbl, &ncnt);
        n = col->len;
        c->seg = (cdb_seg_t*)ray_calloc_raw((size_t)(n > 0 ? n : 1) * sizeof(cdb_seg_t));
        if (!c->seg) { *err = ray_error("oom", NULL); return -1; }
        for (int64_t s = 0; s < n; s++) {
            ray_t* sg = segs[s];
            cdb_seg_t* d = &c->seg[s];
            d->type = base;
            if (sg && !RAY_IS_ERR(sg)) {
                if (sg->type != base || !ray_is_vec(sg)) { cdb_col_close(c); return 0; }
                d->data = sg->len > 0 ? ray_data(sg) : NULL;
                d->n = sg->len;
                d->attrs = sg->attrs;
                d->esz = ray_sym_elem_size(base, sg->attrs);
                d->cold = d->data && ray_col_cold(sg);
                if (!d->data) d->fill = 0;   /* no rows */
            } else {
                d->n = parted_seg_rows(segs, s, cnt, ncnt);
                d->fill = cdb_null_cell(base);
            }
        }
    } else {
        if (!ray_is_vec(col) || !cdb_type_ok(col->type, is_val)) return 0;
        base = col->type;
        n = 1;
        c->seg = (cdb_seg_t*)ray_calloc_raw(sizeof(cdb_seg_t));
        if (!c->seg) { *err = ray_error("oom", NULL); return -1; }
        c->seg[0].data = col->len > 0 ? ray_data(col) : NULL;
        c->seg[0].n = col->len;
        c->seg[0].type = base;
        c->seg[0].attrs = col->attrs;
        c->seg[0].esz = ray_sym_elem_size(base, col->attrs);
        c->seg[0].cold = c->seg[0].data && ray_col_cold(col);
    }
    c->nseg = n;
    int64_t lo = 0;
    for (int64_t s = 0; s < n; s++) { c->seg[s].lo = lo; lo += c->seg[s].n; }
    c->rows = lo;
    switch (base) {
    case RAY_I16: case RAY_I32: case RAY_DATE: case RAY_TIME:
        c->narrow = true;
        break;
    case RAY_BOOL: case RAY_U8: case RAY_SYM:
        c->narrow = true;
        for (int64_t s = 0; s < n && c->narrow; s++) {
            const cdb_seg_t* d = &c->seg[s];
            if (d->data || d->src ? d->esz > 4 : (uint64_t)d->fill > UINT32_MAX) c->narrow = false;
        }
        break;
    default:
        c->narrow = false;
    }
    return 1;
}

bool ray_cd_bounded_col_ok(ray_t* col, bool is_val) {
    if (!col || RAY_IS_ERR(col)) return false;
    if (col->type == RAY_ENCCOL) {   /* from metadata: a single-file encoded column */
        ray_col_meta_t m;
        return ray_col_meta_of(col, &m) && !RAY_IS_PARTED(m.type) && cdb_type_ok(m.type, is_val);
    }
    if (col->type == RAY_MAPCOMMON) {
        ray_t* kv = ((ray_t**)ray_data(col))[0];
        return kv && !RAY_IS_ERR(kv) && ray_is_vec(kv) && cdb_type_ok(kv->type, is_val);
    }
    if (RAY_IS_PARTED(col->type))
        return cdb_type_ok((int8_t)RAY_PARTED_BASETYPE(col->type), is_val);
    return ray_is_vec(col) && cdb_type_ok(col->type, is_val);
}

/* The segment holding global row r (r < rows). */
static int64_t cdb_seek(const cdb_col_t* c, int64_t r) {
    int64_t a = 0, b = c->nseg - 1;
    while (a < b) {
        int64_t m = (a + b + 1) / 2;
        if (c->seg[m].lo <= r) a = m; else b = m - 1;
    }
    return a;
}

/* Cells [off, off + m) of a plain run of `type` at element size esz. */
static void cdb_cells(const void* data, int8_t type, uint8_t esz, int64_t off, int64_t m, int64_t* out) {
    if (type == RAY_F64) {
        const double* d = (const double*)data + off;
        for (int64_t i = 0; i < m; i++) out[i] = canon_f64_key(d[i]);
    } else if (esz == 8) {
        memcpy(out, (const int64_t*)data + off, (size_t)m * sizeof(int64_t));
    } else if (esz == 4) {
        if (type == RAY_SYM) {
            const uint32_t* d = (const uint32_t*)data + off;
            for (int64_t i = 0; i < m; i++) out[i] = d[i];
        } else {
            const int32_t* d = (const int32_t*)data + off;
            for (int64_t i = 0; i < m; i++) out[i] = d[i];
        }
    } else if (esz == 2) {
        if (type == RAY_SYM) {
            const uint16_t* d = (const uint16_t*)data + off;
            for (int64_t i = 0; i < m; i++) out[i] = d[i];
        } else {
            const int16_t* d = (const int16_t*)data + off;
            for (int64_t i = 0; i < m; i++) out[i] = d[i];
        }
    } else {
        const uint8_t* d = (const uint8_t*)data + off;
        for (int64_t i = 0; i < m; i++) out[i] = d[i];
    }
}

/* Cells of rows [r, r + n) into out[], from segment *si on; an encoded
 * segment through the caller's cursor (one per task, so a vector is
 * decoded once a task).  NULL or an owned error (a range read's). */
static ray_t* cdb_decode(const cdb_col_t* c, int64_t* si, int64_t r, int64_t n, int64_t* out,
                         ray_col_cursor_t* cur) {
    int64_t s = *si;
    while (n > 0) {
        while (r >= c->seg[s].lo + c->seg[s].n) s++;
        const cdb_seg_t* g = &c->seg[s];
        int64_t off = r - g->lo, m = g->n - off;
        if (m > n) m = n;
        if (g->src) {
            const ray_col_range_t* v;
            ray_t* err = ray_col_cursor_at(cur, off, &v);
            if (err) return err;
            int64_t vm = v->start + v->n - off;
            if (vm < m) m = vm;
            cdb_cells(v->data, v->type, ray_sym_elem_size(v->type, v->attrs), off - v->start, m, out);
        } else if (!g->data) {
            for (int64_t i = 0; i < m; i++) out[i] = g->fill;
        } else {
            cdb_cells(g->data, g->type, g->esz, off, m, out);
        }
        out += m; r += m; n -= m;
    }
    *si = s;
    return NULL;
}

/* Ask for the cold segments' bytes of rows [r0, r1). */
static void cdb_want(const cdb_col_t* c, int64_t r0, int64_t r1) {
    if (r0 >= r1) return;
    for (int64_t s = cdb_seek(c, r0); s < c->nseg && c->seg[s].lo < r1; s++) {
        const cdb_seg_t* g = &c->seg[s];
        if (!g->cold || !g->data) continue;
        int64_t a = r0 > g->lo ? r0 - g->lo : 0;
        int64_t b = r1 < g->lo + g->n ? r1 - g->lo : g->n;
        if (b > a)
            ray_vm_advise_willneed((char*)g->data + (size_t)a * g->esz,
                                   (size_t)(b - a) * g->esz);
    }
}

/* Ask for rows [r0, r1) of the columns the kernel reads — with a
 * selection, only the runs of morsels it keeps rows of. */
static void cdb_want_rows(const cdb_ctx_t* c, int64_t r0, int64_t r1) {
    if (!c->sel_flg) {
        if (!c->row_gid && !c->one_group) cdb_want(&c->key, r0, r1);
        cdb_want(&c->val, r0, r1);
        return;
    }
    int64_t m = r0 / RAY_MORSEL_ELEMS, m_end = (r1 + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    while (m < m_end) {
        while (m < m_end && (m >= c->sel_nsegs || c->sel_flg[m] == RAY_SEL_NONE)) m++;
        int64_t run = m;
        while (m < m_end && m < c->sel_nsegs && c->sel_flg[m] != RAY_SEL_NONE) m++;
        if (m > run) {
            int64_t a = run * RAY_MORSEL_ELEMS > r0 ? run * RAY_MORSEL_ELEMS : r0;
            int64_t b = m * RAY_MORSEL_ELEMS < r1 ? m * RAY_MORSEL_ELEMS : r1;
            if (!c->row_gid && !c->one_group) cdb_want(&c->key, a, b);
            cdb_want(&c->val, a, b);
        }
    }
}

static inline int64_t cdb_gid(const cdb_ctx_t* c, int64_t k) {
    uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    uint64_t s = h & c->kmask;
    for (;;) {
        uint32_t t = c->ktag[s];
        if (!t) return -1;
        if (c->kkeys[s] == k) return (int64_t)t - 1;
        s = (s + 1) & c->kmask;
    }
}

/* Phase A: task t of the chunk takes rows [chunk_r0 + t*S, +S), keeps the
 * pairs whose partition is in [lo, hi) and lays them out in its region of
 * the record buffer partition by partition (toff[t][p] .. toff[t][p+1]). */
static void cdb_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    cdb_ctx_t* c = (cdb_ctx_t*)vctx;
    int64_t kbuf[RAY_MORSEL_ELEMS], xbuf[RAY_MORSEL_ELEMS];
    uint32_t hist[CDB_MAX_PARTS];
    cdb_recent_t recent[CDB_CACHE];
    static const int64_t zero_gid[RAY_MORSEL_ELEMS];   /* one_group: every row's group */
    const uint32_t P = c->P, lo = c->lo, hi = c->hi, pshift = c->pshift;
    const uint64_t vmask = c->vmask, ng = (uint64_t)c->n_groups;
    /* the encoded columns' cursors, on this task's stack */
    ray_col_fixed_t kfix, xfix;
    ray_col_cursor_t kcur, xcur;
    const bool kenc = c->key.bound, xenc = c->val.bound;
    if (kenc) ray_col_cursor_init_lent(&kcur, &c->key.own, &kfix);
    if (xenc) ray_col_cursor_init_lent(&xcur, &c->val.own, &xfix);
    for (int64_t t = start; t < end; t++) {
        int64_t r0 = c->chunk_r0 + t * c->S;
        int64_t r1 = r0 + c->S;
        if (r1 > c->nrows) r1 = c->nrows;
        if ((kenc || xenc) && ray_scan_failed(&c->fail)) {   /* the round is abandoned */
            atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
            continue;
        }
        ray_t* rerr = NULL;
        cdb_rec_t* st = c->stage + (size_t)wid * c->S;
        uint16_t*  sp = c->spart + (size_t)wid * c->S;
        memset(hist, 0, (size_t)P * sizeof(uint32_t));
        memset(recent, 0, sizeof(recent));
        int64_t ns = 0;
        int64_t ksi = c->row_gid || c->one_group ? 0 : cdb_seek(&c->key, r0);
        int64_t xsi = cdb_seek(&c->val, r0);
        int64_t klast = 0, glast = -2;
        for (int64_t m0 = r0; m0 < r1 && !rerr; m0 += RAY_MORSEL_ELEMS) {
            int64_t mn = r1 - m0 < RAY_MORSEL_ELEMS ? r1 - m0 : RAY_MORSEL_ELEMS;
            const uint16_t* idx = NULL;
            int64_t ni = mn;
            if (c->sel_flg) {
                int64_t seg = m0 / RAY_MORSEL_ELEMS;
                uint8_t f = seg < c->sel_nsegs ? c->sel_flg[seg] : RAY_SEL_NONE;
                if (f == RAY_SEL_NONE) continue;
                if (f != RAY_SEL_ALL) {
                    idx = c->sel_idx + c->sel_offs[seg];
                    ni = c->sel_offs[seg + 1] - c->sel_offs[seg];
                }
            }
            const int64_t* gb;
            if (c->one_group) {
                gb = zero_gid;
            } else if (c->row_gid) {
                gb = c->sel_pre ? c->row_gid + c->sel_pre[m0 / RAY_MORSEL_ELEMS] : c->row_gid + m0;
            } else {
                if ((rerr = cdb_decode(&c->key, &ksi, m0, mn, kbuf, kenc ? &kcur : NULL)) != NULL) break;
                for (int64_t i = 0; i < mn; i++) {
                    int64_t k = kbuf[i];
                    if (k != klast || glast == -2) { klast = k; glast = cdb_gid(c, k); }
                    kbuf[i] = glast;
                }
                gb = kbuf;
            }
            if ((rerr = cdb_decode(&c->val, &xsi, m0, mn, xbuf, xenc ? &xcur : NULL)) != NULL) break;
            for (int64_t j = 0; j < ni; j++) {
                int64_t i = idx ? idx[j] : j;
                uint64_t g = (uint64_t)(c->sel_pre ? gb[j] : gb[i]);
                if (g >= ng) continue;             /* no group (negative too) */
                g++;
                int64_t x = (int64_t)((uint64_t)xbuf[i] & vmask);
                uint64_t h = cdb_hash(x, g);
                uint32_t p = (uint32_t)(h >> pshift);
                if (p < lo || p >= hi) continue;
                cdb_recent_t* e = &recent[(h >> 32) & (CDB_CACHE - 1)];
                if (e->g == g && e->x == x) continue;
                e->g = g; e->x = x;
                st[ns].x = x; st[ns].g = (uint32_t)g; st[ns].h = (uint32_t)h;
                sp[ns] = (uint16_t)p;
                hist[p]++;
                ns++;
            }
        }
        if (rerr) {
            ray_scan_fail_note(&c->fail, r0, rerr);
            atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
            continue;
        }
        int64_t* to = c->toff + (size_t)t * (P + 1);
        int64_t base = t * c->S;
        for (uint32_t p = 0; p < P; p++) {
            to[p] = base;
            uint32_t k = hist[p];
            hist[p] = (uint32_t)(base - t * c->S);
            base += k;
        }
        to[P] = base;
        cdb_rec_t* region = c->recs + t * c->S;
        for (int64_t i = 0; i < ns; i++) region[hist[sp[i]]++] = st[i];
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
    if (kenc) ray_col_cursor_free(&kcur);
    if (xenc) ray_col_cursor_free(&xcur);
}

/* Slot layouts: values of VT, tags of GT — 4-byte values for a narrow
 * column, 2-byte tags below 65535 groups.  Rehash fills a fresh block
 * (from the old one when it grew), insert adds a run of records (the main
 * thread sized the block for all of them at load <= 3/4), count adds each
 * occupied slot to its group's count. */
#define CDB_SET_FNS(SFX, VT, GT)                                               \
static void cdb_rehash_##SFX(cdb_set_t* s) {                                   \
    VT* nx = (VT*)s->blk;                                                      \
    GT* ng = (GT*)((char*)s->blk + s->cap * sizeof(VT));                       \
    memset(ng, 0, s->cap * sizeof(GT));                                        \
    if (!s->old) return;                                                       \
    const VT* ox = (const VT*)s->old;                                          \
    const GT* og = (const GT*)((const char*)s->old + s->ocap * sizeof(VT));    \
    uint64_t cap = s->cap;                                                     \
    for (uint64_t i = 0; i < s->ocap; i++) {                                   \
        GT g = og[i];                                                          \
        if (!g) continue;                                                      \
        uint64_t slot = cdb_slot((uint32_t)cdb_hash((int64_t)ox[i], g), cap);  \
        while (ng[slot]) if (++slot == cap) slot = 0;                          \
        ng[slot] = g;                                                          \
        nx[slot] = ox[i];                                                      \
    }                                                                          \
}                                                                              \
static int64_t cdb_insert_##SFX(cdb_set_t* s, const cdb_rec_t* r, int64_t n) {  \
    VT* xs = (VT*)s->blk;                                                      \
    GT* gs = (GT*)((char*)s->blk + s->cap * sizeof(VT));                       \
    uint64_t cap = s->cap;                                                     \
    int64_t have = s->n;                                                       \
    int64_t room = (int64_t)(cap * 3 / 4);  /* load the set may reach */       \
    int64_t i = 0;                                                             \
    for (; i < n; i++) {                                                       \
        if (i + 8 < n) {                                                       \
            uint64_t ps = cdb_slot(r[i + 8].h, cap);                           \
            __builtin_prefetch(&gs[ps]);                                       \
            __builtin_prefetch(&xs[ps]);                                       \
        }                                                                      \
        GT g = (GT)r[i].g;                                                     \
        VT x = (VT)r[i].x;                                                     \
        uint64_t slot = cdb_slot(r[i].h, cap);                                 \
        for (;;) {                                                             \
            GT cur = gs[slot];                                                 \
            if (!cur) break;                                                   \
            if (cur == g && xs[slot] == x) break;                              \
            if (++slot == cap) slot = 0;                                       \
        }                                                                      \
        if (gs[slot]) continue;                /* seen before */               \
        if (have >= room) break;               /* full: grow, then go on */    \
        gs[slot] = g;                                                          \
        xs[slot] = x;                                                          \
        have++;                                                                \
    }                                                                          \
    s->n = have;                                                               \
    return i;                                                                  \
}                                                                              \
static void cdb_count_##SFX(const cdb_set_t* s, int64_t* cnt, bool shared) {   \
    const GT* gs = (const GT*)((const char*)s->blk + s->cap * sizeof(VT));     \
    if (shared) {                                                              \
        for (uint64_t i = 0; i < s->cap; i++)                                  \
            if (gs[i]) __atomic_fetch_add(&cnt[gs[i] - 1], 1, __ATOMIC_RELAXED); \
    } else {                                                                   \
        for (uint64_t i = 0; i < s->cap; i++)                                  \
            if (gs[i]) cnt[gs[i] - 1]++;                                       \
    }                                                                          \
}

CDB_SET_FNS(v8g4, int64_t, uint32_t)
CDB_SET_FNS(v8g2, int64_t, uint16_t)
CDB_SET_FNS(v4g4, uint32_t, uint32_t)
CDB_SET_FNS(v4g2, uint32_t, uint16_t)
#undef CDB_SET_FNS

static void cdb_rehash(int var, cdb_set_t* s) {
    switch (var) {
    case 0: cdb_rehash_v8g4(s); break;
    case 1: cdb_rehash_v8g2(s); break;
    case 2: cdb_rehash_v4g4(s); break;
    default: cdb_rehash_v4g2(s); break;
    }
}
static int64_t cdb_insert(int var, cdb_set_t* s, const cdb_rec_t* r, int64_t n) {
    switch (var) {
    case 0: return cdb_insert_v8g4(s, r, n);
    case 1: return cdb_insert_v8g2(s, r, n);
    case 2: return cdb_insert_v4g4(s, r, n);
    default: return cdb_insert_v4g2(s, r, n);
    }
}
static void cdb_count(int var, const cdb_set_t* s, int64_t* cnt, bool shared) {
    switch (var) {
    case 0: cdb_count_v8g4(s, cnt, shared); break;
    case 1: cdb_count_v8g2(s, cnt, shared); break;
    case 2: cdb_count_v4g4(s, cnt, shared); break;
    default: cdb_count_v4g2(s, cnt, shared); break;
    }
}

/* Phase B: task i adds the chunk's records of partition plist[i] to its
 * set, from where it stopped last, until they are all in or the set reaches
 * load 3/4 (`full`: the main thread grows it and dispatches it again). */
static void cdb_insert_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    cdb_ctx_t* c = (cdb_ctx_t*)vctx;
    for (int64_t i = start; i < end; i++) {
        uint32_t p = c->plist[i];
        cdb_set_t* s = &c->sets[p];
        if (s->fresh) { cdb_rehash(c->var, s); s->fresh = false; }
        s->full = false;
        int64_t before = s->n;
        for (uint32_t t = s->rt; t < c->T_c; t++, s->ri = 0) {
            const int64_t* to = c->toff + (size_t)t * (c->P + 1);
            int64_t n = to[p + 1] - to[p] - s->ri;
            if (n <= 0) continue;
            int64_t took = cdb_insert(c->var, s, c->recs + to[p] + s->ri, n);
            s->seen += took;
            if (took < n) { s->rt = t; s->ri += took; s->full = true; break; }
        }
        if (!s->full) { s->rt = c->T_c; s->ri = 0; }
        s->added += s->n - before;
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
}

/* End of a round: each finished partition's pairs, counted per group. */
static void cdb_count_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    cdb_ctx_t* c = (cdb_ctx_t*)vctx;
    int64_t* cnt = c->cnt_shared ? c->cnt : c->cnt + (size_t)wid * (size_t)c->n_groups;
    for (int64_t i = start; i < end; i++) {
        const cdb_set_t* s = &c->sets[c->lo + (uint32_t)i];
        if (s->blk) cdb_count(c->var, s, cnt, c->cnt_shared);
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
}

/* Run fn over n tasks: on the pool, or here.  False when tasks were
 * skipped (a cancel). */
static bool cdb_run(ray_pool_t* pool, bool par, ray_pool_fn fn, cdb_ctx_t* c, uint32_t n) {
    if (n == 0) return true;
    atomic_store_explicit(&c->done, 0, memory_order_relaxed);
    if (par) ray_pool_dispatch_n(pool, fn, c, n);
    else     fn(c, 0, 0, n);
    if (atomic_load_explicit(&c->done, memory_order_relaxed) != (int64_t)n) return false;
    if (ray_interrupted()) return false;
    if (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) return false;
    return true;
}

static void cdb_free_set(cdb_set_t* s) {
    ray_free_raw(s->blk);
    ray_free_raw(s->old);
    memset(s, 0, sizeof(*s));
}

/* What the anon watermark leaves: the room under it, the committed blocks
 * the calling heap can reuse added back (a heap keeps the pools a query
 * freed; counted as taken they made a repeat of the same query look short of
 * memory).  INT64_MAX when the RAM is unknown. */
static int64_t cdb_room(void) {
    int64_t wm = ray_heap_anon_watermark();
    if (wm <= 0) return INT64_MAX;
    return wm - ray_heap_anon_committed() + ray_heap_anon_reusable();
}

/* The bytes the kernel may give its sets: three quarters of the room, and
 * when that is less, a sixteenth of the watermark or a quarter of the
 * slices' row buffers (`demand`), whichever is less, CDB_MIN_BUDGET at
 * least — a room used up by other work does not shrink the sets to nothing
 * (one round a partition). */
static int64_t cdb_budget(int64_t demand) {
    int64_t room = cdb_room();
    if (room == INT64_MAX) return INT64_MAX;
    int64_t b = room / 4 * 3;
    int64_t floor = ray_heap_anon_watermark() / 16;
    if (floor > demand / 4) floor = demand / 4;
    if (floor < CDB_MIN_BUDGET) floor = CDB_MIN_BUDGET;
    return b > floor ? b : floor;
}

/* Up to `cap` row indices spread evenly over the rows `sel` keeps; *pop
 * receives how many it keeps.  Returns the count. */
static int64_t cdb_sel_sample(ray_t* sel, int64_t nrows, int64_t* out, int64_t cap, int64_t* pop) {
    ray_rowsel_t* sm = ray_rowsel_meta(sel);
    const uint8_t* flg = ray_rowsel_flags(sel);
    const uint32_t* offs = ray_rowsel_offsets(sel);
    const uint16_t* idx = ray_rowsel_idx(sel);
    int64_t total = sm->total_pass;
    *pop = total;
    if (total <= 0) return 0;
    int64_t step = total / cap + (total % cap != 0);
    int64_t j = 0, next = 0, n = 0;   /* j: kept rows before this morsel */
    for (int64_t m = 0; m < (int64_t)sm->n_segs && n < cap; m++) {
        int64_t lo = m * RAY_MORSEL_ELEMS, hi = lo + RAY_MORSEL_ELEMS;
        if (hi > nrows) hi = nrows;
        if (flg[m] == RAY_SEL_NONE) continue;
        int64_t k = flg[m] == RAY_SEL_ALL ? hi - lo : (int64_t)(offs[m + 1] - offs[m]);
        while (next < j + k && n < cap) {
            int64_t at = next - j;
            out[n++] = lo + (flg[m] == RAY_SEL_ALL ? at : idx[offs[m] + at]);
            next += step;
        }
        j += k;
    }
    return n;
}

/* A partition both parted columns have rows in, the largest: the sample the
 * route reads them through.  -1 when there is none. */
static int64_t cdb_parted_rep(ray_t* key, ray_t* val) {
    ray_t** ks = (ray_t**)ray_data(key);
    ray_t** vs = (ray_t**)ray_data(val);
    int64_t best = -1, rows = 0;
    for (int64_t s = 0; s < key->len && s < val->len; s++) {
        ray_t* a = ks[s];
        ray_t* b = vs[s];
        if (!a || !b || RAY_IS_ERR(a) || RAY_IS_ERR(b) || a->len != b->len) continue;
        if (a->len > rows) { rows = a->len; best = s; }
    }
    return best;
}

bool ray_cd_bounded_preferred(ray_t* tbl, ray_t* key, ray_t* val, ray_t* sel,
                              int64_t nrows, int64_t n_groups) {
    if (nrows <= 0 || n_groups <= 0) return false;
    bool kp = key && (RAY_IS_PARTED(key->type) || key->type == RAY_MAPCOMMON);
    bool vp = val && (RAY_IS_PARTED(val->type) || val->type == RAY_MAPCOMMON);
    bool ke = key && key->type == RAY_ENCCOL, ve = val && val->type == RAY_ENCCOL;   /* decoded whole by the slices */
    /* Memory first: the slices' row buffers — row map and index, 16 B a
     * row, or row map and scattered pairs past 50 K groups, 24 — and the
     * flat copies they make of parted columns, 8 B a row each, against half
     * of the room under the anon watermark.  Past it they spill, and the
     * gathers over them and the inputs turn into rewrites of the spill
     * files: the bounded kernel, whatever the data. */
    int64_t wm = ray_heap_anon_watermark();
    if (wm > 0) {
        double need = (double)nrows * ((n_groups <= 50000 ? 16.0 : 24.0) + (kp || ke ? 8 : 0) + (vp || ve ? 8 : 0));
        /* the reusable blocks (a freelist walk) only when the room without
         * them is short */
        if (need > (double)(wm - ray_heap_anon_committed()) / 2 &&
            need > (double)cdb_room() / 2) return true;
    }
    if (n_groups > 50000 || nrows < CDB_MIN_ROWS || !key || !val) return false;
    /* In memory both fit; the slices dedup each group in cache but serially
     * over a group, the bounded kernel spreads every pair over the workers
     * but holds the distinct pairs in sets read at random.  The bounded
     * kernel where one group holds more than CDB_SKEW workers' shares of the
     * rows (1.4x to 4x faster on 48 workers over 100 M rows, 3 groups or
     * 9 K with one of 18%) or the pairs dedup to a quarter of the rows
     * (6.5x, 2 K values a group); the slices for even groups of nearly
     * distinct pairs (1.6x faster there: 100 M rows, 9 K groups, 95 M
     * pairs).  A column read from storage is read here in order where the
     * slices gather it at the rows of each group.  Sampled estimates
     * (agg_group_card_sample) — over the rows a selection keeps, and over
     * one partition of a parted pair — from CDB_SAMPLE_ROWS rows: the
     * sample's scattered reads take some 6 ms, a quarter of a 10 M-row count
     * the slices finish in 25 ms. */
    ray_t* cols[2] = { key, val };
    int64_t srows = nrows, scale_num = 1, scale_den = 1;
    if (kp || vp) {
        if (!(RAY_IS_PARTED(key->type) && RAY_IS_PARTED(val->type))) return false;
        int64_t r = cdb_parted_rep(key, val);
        if (r < 0) return false;
        cols[0] = ((ray_t**)ray_data(key))[r];
        cols[1] = ((ray_t**)ray_data(val))[r];
        srows = cols[0]->len;
        scale_num = nrows;
        scale_den = srows > 0 ? srows : 1;
        sel = NULL;   /* a selection is over the whole table's rows */
    }
    /* an encoded column is not in memory as values either: it is read in
     * order, a vector at a time, where the slices would decode it whole */
    if (cols[0]->type == RAY_ENCCOL || cols[1]->type == RAY_ENCCOL) return true;
    if (ray_col_cold(cols[0]) || ray_col_cold(cols[1])) return true;
    if (nrows < CDB_SAMPLE_ROWS) return false;
    int64_t kmax = 0, pairs = 0;
    bool ok;
    if (sel) {
        int64_t cap = AGG_EST_SAMPLE_ROWS * 2, pop = 0;
        int64_t* rows = (int64_t*)ray_alloc_raw((size_t)cap * sizeof(int64_t));
        if (!rows) return false;
        int64_t n = cdb_sel_sample(sel, nrows, rows, cap, &pop);
        ok = n > 0 && agg_group_card_sample(cols, 2, 1, rows, n, pop, NULL, NULL, &kmax, &pairs);
        ray_free_raw(rows);
        if (!ok) return false;
        nrows = pop;
    } else {
        ok = agg_group_card_sample(cols, 2, 1, NULL, srows, 0, NULL, NULL, &kmax, &pairs);
        if (!ok) return false;
        kmax = (int64_t)((double)kmax * scale_num / scale_den);
        pairs = (int64_t)((double)pairs * scale_num / scale_den);
    }
    ray_pool_t* pool = ray_pool_get();
    int64_t workers = pool ? ray_pool_total_workers(pool) : 1;
    if (workers > 1 && kmax > nrows / workers * CDB_SKEW) return true;
    return pairs <= nrows / 4;
}

/* The bytes of the buddy block that holds `n` pairs at load 3/4: twice
 * `prev` at least (a power of two, header included). */
static uint64_t cdb_block_for(uint64_t prev, uint64_t n, size_t sb) {
    uint64_t nb = prev ? prev * 2 : CDB_SET_MIN_BYTES;
    while ((nb - 32) / sb * 3 < n * 4) nb <<= 1;
    return nb;
}

/* The pairs to size a full set for: its pairs, and the share of its records
 * so far that were new over the records it has left (a quarter over); for a
 * set without a block, the pairs a partition ended the last round with. */
static uint64_t cdb_want_pairs(const cdb_set_t* s, int64_t expect) {
    if (!s->blk) return expect > 0 ? (uint64_t)expect : 0;
    double rate = (double)(s->added + 1) / (double)(s->seen + 1);
    return (uint64_t)s->n + (uint64_t)(rate * (double)(s->inc - s->seen) * 1.25) + 16;
}

/* ray_cd_bounded's body; one_group: no key, every row is group 0 of 1. */
static ray_t* cdb_bounded_run(ray_t* tbl, ray_t* key_col, ray_t* grp_keys,
                        const int64_t* row_gid, int64_t n_groups,
                        ray_t* val_col, ray_t* sel, int64_t nrows, bool one_group);

ray_t* ray_cd_bounded(ray_t* tbl, ray_t* key_col, ray_t* grp_keys,
                      const int64_t* row_gid, int64_t n_groups,
                      ray_t* val_col, ray_t* sel, int64_t nrows) {
    return cdb_bounded_run(tbl, key_col, grp_keys, row_gid, n_groups, val_col, sel, nrows, false);
}

ray_t* ray_cd_bounded_all(ray_t* val_col, int64_t nrows) {
    ray_t* r = cdb_bounded_run(NULL, NULL, NULL, NULL, 1, val_col, NULL, nrows, true);
    if (!r || RAY_IS_ERR(r)) return r;
    int64_t cnt = r->len > 0 ? ((const int64_t*)ray_data(r))[0] : 0;
    ray_release(r);
    return ray_i64(cnt);
}

static ray_t* cdb_bounded_run(ray_t* tbl, ray_t* key_col, ray_t* grp_keys,
                        const int64_t* row_gid, int64_t n_groups,
                        ray_t* val_col, ray_t* sel, int64_t nrows, bool one_group) {
    if (n_groups < 0 || nrows < 0) return NULL;
    if (n_groups >= (int64_t)UINT32_MAX - 1) return NULL;
    if (!row_gid && !one_group && (!grp_keys || RAY_IS_ERR(grp_keys) || !ray_is_vec(grp_keys) ||
                     !cdb_type_ok(grp_keys->type, false) || grp_keys->len < n_groups))
        return NULL;

    cdb_ctx_t c;
    memset(&c, 0, sizeof(c));
    ray_scan_fail_init(&c.fail);
    c.one_group = one_group;
    ray_t* oerr = NULL;
    int rc = cdb_col_open(&c.val, val_col, tbl, true, &oerr);
    if (rc <= 0) return oerr;
    if (c.val.rows < nrows) { cdb_col_close(&c.val); return NULL; }
    if (!row_gid && !one_group) {
        rc = cdb_col_open(&c.key, key_col, tbl, false, &oerr);
        if (rc <= 0) { cdb_col_close(&c.val); return oerr; }
        if (c.key.rows < nrows) { cdb_col_close(&c.key); cdb_col_close(&c.val); return NULL; }
    }

    ray_t* out = ray_vec_new(RAY_I64, n_groups);
    if (!out || RAY_IS_ERR(out)) {
        cdb_col_close(&c.key); cdb_col_close(&c.val);
        return out ? out : ray_error("oom", NULL);
    }
    out->len = n_groups;
    int64_t* odata = (int64_t*)ray_data(out);
    if (n_groups > 0) memset(odata, 0, (size_t)n_groups * sizeof(int64_t));
    if (n_groups == 0 || nrows == 0) {
        cdb_col_close(&c.key); cdb_col_close(&c.val);
        return out;
    }

    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, nrows, 2 * CDB_TASK_ROWS);
    uint32_t W = par ? ray_pool_total_workers(pool) : 1;
    uint32_t P = 256;
    while (P < CDB_MAX_PARTS && P < 8 * W) P <<= 1;
    uint32_t T = par ? 2 * W : 1;
    if (T > 512) T = 512;
    /* The record buffer and the staging (18 B a row of a task, T and W
     * tasks' worth) within a quarter of the budget: shorter tasks, then
     * fewer of them, down to one a worker. */
    int64_t budget_cap = (int64_t)((double)(sel ? ray_rowsel_meta(sel)->total_pass : nrows) * (n_groups <= 50000 ? 16.0 : 24.0));
    int64_t budget = cdb_budget(budget_cap);
    int64_t S = CDB_TASK_ROWS;
    while ((int64_t)(T + W) * S * 18 > budget / 4 && S > CDB_TASK_ROWS_MIN) S /= 2;
    while ((int64_t)(T + W) * S * 18 > budget / 4 && T > W) T--;
    int64_t chunk_rows = (int64_t)T * S;

    c.row_gid = row_gid;
    c.n_groups = n_groups;
    c.nrows = nrows;
    c.vmask = c.val.narrow ? UINT32_MAX : UINT64_MAX;
    c.P = P;
    c.pshift = 64 - (uint32_t)__builtin_ctz(P);
    c.T = T;
    c.S = S;
    c.var = (c.val.narrow ? 2 : 0) + (n_groups < 65535 ? 1 : 0);
    size_t sb = (c.val.narrow ? 4 : 8) + (n_groups < 65535 ? 2 : 4);   /* bytes a slot */
    if (sel) {
        ray_rowsel_t* sm = ray_rowsel_meta(sel);
        c.sel_flg = ray_rowsel_flags(sel);
        c.sel_offs = ray_rowsel_offsets(sel);
        c.sel_idx = ray_rowsel_idx(sel);
        c.sel_nsegs = sm->n_segs;
    }

    /* Working buffers: all sized by the workers, partitions and groups. */
    ray_t* err = NULL;
    void *khdr = NULL;
    uint32_t* plist = NULL;
    int64_t* selpre = NULL;
    uint64_t kcap = 16;
    bool by_key = !row_gid && !one_group;   /* rows map to groups through the key table */
    if (by_key) while (kcap < (uint64_t)n_groups * 2) kcap <<= 1;
    bool cnt_shared = (int64_t)W * n_groups > CDB_LOCAL_CNT;
    size_t cnt_cells = cnt_shared ? (size_t)n_groups : (size_t)W * (size_t)n_groups;
    c.cnt_shared = cnt_shared;
    c.recs  = (cdb_rec_t*)ray_alloc_raw((size_t)chunk_rows * sizeof(cdb_rec_t));
    c.stage = (cdb_rec_t*)ray_alloc_raw((size_t)W * (size_t)S * sizeof(cdb_rec_t));
    c.spart = (uint16_t*)ray_alloc_raw((size_t)W * (size_t)S * sizeof(uint16_t));
    c.toff  = (int64_t*)ray_alloc_raw((size_t)T * (P + 1) * sizeof(int64_t));
    c.sets  = (cdb_set_t*)ray_calloc_raw((size_t)P * sizeof(cdb_set_t));
    c.cnt   = (int64_t*)ray_calloc_raw(cnt_cells * sizeof(int64_t));
    plist   = (uint32_t*)ray_alloc_raw((size_t)P * sizeof(uint32_t));
    if (by_key) khdr = ray_calloc_raw((size_t)kcap * (sizeof(int64_t) + sizeof(uint32_t)));
    if (!c.recs || !c.stage || !c.spart || !c.toff || !c.sets || !c.cnt || !plist ||
        (by_key && !khdr)) {
        err = ray_error("oom", NULL);
        goto done;
    }
    c.plist = plist;
    if (row_gid && c.sel_flg) {
        /* a row map over the kept rows: rank of the first kept row of each segment */
        selpre = (int64_t*)ray_alloc_raw((size_t)(c.sel_nsegs + 1) * sizeof(int64_t));
        if (!selpre) { err = ray_error("oom", NULL); goto done; }
        int64_t run = 0;
        for (int64_t s = 0; s < c.sel_nsegs; s++) {
            selpre[s] = run;
            if (c.sel_flg[s] == RAY_SEL_ALL) {
                int64_t lo = s * RAY_MORSEL_ELEMS;
                run += nrows - lo < RAY_MORSEL_ELEMS ? nrows - lo : RAY_MORSEL_ELEMS;
            } else if (c.sel_flg[s] != RAY_SEL_NONE) {
                run += c.sel_offs[s + 1] - c.sel_offs[s];
            }
        }
        selpre[c.sel_nsegs] = run;
        c.sel_pre = selpre;
    }
    if (by_key) {
        int64_t* kk = (int64_t*)khdr;
        uint32_t* kt = (uint32_t*)(kk + kcap);
        uint64_t kmask = kcap - 1;
        const void* gd = ray_data(grp_keys);
        for (int64_t gi = 0; gi < n_groups; gi++) {
            int64_t k = read_col_i64(gd, gi, grp_keys->type, grp_keys->attrs);
            uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t s = h & kmask;
            while (kt[s] && kk[s] != k) s = (s + 1) & kmask;
            if (!kt[s]) { kt[s] = (uint32_t)(gi + 1); kk[s] = k; }   /* a repeat keeps the first */
        }
        c.kkeys = kk;
        c.ktag = kt;
        c.kmask = kmask;
    }

    int64_t overhead = (int64_t)chunk_rows * (int64_t)sizeof(cdb_rec_t) +
                       (int64_t)W * S * (int64_t)(sizeof(cdb_rec_t) + 2) +
                       (int64_t)cnt_cells * 8 + (int64_t)kcap * 12;
    if (budget != INT64_MAX) {
        int64_t floor = budget / 4;   /* the sets keep three quarters at least */
        budget = budget - overhead > floor ? budget - overhead : floor;
    }
    /* Past CDB_MAX_ROUNDS rounds' worth of pairs the sets may pass the
     * budget, up to what the slices would have taken; the first trim
     * projects the round's sets over the rows left. */
    bool projected = budget == INT64_MAX;
    bool want = ray_pool_scan_on() && nrows > 0;
    int64_t ra_rows = CDB_RA_BYTES / 12;
    if (ra_rows < 2 * chunk_rows) ra_rows = 2 * chunk_rows;

    uint32_t lo = 0, hi = P;
    int64_t expect = 0;   /* pairs a partition ended the last round with */
    while (lo < P) {
        c.lo = lo;
        c.hi = hi;
        int64_t live = 0;     /* bytes of the round's set blocks */
        int64_t ra_hi = 0;
        for (int64_t r0 = 0; r0 < nrows; r0 += chunk_rows) {
            int64_t r1 = r0 + chunk_rows < nrows ? r0 + chunk_rows : nrows;
            if (want) {
                int64_t ra = r1 + ra_rows < nrows ? r1 + ra_rows : nrows;
                if (ra > ra_hi) {
                    cdb_want_rows(&c, ra_hi > r0 ? ra_hi : r0, ra);
                    ra_hi = ra;
                }
            }
            c.chunk_r0 = r0;
            c.T_c = (uint32_t)((r1 - r0 + S - 1) / S);
            bool ran = cdb_run(pool, par, cdb_scatter_fn, &c, c.T_c);
            if (ray_scan_failed(&c.fail)) { err = ray_scan_fail_finish(&c.fail, NULL); goto done; }
            if (!ran) {
                err = ray_error("cancel", "interrupted");
                goto done;
            }
            /* The partitions with records in this chunk; a set without a
             * block gets one first. */
            uint32_t np = 0;
            for (uint32_t p = lo; p < c.hi; p++) {
                cdb_set_t* s = &c.sets[p];
                int64_t inc = 0;
                for (uint32_t t = 0; t < c.T_c; t++) {
                    const int64_t* to = c.toff + (size_t)t * (P + 1);
                    inc += to[p + 1] - to[p];
                }
                s->inc = inc; s->seen = s->added = 0; s->rt = 0; s->ri = 0;
                s->full = inc > 0 && !s->blk;
                if (inc > 0) plist[np++] = p;
            }
            /* Phase B, then the sets that filled grow and go on.  A set grows
             * to twice its block at least, and to the pairs its records so
             * far promise for the rest of them.  Growth that would pass the
             * budget frees the round's top partitions instead (a later round
             * counts them). */
            while (np > 0) {
                int64_t grow = 0;
                for (uint32_t i = 0; i < np; i++) {
                    cdb_set_t* s = &c.sets[plist[i]];
                    if (s->full) grow += (int64_t)cdb_block_for(s->bytes, cdb_want_pairs(s, expect), sb);
                }
                if (live + grow > budget && c.hi - lo > 1) {
                    if (!projected) {
                        /* the sets this round is heading for, over the rows left */
                        double rows_in = (double)r1;
                        double total = (double)(live + grow) * ((double)nrows / rows_in);
                        if (total > (double)budget * CDB_MAX_ROUNDS) {
                            int64_t nb = (int64_t)(total / CDB_MAX_ROUNDS);
                            if (nb > budget_cap) nb = budget_cap;
                            if (nb > budget) budget = nb;
                        }
                        projected = true;
                    }
                    bool trimmed = false;
                    while (live + grow > budget && c.hi - lo > 1) {
                        uint32_t p = c.hi - 1;
                        cdb_set_t* s = &c.sets[p];
                        if (s->full) grow -= (int64_t)cdb_block_for(s->bytes, cdb_want_pairs(s, expect), sb);
                        live -= (int64_t)s->bytes;
                        cdb_free_set(s);
                        c.hi--;
                        trimmed = true;
                    }
                    if (trimmed) {
                        uint32_t k = 0;
                        for (uint32_t i = 0; i < np; i++) if (plist[i] < c.hi) plist[k++] = plist[i];
                        np = k;
                        ray_profile_tick("count-distinct: partitions left to the next round");
                    }
                }
                for (uint32_t i = 0; i < np; i++) {
                    cdb_set_t* s = &c.sets[plist[i]];
                    if (!s->full) continue;
                    uint64_t nb = cdb_block_for(s->bytes, cdb_want_pairs(s, expect), sb);
                    uint64_t nc = (nb - 32) / sb;
                    if (nc > UINT32_MAX) { err = ray_error("oom", NULL); goto done; }
                    void* blk = ray_alloc_raw((size_t)(nb - 32));
                    if (!blk) { err = ray_error("oom", NULL); goto done; }
                    s->old = s->blk; s->ocap = s->cap; s->obytes = s->bytes;
                    s->blk = blk; s->cap = nc; s->bytes = nb;
                    s->fresh = true;
                    live += (int64_t)nb;
                }
                if (np == 0) break;
                bool ok = cdb_run(pool, par, cdb_insert_fn, &c, np);
                uint32_t k = 0;
                for (uint32_t i = 0; i < np; i++) {
                    cdb_set_t* s = &c.sets[plist[i]];
                    if (s->old) {
                        ray_free_raw(s->old);
                        live -= (int64_t)s->obytes;
                        s->old = NULL; s->ocap = 0; s->obytes = 0;
                    }
                    if (s->full) plist[k++] = plist[i];
                }
                np = k;
                if (!ok) { err = ray_error("cancel", "interrupted"); goto done; }
            }
        }
        hi = c.hi;
        if (!cdb_run(pool, par, cdb_count_fn, &c, hi - lo)) {
            err = ray_error("cancel", "interrupted");
            goto done;
        }
        int64_t pairs = 0;
        for (uint32_t p = lo; p < hi; p++) { pairs += c.sets[p].n; cdb_free_set(&c.sets[p]); }
        ray_profile_tick("count-distinct: bounded round");
        /* the next round: as many partitions as the budget holds at the
         * bytes a partition took in this one */
        int64_t per = live / (int64_t)(hi - lo);
        expect = pairs / (int64_t)(hi - lo);
        lo = hi;
        if (lo >= P) break;
        int64_t take = per > 0 ? budget / 8 * 7 / per : (int64_t)P;
        if (take < 1) take = 1;
        hi = (int64_t)lo + take < (int64_t)P ? lo + (uint32_t)take : P;
    }

    if (cnt_shared) {
        memcpy(odata, c.cnt, (size_t)n_groups * sizeof(int64_t));
    } else {
        for (uint32_t w = 0; w < W; w++) {
            const int64_t* wc = c.cnt + (size_t)w * (size_t)n_groups;
            for (int64_t g = 0; g < n_groups; g++) odata[g] += wc[g];
        }
    }

done:
    if (c.sets) for (uint32_t p = 0; p < P; p++) cdb_free_set(&c.sets[p]);
    ray_free_raw(c.sets);
    ray_free_raw(c.recs);
    ray_free_raw(c.stage);
    ray_free_raw(c.spart);
    ray_free_raw(c.toff);
    ray_free_raw(c.cnt);
    ray_free_raw(plist);
    ray_free_raw(selpre);
    ray_free_raw(khdr);
    cdb_col_close(&c.key);
    cdb_col_close(&c.val);
    if (err) { ray_release(out); return err; }
    return out;
}

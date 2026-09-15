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
#include "table/sym.h"     /* RAY_IS_SYM */

/* Peak-footprint estimate used by the admission gate below: 32B/row for
 * phase 1's records plus up to 24B per (partition, key) pair in phase 2,
 * itself bounded by the row count — see the KNOWN MEMORY BOUND comment at
 * the phase-3 call site.  56B/row is that worst case (all-distinct input),
 * both phases' buffers alive simultaneously until cdf_free_all runs. */
#define CDF_BYTES_PER_ROW 56

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

#define CDF_REC 32 /* [pairhash 8][k 8][v 8][row 8] */

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
static bool cdf_sample_repetition(const cdf_p1_ctx_t* c) {
    if (c->nrows < 1024) return false;
    cdf_recent_t recent[64] = {{0}};
    int repeats = 0;
    int64_t step = c->nrows / 1024;
    for (int i = 0; i < 1024; i++) {
        int64_t row = i * step + ray_hash_i64(i) % step;
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
        for (int64_t r = begin; r < limit; r++) {
            int64_t k = cdf_read(c->kdata, r, c->ktype, c->kattrs);
            int64_t v = cdf_read(c->vdata, r, c->vtype, c->vattrs);
            uint64_t hash = cdf_pair_hash(k, v);
            if (c->dedup && cdf_repeat(recent, hash, k, v)) continue;
            uint32_t part = hash & (c->n_parts - 1);
            if (my[part].cap == UINT32_MAX) { atomic_store(&c->oom, 1); return; }
            my[part].cap++;
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
            ((uint64_t*)record)[0] = hash;
            ((int64_t*)record)[1] = k;
            ((int64_t*)record)[2] = v;
            ((int64_t*)record)[3] = r;
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
    /* Partitions actually processed.  ray_pool_dispatch_n CLAMPS its task
     * count to the ring capacity when the growth realloc fails (see
     * RAY_POOL_INIT_TASKS in core/pool.h) and a cancelled pool skips claimed
     * tasks — either way the tail partitions never run, keep ng == 0, and
     * their groups would silently vanish from the result.  The wrapper
     * compares this against n_parts and declines (NULL) on mismatch. */
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
                uint64_t ph = ((const uint64_t*)rec)[0];
                int64_t k = ((const int64_t*)rec)[1];
                int64_t v = ((const int64_t*)rec)[2];
                int64_t row = ((const int64_t*)rec)[3];

                /* k table probe for EVERY record: this partition's first_row
                 * must be the MIN over all its rows of the key, not only over
                 * rows that open a fresh (k,v) pair.  The slot index comes
                 * from a hash of k ALONE — recomputed here rather than stored,
                 * to keep the record at 32B — and that hash never selects a
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
     * estimate would exceed a quarter of the heap's anon watermark (default:
     * total physical RAM) — leaving headroom for the rest of the query
     * (source columns, other operators, concurrent work) sharing the same
     * budget.  No new env knob: this rides the existing watermark, which
     * ray_heap_set_anon_watermark already lets tests/embedders override.
     * Same "unknown RAM -> stay permissive" convention as heap.c's own
     * heap_anon_would_exceed: a caller that never ran ray_runtime_new (unit
     * tests calling ray_cd_fused directly, without the app's runtime/RAM
     * probe) sees ray_sys_total_ram() == 0, which must not read as a
     * zero-byte budget. */
    int64_t wm = ray_heap_anon_watermark();
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
     * bounded by the row count) coexist with phase 1's 32B-per-row records
     * until the cdf_free_all below, so a pathological all-distinct input peaks
     * near 56B/row.  Cardinality-aware admission (declining when the estimated
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

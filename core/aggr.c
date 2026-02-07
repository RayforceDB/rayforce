/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "aggr.h"
#include "eval.h"
#include "heap.h"
#include "hash.h"
#include "query.h"
#include "error.h"
#include "ops.h"
#include "pool.h"
#include "index.h"
#include "math.h"
#include "misc.h"
#include "items.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define PERFECT_HASH_THRESHOLD 65536   // Use perfect hash if range <= 64K
#define INITIAL_HT_CAPACITY 4096       // Initial hash table capacity
#define HT_LOAD_FACTOR 0.7             // Resize when load > 70%

// Min/max sentinels for aggregation (use rayforce.h constants)
#define AGG_I64_MIN NULL_I64
#define AGG_I64_MAX INF_I64
#define AGG_F64_MAX INF_F64

// ============================================================================
// Window-join binary search helpers
// ============================================================================

// Find rightmost position where vals[pos] <= val within [offset, offset+len)
static i64_t indexr_bin_i32_(i32_t val, i32_t vals[], i64_t offset, i64_t len) {
    i64_t left = 0, right = len - 1, mid, idx = 0;
    vals += offset;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[mid] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return idx + offset;
}

// Find leftmost position where vals[pos] >= val within [offset, offset+len)
static i64_t indexl_bin_i32_(i32_t val, i32_t vals[], i64_t offset, i64_t len) {
    i64_t left = 0, right = len - 1, mid, idx = 0;
    vals += offset;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[mid] < val) {
            left = mid + 1;
        } else {
            idx = mid;
            right = mid - 1;
        }
    }
    return idx + offset;
}

// Get [li, ri] range for window-join row i. Returns B8_TRUE if valid.
static inline b8_t wj_range(obj_p index, i64_t i, i64_t* li, i64_t* ri) {
    obj_p rn = AS_LIST(AS_LIST(index)[5])[i];
    if (rn == NULL_OBJ) return B8_FALSE;

    i64_t fi = AS_I64(rn)[0];
    i64_t ti = AS_I64(rn)[1];
    i32_t kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
    i32_t kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
    i64_t jtype = AS_LIST(index)[6]->i64;
    i32_t* rxcol = AS_I32(AS_LIST(index)[3]);

    if (jtype == 0)
        *li = indexr_bin_i32_(kl, rxcol, fi, ti - fi + 1);
    else
        *li = indexl_bin_i32_(kl, rxcol, fi, ti - fi + 1);
    *ri = indexr_bin_i32_(kr, rxcol, fi, ti - fi + 1);

    if (rxcol[*li] > kr || (jtype == 1 && rxcol[*ri] < kl))
        return B8_FALSE;

    return B8_TRUE;
}

// ============================================================================
// Hash table entry for fused aggregation
// ============================================================================

typedef struct {
    u16_t salt;      // Upper 16 bits of hash for fast collision filtering
    u16_t reserved;
    u32_t group_id;  // Index into aggregate state arrays (0xFFFFFFFF = empty)
} agg_entry_t;

#define AGG_ENTRY_EMPTY 0xFFFFFFFF

// Extract salt from hash (upper 16 bits)
#define HASH_SALT(h) ((u16_t)((h) >> 48))

// ============================================================================
// Per-thread local aggregation state
// ============================================================================

typedef struct {
    agg_entry_t *entries;    // Hash table entries
    i64_t capacity;          // Hash table capacity (power of 2)
    i64_t mask;              // capacity - 1 for fast modulo
    i64_t count;             // Number of groups found
    i64_t *sums_i64;         // Sum accumulators for i64
    f64_t *sums_f64;         // Sum accumulators for f64
    i64_t *counts;           // Count per group
    i64_t *mins_i64;         // Min values for i64
    i64_t *maxs_i64;         // Max values for i64
    f64_t *mins_f64;         // Min values for f64
    f64_t *maxs_f64;         // Max values for f64
    i64_t *first_rows;       // First row index per group (for key extraction)
    i64_t *last_rows;        // Last row index per group
    u64_t *group_hashes;     // Pre-computed hash per group (for fast merge)
    i64_t max_groups;        // Allocated capacity for groups
} local_agg_t;

// ============================================================================
// Perfect hash aggregation (for small key ranges)
// ============================================================================

typedef struct {
    i64_t *sums_i64;
    f64_t *sums_f64;
    i64_t *counts;
    i64_t *mins_i64;
    i64_t *maxs_i64;
    f64_t *mins_f64;
    f64_t *maxs_f64;
    i64_t *first_rows;
    i64_t *last_rows;
    i64_t min_key;
    i64_t range;
} perfect_agg_t;

// ============================================================================
// Helper functions
// ============================================================================

static inline i64_t next_power_of_2(i64_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// Compute composite hash for multiple key columns at row i
static inline u64_t compute_composite_hash(obj_p keys, i64_t nkeys, i64_t row) {
    u64_t h;
    i64_t k;
    obj_p col;

    h = 0xcbf29ce484222325ull;
    for (k = 0; k < nkeys; k++) {
        col = AS_LIST(keys)[k];
        switch (col->type) {
            case TYPE_I64:
            case TYPE_SYMBOL:
            case TYPE_TIMESTAMP:
                h = hash_index_u64(h, (u64_t)AS_I64(col)[row]);
                break;
            case TYPE_I32:
            case TYPE_DATE:
            case TYPE_TIME:
                h = hash_index_u64(h, (u64_t)AS_I32(col)[row]);
                break;
            case TYPE_I16:
                h = hash_index_u64(h, (u64_t)AS_I16(col)[row]);
                break;
            case TYPE_B8:
                h = hash_index_u64(h, (u64_t)AS_I8(col)[row]);
                break;
            case TYPE_F64:
                h = hash_index_u64(h, *(u64_t *)&AS_F64(col)[row]);
                break;
            case TYPE_LIST:
                h = hash_index_u64(h, hash_index_obj(AS_LIST(col)[row]));
                break;
            default:
                h = hash_index_u64(h, (u64_t)row);
                break;
        }
    }
    return h;
}

// Compare key columns at two rows
static inline b8_t keys_equal(obj_p keys, i64_t nkeys, i64_t row1, i64_t row2) {
    i64_t k;
    obj_p col;

    for (k = 0; k < nkeys; k++) {
        col = AS_LIST(keys)[k];
        switch (col->type) {
            case TYPE_I64:
            case TYPE_SYMBOL:
            case TYPE_TIMESTAMP:
                if (AS_I64(col)[row1] != AS_I64(col)[row2])
                    return B8_FALSE;
                break;
            case TYPE_I32:
            case TYPE_DATE:
            case TYPE_TIME:
                if (AS_I32(col)[row1] != AS_I32(col)[row2])
                    return B8_FALSE;
                break;
            case TYPE_I16:
                if (AS_I16(col)[row1] != AS_I16(col)[row2])
                    return B8_FALSE;
                break;
            case TYPE_B8:
                if (AS_I8(col)[row1] != AS_I8(col)[row2])
                    return B8_FALSE;
                break;
            case TYPE_F64:
                if (AS_F64(col)[row1] != AS_F64(col)[row2])
                    return B8_FALSE;
                break;
            case TYPE_LIST:
                if (cmp_obj(AS_LIST(col)[row1], AS_LIST(col)[row2]) != 0)
                    return B8_FALSE;
                break;
            default:
                return B8_FALSE;
        }
    }
    return B8_TRUE;
}

// ============================================================================
// Local hash table operations
// ============================================================================

static nil_t local_agg_init(local_agg_t *agg, i64_t capacity, i64_t max_groups) {
    i64_t i;

    agg->capacity = next_power_of_2(capacity);
    agg->mask = agg->capacity - 1;
    agg->count = 0;
    agg->max_groups = max_groups;

    agg->entries = (agg_entry_t *)heap_alloc(agg->capacity * sizeof(agg_entry_t));
    for (i = 0; i < agg->capacity; i++)
        agg->entries[i].group_id = AGG_ENTRY_EMPTY;

    agg->sums_i64 = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->sums_f64 = (f64_t *)heap_alloc(max_groups * sizeof(f64_t));
    agg->counts = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->mins_i64 = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->maxs_i64 = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->mins_f64 = (f64_t *)heap_alloc(max_groups * sizeof(f64_t));
    agg->maxs_f64 = (f64_t *)heap_alloc(max_groups * sizeof(f64_t));
    agg->first_rows = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->last_rows = (i64_t *)heap_alloc(max_groups * sizeof(i64_t));
    agg->group_hashes = (u64_t *)heap_alloc(max_groups * sizeof(u64_t));

    for (i = 0; i < max_groups; i++) {
        agg->sums_i64[i] = 0;
        agg->sums_f64[i] = 0.0;
        agg->counts[i] = 0;
        agg->mins_i64[i] = AGG_I64_MAX;
        agg->maxs_i64[i] = AGG_I64_MIN;
        agg->mins_f64[i] = AGG_F64_MAX;
        agg->maxs_f64[i] = -AGG_F64_MAX;
        agg->first_rows[i] = -1;
        agg->last_rows[i] = -1;
    }
}

static nil_t local_agg_destroy(local_agg_t *agg) {
    heap_free(agg->entries);
    heap_free(agg->sums_i64);
    heap_free(agg->sums_f64);
    heap_free(agg->counts);
    heap_free(agg->mins_i64);
    heap_free(agg->maxs_i64);
    heap_free(agg->mins_f64);
    heap_free(agg->maxs_f64);
    heap_free(agg->first_rows);
    heap_free(agg->last_rows);
    heap_free(agg->group_hashes);
}

// Resize hash table when load factor exceeded
static nil_t local_agg_resize(local_agg_t *agg) {
    i64_t i, new_capacity, new_mask, idx;
    agg_entry_t *new_entries, *old_entries;
    u64_t h;
    u16_t salt;
    u32_t gid;

    new_capacity = agg->capacity * 2;
    new_mask = new_capacity - 1;
    new_entries = (agg_entry_t *)heap_alloc(new_capacity * sizeof(agg_entry_t));

    for (i = 0; i < new_capacity; i++)
        new_entries[i].group_id = AGG_ENTRY_EMPTY;

    old_entries = agg->entries;

    // Rehash all existing entries using stored hashes
    for (i = 0; i < agg->capacity; i++) {
        gid = old_entries[i].group_id;
        if (gid != AGG_ENTRY_EMPTY) {
            h = agg->group_hashes[gid];
            salt = HASH_SALT(h);
            idx = h & new_mask;

            while (new_entries[idx].group_id != AGG_ENTRY_EMPTY)
                idx = (idx + 1) & new_mask;

            new_entries[idx].salt = salt;
            new_entries[idx].group_id = gid;
        }
    }

    heap_free(old_entries);
    agg->entries = new_entries;
    agg->capacity = new_capacity;
    agg->mask = new_mask;
}

// Find or create group, returns group_id
static inline i64_t local_agg_find_or_create(local_agg_t *agg, obj_p keys, i64_t nkeys, i64_t row, u64_t hash) {
    u16_t salt;
    i64_t idx, group_id;
    agg_entry_t *entry;

    salt = HASH_SALT(hash);
    idx = hash & agg->mask;

    for (;;) {
        entry = &agg->entries[idx];

        if (entry->group_id == AGG_ENTRY_EMPTY) {
            // New group
            if (agg->count >= agg->max_groups) {
                // Need to grow groups arrays - for simplicity, just fail
                // In production, would reallocate
                return -1;
            }

            // Check load factor
            if ((agg->count + 1) * 10 > agg->capacity * 7) {
                local_agg_resize(agg);
                // Retry with new table
                return local_agg_find_or_create(agg, keys, nkeys, row, hash);
            }

            group_id = agg->count++;
            entry->salt = salt;
            entry->group_id = (u32_t)group_id;
            agg->first_rows[group_id] = row;
            agg->last_rows[group_id] = row;
            agg->group_hashes[group_id] = hash;  // Store hash for fast merge
            return group_id;
        }

        // Salt match - check full keys
        if (entry->salt == salt) {
            group_id = entry->group_id;
            if (keys_equal(keys, nkeys, agg->first_rows[group_id], row)) {
                agg->last_rows[group_id] = row;
                return group_id;
            }
        }

        // Linear probe
        idx = (idx + 1) & agg->mask;
    }
}

// ============================================================================
// Perfect hash aggregation (for small key ranges)
// ============================================================================

static nil_t perfect_agg_init(perfect_agg_t *agg, i64_t min_key, i64_t range) {
    i64_t i;

    agg->min_key = min_key;
    agg->range = range;

    agg->sums_i64 = (i64_t *)heap_alloc(range * sizeof(i64_t));
    agg->sums_f64 = (f64_t *)heap_alloc(range * sizeof(f64_t));
    agg->counts = (i64_t *)heap_alloc(range * sizeof(i64_t));
    agg->mins_i64 = (i64_t *)heap_alloc(range * sizeof(i64_t));
    agg->maxs_i64 = (i64_t *)heap_alloc(range * sizeof(i64_t));
    agg->mins_f64 = (f64_t *)heap_alloc(range * sizeof(f64_t));
    agg->maxs_f64 = (f64_t *)heap_alloc(range * sizeof(f64_t));
    agg->first_rows = (i64_t *)heap_alloc(range * sizeof(i64_t));
    agg->last_rows = (i64_t *)heap_alloc(range * sizeof(i64_t));

    for (i = 0; i < range; i++) {
        agg->sums_i64[i] = 0;
        agg->sums_f64[i] = 0.0;
        agg->counts[i] = 0;
        agg->mins_i64[i] = AGG_I64_MAX;
        agg->maxs_i64[i] = AGG_I64_MIN;
        agg->mins_f64[i] = AGG_F64_MAX;
        agg->maxs_f64[i] = -AGG_F64_MAX;
        agg->first_rows[i] = -1;
        agg->last_rows[i] = -1;
    }
}

static nil_t perfect_agg_destroy(perfect_agg_t *agg) {
    heap_free(agg->sums_i64);
    heap_free(agg->sums_f64);
    heap_free(agg->counts);
    heap_free(agg->mins_i64);
    heap_free(agg->maxs_i64);
    heap_free(agg->mins_f64);
    heap_free(agg->maxs_f64);
    heap_free(agg->first_rows);
    heap_free(agg->last_rows);
}

// ============================================================================
// Fused hash-aggregate for single i64 key column
// ============================================================================

static obj_p fused_sum_i64_perfect(obj_p key_col, obj_p val_col, i64_t min_key, i64_t range) {
    perfect_agg_t agg;
    i64_t i, nrows, idx, ngroups, k, v;
    i64_t *keys, *vals, *out_vals;
    obj_p res_vals;

    nrows = key_col->len;
    keys = AS_I64(key_col);
    vals = AS_I64(val_col);

    perfect_agg_init(&agg, min_key, range);

    // Fused aggregation loop with 4x unrolling
    for (i = 0; i + 3 < nrows; i += 4) {
        idx = keys[i] - min_key;
        v = vals[i];
        agg.sums_i64[idx] += (v != NULL_I64) ? v : 0;
        agg.counts[idx] += (v != NULL_I64);
        if (agg.first_rows[idx] < 0)
            agg.first_rows[idx] = i;

        idx = keys[i + 1] - min_key;
        v = vals[i + 1];
        agg.sums_i64[idx] += (v != NULL_I64) ? v : 0;
        agg.counts[idx] += (v != NULL_I64);
        if (agg.first_rows[idx] < 0)
            agg.first_rows[idx] = i + 1;

        idx = keys[i + 2] - min_key;
        v = vals[i + 2];
        agg.sums_i64[idx] += (v != NULL_I64) ? v : 0;
        agg.counts[idx] += (v != NULL_I64);
        if (agg.first_rows[idx] < 0)
            agg.first_rows[idx] = i + 2;

        idx = keys[i + 3] - min_key;
        v = vals[i + 3];
        agg.sums_i64[idx] += (v != NULL_I64) ? v : 0;
        agg.counts[idx] += (v != NULL_I64);
        if (agg.first_rows[idx] < 0)
            agg.first_rows[idx] = i + 3;
    }

    // Handle remainder
    for (; i < nrows; i++) {
        idx = keys[i] - min_key;
        v = vals[i];
        agg.sums_i64[idx] += (v != NULL_I64) ? v : 0;
        agg.counts[idx] += (v != NULL_I64);
        if (agg.first_rows[idx] < 0)
            agg.first_rows[idx] = i;
    }

    // Count non-empty groups
    ngroups = 0;
    for (i = 0; i < range; i++)
        if (agg.counts[i] > 0)
            ngroups++;

    // Extract results
    res_vals = vector(TYPE_I64, ngroups);
    out_vals = AS_I64(res_vals);

    k = 0;
    for (i = 0; i < range; i++) {
        if (agg.counts[i] > 0)
            out_vals[k++] = agg.sums_i64[i];
    }

    perfect_agg_destroy(&agg);
    return res_vals;
}

static obj_p fused_sum_i64_hash(obj_p keys, i64_t nkeys, obj_p val_col) {
    local_agg_t agg;
    i64_t i, nrows, group_id;
    i64_t *vals, *out_vals;
    u64_t h;
    obj_p res_vals;

    nrows = val_col->len;
    vals = AS_I64(val_col);

    // Initialize local aggregation state
    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    // Fused hash-aggregate loop
    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);

        if (group_id >= 0 && vals[i] != NULL_I64)
            agg.sums_i64[group_id] += vals[i];
    }

    // Extract results
    res_vals = vector(TYPE_I64, agg.count);
    out_vals = AS_I64(res_vals);

    for (i = 0; i < agg.count; i++)
        out_vals[i] = agg.sums_i64[i];

    local_agg_destroy(&agg);
    return res_vals;
}

static obj_p fused_sum_f64_hash(obj_p keys, i64_t nkeys, obj_p val_col) {
    local_agg_t agg;
    i64_t i, nrows, group_id;
    f64_t *vals, *out_vals;
    u64_t h;
    obj_p res_vals;

    nrows = val_col->len;
    vals = AS_F64(val_col);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);

        if (group_id >= 0 && vals[i] != NULL_F64)
            agg.sums_f64[group_id] += vals[i];
    }

    res_vals = vector(TYPE_F64, agg.count);
    out_vals = AS_F64(res_vals);

    for (i = 0; i < agg.count; i++)
        out_vals[i] = agg.sums_f64[i];

    local_agg_destroy(&agg);
    return res_vals;
}

// ============================================================================
// Parallel aggregation with per-worker hash tables
// ============================================================================

#define PARALLEL_AGG_THRESHOLD 100000  // Min rows for parallel aggregation
#define MAX_AGG_WORKERS 16             // Cap workers to limit merge overhead

typedef struct {
    obj_p keys;           // Key columns (shared, read-only)
    i64_t nkeys;          // Number of key columns
    i64_t *vals;          // Value array (shared, read-only)
    i64_t chunk_size;     // Size of each row chunk
    local_agg_t *aggs;    // Per-worker hash tables
} parallel_agg_ctx_t;

// Worker function: process rows in chunk, build local hash table
static obj_p parallel_sum_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_agg_ctx_t *ctx = (parallel_agg_ctx_t *)ctx_ptr;
    i64_t chunk_idx, i, end, group_id;
    i64_t *restrict vals;
    i64_t *restrict sums;
    u64_t h;
    local_agg_t *agg;

    chunk_idx = offset / ctx->chunk_size;
    agg = &ctx->aggs[chunk_idx];
    vals = ctx->vals;
    sums = agg->sums_i64;
    end = offset + len;

    // Fused hash-aggregate loop for this chunk
    for (i = offset; i < end; i++) {
        h = compute_composite_hash(ctx->keys, ctx->nkeys, i);
        group_id = local_agg_find_or_create(agg, ctx->keys, ctx->nkeys, i, h);

        if (group_id >= 0 && vals[i] != NULL_I64)
            sums[group_id] += vals[i];
    }

    return NULL_OBJ;
}

// Merge per-worker hash tables into global result using stored hashes
static obj_p parallel_sum_merge(parallel_agg_ctx_t *ctx, i64_t nworkers) {
    local_agg_t merged;
    i64_t w, i, global_group;
    u64_t h;
    i64_t *out_vals;
    obj_p res_vals;

    // Initialize merged hash table
    local_agg_init(&merged, INITIAL_HT_CAPACITY * nworkers,
                   ctx->aggs[0].max_groups * nworkers);

    // Merge all worker hash tables - use stored hashes, avoid recomputing
    for (w = 0; w < nworkers; w++) {
        local_agg_t *worker_agg = &ctx->aggs[w];

        for (i = 0; i < worker_agg->count; i++) {
            // Use stored hash instead of recomputing
            h = worker_agg->group_hashes[i];
            i64_t worker_row = worker_agg->first_rows[i];

            // Find or create group in merged table
            global_group = local_agg_find_or_create(&merged, ctx->keys, ctx->nkeys, worker_row, h);

            if (global_group >= 0)
                merged.sums_i64[global_group] += worker_agg->sums_i64[i];
        }
    }

    // Extract results
    res_vals = vector(TYPE_I64, merged.count);
    out_vals = AS_I64(res_vals);

    for (i = 0; i < merged.count; i++)
        out_vals[i] = merged.sums_i64[i];

    local_agg_destroy(&merged);
    return res_vals;
}

static obj_p fused_sum_i64_parallel(obj_p keys, i64_t nkeys, obj_p val_col) {
    pool_p pool;
    i64_t nrows, nworkers, chunk_size, i, offset;
    parallel_agg_ctx_t ctx;
    obj_p res;

    pool = pool_get();
    nrows = val_col->len;

    // Determine parallelism - cap workers to limit merge overhead
    nworkers = pool_split_by(pool, nrows, 0);
    if (nworkers > MAX_AGG_WORKERS)
        nworkers = MAX_AGG_WORKERS;
    if (nworkers <= 1)
        return fused_sum_i64_hash(keys, nkeys, val_col);

    chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));

    // Setup context
    ctx.keys = keys;
    ctx.nkeys = nkeys;
    ctx.vals = AS_I64(val_col);
    ctx.chunk_size = chunk_size;
    ctx.aggs = (local_agg_t *)heap_alloc(nworkers * sizeof(local_agg_t));

    // Initialize per-worker aggregation states
    for (i = 0; i < nworkers; i++)
        local_agg_init(&ctx.aggs[i], INITIAL_HT_CAPACITY, nrows / (10 * nworkers) + 1024);

    // Submit tasks
    pool_prepare(pool);
    offset = 0;
    for (i = 0; i < nworkers - 1; i++) {
        pool_add_task(pool, (raw_p)parallel_sum_worker, 3, chunk_size, offset, &ctx);
        offset += chunk_size;
    }
    // Last chunk may be smaller
    pool_add_task(pool, (raw_p)parallel_sum_worker, 3, nrows - offset, offset, &ctx);

    // Run workers
    res = pool_run(pool);
    drop_obj(res);

    // Merge results
    res = parallel_sum_merge(&ctx, nworkers);

    // Cleanup
    for (i = 0; i < nworkers; i++)
        local_agg_destroy(&ctx.aggs[i]);
    heap_free(ctx.aggs);

    return res;
}

// ============================================================================
// Public aggregation functions
// ============================================================================

obj_p aggr_sum(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, key_col;
    i64_t nkeys, nrows, min_key, max_key, range, i;
    i64_t *key_vals;

    UNUSED(index);

    // Get groupby keys from query context
    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(val->type, 0);

    // Handle i64 values
    if (val->type == TYPE_I64) {
        // Single key column optimization - check for perfect hash opportunity
        if (nkeys == 1) {
            key_col = AS_LIST(keys)[0];
            if (key_col->type == TYPE_I64 || key_col->type == TYPE_SYMBOL) {
                key_vals = AS_I64(key_col);

                // Sample to find min/max
                min_key = AGG_I64_MAX;
                max_key = AGG_I64_MIN;
                for (i = 0; i < nrows; i += 1000) {
                    if (key_vals[i] < min_key)
                        min_key = key_vals[i];
                    if (key_vals[i] > max_key)
                        max_key = key_vals[i];
                }
                // Full scan for accurate bounds
                for (i = 0; i < nrows; i++) {
                    if (key_vals[i] < min_key)
                        min_key = key_vals[i];
                    if (key_vals[i] > max_key)
                        max_key = key_vals[i];
                }

                range = max_key - min_key + 1;

                if (range > 0 && range <= PERFECT_HASH_THRESHOLD)
                    return fused_sum_i64_perfect(key_col, val, min_key, range);
            }
        }

        // Fall back to parallel hash-based aggregation
        if (nrows >= PARALLEL_AGG_THRESHOLD)
            return fused_sum_i64_parallel(keys, nkeys, val);
        return fused_sum_i64_hash(keys, nkeys, val);
    }

    if (val->type == TYPE_F64)
        return fused_sum_f64_hash(keys, nkeys, val);

    // I32 sum (Time, Date, I32) - accumulate in i64, return i64
    if (val->type == TYPE_I32) {
        local_agg_t lagg;
        i32_t *vals32 = AS_I32(val);
        local_agg_init(&lagg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);
        for (i = 0; i < nrows; i++) {
            u64_t h = compute_composite_hash(keys, nkeys, i);
            i64_t gid = local_agg_find_or_create(&lagg, keys, nkeys, i, h);
            if (gid >= 0)
                lagg.sums_i64[gid] += (i64_t)vals32[i];
        }
        obj_p res = vector(TYPE_I64, lagg.count);
        for (i = 0; i < lagg.count; i++)
            AS_I64(res)[i] = lagg.sums_i64[i];
        local_agg_destroy(&lagg);
        return res;
    }

    // I16 sum - accumulate in i64, return i64
    if (val->type == TYPE_I16) {
        local_agg_t lagg;
        i16_t *vals16 = AS_I16(val);
        local_agg_init(&lagg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);
        for (i = 0; i < nrows; i++) {
            u64_t h = compute_composite_hash(keys, nkeys, i);
            i64_t gid = local_agg_find_or_create(&lagg, keys, nkeys, i, h);
            if (gid >= 0)
                lagg.sums_i64[gid] += (i64_t)vals16[i];
        }
        obj_p res = vector(TYPE_I64, lagg.count);
        for (i = 0; i < lagg.count; i++)
            AS_I64(res)[i] = lagg.sums_i64[i];
        local_agg_destroy(&lagg);
        return res;
    }

    return err_type(TYPE_I64, val->type, 0, 0);
}

obj_p aggr_count(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, key_col, res;
    i64_t nkeys, nrows, min_key, max_key, range, i, idx, ngroups, k;
    i64_t *key_vals, *out_vals;
    perfect_agg_t agg;
    local_agg_t lagg;
    u64_t h;
    i64_t group_id;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(TYPE_I64, 0);

    // Single key column - try perfect hash
    if (nkeys == 1) {
        key_col = AS_LIST(keys)[0];
        if (key_col->type == TYPE_I64 || key_col->type == TYPE_SYMBOL) {
            key_vals = AS_I64(key_col);

            min_key = AGG_I64_MAX;
            max_key = AGG_I64_MIN;
            for (i = 0; i < nrows; i++) {
                if (key_vals[i] < min_key)
                    min_key = key_vals[i];
                if (key_vals[i] > max_key)
                    max_key = key_vals[i];
            }

            range = max_key - min_key + 1;

            if (range > 0 && range <= PERFECT_HASH_THRESHOLD) {
                perfect_agg_init(&agg, min_key, range);

                for (i = 0; i < nrows; i++) {
                    idx = key_vals[i] - min_key;
                    agg.counts[idx]++;
                }

                ngroups = 0;
                for (i = 0; i < range; i++)
                    if (agg.counts[i] > 0)
                        ngroups++;

                res = vector(TYPE_I64, ngroups);
                out_vals = AS_I64(res);

                k = 0;
                for (i = 0; i < range; i++)
                    if (agg.counts[i] > 0)
                        out_vals[k++] = agg.counts[i];

                perfect_agg_destroy(&agg);
                return res;
            }
        }
    }

    // Hash-based count
    local_agg_init(&lagg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        group_id = local_agg_find_or_create(&lagg, keys, nkeys, i, h);
        if (group_id >= 0)
            lagg.counts[group_id]++;
    }

    res = vector(TYPE_I64, lagg.count);
    out_vals = AS_I64(res);

    for (i = 0; i < lagg.count; i++)
        out_vals[i] = lagg.counts[i];

    local_agg_destroy(&lagg);
    return res;
}

obj_p aggr_first(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i;
    local_agg_t agg;
    u64_t h;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(val->type, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    // Build groups (first_rows is populated during find_or_create)
    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        local_agg_find_or_create(&agg, keys, nkeys, i, h);
    }

    // Extract first values
    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_I64, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        f64_t *out;
        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_SYMBOL) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_SYMBOL, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_I32 || val->type == TYPE_DATE || val->type == TYPE_TIME) {
        i32_t *vals = AS_I32(val);
        i32_t *out;
        res = vector(val->type, agg.count);
        out = AS_I32(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_TIMESTAMP) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_TIMESTAMP, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_GUID) {
        guid_t *vals = AS_GUID(val);
        guid_t *out;
        res = GUID(agg.count);
        out = AS_GUID(res);
        for (i = 0; i < agg.count; i++)
            memcpy(&out[i], &vals[agg.first_rows[i]], sizeof(guid_t));
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        i16_t *out;
        res = vector(TYPE_I16, agg.count);
        out = AS_I16(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_B8) {
        i8_t *vals = AS_I8(val);
        i8_t *out;
        res = vector(TYPE_B8, agg.count);
        out = AS_I8(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_U8) {
        u8_t *vals = AS_U8(val);
        u8_t *out;
        res = vector(TYPE_U8, agg.count);
        out = AS_U8(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.first_rows[i]];
    } else if (val->type == TYPE_LIST) {
        res = LIST(agg.count);
        for (i = 0; i < agg.count; i++)
            AS_LIST(res)[i] = clone_obj(AS_LIST(val)[agg.first_rows[i]]);
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_last(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i;
    local_agg_t agg;
    u64_t h;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(val->type, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        local_agg_find_or_create(&agg, keys, nkeys, i, h);
    }

    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_I64, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        f64_t *out;
        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_SYMBOL) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_SYMBOL, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_I32 || val->type == TYPE_DATE || val->type == TYPE_TIME) {
        i32_t *vals = AS_I32(val);
        i32_t *out;
        res = vector(val->type, agg.count);
        out = AS_I32(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_TIMESTAMP) {
        i64_t *vals = AS_I64(val);
        i64_t *out;
        res = vector(TYPE_TIMESTAMP, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_GUID) {
        guid_t *vals = AS_GUID(val);
        guid_t *out;
        res = GUID(agg.count);
        out = AS_GUID(res);
        for (i = 0; i < agg.count; i++)
            memcpy(&out[i], &vals[agg.last_rows[i]], sizeof(guid_t));
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        i16_t *out;
        res = vector(TYPE_I16, agg.count);
        out = AS_I16(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_B8) {
        i8_t *vals = AS_I8(val);
        i8_t *out;
        res = vector(TYPE_B8, agg.count);
        out = AS_I8(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_U8) {
        u8_t *vals = AS_U8(val);
        u8_t *out;
        res = vector(TYPE_U8, agg.count);
        out = AS_U8(res);
        for (i = 0; i < agg.count; i++)
            out[i] = vals[agg.last_rows[i]];
    } else if (val->type == TYPE_LIST) {
        res = LIST(agg.count);
        for (i = 0; i < agg.count; i++)
            AS_LIST(res)[i] = clone_obj(AS_LIST(val)[agg.last_rows[i]]);
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_avg(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;
    f64_t *out;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(TYPE_F64, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_I64) {
                agg.sums_i64[group_id] += vals[i];
                agg.counts[group_id]++;
            }
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.counts[i] > 0) ? (f64_t)agg.sums_i64[i] / agg.counts[i] : 0.0;
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_F64) {
                agg.sums_f64[group_id] += vals[i];
                agg.counts[group_id]++;
            }
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.counts[i] > 0) ? agg.sums_f64[i] / agg.counts[i] : 0.0;
    } else if (val->type == TYPE_I32) {
        i32_t *vals = AS_I32(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0) {
                agg.sums_i64[group_id] += (i64_t)vals[i];
                agg.counts[group_id]++;
            }
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.counts[i] > 0) ? (f64_t)agg.sums_i64[i] / agg.counts[i] : 0.0;
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0) {
                agg.sums_i64[group_id] += (i64_t)vals[i];
                agg.counts[group_id]++;
            }
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.counts[i] > 0) ? (f64_t)agg.sums_i64[i] / agg.counts[i] : 0.0;
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_max(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;

    // Window-join path
    if (index != NULL_OBJ && AS_LIST(index)[0]->i64 == INDEX_TYPE_WINDOW) {
        i64_t ll = AS_LIST(index)[1]->i64;
        i64_t li, ri, x;

        if (val->type == TYPE_I64) {
            i64_t *vals = AS_I64(val);
            res = vector(TYPE_I64, ll);
            i64_t *out = AS_I64(res);
            for (i = 0; i < ll; i++) {
                if (wj_range(index, i, &li, &ri)) {
                    i64_t m = AGG_I64_MIN;
                    for (x = li; x <= ri; x++)
                        if (vals[x] != NULL_I64 && vals[x] > m) m = vals[x];
                    out[i] = (m != AGG_I64_MIN) ? m : NULL_I64;
                } else {
                    out[i] = NULL_I64;
                }
            }
        } else if (val->type == TYPE_F64) {
            f64_t *vals = AS_F64(val);
            res = vector(TYPE_F64, ll);
            f64_t *out = AS_F64(res);
            for (i = 0; i < ll; i++) {
                if (wj_range(index, i, &li, &ri)) {
                    f64_t m = -AGG_F64_MAX;
                    for (x = li; x <= ri; x++)
                        if (vals[x] != NULL_F64 && vals[x] > m) m = vals[x];
                    out[i] = (m != -AGG_F64_MAX) ? m : NULL_F64;
                } else {
                    out[i] = NULL_F64;
                }
            }
        } else {
            return err_type(TYPE_I64, val->type, 0, 0);
        }
        return res;
    }

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(val->type, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        i64_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_I64 && vals[i] > agg.maxs_i64[group_id])
                agg.maxs_i64[group_id] = vals[i];
        }

        res = vector(TYPE_I64, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.maxs_i64[i] != AGG_I64_MIN) ? agg.maxs_i64[i] : NULL_I64;
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        f64_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_F64 && vals[i] > agg.maxs_f64[group_id])
                agg.maxs_f64[group_id] = vals[i];
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.maxs_f64[i] != -AGG_F64_MAX) ? agg.maxs_f64[i] : NULL_F64;
    } else if (val->type == TYPE_I32) {
        i32_t *vals = AS_I32(val);
        i32_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && (i64_t)vals[i] > agg.maxs_i64[group_id])
                agg.maxs_i64[group_id] = (i64_t)vals[i];
        }

        res = vector(TYPE_I32, agg.count);
        out = AS_I32(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (i32_t)agg.maxs_i64[i];
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        i16_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && (i64_t)vals[i] > agg.maxs_i64[group_id])
                agg.maxs_i64[group_id] = (i64_t)vals[i];
        }

        res = vector(TYPE_I16, agg.count);
        out = AS_I16(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (i16_t)agg.maxs_i64[i];
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_min(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;

    // Window-join path: iterate pre-computed per-row windows
    if (index != NULL_OBJ && AS_LIST(index)[0]->i64 == INDEX_TYPE_WINDOW) {
        i64_t ll = AS_LIST(index)[1]->i64;
        i64_t li, ri, x;

        if (val->type == TYPE_I64) {
            i64_t *vals = AS_I64(val);
            res = vector(TYPE_I64, ll);
            i64_t *out = AS_I64(res);
            for (i = 0; i < ll; i++) {
                if (wj_range(index, i, &li, &ri)) {
                    i64_t m = AGG_I64_MAX;
                    for (x = li; x <= ri; x++)
                        if (vals[x] != NULL_I64 && vals[x] < m) m = vals[x];
                    out[i] = (m != AGG_I64_MAX) ? m : NULL_I64;
                } else {
                    out[i] = NULL_I64;
                }
            }
        } else if (val->type == TYPE_F64) {
            f64_t *vals = AS_F64(val);
            res = vector(TYPE_F64, ll);
            f64_t *out = AS_F64(res);
            for (i = 0; i < ll; i++) {
                if (wj_range(index, i, &li, &ri)) {
                    f64_t m = AGG_F64_MAX;
                    for (x = li; x <= ri; x++)
                        if (vals[x] != NULL_F64 && vals[x] < m) m = vals[x];
                    out[i] = (m != AGG_F64_MAX) ? m : NULL_F64;
                } else {
                    out[i] = NULL_F64;
                }
            }
        } else if (val->type == TYPE_I32 || val->type == TYPE_DATE || val->type == TYPE_TIME) {
            i32_t *vals = AS_I32(val);
            res = vector(val->type, ll);
            i32_t *out = AS_I32(res);
            for (i = 0; i < ll; i++) {
                if (wj_range(index, i, &li, &ri)) {
                    i64_t m = AGG_I64_MAX;
                    for (x = li; x <= ri; x++)
                        if ((i64_t)vals[x] < m) m = (i64_t)vals[x];
                    out[i] = (i32_t)m;
                } else {
                    out[i] = NULL_I32;
                }
            }
        } else {
            return err_type(TYPE_I64, val->type, 0, 0);
        }
        return res;
    }

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(val->type, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        i64_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_I64 && vals[i] < agg.mins_i64[group_id])
                agg.mins_i64[group_id] = vals[i];
        }

        res = vector(TYPE_I64, agg.count);
        out = AS_I64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.mins_i64[i] != AGG_I64_MAX) ? agg.mins_i64[i] : NULL_I64;
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        f64_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_F64 && vals[i] < agg.mins_f64[group_id])
                agg.mins_f64[group_id] = vals[i];
        }

        res = vector(TYPE_F64, agg.count);
        out = AS_F64(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (agg.mins_f64[i] != AGG_F64_MAX) ? agg.mins_f64[i] : NULL_F64;
    } else if (val->type == TYPE_I32) {
        i32_t *vals = AS_I32(val);
        i32_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && (i64_t)vals[i] < agg.mins_i64[group_id])
                agg.mins_i64[group_id] = (i64_t)vals[i];
        }

        res = vector(TYPE_I32, agg.count);
        out = AS_I32(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (i32_t)agg.mins_i64[i];
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        i16_t *out;

        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && (i64_t)vals[i] < agg.mins_i64[group_id])
                agg.mins_i64[group_id] = (i64_t)vals[i];
        }

        res = vector(TYPE_I16, agg.count);
        out = AS_I16(res);
        for (i = 0; i < agg.count; i++)
            out[i] = (i16_t)agg.mins_i64[i];
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

static int cmp_f64(const void *a, const void *b) {
    f64_t fa = *(const f64_t *)a;
    f64_t fb = *(const f64_t *)b;
    return (fa > fb) - (fa < fb);
}

obj_p aggr_med(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;
    i64_t *group_ids, *cursors;
    f64_t **group_vals;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(TYPE_F64, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    // First pass: assign groups and count
    group_ids = (i64_t *)heap_alloc(nrows * sizeof(i64_t));
    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
        group_ids[i] = group_id;
        if (group_id >= 0)
            agg.counts[group_id]++;
    }

    // Allocate per-group value arrays (as f64)
    group_vals = (f64_t **)heap_alloc(agg.count * sizeof(f64_t *));
    cursors = (i64_t *)heap_alloc(agg.count * sizeof(i64_t));
    for (i = 0; i < agg.count; i++) {
        group_vals[i] = (f64_t *)heap_alloc(agg.counts[i] * sizeof(f64_t));
        cursors[i] = 0;
    }

    // Second pass: fill values (convert to f64)
    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        for (i = 0; i < nrows; i++) {
            group_id = group_ids[i];
            if (group_id >= 0)
                group_vals[group_id][cursors[group_id]++] = (f64_t)vals[i];
        }
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        for (i = 0; i < nrows; i++) {
            group_id = group_ids[i];
            if (group_id >= 0)
                group_vals[group_id][cursors[group_id]++] = vals[i];
        }
    } else if (val->type == TYPE_I32) {
        i32_t *vals = AS_I32(val);
        for (i = 0; i < nrows; i++) {
            group_id = group_ids[i];
            if (group_id >= 0)
                group_vals[group_id][cursors[group_id]++] = (f64_t)vals[i];
        }
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        for (i = 0; i < nrows; i++) {
            group_id = group_ids[i];
            if (group_id >= 0)
                group_vals[group_id][cursors[group_id]++] = (f64_t)vals[i];
        }
    } else {
        for (i = 0; i < agg.count; i++)
            heap_free(group_vals[i]);
        heap_free(group_vals);
        heap_free(cursors);
        heap_free(group_ids);
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    // Sort each group and extract median
    res = vector(TYPE_F64, agg.count);
    for (i = 0; i < agg.count; i++) {
        i64_t n = agg.counts[i];
        qsort(group_vals[i], n, sizeof(f64_t), cmp_f64);
        if (n % 2 == 1)
            AS_F64(res)[i] = group_vals[i][n / 2];
        else
            AS_F64(res)[i] = (group_vals[i][n / 2 - 1] + group_vals[i][n / 2]) / 2.0;
        heap_free(group_vals[i]);
    }

    heap_free(group_vals);
    heap_free(cursors);
    heap_free(group_ids);
    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_dev(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;
    f64_t *out;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return vector(TYPE_F64, 0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    // Repurpose maxs_f64 as sum-of-squares accumulator
    for (i = 0; i < agg.max_groups; i++)
        agg.maxs_f64[i] = 0.0;

    // Single-pass: accumulate sum (sums_f64) and sum_sq (maxs_f64)
    if (val->type == TYPE_I64) {
        i64_t *vals = AS_I64(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_I64) {
                f64_t v = (f64_t)vals[i];
                agg.sums_f64[group_id] += v;
                agg.maxs_f64[group_id] += v * v;
                agg.counts[group_id]++;
            }
        }
    } else if (val->type == TYPE_F64) {
        f64_t *vals = AS_F64(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0 && vals[i] != NULL_F64) {
                agg.sums_f64[group_id] += vals[i];
                agg.maxs_f64[group_id] += vals[i] * vals[i];
                agg.counts[group_id]++;
            }
        }
    } else if (val->type == TYPE_I32) {
        i32_t *vals = AS_I32(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0) {
                f64_t v = (f64_t)vals[i];
                agg.sums_f64[group_id] += v;
                agg.maxs_f64[group_id] += v * v;
                agg.counts[group_id]++;
            }
        }
    } else if (val->type == TYPE_I16) {
        i16_t *vals = AS_I16(val);
        for (i = 0; i < nrows; i++) {
            h = compute_composite_hash(keys, nkeys, i);
            group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (group_id >= 0) {
                f64_t v = (f64_t)vals[i];
                agg.sums_f64[group_id] += v;
                agg.maxs_f64[group_id] += v * v;
                agg.counts[group_id]++;
            }
        }
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    // Compute sample std dev: sqrt((sum_sq/n - (sum/n)^2) * n/(n-1))
    res = vector(TYPE_F64, agg.count);
    out = AS_F64(res);
    for (i = 0; i < agg.count; i++) {
        i64_t n = agg.counts[i];
        if (n <= 1) {
            out[i] = 0.0;
        } else {
            f64_t mean = agg.sums_f64[i] / n;
            f64_t var = agg.maxs_f64[i] / n - mean * mean;
            if (var < 0.0) var = 0.0;  // Numerical stability
            out[i] = sqrt(var * n / (n - 1));
        }
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_collect(obj_p val, obj_p index) {
    // Window-join path: collect matched values per window into lists
    if (index != NULL_OBJ && AS_LIST(index)[0]->i64 == INDEX_TYPE_WINDOW) {
        i64_t ll = AS_LIST(index)[1]->i64;
        i64_t li, ri, x, j;
        obj_p res = LIST(ll);

        for (i64_t i = 0; i < ll; i++) {
            if (wj_range(index, i, &li, &ri)) {
                i64_t cnt = ri - li + 1;
                obj_p v = vector(val->type, cnt);
                switch (val->type) {
                    case TYPE_I64: case TYPE_TIMESTAMP:
                        for (j = 0, x = li; x <= ri; x++, j++)
                            AS_I64(v)[j] = AS_I64(val)[x];
                        break;
                    case TYPE_F64:
                        for (j = 0, x = li; x <= ri; x++, j++)
                            AS_F64(v)[j] = AS_F64(val)[x];
                        break;
                    case TYPE_I32: case TYPE_DATE: case TYPE_TIME:
                        for (j = 0, x = li; x <= ri; x++, j++)
                            AS_I32(v)[j] = AS_I32(val)[x];
                        break;
                    case TYPE_I16:
                        for (j = 0, x = li; x <= ri; x++, j++)
                            AS_I16(v)[j] = AS_I16(val)[x];
                        break;
                    default:
                        for (j = 0, x = li; x <= ri; x++, j++)
                            AS_LIST(v)[j] = clone_obj(AS_LIST(val)[x]);
                        break;
                }
                AS_LIST(res)[i] = v;
            } else {
                AS_LIST(res)[i] = vector(val->type, 0);
            }
        }
        return res;
    }

    return aggr_first(val, index);
}

obj_p aggr_row(obj_p val, obj_p index) {
    query_ctx_p ctx;
    obj_p keys, res;
    i64_t nkeys, nrows, i, group_id;
    local_agg_t agg;
    u64_t h;
    i64_t *group_ids, *cursors;

    UNUSED(index);

    ctx = VM->query_ctx;
    if (ctx == NULL || ctx->groupby == NULL_OBJ)
        return err_domain(0, 0);

    keys = ctx->groupby;
    nkeys = keys->len;
    nrows = val->len;

    if (nrows == 0)
        return LIST(0);

    local_agg_init(&agg, INITIAL_HT_CAPACITY, nrows / 10 + 1024);

    // First pass: assign group for each row and count per group
    group_ids = (i64_t *)heap_alloc(nrows * sizeof(i64_t));

    for (i = 0; i < nrows; i++) {
        h = compute_composite_hash(keys, nkeys, i);
        group_id = local_agg_find_or_create(&agg, keys, nkeys, i, h);
        group_ids[i] = group_id;
        if (group_id >= 0)
            agg.counts[group_id]++;
    }

    // Allocate per-group i64 vectors
    res = LIST(agg.count);
    cursors = (i64_t *)heap_alloc(agg.count * sizeof(i64_t));

    for (i = 0; i < agg.count; i++) {
        AS_LIST(res)[i] = vector(TYPE_I64, agg.counts[i]);
        cursors[i] = 0;
    }

    // Second pass: fill row indices
    for (i = 0; i < nrows; i++) {
        group_id = group_ids[i];
        if (group_id >= 0)
            AS_I64(AS_LIST(res)[group_id])[cursors[group_id]++] = i;
    }

    heap_free(group_ids);
    heap_free(cursors);
    local_agg_destroy(&agg);

    return res;
}

// ============================================================================
// Fused aggregation (DuckDB-style single-pass)
// ============================================================================

i8_t aggr_identify_func(obj_p fn) {
    i64_t fp = fn->i64;
    if (fp == (i64_t)ray_sum)   return AGGR_ID_SUM;
    if (fp == (i64_t)ray_count) return AGGR_ID_COUNT;
    if (fp == (i64_t)ray_first) return AGGR_ID_FIRST;
    if (fp == (i64_t)ray_last)  return AGGR_ID_LAST;
    if (fp == (i64_t)ray_avg)   return AGGR_ID_AVG;
    if (fp == (i64_t)ray_max)   return AGGR_ID_MAX;
    if (fp == (i64_t)ray_min)   return AGGR_ID_MIN;
    if (fp == (i64_t)ray_med)   return AGGR_ID_MED;
    if (fp == (i64_t)ray_dev)   return AGGR_ID_DEV;
    return -1;
}

// Per-plan-entry accumulator
typedef struct {
    i64_t *i64_acc;    // sums (i64/i32/i16), min/max (i64/i32), count
    f64_t *f64_acc;    // sums (f64), min/max (f64), dev sum
    f64_t *f64_aux;    // dev sum_sq
    i64_t *counts;     // for avg/dev count
} fused_accum_t;

// Inner loop accumulation step (shared by perfect hash and general hash paths)
#define FUSED_ACCUM_STEP(func, gid, col, row, acc) \
    switch (func) { \
    case AGGR_ID_SUM: \
        if (col->type == TYPE_I64) { \
            i64_t v_ = AS_I64(col)[row]; \
            if (v_ != NULL_I64) (acc).i64_acc[gid] += v_; \
        } else if (col->type == TYPE_F64) { \
            f64_t v_ = AS_F64(col)[row]; \
            if (v_ != NULL_F64) (acc).f64_acc[gid] += v_; \
        } else if (col->type == TYPE_I32) { \
            (acc).i64_acc[gid] += (i64_t)AS_I32(col)[row]; \
        } else if (col->type == TYPE_I16) { \
            (acc).i64_acc[gid] += (i64_t)AS_I16(col)[row]; \
        } break; \
    case AGGR_ID_COUNT: (acc).i64_acc[gid]++; break; \
    case AGGR_ID_MAX: \
        if (col->type == TYPE_I64) { \
            i64_t v_ = AS_I64(col)[row]; \
            if (v_ != NULL_I64 && v_ > (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } else if (col->type == TYPE_F64) { \
            f64_t v_ = AS_F64(col)[row]; \
            if (v_ != NULL_F64 && v_ > (acc).f64_acc[gid]) (acc).f64_acc[gid] = v_; \
        } else if (col->type == TYPE_I32) { \
            i64_t v_ = (i64_t)AS_I32(col)[row]; \
            if (v_ > (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } else if (col->type == TYPE_I16) { \
            i64_t v_ = (i64_t)AS_I16(col)[row]; \
            if (v_ > (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } break; \
    case AGGR_ID_MIN: \
        if (col->type == TYPE_I64) { \
            i64_t v_ = AS_I64(col)[row]; \
            if (v_ != NULL_I64 && v_ < (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } else if (col->type == TYPE_F64) { \
            f64_t v_ = AS_F64(col)[row]; \
            if (v_ != NULL_F64 && v_ < (acc).f64_acc[gid]) (acc).f64_acc[gid] = v_; \
        } else if (col->type == TYPE_I32) { \
            i64_t v_ = (i64_t)AS_I32(col)[row]; \
            if (v_ < (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } else if (col->type == TYPE_I16) { \
            i64_t v_ = (i64_t)AS_I16(col)[row]; \
            if (v_ < (acc).i64_acc[gid]) (acc).i64_acc[gid] = v_; \
        } break; \
    case AGGR_ID_AVG: { \
        f64_t av_; \
        if (col->type == TYPE_I64) { \
            i64_t iv_ = AS_I64(col)[row]; \
            if (iv_ == NULL_I64) break; av_ = (f64_t)iv_; \
        } else if (col->type == TYPE_F64) { \
            av_ = AS_F64(col)[row]; if (av_ == NULL_F64) break; \
        } else if (col->type == TYPE_I32) { av_ = (f64_t)AS_I32(col)[row]; \
        } else if (col->type == TYPE_I16) { av_ = (f64_t)AS_I16(col)[row]; \
        } else break; \
        (acc).f64_acc[gid] += av_; (acc).counts[gid]++; break; } \
    case AGGR_ID_DEV: { \
        f64_t dv_; \
        if (col->type == TYPE_I64) { \
            i64_t iv_ = AS_I64(col)[row]; \
            if (iv_ == NULL_I64) break; dv_ = (f64_t)iv_; \
        } else if (col->type == TYPE_F64) { \
            dv_ = AS_F64(col)[row]; if (dv_ == NULL_F64) break; \
        } else if (col->type == TYPE_I32) { dv_ = (f64_t)AS_I32(col)[row]; \
        } else if (col->type == TYPE_I16) { dv_ = (f64_t)AS_I16(col)[row]; \
        } else break; \
        (acc).f64_acc[gid] += dv_; (acc).f64_aux[gid] += dv_ * dv_; (acc).counts[gid]++; break; } \
    default: break; }

// Allocate per-plan accumulators with given capacity
static fused_accum_t *fused_alloc_accum(obj_p tab_vals, fused_plan_t *plan, i64_t nplan, i64_t cap) {
    i64_t p, i;
    fused_accum_t *accum = nplan > 0 ? (fused_accum_t *)heap_alloc(nplan * sizeof(fused_accum_t)) : NULL;
    for (p = 0; p < nplan; p++) {
        i8_t func = plan[p].func_id;
        accum[p].i64_acc = NULL; accum[p].f64_acc = NULL;
        accum[p].f64_aux = NULL; accum[p].counts = NULL;

        if (func == AGGR_ID_SUM || func == AGGR_ID_MIN || func == AGGR_ID_MAX) {
            obj_p col = AS_LIST(tab_vals)[plan[p].col_idx];
            if (col->type == TYPE_F64) {
                accum[p].f64_acc = (f64_t *)heap_alloc(cap * sizeof(f64_t));
                for (i = 0; i < cap; i++)
                    accum[p].f64_acc[i] = (func == AGGR_ID_SUM) ? 0.0
                        : (func == AGGR_ID_MIN) ? AGG_F64_MAX : -AGG_F64_MAX;
            } else {
                accum[p].i64_acc = (i64_t *)heap_alloc(cap * sizeof(i64_t));
                for (i = 0; i < cap; i++)
                    accum[p].i64_acc[i] = (func == AGGR_ID_SUM) ? 0
                        : (func == AGGR_ID_MIN) ? AGG_I64_MAX : AGG_I64_MIN;
            }
        } else if (func == AGGR_ID_COUNT) {
            accum[p].i64_acc = (i64_t *)heap_alloc(cap * sizeof(i64_t));
            for (i = 0; i < cap; i++) accum[p].i64_acc[i] = 0;
        } else if (func == AGGR_ID_AVG) {
            accum[p].f64_acc = (f64_t *)heap_alloc(cap * sizeof(f64_t));
            accum[p].counts = (i64_t *)heap_alloc(cap * sizeof(i64_t));
            for (i = 0; i < cap; i++) { accum[p].f64_acc[i] = 0.0; accum[p].counts[i] = 0; }
        } else if (func == AGGR_ID_DEV) {
            accum[p].f64_acc = (f64_t *)heap_alloc(cap * sizeof(f64_t));
            accum[p].f64_aux = (f64_t *)heap_alloc(cap * sizeof(f64_t));
            accum[p].counts = (i64_t *)heap_alloc(cap * sizeof(i64_t));
            for (i = 0; i < cap; i++) {
                accum[p].f64_acc[i] = 0.0; accum[p].f64_aux[i] = 0.0; accum[p].counts[i] = 0;
            }
        }
    }
    return accum;
}

static nil_t fused_free_accum(fused_accum_t *accum, i64_t nplan) {
    for (i64_t p = 0; p < nplan; p++) {
        if (accum[p].i64_acc) heap_free(accum[p].i64_acc);
        if (accum[p].f64_acc) heap_free(accum[p].f64_acc);
        if (accum[p].f64_aux) heap_free(accum[p].f64_aux);
        if (accum[p].counts) heap_free(accum[p].counts);
    }
    if (accum) heap_free(accum);
}

// Extract result vectors from accumulators, compacting non-empty groups.
// slot_counts[s] > 0 means slot s is a valid group. ng = total valid groups.
static nil_t fused_extract_results(
    obj_p tab_vals, fused_plan_t *plan, i64_t nplan, fused_accum_t *accum,
    i64_t *slot_counts, i64_t nslots, i64_t ng,
    i64_t *first_rows, i64_t *last_rows, obj_p *results)
{
    for (i64_t p = 0; p < nplan; p++) {
        i8_t func = plan[p].func_id;
        obj_p col = AS_LIST(tab_vals)[plan[p].col_idx];
        i64_t gi = 0;

        switch (func) {
        case AGGR_ID_SUM:
            if (col->type == TYPE_F64) {
                results[p] = vector(TYPE_F64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_F64(results[p])[gi++] = accum[p].f64_acc[s];
            } else {
                results[p] = vector(TYPE_I64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_I64(results[p])[gi++] = accum[p].i64_acc[s];
            }
            break;
        case AGGR_ID_COUNT:
            results[p] = vector(TYPE_I64, ng);
            for (i64_t s = 0; s < nslots; s++)
                if (slot_counts[s] > 0) AS_I64(results[p])[gi++] = accum[p].i64_acc[s];
            break;
        case AGGR_ID_MAX:
            if (col->type == TYPE_F64) {
                results[p] = vector(TYPE_F64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0)
                        AS_F64(results[p])[gi++] = (accum[p].f64_acc[s] != -AGG_F64_MAX) ? accum[p].f64_acc[s] : NULL_F64;
            } else if (col->type == TYPE_I32) {
                results[p] = vector(TYPE_I32, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_I32(results[p])[gi++] = (i32_t)accum[p].i64_acc[s];
            } else if (col->type == TYPE_I16) {
                results[p] = vector(TYPE_I16, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_I16(results[p])[gi++] = (i16_t)accum[p].i64_acc[s];
            } else {
                results[p] = vector(TYPE_I64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0)
                        AS_I64(results[p])[gi++] = (accum[p].i64_acc[s] != AGG_I64_MIN) ? accum[p].i64_acc[s] : NULL_I64;
            }
            break;
        case AGGR_ID_MIN:
            if (col->type == TYPE_F64) {
                results[p] = vector(TYPE_F64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0)
                        AS_F64(results[p])[gi++] = (accum[p].f64_acc[s] != AGG_F64_MAX) ? accum[p].f64_acc[s] : NULL_F64;
            } else if (col->type == TYPE_I32) {
                results[p] = vector(TYPE_I32, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_I32(results[p])[gi++] = (i32_t)accum[p].i64_acc[s];
            } else if (col->type == TYPE_I16) {
                results[p] = vector(TYPE_I16, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0) AS_I16(results[p])[gi++] = (i16_t)accum[p].i64_acc[s];
            } else {
                results[p] = vector(TYPE_I64, ng);
                for (i64_t s = 0; s < nslots; s++)
                    if (slot_counts[s] > 0)
                        AS_I64(results[p])[gi++] = (accum[p].i64_acc[s] != AGG_I64_MAX) ? accum[p].i64_acc[s] : NULL_I64;
            }
            break;
        case AGGR_ID_AVG: {
            results[p] = vector(TYPE_F64, ng);
            f64_t *out = AS_F64(results[p]);
            for (i64_t s = 0; s < nslots; s++)
                if (slot_counts[s] > 0)
                    out[gi++] = (accum[p].counts[s] > 0) ? accum[p].f64_acc[s] / accum[p].counts[s] : 0.0;
            break;
        }
        case AGGR_ID_DEV: {
            results[p] = vector(TYPE_F64, ng);
            f64_t *out = AS_F64(results[p]);
            for (i64_t s = 0; s < nslots; s++) {
                if (slot_counts[s] <= 0) continue;
                i64_t n = accum[p].counts[s];
                if (n <= 1) { out[gi++] = 0.0; continue; }
                f64_t mean = accum[p].f64_acc[s] / n;
                f64_t var = accum[p].f64_aux[s] / n - mean * mean;
                if (var < 0.0) var = 0.0;
                out[gi++] = sqrt(var * n / (n - 1));
            }
            break;
        }
        case AGGR_ID_FIRST:
            results[p] = at_ids(col, first_rows, ng);
            break;
        case AGGR_ID_LAST:
            results[p] = at_ids(col, last_rows, ng);
            break;
        default:
            results[p] = vector(TYPE_I64, 0);
            break;
        }
    }
}

// Parallel fused aggregation context (perfect hash path)
typedef struct {
    i64_t *kv;                  // Key values (shared, read-only)
    i64_t mn;                   // Min key value (offset for direct indexing)
    i64_t range;                // Key range
    obj_p tab_vals;             // Table values (shared, read-only)
    fused_plan_t *plan;         // Aggregation plan (shared, read-only)
    i64_t nplan;                // Number of plan entries
    i64_t chunk_size;           // Rows per worker
    fused_accum_t **worker_acc; // Per-worker accumulators
    i64_t **worker_counts;      // Per-worker group counts
    i64_t **worker_first;       // Per-worker first rows
    i64_t **worker_last;        // Per-worker last rows
} parallel_fused_ctx_t;

// Worker: process a chunk of rows using perfect hash direct indexing
static obj_p parallel_fused_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_fused_ctx_t *ctx = (parallel_fused_ctx_t *)ctx_ptr;
    i64_t chunk_idx = offset / ctx->chunk_size;
    i64_t end = offset + len;
    i64_t mn = ctx->mn;
    i64_t *kv = ctx->kv;
    i64_t *pcounts = ctx->worker_counts[chunk_idx];
    i64_t *pfirst = ctx->worker_first[chunk_idx];
    i64_t *plast = ctx->worker_last[chunk_idx];
    fused_accum_t *accum = ctx->worker_acc[chunk_idx];
    fused_plan_t *plan = ctx->plan;
    i64_t nplan = ctx->nplan;
    obj_p tab_vals = ctx->tab_vals;
    i64_t i, p, gid;

    for (i = offset; i < end; i++) {
        gid = kv[i] - mn;
        if (pfirst[gid] < 0) pfirst[gid] = i;
        plast[gid] = i;
        pcounts[gid]++;
        for (p = 0; p < nplan; p++) {
            obj_p col = AS_LIST(tab_vals)[plan[p].col_idx];
            FUSED_ACCUM_STEP(plan[p].func_id, gid, col, i, accum[p]);
        }
    }

    return NULL_OBJ;
}

nil_t aggr_fused_compute(struct query_ctx_t *ctx, fused_plan_t *plan, i64_t nplan, obj_p *results) {
    query_ctx_p qctx = (query_ctx_p)ctx;
    obj_p keys = qctx->groupby;
    i64_t nkeys = keys->len;
    obj_p tab_vals = AS_LIST(qctx->table)[1];
    i64_t nrows = tab_vals->len > 0 ? AS_LIST(tab_vals)[0]->len : 0;
    i64_t p, i, gid;

    if (nrows == 0) {
        qctx->ngroups = 0;
        qctx->first_rows = NULL;
        qctx->last_rows = NULL;
        for (p = 0; p < nplan; p++)
            results[p] = vector(TYPE_I64, 0);
        return;
    }

    // Perfect hash path: single i64/symbol key with small range → direct indexing
    if (nkeys == 1) {
        obj_p key_col = AS_LIST(keys)[0];
        if (key_col->type == TYPE_I64 || key_col->type == TYPE_SYMBOL) {
            i64_t *kv = AS_I64(key_col);
            i64_t mn = kv[0], mx = kv[0];
            for (i = 1; i < nrows; i++) {
                if (kv[i] < mn) mn = kv[i];
                if (kv[i] > mx) mx = kv[i];
            }
            i64_t range = mx - mn + 1;

            if (range > 0 && range <= PERFECT_HASH_THRESHOLD) {
                // Parallel path for large tables
                pool_p pool = pool_get();
                i64_t nworkers = pool_split_by(pool, nrows, 0);
                if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;

                if (nworkers > 1 && nrows >= PARALLEL_AGG_THRESHOLD) {
                    i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));

                    // Allocate per-worker state
                    parallel_fused_ctx_t pctx;
                    pctx.kv = kv;
                    pctx.mn = mn;
                    pctx.range = range;
                    pctx.tab_vals = tab_vals;
                    pctx.plan = plan;
                    pctx.nplan = nplan;
                    pctx.chunk_size = chunk_size;
                    pctx.worker_acc = (fused_accum_t **)heap_alloc(nworkers * sizeof(fused_accum_t *));
                    pctx.worker_counts = (i64_t **)heap_alloc(nworkers * sizeof(i64_t *));
                    pctx.worker_first = (i64_t **)heap_alloc(nworkers * sizeof(i64_t *));
                    pctx.worker_last = (i64_t **)heap_alloc(nworkers * sizeof(i64_t *));

                    for (i = 0; i < nworkers; i++) {
                        pctx.worker_acc[i] = fused_alloc_accum(tab_vals, plan, nplan, range);
                        pctx.worker_counts[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        pctx.worker_first[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        pctx.worker_last[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        for (i64_t j = 0; j < range; j++) {
                            pctx.worker_counts[i][j] = 0;
                            pctx.worker_first[i][j] = -1;
                            pctx.worker_last[i][j] = -1;
                        }
                    }

                    // Submit tasks
                    pool_prepare(pool);
                    i64_t offset = 0;
                    for (i = 0; i < nworkers - 1; i++) {
                        pool_add_task(pool, (raw_p)parallel_fused_worker, 3, chunk_size, offset, &pctx);
                        offset += chunk_size;
                    }
                    pool_add_task(pool, (raw_p)parallel_fused_worker, 3, nrows - offset, offset, &pctx);

                    obj_p pres = pool_run(pool);
                    drop_obj(pres);

                    // Merge: combine per-worker accumulators
                    i64_t *pcounts = pctx.worker_counts[0];
                    i64_t *pfirst = pctx.worker_first[0];
                    i64_t *plast = pctx.worker_last[0];
                    fused_accum_t *accum = pctx.worker_acc[0];

                    for (i64_t w = 1; w < nworkers; w++) {
                        for (i64_t s = 0; s < range; s++) {
                            pcounts[s] += pctx.worker_counts[w][s];
                            if (pctx.worker_first[w][s] >= 0) {
                                if (pfirst[s] < 0 || pctx.worker_first[w][s] < pfirst[s])
                                    pfirst[s] = pctx.worker_first[w][s];
                            }
                            if (pctx.worker_last[w][s] >= 0) {
                                if (pctx.worker_last[w][s] > plast[s])
                                    plast[s] = pctx.worker_last[w][s];
                            }
                        }
                        // Merge accumulators per plan entry
                        for (p = 0; p < nplan; p++) {
                            i8_t func = plan[p].func_id;
                            fused_accum_t *wa = &pctx.worker_acc[w][p];
                            fused_accum_t *ma = &accum[p];
                            for (i64_t s = 0; s < range; s++) {
                                switch (func) {
                                case AGGR_ID_SUM:
                                case AGGR_ID_COUNT:
                                    if (ma->i64_acc && wa->i64_acc) ma->i64_acc[s] += wa->i64_acc[s];
                                    if (ma->f64_acc && wa->f64_acc) ma->f64_acc[s] += wa->f64_acc[s];
                                    break;
                                case AGGR_ID_MAX:
                                    if (ma->i64_acc && wa->i64_acc && wa->i64_acc[s] > ma->i64_acc[s])
                                        ma->i64_acc[s] = wa->i64_acc[s];
                                    if (ma->f64_acc && wa->f64_acc && wa->f64_acc[s] > ma->f64_acc[s])
                                        ma->f64_acc[s] = wa->f64_acc[s];
                                    break;
                                case AGGR_ID_MIN:
                                    if (ma->i64_acc && wa->i64_acc && wa->i64_acc[s] < ma->i64_acc[s])
                                        ma->i64_acc[s] = wa->i64_acc[s];
                                    if (ma->f64_acc && wa->f64_acc && wa->f64_acc[s] < ma->f64_acc[s])
                                        ma->f64_acc[s] = wa->f64_acc[s];
                                    break;
                                case AGGR_ID_AVG:
                                case AGGR_ID_DEV:
                                    if (ma->f64_acc && wa->f64_acc) ma->f64_acc[s] += wa->f64_acc[s];
                                    if (ma->f64_aux && wa->f64_aux) ma->f64_aux[s] += wa->f64_aux[s];
                                    if (ma->counts && wa->counts) ma->counts[s] += wa->counts[s];
                                    break;
                                default: break;
                                }
                            }
                        }
                        // Free worker w's state
                        heap_free(pctx.worker_counts[w]);
                        heap_free(pctx.worker_first[w]);
                        heap_free(pctx.worker_last[w]);
                        fused_free_accum(pctx.worker_acc[w], nplan);
                    }

                    // Compact non-empty groups
                    i64_t ng = 0;
                    for (i = 0; i < range; i++)
                        if (pcounts[i] > 0) ng++;

                    qctx->ngroups = ng;
                    qctx->first_rows = (i64_t *)heap_alloc(ng * sizeof(i64_t));
                    qctx->last_rows = (i64_t *)heap_alloc(ng * sizeof(i64_t));
                    i64_t gi = 0;
                    for (i = 0; i < range; i++) {
                        if (pcounts[i] > 0) {
                            qctx->first_rows[gi] = pfirst[i];
                            qctx->last_rows[gi] = plast[i];
                            gi++;
                        }
                    }

                    fused_extract_results(tab_vals, plan, nplan, accum,
                                          pcounts, range, ng,
                                          qctx->first_rows, qctx->last_rows, results);

                    heap_free(pcounts); heap_free(pfirst); heap_free(plast);
                    fused_free_accum(accum, nplan);
                    heap_free(pctx.worker_acc);
                    heap_free(pctx.worker_counts);
                    heap_free(pctx.worker_first);
                    heap_free(pctx.worker_last);
                    return;
                }

                // Sequential perfect hash path
                i64_t *pcounts = (i64_t *)heap_alloc(range * sizeof(i64_t));
                i64_t *pfirst = (i64_t *)heap_alloc(range * sizeof(i64_t));
                i64_t *plast = (i64_t *)heap_alloc(range * sizeof(i64_t));
                for (i = 0; i < range; i++) { pcounts[i] = 0; pfirst[i] = -1; plast[i] = -1; }

                fused_accum_t *accum = fused_alloc_accum(tab_vals, plan, nplan, range);

                for (i = 0; i < nrows; i++) {
                    gid = kv[i] - mn;
                    if (pfirst[gid] < 0) pfirst[gid] = i;
                    plast[gid] = i;
                    pcounts[gid]++;
                    for (p = 0; p < nplan; p++) {
                        obj_p col = AS_LIST(tab_vals)[plan[p].col_idx];
                        FUSED_ACCUM_STEP(plan[p].func_id, gid, col, i, accum[p]);
                    }
                }

                // Compact non-empty groups
                i64_t ng = 0;
                for (i = 0; i < range; i++)
                    if (pcounts[i] > 0) ng++;

                qctx->ngroups = ng;
                qctx->first_rows = (i64_t *)heap_alloc(ng * sizeof(i64_t));
                qctx->last_rows = (i64_t *)heap_alloc(ng * sizeof(i64_t));
                i64_t gi = 0;
                for (i = 0; i < range; i++) {
                    if (pcounts[i] > 0) {
                        qctx->first_rows[gi] = pfirst[i];
                        qctx->last_rows[gi] = plast[i];
                        gi++;
                    }
                }

                fused_extract_results(tab_vals, plan, nplan, accum,
                                      pcounts, range, ng,
                                      qctx->first_rows, qctx->last_rows, results);

                heap_free(pcounts); heap_free(pfirst); heap_free(plast);
                fused_free_accum(accum, nplan);
                return;
            }
        }
    }

    // General hash path: arbitrary keys
    local_agg_t agg;
    i64_t max_groups = nrows / 10 + 1024;
    local_agg_init(&agg, INITIAL_HT_CAPACITY, max_groups);
    fused_accum_t *accum = fused_alloc_accum(tab_vals, plan, nplan, max_groups);

    for (i = 0; i < nrows; i++) {
        u64_t h = compute_composite_hash(keys, nkeys, i);
        gid = local_agg_find_or_create(&agg, keys, nkeys, i, h);
        if (UNLIKELY(gid < 0)) continue;
        for (p = 0; p < nplan; p++) {
            obj_p col = AS_LIST(tab_vals)[plan[p].col_idx];
            FUSED_ACCUM_STEP(plan[p].func_id, gid, col, i, accum[p]);
        }
    }

    i64_t ng = agg.count;
    qctx->ngroups = ng;
    qctx->first_rows = agg.first_rows;
    qctx->last_rows = agg.last_rows;
    agg.first_rows = NULL;
    agg.last_rows = NULL;

    i64_t *hcounts = (i64_t *)heap_alloc(ng * sizeof(i64_t));
    for (i = 0; i < ng; i++) hcounts[i] = 1;

    fused_extract_results(tab_vals, plan, nplan, accum,
                          hcounts, ng, ng,
                          qctx->first_rows, qctx->last_rows, results);

    heap_free(hcounts);
    fused_free_accum(accum, nplan);
    local_agg_destroy(&agg);
}

#undef FUSED_ACCUM_STEP

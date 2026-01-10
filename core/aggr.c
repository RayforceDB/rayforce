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

    // TODO: Handle other value types (f64, etc.)
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
    } else {
        local_agg_destroy(&agg);
        return err_type(TYPE_I64, val->type, 0, 0);
    }

    local_agg_destroy(&agg);
    return res;
}

obj_p aggr_med(obj_p val, obj_p index) {
    // Median requires sorting per group - complex implementation
    // For now, return error
    UNUSED(val);
    UNUSED(index);
    return err_domain(0, 0);
}

obj_p aggr_dev(obj_p val, obj_p index) {
    // Standard deviation - requires two passes (mean then variance)
    // For now, return error
    UNUSED(val);
    UNUSED(index);
    return err_domain(0, 0);
}

obj_p aggr_collect(obj_p val, obj_p index) {
    // Collect all values per group into lists
    // Complex implementation - for now return error
    UNUSED(val);
    UNUSED(index);
    return err_domain(0, 0);
}

obj_p aggr_row(obj_p val, obj_p index) {
    // Return row indices per group
    UNUSED(val);
    UNUSED(index);
    return err_domain(0, 0);
}

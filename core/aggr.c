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
#include <time.h>

#ifdef DEBUG
#define PH_PHASE_START(name) struct timespec _ph_ts_##name; clock_gettime(CLOCK_MONOTONIC, &_ph_ts_##name);
#define PH_PHASE_END(name) { struct timespec _ph_te; clock_gettime(CLOCK_MONOTONIC, &_ph_te); \
    double _ph_ms = (_ph_te.tv_sec - _ph_ts_##name.tv_sec)*1e3 + (_ph_te.tv_nsec - _ph_ts_##name.tv_nsec)/1e6; \
    fprintf(stderr, "  ph " #name ": %.2fms\n", _ph_ms); }
#else
#define PH_PHASE_START(name) ((void)0);
#define PH_PHASE_END(name) ((void)0);
#endif

// ============================================================================
// Constants
// ============================================================================

#define PERFECT_HASH_THRESHOLD 262144  // Use perfect hash if range <= 256K
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

// Grow per-group arrays when capacity is exhausted
static nil_t local_agg_grow_groups(local_agg_t *agg) {
    i64_t old_max = agg->max_groups;
    i64_t new_max = old_max * 2;
    i64_t i;

    agg->sums_i64 = (i64_t *)heap_realloc(agg->sums_i64, new_max * sizeof(i64_t));
    agg->sums_f64 = (f64_t *)heap_realloc(agg->sums_f64, new_max * sizeof(f64_t));
    agg->counts = (i64_t *)heap_realloc(agg->counts, new_max * sizeof(i64_t));
    agg->mins_i64 = (i64_t *)heap_realloc(agg->mins_i64, new_max * sizeof(i64_t));
    agg->maxs_i64 = (i64_t *)heap_realloc(agg->maxs_i64, new_max * sizeof(i64_t));
    agg->mins_f64 = (f64_t *)heap_realloc(agg->mins_f64, new_max * sizeof(f64_t));
    agg->maxs_f64 = (f64_t *)heap_realloc(agg->maxs_f64, new_max * sizeof(f64_t));
    agg->first_rows = (i64_t *)heap_realloc(agg->first_rows, new_max * sizeof(i64_t));
    agg->last_rows = (i64_t *)heap_realloc(agg->last_rows, new_max * sizeof(i64_t));
    agg->group_hashes = (u64_t *)heap_realloc(agg->group_hashes, new_max * sizeof(u64_t));

    for (i = old_max; i < new_max; i++) {
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

    agg->max_groups = new_max;
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
                local_agg_grow_groups(agg);
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
// Fused aggregation (single-pass)
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
            obj_p col = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(tab_vals)[plan[p].col_idx];
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

// Grow fused accum arrays from old_cap to new_cap, preserving existing data
static nil_t fused_grow_accum(fused_accum_t *accum, fused_plan_t *plan, i64_t nplan,
                              i64_t old_cap, i64_t new_cap) {
    for (i64_t p = 0; p < nplan; p++) {
        i8_t func = plan[p].func_id;
        if (accum[p].i64_acc) {
            accum[p].i64_acc = (i64_t *)heap_realloc(accum[p].i64_acc, new_cap * sizeof(i64_t));
            i64_t init = (func == AGGR_ID_MIN) ? AGG_I64_MAX
                       : (func == AGGR_ID_MAX) ? AGG_I64_MIN : 0;
            for (i64_t i = old_cap; i < new_cap; i++) accum[p].i64_acc[i] = init;
        }
        if (accum[p].f64_acc) {
            accum[p].f64_acc = (f64_t *)heap_realloc(accum[p].f64_acc, new_cap * sizeof(f64_t));
            f64_t finit = (func == AGGR_ID_MIN) ? AGG_F64_MAX
                        : (func == AGGR_ID_MAX) ? -AGG_F64_MAX : 0.0;
            for (i64_t i = old_cap; i < new_cap; i++) accum[p].f64_acc[i] = finit;
        }
        if (accum[p].f64_aux) {
            accum[p].f64_aux = (f64_t *)heap_realloc(accum[p].f64_aux, new_cap * sizeof(f64_t));
            for (i64_t i = old_cap; i < new_cap; i++) accum[p].f64_aux[i] = 0.0;
        }
        if (accum[p].counts) {
            accum[p].counts = (i64_t *)heap_realloc(accum[p].counts, new_cap * sizeof(i64_t));
            for (i64_t i = old_cap; i < new_cap; i++) accum[p].counts[i] = 0;
        }
    }
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
        obj_p col = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(tab_vals)[plan[p].col_idx];
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
    i64_t *kv;                  // Key values (shared, read-only) — NULL for multi-key inline
    i64_t mn;                   // Min key value (offset for direct indexing)
    i64_t range;                // Key range
    obj_p tab_vals;             // Table values for aggregation (shared, read-only)
    i64_t *fids;                // Filter indices (NULL if no filter)
    fused_plan_t *plan;         // Aggregation plan (shared, read-only)
    i64_t nplan;                // Number of plan entries
    i64_t chunk_size;           // Rows per worker
    fused_accum_t **worker_acc; // Per-worker accumulators
    i64_t **worker_counts;      // Per-worker group counts
    i64_t **worker_first;       // Per-worker first rows
    i64_t **worker_last;        // Per-worker last rows
    // Multi-key inline composite: compute gid on-the-fly instead of pre-computing ckv
    i64_t nkeys_inline;         // 0 = use kv[], >0 = compute composite key inline
    i64_t **key_ptrs;           // [nkeys] pointers to key value arrays
    i64_t *mins;                // [nkeys] per-key minimums
    i64_t *strides;             // [nkeys] per-key strides
    // Boolean-fused filter: iterate all rows, skip non-matching
    b8_t *filter_bool;          // Boolean vector (NULL if no filter or using fids)
} parallel_fused_ctx_t;

// Parallel min/max scan context and worker
typedef struct {
    i64_t *kv;                  // Key values (single key)
    i64_t chunk_size;
    i64_t *worker_mins;
    i64_t *worker_maxs;
} parallel_minmax_ctx_t;

typedef struct {
    i64_t nkeys;
    i64_t *key_data[16];
    i64_t chunk_size;
    i64_t (*worker_mins)[16];   // [nworkers][nkeys]
    i64_t (*worker_maxs)[16];   // [nworkers][nkeys]
} parallel_multikey_minmax_ctx_t;

static obj_p parallel_minmax_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_minmax_ctx_t *ctx = (parallel_minmax_ctx_t *)ctx_ptr;
    i64_t chunk_idx = offset / ctx->chunk_size;
    i64_t *kv = ctx->kv;
    i64_t end = offset + len;
    i64_t mn = kv[offset], mx = kv[offset];
    for (i64_t i = offset + 1; i < end; i++) {
        if (kv[i] < mn) mn = kv[i];
        if (kv[i] > mx) mx = kv[i];
    }
    ctx->worker_mins[chunk_idx] = mn;
    ctx->worker_maxs[chunk_idx] = mx;
    return NULL_OBJ;
}

// Helper: parallel min/max scan for a single key array.
// Returns 0 on success (mn/mx set), or -1 if no workers available (caller should scan sequentially).
static i64_t parallel_minmax_scan(pool_p pool, i64_t *kv, i64_t nrows, i64_t *out_mn, i64_t *out_mx) {
    i64_t nworkers = pool_split_by(pool, nrows, 0);
    if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;
    if (nworkers <= 1 || nrows < PARALLEL_AGG_THRESHOLD)
        return -1;

    i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));
    i64_t wmins[MAX_AGG_WORKERS], wmaxs[MAX_AGG_WORKERS];

    parallel_minmax_ctx_t mmctx;
    mmctx.kv = kv;
    mmctx.chunk_size = chunk_size;
    mmctx.worker_mins = wmins;
    mmctx.worker_maxs = wmaxs;

    pool_prepare_light(pool);
    i64_t offset = 0, i;
    for (i = 0; i < nworkers - 1; i++) {
        pool_add_task(pool, (raw_p)parallel_minmax_worker, 3, chunk_size, offset, &mmctx);
        offset += chunk_size;
    }
    pool_add_task(pool, (raw_p)parallel_minmax_worker, 3, nrows - offset, offset, &mmctx);

    pool_run_light(pool);

    i64_t mn = wmins[0], mx = wmaxs[0];
    for (i = 1; i < nworkers; i++) {
        if (wmins[i] < mn) mn = wmins[i];
        if (wmaxs[i] > mx) mx = wmaxs[i];
    }
    *out_mn = mn;
    *out_mx = mx;
    return 0;
}

// Worker: scan min/max for all keys in a single pass
static obj_p parallel_multikey_minmax_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_multikey_minmax_ctx_t *ctx = (parallel_multikey_minmax_ctx_t *)ctx_ptr;
    i64_t chunk_idx = offset / ctx->chunk_size;
    i64_t end = offset + len;
    i64_t nk = ctx->nkeys;
    for (i64_t k = 0; k < nk; k++) {
        i64_t *kd = ctx->key_data[k];
        i64_t mn = kd[offset], mx = kd[offset];
        for (i64_t r = offset + 1; r < end; r++) {
            if (kd[r] < mn) mn = kd[r];
            if (kd[r] > mx) mx = kd[r];
        }
        ctx->worker_mins[chunk_idx][k] = mn;
        ctx->worker_maxs[chunk_idx][k] = mx;
    }
    return NULL_OBJ;
}

// Helper: parallel min/max scan for multiple keys in a single pool dispatch
static i64_t parallel_multikey_minmax_scan(pool_p pool, i64_t nkeys, i64_t *key_data[],
                                           i64_t nrows, i64_t *out_mins, i64_t *out_maxs) {
    i64_t nworkers = pool_split_by(pool, nrows, 0);
    if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;
    if (nworkers <= 1 || nrows < PARALLEL_AGG_THRESHOLD)
        return -1;

    i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));
    i64_t wmins[MAX_AGG_WORKERS][16], wmaxs[MAX_AGG_WORKERS][16];

    parallel_multikey_minmax_ctx_t mmctx;
    mmctx.nkeys = nkeys;
    mmctx.chunk_size = chunk_size;
    mmctx.worker_mins = wmins;
    mmctx.worker_maxs = wmaxs;
    for (i64_t k = 0; k < nkeys; k++)
        mmctx.key_data[k] = key_data[k];

    pool_prepare_light(pool);
    i64_t offset = 0, i;
    for (i = 0; i < nworkers - 1; i++) {
        pool_add_task(pool, (raw_p)parallel_multikey_minmax_worker, 3, chunk_size, offset, &mmctx);
        offset += chunk_size;
    }
    pool_add_task(pool, (raw_p)parallel_multikey_minmax_worker, 3, nrows - offset, offset, &mmctx);

    pool_run_light(pool);

    // Reduce per-worker results
    for (i64_t k = 0; k < nkeys; k++) {
        i64_t mn = wmins[0][k], mx = wmaxs[0][k];
        for (i = 1; i < nworkers; i++) {
            if (wmins[i][k] < mn) mn = wmins[i][k];
            if (wmaxs[i][k] > mx) mx = wmaxs[i][k];
        }
        out_mins[k] = mn;
        out_maxs[k] = mx;
    }
    return 0;
}

// Worker: process a chunk of rows using perfect hash direct indexing
static obj_p parallel_fused_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_fused_ctx_t *ctx = (parallel_fused_ctx_t *)ctx_ptr;
    i64_t chunk_idx = offset / ctx->chunk_size;
    i64_t end = offset + len;
    i64_t mn = ctx->mn;
    i64_t *kv = ctx->kv;
    i64_t *fids = ctx->fids;
    b8_t *fbool = ctx->filter_bool;
    i64_t *pcounts = ctx->worker_counts[chunk_idx];
    i64_t *pfirst = ctx->worker_first ? ctx->worker_first[chunk_idx] : NULL;
    i64_t *plast = ctx->worker_last ? ctx->worker_last[chunk_idx] : NULL;
    fused_accum_t *accum = ctx->worker_acc[chunk_idx];
    fused_plan_t *plan = ctx->plan;
    i64_t nplan = ctx->nplan;
    obj_p tab_vals = ctx->tab_vals;
    i64_t i, p, gid;

    // Multi-key inline: compute composite key on-the-fly
    i64_t nki = ctx->nkeys_inline;
    i64_t **kptrs = ctx->key_ptrs;
    i64_t *kmins = ctx->mins;
    i64_t *kstrides = ctx->strides;

    // Pre-resolve column object pointers (eliminate per-row AS_LIST indirection)
    obj_p cols[MAX_FUSED];
    for (p = 0; p < nplan; p++)
        cols[p] = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(tab_vals)[plan[p].col_idx];

    // Detect specialized fast paths: all plans have same func + same type
    b8_t all_same = (nplan > 0) ? B8_TRUE : B8_FALSE;
    b8_t same_func = (nplan > 0) ? B8_TRUE : B8_FALSE;
    i8_t common_func = nplan > 0 ? plan[0].func_id : -1;
    i8_t common_type = nplan > 0 ? cols[0]->type : -1;
    for (p = 1; p < nplan; p++) {
        if (plan[p].func_id != common_func) same_func = B8_FALSE;
        if (plan[p].func_id != common_func || cols[p]->type != common_type) {
            all_same = B8_FALSE;
        }
    }

    // === Specialized AVG-I64 fast path (Q4 pattern) ===
    if (all_same && common_func == AGGR_ID_AVG && common_type == TYPE_I64 && !fbool && !fids && nki == 0) {
        i64_t *cv[MAX_FUSED];
        for (p = 0; p < nplan; p++)
            cv[p] = AS_I64(cols[p]);
        if (pfirst) {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                if (pfirst[gid] < 0) pfirst[gid] = i;
                plast[gid] = i;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    accum[p].f64_acc[gid] += (f64_t)cv[p][i];
                    accum[p].counts[gid]++;
                }
            }
        } else {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    accum[p].f64_acc[gid] += (f64_t)cv[p][i];
                    accum[p].counts[gid]++;
                }
            }
        }
        return NULL_OBJ;
    }

    // === Specialized SUM-I64 fast path (Q1/Q5 pattern) ===
    if (all_same && common_func == AGGR_ID_SUM && common_type == TYPE_I64 && !fbool && !fids && nki == 0) {
        i64_t *cv[MAX_FUSED];
        for (p = 0; p < nplan; p++)
            cv[p] = AS_I64(cols[p]);
        if (pfirst) {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                if (pfirst[gid] < 0) pfirst[gid] = i;
                plast[gid] = i;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++)
                    accum[p].i64_acc[gid] += cv[p][i];
            }
        } else {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++)
                    accum[p].i64_acc[gid] += cv[p][i];
            }
        }
        return NULL_OBJ;
    }

    // === All-SUM mixed-type fast path (sum over mixed I64/F64 columns) ===
    if (same_func && common_func == AGGR_ID_SUM && !fbool && !fids && nki == 0) {
        i64_t *ci[MAX_FUSED];
        f64_t *cf[MAX_FUSED];
        b8_t is_i64[MAX_FUSED];
        for (p = 0; p < nplan; p++) {
            is_i64[p] = (cols[p]->type == TYPE_I64);
            if (is_i64[p]) ci[p] = AS_I64(cols[p]);
            else cf[p] = AS_F64(cols[p]);
        }
        if (pfirst) {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                if (pfirst[gid] < 0) pfirst[gid] = i;
                plast[gid] = i;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    if (is_i64[p]) accum[p].i64_acc[gid] += ci[p][i];
                    else accum[p].f64_acc[gid] += cf[p][i];
                }
            }
        } else {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    if (is_i64[p]) accum[p].i64_acc[gid] += ci[p][i];
                    else accum[p].f64_acc[gid] += cf[p][i];
                }
            }
        }
        return NULL_OBJ;
    }

    // === All-AVG mixed-type fast path (Q4 pattern: avg over mixed I64/F64 columns) ===
    if (same_func && common_func == AGGR_ID_AVG && !fbool && !fids && nki == 0) {
        // Pre-resolve: extract raw data pointer and type for each plan
        i64_t *ci[MAX_FUSED];
        f64_t *cf[MAX_FUSED];
        b8_t is_i64[MAX_FUSED];
        for (p = 0; p < nplan; p++) {
            is_i64[p] = (cols[p]->type == TYPE_I64);
            if (is_i64[p]) ci[p] = AS_I64(cols[p]);
            else cf[p] = AS_F64(cols[p]);
        }
        if (pfirst) {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                if (pfirst[gid] < 0) pfirst[gid] = i;
                plast[gid] = i;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    accum[p].f64_acc[gid] += is_i64[p] ? (f64_t)ci[p][i] : cf[p][i];
                    accum[p].counts[gid]++;
                }
            }
        } else {
            for (i = offset; i < end; i++) {
                gid = kv[i] - mn;
                pcounts[gid]++;
                for (p = 0; p < nplan; p++) {
                    accum[p].f64_acc[gid] += is_i64[p] ? (f64_t)ci[p][i] : cf[p][i];
                    accum[p].counts[gid]++;
                }
            }
        }
        return NULL_OBJ;
    }

    // === Boolean-fused filter path: iterate all rows, skip non-matching ===
    if (fbool) {
        for (i = offset; i < end; i++) {
            if (!fbool[i]) continue;
            if (nki > 0) {
                gid = 0;
                for (i64_t k = 0; k < nki; k++)
                    gid += (kptrs[k][i] - kmins[k]) * kstrides[k];
            } else {
                gid = kv[i] - mn;
            }
            if (pfirst) { if (pfirst[gid] < 0) pfirst[gid] = i; plast[gid] = i; }
            pcounts[gid]++;
            for (p = 0; p < nplan; p++)
                FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], i, accum[p]);
        }
        return NULL_OBJ;
    }

    // === Generic path (with pre-resolved cols) ===
    for (i = offset; i < end; i++) {
        if (nki > 0) {
            gid = 0;
            for (i64_t k = 0; k < nki; k++)
                gid += (kptrs[k][i] - kmins[k]) * kstrides[k];
        } else {
            gid = kv[i] - mn;
        }
        i64_t row = fids ? fids[i] : i;
        if (pfirst) { if (pfirst[gid] < 0) pfirst[gid] = row; plast[gid] = row; }
        pcounts[gid]++;
        for (p = 0; p < nplan; p++)
            FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], row, accum[p]);
    }

    return NULL_OBJ;
}

// ============================================================================
// Parallel fused hash aggregation (for general/many-group queries like Q10)
// ============================================================================

typedef struct {
    obj_p keys;                 // Key columns (shared, read-only)
    i64_t nkeys;
    obj_p tab_vals;             // Table values (shared, read-only)
    fused_plan_t *plan;         // Aggregation plan (shared, read-only)
    i64_t nplan;
    i64_t chunk_size;
    local_agg_t *aggs;         // Per-worker hash tables
    fused_accum_t **worker_acc; // Per-worker fused accumulators
    b8_t *fbool;               // Optional boolean filter
    i64_t *fids;               // Optional filter indices
} parallel_fused_hash_ctx_t;

static obj_p parallel_fused_hash_worker(i64_t len, i64_t offset, raw_p ctx_ptr) {
    parallel_fused_hash_ctx_t *ctx = (parallel_fused_hash_ctx_t *)ctx_ptr;
    i64_t chunk_idx = offset / ctx->chunk_size;
    local_agg_t *agg = &ctx->aggs[chunk_idx];
    fused_accum_t *accum = ctx->worker_acc[chunk_idx];
    fused_plan_t *plan = ctx->plan;
    i64_t nplan = ctx->nplan;
    b8_t *fbool = ctx->fbool;
    i64_t *fids = ctx->fids;
    i64_t end = offset + len;
    i64_t i, p, gid;
    i64_t accum_cap = agg->max_groups;

    // Pre-resolve column pointers
    obj_p cols[MAX_FUSED];
    for (p = 0; p < nplan; p++)
        cols[p] = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(ctx->tab_vals)[plan[p].col_idx];

    for (i = offset; i < end; i++) {
        if (fbool && !fbool[i]) continue;
        u64_t h = compute_composite_hash(ctx->keys, ctx->nkeys, i);
        gid = local_agg_find_or_create(agg, ctx->keys, ctx->nkeys, i, h);
        if (UNLIKELY(gid >= accum_cap)) {
            fused_grow_accum(accum, plan, nplan, accum_cap, agg->max_groups);
            accum_cap = agg->max_groups;
        }
        i64_t row = fids ? fids[i] : i;
        for (p = 0; p < nplan; p++)
            FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], row, accum[p]);
    }
    return NULL_OBJ;
}

nil_t aggr_fused_compute(struct query_ctx_t *ctx, fused_plan_t *plan, i64_t nplan, obj_p *results) {
    query_ctx_p qctx = (query_ctx_p)ctx;
    obj_p keys = qctx->groupby;
    i64_t nkeys = keys->len;

    // Boolean-fused path: filter_bool is flat B8, keys are unfiltered, no orig_table
    b8_t *fbool = (qctx->filter_bool != NULL_OBJ && qctx->filter_bool->type == TYPE_B8
                   && qctx->orig_table == NULL_OBJ)
                  ? AS_B8(qctx->filter_bool) : NULL;

    // Use orig_table for value columns when filter skipped materialization (gather path)
    obj_p val_table = (qctx->orig_table != NULL_OBJ) ? qctx->orig_table : qctx->table;
    obj_p tab_vals = AS_LIST(val_table)[1];
    i64_t *fids = (!fbool && qctx->orig_table != NULL_OBJ && qctx->filter != NULL_OBJ)
                  ? AS_I64(qctx->filter) : NULL;

    // nrows = number of rows to iterate (full table if boolean path, filtered key length otherwise)
    i64_t nrows = (nkeys > 0 && AS_LIST(keys)[0]->len > 0) ? AS_LIST(keys)[0]->len : 0;
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
            i64_t mn, mx;
            // Try parallel min/max scan first
            PH_PHASE_START(minmax);
            pool_p pool = pool_get();
            if (parallel_minmax_scan(pool, kv, nrows, &mn, &mx) != 0) {
                mn = kv[0]; mx = kv[0];
                for (i = 1; i < nrows; i++) {
                    if (kv[i] < mn) mn = kv[i];
                    if (kv[i] > mx) mx = kv[i];
                }
            }
            PH_PHASE_END(minmax);
            i64_t range = mx - mn + 1;

            if (range > 0 && range <= PERFECT_HASH_THRESHOLD) {
                // Parallel path for large tables
                i64_t nworkers = pool_split_by(pool, nrows, 0);
                if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;

                if (nworkers > 1 && nrows >= PARALLEL_AGG_THRESHOLD) {
                    i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));

                    // Check if any plan entry needs FIRST/LAST row tracking
                    b8_t needs_first_last = B8_FALSE;
                    for (p = 0; p < nplan; p++) {
                        if (plan[p].func_id == AGGR_ID_FIRST || plan[p].func_id == AGGR_ID_LAST) {
                            needs_first_last = B8_TRUE; break;
                        }
                    }

                    // Allocate per-worker state
                    parallel_fused_ctx_t pctx;
                    pctx.kv = kv;
                    pctx.mn = mn;
                    pctx.range = range;
                    pctx.tab_vals = tab_vals;
                    pctx.fids = fids;
                    pctx.plan = plan;
                    pctx.nplan = nplan;
                    pctx.chunk_size = chunk_size;
                    pctx.nkeys_inline = 0;
                    pctx.key_ptrs = NULL;
                    pctx.mins = NULL;
                    pctx.strides = NULL;
                    pctx.filter_bool = fbool;
                    pctx.worker_acc = (fused_accum_t **)heap_alloc(nworkers * sizeof(fused_accum_t *));
                    pctx.worker_counts = (i64_t **)heap_alloc(nworkers * sizeof(i64_t *));
                    pctx.worker_first = needs_first_last ? (i64_t **)heap_alloc(nworkers * sizeof(i64_t *)) : NULL;
                    pctx.worker_last = needs_first_last ? (i64_t **)heap_alloc(nworkers * sizeof(i64_t *)) : NULL;

                    for (i = 0; i < nworkers; i++) {
                        pctx.worker_acc[i] = fused_alloc_accum(tab_vals, plan, nplan, range);
                        pctx.worker_counts[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        for (i64_t j = 0; j < range; j++)
                            pctx.worker_counts[i][j] = 0;
                        if (needs_first_last) {
                            pctx.worker_first[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                            pctx.worker_last[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                            for (i64_t j = 0; j < range; j++) {
                                pctx.worker_first[i][j] = -1;
                                pctx.worker_last[i][j] = -1;
                            }
                        }
                    }

                    // Submit tasks
                    PH_PHASE_START(agg);
                    pool_prepare(pool);
                    i64_t offset = 0;
                    for (i = 0; i < nworkers - 1; i++) {
                        pool_add_task(pool, (raw_p)parallel_fused_worker, 3, chunk_size, offset, &pctx);
                        offset += chunk_size;
                    }
                    pool_add_task(pool, (raw_p)parallel_fused_worker, 3, nrows - offset, offset, &pctx);

                    obj_p pres = pool_run(pool);
                    drop_obj(pres);
                    PH_PHASE_END(agg);

                    PH_PHASE_START(merge);
                    // Merge: combine per-worker accumulators
                    i64_t *pcounts = pctx.worker_counts[0];
                    fused_accum_t *accum = pctx.worker_acc[0];

                    for (i64_t w = 1; w < nworkers; w++) {
                        for (i64_t s = 0; s < range; s++)
                            pcounts[s] += pctx.worker_counts[w][s];
                        if (needs_first_last) {
                            i64_t *pfirst = pctx.worker_first[0];
                            i64_t *plast = pctx.worker_last[0];
                            for (i64_t s = 0; s < range; s++) {
                                if (pctx.worker_first[w][s] >= 0) {
                                    if (pfirst[s] < 0 || pctx.worker_first[w][s] < pfirst[s])
                                        pfirst[s] = pctx.worker_first[w][s];
                                }
                                if (pctx.worker_last[w][s] >= 0) {
                                    if (pctx.worker_last[w][s] > plast[s])
                                        plast[s] = pctx.worker_last[w][s];
                                }
                            }
                        }
                        // Merge accumulators per plan entry (switch hoisted outside inner loop)
                        for (p = 0; p < nplan; p++) {
                            fused_accum_t *wa = &pctx.worker_acc[w][p];
                            fused_accum_t *ma = &accum[p];
                            switch (plan[p].func_id) {
                            case AGGR_ID_SUM: case AGGR_ID_COUNT:
                                if (ma->i64_acc && wa->i64_acc)
                                    for (i64_t s = 0; s < range; s++) ma->i64_acc[s] += wa->i64_acc[s];
                                if (ma->f64_acc && wa->f64_acc)
                                    for (i64_t s = 0; s < range; s++) ma->f64_acc[s] += wa->f64_acc[s];
                                break;
                            case AGGR_ID_MAX:
                                if (ma->i64_acc && wa->i64_acc)
                                    for (i64_t s = 0; s < range; s++)
                                        if (wa->i64_acc[s] > ma->i64_acc[s]) ma->i64_acc[s] = wa->i64_acc[s];
                                if (ma->f64_acc && wa->f64_acc)
                                    for (i64_t s = 0; s < range; s++)
                                        if (wa->f64_acc[s] > ma->f64_acc[s]) ma->f64_acc[s] = wa->f64_acc[s];
                                break;
                            case AGGR_ID_MIN:
                                if (ma->i64_acc && wa->i64_acc)
                                    for (i64_t s = 0; s < range; s++)
                                        if (wa->i64_acc[s] < ma->i64_acc[s]) ma->i64_acc[s] = wa->i64_acc[s];
                                if (ma->f64_acc && wa->f64_acc)
                                    for (i64_t s = 0; s < range; s++)
                                        if (wa->f64_acc[s] < ma->f64_acc[s]) ma->f64_acc[s] = wa->f64_acc[s];
                                break;
                            case AGGR_ID_AVG: case AGGR_ID_DEV:
                                if (ma->f64_acc && wa->f64_acc)
                                    for (i64_t s = 0; s < range; s++) ma->f64_acc[s] += wa->f64_acc[s];
                                if (ma->f64_aux && wa->f64_aux)
                                    for (i64_t s = 0; s < range; s++) ma->f64_aux[s] += wa->f64_aux[s];
                                if (ma->counts && wa->counts)
                                    for (i64_t s = 0; s < range; s++) ma->counts[s] += wa->counts[s];
                                break;
                            default: break;
                            }
                        }
                        // Free worker w's state
                        heap_free(pctx.worker_counts[w]);
                        if (needs_first_last) {
                            heap_free(pctx.worker_first[w]);
                            heap_free(pctx.worker_last[w]);
                        }
                        fused_free_accum(pctx.worker_acc[w], nplan);
                    }

                    PH_PHASE_END(merge);
                    PH_PHASE_START(extract);
                    // Compact non-empty groups
                    i64_t ng = 0;
                    for (i = 0; i < range; i++)
                        if (pcounts[i] > 0) ng++;

                    qctx->ngroups = ng;

                    if (needs_first_last) {
                        // Use first/last row indices for key extraction
                        i64_t *pfirst = pctx.worker_first[0];
                        i64_t *plast = pctx.worker_last[0];
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
                        heap_free(pfirst); heap_free(plast);
                    } else {
                        // Build key vector directly from slot IDs (no first_rows needed)
                        obj_p kcol = vector(key_col->type, ng);
                        i64_t gi = 0;
                        for (i = 0; i < range; i++)
                            if (pcounts[i] > 0) AS_I64(kcol)[gi++] = i + mn;
                        qctx->prebuilt_keys = LIST(1);
                        AS_LIST(qctx->prebuilt_keys)[0] = kcol;
                    }

                    fused_extract_results(tab_vals, plan, nplan, accum,
                                          pcounts, range, ng,
                                          qctx->first_rows, qctx->last_rows, results);

                    heap_free(pcounts);
                    fused_free_accum(accum, nplan);
                    heap_free(pctx.worker_acc);
                    heap_free(pctx.worker_counts);
                    if (pctx.worker_first) heap_free(pctx.worker_first);
                    if (pctx.worker_last) heap_free(pctx.worker_last);
                    PH_PHASE_END(extract);
                    return;
                }

                // Sequential perfect hash path
                i64_t *pcounts = (i64_t *)heap_alloc(range * sizeof(i64_t));
                i64_t *pfirst = (i64_t *)heap_alloc(range * sizeof(i64_t));
                i64_t *plast = (i64_t *)heap_alloc(range * sizeof(i64_t));
                for (i = 0; i < range; i++) { pcounts[i] = 0; pfirst[i] = -1; plast[i] = -1; }

                fused_accum_t *accum = fused_alloc_accum(tab_vals, plan, nplan, range);

                // Pre-resolve column pointers
                obj_p cols[MAX_FUSED];
                for (p = 0; p < nplan; p++)
                    cols[p] = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(tab_vals)[plan[p].col_idx];

                if (fbool) {
                    for (i = 0; i < nrows; i++) {
                        if (!fbool[i]) continue;
                        gid = kv[i] - mn;
                        if (pfirst[gid] < 0) pfirst[gid] = i;
                        plast[gid] = i;
                        pcounts[gid]++;
                        for (p = 0; p < nplan; p++)
                            FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], i, accum[p]);
                    }
                } else {
                    for (i = 0; i < nrows; i++) {
                        gid = kv[i] - mn;
                        i64_t row = fids ? fids[i] : i;
                        if (pfirst[gid] < 0) pfirst[gid] = row;
                        plast[gid] = row;
                        pcounts[gid]++;
                        for (p = 0; p < nplan; p++)
                            FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], row, accum[p]);
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

    // Multi-key perfect hash: composite index when all keys are i64/symbol with small product range
    if (nkeys > 1 && nkeys <= 16) {
        i64_t mins[16], ranges[16], strides[16];
        i64_t composite_range = 1;
        b8_t all_hashable = B8_TRUE;

        // Check all keys are hashable and get per-key data pointers
        i64_t *key_data[16];
        for (i64_t k = 0; k < nkeys; k++) {
            obj_p kc = AS_LIST(keys)[k];
            if (kc->type != TYPE_I64 && kc->type != TYPE_SYMBOL) { all_hashable = B8_FALSE; break; }
            key_data[k] = AS_I64(kc);
        }

        PH_PHASE_START(mk_minmax);
        if (all_hashable) {
            // Parallel multi-key min/max: scan all keys in one pass per worker
            pool_p mpool = pool_get();
            i64_t nw = pool_split_by(mpool, nrows, 0);
            if (nw > MAX_AGG_WORKERS) nw = MAX_AGG_WORKERS;

            if (nw > 1 && nrows >= PARALLEL_AGG_THRESHOLD) {
                // Single pool dispatch scans all keys' min/max in one pass
                i64_t maxs[16];
                if (parallel_multikey_minmax_scan(mpool, nkeys, key_data, nrows, mins, maxs) == 0) {
                    for (i64_t k = 0; k < nkeys; k++) {
                        ranges[k] = maxs[k] - mins[k] + 1;
                        if (ranges[k] <= 0) { all_hashable = B8_FALSE; break; }
                        if (ranges[k] > PERFECT_HASH_THRESHOLD / composite_range) { all_hashable = B8_FALSE; break; }
                        composite_range *= ranges[k];
                        if (composite_range > PERFECT_HASH_THRESHOLD) { all_hashable = B8_FALSE; break; }
                    }
                } else {
                    // Fallback: sequential scan
                    for (i64_t k = 0; k < nkeys; k++) {
                        i64_t *kv = key_data[k];
                        i64_t mn = kv[0], mx = kv[0];
                        for (i = 1; i < nrows; i++) {
                            if (kv[i] < mn) mn = kv[i];
                            if (kv[i] > mx) mx = kv[i];
                        }
                        mins[k] = mn;
                        ranges[k] = mx - mn + 1;
                        if (ranges[k] <= 0) { all_hashable = B8_FALSE; break; }
                        if (ranges[k] > PERFECT_HASH_THRESHOLD / composite_range) { all_hashable = B8_FALSE; break; }
                        composite_range *= ranges[k];
                        if (composite_range > PERFECT_HASH_THRESHOLD) { all_hashable = B8_FALSE; break; }
                    }
                }
            } else {
                // Sequential fallback for small tables
                for (i64_t k = 0; k < nkeys; k++) {
                    i64_t *kv = key_data[k];
                    i64_t mn = kv[0], mx = kv[0];
                    for (i = 1; i < nrows; i++) {
                        if (kv[i] < mn) mn = kv[i];
                        if (kv[i] > mx) mx = kv[i];
                    }
                    mins[k] = mn;
                    ranges[k] = mx - mn + 1;
                    if (ranges[k] <= 0) { all_hashable = B8_FALSE; break; }
                    composite_range *= ranges[k];
                    if (composite_range > PERFECT_HASH_THRESHOLD) { all_hashable = B8_FALSE; break; }
                }
            }
        }

        PH_PHASE_END(mk_minmax);
        if (all_hashable) {
            // Compute strides (row-major: last key varies fastest)
            strides[nkeys - 1] = 1;
            for (i64_t k = nkeys - 2; k >= 0; k--)
                strides[k] = strides[k + 1] * ranges[k + 1];

            // Prepare key pointers for inline composite key computation
            i64_t *key_ptrs[16];
            for (i64_t k = 0; k < nkeys; k++)
                key_ptrs[k] = AS_I64(AS_LIST(keys)[k]);

            i64_t range = composite_range;

            // Parallel path
            pool_p pool = pool_get();
            i64_t nworkers = pool_split_by(pool, nrows, 0);
            if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;

            if (nworkers > 1 && nrows >= PARALLEL_AGG_THRESHOLD) {
                i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));

                b8_t needs_fl = B8_FALSE;
                for (p = 0; p < nplan; p++) {
                    if (plan[p].func_id == AGGR_ID_FIRST || plan[p].func_id == AGGR_ID_LAST) {
                        needs_fl = B8_TRUE; break;
                    }
                }

                parallel_fused_ctx_t pctx;
                pctx.kv = NULL;
                pctx.mn = 0;
                pctx.range = range;
                pctx.tab_vals = tab_vals;
                pctx.fids = fids;
                pctx.plan = plan;
                pctx.nplan = nplan;
                pctx.chunk_size = chunk_size;
                pctx.nkeys_inline = nkeys;
                pctx.key_ptrs = key_ptrs;
                pctx.mins = mins;
                pctx.strides = strides;
                pctx.filter_bool = fbool;
                pctx.worker_acc = (fused_accum_t **)heap_alloc(nworkers * sizeof(fused_accum_t *));
                pctx.worker_counts = (i64_t **)heap_alloc(nworkers * sizeof(i64_t *));
                pctx.worker_first = needs_fl ? (i64_t **)heap_alloc(nworkers * sizeof(i64_t *)) : NULL;
                pctx.worker_last = needs_fl ? (i64_t **)heap_alloc(nworkers * sizeof(i64_t *)) : NULL;

                for (i = 0; i < nworkers; i++) {
                    pctx.worker_acc[i] = fused_alloc_accum(tab_vals, plan, nplan, range);
                    pctx.worker_counts[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                    for (i64_t j = 0; j < range; j++)
                        pctx.worker_counts[i][j] = 0;
                    if (needs_fl) {
                        pctx.worker_first[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        pctx.worker_last[i] = (i64_t *)heap_alloc(range * sizeof(i64_t));
                        for (i64_t j = 0; j < range; j++) {
                            pctx.worker_first[i][j] = -1;
                            pctx.worker_last[i][j] = -1;
                        }
                    }
                }

                PH_PHASE_START(mk_agg);
                pool_prepare(pool);
                i64_t offset = 0;
                for (i = 0; i < nworkers - 1; i++) {
                    pool_add_task(pool, (raw_p)parallel_fused_worker, 3, chunk_size, offset, &pctx);
                    offset += chunk_size;
                }
                pool_add_task(pool, (raw_p)parallel_fused_worker, 3, nrows - offset, offset, &pctx);

                obj_p pres = pool_run(pool);
                drop_obj(pres);
                PH_PHASE_END(mk_agg);

                i64_t *pcounts = pctx.worker_counts[0];
                fused_accum_t *accum = pctx.worker_acc[0];

                for (i64_t w = 1; w < nworkers; w++) {
                    for (i64_t s = 0; s < range; s++)
                        pcounts[s] += pctx.worker_counts[w][s];
                    if (needs_fl) {
                        i64_t *pfirst = pctx.worker_first[0];
                        i64_t *plast = pctx.worker_last[0];
                        for (i64_t s = 0; s < range; s++) {
                            if (pctx.worker_first[w][s] >= 0) {
                                if (pfirst[s] < 0 || pctx.worker_first[w][s] < pfirst[s])
                                    pfirst[s] = pctx.worker_first[w][s];
                            }
                            if (pctx.worker_last[w][s] >= 0) {
                                if (pctx.worker_last[w][s] > plast[s])
                                    plast[s] = pctx.worker_last[w][s];
                            }
                        }
                    }
                    for (p = 0; p < nplan; p++) {
                        fused_accum_t *wa = &pctx.worker_acc[w][p];
                        fused_accum_t *ma = &accum[p];
                        switch (plan[p].func_id) {
                        case AGGR_ID_SUM: case AGGR_ID_COUNT:
                            if (ma->i64_acc && wa->i64_acc)
                                for (i64_t s = 0; s < range; s++) ma->i64_acc[s] += wa->i64_acc[s];
                            if (ma->f64_acc && wa->f64_acc)
                                for (i64_t s = 0; s < range; s++) ma->f64_acc[s] += wa->f64_acc[s];
                            break;
                        case AGGR_ID_MAX:
                            if (ma->i64_acc && wa->i64_acc)
                                for (i64_t s = 0; s < range; s++)
                                    if (wa->i64_acc[s] > ma->i64_acc[s]) ma->i64_acc[s] = wa->i64_acc[s];
                            if (ma->f64_acc && wa->f64_acc)
                                for (i64_t s = 0; s < range; s++)
                                    if (wa->f64_acc[s] > ma->f64_acc[s]) ma->f64_acc[s] = wa->f64_acc[s];
                            break;
                        case AGGR_ID_MIN:
                            if (ma->i64_acc && wa->i64_acc)
                                for (i64_t s = 0; s < range; s++)
                                    if (wa->i64_acc[s] < ma->i64_acc[s]) ma->i64_acc[s] = wa->i64_acc[s];
                            if (ma->f64_acc && wa->f64_acc)
                                for (i64_t s = 0; s < range; s++)
                                    if (wa->f64_acc[s] < ma->f64_acc[s]) ma->f64_acc[s] = wa->f64_acc[s];
                            break;
                        case AGGR_ID_AVG: case AGGR_ID_DEV:
                            if (ma->f64_acc && wa->f64_acc)
                                for (i64_t s = 0; s < range; s++) ma->f64_acc[s] += wa->f64_acc[s];
                            if (ma->f64_aux && wa->f64_aux)
                                for (i64_t s = 0; s < range; s++) ma->f64_aux[s] += wa->f64_aux[s];
                            if (ma->counts && wa->counts)
                                for (i64_t s = 0; s < range; s++) ma->counts[s] += wa->counts[s];
                            break;
                        default: break;
                        }
                    }
                    heap_free(pctx.worker_counts[w]);
                    if (needs_fl) {
                        heap_free(pctx.worker_first[w]);
                        heap_free(pctx.worker_last[w]);
                    }
                    fused_free_accum(pctx.worker_acc[w], nplan);
                }

                i64_t ng = 0;
                for (i = 0; i < range; i++)
                    if (pcounts[i] > 0) ng++;

                qctx->ngroups = ng;

                if (needs_fl) {
                    i64_t *pfirst = pctx.worker_first[0];
                    i64_t *plast = pctx.worker_last[0];
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
                    heap_free(pfirst); heap_free(plast);
                } else {
                    // Build per-key vectors from composite slot IDs
                    obj_p prekeys = LIST(nkeys);
                    i64_t gi = 0;
                    for (i64_t k = 0; k < nkeys; k++) {
                        obj_p kc = AS_LIST(keys)[k];
                        obj_p kcol = vector(kc->type, ng);
                        AS_LIST(prekeys)[k] = kcol;
                    }
                    for (i = 0; i < range; i++) {
                        if (pcounts[i] > 0) {
                            for (i64_t k = 0; k < nkeys; k++)
                                AS_I64(AS_LIST(prekeys)[k])[gi] = (i / strides[k]) % ranges[k] + mins[k];
                            gi++;
                        }
                    }
                    qctx->prebuilt_keys = prekeys;
                }

                fused_extract_results(tab_vals, plan, nplan, accum,
                                      pcounts, range, ng,
                                      qctx->first_rows, qctx->last_rows, results);

                heap_free(pcounts);
                fused_free_accum(accum, nplan);
                heap_free(pctx.worker_acc); heap_free(pctx.worker_counts);
                if (pctx.worker_first) heap_free(pctx.worker_first);
                if (pctx.worker_last) heap_free(pctx.worker_last);
                return;
            }

            // Sequential multi-key perfect hash path
            i64_t *pcounts = (i64_t *)heap_alloc(range * sizeof(i64_t));
            i64_t *pfirst = (i64_t *)heap_alloc(range * sizeof(i64_t));
            i64_t *plast = (i64_t *)heap_alloc(range * sizeof(i64_t));
            for (i = 0; i < range; i++) { pcounts[i] = 0; pfirst[i] = -1; plast[i] = -1; }

            fused_accum_t *accum = fused_alloc_accum(tab_vals, plan, nplan, range);

            // Pre-resolve column pointers
            obj_p cols[MAX_FUSED];
            for (p = 0; p < nplan; p++)
                cols[p] = AS_LIST(tab_vals)[plan[p].col_idx];

            if (fbool) {
                for (i = 0; i < nrows; i++) {
                    if (!fbool[i]) continue;
                    gid = 0;
                    for (i64_t k = 0; k < nkeys; k++)
                        gid += (key_ptrs[k][i] - mins[k]) * strides[k];
                    if (pfirst[gid] < 0) pfirst[gid] = i;
                    plast[gid] = i;
                    pcounts[gid]++;
                    for (p = 0; p < nplan; p++)
                        FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], i, accum[p]);
                }
            } else {
                for (i = 0; i < nrows; i++) {
                    gid = 0;
                    for (i64_t k = 0; k < nkeys; k++)
                        gid += (key_ptrs[k][i] - mins[k]) * strides[k];
                    i64_t row = fids ? fids[i] : i;
                    if (pfirst[gid] < 0) pfirst[gid] = row;
                    plast[gid] = row;
                    pcounts[gid]++;
                    for (p = 0; p < nplan; p++)
                        FUSED_ACCUM_STEP(plan[p].func_id, gid, cols[p], row, accum[p]);
                }
            }

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

    // General hash path: arbitrary keys — parallel when possible
    {
        pool_p pool = pool_get();
        i64_t nworkers = pool_split_by(pool, nrows, 0);
        if (nworkers > MAX_AGG_WORKERS) nworkers = MAX_AGG_WORKERS;

        if (nworkers > 1 && nrows >= PARALLEL_AGG_THRESHOLD) {
            // Parallel fused hash aggregation
            i64_t chunk_size = pool_chunk_aligned(nrows, nworkers, sizeof(i64_t));
            i64_t max_groups_per = nrows / (10 * nworkers) + 1024;

            parallel_fused_hash_ctx_t pctx;
            pctx.keys = keys;
            pctx.nkeys = nkeys;
            pctx.tab_vals = tab_vals;
            pctx.plan = plan;
            pctx.nplan = nplan;
            pctx.chunk_size = chunk_size;
            pctx.fbool = fbool;
            pctx.fids = fids;
            pctx.aggs = (local_agg_t *)heap_alloc(nworkers * sizeof(local_agg_t));
            pctx.worker_acc = (fused_accum_t **)heap_alloc(nworkers * sizeof(fused_accum_t *));

            for (i = 0; i < nworkers; i++) {
                local_agg_init(&pctx.aggs[i], INITIAL_HT_CAPACITY, max_groups_per);
                pctx.worker_acc[i] = fused_alloc_accum(tab_vals, plan, nplan, max_groups_per);
            }

            pool_prepare(pool);
            i64_t offset = 0;
            for (i = 0; i < nworkers - 1; i++) {
                pool_add_task(pool, (raw_p)parallel_fused_hash_worker, 3, chunk_size, offset, &pctx);
                offset += chunk_size;
            }
            pool_add_task(pool, (raw_p)parallel_fused_hash_worker, 3, nrows - offset, offset, &pctx);

            obj_p pres = pool_run(pool);
            drop_obj(pres);

            // Merge: combine per-worker hash tables into a global result
            local_agg_t merged;
            i64_t total_groups = 0;
            for (i = 0; i < nworkers; i++) total_groups += pctx.aggs[i].count;
            local_agg_init(&merged, total_groups * 2 + 16, total_groups + 1024);
            fused_accum_t *maccum = fused_alloc_accum(tab_vals, plan, nplan, total_groups + 1024);

            for (i64_t w = 0; w < nworkers; w++) {
                local_agg_t *wa = &pctx.aggs[w];
                fused_accum_t *wacc = pctx.worker_acc[w];
                for (i64_t g = 0; g < wa->count; g++) {
                    u64_t h = wa->group_hashes[g];
                    i64_t worker_row = wa->first_rows[g];
                    i64_t mgid = local_agg_find_or_create(&merged, keys, nkeys, worker_row, h);
                    if (UNLIKELY(mgid < 0)) continue;

                    // Merge first/last rows
                    if (merged.first_rows[mgid] < 0 || wa->first_rows[g] < merged.first_rows[mgid])
                        merged.first_rows[mgid] = wa->first_rows[g];
                    if (wa->last_rows[g] > merged.last_rows[mgid])
                        merged.last_rows[mgid] = wa->last_rows[g];

                    // Merge fused accumulators
                    for (p = 0; p < nplan; p++) {
                        i8_t func = plan[p].func_id;
                        switch (func) {
                        case AGGR_ID_SUM: case AGGR_ID_COUNT:
                            if (maccum[p].i64_acc && wacc[p].i64_acc) maccum[p].i64_acc[mgid] += wacc[p].i64_acc[g];
                            if (maccum[p].f64_acc && wacc[p].f64_acc) maccum[p].f64_acc[mgid] += wacc[p].f64_acc[g];
                            break;
                        case AGGR_ID_MAX:
                            if (maccum[p].i64_acc && wacc[p].i64_acc && wacc[p].i64_acc[g] > maccum[p].i64_acc[mgid])
                                maccum[p].i64_acc[mgid] = wacc[p].i64_acc[g];
                            if (maccum[p].f64_acc && wacc[p].f64_acc && wacc[p].f64_acc[g] > maccum[p].f64_acc[mgid])
                                maccum[p].f64_acc[mgid] = wacc[p].f64_acc[g];
                            break;
                        case AGGR_ID_MIN:
                            if (maccum[p].i64_acc && wacc[p].i64_acc && wacc[p].i64_acc[g] < maccum[p].i64_acc[mgid])
                                maccum[p].i64_acc[mgid] = wacc[p].i64_acc[g];
                            if (maccum[p].f64_acc && wacc[p].f64_acc && wacc[p].f64_acc[g] < maccum[p].f64_acc[mgid])
                                maccum[p].f64_acc[mgid] = wacc[p].f64_acc[g];
                            break;
                        case AGGR_ID_AVG: case AGGR_ID_DEV:
                            if (maccum[p].f64_acc && wacc[p].f64_acc) maccum[p].f64_acc[mgid] += wacc[p].f64_acc[g];
                            if (maccum[p].f64_aux && wacc[p].f64_aux) maccum[p].f64_aux[mgid] += wacc[p].f64_aux[g];
                            if (maccum[p].counts && wacc[p].counts) maccum[p].counts[mgid] += wacc[p].counts[g];
                            break;
                        default: break;
                        }
                    }
                }
            }

            // Extract results
            i64_t ng = merged.count;
            qctx->ngroups = ng;

            if (fids) {
                for (i = 0; i < ng; i++) {
                    merged.first_rows[i] = fids[merged.first_rows[i]];
                    merged.last_rows[i] = fids[merged.last_rows[i]];
                }
            }
            qctx->first_rows = merged.first_rows;
            qctx->last_rows = merged.last_rows;
            merged.first_rows = NULL;
            merged.last_rows = NULL;

            i64_t *hcounts = (i64_t *)heap_alloc(ng * sizeof(i64_t));
            for (i = 0; i < ng; i++) hcounts[i] = 1;

            fused_extract_results(tab_vals, plan, nplan, maccum,
                                  hcounts, ng, ng,
                                  qctx->first_rows, qctx->last_rows, results);

            heap_free(hcounts);
            fused_free_accum(maccum, nplan);
            local_agg_destroy(&merged);
            for (i = 0; i < nworkers; i++) {
                fused_free_accum(pctx.worker_acc[i], nplan);
                local_agg_destroy(&pctx.aggs[i]);
            }
            heap_free(pctx.worker_acc);
            heap_free(pctx.aggs);
            return;
        }

        // Sequential fallback
        local_agg_t agg;
        i64_t accum_cap = nrows / 10 + 1024;
        local_agg_init(&agg, INITIAL_HT_CAPACITY, accum_cap);
        fused_accum_t *accum = fused_alloc_accum(tab_vals, plan, nplan, accum_cap);

        obj_p gcols[MAX_FUSED];
        for (p = 0; p < nplan; p++)
            gcols[p] = plan[p].col_ptr ? plan[p].col_ptr : AS_LIST(tab_vals)[plan[p].col_idx];

        for (i = 0; i < nrows; i++) {
            if (fbool && !fbool[i]) continue;
            u64_t h = compute_composite_hash(keys, nkeys, i);
            gid = local_agg_find_or_create(&agg, keys, nkeys, i, h);
            if (UNLIKELY(gid >= accum_cap)) {
                fused_grow_accum(accum, plan, nplan, accum_cap, agg.max_groups);
                accum_cap = agg.max_groups;
            }
            i64_t row = fids ? fids[i] : i;
            for (p = 0; p < nplan; p++)
                FUSED_ACCUM_STEP(plan[p].func_id, gid, gcols[p], row, accum[p]);
        }

        i64_t ng = agg.count;
        qctx->ngroups = ng;

        if (fids) {
            for (i = 0; i < ng; i++) {
                agg.first_rows[i] = fids[agg.first_rows[i]];
                agg.last_rows[i] = fids[agg.last_rows[i]];
            }
        }
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
}

#undef FUSED_ACCUM_STEP

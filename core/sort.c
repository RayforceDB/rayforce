/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include "sort.h"
#include "string.h"
#include "ops.h"
#include "error.h"
#include "symbols.h"
#include "pool.h"
#include "index.h"
#include "chrono.h"

// Maximum range for counting sort - configurable constant
#define COUNTING_SORT_MAX_RANGE 1000000

// Sort thresholds
#define SMALL_VEC_THRESHOLD (128 * 1024)
#define PARALLEL_SORT_THRESHOLD_U8 (16 * RAY_PAGE_SIZE)
#define PARALLEL_COUNTING_SORT_THRESHOLD (512 * 1024)
#define PARALLEL_RADIX_SORT_THRESHOLD (768 * 1024)
#define COUNTING_SORT_MAX_RANGE_I32 (512 * 1024)
#define COUNTING_SORT_MAX_RANGE_I64 (512 * 1024)

// U8 counting sort constants
#define U8_RANGE 256

typedef struct {
    i64_t* out;
    i64_t len;
} iota_ctx_t;

obj_p iota_asc_worker(i64_t len, i64_t offset, void* ctx) {
    iota_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++)
        c->out[offset + i] = offset + i;
    return NULL_OBJ;
}

obj_p iota_desc_worker(i64_t len, i64_t offset, void* ctx) {
    iota_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++)
        c->out[offset + i] = c->len - 1 - (offset + i);
    return NULL_OBJ;
}

// Function pointer for comparison
typedef i64_t (*compare_func_t)(obj_p vec, i64_t idx_i, i64_t idx_j);

// Forward declarations for optimized sorting functions
static obj_p ray_iasc_optimized(obj_p x);
static obj_p ray_idesc_optimized(obj_p x);

static i64_t compare_symbols(obj_p vec, i64_t idx_i, i64_t idx_j) {
    i64_t sym_i = AS_I64(vec)[idx_i];
    i64_t sym_j = AS_I64(vec)[idx_j];

    // Fast path: if symbols are identical, no need to compare strings
    if (sym_i == sym_j)
        return 0;

    // For NULL symbols
    if (sym_i == NULL_I64 && sym_j == NULL_I64)
        return 0;
    if (sym_i == NULL_I64)
        return -1;
    if (sym_j == NULL_I64)
        return 1;

    // Compare string representations
    return strcmp(str_from_symbol(sym_i), str_from_symbol(sym_j));
}

static i64_t compare_lists(obj_p vec, i64_t idx_i, i64_t idx_j) {
    return cmp_obj(AS_LIST(vec)[idx_i], AS_LIST(vec)[idx_j]);
}

// Merge Sort implementation for comparison with TimSort
static void merge_sort_indices(obj_p vec, i64_t* indices, i64_t* temp, i64_t left, i64_t right,
                               compare_func_t compare_fn, i64_t asc) {
    if (left >= right)
        return;

    i64_t mid = left + (right - left) / 2;

    // Recursively sort both halves
    merge_sort_indices(vec, indices, temp, left, mid, compare_fn, asc);
    merge_sort_indices(vec, indices, temp, mid + 1, right, compare_fn, asc);

    // Merge the sorted halves
    i64_t i = left, j = mid + 1, k = left;

    while (i <= mid && j <= right) {
        if (asc * compare_fn(vec, indices[i], indices[j]) <= 0) {
            temp[k++] = indices[i++];
        } else {
            temp[k++] = indices[j++];
        }
    }

    // Copy remaining elements
    while (i <= mid)
        temp[k++] = indices[i++];
    while (j <= right)
        temp[k++] = indices[j++];

    // Copy back to original array
    for (i = left; i <= right; i++) {
        indices[i] = temp[i];
    }
}

obj_p mergesort_generic_obj(obj_p vec, i64_t asc) {
    i64_t len = vec->len;

    if (len == 0)
        return I64(0);

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    // Initialize indices
    for (i64_t i = 0; i < len; i++) {
        ov[i] = i;
    }

    // Select comparison function
    compare_func_t compare_fn;
    switch (vec->type) {
        case TYPE_SYMBOL:
            compare_fn = compare_symbols;
            break;
        case TYPE_LIST:
            compare_fn = compare_lists;
            break;
        default:
            return I64(0);
    }

    // Allocate temporary array for merging
    obj_p obj_temp = I64(len);
    if (!obj_temp) {
        drop_obj(indices);
        return I64(0);
    }
    i64_t* temp = AS_I64(obj_temp);

    // Perform merge sort
    merge_sort_indices(vec, ov, temp, 0, len - 1, compare_fn, asc);

    drop_obj(obj_temp);
    return indices;
}

// insertion sort with direction: asc > 0 for ascending, asc < 0 for descending
static inline nil_t insertion_sort_i64(i64_t array[], i64_t indices[], i64_t left, i64_t right, i64_t asc) {
    for (i64_t i = left + 1; i <= right; i++) {
        i64_t temp = indices[i];
        i64_t j = i - 1;
        while (j >= left && asc * (array[indices[j]] - array[temp]) > 0) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = temp;
    }
}

nil_t insertion_sort_asc(i64_t array[], i64_t indices[], i64_t left, i64_t right) {
    insertion_sort_i64(array, indices, left, right, 1);
}

nil_t insertion_sort_desc(i64_t array[], i64_t indices[], i64_t left, i64_t right) {
    insertion_sort_i64(array, indices, left, right, -1);
}

// ============================================================================
// Parallel Counting Sort for U8
// ============================================================================

typedef struct {
    u8_t* data;
    i64_t chunk_size;
    i64_t* histograms;
} histogram_u8_ctx_t;

typedef struct {
    u8_t* data;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
} scatter_u8_ctx_t;

static obj_p histogram_u8_worker(i64_t len, i64_t offset, void* ctx) {
    histogram_u8_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    u8_t* data = c->data + offset;
    i64_t* hist = c->histograms + worker_id * U8_RANGE;

    memset(hist, 0, U8_RANGE * sizeof(i64_t));
    for (i64_t i = 0; i < len; i++)
        hist[data[i]]++;

    return NULL_OBJ;
}

static obj_p scatter_u8_worker(i64_t len, i64_t offset, void* ctx) {
    scatter_u8_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    u8_t* data = c->data + offset;
    i64_t* pos = c->positions + worker_id * U8_RANGE;
    i64_t* out = c->out;

    for (i64_t i = 0; i < len; i++)
        out[pos[data[i]]++] = offset + i;

    return NULL_OBJ;
}

static obj_p parallel_counting_sort_u8(obj_p vec, i64_t asc) {
    i64_t len = vec->len;
    u8_t* data = AS_U8(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    // Allocate histograms
    obj_p hist_obj = I64(n * U8_RANGE);
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    // Allocate output early (overlaps with histogram computation)
    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        return indices;
    }

    // Phase 1: parallel histogram
    histogram_u8_ctx_t hist_ctx = {data, chunk_size, histograms};
    pool_map(len, histogram_u8_worker, &hist_ctx);

    // Phase 2: merge and compute positions
    i64_t global_counts[U8_RANGE] = {0};
    for (i64_t w = 0; w < n; w++)
        for (i64_t b = 0; b < U8_RANGE; b++)
            global_counts[b] += histograms[w * U8_RANGE + b];

    i64_t prefix[U8_RANGE];
    if (asc > 0) {
        prefix[0] = 0;
        for (i64_t b = 1; b < U8_RANGE; b++)
            prefix[b] = prefix[b-1] + global_counts[b-1];
    } else {
        prefix[U8_RANGE-1] = 0;
        for (i64_t b = U8_RANGE - 2; b >= 0; b--)
            prefix[b] = prefix[b+1] + global_counts[b+1];
    }

    // Compute per-worker positions
    for (i64_t b = 0; b < U8_RANGE; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t cnt = histograms[w * U8_RANGE + b];
            histograms[w * U8_RANGE + b] = pos;
            pos += cnt;
        }
    }

    // Phase 3: parallel scatter
    scatter_u8_ctx_t scatter_ctx = {data, chunk_size, histograms, AS_I64(indices)};
    pool_map(len, scatter_u8_worker, &scatter_ctx);

    drop_obj(hist_obj);
    return indices;
}

// ============================================================================
// Parallel Counting Sort for I16 (fixed 65536 range)
// ============================================================================

#define I16_BUCKETS 65536

typedef struct {
    i16_t* data;
    i64_t chunk_size;
    i64_t* histograms;  // n_workers * I16_BUCKETS + null_count at end
} histogram_i16_ctx_t;

typedef struct {
    i16_t* data;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
    i64_t* null_positions;
    i64_t null_offset;
    i64_t asc;
} scatter_i16_ctx_t;

// Histogram worker: builds histogram + counts nulls
// Bucket index: XOR with 0x8000 to convert signed order to unsigned order
// -32768 -> 0, -32767 -> 1, ..., -1 -> 32767, 0 -> 32768, ..., 32767 -> 65535
#define I16_TO_BUCKET(val) ((u16_t)((val) ^ 0x8000))

static obj_p histogram_i16_worker(i64_t len, i64_t offset, void* ctx) {
    histogram_i16_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i16_t* data = c->data + offset;
    i64_t* hist = c->histograms + worker_id * (I16_BUCKETS + 1);
    i64_t null_count = 0;

    memset(hist, 0, I16_BUCKETS * sizeof(i64_t));

    for (i64_t i = 0; i < len; i++) {
        i16_t val = data[i];
        if (val == NULL_I16)
            null_count++;
        else
            hist[I16_TO_BUCKET(val)]++;
    }
    hist[I16_BUCKETS] = null_count;  // store null count at end

    return NULL_OBJ;
}

// Scatter worker: handles NULLs
static obj_p scatter_i16_worker(i64_t len, i64_t offset, void* ctx) {
    scatter_i16_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i16_t* data = c->data + offset;
    i64_t* pos = c->positions + worker_id * I16_BUCKETS;
    i64_t null_pos = c->null_positions[worker_id];

    if (c->asc > 0) {
        for (i64_t i = 0; i < len; i++) {
            i16_t val = data[i];
            if (val == NULL_I16)
                c->out[null_pos++] = offset + i;
            else
                c->out[c->null_offset + pos[I16_TO_BUCKET(val)]++] = offset + i;
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            i16_t val = data[i];
            if (val == NULL_I16)
                c->out[c->null_offset + null_pos++] = offset + i;
            else
                c->out[pos[I16_TO_BUCKET(val)]++] = offset + i;
        }
    }

    return NULL_OBJ;
}

// Parallel counting sort for i16 - like i32 but with fixed 65536 range
static obj_p parallel_counting_sort_i16(obj_p vec, i64_t asc) {
    i64_t len = vec->len;
    i16_t* data = AS_I16(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    // Allocate histograms: n workers * (65536 buckets + 1 for null_count)
    obj_p hist_obj = I64(n * (I16_BUCKETS + 1));
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    // Allocate output early (overlaps with histogram computation)
    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        return indices;
    }

    // Phase 1: parallel histogram
    histogram_i16_ctx_t hist_ctx = {data, chunk_size, histograms};
    pool_map(len, histogram_i16_worker, &hist_ctx);

    // Phase 2: merge histograms and compute prefix sum
    obj_p counts_obj = I64(I16_BUCKETS);
    if (IS_ERR(counts_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return counts_obj;
    }
    i64_t* global_counts = AS_I64(counts_obj);

    // Merge all worker histograms
    memset(global_counts, 0, I16_BUCKETS * sizeof(i64_t));
    i64_t total_null_count = 0;
    for (i64_t w = 0; w < n; w++) {
        i64_t* worker_hist = histograms + w * (I16_BUCKETS + 1);
        for (i64_t b = 0; b < I16_BUCKETS; b++)
            global_counts[b] += worker_hist[b];
        total_null_count += worker_hist[I16_BUCKETS];
    }

    i64_t non_null_count = len - total_null_count;

    // Compute prefix sum
    obj_p prefix_obj = I64(I16_BUCKETS);
    if (IS_ERR(prefix_obj)) {
        drop_obj(hist_obj);
        drop_obj(counts_obj);
        drop_obj(indices);
        return prefix_obj;
    }
    i64_t* prefix = AS_I64(prefix_obj);

    if (asc > 0) {
        prefix[0] = 0;
        for (i64_t b = 1; b < I16_BUCKETS; b++)
            prefix[b] = prefix[b-1] + global_counts[b-1];
    } else {
        prefix[I16_BUCKETS-1] = 0;
        for (i64_t b = I16_BUCKETS - 2; b >= 0; b--)
            prefix[b] = prefix[b+1] + global_counts[b+1];
    }

    // Compute per-worker positions
    for (i64_t b = 0; b < I16_BUCKETS; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * (I16_BUCKETS + 1);
            i64_t cnt = worker_hist[b];
            worker_hist[b] = pos;
            pos += cnt;
        }
    }

    drop_obj(counts_obj);
    drop_obj(prefix_obj);

    // Compute per-worker null positions
    obj_p null_pos_obj = I64(n);
    if (IS_ERR(null_pos_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return null_pos_obj;
    }
    i64_t* null_positions = AS_I64(null_pos_obj);

    i64_t null_offset_acc = 0;
    for (i64_t w = 0; w < n; w++) {
        null_positions[w] = null_offset_acc;
        null_offset_acc += histograms[w * (I16_BUCKETS + 1) + I16_BUCKETS];
    }

    // Phase 3: parallel scatter
    i64_t null_offset = asc > 0 ? total_null_count : non_null_count;
    scatter_i16_ctx_t scatter_ctx = {data, chunk_size, histograms, AS_I64(indices), null_positions, null_offset, asc};
    pool_map(len, scatter_i16_worker, &scatter_ctx);

    drop_obj(hist_obj);
    drop_obj(null_pos_obj);
    return indices;
}

obj_p ray_sort_asc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    u8_t* iv = AS_U8(vec);

    if (len >= PARALLEL_SORT_THRESHOLD_U8)
        return parallel_counting_sort_u8(vec, 1);

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    u64_t pos[257] = {0};

    for (i = 0; i < len; i++)
        pos[iv[i] + 1]++;

    for (i = 2; i <= 256; i++)
        pos[i] += pos[i - 1];

    for (i = 0; i < len; i++)
        ov[pos[iv[i]]++] = i;

    return indices;
}

obj_p ray_sort_asc_i16(obj_p vec) {
    i64_t i, len = vec->len;
    i16_t* iv = AS_I16(vec);

    if (len >= PARALLEL_COUNTING_SORT_THRESHOLD)
        return parallel_counting_sort_i16(vec, 1);

    // Medium arrays: 1-pass counting sort (65536 buckets)
    if (len >= SMALL_VEC_THRESHOLD) {
        obj_p indices = I64(len);
        i64_t* ov = AS_I64(indices);

        u64_t pos[65537] = {0};

        for (i = 0; i < len; i++)
            pos[iv[i] + 32769]++;

        for (i = 2; i <= 65536; i++)
            pos[i] += pos[i - 1];

        for (i = 0; i < len; i++)
            ov[pos[iv[i] + 32768]++] = i;

        return indices;
    }

    // Small arrays: 2-pass radix sort (256 buckets)
    obj_p temp = I64(len);
    i64_t* ti = AS_I64(temp);

    u64_t pos1[257] = {0};
    u64_t pos2[257] = {0};

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[i] ^ 0x8000);
        pos1[(t & 0xff) + 1]++;
        pos2[(t >> 8) + 1]++;
    }

    for (i = 2; i <= 256; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
    }

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[i] ^ 0x8000);
        ti[pos1[t & 0xff]++] = i;
    }

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[ti[i]] ^ 0x8000);
        ov[pos2[t >> 8]++] = ti[i];
    }

    drop_obj(temp);
    return indices;
}

// Counting sort for i32 with known min and range (ascending)
static obj_p counting_sort_asc_i32(obj_p vec, i64_t min_val, i64_t range, i64_t null_count) {
    i64_t i, len = vec->len;
    i32_t* iv = AS_I32(vec);
    i64_t null_idx = 0;

    obj_p pos_obj = I64(range + 1);
    u64_t* pos = (u64_t*)AS_I64(pos_obj);
    memset(pos, 0, (range + 1) * sizeof(u64_t));

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    // Count occurrences (skip NULLs)
    for (i = 0; i < len; i++) {
        if (iv[i] != NULL_I32)
            pos[iv[i] - min_val + 1]++;
    }

    // Prefix sum (offset by null_count - NULLs go first in ascending)
    pos[0] = null_count;
    for (i = 1; i <= range; i++)
        pos[i] += pos[i - 1];

    // Scatter (NULLs to beginning, others after)
    for (i = 0; i < len; i++) {
        if (iv[i] == NULL_I32)
            ov[null_idx++] = i;
        else
            ov[pos[iv[i] - min_val]++] = i;
    }

    drop_obj(pos_obj);
    return indices;
}

// Counting sort for i32 with known min and range (descending)
static obj_p counting_sort_desc_i32(obj_p vec, i64_t min_val, i64_t range, i64_t null_count) {
    i64_t i, len = vec->len;
    i32_t* iv = AS_I32(vec);
    i64_t null_idx = len - null_count;
    i64_t max_val = min_val + range - 1;

    obj_p pos_obj = I64(range + 1);
    u64_t* pos = (u64_t*)AS_I64(pos_obj);
    memset(pos, 0, (range + 1) * sizeof(u64_t));

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    // Count occurrences (reversed: max goes to bucket 0, min to bucket range-1)
    for (i = 0; i < len; i++) {
        if (iv[i] != NULL_I32)
            pos[max_val - iv[i] + 1]++;
    }

    // Prefix sum
    for (i = 1; i <= range; i++)
        pos[i] += pos[i - 1];

    // Scatter (NULLs to end, others before)
    for (i = 0; i < len; i++) {
        if (iv[i] == NULL_I32)
            ov[null_idx++] = i;
        else
            ov[pos[max_val - iv[i]]++] = i;
    }

    drop_obj(pos_obj);
    return indices;
}

// Radix sort 8-bit for i32 (4 passes, ascending)
static obj_p radix8_sort_asc_i32(obj_p vec) {
    i64_t i, len = vec->len;
    i32_t* iv = AS_I32(vec);
    obj_p temp1 = I64(len);
    obj_p temp2 = I64(len);
    i64_t* t1 = AS_I64(temp1);
    i64_t* t2 = AS_I64(temp2);
    u64_t pos[257];

    // Pass 1: bits 0-7
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[i] ^ 0x80000000;
        pos[(v & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[i] ^ 0x80000000;
        t1[pos[v & 0xff]++] = i;
    }

    // Pass 2: bits 8-15
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        pos[((v >> 8) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        t2[pos[(v >> 8) & 0xff]++] = t1[i];
    }

    // Pass 3: bits 16-23
    memset(pos, 0, sizeof(pos));
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t2[i]] ^ 0x80000000;
        pos[((v >> 16) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t2[i]] ^ 0x80000000;
        t1[pos[(v >> 16) & 0xff]++] = t2[i];
    }

    // Pass 4: bits 24-31
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        pos[(v >> 24) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        ov[pos[v >> 24]++] = t1[i];
    }

    drop_obj(temp1);
    drop_obj(temp2);
    return indices;
}

// Radix sort 8-bit for i32 (4 passes, descending)
static obj_p radix8_sort_desc_i32(obj_p vec) {
    i64_t i, len = vec->len;
    i32_t* iv = AS_I32(vec);
    obj_p temp1 = I64(len);
    obj_p temp2 = I64(len);
    i64_t* t1 = AS_I64(temp1);
    i64_t* t2 = AS_I64(temp2);

    u64_t pos[257];

    // Pass 1: bits 0-7 (descending: 255-x)
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[i] ^ 0x80000000;
        pos[(255 - (v & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[i] ^ 0x80000000;
        t1[pos[255 - (v & 0xff)]++] = i;
    }

    // Pass 2: bits 8-15
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        pos[(255 - ((v >> 8) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        t2[pos[255 - ((v >> 8) & 0xff)]++] = t1[i];
    }

    // Pass 3: bits 16-23
    memset(pos, 0, sizeof(pos));
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t2[i]] ^ 0x80000000;
        pos[(255 - ((v >> 16) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t2[i]] ^ 0x80000000;
        t1[pos[255 - ((v >> 16) & 0xff)]++] = t2[i];
    }

    // Pass 4: bits 24-31
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        pos[(255 - (v >> 24)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u32_t v = (u32_t)iv[t1[i]] ^ 0x80000000;
        ov[pos[255 - (v >> 24)]++] = t1[i];
    }

    drop_obj(temp1);
    drop_obj(temp2);
    return indices;
}

// Radix sort 16-bit for i32 (2 passes, ascending) - current implementation
static obj_p radix16_sort_asc_i32(obj_p vec) {
    i64_t i, t, len = vec->len;
    obj_p temp = I64(len);
    i32_t* iv = AS_I32(vec);
    i64_t* ti = AS_I64(temp);

    obj_p pos1_obj = I64(65537);
    obj_p pos2_obj = I64(65537);
    u64_t* pos1 = (u64_t*)AS_I64(pos1_obj);
    u64_t* pos2 = (u64_t*)AS_I64(pos2_obj);
    memset(pos1, 0, 65537 * sizeof(u64_t));
    memset(pos2, 0, 65537 * sizeof(u64_t));

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i] ^ 0x80000000;
        pos1[(t & 0xffff) + 1]++;
        pos2[(t >> 16) + 1]++;
    }
    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
    }
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i];
        ti[pos1[iv[i] & 0xffff]++] = i;
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[ti[i]] ^ 0x80000000;
        ov[pos2[t >> 16]++] = ti[i];
    }

    drop_obj(pos1_obj);
    drop_obj(pos2_obj);
    drop_obj(temp);
    return indices;
}

// Radix sort 16-bit for i32 (2 passes, descending)
static obj_p radix16_sort_desc_i32(obj_p vec) {
    i64_t i, t, len = vec->len;
    obj_p temp = I64(len);
    i32_t* iv = AS_I32(vec);
    i64_t* ti = AS_I64(temp);

    obj_p pos1_obj = I64(65537);
    obj_p pos2_obj = I64(65537);
    u64_t* pos1 = (u64_t*)AS_I64(pos1_obj);
    u64_t* pos2 = (u64_t*)AS_I64(pos2_obj);
    memset(pos1, 0, 65537 * sizeof(u64_t));
    memset(pos2, 0, 65537 * sizeof(u64_t));

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i] ^ 0x80000000;
        pos1[(65535 - (t & 0xffff)) + 1]++;
        pos2[(65535 - (t >> 16)) + 1]++;
    }
    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
    }
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i] ^ 0x80000000;
        ti[pos1[65535 - (t & 0xffff)]++] = i;
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[ti[i]] ^ 0x80000000;
        ov[pos2[65535 - (t >> 16)]++] = ti[i];
    }

    drop_obj(pos1_obj);
    drop_obj(pos2_obj);
    drop_obj(temp);
    return indices;
}

// ============================================================================
// Parallel Counting Sort for I32 (dynamic range)
// ============================================================================

typedef struct {
    i32_t* data;
    i64_t chunk_size;
    i64_t* histograms;
    i64_t min_val;
    i64_t range;
} histogram_i32_ctx_t;

typedef struct {
    i32_t* data;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
    i64_t* null_positions;
    i64_t null_offset;
    i64_t min_val;
    i64_t range;
    i64_t asc;
} scatter_i32_ctx_t;

static obj_p histogram_i32_worker(i64_t len, i64_t offset, void* ctx) {
    histogram_i32_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i32_t* data = c->data + offset;
    i64_t* hist = c->histograms + worker_id * (c->range + 1);
    i64_t null_count = 0;

    memset(hist, 0, c->range * sizeof(i64_t));

    for (i64_t i = 0; i < len; i++) {
        i32_t val = data[i];
        if (val == NULL_I32)
            null_count++;
        else
            hist[val - c->min_val]++;
    }
    hist[c->range] = null_count;

    return NULL_OBJ;
}

static obj_p scatter_i32_worker(i64_t len, i64_t offset, void* ctx) {
    scatter_i32_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i32_t* data = c->data + offset;
    i64_t* pos = c->positions + worker_id * (c->range + 1);
    i64_t null_pos = c->null_positions[worker_id];

    if (c->asc > 0) {
        for (i64_t i = 0; i < len; i++) {
            i32_t val = data[i];
            if (val == NULL_I32)
                c->out[null_pos++] = offset + i;
            else
                c->out[c->null_offset + pos[val - c->min_val]++] = offset + i;
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            i32_t val = data[i];
            if (val == NULL_I32)
                c->out[c->null_offset + null_pos++] = offset + i;
            else
                c->out[pos[c->range - 1 - (val - c->min_val)]++] = offset + i;
        }
    }

    return NULL_OBJ;
}

static obj_p parallel_counting_sort_i32(obj_p vec, i64_t min_val, i64_t range, i64_t asc) {
    i64_t len = vec->len;
    i32_t* data = AS_I32(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    obj_p hist_obj = I64(n * (range + 1));
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        return indices;
    }

    // Phase 1: parallel histogram
    histogram_i32_ctx_t hist_ctx = {data, chunk_size, histograms, min_val, range};
    pool_map(len, histogram_i32_worker, &hist_ctx);

    // Phase 2: merge histograms and compute prefix sum
    obj_p counts_obj = I64(range);
    if (IS_ERR(counts_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return counts_obj;
    }
    i64_t* global_counts = AS_I64(counts_obj);

    memset(global_counts, 0, range * sizeof(i64_t));
    i64_t total_null_count = 0;
    for (i64_t w = 0; w < n; w++) {
        i64_t* worker_hist = histograms + w * (range + 1);
        for (i64_t b = 0; b < range; b++)
            global_counts[b] += worker_hist[b];
        total_null_count += worker_hist[range];
    }

    i64_t non_null_count = len - total_null_count;

    obj_p prefix_obj = I64(range);
    if (IS_ERR(prefix_obj)) {
        drop_obj(hist_obj);
        drop_obj(counts_obj);
        drop_obj(indices);
        return prefix_obj;
    }
    i64_t* prefix = AS_I64(prefix_obj);

    if (asc > 0) {
        prefix[0] = 0;
        for (i64_t b = 1; b < range; b++)
            prefix[b] = prefix[b-1] + global_counts[b-1];
    } else {
        prefix[range-1] = 0;
        for (i64_t b = range - 2; b >= 0; b--)
            prefix[b] = prefix[b+1] + global_counts[b+1];
    }

    // Compute per-worker positions
    for (i64_t b = 0; b < range; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * (range + 1);
            i64_t cnt = worker_hist[b];
            worker_hist[b] = pos;
            pos += cnt;
        }
    }

    drop_obj(counts_obj);
    drop_obj(prefix_obj);

    obj_p null_pos_obj = I64(n);
    if (IS_ERR(null_pos_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return null_pos_obj;
    }
    i64_t* null_positions = AS_I64(null_pos_obj);

    i64_t null_offset_acc = 0;
    for (i64_t w = 0; w < n; w++) {
        null_positions[w] = null_offset_acc;
        null_offset_acc += histograms[w * (range + 1) + range];
    }

    // Phase 3: parallel scatter
    i64_t null_offset = asc > 0 ? total_null_count : non_null_count;
    scatter_i32_ctx_t scatter_ctx = {data, chunk_size, histograms, AS_I64(indices), null_positions, null_offset, min_val, range, asc};
    pool_map(len, scatter_i32_worker, &scatter_ctx);

    drop_obj(hist_obj);
    drop_obj(null_pos_obj);
    return indices;
}

// ============================================================================
// Parallel Radix Sort 16-bit for I32
// ============================================================================

typedef struct {
    i32_t* data;
    i64_t chunk_size;
    i64_t* histograms;
    i64_t pass;
} radix16_hist_i32_ctx_t;

typedef struct {
    i32_t* data;
    i64_t* src_indices;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
    i64_t pass;
    i64_t asc;
} radix16_scatter_i32_ctx_t;

#define RADIX16_BUCKETS 65536

static obj_p radix16_hist_i32_worker(i64_t len, i64_t offset, void* ctx) {
    radix16_hist_i32_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i32_t* data = c->data + offset;
    i64_t* hist = c->histograms + worker_id * RADIX16_BUCKETS;

    memset(hist, 0, RADIX16_BUCKETS * sizeof(i64_t));

    if (c->pass == 0) {
        for (i64_t i = 0; i < len; i++) {
            u32_t v = (u32_t)data[i] ^ 0x80000000;
            hist[v & 0xffff]++;
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            u32_t v = (u32_t)data[i] ^ 0x80000000;
            hist[v >> 16]++;
        }
    }

    return NULL_OBJ;
}

static obj_p radix16_scatter_i32_worker(i64_t len, i64_t offset, void* ctx) {
    radix16_scatter_i32_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i64_t* pos = c->positions + worker_id * RADIX16_BUCKETS;

    if (c->pass == 0) {
        i32_t* data = c->data + offset;
        if (c->asc > 0) {
            for (i64_t i = 0; i < len; i++) {
                u32_t v = (u32_t)data[i] ^ 0x80000000;
                c->out[pos[v & 0xffff]++] = offset + i;
            }
        } else {
            for (i64_t i = 0; i < len; i++) {
                u32_t v = (u32_t)data[i] ^ 0x80000000;
                c->out[pos[65535 - (v & 0xffff)]++] = offset + i;
            }
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            i64_t idx = c->src_indices[offset + i];
            u32_t v = (u32_t)c->data[idx] ^ 0x80000000;
            if (c->asc > 0)
                c->out[pos[v >> 16]++] = idx;
            else
                c->out[pos[65535 - (v >> 16)]++] = idx;
        }
    }

    return NULL_OBJ;
}

static obj_p parallel_radix16_sort_i32(obj_p vec, i64_t asc) {
    i64_t len = vec->len;
    i32_t* data = AS_I32(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    obj_p hist_obj = I64(n * RADIX16_BUCKETS);
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    obj_p temp = I64(len);
    if (IS_ERR(temp)) {
        drop_obj(hist_obj);
        return temp;
    }

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        drop_obj(temp);
        return indices;
    }

    // Pass 1: low 16 bits
    radix16_hist_i32_ctx_t hist_ctx1 = {data, chunk_size, histograms, 0};
    pool_map(len, radix16_hist_i32_worker, &hist_ctx1);

    // Merge and prefix sum
    obj_p prefix_obj = I64(RADIX16_BUCKETS);
    if (IS_ERR(prefix_obj)) {
        drop_obj(hist_obj);
        drop_obj(temp);
        drop_obj(indices);
        return prefix_obj;
    }
    i64_t* prefix = AS_I64(prefix_obj);

    memset(prefix, 0, RADIX16_BUCKETS * sizeof(i64_t));
    for (i64_t w = 0; w < n; w++) {
        i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
        for (i64_t b = 0; b < RADIX16_BUCKETS; b++)
            prefix[b] += worker_hist[b];
    }

    // Compute prefix sum
    i64_t acc = 0;
    for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
        i64_t cnt = prefix[b];
        prefix[b] = acc;
        acc += cnt;
    }

    // Compute per-worker positions
    for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
            i64_t cnt = worker_hist[b];
            worker_hist[b] = pos;
            pos += cnt;
        }
    }

    // Scatter pass 1
    radix16_scatter_i32_ctx_t scatter_ctx1 = {data, NULL, chunk_size, histograms, AS_I64(temp), 0, asc};
    pool_map(len, radix16_scatter_i32_worker, &scatter_ctx1);

    // Pass 2: high 16 bits
    radix16_hist_i32_ctx_t hist_ctx2 = {data, chunk_size, histograms, 1};
    pool_map(len, radix16_hist_i32_worker, &hist_ctx2);

    memset(prefix, 0, RADIX16_BUCKETS * sizeof(i64_t));
    for (i64_t w = 0; w < n; w++) {
        i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
        for (i64_t b = 0; b < RADIX16_BUCKETS; b++)
            prefix[b] += worker_hist[b];
    }

    acc = 0;
    for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
        i64_t cnt = prefix[b];
        prefix[b] = acc;
        acc += cnt;
    }

    for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
            i64_t cnt = worker_hist[b];
            worker_hist[b] = pos;
            pos += cnt;
        }
    }

    // Scatter pass 2
    radix16_scatter_i32_ctx_t scatter_ctx2 = {data, AS_I64(temp), chunk_size, histograms, AS_I64(indices), 1, asc};
    pool_map(len, radix16_scatter_i32_worker, &scatter_ctx2);

    drop_obj(hist_obj);
    drop_obj(prefix_obj);
    drop_obj(temp);
    return indices;
}

obj_p ray_sort_asc_i32(obj_p vec) {
    i64_t len = vec->len;
    index_scope_t scope = index_scope_i32(AS_I32(vec), NULL, len);

    if (len < SMALL_VEC_THRESHOLD) {
        if (scope.range <= COUNTING_SORT_MAX_RANGE_I32)
            return counting_sort_asc_i32(vec, scope.min, scope.range, scope.null_count);
        else
            return radix8_sort_asc_i32(vec);
    }

    i64_t n_workers = pool_get_executors_count(pool_get());
    if (scope.range <= COUNTING_SORT_MAX_RANGE_I32 || scope.range <= len / n_workers) {
        return parallel_counting_sort_i32(vec, scope.min, scope.range, 1);
    } else {
        if (len < PARALLEL_RADIX_SORT_THRESHOLD)
            return radix16_sort_asc_i32(vec);
        else
            return parallel_radix16_sort_i32(vec, 1);
    }
}

// ============================================================================
// I64 Counting Sort (with scope parameters)
// ============================================================================

static obj_p counting_sort_asc_i64(obj_p vec, i64_t min_val, i64_t range, i64_t null_count) {
    i64_t len = vec->len;
    i64_t* data = AS_I64(vec);
    i64_t non_null_count = len - null_count;

    obj_p counts_obj = I64(range);
    if (IS_ERR(counts_obj)) return counts_obj;
    i64_t* counts = AS_I64(counts_obj);
    memset(counts, 0, range * sizeof(i64_t));

    // Count occurrences (skip NULLs)
    for (i64_t i = 0; i < len; i++) {
        if (data[i] != NULL_I64)
            counts[data[i] - min_val]++;
    }

    // Compute prefix sum
    i64_t acc = null_count;  // NULLs go first in ascending
    for (i64_t b = 0; b < range; b++) {
        i64_t cnt = counts[b];
        counts[b] = acc;
        acc += cnt;
    }

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(counts_obj);
        return indices;
    }
    i64_t* result = AS_I64(indices);

    // Place NULLs first
    i64_t null_pos = 0;
    for (i64_t i = 0; i < len; i++) {
        if (data[i] == NULL_I64)
            result[null_pos++] = i;
        else
            result[counts[data[i] - min_val]++] = i;
    }

    drop_obj(counts_obj);
    return indices;
}

static obj_p counting_sort_desc_i64(obj_p vec, i64_t min_val, i64_t range, i64_t null_count) {
    i64_t len = vec->len;
    i64_t* data = AS_I64(vec);
    i64_t non_null_count = len - null_count;

    obj_p counts_obj = I64(range);
    if (IS_ERR(counts_obj)) return counts_obj;
    i64_t* counts = AS_I64(counts_obj);
    memset(counts, 0, range * sizeof(i64_t));

    // Count occurrences (skip NULLs)
    for (i64_t i = 0; i < len; i++) {
        if (data[i] != NULL_I64)
            counts[data[i] - min_val]++;
    }

    // Compute prefix sum (descending)
    i64_t acc = 0;
    for (i64_t b = range - 1; b >= 0; b--) {
        i64_t cnt = counts[b];
        counts[b] = acc;
        acc += cnt;
    }

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(counts_obj);
        return indices;
    }
    i64_t* result = AS_I64(indices);

    // Place values (NULLs go last in descending)
    i64_t null_pos = non_null_count;
    for (i64_t i = 0; i < len; i++) {
        if (data[i] == NULL_I64)
            result[null_pos++] = i;
        else
            result[counts[data[i] - min_val]++] = i;
    }

    drop_obj(counts_obj);
    return indices;
}

// ============================================================================
// I64 Radix Sort 8-bit (8 passes, for small vectors)
// ============================================================================

static obj_p radix8_sort_asc_i64(obj_p vec) {
    i64_t i, len = vec->len;
    i64_t* iv = AS_I64(vec);
    obj_p temp1 = I64(len);
    obj_p temp2 = I64(len);
    i64_t* t1 = AS_I64(temp1);
    i64_t* t2 = AS_I64(temp2);
    u64_t pos[257];

    // Pass 1: bits 0-7
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[i] ^ 0x8000000000000000ULL;
        pos[(v & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[i] ^ 0x8000000000000000ULL;
        t1[pos[v & 0xff]++] = i;
    }

    // Pass 2: bits 8-15
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 8) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[(v >> 8) & 0xff]++] = t1[i];
    }

    // Pass 3: bits 16-23
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 16) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[(v >> 16) & 0xff]++] = t2[i];
    }

    // Pass 4: bits 24-31
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 24) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[(v >> 24) & 0xff]++] = t1[i];
    }

    // Pass 5: bits 32-39
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 32) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[(v >> 32) & 0xff]++] = t2[i];
    }

    // Pass 6: bits 40-47
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 40) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[(v >> 40) & 0xff]++] = t1[i];
    }

    // Pass 7: bits 48-55
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[((v >> 48) & 0xff) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[(v >> 48) & 0xff]++] = t2[i];
    }

    // Pass 8: bits 56-63
    memset(pos, 0, sizeof(pos));
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[(v >> 56) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        ov[pos[v >> 56]++] = t1[i];
    }

    drop_obj(temp1);
    drop_obj(temp2);
    return indices;
}

static obj_p radix8_sort_desc_i64(obj_p vec) {
    i64_t i, len = vec->len;
    i64_t* iv = AS_I64(vec);
    obj_p temp1 = I64(len);
    obj_p temp2 = I64(len);
    i64_t* t1 = AS_I64(temp1);
    i64_t* t2 = AS_I64(temp2);
    u64_t pos[257];

    // Pass 1: bits 0-7 (descending)
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[i] ^ 0x8000000000000000ULL;
        pos[(255 - (v & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[i] ^ 0x8000000000000000ULL;
        t1[pos[255 - (v & 0xff)]++] = i;
    }

    // Pass 2: bits 8-15
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 8) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[255 - ((v >> 8) & 0xff)]++] = t1[i];
    }

    // Pass 3: bits 16-23
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 16) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[255 - ((v >> 16) & 0xff)]++] = t2[i];
    }

    // Pass 4: bits 24-31
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 24) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[255 - ((v >> 24) & 0xff)]++] = t1[i];
    }

    // Pass 5: bits 32-39
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 32) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[255 - ((v >> 32) & 0xff)]++] = t2[i];
    }

    // Pass 6: bits 40-47
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 40) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        t2[pos[255 - ((v >> 40) & 0xff)]++] = t1[i];
    }

    // Pass 7: bits 48-55
    memset(pos, 0, sizeof(pos));
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        pos[(255 - ((v >> 48) & 0xff)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t2[i]] ^ 0x8000000000000000ULL;
        t1[pos[255 - ((v >> 48) & 0xff)]++] = t2[i];
    }

    // Pass 8: bits 56-63
    memset(pos, 0, sizeof(pos));
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        pos[(255 - (v >> 56)) + 1]++;
    }
    for (i = 2; i <= 256; i++) pos[i] += pos[i - 1];
    for (i = 0; i < len; i++) {
        u64_t v = (u64_t)iv[t1[i]] ^ 0x8000000000000000ULL;
        ov[pos[255 - (v >> 56)]++] = t1[i];
    }

    drop_obj(temp1);
    drop_obj(temp2);
    return indices;
}

// ============================================================================
// I64 Radix Sort 16-bit (4 passes, for medium vectors)
// ============================================================================

static obj_p radix16_sort_asc_i64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i64_t* iv = AS_I64(vec);
    i64_t* t = AS_I64(temp);

    // Single histogram allocation for better cache locality
    obj_p hist_obj = I64(65537 * 4);
    u64_t* hist = (u64_t*)AS_I64(hist_obj);
    u64_t* pos1 = hist;
    u64_t* pos2 = hist + 65537;
    u64_t* pos3 = hist + 65537 * 2;
    u64_t* pos4 = hist + 65537 * 3;
    memset(hist, 0, 65537 * 4 * sizeof(u64_t));

    // Count occurrences with prefetching
    #define PREFETCH_DIST 16
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[i + PREFETCH_DIST], 0, 0);
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        pos1[(u & 0xffff) + 1]++;
        pos2[((u >> 16) & 0xffff) + 1]++;
        pos3[((u >> 32) & 0xffff) + 1]++;
        pos4[(u >> 48) + 1]++;
    }

    // Calculate cumulative positions
    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
        pos3[i] += pos3[i - 1];
        pos4[i] += pos4[i - 1];
    }

    // Pass 1: sort by least significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        t[pos1[u & 0xffff]++] = i;
    }

    // Pass 2: sort by second 16-bit chunk (with prefetch for indirect access)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[t[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos2[(u >> 16) & 0xffff]++] = t[i];
    }

    // Pass 3: sort by third 16-bit chunk (with prefetch)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[ov[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[ov[i]] ^ 0x8000000000000000ULL;
        t[pos3[(u >> 32) & 0xffff]++] = ov[i];
    }

    // Pass 4: sort by most significant 16 bits (with prefetch)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[t[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos4[u >> 48]++] = t[i];
    }
    #undef PREFETCH_DIST

    drop_obj(hist_obj);
    drop_obj(temp);
    return indices;
}

static obj_p radix16_sort_desc_i64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i64_t* iv = AS_I64(vec);
    i64_t* t = AS_I64(temp);

    // Single histogram allocation for better cache locality
    obj_p hist_obj = I64(65537 * 4);
    u64_t* hist = (u64_t*)AS_I64(hist_obj);
    u64_t* pos1 = hist;
    u64_t* pos2 = hist + 65537;
    u64_t* pos3 = hist + 65537 * 2;
    u64_t* pos4 = hist + 65537 * 3;
    memset(hist, 0, 65537 * 4 * sizeof(u64_t));

    // Count occurrences with prefetching (descending order)
    #define PREFETCH_DIST 16
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[i + PREFETCH_DIST], 0, 0);
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        pos1[(65535 - (u & 0xffff)) + 1]++;
        pos2[(65535 - ((u >> 16) & 0xffff)) + 1]++;
        pos3[(65535 - ((u >> 32) & 0xffff)) + 1]++;
        pos4[(65535 - (u >> 48)) + 1]++;
    }

    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
        pos3[i] += pos3[i - 1];
        pos4[i] += pos4[i - 1];
    }

    // Pass 1
    for (i = 0; i < len; i++) {
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        t[pos1[65535 - (u & 0xffff)]++] = i;
    }

    // Pass 2 (with prefetch)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[t[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos2[65535 - ((u >> 16) & 0xffff)]++] = t[i];
    }

    // Pass 3 (with prefetch)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[ov[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[ov[i]] ^ 0x8000000000000000ULL;
        t[pos3[65535 - ((u >> 32) & 0xffff)]++] = ov[i];
    }

    // Pass 4 (with prefetch)
    for (i = 0; i < len; i++) {
        if (i + PREFETCH_DIST < len)
            __builtin_prefetch(&iv[t[i + PREFETCH_DIST]], 0, 0);
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos4[65535 - (u >> 48)]++] = t[i];
    }
    #undef PREFETCH_DIST

    drop_obj(hist_obj);
    drop_obj(temp);
    return indices;
}

// ============================================================================
// Parallel Counting Sort for I64
// ============================================================================

typedef struct {
    i64_t* data;
    i64_t chunk_size;
    i64_t* histograms;
    i64_t min_val;
    i64_t range;
} histogram_i64_ctx_t;

typedef struct {
    i64_t* data;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
    i64_t* null_positions;
    i64_t null_offset;
    i64_t min_val;
    i64_t range;
    i64_t asc;
} scatter_i64_ctx_t;

static obj_p histogram_i64_worker(i64_t len, i64_t offset, void* ctx) {
    histogram_i64_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i64_t* data = c->data + offset;
    i64_t* hist = c->histograms + worker_id * (c->range + 1);

    memset(hist, 0, (c->range + 1) * sizeof(i64_t));

    for (i64_t i = 0; i < len; i++) {
        if (data[i] == NULL_I64)
            hist[c->range]++;  // NULL count in last slot
        else
            hist[data[i] - c->min_val]++;
    }

    return NULL_OBJ;
}

static obj_p scatter_i64_worker(i64_t len, i64_t offset, void* ctx) {
    scatter_i64_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i64_t* data = c->data + offset;
    i64_t* pos = c->positions + worker_id * (c->range + 1);
    i64_t null_pos = c->null_offset + c->null_positions[worker_id];

    for (i64_t i = 0; i < len; i++) {
        if (data[i] == NULL_I64) {
            if (c->asc > 0)
                c->out[null_pos++] = offset + i;  // NULLs first in asc
            else
                c->out[null_pos++] = offset + i;  // NULLs last in desc
        } else {
            c->out[pos[data[i] - c->min_val]++] = offset + i;
        }
    }

    return NULL_OBJ;
}

static obj_p parallel_counting_sort_i64(obj_p vec, i64_t min_val, i64_t range, i64_t asc) {
    i64_t len = vec->len;
    i64_t* data = AS_I64(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    obj_p hist_obj = I64(n * (range + 1));
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        return indices;
    }

    // Phase 1: parallel histogram
    histogram_i64_ctx_t hist_ctx = {data, chunk_size, histograms, min_val, range};
    pool_map(len, histogram_i64_worker, &hist_ctx);

    // Phase 2: merge histograms and compute prefix sum
    obj_p counts_obj = I64(range);
    if (IS_ERR(counts_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return counts_obj;
    }
    i64_t* global_counts = AS_I64(counts_obj);

    memset(global_counts, 0, range * sizeof(i64_t));
    i64_t total_null_count = 0;
    for (i64_t w = 0; w < n; w++) {
        i64_t* worker_hist = histograms + w * (range + 1);
        for (i64_t b = 0; b < range; b++)
            global_counts[b] += worker_hist[b];
        total_null_count += worker_hist[range];
    }

    i64_t non_null_count = len - total_null_count;

    obj_p prefix_obj = I64(range);
    if (IS_ERR(prefix_obj)) {
        drop_obj(hist_obj);
        drop_obj(counts_obj);
        drop_obj(indices);
        return prefix_obj;
    }
    i64_t* prefix = AS_I64(prefix_obj);

    if (asc > 0) {
        prefix[0] = total_null_count;  // NULLs first
        for (i64_t b = 1; b < range; b++)
            prefix[b] = prefix[b-1] + global_counts[b-1];
    } else {
        prefix[range-1] = 0;
        for (i64_t b = range - 2; b >= 0; b--)
            prefix[b] = prefix[b+1] + global_counts[b+1];
    }

    // Compute per-worker positions
    for (i64_t b = 0; b < range; b++) {
        i64_t pos = prefix[b];
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * (range + 1);
            i64_t cnt = worker_hist[b];
            worker_hist[b] = pos;
            pos += cnt;
        }
    }

    drop_obj(counts_obj);
    drop_obj(prefix_obj);

    obj_p null_pos_obj = I64(n);
    if (IS_ERR(null_pos_obj)) {
        drop_obj(hist_obj);
        drop_obj(indices);
        return null_pos_obj;
    }
    i64_t* null_positions = AS_I64(null_pos_obj);

    i64_t null_offset_acc = 0;
    for (i64_t w = 0; w < n; w++) {
        null_positions[w] = null_offset_acc;
        null_offset_acc += histograms[w * (range + 1) + range];
    }

    // Phase 3: parallel scatter
    i64_t null_offset = asc > 0 ? 0 : non_null_count;
    scatter_i64_ctx_t scatter_ctx = {data, chunk_size, histograms, AS_I64(indices), null_positions, null_offset, min_val, range, asc};
    pool_map(len, scatter_i64_worker, &scatter_ctx);

    drop_obj(hist_obj);
    drop_obj(null_pos_obj);
    return indices;
}

// ============================================================================
// Parallel Radix Sort 16-bit for I64 (4 passes)
// ============================================================================

typedef struct {
    i64_t* data;
    i64_t* src_indices;
    i64_t chunk_size;
    i64_t* histograms;
    i64_t pass;
} radix16_hist_i64_ctx_t;

typedef struct {
    i64_t* data;
    i64_t* src_indices;
    i64_t chunk_size;
    i64_t* positions;
    i64_t* out;
    i64_t pass;
    i64_t asc;
} radix16_scatter_i64_ctx_t;

static obj_p radix16_hist_i64_worker(i64_t len, i64_t offset, void* ctx) {
    radix16_hist_i64_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i64_t* hist = c->histograms + worker_id * RADIX16_BUCKETS;

    memset(hist, 0, RADIX16_BUCKETS * sizeof(i64_t));

    if (c->pass == 0) {
        i64_t* data = c->data + offset;
        for (i64_t i = 0; i < len; i++) {
            u64_t v = (u64_t)data[i] ^ 0x8000000000000000ULL;
            hist[(v >> (c->pass * 16)) & 0xffff]++;
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            i64_t idx = c->src_indices[offset + i];
            u64_t v = (u64_t)c->data[idx] ^ 0x8000000000000000ULL;
            hist[(v >> (c->pass * 16)) & 0xffff]++;
        }
    }

    return NULL_OBJ;
}

static obj_p radix16_scatter_i64_worker(i64_t len, i64_t offset, void* ctx) {
    radix16_scatter_i64_ctx_t* c = ctx;
    i64_t worker_id = offset / c->chunk_size;
    i64_t* pos = c->positions + worker_id * RADIX16_BUCKETS;

    if (c->pass == 0) {
        i64_t* data = c->data + offset;
        if (c->asc > 0) {
            for (i64_t i = 0; i < len; i++) {
                u64_t v = (u64_t)data[i] ^ 0x8000000000000000ULL;
                c->out[pos[v & 0xffff]++] = offset + i;
            }
        } else {
            for (i64_t i = 0; i < len; i++) {
                u64_t v = (u64_t)data[i] ^ 0x8000000000000000ULL;
                c->out[pos[65535 - (v & 0xffff)]++] = offset + i;
            }
        }
    } else {
        for (i64_t i = 0; i < len; i++) {
            i64_t idx = c->src_indices[offset + i];
            u64_t v = (u64_t)c->data[idx] ^ 0x8000000000000000ULL;
            u64_t bucket = (v >> (c->pass * 16)) & 0xffff;
            if (c->asc > 0)
                c->out[pos[bucket]++] = idx;
            else
                c->out[pos[65535 - bucket]++] = idx;
        }
    }

    return NULL_OBJ;
}

static obj_p parallel_radix16_sort_i64(obj_p vec, i64_t asc) {
    i64_t len = vec->len;
    i64_t* data = AS_I64(vec);

    pool_p pool = pool_get();
    i64_t n = pool_split_by(pool, len, 0);
    i64_t chunk_size = len / n;

    obj_p hist_obj = I64(n * RADIX16_BUCKETS);
    if (IS_ERR(hist_obj)) return hist_obj;
    i64_t* histograms = AS_I64(hist_obj);

    obj_p temp = I64(len);
    if (IS_ERR(temp)) {
        drop_obj(hist_obj);
        return temp;
    }

    obj_p indices = I64(len);
    if (IS_ERR(indices)) {
        drop_obj(hist_obj);
        drop_obj(temp);
        return indices;
    }

    obj_p prefix_obj = I64(RADIX16_BUCKETS);
    if (IS_ERR(prefix_obj)) {
        drop_obj(hist_obj);
        drop_obj(temp);
        drop_obj(indices);
        return prefix_obj;
    }
    i64_t* prefix = AS_I64(prefix_obj);

    // 4 passes for 64-bit values
    obj_p* buffers[2] = {&temp, &indices};

    for (i64_t pass = 0; pass < 4; pass++) {
        // Get source indices from previous pass
        obj_p src = (pass == 0) ? NULL_OBJ : *buffers[(pass - 1) % 2];
        i64_t* src_indices = (pass == 0) ? NULL : AS_I64(src);

        // Histogram pass
        radix16_hist_i64_ctx_t hist_ctx = {data, src_indices, chunk_size, histograms, pass};
        pool_map(len, radix16_hist_i64_worker, &hist_ctx);

        // Merge and prefix sum
        memset(prefix, 0, RADIX16_BUCKETS * sizeof(i64_t));
        for (i64_t w = 0; w < n; w++) {
            i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
            for (i64_t b = 0; b < RADIX16_BUCKETS; b++)
                prefix[b] += worker_hist[b];
        }

        i64_t acc = 0;
        for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
            i64_t cnt = prefix[b];
            prefix[b] = acc;
            acc += cnt;
        }

        // Compute per-worker positions
        for (i64_t b = 0; b < RADIX16_BUCKETS; b++) {
            i64_t pos = prefix[b];
            for (i64_t w = 0; w < n; w++) {
                i64_t* worker_hist = histograms + w * RADIX16_BUCKETS;
                i64_t cnt = worker_hist[b];
                worker_hist[b] = pos;
                pos += cnt;
            }
        }

        // Scatter pass
        obj_p dst = *buffers[pass % 2];
        radix16_scatter_i64_ctx_t scatter_ctx = {data, src_indices, chunk_size, histograms, AS_I64(dst), pass, asc};
        pool_map(len, radix16_scatter_i64_worker, &scatter_ctx);
    }

    drop_obj(hist_obj);
    drop_obj(prefix_obj);

    // Result is in indices (pass 3 output goes to buffers[3%2] = buffers[1] = &indices)
    drop_obj(temp);
    return indices;
}

// Helper inline function to convert f64 to sortable u64
static inline u64_t f64_to_sortable_u64(f64_t value) {
    union {
        f64_t f;
        u64_t u;
    } u;

    if (ISNANF64(value)) {
        return 0ull;
    }
    u.f = value;

    // Flip sign bit for negative values to maintain correct ordering
    if (u.u & 0x8000000000000000ULL) {
        u.u = ~u.u;
    } else {
        u.u |= 0x8000000000000000ULL;
    }

    return u.u;
}

obj_p ray_sort_asc_i64(obj_p vec) {
    i64_t len = vec->len;
    index_scope_t scope = index_scope_i64(AS_I64(vec), NULL, len);

    if (len < SMALL_VEC_THRESHOLD) {
        if (scope.range <= COUNTING_SORT_MAX_RANGE_I64)
            return counting_sort_asc_i64(vec, scope.min, scope.range, scope.null_count);
        else
            return radix8_sort_asc_i64(vec);
    }

    i64_t n_workers = pool_get_executors_count(pool_get());
    if (scope.range <= COUNTING_SORT_MAX_RANGE_I64 || scope.range <= len / n_workers) {
        return parallel_counting_sort_i64(vec, scope.min, scope.range, 1);
    } else {
        if (len < PARALLEL_RADIX_SORT_THRESHOLD)
            return radix16_sort_asc_i64(vec);
        else
            return parallel_radix16_sort_i64(vec, 1);
    }
}

obj_p ray_sort_asc_f64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    f64_t* fv = AS_F64(vec);
    i64_t* t = AS_I64(temp);

    // Allocate on heap to avoid stack overflow on Windows
    obj_p pos1_obj = I64(65537);
    obj_p pos2_obj = I64(65537);
    obj_p pos3_obj = I64(65537);
    obj_p pos4_obj = I64(65537);
    u64_t* pos1 = (u64_t*)AS_I64(pos1_obj);
    u64_t* pos2 = (u64_t*)AS_I64(pos2_obj);
    u64_t* pos3 = (u64_t*)AS_I64(pos3_obj);
    u64_t* pos4 = (u64_t*)AS_I64(pos4_obj);
    memset(pos1, 0, 65537 * sizeof(u64_t));
    memset(pos2, 0, 65537 * sizeof(u64_t));
    memset(pos3, 0, 65537 * sizeof(u64_t));
    memset(pos4, 0, 65537 * sizeof(u64_t));

    // Count occurrences of each 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[i]);

        pos1[(u & 0xffff) + 1]++;
        pos2[((u >> 16) & 0xffff) + 1]++;
        pos3[((u >> 32) & 0xffff) + 1]++;
        pos4[(u >> 48) + 1]++;
    }

    // Calculate cumulative positions for each pass
    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
        pos3[i] += pos3[i - 1];
        pos4[i] += pos4[i - 1];
    }

    // First pass: sort by least significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[i]);
        t[pos1[u & 0xffff]++] = i;
    }

    // Second pass: sort by second 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[t[i]]);
        ov[pos2[(u >> 16) & 0xffff]++] = t[i];
    }

    // Third pass: sort by third 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[ov[i]]);
        t[pos3[(u >> 32) & 0xffff]++] = ov[i];
    }

    // Fourth pass: sort by most significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[t[i]]);
        ov[pos4[u >> 48]++] = t[i];
    }

    drop_obj(pos1_obj);
    drop_obj(pos2_obj);
    drop_obj(pos3_obj);
    drop_obj(pos4_obj);
    drop_obj(temp);
    return indices;
}

obj_p ray_sort_asc(obj_p vec) {
    i64_t len = vec->len;
    obj_p indices;

    if (len == 0) return I64(0);

    if (len == 1) {
        indices = I64(1);
        AS_I64(indices)[0] = 0;
        indices->attrs = ATTR_ASC | ATTR_DISTINCT;
        return indices;
    }

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        indices->attrs = ATTR_ASC | ATTR_DISTINCT;
        iota_ctx_t ctx = {AS_I64(indices), len};
        pool_map(len, iota_asc_worker, &ctx);
        return indices;
    }

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        indices->attrs = ATTR_DESC | ATTR_DISTINCT;
        iota_ctx_t ctx = {AS_I64(indices), len};
        pool_map(len, iota_desc_worker, &ctx);
        return indices;
    }

    switch (vec->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            return ray_sort_asc_u8(vec);
        case TYPE_I16:
            return ray_sort_asc_i16(vec);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return ray_sort_asc_i32(vec);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return ray_sort_asc_i64(vec);
        case TYPE_F64:
            return ray_sort_asc_f64(vec);
        case TYPE_SYMBOL:
            // Use optimized sorting
            return ray_iasc_optimized(vec);
        case TYPE_LIST:
            return mergesort_generic_obj(vec, 1);
        case TYPE_DICT:
            return at_obj(AS_LIST(vec)[0], ray_sort_asc(AS_LIST(vec)[1]));
        default:
            return err_type(0, 0, 0, 0);
    }
}

obj_p ray_sort_desc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    u8_t* iv = AS_U8(vec);

    if (len >= PARALLEL_SORT_THRESHOLD_U8)
        return parallel_counting_sort_u8(vec, -1);

    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);

    u64_t pos[257] = {0};

    for (i = 0; i < len; i++)
        pos[iv[i]]++;

    for (i = 254; i >= 0; i--)
        pos[i] += pos[i + 1];

    for (i = 0; i < len; i++)
        ov[pos[iv[i] + 1]++] = i;

    return indices;
}

obj_p ray_sort_desc_i16(obj_p vec) {
    i64_t i, len = vec->len;
    i16_t* iv = AS_I16(vec);

    if (len >= PARALLEL_COUNTING_SORT_THRESHOLD)
        return parallel_counting_sort_i16(vec, -1);

    // Medium arrays: 1-pass counting sort (65536 buckets)
    if (len >= SMALL_VEC_THRESHOLD) {
        obj_p indices = I64(len);
        i64_t* ov = AS_I64(indices);

        u64_t pos[65537] = {0};

        for (i = 0; i < len; i++)
            pos[(u16_t)(iv[i] + 32768)]++;

        for (i = 65534; i >= 0; i--)
            pos[i] += pos[i + 1];

        for (i = 0; i < len; i++)
            ov[pos[(u64_t)(iv[i] + 32769)]++] = i;

        return indices;
    }

    // Small arrays: 2-pass radix sort (256 buckets)
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i64_t* ti = AS_I64(temp);

    u64_t pos1[257] = {0};
    u64_t pos2[257] = {0};

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[i] ^ 0x8000);
        pos1[t & 0xff]++;
        pos2[t >> 8]++;
    }

    for (i = 254; i >= 0; i--) {
        pos1[i] += pos1[i + 1];
        pos2[i] += pos2[i + 1];
    }

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[i] ^ 0x8000);
        ti[pos1[(t & 0xff) + 1]++] = i;
    }

    for (i = 0; i < len; i++) {
        u16_t t = (u16_t)(iv[ti[i]] ^ 0x8000);
        ov[pos2[(t >> 8) + 1]++] = ti[i];
    }

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_desc_i32(obj_p vec) {
    i64_t len = vec->len;
    index_scope_t scope = index_scope_i32(AS_I32(vec), NULL, len);

    if (len < SMALL_VEC_THRESHOLD) {
        if (scope.range <= COUNTING_SORT_MAX_RANGE_I32)
            return counting_sort_desc_i32(vec, scope.min, scope.range, scope.null_count);
        else
            return radix8_sort_desc_i32(vec);
    }

    i64_t n_workers = pool_get_executors_count(pool_get());
    if (scope.range <= COUNTING_SORT_MAX_RANGE_I32 || scope.range <= len / n_workers) {
        return parallel_counting_sort_i32(vec, scope.min, scope.range, 0);
    } else {
        if (len < PARALLEL_RADIX_SORT_THRESHOLD)
            return radix16_sort_desc_i32(vec);
        else
            return parallel_radix16_sort_i32(vec, 0);
    }
}

obj_p ray_sort_desc_i64(obj_p vec) {
    i64_t len = vec->len;
    index_scope_t scope = index_scope_i64(AS_I64(vec), NULL, len);

    if (len < SMALL_VEC_THRESHOLD) {
        if (scope.range <= COUNTING_SORT_MAX_RANGE_I64)
            return counting_sort_desc_i64(vec, scope.min, scope.range, scope.null_count);
        else
            return radix8_sort_desc_i64(vec);
    }

    i64_t n_workers = pool_get_executors_count(pool_get());
    if (scope.range <= COUNTING_SORT_MAX_RANGE_I64 || scope.range <= len / n_workers) {
        return parallel_counting_sort_i64(vec, scope.min, scope.range, 0);
    } else {
        if (len < PARALLEL_RADIX_SORT_THRESHOLD)
            return radix16_sort_desc_i64(vec);
        else
            return parallel_radix16_sort_i64(vec, 0);
    }
}

obj_p ray_sort_desc_f64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    f64_t* fv = AS_F64(vec);
    i64_t* t = AS_I64(temp);

    // Allocate on heap to avoid stack overflow on Windows
    obj_p pos1_obj = I64(65537);
    obj_p pos2_obj = I64(65537);
    obj_p pos3_obj = I64(65537);
    obj_p pos4_obj = I64(65537);
    u64_t* pos1 = (u64_t*)AS_I64(pos1_obj);
    u64_t* pos2 = (u64_t*)AS_I64(pos2_obj);
    u64_t* pos3 = (u64_t*)AS_I64(pos3_obj);
    u64_t* pos4 = (u64_t*)AS_I64(pos4_obj);
    memset(pos1, 0, 65537 * sizeof(u64_t));
    memset(pos2, 0, 65537 * sizeof(u64_t));
    memset(pos3, 0, 65537 * sizeof(u64_t));
    memset(pos4, 0, 65537 * sizeof(u64_t));

    // Count occurrences for each 16-bit chunk (descending)
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[i]);
        pos1[u & 0xffff]++;
        pos2[(u >> 16) & 0xffff]++;
        pos3[(u >> 32) & 0xffff]++;
        pos4[u >> 48]++;
    }
    for (i = 65534; i >= 0; i--) {
        pos1[i] += pos1[i + 1];
        pos2[i] += pos2[i + 1];
        pos3[i] += pos3[i + 1];
        pos4[i] += pos4[i + 1];
    }
    // First pass: sort by least significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[i]);
        t[pos1[(u & 0xffff) + 1]++] = i;
    }
    // Second pass: sort by second 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[t[i]]);
        ov[pos2[((u >> 16) & 0xffff) + 1]++] = t[i];
    }
    // Third pass: sort by third 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[ov[i]]);
        t[pos3[((u >> 32) & 0xffff) + 1]++] = ov[i];
    }
    // Fourth pass: sort by most significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = f64_to_sortable_u64(fv[t[i]]);
        ov[pos4[(u >> 48) + 1]++] = t[i];
    }

    drop_obj(pos1_obj);
    drop_obj(pos2_obj);
    drop_obj(pos3_obj);
    drop_obj(pos4_obj);
    drop_obj(temp);
    return indices;
}

obj_p ray_sort_desc(obj_p vec) {
    i64_t len = vec->len;
    obj_p indices;

    if (len == 0) return I64(0);

    if (len == 1) {
        indices = I64(1);
        AS_I64(indices)[0] = 0;
        indices->attrs = ATTR_DESC | ATTR_DISTINCT;
        return indices;
    }

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        indices->attrs = ATTR_ASC | ATTR_DISTINCT;
        iota_ctx_t ctx = {AS_I64(indices), len};
        pool_map(len, iota_asc_worker, &ctx);
        return indices;
    }

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        indices->attrs = ATTR_DESC | ATTR_DISTINCT;
        iota_ctx_t ctx = {AS_I64(indices), len};
        pool_map(len, iota_desc_worker, &ctx);
        return indices;
    }

    switch (vec->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            return ray_sort_desc_u8(vec);
        case TYPE_I16:
            return ray_sort_desc_i16(vec);
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            return ray_sort_desc_i32(vec);
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return ray_sort_desc_i64(vec);
        case TYPE_F64:
            return ray_sort_desc_f64(vec);
        case TYPE_SYMBOL:
            // Use optimized sorting
            return ray_idesc_optimized(vec);
        case TYPE_LIST:
            return mergesort_generic_obj(vec, -1);
        case TYPE_DICT:
            return at_obj(AS_LIST(vec)[0], ray_sort_desc(AS_LIST(vec)[1]));
        default:
            return err_type(0, 0, 0, 0);
    }
}

// Optimized sorting implementations

// Fast binary insertion sort for small arrays with proper symbol comparison
static void binary_insertion_sort_symbols(i64_t* indices, obj_p vec, i64_t len, i64_t asc) {
    for (i64_t i = 1; i < len; i++) {
        i64_t key_idx = indices[i];

        // Binary search for insertion position
        i64_t left = 0, right = i;
        while (left < right) {
            i64_t mid = (left + right) / 2;
            i64_t cmp = compare_symbols(vec, key_idx, indices[mid]);

            if ((asc > 0 && cmp < 0) || (asc <= 0 && cmp > 0)) {
                right = mid;
            } else {
                left = mid + 1;
            }
        }

        // Shift elements and insert
        for (i64_t j = i; j > left; j--) {
            indices[j] = indices[j - 1];
        }
        indices[left] = key_idx;
    }
}

// Fast binary insertion sort for small arrays with numeric comparison
static void binary_insertion_sort_numeric(i64_t* indices, i64_t* data, i64_t len, i64_t asc) {
    for (i64_t i = 1; i < len; i++) {
        i64_t key_idx = indices[i];
        i64_t key_val = data[key_idx];

        // Binary search for insertion position
        i64_t left = 0, right = i;
        while (left < right) {
            i64_t mid = (left + right) / 2;
            i64_t mid_val = data[indices[mid]];

            if ((asc > 0 && key_val < mid_val) || (asc <= 0 && key_val > mid_val)) {
                right = mid;
            } else {
                left = mid + 1;
            }
        }

        // Shift elements and insert
        for (i64_t j = i; j > left; j--) {
            indices[j] = indices[j - 1];
        }
        indices[left] = key_idx;
    }
}

// General counting sort for integer vectors when range is reasonable
static obj_p counting_sort_i64(obj_p vec, i64_t asc) {
    i64_t len = vec->len;
    if (len == 0)
        return I64(0);

    i64_t* data = AS_I64(vec);

    // Find min/max symbol IDs
    i64_t min_sym = data[0], max_sym = data[0];
    for (i64_t i = 1; i < len; i++) {
        if (data[i] < min_sym)
            min_sym = data[i];
        if (data[i] > max_sym)
            max_sym = data[i];
    }

    i64_t range = max_sym - min_sym + 1;

    // Use counting sort only if range is reasonable
    // Don't use if too sparse (range > len) or exceeds maximum range
    if (range > len || range > COUNTING_SORT_MAX_RANGE)
        return NULL;  // Fall back to other sorting

    // Allocate counting array
    i64_t* counts = (i64_t*)heap_alloc(range * sizeof(i64_t));
    if (!counts)
        return NULL;
    memset(counts, 0, range * sizeof(i64_t));

    // Create buckets to store indices for each symbol
    i64_t** buckets = (i64_t**)heap_alloc(range * sizeof(i64_t*));
    if (!buckets) {
        heap_free(counts);
        return NULL;
    }

    // Count occurrences and allocate buckets
    for (i64_t i = 0; i < len; i++)
        counts[data[i] - min_sym]++;

    // Allocate memory for each bucket
    for (i64_t i = 0; i < range; i++)
        buckets[i] = (counts[i] > 0) ? (i64_t*)heap_alloc(counts[i] * sizeof(i64_t)) : NULL;

    // Reset counts to use as bucket indices
    memset(counts, 0, range * sizeof(i64_t));

    // Fill buckets with indices
    for (i64_t i = 0; i < len; i++) {
        i64_t bucket_idx = data[i] - min_sym;
        buckets[bucket_idx][counts[bucket_idx]++] = i;
    }

    // Create result indices array
    obj_p indices = I64(len);
    i64_t* result = AS_I64(indices);
    i64_t pos = 0;

    if (asc > 0) {
        // Ascending: iterate symbols from min to max
        for (i64_t sym = 0; sym < range; sym++) {
            i64_t count = counts[sym];
            for (i64_t i = 0; i < count; i++) {
                result[pos++] = buckets[sym][i];
            }
        }
    } else {
        // Descending: iterate symbols from max to min
        for (i64_t sym = range - 1; sym >= 0; sym--) {
            i64_t count = counts[sym];
            for (i64_t i = 0; i < count; i++) {
                result[pos++] = buckets[sym][i];
            }
        }
    }

    // Cleanup buckets
    for (i64_t i = 0; i < range; i++)
        heap_free(buckets[i]);

    heap_free(buckets);
    heap_free(counts);
    return indices;
}

// Optimized sort dispatcher
static obj_p optimized_sort(obj_p vec, i64_t asc) {
    obj_p res;
    i64_t len = vec->len;

    if (len <= 1)
        return I64(len);

    // Small arrays: use insertion sort
    if (len <= 32) {
        obj_p indices = I64(len);
        i64_t* result = AS_I64(indices);

        // Initialize indices
        for (i64_t i = 0; i < len; i++) {
            result[i] = i;
        }

        switch (vec->type) {
            case TYPE_I64:
            case TYPE_TIME:
                binary_insertion_sort_numeric(result, AS_I64(vec), len, asc);
                return indices;
            case TYPE_SYMBOL:
                // For symbols, use proper symbol comparison
                binary_insertion_sort_symbols(result, vec, len, asc);
                return indices;
        }
    }

    // For larger arrays: try counting sort first for integer types
    switch (vec->type) {
        case TYPE_I64:
        case TYPE_TIME:
        case TYPE_SYMBOL:
            res = counting_sort_i64(vec, asc);
            if (res)
                return res;
            break;
        default:
            break;
    }

    // Fall back to merge sort for larger arrays
    return mergesort_generic_obj(vec, asc);
}

// Optimized sorting functions
static obj_p ray_iasc_optimized(obj_p x) { return optimized_sort(x, 1); }
static obj_p ray_idesc_optimized(obj_p x) { return optimized_sort(x, -1); }

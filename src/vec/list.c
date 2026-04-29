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

#include "list.h"
#include "mem/heap.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Capacity helpers (same pattern as vec.c)
 * -------------------------------------------------------------------------- */

static int64_t list_capacity(ray_t* list) {
    size_t block_size = (size_t)1 << list->order;
    size_t data_space = block_size - 32;  /* 32B ray_t header */
    return (int64_t)(data_space / sizeof(ray_t*));
}

/* --------------------------------------------------------------------------
 * ray_list_new
 * -------------------------------------------------------------------------- */

ray_t* ray_list_new(int64_t capacity) {
    if (capacity < 0) return ray_error("range", NULL);
    if ((uint64_t)capacity > SIZE_MAX / sizeof(ray_t*))
        return ray_error("oom", NULL);
    size_t data_size = (size_t)capacity * sizeof(ray_t*);

    ray_t* list = ray_alloc(data_size);
    if (!list) return ray_error("oom", "list_new(cap=%lld): %zu bytes",
                                (long long)capacity, data_size);
    if (RAY_IS_ERR(list)) return list;

    list->type = RAY_LIST;
    list->len = 0;
    list->attrs = 0;
    memset(list->nullmap, 0, 16);

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_append
 * -------------------------------------------------------------------------- */

ray_t* ray_list_append(ray_t* list, ray_t* item) {
    if (!list || RAY_IS_ERR(list)) return list;

    /* COW if shared */
    ray_t* original = list;
    list = ray_cow(list);
    if (!list || RAY_IS_ERR(list)) return list;

    int64_t cap = list_capacity(list);

    /* Grow if needed */
    if (list->len >= cap) {
        size_t new_data_size = (size_t)(list->len + 1) * sizeof(ray_t*);
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) {
                    if (list != original) ray_release(list);
                    return ray_error("oom", NULL);
                }
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_list = ray_scratch_realloc(list, new_data_size);
        if (!new_list || RAY_IS_ERR(new_list)) {
            if (list != original) ray_release(list);
            return new_list ? new_list : ray_error("oom", NULL);
        }
        list = new_list;
    }

    /* Store item pointer and retain it */
    ray_t** slots = (ray_t**)ray_data(list);
    slots[list->len] = item;
    if (item) ray_retain(item);
    list->len++;

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_get
 * -------------------------------------------------------------------------- */

ray_t* ray_list_get(ray_t* list, int64_t idx) {
    if (!list || RAY_IS_ERR(list)) return NULL;
    if (idx < 0 || idx >= list->len) return NULL;

    ray_t** slots = (ray_t**)ray_data(list);
    return slots[idx];
}

/* --------------------------------------------------------------------------
 * ray_list_set
 * -------------------------------------------------------------------------- */

ray_t* ray_list_set(ray_t* list, int64_t idx, ray_t* item) {
    if (!list || RAY_IS_ERR(list)) return list;
    if (idx < 0 || idx >= list->len)
        return ray_error("range", NULL);

    /* COW if shared */
    list = ray_cow(list);
    if (!list || RAY_IS_ERR(list)) return list;

    ray_t** slots = (ray_t**)ray_data(list);

    /* Release old item */
    ray_t* old = slots[idx];
    if (old) ray_release(old);

    /* Store new item and retain it */
    slots[idx] = item;
    if (item) ray_retain(item);

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_insert_at — insert one item at pre-insertion position idx.
 *
 * idx ∈ [0, list->len]; idx == len is equivalent to ray_list_append.
 * -------------------------------------------------------------------------- */

ray_t* ray_list_insert_at(ray_t* list, int64_t idx, ray_t* item) {
    if (!list || RAY_IS_ERR(list)) return list;
    if (list->type != RAY_LIST) return ray_error("type", NULL);
    if (idx < 0 || idx > list->len) return ray_error("range", NULL);

    ray_t* original = list;
    list = ray_cow(list);
    if (!list || RAY_IS_ERR(list)) return list;

    int64_t cap = list_capacity(list);

    if (list->len >= cap) {
        size_t new_data_size = (size_t)(list->len + 1) * sizeof(ray_t*);
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) {
                    if (list != original) ray_release(list);
                    return ray_error("oom", NULL);
                }
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_list = ray_scratch_realloc(list, new_data_size);
        if (!new_list || RAY_IS_ERR(new_list)) {
            if (list != original) ray_release(list);
            return new_list ? new_list : ray_error("oom", NULL);
        }
        list = new_list;
    }

    ray_t** slots = (ray_t**)ray_data(list);

    if (idx < list->len) {
        memmove(&slots[idx + 1], &slots[idx],
                (size_t)(list->len - idx) * sizeof(ray_t*));
    }

    slots[idx] = item;
    if (item) ray_retain(item);
    list->len++;

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_insert_many — insert N items at N pre-insertion positions.
 *
 * idxs : RAY_I64 vec of length N, each idx in [0, list->len].
 * vals : RAY_LIST. Length 1 broadcasts the single ptr; length N is parallel.
 *
 * Stable on duplicate indices. Returns a fresh block; broadcast retains the
 * same pointer once per insertion site.
 * -------------------------------------------------------------------------- */

ray_t* ray_list_insert_many(ray_t* list, ray_t* idxs, ray_t* vals) {
    if (!list || RAY_IS_ERR(list)) return list;
    if (!idxs || RAY_IS_ERR(idxs)) return idxs;
    if (!vals || RAY_IS_ERR(vals)) return vals;
    if (list->type != RAY_LIST) return ray_error("type", NULL);
    if (idxs->type != RAY_I64) return ray_error("type", NULL);
    if (vals->type != RAY_LIST) return ray_error("type", NULL);

    int64_t N = idxs->len;
    int64_t old_len = list->len;

    if (N == 0) { ray_retain(list); return list; }

    const int64_t* idx_arr = (const int64_t*)ray_data(idxs);
    for (int64_t k = 0; k < N; k++) {
        if (idx_arr[k] < 0 || idx_arr[k] > old_len)
            return ray_error("range", NULL);
    }

    int broadcast;
    if (vals->len == 1) broadcast = 1;
    else if (vals->len == N) broadcast = 0;
    else return ray_error("range", NULL);

    /* Sort buffer of (idx, src_pos) pairs */
    ray_t* pair_vec = ray_vec_new(RAY_I64, 2 * N);
    if (!pair_vec || RAY_IS_ERR(pair_vec)) return ray_error("oom", NULL);
    pair_vec->len = 2 * N;
    int64_t* pairs = (int64_t*)ray_data(pair_vec);
    for (int64_t k = 0; k < N; k++) {
        pairs[2 * k]     = idx_arr[k];
        pairs[2 * k + 1] = k;
    }

    /* Stable insertion sort by idx */
    for (int64_t i = 1; i < N; i++) {
        int64_t ki = pairs[2 * i];
        int64_t ks = pairs[2 * i + 1];
        int64_t j = i - 1;
        while (j >= 0 && pairs[2 * j] > ki) {
            pairs[2 * (j + 1)]     = pairs[2 * j];
            pairs[2 * (j + 1) + 1] = pairs[2 * j + 1];
            j--;
        }
        pairs[2 * (j + 1)]     = ki;
        pairs[2 * (j + 1) + 1] = ks;
    }

    int64_t new_len = old_len + N;
    if (new_len < old_len) { ray_release(pair_vec); return ray_error("oom", NULL); }
    if ((uint64_t)new_len > SIZE_MAX / sizeof(ray_t*)) {
        ray_release(pair_vec);
        return ray_error("oom", NULL);
    }
    size_t data_size = (size_t)new_len * sizeof(ray_t*);

    ray_t* result = ray_alloc(data_size);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(pair_vec);
        return result ? result : ray_error("oom", NULL);
    }
    result->type = RAY_LIST;
    result->len = new_len;
    result->attrs = 0;
    memset(result->nullmap, 0, 16);

    ray_t** src_slots = (ray_t**)ray_data(list);
    ray_t** val_slots = (ray_t**)ray_data(vals);
    ray_t** dst_slots = (ray_t**)ray_data(result);

    int64_t w = 0;
    int64_t p = 0;
    for (int64_t r = 0; r <= old_len; r++) {
        while (p < N && pairs[2 * p] == r) {
            int64_t src_pos = pairs[2 * p + 1];
            ray_t* item = broadcast ? val_slots[0] : val_slots[src_pos];
            dst_slots[w] = item;
            if (item) ray_retain(item);
            w++;
            p++;
        }
        if (r < old_len) {
            ray_t* item = src_slots[r];
            dst_slots[w] = item;
            if (item) ray_retain(item);
            w++;
        }
    }

    ray_release(pair_vec);
    return result;
}

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

// Maximum range for counting sort - configurable constant
#define COUNTING_SORT_MAX_RANGE 1000000

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

// insertion sort
nil_t insertion_sort_asc(i64_t array[], i64_t indices[], i64_t left, i64_t right) {
    for (i64_t i = left + 1; i <= right; i++) {
        i64_t temp = indices[i];
        i64_t j = i - 1;
        while (j >= left && array[indices[j]] > array[temp]) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = temp;
    }
}

nil_t insertion_sort_desc(i64_t array[], i64_t indices[], i64_t left, i64_t right) {
    for (i64_t i = left + 1; i <= right; i++) {
        i64_t temp = indices[i];
        i64_t j = i - 1;
        while (j >= left && array[indices[j]] < array[temp]) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = temp;
    }
}
//

obj_p ray_sort_asc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    u8_t* iv = AS_U8(vec);

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
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    i16_t* iv = AS_I16(vec);

    u64_t pos[65537] = {0};

    for (i = 0; i < len; i++) {
        pos[iv[i] + 32769]++;
    }
    for (i = 2; i <= 65536; i++) {
        pos[i] += pos[i - 1];
    }

    for (i = 0; i < len; i++) {
        ov[pos[iv[i] + 32768]++] = i;
    }

    return indices;
}

obj_p ray_sort_asc_i32(obj_p vec) {
    i64_t i, t, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i32_t* iv = AS_I32(vec);
    i64_t* ti = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i] ^ 0x80000000;
        pos1[(t & 0xffff) + 1]++;
        pos2[(t >> 16) + 1]++;
    }
    for (i = 2; i <= 65536; i++) {
        pos1[i] += pos1[i - 1];
        pos2[i] += pos2[i - 1];
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i];
        ti[pos1[iv[i] & 0xffff]++] = i;
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[ti[i]] ^ 0x80000000;
        ov[pos2[t >> 16]++] = ti[i];
    }

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
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i64_t* iv = AS_I64(vec);
    i64_t* t = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};
    u64_t pos3[65537] = {0};
    u64_t pos4[65537] = {0};

    // Count occurrences of each 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
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
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        t[pos1[u & 0xffff]++] = i;
    }

    // Second pass: sort by second 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos2[(u >> 16) & 0xffff]++] = t[i];
    }

    // Third pass: sort by third 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[ov[i]] ^ 0x8000000000000000ULL;
        t[pos3[(u >> 32) & 0xffff]++] = ov[i];
    }

    // Fourth pass: sort by most significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos4[u >> 48]++] = t[i];
    }

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_asc_f64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    f64_t* fv = AS_F64(vec);
    i64_t* t = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};
    u64_t pos3[65537] = {0};
    u64_t pos4[65537] = {0};

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

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_asc(obj_p vec) {
    i64_t len = vec->len;
    obj_p indices;

    if (len == 0)
        return I64(0);

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        indices->attrs = ATTR_ASC | ATTR_DISTINCT;
        iota_ctx_t ctx = { AS_I64(indices), len };
        pool_map(len, iota_asc_worker, &ctx);
        return indices;
    }

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        indices->attrs = ATTR_DESC | ATTR_DISTINCT;
        iota_ctx_t ctx = { AS_I64(indices), len };
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
            THROW(ERR_TYPE, "sort: unsupported type: '%s", type_name(vec->type));
    }
}

obj_p ray_sort_desc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    u8_t* iv = AS_U8(vec);

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
    obj_p indices = I64(len);
    i64_t* ov = AS_I64(indices);
    i16_t* iv = AS_I16(vec);

    u64_t pos[65537] = {0};

    for (i = 0; i < len; i++)
        pos[(u16_t)(iv[i] + 32768)]++;

    for (i = 65534; i >= 0; i--)
        pos[i] += pos[i + 1];

    for (i = 0; i < len; i++)
        ov[pos[(u64_t)(iv[i] + 32769)]++] = i;

    return indices;
}

obj_p ray_sort_desc_i32(obj_p vec) {
    i64_t i, t, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i32_t* iv = AS_I32(vec);
    i64_t* ti = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};

    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i] ^ 0x80000000;
        pos1[t & 0xffff]++;
        pos2[t >> 16]++;
    }
    for (i = 65534; i >= 0; i--) {
        pos1[i] += pos1[i + 1];
        pos2[i] += pos2[i + 1];
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[i];
        ti[pos1[(t & 0xffff) + 1]++] = i;
    }
    for (i = 0; i < len; i++) {
        t = (u32_t)iv[ti[i]] ^ 0x80000000;
        ov[pos2[(t >> 16) + 1]++] = ti[i];
    }

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_desc_i64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    i64_t* iv = AS_I64(vec);
    i64_t* t = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};
    u64_t pos3[65537] = {0};
    u64_t pos4[65537] = {0};

    // Count occurrences of each 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[i] ^ 0x8000000000000000ULL;  // invert bits for descending order
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
        u64_t u = iv[i] ^ 0x8000000000000000ULL;
        t[pos1[(u & 0xffff) + 1]++] = i;
    }
    // Second pass: sort by second 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos2[((u >> 16) & 0xffff) + 1]++] = t[i];
    }
    // Third pass: sort by third 16-bit chunk
    for (i = 0; i < len; i++) {
        u64_t u = iv[ov[i]] ^ 0x8000000000000000ULL;
        t[pos3[((u >> 32) & 0xffff) + 1]++] = ov[i];
    }
    // Fourth pass: sort by most significant 16 bits
    for (i = 0; i < len; i++) {
        u64_t u = iv[t[i]] ^ 0x8000000000000000ULL;
        ov[pos4[(u >> 48) + 1]++] = t[i];
    }

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_desc_f64(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    obj_p temp = I64(len);
    i64_t* ov = AS_I64(indices);
    f64_t* fv = AS_F64(vec);
    i64_t* t = AS_I64(temp);

    u64_t pos1[65537] = {0};
    u64_t pos2[65537] = {0};
    u64_t pos3[65537] = {0};
    u64_t pos4[65537] = {0};

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

    drop_obj(temp);
    return indices;
}

obj_p ray_sort_desc(obj_p vec) {
    i64_t len = vec->len;
    obj_p indices;

    if (len == 0)
        return I64(0);

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        indices->attrs = ATTR_ASC | ATTR_DISTINCT;
        iota_ctx_t ctx = { AS_I64(indices), len };
        pool_map(len, iota_asc_worker, &ctx);
        return indices;
    }

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        indices->attrs = ATTR_DESC | ATTR_DISTINCT;
        iota_ctx_t ctx = { AS_I64(indices), len };
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
            THROW(ERR_TYPE, "sort: unsupported type: '%s", type_name(vec->type));
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

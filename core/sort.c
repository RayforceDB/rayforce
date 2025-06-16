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
#include "util.h"
#include "string.h"
#include "ops.h"
#include "error.h"

#define COUNTING_SORT_LIMIT 1024 * 1024

inline __attribute__((always_inline)) nil_t swap(i64_t *a, i64_t *b) {
    i64_t t = *a;
    *a = *b;
    *b = t;
}

// quick sort
// https://www.geeksforgeeks.org/quick-sort/
i64_t partition_asc(i64_t array[], i64_t indices[], i64_t low, i64_t high) {
    i64_t pivot = array[indices[high]];
    i64_t i = (low - 1);

    for (i64_t j = low; j <= high - 1; j++) {
        if (array[indices[j]] < pivot) {
            i++;
            swap(&indices[i], &indices[j]);
        }
    }
    swap(&indices[i + 1], &indices[high]);

    return i + 1;
}

i64_t partition_desc(i64_t array[], i64_t indices[], i64_t low, i64_t high) {
    i64_t pivot = array[indices[high]];
    i64_t i = (low - 1);

    for (i64_t j = low; j <= high - 1; j++) {
        if (array[indices[j]] > pivot) {
            i++;
            swap(&indices[i], &indices[j]);
        }
    }
    swap(&indices[i + 1], &indices[high]);

    return i + 1;
}

nil_t quick_sort_asc(i64_t array[], i64_t indices[], i64_t low, i64_t high) {
    i64_t pi;

    if (low < high) {
        pi = partition_asc(array, indices, low, high);
        quick_sort_asc(array, indices, low, pi - 1);
        quick_sort_asc(array, indices, pi + 1, high);
    }
}

nil_t quick_sort_desc(i64_t array[], i64_t indices[], i64_t low, i64_t high) {
    i64_t pi;

    if (low < high) {
        pi = partition_desc(array, indices, low, high);
        quick_sort_desc(array, indices, low, pi - 1);
        quick_sort_desc(array, indices, pi + 1, high);
    }
}

// heap sort
nil_t heapify_asc(i64_t array[], i64_t indices[], i64_t n, i64_t i) {
    i64_t largest = i;
    i64_t left = 2 * i + 1;
    i64_t right = 2 * i + 2;

    if (left < n && array[indices[left]] > array[indices[largest]])
        largest = left;

    if (right < n && array[indices[right]] > array[indices[largest]])
        largest = right;

    if (largest != i) {
        swap(&indices[i], &indices[largest]);
        heapify_asc(array, indices, n, largest);
    }
}

nil_t heapify_desc(i64_t array[], i64_t indices[], i64_t n, i64_t i) {
    i64_t largest = i;
    i64_t left = 2 * i + 1;
    i64_t right = 2 * i + 2;

    if (left < n && array[indices[left]] < array[indices[largest]])
        largest = left;

    if (right < n && array[indices[right]] < array[indices[largest]])
        largest = right;

    if (largest != i) {
        swap(&indices[i], &indices[largest]);
        heapify_desc(array, indices, n, largest);
    }
}

nil_t heap_sort_asc(i64_t array[], i64_t indices[], i64_t n) {
    for (i64_t i = n / 2 - 1; i >= 0; i--)
        heapify_asc(array, indices, n, i);

    for (i64_t i = n - 1; i >= 0; i--) {
        swap(&indices[0], &indices[i]);
        heapify_asc(array, indices, i, 0);
    }
}

nil_t heap_sort_desc(i64_t array[], i64_t indices[], i64_t n) {
    for (i64_t i = n / 2 - 1; i >= 0; i--)
        heapify_asc(array, indices, n, i);

    for (i64_t i = n - 1; i >= 0; i--) {
        swap(&indices[0], &indices[i]);
        heapify_desc(array, indices, i, 0);
    }
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

// counting sort
nil_t counting_sort_asc(i64_t array[], i64_t indices[], i64_t len, i64_t min, i64_t max) {
    i64_t i, j = 0, n, p, range, *m;
    obj_p mask;

    range = max - min + 1;
    mask = I64(range);
    m = AS_I64(mask);

    memset(m, 0, range * sizeof(i64_t));

    for (i = 0; i < len; i++) {
        n = array[i] - min;
        m[n]++;
    }

    for (i = 0, j = 0; i < range; i++) {
        if (m[i] > 0) {
            p = j;
            j += m[i];
            m[i] = p;
        }
    }

    for (i = 0; i < len; i++) {
        n = array[i] - min;
        indices[m[n]++] = i;
    }

    drop_obj(mask);
}

nil_t counting_sort_desc(i64_t array[], i64_t indices[], i64_t len, i64_t min, i64_t max) {
    i64_t i, j = 0, n, p, range, *m;
    obj_p mask;

    range = max - min + 1;
    mask = I64(range);
    m = AS_I64(mask);

    memset(m, 0, range * sizeof(i64_t));

    for (i = 0; i < len; i++) {
        n = array[i] - min;
        m[n]++;
    }

    for (i = range - 1, j = 0; i >= 0; i--) {
        if (m[i] > 0) {
            p = j;
            j += m[i];
            m[i] = p;
        }
    }

    for (i = 0; i < len; i++) {
        n = array[i] - min;
        indices[m[n]++] = i;
    }

    drop_obj(mask);
}

obj_p ray_sort_asc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    i64_t *ov = AS_I64(indices);
    u8_t *iv = AS_U8(vec);

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
    i64_t *ov = AS_I64(indices);
    i16_t *iv = AS_I16(vec);

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
    i64_t *ov = AS_I64(indices);
    i32_t *iv = AS_I32(vec);
    i64_t *ti = AS_I64(temp);

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

// TODO need fix
obj_p ray_sort_asc_i64_(obj_p vec) {
    i64_t i, len = vec->len, out_of_order = 0, inrange = 0, min, max;
    obj_p indices = I64(len);
    i64_t *iv = AS_I64(vec), *ov = AS_I64(indices);

    max = min = iv[0];

    for (i = 0; i < len; i++) {
        if (iv[i] < min)
            min = iv[i];
        else if (iv[i] > max)
            max = iv[i];

        if ((iv[i] - min) < COUNTING_SORT_LIMIT)
            inrange++;

        if (i > 0 && iv[i] < iv[i - 1])
            out_of_order++;
    }

    // ascending order
    if (out_of_order == 0) {
        vec->attrs |= ATTR_ASC;
        for (i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    // descending order
    if (out_of_order == len - 1) {
        vec->attrs |= ATTR_DESC;
        for (i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    if (inrange == len) {
        counting_sort_asc(iv, ov, len, min, max);
        return indices;
    }

    // fill with indexes
    for (i = 0; i < len; i++)
        ov[i] = i;

    if (len < 100) {
        insertion_sort_asc(iv, ov, 0, len - 1);
        return indices;
    }

    // mainly ordered
    if (out_of_order < len / 3) {
        heap_sort_asc(iv, ov, len);
        return indices;
    }

    quick_sort_asc(iv, ov, 0, len - 1);
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
    i64_t *ov = AS_I64(indices);
    i64_t *iv = AS_I64(vec);
    i64_t *t = AS_I64(temp);

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
    i64_t *ov = AS_I64(indices);
    f64_t *fv = AS_F64(vec);
    i64_t *t = AS_I64(temp);

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
    i64_t i, len = vec->len;
    obj_p indices;
    i64_t *ov;

    if (vec->len == 0)
        return I64(0);

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        ov = AS_I64(indices);
        for (i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        ov = AS_I64(indices);
        for (i = 0; i < len; i++)
            ov[i] = len - i - 1;
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
        default:
            THROW(ERR_TYPE, "sort: unsupported type: '%s", type_name(vec->type));
    }
}

obj_p ray_sort_desc_u8(obj_p vec) {
    i64_t i, len = vec->len;
    obj_p indices = I64(len);
    i64_t *ov = AS_I64(indices);
    u8_t *iv = AS_U8(vec);

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
    i64_t *ov = AS_I64(indices);
    i16_t *iv = AS_I16(vec);

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
    i64_t *ov = AS_I64(indices);
    i32_t *iv = AS_I32(vec);
    i64_t *ti = AS_I64(temp);

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
    i64_t *ov = AS_I64(indices);
    i64_t *iv = AS_I64(vec);
    i64_t *t = AS_I64(temp);

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
    i64_t *ov = AS_I64(indices);
    f64_t *fv = AS_F64(vec);
    i64_t *t = AS_I64(temp);

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
    i64_t i, len = vec->len;
    obj_p indices;
    i64_t *ov;

    if (vec->len == 0)
        return I64(0);

    if (vec->attrs & ATTR_DESC) {
        indices = I64(len);
        ov = AS_I64(indices);
        for (i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    if (vec->attrs & ATTR_ASC) {
        indices = I64(len);
        ov = AS_I64(indices);
        for (i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    switch (vec->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8: {
            return ray_sort_desc_u8(vec);
        };
        case TYPE_I16: {
            return ray_sort_desc_i16(vec);
        };
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME: {
            return ray_sort_desc_i32(vec);
        };
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            return ray_sort_desc_i64(vec);
        case TYPE_F64: {
            return ray_sort_desc_f64(vec);
        };
        default:
            THROW(ERR_TYPE, "sort: unsupported type: '%s", type_name(vec->type));
    }
}

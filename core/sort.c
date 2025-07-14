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
#include "symbols.h"
#include <stdlib.h>

#define COUNTING_SORT_LIMIT 1024 * 1024

inline __attribute__((always_inline)) nil_t swap(i64_t *a, i64_t *b) {
    i64_t t = *a;
    *a = *b;
    *b = t;
}

static obj_p timsort_generic_obj(obj_p *data, i64_t len, i64_t asc) {
    if (len == 0)
        return I64(0);

    obj_p indices = I64(len);
    i64_t *ov = AS_I64(indices);

    // Initialize indices
    for (i64_t i = 0; i < len; i++) {
        ov[i] = i;
    }

#define MIN_MERGE 32

    // For small arrays, use insertion sort
    if (len < MIN_MERGE) {
        for (i64_t i = 1; i < len; i++) {
            i64_t key = ov[i];
            i64_t j = i - 1;
            while (j >= 0 && asc * cmp_obj(data[ov[j]], data[key]) > 0) {
                ov[j + 1] = ov[j];
                j--;
            }
            ov[j + 1] = key;
        }
        return indices;
    }

    // TimSort: Find natural runs and merge them
    i64_t stack_size = 0;
    i64_t runs[64][2];  // [start, length] pairs

    // Find runs and merge them
    i64_t pos = 0;
    while (pos < len) {
        i64_t run_start = pos;
        i64_t run_len = 1;

        // Find natural run
        if (pos + 1 < len) {
            i64_t cmp = asc * cmp_obj(data[ov[pos]], data[ov[pos + 1]]);
            if (cmp <= 0) {
                // Ascending run
                while (pos + run_len < len &&
                       asc * cmp_obj(data[ov[pos + run_len - 1]], data[ov[pos + run_len]]) <= 0) {
                    run_len++;
                }
            } else {
                // Descending run - reverse it
                while (pos + run_len < len && asc * cmp_obj(data[ov[pos + run_len - 1]], data[ov[pos + run_len]]) > 0) {
                    run_len++;
                }
                // Reverse the descending run
                for (i64_t i = 0; i < run_len / 2; i++) {
                    i64_t temp = ov[run_start + i];
                    ov[run_start + i] = ov[run_start + run_len - 1 - i];
                    ov[run_start + run_len - 1 - i] = temp;
                }
            }
        }

        // Extend short runs with insertion sort
        if (run_len < MIN_MERGE) {
            i64_t force_len = (pos + MIN_MERGE <= len) ? MIN_MERGE : (len - pos);
            for (i64_t i = run_start + run_len; i < run_start + force_len; i++) {
                i64_t key = ov[i];
                i64_t j = i - 1;
                while (j >= run_start && asc * cmp_obj(data[ov[j]], data[key]) > 0) {
                    ov[j + 1] = ov[j];
                    j--;
                }
                ov[j + 1] = key;
            }
            run_len = force_len;
        }

        // Add run to stack
        runs[stack_size][0] = run_start;
        runs[stack_size][1] = run_len;
        stack_size++;

        // Merge runs to maintain stack invariant
        while (stack_size > 1) {
            i64_t n = stack_size - 1;
            if ((n >= 2 && runs[n - 2][1] <= runs[n - 1][1] + runs[n][1]) ||
                (n >= 3 && runs[n - 3][1] <= runs[n - 2][1] + runs[n - 1][1])) {
                // Determine which runs to merge
                i64_t merge_at = (n >= 2 && runs[n - 2][1] < runs[n][1]) ? n - 2 : n - 1;

                // Merge two adjacent runs
                i64_t start1 = runs[merge_at][0];
                i64_t len1 = runs[merge_at][1];
                i64_t start2 = runs[merge_at + 1][0];
                i64_t len2 = runs[merge_at + 1][1];

                i64_t *temp = malloc(sizeof(i64_t) * (len1 + len2));
                i64_t i = 0, j = 0, k = 0;

                while (i < len1 && j < len2) {
                    if (asc * cmp_obj(data[ov[start1 + i]], data[ov[start2 + j]]) <= 0) {
                        temp[k++] = ov[start1 + i++];
                    } else {
                        temp[k++] = ov[start2 + j++];
                    }
                }

                while (i < len1)
                    temp[k++] = ov[start1 + i++];
                while (j < len2)
                    temp[k++] = ov[start2 + j++];

                for (i = 0; i < len1 + len2; i++) {
                    ov[start1 + i] = temp[i];
                }

                free(temp);

                // Update runs stack
                runs[merge_at][1] = len1 + len2;
                for (i64_t i = merge_at + 1; i < stack_size - 1; i++) {
                    runs[i][0] = runs[i + 1][0];
                    runs[i][1] = runs[i + 1][1];
                }
                stack_size--;
            } else {
                break;
            }
        }

        pos += run_len;
    }

    // Merge remaining runs
    while (stack_size > 1) {
        i64_t n = stack_size - 1;

        // Merge last two runs
        i64_t start1 = runs[n - 1][0];
        i64_t len1 = runs[n - 1][1];
        i64_t start2 = runs[n][0];
        i64_t len2 = runs[n][1];

        i64_t *temp = malloc(sizeof(i64_t) * (len1 + len2));
        i64_t i = 0, j = 0, k = 0;

        while (i < len1 && j < len2) {
            if (asc * cmp_obj(data[ov[start1 + i]], data[ov[start2 + j]]) <= 0) {
                temp[k++] = ov[start1 + i++];
            } else {
                temp[k++] = ov[start2 + j++];
            }
        }

        while (i < len1)
            temp[k++] = ov[start1 + i++];
        while (j < len2)
            temp[k++] = ov[start2 + j++];

        for (i = 0; i < len1 + len2; i++) {
            ov[start1 + i] = temp[i];
        }

        free(temp);

        runs[n - 1][1] = len1 + len2;
        stack_size--;
    }

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
        case TYPE_SYMBOL:
            return timsort_generic_obj(&vec, vec->len, 1);
        case TYPE_LIST:
            return timsort_generic_obj(AS_LIST(vec), vec->len, 1);
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
            return timsort_generic_obj(&vec, vec->len, -1);
        case TYPE_LIST:
            return timsort_generic_obj(AS_LIST(vec), vec->len, -1);
        default:
            THROW(ERR_TYPE, "sort: unsupported type: '%s", type_name(vec->type));
    }
}

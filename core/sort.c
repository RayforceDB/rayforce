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
#include "vector.h"

#define COUNTING_SORT_LIMIT 1024 * 1024

inline __attribute__((always_inline)) null_t swap(i64_t *a, i64_t *b)
{
    i64_t t = *a;
    *a = *b;
    *b = t;
}

// quick sort
// https://www.geeksforgeeks.org/quick-sort/
i64_t partition_asc(i64_t array[], i64_t indices[], i64_t low, i64_t high)
{
    i64_t pivot = array[indices[high]];
    i64_t i = (low - 1);

    for (i64_t j = low; j <= high - 1; j++)
    {
        if (array[indices[j]] < pivot)
        {
            i++;
            swap(&indices[i], &indices[j]);
        }
    }
    swap(&indices[i + 1], &indices[high]);

    return i + 1;
}

i64_t partition_desc(i64_t array[], i64_t indices[], i64_t low, i64_t high)
{
    i64_t pivot = array[indices[high]];
    i64_t i = (low - 1);

    for (i64_t j = low; j <= high - 1; j++)
    {
        if (array[indices[j]] > pivot)
        {
            i++;
            swap(&indices[i], &indices[j]);
        }
    }
    swap(&indices[i + 1], &indices[high]);

    return i + 1;
}

null_t quick_sort_asc(i64_t array[], i64_t indices[], i64_t low, i64_t high)
{
    i64_t pi;

    if (low < high)
    {
        pi = partition_asc(array, indices, low, high);
        quick_sort_asc(array, indices, low, pi - 1);
        quick_sort_asc(array, indices, pi + 1, high);
    }
}

null_t quick_sort_desc(i64_t array[], i64_t indices[], i64_t low, i64_t high)
{
    i64_t pi;

    if (low < high)
    {
        pi = partition_desc(array, indices, low, high);
        quick_sort_desc(array, indices, low, pi - 1);
        quick_sort_desc(array, indices, pi + 1, high);
    }
}

// heap sort
null_t heapify_asc(i64_t array[], i64_t indices[], i64_t n, i64_t i)
{
    i64_t largest = i;
    i64_t left = 2 * i + 1;
    i64_t right = 2 * i + 2;

    if (left < n && array[indices[left]] > array[indices[largest]])
        largest = left;

    if (right < n && array[indices[right]] > array[indices[largest]])
        largest = right;

    if (largest != i)
    {
        swap(&indices[i], &indices[largest]);
        heapify_asc(array, indices, n, largest);
    }
}

null_t heapify_desc(i64_t array[], i64_t indices[], i64_t n, i64_t i)
{
    i64_t largest = i;
    i64_t left = 2 * i + 1;
    i64_t right = 2 * i + 2;

    if (left < n && array[indices[left]] < array[indices[largest]])
        largest = left;

    if (right < n && array[indices[right]] < array[indices[largest]])
        largest = right;

    if (largest != i)
    {
        swap(&indices[i], &indices[largest]);
        heapify_desc(array, indices, n, largest);
    }
}

null_t heap_sort_asc(i64_t array[], i64_t indices[], i64_t n)
{
    for (i64_t i = n / 2 - 1; i >= 0; i--)
        heapify_asc(array, indices, n, i);

    for (i64_t i = n - 1; i >= 0; i--)
    {
        swap(&indices[0], &indices[i]);
        heapify_asc(array, indices, i, 0);
    }
}

null_t heap_sort_desc(i64_t array[], i64_t indices[], i64_t n)
{
    for (i64_t i = n / 2 - 1; i >= 0; i--)
        heapify_asc(array, indices, n, i);

    for (i64_t i = n - 1; i >= 0; i--)
    {
        swap(&indices[0], &indices[i]);
        heapify_desc(array, indices, i, 0);
    }
}

// insertion sort
null_t insertion_sort_asc(i64_t array[], i64_t indices[], i64_t left, i64_t right)
{
    for (i64_t i = left + 1; i <= right; i++)
    {
        i64_t temp = indices[i];
        i64_t j = i - 1;
        while (j >= left && array[indices[j]] > array[temp])
        {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = temp;
    }
}

null_t insertion_sort_desc(i64_t array[], i64_t indices[], i64_t left, i64_t right)
{
    for (i64_t i = left + 1; i <= right; i++)
    {
        i64_t temp = indices[i];
        i64_t j = i - 1;
        while (j >= left && array[indices[j]] < array[temp])
        {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = temp;
    }
}
//

// counting sort
null_t counting_sort_asc(i64_t array[], i64_t indices[], i64_t len, i64_t min, i64_t max)
{
    i64_t i, j = 0, n, p, range, *m;
    rf_object_t mask;

    range = max - min + 1;
    mask = vector_i64(range);
    m = as_vector_i64(&mask);

    memset(m, 0, range * sizeof(i64_t));

    for (i = 0; i < len; i++)
    {
        n = array[i] - min;
        m[n]++;
    }

    j = 0;
    p = 0;
    for (i = 0; i < range; i++)
    {
        if (m[i] > 0)
        {
            p = j;
            j += m[i];
            m[i] = p;
        }
    }

    for (i = 0; i < len; i++)
    {
        n = array[i] - min;
        indices[m[n]++] = i;
    }

    rf_object_free(&mask);
}

null_t counting_sort_desc(i64_t array[], i64_t indices[], i64_t len, i64_t min, i64_t max)
{
    i64_t i, j = 0, n, p, range, *m;
    rf_object_t mask;

    range = max - min + 1;
    mask = vector_i64(range);
    m = as_vector_i64(&mask);

    memset(m, 0, range * sizeof(i64_t));

    for (i = 0; i < len; i++)
    {
        n = array[i] - min;
        m[n]++;
    }

    j = 0;
    p = 0;
    for (i = range - 1; i >= 0; i--)
    {
        if (m[i] > 0)
        {
            p = j;
            j += m[i];
            m[i] = p;
        }
    }

    for (i = 0; i < len; i++)
    {
        n = array[i] - min;
        indices[m[n]++] = i;
    }

    rf_object_free(&mask);
}

rf_object_t rf_sort_asc(rf_object_t *vec)
{
    i64_t i, len = vec->adt->len, out_of_order = 0, inrange = 0, min, max;
    rf_object_t indices = vector_i64(len);
    i64_t *iv = as_vector_i64(vec), *ov = as_vector_i64(&indices);

    if (vec->adt->attrs & VEC_ATTR_ASC)
    {
        for (i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    if (vec->adt->attrs & VEC_ATTR_DESC)
    {
        for (i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    max = min = iv[0];

    for (i = 0; i < len; i++)
    {
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
    if (out_of_order == 0)
    {
        vec->adt->attrs |= VEC_ATTR_ASC;
        for (i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    // descending order
    if (out_of_order == len - 1)
    {
        vec->adt->attrs |= VEC_ATTR_DESC;
        for (i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    if (inrange == len)
    {
        counting_sort_asc(iv, ov, len, min, max);
        return indices;
    }

    // fill with indexes
    for (i = 0; i < len; i++)
        ov[i] = i;

    if (len < 100)
    {
        insertion_sort_asc(iv, ov, 0, len - 1);
        return indices;
    }

    // mainly ordered
    if (out_of_order < len / 3)
    {
        heap_sort_asc(iv, ov, len);
        return indices;
    }

    quick_sort_asc(iv, ov, 0, len - 1);
    return indices;
}

rf_object_t rf_sort_desc(rf_object_t *vec)
{
    i64_t i, len = vec->adt->len, out_of_order = 0, inrange = 0, min, max;
    rf_object_t indices = vector_i64(len);
    i64_t *iv = as_vector_i64(vec), *ov = as_vector_i64(&indices);

    if (vec->adt->attrs & VEC_ATTR_DESC)
    {
        for (i64_t i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    if (vec->adt->attrs & VEC_ATTR_ASC)
    {
        for (i64_t i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    max = min = iv[0];

    for (i = 0; i < len; i++)
    {
        if (iv[i] < min)
            min = iv[i];
        else if (iv[i] > max)
            max = iv[i];

        if ((iv[i] - min) < COUNTING_SORT_LIMIT)
            inrange++;

        if (i > 0 && iv[i] > iv[i - 1])
            out_of_order++;
    }

    // descending order
    if (out_of_order == 0)
    {
        vec->adt->attrs |= VEC_ATTR_DESC;
        for (i64_t i = 0; i < len; i++)
            ov[i] = i;
        return indices;
    }

    // ascending order
    if (out_of_order == len - 1)
    {
        vec->adt->attrs |= VEC_ATTR_ASC;
        for (i64_t i = 0; i < len; i++)
            ov[i] = len - i - 1;
        return indices;
    }

    if (inrange == len)
    {
        counting_sort_desc(iv, ov, len, min, max);
        return indices;
    }

    // fill with indexes
    for (i = 0; i < len; i++)
        ov[i] = i;

    if (len < 100)
    {
        insertion_sort_desc(iv, ov, 0, len - 1);
        return indices;
    }

    // mainly ordered
    if (out_of_order < len / 3)
    {
        heap_sort_desc(iv, ov, len);
        return indices;
    }

    quick_sort_desc(iv, ov, 0, len - 1);
    return indices;
}

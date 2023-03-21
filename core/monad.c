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

#include "rayforce.h"
#include "alloc.h"
#include "vm.h"

// rf_object_t til(i64_t count)
// {
//     i64_t *vec;

//     vec = (i64_t *)rayforce_malloc(count * sizeof(i64_t));
//     for (i64_t i = 0; i < count; i++)
//     {
//         vec[i] = i;
//     }
//     return vector_i64(vec, count);
// }

// rf_object_t rayforce_add(rf_object_t *a, rf_object_t *b)
// {
//     i64_t a_len, b_len, sum = 0;
//     i64_t *a_vec;

//     a_len = a->adt.len;
//     a_vec = a->adt.ptr;

//     sum = b->i64;

//     for (i64_t i = 0; i < a_len; i++)
//     {
//         sum += a_vec[i];
//     }

//     return i64(sum);
// }

i8_t *compile(rf_object_t *object)
{
    i8_t *code;

    UNUSED(object);

    code = (i8_t *)rayforce_malloc(1024);

    code[0] = VM_ADD;

    return code;
}

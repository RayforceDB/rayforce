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

#ifndef MPMC_H
#define MPMC_H

#include "rayforce.h"

#define CACHELINE_SIZE 64

typedef c8_t cachepad_t[CACHELINE_SIZE];
typedef obj_p (*task_fn)(raw_p, u64_t);

typedef struct
{
    task_fn fn;
    raw_p arg;
    u64_t len;
} mpmc_data_in_t;

typedef struct
{
    raw_p arg;
    u64_t len;
    obj_p result;
} mpmc_data_out_t;

typedef struct
{
    i64_t id;
    union
    {
        mpmc_data_in_t in;
        mpmc_data_out_t out;
    };
} mpmc_data_t;

typedef struct cell_t
{
    u64_t seq;
    mpmc_data_t data;
} *cell_p;

typedef struct mpmc_t
{
    cachepad_t pad0;
    cell_p buf;
    i64_t mask;
    cachepad_t pad1;
    i64_t tail;
    cachepad_t pad2;
    i64_t head;
    cachepad_t pad3;
} *mpmc_p;

mpmc_p mpmc_create(u64_t size);
nil_t mpmc_destroy(mpmc_p queue);
i64_t mpmc_push(mpmc_p queue, mpmc_data_t data);
mpmc_data_t mpmc_pop(mpmc_p queue);
u64_t mpmc_size(mpmc_p queue);

#endif // MPMC_H

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

#ifndef THREAD_H
#define THREAD_H

#include "rayforce.h"

#if defined(OS_WINDOWS)

typedef struct
{
    HANDLE handle;
} thread_t;

typedef struct
{
    CRITICAL_SECTION inner;
} mutex_t;

typedef struct
{
    CONDITION_VARIABLE inner;
} cond_t;

#else

typedef struct
{
    pthread_t handle;
} thread_t;

typedef struct
{
    pthread_mutex_t inner;
} mutex_t;

typedef struct
{
    pthread_cond_t inner;
} cond_t;

#endif

mutex_t mutex_create();
nil_t mutex_destroy(mutex_t *mutex);
nil_t mutex_lock(mutex_t *mutex);
nil_t mutex_unlock(mutex_t *mutex);

cond_t cond_create();
nil_t cond_destroy(cond_t *cond);
i32_t cond_wait(cond_t *cond, mutex_t *mutex);
i32_t cond_signal(cond_t *cond);
i32_t cond_broadcast(cond_t *cond);

thread_t thread_create(raw_p (*fn)(raw_p), raw_p arg);
i32_t thread_destroy(thread_t *thread);
i32_t thread_join(thread_t thread);
i32_t thread_detach(thread_t thread);
nil_t thread_exit(raw_p res);

#endif // THREAD_H

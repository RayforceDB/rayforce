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

#ifndef TIME_H
#define TIME_H

#include "rayforce.h"

#define TIMEOUT_INFINITY -1
#define TIMEIT_SPANS_MAX 1024

typedef struct ray_timer_t
{
    i64_t id;  // Timer ID
    i64_t tic; // Interval between timer calls
    i64_t exp; // Expiration time of the timer
    i64_t num; // Number of times the timer has to be called
    obj_p clb; // Callback function to be called when timer expires
} *ray_timer_p;

typedef struct timers_t
{
    ray_timer_p *timers; // Array of timer pointers
    u64_t capacity;      // Maximum size of the heap
    u64_t size;          // Current number of timers in the heap
    i64_t counter;       // Counter to assign unique IDs to timers
} *timers_p;

#if defined(OS_WINDOWS)

typedef struct
{
    LARGE_INTEGER clock;
    LARGE_INTEGER freq;
} ray_clock_t;

#else

typedef struct
{
    struct timespec clock;
} ray_clock_t;

#endif

typedef enum
{
    TIMEIT_SPAN_START,
    TIMEIT_SPAN_END,
    TIMEIT_SPAN_TICK
} timeit_span_type_t;

typedef struct
{
    timeit_span_type_t type;
    lit_p msg;
    ray_clock_t clock;
} timeit_span_t;

typedef struct
{
    b8_t active;
    u64_t n;
    timeit_span_t spans[TIMEIT_SPANS_MAX];
} timeit_t;

nil_t timeit_activate(b8_t active);
nil_t timeit_reset();
nil_t timeit_span_start(lit_p name);
nil_t timeit_span_end(lit_p name);
nil_t timeit_tick(lit_p msg);
nil_t timeit_print(nil_t);

nil_t ray_clock_get_time(ray_clock_t *clock);
f64_t ray_clock_elapsed_ms(ray_clock_t *start, ray_clock_t *end);

timers_p timers_create(u64_t capacity);
nil_t timers_destroy(timers_p timers);
i64_t timer_next_timeout(timers_p timers);
nil_t timer_sleep(u64_t ms);

obj_p ray_timer(obj_p *x, u64_t n);
obj_p ray_timeit(obj_p x);

#endif // TIME_H

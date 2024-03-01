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

#ifndef TIMER_H
#define TIMER_H

#include "rayforce.h"

#define TIMEOUT_INFINITY -1

typedef struct ray_timer_t
{
    i64_t id;  // Timer ID
    i64_t tic; // Interval between timer calls
    i64_t exp; // Expiration time of the timer
    i64_t num; // Number of times the timer has to be called
    obj_p clb; // Callback function to be called when timer expires
} ray_timer_t;

typedef struct timers_t
{
    ray_timer_t **timers; // Array of timer pointers
    u64_t capacity;       // Maximum size of the heap
    u64_t size;           // Current number of timers in the heap
    i64_t counter;        // Counter to assign unique IDs to timers
} timers_t;

timers_t *timers_new(u64_t capacity);
nil_t timers_free(timers_t *timers);
i64_t timer_next_timeout(timers_t *timers);

obj_p ray_timer(obj_p *x, u64_t n);

#endif // TIMER_H

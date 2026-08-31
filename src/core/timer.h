/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_TIMER_H
#define RAY_TIMER_H

#include <rayforce.h>
#include <stdbool.h>

/* Single scheduled timer.  Lives in ray_timers_t.heap. */
typedef struct {
    int64_t id;       /* unique within a ray_timers_t; never reused */
    int64_t tic_ms;   /* interval between fires (and delay until first fire) */
    int64_t exp_ms;   /* next fire deadline, monotonic ms */
    int64_t num;      /* 0 = forever; >=1 = remaining fires (decremented each fire) */
    ray_t*  fn;       /* retained 1-arity lambda */
} ray_timer_t;

/* Min-heap keyed on (exp_ms, id).  Owns its entries and their lambdas. */
typedef struct {
    ray_timer_t** heap;
    int64_t       n;
    int64_t       cap;
    int64_t       next_id;
} ray_timers_t;

/* Create an empty heap with `initial_cap` slots (rounded up to 1).
 * Returns NULL on allocation failure. */
ray_timers_t* ray_timers_create(int64_t initial_cap);

/* Free the heap and release every retained callback lambda. */
void ray_timers_destroy(ray_timers_t* t);

/* Push a new timer.  Retains `fn`; caller's reference is unchanged.
 * Returns the new id (>= 0) on success, -1 on allocation failure. */
int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn);

/* Remove the timer with `id`.  Releases its lambda.
 * Returns true if found+removed, false otherwise. */
bool ray_timers_del(ray_timers_t* t, int64_t id);

/* Deadline (monotonic ms) of the soonest-firing timer, or INT64_MAX if empty. */
int64_t ray_timers_next_deadline_ms(ray_timers_t* t);

/* Fire every timer whose exp_ms <= now.  Pops, calls callback via call_fn1,
 * then either re-pushes (forever / num>1) or frees (num==1 / num==0 one-shot).
 * Callback errors are printed to stderr and counted as a successful fire. */
/* Fire every timer whose deadline has passed; returns how many ran. */
uint32_t ray_timers_fire_expired(ray_timers_t* t);

/* Current monotonic time in milliseconds. */
int64_t ray_time_now_ms(void);

/* TEST-ONLY: pump the timer heap for up to `budget_ms` milliseconds.
 * Sleeps in small slices and calls ray_timers_fire_expired between
 * slices.  Not intended for production code — ray_poll_run is the
 * real driver. */
void ray_timers_pump_for(ray_timers_t* t, int64_t budget_ms);

#endif /* RAY_TIMER_H */

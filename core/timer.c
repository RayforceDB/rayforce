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

#define _POSIX_C_SOURCE 200809L // Define POSIX source version for CLOCK_REALTIME
#include <time.h>
#include "timer.h"
#include "heap.h"
#include "runtime.h"
#include "error.h"
#include "io.h"

#if defined(_WIN32) || defined(__CYGWIN__)
u64_t get_time_millis(nil_t)
{
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10000ULL - 11644473600000ULL; // Convert to milliseconds since Unix epoch
}
#else
u64_t get_time_millis(nil_t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64_t)ts.tv_sec * 1000 + (u64_t)ts.tv_nsec / 1000000;
}
#endif

ray_timer_t *timer_new(i64_t id, u64_t tic, u64_t exp, i64_t num, obj_p clb)
{
    ray_timer_t *timer = (ray_timer_t *)heap_alloc(sizeof(ray_timer_t));

    timer->id = id;
    timer->tic = tic;
    timer->exp = exp;
    timer->num = num;
    timer->clb = clb;

    return timer;
}

nil_t timer_free(ray_timer_t *t)
{
    drop_obj(t->clb);
    heap_free(t);
}

nil_t timers_swap(ray_timer_t **a, ray_timer_t **b)
{
    ray_timer_t *temp = *a;
    *a = *b;
    *b = temp;
}

nil_t timer_up(timers_t *timers, u64_t index)
{
    u64_t parent_index = (index - 1) / 2;

    if (index > 0 && timers->timers[parent_index]->exp > timers->timers[index]->exp)
    {
        timers_swap(&(timers->timers[parent_index]), &(timers->timers[index]));
        timer_up(timers, parent_index);
    }
}

nil_t timer_down(timers_t *timers, u64_t index)
{
    u64_t left_child_index = 2 * index + 1;
    u64_t right_child_index = 2 * index + 2;
    u64_t smallest_index = index;

    if (left_child_index < timers->size && timers->timers[left_child_index]->exp < timers->timers[smallest_index]->exp)
        smallest_index = left_child_index;
    if (right_child_index < timers->size && timers->timers[right_child_index]->exp < timers->timers[smallest_index]->exp)
        smallest_index = right_child_index;
    if (smallest_index != index)
    {
        timers_swap(&(timers->timers[index]), &(timers->timers[smallest_index]));
        timer_down(timers, smallest_index);
    }
}

i64_t timer_push(timers_t *timers, ray_timer_t *timer)
{
    if (timers->size == timers->capacity)
        return -1;

    timers->timers[timers->size] = timer;
    timer_up(timers, timers->size);
    return timers->size++;
}

ray_timer_t *timer_pop(timers_t *timers)
{
    if (timers->size == 0)
        return NULL;

    ray_timer_t *timer = timers->timers[0];
    timers->timers[0] = timers->timers[timers->size - 1];
    timers->size--;
    timer_down(timers, 0);

    return timer;
}

timers_t *timers_new(u64_t capacity)
{
    timers_t *timers = (timers_t *)heap_alloc(sizeof(timers_t));

    timers->timers = (ray_timer_t **)heap_alloc(sizeof(ray_timer_t *) * capacity);
    timers->capacity = capacity;
    timers->size = 0;
    timers->counter = 0;

    return timers;
}

nil_t timers_free(timers_t *timers)
{
    u64_t i, l;

    l = timers->size;

    for (i = 0; i < l; i++)
        timer_free(timers->timers[i]);

    heap_free(timers->timers);
    heap_free(timers);
}

i64_t timer_add(timers_t *timers, u64_t tic, u64_t exp, u64_t num, obj_p clb)
{
    i64_t id;
    ray_timer_t *timer;

    id = timers->counter++;
    timer = timer_new(id, tic, exp, num, clb);

    return timer_push(timers, timer);
}

nil_t timer_del(timers_t *timers, i64_t id)
{
    i64_t i, l;

    l = (i64_t)timers->size;

    if (id < 0 || id >= l)
        return;

    for (i = 0; i < l; i++)
    {
        if (timers->timers[i]->id == id)
        {
            timer_free(timers->timers[i]);
            timers->timers[i] = timers->timers[timers->size - 1];
            timers->size--;
            timer_down(timers, i);
            return;
        }
    }
}

i64_t timer_next_timeout(timers_t *timers)
{
    i64_t now = get_time_millis();
    ray_timer_t *timer;
    obj_p res;

    if (timers->size == 0)
        return TIMEOUT_INFINITY;

    while (timers->size > 0 && timers->timers[0]->exp <= now)
    {
        // Pop the top timer for processing
        timer = timer_pop(timers);

        // Execute the callback associated with the timer
        stack_push(i64(now));
        res = call(timer->clb, 1);

        if (is_error(res))
            io_write(1, MSG_TYPE_RESP, res);

        drop_obj(res);

        // Check if the timer should be repeated
        if (timer->num > 0 || timer->num == NULL_I64)
        {
            if (timer->num > 1)
            {
                timer->num--; // Decrement repeat count if not infinite

                // Update the timer's expiration time for the next occurrence
                timer->exp += timer->tic;

                // Re-add the timer to the heap
                timer_push(timers, timer);
            }
            else
            {
                // Free the timer if it should not be repeated
                timer_free(timer);
            }
        }
        else
        {
            // Free the timer if it should not be repeated
            timer_free(timer);
        }

        // Update `now` in case time has advanced during processing
        now = get_time_millis();
    }

    // After processing all expired (and potentially re-added) timers,
    // return the expiration of the next timer or TIMEOUT_INFINITY
    return (timers->size > 0) ? timers->timers[0]->exp - now : TIMEOUT_INFINITY;
}

obj_p ray_timer(obj_p *x, u64_t n)
{
    i64_t id;
    timers_t *timers;

    if (n == 0)
        throw(ERR_IO, "timer: no arguments provided");

    timers = runtime_get()->poll->timers;

    if (n == 1)
    {
        if (x[0]->type != -TYPE_I64)
            throw(ERR_TYPE, "timer del: expected 'i64");

        timer_del(timers, x[0]->i64);

        return NULL_OBJ;
    }

    if (n != 3)
        throw(ERR_IO, "timer add: expected 3 arguments");

    if (x[0]->type != -TYPE_I64)
        throw(ERR_TYPE, "timer add: expected timeout as 'i64");

    if (x[1]->type != -TYPE_I64)
        throw(ERR_TYPE, "timer add: expected number of times as 'i64");

    if (x[2]->type != TYPE_LAMBDA)
        throw(ERR_TYPE, "timer add: expected callback as 'Lambda");

    if (as_lambda(x[2])->args->len != 1)
        throw(ERR_IO, "timer add: callback should take 1 argument");

    timers = runtime_get()->poll->timers;

    id = timer_add(timers, x[0]->i64, x[0]->i64 + get_time_millis(), x[1]->i64, clone_obj(x[2]));

    return i64(id);
}

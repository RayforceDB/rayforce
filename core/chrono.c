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

#include "chrono.h"
#include "heap.h"
#include "runtime.h"
#include "error.h"
#include "eval.h"
#include "io.h"
#include "ipc.h"

#if defined(OS_WINDOWS)

nil_t timer_sleep(i64_t ms) { Sleep((DWORD)ms); }

i64_t get_time_millis(nil_t) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10000ULL - 11644473600000ULL;  // Convert to milliseconds since Unix epoch
}

nil_t ray_clock_get_time(ray_clock_t *clock) {
    QueryPerformanceFrequency(&clock->freq);
    QueryPerformanceCounter(&clock->clock);
}

f64_t ray_clock_elapsed_ms(ray_clock_t *start, ray_clock_t *end) {
    return ((end->clock.QuadPart - start->clock.QuadPart) * 1000.0) / start->freq.QuadPart;  // Convert to milliseconds
}

#else

nil_t timer_sleep(i64_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

i64_t get_time_millis(nil_t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64_t)ts.tv_sec * 1000 + (i64_t)ts.tv_nsec / 1000000;
}

nil_t ray_clock_get_time(ray_clock_t *clock) { clock_gettime(CLOCK_MONOTONIC, &clock->clock); }

f64_t ray_clock_elapsed_ms(ray_clock_t *start, ray_clock_t *end) {
    f64_t elapsed;

    elapsed = (end->clock.tv_sec - start->clock.tv_sec) * 1e3;     // Convert to milliseconds
    elapsed += (end->clock.tv_nsec - start->clock.tv_nsec) / 1e6;  // Convert nanoseconds to milliseconds

    return elapsed;
}

#endif

// Lazy allocate timeit structure
static timeit_t *timeit_get_or_create(nil_t) {
    vm_p vm = VM;
    if (vm->timeit == NULL) {
        vm->timeit = (timeit_t *)heap_alloc(sizeof(timeit_t));
        vm->timeit->active = B8_FALSE;
        vm->timeit->n = 0;
    }
    return vm->timeit;
}

nil_t timeit_activate(b8_t active) {
    timeit_t *timeit = timeit_get_or_create();
    timeit->active = active;
    timeit->n = 0;
}

nil_t timeit_reset(nil_t) {
    timeit_t *timeit = VM->timeit;
    if (timeit && timeit->active)
        timeit->n = 0;
}

nil_t timeit_span_start(lit_p name) {
    timeit_t *timeit = VM->timeit;
    if (timeit && timeit->active && timeit->n < TIMEIT_SPANS_MAX) {
        timeit->spans[timeit->n].type = TIMEIT_SPAN_START;
        timeit->spans[timeit->n].msg = name;
        ray_clock_get_time(&timeit->spans[timeit->n].clock);
        timeit->n++;
    }
}

nil_t timeit_span_end(lit_p name) {
    timeit_t *timeit = VM->timeit;
    if (timeit && timeit->active && timeit->n < TIMEIT_SPANS_MAX) {
        timeit->spans[timeit->n].type = TIMEIT_SPAN_END;
        timeit->spans[timeit->n].msg = name;
        ray_clock_get_time(&timeit->spans[timeit->n].clock);
        timeit->n++;
    }
}

nil_t timeit_tick(lit_p msg) {
    timeit_t *timeit = VM->timeit;
    if (timeit && timeit->active && timeit->n < TIMEIT_SPANS_MAX) {
        timeit->spans[timeit->n].type = TIMEIT_SPAN_TICK;
        timeit->spans[timeit->n].msg = msg;
        ray_clock_get_time(&timeit->spans[timeit->n].clock);
        timeit->n++;
    }
}

nil_t timeit_print(nil_t) {
    timeit_t *timeit = VM->timeit;
    obj_p fmt;

    if (!timeit || !timeit->active)
        return;

    fmt = timeit_fmt();
    printf("%s%.*s%s", GRAY, (i32_t)fmt->len, AS_C8(fmt), RESET);
    drop_obj(fmt);
}

obj_p ray_timeit(obj_p *x, i64_t n) {
    i64_t i, l;
    ray_clock_t start, end;
    obj_p v;

    switch (n) {
        case 1:
            ray_clock_get_time(&start);

            v = eval(x[0]);
            if (IS_ERR(v))
                return v;
            drop_obj(v);

            ray_clock_get_time(&end);

            return f64(ray_clock_elapsed_ms(&start, &end));
        case 2:
            if (x[0]->type != -TYPE_I64)
                return err_type(0, 0, 0);

            l = x[0]->i64;

            if (l < 1)
                return err_domain();

            ray_clock_get_time(&start);

            for (i = 0; i < l; i++) {
                v = eval(x[1]);
                if (IS_ERR(v))
                    return v;
                drop_obj(v);
            }

            ray_clock_get_time(&end);

            return f64(ray_clock_elapsed_ms(&start, &end));
        default:
            return err_length(0, 0);
    }
}

ray_timer_p ray_timer_create(i64_t id, i64_t tic, i64_t exp, i64_t num, obj_p clb) {
    ray_timer_p timer = (ray_timer_p)heap_alloc(sizeof(struct ray_timer_t));

    timer->id = id;
    timer->tic = tic;
    timer->exp = exp;
    timer->num = num;
    timer->clb = clb;

    return timer;
}

nil_t timer_free(ray_timer_p t) {
    drop_obj(t->clb);
    heap_free(t);
}

nil_t timers_swap(ray_timer_p *a, ray_timer_p *b) {
    ray_timer_p temp = *a;
    *a = *b;
    *b = temp;
}

nil_t timer_up(timers_p timers, i64_t index) {
    i64_t parent_index = (index - 1) / 2;

    if (index > 0 && timers->timers[parent_index]->exp > timers->timers[index]->exp) {
        timers_swap(&(timers->timers[parent_index]), &(timers->timers[index]));
        timer_up(timers, parent_index);
    }
}

nil_t timer_down(timers_p timers, i64_t index) {
    i64_t left_child_index = 2 * index + 1;
    i64_t right_child_index = 2 * index + 2;
    i64_t smallest_index = index;

    if (left_child_index < timers->size && timers->timers[left_child_index]->exp < timers->timers[smallest_index]->exp)
        smallest_index = left_child_index;
    if (right_child_index < timers->size &&
        timers->timers[right_child_index]->exp < timers->timers[smallest_index]->exp)
        smallest_index = right_child_index;
    if (smallest_index != index) {
        timers_swap(&(timers->timers[index]), &(timers->timers[smallest_index]));
        timer_down(timers, smallest_index);
    }
}

i64_t timer_push(timers_p timers, ray_timer_p timer) {
    if (timers->size == timers->capacity)
        return -1;

    timers->timers[timers->size] = timer;
    timer_up(timers, timers->size);
    return timers->size++;
}

ray_timer_p timer_pop(timers_p timers) {
    if (timers->size == 0)
        return NULL;

    ray_timer_p timer = timers->timers[0];
    timers->timers[0] = timers->timers[timers->size - 1];
    timers->size--;
    timer_down(timers, 0);

    return timer;
}

timers_p timers_create(i64_t capacity) {
    timers_p timers = (timers_p)heap_alloc(sizeof(struct timers_t));

    timers->timers = (ray_timer_p *)heap_alloc(capacity * sizeof(ray_timer_p));
    timers->capacity = capacity;
    timers->size = 0;
    timers->counter = 0;

    return timers;
}

nil_t timers_destroy(timers_p timers) {
    i64_t i, l;

    l = timers->size;

    for (i = 0; i < l; i++)
        timer_free(timers->timers[i]);

    heap_free(timers->timers);
    heap_free(timers);
}

i64_t timer_add(timers_p timers, i64_t tic, i64_t exp, i64_t num, obj_p clb) {
    i64_t id;
    ray_timer_p timer;

    id = timers->counter++;
    timer = ray_timer_create(id, tic, exp, num, clb);

    return timer_push(timers, timer);
}

nil_t timer_del(timers_p timers, i64_t id) {
    i64_t i, l;

    l = (i64_t)timers->size;

    if (id < 0 || id >= l)
        return;

    for (i = 0; i < l; i++) {
        if (timers->timers[i]->id == id) {
            timer_free(timers->timers[i]);
            timers->timers[i] = timers->timers[timers->size - 1];
            timers->size--;
            timer_down(timers, i);
            return;
        }
    }
}

i64_t timer_next_timeout(timers_p timers) {
    i64_t now = get_time_millis();
    ray_timer_p timer;
    obj_p res;

    if (timers->size == 0)
        return TIMEOUT_INFINITY;

    while (timers->size > 0 && timers->timers[0]->exp <= now) {
        // Pop the top timer for processing
        timer = timer_pop(timers);

        // Execute the callback associated with the timer
        vm_stack_push(i64(now));
        res = call(timer->clb, 1);
        drop_obj(vm_stack_pop());

        if (IS_ERR(res))
            io_write(1, MSG_TYPE_RESP, res);

        drop_obj(res);

        // Check if the timer should be repeated
        if (timer->num > 0) {
            if (timer->num > 1) {
                timer->num--;  // Decrement repeat count if not infinite

                // Update the timer's expiration time for the next occurrence
                timer->exp += timer->tic;

                // Re-add the timer to the heap
                timer_push(timers, timer);
            } else {
                // Free the timer if it should not be repeated
                timer_free(timer);
            }
        } else {
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

obj_p ray_timer(obj_p *x, i64_t n) {
    i64_t id;
    i64_t num;
    timers_p timers;

    if (n == 0)
        return err_length(0, 0);

    timers = runtime_get()->poll->timers;

    if (n == 1) {
        if (x[0]->type != -TYPE_I64)
            return err_type(0, 0, 0);

        timer_del(timers, x[0]->i64);

        return NULL_OBJ;
    }

    if (n != 3)
        return err_length(0, 0);

    if (x[0]->type != -TYPE_I64)
        return err_type(0, 0, 0);

    if (x[1]->type != -TYPE_I64)
        return err_type(0, 0, 0);

    if (x[2]->type != TYPE_LAMBDA)
        return err_type(0, 0, 0);

    if (AS_LAMBDA(x[2])->args->len != 1)
        return err_length(0, 0);

    timers = runtime_get()->poll->timers;

    num = (i64_t)((x[1]->i64 == 0) ? NULL_I64 : x[1]->i64);

    id = timer_add(timers, x[0]->i64, x[0]->i64 + get_time_millis(), num, clone_obj(x[2]));

    return i64(id);
}

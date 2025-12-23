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

#ifndef POOL_H
#define POOL_H

#include "rayforce.h"
#include "thread.h"
#include "heap.h"
#include "eval.h"

#define CACHELINE_SIZE 64

typedef c8_t cachepad_t[CACHELINE_SIZE];

typedef obj_p (*fn0)(nil_t);
typedef obj_p (*fn1)(raw_p);
typedef obj_p (*fn2)(raw_p, raw_p);
typedef obj_p (*fn3)(raw_p, raw_p, raw_p);
typedef obj_p (*fn4)(raw_p, raw_p, raw_p, raw_p);
typedef obj_p (*fn5)(raw_p, raw_p, raw_p, raw_p, raw_p);
typedef obj_p (*fn6)(raw_p, raw_p, raw_p, raw_p, raw_p, raw_p);
typedef obj_p (*fn7)(raw_p, raw_p, raw_p, raw_p, raw_p, raw_p, raw_p);
typedef obj_p (*fn8)(raw_p, raw_p, raw_p, raw_p, raw_p, raw_p, raw_p, raw_p);

typedef struct {
    i64_t id;
    raw_p fn;
    i64_t argc;
    raw_p argv[8];
    obj_p result;
} task_data_t;

typedef struct cell_t {
    i64_t seq;
    task_data_t data;
} *cell_p;

typedef struct mpmc_t {
    cachepad_t pad0;
    cell_p buf;
    i64_t mask;
    cachepad_t pad1;
    i64_t tail;
    cachepad_t pad2;
    i64_t head;
    cachepad_t pad3;
} *mpmc_p;

typedef struct pool_t *pool_p;

typedef enum run_state_t { RUN_STATE_RUNNING = 0, RUN_STATE_STOPPED = 1 } run_state_t;

typedef struct executor_t {
    i64_t id;
    struct pool_t *pool;
    ray_thread_t handle;
    heap_p heap;
    vm_p vm;
} executor_t;

typedef struct pool_t {
    mutex_t mutex;           // Mutex for condition variable
    cond_t run;              // Condition variable for run VMs
    cond_t done;             // Condition variable for signal that VM is done
    run_state_t state;       // Pool's state
    i64_t done_count;        // Number of done VMs
    i64_t executors_count;   // Number of executors
    i64_t tasks_count;       // Number of tasks
    mpmc_p task_queue;       // Pool's task queue
    mpmc_p result_queue;     // Pool's result queue
    executor_t executors[];  // Array of executors
} *pool_p;

pool_p pool_create(i64_t executors_count);
nil_t pool_destroy(pool_p pool);
pool_p pool_get(nil_t);
nil_t pool_prepare(pool_p pool);
nil_t pool_add_task(pool_p pool, raw_p fn, i64_t argc, ...);
obj_p pool_call_task_fn(raw_p fn, i64_t argc, raw_p argv[]);
obj_p pool_run(pool_p pool);
i64_t pool_split_by(pool_p pool, i64_t input_len, i64_t groups_len);
i64_t pool_get_executors_count(pool_p pool);
i64_t pool_chunk_aligned(i64_t total_len, i64_t num_workers, i64_t elem_size);

typedef obj_p (*pool_map_fn)(i64_t len, i64_t offset, void *ctx);
nil_t pool_map(i64_t total_len, pool_map_fn fn, void *ctx);

#endif  // POOL_H

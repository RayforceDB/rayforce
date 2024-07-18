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

#include "pool.h"
#include "util.h"
#include "runtime.h"
#include "error.h"
#include "symbols.h"
#include "atomic.h"
#include "util.h"
#include "heap.h"
#include "string.h"

#define MPMC_SIZE 2048

mpmc_p mpmc_create(u64_t size)
{
    size = next_power_of_two_u64(size);

    i64_t i;
    mpmc_p queue;
    cell_p buf;

    queue = (mpmc_p)heap_mmap(sizeof(struct mpmc_t));

    if (queue == NULL)
        return NULL;

    buf = (cell_p)heap_mmap(size * sizeof(struct cell_t));

    if (buf == NULL)
        return NULL;

    for (i = 0; i < (i64_t)size; i += 1)
        __atomic_store_n(&buf[i].seq, i, __ATOMIC_RELAXED);

    queue->buf = buf;
    queue->mask = size - 1;

    __atomic_store_n(&queue->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&queue->head, 0, __ATOMIC_RELAXED);

    return queue;
}

nil_t mpmc_destroy(mpmc_p queue)
{
    if (queue->buf)
        heap_unmap(queue->buf, (queue->mask + 1) * sizeof(struct cell_t));

    heap_unmap(queue, sizeof(struct mpmc_t));
}

i64_t mpmc_push(mpmc_p queue, task_data_t data)
{
    cell_p cell;
    i64_t pos, seq, dif;
    u64_t rounds = 0;

    pos = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);

    for (;;)
    {
        cell = &queue->buf[pos & queue->mask];
        seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);

        dif = seq - pos;
        if (dif == 0)
        {
            if (__atomic_compare_exchange_n(&queue->tail, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
        else if (dif < 0)
            return -1;
        else
        {
            backoff_spin(&rounds);
            pos = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);
        }
    }

    cell->data = data;
    __atomic_store_n(&cell->seq, pos + 1, __ATOMIC_RELEASE);

    return 0;
}

task_data_t mpmc_pop(mpmc_p queue)
{
    cell_p cell;
    task_data_t data = {.id = -1, .fn = NULL, .argc = 0, .result = NULL_OBJ};
    i64_t pos, seq, dif;
    u64_t rounds = 0;

    pos = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);

    for (;;)
    {
        cell = &queue->buf[pos & queue->mask];
        seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
        dif = seq - (pos + 1);
        if (dif == 0)
        {
            if (__atomic_compare_exchange_n(&queue->head, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
        else if (dif < 0)
            return data;
        else
        {
            backoff_spin(&rounds);
            pos = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);
        }
    }

    data = cell->data;
    __atomic_store_n(&cell->seq, pos + queue->mask + 1, __ATOMIC_RELEASE);

    return data;
}

u64_t mpmc_count(mpmc_p queue)
{
    return __atomic_load_n(&queue->tail, __ATOMIC_SEQ_CST) - __atomic_load_n(&queue->head, __ATOMIC_SEQ_CST);
}

u64_t mpmc_size(mpmc_p queue)
{
    return queue->mask + 1;
}

obj_p pool_call_task_fn(raw_p fn, u64_t argc, raw_p argv[])
{
    switch (argc)
    {
    case 0:
        return ((fn0)fn)();
    case 1:
        return ((fn1)fn)(argv[0]);
    case 2:
        return ((fn2)fn)(argv[0], argv[1]);
    case 3:
        return ((fn3)fn)(argv[0], argv[1], argv[2]);
    case 4:
        return ((fn4)fn)(argv[0], argv[1], argv[2], argv[3]);
    case 5:
        return ((fn5)fn)(argv[0], argv[1], argv[2], argv[3], argv[4]);
    case 6:
        return ((fn6)fn)(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
    case 7:
        return ((fn7)fn)(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
    case 8:
        return ((fn8)fn)(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    default:
        return NULL_OBJ;
    }
}

raw_p executor_run(raw_p arg)
{
    executor_t *executor = (executor_t *)arg;
    task_data_t data;
    u64_t i, tasks_count;
    obj_p res;

    executor->heap = heap_create(executor->id + 1);
    executor->interpreter = interpreter_create();
    rc_sync(B8_TRUE);

    for (;;)
    {
        mutex_lock(&executor->pool->mutex);
        cond_wait(&executor->pool->run, &executor->pool->mutex);

        if (executor->pool->state == POOL_STATE_STOP)
        {
            mutex_unlock(&executor->pool->mutex);
            break;
        }

        tasks_count = executor->pool->tasks_count;
        mutex_unlock(&executor->pool->mutex);

        // process tasks
        for (i = 0; i < tasks_count; i++)
        {
            data = mpmc_pop(executor->pool->task_queue);

            // Nothing to do
            if (data.id == -1)
                break;

            // execute task
            res = pool_call_task_fn(data.fn, data.argc, data.argv);
            data.result = res;
            mpmc_push(executor->pool->result_queue, data);
        }

        if (i > 0)
        {
            mutex_lock(&executor->pool->mutex);
            executor->pool->done_count += i;
            cond_signal(&executor->pool->done);
            mutex_unlock(&executor->pool->mutex);
        }
    }

    interpreter_destroy();
    heap_destroy();

    return NULL;
}

pool_p pool_create(u64_t executors_count)
{
    u64_t i;
    pool_p pool;

    pool = (pool_p)heap_mmap(sizeof(struct pool_t) + (sizeof(executor_t) * executors_count));
    pool->executors_count = executors_count;
    pool->done_count = 0;
    pool->tasks_count = 0;
    pool->task_queue = mpmc_create(MPMC_SIZE);
    pool->result_queue = mpmc_create(MPMC_SIZE);
    pool->state = POOL_STATE_RUN;
    pool->mutex = mutex_create();
    pool->run = cond_create();
    pool->done = cond_create();
    mutex_lock(&pool->mutex);

    for (i = 0; i < executors_count; i++)
    {
        pool->executors[i].id = i;
        pool->executors[i].pool = pool;
        pool->executors[i].handle = thread_create(executor_run, &pool->executors[i]);
    }

    mutex_unlock(&pool->mutex);

    return pool;
}

nil_t pool_destroy(pool_p pool)
{
    u64_t i, n;

    mutex_lock(&pool->mutex);
    pool->state = POOL_STATE_STOP;
    cond_broadcast(&pool->run);
    mutex_unlock(&pool->mutex);

    n = pool->executors_count;
    for (i = 0; i < n; i++)
    {
        if (thread_join(pool->executors[i].handle) != 0)
            printf("Pool destroy: failed to join thread %lld\n", i);
    }

    mutex_destroy(&pool->mutex);
    cond_destroy(&pool->run);
    cond_destroy(&pool->done);
    mpmc_destroy(pool->task_queue);
    mpmc_destroy(pool->result_queue);

    heap_unmap(pool, sizeof(struct pool_t) + sizeof(executor_t) * pool->executors_count);
}

pool_p pool_get(nil_t)
{
    return runtime_get()->pool;
}

nil_t pool_prepare(pool_p pool)
{
    u64_t i, n;
    obj_p env;

    env = interpreter_env_get();

    mutex_lock(&pool->mutex);

    pool->tasks_count = 0;

    n = pool->executors_count;
    for (i = 0; i < n; i++)
    {
        heap_borrow(pool->executors[i].heap);
        interpreter_env_set(pool->executors[i].interpreter, clone_obj(env));
    }

    mutex_unlock(&pool->mutex);
}

nil_t pool_add_task(pool_p pool, raw_p fn, u64_t argc, ...)
{
    u64_t i;
    va_list args;
    task_data_t data;

    mutex_lock(&pool->mutex);

    data.id = pool->tasks_count++;
    data.fn = fn;
    data.argc = argc;

    // TODO: resize task queue
    // if (tasks_count > mpmc_size(pool->task_queue))
    //     {
    //         // Grow task queue
    //         mpmc_destroy(pool->task_queue);
    //         pool->task_queue = mpmc_create(tasks_count);
    //         // Grow result queue
    //         mpmc_destroy(pool->result_queue);
    //         pool->result_queue = mpmc_create(tasks_count);
    //     }

    va_start(args, argc);

    for (i = 0; i < argc; i++)
        data.argv[i] = va_arg(args, raw_p);

    va_end(args);

    mpmc_push(pool->task_queue, data);

    mutex_unlock(&pool->mutex);
}

obj_p pool_run(pool_p pool)
{
    u64_t i, n, m, tasks_count;
    obj_p res;
    task_data_t data;

    mutex_lock(&pool->mutex);

    rc_sync(B8_TRUE);

    // pool->done_count = 0;
    tasks_count = pool->tasks_count;
    m = (pool->executors_count < tasks_count) ? pool->executors_count : tasks_count;

    // wake up needed executors
    for (i = 0; i < m; i++)
        cond_signal(&pool->run);

    mutex_unlock(&pool->mutex);

    // process tasks on self too
    for (i = 0; i < tasks_count; i++)
    {
        data = mpmc_pop(pool->task_queue);

        // Nothing to do
        if (data.id == -1)
            break;

        // execute task
        res = pool_call_task_fn(data.fn, data.argc, data.argv);
        data.result = res;
        mpmc_push(pool->result_queue, data);
    }

    mutex_lock(&pool->mutex);

    pool->done_count += i;

    // wait for all tasks to be done
    while (pool->done_count < tasks_count)
        cond_wait(&pool->done, &pool->mutex);

    // collect results
    res = list(tasks_count);

    for (i = 0; i < tasks_count; i++)
    {
        data = mpmc_pop(pool->result_queue);
        if (data.id < 0 || data.id >= (i64_t)tasks_count)
            panic("Pool run: corrupted data: %lld\n", data.id);

        ins_obj(&res, data.id, data.result);
    }

    // merge heaps
    n = pool->executors_count;
    for (i = 0; i < n; i++)
    {
        heap_merge(pool->executors[i].heap);
        interpreter_env_unset(pool->executors[i].interpreter);
    }

    pool->done_count = 0;
    rc_sync(B8_FALSE);

    mutex_unlock(&pool->mutex);

    return res;
}

u64_t pool_executors_count(pool_p pool)
{
    if (pool)
        return pool->executors_count + 1;
    else
        return 1;
}

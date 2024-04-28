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

#define MPMC_SIZE 1024

raw_p executor_run(raw_p arg)
{
    executor_t *executor = (executor_t *)arg;
    mpmc_data_t data;
    obj_p res;

    executor->heap = heap_init(executor->id + 1);
    executor->interpreter = interpreter_new();
    rc_sync(B8_TRUE);

    for (;;)
    {
        pthread_mutex_lock(&executor->pool->mutex);
        pthread_cond_wait(&executor->pool->run, &executor->pool->mutex);

        if (executor->pool->state == POOL_STATE_STOP)
        {
            pthread_mutex_unlock(&executor->pool->mutex);
            break;
        }

        pthread_mutex_unlock(&executor->pool->mutex);

        // process tasks
        for (;;)
        {
            data = mpmc_pop(executor->pool->task_queue);

            // Nothing to do
            if (data.id == -1)
                break;

            // execute task
            res = data.in.fn(data.in.arg, data.in.len);
            mpmc_push(executor->pool->result_queue, (mpmc_data_t){data.id, .out = {data.in.arg, data.in.len, res}});
        }

        pthread_mutex_lock(&executor->pool->mutex);
        executor->pool->done_count++;
        pthread_cond_signal(&executor->pool->done);
        pthread_mutex_unlock(&executor->pool->mutex);
    }

    interpreter_destroy();
    heap_destroy();

    return NULL;
}

pool_p pool_new(u64_t executors_count)
{
    u64_t i;
    pool_p pool;

    pool = (pool_p)heap_mmap(sizeof(struct pool_t) + (sizeof(executor_t) * executors_count));
    pool->executors_count = executors_count;
    pool->done_count = 0;
    pool->task_queue = mpmc_create(MPMC_SIZE);
    pool->result_queue = mpmc_create(MPMC_SIZE);
    pool->state = POOL_STATE_RUN;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->run, NULL);
    pthread_cond_init(&pool->done, NULL);
    pthread_mutex_lock(&pool->mutex);

    for (i = 0; i < executors_count; i++)
    {
        pool->executors[i].id = i;
        pool->executors[i].pool = pool;
        pthread_create(&pool->executors[i].handle, NULL, executor_run, &pool->executors[i]);
    }

    pthread_mutex_unlock(&pool->mutex);

    return pool;
}

nil_t pool_destroy(pool_p pool)
{
    u64_t i;

    pthread_mutex_lock(&pool->mutex);
    pool->state = POOL_STATE_STOP;
    pthread_cond_broadcast(&pool->run);
    pthread_mutex_unlock(&pool->mutex);

    for (i = 0; i < pool->executors_count; i++)
    {
        if (pthread_join(pool->executors[i].handle, NULL) != 0)
            printf("Pool destroy: failed to join thread %lld\n", i);
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->run);
    pthread_cond_destroy(&pool->done);

    mpmc_destroy(pool->task_queue);
    mpmc_destroy(pool->result_queue);

    heap_unmap(pool, sizeof(struct pool_t) + sizeof(executor_t) * pool->executors_count);
}

nil_t pool_prepare(pool_p pool, u64_t tasks_count)
{
    if (!pool)
        return;

    rc_sync(B8_TRUE);

    u64_t i, n;
    obj_p env = interpreter_env_get();

    n = pool->executors_count;
    pool->done_count = 0;

    if (tasks_count > mpmc_size(pool->task_queue))
    {
        // Grow task queue
        mpmc_destroy(pool->task_queue);
        pool->task_queue = mpmc_create(tasks_count);
        // Grow result queue
        mpmc_destroy(pool->result_queue);
        pool->result_queue = mpmc_create(tasks_count);
    }

    for (i = 0; i < n; i++)
    {
        heap_borrow(pool->executors[i].heap);
        interpreter_env_set(pool->executors[i].interpreter, clone_obj(env));
    }
}

nil_t pool_add_task(pool_p pool, u64_t id, task_fn fn, raw_p arg, u64_t len)
{
    mpmc_push(pool->task_queue, (mpmc_data_t){id, .in = {fn, arg, len}});
}

obj_p pool_run(pool_p pool, u64_t tasks_count)
{
    u64_t i, n;
    obj_p res;
    mpmc_data_t data;

    n = pool->executors_count;

    // wake up all executors
    pthread_mutex_lock(&pool->mutex);
    pthread_cond_broadcast(&pool->run);
    pthread_mutex_unlock(&pool->mutex);

    // process tasks on self too
    for (;;)
    {
        data = mpmc_pop(pool->task_queue);

        // Nothing to do
        if (data.id == -1)
            break;

        // execute task
        res = data.in.fn(data.in.arg, data.in.len);
        mpmc_push(pool->result_queue, (mpmc_data_t){data.id, .out = {data.in.arg, data.in.len, res}});
    }

    pthread_mutex_lock(&pool->mutex);

    // wait for all tasks to be done
    while (pool->done_count < n)
        pthread_cond_wait(&pool->done, &pool->mutex);

    // merge heaps
    for (i = 0; i < n; i++)
    {
        heap_merge(pool->executors[i].heap);
        interpreter_env_unset(pool->executors[i].interpreter);
    }

    // collect results
    res = vector(TYPE_LIST, tasks_count);

    for (i = 0; i < tasks_count; i++)
    {
        data = mpmc_pop(pool->result_queue);
        debug_assert(data.id != -1, "Pool run: invalid data id!!!!");
        drop_obj(data.out.arg);
        ins_obj(&res, data.id, data.out.result);
    }

    pthread_mutex_unlock(&pool->mutex);
    rc_sync(B8_FALSE);

    return res;
}

u64_t pool_executors_count(pool_p pool)
{
    if (pool)
        return pool->executors_count;
    else
        return 0;
}

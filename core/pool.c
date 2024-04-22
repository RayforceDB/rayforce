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
#include "heap.h"
#include "util.h"
#include "runtime.h"
#include "eval.h"

raw_p executor_run(raw_p arg)
{
    executor_t *executor = (executor_t *)arg;

    executor->heap = heap_init(executor->id + 1);
    interpreter_new();

    for (;;)
    {
        pthread_mutex_lock(&executor->mutex);
        pthread_cond_wait(&executor->has_task, &executor->mutex);

        if (executor->stop)
        {
            pthread_mutex_unlock(&executor->mutex);
            break;
        }

        pthread_mutex_unlock(&executor->mutex);

        // execute task
        rc_sync(B8_TRUE);
        executor->task.result = executor->task.fn(executor->task.arg, executor->task.len);
        rc_sync(B8_FALSE);

        pthread_mutex_lock(&executor->mutex);
        executor->done = B8_TRUE;
        pthread_cond_signal(&executor->done_task);
        pthread_mutex_unlock(&executor->mutex);
    }

    interpreter_free();
    heap_cleanup();

    return NULL;
}

pool_p pool_new(u64_t executors_count)
{
    u64_t i;
    pool_p pool = (pool_p)heap_mmap(sizeof(struct pool_t) + (sizeof(executor_t) * executors_count));
    pool->executors_count = executors_count;

    for (i = 0; i < executors_count; i++)
    {
        pool->executors[i].id = i;
        pthread_mutex_init(&pool->executors[i].mutex, NULL);
        pthread_cond_init(&pool->executors[i].has_task, NULL);
        pthread_cond_init(&pool->executors[i].done_task, NULL);
        pool->executors[i].stop = B8_FALSE;
        pool->executors[i].done = B8_FALSE;
        pool->executors[i].task.fn = NULL;
        pool->executors[i].task.arg = NULL;
        pool->executors[i].task.len = 0;
        pool->executors[i].task.result = NULL_OBJ;
        pthread_create(&pool->executors[i].handle, NULL, executor_run, &pool->executors[i]);
    }

    return pool;
}

nil_t pool_run(pool_p pool, u64_t executor_id, task_fn fn, raw_p arg, u64_t len)
{
    executor_t *executor = &pool->executors[executor_id];

    pthread_mutex_lock(&executor->mutex);

    // borrow heap
    heap_borrow(executor->heap);

    executor->task.fn = fn;
    executor->task.arg = arg;
    executor->task.len = len;
    executor->task.result = NULL_OBJ;
    executor->done = B8_FALSE;

    pthread_cond_signal(&executor->has_task);
    pthread_mutex_unlock(&executor->mutex);
}

nil_t pool_wait(pool_p pool, u64_t executor_id)
{
    executor_t *executor = &pool->executors[executor_id];

    pthread_mutex_lock(&executor->mutex);

    while (!executor->done)
        pthread_cond_wait(&executor->done_task, &executor->mutex);

    // merge heap
    heap_merge(executor->heap);

    pthread_mutex_unlock(&executor->mutex);
}

obj_p pool_collect(pool_p pool, obj_p x)
{
    u64_t i;
    obj_p v, lst;
    u64_t n = pool->executors_count;

    if (is_error(x))
    {
        for (i = 0; i < n; i++)
            drop_obj(pool->executors[i].task.result);

        return x;
    }

    lst = vector(x->type, n + 1);

    for (i = 0; i < n; i++)
    {
        v = pool->executors[i].task.result;
        if (is_error(v))
        {
            v = clone_obj(v);
            lst->len = i;
            drop_obj(lst);

            for (i = 0; i < n; i++)
                drop_obj(pool->executors[i].task.result);

            return v;
        }

        ins_obj(&lst, i, v);
    }

    ins_obj(&lst, n, x);

    return lst;
}

u64_t pool_executors_count(pool_p pool)
{
    if (pool)
        return pool->executors_count;
    else
        return 0;
}

nil_t pool_stop(pool_p pool)
{
    u64_t i;

    for (i = 0; i < pool->executors_count; i++)
    {
        pthread_mutex_lock(&pool->executors[i].mutex);
        pool->executors[i].stop = B8_TRUE;
        pthread_cond_signal(&pool->executors[i].has_task);
        pthread_mutex_unlock(&pool->executors[i].mutex);
        if (pthread_join(pool->executors[i].handle, NULL) != 0)
            printf("Failed to join thread %lld\n", i);
        pthread_mutex_destroy(&pool->executors[i].mutex);
        pthread_cond_destroy(&pool->executors[i].has_task);
        pthread_cond_destroy(&pool->executors[i].done_task);
    }
}

nil_t pool_free(pool_p pool)
{
    pool_stop(pool);
    heap_unmap(pool, sizeof(struct pool_t) + sizeof(executor_t) * pool->executors_count);
}

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
    executor_p executor = (executor_p)arg;
    shared_p shared = executor->shared;
    u64_t id = executor->id;
    task_p task = &shared->tasks[id];

    executor->heap = heap_init(id + 1, 0);
    interpreter_new();

    for (;;)
    {
        pthread_mutex_lock(&shared->lock);
        pthread_cond_wait(&shared->run, &shared->lock);

        if (shared->stop)
        {
            pthread_mutex_unlock(&shared->lock);
            break;
        }

        pthread_mutex_unlock(&shared->lock);

        // execute task
        rc_sync(B8_TRUE);
        task->result = task->fn(task->arg, task->len);
        rc_sync(B8_FALSE);

        pthread_mutex_lock(&shared->lock);
        shared->tasks_remaining--;

        pthread_cond_signal(&shared->done);
        pthread_mutex_unlock(&shared->lock);
    }

    interpreter_free();
    heap_cleanup();

    return NULL;
}

pool_p pool_new(u64_t executors_count)
{
    u64_t i;
    shared_p shared;
    pool_p pool;
    executor_p executors;

    shared = (shared_p)heap_alloc(sizeof(struct shared_t));
    shared->tasks_remaining = 0;
    pthread_mutex_init(&shared->lock, NULL);
    pthread_cond_init(&shared->run, NULL);
    pthread_cond_init(&shared->done, NULL);
    shared->stop = B8_FALSE;

    shared->tasks = (task_p)heap_alloc(sizeof(struct task_t) * executors_count);
    for (i = 0; i < executors_count; i++)
    {
        shared->tasks[i].fn = NULL;
        shared->tasks[i].arg = NULL;
        shared->tasks[i].len = 0;
        shared->tasks[i].result = NULL_OBJ;
    }

    executors = (executor_p)heap_alloc(sizeof(struct executor_t) * executors_count);
    for (i = 0; i < executors_count; i++)
    {
        executors[i].id = i;
        executors[i].shared = shared;
        pthread_create(&executors[i].handle, NULL, executor_run, &executors[i]);
    }

    pool = (pool_p)heap_alloc(sizeof(struct pool_t));
    pool->executors = executors;
    pool->executors_count = executors_count;
    pool->shared = shared;

    return pool;
}

nil_t pool_run(pool_p pool)
{
    shared_p shared = pool->shared;

    rc_sync(B8_TRUE);

    pthread_mutex_lock(&shared->lock);
    shared->tasks_remaining = pool->executors_count;
    pthread_cond_broadcast(&shared->run);
}

nil_t pool_wait(pool_p pool)
{
    u64_t i, l;
    shared_p shared = pool->shared;

    while (shared->tasks_remaining)
        pthread_cond_wait(&shared->done, &shared->lock);

    // merge all heaps into the main heap
    l = pool->executors_count;
    for (i = 0; i < l; i++)
        heap_merge(pool->executors[i].heap);

    pthread_mutex_unlock(&shared->lock);

    rc_sync(B8_FALSE);
}

obj_p pool_collect(pool_p pool, obj_p x)
{
    u64_t i;
    obj_p v, lst;
    shared_p shared = pool->shared;
    u64_t n = pool->executors_count;

    if (is_error(x))
    {
        for (i = 0; i < n; i++)
            drop_obj(shared->tasks[i].result);

        return x;
    }

    lst = vector(x->type, n + 1);

    for (i = 0; i < n; i++)
    {
        v = shared->tasks[i].result;
        if (is_error(v))
        {
            v = clone_obj(v);
            lst->len = i;
            drop_obj(lst);

            for (i = 0; i < n; i++)
                drop_obj(shared->tasks[i].result);

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
        return pool->executors_count + 1;
    else
        return 1;
}

nil_t pool_stop(pool_p pool)
{
    u64_t i;

    pthread_mutex_lock(&pool->shared->lock);
    pool->shared->stop = B8_TRUE;
    pthread_cond_broadcast(&pool->shared->run);
    pthread_mutex_unlock(&pool->shared->lock);

    for (i = 0; i < pool->executors_count; i++)
        pthread_join(pool->executors[i].handle, NULL);
}

nil_t pool_free(pool_p pool)
{
    pool_stop(pool);
    heap_free(pool->shared->tasks);
    heap_free(pool->shared);
    heap_free(pool->executors);
    heap_free(pool);
}

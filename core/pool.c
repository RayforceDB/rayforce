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
#include "atomic.h"
#include "util.h"
#include "heap.h"
#include "eval.h"
#include "string.h"
#include "log.h"

#define DEFAULT_MPMC_SIZE 2048
#define POOL_SPLIT_THRESHOLD (RAY_PAGE_SIZE * 4)
#define GROUP_SPLIT_THRESHOLD 100000

mpmc_p mpmc_create(i64_t size) {
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

nil_t mpmc_destroy(mpmc_p queue) {
    if (queue->buf)
        heap_unmap(queue->buf, (queue->mask + 1) * sizeof(struct cell_t));

    heap_unmap(queue, sizeof(struct mpmc_t));
}

i64_t mpmc_push(mpmc_p queue, task_data_t data) {
    cell_p cell;
    i64_t pos, seq, dif;
    i64_t rounds = 0;

    pos = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);

    for (;;) {
        cell = &queue->buf[pos & queue->mask];
        seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);

        dif = seq - pos;
        if (dif == 0) {
            if (__atomic_compare_exchange_n(&queue->tail, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        } else if (dif < 0)
            return -1;
        else {
            backoff_spin(&rounds);
            pos = __atomic_load_n(&queue->tail, __ATOMIC_RELAXED);
        }
    }

    cell->data = data;
    __atomic_store_n(&cell->seq, pos + 1, __ATOMIC_RELEASE);

    return 0;
}

task_data_t mpmc_pop(mpmc_p queue) {
    cell_p cell;
    task_data_t data = {.id = -1,
                        .fn = NULL,
                        .argc = 0,
                        .argv = {NULL},  // Initialize the array elements
                        .result = NULL_OBJ};
    i64_t pos, seq, dif;
    i64_t rounds = 0;

    pos = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);

    for (;;) {
        cell = &queue->buf[pos & queue->mask];
        seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
        dif = seq - (pos + 1);
        if (dif == 0) {
            if (__atomic_compare_exchange_n(&queue->head, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        } else if (dif < 0)
            return data;
        else {
            backoff_spin(&rounds);
            pos = __atomic_load_n(&queue->head, __ATOMIC_RELAXED);
        }
    }

    data = cell->data;
    __atomic_store_n(&cell->seq, pos + queue->mask + 1, __ATOMIC_RELEASE);

    return data;
}

i64_t mpmc_count(mpmc_p queue) {
    return __atomic_load_n(&queue->tail, __ATOMIC_SEQ_CST) - __atomic_load_n(&queue->head, __ATOMIC_SEQ_CST);
}

i64_t mpmc_size(mpmc_p queue) { return queue->mask + 1; }

obj_p pool_call_task_fn(raw_p fn, i64_t argc, raw_p argv[]) {
    switch (argc) {
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

raw_p executor_run(raw_p arg) {
    executor_t *executor = (executor_t *)arg;
    task_data_t data;
    i64_t i, tasks_count;
    obj_p res;
    vm_p vm;

    // Create VM (which also creates heap) with pool pointer
    vm = vm_create(executor->id, executor->pool);
    vm->rc_sync = 1;  // Enable atomic RC for worker threads

    __atomic_store_n(&executor->heap, vm->heap, __ATOMIC_RELAXED);
    __atomic_store_n(&executor->vm, vm, __ATOMIC_RELAXED);

    for (;;) {
        mutex_lock(&executor->pool->mutex);
        cond_wait(&executor->pool->run, &executor->pool->mutex);

        if (executor->pool->state == RUN_STATE_STOPPED) {
            mutex_unlock(&executor->pool->mutex);
            break;
        }

        tasks_count = executor->pool->tasks_count;
        mutex_unlock(&executor->pool->mutex);

        // process tasks
        for (i = 0; i < tasks_count; i++) {
            data = mpmc_pop(executor->pool->task_queue);

            // Nothing to do
            if (data.id == -1)
                break;

            // execute task
            res = pool_call_task_fn(data.fn, data.argc, data.argv);
            data.result = res;
            mpmc_push(executor->pool->result_queue, data);
        }

        if (i > 0) {
            mutex_lock(&executor->pool->mutex);
            executor->pool->done_count += i;
            cond_signal(&executor->pool->done);
            mutex_unlock(&executor->pool->mutex);
        }
    }

    vm_destroy(__VM);

    return NULL;
}

// ============================================================================
// CPU Topology - build optimal CPU mapping for thread pinning
// ============================================================================

#if defined(OS_LINUX)

#define MAX_CPUS 256

typedef struct {
    i64_t cpu_id;
    i64_t core_id;      // physical core (first sibling CPU id)
    i64_t smt_index;    // 0 = first thread, 1 = second thread, etc.
} cpu_info_t;

// Parse "0,12" or "0-2,12-14" format, return first CPU as core_id
static i64_t parse_siblings(const char* str, i64_t cpu_id, i64_t* smt_index) {
    i64_t first_cpu = -1;
    i64_t idx = 0;
    const char* p = str;

    while (*p) {
        i64_t num = 0;
        while (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
            p++;
        }
        if (first_cpu < 0) first_cpu = num;
        if (num == cpu_id) {
            *smt_index = idx;
            return first_cpu;
        }
        idx++;
        // Skip '-N' ranges
        if (*p == '-') {
            p++;
            i64_t range_end = 0;
            while (*p >= '0' && *p <= '9') {
                range_end = range_end * 10 + (*p - '0');
                p++;
            }
            for (i64_t r = num + 1; r <= range_end; r++) {
                if (r == cpu_id) {
                    *smt_index = idx;
                    return first_cpu;
                }
                idx++;
            }
        }
        if (*p == ',') p++;
    }
    *smt_index = 0;
    return cpu_id;  // fallback: CPU is its own core
}

// Build CPU mapping: cpu_map[worker_id] = cpu_id
// Order: core0_t0, core0_t1, core1_t0, core1_t1, ...
static void build_cpu_topology(i64_t* cpu_map, i64_t count) {
    cpu_info_t cpus[MAX_CPUS];
    i64_t num_cpus = 0;
    char path[128];
    char buf[256];

    // Read topology for each CPU
    for (i64_t cpu = 0; cpu < MAX_CPUS && num_cpus < count; cpu++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%lld/topology/thread_siblings_list", cpu);
        FILE* f = fopen(path, "r");
        if (!f) continue;

        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = 0;
            cpus[num_cpus].cpu_id = cpu;
            cpus[num_cpus].core_id = parse_siblings(buf, cpu, &cpus[num_cpus].smt_index);
            num_cpus++;
        }
        fclose(f);
    }

    // Sort by (smt_index, core_id) to get: all thread0s first, then all thread1s
    for (i64_t i = 0; i < num_cpus - 1; i++) {
        for (i64_t j = i + 1; j < num_cpus; j++) {
            int swap = 0;
            if (cpus[i].smt_index > cpus[j].smt_index) swap = 1;
            else if (cpus[i].smt_index == cpus[j].smt_index &&
                     cpus[i].core_id > cpus[j].core_id) swap = 1;
            if (swap) {
                cpu_info_t tmp = cpus[i];
                cpus[i] = cpus[j];
                cpus[j] = tmp;
            }
        }
    }

    // Now interleave: take one from each SMT level alternately per core
    // Result: core0_t0, core0_t1, core1_t0, core1_t1, ...
    i64_t max_smt = 0;
    for (i64_t i = 0; i < num_cpus; i++)
        if (cpus[i].smt_index > max_smt) max_smt = cpus[i].smt_index;

    i64_t physical_cores = num_cpus / (max_smt + 1);
    if (physical_cores == 0) physical_cores = num_cpus;

    i64_t out_idx = 0;
    for (i64_t core = 0; core < physical_cores && out_idx < count; core++) {
        for (i64_t smt = 0; smt <= max_smt && out_idx < count; smt++) {
            // Find CPU with this core_id and smt_index
            for (i64_t i = 0; i < num_cpus; i++) {
                // core_id is the first CPU of the physical core
                // We need to match by position in sorted order
                if (cpus[i].smt_index == smt) {
                    // Count how many cores we've seen at this SMT level
                    i64_t core_count = 0;
                    for (i64_t k = 0; k < i; k++)
                        if (cpus[k].smt_index == smt) core_count++;
                    if (core_count == core) {
                        cpu_map[out_idx++] = cpus[i].cpu_id;
                        break;
                    }
                }
            }
        }
    }

    // Fill remaining with sequential if topology read failed
    for (i64_t i = out_idx; i < count; i++)
        cpu_map[i] = i;

    LOG_INFO("CPU topology: %lld physical cores, SMT x%lld", physical_cores, max_smt + 1);
}

#else

static void build_cpu_topology(i64_t* cpu_map, i64_t count) {
    // Fallback: sequential mapping
    for (i64_t i = 0; i < count; i++)
        cpu_map[i] = i;
}

#endif

// ============================================================================

pool_p pool_create(i64_t thread_count) {
    i64_t i, rounds = 0;
    pool_p pool;
    vm_p vm;
    i64_t cpu_map[MAX_CPUS];

    // Build optimal CPU mapping based on topology
    build_cpu_topology(cpu_map, thread_count);

    // thread_count includes main thread, so we allocate for all
    pool = (pool_p)heap_mmap(sizeof(struct pool_t) + (sizeof(executor_t) * thread_count));
    pool->executors_count = thread_count;
    pool->done_count = 0;
    pool->tasks_count = 0;
    pool->task_queue = mpmc_create(DEFAULT_MPMC_SIZE);
    pool->result_queue = mpmc_create(DEFAULT_MPMC_SIZE);
    pool->state = RUN_STATE_RUNNING;
    pool->mutex = mutex_create();
    pool->run = cond_create();
    pool->done = cond_create();

    // Executor[0] is the main thread - create VM directly here
    pool->executors[0].id = 0;
    pool->executors[0].pool = pool;
    vm = vm_create(0, pool);
    pool->executors[0].vm = vm;
    pool->executors[0].heap = vm->heap;
    pool->executors[0].handle = thread_self();

    if (thread_pin(thread_self(), cpu_map[0]) != 0)
        LOG_WARN("failed to pin main thread to CPU %lld", cpu_map[0]);

    // Create worker threads for executor[1..n-1]
    mutex_lock(&pool->mutex);
    for (i = 1; i < thread_count; i++) {
        pool->executors[i].id = i;
        pool->executors[i].pool = pool;
        pool->executors[i].heap = NULL;
        pool->executors[i].vm = NULL;
        pool->executors[i].handle = ray_thread_create(executor_run, &pool->executors[i]);
        if (thread_pin(pool->executors[i].handle, cpu_map[i]) != 0)
            LOG_WARN("failed to pin thread %lld to CPU %lld", i, cpu_map[i]);
    }
    mutex_unlock(&pool->mutex);

    // Wait for worker threads to initialize
    for (i = 1; i < thread_count; i++) {
        while (__atomic_load_n(&pool->executors[i].vm, __ATOMIC_RELAXED) == NULL)
            backoff_spin(&rounds);
    }

    return pool;
}

nil_t pool_destroy(pool_p pool) {
    i64_t i, n;

    mutex_lock(&pool->mutex);
    pool->state = RUN_STATE_STOPPED;
    cond_broadcast(&pool->run);
    mutex_unlock(&pool->mutex);

    n = pool->executors_count;

    // Join worker threads (executor[1..n-1]), not main thread
    for (i = 1; i < n; i++) {
        if (thread_join(pool->executors[i].handle) != 0)
            LOG_WARN("failed to join thread %lld", i);
    }

    mutex_destroy(&pool->mutex);
    cond_destroy(&pool->run);
    cond_destroy(&pool->done);
    mpmc_destroy(pool->task_queue);
    mpmc_destroy(pool->result_queue);

    // Destroy main thread's VM (executor[0]) last - after all heap operations
    vm_destroy(pool->executors[0].vm);

    // Use mmap_free directly since heap is already destroyed
    mmap_free(pool, sizeof(struct pool_t) + sizeof(executor_t) * pool->executors_count);
}

pool_p pool_get(nil_t) { return runtime_get()->pool; }

nil_t pool_prepare(pool_p pool) {
    i64_t i, n;

    if (pool == NULL)
        PANIC("pool is NULL");

    mutex_lock(&pool->mutex);

    pool->tasks_count = 0;
    pool->done_count = 0;

    n = pool->executors_count;
    for (i = 1; i < n; i++) {  // Skip executor[0] (main thread) - no self-borrow
        heap_borrow(pool->executors[i].heap);
    }

    mutex_unlock(&pool->mutex);
}

nil_t pool_add_task(pool_p pool, raw_p fn, i64_t argc, ...) {
    i64_t i, size;
    va_list args;
    task_data_t data, old_data;
    mpmc_p queue;

    if (pool == NULL)
        PANIC("pool is NULL");

    mutex_lock(&pool->mutex);

    data.id = pool->tasks_count++;
    data.fn = fn;
    data.argc = argc;

    va_start(args, argc);

    for (i = 0; i < argc; i++)
        data.argv[i] = va_arg(args, raw_p);

    va_end(args);

    if (mpmc_push(pool->task_queue, data) == -1)  // queue is full
    {
        size = pool->tasks_count * 2;
        // Grow task queue
        queue = mpmc_create(size);

        for (;;) {
            old_data = mpmc_pop(pool->task_queue);
            if (old_data.id == -1)
                break;
            mpmc_push(queue, old_data);
        }

        if (mpmc_push(queue, data) == -1)
            PANIC("oom");

        mpmc_destroy(pool->task_queue);
        pool->task_queue = queue;
        // Grow result queue
        mpmc_destroy(pool->result_queue);
        pool->result_queue = mpmc_create(size);
    }

    mutex_unlock(&pool->mutex);
}

obj_p pool_run(pool_p pool) {
    i64_t i, n, tasks_count, executors_count;
    obj_p e, res;
    task_data_t data;

    if (pool == NULL)
        PANIC("pool is NULL");

    mutex_lock(&pool->mutex);

    rc_sync_set(1);

    tasks_count = pool->tasks_count;
    executors_count = pool->executors_count;

    // wake up needed executors
    if (executors_count < tasks_count) {
        for (i = 0; i < executors_count; i++)
            cond_signal(&pool->run);
    } else
        cond_broadcast(&pool->run);

    mutex_unlock(&pool->mutex);

    // process tasks on self too
    for (i = 0; i < tasks_count; i++) {
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
    res = LIST(tasks_count);

    for (i = 0; i < tasks_count; i++) {
        data = mpmc_pop(pool->result_queue);
        if (data.id < 0 || data.id >= (i64_t)tasks_count)
            PANIC("corrupted: %lld", data.id);

        ins_obj(&res, data.id, data.result);
    }

    // merge heaps
    n = pool->executors_count;
    for (i = 1; i < n; i++) {  // Skip executor[0] (main thread) - no self-merge
        heap_merge(pool->executors[i].heap);
    }

    rc_sync_set(0);

    mutex_unlock(&pool->mutex);

    // Check res for errors
    for (i = 0; i < tasks_count; i++) {
        if (IS_ERR(AS_LIST(res)[i])) {
            e = clone_obj(AS_LIST(res)[i]);
            drop_obj(res);
            return e;
        }
    }

    return res;
}

i64_t pool_split_by(pool_p pool, i64_t input_len, i64_t groups_len) {
    if (pool == NULL || input_len < POOL_SPLIT_THRESHOLD)
        return 1;
    else if (rc_sync_get())
        return 1;
    else if (input_len <= pool->executors_count)
        return 1;
    else if (groups_len >= GROUP_SPLIT_THRESHOLD)
        return 1;
    else
        return pool->executors_count;
}

i64_t pool_get_executors_count(pool_p pool) {
    if (pool == NULL)
        return 1;
    else
        return pool->executors_count;
}

// Calculate page-aligned chunk size for parallel operations
// This ensures each worker operates on contiguous pages for cache efficiency
i64_t pool_chunk_aligned(i64_t total_len, i64_t num_workers, i64_t elem_size) {
    if (num_workers <= 1 || elem_size <= 0)
        return total_len;

    i64_t elems_per_page = RAY_PAGE_SIZE / elem_size;
    if (elems_per_page == 0)
        elems_per_page = 1;

    i64_t total_pages = (total_len + elems_per_page - 1) / elems_per_page;
    i64_t pages_per_chunk = (total_pages + num_workers - 1) / num_workers;

    return pages_per_chunk * elems_per_page;
}

nil_t pool_map(i64_t total_len, pool_map_fn fn, void *ctx) {
    pool_p pool;
    i64_t i, n, chunk;
    obj_p v;

    pool = pool_get();
    n = pool_split_by(pool, total_len, 0);

    if (n == 1) {
        fn(total_len, 0, ctx);
        return;
    }

    chunk = total_len / n;
    pool_prepare(pool);

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, (raw_p)fn, 3, (raw_p)chunk, (raw_p)(i * chunk), ctx);

    pool_add_task(pool, (raw_p)fn, 3, (raw_p)(total_len - i * chunk), (raw_p)(i * chunk), ctx);

    v = pool_run(pool);
    drop_obj(v);
}
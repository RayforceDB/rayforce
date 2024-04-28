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

#include "mpmc.h"
#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif
#include "util.h"
#include "heap.h"
#include "string.h"

#define BACKOFF_SPIN_LIMIT 8

static inline void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__arm__) || defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    // Generic fallback: no-op or compiler barrier
    __asm__ volatile("" ::: "memory"); // acts as a compiler barrier
#endif
}

nil_t backoff_spin(u64_t *rounds)
{
    u64_t i;

    for (i = 0; i < (1ull << *rounds); i++)
        cpu_relax();

    if (*rounds <= BACKOFF_SPIN_LIMIT)
        (*rounds)++;
}

mpmc_p mpmc_create(u64_t size)
{
    size = next_power_of_two_u64(size);

    i64_t i;
    mpmc_p queue;
    cell_p buf;

    queue = heap_mmap(sizeof(struct mpmc_t));

    if (queue == NULL)
        return NULL;

    buf = heap_mmap(size * sizeof(struct cell_t));

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

i64_t mpmc_push(mpmc_p queue, mpmc_data_t data)
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

mpmc_data_t mpmc_pop(mpmc_p queue)
{
    cell_p cell;
    mpmc_data_t data = {.id = -1, .in = {0}};
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

u64_t mpmc_size(mpmc_p queue)
{
    return queue->mask + 1;
}
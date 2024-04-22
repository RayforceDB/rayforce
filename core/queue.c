/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include "queue.h"
#include "heap.h"
#include "util.h"
#include "ops.h"

queue_t queue_new(i64_t size)
{
    return (queue_t){
        .size = size,
        .head = 0,
        .tail = 0,
        .data = (raw_p *)heap_alloc(size * sizeof(raw_p)),
    };
}

nil_t queue_free(queue_t *queue)
{
    heap_free(queue->data);
}

nil_t queue_push(queue_t *queue, raw_p val)
{
    if (queue->tail - queue->head == queue->size)
    {
        queue->size *= 2;
        queue->data = (raw_p *)heap_realloc(queue->data, queue->size * sizeof(raw_p));
    }

    queue->data[queue->tail % queue->size] = val;
    queue->tail++;
}

raw_p queue_pop(queue_t *queue)
{
    raw_p v;

    if (queue->head == queue->tail)
        return NULL_OBJ;

    v = queue->data[queue->head % queue->size];
    queue->head++;

    return v;
}

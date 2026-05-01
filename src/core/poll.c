/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#include "core/poll.h"
#include "mem/sys.h"

/* ===== Shared (platform-independent) poll helpers ===== */

void ray_poll_exit(ray_poll_t* poll, int64_t code)
{
    if (poll) poll->code = code;
}

ray_selector_t* ray_poll_get(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels)
        return NULL;
    return poll->sels[id];
}

ray_poll_buf_t* ray_poll_buf_new(int64_t size)
{
    ray_poll_buf_t* buf = (ray_poll_buf_t*)ray_sys_alloc(
        sizeof(ray_poll_buf_t) + (size_t)size);
    if (!buf) return NULL;
    buf->next   = NULL;
    buf->size   = size;
    buf->offset = 0;
    return buf;
}

void ray_poll_buf_free(ray_poll_buf_t* buf)
{
    while (buf) {
        ray_poll_buf_t* next = buf->next;
        ray_sys_free(buf);
        buf = next;
    }
}

void ray_poll_rx_request(ray_poll_t* poll, ray_selector_t* sel, int64_t size)
{
    (void)poll;
    if (sel->rx.buf) {
        /* Reuse if large enough, otherwise reallocate */
        if (sel->rx.buf->size >= size) {
            sel->rx.buf->offset = 0;
            sel->rx.buf->size   = size;
            return;
        }
        ray_poll_buf_free(sel->rx.buf);
    }
    sel->rx.buf = ray_poll_buf_new(size);
}

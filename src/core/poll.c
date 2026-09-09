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
#include "core/timer.h"
#include "mem/sys.h"
#include <errno.h>

/* ===== Shared (platform-independent) poll helpers ===== */

void ray_poll_exit(ray_poll_t* poll, int64_t code)
{
    if (poll) poll->code = code;
}

void ray_poll_set_restricted(ray_poll_t* poll, bool restricted)
{
    if (poll) poll->restricted = restricted;
}

ray_selector_t* ray_poll_get(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels)
        return NULL;
    return poll->sels[id];
}

bool ray_poll_idle(ray_poll_t* poll)
{
    if (!poll) return true;
    if (poll->n_live > 0) return false;
    if (!poll->timers) return true;
    return ray_timers_next_deadline_ms((ray_timers_t*)poll->timers) == INT64_MAX;
}

void ray_poll_drain_timers(ray_poll_t* poll)
{
    if (!poll) return;
    while (poll->code < 0 && poll->timers &&
           ray_timers_next_deadline_ms((ray_timers_t*)poll->timers) != INT64_MAX) {
        /* One bounded pass: wakes for the next deadline (the loop trims
         * the wait to it) or for any selector event, fires what is due. */
        if (ray_poll_run_for(poll, 1000) < 0) break;
    }
}

ray_poll_buf_t* ray_poll_buf_new(int64_t size)
{
    ray_poll_buf_t* buf = (ray_poll_buf_t*)ray_sys_alloc(
        sizeof(ray_poll_buf_t) + (size_t)size);
    if (!buf) return NULL;
    buf->next   = NULL;
    buf->size   = size;
    buf->offset = 0;
    buf->data   = buf->storage;
    buf->frame  = NULL;
    return buf;
}

void ray_poll_buf_free(ray_poll_buf_t* buf)
{
    while (buf) {
        ray_poll_buf_t* next = buf->next;
        if (buf->frame) ray_poll_frame_release(buf->frame);
        ray_sys_free(buf);
        buf = next;
    }
}

ray_poll_frame_t* ray_poll_frame_new(int64_t size)
{
    ray_poll_frame_t* f = (ray_poll_frame_t*)ray_sys_alloc(
        sizeof(ray_poll_frame_t) + (size_t)size);
    if (!f) return NULL;
    f->rc   = 1;
    f->size = size;
    return f;
}

void ray_poll_frame_retain(ray_poll_frame_t* f)
{
    if (f) f->rc++;
}

void ray_poll_frame_release(ray_poll_frame_t* f)
{
    if (f && --f->rc == 0) ray_sys_free(f);
}

ray_poll_buf_t* ray_poll_buf_from_frame(ray_poll_frame_t* f)
{
    if (!f) return NULL;
    ray_poll_buf_t* buf = (ray_poll_buf_t*)ray_sys_alloc(sizeof(ray_poll_buf_t));
    if (!buf) return NULL;
    buf->next   = NULL;
    buf->size   = f->size;
    buf->offset = 0;
    buf->data   = f->data;
    buf->frame  = f;
    ray_poll_frame_retain(f);
    return buf;
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

int ray_poll_tx_flush(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!poll || !sel || !sel->tx.send_fn) return -1;

    while (sel->tx.buf) {
        ray_poll_buf_t* buf = sel->tx.buf;
        while (buf->offset < buf->size) {
            int64_t nw = sel->tx.send_fn(
                sel->fd,
                buf->data + buf->offset,
                buf->size - buf->offset);
            if (nw < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                return -1;
            }
            if (nw == 0) return -1;
            buf->offset += nw;
        }

        sel->tx.buf = buf->next;
        buf->next = NULL;
        ray_poll_buf_free(buf);
    }

    ray_poll_tx_cancel(poll, sel);
    return 1;
}

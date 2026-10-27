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
    ray_poll_buf_t* buf = (ray_poll_buf_t*)ray_alloc_raw(
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
        ray_free_raw(buf);
        buf = next;
    }
}

ray_poll_frame_t* ray_poll_frame_new(int64_t size)
{
    ray_poll_frame_t* f = (ray_poll_frame_t*)ray_alloc_raw(
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
    if (f && --f->rc == 0) ray_free_raw(f);
}

ray_poll_buf_t* ray_poll_buf_from_frame(ray_poll_frame_t* f)
{
    if (!f) return NULL;
    ray_poll_buf_t* buf = (ray_poll_buf_t*)ray_alloc_raw(sizeof(ray_poll_buf_t));
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

/* ===== The connection's byte stream: transport or socket ===== */

int64_t ray_poll_sel_recv(ray_selector_t* sel, uint8_t* buf, int64_t len)
{
    if (sel->xport) {
        int64_t n = sel->xport->ops->read(sel->xport, buf, len);
        if (n < 0) { errno = ECONNRESET; return -1; }
        if (n == 0) { errno = EAGAIN; return -1; }
        return n;
    }
    if (!sel->rx.recv_fn) { errno = EAGAIN; return -1; }
    return sel->rx.recv_fn(sel->fd, buf, len);
}

int64_t ray_poll_sel_send(ray_selector_t* sel, const uint8_t* buf, int64_t len)
{
    if (sel->xport) {
        int64_t n = sel->xport->ops->write(sel->xport, buf, len, false);
        if (n < 0) { errno = EPIPE; return -1; }
        if (n == 0) { errno = EAGAIN; return -1; }
        return n;
    }
    if (!sel->tx.send_fn) { errno = EPIPE; return -1; }
    return sel->tx.send_fn(sel->fd, (uint8_t*)buf, len);
}

static void sel_fail(ray_poll_t* poll, ray_selector_t* sel)
{
    if (sel->error_fn) sel->error_fn(poll, sel);
    else ray_poll_deregister(poll, sel->id);
}

int ray_poll_serve_rx(ray_poll_t* poll, int64_t id)
{
    /* Loop: read data → call read_fn → if state advanced, read more.
     * Handles multi-phase protocols (handshake → header → payload)
     * arriving in a single wakeup. */
    for (;;) {
        ray_selector_t* sel = ray_poll_get(poll, id);
        if (!sel) return -1;
        if (sel->rx.buf && (sel->rx.recv_fn || sel->xport)) {
            while (sel->rx.buf->offset < sel->rx.buf->size) {
                int64_t nr = ray_poll_sel_recv(sel,
                                               sel->rx.buf->data + sel->rx.buf->offset,
                                               sel->rx.buf->size - sel->rx.buf->offset);
                if (nr <= 0) {
                    if (nr < 0 && errno == EINTR) continue;
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    /* Error or peer closed mid-read */
                    sel_fail(poll, sel);
                    return -1;
                }
                sel->rx.buf->offset += nr;
            }
        }

        /* Not enough data for the current phase */
        if (sel->rx.buf && sel->rx.buf->offset < sel->rx.buf->size)
            return 0;

        /* Call read_fn — may advance state and request a new buffer */
        if (!sel->rx.read_fn) return 0;
        ray_t* obj = sel->rx.read_fn(poll, sel);

        /* Re-validate: read_fn may have deregistered this selector */
        sel = ray_poll_get(poll, id);
        if (!sel) return -1;

        if (obj && sel->data_fn)
            sel->data_fn(poll, sel, obj);

        /* If data_fn deregistered the selector, stop */
        sel = ray_poll_get(poll, id);
        if (!sel) return -1;

        /* No rx buffer (state machine done or not set): stop */
        if (!sel->rx.buf) return 0;
        /* The buffer already holds enough for the next phase: loop; else
         * try reading more (may EAGAIN → done for now). */
    }
}

int ray_poll_serve_tx(ray_poll_t* poll, int64_t id)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (!sel) return -1;
    if (ray_poll_tx_flush(poll, sel) < 0) {
        sel = ray_poll_get(poll, id);
        if (sel) sel_fail(poll, sel);
        return -1;
    }
    return 0;
}

/* The event_fd of a transport became readable: bytes landed or ring
 * space came back.  Serve the connection it belongs to both ways. */
static ray_t* xport_wake(ray_poll_t* poll, ray_selector_t* sel)
{
    int64_t id = (int64_t)(intptr_t)sel->data;
    ray_selector_t* conn = ray_poll_get(poll, id);
    if (!conn || !conn->xport) return NULL;
    conn->xport->ops->woke(conn->xport);
    if (conn->tx.buf && ray_poll_serve_tx(poll, id) < 0) return NULL;
    ray_poll_serve_rx(poll, id);
    return NULL;
}

int ray_poll_xport_attach(ray_poll_t* poll, ray_selector_t* sel, ray_xport_t* xport)
{
    if (!poll || !sel || !xport || sel->xport) return -1;
    ray_poll_reg_t reg = {0};
    reg.fd      = xport->event_fd;
    reg.type    = RAY_SEL_XPORT;
    reg.read_fn = xport_wake;
    reg.data    = (void*)(intptr_t)sel->id;
    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) return -1;
    sel->xport    = xport;
    sel->xport_id = id;
    poll->n_xport++;
    /* The socket's write readiness means nothing now: room is announced
     * through event_fd.  Re-derive its interest set (drops EPOLLOUT). */
    ray_poll_tx_cancel(poll, sel);
    return 0;
}

/* Called by a backend's deregister: the transport and its wake selector
 * go with the connection. */
void ray_poll_xport_detach(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->xport) return;
    ray_xport_t* x = sel->xport;
    sel->xport = NULL;
    if (poll->n_xport > 0) poll->n_xport--;
    ray_poll_deregister(poll, sel->xport_id);
    x->ops->destroy(x);
}

int ray_poll_tx_flush(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!poll || !sel || (!sel->tx.send_fn && !sel->xport)) return -1;

    while (sel->tx.buf) {
        ray_poll_buf_t* buf = sel->tx.buf;
        while (buf->offset < buf->size) {
            int64_t nw = ray_poll_sel_send(sel,
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

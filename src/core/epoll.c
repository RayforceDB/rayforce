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

#if defined(__linux__)

#include "core/poll.h"
#include "mem/sys.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define RAY_POLL_MAX_EVENTS 64
#define RAY_POLL_INITIAL_CAP 16

ray_poll_t* ray_poll_create(void)
{
    int fd = epoll_create1(0);
    if (fd < 0) return NULL;

    ray_poll_t* poll = (ray_poll_t*)ray_sys_alloc(sizeof(ray_poll_t));
    if (!poll) { close(fd); return NULL; }

    memset(poll, 0, sizeof(*poll));
    poll->fd      = fd;
    poll->code    = -1;
    poll->sel_cap = RAY_POLL_INITIAL_CAP;
    poll->sels    = (ray_selector_t**)ray_sys_alloc(
                        poll->sel_cap * sizeof(ray_selector_t*));
    if (!poll->sels) {
        close(fd);
        ray_sys_free(poll);
        return NULL;
    }
    memset(poll->sels, 0, poll->sel_cap * sizeof(ray_selector_t*));
    return poll;
}

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;

    /* Deregister all selectors */
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;
        if (sel->close_fn) sel->close_fn(poll, sel);
        epoll_ctl((int)poll->fd, EPOLL_CTL_DEL, (int)sel->fd, NULL);
        if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        ray_sys_free(sel);
        poll->sels[i] = NULL;
    }

    if (poll->sels) ray_sys_free(poll->sels);
    close((int)poll->fd);
    ray_sys_free(poll);
}

int64_t ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg)
{
    if (!poll || !reg) return -1;

    /* Find free slot or grow */
    int64_t id = -1;
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        if (!poll->sels[i]) { id = (int64_t)i; break; }
    }
    if (id < 0) {
        if (poll->n_sels >= poll->sel_cap) {
            uint32_t new_cap = poll->sel_cap * 2;
            ray_selector_t** ns = (ray_selector_t**)ray_sys_alloc(
                new_cap * sizeof(ray_selector_t*));
            if (!ns) return -1;
            memcpy(ns, poll->sels, poll->n_sels * sizeof(ray_selector_t*));
            memset(ns + poll->n_sels, 0,
                   (new_cap - poll->n_sels) * sizeof(ray_selector_t*));
            ray_sys_free(poll->sels);
            poll->sels    = ns;
            poll->sel_cap = new_cap;
        }
        id = (int64_t)poll->n_sels;
        poll->n_sels++;
    }

    ray_selector_t* sel = (ray_selector_t*)ray_sys_alloc(sizeof(ray_selector_t));
    if (!sel) return -1;
    memset(sel, 0, sizeof(*sel));

    sel->fd       = reg->fd;
    sel->id       = id;
    sel->type     = reg->type;
    sel->data     = reg->data;
    sel->open_fn  = reg->open_fn;
    sel->close_fn = reg->close_fn;
    sel->error_fn = reg->error_fn;
    sel->data_fn  = reg->data_fn;
    sel->rx.recv_fn = reg->recv_fn;
    sel->rx.read_fn = reg->read_fn;
    sel->tx.send_fn = reg->send_fn;

    poll->sels[id] = sel;

    /* Register with epoll */
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.u64 = (uint64_t)id;

    if (epoll_ctl((int)poll->fd, EPOLL_CTL_ADD, (int)reg->fd, &ev) < 0) {
        poll->sels[id] = NULL;
        ray_sys_free(sel);
        return -1;
    }

    if (sel->open_fn) sel->open_fn(poll, sel);
    return id;
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;

    epoll_ctl((int)poll->fd, EPOLL_CTL_DEL, (int)sel->fd, NULL);
    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);
    poll->sels[id] = NULL;
}

int64_t ray_poll_run(ray_poll_t* poll)
{
    if (!poll) return -1;

    struct epoll_event events[RAY_POLL_MAX_EVENTS];

    while (poll->code < 0) {
        int n = epoll_wait((int)poll->fd, events, RAY_POLL_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < n; i++) {
            uint64_t eid = events[i].data.u64;
            ray_selector_t* sel = NULL;

            if (eid < poll->n_sels)
                sel = poll->sels[eid];
            if (!sel) continue;

            /* Process readable data first — even if hangup is also set.
             * A client may send a message and close; epoll reports both
             * EPOLLIN and EPOLLHUP in the same event. */
            if (events[i].events & EPOLLIN) {
                /* Loop: read data → call read_fn → if state advanced,
                 * read more. Handles multi-phase protocols (handshake →
                 * header → payload) arriving in a single epoll event. */
                for (;;) {
                    /* Fill rx buffer */
                    if (sel->rx.recv_fn && sel->rx.buf) {
                        while (sel->rx.buf->offset < sel->rx.buf->size) {
                            int64_t nr = sel->rx.recv_fn(
                                sel->fd,
                                sel->rx.buf->data + sel->rx.buf->offset,
                                sel->rx.buf->size - sel->rx.buf->offset);
                            if (nr <= 0) {
                                if (nr < 0 && errno == EINTR) continue;
                                if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                                    break;
                                /* Error or peer closed mid-read */
                                if (sel->error_fn)
                                    sel->error_fn(poll, sel);
                                else
                                    ray_poll_deregister(poll, sel->id);
                                goto next_event;
                            }
                            sel->rx.buf->offset += nr;
                        }
                    }

                    /* Not enough data for current phase */
                    if (sel->rx.buf && sel->rx.buf->offset < sel->rx.buf->size)
                        break;

                    /* Call read_fn — may advance state and request new buffer */
                    if (!sel->rx.read_fn) break;
                    ray_t* obj = sel->rx.read_fn(poll, sel);

                    /* Re-validate: read_fn may have deregistered this selector */
                    if (eid >= poll->n_sels || !poll->sels[eid]) goto next_event;
                    sel = poll->sels[eid];

                    if (obj && sel->data_fn)
                        sel->data_fn(poll, sel, obj);

                    /* If data_fn deregistered the selector, stop */
                    if (eid >= poll->n_sels || !poll->sels[eid]) goto next_event;
                    sel = poll->sels[eid];

                    /* If no rx buffer (state machine done or not set), stop */
                    if (!sel->rx.buf) break;
                    /* If buffer already has enough data for next phase, loop */
                    if (sel->rx.buf->offset >= sel->rx.buf->size) continue;
                    /* Otherwise try reading more (may EAGAIN → break) */
                }
            }

            /* Error / hangup — after data is drained */
            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                /* Re-check: selector may have been freed by data_fn */
                if (eid < poll->n_sels && poll->sels[eid]) {
                    sel = poll->sels[eid];
                    if (sel->error_fn)
                        sel->error_fn(poll, sel);
                    else
                        ray_poll_deregister(poll, sel->id);
                }
            }

        next_event:;
        }
    }

    return poll->code;
}

#endif /* __linux__ */

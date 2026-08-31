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

#if defined(__APPLE__)

#include "core/poll.h"
#include "core/timer.h"
#include "mem/sys.h"
#include "mem/heap.h"   /* idle decay: bound the wait, sweep after wakeup */
#include <sys/event.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define RAY_POLL_MAX_EVENTS 64
#define RAY_POLL_INITIAL_CAP 16

ray_poll_t* ray_poll_create(void)
{
    int fd = kqueue();
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

    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;
        if (sel->close_fn) sel->close_fn(poll, sel);
        struct kevent kev;
        EV_SET(&kev, (uintptr_t)sel->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent((int)poll->fd, &kev, 1, NULL, 0, NULL);
        if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        ray_sys_free(sel);
        poll->sels[i] = NULL;
    }

    if (poll->sels) ray_sys_free(poll->sels);
    close((int)poll->fd);
    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }
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

    /* Register with kqueue */
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)reg->fd, EVFILT_READ, EV_ADD, 0, 0,
           (void*)(uintptr_t)id);

    if (kevent((int)poll->fd, &kev, 1, NULL, 0, NULL) < 0) {
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

    struct kevent kev;
    EV_SET(&kev, (uintptr_t)sel->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent((int)poll->fd, &kev, 1, NULL, 0, NULL);

    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);
    poll->sels[id] = NULL;
}

int64_t ray_poll_run_for(ray_poll_t* poll, int timeout_ms)
{
    if (!poll) return -1;

    struct kevent events[RAY_POLL_MAX_EVENTS];
    bool bounded = timeout_ms >= 0;
    int64_t end_ms = bounded ? ray_time_now_ms() + timeout_ms : INT64_MAX;

    while (poll->code < 0) {
        int64_t wait_ms = -1;
        if (bounded) {
            int64_t remaining = end_ms - ray_time_now_ms();
            if (remaining < 0) remaining = 0;
            wait_ms = remaining;
        }

        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms(
                (ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t now = ray_time_now_ms();
                int64_t delta = deadline - now;
                if (delta < 0) delta = 0;
                if (wait_ms < 0 || delta < wait_ms)
                    wait_ms = delta;
            }
        }

        /* A process waiting here is a process doing nothing, and this is the
         * only moment it gets to notice.  Bound the wait by the allocator's
         * pending decay exactly as a timer would; once the decay has run
         * there is nothing pending and the wait goes back to indefinite.
         *
         * Only when the loop is unbounded.  ray_poll_run_for carries the
         * caller's own deadline and runs a single pass, so shortening its
         * wait here returns from it early with nothing done — a timer the
         * caller was waiting on would not have fired yet. */
        if (!bounded) {
            int64_t decay = ray_heap_decay_due_ms();
            if (decay >= 0 && (wait_ms < 0 || decay < wait_ms))
                wait_ms = decay;
        }

        struct timespec  ts;
        struct timespec* timeout = NULL;
        if (wait_ms >= 0) {
            ts.tv_sec  = (time_t)(wait_ms / 1000);
            ts.tv_nsec = (long)((wait_ms % 1000) * 1000000L);
            timeout = &ts;
        }

        int n = kevent((int)poll->fd, NULL, 0, events,
                       RAY_POLL_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        for (int i = 0; i < n; i++) {
            uint64_t eid = (uint64_t)(uintptr_t)events[i].udata;
            ray_selector_t* sel = NULL;

            if (eid < poll->n_sels)
                sel = poll->sels[eid];
            if (!sel) continue;

            /* EV_ERROR without data — fatal, skip directly */
            if ((events[i].flags & EV_ERROR) && events[i].filter != EVFILT_READ) {
                if (sel->error_fn)
                    sel->error_fn(poll, sel);
                else
                    ray_poll_deregister(poll, sel->id);
                continue;
            }

            /* Process readable data first — even if EOF is also set.
             * A client may send a message and close simultaneously. */
            if (events[i].filter == EVFILT_READ) {
                for (;;) {
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
                                if (sel->error_fn)
                                    sel->error_fn(poll, sel);
                                else
                                    ray_poll_deregister(poll, sel->id);
                                goto next_event;
                            }
                            sel->rx.buf->offset += nr;
                        }
                    }
                    if (sel->rx.buf && sel->rx.buf->offset < sel->rx.buf->size)
                        break;
                    if (!sel->rx.read_fn) break;
                    ray_t* obj = sel->rx.read_fn(poll, sel);

                    /* Re-validate: read_fn may have deregistered this selector */
                    if (eid >= poll->n_sels || !poll->sels[eid]) goto next_event;
                    sel = poll->sels[eid];

                    if (obj && sel->data_fn)
                        sel->data_fn(poll, sel, obj);
                    if (eid >= poll->n_sels || !poll->sels[eid]) goto next_event;
                    sel = poll->sels[eid];
                    if (!sel->rx.buf) break;
                    if (sel->rx.buf->offset >= sel->rx.buf->size) continue;
                }
            }

            /* EOF / error — after data is drained */
            if (events[i].flags & (EV_EOF | EV_ERROR)) {
                if (eid < poll->n_sels && poll->sels[eid]) {
                    sel = poll->sels[eid];
                    if (sel->type == RAY_SEL_STDIN) goto next_event; /* Ctrl-D handled by read_fn */
                    if (sel->error_fn)
                        sel->error_fn(poll, sel);
                    else
                        ray_poll_deregister(poll, sel->id);
                }
            }

        next_event:;
        }

        if (poll->timers) {
            /* A fired timer is work, and on a timer-driven server it is the
             * only work there is: without this stamp the decay sees an idle
             * process mid-workload, and — having disarmed itself — never
             * looks again. */
            if (ray_timers_fire_expired((ray_timers_t*)poll->timers) > 0)
                ray_heap_note_activity();
        }
        /* After the events of this wakeup, not before: a request handled
         * above has just re-stamped the activity clock, so a busy loop
         * finds nothing due and pays one timestamp comparison. */
        ray_heap_decay();
        if (bounded) break;
    }

    return poll->code >= 0 ? poll->code : 0;
}

int64_t ray_poll_run(ray_poll_t* poll)
{
    return ray_poll_run_for(poll, -1);
}

#endif /* __APPLE__ */

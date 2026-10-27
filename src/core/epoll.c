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
#define _GNU_SOURCE
#endif

#if defined(__linux__)

#include "core/poll.h"
#include "core/mcast.h"
#include "core/timer.h"
#include "mem/sys.h"
#include "mem/heap.h"   /* idle decay: bound the wait, sweep after wakeup */
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#define RAY_POLL_MAX_EVENTS 64
#define RAY_POLL_INITIAL_CAP 16

static uint32_t epoll_selector_events(ray_selector_t* sel)
{
    uint32_t events = EPOLLIN | EPOLLRDHUP;
    /* An upgraded connection's socket is always writable and carries
     * nothing: waiting on EPOLLOUT for its queue would spin the loop while
     * the ring is full.  Its transport announces room on event_fd. */
    if (sel && sel->tx.buf && !sel->xport) events |= EPOLLOUT;
    return events;
}

static void epoll_selector_mod(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!poll || !sel) return;
    struct epoll_event ev;
    ev.events  = epoll_selector_events(sel);
    ev.data.u64 = (uint64_t)sel->id;
    epoll_ctl((int)poll->fd, EPOLL_CTL_MOD, (int)sel->fd, &ev);
}

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
        /* A transport goes with its connection; its wake selector is
         * freed by this same loop (the transport closes the descriptor). */
        if (sel->xport) {
            ray_xport_t* x = sel->xport;
            sel->xport = NULL;
            x->ops->destroy(x);
        }
        epoll_ctl((int)poll->fd, EPOLL_CTL_DEL, (int)sel->fd, NULL);
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
    if (poll->mcast) {
        ray_mcast_destroy((ray_mcast_t*)poll->mcast);
        poll->mcast = NULL;
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

    /* Register with epoll */
    struct epoll_event ev;
    ev.events  = epoll_selector_events(sel);
    ev.data.u64 = (uint64_t)id;

    if (epoll_ctl((int)poll->fd, EPOLL_CTL_ADD, (int)reg->fd, &ev) < 0) {
        poll->sels[id] = NULL;
        ray_sys_free(sel);
        return -1;
    }

    poll->n_live++;
    if (sel->open_fn) sel->open_fn(poll, sel);
    return id;
}

void ray_poll_tx_request(ray_poll_t* poll, ray_selector_t* sel)
{
    epoll_selector_mod(poll, sel);
}

void ray_poll_tx_cancel(ray_poll_t* poll, ray_selector_t* sel)
{
    epoll_selector_mod(poll, sel);
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;

    epoll_ctl((int)poll->fd, EPOLL_CTL_DEL, (int)sel->fd, NULL);
    ray_poll_xport_detach(poll, sel);
    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);
    poll->sels[id] = NULL;
    if (poll->n_live > 0) poll->n_live--;
}

/* ===== Transports: what no descriptor announces =====
 *
 * Bytes land in a transport's ring without a wakeup unless the peer was
 * told the loop sleeps.  So before each wait: serve what landed while the
 * loop was busy, spin a moment for what is about to (an answer within it
 * costs neither side a system call), then raise each transport's sleeping
 * flag.  A transport with news in the gap between the look and the flag
 * cancels the sleep. */

#define RAY_XPORT_SPIN_NS 20000   /* 20 µs */

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Serve every transport connection with bytes to read or room for its
 * queue; true when any was served. */
static bool xport_serve(ray_poll_t* poll)
{
    bool served = false;
    for (uint32_t i = 0; i < poll->n_sels && poll->code < 0; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel || !sel->xport) continue;
        ray_xport_t* x = sel->xport;
        if (sel->tx.buf && x->ops->room(x)) {
            served = true;
            if (ray_poll_serve_tx(poll, (int64_t)i) < 0) continue;
        }
        if (x->ops->pending(x)) {
            served = true;
            ray_poll_serve_rx(poll, (int64_t)i);
        }
    }
    return served;
}

static bool xport_news(ray_poll_t* poll)
{
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel || !sel->xport) continue;
        ray_xport_t* x = sel->xport;
        if (x->ops->pending(x) || (sel->tx.buf && x->ops->room(x))) return true;
    }
    return false;
}

static void xport_woke(ray_poll_t* poll)
{
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (sel && sel->xport) sel->xport->ops->woke(sel->xport);
    }
}

/* The wait the loop may take after this pass: -1 as planned, 0 when a
 * transport was served (the socket events of this round are still owed a
 * look), 1 ms when a transport with queued output cannot announce room. */
static int xport_pass(ray_poll_t* poll)
{
    if (xport_serve(poll)) return 0;
    int64_t until = now_ns() + RAY_XPORT_SPIN_NS;
    do {
        if (xport_news(poll)) { xport_serve(poll); return 0; }
    } while (now_ns() < until);
    bool news = false, queued = false;
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel || !sel->xport) continue;
        ray_xport_t* x = sel->xport;
        bool want_room = sel->tx.buf != NULL;
        if (!x->ops->prepare_sleep(x, true, want_room)) news = true;
        if (want_room && !x->fd_tells_room) queued = true;
    }
    if (news) { xport_woke(poll); return 0; }
    return queued ? 1 : -1;
}

int64_t ray_poll_run_for(ray_poll_t* poll, int timeout_ms)
{
    if (!poll) return -1;

    struct epoll_event events[RAY_POLL_MAX_EVENTS];
    bool bounded = timeout_ms >= 0;
    int64_t end_ms = bounded ? ray_time_now_ms() + timeout_ms : INT64_MAX;

    while (poll->code < 0) {
        int wait_ms = -1;
        if (bounded) {
            int64_t remaining = end_ms - ray_time_now_ms();
            if (remaining < 0) remaining = 0;
            if (remaining > INT_MAX) remaining = INT_MAX;
            wait_ms = (int)remaining;
        }

        /* Nothing registered and nothing scheduled: an unbounded loop
         * would block here forever.  Return instead, so a process that
         * stayed only for its timers can end once they are spent. */
        if (!bounded && ray_poll_idle(poll)) return 0;

        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms(
                (ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t now = ray_time_now_ms();
                int64_t delta = deadline - now;
                if (delta < 0) delta = 0;
                if (delta > INT_MAX) delta = INT_MAX;
                if (wait_ms < 0 || delta < wait_ms)
                    wait_ms = (int)delta;
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
        {
            int64_t decay = bounded ? -1 : ray_heap_decay_due_ms();
            if (decay >= 0) {
                if (decay > INT_MAX) decay = INT_MAX;
                if (wait_ms < 0 || decay < wait_ms) wait_ms = (int)decay;
            }
        }

        if (poll->n_xport > 0) {
            int w = xport_pass(poll);
            if (w >= 0 && (wait_ms < 0 || w < wait_ms)) wait_ms = w;
        }

        int n = epoll_wait((int)poll->fd, events, RAY_POLL_MAX_EVENTS, wait_ms);
        if (poll->n_xport > 0) xport_woke(poll);
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
                if (ray_poll_serve_rx(poll, (int64_t)eid) < 0) goto next_event;
            }

            if (events[i].events & EPOLLOUT) {
                if (ray_poll_serve_tx(poll, (int64_t)eid) < 0) goto next_event;
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

        if (poll->timers) {
            /* A fired timer is work, and on a timer-driven server it is the
             * only work there is: without this stamp the decay sees an idle
             * process mid-workload, and — having disarmed itself — never
             * looks again.  It is also a unit of work ending, so the same
             * boundary relief as after an IPC request applies. */
            if (ray_timers_fire_expired((ray_timers_t*)poll->timers) > 0) {
                ray_heap_note_activity();
                ray_heap_relieve();
            }
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

#endif /* __linux__ */

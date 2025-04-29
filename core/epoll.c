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

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include "poll.h"
#include "heap.h"

__thread i32_t __EVENT_FD;  // eventfd to notify epoll loop of shutdown

#define INITIAL_RX_BUFFER_SIZE 16

nil_t sigint_handler(i32_t signo) {
    i64_t val = 1;
    i32_t res;

    UNUSED(signo);

    // Write to the eventfd to wake up the epoll loop.
    res = write(__EVENT_FD, &val, sizeof(val));
    UNUSED(res);
}

poll_p poll_create() {
    i64_t fd;
    poll_p poll;
    struct epoll_event ev;

    fd = epoll_create1(0);
    if (fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // Add eventfd
    __EVENT_FD = eventfd(0, 0);
    if (__EVENT_FD == -1) {
        perror("eventfd");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN;
    ev.data.fd = __EVENT_FD;
    if (epoll_ctl(fd, EPOLL_CTL_ADD, __EVENT_FD, &ev) == -1) {
        perror("epoll_ctl: eventfd");
        exit(EXIT_FAILURE);
    }

    // Set up the SIGINT signal handler
    signal(SIGINT, sigint_handler);

    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;
    poll->fd = fd;
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(16);

    return poll;
}

nil_t poll_destroy(poll_p poll) {
    i64_t i, l;

    // Free all selectors
    l = poll->selectors->data_pos;
    for (i = 0; i < l; i++) {
        if (poll->selectors->data[i] != NULL_I64)
            poll_deregister(poll, i + SELECTOR_ID_OFFSET);
    }

    freelist_free(poll->selectors);
    timers_destroy(poll->timers);

    close(__EVENT_FD);
    close(poll->fd);
    heap_free(poll);
}

poll_result_t poll_register(poll_p poll, poll_registry_p registry) {
    i64_t id;
    selector_p selector;
    struct epoll_event ev;

    selector = (selector_p)heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->fd = registry->fd;
    selector->type = registry->type;
    selector->open_fn = registry->open_fn;
    selector->close_fn = registry->close_fn;
    selector->error_fn = registry->error_fn;
    selector->rx.read_fn = registry->read_fn;
    selector->tx.write_fn = registry->write_fn;
    selector->rx.recv_fn = registry->recv_fn;
    selector->tx.send_fn = registry->send_fn;
    selector->data = registry->data;
    selector->tx.isset = B8_FALSE;
    selector->rx.buf = NULL;
    selector->tx.buf = NULL;

    ev.events = registry->events;
    ev.data.ptr = selector;

    if (epoll_ctl(poll->fd, EPOLL_CTL_ADD, selector->fd, &ev) == -1) {
        perror("epoll_ctl: add");
        return POLL_ERROR;
    }

    if (registry->open_fn != NULL)
        registry->open_fn(poll, selector);

    return id;
}

poll_result_t poll_deregister(poll_p poll, i64_t id) {
    i64_t idx;
    selector_p selector;
    poll_buffer_p buf;

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return POLL_ERROR;

    selector = (selector_p)idx;

    if (selector->close_fn != NULL)
        selector->close_fn(poll, selector);

    epoll_ctl(poll->fd, EPOLL_CTL_DEL, selector->fd, NULL);

    if (selector->rx.buf != NULL) {
        heap_free(selector->rx.buf);
        selector->rx.buf = NULL;
    }

    while (selector->tx.buf != NULL) {
        buf = selector->tx.buf->next;
        heap_free(selector->tx.buf);
        selector->tx.buf = buf;
    }

    heap_free(selector);

    return POLL_OK;
}

poll_result_t poll_recv(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size, total;

    total = selector->rx.buf->offset;
    do {
        size = selector->rx.recv_fn(selector->fd, &selector->rx.buf->data[selector->rx.buf->offset],
                                    selector->rx.buf->size - selector->rx.buf->offset);

        if (size == -1)
            return POLL_ERROR;
        else if (size == 0)
            return POLL_OK;

        selector->rx.buf->offset += size;
    } while (selector->rx.buf->offset < selector->rx.buf->size);

    total = selector->rx.buf->offset - total;

    return total;
}

poll_result_t poll_send(poll_p poll, selector_p selector) {
    i64_t size, total;
    poll_buffer_p buf;
    struct epoll_event ev;

    total = 0;

send_loop:
    while (selector->tx.buf->offset < selector->tx.buf->size) {
        size = selector->tx.send_fn(selector->fd, &selector->tx.buf->data[selector->tx.buf->offset],
                                    selector->tx.buf->size - selector->tx.buf->offset);
        if (size == POLL_ERROR)
            return POLL_ERROR;
        else if (size == POLL_OK) {
            // setup epoll for write event only if it's not already set
            if (!selector->tx.isset) {
                selector->tx.isset = B8_TRUE;
                ev.events |= POLL_EVENT_WRITE;
                ev.data.ptr = selector;
                if (epoll_ctl(poll->fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
                    return POLL_ERROR;
            }

            return POLL_OK;
        }

        selector->tx.buf->offset += size;
        total += size;
    }

    // reset tx buffer
    buf = selector->tx.buf->next;
    heap_free(selector->tx.buf);
    selector->tx.buf = buf;

    if (selector->tx.buf != NULL)
        goto send_loop;

    // remove write event only if it's set
    if (selector->tx.isset) {
        selector->tx.isset = B8_FALSE;
        ev.events &= ~POLL_EVENT_WRITE;
        ev.data.ptr = selector;
        if (epoll_ctl(poll->fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
            return POLL_ERROR;
    }

    return total;
}

poll_result_t poll_run(poll_p poll) {
    i64_t n, nfds, timeout;
    poll_result_t poll_result;
    selector_p selector;
    struct epoll_event ev, events[MAX_EVENTS];

    while (poll->code == NULL_I64) {
        timeout = timer_next_timeout(poll->timers);
        nfds = epoll_wait(poll->fd, events, MAX_EVENTS, timeout);

        if (nfds == -1) {
            if (errno == EINTR)
                continue;

            return POLL_ERROR;
        }

    process_events:
        for (n = 0; n < nfds; n++) {
            ev = events[n];

            // shutdown
            if (ev.data.fd == __EVENT_FD) {
                poll->code = 0;
                break;
            }

            selector = (selector_p)ev.data.ptr;

            // error or hang up
            if ((ev.events & POLL_EVENT_ERROR) || (ev.events & POLL_EVENT_HUP)) {
                poll_deregister(poll, selector->id);
                continue;
            }

            // read
            if (ev.events & POLL_EVENT_READ) {
                poll_result = POLL_READY;

                do {
                    // In case we have a low level IO recv function, use it
                    if (selector->rx.recv_fn != NULL) {
                        poll_result = poll_recv(poll, selector);

                        if (poll_result == POLL_ERROR) {
                            poll_deregister(poll, selector->id);
                            goto process_events;
                        }

                        if (poll_result == POLL_OK)
                            break;
                    }

                    if (POLL_IS_READY(poll_result) && selector->rx.read_fn != NULL)
                        poll_result = selector->rx.read_fn(poll, selector);

                    if (poll_result == POLL_ERROR)
                        poll_deregister(poll, selector->id);

                } while (poll_result == POLL_READY);
            }

            // write
            if (ev.events & POLL_EVENT_WRITE) {
                do {
                    poll_result = poll_send(poll, selector);

                    if (poll_result == POLL_ERROR) {
                        poll_deregister(poll, selector->id);
                        goto process_events;
                    }

                    if (poll_result == POLL_OK)
                        break;

                } while (selector->tx.buf != NULL);
            }
        }
    }

    return poll->code;
}

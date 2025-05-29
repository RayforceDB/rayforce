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
#include <fcntl.h>
#include "poll.h"
#include "heap.h"
#include "log.h"

__thread i32_t __EVENT_FD;  // eventfd to notify epoll loop of shutdown

#define INITIAL_RX_BUFFER_SIZE 16

nil_t sigint_handler(i32_t signo) {
    i64_t val = 1;
    i32_t res;

    UNUSED(signo);

    LOG_TRACE("Writing to eventfd to wake up the epoll loop");

    // Write to the eventfd to wake up the epoll loop.
    res = write(__EVENT_FD, &val, sizeof(val));
    UNUSED(res);
}

poll_p poll_create() {
    i64_t fd;
    poll_p poll;
    struct epoll_event ev;

    LOG_DEBUG("Creating epoll instance");

    fd = epoll_create1(0);
    if (fd == -1) {
        LOG_ERROR("Failed to create epoll instance");
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    LOG_DEBUG("Creating eventfd");

    // Add eventfd
    __EVENT_FD = eventfd(0, 0);
    if (__EVENT_FD == -1) {
        LOG_ERROR("Failed to create eventfd");
        perror("eventfd");
        exit(EXIT_FAILURE);
    }

    LOG_DEBUG("Adding eventfd to epoll");
    ev.events = EPOLLIN;
    ev.data.fd = __EVENT_FD;
    if (epoll_ctl(fd, EPOLL_CTL_ADD, __EVENT_FD, &ev) == -1) {
        LOG_ERROR("Failed to add eventfd to epoll");
        perror("epoll_ctl: eventfd");
        exit(EXIT_FAILURE);
    }

    LOG_DEBUG("Setting up the SIGINT signal handler");
    signal(SIGINT, sigint_handler);

    LOG_DEBUG("Creating poll instance");
    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;
    poll->fd = fd;
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(16);

    LOG_DEBUG("Poll instance created");

    return poll;
}

nil_t poll_destroy(poll_p poll) {
    i64_t i, l;

    // Free all selectors
    LOG_DEBUG("Freeing all selectors");
    l = poll->selectors->data_pos;
    for (i = 0; i < l; i++) {
        if (poll->selectors->data[i] != NULL_I64)
            poll_deregister(poll, i + SELECTOR_ID_OFFSET);
    }

    LOG_DEBUG("Freeing selectors list");
    freelist_free(poll->selectors);
    timers_destroy(poll->timers);

    LOG_DEBUG("Closing eventfd");
    close(__EVENT_FD);

    LOG_DEBUG("Closing epoll instance");
    close(poll->fd);

    LOG_DEBUG("Freeing poll instance");
    heap_free(poll);
}

i64_t poll_register(poll_p poll, poll_registry_p registry) {
    i64_t id;
    selector_p selector;
    struct epoll_event ev;

    LOG_DEBUG("Registering selector");

    selector = (selector_p)heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->fd = registry->fd;
    selector->type = registry->type;
    selector->interest = registry->events;
    selector->open_fn = registry->open_fn;
    selector->close_fn = registry->close_fn;
    selector->error_fn = registry->error_fn;
    selector->rx.read_fn = registry->read_fn;
    selector->tx.write_fn = registry->write_fn;
    selector->rx.recv_fn = registry->recv_fn;
    selector->tx.send_fn = registry->send_fn;
    selector->data_fn = registry->data_fn;
    selector->data = registry->data;
    selector->rx.buf = NULL;
    selector->tx.buf = NULL;

    LOG_DEBUG("Setting up epoll event");

    ev.events = selector->interest;
    ev.data.ptr = selector;

    if (epoll_ctl(poll->fd, EPOLL_CTL_ADD, selector->fd, &ev) == -1) {
        perror("epoll_ctl: add");
        return -1;
    }

    LOG_DEBUG("Calling open function");

    if (registry->open_fn != NULL)
        registry->open_fn(poll, selector);

    LOG_DEBUG("Selector %lld: registered", selector->id);

    return id;
}

i64_t poll_deregister(poll_p poll, i64_t id) {
    i64_t idx;
    selector_p selector;
    poll_buffer_p buf;

    LOG_DEBUG("Deregistering selector");

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return -1;

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

    LOG_DEBUG("Freeing selector");

    heap_free(selector);

    return 0;
}

i64_t poll_recv(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size, total;

    LOG_TRACE("Receiving data from selector %lld", selector->id);

    total = selector->rx.buf->offset;

    LOG_TRACE("RX BUFFER: %p, size: %lld, offset: %lld", selector->rx.buf, selector->rx.buf->size,
              selector->rx.buf->offset);

    while (selector->rx.buf->offset < selector->rx.buf->size) {
        LOG_DEBUG("buf size, offset: %lld, %lld", selector->rx.buf->size, selector->rx.buf->offset);
        LOG_DEBUG("READ SIZE: %lld", selector->rx.buf->size - selector->rx.buf->offset);
        size = selector->rx.recv_fn(selector->fd, &selector->rx.buf->data[selector->rx.buf->offset],
                                    selector->rx.buf->size - selector->rx.buf->offset);

        LOG_TRACE("Received %lld bytes from selector %lld", size, selector->id);

        if (size == -1) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (size == 0) {
            // closed
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;

            return 0;
        }

        selector->rx.buf->offset += size;
    }

    total = selector->rx.buf->offset - total;

    LOG_TRACE("Total bytes received from selector %lld: %lld", selector->id, total);

    return total;
}

i64_t poll_send(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size, total;
    poll_buffer_p buf;

    LOG_TRACE("Sending data to selector %lld", selector->id);

    total = 0;

send_loop:
    while (selector->tx.buf->offset < selector->tx.buf->size) {
        size = selector->tx.send_fn(selector->fd, selector->tx.buf->data + selector->tx.buf->offset,
                                    selector->tx.buf->size - selector->tx.buf->offset);

        LOG_TRACE("Sent %lld bytes to selector %lld", size, selector->id);

        if (size == -1) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (size == 0) {
            // closed
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;

            return 0;
        }

        selector->tx.buf->offset += size;
    }

    total = selector->tx.buf->offset;

    // switch to next buffer
    LOG_TRACE("Switching to next buffer");
    buf = selector->tx.buf->next;
    heap_free(selector->tx.buf);
    selector->tx.buf = buf;

    if (selector->tx.buf != NULL)
        goto send_loop;

    LOG_TRACE("Total bytes sent to selector %lld: %lld", selector->id, total);

    return total;
}

i64_t poll_run(poll_p poll) {
    i64_t n, nbytes, nfds, timeout;
    option_t poll_result;
    selector_p selector;
    struct epoll_event ev, events[MAX_EVENTS];

    LOG_DEBUG("Running poll");

    while (poll->code == NULL_I64) {
        LOG_TRACE("Waiting for events");

        timeout = timer_next_timeout(poll->timers);
        nfds = epoll_wait(poll->fd, events, MAX_EVENTS, timeout);

        if (nfds == -1) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        for (n = 0; n < nfds; n++) {
            LOG_TRACE("Event %lld: %lld", n, nfds);
            ev = events[n];

            // shutdown
            if (ev.data.fd == __EVENT_FD) {
                LOG_DEBUG("Shutdown event received");
                poll->code = 0;
                break;
            }

            selector = (selector_p)ev.data.ptr;

            // error or hang up
            if ((ev.events & POLL_EVENT_ERROR) || (ev.events & POLL_EVENT_RDHUP)) {
                LOG_DEBUG("Connection error or hangup for selector %lld, events: 0x%llx", selector->id, ev.events);
                poll_deregister(poll, selector->id);
                continue;
            }

            // read
            if (ev.events & POLL_EVENT_READ) {
                LOG_TRACE("Read event received for selector %lld, events: 0x%llx", selector->id, ev.events);
                while (B8_TRUE) {
                    // In case we have a low level IO recv function, use it
                    nbytes = 0;
                    if (selector->rx.recv_fn != NULL) {
                        nbytes = poll_recv(poll, selector);

                        if (nbytes == -1) {
                            // Error or connection closed
                            LOG_DEBUG("Error or connection closed for selector %lld", selector->id);
                            poll_deregister(poll, selector->id);
                            goto next_event;
                        }

                        if (nbytes == 0) {
                            // No more data to read (EAGAIN/EWOULDBLOCK), wait for next edge trigger
                            goto next_event;
                        }
                    }

                    poll_result = option_none();

                    if (selector->rx.read_fn != NULL)
                        poll_result = selector->rx.read_fn(poll, selector);

                    if (option_is_some(&poll_result)) {
                        if (poll_result.value != NULL && selector->data_fn != NULL)
                            poll_result = selector->data_fn(poll, selector, poll_result.value);

                        if (option_is_some(&poll_result))
                            continue;
                    }

                    if (option_is_error(&poll_result))
                        poll_deregister(poll, selector->id);

                    break;
                }
            }

            // write
            if (ev.events & POLL_EVENT_WRITE) {
                LOG_TRACE("Write event received for selector %lld", selector->id);
                nbytes = 0;
                while (selector->tx.buf != NULL) {
                    nbytes = poll_send(poll, selector);

                    if (nbytes == -1) {
                        poll_deregister(poll, selector->id);
                        goto next_event;
                    }

                    if (nbytes == 0)
                        break;
                }
            }

        next_event:;
        }
    }

    return poll->code;
}

option_t poll_block_on(poll_p poll, selector_p selector) {
    option_t result;
    fd_set readfds;
    struct timeval timeout;
    i64_t nbytes, ret;

    LOG_TRACE("Blocking on selector id: %lld, fd: %lld", selector->id, selector->fd);

    // Setup select
    FD_ZERO(&readfds);
    FD_SET(selector->fd, &readfds);
    timeout.tv_sec = 0;
    // timeout.tv_usec = 30000000;  // 30 seconds
    timeout.tv_usec = 3000000;

    // Perform the read operation
    while (selector->rx.buf != NULL) {
        // Wait for the file descriptor to become readable
        ret = select(selector->fd + 1, &readfds, NULL, NULL, &timeout);
        if (ret == -1) {
            LOG_ERROR("select failed");
            return option_error(sys_error(ERR_IO, "recv select failed"));
        }

        LOG_TRACE("select returned %lld", ret);

        if (ret == 0)
            return option_error(sys_error(ERR_IO, "recv timeout"));

        // In case we have a low level IO recv function, use it
        if (selector->rx.buf != NULL && selector->rx.recv_fn != NULL) {
            nbytes = poll_recv(poll, selector);

            if (nbytes == -1) {
                poll_deregister(poll, selector->id);
                return option_error(sys_error(ERR_IO, "recv failed"));
            }

            if (nbytes == 0)
                return option_none();
        }

        if (selector->rx.read_fn != NULL)
            result = selector->rx.read_fn(poll, selector);

        if (option_is_error(&result)) {
            poll_deregister(poll, selector->id);
            return result;
        }

        if (option_is_some(&result) && result.value != NULL)
            return result;
    }

    LOG_DEBUG("empty buffer");

    return option_none();
}
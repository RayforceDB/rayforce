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

#include <sys/event.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "sock.h"
#include "heap.h"
#include "io.h"
#include "error.h"
#include "sys.h"
#include "eval.h"
#include "binary.h"

__thread i32_t __POLL_WAKER[2];  // pipe to notify kqueue loop of shutdown

nil_t sigint_handler(i32_t signo) {
    UNUSED(signo);
    i64_t val = 1;
    // Write to the pipe to wake up the kqueue loop.
    write(__POLL_WAKER[1], &val, sizeof(val));
}

poll_p poll_create() {
    i64_t kq_fd = -1;
    poll_p poll;
    struct kevent ev;

    kq_fd = kqueue();
    if (kq_fd == -1) {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }

    // Create a pipe
    if (pipe(__POLL_WAKER) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    // Add pipe to kqueue
    EV_SET(&ev, __POLL_WAKER[0], POLL_EVENT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent: pipe");
        exit(EXIT_FAILURE);
    }

    // Set up the SIGINT signal handler
    signal(SIGINT, sigint_handler);

    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->fd = kq_fd;
    poll->code = NULL_I64;
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(128);

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
    close(__POLL_WAKER[0]);
    close(__POLL_WAKER[1]);

    LOG_DEBUG("Closing epoll instance");
    close(poll->fd);

    LOG_DEBUG("Freeing poll instance");
    heap_free(poll);
}

i64_t poll_register(poll_p poll, poll_registry_p registry) {
    i64_t id;
    selector_p selector;
    struct kevent ev[2];

    selector = (selector_p)heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->fd = registry->fd;
    selector->type = registry->type;
    selector->interest = registry->events;
    selector->open_fn = registry->open_fn;
    selector->close_fn = registry->close_fn;
    selector->error_fn = registry->error_fn;
    selector->rx.recv_fn = registry->recv_fn;
    selector->rx.read_fn = registry->read_fn;
    selector->tx.write_fn = registry->write_fn;
    selector->tx.send_fn = registry->send_fn;
    selector->data_fn = registry->data_fn;
    selector->data = registry->data;
    selector->rx.buf = NULL;
    selector->tx.buf = NULL;

    // Set up events
    if (registry->events & POLL_EVENT_READ) {
        EV_SET(&ev[0], selector->fd, EVFILT_READ, EV_ADD, 0, 0, selector);
        if (kevent(poll->fd, &ev[0], 1, NULL, 0, NULL) == -1) {
            perror("kevent add read");
            heap_free(selector);
            return -1;
        }
    }

    if (registry->events & POLL_EVENT_WRITE) {
        EV_SET(&ev[0], selector->fd, EVFILT_WRITE, EV_ADD, 0, 0, selector);
        if (kevent(poll->fd, &ev[0], 1, NULL, 0, NULL) == -1) {
            perror("kevent add write");
            heap_free(selector);
            return -1;
        }
    }

    // Add error and EOF events if requested
    if (registry->events & (POLL_EVENT_ERROR | POLL_EVENT_HUP | POLL_EVENT_RDHUP)) {
        EV_SET(&ev[0], selector->fd, EVFILT_READ, EV_ADD | EV_ENABLE, EV_ERROR | EV_EOF, 0, selector);
        if (kevent(poll->fd, &ev[0], 1, NULL, 0, NULL) == -1) {
            perror("kevent add error");
            heap_free(selector);
            return -1;
        }
    }

    if (registry->open_fn != NULL)
        registry->open_fn(poll, selector);

    return id;
}

i64_t poll_deregister(poll_p poll, i64_t id) {
    i64_t idx;
    selector_p selector;
    struct kevent ev[2];

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return 0;

    selector = (selector_p)idx;

    EV_SET(&ev[0], selector->fd, selector->interest, EV_DELETE, 0, 0, NULL);
    kevent(poll->fd, ev, 1, NULL, 0, NULL);

    close(selector->fd);

    heap_free(selector->rx.buf);
    heap_free(selector->tx.buf);
    heap_free(selector);

    return 0;
}

i64_t poll_recv(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size, total;

    LOG_TRACE("Receiving data from selector %lld", selector->id);

    total = selector->rx.buf->offset;

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
    struct kevent ev;

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

    // If we've sent all data, disable write events
    EV_SET(&ev, selector->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, selector);
    if (kevent(poll->fd, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent disable write");
        return -1;
    }

    LOG_TRACE("Total bytes sent to selector %lld: %lld", selector->id, total);

    return total;
}

i64_t poll_run(poll_p poll) {
    i64_t n, nbytes, nfds, next_tm;
    option_t poll_result;
    selector_p selector;
    struct kevent ev, events[MAX_EVENTS];
    struct timespec tm, *timeout = NULL;

    while (poll->code == NULL_I64) {
        next_tm = timer_next_timeout(poll->timers);
        timeout = (next_tm == TIMEOUT_INFINITY)
                      ? NULL
                      : (tm.tv_sec = next_tm / 1000, tm.tv_nsec = (next_tm % 1000) * 1000000, &tm);

        nfds = kevent(poll->fd, NULL, 0, events, MAX_EVENTS, timeout);
        if (nfds == -1)
            return 1;

        for (n = 0; n < nfds; ++n) {
            ev = events[n];

            // Check for shutdown signal
            if (ev.ident == (u32_t)__POLL_WAKER[0]) {
                LOG_DEBUG("Shutdown event received");
                poll->code = 0;
                break;
            }

            selector = (selector_p)ev.udata;

            // error or hang up
            if (ev.flags & EV_ERROR) {
                LOG_DEBUG("Connection error for selector %lld, flags: 0x%x", selector->id, ev.flags);
                poll_deregister(poll, selector->id);
                continue;
            }

            if (ev.flags & EV_EOF) {
                LOG_DEBUG("Connection closed for selector %lld, flags: 0x%x", selector->id, ev.flags);
                poll_deregister(poll, selector->id);
                continue;
            }

            // read
            if (ev.filter == EVFILT_READ) {
                LOG_TRACE("Read event received for selector %lld", selector->id);
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
            if (ev.filter == EVFILT_WRITE) {
                LOG_TRACE("Write event received for selector %lld", selector->id);
                nbytes = 0;

                // If there's no data to write, disable write events
                if (selector->tx.buf == NULL) {
                    struct kevent ev;
                    EV_SET(&ev, selector->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, selector);
                    kevent(poll->fd, &ev, 1, NULL, 0, NULL);
                    goto next_event;
                }

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
    i64_t nbytes, ret;
    struct kevent ev;
    struct timespec timeout;

    LOG_TRACE("Blocking on selector id: %lld, fd: %lld", selector->id, selector->fd);

    // Validate file descriptor
    if (selector->fd < 0) {
        LOG_ERROR("Invalid file descriptor %lld", selector->fd);
        return option_error(sys_error(ERR_IO, "invalid file descriptor"));
    }

    // Verify file descriptor is still valid
    if (fcntl(selector->fd, F_GETFD) == -1) {
        LOG_ERROR("File descriptor %lld is not valid: %s", selector->fd, strerror(errno));
        return option_error(sys_error(ERR_IO, "invalid file descriptor"));
    }

    // Setup timeout
    timeout.tv_sec = 3;  // 3 seconds
    timeout.tv_nsec = 0;

    // Perform the read operation
    while (selector->rx.buf != NULL) {
        // Try to read first without blocking
        if (selector->rx.recv_fn != NULL) {
            nbytes = poll_recv(poll, selector);
            if (nbytes > 0) {
                // We got data, process it
                if (selector->rx.read_fn != NULL) {
                    result = selector->rx.read_fn(poll, selector);
                    if (option_is_error(&result)) {
                        poll_deregister(poll, selector->id);
                        return result;
                    }
                    if (option_is_some(&result) && result.value != NULL)
                        return result;
                }
                continue;
            }
            if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Would block, use kqueue to wait
            } else if (nbytes == -1) {
                poll_deregister(poll, selector->id);
                return option_error(sys_error(ERR_IO, "recv failed"));
            }
        }

        // Setup kqueue event
        EV_SET(&ev, selector->fd, EVFILT_READ, EV_ADD, 0, 0, selector);

        // Wait for the file descriptor to become readable
        ret = kevent(poll->fd, &ev, 1, &ev, 1, &timeout);
        if (ret == -1) {
            if (errno == EINVAL) {
                // Check if the file descriptor is still valid
                if (fcntl(selector->fd, F_GETFD) == -1) {
                    LOG_ERROR("File descriptor %lld became invalid: %s", selector->fd, strerror(errno));
                    return option_error(sys_error(ERR_IO, "file descriptor became invalid"));
                }
                // Try to get more information about the file descriptor
                int flags = fcntl(selector->fd, F_GETFL);
                if (flags == -1) {
                    LOG_ERROR("Cannot get file descriptor flags: %s", strerror(errno));
                } else {
                    LOG_ERROR("File descriptor flags: 0x%x", flags);
                }
                LOG_ERROR("kevent failed: Invalid argument (fd=%lld, errno=%d)", selector->fd, errno);
                return option_error(sys_error(ERR_IO, "kevent failed: invalid argument"));
            }
            LOG_ERROR("kevent failed: %s (fd=%lld, errno=%d)", strerror(errno), selector->fd, errno);
            return option_error(sys_error(ERR_IO, "recv kevent failed"));
        }

        if (ret == 0)
            return option_error(sys_error(ERR_IO, "recv timeout"));

        // Check if we got an error event
        if (ev.flags & (EV_ERROR | EV_EOF)) {
            LOG_ERROR("kevent error events: 0x%x", ev.flags);
            return option_error(sys_error(ERR_IO, "kevent error events"));
        }

        // Try to read again after kevent indicates data is available
        if (selector->rx.recv_fn != NULL) {
            nbytes = poll_recv(poll, selector);
            if (nbytes == -1) {
                poll_deregister(poll, selector->id);
                return option_error(sys_error(ERR_IO, "recv failed"));
            }
            if (nbytes == 0)
                return option_none();
        }

        if (selector->rx.read_fn != NULL) {
            result = selector->rx.read_fn(poll, selector);
            if (option_is_error(&result)) {
                poll_deregister(poll, selector->id);
                return result;
            }
            if (option_is_some(&result) && result.value != NULL)
                return result;
        }
    }

    LOG_DEBUG("empty buffer");
    return option_none();
}
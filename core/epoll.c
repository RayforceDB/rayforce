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
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "sock.h"
#include "heap.h"
#include "io.h"
#include "error.h"
#include "symbols.h"
#include "eval.h"
#include "sys.h"
#include "chrono.h"
#include "binary.h"
#include "def.h"  // Added for RAYFORCE_VERSION

__thread i32_t __EVENT_FD;  // eventfd to notify epoll loop of shutdown

// Forward declaration of callback handlers
poll_result_t stdin_on_read(poll_p poll, selector_p selector);
poll_result_t event_fd_on_read(poll_p poll, selector_p selector);
poll_result_t listener_on_read(poll_p poll, selector_p selector);
poll_result_t default_on_read(poll_p poll, selector_p selector);
poll_result_t default_on_write(poll_p poll, selector_p selector);
poll_result_t default_on_error(poll_p poll, selector_p selector);

nil_t sigint_handler(i32_t signo) {
    u64_t val = 1;
    i32_t res;

    UNUSED(signo);
    // Write to the eventfd to wake up the epoll loop.
    res = write(__EVENT_FD, &val, sizeof(val));
    UNUSED(res);
}

poll_p poll_init(i64_t port) {
    i64_t epoll_fd = -1, listen_fd = -1;
    poll_p poll;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->code = NULL_I64;
    poll->poll_fd = epoll_fd;
    poll->replfile = string_from_str("repl", 4);
    poll->ipcfile = string_from_str("ipc", 3);
    poll->term = term_create();
    poll->selectors = freelist_create(128);
    poll->timers = timers_create(16);

    // Add eventfd with callback
    __EVENT_FD = eventfd(0, 0);
    if (__EVENT_FD == -1) {
        perror("eventfd");
        exit(EXIT_FAILURE);
    }

    // Register eventfd with callback
    poll_register_with_callbacks(poll, __EVENT_FD, event_fd_on_read, NULL, NULL, NULL);

    // Set up the SIGINT signal handler
    signal(SIGINT, sigint_handler);

    // Register stdin with callback
    poll_register_with_callbacks(poll, STDIN_FILENO, stdin_on_read, NULL, NULL, NULL);

    // Add server socket with callback if port is specified
    if (port) {
        listen_fd = sock_listen(port);
        if (listen_fd == -1) {
            perror("listen");
            exit(EXIT_FAILURE);
        }

        poll_register_with_callbacks(poll, listen_fd, listener_on_read, NULL, NULL, NULL);
    }

    return poll;
}

i64_t poll_listen(poll_p poll, i64_t port) {
    i64_t listen_fd;

    if (poll == NULL)
        return -1;

    listen_fd = sock_listen(port);
    if (listen_fd == -1)
        return -1;

    // Register with listener callback
    poll_register_with_callbacks(poll, listen_fd, listener_on_read, NULL, NULL, NULL);

    return listen_fd;
}

nil_t poll_destroy(poll_p poll) {
    i64_t i, l;

    term_destroy(poll->term);
    close(STDIN_FILENO);

    // Free all selectors
    l = poll->selectors->data_pos;
    for (i = 0; i < l; i++) {
        if (poll->selectors->data[i] != NULL_I64)
            poll_deregister(poll, i + SELECTOR_ID_OFFSET);
    }

    drop_obj(poll->replfile);
    drop_obj(poll->ipcfile);

    freelist_free(poll->selectors);
    timers_destroy(poll->timers);

    close(__EVENT_FD);
    close(poll->poll_fd);
    heap_free(poll);
}

i64_t poll_register(poll_p poll, i64_t fd) {
    // Default to using the default handlers
    return poll_register_with_callbacks(poll, fd, default_on_read, default_on_write, default_on_error, NULL);
}

i64_t poll_register_with_callbacks(poll_p poll, i64_t fd, on_read_callback_t on_read, on_write_callback_t on_write,
                                   on_error_callback_t on_error, raw_p user_data) {
    i64_t id;
    selector_p selector;
    struct epoll_event ev;

    selector = (selector_p)heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->handshake_completed = B8_FALSE;  // Start with handshake not completed
    selector->fd = fd;
    selector->on_read = on_read;
    selector->on_write = on_write;
    selector->on_error = on_error;
    selector->user_data = user_data;
    selector->tx.isset = B8_FALSE;
    selector->rx.buf = NULL;
    selector->rx.size = 0;
    selector->rx.bytes_transfered = 0;
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.bytes_transfered = 0;
    selector->tx.queue = queue_create(TX_QUEUE_SIZE);

    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.ptr = selector;
    if (epoll_ctl(poll->poll_fd, EPOLL_CTL_ADD, fd, &ev) == -1)
        perror("epoll_ctl: add");

    return id;
}

nil_t poll_call_usr_on_open(poll_p poll, i64_t id) {
    UNUSED(poll);
    i64_t clbnm;
    obj_p v, f, *clbfn;

    stack_push(NULL_OBJ);  // null env
    clbnm = symbols_intern(".z.po", 5);
    clbfn = resolve(clbnm);
    stack_pop();  // null env

    // Call the callback if it's a lambda
    if (clbfn != NULL && (*clbfn)->type == TYPE_LAMBDA) {
        poll_set_usr_fd(id);
        stack_push(i64(id));
        v = call(*clbfn, 1);
        drop_obj(stack_pop());
        poll_set_usr_fd(0);
        if (IS_ERR(v)) {
            f = obj_fmt(v, B8_FALSE);
            fprintf(stderr, "Error in .z.po callback: \n%.*s\n", (i32_t)f->len, AS_C8(f));
            drop_obj(f);
        }

        drop_obj(v);
    }
}

nil_t poll_call_usr_on_close(poll_p poll, i64_t id) {
    UNUSED(poll);
    i64_t clbnm;
    obj_p v, f, *clbfn;

    stack_push(NULL_OBJ);  // null env
    clbnm = symbols_intern(".z.pc", 5);
    clbfn = resolve(clbnm);
    stack_pop();  // null env

    // Call the callback if it's a lambda
    if (clbfn != NULL && (*clbfn)->type == TYPE_LAMBDA) {
        poll_set_usr_fd(id);
        stack_push(i64(id));
        v = call(*clbfn, 1);
        drop_obj(stack_pop());
        poll_set_usr_fd(0);
        if (IS_ERR(v)) {
            f = obj_fmt(v, B8_FALSE);
            fprintf(stderr, "Error in .z.pc callback: \n%.*s\n", (i32_t)f->len, AS_C8(f));
            drop_obj(f);
        }

        drop_obj(v);
    }
}

nil_t poll_deregister(poll_p poll, i64_t id) {
    i64_t idx;
    selector_p selector;

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return;

    selector = (selector_p)idx;

    // If there is a callback, call it
    poll_call_usr_on_close(poll, id);

    epoll_ctl(poll->poll_fd, EPOLL_CTL_DEL, selector->fd, NULL);
    close(selector->fd);

    heap_free(selector->rx.buf);
    heap_free(selector->tx.buf);
    queue_free(selector->tx.queue);
    heap_free(selector);
}

poll_result_t _recv(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t sz, size;
    header_t *header;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    if (selector->rx.buf == NULL)
        selector->rx.buf = (u8_t *)heap_alloc(sizeof(struct header_t));

    // wait for handshake
    if (selector->handshake_completed == B8_FALSE) {
        while (selector->rx.bytes_transfered == 0 || selector->rx.buf[selector->rx.bytes_transfered - 1] != '\0') {
            size = sock_recv(selector->fd, &selector->rx.buf[selector->rx.bytes_transfered], sizeof(struct header_t));
            if (size == -1)
                return POLL_ERROR;
            else if (size == 0)
                return POLL_PENDING;

            selector->rx.bytes_transfered += size;
        }

        // Store client version if needed for future version checking
        u8_t client_version = selector->rx.buf[selector->rx.bytes_transfered - 2];
        UNUSED(client_version);  // Currently unused but could be used for version compatibility checks

        selector->handshake_completed = B8_TRUE;
        selector->rx.bytes_transfered = 0;

        // send handshake response
        size = 0;
        while (size < (i64_t)sizeof(handshake)) {
            sz = sock_send(selector->fd, &handshake[size], sizeof(handshake) - size);

            if (sz == -1)
                return POLL_ERROR;

            size += sz;
        }

        // Now we are ready for income messages and can call userspace callback (if any)
        poll_call_usr_on_open(poll, selector->id);
    }

    // read header
    if (selector->rx.size == 0) {
        while (selector->rx.bytes_transfered < (i64_t)sizeof(struct header_t)) {
            size = sock_recv(selector->fd, &selector->rx.buf[selector->rx.bytes_transfered],
                             sizeof(struct header_t) - selector->rx.bytes_transfered);
            if (size == -1)
                return POLL_ERROR;
            else if (size == 0)
                return POLL_PENDING;

            selector->rx.bytes_transfered += size;
        }

        header = (header_t *)selector->rx.buf;
        selector->rx.msgtype = header->msgtype;
        selector->rx.size = header->size + sizeof(struct header_t);
        selector->rx.buf = (u8_t *)heap_realloc(selector->rx.buf, selector->rx.size);
    }

    while (selector->rx.bytes_transfered < selector->rx.size) {
        size = sock_recv(selector->fd, &selector->rx.buf[selector->rx.bytes_transfered],
                         selector->rx.size - selector->rx.bytes_transfered);
        if (size == -1)
            return POLL_ERROR;
        else if (size == 0)
            return POLL_PENDING;

        selector->rx.bytes_transfered += size;
    }

    return POLL_DONE;
}

poll_result_t _send(poll_p poll, selector_p selector) {
    i64_t size;
    obj_p obj;
    nil_t *v;
    i8_t msg_type = MSG_TYPE_RESP;
    struct epoll_event ev;

send:
    while (selector->tx.bytes_transfered < selector->tx.size) {
        size = sock_send(selector->fd, &selector->tx.buf[selector->tx.bytes_transfered],
                         selector->tx.size - selector->tx.bytes_transfered);
        if (size == -1)
            return POLL_ERROR;
        else if (size == 0) {
            // setup epoll for EPOLLOUT only if it's not already set
            if (!selector->tx.isset) {
                selector->tx.isset = B8_TRUE;
                ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
                ev.data.ptr = selector;
                if (epoll_ctl(poll->poll_fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
                    return POLL_ERROR;
            }

            return POLL_PENDING;
        }

        selector->tx.bytes_transfered += size;
    }

    heap_free(selector->tx.buf);
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.bytes_transfered = 0;

    v = queue_pop(selector->tx.queue);

    if (v != NULL) {
        obj = (obj_p)((i64_t)v & ~(3ll << 61));
        msg_type = (((i64_t)v & (3ll << 61)) >> 61);
        size = ser_raw(&selector->tx.buf, obj);
        selector->tx.size = size;
        drop_obj(obj);
        if (size == -1)
            return POLL_ERROR;

        ((header_t *)selector->tx.buf)->msgtype = msg_type;
        goto send;
    }

    // remove EPOLLOUT only if it's set
    if (selector->tx.isset) {
        selector->tx.isset = B8_FALSE;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.ptr = selector;
        if (epoll_ctl(poll->poll_fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
            return POLL_ERROR;
    }

    return POLL_DONE;
}

obj_p read_obj(selector_p selector) {
    obj_p res;

    res = de_raw(selector->rx.buf, selector->rx.size);
    heap_free(selector->rx.buf);
    selector->rx.buf = NULL;
    selector->rx.bytes_transfered = 0;
    selector->rx.size = 0;

    return res;
}

nil_t process_request(poll_p poll, selector_p selector) {
    poll_result_t poll_result;
    obj_p v, res;

    res = read_obj(selector);

    poll_set_usr_fd(selector->id);

    if (IS_ERR(res) || is_null(res))
        v = res;
    else if (res->type == TYPE_C8) {
        v = ray_eval_str(res, poll->ipcfile);
        drop_obj(res);
    } else {
        v = eval_obj(res);
        drop_obj(res);
    }

    poll_set_usr_fd(0);

    // sync request
    if (selector->rx.msgtype == MSG_TYPE_SYNC) {
        queue_push(selector->tx.queue, (nil_t *)((i64_t)v | ((i64_t)MSG_TYPE_RESP << 61)));
        poll_result = _send(poll, selector);

        if (poll_result == POLL_ERROR)
            poll_deregister(poll, selector->id);
    } else
        drop_obj(v);
}

// Event handler for stdin
poll_result_t stdin_on_read(poll_p poll, selector_p selector) {
    UNUSED(selector);
    UNUSED(poll);

    obj_p str, res;
    b8_t error;

    if (!term_getc(poll->term)) {
        poll->code = 1;
        return POLL_ERROR;
    }

    str = term_read(poll->term);
    if (str != NULL) {
        if (IS_ERR(str))
            io_write(STDOUT_FILENO, MSG_TYPE_RESP, str);
        else if (str != NULL_OBJ) {
            res = ray_eval_str(str, poll->replfile);
            drop_obj(str);
            io_write(STDOUT_FILENO, MSG_TYPE_RESP, res);
            error = IS_ERR(res);
            drop_obj(res);
            if (!error)
                timeit_print();
        }

        term_prompt(poll->term);
    }

    return POLL_DONE;
}

// Event handler for eventfd (shutdown)
poll_result_t event_fd_on_read(poll_p poll, selector_p selector) {
    UNUSED(selector);
    UNUSED(poll);

    poll->code = 0;
    return POLL_DONE;
}

// Event handler for listener socket
poll_result_t listener_on_read(poll_p poll, selector_p selector) {
    i64_t sock;

    sock = sock_accept(selector->fd);
    if (sock != -1)
        poll_register(poll, sock);

    return POLL_DONE;
}

// Default handler for regular socket connections
poll_result_t default_on_read(poll_p poll, selector_p selector) {
    poll_result_t poll_result;

    poll_result = _recv(poll, selector);
    if (poll_result == POLL_PENDING)
        return POLL_PENDING;

    if (poll_result == POLL_ERROR)
        return POLL_ERROR;

    process_request(poll, selector);

    return POLL_DONE;
}

i64_t poll_run(poll_p poll) {
    i64_t n, nfds, timeout = TIMEOUT_INFINITY;
    poll_result_t poll_result;
    selector_p selector;
    struct epoll_event events[MAX_EVENTS];

    term_prompt(poll->term);

    while (poll->code == NULL_I64) {
        timeout = timer_next_timeout(poll->timers);
        nfds = epoll_wait(poll->poll_fd, events, MAX_EVENTS, timeout);
        if (nfds == -1 && errno == EINTR)
            continue;

        if (nfds == -1)
            return 1;

        for (n = 0; n < nfds; n++) {
            selector = (selector_p)events[n].data.ptr;

            // Store current events in the selector
            selector->events = events[n].events;

            // Handle error events first
            if ((events[n].events & EPOLLERR) || (events[n].events & EPOLLHUP)) {
                if (selector->on_error) {
                    poll_result = selector->on_error(poll, selector);
                    if (poll_result == POLL_ERROR)
                        poll_deregister(poll, selector->id);
                    continue;
                }
            }

            // Handle read events
            if (events[n].events & EPOLLIN) {
                if (selector->on_read) {
                    poll_result = selector->on_read(poll, selector);
                    if (poll_result == POLL_ERROR)
                        poll_deregister(poll, selector->id);
                }
            }

            // Handle write events
            if (events[n].events & EPOLLOUT) {
                if (selector->on_write) {
                    poll_result = selector->on_write(poll, selector);
                    if (poll_result == POLL_ERROR)
                        poll_deregister(poll, selector->id);
                }
            }
        }
    }

    return poll->code;
}

// Default handler for error events
poll_result_t default_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return POLL_ERROR;  // Simply signal an error for cleanup
}

// Default handler for write events
poll_result_t default_on_write(poll_p poll, selector_p selector) {
    poll_result_t poll_result;

    poll_result = _send(poll, selector);
    if (poll_result == POLL_ERROR)
        return POLL_ERROR;

    return POLL_DONE;
}

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    poll_result_t poll_result = POLL_PENDING;
    selector_p selector;
    i32_t result;
    i64_t idx;
    obj_p res;
    fd_set fds;
    struct timeval tv;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    selector = (selector_p)idx;

    queue_push(selector->tx.queue, (nil_t *)((i64_t)msg | ((i64_t)MSG_TYPE_SYNC << 61)));

    while (B8_TRUE) {
        poll_result = _send(poll, selector);

        if (poll_result != POLL_PENDING)
            break;

        // block on select until we can send
        FD_ZERO(&fds);
        FD_SET(selector->fd, &fds);
        tv.tv_sec = 30;  // 30 second timeout
        tv.tv_usec = 0;
        result = select(selector->fd + 1, NULL, &fds, NULL, &tv);

        if (result == -1) {
            if (errno != EINTR) {
                poll_deregister(poll, selector->id);
                return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error sending message (can't block on send)");
            }
        }
    }

    if (poll_result == POLL_ERROR) {
        poll_deregister(poll, selector->id);
        return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error sending message");
    }

recv:
    while (B8_TRUE) {
        poll_result = _recv(poll, selector);

        if (poll_result != POLL_PENDING)
            break;

        // block on select until we can recv
        FD_ZERO(&fds);
        FD_SET(selector->fd, &fds);
        tv.tv_sec = 30;  // 30 second timeout
        tv.tv_usec = 0;
        result = select(selector->fd + 1, &fds, NULL, NULL, &tv);

        if (result == -1) {
            if (errno != EINTR) {
                poll_deregister(poll, selector->id);
                return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error receiving message (can't block on recv)");
            }
        }
    }

    if (poll_result == POLL_ERROR) {
        poll_deregister(poll, selector->id);
        return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error receiving message");
    }

    // recv until we get response
    switch (selector->rx.msgtype) {
        case MSG_TYPE_RESP:
            res = read_obj(selector);
            break;
        default:
            process_request(poll, selector);
            goto recv;
    }

    return res;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    i64_t idx;
    selector_p selector;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    selector = (selector_p)idx;
    if (selector == NULL)
        THROW(ERR_IO, "ipc_send_async: invalid socket fd: %lld", id);

    queue_push(selector->tx.queue, (nil_t *)msg);

    if (_send(poll, selector) == POLL_ERROR)
        THROW(ERR_IO, "ipc_send_async: error sending message");

    return NULL_OBJ;
}

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
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "sock.h"

__thread i32_t __EVENT_FD[2]; // eventfd to notify epoll loop of shutdown
__thread u8_t __STDIN_BUF[BUF_SIZE + 1];

nil_t sigint_handler(i32_t signo)
{
    unused(signo);
    u64_t val = 1;
    // Write to the eventfd to wake up the epoll loop.
    write(__EVENT_FD[1], &val, sizeof(val));
}

poll_t poll_init(i64_t port)
{
    i64_t kq_fd = -1, listen_fd = -1;
    poll_t poll;
    struct kevent ev;

    kq_fd = kqueue();
    if (kq_fd == -1)
    {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }

    // Create a pipe
    if (pipe(__EVENT_FD) == -1)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    // Add eventfd to kqueue
    EV_SET(&ev, __EVENT_FD[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) == -1)
    {
        perror("kevent: eventfd");
        exit(EXIT_FAILURE);
    }

    // Set up the SIGINT signal handler
    signal(SIGINT, sigint_handler);

    // Add stdin
    EV_SET(&ev, STDIN_FILENO, EVFILT_READ | EVFILT_EXCEPT, EV_ADD, 0, 0, NULL);
    if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) == -1)
    {
        perror("kevent: stdinfd");
        exit(EXIT_FAILURE);
    }

    // Add server socket
    if (port)
    {
        listen_fd = sock_listen(port);
        if (listen_fd == -1)
        {
            perror("listen");
            exit(EXIT_FAILURE);
        }

        EV_SET(&ev, listen_fd, EVFILT_READ | EVFILT_EXCEPT, EV_ADD, 0, 0, NULL);
        if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) == -1)
        {
            perror("kevent: listenfd");
            exit(EXIT_FAILURE);
        }
    }

    poll = (poll_t)heap_alloc(sizeof(struct poll_t));

    poll->poll_fd = kq_fd;
    poll->ipc_fd = listen_fd;
    poll->selectors = freelist_new(128);

    return poll;
}

nil_t poll_cleanup(poll_t poll)
{
    i64_t i, l;

    if (poll->ipc_fd != -1)
        close(poll->ipc_fd);

    // Free all selectors
    l = poll->selectors->data_pos;
    for (i = 0; i < l; i++)
    {
        if (poll->selectors->data[i] != NULL_I64)
            poll_deregister(poll, i + SELECTOR_ID_OFFSET);
    }

    freelist_free(poll->selectors);

    close(__EVENT_FD[0]);
    close(__EVENT_FD[1]);
    close(poll->poll_fd);
    heap_free(poll);

    printf("\nBye.\n");
    fflush(stdout);
}

i64_t poll_register(poll_t poll, i64_t fd, u8_t version)
{
    i64_t id;
    selector_t selector;
    struct kevent ev;

    selector = heap_alloc(sizeof(struct selector_t));
    id = freelist_push(poll->selectors, (i64_t)selector) + SELECTOR_ID_OFFSET;
    selector->id = id;
    selector->version = version;
    selector->fd = fd;
    selector->tx.events = EVFILT_READ;
    selector->rx.buf = NULL;
    selector->rx.size = 0;
    selector->rx.bytes_transfered = 0;
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.bytes_transfered = 0;
    selector->tx.queue = queue_new(TX_QUEUE_SIZE);

    EV_SET(&ev, fd, EVFILT_READ | EVFILT_WRITE, EV_ADD, 0, 0, selector);

    if (kevent(poll->poll_fd, &ev, 1, NULL, 0, NULL) == -1)
        perror("kevent add");

    return id;
}

nil_t poll_deregister(poll_t poll, i64_t id)
{
    i64_t idx;
    selector_t selector;
    struct kevent ev;

    idx = freelist_pop(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return;

    selector = (selector_t)idx;

    EV_SET(&ev, selector->fd, EVFILT_READ | EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(poll->poll_fd, &ev, 1, NULL, 0, NULL) == -1)
        perror("select del");

    close(selector->fd);

    heap_free(selector->rx.buf);
    heap_free(selector->tx.buf);
    queue_free(&selector->tx.queue);
    heap_free(selector);
}

poll_result_t _recv(poll_t poll, selector_t selector)
{
    unused(poll);

    i64_t sz, size;
    header_t *header;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    if (selector->rx.buf == NULL)
        selector->rx.buf = heap_alloc(sizeof(struct header_t));

    // wait for handshake
    if (selector->version == 0)
    {
        while (selector->rx.bytes_transfered == 0 || selector->rx.buf[selector->rx.bytes_transfered - 1] != '\0')
        {
            size = sock_recv(selector->fd, &selector->rx.buf[selector->rx.bytes_transfered], sizeof(struct header_t));
            if (size == -1)
                return POLL_ERROR;
            else if (size == 0)
                return POLL_PENDING;

            selector->rx.bytes_transfered += size;
        }

        selector->version = selector->rx.buf[selector->rx.bytes_transfered - 2];
        selector->rx.bytes_transfered = 0;

        // send handshake response
        size = 0;
        while (size < (i64_t)sizeof(handshake))
        {
            sz = sock_send(selector->fd, &handshake[size], sizeof(handshake) - size);

            if (sz == -1)
                return POLL_ERROR;

            size += sz;
        }
    }

    // read header
    if (selector->rx.size == 0)
    {
        while (selector->rx.bytes_transfered < (i64_t)sizeof(struct header_t))
        {
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
        selector->rx.buf = heap_realloc(selector->rx.buf, selector->rx.size);
    }

    while (selector->rx.bytes_transfered < selector->rx.size)
    {
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

poll_result_t _send(poll_t poll, selector_t selector)
{
    unused(poll);

    i64_t size;
    obj_t obj;
    nil_t *v;
    i8_t msg_type = MSG_TYPE_RESP;
    struct kevent ev;

send:
    while (selector->tx.bytes_transfered < selector->tx.size)
    {
        size = sock_send(selector->fd, &selector->tx.buf[selector->tx.bytes_transfered],
                         selector->tx.size - selector->tx.bytes_transfered);
        if (size == -1)
            return POLL_ERROR;
        else if (size == 0)
            return POLL_PENDING;

        selector->tx.bytes_transfered += size;
    }

    heap_free(selector->tx.buf);
    selector->tx.buf = NULL;
    selector->tx.size = 0;
    selector->tx.bytes_transfered = 0;

    v = queue_pop(&selector->tx.queue);

    if (v)
    {
        obj = (obj_t)((i64_t)v & ~(3ll << 61));
        msg_type = (((i64_t)v & (3ll << 61)) >> 61);
        size = ser_raw(&selector->tx.buf, obj);
        selector->tx.size = size;
        drop(obj);
        if (size == -1)
            return POLL_ERROR;

        ((header_t *)selector->tx.buf)->msgtype = msg_type;
        goto send;
    }

    return POLL_DONE;
}

obj_t read_obj(selector_t selector)
{
    obj_t res;

    res = de_raw(selector->rx.buf, selector->rx.size);
    heap_free(selector->rx.buf);
    selector->rx.buf = NULL;
    selector->rx.bytes_transfered = 0;
    selector->rx.size = 0;

    return res;
}

nil_t process_request(poll_t poll, selector_t selector)
{
    poll_result_t poll_result;
    obj_t v, res;

    res = read_obj(selector);

    if (is_error(res))
    {
        v = res;
    }
    else if (res->type == TYPE_CHAR)
    {
        v = eval_str(0, "ipc", as_string(res));
        drop(res);
    }
    else
        v = eval_obj(0, "ipc", res);

    // sync request
    if (selector->rx.msgtype == MSG_TYPE_SYNC)
    {
        queue_push(&selector->tx.queue, (nil_t *)((i64_t)v | ((i64_t)MSG_TYPE_RESP << 61)));
        poll_result = _send(poll, selector);

        if (poll_result == POLL_ERROR)
            poll_deregister(poll, selector->id);
    }
    else
        drop(v);
}

i64_t poll_run(poll_t poll)
{
    i64_t kq_fd = poll->poll_fd, listen_fd = poll->ipc_fd,
          nfds, len, poll_result, sock;
    i32_t n;
    selector_t selector;
    obj_t res, v;
    str_t fmt;
    bool_t running = true;
    struct kevent ev, events[MAX_EVENTS];

    prompt();

    while (running)
    {
        nfds = kevent(kq_fd, NULL, 0, events, MAX_EVENTS, NULL);
        if (nfds == -1)
            return 1;

        for (n = 0; n < nfds; ++n)
        {
            ev = events[n];

            // stdin
            if (ev.ident == STDIN_FILENO)
            {
                len = read(STDIN_FILENO, __STDIN_BUF, BUF_SIZE);
                __STDIN_BUF[len] = '\0';
                res = eval_str(0, "stdin", (str_t)__STDIN_BUF);
                if (res)
                {
                    fmt = obj_fmt(res);
                    printf("%s\n", fmt);
                    heap_free(fmt);
                    drop(res);
                }
                prompt();
            }
            // accept new connections
            else if (ev.ident == (uintptr_t)listen_fd)
            {
                sock = sock_accept(listen_fd);
                if (sock != -1)
                    poll_register(poll, sock, 0);
            }
            else if (ev.ident == (uintptr_t)__EVENT_FD[0])
                running = false;
            // tcp socket event
            else
            {
                selector = (selector_t)ev.udata;

                if ((ev.flags & EV_ERROR) || (ev.fflags & EV_EOF))
                {
                    poll_deregister(poll, selector->id);
                    continue;
                }

                // ipc in
                if (ev.filter & EVFILT_READ)
                {
                    poll_result = _recv(poll, selector);
                    if (poll_result == POLL_PENDING)
                        continue;

                    if (poll_result == POLL_ERROR)
                    {
                        poll_deregister(poll, selector->id);
                        continue;
                    }

                    process_request(poll, selector);
                }

                // ipc out
                if (ev.filter & EVFILT_WRITE)
                {
                    poll_result = _send(poll, selector);

                    if (poll_result == POLL_ERROR)
                        poll_deregister(poll, selector->id);
                }
            }
        }
    }

    return 0;
}

obj_t ipc_send_sync(poll_t poll, i64_t id, obj_t msg)
{
    poll_result_t poll_result = POLL_PENDING;
    selector_t selector;
    i32_t result;
    i64_t idx;
    obj_t res;
    fd_set fds;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        emit(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    selector = (selector_t)idx;

    queue_push(&selector->tx.queue, (nil_t *)((i64_t)msg | ((i64_t)MSG_TYPE_SYNC << 61)));

    while (true)
    {
        poll_result = _send(poll, selector);

        if (poll_result != POLL_PENDING)
            break;

        // block on select until we can send
        FD_ZERO(&fds);
        FD_SET(selector->fd, &fds);
        result = select(selector->fd + 1, NULL, &fds, NULL, NULL);

        if (result == -1)
        {
            poll_deregister(poll, selector->id);
            emit(ERR_IO, "ipc_send_sync: error sending message (can't block on send)");
        }
    }

    if (poll_result == POLL_ERROR)
    {
        poll_deregister(poll, selector->id);
        emit(ERR_IO, "ipc_send_sync: error sending message");
    }

    poll_result = POLL_PENDING;

recv:
    while (true)
    {
        poll_result = _recv(poll, selector);

        if (poll_result != POLL_PENDING)
            break;

        // block on select until we can recv
        FD_ZERO(&fds);
        FD_SET(selector->fd, &fds);
        result = select(selector->fd + 1, &fds, NULL, NULL, NULL);

        if (result == -1)
        {
            poll_deregister(poll, selector->id);
            emit(ERR_IO, "ipc_send_sync: error receiving message (can't block on recv)");
        }
    }

    if (poll_result == POLL_ERROR)
    {
        poll_deregister(poll, selector->id);
        emit(ERR_IO, "ipc_send_sync: error receiving message");
    }

    // recv until we get response
    switch (selector->rx.msgtype)
    {
    case MSG_TYPE_RESP:
        res = read_obj(selector);
        break;
    default:
        process_request(poll, selector);
        goto recv;
    }

    return res;
}

obj_t ipc_send_async(poll_t poll, i64_t id, obj_t msg)
{
    i64_t idx;
    selector_t selector;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        emit(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    selector = (selector_t)idx;
    if (selector == NULL)
        emit(ERR_IO, "ipc_send_async: invalid socket fd: %lld", id);

    queue_push(&selector->tx.queue, (nil_t *)msg);

    if (_send(poll, selector) == POLL_ERROR)
        emit(ERR_IO, "ipc_send_async: error sending message");

    return null(0);
}
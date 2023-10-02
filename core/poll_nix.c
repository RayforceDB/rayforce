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
#include <netinet/in.h>
#include <unistd.h>
#include "poll.h"
#include "string.h"
#include "hash.h"
#include "format.h"
#include "util.h"
#include "sock.h"
#include "heap.h"

i32_t __EVENT_FD; // eventfd to notify epoll loop of shutdown

nil_t sigint_handler(i32_t signo)
{
    unused(signo);
    u64_t val = 1;
    // Write to the eventfd to wake up the epoll loop.
    write(__EVENT_FD, &val, sizeof(val));
}

poll_t poll_init(i64_t port)
{
    i64_t epoll_fd = -1, listen_fd = -1;
    poll_t s;
    ipc_data_t data = NULL;
    struct epoll_event ev;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // Add eventfd
    __EVENT_FD = eventfd(0, 0);
    if (__EVENT_FD == -1)
    {
        perror("eventfd");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN;
    ev.data.ptr = add_data(&data, __EVENT_FD, -1);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, __EVENT_FD, &ev) == -1)
    {
        perror("epoll_ctl: eventfd");
        exit(EXIT_FAILURE);
    }

    // Set up the SIGINT signal handler
    signal(SIGINT, sigint_handler);

    // Add stdin
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.ptr = add_data(&data, STDIN_FILENO, BUF_SIZE);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1)
    {
        perror("epoll_ctl: stdin");
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
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.ptr = add_data(&data, listen_fd, 0);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1)
        {
            perror("epoll_ctl: listen_fd");
            exit(EXIT_FAILURE);
        }
    }

    s = (poll_t)heap_alloc(sizeof(struct poll_t));

    s->poll_fd = epoll_fd;
    s->ipc_fd = listen_fd;
    s->data = data;

    return s;
}

nil_t poll_cleanup(poll_t select)
{
    ipc_data_t data;
    obj_t v;

    if (select->ipc_fd != -1)
        close(select->ipc_fd);

    close(select->poll_fd);
    close(__EVENT_FD);

    // Free all ipc_data_t
    while (select->data)
    {
        data = select->data;
        select->data = data->next;
        heap_free(data->rx.buf);
        heap_free(data->tx.buf);
        epoll_ctl(select->poll_fd, EPOLL_CTL_DEL, data->fd, NULL);
        close(data->fd);

        while ((v = queue_pop(&data->tx.queue)))
            drop((obj_t)((i64_t)v & ~(3ll << 61)));

        queue_free(&data->tx.queue);
        heap_free(data);
    }

    printf("\nBye.\n");
    fflush(stdout);

    heap_free(select);
}

ipc_data_t poll_add(poll_t select, i64_t fd)
{
    ipc_data_t data = NULL;
    struct epoll_event ev;

    if (fd == -1)
    {
        perror("select add");
        return NULL;
    }
    else
    {
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.ptr = add_data(&select->data, fd, 0);
        if (epoll_ctl(select->poll_fd, EPOLL_CTL_ADD, fd, &ev) == -1)
            perror("epoll_ctl: add");

        data = (ipc_data_t)ev.data.ptr;
    }

    return data;
}

i64_t poll_del(poll_t select, i64_t fd)
{
    epoll_ctl(select->poll_fd, EPOLL_CTL_DEL, fd, NULL);
    remove_data(&select->data, fd);
    return 0;
}

i64_t poll_dispatch(poll_t select)
{
    i64_t epoll_fd = select->poll_fd, listen_fd = select->ipc_fd,
          nfds, len, poll;
    i32_t n;
    ipc_data_t data;
    obj_t res, v;
    str_t fmt;
    bool_t running = true;
    struct epoll_event events[MAX_EVENTS];

    prompt();

    while (running)
    {
        nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1)
            return 1;

        for (n = 0; n < nfds; ++n)
        {
            if (events[n].events & EPOLLERR)
            {
                poll_del(select, events[n].data.fd);
                continue;
            }

            // stdin
            data = (ipc_data_t)events[n].data.ptr;
            if (data->fd == STDIN_FILENO)
            {
                len = read(STDIN_FILENO, data->rx.buf, BUF_SIZE);
                data->rx.buf[len] = '\0';
                res = eval_str(0, "stdin", (str_t)data->rx.buf);
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
            else if (data->fd == listen_fd)
            {
                poll_add(select, sock_accept(listen_fd));
            }
            else if (data->fd == __EVENT_FD)
            {
                running = false;
            }
            // tcp socket event
            else
            {
                data = (ipc_data_t)events[n].data.ptr;

                // ipc in
                if (events[n].events & EPOLLIN)
                {
                    poll = poll_recv(select, data);
                    if (poll == POLL_PENDING)
                        continue;
                    else if (poll == POLL_ERROR)
                    {
                        poll_del(select, data->fd);
                        continue;
                    }
                    else
                    {
                        res = de_raw(data->rx.buf, data->rx.size);
                        heap_free(data->rx.buf);
                        data->rx.buf = NULL;
                        data->rx.read_size = 0;
                        data->rx.size = 0;

                        // sync || async
                        if (data->msgtype < 2)
                        {
                            if (res->type == TYPE_CHAR)
                            {
                                v = eval_str(0, "ipc", as_string(res));
                                drop(res);
                            }
                            else
                                v = eval_obj(0, "ipc", res);

                            // sync request
                            if (data->msgtype == MSG_TYPE_SYNC)
                            {
                                ipc_enqueue_msg(data, v, MSG_TYPE_RESP);
                                poll = poll_send(select, data);

                                if (poll == POLL_ERROR)
                                    perror("send reply");
                            }
                            else
                                drop(v);
                        }
                    }
                }
                // ipc out
                if (events[n].events & EPOLLOUT)
                {
                    poll = poll_send(select, data);

                    if (poll == -1)
                        poll_del(select, data->fd);
                }
            }
        }
    }

    return 0;
}

i64_t poll_recv(poll_t select, ipc_data_t data)
{
    unused(select);

    i64_t size, r;
    header_t *header;

    // read handshake first
    if (data->rx.version == 0)
    {
        if (data->rx.buf == NULL)
            data->rx.buf = heap_alloc(128);

        while (data->rx.read_size == 0 || data->rx.buf[data->rx.read_size - 1] != '\0')
        {
            size = sock_recv(data->fd, &data->rx.buf[data->rx.read_size], 1);
            if (size < 0)
                return POLL_ERROR;
            else if (size == 0)
                return POLL_PENDING;

            data->rx.read_size += size;
        }

        size = sock_recv(data->fd, &data->rx.buf[data->rx.read_size], 1);
        if (size < 0)
            return POLL_ERROR;
        else if (size == 0)
            return POLL_PENDING;

        data->rx.version = data->rx.buf[data->rx.read_size];
        data->rx.read_size += size;

        // send handshake back
        while (true)
        {
            r = sock_send(data->fd, data->rx.buf, data->rx.read_size);
            if (r == data->rx.read_size)
                break;
            else if (r == -1)
                return POLL_ERROR;
        }

        data->rx.read_size = 0;
    }

    // read message header
    if (data->rx.size == 0)
    {
        if (data->rx.buf == NULL)
            data->rx.buf = heap_alloc(sizeof(struct header_t));

        while (data->rx.read_size < (i64_t)sizeof(struct header_t))
        {
            size = sock_recv(data->fd, &data->rx.buf[data->rx.read_size], sizeof(struct header_t) - data->rx.read_size);
            if (size < 0)
                return POLL_ERROR;
            else if (size == 0)
                return POLL_PENDING;

            data->rx.read_size += size;
        }

        header = (header_t *)data->rx.buf;
        data->msgtype = header->msgtype;
        data->rx.size = header->size + sizeof(struct header_t);
        data->rx.buf = heap_realloc(data->rx.buf, data->rx.size);
    }

    // read message body
    while (data->rx.read_size < data->rx.size)
    {
        size = sock_recv(data->fd, &data->rx.buf[data->rx.read_size], data->rx.size - data->rx.read_size);
        if (size < 0)
            return POLL_ERROR;
        else if (size == 0)
            return POLL_PENDING;

        data->rx.read_size += size;
    }

    return POLL_READY;
}

i64_t poll_send(poll_t select, ipc_data_t data)
{
    i64_t size;
    obj_t obj;
    nil_t *v;
    i8_t msg_type = MSG_TYPE_RESP;
    struct epoll_event ev;

send:
    if (data->tx.write_size < data->tx.size)
    {
        size = sock_send(data->fd, &data->tx.buf[data->tx.write_size], data->tx.size - data->tx.write_size);
        if (size < 1)
            return POLL_ERROR;
        else if (size == 0)
        {
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            if (epoll_ctl(select->poll_fd, EPOLL_CTL_MOD, data->fd, &ev) == -1)
                perror("epoll_ctl");

            return POLL_PENDING;
        }

        data->tx.write_size += size;

        if (data->tx.write_size < data->tx.size)
        {
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
            if (epoll_ctl(select->poll_fd, EPOLL_CTL_MOD, data->fd, &ev) == -1)
                perror("epoll_ctl");

            return POLL_PENDING;
        }
    }

    v = queue_pop(&data->tx.queue);

    if (v)
    {
        obj = (obj_t)((i64_t)v & ~(3ll << 61));
        msg_type = (((i64_t)v & (3ll << 61)) >> 61);
        size = ser_raw(&data->tx.buf, obj);
        drop(obj);
        if (size == -1)
            return POLL_ERROR;

        data->tx.size = size;
        data->tx.write_size = 0;
        ((header_t *)data->tx.buf)->msgtype = msg_type;
        goto send;
    }

    // Nothing to send anymore
    heap_free(data->tx.buf);
    data->tx.buf = NULL;
    data->tx.write_size = 0;
    data->tx.size = 0;

    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(select->poll_fd, EPOLL_CTL_MOD, data->fd, &ev) == -1)
        perror("epoll_ctl");

    return POLL_READY;
}

ipc_data_t add_data(ipc_data_t *head, i32_t fd, i32_t size)
{
    ipc_data_t data;

    data = heap_alloc(sizeof(struct ipc_data_t));
    data->fd = fd;
    data->rx.version = 0;
    if (size < 1)
        data->rx.buf = NULL;
    else
        data->rx.buf = heap_alloc(size);
    data->rx.size = 0;
    data->rx.read_size = 0;
    data->tx.size = 0;
    data->tx.write_size = 0;
    data->tx.buf = NULL;
    data->next = *head;
    *head = data;
    if (size == -1)
        data->tx.queue = (queue_t){0};
    else
        data->tx.queue = queue_new(TX_QUEUE_SIZE);

    return data;
}

nil_t remove_data(ipc_data_t *head, i64_t fd)
{
    ipc_data_t *next, data;
    nil_t *v;

    next = head;
    while (*next)
    {
        if ((*next)->fd == fd)
        {
            data = *next;
            *next = data->next;
            sock_close(data->fd);
            heap_free(data->rx.buf);
            heap_free(data->tx.buf);

            while ((v = queue_pop(&data->tx.queue)))
                drop((obj_t)((i64_t)v & ~(3ll << 61)));

            queue_free(&data->tx.queue);
            heap_free(data);

            return;
        }
        next = &(*next)->next;
    }
}

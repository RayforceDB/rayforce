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

#include "poll.h"
#include "format.h"
#include "cc.h"
#include "sock.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include "poll_win.c"
#elif defined(__APPLE__) && defined(__MACH__)
#include "poll_osx.c"
#elif defined(__linux__)
#include "poll_nix.c"
#elif defined(__EMSCRIPTEN__)
#include "poll_emc.c"
#else
#error "Unsupported platform"
#endif

nil_t prompt()
{
    printf("%s%s%s", GREEN, PROMPT, RESET);
    fflush(stdout);
}

nil_t ipc_enqueue_msg(ipc_data_t data, obj_t obj, i8_t msg_type)
{
    queue_push(&data->tx.queue, (nil_t *)((i64_t)obj | ((i64_t)msg_type << 61)));
}

ipc_data_t find_data(ipc_data_t *head, i64_t fd)
{
    ipc_data_t next;

    next = *head;
    while (next)
    {
        if (next->fd == fd)
            return next;
        next = next->next;
    }

    return NULL;
}

obj_t ipc_send_sync(poll_t select, i64_t fd, obj_t obj)
{
    obj_t v;
    i64_t r;
    ipc_data_t data;

    data = find_data(&select->data, fd);

    if (data == NULL)
        emit(ERR_IO, "ipc send sync: invalid socket fd: %lld", fd);

    ipc_enqueue_msg(data, clone(obj), MSG_TYPE_SYNC);

    // flush all messages in the queue
    while (true)
    {
        r = poll_send(select, data);
        if (r == POLL_READY)
            break;
        else if (r == POLL_ERROR)
        {
            poll_del(select, fd);
            return sys_error(TYPE_GETLASTERROR, "ipc send sync");
        }
    }

    // read until we get response
    while (true)
    {
        r = poll_recv(select, data);

        if (r == POLL_PENDING)
            continue;

        if (r == POLL_ERROR)
        {
            poll_del(select, fd);
            return sys_error(TYPE_GETLASTERROR, "ipc send sync");
        }

        v = de_raw(data->rx.buf, data->rx.size);
        data->rx.read_size = 0;
        data->rx.size = 0;
        heap_free(data->rx.buf);

        return v;
    }
}

obj_t ipc_send_async(poll_t select, i64_t fd, obj_t obj)
{
    ipc_data_t data;
    i64_t snd;

    data = find_data(&select->data, fd);

    if (data == NULL)
        emit(ERR_IO, "ipc_send_async: invalid socket fd: %lld", fd);

    ipc_enqueue_msg(data, clone(obj), MSG_TYPE_ASYN);

    snd = poll_send(select, data);

    if (snd == POLL_ERROR)
    {
        poll_del(select, fd);
        return sys_error(TYPE_GETLASTERROR, "ipc send async");
    }

    return null(0);
}
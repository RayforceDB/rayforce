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

#ifndef POLL_H
#define POLL_H

#if defined(_WIN32) || defined(__CYGWIN__)
#include <winsock2.h>
#include <windows.h>
#endif
#include "rayforce.h"
#include "parse.h"
#include "serde.h"
#include "format.h"
#include "queue.h"

#define MAX_EVENTS 1024
#define BUF_SIZE 2048
#define PROMPT "> "

#define MSG_TYPE_ASYN 0
#define MSG_TYPE_SYNC 1
#define MSG_TYPE_RESP 2

#define TX_QUEUE_SIZE 16

#define POLL_ERROR -1
#define POLL_READY 0
#define POLL_PENDING 1

#if defined(_WIN32) || defined(__CYGWIN__)

typedef struct ipc_data_t
{
    i64_t fd;
    u8_t msgtype;

    struct
    {
        u8_t version;
        OVERLAPPED overlapped;
        i64_t read_size;
        i64_t size;
        u8_t *buf;
    } rx;

    struct
    {
        OVERLAPPED overlapped;
        i64_t write_size;
        i64_t size;
        u8_t *buf;
        queue_t queue; // queue for async messages waiting to be sent
    } tx;

    struct ipc_data_t *next;
} *ipc_data_t;

#else
typedef struct ipc_data_t
{
    i64_t fd;
    u8_t msgtype;

    struct
    {
        u8_t version;
        i64_t read_size;
        i64_t size;
        u8_t *buf;
    } rx;

    struct
    {
        i64_t write_size;
        i64_t size;
        u8_t *buf;
        queue_t queue; // queue for async messages waiting to be sent
    } tx;

    struct ipc_data_t *next;
} *ipc_data_t;

#endif

typedef struct poll_t
{
    i64_t poll_fd;
    i64_t ipc_fd;
    ipc_data_t data;
} *poll_t;

nil_t prompt();
nil_t remove_data(ipc_data_t *head, i64_t fd);
ipc_data_t add_data(ipc_data_t *head, i32_t fd, i32_t size);
ipc_data_t find_data(ipc_data_t *head, i64_t fd);
nil_t ipc_enqueue_msg(ipc_data_t data, obj_t obj, i8_t msg_type);

poll_t poll_init(i64_t port);
i64_t poll_recv(poll_t poll, ipc_data_t data);
i64_t poll_send(poll_t poll, ipc_data_t data);
nil_t poll_cleanup(poll_t poll);
obj_t ipc_send_sync(poll_t poll, i64_t fd, obj_t obj);
obj_t ipc_send_async(poll_t poll, i64_t fd, obj_t obj);
i64_t poll_dispatch(poll_t poll);
ipc_data_t poll_add(poll_t poll, i64_t fd);
i64_t poll_del(poll_t poll, i64_t fd);

#endif // POLL_H
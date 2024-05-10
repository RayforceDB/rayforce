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
#include "freelist.h"
#include "timer.h"

#define MAX_EVENTS 1024
#define BUF_SIZE 2048

#define MSG_TYPE_ASYN 0
#define MSG_TYPE_SYNC 1
#define MSG_TYPE_RESP 2

#define TX_QUEUE_SIZE 16
#define SELECTOR_ID_OFFSET 3 // shifts all selector ids by 2 to avoid 0, 1 ,2 ids (stdin, stdout, stderr)

typedef enum poll_result_t
{
    POLL_DONE = 0,
    POLL_PENDING = 1,
    POLL_ERROR = 2,
} poll_result_t;

#if defined(_WIN32) || defined(__CYGWIN__)

typedef struct selector_t
{
    i64_t fd; // socket fd
    i64_t id; // selector id
    u8_t version;

    struct
    {
        b8_t ignore;
        u8_t msgtype;
        b8_t header;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD bytes_transfered;
        i64_t size;
        u8_t *buf;
        WSABUF wsa_buf;
    } rx;

    struct
    {
        b8_t ignore;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD bytes_transfered;
        i64_t size;
        u8_t *buf;
        WSABUF wsa_buf;
        queue_p queue; // queue for async messages waiting to be sent
    } tx;

} *selector_p;

#else
typedef struct selector_t
{
    i64_t fd; // socket fd
    i64_t id; // selector id
    u8_t version;

    struct
    {
        u8_t msgtype;
        i64_t bytes_transfered;
        i64_t size;
        u8_t *buf;
    } rx;

    struct
    {
        b8_t isset;
        i64_t bytes_transfered;
        i64_t size;
        u8_t *buf;
        queue_p queue; // queue for async messages waiting to be sent
    } tx;

} *selector_p;

#endif

typedef struct poll_t
{
    i64_t code;
    i64_t poll_fd;
    i64_t ipc_fd;
    obj_p replfile;
    obj_p ipcfile;
    freelist_p selectors; // freelist of selectors
    timers_p timers;      // timers heap
} *poll_p;

poll_p poll_init(i64_t port);
nil_t poll_destroy(poll_p poll);
i64_t poll_run(poll_p poll);
i64_t poll_register(poll_p poll, i64_t fd, u8_t version);
nil_t poll_deregister(poll_p poll, i64_t id);

// send ipc messages
obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg);
obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg);

// Exit the app
nil_t poll_exit(poll_p poll, i64_t code);

#endif // POLL_H

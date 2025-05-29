/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef IPC_H
#define IPC_H

#include "rayforce.h"
#include "poll.h"
#include "sock.h"

#define MSG_TYPE_ASYN 0
#define MSG_TYPE_SYNC 1
#define MSG_TYPE_RESP 2

typedef struct ipc_ctx_t {
    u8_t msgtype;
    obj_p name;
} *ipc_ctx_p;

option_t ipc_read_handshake(poll_p poll, selector_p selector);
option_t ipc_read_header(poll_p poll, selector_p selector);
option_t ipc_read_msg(poll_p poll, selector_p selector);
nil_t ipc_on_open(poll_p poll, selector_p selector);
nil_t ipc_on_close(poll_p poll, selector_p selector);
nil_t ipc_on_error(poll_p poll, selector_p selector);
option_t ipc_on_data(poll_p poll, selector_p selector, raw_p data);
nil_t ipc_send_msg(poll_p poll, selector_p selector, obj_p msg, u8_t msgtype);
obj_p ipc_process_msg(poll_p poll, selector_p selector, obj_p msg);

// listen for incoming connections
i64_t ipc_listen(poll_p poll, i64_t port);

// open a connection
i64_t ipc_open(poll_p poll, sock_addr_t *addr, i64_t timeout);

// send messages
obj_p ipc_send(poll_p poll, i64_t id, obj_p msg, u8_t msgtype);

#endif  // IPC_H

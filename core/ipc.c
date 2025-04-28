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

#include "ipc.h"
#include "eval.h"
#include "binary.h"
#include "symbols.h"
#include "error.h"
#include "sock.h"
#include "poll.h"
#include "string.h"
#include "util.h"

// forward declarations
poll_result_t ipc_read_handshake(poll_p poll, selector_p selector);
poll_result_t ipc_read_header(poll_p poll, selector_p selector);
poll_result_t ipc_read_msg_sync(poll_p poll, selector_p selector);
poll_result_t ipc_read_msg_async(poll_p poll, selector_p selector);
poll_result_t ipc_on_open(poll_p poll, selector_p selector);
poll_result_t ipc_on_close(poll_p poll, selector_p selector);
poll_result_t ipc_on_error(poll_p poll, selector_p selector);

poll_result_t ipc_send_handshake(poll_p poll, selector_p selector);
poll_result_t ipc_send_msg(poll_p poll, selector_p selector);
poll_result_t ipc_send_header(poll_p poll, selector_p selector);

nil_t poll_set_usr_fd(i64_t fd) {
    obj_p s, k, v;

    s = symbol(".z.w", 4);
    k = i64(fd);
    v = binary_set(s, k);
    drop_obj(k);
    drop_obj(v);
    drop_obj(s);
}

poll_result_t ipc_listener_accept(poll_p poll, selector_p selector) {
    i64_t fd;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;
    ipc_ctx_p ctx;

    fd = sock_accept(selector->fd);

    if (fd != -1) {
        ctx = (ipc_ctx_p)heap_alloc(sizeof(struct ipc_ctx_t));
        ctx->name = string_from_str("ipc", 4);

        registry.fd = fd;
        registry.type = SELECTOR_TYPE_SOCKET;
        registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
        registry.open_fn = ipc_on_open;
        registry.close_fn = ipc_on_close;
        registry.error_fn = ipc_on_error;
        registry.read_fn = ipc_read_handshake;
        registry.recv_fn = sock_recv;
        registry.data = ctx;

        if (poll_register(poll, &registry) == -1) {
            heap_free(ctx);
            return POLL_ERROR;
        }
    }

    return POLL_OK;
}

poll_result_t ipc_listener_close(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return POLL_OK;
}

poll_result_t ipc_listen(poll_p poll, i64_t port) {
    i64_t fd;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;

    if (poll == NULL)
        return POLL_ERROR;

    fd = sock_listen(port);
    if (fd == -1)
        return POLL_ERROR;

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
    registry.recv_fn = NULL;
    registry.read_fn = ipc_listener_accept;
    registry.close_fn = ipc_listener_close;
    registry.error_fn = NULL;
    registry.data = NULL;

    return poll_register(poll, &registry);
}

poll_result_t ipc_call_usr_cb(poll_p poll, selector_p selector, lit_p sym, i64_t len) {
    UNUSED(poll);
    i64_t clbnm;
    obj_p v, f, *clbfn;

    stack_push(NULL_OBJ);  // null env
    clbnm = symbols_intern(sym, len);
    clbfn = resolve(clbnm);
    stack_pop();  // null env

    // Call the callback if it's a lambda
    if (clbfn != NULL && (*clbfn)->type == TYPE_LAMBDA) {
        poll_set_usr_fd(selector->id);
        stack_push(i64(selector->id));
        v = call(*clbfn, 1);
        drop_obj(stack_pop());
        poll_set_usr_fd(0);
        if (IS_ERR(v)) {
            f = obj_fmt(v, B8_FALSE);
            fprintf(stderr, "Error in user callback: \n%.*s\n", (i32_t)f->len, AS_C8(f));
            drop_obj(f);
        }

        drop_obj(v);
    }

    return POLL_OK;
}

poll_result_t ipc_read_handshake(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t sz, size;
    u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    if (selector->rx.buf->offset > 0 && selector->rx.buf->data[selector->rx.buf->offset - 1] == '\0') {
        // send handshake response
        size = 0;
        while (size < ISIZEOF(handshake)) {
            sz = sock_send(selector->fd, &handshake[size], ISIZEOF(handshake) - size);

            if (sz == -1)
                return POLL_ERROR;

            size += sz;
        }

        // Now we are ready for income messages and can call userspace callback (if any)
        ipc_call_usr_cb(poll, selector, ".z.po", 5);

        selector->rx.read_fn = ipc_read_header;

        poll_rx_buf_request(poll, selector, ISIZEOF(struct header_t));
        poll_rx_buf_reset(poll, selector);

        return POLL_READY;
    }

    return POLL_OK;
}

poll_result_t ipc_read_header(poll_p poll, selector_p selector) {
    UNUSED(poll);

    header_t *header;

    header = (header_t *)selector->rx.buf->data;

    // determine the read function based on the message type
    // selector->rx.msgtype = header->msgtype;
    selector->rx.read_fn = ipc_read_msg_sync;

    // request the buffer for the entire message (including the header)
    poll_rx_buf_request(poll, selector, ISIZEOF(struct header_t) + header->size);

    return POLL_READY;
}

poll_result_t ipc_read_msg_sync(poll_p poll, selector_p selector) {
    UNUSED(poll);

    obj_p v, res;
    ipc_ctx_p ctx;

    ctx = (ipc_ctx_p)selector->data;

    res = de_raw(selector->rx.buf->data, selector->rx.buf->size);

    poll_set_usr_fd(selector->id);

    if (IS_ERR(res) || is_null(res))
        v = res;
    else if (res->type == TYPE_C8) {
        v = ray_eval_str(res, ctx->name);
        drop_obj(res);
    } else {
        v = eval_obj(res);
        drop_obj(res);
    }

    poll_set_usr_fd(0);

    poll_rx_buf_release(poll, selector);
    poll_rx_buf_request(poll, selector, ISIZEOF(struct header_t));

    selector->rx.read_fn = ipc_read_header;

    // respond
    // queue_push(selector->tx.queue, (nil_t *)((i64_t)v | ((i64_t)MSG_TYPE_RESP << 61)));
    // poll_result = _send(poll, selector);

    //     if (poll_result == POLL_ERROR)
    //         poll_deregister(poll, selector->id);
    // } else

    drop_obj(v);

    return POLL_READY;
}

poll_result_t ipc_read_msg_async(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return POLL_OK;
}

poll_result_t ipc_on_open(poll_p poll, selector_p selector) {
    // request the minimal handshake buffer
    return poll_rx_buf_request(poll, selector, 2);
}

poll_result_t ipc_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return POLL_OK;
}

poll_result_t ipc_on_close(poll_p poll, selector_p selector) {
    UNUSED(poll);

    ipc_ctx_p ctx;

    ipc_call_usr_cb(poll, selector, ".z.pc", 5);

    ctx = (ipc_ctx_p)selector->data;
    drop_obj(ctx->name);
    heap_free(ctx);

    return POLL_OK;
}

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);

    // poll_result_t poll_result = POLL_OK;
    // selector_p selector;
    // i32_t result;
    // i64_t idx;
    // obj_p res;
    // fd_set fds;

    //     idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    //     if (idx == NULL_I64)
    //         THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    //     selector = (selector_p)idx;

    //     queue_push(selector->tx.queue, (nil_t *)((i64_t)msg | ((i64_t)MSG_TYPE_SYNC << 61)));

    //     while (B8_TRUE) {
    //         poll_result = _send(poll, selector);

    //         if (poll_result != POLL_OK)
    //             break;

    //         // block on select until we can send
    //         FD_ZERO(&fds);
    //         FD_SET(selector->fd, &fds);
    //         result = select(selector->fd + 1, NULL, &fds, NULL, NULL);

    //         if (result == -1) {
    //             if (errno != EINTR) {
    //                 poll_deregister(poll, selector->id);
    //                 return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error sending message (can't block on send)");
    //             }
    //         }
    //     }

    //     if (poll_result == POLL_ERROR) {
    //         poll_deregister(poll, selector->id);
    //         return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error sending message");
    //     }

    // recv:
    //     while (B8_TRUE) {
    //         poll_result = _recv(poll, selector);

    //         if (poll_result != POLL_OK)
    //             break;

    //         // block on select until we can recv
    //         FD_ZERO(&fds);
    //         FD_SET(selector->fd, &fds);
    //         result = select(selector->fd + 1, &fds, NULL, NULL, NULL);

    //         if (result == -1) {
    //             if (errno != EINTR) {
    //                 poll_deregister(poll, selector->id);
    //                 return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error receiving message (can't block on recv)");
    //             }
    //         }
    //     }

    //     if (poll_result == POLL_ERROR) {
    //         poll_deregister(poll, selector->id);
    //         return sys_error(ERROR_TYPE_OS, "ipc_send_sync: error receiving message");
    //     }

    //     // recv until we get response
    //     switch (selector->rx.msgtype) {
    //         case MSG_TYPE_RESP:
    //             res = read_obj(selector);
    //             break;
    //         default:
    //             process_request(poll, selector);
    //             goto recv;
    //     }

    return NULL_OBJ;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);

    // idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    // if (idx == NULL_I64)
    //     THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    // selector = (selector_p)idx;
    // if (selector == NULL)
    //     THROW(ERR_IO, "ipc_send_async: invalid socket fd: %lld", id);

    // queue_push(selector->tx.queue, (nil_t *)msg);

    // if (_send(poll, selector) == POLL_ERROR)
    //     THROW(ERR_IO, "ipc_send_async: error sending message");

    return NULL_OBJ;
}

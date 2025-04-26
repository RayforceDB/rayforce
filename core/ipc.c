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

// forward declarations
poll_result_t ipc_read_handshake(poll_p poll, selector_p selector);
poll_result_t ipc_read_header(poll_p poll, selector_p selector);
poll_result_t ipc_read_msg_sync(poll_p poll, selector_p selector);
poll_result_t ipc_read_msg_async(poll_p poll, selector_p selector);
poll_result_t ipc_on_error(poll_p poll, selector_p selector);
poll_result_t ipc_on_close(poll_p poll, selector_p selector);

i64_t ipc_recv(poll_p poll, selector_p selector) {
    return sock_recv(selector->fd, &AS_U8(selector->rx.buf)[selector->rx.size],
                     selector->rx.buf->len - selector->rx.size);
}

i64_t ipc_send(poll_p poll, selector_p selector) {
    return sock_send(selector->fd, &AS_U8(selector->tx.buf)[selector->tx.size],
                     selector->tx.buf->len - selector->tx.size);
}

nil_t poll_set_usr_fd(i64_t fd) {
    obj_p s, k, v;

    s = symbol(".z.w", 4);
    k = i64(fd);
    v = binary_set(s, k);
    drop_obj(k);
    drop_obj(v);
    drop_obj(s);
}

i64_t ipc_listener_accept(poll_p poll, selector_p selector) {
    i64_t fd;
    struct poll_registry_t registry;
    ipc_ctx_p ctx;

    fd = sock_accept(selector->fd);

    if (fd != -1) {
        ctx = (ipc_ctx_p)heap_alloc(sizeof(struct ipc_ctx_t));
        ctx->name = string_from_str("ipc", 4);

        registry.fd = fd;
        registry.type = SELECTOR_TYPE_SOCKET;
        registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
        registry.recv_fn = ipc_recv;
        registry.read_fn = ipc_read_handshake;
        registry.close_fn = ipc_on_close;
        registry.error_fn = ipc_on_error;
        registry.data = ctx;

        if (poll_register(poll, &registry) == -1) {
            heap_free(ctx);
            return -1;
        }
    }

    return fd;
}

i64_t ipc_listener_close(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return 0;
}

i64_t ipc_listen(poll_p poll, i64_t port) {
    i64_t fd;
    struct poll_registry_t registry;

    if (poll == NULL)
        return -1;

    fd = sock_listen(port);
    if (fd == -1)
        return -1;

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
    registry.recv_fn = ipc_listener_accept;
    registry.read_fn = NULL;
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

    return POLL_READY;
}

poll_result_t ipc_read_handshake(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size;

    if (AS_U8(selector->rx.buf)[selector->rx.size - 1] == '\0') {
        // send handshake response
        if (ipc_send_handshake(poll, selector) == POLL_ERROR)
            return POLL_ERROR;

        // Now we are ready for income messages and can call userspace callback (if any)
        ipc_call_usr_cb(poll, selector, ".z.po", 5);

        selector->rx.read_fn = ipc_read_header;
        selector->rx.size = 0;

        return ipc_read_header(poll, selector);
    }

    return POLL_PENDING;
}

poll_result_t ipc_read_header(poll_p poll, selector_p selector) {
    UNUSED(poll);

    header_t *header;

    header = (header_t *)AS_U8(selector->rx.buf);

    if (poll_rx_buf_request(poll, selector, header->size + sizeof(struct header_t)) == -1)
        return POLL_ERROR;

    // determine the read function based on the message type
    // selector->rx.msgtype = header->msgtype;
    selector->rx.read_fn = ipc_read_msg_sync;

    return POLL_READY;
}

poll_result_t ipc_read_msg_sync(poll_p poll, selector_p selector) {
    UNUSED(poll);

    obj_p v, res;
    ipc_ctx_p ctx;

    ctx = (ipc_ctx_p)selector->data;

    res = de_raw(selector->rx.buf, selector->rx.size);
    poll_rx_buf_release(poll, selector);

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

    // respond
    // queue_push(selector->tx.queue, (nil_t *)((i64_t)v | ((i64_t)MSG_TYPE_RESP << 61)));
    // poll_result = _send(poll, selector);

    //     if (poll_result == POLL_ERROR)
    //         poll_deregister(poll, selector->id);
    // } else
    //     drop_obj(v);

    return POLL_READY;
}

poll_result_t ipc_read_msg_async(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return POLL_READY;
}

poll_result_t ipc_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);

    return POLL_READY;
}

poll_result_t ipc_on_close(poll_p poll, selector_p selector) {
    UNUSED(poll);

    ipc_ctx_p ctx;

    ctx = (ipc_ctx_p)selector->data;
    drop_obj(ctx->name);
    drop_obj(ctx);

    return POLL_READY;
}

poll_result_t ipc_send_handshake(poll_p poll, selector_p selector) {
    UNUSED(poll);

    // i64_t sz, size;
    // u8_t handshake[2] = {RAYFORCE_VERSION, 0x00};

    // size = 0;
    // while (size < (i64_t)sizeof(handshake)) {
    //     sz = sock_send(selector->fd, &handshake[size], sizeof(handshake) - size);

    //     if (sz == -1)
    //         return POLL_ERROR;

    //     size += sz;
    // }

    return POLL_READY;
}

// Send
poll_result_t _send(poll_p poll, selector_p selector) {
    //     i64_t size;
    //     obj_p obj;
    //     nil_t *v;
    //     i8_t msg_type = MSG_TYPE_RESP;
    //     struct epoll_event ev;

    // send:
    //     while (selector->tx.size < selector->tx.size) {
    //         size = sock_send(selector->fd, &selector->tx.buf[selector->tx.size],
    //                          selector->tx.size - selector->tx.size);
    //         if (size == -1)
    //             return POLL_ERROR;
    //         else if (size == 0) {
    //             // setup epoll for EPOLLOUT only if it's not already set
    //             if (!selector->tx.isset) {
    //                 selector->tx.isset = B8_TRUE;
    //                 ev.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
    //                 ev.data.ptr = selector;
    //                 if (epoll_ctl(poll->fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
    //                     return POLL_ERROR;
    //             }

    //             return POLL_PENDING;
    //         }

    //         selector->tx.size += size;
    //     }

    //     heap_free(selector->tx.buf);
    //     selector->tx.buf = NULL;
    //     selector->tx.size = 0;
    //     selector->tx.size = 0;

    //     v = queue_pop(selector->tx.queue);

    //     if (v != NULL) {
    //         obj = (obj_p)((i64_t)v & ~(3ll << 61));
    //         msg_type = (((i64_t)v & (3ll << 61)) >> 61);
    //         size = ser_raw(&selector->tx.buf, obj);
    //         selector->tx.size = size;
    //         drop_obj(obj);
    //         if (size == -1)
    //             return POLL_ERROR;

    //         ((header_t *)selector->tx.buf)->msgtype = msg_type;
    //         goto send;
    //     }

    //     // remove EPOLLOUT only if it's set
    //     if (selector->tx.isset) {
    //         selector->tx.isset = B8_FALSE;
    //         ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    //         ev.data.ptr = selector;
    //         if (epoll_ctl(poll->fd, EPOLL_CTL_MOD, selector->fd, &ev) == -1)
    //             return POLL_ERROR;
    //     }

    return POLL_READY;
}

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    poll_result_t poll_result = POLL_PENDING;
    selector_p selector;
    i32_t result;
    i64_t idx;
    obj_p res;
    fd_set fds;

    //     idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    //     if (idx == NULL_I64)
    //         THROW(ERR_IO, "ipc_send_sync: invalid socket fd: %lld", id);

    //     selector = (selector_p)idx;

    //     queue_push(selector->tx.queue, (nil_t *)((i64_t)msg | ((i64_t)MSG_TYPE_SYNC << 61)));

    //     while (B8_TRUE) {
    //         poll_result = _send(poll, selector);

    //         if (poll_result != POLL_PENDING)
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

    //         if (poll_result != POLL_PENDING)
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

    return res;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    i64_t idx;
    selector_p selector;

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

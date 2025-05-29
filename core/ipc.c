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
#include "poll.h"
#include "string.h"
#include "util.h"
#include "log.h"

// ============================================================================
// Listener Management
// ============================================================================

option_t ipc_listener_accept(poll_p poll, selector_p selector) {
    i64_t fd;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;
    ipc_ctx_p ctx;

    LOG_TRACE("Accepting new connection on fd %lld", selector->fd);
    fd = sock_accept(selector->fd);
    LOG_DEBUG("New connection accepted on fd %lld", fd);

    if (fd != -1) {
        ctx = (ipc_ctx_p)heap_alloc(sizeof(struct ipc_ctx_t));
        ctx->name = string_from_str("ipc", 4);
        ctx->msgtype = MSG_TYPE_RESP;

        registry.fd = fd;
        registry.type = SELECTOR_TYPE_SOCKET;
        registry.events =
            POLL_EVENT_READ | POLL_EVENT_WRITE | POLL_EVENT_ERROR | POLL_EVENT_HUP | POLL_EVENT_RDHUP | POLL_EVENT_EDGE;
        registry.open_fn = ipc_on_open;
        registry.close_fn = ipc_on_close;
        registry.error_fn = ipc_on_error;
        registry.read_fn = ipc_read_handshake;
        registry.recv_fn = sock_recv;
        registry.send_fn = sock_send;
        registry.data_fn = ipc_on_data;
        registry.data = ctx;

        if (poll_register(poll, &registry) == -1) {
            LOG_ERROR("Failed to register new connection in poll registry");
            heap_free(ctx);
            return option_error(
                sys_error(ERR_IO, "ipc_listener_accept: failed to register new connection in poll registry"));
        }

        LOG_INFO("New connection registered successfully");
    }

    return option_none();
}

nil_t ipc_listener_close(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
}

i64_t ipc_listen(poll_p poll, i64_t port) {
    i64_t fd;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;

    if (poll == NULL)
        return -1;

    fd = sock_listen(port);
    if (fd == -1)
        return -1;

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
    registry.recv_fn = NULL;
    registry.read_fn = ipc_listener_accept;
    registry.close_fn = ipc_listener_close;
    registry.error_fn = NULL;
    registry.data = NULL;

    LOG_DEBUG("Registering listener on port %lld", port);

    return poll_register(poll, &registry);
}

// ============================================================================
// User Callback Management
// ============================================================================

nil_t ipc_call_usr_cb(poll_p poll, selector_p selector, lit_p sym, i64_t len) {
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
}

// ============================================================================
// Connection Management
// ============================================================================

i64_t ipc_open(poll_p poll, sock_addr_t *addr, i64_t timeout) {
    i64_t fd, id;
    selector_p selector;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;
    ipc_ctx_p ctx;
    u8_t buf[2] = {RAYFORCE_VERSION, 0x00};

    LOG_DEBUG("Opening connection to %s:%lld", addr->ip, addr->port);

    fd = sock_open(addr, timeout);
    LOG_DEBUG("Connection opened on fd %lld", fd);

    if (fd == -1)
        return -1;

    if (sock_send(fd, buf, 2) == -1)
        return -1;

    if (sock_recv(fd, buf, 1) == -1)
        return -1;

    LOG_TRACE("Setting socket to non-blocking mode");
    sock_set_nonblocking(fd, B8_TRUE);
    LOG_TRACE("Socket set to non-blocking mode");

    ctx = (ipc_ctx_p)heap_alloc(sizeof(struct ipc_ctx_t));
    ctx->name = string_from_str("ipc", 4);

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
    registry.recv_fn = sock_recv;
    registry.send_fn = sock_send;
    registry.read_fn = ipc_read_header;
    registry.close_fn = ipc_on_close;
    registry.error_fn = ipc_on_error;
    registry.data = ctx;

    LOG_DEBUG("Registering connection in poll registry");
    id = poll_register(poll, &registry);
    LOG_DEBUG("Connection registered in poll registry with id %lld", id);

    // request rx buffer
    selector = poll_get_selector(poll, id);
    if (poll_rx_buf_request(poll, selector, ISIZEOF(struct ipc_header_t)) == -1) {
        poll_deregister(poll, id);
        return -1;
    }

    return id;
}

// ============================================================================
// Message Reading
// ============================================================================

option_t ipc_read_handshake(poll_p poll, selector_p selector) {
    UNUSED(poll);

    poll_buffer_p buf;

    if (selector->rx.buf == NULL) {
        LOG_DEBUG("No handshake buffer received, closing connection");
        poll_deregister(poll, selector->id);
        return option_error(sys_error(ERR_IO, "ipc_read_handshake: no handshake buffer received, closing connection"));
    }

    if (selector->rx.buf->offset > 0 && selector->rx.buf->data[selector->rx.buf->offset - 1] == '\0') {
        LOG_DEBUG("Handshake received, sending response");

        // send handshake response (single byte version)
        buf = poll_buf_create(1);
        buf->data[0] = RAYFORCE_VERSION;
        poll_send_buf(poll, selector, buf);

        // Now we are ready for income messages and can call userspace callback (if any)
        ipc_call_usr_cb(poll, selector, ".z.po", 5);

        selector->rx.read_fn = ipc_read_header;
        LOG_DEBUG("Handshake completed, switching to header reading mode");

        poll_rx_buf_request(poll, selector, ISIZEOF(struct ipc_header_t));

        return option_some(NULL);
    }

    // extend the buffer to the next 1 byte
    poll_rx_buf_extend(poll, selector, 1);

    return option_some(NULL);
}

option_t ipc_read_header(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t msgtype;
    i64_t msgsize;
    ipc_header_t *header;
    ipc_ctx_p ctx;

    LOG_DEBUG("Reading header from connection %lld", selector->id);

    ctx = (ipc_ctx_p)selector->data;
    header = (ipc_header_t *)selector->rx.buf->data;
    msgtype = header->msgtype;
    msgsize = header->size;

    LOG_TRACE("Header read: {.prefix: 0x%08x, .version: %d, .flags: %d, .endian: %d, .msgtype: %d, .size: %lld}",
              header->prefix, header->version, header->flags, header->endian, header->msgtype, header->size);

    // request the buffer for the entire message (including the header)
    LOG_DEBUG("Requesting buffer for message of size %lld", ISIZEOF(struct ipc_header_t) + msgsize);
    poll_rx_buf_extend(poll, selector, msgsize);

    LOG_DEBUG("Switching to message reading mode");
    selector->rx.read_fn = ipc_read_msg;
    ctx->msgtype = msgtype;

    return option_some(NULL);
}

option_t ipc_read_msg(poll_p poll, selector_p selector) {
    UNUSED(poll);

    i64_t size;
    obj_p res;
    ipc_header_t *header;

    LOG_DEBUG("Reading message from connection %lld", selector->id);
    header = (ipc_header_t *)selector->rx.buf->data;
    size = header->size;
    LOG_DEBUG("Message size: %lld", size);
    res = de_raw(selector->rx.buf->data + ISIZEOF(struct ipc_header_t), &size);
    LOG_DEBUG("Message read");

    // Prepare for the next message
    poll_rx_buf_request(poll, selector, ISIZEOF(struct ipc_header_t));
    selector->rx.read_fn = ipc_read_header;

    return option_some(res);
}

// ============================================================================
// Event Handlers
// ============================================================================

obj_p ipc_process_msg(poll_p poll, selector_p selector, obj_p msg) {
    UNUSED(poll);

    obj_p res;
    ipc_ctx_p ctx;

    ctx = (ipc_ctx_p)selector->data;

    if (IS_ERR(msg) || is_null(msg))
        res = msg;
    else if (msg->type == TYPE_C8) {
        LOG_TRACE("Evaluating string message: %.*s", (i32_t)msg->len, AS_C8(msg));
        res = ray_eval_str(msg, ctx->name);
        drop_obj(msg);
    } else {
        LOG_TRACE("Evaluating object message");
        res = eval_obj(msg);
        drop_obj(msg);
    }

    LOG_TRACE_OBJ("Resulting object: ", res);

    return res;
}

nil_t ipc_send_msg(poll_p poll, selector_p selector, obj_p msg, u8_t msgtype) {
    i64_t size;
    poll_buffer_p buf;
    ipc_header_t *header;

    LOG_TRACE("Serializing message");
    size = size_obj(msg);
    buf = poll_buf_create(ISIZEOF(struct ipc_header_t) + size);

    header = (ipc_header_t *)buf->data;
    header->prefix = SERDE_PREFIX;
    header->version = RAYFORCE_VERSION;
    header->flags = 0x00;
    header->endian = 0x00;
    header->msgtype = msgtype;
    header->size = size;

    ser_raw(buf->data + ISIZEOF(struct ipc_header_t), msg);
    LOG_DEBUG("Sending message of size %lld", size);
    poll_send_buf(poll, selector, buf);
    LOG_DEBUG("Message sent");
}

option_t ipc_on_data(poll_p poll, selector_p selector, raw_p data) {
    UNUSED(poll);

    LOG_TRACE("Received data from connection %lld", selector->id);

    ipc_ctx_p ctx;
    obj_p v, res;

    ctx = (ipc_ctx_p)selector->data;
    res = (obj_p)data;

    poll_set_usr_fd(selector->id);
    v = ipc_process_msg(poll, selector, res);
    poll_set_usr_fd(0);

    // Send a response if the message is a synchronous request
    if (ctx->msgtype == MSG_TYPE_SYNC)
        ipc_send_msg(poll, selector, v, MSG_TYPE_RESP);

    drop_obj(v);

    return option_some(NULL);
}

nil_t ipc_on_open(poll_p poll, selector_p selector) {
    LOG_DEBUG("Connection opened, requesting handshake buffer");
    // request the minimal handshake buffer
    poll_rx_buf_request(poll, selector, 2);
}

nil_t ipc_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    LOG_ERROR("Error occurred on connection %lld", selector->id);
}

nil_t ipc_on_close(poll_p poll, selector_p selector) {
    ipc_ctx_p ctx;

    LOG_INFO("Connection %lld closed", selector->id);

    // Clear any pending read operations
    selector->rx.read_fn = NULL;
    if (selector->rx.buf != NULL) {
        poll_rx_buf_release(poll, selector);
    }

    // Call user callback before freeing context
    ipc_call_usr_cb(poll, selector, ".z.pc", 5);

    // Free context
    ctx = (ipc_ctx_p)selector->data;
    if (ctx != NULL) {
        drop_obj(ctx->name);
        heap_free(ctx);
    }
}

// ============================================================================
// Message Sending
// ============================================================================

obj_p ipc_send(poll_p poll, i64_t id, obj_p msg, u8_t msgtype) {
    selector_p selector;
    option_t result;
    obj_p res;
    ipc_ctx_p ctx;

    LOG_DEBUG("Starting synchronous IPC send for id %lld", id);

    selector = poll_get_selector(poll, id);
    if (selector == NULL) {
        LOG_ERROR("Invalid selector for fd %lld", id);
        return sys_error(ERR_IO, "ipc_send: invalid selector for fd");
    }

    ctx = (ipc_ctx_p)selector->data;
    ipc_send_msg(poll, selector, msg, msgtype);

    res = NULL_OBJ;

    // wait for the response
    if (msgtype == MSG_TYPE_SYNC) {
        do {
            LOG_DEBUG("Waiting for response from connection %lld", selector->id);
            result = poll_block_on(poll, selector);
            LOG_DEBUG("Response received from connection %lld RESULT: %s", selector->id,
                      option_is_some(&result) ? "some" : "none");
            if (option_is_some(&result) && result.value != NULL) {
                res = option_take(&result);
                // If the message is a response, break the loop
                if (ctx->msgtype == MSG_TYPE_RESP)
                    break;

                // Process the request otherwise
                res = ipc_process_msg(poll, selector, res);
                drop_obj(res);
            } else if (option_is_error(&result)) {
                LOG_ERROR("Error occurred on connection %lld", selector->id);
                return option_take(&result);
            }
        } while (option_is_none(&result));
    }

    return res;
}

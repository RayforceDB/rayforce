/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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
#include <string.h>
#include "../../core/def.h"
#include "../../core/rayforce.h"
#include "../../core/poll.h"
#include "../../core/sock.h"
#include "../../core/error.h"
#include "../../core/log.h"
#include "../../core/str.h"
#include "../../core/runtime.h"
#include "../../core/vary.h"
#include "raykx.h"
#include "serde.h"

// ============================================================================
// Windows Implementation - Uses blocking sockets for simplicity
// ============================================================================
#if defined(OS_WINDOWS)

// Connection context for Windows
typedef struct raykx_conn_t {
    i64_t fd;           // socket file descriptor
    obj_p name;         // connection name
    u8_t msgtype;       // last message type
    u8_t compressed;    // compression flag
} *raykx_conn_p;

// Simple connection storage (up to 256 connections)
#define MAX_CONNECTIONS 256
static raykx_conn_p connections[MAX_CONNECTIONS] = {0};

static i64_t find_free_slot(void) {
    for (i64_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i] == NULL)
            return i;
    }
    return -1;
}

static option_t raykx_decompress(const u8_t* compressed, i64_t compressed_size, u8_t** decompressed,
                                 i64_t* decompressed_size) {
    if (compressed_size < (i64_t)sizeof(u32_t))
        return option_error(err_os());

    i64_t i = 0;
    i64_t n = 0;
    i64_t f = 0;
    i64_t s = 0;
    i64_t p = 0;
    i64_t d = 4;  // Skip the header size

    const u32_t* header_size = (const u32_t*)compressed;
    i64_t len = (i64_t)(*header_size - sizeof(struct raykx_header_t));

    if (len == 0)
        return option_error(err_os());

    u32_t buffer[256] = {0};
    u8_t* result = (u8_t*)heap_alloc(len);
    if (result == NULL)
        return option_error(err_limit(0));

    while (s < len) {
        if (i == 0) {
            f = compressed[d];
            d++;
            i = 1;
        }
        if (f & i) {
            i64_t r = buffer[compressed[d]];
            d++;
            result[s] = result[r];
            s++;
            r++;
            result[s] = result[r];
            s++;
            r++;
            n = compressed[d];
            d++;
            for (i64_t m = 0; m < n; m++) {
                result[s + m] = result[r + m];
            }
        } else {
            result[s] = compressed[d];
            s++;
            d++;
        }
        while (p < s - 1) {
            i64_t pp = p;
            p++;
            buffer[result[pp] ^ result[p]] = pp;
        }
        if (f & i) {
            s += n;
            p = s;
        }
        i *= 2;
        if (i == 256) {
            i = 0;
        }
    }

    *decompressed = result;
    *decompressed_size = len;
    return option_none();
}

// Blocking receive helper
static i64_t recv_all(i64_t fd, u8_t *buf, i64_t size) {
    i64_t total = 0;
    while (total < size) {
        i64_t n = recv((SOCKET)fd, (char*)(buf + total), (int)(size - total), 0);
        if (n <= 0) {
            if (n == 0) return -1;  // connection closed
            i32_t err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                Sleep(1);  // Small delay for non-blocking
                continue;
            }
            return -1;
        }
        total += n;
    }
    return total;
}

// Blocking send helper
static i64_t send_all(i64_t fd, u8_t *buf, i64_t size) {
    i64_t total = 0;
    while (total < size) {
        i64_t n = send((SOCKET)fd, (const char*)(buf + total), (int)(size - total), 0);
        if (n <= 0) {
            if (n == 0) return -1;
            i32_t err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                Sleep(1);
                continue;
            }
            return -1;
        }
        total += n;
    }
    return total;
}

obj_p raykx_listen(obj_p x) {
    UNUSED(x);
    // Server mode not yet supported on Windows
    LOG_ERROR("raykx_listen not supported on Windows");
    return err_os();
}

obj_p raykx_hopen(obj_p addr) {
    i64_t fd, slot;
    raykx_conn_p conn;
    u8_t handshake[2] = {0x03, 0x00};  // KDB+ handshake
    sock_addr_t sock_addr;

    LOG_DEBUG("Opening KDB+ connection to %.*s", (i32_t)addr->len, AS_C8(addr));

    // Find a free slot
    slot = find_free_slot();
    if (slot == -1) {
        LOG_ERROR("No free connection slots");
        return err_limit(0);
    }

    // Parse address string into sock_addr_t
    if (sock_addr_from_str(AS_C8(addr), addr->len, &sock_addr) == -1) {
        return err_os();
    }

    // Open socket connection (blocking)
    fd = sock_open(&sock_addr, 5000);
    LOG_DEBUG("Connection opened on fd %lld", fd);

    if (fd == -1)
        return err_os();

    // Send handshake
    if (send_all(fd, handshake, 2) == -1) {
        sock_close(fd);
        return err_os();
    }

    // Receive handshake response
    if (recv_all(fd, handshake, 1) == -1) {
        sock_close(fd);
        return err_os();
    }

    LOG_DEBUG("Handshake response: %d", handshake[0]);

    // Create connection context
    conn = (raykx_conn_p)heap_alloc(sizeof(struct raykx_conn_t));
    conn->fd = fd;
    conn->name = string_from_str("raykx", 6);
    conn->msgtype = KDB_MSG_SYNC;
    conn->compressed = 0;

    connections[slot] = conn;

    return i64(slot);
}

obj_p raykx_hclose(obj_p handle) {
    i64_t slot;
    raykx_conn_p conn;

    if (handle->type != -TYPE_I64)
        return err_type(-TYPE_I64, handle->type, 0, 0);

    slot = handle->i64;
    if (slot < 0 || slot >= MAX_CONNECTIONS || connections[slot] == NULL) {
        return err_domain(0, 0);
    }

    conn = connections[slot];
    sock_close(conn->fd);
    drop_obj(conn->name);
    heap_free(conn);
    connections[slot] = NULL;

    return null(0);
}

obj_p raykx_send(obj_p handle, obj_p msg) {
    i64_t slot, size;
    raykx_conn_p conn;
    u8_t *buf;
    struct raykx_header_t header;
    obj_p res;

    if (handle->type != -TYPE_I64)
        return err_type(-TYPE_I64, handle->type, 0, 0);

    slot = handle->i64;
    if (slot < 0 || slot >= MAX_CONNECTIONS || connections[slot] == NULL) {
        return err_domain(0, 0);
    }

    conn = connections[slot];

    LOG_DEBUG("Starting KDB+ send on slot %lld", slot);

    // Serialize message
    size = raykx_size_obj(msg);
    LOG_TRACE("Serialized message size: %lld", size);

    // Allocate buffer for header + message
    buf = (u8_t*)heap_alloc(sizeof(struct raykx_header_t) + size);
    if (buf == NULL) {
        return err_limit(0);
    }

    // Serialize the message after header
    size = raykx_ser_obj(buf + sizeof(struct raykx_header_t), msg);
    if (size < 0) {
        heap_free(buf);
        return err_os();
    }

    // Set up header
    header.endianness = 1;
    header.msgtype = KDB_MSG_SYNC;
    header.compressed = 0;
    header.reserved = 0;
    header.size = (u32_t)(size + sizeof(struct raykx_header_t));

    memcpy(buf, &header, sizeof(struct raykx_header_t));

    LOG_TRACE("Sending message: size=%u", header.size);

    // Send message
    if (send_all(conn->fd, buf, header.size) == -1) {
        heap_free(buf);
        return err_os();
    }
    heap_free(buf);

    // Receive response header
    if (recv_all(conn->fd, (u8_t*)&header, sizeof(struct raykx_header_t)) == -1) {
        return err_os();
    }

    LOG_TRACE("Response header: size=%u, msgtype=%d, compressed=%d",
              header.size, header.msgtype, header.compressed);

    conn->msgtype = header.msgtype;
    conn->compressed = header.compressed;

    // Receive response body
    size = header.size - sizeof(struct raykx_header_t);
    buf = (u8_t*)heap_alloc(size);
    if (buf == NULL) {
        return err_limit(0);
    }

    if (recv_all(conn->fd, buf, size) == -1) {
        heap_free(buf);
        return err_os();
    }

    // Deserialize response
    if (conn->compressed) {
        u8_t* decompressed;
        i64_t decompressed_size;
        option_t decomp_result = raykx_decompress(buf, size, &decompressed, &decompressed_size);
        if (option_is_error(&decomp_result)) {
            heap_free(buf);
            return option_take(&decomp_result);
        }
        res = raykx_des_obj(decompressed, &decompressed_size);
        heap_free(decompressed);
    } else {
        res = raykx_des_obj(buf, &size);
    }

    heap_free(buf);

    LOG_DEBUG("KDB+ send completed");

    return res;
}

#else
// Non-Windows implementation using poll callbacks

// Forward declarations
static option_t raykx_read_handshake(poll_p poll, selector_p selector);
static option_t raykx_read_header(poll_p poll, selector_p selector);
static option_t raykx_read_msg(poll_p poll, selector_p selector);
static nil_t raykx_send_msg(poll_p poll, selector_p selector, obj_p msg, u8_t msgtype);
static nil_t raykx_on_error(poll_p poll, selector_p selector);
static nil_t raykx_on_close(poll_p poll, selector_p selector);
static nil_t raykx_on_open(poll_p poll, selector_p selector);
static option_t raykx_on_data(poll_p poll, selector_p selector, raw_p data);

// ============================================================================
// Listener Management
// ============================================================================

option_t raykx_listener_accept(poll_p poll, selector_p selector) {
    i64_t fd, id;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;
    raykx_ctx_p ctx;

    LOG_TRACE("Accepting new connection on fd %lld", selector->fd);
    fd = sock_accept(selector->fd);
    LOG_DEBUG("New connection accepted on fd %lld", fd);

    if (fd != -1) {
        ctx = (raykx_ctx_p)heap_alloc(sizeof(struct raykx_ctx_t));
        ctx->name = string_from_str("raykx", 6);
        ctx->msgtype = KDB_MSG_RESP;
        ctx->compressed = 0;

        registry.fd = fd;
        registry.type = SELECTOR_TYPE_SOCKET;
        registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
        registry.open_fn = raykx_on_open;
        registry.close_fn = raykx_on_close;
        registry.error_fn = raykx_on_error;
        registry.read_fn = raykx_read_handshake;
        registry.recv_fn = sock_recv;
        registry.send_fn = sock_send;
        registry.data_fn = raykx_on_data;
        registry.data = ctx;

        id = poll_register(poll, &registry);

        if (id == -1) {
            LOG_ERROR("Failed to register new connection in poll registry");
            heap_free(ctx);
            return option_error(err_os());
        }

        LOG_INFO("New connection registered successfully");
    }

    return option_none();
}

nil_t raykx_listener_close(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
}

obj_p raykx_listen(obj_p x) {
    i64_t fd, port;
    poll_p poll = runtime_get()->poll;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;

    if (x->type != -TYPE_I64)
        return err_type(-TYPE_I64, x->type, 0, 0);

    port = x->i64;

    if (poll == NULL)
        return err_os();

    fd = sock_listen(port);
    if (fd == -1)
        return err_os();

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events = POLL_EVENT_READ | POLL_EVENT_ERROR | POLL_EVENT_HUP;
    registry.recv_fn = NULL;
    registry.read_fn = raykx_listener_accept;
    registry.close_fn = raykx_listener_close;
    registry.error_fn = NULL;
    registry.data = NULL;

    LOG_DEBUG("Registering listener on port %lld", port);

    return i64(poll_register(poll, &registry));
}

// ============================================================================
// Connection Management
// ============================================================================

obj_p raykx_hopen(obj_p addr) {
    i64_t fd, id;
    struct poll_registry_t registry = ZERO_INIT_STRUCT;
    raykx_ctx_p ctx;
    u8_t handshake[2] = {0x03, 0x00};  // KDB+ handshake
    sock_addr_t sock_addr;
    selector_p selector;

    LOG_DEBUG("Opening KDB+ connection to %.*s", (i32_t)addr->len, AS_C8(addr));

    // Parse address string into sock_addr_t
    if (sock_addr_from_str(AS_C8(addr), addr->len, &sock_addr) == -1) {
        return err_os();
    }

    // Open socket connection
    fd = sock_open(&sock_addr, 5000);  // 5 second timeout
    LOG_DEBUG("Connection opened on fd %lld", fd);

    if (fd == -1)
        return err_os();

    // Send handshake
    if (sock_send(fd, handshake, 2) == -1)
        return err_os();

    // Receive handshake response
    if (sock_recv(fd, handshake, 1) == -1)
        return err_os();

    LOG_DEBUG("Handshake response: %d", handshake[0]);

    LOG_TRACE("Setting socket to non-blocking mode");
    sock_set_nonblocking(fd, B8_TRUE);
    LOG_TRACE("Socket set to non-blocking mode");

    ctx = (raykx_ctx_p)heap_alloc(sizeof(struct raykx_ctx_t));
    ctx->name = string_from_str("raykx", 6);
    ctx->msgtype = KDB_MSG_SYNC;
    ctx->compressed = 0;

    registry.fd = fd;
    registry.type = SELECTOR_TYPE_SOCKET;
    registry.events =
        POLL_EVENT_READ | POLL_EVENT_WRITE | POLL_EVENT_ERROR | POLL_EVENT_HUP | POLL_EVENT_RDHUP | POLL_EVENT_EDGE;
    registry.recv_fn = sock_recv;
    registry.send_fn = sock_send;
    registry.read_fn = raykx_read_header;
    registry.close_fn = raykx_on_close;
    registry.error_fn = raykx_on_error;
    registry.data_fn = raykx_on_data;
    registry.data = ctx;

    LOG_DEBUG("Registering connection in poll registry");
    id = poll_register(runtime_get()->poll, &registry);
    LOG_DEBUG("Connection registered in poll registry with id %lld", id);

    selector = poll_get_selector(runtime_get()->poll, id);
    poll_rx_buf_request(runtime_get()->poll, selector, ISIZEOF(struct raykx_header_t));

    return i64(id);
}

obj_p raykx_hclose(obj_p fd) {
    if (fd->type != -TYPE_I64)
        return err_type(-TYPE_I64, fd->type, 0, 0);

    poll_deregister(runtime_get()->poll, fd->i64);

    return null(0);
}

// ============================================================================
// Message Reading
// ============================================================================

option_t raykx_read_handshake(poll_p poll, selector_p selector) {
    UNUSED(poll);

    poll_buffer_p buf;

    LOG_DEBUG("Reading handshake from connection %lld", selector->id);

    if (selector->rx.buf->offset > 0 && selector->rx.buf->data[selector->rx.buf->offset - 1] == '\0') {
        LOG_TRACE("Handshake from connection %lld: '%s'", selector->id, selector->rx.buf->data);

        // send handshake response (single byte version)
        buf = poll_buf_create(1);
        buf->data[0] = 0x03;
        poll_send_buf(poll, selector, buf);

        selector->rx.read_fn = raykx_read_header;
        LOG_DEBUG("Handshake completed, switching to header reading mode");

        poll_rx_buf_request(poll, selector, ISIZEOF(struct raykx_header_t));

        return option_some(NULL);
    }

    // extend the buffer to the next 1 byte
    poll_rx_buf_extend(poll, selector, 1);

    return option_some(NULL);
}

static option_t raykx_read_header(poll_p poll, selector_p selector) {
    UNUSED(poll);
    raykx_header_p header;
    raykx_ctx_p ctx;
    i64_t msg_size;

    LOG_DEBUG("Reading KDB+ message header from connection %lld", selector->id);

    ctx = (raykx_ctx_p)selector->data;
    header = (raykx_header_p)selector->rx.buf->data;

    LOG_TRACE("Header read: {.endianness: %d, .msgtype: %d, .compressed: %d, .reserved: %d, .size: %lld}",
              header->endianness, header->msgtype, header->compressed, header->reserved, header->size);

    // Store message size before requesting new buffer
    msg_size = header->size - ISIZEOF(struct raykx_header_t);
    ctx->msgtype = header->msgtype;
    ctx->compressed = header->compressed;

    // request the buffer for the entire message (including the header)
    LOG_DEBUG("Requesting buffer for message of size %lld", msg_size);
    poll_rx_buf_request(poll, selector, msg_size);

    LOG_DEBUG("Switching to message reading mode");
    selector->rx.read_fn = raykx_read_msg;

    return option_some(NULL);
}

static option_t raykx_decompress(const u8_t* compressed, i64_t compressed_size, u8_t** decompressed,
                                 i64_t* decompressed_size) {
    if (compressed_size < (i64_t)sizeof(u32_t))
        return option_error(err_os());

    i64_t i = 0;
    i64_t n = 0;
    i64_t f = 0;
    i64_t s = 0;
    i64_t p = 0;
    i64_t d = 4;  // Skip the header size

    // Get uncompressed length from the first 4 bytes (minus header size)
    const u32_t* header_size = (const u32_t*)compressed;
    i64_t len = (i64_t)(*header_size - sizeof(struct raykx_header_t));

    if (len == 0)
        return option_error(err_os());

    u32_t buffer[256] = {0};
    u8_t* result = (u8_t*)heap_alloc(len);
    if (result == NULL)
        return option_error(err_limit(0));

    while (s < len) {
        if (i == 0) {
            f = compressed[d];
            d++;
            i = 1;
        }
        if (f & i) {
            i64_t r = buffer[compressed[d]];
            d++;
            result[s] = result[r];
            s++;
            r++;
            result[s] = result[r];
            s++;
            r++;
            n = compressed[d];
            d++;
            for (i64_t m = 0; m < n; m++) {
                result[s + m] = result[r + m];
            }
        } else {
            result[s] = compressed[d];
            s++;
            d++;
        }
        while (p < s - 1) {
            i64_t pp = p;
            p++;
            buffer[result[pp] ^ result[p]] = pp;
        }
        if (f & i) {
            s += n;
            p = s;
        }
        i *= 2;
        if (i == 256) {
            i = 0;
        }
    }

    *decompressed = result;
    *decompressed_size = len;
    return option_none();
}

static option_t raykx_read_msg(poll_p poll, selector_p selector) {
    UNUSED(poll);
    obj_p res;
    i64_t len;
    raykx_ctx_p ctx;

    LOG_DEBUG("Reading KDB+ message from connection %lld", selector->id);
    len = selector->rx.buf->size;

    ctx = (raykx_ctx_p)selector->data;
    if (ctx->compressed) {
        u8_t* decompressed;
        i64_t decompressed_size;
        option_t decomp_result = raykx_decompress(selector->rx.buf->data, len, &decompressed, &decompressed_size);
        if (option_is_error(&decomp_result)) {
            LOG_ERROR("Failed to decompress message");
            poll_rx_buf_request(poll, selector, ISIZEOF(struct raykx_header_t));
            selector->rx.read_fn = raykx_read_header;
            return decomp_result;
        }
        res = raykx_des_obj(decompressed, &decompressed_size);
        heap_free(decompressed);
    } else {
        res = raykx_des_obj(selector->rx.buf->data, &len);
    }

    // LOG_TRACE_OBJ("Deserialized message: ", res);

    // Prepare for the next message
    poll_rx_buf_request(poll, selector, ISIZEOF(struct raykx_header_t));
    selector->rx.read_fn = raykx_read_header;

    return option_some(res);
}

// ============================================================================
// Event Handlers
// ============================================================================

static nil_t raykx_on_open(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    LOG_DEBUG("Connection opened, requesting handshake buffer");
    poll_rx_buf_request(poll, selector, 2);
}

static nil_t raykx_on_error(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    LOG_ERROR("Error occurred on KDB+ connection %lld", selector->id);
}

static nil_t raykx_on_close(poll_p poll, selector_p selector) {
    UNUSED(poll);
    raykx_ctx_p ctx;

    LOG_INFO("KDB+ connection %lld closed", selector->id);

    // Free context
    ctx = (raykx_ctx_p)selector->data;
    drop_obj(ctx->name);
    heap_free(ctx);
}

// ============================================================================
// Message Sending
// ============================================================================

obj_p raykx_process_msg(poll_p poll, selector_p selector, obj_p msg) {
    UNUSED(poll);

    obj_p res;
    raykx_ctx_p ctx;

    ctx = (raykx_ctx_p)selector->data;

    LOG_TRACE_OBJ("Processing message: ", msg);

    if (IS_ERR(msg) || is_null(msg))
        res = msg;
    else if (msg->type == TYPE_C8) {
        LOG_TRACE("Evaluating string message: %.*s", (i32_t)msg->len, AS_C8(msg));
        res = ray_eval_str(msg, ctx->name);
        drop_obj(msg);
    } else if (msg->type == TYPE_LIST && msg->len > 0) {
        // KDB+ apply semantics: resolve car, apply to rest as values
        obj_p *elems = AS_LIST(msg);
        i64_t n = msg->len;
        obj_p args[n];
        args[0] = eval(elems[0]);
        if (IS_ERR(args[0])) {
            drop_obj(msg);
            return args[0];
        }
        for (i64_t i = 1; i < n; i++)
            args[i] = clone_obj(elems[i]);
        res = ray_apply(args, n);
        drop_obj(args[0]);
        for (i64_t i = 1; i < n; i++)
            drop_obj(args[i]);
        drop_obj(msg);
    } else {
        LOG_TRACE("Evaluating object message");
        res = eval_obj(msg);
        drop_obj(msg);
    }

    LOG_TRACE_OBJ("Resulting object: ", res);

    return res;
}

option_t raykx_on_data(poll_p poll, selector_p selector, raw_p data) {
    UNUSED(poll);

    LOG_TRACE("Received data from connection %lld", selector->id);

    raykx_ctx_p ctx;
    obj_p v, res;

    ctx = (raykx_ctx_p)selector->data;
    res = (obj_p)data;

    poll_set_usr_fd(selector->id);
    v = raykx_process_msg(poll, selector, res);
    poll_set_usr_fd(0);

    // Send a response if the message is a synchronous request
    if (ctx->msgtype == KDB_MSG_SYNC)
        raykx_send_msg(poll, selector, v, KDB_MSG_RESP);

    drop_obj(v);

    return option_some(NULL);
}

nil_t raykx_send_msg(poll_p poll, selector_p selector, obj_p msg, u8_t msgtype) {
    i64_t size;
    poll_buffer_p buf;
    raykx_header_p header;

    LOG_TRACE("Serializing message");
    size = raykx_size_obj(msg);
    LOG_TRACE("Serialized message size: %lld", size);

    // Create buffer with exact size needed
    buf = poll_buf_create(ISIZEOF(struct raykx_header_t) + size);
    if (buf == NULL) {
        LOG_ERROR("Failed to create buffer");
        return;
    }

    LOG_TRACE("poll buf size: %lld", buf->size);

    // Serialize the message
    size = raykx_ser_obj(buf->data + ISIZEOF(struct raykx_header_t), msg);
    if (size < 0) {
        LOG_ERROR("Failed to serialize message");
        poll_buf_destroy(buf);
        return;
    }

    LOG_TRACE("Serialized message size: %lld", size);

    // Set up header msgtype
    header = (raykx_header_p)buf->data;
    header->endianness = 1;
    header->msgtype = msgtype;
    header->compressed = 0;
    header->reserved = 0;
    header->size = size + ISIZEOF(struct raykx_header_t);

    LOG_TRACE("Sending header: {.endianness: %d, .msgtype: %d, .compressed: %d, .reserved: %d, .size: %lld}",
              header->endianness, header->msgtype, header->compressed, header->reserved, header->size);

    poll_send_buf(poll, selector, buf);
    LOG_DEBUG("Message sent");
}

obj_p raykx_send(obj_p fd, obj_p msg) {
    selector_p selector;
    raykx_ctx_p ctx;
    poll_p poll = runtime_get()->poll;
    i64_t id = fd->i64;
    u8_t msgtype = KDB_MSG_SYNC;
    option_t result;
    obj_p res;

    LOG_DEBUG("Starting synchronous KDB+ send");

    selector = poll_get_selector(poll, id);
    if (selector == NULL) {
        LOG_ERROR("Invalid selector for fd %lld", id);
        return err_os();
    }

    ctx = (raykx_ctx_p)selector->data;

    // Initialize receive buffer for header
    if (selector->rx.buf == NULL) {
        if (poll_rx_buf_request(poll, selector, ISIZEOF(struct raykx_header_t)) == -1) {
            LOG_ERROR("Failed to initialize receive buffer");
            return err_os();
        }
    }

    raykx_send_msg(poll, selector, msg, msgtype);

    res = NULL_OBJ;

    // wait for the response
    if (msgtype == KDB_MSG_SYNC) {
        do {
            LOG_DEBUG("Waiting for response from connection %lld", selector->id);
            result = poll_block_on(poll, selector);
            LOG_DEBUG("Response received from connection %lld RESULT: %s", selector->id,
                      option_is_some(&result) ? "some" : "none");
            if (option_is_some(&result) && result.value != NULL) {
                res = option_take(&result);
                // If the message is a response, break the loop
                if (ctx->msgtype == KDB_MSG_RESP)
                    break;

                // Process the request otherwise
                res = raykx_process_msg(poll, selector, res);
                drop_obj(res);
            } else if (option_is_error(&result)) {
                LOG_ERROR("Error occurred on connection %lld", selector->id);
                return option_take(&result);
            }

        } while (option_is_none(&result));
    }

    return res;
}

#endif  // OS_WINDOWS

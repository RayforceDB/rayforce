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

/*
 * WASM platform-specific implementations.
 * 
 * WASM doesn't use poll/epoll/kqueue - it has its own event loop.
 * This file provides stub implementations for all poll functions.
 *
 * Note: The main entry point should be provided by the consuming project
 * (e.g., rayforce-wasm/src/wasm_main.c), not here.
 */

#include "heap.h"

// ============================================================================
// Poll stub implementations - WASM doesn't use poll
// ============================================================================

poll_p poll_create() {
    poll_p poll = (poll_p)heap_alloc(sizeof(struct poll_t));
    poll->fd = -1;
    poll->code = NULL_I64;
    poll->selectors = NULL;
    poll->timers = NULL;
    return poll;
}

nil_t poll_destroy(poll_p poll) { 
    if (poll) heap_free(poll); 
}

i64_t poll_run(poll_p poll) {
    UNUSED(poll);
    return 0;
}

i64_t poll_register(poll_p poll, poll_registry_p registry) {
    UNUSED(poll);
    UNUSED(registry);
    return 0;
}

i64_t poll_deregister(poll_p poll, i64_t id) {
    UNUSED(poll);
    UNUSED(id);
    return 0;
}

selector_p poll_get_selector(poll_p poll, i64_t id) {
    UNUSED(poll);
    UNUSED(id);
    return NULL;
}

poll_buffer_p poll_buf_create(i64_t size) {
    UNUSED(size);
    return NULL;
}

nil_t poll_buf_destroy(poll_buffer_p buf) {
    UNUSED(buf);
}

i64_t poll_rx_buf_request(poll_p poll, selector_p selector, i64_t size) {
    UNUSED(poll);
    UNUSED(selector);
    UNUSED(size);
    return 0;
}

i64_t poll_rx_buf_extend(poll_p poll, selector_p selector, i64_t size) {
    UNUSED(poll);
    UNUSED(selector);
    UNUSED(size);
    return 0;
}

i64_t poll_rx_buf_release(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return 0;
}

i64_t poll_rx_buf_reset(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return 0;
}

i64_t poll_send_buf(poll_p poll, selector_p selector, poll_buffer_p buf) {
    UNUSED(poll);
    UNUSED(selector);
    UNUSED(buf);
    return 0;
}

i64_t poll_recv(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return 0;
}

i64_t poll_send(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return 0;
}

option_t poll_block_on(poll_p poll, selector_p selector) {
    UNUSED(poll);
    UNUSED(selector);
    return option_none();
}

nil_t poll_exit(poll_p poll, i64_t code) {
    if (poll) poll->code = code;
}

nil_t poll_set_usr_fd(i64_t fd) {
    UNUSED(fd);
}

// ============================================================================
// IPC stub implementations - WASM doesn't support direct socket IPC
// ============================================================================

obj_p ipc_send_sync(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);
    return NULL_OBJ;
}

obj_p ipc_send_async(poll_p poll, i64_t id, obj_p msg) {
    UNUSED(poll);
    UNUSED(id);
    UNUSED(msg);
    return NULL_OBJ;
}

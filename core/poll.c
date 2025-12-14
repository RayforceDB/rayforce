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
#include "binary.h"
#include "log.h"

#if defined(OS_WINDOWS)
#include "iocp.c"
#elif defined(OS_MACOS)
#include "kqueue.c"
#elif defined(OS_LINUX)
#include "epoll.c"
#elif defined(OS_WASM)
// WASM provides all poll functions as stubs - no common code needed
#include "wasm.c"
#endif

// Common poll functions - not needed for WASM (wasm.c provides stubs)
#if !defined(OS_WASM)

RAYASSERT(sizeof(struct poll_buffer_t) == 16, poll_h)

selector_p poll_get_selector(poll_p poll, i64_t id) {
    i64_t idx;

    idx = freelist_get(poll->selectors, id - SELECTOR_ID_OFFSET);

    if (idx == NULL_I64)
        return NULL;

    return (selector_p)idx;
}

poll_buffer_p poll_buf_create(i64_t size) {
    poll_buffer_p buf;

    buf = (poll_buffer_p)heap_alloc(ISIZEOF(struct poll_buffer_t) + size);

    if (buf == NULL)
        return NULL;

    buf->next = NULL;
    buf->size = size;
    buf->offset = 0;

    return buf;
}

nil_t poll_buf_destroy(poll_buffer_p buf) { heap_free(buf); }

// Buffer management functions - Unix platforms use poll_buffer_p, Windows uses different buffer model
#if !defined(OS_WINDOWS)

i64_t poll_rx_buf_request(poll_p poll, selector_p selector, i64_t size) {
    UNUSED(poll);

    LOG_TRACE("Requesting buffer of %d", size);
    if (selector->rx.buf == NULL) {
        selector->rx.buf = heap_alloc(ISIZEOF(struct poll_buffer_t) + size);
    } else {
        selector->rx.buf = heap_realloc(selector->rx.buf, ISIZEOF(struct poll_buffer_t) + size);
    }
    LOG_TRACE("New buffer: %p", selector->rx.buf);
    if (selector->rx.buf == NULL)
        return -1;

    selector->rx.buf->size = size;
    selector->rx.buf->offset = 0;

    return 0;
}

i64_t poll_rx_buf_extend(poll_p poll, selector_p selector, i64_t size) {
    i64_t new_size;
    UNUSED(poll);

    new_size = selector->rx.buf->offset + size;

    LOG_TRACE("Extending buffer from %d to %d", selector->rx.buf->size, new_size);
    selector->rx.buf = heap_realloc(selector->rx.buf, ISIZEOF(struct poll_buffer_t) + new_size);
    LOG_TRACE("New buffer: %p", selector->rx.buf);
    if (selector->rx.buf == NULL)
        return -1;

    selector->rx.buf->size = new_size;

    return 0;
}

i64_t poll_rx_buf_release(poll_p poll, selector_p selector) {
    UNUSED(poll);

    heap_free(selector->rx.buf);
    selector->rx.buf = NULL;

    return 0;
}

i64_t poll_rx_buf_reset(poll_p poll, selector_p selector) {
    UNUSED(poll);

    LOG_TRACE("Resetting buffer offset to 0");

    selector->rx.buf->offset = 0;

    return 0;
}

i64_t poll_send_buf(poll_p poll, selector_p selector, poll_buffer_p buf) {
    // Attach the buffer to the end of the list
    if (selector->tx.buf != NULL)
        selector->tx.buf->next = buf;
    else
        selector->tx.buf = buf;

    return poll_send(poll, selector);
}

#endif  // !OS_WINDOWS

nil_t poll_exit(poll_p poll, i64_t code) { poll->code = code; }

// ============================================================================
// User FD setup for a duration of a callback
// ============================================================================

nil_t poll_set_usr_fd(i64_t fd) {
    obj_p s, k, v;

    s = symbol(".z.w", 4);
    k = i64(fd);
    v = binary_set(s, k);
    drop_obj(k);
    drop_obj(v);
    drop_obj(s);
}

#endif  // !defined(OS_WASM)

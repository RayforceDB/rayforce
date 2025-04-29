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

#include <sys/epoll.h>
#include "rayforce.h"
#include "parse.h"
#include "serde.h"
#include "format.h"
#include "freelist.h"
#include "chrono.h"
#include "term.h"

#define MAX_EVENTS 1024
#define BUF_SIZE 2048
#define TX_QUEUE_SIZE 16
#define SELECTOR_ID_OFFSET 3  // shifts all selector ids by 2 to avoid 0, 1 ,2 ids (stdin, stdout, stderr)

// forward declarations
struct poll_t;
struct selector_t;

// Represents the status or result of a poll-related operation.
// Convention used:
//   POLL_OK (0): Operation would block (for I/O), or needs more data (for read_fn),
//                   or is simply waiting for the next event.
//   POLL_ERROR (-1): An error occurred during the operation (check errno for I/O funcs).
//   POLL_EXIT (-2):  A request to cleanly terminate the polling loop was made.
//   Result > 0:     Indicates success/data transferred.
//                   - For I/O functions (recv_fn/send_fn): Represents the number of bytes transferred.
//                   - For read_fn callback: Indicates success *and* potentially more data can be processed
//                     from the buffer immediately (value > 0, often just 1).
typedef enum poll_result_t {
    POLL_OK = 0ll,      // Operation ok / pending / would block / need more data
    POLL_ERROR = -1ll,  // Operation failed with an error
    POLL_EXIT = -2ll,   // Request clean exit from poll loop
    POLL_READY = 1ll,   // Operation is ready to be processed
    // Positive values indicate success/bytes transferred/ready for more processing.
} poll_result_t;

#define POLL_IS_READY(x) ((x) > 0)  // Macro to check for success state (> 0)

typedef enum selector_type_t {
    SELECTOR_TYPE_STDIN = 0,
    SELECTOR_TYPE_STDOUT = 1,
    SELECTOR_TYPE_STDERR = 2,
    SELECTOR_TYPE_SOCKET = 3,
    SELECTOR_TYPE_FILE = 4,
} selector_type_t;

// Low level IO function (to read/write from/to the fd)
typedef i64_t (*poll_io_fn)(i64_t, u8_t *, i64_t);

// High level functions (to read/write from/to the selector buffer)
typedef poll_result_t (*poll_fn)(struct poll_t *, struct selector_t *);

typedef struct poll_buffer_t {
    struct poll_buffer_t *next;
    i64_t size;
    i64_t offset;
    u8_t data[];
} *poll_buffer_p;

#if defined(OS_WINDOWS)

typedef struct selector_t {
    i64_t fd;  // socket fd
    i64_t id;  // selector id
    u8_t version;

    struct {
        b8_t ignore;
        u8_t msgtype;
        b8_t header;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD size;
        i64_t size;
        u8_t *buf;
        WSABUF wsa_buf;
    } rx;

    struct {
        b8_t ignore;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD size;
        i64_t size;
        u8_t *buf;
        WSABUF wsa_buf;
        queue_p queue;  // queue for async messages waiting to be sent
    } tx;

} *selector_p;

#else

typedef struct selector_t {
    i64_t fd;  // socket fd
    i64_t id;  // selector id

    selector_type_t type;

    poll_fn open_fn;
    poll_fn close_fn;
    poll_fn error_fn;

    raw_p data;

    struct {
        poll_buffer_p buf;   // pointer to the buffer
        poll_io_fn recv_fn;  // to be called when the selector is ready to read
        poll_fn read_fn;     // to be called when the selector is ready to read
    } rx;

    struct {
        b8_t isset;
        poll_buffer_p buf;   // pointer to the buffer
        poll_io_fn send_fn;  // to be called when the selector is ready to send
        poll_fn write_fn;    // to be called when the selector is ready to send
    } tx;

} *selector_p;

typedef enum poll_events_t {
    POLL_EVENT_READ = EPOLLIN,
    POLL_EVENT_WRITE = EPOLLOUT,
    POLL_EVENT_ERROR = EPOLLERR,
    POLL_EVENT_HUP = EPOLLHUP,
} poll_events_t;

typedef struct poll_t {
    i64_t code;            // exit code
    i64_t fd;              // file descriptor of the poll
    freelist_p selectors;  // freelist of selectors
    timers_p timers;       // timers heap
} *poll_p;

// Structure used to pass information when registering a new file descriptor.
typedef struct poll_registry_t {
    i64_t fd;              // The file descriptor to register.
    selector_type_t type;  // Type of the file descriptor.
    poll_events_t events;  // Initial set of events to monitor (e.g., POLL_EVENT_READ).
    poll_fn open_fn;       // Called upon registration.
    poll_fn close_fn;      // Called upon deregistration.
    poll_fn error_fn;      // Handles errors.
    poll_io_fn recv_fn;    // to be called when the selector is ready to read
    poll_io_fn send_fn;    // to be called when the selector is ready to send
    poll_fn read_fn;       // Processes received data.
    poll_fn write_fn;      // Processes data to be sent.
    raw_p data;            // User-defined data to associate with the selector.
} *poll_registry_p;

poll_p poll_create();
nil_t poll_destroy(poll_p poll);
poll_result_t poll_run(poll_p poll);
poll_result_t poll_register(poll_p poll, poll_registry_p registry);
poll_result_t poll_deregister(poll_p poll, i64_t id);
selector_p poll_get_selector(poll_p poll, i64_t id);
poll_buffer_p poll_buf_create(i64_t size);
nil_t poll_buf_destroy(poll_buffer_p buf);
poll_result_t poll_rx_buf_request(poll_p poll, selector_p selector, i64_t size);
poll_result_t poll_rx_buf_release(poll_p poll, selector_p selector);
poll_result_t poll_rx_buf_reset(poll_p poll, selector_p selector);
poll_result_t poll_send_buf(poll_p poll, selector_p selector, poll_buffer_p buf);

// Exit the app
nil_t poll_exit(poll_p poll, i64_t code);

#endif  // OS

#endif  // POLL_H

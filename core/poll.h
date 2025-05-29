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

// Core includes
#include "rayforce.h"
#include "def.h"
#include "parse.h"
#include "serde.h"
#include "format.h"
#include "freelist.h"
#include "chrono.h"
#include "term.h"
#include "option.h"

// Constants
#define MAX_EVENTS 1024
#define BUF_SIZE 2048
#define TX_QUEUE_SIZE 16
#define SELECTOR_ID_OFFSET 3  // shifts all selector ids by 2 to avoid 0, 1, 2 ids (stdin, stdout, stderr)

// Forward declarations
struct poll_t;
struct selector_t;

// Type definitions
typedef enum selector_type_t {
    SELECTOR_TYPE_STDIN = 0,
    SELECTOR_TYPE_STDOUT = 1,
    SELECTOR_TYPE_STDERR = 2,
    SELECTOR_TYPE_SOCKET = 3,
    SELECTOR_TYPE_FILE = 4,
} selector_type_t;

// Function type definitions
typedef i64_t (*poll_io_fn)(i64_t, u8_t *, i64_t);                              // Low level IO
typedef option_t (*poll_rdwr_fn)(struct poll_t *, struct selector_t *);         // High level IO
typedef option_t (*poll_data_fn)(struct poll_t *, struct selector_t *, raw_p);  // Data callback
typedef nil_t (*poll_evts_fn)(struct poll_t *, struct selector_t *);            // Event callbacks

// Buffer structure
typedef struct poll_buffer_t {
    struct poll_buffer_t *next;
    u32_t size;
    u32_t offset;
    u8_t data[];
} *poll_buffer_p;

// Platform-specific event definitions and structures
#if defined(OS_WINDOWS)
#include <windows.h>
typedef enum poll_events_t {
    POLL_EVENT_READ = POLLIN,
    POLL_EVENT_WRITE = POLLOUT,
    POLL_EVENT_ERROR = POLLERR,
} poll_events_t;

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
#if defined(OS_LINUX)
#include <sys/epoll.h>
typedef enum poll_events_t {
    POLL_EVENT_READ = EPOLLIN,
    POLL_EVENT_WRITE = EPOLLOUT,
    POLL_EVENT_ERROR = EPOLLERR,
    POLL_EVENT_HUP = EPOLLHUP,
    POLL_EVENT_RDHUP = EPOLLRDHUP,
    POLL_EVENT_EDGE = EPOLLET,
} poll_events_t;
#elif defined(OS_MACOS)
#include <sys/event.h>
typedef enum poll_events_t {
    POLL_EVENT_READ = EVFILT_READ,
    POLL_EVENT_WRITE = EVFILT_WRITE,
    POLL_EVENT_ERROR = EV_ERROR,
    POLL_EVENT_HUP = EV_EOF,
    POLL_EVENT_RDHUP = EV_EOF,
    POLL_EVENT_EDGE = 0,
} poll_events_t;
#endif

typedef struct selector_t {
    i64_t fd;  // socket fd
    i64_t id;  // selector id
    selector_type_t type;
    poll_events_t interest;

    poll_evts_fn open_fn;
    poll_evts_fn close_fn;
    poll_evts_fn error_fn;
    poll_data_fn data_fn;
    raw_p data;

    struct {
        poll_buffer_p buf;     // pointer to the buffer
        poll_io_fn recv_fn;    // to be called when the selector is ready to read
        poll_rdwr_fn read_fn;  // to be called when the selector is ready to read
    } rx;

    struct {
        poll_buffer_p buf;      // pointer to the buffer
        poll_io_fn send_fn;     // to be called when the selector is ready to send
        poll_rdwr_fn write_fn;  // to be called when the selector is ready to send
    } tx;
} *selector_p;

typedef struct poll_t {
    i64_t fd;              // file descriptor of the poll
    i64_t code;            // exit code
    freelist_p selectors;  // freelist of selectors
    timers_p timers;       // timers heap
} *poll_p;

// Registry structure for new file descriptor registration
typedef struct poll_registry_t {
    i64_t fd;               // The file descriptor to register
    selector_type_t type;   // Type of the file descriptor
    poll_events_t events;   // Initial set of events to monitor
    poll_evts_fn open_fn;   // Called upon registration
    poll_evts_fn close_fn;  // Called upon deregistration
    poll_evts_fn error_fn;  // Handles errors
    poll_io_fn recv_fn;     // Called when ready to read
    poll_io_fn send_fn;     // Called when ready to send
    poll_rdwr_fn read_fn;   // Processes received data
    poll_rdwr_fn write_fn;  // Processes data to be sent
    poll_data_fn data_fn;   // Processes retrieved data
    raw_p data;             // User-defined data
} *poll_registry_p;
#endif

// Function declarations
poll_p poll_create();
nil_t poll_destroy(poll_p poll);
i64_t poll_run(poll_p poll);
i64_t poll_register(poll_p poll, poll_registry_p registry);
i64_t poll_deregister(poll_p poll, i64_t id);
selector_p poll_get_selector(poll_p poll, i64_t id);
poll_buffer_p poll_buf_create(i64_t size);
nil_t poll_buf_destroy(poll_buffer_p buf);
i64_t poll_rx_buf_request(poll_p poll, selector_p selector, i64_t size);
i64_t poll_rx_buf_extend(poll_p poll, selector_p selector, i64_t size);
i64_t poll_rx_buf_release(poll_p poll, selector_p selector);
i64_t poll_rx_buf_reset(poll_p poll, selector_p selector);
i64_t poll_send_buf(poll_p poll, selector_p selector, poll_buffer_p buf);
option_t poll_block_on(poll_p poll, selector_p selector);
nil_t poll_exit(poll_p poll, i64_t code);
nil_t poll_set_usr_fd(i64_t fd);

#endif  // POLL_H

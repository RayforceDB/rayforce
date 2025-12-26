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
#include "option.h"

// Forward declarations (defined in app/)
struct term_t;
struct repl_t;
typedef struct term_t *term_p;
typedef struct repl_t *repl_p;

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

// Buffer structure (32-byte aligned for cache efficiency)
typedef struct poll_buffer_t {
    struct poll_buffer_t *next;
    i64_t size;
    i64_t offset;
    i64_t _padding;  // align to 32 bytes
    u8_t data[];
} *poll_buffer_p;

// Platform-specific event definitions and structures
#if defined(OS_WINDOWS)

// Windows poll events (not used directly, IOCP handles differently)
typedef enum poll_events_t {
    POLL_EVENT_READ = 0x01,
    POLL_EVENT_WRITE = 0x02,
    POLL_EVENT_ERROR = 0x04,
} poll_events_t;

// Poll result type for async I/O operations
typedef enum poll_result_t {
    POLL_OK = 0,     // Operation pending, waiting for completion
    POLL_DONE = 1,   // Operation completed successfully
    POLL_ERROR = -1  // Error occurred
} poll_result_t;

// Simple queue for Windows async message handling
typedef struct queue_t {
    i64_t capacity;
    i64_t head;
    i64_t tail;
    i64_t count;
    raw_p *data;
} *queue_p;

queue_p queue_create(i64_t capacity);
nil_t queue_free(queue_p queue);
nil_t queue_push(queue_p queue, raw_p item);
raw_p queue_pop(queue_p queue);

typedef struct selector_t {
    i64_t fd;  // socket fd
    i64_t id;  // selector id
    u8_t version;
    raw_p data;  // user-defined data

    struct {
        b8_t ignore;
        u8_t msgtype;
        b8_t header;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD size;
        u8_t *buf;
        WSABUF wsa_buf;
    } rx;

    struct {
        b8_t ignore;
        OVERLAPPED overlapped;
        DWORD flags;
        DWORD size;
        u8_t *buf;
        WSABUF wsa_buf;
        queue_p queue;  // queue for async messages waiting to be sent
    } tx;
} *selector_p;

typedef struct poll_t {
    i64_t poll_fd;         // IOCP handle
    i64_t ipc_fd;          // IPC socket fd
    i64_t code;            // exit code
    obj_p replfile;        // REPL file name
    obj_p ipcfile;         // IPC file name
    struct term_t *term;   // terminal
    struct repl_t *repl;   // REPL (for cleanup)
    freelist_p selectors;  // freelist of selectors
    timers_p timers;       // timers heap
} *poll_p;

// Registry not used on Windows, but define for API compatibility
typedef struct poll_registry_t {
    i64_t fd;
    selector_type_t type;
    poll_events_t events;
    raw_p data;
} *poll_registry_p;

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
#elif defined(OS_WASM)
// WASM stub - poll is not supported in browser environment
typedef enum poll_events_t {
    POLL_EVENT_READ = 1,
    POLL_EVENT_WRITE = 2,
    POLL_EVENT_ERROR = 4,
    POLL_EVENT_HUP = 8,
    POLL_EVENT_RDHUP = 16,
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
#if defined(OS_WINDOWS)
// Windows uses IOCP with different initialization and registration
poll_p poll_init(i64_t port);
i64_t poll_listen(poll_p poll, i64_t port);
i64_t poll_register(poll_p poll, i64_t fd, u8_t version);
nil_t poll_deregister(poll_p poll, i64_t id);
#define poll_create() poll_init(0)
#else
poll_p poll_create();
i64_t poll_register(poll_p poll, poll_registry_p registry);
i64_t poll_deregister(poll_p poll, i64_t id);
#endif

nil_t poll_destroy(poll_p poll);
i64_t poll_run(poll_p poll);
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

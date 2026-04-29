/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_POLL_H
#define RAY_POLL_H

#include <rayforce.h>

/* Forward declarations */
typedef struct ray_poll     ray_poll_t;
typedef struct ray_selector ray_selector_t;

/* ===== Selector types ===== */

#define RAY_SEL_STDIN   0
#define RAY_SEL_SOCKET  3

/* ===== Callbacks ===== */

typedef int64_t (*ray_io_fn)(int64_t fd, uint8_t* buf, int64_t len);
typedef ray_t*  (*ray_read_fn)(ray_poll_t* poll, ray_selector_t* sel);
typedef void    (*ray_event_fn)(ray_poll_t* poll, ray_selector_t* sel);
typedef ray_t*  (*ray_poll_data_fn)(ray_poll_t* poll, ray_selector_t* sel, void* data);

/* ===== Buffer ===== */

typedef struct ray_poll_buf {
    struct ray_poll_buf* next;
    int64_t              size;
    int64_t              offset;
    uint8_t              data[];
} ray_poll_buf_t;

/* ===== Selector — one per registered fd ===== */

struct ray_selector {
    int64_t          fd;
    int64_t          id;
    uint8_t          type;
    void*            data;
    ray_event_fn     open_fn;
    ray_event_fn     close_fn;
    ray_event_fn     error_fn;
    ray_poll_data_fn      data_fn;
    struct { ray_poll_buf_t* buf; ray_io_fn recv_fn; ray_read_fn read_fn; } rx;
    struct { ray_poll_buf_t* buf; ray_io_fn send_fn; }                      tx;
};

/* ===== Registration ===== */

typedef struct ray_poll_reg {
    int64_t          fd;
    uint8_t          type;
    ray_event_fn     open_fn;
    ray_event_fn     close_fn;
    ray_event_fn     error_fn;
    ray_poll_data_fn      data_fn;
    ray_io_fn        recv_fn;
    ray_io_fn        send_fn;
    ray_read_fn      read_fn;
    void*            data;
} ray_poll_reg_t;

/* ===== Poll ===== */

struct ray_poll {
    int64_t          fd;       /* epoll/kqueue/iocp handle */
    int64_t          code;     /* exit code (-1 = running) */
    ray_selector_t** sels;     /* selector array */
    uint32_t         n_sels;
    uint32_t         sel_cap;
    char             auth_secret[256]; /* password from -u/-U, empty = no auth */
    bool             restricted;       /* true if -U (read-only IPC mode) */
};

/* ===== API ===== */

ray_poll_t*     ray_poll_create(void);
void            ray_poll_destroy(ray_poll_t* poll);
int64_t         ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg);
void            ray_poll_deregister(ray_poll_t* poll, int64_t id);
int64_t         ray_poll_run(ray_poll_t* poll);
void            ray_poll_exit(ray_poll_t* poll, int64_t code);
ray_selector_t* ray_poll_get(ray_poll_t* poll, int64_t id);

ray_poll_buf_t* ray_poll_buf_new(int64_t size);
void            ray_poll_buf_free(ray_poll_buf_t* buf);
void            ray_poll_rx_request(ray_poll_t* poll, ray_selector_t* sel,
                                    int64_t size);
void            ray_poll_rx_extend(ray_poll_t* poll, ray_selector_t* sel,
                                   int64_t extra);
void            ray_poll_send(ray_poll_t* poll, ray_selector_t* sel,
                              ray_poll_buf_t* buf);

#endif /* RAY_POLL_H */

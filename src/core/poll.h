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

/* Immutable, reference-counted bytes.  A frame built once for a multicast
 * publication is shared by every subscriber's transmit queue; each queue
 * holds its own ray_poll_buf_t node (offset, link) pointing at the same
 * frame, and the frame is freed when the last node is done with it. */
typedef struct ray_poll_frame {
    int32_t rc;
    int64_t size;
    uint8_t data[];
} ray_poll_frame_t;

/* A queue node.  `data` points either at the node's own trailing storage
 * (ray_poll_buf_new: rx buffers, one-off tx frames) or into a shared
 * ray_poll_frame_t (ray_poll_buf_from_frame), which `frame` then owns a
 * reference to.  Readers use data/size/offset the same way either way. */
typedef struct ray_poll_buf {
    struct ray_poll_buf* next;
    int64_t              size;
    int64_t              offset;
    uint8_t*             data;
    ray_poll_frame_t*    frame;
    uint8_t              storage[];
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
    struct {
        ray_poll_buf_t* buf;
        ray_io_fn       send_fn;
        int64_t         limit_bytes;   /* per-connection backlog override; 0 = process default */
        int64_t         limit_frames;  /* idem; 0 = process default (which may be unlimited) */
        int64_t         hwm_bytes;     /* largest backlog ever queued on this connection */
    } tx;
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
    /* Exit code / stop flag (-1 = running).  Atomic because ray_poll_exit
     * may set it from a different thread than the one spinning in
     * ray_poll_run's `while (code < 0)` loop (the production poll loop is
     * single-threaded, but the API permits — and tests exercise — a
     * cross-thread stop); a plain field there is a data race and lets the
     * compiler hoist the load out of the loop. */
    _Atomic int64_t  code;     /* exit code (-1 = running) */
    ray_selector_t** sels;     /* selector array */
    uint32_t         n_sels;
    uint32_t         sel_cap;
    char             auth_secret[256]; /* password from -u/-U, empty = no auth */
    bool             restricted;       /* true if -U (read-only IPC mode) */
    void*            timers;           /* opaque ray_timers_t*; lazily allocated */
    void*            mcast;            /* opaque ray_mcast_t*; lazily allocated */
    int64_t          tx_hwm_bytes;     /* largest backlog ever queued on any connection */
};

/* ===== API ===== */

ray_poll_t*     ray_poll_create(void);
void            ray_poll_destroy(ray_poll_t* poll);
void            ray_poll_set_restricted(ray_poll_t* poll, bool restricted);
int64_t         ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg);
void            ray_poll_deregister(ray_poll_t* poll, int64_t id);
int64_t         ray_poll_run(ray_poll_t* poll);
int64_t         ray_poll_run_for(ray_poll_t* poll, int timeout_ms);
void            ray_poll_exit(ray_poll_t* poll, int64_t code);
ray_selector_t* ray_poll_get(ray_poll_t* poll, int64_t id);

ray_poll_buf_t* ray_poll_buf_new(int64_t size);
void            ray_poll_buf_free(ray_poll_buf_t* buf);
void            ray_poll_rx_request(ray_poll_t* poll, ray_selector_t* sel,
                                    int64_t size);
ray_poll_frame_t* ray_poll_frame_new(int64_t size);          /* rc = 1 */
void              ray_poll_frame_retain(ray_poll_frame_t* f);
void              ray_poll_frame_release(ray_poll_frame_t* f);
ray_poll_buf_t*   ray_poll_buf_from_frame(ray_poll_frame_t* f); /* node holding a new ref */
void            ray_poll_tx_request(ray_poll_t* poll, ray_selector_t* sel);
void            ray_poll_tx_cancel(ray_poll_t* poll, ray_selector_t* sel);
int             ray_poll_tx_flush(ray_poll_t* poll, ray_selector_t* sel);

#endif /* RAY_POLL_H */

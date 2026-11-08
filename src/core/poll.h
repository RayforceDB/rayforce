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
/* The wake-up descriptor of an upgraded connection's transport (below):
 * readable when bytes landed in its ring or ring space came back.  Its
 * selector carries no rx/tx state of its own; `data` names the
 * connection selector it serves (as an id cast to a pointer). */
#define RAY_SEL_XPORT   4

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

/* ===== Transport — where an upgraded connection's bytes go =====
 *
 * A connection starts on its socket.  Once both peers agree (core/ipc.c),
 * its frames move to a faster transport: shared-memory rings on one host
 * (core/shm.c), remote memory across hosts with RDMA (core/rdma.c).  The
 * socket stays open, carrying nothing but the peer's going.  A transport
 * is a reliable byte stream the poll loop drives like a socket, with one
 * difference it has to know about: bytes can land without any descriptor
 * becoming readable, so the loop asks `pending` before it sleeps and tells
 * the transport that it sleeps (`prepare_sleep`), at which point the peer
 * starts writing to `event_fd`. */
typedef struct ray_xport ray_xport_t;

typedef struct ray_xport_ops {
    /* Bytes copied out: > 0, or 0 when none has arrived, -1 when the link
     * failed. */
    int64_t (*read)(ray_xport_t* x, uint8_t* buf, int64_t len);
    /* Bytes taken now: > 0, or 0 when nothing fits (never with `block`,
     * which waits for room until all are taken), -1 when the link failed. */
    int64_t (*write)(ray_xport_t* x, const uint8_t* buf, int64_t len, bool block);
    /* Bytes wait to be read (or the link failed).  Takes what the peer
     * announced first, so it is also the loop's "progress" step. */
    bool    (*pending)(ray_xport_t* x);
    /* At least one byte can be written now. */
    bool    (*room)(ray_xport_t* x);
    /* About to sleep on event_fd: the peer will ring it from now on when
     * bytes arrive if `want_data`, when ring space returns if `want_room`.
     * False when what is waited for arrived between the last look and the
     * flag: do not sleep. */
    bool    (*prepare_sleep)(ray_xport_t* x, bool want_data, bool want_room);
    /* Awake again: the peer stops ringing, the bell is quietened. */
    void    (*woke)(ray_xport_t* x);
    void    (*destroy)(ray_xport_t* x);
} ray_xport_ops_t;

#define RAY_XPORT_TCP   0
#define RAY_XPORT_SHM   1
#define RAY_XPORT_RDMA  2

struct ray_xport {
    const ray_xport_ops_t* ops;
    int     event_fd;
    uint8_t kind;           /* RAY_XPORT_SHM / RAY_XPORT_RDMA */
    /* Whether event_fd also announces returned ring space; a sleeper with
     * queued output on a transport without it looks again soon. */
    bool    fd_tells_room;
};

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
    /* The transport this connection moved to, or NULL while on its socket.
     * Owned by the selector: deregistering it destroys the transport and
     * the RAY_SEL_XPORT selector (`xport_id`) that waits on its event_fd. */
    ray_xport_t*     xport;
    int64_t          xport_id;
    /* Moved, but still reading the socket: each direction switches on its
     * own, and a connection that already writes to its transport reads TCP
     * until the peer's last TCP frame (core/ipc.c, XP_SWITCHED) has come.
     * Bytes the peer puts on the transport meanwhile wait in it. */
    bool             rx_on_sock;
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
    uint32_t         n_sels;   /* slots (deregistered slots stay NULL) */
    uint32_t         n_live;   /* registered selectors right now */
    uint32_t         sel_cap;
    char             auth_secret[256]; /* password from -u/-U, empty = no auth */
    bool             restricted;       /* true if -U (read-only IPC mode) */
    void*            timers;           /* opaque ray_timers_t*; lazily allocated */
    void*            mcast;            /* opaque ray_mcast_t*; lazily allocated */
    int64_t          tx_hwm_bytes;     /* largest backlog ever queued on any connection */
    uint32_t         n_xport;          /* connections on a transport right now */
};

/* ===== API ===== */

ray_poll_t*     ray_poll_create(void);
/* True when the loop has nothing to wait for: no registered selector and
 * no pending timer.  ray_poll_run returns instead of blocking forever. */
bool            ray_poll_idle(ray_poll_t* poll);
/* Run bounded passes until no timer is pending (or the loop was told to
 * exit).  Serves whatever selectors exist meanwhile, but does not stay
 * for them: a script's lingering client handle must not keep the
 * process alive once its timers are spent. */
void            ray_poll_drain_timers(ray_poll_t* poll);
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

/* ===== Transports ===== */

/* Move `sel` to `xport`: registers a RAY_SEL_XPORT selector on its
 * event_fd and routes the connection's bytes through it from now on.  The
 * transport is owned by the selector on success; on failure (-1) the
 * caller still owns it. */
int             ray_poll_xport_attach(ray_poll_t* poll, ray_selector_t* sel,
                                      ray_xport_t* xport);
/* The connection's byte stream, wherever it is: its transport once it
 * has one (for reading, once `rx_on_sock` is clear), else its socket
 * through recv_fn/send_fn.  Same contract as the
 * io callbacks: bytes moved, 0 for EOF (recv) or nothing taken (send with
 * errno EAGAIN), -1 with errno (EAGAIN when nothing is there yet). */
int64_t         ray_poll_sel_recv(ray_selector_t* sel, uint8_t* buf, int64_t len);
int64_t         ray_poll_sel_send(ray_selector_t* sel, const uint8_t* buf, int64_t len);
/* Fill the selector's rx buffer and run its read_fn for every phase it
 * completes, until the stream has nothing more right now.  Returns 0 then,
 * or -1 when the selector was deregistered (peer gone, protocol error).
 * The one rx pump: the poll backends and the sync-send wait share it. */
int             ray_poll_serve_rx(ray_poll_t* poll, int64_t id);
/* Write what the queue holds and the stream takes; -1 when the selector
 * was deregistered on a write error. */
int             ray_poll_serve_tx(ray_poll_t* poll, int64_t id);
/* For the backends' deregister: tears the transport down with its
 * connection (no-op without one). */
void            ray_poll_xport_detach(ray_poll_t* poll, ray_selector_t* sel);

#endif /* RAY_POLL_H */

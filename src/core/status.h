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

#ifndef RAY_STATUS_H
#define RAY_STATUS_H

/* The status block: what this process is doing right now.
 *
 * A server evaluates one request at a time on its main thread and reads
 * no socket meanwhile, so nothing can ask it over IPC what it is busy
 * with.  The main thread instead writes this block at the boundaries of
 * its work: when a request, a REPL line, a timer callback or a
 * connection hook starts and ends, and around the poll loop's blocking
 * wait.  Any thread reads it without a lock (ray_status_read): every
 * field is an atomic, and the ones that must agree with each other are
 * written under a sequence count that a reader checks before and after.
 *
 * Cost when nobody reads: no allocation and no system call but clock
 * reads.  The poll loop reads the monotonic clock before and after its
 * wait (it could not otherwise tell working from waiting).  A unit of
 * work reads it when it starts and when it ends (how long it ran), and
 * the wall clock when it starts (since when, for readers in other
 * processes). */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rayforce.h>    /* ray_progress_t */

/* Source text kept for the running work; longer is truncated.  The same
 * bound as the query log's (RAY_QLOG_QUERY_MAX). */
#define RAY_STATUS_QUERY_MAX 256

/* What the main thread is running. */
#define RAY_WORK_IDLE  0
#define RAY_WORK_IPC   1   /* a request from a connection; handle: the connection */
#define RAY_WORK_REPL  2   /* a line typed at, or piped to, the REPL */
#define RAY_WORK_TIMER 3   /* a timer callback; handle: the timer's id */
#define RAY_WORK_HOOK  4   /* a connection hook outside a request; handle: the connection */

/* A RAY_TIMESTAMP counts from 2000-01-01; this many nanoseconds after the
 * Unix epoch (ray_epoch_offset in ops/temporal.c). */
#define RAY_STATUS_EPOCH_UNIX_NS (946684800LL * 1000000000LL)

/* A consistent copy of the block, for one reader. */
typedef struct {
    uint32_t kind;          /* RAY_WORK_* */
    int64_t  handle;        /* see RAY_WORK_*; -1 when none */
    int64_t  since_ts;      /* when the work started, a RAY_TIMESTAMP; 0 when idle */
    int64_t  since_mono;    /* the same instant on ray_time_now_ns's clock */
    uint32_t query_len;
    char     query[RAY_STATUS_QUERY_MAX];
    bool     waiting;       /* the loop is inside its blocking wait */
    uint64_t busy_ns;       /* time the loop spent working, up to now */
    uint64_t total_ns;      /* working and waiting together, up to now */
    uint64_t iterations;    /* waits the loop has made */
    uint64_t work_count;    /* units of work finished */
    uint64_t work_max_ns;   /* the longest of them */
    uint32_t conns_in;      /* live IPC connections accepted here */
    uint32_t conns_out;     /* live IPC connections opened from here */
    uint32_t streams;       /* status streams being served */
} ray_status_snap_t;

/* Any thread; never blocks, never allocates. */
void ray_status_read(ray_status_snap_t* out);

/* ---- the main thread's side ---- */

/* A unit of work starts.  Only the outermost one is published: work that
 * runs inside other work (a request served while another waits for a
 * reply, a hook inside a request) leaves the block naming what the loop
 * was given first.  `since_ts` is the RAY_TIMESTAMP of now when the
 * caller has just read it, else 0.  Pair with ray_status_end. */
void ray_status_begin(uint32_t kind, int64_t handle, int64_t since_ts,
                      const char* text, size_t len);
void ray_status_end(void);
/* True inside the outermost request from a connection: its text is not
 * known when it starts (the payload is decoded later) and is set here. */
bool ray_status_wants_text(void);
void ray_status_set_text(const char* text, size_t len);

/* Around the poll loop's blocking wait, in every backend. */
void ray_status_wait_begin(void);
void ray_status_wait_end(void);

/* Someone outside is reading: a status stream is open (core/status_stream.c).
 * Text that costs something to make is made only then. */
extern _Atomic(uint32_t) ray_status_streams;
static inline bool ray_status_watched(void) {
    return atomic_load_explicit(&ray_status_streams, memory_order_relaxed) != 0;
}

void ray_status_conn_opened(bool inbound);
void ray_status_conn_closed(bool inbound);

/* Share of the last `window_ns` the loop spent working, 0..1.  A process
 * whose loop is younger than the window answers for the time it has. */
double ray_status_utilisation(int64_t window_ns);

/* ---- the status stream (core/status_stream.c) ---- */

/* One frame of the stream: the payload of a RAY_IPC_MSG_STATUS frame the
 * server sends.  On the wire it is this, little-endian, then the text:
 *
 *   offset 0  u8   layout version, RAY_STATUS_FRAME_VERSION
 *   offset 1  u8   state, RAY_STATUS_*
 *   offset 2  u16  query_len: bytes of text after the fixed part, 0..=256
 *   offset 4  u32  pid
 *   offset 8  i64  now: nanoseconds since the Unix epoch
 *   offset 16 i64  busy_since: when the running work started, same clock; 0 when idle
 *   offset 24 u64  loop_busy_ns: time the main loop has spent working
 *   offset 32 u64  loop_total_ns: working and waiting together
 *   offset 40 u64  cpu_user_ns
 *   offset 48 u64  cpu_sys_ns
 *   offset 56 u64  rss_bytes
 *   offset 64 u64  heap_bytes: anonymous memory the allocator has committed
 *                  (`anon-committed` of .sys.mem)
 *   offset 72 u64  mem_limit_bytes: the -m budget; 0 when none was given
 *   offset 80 u32  conns: live IPC connections, inbound and outbound
 *   offset 84 u32  workers: threads of the execution pool, main included
 *   offset 88 u32  cores: CPUs this process may run on
 *   offset 92 u32  reserved, 0
 *   offset 96      the text, query_len bytes, not NUL-terminated */
#define RAY_STATUS_FRAME_VERSION 1
#define RAY_STATUS_FRAME_FIXED   96
#define RAY_STATUS_FRAME_MAX     (RAY_STATUS_FRAME_FIXED + RAY_STATUS_QUERY_MAX)

#define RAY_STATUS_IDLE  0   /* the loop waits, or is between two pieces of work */
#define RAY_STATUS_IPC   1   /* a request from a connection */
#define RAY_STATUS_OTHER 2   /* a REPL line, a timer callback, a connection hook */

/* The interval a client asks for is brought into this range. */
#define RAY_STATUS_INTERVAL_MIN_MS 100u
#define RAY_STATUS_INTERVAL_MAX_MS 10000u
/* Streams served at once; one asked for past this is refused. */
#define RAY_STATUS_STREAMS_MAX 64

typedef struct {
    uint8_t  state;
    uint16_t query_len;
    uint32_t pid;
    int64_t  now_ns;
    int64_t  busy_since_ns;
    uint64_t loop_busy_ns;
    uint64_t loop_total_ns;
    uint64_t cpu_user_ns;
    uint64_t cpu_sys_ns;
    uint64_t rss_bytes;
    uint64_t heap_bytes;
    uint64_t mem_limit_bytes;
    uint32_t conns;
    uint32_t workers;
    uint32_t cores;
    char     query[RAY_STATUS_QUERY_MAX];
} ray_status_frame_t;

/* Writes the payload to `out` (RAY_STATUS_FRAME_MAX bytes of room) and
 * returns its length. */
size_t ray_status_frame_encode(const ray_status_frame_t* f, uint8_t* out);
/* False for a payload that is not one whole frame of this version. */
bool   ray_status_frame_decode(const uint8_t* p, size_t len, ray_status_frame_t* f);

/* Hand a connected socket, already out of the poll loop, to the stream's
 * thread (started on first use), which owns it from here: a frame every
 * `interval_ms` (brought into range) until the peer closes or stops
 * reading.  Main thread only.  False when it could not be taken, on a
 * platform without the stream included; the socket is closed then. */
bool ray_status_stream_add(int64_t fd, uint32_t interval_ms);
void ray_status_stream_for_test(int sndbuf, uint32_t min_interval_ms);

/* core/progress.c: the progress of the running query, when it is tracked. */
bool ray_progress_current(ray_progress_t* out);

/* ---- the process, as the operating system counts it ---- */

typedef struct {
    uint64_t cpu_user_ns;
    uint64_t cpu_sys_ns;
    uint64_t rss_bytes;       /* resident now */
    uint64_t rss_peak_bytes;  /* the most it has been */
    uint32_t threads;
} ray_proc_usage_t;

/* Any thread; no allocation.  A figure the platform cannot give reads 0. */
void ray_os_proc_usage(ray_proc_usage_t* out);

#endif /* RAY_STATUS_H */

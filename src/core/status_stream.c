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

#if defined(__linux__)
#define _GNU_SOURCE
#endif

/* The status stream: the status block, sent to readers outside.
 *
 * A client asks for it with one RAY_IPC_MSG_STATUS frame (core/ipc.h);
 * the main thread then takes that connection out of its poll loop and
 * hands the socket here.  One thread, started on first use, serves every
 * such socket: each interval it writes one frame holding what the main
 * thread is running, whatever the main thread is doing.
 *
 * The thread shares nothing with the engine but the status block's
 * atomics.  Sockets reach it through a pipe, so there is no lock; it
 * keeps them in its own array and builds frames on its stack, so it never
 * touches the heap allocator; and it never waits on a socket: one that
 * cannot take a whole frame at once is closed. */

#include "core/status.h"
#include "core/ipc.h"
#include "core/platform.h"
#include "core/pool.h"      /* ray_pool_size */
#include "core/sock.h"
#include "core/timer.h"     /* ray_time_now_ns */
#include "mem/heap.h"
#include <string.h>

/* ---- the frame ---- */

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

size_t ray_status_frame_encode(const ray_status_frame_t* f, uint8_t* out)
{
    uint16_t qn = f->query_len > RAY_STATUS_QUERY_MAX ? RAY_STATUS_QUERY_MAX : f->query_len;
    out[0] = RAY_STATUS_FRAME_VERSION;
    out[1] = f->state;
    put16(out + 2, qn);
    put32(out + 4, f->pid);
    put64(out + 8, (uint64_t)f->now_ns);
    put64(out + 16, (uint64_t)f->busy_since_ns);
    put64(out + 24, f->loop_busy_ns);
    put64(out + 32, f->loop_total_ns);
    put64(out + 40, f->cpu_user_ns);
    put64(out + 48, f->cpu_sys_ns);
    put64(out + 56, f->rss_bytes);
    put64(out + 64, f->heap_bytes);
    put64(out + 72, f->mem_limit_bytes);
    put32(out + 80, f->conns);
    put32(out + 84, f->workers);
    put32(out + 88, f->cores);
    put32(out + 92, 0);
    memcpy(out + RAY_STATUS_FRAME_FIXED, f->query, qn);
    return RAY_STATUS_FRAME_FIXED + qn;
}

bool ray_status_frame_decode(const uint8_t* p, size_t len, ray_status_frame_t* f)
{
    if (len < RAY_STATUS_FRAME_FIXED || p[0] != RAY_STATUS_FRAME_VERSION) return false;
    uint16_t qn = get16(p + 2);
    if (p[1] > RAY_STATUS_OTHER || qn > RAY_STATUS_QUERY_MAX ||
        len != (size_t)RAY_STATUS_FRAME_FIXED + qn) return false;
    f->state           = p[1];
    f->query_len       = qn;
    f->pid             = get32(p + 4);
    f->now_ns          = (int64_t)get64(p + 8);
    f->busy_since_ns   = (int64_t)get64(p + 16);
    f->loop_busy_ns    = get64(p + 24);
    f->loop_total_ns   = get64(p + 32);
    f->cpu_user_ns     = get64(p + 40);
    f->cpu_sys_ns      = get64(p + 48);
    f->rss_bytes       = get64(p + 56);
    f->heap_bytes      = get64(p + 64);
    f->mem_limit_bytes = get64(p + 72);
    f->conns           = get32(p + 80);
    f->workers         = get32(p + 84);
    f->cores           = get32(p + 88);
    memcpy(f->query, p + RAY_STATUS_FRAME_FIXED, qn);
    return true;
}

#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* Facts the thread cannot read safely itself (the pool's size and the
 * affinity mask are the main thread's to read): refreshed by the main
 * thread each time a stream is opened. */
static _Atomic(uint32_t) g_workers;
static _Atomic(uint32_t) g_cores;
static _Atomic(int64_t)  g_mem_limit;

/* What the main thread writes to the pipe for each socket it hands over.
 * Smaller than PIPE_BUF, so each arrives whole. */
typedef struct { int32_t fd; uint32_t interval_ms; } handoff_t;

static int   g_pipe[2] = { -1, -1 };
static pid_t g_pid;                 /* the process the thread was started in */

typedef struct { int fd; int64_t interval_ns; int64_t due; } stream_t;

static size_t frame_now(uint8_t* out)
{
    ray_status_snap_t s;
    ray_status_read(&s);
    ray_proc_usage_t u;
    ray_os_proc_usage(&u);

    ray_status_frame_t f = {0};
    f.state = s.kind == RAY_WORK_IDLE ? RAY_STATUS_IDLE
            : s.kind == RAY_WORK_IPC  ? RAY_STATUS_IPC : RAY_STATUS_OTHER;
    f.pid             = (uint32_t)getpid();
    f.now_ns          = ray_timestamp_now_ns() + RAY_STATUS_EPOCH_UNIX_NS;
    f.busy_since_ns   = s.since_ts ? s.since_ts + RAY_STATUS_EPOCH_UNIX_NS : 0;
    f.loop_busy_ns    = s.busy_ns;
    f.loop_total_ns   = s.total_ns;
    f.cpu_user_ns     = u.cpu_user_ns;
    f.cpu_sys_ns      = u.cpu_sys_ns;
    f.rss_bytes       = u.rss_bytes;
    f.heap_bytes      = (uint64_t)ray_heap_anon_committed();
    f.mem_limit_bytes = (uint64_t)atomic_load_explicit(&g_mem_limit, memory_order_relaxed);
    f.conns           = s.conns_in + s.conns_out;
    f.workers         = atomic_load_explicit(&g_workers, memory_order_relaxed);
    f.cores           = atomic_load_explicit(&g_cores, memory_order_relaxed);
    if (s.kind == RAY_WORK_TIMER)
        f.query_len = (uint16_t)snprintf(f.query, sizeof(f.query), "timer %lld", (long long)s.handle);
    else {
        f.query_len = (uint16_t)s.query_len;
        memcpy(f.query, s.query, s.query_len);
    }

    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_SERDE_WIRE_VERSION,
        .flags   = 0,
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = RAY_IPC_MSG_STATUS,
    };
    size_t n = ray_status_frame_encode(&f, out + sizeof(hdr));
    hdr.size = (int64_t)n;
    memcpy(out, &hdr, sizeof(hdr));
    return sizeof(hdr) + n;
}

static void sampler_main(void* arg)
{
    (void)arg;
    /* Signals are the main thread's: a cancel's SIGURG and the terminal's
     * SIGINT must never land here. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGURG);
    sigaddset(&sigs, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    int      rd = g_pipe[0];
    stream_t st[RAY_STATUS_STREAMS_MAX];
    int      n = 0;

    for (;;) {
        /* Sleep until the next frame is due or a socket is handed over;
         * with no stream, until one is. */
        int64_t now = ray_time_now_ns();
        int wait_ms = -1;
        for (int i = 0; i < n; i++) {
            int64_t left = st[i].due - now;
            int ms = left <= 0 ? 0 : (int)((left + 999999) / 1000000);
            if (wait_ms < 0 || ms < wait_ms) wait_ms = ms;
        }
        struct pollfd pf = { rd, POLLIN, 0 };
        if (poll(&pf, 1, wait_ms) < 0 && errno != EINTR) ray_sleep_ms(100);

        handoff_t h;
        while (read(rd, &h, sizeof(h)) == (ssize_t)sizeof(h)) {
            if (n == RAY_STATUS_STREAMS_MAX) { close(h.fd); continue; }
            st[n++] = (stream_t){ h.fd, (int64_t)h.interval_ms * 1000000, 0 };   /* first frame now */
        }

        now = ray_time_now_ns();
        uint8_t frame[sizeof(ray_ipc_header_t) + RAY_STATUS_FRAME_MAX];
        size_t  flen = 0;
        for (int i = n - 1; i >= 0; i--) {
            if (st[i].due > now) continue;
            /* Nothing is expected from the reader.  Whatever it sent is
             * dropped here, a frame's worth an interval, and its close
             * (a read of 0) ends the stream. */
            uint8_t junk[256];
            ssize_t got = recv(st[i].fd, junk, sizeof(junk), MSG_DONTWAIT);
            bool gone = got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
            if (!gone) {
                if (!flen) flen = frame_now(frame);
                /* All of it now, or the reader is too slow to keep. */
                gone = send(st[i].fd, frame, flen, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)flen;
            }
            if (gone) {
                close(st[i].fd);
                st[i] = st[--n];
                continue;
            }
            st[i].due = (st[i].due ? st[i].due : now) + st[i].interval_ns;
            if (st[i].due <= now) st[i].due = now + st[i].interval_ns;   /* fell behind: no burst */
        }
        atomic_store_explicit(&ray_status_streams, (uint32_t)n, memory_order_relaxed);
    }
}

/* Test hook, so a test can fill a reader's socket in seconds: the send
 * buffer given to the sockets handed over from now on (0: as the system
 * sets it) and the shortest interval allowed them (0: the real one). */
static _Atomic(int)      g_sndbuf_for_test;
static _Atomic(uint32_t) g_min_ms_for_test;
void ray_status_stream_for_test(int sndbuf, uint32_t min_interval_ms)
{
    atomic_store_explicit(&g_sndbuf_for_test, sndbuf, memory_order_relaxed);
    atomic_store_explicit(&g_min_ms_for_test, min_interval_ms, memory_order_relaxed);
}

static bool sampler_start(void)
{
    if (g_pipe[1] >= 0 && g_pid == getpid()) return true;
    /* A forked child has the parent's pipe and no thread: start over. */
    if (g_pipe[1] >= 0) { close(g_pipe[0]); close(g_pipe[1]); g_pipe[0] = g_pipe[1] = -1; }
    if (pipe(g_pipe) != 0) { g_pipe[0] = g_pipe[1] = -1; return false; }
    for (int i = 0; i < 2; i++) {
        fcntl(g_pipe[i], F_SETFL, fcntl(g_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(g_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    /* Blocked here too, so the thread never runs a moment with them open. */
    sigset_t sigs, old;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGURG);
    sigaddset(&sigs, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigs, &old);
    ray_thread_t t;
    ray_err_t err = ray_thread_create(&t, sampler_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err != RAY_OK) {
        close(g_pipe[0]); close(g_pipe[1]);
        g_pipe[0] = g_pipe[1] = -1;
        return false;
    }
    g_pid = getpid();
    return true;
}

bool ray_status_stream_add(int64_t fd, uint32_t interval_ms)
{
    uint32_t min_ms = atomic_load_explicit(&g_min_ms_for_test, memory_order_relaxed);
    if (!min_ms) min_ms = RAY_STATUS_INTERVAL_MIN_MS;
    if (interval_ms < min_ms) interval_ms = min_ms;
    if (interval_ms > RAY_STATUS_INTERVAL_MAX_MS) interval_ms = RAY_STATUS_INTERVAL_MAX_MS;
    atomic_store_explicit(&g_workers, ray_pool_size(), memory_order_relaxed);
    atomic_store_explicit(&g_cores, ray_cpu_set_count(), memory_order_relaxed);
    atomic_store_explicit(&g_mem_limit, ray_heap_anon_watermark_explicit(), memory_order_relaxed);
    int sndbuf = atomic_load_explicit(&g_sndbuf_for_test, memory_order_relaxed);
    if (sndbuf > 0) setsockopt((int)fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    handoff_t h = { (int32_t)fd, interval_ms };
    if (sampler_start() && write(g_pipe[1], &h, sizeof(h)) == (ssize_t)sizeof(h))
        return true;
    close((int)fd);
    return false;
}

#else   /* no stream on this platform: the request is refused */

void ray_status_stream_for_test(int sndbuf, uint32_t min_interval_ms) { (void)sndbuf; (void)min_interval_ms; }

bool ray_status_stream_add(int64_t fd, uint32_t interval_ms)
{
    (void)interval_ms;
    ray_sock_close((ray_sock_t)fd);
    return false;
}

#endif

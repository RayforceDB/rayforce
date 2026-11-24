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

#include "core/status.h"
#include "core/platform.h"
#include "core/timer.h"     /* ray_time_now_ns */
#include <rayforce.h>       /* ray_timestamp_now_ns */
#include <string.h>

#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS)
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#endif
#if defined(RAY_OS_MACOS)
#include <libproc.h>
#endif
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#endif

#define QUERY_WORDS (RAY_STATUS_QUERY_MAX / 8)

/* Every field is an atomic so a reader on another thread is never a data
 * race.  The fields under `seq` change together: the writer makes `seq`
 * odd, stores them, makes it even again; a reader that saw the same even
 * value before and after its loads has a consistent set. */
static struct {
    _Atomic(uint32_t) seq;
    /* under seq: the running work */
    _Atomic(uint32_t) kind;
    _Atomic(int64_t)  handle;
    _Atomic(int64_t)  since_ts;
    _Atomic(int64_t)  since_mono;
    _Atomic(uint32_t) query_len;
    _Atomic(uint64_t) query[QUERY_WORDS];
    /* under seq: the loop's clock.  busy_ns and idle_ns hold the phases
     * that ended; the one running since phase_mono is added by readers. */
    _Atomic(uint32_t) waiting;
    _Atomic(int64_t)  phase_mono;   /* 0 until the loop first waits */
    _Atomic(uint64_t) busy_ns;
    _Atomic(uint64_t) idle_ns;
    /* each on its own */
    _Atomic(uint64_t) iterations;
    _Atomic(uint64_t) work_count;
    _Atomic(uint64_t) work_max_ns;
    _Atomic(uint32_t) conns_in;
    _Atomic(uint32_t) conns_out;
} g_st;

/* Written by the stream's thread (core/status_stream.c). */
_Atomic(uint32_t) ray_status_streams;

#define LD(x)    atomic_load_explicit(&(x), memory_order_relaxed)
#define ST(x, v) atomic_store_explicit(&(x), (v), memory_order_relaxed)

static inline void seq_flip(void) { atomic_fetch_add(&g_st.seq, 1); }

/* Depth of work on this thread and the start of the outermost.  Thread
 * local: the rule is one serving thread per process, but tests run a
 * server beside their own thread. */
static _Thread_local int     tl_depth;
static _Thread_local int64_t tl_t0;

static void text_store(const char* text, size_t len)
{
    if (len > RAY_STATUS_QUERY_MAX) len = RAY_STATUS_QUERY_MAX;
    uint64_t w[QUERY_WORDS];
    size_t nw = (len + 7) / 8;
    if (nw) w[nw - 1] = 0;
    if (len) memcpy(w, text, len);
    for (size_t i = 0; i < nw; i++) ST(g_st.query[i], w[i]);
    ST(g_st.query_len, (uint32_t)len);
}

void ray_status_begin(uint32_t kind, int64_t handle, int64_t since_ts,
                      const char* text, size_t len)
{
    if (tl_depth++ > 0) return;
    tl_t0 = ray_time_now_ns();
    if (!since_ts) since_ts = ray_timestamp_now_ns();
    seq_flip();
    ST(g_st.kind, kind);
    ST(g_st.handle, handle);
    ST(g_st.since_ts, since_ts);
    ST(g_st.since_mono, tl_t0);
    text_store(text, text ? len : 0);
    seq_flip();
}

void ray_status_end(void)
{
    if (--tl_depth > 0) return;
    uint64_t ran = (uint64_t)(ray_time_now_ns() - tl_t0);
    seq_flip();
    ST(g_st.kind, RAY_WORK_IDLE);
    ST(g_st.handle, -1);
    ST(g_st.since_ts, 0);
    ST(g_st.since_mono, 0);
    ST(g_st.query_len, 0);
    seq_flip();
    if (ran > LD(g_st.work_max_ns)) ST(g_st.work_max_ns, ran);
    ST(g_st.work_count, LD(g_st.work_count) + 1);
}

bool ray_status_wants_text(void)
{
    return tl_depth == 1 && LD(g_st.kind) == RAY_WORK_IPC;
}

void ray_status_set_text(const char* text, size_t len)
{
    seq_flip();
    text_store(text, len);
    seq_flip();
}

/* ---- the loop's clock ---- */

/* Points (time, busy so far) on the loop's busy line, taken when it
 * changes between working and waiting but no closer than HIST_STEP_NS:
 * between two neighbours the line is read as straight, which is off by at
 * most one step of work.  Enough of them for the longest window asked. */
#define HIST_LEN     2048u
#define HIST_STEP_NS 50000000LL
static _Atomic(int64_t)  g_hist_t[HIST_LEN];
static _Atomic(uint64_t) g_hist_busy[HIST_LEN];
static _Atomic(uint32_t) g_hist_n;     /* points written, ever */

static void hist_note(int64_t now, uint64_t busy)
{
    uint32_t n = LD(g_hist_n);
    if (n && now - LD(g_hist_t[(n - 1) % HIST_LEN]) < HIST_STEP_NS) return;
    ST(g_hist_t[n % HIST_LEN], now);
    ST(g_hist_busy[n % HIST_LEN], busy);
    ST(g_hist_n, n + 1);
}

static void loop_phase(bool waiting)
{
    int64_t  now  = ray_time_now_ns();
    int64_t  prev = LD(g_st.phase_mono);
    uint64_t ran  = prev && now > prev ? (uint64_t)(now - prev) : 0;
    uint64_t busy = LD(g_st.busy_ns) + (waiting ? ran : 0);
    seq_flip();
    if (waiting) ST(g_st.busy_ns, busy);
    else         ST(g_st.idle_ns, LD(g_st.idle_ns) + ran);
    ST(g_st.phase_mono, now);
    ST(g_st.waiting, waiting ? 1u : 0u);
    seq_flip();
    hist_note(now, busy);
}

void ray_status_wait_begin(void)
{
    loop_phase(true);
    ST(g_st.iterations, LD(g_st.iterations) + 1);
}

void ray_status_wait_end(void) { loop_phase(false); }

void ray_status_conn_opened(bool inbound)
{
    atomic_fetch_add_explicit(inbound ? &g_st.conns_in : &g_st.conns_out, 1,
                              memory_order_relaxed);
}

void ray_status_conn_closed(bool inbound)
{
    atomic_fetch_sub_explicit(inbound ? &g_st.conns_in : &g_st.conns_out, 1,
                              memory_order_relaxed);
}

void ray_status_read(ray_status_snap_t* out)
{
    uint64_t w[QUERY_WORDS];
    uint64_t busy = 0, idle = 0;
    int64_t  phase = 0;
    uint32_t waiting = 0;
    /* Bounded: a reader must never wait on the writer.  Past the bound the
     * last copy is used as it is, each field still a whole value. */
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s1 = atomic_load(&g_st.seq);
        if (s1 & 1) continue;
        out->kind       = LD(g_st.kind);
        out->handle     = LD(g_st.handle);
        out->since_ts   = LD(g_st.since_ts);
        out->since_mono = LD(g_st.since_mono);
        out->query_len  = LD(g_st.query_len);
        if (out->query_len > RAY_STATUS_QUERY_MAX) out->query_len = RAY_STATUS_QUERY_MAX;
        size_t nw = ((size_t)out->query_len + 7) / 8;
        for (size_t i = 0; i < nw; i++) w[i] = LD(g_st.query[i]);
        waiting = LD(g_st.waiting);
        phase   = LD(g_st.phase_mono);
        busy    = LD(g_st.busy_ns);
        idle    = LD(g_st.idle_ns);
        if (atomic_load(&g_st.seq) == s1) break;
    }
    if (out->kind == RAY_WORK_IDLE) { out->handle = -1; out->since_ts = 0; out->query_len = 0; }
    memcpy(out->query, w, out->query_len);
    int64_t now = ray_time_now_ns();
    uint64_t ran = phase && now > phase ? (uint64_t)(now - phase) : 0;
    if (waiting) idle += ran; else busy += ran;
    out->waiting     = waiting != 0;
    out->busy_ns     = busy;
    out->total_ns    = busy + idle;
    out->iterations  = LD(g_st.iterations);
    out->work_count  = LD(g_st.work_count);
    out->work_max_ns = LD(g_st.work_max_ns);
    out->conns_in    = LD(g_st.conns_in);
    out->conns_out   = LD(g_st.conns_out);
    out->streams     = LD(ray_status_streams);
}

double ray_status_utilisation(int64_t window_ns)
{
    ray_status_snap_t s;
    ray_status_read(&s);
    int64_t  now  = ray_time_now_ns();
    int64_t  from = now - window_ns;
    uint32_t n    = LD(g_hist_n);
    uint32_t have = n < HIST_LEN ? n : HIST_LEN;
    if (!have) return s.total_ns ? (double)s.busy_ns / (double)s.total_ns : 0.0;

    /* Newest point at or before `from` and the one after it (the present
     * when there is none): the busy line is straight between them. */
    int64_t  t0 = 0, t1 = now;
    uint64_t b0 = 0, b1 = s.busy_ns;
    bool found = false;
    for (uint32_t k = 0; k < have; k++) {
        uint32_t i = (n - 1 - k) % HIST_LEN;
        int64_t  t = LD(g_hist_t[i]);
        uint64_t b = LD(g_hist_busy[i]);
        if (t <= from) { t0 = t; b0 = b; found = true; break; }
        t1 = t; b1 = b;
    }
    uint64_t at;
    if (!found) { from = t1; at = b1; }           /* the loop is younger than the window */
    else if (t1 <= t0) at = b0;
    else at = b0 + (uint64_t)((double)(b1 - b0) * (double)(from - t0) / (double)(t1 - t0));
    if (now <= from || s.busy_ns <= at) return 0.0;
    double u = (double)(s.busy_ns - at) / (double)(now - from);
    return u > 1.0 ? 1.0 : u;
}

/* ---- the process ---- */

void ray_os_proc_usage(ray_proc_usage_t* out)
{
    memset(out, 0, sizeof(*out));
#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        out->cpu_user_ns = (uint64_t)ru.ru_utime.tv_sec * 1000000000ull + (uint64_t)ru.ru_utime.tv_usec * 1000ull;
        out->cpu_sys_ns  = (uint64_t)ru.ru_stime.tv_sec * 1000000000ull + (uint64_t)ru.ru_stime.tv_usec * 1000ull;
#if defined(RAY_OS_MACOS)
        out->rss_peak_bytes = (uint64_t)ru.ru_maxrss;            /* bytes here */
#else
        out->rss_peak_bytes = (uint64_t)ru.ru_maxrss * 1024ull;  /* kilobytes here */
#endif
    }
#endif
#if defined(RAY_OS_MACOS)
    struct proc_taskinfo ti;
    if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == (int)sizeof(ti)) {
        out->rss_bytes = ti.pti_resident_size;
        out->threads   = (uint32_t)ti.pti_threadnum;
    }
#elif defined(RAY_OS_LINUX)
    /* /proc/self/stat: "pid (comm) state ..." with num_threads the 20th
     * field and rss, in pages, the 24th.  comm may hold spaces and
     * parentheses, so count from the last ')'. */
    char buf[1024];
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char* p = strrchr(buf, ')');
    for (int field = 2; p && *p; ) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        field++;
        if (field == 20) out->threads = (uint32_t)strtoul(p, NULL, 10);
        if (field == 24) {
            out->rss_bytes = strtoull(p, NULL, 10) * (uint64_t)sysconf(_SC_PAGESIZE);
            break;
        }
    }
#elif defined(RAY_OS_WINDOWS)
    /* CPU time only: resident memory and the thread count are not read here. */
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        out->cpu_user_ns = (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime) * 100ull;
        out->cpu_sys_ns  = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) * 100ull;
    }
#endif
}

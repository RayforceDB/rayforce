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

#include "core/shm.h"
#include "mem/sys.h"

#if defined(__linux__)

#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SHM_MAGIC    0x72616679736872ULL   /* "rayfshr" */
#define SHM_PAGE     4096
#define SHM_SPIN_NS  30000                 /* a writer waits this long for room before sleeping */
#define SHM_COPY_MAX (1u << 22)            /* one copy at most this long: the head moves often */

/* One direction's positions, each on its own cache line: the writer moves
 * head, the reader tail; each says when it sleeps on its eventfd. */
typedef struct {
    _Alignas(64) _Atomic uint64_t head;
    _Alignas(64) _Atomic uint64_t tail;
    _Alignas(64) _Atomic uint32_t reader_sleeps;
    _Alignas(64) _Atomic uint32_t writer_sleeps;
} shm_dir_t;

/* The file's first page; the rings follow it: server to client, then
 * client to server. */
typedef struct {
    uint64_t  magic;
    uint64_t  ring;
    shm_dir_t dir[2];
} shm_control_t;
_Static_assert(sizeof(shm_control_t) <= SHM_PAGE, "control page");

typedef struct {
    ray_xport_t    base;
    void*          map;
    uint64_t       ring;
    int            mine, peer;      /* the eventfd I sleep on; the one I ring */
    shm_dir_t*     out;
    shm_dir_t*     in;
    uint8_t*       out_ring;
    uint8_t*       in_ring;
} shm_xport_t;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void ring_bell(int fd)
{
    uint64_t one = 1;
    ssize_t r = write(fd, &one, sizeof one);
    (void)r;
}

static void quiet_bell(int fd)
{
    uint64_t v;
    ssize_t r = read(fd, &v, sizeof v);   /* non-blocking */
    (void)r;
}

static bool shm_room(ray_xport_t* x)
{
    shm_xport_t* s = (shm_xport_t*)x;
    return atomic_load_explicit(&s->out->head, memory_order_relaxed) -
           atomic_load_explicit(&s->out->tail, memory_order_acquire) < s->ring;
}

static void wait_for_room(shm_xport_t* s)
{
    int64_t until = now_ns() + SHM_SPIN_NS;
    while (now_ns() < until)
        if (shm_room(&s->base)) return;
    atomic_store_explicit(&s->out->writer_sleeps, 1, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);   /* the flag before the look, on every machine */
    if (!shm_room(&s->base)) {
        struct pollfd p = { s->mine, POLLIN, 0 };
        poll(&p, 1, 100);
    }
    atomic_store_explicit(&s->out->writer_sleeps, 0, memory_order_relaxed);
    quiet_bell(s->mine);
}

static int64_t shm_write(ray_xport_t* x, const uint8_t* p, int64_t len, bool block)
{
    shm_xport_t* s = (shm_xport_t*)x;
    int64_t taken = 0;
    while (len > 0) {
        uint64_t head = atomic_load_explicit(&s->out->head, memory_order_relaxed);
        uint64_t room = s->ring - (head - atomic_load_explicit(&s->out->tail, memory_order_acquire));
        if (room == 0) {
            if (!block) return taken;
            wait_for_room(s);
            continue;
        }
        uint64_t at = head % s->ring;
        uint64_t k  = (uint64_t)len;
        if (k > room) k = room;
        if (k > SHM_COPY_MAX) k = SHM_COPY_MAX;
        uint64_t first = k < s->ring - at ? k : s->ring - at;
        memcpy(s->out_ring + at, p, first);
        if (k > first) memcpy(s->out_ring, p + first, k - first);
        atomic_store_explicit(&s->out->head, head + k, memory_order_seq_cst);
        if (atomic_load_explicit(&s->out->reader_sleeps, memory_order_seq_cst)) ring_bell(s->peer);
        p     += k;
        len   -= (int64_t)k;
        taken += (int64_t)k;
    }
    return taken;
}

static int64_t shm_read(ray_xport_t* x, uint8_t* p, int64_t len)
{
    shm_xport_t* s = (shm_xport_t*)x;
    uint64_t tail  = atomic_load_explicit(&s->in->tail, memory_order_relaxed);
    uint64_t avail = atomic_load_explicit(&s->in->head, memory_order_acquire) - tail;
    if (avail == 0 || len <= 0) return 0;
    uint64_t k = (uint64_t)len < avail ? (uint64_t)len : avail;
    uint64_t at = tail % s->ring;
    uint64_t first = k < s->ring - at ? k : s->ring - at;
    memcpy(p, s->in_ring + at, first);
    if (k > first) memcpy(p + first, s->in_ring, k - first);
    atomic_store_explicit(&s->in->tail, tail + k, memory_order_seq_cst);
    if (atomic_load_explicit(&s->in->writer_sleeps, memory_order_seq_cst)) ring_bell(s->peer);
    return (int64_t)k;
}

static bool shm_pending(ray_xport_t* x)
{
    shm_xport_t* s = (shm_xport_t*)x;
    return atomic_load_explicit(&s->in->head, memory_order_acquire) !=
           atomic_load_explicit(&s->in->tail, memory_order_relaxed);
}

static void shm_woke(ray_xport_t* x)
{
    shm_xport_t* s = (shm_xport_t*)x;
    atomic_store_explicit(&s->in->reader_sleeps, 0, memory_order_relaxed);
    atomic_store_explicit(&s->out->writer_sleeps, 0, memory_order_relaxed);
    quiet_bell(s->mine);
}

static bool shm_prepare_sleep(ray_xport_t* x, bool want_data, bool want_room)
{
    shm_xport_t* s = (shm_xport_t*)x;
    if (want_data) atomic_store_explicit(&s->in->reader_sleeps, 1, memory_order_seq_cst);
    if (want_room) atomic_store_explicit(&s->out->writer_sleeps, 1, memory_order_seq_cst);
    /* The flags must be visible before the look below, or a writer that
     * moved head between the two sees no sleeper and rings nothing: a
     * full fence orders the seq_cst stores before the acquire loads on
     * every memory model, not only the strongly ordered ones. */
    atomic_thread_fence(memory_order_seq_cst);
    if ((want_data && shm_pending(x)) || (want_room && shm_room(x))) {   /* news between the look and the flags */
        shm_woke(x);
        return false;
    }
    return true;
}

static void shm_destroy(ray_xport_t* x)
{
    shm_xport_t* s = (shm_xport_t*)x;
    size_t bytes = SHM_PAGE + 2 * (size_t)s->ring;
    munmap(s->map, bytes);
    ray_sys_track_sub((int64_t)bytes);
    close(s->mine);
    close(s->peer);
    ray_sys_free(s);
}

static const ray_xport_ops_t shm_ops = {
    .read          = shm_read,
    .write         = shm_write,
    .pending       = shm_pending,
    .room          = shm_room,
    .prepare_sleep = shm_prepare_sleep,
    .woke          = shm_woke,
    .destroy       = shm_destroy,
};

static void* shm_map(int fd, uint64_t ring)
{
    size_t bytes = SHM_PAGE + 2 * (size_t)ring;
    /* Faulted in now, not mid-transfer: a page fault per 4 KB inside a
     * copy measured slower than the whole map up front. */
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (p == MAP_FAILED) return NULL;
    ray_sys_track_add((int64_t)bytes);
    return p;
}

static shm_xport_t* shm_new(void* map, uint64_t ring, bool server, int mine, int peer)
{
    shm_xport_t* s = (shm_xport_t*)ray_sys_alloc(sizeof(shm_xport_t));
    if (!s) return NULL;
    shm_control_t* c = (shm_control_t*)map;
    uint8_t* rings = (uint8_t*)map + SHM_PAGE;
    s->base.ops           = &shm_ops;
    s->base.event_fd      = mine;
    s->base.kind          = RAY_XPORT_SHM;
    s->base.fd_tells_room = true;
    s->map      = map;
    s->ring     = ring;
    s->mine     = mine;
    s->peer     = peer;
    s->out      = &c->dir[server ? 0 : 1];
    s->in       = &c->dir[server ? 1 : 0];
    s->out_ring = rings + (server ? 0 : ring);
    s->in_ring  = rings + (server ? ring : 0);
    return s;
}

const char* ray_shm_host_identity(void)
{
    static char id[RAY_SHM_IDENTITY_LEN + 4];
    static bool read_once = false;
    if (!read_once) {
        read_once = true;
        int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t r = read(fd, id, RAY_SHM_IDENTITY_LEN);
            close(fd);
            if (r != RAY_SHM_IDENTITY_LEN) id[0] = 0;
            else id[RAY_SHM_IDENTITY_LEN] = 0;
        }
    }
    return id;
}

uint64_t ray_shm_ring_bytes(void)
{
    /* 16 MB a direction, faulted in when the connection upgrades (about
     * 2 ms): larger rings cost setup and gain nothing measured. */
    return RAY_SHM_RING_DEFAULT;
}

ray_xport_t* ray_shm_create(uint64_t ring, int fds[3])
{
    int f = -1;
#ifdef SYS_memfd_create
    f = (int)syscall(SYS_memfd_create, "rayforce-ring", 1u /* MFD_CLOEXEC */);
#endif
    if (f < 0) return NULL;
    int client = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int server = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    void* map = NULL;
    if (client >= 0 && server >= 0 && ftruncate(f, (off_t)(SHM_PAGE + 2 * ring)) == 0)
        map = shm_map(f, ring);
    int mine = map ? dup(server) : -1, peer = map ? dup(client) : -1;
    shm_xport_t* s = (map && mine >= 0 && peer >= 0) ? shm_new(map, ring, true, mine, peer) : NULL;
    if (!s) {
        int all[5] = { f, client, server, mine, peer };
        for (int i = 0; i < 5; i++) if (all[i] >= 0) close(all[i]);
        if (map) { munmap(map, SHM_PAGE + 2 * (size_t)ring); ray_sys_track_sub((int64_t)(SHM_PAGE + 2 * ring)); }
        return NULL;
    }
    shm_control_t* c = (shm_control_t*)map;
    memset(c, 0, sizeof *c);
    c->magic = SHM_MAGIC;
    c->ring  = ring;
    fds[0] = f;
    fds[1] = client;
    fds[2] = server;
    return &s->base;
}

ray_xport_t* ray_shm_attach(uint64_t ring, const int fds[3])
{
    /* The ring size came over the wire and the file from a socket: both
     * are checked against each other and against sense before anything
     * is indexed by them. */
    struct stat st;
    bool sane = ring > 0 && ring <= RAY_SHM_RING_MAX &&
                fstat(fds[0], &st) == 0 && (uint64_t)st.st_size == SHM_PAGE + 2 * ring;
    void* map = sane ? shm_map(fds[0], ring) : NULL;
    close(fds[0]);
    shm_control_t* c = (shm_control_t*)map;
    shm_xport_t* s = NULL;
    if (map && c->magic == SHM_MAGIC && c->ring == ring)
        s = shm_new(map, ring, false, fds[1], fds[2]);
    if (!s) {
        if (map) { munmap(map, SHM_PAGE + 2 * (size_t)ring); ray_sys_track_sub((int64_t)(SHM_PAGE + 2 * ring)); }
        close(fds[1]);
        close(fds[2]);
        return NULL;
    }
    return &s->base;
}

#else

const char*  ray_shm_host_identity(void) { return ""; }
uint64_t     ray_shm_ring_bytes(void) { return 0; }
ray_xport_t* ray_shm_create(uint64_t ring, int fds[3]) { (void)ring; (void)fds; return NULL; }
ray_xport_t* ray_shm_attach(uint64_t ring, const int fds[3]) { (void)ring; (void)fds; return NULL; }

#endif

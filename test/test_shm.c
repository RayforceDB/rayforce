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

/* The same-host transport's rings (src/core/shm.c), both ends in one
 * process: bytes cross in order through a wrap, the ring's room is the
 * writer's limit, a sleeping reader is woken through its eventfd and an
 * awake one is not, attaching refuses a file that is not a ring, a
 * peer's positions that make no sense break the link, and a process makes
 * no more rings than its budget. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "core/shm.h"

#include <string.h>
#include <stdlib.h>
#if defined(__linux__)
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__linux__)

#define RING (1u << 20)   /* 1 MB a direction: wraps within a few writes */

static bool fd_readable(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, 0) > 0;
}

static test_result_t test_shm_round_trip_and_wrap(void) {
    int fds[3];
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    ray_xport_t* cli = ray_shm_attach(RING, fds);
    TEST_ASSERT_NOT_NULL(cli);
    TEST_ASSERT_EQ_I(srv->kind, RAY_XPORT_SHM);
    TEST_ASSERT_TRUE(srv->ops->room(srv));
    TEST_ASSERT_FALSE(srv->ops->pending(srv));
    TEST_ASSERT_FALSE(cli->ops->pending(cli));

    /* 5 chunks of 300 KB server -> client, read back as they come: the
     * stream wraps past the 1 MB ring end and stays in order. */
    size_t chunk = 300 * 1024;
    uint8_t* out = (uint8_t*)malloc(chunk);
    uint8_t* in  = (uint8_t*)malloc(chunk);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(in);
    for (int c = 0; c < 5; c++) {
        for (size_t i = 0; i < chunk; i++) out[i] = (uint8_t)(i * 7 + (size_t)c);
        size_t at = 0;
        while (at < chunk) {
            int64_t n = srv->ops->write(srv, out + at, (int64_t)(chunk - at), false);
            TEST_ASSERT((n) > (0), "write takes bytes while the ring has room");
            at += (size_t)n;
        }
        TEST_ASSERT_TRUE(cli->ops->pending(cli));
        at = 0;
        while (at < chunk) {
            int64_t n = cli->ops->read(cli, in + at, (int64_t)(chunk - at));
            TEST_ASSERT((n) > (0), "read gives what was written");
            at += (size_t)n;
        }
        TEST_ASSERT_EQ_I(memcmp(in, out, chunk), 0);
        TEST_ASSERT_FALSE(cli->ops->pending(cli));
        TEST_ASSERT_EQ_I(cli->ops->read(cli, in, 16), 0);
    }

    /* The other direction, a small frame. */
    const char* hello = "client to server";
    TEST_ASSERT_EQ_I(cli->ops->write(cli, (const uint8_t*)hello, 16, false), 16);
    TEST_ASSERT_TRUE(srv->ops->pending(srv));
    TEST_ASSERT_EQ_I(srv->ops->read(srv, in, 16), 16);
    TEST_ASSERT_EQ_I(memcmp(in, hello, 16), 0);

    free(out);
    free(in);
    cli->ops->destroy(cli);
    srv->ops->destroy(srv);
    PASS();
}

static test_result_t test_shm_room_is_the_limit(void) {
    int fds[3];
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    ray_xport_t* cli = ray_shm_attach(RING, fds);
    TEST_ASSERT_NOT_NULL(cli);

    uint8_t* buf = (uint8_t*)calloc(1, 2 * RING);
    TEST_ASSERT_NOT_NULL(buf);
    /* A non-blocking write takes exactly the ring, then nothing. */
    int64_t took = 0;
    for (;;) {
        int64_t n = srv->ops->write(srv, buf, (int64_t)(2 * RING), false);
        if (n <= 0) break;
        took += n;
    }
    TEST_ASSERT_EQ_I(took, (int64_t)RING);
    TEST_ASSERT_FALSE(srv->ops->room(srv));
    TEST_ASSERT_EQ_I(srv->ops->write(srv, buf, 1, false), 0);
    /* Reading frees room, byte for byte. */
    TEST_ASSERT_EQ_I(cli->ops->read(cli, buf, 4096), 4096);
    TEST_ASSERT_TRUE(srv->ops->room(srv));
    TEST_ASSERT_EQ_I(srv->ops->write(srv, buf, 8192, false), 4096);
    free(buf);
    cli->ops->destroy(cli);
    srv->ops->destroy(srv);
    PASS();
}

static test_result_t test_shm_bell_only_for_a_sleeper(void) {
    int fds[3];
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    ray_xport_t* cli = ray_shm_attach(RING, fds);
    TEST_ASSERT_NOT_NULL(cli);
    uint8_t b[8] = {0};

    /* Awake reader: a write rings nothing. */
    TEST_ASSERT_EQ_I(srv->ops->write(srv, b, 8, false), 8);
    TEST_ASSERT_FALSE(fd_readable(cli->event_fd));
    TEST_ASSERT_EQ_I(cli->ops->read(cli, b, 8), 8);

    /* A reader about to sleep with nothing pending may sleep; the next
     * write rings its eventfd; waking quietens it. */
    TEST_ASSERT_TRUE(cli->ops->prepare_sleep(cli, true, false));
    TEST_ASSERT_EQ_I(srv->ops->write(srv, b, 8, false), 8);
    TEST_ASSERT_TRUE(fd_readable(cli->event_fd));
    cli->ops->woke(cli);
    TEST_ASSERT_FALSE(fd_readable(cli->event_fd));
    TEST_ASSERT_TRUE(cli->ops->pending(cli));
    /* With bytes already there, prepare_sleep refuses the sleep. */
    TEST_ASSERT_FALSE(cli->ops->prepare_sleep(cli, true, false));
    TEST_ASSERT_EQ_I(cli->ops->read(cli, b, 8), 8);

    /* A writer waiting for room is rung when the reader frees some. */
    uint8_t* big = (uint8_t*)calloc(1, RING);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_EQ_I(srv->ops->write(srv, big, (int64_t)RING, false), (int64_t)RING);
    TEST_ASSERT_TRUE(srv->ops->prepare_sleep(srv, false, true));
    /* A writer waiting for room only is not kept awake by inbound bytes. */
    srv->ops->woke(srv);
    TEST_ASSERT_EQ_I(cli->ops->write(cli, b, 8, false), 8);
    TEST_ASSERT_TRUE(srv->ops->prepare_sleep(srv, false, true));
    TEST_ASSERT_FALSE(srv->ops->prepare_sleep(srv, true, true));
    TEST_ASSERT_EQ_I(srv->ops->read(srv, b, 8), 8);
    TEST_ASSERT_TRUE(srv->ops->prepare_sleep(srv, false, true));
    TEST_ASSERT_FALSE(fd_readable(srv->event_fd));
    TEST_ASSERT_EQ_I(cli->ops->read(cli, big, 4096), 4096);
    TEST_ASSERT_TRUE(fd_readable(srv->event_fd));
    srv->ops->woke(srv);
    TEST_ASSERT_FALSE(fd_readable(srv->event_fd));
    free(big);
    cli->ops->destroy(cli);
    srv->ops->destroy(srv);
    PASS();
}

static test_result_t test_shm_attach_refuses_a_stranger(void) {
    int fds[3];
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    /* The wrong ring size for this file: refused, descriptors consumed. */
    ray_xport_t* cli = ray_shm_attach(RING / 2, fds);
    TEST_ASSERT_NULL(cli);
    srv->ops->destroy(srv);
    /* A zero ring, and one past the cap: refused before the file is read. */
    srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_NULL(ray_shm_attach(0, fds));
    srv->ops->destroy(srv);
    srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_NULL(ray_shm_attach(RAY_SHM_RING_MAX + 1, fds));
    srv->ops->destroy(srv);
    PASS();
}

/* The control page as src/core/shm.c lays it out: magic, ring size, then
 * per direction (server to client first) head, tail and the two sleeping
 * flags, each on its own 64-byte line. */
#define CTL_MAGIC   0
#define CTL_RING    8
#define CTL_DIR(d)  (64 + (d) * 256)
#define CTL_HEAD(d) (CTL_DIR(d))
#define CTL_TAIL(d) (CTL_DIR(d) + 64)

static uint8_t* map_control(int fd) {
    void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return p == MAP_FAILED ? NULL : (uint8_t*)p;
}

static void put_u64(uint8_t* ctl, size_t off, uint64_t v) {
    memcpy(ctl + off, &v, sizeof v);
}

static test_result_t test_shm_attach_refuses_a_bad_header(void) {
    int fds[3];
    /* A file of the right size whose magic is not ours. */
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    uint8_t* ctl = map_control(fds[0]);
    TEST_ASSERT_NOT_NULL(ctl);
    put_u64(ctl, CTL_MAGIC, 0x1234);
    munmap(ctl, 4096);
    TEST_ASSERT_NULL(ray_shm_attach(RING, fds));
    srv->ops->destroy(srv);
    /* The right magic, but a ring size that disagrees with the offer. */
    srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    ctl = map_control(fds[0]);
    TEST_ASSERT_NOT_NULL(ctl);
    put_u64(ctl, CTL_RING, RING / 2);
    munmap(ctl, 4096);
    TEST_ASSERT_NULL(ray_shm_attach(RING, fds));
    srv->ops->destroy(srv);
    PASS();
}

static test_result_t test_shm_peer_positions_break_the_link(void) {
    int fds[3];
    ray_xport_t* srv = ray_shm_create(RING, fds);
    TEST_ASSERT_NOT_NULL(srv);
    int file = dup(fds[0]);   /* the attach consumes the passed one */
    TEST_ASSERT((file) >= (0), "dup the ring file");
    ray_xport_t* cli = ray_shm_attach(RING, fds);
    TEST_ASSERT_NOT_NULL(cli);
    uint8_t* ctl = map_control(file);
    TEST_ASSERT_NOT_NULL(ctl);
    uint8_t buf[64] = {0};

    /* The reader's tail past the writer's head: no amount of room makes
     * sense of it, and the writer fails instead of overwriting. */
    TEST_ASSERT_EQ_I(srv->ops->write(srv, buf, 16, false), 16);
    put_u64(ctl, CTL_TAIL(0), 17);
    TEST_ASSERT_EQ_I(srv->ops->write(srv, buf, 16, false), -1);

    /* The writer's head more than a ring ahead of the reader's tail: the
     * reader fails instead of reading bytes that were overwritten. */
    put_u64(ctl, CTL_HEAD(1), (uint64_t)RING + 1);
    TEST_ASSERT_EQ_I(srv->ops->read(srv, buf, 16), -1);

    munmap(ctl, 4096);
    close(file);
    cli->ops->destroy(cli);
    srv->ops->destroy(srv);
    PASS();
}

static test_result_t test_shm_budget_bounds_the_rings(void) {
    /* Default-sized rings until the budget refuses one: the pages come
     * only when written, so these cost descriptors and address space. */
    uint64_t ring = ray_shm_ring_bytes();
    uint64_t per  = 4096 + 2 * ring;
    uint64_t base = ray_shm_rings_in_use();
    int64_t  fit  = (int64_t)((RAY_SHM_BUDGET - base) / per);
    TEST_ASSERT((fit) > (0), "the budget holds some rings");
    TEST_ASSERT((fit) <= (128), "few enough to make here");
    ray_xport_t* made[128];
    int fds[128][3];
    int64_t n = 0;
    while (n < 128) {
        ray_xport_t* x = ray_shm_create(ring, fds[n]);
        if (!x) break;
        made[n++] = x;
    }
    TEST_ASSERT_EQ_I(n, fit);
    TEST_ASSERT_EQ_I((int64_t)ray_shm_rings_in_use(), (int64_t)(base + (uint64_t)n * per));
    /* One torn down makes room for one more, and no more. */
    made[n - 1]->ops->destroy(made[n - 1]);
    for (int i = 0; i < 3; i++) close(fds[n - 1][i]);
    made[n - 1] = ray_shm_create(ring, fds[n - 1]);
    TEST_ASSERT_NOT_NULL(made[n - 1]);
    int extra[3];
    TEST_ASSERT_NULL(ray_shm_create(ring, extra));
    for (int64_t i = 0; i < n; i++) {
        made[i]->ops->destroy(made[i]);
        for (int k = 0; k < 3; k++) close(fds[i][k]);
    }
    TEST_ASSERT_EQ_I((int64_t)ray_shm_rings_in_use(), (int64_t)base);
    PASS();
}

static test_result_t test_shm_host_identity(void) {
    const char* id = ray_shm_host_identity();
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_EQ_I((int64_t)strlen(id), RAY_SHM_IDENTITY_LEN);
    TEST_ASSERT((ray_shm_ring_bytes()) >= (1u << 20), "ring is at least a megabyte");
    PASS();
}

const test_entry_t shm_entries[] = {
    { "shm/round_trip_and_wrap",     test_shm_round_trip_and_wrap,     NULL, NULL },
    { "shm/room_is_the_limit",       test_shm_room_is_the_limit,       NULL, NULL },
    { "shm/bell_only_for_a_sleeper", test_shm_bell_only_for_a_sleeper, NULL, NULL },
    { "shm/attach_refuses_stranger", test_shm_attach_refuses_a_stranger, NULL, NULL },
    { "shm/attach_refuses_bad_header", test_shm_attach_refuses_a_bad_header, NULL, NULL },
    { "shm/peer_positions_break_link", test_shm_peer_positions_break_the_link, NULL, NULL },
    { "shm/budget_bounds_the_rings", test_shm_budget_bounds_the_rings, NULL, NULL },
    { "shm/host_identity",           test_shm_host_identity,           NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

#else

static test_result_t test_shm_unavailable(void) {
    int fds[3];
    TEST_ASSERT_NULL(ray_shm_create(1u << 20, fds));
    PASS();
}

const test_entry_t shm_entries[] = {
    { "shm/unavailable", test_shm_unavailable, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

#endif

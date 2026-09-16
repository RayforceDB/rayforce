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

#ifndef RAY_TEST_IPC_HARNESS_H
#define RAY_TEST_IPC_HARNESS_H

/*
 * ipc_harness.h -- the one way to stand up an IPC server in tests.
 *
 * There used to be two: the poll API that production uses
 * (ray_ipc_listen on a ray_poll_t) and a second server implementation
 * behind ray_ipc_server_t, which despite its header comment did not wrap
 * the poll layer at all — it ran its own epoll/kqueue loop, its own
 * conns[] array and its own copies of the handshake and the header
 * validator.  Nothing in src/ used it; only tests did, so every
 * wire-level change had to be made twice and the test path could drift
 * from the production one silently.
 *
 * These helpers are `static inline` so each test translation unit gets
 * its own copy without unused-function warnings.
 */

#include "test.h"
#include <rayforce.h>
#include "core/ipc.h"
#include "core/poll.h"
#include "core/sock.h"
#include "core/runtime.h"
#include "mem/sys.h"
#include "lang/internal.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <string.h>

static inline void ray_test_sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static inline uint16_t ray_test_listen_port(ray_sock_t fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname((int)fd, (struct sockaddr*)&addr, &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

/* A client-side poll must be published on the runtime: IPC handles are
 * selector ids resolved there, exactly as main.c does at startup. */
static inline ray_poll_t* ray_test_client_poll(void) {
    ray_poll_t* p = ray_poll_create();
    if (p) ray_runtime_set_poll(p);
    return p;
}

static inline void ray_test_client_poll_done(void) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (p) { ray_runtime_set_poll(NULL); ray_poll_destroy(p); }
}

typedef struct {
    ray_poll_t* poll;
    ray_vm_t*   vm;
    uint16_t    port;
    ray_thread_t tid;
} ray_test_server_t;

static inline void ray_test_server_thread(void* arg) {
    ray_test_server_t* s = (ray_test_server_t*)arg;
    __VM = s->vm;                 /* TLS VM so ray_eval_str works here */
    ray_poll_run(s->poll);        /* returns once poll->code >= 0 */
}

/* Stand up a listener on an ephemeral port and run its poll on a thread.
 * auth_secret NULL means no -u; restricted mirrors -U.  Both must be set
 * before ray_ipc_listen, which is why they are parameters rather than
 * fields the caller pokes afterwards.
 * Returns 0 on success, -1 on failure (caller fails the test). */
static inline int ray_test_server_start_opts(ray_test_server_t* s,
                                             const char* auth_secret,
                                             bool restricted) {
    memset(s, 0, sizeof(*s));
    s->poll = ray_poll_create();
    if (!s->poll) return -1;
    if (auth_secret) {
        size_t n = strlen(auth_secret);
        if (n >= sizeof(s->poll->auth_secret)) return -1;
        memcpy(s->poll->auth_secret, auth_secret, n + 1);
    }
    s->poll->restricted = restricted;
    int64_t id = ray_ipc_listen(s->poll, 0);
    if (id < 0) return -1;
    ray_selector_t* sel = ray_poll_get(s->poll, id);
    if (!sel) return -1;
    s->port = ray_test_listen_port((ray_sock_t)sel->fd);
    if (s->port == 0) return -1;

    s->vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    if (!s->vm) return -1;
    ray_vm_init(s->vm, 99);

    ray_thread_create(&s->tid, ray_test_server_thread, s);
    ray_test_sleep_ms(20);        /* let the thread reach poll_run */
    return 0;
}

static inline int ray_test_server_start(ray_test_server_t* s) {
    return ray_test_server_start_opts(s, NULL, false);
}

/* ray_poll_run blocks in epoll_wait, so setting the exit code is not
 * enough — connect a throwaway socket to generate an accept event that
 * wakes it. */
static inline void ray_test_server_stop(ray_test_server_t* s) {
    ray_poll_exit(s->poll, 0);
    ray_sock_t k = ray_sock_connect("127.0.0.1", s->port, 200);
    if (k != RAY_INVALID_SOCK) ray_sock_close(k);
    ray_thread_join(s->tid);
    ray_poll_destroy(s->poll);
    ray_sys_free(s->vm);
    memset(s, 0, sizeof(*s));
}

#define RAY_TEST_SERVER_START(s) \
    TEST_ASSERT_EQ_I(ray_test_server_start(&(s)), 0)

#define RAY_TEST_SERVER_START_OPTS(s, secret, restricted) \
    TEST_ASSERT_EQ_I(ray_test_server_start_opts(&(s), (secret), (restricted)), 0)

#endif /* RAY_TEST_IPC_HARNESS_H */

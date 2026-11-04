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

/* The cross-host transport's bounds (src/core/rdma.c) that hold before
 * any byte crosses: a peer may not describe a receiving ring larger than
 * this side makes (its staging mirrors it, registered and pinned), and a
 * server's endpoints together stay inside RAY_RDMA_BUDGET.  Needs an
 * active RDMA port; skipped on a host without one. */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "core/rdma.h"

#include <string.h>

static test_result_t test_rdma_peer_ring_is_bounded(void) {
    if (!ray_rdma_available()) SKIP("no RDMA port on this host");
    ray_rdma_record_t mine, peer;
    uint64_t sizes[] = { 0, RAY_RDMA_RING_MIN - 1, RAY_RDMA_RING_DEFAULT + 1, 1ULL << 31 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        ray_xport_t* a = ray_rdma_create(&mine, false);
        ray_xport_t* b = ray_rdma_create(&peer, false);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_NULL(b);
        peer.ring_size = sizes[i];
        TEST_ASSERT((ray_rdma_connect(a, &peer)) == (false), "an out-of-bounds ring is refused");
        a->ops->destroy(a);
        b->ops->destroy(b);
    }
    /* The bound itself connects, and so does a small but legal ring. */
    uint64_t ok[] = { RAY_RDMA_RING_DEFAULT, RAY_RDMA_RING_MIN };
    for (size_t i = 0; i < 2; i++) {
        ray_xport_t* a = ray_rdma_create(&mine, false);
        ray_xport_t* b = ray_rdma_create(&peer, false);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_NULL(b);
        peer.ring_size = ok[i];
        TEST_ASSERT(ray_rdma_connect(a, &peer), "an in-bounds ring connects");
        a->ops->destroy(a);
        b->ops->destroy(b);
    }
    PASS();
}

static test_result_t test_rdma_budget_bounds_server_endpoints(void) {
    if (!ray_rdma_available()) SKIP("no RDMA port on this host");
    const uint64_t per = 2 * RAY_RDMA_RING_DEFAULT;
    uint64_t base = ray_rdma_bytes_in_use();
    int64_t  fit  = (int64_t)((RAY_RDMA_BUDGET - base) / per);
    TEST_ASSERT((fit) > (0), "the budget holds some endpoints");
    TEST_ASSERT((fit) <= (32), "few enough to make here");
    ray_xport_t* made[32];
    ray_rdma_record_t rec;
    int64_t n = 0;
    while (n < 32) {
        ray_xport_t* x = ray_rdma_create(&rec, true);
        if (!x) break;
        made[n++] = x;
    }
    TEST_ASSERT_EQ_I(n, fit);
    TEST_ASSERT_EQ_I((int64_t)ray_rdma_bytes_in_use(), (int64_t)(base + (uint64_t)n * per));
    /* A client's endpoint is not counted, and still made. */
    ray_xport_t* client = ray_rdma_create(&rec, false);
    TEST_ASSERT_NOT_NULL(client);
    client->ops->destroy(client);
    /* One torn down makes room for one more, and no more. */
    made[n - 1]->ops->destroy(made[n - 1]);
    made[n - 1] = ray_rdma_create(&rec, true);
    TEST_ASSERT_NOT_NULL(made[n - 1]);
    TEST_ASSERT_NULL(ray_rdma_create(&rec, true));
    for (int64_t i = 0; i < n; i++) made[i]->ops->destroy(made[i]);
    TEST_ASSERT_EQ_I((int64_t)ray_rdma_bytes_in_use(), (int64_t)base);
    PASS();
}

const test_entry_t rdma_entries[] = {
    { "rdma/peer_ring_is_bounded",          test_rdma_peer_ring_is_bounded,          NULL, NULL },
    { "rdma/budget_bounds_server_endpoints", test_rdma_budget_bounds_server_endpoints, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

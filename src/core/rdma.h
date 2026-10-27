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

#ifndef RAY_RDMA_H
#define RAY_RDMA_H

#include "core/poll.h"

/* ===== The remote-memory transport =====
 *
 * Across hosts with RDMA (InfiniBand, or RoCE v2 over Ethernet) a
 * connection's frames move between registered memory without the kernel.
 * The design is the same-host ring at a distance: each side registers a
 * receiving ring once; a sender copies a frame's bytes into its staging
 * ring and writes them into the peer's ring with an RDMA write whose
 * immediate value counts the bytes, which lands on the receiver's
 * completion queue; the receiver gives space back with an empty write
 * whose immediate has the high bit set.  Completions are polled with a
 * short spin, then waited on through the completion channel's descriptor
 * in the poll loop like everything else.
 *
 * The verbs library is loaded the first time a connection wants it, and
 * only if it is installed: a build with the verbs headers runs on a host
 * without the library, and every connection there stays on its socket. */

/* What a side tells the peer when a connection upgrades: how to reach
 * its queue pair, and where its receiving ring is.  Travels as is. */
typedef struct {
    uint32_t version;       /* RAY_RDMA_RECORD_VERSION */
    uint32_t qpn, psn;
    uint32_t rkey;
    uint16_t lid;
    uint8_t  link_layer;    /* 1 InfiniBand, 2 Ethernet */
    uint8_t  mtu;           /* the verbs MTU enumeration */
    uint8_t  gid[16];
    uint32_t reserved;
    uint64_t ring, ring_size;
} ray_rdma_record_t;
_Static_assert(sizeof(ray_rdma_record_t) == 56, "rdma record layout");
#define RAY_RDMA_RECORD_VERSION 1

#define RAY_RDMA_RING_DEFAULT (64ULL << 20)

/* Whether this host can upgrade: the library loads and an active port
 * exists (the first active port with an address is taken). */
bool         ray_rdma_available(void);
/* A queue pair and a registered receiving ring, ready to be connected,
 * described in `mine`; NULL when it cannot be made. */
ray_xport_t* ray_rdma_create(ray_rdma_record_t* mine);
/* Connects to the peer the record describes; false on failure, after
 * which the transport is only good for destroying. */
bool         ray_rdma_connect(ray_xport_t* x, const ray_rdma_record_t* peer);
/* After connecting: one empty write to the peer's ring, waited for within
 * timeout_ms.  True when it completed, which is the only proof that the
 * two fabrics reach each other; false when the hardware gave up or the
 * time ran out, after which the transport is only good for destroying. */
bool         ray_rdma_probe(ray_xport_t* x, int timeout_ms);
/* The device in use, for reporting: NULL without one. */
const char*  ray_rdma_device_name(void);

#endif /* RAY_RDMA_H */

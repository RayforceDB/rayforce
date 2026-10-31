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

#include "core/rdma.h"
#include "core/platform.h"
#include "mem/sys.h"

#if defined(RAY_HAVE_VERBS) && defined(__linux__)

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The library's entry points, looked up by name: only those every verbs
 * library has exported for years, so an older one serves as well.  The
 * inline wrappers of the header (post_send, poll_cq, ...) go through the
 * context's own operation table and need no symbol. */
typedef struct {
    bool ok;
    struct ibv_device**       (*get_device_list)(int*);
    void                      (*free_device_list)(struct ibv_device**);
    const char*               (*get_device_name)(struct ibv_device*);
    struct ibv_context*       (*open_device)(struct ibv_device*);
    int                       (*close_device)(struct ibv_context*);
    int                       (*query_port)(struct ibv_context*, uint8_t, void*);   /* the exported form takes the older, shorter struct */
    int                       (*query_gid)(struct ibv_context*, uint8_t, int, union ibv_gid*);
    struct ibv_pd*            (*alloc_pd)(struct ibv_context*);
    int                       (*dealloc_pd)(struct ibv_pd*);
    struct ibv_mr*            (*reg_mr)(struct ibv_pd*, void*, size_t, int);
    int                       (*dereg_mr)(struct ibv_mr*);
    struct ibv_comp_channel*  (*create_comp_channel)(struct ibv_context*);
    int                       (*destroy_comp_channel)(struct ibv_comp_channel*);
    struct ibv_cq*            (*create_cq)(struct ibv_context*, int, void*, struct ibv_comp_channel*, int);
    int                       (*destroy_cq)(struct ibv_cq*);
    int                       (*get_cq_event)(struct ibv_comp_channel*, struct ibv_cq**, void**);
    void                      (*ack_cq_events)(struct ibv_cq*, unsigned int);
    struct ibv_qp*            (*create_qp)(struct ibv_pd*, struct ibv_qp_init_attr*);
    int                       (*destroy_qp)(struct ibv_qp*);
    int                       (*modify_qp)(struct ibv_qp*, struct ibv_qp_attr*, int);
} rdma_api_t;

static rdma_api_t g_api;
static bool       g_api_tried;

#define RDMA_SYM(field, name) do { \
        *(void**)&g_api.field = dlsym(h, name); \
        if (!g_api.field) ok = false; \
    } while (0)

static rdma_api_t* api(void)
{
    if (g_api_tried) return &g_api;
    g_api_tried = true;
    void* h = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return &g_api;
    bool ok = true;
    RDMA_SYM(get_device_list,      "ibv_get_device_list");
    RDMA_SYM(free_device_list,     "ibv_free_device_list");
    RDMA_SYM(get_device_name,      "ibv_get_device_name");
    RDMA_SYM(open_device,          "ibv_open_device");
    RDMA_SYM(close_device,         "ibv_close_device");
    RDMA_SYM(query_port,           "ibv_query_port");
    RDMA_SYM(query_gid,            "ibv_query_gid");
    RDMA_SYM(alloc_pd,             "ibv_alloc_pd");
    RDMA_SYM(dealloc_pd,           "ibv_dealloc_pd");
    RDMA_SYM(reg_mr,               "ibv_reg_mr");
    RDMA_SYM(dereg_mr,             "ibv_dereg_mr");
    RDMA_SYM(create_comp_channel,  "ibv_create_comp_channel");
    RDMA_SYM(destroy_comp_channel, "ibv_destroy_comp_channel");
    RDMA_SYM(create_cq,            "ibv_create_cq");
    RDMA_SYM(destroy_cq,           "ibv_destroy_cq");
    RDMA_SYM(get_cq_event,         "ibv_get_cq_event");
    RDMA_SYM(ack_cq_events,        "ibv_ack_cq_events");
    RDMA_SYM(create_qp,            "ibv_create_qp");
    RDMA_SYM(destroy_qp,           "ibv_destroy_qp");
    RDMA_SYM(modify_qp,            "ibv_modify_qp");
    g_api.ok = ok;
    return &g_api;
}

/* The device and port a connection uses, found once. */
typedef struct {
    struct ibv_context* ctx;
    char                name[64];
    uint8_t             num;
    bool                ib;
    int                 gid_index;
    uint16_t            lid;
    uint8_t             mtu;
    union ibv_gid       gid;
} rdma_port_t;

/* A port's attributes, in a buffer wider than any library's struct. */
typedef struct {
    struct ibv_port_attr a;
    unsigned char        spare[256];
} rdma_port_attr_t;

static rdma_port_t g_port;
static bool        g_port_tried;

/* For Ethernet: the GID index whose type is RoCE v2, as the kernel lists it. */
static int roce_v2_index(const char* dev, int port, int n)
{
    for (int i = 0; i < n; i++) {
        char path[256];
        snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d", dev, port, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[32] = {0};
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r > 0 && strstr(buf, "RoCE v2")) return i;
    }
    return -1;
}

static rdma_port_t* port(void)
{
    if (g_port_tried) return &g_port;
    g_port_tried = true;
    rdma_api_t* a = api();
    if (!a->ok) return &g_port;
    int n = 0;
    struct ibv_device** list = a->get_device_list(&n);
    if (!list) return &g_port;
    for (int d = 0; d < n && !g_port.ctx; d++) {
        const char* name = a->get_device_name(list[d]);
        struct ibv_context* ctx = a->open_device(list[d]);
        if (!ctx) continue;
        for (uint8_t pn = 1; pn <= 2 && !g_port.ctx; pn++) {
            rdma_port_attr_t pa;
            memset(&pa, 0, sizeof pa);
            if (a->query_port(ctx, pn, &pa) != 0 || pa.a.state != IBV_PORT_ACTIVE) continue;
            bool ib = pa.a.link_layer != IBV_LINK_LAYER_ETHERNET;
            int gi = ib ? 0 : roce_v2_index(name, pn, pa.a.gid_tbl_len);
            if (gi < 0) continue;
            union ibv_gid g;
            memset(&g, 0, sizeof g);
            if (a->query_gid(ctx, pn, gi, &g) != 0) continue;
            g_port.ctx       = ctx;
            snprintf(g_port.name, sizeof g_port.name, "%s", name ? name : "");
            g_port.num       = pn;
            g_port.ib        = ib;
            g_port.gid_index = gi;
            g_port.lid       = pa.a.lid;
            g_port.mtu       = (uint8_t)pa.a.active_mtu;
            g_port.gid       = g;
        }
        if (!g_port.ctx) a->close_device(ctx);
    }
    a->free_device_list(list);
    return &g_port;
}

#define RDMA_CREDIT       0x80000000u   /* an immediate that returns ring space */
#define RDMA_RECVS        1024
#define RDMA_SENDS        1024
#define RDMA_SIGNAL_EVERY 128
#define RDMA_SPIN_NS      50000         /* a waiting side polls this long before sleeping */
#define RDMA_COPY_MAX     (1u << 22)
/* Sends outstanding at most (the queue's depth less a signalling
 * interval: a send is known done only when a later signalled one is), and
 * of them ring writes: the rest is kept for credits, so a sender whose
 * queue is full of its own bytes can still return the peer's space. */
#define RDMA_SQ_MAX       (RDMA_SENDS - RDMA_SIGNAL_EVERY)
#define RDMA_SQ_DATA      (RDMA_SQ_MAX - 16)

typedef struct {
    ray_xport_t              base;
    rdma_port_t*             port;
    struct ibv_pd*           pd;
    struct ibv_comp_channel* ch;
    struct ibv_cq*           cq;
    struct ibv_qp*           qp;
    struct ibv_mr*           rx_mr;
    struct ibv_mr*           tx_mr;
    uint8_t*                 rx;          /* the ring the peer writes into */
    uint8_t*                 tx;          /* the staging the writes go from, the peer's ring's size */
    uint64_t                 rx_size, tx_size;
    uint32_t                 psn;
    /* Receiving: bytes landed and not yet read, read in all, read since the peer last heard. */
    uint64_t                 rx_avail, rx_pos, rx_unreported;
    /* Sending: bytes written in all, the peer's reading position as last heard. */
    uint64_t                 tx_pos, peer_read;
    uint64_t                 peer_ring;
    uint32_t                 peer_rkey;
    int                      unsignaled, outstanding;
    bool                     failed;
    bool                     armed;       /* the completion queue will raise an event on the channel */
    bool                     connected;
    bool                     probe_done;  /* the probe's own completion was seen */
} rdma_xport_t;

#define RDMA_WR_PROBE 2   /* the probe's work request id; ring writes carry 0 */

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool ray_rdma_available(void)
{
    return api()->ok && port()->ctx != NULL;
}

const char* ray_rdma_device_name(void)
{
    return port()->ctx ? g_port.name : NULL;
}

static bool post_recvs(rdma_xport_t* e, int n)
{
    for (int i = 0; i < n; i++) {
        struct ibv_recv_wr wr, *bad = NULL;
        memset(&wr, 0, sizeof wr);
        wr.wr_id = 1;
        if (ibv_post_recv(e->qp, &wr, &bad) != 0) { e->failed = true; return false; }
    }
    return true;
}

/* Takes the completions that have arrived: bytes landed, space freed.
 * No system call: the queue is read from memory, so the spins that call
 * this cost nothing but the spin.  False when the connection failed. */
static bool progress(rdma_xport_t* e)
{
    struct ibv_wc wc[64];
    for (;;) {
        int n = ibv_poll_cq(e->cq, 64, wc);
        if (n < 0) e->failed = true;
        if (n <= 0) break;
        int reposts = 0;
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) { e->failed = true; continue; }
            if (wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                uint32_t imm = ntohl(wc[i].imm_data);
                /* The peer's word is bounded by what the rings allow: a
                 * credit past what was sent, or bytes past the ring, name
                 * nothing that exists. */
                if (imm & RDMA_CREDIT) {
                    uint64_t credit = imm & ~RDMA_CREDIT;
                    if (credit > e->tx_pos - e->peer_read) credit = e->tx_pos - e->peer_read;
                    e->peer_read += credit;
                } else {
                    uint64_t landed = imm;
                    if (landed > e->rx_size - e->rx_avail) landed = e->rx_size - e->rx_avail;
                    e->rx_avail += landed;
                }
                reposts++;
            } else if (wc[i].wr_id == RDMA_WR_PROBE) {
                e->probe_done = true;
            } else {
                e->outstanding -= RDMA_SIGNAL_EVERY;   /* a signalled send stands for the ones before it */
            }
        }
        if (reposts) post_recvs(e, reposts);
    }
    return !e->failed;
}

/* Takes the channel's events, which is what makes its descriptor readable
 * when the armed queue completes something, and re-arms it.  A system call
 * or two: for the moments around a sleep, not for a spin. */
static void drain_events(rdma_xport_t* e)
{
    rdma_api_t* a = api();
    struct ibv_cq* cq;
    void* ctx;
    unsigned events = 0;
    struct pollfd p = { e->ch->fd, POLLIN, 0 };
    if (poll(&p, 1, 0) > 0) {   /* the channel is non-blocking */
        while (a->get_cq_event(e->ch, &cq, &ctx) == 0) events++;
        if (events) a->ack_cq_events(e->cq, events);
        e->armed = false;
    }
    if (!e->armed) {
        ibv_req_notify_cq(e->cq, 0);   /* armed first, then polled: nothing slips between */
        e->armed = true;
    }
}

/* The send queue takes another send while fewer than `limit` are
 * outstanding.  A peer that stops taking completions — busy with a long
 * query — leaves our sends unacknowledged: a non-blocking writer then
 * reports no progress and the poll loop waits for the completions on the
 * channel, as it waits for ring space. */
static bool sq_room(rdma_xport_t* e, int limit)
{
    if (e->outstanding >= limit) progress(e);
    return e->outstanding < limit;
}

/* One write of ring bytes (or none, for a credit) with its immediate;
 * the caller has seen room for it in the send queue. */
static bool post_write(rdma_xport_t* e, uint64_t off, uint32_t n, uint32_t imm)
{
    if (e->failed) return false;
    struct ibv_sge sge;
    memset(&sge, 0, sizeof sge);
    sge.addr   = (uint64_t)(uintptr_t)(e->tx + off);
    sge.length = n;
    sge.lkey   = e->tx_mr->lkey;
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof wr);
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.sg_list             = n ? &sge : NULL;
    wr.num_sge             = n ? 1 : 0;
    wr.imm_data            = htonl(imm);
    wr.wr.rdma.remote_addr = e->peer_ring + off;
    wr.wr.rdma.rkey        = e->peer_rkey;
    if (++e->unsignaled == RDMA_SIGNAL_EVERY) {
        wr.send_flags = IBV_SEND_SIGNALED;
        e->unsignaled = 0;
    }
    e->outstanding++;
    if (ibv_post_send(e->qp, &wr, &bad) != 0) e->failed = true;
    return !e->failed;
}

/* What was read goes back to the sender as ring space: in quarters of the
 * ring, or all of it once everything that landed is read. */
static void report(rdma_xport_t* e)
{
    /* A full send queue defers the credit: the next look (a read, a
     * pending check, a sleep) sends it once a completion has made room. */
    if (e->rx_unreported &&
        (e->rx_unreported >= e->rx_size / 4 ||
         (e->rx_avail == 0 && e->rx_unreported >= e->rx_size / 16) ||
         (e->rx_avail == 0 && e->rx_unreported >= 65536)) &&
        sq_room(e, RDMA_SQ_MAX)) {
        post_write(e, 0, 0, RDMA_CREDIT | (uint32_t)e->rx_unreported);
        e->rx_unreported = 0;
    }
}

static bool rdma_room(ray_xport_t* x)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    if (e->tx_pos - e->peer_read == e->tx_size) progress(e);
    return e->failed ||
           (e->tx_pos - e->peer_read < e->tx_size && sq_room(e, RDMA_SQ_DATA));
}

/* A blocking writer's wait for the peer: its credit, or the completions
 * that free the send queue. */
static void wait_peer(rdma_xport_t* e)
{
    drain_events(e);
    struct pollfd pf = { e->ch->fd, POLLIN, 0 };
    poll(&pf, 1, 10);
    drain_events(e);
}

static int64_t rdma_write(ray_xport_t* x, const uint8_t* p, int64_t len, bool block)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    int64_t taken = 0;
    while (len > 0) {
        if (e->failed) return -1;
        uint64_t used = e->tx_pos - e->peer_read, room = e->tx_size - used;
        if (room == 0) {
            if (!progress(e)) return -1;
            if (e->tx_pos - e->peer_read == e->tx_size) {
                if (!block) return taken;
                wait_peer(e);
            }
            continue;
        }
        if (!sq_room(e, RDMA_SQ_DATA)) {
            if (e->failed) return -1;
            if (!block) return taken;
            wait_peer(e);
            continue;
        }
        uint64_t off = e->tx_pos % e->tx_size;
        uint64_t k = (uint64_t)len;
        if (k > room) k = room;
        if (k > e->tx_size - off) k = e->tx_size - off;
        if (k > RDMA_COPY_MAX) k = RDMA_COPY_MAX;
        memcpy(e->tx + off, p, k);
        if (!post_write(e, off, (uint32_t)k, (uint32_t)k)) return -1;
        e->tx_pos += k;
        p     += k;
        len   -= (int64_t)k;
        taken += (int64_t)k;
    }
    return taken;
}

static int64_t rdma_read(ray_xport_t* x, uint8_t* p, int64_t len)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    if (!progress(e) && !e->rx_avail) return -1;
    int64_t got = 0;
    while (len > 0 && e->rx_avail) {
        uint64_t off = e->rx_pos % e->rx_size;
        uint64_t k = (uint64_t)len;
        if (k > e->rx_avail) k = e->rx_avail;
        if (k > e->rx_size - off) k = e->rx_size - off;
        memcpy(p, e->rx + off, k);
        e->rx_pos        += k;
        e->rx_avail      -= k;
        e->rx_unreported += k;
        p   += k;
        len -= (int64_t)k;
        got += (int64_t)k;
    }
    report(e);
    return got;
}

static bool rdma_pending(ray_xport_t* x)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    progress(e);
    report(e);   /* a credit a full send queue deferred */
    return e->rx_avail || e->failed;
}

static bool rdma_prepare_sleep(ray_xport_t* x, bool want_data, bool want_room)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    /* Armed, then polled: a completion between the two is seen here, one
     * after raises the channel. */
    drain_events(e);
    progress(e);
    report(e);
    if (e->failed) return false;
    if (want_data && e->rx_avail) return false;
    if (want_room && e->tx_pos - e->peer_read < e->tx_size && e->outstanding < RDMA_SQ_DATA)
        return false;
    return true;
}

static void rdma_woke(ray_xport_t* x)
{
    drain_events((rdma_xport_t*)x);   /* the channel quiet again, the queue re-armed */
}

static void rdma_destroy(ray_xport_t* x)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    rdma_api_t* a = api();
    if (e->qp) a->destroy_qp(e->qp);
    if (e->rx_mr) a->dereg_mr(e->rx_mr);
    if (e->tx_mr) a->dereg_mr(e->tx_mr);
    if (e->cq) a->destroy_cq(e->cq);
    if (e->ch) a->destroy_comp_channel(e->ch);
    if (e->pd) a->dealloc_pd(e->pd);
    if (e->rx) ray_vm_free(e->rx, (size_t)e->rx_size);
    if (e->tx) ray_vm_free(e->tx, (size_t)e->tx_size);
    ray_sys_free(e);
}

static const ray_xport_ops_t rdma_ops = {
    .read          = rdma_read,
    .write         = rdma_write,
    .pending       = rdma_pending,
    .room          = rdma_room,
    .prepare_sleep = rdma_prepare_sleep,
    .woke          = rdma_woke,
    .destroy       = rdma_destroy,
};

ray_xport_t* ray_rdma_create(ray_rdma_record_t* mine)
{
    if (!ray_rdma_available()) return NULL;
    rdma_api_t* a = api();
    rdma_port_t* p = port();
    rdma_xport_t* e = (rdma_xport_t*)ray_sys_alloc(sizeof(rdma_xport_t));
    if (!e) return NULL;
    e->base.ops           = &rdma_ops;
    e->base.kind          = RAY_XPORT_RDMA;
    e->base.fd_tells_room = true;   /* credits are completions on the same channel */
    e->port    = p;
    e->rx_size = RAY_RDMA_RING_DEFAULT;
    e->rx      = (uint8_t*)ray_vm_alloc((size_t)e->rx_size);
    e->pd      = a->alloc_pd(p->ctx);
    e->ch      = e->pd ? a->create_comp_channel(p->ctx) : NULL;
    e->cq      = e->ch ? a->create_cq(p->ctx, RDMA_RECVS + RDMA_SENDS + 16, NULL, e->ch, 0) : NULL;
    if (e->cq) {
        struct ibv_qp_init_attr qa;
        memset(&qa, 0, sizeof qa);
        qa.send_cq = qa.recv_cq = e->cq;
        qa.qp_type = IBV_QPT_RC;
        qa.cap.max_send_wr  = RDMA_SENDS;
        qa.cap.max_recv_wr  = RDMA_RECVS;
        qa.cap.max_send_sge = qa.cap.max_recv_sge = 1;
        e->qp = a->create_qp(e->pd, &qa);
    }
    if (e->qp && e->rx)
        e->rx_mr = a->reg_mr(e->pd, e->rx, (size_t)e->rx_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!e->rx_mr) { rdma_destroy(&e->base); return NULL; }
    fcntl(e->ch->fd, F_SETFL, fcntl(e->ch->fd, F_GETFL) | O_NONBLOCK);
    e->base.event_fd = e->ch->fd;
    /* Receives for the writes that will land: each takes one, carrying no bytes itself. */
    if (!post_recvs(e, RDMA_RECVS)) { rdma_destroy(&e->base); return NULL; }
    ibv_req_notify_cq(e->cq, 0);
    e->armed = true;
    e->psn = (uint32_t)now_ns() & 0xffffff;
    memset(mine, 0, sizeof *mine);
    mine->version    = RAY_RDMA_RECORD_VERSION;
    mine->qpn        = e->qp->qp_num;
    mine->psn        = e->psn;
    mine->lid        = p->lid;
    mine->link_layer = p->ib ? 1 : 2;
    mine->mtu        = p->mtu;
    memcpy(mine->gid, p->gid.raw, 16);
    mine->ring       = (uint64_t)(uintptr_t)e->rx;
    mine->ring_size  = e->rx_size;
    mine->rkey       = e->rx_mr->rkey;
    return &e->base;
}

bool ray_rdma_connect(ray_xport_t* x, const ray_rdma_record_t* peer)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    rdma_api_t* a = api();
    rdma_port_t* p = e->port;
    if (peer->version != RAY_RDMA_RECORD_VERSION || peer->ring_size == 0 ||
        peer->ring_size > (1ULL << 31) || peer->link_layer != (p->ib ? 1 : 2))
        return false;
    /* The staging ring mirrors the peer's receiving ring. */
    e->tx_size = peer->ring_size;
    e->tx      = (uint8_t*)ray_vm_alloc((size_t)e->tx_size);
    if (!e->tx) return false;
    e->tx_mr = a->reg_mr(e->pd, e->tx, (size_t)e->tx_size, IBV_ACCESS_LOCAL_WRITE);
    if (!e->tx_mr) return false;
    e->peer_ring = peer->ring;
    e->peer_rkey = peer->rkey;
    struct ibv_qp_attr at;
    memset(&at, 0, sizeof at);
    at.qp_state        = IBV_QPS_INIT;
    at.pkey_index      = 0;
    at.port_num        = p->num;
    at.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (a->modify_qp(e->qp, &at, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0)
        return false;
    memset(&at, 0, sizeof at);
    at.qp_state           = IBV_QPS_RTR;
    at.path_mtu           = (enum ibv_mtu)(p->mtu < peer->mtu ? p->mtu : peer->mtu);
    at.dest_qp_num        = peer->qpn;
    at.rq_psn             = peer->psn;
    at.max_dest_rd_atomic = 1;
    at.min_rnr_timer      = 12;
    at.ah_attr.port_num   = p->num;
    if (p->ib) {
        at.ah_attr.dlid = peer->lid;
    } else {
        at.ah_attr.is_global = 1;
        memcpy(at.ah_attr.grh.dgid.raw, peer->gid, 16);
        at.ah_attr.grh.sgid_index = (uint8_t)p->gid_index;
        at.ah_attr.grh.hop_limit  = 64;
    }
    if (a->modify_qp(e->qp, &at,
                     IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                     IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0)
        return false;
    memset(&at, 0, sizeof at);
    at.qp_state      = IBV_QPS_RTS;
    at.timeout       = 14;
    at.retry_cnt     = 7;
    at.rnr_retry     = 7;
    at.sq_psn        = e->psn;
    at.max_rd_atomic = 1;
    if (a->modify_qp(e->qp, &at,
                     IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                     IBV_QP_MAX_QP_RD_ATOMIC) != 0)
        return false;
    /* The queue pair is ready to send before the peer's may be ready to
     * receive (a server answers before the client has connected); the
     * hardware retransmits across that gap, and the client's probe is the
     * first thing that crosses. */
    e->connected = true;
    return true;
}

bool ray_rdma_probe(ray_xport_t* x, int timeout_ms)
{
    rdma_xport_t* e = (rdma_xport_t*)x;
    if (!e->connected || e->failed) return false;
    /* An empty write carrying a credit of nothing: the peer takes it as
     * no news and reposts the receive it used.  Signalled, under its own
     * id, so its completion is told apart from the ring writes'. */
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof wr);
    wr.wr_id               = RDMA_WR_PROBE;
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data            = htonl(RDMA_CREDIT);
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = e->peer_ring;
    wr.wr.rdma.rkey        = e->peer_rkey;
    e->probe_done = false;
    if (ibv_post_send(e->qp, &wr, &bad) != 0) { e->failed = true; return false; }
    int64_t until = now_ns() + (int64_t)timeout_ms * 1000000LL;
    for (;;) {
        drain_events(e);
        progress(e);
        if (e->probe_done) return true;
        if (e->failed) return false;
        int64_t left_ms = (until - now_ns()) / 1000000LL;
        if (left_ms <= 0) { e->failed = true; return false; }
        struct pollfd p = { e->ch->fd, POLLIN, 0 };
        poll(&p, 1, left_ms > 10 ? 10 : (int)left_ms);
    }
}

#else

/* This build has no verbs headers, or is not Linux: no host can upgrade,
 * every connection stays on its socket. */
bool         ray_rdma_available(void) { return false; }
ray_xport_t* ray_rdma_create(ray_rdma_record_t* mine) { (void)mine; return NULL; }
bool         ray_rdma_connect(ray_xport_t* x, const ray_rdma_record_t* peer) { (void)x; (void)peer; return false; }
bool         ray_rdma_probe(ray_xport_t* x, int timeout_ms) { (void)x; (void)timeout_ms; return false; }
const char*  ray_rdma_device_name(void) { return NULL; }

#endif

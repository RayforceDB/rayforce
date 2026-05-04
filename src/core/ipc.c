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

#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE
#endif

#include "core/ipc.h"
#include "mem/sys.h"
#include "ops/ops.h"
#include "store/journal.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
#endif

#if defined(__linux__)
  #include <sys/epoll.h>
  #define RAY_IPC_MAX_EVENTS 64
#elif defined(__APPLE__)
  #include <sys/event.h>
  #define RAY_IPC_MAX_EVENTS 64
#endif

#include "lang/eval.h"

/* ===== Compression (delta + RLE) ===== */

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap)
{
    if (len <= RAY_IPC_COMPRESS_THRESHOLD) return 0;

    /* Step 1: delta-encode into temporary buffer */
    uint8_t* delta = (uint8_t*)ray_sys_alloc(len);
    if (!delta) return 0;

    delta[0] = src[0];
    for (size_t i = 1; i < len; i++)
        delta[i] = (uint8_t)(src[i] - src[i - 1]);

    /* Step 2: RLE-compress the delta stream */
    size_t di = 0;
    size_t si = 0;

    while (si < len) {
        if (si + 1 < len && delta[si] == delta[si + 1]) {
            uint8_t val = delta[si];
            size_t run = 1;
            while (si + run < len && delta[si + run] == val && run < 127)
                run++;
            if (di + 2 > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)run;
            dst[di++] = val;
            si += run;
        } else {
            size_t start = si;
            size_t llen = 0;
            while (si < len && llen < 128) {
                if (si + 1 < len && delta[si] == delta[si + 1])
                    break;
                si++;
                llen++;
            }
            if (di + 1 + llen > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)(-(int8_t)llen);
            memcpy(dst + di, delta + start, llen);
            di += llen;
        }
    }

    ray_sys_free(delta);
    if (di >= len) return 0;
    return di;
}

size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len)
{
    uint8_t* decoded = (uint8_t*)ray_sys_alloc(dst_len);
    if (!decoded) return 0;

    size_t si = 0;
    size_t di = 0;

    while (si < clen && di < dst_len) {
        int8_t count = (int8_t)src[si++];
        if (count > 0) {
            if (si >= clen) { ray_sys_free(decoded); return 0; }
            uint8_t val = src[si++];
            size_t n = (size_t)count;
            if (di + n > dst_len) { ray_sys_free(decoded); return 0; }
            memset(decoded + di, val, n);
            di += n;
        } else {
            size_t n = (size_t)(-(int)count);
            if (si + n > clen || di + n > dst_len) {
                ray_sys_free(decoded);
                return 0;
            }
            memcpy(decoded + di, src + si, n);
            si += n;
            di += n;
        }
    }

    /* Un-delta */
    if (di == 0) { ray_sys_free(decoded); return 0; }
    dst[0] = decoded[0];
    for (size_t i = 1; i < di; i++)
        dst[i] = (uint8_t)(decoded[i] + dst[i - 1]);

    ray_sys_free(decoded);
    return di;
}

/* ===== Shared protocol helpers ===== */

#define RAY_IPC_PHASE_HANDSHAKE 0
#define RAY_IPC_PHASE_HEADER    1
#define RAY_IPC_PHASE_PAYLOAD   2
#define RAY_IPC_PHASE_CREDS     3

/* Constant-time comparison — prevents timing side-channel on password. */
static bool ct_eq(const void* a, const void* b, size_t len) {
    const volatile uint8_t* x = a;
    const volatile uint8_t* y = b;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

/* Validate credential buffer against secret. Returns true if password matches.
 * creds is "user:password\0" with length cred_len.
 * secret MUST point to a char[256] buffer (zero-padded beyond the password).
 * Compares pw against the full 256-byte secret buffer in constant time.
 * No strlen, no secret-length-dependent copies. */
static bool validate_creds(const uint8_t* buf, uint8_t cred_len,
                           const char* secret) {
    if (cred_len == 0) return false;
    const char* creds = (const char*)buf;
    const char* colon = memchr(creds, ':', cred_len);
    const char* pw = colon ? colon + 1 : creds;
    size_t pw_len = colon ? (size_t)(cred_len - (pw - creds)) : cred_len;
    if (pw_len > 0 && pw[pw_len - 1] == '\0') pw_len--;
    if (pw_len > 255) pw_len = 255;

    /* Zero-pad pw into a 256-byte buffer, then compare all 256 bytes
     * against the secret buffer (also 256 bytes, zero-padded at init).
     * Matching passwords produce identical 256-byte buffers. */
    uint8_t pw_buf[256] = {0};
    memcpy(pw_buf, pw, pw_len);
    return ct_eq(pw_buf, secret, 256);
}

static void send_response(ray_sock_t fd, ray_t* result)
{
    int64_t ser_size = ray_serde_size(result);
    if (ser_size <= 0) return;

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)ser_size);
    if (!payload) return;
    ray_ser_raw(payload, result);

    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > RAY_IPC_COMPRESS_THRESHOLD) {
        uint8_t* comp = (uint8_t*)ray_sys_alloc((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress(payload, (size_t)ser_size,
                                           comp, (size_t)ser_size);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_sys_alloc(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_sys_free(comp);
        }
    }

    if (!send_buf) {
        send_buf = payload;
        send_len = (size_t)ser_size;
        payload  = NULL;
    }

    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_SERDE_WIRE_VERSION,
        .flags   = flags,
        .endian  = 0,
        .msgtype = RAY_IPC_MSG_RESP,
        .size    = (int64_t)send_len,
    };
    ray_sock_send(fd, &hdr, sizeof(hdr));
    ray_sock_send(fd, send_buf, send_len);

    ray_sys_free(send_buf);
    if (payload) ray_sys_free(payload);
}

/* Run the actual decompress + de-serialize + ray_eval pipeline on a
 * single inbound payload.  The wrapper eval_payload() below decides
 * whether to capture stdout/stderr (RAY_IPC_FLAG_VERBOSE) and whether
 * to wrap the result in a [captured_str, result] list. */
static ray_t* eval_payload_core(uint8_t* payload, size_t payload_len,
                                ray_ipc_header_t* hdr)
{
    /* Journal hook: log every inbound SYNC message (state-mutation
     * channel in q's model) before evaluation, so a crash mid-handler
     * still leaves the message on disk for replay.  We write the raw
     * inbound bytes — header + payload — verbatim, no decompression
     * round-trip.  Async messages and responses are not logged, so
     * background pings and result frames don't pollute the log.
     * No-op when no journal is open or during in-progress replay.
     *
     * RAY_IPC_FLAG_RESTRICTED is captured into a LOCAL header copy:
     * we mark the persisted frame with the connection's restricted
     * state at write time so replay can re-impose it.  Without this
     * a `-U` client's writes silently elevate to full privilege on
     * crash-recovery, since replay runs on the main thread with no
     * IPC connection context.  The bit is meaningless on the live
     * IPC wire and doesn't affect this handler's eval — that uses
     * the connection's own flag set by the caller above us.
     *
     * If the journal write fails (disk full, EIO), we ABORT the
     * eval and return an error to the client.  q's documented
     * behaviour: "the message has not been logged so we cannot
     * accept it".  Silently evaluating un-logged mutations defeats
     * the entire durability premise of `-l`/`-L`. */
    if (ray_journal_is_open() && hdr->msgtype == RAY_IPC_MSG_SYNC) {
        ray_ipc_header_t log_hdr = *hdr;
        if (ray_eval_get_restricted())
            log_hdr.flags |= RAY_IPC_FLAG_RESTRICTED;
        ray_err_t je = ray_journal_write_bytes(&log_hdr, payload, (int64_t)payload_len);
        if (je != RAY_OK) {
            fprintf(stderr, "log: ERROR  journal write failed (rc=%d) — refusing to evaluate\n", (int)je);
            return ray_error("io", "journal write failed; mutation refused");
        }
    }

    uint8_t* decompressed = NULL;
    if (hdr->flags & RAY_IPC_FLAG_COMPRESSED) {
        if (payload_len < 4) return NULL;
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        if (uncomp_size == 0 || uncomp_size > 256u * 1024u * 1024u) return NULL;
        decompressed = (uint8_t*)ray_sys_alloc(uncomp_size);
        if (!decompressed) return NULL;
        size_t dlen = ray_ipc_decompress(payload + 4, payload_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_sys_free(decompressed);
            return NULL;
        }
        payload     = decompressed;
        payload_len = uncomp_size;
    }

    int64_t de_len = (int64_t)payload_len;
    ray_t*  msg    = ray_de_raw(payload, &de_len);
    if (decompressed) ray_sys_free(decompressed);

    ray_t* result = NULL;
    if (msg && !RAY_IS_ERR(msg)) {
        if (msg->type == -RAY_STR) {
            const char* str  = ray_str_ptr(msg);
            size_t      slen = ray_str_len(msg);
            if (str && slen > 0) {
                char* tmp = (char*)ray_sys_alloc(slen + 1);
                if (tmp) {
                    memcpy(tmp, str, slen);
                    tmp[slen] = '\0';
                    result = ray_eval_str(tmp);
                    ray_sys_free(tmp);
                }
            }
            ray_release(msg);
        } else {
            result = ray_eval(msg);
            ray_release(msg);
        }
    }
    return result ? result : RAY_NULL_OBJ;
}

/* eval_payload — outer wrapper.  When the client sets
 * RAY_IPC_FLAG_VERBOSE on the request header, redirect stdout / stderr
 * to a tmpfile for the duration of the eval, then return a 2-element
 * RAY_LIST [captured_str, result] so the client can print the
 * captured output on its own terminal.  Without the flag the bare
 * result is returned (current behaviour, unchanged for existing
 * clients).
 *
 * On any failure of the capture setup (tmpfile, dup, dup2) we fall
 * back to the no-capture path: better to keep the eval running than
 * to fail the whole request because /tmp is full.  The captured
 * string is then empty, and the response shape is still the 2-elem
 * list — clients can rely on that invariant. */
static ray_t* eval_payload(uint8_t* payload, size_t payload_len,
                           ray_ipc_header_t* hdr)
{
    if (!(hdr->flags & RAY_IPC_FLAG_VERBOSE))
        return eval_payload_core(payload, payload_len, hdr);

    FILE* cap = tmpfile();
    int saved_out = -1, saved_err = -1;
    bool capturing = false;

    if (cap) {
        fflush(stdout); fflush(stderr);
        saved_out = dup(STDOUT_FILENO);
        saved_err = dup(STDERR_FILENO);
        if (saved_out >= 0 && saved_err >= 0) {
            int capfd = fileno(cap);
            if (dup2(capfd, STDOUT_FILENO) >= 0 &&
                dup2(capfd, STDERR_FILENO) >= 0) {
                capturing = true;
            }
        }
    }

    ray_t* result = eval_payload_core(payload, payload_len, hdr);

    char* captured = NULL;
    int   cap_len  = 0;
    if (capturing) {
        fflush(stdout); fflush(stderr);
        dup2(saved_out, STDOUT_FILENO);
        dup2(saved_err, STDERR_FILENO);
        long pos = ftell(cap);
        if (pos > 0) {
            captured = (char*)ray_sys_alloc((size_t)pos + 1);
            if (captured) {
                fseek(cap, 0, SEEK_SET);
                size_t got = fread(captured, 1, (size_t)pos, cap);
                captured[got] = '\0';
                cap_len = (int)got;
            }
        }
    }
    if (saved_out >= 0) close(saved_out);
    if (saved_err >= 0) close(saved_err);
    if (cap) fclose(cap);

    /* Build [captured_str, result] list.  If the result is the
     * "no result" sentinel (RAY_NULL_OBJ), keep it as-is; the
     * client can distinguish via the second element's type. */
    ray_t* cap_str = ray_str(captured ? captured : "", (size_t)cap_len);
    if (captured) ray_sys_free(captured);
    if (!cap_str) {
        /* Allocator pressure on a tiny string is unlikely but possible. */
        return result;
    }

    ray_t* response = ray_list_new(2);
    if (!response) {
        ray_release(cap_str);
        return result;
    }
    ray_list_append(response, cap_str);
    ray_list_append(response, result);
    /* ray_list_append takes its own ref; release ours. */
    ray_release(cap_str);
    if (result != RAY_NULL_OBJ) ray_release(result);
    return response;
}

/* ======================================================================
 * Poll-based IPC (new API)
 * ====================================================================== */

/* Per-connection state stored in selector->data */
typedef struct {
    ray_ipc_header_t hdr;
    uint8_t          phase;
    int64_t          listener_id;  /* id of the listener selector */
    bool             auth_required;  /* server has -u/-U */
    bool             restricted;     /* server has -U */
} ray_ipc_conn_data_t;

static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_creds(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_header(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel);
static void   ipc_on_close(ray_poll_t* poll, ray_selector_t* sel);

/* Wrappers matching ray_io_fn signature for socket recv */
static int64_t ipc_recv_fn(int64_t fd, uint8_t* buf, int64_t len) {
    return ray_sock_recv((ray_sock_t)fd, buf, (size_t)len);
}

/* Accept callback — called when listener fd is readable */
static ray_t* ipc_accept(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_sock_t new_fd = ray_sock_accept((ray_sock_t)sel->fd);
    if (new_fd == RAY_INVALID_SOCK) return NULL;
    ray_sock_set_nonblocking(new_fd);

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(new_fd); return NULL; }
    memset(cd, 0, sizeof(*cd));
    cd->phase = RAY_IPC_PHASE_HANDSHAKE;
    cd->listener_id = sel->id;
    cd->auth_required = (poll->auth_secret[0] != '\0');
    cd->restricted    = poll->restricted;

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)new_fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.read_fn  = ipc_read_handshake;
    reg.close_fn = ipc_on_close;
    reg.data     = cd;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(new_fd);
        ray_sys_free(cd);
        return NULL;
    }

    /* Request 2 bytes for handshake */
    ray_selector_t* ns = ray_poll_get(poll, id);
    if (ns) ray_poll_rx_request(poll, ns, 2);

    return NULL;
}

static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 2) return NULL;
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    /* Refuse peers speaking a different wire version BEFORE we commit to
     * exchanging any serialized payloads.  Without this check a new
     * server would happily send v3-layout values to a v2 client, which
     * would misparse every atom after the version-bump byte. */
    if (sel->rx.buf->data[0] != RAY_SERDE_WIRE_VERSION) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    /* Send handshake response: version + auth_required flag */
    uint8_t resp[2] = { RAY_SERDE_WIRE_VERSION, cd->auth_required ? 0x01 : 0x00 };
    ray_sock_send((ray_sock_t)sel->fd, resp, 2);

    if (cd->auth_required) {
        cd->phase = RAY_IPC_PHASE_HANDSHAKE;
        sel->rx.read_fn = ipc_read_creds;
        ray_poll_rx_request(poll, sel, 1);  /* length byte first */
        return NULL;
    }

    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));
    return NULL;
}

static ray_t* ipc_read_creds(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 1) return NULL;
    uint8_t cred_len = sel->rx.buf->data[0];

    /* The handshake first asks for 1 byte (the cred_len prefix); after
     * reading it we need to grow the rx buffer to 1 + cred_len without
     * losing the byte we already have.  ray_poll_rx_request resets the
     * buffer when it grows, so do the grow in-place here. */
    int64_t need = 1 + (int64_t)cred_len;
    if (sel->rx.buf->size < need) {
        ray_poll_buf_t* old = sel->rx.buf;
        ray_poll_buf_t* nb  = ray_poll_buf_new(need);
        if (!nb) { ray_poll_deregister(poll, sel->id); return NULL; }
        nb->data[0] = cred_len;
        nb->offset  = 1;
        nb->size    = need;
        ray_poll_buf_free(old);
        sel->rx.buf = nb;
        return NULL;
    }
    if (sel->rx.buf->offset < need) {
        sel->rx.buf->size = need;
        return NULL;
    }

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    bool ok = validate_creds(sel->rx.buf->data + 1, cred_len,
                             poll->auth_secret);
    uint8_t result = ok ? 0x00 : 0x01;
    ray_sock_send((ray_sock_t)sel->fd, &result, 1);

    if (!ok) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));
    return NULL;
}

static ray_t* ipc_read_header(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf ||
        sel->rx.buf->offset < (int64_t)sizeof(ray_ipc_header_t))
        return NULL;

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    memcpy(&cd->hdr, sel->rx.buf->data, sizeof(ray_ipc_header_t));

    if (cd->hdr.prefix != RAY_SERDE_PREFIX ||
        cd->hdr.version != RAY_SERDE_WIRE_VERSION ||
        cd->hdr.size <= 0 ||
        cd->hdr.size > 256 * 1024 * 1024) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    cd->phase = RAY_IPC_PHASE_PAYLOAD;
    sel->rx.read_fn = ipc_read_payload;
    ray_poll_rx_request(poll, sel, cd->hdr.size);

    return NULL;
}

static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    if (!sel->rx.buf || sel->rx.buf->offset < cd->hdr.size)
        return NULL;

    bool prev_restricted = ray_eval_get_restricted();
    ray_eval_set_restricted(cd->restricted);

    /* Eval and produce result */
    ray_t* result = eval_payload(sel->rx.buf->data,
                                 (size_t)sel->rx.buf->offset, &cd->hdr);

    ray_eval_set_restricted(prev_restricted);

    /* Send response for sync messages */
    if (cd->hdr.msgtype == RAY_IPC_MSG_SYNC)
        send_response((ray_sock_t)sel->fd, result);
    if (result != RAY_NULL_OBJ) ray_release(result);

    /* Reset for next message */
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));

    return NULL;
}

static void ipc_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll;
    if (sel->data) {
        ray_sys_free(sel->data);
        sel->data = NULL;
    }
    ray_sock_close((ray_sock_t)sel->fd);
}

int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port)
{
    if (!poll) return -1;

    ray_sock_t fd = ray_sock_listen(port);
    if (fd == RAY_INVALID_SOCK) return -1;
    ray_sock_set_nonblocking(fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.read_fn  = ipc_accept;
    reg.close_fn = ipc_on_close;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(fd);
        return -1;
    }
    return id;
}

/* ======================================================================
 * Server API
 * ====================================================================== */

static void conn_close(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
#if defined(__linux__)
    epoll_ctl(srv->poll_fd, EPOLL_CTL_DEL, c->fd, NULL);
#elif defined(__APPLE__)
    struct kevent kev;
    EV_SET(&kev, c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    (void)srv;
#endif

    ray_sock_close(c->fd);
    if (c->rx_buf) ray_sys_free(c->rx_buf);
    c->fd      = RAY_INVALID_SOCK;
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = 0;

    uint32_t idx = (uint32_t)(c - srv->conns);
    if (idx + 1 < srv->n_conns)
        srv->conns[idx] = srv->conns[srv->n_conns - 1];
    if (srv->n_conns > 0) srv->n_conns--;
}

static void conn_on_handshake(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* Refuse peers speaking a different wire version up front — see the
     * matching check in ipc_read_handshake. */
    if (!c->rx_buf || c->rx_buf[0] != RAY_SERDE_WIRE_VERSION) {
        conn_close(srv, c);
        return;
    }

    bool auth_req = (srv->auth_secret[0] != '\0');
    uint8_t resp[2] = { RAY_SERDE_WIRE_VERSION, auth_req ? 0x01 : 0x00 };
    ray_sock_send(c->fd, resp, 2);

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;

    if (auth_req) {
        c->rx_need = 1; /* length byte */
        c->phase   = RAY_IPC_PHASE_CREDS;
        return;
    }

    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_header(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    memcpy(&c->hdr, c->rx_buf, sizeof(ray_ipc_header_t));

    if (c->hdr.prefix != RAY_SERDE_PREFIX) { conn_close(srv, c); return; }
    if (c->hdr.version != RAY_SERDE_WIRE_VERSION) { conn_close(srv, c); return; }
    if (c->hdr.size <= 0)                  { conn_close(srv, c); return; }
    if (c->hdr.size > 256 * 1024 * 1024)   { conn_close(srv, c); return; }

    ray_sys_free(c->rx_buf);
    c->rx_buf = (uint8_t*)ray_sys_alloc((size_t)c->hdr.size);
    if (!c->rx_buf) { conn_close(srv, c); return; }
    c->rx_len  = 0;
    c->rx_need = (size_t)c->hdr.size;
    c->phase   = RAY_IPC_PHASE_PAYLOAD;
}

static void conn_on_payload(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    bool prev = ray_eval_get_restricted();
    ray_eval_set_restricted(srv->restricted);

    ray_t* result = eval_payload(c->rx_buf, c->rx_len, &c->hdr);

    ray_eval_set_restricted(prev);

    if (c->hdr.msgtype == RAY_IPC_MSG_SYNC)
        send_response(c->fd, result);
    if (result != RAY_NULL_OBJ) ray_release(result);

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_creds(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    if (c->rx_len == 1) {
        /* Got length byte — reallocate buffer for full credential */
        uint8_t cred_len = c->rx_buf[0];
        size_t need = 1 + (size_t)cred_len;
        uint8_t* newbuf = (uint8_t*)ray_sys_alloc(need);
        if (!newbuf) { conn_close(srv, c); return; }
        newbuf[0] = cred_len;
        ray_sys_free(c->rx_buf);
        c->rx_buf  = newbuf;
        c->rx_need = need;
        return;
    }

    uint8_t cred_len = c->rx_buf[0];
    bool ok = validate_creds(c->rx_buf + 1, cred_len, srv->auth_secret);

    uint8_t result = ok ? 0x00 : 0x01;
    ray_sock_send(c->fd, &result, 1);

    if (!ok) {
        conn_close(srv, c);
        return;
    }

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_readable(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    if (!c->rx_buf) {
        c->rx_buf = (uint8_t*)ray_sys_alloc(c->rx_need);
        if (!c->rx_buf) { conn_close(srv, c); return; }
    }

    int64_t n = ray_sock_recv(c->fd, c->rx_buf + c->rx_len,
                              c->rx_need - c->rx_len);
    if (n <= 0) { conn_close(srv, c); return; }
    c->rx_len += (size_t)n;

    if (c->rx_len < c->rx_need) return;

    switch (c->phase) {
    case RAY_IPC_PHASE_HANDSHAKE: conn_on_handshake(srv, c); break;
    case RAY_IPC_PHASE_CREDS:     conn_on_creds(srv, c);     break;
    case RAY_IPC_PHASE_HEADER:    conn_on_header(srv, c);    break;
    case RAY_IPC_PHASE_PAYLOAD:   conn_on_payload(srv, c);   break;
    }
}

ray_err_t ray_ipc_server_init(ray_ipc_server_t* srv, uint16_t port)
{
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = ray_sock_listen(port);
    if (srv->listen_fd == RAY_INVALID_SOCK) return RAY_ERR_IO;
    ray_sock_set_nonblocking(srv->listen_fd);

#if defined(__linux__)
    srv->poll_fd = epoll_create1(0);
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#elif defined(__APPLE__)
    srv->poll_fd = kqueue();
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct kevent kev;
    EV_SET(&kev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    srv->poll_fd = -1;
#endif

    srv->running = true;
    return RAY_OK;
}

void ray_ipc_server_destroy(ray_ipc_server_t* srv)
{
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        ray_ipc_conn_t* c = &srv->conns[i];
        if (c->fd != RAY_INVALID_SOCK) {
            if (c->rx_buf) ray_sys_free(c->rx_buf);
            ray_sock_close(c->fd);
        }
    }
    srv->n_conns = 0;

    ray_sock_close(srv->listen_fd);
    srv->listen_fd = RAY_INVALID_SOCK;

    if (srv->poll_fd >= 0) {
#ifndef RAY_OS_WINDOWS
        close(srv->poll_fd);
#endif
    }
    srv->poll_fd = -1;
    srv->running = false;
}

int ray_ipc_poll(ray_ipc_server_t* srv, int timeout_ms)
{
    int ready = 0;

#if defined(__linux__)
    struct epoll_event events[RAY_IPC_MAX_EVENTS];
    int nfds = epoll_wait(srv->poll_fd, events, RAY_IPC_MAX_EVENTS, timeout_ms);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;

        if (fd == srv->listen_fd) {
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            c->fd      = new_fd;
            c->rx_buf  = NULL;
            c->rx_len  = 0;
            c->rx_need = 2;
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct epoll_event cev = { .events = EPOLLIN, .data.fd = new_fd };
            epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, new_fd, &cev);
        } else {
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;
        }
    }

#elif defined(__APPLE__)
    struct kevent events[RAY_IPC_MAX_EVENTS];
    struct timespec ts;
    struct timespec* tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int nfds = kevent(srv->poll_fd, NULL, 0, events, RAY_IPC_MAX_EVENTS, tsp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = (int)events[i].ident;

        if (fd == srv->listen_fd) {
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            c->fd      = new_fd;
            c->rx_buf  = NULL;
            c->rx_len  = 0;
            c->rx_need = 2;
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct kevent kev;
            EV_SET(&kev, new_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
            kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
        } else {
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;
        }
    }

#else  /* Windows: select-based fallback */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv->listen_fd, &rfds);
    ray_sock_t maxfd = srv->listen_fd;
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        FD_SET(srv->conns[i].fd, &rfds);
        if (srv->conns[i].fd > maxfd) maxfd = srv->conns[i].fd;
    }

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int nfds = select((int)(maxfd + 1), &rfds, NULL, NULL, tvp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    if (FD_ISSET(srv->listen_fd, &rfds)) {
        ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
        if (new_fd != RAY_INVALID_SOCK) {
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
            } else {
                ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
                c->fd      = new_fd;
                c->rx_buf  = NULL;
                c->rx_len  = 0;
                c->rx_need = 2;
                c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            }
        }
    }

    for (uint32_t i = srv->n_conns; i > 0; ) {
        --i;
        if (srv->conns[i].fd != RAY_INVALID_SOCK && FD_ISSET(srv->conns[i].fd, &rfds))
            conn_on_readable(srv, &srv->conns[i]);
    }
#endif

    return ready;
}

/* ===== Client API ===== */

static ray_sock_t g_client_fds[RAY_IPC_MAX_CONNS];
static int        g_client_count = 0;
static bool       g_client_init = false;

static void client_init(void) {
    if (g_client_init) return;
    for (int i = 0; i < RAY_IPC_MAX_CONNS; i++)
        g_client_fds[i] = RAY_INVALID_SOCK;
    g_client_init = true;
}

static int64_t recv_full(ray_sock_t fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int64_t n = ray_sock_recv(fd, (uint8_t*)buf + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return (int64_t)total;
}

static int64_t client_send_msg(int64_t handle, ray_t* msg, uint8_t msgtype,
                               uint8_t extra_flags)
{
    if (handle < 0 || handle >= RAY_IPC_MAX_CONNS) return -2;
    ray_sock_t fd = g_client_fds[handle];
    if (fd == RAY_INVALID_SOCK) return -2;

    int64_t ser_size = ray_serde_size(msg);
    if (ser_size <= 0) return -1;

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)ser_size);
    if (!payload) return -1;
    ray_ser_raw(payload, msg);

    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > RAY_IPC_COMPRESS_THRESHOLD) {
        uint8_t* comp = (uint8_t*)ray_sys_alloc((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress(payload, (size_t)ser_size,
                                           comp, (size_t)ser_size);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_sys_alloc(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_sys_free(comp);
        }
    }

    if (!send_buf) {
        send_buf = payload;
        send_len = (size_t)ser_size;
        payload  = NULL;
    }

    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_SERDE_WIRE_VERSION,
        .flags   = (uint8_t)(flags | extra_flags),
        .endian  = 0,
        .msgtype = msgtype,
        .size    = (int64_t)send_len,
    };

    int64_t rc = ray_sock_send(fd, &hdr, sizeof(hdr));
    if (rc < 0) { ray_sys_free(send_buf); if (payload) ray_sys_free(payload); return -1; }
    rc = ray_sock_send(fd, send_buf, send_len);

    ray_sys_free(send_buf);
    if (payload) ray_sys_free(payload);
    return rc < 0 ? -1 : 0;
}

int64_t ray_ipc_connect(const char* host, uint16_t port,
                         const char* user, const char* password)
{
    client_init();

    ray_sock_t fd = ray_sock_connect(host, port, 5000);
    if (fd == RAY_INVALID_SOCK) return -1;

    uint8_t hs[2] = { RAY_SERDE_WIRE_VERSION, 0x00 };
    if (ray_sock_send(fd, hs, 2) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    uint8_t resp[2];
    if (recv_full(fd, resp, 2) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    /* Refuse a peer that speaks a different wire version.  This gives
     * the new client an explicit error at connect time rather than
     * silently sending a v3 payload to a server that would misparse
     * every atom. */
    if (resp[0] != RAY_SERDE_WIRE_VERSION) {
        ray_sock_close(fd);
        return -4; /* wire version mismatch */
    }

    /* Auth required? */
    if (resp[1] == 0x01) {
        if (!password) {
            ray_sock_close(fd);
            return -2; /* auth required but no creds */
        }
        char cred[256];
        int cred_len;
        if (user && user[0])
            cred_len = snprintf(cred, sizeof(cred), "%s:%s", user, password);
        else
            cred_len = snprintf(cred, sizeof(cred), ":%s", password);
        if (cred_len < 0 || cred_len >= (int)sizeof(cred)) {
            ray_sock_close(fd);
            return -1;
        }
        cred_len++; /* include null terminator */
        uint8_t len_byte = (uint8_t)cred_len;
        if (ray_sock_send(fd, &len_byte, 1) < 0 ||
            ray_sock_send(fd, cred, cred_len) < 0) {
            ray_sock_close(fd);
            return -1;
        }
        uint8_t auth_result;
        if (recv_full(fd, &auth_result, 1) < 0 || auth_result != 0x00) {
            ray_sock_close(fd);
            return -3; /* auth rejected */
        }
    } else if (resp[1] != 0x00) {
        ray_sock_close(fd);
        return -1;
    }

#ifdef RAY_OS_WINDOWS
    { DWORD z = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&z, sizeof(z)); }
#else
    { struct timeval z = {0, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z)); }
#endif

    for (int i = 0; i < RAY_IPC_MAX_CONNS; i++) {
        if (g_client_fds[i] == RAY_INVALID_SOCK) {
            g_client_fds[i] = fd;
            if (i >= g_client_count) g_client_count = i + 1;
            return (int64_t)i;
        }
    }

    ray_sock_close(fd);
    return -1;
}

void ray_ipc_close(int64_t handle)
{
    if (handle < 0 || handle >= RAY_IPC_MAX_CONNS) return;
    if (g_client_fds[handle] == RAY_INVALID_SOCK) return;
    ray_sock_close(g_client_fds[handle]);
    g_client_fds[handle] = RAY_INVALID_SOCK;
}

ray_t* ray_ipc_send(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        msg = ray_lazy_materialize(msg);
        if (RAY_IS_ERR(msg)) return msg;
        owned = true;
    }
    { int64_t sr = client_send_msg(handle, msg, RAY_IPC_MSG_SYNC, 0);
      if (sr == -2) { if (owned) ray_release(msg); return ray_error("io", "connection closed"); }
      if (sr < 0)  { if (owned) ray_release(msg); return ray_error("io", "ipc send failed"); } }

    ray_sock_t fd = g_client_fds[handle];

    ray_ipc_header_t hdr;
    if (recv_full(fd, &hdr, sizeof(hdr)) < 0) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc recv header failed");
    }
    if (hdr.prefix != RAY_SERDE_PREFIX || hdr.size <= 0) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc bad response header");
    }
    if (hdr.version != RAY_SERDE_WIRE_VERSION) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("version", "ipc peer wire version mismatch");
    }
    if (hdr.size > 256 * 1024 * 1024) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc response too large");
    }

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
    if (!payload) { if (owned) ray_release(msg); return ray_error("oom", NULL); }
    if (recv_full(fd, payload, (size_t)hdr.size) < 0) {
        ray_sys_free(payload);
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc recv payload failed");
    }

    uint8_t* deser_buf     = payload;
    size_t   deser_len     = (size_t)hdr.size;
    uint8_t* decompressed  = NULL;

    if (hdr.flags & RAY_IPC_FLAG_COMPRESSED) {
        if (deser_len < 4) { ray_sys_free(payload); if (owned) ray_release(msg); return ray_error("io", "ipc compressed payload too short"); }
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        decompressed = (uint8_t*)ray_sys_alloc(uncomp_size);
        if (!decompressed) { ray_sys_free(payload); if (owned) ray_release(msg); return ray_error("oom", NULL); }
        size_t dlen = ray_ipc_decompress(payload + 4, deser_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_sys_free(decompressed);
            ray_sys_free(payload);
            if (owned) ray_release(msg);
            return ray_error("io", "ipc decompress failed");
        }
        deser_buf = decompressed;
        deser_len = uncomp_size;
    }

    int64_t de_len = (int64_t)deser_len;
    ray_t*  result = ray_de_raw(deser_buf, &de_len);

    if (decompressed) ray_sys_free(decompressed);
    ray_sys_free(payload);
    if (owned) ray_release(msg);

    return result ? result : RAY_NULL_OBJ;
}

ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        msg = ray_lazy_materialize(msg);
        if (RAY_IS_ERR(msg)) {
            ray_err_t code = ray_err_from_obj(msg);
            ray_error_free(msg);
            return code;
        }
        owned = true;
    }
    ray_err_t rc = (client_send_msg(handle, msg, RAY_IPC_MSG_ASYNC, 0) < 0)
                   ? RAY_ERR_IO : RAY_OK;
    if (owned) ray_release(msg);
    return rc;
}

/* Verbose-eval client send: sets RAY_IPC_FLAG_VERBOSE on the
 * outbound header so the server captures whatever the eval writes
 * to stdout/stderr and returns a 2-element list [captured_str,
 * result] instead of bare result.  Same wire path as ray_ipc_send
 * otherwise — sync, blocking until response. */
ray_t* ray_ipc_send_verbose(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        msg = ray_lazy_materialize(msg);
        if (RAY_IS_ERR(msg)) return msg;
        owned = true;
    }
    { int64_t sr = client_send_msg(handle, msg, RAY_IPC_MSG_SYNC,
                                   RAY_IPC_FLAG_VERBOSE);
      if (sr == -2) { if (owned) ray_release(msg); return ray_error("io", "connection closed"); }
      if (sr < 0)  { if (owned) ray_release(msg); return ray_error("io", "ipc send failed"); } }

    ray_sock_t fd = g_client_fds[handle];

    ray_ipc_header_t hdr;
    if (recv_full(fd, &hdr, sizeof(hdr)) < 0) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc recv header failed");
    }
    if (hdr.prefix != RAY_SERDE_PREFIX || hdr.size <= 0) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc bad response header");
    }
    if (hdr.version != RAY_SERDE_WIRE_VERSION) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("version", "ipc peer wire version mismatch");
    }
    if (hdr.size > 256 * 1024 * 1024) {
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc response too large");
    }

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
    if (!payload) { if (owned) ray_release(msg); return ray_error("oom", NULL); }
    if (recv_full(fd, payload, (size_t)hdr.size) < 0) {
        ray_sys_free(payload);
        ray_ipc_close(handle);
        if (owned) ray_release(msg);
        return ray_error("io", "ipc recv payload failed");
    }

    uint8_t* deser_buf    = payload;
    size_t   deser_len    = (size_t)hdr.size;
    uint8_t* decompressed = NULL;

    if (hdr.flags & RAY_IPC_FLAG_COMPRESSED) {
        if (deser_len < 4) { ray_sys_free(payload); if (owned) ray_release(msg); return ray_error("io", "ipc compressed payload too short"); }
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        decompressed = (uint8_t*)ray_sys_alloc(uncomp_size);
        if (!decompressed) { ray_sys_free(payload); if (owned) ray_release(msg); return ray_error("oom", NULL); }
        size_t dlen = ray_ipc_decompress(payload + 4, deser_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_sys_free(decompressed);
            ray_sys_free(payload);
            if (owned) ray_release(msg);
            return ray_error("io", "ipc decompress failed");
        }
        deser_buf = decompressed;
        deser_len = uncomp_size;
    }

    int64_t de_len = (int64_t)deser_len;
    ray_t*  result = ray_de_raw(deser_buf, &de_len);

    if (decompressed) ray_sys_free(decompressed);
    ray_sys_free(payload);
    if (owned) ray_release(msg);

    return result ? result : RAY_NULL_OBJ;
}

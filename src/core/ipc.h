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

#ifndef RAY_IPC_H
#define RAY_IPC_H

#include <rayforce.h>
#include "core/poll.h"
#include "core/sock.h"
#include "store/serde.h"

/* ===== Compression ===== */

#define RAY_IPC_COMPRESS_THRESHOLD 2000

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap);
size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len);

/* ===== Message types ===== */

#define RAY_IPC_MSG_ASYNC  0
#define RAY_IPC_MSG_SYNC   1
#define RAY_IPC_MSG_RESP   2

#define RAY_IPC_FLAG_COMPRESSED 0x01
/* Set by the journal hook in core/ipc.c eval_payload when the inbound
 * IPC message arrived on a `-U` restricted connection.  Used ONLY for
 * persisted log frames; the live IPC path ignores it (the connection's
 * restricted state is the source of truth there).  Replay reads the
 * bit to re-impose the original sender's restrictions, otherwise a
 * crash + restart silently elevates restricted commands to full
 * privilege. */
#define RAY_IPC_FLAG_RESTRICTED 0x02
/* Set by clients that want a "verbose" eval response: the server
 * captures whatever the eval wrote to stdout/stderr and returns a
 * 2-element list [captured_str, result] instead of bare result.
 * Used by the remote-REPL client wrapper so a `(println x)` from
 * the connected client produces output on the *client's* terminal,
 * not on the server's.  Default sync messages still get the bare
 * result, so existing IPC clients are unaffected. */
#define RAY_IPC_FLAG_VERBOSE    0x04
#define RAY_IPC_MAX_CONNS 256

/* ===== Connection hooks (.ipc.on.*) ===== */

/* Current connection handle, readable from Rayfall via the `.ipc.handle`
 * builtin while a `.ipc.on.*` hook is on the stack.  Set/restored by the
 * server around every hook invocation; defaults to -1 outside any hook.
 * Thread-local — IPC dispatch is single-threaded today, but the storage
 * class keeps the value scoped to the dispatch thread should that ever
 * change. */
int64_t ray_ipc_current_handle(void);
ray_poll_t* ray_ipc_active_poll(void);

/* ===== Poll-based IPC (new API) ===== */

/* Register IPC listener on poll. Returns selector id or -1. */
int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port);
/* Bind to a specific IPv4 address; host NULL/empty means INADDR_ANY (#427). */
int64_t ray_ipc_listen_at(ray_poll_t* poll, const char* host, uint16_t port);

/* ===== Legacy server API (wraps poll internally for tests) ===== */

typedef struct ray_ipc_conn {
    ray_sock_t        fd;
    uint8_t*          rx_buf;
    size_t            rx_len;
    size_t            rx_need;
    uint8_t           phase;
    ray_ipc_header_t  hdr;
} ray_ipc_conn_t;

typedef struct ray_ipc_server {
    ray_sock_t        listen_fd;
    int               poll_fd;
    ray_ipc_conn_t    conns[RAY_IPC_MAX_CONNS];
    uint32_t          n_conns;
    bool              running;
    char              auth_secret[256]; /* password from -u/-U */
    bool              restricted;       /* -U mode */
} ray_ipc_server_t;

ray_err_t ray_ipc_server_init(ray_ipc_server_t* srv, uint16_t port);
ray_err_t ray_ipc_server_init_at(ray_ipc_server_t* srv, const char* host, uint16_t port);
void      ray_ipc_server_destroy(ray_ipc_server_t* srv);
int       ray_ipc_poll(ray_ipc_server_t* srv, int timeout_ms);

/* ===== Connection-handle API =====
 *
 * One handle namespace: a handle is the poll selector id of an IPC
 * connection, inbound or outbound.  ray_ipc_connect registers the new
 * connection in the active poll (the poll dispatching the current
 * hook/eval, else the runtime's main poll) and returns its selector
 * id; send/close resolve handles the same way, so server-side hooks
 * can write back through the handles they receive.  Sync sends pump
 * the connection's rx machinery while waiting, dispatching any
 * interleaved async/sync frames from the peer. */

/* timeout_ms > 0 bounds the TCP connect and the handshake I/O; <= 0 uses
 * the default budget.  Returns the handle (>= 0) or a negative code:
 * -1 refused/error, -2 auth required, -3 auth failed, -4 wire mismatch,
 * -5 connect timed out. */
int64_t   ray_ipc_connect(const char* host, uint16_t port,
                           const char* user, const char* password,
                           int timeout_ms);
void      ray_ipc_close(int64_t handle);
ray_t*    ray_ipc_send(int64_t handle, ray_t* msg);
ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg);

/* Remote-REPL helper: send a SYNC message with RAY_IPC_FLAG_VERBOSE
 * set, returning a 2-element list [captured_str, result] where
 * captured_str is whatever the server's eval wrote to stdout/stderr
 * (combined) while running this request.  Used by the `-h` client
 * loop in main.c so output produced by the remote eval is shown on
 * the local user's terminal, not lost on the server. */
ray_t*    ray_ipc_send_verbose(int64_t handle, ray_t* msg);

#endif /* RAY_IPC_H */

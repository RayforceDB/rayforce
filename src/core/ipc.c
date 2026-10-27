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

#include "store/enccol.h"    /* ray_enccol_cache_trim after a request */
#include "core/ipc.h"
#include "core/mcast.h"
#include "core/timer.h"
#include "core/shm.h"
#include "core/rdma.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "ops/ops.h"
#include "store/journal.h"
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <io.h>      /* dup, dup2, close */
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
#endif

#if defined(__linux__)
  #include <sys/epoll.h>
  #include <sys/un.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <time.h>
  #define RAY_IPC_MAX_EVENTS 64
  /* The transports live here: memfd and eventfd for the rings, abstract
   * Unix sockets to hand them over, verbs for remote memory. */
  #define RAY_IPC_XPORT 1
#elif defined(__APPLE__)
  #include <sys/event.h>
  #define RAY_IPC_MAX_EVENTS 64
#endif

#include "core/runtime.h"
#include "lang/eval.h"
#include "lang/format.h"   /* ray_fmt — query text for the stats log */
#include "core/qlog.h"     /* query-statistics ring */
#include "lang/env.h"
#include "lang/internal.h"
#include "table/sym.h"

static void mark_ipc_literal_fallbacks(ray_t* obj) {
    if (!obj || RAY_IS_ERR(obj) || obj->type != RAY_LIST)
        return;

    obj->attrs |= RAY_EVAL_LITERAL_FALLBACK;
    ray_t** elems = (ray_t**)ray_data(obj);
    for (int64_t i = 0; i < ray_len(obj); i++)
        mark_ipc_literal_fallbacks(elems[i]);
}

/* ===== Compression (delta + RLE) ===== */

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap)
{
    return ray_ipc_compress_at(src, len, dst, dst_cap,
                               (size_t)RAY_IPC_COMPRESS_THRESHOLD);
}

size_t ray_ipc_link_threshold(ray_sock_t fd)
{
    return ray_sock_peer_is_local(fd) ? RAY_IPC_COMPRESS_NEVER
                                      : (size_t)RAY_IPC_COMPRESS_THRESHOLD;
}

size_t ray_ipc_compress_at(const uint8_t* src, size_t len,
                           uint8_t* dst, size_t dst_cap, size_t threshold)
{
    /* RAY_IPC_COMPRESS_NEVER is SIZE_MAX, so this rejects every length. */
    if (len <= threshold) return 0;

    /* Step 1: delta-encode into temporary buffer */
    uint8_t* delta = (uint8_t*)ray_alloc_raw(len);
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
            if (di + 2 > dst_cap) { ray_free_raw(delta); return 0; }
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
            if (di + 1 + llen > dst_cap) { ray_free_raw(delta); return 0; }
            dst[di++] = (uint8_t)(-(int8_t)llen);
            memcpy(dst + di, delta + start, llen);
            di += llen;
        }
    }

    ray_free_raw(delta);
    if (di >= len) return 0;
    return di;
}

size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len)
{
    uint8_t* decoded = (uint8_t*)ray_alloc_raw(dst_len);
    if (!decoded) return 0;

    size_t si = 0;
    size_t di = 0;

    while (si < clen && di < dst_len) {
        int8_t count = (int8_t)src[si++];
        if (count > 0) {
            if (si >= clen) { ray_free_raw(decoded); return 0; }
            uint8_t val = src[si++];
            size_t n = (size_t)count;
            if (di + n > dst_len) { ray_free_raw(decoded); return 0; }
            memset(decoded + di, val, n);
            di += n;
        } else {
            size_t n = (size_t)(-(int)count);
            if (si + n > clen || di + n > dst_len) {
                ray_free_raw(decoded);
                return 0;
            }
            memcpy(decoded + di, src + si, n);
            si += n;
            di += n;
        }
    }

    /* Un-delta */
    if (di == 0) { ray_free_raw(decoded); return 0; }
    dst[0] = decoded[0];
    for (size_t i = 1; i < di; i++)
        dst[i] = (uint8_t)(decoded[i] + dst[i - 1]);

    ray_free_raw(decoded);
    return di;
}

/* ===== Shared protocol helpers ===== */

#define RAY_IPC_PHASE_HANDSHAKE 0
#define RAY_IPC_PHASE_HEADER    1
#define RAY_IPC_PHASE_PAYLOAD   2
#define RAY_IPC_PHASE_CREDS     3
/* Hard cap on a single wire frame; the backlog a connection may hold is
 * the separately configurable transmit limit (ray_ipc_tx_limit_set). */
#define RAY_IPC_FRAME_MAX_BYTES (256LL * 1024LL * 1024LL)

static int64_t g_tx_limit_bytes  = RAY_IPC_TX_DEFAULT_BYTES;
static int64_t g_tx_limit_frames = 0;   /* unlimited */

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

/* ===== Connection hooks (.ipc.on.*) =====
 *
 * Five user-settable lambdas that intercept the connection lifecycle.
 * Lookup is by interned sym id; we cache the ids in `hook_syms[]` so the
 * fast path is a single ray_env_get + RAY_LAMBDA-type check per dispatch.
 *
 * `__VM->ipc_handle` is the per-thread value `.ipc.handle` reads back
 * to Rayfall.  Each dispatch saves/restores it around the invocation,
 * so a hook that opens its own connection (calling `.ipc.handle` inside
 * a nested `.ipc.send` round-trip) still sees the outer handle when its
 * body resumes.  Default value -1 means "no hook is currently on the
 * stack" — exposed verbatim through the builtin.
 *
 * Errors:
 *   - on.open / on.close / on.async: logged to stderr, swallowed.
 *   - on.sync: error becomes the response (same as a raw `eval` error).
 *   - on.auth: error treated as reject (handshake refused). */

/* Hook indices must match the order in `src/lang/env.c`
 * (g_ipc_hook_syms[]) — the `ray_sym_ipc_hook(idx)` getter assumes this
 * mapping.  Keep them in lockstep. */
enum {
    IPC_HOOK_OPEN  = 0,
    IPC_HOOK_CLOSE = 1,
    IPC_HOOK_SYNC  = 2,
    IPC_HOOK_ASYNC = 3,
    IPC_HOOK_AUTH  = 4,
    IPC_HOOK_COUNT = 5,
};

/* The IPC dispatch context — which connection's hook/eval is on this
 * thread's stack, and the poll it lives in — is per-thread state and
 * therefore lives on the VM (__VM->ipc_handle / __VM->ipc_poll, see
 * core/runtime.h).  Connection handles are selector ids, so resolving
 * one needs the poll: inside a hook / inbound eval that's the poll
 * that fired it; everywhere else (REPL, scripts) it's the runtime's
 * main poll. */

static int64_t ipc_ctx_handle(void) {
    return __VM ? __VM->ipc_handle : -1;
}

static ray_poll_t* ipc_ctx_poll(void) {
    return __VM ? (ray_poll_t*)__VM->ipc_poll : NULL;
}

static void ipc_ctx_set(int64_t handle, ray_poll_t* poll) {
    if (!__VM) return;
    __VM->ipc_handle = handle;
    __VM->ipc_poll   = poll;
}

static _Thread_local bool g_auto_journal_eval = false;

bool ray_ipc_auto_journal_eval(void) {
    return g_auto_journal_eval;
}

bool ray_ipc_set_auto_journal_eval(bool enabled) {
    bool prev = g_auto_journal_eval;
    g_auto_journal_eval = enabled;
    return prev;
}

#ifdef DEBUG
void ray_ipc_set_auto_journal_eval_for_test(bool enabled) {
    g_auto_journal_eval = enabled;
}
#endif

ray_poll_t* ray_ipc_active_poll(void) {
    ray_poll_t* p = ipc_ctx_poll();
    return p ? p : (ray_poll_t*)ray_runtime_get_poll();
}

ray_poll_t* ray_ipc_context_poll(void) {
    return ipc_ctx_poll();
}

int64_t ray_ipc_current_handle(void) {
    return ipc_ctx_handle();
}

/* Fetch the hook lambda if one is installed and is in fact a lambda.
 * Non-lambda bindings (cleared via `set .ipc.on.X 0` or never bound)
 * yield NULL — caller falls back to default behaviour.  Returns a
 * borrowed ref; do not release.  Sym IDs come from env.c's central
 * cache, so a runtime destroy/recreate cycle invalidates them in one
 * place and the lookup here always sees IDs from the current sym
 * table. */
static ray_t* hook_lookup(int idx) {
    int64_t sym = ray_sym_ipc_hook(idx);
    if (sym < 0) return NULL;
    ray_t* fn = ray_env_get(sym);
    if (!fn || fn->type != RAY_LAMBDA) return NULL;
    return fn;
}

/* ── Temporary VM for a hook on a VM-less thread (#569) ──────────────
 *
 * `__VM` is bound only by ray_runtime_create, so any thread an embedder
 * drives IPC from — the FFI teardown path in particular — has none.  #565
 * made the *no hook installed* case safe by letting ray_env_get fall
 * through to globals, which left this question open: with a hook actually
 * bound, call_fn1 would run user code with __VM == NULL.
 *
 * A hook is user code and should behave the same wherever the event came
 * from, so bind a VM for the duration rather than skipping the hook
 * (silent, and invisible at the call site) or failing the close (which
 * would break the FFI teardown that runtime.c:50-54 explicitly supports).
 *
 * Binding also restores `.ipc.handle` inside the hook: ipc_ctx_set stores
 * through __VM and returns early without one, so on a VM-less thread the
 * hook would otherwise see a handle of -1.
 *
 * ray_vm_t is ~68 KB, too large for a teardown-path stack, so it comes
 * from the buddy heap (process-wide and already live — the heap belongs to
 * the runtime, not the thread).  Teardown mirrors ray_runtime_destroy:
 * release raise_val and trace, then free.  Grown scope frames are not
 * walked, matching that same teardown — a hook that returns normally has
 * already popped its scopes. */
typedef struct { ray_vm_t* owned; } hook_vm_t;

static bool hook_vm_bind(hook_vm_t* hv) {
    hv->owned = NULL;
    if (__VM) return true;                      /* already on a VM thread */
    ray_vm_t* vm = (ray_vm_t*)ray_alloc_raw(sizeof(ray_vm_t));
    if (!vm) {
        /* Skipping a hook the operator installed is worth a line, matching
         * how this file reports every other hook failure. */
        fprintf(stderr, "ipc: cannot bind a VM for hook dispatch (out of memory); hook skipped\n");
        return false;
    }
    ray_vm_init(vm, -1);                        /* id -1: not a pool VM */
    __VM = vm;
    hv->owned = vm;
    return true;
}

static void hook_vm_unbind(hook_vm_t* hv) {
    if (!hv->owned) return;
    if (hv->owned->raise_val) ray_release(hv->owned->raise_val);
    if (hv->owned->return_val) ray_release(hv->owned->return_val);
    if (hv->owned->trace) ray_release(hv->owned->trace);
    __VM = NULL;
    ray_free_raw(hv->owned);
    hv->owned = NULL;
}

/* Call a single-arg hook for lifecycle events (on.open / on.close).
 * Errors are logged and swallowed — a buggy logging hook must never
 * wedge connection teardown.  `poll` is the poll the connection lives
 * in, exposed thread-locally so the hook body can use the handle with
 * `.ipc.post` / `.ipc.send` / `.ipc.close`. */
static void hook_call_lifecycle(ray_poll_t* poll, int idx, int64_t handle) {
    ray_t* fn = hook_lookup(idx);
    if (!fn) return;
    hook_vm_t hv;
    if (!hook_vm_bind(&hv)) return;
    ray_t* arg = make_i64(handle);
    if (!arg || RAY_IS_ERR(arg)) {
        if (arg) ray_release(arg);
        hook_vm_unbind(&hv);
        return;
    }
    int64_t prev = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(handle, poll ? poll : prev_poll);
    ray_t* r = call_fn1(fn, arg);
    ipc_ctx_set(prev, prev_poll);
    if (r && RAY_IS_ERR(r)) {
        const char* name = (idx == IPC_HOOK_OPEN) ? ".ipc.on.open" : ".ipc.on.close";
        fprintf(stderr, "ipc: %s hook raised an error (handle=%lld)\n",
                name, (long long)handle);
    }
    ray_release(arg);
    if (r && r != RAY_NULL_OBJ) ray_release(r);
    hook_vm_unbind(&hv);
}

/* Call the on.auth hook with (user, pass) string atoms.  Returns:
 *  -  1 → hook ran and returned truthy; caller continues the handshake.
 *  -  0 → hook ran and returned falsy (or errored); caller rejects.
 *  - -1 → no hook installed; caller uses the existing pass-through.
 * The constant-time secret compare in validate_creds always runs first,
 * so this hook can only narrow access — never widen it. */
/* Not VM-bound, unlike hook_call_lifecycle: this runs only from inside
 * ray_poll_run, on a thread that necessarily has a VM.  If a future path
 * ever drives the handshake from an embedder's own thread, it needs the
 * same hook_vm_bind treatment (#569). */
static int hook_call_auth(ray_poll_t* poll, int64_t handle,
                          const uint8_t* cred_buf, uint8_t cred_len) {
    ray_t* fn = hook_lookup(IPC_HOOK_AUTH);
    if (!fn) return -1;

    /* Split user:pass exactly the way validate_creds does — colon-
     * separated, with the leading user part possibly empty.  Strip the
     * trailing NUL the client appends so the hook sees clean strings. */
    const char* creds = (const char*)cred_buf;
    const char* colon = memchr(creds, ':', cred_len);
    const char* upart = creds;
    size_t      ulen  = colon ? (size_t)(colon - creds) : 0;
    const char* ppart = colon ? colon + 1 : creds;
    size_t      plen  = colon ? (size_t)(cred_len - (ppart - creds))
                              : (size_t)cred_len;
    if (plen > 0 && ppart[plen - 1] == '\0') plen--;

    ray_t* u = ray_str(upart, ulen);
    ray_t* p = ray_str(ppart, plen);
    if (!u || !p || RAY_IS_ERR(u) || RAY_IS_ERR(p)) {
        if (u && !RAY_IS_ERR(u)) ray_release(u);
        if (p && !RAY_IS_ERR(p)) ray_release(p);
        return 0;  /* allocation failure → reject conservatively */
    }
    int64_t prev = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(handle, poll ? poll : prev_poll);
    ray_t* r = call_fn2(fn, u, p);
    ipc_ctx_set(prev, prev_poll);
    ray_release(u);
    ray_release(p);

    int ok;
    if (!r || RAY_IS_ERR(r)) {
        fprintf(stderr, "ipc: .ipc.on.auth hook raised an error — rejecting\n");
        ok = 0;
    } else {
        ok = is_truthy(r) ? 1 : 0;
    }
    if (r && r != RAY_NULL_OBJ) ray_release(r);
    return ok;
}

/* A partially-sent multicast frame lives on sel->tx.buf until EPOLLOUT/
 * EVFILT_WRITE drains it.  Any OTHER write to the same fd (a sync RESP, an
 * async push) must not splice its bytes into the middle of that half-sent
 * frame, or the peer parses the two framings into each other and the stream
 * for that connection is corrupt.  Flush the queued remainder first, blocking
 * on write-readiness the way ray_sock_send already does, so the direct write
 * that follows starts on a frame boundary.  Fast-path returns 0 when nothing
 * is queued (the common, non-subscriber case).  Returns -1 on socket error. */
#ifdef RAY_IPC_XPORT
/* The socket of an upgraded connection carries nothing but the peer's
 * going: readable, it has closed (or sent what it must not).  -3 then. */
static int conn_sock_gone(ray_selector_t* sel)
{
    char b;
    ssize_t n = recv((ray_sock_t)sel->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n >= 0) return -3;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -3;
}

static int64_t xport_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#define XPORT_SPIN_NS 20000
#endif

/* Block until the connection has something to read: its socket, or,
 * once upgraded, its transport — a short spin on the ring, then a sleep
 * on the transport's descriptor and the socket together.  Returns 1
 * readable, 0 timed out, -1 error, -2 interrupted by a signal, -3 the
 * peer is gone (the socket of an upgraded connection spoke). */
static int conn_wait_readable(ray_selector_t* sel, int wait_ms)
{
    ray_xport_t* x = sel->xport;
    if (!x) return ray_sock_wait_readable_intr((ray_sock_t)sel->fd, wait_ms);
#ifdef RAY_IPC_XPORT
    if (x->ops->pending(x)) return 1;
    int64_t until = xport_now_ns() + XPORT_SPIN_NS;
    do {
        if (x->ops->pending(x)) return 1;
    } while (xport_now_ns() < until);
    if (!x->ops->prepare_sleep(x, true, false)) return 1;
    struct pollfd p[2] = { { x->event_fd, POLLIN, 0 }, { (int)sel->fd, POLLIN, 0 } };
    int r = poll(p, 2, wait_ms);
    int e = errno;
    x->ops->woke(x);
    if (r < 0) return e == EINTR ? -2 : -1;
    if (r == 0) return x->ops->pending(x) ? 1 : 0;
    if (p[1].revents && !x->ops->pending(x)) {
        int gone = conn_sock_gone(sel);
        if (gone < 0) return gone;
    }
    return 1;
#else
    return -1;
#endif
}

/* Write all of `buf` to the connection, wherever its bytes go, giving up
 * at the deadline (ETIMEDOUT) as ray_sock_send_until does.  On a
 * transport, a full ring waits for the peer to read; the socket is
 * watched meanwhile so a peer that died with the ring full is noticed. */
static int64_t conn_send_all(ray_selector_t* sel, const uint8_t* buf, size_t len,
                             int64_t deadline_ms)
{
    ray_xport_t* x = sel->xport;
    if (!x) return ray_sock_send_until((ray_sock_t)sel->fd, buf, len, deadline_ms);
#ifdef RAY_IPC_XPORT
    size_t at = 0;
    while (at < len) {
        int64_t n = x->ops->write(x, buf + at, (int64_t)(len - at), false);
        if (n < 0) { errno = EPIPE; return -1; }
        if (n > 0) { at += (size_t)n; continue; }
        int wait_ms = 100;
        if (deadline_ms >= 0) {
            int64_t left = deadline_ms - ray_time_now_ms();
            if (left <= 0) { errno = ETIMEDOUT; return -1; }
            if (left < wait_ms) wait_ms = (int)left;
        }
        /* Room is all that is waited for: inbound bytes nobody is reading
         * right now must not keep the writer awake. */
        if (x->ops->prepare_sleep(x, false, true)) {
            struct pollfd p[2] = { { x->event_fd, POLLIN, 0 }, { (int)sel->fd, POLLIN, 0 } };
            poll(p, 2, wait_ms);
            x->ops->woke(x);
            if (p[1].revents && conn_sock_gone(sel) < 0) { errno = EPIPE; return -1; }
        }
    }
    return (int64_t)len;
#else
    errno = EPIPE;
    return -1;
#endif
}

static int conn_tx_drain_blocking(ray_poll_t* poll, ray_selector_t* sel,
                                  int64_t deadline_ms)
{
    if (!sel || !sel->tx.buf) return 0;
    while (sel->tx.buf) {
        ray_poll_buf_t* buf = sel->tx.buf;
        int64_t rem = buf->size - buf->offset;
        if (rem > 0 &&
            conn_send_all(sel, buf->data + buf->offset, (size_t)rem,
                          deadline_ms) < 0)
            return -1;
        sel->tx.buf = buf->next;
        buf->next = NULL;
        ray_poll_buf_free(buf);
    }
    ray_poll_tx_cancel(poll, sel);
    return 0;
}

static void send_response(ray_selector_t* sel, ray_t* result, size_t threshold)
{
    ray_serde_fail_clear();
    int64_t ser_size = ray_serde_size(result);

    /* A result we cannot serialize must never leave the client waiting on
     * a reply that never arrives.  Substitute a serializable error so the
     * caller observes a clean failure instead of a silent infinite hang
     * (issue #285).  ray_error frames serialize, so the substitute always
     * goes out unless even that fails — in which case there is nothing we
     * can put on the wire and we drop as before. */
    ray_t* fallback = NULL;
    if (ser_size <= 0) {
        const char* fcode = ray_serde_fail_code();
        if (fcode) {                       /* an encoded column failed to decode: say so */
            char code[8], msg[256];
            snprintf(code, sizeof code, "%s", fcode);
            snprintf(msg, sizeof msg, "%s", ray_error_msg() ? ray_error_msg() : "");
            fallback = ray_error(code, "%s", msg);
        } else {
            fallback = ray_error("type", "result of type %s is not serializable over IPC",
                                 result ? ray_type_name(result->type) : "null");
        }
        result   = fallback;
        ser_size = ray_serde_size(result);
        if (ser_size <= 0) { if (fallback) ray_error_free(fallback); return; }
    }

    uint8_t* payload = (uint8_t*)ray_alloc_raw((size_t)ser_size);
    if (!payload) { if (fallback) ray_error_free(fallback); return; }
    int64_t  wrote    = ray_ser_raw(payload, result);
    if (wrote != ser_size) {
        /* The write phase disagrees with the size phase (an encoded column
         * failed to decode): never send the frame, answer with an error. */
        ray_free_raw(payload);
        if (fallback) { ray_error_free(fallback); return; }
        fallback = ray_error("io", "result could not be serialized over IPC");
        result   = fallback;
        ser_size = ray_serde_size(result);
        payload  = ser_size > 0 ? (uint8_t*)ray_alloc_raw((size_t)ser_size) : NULL;
        if (!payload || ray_ser_raw(payload, result) != ser_size) {
            if (payload) ray_free_raw(payload);
            ray_error_free(fallback);
            return;
        }
    }

    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > threshold) {
        uint8_t* comp = (uint8_t*)ray_alloc_raw((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress_at(payload, (size_t)ser_size,
                                              comp, (size_t)ser_size, threshold);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_alloc_raw(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_free_raw(comp);
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
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = RAY_IPC_MSG_RESP,
        .size    = (int64_t)send_len,
    };
    if (conn_send_all(sel, (const uint8_t*)&hdr, sizeof(hdr), -1) == (int64_t)sizeof(hdr))
        conn_send_all(sel, send_buf, send_len, -1);

    ray_free_raw(send_buf);
    if (payload) ray_free_raw(payload);
    if (fallback) ray_error_free(fallback);
}

/* Decompress (when flagged) + de-serialize one framed payload into an
 * object.  Returns NULL on any decompress/decode failure.  Shared by
 * the request-eval path below and the RESP-frame path in
 * ipc_read_payload. */
static ray_t* deser_frame(uint8_t* payload, size_t payload_len, uint8_t flags)
{
    uint8_t* decompressed = NULL;
    if (flags & RAY_IPC_FLAG_COMPRESSED) {
        if (payload_len < 4) return NULL;
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        if (uncomp_size == 0 || uncomp_size > 256u * 1024u * 1024u) return NULL;
        decompressed = (uint8_t*)ray_alloc_raw(uncomp_size);
        if (!decompressed) return NULL;
        size_t dlen = ray_ipc_decompress(payload + 4, payload_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_free_raw(decompressed);
            return NULL;
        }
        payload     = decompressed;
        payload_len = uncomp_size;
    }
    int64_t de_len = (int64_t)payload_len;
    ray_t*  msg    = ray_de_raw(payload, &de_len);
    if (decompressed) ray_free_raw(decompressed);
    return msg;
}

/* Run the actual decompress + de-serialize + ray_eval pipeline on a
 * single inbound payload.  The wrapper eval_payload() below decides
 * whether to capture stdout/stderr (RAY_IPC_FLAG_VERBOSE) and whether
 * to wrap the result in a [captured_str, result] list. */
static ray_t* eval_payload_core(uint8_t* payload, size_t payload_len,
                                ray_ipc_header_t* hdr)
{
    /* Journal hook: log every inbound SYNC message (state-mutation
     * channel in the IPC model) before evaluation, so a crash mid-handler
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
     * eval and return an error to the client.  The documented
     * behaviour: "the message has not been logged so we cannot
     * accept it".  Silently evaluating un-logged mutations defeats
     * the entire durability premise of `-l`/`-L`. */
    bool auto_journaled = false;
    if (ray_journal_is_open() && hdr->msgtype == RAY_IPC_MSG_SYNC) {
        ray_ipc_header_t log_hdr = *hdr;
        if (ray_eval_get_restricted())
            log_hdr.flags |= RAY_IPC_FLAG_RESTRICTED;
        ray_err_t je = ray_journal_write_bytes(&log_hdr, payload, (int64_t)payload_len);
        if (je != RAY_OK) {
            fprintf(stderr, "log: ERROR  journal write failed (rc=%d) — refusing to evaluate\n", (int)je);
            return ray_error("io", "journal write failed; mutation refused");
        }
        auto_journaled = true;
    }

    /* Query-statistics ring: bracket the eval so the server records one
     * summary row per handled query.  No-op when logging is disabled. */
    ray_qlog_ctx_t qc;
    ray_qlog_begin(&qc);

    ray_t* msg = deser_frame(payload, payload_len, hdr->flags);

    /* Capture the query source for the log before `msg` is released during
     * dispatch: a string payload is the text verbatim; any other expression
     * is formatted back to source.  Only when logging is on. */
    char   qsrc[RAY_QLOG_QUERY_MAX];
    size_t qsrc_len = 0;
    if (qc.measure.active && msg && !RAY_IS_ERR(msg)) {
        if (msg->type == -RAY_STR) {
            qsrc_len = ray_str_len(msg);
            if (qsrc_len > sizeof(qsrc)) qsrc_len = sizeof(qsrc);
            memcpy(qsrc, ray_str_ptr(msg), qsrc_len);
        } else {
            ray_t* s = ray_fmt(msg, 0);
            if (s && !RAY_IS_ERR(s)) {
                qsrc_len = ray_str_len(s);
                if (qsrc_len > sizeof(qsrc)) qsrc_len = sizeof(qsrc);
                memcpy(qsrc, ray_str_ptr(s), qsrc_len);
            }
            if (s) ray_release(s);
        }
    }

    ray_t* result = NULL;
    bool prev_auto_journal_eval = ray_ipc_set_auto_journal_eval(auto_journaled ? true : g_auto_journal_eval);
    if (msg && !RAY_IS_ERR(msg)) {
        if (msg->type == RAY_LIST) {
            ray_t** elems = (ray_t**)ray_data(msg);
            for (int64_t i = 1; i < ray_len(msg); i++)
                mark_ipc_literal_fallbacks(elems[i]);
        }

        /* Dispatch through `.ipc.on.sync` / `.ipc.on.async` hook if
         * installed; otherwise fall back to v1's inline-eval default.
         * The hook receives the raw deserialised payload — same shape
         * any other Rayfall lambda would see — so a hook installed
         * as `{[m] eval m}` reproduces the default behaviour. */
        int hook_idx = (hdr->msgtype == RAY_IPC_MSG_SYNC) ? IPC_HOOK_SYNC
                                                           : IPC_HOOK_ASYNC;
        /* Also not VM-bound — eval_payload runs under ray_poll_run, which
         * owns a VM.  See hook_call_lifecycle for the case that does not. */
        ray_t* hook = hook_lookup(hook_idx);
        if (hook) {
            result = call_fn1(hook, msg);
            ray_release(msg);
            /* Async errors have nowhere to go on the wire (async never
             * sends a response), so log + drop here.  Without this the
             * caller would silently release the error and the operator
             * would never see the hook misbehaving. */
            if (result && RAY_IS_ERR(result) &&
                hdr->msgtype == RAY_IPC_MSG_ASYNC) {
                fprintf(stderr, "ipc: .ipc.on.async hook raised an error\n");
                ray_release(result);
                result = NULL;
            }
        } else if (msg->type == -RAY_STR) {
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
    ray_ipc_set_auto_journal_eval(prev_auto_journal_eval);
    /* A lazy result is an internal deferred-DAG representation that cannot
     * be serialized — force it to a concrete value before it reaches the
     * wire.  The direct ray_eval(msg) path (non-STR payloads, e.g. an
     * expression list `(first v)`) returns lazy chains verbatim; the
     * ray_eval_str path already materializes, so this is a no-op there.
     * Without this, send_response cannot serialize the result and the
     * client blocks forever waiting for a reply (issue #285). */
    if (result && ray_is_lazy(result))
        result = ray_lazy_materialize(result);  /* consumes the retain */

    ray_qlog_end(&qc, qsrc, qsrc_len, result);
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
#ifdef RAY_OS_WINDOWS
/* MSVCRT's tmpfile() creates its file in the drive root (admin-only on a
 * stock system); use the user's temp directory, deleted on close ("D").
 * With no TMP/TEMP/USERPROFILE in the environment GetTempPath falls back
 * to the (unwritable) Windows directory, so retry in the working dir. */
static FILE* ipc_tmpfile(void)
{
    char dir[MAX_PATH + 1], path[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof(dir), dir);
    if (n == 0 || n > sizeof(dir) || !GetTempFileNameA(dir, "ray", 0, path)) {
        if (!GetTempFileNameA(".", "ray", 0, path)) return NULL;
    }
    return fopen(path, "w+bD");
}
#else
#define ipc_tmpfile() tmpfile()
#endif

static ray_t* eval_payload(uint8_t* payload, size_t payload_len,
                           ray_ipc_header_t* hdr)
{
    if (!(hdr->flags & RAY_IPC_FLAG_VERBOSE))
        return eval_payload_core(payload, payload_len, hdr);

    FILE* cap = ipc_tmpfile();
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

/* Per-connection state stored in selector->data.  Used for BOTH
 * directions: inbound conns accepted by a listener and outbound conns
 * registered by ray_ipc_connect — the selector id is the one handle
 * namespace either side of the wire works with. */
typedef struct {
    ray_ipc_header_t hdr;
    uint8_t          phase;
    int64_t          listener_id;  /* id of the listener selector; -1 = outbound */
    /* Compression policy for this link, resolved once when the connection
     * is established (ray_ipc_link_threshold) so the send paths do not pay
     * a getpeername per frame. */
    size_t           compress_threshold;
    bool             auth_required;  /* server has -u/-U */
    bool             restricted;     /* server has -U */
    /* Sync round-trip state: while a ray_ipc_send waits on this conn it
     * pumps the rx machinery itself; a RESP frame is deposited here
     * instead of being evaluated.  Interleaved ASYNC/SYNC frames still
     * dispatch normally during the wait. */
    bool             sync_waiting;
    bool             sync_ready;
    ray_t*           sync_resp;
    /* Transport negotiation: the capabilities the peer shares, whether
     * the compression threshold was asked for explicitly (an upgraded link
     * otherwise stops compressing: there is no bandwidth to buy), and the
     * UPGRADE answer a client waits for, deposited by the rx pump. */
    uint8_t          peer_caps;
    bool             compress_explicit;
    bool             upgrade_waiting;
    ray_poll_buf_t*  upgrade_resp;
    /* A server's RDMA endpoint answered but not yet moved to: the client
     * probes the fabric first and says XP_READY, or refuses. */
    ray_xport_t*     pending_xport;
} ray_ipc_conn_data_t;

/* ===== Transport policy ===== */

static int g_xport_policy = RAY_IPC_XPORT_AUTO;

int ray_ipc_xport_policy_get(void) { return g_xport_policy; }

bool ray_ipc_xport_policy_set(int policy)
{
    if (policy < RAY_IPC_XPORT_AUTO || policy > RAY_IPC_XPORT_RDMA) return false;
    g_xport_policy = policy;
    return true;
}

const char* ray_ipc_xport_policy_name(int policy)
{
    switch (policy) {
    case RAY_IPC_XPORT_AUTO: return "auto";
    case RAY_IPC_XPORT_TCP:  return "tcp";
    case RAY_IPC_XPORT_SHM:  return "shm";
    case RAY_IPC_XPORT_RDMA: return "rdma";
    default: return NULL;
    }
}

const char* ray_ipc_xport_kind_name(int kind)
{
    switch (kind) {
    case RAY_XPORT_TCP:  return "tcp";
    case RAY_XPORT_SHM:  return "shm";
    case RAY_XPORT_RDMA: return "rdma";
    default: return NULL;
    }
}

int ray_ipc_xport_policy_parse(const char* s, size_t n)
{
    if (n == 4 && memcmp(s, "auto", 4) == 0) return RAY_IPC_XPORT_AUTO;
    if (n == 3 && memcmp(s, "tcp", 3) == 0)  return RAY_IPC_XPORT_TCP;
    if (n == 3 && memcmp(s, "shm", 3) == 0)  return RAY_IPC_XPORT_SHM;
    if (n == 4 && memcmp(s, "rdma", 4) == 0) return RAY_IPC_XPORT_RDMA;
    return -1;
}

/* What this process puts in its greeting: the upgrade capability, when
 * the policy allows any transport and the build has them. */
static uint8_t our_caps(int policy)
{
#ifdef RAY_IPC_XPORT
    return policy == RAY_IPC_XPORT_TCP ? 0 : RAY_IPC_CAP_XPORT;
#else
    (void)policy;
    return 0;
#endif
}

#ifdef RAY_IPC_XPORT
static bool policy_allows(int policy, int kind)
{
    if (policy == RAY_IPC_XPORT_AUTO) return true;
    if (policy == RAY_IPC_XPORT_SHM)  return kind == RAY_XPORT_SHM;
    if (policy == RAY_IPC_XPORT_RDMA) return kind == RAY_XPORT_RDMA;
    return false;
}
#endif

/* Upgrade records: a kind byte, then the kind's fields. */
enum {
    XP_REFUSE    = 0,
    XP_SHM_ASK   = 1,   /* + the client's host identity (RAY_SHM_IDENTITY_LEN) */
    XP_SHM_OFFER = 2,   /* + token (16) + ring bytes (8) + the Unix socket's name, NUL-terminated */
    XP_RDMA      = 3,   /* + ray_rdma_record_t */
    XP_SWITCHED  = 4,   /* the server's last TCP frame: its side now reads the rings */
    XP_READY     = 5,   /* the client's last TCP frame: its RDMA probe crossed, move now */
};
#define XP_TOKEN_LEN 16
#define XP_NAME_MAX  64

static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_creds(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_header(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel);
static void   ipc_on_close(ray_poll_t* poll, ray_selector_t* sel);
static void   serve_upgrade(ray_poll_t* poll, int64_t id, ray_ipc_conn_data_t* cd,
                            ray_poll_buf_t* payload);
static int    client_upgrade(ray_poll_t* poll, int64_t id, int want, int64_t deadline);
#ifdef RAY_IPC_XPORT
static void   shm_offers_drop_conn(ray_poll_t* poll, int64_t id, const void* cd);
static void   shm_side_listen(ray_poll_t* poll);
#endif
static int    conn_tx_drain_blocking(ray_poll_t* poll, ray_selector_t* sel,
                                     int64_t deadline_ms);

/* Wrappers matching ray_io_fn signature for socket recv */
static int64_t ipc_recv_fn(int64_t fd, uint8_t* buf, int64_t len) {
    return ray_sock_recv((ray_sock_t)fd, buf, (size_t)len);
}

static int64_t ipc_send_fn(int64_t fd, uint8_t* buf, int64_t len)
{
#ifdef RAY_OS_WINDOWS
    int n = send((ray_sock_t)fd, (const char*)buf, (int)len, 0);
    if (n < 0) {
        /* Always overwrite errno: callers treat EAGAIN as "queue and retry",
         * so a stale EAGAIN left over from an earlier call would turn a dead
         * socket into a silently parked frame. */
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) errno = EAGAIN;
        else if (e == WSAEINTR) errno = EINTR;
        else errno = EIO;
    }
    return n;
#else
    return send((ray_sock_t)fd, buf, (size_t)len,
                MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
}

/* ── Out-of-band cancel: client Ctrl-C → SIGURG → cancel the running query ──
 * The server evaluates single-threaded, so while a query runs it is NOT reading
 * the socket — an ordinary cancel message would sit unread until the query
 * finished.  A client instead sends one byte of TCP urgent data (see the
 * sync-send await loop), which raises SIGURG here even mid-eval.  The handler
 * drains the byte and requests interrupt; the running query's checkpoints then
 * unwind it and a "cancel" error is returned.  g_ipc_active_fd names the
 * connection whose payload is currently being evaluated (-1 when idle) so the
 * handler knows which socket to drain.  Everything here is async-signal-safe:
 * a sig_atomic_t read, recv(MSG_OOB), and the flag stores in
 * ray_request_interrupt(). */
static volatile sig_atomic_t g_ipc_active_fd = -1;

#ifndef RAY_OS_WINDOWS   /* no SIGURG on Windows: OOB cancel is POSIX-only */
static void ipc_sigurg_handler(int sig)
{
    (void)sig;
    int fd = g_ipc_active_fd;
    if (fd < 0) return;   /* nothing running — a queued client's cancel is
                           * honored when its own request is dispatched */
    /* Cancel ONLY if the urgent byte is on the RUNNING query's connection.
     * OOB from any OTHER (queued) client must not touch this query; that byte
     * stays pending on its socket and is honored when that client runs. */
    if (ray_sock_take_oob((ray_sock_t)fd))
        ray_request_interrupt();
}
#endif

static void ipc_install_oob_cancel(void)
{
    static int installed = 0;
    if (installed) return;
    installed = 1;
#ifndef RAY_OS_WINDOWS
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ipc_sigurg_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGURG, &sa, NULL);
#endif
}

/* Accept callback — called when listener fd is readable */
static ray_t* ipc_accept(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_sock_t new_fd = ray_sock_accept((ray_sock_t)sel->fd);
    if (new_fd == RAY_INVALID_SOCK) return NULL;
    ray_sock_set_nonblocking(new_fd);
    ray_sock_set_oob_owner(new_fd);   /* so a client's OOB cancel raises SIGURG here */

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(new_fd); return NULL; }
    memset(cd, 0, sizeof(*cd));
    cd->phase = RAY_IPC_PHASE_HANDSHAKE;
    cd->listener_id = sel->id;
    cd->auth_required = (poll->auth_secret[0] != '\0');
    cd->restricted    = poll->restricted;
    cd->compress_threshold = ray_ipc_link_threshold(new_fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)new_fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.send_fn  = ipc_send_fn;
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

    /* Send handshake response: version + auth_required flag, and the
     * capabilities both sides share — only when the client advertised
     * any: an older client refuses a second byte that is not 0 or 1. */
    uint8_t client_caps = sel->rx.buf->data[1] & RAY_IPC_CAP_MASK;
    cd->peer_caps = client_caps & our_caps(g_xport_policy);
    uint8_t resp[2] = { RAY_SERDE_WIRE_VERSION,
                        (uint8_t)((cd->auth_required ? 0x01 : 0x00) | cd->peer_caps) };
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
    /* No-auth path: connection is now fully ready for inbound messages.
     * Fire `.ipc.on.open` AFTER we've requested the next read, so a
     * hook that calls back into the server can't race the read pump. */
    hook_call_lifecycle(poll, IPC_HOOK_OPEN, sel->id);
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

    /* Secondary user-defined check via `.ipc.on.auth`.  Only consulted
     * when the constant-time secret compare already passed — this hook
     * can narrow access (deny extras) but never widen it.  Errors and
     * falsy returns flip `ok` to false, triggering the same reject byte
     * + deregister the secret-mismatch path would. */
    if (ok) {
        int hook_ok = hook_call_auth(poll, sel->id, sel->rx.buf->data + 1, cred_len);
        if (hook_ok == 0) ok = false;
    }

    uint8_t result = ok ? 0x00 : 0x01;
    ray_sock_send((ray_sock_t)sel->fd, &result, 1);

    if (!ok) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));
    /* Auth path: fully handshaked and authed — connection is now ready
     * for inbound messages.  Same ordering as the no-auth branch above:
     * fire AFTER the next read is requested. */
    hook_call_lifecycle(poll, IPC_HOOK_OPEN, sel->id);
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
        cd->hdr.endian != RAY_SERDE_ENDIAN ||
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

    /* Detach the payload buffer and advance the state machine to the
     * next header BEFORE dispatching: the eval below may re-enter this
     * connection's rx pump (a hook doing a nested sync round-trip on
     * the same handle), and that pump must find the selector parked on
     * a fresh header read — not on this very payload, which would
     * re-dispatch it.  Same reason for the local header copy: a nested
     * frame would overwrite cd->hdr under our feet. */
    ray_ipc_header_t hdr     = cd->hdr;
    ray_poll_buf_t*  payload = sel->rx.buf;
    int64_t          id      = sel->id;
    sel->rx.buf = NULL;
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));

    /* An upgrade record: the answer a client of ours waits for, or a
     * client's ask that we answer.  Never evaluated, never journaled. */
    if (hdr.msgtype == RAY_IPC_MSG_UPGRADE) {
        if (cd->upgrade_waiting && !cd->upgrade_resp) {
            cd->upgrade_resp = payload;
            return NULL;
        }
        serve_upgrade(poll, id, cd, payload);
        ray_poll_buf_free(payload);
        return NULL;
    }

    /* Response frame: deposit it for the sync send waiting on this
     * conn instead of evaluating it.  A response nobody waits for has
     * no defined meaning — log and drop. */
    if (hdr.msgtype == RAY_IPC_MSG_RESP) {
        ray_t* obj = deser_frame(payload->data, (size_t)payload->offset,
                                 hdr.flags);
        ray_poll_buf_free(payload);
        if (cd->sync_waiting && !cd->sync_ready) {
            cd->sync_resp  = obj;
            cd->sync_ready = true;
        } else {
            fprintf(stderr, "ipc: dropping unexpected response frame "
                            "(handle=%lld)\n", (long long)id);
            if (obj && obj != RAY_NULL_OBJ) ray_release(obj);
        }
        return NULL;
    }

    bool prev_restricted = ray_eval_get_restricted();
    ray_eval_set_restricted(cd->restricted);

    /* Expose this connection's selector id to `.ipc.handle` (and its
     * poll to handle resolution) for the duration of any hook that
     * runs inside eval_payload.  Save/restore so a hook that itself
     * opens a nested IPC round-trip doesn't leave the wrong handle
     * visible when its caller resumes. */
    int64_t prev_handle = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(id, poll);

    /* If THIS client already requested cancel (an OOB byte is pending on its
     * socket) — e.g. it Ctrl-C'd while its request waited behind another
     * client's running query — honor it now: return cancel without running,
     * which unblocks the waiting client.  A cancel is thus always scoped to the
     * connection that sent it and never affects another client's query. */
    ray_t* result;
    if (ray_sock_take_oob((ray_sock_t)sel->fd)) {
        result = ray_error("cancel", "interrupted");
    } else {
        /* Clear any stale cancel first: the server is single-threaded, so each
         * request starts a fresh query — without this reset a prior cancel
         * would make every later query fail spuriously (the REPL clears per
         * line the same way).  Publish the active fd so a SIGURG for THIS
         * connection cancels this query. */
        ray_clear_interrupt();
        g_ipc_active_fd = (int)sel->fd;
        result = eval_payload(payload->data, (size_t)payload->offset, &hdr);
        g_ipc_active_fd = -1;
    }

    ipc_ctx_set(prev_handle, prev_poll);
    ray_eval_set_restricted(prev_restricted);
    ray_poll_buf_free(payload);

    /* Send response for sync messages.  The eval may have closed this
     * very connection (`.ipc.close` on its own handle) — revalidate the
     * selector before writing to its fd. */
    if (hdr.msgtype == RAY_IPC_MSG_SYNC) {
        ray_selector_t* cur = ray_poll_get(poll, id);
        /* Flush any multicast frame still queued to this fd before the RESP,
         * so the reply lands on a frame boundary instead of splicing into a
         * half-sent push.  A drain error means the peer is gone — skip the
         * write and let the poll/pump layer deregister it. */
        if (cur && cur->data == (void*)cd &&
            conn_tx_drain_blocking(poll, cur, -1) == 0)
            {
                ray_ipc_conn_data_t* rcd = (ray_ipc_conn_data_t*)cur->data;
                send_response(cur, result,
                              rcd ? rcd->compress_threshold
                                  : ray_ipc_link_threshold((ray_sock_t)cur->fd));
            }
    }
    if (result != RAY_NULL_OBJ) ray_release(result);
    /* The request is served: this is the end of the server's unit of work,
     * and stamping here is what lets the poll loop tell an idle server from
     * one between two requests.  Stamping on frame arrival instead would
     * make the measured gap the request's own duration. */
    ray_heap_note_activity();
    ray_enccol_cache_trim();
    ray_heap_relieve();

    return NULL;
}

static void ipc_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_mcast_on_close(poll, sel ? sel->id : -1);
    /* Fire `.ipc.on.close` BEFORE tearing the per-conn state down so a
     * hook reading `.ipc.handle` still sees this connection's id, and
     * before the listener's own close path (which would otherwise also
     * route through here) runs the hook with a stale fd.  Guard on:
     *   - sel->data: the listener itself has no conn data.
     *   - phase ≥ HEADER: the connection actually completed handshake.
     *     An inbound one that died mid-handshake never fired on.open;
     *     an outbound one is registered after its handshake, so it
     *     always qualifies.
     * The hook fires for both directions (#503): a receive-only client —
     * every multicast subscriber — otherwise learns of the peer's close
     * only by probing.  It fires once per established connection on any
     * teardown (peer EOF, socket error, local .ipc.close, eviction);
     * on.open stays inbound-only, and (.ipc.handle h) reports `inbound`
     * so one handler can tell the direction. */
    if (sel->data) {
        ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
        if (cd->phase == RAY_IPC_PHASE_HEADER ||
            cd->phase == RAY_IPC_PHASE_PAYLOAD) {
            hook_call_lifecycle(poll, IPC_HOOK_CLOSE, sel->id);
        }
        /* A RESP deposited for a sync wait that never consumed it
         * (connection died mid-round-trip) would otherwise leak. */
        if (cd->sync_resp) {
            if (cd->sync_resp != RAY_NULL_OBJ) ray_release(cd->sync_resp);
            cd->sync_resp = NULL;
        }
        if (cd->upgrade_resp) {
            ray_poll_buf_free(cd->upgrade_resp);
            cd->upgrade_resp = NULL;
        }
        if (cd->pending_xport) {
            cd->pending_xport->ops->destroy(cd->pending_xport);
            cd->pending_xport = NULL;
        }
#ifdef RAY_IPC_XPORT
        shm_offers_drop_conn(poll, sel->id, cd);
#endif
        ray_sys_free(sel->data);
        sel->data = NULL;
    }
    ray_sock_close((ray_sock_t)sel->fd);
}


/* ======================================================================
 * Transport upgrades
 *
 * The client asks, the server answers, both over the TCP frames they
 * already speak (RAY_IPC_MSG_UPGRADE).  Shared memory needs a second,
 * private road for the descriptors: the server makes the rings, keeps
 * them under a one-use token, and the client shows the token on the
 * server's abstract Unix socket — from the same user, checked by peer
 * credentials — to receive them.  The server moves the connection when
 * the descriptors have gone and says so with one last TCP frame
 * (XP_SWITCHED); the client moves when it has read that frame, so every
 * byte either side wrote on TCP before moving has been read before the
 * rings are.  RDMA needs no second road: the records carry everything,
 * the server moves as it answers, the client as it reads the answer.
 * ====================================================================== */

/* One UPGRADE frame, written straight to the socket: the connection is on
 * TCP while it negotiates, and its queue was drained first. */
static int upgrade_send(ray_sock_t fd, const uint8_t* rec, size_t n)
{
    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_SERDE_WIRE_VERSION,
        .flags   = 0,
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = RAY_IPC_MSG_UPGRADE,
        .size    = (int64_t)n,
    };
    if (ray_sock_send(fd, &hdr, sizeof hdr) < 0) return -1;
    return ray_sock_send(fd, rec, n) < 0 ? -1 : 0;
}

#ifdef RAY_IPC_XPORT

/* Once a connection moves, nothing crosses the host's network any more:
 * compression would buy no bandwidth with the CPU it costs. */
static void upgraded_threshold(ray_ipc_conn_data_t* cd)
{
    if (!cd->compress_explicit) cd->compress_threshold = RAY_IPC_COMPRESS_NEVER;
}

/* ---- the server's side: offers and the Unix side channel ---- */

/* A same-host upgrade offered and not yet taken: the token the client
 * shows on the Unix socket, the connection, the rings and the
 * descriptors to pass.  Offers nobody takes expire. */
typedef struct {
    bool         used;
    uint8_t      token[XP_TOKEN_LEN];
    ray_poll_t*  poll;
    int64_t      conn_id;
    const void*  cd;        /* the connection's data, to tell a reused id apart */
    ray_xport_t* xport;
    int          fds[3];
    int64_t      made_ms;
} shm_offer_t;

#define SHM_OFFERS_MAX   32
#define SHM_OFFER_TTL_MS 10000

/* Offers and side listeners are process globals touched only by the
 * thread running the listening poll (serve_upgrade, the side channel's
 * callbacks), like g_journal: one listening poll per process is the rule,
 * enforced by nothing but this comment.  A client poll on another thread
 * (test_mcast.c's layout) never touches them. */
static shm_offer_t g_offers[SHM_OFFERS_MAX];

static void shm_offer_drop(shm_offer_t* o)
{
    if (!o->used) return;
    if (o->xport) o->xport->ops->destroy(o->xport);
    for (int i = 0; i < 3; i++) if (o->fds[i] >= 0) close(o->fds[i]);
    memset(o, 0, sizeof *o);
}

static void shm_offers_expire(void)
{
    int64_t now = ray_time_now_ms();
    for (int i = 0; i < SHM_OFFERS_MAX; i++)
        if (g_offers[i].used && now - g_offers[i].made_ms > SHM_OFFER_TTL_MS)
            shm_offer_drop(&g_offers[i]);
}

static void shm_offers_drop_conn(ray_poll_t* poll, int64_t id, const void* cd)
{
    for (int i = 0; i < SHM_OFFERS_MAX; i++)
        if (g_offers[i].used && g_offers[i].poll == poll &&
            g_offers[i].conn_id == id && g_offers[i].cd == cd)
            shm_offer_drop(&g_offers[i]);
}

static bool random_bytes(uint8_t* p, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t at = 0;
    while (at < n) {
        ssize_t r = read(fd, p + at, n - at);
        if (r <= 0) { close(fd); return false; }
        at += (size_t)r;
    }
    close(fd);
    return true;
}

static void hex_of(const uint8_t* p, size_t n, char* out)
{
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 15];
    }
    out[2 * n] = 0;
}

/* The side channel's listener, one per poll that listens, named after
 * the process and its descriptor so the name is unique on the host; the
 * name travels in the offer. */
typedef struct {
    ray_poll_t* poll;
    int64_t     id;
    char        name[XP_NAME_MAX];
} shm_side_t;

#define SHM_SIDES_MAX 16
static shm_side_t g_sides[SHM_SIDES_MAX];

static shm_side_t* shm_side_for(ray_poll_t* poll)
{
    for (int i = 0; i < SHM_SIDES_MAX; i++)
        if (g_sides[i].poll == poll) return &g_sides[i];
    return NULL;
}

static void shm_side_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    for (int i = 0; i < SHM_SIDES_MAX; i++)
        if (g_sides[i].poll == poll && g_sides[i].id == sel->id)
            memset(&g_sides[i], 0, sizeof g_sides[i]);
    close((int)sel->fd);
}

/* Moves `conn` to `x`, its queue drained first so no frame straddles the
 * two roads.  On any failure the connection is dropped: the peer has
 * moved, or is about to, and TCP alone can no longer carry it. */
static bool conn_move(ray_poll_t* poll, int64_t id, ray_xport_t* x)
{
    ray_selector_t* conn = ray_poll_get(poll, id);
    if (!conn || !conn->data || conn->xport ||
        conn_tx_drain_blocking(poll, conn, -1) < 0 ||
        ray_poll_xport_attach(poll, conn, x) < 0) {
        x->ops->destroy(x);
        if (conn) ray_poll_deregister(poll, id);
        return false;
    }
    upgraded_threshold((ray_ipc_conn_data_t*)conn->data);
    return true;
}

/* The server's side of a move: the connection goes to `x`, and the
 * client hears so in the server's last TCP frame. */
static void server_switch(ray_poll_t* poll, int64_t id, ray_xport_t* x)
{
    ray_selector_t* conn = ray_poll_get(poll, id);
    if (!conn) { x->ops->destroy(x); return; }
    ray_sock_t sock = (ray_sock_t)conn->fd;
    if (conn_move(poll, id, x)) {
        uint8_t switched = XP_SWITCHED;
        if (upgrade_send(sock, &switched, 1) < 0) ray_poll_deregister(poll, id);
    }
}

/* The side channel's one-byte answers: 'k' with the descriptors, 'n' for
 * no — a client that hears no knows the server has not moved and stays
 * on TCP, where a silent close would have left it guessing. */
static void side_say_no(int fd)
{
    char no = 'n';
    ssize_t r = send(fd, &no, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    (void)r;
}

/* A connection on the side channel showing a token: the descriptors go
 * to it, the offered connection moves to the rings, and the client hears
 * so on TCP. */
static ray_t* shm_side_read(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 2 * XP_TOKEN_LEN) return NULL;
    int fd = (int)sel->fd;
    shm_offer_t* o = NULL;
    for (int i = 0; i < SHM_OFFERS_MAX && !o; i++) {
        if (!g_offers[i].used) continue;
        char hex[2 * XP_TOKEN_LEN + 1];
        hex_of(g_offers[i].token, XP_TOKEN_LEN, hex);
        if (memcmp(hex, sel->rx.buf->data, 2 * XP_TOKEN_LEN) == 0) o = &g_offers[i];
    }
    struct ucred who;
    socklen_t wl = sizeof who;
    if (!o || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &who, &wl) != 0 || who.uid != getuid()) {
        side_say_no(fd);
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }
    shm_offer_t taken = *o;
    memset(o, 0, sizeof *o);

    char byte = 'k';
    struct iovec io = { &byte, 1 };
    union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(int) * 3)]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov        = &io;
    mh.msg_iovlen     = 1;
    mh.msg_control    = u.space;
    mh.msg_controllen = sizeof u.space;
    struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int) * 3);
    memcpy(CMSG_DATA(cm), taken.fds, sizeof(int) * 3);
    bool sent = sendmsg(fd, &mh, MSG_NOSIGNAL) == 1;
    for (int i = 0; i < 3; i++) close(taken.fds[i]);
    ray_poll_deregister(poll, sel->id);

    ray_selector_t* conn = ray_poll_get(poll, taken.conn_id);
    if (!sent || !conn || conn->data != taken.cd) {
        taken.xport->ops->destroy(taken.xport);
        return NULL;
    }
    server_switch(poll, taken.conn_id, taken.xport);
    return NULL;
}

/* Side connections waiting for their token.  The abstract socket has no
 * permissions of its own, so a stranger is turned away at accept, and
 * what is left is bounded: an idler cannot hold descriptors for ever. */
#define SHM_SIDE_CONNS_MAX 64
static int g_side_conns;

static void shm_side_conn_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll;
    if (g_side_conns > 0) g_side_conns--;
    close((int)sel->fd);
}

static ray_t* shm_side_accept(ray_poll_t* poll, ray_selector_t* sel)
{
    int c = accept4((int)sel->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return NULL;
    struct ucred who;
    socklen_t wl = sizeof who;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &who, &wl) != 0 || who.uid != getuid() ||
        g_side_conns >= SHM_SIDE_CONNS_MAX) {
        /* Another user's client, or too many waiting: told no, so it
         * stays on TCP as it always could. */
        side_say_no(c);
        close(c);
        return NULL;
    }
    ray_poll_reg_t reg = {0};
    reg.fd       = c;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.read_fn  = shm_side_read;
    reg.close_fn = shm_side_conn_on_close;
    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) { close(c); return NULL; }
    g_side_conns++;
    shm_offers_expire();
    ray_selector_t* ns = ray_poll_get(poll, id);
    if (ns) ray_poll_rx_request(poll, ns, 2 * XP_TOKEN_LEN);
    return NULL;
}

static void shm_side_listen(ray_poll_t* poll)
{
    if (shm_side_for(poll)) return;
    shm_side_t* slot = NULL;
    for (int i = 0; i < SHM_SIDES_MAX && !slot; i++)
        if (!g_sides[i].poll) slot = &g_sides[i];
    if (!slot) return;
    int u = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (u < 0) return;
    char name[XP_NAME_MAX];
    int len = snprintf(name, sizeof name, "rayforce-shm.%ld.%d", (long)getpid(), u);
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof ua);
    ua.sun_family = AF_UNIX;
    memcpy(ua.sun_path + 1, name, (size_t)len);   /* abstract: no file, gone with the process */
    if (bind(u, (struct sockaddr*)&ua, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)len)) != 0 ||
        listen(u, 64) != 0) {
        close(u);
        return;
    }
    ray_poll_reg_t reg = {0};
    reg.fd       = u;
    reg.type     = RAY_SEL_SOCKET;
    reg.read_fn  = shm_side_accept;
    reg.close_fn = shm_side_on_close;
    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) { close(u); return; }
    slot->poll = poll;
    slot->id   = id;
    memcpy(slot->name, name, (size_t)len + 1);
}

/* ---- the client's side: fetching the rings ---- */

/* The descriptors of an offer, fetched over the server's Unix socket
 * with its token, within the deadline.  1 with the descriptors in fds;
 * 0 when the server could not be reached (it has not seen the token, so
 * it has not moved: the connection may stay on TCP); -1 when it was
 * reached and the fetch still failed (it may have moved: the connection
 * is lost). */
static int shm_fetch_rings(const char* name, const uint8_t* token, int fds[3], int64_t deadline)
{
    size_t len = strlen(name);
    if (len == 0 || len >= XP_NAME_MAX) return 0;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, name, len);
    int64_t left = deadline - ray_time_now_ms();
    int wait_ms = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
    if (connect(s, (struct sockaddr*)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len)) != 0) {
        /* A full backlog on a busy server says EAGAIN: wait for it within
         * the deadline rather than block without one. */
        struct pollfd pc = { s, POLLOUT, 0 };
        int err = errno;
        if (!(err == EAGAIN || err == EINPROGRESS) || poll(&pc, 1, wait_ms) <= 0 ||
            connect(s, (struct sockaddr*)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len)) != 0) {
            if (errno != EISCONN) { close(s); return 0; }
        }
    }
    char hex[2 * XP_TOKEN_LEN + 1];
    hex_of(token, XP_TOKEN_LEN, hex);
    if (ray_sock_send_until(s, hex, 2 * XP_TOKEN_LEN, deadline) < 0) { close(s); return 0; }
    char byte;
    struct iovec io = { &byte, 1 };
    union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(int) * 3)]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov        = &io;
    mh.msg_iovlen     = 1;
    mh.msg_control    = u.space;
    mh.msg_controllen = sizeof u.space;
    left = deadline - ray_time_now_ms();
    wait_ms = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
    struct pollfd p = { s, POLLIN, 0 };
    bool got = poll(&p, 1, wait_ms) > 0 && recvmsg(s, &mh, MSG_CMSG_CLOEXEC) == 1;
    if (got && byte == 'n') { close(s); return 0; }   /* refused before anything moved */
    /* Exactly three descriptors in one message, or none of what came is
     * kept: a stray descriptor installed by recvmsg must still be closed. */
    bool ok = false;
    for (struct cmsghdr* cm = got ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        size_t n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int* got_fds = (int*)CMSG_DATA(cm);
        if (!ok && n == 3 && byte == 'k') {
            memcpy(fds, got_fds, sizeof(int) * 3);
            ok = true;
        } else {
            for (size_t i = 0; i < n; i++) close(got_fds[i]);
        }
    }
    close(s);
    return ok ? 1 : -1;
}

/* Pumps the connection until the server's UPGRADE frame is deposited.
 * 0 with the frame in *reply; -1 when the connection closed or the
 * deadline passed (the caller closes it: the exchange is half done). */
static int upgrade_wait(ray_poll_t* poll, int64_t id, int64_t deadline, ray_poll_buf_t** reply)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (!sel || !sel->data) return -1;
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    cd->upgrade_waiting = true;
    cd->upgrade_resp    = NULL;
    for (;;) {
        int rc = ray_poll_serve_rx(poll, id);
        sel = ray_poll_get(poll, id);
        if (!sel || sel->data != (void*)cd) return -1;
        if (cd->upgrade_resp) {
            *reply = cd->upgrade_resp;
            cd->upgrade_resp    = NULL;
            cd->upgrade_waiting = false;
            return 0;
        }
        if (rc < 0) return -1;
        int64_t left = deadline - ray_time_now_ms();
        if (left <= 0) { cd->upgrade_waiting = false; return -1; }
        int w = conn_wait_readable(sel, left > INT_MAX ? INT_MAX : (int)left);
        if (w == -2) continue;
        if (w < 0) { cd->upgrade_waiting = false; return -1; }
    }
}

static int upgrade_exchange(ray_poll_t* poll, int64_t id, const uint8_t* rec, size_t n,
                            int64_t deadline, ray_poll_buf_t** reply)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (!sel || conn_tx_drain_blocking(poll, sel, deadline) < 0 ||
        upgrade_send((ray_sock_t)sel->fd, rec, n) < 0)
        return -1;
    return upgrade_wait(poll, id, deadline, reply);
}

static uint8_t reply_kind(const ray_poll_buf_t* r)
{
    return r && r->offset >= 1 ? r->data[0] : XP_REFUSE;
}

#endif /* RAY_IPC_XPORT */

/* A client's upgrade record, answered: shared memory on this host (the
 * rings made and offered under a token), RDMA (our record, the move
 * waiting for the client's XP_READY once its probe has crossed), or a
 * refusal.  XP_READY and a client's XP_REFUSE settle a pending RDMA
 * endpoint and are not answered. */
static void serve_upgrade(ray_poll_t* poll, int64_t id, ray_ipc_conn_data_t* cd,
                          ray_poll_buf_t* payload)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (!sel) return;
    const uint8_t* r = payload->data;
    size_t n = (size_t)payload->offset;
    uint8_t kind = n ? r[0] : XP_REFUSE;
    uint8_t answer[1 + XP_TOKEN_LEN + 8 + XP_NAME_MAX + sizeof(ray_rdma_record_t)];
    size_t  alen = 1;
    answer[0] = XP_REFUSE;
    ray_xport_t* rdma = NULL;
#ifdef RAY_IPC_XPORT
    if (cd->pending_xport && (kind == XP_READY || kind == XP_REFUSE)) {
        ray_xport_t* x = cd->pending_xport;
        cd->pending_xport = NULL;
        if (kind == XP_READY) server_switch(poll, id, x);
        else x->ops->destroy(x);
        return;
    }
    if (kind == XP_READY || kind == XP_REFUSE) return;   /* nothing pending: nothing to say */
    bool may = !sel->xport && !cd->pending_xport && (cd->peer_caps & RAY_IPC_CAP_XPORT);
    if (may && kind == XP_SHM_ASK && n == 1 + RAY_SHM_IDENTITY_LEN &&
        policy_allows(g_xport_policy, RAY_XPORT_SHM)) {
        const char* me = ray_shm_host_identity();
        shm_side_t* side = shm_side_for(poll);
        if (side && *me && memcmp(r + 1, me, RAY_SHM_IDENTITY_LEN) == 0) {
            shm_offers_expire();
            shm_offers_drop_conn(poll, id, cd);   /* at most one offer per connection */
            shm_offer_t* slot = NULL;
            for (int i = 0; i < SHM_OFFERS_MAX && !slot; i++)
                if (!g_offers[i].used) slot = &g_offers[i];
            if (slot) {
                uint64_t ring = ray_shm_ring_bytes();
                int fds[3] = { -1, -1, -1 };
                ray_xport_t* x = ray_shm_create(ring, fds);
                uint8_t token[XP_TOKEN_LEN];
                if (x && random_bytes(token, sizeof token)) {
                    slot->used    = true;
                    memcpy(slot->token, token, sizeof token);
                    slot->poll    = poll;
                    slot->conn_id = id;
                    slot->cd      = cd;
                    slot->xport   = x;
                    memcpy(slot->fds, fds, sizeof fds);
                    slot->made_ms = ray_time_now_ms();
                    size_t nl = strlen(side->name) + 1;
                    answer[0] = XP_SHM_OFFER;
                    memcpy(answer + 1, token, XP_TOKEN_LEN);
                    memcpy(answer + 1 + XP_TOKEN_LEN, &ring, 8);
                    memcpy(answer + 1 + XP_TOKEN_LEN + 8, side->name, nl);
                    alen = 1 + XP_TOKEN_LEN + 8 + nl;
                } else if (x) {
                    x->ops->destroy(x);
                    for (int i = 0; i < 3; i++) close(fds[i]);
                }
            }
        }
    } else if (may && kind == XP_RDMA && n == 1 + sizeof(ray_rdma_record_t) &&
               policy_allows(g_xport_policy, RAY_XPORT_RDMA) && ray_rdma_available()) {
        ray_rdma_record_t peer, mine;
        memcpy(&peer, r + 1, sizeof peer);
        rdma = ray_rdma_create(&mine);
        if (rdma && !ray_rdma_connect(rdma, &peer)) { rdma->ops->destroy(rdma); rdma = NULL; }
        if (rdma) {
            answer[0] = XP_RDMA;
            memcpy(answer + 1, &mine, sizeof mine);
            alen = 1 + sizeof mine;
        }
    }
#else
    (void)kind; (void)cd;
#endif
    if (conn_tx_drain_blocking(poll, sel, -1) < 0 ||
        upgrade_send((ray_sock_t)sel->fd, answer, alen) < 0) {
        if (rdma) rdma->ops->destroy(rdma);
        ray_poll_deregister(poll, id);
        return;
    }
#ifdef RAY_IPC_XPORT
    if (rdma) cd->pending_xport = rdma;   /* moved on the client's XP_READY */
#endif
}

/* A new client connection, handshaken and registered: ask the server to
 * move it.  Shared memory when the peer is on this host, RDMA when it is
 * on another and this host has a device; `want` names one of them to
 * insist on.  0 when the connection is ready (moved, or on TCP where AUTO
 * allows); -7 when `want` cannot be honoured; -1 when the exchange broke
 * the connection.  The caller closes it on either failure. */
static int client_upgrade(ray_poll_t* poll, int64_t id, int want, int64_t deadline)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (!sel || !sel->data) return -1;
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    if (want == RAY_IPC_XPORT_TCP) return 0;
    if (!(cd->peer_caps & RAY_IPC_CAP_XPORT)) return want == RAY_IPC_XPORT_AUTO ? 0 : -7;
#ifdef RAY_IPC_XPORT
    bool local    = ray_sock_peer_is_local((ray_sock_t)sel->fd);
    bool try_shm  = want == RAY_IPC_XPORT_SHM  || (want == RAY_IPC_XPORT_AUTO && local);
    bool try_rdma = want == RAY_IPC_XPORT_RDMA || (want == RAY_IPC_XPORT_AUTO && !local);
    int  miss     = want == RAY_IPC_XPORT_AUTO ? 0 : -7;
    ray_poll_buf_t* reply = NULL;

    if (try_shm) {
        const char* me = ray_shm_host_identity();
        if (!*me) return miss;
        uint8_t ask[1 + RAY_SHM_IDENTITY_LEN];
        ask[0] = XP_SHM_ASK;
        memcpy(ask + 1, me, RAY_SHM_IDENTITY_LEN);
        if (upgrade_exchange(poll, id, ask, sizeof ask, deadline, &reply) < 0) return -1;
        size_t fixed = 1 + XP_TOKEN_LEN + 8;
        if (reply_kind(reply) == XP_SHM_OFFER && (size_t)reply->offset > fixed + 1 &&
            (size_t)reply->offset <= fixed + XP_NAME_MAX &&
            reply->data[reply->offset - 1] == 0) {
            uint8_t token[XP_TOKEN_LEN];
            uint64_t ring;
            char name[XP_NAME_MAX];
            memcpy(token, reply->data + 1, XP_TOKEN_LEN);
            memcpy(&ring, reply->data + 1 + XP_TOKEN_LEN, 8);
            memcpy(name, reply->data + fixed, (size_t)reply->offset - fixed);
            ray_poll_buf_free(reply);
            reply = NULL;
            int fds[3];
            /* The server moves once the descriptors have gone: from then
             * on only a move of our own keeps the two in step.  A server
             * that could not be reached has not moved — a shared boot
             * identity across network namespaces, say — and the
             * connection stays on TCP where AUTO allows; its offer expires. */
            int fetched = shm_fetch_rings(name, token, fds, deadline);
            if (fetched == 0) return miss;
            if (fetched < 0) return -1;
            if (upgrade_wait(poll, id, deadline, &reply) < 0) {
                for (int i = 0; i < 3; i++) close(fds[i]);
                return -1;
            }
            uint8_t k = reply_kind(reply);
            ray_poll_buf_free(reply);
            ray_xport_t* x = ray_shm_attach(ring, fds);   /* consumes fds */
            if (k != XP_SWITCHED || !x) {
                if (x) x->ops->destroy(x);
                return -1;
            }
            return conn_move(poll, id, x) ? 0 : -1;
        }
        ray_poll_buf_free(reply);
        return miss;   /* refused: stay on TCP (AUTO never tries RDMA on one host) */
    }

    if (try_rdma) {
        if (!ray_rdma_available()) return miss;
        ray_rdma_record_t mine, peer;
        ray_xport_t* x = ray_rdma_create(&mine);
        if (!x) return miss;
        uint8_t ask[1 + sizeof mine];
        ask[0] = XP_RDMA;
        memcpy(ask + 1, &mine, sizeof mine);
        if (upgrade_exchange(poll, id, ask, sizeof ask, deadline, &reply) < 0) {
            x->ops->destroy(x);
            return -1;
        }
        if (reply_kind(reply) == XP_RDMA && (size_t)reply->offset == 1 + sizeof peer) {
            memcpy(&peer, reply->data + 1, sizeof peer);
            ray_poll_buf_free(reply);
            /* Two devices do not make one fabric: the first write to the
             * peer's ring is the proof, and it is tried before either side
             * moves.  Without it both stay on TCP (the server drops its
             * endpoint on the refusal). */
            int64_t left = deadline - ray_time_now_ms();
            int probe_ms = left > 3000 ? 3000 : left < 100 ? 100 : (int)left;
            bool reach = ray_rdma_connect(x, &peer) && ray_rdma_probe(x, probe_ms);
            uint8_t word = reach ? XP_READY : XP_REFUSE;
            sel = ray_poll_get(poll, id);
            if (!sel || conn_tx_drain_blocking(poll, sel, deadline) < 0 ||
                upgrade_send((ray_sock_t)sel->fd, &word, 1) < 0) {
                x->ops->destroy(x);
                return -1;
            }
            if (!reach) { x->ops->destroy(x); return miss; }
            if (upgrade_wait(poll, id, deadline, &reply) < 0) { x->ops->destroy(x); return -1; }
            uint8_t k = reply_kind(reply);
            ray_poll_buf_free(reply);
            if (k != XP_SWITCHED) { x->ops->destroy(x); return -1; }
            return conn_move(poll, id, x) ? 0 : -1;
        }
        ray_poll_buf_free(reply);
        x->ops->destroy(x);
        return miss;
    }
    return 0;
#else
    (void)deadline;
    return want == RAY_IPC_XPORT_AUTO ? 0 : -7;
#endif
}

int64_t ray_ipc_listen_at(ray_poll_t* poll, const char* host, uint16_t port)
{
    if (!poll) return -1;
    ipc_install_oob_cancel();   /* enable client Ctrl-C → SIGURG query cancel */

    ray_sock_t fd = ray_sock_listen_at(host, port);
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
#ifdef RAY_IPC_XPORT
    /* And the private Unix socket the same-host transport hands its
     * rings over.  Best effort: without it, no shared-memory offers. */
    shm_side_listen(poll);
#endif
    return id;
}

int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port)
{
    return ray_ipc_listen_at(poll, NULL, port);
}

/* ===== Connection-handle API =====
 *
 * One handle namespace: a handle is the poll selector id of an IPC
 * connection — inbound (accepted by a listener) or outbound (opened by
 * ray_ipc_connect).  Every entry point below resolves the handle in the
 * "active" poll (the poll dispatching the current hook/eval, else the
 * runtime's main poll), so server-side code can write to the very
 * handles its hooks receive — server-initiated push. */

static int64_t recv_full(ray_sock_t fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int64_t n = ray_sock_recv(fd, (uint8_t*)buf + total, len - total);
        if (n == 0) { errno = ECONNRESET; return -1; }   /* peer closed: not a timeout */
        if (n < 0) return -1;
        total += (size_t)n;
    }
    return (int64_t)total;
}

/* Resolve `handle` to a handshake-complete IPC connection selector in
 * the active poll.  Rejects anything else living in the selector table
 * (listeners, the REPL's stdin, conns still mid-handshake) so a stray
 * integer can never write to a non-IPC fd. */
static ray_selector_t* conn_resolve(ray_poll_t** poll_out, int64_t handle)
{
    ray_poll_t* poll = ray_ipc_active_poll();
    if (!poll) return NULL;
    ray_selector_t* sel = ray_poll_get(poll, handle);
    if (!sel || sel->type != RAY_SEL_SOCKET || !sel->data) return NULL;
    if (sel->rx.read_fn != ipc_read_header &&
        sel->rx.read_fn != ipc_read_payload) return NULL;
    if (poll_out) *poll_out = poll;
    return sel;
}

/* Drain whatever is available on one connection's socket through its rx
 * state machine WITHOUT blocking: fill the requested rx buffer, invoke
 * read_fn for each completed phase, repeat until the socket is dry.
 * Mirrors the per-event rx loop in the poll backends; used by sync
 * sends to wait for their RESP while still dispatching any interleaved
 * frames (pushed ASYNCs, nested SYNC requests).  Returns 0 when the
 * socket has no more data right now, -1 if the selector was
 * deregistered (peer closed or protocol error). */
static int conn_pump(ray_poll_t* poll, int64_t id)
{
    return ray_poll_serve_rx(poll, id);
}

/* Serialize + compress + frame one message into an immutable, reference-
 * counted frame (rc = 1 to the caller).  Built once per message however
 * many connections it goes to: ray_mcast_pub shares one frame across
 * every subscriber's queue (#487). */
static ray_poll_frame_t* conn_frame_msg(ray_t* msg, uint8_t msgtype,
                                        uint8_t extra_flags, size_t threshold,
                                        ray_err_t* err_out)
{
    if (err_out) *err_out = RAY_OK;
    int64_t ser_size = ray_serde_size(msg);
    if (ser_size <= 0) {
        if (err_out) *err_out = RAY_ERR_TYPE;
        return NULL;
    }

    uint8_t* payload = (uint8_t*)ray_alloc_raw((size_t)ser_size);
    if (!payload) {
        if (err_out) *err_out = RAY_ERR_OOM;
        return NULL;
    }
    if (ray_ser_raw(payload, msg) != ser_size) {
        ray_free_raw(payload);
        if (err_out) *err_out = RAY_ERR_IO;
        return NULL;
    }

    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > threshold) {
        uint8_t* comp = (uint8_t*)ray_alloc_raw((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress_at(payload, (size_t)ser_size,
                                              comp, (size_t)ser_size, threshold);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_alloc_raw(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_free_raw(comp);
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
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = msgtype,
        .size    = (int64_t)send_len,
    };

    size_t total = sizeof(hdr) + send_len;
    if (total > (size_t)RAY_IPC_FRAME_MAX_BYTES) {
        ray_free_raw(send_buf);
        if (payload) ray_free_raw(payload);
        if (err_out) *err_out = RAY_ERR_IO;
        return NULL;
    }

    ray_poll_frame_t* frame = ray_poll_frame_new((int64_t)total);
    if (!frame) {
        ray_free_raw(send_buf);
        if (payload) ray_free_raw(payload);
        if (err_out) *err_out = RAY_ERR_OOM;
        return NULL;
    }
    memcpy(frame->data, &hdr, sizeof(hdr));
    memcpy(frame->data + sizeof(hdr), send_buf, send_len);

    ray_free_raw(send_buf);
    if (payload) ray_free_raw(payload);
    return frame;
}

/* Serialize + frame + write one message to a connection's fd.
 * ray_sock_send loops on partial writes/EAGAIN internally, so this is
 * safe on the nonblocking fds the poll owns.  Writes directly and does NOT
 * consult sel->tx.buf, so a caller whose fd may carry a queued multicast
 * frame must conn_tx_drain_blocking() first (see sync_send /
 * ray_ipc_send_async).  Returns 0 on success, -1 on serialization or
 * socket failure. */
/* The compression policy of an established connection.  Prefer the value
 * cached on the conn data — it carries any explicit `compress` option from
 * .ipc.open, which re-deriving from the peer address would silently
 * discard — and fall back to link locality when there is no conn data. */
static size_t conn_threshold(ray_selector_t* sel)
{
    if (!sel) return (size_t)RAY_IPC_COMPRESS_THRESHOLD;
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    if (cd) return cd->compress_threshold;
    return ray_ipc_link_threshold((ray_sock_t)sel->fd);
}

static int64_t conn_write_msg(ray_selector_t* sel, ray_t* msg, uint8_t msgtype,
                              uint8_t extra_flags, size_t threshold,
                              int64_t deadline_ms)
{
    ray_err_t err = RAY_OK;
    ray_poll_frame_t* frame = conn_frame_msg(msg, msgtype, extra_flags,
                                             threshold, &err);
    if (!frame) return -1;
    int64_t rc = conn_send_all(sel, frame->data, (size_t)frame->size,
                               deadline_ms);
    ray_poll_frame_release(frame);
    return rc < 0 ? -1 : 0;
}

/* Classify a failed connect or handshake step by errno.  The connect
 * and the two-byte wire handshake share one budget: ray_sock_connect
 * installs it as SO_RCVTIMEO / SO_SNDTIMEO once the connect completes,
 * so a handshake that the peer never answers surfaces as EAGAIN /
 * EWOULDBLOCK — a server that is alive and listening but inside a long
 * evaluation.  That must not read as "refused", which is what a dead
 * port says, instantly.
 *   -5  no answer within the budget (ETIMEDOUT, EAGAIN, EWOULDBLOCK)
 *   -1  ECONNREFUSED
 *   -6  anything else; errno is left holding it for the caller's
 *       strerror. */
static int64_t connect_fail_code(int err) {
    if (err == ETIMEDOUT || err == EAGAIN) return -5;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (err == EWOULDBLOCK) return -5;
#endif
    if (err == ECONNREFUSED) return -1;
    errno = err;
    return -6;
}

int64_t ray_ipc_connect(const char* host, uint16_t port,
                         const char* user, const char* password,
                         int timeout_ms)
{
    return ray_ipc_connect_opts(host, port, user, password, timeout_ms,
                                RAY_IPC_COMPRESS_AUTO, RAY_IPC_KEEPALIVE_AUTO);
}

int64_t ray_ipc_connect_opts(const char* host, uint16_t port,
                         const char* user, const char* password,
                         int timeout_ms, size_t compress_threshold,
                         int keepalive_ms)
{
    ray_ipc_open_opts_t o = {
        .timeout_ms         = timeout_ms,
        .compress_threshold = compress_threshold,
        .keepalive_ms       = keepalive_ms,
        .transport          = RAY_IPC_XPORT_AUTO,
    };
    return ray_ipc_connect_with(host, port, user, password, &o);
}

int64_t ray_ipc_connect_with(const char* host, uint16_t port,
                             const char* user, const char* password,
                             const ray_ipc_open_opts_t* opts)
{
    int    timeout_ms         = opts->timeout_ms;
    size_t compress_threshold = opts->compress_threshold;
    int    keepalive_ms       = opts->keepalive_ms;
    int    want = opts->transport == RAY_IPC_XPORT_AUTO ? g_xport_policy : opts->transport;

    /* The connection lives in the active poll's selector table — its
     * selector id IS the handle.  No poll, no handle namespace: refuse
     * up front rather than hand out an integer nothing can resolve. */
    ray_poll_t* poll = ray_ipc_active_poll();
    if (!poll) return -1;

    /* Default the connect/handshake budget to 5s when the caller gives
     * no explicit timeout, matching the long-standing handshake timeout. */
    int connect_to = timeout_ms > 0 ? timeout_ms : 5000;
    ray_sock_t fd = ray_sock_connect(host, port, connect_to);
    if (fd == RAY_INVALID_SOCK) return connect_fail_code(errno);

    uint8_t hs[2] = { RAY_SERDE_WIRE_VERSION, our_caps(want) };
    if (ray_sock_send(fd, hs, 2) < 0) {
        int e = errno;                  /* close() may clobber it */
        ray_sock_close(fd);
        return connect_fail_code(e);
    }

    uint8_t resp[2];
    if (recv_full(fd, resp, 2) < 0) {
        int e = errno;
        ray_sock_close(fd);
        return connect_fail_code(e);
    }

    /* Refuse a peer that speaks a different wire version.  This gives
     * the new client an explicit error at connect time rather than
     * silently sending a v3 payload to a server that would misparse
     * every atom. */
    if (resp[0] != RAY_SERDE_WIRE_VERSION) {
        ray_sock_close(fd);
        return -4; /* wire version mismatch */
    }

    /* The high bits are the capabilities both sides share; an older
     * server leaves them zero.  The rest of the byte is the auth flag. */
    uint8_t peer_caps = resp[1] & RAY_IPC_CAP_MASK & our_caps(want);
    resp[1] &= (uint8_t)~RAY_IPC_CAP_MASK;

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

    /* The handshake timeouts are gone, so from here on only keepalive
     * notices a peer that vanished without closing (#589).  Best-effort:
     * failing to arm it leaves the link no worse than before. */
    if (keepalive_ms == RAY_IPC_KEEPALIVE_AUTO)
        ray_sock_set_keepalive(fd, RAY_IPC_KEEPALIVE_DEFAULT_MS, false);
    else if (keepalive_ms > 0)
        ray_sock_set_keepalive(fd, keepalive_ms, true);

    /* Handshake + auth done (blocking socket).  Hand the fd over to the
     * poll: nonblocking, parked on a header read, same read pump as an
     * inbound connection — which is exactly what makes pushed frames
     * from the server dispatchable on this side. */
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(fd); return -1; }
    memset(cd, 0, sizeof(*cd));
    cd->phase       = RAY_IPC_PHASE_HEADER;
    cd->listener_id = -1;               /* outbound: on.open never fires, on.close does */
    cd->restricted  = poll->restricted; /* -U narrows pushed evals too */
    cd->compress_threshold = (compress_threshold == RAY_IPC_COMPRESS_AUTO)
                           ? ray_ipc_link_threshold(fd)
                           : compress_threshold;
    cd->compress_explicit = compress_threshold != RAY_IPC_COMPRESS_AUTO;
    cd->peer_caps = peer_caps;

    ray_sock_set_nonblocking(fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.send_fn  = ipc_send_fn;
    reg.read_fn  = ipc_read_header;
    reg.close_fn = ipc_on_close;
    reg.data     = cd;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(fd);
        ray_sys_free(cd);
        return -1;
    }
    ray_selector_t* ns = ray_poll_get(poll, id);
    if (ns) ray_poll_rx_request(poll, ns, sizeof(ray_ipc_header_t));

    /* Registered and parked on a header read like any connection, so the
     * upgrade exchange below runs through the same rx pump: whatever the
     * server pushes meanwhile is dispatched, not lost. */
    if ((peer_caps & RAY_IPC_CAP_XPORT) || want != RAY_IPC_XPORT_AUTO) {
        int64_t deadline = ray_time_now_ms() + connect_to;
        int rc = client_upgrade(poll, id, want, deadline);
        if (rc < 0) {
            ray_selector_t* cur = ray_poll_get(poll, id);
            if (cur && cur->data == (void*)cd) ray_poll_deregister(poll, id);
            return rc == -7 ? -7 : -1;
        }
    }

    return id;
}

void ray_ipc_close(int64_t handle)
{
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return;
    /* Deregister fires ipc_on_close: socket closed, conn data freed,
     * and the on.close hook for inbound connections — so a server can
     * kick a client through the same handle its hooks were given. */
    ray_poll_deregister(poll, sel->id);
}

/* Sync round-trip on any connection handle.  Writes a SYNC frame, then
 * pumps this connection's rx machinery until the matching RESP frame is
 * deposited — dispatching (not swallowing) any frames that arrive in
 * between: a pushed ASYNC gets evaluated, a nested SYNC request from
 * the peer gets evaluated and answered.  Full-duplex, either side of
 * the wire.
 *
 * timeout_ms > 0 bounds the whole round trip, write included (#589).
 * On expiry the connection is closed — on.close fires — because a RESP
 * carries no request id: a reply that arrived later would be taken as
 * the answer to the NEXT send on the handle.  A write cut off by the
 * deadline has left a partial frame on the stream, which is unusable for
 * the same reason.  Before closing, the peer is sent the same urgent-data
 * cancel as Ctrl-C, so it stops evaluating a query nobody will read. */
static ray_t* sync_expire(ray_poll_t* poll, int64_t id, int64_t timeout_ms)
{
    ray_selector_t* sel = ray_poll_get(poll, id);
    if (sel) {
        ray_sock_send_oob((ray_sock_t)sel->fd, '!');
        ray_poll_deregister(poll, id);
    }
    return ray_error("io", "ipc send timed out after %lld ms; connection closed",
                     (long long)timeout_ms);
}

static ray_t* sync_send(int64_t handle, ray_t* msg, uint8_t extra_flags,
                        int64_t timeout_ms)
{
    int64_t deadline = timeout_ms > 0 ? ray_time_now_ms() + timeout_ms : -1;

    bool owned = false;
    if (ray_is_lazy(msg)) {
        ray_retain(msg);
        msg = ray_lazy_materialize(msg); /* consumes the retain */
        if (RAY_IS_ERR(msg)) return msg;
        owned = true;
    }

    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) {
        if (owned) ray_release(msg);
        return ray_error("io", "connection closed");
    }
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    if (cd->sync_waiting) {
        /* A sync wait is already pumping this conn further down the
         * stack (hook → nested send on the SAME handle).  Two waiters
         * can't both claim the next RESP — refuse the inner one. */
        if (owned) ray_release(msg);
        return ray_error("io", "nested sync send on busy handle");
    }

    /* Drain any half-sent multicast frame queued to this fd before writing the
     * sync request, so its bytes can't interleave into the pending frame. */
    errno = 0;
    if (conn_tx_drain_blocking(poll, sel, deadline) < 0 ||
        conn_write_msg(sel, msg, RAY_IPC_MSG_SYNC,
                       extra_flags, conn_threshold(sel), deadline) < 0) {
        int e = errno;
        if (owned) ray_release(msg);
        if (deadline >= 0 && e == ETIMEDOUT)
            return sync_expire(poll, sel->id, timeout_ms);
        return ray_error("io", "ipc send failed");
    }
    if (owned) ray_release(msg);

    cd->sync_waiting = true;
    cd->sync_ready   = false;
    cd->sync_resp    = NULL;
    int64_t id = sel->id;

    for (;;) {
        /* Process anything already buffered/readable first, then block
         * for more.  The pump can deregister the selector (peer died) —
         * conn data is freed with it, so re-resolve every iteration.
         * The data-pointer compare also guards against this id being
         * reused by a NEW connection opened inside a dispatched eval:
         * that conn's state must not be mistaken for ours. */
        int rc = conn_pump(poll, id);
        sel = ray_poll_get(poll, id);
        if (!sel || sel->data != (void*)cd)
            return ray_error("io", "connection closed");
        if (cd->sync_ready) {
            ray_t* result = cd->sync_resp;
            cd->sync_resp    = NULL;
            cd->sync_ready   = false;
            cd->sync_waiting = false;
            if (!result)
                return ray_error("io", "ipc bad response");
            return result;
        }
        if (rc < 0) {
            /* Deregistered but selector still resolves?  Can't happen —
             * defensive: treat as closed. */
            return ray_error("io", "connection closed");
        }
        /* The deadline is checked after the pump, so a reply that is
         * already here when it expires is still delivered. */
        int wait_ms = -1;
        if (deadline >= 0) {
            int64_t left = deadline - ray_time_now_ms();
            if (left <= 0) return sync_expire(poll, id, timeout_ms);
            wait_ms = left > INT_MAX ? INT_MAX : (int)left;
        }
        int w = conn_wait_readable(sel, wait_ms);
        if (w == -3) {
            /* The socket of an upgraded connection spoke: the peer is
             * gone.  Deregistering frees cd; nothing of it is touched. */
            ray_poll_deregister(poll, id);
            return ray_error("io", "connection closed");
        }
        if (w == -2) {
            /* Interrupted by a signal.  If it was a cancel request (Ctrl-C),
             * forward it to the server as TCP urgent data — the server is busy
             * in eval and not reading the normal stream, but OOB raises SIGURG
             * there and cancels the running query.  Then keep awaiting its
             * cancel reply.  Best-effort: a server without OOB-cancel support
             * ignores the byte and the query completes as before. */
            if (ray_interrupted()) {
                ray_sock_send_oob((ray_sock_t)sel->fd, '!');
                ray_clear_interrupt();  /* consume; do not resend or cancel locally */
            }
            continue;
        }
        if (w < 0) {
            cd->sync_waiting = false;
            return ray_error("io", "ipc recv failed");
        }
    }
}

ray_t* ray_ipc_send(int64_t handle, ray_t* msg)
{
    return sync_send(handle, msg, 0, 0);
}

ray_t* ray_ipc_send_timeout(int64_t handle, ray_t* msg, int64_t timeout_ms)
{
    return sync_send(handle, msg, 0, timeout_ms);
}

size_t ray_ipc_handle_threshold(int64_t handle)
{
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return RAY_IPC_COMPRESS_AUTO;
    return conn_threshold(sel);
}

ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        ray_retain(msg);
        msg = ray_lazy_materialize(msg); /* consumes the retain */
        if (RAY_IS_ERR(msg)) {
            ray_err_t code = ray_err_from_obj(msg);
            ray_error_free(msg);
            return code;
        }
        owned = true;
    }
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    ray_err_t rc = (!sel || conn_tx_drain_blocking(poll, sel, -1) < 0 ||
                    conn_write_msg(sel, msg,
                                   RAY_IPC_MSG_ASYNC, 0,
                                   conn_threshold(sel), -1) < 0)
                   ? RAY_ERR_IO : RAY_OK;
    if (owned) ray_release(msg);
    return rc;
}

static void conn_tx_limits(ray_selector_t* sel, int64_t* bytes, int64_t* frames)
{
    *bytes  = (sel && sel->tx.limit_bytes  > 0) ? sel->tx.limit_bytes  : g_tx_limit_bytes;
    *frames = (sel && sel->tx.limit_frames > 0) ? sel->tx.limit_frames : g_tx_limit_frames;
}

/* Bytes still to be written across the queue, and the number of queued
 * frames.  Saturates at INT64_MAX rather than overflowing. */
static int64_t conn_tx_pending(ray_selector_t* sel, int64_t* frames_out)
{
    int64_t pending = 0, frames = 0;
    for (ray_poll_buf_t* b = sel ? sel->tx.buf : NULL; b; b = b->next) {
        if (b->size > b->offset) {
            int64_t rem = b->size - b->offset;
            pending = (pending > INT64_MAX - rem) ? INT64_MAX : pending + rem;
            frames++;
        }
    }
    if (frames_out) *frames_out = frames;
    return pending;
}

/* Admit `frame` to the queue if the configured backlog allows it. */
static int conn_tx_append(ray_poll_t* poll, ray_selector_t* sel, ray_poll_buf_t* frame)
{
    if (!sel || !frame || frame->offset > frame->size) return -1;

    int64_t add = frame->size - frame->offset;
    int64_t limit_bytes, limit_frames;
    conn_tx_limits(sel, &limit_bytes, &limit_frames);
    int64_t frames = 0;
    int64_t pending = conn_tx_pending(sel, &frames);
    if (add < 0 || pending > limit_bytes - add) return -1;
    if (limit_frames > 0 && frames + 1 > limit_frames) return -1;

    ray_poll_buf_t** tail = &sel->tx.buf;
    while (*tail) tail = &(*tail)->next;
    *tail = frame;

    int64_t now = pending + add;
    if (now > sel->tx.hwm_bytes) sel->tx.hwm_bytes = now;
    if (poll && now > poll->tx_hwm_bytes) poll->tx_hwm_bytes = now;
    return 0;
}

static int conn_try_send_frame(ray_selector_t* sel, ray_poll_buf_t* frame)
{
    while (frame->offset < frame->size) {
        int64_t nw = ray_poll_sel_send(sel,
            frame->data + frame->offset,
            frame->size - frame->offset);
        if (nw < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return -1;
        }
        if (nw == 0) return -1;
        frame->offset += nw;
    }
    return 0;
}

ray_err_t ray_ipc_frame_async(ray_t* msg, ray_poll_frame_t** out)
{
    return ray_ipc_frame_async_at(msg, (size_t)RAY_IPC_COMPRESS_THRESHOLD, out);
}

ray_err_t ray_ipc_frame_async_at(ray_t* msg, size_t compress_threshold,
                                 ray_poll_frame_t** out)
{
    if (!out) return RAY_ERR_TYPE;
    *out = NULL;
    bool owned = false;
    if (ray_is_lazy(msg)) {
        ray_retain(msg);
        msg = ray_lazy_materialize(msg);
        if (RAY_IS_ERR(msg)) {
            ray_err_t code = ray_err_from_obj(msg);
            ray_error_free(msg);
            return code;
        }
        owned = true;
    }
    ray_err_t err = RAY_OK;
    /* A frame is shared by every subscriber that wants this threshold
     * (#487), so the policy belongs to the framing, not to the peer: the
     * caller groups its subscribers by threshold and asks for one framing
     * per distinct value (#551). */
    ray_poll_frame_t* frame = conn_frame_msg(msg, RAY_IPC_MSG_ASYNC, 0,
                                             compress_threshold, &err);
    if (owned) ray_release(msg);
    if (!frame) return err == RAY_OK ? RAY_ERR_IO : err;
    *out = frame;
    return RAY_OK;
}

ray_err_t ray_ipc_try_send_frame(int64_t handle, ray_poll_frame_t* frame)
{
    if (!frame) return RAY_ERR_TYPE;
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return RAY_ERR_IO;

    /* The queue node holds its own reference; the caller keeps its own. */
    ray_poll_buf_t* node = ray_poll_buf_from_frame(frame);
    if (!node) return RAY_ERR_OOM;

    int rc = 0;
    if (sel->tx.buf) {
        rc = 1;                         /* keep ordering behind queued bytes */
    } else {
        rc = conn_try_send_frame(sel, node);
    }

    if (rc == 0) {
        ray_poll_buf_free(node);
    } else if (rc > 0) {
        if (conn_tx_append(poll, sel, node) < 0) {
            ray_poll_buf_free(node);
            return RAY_ERR_IO;
        }
        ray_poll_tx_request(poll, sel);
    } else {
        ray_poll_buf_free(node);
    }
    return rc >= 0 ? RAY_OK : RAY_ERR_IO;
}

void ray_ipc_tx_limit_get(int64_t* bytes, int64_t* frames)
{
    if (bytes)  *bytes  = g_tx_limit_bytes;
    if (frames) *frames = g_tx_limit_frames;
}

ray_err_t ray_ipc_tx_limit_set(int64_t bytes, int64_t frames)
{
    if (bytes < RAY_IPC_TX_MIN_BYTES || frames < 0) return RAY_ERR_DOMAIN;
    g_tx_limit_bytes  = bytes;
    g_tx_limit_frames = frames;
    return RAY_OK;
}

ray_err_t ray_ipc_tx_limit_set_handle(int64_t handle, int64_t bytes, int64_t frames)
{
    if (bytes < RAY_IPC_TX_MIN_BYTES || frames < 0) return RAY_ERR_DOMAIN;
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return RAY_ERR_IO;
    sel->tx.limit_bytes  = bytes;
    sel->tx.limit_frames = frames;
    return RAY_OK;
}

ray_err_t ray_ipc_tx_info(int64_t handle, ray_ipc_tx_info_t* out)
{
    if (!out) return RAY_ERR_TYPE;
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return RAY_ERR_IO;
    conn_tx_limits(sel, &out->limit_bytes, &out->limit_frames);
    out->queued_bytes = conn_tx_pending(sel, &out->queued_frames);
    out->hwm_bytes    = sel->tx.hwm_bytes;
    out->inbound      = sel->data &&
                        ((ray_ipc_conn_data_t*)sel->data)->listener_id >= 0;
    out->transport    = sel->xport ? sel->xport->kind : RAY_XPORT_TCP;
    return RAY_OK;
}

int64_t ray_ipc_tx_hwm_bytes(ray_poll_t* poll)
{
    return poll ? poll->tx_hwm_bytes : 0;
}

ray_err_t ray_ipc_try_send_async(int64_t handle, ray_t* msg)
{
    ray_poll_frame_t* frame = NULL;
    ray_err_t err = ray_ipc_frame_async(msg, &frame);
    if (err != RAY_OK) return err;
    err = ray_ipc_try_send_frame(handle, frame);
    ray_poll_frame_release(frame);
    return err;
}

/* Verbose-eval sync send: sets RAY_IPC_FLAG_VERBOSE on the outbound
 * header so the peer captures whatever the eval writes to
 * stdout/stderr and returns a 2-element list [captured_str, result]
 * instead of bare result.  Same wire path as ray_ipc_send otherwise. */
ray_t* ray_ipc_send_verbose(int64_t handle, ray_t* msg)
{
    return sync_send(handle, msg, RAY_IPC_FLAG_VERBOSE, 0);
}

/* ===== .ipc.open options ===== */

/* Read one optional integer field.  Returns 1 if present, 0 if absent,
 * -1 on a type error (with *err set). */
static int opts_i64(ray_t* d, const char* name, int64_t* out, ray_t** err)
{
    ray_t* key = ray_sym(ray_sym_intern(name, strlen(name)));
    if (!key) { *err = ray_error("oom", ".ipc.open options"); return -1; }
    ray_t* v = ray_dict_get(d, key);
    ray_release(key);
    if (!v) return 0;
    if (!ray_is_atom(v) || (v->type != -RAY_I64 && v->type != -RAY_I32)) {
        *err = ray_error("type", ".ipc.open option `%s` must be an integer, got %s",
                         name, ray_type_name(v->type));
        ray_release(v);
        return -1;
    }
    *out = (v->type == -RAY_I64) ? v->i64 : (int64_t)v->i32;
    ray_release(v);
    return 1;
}

ray_t* ray_ipc_parse_open_opts(ray_t* arg, int* timeout_ms,
                               size_t* compress_threshold,
                               int* keepalive_ms)
{
    ray_ipc_open_opts_t o;
    ray_t* err = ray_ipc_parse_open_opts_full(arg, &o);
    *timeout_ms         = o.timeout_ms;
    *compress_threshold = o.compress_threshold;
    *keepalive_ms       = o.keepalive_ms;
    return err;
}

ray_t* ray_ipc_parse_open_opts_full(ray_t* arg, ray_ipc_open_opts_t* o)
{
    int*    timeout_ms         = &o->timeout_ms;
    size_t* compress_threshold = &o->compress_threshold;
    int*    keepalive_ms       = &o->keepalive_ms;
    *timeout_ms         = 0;
    *compress_threshold = RAY_IPC_COMPRESS_AUTO;
    *keepalive_ms       = RAY_IPC_KEEPALIVE_AUTO;
    o->transport        = RAY_IPC_XPORT_AUTO;
    if (!arg) return NULL;

    /* Backwards-compatible form: a bare integer is the connect timeout. */
    if (ray_is_atom(arg) && (arg->type == -RAY_I64 || arg->type == -RAY_I32)) {
        int64_t tv = (arg->type == -RAY_I64) ? arg->i64 : (int64_t)arg->i32;
        if (tv == NULL_I64) return NULL;             /* 0N -> default budget */
        if (tv < 0)
            return ray_error("domain", ".ipc.open timeout must be >= 0, got %lld",
                             (long long)tv);
        *timeout_ms = (tv > INT_MAX) ? INT_MAX : (int)tv;
        return NULL;
    }

    if (arg->type != RAY_DICT)
        return ray_error("type", ".ipc.open expects an integer timeout or an options dict, got %s",
                         ray_type_name(arg->type));

    /* Reject unknown keys: a typo must not silently mean "default".
     *
     * Every key must be a symbol and must be validated.  ray_dict_find_idx
     * returns -1 on a key-type mismatch rather than erroring, so a dict
     * keyed by anything else would make both lookups miss and this
     * function return success with defaults — reinstating the very silent
     * default this check exists to prevent, and swallowing a typo with
     * it. */
    ray_t* keys = ray_dict_keys(arg);
    if (keys && keys->len > 0) {
        for (int64_t i = 0; i < keys->len; i++) {
            const char* p = NULL;
            size_t      n = 0;
            if (keys->type == RAY_SYM) {
                const int64_t* kd = (const int64_t*)ray_data(keys);
                ray_t* ks = ray_sym_str(kd[i]);
                if (ks) { p = ray_str_ptr(ks); n = ray_str_len(ks); }
            } else if (keys->type == RAY_LIST) {
                ray_t* ke = ((ray_t**)ray_data(keys))[i];
                if (!ke || ke->type != -RAY_SYM)
                    return ray_error("type", ".ipc.open: option keys must be symbols, got %s",
                                     ke ? ray_type_name(ke->type) : "null");
                ray_t* ks = ray_sym_str(ke->i64);
                if (ks) { p = ray_str_ptr(ks); n = ray_str_len(ks); }
            } else {
                return ray_error("type", ".ipc.open: option keys must be symbols, got %s",
                                 ray_type_name(keys->type));
            }
            bool known = (n == 7 && memcmp(p, "timeout",   7) == 0)
                      || (n == 8 && memcmp(p, "compress",  8) == 0)
                      || (n == 9 && memcmp(p, "keepalive", 9) == 0)
                      || (n == 9 && memcmp(p, "transport", 9) == 0);
            if (!known)
                return ray_error("domain", ".ipc.open: unknown option `%.*s` (expected `timeout`, `compress`, `keepalive` or `transport`)",
                                 (int)n, p ? p : "");
        }
    }

    ray_t*  err = NULL;
    int64_t tv  = 0;
    int     got = opts_i64(arg, "timeout", &tv, &err);
    if (got < 0) return err;
    if (got == 1 && tv != NULL_I64) {
        if (tv < 0)
            return ray_error("domain", ".ipc.open timeout must be >= 0, got %lld",
                             (long long)tv);
        *timeout_ms = (tv > INT_MAX) ? INT_MAX : (int)tv;
    }

    int64_t cv = 0;
    got = opts_i64(arg, "compress", &cv, &err);
    if (got < 0) return err;
    if (got == 1) {
        /* 0N = never.  0 = always.  n = compress payloads larger than n. */
        if (cv == NULL_I64)      *compress_threshold = RAY_IPC_COMPRESS_NEVER;
        else if (cv < 0)         return ray_error("domain", ".ipc.open compress threshold must be >= 0 or 0N (never), got %lld", (long long)cv);
        else                     *compress_threshold = (size_t)cv;
    }

    int64_t kv = 0;
    got = opts_i64(arg, "keepalive", &kv, &err);
    if (got < 0) return err;
    if (got == 1) {
        /* 0N = off.  n > 0 = dead-peer budget in ms.  0 is refused rather
         * than guessed at: it could mean "off" or "immediately". */
        if (kv == NULL_I64)      *keepalive_ms = RAY_IPC_KEEPALIVE_OFF;
        else if (kv <= 0)        return ray_error("domain", ".ipc.open keepalive must be > 0 or 0N (off), got %lld", (long long)kv);
        else                     *keepalive_ms = (kv > INT_MAX) ? INT_MAX : (int)kv;
    }

    /* `transport`: a symbol naming the policy for this connection. */
    {
        ray_t* key = ray_sym(ray_sym_intern("transport", 9));
        if (!key) return ray_error("oom", ".ipc.open options");
        ray_t* v = ray_dict_get(arg, key);
        ray_release(key);
        if (v) {
            int policy = -1;
            if (ray_is_atom(v) && v->type == -RAY_SYM) {
                ray_t* vs = ray_sym_str(v->i64);
                if (vs) policy = ray_ipc_xport_policy_parse(ray_str_ptr(vs), ray_str_len(vs));
            }
            ray_release(v);
            if (policy < 0)
                return ray_error("domain", ".ipc.open transport must be `auto`, `tcp`, `shm` or `rdma`");
            o->transport = policy;
        }
    }
    return NULL;
}

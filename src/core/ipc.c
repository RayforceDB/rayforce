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
#include <signal.h>

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

#include "core/runtime.h"
#include "lang/eval.h"
#include "lang/format.h"   /* ray_fmt — query text for the stats log */
#include "core/qlog.h"     /* query-statistics ring */
#include "lang/env.h"
#include "lang/internal.h"
#include "table/sym.h"

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

static ray_poll_t* ipc_active_poll(void) {
    ray_poll_t* p = ipc_ctx_poll();
    return p ? p : (ray_poll_t*)ray_runtime_get_poll();
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

/* Call a single-arg hook for lifecycle events (on.open / on.close).
 * Errors are logged and swallowed — a buggy logging hook must never
 * wedge connection teardown.  `poll` is the poll the connection lives
 * in, exposed thread-locally so the hook body can use the handle with
 * `.ipc.post` / `.ipc.send` / `.ipc.close`; the legacy server path
 * passes NULL (its conn-index handles are not selector ids). */
static void hook_call_lifecycle(ray_poll_t* poll, int idx, int64_t handle) {
    ray_t* fn = hook_lookup(idx);
    if (!fn) return;
    ray_t* arg = make_i64(handle);
    if (!arg || RAY_IS_ERR(arg)) { if (arg) ray_release(arg); return; }
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
}

/* Call the on.auth hook with (user, pass) string atoms.  Returns:
 *  -  1 → hook ran and returned truthy; caller continues the handshake.
 *  -  0 → hook ran and returned falsy (or errored); caller rejects.
 *  - -1 → no hook installed; caller uses the existing pass-through.
 * The constant-time secret compare in validate_creds always runs first,
 * so this hook can only narrow access — never widen it. */
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

static void send_response(ray_sock_t fd, ray_t* result)
{
    int64_t ser_size = ray_serde_size(result);

    /* A result we cannot serialize must never leave the client waiting on
     * a reply that never arrives.  Substitute a serializable error so the
     * caller observes a clean failure instead of a silent infinite hang
     * (issue #285).  ray_error frames serialize, so the substitute always
     * goes out unless even that fails — in which case there is nothing we
     * can put on the wire and we drop as before. */
    ray_t* fallback = NULL;
    if (ser_size <= 0) {
        fallback = ray_error("type", "result of type %s is not serializable over IPC",
                             result ? ray_type_name(result->type) : "null");
        result   = fallback;
        ser_size = ray_serde_size(result);
        if (ser_size <= 0) { if (fallback) ray_error_free(fallback); return; }
    }

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)ser_size);
    if (!payload) { if (fallback) ray_error_free(fallback); return; }
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
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = RAY_IPC_MSG_RESP,
        .size    = (int64_t)send_len,
    };
    ray_sock_send(fd, &hdr, sizeof(hdr));
    ray_sock_send(fd, send_buf, send_len);

    ray_sys_free(send_buf);
    if (payload) ray_sys_free(payload);
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
    if (msg && !RAY_IS_ERR(msg)) {
        /* Dispatch through `.ipc.on.sync` / `.ipc.on.async` hook if
         * installed; otherwise fall back to v1's inline-eval default.
         * The hook receives the raw deserialised payload — same shape
         * any other Rayfall lambda would see — so a hook installed
         * as `{[m] eval m}` reproduces the default behaviour. */
        int hook_idx = (hdr->msgtype == RAY_IPC_MSG_SYNC) ? IPC_HOOK_SYNC
                                                           : IPC_HOOK_ASYNC;
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

/* Per-connection state stored in selector->data.  Used for BOTH
 * directions: inbound conns accepted by a listener and outbound conns
 * registered by ray_ipc_connect — the selector id is the one handle
 * namespace either side of the wire works with. */
typedef struct {
    ray_ipc_header_t hdr;
    uint8_t          phase;
    int64_t          listener_id;  /* id of the listener selector; -1 = outbound */
    bool             auth_required;  /* server has -u/-U */
    bool             restricted;     /* server has -U */
    /* Sync round-trip state: while a ray_ipc_send waits on this conn it
     * pumps the rx machinery itself; a RESP frame is deposited here
     * instead of being evaluated.  Interleaved ASYNC/SYNC frames still
     * dispatch normally during the wait. */
    bool             sync_waiting;
    bool             sync_ready;
    ray_t*           sync_resp;
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
        if (cur && cur->data == (void*)cd)
            send_response((ray_sock_t)cur->fd, result);
    }
    if (result != RAY_NULL_OBJ) ray_release(result);

    return NULL;
}

static void ipc_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll;
    /* Fire `.ipc.on.close` BEFORE tearing the per-conn state down so a
     * hook reading `.ipc.handle` still sees this connection's id, and
     * before the listener's own close path (which would otherwise also
     * route through here) runs the hook with a stale fd.  Guard on:
     *   - sel->data: the listener itself has no conn data.
     *   - listener_id ≥ 0: lifecycle hooks pair with inbound on.open
     *     only — outbound conns (ray_ipc_connect) never fired on.open,
     *     so they must not fire on.close either.
     *   - phase ≥ HEADER: the connection actually completed handshake
     *     (otherwise no matching on.open was fired, so on.close must
     *     also stay silent to keep the pair balanced for the user). */
    if (sel->data) {
        ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
        if (cd->listener_id >= 0 &&
            (cd->phase == RAY_IPC_PHASE_HEADER ||
             cd->phase == RAY_IPC_PHASE_PAYLOAD)) {
            hook_call_lifecycle(poll, IPC_HOOK_CLOSE, sel->id);
        }
        /* A RESP deposited for a sync wait that never consumed it
         * (connection died mid-round-trip) would otherwise leak. */
        if (cd->sync_resp) {
            if (cd->sync_resp != RAY_NULL_OBJ) ray_release(cd->sync_resp);
            cd->sync_resp = NULL;
        }
        ray_sys_free(sel->data);
        sel->data = NULL;
    }
    ray_sock_close((ray_sock_t)sel->fd);
}

int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port)
{
    if (!poll) return -1;
    ipc_install_oob_cancel();   /* enable client Ctrl-C → SIGURG query cancel */

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
    /* `.ipc.on.close` fires only for conns that were actually opened —
     * a slot whose phase never advanced past HANDSHAKE/CREDS was never
     * announced via on.open and so shouldn't be announced via on.close.
     * Keeps the pair balanced for the user. */
    if (c->phase == RAY_IPC_PHASE_HEADER ||
        c->phase == RAY_IPC_PHASE_PAYLOAD) {
        hook_call_lifecycle(NULL, IPC_HOOK_CLOSE, (int64_t)(c - srv->conns));
    }

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
    /* Legacy path mirror of the poll-path post-handshake fire. */
    hook_call_lifecycle(NULL, IPC_HOOK_OPEN, (int64_t)(c - srv->conns));
}

static void conn_on_header(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    memcpy(&c->hdr, c->rx_buf, sizeof(ray_ipc_header_t));

    if (c->hdr.prefix != RAY_SERDE_PREFIX) { conn_close(srv, c); return; }
    if (c->hdr.version != RAY_SERDE_WIRE_VERSION) { conn_close(srv, c); return; }
    if (c->hdr.endian != RAY_SERDE_ENDIAN) { conn_close(srv, c); return; }
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

    /* Conn-array index doubles as the handle on the legacy path —
     * stable for the connection's lifetime, distinct across active
     * connections, freed back to the pool on close.  Mirrored shape
     * of the poll path's sel->id. */
    int64_t prev_handle = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set((int64_t)(c - srv->conns), prev_poll);

    ray_t* result = eval_payload(c->rx_buf, c->rx_len, &c->hdr);

    ipc_ctx_set(prev_handle, prev_poll);
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

    /* Legacy path mirror of the poll-path on.auth call: same handle-as-
     * conn-index convention, same narrowing semantics. */
    if (ok) {
        int hook_ok = hook_call_auth(NULL, (int64_t)(c - srv->conns),
                                     c->rx_buf + 1, cred_len);
        if (hook_ok == 0) ok = false;
    }

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
    hook_call_lifecycle(NULL, IPC_HOOK_OPEN, (int64_t)(c - srv->conns));
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
        if (n <= 0) return -1;
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
    ray_poll_t* poll = ipc_active_poll();
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
    for (;;) {
        ray_selector_t* sel = ray_poll_get(poll, id);
        if (!sel) return -1;
        if (!sel->rx.buf || !sel->rx.recv_fn || !sel->rx.read_fn) return 0;
        while (sel->rx.buf->offset < sel->rx.buf->size) {
            int64_t nr = sel->rx.recv_fn(sel->fd,
                                         sel->rx.buf->data + sel->rx.buf->offset,
                                         sel->rx.buf->size - sel->rx.buf->offset);
            if (nr <= 0) {
                if (nr < 0 && errno == EINTR) continue;
                if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return 0;  /* drained — caller decides whether to wait */
                ray_poll_deregister(poll, id);  /* error or peer closed */
                return -1;
            }
            sel->rx.buf->offset += nr;
        }
        /* Phase buffer complete — advance the state machine.  read_fn
         * may deregister the selector or re-arm it for the next phase;
         * the loop re-validates from scratch either way. */
        sel->rx.read_fn(poll, sel);
    }
}

/* Serialize + frame + write one message to a connection's fd.
 * ray_sock_send loops on partial writes/EAGAIN internally, so this is
 * safe on the nonblocking fds the poll owns.  Returns 0 on success,
 * -1 on serialization or socket failure. */
static int64_t conn_write_msg(ray_sock_t fd, ray_t* msg, uint8_t msgtype,
                              uint8_t extra_flags)
{
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
        .endian  = RAY_SERDE_ENDIAN,
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
                         const char* user, const char* password,
                         int timeout_ms)
{
    /* The connection lives in the active poll's selector table — its
     * selector id IS the handle.  No poll, no handle namespace: refuse
     * up front rather than hand out an integer nothing can resolve. */
    ray_poll_t* poll = ipc_active_poll();
    if (!poll) return -1;

    /* Default the connect/handshake budget to 5s when the caller gives
     * no explicit timeout, matching the long-standing handshake timeout. */
    int connect_to = timeout_ms > 0 ? timeout_ms : 5000;
    ray_sock_t fd = ray_sock_connect(host, port, connect_to);
    if (fd == RAY_INVALID_SOCK) return (errno == ETIMEDOUT) ? -5 : -1;

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

    /* Handshake + auth done (blocking socket).  Hand the fd over to the
     * poll: nonblocking, parked on a header read, same read pump as an
     * inbound connection — which is exactly what makes pushed frames
     * from the server dispatchable on this side. */
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(fd); return -1; }
    memset(cd, 0, sizeof(*cd));
    cd->phase       = RAY_IPC_PHASE_HEADER;
    cd->listener_id = -1;               /* outbound — no lifecycle hooks */
    cd->restricted  = poll->restricted; /* -U narrows pushed evals too */

    ray_sock_set_nonblocking(fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
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
 * the wire. */
static ray_t* sync_send(int64_t handle, ray_t* msg, uint8_t extra_flags)
{
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

    if (conn_write_msg((ray_sock_t)sel->fd, msg, RAY_IPC_MSG_SYNC,
                       extra_flags) < 0) {
        if (owned) ray_release(msg);
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
        int w = ray_sock_wait_readable_intr((ray_sock_t)sel->fd, -1);
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
    return sync_send(handle, msg, 0);
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
    ray_selector_t* sel = conn_resolve(NULL, handle);
    ray_err_t rc = (!sel || conn_write_msg((ray_sock_t)sel->fd, msg,
                                           RAY_IPC_MSG_ASYNC, 0) < 0)
                   ? RAY_ERR_IO : RAY_OK;
    if (owned) ray_release(msg);
    return rc;
}

/* Verbose-eval sync send: sets RAY_IPC_FLAG_VERBOSE on the outbound
 * header so the peer captures whatever the eval writes to
 * stdout/stderr and returns a 2-element list [captured_str, result]
 * instead of bare result.  Same wire path as ray_ipc_send otherwise. */
ray_t* ray_ipc_send_verbose(int64_t handle, ray_t* msg)
{
    return sync_send(handle, msg, RAY_IPC_FLAG_VERBOSE);
}

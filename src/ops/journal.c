/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "journal.h"
#include "store/journal.h"
#include "store/serde.h"
#include "core/ipc.h"
#include "mem/sys.h"
#include "ops/ops.h"

#include <string.h>

/* Copy a Rayfall string atom into a NUL-terminated C buffer.  Returns
 * NULL if the atom isn't a string or doesn't fit. */
static const char* str_to_cpath(ray_t* s, char* buf, size_t bufsz) {
    if (!s || s->type != -RAY_STR) return NULL;
    const char* p = ray_str_ptr(s);
    size_t      n = ray_str_len(s);
    if (n + 1 > bufsz) return NULL;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return buf;
}

/* Map a ray_err_t into a Rayfall error object so callers can `try` them. */
static ray_t* err_to_ray(ray_err_t e, const char* fallback) {
    if (e == RAY_OK) return RAY_NULL_OBJ;
    const char* code = ray_err_code_str(e);
    return ray_error(code ? code : (fallback ? fallback : "io"), NULL);
}

/* (.log.open args) — args is a 2-tuple (`async; "base") or (`sync; "base").
 * Accepting the mode as a sym keyword keeps the call self-documenting
 * without needing a second function or a magic int. */
ray_t* ray_log_open_fn(ray_t** args, int64_t n) {
#ifdef RAY_FUZZING
    /* No journal files opened for writing at attacker-controlled paths. */
    (void)args; (void)n;
    return ray_error("restricted", "log.open disabled under fuzzing");
#endif
    if (n != 2)
        return ray_error("rank", ".log.open expects (`async|`sync; \"base\")");
    if (!args[0] || args[0]->type != -RAY_SYM)
        return ray_error("type", ".log.open mode must be `async or `sync");
    if (!args[1] || args[1]->type != -RAY_STR)
        return ray_error("type", ".log.open base must be a string");

    int64_t sym_async = ray_sym_intern("async", 5);
    int64_t sym_sync  = ray_sym_intern("sync",  4);
    int64_t mode_id   = args[0]->i64;
    ray_journal_mode_t mode;
    if      (mode_id == sym_async) mode = RAY_JOURNAL_ASYNC;
    else if (mode_id == sym_sync)  mode = RAY_JOURNAL_SYNC;
    else return ray_error("domain", ".log.open mode must be `async or `sync");

    char base[1024];
    if (!str_to_cpath(args[1], base, sizeof(base)))
        return ray_error("type", ".log.open base path too long or not a string");

    ray_err_t e = ray_journal_open_append(base, mode);
    return err_to_ray(e, "io");
}

/* (.log.write expr) — append a synthetic entry containing the
 * serialized form of `expr`.  Useful for users who want REPL-driven
 * mutations captured in the log alongside the IPC stream.
 *
 * If the journal isn't open, ERROR rather than silently no-op — a
 * silent no-op would lie to the user about durability ("I logged
 * your change") when in fact nothing was persisted. */
ray_t* ray_log_write_fn(ray_t* expr) {
    if (!ray_journal_is_open())
        return ray_error("noopen", ".log.write: no journal open (start with -l/-L)");
    if (ray_ipc_auto_journal_eval())
        return ray_error("domain", ".log.write is disabled inside auto-journaled IPC eval");
    if (!expr) return ray_error("type", ".log.write expects an argument");

    bool owned = false;
    if (ray_is_lazy(expr)) {
        ray_retain(expr);
        expr = ray_lazy_materialize(expr); /* consumes the retain */
        if (RAY_IS_ERR(expr)) return expr;
        owned = true;
    }

    int64_t pay_size = ray_serde_size(expr);
    if (pay_size <= 0) {
        if (owned) ray_release(expr);
        return ray_error("domain", ".log.write: serde size 0");
    }

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)pay_size);
    if (!payload) {
        if (owned) ray_release(expr);
        return ray_error("oom", NULL);
    }

    int64_t written = ray_ser_raw(payload, expr);
    if (written != pay_size) {
        ray_sys_free(payload);
        if (owned) ray_release(expr);
        return ray_error("io", NULL);
    }

    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_SERDE_WIRE_VERSION,
        .flags   = 0,
        .endian  = RAY_SERDE_ENDIAN,
        .msgtype = RAY_IPC_MSG_ASYNC,
        .size    = pay_size,
    };
    ray_err_t e = ray_journal_write_bytes(&hdr, payload, pay_size);
    ray_sys_free(payload);
    if (owned) ray_release(expr);
    return err_to_ray(e, "io");
}

ray_t* ray_log_replay_fn(ray_t* path) {
    char p[1024];
    if (!str_to_cpath(path, p, sizeof(p)))
        return ray_error("type", ".log.replay expects a string path");

    int64_t chunks = 0, errs = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_journal_replay(p, &chunks, &errs, &status);
    switch (status) {
    case RAY_JREPLAY_OK:
        return ray_i64(chunks);
    case RAY_JREPLAY_BADTAIL: {
        int64_t valid_chunks = 0, valid_bytes = 0;
        ray_journal_validate(p, &valid_chunks, &valid_bytes);
        return ray_error("badtail",
                         "%s: framing broken after %lld entries (valid bytes = %lld)",
                         p, (long long)chunks, (long long)valid_bytes);
    }
    case RAY_JREPLAY_DESER:
        return ray_error("deser",
                         "%s: deserialization failed at chunk %lld — framing intact, content/version skew",
                         p, (long long)chunks);
    case RAY_JREPLAY_DECOMP:
        return ray_error("decompress",
                         "%s: decompression failed at chunk %lld — framing intact, do not truncate",
                         p, (long long)chunks);
    case RAY_JREPLAY_OOM:
        return ray_error("oom",
                         "%s: out of memory mid-replay after %lld entries",
                         p, (long long)chunks);
    case RAY_JREPLAY_IO:
        return ray_error("io",
                         "%s: I/O failure after %lld entries", p, (long long)chunks);
    case RAY_JREPLAY_CANCEL:
        return ray_error("cancel",
                         "%s: replay interrupted after %lld entries (log intact)",
                         p, (long long)chunks);
    }
    return ray_error("internal", "unknown replay status");
}

/* (.log.validate "path") -> (chunks; valid_bytes) — a 2-list. */
ray_t* ray_log_validate_fn(ray_t* path) {
    char p[1024];
    if (!str_to_cpath(path, p, sizeof(p)))
        return ray_error("type", ".log.validate expects a string path");

    int64_t chunks = 0, valid_bytes = 0;
    ray_err_t e = ray_journal_validate(p, &chunks, &valid_bytes);
    if (e != RAY_OK) return err_to_ray(e, "io");

    ray_t* list = ray_list_new(2);
    if (!list || RAY_IS_ERR(list)) return ray_error("oom", NULL);
    ray_t* a = ray_i64(chunks);
    ray_t* b = ray_i64(valid_bytes);
    list = ray_list_append(list, a); ray_release(a);
    if (RAY_IS_ERR(list)) { ray_release(b); return list; }
    list = ray_list_append(list, b); ray_release(b);
    return list;
}

ray_t* ray_log_roll_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    if (!ray_journal_is_open())
        return ray_error("domain", ".log.roll: no journal open");
    return err_to_ray(ray_journal_roll(), "io");
}

ray_t* ray_log_snapshot_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    if (!ray_journal_is_open())
        return ray_error("domain", ".log.snapshot: no journal open");
    return err_to_ray(ray_journal_snapshot(), "io");
}

ray_t* ray_log_sync_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return err_to_ray(ray_journal_sync(), "io");
}

ray_t* ray_log_close_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return err_to_ray(ray_journal_close(), "io");
}

/* (.log.purge) — close the active journal (if open) and unlink ALL files
 * of the current journal base: <base>.log, every rolled <base>.<stamp>.log
 * archive, the <base>.qdb snapshot, and a stray <base>.qdb.tmp.  No path
 * argument; acts on the journal .log.open/.write/.close target.  Errors
 * with `domain` when no journal base is known (none ever opened). */
ray_t* ray_log_purge_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return err_to_ray(ray_journal_purge(), "io");
}

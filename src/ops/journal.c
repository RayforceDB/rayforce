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
#include "core/errpath.h"
#include "store/journal.h"
#include "store/serde.h"
#include "core/ipc.h"
#include "lang/format.h"
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
static ray_t* err_to_ray(ray_err_t e, const char* fallback, const char* op) {
    if (e == RAY_OK) return RAY_NULL_OBJ;
    const char* code = ray_err_code_str(e);
    code = code ? code : (fallback ? fallback : "io");
    return ray_error(code, "%s: the journal failed (%s)", op, code);   /* named (#1886) */
}

/* As err_to_ray, for a builtin given a path: it names the path too. */
static ray_t* err_to_ray_path(ray_err_t e, const char* op, const char* path) {
    if (e == RAY_OK) return RAY_NULL_OBJ;
    const char* code = ray_err_code_str(e);
    code = code ? code : "io";
    char pbuf[RAY_ERR_PATH_MAX + 1];
    return ray_error(code, "%s \"%s\": the journal failed (%s)", op, ray_err_path(path, pbuf), code);
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
    return err_to_ray_path(e, ".log.open", base);
}

/* Checked atom construction for (.log.status).  ray_bool and ray_i64 test
 * ray_alloc's result only with RAY_IS_ERR and then dereference it, so a NULL
 * allocation crashes before they return.  These two allocate through
 * ray_alloc directly and test for NULL as well as an error.  The primitives
 * themselves are left as they are (a separate change). */
static ray_t* status_atom(int8_t type) {
    ray_t* v = ray_alloc(0);
    if (!v) return ray_error("oom", NULL);
    if (RAY_IS_ERR(v)) return v;
    v->type = type;
    return v;
}
static ray_t* status_bool(bool b) {
    ray_t* v = status_atom(-RAY_BOOL);
    if (!RAY_IS_ERR(v)) v->b8 = b ? 1 : 0;
    return v;
}
static ray_t* status_i64(int64_t x) {
    ray_t* v = status_atom(-RAY_I64);
    if (!RAY_IS_ERR(v)) v->i64 = x;
    return v;
}
static ray_t* status_sym(const char* s) {
    int64_t id = ray_sym_intern(s, strlen(s));
    if (id < 0) return ray_error("oom", NULL);
    return ray_sym(id);
}

/* (.log.status) — read-only view of the journal state as a dict:
 *   open       bool  a journal handle is present (writes are being
 *                    journaled).  A handle, not proof that the last append
 *                    reached the disk.
 *   opened_ok  bool  a journal was opened successfully at least once in
 *                    this process (false after a failed first open, and
 *                    after .log.purge)
 *   path       str   the last SUCCESSFULLY opened log file, null if none
 *   mode       sym   `sync or `async of that journal, null if none
 *   bytes      i64   size of the active log; 0 when not open, null when
 *                    the size probe failed
 *   auto       bool  the current evaluation runs inside an auto-journaled
 *                    IPC request (same value as (.ipc.auto_journal_eval))
 *   posts      bool  async IPC requests are journaled too (--log-posts,
 *                    .log.posts); a process setting, false by default
 * path and mode are committed only when an open succeeds, so a failed open
 * (of a first or a later journal) never replaces them.  They survive
 * .log.close and a .log.roll that lost its handle: `open` false with
 * `opened_ok` true tells a lost or closed journal from one that never
 * existed.  Raises only on allocation or symbol-intern failure; any
 * partially built result is released and the error is returned. */
#define STATUS_N 7
ray_t* ray_log_status_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    ray_journal_info_t info;
    ray_journal_info(&info);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, STATUS_N);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(STATUS_N);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    static const char* names[STATUS_N] =
        { "open", "opened_ok", "path", "mode", "bytes", "auto", "posts" };
    for (int i = 0; i < STATUS_N; i++) {
        ray_t* v;
        switch (i) {
        case 0: v = status_bool(info.open); break;
        case 1: v = status_bool(info.opened_ok); break;
        case 2: v = info.opened_ok ? ray_str(info.path, strlen(info.path)) : RAY_NULL_OBJ; break;
        case 3: v = !info.opened_ok ? RAY_NULL_OBJ
                  : status_sym(info.mode == RAY_JOURNAL_SYNC ? "sync" : "async"); break;
        case 4: v = info.bytes_ok ? status_i64(info.bytes) : RAY_NULL_OBJ; break;
        case 5: v = status_bool(ray_ipc_auto_journal_eval()); break;
        default: v = status_bool(ray_journal_posts()); break;
        }
        if (RAY_IS_ERR(v)) { ray_release(keys); ray_release(vals); return v; }

        int64_t k = ray_sym_intern(names[i], strlen(names[i]));
        if (k < 0) {
            if (v != RAY_NULL_OBJ) ray_release(v);
            ray_release(keys); ray_release(vals);
            return ray_error("oom", NULL);
        }
        ray_t* nk = ray_vec_append(keys, &k);
        if (RAY_IS_ERR(nk)) {   /* a failed append leaves keys, which is ours (#1857) */
            if (v != RAY_NULL_OBJ) ray_release(v);
            ray_release(keys); ray_release(vals);
            return nk;
        }
        keys = nk;
        ray_t* nv = ray_list_append(vals, v);
        if (v != RAY_NULL_OBJ) ray_release(v);
        if (RAY_IS_ERR(nv)) { ray_release(keys); return nv; }
        vals = nv;
    }
    return ray_dict_new(keys, vals);
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

    ray_serde_fail_clear();
    int64_t pay_size = ray_serde_size(expr);
    if (pay_size <= 0) {
        if (owned) ray_release(expr);
        /* the serializer's own refusal (limit: nested too deep, or shared
         * parts repeated beyond the budget; a column that cannot be read)
         * passes through with its code and message */
        const char* fcode = ray_serde_fail_code();
        if (pay_size < 0 && fcode) {
            char code[8], msg[256];
            snprintf(code, sizeof code, "%s", fcode);
            snprintf(msg, sizeof msg, "%s", ray_error_msg() ? ray_error_msg() : "");
            return ray_error(code, ".log.write: %s", msg);
        }
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
        return ray_error("io", ".log.write: serialized %lld of %lld bytes", (long long)written, (long long)pay_size);
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
    return err_to_ray(e, "io", ".log.write");
}

/* Prepare the private report BEFORE evaluating any journal entry.  A result-
 * construction failure must not first apply the log and then hide its counts.
 * The dictionary owns both vectors; its two I64 cells are filled after replay. */
static ray_t* replay_report_new(void) {
    static const char* names[2] = { "entries", "eval_errors" };
    int64_t ids[2];
    const int64_t zero[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i) {
        ids[i] = ray_sym_intern(names[i], strlen(names[i]));
        if (ids[i] < 0) return ray_error("oom", NULL);
    }
    ray_t* keys = ray_vec_from_raw(RAY_SYM, ids, 2);
    if (!keys) return ray_error("oom", NULL);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_vec_from_raw(RAY_I64, zero, 2);
    if (!vals || RAY_IS_ERR(vals)) {
        ray_release(keys);
        return vals ? vals : ray_error("oom", NULL);
    }
    return ray_dict_new(keys, vals); /* consumes both refs, also on error */
}

static ray_t* log_replay_impl(ray_t* path, bool reporting) {
    char p[1024];
    if (!str_to_cpath(path, p, sizeof(p)))
        return ray_error("type", reporting
                         ? ".log.replay_report expects a string path"
                         : ".log.replay expects a string path");

    ray_t* report = NULL;
    if (reporting) {
        report = replay_report_new();
        if (RAY_IS_ERR(report)) return report;
    }
    int64_t chunks = 0, errs = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_journal_replay(p, &chunks, &errs, &status);
    if (report) {
        if (status == RAY_JREPLAY_OK) {
            int64_t* counts = (int64_t*)ray_data(ray_dict_vals(report));
            counts[0] = chunks;
            counts[1] = errs;
            return report;
        }
        ray_release(report);
    }
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

ray_t* ray_log_replay_fn(ray_t* path) {
    return log_replay_impl(path, false);
}

ray_t* ray_log_replay_report_fn(ray_t* path) {
    return log_replay_impl(path, true);
}

/* (.log.validate "path") -> (chunks; valid_bytes) — a 2-list. */
ray_t* ray_log_validate_fn(ray_t* path) {
    char p[1024];
    if (!str_to_cpath(path, p, sizeof(p)))
        return ray_error("type", ".log.validate expects a string path");

    int64_t chunks = 0, valid_bytes = 0;
    ray_err_t e = ray_journal_validate(p, &chunks, &valid_bytes);
    if (e != RAY_OK) return err_to_ray_path(e, ".log.validate", p);

    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(chunks);
    ray_t* b = ray_i64(valid_bytes);
    if (!list || RAY_IS_ERR(list) || !a || RAY_IS_ERR(a) || !b || RAY_IS_ERR(b)) {
        /* every error is ours: free it as one */
        if (list && RAY_IS_ERR(list)) ray_error_free(list); else if (list) ray_release(list);
        if (a && RAY_IS_ERR(a)) ray_error_free(a); else if (a) ray_release(a);
        if (b && RAY_IS_ERR(b)) ray_error_free(b); else if (b) ray_release(b);
        return ray_error("oom", NULL);
    }
    list = ray_list_append_take(list, a); ray_release(a);
    if (RAY_IS_ERR(list)) { ray_release(b); return list; }
    list = ray_list_append_take(list, b); ray_release(b);
    return list;
}

ray_t* ray_log_roll_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", ".log.roll takes no arguments");
    if (!ray_journal_is_open())
        return ray_error("domain", ".log.roll: no journal open");
    return err_to_ray(ray_journal_roll(), "io", ".log.roll");
}

ray_t* ray_log_snapshot_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", ".log.snapshot takes no arguments");
    if (!ray_journal_is_open())
        return ray_error("domain", ".log.snapshot: no journal open");
    ray_err_t e = ray_journal_snapshot();
    ray_t* why = e != RAY_OK ? ray_journal_take_error() : NULL;
    if (why) return why;   /* the serializer's own code and message */
    return err_to_ray(e, "io", ".log.snapshot");
}

/* (.log.posts [flag]) — the posts switch (#1608).  No argument reads it;
 * 0 turns it off, any other i64 on.  When on, an open journal also records
 * each inbound async IPC request (.ipc.post) before evaluating it, as a
 * request frame that replay evaluates like a sync one.  Off by default.
 * Returns the state in effect as i64 (0/1).  Restricted: it changes what
 * the server persists.  During replay it only reads: a journaled call must
 * not change the live setting, which is the operator's (--log-posts). */
ray_t* ray_log_posts_fn(ray_t** args, int64_t n) {
    if (n == 1) {
        if (args[0]->type != -RAY_I64)
            return ray_error("type", ".log.posts: flag must be i64, got %s", ray_type_name(args[0]->type));
        if (!ray_journal_in_replay()) ray_journal_set_posts(args[0]->i64 != 0);
    } else if (n != 0) {
        return ray_error("domain", ".log.posts takes 0 or 1 arguments");
    }
    return status_i64(ray_journal_posts() ? 1 : 0);
}

ray_t* ray_log_sync_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", ".log.sync takes no arguments");
    return err_to_ray(ray_journal_sync(), "io", ".log.sync");
}

ray_t* ray_log_close_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", ".log.close takes no arguments");
    return err_to_ray(ray_journal_close(), "io", ".log.close");
}

/* (.log.purge) — close the active journal (if open) and unlink ALL files
 * of the current journal base: <base>.log, every rolled <base>.<stamp>.log
 * archive, the <base>.qdb snapshot, and a stray <base>.qdb.tmp.  No path
 * argument; acts on the journal .log.open/.write/.close target.  Errors
 * with `domain` when no journal base is known (none ever opened). */
ray_t* ray_log_purge_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", ".log.purge takes no arguments");
    return err_to_ray(ray_journal_purge(), "io", ".log.purge");
}

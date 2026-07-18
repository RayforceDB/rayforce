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

#ifndef RAY_OS_WINDOWS
#  define _GNU_SOURCE   /* fileno(), gmtime_r() */
#endif

#include "journal.h"
#include "fileio.h"
#include "serde.h"
#include "core/ipc.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "mem/sys.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef RAY_OS_WINDOWS
#  include <unistd.h>
#  include <sys/stat.h>
#endif

#define RAY_JOURNAL_PATH_MAX 1024

/* Module-private state.  Single-threaded by construction (the IPC
 * dispatch loop is single-threaded for eval_payload, and replay runs
 * from main before any worker thread spins up). */
static struct {
    ray_journal_mode_t mode;
    FILE*              fp;
    char               base[RAY_JOURNAL_PATH_MAX];
    char               log_path[RAY_JOURNAL_PATH_MAX];
    bool               in_replay;
} g_journal = {
    .mode      = RAY_JOURNAL_OFF,
    .fp        = NULL,
    .base      = {0},
    .log_path  = {0},
    .in_replay = false,
};

/* ── helpers ──────────────────────────────────────────────────────── */

static bool path_join_ext(char* dst, size_t dstsz, const char* base, const char* ext) {
    int n = snprintf(dst, dstsz, "%s%s", base, ext);
    return n > 0 && (size_t)n < dstsz;
}

static bool file_exists(const char* path) {
#ifdef RAY_OS_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* Read fixed-size buffer in a loop — fread can short-read on signals.
 * Returns SIZE_MAX on a real I/O error (vs. clean EOF) so the caller
 * can distinguish "log ended cleanly" from "torn read mid-frame".  The
 * difference matters: clean EOF after N entries means N replayed; an
 * error mid-frame must abort with RAY_JREPLAY_IO so we don't open the
 * log for append on top of a partially-replayed state. */
static size_t read_full(FILE* f, void* buf, size_t want) {
    uint8_t* p = (uint8_t*)buf;
    size_t   got = 0;
    while (got < want) {
        size_t n = fread(p + got, 1, want - got, f);
        if (n == 0) {
            if (ferror(f)) return SIZE_MAX;
            break;
        }
        got += n;
    }
    return got;
}

/* Decompress an IPC payload in place if the COMPRESSED flag is set;
 * otherwise no-op.  Returns owned buffer + length on success (caller
 * frees with ray_sys_free), NULL on failure.  When no decompression
 * is needed, *out_owned is set to false and *out_buf aliases payload. */
static bool decompress_if_needed(const ray_ipc_header_t* hdr,
                                 const uint8_t* payload, int64_t payload_len,
                                 uint8_t** out_buf, int64_t* out_len,
                                 bool* out_owned)
{
    if (!(hdr->flags & RAY_IPC_FLAG_COMPRESSED)) {
        *out_buf   = (uint8_t*)payload;
        *out_len   = payload_len;
        *out_owned = false;
        return true;
    }
    if (payload_len < 4) return false;
    uint32_t uncomp_size;
    memcpy(&uncomp_size, payload, 4);
    if (uncomp_size == 0 || uncomp_size > 256u * 1024u * 1024u) return false;

    uint8_t* tmp = (uint8_t*)ray_sys_alloc(uncomp_size);
    if (!tmp) return false;
    size_t dlen = ray_ipc_decompress(payload + 4, (size_t)payload_len - 4,
                                     tmp, uncomp_size);
    if (dlen != uncomp_size) { ray_sys_free(tmp); return false; }
    *out_buf   = tmp;
    *out_len   = (int64_t)uncomp_size;
    *out_owned = true;
    return true;
}

/* Evaluate a deserialized message exactly as eval_payload would: string
 * payloads run through ray_eval_str, everything else through ray_eval. */
static ray_t* eval_one(ray_t* msg) {
    if (!msg || RAY_IS_ERR(msg)) return msg;
    if (msg->type == -RAY_STR) {
        const char* s = ray_str_ptr(msg);
        size_t      n = ray_str_len(msg);
        if (!s || n == 0) return RAY_NULL_OBJ;
        char* tmp = (char*)ray_sys_alloc(n + 1);
        if (!tmp) return ray_error("oom", NULL);
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        ray_t* r = ray_eval_str(tmp);
        ray_sys_free(tmp);
        return r;
    }
    return ray_eval(msg);
}

/* ── public API ───────────────────────────────────────────────────── */

bool ray_journal_is_open(void) { return g_journal.fp != NULL; }

ray_err_t ray_journal_write_bytes(const ray_ipc_header_t* hdr,
                                  const uint8_t*          payload,
                                  int64_t                 payload_len)
{
    if (!g_journal.fp || g_journal.in_replay) return RAY_OK;
    if (!hdr || !payload || payload_len < 0) return RAY_ERR_DOMAIN;

    if (fwrite(hdr, 1, sizeof(*hdr), g_journal.fp) != sizeof(*hdr))
        return RAY_ERR_IO;
    if (payload_len > 0 &&
        fwrite(payload, 1, (size_t)payload_len, g_journal.fp) != (size_t)payload_len)
        return RAY_ERR_IO;

    if (g_journal.mode == RAY_JOURNAL_SYNC) {
        if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
        if (fsync(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
        /* FlushFileBuffers returns 0 (FALSE) on failure — surface it as an I/O
         * error like the fsync path above, so SYNC mode's durability guarantee
         * isn't silently dropped on Windows. */
        if (!FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_journal.fp))))
            return RAY_ERR_IO;
#endif
    }
    return RAY_OK;
}

ray_err_t ray_journal_replay(const char*           path,
                             int64_t*              out_chunks,
                             int64_t*              out_eval_errors,
                             ray_jreplay_status_t* out_status)
{
    if (out_chunks)      *out_chunks      = 0;
    if (out_eval_errors) *out_eval_errors = 0;
    if (out_status)      *out_status      = RAY_JREPLAY_OK;

    FILE* f = fopen(path, "rb");
    if (!f) {
        if (out_status) *out_status = RAY_JREPLAY_IO;
        return RAY_ERR_IO;
    }

    bool prev_in_replay = g_journal.in_replay;
    g_journal.in_replay = true;

    int64_t chunks = 0;
    int64_t errs   = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;

    for (;;) {
        /* Cancellation checkpoint (per frame — no per-frame allocation live
         * yet).  A clean abort: the log is intact and partially replayed, so
         * this must NOT be treated as a truncatable BADTAIL. */
        if (ray_interrupted()) { status = RAY_JREPLAY_CANCEL; break; }

        ray_ipc_header_t hdr;
        size_t r = read_full(f, &hdr, sizeof(hdr));
        if (r == 0) break;                          /* clean EOF */
        if (r == SIZE_MAX)                          { status = RAY_JREPLAY_IO;      break; }
        if (r != sizeof(hdr))                       { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.prefix  != RAY_SERDE_PREFIX)        { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.version != RAY_SERDE_WIRE_VERSION)  { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.endian  != RAY_SERDE_ENDIAN)        { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.size <= 0 || hdr.size > 256LL*1024*1024)
                                                    { status = RAY_JREPLAY_BADTAIL; break; }

        uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
        if (!buf)                                   { status = RAY_JREPLAY_OOM;     break; }
        size_t pr = read_full(f, buf, (size_t)hdr.size);
        if (pr == SIZE_MAX) { ray_sys_free(buf); status = RAY_JREPLAY_IO;      break; }
        if (pr != (size_t)hdr.size) {
            ray_sys_free(buf);
            status = RAY_JREPLAY_BADTAIL;
            break;
        }

        uint8_t* payload  = NULL;
        int64_t  pay_len  = 0;
        bool     owned    = false;
        if (!decompress_if_needed(&hdr, buf, hdr.size,
                                  &payload, &pay_len, &owned)) {
            ray_sys_free(buf);
            /* Framing was intact (header parsed OK, payload size matched);
             * "decode failed" is a content/code bug, NOT a tail truncation,
             * so do not point the operator at `truncate to recover`. */
            status = RAY_JREPLAY_DECOMP;
            break;
        }

        int64_t consumed = pay_len;
        ray_t*  msg      = ray_de_raw(payload, &consumed);
        if (owned) ray_sys_free(payload);
        ray_sys_free(buf);

        if (!msg || RAY_IS_ERR(msg)) {
            if (msg) ray_error_free(msg);   /* ray_release is a no-op on errors */
            status = RAY_JREPLAY_DESER;
            break;
        }

        /* Re-impose the sender's restricted state for THIS frame.  Without
         * this a `-U` client's writes silently elevate to full privilege
         * across crash-restart, since replay runs in the main thread with
         * no IPC connection context. */
        bool prev_restricted = ray_eval_get_restricted();
        ray_eval_set_restricted(hdr.flags & RAY_IPC_FLAG_RESTRICTED);
        ray_t* result = eval_one(msg);
        ray_eval_set_restricted(prev_restricted);
        ray_release(msg);

        if (result && RAY_IS_ERR(result)) {
            const char* code = ray_err_code(result);
            fprintf(stderr, "log: WARN  chunk %lld raised: %s (during replay)\n",
                    (long long)chunks, code ? code : "?");
            errs++;
            ray_error_free(result);
        } else if (result && result != RAY_NULL_OBJ) {
            ray_release(result);
        }

        chunks++;
    }

    fclose(f);
    g_journal.in_replay = prev_in_replay;

    if (out_chunks)      *out_chunks      = chunks;
    if (out_eval_errors) *out_eval_errors = errs;
    if (out_status)      *out_status      = status;

    if (status == RAY_JREPLAY_OK)     return RAY_OK;
    if (status == RAY_JREPLAY_CANCEL) return RAY_ERR_CANCEL;
    if (status == RAY_JREPLAY_IO ||
        status == RAY_JREPLAY_OOM) return RAY_ERR_IO;
    return RAY_ERR_DOMAIN;
}

ray_err_t ray_journal_validate(const char* path,
                               int64_t*    out_chunks,
                               int64_t*    out_valid_bytes)
{
    if (out_chunks)      *out_chunks      = 0;
    if (out_valid_bytes) *out_valid_bytes = 0;

    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    int64_t chunks = 0;
    int64_t valid_off = 0;
    /* Reuse one growing buffer for payload reads — most logs hold
     * many small entries plus the occasional large one, so growing on
     * demand is simpler than trying to size up front. */
    uint8_t* buf = NULL;
    int64_t  cap = 0;

    for (;;) {
        ray_ipc_header_t hdr;
        size_t r = read_full(f, &hdr, sizeof(hdr));
        if (r == 0) break;                                /* clean EOF */
        if (r != sizeof(hdr))                       break;
        if (hdr.prefix  != RAY_SERDE_PREFIX)        break;
        if (hdr.version != RAY_SERDE_WIRE_VERSION)  break;
        if (hdr.endian  != RAY_SERDE_ENDIAN)        break;
        if (hdr.size <= 0 || hdr.size > 256LL*1024*1024) break;

        if (hdr.size > cap) {
            uint8_t* tmp = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
            if (!tmp) break;                              /* OOM mid-validate */
            if (buf) ray_sys_free(buf);
            buf = tmp;
            cap = hdr.size;
        }
        /* Actually consume the payload bytes — fseek would silently
         * succeed past EOF and we'd over-count valid frames on a
         * truncated log. */
        if (read_full(f, buf, (size_t)hdr.size) != (size_t)hdr.size) break;

        valid_off += (int64_t)sizeof(hdr) + hdr.size;
        chunks++;
    }
    if (buf) ray_sys_free(buf);
    fclose(f);

    if (out_chunks)      *out_chunks      = chunks;
    if (out_valid_bytes) *out_valid_bytes = valid_off;
    return RAY_OK;
}

/* Open <base>.log in append mode after replay. */
static ray_err_t open_log_for_append(void) {
    g_journal.fp = fopen(g_journal.log_path, "ab");
    if (!g_journal.fp) return RAY_ERR_IO;
    /* Disable stdio buffering: every fwrite must reach the OS buffer
     * immediately so a SIGTERM (or any non-clean shutdown) still leaves
     * the entry on disk.  Without this, the default block-buffered FILE*
     * keeps recent writes in user-space until 4 KB accumulates — a
     * silent data-loss window that defeats the whole point of -l mode.
     * In RAY_JOURNAL_SYNC mode we additionally fsync per write; here
     * we just need the bytes to leave the process. */
    setvbuf(g_journal.fp, NULL, _IONBF, 0);
    return RAY_OK;
}

/* Phases 1+2 of recovery: load <base>.qdb then replay <base>.log.  Does
 * NOT open the journal for writing.  Used by -l/-L startup; NOT by the
 * .log.open verb (which opens for append only). */
ray_err_t ray_journal_recover(const char* base) {
    if (!base || !*base) return RAY_ERR_DOMAIN;

    char qdb_path[RAY_JOURNAL_PATH_MAX];
    char log_path[RAY_JOURNAL_PATH_MAX];
    if (!path_join_ext(qdb_path, sizeof(qdb_path), base, ".qdb"))
        return RAY_ERR_DOMAIN;
    if (!path_join_ext(log_path, sizeof(log_path), base, ".log"))
        return RAY_ERR_DOMAIN;

    /* 1. Snapshot — load <base>.qdb if present. */
    if (file_exists(qdb_path)) {
        bool prev_in_replay = g_journal.in_replay;
        g_journal.in_replay = true;
        ray_t* snap = ray_obj_load(qdb_path);
        g_journal.in_replay = prev_in_replay;

        if (!snap || RAY_IS_ERR(snap)) {
            const char* code = (snap && RAY_IS_ERR(snap)) ? ray_err_code(snap) : "io";
            fprintf(stderr, "log: ERROR  failed to load snapshot %s (%s)\n",
                    qdb_path, code ? code : "io");
            if (snap) ray_error_free(snap);
            return RAY_ERR_IO;
        }
        if (snap->type != RAY_DICT) {
            fprintf(stderr, "log: ERROR  snapshot %s is not a dict\n", qdb_path);
            ray_release(snap);
            return RAY_ERR_DOMAIN;
        }
        ray_t* keys = ray_dict_keys(snap);
        ray_t* vals = ray_dict_vals(snap);
        int64_t n = keys ? keys->len : 0;
        int64_t bound = 0;
        int64_t skipped = 0;
        int64_t bind_errs = 0;
        for (int64_t i = 0; i < n; i++) {
            if (!keys || keys->type != RAY_SYM) {
                fprintf(stderr, "log: WARN  snapshot key vector has type %d, expected RAY_SYM — dropping %lld bindings\n",
                        keys ? (int)keys->type : -1, (long long)(n - i));
                skipped += n - i;
                break;
            }
            int64_t sym_id = ((int64_t*)ray_data(keys))[i];
            ray_t* v = ray_list_get(vals, i);
            if (!v) {
                fprintf(stderr, "log: WARN  snapshot value missing for sym %lld — skipping\n",
                        (long long)sym_id);
                skipped++;
                continue;
            }
            /* MUST go through ray_env_set, NOT ray_env_bind: the former
             * flips the slot's user flag, the latter installs as builtin
             * and the value silently drops out of the next snapshot's
             * ray_env_list_user filter — silent corruption across two
             * restarts.  ray_env_set handles its own retain via
             * env_bind_global_impl, so do NOT explicitly retain here
             * (would leak one ref per binding). */
            ray_err_t e = ray_env_set(sym_id, v);
            if (e != RAY_OK) {
                fprintf(stderr, "log: WARN  snapshot bind for sym %lld failed (%d)\n",
                        (long long)sym_id, (int)e);
                bind_errs++;
                continue;
            }
            bound++;
        }
        ray_release(snap);
        fprintf(stderr, "log: loaded snapshot %s (%lld bound, %lld skipped, %lld errors)\n",
                qdb_path, (long long)bound, (long long)skipped, (long long)bind_errs);
        if (bind_errs > 0) {
            /* Partial state is a footgun.  The caller should treat this as
             * fatal and either restore from backup or skip the snapshot. */
            fprintf(stderr, "log: ERROR  snapshot load left env in a partially-applied state\n");
            return RAY_ERR_DOMAIN;
        }
    }

    /* 2. Log — replay <base>.log if present. */
    if (file_exists(log_path)) {
        int64_t chunks = 0, errs = 0;
        ray_jreplay_status_t status = RAY_JREPLAY_OK;
        ray_journal_replay(log_path, &chunks, &errs, &status);
        switch (status) {
        case RAY_JREPLAY_OK: {
            fprintf(stderr, "log: replayed %lld entries (%lld eval errors) from %s\n",
                    (long long)chunks, (long long)errs, log_path);
            break;
        }
        case RAY_JREPLAY_BADTAIL: {
            int64_t valid_chunks = 0, valid_bytes = 0;
            ray_journal_validate(log_path, &valid_chunks, &valid_bytes);
            fprintf(stderr,
                    "log: ERROR badtail in %s after %lld entries (valid bytes = %lld)\n"
                    "log: hint: truncate the file at offset %lld to recover the\n"
                    "log:       valid prefix, then restart\n",
                    log_path, (long long)chunks,
                    (long long)valid_bytes, (long long)valid_bytes);
            return RAY_ERR_DOMAIN;
        }
        case RAY_JREPLAY_DESER:
        case RAY_JREPLAY_DECOMP: {
            fprintf(stderr,
                    "log: ERROR replay failed at chunk %lld in %s: %s — framing\n"
                    "log:       was intact so this is content/code mismatch, NOT\n"
                    "log:       tail truncation.  Do NOT truncate the log; either\n"
                    "log:       fix the version skew or restore from .qdb backup.\n",
                    (long long)chunks, log_path,
                    status == RAY_JREPLAY_DECOMP ? "decompression failed" : "deserialization failed");
            return RAY_ERR_DOMAIN;
        }
        case RAY_JREPLAY_OOM:
        case RAY_JREPLAY_IO: {
            fprintf(stderr, "log: ERROR replay aborted at chunk %lld in %s (%s)\n",
                    (long long)chunks, log_path,
                    status == RAY_JREPLAY_OOM ? "out of memory" : "I/O failure");
            return RAY_ERR_IO;
        }
        case RAY_JREPLAY_CANCEL: {
            /* Interrupted: the log is intact and partially applied.  Report a
             * clean abort — must NOT advise truncation like BADTAIL. */
            fprintf(stderr, "log: replay interrupted after %lld entries in %s "
                    "(log intact, partially applied)\n",
                    (long long)chunks, log_path);
            return RAY_ERR_CANCEL;
        }
        }
    }

    return RAY_OK;
}

/* Phase 3 + state setup: open <base>.log for append in `mode`.  Does NOT
 * load the snapshot or replay.  Used by the .log.open verb and by -l/-L
 * startup (after ray_journal_recover). */
ray_err_t ray_journal_open_append(const char* base, ray_journal_mode_t mode) {
    if (!base || !*base) return RAY_ERR_DOMAIN;
    if (g_journal.fp) return RAY_ERR_DOMAIN;   /* already open */

    size_t blen = strlen(base);
    if (blen + 5 >= sizeof(g_journal.base)) return RAY_ERR_DOMAIN;

    memcpy(g_journal.base, base, blen + 1);
    g_journal.mode = mode;
    if (!path_join_ext(g_journal.log_path, sizeof(g_journal.log_path), base, ".log"))
        return RAY_ERR_DOMAIN;

    return open_log_for_append();
}

ray_err_t ray_journal_open(const char* base, ray_journal_mode_t mode) {
    if (!base || !*base) return RAY_ERR_DOMAIN;
    if (g_journal.fp) return RAY_ERR_DOMAIN;   /* already open */

    ray_err_t re = ray_journal_recover(base);
    if (re != RAY_OK) return re;
    return ray_journal_open_append(base, mode);
}

ray_err_t ray_journal_close(void) {
    if (!g_journal.fp) return RAY_OK;
    /* Check both fflush and fclose return — buffered ENOSPC slips
     * through silently otherwise and the "best-effort durability at
     * clean shutdown" promise becomes a lie.  Even on failure we null
     * the fp so the journal isn't left in a half-open zombie state. */
    int flush_rc = fflush(g_journal.fp);
    int close_rc = fclose(g_journal.fp);
    g_journal.fp = NULL;
    if (flush_rc != 0 || close_rc != 0) {
        fprintf(stderr, "log: ERROR  journal close (flush rc=%d, close rc=%d)\n",
                flush_rc, close_rc);
        return RAY_ERR_IO;
    }
    return RAY_OK;
}

/* Remove one path, tolerating ENOENT.  Sets *result to RAY_ERR_IO if an
 * existing file could not be unlinked. */
static void purge_one(const char* path, ray_err_t* result) {
    if (remove(path) != 0 && errno != ENOENT) *result = RAY_ERR_IO;
}

ray_err_t ray_journal_purge(void) {
    /* No base means no journal was ever opened in this process — there is
     * nothing to act on, and (unlike .log.close) we cannot derive a path. */
    if (!g_journal.base[0]) return RAY_ERR_DOMAIN;

    /* Close the active log FIRST: never unlink a path out from under
     * buffered writes (and Windows refuses to unlink an open file). */
    if (g_journal.fp) {
        fflush(g_journal.fp);
        fclose(g_journal.fp);
        g_journal.fp = NULL;
    }

    ray_err_t result = RAY_OK;
    char path[RAY_JOURNAL_PATH_MAX];

    /* 1. Fixed-name files: active log, snapshot, stray snapshot temp. */
    static const char* const exts[] = { ".log", ".qdb", ".qdb.tmp" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (path_join_ext(path, sizeof(path), g_journal.base, exts[i]))
            purge_one(path, &result);

    /* 2. Rolled archives <base>.<stamp>.log live beside the base file.
     *    Split base into dir + leaf, then unlink every sibling named
     *    "<leaf>.*.log" (the leading "<leaf>." guards against a different
     *    base that merely shares a prefix). */
    const char* slash = strrchr(g_journal.base, '/');
#ifdef RAY_OS_WINDOWS
    const char* bslash = strrchr(g_journal.base, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    char dir[RAY_JOURNAL_PATH_MAX];
    const char* leaf;
    if (slash) {
        size_t dlen = (size_t)(slash - g_journal.base);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, g_journal.base, dlen);
        dir[dlen] = '\0';
        leaf = slash + 1;
    } else {
        dir[0] = '.'; dir[1] = '\0';
        leaf = g_journal.base;
    }
    size_t leaflen = strlen(leaf);

    DIR* d = opendir(dir);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            const char* nm = ent->d_name;
            size_t nlen = strlen(nm);
            /* "<leaf>." prefix + non-empty stamp + ".log" suffix. The
             * length floor guarantees nm[leaflen] is in bounds and that
             * the bare active "<leaf>.log" (already removed above) is not
             * re-matched here. */
            if (nlen <= leaflen + 4) continue;
            if (strncmp(nm, leaf, leaflen) != 0 || nm[leaflen] != '.') continue;
            if (strcmp(nm + nlen - 4, ".log") != 0) continue;
            int n = snprintf(path, sizeof(path), "%s/%s", dir, nm);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;
            purge_one(path, &result);
        }
        closedir(d);
    }

    /* 3. Reset: the journal is gone, so a later .log.open starts clean. */
    g_journal.mode        = RAY_JOURNAL_OFF;
    g_journal.base[0]     = '\0';
    g_journal.log_path[0] = '\0';

    return result;
}

ray_err_t ray_journal_sync(void) {
    if (!g_journal.fp) return RAY_OK;
    if (g_journal.mode == RAY_JOURNAL_SYNC) return RAY_OK;
    if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
    if (fsync(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_journal.fp)));
#endif
    return RAY_OK;
}

/* UTC ISO-8601 with safe filename chars (no ':'). */
static void utc_stamp(char* buf, size_t bufsz) {
    time_t t = time(NULL);
    struct tm tm_;
#ifdef RAY_OS_WINDOWS
    gmtime_s(&tm_, &t);
#else
    gmtime_r(&t, &tm_);
#endif
    strftime(buf, bufsz, "%Y.%m.%dT%H.%M.%SZ", &tm_);
}

ray_err_t ray_journal_roll(void) {
    if (!g_journal.fp) return RAY_ERR_DOMAIN;

    /* Build the archive name BEFORE closing the fp — if path build
     * fails we can return without leaving the journal in a closed
     * state that ray_journal_write_bytes silently no-ops on. */
    char stamp[64];
    utc_stamp(stamp, sizeof(stamp));
    char archive[RAY_JOURNAL_PATH_MAX];
    int  n = snprintf(archive, sizeof(archive), "%s.%s.log", g_journal.base, stamp);
    if (n <= 0 || (size_t)n >= sizeof(archive)) return RAY_ERR_DOMAIN;

    int flush_rc = fflush(g_journal.fp);
    int close_rc = fclose(g_journal.fp);
    g_journal.fp = NULL;
    if (flush_rc != 0 || close_rc != 0) {
        /* Don't rename a partial/possibly-corrupt log.  Try to reopen
         * for append so subsequent writes still land somewhere. */
        fprintf(stderr, "log: ERROR  roll: pre-rename flush/close failed (flush=%d close=%d)\n",
                flush_rc, close_rc);
        (void)open_log_for_append();   /* best-effort restore; fp may stay NULL */
        return RAY_ERR_IO;
    }

    if (ray_file_rename(g_journal.log_path, archive) != RAY_OK) {
        /* Rename failed but the log file is still on disk under its
         * original name.  Reopen for append so we don't silently
         * disable journaling for the rest of the process. */
        fprintf(stderr, "log: ERROR  roll: rename %s -> %s failed\n",
                g_journal.log_path, archive);
        (void)open_log_for_append();
        return RAY_ERR_IO;
    }

    /* Durability: the rename itself is atomic but the directory entry
     * may not survive a power loss without a parent fsync.  Best-
     * effort — log if it fails but don't abort, the rename did
     * succeed. */
    (void)ray_file_sync_dir(archive);

    return open_log_for_append();
}

ray_err_t ray_journal_snapshot(void) {
    if (!g_journal.fp) return RAY_ERR_DOMAIN;

    /* Enumerate ONLY user-defined globals (slots last written via
     * ray_env_set).  Builtin function objects must NOT enter the
     * snapshot — they hold absolute pointers from the prior process
     * and would dangle on reload.  ray_env_list_user is the one-bit-
     * per-slot filter maintained by env.c. */
    int32_t cap = ray_env_global_count();
    if (cap <= 0) cap = 1;
    int64_t* sym_ids = (int64_t*)ray_sys_alloc((size_t)cap * sizeof(int64_t));
    ray_t**  vals_buf = (ray_t**) ray_sys_alloc((size_t)cap * sizeof(ray_t*));
    if (!sym_ids || !vals_buf) {
        if (sym_ids)  ray_sys_free(sym_ids);
        if (vals_buf) ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    int32_t kept = ray_env_list_user(sym_ids, vals_buf, cap);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, kept);
    if (!keys || RAY_IS_ERR(keys)) {
        if (keys && RAY_IS_ERR(keys)) ray_error_free(keys);
        ray_sys_free(sym_ids); ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    ray_t* vals = ray_list_new(kept);
    if (!vals || RAY_IS_ERR(vals)) {
        if (vals && RAY_IS_ERR(vals)) ray_error_free(vals);
        ray_release(keys);
        ray_sys_free(sym_ids); ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    for (int32_t i = 0; i < kept; i++) {
        /* ray_vec_append returns an error sentinel on failure but the
         * input `keys` was either mutated in place (rc==1, no cow) or
         * cow'd and released internally — either way the caller still
         * owns the original pointer which is now stale.  Take the
         * pre-call pointer so we can release whichever survived. */
        ray_t* prev_keys = keys;
        keys = ray_vec_append(keys, &sym_ids[i]);
        if (RAY_IS_ERR(keys)) {
            ray_error_free(keys);
            ray_release(prev_keys);
            ray_release(vals);
            ray_sys_free(sym_ids); ray_sys_free(vals_buf);
            return RAY_ERR_OOM;
        }
        ray_t* prev_vals = vals;
        vals = ray_list_append(vals, vals_buf[i]);
        if (RAY_IS_ERR(vals)) {
            ray_error_free(vals);
            ray_release(prev_vals);
            ray_release(keys);
            ray_sys_free(sym_ids); ray_sys_free(vals_buf);
            return RAY_ERR_OOM;
        }
    }
    ray_sys_free(sym_ids);
    ray_sys_free(vals_buf);

    ray_t* snap = ray_dict_new(keys, vals);
    if (!snap || RAY_IS_ERR(snap)) {
        if (snap && RAY_IS_ERR(snap)) ray_error_free(snap);
        return RAY_ERR_OOM;
    }

    char qdb_path[RAY_JOURNAL_PATH_MAX];
    char qdb_tmp[RAY_JOURNAL_PATH_MAX];
    if (!path_join_ext(qdb_path, sizeof(qdb_path), g_journal.base, ".qdb") ||
        !path_join_ext(qdb_tmp,  sizeof(qdb_tmp),  g_journal.base, ".qdb.tmp")) {
        ray_release(snap);
        return RAY_ERR_DOMAIN;
    }

    /* ray_obj_save writes prefix-headered bytes (same wire framing). */
    ray_err_t e = ray_obj_save(snap, qdb_tmp);
    ray_release(snap);
    if (e != RAY_OK) {
        /* Don't leave a half-written .qdb.tmp behind to confuse the
         * next snapshot or the operator. */
        remove(qdb_tmp);
        return e;
    }

    if (ray_file_rename(qdb_tmp, qdb_path) != RAY_OK) {
        remove(qdb_tmp);
        return RAY_ERR_IO;
    }
    /* Parent-dir fsync: rename(2) is atomic but the directory entry
     * isn't durable across a power loss without it.  Best-effort. */
    (void)ray_file_sync_dir(qdb_path);

    return ray_journal_roll();
}

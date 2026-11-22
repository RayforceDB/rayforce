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
#include "core/platform.h"   /* RAY_CPU_RELAX */
#include "core/timer.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/internal.h"
#include "table/sym.h"
#include "table/table.h"
#include "store/splay.h"
#include "mem/sys.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef RAY_OS_WINDOWS
#  include <unistd.h>
#  include <sys/stat.h>
#  include <fcntl.h>
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
    int64_t            log_gen;    /* live log's generation id, 0 = legacy/unknown */
    ray_fd_t           lock_fd;    /* <base>.lk, held while the journal is open */
    char               lock_path[RAY_JOURNAL_PATH_MAX];   /* the held <base>.lk */
    /* Identity of the last journal whose open SUCCEEDED (for
     * ray_journal_info).  log_path/mode above are assigned before the open
     * is attempted, so they can describe a failed attempt; these are only
     * written once the log is open with its preamble, and cleared by purge. */
    bool               opened_ok;
    ray_journal_mode_t ok_mode;
    char               ok_path[RAY_JOURNAL_PATH_MAX];
} g_journal = {
    .mode      = RAY_JOURNAL_OFF,
    .fp        = NULL,
    .base      = {0},
    .log_path  = {0},
    .in_replay = false,
    .log_gen   = 0,
    .lock_fd   = RAY_FD_INVALID,
    .opened_ok = false,
    .ok_mode   = RAY_JOURNAL_OFF,
    .ok_path   = {0},
};

/* One writer per journal base.  The journal is single-writer (an entry is
 * two writes, a roll renames the live log, a snapshot writes a fixed temp
 * name), so this process holds an exclusive lock on <base>.lk while it has
 * the journal open, and a second opener fails instead of interleaving.
 * .log.purge removes the lock file while holding it; a lock taken on a file
 * that was removed meanwhile is dropped and taken again on the new one, so
 * two openers can never hold locks on different files.  A lock still held
 * for another base (a roll that failed to reopen its log) is dropped first.
 * *missing, when given, tells that the lock file could not be created
 * because its directory is gone. */
static void journal_unlock(void);
static ray_err_t journal_lock(const char* base, bool* missing) {
    if (missing) *missing = false;
    char path[RAY_JOURNAL_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s.lk", base);
    if (n <= 0 || (size_t)n >= sizeof(path)) return RAY_ERR_DOMAIN;
    if (g_journal.lock_fd != RAY_FD_INVALID) {
        if (strcmp(g_journal.lock_path, path) == 0) return RAY_OK;
        journal_unlock();
    }
    int open_err = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        /* Open for writing: flock emulated over byte-range locks (NFS, SMB)
         * takes an exclusive lock only on a writable fd.  A lock file that
         * is not ours to write (another user's) is still locked through a
         * read-only fd, so it serialises with that user's process. */
        ray_fd_t fd = ray_file_open(path, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
        if (fd == RAY_FD_INVALID) {
            int e = errno;
            if (e == EACCES || e == EROFS) {
                fd = ray_file_open(path, RAY_OPEN_READ);
                /* Removed between the two opens: try again. */
                if (fd == RAY_FD_INVALID && errno == ENOENT) { open_err = e; continue; }
            }
            if (fd == RAY_FD_INVALID) {
                /* Only a missing directory means nobody can hold it. */
                if (missing) *missing = e == ENOENT;
                fprintf(stderr, "log: ERROR  cannot open %s: %s\n", path, strerror(e));
                return RAY_ERR_IO;
            }
        }
#ifndef RAY_OS_WINDOWS
        /* A program started with .sys.exec must not keep the lock alive
         * after this process exits. */
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
        if (ray_file_trylock_ex(fd) != RAY_OK) {
#ifdef RAY_OS_WINDOWS
            bool held = true;
#else
            int e = errno;
            bool held = e == EWOULDBLOCK || e == EAGAIN;
#endif
            ray_file_close(fd);
            if (held)
                fprintf(stderr, "log: ERROR  journal %s is open in another process\n", base);
#ifndef RAY_OS_WINDOWS
            else
                fprintf(stderr, "log: ERROR  cannot lock %s: %s\n", path, strerror(e));
#endif
            return RAY_ERR_IO;
        }
#ifndef RAY_OS_WINDOWS
        struct stat held, named;
        if (fstat(fd, &held) != 0 || stat(path, &named) != 0 ||
            held.st_ino != named.st_ino || held.st_dev != named.st_dev) {
            ray_file_close(fd);                  /* purged under us: retry */
            continue;
        }
#endif
        g_journal.lock_fd = fd;
        memcpy(g_journal.lock_path, path, (size_t)n + 1);
        return RAY_OK;
    }
    if (open_err)
        fprintf(stderr, "log: ERROR  cannot open %s: %s\n", path, strerror(open_err));
    return RAY_ERR_IO;
}

static void journal_unlock(void) {
    if (g_journal.lock_fd == RAY_FD_INVALID) return;
    (void)ray_file_unlock(g_journal.lock_fd);
    ray_file_close(g_journal.lock_fd);
    g_journal.lock_fd = RAY_FD_INVALID;
}

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

static ray_err_t log_truncate(const char* path, int64_t len) {
#ifdef RAY_OS_WINDOWS
    FILE* f = fopen(path, "r+b");
    if (!f) return RAY_ERR_IO;
    int rc = _chsize_s(_fileno(f), len);
    fclose(f);
    return rc == 0 ? RAY_OK : RAY_ERR_IO;
#else
    return truncate(path, (off_t)len) == 0 ? RAY_OK : RAY_ERR_IO;
#endif
}

/* ── log generation preamble (#420) ──────────────────────────────────
 *
 * Snapshot-then-roll is two renames that are individually but not
 * jointly atomic.  A crash between them leaves a fresh <base>.qdb next
 * to the OLD <base>.log whose entries are already baked into that
 * snapshot; replaying it applies every entry twice (entries are
 * arbitrary expressions — not idempotent).  The same happens without a
 * crash when the roll's rename fails and the old log is reopened for
 * append.
 *
 * The fix is identity, not ordering: a fresh log starts with a 16-byte
 * preamble carrying a generation id, and the snapshot records inside
 * the .qdb which (generation, byte length) it covers.  Recovery then
 * replays the live log only PAST the covered offset when the ids
 * match — a crash in the window replays nothing; the roll-failure
 * append path replays exactly the post-snapshot tail.  A log without
 * the preamble (pre-#420) keeps today's full-replay behavior.
 *
 * The preamble cannot be confused with a frame: entries start with
 * RAY_SERDE_PREFIX, not this magic. */

#define RAY_JLOG_MAGIC     "RAYJLG1"        /* 7 chars + NUL = 8 bytes */
#define RAY_JLOG_PREAMBLE  16

/* The sym key the snapshot dict reserves for the coverage marker; the
 * recovery bind loop skips it.  Value: I64 vec [generation, bytes]. */
#define RAY_JLOG_COVER_KEY ".journal.covered"

/* Probe a log file's preamble.  Returns the byte offset where frames
 * start (0 for a legacy headerless log) and sets *out_gen (0 legacy). */
static int64_t jlog_probe(const char* path, int64_t* out_gen) {
    if (out_gen) *out_gen = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t pre[RAY_JLOG_PREAMBLE];
    size_t r = fread(pre, 1, sizeof(pre), f);
    fclose(f);
    if (r != sizeof(pre) || memcmp(pre, RAY_JLOG_MAGIC, 8) != 0) return 0;
    int64_t gen;
    memcpy(&gen, pre + 8, sizeof(gen));
    if (out_gen) *out_gen = gen;
    return RAY_JLOG_PREAMBLE;
}

/* Generation ids need only be distinct across the rolls of one journal;
 * wall-clock ms with a low sequence salt is ample. */
static int64_t jlog_fresh_gen(void) {
    static int64_t seq = 0;
    return (ray_time_now_ms() << 12) | (++seq & 0xFFF);
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

/* ── physical records ─────────────────────────────────────────────────
 *
 * The log holds what a commit changed, not the command that changed it:
 * replay applies values and never evaluates code, so `rand`, the clock and
 * every other source of nondeterminism are settled once, when the change
 * was made.  A commit is one frame (RAY_IPC_MSG_JREC) whose payload is its
 * records back to back, each a serialized list:
 *
 *     [RAY_JREC_SET, name, value]    bind the global
 *     [RAY_JREC_DEL, name]           unbind it
 *     [RAY_JREC_APPEND, name, tail]  concat `tail` to the bound table
 *     [RAY_JREC_UPSERT, name, [key, rows]]  upsert `rows` into the bound
 *                                    table by `key`, as (upsert name key rows)
 *     [RAY_JREC_ROWS, name, [idx, rows]]  rows `idx` of the bound table now
 *                                    hold `rows` (an update with where:)
 *     [RAY_JREC_DROP, name, [idx]]   rows `idx` of it are gone (a delete)
 *     [RAY_JREC_PUBLISH, _, [root, gen, prev]]  a splayed table written in
 *                                    a transaction: make `gen` its current
 *                                    generation if `prev` still is
 *
 * Records are serialized as they are noted, so the buffer holds no reference
 * that would turn a later in-place write into a copy. */

#define RAY_JREC_SET     0
#define RAY_JREC_DEL     1
#define RAY_JREC_APPEND  2
#define RAY_JREC_PUBLISH 3
#define RAY_JREC_UPSERT  4
#define RAY_JREC_ROWS    5
#define RAY_JREC_DROP    6
#define RAY_JREC_MAX     (256LL * 1024 * 1024)   /* the frame size replay accepts */

bool ray_journal_capture = false;

static struct {
    uint8_t* buf;
    int64_t  len, cap;
    bool     lost;       /* a change of this commit could not be noted */
    bool     torn;       /* the last failed commit left part of a frame in the file */
    bool     stale;      /* the log misses a change memory has: refuse until a snapshot */
    _Atomic(int) lock;   /* globals are also set from worker threads */
} g_rec;

/* The mutator in flight on this thread, innermost first. */
typedef struct jmut {
    struct jmut* prev;
    int64_t target;      /* the global an append grows, -1 when not announced */
    int64_t rows0;       /* its length before the append */
    uint64_t shape0;     /* its type, and a table's column types, before it */
    bool    resolved;    /* the mutator said what it amends... */
    bool    local;       /* ...and it is not the global of that name */
    int64_t set_name;    /* last global this mutator published through ray_env_set */
    int64_t op;          /* the record a mutator made itself for `target`... */
    ray_t*  val;         /* ...and its value, owned; NULL for an insert */
} jmut_t;
static _Thread_local jmut_t* g_mut;

static void capture_sync(void) {
    ray_journal_capture = g_journal.fp != NULL && !g_journal.in_replay;
}

static ray_err_t rec_push(int64_t op, int64_t name, ray_t* val) {
    if (g_rec.stale) { g_rec.lost = true; return RAY_OK; }
    ray_t* rec = ray_list_new(3);
    ray_t* o = ray_i64(op);
    ray_t* s = ray_sym(name);
    ray_t* parts[3] = { o, s, val };
    ray_err_t e = !rec || RAY_IS_ERR(rec) ? RAY_ERR_OOM : RAY_OK;
    for (int i = 0; i < (val ? 3 : 2) && e == RAY_OK; i++) {
        if (!parts[i] || RAY_IS_ERR(parts[i])) { e = RAY_ERR_OOM; break; }
        ray_t* grown = ray_list_append(rec, parts[i]);   /* retains the item */
        if (!grown || RAY_IS_ERR(grown)) { if (grown) ray_error_free(grown); e = RAY_ERR_OOM; }
        else rec = grown;
    }
    if (o && !RAY_IS_ERR(o)) ray_release(o);
    if (s && !RAY_IS_ERR(s)) ray_release(s);
    if (e != RAY_OK) {
        if (rec && !RAY_IS_ERR(rec)) ray_release(rec);
        return e;
    }

    /* The log is the server's own data: lambdas keep their .sys.trust marks. */
    bool prev_trust = ray_serde_keep_trust(true);
    int64_t size = ray_serde_size(rec);
    if (size <= 0) e = RAY_ERR_DOMAIN;           /* a value serde cannot write */
    while (atomic_exchange_explicit(&g_rec.lock, 1, memory_order_acquire)) RAY_CPU_RELAX();
    if (e == RAY_OK && g_rec.len + size > RAY_JREC_MAX) e = RAY_ERR_LIMIT;
    if (e == RAY_OK && g_rec.len + size > g_rec.cap) {
        int64_t cap = g_rec.cap ? g_rec.cap : 4096;
        while (cap < g_rec.len + size) cap *= 2;
        uint8_t* nb = (uint8_t*)ray_sys_realloc(g_rec.buf, (size_t)cap);
        if (nb) { g_rec.buf = nb; g_rec.cap = cap; } else e = RAY_ERR_OOM;
    }
    if (e == RAY_OK) {
        if (ray_ser_raw(g_rec.buf + g_rec.len, rec) == size) g_rec.len += size;
        else e = RAY_ERR_DOMAIN;
    }
    atomic_store_explicit(&g_rec.lock, 0, memory_order_release);
    ray_serde_keep_trust(prev_trust);
    ray_release(rec);
    return e;
}

/* A long evaluation outside a transaction — a script, a loop — is many
 * changes, each its own commit: write what has piled up before noting the
 * next, so the pile never nears the size of one entry.  A transaction is
 * one entry and waits for its commit. */
#define RAY_JREC_FLUSH (16LL * 1024 * 1024)
static void rec_room(void) {
    if (g_rec.len >= RAY_JREC_FLUSH && !ray_tx_active) (void)ray_journal_commit();
}

int64_t ray_journal_mark(void) { rec_room(); return g_rec.len; }
void    ray_journal_rewind(int64_t mark) { if (mark < g_rec.len) g_rec.len = mark; }

ray_err_t ray_journal_note_set(int64_t name, ray_t* val) {
    jmut_t* m = g_mut;
    /* insert publishes its table itself: the appended rows are noted when
     * it returns, not the whole table now. */
    if (m && m->target == name) return RAY_OK;
    ray_err_t e = rec_push(val ? RAY_JREC_SET : RAY_JREC_DEL, name, val);
    if (e == RAY_OK && m) m->set_name = name;
    return e;
}

static ray_t* global_value(int64_t name);

/* What an append must leave alone for its rows to be all that changed:
 * the value's type, and for a table the type of every column (the first
 * rows of a table with typeless columns give them one). */
static uint64_t append_shape(ray_t* v) {
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ (uint8_t)v->type;
    if (v->type != RAY_TABLE) return h;
    int64_t n = ray_table_ncols(v);
    for (int64_t c = 0; c < n; c++) {
        ray_col_meta_t m;
        h = (h ^ (uint8_t)(ray_table_col_meta_idx(v, c, &m) ? m.type : 0xff)) * UINT64_C(0x100000001b3);
    }
    return h;
}

static int64_t append_len(ray_t* v) {
    return v->type == RAY_TABLE ? ray_table_nrows(v) : ray_len(v);
}

void ray_journal_mut_target(int64_t name, ray_t* v) {
    if (!g_mut || !v) return;
    g_mut->target = name;
    g_mut->rows0  = append_len(v);
    g_mut->shape0 = append_shape(v);
}

void ray_journal_mut_resolved(int64_t name, ray_t* v) {
    jmut_t* m = g_mut;
    if (!m || m->resolved) return;
    ray_t* cur = global_value(name);
    if (cur) ray_release(cur);
    m->resolved = true;
    m->local = cur != v;
}

void ray_journal_mark_lost(void) { g_rec.lost = true; }

/* A mutator that amended `tbl` under `name` says what it did, as the record
 * `op` of the one or two values: it is noted in place of the table. */
static void mut_note(int64_t name, ray_t* tbl, int64_t op, ray_t* a, ray_t* b) {
    jmut_t* m = g_mut;
    if (!m || m->val) return;
    /* A local of that name is not the global the record would amend. */
    ray_t* cur = global_value(name);
    if (cur) ray_release(cur);
    if (cur != tbl) return;
    ray_t* pair = ray_list_new(2);
    ray_t* parts[2] = { a, b };
    for (int i = 0; i < (b ? 2 : 1) && pair && !RAY_IS_ERR(pair); i++) {
        ray_t* grown = ray_list_append(pair, parts[i]);
        if (!grown || RAY_IS_ERR(grown)) { ray_release(pair); pair = NULL; }
        else pair = grown;
    }
    if (!pair || RAY_IS_ERR(pair)) return;       /* the whole table is logged instead */
    m->op = op;
    m->val = pair;
    m->target = name;
}

void ray_journal_mut_upsert(int64_t name, ray_t* tbl, ray_t* key, ray_t* rows) {
    mut_note(name, tbl, RAY_JREC_UPSERT, key, rows);
}
void ray_journal_mut_rows(int64_t name, ray_t* tbl, ray_t* idx, ray_t* rows) {
    mut_note(name, tbl, rows ? RAY_JREC_ROWS : RAY_JREC_DROP, idx, rows);
}

/* The global a (possibly dotted) name reads, owned; NULL when unbound. */
static ray_t* global_value(int64_t name) {
    if (!ray_sym_is_dotted(name)) {
        ray_t* v = ray_env_get_global(name);
        if (v) ray_retain(v);
        return v;
    }
    const int64_t* segs;
    if (ray_sym_segs(name, &segs) < 2) return NULL;
    ray_t* v = ray_env_path_get(ray_env_get_global(segs[0]), name);
    if (v && RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    return v;
}

/* A mutator returned the name it amended.  A write that went through
 * ray_env_set is noted already; one made in place is noted here. */
static ray_err_t mut_record(const jmut_t* m, int64_t name) {
    if (m->target != name && m->set_name == name) return RAY_OK;
    if (m->target == name && m->val) return rec_push(m->op, name, m->val);
    if (m->resolved && m->local) return RAY_OK;  /* a local of that name: no global changed */
    ray_t* cur = global_value(name);
    if (!cur) return RAY_OK;
    ray_err_t e;
    /* The rows past the old end are the whole change only if the value kept
     * its shape and had rows to keep it by; otherwise the value is logged. */
    int64_t added = m->target == name && m->rows0 > 0 &&
                    (cur->type == RAY_TABLE || cur->type == RAY_LIST || ray_is_vec(cur)) &&
                    append_shape(cur) == m->shape0
                  ? append_len(cur) - m->rows0 : -1;
    if (added == 0) {
        e = RAY_OK;
    } else if (added > 0) {
        ray_t* k = ray_i64(-added);
        ray_t* tail = k && !RAY_IS_ERR(k) ? ray_take_fn(cur, k) : NULL;
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (!tail || RAY_IS_ERR(tail)) {
            if (tail) ray_error_free(tail);
            e = RAY_ERR_OOM;
        } else {
            e = rec_push(RAY_JREC_APPEND, name, tail);
            ray_release(tail);
        }
    } else {
        e = rec_push(RAY_JREC_SET, name, cur);
    }
    ray_release(cur);
    return e;
}

ray_t* ray_journal_mutator(ray_t* (*impl)(ray_t**, int64_t), ray_t** args, int64_t n) {
    if (!ray_journal_capture) return impl(args, n);
    rec_room();
    jmut_t m = { .prev = g_mut, .target = -1, .rows0 = 0, .set_name = -1, .op = 0, .val = NULL,
                 .shape0 = 0, .resolved = false, .local = false };
    g_mut = &m;
    ray_t* r = impl(args, n);
    g_mut = m.prev;
    /* The change is made: one that cannot be noted leaves the log behind. */
    if (r && !RAY_IS_ERR(r) && r->type == -RAY_SYM && mut_record(&m, r->i64) != RAY_OK)
        g_rec.lost = true;
    if (m.val) ray_release(m.val);
    return r;
}

static ray_err_t log_truncate_open(int64_t len);

ray_err_t ray_journal_commit(void) {
    if (ray_tx_active) return RAY_OK;            /* the transaction commits as one */
    g_rec.torn = false;
    if (g_rec.lost) {
        g_rec.lost = false;
        g_rec.len = 0;
        if (!g_rec.stale)
            fprintf(stderr, "log: ERROR  a change could not be logged; the log is stale "
                            "until the next .log.snapshot\n");
        g_rec.stale = true;
        return RAY_ERR_IO;
    }
    if (g_rec.len == 0) return RAY_OK;
    ray_ipc_header_t hdr = { .prefix = RAY_SERDE_PREFIX, .version = RAY_SERDE_WIRE_VERSION,
                             .endian = RAY_SERDE_ENDIAN, .msgtype = RAY_IPC_MSG_JREC,
                             .size = g_rec.len };
    int64_t gen, pos = -1;
    if (!ray_journal_position(&gen, &pos)) pos = -1;
    ray_err_t e = ray_journal_write_bytes(&hdr, g_rec.buf, g_rec.len);
    /* Part of the frame may be in the file: take it back out, or the next
     * commit lands behind garbage. */
    if (e != RAY_OK && g_journal.fp && (pos < 0 || log_truncate_open(pos) != RAY_OK))
        g_rec.torn = true;
    g_rec.len = 0;
    if (g_rec.cap > (1 << 20)) {                 /* a large commit: give the buffer back */
        ray_sys_free(g_rec.buf);
        g_rec.buf = NULL;
        g_rec.cap = 0;
    }
    if (e != RAY_OK) {
        fprintf(stderr, "log: ERROR  journal write failed (rc=%d); the log is stale "
                        "until the next .log.snapshot\n", (int)e);
        g_rec.stale = true;
    }
    return e;
}

static ray_err_t log_truncate_open(int64_t len) {
    if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifdef RAY_OS_WINDOWS
    return _chsize_s(_fileno(g_journal.fp), len) == 0 ? RAY_OK : RAY_ERR_IO;
#else
    return ftruncate(fileno(g_journal.fp), (off_t)len) == 0 ? RAY_OK : RAY_ERR_IO;
#endif
}

/* ── transactions ─────────────────────────────────────────────────────
 *
 * One at a time, and while it is open nobody but its owner is served (the
 * owner's loop is in core/ipc.c), so isolation needs no versions: the
 * owner writes the globals themselves and reads its own writes.
 *
 * Atomicity is an undo log: the first time a name is about to change, the
 * value it holds is kept (one reference), and rollback binds the kept
 * values back, last first.  Holding the old value is also what makes a
 * mutator copy instead of writing in place.
 * shortcut: the first write to a table in a transaction copies it; undo an
 * insert by truncating instead once large tables in transactions matter.
 *
 * Durability is the journal: the records noted during the transaction are
 * held back and written as one frame by the commit. */

bool ray_tx_active = false;
bool ray_tx_unpublished = false;


static struct {
    int64_t owner;
    int64_t idle_ms, idle_at, end_at;            /* 0: no limit */
    struct { int64_t name; ray_t* old; }* undo;
    int64_t n, cap;
    int64_t mark;        /* journal buffer position at begin */
    bool    broken;      /* an old value could not be kept: only rollback is left */
    bool    doomed;      /* opened to be rolled back (see ray_tx_doom) */
    bool    acked;       /* the owner rolled a doomed transaction back itself */
} g_tx;

#ifdef DEBUG
/* Tests: end the transaction as a process dying at this point would leave
 * things.  Before the commit: nothing logged, the tables' generations on
 * disk unpublished.  After it reached the journal: the same, but logged. */
static bool g_tx_crash;
static bool tx_end(bool undo);
void ray_tx_crash_for_test(bool committed) {
    if (!ray_tx_active) return;
    g_tx_crash = true;
    if (committed) (void)ray_tx_commit();
    else { ray_journal_rewind(g_tx.mark); tx_end(false); }
    g_tx_crash = false;
}
#endif

int64_t ray_tx_owner(void) { return g_tx.owner; }

void ray_tx_heard(void) {
    if (g_tx.idle_ms > 0) g_tx.idle_at = ray_time_now_ms() + g_tx.idle_ms;
}

int64_t ray_tx_left_ms(void) {
    int64_t at = g_tx.idle_ms > 0 ? g_tx.idle_at : INT64_MAX;
    if (g_tx.end_at > 0 && g_tx.end_at < at) at = g_tx.end_at;
    return at == INT64_MAX ? INT64_MAX : at - ray_time_now_ms();
}

ray_err_t ray_tx_begin(int64_t owner, int64_t idle_ms, int64_t total_ms) {
    if (ray_tx_active) return RAY_ERR_DOMAIN;
    ray_err_t e = ray_journal_commit();          /* what came before is not part of it */
    if (e != RAY_OK) return e;
    g_tx.owner = owner;
    g_tx.idle_ms = idle_ms;
    g_tx.end_at = total_ms > 0 ? ray_time_now_ms() + total_ms : 0;
    g_tx.n = 0;
    g_tx.mark = ray_journal_mark();
    g_tx.broken = g_tx.doomed = false;           /* acked is the doomed message's to keep */
    ray_tx_active = true;
    ray_splay_defer = true;                      /* table writes wait for the commit */
    ray_tx_heard();
    return RAY_OK;
}

void ray_tx_touch(int64_t name) {
    if (!ray_tx_active) return;
    for (int64_t i = 0; i < g_tx.n; i++)
        if (g_tx.undo[i].name == name) return;
    if (g_tx.n == g_tx.cap) {
        int64_t cap = g_tx.cap ? g_tx.cap * 2 : 16;
        void* nu = ray_sys_realloc(g_tx.undo, (size_t)cap * sizeof(*g_tx.undo));
        if (!nu) { g_tx.broken = true; return; }
        g_tx.undo = nu;
        g_tx.cap = cap;
    }
    g_tx.undo[g_tx.n].name = name;
    g_tx.undo[g_tx.n].old  = global_value(name);     /* NULL: it was not bound */
    g_tx.n++;
}

/* Close the transaction; `undo` binds the kept values back first. */
static bool tx_end(bool undo) {
    bool published = true;
    ray_tx_active = false;
    ray_splay_defer = false;
    /* The tables written in it: published now that the commit is logged,
     * or removed.  A publication that fails here is finished by recovery,
     * which finds it in the journal. */
#ifdef DEBUG
    if (g_tx_crash) ray_splay_pending_abandon();
#endif
    if (ray_splay_pending_end(!undo) != RAY_OK) {
        published = false;
        fprintf(stderr, "tx: ERROR  a table written by the transaction is committed but not "
                        "published; a restart with the journal publishes it\n");
    }
    bool capture = ray_journal_capture;
    ray_journal_capture = false;                 /* restoring is not a change to log */
    for (int64_t i = g_tx.n - 1; i >= 0; i--) {
        ray_t* old = g_tx.undo[i].old;
        if (undo && ray_env_set(g_tx.undo[i].name, old) != RAY_OK)
            fprintf(stderr, "tx: ERROR  rollback could not restore a binding\n");
        if (old) ray_release(old);
    }
    ray_journal_capture = capture;
    g_tx.n = 0;
    if (undo) {
        ray_journal_rewind(g_tx.mark);
        g_rec.lost = false;
    }
    return published;
}

void ray_tx_rollback(void) {
    if (!ray_tx_active) return;
    if (g_tx.doomed) g_tx.acked = true;
    tx_end(true);
}

ray_err_t ray_tx_commit(void) {
    ray_tx_unpublished = false;
    if (!ray_tx_active) return RAY_ERR_DOMAIN;
    if (g_tx.broken || g_tx.doomed) { tx_end(true); return RAY_ERR_DOMAIN; }
    bool was_stale = g_rec.stale;
    /* The tables it wrote go into the commit: once it is on disk they are
     * published even if the process dies before publishing them. */
    ray_err_t e = RAY_OK;
    for (int64_t i = 0; ray_journal_capture && i < ray_splay_pending_count() && e == RAY_OK; i++) {
        const char* part[3] = { ray_splay_pending_root(i), ray_splay_pending_gen(i),
                                ray_splay_pending_prev(i) };
        ray_t* pair = ray_list_new(3);
        for (int k = 0; k < 3 && pair && !RAY_IS_ERR(pair); k++) {
            ray_t* str = ray_str(part[k], strlen(part[k]));
            ray_t* grown = str && !RAY_IS_ERR(str) ? ray_list_append(pair, str) : NULL;
            if (str && !RAY_IS_ERR(str)) ray_release(str);
            if (!grown || RAY_IS_ERR(grown)) { ray_release(pair); pair = NULL; }
            else pair = grown;
        }
        e = pair && !RAY_IS_ERR(pair) ? rec_push(RAY_JREC_PUBLISH, 0, pair) : RAY_ERR_OOM;
        if (pair && !RAY_IS_ERR(pair)) ray_release(pair);
    }
    if (e != RAY_OK) g_rec.lost = true;
    ray_tx_active = false;                       /* let the journal write */
    e = ray_journal_commit();
    ray_tx_active = true;
    if (e != RAY_OK) {
        /* Not logged, so not done: memory goes back to match the log,
         * which is then no staler than it was. */
        tx_end(true);
        if (!g_rec.torn) g_rec.stale = was_stale;
        return e;
    }
    /* Committed either way; a table left unpublished is still an error to
     * whoever asked, who would otherwise read the old one as the new. */
    ray_tx_unpublished = !tx_end(false);
    return ray_tx_unpublished ? RAY_ERR_IO : RAY_OK;
}

void ray_tx_doom(int64_t owner) {
    g_tx.acked = false;
    if (ray_tx_begin(owner, 0, 0) == RAY_OK) g_tx.doomed = true;
}

bool ray_tx_doom_end(void) {
    bool acked = g_tx.acked;
    if (ray_tx_active && g_tx.doomed) tx_end(true);
    g_tx.acked = false;
    return acked;
}

/* Apply one record of a replayed frame. */
static ray_err_t rec_apply(ray_t* rec) {
    if (!rec || rec->type != RAY_LIST || rec->len < 2) return RAY_ERR_DOMAIN;
    ray_t* o = ray_list_get(rec, 0);
    ray_t* s = ray_list_get(rec, 1);
    ray_t* v = rec->len > 2 ? ray_list_get(rec, 2) : NULL;
    if (!o || o->type != -RAY_I64 || !s || s->type != -RAY_SYM) return RAY_ERR_DOMAIN;
    int64_t name = s->i64;
    switch (o->i64) {
    case RAY_JREC_PUBLISH: {
        if (!v || v->type != RAY_LIST || v->len != 3) return RAY_ERR_DOMAIN;
        ray_t* root = ray_list_get(v, 0);
        ray_t* gen  = ray_list_get(v, 1);
        ray_t* prev = ray_list_get(v, 2);
        if (!root || root->type != -RAY_STR || !gen || gen->type != -RAY_STR ||
            !prev || prev->type != -RAY_STR) return RAY_ERR_DOMAIN;
        char r[1024], g[256], pv[256];
        if (ray_str_len(root) >= sizeof(r) || ray_str_len(gen) >= sizeof(g) ||
            ray_str_len(prev) >= sizeof(pv)) return RAY_ERR_DOMAIN;
        memcpy(r, ray_str_ptr(root), ray_str_len(root)); r[ray_str_len(root)] = '\0';
        memcpy(g, ray_str_ptr(gen), ray_str_len(gen));   g[ray_str_len(gen)] = '\0';
        memcpy(pv, ray_str_ptr(prev), ray_str_len(prev)); pv[ray_str_len(prev)] = '\0';
        /* A generation name is all this may put under the table. */
        if (strncmp(g, ".generations/g-", 15) != 0 || strchr(g + 13, '/') || strstr(g, ".."))
            return RAY_ERR_DOMAIN;
        return ray_splay_publish(r, g, pv);
    }
    case RAY_JREC_DEL:
        return ray_env_set(name, NULL);
    case RAY_JREC_SET: {
        if (!v) return RAY_ERR_DOMAIN;
        ray_err_t e = ray_env_set(name, v);
        /* A verb trusted by name: its identity includes that name. */
        if (e == RAY_OK && v->type == RAY_LAMBDA && (LAMBDA_TRUSTED(v) & 1))
            (void)ray_eval_lambda_trust(v, name);
        return e;
    }
    case RAY_JREC_UPSERT: {
        ray_t* cur = global_value(name);
        if (!cur || !v || v->type != RAY_LIST || v->len != 2) { if (cur) ray_release(cur); return RAY_ERR_DOMAIN; }
        /* A table in first place is the mutator's own already-evaluated
         * form: it evaluates none of the three and returns the new table. */
        ray_t* args[3] = { cur, ray_list_get(v, 0), ray_list_get(v, 1) };
        ray_t* all = cur->type == RAY_TABLE && args[1] && args[2] ? ray_upsert(args, 3) : NULL;
        ray_release(cur);
        if (!all || RAY_IS_ERR(all)) { if (all) ray_error_free(all); return RAY_ERR_DOMAIN; }
        ray_err_t e = ray_env_set(name, all);
        ray_release(all);
        return e;
    }
    case RAY_JREC_ROWS:
    case RAY_JREC_DROP: {
        ray_t* cur = global_value(name);
        bool rows = o->i64 == RAY_JREC_ROWS;
        if (!cur || !v || v->type != RAY_LIST || v->len != (rows ? 2 : 1) || cur->type != RAY_TABLE) {
            if (cur) ray_release(cur);
            return RAY_ERR_DOMAIN;
        }
        ray_t* all = ray_table_patch_rows(cur, ray_list_get(v, 0), rows ? ray_list_get(v, 1) : NULL);
        ray_release(cur);
        if (!all || RAY_IS_ERR(all)) { if (all) ray_error_free(all); return RAY_ERR_DOMAIN; }
        ray_err_t e = ray_env_set(name, all);
        ray_release(all);
        return e;
    }
    case RAY_JREC_APPEND: {
        ray_t* cur = global_value(name);
        if (!cur || !v) { if (cur) ray_release(cur); return RAY_ERR_DOMAIN; }
        ray_t* all = ray_concat_fn(cur, v);
        bool grown = all && !RAY_IS_ERR(all) && all->type == cur->type &&
                     append_len(all) == append_len(cur) + append_len(v);
        ray_release(cur);
        if (!grown) {
            if (all && RAY_IS_ERR(all)) ray_error_free(all);
            else if (all) ray_release(all);
            return RAY_ERR_DOMAIN;
        }
        ray_err_t e = ray_env_set(name, all);
        ray_release(all);
        return e;
    }
    default:
        return RAY_ERR_DOMAIN;
    }
}

/* ── public API ───────────────────────────────────────────────────── */

bool ray_journal_is_open(void) { return g_journal.fp != NULL; }

void ray_journal_info(ray_journal_info_t* out) {
    out->open      = g_journal.fp != NULL;
    out->opened_ok = g_journal.opened_ok;
    out->mode      = g_journal.ok_mode;
    out->path      = g_journal.ok_path;
    out->bytes     = 0;
    out->bytes_ok  = true;
    if (g_journal.fp) {
        /* ftello/off_t: ftell's long is 32 bits on LLP64 and 32-bit targets */
        off_t end = -1;
        if (fseeko(g_journal.fp, 0, SEEK_END) == 0) end = ftello(g_journal.fp);
        if (end >= 0) out->bytes = (int64_t)end;
        else out->bytes_ok = false;
    }
}

/* Where the next frame will be appended: the live log's generation and
 * byte offset.  False when nothing would be written (no journal, or a
 * replay in progress). */
bool ray_journal_position(int64_t* gen, int64_t* off) {
    if (!g_journal.fp || g_journal.in_replay) return false;
    /* An append stream that has not been written since it was opened
     * reports the start of the file on some platforms: go to the end. */
    if (fseek(g_journal.fp, 0, SEEK_END) != 0) return false;
    long pos = ftell(g_journal.fp);   /* the stream is unbuffered */
    if (pos < 0) return false;
    *gen = g_journal.log_gen;
    *off = (int64_t)pos;
    return true;
}

ray_err_t ray_journal_write_bytes(const ray_ipc_header_t* hdr,
                                  const uint8_t*          payload,
                                  int64_t                 payload_len)
{
    if (!g_journal.fp || g_journal.in_replay) return RAY_OK;
    if (!hdr || !payload || payload_len < 0) return RAY_ERR_DOMAIN;

    /* The stream is unbuffered (setvbuf _IONBF at open), so every fwrite is one
     * write(2).  A frame is header + payload: gather a small frame into one
     * buffer so it costs one syscall instead of two (the append itself is
     * ~2-3 us on a local filesystem, i.e. a third of a small sync request).
     * Large frames keep the two-call path to avoid the copy. */
    uint8_t frame[2048];
    if (payload_len >= 0 && (size_t)payload_len <= sizeof(frame) - sizeof(*hdr)) {
        memcpy(frame, hdr, sizeof(*hdr));
        if (payload_len > 0) memcpy(frame + sizeof(*hdr), payload, (size_t)payload_len);
        size_t total = sizeof(*hdr) + (size_t)payload_len;
        if (fwrite(frame, 1, total, g_journal.fp) != total) return RAY_ERR_IO;
    } else {
        if (fwrite(hdr, 1, sizeof(*hdr), g_journal.fp) != sizeof(*hdr))
            return RAY_ERR_IO;
        if (payload_len > 0 &&
            fwrite(payload, 1, (size_t)payload_len, g_journal.fp) != (size_t)payload_len)
            return RAY_ERR_IO;
    }

    if (g_journal.mode == RAY_JOURNAL_SYNC) {
        if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
        if (ray_fsync_fd(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
        /* FlushFileBuffers returns 0 (FALSE) on failure — surface it as an I/O
         * error like the fsync path above, so SYNC mode's durability guarantee
         * isn't silently dropped on Windows. */
        if (!ray_fsync_disabled() &&
            !FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_journal.fp))))
            return RAY_ERR_IO;
#endif
    }
    return RAY_OK;
}

static bool frame_header_ok(const ray_ipc_header_t* h) {
    return h->prefix == RAY_SERDE_PREFIX && ray_serde_wire_version_supported(h->version) &&
           h->endian == RAY_SERDE_ENDIAN && h->msgtype == RAY_IPC_MSG_JREC &&
           h->size > 0 && h->size <= RAY_JREC_MAX;
}

/* Do the `n` bytes that follow a frame header, up to the end of the file,
 * hold a chain of whole frames that ends exactly there?  A payload can hold
 * the prefix bytes by chance; a chain that fits to the last byte it cannot,
 * short of being a log itself. */
static bool tail_holds_frames(const uint8_t* p, size_t n) {
    static const uint32_t magic = RAY_SERDE_PREFIX;
    for (size_t i = 0; i + sizeof(ray_ipc_header_t) <= n; i++) {
        if (memcmp(p + i, &magic, sizeof(magic)) != 0) continue;
        size_t at = i;
        while (at + sizeof(ray_ipc_header_t) <= n) {
            ray_ipc_header_t h;
            memcpy(&h, p + at, sizeof(h));
            if (!frame_header_ok(&h) || (size_t)h.size > n - at - sizeof(h)) break;
            at += sizeof(h) + (size_t)h.size;
        }
        if (at == n) return true;
    }
    return false;
}

/* Replay frames from `start_off` (bytes) to EOF.  The offset is either
 * the preamble size or the snapshot's covered length; seeking past EOF
 * just reads a clean end (a log shorter than the covered length is a
 * strict prefix of what the snapshot already holds — nothing to do). */
static ray_err_t journal_replay_from(const char*           path,
                                     int64_t               start_off,
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
    if (start_off > 0 && fseek(f, (long)start_off, SEEK_SET) != 0) {
        fclose(f);
        if (out_status) *out_status = RAY_JREPLAY_IO;
        return RAY_ERR_IO;
    }

    /* Recovery applies a log to memory that is about to follow it.  A replay
     * asked for while a journal is open changes memory the open log has to
     * follow too: there the changes are noted like any others. */
    bool prev_in_replay = g_journal.in_replay;
    if (!g_journal.fp) g_journal.in_replay = true;
    capture_sync();

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
        if (r != sizeof(hdr))                       { status = RAY_JREPLAY_TORN;    break; }
        if (hdr.prefix  != RAY_SERDE_PREFIX)        { status = RAY_JREPLAY_BADTAIL; break; }
        if (!ray_serde_wire_version_supported(hdr.version))
                                                    { status = RAY_JREPLAY_DESER;   break; }
        if (hdr.endian  != RAY_SERDE_ENDIAN)        { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.size <= 0 || hdr.size > 256LL*1024*1024)
                                                    { status = RAY_JREPLAY_BADTAIL; break; }

        uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
        if (!buf)                                   { status = RAY_JREPLAY_OOM;     break; }
        size_t pr = read_full(f, buf, (size_t)hdr.size);
        if (pr == SIZE_MAX) { ray_sys_free(buf); status = RAY_JREPLAY_IO;      break; }
        if (pr != (size_t)hdr.size) {
            /* The file ends before this frame does.  A commit cut short —
             * unless what was read holds frames of its own: then the size
             * is what is wrong, and the commits behind it are real. */
            status = tail_holds_frames(buf, pr) ? RAY_JREPLAY_BADTAIL : RAY_JREPLAY_TORN;
            ray_sys_free(buf);
            break;
        }
        /* A frame of the command format this log used to hold: replaying
         * it would mean evaluating it, which is what the log no longer does. */
        if (hdr.msgtype != RAY_IPC_MSG_JREC) {
            ray_sys_free(buf);
            status = RAY_JREPLAY_FORMAT;
            break;
        }

        uint8_t* payload = buf;
        int64_t  pay_len = hdr.size;

        /* The records of one commit, back to back. */
        bool prev_trust = ray_serde_keep_trust(true);
        int64_t off = 0;
        while (off < pay_len && status == RAY_JREPLAY_OK) {
            int64_t left = pay_len - off;             /* ray_de_raw leaves what it did not read */
            ray_t* rec = ray_de_raw_version(payload + off, &left, hdr.version);
            int64_t consumed = pay_len - off - left;
            if (!rec || RAY_IS_ERR(rec) || consumed <= 0) {
                if (rec && RAY_IS_ERR(rec)) ray_error_free(rec);
                else if (rec) ray_release(rec);
                status = RAY_JREPLAY_DESER;
                break;
            }
            off += consumed;
            ray_err_t ae = rec_apply(rec);
            ray_release(rec);
            if (ae != RAY_OK) {
                /* The log says a change was made that cannot be made again:
                 * what follows was written on a state this is not. */
                fprintf(stderr, "log: ERROR chunk %lld: a record did not apply (rc=%d)\n",
                        (long long)chunks, (int)ae);
                status = RAY_JREPLAY_APPLY;
            }
        }
        ray_serde_keep_trust(prev_trust);
        ray_sys_free(buf);
        if (status != RAY_JREPLAY_OK) break;

        chunks++;
    }

    fclose(f);
    g_journal.in_replay = prev_in_replay;
    capture_sync();

    if (out_chunks)      *out_chunks      = chunks;
    if (out_eval_errors) *out_eval_errors = errs;
    if (out_status)      *out_status      = status;

    if (status == RAY_JREPLAY_OK)     return RAY_OK;
    if (status == RAY_JREPLAY_CANCEL) return RAY_ERR_CANCEL;
    if (status == RAY_JREPLAY_IO ||
        status == RAY_JREPLAY_OOM) return RAY_ERR_IO;
    return RAY_ERR_DOMAIN;
}

ray_err_t ray_journal_replay(const char*           path,
                             int64_t*              out_chunks,
                             int64_t*              out_eval_errors,
                             ray_jreplay_status_t* out_status)
{
    return journal_replay_from(path, jlog_probe(path, NULL),
                               out_chunks, out_eval_errors, out_status);
}

ray_err_t ray_journal_validate(const char* path,
                               int64_t*    out_chunks,
                               int64_t*    out_valid_bytes)
{
    if (out_chunks)      *out_chunks      = 0;
    if (out_valid_bytes) *out_valid_bytes = 0;

    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    /* Frames start after the generation preamble (0 on legacy logs);
     * valid_off stays an absolute file offset so truncate hints hold. */
    int64_t start = jlog_probe(path, NULL);
    if (start > 0 && fseek(f, (long)start, SEEK_SET) != 0) {
        fclose(f);
        return RAY_ERR_IO;
    }

    int64_t chunks = 0;
    int64_t valid_off = start;
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
        if (!ray_serde_wire_version_supported(hdr.version)) break;
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

/* Open <base>.log in append mode after replay.  A fresh (empty or
 * absent) log gets the generation preamble; an existing one keeps its
 * generation (0 for a pre-#420 headerless log). */
static ray_err_t open_log_for_append(void) {
    bool fresh = true;
    {   /* portable size probe (stat is unix-only here) */
        FILE* probe = fopen(g_journal.log_path, "rb");
        if (probe) {
            if (fseek(probe, 0, SEEK_END) == 0 && ftell(probe) > 0) fresh = false;
            fclose(probe);
        }
    }
    if (!fresh) {
        jlog_probe(g_journal.log_path, &g_journal.log_gen);
    }
    g_journal.fp = fopen(g_journal.log_path, "ab");
    if (!g_journal.fp) return RAY_ERR_IO;
    if (fresh) {
        int64_t gen = jlog_fresh_gen();
        uint8_t pre[RAY_JLOG_PREAMBLE] = {0};
        memcpy(pre, RAY_JLOG_MAGIC, 8);
        memcpy(pre + 8, &gen, sizeof(gen));
        if (fwrite(pre, 1, sizeof(pre), g_journal.fp) != sizeof(pre) ||
            fflush(g_journal.fp) != 0) {
            fclose(g_journal.fp);
            g_journal.fp = NULL;
    capture_sync();
            return RAY_ERR_IO;
        }
        g_journal.log_gen = gen;
    }
    /* Disable stdio buffering: every fwrite must reach the OS buffer
     * immediately so a SIGTERM (or any non-clean shutdown) still leaves
     * the entry on disk.  Without this, the default block-buffered FILE*
     * keeps recent writes in user-space until 4 KB accumulates — a
     * silent data-loss window that defeats the whole point of -l mode.
     * In RAY_JOURNAL_SYNC mode we additionally fsync per write; here
     * we just need the bytes to leave the process. */
    setvbuf(g_journal.fp, NULL, _IONBF, 0);
    /* Open with its preamble in place: commit the identity for introspection. */
    memcpy(g_journal.ok_path, g_journal.log_path, sizeof(g_journal.ok_path));
    g_journal.ok_mode   = g_journal.mode;
    g_journal.opened_ok = true;
    capture_sync();
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

    int64_t cover_gen = 0;
    int64_t cover_len = 0;

    /* 1. Snapshot — load <base>.qdb if present. */
    if (file_exists(qdb_path)) {
        bool prev_in_replay = g_journal.in_replay;
        g_journal.in_replay = true;
        /* The snapshot is the server's own data: its lambdas keep their
         * .sys.trust marks, so a logged request that called a trusted
         * verb replays as it ran (see ray_serde_keep_trust). */
        bool prev_trust = ray_serde_keep_trust(true);
        ray_t* snap = ray_obj_load(qdb_path);
        ray_serde_keep_trust(prev_trust);
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
            /* The coverage marker (#420) is snapshot metadata, not a
             * user binding — record it and keep it out of the env. */
            {
                ray_t* ks = ray_sym_str(sym_id);   /* borrowed */
                if (ks && !RAY_IS_ERR(ks) &&
                    ray_str_len(ks) == strlen(RAY_JLOG_COVER_KEY) &&
                    memcmp(ray_str_ptr(ks), RAY_JLOG_COVER_KEY,
                           strlen(RAY_JLOG_COVER_KEY)) == 0) {
                    if (v && v->type == RAY_I64 && v->len == 2) {
                        cover_gen = ((int64_t*)ray_data(v))[0];
                        cover_len = ((int64_t*)ray_data(v))[1];
                    }
                    continue;
                }
            }
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
            /* A verb trusted by name: its identity includes that name. */
            if (e == RAY_OK && v->type == RAY_LAMBDA && (LAMBDA_TRUSTED(v) & 1))
                (void)ray_eval_lambda_trust(v, sym_id);
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

    /* 2. Log — replay <base>.log if present.  When the snapshot's
     * coverage marker names THIS log generation, its prefix is already
     * baked into the .qdb we just loaded — a crash between the .qdb
     * rename and the roll (or a failed roll that kept appending) —
     * so replay starts past the covered bytes instead of applying
     * every covered entry a second time (#420). */
    if (file_exists(log_path)) {
        int64_t log_gen = 0;
        int64_t start = jlog_probe(log_path, &log_gen);
        if (cover_gen != 0 && log_gen == cover_gen && cover_len > start) {
            fprintf(stderr,
                    "log: snapshot covers %lld bytes of %s (gen %lld) — replaying tail only\n",
                    (long long)cover_len, log_path, (long long)log_gen);
            start = cover_len;
        }
        int64_t chunks = 0, errs = 0;
        ray_jreplay_status_t status = RAY_JREPLAY_OK;
        journal_replay_from(log_path, start, &chunks, &errs, &status);
        if (status == RAY_JREPLAY_TORN) {
            /* The last frame was cut short: a commit that never finished,
             * so nobody was told it had.  Drop it and keep the rest. */
            int64_t valid_chunks = 0, valid_bytes = 0;
            ray_journal_validate(log_path, &valid_chunks, &valid_bytes);
            if (log_truncate(log_path, valid_bytes) != RAY_OK) {
                fprintf(stderr, "log: ERROR  cannot drop the unfinished commit at offset %lld of %s\n",
                        (long long)valid_bytes, log_path);
                return RAY_ERR_IO;
            }
            fprintf(stderr, "log: dropped an unfinished commit at offset %lld of %s\n",
                    (long long)valid_bytes, log_path);
            status = RAY_JREPLAY_OK;
        }
        switch (status) {
        case RAY_JREPLAY_TORN:
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
        case RAY_JREPLAY_APPLY: {
            fprintf(stderr,
                    "log: ERROR replay stopped at chunk %lld in %s: a recorded change did not\n"
                    "log:       apply, so memory no longer follows the log.  Do NOT truncate;\n"
                    "log:       restore from a snapshot taken before that entry.\n",
                    (long long)chunks, log_path);
            return RAY_ERR_DOMAIN;
        }
        case RAY_JREPLAY_FORMAT: {
            fprintf(stderr,
                    "log: ERROR %s holds command entries, which this version does not\n"
                    "log:       replay.  Start the previous version on it, run\n"
                    "log:       (.log.snapshot), then start this one.\n", log_path);
            return RAY_ERR_DOMAIN;
        }
        case RAY_JREPLAY_DESER: {
            fprintf(stderr,
                    "log: ERROR replay failed at chunk %lld in %s: deserialization failed — framing\n"
                    "log:       was intact so this is content/code mismatch, NOT\n"
                    "log:       tail truncation.  Do NOT truncate the log; either\n"
                    "log:       fix the version skew or restore from .qdb backup.\n",
                    (long long)chunks, log_path);
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

    ray_err_t le = journal_lock(base, NULL);
    if (le != RAY_OK) return le;
    memcpy(g_journal.base, base, blen + 1);
    g_journal.mode = mode;
    if (!path_join_ext(g_journal.log_path, sizeof(g_journal.log_path), base, ".log")) {
        journal_unlock();
        return RAY_ERR_DOMAIN;
    }

    ray_err_t oe = open_log_for_append();
    if (oe != RAY_OK) journal_unlock();
    return oe;
}

ray_err_t ray_journal_open(const char* base, ray_journal_mode_t mode) {
    if (!base || !*base) return RAY_ERR_DOMAIN;
    if (g_journal.fp) return RAY_ERR_DOMAIN;   /* already open */

    /* Lock before replaying, so a second process fails before it applies
     * a log another process is still writing. */
    ray_err_t le = journal_lock(base, NULL);
    if (le != RAY_OK) return le;
    ray_err_t re = ray_journal_recover(base);
    if (re != RAY_OK) { journal_unlock(); return re; }
    /* The append step fails before it reaches the lock it already holds
     * (base too long for <base>.log): release ours. */
    ray_err_t oe = ray_journal_open_append(base, mode);
    if (oe != RAY_OK) journal_unlock();
    return oe;
}

static ray_err_t journal_close_fp(void);

ray_err_t ray_journal_close(void) {
    ray_err_t ce = ray_journal_commit();
    ray_err_t r = journal_close_fp();
    journal_unlock();
    return ce != RAY_OK ? ce : r;
}

static ray_err_t journal_close_fp(void) {
    if (!g_journal.fp) return RAY_OK;
    /* Check both fflush and fclose return — buffered ENOSPC slips
     * through silently otherwise and the "best-effort durability at
     * clean shutdown" promise becomes a lie.  Even on failure we null
     * the fp so the journal isn't left in a half-open zombie state. */
    int flush_rc = fflush(g_journal.fp);
    int close_rc = fclose(g_journal.fp);
    g_journal.fp = NULL;
    capture_sync();
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

    /* Hold the journal's lock while its files are removed: another process
     * that has it open is still writing them.  Only when the directory is
     * gone, so nobody can hold it, purge goes on without the lock. */
    bool missing = false;
    ray_err_t lock_result = journal_lock(g_journal.base, &missing);
    if (lock_result != RAY_OK && !missing) return lock_result;

    /* Close the active log FIRST: never unlink a path out from under
     * buffered writes (and Windows refuses to unlink an open file). */
    if (g_journal.fp) {
        ray_err_t close_result = journal_close_fp();
        if (close_result != RAY_OK) { journal_unlock(); return close_result; }
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

    /* 3. The lock file last, while it is still held (see journal_lock).
     *    Windows cannot remove a file that is open, so it stays there. */
#ifndef RAY_OS_WINDOWS
    if (g_journal.lock_fd != RAY_FD_INVALID &&
        path_join_ext(path, sizeof(path), g_journal.base, ".lk"))
        purge_one(path, &result);
#endif

    /* 4. Reset: the journal is gone, so a later .log.open starts clean. */
    journal_unlock();
    g_journal.mode        = RAY_JOURNAL_OFF;
    g_journal.base[0]     = '\0';
    g_journal.log_path[0] = '\0';
    g_journal.opened_ok   = false;
    g_journal.ok_mode     = RAY_JOURNAL_OFF;
    g_journal.ok_path[0]  = '\0';

    return result;
}

ray_err_t ray_journal_sync(void) {
    if (!g_journal.fp) return RAY_OK;
    ray_err_t ce = ray_journal_commit();
    if (ce != RAY_OK) return ce;
    if (g_journal.mode == RAY_JOURNAL_SYNC) return RAY_OK;
    if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
    if (ray_fsync_fd(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
    if (!ray_fsync_disabled())
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
    ray_err_t ce = ray_journal_commit();
    if (ce != RAY_OK) return ce;

    /* Build the archive name BEFORE closing the fp — if path build
     * fails we can return without leaving the journal in a closed
     * state that ray_journal_write_bytes silently no-ops on. */
    char stamp[64];
    utc_stamp(stamp, sizeof(stamp));
    char archive[RAY_JOURNAL_PATH_MAX];
    int  n = snprintf(archive, sizeof(archive), "%s.%s.log", g_journal.base, stamp);
    if (n <= 0 || (size_t)n >= sizeof(archive)) return RAY_ERR_DOMAIN;

    /* Rolls can happen more than once during a single UTC second.  Keep the
     * readable timestamp for the common case, but never let rename() replace
     * an earlier archive when the timestamp collides. */
    for (unsigned int suffix = 1; file_exists(archive); suffix++) {
        n = snprintf(archive, sizeof(archive), "%s.%s.%u.log",
                     g_journal.base, stamp, suffix);
        if (n <= 0 || (size_t)n >= sizeof(archive)) return RAY_ERR_DOMAIN;
    }

    int flush_rc = fflush(g_journal.fp);
    int close_rc = fclose(g_journal.fp);
    g_journal.fp = NULL;
    capture_sync();
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
    /* What is buffered belongs before the snapshot.  A commit that fails
     * here is covered anyway: the snapshot is the whole of memory. */
    (void)ray_journal_commit();

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

    /* Coverage marker (#420): record which live-log prefix this snapshot
     * already contains, so recovery can skip it when a crash (or a failed
     * roll) leaves that log next to the fresh .qdb.  Captured before the
     * .qdb rename so the marker can only under-cover, never over-cover:
     * an entry logged after this point replays as tail. */
    if (g_journal.log_gen != 0) {
        fflush(g_journal.fp);
        long pos = ftell(g_journal.fp);
        if (pos >= RAY_JLOG_PREAMBLE) {
            int64_t marker[2] = { g_journal.log_gen, (int64_t)pos };
            ray_t* mv = ray_vec_from_raw(RAY_I64, marker, 2);
            if (!mv || RAY_IS_ERR(mv)) {
                if (mv) ray_error_free(mv);
                ray_release(keys);
                ray_release(vals);
                return RAY_ERR_OOM;
            }
            int64_t mk = ray_sym_intern(RAY_JLOG_COVER_KEY,
                                        (int64_t)strlen(RAY_JLOG_COVER_KEY));
            ray_t* prev_keys = keys;
            keys = ray_vec_append(keys, &mk);
            if (RAY_IS_ERR(keys)) {
                ray_error_free(keys);
                ray_release(prev_keys);
                ray_release(vals);
                ray_release(mv);
                return RAY_ERR_OOM;
            }
            ray_t* prev_vals = vals;
            vals = ray_list_append(vals, mv);
            ray_release(mv);
            if (RAY_IS_ERR(vals)) {
                ray_error_free(vals);
                ray_release(prev_vals);
                ray_release(keys);
                return RAY_ERR_OOM;
            }
        }
    }

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

    /* ray_obj_save writes prefix-headered bytes (same wire framing).  The
     * lambdas keep their .sys.trust marks, which recovery restores. */
    bool prev_trust = ray_serde_keep_trust(true);
    ray_err_t e = ray_obj_save(snap, qdb_tmp);
    ray_serde_keep_trust(prev_trust);
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

    g_rec.stale = false;
    return ray_journal_roll();
}

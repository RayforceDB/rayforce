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

/* Journaling — the `-l` / `-L` feature.
 *
 * The log holds what changed, not the command that changed it: each entry
 * is one commit, a frame (16-byte ray_ipc_header_t, msgtype
 * RAY_IPC_MSG_JREC) whose payload is the commit's records — a global
 * bound, unbound, or a table appended to (see "Physical records" below
 * and journal.c).  Replay applies values and evaluates nothing, so it
 * rebuilds the state the writer had whatever `rand` or the clock said.
 *
 * Open with ray_journal_open(base, mode):
 *   1. If <base>.qdb exists, load it and bind every key into the global env.
 *   2. If <base>.log exists, apply it.  A last entry cut short is a commit
 *      that never finished: it is dropped.  Broken framing anywhere else
 *      (badtail) is fatal.
 *   3. Open <base>.log for append.
 *
 * After open, every change to a global is noted as it is made, and every
 * evaluation boundary — an IPC message of any kind, a REPL line, a script,
 * a timer callback, a ray_eval_str — commits what was noted.
 *
 * Replay is single-threaded by construction (it runs from main, before
 * the poll loop starts), and so is the commit; only the buffer notes go
 * into takes a lock, since a worker thread may set a global.
 */
#ifndef RAY_JOURNAL_H
#define RAY_JOURNAL_H

#include <rayforce.h>
#include "store/serde.h"

typedef enum {
    RAY_JOURNAL_OFF   = 0,
    RAY_JOURNAL_ASYNC = 1,   /* -l: write, no per-message fsync          */
    RAY_JOURNAL_SYNC  = 2,   /* -L: write + fsync per message            */
} ray_journal_mode_t;

typedef enum {
    RAY_JREPLAY_OK      = 0,
    RAY_JREPLAY_BADTAIL = 1, /* truncated frame / bad magic — framing broken                    */
    RAY_JREPLAY_IO      = 2, /* file open / read I/O failure                                    */
    RAY_JREPLAY_OOM     = 3, /* allocation failed mid-replay — transient, retryable             */
    RAY_JREPLAY_DESER   = 4, /* header valid but ray_de_raw rejected the payload                */
    RAY_JREPLAY_CANCEL  = 6, /* interrupted mid-replay — clean abort, log intact, DO NOT truncate */
    RAY_JREPLAY_TORN    = 7, /* the last frame ends early — a commit that never finished        */
    RAY_JREPLAY_FORMAT  = 8, /* a command entry of the old log format                           */
    RAY_JREPLAY_APPLY   = 9, /* a recorded change did not apply: memory no longer follows the log */
} ray_jreplay_status_t;

/* Open the journal: load <base>.qdb, replay <base>.log, open log for append.
 * Returns RAY_OK on success.  Prints a one-line summary to stderr
 * ("log: replayed N entries (M eval errors)").  Returns RAY_ERR_DOMAIN
 * if the log replay hits a badtail; the caller should print a recovery
 * hint and exit non-zero. */
ray_err_t ray_journal_open(const char* base, ray_journal_mode_t mode);

/* Phases 1+2 of recovery: load <base>.qdb then replay <base>.log.  Does
 * NOT open the journal for writing.  Composed into ray_journal_open (the
 * -l/-L startup recovery path); not invoked directly by any caller, and
 * NOT used by the .log.open verb. */
ray_err_t ray_journal_recover(const char* base);

/* Open <base>.log for append in `mode` WITHOUT loading the snapshot or
 * replaying.  This is what the .log.open verb calls (resume appending
 * without surprising re-execution of the whole log). */
ray_err_t ray_journal_open_append(const char* base, ray_journal_mode_t mode);

/* True iff a journal is currently open for append. */
bool ray_journal_is_open(void);

/* Snapshot of the journal state, for introspection.  `open` is the same
 * answer as ray_journal_is_open(): a handle is present, not proof that the
 * last append reached the disk.  `opened_ok` is true once a journal was
 * opened successfully in this process (cleared by ray_journal_purge).
 * `mode` and `path` describe the last SUCCESSFULLY opened journal: they are
 * committed only when an open succeeds, so a failed open never changes
 * them, and they survive ray_journal_close and a ray_journal_roll that lost
 * its handle.  Without a successful open `path` is "" and `mode` is
 * RAY_JOURNAL_OFF.  `bytes` is the size of the active log, 0 when not open;
 * `bytes_ok` is false when the size probe failed (then `bytes` is 0 and
 * meaningless).  `path` points into module storage and is valid until the
 * next journal call. */
typedef struct {
    bool               open;
    bool               opened_ok;
    ray_journal_mode_t mode;
    const char*        path;
    int64_t            bytes;
    bool               bytes_ok;
} ray_journal_info_t;

void ray_journal_info(ray_journal_info_t* out);

/* Append one entry to the active journal: what ray_journal_commit does
 * with the records it holds.  No-op (returns RAY_OK) if no journal is
 * open or a replay is in progress.  In RAY_JOURNAL_SYNC mode, fflush +
 * fsync before returning. */
ray_err_t ray_journal_write_bytes(const ray_ipc_header_t* hdr,
                                  const uint8_t*          payload,
                                  int64_t                 payload_len);

/* ── Physical records ─────────────────────────────────────────────────
 * The log holds what changed, not the command that changed it (see
 * journal.c).  Changes are noted as they are made and written as one
 * frame by ray_journal_commit(), which every evaluation boundary calls:
 * the end of an IPC message, of a REPL line, of a script. */

/* True while a journal is open for append and no replay runs: the gate
 * in front of every call below. */
extern bool ray_journal_capture;

/* Note `name := val` (NULL unbinds) BEFORE binding it; an error means the
 * change cannot be logged and must not be made.  ray_journal_rewind() to a
 * ray_journal_mark() taken first drops the note when the bind then fails. */
ray_err_t ray_journal_note_set(int64_t name, ray_t* val);
int64_t   ray_journal_mark(void);
void      ray_journal_rewind(int64_t mark);

/* Run a mutator (insert, upsert, update, delete, alter) and note what it
 * changed when it returns the name it amended. */
ray_t* ray_journal_mutator(ray_t* (*impl)(ray_t**, int64_t), ray_t** args, int64_t n);
/* A mutator, right before it appends to the global `name`, which holds the
 * table, vector or list `v`: what lies past v's end afterwards is noted in
 * place of the whole value, provided the append left its shape alone. */
void ray_journal_mut_target(int64_t name, ray_t* v);
/* A mutator, once it has found what the name `name` gave it to amend: when
 * that is not the global of that name (a local is), nothing is noted. */
void ray_journal_mut_resolved(int64_t name, ray_t* v);
/* A change was made that could not be noted: the next commit fails. */
void ray_journal_mark_lost(void);
/* upsert, once its key and rows are evaluated, amending `tbl` under the
 * name `name`: the two are noted in place of the table they leave. */
void ray_journal_mut_upsert(int64_t name, ray_t* tbl, ray_t* key, ray_t* rows);
/* update and delete, having amended `tbl` in place: rows `idx` now hold
 * `rows`, or are gone when rows is NULL. */
void ray_journal_mut_rows(int64_t name, ray_t* tbl, ray_t* idx, ray_t* rows);
/* What replay does with such a record: a new table (ops/query.c). */
ray_t* ray_table_patch_rows(ray_t* tbl, ray_t* idx, ray_t* rows);

/* Write the changes noted since the last commit as one frame (fsync in
 * RAY_JOURNAL_SYNC mode).  RAY_OK when there are none.  On an error the
 * log misses a change memory has: later commits fail too, until
 * ray_journal_snapshot() writes memory out whole. */
ray_err_t ray_journal_commit(void);

/* ── Transactions ─────────────────────────────────────────────────────
 * One at a time (see journal.c).  The owner is the handle of the IPC
 * connection that opened it, or RAY_TX_LOCAL for the REPL or a script. */

#define RAY_TX_LOCAL (-1)

extern bool ray_tx_active;

/* Open a transaction for `owner`; RAY_ERR_DOMAIN when one is open.
 * idle_ms bounds the owner's silence between messages and total_ms the
 * whole transaction; 0 is no limit. */
ray_err_t ray_tx_begin(int64_t owner, int64_t idle_ms, int64_t total_ms);
/* Write the transaction's changes as one journal commit and close it.  On
 * an error nothing is logged and everything is rolled back — except when
 * ray_tx_unpublished is set afterwards: then it is committed, and a table
 * it wrote could not be published (a restart with the journal does it). */
ray_err_t ray_tx_commit(void);
extern bool ray_tx_unpublished;
/* Bind every global back to what it held at begin and close.  No-op when
 * none is open. */
void      ray_tx_rollback(void);
int64_t   ray_tx_owner(void);
/* The owner was heard from: its idle limit starts over. */
void      ray_tx_heard(void);
/* Milliseconds until a limit of the open transaction runs out (<= 0: it
 * has); INT64_MAX when it has none. */
int64_t   ray_tx_left_ms(void);
/* `name` is about to change: keep what it holds, once per transaction. */
void      ray_tx_touch(int64_t name);
/* A message from an owner whose transaction was aborted runs in one that
 * can only be rolled back: ray_tx_doom before it, ray_tx_doom_end after,
 * which rolls back whatever it did and tells whether it was the owner's
 * own (.tx.rollback) — the acknowledgement that ends the aborted state. */
void      ray_tx_doom(int64_t owner);
bool      ray_tx_doom_end(void);
#ifdef DEBUG
/* Tests: end the open transaction as a process dying would leave things —
 * before its commit (false), or after the commit reached the journal but
 * before its tables were published (true). */
void      ray_tx_crash_for_test(bool committed);
#endif

/* Replay a log file, applying each entry in order.  *out_chunks counts
 * evaluated entries INCLUDING entries whose evaluation returned an error;
 * *out_eval_errors counts those errors.  They warn and replay continues:
 * no rollback of an entry's partial effects or the already-applied prefix.
 * *out_status records a terminal framing/read/decode/resource/interruption
 * failure, or RAY_JREPLAY_OK on a clean tail (not proof all evals succeeded). */
ray_err_t ray_journal_replay(const char*           path,
                             int64_t*              out_chunks,
                             int64_t*              out_eval_errors,
                             ray_jreplay_status_t* out_status);

/* Validate (parse but don't eval) frames without evaluating.
 * *out_chunks counts valid entries; *out_valid_bytes is the byte
 * offset of the first bad header (== file size on a clean log). */
ray_err_t ray_journal_validate(const char* path,
                               int64_t*    out_chunks,
                               int64_t*    out_valid_bytes);

/* Close the active log, rename it to <base>.<UTC-ISO8601>.log, open a
 * fresh empty <base>.log for append.  Errors if no journal is open. */
ray_err_t ray_journal_roll(void);

/* Serialize every user (non-reserved) global env binding into a dict and
 * write it as a single entry to <base>.qdb.tmp, then atomic-rename to
 * <base>.qdb, then call ray_journal_roll.  After this, the .log file
 * is fresh and a future restart loads .qdb instead of replaying the
 * old (now archived) log. */
ray_err_t ray_journal_snapshot(void);

/* Where the next frame will be appended: the live log's generation and
 * byte offset.  False when nothing would be written. */
bool ray_journal_position(int64_t* gen, int64_t* off);

/* Force fflush + fsync on the active journal.  No-op (RAY_OK) when no
 * journal is open or when in RAY_JOURNAL_SYNC mode (where every write
 * already syncs). */
ray_err_t ray_journal_sync(void);

/* Close the active journal.  No-op if none is open. */
ray_err_t ray_journal_close(void);

/* Close the active journal (if open) and unlink EVERY on-disk file of the
 * current journal base: the active <base>.log, every rolled
 * <base>.<stamp>.log archive, the <base>.qdb snapshot, and a stray
 * <base>.qdb.tmp.  Operates on the same journal .log.open/.write/.close
 * target — the base survives .log.close, so this works after a close.
 * Resets module state to OFF so a later .log.open starts clean.
 * Best-effort: a missing file is not an error; returns RAY_ERR_IO if an
 * existing file cannot be removed, RAY_ERR_DOMAIN if no journal base is
 * known (none ever opened this process). */
ray_err_t ray_journal_purge(void);

#endif /* RAY_JOURNAL_H */

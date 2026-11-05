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

/* Transaction-log journaling — the `-l` / `-L` feature.
 *
 * Wire format: every entry is a complete IPC message (16-byte
 * ray_ipc_header_t followed by serialized payload), so log frames
 * share parser code with the live IPC path.  Concatenation by `cat`
 * is valid by construction.
 *
 * Open with ray_journal_open(base, mode):
 *   1. If <base>.qdb exists, load it and bind every key into the global env.
 *   2. If <base>.log exists, replay it (badtail is fatal, eval errors warn).
 *   3. Open <base>.log for append.
 *
 * After open, the IPC dispatch hook (eval_payload in core/ipc.c) calls
 * ray_journal_write_bytes() for every inbound sync message before
 * evaluating it.  Async messages and responses are not journaled,
 * journaling only the .z.ps stream.
 *
 * Replay is single-threaded by construction (it runs from main, before
 * the poll loop starts) so the module is intentionally not thread-safe;
 * the IPC dispatch loop is also single-threaded for eval_payload, so the
 * shared file handle does not need a mutex either.
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
    RAY_JREPLAY_DECOMP  = 5, /* compressed payload, but decompression failed                    */
    RAY_JREPLAY_CANCEL  = 6, /* interrupted mid-replay — clean abort, log intact, DO NOT truncate */
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

/* Append one entry to the active journal.  No-op (returns RAY_OK) if
 * no journal is open or if a replay is currently in progress (we do
 * NOT recursively log replayed messages even if .log.write is called
 * from a replayed entry).  In RAY_JOURNAL_SYNC mode, fflush + fsync
 * before returning. */
ray_err_t ray_journal_write_bytes(const ray_ipc_header_t* hdr,
                                  const uint8_t*          payload,
                                  int64_t                 payload_len);

/* Replay a log file, evaluating each entry in order.  Sets *out_chunks
 * to entries successfully replayed and *out_eval_errors to entries that
 * deserialized cleanly but raised an error during ray_eval (those are
 * skipped with a stderr warning, not fatal — framing was intact).
 * *out_status is RAY_JREPLAY_OK on a clean tail or RAY_JREPLAY_BADTAIL
 * if a truncated/corrupt frame was found. */
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

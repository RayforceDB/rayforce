# `.log.*` — write-ahead log

A binary append-only journal of what changed. The runtime keeps one journal open at a time. While it is open, every change to a global is recorded as it is made, and written at the end of the evaluation that made it: an IPC message of any kind, a REPL line, a script, a timer callback. Recovery applies the recorded changes in order; it evaluates nothing.

Each entry is one **commit**. It holds physical records, not the command that produced them:

| Record | Written for | Recovery does |
|---|---|---|
| bind | `set`, top-level `let`, `alter`, and any mutator that had to rebuild its table | binds the name to the recorded value |
| unbind | `del` | removes the name |
| append | `insert` into a named table | appends the recorded rows |
| upsert | `upsert` into a named table | upserts the recorded rows by the recorded key |
| rows | `update` with `where:`, made in place | writes the recorded rows at the recorded positions |
| drop | `delete`, made in place | removes the rows at the recorded positions |
| publish | a splayed table written in a [transaction](tx.md) | makes its new generation current, if it is not already |

Consequences worth knowing:

- **Recovery is deterministic.** `rand`, the clock and anything else a command consulted were settled when the change was made; recovery reproduces the values, not the computation.
- **A change costs its rows.** `insert` and `upsert` write the rows they were given; an `update` with `where:` and a `delete` write the rows they touched. The whole table is written only when the mutator could not work in place and rebuilt it: an `update` without `where:` or with `by:`, one that adds a column, any `alter`, a table that something else also holds, or the first write to a table inside a [transaction](tx.md).
- **An entry cannot exceed 256 MB.** A change larger than that is refused.
- **A value that cannot be serialised cannot be bound while a journal is open** (a graph or index handle, for example): `set` fails with `domain` and the name stays as it was.
- **Only globals come back.** A timer, a listener, an open handle or a file written by a request is not re-created by recovery.
- **A failed statement keeps what it changed.** Outside a transaction each change stands on its own, as each statement does under autocommit in SQL: the changes a statement made before it failed are in memory, so they are in the log too. Use a [transaction](tx.md) for all-or-nothing.

If a commit cannot be written (disk full, I/O error) the reply is `io`, the change stays in memory, and the journal refuses every later commit until [`.log.snapshot`](#log-snapshot) writes memory out whole. A partly written entry is taken back out of the file.

Logs written by earlier versions held commands. They are not replayed: recovery stops with a message. Start the earlier version on such a log, run `(.log.snapshot)`, then start this one — the snapshot format is unchanged.

!!! note "Restricted under `-U`"
    `.log.open`, `.log.replay`, `.log.replay_report`, `.log.roll`, `.log.snapshot`, `.log.close`, `.log.purge` are `RAY_FN_RESTRICTED`. The introspection / I/O-only entries (`.log.sync`, `.log.validate`, `.log.status`) are unrestricted. Replay changes globals; the reporting variant is **not** a read-only status getter.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.log.open`](#log-open) | variadic | restricted | Open a journal in async or sync mode. |
| [`.log.status`](#log-status) | variadic | — | Report the journal state as a dict. |
| [`.log.sync`](#log-sync) | nullary | — | `fsync` the journal. |
| [`.log.snapshot`](#log-snapshot) | nullary | restricted | Write a snapshot of current state and roll to a fresh segment. |
| [`.log.roll`](#log-roll) | nullary | restricted | Close the active segment and start a new one. |
| [`.log.replay`](#log-replay) | unary | restricted | Apply a journal file; return the number of entries, including those with a record that did not apply. |
| [`.log.replay_report`](#log-replay-report) | unary | restricted | Apply once; return `entries` and `eval_errors` on a clean tail. |
| [`.log.validate`](#log-validate) | unary | — | Scan a journal file; return `(chunks valid_bytes)`. |
| [`.log.close`](#log-close) | nullary | restricted | Flush and close the active journal. |
| [`.log.purge`](#log-purge) | nullary | restricted | Close the active journal and delete all its files. |

## `.log.open` { #log-open }

Signature: `(.log.open mode "base")`. `mode` is `'async` or `'sync`; `base` is the journal base path (the actual file ends with `.log` and rolled segments use numeric suffixes).

- `'async` — each commit is handed to the operating system; a crash of the process loses nothing, a crash of the machine may lose the last commits.
- `'sync` — each commit is `fsync`'d before its reply is sent. Durable, ~10–100× slower per commit.

Returns the null object on success.

Opens the journal **for append only** — it does not load a snapshot or replay the existing log. Resuming an existing journal therefore does not re-apply its entries; recover state explicitly with [`.log.replay`](#log-replay) (or let the `-l`/`-L` startup path do it for you). This keeps "open to keep writing" distinct from "recover prior state".

A journal has one writer. While it is open, the process holds an exclusive lock on `<base>.lk`; a second process that opens the same base — with `.log.open`, `-l` or `-L` — fails with `io` and logs `journal <base> is open in another process`. The lock is released by `.log.close`, `.log.purge` or process exit.

Errors: `rank` (arity != 2), `type` (mode not a sym / base not a string), `domain` (unknown mode), `io` (open failed, or another process has the journal open).

```lisp
(.log.open 'async "/tmp/rayforce-journal")
(.log.close)
```

The server CLI flags `-l` (async) and `-L` (sync) open the journal at startup and, unlike the bare verb, additionally load the latest snapshot and replay the log first — a full crash-recovery, then open-for-append.

## `.log.status` { #log-status }

Signature: `(.log.status)`. Returns a dict describing the journal; read-only, and answers the same way when no journal was ever opened. It raises only if allocating the result (or interning a symbol) fails.

| Key | Type | Meaning |
|---|---|---|
| `open` | bool | a journal handle is present. Not proof of append health or durability: a failed append is not reflected here, and writes are also suppressed during replay |
| `opened_ok` | bool | a journal was opened successfully at least once in this process; false after a failed first open, and after [`.log.purge`](#log-purge) |
| `path` | string, or null | the last **successfully** opened log file (`<base>.log`); null if there was none |
| `mode` | sym, or null | `'sync` or `'async` of that journal; null if there was none |
| `bytes` | i64, or null | size of the active log in bytes; `0` when `open` is false, null if the size could not be read |
| `auto` | bool | the value of `(.ipc.auto_journal_eval)`: set only by an embedder that journals requests itself |

`path` and `mode` are committed only when an open succeeds. A failed `.log.open` therefore never reports an identity (`opened_ok` stays false, `path` and `mode` stay null) and never replaces the remembered one. They keep the values of the last successful journal after `.log.close`, and after a `.log.roll` that failed once the old file was closed (for example the new log could not be created: disk full, out of file descriptors). In that second case the process has no journal handle, sync requests are no longer journaled, and `.log.sync` still returns null; `open` is false while `opened_ok` is true. An embedder that must not run unjournaled can check it and stop:

```lisp
(if (get (.log.status) 'open) null (exit 1))
```

The getter only reports. Whether a request is acknowledged while the journal is down is a separate IPC policy.

```lisp
(.log.status)
;; {open:false opened_ok:false path:null mode:null bytes:0 auto:false}
(.log.open 'async "/tmp/rayforce-journal")
(.log.status)
;; {open:true opened_ok:true path:"/tmp/rayforce-journal.log" mode:async bytes:16 auto:false}
(.log.close)
```

## `.log.sync` { #log-sync }

Signature: `(.log.sync)`. Writes what the current evaluation has changed so far as a commit and forces an `fsync` on the active segment. Returns null. No-op when no journal is open (the underlying call returns `RAY_OK`).

## `.log.snapshot` { #log-snapshot }

Signature: `(.log.snapshot)`. Writes a snapshot of current state (env globals serialised into a special frame) and rolls to a new segment. Snapshots let `replay` start from a recent state and only re-apply entries written after.

What the current evaluation changed before the call is committed first, so it lands in the snapshot and not in the fresh log. A snapshot also ends the refusal that follows a failed commit.

A lambda marked with [`.sys.trust`](sys.md#sys-trust) keeps its mark in the snapshot and in the journal, and recovery restores it. The mark is kept only there; `ser`, `de` and IPC never carry it. A journal entry is written only by the server, and a frame of that type arriving on a connection closes it.

Errors: `domain` (no journal open), `io`.

## `.log.roll` { #log-roll }

Signature: `(.log.roll)`. Closes the active segment and opens a fresh one with the next numeric suffix. Useful for time-bounded rotation (cron-driven) without involving a snapshot.

Errors: `domain` (no journal open), `io`.

## `.log.replay` { #log-replay }

Signature: `(.log.replay "path")`. Reads each entry in the file and applies its records. Returns the number of **entries**, including entries with a record that did not apply (an append to a name that is not bound, for example). Such a record produces a warning and replay continues; the integer is **not** a count of entries applied in full.

Use [`.log.replay_report`](#log-replay-report) **instead of** `.log.replay` when the caller needs the count of such entries; the reporting verb performs its own replay, not an inspection of a previous call. Neither verb rolls back an already-applied prefix.

Unlike recovery at startup, the verb does not drop an entry cut short at the end of the file: it reports it.

| Error code | Meaning | Action |
|---|---|---|
| `badtail` | Framing not accepted after a prefix, or the last entry is cut short (an unfinished commit). | Inspect with `.log.validate`. Startup recovery drops an entry cut short at the very end by itself; broken framing anywhere else needs a decision. |
| `format` | An entry of the earlier command format. | Snapshot the log with the earlier version. |
| `deser` | Entry intact but a record could not be decoded — content/version skew. | Do **not** truncate; inspect and fix. |
| `oom` / `io` | Resource exhaustion / read failure. | Resolve the cause and rebuild from a known state before retrying; earlier entries may already have taken effect. |
| `cancel` | Replay interrupted; the log is intact. | Reconstruct the intended state before retrying. |

```lisp
(.log.replay "/tmp/rayforce-journal.log")
;; => 2   (entries read, not proof every record applied)
```

## `.log.replay_report` { #log-replay-report }

Signature: `(.log.replay_report "path")`. **Applies the file once**, under the same rules as `.log.replay`. On a clean tail it returns a dictionary of two I64 fields:

- `entries`: entries read, including those with a record that did not apply.
- `eval_errors`: records that did not apply.

The report is allocated before replay starts; failure to allocate it raises `oom` without applying anything. Terminal failures raise the same errors as `.log.replay`, and may follow an already-applied prefix.

```lisp
(set report (.log.replay_report "/tmp/rayforce-journal.log"))
(get report 'entries)      ;; 3
(get report 'eval_errors)  ;; 1
```

This is observability, not atomic recovery or validation of application invariants. `.log.validate` checks framing only.

## `.log.validate` { #log-validate }

Signature: `(.log.validate "path")`. Scans the file framing-only — does not deserialise payloads. Returns a 2-list `(chunks valid_bytes)` where `valid_bytes` is the offset at which framing breaks (= file size if intact).

```lisp
(.log.validate "/tmp/rayforce-journal.log")
;; => (2 92)
```

## `.log.close` { #log-close }

Signature: `(.log.close)`. Flushes and closes the active journal. Returns null. No-op if no journal is open.

## `.log.purge` { #log-purge }

Signature: `(.log.purge)`. Closes the active journal (if open) and deletes its **entire** on-disk footprint: the active `<base>.log`, every rolled `<base>.<stamp>.log` archive, the `<base>.qdb` snapshot, a stray `<base>.qdb.tmp` and the `<base>.lk` lock file. It fails with `io` while another process has the journal open. Takes no path argument — it acts on the current journal, the same one `.log.open` / `.log.close` operate on (the base survives `.log.close`, so purge works after a close). Returns null on success; errors with `domain` if no journal has been opened in this process. Best-effort delete: a missing file is not an error.

```
(.log.close)
(.log.purge)   ;; no path needed — deletes every segment + snapshot of the active journal
```

## See also

- [`.tx.*` — Transactions](tx.md) — several changes as one commit.
- [IPC & Serialization](../storage/ipc.md) — wire format used by journal frames.

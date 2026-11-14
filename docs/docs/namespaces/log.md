# `.log.*` — write-ahead log

A binary append-only journal of serialised Rayfall expressions. The runtime keeps one journal open at a time. While open, every IPC mutation (and any value explicitly handed to `.log.write`) is appended; `replay` deserialises and re-evaluates each entry to reconstruct state. Files use the same 16-byte IPC header + serialized-payload frames as `.ipc.send`, with optional delta+RLE compression on large entries — the journal is interoperable with the wire format.

The journal is intentionally minimal: there's no per-entry timestamp or transaction grouping at the journal layer. Snapshot + replay is the only recovery model.

!!! note "Restricted under `-U`"
    `.log.open`, `.log.write`, `.log.replay`, `.log.replay_report`, `.log.roll`, `.log.snapshot`, `.log.close`, `.log.purge` are `RAY_FN_RESTRICTED`. The introspection / I/O-only entries (`.log.sync`, `.log.validate`, `.log.status`) are unrestricted. Replay executes journal expressions; the reporting variant is **not** a read-only status getter.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.log.open`](#log-open) | variadic | restricted | Open a journal in async or sync mode. |
| [`.log.status`](#log-status) | variadic | — | Report the journal state as a dict. |
| [`.log.write`](#log-write) | unary | restricted | Append a serialised expression to the open journal. |
| [`.log.sync`](#log-sync) | nullary | — | `fsync` the journal. |
| [`.log.snapshot`](#log-snapshot) | nullary | restricted | Write a snapshot of current state and roll to a fresh segment. |
| [`.log.roll`](#log-roll) | nullary | restricted | Close the active segment and start a new one. |
| [`.log.replay`](#log-replay) | unary | restricted | Replay a journal file; return the processed-entry count, including evaluation errors. |
| [`.log.replay_report`](#log-replay-report) | unary | restricted | Replay once; return `entries` and `eval_errors` on a clean tail. |
| [`.log.validate`](#log-validate) | unary | — | Scan a journal file; return `(chunks valid_bytes)`. |
| [`.log.close`](#log-close) | nullary | restricted | Flush and close the active journal. |
| [`.log.purge`](#log-purge) | nullary | restricted | Close the active journal and delete all its files. |

## `.log.open` { #log-open }

Signature: `(.log.open mode "base")`. `mode` is `'async` or `'sync`; `base` is the journal base path (the actual file ends with `.log` and rolled segments use numeric suffixes).

- `'async` — buffered writes; faster, may lose entries on a crash within the write window.
- `'sync` — each write is `fsync`'d before returning. Durable, ~10–100× slower per write.

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
| `auto` | bool | the current evaluation runs inside an auto-journaled IPC request (same value as [`.ipc.auto_journal_eval`](#log-write)) |

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

## `.log.write` { #log-write }

Signature: `(.log.write expr)`. Serialises `expr` and appends a frame to the open journal.

Errors: `noopen` (no journal active — surfaced explicitly so callers don't believe a no-op succeeded), `type` (no arg), `domain` (`expr` has zero serialised size, or the call runs inside an auto-journaled IPC request), `oom`, `io`. Lazy values are forced before serialisation.

Inside an auto-journaled IPC request the request itself is the journal entry and is re-evaluated on replay, so an extra frame would apply the change twice. Code shared between journaled request handlers and other paths tests `(.ipc.auto_journal_eval)` and skips its own write there:

```lisp
(if (.ipc.auto_journal_eval) null (.log.write expr))
```

```lisp
(.log.open 'async "/tmp/rayforce-journal")
(.log.write 42)
(.log.write [1 2 3 4 5])
(.log.close)
```

## `.log.sync` { #log-sync }

Signature: `(.log.sync)`. Forces an `fsync` on the active segment. Returns null. No-op when no journal is open (the underlying call returns `RAY_OK`).

## `.log.snapshot` { #log-snapshot }

Signature: `(.log.snapshot)`. Writes a snapshot of current state (env globals serialised into a special frame) and rolls to a new segment. Snapshots let `replay` start from a recent state and only re-apply entries written after.

A lambda marked with [`.sys.trust`](sys.md#sys-trust) keeps its mark in the snapshot, and `-l`/`-L` recovery restores it before replaying the log. Replay lifts a restricted (`-U`) request's trusted calls only as they ran live:

- After a journaled restricted request whose trusted calls ran lifted, the server writes a note. The note names its request by the request's position in the log, so requests logged in between (ones a peer sent back while a trusted body waited on it) do not part the two. It lists the lifted calls in order, each by the verb's identity: a hash of the global name it was trusted under, its parameters, its body and its captures, computed once by `.sys.trust`. A captured lambda counts by its own parameters, body and captures, not by whether it is trusted.
- On replay a call runs lifted only if its lambda is marked and is the next one its request's note lists. A request with no note replays with no lifted call.

When a verb's mark changed between the snapshot and the restart:

- **Trust removed after the snapshot** (the boot script no longer marks the verb): the snapshot still marks it during replay, but a call that was refused live is in no note and is refused again. That holds when another verb of the same text is still trusted, since a note entry names the verb.
- **Trust added after the snapshot**: replay finds the verb unmarked, or not bound at all if the boot script defines it, so a call that ran lifted live is refused on replay until a snapshot holds the verb marked. Take a snapshot after changing what the boot script trusts.
- **The verb's name, parameters, body or captures changed after the snapshot**: its identity changed with them, so calls logged since are refused on replay until a snapshot holds the new verb.

Limits:

- The identity includes the name only for a verb trusted by name and bound directly to that global. A lambda trusted as a value has the identity of its parameters, body and captures alone: two closures of one factory differ by their captures, but two such lambdas that are the same in all three share an identity, so trust verbs by name when one of a pair may later be revoked.
- One lambda is trusted under one name. `.sys.trust` refuses a second name for the same object with `domain`, since recovery would give each binding its own identity. A trusted lambda also bound to a second name without `.sys.trust`, or held in a dict, runs lifted live through either, but a call made through the second name or the dict is refused on replay.
- A trusted call that rolls the log itself (`.log.roll`, or `.log.snapshot`) leaves its request in the old log and its note in the new one, where it names nothing: that request's lift record is dropped. A snapshot already holds what the call wrote before it; after a bare roll the old log is no longer replayed at startup at all.
- If the note cannot be written (disk full, I/O error), the request answers `io`, but its trusted calls already ran and their writes are in effect in the running process. Replay will refuse them, so the state after a restart lacks those writes.
- A crash after a request's trusted calls ran but before its note was written replays that request with no lifted call; its client got no answer.
- A binary that predates notes reads each one as an extra log entry: the note evaluates to its vector of integers, changes nothing and is counted among the replayed entries. Such a binary also loads the snapshot's lambdas unmarked.

Recovery runs before the boot script, so a verb the boot script defines exists during replay only through a snapshot that holds it. The mark is kept only in the snapshot; `ser`, `de` and IPC never carry it. A note is written only by the server's own journal, and a frame of that type arriving on a connection closes it.

Errors: `domain` (no journal open), `io`.

## `.log.roll` { #log-roll }

Signature: `(.log.roll)`. Closes the active segment and opens a fresh one with the next numeric suffix. Useful for time-bounded rotation (cron-driven) without involving a snapshot.

Errors: `domain` (no journal open), `io`.

## `.log.replay` { #log-replay }

Signature: `(.log.replay "path")`. Reads each frame in the file, deserialises the payload, and `eval`s the result. Returns the number of **processed entries**, including entries whose evaluation returned an error. Such errors produce a warning and replay continues with the next entry; the integer is **not** a count of successful evaluations.

This legacy integer result is unchanged. Use [`.log.replay_report`](#log-replay-report) **instead of** `.log.replay` when the caller needs the evaluation-error count; the reporting verb performs its own replay, not an inspection of a previous call. Neither verb rolls back an entry's partial effects or an already-applied prefix. After an error, retry only from an appropriate known state rather than blindly applying the same log again.

Errors are precise enough to distinguish recovery strategies:

| Error code | Meaning | Action |
|---|---|---|
| `badtail` | Framing or version not accepted after a prefix. | Confirm the expected format/version and inspect `.log.validate` before manual recovery; an older reader is not a reason to truncate an intact newer log. |
| `deser` | Frame intact but payload deserialisation failed — content/version skew. | Do **not** truncate; inspect and fix. |
| `decompr` | Compressed-frame decompression failed. | Same as `deser`. |
| `oom` / `io` | Resource exhaustion / read failure. | Resolve the cause and rebuild from a known state before retrying; earlier entries may already have taken effect. |
| `cancel` | Replay interrupted; the log is intact. | No implicit interrupt clearing or rollback. Reconstruct the intended state before retrying. |

```lisp
(.log.replay "/tmp/rayforce-journal.log")
;; => 2   (processed entries, not proof both evaluations succeeded)
```

## `.log.replay_report` { #log-replay-report }

Signature: `(.log.replay_report "path")`. **Replays the file once**, with the same evaluation and continuation rules as `.log.replay`. On a clean tail it returns a dictionary of two I64 fields:

- `entries`: processed entries, including entries that returned an evaluation error.
- `eval_errors`: entries whose evaluation returned an error. A successful null result is not an evaluation error.

The private report is allocated before replay starts; failure to allocate it or intern its keys raises `oom` without applying journal entries. Terminal framing/read/decode/allocation/interruption failures during replay raise the same errors as `.log.replay`, rather than returning a report for that terminal failure. They may follow an already-applied prefix.

For example, for three valid entries with an evaluation error in the middle, a **single** reporting call returns `entries: 3` and `eval_errors: 1`, and the third expression still runs:

```lisp
(set report (.log.replay_report "/tmp/rayforce-journal.log"))
(get report 'entries)      ;; 3
(get report 'eval_errors)  ;; 1
```

This is observability, not fail-fast execution, atomic recovery, rollback, or validation of application invariants. Even `eval_errors == 0` does not certify the expected application prefix. A strict recovery caller must validate the complete expected state before publishing it; partially reconstructed state after an error must not be treated as successful recovery. `.log.validate` checks framing, not those semantic properties.

## `.log.validate` { #log-validate }

Signature: `(.log.validate "path")`. Scans the file framing-only — does not deserialise payloads. Returns a 2-list `(chunks valid_bytes)` where `valid_bytes` is the offset at which framing breaks (= file size if intact).

```lisp
(.log.validate "/tmp/rayforce-journal.log")
;; => (2 92)
```

## `.log.close` { #log-close }

Signature: `(.log.close)`. Flushes and closes the active journal. Returns null. No-op if no journal is open.

## `.log.purge` { #log-purge }

Signature: `(.log.purge)`. Closes the active journal (if open) and deletes its **entire** on-disk footprint: the active `<base>.log`, every rolled `<base>.<stamp>.log` archive, the `<base>.qdb` snapshot, a stray `<base>.qdb.tmp` and the `<base>.lk` lock file. It fails with `io` while another process has the journal open. Takes no path argument — it acts on the current journal, the same one `.log.open` / `.log.write` / `.log.close` operate on (the base survives `.log.close`, so purge works after a close). Returns null on success; errors with `domain` if no journal has been opened in this process. Best-effort delete: a missing file is not an error.

```
(.log.close)
(.log.purge)   ;; no path needed — deletes every segment + snapshot of the active journal
```

## See also

- [IPC & Serialization](../storage/ipc.md) — wire format used by journal frames.

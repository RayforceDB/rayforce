# `.log.*` — write-ahead log

A binary append-only journal of serialised Rayfall expressions. The runtime keeps one journal open at a time. While open, every IPC mutation (and any value explicitly handed to `.log.write`) is appended; `replay` deserialises and re-evaluates each entry to reconstruct state. Files use the same 16-byte IPC header + serialized-payload frames as `.ipc.send`, with optional delta+RLE compression on large entries — the journal is interoperable with the wire format.

The journal is intentionally minimal: there's no per-entry timestamp or transaction grouping at the journal layer. Snapshot + replay is the only recovery model.

!!! note "Restricted under `-U`"
    `.log.open`, `.log.replay`, `.log.roll`, `.log.snapshot`, `.log.close`, `.log.purge` are `RAY_FN_RESTRICTED`. The introspection / I/O-only entries (`.log.write`, `.log.sync`, `.log.validate`, `.log.status`) are unrestricted — they cannot escalate privilege beyond what the IPC peer could already do.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.log.open`](#log-open) | variadic | restricted | Open a journal in async or sync mode. |
| [`.log.status`](#log-status) | variadic | — | Report the journal state as a dict. |
| [`.log.write`](#log-write) | unary | — | Append a serialised expression to the open journal. |
| [`.log.sync`](#log-sync) | nullary | — | `fsync` the journal. |
| [`.log.snapshot`](#log-snapshot) | nullary | restricted | Write a snapshot of current state and roll to a fresh segment. |
| [`.log.roll`](#log-roll) | nullary | restricted | Close the active segment and start a new one. |
| [`.log.replay`](#log-replay) | unary | restricted | Replay a journal file; return entry count. |
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

Signature: `(.log.status)`. Returns a dict describing the journal; read-only, never raises, and answers the same way when no journal was ever opened.

| Key | Type | Meaning |
|---|---|---|
| `open` | bool | a journal handle is present, i.e. writes and auto-journaled requests are being appended |
| `path` | string, or null | the active log file (`<base>.log`); null if no journal was opened |
| `mode` | sym, or null | `'sync` or `'async`; null if no journal was opened |
| `bytes` | i64 | size of the active log in bytes, `0` when `open` is false |
| `auto` | bool | the current evaluation runs inside an auto-journaled IPC request (same value as [`.ipc.auto_journal_eval`](#log-write)) |

`path` and `mode` keep the values of the last journal after `.log.close`, and after a `.log.roll` that failed once the old file was closed (for example the new log could not be created: disk full, out of file descriptors). In that second case the process has no journal handle, sync requests are no longer journaled, and `.log.sync` still returns null; `open` is the only way to see it. An embedder that must not run unjournaled can check it and stop:

```lisp
(if (get (.log.status) 'open) null (exit 1))
```

```lisp
(.log.status)
;; {open:false path:null mode:null bytes:0 auto:false}
(.log.open 'async "/tmp/rayforce-journal")
(.log.status)
;; {open:true path:"/tmp/rayforce-journal.log" mode:async bytes:16 auto:false}
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

Errors: `domain` (no journal open), `io`.

## `.log.roll` { #log-roll }

Signature: `(.log.roll)`. Closes the active segment and opens a fresh one with the next numeric suffix. Useful for time-bounded rotation (cron-driven) without involving a snapshot.

Errors: `domain` (no journal open), `io`.

## `.log.replay` { #log-replay }

Signature: `(.log.replay "path")`. Reads each frame in the file, deserialises the payload, and `eval`s the result. Returns the count of successfully-replayed chunks.

Errors are precise enough to distinguish recovery strategies:

| Error code | Meaning | Action |
|---|---|---|
| `badtail` | Framing broken after some valid prefix. | Truncate to `valid_bytes` from `.log.validate`. |
| `deser` | Frame intact but payload deserialisation failed — content/version skew. | Do **not** truncate; inspect and fix. |
| `decompr` | Compressed-frame decompression failed. | Same as `deser`. |
| `oom` / `io` | Resource exhaustion / read failure. | Retry. |

```lisp
(.log.replay "/tmp/rayforce-journal.log")
;; => 2   (chunks replayed)
```

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

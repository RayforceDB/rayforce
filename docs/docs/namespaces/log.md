# `.log.*` — write-ahead log

A binary append-only journal of serialised Rayfall expressions. The runtime keeps one journal open at a time. While open, every inbound sync IPC request (`.ipc.send`) is appended before it is evaluated, as is any value explicitly handed to `.log.write`; async requests (`.ipc.post`) are appended too only when the [posts switch](#log-posts) is on; `replay` deserialises and re-evaluates each entry to reconstruct state. Files use the same 16-byte IPC header + serialized-payload frames as `.ipc.send`, with optional delta+RLE compression on large entries — the journal is interoperable with the wire format.

The journal is intentionally minimal: there's no per-entry timestamp or transaction grouping at the journal layer. Snapshot + replay is the only recovery model.

!!! note "Restricted under `-U`"
    `.log.open`, `.log.write`, `.log.posts`, `.log.replay`, `.log.replay_report`, `.log.roll`, `.log.snapshot`, `.log.close`, `.log.purge` are `RAY_FN_RESTRICTED`. The introspection / I/O-only entries (`.log.sync`, `.log.validate`, `.log.status`) are unrestricted. Replay executes journal expressions; the reporting variant is **not** a read-only status getter.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.log.open`](#log-open) | variadic | restricted | Open a journal in async or sync mode. |
| [`.log.status`](#log-status) | variadic | — | Report the journal state as a dict. |
| [`.log.write`](#log-write) | unary | restricted | Append a serialised expression to the open journal. |
| [`.log.posts`](#log-posts) | variadic | restricted | Read or set whether async requests are journaled too (off by default). |
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

The server CLI flags `-l` (async) and `-L` (sync) open the journal at startup and, unlike the bare verb, additionally load the latest snapshot and replay the log first — a full crash-recovery, then open-for-append. `--log-posts 1` beside them also journals async requests; see [`.log.posts`](#log-posts).

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
| `posts` | bool | async IPC requests are journaled too: the [posts switch](#log-posts), set by `--log-posts` or `.log.posts`; false by default |

`path` and `mode` are committed only when an open succeeds. A failed `.log.open` therefore never reports an identity (`opened_ok` stays false, `path` and `mode` stay null) and never replaces the remembered one. They keep the values of the last successful journal after `.log.close`, and after a `.log.roll` that failed once the old file was closed (for example the new log could not be created: disk full, out of file descriptors). In that second case the process has no journal handle, requests are no longer journaled, and `.log.sync` still returns null; `open` is false while `opened_ok` is true. An embedder that must not run unjournaled can check it and stop:

```lisp
(if (get (.log.status) 'open) null (exit 1))
```

The getter only reports. Whether a request is acknowledged while the journal is down is a separate IPC policy.

```lisp
(.log.status)
;; {open:false opened_ok:false path:null mode:null bytes:0 auto:false posts:false}
(.log.open 'async "/tmp/rayforce-journal")
(.log.status)
;; {open:true opened_ok:true path:"/tmp/rayforce-journal.log" mode:async bytes:16 auto:false posts:false}
(.log.close)
```

## `.log.write` { #log-write }

Signature: `(.log.write expr)`. Serialises `expr` and appends a frame to the open journal.

Errors: `limit` (`expr` nested deeper than 512 levels, or one that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output: a frame writes it out in full), `noopen` (no journal active — surfaced explicitly so callers don't believe a no-op succeeded), `type` (no arg), `domain` (`expr` has zero serialised size, or the call runs inside an auto-journaled IPC request), `oom`, `io`. Lazy values are forced before serialisation.

Every inbound sync request is auto-journaled, and so is every async request while the [posts switch](#log-posts) is on; `.log.write` raises `domain` inside any of them. With the switch off, the default, an async request is not journaled and `.log.write` works inside it. Inside an auto-journaled IPC request the request itself is the journal entry and is re-evaluated on replay, so an extra frame would apply the change twice. Code shared between journaled request handlers and other paths tests `(.ipc.auto_journal_eval)` and skips its own write there:

```lisp
(if (.ipc.auto_journal_eval) null (.log.write expr))
```

```lisp
(.log.open 'async "/tmp/rayforce-journal")
(.log.write 42)
(.log.write [1 2 3 4 5])
(.log.close)
```

## `.log.posts` { #log-posts }

Signature: `(.log.posts [flag])`. Reads or sets the posts switch, which decides whether an open journal records async IPC requests (`.ipc.post`) as well as sync ones. With no argument it returns the current state; `0` turns it off and any other `i64` turns it on. Returns the state in effect as `i64` (`0` or `1`). The server flag `--log-posts N` sets it at startup (`N` other than `0` turns it on). It is **off by default**, and [`.log.status`](#log-status) reports it as `posts`. Restricted under `-U`.

Errors: `type` (`flag` not an `i64`), `domain` (more than one argument).

```lisp
(.log.posts)      ;; => 0  (off: the default)
(.log.posts 1)    ;; => 1  (async requests are journaled from now on)
(.log.posts 0)    ;; => 0
```

The switch belongs to the process, not to a journal: opening, closing, rolling or purging a journal leaves it as it is. Replay does not change it either: while a log replays, `.log.posts` only reads the switch, so a journaled `(.log.posts 1)` or `(.log.posts 0)` has no effect on recovery. `--log-posts` is applied after the `-l`/`-L` startup replay, so the server runs with what the flag says; without the flag the switch stays off.

**Off** (the default), the journal holds the sync requests and the `.log.write` entries, as it always has. An async request is evaluated without being journaled, so its effects are lost on recovery, and `.log.write` inside it appends an entry of its own.

**On**, each async request is written once, before it is evaluated, as a request frame like a sync one. The log keeps the requests from every connection in the order the server ran them, and replay evaluates an async request the way it evaluates a sync one (see [`.log.replay`](#log-replay)): the same argument marking, a request refused live (`-U`, a parse error) refused again, and no hook run. Replay does not read the switch: a journal that holds async requests replays them whatever the switch is set to. A journal written with the switch off, or by an earlier build, has none and replays as before. Turn the switch on only after checking each of the following.

- **Cost.** Each post becomes a journal write. On x86 with an NVMe disk, 20,000 small posts took 74–160 ms on a server with `-l` and the switch off, and 96–188 ms with it on: about 760 instructions and one `write` system call per post. Under `-L` every write is `fsync`'d, so every post is: 2,000 posts took 13–20 ms with the switch off and 10.1–10.6 s with it on, about 5 ms a post. The server is single-threaded, so this caps an `-L` server at about 200 posts a second. On macOS `fsync` does not force the data to the device, so a timing taken there understates the cost by about 200×.
- **Hooks are not replayed.** An installed `.ipc.on.async` hook is not run on replay, as `.ipc.on.sync` is not: the journaled message is evaluated as if no hook were installed. A hook that filters, counts or rewrites the messages it receives, instead of only evaluating them, rebuilds a different state on replay.
- **Handlers must exist at replay time.** A post such as `(upd x)` calls `upd` again on replay. The `-l`/`-L` startup replay runs before the startup script, so a function the script defines does not exist yet and each such entry fails with `name`. Keep the handlers in the snapshot ([`.log.snapshot`](#log-snapshot) saves every global binding), or define them first and replay with `.log.replay`.
- **Subscribers.** A process that has a journal open and receives posts from a peer, such as a subscriber fed by its publisher, journals every message pushed to it. The log grows with each tick until a `.log.snapshot` or `.log.roll`, each tick is an `fsync` under `-L`, and in the usual setup, where the script defines `upd` and replay runs before the script, every tick fails on replay with `name`. A subscriber usually rebuilds from its publisher instead, and leaves the switch off.
- **`.log.write`** inside an async request raises `domain` while the switch is on, because the request is already the entry (see [`.log.write`](#log-write)). Use `(if (.ipc.auto_journal_eval) null (.log.write expr))` in code that runs both ways.

## `.log.sync` { #log-sync }

Signature: `(.log.sync)`. Forces an `fsync` on the active segment. Returns null. No-op when no journal is open (the underlying call returns `RAY_OK`).

## `.log.snapshot` { #log-snapshot }

Signature: `(.log.snapshot)`. Writes a snapshot of current state (env globals serialised into a special frame) and rolls to a new segment. Snapshots let `replay` start from a recent state and only re-apply entries written after.

A process-local handle has no saved form: a graph from [`.graph.build`](graph.md#graph-build), a vector index from [`.emb.hnsw.build`](emb.md#emb-hnsw-build), an embedding model from [`.emb.model.get`](emb.md#emb-model-get) and a Datalog program from `dl-program` each point into the running process. The snapshot leaves out every global that holds one, alone or anywhere inside a list, dict or table, and prints one line per name on stderr:

```
log: WARN  snapshot skips 'G': it holds a process-local handle (graph, vector index, embedding model or Datalog program)
```

The other globals are saved and the log rolls as usual. After a restart the skipped names are unbound, so rebuild them from saved data (for example in the boot script) before using them. Outside the snapshot nothing changes: a direct `(ser G)` fails with `domain`, and an IPC reply that holds a handle reaches the client as a `type` error ("result of type i64 is not serializable over IPC").

The snapshot is the server's own data and writes every kept global in full: a value bound under several names is saved once per name, whatever its size, outside the [budget of repeated output](../data-types/collections.md#values-that-repeat-their-parts) that `ser` and `.log.write` keep. A global the serializer cannot write (one nested deeper than 512 levels) fails the snapshot with that error's own code and message, `limit` for the depth; nothing is written and the log does not roll.

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

Recovery runs before the boot script, so a verb the boot script defines exists during replay only through a snapshot that holds it. The mark is kept only in the snapshot; `ser`, `de` and IPC never carry it.

A list that arrived as an IPC request argument evaluates to itself when its head does not resolve or names a global that is not a function (see [`.log.replay`](#log-replay)). A handler that stores such a list stores that behaviour with it, and the snapshot keeps it too. With `a` bound to `42`, a handler that appends its argument to `S`, and a request `(keep (a 1 2))`, `(eval (first S))` gives `(a 1 2)` live, after recovery from the log, and after recovery from a snapshot. Like the trust mark, it is written only into the snapshot: `ser`, `de`, IPC and journal entries never carry it.

!!! note "Snapshots written by an earlier build"
    Earlier builds did not write this mark, so recovering from a snapshot evaluated such a stored list as a call: `(eval (first S))` raised `type` (`head is not callable, got i64`). A snapshot written by an earlier build still recovers that way, because it never held the mark. Take a new snapshot to keep it. An earlier build reading a snapshot written by this build ignores the mark and recovers as it did before. A note is written only by the server's own journal, and a frame of that type arriving on a connection closes it.

Errors: `domain` (no journal open), `io`.

## `.log.roll` { #log-roll }

Signature: `(.log.roll)`. Closes the active segment and opens a fresh one with the next numeric suffix. Useful for time-bounded rotation (cron-driven) without involving a snapshot.

Errors: `domain` (no journal open), `io`.

## `.log.replay` { #log-replay }

Signature: `(.log.replay "path")`. Reads each frame in the file, deserialises the payload, and `eval`s the result. Returns the number of **processed entries**, including entries whose evaluation returned an error. Such errors produce a warning and replay continues with the next entry; the integer is **not** a count of successful evaluations.

This legacy integer result is unchanged. Use [`.log.replay_report`](#log-replay-report) **instead of** `.log.replay` when the caller needs the evaluation-error count; the reporting verb performs its own replay, not an inspection of a previous call. Neither verb rolls back an entry's partial effects or an already-applied prefix. After an error, retry only from an appropriate known state rather than blindly applying the same log again.

A frame an IPC request wrote is evaluated directly, with the same argument marking the live request had: each argument list whose head does not resolve, or names a global that is not a function, is data. So `(upd (a 1 "s"))` with `a` bound to `42` passes the list `(a 1 "s")` to `upd` on replay, as it did live. A head naming a function is called, live and on replay. An async request posted with `.ipc.post`, journaled when the [posts switch](#log-posts) was on, replays the same way. An installed `.ipc.on.sync` or `.ipc.on.async` hook is **not** run on replay: the journaled message is evaluated as if no hook were installed, so a hook's own side effects are not replayed, and a hook that does more than evaluate its message rebuilds a different state. A request the live server refused is refused again on replay: one that arrived on a `-U` restricted connection is replayed restricted, and one that failed to parse fails again and counts as an evaluation error. An entry written by `.log.write` is evaluated as written. The `-l`/`-L` startup replay works the same way.

!!! note "A journal written by an earlier build now recovers to the live state"
    The journal format did not change, but earlier builds replayed a request without the argument marking: a data list whose head named a bound global was called during replay. A journal written by an older build therefore recovers to a different state with this build, the state the live server had. With `a` bound to `42` and `upd` appending its argument to `state`, the requests `(upd (zz 1 2))`, `(upd (a 1 "s"))` and `(upd (in 1 2))` left the live `state` at `((zz 1 2) (a 1 "s") false)`. An older build replays them to `(false)`, raising `name` and `type` on the first two, and this build replays them to the live state. A server already recovered with an older build holds the older result until it is recovered again.

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

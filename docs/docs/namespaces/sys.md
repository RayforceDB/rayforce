# `.sys.*` — system info and control

Process-level introspection (build, memory, host info) and command-style operations (shell-out, profiler toggle, IPC listener bind, env dump, GC trigger). The dotted `.sys.<name>` builtins are the typed entry points; `.sys.cmd` parses a colon-style command string and dispatches to the same handlers — used by the REPL's `:listen 5000` style commands.

!!! note "Restricted under `-U`"
    `.sys.exec`, `.sys.cmd`, `.sys.listen`, and `.sys.querylog.enable` are `RAY_FN_RESTRICTED` — they can run arbitrary shell commands, change the process's network surface, or change server-wide logging behaviour. The introspection entries (`.sys.args`, `.sys.build`, `.sys.info`, `.sys.mem`, `.sys.prof`, `.sys.querylog`, `.sys.gc`, `.sys.env`, `.sys.timeit`) are unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.sys.args`](#sys-args) | variadic | — | Command-line arguments as a typed dict. |
| [`.sys.build`](#sys-build) | variadic | — | Version + build date as a dict. |
| [`.sys.info`](#sys-info) | variadic | — | Host facts: cores, page size, total memory. |
| [`.sys.mem`](#sys-mem) | variadic | — | Allocator statistics. |
| [`.sys.prof`](#sys-prof) | variadic | — | Last profiled query's per-step statistics as a table. |
| [`.sys.querylog`](#sys-querylog) | variadic | — | Ambient per-query statistics ring as a table. |
| [`.sys.querylog.enable`](#sys-querylog-enable) | variadic | restricted | Toggle query-statistics logging. |
| [`.sys.gc`](#sys-gc) | variadic | — | Run allocator maintenance and return `0`. |
| [`.sys.env`](#sys-env) | variadic | — | Count or list of globally bound names. |
| [`.sys.exec`](#sys-exec) | unary | restricted | Run a shell command; return its exit code. |
| [`.sys.cmd`](#sys-cmd) | unary | restricted | Dispatch a colon-command string. |
| [`.sys.listen`](#sys-listen) | unary | restricted | Bind an IPC listener on a TCP port. |
| [`.sys.timeit`](#sys-timeit) | variadic | — | Toggle / set the per-expression profiler. |

## `.sys.args` { #sys-args }

Signature: `(.sys.args)`. Returns the process's command-line arguments as a dictionary. Because the launcher flags have a known format, each top-level value carries its natural type; everything after a `--` separator is collected into a `user` subdict (`string → string`), so an application can read its own options without colliding with Rayforce's own flags.

| Key | Type | Source flag | Notes |
|---|---|---|---|
| `file` | str | `-f` / positional | Script path; empty if none. |
| `port` | i64 | `-p` | IPC listen port; `0` if unset. |
| `cores` | i64 | `-c` | Total execution cores, including the main thread; `0` = auto. |
| `timeit` | bool | `-t` | Profiler enabled at startup. |
| `querylog` | bool | `-Q` | Query-statistics logging enabled at startup. |
| `interactive` | bool | `-i` | Force the REPL after a script. |
| `log` | str | `-l` / `-L` | Journal base path; empty if none. |
| `user` | dict | after `--` | The application's own arguments. |

The top-level schema is **stable** — every launcher key is always present with its effective value (the default when the flag wasn't passed), so `(get (.sys.args) 'port)` never misses. Auth passwords (`-u` / `-U`) are deliberately **not** exposed.

**`user` parsing.** Tokens after `--` are paired `-key value` / `--key value`: a token starting with `-` is a key (leading dashes stripped to a symbol), and the next token is its value — unless that token also starts with `-`, in which case the value is the empty string (a bare flag). Duplicate keys keep the last value.

```lisp
;; launched as:  ./rayforce app.rfl -- -opt 123 --verbose
(.sys.args)
;; => {file:"app.rfl" port:0 cores:0 timeit:false interactive:false log:0Nc
;;     user:{opt:"123" verbose:0Nc}}

(at (.sys.args) 'user)     ;; just the application's options
;; => {opt:"123" verbose:0Nc}

(get (.sys.args) 'port)    ;; a typed launcher value
;; => 0
```

Empty strings render as `0Nc` in the REPL (`log` and the bare `verbose` flag above). See [passing arguments to a script](../getting-started/quick-start.md#passing-arguments-to-a-script) for the launcher side.

## `.sys.build` { #sys-build }

Signature: `(.sys.build)`. Returns a dict with `version` (string) and `build-date` (string). Both fall back to `"unknown"` when the build didn't define `RAYFORCE_VERSION` / `RAYFORCE_BUILD_DATE`.

```lisp
(.sys.build)
;; => {version: "0.3.1", build-date: "2026-05-27"}
```

## `.sys.info` { #sys-info }

Signature: `(.sys.info)`. Returns `{cores: i64, page-size: i64, total-mem: i64}` on POSIX. On Windows the response is `{cores: 1}` (a fallback — the sysconf-backed values aren't wired).

```lisp
(.sys.info)
;; => {cores: 10, page-size: 16384, total-mem: 68719476736}
```

## `.sys.mem` { #sys-mem }

Signature: `(.sys.mem)`. Returns the buddy allocator's running counters:

| Key | Meaning |
|---|---|
| `alloc-count` | Cumulative `ray_alloc` calls. |
| `bytes-allocated` | Live bytes in buddy-pool blocks (sub-32 MB objects). |
| `direct-bytes` | Live bytes in **direct** mmaps — large objects (≥ 32 MB) mapped at their exact page-rounded size, bypassing the power-of-2 buddy pool. `bytes-allocated + direct-bytes` is the true live object footprint. |
| `peak-bytes` | High-watermark since process start. |
| `slab-hits` | Cumulative fast-path slab allocations. |
| `sys-current` | **Committed RAM**: every anonymous mapping — the buddy pools (where all vectors live), sys allocations, and the swap-fallback pool. The true live-memory figure. |
| `sys-mapped` | **File-backed** bytes currently mapped (splayed columns, the symbol file, CSV/script parse buffers). Page-cache, evictable, resident only on touch — kept separate from `sys-current` so a read-once column can't masquerade as live RAM. |
| `sys-mapped-peak` | High-watermark of `sys-mapped`. |

Every mapping — buddy pool or file — is counted exactly once through the VM
wrapper layer, so `sys-current` reflects the real committed footprint (a large
`til` shows up here, not as a flat few MB). There is **no enforced memory
ceiling**: the heap is out-of-core and spills to a file-backed mapping when
anonymous `mmap` is refused. Total physical RAM is reported separately by
[`.sys.info`](#sys-info) as `total-mem`; compare it against
`bytes-allocated + direct-bytes` to gauge how close a workload is to spilling.

```lisp
(.sys.mem)
;; => {alloc-count: 12345, bytes-allocated: 524288, peak-bytes: 2097152,
;;     sys-current: 34754560, sys-mapped: 0, sys-mapped-peak: 0}
```

## `.sys.prof` { #sys-prof }

Signature: `(.sys.prof)`. Returns the profile of the **most recently
completed** query as a table — one row per pipeline step (parse, the
optimizer passes, each heavy operator, and materialize). Because it is an
ordinary table it can be queried like any other, and an IPC client gets it
with no extra protocol.

Profiling is opt-in: toggle it with the REPL `:t` command, `.sys.timeit`,
or the `-t` startup flag. With profiling off the table is empty. Capturing
the payload adds no measurable cost to a non-profiled query.

| Column | Meaning |
|---|---|
| `operator` | Step / operator label (symbol). |
| `depth` | Nesting depth (0 = top level). |
| `cumulative-ms` | Wall-clock milliseconds, **including** child operators. |
| `exclusive-ms` | Self time — this step minus its children (where time actually went). |
| `percent` | Share of total query time, exclusive-based (columns sum to ~100). |
| `rows` | Result element / row count (operators). |
| `output-kib` | Result serialized footprint, KiB (bandwidth produced). |
| `allocated-kib` | Net process bytes allocated across the step, KiB. |
| `workers` | Worker threads that ran a task for this step. |
| `busy-ms` | Summed worker busy time, ms. |
| `parallelism` | Effective parallelism = `busy-ms` / `cumulative-ms` (worker time ÷ wall time). |

Timing follows the same convention as `EXPLAIN ANALYZE`-style profilers:
`cumulative-ms` is the wall time of a step *and everything nested under it*,
while `exclusive-ms` strips the children out so the true hot step stands out
(a `count` over a huge `til` shows a large cumulative but a tiny exclusive —
the `til` underneath is the real cost). `percent` ranks steps by exclusive
time, so scanning that column finds the bottleneck directly.

```lisp
;; run a query with the profiler on (:t 1), then:
(.sys.prof)
;; a table: operator | depth | cumulative-ms | exclusive-ms | percent | rows |
;;          output-kib | allocated-kib | workers | busy-ms | parallelism
;; e.g. the GROUP row → rows=5000, output-kib=78.2, workers=28, parallelism=7.2

;; it is a normal table, so query it — rank steps by self time:
(select {from: (.sys.prof) where: (> exclusive-ms 1.0)})
```

The `:t` command also prints this as an indented tree after each query,
with self time, percent, and the same payload appended to every operator line.

## `.sys.querylog` { #sys-querylog }

Signature: `(.sys.querylog)`. Returns the **query-statistics ring** as a table —
one row per completed query, oldest first, capped at the ring capacity (4096).
This is the ambient, server-side statistics feed: when logging is enabled the
server records a summary of every query it handles, and you read it back with an
ordinary query (an IPC client needs no extra protocol). The model mirrors a
`system.query_log` table.

Because each call returns a fresh materialized snapshot, anything you do to the
returned table (filter, sort, `upsert`) touches the copy, never the ring; and
`.sys.querylog` is a reserved name, so user code cannot rebind it. Empty until
logging is enabled with the `-Q` startup flag or `(.sys.querylog.enable 1)`.

| Column | Meaning |
|---|---|
| `time` | Wall-clock time the query finished (timestamp). |
| `duration-ms` | Total wall time to answer the query. |
| `result-rows` | Rows in the result (a scalar counts as 1). |
| `output-kib` | Serialized result footprint, KiB. |
| `memory-kib` | Net process allocation across the query, KiB. |
| `workers` | Worker threads that ran a task. |
| `parallelism` | Effective parallelism = worker busy time ÷ wall time. |
| `status` | `ok`, or the error kind (e.g. `type`, `name`) on failure. |
| `query` | Source text (truncated to 256 chars). |

```lisp
;; start the server with -Q 1 (or toggle at runtime), then:
(.sys.querylog)
;; a table: time | duration-ms | result-rows | output-kib | memory-kib |
;;          workers | parallelism | status | query

;; it is a normal table, so query it — slowest queries first:
(select {from: (.sys.querylog) where: (> duration-ms 100.0)})

;; error rate:
(select {from: (.sys.querylog) by: status total: (count status)})
```

Capturing a row adds no measurable cost to a query, and logging is off by
default — the disabled path is a single predicted branch.

## `.sys.querylog.enable` { #sys-querylog-enable }

Signature: `(.sys.querylog.enable [flag])`. Toggles query-statistics logging.
No argument flips the current state; `0` disables; anything non-zero enables.
Returns the new state as `i64` (0/1). Restricted under `-U` — it changes
server-wide behaviour.

```lisp
(.sys.querylog.enable 1)   ;; => 1  (logging on)
(.sys.querylog.enable)     ;; => 0  (flip: now off)
```

The `-Q` startup flag is the equivalent for enabling logging from launch.

## `.sys.gc` { #sys-gc }

Signature: `(.sys.gc)`. Runs allocator maintenance: drains cross-thread frees,
flushes slab caches, coalesces free buddy blocks, reclaims empty oversized
pools, and incrementally returns aged free pages to the operating system.
Rayforce values remain deterministically reference-counted—this is allocator
GC, not a tracing object collector. Returns `0`.

## `.sys.env` { #sys-env }

Signature: `(.sys.env)`. From a script / IPC context returns the **count** of globally bound names (i64). In a REPL context the same dispatcher prints one line per binding (name + type label) and returns null — the variadic registration accommodates both forms.

```lisp
(.sys.env)
;; => 42  (number of global bindings)
```

## `.sys.exec` { #sys-exec }

Signature: `(.sys.exec "command")`. Runs `command` through `system(3)` and returns its exit status as an `i64`. Used pervasively in tests to set up / tear down `/tmp` fixtures.

Errors: `type` (arg not a string), `domain` (null pointer).

```lisp
(.sys.exec "rm -rf /tmp/scratch")   ;; => 0
(.sys.exec "false")                  ;; => 256  (waitpid-encoded)
```

## `.sys.cmd` { #sys-cmd }

Signature: `(.sys.cmd "name args")`. Parses a colon-style command name (without the leading `:`) and dispatches. Names supported: `timeit`/`t`, `listen`, `env`, `clear`, `help`/`?`, `q`/`quit`. Unknown names fall back to `system()` (so `(.sys.cmd "ls /tmp")` works too).

`clear`, `help`, and `q` are REPL-only — invoking them from a script returns a `domain` error rather than silently no-op'ing.

```text
(.sys.cmd "listen 5000")   ;; => listener id
(.sys.cmd "env")           ;; => binding count
(.sys.cmd "timeit 1")      ;; => 1   (profiler enabled)
```

## `.sys.listen` { #sys-listen }

Signature: `(.sys.listen port)`. Binds an IPC listener on `port` using the runtime's main poll. Returns the listener id (i64).

Errors: `type` (port not an int / not parseable from string), `domain` (port outside `[1, 65535]`), `nyi` (no main loop — embedded library without a poll), `io` (`bind` failure, e.g. port already in use).

```text
(.sys.listen 5000)
;; => 12  (listener id)
```

## `.sys.timeit` { #sys-timeit }

Signature: `(.sys.timeit [flag])`. Toggles the per-expression profiler. Calling with no argument flips the current state; passing `0` disables, anything non-zero enables. Returns the new state as `i64` (0/1).

```lisp
(.sys.timeit 1)   ;; enable profiling
(sum (til 1000))
;; (timings printed for each subexpression)
(.sys.timeit 0)   ;; disable
```

## See also

- [`.os.*`](os.md) — filesystem and env-variable primitives that don't shell out.
- [`.ipc.*`](ipc.md) — the client side; `.sys.listen` binds the server.
- [`.time.*`](time.md) — `.time.now` / `.time.timer.set` for measurement and scheduling without the profiler.

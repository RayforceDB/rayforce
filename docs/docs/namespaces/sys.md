# `.sys.*` — system info and control

Process-level introspection (build, memory, host info) and command-style operations (shell-out, profiler toggle, IPC listener bind, env dump, GC trigger). The dotted `.sys.<name>` builtins are the typed entry points; `.sys.cmd` parses a colon-style command string and dispatches to the same handlers — used by the REPL's `:listen 5000` style commands.

!!! note "Restricted under `-U`"
    `.sys.exec`, `.sys.cmd`, `.sys.listen`, `.sys.querylog.enable`, `.sys.join.par.min`, `.sys.profile.enable`, `.sys.profile.reset` and `.sys.trust` are `RAY_FN_RESTRICTED` — they can run arbitrary shell commands, change the process's network surface, change server-wide logging, profiling or execution behaviour, or hand restricted callers more rights. The introspection entries (`.sys.args`, `.sys.build`, `.sys.info`, `.sys.mem`, `.sys.prof`, `.sys.profile`, `.sys.querylog`, `.sys.querylog.stats`, `.sys.gc`, `.sys.env`, `.sys.timeit`) are unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.sys.args`](#sys-args) | variadic | — | Command-line arguments as a typed dict. |
| [`.sys.build`](#sys-build) | nullary | — | Version + build date as a dict. |
| [`.sys.info`](#sys-info) | nullary | — | Host and process facts: cores, page size, total memory, pool size, pid, hostname. |
| [`.sys.mem`](#sys-mem) | nullary | — | Allocator statistics. |
| [`.sys.prof`](#sys-prof) | nullary | — | Last profiled query's per-step statistics as a table. |
| [`.sys.profile`](#sys-profile) | nullary | — | Per-call-site profile of Rayfall code (file, line, function) as a table. |
| [`.sys.profile.enable`](#sys-profile-enable) | variadic | restricted | Toggle the call profiler. |
| [`.sys.profile.reset`](#sys-profile-reset) | nullary | restricted | Drop the call profiler's rows. |
| [`.sys.querylog`](#sys-querylog) | variadic (0–1) | — | Ambient per-query statistics ring as a table; `(.sys.querylog n)` only the rows from `seq` `n` on. |
| [`.sys.querylog.stats`](#sys-querylog-stats) | nullary | — | The ring's counters: the next `seq`, the rows overwritten, the capacity. |
| [`.sys.querylog.enable`](#sys-querylog-enable) | variadic | restricted | Toggle query-statistics logging. |
| [`.sys.join.par.min`](#sys-join-par-min) | variadic (0–1) | restricted | Read or set the probe-side row count from which a hash join's probe is split across the pool. |
| [`.sys.gc`](#sys-gc) | variadic (0–1) | — | Run allocator maintenance and return `0`; `(.sys.gc 1)` also releases free pages now, best effort. |
| [`.sys.env`](#sys-env) | variadic (0–1) | — | Count or list of globally bound names. |
| [`.sys.exec`](#sys-exec) | variadic | restricted | Run a shell command; return its exit code, optionally with stdout. |
| [`.sys.cmd`](#sys-cmd) | unary | restricted | Dispatch a colon-command string. |
| [`.sys.listen`](#sys-listen) | unary | restricted | Bind an IPC listener on a TCP port. |
| [`.sys.trust`](#sys-trust) | unary | restricted | Let restricted (`-U`) callers run these lambdas with the restriction lifted. |
| [`.sys.timeit`](#sys-timeit) | variadic (0–1) | — | Toggle / set the per-expression profiler. |

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
| `source` | str | — | The file currently being evaluated: the innermost `load`, or the command-line script, by the path that was actually opened. Empty at the REPL, under a pipe, or in a hook or timer that runs outside any file. `file` is the script named on the command line (`$0`); `source` follows nested loads (`$BASH_SOURCE`). |
| `user` | dict | after `--` | The application's own arguments. |

The top-level schema is **stable** — every launcher key is always present with its effective value (the default when the flag wasn't passed), so `(get (.sys.args) 'port)` never misses. Auth passwords (`-u` / `-U`) and the RAM budget (`-m` / `--mem`) are deliberately **not** exposed; the effective budget is [`anon-watermark`](#sys-mem) in `(.sys.mem)`.

```lisp
;; load a sibling of the running file, whichever directory it was started from
(set here (str-join (drop -1 (split (at (.sys.args) 'source) "/")) "/"))
(load (concat here "/lib.rfl"))
```

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

Signature: `(.sys.info)`. Returns `{cores: i64, page-size: i64, total-mem: i64, workers: i64, pid: i64, hostname: str}` on every platform (on Windows from `GetSystemInfo` / `GlobalMemoryStatusEx`).

- `cores`: the CPUs this process may run on. On Linux that is its affinity mask, which `taskset`, a cpuset or `docker --cpuset-cpus` may narrow below the machine's CPU count.
- `workers`: the threads of the execution pool, main included; before the pool exists, the size it will get. `.sys.info` does not create the pool (see [Running Several Instances](../guides/instances.md)).

```lisp
(.sys.info)
;; => {cores: 10, page-size: 16384, total-mem: 68719476736,
;;     workers: 10, pid: 48213, hostname: "tp-prod-01"}
```

`pid` and `hostname` describe *this process*, not the machine, and exist so an
embedded process can name itself. The common use is a per-start epoch that a
peer can check a stale cursor against:

```lisp
(format "%-%-%" (get (.sys.info) 'hostname)
                (get (.sys.info) 'pid)
                (timestamp 'global))
;; => "tp-prod-01-48213-2026.09.18D10:05:59.482917304"
```

That identity is worth preferring over a random token because it is
diagnosable: reading it back tells you which host and which process produced
it. `(timestamp 'global)` carries the system clock's sub-second part, so two
starts rarely share a stamp; pid is what still separates them when they do.

If `hostname` cannot be obtained, it reports `""` rather than failing the whole
call — one unavailable field should not cost the caller the others.

## `.sys.mem` { #sys-mem }

Signature: `(.sys.mem)`. Returns the allocator's running counters, the spill
watermark, a few diagnostic counters and the symbol-table size, all `i64`:

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
| `anon-committed` | **Anonymous RAM** the heap has committed: buddy pools, direct blocks and sys blocks (off-heap tables such as the symbol arenas). Spill-file mappings are not counted. The figure checked against `anon-watermark`. |
| `anon-peak` | High-watermark of `anon-committed` since process start — the peak RAM the workload demanded, spill excluded. |
| `anon-watermark` | **Spill threshold** in bytes: a new pool, direct or sys block that would push `anon-committed` past it is mapped over a disk spill file instead of anonymous RAM. Set by `-m SIZE` / `--mem SIZE`; by default the RAM limit (physical RAM, or the cgroup memory limit when smaller) less a quarter of it, at least 3 GiB and at most half, left to the page cache. |
| `decode-cache-bytes` | Bytes held by the decoded copies of compressed table columns kept from one statement to the next — a string column's pool and a parted column's segments included. The cache is trimmed to a quarter of `anon-watermark` at statement boundaries (after each IPC request and each REPL line; a script trims at its end). |
| `locked-bytes` | Bytes of the heap's mappings locked against swap-out (Linux): the mapped size, which is what `RLIMIT_MEMLOCK` counts, not the resident pages — a page is locked when it is first touched. At most the smaller of `anon-watermark` and half the RAM limit. |
| `lock-failed` | Times the kernel refused a lock, or a page release could not lock its range again, since start. |
| `lock-skipped` | Bytes, summed since start, that the heap mapped under the watermark but left unlocked: past the bound, past `RLIMIT_MEMLOCK`, or refused. The heap works on, unlocked. |
| `group-perpart-runs` | Diagnostic: results served by the streaming parted `by:` (GROUP) kernel — one per query, not per partition; a run that falls back to the flatten path is not counted. |
| `group-boxed-i64-runs` | Diagnostic: serial `group` invocations over a LIST that hashed integer atoms by their exact value — a list of I64 atoms, or any list with no float atom and no TIMESPAN at any depth — one per invocation, not per key. |
| `asof-perpart-runs` | Diagnostic: days processed by the streaming parted asof join — one per day in the union of both sides' partitions. |
| `sort-perpart-runs` | Diagnostic: partitions processed by the streaming parted `asc:` / `desc:` paths (ordered concat of pre-sorted partitions, or per-partition top-k). |
| `window-perpart-runs` | Diagnostic: segments processed by the streaming parted window path (the window's partition key includes the table's partition key). |
| `join-perpart-runs` | Diagnostic: segments processed by the broadcast-streaming join (one side parted, each segment joined against the whole other side). |
| `sym-map-runs` | Diagnostic: calls of `upper` / `lower` / `trim`, `substr` at scalar bounds, `replace` with atom arguments or `concat` with constants over a SYM column of 4096 rows or more that ran once per distinct symbol — one per call, not per symbol. |
| `sym-map-row-runs` | Diagnostic: of those, the calls over a column whose dictionary has more than 64 entries per row (a few rows of a column over a large symbol table): the rows are transformed, no pass over the dictionary. |
| `sym-map-same-runs` | Diagnostic: of those, the calls in which every symbol mapped to its own string (`trim` of trimmed text, `lower` of lowercase): the column is the result, nothing interned. |
| `sym-count` | Entries in the global symbol table. |
| `sym-bytes` | Bytes held by the symbol table's string arena (every interned name). |

Every mapping — buddy pool or file — is counted exactly once through the VM
wrapper layer, so `sys-current` reflects the real committed footprint (a large
`til` shows up here, not as a flat few MB). `anon-watermark` is a **spill
threshold, not a hard cap**: work past it is never rejected, it goes to a
file-backed spill mapping (as it does when anonymous `mmap` is refused)
instead of risking the OOM killer — see
[out-of-core spill](../guides/memory.md#8-total-ram-and-out-of-core-spill).
Compare `anon-committed` against `anon-watermark` to gauge how close a
workload is to spilling; total physical RAM is reported separately by
[`.sys.info`](#sys-info) as `total-mem`.

The `*-perpart-runs` keys are monotonic diagnostic counters: a delta across a
query shows whether a parted table was streamed partition by partition
instead of flattened. `sym-count` / `sym-bytes` show whether a query left the
symbol table where it found it.

```lisp
(.sys.mem)
;; => {alloc-count: 786, bytes-allocated: 28672, direct-bytes: 0,
;;     peak-bytes: 28672, slab-hits: 354, sys-current: 34836480,
;;     sys-mapped: 0, sys-mapped-peak: 0, anon-committed: 34795520,
;;     anon-peak: 34799616, anon-watermark: 50178081792,
;;     decode-cache-bytes: 0,
;;     group-perpart-runs: 0, group-boxed-i64-runs: 0, asof-perpart-runs: 0,
;;     sort-perpart-runs: 0,
;;     window-perpart-runs: 0, join-perpart-runs: 0,
;;     sym-map-runs: 0, sym-map-row-runs: 0, sym-map-same-runs: 0,
;;     sym-count: 424, sym-bytes: 30336}
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

## `.sys.profile` { #sys-profile }

Signature: `(.sys.profile)`. Returns the **call profile** as a table: one row
per Rayfall call site that ran while the call profiler was on, highest self
time first. Where `.sys.prof` breaks one query into engine operators,
`.sys.profile` names the Rayfall code that spent the time — which line of
which function — across everything the process ran, IPC requests included.

A call site is a builtin call inside a compiled function (a `select`, a
`left-join`, a `+`) or a call written in top-level code. Calls from one
compiled function to another are not rows of their own: the callee's calls
are its rows.

A site is the code at a place in the source: its file, line and column,
and what is there. A top-level call is told apart by its callee (`(work
10)` and `(work 11)` share the `<top>work` row; `(later 5)` has its own),
a call inside a function by the function's own place. Every IPC request
and REPL line is parsed from line 1 of `repl`, so there the text tells
sites apart too: lambdas written on different lines are different rows.
Running the same text again adds to the rows it had, so a client sending
the same request a thousand times, or a lambda written inside a request,
keeps one set of rows. Each distinct site counts toward the 4096-site cap;
calls at sites past it land in the `<other>` row until
`(.sys.profile.reset)`.

| Column | Meaning |
|---|---|
| `file` | Source file of the call (`repl` for typed or IPC text). |
| `line` | 1-based line of the call; `0` when unknown. |
| `fn` | The global the enclosing function is bound to (dotted names included); `<lambda@N>` for a function bound to no global, whose body starts on line N; `<top>NAME` for a top-level call to `NAME` (`<top>` when the head is not a name); `<other>` for calls past the 4096-site cap. |
| `calls` | Times the call ran. Errors count. |
| `self-ms` | Wall time inside the call, less the timed calls it made (a lambda run by `map`, say). Sums to the profiled time. |
| `total-ms` | Wall time inside the call, its nested calls included. A site that calls itself counts its outermost activation. |
| `busy-ms` | Time the worker pool spent while the call ran, less the timed calls it made: the parallel work the call dispatched. The call waits for the pool, so this is where pool time lands. |

Profiling is off by default: `(.sys.profile.enable 1)` turns it on, and rows
accumulate until `(.sys.profile.reset)`. Turning it off keeps the rows. Off,
each call pays one predicted branch; on, each call reads the clock twice and
updates its row, about 25 ns per call. A loop of cheap builtin calls (`<`,
`+`) runs 1.5-1.9x slower profiled; beside a call that does real work (a
join, a `select`) the cost vanishes.

The profiler times the main thread, which runs the REPL, scripts and IPC
requests. Calls on other threads that own a VM (an embedding host's, an IPC
hook thread's) are not timed. Functions made inside a query run on the tree
walker; their time stays in the call that ran them.

### Finding the hot spot in a running service

```lisp
;; over IPC or at the REPL of the running process
(.sys.profile.enable 1)
;; ... let it serve traffic for a while ...
(.sys.profile.enable 0)

;; where the time went, by call site
(select {from: (.sys.profile) where: (> self-ms 10.0)})

;; by function
(select {self: (sum self-ms) busy: (sum busy-ms) calls: (sum calls)
         from: (.sys.profile) by: fn})

;; the calls that kept the worker pool busy
(select {from: (.sys.profile) where: (> busy-ms 0.0)})

(.sys.profile.reset)   ;; start the next measurement from zero
```

A row with a large `busy-ms` beside a small `self-ms` is a call that spread
its work over the pool — a join or a grouped `select`, typically; a large
`self-ms` with `busy-ms` near zero ran on the calling thread alone.

## `.sys.profile.enable` { #sys-profile-enable }

Signature: `(.sys.profile.enable [flag])`. Toggles the call profiler, as
`.sys.querylog.enable` toggles logging: no argument flips the current state,
`0` disables, anything non-zero enables. Returns the new state as `i64`
(0/1). Restricted under `-U`.

```lisp
(.sys.profile.enable 1)   ;; => 1  (profiling on)
(.sys.profile.enable)     ;; => 0  (flip: now off)
```

## `.sys.profile.reset` { #sys-profile-reset }

Signature: `(.sys.profile.reset)`. Drops every row of the call profile;
whether profiling is on does not change. Returns null. Restricted under `-U`.

## `.sys.querylog` { #sys-querylog }

Signature: `(.sys.querylog)` or `(.sys.querylog n)`. Returns the
**query-statistics ring** as a table — one row per completed query, oldest
first, capped at the ring capacity (4096). With `n`, only the kept rows whose
`seq` is `n` or more.
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
| `seq` | The row's number among all queries logged since the process started: 1, 2, 3, … with no gap and no repeat, whichever connection a query came from. |
| `time` | Wall-clock time the query finished (timestamp, nanosecond resolution). |
| `duration-ms` | Total wall time to answer the query. |
| `result-rows` | Rows in the result (a scalar counts as 1). |
| `memory-kib` | Net process allocation across the query, KiB. |
| `workers` | Worker threads that ran a task. |
| `parallelism` | Effective parallelism = worker busy time ÷ wall time. |
| `status` | `ok`, or the error kind (e.g. `type`, `name`) on failure. |
| `query` | Source text (truncated to 256 chars). |

```lisp
;; start the server with -Q 1 (or toggle at runtime), then:
(.sys.querylog)
;; a table: seq | time | duration-ms | result-rows | memory-kib |
;;          workers | parallelism | status | query

;; it is a normal table, so query it — slowest queries first:
(select {from: (.sys.querylog) where: (> duration-ms 100.0)})

;; error rate:
(select {from: (.sys.querylog) by: status total: (count status)})
```

Capturing a row adds no measurable cost to a query, and logging is off by
default — the disabled path is a single predicted branch.

**Reading incrementally.** A poller keeps the `seq` it has read up to and asks
for the rows after it: `(.sys.querylog (+ upto 1))` builds only those rows, so a
poll costs what is new, not the whole ring, and a `select` of the columns it
needs leaves the `query` text out. `n` past the next `seq` gives an empty table
with the same columns; `n` below the oldest kept row gives every kept row. A
negative `n` is a `domain` error and anything but an `i64` a `type` error. When
the first row's `seq` is above the `n` asked for, the ring wrapped in between
and the difference is the number of rows lost.

```lisp
(set upto 0)
(set rows (select {seq: seq status: status duration-ms: duration-ms from: (.sys.querylog (+ upto 1))}))
(set lost (if (> (count rows) 0) (- (first (at rows 'seq)) (+ upto 1)) 0))
(set upto (if (> (count rows) 0) (last (at rows 'seq)) upto))
```

## `.sys.querylog.stats` { #sys-querylog-stats }

Signature: `(.sys.querylog.stats)`. Returns the ring's counters as a dict:
`next`, the `seq` the next logged query will get; `dropped`, the rows
overwritten since the process started (logged beyond the capacity); and
`capacity`, 4096. Unrestricted, like `.sys.querylog`.

```lisp
(.sys.querylog.stats)   ;; => {next:4201 dropped:104 capacity:4096}
```

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

While a journal replays (`.log.replay`, or the `-l`/`-L` startup replay), a
journaled `.sys.querylog.enable` changes nothing and returns the current state,
so recovering a log never turns logging on or off behind `-Q`.

## `.sys.join.par.min` { #sys-join-par-min }

Signature: `(.sys.join.par.min [rows])`. Reads or sets the probe-side row count
from which a hash join's probe is split across the thread pool. No argument
returns the current value; an `i64` of `0` or more sets it. Either way it
returns the value in effect. Anything else is an error: a negative or null
count is a `domain` error, a non-`i64` a `type` error, more than one argument an
`arity` error. Restricted under `-U`, since it changes execution for the whole
process.

The default is `8193`. The probe runs in morsels of 8192 rows, and a probe of
more than one morsel goes to the pool, so values from `0` to `8193` all give the
default behaviour. Raising the value makes probes below it run on the calling
thread. That trades latency for CPU: past a few workers, a small parallel probe
costs more CPU than it saves in wall time. Most of the extra is the main thread
spinning while it waits for the slowest worker, not extra work. The answer is
the same either way, rows and row order included.

```lisp
(.sys.join.par.min)          ;; => 8193   (the default)
(.sys.join.par.min 65536)    ;; => 65536  probes under 65536 rows stay serial
(.sys.join.par.min 8193)     ;; => 8193   back to the default
```

Set it in the startup script of a process that is not latency-bound. `-c` sets
how many threads the pool may use; this knob sets how large a join must be
before it uses them. The pool stays available for the rare large job while the
steady small joins run serially. See
[parallel probe floor](../queries/joins.md#parallel-probe-floor) for which joins
it covers and the measured trade-off.

## `.sys.gc` { #sys-gc }

Signature: `(.sys.gc [mode])`. Runs allocator maintenance: drains cross-thread frees,
flushes slab caches, coalesces free buddy blocks, reclaims empty oversized
pools, and incrementally returns aged free pages to the operating system.
Rayforce values remain deterministically reference-counted—this is allocator
GC, not a tracing object collector. Returns `0`.

`mode` is `0` (the same as no argument) or `1`. `(.sys.gc 1)` also runs the
ten-second idle decay's sweep now, without waiting for the process to go
quiet or for free blocks to age, and unmaps the cached large blocks, which
the decay keeps. A server that a timer or a client keeps busy never goes
quiet for that long, so this is how it gives back a peak it has finished
with; the next use of that memory faults the released pages in again. It is
best effort, not a promise that every free page leaves the process: the
sweep covers the calling thread's heap and the idle workers' heaps, a block
keeps its first page and one under 8 KiB keeps all of them, a spill pool's
memory goes back only with the pool, and when resident memory drops is up
to the operating system. It returns `0`, as `(.sys.gc)` does. Another mode
is a `domain` error, a non-`i64` a `type` error, and more than one argument
an `arity` error. See
[Giving a peak back](../guides/memory.md#giving-a-peak-back).

```lisp
(.sys.gc)    ; 0
(.sys.gc 1)  ; 0 — free pages released now, not after an idle spell
```

## `.sys.env` { #sys-env }

Signature: `(.sys.env [compat-arg])`. From a script / IPC context returns the **count** of globally bound names (i64). In a REPL context the same dispatcher prints one line per binding (name + type label) and returns null. At most one optional compatibility argument is accepted; additional arguments return an `arity` error.

```lisp
(.sys.env)
;; => 42  (number of global bindings)
```

## `.sys.exec` { #sys-exec }

Signature: `(.sys.exec "command")` or `(.sys.exec "command" 'out)`.

Runs `command` through the shell. The one-argument form returns the **exit
code** as an `i64` — the code the shell itself would report, not the
`waitpid`-encoded status. A command killed by a signal reports `128 + signum`,
following the same shell convention.

The `'out` form returns `{code: i64, out: str}`, capturing the command's
standard output. Output is read until EOF and is not sized from `stat`, so
pipes and procfs reads come back whole. **stderr is not captured** — it stays
on the process's own stderr, so a command's diagnostics land in the log rather
than corrupting a value you are about to parse.

Errors: `type` (command not a string, or mode not a symbol), `domain` (null
pointer, or a mode other than `'out`), `io` (the command could not be started
or did not complete), `oom`.

```lisp
(.sys.exec "rm -rf /tmp/scratch")        ;; => 0
(.sys.exec "false")                      ;; => 1
(.sys.exec "exit 3")                     ;; => 3
(.sys.exec "echo hi" 'out)               ;; => {code: 0, out: "hi\n"}
(.sys.exec "echo partial; exit 4" 'out)  ;; => {code: 4, out: "partial\n"}
```

!!! warning "`'out` waits for every process holding the pipe"
    The capture reads until end-of-file on the command's stdout, and a
    backgrounded child inherits that pipe. So `'out` waits for the background
    process too, not just for the shell:

    ```lisp
    (.sys.exec "(sleep 3 &) ; true" 'out)           ;; returns after 3s
    (.sys.exec "./server &" 'out)                   ;; waits for the server
    ```

    Detach the child's stdout and the call returns at once:

    ```lisp
    (.sys.exec "(sleep 3 &) >/dev/null 2>&1" 'out)  ;; returns immediately
    ```

    This is inherent to capturing output — `popen(3)` behaves the same way, as
    do the equivalents in other languages. The one-argument form does **not**
    have it, since it only waits for the shell itself, so prefer plain
    `(.sys.exec "./server &")` to launch something that outlives the call.

!!! warning "Behaviour change"
    Before this, the one-argument form returned the raw `waitpid`-encoded
    status, so `(.sys.exec "false")` was `256` and `(.sys.exec "exit 3")` was
    `768`. Success was `0` either way, so code that only checks for zero is
    unaffected; code comparing against an encoded value needs updating.

## `.sys.cmd` { #sys-cmd }

Signature: `(.sys.cmd "name args")`. Parses a colon-style command name (without the leading `:`) and dispatches. Names supported: `timeit`/`t`, `listen`, `env`, `clear`, `help`/`?`, `q`/`quit`. An unknown name hands the whole string to the shell (so `(.sys.cmd "ls /tmp")` works too) and returns the command's **exit code**, as [`.sys.exec`](#sys-exec) does: the code of a normal exit, and `128 + signum` for a command a signal killed (`143` for `SIGTERM`).

`clear`, `help`, and `q` are REPL-only — invoking them from a script returns a `domain` error rather than silently no-op'ing.

```text
(.sys.cmd "listen 5000")   ;; => listener id
(.sys.cmd "env")           ;; => binding count
(.sys.cmd "timeit 1")      ;; => 1   (profiler enabled)
(.sys.cmd "exit 3")        ;; => 3   (the shell's exit code)
```

!!! warning "Behaviour change"
    Before this, the shell fallback returned the raw `waitpid`-encoded status
    `system()` gives, so `(.sys.cmd "exit 3")` was `768`, `(.sys.cmd "false")`
    was `256`, and a command killed by `SIGTERM` was `15`. Success was `0`
    either way, so code that only checks for zero is unaffected; code
    comparing against an encoded value needs updating.

## `.sys.listen` { #sys-listen }

Signature: `(.sys.listen port)`. Binds an IPC listener on `port` using the runtime's main poll. Returns the listener id (i64).

Errors: `type` (port not an int / not parseable from string), `domain` (port outside `[1, 65535]`), `nyi` (no main loop — embedded library without a poll), `io` (`bind` failure, e.g. port already in use).

```text
(.sys.listen 5000)
;; => 12  (listener id)
```

## `.sys.trust` { #sys-trust }

Signature: `(.sys.trust f)`, `(.sys.trust 'name)` or `(.sys.trust ['a 'b])`. Marks lambdas as trusted and returns its argument.

Under `-U` a restricted connection may not call a restricted builtin or write a global, and the rule covers everything its request evaluates, including the server's own functions that the request calls. A call to a trusted lambda is the exception: its body runs with the restriction lifted, and the restriction is back as soon as the call returns or fails. A `-U` server can so keep read verbs that need a restricted builtin themselves, such as a read that asks another process for its half over `.ipc.send`, while its clients stay read-only:

```lisp
;; boot script of a server started with -U
(set peer (.ipc.open "127.0.0.1:5001"))
(set peer_count (fn [] (.ipc.send peer (parse "(count (til 3))"))))
(.sys.trust 'peer_count)
```

```text
;; a client of that server
(.ipc.send h "(peer_count)")                     ;; => 3
(.ipc.send h "(.ipc.send peer (parse \"1\"))")   ;; => error: access
```

- Only unrestricted code can mark: `.sys.trust` is restricted itself, so it runs in a boot script, at the REPL or inside a trusted body, never in a restricted request.
- The mark is on the lambda object, not on the name: binding the name to a new lambda leaves the new one unmarked.
- It does not travel: a copy made by `ser`/`de` or sent over IPC has none, and neither does a closure the trusted body makes and returns.
- A journal snapshot keeps it. [`.log.snapshot`](log.md#log-snapshot) writes the marks of the lambdas it holds, and `-l`/`-L` recovery restores them before it replays the log. Replay lifts a logged restricted request's trusted calls only as they ran lifted live, recorded in a journal note that names each verb, so a call refused live stays refused even if the snapshot still marks the verb. [`.log.snapshot`](log.md#log-snapshot) lists the limits. A call that ran lifted live replays lifted only if the snapshot holds the verb marked: take a snapshot after changing what the boot script trusts. A reader older than this mark loads such a snapshot with its lambdas unmarked.
- A name means the global bound to it, as at the top level. A dotted name (`'ns.verb`) is refused: to trust a lambda held in a dict, pass the lambda itself, `(.sys.trust ns.verb)`. A local of the same name in the code calling `.sys.trust` is never the one marked. Each name must hold a lambda; if one does not, nothing is marked. One lambda is trusted under one name: a second name for the same object is refused.
- Called by restricted code, the body also runs isolated from its caller, as if called at the top level. The names it reads, and the names read by what it calls, resolve to its arguments, its own captures and the globals. A local, an `eval` window or a query column of the caller is never one of them, so a request cannot rebind a name the body reads, and an in-place `alter` or `insert` by name in the body targets the global.
- In a query, a trusted lambda is always called, never inlined by the planner. Called by restricted code, it runs outside the query: it gets the arguments the query evaluated for it and does not read source columns by free name, as an ordinary helper does.
- The lift covers the whole call: the body and everything it calls run unrestricted. A trusted lambda must treat its arguments as data, since a function it is passed and calls, or an expression it evaluates, runs unrestricted too. The same holds for a trusted `.ipc.on.*` hook: trusting an `.ipc.on.sync` that evaluates the message lifts the restriction from every request.

Errors: `type` (an argument that is neither a lambda nor symbols, or a name bound to something other than a lambda), `name` (an unbound name), `domain` (a dotted name, the lambda a name holds is already trusted under another name, or a lambda cannot be serialized and so has no identity), `access` (called from restricted code).

## `.sys.timeit` { #sys-timeit }

Signature: `(.sys.timeit [flag])`. Toggles the per-expression profiler. Calling with no argument flips the current state; passing `0` disables, anything non-zero enables. Returns the new state as `i64` (0/1). More than one argument returns an `arity` error.

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

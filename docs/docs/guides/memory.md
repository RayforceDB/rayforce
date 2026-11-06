# Memory & Monitoring

## 1. Memory Architecture Overview

Rayforce uses a custom memory subsystem — no calls to `malloc` or `free` ever reach the C library. Every allocation flows through one of these layers:

- **Buddy allocator with thread-local heaps** — Each VM thread gets its own heap (identified by a `heap_id`). Allocations are fast, lock-free within a thread. Cross-heap frees are deferred to a lock-free queue and reclaimed lazily.
- **Slab cache** — Small allocations (common for atoms and short vectors) are served from pre-sized slab pools, avoiding buddy-tree overhead.
- **COW ref counting** — Vectors use copy-on-write semantics via `ray_retain`/`ray_release`. Shared vectors are only copied when mutated. Note that `ray_retain`/`ray_release`/`ray_cow` are no-ops on `RAY_ERROR` objects, so an error block must be reclaimed with `ray_error_free()` rather than `ray_release()`.
- **Arena allocator** — For bulk short-lived blocks (e.g., intermediate query results). Arena objects carry an `RAY_ATTR_ARENA` flag that makes retain/release no-ops. The entire arena is freed at once when work completes.
- **Out-of-core spill** — There is no enforced memory ceiling. The heap tracks how much anonymous (RAM) memory it has committed; once an allocation would push that past the **anon watermark** (by default total physical RAM, or the container's cgroup memory limit when that is smaller — `docker run --memory`, systemd `MemoryMax=` — less a quarter of it, at least 3 GiB and at most half, left to the page cache; `-m SIZE` overrides it), it is backed by a preallocated disk file instead of anonymous RAM. File-backed pages are always reclaimable to disk, so they can't trigger the OOM killer — the working set spills and the query completes (slowly) rather than being killed. This never rejects work. Total physical RAM is detected at startup for this threshold and for reporting (see `.sys.info` → `total-mem`).

For a deep dive into the allocator internals, see [Memory Model](../architecture/memory.md).

## 2. The `.sys.mem` Function

Call `(.sys.mem)` to get a snapshot of the current heap's allocation statistics. It returns a dictionary with the following fields:

| Field | Type | Description |
|---|---|---|
| `alloc-count` | i64 | Total number of allocations performed since init |
| `bytes-allocated` | i64 | Live bytes in buddy-pool blocks (sub-32 MB objects) |
| `direct-bytes` | i64 | Live bytes in direct mmaps (objects ≥ 32 MB, mapped at exact size) |
| `peak-bytes` | i64 | High-water mark of bytes-allocated |
| `slab-hits` | i64 | Number of allocations served from the slab cache |
| `sys-current` | i64 | Committed RAM: every anonymous mapping (buddy pools, sys allocations, swap-fallback pool) |
| `sys-mapped` | i64 | File-backed bytes currently mapped (splayed columns, symbol file, parse buffers) |
| `sys-mapped-peak` | i64 | High-water mark of `sys-mapped` |
| `anon-committed` | i64 | Anonymous RAM the heap has committed (pools, direct and sys blocks; spill excluded) |
| `anon-peak` | i64 | High-water mark of `anon-committed` |
| `anon-watermark` | i64 | Spill threshold: set by `-m SIZE`, see [section 8](#8-total-ram-and-out-of-core-spill) |

It also returns diagnostic `*-perpart-runs` counters and the symbol-table size;
the [`.sys.mem` reference](../namespaces/sys.md#sys-mem) documents every key.

### Basic Usage

```lisp
;; Check current memory stats
(.sys.mem)
```

```text
alloc-count     | 14523
bytes-allocated | 2621440
peak-bytes      | 5242880
slab-hits       | 9870
sys-current     | 8388608
```

### Checking Memory Before and After an Operation

```lisp
;; Snapshot before
(set before (.sys.mem))

;; Load a CSV
(write "/tmp/rayforce-memory-trades.csv"
  "sym,price,qty\nAAPL,150.0,100\nGOOG,280.0,50\n")
(set trades (.csv.read "/tmp/rayforce-memory-trades.csv"))

;; Snapshot after
(set after (.sys.mem))

;; See how much memory the load consumed
(- (get after 'bytes-allocated) (get before 'bytes-allocated))
```

```text
838860800
```

In this example, loading 10 million rows consumed roughly 800 MB of heap memory.

## 3. The `.sys.gc` Function

Call `(.sys.gc)` to run allocator maintenance after references have been
dropped. It drains cross-thread frees, flushes slab caches, coalesces free
blocks, reclaims empty oversized pools, and incrementally releases aged free
pages. Values themselves use deterministic reference counting; this is not a
tracing collector.

```lisp
(.sys.gc)  ; 0
```

!!! note "Note"

    Because Rayforce uses deterministic reference counting, objects become
    reclaimable immediately when their last reference is released. `(.sys.gc)`
    performs the allocator-side consolidation and page-return pass.

### Giving a peak back: `(.sys.gc 1)` { #giving-a-peak-back }

A free block keeps its pages, so the next query of the same shape reuses
them without faulting them in again. Short of memory pressure, the process
hands most of them back to the operating system only once it has been idle
for ten seconds (the idle decay). Every fired timer and every served
request counts as activity, so a server that polls on a timer, or that a
client polls, is never idle for that long. It keeps its largest working set
for the rest of its life, even when that memory is free.

`(.sys.gc 1)` runs the idle decay's sweep now, without waiting for the
process to go quiet or for free blocks to age, and also unmaps the cached
large blocks, which the decay keeps. It drains cross-thread frees and slab
caches, unmaps empty spill pools, and releases the pages of the free blocks
the sweep reaches. Like `(.sys.gc)` it returns `0`; what went back shows in
the process's resident memory (`VmRSS` in `/proc/<pid>/status` on Linux).
`(.sys.gc 0)` is the same as `(.sys.gc)`.

It is a best-effort maintenance request, not a promise that every free page
leaves the process:

- It works on the calling thread's heap and the idle worker threads' heaps.
  A heap of another live thread, such as an embedding's own thread, is left
  to that thread.
- A block keeps its first page, which holds its free-list links, and blocks
  under 8 KiB keep all their pages. Under transparent huge pages only the
  whole 2 MiB regions inside a block are released.
- The free pages of a [spill pool](#8-total-ram-and-out-of-core-spill) are
  the spill file's page cache, which releasing would not free. They go back
  only when the whole pool is empty and unmapped, and a heap keeps its last
  pool. An empty anonymous pool stays mapped, with its pages released.
- When the resident memory drops is up to the operating system. Linux drops
  released pages at once; macOS reclaims them lazily, when it needs the
  memory.

```lisp
(set v (til 100000000))  ; an 800 MB peak
(set v 0)                ; finished with it
(.sys.gc 1)              ; 0 — free pages released now, not after an idle spell
```

Call it where a program knows it has finished with a peak and will not
need that memory again soon: after a nightly batch, after closing a day.
The cost is paid on reuse: the next query that needs the memory faults the
released pages in again.

## 4. The `.sys.info` Function

Call `(.sys.info)` to see system-level information about the Rayforce runtime:

```lisp
(.sys.info)
```

```text
cores     | 16
page-size | 4096
total-mem | 16777216000
```

| Field | Description |
|---|---|
| `cores` | Number of logical CPU cores available |
| `page-size` | OS page size in bytes |
| `total-mem` | Total physical RAM in bytes |

## 5. Progress Monitoring

Long-running queries display a progress bar automatically in the REPL. The bar appears after approximately 2 seconds of execution and shows real-time feedback.

### Progress Bar Format

```text
[████████░░░░░░░░]  52% · group: hash · 3.2s · 1.2G/12.8G
```

The bar displays:

- **Percentage** — Estimated completion based on rows processed
- **Operation name** — The current phase (e.g., `group: hash`, `sort: merge`, `join: probe`)
- **Elapsed time** — Wall-clock time since query start
- **Memory** — Live object footprint (`used / total`) against total physical RAM, so you can watch a large query climb toward the point where it no longer fits

### Example Session

```lisp
;; A long-running version of this query shows the progress bar automatically
(set trades (table [sym price]
  (list [AAPL GOOG AAPL GOOG]
        [150.0 280.0 151.0 282.0])))
(select {from: trades
         by: {sym: sym}
         total: (sum price)
                n: (count price)
                hi: (max price)})
```

```text
[████████████████]  100% · group: merge · 4.1s · 3.8G/12.8G
┌──────┬───────────────┬──────────┬──────────┐
│  sym │     total     │    n     │    hi    │
│  SYM │      F64      │   I64    │   F64    │
├──────┼───────────────┼──────────┼──────────┤
│ AAPL │ 8825431692.50 │ 50120832 │   502.39 │
│ GOOG │ 6129847201.75 │ 49879168 │   501.97 │
├──────┴───────────────┴──────────┴──────────┤
│ 2 rows                        4 columns    │
└────────────────────────────────────────────┘
```

The progress bar is cleared automatically when the query completes and the result is displayed. In non-interactive mode (file execution), progress output is suppressed.

## 6. The `timeit` Function

Wrap any expression in `(timeit ...)` to measure its execution time. It evaluates the expression, discards the result, and **returns the elapsed time in milliseconds** as an f64 value. A deferred (lazy) result, such as a bare `(sum v)`, is forced inside the timed span, so the figure covers the computation, not the construction of its graph.

```lisp
;; Time a simple vector operation
(timeit (sum (til 10000000)))
```

```text
12.4
```

```lisp
;; Time a grouped aggregation
(set trades (table [sym price]
  (list [AAPL GOOG AAPL GOOG]
        [150.0 280.0 151.0 282.0])))
(timeit (select {from: trades
                 by: {sym: sym}
                 n: (count price)}))
```

```text
291.0
```

!!! note "Note"

    `timeit` returns only the elapsed milliseconds — it does not return the expression's result. To see the result and the timing, evaluate the expression separately and use `timeit` for benchmarking.

## 7. Profiling with `:t`

The REPL command `:t` (or `:timeit`) toggles profiling mode. When active, every expression displays a detailed timing span tree showing where time was spent.

### Example Session

```lisp
;; Toggle profiling on
:t
```

```text
. Timeit is on.
```

```lisp
;; Now every expression shows timing spans
(sum (til 1000000))
```

```text
499999500000
── top-level ─────────── 1.8ms
```

```lisp
;; Toggle profiling off
:t
```

```text
. Timeit is off.
```

This is the fastest way to identify slow expressions in an interactive session. Combine it with `.sys.mem` snapshots to see both time and memory costs.

## 8. Total RAM and Out-of-Core Spill

Rayforce imposes **no enforced memory ceiling** — it never rejects or throttles
work. Instead it is out-of-core: memory that does not fit in RAM spills to disk.

**Why not just rely on the OS.** An anonymous (RAM) mapping is dangerous: under
Linux's default overcommit the kernel *accepts* a mapping larger than it can
actually back, then invokes the OOM killer when the pages fault in. You cannot
tell at allocation time whether that will happen. A **file-backed** mapping over
a preallocated disk file is safe: its pages are always reclaimable to the file,
so it can never trigger the OOM killer — worst case is slower I/O or a clean
disk-full error.

**The anon watermark.** So the heap tracks the anonymous (RAM-resident) bytes it
has committed, and when a new pool or large allocation would push that past the
watermark — by default the smaller of total physical RAM and the container's
cgroup memory limit (cgroup v2 `memory.max` / v1 `memory.limit_in_bytes`,
including limits set on a parent slice), less a quarter of it (at least 3 GiB,
at most half) left to the page cache, or the `-m SIZE` flag when given
(reported as [`anon-watermark`](../namespaces/sys.md#sys-mem)) — it
backs that allocation with a disk spill file **instead of** anonymous RAM. This never rejects work; it just routes
the overflow to disk so it spills rather than getting OOM-killed. A query like
`(til 10000000000)` (a 74 GiB vector) on a smaller machine now spills to disk and
completes (slowly) instead of being terminated. The progress bar's `used / total`
figure shows the footprint approaching total RAM; spill begins below it, at the
watermark.

### Checking total RAM

```lisp
;; The total-mem field shows total physical RAM
(.sys.info)
```

```text
cores     | 16
page-size | 4096
total-mem | 16777216000
```

### Gauging headroom

To see how close a workload is to spilling to disk, compare the anonymous
footprint the heap has committed (`anon-committed` from
[`(.sys.mem)`](../namespaces/sys.md#sys-mem): pools,
large blocks and the off-heap tables such as the symbol arenas) against the
spill threshold (`anon-watermark`):

```lisp
;; Committed RAM as a percentage of the spill threshold
(set stats (.sys.mem))
(* 100.0 (/ (get stats 'anon-committed) (get stats 'anon-watermark)))
```

```text
29.4
```

This shows the workload uses 29.4% of the RAM it may take before new memory
starts going to spill files.

**No swap.** On Linux the heap locks the anonymous memory it maps under the
watermark — pools, large blocks, large off-heap tables — against swap-out
(`mlock2` with `MLOCK_ONFAULT`: a page is locked when it is first touched,
nothing is faulted in ahead), so the kernel never moves it to swap to make room
for page cache, and a query never waits for a swap-in. Memory past the
watermark and spill files are never locked, and the locked bytes stay within
the smaller of the watermark and half the RAM limit (the watermark alone is
half the limit on a small machine but up to three quarters on a large one, and
`-m` may set it past RAM). An empty pool kept for reuse is unlocked while it is
parked, and `(.sys.gc)` and the idle decay unlock the blocks the heap keeps
cached.

`RLIMIT_MEMLOCK` bounds it too: the `rayforce` binary raises the soft limit to
the hard one at startup (an embedding keeps its host's limits, and children of
`.sys.exec` inherit the raised soft limit). A process with `CAP_IPC_LOCK` — root
outside a container — is past any limit; a user often has 8 or 64 MB, and a
container as little as 64 KB, root inside it too. What does not fit is left
unlocked, the heap works on as before, and `(.sys.mem)` counts it
(`lock-skipped`, `lock-failed`); `locked-bytes` shows the mapped bytes locked.
`RAY_HEAP_MLOCK=0` turns locking off (read once, at the first lock). The
bound follows the RAM limit read at startup: a cgroup limit lowered later is
not seen, and what was locked stays locked until it is freed. Each process
bounds only its own
locking: on a machine shared by several heavy rayforce processes, give each a
`-m` that adds up, or turn locking off.

A spill file is mapped shared.  An embedding process that `fork()`s and keeps
running rayforce code in the child shares the spilled blocks with the parent
instead of getting its own copy, so the child must not write them; run the
child through `exec` (as `.sys.exec` does) or start it before anything spills.

## 9. Practical Patterns

### Pattern 1: Monitor Memory During CSV Loading

```lisp
;; Check baseline
(set m0 (.sys.mem))

;; Load data
(write "/tmp/rayforce-memory-trades.csv"
  "sym,price,qty,date\nAAPL,150.0,100,2024.01.15\nGOOG,280.0,50,2024.01.15\n")
(set trades (.csv.read "/tmp/rayforce-memory-trades.csv"))

;; Check cost
(set m1 (.sys.mem))
(println (format "Loaded: % bytes, peak: % bytes"
  (- (get m1 'bytes-allocated) (get m0 'bytes-allocated))
  (get m1 'peak-bytes)))
```

```text
Loaded: 4194304000 bytes, peak: 4831838208 bytes
```

### Pattern 2: GC Between Independent Queries

```lisp
;; First analysis pass
(set result1 (select {from: trades
                       by: {date: date}
                       vol: (sum qty)}))
(.csv.write result1 "daily-volume.csv")
(set result1 0)

;; Free intermediates before the next heavy query
(.sys.gc)

;; Second analysis pass with maximum headroom
(set result2 (select {from: trades
                       by: {sym: sym}
                       vwap: (/ (sum (* price qty)) (sum qty))}))
```

### Pattern 3: Profile a Slow Query

```text
;; Enable REPL profiling
:timeit

;; Break the query into parts to find the bottleneck
(set filtered (select {from: trades where: (> price 100.0)}))
```

```text
elapsed: 42.1ms
```

```text
(set grouped (select {from: filtered
                        by: {sym: sym}
                        avg_price: (avg price)
                               n: (count price)}))
```

```text
elapsed: 203.7ms
```

```text
(set sorted (select {from: grouped by: {n: (desc n)}}))
```

```text
elapsed: 0.8ms
```

```text
;; The group-by at 204ms is the bottleneck, not the filter (42ms) or sort (<1ms)
:timeit
```

### Pattern 4: Check Peak Memory After a Join

```lisp
;; Reset peak tracking with .sys.gc
(.sys.gc)
(set before-peak (get (.sys.mem) 'peak-bytes))

;; Run a memory-intensive join
(set quotes (table [sym time bid]
  (list [AAPL GOOG]
        [10 20]
        [149.5 279.5])))
(set joined (left-join [sym] trades quotes))

;; Check the peak
(set after-peak (get (.sys.mem) 'peak-bytes))
(println (format "Join peak overhead: % bytes"
  (- after-peak before-peak)))
```

```text
Join peak overhead: 1073741824 bytes
```

This tells you the join needed about 1 GB of temporary memory beyond what was already allocated.

## Summary

| Tool | What It Does | When to Use |
|---|---|---|
| `(.sys.mem)` | Returns heap allocation statistics | Monitor memory usage, detect leaks |
| `(.sys.gc)` | Flushes caches, releases pages | Between heavy queries, before benchmarks |
| `(.sys.gc 1)` | Also releases free pages now, best effort | A server finished with a peak |
| `(.sys.info)` | Shows system and runtime info | Check total RAM, CPU count, OS details |
| `(timeit expr)` | Measures execution time of one expression | Benchmark a specific operation |
| `:timeit` | Toggles profiling for all REPL expressions | Interactive performance exploration |
| Progress bar | Automatic during long queries | Visual feedback on query progress |

## Next Steps

- [**Memory Model**](../architecture/memory.md) — Deep dive into the buddy allocator, arenas, COW, and per-VM heaps
- [**Block Offloading**](../architecture/offloading.md) — How Rayforce handles datasets larger than RAM
- [**Pipeline & Optimizer**](../architecture/pipeline.md) — How queries are planned, optimized, and executed
- [**REPL Reference**](../language/repl.md) — All REPL commands and configuration

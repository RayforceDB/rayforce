# Block Offloading

## Overview

When a partitioned table is too large to fit in memory, Rayforce can process it one partition at a time. Instead of loading all partitions and concatenating them into a flat table, the executor **streams** through partition segments — loading each segment, running the full query pipeline on it, merging the partial result, and releasing the segment before moving to the next.

This happens transparently inside `ray_execute`. The same DAG, the same optimizer passes, and the same morsel-driven inner loops are used. The only difference is how data flows into the executor: one segment at a time rather than all at once.

!!! note "Zero overhead for in-memory tables"
    Block offloading only activates for tables opened with `ray_read_parted`. Regular (non-parted) tables take the existing direct-execution path with no additional checks in the hot loop.

## How It Works

The streaming execution path has four stages:

### 1. Detection

When `ray_execute` is called, it inspects the bound table. If the table contains parted columns (created by `ray_read_parted`), and the DAG only contains **streamable operations**, the streaming path is activated.

Streamable operations are those whose results can be correctly concatenated across segments:

- **Element-wise ops** — arithmetic, comparisons, logical, cast, isnull
- **String ops** — upper, lower, trim, substr, replace, concat, like
- **Temporal ops** — extract, date_trunc
- **Structure ops** — filter, select (projection), alias

DAGs containing joins, group-by, sort, window functions, or graph operations fall back to the flat-materialization path, which loads all segments into memory first.

### 2. Segment Table Construction

For each active partition segment, Rayforce builds a flat temporary table containing just that segment's columns. Parted columns resolve to the segment's mmap'd vector. The `MAPCOMMON` (partition key) column is materialized by broadcasting the partition key value across all rows in that segment.

The executor's `g->table` pointer is temporarily swapped to this segment table. All downstream operators — scans, filters, expressions — see regular flat columns and work exactly as they do for in-memory tables.

### 3. Execution and Merge

The full DAG is executed on each segment table using the standard morsel-driven pipeline. Each segment produces a partial result (a filtered/projected table). Partial results are merged by concatenating columns across segments.

Between segments, the executor:

- Releases the previous segment table (the mmap'd pages return to the OS)
- Checks the cancellation flag — queries can be cancelled between segments
- Proceeds to the next active (non-pruned) segment

### 4. Result Assembly

After all segments are processed, the merged accumulator is the final result. If all segments were pruned (no partitions match the filter), Rayforce executes the DAG on an empty table to produce a result with the correct schema and zero rows.

## Partition Pruning

The optimizer's partition pruning pass can skip entire partitions that cannot contain matching rows. When a filter predicate compares the partition column against a constant, the optimizer evaluates that comparison against each partition key and produces a **segment mask** — a bitmap where each bit indicates whether a partition is active.

Supported predicates:

| Operator | Example | Effect |
|---|---|---|
| `==` | `date == 2024.06.15` | Only one partition loaded |
| `!=` | `date != 2024.01.01` | Skip one partition |
| `>`, `>=` | `date >= 2024.06.01` | Skip earlier partitions |
| `<`, `<=` | `date < 2024.03.01` | Skip later partitions |

Multiple filter predicates on the partition column are combined with AND — each narrows the set of active partitions further.

!!! note "Pruning applies to all partition types"
    Date-partitioned (`YYYY.MM.DD`), integer-partitioned, and symbol-partitioned tables all support pruning. Symbol partition keys in `MAPCOMMON` are stored as 64-bit intern IDs (the key_values vector is always `RAY_SYM_W64`), and SYM atom constants also carry the intern ID, so equality comparisons work correctly.

## Memory Budget

Rayforce auto-detects available physical RAM at startup and sets a memory budget to 80% of it. This budget provides a baseline for future memory-aware scheduling.

```c
/* Query the memory budget (bytes) */
int64_t budget = ray_mem_budget();

/* Check if calling thread's heap usage exceeds the budget */
bool pressure = ray_mem_pressure();
```

!!! note "Thread-local stats only"
    `ray_mem_pressure()` reads the calling thread's heap statistics (`ray_tl_stats`), not the sum of all threads. In a parallel query, worker thread allocations are not reflected. This will be improved in a future release with a global memory tracking mechanism. Currently, `ray_mem_pressure()` is not used in the segment streaming hot path — it exists as infrastructure for future memory-driven scheduling.

Detection is platform-aware:

- **Linux / macOS** — `sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)`
- **Windows** — `GlobalMemoryStatusEx`

If detection fails (e.g., in some container environments), a conservative 4 GB fallback is used.

### Two Layers for Working Past RAM

Block offloading is the **query-side** mechanism for processing parted-table data that doesn't fit in memory: each segment streams through the executor and the next segment evicts the previous one's mmap'd pages. It's the right answer when the data already lives on disk in partition shape.

The complementary **allocator-side** mechanism is the [file-backed pool fallback](memory.md#buddy-allocator): when an anonymous mmap refuses a fresh pool (RAM + swap can't cover the request), the buddy allocator transparently spills the pool to a tempfile in the configured swap directory. That covers the case of fresh in-memory allocations — e.g. `(til 10000000000)` — that have no on-disk parted source to stream from.

## End-to-End Example

Consider a partitioned table of trade data with daily partitions spanning a year (365 partitions, each with millions of rows). A query that filters by date and aggregates by symbol:

```lisp
; Small verified partitioned fixture
(set dbroot "/tmp/rayforce-offload-db")
(set symfile (format "%/.sym" dbroot))
(set jun01 (table [sym price] (list [AAPL GOOG] [150.5 2800.0])))
(set jun02 (table [sym price] (list [MSFT] [410.0])))
(.db.splayed.set (format "%/2024.06.01/trades" dbroot) jun01 symfile)
(.db.splayed.set (format "%/2024.06.02/trades" dbroot) jun02 symfile)

; Load the partitioned table (zero-copy, segments stay on disk)
(set trades (.db.parted.get dbroot 'trades))

; Filter by date and select columns
(select {from: trades where: (>= date 2024.06.01) sym: sym price: price})
```

What happens internally:

1. **Load** — `ray_read_parted` creates a table with mmap'd parted columns. No data is read from disk yet.
2. **Optimize** — Partition pruning evaluates `date >= 2024.06.01` against the 365 partition keys. Partitions before June are skipped (roughly 150 partitions pruned).
3. **Stream** — For each of the ~215 remaining partitions:
    - The segment's columns are mmap'd (pages fault in on access)
    - The filter + projection runs through the morsel pipeline
    - The partial result is merged into the accumulator
    - The segment is unmapped (pages return to OS)
4. **Result** — The final table contains all matching rows from the active partitions. At no point was more than one partition's data resident in memory.

## Streamable Operations Reference

The following operations are safe for segment streaming. All other operations trigger fallback to flat materialization.

| Category | Operations |
|---|---|
| Data access | `OP_SCAN` |
| Unary element-wise | `NEG`, `ABS`, `NOT`, `SQRT`, `LOG`, `EXP`, `CEIL`, `FLOOR`, `ISNULL`, `CAST` |
| Binary element-wise | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `MIN2`, `MAX2` |
| Comparison | `EQ`, `NE`, `LT`, `LE`, `GT`, `GE` |
| Logical | `AND`, `OR`, `IF` |
| String | `LIKE`, `ILIKE`, `UPPER`, `LOWER`, `STRLEN`, `SUBSTR`, `REPLACE`, `TRIM`, `CONCAT` |
| Temporal | `EXTRACT`, `DATE_TRUNC` |
| Structure | `FILTER`, `SELECT`, `ALIAS`, `MATERIALIZE` |

When adding new element-wise operations to Rayforce, they must be added to the `op_streamable()` whitelist in `exec.c` to participate in segment streaming.

## Current Limitations

- **No streaming for aggregations, sorts, or joins** — these operations require seeing all data at once (or specialized merge logic). They fall back to loading all partitions into memory. This will be addressed in a future release with operator-aware merge functions.
- **Pruning requires literal constants** — the partition pruning pass only handles comparisons against literal values (e.g., `date > 2024.01.01`), not computed expressions (e.g., `date > (today - 30)`).
- **Single partition column** — pruning works on the single partition key inferred from directory names. Multi-column partition keys are not supported.
- **Thread-local memory pressure** — `ray_mem_pressure()` only reads the calling thread's heap statistics, not the global total across all worker threads. It is not currently used in the streaming hot path.
- **Container memory detection** — on Linux the anon watermark (spill threshold) is capped by the cgroup memory limit (`memory.max` / `memory.limit_in_bytes`, nearest ancestor included), but `.sys.info` → `total-mem` still reports host physical RAM.

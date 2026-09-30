# Data Persistence Guide

Rayforce provides multiple storage layers, from simple CSV for interchange to high-performance columnar files for production workloads. CSV I/O, splayed tables (`.db.splayed.set`/`.db.splayed.get`), and partitioned tables (`.db.parted.get`/`.db.parted.tables`/`.db.parted.fill`) are all available directly from Rayfall. Only raw single-column persistence (`ray_col_*`) is C-API-only.

## 1. CSV I/O { #csv-io }

The simplest way to persist and exchange data. Available directly from Rayfall.

### Reading CSV

```lisp
(write "/tmp/rayforce-data.csv" "sym,price,qty\nAAPL,150.5,100\nGOOG,2800.0,50\n")
(set data (.csv.read "/tmp/rayforce-data.csv"))
```

The reader:

- Treats the first row as column headers
- Infers column types automatically: `i64`, `f64`, `date`, `time`, `timestamp`, `sym`
- Uses parallel parsing for large files
- Handles null values (empty cells)

### Writing CSV

```lisp
(set data (table [sym price qty]
  (list [AAPL GOOG] [150.5 2800.0] [100 50])))
(.csv.write data "/tmp/rayforce-output.csv")
```

### Example: Round-Trip

```lisp
(set data (table [Name Score Grade]
  (list [Alice Bob Charlie]
        [95 87 92]
        [A B A])))

(.csv.write data "/tmp/grades.csv")
(.csv.read "/tmp/grades.csv")
```

```text
┌─────────┬───────┬───────────────────┐
│  Name   │ Score │       Grade       │
│   SYM   │  I64  │        SYM        │
├─────────┼───────┼───────────────────┤
│ Alice   │ 95    │ A                 │
│ Bob     │ 87    │ B                 │
│ Charlie │ 92    │ A                 │
├─────────┴───────┴───────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

**Note:** Float values without a fractional part (e.g., 150.0) will be read back as integers. The CSV reader always infers the narrowest matching type.

## 2. Symbol Table Persistence { #symbol-table }

Rayforce maintains a global symbol intern table for `sym` columns. When saving and loading columnar data, the symbol table must be persisted alongside it so that symbol IDs remain meaningful.

The C API provides:

```c
// Save the global symbol table to a file
ray_err_t ray_sym_save(const char* path);

// Load symbols from a file (merges with the current table)
ray_err_t ray_sym_load(const char* path);
```

The symbol file uses an append-only format: new symbols are appended on save, and loading merges them into the running table. File locking (`flock` on POSIX, `LockFileEx` on Windows) ensures safe concurrent access.

In typical usage, you do not call these directly — the splayed and partitioned table functions handle symbol persistence automatically.

## 3. Columnar Files { #columnar-files }

!!! note "C API only"

    The single-column functions below (`ray_col_*`, in `src/store/col.h`) are C-API-only — compile with `-Isrc` and include the header. They are not exposed as Rayfall builtins. The higher-level splayed and partitioned tables (next sections) *are* available from Rayfall via `.db.splayed.*` / `.db.parted.*`.

A **column file** stores a single vector (column) in Rayforce's native binary format. This is the building block for all higher-level storage.

```c
// Save a vector to a column file
ray_err_t ray_col_save(ray_t* vec, const char* path);

// Load a vector from a column file
ray_t* ray_col_load(const char* path);

// Memory-map a column file (zero-copy, read-only)
ray_t* ray_col_mmap(const char* path);
```

The file format is compact: a header with type and length, followed by the raw element data. For string columns (`RAY_STR`), the pool data is written after the element array.

The difference between `ray_col_load` and `ray_col_mmap`:

| Function | Memory | Access | Use case |
|---|---|---|---|
| `ray_col_load` | Copies data into heap | Read/write | Data you will modify |
| `ray_col_mmap` | Maps file directly | Read-only | Large data, zero startup cost |

## 4. Splayed Tables { #splayed-tables }

A **splayed table** stores each column as a separate file inside a directory. This allows loading individual columns on demand instead of the entire table.

```c
// Save a table as a splayed directory
ray_err_t ray_splay_save(ray_t* tbl, const char* dir,
                         const char* sym_path);

// Load a splayed table from a directory
ray_t* ray_splay_load(const char* dir,
                      const char* sym_path);
```

The directory layout looks like:

```text
trades/
  .d            # schema (column name symbol IDs)
  Symbol        # sym column
  Price         # f64 column
  Qty           # i64 column
```

Pass a `sym_path` to automatically save/load the symbol table alongside the data. Pass `NULL` if you manage symbols separately.

From Rayfall, use `(.db.splayed.set "/tmp/weather" t)` to save and `(.db.splayed.get "/tmp/weather")` to load — no C code required.

## 5. Partitioned Tables { #partitioned-tables }

For very large datasets, Rayforce supports **date-partitioned** storage. Each partition is a splayed table inside a date-named subdirectory.

```c
// Load a partitioned table
ray_t* ray_read_parted(const char* db_root,
                       const char* table_name);
```

From Rayfall, load a partitioned table with `(.db.parted.get "db" 'trades)`; list a root's tables with `(.db.parted.tables "db")`.

Expected directory layout:

```text
db/
  .sym                 # shared symbol table (dotfile)
  2024.01.01/
    trades/
      Symbol
      Price
      Qty
  2024.01.02/
    trades/
      Symbol
      Price
      Qty
  ...
```

The loader:

1. Loads the shared `.sym` file from `db_root`
2. Discovers date directories (sorted numerically)
3. Loads each partition as a splayed table
4. Adds a virtual `Date` column from the directory name
5. Concatenates all partitions into a single table

This is the recommended layout for time-series data that grows continuously. New partitions can be added without rewriting existing data.

For datasets larger than available RAM, Rayforce can process partitioned tables one segment at a time using **block offloading** — streaming through partitions without loading them all at once. The optimizer's partition pruning pass can skip entire partitions that don't match filter predicates. See [Block Offloading](../architecture/offloading.md) for details.

## 6. Memory-Mapped I/O { #mmap }

Memory-mapped I/O (`mmap`) lets Rayforce access on-disk data without copying it into memory. The operating system pages data in on demand as it is accessed.

### Benefits

- **Zero startup cost** — opening a 10 GB file is instant; only accessed pages are loaded
- **Shared memory** — multiple processes can mmap the same file without duplicating data in RAM
- **OS-managed caching** — the kernel manages which pages stay in memory vs. on disk

### Trade-offs

- **Read-only** — mmap vectors cannot be modified (COW will copy if you try)
- **Random I/O** — if access patterns are truly random across a large file, performance can be worse than sequential reads

### Usage

Use `ray_col_mmap` for individual columns:

```c
ray_t* prices = ray_col_mmap("db/2024.01.01/trades/Price");
```

Splayed table loading can also use mmap internally. For most analytics workloads — scans, filters, aggregations — mmap provides the best combination of startup time and throughput.

## Choosing a Storage Layer { #choosing }

| Layer | Interface | Best for |
|---|---|---|
| CSV | Rayfall | Data interchange, small datasets, prototyping |
| Columnar | C API (`ray_col_*`) | Single-vector persistence, embedding in applications |
| Splayed tables | Rayfall (`.db.splayed.*`) + C API | Multi-column tables, column-selective loading |
| Partitioned tables | Rayfall (`.db.parted.*`) + C API | Large time-series, append-only growth, date-range queries |

## Next Steps

- [**Storage Reference**](../storage/index.md) — Detailed file format specifications and API reference
- [**Core C API**](../c-api/core.md) — Working with `ray_t` objects, vectors, and tables in C
- [**Memory Model**](../architecture/memory.md) — Buddy allocator, arenas, COW, and per-VM heaps
- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Hands-on introduction to Rayfall

## Parquet input

Rayforce's Parquet reader is implemented in C, including the Thrift Compact
Protocol parser and Snappy decoder. It adds no library dependencies. The same
batch decoder serves materialized reads, streaming callbacks, and native
conversion.

```lisp
;; Footer inventory: one row per column chunk; no data pages decoded.
(.parquet.meta "hits.parquet")

;; Materialize only these columns, then use normal queries.
(set hits (.parquet.read [CounterID URL] "hits.parquet"))
(select {from: hits by: CounterID n: (count CounterID)})

;; A reusable lazy source: select infers the columns and eligible filters.
(set source (.parquet.scan "hits.parquet"))
(select {from: source n: (count WatchID)})
(select {from: source URL: URL where: (== CounterID 62)})

;; Inclusive signed-integer range: prune disjoint row groups, then filter
;; surviving rows exactly. The filter column need not be projected.
(.parquet.read
  {columns: [URL] range: {column: CounterID min: 62 max: 62}}
  "hits.parquet")

;; Process at most 65,536 rows per callback. Returns the number of rows
;; delivered; callback results are discarded and errors stop the scan.
(.parquet.each [CounterID URL] "hits.parquet"
  (fn [batch] (show (select {from: batch by: CounterID n: (count CounterID)}))))

;; Convert in batches into an existing parent's NEW native database root.
;; Returns the imported row count. Open the result with normal native I/O.
(.parquet.splayed "hits.parquet" "/tmp/hits-splayed")
(set flat (.db.splayed.get "/tmp/hits-splayed"))
(.parquet.parted "hits.parquet" "/tmp/hits-native" 'hits)
(set stored (.db.parted.get "/tmp/hits-native" 'hits))
```

Both `.parquet.read` and `.parquet.each` accept an optional column symbol
vector or an options dictionary before the path. Supported options are
`columns`, `types`, `strict`, and `range: {column: name min: lo max: hi}`. Bounds are I64 atoms;
range filtering currently supports signed integer columns only. Missing or
unusable row-group bounds fall back to decoding. A range is an exact filter,
not just a pruning hint. A scan with no matching rows may deliver an empty
schema-bearing batch.

`types` is a positional vector covering the complete file schema, including
unprojected columns. It uses the native import type rules described below.
For example, `{types: [UNIX_DATE UNIX_SECONDS SYM]}` reads two unannotated
integer temporal columns and dictionary-encodes a text column. Each read owns
a private symbol dictionary shared by its workers; returned vectors retain it
after the reader closes. This avoids the STR vector's 4 GiB pooled-byte limit,
but the dictionary and materialized rows still consume memory.

Lazy sources accept the same type overrides:

```clj
(set events (.parquet.scan {types: [UNIX_DATE UNIX_SECONDS SYM]} "events.parquet"))
(select {from: events by: s n: (count s)})
```

The lazy descriptor accepts `types` and `strict`; projections and predicates belong
in the query. Schema conversions are validated when a query opens the file.

`strict: true` rejects decoded nonnull values that would become native nulls
(including empty text, NaNs, and sentinel integers after type conversion), and
validates UTF-8 for annotated text. Actual Parquet nulls remain supported.
Widening an integer type can preserve a value otherwise reserved as null.
Strict mode validates decoded values; projections, footer-only counts, and
pruning do not certify unvisited payloads. The default retains native null
semantics. This option does not add separate null bitmaps or retain timezone
annotations.

`.parquet.read` retains the entire result. `.parquet.each` releases each batch
after the callback; a callback may retain it, but that increases memory use.
Grouping, ordering, distinct, and limits inside a callback apply to that batch.
Global answers inside callbacks require explicitly combining partial results.
Alternatively, use `.parquet.scan` as a `select` source: it reopens the file for
each query, infers projections for statically understood expressions, and
extracts signed-integer comparisons (`==`, `<`, `<=`, `>`, `>=`, including
conjunctions, reversed operands, and literal inclusive `within` bounds).
A standalone `like` predicate filters each decoded batch before materializing
the result. Filtered text enters a private symbol dictionary only after it
survives the reader predicate. Dynamic expressions fall back to carrying
all columns. Predicates with aggregates or other vector-dependent expressions
are evaluated by the ordinary query engine without early filtering.

Simple global `count`, integer `sum`/`avg`, and supported numeric/temporal
`min`/`max` expressions use the native streaming accumulators. An unfiltered
count uses footer row counts without decoding pages. Filtered streaming
aggregates currently require row-local signed-integer comparisons; one column
uses reader pruning and residual comparisons run on each batch. Other queries,
including grouping, distinct and ordering, materialize the inferred columns
and candidate rows before ordinary query execution, preserving global query
semantics. These queries are not bounded-memory scans. The lazy descriptor is
intended for `select`; materialize it explicitly with `.parquet.read` for other
table operations.

### Supported input and limits

- One local, unencrypted file with a flat schema.
- Uncompressed and raw Snappy column chunks.
- Data pages v1 and v2; PLAIN and dictionary encoding, including the legacy
  dictionary tag and dictionary-to-plain fallback within a chunk.
- RLE/bit-packed definition levels and dictionary IDs; page CRC32 validation
  when a checksum is present on a decoded page. Pruned pages are not read for
  checksum validation.
- BOOLEAN, INT32, INT64, FLOAT, DOUBLE, and BYTE_ARRAY physical types. Strings
  become STR vectors. Nullable booleans become I16 so nulls are representable.
- UTF8, DATE, signed integers, UINT8/16/32, and millisecond/microsecond/nanosecond
  TIMESTAMP annotations. Narrow signed annotations may use a wider native type
  to preserve values otherwise reserved as a null sentinel. UINT64, decimal,
  INT96, nested/repeated data, fixed byte arrays, and other annotations are
  rejected. BYTE_ARRAY bytes are preserved; annotated UTF8 is validated when
  `strict: true` is enabled.
- Parquet dates/timestamps are converted from the Unix epoch to Rayforce's
  2000 epoch. Timestamp timezone annotations are not retained as separate
  metadata. Unannotated integer columns remain integers.
- Existing Rayforce null semantics apply: empty strings, NaNs, and native
  integer null sentinels are represented as nulls. Their original distinction
  from Parquet nulls is not preserved. This is not a lossless interchange path
  for those values. Enable `strict: true` to reject these collisions instead.
- At most 4,096 columns, a 64 MiB footer, 64 MiB per compressed or decompressed
  page, and a 64 MiB string pool per decoded column batch. Oversized values
  return errors. Total scratch memory depends on the selected column count
  and page/dictionary sizes; there is no query-wide memory budget yet.

Projection avoids decoding other columns, except an unprojected range-filter
column. Signed integer ranges use row-group min/max and page column/offset
indexes. Excluded row intervals are applied across columns even when their
page boundaries differ: whole excluded pages bypass decompression; partially
covered pages are decoded to advance their cursors correctly. Integer equality
also probes standard split-block Bloom filters using a native, seed-zero XXH64
implementation specialized to INT32/INT64. Missing, unsupported or malformed
optional indexes fall back to scanning. See the Apache
[page-index](https://parquet.apache.org/docs/file-format/pageindex/) and
[Bloom-filter](https://parquet.apache.org/docs/file-format/bloomfilter/) specifications.

Materialized reads and lazy queries dispatch independent row groups through the
existing worker pool. Streaming aggregates merge native partial accumulators in
source order, including residual integer comparisons; unfiltered counts still
use only the footer. Materialized output also preserves source order. Callbacks
in `.parquet.each` run in order on the calling thread; their column decoding can
run in parallel for batches of at least 4,096 rows. `-c 1` uses one execution
participant; other core settings use the configured pool without a four-core cap.
Buffers and output vectors use Rayforce's block allocator. Required PLAIN signed
integers use SSE2 copy/null detection where available, with a scalar fallback.
Repeated dictionary runs fill native blocks in bulk. Snappy back references use
bounded block copies. Packed dictionary IDs remain scalar. A standalone `like`
predicate can filter decoded batches; string Bloom probes, fully deferred output
decoding, and streaming grouped queries remain future work.

Both `.parquet.splayed [types] path directory` and
`.parquet.parted [types] path root table-name` return the imported row count.
The optional positional type vector has one entry per source column. It accepts
matching native types, checked signed integer widths, and `SYM` for text. Raw
integer dates/timestamps require explicit `UNIX_DATE` (days), `UNIX_SECONDS`,
`UNIX_MILLIS`, `UNIX_MICROS`, or `UNIX_NANOS`; no epoch is guessed. Annotated
Parquet dates/timestamps retain their declared units with `DATE`/`TIMESTAMP`.
For example, a file containing an integer ID, unannotated Unix seconds and text:

```lisp
(.parquet.splayed [I64 UNIX_SECONDS SYM] "events.parquet" "/tmp/events")
(.parquet.parted [I64 UNIX_SECONDS SYM] "events.parquet" "/tmp/events-db" 'events)

;; CSV supports the same two destination layouts. An explicit name vector
;; means headerless input. Its DATE/TIMESTAMP fields are formatted text.
(.csv.splayed [id time text] [I64 TIMESTAMP SYM] "events.csv" "/tmp/events-csv")
(.csv.parted [id time text] [I64 TIMESTAMP SYM] "events.csv" 65536 "/tmp/events-csv-db" 'events)
```

For fixed-width and `SYM` splayed output, workers decode column chunks across
row groups directly into disjoint ranges of the final column files. Parquet
string dictionary IDs map directly to native file-domain symbol IDs. The file
mapping is shared, while decode buffers and cursors remain private to each task.
The loader builds the same native indexes as the CSV splayed loader. Files and
the staging directory are synced before publication, avoiding a directory sync
for every individual column rename.

Splayed `STR` output uses bounded temporary native batches and merges their
string pools in source order. This requires temporary disk space and an extra
write pass. A native STR pool has 32-bit offsets: use `SYM` or partitioned output
when a column's pooled bytes exceed 4 GiB. CSV splayed STR import now uses the
same streaming column writer, without materializing the entire CSV.

Partitioned conversion dispatches row groups independently and assigns numeric
partition IDs in source order, with at most 65,536 rows per partition by default.
An import options dictionary can replace the positional type vector:
`{types: [I64 SYM] rows: 262144 strict: true}`. `types` and `strict` are optional;
`rows` accepts 1 through 1,048,576. Row-group boundaries also end partitions.
Larger batches require more memory; decoded STR pools retain their 64 MiB batch
limit. Splayed imports accept the same options to control their decode batches.
The importer writes
exact native numeric zone indexes while each decoded batch is resident (F32
currently excluded). STR pools stay local to each partition. Typed SYM imports
use a shared file dictionary. Group decoding and independent partition writes
remain parallel; the dictionary is flushed once after workers finish. Column
files are then synced, followed by each leaf and partition directory, before
the staged root is published. This avoids syncing a directory after every
individual column rename while preserving durability at publication.

Parquet conversions write to `<destination>.parquet-partial` and publish the
result after successful writes. Existing destinations or staging directories
are rejected; failed staging data is retained for inspection. The destination's
parent must already exist. Column names must be safe native filenames; parted
imports also reject an input `part` column. These imports are not resumable.
Publication uses the host's exclusive rename primitive so a destination
created during the import cannot be overwritten. Linux uses
[`RENAME_NOREPLACE`](https://man7.org/linux/man-pages/man2/renameat2.2.html);
macOS uses [`RENAME_EXCL`](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsexclusiverenaming).
An unsupported host or filesystem returns an error and retains the staging
directory; it does not fall back to an overwrite-capable rename.

CSV parted imports into a new root use `<destination>.csv-partial` and publish
after the final shared dictionary flush. Failed staging directories are kept
and rejected on retry. Imports into an existing CSV root retain the existing
per-partition update behavior. CSV bulk writes require an external file and
directory sync before they can be considered durable.

### ClickBench development checks

The ClickBench Athena `hits.parquet` footer inspected on 2026-09-30 contains
99,997,497 rows, 105 columns, and 226 row groups. All 23,730 column chunks use
Snappy and have statistics; none have page indexes or Bloom filters.
`EventTime` and `EventDate` are unannotated/ordinary integers in that dataset:
benchmark queries must explicitly convert Unix seconds and Unix days to native
temporal types. Those benchmark-specific conversions do not belong in the
format reader.

The following developer tools use Python's standard library; remote range
fetches use the `curl` executable. DuckDB is an external test oracle only.

```bash
# Inventory the original footer and copy original pages for a bounded sample.
python3 scripts/parquet-inspect.py \
  https://datasets.clickhouse.com/hits_compatible/athena/hits.parquet \
  --sample /tmp/hits-sample.parquet --groups 1

# Compare every sample cell to DuckDB and every native batch to the source.
python3 scripts/parquet-verify.py /tmp/hits-sample.parquet

# Compare representative lazy query shapes with the external oracle.
python3 scripts/parquet-query-verify.py /tmp/hits-sample.parquet --cores 4

# Warm scan timings: all columns, projection, footer count, streaming aggregates.
make release
python3 scripts/parquet-bench.py /tmp/hits-sample.parquet --cores 1 4

# Regression suite; fixtures are checked in, with a standard-library generator.
make test TEST_FILTER=parquet
```

The first original row group (450,560 rows) was checked across all 105 columns:
47,308,800 cells matched DuckDB, and all seven converted native partitions
matched the decoded source. Twelve representative lazy query shapes also
matched the oracle, including grouping, distinct counts, top-k and empty
filtered aggregates.

The full 99,997,497-row input subsequently passed all **43 ClickBench query
shapes** against DuckDB on 2026-09-30, using explicit SYM text types, 28 workers,
and a recorded 48 GiB process address-space ceiling. The per-query scripts,
translations, timings, peak RSS and oracle outcomes are recorded in
`bench/parquet_load/results/2026-09-30-full-queries.json`. Tied LIMIT alternatives
are checked for rank, full-row membership and multiplicity. Reproduce with:

```bash
python3 scripts/parquet-clickbench.py /path/to/hits.parquet \
  --symbol-text --cores 28 --address-space-gib 48 --timeout 180 \
  --work-dir /path/to/new-query-results
```

This is a correctness census with one process per query, not an official
ClickBench score. General grouped/sorted queries still materialize candidate
rows; passing this dataset does not establish bounded memory for arbitrary data.

Both `parquet-clickbench.py` and `parquet-load-bench.py` accept `--resume` for
runs created with their checkpoint support. Repeat the original command with
that flag and the same work directory. A durable `run.json` records the build,
harness, settings and source identity; incompatible runs are rejected. The
query census fingerprints the input contents, while the load harness checks
file identity, size and modification/change timestamps for its much larger
CSV input and retained reference. Completed cases are retained; unfinished
outputs move into `interrupted-*` directories and their cases restart with new
timings. Remove a deliberate `stop` marker before continuing. Older runs
without `run.json` require a new directory. This resumes benchmark cases, not
an individual native import.

Earlier single-row-group scan measurements on an Intel Core i7-14700, release build with
`-march=native`, using that sample (median of five runs after one warmup):

| Operation | 1 core | 4 cores |
| --- | ---: | ---: |
| Scan all 105 columns | 424.0 ms | 236.6 ms |
| Scan nine projected columns | 105.3 ms | 81.6 ms |
| Footer count | 0.112 ms | 0.112 ms |
| Stream three aggregates over two columns | 8.20 ms | 8.50 ms |

The wide scan benefits from parallel decoding; the small aggregate projection
does not in this measurement. These are sample scan timings, not a published
43-query ClickBench score or a cold-storage benchmark.

### Comparing the ClickBench load stage

`scripts/parquet-load-bench.py` reads the actual type vector from
`../ClickBench/rayforce/create.rfl` and runs its `.csv.splayed` load against
`.parquet.splayed` with the same native schema. It maps the Athena file's raw
integer dates/timestamps using explicit Unix units and stores all text as SYM,
matching ClickBench. An optional external DuckDB export prepares a headerless
CSV containing the identical rows, with UTC timestamps. Export and download are
outside the measured load stage; no external library is linked to Rayforce.

```bash
make release
python3 scripts/parquet-load-bench.py /tmp/hits-sample.parquet /tmp/hits-sample.csv \
  --prepare-csv --duckdb duckdb --cores 1 8 28 --repeats 3 \
  --work-dir /tmp/rayforce-load-comparison
```

Use a sample with several **original row groups** (`--groups 8` when extracting)
to exercise group parallelism. The script uses fresh processes and destinations,
alternates CSV/Parquet order across repetitions, and includes index creation,
reopening, and output fsync in wall time. It compares every resulting value and
column type with the first CSV import before reporting a successful run.
Peak RSS, output sizes, individual times and medians are written to
`results.json`. Inputs use the OS page cache; this is not a cache-evicted storage
benchmark. The benchmark work directory must be new and retains the reference
CSV native table and logs.

For the full dataset, use the bounded-memory verifier. It compares every
numeric payload and resolves symbol IDs by their actual bytes, without
interning both vocabularies into the runtime. Verification is outside timing.

```bash
cc -O3 -Iinclude bench/parquet_load/verify-native.c -o /tmp/verify-native
curl --fail --location --output /tmp/hits.parquet \
  https://datasets.clickhouse.com/hits_compatible/athena/hits.parquet
python3 scripts/parquet-load-bench.py /tmp/hits.parquet /tmp/hits.csv \
  --prepare-csv --duckdb duckdb --cores 28 8 --repeats 3 \
  --native-verifier /tmp/verify-native --work-dir /tmp/rayforce-full-load
```

An existing verified native CSV table can be supplied with `--reference`.
`--kinds parquet` or `--kinds csv` measures only that input format against the
reference. Creating a `stop` file in the work directory asks the runner to stop
after saving the current verified result.

Before the full-dataset contention fixes, the first eight **original**
ClickBench row groups contained
3,266,382 rows × 105 columns. On an Intel Core i7-14700 (28 logical CPUs),
three repetitions per case produced these median durable load times:

| Execution participants | CSV → splayed | Parquet → splayed | CSV / Parquet |
| ---: | ---: | ---: | ---: |
| 1 | 8.652 s | 4.692 s | 1.84× |
| 8 | 2.627 s | 1.661 s | 1.58× |
| 28 | 2.310 s | 2.089 s | 1.11× |

All 342,970,110 cells and native column types matched the CSV reference for
every Parquet import. Both formats produced 2,195,585,704 bytes of indexed
native storage. Eight participants were faster than 28 for this sample;
the implementation imposes no fixed core cap. The input files were 594,562,209
bytes of Parquet and 2,722,297,967 bytes of CSV. This earlier load-stage
comparison used a 3.27M-row sample. Raw runs,
peak RSS, source checksum and environment are recorded in
`bench/parquet_load/results/2026-09-30.json` in the repository.

### Full ClickBench load proof

The complete original file was measured on 2026-09-30: **99,997,497 rows,
105 columns, 226 row groups**. The same Intel Core i7-14700 has 20 physical
cores and 28 logical CPUs, 62 GiB RAM and NVMe storage. These are medians of
three durable loads per format and execution-participant count, including
native index construction, reopening and output fsync:

| Execution participants | CSV → splayed | Parquet → splayed | CSV / Parquet |
| ---: | ---: | ---: | ---: |
| 8 | 124.113 s | 84.285 s | 1.47× |
| 28 | 112.247 s | 77.772 s | 1.44× |

Every measured output was checked across all **10,499,737,185 cells**, column
names/order and native types against the verified CSV reference. Both inputs
produce **59,042,332,197 bytes** of indexed native storage. The inputs are
14,779,976,446 bytes of original Parquet and 78,462,321,894 bytes of matching
CSV. Download and the external oracle's CSV export are outside load timing.

Profiling found contention on the shared symbol-domain spinlock and frequent
fallback from batched to individual symbol insertion when concurrent chunks
changed the dictionary. The domain now uses an OS mutex, rechecks distinct
misses while preserving bulk arena allocation, and the Parquet importer drops
its completed ingestion dictionary before reopening for index construction.
Row groups and columns continue to use the configured worker pool.

Before these changes, full-data Parquet medians were 99.688 seconds at eight
participants and 108.551 seconds at 28. Extra workers were making it slower.
The load still contains shared dictionary work, native index construction and
59 GB of output writes, so additional workers do not produce linear scaling.
Inputs use the OS page cache, with no cache eviction or CPU pinning. The host
has 16 GiB zram plus about 4 GiB disk swap; swapping was observed, mainly zram.
The results describe this host under these conditions.

Raw before/after runs, CPU time, page faults, I/O counters, peak RSS, input and
binary checksums, export SQL and validation details are in
`bench/parquet_load/results/2026-09-30-full.json` and
`bench/parquet_load/results/2026-09-30-full-baseline.json`.

### Full partitioned load proof

The subsequent partitioned comparison used the same 99,997,497 rows and 105
columns, a 262,144-row partition cap, and three repetitions per format at both
eight and 28 execution participants. All **12 outputs** passed comparison of
every cell and native column type against the retained full CSV splayed
reference. The medians include process startup, native indexes, reopening,
and output file/directory sync; verification runs after timing stops.

| Execution participants | CSV → partitioned | Parquet → partitioned | CSV / Parquet |
| ---: | ---: | ---: | ---: |
| 8 | 241.077 s | 118.807 s | 2.03× |
| 28 | 233.398 s | 117.488 s | 1.99× |

Both outputs preserve source row order. CSV produces 382 partitions; Parquet
produces 501 because source row-group boundaries also terminate partitions.
Their native sizes are 47,319,348,589 and 47,323,067,877 bytes, respectively.
Across these runs, CSV peak RSS was 47.1–49.9 GiB and Parquet peak RSS was
33.4–40.5 GiB. These are observed peaks, not configured memory bounds.

Increasing participants from eight to 28 improved the median by about 3.2%
for CSV and 1.1% for Parquet on this host. It does not establish linear CPU
scaling: the workload also includes shared dictionaries, memory pressure,
index construction, and approximately 47 GB of durable output per run.
The same OS-managed page cache and unpinned CPU setup applies. No tests or
diagnostic queries ran concurrently with these measurements.

The raw cases, verification outcomes, source/build identities and resource
counters are in `bench/parquet_load/results/2026-09-30-parted-full.json`.
The measured release is retained separately from later write-failure and
exclusive-publication hardening. The earlier splayed results above remain
measurements of their recorded build.

A separate, single-run 65,536-row-cap diagnostic measured 231.194 seconds
after grouping staging syncs, versus the earlier 815.970-second run. This
is not a repeated median; its report is
`bench/parquet_load/results/2026-09-30-parted-sync-diagnostic.json`.

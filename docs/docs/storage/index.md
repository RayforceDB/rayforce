# Storage

Columnar file I/O, splayed tables, date-partitioned storage, symbol table persistence, and CSV import/export — everything for getting data in and out of Rayforce.

## Columnar Files

Rayforce's native binary format stores a single vector per file. Each file contains the 32-byte `ray_t` header followed by raw element data. Null state is stored in the payload as type-correct sentinels; there is no separate null bitmap region. The format is designed for direct memory mapping — no deserialization needed.

The header carries a single-byte format generation. The current generation is `0`, and data written by any prior engine that produced the same layout (also generation `0`) loads unchanged — no migration required. A file whose generation the reader does not recognize is refused with a `version` error rather than mis-read; a future breaking layout change bumps the generation so old files are rejected for migration instead of silently misinterpreted.

### File Structure

```c
/*
 * Column file layout:
 *
 * Bytes  0-31:   ray_t header (type, attrs, len, etc.)
 * Bytes 32-N:    element data (len * elem_size bytes)
 *                nulls use type-correct sentinels in this payload
 *
 * String columns (RAY_STR) continue:
 * Bytes 32-N:    16-byte descriptors; a string longer than 12 bytes
 *                lives in the pool and its descriptor carries the
 *                pool offset and a 32-bit content hash
 * Bytes N+1..:   32-byte pool header, then the pool bytes
 */
```

A string column file written by the current engine stores the content hash of every pooled string in its descriptor and marks the header to say so. Loading or mapping such a file checks descriptor bounds only: no string bytes are read and no descriptor page is written, so the mapping stays shared with the page cache. Files written before the hash was stored carry padding in that field; the loaders detect the missing mark, recompute the hashes and repair them in the private mapping, without touching the file.

### C API

| Function | Description |
|---|---|
| `ray_col_save(vec, path)` | Write a vector to a column file. Handles slices, sentinel nulls, and string pools transparently. |
| `ray_col_load(path)` | Read a column file into a heap-allocated vector. The file is read entirely into memory. |
| `ray_col_mmap(path)` | Memory-map a column file. A generation-0 file is zero-copy: the returned vector points directly into the mapped file pages. Ideal for large datasets that exceed available RAM. A compressed (generation-2) file is mapped too, but the column decodes into heap memory the first time it is read. |

```c
// Save a vector
ray_t* prices = ray_vec_from_raw(RAY_F64, data, 1000000);
ray_err_t err = ray_col_save(prices, "db/trades/price");

// Load into memory
ray_t* loaded = ray_col_load("db/trades/price");

// Memory-map for zero-copy access
ray_t* mapped = ray_col_mmap("db/trades/price");
// mapped->data points into file pages — no allocation
```

!!! note "Memory-mapped vectors"
    Memory-mapped generation-0 vectors have `mmod = 1` in their header, distinguishing them from heap-allocated vectors; a decoded compressed column is heap memory. The buddy allocator skips them during free. Slices of mmap'd vectors retain a reference to the parent mapping.

## Splayed Tables

A splayed table stores each column as a separate file in a directory. This is the standard on-disk representation for a Rayforce table. The schema (column names) is stored in a `.d` file alongside the data files.

### Directory Layout

```text
db/trades/
  .d                 — I64 vector of column name symbol IDs
  .sym               — symbol table (domain for SYM columns; dotfile)
  sym                — SYM column (stock tickers)
  price              — F64 column (trade prices)
  qty                — I64 column (quantities)
  time               — TIMESTAMP column
```

The symbol table is a dotfile (`.sym`, lock `.sym.lk`), so it never collides
with a user column — a table may have an ordinary column named `sym` (the
canonical ticker column), as shown above.

### Atomic table replacement

When an existing splayed table is replaced, Rayforce writes the new schema and
all columns into a fresh directory below `.generations/`. After the files are
complete, it atomically replaces the small `.current` manifest. Readers follow
that manifest, so they see either the previous complete generation or the new
complete generation — never a mixture of columns from both. A failed write
before publication leaves the previous generation selected. A cancelled write
reports `cancel` and changes nothing: the previous generation stays selected,
and a cancelled first write leaves no table behind, while files that were
already in the directory before the write are kept. Superseded generations,
and what a failed first write created, are removed afterwards; a symbolic
link found among them is unlinked, never followed, so nothing outside the
table directory is touched. Tables written by older Rayforce versions remain
readable; the first replacement upgrades the directory to the generation
layout.

The table, partition and CSV entry points follow the same publication protocol.
Indexes are built in the new generation before it is published. The CSV and
Parquet converters build each column's chunk zone or string dictionary from
the rows as they are streamed and write it with the column, so a conversion
does not read its own output back. The converters add no hash index of their
own: a hash index is persisted only when the column carries one built with
`.idx.hash` when the table is saved (a table resaved from an older store keeps
a hash it already has), and a parted conversion builds none for its segments.
An index that cannot be written (for example, the disk fills) is dropped and the
column is published without it. Writers to the same table serialize on
`.write.lock`; readers resolve the manifest once and continue using that generation. The symbol vocabulary stays at its original
location and grows append-only.

Publication keeps only the selected generation and removes the others
best-effort, so a replaced table holds one copy of itself on disk. A reader that
resolved a generation removed before it opened every file resolves the manifest
again and reads the selected one; a table already loaded stays readable. A
failed unpublished generation is removed immediately where the platform allows
it; on Windows an open mapped file can defer that cleanup until a later publish.
At the first manifest publish, the legacy root `.d` is retired so older binaries
fail loudly, and the column files it named are removed from the table
directory; nothing else there is touched.

The `.write.lock` writer lock is local-filesystem coordination. Do not rely on
it for NFS-backed shared writers.

The first write of a table directory, one without `.d` or `.current`, writes in
place. When it fails, the writer removes what it produced there and, once the
directory holds nothing else, the directory itself and every directory it
created above it, so no schema-less partition is left behind. A directory that
existed before the write is handled the same way, whether an operator pre-made
it or an earlier first write was killed: files that were there before the lock
was taken and that the write did not rewrite stay, since a directory without a
schema is not always a dead table. A partition directory left empty this way
is still a partition; `.db.parted.fill` gives it the missing tables.

`ray_splay_save` (including `.db.splayed.set`) syncs primary data and directory
entries before acknowledging publication. Inline indexes are rebuildable
accelerators written with a marker-last protocol. The bulk C API and CSV import
retain their no-fsync contract: publication is atomic for readers, but is not a
power-loss durability guarantee. An I/O error after the manifest rename (during
the final directory sync) has an uncertain commit outcome; reopen the table to
inspect the selected generation.

This protects publication of one splayed table. Coordinating a consistent
snapshot across multiple tables or date partitions still belongs to the caller.

### C API

| Function | Description |
|---|---|
| `ray_splay_save(tbl, dir, sym_path)` | Save a table as a splayed directory. Each column becomes a file named after its column symbol. Pass `sym_path` to also save the symbol table. |
| `ray_splay_load(dir, sym_path)` | Load a splayed table from a directory. Columns are memory-mapped by default. Pass `sym_path` to load the associated symbol table. |

```c
// Save table to disk
ray_err_t err = ray_splay_save(table, "db/trades", "db/.sym");

// Load table (columns are mmap'd)
ray_t* trades = ray_splay_load("db/trades", "db/.sym");
```

!!! note "Rayfall builtins"
    Use `.db.splayed.set` and `.db.splayed.get` from Rayfall to save and load splayed tables without writing C code. See the [Rayfall Storage Builtins](#rayfall-storage) section below.

## Date-Partitioned Tables

For large time-series datasets, Rayforce supports date-partitioned storage. Data is split into directories named by date, each containing a splayed table for that day's data.

### Directory Layout

```text
db/
  .sym                  — shared symbol table (domain for all partitions)
  2024.01.15/
    trades/
      sym
      price
      qty
      time
  2024.01.16/
    trades/
      sym
      price
      qty
      time
  2024.01.17/
    ...
```

The shared symbol table lives in the parted root (`db/`) as the dotfile
`.sym`; each partition is a date-named directory holding one splayed table
per table name (`db/<date>/<table>/cols`). Partitions have no own symfile and
encode their SYM columns (e.g. `sym`) against the shared `.sym`.

### Writing a Partitioned Database from Rayfall

There is no `.db.parted.set`. The supported way to write a partitioned database is to save each partition's table with `.db.splayed.set` into `<root>/<partition>/<table>`, passing the root's one shared symbol file. `.db.parted.get` then loads the result.

```lisp
(set root "/data/db")
(set symfile (format "%/.sym" root))
(.db.splayed.set (format "%/0/ohlcv" root) t0 symfile)
(.db.splayed.set (format "%/1/ohlcv" root) t1 symfile)
(.db.parted.get root 'ohlcv)              ; adds the `part` column
```

What this guarantees:

- **Directories.** `.db.splayed.set` creates the partition directory and any missing parents; nothing needs to exist beforehand.
- **Partition names.** Any name `.db.parted.get` accepts is a partition: dates (`2024.01.15`), timestamps (`YYYY.MM.DD.HH.MM.SS`) or plain integers. Integer names need not start at 0, be consecutive or be zero-padded; they are ordered by value (`1`, `2`, `10`), and the `part` column is `I64`. See [`.db.parted.get`](../namespaces/db.md#db-parted-get) for the full rule and for how mixed names are handled.
- **Shared symbol file.** Pass the same `sym_path` (conventionally `<root>/.sym`) for every partition and every table of the root. `SYM` columns of all partitions then share one vocabulary, which is what the loader reads (see [Symbol Table Persistence](#symbol-table-persistence)). New symbols append to the file, so partitions can be written one at a time, in any order.
- **Pruning.** A predicate on the key (`(== part 7)`, `(> date 2024.01.15)`) prunes from the directory names alone; it needs no `compress` option. `compress: true` matters only for pruning on a *data* column, whose per-partition minimum and maximum are kept for compressed columns and for plain columns of 65,536 rows or more.
- **Tables in every partition.** Write each table into every partition. A table that appears in later partitions only, because it was added midway, is handled by `.db.parted.fill`, which gives every partition that lacks a table an empty copy of it. It does not change existing data and a repeat call is a no-op.

### Writing a Parted Table as One Splayed Table

`.db.splayed.set` takes a parted table as it takes any other, and writes it a partition segment at a time: the table is never held in memory. Use it to merge the partitions of a root into one larger partition, or to copy a parted table elsewhere.

```lisp
(set P (.db.parted.get "/data/db" 'trades))
(.db.splayed.set "/data/merged/2024.01/trades" (xcols P (drop (cols P) 1)) "/data/merged/.sym")
```

The written table is the one an in-memory copy of `P` would be, so it keeps the partition key (`date` here) as an ordinary column unless the key is dropped first, as above. `SYM` columns are re-encoded into the destination's symbol file, and a partition without a column contributes its rows as nulls (`false` / `0` for `BOOL` and `U8`, which have no null). The writer's memory is one chunk per column, a cache of symbol positions and the indexes the save builds; only a string column's dictionary index (up to about 12 bytes a row while it is built, `{index: false}` skips it) grows with the rows. See [`.db.splayed.set`](../namespaces/db.md#db-splayed-set) for the options and errors.

### Loading Partitioned Data

The `ray_read_parted()` function scans all partition directories, memory-maps every column file, and assembles them into a single logical table with [parted columns](../data-types/collections.md#partitioned-columns) and a virtual `MAPCOMMON` date column.

```c
// C API: load all partitions
ray_t* trades = ray_read_parted("db", "trades");

// The result is a single table with:
// - A MAPCOMMON 'date' column derived from directory names
// - Parted columns (RAY_PARTED_BASE + base_type) for each data column
// - All segments are memory-mapped — no data copy
```

!!! note "Rayfall builtin"
    Use `.db.parted.get` from Rayfall to load partitioned tables: `(.db.parted.get "db" 'trades)`. See the [Rayfall Storage Builtins](#rayfall-storage) section below.

Mounting the same root again on the same thread reuses unchanged partitions while the earlier view is held. Cache entries record the resolved generation directory and the device, inode, size, modification time and change time of the schema and every column file. A changed partition is loaded afresh; a disappeared partition is dropped. Symbol columns must match the root's current domain, so a missing `.sym` is still an error.

The cache holds at most 1024 partition tables per thread. GC, symbol-domain opens and the next mount discard entries whose columns have no external references; heap abandonment, heap teardown and runtime shutdown clear the thread's entries. To release a dropped view's mappings and copied LIST values immediately, call `(.sys.gc)`. C callers can also use `ray_parted_cache_clear()` from `store/part.h` to clear their thread's cache.

### Partitions without a column (ragged tables)

A partition may hold fewer columns than the first one, as long as the columns it has come first in the same order with the same names and types; a column it lacks is a NULL segment for that partition. Such a ragged table reads as the flat table with typed nulls in the missing rows, wherever its rows are read: a select over it (projections, `where:`, `asc:`/`desc:`, `take:`, `by:` and aggregates), `(at table 'col)`, the standalone `take` verb, printing and `show`, and `ser`, whose bytes equal those of the equivalent flat table. The partition-key column's row counts give the missing rows; `BOOL` and `U8`, which have no null, read as `false` and `0`. A parted column met away from its table (through `get` on a column value rather than a query) still contributes no rows for a partition that lacks it.

!!! warning "`update` over a partitioned table materializes in memory"
    `update` on a partitioned table flattens the whole table into memory first — the parted/`MAPCOMMON` columns cannot be mutated in place — so the result is an ordinary in-memory table. It is **not** written back to the store: re-reading the root returns the original values, and the returned table loses its parted / memory-mapped identity.

### Partition Pruning

The optimizer can derive a necessary partition mask from literal comparisons or literal `in` sets on the virtual `MAPCOMMON` partition column. It applies that mask only when the remaining query shape is safe to evaluate over the retained partitions. The original `where` still runs. This is not a guarantee that every query with a date bound prunes, nor a count of physical file opens or reads.

Ordinary row-local atomic expressions retain their uncapped admission walk. Canonical nested `and`/`or`, restricted `not` conditions, grouped unary `first`/`last`, and output `count(distinct column)` can retain pruning too. Call heads shadowed by lexical bindings or source columns are not assumed to be the corresponding builtin.

Two additional IF shapes have deliberately bounded proofs:

- **Ungrouped:** fresh outputs, beside any outputs the ordinary proof admits (a source column among them only as itself, `a: a`), with `from` and `where` and no other clause (no `by`, `take` or sorting), and an explicit ELSE. Both branches must have one type, I64, F64 or SYM: columns or literals of it, or nested `if` expressions with explicit ELSE. Conditions use BOOL literals, I64 or F64 comparisons of one type (a null, NaN included, is the lowest value and equals itself) or SYM equality/inequality, with canonical `and`/`or` and unary `not`. The proof allows 128 visits and depth 32 per IF expression. Quoted SYM names that resolve to source columns decline this proof. An `if` of this grammar may also be an operand in `where:`, as in `(== (if (== src 'x) 'A 'B) 'B)`, beside any outputs the ordinary proof admits; a comparison inside it never narrows the partition mask.
- **Grouped:** one bare I64 or SYM group key (not the partition key unless it is I64) and up to 32 outputs, each a fresh `sum(if condition then else)` with explicit ELSE and two I64 or two F64 branches, or an aggregate beside them: a native aggregate (`sum`, `count`, `avg`, `min`, `max`, `med`, `dev`, ...) or `first`/`last` of row-local arguments. Conditions must depend on a column; supported predicates include I64 or F64 comparisons and SYM equality/inequality against one unambiguous non-NULL literal. The shared proof allows 256 visits, depth 24 and 256 recorded references. No output, `where` or `by` may spell another output's name; only an aggregate beside the `sum(if)`s may spell its own (`qty: (sum qty)`). A bare projection or `count(distinct ...)` beside a `sum(if)` keeps the full source.

These limits apply only to the added proofs, not ordinary atomic traversal. Unsupported shapes conservatively keep the original source. In particular, arbitrary calls, lexical heads, IF branches of mixed types (I64 beside F64), composite, computed or other-typed grouping, `take`/sorting in the grouped IF proof, and variadic grouped logical predicates are not covered. Nonliteral partition bounds are not resolved by this pass. Ordinary nonlocal operations such as running sums or an aggregate inside `where` can require all rows and therefore prevent pruning.

```lisp
;; A supported narrow grouped shape; the literal date bound selects partitions.
(select {from: trades where: (== date 2024.01.02) by: account
         qty_ok: (sum (if (> qty 0) qty 0)) n: (count qty)})
```

The example assumes `account` and `qty` are I64 columns. A declined proof changes optimization, not permission to substitute a different query result.

A predicate on a parted integer or temporal data column prunes as well, by the partitions' statistics: the non-null minimum and maximum of a column saved with `compress: true` (its chunk directory) or of a plain column of 65,536 rows or more (its chunk zone). A partition is dropped only when they decide the comparison false for every row, by the rule row-level zone skipping uses, so nulls are kept and a partition without statistics is kept. `(> time 2024.01.16D12:00:00.000)` over a date-partitioned table thus reads the partitions whose `time` range reaches past the literal and no other. Partition directories may also be named `YYYY.MM.DD.HH.MM.SS`, which gives a `timestamp` partition key. See [`.db.parted.get`](../namespaces/db.md#db-parted-get).

A parted table whose partition-key column was dropped, as `(xcols P (drop (cols P) 1))` does, still narrows by its data columns' statistics, as the table with its key does.

## Symbol Table Persistence

The global symbol intern table maps strings to integer IDs. When saving data to disk, the symbol table must be persisted so that symbol vectors can be correctly interpreted when reloaded.

### Append-Only .sym Files

A table's symbol file, which `.db.splayed.set` and the importers write, is append-only: new symbols are appended to the end of the file without rewriting existing entries, so a symbol's position never changes and updates are incremental. One process writes a database at a time: a writer whose view of the file is out of date fails with `RAY_ERR_CORRUPT` instead of overwriting another writer's symbols (see [Running Several Instances](../guides/instances.md)).

The file is a header (the `STRL` magic and the number of symbols) followed by one record per symbol, a 4-byte length and the bytes.

The first write of a symbol file builds it under a temporary name and renames it into place. Every later `.db.splayed.set` that brings new symbols appends their records to the existing file, in place, and then writes the new count into the header; a save with no new symbols does not write the file. The header count is the commit point: the records are written first (and, in a durable save, synced to media before the count is written), so a crash leaves either the old count, or a count whose records are all there. Records past the count, left by an append that did not finish, are a torn tail: opening the file ignores them, and the next append cuts them off before it writes its own records. A file that holds fewer records than its count says is damaged and fails with a `corrupt` error.

A process that opens the file while another appends to it reads the symbols the header counted when it opened the file, never a count over records it cannot see. The records appended meanwhile are read at its next open of the file. An append cuts only bytes that no count covers: when the header no longer holds the count the process last read, its append fails with a `corrupt` error and leaves the file as it is.

The C functions below work on the runtime's global symbol table instead. `ray_sym_save` writes the whole table to a temporary file and renames it over the target, and `ray_sym_load` merges a file into the running table.

| Function | Description |
|---|---|
| `ray_sym_save(path)` | Persist the current global symbol table to a `.sym` file |
| `ray_sym_load(path)` | Load a symbol table from disk, merging with any existing entries |
| `ray_sym_intern(str, len)` | Intern a string, returning its integer ID |
| `ray_sym_find(str, len)` | Look up a string without interning (returns -1 if absent) |
| `ray_sym_str(id)` | Resolve an ID back to its string |

```c
// Save symbol table alongside data
ray_sym_save("db/.sym");

// On startup, load symbols before loading data
ray_sym_load("db/.sym");
```

### Concurrency and Integrity

- **File locking** — `flock()` on POSIX, `LockFileEx()` on Windows. Multiple processes can safely read the symbol file; writes acquire an exclusive lock.
- **Corruption detection** — symbol files carry no checksum. The loader checks the magic, the entry count and every entry's length; a truncated or malformed file makes `ray_sym_load()` return `RAY_ERR_CORRUPT`.
- **Arena backing** — interned strings are allocated from a dedicated arena (`ray_arena_t`), making bulk allocation fast and ensuring all strings are freed together when the symbol table is destroyed.

## CSV Import and Export

Rayforce includes a high-performance CSV loader with parallel parsing, automatic type inference, and null handling. No external libraries are used — the parser operates directly on memory-mapped file contents.

### Reading CSV Files

```text
; Basic CSV load — auto-detect types, comma delimiter, header row
ray> (set data (.csv.read "trades.csv"))
sym  price   qty  date
----------------------------
AAPL 150.25  100  2024.01.15
GOOG 140.50  200  2024.01.15
MSFT 380.00   50  2024.01.15
```

### C API

| Function | Description |
|---|---|
| `ray_read_csv(path)` | Load a CSV file with default options: comma delimiter, first row as header, and automatic type inference. Empty fields are treated as null. |
| `ray_read_csv_opts(path, delim, header, col_types, n_types)` | Load with custom options: delimiter character, whether first row is a header, explicit column type array (`int8_t*`), and number of type entries. Pass `NULL, 0` for automatic type inference. |
| `ray_write_csv(table, path)` | Write a table to a CSV file with header row and comma delimiter. |

```c
// Default options
ray_t* data = ray_read_csv("trades.csv");

// Tab-delimited, no header, auto type inference
ray_t* tsv = ray_read_csv_opts("data.tsv", '\t', false, NULL, 0);

// Write results back
ray_write_csv(result, "output.csv");
```

### Type Inference

The CSV loader samples values in each column and infers types in priority order:

1. **BOOL** — `true`/`false`, `1`/`0`
2. **I64** — integer values within 64-bit range
3. **F64** — floating-point values
4. **DATE** — `YYYY-MM-DD` format (hyphen-separated)
5. **TIMESTAMP** — date + time with nanosecond precision
6. **SYM** — short repeated strings (auto-interned as symbols)
7. **STR** — fallback for everything else

### Parallel Parsing

The CSV file is memory-mapped and split into chunks. Multiple threads parse chunks in parallel, with a merge step that reconciles column types and combines partial results. For large files (100 MB+), this delivers near-linear speedup with core count.

### Null Handling

Empty fields and fields matching the null string (default: `""`) are recognized as null values. The loader writes the appropriate type-correct null sentinels into the resulting vectors and marks them with `RAY_ATTR_HAS_NULLS`.

### Symbol Merge

When loading CSV data with symbol columns, the loader interns all unique strings into the global symbol table. If a symbol table was previously loaded from disk, existing IDs are preserved and new symbols are appended.

## Cross-Platform File I/O

All file operations go through a portable abstraction layer in `src/store/fileio.{h,c}` that handles platform differences:

| Feature | POSIX | Windows |
|---|---|---|
| File locking | `flock()` | `LockFileEx()` |
| Sync to disk | `fsync()` | `FlushFileBuffers()` |
| Atomic rename | `rename()` | `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` |
| Memory mapping | `mmap()` | `CreateFileMapping()` + `MapViewOfFile()` |

!!! note "Atomic writes"
    When saving column files, Rayforce writes to a temporary file first, calls `fsync()`, then atomically renames it to the target path. This prevents data corruption if the process is interrupted during a write.

## Rayfall Storage Builtins {#rayfall-storage}

Rayfall provides three builtins for working with on-disk storage directly from the REPL or scripts.

### `.db.splayed.set` — Save a Splayed Table

Writes a table as column files to a directory. A symbol table file is saved automatically alongside the data.

```lisp
(set t (table [City Temp Rain]
  (list [London Paris Tokyo]
        [15 22 28]
        [120.5 60.3 200.1])))

; Save to /tmp/weather
(.db.splayed.set "/tmp/weather" t)
```

An optional third argument specifies a custom symbol table path: `(.db.splayed.set "/tmp/weather" t "/tmp/my_sym")`. Without it, a `sym` file is created in the table directory.

The table may be a parted one: it is then written a partition segment at a time, never held in memory (see [Writing a Parted Table as One Splayed Table](#writing-a-parted-table-as-one-splayed-table)).

A trailing options dict selects the column file format: `(.db.splayed.set "/tmp/weather" t {compress: true})`, also after the symbol path. `true` writes generation-2 (compressed) files where eligible, `false` generation 0; when absent each file inherits (generation 2 when its source is an encoded column or the destination's current file is generation 2). Unknown keys or a non-bool value are a `domain` error. Linked and nested/list columns are always written uncompressed. Compressed files record their indexes in the file header: those you built, and the dictionary or chunk zone a save builds on its own.

A compressed float column is lossless, and a `sum` or `avg` over it gives the same bits as over its generation-0 file, at any worker count and whatever floating-point flags the library was built with. `(sum v)` and `(avg v)` of an F64 or F32 column or vector, nulls or not, and a `select` whose only aggregate is that `sum` or `avg` (no `by:`, no `where:`) add the rows in one order: row r into the r mod 32nd of thirty-two running sums inside chunks fixed by the row count, the sums combined in a fixed tree and the chunks added in order. So these forms also agree with each other. It is not the rows' left-to-right order, so the low bits can differ from a sum written as a loop. A `select` with `where:`, `by:` or other aggregates beside the sum adds row by row in its own order, again the same for both generations at a given worker count; its low bits can differ from `(sum v)`. For a `by:` over more than 65,536 rows those bits can also change with the worker count, since the groups' partial sums are combined per worker. Because each of the thirty-two running sums takes every 32nd row, data that alternates near the largest magnitudes (for example ±1.7e308) can overflow a running sum that a left-to-right loop would not, and the sum is then null.

A save also builds indexes of its own for columns of at least 65 536 rows: a dictionary for a string column and a chunk zone (per-chunk min/max) for a numeric or temporal one. `{index: false}` builds neither, in either generation, for a table that is never queried by value (read by partition, by position or whole); the write is faster and the files are smaller. It governs only what the save builds. An index the column already carries is written as it always is: one you built (`.idx.hash`, `.idx.sort`, `.idx.bloom`), or a chunk zone a loaded column brought from its file, as when a table read from one splayed store is saved to another. A dictionary is only ever built, never carried over from the source, so `{index: false}` leaves a loaded string column without one. A column without an index reads as it does after a save of a short one. A generation-2 (compressed) save keeps a string column's dictionary only while the file with it is no larger than the generation-0 file of the same column with its dictionary, headers and padding included: 16 bytes a row plus the bytes of every string over 12 bytes for the column, 4 bytes a row and 4 bytes per distinct string for the dictionary (generation 0 reads the dictionary's strings through the column), plus 256 bytes of headers, each part padded to 32 bytes. A generation-2 dictionary also stores an uncompressed copy of every distinct string, so for mostly distinct strings (IDs, raw messages, payloads) it is left out and the column is the file `{index: false}` writes: `.idx.info` reports `index: 'none`. 140 000 distinct 264-byte strings save as 39.2 MB, as raw, instead of 78.4 MB. Each file decides for its own rows, on every save, re-save, partition, write of a parted table and `.csv.splayed` or Parquet import; generation 0 is unchanged. Such a column answers equality filters, `distinct` and group-by without the dictionary's codes, so they read the strings: on 1 000 000 distinct short IDs a point filter took 0.94 ms instead of 0.04 ms and a group-by 45 ms instead of 3.8 ms, with a 3.6 MB file instead of 27.5 MB (one measurement, 28-core Linux). `{index: true}` keeps the dictionary whatever it costs, for a column filtered or grouped by value: the 140 000 strings above then take 78.4 MB. So the option has three states: absent builds the automatic indexes under this size rule, `true` builds them all, `false` builds none; a non-bool value is a `domain` error. The CSV and Parquet converters take no `index` option and follow the size rule. `compress`, `symfile` and `index` combine in one dict: `{compress: true index: false}`.

`{durable: false}` skips every fsync of the save (column files, symbol files, directories), the path `.csv.splayed` and the Parquet import already take; `true`, the default, syncs as before. Column files are still written through temporary files and atomic renames, so a reader sees whole files; the shared symbol file is appended to in place, as described under [Append-Only .sym Files](#append-only-sym-files). A crash or power loss may lose the write or leave it torn. For a table that is republished often and can be rebuilt from its source. A non-bool value is a `domain` error; it combines with the other options: `{durable: false compress: true}`.

### Compressed floating-point environment

Generation-2 compressed F32/F64 I/O requires **default floating-point controls** on every calling/executing thread: round-to-nearest (`FE_TONEAREST`), masked FP exceptions, and gradual underflow (FTZ and DAZ disabled). Embedders must maintain those controls during calls. Rayforce does not silently change or normalize them; sticky exception status flags need not be clear and arithmetic may set them. Detected unsupported controls produce `fpenv` (`RAY_ERR_FPENV` in C), not a corruption error. Restore supported controls before retrying the same mapped column; an environment failure is transient, not permanently cached.

The refusal applies conservatively to compressed float writes and whole-column materialization, including decoded-cache hits and non-ALP encodings. ALP range decoding also checks the executing thread. It does not change the file format or require every non-ALP range read to reject nondefault controls.

Detection is deliberately limited: standard rounding is checked, plus SSE rounding, FTZ/DAZ and exception masks on supported x86 SSE builds. This is **not a complete portable FP-environment validator**: x87 trap masks are not checked, and on other native architectures only standard rounding is checked. WebAssembly has fixed nearest rounding. Maintaining other required controls remains the embedder's responsibility; passing a check is not proof that every control is supported.

### `.db.splayed.get` — Load a Splayed Table

Loads a splayed table from a directory. Columns are memory-mapped: generation-0 files for zero-copy access, while a compressed (generation-2) column decodes into heap memory the first time a query reads it.

```lisp
(.db.splayed.get "/tmp/weather")
```

```text
┌─────────┬──────┬────────────────────┐
│  City   │ Temp │        Rain        │
│   SYM   │ I64  │        F64         │
├─────────┼──────┼────────────────────┤
│ 'London │ 15   │ 120.5              │
│ 'Paris  │ 22   │ 60.3               │
│ 'Tokyo  │ 28   │ 200.1              │
└─────────┴──────┴────────────────────┘
```

An optional second argument specifies the symbol table path: `(.db.splayed.get "/tmp/weather" "/tmp/my_sym")`.

### `.db.parted.get` — Load a Partitioned Table

Loads a date-partitioned table. The first argument is the database root directory, the second is the table name as a quoted symbol.

```lisp
; Load the "trades" partitioned table from db/
(set dbroot "/tmp/rayforce-storage-db")
(set symfile (format "%/.sym" dbroot))
(set jan15 (table [sym price qty] (list [AAPL GOOG] [150.5 2800.0] [100 50])))
(set jan16 (table [sym price qty] (list [MSFT] [410.0] [75])))
(.db.splayed.set (format "%/2024.01.15/trades" dbroot) jan15 symfile)
(.db.splayed.set (format "%/2024.01.16/trades" dbroot) jan16 symfile)
(.db.parted.get dbroot 'trades)
```

This scans all date-named subdirectories under `db/` (loading `db/<date>/trades/` from each), memory-maps every column, and returns a single table with a virtual date column. Partition pruning applies to subsequent queries.

### `.db.parted.tables` — List a Root's Table Names

Returns a sorted `sym` vector of the table names available under a partitioned database root — the table subdirectories of the **most recent** (last, sorted) partition, which reflects the current table set. Each name can be passed straight to `.db.parted.get`; nothing is loaded by this call.

```lisp
; Discover which tables live under db/, then load each one
(.db.parted.tables dbroot)                ; => [`trades]
(map (fn [t] (.db.parted.get dbroot t)) (.db.parted.tables dbroot))
```

### `.db.parted.fill` — Backfill Missing Tables Across Partitions

For every table present in **any** partition, ensures **every** partition has it: a partition missing the table gets an **empty** copy whose schema is taken from the most recent partition that has it. This keeps queries that span partitions from failing on a partition where a table is absent — typically a table added partway through the database's life. Returns a sorted `sym` vector of the partition names it filled (empty when nothing needed fixing, so a repeat call is a no-op). Requires write permission on the root.

```text
; `news` was only added from 2024.01.10 onward; backfill the earlier days.
(.db.parted.fill "db")                  ; => [`2024.01.01 … `2024.01.09]
; now every partition has every table — the empty copies add no rows.
(count (.db.parted.get "db" 'news))     ; unchanged
```

## Symbol Table Management

Symbol tables are persisted automatically when you use `.db.splayed.set`. A `.sym` file is written into the table directory containing all interned symbol strings. When loading with `.db.splayed.get` or `.db.parted.get`, the symbol table is loaded first so that symbol columns decode correctly.

At the C API level, `ray_sym_save(path)` and `ray_sym_load(path)` handle persistence directly. `ray_sym_save` writes the whole runtime table to a temporary file and renames it over `path`; one process writes a given file.

## Working Example: Round-Trip

A complete workflow: create a table, save it to disk, load it back, and verify the data.

```lisp
; 1. Create a table
(set t (table [City Temp Rain]
  (list [London Paris Tokyo]
        [15 22 28]
        [120.5 60.3 200.1])))

; 2. Save as splayed table
(.db.splayed.set "/tmp/weather" t)

; 3. Load it back
(set loaded (.db.splayed.get "/tmp/weather"))

; 4. Query the loaded table
(select {from:loaded where: (> Temp 20)})
```

```text
┌────────┬──────┬────────────────────┐
│  City  │ Temp │        Rain        │
│  SYM   │ I64  │        F64         │
├────────┼──────┼────────────────────┤
│ 'Paris │ 22   │ 60.3               │
│ 'Tokyo │ 28   │ 200.1              │
└────────┴──────┴────────────────────┘
```

The loaded table is fully functional — you can run `select`, `update`, joins, and aggregations on it just like an in-memory table. Symbol columns resolve correctly because the symbol table was saved and loaded alongside the data.

## Duration column compatibility

TIMESPAN uses the concrete type tag 16 and a signed 64-bit nanosecond
payload. Serialization, generation-0 and generation-2 splayed columns,
and partitioned columns preserve the tag and `0Nn` nulls. Tags 14 and 15
remain internal selection and encoded-column tags.

Readers built before TIMESPAN support reject the new tag, including when
it appears in a table schema. Upgrade readers before writing duration
columns to shared storage. Existing column tags and format generations
retain their meanings. Parquet logical time and duration mappings are
separate from this column format.

## GUID hash index compatibility

A splayed save persists a hash index on a GUID column (`.idx.hash`,
`grouped`) in both format generations, so the index is there after a
load. Readers built before GUID hash indexes, v3.0.1 among them, treat
the two generations differently:

- **Generation 0.** They decline the index and load the column without
  it; every answer is the same.
- **Generation 2** (`{compress: true}`). They open the table and read its
  other columns, but reading the GUID column fails with
  `corrupt: index kind 1 is not admitted on type GUID`. The values on
  disk are intact; only the index is unknown to them.

Upgrade readers before writing indexed GUID columns to shared
generation-2 storage, or save the column without its index
(`.idx.drop`). Hash indexes on other column types, including one an
append extended, keep the format older readers read. Existing column
tags and format generations retain their meanings.

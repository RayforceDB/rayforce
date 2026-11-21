# `.parquet.*` — Parquet input and native conversion

Read or query a local Parquet file, process it in batches, or convert it to
Rayforce's native splayed or partitioned layout. The reader and the Snappy and LZ4
decoders are implemented in C without third-party library dependencies.

!!! note "Restricted under `-U`"
    All six functions are restricted for IPC peers under a `-U` server and
    return an `access` error. Querying a previously created lazy descriptor is
    also restricted. Local scripts and the REPL are unaffected.

## Reference

| Function | Arguments | Returns |
| --- | --- | --- |
| [`.parquet.meta`](#parquet-meta) | `path` | Column-chunk metadata table |
| [`.parquet.read`](#parquet-read) | optional projection/options, `path` | Materialized table |
| [`.parquet.scan`](#parquet-scan) | optional options, `path` | Reusable lazy `select` source |
| [`.parquet.each`](#parquet-each) | optional projection/options, `path`, callback | Number of rows delivered |
| [`.parquet.splayed`](#parquet-splayed) | optional types/options, `path`, destination | Imported row count |
| [`.parquet.parted`](#parquet-parted) | optional types/options, `path`, root, table name | Imported row count |

Paths are nonempty strings. Type overrides are positional SYM vectors covering
the entire source schema. Supported formats, type conversions, strict fidelity
checks, pruning, and memory limits are described in the
[Parquet storage guide](../guides/storage.md#parquet-input).

## `.parquet.meta` { #parquet-meta }

Read footer metadata without decoding data pages. The returned table has one
row per column chunk, including its row group, type, codec, compressed and
uncompressed sizes, and the presence of statistics or optional indexes. A column the reader
cannot read has one row per row group too, with its reason in `unsupported`.

```lisp
(.parquet.meta "events.parquet")
```

## `.parquet.read` { #parquet-read }

Materialize all columns, a projection vector, or a read configured with
`columns`, `types`, `strict`, and an inclusive signed-integer `range`.

```lisp
(.parquet.read "events.parquet")
(.parquet.read [id text] "events.parquet")
(.parquet.read
  {columns: [text] range: {column: id min: 10 max: 20}}
  "events.parquet")
```

The whole result remains in memory. Typed `SYM` text uses a private dictionary
that remains valid for as long as returned vectors retain it.

Column types: `UINT64` reads as `I64`, and a value above `INT64_MAX`
(including 2^63) fails the read. `DECIMAL` reads as `F64`, the stored integer
divided by 10^scale, on `INT32`, `INT64`, `FIXED_LEN_BYTE_ARRAY` (up to 16
bytes) and `BYTE_ARRAY` columns (up to 38 digits). On a byte array the
big-endian integer is first rounded to the nearest `F64` (ties to even), so the
result is exact only while the unscaled integer is below 2^53.

A column the reader cannot read (nested `LIST`, `MAP` or `STRUCT`, `INT96`, a
`FIXED_LEN_BYTE_ARRAY` that is not a `DECIMAL`) does not make the file
unreadable: a projection, or a query that never names the column, reads the
others, and `.parquet.meta` lists every column, giving the reason in its
`unsupported` column (null for a readable one). Naming such a column, or
reading all columns, fails with `column <name>: <reason>`.

## Encodings and limits { #parquet-encodings }

Value encodings: `PLAIN`, dictionary (including the legacy dictionary tag and
the plain fallback a writer may take within a chunk), RLE-encoded `BOOLEAN`
values, `DELTA_BINARY_PACKED` on `INT32` and `INT64`, and
`DELTA_LENGTH_BYTE_ARRAY` and `DELTA_BYTE_ARRAY` on `BYTE_ARRAY`. A page in any
other value encoding fails the read with `unsupported value encoding`, followed
by the supported list.

A page decodes to at most 64 MiB. For `DELTA_BYTE_ARRAY` the bound covers the
page's reconstructed total, so a page whose prefix chain would rebuild past it
is refused with `delta byte array page expands past the 64 MiB page limit`, on
`.parquet.read` and on both symbol import paths.

A decoded batch's string pool holds up to 4 GiB. A `SYM` import of
`DELTA_BYTE_ARRAY` values copies each page's reconstructed values into an arena
capped at a sixteenth of the engine's memory budget (`-m` when set, the machine
limit otherwise), never below 1 MiB and never above 4 GiB; a value repeated by
the page's prefix chain is interned from the copy already there. An import that
would grow the arena past its cap fails with `delta byte array expansion exceeds
the memory budget`.

## `.parquet.scan` { #parquet-scan }

Create a descriptor for `select`. Each query reopens the file and infers eligible
projections and predicates. Options are `types` and `strict`; put filters in
the query itself. Creation does not read or validate the file.

```lisp
(set events (.parquet.scan "events.parquet"))
(select {from: events n: (count id)})
(select {from: events text: text where: (within id [10 20])})
```

Simple global aggregates can stream. Grouping, distinct and ordering currently
materialize candidate rows and selected columns before ordinary query execution.

## `.parquet.each` { #parquet-each }

Call a function with consecutive batches of at most 65,536 rows. Read options
match `.parquet.read`. Callbacks run in source order on the calling thread;
their return values are discarded, and an error stops the scan. The result is
the total number of rows delivered after reader filtering.

```lisp
(.parquet.each [id text] "events.parquet" (fn [batch] (show batch)))
```

Each callback sees one batch: a grouped count inside it is local to that batch.
Retaining batches increases memory use.

## `.parquet.splayed` { #parquet-splayed }

Convert into a new native directory and return the row count. The optional
argument is a type vector or `{types: [...] rows: n strict: true compress: true}`. For example,
a source containing an integer ID and text can be imported as:

```lisp
(.parquet.splayed [I64 SYM] "events.parquet" "/tmp/events-native")
(set events (.db.splayed.get "/tmp/events-native"))
```

## `.parquet.parted` { #parquet-parted }

Convert into `root/<partition-number>/<table-name>`, preserving source row
order. The default maximum partition size is 65,536 rows. `rows` accepts
1 through 1,048,576; source row-group boundaries also end partitions.

```lisp
(.parquet.parted {types: [I64 SYM] rows: 262144}
  "events.parquet" "/tmp/events-db" 'events)
(set events (.db.parted.get "/tmp/events-db" 'events))
```

`.parquet.parted` also takes a list of paths in place of the path. The files
are concatenated in order into one table: partitions continue across the files,
string columns imported as `SYM` share one vocabulary, and the files must have the
same columns, in order, with the same native types after the import's type
conversions (a column that differs is refused, naming it). This is how the
several files of one export, one per instrument and definition segment, become
a single on-disk table:

```lisp
(.parquet.parted {types: [I16 I32 I64 SYM TIMESTAMP F64 F64 F64 F64 F64] compress: true}
  (list "btc-1.parquet" "btc-2.parquet" "eth-1.parquet") "/tmp/ohlcv-db" 'ohlcv)
```

Both native conversions require an existing parent directory and a new
destination. They write to `<destination>.parquet-partial`, sync the completed
output, and publish it without replacing an existing destination. Failed staging
data is retained; existing staging directories are rejected. Imports do not
support resume or append. Format or conversion failures return `parquet`
errors; argument, I/O, allocation, and cancellation errors also propagate.

The options dict also takes `compress`: `true` writes generation-2
(compressed) column files where the column is eligible, `false` (or no
option, since the destination is always new) generation 0. A non-bool
`compress` or a second options dict is a `domain` error. Compressed files keep their chunk zones and persist the same indexes a generation-0 save does, unless a string column's dictionary would make the file larger than the generation-0 one (mostly distinct strings; see `.db.splayed.set` in [Storage](../storage/index.md)): the import writes through the same splayed-table writer as `.db.splayed.set`.
For example:

```lisp
(.parquet.splayed {compress: true} "events.parquet" "/tmp/events-native")
(.parquet.parted {compress: true rows: 262144} "events.parquet" "/tmp/events-db" 'events)
```

`.parquet.splayed` also takes `symfile`: `'shared` (the default) writes one
`.sym` for every `SYM` column; `'column` gives each `SYM` column a symbol file
of its own, `.<column>.sym` next to the column file, so a scan of one column
(`like`, a grouping that resolves its strings) reads that column's strings
only. A string that several columns hold is then stored once per column.
`.db.splayed.get` finds the per-column files by themselves. Any other value is
a `domain` error, and `.parquet.parted` refuses `'column`: every partition of a
parted table resolves over the one root `.sym`.

```lisp
(.parquet.splayed {types: [I64 SYM SYM] symfile: 'column} "hits.parquet" "/tmp/hits")
```

# `.parquet.*` — Parquet input and native conversion

Read or query a local Parquet file, process it in batches, or convert it to
Rayforce's native splayed or partitioned layout. The reader and Snappy decoder
are implemented in C without third-party library dependencies.

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
uncompressed sizes, and the presence of statistics or optional indexes.

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
argument is a type vector or `{types: [...] rows: n strict: true}`. For example,
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

Both native conversions require an existing parent directory and a new
destination. They write to `<destination>.parquet-partial`, sync the completed
output, and publish it without replacing an existing destination. Failed staging
data is retained; existing staging directories are rejected. Imports do not
support resume or append. Format or conversion failures return `parquet`
errors; argument, I/O, allocation, and cancellation errors also propagate.

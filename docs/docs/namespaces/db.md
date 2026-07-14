# `.db.*` — on-disk tables

Persist and reload tables. Rayforce stores tables in two on-disk shapes:

- **Splayed**: one directory per table, one file per column, plus a `.d` schema file and a `.sym` symbol-table file. The standard layout for a single-table dataset.
- **Partitioned (parted)**: a database root containing one subdirectory per partition (date or other numeric/dotted name); each partition contains splayed-style table directories. A single shared `.sym` file sits at the root. The query optimizer can prune partitions when predicates select on the virtual `MAPCOMMON` partition column.

The `get` builtins memory-map every column file — load is constant-time regardless of dataset size. `set` writes a table's columns to a splayed directory. A loaded parted table can also grow an in-memory live tail with `insert`; that operation is deliberately separate from persistence.

!!! note "Restricted under `-U`"
    `.db.splayed.set` and `.db.parted.fill` are `RAY_FN_RESTRICTED` (they write to disk). The `get`/`tables` builtins are read-only and unrestricted.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.db.splayed.set`](#db-splayed-set) | variadic | restricted | Save a table to a splayed directory. |
| [`.db.splayed.get`](#db-splayed-get) | variadic | — | Load a splayed table (columns mmap'd). |
| [`.db.parted.get`](#db-parted-get) | variadic | — | Load a partitioned table by name from a db root. |
| [`.db.parted.tables`](#db-parted-tables) | variadic | — | List the table names available under a parted db root. |
| [`.db.parted.fill`](#db-parted-fill) | variadic | restricted | Fill missing tables across a parted db's partitions. |

## `.db.splayed.set` { #db-splayed-set }

Signatures:

- `(.db.splayed.set "dir" tbl)` — write columns to `dir/`; emit a `.sym` file in the same directory.
- `(.db.splayed.set "dir" tbl "sym_path")` — write the symbol table to a custom location (typically a shared db-root `.sym`).

Returns `tbl` (retained), so the call can be threaded inside a larger expression.

Errors: `type` (dir/sym not strings, tbl not a table), `domain` (paths empty / too long), and any `io`/`oom`/`corrupt` error code surfaced by the splayed writer.

```lisp
(set t (table [city temp rain]
  (list ['London 'Paris 'Tokyo] [15 22 28] [120.5 60.3 200.1])))

(.db.splayed.set "/tmp/weather" t)
```

## `.db.splayed.get` { #db-splayed-get }

Signatures:

- `(.db.splayed.get "dir")` — look for `dir/.sym` automatically.
- `(.db.splayed.get "dir" "sym_path")` — use a custom symbol-table path (e.g. a shared db-root sym).

Returns a `table` with every column memory-mapped — zero allocation per row.

```lisp
(set loaded (.db.splayed.get "/tmp/weather"))
(select {from: loaded where: (> temp 20)})
```

## `.db.parted.get` { #db-parted-get }

Signature: `(.db.parted.get "db_root" 'tbl_name)`. The table name **must** be a quoted symbol atom (e.g. `'trades`), not a string.

Returns a single logical table assembled from every partition directory under `db_root/tbl_name/`. The result carries a virtual `MAPCOMMON` partition column derived from directory names, and every data column is a `RAY_PARTED_*` view over the segment files. Partition pruning kicks in automatically for `select` predicates on the virtual column.

```lisp
(set dbroot "/tmp/rayforce-parted-db")
(set symfile (format "%/.sym" dbroot))
(set jan15 (table ['sym 'price 'qty]
             (list ['AAPL 'GOOG] [150.5 2800.0] [100 50])))
(set jan16 (table ['sym 'price 'qty]
             (list ['MSFT] [410.0] [75])))
(.db.splayed.set (format "%/2024.01.15/trades" dbroot) jan15 symfile)
(.db.splayed.set (format "%/2024.01.16/trades" dbroot) jan16 symfile)
(set trades (.db.parted.get dbroot 'trades))

;; Partition prune — only the matching day's files are touched.
(select {from: trades where: (== date 2024.01.15)})
```

Errors: `domain` (arity != 2 or `tbl_name` invalid), `type` (root not a string or name not a sym), `name` (sym ID unknown).

### In-memory live-tail inserts

Use `(insert parted partition-key rows)` to build a fresh logical view containing immutable history and a growing current partition; the source value remains unchanged. Quote a bound symbol, as in `(insert 'parted partition-key rows)`, to rebind it to that fresh view. A validated zero-row batch returns the existing view without rebinding or creating a partition. The source is normally a table returned by `.db.parted.get`.

The row payload describes the physical splayed schema only: omit the virtual `date` or `part` column, and supply exactly one matching value per physical column. A list follows physical column order. A table or dictionary may reorder its payload, but must contain every physical column name exactly once. Atoms append one row; equal-length vectors append a batch, with atom values broadcast across that batch. Concrete vector types must match exactly. Generic `null` is accepted for sentinel-nullable columns; it becomes the empty value for `SYM`/`STR`, while non-nullable `BOOL`/`U8` reject it.

The key may equal the table's last partition key, growing the current segment, or be strictly later, starting a new current segment. Inserts into any earlier or non-last partition are rejected; historical partitions are immutable. The loaded partition keys must already be strictly increasing in their logical type. In particular, zero-pad integer directory names if their lexical order would otherwise differ from numeric order. All historical physical segments must be present. A missing segment in the last partition can be repaired only by a non-empty same-key append; until repaired it blocks advancing the key. Missing `BOOL`/`U8` cannot be null-backfilled when the partition already has rows, but a zero-row segment needs no backfill.

For a non-empty insert, the quoted-symbol form rebinds the target to the new logical view. Existing historical mmap segments are retained, while a segment receiving live rows becomes heap-backed. Queries and values that already retained the previous table remain stable snapshots; queries that resolve the symbol after the rebind see the new rows. Same-key growth rebuilds partition metadata and copies only the active segment; a later key builds a new tail. Batch incoming rows to avoid repeatedly copying a growing intraday segment.

!!! warning "Memory only — no implicit durability"
    Live-tail insert does **not** create or change any on-disk partition, column, `.d`, or `.sym` file. An unpersisted tail is lost when the process exits. `upsert` is not supported on parted tables.

All `SYM` segments in the parted table share the FILE domain loaded from the database's `.sym`. Existing rows store stable positions in that domain. A live insert resolves existing symbols there and appends novel symbols to an in-memory extension shared by every symbol column and partition; it never rewrites historical positions. Live insert alone persists neither the new vocabulary nor its rows. Explicit `.db.splayed.set` rollover writes the enlarged `.sym` first; if a later column write fails, unused vocabulary entries can remain durable and the affected physical partition must be repaired or rewritten before reload.

A typical production cycle loads history once, batches rows into the live tail during the day, then explicitly persists the completed physical partition at rollover. Serialize rollover with ingestion so the projected snapshot is stable:

```lisp
(set dbroot "/data/market")
(set symfile (format "%/.sym" dbroot))
(set trades (.db.parted.get dbroot 'trades))

;; Named batches match physical columns by name and may reorder them.
;; `date` is virtual and therefore absent from the payload.
(insert 'trades 2024.01.16
  {qty: [200 75]
   sym: ['AAPL 'MSFT]
   price: [151.0 410.0]})

;; A positional list follows physical order: [sym price qty].
;; The scalar price broadcasts across the two-row vector batch.
(insert 'trades 2024.01.16
  (list ['AAPL 'GOOG] 151.25 [50 25]))

;; Disk history and the heap-backed current day query as one table.
(select {from: trades where: (== date 2024.01.16)})

;; Rollover: stop/serialize ingestion, project away the virtual column,
;; persist the complete day explicitly, and reopen mmap-backed history.
(set closed-day
  (select {from: trades
           where: (== date 2024.01.16)
           sym: sym price: price qty: qty}))
(.db.splayed.set
  (format "%/2024.01.16/trades" dbroot) closed-day symfile)
(set trades (.db.parted.get dbroot 'trades))

;; The next strictly later key starts the new live tail.
(insert 'trades 2024.01.17 (list 'AAPL 152.0 100))

;; Several later memory-only dates may coexist. After advancing to the 18th,
;; only that last key may grow; another insert into the 17th is rejected.
(insert 'trades 2024.01.18 (list 'MSFT 412.0 40))
```

A complete, repeatable version is available in
[`examples/rfl/parted_live_tail.rfl`](https://github.com/RayforceDB/rayforce/blob/master/examples/rfl/parted_live_tail.rfl). It creates a small database under `/tmp`, demonstrates both batch forms and unified queries, performs rollover/reload, creates multiple live dates, and cleans up afterward:

```bash
./rayforce examples/rfl/parted_live_tail.rfl
```

## `.db.parted.tables` { #db-parted-tables }

Signature: `(.db.parted.tables "db_root")`.

Returns a sorted `sym` vector of the table names available under a parted `db_root` — the splayed-table subdirectories (those with a `.d` schema) of the **most recent** (last, sorted) partition, which reflects the current table set. Each name can be passed straight to `.db.parted.get`; nothing is loaded or bound by this call.

```lisp
(.db.parted.tables dbroot)
;; => [`trades]

;; load each discovered table by name
(map (fn [t] (.db.parted.get dbroot t)) (.db.parted.tables dbroot))
```

An existing root with no partition directories (a freshly-created or non-parted directory) lists **no tables** — the call returns an empty `sym` vector rather than failing.

Errors: `domain` (arity != 1), `type` (root not a string), `io` (root missing or unreadable — `opendir` fails).

## `.db.parted.fill` { #db-parted-fill }

Signature: `(.db.parted.fill "db_root")`.

For every table that appears in **any** partition, ensures **every** partition has it: a partition missing the table gets an **empty** copy whose schema is taken from the most recent partition that does have it. This keeps `select`s that span partitions from failing on a partition where a table is absent — the typical case being a table added partway through the database's life, or a partition written before that table existed.

Returns a sorted `sym` vector of the partition names that were filled (an **empty** vector when nothing needed fixing, so a repeat call is a no-op). Requires write permission on the db root.

```text
;; trades exists in every day, but `news` was only added from 2024.01.10 on.
(.db.parted.fill "/data/db")
;; => [`2024.01.01 `2024.01.02 … `2024.01.09]   ; days that gained an empty `news`

;; now every partition has every table; cross-partition queries are safe.
(select {from: (.db.parted.get "/data/db" 'news)})
```

The filled copies are empty, so aggregate results across the db are unchanged — only the on-disk uniformity is. An existing root with no partition directories is a no-op: the call returns an empty `sym` vector. Errors: `domain` (arity != 1), `type` (root not a string), `io` (root missing or unreadable), plus any `oom`/`corrupt` surfaced while reading a template or writing a copy.

## See also

- [`.csv.splayed`](csv.md#csv-splayed) / [`.csv.parted`](csv.md#csv-parted) — stream CSV directly into splayed / parted layouts.
- [Columnar Storage](../storage/index.md) — file format, mmap semantics, symbol-table persistence.

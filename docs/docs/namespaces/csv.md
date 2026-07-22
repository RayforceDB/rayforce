# `.csv.*` — CSV import and export

CSV is Rayfall's bulk text I/O format. The `.csv.*` builtins cover four shapes: in-memory read, in-memory write, direct-to-splayed conversion, and direct-to-partitioned conversion. The reader is parallel and zero-copy on the input side (mmap), supports automatic type inference, and accepts an optional schema for deterministic typing. The splayed and parted variants stream straight from the source CSV into on-disk column files without ever materialising the full table in memory — useful for files that don't fit.

!!! note "Restricted under `-U`"
    All four CSV builtins (`.csv.read`, `.csv.write`, `.csv.splayed`, `.csv.parted`) are `RAY_FN_RESTRICTED` and return an `access` error when invoked from an IPC peer under a `-U` server. Local REPL / script execution is unaffected.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.csv.read`](#csv-read) | variadic | restricted | Parse a CSV file into an in-memory table. |
| [`.csv.write`](#csv-write) | variadic | restricted | Serialise a table to a CSV file. |
| [`.csv.splayed`](#csv-splayed) | variadic | restricted | Stream a CSV directly into a splayed on-disk table; return it loaded. |
| [`.csv.parted`](#csv-parted) | variadic | restricted | Stream a CSV directly into a partitioned table under a db root. |

## `.csv.read` { #csv-read }

Signatures:

- `(.csv.read "path")` — auto-detect column types from the header + a content sample.
- `(.csv.read [types] "path")` — types is a SYM vector naming `B8`, `I64`, `F64`, `STR`, `SYMBOL`, `F32`, `DATE`, `TIME`, `TIMESTAMP`, `GUID` (case-insensitive). One entry per column.
- `(.csv.read [names] [types] "path")` — also override the column names. With explicit names the input is assumed to have **no header row**.

Returns: a `table`. Empty fields use the selected type's missing-input behavior. Sentinel-nullable columns record nulls with `RAY_ATTR_HAS_NULLS`; `SYM`/`STR` fields collapse to ordinary empty values and those columns do not acquire `RAY_ATTR_HAS_NULLS` from them.

Errors: `type` (bad arg), `domain` (path is null / too long, or zero columns), `io` (open/read failure), `limit` (>256 columns).

Examples:

```lisp
(write "/tmp/rayforce-trades.csv"
  "sym,price,qty,date\nAAPL,150.5,100,2024.01.15\nGOOG,2800.0,50,2024.01.16\n")
(write "/tmp/rayforce-trades-headerless.csv"
  "AAPL,150.5,100,2024.01.15\nGOOG,2800.0,50,2024.01.16\n")

;; Auto-typed
(set t (.csv.read "/tmp/rayforce-trades.csv"))

;; Force schema — explicit column types
(set t (.csv.read [SYMBOL F64 I64 DATE] "/tmp/rayforce-trades.csv"))

;; Force schema AND headerless input
(set t (.csv.read [sym price qty date] [SYMBOL F64 I64 DATE] "/tmp/rayforce-trades-headerless.csv"))
```

The reader memory-maps the input, splits it into chunks, parses chunks in parallel, then merges column types and symbol-intern tables. Speedup is near-linear with core count on files of 100 MB+.

## `.csv.write` { #csv-write }

Signature: `(.csv.write tbl "path")`. Writes `tbl` with a header row and comma delimiter. Returns `0` on success.

Errors: `type` (tbl isn't a table, path isn't a string), `domain` (empty path), `io` on write failure.

```lisp
(.csv.write t "/tmp/rayforce-trades-out.csv")
```

## `.csv.splayed` { #csv-splayed }

Signatures:

- `(.csv.splayed "src.csv" "out_dir")`
- `(.csv.splayed [types] "src.csv" "out_dir")`
- `(.csv.splayed [names] [types] "src.csv" "out_dir")`

Streams `src.csv` chunk-by-chunk through the parser into a splayed directory at `out_dir` (one column file per field, plus a `sym` symbol-table file). Returns the resulting splayed table loaded with `ray_read_splayed` — zero-copy mmap, so the bytes stay on disk.

Use this when the CSV is too large to fit in memory but you want to query it like an in-memory table.

```lisp
(write "/tmp/rayforce-huge.csv" "id,price,sym\n1,10.5,AAPL\n2,11.5,GOOG\n")
(set big (.csv.splayed [I64 F64 SYMBOL] "/tmp/rayforce-huge.csv" "/tmp/rayforce-huge"))
;; big is now a splayed table mmap'd from /tmp/rayforce-huge/.
```

Errors propagate from the CSV parser (`type`, `domain`, `limit`) and from the splayed writer (`io`, `oom`).

## `.csv.parted` { #csv-parted }

Signatures (with optional rows-per-part):

- `(.csv.parted "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [types] "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [names] [types] "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [...]types "src.csv" rows_per_part "db_root" 'tbl_name)` — pass a positive integer just before the db-root to override the default partition size.

Streams the CSV into a partitioned table under `db_root/tbl_name/`, creating subdirectories per partition. Returns the parted table loaded back from disk.

Constraints on `tbl_name`: must not start with `.`, must not contain `/` or `\\` or `..`.

```text
;; Default partition size, schema-driven
(set t (.csv.parted [SYMBOL F64 I64 DATE] "trades.csv" "/data/db" 'trades))

;; Custom rows per partition (10M rows each)
(set t (.csv.parted [SYMBOL F64 I64 DATE] "trades.csv" 10000000 "/data/db" 'trades))
```

See [`.db.parted.get`](db.md#db-parted-get) for how to reload the resulting layout.

## See also

- [Storage — Columnar Storage](../storage/index.md) — column file layout, splayed and partitioned directory structure, symbol-table persistence.
- [Type Casting](../guides/types.md) — type-name resolution rules used by the schema argument.

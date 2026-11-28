# `.csv.*` — CSV import and export

CSV is Rayfall's bulk text I/O format. The `.csv.*` builtins cover four shapes: in-memory read, in-memory write, direct-to-splayed conversion, and direct-to-partitioned conversion. The reader is parallel and zero-copy on the input side (mmap), supports automatic type inference, and accepts an optional schema for deterministic typing. The splayed and parted variants stream straight from the source CSV into on-disk column files without ever materialising the full table in memory — useful for files that don't fit.

!!! note "Restricted under `-U`"
    All four CSV builtins (`.csv.read`, `.csv.write`, `.csv.splayed`, `.csv.parted`) are `RAY_FN_RESTRICTED` and return an `access` error when invoked from an IPC peer under a `-U` server. Local REPL / script execution is unaffected.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.csv.read`](#csv-read) | variadic | restricted | Load CSV file into table (mmap, parallel parse) |
| [`.csv.write`](#csv-write) | variadic | restricted | Write table to CSV file |
| [`.csv.splayed`](#csv-splayed) | variadic | restricted | Stream a CSV directly into a splayed on-disk table; return it loaded |
| [`.csv.parted`](#csv-parted) | variadic | restricted | Stream a CSV directly into a partitioned table under a db root |

## `.csv.read` { #csv-read }

Signatures:

- `(.csv.read "path")` — auto-detect column types from the header + a content sample.
- `(.csv.read [types] "path")` — types is a SYM vector naming `B8`, `I64`, `F64`, `STR`, `SYMBOL`, `F32`, `DATE`, `TIME`, `TIMESTAMP`, `GUID` (case-insensitive). One entry per column.
- `(.csv.read [names] [types] "path")` — also override the column names. With explicit names the input is assumed to have **no header row**.
- `(.csv.read {strict: true} ...)` — any arrangement above may start with an options dict. `strict` true refuses a malformed file instead of reading its bad cells as nulls; see [Strict mode](#csv-strict). `strict` is the only key: an unknown key, a non-bool value, a second options dict or one that is not the first argument is a `domain` error. Each arrangement takes exactly its own arguments; one more is an `arity` error.

Returns: a `table`. Empty fields use the selected type's missing-input behavior. Sentinel-nullable columns record nulls with `RAY_ATTR_HAS_NULLS`; `SYM`/`STR` fields collapse to ordinary empty values and those columns do not acquire `RAY_ATTR_HAS_NULLS` from them.

A row, and the header, ends at a line end, with quotes in the file or without: line ends are read greedily from left to right, a CRLF or an LFCR whenever the byte at the position starts one, else the single LF or CR (so `\n\r\n\r` is two line ends); inside a quoted cell these bytes are part of the text. `.csv.write` ends rows with LF and quotes every cell that holds a CR or an LF, so what it writes reads back cell for cell. Files that read differently before this rule: a file without quotes in which a CR stands alone under that reading (rows were split at LF only, and the text after such a CR in a line was dropped), a file with quotes that holds an LFCR (each read as two line ends, an empty row after each), a file whose header ends in LFCR (its first data row read as nulls), and a file whose header ends in a CR (its delimiter was guessed from the header and the data after it).

A UTF-8 byte-order mark (the bytes `EF BB BF`, as spreadsheet exports write) at the start of the file is skipped: it is part of neither the first column name nor, in a file without a header, the first cell. `.csv.splayed` and `.csv.parted` skip it too.

Errors: `type` (bad arg), `domain` (path is null / too long, zero columns, or a bad options dict), `io` (open/read failure), `limit` (>256 columns), `parse` (a bad row under `{strict: true}`).

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

### Strict mode { #csv-strict }

By default the reader is lenient: a cell it cannot read as its column's type becomes a null (`false` in a `B8` column, `0` in a `U8` one, which hold no null), a row with too few cells is padded with nulls, and cells past the last column are dropped. `{strict: true}` makes each of these an error instead, a `parse` error that names the file, its line and, for a cell, the column, its text and the type:

```text
.csv.read "bad.csv": line 3: expected 3 cells, got 2
.csv.read "bad.csv": line 5, column c: "abc" is not an F64
```

The rule: a non-empty cell must be stored as the value its text says. A strict load refuses a row when

- its cell count is not the schema's: a cell is a field ended by the delimiter, the line end or the end of the file, so a trailing delimiter adds an empty cell and a blank line is one empty cell — an error unless the table has one column. Without a header the first row is checked like any other (the schema, not the first line, says how many cells a row has);
- a non-empty cell does not parse as its column's type: `"abc" is not an F64`. A number with a space before or after it (`" 1"`, `"2.5 "`) is such a cell; a leading `+` is not. `SYM` and `STR` cells hold any text;
- a value is past its type's range: `"300" is out of range for a U8`. The default wraps these (300 in a `U8` reads as 44, 40000 in an `I16` as -25536, `9999-01-01T00:00:00` in a `TIMESTAMP` as an instant in 1815, `106752D00:00:00` in a `TIMESPAN` as a negative duration) or makes them an infinity (1e39 in an `F32`); a wrapped value is worse than a null. The ranges are `U8` 0 to 255; `I16`, `I32`, `I64` from their minimum plus one; `F32` and `F64` finite; `TIMESTAMP` 1707-09-22T00:12:43.145224193 to 2292-04-10T23:47:16.854775807; a finite `TIMESPAN` short of `0Wn`, the infinity it would otherwise read as (`0Wn` itself is accepted);
- a non-zero float text stores as zero: `"1e-400" underflows to zero for an F64` (`1e-50` in an `F32`), the counterpart of overflowing to an infinity. Rounding to the nearest value the type holds is not a loss and is accepted, a non-zero subnormal (`5e-324`, `1e-45` in an `F32`) included;
- the text spells the type's null: `"-32768" reads as a null I16`. These are the values each type keeps as its null sentinel: the minimum of `I16`, `I32`, `I64`, `TIME` (`-596:31:23.648`) and `TIMESTAMP` (`1707-09-22T00:12:43.145224192`), the all-zero `GUID`, and `0Nn` for a `TIMESPAN`;
- a time has more fraction digits than its type holds and they are not zeros: `"10:00:00.1234" has more fraction digits than a TIME holds` (`TIME` keeps milliseconds, `TIMESPAN` and `TIMESTAMP` nanoseconds);
- a quoted cell has text after its closing quote (`"ab"cd`) or no closing quote. The header's quoted cells are held to the same: `line 1: header cell 1: text after the closing quote`;

Without a schema the types checked are the inferred ones: a column inferred from the sample as `I64` refuses a later cell of text, so give a schema when the sample may not show every kind of value. An empty cell (`,,` or `""`) is a null in both modes. A final line without a newline is an ordinary row. The line is the file's: the header is line 1 when there is one, and newlines inside quoted cells count, so the line is the one an editor shows the row starting on. When several rows are bad the error names the first in the file, whatever the parallel split.

The message fits the 255 bytes an error holds, and only the cell text gives way: at most 40 bytes of it are shown, fewer when the path and the column leave less room, cut on a whole character and ending in `...`; quotes, control bytes and bytes that are not UTF-8 are escaped (`\"`, `\n`, `\x01`), never split. The path and the column name are shortened first only when they would leave the text no room, and the note `.csv.parted` adds about retained output always fits.

The default path costs nothing for the option: the lenient parse is unchanged, and the strict checks run only when the option is given. Strict costs a few percent of a load, more where cells are short: about 5% at one worker (3% across all cores) on files of eight mixed column types, measured up to 12%; about 10-15% on a file of one narrow numeric or date column, up to about 20% on one of only `TIME` or `TIMESPAN` cells, and 1% on one of `GUID` cells. `.csv.splayed` and `.csv.parted` share the reader and take the same option.

```lisp
(write "/tmp/rayforce-strict.csv" "a,b,c\n1,x,2.5\n2,y\n")
(.csv.read [I64 SYMBOL F64] "/tmp/rayforce-strict.csv")              ;; 2 rows, c is [2.5 0Nf]
(try (.csv.read {strict: true} [I64 SYMBOL F64] "/tmp/rayforce-strict.csv")
     (fn [e] (at e 'msg)))                                          ;; ".csv.read \"/tmp/rayforce-strict.csv\": line 3: expected 3 cells, got 2"
```

The reader memory-maps the input, splits it into chunks, parses chunks in parallel, then merges column types and symbol-intern tables. Scaling depends on parsing, dictionary work, memory pressure, and output I/O; see the [measured load comparisons](../guides/storage.md#full-clickbench-load-proof).

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
- `(.csv.splayed {compress: true} ...)` — any arrangement above may start with an options dict. `compress` true writes generation-2 (compressed) column files where eligible, false generation 0; absent inherits per file (generation 2 when the existing destination's current file is generation 2). `strict` true reads the source as [`.csv.read`'s strict mode](#csv-strict) does, the message headed `.csv.splayed "src.csv":`. Unknown keys, a non-bool value or a second options dict are a `domain` error. Linked and nested/list columns are always written uncompressed. A compressed string column persists its dictionary index, as a generation-0 one does, unless it would make the file larger than the generation-0 one (mostly distinct strings); see `.db.splayed.set` in [Storage](../storage/index.md).

Streams `src.csv` chunk-by-chunk through the parser into a splayed directory at `out_dir` (one column file per field, `.d` schema, and `.sym` symbol dictionary when needed). Returns the resulting splayed table loaded with `ray_read_splayed`: generation-0 columns are zero-copy mmap, so the bytes stay on disk; a compressed column decodes into heap memory the first time a query reads it.

Use this when the CSV is too large to fit in memory but you want to query it like an in-memory table.

```lisp
(write "/tmp/rayforce-huge.csv" "id,price,sym\n1,10.5,AAPL\n2,11.5,GOOG\n")
(set big (.csv.splayed [I64 F64 SYMBOL] "/tmp/rayforce-huge.csv" "/tmp/rayforce-huge"))
;; big is now a splayed table mmap'd from /tmp/rayforce-huge/.
```

Errors propagate from the CSV parser (`type`, `domain`, `limit`, and `parse` under `strict`) and from the splayed writer (`io`, `oom`).

## `.csv.parted` { #csv-parted }

Signatures (with optional rows-per-part):

- `(.csv.parted "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [types] "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [names] [types] "src.csv" "db_root" 'tbl_name)`
- `(.csv.parted [...]types "src.csv" rows_per_part "db_root" 'tbl_name)` — pass a positive integer just before the db-root to override the default partition size.
- `(.csv.parted {compress: true} ...)` — any arrangement above may start with an options dict. `compress` true writes generation-2 (compressed) files where eligible, false generation 0; absent inherits per file (generation 2 when the existing destination file is generation 2). `strict` true reads the source as [`.csv.read`'s strict mode](#csv-strict) does, the message headed `.csv.parted "src.csv":`; a bad line after the first partition was written leaves the staging directory as any failed import does, and the message ends `; partial output retained in "<dir>"`. Unknown keys, a non-bool value or a second options dict are a `domain` error. Linked and nested/list columns are always written uncompressed. A compressed string column persists its dictionary index, as a generation-0 one does, unless it would make the file larger than the generation-0 one (mostly distinct strings); see `.db.splayed.set` in [Storage](../storage/index.md).

Streams the CSV into `db_root/<partition-number>/tbl_name/`. Returns the parted table loaded back from disk. A new root is staged at `<db_root>.csv-partial` and published after the shared dictionary flush; staging from an import that failed after writing a partition is retained for diagnosis, and a retry into the same root fails with an `io` error naming the directory to remove. A failure before the first partition (an unreadable or empty source, a type vector that does not match) leaves nothing behind. Existing roots retain per-partition update behavior. Bulk writes require an external file and directory sync for durability.

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

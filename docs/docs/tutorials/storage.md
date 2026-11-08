# Data Loading & Storage Tutorial

Read CSV files, explore and filter data, serialize tables to binary, persist with splayed storage, and export results — all from the Rayfall REPL.

This tutorial assumes you have [built Rayforce](../getting-started/quick-start.md) and can start the REPL with `./rayforce`. We use `/tmp/rayforce-test/` as the working directory for all file operations.

## 1. Reading CSV Files

Create a CSV file on disk, then load it with `.csv.read`. The first row is treated as column headers, and types are inferred automatically:

```lisp
(set trades (.csv.read "/tmp/rayforce-test/trades.csv"))
trades
```

Assuming `trades.csv` contains:

```text
Symbol,Price,Qty
AAPL,150.5,100
GOOG,280.0,200
MSFT,420.0,50
AAPL,155.0,300
GOOG,275.5,150
```

Output:

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

String-like columns (e.g. ticker symbols, names) are loaded as `sym` (dictionary-encoded symbols). Numeric columns are inferred as `i64` or `f64` depending on whether any value has a decimal point.

You can also create tables in-memory and skip CSV entirely:

```lisp
(set trades (table [Symbol Price Qty]
  (list
    [AAPL GOOG MSFT AAPL GOOG]
    [150.5 280.0 420.0 155.0 275.5]
    [100 200 50 300 150])))
```

## 2. Exploring Data

Use `count`, `type`, and `show` to inspect a loaded table:

```lisp
(count trades)
```

```text
5
```

```lisp
(type trades)
```

```text
TABLE
```

`show` limits the display to the first *n* rows — useful for large datasets:

```lisp
(show trades 3)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## 3. Filtering Rows

Use `select` with a `where:` clause to filter rows. Find all trades where the price exceeds 200:

```lisp
(select {from:trades where: (> Price 200.0)})
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## 4. Grouping and Aggregation

Group by a column and compute aggregates with `by:`:

```lisp
(select {from:trades by: Symbol
         total_qty: (sum Qty)
         avg_price: (avg Price)})
```

```text
┌────────┬───────────┬────────────────┐
│ Symbol │ total_qty │   avg_price    │
│  SYM   │    I64    │      F64       │
├────────┼───────────┼────────────────┤
│ AAPL   │ 400       │ 152.75         │
│ GOOG   │ 350       │ 277.75         │
│ MSFT   │ 50        │ 420.0          │
├────────┴───────────┴────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Available aggregation functions: `sum`, `avg`, `min`, `max`, `count`, `first`, `last`, `med`.

## 5. Binary Serialization

Use `ser` to serialize any Rayforce object to a compact binary format (a U8 byte vector), and `de` to deserialize it back. This is useful for caching, IPC, or storing intermediate results:

```lisp
(set bytes (ser trades))
(count bytes)
```

```text
190
```

The entire table is encoded in 190 bytes. Deserialize with `de`:

```lisp
(set restored (de bytes))
restored
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

The deserialized table is identical to the original — all types, column names, and values are preserved.

`ser` bytes are a transport representation, not a canonical encoding for value
equality or content hashes. Equal values built different ways can serialize
differently, so do not use a digest of `ser` as a value identity check. For
example, a symbol atom's serialized flags include whether it was written as a
quoted literal: `(ser 'x)` and `(ser (as 'SYMBOL "x"))` differ at byte 17
(`0x20` versus `0x00`) although the two symbols are equal. A generic LIST
column loaded from splayed storage reconstructs its symbol atoms without that
flag, so the original and loaded values can compare equal while their `ser`
byte vectors differ.

To compare values, use `==` and reduce its result, because `==` is element-wise
and returns a collection of booleans rather than one boolean. For a nested
column such as a LIST of lists, reduce once per level of nesting. With `t` the
original table and `r` the table loaded back from a splayed directory, and a
`payload` column of `(list (list 1 'x) (list 2 'y))`:

```lisp
(== (at t 'payload) (at r 'payload))              ; [[true true] [true true]]
(all (map all (== (at t 'payload) (at r 'payload))))  ; true: one boolean
```

The second form returns `false` if any element differs. A flat column needs
only one level: `(all (== (at t 'seq) (at r 'seq)))`.

This form assumes both sides have the same shape and comparable types:

- Different lengths raise an error instead of returning `false`. A different
  inner length gives `length: binary op: operand lengths must match, got 2 and 3`,
  and a different row count gives `length: EQ: operand lengths must match, got 2 and 1`.
- Different types either raise or compare equal. A symbol against a string
  gives `type: =: incomparable operand types, got sym and str`, while `2`
  against `2.0` gives `true`, so the form does not tell integers from floats.

Check `count` and `type` first when the shapes may differ:

```lisp
(== (count [1 2]) (count [1 2 3]))   ; false: lengths differ, do not compare
(== (type [1 2]) (type [1.0 2.0]))   ; false: 'I64 versus 'F64
```

## 6. Splayed and Partitioned Tables

Splayed tables store each column as a separate file on disk. This is Rayforce's native columnar format — faster than CSV and preserving exact types. Three functions cover on-disk tables:

```lisp
(.db.splayed.set "/tmp/rayforce-tutorial-dir" trades)            ; write a splayed dir
(.db.splayed.set "/tmp/rayforce-tutorial-dir2" trades "/tmp/rayforce-tutorial.sym")  ; explicit shared symfile
(.db.splayed.get "/tmp/rayforce-tutorial-dir")                    ; open one splayed table
```

```text
(.db.parted.get "root" 'name)           ; open a partitioned table (root/.sym)
```

Save and load a table:

```lisp
(.db.splayed.set "/tmp/rayforce-test/trades_db" trades)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

On disk, this creates one file per column, the schema marker `.d`, and a symbol vocabulary file (plus its writer lock `sym.lk`):

```text
/tmp/rayforce-test/trades_db/
  .d    Symbol    Price    Qty    sym    sym.lk
```

`.d` holds the column names (a string vector — the schema is self-describing), and `sym` is the table's **vocabulary**: the distinct symbols its `sym` columns use, in the file's own enumeration order. On-disk `sym` cells are positions in that vocabulary — never process-global ids — so the file is valid in any session and any process. A table without symbol columns never requires (or writes) a symfile; symbols nested inside list columns are stored as strings and need no symfile either.

Load it back in a new session (or the same one) with `.db.splayed.get`:

```lisp
(set loaded (.db.splayed.get "/tmp/rayforce-test/trades_db"))
loaded
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.5 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

All types are preserved exactly — `f64` stays `f64`, `sym` stays `sym`. No type inference needed on reload.

### Symfile resolution

When no sym path is given, both `.set` and `.get` resolve the vocabulary by convention: `dir/.sym` for a standalone splayed dir; for a partition-shaped dir (`/db/2024.01.15/t/`, `/db/100/t/`) the parted **root**'s `.sym`. The symbol table is a dotfile (`.sym`), so it never collides with a user column — a table may carry an ordinary column named `sym`. Writing a partition therefore lands its symbols in the root symfile automatically, and `.db.parted.get` opens `root/.sym` once and attaches it across every partition — the parted view is index-coherent by construction. An explicit third argument to `.db.splayed.set` (or second to `.db.splayed.get`) always wins.

A read that finds no symfile for a table that *has* symbol columns fails loudly with `error: sym` — it never resolves against whatever symbols happen to be in memory.

### Sharing a vocabulary

Several tables may use one symfile — pass the same explicit sym path, or simply place them under one parted root. Tables that share a vocabulary compare and join their `sym` columns on raw indices (the same-file fast path); tables with different vocabularies still work together, paying a one-time translation of each vocabulary (proportional to its distinct symbols, never to row count).

Because a symfile only ever **appends** — a symbol's position is permanent, never removed or reordered — previously written column files stay valid as the vocabulary grows, and a re-opened reader is at worst momentarily behind, never wrong. The flip side is a single-writer contract: exactly one process may write to a given symfile (the writer holds `sym.lk` exclusively); readers are unlimited.

Column index width follows the table's own vocabulary (`u8` up to 255 symbols, then `u16`, `u32`, …) — not the process dictionary — so files stay as narrow as the data allows.

### Crash safety

Writes are ordered: the symfile is flushed first, then the columns, then `.d` as the commit marker; files from a previous wider schema are swept. A crash mid-write leaves a directory without `.d`, which reads as missing — never as corrupt data — and the next `.db.splayed.set` heals it.

## 7. Writing CSV

Export any table to CSV with `.csv.write`. The return value `0` indicates success:

```lisp
(set big (select {from:trades where: (> Price 200.0)}))
(.csv.write big "/tmp/rayforce-test/big_trades.csv")
```

```text
0
```

Verify by reading it back:

```lisp
(.csv.read "/tmp/rayforce-test/big_trades.csv")
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Note: float values written without a fractional part (e.g. `280.0`) may be read back as `i64` since the CSV reader infers the narrowest fitting type. Use splayed tables when exact type preservation matters.

## 8. Complete Example

End-to-end workflow: create data, save as CSV, reload, filter, serialize, deserialize, and verify:

```lisp
; 1. Create a table
(set trades (table [Symbol Price Qty]
  (list
    [AAPL GOOG MSFT AAPL GOOG]
    [150.5 280.0 420.0 155.0 275.5]
    [100 200 50 300 150])))

; 2. Save to CSV
(.csv.write trades "/tmp/rayforce-test/trades.csv")

; 3. Reload from CSV
(set loaded (.csv.read "/tmp/rayforce-test/trades.csv"))

; 4. Filter: only trades above 200
(set big (select {from:loaded where: (> Price 200.0)}))

; 5. Serialize to binary
(set bytes (ser big))

; 6. Deserialize and verify
(set restored (de bytes))
(count restored)

; 7. Save as splayed table for fast reload
(.db.splayed.set "/tmp/rayforce-test/big_db" big)

; 8. Load splayed table back
(.db.splayed.get "/tmp/rayforce-test/big_db")
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ GOOG   │ 275.5 │ 150                │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## Storage Format Comparison

| Format | Function | Preserves Types | Best For |
|---|---|---|---|
| CSV | `.csv.read` / `.csv.write` | No (re-inferred on load) | Interoperability, human-readable data |
| Binary | `ser` / `de` | Yes | Caching, IPC, embedding in messages |
| Splayed | `.db.splayed.set` / `.db.splayed.get` | Yes | Persistent storage, fast reload, mmap |
| Partitioned | `.db.splayed.set` per partition / `.db.parted.get` | Yes | Large datasets split by date or key |

## Next Steps

- [**Getting Started Tutorial**](../getting-started/tutorial.md) — Tables, filtering, joins, pivots, and sorting
- [**Data Persistence**](../guides/storage.md) — Partitioned storage and advanced file I/O
- [**Analytics Cookbook**](../guides/analytics.md) — Time-series, top-N, ASOF joins
- [**Functions Reference**](../language/functions.md) — Complete list of all built-in functions

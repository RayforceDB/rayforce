# Joins

Equi-joins, outer joins, anti-joins, as-of joins for time-series alignment, and window joins with aggregation. Join operations compile to the DAG and use parallel hash or sorted-merge execution paths.

## Inner Join

The `inner-join` function performs an equi-join on one or more key columns. Only rows with matching keys in both tables appear in the result.

### Signature

`(inner-join [keys] left right)`

Join two tables on shared key columns. Returns a table with all columns from both sides (key columns appear once).

### Examples

```lisp
(set orders (table [id sym qty]
    (list [1 2 3 4] [AAPL GOOG MSFT AAPL] [100 200 50 150])))

(set prices (table [sym price]
    (list [AAPL GOOG TSLA] [150 280 245])))

; Join on sym — MSFT and TSLA have no match, so they are excluded
(inner-join [sym] orders prices)
; id sym  qty price
; -- ---  --- -----
;  1 AAPL 100   150
;  2 GOOG 200   280
;  4 AAPL 150   150
```

Join on multiple keys:

```lisp
; Key columns may be strings, symbols, or numeric values.
(set x (table [a b c]
    (list (take [aa bb cc] 10)
          (take [I J K] 10)
          (til 10))))

(set y (table [a b d]
    (list (take [aa bb] 10)
          (take [I J K] 10)
          (til 10))))

; Join on both [a b]
(inner-join [a b] x y)
```

Every named key must exist in both input tables. A misspelled or missing key is
a `domain` error instead of an empty result. A string key must be paired with a
string key on the other side.

## Left Join

The `left-join` function keeps all rows from the left table. Unmatched rows have null in the right-side columns.

### Signature

`(left-join [keys] left right)`

Left outer join. All left rows are preserved; unmatched right columns are null.

### Examples

```lisp
(left-join [sym] orders prices)
; id sym  qty price
; -- ---  --- -----
;  1 AAPL 100   150
;  2 GOOG 200   280
;  3 MSFT  50   0Nl  ← null (no MSFT in prices)
;  4 AAPL 150   150
```

Left join on multiple keys:

```lisp
(left-join [a b] x y)
```

## Full Join

The `full-join` function keeps every row from both tables. Matched rows combine columns from both sides. Left-only and right-only rows fill the missing side's columns with nulls, and shared key columns appear once.

### Signature

`(full-join [keys] left right)`

Full outer join. All left and right rows are preserved; unmatched columns are null.

### Examples

```lisp
(set left (table [id qty] (list [1 2 3] [10 20 30])))
(set right (table [id px] (list [2 3 4] [200 300 400])))

(full-join [id] left right)
; id qty px
; -- --- ---
;  1  10 0Nl
;  2  20 200
;  3  30 300
;  4 0Nl 400
```

## Anti Join

The `anti-join` function keeps rows from the left table whose key does not appear in the right table. The result has only the left table's columns.

### Signature

`(anti-join [keys] left right)`

### Examples

```lisp
(set orders (table [id sym] (list [1 2 3] [AAPL GOOG MSFT])))
(set blocked (table [sym] (list [GOOG])))

(anti-join [sym] orders blocked)
; id sym
; -- ----
;  1 AAPL
;  3 MSFT
```

## As-of Join

The `asof-join` function is designed for time-series data. For each row in the left table, it finds the most recent matching row in the right table where the time column is less than or equal to the left's time value. The last column in the key list is the temporal column; any preceding keys are exact-match equality keys.

### Signature

`(asof-join [keys time-col] left right)`

As-of join for time-series alignment. The last key is the temporal column. For each left row, returns the most recent right row where `right.time <= left.time`.

Equality keys are **optional**:

- `(asof-join [time-col] left right)` — a lone time key runs an un-partitioned as-of join across all rows.
- `(asof-join [keys time-col] left right)` — preceding keys partition the join; within each partition the temporal match applies.

### Examples

```lisp
; Trades and quotes for time-series alignment
(set trades (table [Sym Ts Price]
    (list [AAPL AAPL MSFT]
          [10:00:01 10:00:05 10:00:04]
          [190.05 190.1 410.25])))

(set quotes (table [Sym Ts Bid Ask]
    (list [AAPL AAPL AAPL MSFT]
          [09:59:55 10:00:03 10:00:07 10:00:02]
          [189.9 190 190.05 410.1]
          [190.1 190.2 190.25 410.3])))

; As-of join: for each trade, find prevailing quote
(asof-join [Sym Ts] trades quotes)
; Sym  Ts       Price  Bid   Ask
; ---  --------  ----- ----- -----
; AAPL 10:00:01 190.05 189.9 190.1   ← quote from 09:59:55
; AAPL 10:00:05  190.1 190   190.2   ← quote from 10:00:03
; MSFT 10:00:04 410.25 410.1 410.3   ← quote from 10:00:02
```

### Pre-sorted fast-path

The as-of executor normally sorts both inputs by `(equality-keys, time)` on every call. [Column attributes](attributes.md) let it skip that sort when the data already satisfies the required ordering:

- **Un-partitioned** — if both input time columns carry the `sorted` attribute, both per-join sorts are skipped (O(n+m) instead of O(n log n)).
- **Partitioned** — if a single numeric equality key carries the `parted` attribute and its time column is non-descending within each part, that side's sort is skipped.

These are pure opt-in acceleration: when the attribute is absent the executor falls back to the usual sort-merge, so results are identical either way.

```lisp
; Stamp the time column as sorted, then run an un-partitioned as-of join.
(set Ls (table [Time Price] (list (.attr.set 'sorted [10:00:01.000 10:00:03.000]) [100.0 101.0])))
(.attr.get (at Ls 'Time))                                        ; ⇒ ['sorted]

; Partitioned: a parted numeric equality key (parted is numeric-only — not a symbol).
(set Lgp (table [ID Time Price]
    (list (.attr.set 'parted [1 1 2 2])
          [10:00:01.000 10:00:03.000 10:00:01.000 10:00:02.000]
          [1.0 2.0 3.0 4.0])))
(set Rgp (table [ID Time Bid]
    (list [1 1 2 2]
          [10:00:00.000 10:00:02.000 10:00:00.000 10:00:01.000]
          [1.1 2.2 3.3 4.4])))
(asof-join [ID Time] Lgp Rgp)
```

See [Column Attributes](attributes.md) for how to stamp and verify these properties.

### Large-scale Usage

```lisp
; 10 million row as-of join
(set n 10000000)
(set tsym (take (concat (take 'AAPL 99) (take 'MSFT 1)) n))
(set ttime (+ 09:00:00 (as 'TIME (/ (* (til n) 3) 10))))
(set trades (table [Sym Ts Price] (list tsym ttime (+ 10 (til n)))))
; ... build quotes similarly ...
(asof-join [Sym Ts] trades quotes)
```

### Null-key semantics

As-of-join treats **NULL keys as never matching**:

- A left row with a null in any equality key or in the time column is kept in the output (left-outer behaviour) but the right-side columns are explicitly null.
- A right row with a null in any equality key or time column is skipped entirely during the merge walk — it cannot become the best match for any left row.
- Right-side columns for unmatched rows are filled with type-correct null sentinels and marked with the null hint, distinguishable from genuine zero values via `(nil? x)`.

This means a null time in the left table is never silently treated as time zero, and two null sym keys on opposite sides do not merge into a spurious match.

## Window Join

The `window-join` function extends the as-of join with a time window. For each left row, it finds all right rows within a time interval and applies aggregation functions to the matched rows.

### Signature

`(window-join [keys time-col] intervals left right {col: (agg expr) ...})`

Window join with per-row time intervals. `intervals` is a list of two vectors `[lo hi]` defining the time window for each left row. Aggregation functions are applied to all right rows falling within each window.

### Examples

```lisp
(set trades (table [Sym Time Price]
    (list [x x x]
          [12:00:01 12:00:04 12:00:06]
          [89.17 70.5 80.54])))

(set quotes (table [Sym Time Size]
    (list [x x x x x x x x x x]
          [12:00:00 12:00:01 12:00:02 12:00:03 12:00:04
           12:00:05 12:00:06 12:00:07 12:00:08 12:00:09]
          [928 528 648 914 918 626 577 817 620 698])))

; Build the intervals as two vectors: a `lo` and a `hi` bound per left row.
; `intervals` is a two-element list `(list lo hi)`, each vector matching the
; left row count.
(set lo (+ (at trades 'Time) -1000))
(set hi (+ (at trades 'Time)  1000))

; Window join: min bid and max ask within each window
(window-join [Sym Time] (list lo hi) trades quotes
    {size_min: (min Size) size_max: (max Size)})
```

### Large-scale Window Join

```lisp
(set n 100000)
(set tsym (take [AAPL MSFT] n))
(set ttime (+ 09:00:00 (as 'TIME (/ (* (til n) 3) 10))))
(set bsym (take [AAPL MSFT GOOG] (* 2 n)))
(set btime (+ 09:00:00 (as 'TIME (/ (* (til (* 2 n)) 2) 10))))
(set bid (+ 8 (/ (til (* 2 n)) 2)))
(set ask (+ 12 (/ (til (* 2 n)) 2)))
(set trades (table [Sym Ts Price]
    (list tsym ttime (+ 10 (til n)))))
(set quotes (table [Sym Ts Bid Ask]
    (list bsym btime bid ask)))

; Build intervals from the trades timestamp
(set intervals (map-left + [-1000 1000] (at trades 'Ts)))

(window-join [Sym Ts] intervals trades quotes
    {bid: (min Bid) ask: (max Ask)})
```

## How Joins Compile to the DAG

All join operations compile to the Rayforce execution DAG. The optimizer and executor handle the details:

1. **DAG construction** — `inner-join`, `left-join`, and `full-join` emit `OP_JOIN` nodes with join type flags. `asof-join` emits `OP_ASOF_JOIN`. `window-join` emits `OP_WINDOW_JOIN`.
2. **Optimizer** — Predicate pushdown moves filters closer to data sources (past `SELECT`/`ALIAS`, `GROUP`, and `EXPAND` nodes); filters on join inputs are not currently pushed across join boundaries. Type inference propagates column types through join boundaries. SIP (Sideways Information Passing) can prune the build side using selection bitmaps.
3. **Execution** — Equi-joins use a radix-partitioned hash join: the build side is partitioned by hash, then each morsel from the probe side looks up matches in the corresponding partition. For inner joins on the radix-parallel path, the executor picks the build side at runtime using actual materialized row counts — whichever input has fewer rows becomes the build side, reducing hash-table memory and improving cache utilisation. This selection is most effective when the larger side has many rows per key (e.g. a fact table joining a small dimension); on near-unique keys the benefit is small. LEFT, FULL, and ANTI joins always build on the right because their semantics require preserving every left row. The small-input (chained) path also always builds on the right. During the per-partition build the executor monitors per-key duplicate counts; if any single key exceeds a threshold it abandons the radix attempt and re-runs the whole join through the chained hash table, which is O(n) regardless of duplication. This ensures that no join — INNER, LEFT, or FULL — degrades to quadratic cost when the build side contains a heavily-duplicated key. It complements build-side selection (which handles INNER joins by choosing the smaller side) by covering LEFT and FULL joins, which cannot swap sides, and any INNER case where the forced-build side happens to be skewed. Output row order for inner joins on the radix-parallel path is partition- and thread-dependent and is not guaranteed to be stable. As-of and window joins use sorted merge with binary search on the temporal column — the as-of executor skips the per-join sort when the inputs carry the `sorted` / `parted` [attributes](attributes.md) described above.

!!! note "Performance note"
    Join key columns must be symbol (`RAY_SYM`) or numeric. Symbol columns are dictionary-encoded integers and join fastest. String (`RAY_STR`) key columns are **not** supported — a join keyed on a string column raises `nyi`. Convert string keys to symbols (`(as 'sym col)`) before joining.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `inner-join` | `(inner-join [keys] left right)` | Equi-join; only matching rows |
| `left-join` | `(left-join [keys] left right)` | Left outer join; all left rows preserved |
| `full-join` | `(full-join [keys] left right)` | Full outer join; all rows from both sides preserved |
| `anti-join` | `(anti-join [keys] left right)` | Keep left rows with no right match |
| `asof-join` | `(asof-join [keys time-col] left right)` | Time-series alignment; most recent match |
| `window-join` | `(window-join [keys tc] intervals left right {aggs})` | Window aggregation over time intervals |

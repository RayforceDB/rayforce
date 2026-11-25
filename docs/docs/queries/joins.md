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

The order of the result's rows is not specified: sort it when you need one
(see [Row order](#row-order)).

## Left Join

The `left-join` function keeps all rows from the left table. Unmatched rows have null in the right-side columns.

When each left row produces exactly one output row in its original position,
the flat join executor retains ordinary left columns instead of copying them.
Slice-valued columns still use an independent gather because copying a slice
header does not detach its parent payload.
Subsequent updates use copy-on-write, preserving the source and result as
independent values. Duplicate matches still expand and gather the rows.

### Signature

`(left-join [keys] left right)`

Left outer join. All left rows are preserved; unmatched right columns are null.
When a left row matches several right rows, the order of its matches, and of
the result's rows, is not specified (see [Row order](#row-order)).

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
The order of the result's rows is not specified (see [Row order](#row-order)).

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

## Semi and Anti Masks

`semi-mask` and `anti-mask` answer the same question as `inner-join` and `anti-join`, but return a boolean vector over the left table's rows instead of a table. `semi-mask` is true where the row has a match in the right table on every key; `anti-mask` is its negation. No column is gathered, so the cost is the match alone: use a mask when all you want is a selection, for `where:` or `delete`.

### Signature

`(semi-mask [keys] left right)`
`(anti-mask [keys] left right)`

The keys may also come last, as with the joins: `(semi-mask left right [keys])`.

### Examples

```lisp
(set levels (table [venue instrument px] (list [V0 V1 V0 V1 V2 V0] [A B C B A A] [1 2 3 4 5 6])))
(set resets (table [venue instrument] (list [V0 V1 V9] [A B Q])))

(semi-mask [venue instrument] levels resets)  ; [true true false true false true]
(anti-mask [venue instrument] levels resets)  ; [false false true false true false]

(set M (semi-mask [venue instrument] levels resets))
(select {from: levels where: M})              ; the rows inner-join would emit, once each
(delete {from: 'levels where: M})             ; drop them in place
```

A compound key matches the tuple, not each column on its own: `(V0 C)` above is false although both `V0` and `C` occur somewhere in `resets`. A left row is true in `semi-mask` exactly when `inner-join` emits it (once, however many right rows it matches), and `(select {from: left where: (anti-mask ks left right)})` returns the rows of `(anti-join ks left right)` in the same order. Null keys match each other, as in every equi-join below.

## Null-key semantics (equi-joins)

`inner-join`, `left-join`, `full-join`, `anti-join`, `semi-mask` and `anti-mask` treat **a null key as equal to another null key**, and never equal to a non-null one:

```lisp
(set left  (table [k v] (list (as 'SYM ["a" ""]) [1 2])))
(set right (table [k w] (list (as 'SYM ["a" ""]) [10 20])))

(at (left-join [k] left right) 'w)  ; [10 20]  — the null key finds its match
(count (anti-join [k] left left))   ; 0        — X anti-joined with itself is empty
```

This is the rule the rest of the runtime already applies to key columns: `distinct` keeps exactly one null, group-by folds nulls into a single group, and `in` matches a null against a null. It differs from SQL, where `NULL = NULL` is unknown and a self-anti-join can return rows. The invariant that follows — `(anti-join [cols] X X)` is empty for every `X` and every key set — is what makes anti-join usable for "which of these tuples have I not seen yet".

As-of join is the exception — its null keys never match. See its own [Null-key semantics](#null-key-semantics) section below.

## Row order

Which rows a join returns is fixed; the order it returns them in is not, for
the equi-joins that can return several rows per left row.

- **`inner-join`, `left-join` and `full-join`: the order is not specified.**
  When a key matches several right rows, the order of those matches is not
  specified, and on the large-input (radix) path neither is the order of the
  left rows: it can differ between two runs of the same join in the same
  process, between core counts, and between the small-input and radix
  routes. Each run returns the same rows, the same multiset, and only their
  order changes. The order is left unspecified so the join can run on the
  fastest path; do not rely on any order you observe.
- **`anti-join`: left order.** The kept left rows come out in the left
  table's order, the same rows in the same order as
  `(select {from: left where: (anti-mask ks left right)})`.
- **`semi-mask` and `anti-mask`: one value per left row**, in left order.
- **`asof-join` and `window-join`: one row per left row, in left order.** An
  as-of join that finds several right rows with the same key and time takes
  the last of them in right order.

To get an order from an equi-join, sort its result with `xasc` (or `xdesc`)
by columns that tell every row apart. When no column does, number the right
rows before joining:

```lisp
(set orders (table [id sym qty] (list [1 2 3 4] [AAPL GOOG MSFT AAPL] [100 200 50 150])))
(set fills (table [sym px] (list [AAPL GOOG AAPL AAPL] [150 280 151 149])))

; each AAPL order matches three fills, in no specified order
(left-join [sym] orders fills)

; ordered by order id, then by the fill's position in fills
(set fills (update {rn: (til (count fills)) from: fills}))
(xasc (left-join [sym] orders fills) [id rn])
; id sym  qty px  rn
; -- ---- --- --- ---
;  1 AAPL 100 150   0
;  1 AAPL 100 151   2
;  1 AAPL 100 149   3
;  2 GOOG 200 280   1
;  3 MSFT  50 0Nl 0Nl
;  4 AAPL 150 150   0
;  4 AAPL 150 151   2
;  4 AAPL 150 149   3
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
  When `xasc` by the key and then the time column produced that side, it records the order and the check is skipped too. The record names the time column, so it holds for that table and for what `select`, `update`, `left-join`, `xcols` and `xcol` make of it while the key column and the column named by the record pass through unchanged: a projection or reorder of columns, an update of other columns, a rename of other columns. After a rename that moves the time column's name, once the time column is replaced (even by the same values), or when the key column is placed in a table built by hand, the order is checked again; a splayed save of such a table writes no record.

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
3. **Execution** — Equi-joins use a radix-partitioned hash join: the build side is partitioned by hash, then each morsel from the probe side looks up matches in the corresponding partition. For inner joins on the radix-parallel path, the executor picks the build side at runtime using actual materialized row counts — whichever input has fewer rows becomes the build side, reducing hash-table memory and improving cache utilisation. This selection is most effective when the larger side has many rows per key (e.g. a fact table joining a small dimension); on near-unique keys the benefit is small. LEFT, FULL, and ANTI joins always build on the right because their semantics require preserving every left row. The small-input (chained) path also always builds on the right. During the per-partition build the executor monitors per-key duplicate counts; if any single key exceeds a threshold it abandons the radix attempt and re-runs the whole join through the chained hash table, which is O(n) regardless of duplication. This ensures that no join — INNER, LEFT, or FULL — degrades to quadratic cost when the build side contains a heavily-duplicated key. It complements build-side selection (which handles INNER joins by choosing the smaller side) by covering LEFT and FULL joins, which cannot swap sides, and any INNER case where the forced-build side happens to be skewed. Output row order is partition- and thread-dependent on the radix-parallel path and differs from the chained path; see [Row order](#row-order) for what each join kind guarantees. On the small-input (chained) path, an inner or left join whose left side is more than twice the size of the right first builds a bitmap of the right side's values for its symbol and integer key columns (a column gets none when its right-side values span more than 2^24, when that span is more than 64 times the left row count, or when it pairs a symbol key with a non-symbol one). The executor tests a sample of 1024 left rows and keeps only the bitmaps that reject enough of them, up to four, most selective first. A left row whose value on a kept column appears nowhere on the right cannot match, so it is not hashed: an inner join drops it, and a left join emits it with null right-side columns. This works for any number of key columns. For `(inner-join [venue instrument] big small)` with a few books in `small`, the sample usually keeps `instrument` alone, since `venue` rejects nothing more after it. The cost becomes a bit test per left row plus hashing only for the rows whose instrument occurs in `small`. A join where most left rows match keeps no bitmap and pays almost nothing extra: only the bitmap build over the right side and the sample. Null keys are unaffected and still match each other. `semi-mask` and `anti-mask` run the same probe on the chained hash table built over the right side, with the same bitmap rule, at any right-side size; the probe is split across workers, and each left row writes its own byte of the mask instead of a row pair. When a key column of the left side carries an [`.idx.hash`](indexes.md#routing-table) index and the right side names few of its values, an inner join, `semi-mask` and `anti-mask` take the left rows from the index instead of passing over all of them: the index's rows for the values the right side holds on that column (rows an append added included), in row order, then the same hash, probe and whole-key equality, so the result and its row order are those of the scan. The index is used only when it is the cheaper path: when 32 × the right side's rows + r × the candidate rows stays below the left side's rows, r being 32 for an inner join and 8 for a mask (a mask probes each candidate once, a join twice), since each right row costs a lookup in the index and each candidate a gathered, checked row. It is decided in two steps. The first costs nothing: from sizes and the index's own row and group counts it declines a right side with a row for every 2048 left rows or more, and applies the rule to the candidates the index's mean rows per value predicts, so a join declined there looks up nothing. Only then are the chosen column's right values counted exactly from the index, before any row is read, stopping as soon as the count breaks the rule. A skewed index, whose mean underrates a heavy group, can be declined there, after at most one lookup per right key. Among several indexed key columns the one with the fewest predicted rows is used, so `(inner-join [venue instrument] big small)` with `instrument` indexed and a few books in `small` reads only those books' rows of `big`. A left, full or anti join keeps every left row and never uses it; nor does a float, string or guid key, a numeric left key column that may hold nulls, a symbol right key column holding the null, or an inner join whose right side is over 65536 rows (the radix path; the masks have no such cutoff). As-of and window joins use sorted merge with binary search on the temporal column — the as-of executor skips the per-join sort when the inputs carry the `sorted` / `parted` [attributes](attributes.md) described above.

!!! note "Performance note"
    Join key columns may be symbol (`RAY_SYM`), numeric or string (`RAY_STR`); a string key must be paired with a string key on the other side. Symbol columns are dictionary-encoded integers and join fastest. A string key is hashed and compared by its bytes and gets no bitmap prefilter, so converting it to a symbol (`(as 'sym col)`) pays off when the same column is joined repeatedly.

### Parallel probe floor

An inner, left or full join whose right side has at most 65536 rows takes the small-input (chained) path. It builds a hash table over the right side and probes it with the left side in morsels of 8192 rows. By default the probe goes to the thread pool once the left side has more than one morsel, from 8193 rows. Most other parallel operators start at 65536 rows.

That default favours latency. A small probe split across many workers finishes sooner but costs more CPU in total, mostly because the main thread spins while it waits for the slowest worker. For a service that is not latency-bound, [`.sys.join.par.min`](../namespaces/sys.md#sys-join-par-min) raises the floor so that smaller probes run on the calling thread:

```lisp
(.sys.join.par.min 65536)   ;; probes under 65536 left rows stay serial
```

Measured on joins below 65536 rows, a floor of 65536 costs about 2.6x the wall time for about 44% less CPU. The gain depends on how much work each probed row does. A join against a few keys, where the [prefilter](#how-joins-compile-to-the-dag) drops most left rows before they are hashed, saves about half its CPU and takes 1.2–1.9x as long, closer to 1.2x near 20000 left rows and up to 1.9x near 60000. A join where every left row matches saves 20–40% of its CPU and takes 1.3–2.8x as long. Joins above the floor are unchanged, and so are joins whose right side is over 65536 rows (the radix path), unless the radix join falls back to the chained path (many null or duplicate build keys). The result is the same at any floor, rows and row order included.

`-c` and this floor control different things. `-c` sets how many threads the pool has. The floor sets how large a join must be before it uses them. With a raised floor, the pool is still there for an occasional large job while the steady small joins run serially.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `inner-join` | `(inner-join [keys] left right)` | Inner join — only matching rows from both sides |
| `left-join` | `(left-join [keys] left right)` | Left join — all left rows, unmatched filled with null. Keys are a symbol list |
| `full-join` | `(full-join [keys] left right)` | Full outer join — all rows from both sides, unmatched columns filled with null |
| `anti-join` | `(anti-join [keys] left right)` | Anti-semi-join — left rows with no right match |
| `semi-mask` | `(semi-mask [keys] left right)` | Boolean vector over left rows — true where the row has a right match |
| `anti-mask` | `(anti-mask [keys] left right)` | Boolean vector over left rows — true where the row has no right match |
| `asof-join` | `(asof-join [keys time-col] left right)` | As-of join — match most recent preceding value. Keys come first, last key is the time key |
| `window-join` | `(window-join [keys tc] intervals left right {aggs})` | Window join — `[eq-keys... time-key]`, intervals, left, right, agg dict |

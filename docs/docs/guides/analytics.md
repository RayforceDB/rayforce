# Analytics Cookbook

## 1. Time-Series Aggregation { #time-series }

**Problem:** Given daily sales records, compute the total amount per day.

```lisp
(set sales (table [Date Product Amount]
  (list
    (+ 2024.01.01 [0 0 1 1 2 2])
    [Widget Gadget Widget Gadget Widget Gadget]
    [100 200 150 300 120 180])))
```

```text
┌────────────┬─────────┬──────────────┐
│    Date    │ Product │    Amount    │
│    DATE    │   SYM   │     I64      │
├────────────┼─────────┼──────────────┤
│ 2024.01.01 │ Widget  │ 100          │
│ 2024.01.01 │ Gadget  │ 200          │
│ 2024.01.02 │ Widget  │ 150          │
│ 2024.01.02 │ Gadget  │ 300          │
│ 2024.01.03 │ Widget  │ 120          │
│ 2024.01.03 │ Gadget  │ 180          │
├────────────┴─────────┴──────────────┤
│ 6 rows (6 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

```lisp
; Group by Date, sum Amount
(select {from:sales by: Date total: (sum Amount)})
```

```text
┌────────────┬────────────────────────┐
│    Date    │         total          │
│    DATE    │          I64           │
├────────────┼────────────────────────┤
│ 2024.01.01 │ 300                    │
│ 2024.01.02 │ 450                    │
│ 2024.01.03 │ 300                    │
├────────────┴────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

Date arithmetic uses the format `YYYY.MM.DD`. Adding an integer vector to a date produces a date vector offset by that many days.

## 2. Top-N Queries { #top-n }

**Problem:** Find the top 3 products by revenue.

```lisp
(set products (table [Name Revenue]
  (list [Alpha Beta Gamma Delta Epsilon Zeta Eta]
        [500 1200 300 800 950 150 700])))
```

```text
┌─────────┬───────────────────────────┐
│  Name   │          Revenue          │
│   SYM   │            I64            │
├─────────┼───────────────────────────┤
│ Alpha   │ 500                       │
│ Beta    │ 1200                      │
│ Gamma   │ 300                       │
│ Delta   │ 800                       │
│ Epsilon │ 950                       │
│ Zeta    │ 150                       │
│ Eta     │ 700                       │
├─────────┴───────────────────────────┤
│ 7 rows (7 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

```lisp
; Sort descending, then filter to keep top 3
(set sorted (xdesc products 'Revenue))
(select {from:sorted where: (> Revenue 700)})
```

```text
┌─────────┬───────────────────────────┐
│  Name   │          Revenue          │
│   SYM   │            I64            │
├─────────┼───────────────────────────┤
│ Beta    │ 1200                      │
│ Epsilon │ 950                       │
│ Delta   │ 800                       │
├─────────┴───────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

The pattern is: sort descending with `xdesc`, then filter with a threshold. For dynamic top-N where you don't know the cutoff value in advance, sort and filter by rank.

## 3. Running Totals { #running-totals }

**Problem:** Compute a cumulative sum over a vector of values.

```lisp
; sums is the DAG-backed running sum
(sums [3 1 4 1 5 9 2 6])
```

```text
[3 4 8 9 14 23 25 31]
```

```lisp
; Running product
(prds [1 2 3 4 5])
```

```text
[1 2 6 24 120]
```

```lisp
; Fixed trailing window: current row plus the previous two rows
(msum 3 [3 1 4 1 5 9])
(mavg 3 [3 1 4 1 5 9])
(mdev 3 [3 1 4 1 5 9])
```

```text
[3 4 8 6 10 15]
[3.0 2.0 2.6666666667 2.0 3.3333333333 5.0]
[0.0 1.0 1.2472191289 1.4142135624 1.6996731712 3.2659863237]
```

Use the specialized running verbs (`sums`, `prds`, `avgs`, `mins`, `maxs`) and moving-window verbs (`msum`, `mavg`, `mmin`, `mmax`, `mcount`, `mvar`, `mdev`) for common analytics paths. Constant-window calls lower to DAG vector kernels, run with morsel-based execution, and poll cancellation during execution. The generic `scan` function is still available when the accumulator is a custom function. The related `fold` function returns only the final accumulated value.

## 4. Pivoting (Cross-Tabulation) { #pivot }

**Problem:** Cross-tabulate sales by region and product.

```lisp
(set data (table [Region Product Sales]
  (list [East East West West East West]
        [Widget Gadget Widget Gadget Widget Gadget]
        [100 200 150 300 120 180])))
```

```text
┌────────┬─────────┬──────────────────┐
│ Region │ Product │      Sales       │
│  SYM   │   SYM   │       I64        │
├────────┼─────────┼──────────────────┤
│ East   │ Widget  │ 100              │
│ East   │ Gadget  │ 200              │
│ West   │ Widget  │ 150              │
│ West   │ Gadget  │ 300              │
│ East   │ Widget  │ 120              │
│ West   │ Gadget  │ 180              │
├────────┴─────────┴──────────────────┤
│ 6 rows (6 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

```lisp
; Pivot: rows=Region, cols=Product, values=Sales, agg=sum
(pivot data 'Region 'Product 'Sales sum)
```

```text
┌────────┬────────┬───────────────────┐
│ Region │ Widget │      Gadget       │
│  SYM   │  I64   │        I64        │
├────────┼────────┼───────────────────┤
│ East   │ 220    │ 200               │
│ West   │ 150    │ 480               │
├────────┴────────┴───────────────────┤
│ 2 rows (2 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

The distinct values of the column key (`Product`) become the new column headers. Use any aggregation function: `sum`, `avg`, `count`, `min`, `max`, `first`, `last`, `med`, or a custom lambda like `(fn [x] (sum (* x x)))`.

## 5. ASOF Joins { #asof-join }

**Problem:** Match each trade to the most recent quote at or before the trade time.

```lisp
(set trades (table [Sym Ts Price]
  (list [AAPL AAPL GOOG]
        [09:30:00 09:31:00 09:30:30]
        [150 151 280])))

(set quotes (table [Sym Ts Bid Ask]
  (list [AAPL AAPL AAPL GOOG GOOG GOOG]
        [09:29:50 09:30:10 09:31:05 09:30:00 09:30:20 09:31:00]
        [149 150 151 279 280 281]
        [151 152 153 281 282 283])))
```

```lisp
; ASOF join: match on Sym, find latest Ts <= trade Ts
(asof-join [Sym Ts] trades quotes)
```

```text
┌──────┬──────────────┬───────┬─────┬─────┐
│ Sym  │      Ts      │ Price │ Bid │ Ask │
│ SYM  │     TIME     │  I64  │ I64 │ I64 │
├──────┼──────────────┼───────┼─────┼─────┤
│ AAPL │ 09:30:00.000 │ 150   │ 149 │ 151 │
│ AAPL │ 09:31:00.000 │ 151   │ 150 │ 152 │
│ GOOG │ 09:30:30.000 │ 280   │ 280 │ 282 │
├──────┴──────────────┴───────┴─────┴─────┤
│ 3 rows (3 shown) 5 columns (5 shown)    │
└─────────────────────────────────────────┘
```

The key list `[Sym Ts]` specifies equality columns followed by the time column (last). For each left row, the join finds the right row with matching `Sym` and the largest `Ts` that does not exceed the left `Ts`.

## 6. Window Joins { #window-join }

**Problem:** For each trade, aggregate quote data within a time window around the trade time.

```lisp
(set trades (table [Sym Time Price]
  (list [AAPL AAPL AAPL]
        [12:00:01 12:00:04 12:00:06]
        [89.17 70.5 80.54])))

(set quotes (table [Sym Time Size]
  (list [AAPL AAPL AAPL AAPL AAPL AAPL AAPL AAPL AAPL AAPL]
        [12:00:00 12:00:01 12:00:02 12:00:03 12:00:04 12:00:05 12:00:06 12:00:07 12:00:08 12:00:09]
        [928 528 648 914 918 626 577 817 620 698])))

; Simple window join: match on [Sym], join by Time
(window-join trades quotes [Sym] 'Time)
```

```text
┌──────┬──────────────┬───────┬───────┐
│ Sym  │     Time     │ Price │ Size  │
│ SYM  │     TIME     │  F64  │  I64  │
├──────┼──────────────┼───────┼───────┤
│ AAPL │ 12:00:01.000 │ 89.17 │ 528   │
│ AAPL │ 12:00:04.000 │ 70.5  │ 918   │
│ AAPL │ 12:00:06.000 │ 80.54 │ 577   │
├──────┴──────────────┴───────┴───────┤
│ 3 rows (3 shown) 4 columns (4 shown)│
└─────────────────────────────────────┘
```

The `window-join` function matches rows from the right table within a time window around each left row. The equality keys (`[Sym]`) filter candidates, and the time column (`'Time`) determines the window range.

## 7. Working with CSV { #csv-workflow }

**Problem:** Load a CSV, transform the data, and save the result.

```lisp
; Create sample data
(set data (table [Name Score Grade]
  (list [Alice Bob Charlie]
        [95 87 92]
        [A B A])))

; Save to CSV
(.csv.write data "/tmp/grades.csv")

; Load it back
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

A typical workflow: `.csv.read` to load, `select` to filter and transform, `.csv.write` to save results. The CSV reader uses parallel parsing and automatic type inference for fast loading of large files.

## 8. Complex Aggregation with `fold` { #fold-aggregation }

**Problem:** Compute a weighted average where each value has a different weight.

```lisp
; Weighted average: sum(w*p) / sum(w)
(set w [2 4 4])
(set p [10.0 20.0 30.0])
(/ (fold + 0.0 (* w p)) (fold + 0.0 w))
```

```text
22.0
```

The `fold` function takes a binary function, an initial accumulator, and a vector. It reduces the vector to a single value by applying the function left-to-right. Unlike `scan` which returns all intermediate results, `fold` returns only the final result.

```lisp
; Sum of squares using fold with a custom function
(fold (fn [a b] (+ a (* b b))) 0 [3 4 5])
```

```text
50
```

Here the lambda squares each element and adds it to the running total: 9 + 16 + 25 = 50. Any binary function works with `fold`, including builtins like `+`, `*`, `min`, `max`.

## 9. Null Handling in Aggregations { #null-handling }

**Problem:** What happens when your data contains null values?

```lisp
; Create a vector with null entries (0Nl = null i64)
(set v [10 0Nl 30 0Nl 50])
(sum v)    ; 90  — nulls skipped
(avg v)    ; 30.0 — average of non-null values (90 / 3)
(min v)    ; 10
(max v)    ; 50
(count v)  ; 5 — total length including nulls
```

All built-in aggregation functions (`sum`, `avg`, `min`, `max`, `med`) skip null values automatically. The `count` function returns the total vector length including nulls.

Use `nil?` to check for null values:

```lisp
(nil? 0Nl)            ; true
(nil? 42)             ; false
(map nil? (list 10 0Nl 30))  ; (false true false)
```

Null literals are type-suffixed: `0Nl` (i64), `0Nf` (f64), `0Nd` (date), `0Nt` (time), `0Np` (timestamp).

## 10. Performance Tips { #performance-tips }

!!! note "Tips for fast analytics queries"

    - **Prefer `select` with `by:`** over manual group-then-map. The `select` pipeline fuses grouping and aggregation into a single pass, avoiding intermediate materialization.
    - **Use `xbar` for time bucketing** to bin timestamps into fixed intervals (e.g., 5-minute bars). This avoids materializing group keys and is faster than building date groups by hand.
    - **Use `pmap` for embarrassingly parallel work.** It distributes a function across list elements using worker threads. Ideal for independent per-group transformations:

        ```lisp
        (pmap (fn [x] (* x x)) (list 1 2 3 4 5))
        ```

        ```text
        (1 4 9 16 25)
        ```

## Next Steps

- [**Data Persistence**](storage.md) — Columnar files, splayed tables, partitioned storage
- [**Joins Reference**](../queries/joins.md) — Detailed join semantics and options
- [**Pivot & Window Reference**](../queries/pivot.md) — All pivot options and window functions
- [**Math & Aggregation**](../language/math.md) — Complete list of math and aggregation functions

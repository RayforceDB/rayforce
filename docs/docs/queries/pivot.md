# Pivot Tables & Window Functions

Reshape data with pivot tables for wide-format aggregation, and compute rolling or windowed calculations over ordered data.

## Pivot

The `pivot` function reshapes long-format data into wide format. It groups by an index column, spreads a pivot column's distinct values into new columns, and fills each cell with an aggregated value.

### Signature

`(pivot table index pivot_col value_col agg_fn)`

Pivot a table to wide format.

- **table** — source table
- **index** — row index column (symbol or vector of symbols for multi-index)
- **pivot_col** — column whose distinct values become new column headers
- **value_col** — column to aggregate
- **agg_fn** — aggregation function (`sum`, `avg`, `count`, `min`, `max`, `first`, `last`, `med`, or a custom lambda)

### Basic Pivot

```lisp
(set trades (table [Symbol Side Price Quantity]
    (list [AAPL AAPL GOOG GOOG AAPL GOOG]
          [Buy Sell Buy Sell Buy Buy]
          [150 152 280 282 149 278]
          [100 200 50 75 300 60])))

; Total Quantity by Symbol and Side
(pivot trades 'Symbol 'Side 'Quantity sum)
; Symbol  Buy  Sell
; ------  ---  ----
; AAPL    400   200
; GOOG    110    75
```

### Different Aggregations

```lisp
; Average Price by Symbol and Side
(pivot trades 'Symbol 'Side 'Price avg)
; Symbol   Buy   Sell
; ------  -----  -----
; AAPL    149.5  152.0
; GOOG    279.0  282.0

; Trade Count by Symbol and Side
(pivot trades 'Symbol 'Side 'Quantity count)
; Symbol  Buy  Sell
; ------  ---  ----
; AAPL      2     1
; GOOG      2     1

; Min/Max Price
(pivot trades 'Symbol 'Side 'Price min)
(pivot trades 'Symbol 'Side 'Price max)

; First/Last Price
(pivot trades 'Symbol 'Side 'Price first)
(pivot trades 'Symbol 'Side 'Price last)

; Median Price
(pivot trades 'Symbol 'Side 'Price med)
```

### Custom Aggregation

Pass a lambda for custom aggregation logic:

```lisp
; Sum of squares
(pivot trades 'Symbol 'Side 'Quantity (fn [x] (sum (* x x))))
```

### Multi-index Pivot

Pass a vector of column symbols as the index to create a multi-level row grouping:

```lisp
(set n 12)
(set trades (table [Date Symbol Sector Side Price Quantity]
    (list
        (+ 2024.01.01 (take (til 30) n))
        (take [AAPL GOOG MSFT AMZN] n)
        (take [Tech Tech Tech Retail] n)
        (take [Buy Sell Short] n)
        (+ 150 (til n))
        (* 100 (+ 1 (til n))))))

; Multi-index: Quantity by [Date Symbol] and Side
(pivot trades [Date Symbol] 'Side 'Quantity sum)

; Multi-index: Avg Price by [Sector Symbol] and Side
(pivot trades [Sector Symbol] 'Side 'Price avg)
```

## OP_PIVOT DAG Implementation

Pivot compiles to a single `OP_PIVOT` node in the execution DAG. The implementation uses a single-pass parallel hash aggregation:

1. **Phase 1: Discover pivot values** — Scan the pivot column to collect its distinct values. These become the output column names.
2. **Phase 2: Hash aggregation** — For each morsel, hash the (index, pivot_value) composite key and route the value to the corresponding accumulator. Each output column has its own aggregation state array.
3. **Phase 3: Materialize** — Build the result table with the index column(s) as rows and one column per distinct pivot value, filled with the aggregated results.

!!! note "Single-pass design"
    Unlike the naive approach of grouping then reshaping, `OP_PIVOT` performs aggregation and reshaping in one pass. This avoids materializing the grouped intermediate table and reduces memory pressure for large datasets.

### Supported Aggregations

| Function | Description |
|---|---|
| `sum` | Sum of values |
| `avg` | Arithmetic mean |
| `count` | Number of values |
| `min` | Minimum value |
| `max` | Maximum value |
| `first` | First value in group |
| `last` | Last value in group |
| `med` | Median value |
| `dev` | Standard deviation |
| `(fn [x] ...)` | Custom lambda aggregation |

## Window Functions

Window functions compute values across a set of related rows without collapsing the result. In Rayfall, windowed computations are expressed through `select` with rolling or cumulative expressions.

### Rolling Computations

Running computations like cumulative sums can be computed using the DAG-backed time-series vector helpers directly on column vectors:

```lisp
(set trades (table [Sym Time Price]
    (list [x x x]
          [12:00:01 12:00:04 12:00:06]
          [89.17 70.5 80.54])))

; Cumulative sum on the Price column
(sums (at trades 'Price))
; [89.17 159.67 240.21]

; Three-row trailing sum inside a projection
(select {from: trades
         px3: (msum 3 Price)
         avg3: (mavg 3 Price)})
```

Use `scan` when the accumulator is a custom function. Use `sums`, `avgs`, `mins`, `maxs`, and `prds` for built-in running aggregates, and `msum`, `mavg`, `mmin`, `mmax`, `mcount`, `mvar`, and `mdev` for fixed trailing windows. Constant-window calls are lazy-aware DAG operations and materialize with morsel-based kernels.

When the calculation must restart for each key, use the partitioned `window`
form. A positive `frame:` value is the trailing row count, including the
current row:

```lisp
(window {from: trades
         part: [Sym]
         order: [Time]
         frame: 3
         funcs: {avg3: (avg Price)}})
```

Use `frame: 'running` for an unbounded cumulative frame and omit `frame:` (or
use `frame: 'whole`) to aggregate over the whole partition.

### Rank and Order

```lisp
; Rank prices — double iasc gives dense rank
(set prices [89.17 70.5 80.54])
(iasc (iasc prices))
; [2 0 1]
```

### Grouped Aggregation

Use `by:` with aggregate expressions for per-group computations:

```lisp
(select {from: trades
         by: Sym
         total_price: (sum Price)
         min_price: (min Price)
         max_price: (max Price)})
; Sym  total_price min_price max_price
; ---  ----------- --------- ---------
; x         240.21      70.5     89.17
```

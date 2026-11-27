# Getting Started Tutorial

A hands-on walkthrough: build tables, query data, join, pivot, and work with CSV files — all from the Rayfall REPL.

This tutorial assumes you have already [built Rayforce](quick-start.md) and can start the REPL with `./rayforce`. Every example below shows the exact input and output you will see.

## 1. Your First Table

Create a table of stock trades with three columns: `Symbol` (sym), `Price` (float), and `Qty` (integer). Column names are given as a vector of symbols, and column data as a `list` of vectors:

```lisp
; Create a trades table
(set trades (table [Symbol Price Qty]
  (list
    [AAPL GOOG MSFT AAPL GOOG MSFT]
    [150.0 280.0 420.0 155.0 275.0 415.0]
    [100 200 50 300 150 75])))
```

Output:

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.0 │ 100                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 420.0 │ 50                 │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.0 │ 150                │
│ MSFT   │ 415.0 │ 75                 │
├────────┴───────┴────────────────────┤
│ 6 rows (6 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Each column has an inferred type shown on the second header row: `sym` for symbols, `f64` for 64-bit floats, and `i64` for 64-bit integers.

## 2. Filtering Rows

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
│ GOOG   │ 275.0 │ 150                │
│ MSFT   │ 415.0 │ 75                 │
├────────┴───────┴────────────────────┤
│ 4 rows (4 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

The `where:` expression is evaluated element-wise across the `Price` column, producing a boolean mask that selects matching rows.

## 3. Grouping and Aggregation

Group by `Symbol` and compute aggregates. The `by:` clause names the grouping column; additional key-value pairs define computed columns:

```lisp
(select {from:trades by: Symbol
         total: (sum Qty)
         avg_price: (avg Price)})
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ total │     avg_price      │
│  SYM   │  I64  │        F64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 400   │ 152.5              │
│ GOOG   │ 350   │ 277.5              │
│ MSFT   │ 125   │ 417.5              │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Available aggregation functions include `sum`, `avg`, `min`, `max`, `count`, `first`, `last`, and `med` (the median).

## 4. Sorting

Sort a table by one or more columns using `xasc` (ascending) or `xdesc` (descending):

```lisp
; Sort by Price, lowest first
(xasc trades 'Price)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150.0 │ 100                │
│ AAPL   │ 155.0 │ 300                │
│ GOOG   │ 275.0 │ 150                │
│ GOOG   │ 280.0 │ 200                │
│ MSFT   │ 415.0 │ 75                 │
│ MSFT   │ 420.0 │ 50                 │
├────────┴───────┴────────────────────┤
│ 6 rows (6 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

```lisp
; Sort by Price, highest first
(xdesc trades 'Price)
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  F64  │        I64         │
├────────┼───────┼────────────────────┤
│ MSFT   │ 420.0 │ 50                 │
│ MSFT   │ 415.0 │ 75                 │
│ GOOG   │ 280.0 │ 200                │
│ GOOG   │ 275.0 │ 150                │
│ AAPL   │ 155.0 │ 300                │
│ AAPL   │ 150.0 │ 100                │
├────────┴───────┴────────────────────┤
│ 6 rows (6 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

For sorting plain vectors (not tables), use `asc` and `desc`. To get the sort permutation indices without reordering, use `iasc` and `idesc`.

## 5. Joining Tables

Create two tables and join them on a shared key column:

```lisp
; Orders table
(set orders (table [Id Symbol Qty]
  (list [1 2 3 4]
        [AAPL GOOG AAPL MSFT]
        [100 200 150 50])))

; Price reference table
(set prices (table [Symbol Price]
  (list [AAPL GOOG MSFT]
        [150.0 280.0 420.0])))

; Inner join on Symbol
(inner-join [Symbol] orders prices)
```

```text
┌─────┬────────┬─────┬────────────────┐
│ Id  │ Symbol │ Qty │     Price      │
│ I64 │  SYM   │ I64 │      F64       │
├─────┼────────┼─────┼────────────────┤
│ 1   │ AAPL   │ 100 │ 150.0          │
│ 2   │ GOOG   │ 200 │ 280.0          │
│ 3   │ AAPL   │ 150 │ 150.0          │
│ 4   │ MSFT   │ 50  │ 420.0          │
├─────┴────────┴─────┴────────────────┤
│ 4 rows (4 shown) 4 columns (4 shown)│
└─────────────────────────────────────┘
```

Rayforce also supports `left-join` (keeps all left rows, fills missing right columns with defaults) and `asof-join` (matches the nearest key — covered in the [Analytics Cookbook](../guides/analytics.md#asof-join)).

## 6. Pivoting

Cross-tabulate data with `pivot`. The arguments are: table, row key, column key, value column, and aggregation function:

```lisp
; Trades with a Side column
(set trades (table [Symbol Side Qty]
  (list [AAPL AAPL GOOG GOOG MSFT MSFT]
        [Buy Sell Buy Sell Buy Sell]
        [100 50 200 150 75 25])))

; Total Qty by Symbol and Side
(pivot trades 'Symbol 'Side 'Qty sum)
```

```text
┌────────┬─────┬──────────────────────┐
│ Symbol │ Buy │         Sell         │
│  SYM   │ I64 │         I64          │
├────────┼─────┼──────────────────────┤
│ AAPL   │ 100 │ 50                   │
│ GOOG   │ 200 │ 150                  │
│ MSFT   │ 75  │ 25                   │
├────────┴─────┴──────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

The distinct values of `Side` become column headers. You can use any aggregation function: `sum`, `avg`, `count`, `min`, `max`, `first`, `last`, `med`, or even a custom lambda.

## 7. Loading CSV Files

Load a CSV file with automatic type inference and parallel parsing:

```lisp
(write "/tmp/rayforce-tutorial-trades.csv"
  "Symbol,Price,Qty\nAAPL,150.0,100\nGOOG,280.0,200\nMSFT,420.0,50\n")
(set data (.csv.read "/tmp/rayforce-tutorial-trades.csv"))
```

The first row is treated as column headers. Types are inferred from the data: integers, floats, dates, times, timestamps, and symbols are all detected automatically.

## 8. Saving Results to CSV

Write any table to a CSV file with `.csv.write`:

```lisp
(set data (table [Symbol Price Qty]
  (list [AAPL GOOG MSFT]
        [150.0 280.0 420.0]
        [100 200 50])))

; Save to CSV
(.csv.write data "/tmp/trades.csv")

; Load it back to verify
(.csv.read "/tmp/trades.csv")
```

```text
┌────────┬───────┬────────────────────┐
│ Symbol │ Price │        Qty         │
│  SYM   │  I64  │        I64         │
├────────┼───────┼────────────────────┤
│ AAPL   │ 150   │ 100                │
│ GOOG   │ 280   │ 200                │
│ MSFT   │ 420   │ 50                 │
├────────┴───────┴────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

Note that `f64` values written to CSV are read back as `i64` when they have no fractional part (150.0 becomes 150). This is expected — the CSV reader infers the narrowest type that fits.

## Next Steps

- [**Analytics Cookbook**](../guides/analytics.md) — Common analytics patterns: time-series, top-N, pivots, ASOF joins
- [**Building a Knowledge Base**](../guides/datalog.md) — Datalog rules, recursive queries, and entity-attribute-value storage
- [**Data Persistence**](../guides/storage.md) — CSV, columnar files, splayed tables, and partitioned storage
- [**Functions Reference**](../language/functions.md) — Complete list of all built-in functions

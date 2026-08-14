# Advanced Analytics Tutorial

Time-bucketed aggregation, running totals, top-N queries, pivoting, ASOF joins, and multi-step pipelines — all from the Rayfall REPL.

This tutorial builds on the [Getting Started Tutorial](../getting-started/tutorial.md). You should already be comfortable creating tables, filtering, grouping, and sorting. Start the REPL with `./rayforce` and follow along.

We will reuse a single trade dataset throughout. Create it now:

```lisp
(set trades (table [Time Symbol Price Qty]
  (list
    [1 2 3 4 5 6 7 8 9 10 11 12]
    [AAPL GOOG MSFT AAPL GOOG MSFT AAPL GOOG MSFT AAPL GOOG MSFT]
    [150.0 280.0 420.0 155.0 275.0 415.0 160.0 285.0 410.0 165.0 290.0 425.0]
    [100 200 50 300 150 75 120 180 60 250 160 80])))
```

```text
┌──────┬────────┬───────┬───────────────┐
│ Time │ Symbol │ Price │      Qty      │
│ I64  │  SYM   │  F64  │      I64      │
├──────┼────────┼───────┼───────────────┤
│ 1    │ AAPL   │ 150.0 │ 100           │
│ 2    │ GOOG   │ 280.0 │ 200           │
│ 3    │ MSFT   │ 420.0 │ 50            │
│ ...  │ ...    │ ...   │ ...           │
│ 12   │ MSFT   │ 425.0 │ 80            │
├──────┴────────┴───────┴───────────────┤
│ 12 rows                                  │
└─────────────────────────────────────────┘
```

## 1. Time-Bucketed Aggregation

Use `xbar` inside a `select` `by:` clause to bucket a numeric column into fixed-width intervals. `(xbar Time 4)` rounds each `Time` value down to the nearest multiple of 4, creating time buckets.

Build 4-unit OHLCV bars (open, high, low, close, volume) from the trade data:

```lisp
(select {from: trades
         by: (xbar Time 4)
         open:  (first Price)
         high:  (max Price)
         low:   (min Price)
         close: (last Price)
         vol:   (sum Qty)})
```

```text
┌─────┬───────┬───────┬───────┬───────┬─────┐
│     │ open  │ high  │  low  │ close │ vol │
│ I64 │  F64  │  F64  │  F64  │  F64  │ I64 │
├─────┼───────┼───────┼───────┼───────┼─────┤
│ 0   │ 150.0 │ 420.0 │ 150.0 │ 420.0 │ 350 │
│ 4   │ 155.0 │ 415.0 │ 155.0 │ 160.0 │ 645 │
│ 8   │ 285.0 │ 410.0 │ 165.0 │ 290.0 │ 650 │
│ 12  │ 425.0 │ 425.0 │ 425.0 │ 425.0 │ 80  │
└─────┴───────┴───────┴───────┴───────┴─────┘
```

The bucket column is left unnamed (blank header) because `by:` was given an unnamed computed expression. Give it a name with a dict — `by: {bucket: (xbar Time 4)}` — to label the column. Each row represents one time bucket with its OHLCV values; change the bucket width by adjusting the second argument to `xbar`.

## 2. Running Totals

Use `scan` to compute a cumulative reduction over a vector. `(scan f v)` applies `f` pairwise across `v`, carrying the accumulator forward.

```lisp
; Cumulative sum of quantities
(scan + [10 20 30 40 50])
```

```text
[10 30 60 100 150]
```

Use it to track running volume from a list of order quantities:

```lisp
(set quantities [100 200 50 300 150])

; Running total: 100, 300, 350, 650, 800
(scan + quantities)
```

```text
[100 300 350 650 800]
```

`scan` works with any binary function: use `*` for running products, or a custom lambda for running maximums:

```lisp
; Running maximum via lambda
(scan (fn [a b] (if (> a b) a b)) [3 1 4 1 5 9 2])
```

```text
[3 3 4 4 5 9 9]
```

## 3. Top-N Queries

Combine `xdesc` (sort descending) with `take` to extract the top N rows. First, aggregate by product, then rank:

```lisp
(set products (table [Product Revenue]
  (list [A B C D E A B C]
        [500 300 800 200 600 400 350 750])))

; Sort by Revenue descending, take top 3
(take (xdesc products 'Revenue) 3)
```

```text
┌─────────┬───────────────────────────┐
│ Product │          Revenue          │
│   SYM   │            I64            │
├─────────┼───────────────────────────┤
│ C       │ 800                       │
│ C       │ 750                       │
│ E       │ 600                       │
└─────────┴───────────────────────────┘
```

For top-N by group, first aggregate with `select ... by:`, then sort and take:

```lisp
; Top 2 symbols by total quantity from the trades table
(set by_sym (select {from: trades by: Symbol
                     total: (sum Qty)}))
(take (xdesc by_sym 'total) 2)
```

```text
┌────────┬────────────────────────────┐
│ Symbol │           total            │
│  SYM   │            I64             │
├────────┼────────────────────────────┤
│ AAPL   │ 770                        │
│ GOOG   │ 690                        │
└────────┴────────────────────────────┘
```

## 4. Pivoting

Use `pivot` to cross-tabulate data. Arguments: table, row key, column key, value column, aggregation function.

```lisp
(set sales (table [Product Region Revenue]
  (list [A A B B A B]
        [East West East West East West]
        [100 200 150 250 300 175])))

(pivot sales 'Product 'Region 'Revenue sum)
```

```text
┌─────────┬──────┬────────────────────┐
│ Product │ East │        West        │
│   SYM   │ I64  │        I64         │
├─────────┼──────┼────────────────────┤
│ A       │ 400  │ 200                │
│ B       │ 150  │ 425                │
└─────────┴──────┴────────────────────┘
```

The distinct values of `Region` become column headers. You can use any aggregation function: `sum`, `avg`, `count`, `min`, `max`, `first`, `last`, or `med`.

## 5. ASOF Joins

An ASOF join matches each left row to the most recent right row where the time key is less than or equal to the left's time, within the same equality partition. This is the standard way to do point-in-time lookups.

The syntax is `(asof-join [eqKey... timeKey] leftTable rightTable)`. The last element of the key vector is the time column; all preceding elements are equality columns.

```lisp
(set trades (table [Time Symbol Price Qty]
  (list [10 15 20 25 30]
        [AAPL GOOG AAPL GOOG AAPL]
        [150.0 280.0 151.0 279.0 152.0]
        [100 200 150 300 120])))

(set quotes (table [Time Symbol Bid Ask]
  (list [8 12 14 18 22]
        [AAPL AAPL GOOG GOOG AAPL]
        [149.0 150.0 279.0 278.0 151.0]
        [150.0 151.0 280.0 279.0 152.0])))

; Match each trade to the latest quote for the same symbol
(asof-join [Symbol Time] trades quotes)
```

```text
┌──────┬────────┬───────┬─────┬───────┬───────┐
│ Time │ Symbol │ Price │ Qty │  Bid  │  Ask  │
│ I64  │  SYM   │  F64  │ I64 │  F64  │  F64  │
├──────┼────────┼───────┼─────┼───────┼───────┤
│ 10   │ AAPL   │ 150.0 │ 100 │ 149.0 │ 150.0 │
│ 15   │ GOOG   │ 280.0 │ 200 │ 279.0 │ 280.0 │
│ 20   │ AAPL   │ 151.0 │ 150 │ 150.0 │ 151.0 │
│ 25   │ GOOG   │ 279.0 │ 300 │ 278.0 │ 279.0 │
│ 30   │ AAPL   │ 152.0 │ 120 │ 151.0 │ 152.0 │
└──────┴────────┴───────┴─────┴───────┴───────┘
```

Each trade row now carries the `Bid` and `Ask` from the most recent quote for the same `Symbol` at or before the trade's `Time`. For example, the AAPL trade at time 20 picks up the quote from time 12 (Bid 150.0), not the one from time 22 (which is in the future).

## 6. Window Joins

A window join aggregates right rows within a time interval for each left row,
partitioned by optional equality keys. Its form is
`(window-join [eqKeys... timeKey] intervals left right {aggregations})`:

```lisp
(set trades (table [Time Symbol Price]
  (list [10 20 30 40 50]
        [AAPL AAPL GOOG GOOG AAPL]
        [150.0 151.0 280.0 279.0 152.0])))

(set quotes (table [Time Symbol Bid Ask]
  (list [5 8 15 18 22 25 35 45]
        [AAPL AAPL GOOG GOOG AAPL GOOG GOOG AAPL]
        [149.0 149.5 278.0 279.0 150.5 279.5 278.5 151.0]
        [150.0 150.5 279.0 280.0 151.5 280.5 279.5 152.0])))

; Average quotes from ten time units before each trade through the trade time.
(set intervals (map-left + [-10 0] (at trades 'Time)))
(window-join [Symbol Time] intervals trades quotes
  {avg_bid: (avg Bid) avg_ask: (avg Ask)})
```

The last key, `Time`, is the temporal key; `Symbol` is the equality key. The
two vectors in `intervals` provide the lower and upper offsets for each trade.

## 7. Multi-Step Pipeline

Combine multiple techniques into a single analysis. Starting from the original trade data: aggregate by symbol, compute revenue, rank by revenue, and pivot quantity by symbol and time bucket.

```lisp
; Recreate the base trade table
(set trades (table [Hour Symbol Price Qty]
  (list [9 9 9 10 10 10 11 11 11]
        [AAPL GOOG MSFT AAPL GOOG MSFT AAPL GOOG MSFT]
        [150.0 280.0 420.0 155.0 275.0 415.0 160.0 285.0 410.0]
        [100 200 50 300 150 75 120 180 60])))
```

**Step 1: Revenue by symbol.** Group by `Symbol`, compute total revenue:

```lisp
(set summary (select {from: trades by: Symbol
                      revenue: (sum (* Price Qty))}))
```

```text
┌────────┬────────────────────────────┐
│ Symbol │          revenue           │
│  SYM   │            F64             │
├────────┼────────────────────────────┤
│ AAPL   │ 80700.0                    │
│ GOOG   │ 148550.0                   │
│ MSFT   │ 76725.0                    │
└────────┴────────────────────────────┘
```

**Step 2: Top 2 symbols by revenue.**

```lisp
(take (xdesc summary 'revenue) 2)
```

```text
┌────────┬────────────────────────────┐
│ Symbol │          revenue           │
│  SYM   │            F64             │
├────────┼────────────────────────────┤
│ GOOG   │ 148550.0                   │
│ AAPL   │ 80700.0                    │
└────────┴────────────────────────────┘
```

**Step 3: Pivot quantity by symbol and hour.**

```lisp
(pivot trades 'Symbol 'Hour 'Qty sum)
```

```text
┌────────┬─────┬─────┬────────────────┐
│ Symbol │  9  │ 10  │       11       │
│  SYM   │ I64 │ I64 │      I64       │
├────────┼─────┼─────┼────────────────┤
│ AAPL   │ 100 │ 300 │ 120            │
│ GOOG   │ 200 │ 150 │ 180            │
│ MSFT   │ 50  │ 75  │ 60             │
└────────┴─────┴─────┴────────────────┘
```

The pivot shows how each symbol's trading volume is distributed across the three hours. AAPL volume concentrates in hour 10, while GOOG is strongest in hours 9 and 11.

## Next Steps

- [**Graph Queries Tutorial**](graph.md) — Model a social network, traverse paths, and detect communities
- [**Analytics Cookbook**](../guides/analytics.md) — More analytics patterns and recipes
- [**Joins Reference**](../queries/joins.md) — Full details on inner, left, ASOF, and window joins
- [**Functions Reference**](../language/functions.md) — Complete list of all built-in functions

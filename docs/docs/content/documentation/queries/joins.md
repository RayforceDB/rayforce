# :material-table-network: Joins

Join operations combine rows from two [:material-table: Tables](../data-types/table.md) based on matching column values. 

## :material-set-left-right: Left Join

Keeps **all rows from the left table** and matching rows from the right table. Missing matches are filled with null values. Use left join when you want to preserve all records from the left table.

```clj
(set trades (table [symbol order_id price quantity] 
    (list ['AAPL 'MSFT 'GOOG] [1001 1002 1003] [150.25 300.50 125.75] [100 200 150])))
┌────────┬──────────┬────────┬──────────┐
│ symbol │ order_id │ price  │ quantity │
├────────┼──────────┼────────┼──────────┤
│ AAPL   │ 1001     │ 150.25 │ 100      │
│ MSFT   │ 1002     │ 300.50 │ 200      │
│ GOOG   │ 1003     │ 125.75 │ 150      │
├────────┴──────────┴────────┴──────────┤
│ 3 rows (3 shown) 4 columns (4 shown)  │
└───────────────────────────────────────┘

(set orders (table [order_id client_id timestamp status] 
   (list [1001 1002 1004] 
         ['CLIENT_A 'CLIENT_B 'CLIENT_C] 
         [09:00:00 09:05:00 09:10:00] 
         ['FILLED 'FILLED 'PENDING])))
┌──────────┬───────────┬──────────────┬─────────┐
│ order_id │ client_id │ timestamp    │ status  │
├──────────┼───────────┼──────────────┼─────────┤
│ 1001     │ CLIENT_A  │ 09:00:00.000 │ FILLED  │
│ 1002     │ CLIENT_B  │ 09:05:00.000 │ FILLED  │
│ 1004     │ CLIENT_C  │ 09:10:00.000 │ PENDING │
├──────────┴───────────┴──────────────┴─────────┤
│ 3 rows (3 shown) 4 columns (4 shown)          │
└───────────────────────────────────────────────┘

(left-join [order_id] trades orders)
┌──────────┬──────────────┬────────┬────────┬──────────┬───────────┬────────┐
│ order_id │ timestamp    │ symbol │ price  │ quantity │ client_id │ status │
├──────────┼──────────────┼────────┼────────┼──────────┼───────────┼────────┤
│ 1001     │ 09:00:00.000 │ AAPL   │ 150.25 │ 100      │ CLIENT_A  │ FILLED │
│ 1002     │ 09:05:00.000 │ MSFT   │ 300.50 │ 200      │ CLIENT_B  │ FILLED │
│ 1003     │ Null         │ GOOG   │ 125.75 │ 150      │ Null      │ Null   │
├──────────┴──────────────┴────────┴────────┴──────────┴───────────┴────────┤
│ 3 rows (3 shown) 7 columns (7 shown)                                      │
└───────────────────────────────────────────────────────────────────────────┘
```

### Multiple Column Join

```clj
(set positions (table [symbol account_id quantity] 
  (list ['AAPL 'MSFT 'AAPL] ['ACC001 'ACC001 'ACC002] [1000 500 2000])))

(set prices (table [symbol account_id price] 
  (list ['AAPL 'MSFT 'AAPL] ['ACC001 'ACC001 'ACC002] [150.25 300.50 150.30])))

(left-join [symbol account_id] positions prices)  ;; Left join on multiple columns: symbol and account_id
┌────────┬────────────┬────────┬──────────┐
│ symbol │ account_id │ price  │ quantity │
├────────┼────────────┼────────┼──────────┤
│ AAPL   │ ACC001     │ 150.25 │ 1000     │
│ MSFT   │ ACC001     │ 300.50 │ 500      │
│ AAPL   │ ACC002     │ 150.30 │ 2000     │
├────────┴────────────┴────────┴──────────┤
│ 3 rows (3 shown) 4 columns (4 shown)    │
└─────────────────────────────────────────┘
```

!!! note ""
    The `left-join` function takes three arguments:
    1. A [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) representing join columns.
    2. The left [:material-table: Table](../data-types/table.md)
    3. The right [:material-table: Table](../data-types/table.md)

## :material-border-inside: Inner Join

Keeps **only rows where the join columns match in both tables**. Returns the intersection of both tables. Use inner join when you only want records that exist in both tables.

```clj
(set trades (table [symbol order_id price quantity] 
  (list ['AAPL 'MSFT 'GOOG] [1001 1002 1003] 
        [150.25 300.50 125.75] [100 200 150])))

(set settlements (table [order_id settlement_date fee] 
  (list [1001 1002 1004] [2024.01.15 2024.01.15 2024.01.16] [0.50 1.00 0.75])))

;; Inner join: only trades that have been settled
(inner-join [order_id] trades settlements)
┌──────────┬────────┬────────┬──────────┬─────────────────┬──────┐
│ order_id │ symbol │ price  │ quantity │ settlement_date │ fee  │
├──────────┼────────┼────────┼──────────┼─────────────────┼──────┤
│ 1001     │ AAPL   │ 150.25 │ 100      │ 2024.01.15      │ 0.50 │
│ 1002     │ MSFT   │ 300.50 │ 200      │ 2024.01.15      │ 1.00 │
├──────────┴────────┴────────┴──────────┴─────────────────┴──────┤
│ 2 rows (2 shown) 6 columns (6 shown)                           │
└────────────────────────────────────────────────────────────────┘
```

*Note that `GOOG` (order_id 1003) is excluded because it has no matching settlement record.*

!!! note ""
    The `inner-join` function takes three arguments:
    1. A [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) representing join columns.
    2. The left [:material-table: Table](../data-types/table.md)
    3. The right [:material-table: Table](../data-types/table.md)

## :material-clock-time-four: Asof Join

Matches rows based on equality for all join columns **except the last one**, and finds the **greatest value less than or equal to** the left table's value for the last column. Ideal for time-series data where you want the most recent value up to a point in time.

```clj
(set n 10)
(set tsym (take n (concat (take 99 'AAPL) (take 1 'MSFT))))
(set ttime (+ 09:00:00 (as 'Time (/ (* (til n) 3) 10))))
(set price (+ 10 (til n)))
(set bsym (take (* 2 n) (concat (concat (take 3 'AAPL) (take 2 'MSFT)) (take 1 'GOOG))))
(set btime (+ 09:00:00 (as 'Time (/ (* (til (* 2 n)) 2) 10))))
(set bid (+ 8 (/ (til (* 2 n)) 2)))
(set ask (+ 12 (/ (til (* 2 n)) 2)))
(set trades (table [Sym Ts Price] (list tsym ttime price)))
(set quotes (table [Sym Ts Bid Ask] (list bsym btime bid ask)))

(asof-join [Sym Ts] trades quotes)
┌──────┬──────────────┬───────┬─────┬─────┐
│ Sym  │ Ts           │ Price │ Bid │ Ask │
├──────┼──────────────┼───────┼─────┼─────┤
│ AAPL │ 09:00:00.000 │ 10    │ 9   │ 13  │
│ AAPL │ 09:00:00.000 │ 11    │ 9   │ 13  │
│ AAPL │ 09:00:00.000 │ 12    │ 9   │ 13  │
│ AAPL │ 09:00:00.000 │ 13    │ 9   │ 13  │
│ AAPL │ 09:00:00.001 │ 14    │ 12  │ 16  │
│ AAPL │ 09:00:00.001 │ 15    │ 12  │ 16  │
│ AAPL │ 09:00:00.001 │ 16    │ 12  │ 16  │
│ AAPL │ 09:00:00.002 │ 17    │ 15  │ 19  │
│ AAPL │ 09:00:00.002 │ 18    │ 15  │ 19  │
│ AAPL │ 09:00:00.002 │ 19    │ 15  │ 19  │
├──────┴──────────────┴───────┴─────┴─────┤
│ 10 rows (10 shown) 5 columns (5 shown)  │
└─────────────────────────────────────────┘
```

!!! note ""
    The `asof-join` function takes three arguments:
    a [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) - the last column is used for "as of" matching (typically a time/timestamp column), the left [:material-table: Table](../data-types/table.md), the right [:material-table: Table](../data-types/table.md)
    
    For each row in the left table, it finds the row in the right table where:

    - All join columns except the last match exactly
    - The last join column in the right table is ≤ the last join column in the left table
    - And it's the greatest such value


## :material-window-restore: Window Join

The `window-join` and `window-join1` functions are designed for **time-series data**. They join tables based on equality for all join columns except the last one, and use **time windows** for the last column. Perfect for aggregating data within time ranges.

```clj
(set n 100000)
(set tsym (take n (concat (take 99 'AAPL) (take 1 'MSFT))))
(set ttime (+ 09:00:00 (as 'Time (/ (* (til n) 3) 10))))
(set price (+ 10 (til n)))
(set bsym (take (* 2 n) (concat (concat (take 3 'AAPL) (take 2 'MSFT)) (take 1 'GOOG))))
(set btime (+ 09:00:00 (as 'Time (/ (* (til (* 2 n)) 2) 10))))
(set bid (+ 8 (/ (til (* 2 n)) 2)))
(set ask (+ 12 (/ (til (* 2 n)) 2)))
(set trades (table [Sym Ts Price] (list tsym ttime price)))
(set quotes (table [Sym Ts Bid Ask] (list bsym btime bid ask)))

;; Create intervals: ±1000 milliseconds around each trade timestamp
(set intervals (map-left + [-1000 1000] (at trades 'Ts)))
(
  [08:59:59.000 08:59:59.000 08:59:59.000 08:59:59.000 08:59:59.001 08:59:59.001 08:59:59.001..]
  [09:00:01.000 09:00:01.000 09:00:01.000 09:00:01.000 09:00:01.001 09:00:01.001 09:00:01.001..]
)

(window-join [Sym Ts] intervals trades quotes {  ;; Excludes interval bounds
  bid: (min Bid)
  ask: (max Ask)})
┌──────┬──────────────┬────────┬───────┬───────┐
│ Sym  │ Ts           │ Price  │ bid   │ ask   │
├──────┼──────────────┼────────┼───────┼───────┤
│ AAPL │ 09:00:00.000 │ 10     │ 8     │ 2514  │
│ AAPL │ 09:00:00.000 │ 11     │ 8     │ 2514  │
│ AAPL │ 09:00:00.000 │ 12     │ 8     │ 2514  │
│ AAPL │ 09:00:00.000 │ 13     │ 8     │ 2514  │
│ AAPL │ 09:00:00.001 │ 14     │ 8     │ 2515  │
│ AAPL │ 09:00:00.001 │ 15     │ 8     │ 2515  │
│ AAPL │ 09:00:00.001 │ 16     │ 8     │ 2515  │
│ AAPL │ 09:00:00.002 │ 17     │ 8     │ 2518  │
│ AAPL │ 09:00:00.002 │ 18     │ 8     │ 2518  │
│ AAPL │ 09:00:00.002 │ 19     │ 8     │ 2518  │
┆ …    ┆ …            ┆ …      ┆ …     ┆ …     ┆
│ AAPL │ 09:00:29.997 │ 100000 │ 72501 │ 77506 │
│ AAPL │ 09:00:29.997 │ 100001 │ 72501 │ 77506 │
│ AAPL │ 09:00:29.997 │ 100002 │ 72501 │ 77506 │
│ AAPL │ 09:00:29.997 │ 100003 │ 72501 │ 77506 │
│ AAPL │ 09:00:29.998 │ 100004 │ 72504 │ 77509 │
│ AAPL │ 09:00:29.998 │ 100005 │ 72504 │ 77509 │
│ AAPL │ 09:00:29.998 │ 100006 │ 72504 │ 77509 │
│ AAPL │ 09:00:29.999 │ 100007 │ 72507 │ 77511 │
│ AAPL │ 09:00:29.999 │ 100008 │ 72507 │ 77511 │
│ MSFT │ 09:00:29.999 │ 100009 │ 72507 │ 77510 │
├──────┴──────────────┴────────┴───────┴───────┤
│ 100000 rows (20 shown) 5 columns (5 shown)   │
└──────────────────────────────────────────────┘

(window-join1 [Sym Ts] intervals trades quotes {  ;; Window join1: same but includes interval bounds
  bid: (min Bid)
  ask: (max Ask)})
```

Both functions take five arguments:

1. **`columns`**: A [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) - the last column is used for window matching
2. **`intervals`**: A [:material-code-array: List](../data-types/list.md) of two [:material-vector-line: Vectors](../data-types/vector.md) representing time windows `[lower-bounds upper-bounds]`
3. **`left-table`**: The left [:material-table: Table](../data-types/table.md)
4. **`right-table`**: The right [:material-table: Table](../data-types/table.md)
5. **`aggregations`**: A [:material-code-braces: Dictionary](../data-types/dictionary.md) of aggregations

### Creating Intervals

Intervals are created using `map-left` to add time offsets to each timestamp:

```clj
;; Create intervals of ±1000 milliseconds around each timestamp
(set intervals (map-left + [-1000 1000] (at trades 'Ts)))
```

This creates a list of two vectors:

- First vector: <b>lower bounds (timestamp - 1000ms)</b>
- Second vector: <b>upper bounds (timestamp + 1000ms)</b>

!!! note ""
    The intervals define time windows around each timestamp. For each trade, the window join finds all quotes within that time window and applies the specified aggregations.

!!! warning "Difference between `window-join` and `window-join1`"

    - **`window-join`**: Excludes interval bounds from aggregation (open interval)
    - **`window-join1`**: Includes interval bounds in aggregation (closed interval)

    Use `window-join1` when you want to include data exactly at the boundary times.

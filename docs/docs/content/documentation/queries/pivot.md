# :material-table-pivot: Pivot

The `pivot` function transforms data from long format to wide format by rotating unique values from one column into multiple columns. This is useful for creating summary tables, cross-tabulations, and reports where you want to compare values across categories.

```clj
(set trades (table [symbol side quantity price]
    (list
        ['AAPL 'GOOG 'AAPL 'GOOG 'MSFT 'MSFT]
        ['Buy 'Buy 'Sell 'Sell 'Buy 'Sell]
        [100 200 150 180 120 90]
        [150 280 152 275 420 418])))
┌────────┬────────┬──────────┬───────┐
│ symbol │  side  │ quantity │ price │
│ SYMBOL │ SYMBOL │   I64    │  I64  │
├────────┼────────┼──────────┼───────┤
│ AAPL   │ Buy    │ 100      │ 150   │
│ GOOG   │ Buy    │ 200      │ 280   │
│ AAPL   │ Sell   │ 150      │ 152   │
│ GOOG   │ Sell   │ 180      │ 275   │
│ MSFT   │ Buy    │ 120      │ 420   │
│ MSFT   │ Sell   │ 90       │ 418   │
└────────┴────────┴──────────┴───────┘

;; Pivot: sum of quantity by symbol (rows) and side (columns)
(pivot trades 'symbol 'side 'quantity sum)
┌────────┬─────┬──────┐
│ symbol │ Buy │ Sell │
│ SYMBOL │ I64 │ I64  │
├────────┼─────┼──────┤
│ AAPL   │ 100 │ 150  │
│ GOOG   │ 200 │ 180  │
│ MSFT   │ 120 │ 90   │
└────────┴─────┴──────┘
```

## Syntax

```clj
(pivot table index columns values aggfunc)
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `table` | [:material-table: Table](../data-types/table.md) | The source table to pivot |
| `index` | [:simple-scalar: Symbol](../data-types/symbol.md) or [:material-vector-line: Vector](../data-types/vector.md) | Column(s) to use as row labels |
| `columns` | [:simple-scalar: Symbol](../data-types/symbol.md) | Column whose unique values become new columns |
| `values` | [:simple-scalar: Symbol](../data-types/symbol.md) | Column to aggregate |
| `aggfunc` | [:material-function: Function](../data-types/functions.md) | Aggregation function (built-in or custom lambda) |

## Aggregation Functions

Any unary function that takes a vector and returns a scalar can be used. Built-in aggregation functions include:

| Function | Description |
|----------|-------------|
| `sum` | Sum of values |
| `count` | Count of values |
| `avg` | Mean (average) of values |
| `min` | Minimum value |
| `max` | Maximum value |
| `first` | First value in group |
| `last` | Last value in group |
| `med` | Median value |

You can also use custom lambda functions for specialized aggregations:

```clj
;; Sum of squares
(pivot trades 'symbol 'side 'quantity (fn [x] (sum (* x x))))

;; Standard deviation
(pivot trades 'symbol 'side 'price dev)

;; Custom weighted calculation
(pivot trades 'symbol 'side 'quantity (fn [x] (/ (sum x) (count x))))
```

## Multi-Index Pivot

You can specify multiple columns as the index by using a [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md):

```clj
(set trades (table [date symbol side quantity]
    (list
        [2024.01.01 2024.01.01 2024.01.02 2024.01.02]
        ['AAPL 'GOOG 'AAPL 'GOOG]
        ['Buy 'Sell 'Sell 'Buy]
        [100 200 150 180])))

;; Pivot with multi-column index
(pivot trades [date symbol] 'side 'quantity sum)
┌────────────┬────────┬──────┬──────┐
│    date    │ symbol │ Buy  │ Sell │
│    DATE    │ SYMBOL │ LIST │ LIST │
├────────────┼────────┼──────┼──────┤
│ 2024.01.01 │ AAPL   │ 100  │ Null │
│ 2024.01.01 │ GOOG   │ Null │ 200  │
│ 2024.01.02 │ AAPL   │ Null │ 150  │
│ 2024.01.02 │ GOOG   │ 180  │ Null │
└────────────┴────────┴──────┴──────┘
```

!!! note "Null Values"
    When a combination of index and column values doesn't exist in the source data, the pivot result will contain `Null` for that cell.


## See Also

- [:material-table: Table](../data-types/table.md) - Table data type
- [:simple-googlebigquery: Select Query](select.md) - Data selection and aggregation
- [:material-table-network: Joins](joins.md) - Combining tables

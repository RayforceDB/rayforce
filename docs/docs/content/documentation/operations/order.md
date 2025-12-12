# :material-sort: Order Operations

RayforceDB provides functions for sorting and ordering [:material-vector-line: Vectors](../data-types/vector.md), [:material-code-array: Lists](../data-types/list.md), and [:material-table: Tables](../data-types/table.md).

## Sorting Functions

### :material-sort-ascending: Asc

Sorts elements in ascending order. Returns a new sorted [:material-vector-line: Vector](../data-types/vector.md) or [:material-code-array: List](../data-types/list.md).

```clj
(asc [3 1 4 1 5 9 2 6])
[1 1 2 3 4 5 6 9]

(asc [150.25 300.50 125.75 200.00])
[125.75 150.25 200.00 300.50]

(asc (list "banana" "apple" "cherry"))
(apple banana cherry)
```

### :material-sort-descending: Desc

Sorts elements in descending order. Returns a new sorted [:material-vector-line: Vector](../data-types/vector.md) or [:material-code-array: List](../data-types/list.md).

```clj
(desc [3 1 4 1 5 9 2 6])
[9 6 5 4 3 2 1 1]

(desc [150.25 300.50 125.75 200.00])
[300.50 200.00 150.25 125.75]

(desc (list "banana" "apple" "cherry"))
(cherry banana apple)
```

## Index Functions

### :material-sort-ascending: Iasc

Returns indices that would sort the input in ascending order.

```clj
(iasc [3 1 4 1 5])
[1 3 0 2 4]

(set prices [150.25 300.50 125.75])
(set symbols ['AAPL 'MSFT 'GOOG])
(at symbols (iasc prices))
[GOOG AAPL MSFT]

(iasc (list "banana" "apple" "cherry"))
[1 0 2]
```

### :material-sort-descending: Idesc

Returns indices that would sort the input in descending order.

```clj
(idesc [3 1 4 1 5])
[4 2 0 1 3]

;; Use indices to reorder another vector
(set prices [150.25 300.50 125.75])
(set symbols ['AAPL 'MSFT 'GOOG])
(at symbols (idesc prices))
[MSFT AAPL GOOG]

(idesc (list "banana" "apple" "cherry"))
[2 0 1]
```


## Ranking Functions

### :material-numeric: Rank

Returns the rank (position in sorted order) of each element in a [:material-vector-line: Vector](../data-types/vector.md). The smallest element gets rank 0, the next smallest gets rank 1, and so on.

```clj
(rank [30 10 20])
[2 0 1]

(rank [5 3 1 4 2])
[4 2 0 3 1]

;; Rank prices: smallest price gets rank 0
(rank [150.25 300.50 125.75])
[1 2 0]
```

### :material-numeric-1-box: Xrank

Assigns each element to a bucket based on its rank, dividing the data into n equal-sized groups. Returns bucket indices from 0 to n-1.

```clj
(xrank [30 10 20 40 50 60] 3)
[1 0 0 1 2 2]

(xrank [1 2 3 4] 2)
[0 0 1 1]

;; Divide prices into 3 buckets (tertiles)
(xrank [150.25 300.50 125.75 200.00 175.50] 3)
[1 2 0 2 1]
```

## Table Sorting Functions

### :material-sort-ascending: Xasc

Sorts a [:material-table: Table](../data-types/table.md) in ascending order based on specified columns. When multiple columns are specified, sorting is done by the first column, then by the second column for ties, and so on.

```clj
(set trades (table [symbol price quantity] 
  (list ['AAPL 'MSFT 'GOOG] [150.25 300.50 125.75] [100 200 150])))

(xasc [price] trades)
┌────────┬────────┬──────────┐
│ symbol │ price  │ quantity │
├────────┼────────┼──────────┤
│ GOOG   │ 125.75 │ 150      │
│ AAPL   │ 150.25 │ 100      │
│ MSFT   │ 300.50 │ 200      │
└────────┴────────┴──────────┘

;; Sort by multiple columns: first by symbol, then by price
(xasc [symbol price] trades)
```

!!! note ""
    The `xasc` function takes two arguments:
    1. A [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) representing column names to sort by
    2. The [:material-table: Table](../data-types/table.md) to sort

### :material-sort-descending: Xdesc

Sorts a [:material-table: Table](../data-types/table.md) in descending order based on specified columns. When multiple columns are specified, sorting is done by the first column, then by the second column for ties, and so on.

```clj
(set trades (table [symbol price quantity] 
  (list ['AAPL 'MSFT 'GOOG] [150.25 300.50 125.75] [100 200 150])))

(xdesc [price] trades)
┌────────┬────────┬──────────┐
│ symbol │ price  │ quantity │
├────────┼────────┼──────────┤
│ MSFT   │ 300.50 │ 200      │
│ AAPL   │ 150.25 │ 100      │
│ GOOG   │ 125.75 │ 150      │
└────────┴────────┴──────────┘

;; Sort by multiple columns: first by price (descending), then by quantity (descending)
(xdesc [price quantity] trades)
```

!!! note ""
    The `xdesc` function takes two arguments:
    1. A [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Symbols](../data-types/symbol.md) representing column names to sort by
    2. The [:material-table: Table](../data-types/table.md) to sort


## Numeric Operations

### :material-minus-circle: Neg

Returns the negative of a number or applies negation element-wise to [:material-vector-line: Vectors](../data-types/vector.md).

```clj
(neg 5)
-5

(neg -3)
3

(neg [1 -2 3 -4])
[-1 2 -3 4]

;; Negate prices to calculate price changes
(neg [150.25 300.50 125.75])
[-150.25 -300.50 -125.75]
```

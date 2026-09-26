# Select, Update, Insert & Upsert

Query, mutate, and manage table data with Rayfall's declarative query syntax. All query forms compile to a fused DAG for morsel-driven parallel execution.

## Select

The `select` form is the primary way to query tables. It supports projection, filtering, computed columns, and group-by aggregation through a single dictionary argument.

### Basic Projection

Select all columns from a table (identity query):

```lisp
; Create a sample table
(set t (table [sym price volume]
    (list [AAPL GOOG MSFT] [150 280 420] [500 400 900])))

; Select all rows and columns
(select {from: t})
; sym  price volume
; ---  ----- ------
; AAPL   150    500
; GOOG   280    400
; MSFT   420    900
```

Select specific columns with computed expressions:

```lisp
(select {from: t sym: sym notional: (* price volume)})
; sym  notional
; ---  --------
; AAPL    75000
; GOOG   112000
; MSFT   378000
```

### Projections that build on earlier projections

A projection sees every projection defined before it in the same `select`,
by its alias — as a bare name or as a literal symbol:

```lisp
(select {from: t sym: sym notional: (* price volume) nn: (+ notional 1)})
; sym  notional nn
; ---  -------- ------
; AAPL    75000  75001
; GOOG   112000 112001
; MSFT   378000 378001
```

The alias is bound after its own expression, so an alias that shadows a
source column reads the source in its own definition and the new column in
every projection after it:

```lisp
(select {from: t price: (* price 2) p2: (+ price 1)})
; price p2
; ----- ---
;   300 301
;   560 561
;   840 841
```

Only projections see aliases. `where:` and `by:` are evaluated against the
source table, so `where: (> notional 100000)` raises `schema` — filter on
the expression itself, or on a nested select.

In a grouped select a later output may build on an earlier aggregate. An
alias that is one value per group — an aggregate, or an output built on one
— is read from the group result, so `nn` below is computed from the
per-group sum, and a chain of such outputs costs one column each. An alias
of a per-row expression (`vals: price`) is substituted where it is named,
so `m: (max vals)` is `(max price)`.

```lisp
(select {from: t by: sym notional: (sum (* price volume)) nn: (+ notional 1)})
```

Three rules follow from an alias being one value per group. Inside an
aggregate's argument a name that is a source column is always the source
column, so `s: (sum s) mx: (max s)` takes the maximum of the rows, not of a
sum. An aggregate cannot be aggregated again, through an alias or written
out: `s: (sum price) mx: (max s)` and `mx: (max (sum price))` both raise
`domain`. And an output may not put a source column beside a per-group
value outside an aggregate: `f: (* price n)` with `n: (count price)` raises
`domain`; write `(* (sum price) n)` or whichever aggregate is meant.

A literal symbol inside a select resolves to an earlier alias or a source
column of that name, in that order; one naming neither stays a constant
symbol.  Arithmetic on a symbol — a literal or a symbol column — is a `type`
error inside a select, as it is outside.

### Whole-column projections

A projection may be a whole-column verb — `distinct`, `asc`, `desc`, or
`reverse` — applied to a column. Unlike a per-row computed expression, these
consume the entire column and return a vector, so the result's row count can
differ from the source table (`distinct` in particular collapses duplicates):

```lisp
(select {price: (distinct price) from: t})
; price
; -----
;   150
;   280
;   420

(select {price: (asc price) from: t})   ; ascending column
```

A `where:` clause is applied first, so the verb only sees the surviving rows:

```lisp
(select {price: (distinct price) from: t where: (> volume 400)})
```

Because a whole-column verb changes the row count, it cannot be mixed with a
full-length column in the same query — pairing `(distinct price)` with a plain
`sym` column raises a `length` error. Project the single reshaped column on its
own, then compose further clauses (`asc:`, `take:`) or an outer `xasc`/`xdesc`
around it.

### Filtering with `where:`

The `where:` clause accepts any predicate expression. Predicates are pushed down through the DAG for early elimination of rows:

```lisp
(select {from: t where: (> price 200)})
; sym  price volume
; ---  ----- ------
; GOOG   280    400
; MSFT   420    900
```

Combine multiple conditions:

```lisp
(select {from: t where: (and (> price 100) (< volume 600))})
; sym  price volume
; ---  ----- ------
; AAPL   150    500
; GOOG   280    400
```

### Group-by Aggregation

The `by:` clause groups rows by one or more key columns. Aggregation functions are specified as additional key-value pairs in the query dict:

```lisp
(set trades (table [sym side price qty]
    (list [AAPL AAPL GOOG GOOG AAPL]
          [Buy Sell Buy Buy Buy]
          [150 152 280 282 149]
          [100 200 50 75 300])))

; Group by sym, aggregate price and qty
(select {from: trades by: sym
         avg_price: (avg price)
         total_qty: (sum qty)
         n: (count price)})
; sym  avg_price total_qty  n
; ---  --------- ---------  -
; AAPL    150.33       600  3
; GOOG     281.0       125  2
```

Group by multiple keys:

```lisp
(select {from: trades by: [sym side]
         total: (sum qty)})
; sym  side total
; ---  ---- -----
; AAPL Buy    400
; AAPL Sell   200
; GOOG Buy    125
```

Expressions over aggregates work with any number of keys and with binary aggregates such as `pearson_corr`, `cov`, or `wsum`; the aggregates are computed in the grouping pass and the outer expression once per group:

```lisp
(select {from: trades spread: (- (max price) (min price)) by: {sym: sym venue: venue}})
(select {from: trades r2: (pow (pearson_corr price size) 2) by: {sym: sym}})
```

### Filter + Group-by

Filter first, then group by. Chain two `select` calls for correct results:

```lisp
; Step 1: filter to Buy rows only
(set buys (select {from: trades where: (== side "Buy")}))

; Step 2: group and aggregate
(select {from: buys by: sym
         buy_qty: (sum qty)})
; sym  buy_qty
; ---  -------
; AAPL     400
; GOOG     125
```

### Sorting with `asc:` and `desc:`

Sort the result by one or more columns. Use `asc:` for ascending and `desc:` for descending order:

```lisp
; Sort by price ascending
(select {from: t asc: price})

; Sort by price descending
(select {from: t desc: price})
```

Sort by multiple columns by passing a symbol vector:

```lisp
; Sort by sym ascending, then price ascending
(select {from: t asc: [sym price]})
```

Mix ascending and descending with separate clauses. Sort priority follows the order of clauses in the dictionary:

```lisp
; Primary: sym ascending. Secondary: price descending.
(select {from: t asc: sym desc: price})
```

### Limiting Rows with `take:`

Limit the number of rows in the result. Positive values take from the beginning, negative from the end:

```lisp
; First 5 rows
(select {from: t take: 5})

; Last 3 rows
(select {from: t take: -3})
```

For a range slice, pass a two-element vector `[start count]`:

```lisp
; Skip 10 rows, then take 5
(select {from: t take: [10 5]})
```

### Combining Clauses

All clauses can be combined. They apply in this order: `where:` → `by:` / projection → `asc:`/`desc:` → `take:`

```lisp
; Filter, sort, and limit in one query
(select {from: trades where: (> price 100) desc: price take: 10})
; Top 10 most expensive trades above $100
```

## Update

The `update` form modifies columns in-place on a quoted table variable. It supports the same `where:` and `by:` clauses as `select`.

### Column Mutation

Update one or more columns with a new expression. The table is modified in-place when the first argument is a quoted symbol:

```lisp
(set tab (table [sym price volume]
    (list [AAPL GOOG MSFT] [102 203 99] [500 400 900])))

; Increment volume by 1
(update {volume: (+ 1 volume) from: 'tab})
; => tab           (returns the quoted table symbol, not the table)

; The mutation is applied in-place; re-query to see it:
tab
; sym  price volume
; ---  ----- ------
; AAPL   102    501
; GOOG   203    401
; MSFT    99    901
```

### Conditional Update

Apply updates only to rows matching a predicate:

```lisp
; Apply a custom function to price where volume == 901
(update {price: ((fn [x] (+ x 11)) price) from: 'tab where: (== volume 901)})
; sym  price volume
; ---  ----- ------
; AAPL   102    501
; GOOG   203    401
; MSFT   110    901
```

Set a column to a constant for matching rows:

```lisp
(update {price: 0 from: 'tab where: (== volume 901)})
```

### Update with Group-by

Combine `where:` and `by:` for grouped conditional updates:

```lisp
(update {price: 0 from: 'tab where: (> volume 400)})
```

## Insert

The `insert` function appends rows to a table. It accepts rows as a list of atoms, a list of vectors, a dictionary, or another table. Column order is flexible — missing columns receive null values.

### Insert from List

```lisp
(set t (table [ID Name Value]
    (list [1 2 3] ['Alice 'Bob 'Charlie] [10.0 20.0 30.0])))

; Insert a single record
(set t (insert t (list 4 'David 40.0)))

; Insert multiple records (list of vectors)
(set t (insert t (list [5 6] ['Eve 'Frank] [50.0 60.0])))
```

### Insert from Dictionary

Dictionaries allow columns in any order. Missing columns are filled with null:

```lisp
; Columns in different order
(set t (insert t (dict [Value Name ID] (list 120.0 'Leo 12))))

; Partial columns — Value will be null
(set t (insert t (dict [ID Name] (list 14 'Nancy))))
```

### Insert from Table

```lisp
; Append another table (column order can differ)
(set t (insert t (table [Value ID Name]
    (list [180.0 190.0] [18 19] ['Rose 'Sam]))))
```

### In-place Insert

Pass a quoted table symbol for in-place mutation:

```lisp
; Mutate table in-place (no set needed)
(insert 't (dict [Value Name ID] (list 200.0 'Tom 20)))
```

## Upsert

The `upsert` function inserts new rows or updates existing ones based on a composite key formed from the table's leading columns. The second argument is the **key count** — the number of leading columns used as the match key. It must be in the range `1..min(ncols, 16)`; passing `0` is an error.

### Insert New Rows

```lisp
(set t (table [ID Name Value]
    (list [1 2 3] ['Alice 'Bob 'Charlie] [10.0 20.0 30.0])))

; Upsert on the first 1 column (ID) as key. ID=4 is new, so insert.
(set t (upsert t 1 (list 4 'David 40.0)))
```

### Update Existing Rows

```lisp
; ID=2 exists — update in place
(set t (upsert t 1 (list 2 'Bobby 25.0)))
; ID Name    Value
; -- ----    -----
;  1 Alice    10.0
;  2 Bobby    25.0  ← updated
;  3 Charlie  30.0
;  4 David    40.0
```

### Upsert from Dictionary

Like `insert`, columns can be in any order:

```lisp
; Columns reordered — upsert matches on ID
(set t (upsert t 1 (dict [Name Value ID] (list 'Bobby2 22.0 2))))

; Mixed insert + update in one call
(set t (upsert t 1 (dict [Value ID Name]
    (list [35.0 140.0] [3 14] ['Charlie2 'Nancy]))))
```

### In-place Upsert

```lisp
(upsert 't 1 (dict [Value Name ID] (list 170.0 'Quinn 17)))
```

## Delete Rows

To remove rows matching a predicate, use `select` with the inverse condition:

```lisp
(set t (table [x y] (list [1 2 3 4] [A B C D])))

; Remove rows where x > 2 by keeping rows where x <= 2
(set t (select {from: t where: (<= x 2)}))
; x y
; - -
; 1 A
; 2 B
```

## Quick Reference

| Form | Syntax | Description |
|---|---|---|
| `select` | `(select {from: t col: expr where: pred by: key asc: col desc: col take: n})` | Query with projection, filtering, grouping, sorting, and limiting |
| `update` | `(update {col: expr from: 't where: pred})` | In-place column mutation |
| `insert` | `(insert t rows)` or `(insert 't rows)` | Append rows (list, dict, or table) |
| `upsert` | `(upsert t key-idx rows)` | Insert or update by key column |
| *delete rows* | `(select {from: t where: (not pred)})` | Remove matching rows via inverse filter |

!!! note "Execution model"
    All `select` and `update` queries compile to a lazy DAG. The optimizer applies predicate pushdown, filter reordering, operator fusion, and dead-code elimination before the fused morsel-driven executor processes data in 1024-element chunks.

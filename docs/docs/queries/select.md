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
`domain`. An aggregate that still folds the group's rows may use one:
`mx: (max (+ price (count price)))` adds each group's count to its prices
and takes the largest. And an output may not mix a source column with a reference to an earlier
per-group alias outside an aggregate: `f: (* price n)` with `n: (count price)` raises
`domain`; write `(* (sum price) n)` or whichever aggregate is meant.

The names `min` and `max` reduce only in their **one-argument** forms.
Their two-argument forms are element-wise expressions, as outside a query:
`r: (min x y)` preserves the input rows, and in a grouped select produces a
vector for each group. In contrast, `r: (min (sum x) (sum y))` compares two
per-group scalars; it is not a nested reduction. Genuine unary nesting such
as `r: (min (sum x))` still raises `domain` in a grouped select.

A literal symbol inside a select resolves to an earlier alias or a source
column of that name, in that order; one naming neither stays a constant
symbol.  Arithmetic on a symbol — a literal or a symbol column — is a `type`
error inside a select, as it is outside.

Equality (`==`) and inequality (`!=`) between a non-null symbol and a
non-null numeric or boolean value also raise `type`, rather than comparing
internal symbol IDs. Those IDs are not numeric values and can differ
between in-memory and file-backed symbol domains. Null operands retain the
ordinary comparison builtin's value-sensitive null rules; this is not a
blanket error based only on declared column types.

To use a symbol that is also a column name as a constant, write it as
`(quote name)` or pass it in through a variable.  `enlist` does not escape it:
its argument resolves first, so `(enlist 'price)` enlists the `price` column.

```lisp
(set t (table [sym price] (list [AAPL GOOG price] [10 20 30])))
(select {from: t x: (== sym 'price)})          ; error: type, compares SYM with the I64 column
(select {from: t x: (== sym (quote price))})   ; x: [false false true]
```

### Whole-column projections

A projection may be a whole-column verb — `distinct`, `asc`, `desc`,
`reverse`, `iasc`, `idesc`, `rank`, or `where` — applied to a column. Unlike a
per-row computed expression, these consume the entire column and return a vector, so the
result's row count can differ from the source table (`distinct` in particular
collapses duplicates, `where` keeps the positions of the true rows):

```lisp
(select {price: (distinct price) from: t})
; price
; -----
;   150
;   280
;   420

(select {price: (asc price) from: t})   ; ascending column
```

A `where:` clause is applied first, so the verb only sees the surviving rows.
The positions `where` returns count those rows, not the table's:

```lisp
(select {price: (distinct price) from: t where: (> volume 400)})

(set m (table [k v] (list [1 1 2 2 2] [10 20 30 40 50])))
(select {w: (where (> v 20)) from: m where: (> k 1)})
; w
; -
; 0
; 1
; 2

(set g (table [k v] (list [1 1 2 2 2] [10 20 40 30 50])))
(select {i: (iasc v) from: g where: (> k 1)})   ; i: [1 0 2]
```

Under `by:` the verb sees each group's surviving rows, as an aggregate does:
its grades and ranks count within the group. The same holds for `rotate`
and `xrank`, for a verb under an element-wise call (`(+ 1 (rank v))`), and
for one inside an aggregate's argument (`(sum (rank v))`, `(sum (asc v))`):

```lisp
(at (select {r: (rank v) from: g by: k}) 'r)        ; [[0 1] [1 0 2]]
(at (select {r: (sum (rank v)) from: g by: k}) 'r)  ; [1 3]
```

A verb inside a running function's argument runs with that function, over
the whole table (see [running functions](#running-functions-under-where)):
`(sums (rank v))` by `k` takes each group's rows of the running sum of the
whole table's ranks, not the running sum of each group's ranks.

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

The predicate itself must give a boolean mask (or a scalar truth value): a
numeric column alone, `where: volume`, is a `type` error. Inside `and` and
`or`, though, an operand is read by its truth value, as outside a query, so
`where: (and volume (> price 100))` keeps the rows with a non-zero, non-null
volume. Comparisons take the operand types they take outside a query
([Comparison](../language/math.md#comparison)): `where: (> date 8000)` over
a `DATE` column is a `type` error; compare with a date, `(> date 2021.11.26)`.

### Running functions under `where:`

A running, moving or shift function in a projection (`sums`, `deltas`,
`msum`, `lag` and the rest) runs over the whole table, rows the `where:`
drops included, and the query then keeps the `where:` rows. An aggregate
nested inside such a function sees the whole table too; an aggregate on its
own sees the surviving rows:

```lisp
(set r (table [name room score] (list ['Ann 'Bob 'Cid 'Dan] ['A 'B 'A 'B] [60 20 45 35])))
(select {s: (sums score) from: r where: (== room 'A)})                 ; s: [60 125]
(select {s: (avg score) from: r where: (== room 'A)})                  ; s: [52.5]
(select {s: (sums (- score (avg score))) from: r where: (== room 'A)}) ; s: [20.0 5.0]
```

In the last query `avg` averages all four rows (40), the running sum of
`20 -20 5 -5` is `20 0 5 0`, and rows A keep `20` and `5`. A select used as
`from:` establishes its own input, so to run over the surviving rows only,
select them first:

```lisp
(set a (select {from: r where: (== room 'A)}))
(select {s: (sums score) from: a})                 ; s: [60 105]
(select {s: (sums (- score (avg score))) from: a}) ; s: [7.5 0.0]
```

The [whole-column verbs](#whole-column-projections) (`distinct`, `asc`,
`desc`, `reverse`, `iasc`, `idesc`, `rank`, `xrank`, `rotate`, `take`, `drop`,
`where`) are different: they see the surviving rows only, and under `by:` each
group's surviving rows.

The same holds under `by:`. A running function in a grouped select runs over
the whole table, not over each group, and the groups then take their rows of
what it gave: an aggregate over it aggregates them, and a running output on
its own is that whole-table column split by group. A helper is no different,
whether the planner inlines it or not, named or written in place: a helper
with several statements, or one that passes a builtin as a value, is called
once over the whole columns. A helper runs once per group instead when

- an argument holds an aggregate (`(f i (avg i))`): that aggregate is the
  group's, as it is written inline;
- it has an effect (it prints, sets a global), so it runs as often as before;
- its code uses an aggregate anywhere, in any statement
  (`(fn [x] (let y x) (- y (avg y)))`, `(fn [x] (let n (count x)) (% x n))`),
  or a helper it calls does, or one is passed to it (`(f avg i)`): the
  aggregate is the group's, as `(- i (avg i))` gives it inline;
- its code uses a whole-column verb (`(fn [x] (let y x) (reverse y))`), in
  the same ways: the verb sees each group's surviving rows, as `(reverse i)`
  does inline. These are the verbs above and `rotate`, `xrank`, `xasc`,
  `xdesc`, `group`, `take` and `drop`;
- or its call over the whole table fails or gives anything but one typed
  value per row (a scalar, a LIST). That call is a probe: its cost is paid
  once over the whole table before the helper runs per group.

A running call in the arguments of a helper that runs per group still runs
over the whole table: given `(sums i)`, the reversing helper above reverses
each group's surviving rows of the whole-table `sums`. A running call in the
helper's own body runs with the helper, over the group's rows:
`(fn [x] (let y (sums x)) (reverse y))` and
`(fn [x] (let s (sums x)) (- s (avg s)))` sum each group. The same holds for
a plain output of such a helper: it is the helper over each group's rows.
Over the table `r` above:

```lisp
(select {s: (sums score) from: r by: room})                         ; A: [60 125], B: [80 160]
(select {s: (last (sums score)) from: r by: room})                  ; s: [125 160]
(select {s: (sum (sums score)) from: r by: room where: (> score 30)}) ; s: [185 160]
```

The running sum over all four rows is `60 80 125 160`; room A keeps rows 1
and 3, room B rows 2 and 4. For a running column per group, use `update`
with `by:`, which runs the function over each group's rows:

```lisp
(update {s: (sums score) from: r by: room})   ; s: [60 20 105 55]
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

An output that projects a key column as itself (`sym: sym`) is the key
column: it is not repeated after the keys, and the result is exactly the
select without the projections, the same columns and the same rows in the
same order. The keys come first, in `by:` order, wherever the projections
are written, with one key or with several: `{total: (sum qty) sym: sym}
by: sym` gives the columns `sym total`, and `{n: (count qty) k: k}
by: {k: sym}` gives `k n`. A select whose only outputs are key projections
is the select with no outputs. A key projected under another name (`x: sym`)
is an ordinary output containing each group's key values. A computed projection of a key is an
ordinary output.

```lisp
(select {from: trades by: sym total: (sum qty) sym: sym})   ; columns sym total
(select {from: trades by: [sym side]
         sym: sym side: side total: (sum qty)})   ; columns sym side total
(select {from: trades by: [sym side]
         total: (sum qty) side: side sym: sym})   ; columns sym side total
```

Expressions over aggregates work with any number of keys and with binary aggregates such as `pearson_corr`, `cov`, or `wsum`; the aggregates are computed in the grouping pass and the outer expression once per group:

```lisp
(select {from: trades spread: (- (max price) (min price)) by: {sym: sym venue: venue}})
(select {from: trades r2: (pow (pearson_corr price size) 2) by: {sym: sym}})
```

A `by:` name that reads no column — a lambda parameter or a `let` (which
hides a column of its name), or a global holding a vector — groups by the
values it holds, under its own name. It must hold one value per row of the
table; a key of any other length raises `length`, under a `where:` too (the
key meets the rows the filter keeps):

```lisp
(set t (table [g x] (list [a a b b b c c c c c] (til 10))))
(set K [1 1 1 1 1 2 2 2 2 2])
(select {s: (sum x) by: K from: t})                        ; K [1 2], s [10 35]
((fn [g] (select {s: (sum x) by: g from: t})) K)           ; the same: g is K
(set K3 [1 2 3])
(select {s: (sum x) by: K3 from: t})                       ; error: length
```

### Aggregate sources

What an aggregate reads depends on what its argument is. A column, or an
expression over columns, gives one value per row, and each group aggregates
its own rows. An argument that reads no column follows one of two rules,
depending on whether it is written as a value or computed.

A keyless select gives `first` and `last` the argument `by:` gives its one
group, whichever route evaluates it: builtin calls over the columns, such as
`(first (at s (til 4)))` or `(first (except s 'b))`, are evaluated over the
whole column and their error is the query's (`(last (if c s d))` with SYM
and DATE arms raises `if: branches mix SYM and DATE`); over an empty
`where:` the answer is the null of the argument's type, `0Nd` for a DATE
lambda, `0x00` for U8; and an answer that is a collection, such as the cell
of a LIST column, is one cell: `(first l)` over `(list (list 1) …)` is
`(list (list 1))`.

A vector written as a value — a literal, a global holding one, or a bound
value: a lambda parameter or a `let`, which hides a column of its name —
stands for a column:

- a vector as long as the table is sliced per group, and filtered by
  `where:` like any column (the table's rows before the filter);
- a one-element vector, like an atom, is spread over the rows: `(sum [7])`
  and `(sum 7)` over ten rows are `70`;
- a vector of any other length raises a `length` error;
- `count` keeps a vector's own length: `(count [1 2 3])` is `3` in every
  group;
- `count (distinct v)` of a bound vector counts the distinct values of the
  whole collection, the same in every group, whatever its length or name.
  A helper whose body reads a column, itself or through another helper,
  reads the rows: `(count (distinct (f)))` with `f` = `(fn [] y)` counts
  each group's values of `y`.

```lisp
(set t (table [g x] (list [a a b b b c c c c c] (til 10))))
(set W (* 10 (til 10)))
(select {s: (sum W) by: g from: t})
; g s
; - ---
; a  10
; b  90
; c 350
(select {s: (sum W) by: g from: t where: (> x 2)})   ; s [70 350]
(select {s: (sum [1 2 3]) by: g from: t})            ; error: length
((fn [v] (select {d: (count (distinct v)) by: g from: t})) [1 1 2 2 3 3 4 4 5 5])
; d [5 5 5]
```

A computed argument that reads no column — `(til 5)`, `(at [1 2 3] 0)`, an
expression of constants, a call of a single-expression helper included — is
evaluated once for the whole query, not once per row or per group, so a side
effect in it runs once. A helper with several statements is not inlined.
Given no argument that reads the rows, it runs once per group; given one, it
is called once over the whole table unless one of the exceptions in
[Running functions under `where:`](#running-functions-under-where) sends it
per group. A collection it
returns is aggregated as it stands, as outside a select, and gives the same
value in every group; a scalar stands for every row, as `1` does in
`(sum 1)`, which counts the rows:

```lisp
(select {s: (sum (til 5)) by: g from: t})        ; s [10 10 10]
(select {s: (count (til 5)) by: g from: t})      ; s [5 5 5]
(select {s: (sum (at [1 2 3] 0)) by: g from: t}) ; s [2 3 5], the group sizes
```

The two rules answer differently for equal values: over the ten rows above,
`(sum (til 3))` is `3` while `(sum [0 1 2])` raises `length`, and
`(sum (take [7 8] 1))` is `7` while `(sum [7])` is `70`.

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

A sort key names a column of the result. A key that names an output sorts by
that output's values, computed or renamed, even when a source column has the
same name; the answer is the one `xasc`/`xdesc` gives on the unsorted select. A
source column is used only when no output has the key's name, and it is not
added to the result. That fallback applies to a plain select only: in a select
with `by:`, or one whose outputs are all aggregates, the result rows are groups
(or one row), so a sort key must name an output. A key that names only a
source column there is not supported, e.g. `(select {r: (sum a) from: t by: k
asc: c})`. A key missing from the sortable result raises a `schema` error
that includes the key, for example `sort key 'zz' is not available to this SELECT`:

```lisp
(set t (table [a b] (list [3 1 2] [0 2 1])))
(select {r: (+ a 1) from: t asc: r})   ; r: [2 3 4]
(select {b: a from: t asc: b})         ; the output b: [1 2 3]
(select {c: a from: t asc: b})         ; the source b: c is [3 2 1]
```

The sort is stable: rows that tie on every sort key keep the order in which
they first appear in the source, and in a grouped select the group whose first
source row comes first wins a tie. A `take:` after the sort therefore returns
the same rows on every run, on in-memory, splayed and parted tables alike.

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

An aggregate in an update with `where:` is computed over the rows the
`where:` keeps, as in a `select` with the same `where:`, whether it stands
alone, inside a larger expression or inside a helper. Running functions
(`sums`, `deltas`, `prior` and the rest) run over the whole table, as in
[Running functions under `where:`](#running-functions-under-where), and the
kept rows take their values. Rows the `where:` drops keep their old value
(a new column is null there):

```lisp
(set r (table [name room score] (list ['Ann 'Bob 'Cid 'Dan] ['A 'B 'A 'B] [60 20 45 35])))
(update {s: (max score) from: r where: (== room 'A)})               ; s: [60 0Nl 60 0Nl]
(update {s: (- score (avg score)) from: r where: (== room 'A)})     ; s: [7.5 0Nf -7.5 0Nf]
(update {s: (sums score) from: r where: (== room 'A)})              ; s: [60 0Nl 125 0Nl]
(update {s: (- (sums score) (max score)) from: r where: (== room 'A)}) ; s: [0 0Nl 65 0Nl]
```

The named (`from: 'r`, in place) and value (`from: r`) spellings give the
same values.

### Column types

An updated column keeps its type, with or without `where:` and `by:`. A value of the column's type is stored as it is. A value of a type that widens into it without loss is stored at the column's type:

- `BOOL` and `U8` widen into any wider integer or float;
- `I16` widens into `I32`, `I64`, `F32` or `F64`;
- `I32` widens into `I64` or `F64`;
- `I64` widens into `F64`;
- `F32` widens into `F64`.

So `(update {v: 7 from: t by: k})` over an `F64` column `v` gives `7.0` in every row. Any other type is a `type` error: an `F64` value into an `I64` column, an `I64` value into an `I32` column, or an integer into a `DATE` column. Cast explicitly to store it, for example `(update {salary: (as 'I64 (* salary 1.1)) from: 't})`. A `LIST` column takes any value. A new column takes the value's type.

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
    Supported `select` and `update` expression shapes compile to a lazy DAG. The optimizer applies predicate pushdown, filter reordering, operator fusion, and dead-code elimination before the fused morsel-driven executor processes data in 1024-element chunks. Other expression shapes can use language-evaluator fallbacks to preserve their semantics; not every expression is fused.

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
and takes the largest. An aggregate of a value that reads no row of the
group, such as one taken from another query, `(first (first (at (select
{from: u by: k x: (reverse v)}) 'x)))`, may be combined with the group's
rows or aggregates, `(+ v …)` or `(+ (sum v) …)`, when that aggregate is
`first`, `last`, `min`, `max`, `avg`, `med`, `mode`, `all` or `any`, which
answer the same whether the value is taken once or spread over the rows,
and when its source is a collection, as the reversed `v` there is.
`sum`, `count` and the others of such a value, any of them aggregated
alone, and any aggregate whose source is one value of another query,
`(first (first N))` with `N` a column of sums, still raise `domain`. And an output may not mix a source column with a reference to an earlier
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
symbol. The bare name `null` is the null value inside a select, update,
`where:`, `take:` or `by:` (a key expression too), as outside a query, unless
the table has a column named `null`: `(if (> v 1) v null)` keeps each row's
`v` above 1 and is null elsewhere, and `by: (if (> v 2) g null)` groups the
rows at or below 2 under a null key. A name that is neither an alias nor a
column and holds a table or a dict reads that global: `(count W)` with `W` a
table or a dict counts it. A name holding a list (a `LIST`, such as
`(list 10 20 30)`) is one whole value too, at any length, on every route:
- `r: L` gives every row, or every group, the whole list, in `select` and in
  `update` alike, as an atom fills every row;
- an aggregate sees the whole list, so `(count L)` is the list's length in
  every group;
- `where: L` is the `type` error any value that is no mask gives, and
  `where: (first L)` filters by the mask `L` holds;
- `by: L` is the `schema` error a table or a dict there gives (`update`:
  `domain`), since a whole value is no key.

In `update`, a whole list into a typed column, or a new column of it under
`by:` or `where:`, is the `type` error any `LIST` value gives there. A name
holding a vector is read differently: a vector as long as the table is a
column, sliced per group and filtered by `where:`
([Aggregate sources](#aggregate-sources)). A vector is one value per row of
one type, so it lines up with the rows; a list holds arbitrary values (a
cell may itself be a vector), so it is never taken to be rows. To read a
list's elements as a column, make it a vector first, e.g. `(as 'I64 L)`. Inside a nested select, a column of its own table, and
within its outputs its own outputs, are its own, even when the outer table
has a column of that name (its table given by name, as a parameter or a
`let`, or as an expression of builtins): in `(count (at (select {from: u by: k x: (sum v)}) 'x))` over a table
that also has a `v`, the nested `v` is `u`'s, and the count is of the nested
result, the same in every group. A name only the outer table has is the
outer column there, and so is a name in its `where:` that only one of its
outputs has.  Arithmetic on a symbol — a literal or a symbol column — is a `type`
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

The verb is the builtin the name holds, whatever it is spelled: a global
alias `(set rv reverse)`, a lambda parameter or a `let` holding `reverse`
reverses the column as `(reverse price)` does, and a name holding any other
function keeps its row-by-row call (#1810).

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

Without `by:` the verb also reads the whole column when it sits under a call
(`(+ 1 (reverse v))`) or is the last form of a `do`, as it does in `update` and
under `by:`. A helper the planner does not inline (one whose body has more than one
statement, `(fn [x] (let y 1) (reverse x))`, named or written in place as a
lambda literal) is called with the column, not a row, when its body applies a
whole-column verb to its argument: to a parameter, or to a value the body
derives from one through `let`s and calls (the latest `let` of a name decides,
so a parameter rebound to a constant no longer counts, nor does a name rebound
inside a nested `do`, an `if` arm or another argument). The verb may stand at the top of a
statement, under a call, in a `let`'s value, in a `do`'s last form, or in a
helper the body calls with such a value. In a `select` projection such a helper
receives the whole column (the rows `where:` keeps), as it does in `update` and
under `by:`, so it gives what the verb written in place gives; an effect in it
happens once, not once per row. Otherwise the helper keeps its per-row call,
and a verb in it sees one cell: a closure, a verb only reached through an `if`
arm, a verb applied to a constant or a global
(`(fn [x] (let k (reverse [1 2])) (+ x (count k)))` adds 2 to each row's
cell), a verb
passed in as an argument, and a lambda bound by a `let` in the projection and
then called, `(do (let h (fn [x] (let y 1) (reverse x))) (h b))`
([#1654](https://github.com/RayforceDB/rayforce/issues/1654)).

The same holds for an aggregate: a helper the planner does not inline whose
body applies an aggregate to its argument, by the same rule, means what its
body written inline means. In a keyless select it is evaluated once over the
whole column (the rows `where:` keeps), so `(fn [x] (let q 1) (sum x))` gives
one value, one row alone and spread beside a row output as an inline `(sum h)`
is, and a body that keeps the rows, `(fn [x] (let q 1) (- x (avg x)))`, gives
the column `(- h (avg h))` gives; under `by:` it gives each group's value, and
in `update` the value written to every row. Inside such a body called from a
query, `first` and `last` of a value of the query's rows skip nulls, as the
query's own do, so `(fn [x] (let q 1) (first x))` gives the first non-null
cell on every route. A closure, an aggregate reached only through an `if` arm,
one applied to a constant or a global, or passed in as an argument, keep the
per-row call ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)).

```lisp
(set t (table [g h] (list [0 1 0 1 0 1] [0N 2 3 0N 5 6])))
(set H (fn [x] (let q 1) (first x)))
(at (select {v: (H h) from: t}) 'v)         ; [2]
(at (select {v: (H h) from: t by: g}) 'v)   ; [3 2]
(at (update {v: (H h) from: t}) 'v)         ; [2 2 2 2 2 2]
```

A call whose head is computed rather than written as a name, such as a
dictionary lookup `((at D 'f) i c)`, a helper's result `((rf) i)` or `first`
of a list `((first (list reverse)) i)`, means the call of the function the head
gives. In a select without `by:`, a head that reads no column and has no
effect is evaluated once. A builtin it gives is called as if written by its
name, when that name holds it there. A lambda it gives with nothing captured
(no closure) is called as its text written in place. So
`(sum ((at D 'f) i c))` with `D` holding `filter` gives what
`(sum (filter i c))` gives, and `(first ((at D 'r) i))` under a `where:`
gives the first of the kept rows reversed, not one cell. Any other computed
head is called row by row: one that reads a column, or a name the select
itself binds (an earlier output, a `let`), has an effect, fails, or gives a
closure, and every computed head of a select that can have an effect in any
clause (a helper that sets a global, a lambda the head gives that does) ([#1815](https://github.com/RayforceDB/rayforce/issues/1815)).

```lisp
(set T (table [i c] (list [10 20 30] [true false true])))
(set D (dict ['f 'r] (list filter reverse)))
(at (select {s: (sum ((at D 'f) i c)) from: T}) 's)               ; [40]
(at (select {r: (first ((at D 'r) i)) from: T where: (> i 10)}) 'r)  ; [30]
```

Element-wise builtins work row by row on every route. The text renderers
`format`, `repr` and `.json.format` are element-wise: over a column each gives
one string per row in a projection, in `update`, in a `by:` output (each
group's rows) or key, in an aggregate's argument and in `where:`
([#1594](https://github.com/RayforceDB/rayforce/issues/1594)).

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

An `or` of `==` on one column against literals of its own type,
`(or (== sym 'AAPL) (== sym 'MSFT) ...)`, nested or written as one `or`, is
planned as that column's `in`, `(in sym ['AAPL 'MSFT ...])`: the column is
read once, however many values there are. This covers integer, temporal and
symbol columns. It does not cover an `or` over different columns, a float
column, a null literal, or a literal symbol that names a column (which reads
that column). Those keep the `or`, with the same rows.

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
`where`, `filter`, `raze`) are different: they see the surviving rows only, and under `by:` each
group's surviving rows. An aggregate of one, `(sum (filter i c))` by `g`,
aggregates what the verb gives over the group's rows: the group's values of
`i` where `c` holds, `count`, `distinct` and the other aggregates included
(#1803).

So does every other expression of an output that reads a column whole:
`fold` over a column (`fold-left`, `fold-right` and `fold-while` too, and a
helper that folds), `at` of a column at any index (a constant, a vector or
a column of indexes), and an aggregate inside a call such as
`(til (count v))`. What "the rows" are depends on the route:

- under `by:`, the group's rows: `(- v (fold + v))` subtracts each group's
  own total, `(- v (at v 0))` each group's first value, `(at v i)` indexes
  within the group, and `(til (count v))` counts each group;
- keyless, every row the `where:` keeps (all rows without one):
  `(fold + v)` is the sum of the kept rows, one value as an aggregate is,
  and so is an expression built from such values, `(+ (fold + v) 1)`;
  over no rows the output has no rows, as before;
- `update` with `by:`, the group, its answer written back to its rows;
  `update` with `where:`, the kept rows, written back to them.

Each answers what the same expression gives outside the query on those rows'
values, save that a query's `first` and `last` skip nulls, and that an
aggregate of an aggregate of the group is still refused (`(count (til
(count v)))` by `g` raises `domain`). `at` over a vector that is no column
is a lookup and reads that vector whole, its index the group's rows:
`(sum (at W i))` reads `W` at the group's `i` (#1492); so is `at` of a
column at an index read from such a vector, `(at v (- (div W 100) 1))`,
which picks rows of the whole column by position. A
`fold` over such a vector as long as the table folds the group's slice of it,
as `sum` sums it: `(fold + W)` by `g` is `(sum W)` by `g` (see Aggregate
sources).

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

An output named like a key must be that key. An output that takes the name of
a `by:` key (a key column, or an alias of a by-dict such as `k` in
`by: {k: (+ a 1)}`) but holds anything else, a computed value, an aggregate,
another column or a quoted symbol, is a `domain` error that names the output:
the result would otherwise have two columns of that name, and only the key
could be reached by it. Give the output another name. A key computed without a
name (`by: (+ a 1)`) does not clash: when an output takes the name it would get,
the key is called `key`.

```lisp
(select {from: trades by: sym sym: (count qty)})            ; error: domain, output `sym`
(select {from: trades by: [sym side] side: (sum qty)})      ; error: domain, output `side`
(select {from: trades by: {k: sym} k: (sum qty)})           ; error: domain, output `k`
(select {from: trades by: sym n: (count qty)})              ; columns sym n
```

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

A keyless `prod` whose rows are all null is the empty product, `1` (`1.0`
for a float input), as the builtin `(prod [0Nl 0Nl])` is, with or without
`where:`. Under `by:` a group whose rows are all null keeps the null
(`0Nl`, `0Nf`) for `prod`, as for `min`, `max`, `first`, `last` and `avg`:

```lisp
(set t (table [k c] (list [0 0 1 1] [2 3 0Nl 0Nl])))
(select {p: (prod c) from: t where: (== k 1)})              ; p [1]
(select {p: (prod c) from: t by: k})                        ; k [0 1], p [6 0Nl]
```

### Aggregate sources

What an aggregate reads depends on what its argument is. A column, or an
expression over columns, gives one value per row, and each group aggregates
its own rows. A set verb over a column, `except`, `sect` or `union`, gives
no value per row; under `by:` it is applied to each group's rows, so
`(count (except s 'b))` counts each group's cells other than `b` and
`(sum (except v 10))` sums each group's values other than 10. An argument that reads no column follows one of two rules,
depending on whether it is written as a value or computed.

A keyless select of aggregates answers one row whatever rows it reads.
Over a table with no rows, with or without `where:`, and over a `where:`
that keeps no row, each aggregate gives its answer over no values:
`count` `0`, `sum` `0`, `prod` `1`, `all` `true`, `any` `false`, and the
null of its type for `min`, `max`, `avg`, `first`, `last`, `med` and the
deviations, the same row on a plain or a parted table (a parted table
whose partitions are all empty included). A select with `by:` over
no rows has no groups and gives no row:

```lisp
(set e (take (table [g v] (list [a b] [1 2])) 0))
(select {s: (sum v) n: (count v) m: (min v) from: e})   ; s [0], n [0], m [0Nl]
(select {s: (sum v) by: g from: e})                     ; no row
```

A keyless select gives every aggregate the argument `by:` gives its one
group, whichever route evaluates it: builtin calls over the columns, such as
`(first (at s (til 4)))`, `(sum (at c (til 4)))` or `(count (except s 'b))`,
and calls of a lambda the planner does not inline, which gets the column,
are evaluated over the whole column and their error is the query's (`(last (if c s d))` with SYM
and DATE arms raises `if: branches mix SYM and DATE`); over an empty
`where:` the answer is the null of the argument's type, `0Nd` for a DATE
lambda, `0x00` for U8, and `0Nl` for `first` or `last` of a LIST column, as
`(first (list))` is; and an answer that is a collection, such as the cell
of a LIST column, is one cell: `(first l)` over `(list (list 1) …)` is
`(list (list 1))`. `fold` and `at` of a column read the kept rows, as
[Running functions under `where:`](#running-functions-under-where) says:
`(sum (at c [3 2 1 0]))` is the sum of the kept rows at those indexes. A scalar the argument gives stands for every row on
every keyless route, as `1` does in `(sum 1)`: `(sum (count v))` over four rows
is `16` whether or not another output is declined, and over a `where:` that
keeps no row `(max (count v))` is `0Nl`, the empty column's. So does a
scalar that a helper the planner does not inline returns, here and under
`by:`, where it stands for each row of the group: with `hq` giving the
count of its argument in two statements, `(sum (hq v))` over the same four
rows is `16`, `9` over a `where:` keeping three, and under `by:` each
group's count times its rows ([#1576](https://github.com/RayforceDB/rayforce/issues/1576)).
`update` spreads a scalar the same way, on every route
([#1682](https://github.com/RayforceDB/rayforce/issues/1682)): with `C`
bound to `4` over five rows, `(update {n: (sum C) from: t})` writes `20` to
every row; under a `where:` keeping four rows, `16` to those rows; under
`by:`, each group's `4` times its rows (`8` and `12` for groups of 2 and 3).
Whether an argument is one value is decided by what it gives when it runs,
so every form `select` spreads spreads here: inline code (`(sum (count v))`),
a helper's scalar (with or without arguments), a cast, an `at`, an `if`, a
quoted symbol (`(count 'x)` is the row count). A null scalar gives
`select`'s answers too (`(prod 0Nl)` is null under `by:`). So an aggregate
gives the same value per row or group in `update` as in `select`.
That includes `select`'s refusal under `by:` of an aggregate of an aggregate
of the group: `(update {n: (sum (count v)) from: t by: k})` raises
`domain`.

A vector written as a value — a literal, a global holding one, a global a
single-expression helper reads, or a bound value: a lambda parameter or a
`let`, which hides a column of its name — stands for a column wherever an
aggregate reads its rows: as the aggregate's argument (both arguments of
`wavg`, `wsum`, `cov`, `scov` and `pearson_corr`), through every call in it
that maps that argument element by element, as the source of
`distinct`, as the vector `fold` folds, and as either operand of `filter`,
whose values and mask pair up row by row: `(sum (filter W c))` by `g` sums
each group's values of `W` where `c` holds, as `(sum (filter w c))` does over
a column `w`. A call maps an argument element by element when, as the
builtin is applied there, each element gives one result from itself alone:
arithmetic and comparisons, `if`, `and`, `or`, `not`, `nil?`, binary
`(min W 30)` and `(max W 30)` (the one-argument forms are aggregates), a
cast's value `(as 'F64 W)`, `xbar`, `neg`, `abs`, `round`, `floor`, `ceil`,
`sqrt`, `log`, `exp`, `pow`, `fill`, `div`, `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, `reciprocal`, `signum`, the date and time parts (`yyyy`,
`mm`, `dd`, `hh`, `minute`, `ss`, `dow`, `doy`), a timestamp's `date` and
`time`, `trunc`'s values, `upper`, `lower`, `trim`, `strlen`, `sym-name`,
`substr`'s, `replace`'s and `split`'s text, the texts of `.emb.tokens`, and
the tested side of `in`,
`not-in`, `like`, `str-find` and `within`. There, in every
aggregate, `count` included, in `select` and `update`, keyless, under
`where:` and under `by:`:

- a vector as long as the table is sliced per group, and filtered by
  `where:` like any column (the table's rows before the filter):
  `(count W)` counts each group's rows, `(count (distinct v))` each
  group's distinct values, `(max (+ W (count x)))` adds each group's
  count to its own slice;
- a one-element vector, like an atom, is spread over the rows: `(sum [7])`
  and `(sum 7)` over ten rows are `70`, and `(count [7])` is `1`, as
  `(count 7)` is, even over a one-row table;
- a vector of any other length raises a `length` error, `count` included:
  `(count [1 2 3])` over ten rows is an error, as `(sum [1 2 3])` is
  (count kept the vector's own length until 2026-11; a computed
  collection such as `(count (til 3))` still counts its values, see
  below); over an empty table nothing of the vector is read:
  `(count (til 0))` and a bound empty vector both count `0`.

The rule is by length alone: over a three-row table with `g` `[a b b]`,
`(sum [1 2 3])` by `g` is `[1 5]`, while over ten rows it is a `length`
error. A list is never read this way: a name holding one is one whole
value (see above). A vector meant as one whole value that happens to be as long as the
table is read as a column too; read it through a call that takes it whole,
`(sum (raze W))`, or a helper of several statements. An earlier output's alias is that output, not a vector of
the same name: in `{W: (* x 2) s: (sum W)}` the sum reads the projected
`W`. A helper's global is never an alias.

Lookups and sets are read whole: neither side of a lookup is sliced unless
it is a column. The source and the index of `at` (`(sum (at W [0 1]))`,
`(sum (at W I))` with `I` a table-length vector), the list and the needles
of `find`, the breaks and the values of `bin` and `binr`, the set of `in` and
`not-in`, the pattern of `like` and `str-find`, the bounds of `within`, and
the operands of `concat`, `raze`, `take`, `asc`, `reverse` and every other
call that does not map element by element are the whole vector, the same
in every group. To index per row, put the index in a column:
`(sum (at W i))` with `i` a column reads `W` at each row's index. A helper
whose body reads a column, itself or through another helper, reads the
rows: `(count (distinct (f)))` with `f` = `(fn [] y)` counts each group's
values of `y`. Projections, outputs with no aggregate, are read as before.

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
(select {s: (count W) by: g from: t})                ; s [2 3 5]
(select {s: (count W) from: t where: (> x 4)})       ; s [5]
(update {s: (sum W) from: t where: (> x 4)})         ; s [0N 0N 0N 0N 0N 350 350 350 350 350]
(select {s: (sum [1 2 3]) by: g from: t})            ; error: length
(select {s: (count [1 2 3]) by: g from: t})          ; error: length
((fn [v] (select {d: (count (distinct v)) by: g from: t})) [1 1 2 2 3 3 4 4 5 5])
; d [1 2 3]
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

The probability of `quantile` and `percentile` is not a source: it is one
value for the whole aggregate, as the builtin's second argument is. It is
evaluated once over the rows the aggregate reads, each group's under `by:`
and the rows `where:` keeps in a keyless select, and checked as the builtin
checks it. A literal, a bound name, `(+ P 0)`, a helper call and
`(first t)` each give one probability. A column gives one per row and is
the builtin's `type` error, as a vector is; a null, or a value outside
`0..1` (`0..100` for `percentile`), is its `domain` error. None of them
answers the median instead. Over no rows (an empty table, or a `where:`
that keeps none) there is no row for the probability to be wrong about:
the select answers as for a valid probability, `[0Nf]` over an empty
table and under such a `where:` alike, and `by:` gives no groups.

```lisp
(set q (table [s t] (list [0.0 8.0 16.0 24.0] [0.1 0.2 0.3 0.4])))
(select {v: (quantile s (first t)) from: q})                ; v [2.4]
(select {v: (quantile s (first t)) from: q where: (> s 5)}) ; v [11.2]
(select {v: (quantile s t) from: q})      ; error: type
(select {v: (quantile s 1.25) from: q})   ; error: domain
```

A keyless aggregate of two arguments, `cov`, `scov`, `wsum`, `wavg`,
`pearson_corr`, `top`, `bot`, `quantile` or `percentile`, answers the
builtin's value over the rows, whatever else the select asks for. `top`
and `bot` give one cell, the list the builtin returns. Over no rows `cov`,
`scov`, `wavg` and `pearson_corr` give `0Nf`, `wsum` gives `0.0`, and
`top` and `bot` give an empty list:

```lisp
(set w (table [c f v] (list [1 5 3] [1.5 2.0 4.0] [10 20 30])))
(select {k: (top c 2) from: w})                    ; k (list [5 3])
(select {r: (cov c f) from: w where: (> v 99)})    ; r [0Nf]
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

There is one exception for now: when the group keys are integer, temporal or
symbol columns, the order of groups that tie on the sort keys is not
guaranteed, because the engine may emit such groups in key order
([#1678](https://github.com/RayforceDB/rayforce/issues/1678)). If the order of
ties matters, add the group key as a last sort key, as in
`(select {r: (count v) from: t by: k asc: [r k] take: 10})`.

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

So `(update {v: 7 from: t by: k})` over an `F64` column `v` gives `7.0` in every row. Any other type is a `type` error: an `F64` value into an `I64` column, an `I64` value into an `I32` column, or an integer into a `DATE` column. Cast explicitly to store it, for example `(update {salary: (as 'I64 (* salary 1.1)) from: 't})`.

Integer arithmetic yields `I64`, so to update an `I32`, `I16` or `U8` column, write the literal in the column's type, as in `(update {qty: (+ qty 1i) from: 't})`, or cast the result.

A `LIST` value into a typed column is a `type` error. A `LIST` column keeps its type too: a `LIST` value is stored as it is, and an atom or a typed vector is boxed into it cell by cell, so `(update {l: [1 2 3] from: t})` gives a `LIST` of three `i64` atoms. The same holds with `where:` and `by:`: under `where:` the kept rows take their cell of the value (an atom is boxed into each) and the others keep theirs, so `(update {l: 7 from: t where: (== k 1)})` boxes `7` into the rows of `k` 1; under `by:` each group's result is boxed into its rows, whatever its type. `first` or `last` of a `LIST` column is one cell for the group (or for the rows), as in a select, so every row of the group holds that cell, a vector included: with `U: (table [g l] (list ['a 'a 'b] (list [1 2] [3] [4])))`, `(update {l: (first l) from: U by: g})` gives `(list [1 2] [1 2] [4])`; a `where:` that keeps no row leaves the column as it is. A row-wise value, such as `(at l i)` with `i` a column, gives each row its own cell, its index counting within the group under `by:`. `(at l 0)`, a `LIST` column at a one-value index, is one cell of the group (under `by:`) or of the kept rows, as `first` is, held by every one of those rows. A new column under `where:` from `first` or `last` of a `LIST` column is a `type` error (a new column under `where:` must be a typed vector), whatever the cell's length: `(update {w: (last l) from: U where: (== g 'b)})` raises it even when the cell is `[4]`. A cell that is an atom is a value of its own type and fills the kept rows as any atom does. A new column takes the value's type.

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

### Key Matching

A key matches by value, and **a null key matches a null key** for every key type (integers, floats, temporals, `SYM`, `STR`, `GUID`), in memory and splayed, for single and compound keys. Upserting a null key twice therefore appends one row and then updates it. For `F32` and `F64` keys only the null matches the null; other values compare by value.

```lisp
(set t (table [d v] (list (as 'DATE [2024.01.01 0Nd]) [1 2])))
(upsert 't 1 (list 0Nd 9))
(at t 'v)                          ; [1 9] — the null-keyed row is updated
```

A key value that does not fit the key column is a `domain` error and nothing is written, rather than being narrowed into a duplicate key:

```lisp
(set t (table [k v] (list (as 'I16 [1 2]) [1 2])))
(upsert 't 1 (list 65537 9))       ; error: domain
(upsert 't 1 (list [3 65537] [8 9])) ; error: domain — the 3 is not written either
```

See [Query Operations](../language/functions.md#query-operations) for the full list of key types.

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
| `select` | `(select {from: t col: expr where: pred by: key asc: col desc: col take: n})` | Query table with optional filter, projection, grouping, and aggregation |
| `update` | `(update {col: expr from: 't where: pred})` | Add or modify columns: `{from: 't …}` updates the named table in place, `{from: t …}` returns a new table |
| `insert` | `(insert t rows)` or `(insert 't rows)` | Append rows, or grow a parted table's in-memory live tail with an explicit partition key |
| `upsert` | `(upsert t key-idx rows)` | Insert or update flat-table rows by key match (target, key, row); a null key matches a null-keyed row for every key type, and a key value the key column cannot hold is a `domain` error |
| *delete rows* | `(select {from: t where: (not pred)})` | Remove matching rows via inverse filter |

!!! note "Execution model"
    Supported `select` and `update` expression shapes compile to a lazy DAG. The optimizer applies predicate pushdown, filter reordering, operator fusion, and dead-code elimination before the fused morsel-driven executor processes data in 1024-element chunks. Other expression shapes can use language-evaluator fallbacks to preserve their semantics; not every expression is fused.

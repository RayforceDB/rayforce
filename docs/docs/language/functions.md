# Functions Reference

Complete reference for all Rayfall built-in functions. Each entry shows the function name, arity type (unary/binary/variadic), flags, description, and example usage.

!!! note "Legend"
    Functions marked **atomic** auto-map element-wise over vectors. Functions marked **aggr** reduce vectors to scalars. Functions marked **special** receive unevaluated arguments.

## Arithmetic

Arithmetic operators auto-map over vectors and broadcast scalars. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

| Function | Type | Description | Example |
|---|---|---|---|
| `+` | binary | Addition | `(+ 3 4)` → `7` |
| `-` | binary | Subtraction | `(- 10 3)` → `7` |
| `*` | binary | Multiplication | `(* 3 4)` → `12` |
| `/` | binary | Float division (always returns f64) | `(/ 7 2)` → `3.5` |
| `%` | binary | Modulo (remainder) | `(% 7 3)` → `1` |
| `div` | binary | Floor division, promoted type | `(div 7 2)` → `3` |
| `neg` | unary | Negate value | `(neg 5)` → `-5` |
| `round` | unary | Round to nearest integer | `(round 3.7)` → `4.0` |
| `floor` | unary | Floor (round down) | `(floor 3.7)` → `3.0` |
| `ceil` | unary | Ceiling (round up) | `(ceil 3.2)` → `4.0` |
| `abs` | unary | Absolute value | `(abs -7)` → `7` |
| `sqrt` | unary | Square root (returns f64) | `(sqrt 9)` → `3.0` |
| `log` | unary | Natural logarithm | `(log 2.718)` → `~1.0` |
| `exp` | unary | Exponential (e^x) | `(exp 1)` → `2.718...` |
| `sin` | unary | Sine, radians; returns f64 | `(sin 0.0)` → `0.0` |
| `asin` | unary | Arcsine, radians; out-of-domain values return `0Nf` | `(asin 1.0)` → `1.570...` |
| `cos` | unary | Cosine, radians; returns f64 | `(cos 0.0)` → `1.0` |
| `acos` | unary | Arccosine, radians; out-of-domain values return `0Nf` | `(acos 1.0)` → `0.0` |
| `tan` | unary | Tangent, radians; returns f64 | `(tan 0.0)` → `0.0` |
| `atan` | unary | Arctangent, radians; returns f64 | `(atan 1.0)` → `0.785...` |
| `reciprocal` | unary | Reciprocal `1/x`; zero returns `0Nf` | `(reciprocal 4)` → `0.25` |
| `signum` | unary | Sign as i64: `-1`, `0`, or `1`; null stays null | `(signum -3.5)` → `-1` |
| `pow` | binary | Power (x^y, returns f64; lowers in query expressions) | `(pow 2 10)` → `1024.0` |
| `xbar` | binary | Round down to nearest multiple (bucketing) | `(xbar [3 7 12] 5)` → `[0 5 10]` |

Vector examples:

```lisp
(+ [1 2 3] [10 20 30])   ; [11 22 33]
(* [1 2 3] 10)            ; [10 20 30]  scalar broadcast
(neg [1 -2 3])             ; [-1 2 -3]
(sqrt [4 9 16])             ; [2.0 3.0 4.0]
(sin [0.0 1.57079632679])   ; [0.0 1.0]
(reciprocal [2 0 4])        ; [0.5 0Nf 0.25]
(signum [-2.5 0.0 4.0])     ; [-1 0 1]
(xbar [3 15 27] 10)      ; [0 10 20]
```

## Comparison

All comparison operators are **atomic** and return boolean results.

| Function | Type | Description | Example |
|---|---|---|---|
| `>` | binary | Greater than | `(> 5 3)` → `true` |
| `<` | binary | Less than | `(< 3 5)` → `true` |
| `>=` | binary | Greater than or equal | `(>= 5 5)` → `true` |
| `<=` | binary | Less than or equal | `(<= 3 5)` → `true` |
| `identical` | binary | Whole-value equality: same type, shape and contents; returns one boolean | `(identical 1 1i)` → `false` |
| `==` | binary | Equal | `(== 3 3)` → `true` |
| `!=` | binary | Not equal | `(!= 3 4)` → `true` |
| `within` | binary | Check which elements fall within an inclusive `[lo hi]` range | `(within [1 5 10] [3 7])` → `[false true false]` |

## Logic

| Function | Type | Description | Example |
|---|---|---|---|
| `and` | binary | Logical AND; element-wise on vectors and lists, each element by its truthiness (zero, null and NaN are false), in a query too (lengths must match) | `(and true false)` → `false`, `(and [1 0 2] [1 1 0])` → `[true false false]` |
| `or` | binary | Logical OR; element-wise on vectors and lists, each element by its truthiness, in a query too (lengths must match) | `(or true false)` → `true` |
| `not` | unary | Logical NOT; element-wise over a BOOL, numeric or temporal vector (zero and null are not true) | `(not true)` → `false`, `(not [1 0 0N])` → `[false true true]` |

## Aggregation

Aggregation functions are marked **aggr** and reduce vectors to scalar values. Used in `select` with `by:` for group-by aggregation. A query recognises an aggregate by the function a call's head holds, not by its spelling: after `(set total sum)`, `(total price)` aggregates as `(sum price)` does, and so does a parameter or a `let` holding `sum`; a name spelled like an aggregate but holding another function, `((fn [sum] (select {from: t r: (sum i)})) desc)`, calls that function. A column named like an aggregate (`count`, `max`, `first`) does not hide it: `(max price)` still aggregates.

| Function | Type | Description | Example |
|---|---|---|---|
| `sum` | unary, aggr | Sum of all elements | `(sum [1 2 3])` → `6` |
| `prod` | unary, aggr | Product of all non-null numeric elements; empty/all-null returns `1` | `(prod [2 3 4])` → `24` |
| `all` | unary, aggr | True if every non-null numeric element is truthy; empty/all-null returns `true` | `(all [1 2 3])` → `true` |
| `any` | unary, aggr | True if any non-null numeric element is truthy; empty/all-null returns `false` | `(any [0 0 3])` → `true` |
| `count` | unary, aggr | Count of elements | `(count [1 2 3])` → `3` |
| `avg` | unary, aggr | Arithmetic mean | `(avg [1 2 3])` → `2.0` |
| `min` | unary aggr, or binary atomic | Reduce one vector, or choose the element-wise minimum of two values, in their promoted type (null lowest); f32 input answers f32 | `(min [3 9 1] [2 10 4])` → `[2 9 1]` |
| `max` | unary aggr, or binary atomic | Reduce one vector, or choose the element-wise maximum of two values, in their promoted type (null lowest); f32 input answers f32 | `(max [3 9 1] 5)` → `[5 9 5]` |
| `med` | unary, aggr | Median value (returns f64); a vector of another type than numbers and temporals is a type error, empty or not | `(med [1 3 2])` → `2.0` |
| `mode` | unary, aggr | Most frequent non-null value; ties keep the first encountered value | `(mode [1 2 2 3])` → `2` |
| `dev` | unary, aggr | Population standard deviation; a vector of another type than numbers and temporals is a type error, empty or not | `(dev [1 2 3])` → `0.816...` |
| `stddev` | unary, aggr | Sample standard deviation (Bessel-corrected; not an alias of `dev`); a vector of another type than numbers and temporals is a type error, empty or not | `(stddev [1 2 3])` → `1.0` |
| `dev_pop` | unary, aggr | Population standard deviation | `(dev_pop [1 2 3])` |
| `var` | unary, aggr | Sample variance; a vector of another type than numbers and temporals is a type error, empty or not | `(var [1 2 3])` → `1.0` |
| `var_pop` | unary, aggr | Population variance ([accuracy](../language/math.md#variance-and-standard-deviation-accuracy)) | `(var_pop [1 2 3])` |
| `pearson_corr` | binary, aggr | Pearson correlation over paired non-null numeric inputs | `(pearson_corr [1 2 3] [2 4 6])` → `1.0` |
| `cov` | binary, aggr | Population covariance over paired non-null numeric inputs | `(cov [1 3] [2 6])` → `2.0` |
| `scov` | binary, aggr | Sample covariance over paired non-null numeric inputs | `(scov [1 3] [2 6])` → `4.0` |
| `wsum` | binary, aggr | Weighted sum, `sum(weights * values)`, skipping null pairs | `(wsum [1 3] [10 20])` → `70.0` |
| `wavg` | binary, aggr | Weighted average, `wsum / sum(weights)`; null for zero total weight | `(wavg [1 3] [10 20])` → `17.5` |
| `quantile` | binary, aggr | Exact linear-interpolated quantile; probability is `0.0..1.0`; a vector of another type than numbers and temporals is a type error, empty or not | `(quantile [1 2 3 4] 0.5)` → `2.5` |
| `percentile` | binary, aggr | Exact linear-interpolated percentile; probability is `0..100` | `(percentile [1 2 3 4] 50)` → `2.5` |
| `first` | unary, aggr | First element of a vector. A direct call takes the element by position, nulls included, for every type: `(first [0Nl 1])` is `0Nl`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(first s)` over `["" "x"]` is `"x"`, a direct `(first ["" "x"])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(first (+ d 1))`, `(first (if c d 0Nd))`); a computed argument that reads no column, such as `(first (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) | `(first [10 20 30])` → `10`, `(first [0Nl 1])` → `0Nl` |
| `last` | unary, aggr | Last element of a vector. A direct call takes the element by position, nulls included, for every type: `(at v (- (count v) 1))`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(last s)` over `["x" ""]` is `"x"`, a direct `(last ["x" ""])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(last (+ d 1))`, `(last (if c d 0Nd))`); a computed argument that reads no column, such as `(last (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) | `(last [10 20 30])` → `30`, `(last [1 2 0Nl])` → `0Nl` |

Numeric reducers skip nulls where applicable. Pairwise binary reducers skip a row when either vector input is null. Inside `select`/`by:`, aggregate reducers lower to morsel-based DAG paths that can run in parallel and poll for cancellation.

The examples below use this small in-memory table:

```lisp
(set trades (table [sym price size time]
  (list [AAPL GOOG AAPL]
        [150.0 280.0 151.0]
        [100 50 200]
        [10 20 15])))
```

```lisp
; Group-by aggregation example
(select {from: trades
         by:   {sym: sym}
         hi: (max price) lo: (min price) n: (count price)})
```

## Linear Algebra

A matrix is a list of equal-length numeric vectors, one per row. F64 and F32 rows are used in place; I32 and I64 rows are widened to F64, their nulls becoming `0Nf`. `inv` and `lsq` raise `domain` for an input that holds a null.

| Function | Type | Description | Example |
|---|---|---|---|
| `mmu` | binary | Matrix product: matrix × matrix, matrix × vector, vector × matrix; two vectors give their dot product (an F64 atom) | `(mmu (list [1.0 2.0] [3.0 4.0]) [1.0 1.0])` → `[3.0 7.0]` |
| `inv` | unary | Inverse of a square matrix; a singular matrix raises `domain` | `(inv (list [1.0 2.0] [3.0 4.0]))` → `[[-2.0 1.0] [1.5 -0.5]]` |
| `lsq` | binary | `(lsq b a)` is `b mmu inv a` for a square `a`; for an `a` with more columns than rows, the least-squares solution; a rank-deficient `a` raises `domain` | `(lsq [1.0 2.0] (list [2.0 1.0] [1.0 3.0]))` → `[0.2 0.6]` |
| `flip` | unary | Transpose of a matrix; rows of one type keep it, mixed rows widen to F64 | `(flip (list [1 2 3] [4 5 6]))` → `[[1 4] [2 5] [3 6]]` |
| `gram` | unary | `x mmu flip x` without forming the transpose; symmetric, computed on the upper triangle | `(gram (list [1.0 2.0 3.0] [4.0 5.0 6.0]))` → `[[14.0 32.0] [32.0 77.0]]` |

The result is F32 when every operand is F32, otherwise F64 (the dot product of two vectors is always an F64 atom). `mmu` and `gram` compute in the operands' own precision: F32 rows multiply and sum in single precision, F64 rows in double. `inv` and `lsq` always solve in double. A result outside the range of its type is null. See the [analytics cookbook](../guides/analytics.md#linear-regression) for a regression with `lsq` and `gram`.

## Higher-Order Functions

Functions that take other functions as arguments.

| Function | Type | Description | Example |
|---|---|---|---|
| `map` | variadic | `(map f v)` applies `f` to each element of `v`; `(map f val v)` calls `f(val, elem)` for each element, passing `val` whole each time (a collection `val` gives one row per element; same as `map-right`). Same-type atom results form a typed vector, otherwise a list. String results that repeat shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fail with `limit`. Exactly 2 or 3 arguments, else `domain` | `(map (fn [x] (* x 2)) [1 2 3])` → `[2 4 6]`, `(map + 100 [1 2 3])` → `[101 102 103]` |
| `pmap` | variadic | `map` with the calls run in parallel on the pool's workers when the function is proven pure and the calls are worth it; the same result, result types, error and 2 or 3 arguments as `map`, else `domain` | `(pmap (fn [x] (* x x)) [1 2 3])` → `[1 4 9]` |
| `filter` | binary | Keep elements where boolean mask is true | `(filter [1 2 3 4] (> [1 2 3 4] 2))` → `[3 4]` |
| `fold` | variadic | Reduce with a function, from an initial value or else the first element | `(fold + 0 [1 2 3])` → `6` |
| `fold-left` | variadic | Left-associative fold | `(fold-left - 10 [1 2 3])` → `4` |
| `fold-while` | variadic | Fold that stops when the accumulator fails pred | `(fold-while (fn [a] (< a 100)) + 0 (til 1000))` → `105` |
| `fold-right` | variadic | Right-associative fold | `(fold-right - 10 [1 2 3])` → `-8` |
| `scan` | variadic | Running fold (all intermediate results) | `(scan + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-left` | variadic | Left-to-right running fold | `(scan-left + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-right` | variadic | Right-to-left running fold (returns a list) | `(scan-right + (enlist 1 2 3))` → `(6 5 3)` |
| `apply` | variadic | `(apply f a b)` calls `f` on the elements of two collections taken in step, `f(a_0, b_0)`, `f(a_1, b_1)`, ...; the result is as long as the shorter one. Results are collected as `map`'s are. Two atoms call `f` once. Exactly 3 arguments, else `domain` | `(apply + [1 2 3] [10 20 30])` → `[11 22 33]` |
| `map-left` | variadic | Map each element of the left over the whole right | `(map-left + 10 [1 2 3])` → `[11 12 13]` |
| `map-right` | variadic | Map the whole left over each element of the right | `(map-right - [10 20 30] 5)` → `[5 15 25]` |

`map`, `pmap` and `apply` return a typed vector when every result is an atom of
exactly the same vector-supported type, including strings, symbols and
representable typed nulls. Native boolean/byte null atoms remain boxed
because those vector types are non-nullable. Opaque graph, HNSW index,
embedding model and Datalog program handles also remain boxed: their integer payload alone cannot
represent the handle's resource ownership. Mixed atom types (including
integers mixed with floats), nested results and untyped `null` results
remain a `LIST`. Empty input returns an empty `LIST`, since there is no
result type to infer. A builtin that takes a typed vector takes that empty
`LIST` too: it gives what it gives an empty vector when every type it takes
gives the same answer (`(where (list))` is `[]`, as `(where (as 'B8 []))`
is), the empty `LIST` itself when its vector answer would take a type from
its input (`(asc (list))`, `(sums (list))`), and `avg` gives `0Nf`; a
non-empty `LIST` is still rejected. A binary map with a scalar third argument still calls
the function once and returns its result directly. Callbacks run left to
right, exactly once per input element; an error stops the walk. A later type
mismatch converts the collected prefix to a list without repeating
callbacks. `scan` and the other iterators keep their existing result-type
rules. This changes callers that relied on every map result being a `LIST`.
Use `(concat (list) result)` to explicitly box a result for a list-only
consumer, such as a positional row payload or `raze` joining string fragments.
`raze` leaves an already typed vector unchanged.

A symbol result is packed as a name. A `SYM` vector holds only each cell's
symbol, not a quote, so a quoted symbol (one `parse` returns for `'a`, or
one a lambda returns as `'a`) loses its quote when `map` packs it beside
other symbols. Mapping over parsed code can therefore change what it
evaluates to: the cell is now the name `a`, and evaluating it looks the name
up. Boxing the packed result with `(concat (list) result)` comes after the
packing and does not bring the quote back:

```lisp
(set L (list (parse "'a") (parse "b")))
(set M (map (fn [x] x) L))
(type L)            ; 'LIST
(type M)            ; 'SYM
(set b 42)
(eval (first L))    ; 'a
(eval (first M))    ; error: name: 'a' undefined
(eval (at M 1))     ; 42
```

To keep quoted symbols, build the result as a `LIST` so nothing is packed:
fold with `concat` into a list, or have the function wrap each result in a
list and read it back with `first`:

```lisp
(set R (fold (fn [acc x] (concat acc (list x))) (list) L))
(type R)                          ; 'LIST
(eval (first R))                  ; 'a
(set W (map (fn [x] (list x)) L))
(eval (first (first W)))          ; 'a
```

## Collection Operations

Operations on vectors as collections.

Outside queries, `in` matches null to null. `not-in` inverts each membership result, preserving nested list/vector shapes. For numeric, temporal, SYM, and GUID membership evaluated during a query (including called functions), null input rows return `false` for both operations. STR membership is not supported by the typed query-plan kernel. A string atom tested against a list is one whole value and returns one boolean: `(in "ab" (list "ab" "c"))` → `true`. A string tested against a string returns one result per character: `(in "ab" "abc")` → `[true true]`. A vector or list of strings is tested element by element: `(in ["p" "qq"] (list "qq"))` → `[false true]`. In a query, a `SYM` column against a string, a string vector or a list of strings reads each string as the symbol it spells, as `==` does: `(select {from: t where: (in s (list "a" "b"))})` keeps the rows holding `'a` or `'b`; each string is looked up in the column's own symbols (a string no row holds matches nothing), and `""` selects the null rows, as `(== s "")` does, whether the set is written out, bound to a name or computed while the query runs (`(in s (concat ["a"] [""]))`, `(in s (f "a"))`); a `GUID` column against strings is a `type` error, as for `==`.

Query `concat` joins text row by row when every vector operand comes from a source column, including computed text columns and column arguments to inlined helpers. Scalar text operands also retain query string concatenation. A literal or bound collection operand keeps ordinary collection concatenation, in either argument order. Inside a user function `concat` keeps ordinary collection semantics unless an operand derives from the query's rows. Numeric collection concatenation evaluates over the whole column, or each group inside an aggregate. Non-text collection concatenation is binary. Scalars broadcast to the projected collection length; a one-row input column is still a column. Non-scalar output columns of different lengths are rejected before `take:`; a standalone collection projection may change the row count. When a `where:` selection is applied to a collection projection, its rows must align with the input rows. Counting a computed constant collection, such as `(count (til 3))`, returns its length, independently of the source row count or group size; a written vector (a literal, a global, a parameter or a `let`) is read as a column instead, so one as long as the table counts each group's rows and one of another length is a `length` error ([Aggregate sources](../queries/select.md#aggregate-sources)). In a projection, a `concat` operand the planner has no node for (a reordered, indexed or cast column such as `(reverse s)`, `(at s 0)` or `(as 'STR x)`) is evaluated over the whole column, so select, update, `where:` and `by:` agree. Inside a helper body a quoted symbol is the symbol itself, as when the helper runs as bytecode, and a nested helper's free names are globals or source columns, never its caller's formals; a helper whose body uses only pure builtins is inlined; a closure never is, since its body reads its captures. Grouped outputs that the planner cannot serve natively keep the requested output order, and an output named like a group key keeps both columns, the key first. A helper that the planner cannot inline (several statements, a closure, recursion) runs as bytecode when called from a query fallback; `return`, lazy arguments and recursion depth behave as outside a query, and each value carries whether it derives from the query's rows, so `concat` keeps text semantics through `let` bindings, nested calls and helper results while a bound vector stays a collection. Inside such a helper a free name that is a source column is the row's cell during row-by-row evaluation and the whole column otherwise; an output alias never captures it, and other free names are globals. In a `select` projection such a helper is called once per row, except that one that is no closure (named, or a lambda literal) and whose body applies a whole-column verb (`reverse`, `asc`, `distinct`, ...) to its argument outside an `if` arm receives the whole column, as in `update` and under `by:` ([whole-column projections](../queries/select.md#whole-column-projections)). One whose body applies an aggregate to its argument the same way is that aggregate, as written inline: one value in a keyless select (spread beside a row output), each group's value under `by:`, and inside such a body `first` and `last` of the query's rows skip nulls ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)). Such a helper can also be the `by:` key, evaluated over the rows `where:` keeps: a body built from pure builtins and pure helpers runs whole first, and again row by row when a conditional in it tested a vector or the whole-column run failed (a failed run is swallowed whole, its `raise` payload included); a body with an effect runs row by row from the start, so the effect happens once per row. The decision follows each callee wherever it is bound (a global helper, a parameter, a `let`, a capture, a lambda literal bound earlier in the body): `set`, `eval`, system and I/O builtins and a callee that is computed or unknown are effects; every other builtin and every helper built from them is pure, and a pure body keeps the whole-column semantics whichever way it is reached. A source column shadows a global of the same name inside a helper, on the planned path and in fallbacks alike. The same holds for a helper a higher-order builtin calls inside a query (`map`, `pmap`, `apply`, `fold`, `scan`, `prior` and their `-left`, `-right` and `-while` forms), whether it is named or fetched at run time, such as `(map (at (value (env 0)) k) [1 2])`: it sees the column, or the group's rows under `by:`, as the direct call does ([#1762](https://github.com/RayforceDB/rayforce/issues/1762)); in an output with no aggregate it does too, as the lambda literal with its body does, and so does a lambda literal calling such a helper ([#1772](https://github.com/RayforceDB/rayforce/issues/1772)). That holds whatever the length of the result: `(map f [1 2 3 4])` over a four-row table is each group's call too, not the whole table's value split by row; only an output whose columns are all read row by row (`(+ a c)`, `(if (> a 2) c a)`) is evaluated once and split ([#1832](https://github.com/RayforceDB/rayforce/issues/1832)). Without `by:`, a call through a computed head (`((at D 'f) i c)`, `((rf) i)`) that reads no column or name the select binds, in a select with no effect in any clause, is the call of the builtin or uncaptured lambda the head gives, written in place; under `by:` a head giving a builtin outside an aggregate's arguments is too, and one left as written runs per group ([computed heads](../queries/select.md#whole-column-projections), [#1815](https://github.com/RayforceDB/rayforce/issues/1815), [#1772](https://github.com/RayforceDB/rayforce/issues/1772)). A text cell reduced from a column (`first`, `last`, `min`, `max`) is a cell of those rows: in a grouped select or update `(concat (first s) 'zz)` joins each group's cell with the literal, and an empty symbol cell is empty text, as for the cell itself; a cell reduced from a STR column makes the result STR, as the column does.

| Function | Type | Description | Example |
|---|---|---|---|
| `distinct` | unary | Remove duplicates, keeping each value at its first occurrence, in the order the values first appear, for every type, list cells included; a null is kept once; `-0.0` equals `0.0` in float vector cells, as between atoms | `(distinct [3 1 3 2])` → `[3 1 2]` |
| `in` | binary | Membership test; a string in a list is one whole value | `(in "ab" (list "ab" "c"))` → `true` |
| `not-in` | binary | Negated membership test | `(not-in 2 [1 3])` → `true` |
| `except` | binary | Elements of A not in B. Not a set: the duplicates of A are kept | `(except [1 1 2 3] [3])` → `[1 1 2]` |
| `union` | binary | Set union: distinct elements of the left operand, then those of the right it lacks, in order of first appearance | `(union [1 2] [2 3])` → `[1 2 3]` |
| `sect` | binary | Elements of A that are also in B. Not a set: the duplicates of A are kept | `(sect [1 1 2 2 3] [1 2])` → `[1 1 2 2]` |
| `take` | binary | Take first N elements (negative N takes from end); past the length it cycles (`select … take:` stops at the table's length). A float or boolean count is a `type` error, as for `drop`. A parted table handle (`.db.parted.get`) is taken as its flat table: the same rows `(at table (til n))` returns, cycling past the row count | `(take [10 20 30] 2)` → `[10 20]`; `(take [1 2 3] 5)` → `[1 2 3 1 2]` |
| `drop` | binary | Drop first N elements (negative N drops from end) | `(drop [10 20 30] 1)` → `[20 30]` |
| `rotate` | binary | Rotate left by N positions (negative rotates right) | `(rotate [1 2 3 4] 1)` → `[2 3 4 1]` |
| `cut` | binary | Split a collection at sorted 0-based indices | `(cut [1 2 3 4] [2])` → `([1 2] [3 4])` |
| `cross` | binary | Cartesian product as pairs | `(cross [1 2] ['a 'b])` → `((1 'a) (1 'b) (2 'a) (2 'b))` |
| `at` | binary | Index a collection; vector keys batch dictionary lookups. A typed vector indexed by an integer vector keeps its type: a null or out-of-range index gives the type's null (`false`/`0x00` for BOOL/U8). A string takes the same keys: an integer atom gives a one-character string, a collection of them a string, `(at "boom" [1 3])` is `"om"`, and an empty key of any type the empty string; any other key, a boolean or a date included, is the same `type` error as for a vector (`at: index must be an integer, got sym`), and an out-of-range, negative or null position on a string is a `domain` error | `(at [10 20 30] 1)` → `20`, `(at [10 20 30] [2 9])` → `[30 0Nl]` |
| `find` | binary | Find index of first occurrence | `(find [10 20 30] 20)` → `1` |
| `fill` | binary, atomic | Replace null values; the replacement is the first argument; f32 input stays f32. A replacement of another numeric or temporal type is cast to the vector's type first, as `(as 'T r)` casts it, so the result is the vector's type whichever cell comes first: `(fill 3 [0Nf 1.5])` is `[3.0 1.5]`, `(fill 2.5 [0N 1])` is `[2 1]` (a float casts to an integer, DATE, TIME or TIMESTAMP toward zero, and to a TIMESPAN to the nearest nanosecond, half away from zero), and `(fill true [0N 1 2])` is `[1 1 2]`. A null atom follows the same rule: `(fill 2.5 0N)` is `2`, so a select projection that fills row by row answers as the vector does. A replacement `as` cannot cast to the vector's type (a DATE into a TIMESPAN vector or a TIMESPAN into a DATE, TIME or TIMESTAMP one; `as` casts a TIME or TIMESTAMP into a TIMESPAN), or one of another kind (a number with a GUID, symbol or string), gives the `LIST` of each cell, whichever comes first: `(fill 3 ["" "ab"])` is `(list 3 "ab")` | `(fill 0 [1 0Nl 3])` → `[1 0 3]` |
| `reverse` | unary | Reverse element order; a table reverses its rows (columns and types kept), a dictionary its entries (keys and values together) | `(reverse [1 2 3])` → `[3 2 1]` |
| `til` | unary | Generate range [0..n) | `(til 5)` → `[0 1 2 3 4]` |
| `lag` | unary | Shift values one row back; first row is null/sentinel | `(lag [10 20 30])` → `[0Nl 10 20]` |
| `lead` | unary | Shift values one row forward; last row is null/sentinel | `(lead [10 20 30])` → `[20 30 0Nl]` |
| `deltas` | unary | Adjacent differences, of the type `-` gives: TIMESTAMP → TIMESPAN, TIME → TIMESPAN, DATE → I32 (days), TIMESPAN → TIMESPAN, other integers → I64, floats → F64; first row is null | `(deltas [10 15 13])` → `[0Nl 5 -2]` |
| `ratios` | unary | Adjacent ratios as f64; first row is null | `(ratios [2 4 8])` → `[0Nf 2.0 2.0]` |
| `fills` | unary | Forward-fill nullable vectors, or every column of a table | `(fills (as 'I64 (list 0N 2 0N)))` → `[0Nl 2 2]` |
| `sums` | unary | Running sum; nulls are skipped | `(sums [1 2 3])` → `[1 3 6]` |
| `avgs` | unary | Running average over non-null values | `(avgs [2 4 6])` → `[2.0 3.0 4.0]` |
| `mins` | unary | Running minimum; f32 input stays f32 | `(mins [3 1 2])` → `[3 1 1]` |
| `maxs` | unary | Running maximum; f32 input stays f32 | `(maxs [3 1 2])` → `[3 3 3]` |
| `prds` | unary | Running product; nulls are skipped | `(prds [2 3 4])` → `[2 6 24]` |
| `differ` | unary | Boolean change flag versus previous row; first row is true | `(differ [1 1 2])` → `[true false true]` |
| `msum` | binary | Moving sum over trailing N rows; nulls are skipped | `(msum 3 [1 2 3 4])` → `[1 3 6 9]` |
| `mavg` | binary | Moving average over trailing N rows and non-null values | `(mavg 3 [1 2 3 4])` → `[1.0 1.5 2.0 3.0]` |
| `mmin` | binary | Moving minimum over trailing N rows; f32 input stays f32 | `(mmin 3 [3 2 4 1])` → `[3 2 2 1]` |
| `mmax` | binary | Moving maximum over trailing N rows; f32 input stays f32 | `(mmax 3 [3 2 4 1])` → `[3 3 4 4]` |
| `mcount` | binary | Moving non-null count over trailing N rows | `(mcount 3 [1 2 3 4])` → `[1 2 3 3]` |
| `mvar` | binary | Moving population variance over trailing N rows and non-null values | `(mvar 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `mdev` | binary | Moving population standard deviation over trailing N rows and non-null values | `(mdev 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `enlist` | variadic | Wrap value(s) in a vector (list of atoms) | `(enlist 1 2 3)` → `[1 2 3]` |
| `concat` | binary | Join two atoms, vectors or lists, mixed types giving a list; an atom joins a vector only of its own type, and a string joins each cell of a `STR` vector (`(concat "ab" ["c"])` → `["abc"]`, so `(concat "ab" [1 2])` is a `type` error); merge two dicts; append the rows of a table to a table that has each of its columns, of the same types (a parted table is refused: use `union-all`). The STR null `""` is empty text, in a direct call and on every query route: `(concat "" "a")` is `"a"`, and `(concat s "")` in a select keeps each cell of `s` | `(concat [1 2] [3 4])` → `[1 2 3 4]` |
| `raze` | unary | Flatten a list of vectors into one vector; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(raze (list [1 2] [3 4]))` → `[1 2 3 4]` |
| `where` | unary | Indices where boolean vector is true | `(where [true false true])` → `[0 2]` |
| `group` | unary | Group indices by value (returns dict) | `(group [A B A])` → dict of groups |
| `diverse` | unary | Check if all elements are unique (no duplicates) | `(diverse [1 2 3])` → `true` |
| `rand` | binary | N random integers from 0 up to, not including, `max`; **deterministic**: the same sequence in every process | `(rand 3 100)` → 3 random ints 0..99 |
| `bin` | binary | Binary search — left boundary (sorted input) | `(bin [10 20 30] 25)` → `1` |
| `binr` | binary | Binary search — right boundary (sorted input) | `(binr [10 20 30] 25)` → `2` |
| `unify` | binary | A two-element list of both values, as given (no merge, no type coercion) | `(unify d1 d2)` |

The time-series vector functions above are lazy-aware DAG operations for vector inputs. Moving-window helpers take a positive integer window first, then the vector. Constant windows inside `select` lower into DAG nodes; dynamic windows evaluate through the normal function path. These functions materialize through morsel-based kernels, can run in parallel, and poll the query cancellation flag during execution.

## Sorting & Ordering

| Function | Type | Description | Example |
|---|---|---|---|
| `asc` | unary | Sort ascending | `(asc [3 1 2])` → `[1 2 3]` |
| `desc` | unary | Sort descending | `(desc [3 1 2])` → `[3 2 1]` |
| `iasc` | unary | Indices that would sort ascending (grade up) | `(iasc [30 10 20])` → `[1 2 0]` |
| `idesc` | unary | Indices that would sort descending (grade down) | `(idesc [30 10 20])` → `[0 2 1]` |
| `rank` | unary | Rank of each element (0-based) | `(rank [30 10 20])` → `[2 0 1]` |
| `xasc` | binary | Sort table ascending by column(s) | `(xasc trades 'price)` |
| `xdesc` | binary | Sort table descending by column(s) | `(xdesc trades 'price)` |
| `xrank` | binary | Assign N rank buckets (quantile ranking) | `(xrank 4 [10 20 30 40])` → `[0 1 2 3]` |

## Table Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `list` | variadic | A list of its arguments, of any types | `(list [1 2] [A B])` |
| `table` | binary | Create table from column names and list of vectors. A LIST of numeric atoms of one type keeps that type; mixed numeric atoms use the wider type (BOOL < U8 < I16 < I32 < I64 < F64); F32 atoms alone give F32, and F32 mixed with any other numeric type gives F64. Typed nulls stay null except in BOOL and U8 columns, which store false and zero respectively; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(table [x y] (list [1 2] [A B]))` |
| `key` | unary | Get column names (table) or keys (dict) | `(key trades)` → `[sym price size]` |
| `cols` | unary | Get table column names | `(cols trades)` → `[sym price size]` |
| `value` | unary | Get column data (table) or values (dict) | `(value trades)` |
| `dict` | binary | Create dictionary from evaluated keys and values vectors | `(dict [a b] [1 2])` |
| `get` | binary | Lookup key in dict or column in table | `(get d 'a)` → `1` |
| `remove` | binary | Remove key from dictionary | `(remove d 'a)` |
| `row` | binary | Extract single row from table as dict | `(row trades 0)` |
| `meta` | unary | Table metadata as a dict: its type and its column count | `(meta trades)` |
| `xcol` | binary | Rename all table columns; names must match column count | `(xcol trades [ticker px sz])` |
| `xcols` | binary | Project/reorder named columns | `(xcols trades [size sym])` |
| `xkey` | binary | Build a dict keyed by unique column value(s), with row dict values for remaining columns | `(xkey trades 'time)` |
| `xgroup` | binary | Group rows by column value(s), returning dict key -> table slice | `(xgroup trades 'sym)` |
| `fkeys` | unary | Inspect linked-column metadata as `column -> target table` | `(fkeys fact)` |
| `alter` | variadic, special | Amend a named local or global binding and return its name: `(alter 'name set idx val)` writes `val` at an index or index vector (an integer atom or an integer vector, as `at` takes; a float, boolean or symbol index is a `type` error) (for a dict, `idx` is a key or a vector of keys: an existing key is replaced, a missing one is added), `(alter 'name concat vals)` appends, `(alter 'name remove idx)` drops list items, by the same integer index rule. On a typed vector `val` must be of the vector's kind: a number into a numeric vector (converted to the vector's type), otherwise a value of the vector's own type, and a null of any type, or a value vector of nulls only, writes the vector's null; anything else, or a value vector whose length is not the index count, is a `type` error and the vector is unchanged. A SYM vector read from a table saved with a symbol file takes any symbol, held by that file or not; the write stays in memory and the file is not changed. The innermost visible local or parameter takes precedence over a global, through a literal `alter` head or a global alias of it; shared values retain copy-on-write isolation | `(alter 'v set 0 99)` |
| `del` | unary, special | Unbind one variable or dotted-path leaf and return `0`; deleting the last leaf of a dotted path removes the intermediate dicts too. Extra arguments raise `arity` | `(del cfg.db.host)` |
| `delete` | unary, special | Remove the rows a predicate matches: `{from: 't where: p}` removes them in place, `{from: t where: p}` returns a new table. Both clauses are required | `(delete {from: 'trades where: (> price 100)})` |
| `modify` | variadic | Functional table update (returns new table) | `(modify trades 'price (fn [p] (* p 1.1)))` |

`xkey` returns a dictionary keyed by unique key column value(s). Duplicate keys are a domain error; use `xgroup` to get a dictionary whose values are grouped table slices. Passing a typed vector of keys to `at` performs a batch lookup and returns a list of the corresponding values.

Dictionary literals are literals: their entries are not evaluated, just as the
elements of other literals are not evaluated. Use `(dict keys values)` to
construct a dictionary from evaluated expressions.

## Query Operations

These are **special forms** that bridge to the Rayforce DAG executor.

| Function | Type | Description |
|---|---|---|
| `select` | variadic, special | Query table with optional filter, projection, grouping, and aggregation |
| `update` | variadic, special | Add or modify columns: `{from: 't …}` updates the named table in place, `{from: t …}` returns a new table |
| `insert` | variadic, special | Append rows, or grow a parted table's in-memory live tail with an explicit partition key; a quoted target names the innermost visible local or parameter before a global, through a literal `insert` head or a global alias of it |
| `upsert` | variadic, special | Insert or update flat-table rows by key match (target, key, row); a null key matches a null-keyed row for every key type, and a key value the key column cannot hold is a `domain` error |

```lisp
; Select with filter and projection
(select {from: trades
         where: (> price 100)
         sym: sym notional: (* price size)})

; Group-by with multiple aggregates
(select {from: trades
         by:   {sym: sym}
         vwap: (/ (sum (* price size)) (sum size))
                count: (count price)})

; Update: add a column
(update {from: trades
          notional: (* price size)})
```

`insert` has two table forms:

- `(insert target rows)` is the ordinary two-argument form. It returns a new flat table, or rebinds and returns a quoted target symbol such as `'trades`, which names the innermost visible local or parameter before a global, through a literal `insert` head or a global alias of it. The row payload can be a list, table, or dictionary, as described by the target's physical columns.
- `(insert parted partition-key rows)` grows the in-memory live tail of a parted table and returns a fresh logical view, leaving `parted` unchanged. Quote a symbol — `(insert 'parted partition-key rows)` — to grow the table that symbol is bound to and return the symbol; when the binding is the only holder of the table, a same-key insert appends to the current partition in place. A validated zero-row batch is a no-op and returns the existing view without rebinding. This is distinct from the three-argument positional form for vectors and lists, where the second argument is an insertion index.

```lisp
; Append a row to a table
(insert 'trades (list 'AAPL 150.0 100 12))

; Append two rows to the live 2024.01.16 partition of a parted table.
; `date` is virtual, so the payload contains only physical data columns.
(insert 'trades 2024.01.16
  (list ['AAPL 'MSFT] [151.0 410.0] [200 75] [13 14]))

; Vector / list operations
(set v (til 5))               ; [0 1 2 3 4]
(insert 'v 99)                  ; append:    [0 1 2 3 4 99]
(insert 'v 0 -1)                ; head:      [-1 0 1 2 3 4 99]
(insert 'v 3 [100 200])         ; splice:    [-1 0 1 100 200 2 3 4 99]
(insert 'v [0 3] 77)             ; broadcast 77 at pre-positions 0 and 3
(insert 'v [1 3] [10 30])         ; parallel: 10 at pos 1, 30 at pos 3
```

Indices are *pre-insertion* positions in `[0, count]`; `idx == count` is equivalent to append. Vector positional inserts of a same-typed vector splice that vector in. List positional inserts always add the value as a single slot — use `concat` to splice. Multi-insert is stable on duplicate indices, preserving input order. Typed-null atoms (`0Nl`, `0Nf`, …) carry their null flag through — the inserted slot is marked null, not zero.

For a parted target, `rows` must match the physical data-column schema exactly in name and concrete vector type. Do **not** include the virtual `date` or `part` column. A list maps values in physical column order; a table or dictionary may reorder columns, but must contain every physical name exactly once. Atoms append one row. Equal-length vectors append a batch, and atom values alongside them broadcast across that batch. Generic `null` is accepted for sentinel-nullable columns; `SYM` and `STR` map it to their null, which is their empty value (`'` or `""`), while `BOOL` and `U8` reject it because they are non-nullable.

The partition key must equal the current last key, which grows that live segment, or be strictly later, which starts a new live segment. Earlier keys — including an existing historical partition — are immutable and cannot be inserted into. Existing partition metadata must already be strictly increasing in logical key order; integer directory names are ordered by value (`1`, `2`, `10` — no zero-padding needed), so only two spellings of one value (`1` and `01`) violate this, and they are rejected as `corrupt`. Every historical physical segment must be present. A missing segment in the last partition can be repaired only by a non-empty same-key append; it blocks an empty insert or a move to a later key. `BOOL`/`U8` cannot be null-backfilled when that missing segment already represents existing rows, but a missing zero-row segment needs no backfill.

A non-empty functional insert returns a new logical table view and always copies the active segment. Historical mmap segments are retained without copying, while a segment receiving rows becomes heap-backed. Queries and values that already retained the previous table continue to see that snapshot; resolving the target symbol after the insert sees the newly appended rows.

A same-key insert through a quoted symbol appends to the active segment in place when the binding is the only holder of the table: the symbol keeps the same table, and the cost follows the rows appended, not the rows the partition already holds. It copies the active segment and rebinds the symbol instead when the table or one of its columns is held elsewhere (a snapshot bound to another name, a query still running over it), and on the first insert after `.db.parted.get`, whose active segment is still the mapped file. The copy belongs to the binding alone, so the next insert appends in place again. A later key materializes a new tail. A failed insert leaves the table with the rows it had.

!!! warning "Live-tail insert is not persistence"
    Parted `insert` changes memory only. It does not modify partition directories, column files, `.d`, or `.sym`; process exit loses an unpersisted tail. Persist the completed physical partition explicitly at rollover, then reload the parted table if it should return to a fully mmap-backed view. See [`.db.parted.get`](../namespaces/db.md#db-parted-get) for the production pattern.

`upsert` is not supported on parted tables. Materialize a flat table or use an application-level update strategy when key-based replacement is required.

`upsert` into a named flat table (`(upsert 'book 'px row)`) writes matched rows in place and appends new keys, so its cost is proportional to the payload rather than to the rows already present. A multi-row payload (a table, or a list of column vectors) is applied in one pass; a key repeated inside the payload resolves to its last row. A single-row `upsert` locates its row through a `.idx.hash` index on the key column when one is attached and fresh, and by one scan of the key column otherwise. A payload with a mistyped value is rejected before anything is written.

The key column may be `BOOL`, `U8`, `I16`, `I32`, `I64`, `F32`, `F64`, `DATE`, `TIME`, `TIMESTAMP`, `TIMESPAN`, `SYM`, `STR` or `GUID`; a key is matched by value, and the same key updates on a flat in-memory table and on a splayed or mapped one. A `SYM` or `GUID` key column needs a `SYM` or `GUID` key value, and a `STR` key column a `STR` one (otherwise a `type` error). A null key matches an existing null-keyed row for every key type, in memory and splayed, for a single and a compound key, so a second upsert of a null key updates the row the first one appended. A null of another type counts as the key column's null (`0Nl` against a `DATE` key matches `0Nd`); `BOOL` and `U8` cannot hold a null, so a null there is stored and matched as `0`. For `F32` and `F64` keys only the null matches the null: other values compare by value as before. A key value the key column cannot hold is a `domain` error, not narrowed: `65537` against an `I16` key, `257` or `-1` against a `U8` key, `4294967297` against an `I32` key, and the `I16` or `I32` null bit pattern written as a plain number (`-32768`, `-2147483648`). A batch with one such key changes nothing. `F32` keys are compared by a scan of the key column rather than through the hashed fast path.

Payload values are narrowed into the column they land in: an `F32` column accepts `F32`, `F64` and integer atoms (a null stays null), and an `F64` column accepts `F32`, `F64` and integer atoms. A `BOOL` value into a float column is a `type` error, and the table is left unchanged.

`update` with `where:` into a named flat table writes the matched rows in place: the predicate is resolved through a fresh `.idx.hash` index on the column when it is a plain equality (`(== sym 'AAPL)`), elementwise update expressions (arithmetic, comparisons, and other atomic builtins over columns and literals) are evaluated over the matched rows only, and the results are scattered into the live columns. An aggregate is computed over the rows `where:` keeps, as in `select` with the same `where:`, including one inside a larger expression (`(- v (avg v))`) or inside a helper, and a scalar argument stands for every kept row, as in `select` (`(sum C)` with `C` 4 over four kept rows is `16`, [Aggregate sources](../queries/select.md#aggregate-sources)): `(update {v: (max v) from: 't where: (== k 1)})` writes the largest `v` among the rows with `k` 1. Running and order-dependent functions such as `sums`, `deltas` and `prior` run over the whole table, as in `select` ([Running functions under `where:`](../queries/select.md#running-functions-under-where)), and the kept rows take their values; in an expression that mixes both, such as `(- (sums v) (max v))`, the running call sees the whole table and the aggregate the kept rows. A whole-column verb such as `reverse` is read over the whole table too. A written vector an aggregate reads (a global, literal, parameter or `let`) is a column, as in `select`: one as long as the table is read at the kept rows, `(sum W)` summing the kept rows' values of `W`, and one of another length is a `length` error ([Aggregate sources](../queries/select.md#aggregate-sources)). When `where:` keeps no row nothing is written, but every expression is still evaluated over the table, so one that cannot be evaluated raises its error. Both spellings of `from:` agree. A mistyped or wrong-length value is rejected before anything is written.

## Joins

Rayforce supports equi-joins, outer joins, anti-joins, and time-series-aware joins.

| Function | Type | Description |
|---|---|---|
| `left-join` | variadic | Left join — all left rows, unmatched filled with null. Keys are a symbol list |
| `inner-join` | variadic | Inner join — only matching rows from both sides |
| `full-join` | variadic | Full outer join — all rows from both sides, unmatched columns filled with null |
| `anti-join` | variadic | Anti-semi-join — left rows with no right match |
| `semi-mask` | variadic | Boolean vector over left rows — true where the row has a right match |
| `anti-mask` | variadic | Boolean vector over left rows — true where the row has no right match |
| `window-join` | variadic, special | Window join — `[eq-keys... time-key]`, intervals, left, right, agg dict |
| `asof-join` | variadic | As-of join — match most recent preceding value. Keys come first, last key is the time key |

```lisp
; Fixtures for join examples
(set trades_j (table [sym price size time]
  (list [AAPL GOOG AAPL]
        [150.0 280.0 151.0]
        [100 50 200]
        [10 20 15])))
(set quotes (table [sym time bid]
  (list [AAPL GOOG AAPL]
        [9 19 16]
        [149.5 279.5 150.5])))
(set orders (table [product_id qty] (list [10 20 10] [2 1 5])))
(set products (table [product_id name] (list [10 20] [widget gadget])))

; Left join two tables on the sym column (join keys are a symbol list)
(left-join [sym] trades_j quotes)

; Inner join
(inner-join [product_id] orders products)

; Full outer join keeps rows from both sides
(full-join [sym] trades_j quotes)

; Window join: keys are [equality-keys... time-key]; intervals is
; a two-vector list with one [lo hi] window bound per left row.
; For each left row, aggregate the right rows whose time key falls in the window.
(set intervals (map-left + [-2 2] (at trades_j 'time)))
(window-join [sym time]
             intervals
             trades_j quotes
             {avg_bid: (avg bid)})

; As-of join: keys come first, the last key is the time key
(asof-join [sym time] trades_j quotes)
```

## Pivot & Window

| Function | Type | Description | Example |
|---|---|---|---|
| `pivot` | variadic | Pivot table — reshape long to wide format | `(pivot trades 'sym 'date 'price sum)` |
| `xbar` | binary, atomic | Round down to nearest multiple (bucketing) | `(xbar [3 7 12] 5)` → `[0 5 10]` |
| `xrank` | binary | Assign N rank buckets (quantile ranking) | `(xrank 4 [10 20 30 40])` |

```lisp
; OHLC bars: bucket trades by 5-minute intervals
(select {from: trades
         by:   {sym: sym  bucket: (xbar time 300000)}
         open: (first price)
                high: (max price)
                low:  (min price)
                close: (last price)
                vol:  (sum size)})
```

## String Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `split` | binary | Split string by delimiter into a list of strings; a string or symbol vector, or a list of string or symbol atoms, splits each element into its own list | `(split "a,b,c" ",")` → `("a" "b" "c")` |
| `str-find` | binary | First byte index of a literal substring (no wildcards; see `like`); `0Nl` when absent | `(str-find "banana" "na")` → `2` |
| `str-join` | binary | Join string/symbol atoms, vectors, or lists with a delimiter | `(str-join ["a" "b"] ",")` → `"a,b"` |
| `strlen` | unary | Length of each string. `""` is the STR/SYM null: `strlen` maps it to a null length, `sum` and `avg` skip it, and the `avg` of an all-empty group is null | `(strlen "hello")` → `5` |
| `upper` | unary | Uppercase string or symbol atoms/vectors | `(upper ["ab" "Cd"])` → `["AB" "CD"]` |
| `lower` | unary | Lowercase string or symbol atoms/vectors | `(lower 'AbC)` → `'abc` |
| `trim` | unary | Strip leading/trailing whitespace | `(trim "  abc  ")` → `"abc"` |
| `substr` | variadic | Substring by 1-based start and length; 0 clamps to start | `(substr "abcdef" 2 3)` → `"bcd"` |
| `replace` | variadic | Replace all occurrences of a string/symbol atom. An empty `to` deletes each match; an empty `from` matches nothing and returns the input | `(replace "a-b" "-" "_")` → `"a_b"` |
| `like` | binary | Glob-style pattern match: `*` any run, `?` one character, `[abc]`, `[a-z]` or `[!abc]` a character class. The pattern is a string atom, or a string column matched row by row (`(like t pat)`); a symbol or a number is a `type` error | `(like "hello" "hel*")` → `true` |
| `concat` | binary | Join two atoms, vectors or lists, mixed types giving a list; an atom joins a vector only of its own type, and a string joins each cell of a `STR` vector (`(concat "ab" ["c"])` → `["abc"]`, so `(concat "ab" [1 2])` is a `type` error); merge two dicts; append the rows of a table to a table that has each of its columns, of the same types (a parted table is refused: use `union-all`). The STR null `""` is empty text, in a direct call and on every query route: `(concat "" "a")` is `"a"`, and `(concat s "")` in a select keeps each cell of `s` | `(concat "hello" " world")` → `"hello world"` |
| `format` | variadic | Format value to string (% as placeholder); a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`; in a query element-wise: one string per row of a column, on every route | `(format "x=%" 42)` → `"x=42"` |

!!! note "Note"
    String transforms are available as direct builtins and use the same DAG opcodes inside `select`/`update`. All string transformations propagate nulls: null input rows produce null output rows.

## Date & Time

| Function | Type | Description | Example |
|---|---|---|---|
| `date` | unary | Current date, or extract date from timestamp; answers a DATE | `(date 0)` → today's date |
| `time` | unary | Current wall-clock time (millisecond resolution), or extract time from timestamp; answers a TIME | `(time 0)` → current time |

### Calendar/Clock Field Extraction

Unary functions that pull a single calendar or clock field out of a DATE / TIME / TIMESTAMP atom or vector. Null input rows propagate to null output rows (the null sentinel bit pattern is *not* decoded as a bogus year / second). The same set of fields is reachable via dotted access on a temporal value (e.g. `ts.yyyy` — see [Dotted Namespaces](../language/syntax.md)).

| Function | Range | Example |
|---|---|---|
| `yyyy` | year | `(yyyy 2024.03.15)` → `2024` |
| `mm` | 1..12 | `(mm 2024.03.15)` → `3` |
| `dd` | 1..31 | `(dd 2024.03.15)` → `15` |
| `hh` | 0..23 | `(hh 12:34:56)` → `12` |
| `minute` | 0..59 | `(minute 12:34:56)` → `34` |
| `ss` | 0..59 | `(ss 12:34:56)` → `56` |
| `dow` | 1..7 (Mon=1) | `(dow 2024.03.15)` → `5` |
| `doy` | 1..366 | `(doy 2024.03.15)` → `75` |

`mm` is unambiguously MONTH; the minute spelling stays long-form because a two-letter token can't serve both meanings in a uniform dotted walk. `.date` and `.time` answer the whole DATE and the whole TIME of a timestamp.

### Calendar/Clock Truncation

`(trunc 'unit x)` floors a DATE, TIME or TIMESTAMP (atom or vector) to the start of the calendar or clock unit that holds it. The result has the **type of `x`**: a DATE floored to its month is a DATE, a TIMESTAMP stays a TIMESTAMP. Null rows stay null.

| Unit | Start of the... | Takes | Example |
|---|---|---|---|
| `'year` | year | DATE, TIMESTAMP | `(trunc 'year 2024.05.17)` → `2024.01.01` |
| `'quarter` | quarter (Jan, Apr, Jul, Oct) | DATE, TIMESTAMP | `(trunc 'quarter 2024.05.17)` → `2024.04.01` |
| `'month` | month | DATE, TIMESTAMP | `(trunc 'month 2024.05.17)` → `2024.05.01` |
| `'week` | week, **Monday-based** as `dow` counts | DATE, TIMESTAMP | `(trunc 'week 2024.05.17)` → `2024.05.13` |
| `'day` | day | DATE, TIMESTAMP | `(trunc 'day 2024.05.17D13:45:00.000000000)` → `2024.05.17D00:00:00.000000000` |
| `'hour` | hour | TIME, TIMESTAMP | `(trunc 'hour 13:45:12.123)` → `13:00:00.000` |
| `'minute` | minute | TIME, TIMESTAMP | `(trunc 'minute 13:45:12.123)` → `13:45:00.000` |
| `'second` | second | TIME, TIMESTAMP | `(trunc 'second 13:45:12.123)` → `13:45:12.000` |

A calendar unit on a TIME and a clock unit on a DATE are `type` errors (neither carries what the unit needs); an unknown unit is a `domain` error. A TIMESTAMP whose period starts before the TIMESTAMP range (1707.09.23 onward) truncates to null, as does a DATE whose period starts below the DATE range. All calendar arithmetic is UTC.

It works as a `by:` key and in projections, which is how calendar bucketing is written (`xbar` takes a fixed width, so it cannot do months or years):

```lisp
(select {from: Bars
         by:   {m: (trunc 'month ts)}
         open: (first px)  high: (max px)  low: (min px)  close: (last px)})
```

Inside queries the same truncations are available as dotted segments: `ts.year`, `ts.quarter`, `ts.month`, `ts.week`, `ts.day`, `ts.hour`, `ts.second` (`ts.minute` is the extract, `(trunc 'minute ts)` the truncation). Outside a query, use `trunc`.

Cross-temporal comparisons are supported: dates, times, and timestamps are all converted to nanoseconds internally for comparison operations.

## Type Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `type` | unary | Get the type name of a value; a function value answers `UNARY`, `BINARY`, `VARY` or `LAMBDA` | `(type 42)` → `i64` |
| `as` | binary | Cast value to another type; `'LIST` boxes a typed vector into a generic `LIST` of atoms (in a query, the whole column on every route: `select`, `update`, each group of `by:`); a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(as 'i64 "42")` → `42` |
| `nil?` | unary | Test for null; element-wise over vectors and lists (a `B8` per element), the same in and out of queries | `(nil? x)` |
| `rc` | unary | Get reference count of an object | `(rc x)` → `1` |
| `guid` | unary | Generate a vector of N v4 GUIDs, seeded from the OS (`(guid 0)` → `[]`) | `(guid 1)` |

An `as` to I16, I32, DATE or TIME whose result is the target's minimum gives that type's null, so `(as 'I16 [-32768 32768 5])` is `[0Nh 0Nh 5]`: the minimum is the null sentinel, and an integer that wraps onto it when narrowed is null too. A float source clamps to the minimum plus one instead. See [Null Semantics in Casting](../guides/types.md#null-semantics).

`guid` and `rand` draw from **different** sources on purpose:

- `guid` is seeded from the OS at first use, per thread, so values are unique
  across processes and machines. That is the property the type exists to
  provide, so it is not something to trade for reproducibility.
- `rand` is deterministic — the same process-wide sequence every run. That is
  what makes tests and simulations reproducible, and changing `guid`'s seeding
  deliberately left it alone.

```lisp
(first (guid 1))   ;; a different value in every process
(rand 3 1000)      ;; [383 886 777] in every process
```

If you need a reproducible stream, use `rand`. If you need identifiers that do
not collide between two processes writing into the same table, use `guid`.

## I/O & File Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `println` | variadic | Print values with newline; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(println "hello" 42)` |
| `print` | unary | Print value without newline; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(print "hello")` |
| `show` | variadic | Pretty-print a value (tables formatted); a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(show trades)` |
| `format` | variadic | Format value to string (% as placeholder); a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`; in a query element-wise: one string per row of a column, on every route | `(format "val=%" 42)` → `"val=42"` |
| `.csv.read` | variadic | Load CSV file into table (mmap, parallel parse) | `(.csv.read "data.csv")` |
| `.csv.write` | variadic | Write table to CSV file | `(.csv.write trades "out.csv")` |
| [`.parquet.read`](../namespaces/parquet.md#parquet-read) | variadic | Read a Parquet file into a table; optional projection or options before the path | `(.parquet.read [id price] "trades.parquet")` |
| [`.parquet.scan`](../namespaces/parquet.md#parquet-scan) | variadic | Reusable lazy `select` source over a Parquet file; optional options before the path | `(.parquet.scan "trades.parquet")` |
| `read` | unary | Read file contents as string, to EOF | `(read "file.txt")` |
| `read-bytes` | unary | Read file contents as a `U8` byte vector, to EOF | `(read-bytes "file.bin")` |
| `write` | binary | Write a string to a file | `(write "file.txt" "content")` |
| `write-bytes` | binary | Write a `U8` byte vector to a file | `(write-bytes "file.bin" bytes)` |
| `load` | unary | Load and evaluate a Rayfall script file. A relative path is tried against the working directory, then below `RAYFORCE_HOME` when set; absolute paths are used as given; a failure names every path tried | `(load "lib.rfl")` |

`read` and `read-bytes` read until EOF rather than to the size the file
reports. That matters for anything whose reported size is not its content
length: every file under `/proc` and `/sys` reports `0` while yielding data,
FIFOs and character devices cannot be seeked at all, and an ordinary file may
change between the measure and the read.

```lisp
(read "/proc/sys/kernel/hostname")        ;; => "sita\n"
(read "/proc/sys/kernel/random/uuid")     ;; => a fresh UUID per call
(.fs.size "/proc/sys/kernel/hostname")    ;; => 0  (what the kernel reports)
```

`.fs.size` still reports what the OS reports — the two disagreeing on a
special file is expected, not a bug.

A stream with no size to go on — `/dev/zero`, or a FIFO nobody closes — has no
EOF to reach. Reading one grows a buffer until it crosses a ceiling derived
from the heap's remaining headroom, then fails with `io` rather than consuming
the machine. Reads of a file whose size *is* known are not subject to that
ceiling: a reported size is a finite claim, and a large file allocates and
spills exactly as it did before.

## Control Flow

| Function | Type | Description | Example |
|---|---|---|---|
| `set` | binary, special | Bind value to global variable. Returns the value it binds, so a large value assigned through `.ipc.send` comes back whole as the reply: `(.ipc.send h "(do (set x expr) null)")` assigns it and replies `null` (see [Remote assignments](../namespaces/ipc.md#remote-assignments)) | `(set x 42)` |
| `let` | binary, special | Bind value to local variable (lexical scope); at the top level it binds a global, refused for a restricted caller. Returns the value it binds, as `set` does: sent through `.ipc.send`, that value is the reply (see [Remote assignments](../namespaces/ipc.md#remote-assignments)) | `(let y (+ x 1))` |
| `if` | variadic, special | Conditional: (if c1 r1 c2 r2 ... else?) — first truthy condition wins. A boolean-vector condition chooses per row between the two arms, as in a query: both arms are always evaluated (their side effects happen, and an arm that raises raises), arms are vectors of its length or atoms, and a length-1 vector gives a 1-element vector. In a query the arms give one column: numbers promote, a temporal beside a null keeps its type, two different temporal types (DATE, TIME, TIMESTAMP, TIMESPAN) or a temporal beside a number raise `type` ([control flow](../language/control-flow.md#conditionals-if)) | `(if (> x 0) "pos" "neg")`, `(if [true false] [1 2] [3 4])` → `[1 4]` |
| `do` | variadic, special | Sequential execution, returns last value. Inside a query the earlier forms run once per query, over the selected rows, and the last form is planned as if alone, seeing their `let`s ([`do` inside a query](../language/control-flow.md#do-inside-a-query)) | `(do (set x 1) (set y 2) (+ x y))` |
| `fn` | variadic, special | Create lambda function | `(fn [x] (* x x))` |
| `try` | binary, special | Error handling: (try expr handler-fn-or-fallback-value); the handler gets the raised value, or `{code msg}` for an engine error (`{code msg usage}` when it is about a builtin's call) | `(try (+ 1 'a) (fn [e] (at e 'code)))` → `'type` |
| `raise` | unary | Throw an error with message | `(raise "bad input")` |
| `return` | variadic | Early return from the enclosing lambda; with no argument it returns null | `(return 42)` |
| `quote` | variadic, special | Return argument unevaluated; a bare name yields a literal symbol (`(quote x)` ≡ `'x`) | `(quote (+ 1 2))` → `(+ 1 2)` |
| `resolve` | variadic, special | Resolve a symbol in the current scope | `(resolve 'x)` |

## System & Utility

| Function | Type | Description | Example |
|---|---|---|---|
| `eval` | unary | Evaluate a parsed Rayfall expression | `(eval (parse "(+ 1 2)"))` → `3` |
| `parse` | unary | Parse a string into a Rayfall expression tree | `(parse "(+ 1 2)")` |
| `.sys.gc` | variadic (0–1) | Trigger GC / heap flush, returns `0`; mode `1` also releases free pages now, best effort | `(.sys.gc)`, `(.sys.gc 1)` |
| `.sys.exec` | unary | Execute a shell command; exit code, or `{code, out}` with `'out` | `(.sys.exec "ls -la")` |
| `.sys.trust` | unary, restricted | Mark lambdas a restricted (`-U`) caller may run with the restriction lifted, isolated from the caller; returns its argument. See [`.sys.trust`](../namespaces/sys.md#sys-trust) | `(.sys.trust 'peer_count)` |
| `.os.getenv` | unary | Get environment variable value | `(.os.getenv "HOME")` |
| `.os.setenv` | binary | Set environment variable | `(.os.setenv "KEY" "value")` |
| `exit` | unary | Exit the process with status code | `(exit 0)` |
| `timeit` | variadic, special | Evaluate an expression and return the elapsed time in milliseconds | `(timeit (sum (til 1000000)))` |
| `.time.mono` | variadic | Monotonic clock in milliseconds from a platform-defined epoch, for intervals and timer deadlines | `(.time.mono)` |
| `.time.utc` | variadic | Current wall-clock timestamp in UTC, at the system clock's full resolution (nanoseconds on Linux) | `(.time.utc)` |
| `.time.local` | variadic | Current wall-clock timestamp in local time (a timestamp carries no zone), at the system clock's full resolution | `(.time.local)` |
| `.time.timer.set` | variadic, restricted | Schedule callback every `ms`, `num` times (0 = forever); returns id | `(.time.timer.set 1000 0 (fn [t] (println t)))` |
| `.time.timer.del` | unary, restricted | Cancel a scheduled timer by id; returns null | `(.time.timer.del 0)` |
| `.time.sleep` | unary, restricted | Block the calling thread for `ms` milliseconds (an integer) or a timespan; returns null; a REPL Ctrl-C, an IPC cancel, a server-side `kill -INT`, or the client of a sync request hanging up ends it with `cancel`; an async one runs its full time; a journal replay skips it; it stalls a server's other connections while it waits | `(.time.sleep 500)` |
| `.sys.args` | nullary | Application arguments as a typed dict; `user` subdict holds post-`--` args | `(.sys.args)` |
| `env` | unary | List all global environment bindings | `(env 0)` |
| `.sys.build` | nullary | Build metadata dict with `version` + `build-date` | `(.sys.build)` |
| `.sys.mem` | nullary | Memory allocator statistics (alloc / peak / slab hits) | `(.sys.mem)` |
| `.sys.info` | nullary | Host and process info (cores, page size, total memory, workers, pid, hostname) | `(.sys.info)` |

## Serialization & Storage

| Function | Type | Description | Example |
|---|---|---|---|
| `ser` | unary | Serialize any value to binary format (byte vector); a value nested deeper than 512 levels fails with `limit`, the depth `de` reads; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(ser [1 2 3])` |
| `de` | unary | Deserialize from binary format back to value; a list holding an error value (built in C, never in Rayfall, where a caught error is a dict) reads back as its first error, see [A list holding an error value](../storage/ipc.md#a-list-holding-an-error-value) | `(de bytes)` |
| `repr` | variadic | `(repr v)` writes `v` as Rayfall text that reads back to the same value; `(repr v n)` fails with `limit` rather than return more than `n` bytes; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`. See [`repr`](../language/string.md#repr); in a query element-wise: one string per row of a column, on every route | `(repr [1i 2i])` |
| `read-value` | variadic | Reads one value from data text without evaluating anything; an optional dict overrides the limits. See [Reading Data Text](../language/read-value.md) | `(read-value "{a: 1 b: [2 3]}")` |
| `read-limits` | variadic | The default limits of `read-value`, as a dict | `(read-limits)` |
| `.json.parse` | variadic | JSON text to a value (object to dict with string keys, array to a vector or list); element-wise over a string vector; strict, the limits of `read-value`. See [`.json.*`](../namespaces/json.md) | `(.json.parse "[1,2]")` |
| `.json.get` | variadic | One field of each JSON document by a dotted path (or one path per row), without building the rest; an optional type (`'STR`, `'I64`, `'F64`) makes it always that type, null where the field is absent or another kind; optional limits | `(.json.get line "kind")` |
| `.json.format` | unary | A value as JSON text (a table as an array of objects); in a query element-wise: one string per row of a column, on every route | `(.json.format (dict ["a"] (list 1)))` |
| `.db.splayed.set` | variadic | Save table as splayed columns to a directory; optional `{compress: bool, symfile: 'shared/'column, index: bool, durable: bool}`; `{durable: false}` skips the fsyncs; a parted table is written a segment at a time | `(.db.splayed.set "db/trades" trades)` |
| `.db.splayed.append` | variadic | Append a table's rows to a splayed table on disk without loading it; the table has the target's column names and types; optional symbol-file path and `{index: bool, durable: bool}`; no table there: written as by `.db.splayed.set` | `(.db.splayed.append "db/trades" new_trades)` |
| `.db.splayed.get` | variadic | Load splayed table from a directory | `(.db.splayed.get "db/trades")` |
| `.db.parted.get` | variadic | Load partitioned table from root directory | `(.db.parted.get "db" 'trades)` |
| `.db.parted.tables` | variadic | List table names under a parted root (from the most recent partition) | `(.db.parted.tables "db")` |
| `.db.parted.fill` | variadic | Backfill missing tables across a parted db's partitions | `(.db.parted.fill "db")` |

## EAV (Entity-Attribute-Value)

Built-in support for triple stores. The EAV table has three columns: `e` (entity, i64), `a` (attribute, sym), `v` (value, i64).

| Function | Type | Description | Example |
|---|---|---|---|
| `datoms` | nullary | Create an empty EAV table | `(datoms)` |
| `assert-fact` | variadic | Append a triple (e, a, v) to the EAV table | `(assert-fact db 1 'name 100)` |
| `retract-fact` | variadic | Remove a triple from the EAV table | `(retract-fact db 1 'name 100)` |
| `scan-eav` | binary | Query EAV by attribute, returns (e, v) table | `(scan-eav db 'name)` |
| `pull` | binary | Entity-centric retrieval — returns all attributes as dict | `(pull db 1)` → `{name:100 age:30}` |
| `sym-name` | unary | Resolve integer sym IDs to symbols (passthrough for sym atoms) | `(sym-name 0)` |

```lisp
; Build an EAV store
(set db (datoms))
(set db (assert-fact db 1 'name 100))
(set db (assert-fact db 1 'age 30))
(pull db 1)              ; {name:100 age:30}
(scan-eav db 'name)     ; table of (e, v) pairs where a='name
```

## Table Set Operations

| Function | Type | Description | Example |
|---|---|---|---|
| `union-all` | binary | Concatenate two tables (all rows, no dedup). A parted operand (`.db.parted.get`) is read into memory first: the result is a plain table, and the partition key becomes a column | `(union-all t1 t2)` |
| `distinct` | unary | Remove duplicate rows from a table <!-- docs-gen: own --> | `(distinct t)` |
| `anti-join` | variadic | Anti-semi-join — left rows with no right match | `(anti-join [x] t1 t2)` |

```lisp
; Table concatenation and deduplication
(set t1 (table [x] (list [1 2])))
(set t2 (table [x] (list [2 3])))
(union-all t1 t2)          ; 4 rows: 1 2 2 3
(distinct (union-all t1 t2))  ; 3 rows: 1 2 3
(anti-join [x] t1 t2)     ; 1 row: 1
```

## Datalog

Datalog rules and queries integrate with the EAV store. Rules use the `(?entity :attribute ?value)` pattern to match triples.

| Function | Type | Description | Example |
|---|---|---|---|
| `rule` | variadic, special | Define a Datalog rule (head + body clauses) | `(rule (path ?x ?y) (?x :edge ?y))` |
| `query` | variadic, special | Compile and execute a Datalog query against EAV store | `(query db (find ?x ?y) (where (path ?x ?y)))` |
| `not` | special (in Datalog WHERE) | Stratified negation in WHERE clause <!-- docs-gen: own --> | `(not (?y :edge 3))` |

```lisp
; Define rules and query
(rule (path ?x ?y) (?x :edge ?y))
(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))

(set db (datoms))
(set db (assert-fact db 1 'edge 2))
(set db (assert-fact db 2 'edge 3))
(query db (find ?x ?y) (where (path ?x ?y)))
; returns table: (1,2) (1,3) (2,3)
```

## Datalog Program API

Low-level API for building and evaluating Datalog programs directly, bypassing the EAV store.

| Function | Type | Description | Example |
|---|---|---|---|
| `dl-program` | nullary | Create an empty Datalog program | `(dl-program)` |
| `dl-add-edb` | variadic | Register a base relation (table + arity) | `(dl-add-edb prog 'edge tbl 2)` |
| `dl-stratify` | unary | Compute strata for the program (required before eval) | `(dl-stratify prog)` |
| `dl-eval` | unary | Evaluate program to fixpoint (semi-naive iteration) | `(dl-eval prog)` |
| `dl-query` | binary | Query a derived or base relation by name | `(dl-query prog 'edge)` |
| `dl-provenance` | binary | Reserved provenance hook; currently returns `domain: not available` | `(dl-provenance prog 'rel)` |

A `dl-program` handle owns its program. Rebinding its symbol or leaving the handle's scope releases the program automatically; use `(dl-free prog)` to release it earlier.

```lisp
; Low-level Datalog program
(set prog (dl-program))
(set edges (table [x y] (list [1 2 3] [2 3 4])))
(dl-add-edb prog 'edge edges 2)
(dl-stratify prog)
(dl-eval prog)
(dl-query prog 'edge)  ; returns the edge table
```

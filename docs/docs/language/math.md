# Math, Comparison, Aggregation & Iteration

All numeric, logical, aggregation, ordering, generation, and higher-order operations available in Rayfall. Operations marked `FN_ATOMIC` auto-map over vectors element-wise.

## Arithmetic

Arithmetic operations work on scalars and auto-map element-wise over vectors. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

### Result types { #result-types }

`+`, `-`, `*`, `div`, `%` and the two-argument `min` and `max` take their result type from one promotion table. It depends only on the two operand types, so swapping the operands gives the same type, and so does every way of evaluating the call: atoms, vectors, an atom beside a vector, and columns in `select`, `select` with `where:`, `select` with `by:` (each group's vector), `update` and `update` with `by:`.

| left \ right | b8 | u8 | i16 | i32 | i64 | f32 | f64 |
|---|---|---|---|---|---|---|---|
| **b8**  | i64 | u8  | i16 | i32 | i64 | f64 | f64 |
| **u8**  | u8  | u8  | i16 | i32 | i64 | f64 | f64 |
| **i16** | i16 | i16 | i16 | i32 | i64 | f64 | f64 |
| **i32** | i32 | i32 | i32 | i32 | i64 | f64 | f64 |
| **i64** | i64 | i64 | i64 | i64 | i64 | f64 | f64 |
| **f32** | f64 | f64 | f64 | f64 | f64 | f32 | f64 |
| **f64** | f64 | f64 | f64 | f64 | f64 | f64 | f64 |

Atoms report the lowercase name (`'i32`), vectors and columns the uppercase one (`'I32`). Read it as three rules:

- among the integers, b8 < u8 < i16 < i32 < i64 and the wider type wins;
- f32 stays f32 only beside another f32. f32 with any other type, an integer of any width included, is f64: `(+ (as 'F32 [1 2]) 1)` is an `F64` vector, so an integer operand is never rounded to f32 precision. f64 with anything is f64;
- two b8 operands give i64 for `+ - * div %` (the cell above) and b8 for `min` and `max`; every other cell is the same for all seven verbs.

`min` and `max` of a TIMESPAN and a number are a TIMESPAN, whichever wins: the number counts nanoseconds (see the [timespan guide](../guides/timespan.md#min-and-max-with-a-number)).

Over a nonempty list (a `LIST`, whose cells may be of different types), an atomic builtin maps each cell on its own. What it returns depends on the builtin:

- **`+ - * / % div`** return a `LIST` of the cells' results, each the cell promoted with the other operand (the table above): `(+ (list 1 2.5) 0)` is `(list 1 2.5)`. With every cell of one type it is still a `LIST`: `(+ (list 1 2) 0)` is `(list 1 2)`, which prints as `[1 2]`, not an I64 vector.
- **`min` and `max`** promote each cell the same way, but return a typed vector when every cell's result is an atom of one type: `(min (list 1 2) 1)` is the I64 vector `[1 1]`, while `(min (list 1 2.5) 1)` is `(list 1 1.0)`, an I64 and an F64 cell.
- **The comparisons** (`== != < <= > >=`) return a `B8` vector, whatever the cells: `(== (list 1 2.5) 2.5)` is `[false true]`.
- **A unary one** (`neg`, `abs`, `floor`, `ceil`, ...) returns what `(map f x)` returns: a typed vector when every cell's result is an atom of one type, `(neg (list 1 2))` is `[-1 -2]` and `(sqrt (list 4 2.25))` is `[2.0 1.5]`, and otherwise the `LIST` of the cells' results, each in its own type, in either order of the cells: `(neg (list 1 2.5))` is `(list -1 -2.5)`, as `(- 0 (list 1 2.5))` is. Nothing is converted to the first cell's type. Cells whose results are vectors stay a `LIST` even when the vectors share a type: `(neg (list [1 2] [3 4]))` is a `LIST` of two I64 vectors.
- **Unary against binary:** a unary builtin has no other operand to promote a cell with, so on cells of different integer widths it keeps each width. `(neg (list 1 -3i))` is a list of an I64 `-1` and an I32 `3`, where `(- 0 (list 1 -3i))` is a list of two I64 cells, each promoted with the I64 `0`.

The empty list is the exception to `(map f x)`: `(neg (list))` is an empty I64 vector, where `(map neg (list))` is an empty `LIST`; `abs`, `floor` and `ceil` do the same, and `round`, `sqrt`, `log` and `exp` give an empty F64 vector.

Two operands of one type keep it, and an integer result wraps in that width, as i64 does: `(+ 0xff 0x01)` is `0x00`, `(* 300h 300h)` is `24464`. A result that wraps exactly onto the i16, i32 or i64 minimum is that type's null (`(+ 32767h 1h)` is `0Nh`), and both vector and indexed reads report it as null even if neither input contains a null (see the [types guide](../guides/types.md#implicit-coercion)).

Because `update` keeps a column's type, the f64 that f32 with an integer gives does not go back into an f32 column: `(update {f: (+ f 1) from: t})` over an `F32` column `f` is a `type` error. Add an f32 literal, `(+ f 1e)`, or cast the result with `as 'F32` (see [Column types](../queries/select.md#column-types)).

`/`, `pow` and the math functions below always return f64.

```lisp
(type (+ 2h 3i))              ; 'i32
(type (+ [2i] [true]))        ; 'I32
(type (min 2h 3i))            ; 'i32
(type (div [7i] [2h]))        ; 'I32
(type (+ (as 'F32 2.5) 1))    ; 'f64
(type (+ (as 'F32 [1 2]) 1))  ; 'F64
(type (+ 1e 2e))              ; 'f32
(+ true true)                 ; 2
```

### `(+ a b)` — atomic

Addition. Works on integers, floats, dates, and times.

```lisp
(+ 3 4)  ; 7
(+ [1 2 3] 10)  ; [11 12 13]
(+ [1 2 3] [10 20 30])  ; [11 22 33]
```

### `(- a b)` — atomic

Subtraction.

```lisp
(- 10 3)  ; 7
(- [10 20 30] 5)  ; [5 15 25]
```

### `(* a b)` — atomic

Multiplication.

```lisp
(* 3 4)  ; 12
(* [2 3 4] [10 10 10])  ; [20 30 40]
```

### `(/ a b)` — atomic

Division. Always returns f64, even for integer arguments. Use `div` for
integer (floor) division.

```lisp
(/ 10 3)  ; 3.33
(/ 10.0 3)  ; 3.33
```

### `(% a b)` — atomic

Modulo: `a - b * floor(a / b)`, so the result takes the sign of `b`. A
float result is that value rounded once, so it lies in `[0, b)` for a
positive `b` and in `(b, 0]` for a negative one at any magnitude:
`(% 1e300 7.0)` is `1.0`, and a remainder that would round to `b` itself
is `0` (`(% -1e-20 7.0)` is `0.0`). The
result type comes from the [table above](#result-types). A zero divisor
gives null, with one exception kept from u8 byte modulo: an integer result
with a u8 divisor (or a u8 result) gives `0`. A float result of a zero
divisor is always null: `(% 7.5 0x00)` is `0Nf`.

```lisp
(% 10 3)  ; 1
(% -7 2)  ; 1
(% [10 11 12] 3)  ; [1 2 0]
(% 300 0xff)  ; 45
(% 7.5 0x02)  ; 1.5
```

### `(div a b)` — atomic

Floor division: the largest integer not above `a / b`. The result type
comes from the [table above](#result-types), so integers stay integers of
the wider operand's width and a float operand gives a float. A zero divisor
gives null, or `0` for a u8 result, which has no null. Unlike `%`, a u8
divisor alone does not make it `0`: `(div 7 0x00)` is `0Nl`, while
`(% 7 0x00)` is `0`.

```lisp
(div 7 2)  ; 3
(div -7 2)  ; -4
(type (div 7i 2i))  ; 'i32
(div 7.5 2.0)  ; 3.0
```

### `(neg x)` — atomic

Negate a number.

```lisp
(neg 5)  ; -5
(neg [1 -2 3])  ; [-1 2 -3]
```

### `(round x)` — atomic

Round to nearest integer.

```lisp
(round 3.7)  ; 4.0
(round [1.2 2.5 3.8])  ; [1.0 3.0 4.0]
```

### `(floor x)` — atomic

Round down to nearest integer.

```lisp
(floor 3.9)  ; 3.0
```

### `(pow x y)` — atomic

Power. Always returns f64. Vector and query expressions lower to the DAG
executor for morsel-based execution; null in either operand yields `0Nf`.

```lisp
(pow 2 10)  ; 1024.0
(pow [1 2 3] 2)  ; [1.0 4.0 9.0]
```

### `(ceil x)` — atomic

Round up to nearest integer.

```lisp
(ceil 3.1)  ; 4.0
```

### `(abs x)` — atomic

Absolute value.

```lisp
(abs -7)  ; 7
(abs [-3 0 5])  ; [3 0 5]
```

### `(sqrt x)` — atomic

Square root. Always returns f64; negative inputs return `0Nf`.

```lisp
(sqrt 9)  ; 3.0
(sqrt [4 9 16])  ; [2.0 3.0 4.0]
```

### `(log x)` — atomic

Natural logarithm. Always returns f64; non-positive inputs return `0Nf`.

```lisp
(log 2.718281828459045)  ; 1.0
```

### `(exp x)` — atomic

Exponential, `e^x`. Always returns f64.

```lisp
(exp 1)  ; 2.718...
```

### `(sin x)` / `(cos x)` / `(tan x)` — atomic

Trigonometric functions in radians. Always return f64.

```lisp
(sin 0.0)  ; 0.0
(cos 0.0)  ; 1.0
(sin [0.0 1.57079632679])  ; [0.0 1.0]
```

### `(asin x)` / `(acos x)` / `(atan x)` — atomic

Inverse trigonometric functions in radians. `asin` and `acos` return `0Nf` for values outside `[-1, 1]`.

```lisp
(asin 1.0)  ; 1.570...
(acos 1.0)  ; 0.0
(nil? (asin 2.0))  ; true
```

### `(reciprocal x)` — atomic

Reciprocal, `1/x`. Always returns f64; zero returns `0Nf`.

```lisp
(reciprocal 4)  ; 0.25
(reciprocal [2 0 4])  ; [0.5 0Nf 0.25]
```

### `(signum x)` — atomic

Return the sign as i64: `-1`, `0`, or `1`. Null input stays null.

```lisp
(signum -3.5)  ; -1
(signum [-2.5 0.0 4.0])  ; [-1 0 1]
```

The direct numeric unary functions accept `BOOL`, `U8`, `I16`, `I32`, `I64`, and `F64` inputs. They do not treat temporal encodings as numbers.

## Comparison

All comparison operations are atomic and return boolean values (or boolean vectors).

The operand types must be comparable; any other pair is a `type` error:

- numbers (`BOOL`, `U8`, `I16`, `I32`, `I64`, `F32`, `F64`) with each other
  and with `TIMESPAN`;
- `DATE`, `TIME` and `TIMESTAMP` with each other, on one nanosecond scale;
  `TIME` and `TIMESTAMP` also with `TIMESPAN`;
- `GUID`, `SYM` and `STR` each with its own type.

A temporal value is not a number: `(> 2024.01.02 8000)` is a `type` error
rather than a comparison of the stored day count, and so is ordering a
symbol against a number. `==` and `!=` against a null answer without
looking at the other type: `(== 2024.01.02 0N)` is `false`.

The same rule holds inside a query, with the same errors. The one difference
is a string literal, which a query reads as a symbol, so `(== sym "AAPL")`
compares a `SYM` column with the symbol `AAPL`, and `in` reads a set of
strings the same way: `(in sym (list "AAPL" "MSFT"))` and
`(in sym ["AAPL" "MSFT"])` keep the rows holding either symbol. Each string
is looked up in the column's own symbols, so a string no row holds matches
nothing, and `""` selects the null rows, as `(== sym "")` does. A set
computed while the query runs reads the same way as a written one. A `GUID`
column against a string is a `type` error, for `==` and for `in` alike.

The reverse is not converted. A `STR` value (a string column, or an `if` over
string literals, see [control flow](control-flow.md)) against a symbol is a
`type` error for `==`, but `in` against a set of symbols answers `false` for
every cell, with no error: `(in s ['a])` over a string column `s` keeps no
row. Write the set as strings, `(in s ["a"])`, or convert the column,
`(in (as 'SYM s) ['a])`.

### `(== a b)` — atomic

Equal. Works on all types including strings and symbols.

```lisp
(== 3 3)  ; true
(== [1 2 3] 2)  ; [false true false]
```

Dictionaries retain their keys and compare values by key. When keys differ,
the result contains the left keys followed by new right keys; a key present
on only one side compares `false`. An atom broadcasts over dictionary values.
Tables compare cell by cell, returning boolean columns. Table column names
and order and row counts must agree, otherwise `==` raises `length`. An atom
broadcasts over all cells. Other comparisons and arithmetic keep their
existing behavior. In particular, `!=` still rejects dictionaries and tables.
Nonempty dictionary key vector types must match; different key types raise `type`.
Identical key vectors (including duplicate keys) compare values positionally;
otherwise each left value compares with the first right value for its key,
including repeated left keys. For example, `[a a b]` versus `[a a c]` with
values `[1 2 3]` on both sides yields `{a:true a:false b:false c:false}`.
Keyed-table comparisons support single and composite keys, including durations.

### `(identical a b)`

Returns one boolean for whole-value identity: the same type, shape and
contents recursively. Numeric types are never promoted. Type and length
mismatches return `false`. Lists and typed vectors are distinct types.
Nulls match only the same type; floating values compare exactly, with NaNs
matching NaNs and negative zero matching positive zero. Strings compare by
bytes and symbols by text. Storage attributes and indexes do not matter.
Dictionary keys and values must agree in order; tables must have the same
column names in order and identical columns. Lazy values are evaluated.
Builtins match the same builtin; lambdas match only the same function object.

```clojure
(identical [1 2] [1 2])  ; true
(identical 1 1i)        ; false
(identical (list 1 2) [1 2]) ; false
(== {a: 1 b: 2} 1)     ; {a:true b:false}
```

### `(!= a b)` — atomic

Not equal.

```lisp
(!= 3 4)  ; true
```

### `(< a b)` — atomic

Less than.

```lisp
(< 3 5)  ; true
(< [1 5 3] 4)  ; [true false true]
```

### `(<= a b)` — atomic

Less than or equal.

```lisp
(<= 3 3)  ; true
```

### `(> a b)` — atomic

Greater than.

```lisp
(> 5 3)  ; true
```

### `(>= a b)` — atomic

Greater than or equal.

```lisp
(>= 5 5)  ; true
```

### `(within vec [lo hi])`

Check if each element of a vector is within a range (inclusive). The second argument is a 2-element vector `[lo hi]`.

```lisp
(within [1 5 9] [3 7])  ; [false true false]
```

## Logic

### `(and a b)` — atomic

Logical AND. On atoms it answers one boolean; on a vector or a list it
works element by element, each element by its truthiness, and returns a
boolean vector (a nested list keeps its shape). An atom broadcasts, and two
collections of different lengths are a `length` error. The exception is a
first operand that is an atom and already decides the answer: it is
returned without reading the rest, as one boolean: `(and false [1 0 2])` is `false`, `(or true [1 0])`
is `true`. Inside a query the answer is per row either way.

```lisp
(set x 5)
(and (> x 0) (< x 10))
(and [true false] (list true true))  ; [true false]
(and [1 0 2] true)                   ; [true false true]
```

The truth value of an element: `false`, zero, null and NaN are false; any
other number or temporal is true, and so is any symbol, string or GUID that
is not null. A query reads its operands the same way, so
`(select {from: t where: (and qty (> price 100))})` keeps the rows where
`qty` is non-zero and not null.

### `(or a b)` — atomic

Logical OR, element by element on collections, as `and` is.

```lisp
(set x 5)
(or (== x 0) (== x 10))
```

### `(not x)` — atomic

Logical NOT. Flips boolean values. A STR, SYM or GUID vector is a `type`
error: `not` has no meaning for those cells.

```lisp
(not true)  ; false
(not [true false true])  ; [false true false]
(not (list 1 0))         ; [false true]
```

Comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) on a list also give a
boolean vector, element by element, as on a typed vector.

## Aggregation

Aggregation functions reduce a vector to a single scalar. They carry the `FN_AGGR` flag and are recognized by the `select`/`update` query planner for group-by operations.

### `(sum x)` — aggregate

Sum of all elements.

```lisp
(sum [1 2 3 4])  ; 10
```

In a query, the sum of a column plus a constant may be computed from the
column's total; see [A column plus a constant in a query](#affine-sum-avg)
for how an F64 answer can differ from the row-by-row one.

### `(prod x)` — aggregate

Product of all non-null numeric elements. An empty input is the empty product, `1`, and so is a query `prod` over a `where:` that keeps no row.

```lisp
(prod [2 3 4])  ; 24
(prod (as 'I64 []))  ; 1
```

### `(all x)` — aggregate

Returns `true` when every non-null numeric element is truthy. Empty and all-null inputs return `true`.

```lisp
(all [1 2 3])  ; true
(all [1 0 3])  ; false
```

### `(any x)` — aggregate

Returns `true` when at least one non-null numeric element is truthy. Empty and all-null inputs return `false`.

```lisp
(any [0 0 3])  ; true
(any [0 0 0])  ; false
```

### `(count x)` — aggregate

Number of elements. Counts total vector length, including nulls.

```lisp
(count [1 2 3])  ; 3
```

### `(avg x)` — aggregate

Arithmetic mean. The mean of a TIMESPAN vector is a TIMESPAN, rounded to the
nanosecond (half away from zero); `avgs` and `mavg` of a TIMESPAN vector
are TIMESPAN vectors as well. The mean of a vector is computed from an exact
128-bit total, so it is exact to the nanosecond; the grouped, scan and moving
forms go through F64 and are exact up to 2^53 nanoseconds (about 104 days).

A TIMESPAN vector is also accepted by `sums`, `mins`, `maxs`, `msum`, `mmin`,
`mmax`, `avgs`, `mavg`, `mdev` and `deltas`, and answers a TIMESPAN;
`ratios` accepts it and answers F64.

```lisp
(avg [2 4 6])  ; 4.0
```

#### A column plus a constant in a query { #affine-sum-avg }

In a `select` or `update`, `sum` and `avg` of a column plus or minus a
constant, `(+ x c)`, `(+ c x)` or `(- x c)`, may be computed from the
column's own total: the query sums `x` once and adds `n * c` (or, for
`avg`, `c`) at the end, without forming the `n` rows of `x + c`. The answer
can differ from summing the rows `x + c` one by one.

For an I64 or other integer column with an integer constant the result is
exact: the total of `x` and the term `n * c` are added in 128 bits and
divided once, so the query answers what `sum` and `avg` answer over the
materialized rows, on every route (with `where:`, beside other aggregates,
grouped, and on parted tables).

For an F64 column the result is not bit-identical to the row-by-row one.
Each row `x + c` rounds on its own and the query skips that rounding; the
two routes also add in different orders, and `avg` divides at a different
point. Two rows show it:

```lisp
(set t (table [x] (list [9007199254740992.0 -9007199254740992.0])))
(select {from: t s: (sum (+ x 1.0)) a: (avg (+ x 1.0))})
; s 2.0, a 1.0: (2^53 + 1) + (-2^53 + 1) without row rounding
(sum (do (+ (at t 'x) 1.0)))
; 1.0: the row 2^53 + 1 rounds to 2^53 first
```

The bound below holds under two conditions: the rows counted are the `n`
non-null values of `x` (null rows are skipped by both routes), and no
intermediate overflows, that is, every row `x + c`, the product `n * c`, and
every partial total that either route forms in its order of addition stays
finite. A sufficient check for the second condition is
`sum |x| + n * |c| <= 2^1023` (about `8.99e307`). With `T = sum |x| + n * |c|`:

- the two sums differ by at most `(n + 1) * 2^-51 * T`;
- the two averages differ by at most `(n + 1) * 2^-51 * T / n + 2^-1074`.

The sum bound holds for subnormal values as well, because an F64 addition
whose result is subnormal is exact. Each route's division by `n` can
underflow, though, and an underflowing division loses up to half of the
smallest subnormal `2^-1074`, which a relative bound does not cover; the
term `2^-1074` in the average bound accounts for those two divisions. Two
subnormal rows show it:

```lisp
(set t (table [x] (list [5e-324 0.0])))
(select {from: t a: (avg (+ x 5e-324))})
; 5e-324 (2^-1074): the mean of x, 2^-1075, rounds to 0.0 before c is added
(avg (do (+ (at t 'x) 5e-324)))
; 1e-323 (2 * 2^-1074): the rows sum exactly to 3 * 2^-1074, and half
; of that rounds to 2 * 2^-1074
```

When an intermediate overflows, no bound is guaranteed. An overflowing F64
total is `0Nf`, and each route answers `0Nf` or a finite value depending on
whether one of its own partial totals overflowed, which depends on the
values, the constant and the order of the additions; how far the exact
total lies from the F64 limit does not decide it. With rows
`1.7976931348623157e308`, `2^970` and `-1.7976931348623157e308` and
`c = -2^969`, the exact total is `-2^969`: in the query's column total
`1.7976931348623157e308 + 2^970` overflows and the query answers `0Nf`, while the rows `x + c`
cancel and sum to `0.0`.

The query computes from the column total only while `|c|` is below
`2^970`; a larger constant is added row by row, so the answer is the
row-by-row one, and `(sum (+ x -1e308))` over rows `1e308` is `0.0`.

### `(min x)` — aggregate

Minimum value. With two arguments, `(min a b)` is the element-wise minimum,
typed by the [promotion table](#result-types).

```lisp
(min [5 1 9])  ; 1
(min [5 1 9] [2 2 2])  ; [2 1 2]
```

### `(max x)` — aggregate

Maximum value. With two arguments, `(max a b)` is the element-wise maximum,
typed by the [promotion table](#result-types).

```lisp
(max [5 1 9])  ; 9
(max [5 1 9] [2 2 2])  ; [5 2 9]
```

### `(med x)` — aggregate

Median value.

```lisp
(med [1 3 5 7 9])  ; 5.0
```

For a TIMESPAN vector, the median is a TIMESPAN: a median of durations is a
duration. An even-sized input takes the midpoint of the two middle values,
rounded to the nanosecond (half away from zero); an odd-sized input answers one
of its values. `SELECT` uses the same result type, as do `quantile` and
`percentile`. The endpoints are taken from the integer values, so a median
or quantile of a vector is exact at any magnitude.

```lisp
(med [0D01:00:00 0D02:00:00])  ; 0D01:30:00.000000000
```

### `(dev x)` — aggregate

Population standard deviation. The standard deviation of a TIMESPAN vector
(`dev`, `stddev`, `stddev_pop`, `mdev`) is a TIMESPAN, rounded to the
nanosecond; a variance of durations is a squared duration and stays F64.

```lisp
(dev [2 4 4 4 5 5 7 9])  ; 2.0
```

### `(stddev x)` — aggregate

Sample standard deviation.

```lisp
(stddev [1 2 3])  ; 1.0
```

### `(stddev_pop x)` / `(dev_pop x)` — aggregate

Population standard deviation.

```lisp
(stddev_pop [1 2 3])  ; 0.816...
```

### `(var x)` / `(var_pop x)` — aggregate

Sample and population variance.

```lisp
(var [1 2 3])      ; 1.0
(var_pop [1 2 3])  ; 0.666...
```

#### Variance and standard deviation accuracy

`var`, `var_pop`, `stddev`, `stddev_pop`, `dev` and `dev_pop` give the same
value, to the bound below, on every route a query takes: keyless or
grouped, `select` or `update`, in-memory, splayed or partitioned, any key
type, any number of workers. Each sums the differences of the values from a
value of the data (the first value of the group, or of the part of it one
worker or one partition summed) and the squares of those differences; an
integer's difference is taken exactly in 64-bit integer arithmetic and
rounded once. The parts combine exactly: each re-centres its sums on the
others' value, and a partition hands its value, its mean about it and its
variance to the merge. So the error
follows the spread of the data, not its magnitude: values near 1e8 or at
timestamp scale (1e18 nanoseconds) are as accurate as values near 1.

For n values, the relative error of the variance is at most
c·n·ε·(1 + z0²), with ε = 2⁻⁵³ ≈ 1.1e-16, c a small constant, and z0 the
largest distance, in standard deviations, between the mean and a value
the sums were taken about. A standard deviation's relative error is half
that. The eager builtin on a vector (`(var v)` outside a query) takes two
passes over the differences from the first value and is tighter: at most
c·n·ε, whatever the data's first value is.

z0 is a few at most when the data does not start far out in its own tail:
for values in no particular order the values summed about are typical
ones, and for values sorted by size the farthest is the minimum or the
maximum (under 2 standard deviations for evenly spread values, about 5 for
a million normally distributed ones). z0 is large when data starts far
from its mean, for example a group whose first value is an outlier a
thousand standard deviations from the rest: the error then grows with
z0², a million-fold in that example, and stays within the bound. That is
the one kind of data where the result can be less accurate than summing
the raw squares, which was accurate for values near 0 and lost everything
once their magnitude was large next to their spread (1e8 + [0 1 2 3] gave
0). A variance of values that are all equal is exactly 0.

Overflow. The sums are doubles; Rayfall has no infinity, and an
overflowing float is `0Nf`, as in `(* 1e300 1e300)`. When an intermediate
of the variance (a difference, the sum of differences or its square, the
sum of squares) overflows, every member of the family returns `0Nf`,
standard deviations included, never a wrong number. Only a tiny negative
residue from rounding is taken as 0. The sum of differences is the one to
watch: it is about n·|x − first value|, and its square overflows once that
passes roughly 1e154. For example, a million values alternating 0 and 1e149
sum to about 5e154: a grouped `(var_pop v)` over them gives `0Nf`, while
the eager builtin (two passes, the second about the mean) and, here, the
keyless routes (whose partial sums per chunk stay smaller) give the
variance 2.5e297. Squares that overflow outright overflow on
every route: `(stddev [1e300 -1e300 1e300])` is `0Nf`. Routes may differ
in whether they overflow, since each takes its sums about a different value
(the first of the group, of a worker's part or of a partition). Ordinary
data is far from these magnitudes and is unaffected.

### `(pearson_corr x y)` — aggregate

Pearson correlation over paired non-null numeric inputs.

```lisp
(pearson_corr [1 2 3] [2 4 6])  ; 1.0
```

### `(cov x y)` / `(scov x y)` — aggregate

Population and sample covariance over paired non-null numeric inputs.

```lisp
(cov [1 3] [2 6])   ; 2.0
(scov [1 3] [2 6])  ; 4.0
```

### `(wsum weights values)` / `(wavg weights values)` — aggregate

Weighted sum and weighted average over paired non-null inputs. `wavg` returns null when there are no valid pairs or the total weight is zero.

```lisp
(wsum [1 3] [10 20])  ; 70.0
(wavg [1 3] [10 20])  ; 17.5
```

Unary numeric reducers skip nulls where applicable. Binary reducers skip a row when either input is null. Inside `select`/`by:`, aggregate reducers lower to morsel-based DAG paths that can run in parallel and poll for cancellation.

### `(first x)` — aggregate

First element of a vector. A direct call is positional: it returns the
element at the front, a null included, for every type. In a query, inside
`select … by:` or as the aggregate of a keyless `select` output, `first` is
an aggregate and skips nulls like the other aggregates, so it returns the
first non-null cell, or the null when every cell is null. See
[Nulls in a query](#first-last-nulls) below.

```lisp
(first [10 20 30])  ; 10
(first [0Nl 1])     ; 0Nl
(first ["" "x"])    ; ""   the STR null, by position
```

### `(last x)` — aggregate

Last element of a vector, with the same split as `first`: positional in a
direct call, the last non-null cell in a query.

```lisp
(last [10 20 30])  ; 30
(last [1 2 0Nl])   ; 0Nl
(last ["x" ""])    ; ""
```

#### Nulls in a query { #first-last-nulls }

The empty string is the STR null, as `'` is the SYM null and `0Nl` the I64
one, so a grouped or keyless-select `first` or `last` skips `""` cells the way
it skips `0Nl`:

```lisp
(set t (table [g s] (list ['a 'a 'b 'b] ["x" "" "y" ""])))
(at (select {from: t by: g l: (last s)}) 'l)   ; ["x" "y"]   "" skipped
(at (select {from: t l: (last s)}) 'l)         ; ["y"]
(last ["x" ""])                                ; ""          positional
```

The two forms answer different questions. A direct `(last v)` is an
accessor, the same as `(at v (- (count v) 1))`, so it returns whatever sits
there. In a query, `first` and `last` are aggregates like `sum`, `min` and
`max`, which all skip nulls, so a group's `last` is its last known value. A
group whose cells are all `""` gives `""`. To get the positional answer in
a `by:` select, index the group instead: `(at s (- (count s) 1))` gives
`["" ""]` above.

The skip applies wherever `first` or `last` aggregates a group's rows,
including inside the body of a helper called from a query, for a value of
the query's rows: `(fn [x] (let q 1) (first x))` applied to a column gives
the first non-null cell, as `(first h)` does
([#1673](https://github.com/RayforceDB/rayforce/issues/1673)).
Which arguments a query reads as rows is set out in
[Aggregate sources](../queries/select.md#aggregate-sources).

### `(distinct x)` — aggregate

Unique elements of a vector or list, in the order values first appear. A list
keeps each cell where it first occurs, whether its cells are atoms, vectors or
nested lists. A string atom gives its characters, in the order they first
appear: `(distinct "tset")` is `"tse"`.

List cells are equal when their values are, the rule `distinct`,
`except`, `sect`, `union`, `group`, `xkey` and keyed-table `==` share: a
vector cell equals one of the same type, length and elements, and in F64
and F32 vector cells, as between float atoms, negative zero equals positive
zero. NaN, the F64 null, equals NaN and no number.

```lisp
(distinct [3 1 3 2 1])          ; [3 1 2], in the order values first appear
(distinct (list [1] [2] 3 [1])) ; [[1] [2] 3]
(count (distinct (list [0.0] (enlist (neg 0.0))))) ; 1
```

## Ordering

Sorting and ranking operations for vectors and tables.

### `(asc x)`

Sort a vector in ascending order.

```lisp
(asc [3 1 2])  ; [1 2 3]
```

### `(desc x)`

Sort a vector in descending order.

```lisp
(desc [3 1 2])  ; [3 2 1]
```

### `(iasc x)`

Return indices that would sort the vector ascending (grade up).

```lisp
(iasc [30 10 20])  ; [1 2 0]
```

### `(idesc x)`

Return indices that would sort the vector descending (grade down).

```lisp
(idesc [30 10 20])  ; [0 2 1]
```

### `(xasc table col)`

Sort a table ascending by the named column. A parted table is sorted across
all of its partitions into an in-memory table, the partition-key column
included; see [Loading Partitioned Data](../storage/index.md#loading-partitioned-data).

```lisp
(set trades (table [sym price] (list [AAPL GOOG AAPL] [150.0 280.0 151.0])))
(xasc trades 'price)
```

### `(xdesc table col)`

Sort a table descending by the named column. A parted table is sorted as
`xasc` sorts one.

```lisp
(xdesc trades 'price)
```

### `(xrank n x)`

Assign each element to one of `n` equal-sized buckets (quantile rank).

```lisp
(xrank 4 [10 20 30 40 50 60 70 80])  ; [0 0 1 1 2 2 3 3]
```

## Generation

Functions for creating, reshaping, and searching vectors.

### `(til n)`

Generate integers from 0 to n-1.

```lisp
(til 5)  ; [0 1 2 3 4]
```

### `(take x n)`

Take the first `n` elements from a vector, or repeat/cycle `x` to length `n`.

```lisp
(take [10 20 30 40] 3)  ; [10 20 30]
(take [A B] 5)  ; [A B A B A]
```

### `(reverse x)`

Reverse a vector, a table's rows, or a dictionary's entries. A table keeps
its column names and types; a dictionary's keys and values move together.

```lisp
(reverse [1 2 3])  ; [3 2 1]
(set t (table [a b] (list [1 2 3] ["x" "y" "z"])))
(at (reverse t) 'a)  ; [3 2 1]
(reverse (dict ['a 'b 'c] [10 20 30]))  ; {c:30 b:20 a:10}
```

### `(where x)`

Return indices where the boolean vector is true.

```lisp
(where [false true false true])  ; [1 3]
(where (> [10 5 20] 8))  ; [0 2]
```

### `(find x val)`

Find the index of the first occurrence of `val` in vector `x`.

```lisp
(find [10 20 30] 20)  ; 1
```

### `(xbar x n)`

Round each element down to the nearest multiple of `n` (bucketing).
A temporal `x` keeps its type. A null `x`, atom or element, gives the null of
the result type. A float bucket (F32 or F64) on a DATE, TIME or TIMESTAMP `x`
is a `type` error, in a query as outside one; an empty `x` still gives an
empty result, with no error.

```lisp
(xbar [3 7 12 18] 5)       ; [0 5 10 15]
(xbar [2000.01.10 0Nd] 7)  ; [2000.01.08 0Nd]
(xbar 0Nd 7)               ; 0Nd
```

## Higher-order Functions

Functions that take other functions as arguments for flexible iteration patterns.
Inside a query, a helper they call sees the source columns its body reads, as a
direct call of the helper does, however the helper was reached
([Names in queries](name-resolution.md#names-in-queries)).

### `(map f x)`

Apply function `f` to each element of `x`. Results that are atoms of the
same type form a typed vector; anything else forms a list.

```lisp
(map (fn [x] (* x x)) [1 2 3])  ; [1 4 9]
```

### `(map f val x)`

The three-argument form holds one operand fixed: it calls `f(val, elem)` for
each element of `x`, passing `val` whole every time. A collection `val` gives
one row per element, as `map-right` does. Results are collected as in
`(map f x)`.

```lisp
(map + 100 [1 2 3])                  ; [101 102 103]
(map + [1 2 3] [10 20])              ; [[11 12 13] [21 22 23]]
```

`map` takes exactly 2 or 3 arguments and raises `domain` otherwise; the
extra arguments are not ignored.

### `(fold f init x)`

Left fold. Accumulate over `x` starting from `init` using binary function `f`.

```lisp
(fold + 0 [1 2 3 4])  ; 10
```

### `(scan f x)`

Running fold (prefix scan). Returns a vector of intermediate accumulation results.

```lisp
(scan + [1 2 3 4])  ; [1 3 6 10]
(scan * [1 2 3 4])  ; [1 2 6 24]
```

For common time-series scans, prefer the built-in DAG forms. They avoid per-row function calls, run through morsel-based kernels, and can be used directly inside `select` projections:

```lisp
(sums [1 2 3 4])            ; [1 3 6 10]
(prds [1 2 3 4])            ; [1 2 6 24]
(avgs [2 4 6])              ; [2.0 3.0 4.0]
(mins [3 1 2])              ; [3 1 1]
(maxs [3 1 2])              ; [3 3 3]
(fills (as 'I64 (list 0N 2 0N))) ; [0Nl 2 2]
(fills (table [t value]
              (list [1 2 3] (as 'I64 (list 10 0N 30))))) ; fills each column
(differ [1 1 2 2])          ; [true false true false]
(msum 3 [1 2 3 4])          ; [1 3 6 9]
(mavg 3 [1 2 3 4])          ; [1.0 1.5 2.0 3.0]
(mmin 3 [3 2 4 1])          ; [3 2 2 1]
(mmax 3 [3 2 4 1])          ; [3 3 4 4]
(mcount 3 [1 2 3 4])        ; [1 2 3 3]
(mvar 2 [1 3 5])            ; [0.0 1.0 1.0]
(mdev 2 [1 3 5])            ; [0.0 1.0 1.0]
```

The moving forms take a positive integer window first. Constant windows inside query projections lower to DAG operations; dynamic windows still evaluate as ordinary function calls.

On floats, each `msum` and `mavg` result reads only the rows of its own window, so a huge value earlier in the column does not change the windows after it. A window total past the largest F64 is `0Nf`, and a mean is null only when its window holds no values. `avgs` stays finite when the running total itself would overflow:

```lisp
(msum 2 [1e300 1e300 1.0 2.0])  ; [1e300 2e300 1e300 3.0]
(msum 2 [1e308 1e308 1.0 2.0])  ; [1e308 0Nf 1e308 3.0]
(mavg 2 [1e308 1e308 1.0 2.0])  ; [1e308 1e308 5e307 1.5]
(avgs [1e308 1e308 1.0 2.0])    ; [1e308 1e308 6.67e307 5e307]
```

### `(apply f a b)`

Call the binary function `f` on the elements of two collections taken in step:
`f(a_0, b_0)`, `f(a_1, b_1)`, and so on. The result is as long as the shorter
collection, and is collected as `map`'s is (a typed vector while the results are
atoms of one vector type, otherwise a list). Two atoms call `f` once; an atom
beside a collection is a `type` error. It takes exactly three arguments.

```lisp
(apply + 1 2)                      ; 3
(apply + [1 2 3] [10 20 30])       ; [11 22 33]
(apply + [1 2 3 4] [10 20])        ; [11 22]
(apply concat ["a" "b"] ["x" "y"]) ; ["ax" "by"]
```

To hold one operand fixed instead, use `(map f val v)` or `map-right`.

### `(filter vec mask)`

Keep elements of `vec` where the boolean `mask` is true.

```lisp
(filter [1 2 3 4] (> [1 2 3 4] 2))  ; [3 4]
```

### `(pmap f x)`

`map` with its calls run in parallel on the thread pool's workers when that is
safe and worth it. It takes the same forms as `map`: `(pmap f x)` or
`(pmap f val x)`, and `domain` for any other number of arguments. It returns
the value `map` returns, of the same type and in the input's order, and the
error `map` reports.

```lisp
(set B (til 3000))
(set heavy (fn [x] (count (distinct (% (* B 7) (+ 1 (% x 97)))))))
(pmap heavy (til 10000))         ; heavy's calls spread over the workers
(pmap (fn [x] (* x x)) [1 2 3])  ; [1 4 9]: too cheap to dispatch, runs as map
(pmap + 100 [1 2 3])             ; [101 102 103]
```

The calls run on the workers only when the function is proven pure, so the
order the calls run in cannot be seen. The proof judges the functions a call
will reach, not the names written in the body. A builtin is recognized by its
own registered name, whatever names it is bound to now: with `reverse` bound
to `rand`, a call of `reverse` is a call of `rand`. A lambda that was already
compiled is judged by what its compiled code holds: a builtin the compiler
baked in stays the function it was then, and a name resolved at call time is
judged by its binding when `pmap` is called. The proof is taken at every call,
so a rebinding between two calls is seen. A function is proven pure when it
is:

- A builtin that is deterministic, has no effect, reads nothing but its
  arguments and never calls back into user code: arithmetic and comparisons,
  aggregates (`sum`, `avg`, `min`, `max`, `count`, ...), the string verbs
  (`upper`, `lower`, `trim`, `like`, ...), the collection verbs (`til`, `take`,
  `drop`, `distinct`, `asc`, `in`, `where`, `group`, `at`, ...) and the casts
  the query planner lowers.
- A lambda whose body is built only from such calls, its parameters, its
  captures, `let` locals, literals and global data, with the forms `if`, `do`,
  `let`, `and`, `or` and `quote`.

Anything else runs as `map` does, on the calling thread, in order. That includes
a body that uses `set`, prints, does I/O, calls `rand`, `eval` or a dotted
(`.sys`, `.ipc`, `.db`, ...) builtin, calls a user function, a local or a lambda
literal, or is a trusted (`.sys.trust`) lambda. It also includes any call
inside a `select`, inside another `pmap`'s calls, under `.sys.profile` or the
query profiler, and on a pool with no workers (`-c 1`).

**Choosing the route.** `pmap` decides between running the rest of the calls on
the workers and running them on the calling thread, as `map` does, from a
timing probe of its first calls. The route therefore depends on time, and the
same input can run sequentially on one call and in parallel on another. The
results, the errors and the effects never differ between the two routes: only a
function proven pure is ever run on the workers.

- An input of fewer than 16 elements runs as `map` outright, with no probe.
- Otherwise the calls start on the calling thread, in `map`'s own loop. The clock
  is read after 1, 2, 4, 8, … calls. The first call is not counted: it pays for
  cold caches and page faults. Each later reading closes a window of calls, and a
  call's cost is the lower of the last two windows' per-call times. One window
  stretched by a stall or a clock jump therefore cannot make cheap calls look
  heavy. A clock that does not move forwards ends the probe, and the calls stay
  on the calling thread.
- After the windows cover about 100 µs, the rest go to the workers only when:
  - one call costs at least about 4 µs;
  - the remaining calls would take at least about 1 ms;
  - the probed calls did not use the thread pool themselves.
- A body that uses the pool itself (one over large vectors, whose kernels already
  run on the workers) stays on the calling thread. Running several such calls at
  once would give up that parallelism, and it would hold several large vectors at
  once instead of one that the caches hold.
- Each worker evaluates its share on a VM of its own, with the caller's `-U`
  restriction. Builtins inside a call that would use the pool run inline on that
  worker. The workers take the calls in chunks of about 50 µs each at the
  probe's estimate.
- The workers time their own calls. Once a worker's last 8 calls average under
  an eighth of the probe's estimate and under about 4 µs, no more calls go to
  the workers, and the rest run on the calling thread as in `map`.

When the probe is right, `pmap` costs at most what `map` costs plus the probe:
a few clock reads, plus the calls made before the decision, which `map` makes
anyway. A probe on the first calls can be wrong when later calls are much
cheaper, for example after a few heavy elements. A cheap call costs more on a
worker than on the calling thread, because the calls share the function's
objects across cores. In that case the workers stop after a few calls each, and
the wrong dispatch costs those calls plus waking the pool. The workers judge by
their own timings, and those include that sharing cost. So a tail that still
measures above the cutoff on the workers keeps running there.

An error at one element stops the workers from starting later elements. The
first element left without a value is then evaluated again on the calling
thread, and the rest follow there in order, as in `map`. So the error you get,
with its source position, is the one `map` gives, from the lowest failing
element. An interrupt also ends as it does in `map`.

## Quick Reference Table

| Category | Functions |
|---|---|
| **Arithmetic** | `+` `-` `*` `/` `%` `div` `neg` `round` `floor` `ceil` `abs` `sqrt` `log` `exp` `sin` `asin` `cos` `acos` `tan` `atan` `reciprocal` `signum` `pow` |
| **Comparison** | `==` `!=` `<` `<=` `>` `>=` `within` |
| **Logic** | `and` `or` `not` |
| **Aggregation** | `sum` `prod` `all` `any` `count` `avg` `min` `max` `med` `dev` `stddev` `var` `pearson_corr` `cov` `scov` `wsum` `wavg` `first` `last` `distinct` |
| **Ordering** | `asc` `desc` `iasc` `idesc` `xasc` `xdesc` `xrank` |
| **Generation** | `til` `take` `reverse` `where` `find` `xbar` |
| **Higher-order** | `map` `fold` `scan` `apply` `filter` `pmap` |

!!! note "Auto-mapping"
    Arithmetic, comparison, and logic functions automatically map over vectors element-wise. You do not need explicit `map` calls for these — `(+ [1 2 3] 10)` works directly.

For equality, mixing a dictionary with a table is a `type` error in either
operand order; a dictionary's keys are not interpreted as table columns.

Table equality and identity read stored columns as values: encoded and
partitioned columns are materialized, including virtual partition columns.

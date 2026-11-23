# Math, Comparison, Aggregation & Iteration

All numeric, logical, aggregation, ordering, generation, and higher-order operations available in Rayfall. Operations marked `FN_ATOMIC` auto-map over vectors element-wise.

## Arithmetic

Arithmetic operations work on scalars and auto-map element-wise over vectors. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

### Result types { #result-types }

`+`, `-`, `*`, `div`, `%` and the two-argument `min` and `max` take their result type from one promotion table. It depends only on the two operand types, so swapping the operands, or passing atoms, vectors or `select` / `update` columns, gives the same type:

- the order is b8 < u8 < i16 < i32 < i64 < f32 < f64, and the wider type wins;
- two operands of one type keep it, and an integer result wraps in that width, as i64 does: `(+ 0xff 0x01)` is `0x00`, `(* 300h 300h)` is `24464`. A result that wraps exactly onto the i16, i32 or i64 minimum is that type's null (`(+ 32767h 1h)` is `0Nh`), and both vector and indexed reads report it as null even if neither input contains a null (see the types guide);
- f32 with any other type is f64;
- two b8 operands give i64 for `+ - * div %` and b8 for `min` and `max`.

`/`, `pow` and the math functions below always return f64.

```lisp
(type (+ 2h 3i))              ; 'i32
(type (+ [2i] [true]))        ; 'I32
(type (min 2h 3i))            ; 'i32
(type (div [7i] [2h]))        ; 'I32
(type (+ (as 'F32 2.5) 1))    ; 'f64
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
nothing, and `""` selects the null rows, as `(== sym "")` does. A `GUID`
column against a string is a `type` error, for `==` and for `in` alike.

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

### `(prod x)` — aggregate

Product of all non-null numeric elements.

```lisp
(prod [2 3 4])  ; 24
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

First element of a vector.

```lisp
(first [10 20 30])  ; 10
```

### `(last x)` — aggregate

Last element of a vector.

```lisp
(last [10 20 30])  ; 30
```

### `(distinct x)` — aggregate

Unique elements of a vector or list, in the order values first appear. A list
keeps each cell where it first occurs, whether its cells are atoms, vectors or
nested lists.

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

Sort a table ascending by the named column.

```lisp
(set trades (table [sym price] (list [AAPL GOOG AAPL] [150.0 280.0 151.0])))
(xasc trades 'price)
```

### `(xdesc table col)`

Sort a table descending by the named column.

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
the result type.

```lisp
(xbar [3 7 12 18] 5)       ; [0 5 10 15]
(xbar [2000.01.10 0Nd] 7)  ; [2000.01.08 0Nd]
(xbar 0Nd 7)               ; 0Nd
```

## Higher-order Functions

Functions that take other functions as arguments for flexible iteration patterns.

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

Currently a **sequential alias for `map`**: it calls the function for each
input element in order on the calling thread's VM and returns the same result as
`map`. A larger input or a larger `-c` setting does not distribute these callback
invocations across workers. The name can signal intent, but does not currently
provide parallel lambda evaluation.

```lisp
(pmap (fn [x] (* x x)) [1 2 3])  ; [1 4 9], evaluated sequentially
(pmap + 100 [1 2 3])             ; [101 102 103]
```

It takes the same forms as `map`: exactly 2 or 3 arguments (`(pmap f x)` or
`(pmap f val x)`), and `domain` otherwise.

Eligible vector builtins and query operators can use the morsel-driven thread
pool **inside** a callback; that is separate from scheduling callbacks in
parallel. Eligibility, input-size thresholds and worker availability depend on
the operator; a large vector alone is not a guarantee of parallel execution.
See [Thread Pool](../architecture/pipeline.md#thread-pool). For independent
Rayfall task sweeps today, use separate processes and budget their workers as
described in [Multiple Instances](../guides/instances.md).

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

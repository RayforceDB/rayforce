# Math, Comparison, Aggregation & Iteration

All numeric, logical, aggregation, ordering, generation, and higher-order operations available in Rayfall. Operations marked `FN_ATOMIC` auto-map over vectors element-wise.

## Arithmetic

Arithmetic operations work on scalars and auto-map element-wise over vectors. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

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

Modulo (remainder).

```lisp
(% 10 3)  ; 1
(% [10 11 12] 3)  ; [1 2 0]
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

### `(== a b)` — atomic

Equal. Works on all types including strings and symbols.

```lisp
(== 3 3)  ; true
(== [1 2 3] 2)  ; [false true false]
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

Logical AND. Works on booleans and boolean vectors.

```lisp
(set x 5)
(and (> x 0) (< x 10))
```

### `(or a b)` — atomic

Logical OR.

```lisp
(set x 5)
(or (== x 0) (== x 10))
```

### `(not x)` — atomic

Logical NOT. Flips boolean values.

```lisp
(not true)  ; false
(not [true false true])  ; [false true false]
```

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

Arithmetic mean.

```lisp
(avg [2 4 6])  ; 4.0
```

### `(min x)` — aggregate

Minimum value.

```lisp
(min [5 1 9])  ; 1
```

### `(max x)` — aggregate

Maximum value.

```lisp
(max [5 1 9])  ; 9
```

### `(med x)` — aggregate

Median value.

```lisp
(med [1 3 5 7 9])  ; 5.0
```

### `(dev x)` — aggregate

Population standard deviation.

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

Unique elements of a vector.

```lisp
(distinct [1 2 2 3 1])  ; [1 2 3]
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

Reverse a vector.

```lisp
(reverse [1 2 3])  ; [3 2 1]
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

```lisp
(xbar [3 7 12 18] 5)  ; [0 5 10 15]
```

## Higher-order Functions

Functions that take other functions as arguments for flexible iteration patterns.

### `(map f x)`

Apply function `f` to each element of `x`. Returns a list of results.

```lisp
(map (fn [x] (* x x)) [1 2 3])  ; [1 4 9]
```

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

### `(apply f ...args)`

Apply function `f` to the given arguments.

```lisp
(apply + 1 2)  ; 3
```

### `(filter vec mask)`

Keep elements of `vec` where the boolean `mask` is true.

```lisp
(filter [1 2 3 4] (> [1 2 3 4] 2))  ; [3 4]
```

### `(pmap f x)`

Parallel map. Like `map` but distributes work across threads for large inputs.

```lisp
(pmap (fn [x] (* x x)) (til 1000000))
```

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

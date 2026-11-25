# Complete Function Reference

!!! note "Legend"
    **atomic** — auto-maps element-wise over vectors.
    **aggr** — reduces vectors to scalars (used in group-by).
    **special** — receives unevaluated arguments (special form).
    **restricted** — disabled in sandboxed mode.

## Categories

| | | |
|---|---|---|
| [Arithmetic](#arithmetic) (24) | [Comparison](#comparison) (8) | [Logic](#logic) (3) |
| [Aggregation](#aggregation) (25) | [Higher-Order](#higher-order) (13) | [Collection](#collection) (41) |
| [Sorting & Ordering](#sorting) (10) | [Control Flow & Special Forms](#control) (13) | [Table Operations](#table-ops) (21) |
| [Query](#query) (4) | [Joins](#joins) (7) | [Pivot](#pivot) (1) |
| [Linear Algebra](#linalg) (5) | [String](#string-ops) (11) | [Temporal](#temporal) (4) |
| [Type & Introspection](#type-ops) (5) | [I/O & Output](#io) (12) | [System & Utility](#system) (17) |
| [Serialization](#serialization) (5) | [Storage](#storage) (11) | [IPC](#ipc) (14) |
| [EAV Triple Store](#eav) (5) | [Datalog](#datalog) (2) | [Datalog Program API](#datalog-program) (6) |

The examples below use these small in-memory fixtures where a table is needed:

```lisp
(set trades (table [sym price size time date]
  (list [AAPL GOOG AAPL]
        [150.0 2800.0 151.0]
        [100 50 200]
        [10 20 15]
        [2024.01.15 2024.01.16 2024.01.15])))
(set quotes (table [sym time bid]
  (list [AAPL GOOG AAPL]
        [9 19 16]
        [149.5 2799.5 150.5])))
```

## Registered Builtin Surface

Listed from `src/lang/eval.c` in this checkout. The categorized reference below expands the most commonly used functions; this list is the complete registered name surface. It is maintained by hand: a builtin registered in `eval.c` is added here, and `scripts/check-builtin-docs.py` fails when one is missing (see [CONTRIBUTING](https://github.com/RayforceDB/rayforce/blob/dev/CONTRIBUTING.md#documentation)).

### Unary

`not`, `neg`, `round`, `floor`, `ceil`, `abs`, `sqrt`, `log`, `exp`, `sin`, `asin`, `cos`, `acos`, `tan`, `atan`, `reciprocal`,
`signum`, `sum`, `prod`, `all`, `any`, `count`, `avg`, `min`, `max`,
`first`, `last`, `med`, `mode`, `dev`, `stddev`, `stddev_pop`, `dev_pop`, `var`, `var_pop`, `raise`, `distinct`,
`reverse`, `til`, `lag`, `lead`, `deltas`, `ratios`, `fills`, `sums`, `avgs`, `mins`, `maxs`, `prds`,
`differ`, `asc`, `desc`, `iasc`, `idesc`, `rank`, `key`, `cols`, `value`, `type`, `read`, `read-bytes`, `load`, `exit`,
`nil?`, `where`, `group`, `raze`, `ungroup`, `ser`, `de`, `guid`, `date`, `time`, `timestamp`, `ss`, `hh`,
`minute`, `yyyy`, `mm`, `dd`, `dow`, `doy`, `eval`, `parse`, `meta`, `fkeys`, `.sys.cmd`, `.sys.listen`,
`.sys.trust`, `.os.getenv`, `.fs.size`, `.fs.list`, `.ipc.close`, `.repl.connect`, `.log.write`, `.log.replay`, `.log.replay_report`,
`.log.validate`, `rc`, `diverse`, `.time.timer.del`, `.time.sleep`, `env`, `sym-name`, `dl-stratify`, `dl-eval`, `dl-free`,
`norm`, `inv`, `flip`, `gram`, `.emb.hnsw.free`, `.emb.hnsw.get`, `.emb.hnsw.info`, `.emb.model.get`, `.emb.model.info`, `.idx.zone`, `.idx.hash`, `.idx.sort`, `.idx.bloom`,
`.idx.drop`, `.idx.has?`, `.attr.get`, `.attr.drop`, `.col.unlink`, `.col.link?`, `.col.target`,
`.graph.free`, `.graph.info`, `strlen`, `upper`, `lower`, `trim`, `.json.format`,
`.parquet.meta`, `.mc.unsub`, `.idx.uses`, `.repl.complete`, `.repl.signature`, `.repl.definition`, `.mem.objsize`

### Binary

`+`, `-`, `*`, `/`, `%`, `>`, `<`, `>=`, `<=`, `==`, `!=`, `pow`, `top`, `bot`, `pearson_corr`, `cov`, `scov`, `wsum`, `wavg`, `quantile`, `percentile`, `set`, `let`,
`try`, `filter`, `in`, `except`, `union`, `sect`, `take`, `drop`, `rotate`, `cut`, `cross`, `at`, `find`, `fill`, `min`, `max`, `msum`, `mavg`, `mmin`, `mmax`,
`mcount`, `mvar`, `mdev`, `xasc`, `xdesc`, `table`, `union-all`, `xbar`, `as`, `write`, `write-bytes`, `dict`, `concat`, `within`, `div`,
`rand`, `bin`, `binr`, `split`, `str-find`, `str-join`, `like`, `.os.setenv`, `.ipc.post`, `get`, `remove`, `row`,
`unify`, `xcol`, `xcols`, `xkey`, `xgroup`, `xrank`, `dl-query`, `dl-provenance`, `.emb.cos-dist`, `.emb.inner-prod`, `.emb.l2-dist`, `.emb.tokens`, `mmu`, `lsq`, `.emb.hnsw.set`, `.attr.set`,
`.col.link`, `identical`, `trunc`, `not-in`, `.mc.pub`

### Variadic / Special

`and`, `or`, `if`, `do`, `fn`, `map`, `pmap`, `fold`, `scan`, `prior`, `apply`, `list`, `select`, `window`,
`update`, `insert`, `upsert`, `delete`, `left-join`, `inner-join`, `full-join`, `anti-join`, `semi-mask`, `anti-mask`, `window-join`, `window-join1`,
`asof-join`, `println`, `show`, `format`, `read-csv`, `write-csv`, `.csv.read`, `.csv.splayed`, `.csv.parted`,
`.csv.write`, `resolve`, `timeit`, `enlist`, `map-left`, `map-right`, `.db.splayed.set`, `.db.splayed.append`, `.db.splayed.get`,
`.db.parted.get`, `.db.parted.tables`, `.db.parted.fill`, `alter`, `print`, `.sys.gc`, `.mem.ts`, `.sys.timeit`,
`.sys.env`, `.sys.args`, `.sys.exec`, `.idx.info`, `.ipc.send`, `.ipc.open`, `.ipc.handle`, `.ipc.handles`, `.ipc.transport`, `.repl.disconnect`, `.repl.inspect`, `.log.open`, `.log.roll`,
`.log.snapshot`, `.log.status`, `.log.posts`, `.log.sync`, `.log.close`, `.log.purge`, `quote`, `return`, `.time.now`, `.time.timer.set`,
`fold-left`, `fold-while`, `fold-right`, `scan-left`, `scan-right`, `del`, `.sys.build`, `.sys.mem`, `.sys.prof`,
`.sys.querylog`, `.sys.querylog.stats`, `.sys.querylog.enable`, `.sys.join.par.min`, `.sys.profile`, `.sys.profile.enable`, `.sys.profile.reset`, `modify`, `pivot`, `.sys.info`, `datoms`, `assert-fact`,
`retract-fact`, `scan-eav`, `pull`, `rule`, `query`, `dl-program`, `dl-add-edb`, `.emb.knn`, `.emb.hnsw.build`, `.emb.ann`, `.emb.encode`,
`.graph.build`, `.graph.pagerank`, `.graph.connected`, `.graph.dijkstra`, `.graph.louvain`, `.graph.degree`,
`.graph.topsort`, `.graph.dfs`, `.graph.cluster`, `.graph.betweenness`, `.graph.closeness`, `.graph.mst`,
`.graph.random-walk`, `.graph.k-shortest`, `.graph.shortest-path`, `.graph.expand`, `.graph.var-expand`,
`substr`, `replace`, `repr`, `read-value`, `read-limits`, `.json.parse`, `.json.get`,
`.parquet.read`, `.parquet.scan`, `.parquet.each`, `.parquet.splayed`, `.parquet.parted`, `.mc.sub`, `.mc.stats`,
`while`, `times`, `.ipc.txlimit`, `.ipc.auto_journal_eval`

## Arithmetic

Arithmetic operators auto-map over vectors and broadcast scalars. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `+` | binary | atomic | Addition | `(+ 3 4)` → `7` |
| `-` | binary | atomic | Subtraction | `(- 10 3)` → `7` |
| `*` | binary | atomic | Multiplication | `(* 3 4)` → `12` |
| `/` | binary | atomic | Float division (always returns f64) | `(/ 7 2)` → `3.5` |
| `%` | binary | atomic | Modulo (remainder) | `(% 7 3)` → `1` |
| `div` | binary | atomic | Floor division, promoted type | `(div 7 2)` → `3` |
| `neg` | unary | atomic | Negate value | `(neg 5)` → `-5` |
| `round` | unary | atomic | Round to nearest integer | `(round 3.7)` → `4.0` |
| `floor` | unary | atomic | Floor (round down) | `(floor 3.7)` → `3.0` |
| `ceil` | unary | atomic | Ceiling (round up) | `(ceil 3.2)` → `4.0` |
| `abs` | unary | atomic | Absolute value | `(abs -7)` → `7` |
| `sqrt` | unary | atomic | Square root (returns f64) | `(sqrt 9)` → `3.0` |
| `log` | unary | atomic | Natural logarithm | `(log 2.718)` → `~1.0` |
| `exp` | unary | atomic | Exponential (e^x) | `(exp 1)` → `2.718...` |
| `sin` | unary | atomic/DAG | Sine, radians; returns f64 | `(sin 0.0)` → `0.0` |
| `asin` | unary | atomic/DAG | Arcsine, radians; out-of-domain values return `0Nf` | `(asin 1.0)` → `1.570...` |
| `cos` | unary | atomic/DAG | Cosine, radians; returns f64 | `(cos 0.0)` → `1.0` |
| `acos` | unary | atomic/DAG | Arccosine, radians; out-of-domain values return `0Nf` | `(acos 1.0)` → `0.0` |
| `tan` | unary | atomic/DAG | Tangent, radians; returns f64 | `(tan 0.0)` → `0.0` |
| `atan` | unary | atomic/DAG | Arctangent, radians; returns f64 | `(atan 1.0)` → `0.785...` |
| `reciprocal` | unary | atomic/DAG | Reciprocal `1/x`; zero returns `0Nf` | `(reciprocal 4)` → `0.25` |
| `signum` | unary | atomic/DAG | Sign as i64: `-1`, `0`, or `1`; null stays null | `(signum -3.5)` → `-1` |
| `pow` | binary | atomic/DAG | Power (x^y, returns f64; lowers in query expressions) | `(pow 2 10)` → `1024.0` |
| `xbar` | binary | atomic | Round down to nearest multiple (bucketing) | `(xbar [3 7 12] 5)` → `[0 5 10]` |

```lisp
; Vector arithmetic — atomic ops broadcast scalars
(+ [1 2 3] [10 20 30])   ; [11 22 33]
(* [1 2 3] 10)            ; [10 20 30]  scalar broadcast
(abs [-3 0 5])              ; [3 0 5]
(sqrt [4 9 16])             ; [2.0 3.0 4.0]
(sin [0.0 1.57079632679])   ; [0.0 1.0]
(reciprocal [2 0 4])        ; [0.5 0Nf 0.25]
(signum [-2.5 0.0 4.0])     ; [-1 0 1]

; Time bucketing for OHLC bars
(xbar [3 15 27] 10)        ; [0 10 20]
```

## Comparison

All comparison operators are **atomic** and return boolean results.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `>` | binary | atomic | Greater than | `(> 5 3)` → `true` |
| `<` | binary | atomic | Less than | `(< 3 5)` → `true` |
| `>=` | binary | atomic | Greater than or equal | `(>= 5 5)` → `true` |
| `<=` | binary | atomic | Less than or equal | `(<= 3 5)` → `true` |
| `==` | binary | atomic | Equal | `(== 3 3)` → `true` |
| `!=` | binary | atomic | Not equal | `(!= 3 4)` → `true` |
| `within` | binary | — | Check which elements fall within an inclusive `[lo hi]` range | `(within [1 5 10] [3 7])` → `[false true false]` |
| `identical` | binary | — | Whole-value equality: same type, shape and contents; returns one boolean | `(identical [1 2] [1 2])` → `true` |

```lisp
; Vector comparisons return boolean vectors
(> [10 20 30] 15)          ; [false true true]
(== [1 2 3] [1 0 3])       ; [true false true]

; within checks the inclusive range [lo, hi]
(within [1 3 5 7] [3 6])  ; [false true true false]
```

`==` and the other operators map over elements, so on two tables or lists they return an elementwise result, not one boolean. To ask whether two values are the same value, use `identical`; it compares type, shape and contents, so `(identical (list 1 2) [1 2])` is `false` and `(identical 1 1i)` is `false`. See [`identical`](../language/math.md).

## Logic

Boolean logic operators. Work on scalars and vectors alike.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `and` | binary | — | Logical AND; element-wise on vectors and lists (lengths must match) | `(and true false)` → `false` |
| `or` | binary | — | Logical OR; element-wise on vectors and lists (lengths must match) | `(or true false)` → `true` |
| `not` | unary | — | Logical NOT; element-wise on vectors and lists | `(not true)` → `false` |

```lisp
; Combine filters with logic ops
(and (> [1 5 3] 2) (< [1 5 3] 4))  ; [false false true]
(not [true false true])    ; [false true false]
```

## Aggregation

Aggregation functions reduce vectors to scalar values. Functions marked **aggr** are recognized by the DAG executor for group-by operations.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `sum` | unary | aggr | Sum of all elements | `(sum [1 2 3])` → `6` |
| `prod` | unary | aggr | Product of all non-null numeric elements; empty/all-null returns `1` | `(prod [2 3 4])` → `24` |
| `all` | unary | aggr | True if every non-null numeric element is truthy; empty/all-null returns `true` | `(all [1 2 3])` → `true` |
| `any` | unary | aggr | True if any non-null numeric element is truthy; empty/all-null returns `false` | `(any [0 0 3])` → `true` |
| `count` | unary | aggr | Count of elements | `(count [1 2 3])` → `3` |
| `avg` | unary | aggr | Arithmetic mean | `(avg [1 2 3])` → `2.0` |
| `min` | unary aggr, or binary | aggr/atomic | Reduce one vector, or choose the element-wise minimum of two values, in their promoted type (null lowest); f32 input answers f32 | `(min [3 9 1] [2 10 4])` → `[2 9 1]`, `(min 1 2.5)` → `1.0` |
| `max` | unary aggr, or binary | aggr/atomic | Reduce one vector, or choose the element-wise maximum of two values, in their promoted type (null lowest); f32 input answers f32 | `(max [3 9 1] 5)` → `[5 9 5]`, `(max [2 0] true)` → `[2 1]` |
| `first` | unary | — | First element of a vector. A direct call takes the element by position, nulls included, for every type: `(first [0Nl 1])` is `0Nl`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(first s)` over `["" "x"]` is `"x"`, a direct `(first ["" "x"])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(first (+ d 1))`, `(first (if c d 0Nd))`); a computed argument that reads no column, such as `(first (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) | `(first [10 20 30])` → `10`, `(first [0Nl 1])` → `0Nl` |
| `last` | unary | — | Last element of a vector. A direct call takes the element by position, nulls included, for every type: `(at v (- (count v) 1))`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(last s)` over `["x" ""]` is `"x"`, a direct `(last ["x" ""])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(last (+ d 1))`, `(last (if c d 0Nd))`); a computed argument that reads no column, such as `(last (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) | `(last [10 20 30])` → `30`, `(last [1 2 0Nl])` → `0Nl` |
| `med` | unary | aggr | Median value (returns f64); a vector of another type than numbers and temporals is a type error, empty or not | `(med [1 3 2])` → `2.0` |
| `mode` | unary | aggr | Most frequent non-null value; ties keep the first encountered value | `(mode [1 2 2 3])` → `2` |
| `dev` | unary | aggr | Population standard deviation; a vector of another type than numbers and temporals is a type error, empty or not | `(dev [2 4 4 4 5 5 7 9])` → `2.0` |
| `stddev` | unary | aggr | Sample standard deviation (Bessel-corrected; not an alias of `dev`); a vector of another type than numbers and temporals is a type error, empty or not | `(stddev [1 2 3])` → `1.0` |
| `stddev_pop` | unary | aggr | Population standard deviation | `(stddev_pop [1 2 3])` |
| `dev_pop` | unary | aggr | Population standard deviation (alias) | `(dev_pop [1 2 3])` |
| `var` | unary | aggr | Sample variance; a vector of another type than numbers and temporals is a type error, empty or not | `(var [1 2 3])` → `1.0` |
| `var_pop` | unary | aggr | Population variance ([accuracy](../language/math.md#variance-and-standard-deviation-accuracy)) | `(var_pop [1 2 3])` |
| `pearson_corr` | binary | aggr | Pearson correlation over paired non-null numeric inputs | `(pearson_corr [1 2 3] [2 4 6])` → `1.0` |
| `cov` | binary | aggr | Population covariance over paired non-null numeric inputs | `(cov [1 3] [2 6])` → `2.0` |
| `scov` | binary | aggr | Sample covariance over paired non-null numeric inputs | `(scov [1 3] [2 6])` → `4.0` |
| `wsum` | binary | aggr | Weighted sum, `sum(weights * values)`, skipping null pairs | `(wsum [1 3] [10 20])` → `70.0` |
| `wavg` | binary | aggr | Weighted average, `wsum / sum(weights)`; null for zero total weight | `(wavg [1 3] [10 20])` → `17.5` |
| `quantile` | binary | aggr | Exact linear-interpolated quantile; probability is `0.0..1.0`; a vector of another type than numbers and temporals is a type error, empty or not | `(quantile [1 2 3 4] 0.5)` → `2.5` |
| `percentile` | binary | aggr | Exact linear-interpolated percentile; probability is `0..100` | `(percentile [1 2 3 4] 50)` → `2.5` |

Numeric reducers skip nulls where applicable. Pairwise binary reducers skip a row when either vector input is null. Inside `select`/`by:`, these reducers lower to morsel-based DAG aggregation paths that can run in parallel and poll for cancellation.

```lisp
; Basic aggregation
(sum [10 20 30])           ; 60
(avg [10 20 30])           ; 20.0
(med [1 100 5])            ; 5
(mode [1 2 2 3])           ; 2
(quantile [1 2 3 4] 0.5)   ; 2.5

; Group-by aggregation in select
(select {from: trades
         by:   {sym: sym}
         hi: (max price) lo: (min price) n: (count price)})
```

## Linear Algebra { #linalg }

A matrix is a list of equal-length numeric vectors, one per row. F64 and F32 rows are used in place; I32 and I64 rows are widened to F64, their nulls becoming `0Nf` as in an F64 row. The result is F32 when every operand is F32, otherwise F64 (the dot product of two vectors is always an F64 atom); `mmu` and `gram` compute in the operands' own precision, F32 rows in single and F64 rows in double, while `inv` and `lsq` always solve in double; a result outside the range of its type is null. `inv` and `lsq` raise `domain` for an input that holds a null, for a singular matrix (a pivot at or below `n·ε` times the largest element) and for a rank-deficient `a`; shape mismatches raise `length`, and a vector where a matrix is required raises `type`.

| Function | Type | Attr | Description | Example |
|---|---|---|---|---|
| `mmu` | binary | — | Matrix product: matrix × matrix, matrix × vector, vector × matrix; two vectors give their dot product | `(mmu (list [1.0 2.0] [3.0 4.0]) [1.0 1.0])` → `[3.0 7.0]` |
| `inv` | unary | — | Inverse of a square matrix | `(inv (list [1.0 2.0] [3.0 4.0]))` → `[[-2.0 1.0] [1.5 -0.5]]` |
| `lsq` | binary | — | `(lsq b a)` is `b mmu inv a` for a square `a`; for an `a` with more columns than rows, the least-squares solution | `(lsq [1.0 2.0] (list [2.0 1.0] [1.0 3.0]))` → `[0.2 0.6]` |
| `flip` | unary | — | Transpose of a matrix; rows of one type keep it, mixed rows widen to F64 | `(flip (list [1 2 3] [4 5 6]))` → `[[1 4] [2 5] [3 6]]` |
| `gram` | unary | — | `x mmu flip x` without forming the transpose; symmetric, computed on the upper triangle | `(gram (list [1.0 2.0 3.0] [4.0 5.0 6.0]))` → `[[14.0 32.0] [32.0 77.0]]` |

## Higher-Order Functions { #higher-order }

Functions that take other functions as arguments for mapping, folding, and filtering.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `map` | variadic | — | `(map f v)` applies `f` to each element of `v`; `(map f val v)` calls `f(val, elem)` for each element, passing `val` whole each time (a collection `val` gives one row per element; same as `map-right`). Same-type atom results form a typed vector, otherwise a list. String results that repeat shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fail with `limit`. Exactly 2 or 3 arguments | `(map (fn [x] (* x 2)) [1 2 3])` → `[2 4 6]`, `(map + 100 [1 2 3])` → `[101 102 103]` |
| `pmap` | variadic | — | Currently a sequential alias for `map`, with the same result types and the same 2 or 3 arguments | `(pmap (fn [x] (* x x)) [1 2 3])` → `[1 4 9]` |
| `fold` | variadic | — | Reduce with function and initial value | `(fold + 0 [1 2 3])` → `6` |
| `fold-left` | variadic | — | Left-associative fold | `(fold-left - 10 [1 2 3])` → `4` |
| `fold-while` | variadic | — | Fold that stops when the accumulator fails pred | `(fold-while (fn [a] (< a 100)) + 0 (til 1000))` → `105` |
| `fold-right` | variadic | — | Right-associative fold | `(fold-right - 10 [1 2 3])` → `-8` |
| `scan` | variadic | — | Running fold (all intermediate results) | `(scan + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-left` | variadic | — | Left-to-right running fold | `(scan-left + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-right` | variadic | — | Right-to-left running fold (returns a list) | `(scan-right + (enlist 1 2 3))` → `(6 5 3)` |
| `filter` | binary | — | Keep elements where boolean mask is true | `(filter [1 2 3 4] (> [1 2 3 4] 2))` → `[3 4]` |
| `apply` | variadic | — | `(apply f a b)` calls `f` on the elements of two collections taken in step, `f(a_0, b_0)`, `f(a_1, b_1)`, ...; the result is as long as the shorter one. Results are collected as `map`'s are. Two atoms call `f` once. Exactly 3 arguments | `(apply + [1 2 3] [10 20 30])` → `[11 22 33]` |
| `map-left` | variadic | — | Map each element of the left over the whole right | `(map-left + 10 [1 2 3])` → `[11 12 13]` |
| `map-right` | variadic | — | Map the whole left over each element of the right | `(map-right - [10 20 30] 5)` → `[5 15 25]` |
| `prior` | variadic | — | Apply a binary fn to each element and its predecessor (first paired with itself). A comparison, or `+` `-` `*` over an `I16`/`I32`/`I64`/`F64` vector, returns a typed vector; any other fn returns a list | `(prior - [1 3 6 10])` → `[0 2 3 4]` (`I64`) |

```lisp
; Transform each row with map
(map (fn [x] (* x x)) [1 2 3 4])  ; [1 4 9 16]

; pmap currently uses the same sequential callback loop as map
(pmap (fn [t] (sum (til t))) [100 200 300])

; Running sum with scan
(scan + (enlist 1 2 3 4))  ; [1 3 6 10]
```

`map` and `pmap` return a typed vector when every result is an atom of
exactly the same vector-supported type, including strings, symbols and
representable typed nulls. Native boolean/byte null atoms remain boxed
because those vector types are non-nullable. Opaque graph, HNSW index and
Datalog program handles also remain boxed: their integer payload alone cannot
represent the handle's resource ownership. Mixed atom types (including
integers mixed with floats), nested results and untyped `null` results
remain a `LIST`. Empty input returns an empty `LIST`, since there is no
result type to infer. A binary map with a scalar third argument still calls
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

## Collection Operations { #collection }

Operations on vectors and lists as collections — set operations, indexing, searching, and construction.

Outside queries, `in` matches null to null. `not-in` inverts each membership result, preserving nested list/vector shapes. For numeric, temporal, SYM, and GUID membership evaluated during a query (including called functions), null input rows return `false` for both operations. STR membership is not supported by the typed query-plan kernel. A string atom tested against a list is one whole value and returns one boolean: `(in "ab" (list "ab" "c"))` → `true`. A string tested against a string returns one result per character: `(in "ab" "abc")` → `[true true]`. A vector or list of strings is tested element by element: `(in ["p" "qq"] (list "qq"))` → `[false true]`. In a query, a `SYM` column against a string, a string vector or a list of strings reads each string as the symbol it spells, as `==` does: `(select {from: t where: (in s (list "a" "b"))})` keeps the rows holding `'a` or `'b`; each string is looked up in the column's own symbols (a string no row holds matches nothing), and `""` selects the null rows, as `(== s "")` does, whether the set is written out, bound to a name or computed while the query runs (`(in s (concat ["a"] [""]))`, `(in s (f "a"))`); a `GUID` column against strings is a `type` error, as for `==`.

Query `concat` joins text row by row when an operand comes from a source column, including computed text columns. Scalar text operands also retain query string concatenation. Literal and bound collections keep ordinary `concat` semantics, as do calls inside user functions whose operands do not derive from the query's rows. Numeric collection concatenation evaluates over the whole column, or each group inside an aggregate. Non-text collection concatenation is binary. Scalars broadcast to the projected collection length; a one-row input column is still a column. Non-scalar output columns of different lengths are rejected before `take:`; a standalone collection projection may change the row count. When a `where:` selection is applied to a collection projection, its rows must align with the input rows. A `concat` operand the planner has no node for (a reordered, indexed or cast column) is evaluated over the whole column in a projection, matching update, `where:` and `by:`. Inside a helper body a quoted symbol is the symbol itself and a nested helper's free names are globals or source columns, never its caller's formals; helpers built from pure builtins are inlined, never closures. Grouped outputs the planner cannot serve natively keep the requested output order; an output named like a group key keeps both columns, the key first. Helpers the planner cannot inline run as bytecode from a query fallback with row provenance tracked per value: `concat` keeps text semantics through `let`, nested calls and results, a bound vector stays a collection, and a free source-column name is the row's cell (or the whole column) rather than an output alias. Such a helper can also be the `by:` key, evaluated over the rows `where:` keeps: a body built from pure builtins and pure helpers runs whole first, and again row by row when a conditional in it tested a vector or the whole-column run failed (a failed run is swallowed whole, its `raise` payload included); a body with an effect (`set`, `eval`, system or I/O builtins, a computed or unknown callee) runs row by row from the start, so the effect happens once per row, whether the key is written bare (`by: (h x)`) or as a by-dict value (`by: {k: (h x)}`), and the select reads every partition of a parted table for it, skipping none by statistics; callees are followed wherever they are bound, so a pure helper reached through a parameter, a `let` or a capture keeps the whole-column semantics. A source column shadows a global of the same name inside a helper, on the planned path and in fallbacks alike. A text cell reduced from a column (`first`, `last`, `min`, `max`) is a cell of those rows, so a grouped `(concat (first s) 'zz)` joins each group's cell with the literal, an empty cell is empty text, and a cell of a STR column keeps STR output, at every operand position (a query `concat` takes up to 255 operands).

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `distinct` | unary | — | Remove duplicates, keeping each value at its first occurrence, in the order the values first appear, for every type, list cells included; a null is kept once; `-0.0` equals `0.0` in float vector cells, as between atoms | `(distinct [3 1 3 2])` → `[3 1 2]` |
| `in` | binary | — | Membership test; a string in a list is one whole value | `(in "ab" (list "ab" "c"))` → `true` |
| `not-in` | binary | — | Negated membership test | `(not-in 2 [1 3])` → `true` |
| `except` | binary | — | Elements of A not in B. Not a set: the duplicates of A are kept | `(except [1 1 2 3] [3])` → `[1 1 2]` |
| `union` | binary | — | Set union: distinct elements of the left operand, then those of the right it lacks, in order of first appearance | `(union [1 2] [2 3])` → `[1 2 3]` |
| `sect` | binary | — | Elements of A that are also in B. Not a set: the duplicates of A are kept | `(sect [1 1 2 2 3] [1 2])` → `[1 1 2 2]` |
| `take` | binary | — | Take first N elements (negative N takes from end); past the length it cycles. A parted table handle (`.db.parted.get`) is taken as its flat table: the same rows `(at table (til n))` returns, cycling past the row count | `(take [10 20 30] 2)` → `[10 20]`; `(take [1 2 3] -5)` → `[2 3 1 2 3]` |
| `drop` | binary | — | Drop first N elements (negative N drops from end) | `(drop [10 20 30] 1)` → `[20 30]` |
| `rotate` | binary | — | Rotate left by N positions (negative rotates right) | `(rotate [1 2 3 4] 1)` → `[2 3 4 1]` |
| `cut` | binary | — | Split a collection at sorted 0-based indices | `(cut [1 2 3 4] [2])` → `([1 2] [3 4])` |
| `cross` | binary | — | Cartesian product as pairs | `(cross [1 2] ['a 'b])` → `((1 'a) (1 'b) (2 'a) (2 'b))` |
| `at` | binary | — | Index a collection; vector keys batch dictionary lookups. A typed vector indexed by an integer vector keeps its type: a null or out-of-range index gives the type's null (`false`/`0x00` for BOOL/U8) | `(at [10 20 30] 1)` → `20`, `(at [10 20 30] [2 9])` → `[30 0Nl]` |
| `find` | binary | — | Find index of first occurrence | `(find [10 20 30] 20)` → `1` |
| `fill` | binary | atomic | Replace null values; the replacement is the first argument; f32 input stays f32 | `(fill 0 [1 0Nl 3])` → `[1 0 3]` |
| `reverse` | unary | — | Reverse element order; a table reverses its rows (columns and types kept), a dictionary its entries (keys and values together) | `(reverse [1 2 3])` → `[3 2 1]` |
| `til` | unary | — | Generate range [0..n) | `(til 5)` → `[0 1 2 3 4]` |
| `lag` | unary | lazy/DAG | Shift values one row back; first row is null/sentinel | `(lag [10 20 30])` → `[0Nl 10 20]` |
| `lead` | unary | lazy/DAG | Shift values one row forward; last row is null/sentinel | `(lead [10 20 30])` → `[20 30 0Nl]` |
| `deltas` | unary | lazy/DAG | Adjacent differences, of the type `-` gives: TIMESTAMP → TIMESPAN, TIME → TIMESPAN, DATE → I32 (days), TIMESPAN → TIMESPAN, other integers → I64, floats → F64; first row is null | `(deltas [10 15 13])` → `[0Nl 5 -2]` |
| `ratios` | unary | lazy/DAG | Adjacent ratios as f64; first row is null | `(ratios [2 4 8])` → `[0Nf 2.0 2.0]` |
| `fills` | unary | lazy/DAG | Forward-fill nullable vectors, or every column of a table | `(fills (as 'I64 (list 0N 2 0N)))` → `[0Nl 2 2]` |
| `sums` | unary | lazy/DAG | Running sum; nulls are skipped | `(sums [1 2 3])` → `[1 3 6]` |
| `avgs` | unary | lazy/DAG | Running average over non-null values | `(avgs [2 4 6])` → `[2.0 3.0 4.0]` |
| `mins` | unary | lazy/DAG | Running minimum; f32 input stays f32 | `(mins [3 1 2])` → `[3 1 1]` |
| `maxs` | unary | lazy/DAG | Running maximum; f32 input stays f32 | `(maxs [3 1 2])` → `[3 3 3]` |
| `prds` | unary | lazy/DAG | Running product; nulls are skipped | `(prds [2 3 4])` → `[2 6 24]` |
| `differ` | unary | lazy/DAG | Boolean change flag versus previous row; first row is true | `(differ [1 1 2])` → `[true false true]` |
| `msum` | binary | lazy/DAG | Moving sum over trailing N rows; nulls are skipped | `(msum 3 [1 2 3 4])` → `[1 3 6 9]` |
| `mavg` | binary | lazy/DAG | Moving average over trailing N rows and non-null values | `(mavg 3 [1 2 3 4])` → `[1.0 1.5 2.0 3.0]` |
| `mmin` | binary | lazy/DAG | Moving minimum over trailing N rows; f32 input stays f32 | `(mmin 3 [3 2 4 1])` → `[3 2 2 1]` |
| `mmax` | binary | lazy/DAG | Moving maximum over trailing N rows; f32 input stays f32 | `(mmax 3 [3 2 4 1])` → `[3 3 4 4]` |
| `mcount` | binary | lazy/DAG | Moving non-null count over trailing N rows | `(mcount 3 [1 2 3 4])` → `[1 2 3 3]` |
| `mvar` | binary | lazy/DAG | Moving population variance over trailing N rows and non-null values | `(mvar 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `mdev` | binary | lazy/DAG | Moving population standard deviation over trailing N rows and non-null values | `(mdev 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `enlist` | variadic | — | Wrap value(s) in a vector (list of atoms) | `(enlist 1 2 3)` → `[1 2 3]` |
| `concat` | binary | — | Join two atoms, vectors or lists, mixed types giving a list; an atom joins a vector only of its own type, and a string joins each cell of a `STR` vector (`(concat "ab" ["c"])` → `["abc"]`, so `(concat "ab" [1 2])` is a `type` error); merge two dicts; append the rows of a table to a table that has each of its columns, of the same types (a parted table is refused: use `union-all`). The STR null `""` is empty text, in a direct call and on every query route: `(concat "" "a")` is `"a"`, and `(concat s "")` in a select keeps each cell of `s` | `(concat [1 2] [3 4])` → `[1 2 3 4]` |
| `raze` | unary | — | Flatten a list of vectors into one vector; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(raze (list [1 2] [3 4]))` → `[1 2 3 4]` |
| `where` | unary | — | Indices where boolean vector is true | `(where [true false true])` → `[0 2]` |
| `group` | unary | — | Group indices by value (returns dict) | `(group ['a 'b 'a])` → dict |
| `diverse` | unary | — | Check if all elements are unique (no duplicates) | `(diverse [1 2 3])` → `true` |
| `rand` | binary | — | Generate N random values from range or sample from vector. **Deterministic**: the same sequence in every process | `(rand 3 100)` → 3 random ints 0..99 |
| `bin` | binary | — | Binary search — left boundary (sorted input) | `(bin [10 20 30] 25)` → `1` |
| `binr` | binary | — | Binary search — right boundary (sorted input) | `(binr [10 20 30] 25)` → `2` |

```lisp
; Build and manipulate vectors
(set v (til 10))             ; [0 1 2 3 4 5 6 7 8 9]
(take v 3)                  ; [0 1 2]
(take v -3)                 ; [7 8 9]
(drop v 2)                  ; [2 3 4 5 6 7 8 9]
(rotate v 3)                ; [3 4 5 6 7 8 9 0 1 2]
(cut v [3 7])               ; ([0 1 2] [3 4 5 6] [7 8 9])
(at v (where (> v 5)))      ; [6 7 8 9]

; Set operations
(union [1 2 3] [3 4 5])   ; [1 2 3 4 5]
(union [1 1 2] [2 3 3])   ; [1 2 3]    a set: duplicates go, from either operand
(sect [1 2 3] [2 3 4])    ; [2 3]
(sect [1 1 2 2] [1 2])    ; [1 1 2 2]  keeps the duplicates of the left operand
(except [1 2 3] [2])      ; [1 3]
(except [1 1 2] [3])      ; [1 1 2]    keeps the duplicates of the left operand

; union is the only one of the three that returns a set: use (distinct (sect a b))
; or (distinct (except a b)) when a set is wanted from the other two.

; Time-series vector helpers
(deltas [10 15 13])       ; [0Nl 5 -2]
(fills (as 'I64 (list 0N 2 0N))) ; [0Nl 2 2]
(sums [1 2 3 4])          ; [1 3 6 10]
(differ [1 1 2 2])        ; [true false true false]
(msum 3 [1 2 3 4])        ; [1 3 6 9]
(mavg 3 [1 2 3 4])        ; [1.0 1.5 2.0 3.0]
(mcount 3 [1 2 3 4])      ; [1 2 3 3]
(mvar 2 [1 3 5])          ; [0.0 1.0 1.0]
(mdev 2 [1 3 5])          ; [0.0 1.0 1.0]
```

Time-series vector helpers are lazy-aware DAG operations for vector inputs. Moving-window helpers take a positive integer window first, then the vector. Constant windows inside `select` lower into DAG nodes; dynamic windows evaluate through the normal function path. These functions materialize through morsel-based kernels, can run in parallel, and poll the query cancellation flag during execution.

## Sorting & Ordering { #sorting }

Sort vectors, compute sort indices, and rank elements.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `asc` | unary | — | Sort ascending | `(asc [3 1 2])` → `[1 2 3]` |
| `desc` | unary | — | Sort descending | `(desc [3 1 2])` → `[3 2 1]` |
| `iasc` | unary | — | Indices that would sort ascending (grade up) | `(iasc [30 10 20])` → `[1 2 0]` |
| `idesc` | unary | — | Indices that would sort descending (grade down) | `(idesc [30 10 20])` → `[0 2 1]` |
| `rank` | unary | — | Rank of each element (0-based) | `(rank [30 10 20])` → `[2 0 1]` |
| `top` | binary | — | Largest N elements (descending) | `(top [5 1 9 3] 2)` → `[9 5]` |
| `bot` | binary | — | Smallest N elements (ascending) | `(bot [5 1 9 3] 2)` → `[1 3]` |
| `xasc` | binary | — | Sort table ascending by column(s) | `(xasc trades 'price)` |
| `xdesc` | binary | — | Sort table descending by column(s) | `(xdesc trades 'price)` |
| `xrank` | binary | — | Assign N rank buckets (quantile ranking) | `(xrank 4 [10 20 30 40])` → `[0 1 2 3]` |

```lisp
; Sort indices for reordering
(set v [30 10 20])
(at v (iasc v))              ; [10 20 30]  same as (asc v)

; Sort a table by price descending
(xdesc trades 'price)

; Quantile buckets
(xrank 10 (til 100))        ; 10 equal-sized buckets 0..9
```

## Control Flow & Special Forms { #control }

Special forms receive their arguments unevaluated. These are the core language primitives for binding, branching, function definition, and error handling.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `set` | binary | special, restricted | Bind value to global variable | `(set x 42)` |
| `let` | binary | special | Bind value to local variable (lexical scope); at the top level it binds a global, refused for a restricted caller | `(let y (+ x 1))` |
| `if` | variadic | special | Conditional: (if c1 r1 c2 r2 ... else?) — first truthy condition wins. A boolean-vector condition chooses per row between the two arms, as in a query: both arms are always evaluated (their side effects happen, and an arm that raises raises), arms are vectors of its length or atoms, and a length-1 vector gives a 1-element vector. In a query the arms give one column: numbers promote, a temporal beside a null keeps its type, two different temporal types (DATE, TIME, TIMESTAMP, TIMESPAN) or a temporal beside a number raise `type` | `(if (> x 0) "pos" "neg")`, `(if [true false] [1 2] [3 4])` → `[1 4]` |
| `do` | variadic | special | Sequential execution, returns last value. Inside a query the earlier forms run once per query, over the selected rows, and the last form is planned as if alone, seeing their `let`s ([`do` inside a query](../language/control-flow.md#do-inside-a-query)) | `(do (set x 1) (set y 2) (+ x y))` |
| `while` | variadic | special | Iterate while cond is truthy; returns null. A vector condition is one truth value (non-empty is true); in a query, a vector as long as the rows (or the group) is true or false when every cell agrees and otherwise raises `nyi`, whatever computes it ([control flow](../language/control-flow.md#iteration-while)) | `(while (> n 0) (set n (- n 1)))` |
| `times` | variadic | special | Run body exactly n times (count evaluated once); `(times [i n] ...)` binds `i` to 0..n-1, one per pass (at the top level a global, refused for a restricted caller); returns null | `(times 5 (set n (+ n 1)))`, `(times [i 3] (println i))` |
| `fn` | variadic | special | Create lambda function | `(fn [x y] (+ x y))` |
| `try` | binary | special | Error handling: (try expr handler-fn-or-fallback-value); the handler gets the raised value, or `{code msg}` for an engine error (`{code msg usage}` when it is about a builtin's call) | `(try (+ 1 'a) (fn [e] (at e 'code)))` → `'type` |
| `raise` | unary | — | Throw an error with message | `(raise "bad input")` |
| `return` | unary | — | Early return from function body | `(return 42)` |
| `quote` | variadic | special | Return argument unevaluated; a bare name yields a literal symbol (`(quote x)` ≡ `'x`) | `(quote (+ 1 2))` → `(+ 1 2)` |
| `alter` | variadic | special | Amend a named local or global binding and return its name: `(alter 'name set idx val)` writes `val` at an index or index vector (for a dict, `idx` is a key or a vector of keys: an existing key is replaced, a missing one is added), `(alter 'name concat vals)` appends, `(alter 'name remove idx)` drops list items. On a typed vector `val` must be of the vector's kind: a number into a numeric vector (converted to the vector's type), otherwise a value of the vector's own type, and a null of any type, or a value vector of nulls only, writes the vector's null; anything else, or a value vector whose length is not the index count, is a `type` error and the vector is unchanged. A SYM vector read from a table saved with a symbol file takes any symbol, held by that file or not; the write stays in memory and the file is not changed. The innermost visible local or parameter takes precedence over a global, through a literal `alter` head or a global alias of it; shared values retain copy-on-write isolation | `(alter 'v set 0 99)` |
| `del` | unary | special, restricted | Unbind one variable or dotted-path leaf and return `0`; deleting the last leaf of a dotted path removes the intermediate dicts too. Extra arguments raise `arity` | `(del cfg.db.host)` |

```lisp
; Variable binding and branching
(set threshold 100)
(set classify (fn [x]
  (if (> x threshold) "high"
      (> x 50)        "mid"
                        "low")))

; Error handling
(try
  (/ 100 0)
  (fn [err] (println "caught:" err) 0))
```

## Table Operations { #table-ops }

Create and manipulate tables, dictionaries, and their metadata.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `list` | variadic | — | Create a list from vectors (column data for tables) | `(list [1 2] ['a 'b])` |
| `table` | binary | — | Create table from column names and list of vectors; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(table [x y] (list [1 2] ['a 'b]))` |
| `dict` | binary | — | Create dictionary from evaluated keys and values vectors | `(dict ['a 'b] [1 2])` |
| `key` | unary | — | Get column names (table) or keys (dict) | `(key trades)` → `[sym price size]` |
| `cols` | unary | — | Get table column names | `(cols trades)` → `[sym price size time date]` |
| `value` | unary | — | Get column data (table) or values (dict) | `(value d)` |
| `get` | binary | — | Lookup key in dict or column in table | `(get d 'a)` → `1` |
| `remove` | binary | — | Remove key from dictionary | `(remove d 'a)` |
| `row` | binary | — | Extract single row from table as dict | `(row trades 0)` |
| `meta` | unary | — | Get table metadata (column types, lengths) | `(meta trades)` |
| `xcol` | binary | — | Rename all table columns; names must match column count | `(xcol trades [ticker px sz tm dt])` |
| `xcols` | binary | — | Project/reorder named columns | `(xcols trades [size sym])` |
| `xkey` | binary | — | Build a dict keyed by unique column value(s), with row dict values for remaining columns | `(xkey trades 'time)` |
| `xgroup` | binary | — | Group rows by column value(s), returning dict key -> table slice | `(xgroup trades 'sym)` |
| `fkeys` | unary | — | Inspect linked-column metadata as `column -> target table` | `(fkeys fact)` |
| `union-all` | binary | — | Concatenate two tables (all rows, no dedup). A parted operand (`.db.parted.get`) is read into memory first: the result is a plain table, and the partition key becomes a column | `(union-all t1 t2)` |
| `unify` | binary | — | Merge two tables/dicts (second takes precedence) | `(unify d1 d2)` |
| `modify` | variadic | restricted | Functional table update (returns new table) | `(modify trades 'price (fn [p] (* p 1.1)))` |
| `delete` | unary | special, restricted | Remove the rows a predicate matches: `{from: 't where: p}` removes them in place, `{from: t where: p}` returns a new table. Both clauses are required | `(delete {from: 'trades where: (> price 100)})` |
| `ungroup` | unary | — | Flatten a grouped table's nested list columns into one row per element | `(ungroup gt)` |
| `pivot` | variadic | — | Pivot table — reshape long to wide format | `(pivot trades 'sym 'date 'price sum)` |

`xcol` requires exactly one new name for each existing column. `xcols`, `xkey`, and `xgroup` accept a symbol atom, a symbol vector, or a list of symbol atoms. `xkey` is a dictionary projection, not a native keyed-table type: key columns must be unique, and each value is a row dictionary containing the non-key columns. Use `xgroup` when keys may repeat; each dictionary value is a table slice for that group. Passing a typed vector of keys to `at` performs a batch lookup and returns a list.

Dictionary literals are literals: their entries are not evaluated, just as the
elements of other literals are not evaluated. Use `(dict keys values)` to
construct a dictionary from evaluated expressions.

```lisp
; Create a table
(set trades (table [sym price size]
  (list [AAPL GOOG AAPL]
        [150.0 2800.0 151.0]
        [100 50 200])))

; Dictionary operations
(set d (dict [name age] (list "Alice" 30)))
(get d 'name)              ; "Alice"
(key d)                      ; [name age]

; Schema/projection helpers
(cols trades)                ; [sym price size]
(xcols trades [size sym])
(xgroup trades 'sym)         ; dict: sym -> table slice

; Pivot: long to wide (5th arg is the aggregation fn)
(set trades (table [sym date price size]
  (list [AAPL GOOG AAPL]
        [2024.01.15 2024.01.15 2024.01.16]
        [150.0 2800.0 151.0]
        [100 50 200])))
(pivot trades 'sym 'date 'price sum)
```

## Query Operations { #query }

Special forms that bridge to the Rayforce DAG executor for high-performance columnar queries.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `select` | variadic | special | Query table with optional filter, projection, grouping, and aggregation | `(select {from: t a: a})` |
| `window` | variadic | special | Partitioned window query. `frame:` is `'whole`, `'running`, or a positive trailing row count. `first` and `last` give the frame's first or last row, a null included, in the value column's type; `min` and `max` skip nulls and keep the value column's type too, ordering SYM and STR by text and GUID by bytes, as `min` and `max` do outside a window | `(window {from: t part: [sym] order: [time] frame: 5 funcs: {avg5: (avg price)}})` |
| `update` | variadic | special, restricted | Add or modify columns in a table (mutates in-place) | `(update {from: t b: (* a 2)})` |
| `insert` | variadic | special, restricted | Append rows, or grow a parted table's in-memory live tail with an explicit partition key; a quoted target names the innermost visible local or parameter before a global, through a literal `insert` head or a global alias of it | `(insert 't 2024.01.16 rows)` |
| `upsert` | variadic | special, restricted | Insert or update flat-table rows by key match (target, key, row); a null key matches a null-keyed row for every key type, and a key value the key column cannot hold is a `domain` error | `(upsert t 'x {x: 10 y: 20})` |

```lisp
; Select with filter and projection
(select {from: trades
         where: (> price 100)
         sym: sym notional: (* price size)})

; Group-by with VWAP
(select {from: trades
         by:   {sym: sym}
         vwap: (/ (sum (* price size)) (sum size))
                n: (count price)})

; Update: add computed column
(update {from: trades
         notional: (* price size)})
```

## Joins

Rayforce supports seven join types, including time-series-aware as-of and window joins.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `left-join` | variadic | — | Left join — all left rows, unmatched filled with null. Keys are a symbol list. | `(left-join [sym] trades quotes)` |
| `inner-join` | variadic | — | Inner join — only matching rows from both sides | `(inner-join [product_id] orders products)` |
| `full-join` | variadic | — | Full outer join — all rows from both sides, unmatched columns filled with null | `(full-join [sym] trades quotes)` |
| `anti-join` | variadic | — | Anti-semi-join — left rows with no right match | `(anti-join [key] t1 t2)` |
| `semi-mask` | variadic | — | Boolean vector over left rows — true where the row has a right match | `(semi-mask [key] t1 t2)` |
| `anti-mask` | variadic | — | Boolean vector over left rows — true where the row has no right match | `(anti-mask [key] t1 t2)` |
| `window-join` | variadic | special | Window join — `[eq-keys... time-key]`, intervals, left, right, agg dict | `(window-join [sym time] iv t1 t2 {avg_bid: (avg bid)})` |
| `window-join1` | variadic | special | Window join variant (strict window, no prevailing quote) | `(window-join1 [sym time] iv t1 t2 {avg_bid: (avg bid)})` |
| `asof-join` | variadic | — | As-of join — match most recent preceding value. Keys come first, last key is the time key. | `(asof-join [sym time] trades quotes)` |

```lisp
; Fixtures for join examples
(set trades (table [sym price size time]
  (list [AAPL GOOG AAPL]
        [150.0 2800.0 151.0]
        [100 50 200]
        [10 20 15])))
(set quotes (table [sym time bid]
  (list [AAPL GOOG AAPL]
        [9 19 16]
        [149.5 2799.5 150.5])))

; Left join on sym column (join keys are a symbol list)
(left-join [sym] trades quotes)

; Window join: keys are [equality-keys... time-key]; intervals is
; a two-vector list with one [lo hi] window bound per left row.
(set intervals (map-left + [-2 2] (at trades 'time)))
(window-join [sym time]
             intervals
             trades quotes
             {avg_bid: (avg bid)})

; As-of join: keys come first, the last key is the time key
(asof-join [sym time] trades quotes)
```

## Pivot

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `pivot` | variadic | — | Pivot table — reshape long to wide. Args: table, index col, pivot col, value col, agg fn | `(pivot sales 'region 'product 'revenue sum)` |

Each value column is named `(format "%" v)` of its pivot value (`2024.01.01`, `1.5`, `0x01`, `true`), in first-seen order. Rows with a null pivot value are kept in one column named by the type's null (`0Nl`, `0Nd`, `'` for the null symbol). Floats that `%g` writes alike take their round-trip digits, and a name an index column or an earlier column already has takes the smallest free suffix `_1`, `_2`, ... The rule is the same for every aggregation; see [Column names](../queries/pivot.md#column-names).

```lisp
; Pivot sales data: rows=region, cols=product, values=revenue, aggregated with sum
(set sales (table [region product revenue]
  (list ['east 'east 'west 'west]
        ['widgets 'gadgets 'widgets 'gadgets]
        [100 200 150 250])))
(pivot sales 'region 'product 'revenue sum)
```

## String Operations { #string-ops }

String functions are available as direct builtins. The transform functions are lazy-aware for vector inputs and share the same DAG opcodes used by `select`/`update`.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `split` | binary | — | Split string by delimiter into a list of strings; a string or symbol vector, or a list of string or symbol atoms, splits each element into its own list | `(split "a,b,c" ",")` → `("a" "b" "c")` |
| `str-find` | binary | parallel vector | First byte index of a literal substring (no wildcards; see `like`); `0Nl` when absent | `(str-find "banana" "na")` → `2` |
| `str-join` | binary | — | Join string/symbol atoms, vectors, or lists with a delimiter | `(str-join ["a" "b"] ",")` → `"a,b"` |
| `strlen` | unary | — | Length of each string. `""` is the STR/SYM null: `strlen` maps it to a null length, `sum` and `avg` skip it, and the `avg` of an all-empty group is null. | `(strlen "hello")` → `5` |
| `upper` | unary | lazy/DAG | Uppercase string or symbol atoms/vectors | `(upper ["ab" "Cd"])` → `["AB" "CD"]` |
| `lower` | unary | lazy/DAG | Lowercase string or symbol atoms/vectors | `(lower 'AbC)` → `'abc` |
| `trim` | unary | lazy/DAG | Strip leading/trailing whitespace | `(trim "  abc  ")` → `"abc"` |
| `substr` | variadic | lazy/DAG | Substring by 1-based start and length; 0 clamps to start | `(substr "abcdef" 2 3)` → `"bcd"` |
| `replace` | variadic | lazy/DAG | Replace all occurrences of a string/symbol atom. An empty `to` deletes each match; an empty `from` matches nothing and returns the input | `(replace "a-b" "-" "_")` → `"a_b"` |
| `like` | binary | — | Glob-style pattern match (* and ? wildcards). The pattern is a string atom, or a string column matched row by row (`(like t pat)`); a symbol or a number is a `type` error | `(like "hello" "hel*")` → `true` |
| `sym-name` | unary | — | Resolve integer sym IDs to symbols (passthrough for sym atoms) | `(sym-name 0)` → sym at ID 0 |

```lisp
; Split and rejoin
(split "2026-04-16" "-")    ; ("2026" "04" "16")
(str-join ["2026" "04" "16"] "-") ; "2026-04-16"
(str-find "2026-04-16" "04")      ; 5

; Pattern matching on a vector of strings
(like ["apple" "banana" "avocado"] "a*")
; [true false true]

; Lazy vector transforms can be chained
(replace (upper ["a-b" "c-d"]) "-" "_") ; ["A_B" "C_D"]

; String concat also works on strings
(concat "hello" " world")  ; "hello world"
```

## Date & Time { #temporal }

Temporal constructors. Pass `0` to get the current clock value. Cross-temporal comparisons are supported (dates, times, timestamps are all converted to nanoseconds internally).

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `date` | unary | — | Current date, or extract date from timestamp | `(date 0)` → today's date |
| `time` | unary | — | Current wall-clock time (millisecond resolution), or extract time from timestamp | `(time 0)` → current time |
| `timestamp` | unary | — | Current wall-clock timestamp at the system clock's full resolution (nanoseconds on Linux) | `(timestamp 0)` |
| `trunc` | binary | — | Floor a DATE, TIME or TIMESTAMP (atom or vector) to the start of the calendar or clock unit that holds it: `'year` `'quarter` `'month` `'week` `'day` `'hour` `'minute` `'second`. The result has the type of the second argument | `(trunc 'month 2024.05.17)` → `2024.05.01` |

```lisp
; Get current date/time
(date 0)                    ; 2026.04.16
(time 0)                    ; 14:30:00.000000000
(timestamp 0)               ; 2026.04.16T14:30:00.000000000
```

## Type & Introspection { #type-ops }

Type checking, casting, null testing, and object inspection.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `type` | unary | — | Get the type name of a value; a function value answers `UNARY`, `BINARY`, `VARY` or `LAMBDA` | `(type 42)` → `i64` |
| `as` | binary | — | Cast value to another type; `'LIST` boxes a typed vector into a generic `LIST` of atoms; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` | `(as 'i64 "42")` → `42` |
| `nil?` | unary | DAG in queries | Test for null; element-wise over vectors and lists (a `B8` per element), the same in and out of queries | `(nil? [1 0N 3])` → `[false true false]` |
| `rc` | unary | — | Get reference count of an object | `(rc x)` → `1` |
| `guid` | unary | — | Generate a vector of N v4 GUIDs, seeded from the OS (`(guid 0)` → `[]`) | `(guid 1)` |

An `as` to I16, I32, DATE or TIME whose result is the target's minimum gives that type's null, so `(as 'I16 [-32768 32768 5])` is `[0Nh 0Nh 5]`: the minimum is the null sentinel, and an integer that wraps onto it when narrowed is null too. A float source clamps to the minimum plus one instead. See [Null Semantics in Casting](../guides/types.md#null-semantics).

```lisp
; Type checking and casting
(type [1 2 3])              ; I64
(type "hello")              ; str
(type +)                   ; BINARY (function values: UNARY, BINARY, VARY, LAMBDA)
(as 'f64 [1 2 3])          ; [1.0 2.0 3.0]
(type (as 'LIST [1 2]))    ; LIST (two i64 atoms; a LIST is returned unchanged)
(nil? 0Ni)                   ; true

; Generate a batch of GUIDs
(guid 5)                    ; vector of 5 GUIDs
```

## I/O & Output { #io }

Printing, file I/O, CSV loading, and script execution.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `println` | variadic | — | Print values with newline; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(println "hello" 42)` |
| `print` | unary | — | Print value without newline; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(print "hello")` |
| `show` | variadic | — | Pretty-print a value (tables formatted); a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(show trades)` |
| `format` | variadic | — | Format value to string (% as placeholder); a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`; in a query element-wise: one string per row of a column, on every route | `(format "val=%" 42)` → `"val=42"` |
| `.csv.read` | variadic | restricted | Load CSV file into table (mmap, parallel parse) | `(.csv.read "data.csv")` |
| `.csv.write` | variadic | restricted | Write table to CSV file | `(.csv.write trades "out.csv")` |
| `read` | unary | restricted | Read file contents as string, to EOF | `(read "/proc/self/cmdline")` |
| `read-bytes` | unary | restricted | Read file contents as a `U8` byte vector, to EOF | `(read-bytes "file.bin")` |
| `write` | binary | restricted | Write a string to a file | `(write "file.txt" "content")` |
| `write-bytes` | binary | restricted | Write a `U8` byte vector to a file | `(write-bytes "file.bin" bytes)` |
| `load` | unary | restricted | Load and evaluate a Rayfall script file. A relative path is tried against the working directory, then below `RAYFORCE_HOME` when set; absolute paths are used as given; a failure names every path tried | `(load "lib.rfl")` |
| `exit` | unary | restricted | Exit the process with status code | `(exit 0)` |
| `resolve` | variadic | special | Resolve a symbol in the current scope | `(resolve 'x)` |
| `timeit` | variadic | special | Benchmark an expression (prints elapsed time) | `(timeit (sum (til 1000000)))` |

```lisp
; Load and query CSV data
(write "/tmp/rayforce-ref-trades.csv"
  "sym,price,size\nAAPL,150.0,100\nGOOG,2800.0,50\n")
(set trades (.csv.read "/tmp/rayforce-ref-trades.csv"))
(show trades)
(println (format "Loaded % rows" (count (at trades 'sym))))

; Benchmark
(timeit (sum (til 10000000)))
```

## System & Utility { #system }

System interaction, metaprogramming, diagnostics, and runtime inspection.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `eval` | unary | — | Evaluate a parsed Rayfall expression | `(eval (parse "(+ 1 2)"))` → `3` |
| `parse` | unary | — | Parse a string into a Rayfall expression tree | `(parse "(+ 1 2)")` |
| `.sys.gc` | variadic (0–1) | — | Trigger GC / heap flush, returns `0`; mode `1` also releases free pages now, best effort | `(.sys.gc)`, `(.sys.gc 1)` |
| `.mem.objsize` | unary | — | Logical bytes retained by an object graph; shared children count once | `(.mem.objsize value)` |
| `.mem.ts` | special form | — | Evaluate once and return result, time, allocation, peak-memory, result-size, and worker statistics as a dict | `(.mem.ts (select {from: trades}))` |
| `.sys.exec` | variadic | restricted | Execute a shell command; exit code, or `{code, out}` with `'out` | `(.sys.exec "ls -la" 'out)` |
| `.sys.trust` | unary | restricted | Mark lambdas a restricted (`-U`) caller may run with the restriction lifted, isolated from the caller; returns its argument | `(.sys.trust 'peer_count)` |
| `.os.getenv` | unary | restricted | Get environment variable value | `(.os.getenv "HOME")` |
| `.os.setenv` | binary | restricted | Set environment variable | `(.os.setenv "KEY" "value")` |
| `.sys.args` | nullary | — | Application arguments as a typed dict; `user` subdict holds post-`--` args | `(.sys.args)` |
| `env` | unary | — | List all global environment bindings | `(env 0)` |
| `.time.now` | variadic | — | Monotonic time in milliseconds | `(.time.now)` |
| `.time.timer.set` | variadic | restricted | Schedule callback every `ms`, `num` times (0 = forever); returns id | `(.time.timer.set 1000 0 (fn [t] (println t)))` |
| `.time.timer.del` | unary | restricted | Cancel a scheduled timer by id; returns null | `(.time.timer.del 0)` |
| `.time.sleep` | unary | restricted | Block the calling thread for `ms` milliseconds (an integer) or a timespan; returns null; a cancel, or a sync request's client hanging up, ends it; a replay skips it | `(.time.sleep 500)` |
| `.sys.build` | nullary | — | Build metadata dict with `version` + `build-date` | `(.sys.build)` |
| `.sys.mem` | nullary | — | Memory allocator statistics (alloc / peak / slab hits) | `(.sys.mem)` |
| `.sys.prof` | nullary | — | Last profiled query's per-step statistics as a table (opt-in via `:t`) | `(.sys.prof)` |
| `.sys.profile` | nullary | — | Per-call-site profile of Rayfall code as a table (opt-in via `.sys.profile.enable`) | `(.sys.profile)` |
| `.sys.profile.enable` | variadic | restricted | Toggle the call profiler; returns new state | `(.sys.profile.enable 1)` |
| `.sys.profile.reset` | nullary | restricted | Drop the call profiler's rows | `(.sys.profile.reset)` |
| `.sys.querylog` | variadic | — | Ambient per-query statistics ring as a table (opt-in via `-Q` / `.sys.querylog.enable`); a `seq` column numbers every logged query from 1, and `(.sys.querylog n)` answers only the rows with `seq >= n` | `(.sys.querylog)`, `(.sys.querylog 4200)` |
| `.sys.querylog.stats` | variadic | — | The query-statistics ring's counters: `next` seq, rows `dropped` by the wrap, `capacity` | `(.sys.querylog.stats)` |
| `.sys.querylog.enable` | variadic | restricted | Toggle query-statistics logging; returns new state | `(.sys.querylog.enable 1)` |
| `.sys.join.par.min` | variadic | restricted | Read or set the probe-side rows from which a hash join's probe goes parallel; returns the value in effect | `(.sys.join.par.min 65536)` |
| `.sys.info` | nullary | — | Host and process info (cores, page size, total memory, pid, hostname) | `(.sys.info)` |
| `.idx.uses` | unary | — | Per-column count of index hits since the previous call; the first call arms the counter. See [`.idx.uses`](../namespaces/idx.md#idx-uses) | `(.idx.uses t)` → `{a:1}` |
| `.repl.complete` | unary | — | Sorted names (builtins, keywords, session bindings) that start with a prefix. See [`.repl.complete`](../namespaces/repl.md#repl-complete) | `(.repl.complete "xas")` → `[xasc]` |
| `.repl.signature` | unary | — | How a named global is called: kind, calling convention, special form, lambda parameters, documented usage; calls nothing. See [`.repl.signature`](../namespaces/repl.md#repl-signature) | `(at (.repl.signature 'min) 'kind)` → `'unary` |
| `.repl.definition` | unary | restricted | Where the function a named global holds was written: file, `(fn …)` span, text, and a digest of the loaded source. See [`.repl.definition`](../namespaces/repl.md#repl-definition) | `(at (.repl.definition 'add) 'start)` → `[0 9]` |
| `.repl.inspect` | variadic | — | Kind, type and length of a named global or member path, with its dict member or table column names and types, read from headers only. See [`.repl.inspect`](../namespaces/repl.md#repl-inspect) | `(at (.repl.inspect 't) 'keys)` → `[a b]` |

```lisp
; Metaprogramming: parse and eval
(set expr (parse "(+ 10 20)"))
(eval expr)                  ; 30

; Timing with monotonic millisecond clock
(set t0 (.time.now))
(sum (til 1000000))
(println (format "%ms" (- (.time.now) t0)))

; System diagnostics
(.sys.info)                    ; OS, CPU cores, memory budget
(.sys.mem)                    ; heap usage statistics
```

## Serialization

Binary serialization for any Rayforce object. Useful for IPC, caching, and persistence.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `ser` | unary | — | Serialize any value to binary format (byte vector); a value nested deeper than 512 levels fails with `limit`, the depth `de` reads; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` | `(ser [1 2 3])` |
| `de` | unary | — | Deserialize from binary format back to value; a list holding an error value (built in C, never in Rayfall, where a caught error is a dict) reads back as its first error, see [A list holding an error value](../storage/ipc.md#a-list-holding-an-error-value) | `(de bytes)` → `[1 2 3]` |
| `repr` | variadic | — | `(repr v)` writes `v` as Rayfall text that reads back to the same value; `(repr v n)` fails with `limit` rather than return more than `n` bytes; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`. See [`repr`](../language/string.md#repr); in a query element-wise: one string per row of a column, on every route | `(repr [1i 2i])` → `"[1i 2i]"` |
| `read-value` | variadic | — | Reads one value from data text without evaluating anything; an optional dict overrides the limits. See [Reading Data Text](../language/read-value.md) | `(read-value "{a: 1 b: [2 3]}")` → `{a:1 b:[2 3]}` |
| `read-limits` | variadic | — | The default limits of `read-value`, as a dict | `(read-limits)` |
| `.json.parse` | variadic | — | JSON text to a value (object to dict with string keys, array to a vector or list); element-wise over a string vector; strict, the limits of `read-value`. See [`.json.*`](../namespaces/json.md) | `(.json.parse "{\"a\":[1,2]}")` → `(dict ["a"] (list [1 2]))` |
| `.json.get` | variadic | — | One field of each JSON document by a dotted path (or one path per row), without building the rest; an optional type (`'STR`, `'I64`, `'F64`) makes it always that type, null where the field is absent or another kind; optional limits | `(.json.get line "kind" 'STR)` |
| `.json.format` | unary | — | A value as JSON text (a table as an array of objects); in a query element-wise: one string per row of a column, on every route | `(.json.format [1 0Nl 3])` → `"[1,null,3]"` |

```lisp
; Round-trip serialization
(set data [1 2 3])
(set bytes (ser data))
(de bytes)                   ; [1 2 3]

; Serialize a table for caching
(set cached (de (ser trades)))
```

## Storage

Persistent columnar storage — splayed (one file per column) and partitioned tables.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `.db.splayed.set` | variadic | restricted | Save table as splayed columns to a directory; optional `{compress: bool, symfile: 'shared/'column, index: bool, durable: bool}`; `{durable: false}` skips the fsyncs; a parted table is written a segment at a time | `(.db.splayed.set "db/trades" trades)` |
| `.db.splayed.append` | variadic | restricted | Append a table's rows to a splayed table on disk without loading it; the table has the target's column names and types; optional symbol-file path and `{index: bool, durable: bool}`; no table there: written as by `.db.splayed.set` | `(.db.splayed.append "db/trades" new_trades)` |
| `.db.splayed.get` | variadic | — | Load splayed table from a directory | `(.db.splayed.get "db/trades")` |
| `.db.parted.get` | variadic | — | Load partitioned table from root directory | `(.db.parted.get "db" 'trades)` |
| `.db.parted.tables` | variadic | — | List table names under a parted root (from the most recent partition) | `(.db.parted.tables "db")` |
| `.db.parted.fill` | variadic | restricted | Backfill missing tables across a parted db's partitions | `(.db.parted.fill "db")` |
| `.parquet.meta` | unary | restricted | Column-chunk metadata table of a Parquet file; see [`.parquet.*`](../namespaces/parquet.md) | `(.parquet.meta "t.parquet")` |
| `.parquet.read` | variadic | restricted | Read a Parquet file into a table; optional projection or options before the path | `(.parquet.read "t.parquet")` |
| `.parquet.scan` | variadic | restricted | Reusable lazy `select` source over a Parquet file; optional options before the path | `(select {from: (.parquet.scan "t.parquet") where: (> x 0)})` |
| `.parquet.each` | variadic | restricted | Deliver a Parquet file to a callback in chunks; returns the number of rows delivered | `(.parquet.each "t.parquet" (fn [chunk] ...))` |
| `.parquet.splayed` | variadic | restricted | Import a Parquet file as a splayed table; returns the row count; optional `{types: [...] strict: bool compress: bool symfile: 'shared/'column reproducible: bool}` | `(.parquet.splayed "t.parquet" "db/t")` |
| `.parquet.parted` | variadic | restricted | Import Parquet files into a partitioned table under a root; returns the row count; optional `{types: [...] rows: n strict: bool compress: bool reproducible: bool}` | `(.parquet.parted "t.parquet" "db" 'trades)` |

```lisp
; Save and reload a splayed table
(.db.splayed.set "/tmp/rayforce-ref-trades" trades)
(set t (.db.splayed.get "/tmp/rayforce-ref-trades"))
(show t)
```

```text
; Load a date-partitioned table
(set hist (.db.parted.get "db" 'trades))
```

## IPC (Inter-Process Communication) { #ipc }

IPC for connecting to remote Rayforce instances. Uses binary serialization on the wire; a connection opens over TCP and moves to shared memory (one host) or RDMA (two hosts with devices) when both sides can — see [Transports](../storage/ipc.md#transports).

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `.ipc.open` | variadic | restricted | Open a connection to host:port (optional connect timeout in ms, or an options dict with `timeout`, `compress`, `keepalive`, `transport`), returns handle | `(.ipc.open "localhost:5000" 2000)` |
| `.ipc.close` | unary | restricted | Close an IPC connection handle | `(.ipc.close h)` |
| `.ipc.send` | variadic | restricted | Send a value over an IPC handle (sync request); optional third argument is a deadline in ms | `(.ipc.send h "(sum (til 100))")`, `(.ipc.send h "(sum (til 100))" 2000)` |
| `.ipc.handle` | variadic | — | Current connection handle inside any `.ipc.on.*` hook, `-1` outside; given a live connection handle, that connection's view (`inbound`, `transport`, `limit_bytes`, `limit_frames`, `queued_bytes`, `queued_frames`, `hwm_bytes`) | `(.ipc.handle)`, `(.ipc.handle h)` |
| `.ipc.handles` | variadic | — | Every open IPC connection of this process as a table, one row per connection in handle order: the columns of `(.ipc.handle h)` and `opened`, when it was accepted or opened. Allowed under `-U` | `(.ipc.handles)` |
| `.ipc.auto_journal_eval` | variadic | — | `true` while evaluating inside an auto-journaled IPC request (the request itself is the journal entry and `.log.write` refuses), `false` otherwise | `(if (.ipc.auto_journal_eval) null (.log.write x))` |
| `.ipc.txlimit` | variadic | restricted | Transmit backlog policy: `()` reads the process default, `(bytes [frames])` sets it, `(h bytes frames)` overrides one connection; a frame that would exceed a connection's limit is refused and a multicast subscriber is dropped and closed | `(.ipc.txlimit 8388608 64)` |
| `.ipc.transport` | variadic | restricted | Transport policy: `()` reads it, `('auto|'tcp|'shm|'rdma)` sets what this process offers as a server and asks for as a client (`auto`: shared memory on one host, RDMA across hosts, else TCP) | `(.ipc.transport 'tcp)` |
| `.mc.sub` | variadic | — | Subscribe the calling connection to a multicast topic (symbol or string); the filter argument must be null. Returns the sequence number the next publication will carry. See [Multicast topics](../storage/ipc.md#multicast) | `(.ipc.send h (list '.mc.sub "depth" null))` |
| `.mc.unsub` | unary | — | Remove the calling connection's subscription to a topic; null | `(.mc.unsub "depth")` |
| `.mc.pub` | binary | restricted | Publish a payload to every subscriber of a topic as an async `(upd topic seq payload)`; returns the sequence number, or `0` when the topic has no subscribers | `(.mc.pub "depth" batch)` |
| `.mc.stats` | variadic | — | Multicast counters as a dict: `topics`, `subscriptions`, `published`, `delivered`, `dropped`, `framed`, `tx_limit_bytes`, `tx_limit_frames`, `tx_hwm_bytes` | `(.mc.stats)` |
| `.ipc.on.open` | hook | user-settable | Fires after inbound connection completes handshake; arg = handle | `(set .ipc.on.open (fn [h] ...))` |
| `.ipc.on.close` | hook | user-settable | Fires once before an established connection is torn down, inbound or outbound, on any cause; arg = handle | `(set .ipc.on.close (fn [h] ...))` |
| `.ipc.on.sync` | hook | user-settable | Intercepts sync messages; return value becomes the response | `(set .ipc.on.sync (fn [m] (eval (parse m))))` |
| `.ipc.on.async` | hook | user-settable | Intercepts async messages; return value ignored | `(set .ipc.on.async (fn [m] ...))` |
| `.ipc.on.auth` | hook | user-settable | Narrows `-u`/`-U` auth; truthy = accept, falsy = reject | `(set .ipc.on.auth (fn [u p] (!= u "ban")))` |

```text
; Connect to a remote Rayforce instance
(set h (.ipc.open "localhost:5000"))
(set result (.ipc.send h "(select {from: trades where: (> price 100)})"))
(show result)
(.ipc.close h)
```

## EAV Triple Store { #eav }

Built-in Entity-Attribute-Value store. The EAV table has three columns: `e` (entity, i64), `a` (attribute, sym), `v` (value, i64). Foundation for the Datalog engine.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `datoms` | variadic | — | Create an empty EAV table | `(datoms)` |
| `assert-fact` | variadic | — | Append a triple (e, a, v) to the EAV table | `(assert-fact db 1 'name 100)` |
| `retract-fact` | variadic | — | Remove a triple from the EAV table | `(retract-fact db 1 'name 100)` |
| `scan-eav` | variadic | — | Query EAV by attribute, returns (e, v) table | `(scan-eav db 'name)` |
| `pull` | variadic | — | Entity-centric retrieval — returns all attributes as dict | `(pull db 1)` → `{name:100 age:30}` |

```lisp
; Build a knowledge base with EAV triples
(set db (datoms))
(set db (assert-fact db 1 'name 100))
(set db (assert-fact db 1 'age 30))
(set db (assert-fact db 2 'name 200))

(pull db 1)                  ; {name:100 age:30}
(scan-eav db 'name)         ; table of (e, v) where a=name
```

## Journal { #journal }

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `.log.replay` | unary | restricted | Replay and return processed entries, **including** evaluation errors; warns and continues, no rollback | `(.log.replay "journal.log")` |
| `.log.replay_report` | unary | restricted | Replay **once** and return I64 `entries` / `eval_errors` on a clean tail; terminal errors still raise and may leave an applied prefix. Not a previous-call status getter, fail-fast mode, or invariant/rollback guarantee | `(.log.replay_report "journal.log")` |
| `.log.status` | variadic | — | Journal state as a dict of seven fields: `open` (handle present), `opened_ok` (a journal was opened successfully at least once), `path`, `mode` (`sync`/`async`; both null if never opened successfully), `bytes` (nullable: null if the size probe failed), `auto` (inside an auto-journaled request), `posts` (async requests are journaled too); raises only on allocation/intern failure | `(if (get (.log.status) 'open) null (exit 1))` |
| `.log.posts` | variadic | restricted | Read (no argument) or set (`0`/`1`) whether an open journal also records async requests; off by default, `--log-posts N` at startup; returns the state as `i64` | `(.log.posts 1)` |

## Datalog

Datalog rules and queries integrate with the EAV store. Rules use the `(?entity :attribute ?value)` pattern to match triples. Supports recursive rules and stratified negation.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `rule` | variadic | special | Define a Datalog rule (head + body clauses) | `(rule (path ?x ?y) (?x :edge ?y))` |
| `query` | variadic | special | Compile and execute a Datalog query against EAV store | `(query db (find ?x ?y) (where (path ?x ?y)))` |

```lisp
; Recursive transitive closure
(rule (path ?x ?y) (?x :edge ?y))
(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))

(set db (datoms))
(set db (assert-fact db 1 'edge 2))
(set db (assert-fact db 2 'edge 3))
(query db (find ?x ?y) (where (path ?x ?y)))
; (1,2) (1,3) (2,3)

; Stratified negation: nodes with no outgoing edges
(rule (leaf ?x) (?x :edge ?_) (not (?x :edge ?_)))
(query db (find ?x) (where (leaf ?x)))
```

## Datalog Program API { #datalog-program }

Low-level API for building and evaluating Datalog programs directly, bypassing the EAV store. Useful for custom base relations from tables.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `dl-program` | variadic | — | Create an empty Datalog program | `(dl-program)` |
| `dl-add-edb` | variadic | — | Register a base relation (table + arity) | `(dl-add-edb prog 'edge tbl 2)` |
| `dl-stratify` | unary | — | Compute strata for the program (required before eval) | `(dl-stratify prog)` |
| `dl-eval` | unary | — | Evaluate program to fixpoint (semi-naive iteration) | `(dl-eval prog)` |
| `dl-query` | binary | — | Query a derived or base relation by name | `(dl-query prog 'path)` |
| `dl-provenance` | binary | — | Reserved provenance hook; currently returns `domain: not available` | `(dl-provenance prog 'path)` |

```lisp
; Build a Datalog program from a table
(set edges (table [x y] (list [1 2 3] [2 3 4])))
(set prog (dl-program))
(dl-add-edb prog 'edge edges 2)

; Evaluate registered EDB tables
(dl-stratify prog)
(dl-eval prog)

; Query results
(dl-query prog 'edge)
```

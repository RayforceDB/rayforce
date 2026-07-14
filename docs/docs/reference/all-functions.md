# Complete Function Reference

!!! note "Legend"
    **atomic** — auto-maps element-wise over vectors.
    **aggr** — reduces vectors to scalars (used in group-by).
    **special** — receives unevaluated arguments (special form).
    **restricted** — disabled in sandboxed mode.

## Categories

| | | |
|---|---|---|
| [Arithmetic](#arithmetic) (24) | [Comparison](#comparison) (7) | [Logic](#logic) (3) |
| [Aggregation](#aggregation) (25) | [Higher-Order](#higher-order) (13) | [Collection](#collection) (41) |
| [Sorting & Ordering](#sorting) (10) | [Control Flow & Special Forms](#control) (11) | [Table Operations](#table-ops) (20) |
| [Query](#query) (4) | [Joins](#joins) (7) | [Pivot](#pivot) (1) |
| [String](#string-ops) (11) | [Temporal](#temporal) (3) | [Type & Introspection](#type-ops) (5) |
| [I/O & Output](#io) (12) | [System & Utility](#system) (15) | [Serialization](#serialization) (2) |
| [Storage](#storage) (5) | [IPC](#ipc) (9) | [EAV Triple Store](#eav) (5) |
| [Datalog](#datalog) (2) | [Datalog Program API](#datalog-program) (6) | |

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

Generated from `src/lang/eval.c` in this checkout. The categorized reference below expands the most commonly used functions; this list is the complete registered name surface.

### Unary

`not`, `neg`, `round`, `floor`, `ceil`, `abs`, `sqrt`, `log`, `exp`, `sin`, `asin`, `cos`, `acos`, `tan`, `atan`, `reciprocal`,
`signum`, `sum`, `prod`, `all`, `any`, `count`, `avg`, `min`, `max`,
`first`, `last`, `med`, `mode`, `dev`, `stddev`, `stddev_pop`, `dev_pop`, `var`, `var_pop`, `raise`, `distinct`,
`reverse`, `til`, `lag`, `lead`, `deltas`, `ratios`, `fills`, `sums`, `avgs`, `mins`, `maxs`, `prds`,
`differ`, `asc`, `desc`, `iasc`, `idesc`, `rank`, `key`, `cols`, `value`, `type`, `read`, `load`, `exit`,
`nil?`, `where`, `group`, `raze`, `ungroup`, `ser`, `de`, `guid`, `date`, `time`, `timestamp`, `ss`, `hh`,
`minute`, `yyyy`, `mm`, `dd`, `dow`, `doy`, `eval`, `parse`, `meta`, `fkeys`, `.sys.exec`, `.sys.cmd`, `.sys.listen`,
`.os.getenv`, `.fs.size`, `.fs.list`, `.ipc.close`, `.repl.connect`, `.log.write`, `.log.replay`,
`.log.validate`, `rc`, `diverse`, `.time.timer.del`, `env`, `sym-name`, `dl-stratify`, `dl-eval`, `dl-free`,
`norm`, `hnsw-free`, `hnsw-load`, `hnsw-info`, `.idx.zone`, `.idx.hash`, `.idx.sort`, `.idx.bloom`,
`.idx.drop`, `.idx.has?`, `.idx.info`, `.attr.get`, `.attr.drop`, `.col.unlink`, `.col.link?`, `.col.target`,
`.graph.free`, `.graph.info`, `strlen`, `upper`, `lower`, `trim`

### Binary

`+`, `-`, `*`, `/`, `%`, `>`, `<`, `>=`, `<=`, `==`, `!=`, `pow`, `top`, `bot`, `pearson_corr`, `cov`, `scov`, `wsum`, `wavg`, `quantile`, `percentile`, `set`, `let`,
`try`, `filter`, `in`, `except`, `union`, `sect`, `take`, `drop`, `rotate`, `cut`, `cross`, `at`, `find`, `msum`, `mavg`, `mmin`, `mmax`,
`mcount`, `mvar`, `mdev`, `xasc`, `xdesc`, `table`, `union-all`, `xbar`, `as`, `write`, `dict`, `concat`, `within`, `div`,
`rand`, `bin`, `binr`, `split`, `str-find`, `str-join`, `like`, `.os.setenv`, `.ipc.send`, `.ipc.post`, `get`, `remove`, `row`,
`unify`, `xcol`, `xcols`, `xkey`, `xgroup`, `xrank`, `dl-query`, `dl-provenance`, `cos-dist`, `inner-prod`, `l2-dist`, `hnsw-save`, `.attr.set`,
`.col.link`

### Variadic / Special

`and`, `or`, `if`, `do`, `fn`, `map`, `pmap`, `fold`, `scan`, `prior`, `apply`, `list`, `select`, `window`,
`update`, `insert`, `upsert`, `left-join`, `inner-join`, `full-join`, `anti-join`, `window-join`, `window-join1`,
`asof-join`, `println`, `show`, `format`, `read-csv`, `write-csv`, `.csv.read`, `.csv.splayed`, `.csv.parted`,
`.csv.write`, `resolve`, `timeit`, `enlist`, `map-left`, `map-right`, `.db.splayed.set`, `.db.splayed.get`,
`.db.parted.get`, `.db.parted.tables`, `.db.parted.fill`, `alter`, `print`, `.sys.gc`, `.mem.objsize`, `.mem.ts`, `.sys.timeit`,
`.sys.env`, `.sys.args`, `.ipc.open`, `.ipc.handle`, `.repl.disconnect`, `.log.open`, `.log.roll`,
`.log.snapshot`, `.log.sync`, `.log.close`, `.log.purge`, `quote`, `return`, `.time.now`, `.time.timer.set`,
`fold-left`, `fold-right`, `scan-left`, `scan-right`, `del`, `.sys.build`, `.sys.mem`, `.sys.prof`,
`.sys.querylog`, `.sys.querylog.enable`, `modify`, `pivot`, `.sys.info`, `datoms`, `assert-fact`,
`retract-fact`, `scan-eav`, `pull`, `rule`, `query`, `dl-program`, `dl-add-edb`, `knn`, `hnsw-build`, `ann`,
`.graph.build`, `.graph.pagerank`, `.graph.connected`, `.graph.dijkstra`, `.graph.louvain`, `.graph.degree`,
`.graph.topsort`, `.graph.dfs`, `.graph.cluster`, `.graph.betweenness`, `.graph.closeness`, `.graph.mst`,
`.graph.random-walk`, `.graph.k-shortest`, `.graph.shortest-path`, `.graph.expand`, `.graph.var-expand`,
`substr`, `replace`

## Arithmetic

Arithmetic operators auto-map over vectors and broadcast scalars. Numeric typed-vector calls and query expressions lower to morsel-based DAG kernels where available.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `+` | binary | atomic | Addition | `(+ 3 4)` → `7` |
| `-` | binary | atomic | Subtraction | `(- 10 3)` → `7` |
| `*` | binary | atomic | Multiplication | `(* 3 4)` → `12` |
| `/` | binary | atomic | Float division (always returns f64) | `(/ 7 2)` → `3.5` |
| `%` | binary | atomic | Modulo (remainder) | `(% 7 3)` → `1` |
| `div` | binary | atomic | Integer division (floor) | `(div 7 2)` → `3` |
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

```lisp
; Vector comparisons return boolean vectors
(> [10 20 30] 15)          ; [false true true]
(== [1 2 3] [1 0 3])       ; [true false true]

; within checks the inclusive range [lo, hi]
(within [1 3 5 7] [3 6])  ; [false true true false]
```

## Logic

Boolean logic operators. Work on scalars and vectors alike.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `and` | binary | — | Logical AND | `(and true false)` → `false` |
| `or` | binary | — | Logical OR | `(or true false)` → `true` |
| `not` | unary | — | Logical NOT | `(not true)` → `false` |

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
| `prod` | unary | aggr | Product of all non-null numeric elements | `(prod [2 3 4])` → `24` |
| `all` | unary | aggr | True if every non-null numeric element is truthy; empty/all-null returns `true` | `(all [1 2 3])` → `true` |
| `any` | unary | aggr | True if any non-null numeric element is truthy; empty/all-null returns `false` | `(any [0 0 3])` → `true` |
| `count` | unary | aggr | Count of elements | `(count [1 2 3])` → `3` |
| `avg` | unary | aggr | Arithmetic mean | `(avg [1 2 3])` → `2.0` |
| `min` | unary | aggr | Minimum value | `(min [3 1 2])` → `1` |
| `max` | unary | aggr | Maximum value | `(max [3 1 2])` → `3` |
| `first` | unary | — | First element of a vector | `(first [10 20 30])` → `10` |
| `last` | unary | — | Last element of a vector | `(last [10 20 30])` → `30` |
| `med` | unary | aggr | Median value (returns f64) | `(med [1 3 2])` → `2.0` |
| `mode` | unary | aggr | Most frequent non-null value; ties keep the first encountered value | `(mode [1 2 2 3])` → `2` |
| `dev` | unary | aggr | Population standard deviation | `(dev [2 4 4 4 5 5 7 9])` → `2.0` |
| `stddev` | unary | aggr | Sample standard deviation (Bessel-corrected; not an alias of `dev`) | `(stddev [1 2 3])` → `1.0` |
| `stddev_pop` | unary | aggr | Population standard deviation | `(stddev_pop [1 2 3])` |
| `dev_pop` | unary | aggr | Population standard deviation (alias) | `(dev_pop [1 2 3])` |
| `var` | unary | aggr | Sample variance | `(var [1 2 3])` → `1.0` |
| `var_pop` | unary | aggr | Population variance | `(var_pop [1 2 3])` |
| `pearson_corr` | binary | aggr | Pearson correlation over paired non-null numeric inputs | `(pearson_corr [1 2 3] [2 4 6])` → `1.0` |
| `cov` | binary | aggr | Population covariance over paired non-null numeric inputs | `(cov [1 3] [2 6])` → `2.0` |
| `scov` | binary | aggr | Sample covariance over paired non-null numeric inputs | `(scov [1 3] [2 6])` → `4.0` |
| `wsum` | binary | aggr | Weighted sum, `sum(weights * values)`, skipping null pairs | `(wsum [1 3] [10 20])` → `70.0` |
| `wavg` | binary | aggr | Weighted average, `wsum / sum(weights)`; null for zero total weight | `(wavg [1 3] [10 20])` → `17.5` |
| `quantile` | binary | aggr | Exact linear-interpolated quantile; probability is `0.0..1.0` | `(quantile [1 2 3 4] 0.5)` → `2.5` |
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

## Higher-Order Functions { #higher-order }

Functions that take other functions as arguments for mapping, folding, and filtering.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `map` | variadic | — | Apply function to each element (returns a list) | `(map (fn [x] (* x 2)) [1 2 3])` → `(2 4 6)` |
| `pmap` | variadic | — | Parallel map (multi-threaded, returns a list) | `(pmap (fn [x] (* x x)) [1 2 3])` → `(1 4 9)` |
| `fold` | variadic | — | Reduce with function and initial value | `(fold + 0 [1 2 3])` → `6` |
| `fold-left` | variadic | — | Left-associative fold | `(fold-left - 10 [1 2 3])` → `4` |
| `fold-right` | variadic | — | Right-associative fold | `(fold-right - 10 [1 2 3])` → `-8` |
| `scan` | variadic | — | Running fold (all intermediate results) | `(scan + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-left` | variadic | — | Left-to-right running fold | `(scan-left + (enlist 1 2 3))` → `[1 3 6]` |
| `scan-right` | variadic | — | Right-to-left running fold (returns a list) | `(scan-right + (enlist 1 2 3))` → `(6 5 3)` |
| `filter` | binary | — | Keep elements where boolean mask is true | `(filter [1 2 3 4] (> [1 2 3 4] 2))` → `[3 4]` |
| `apply` | variadic | — | Zip-apply function pairwise over lists | `(apply + (enlist 1 2) (enlist 3 4))` → `(4 6)` |
| `map-left` | variadic | — | Map each element of the left over the whole right | `(map-left + 10 [1 2 3])` → `[11 12 13]` |
| `map-right` | variadic | — | Map the whole left over each element of the right | `(map-right - [10 20 30] 5)` → `[5 15 25]` |
| `prior` | variadic | — | Apply a binary fn to each element and its predecessor (first paired with itself); returns a list | `(prior - [1 3 6 10])` → `(0 2 3 4)` |

```lisp
; Transform each row with map
(map (fn [x] (* x x)) [1 2 3 4])  ; (1 4 9 16)

; Parallel map for expensive computation
(pmap (fn [t] (sum (til t))) [100 200 300])

; Running sum with scan
(scan + (enlist 1 2 3 4))  ; [1 3 6 10]
```

## Collection Operations { #collection }

Operations on vectors and lists as collections — set operations, indexing, searching, and construction.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `distinct` | unary | — | Remove duplicates, preserving order | `(distinct [1 2 2 3])` → `[1 2 3]` |
| `in` | binary | — | Membership test (is element in vector?) | `(in 2 [1 2 3])` → `true` |
| `except` | binary | — | Set difference (elements in A not in B) | `(except [1 2 3] [2])` → `[1 3]` |
| `union` | binary | — | Set union (deduplicated) | `(union [1 2] [2 3])` → `[1 2 3]` |
| `sect` | binary | — | Set intersection | `(sect [1 2 3] [2 3 4])` → `[2 3]` |
| `take` | binary | — | Take first N elements (negative N takes from end) | `(take [10 20 30] 2)` → `[10 20]` |
| `drop` | binary | — | Drop first N elements (negative N drops from end) | `(drop [10 20 30] 1)` → `[20 30]` |
| `rotate` | binary | — | Rotate left by N positions (negative rotates right) | `(rotate [1 2 3 4] 1)` → `[2 3 4 1]` |
| `cut` | binary | — | Split a collection at sorted 0-based indices | `(cut [1 2 3 4] [2])` → `([1 2] [3 4])` |
| `cross` | binary | — | Cartesian product as pairs | `(cross [1 2] ['a 'b])` → `((1 'a) (1 'b) (2 'a) (2 'b))` |
| `at` | binary | — | Index into vector (0-based) | `(at [10 20 30] 1)` → `20` |
| `find` | binary | — | Find index of first occurrence | `(find [10 20 30] 20)` → `1` |
| `reverse` | unary | — | Reverse element order | `(reverse [1 2 3])` → `[3 2 1]` |
| `til` | unary | — | Generate range [0..n) | `(til 5)` → `[0 1 2 3 4]` |
| `lag` | unary | lazy/DAG | Shift values one row back; first row is null/sentinel | `(lag [10 20 30])` → `[0Nl 10 20]` |
| `lead` | unary | lazy/DAG | Shift values one row forward; last row is null/sentinel | `(lead [10 20 30])` → `[20 30 0Nl]` |
| `deltas` | unary | lazy/DAG | Adjacent differences; first row is null | `(deltas [10 15 13])` → `[0Nl 5 -2]` |
| `ratios` | unary | lazy/DAG | Adjacent ratios as f64; first row is null | `(ratios [2 4 8])` → `[0Nf 2.0 2.0]` |
| `fills` | unary | lazy/DAG | Forward-fill nullable vectors, or every column of a table | `(fills (as 'I64 (list 0N 2 0N)))` → `[0Nl 2 2]` |
| `sums` | unary | lazy/DAG | Running sum; nulls are skipped | `(sums [1 2 3])` → `[1 3 6]` |
| `avgs` | unary | lazy/DAG | Running average over non-null values | `(avgs [2 4 6])` → `[2.0 3.0 4.0]` |
| `mins` | unary | lazy/DAG | Running minimum | `(mins [3 1 2])` → `[3 1 1]` |
| `maxs` | unary | lazy/DAG | Running maximum | `(maxs [3 1 2])` → `[3 3 3]` |
| `prds` | unary | lazy/DAG | Running product; nulls are skipped | `(prds [2 3 4])` → `[2 6 24]` |
| `differ` | unary | lazy/DAG | Boolean change flag versus previous row; first row is true | `(differ [1 1 2])` → `[true false true]` |
| `msum` | binary | lazy/DAG | Moving sum over trailing N rows; nulls are skipped | `(msum 3 [1 2 3 4])` → `[1 3 6 9]` |
| `mavg` | binary | lazy/DAG | Moving average over trailing N rows and non-null values | `(mavg 3 [1 2 3 4])` → `[1.0 1.5 2.0 3.0]` |
| `mmin` | binary | lazy/DAG | Moving minimum over trailing N rows | `(mmin 3 [3 2 4 1])` → `[3 2 2 1]` |
| `mmax` | binary | lazy/DAG | Moving maximum over trailing N rows | `(mmax 3 [3 2 4 1])` → `[3 3 4 4]` |
| `mcount` | binary | lazy/DAG | Moving non-null count over trailing N rows | `(mcount 3 [1 2 3 4])` → `[1 2 3 3]` |
| `mvar` | binary | lazy/DAG | Moving population variance over trailing N rows and non-null values | `(mvar 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `mdev` | binary | lazy/DAG | Moving population standard deviation over trailing N rows and non-null values | `(mdev 2 [1 3 5])` → `[0.0 1.0 1.0]` |
| `enlist` | variadic | — | Wrap value(s) in a vector (list of atoms) | `(enlist 1 2 3)` → `[1 2 3]` |
| `concat` | binary | — | Concatenate two vectors or strings | `(concat [1 2] [3 4])` → `[1 2 3 4]` |
| `raze` | unary | — | Flatten a list of vectors into one vector | `(raze (list [1 2] [3 4]))` → `[1 2 3 4]` |
| `where` | unary | — | Indices where boolean vector is true | `(where [true false true])` → `[0 2]` |
| `group` | unary | — | Group indices by value (returns dict) | `(group ['a 'b 'a])` → dict |
| `diverse` | unary | — | Check if all elements are unique (no duplicates) | `(diverse [1 2 3])` → `true` |
| `rand` | binary | — | Generate N random values from range or sample from vector | `(rand 3 100)` → 3 random ints 0..99 |
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
(sect [1 2 3] [2 3 4])    ; [2 3]
(except [1 2 3] [2])      ; [1 3]

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
| `let` | binary | special | Bind value to local variable (lexical scope) | `(let y (+ x 1))` |
| `if` | variadic | special | Conditional: (if cond then else) | `(if (> x 0) "pos" "neg")` |
| `do` | variadic | special | Sequential execution, returns last value | `(do (set x 1) (set y 2) (+ x y))` |
| `fn` | variadic | special | Create lambda function | `(fn [x y] (+ x y))` |
| `try` | binary | special | Error handling: (try expr handler-fn-or-fallback-value) | `(try (/ 1 0) (fn [e] 0))` |
| `raise` | unary | — | Throw an error with message | `(raise "bad input")` |
| `return` | unary | — | Early return from function body | `(return 42)` |
| `quote` | variadic | special | Return argument unevaluated; a bare name yields a literal symbol (`(quote x)` ≡ `'x`) | `(quote (+ 1 2))` → `(+ 1 2)` |
| `alter` | variadic | special | In-place mutation of table column | `(alter trades 'price (* price 1.1))` |
| `del` | variadic | special, restricted | Delete columns or rows from table | `(del trades 'temp_col)` |

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
| `table` | binary | — | Create table from column names and list of vectors | `(table [x y] (list [1 2] ['a 'b]))` |
| `dict` | binary | — | Create dictionary from keys and values vectors | `(dict ['a 'b] [1 2])` |
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
| `union-all` | binary | — | Concatenate two tables (all rows, no dedup) | `(union-all t1 t2)` |
| `unify` | binary | — | Merge two tables/dicts (second takes precedence) | `(unify d1 d2)` |
| `modify` | variadic | restricted | Functional table update (returns new table) | `(modify trades 'price (fn [p] (* p 1.1)))` |
| `ungroup` | unary | — | Flatten a grouped table's nested list columns into one row per element | `(ungroup gt)` |
| `pivot` | variadic | — | Pivot table — reshape long to wide format | `(pivot trades 'sym 'date 'price sum)` |

`xcol` requires exactly one new name for each existing column. `xcols`, `xkey`, and `xgroup` accept a symbol atom, a symbol vector, or a list of symbol atoms. `xkey` is a dictionary projection, not a native keyed-table type: key columns must be unique, and each value is a row dictionary containing the non-key columns. Use `xgroup` when keys may repeat; each dictionary value is a table slice for that group.

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
| `window` | variadic | special | Partitioned window query. `frame:` is `'whole`, `'running`, or a positive trailing row count | `(window {from: t part: [sym] order: [time] frame: 5 funcs: {avg5: (avg price)}})` |
| `update` | variadic | special, restricted | Add or modify columns in a table (mutates in-place) | `(update {from: t b: (* a 2)})` |
| `insert` | variadic | special, restricted | Append rows, or grow a parted table's in-memory live tail with an explicit partition key | `(insert 't 2024.01.16 rows)` |
| `upsert` | variadic | special, restricted | Insert or update flat-table rows by key match (target, key, row) | `(upsert t 'x {x: 10 y: 20})` |

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
| `left-join` | variadic | — | Left join — all left rows, unmatched filled with null. Keys are a symbol list. | `(left-join trades quotes [sym])` |
| `inner-join` | variadic | — | Inner join — only matching rows from both sides | `(inner-join orders products [product_id])` |
| `full-join` | variadic | — | Full outer join — all rows from both sides, unmatched columns filled with null | `(full-join [sym] trades quotes)` |
| `anti-join` | variadic | — | Anti-semi-join — left rows with no right match | `(anti-join t1 t2 [key])` |
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
(left-join trades quotes [sym])

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
| `split` | binary | — | Split string by delimiter into a list of strings | `(split "a,b,c" ",")` → `("a" "b" "c")` |
| `str-find` | binary | parallel vector | First byte index of a substring; `0Nl` when absent | `(str-find "banana" "na")` → `2` |
| `str-join` | binary | — | Join string/symbol atoms, vectors, or lists with a delimiter | `(str-join ["a" "b"] ",")` → `"a,b"` |
| `strlen` | unary | — | Length of each string | `(strlen "hello")` → `5` |
| `upper` | unary | lazy/DAG | Uppercase string or symbol atoms/vectors | `(upper ["ab" "Cd"])` → `["AB" "CD"]` |
| `lower` | unary | lazy/DAG | Lowercase string or symbol atoms/vectors | `(lower 'AbC)` → `'abc` |
| `trim` | unary | lazy/DAG | Strip leading/trailing whitespace | `(trim "  abc  ")` → `"abc"` |
| `substr` | variadic | lazy/DAG | Substring by 1-based start and length; 0 clamps to start | `(substr "abcdef" 2 3)` → `"bcd"` |
| `replace` | variadic | lazy/DAG | Replace all occurrences of a string/symbol atom | `(replace "a-b" "-" "_")` → `"a_b"` |
| `like` | binary | — | Glob-style pattern match (* and ? wildcards) | `(like "hello" "hel*")` → `true` |
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
| `time` | unary | — | Current time, or extract time from timestamp | `(time 0)` → current time |
| `timestamp` | unary | — | Current timestamp (nanosecond precision) | `(timestamp 0)` |

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
| `type` | unary | — | Get the type name of a value | `(type 42)` → `i64` |
| `as` | binary | — | Cast value to another type | `(as 'i64 "42")` → `42` |
| `nil?` | unary | DAG in queries | Test if value is null; element-wise in query expressions | `(nil? 0Ni)` → `true` |
| `rc` | unary | — | Get reference count of an object | `(rc x)` → `1` |
| `guid` | unary | — | Generate a vector of N GUIDs (`(guid 0)` → `[]`) | `(guid 1)` |

```lisp
; Type checking and casting
(type [1 2 3])              ; I64
(type "hello")              ; str
(as 'f64 [1 2 3])          ; [1.0 2.0 3.0]
(nil? 0Ni)                   ; true

; Generate a batch of GUIDs
(guid 5)                    ; vector of 5 GUIDs
```

## I/O & Output { #io }

Printing, file I/O, CSV loading, and script execution.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `println` | variadic | — | Print values with newline | `(println "hello" 42)` |
| `print` | unary | — | Print value without newline | `(print "hello")` |
| `show` | variadic | — | Pretty-print a value (tables formatted) | `(show trades)` |
| `format` | variadic | — | Format value to string (% as placeholder) | `(format "val=%" 42)` → `"val=42"` |
| `.csv.read` | variadic | restricted | Load CSV file into table (mmap, parallel parse) | `(.csv.read "data.csv")` |
| `.csv.write` | variadic | restricted | Write table to CSV file | `(.csv.write trades "out.csv")` |
| `read` | unary | restricted | Read file contents as string | `(read "file.txt")` |
| `write` | binary | restricted | Write string to file | `(write "file.txt" "content")` |
| `load` | unary | restricted | Load and evaluate a Rayfall script file | `(load "lib.rfl")` |
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
| `.sys.gc` | variadic | — | Trigger GC / heap flush, returns `0` | `(.sys.gc)` |
| `.mem.objsize` | unary | — | Logical bytes retained by an object graph; shared children count once | `(.mem.objsize value)` |
| `.mem.ts` | special form | — | Evaluate once and return result, time, allocation, peak-memory, result-size, and worker statistics as a dict | `(.mem.ts (select {from: trades}))` |
| `.sys.exec` | unary | restricted | Execute a shell command, return exit code | `(.sys.exec "ls -la")` |
| `.os.getenv` | unary | restricted | Get environment variable value | `(.os.getenv "HOME")` |
| `.os.setenv` | binary | restricted | Set environment variable | `(.os.setenv "KEY" "value")` |
| `.sys.args` | nullary | — | Application arguments as a typed dict; `user` subdict holds post-`--` args | `(.sys.args)` |
| `env` | unary | — | List all global environment bindings | `(env 0)` |
| `.time.now` | variadic | — | Monotonic time in milliseconds | `(.time.now)` |
| `.time.timer.set` | variadic | restricted | Schedule callback every `ms`, `num` times (0 = forever); returns id | `(.time.timer.set 1000 0 (fn [t] (println t)))` |
| `.time.timer.del` | unary | restricted | Cancel a scheduled timer by id; returns null | `(.time.timer.del 0)` |
| `.sys.build` | variadic | — | Build metadata dict with `version` + `build-date` | `(.sys.build)` |
| `.sys.mem` | variadic | — | Memory allocator statistics (alloc / peak / slab hits) | `(.sys.mem)` |
| `.sys.prof` | variadic | — | Last profiled query's per-step statistics as a table (opt-in via `:t`) | `(.sys.prof)` |
| `.sys.querylog` | variadic | — | Ambient per-query statistics ring as a table (opt-in via `-Q` / `.sys.querylog.enable`) | `(.sys.querylog)` |
| `.sys.querylog.enable` | variadic | restricted | Toggle query-statistics logging; returns new state | `(.sys.querylog.enable 1)` |
| `.sys.info` | variadic | — | System information (cores, page size, total memory) | `(.sys.info)` |

```lisp
; Metaprogramming: parse and eval
(set expr (parse "(+ 10 20)"))
(eval expr)                  ; 30

; Timing with monotonic millisecond clock
(set t0 (.time.now))
(sum (til 1000000))
(println (format "%ms" (- (.time.now) t0)))

; System diagnostics
(.sys.info 0)                  ; OS, CPU cores, memory budget
(.sys.mem 0)                  ; heap usage statistics
```

## Serialization

Binary serialization for any Rayforce object. Useful for IPC, caching, and persistence.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `ser` | unary | — | Serialize any value to binary format (byte vector) | `(ser [1 2 3])` |
| `de` | unary | — | Deserialize from binary format back to value | `(de bytes)` → `[1 2 3]` |

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
| `.db.splayed.set` | variadic | restricted | Save table as splayed columns to a directory | `(.db.splayed.set "db/trades" trades)` |
| `.db.splayed.get` | variadic | — | Load splayed table from a directory | `(.db.splayed.get "db/trades")` |
| `.db.parted.get` | variadic | — | Load partitioned table from root directory | `(.db.parted.get "db" 'trades)` |
| `.db.parted.tables` | variadic | — | List table names under a parted root (from the most recent partition) | `(.db.parted.tables "db")` |
| `.db.parted.fill` | variadic | restricted | Backfill missing tables across a parted db's partitions | `(.db.parted.fill "db")` |

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

TCP-based IPC for connecting to remote Rayforce instances. Uses binary serialization on the wire.

| Function | Type | Flags | Description | Example |
|---|---|---|---|---|
| `.ipc.open` | variadic | restricted | Open TCP connection to host:port (optional connect timeout in ms), returns handle | `(.ipc.open "localhost:5000" 2000)` |
| `.ipc.close` | unary | restricted | Close an IPC connection handle | `(.ipc.close h)` |
| `.ipc.send` | binary | restricted | Send a value over an IPC handle (sync request) | `(.ipc.send h "(sum (til 100))")` |
| `.ipc.handle` | variadic | — | Current connection handle inside any `.ipc.on.*` hook, `-1` outside | `(.ipc.handle)` |
| `.ipc.on.open` | hook | user-settable | Fires after inbound connection completes handshake; arg = handle | `(set .ipc.on.open (fn [h] ...))` |
| `.ipc.on.close` | hook | user-settable | Fires before inbound connection teardown; arg = handle | `(set .ipc.on.close (fn [h] ...))` |
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

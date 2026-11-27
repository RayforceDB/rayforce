# Control Flow & Error Handling

Conditionals, variable binding, lambdas, error handling, null semantics, and higher-order functions in Rayfall.

## Conditionals: if

The `if` special form evaluates a condition and returns the corresponding branch:

```lisp
‣ (if (> 5 0) "positive" "non-positive")
"positive"
```

Without an else branch, `if` returns `0`:

```lisp
‣ (if 0 "yes")
0
```

More condition/result pairs chain: conditions are tested in order, the
result paired with the first truthy one is evaluated, and an odd trailing
argument is the fallback. Conditions after the first truthy one, and every
other result, are never evaluated:

```lisp
‣ (set classify (fn [x] (if (> x 100) "high" (> x 50) "mid" "low")))
‣ (classify 60)
"mid"
‣ (if false 1 false 2)
0
```

Inside a query (a projection, `where:`, a `by:` key), a chain with a fallback is evaluated row by row like the three-argument form; a chain without a fallback is left to the evaluator. This holds wherever the `if` sits in the expression: in an argument of a builtin call (`(== a (if c 'x 'y))`, `(not ...)`), of `and` and `or`, of `do`, in the value of a `let`, in the expression of a `try`, and in the body of a helper the planner inlines (a pure one-statement lambda). So `(and (> p 0) (== a (if c 'x 'y)))` compares each row with its own arm. These forms keep their own rules: `and` and `or` stop on a scalar that decides them, `do` evaluates in turn, and `try` hands an error raised anywhere in its expression, a mix of arm types included, to its handler for the whole column. Arguments are evaluated left to right, as outside a query, so a `let` in one binds for the arguments after it only: `(== a (let a (if c 'x 'y)))` compares the column with the new value, and `(== (let a (if c 'x 'y)) a)` compares the new value with itself.

A `where:` predicate or a `by:` output that the planner leaves to the evaluator chooses per row as well, because the evaluator's `if` chooses per row for a boolean-vector condition (below). That covers a side effect in the expression (a lambda that sets a global, `set`, a `try` handler that raises), an `if` inside a helper the planner does not inline (several statements, a closure, recursion), a nested query in an arm or beside the `if` such as `(== a (if c (first (at (select {k: k from: U}) 'k)) 'y))`, and a `by:` output where a `let` in one aggregate's argument binds a name the rest of the output also reads: that output is evaluated per group as written, and each group's `if` chooses per row within the group. A side effect in an arm runs once per evaluation, whatever the condition holds, since both arms are evaluated.

A name a `let` in an earlier form of a `do` binds is seen by its last form: `(select {from: T where: (do (let k (if (== act 'New) 'x 'y)) (== a k))})` keeps the rows whose `a` equals that row's arm, since the earlier forms run once over the rows first ([#1612](https://github.com/RayforceDB/rayforce/issues/1612); see [`do` inside a query](#do-inside-a-query)).

### A boolean vector as the condition

When the condition is a boolean vector, `if` chooses per row, outside a query as inside one. Both arms are evaluated, because every row needs one of them; each arm is a vector of the condition's length or an atom that broadcasts:

```lisp
‣ (if [true false true] [1 2 3] [10 20 30])
[1 20 3]
‣ (if [true false] "x" "y")
["x" "y"]
‣ (if (> [3 8 5] 4) 1 0)
[0 1 1]
```

Both arms are always evaluated, whatever the condition holds, so their side effects happen and an arm that raises raises: `(if (< v 100) v (raise 7))` raises as soon as `v` is a vector, even if no cell is `>= 100`. A boolean vector of length one gives a vector of length one, not an atom: `(if [true] 1 2)` is `[1]`. An empty one gives an empty vector.

The result type follows the promotion `if` has in a query: numbers promote, a temporal beside a number or another temporal type raises `type`. An arm of the wrong length raises `length`, and an arm that is neither a vector nor an atom raises `type`. A vector choice needs its fallback, so `(if [true false] 1)` raises `domain`.

In a chain, a boolean-vector condition chooses per row between its result and the rest of the chain, which is evaluated whole; a later condition that is a single value picks a whole result, as it does alone:

```lisp
‣ (if [true false false] [1 2 3] [false true false] [10 20 30] 0)
[1 20 0]
```

Inside a compiled lambda the per-row path runs on the tree walker, which has two limits that a scalar condition does not meet:

- `if`s written inside each other's then arms, each with a vector condition, share the tree walker's depth limit: about five hundred levels raise `limit`.
- A lambda that calls itself from an arm of an `if` with a vector condition holds one of the runtime's 64 scope frames per level, so about sixty levels of that recursion raise `limit` (`stack overflow`). The same lambda with a scalar condition recurses as far as any other, past two hundred levels. Prefer a loop or a fold over a mask to a recursive helper.

A condition that is a single value, or a vector of another type, is unchanged: it is tested as one truth value and only the arm it takes is evaluated.

A query's `if` gives one column, so its arms must have a common type. Two
numeric arms take the [promotion table](math.md#result-types) (`(if c i 1.5)`
over an i64 column is f64). Two temporal arms of one type keep it, and a
temporal arm beside a null literal (`0N`, `0Nd`, `0Np`, ...) keeps its own
type, the null becoming its null. Arms of two different temporal types (any
two of DATE, TIME, TIMESTAMP and TIMESPAN) have no common type and raise a
`type` error, as a text arm beside a number does. So does a temporal arm
beside a numeric one (BOOL, U8, I16, I32, I64, F32, F64): the temporal arm's
raw count (days, ms or ns) is not a number of the other arm's kind. The rule
holds on every route of a query: an arm the planner cannot compile (it is
evaluated per row or per group), a `where:` predicate, a chain without a
fallback (a row no condition takes is `0`, an I64) and
`(cond (p1 e1) ... (else en))`, which is the chain `(if p1 e1 ... en)`:

```lisp
‣ (set t (table [g d p] (list [0 1] [2024.01.01 2024.01.02] [2024.01.01D00:00:00.000000000 2024.01.02D00:00:00.000000000])))
‣ (select {from: t r: (if (> g 0) d p)})
error: type: if: branches mix DATE and TIMESTAMP
‣ (select {from: t r: (if (> g 0) d (as 'DATE p))})   ; convert one arm first
‣ (select {from: t r: (if (> g 0) d 0)})
error: type: if: branches mix DATE and I64
‣ (select {from: t r: (if (> g 0) (as 'I64 d) 0)})     ; the raw day count
```

Text arms keep the type of the text. String literals (`(if c "a" "b")`, the
same chain, or `(cond ((> n 2) "a") (else "b"))`) give a string (`STR`)
column, as a string literal standing alone in a select does, so the two
compare. A symbol arm, such as a symbol column or a `'b` literal, makes the
column symbols (`SYM`). Use string arms when the column is compared with
strings, and symbol arms when it is compared with symbols, joined on a symbol
key or stored as a symbol column:

```lisp
‣ (set t (table [n s] (list [1 2 3] ['a 'b 'a])))
‣ (set u (select {from: t s: s r: (if (> n 1) "a" "b") q: (if (> n 1) 'a 'b)}))
‣ (count (select {from: u where: (== r "a")}))    ; r is a string column
2
‣ (count (select {from: u where: (== q s)}))      ; q is a symbol column
1
‣ (select {from: u where: (== r s)})
error: type: =: incomparable operand types, got str and sym
```

A string column does not mix with symbols: `==` against a symbol raises
`type`, a join on a symbol key raises `type`, and `concat` with symbols gives
a list. `in` against a set of symbols raises nothing and matches no row, so
`(in r ['a])` over a string column `r` is always false: test it with
strings, `(in r ["a"])`, or convert it first, `(in (as 'SYM r) ['a])`.

Outside a query `if` evaluates one arm only, so its arms are never typed
against each other.

## Sequential Evaluation: do

`do` evaluates expressions in order and returns the last result. At the top
level, interpreted `do` gives `let` bindings a block scope. In a compiled
function body, `do` runs inline and a `let` can remain visible afterward:

For the full name lookup rules, see [Names and scope](name-resolution.md).

```text
‣ (do
    (set x 10)
    (set y 20)
    (+ x y))
30
```

### `do` inside a query

Inside a query clause (`where:`, an output column, `update`, `delete`, a `by:`
key or an aggregate), a `do` of several forms runs its earlier forms **once per
query**, in order, over the selected rows: the rows `where:` keeps, or every row
without one. Its last form is then planned as if it stood alone, and the names a
`let` among the earlier forms binds are visible to it. `from:` and `where:` are
still evaluated once each.

```text
‣ (set T (table [a i g] (list ['x 'y 'x 'y] [1 2 3 4] [0 1 0 1])))
‣ (at (select {from: T where: (do (let k 'x) (== a k))}) 'i)
[1 3]
‣ (at (select {v: (do (let k (* i 10)) (+ i k)) from: T where: (> i 1)}) 'v)
[22 33 44]
```

- **Effects happen once.** A `set`, a helper that writes a global or a
  `println` among the earlier forms runs once for the query, not once per row
  or per group.
- **Errors are raised.** A `raise`, a type error or an unknown name in an
  earlier form fails the query, as it would outside one.
- **A `let`'s value is a binding of the query.** Its provenance decides how the
  last form reads it, not its length. A value computed from the rows is
  column-like: it lines up with the selected rows, beside an aggregate, under
  `by:` and in `update … where:` alike, on in-memory and partitioned tables. Any
  other value follows the rule for a vector written in the query (a global, a
  parameter): it is a constant, and in an aggregate one as long as the table
  is read as a column ([Aggregate sources](../queries/select.md#aggregate-sources)).
  The name is never read as a column, and a `let` of a name the query also has
  as a column or a global hides it in that `do`'s last form only: the other
  clauses still read the column or the global.
- **Under `by:`, the earlier forms run over the whole selection, not per
  group.** A `let` bound to an aggregate is therefore the aggregate of every
  selected row:

  ```text
  ‣ (set T6 (table [i g] (list (til 6) (% (til 6) 2))))
  ‣ (at (select {v: (do (let s (sum i)) (+ (sum i) s)) from: T6 by: g}) 'v)
  [21 24]
  ```

  `s` is 15, the sum over all six rows, so each group gets its own sum (6, 9)
  plus 15, not twice its own sum (`[12 18]`). To use each group's value, write
  the aggregate in the last form, or call a helper of more than one statement,
  which runs once per group.

These are the forms a clause always evaluates: the clause itself, a call's
arguments, the first argument of `and` and `or`, the condition of an `if`, and
an aggregate's argument. A `do` anywhere else is evaluated where it stands,
each time the code around it runs, and its earlier forms run with it:

- in an `if` arm, with the arm, as the `if` evaluates it: once per row that
  takes the arm in a projection or `update`, and once over every row in
  `where:`, where both arms are evaluated and the `if` then chooses per row. In
  `update`, an arm whose `do` aggregates raises `nyi`
  (`(do (let s (sum i)) (+ i s))`), as an `if` over a column that also
  aggregates does there;
- in a later argument of `and` or `or`, once, when that argument is evaluated;
- in a `try`, with the `try`: once per row in a projection;
- in a helper's body, each time the helper is called: once per row in a
  projection when the planner does not inline it, once per group under `by:`.

Its earlier forms run there, so their effects happen and their errors are
raised. The one exception is an `if` arm in `update` that would otherwise
raise `nyi`. There, earlier forms that can neither raise nor have an effect
(a literal, a column, `count`, `first` or `last` of a column) are skipped,
and the arm is planned as its last form: `(update {v: (if (> i 2) (do (count
i) i) 0) from: T})` answers.

A `do` of one form is that form.

The order of evaluation is: `from:`; then the earlier forms of a `do` in
`where:`; then the earlier forms of each other clause, in the order the
clauses are written. `where:` itself is evaluated once, the first time one of
those forms reads a row, or else by the query after all of them have run. So
an output's earlier form that reads no rows (`(do (set n 0) …)`) runs before
`where:`'s own effects, such as a helper in the predicate that writes a
global.

See [#1650](https://github.com/RayforceDB/rayforce/issues/1650),
[#1612](https://github.com/RayforceDB/rayforce/issues/1612) and
[#1653](https://github.com/RayforceDB/rayforce/issues/1653).

## Iteration: while

`while` evaluates `cond`, and while it is truthy evaluates each body expression
in order, then tests again. It always returns null — it is a statement form,
run for effect.

```text
‣ (set n 5)
‣ (set total 0)
‣ (while (> n 0) (set total (+ total n)) (set n (- n 1)))
‣ total
15
```

It is the only iteration form that can stop early. `map`, `fold`, `scan` and
`prior` all consume their whole input, so a "repeat until done" loop written as
a fold over a fixed range pays that range's full length on every call, however
early the work finishes. `while` stops when the condition says stop, allocates
no range, and does not recurse — so it is not bounded by the stack depth a
recursive loop would hit.

The body may be omitted, in which case a condition with side effects is the
whole loop. That is the natural shape when there is no sequence to iterate over
in the first place:

```text
‣ (while (drain-one-batch))
```

Unlike `do`, `while` pushes no scope of its own. A `let` in the body binds in
the enclosing frame and therefore survives the iteration, which is what makes a
`let` usable as a loop variable inside a lambda:

```lisp
((fn [n]
   (let i 0)
   (let acc 0)
   (while (< i n) (let acc (+ acc i)) (let i (+ i 1)))
   acc) 4)                       ; => 6
```

When a fresh binding per pass is wanted instead, wrap the body in `do`, which
does push a scope:

```lisp
(while (< i 3) (do (let tmp (* i i)) (use tmp)) (set i (+ i 1)))
```

A loop whose condition never goes false runs until interrupted; Ctrl-C breaks
out of one at the REPL.

Unlike `if`, `while` tests a vector condition as one truth value: a non-empty
vector is true, so a loop over a vector whose cells all turn false still runs.

Inside a query, a `while` condition that is a vector (or a list) whose length is
the number of rows being evaluated (the query's rows, or the group's rows under
`by:`) is tested as a whole, so that a loop over the rows cannot run forever.
It does not matter what computes the vector: `(> v 15)` in a helper or a lambda
called with a column, `(and (> v 15) true)`, `(if (> v 15) true false)`, but
also a global, a constant or another table's column that happens to have that
length.

- A boolean vector true on every row is true.
- A boolean vector false on every row, or an empty one when there are no rows,
  is false, so the loop stops.
- A boolean vector true on some rows and false on others, or a vector of any
  other type, raises `nyi`: a loop cannot take a different number of passes
  for each row.

A vector of any other length, a single value, a `while` outside a query, and a
projection evaluated one row at a time are tested as before. Compared with
testing the vector as one truth value, which every query did before, three
cases now differ: a vector false on every row ends the loop where it used to
run it, a mixed boolean vector or a vector of another type raises `nyi` where
it used to count as true, and an empty vector in a query with no rows ends the
loop where it used to run it. A loop that only needs to know whether any or
every cell holds should test `(any c)` or `(all c)`, which are single values.
Write a loop over one row's value and call the helper from a projection,
where it runs once per row, or replace the loop with a vector expression.

## Bounded Iteration: times

`times` runs the body a fixed number of times and returns null.

```text
‣ (set n 0)
‣ (times 5 (set n (+ n 1)))
‣ n
5
```

The count is evaluated **once**, on entry, so the bound is fixed however the
body mutates whatever produced it:

```lisp
(set k 3)
(times k (set k (+ k 10)))      ; runs 3 times, not forever
```

A count of zero or less runs the body zero times rather than raising — a bound
that computes to empty is a no-op, not an error. A non-integer count is a type
error. Like `while`, `times` pushes no scope of its own, so a `let` in the body
binds in the enclosing frame; wrap the body in `do` when a fresh binding per
pass is wanted.

To loop over positions, give `times` a `[name count]` binding instead of a bare
count. The name takes `0`, `1`, …, `count - 1`, one value per pass:

```lisp
(times [i (count qty)]
  (alter 'qty set i (+ (at qty i) (at delta i))))
```

The index is bound like a `let`, in the enclosing frame, and keeps the last
index once the loop ends; a loop that runs no pass leaves it as it was. It is
set from the loop's own counter at the start of every pass, so a body that
rebinds the name changes it for the rest of that pass only, never the passes
that follow. The count follows the rules above: evaluated once, an integer,
nothing run when it is zero or less.

Reach for `times` when the number of passes is known up front and for `while`
when it is not. Neither allocates a sequence, so neither pays for a range that
exists only to be counted.

## Variable Binding: set and let

`set` creates a global binding. `let` creates a local binding scoped to the enclosing `do`:

```lisp
‣ (set x 42)
42

‣ (do (let y 10) y)
10
```

The variable `y` is not visible outside the `do` block.

Both return the value they bind, as the `42` above shows. Sent to another process through `.ipc.send`, that value is the reply: see [Remote assignments](../namespaces/ipc.md#remote-assignments) for assigning a large value without receiving it back.

## Lambda Functions: fn

Create anonymous functions with `fn`. Parameters are listed in square brackets:

```lisp
‣ (set add1 (fn [x] (+ x 1)))
‣ (add1 5)
6
```

Recursive lambdas use `self` to refer to the enclosing function:

```lisp
‣ (set fib (fn [n] (if (<= n 1) n (+ (self (- n 1)) (self (- n 2))))))
‣ (fib 10)
55
```

`self` is also a value: `(map self node)` recurses over a nested list. Inside a closure it is the closure itself, not the function that made it.

## Error Handling: try / raise

`raise` throws an error with an arbitrary value. `try` catches it and passes the value to a handler function:

```lisp
‣ (try (raise 42) (fn [e] e))
42

‣ (try (raise 42) (fn [e] (+ e 1)))
43

‣ (try (raise "boom") (fn [e] "caught"))
"caught"
```

An error the engine raises itself reaches the handler as a dict of its code and message, and, when it is about a builtin called with the wrong arguments, the call shape as `usage`:

```lisp
‣ (try (+ 1 "hello") (fn [e] e))
{code:type msg:"cannot add i64 and str" usage:"(+ <left: num, date, time, timestamp or timespan> <right: the same kinds>)"}
```

Match on `code`, the class of the error; `msg` is for people, and `usage` shows the call shape (see [Error Handling](../guides/errors.md#usage-lines)). An error that is not about a call, such as a bad value, has only `code` and `msg`.

If no error is raised, `try` returns the result of the body expression normally. Works inside lambdas compiled to bytecode.

### Fallback value

If the second argument is **not** a function, it is returned as-is as the fallback value on error (evaluated only when the body fails):

```lisp
‣ (try (raise "boom") 0)
0

‣ ((fn [data] (try (raise "boom") data)) 123)
123
```

A handler must accept the single error argument, so only a lambda or a unary builtin is *called* with the error; any other value (including a multi-argument builtin) is treated as a fallback value.

Nested lambdas capture visible lexical bindings when they are created:

```lisp
‣ (set make-adder (fn [x] (fn [y] (+ x y))))
‣ (set add7 (make-adder 7))
‣ (add7 5)
12
```

Inside the closure its captures are locals: a `let` of `x` rebinds it for that call only, and a capture named like a builtin is the callee. See [Lambdas & the VM](syntax.md#lambdas-the-vm) for how closures compile.

## Early Return: return

`return` exits the innermost enclosing lambda early with the given value, whether it runs as bytecode or on the tree-walking interpreter:

```lisp
‣ (set f (fn [x] (if (< x 0) (return -1) (+ x 1))))
‣ (f -5)
-1
‣ (f 5)
6
```

The zero-arg form returns null:

```lisp
‣ ((fn [] (return)))
‣
```

`return` works inside `(try ...)`: the trap frame is unwound cleanly before the lambda exits.

```lisp
‣ ((fn [] (try (return 42) (fn [e] e))))
42
```

`return` only exits the lambda it is lexically inside — not any outer lambdas. There is no non-local return.

## Null Semantics

Nulls are sentinel-encoded in the payload (`INT64_MIN` for `i64`, `NaN` for
`f64`, and so on), not a separate bitmap. Typed null literals produce the
sentinel for their type:

| Literal | Type |
|---|---|
| `0Nl` | i64 null |
| `0Nf` | f64 null |
| `0Ni` | i32 null |
| `0Nh` | i16 null |
| `0Nd` | date null |
| `0Nt` | time null |
| `0Np` | timestamp null |

Null rules:

- `nil?` checks for null: `(nil? 0Nl)` → `true`
- All null forms are falsy in `if`
- All null forms are equal via `==`: `(== 0Nl 0Nl)` → `true`
- Typed nulls propagate through arithmetic: `(+ 0Nl 1)` → `0Nl`
- `println` and `show` return null (not printed in the REPL)

```lisp
‣ (nil? 0Nl)
; => true

‣ (+ 0Nl 1)
; => 0Nl

‣ (nil? (println "hello"))
; prints: hello
; => true
```

## Higher-Order Functions

Lambdas are auto-mapped over vectors when called directly. Use `map` for explicit element-wise application, `fold` for reductions, and `scan` for running accumulations:

```lisp
;; map applies a function to each element; same-type atom results
;; form a typed vector, anything else a list
‣ (map (fn [x] (* x x)) [1 2 3 4 5])
; => [1 4 9 16 25]

;; lambdas auto-map over vectors
‣ ((fn [x] (* x x)) (til 5))
; => [0 1 4 9 16]

;; fold reduces a vector with a binary function
‣ (fold + 0 (til 10))
; => 45

;; scan produces running accumulations
‣ (scan + [1 2 3 4 5])
; => [1 3 6 10 15]

;; fold-while stops as soon as the running result fails the predicate,
;; leaving the rest of the collection untouched
‣ (fold-while (fn [acc] (< acc 100)) + 0 (til 1000))
; => 105

;; where returns indices matching a condition
‣ (where (> (til 10) 3))
; => [4 5 6 7 8 9]
```

`map`, `fold`, `scan` and `prior` all consume their whole input. `fold-while`
is the one member of the family that can stop: the accumulator is offered to
the predicate before each step, and a falsy answer ends the fold and yields the
accumulator as it stands. The test happens before the first element too, so a
predicate that is false at the start returns the initial value untouched.
Elements are pulled one at a time, so a fold that stops after three steps costs
three elements however long the collection is.

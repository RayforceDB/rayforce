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

A `where:` predicate leaves the choice to the evaluator, which tests the condition vector whole and so gives every row the first arm, in four cases:

- a side effect anywhere in it: a lambda that sets a global, `set`, or a `try` handler that raises ([#1610](https://github.com/RayforceDB/rayforce/issues/1610));
- an `if` inside a helper the planner does not inline: several statements, a closure, or recursion ([#1611](https://github.com/RayforceDB/rayforce/issues/1611));
- a name a `let` binds and a later argument of the same `do` reads: this raises `schema` ([#1612](https://github.com/RayforceDB/rayforce/issues/1612));
- a nested query anywhere in it, in an arm or beside the `if`, such as `(== a (if c (first (at (select {k: k from: U}) 'k)) 'y))`; the same holds for such an output under `by:` ([#1629](https://github.com/RayforceDB/rayforce/issues/1629)).

Projections answer per row in all four.

Under `by:`, an output where a `let` in one aggregate's argument binds a name that the rest of the output also reads, such as `(+ (sum (== a (let a (if c 'x 'y)))) (sum (== a 'x)))`, is evaluated per group as written. Its `if` then tests each group's condition vector whole ([#1617](https://github.com/RayforceDB/rayforce/issues/1617)). With one aggregate, `(sum (== a (let a (if c 'x 'y))))`, the output answers per row.

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

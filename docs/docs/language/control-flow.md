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

## Sequential Evaluation: do

`do` evaluates expressions in order and returns the last result. `let` bindings inside `do` are scoped to that block:

```text
‣ (do
    (set x 10)
    (set y 20)
    (+ x y))
30
```

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

## Early Return: return

`return` exits the innermost enclosing compiled lambda early with the given value:

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
;; map applies a function to each element, returning a list
‣ (map (fn [x] (* x x)) [1 2 3 4 5])
; => (1 4 9 16 25)

;; lambdas auto-map over vectors
‣ ((fn [x] (* x x)) (til 5))
; => [0 1 4 9 16]

;; fold reduces a vector with a binary function
‣ (fold + 0 (til 10))
; => 45

;; scan produces running accumulations
‣ (scan + [1 2 3 4 5])
; => [1 3 6 10 15]

;; where returns indices matching a condition
‣ (where (> (til 10) 3))
; => [4 5 6 7 8 9]
```

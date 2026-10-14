# Error Handling

Rayfall provides structured error handling through `try` and `raise`, three distinct null representations for missing data, and consistent error propagation rules. This guide covers every pattern you need for robust Rayfall programs.

## 1. try/raise Basics

The `try` special form evaluates an expression and, if it produces an error, calls a handler function with the error value. The `raise` function creates an error explicitly.

### Basic Syntax

```text
(try expr handler)
```

- **expr** — the expression to evaluate
- **handler** — a function that receives the error value if `expr` fails

Both arguments are passed unevaluated (`try` is a special form). The handler is only evaluated and called if `expr` produces an error.

### Catching a Type Error

```lisp
(try
  (+ 1 "hello")
  (fn [err] (println "caught:" err)))
; caught: 0
```

When an error occurs without an explicit `raise`, the handler receives a default value of `0`.

!!! note "Division by zero is not an error"

    Unlike many languages, `(/ 10 0)` does **not** raise — it returns a
    typed null (`0Nf`). Use `nil?` to detect it, not `try`.

### Raising Custom Errors

```lisp
(try
  (raise "invalid input")
  (fn [err] (println "error:" err)))
; error: "invalid input"
```

With `raise`, the handler receives the exact value you raised — a string, number, or any Rayfall object.

### Returning a Default Value

```lisp
(try
  (+ 1 "hello")
  (fn [e] -1))
; => -1
```

The result of `try` is either the successful result of `expr` or the return value of the handler. This makes `try` an expression you can embed anywhere.

### Raising Structured Errors

```lisp
;; raise any value, not just strings
(try
  (raise 42)
  (fn [e] (+ e 8)))
; => 50
```

You can raise any Rayfall value. The handler receives it directly, so you can raise numbers, vectors, or even tables if you want structured error information.

### When No Error Occurs

```lisp
(try
  (+ 2 3)
  (fn [e] 0))
; => 5
```

If the expression succeeds, the handler is never called and the result passes through unchanged.

## 2. Error Types

Rayfall produces several categories of runtime errors. Understanding these helps you write better handlers.

### Type Errors

Occur when an operation receives the wrong argument type:

```text
(+ 1 "hello")
; error: type
```

Arithmetic, comparison, and most builtins check argument types and produce type errors for mismatches.

### Arity Errors

Occur when a function receives the wrong number of arguments:

```text
(+ 1)
; error: arity — + expects 2 arguments
```

### Domain Errors

Occur when arguments have valid types but invalid values:

```text
(as 'i64 "not-a-number")
; error: domain — cannot parse str as i64
```

Note that some operations you might expect to be domain errors are not.
Division by zero returns a typed null rather than raising:

```lisp
(/ 10 0)
; => 0Nf   (not an error)
```

And `take` has the signature `(take list count)` — list first, count second.
Over-taking **cycles** the list rather than truncating:

```lisp
(take [1 2 3] 5)
; => [1 2 3 1 2]   (wraps around, does not stop at 3)
```

### User-Raised Errors

Any error you create with `raise`:

```text
(raise "custom error message")
; error: domain: custom error message
```

Internally, `raise` stores your value and triggers a domain error. The `try` handler receives the stored value. When nothing catches it, the REPL, the script runner and a timer callback print the value after the code: a string as is, a table as `<table RxC>`, anything else in the REPL's display form, cut with `..` when long.

### Access Errors (Restricted Mode)

In restricted evaluation mode (used for IPC), certain functions are blocked:

```text
; on a restricted server connection:
(.sys.exec "rm -rf /some/path")
; error: access — restricted
```

### How Errors Display in the REPL

Unhandled errors print with a category prefix:

```text
rf> (as 'i64 "x")
error: domain

rf> (+ 1 "x")
error: type
```

Errors are terminal — an unhandled error stops evaluation of the current expression and returns to the REPL prompt.

## 3. Null Propagation

Missing data is a first-class concept in Rayforce. There are three distinct null forms, each with different behavior.

### The Three Null Forms

!!! note "Quick reference"

    - **RAY_NULL_OBJ** — the void return value (from `println`, `show`)
    - **Typed nulls** — `0Ni` (int), `0Nf` (float), `0Nd` (date) — missing values in data
    - **Vector null sentinels** — type-correct sentinel values in vector payloads

### Typed Null Literals

Typed nulls represent missing data within a specific type. They look like values but carry a null flag:

```lisp
;; Typed null literals
0Ni   ; null integer
0Nf   ; null float
0Nd   ; null date
```

### Null Propagation Through Arithmetic

Typed nulls propagate through arithmetic — any operation involving a typed null produces a typed null:

```lisp
(+ 1 0Ni)
; => 0Nl (promoted to i64 null because 1 is i64)

(* 3.14 0Nf)
; => 0Nf

(+ 0Ni 0Ni)
; => 0Ni
```

This is the standard SQL/database behavior: null in, null out. It prevents silent corruption of calculations with missing data.

### RAY_NULL_OBJ (Void Return)

Some operations like `println` and `show` return a void null. This is different from typed nulls — using it in arithmetic produces a type error, not propagation:

```text
(set x (println "hello"))
; hello
(+ x 1)
; error: type
```

### Nulls in Vectors

Vectors track null elements with type-correct sentinels plus the `RAY_ATTR_HAS_NULLS` hint. Null elements propagate through vectorized operations:

```lisp
; The null literal must match the vector's element type (0Nl for i64).
(set v [1 0Nl 3 4 0Nl])
(+ v 10)
; => [11 0Nl 13 14 0Nl]
```

### All Nulls Are Falsy

Every null form — void null, typed nulls, and null vector elements — evaluates to false in conditional contexts:

```lisp
(if 0Ni "truthy" "falsy")
; => "falsy"

(if 0Nf "truthy" "falsy")
; => "falsy"
```

### Testing for Null with nil?

The `nil?` function detects all null forms:

```lisp
(nil? 0Ni)
; => true

(nil? 42)
; => false

(nil? (println "x"))
; x
; => true
```

`nil?` is one of the few functions that safely handles the void null without producing a type error.

### Null Propagation in Arithmetic

Arithmetic with typed nulls propagates the null through:

```lisp
(+ [1 0Nl 3] [10 20 30])
; => [11 0Nl 33]  — null element stays null

(sum [1 0Nl 3])
; => 4  — aggregates skip nulls
```

## 4. Defensive Patterns

Practical patterns for writing Rayfall code that handles edge cases gracefully.

### Check for Null Before Operating

```lisp
(set val 0Ni)  ; could be a result from a lookup
(if (nil? val)
  0
  (* val 2))
; returns 0 for null, otherwise doubles the value
```

### Default Value Pattern

```lisp
;; define a "default" helper
(set with-default (fn [x d]
  (if (nil? x) d x)))

(with-default 0Ni -1)
; => -1

(with-default 42 -1)
; => 42
```

### Safe Division

```lisp
(set safe-div (fn [a b]
  (if (== b 0)
    0Nf
    (/ a b))))

(safe-div 10 3)
; => 3.333...

(safe-div 10 0)
; => 0Nf
```

### Guard with try for File I/O

```lisp
;; safely load a CSV, return empty table on failure
(set data
  (try
    (.csv.read "data.csv")
    (fn [e]
      (println "failed to load:" e)
      (table [x] (list [])))))
; if file missing: prints error, returns empty table
```

### Validate Table Columns Before Querying

```lisp
;; check that required columns exist
(set validate-cols (fn [tbl required]
  (set have (key tbl))
  (map (fn [c]
    (if (nil? (find have c))
      (raise (format "missing column: {}" c))))
    required)))

;; use it before a query
(try
  (do
    (validate-cols trades ['sym 'price 'qty])
    (select {from: trades by: {sym: sym} total: (sum qty)}))
  (fn [e] (println "validation failed:" e)))
```

## 5. Error Recovery in Pipelines

When building data pipelines, errors in one stage should not always halt the entire computation.

### Errors in select/update

If a computed column expression fails inside `select`, the entire select produces an error. Wrap the select in `try` to recover:

```lisp
(set trades (table [sym price qty]
  (list [AAPL GOOG MSFT]
        [150.5 2800.0 310.0]
        [100 50 75])))

;; safe query wrapper
(set safe-query (fn [q]
  (try q
    (fn [e]
      (println "query failed:" e)
      0Ni))))
```

### Processing Rows with Error Tolerance

```text
;; process each row, collecting errors separately
(set results
  (map (fn [row]
    (try
      (/ (get row revenue) (get row shares))
      (fn [e] 0Nf)))
    rows))
; failed rows get 0Nf, successful rows get the result
```

### Chaining try for Multi-Step Pipelines

```lisp
;; load -> transform -> aggregate, with recovery at each step
(set result
  (try
    (do
      (set raw (.csv.read "input.csv"))
      (set clean (select {from: raw where: (> price 0)}))
      (select {from: clean by: {sym: sym} p: (avg price)}))
    (fn [e]
      (println "pipeline failed:" e)
      0Ni)))
```

### Nested try for Granular Recovery

```lisp
;; inner try catches load failure, outer catches query failure
(try
  (do
    (set data
      (try
        (.csv.read "primary.csv")
        (fn [e]
          (println "primary failed, trying backup")
          (.csv.read "backup.csv"))))
    (select {from: data by: {sym: sym} total: (sum qty)}))
  (fn [e] (println "all attempts failed:" e)))
```

## 6. Quick Reference

| Pattern | Syntax | Behavior |
|---|---|---|
| Catch errors | `(try expr handler)` | Calls handler with error value on failure |
| Raise error | `(raise value)` | Triggers error, value passed to nearest try handler |
| Test for null | `(nil? x)` | Returns `true` for any null form, `false` otherwise |
| Typed null int | `0Ni` | Propagates through arithmetic |
| Typed null float | `0Nf` | Propagates through arithmetic |
| Typed null date | `0Nd` | Propagates through date operations |
| Null in if | `(if 0Ni ...)` | All nulls are falsy |
| Default value | `(if (nil? x) d x)` | Substitute default for missing data |

## Next Steps

- [**Control Flow**](../language/control-flow.md) — if/do/fn and other control structures
- [**Data Types**](../data-types/index.md) — Full type reference including null representations
- [**Memory & Monitoring**](memory.md) — Understanding memory usage and GC
- [**Select Queries**](../queries/select.md) — Building queries with select/update

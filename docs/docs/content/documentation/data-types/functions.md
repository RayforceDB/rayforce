# :material-function: Function

Functions in RayforceDB can be either built-in or user-defined lambdas.

## :material-lambda: Lambda

A Lambda is a user-defined function that has the Vary type code `103`.

```clj
(fn [x]
    (let p (format "%/%/%/" dbpath (+ 2024.01.01 x) tabname))
    (println "Creating table: %" p)
    (let t (table [OrderId Symbol Price Size Tape Timestamp] 
        (list (take n (guid 1000000))
                (take n [AAPL GOOG MSFT IBM AMZN FB BABA]) 
                (as 'F64 (+ x (til n))) 
                (take n (+ x (til 3)))
                (map (fn [x] (as 'String x)) (take n (+ x (til 3))))
                (as 'Timestamp (til n)) 
        )
    ))
    (set-splayed p t "/tmp/db/sym")
    (println "Table created")
)
```

To define a lambda, you need to use the keyword `fn` followed by a [:material-vector-line: Vector](./vector.md) of [:simple-scalar: Symbols](./symbol.md), which represent the function arguments. And then followed by any amount of expressions that has to be evaluated.

Within the function, you can utilize the `let` keyword (as opposed to `set`) to restrict the scope of the variable to the function, and not the whole runtime.

!!! note ""
    <b>The result value of a lambda is always the last expression that is evaluated.</b>

    Alternatively, the `return` keyword can be used to define the result early in the process.


### Unary Functions

!!! note ""
    Type Code: `101`

These are built-in functions that accept a single argument.

```clj
(sum [1 2 3 4])
10
```

### Binary Functions

!!! note ""
    Type Code: `102`

These are built-in functions that accept two arguments.

```clj
(+ 1 2)
3
```

### Vary Functions

!!! note ""
    Type Code: `103`

These are built-in functions that accept 1 or more arguments.

```clj
(set t (table [id name age] (list [1 2 3] ['a 'b 'c] [20 30 40])))
(upsert t 1 (list 4 'd 50))  ;; Vary function
```

## Control Flow and Evaluation

RayforceDB provides functions for controlling program flow, error handling, and execution order within functions and expressions.

### :material-play: Do

Evaluates expressions sequentially and returns the result of the last expression.

```clj
(do (set x 10) (set y 20) (+ x y))
30

(do (println "First") (println "Second") "Done")
"Done"

(do (set price 150.25) (set quantity 100) (* price quantity))
15025.00
```

### :material-code-brackets: If

Conditional execution. Evaluates a condition and executes the then-branch if true, otherwise executes the else-branch (if provided).

```clj
(if true "yes" "no")
"yes"

(if false "yes" "no")
"no"

(if (> 5 3) "greater" "less")
"greater"

(if 1 3)
3

(if 0 3)
null

;; Check if price is above threshold
(set price 150.25)
(if (> price 100) "high" "low")
"high"
```

!!! note ""
    Takes 2 or 3 arguments: condition, then-expression, and optionally else-expression. If no else-expression is provided and the condition is false, returns `null`.

### :material-shield-alert: Try

Error handling construct. Evaluates an expression and catches any errors, executing a catch handler if an error occurs.

```clj
(try (+ 1 2) (fn [e] 0))
3

(try (raise "error") (fn [e] 99))
99

(try (raise "error") "fallback")
"fallback"

;; Safe division with error handling
(try (/ 10 0) 0)
0

;; Try with custom error handler
(try (raise "Price calculation failed") (fn [e] (println "Error:" e) 0))
```


### :material-alert: Raise

Raises an exception with a string message. Execution jumps to the nearest try block's catch handler.

```clj
(raise "My error message")

(if (< price 0)
    (raise "Price cannot be negative")
    price)
```

!!! note ""
    Must be called with a string argument. Use within a `try` block to handle errors gracefully.

### :material-arrow-left: Return

Returns a value from the current function early.

```clj
(fn [x]
    (if (< x 0)
        (return "negative")
    (+ x 10)))

;; Early return for validation
(fn [price]
    (if (nil? price)
        (return null)
    (if (< price 0)
        (return "invalid")
    (* price 1.1))))
```


### :material-exit-run: Exit

Exits the RayforceDB process with an optional exit code.

```clj
(exit 0)

(exit 1)

(exit)
```

!!! note ""
    Takes 0 or 1 argument. With no argument, exits with code 0 (success). With an argument, exits with that code. Non-zero exit codes typically indicate errors.

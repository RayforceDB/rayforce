# :material-function: Function

Functions in RayforceDB can be either built-in or user-defined lambdas.

## :material-lambda: Lambda

A Lambda is a user-defined function that has the Vary type code `103`.

```clj
↪ (fn [x]
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
    ```clj
    (return 1)
    ```

    `return` is a Unary function and accepts only 1 argument.

The `try` keyword allows you to handle errors and exceptions in a controlled way. It accepts two arguments: the expression to be evaluated and a catch handler expression.

```clj
↪ (try (+ 1 2) (fn [e] 0))
3
↪ (try (raise "error") (fn [e] 99))
99
```

`try` blocks can be nested to handle errors at different levels:

```clj
(try (try (raise "inner") (fn [e] (raise "outer")))
     (fn [e] 42))  ;; Catches the outer error and returns 42
```


### Unary Functions

!!! note ""
    Type Code: `101`

These are built-in functions that accept a single argument.

```clj
↪ (sum [1 2 3 4])
10
```

### Binary Functions

!!! note ""
    Type Code: `102`

These are built-in functions that accept two arguments.

```clj
↪ (+ 1 2)
3
```

### Vary Functions

!!! note ""
    Type Code: `103`

These are built-in functions that accept 1 or more arguments.

```clj
↪ (set t (table [id name age] (list [1 2 3] ['a 'b 'c] [20 30 40])))
↪ (upsert t 1 (list 4 'd 50))  ;; Vary function
```

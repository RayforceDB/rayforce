# Timeit `timeit`

Measures execution time of an expression.

```clj
;; Measure simple calculation
↪ (timeit (+ 1 2))
0.015

;; Measure function execution
↪ (timeit (map {x: (* x 2)} [1 2 3 4 5]))
0.245

;; Measure with error handling
↪ (timeit (try (/ 1 0) {e: "Division by zero"}))
0.032

;; Multiple expressions
↪ (timeit (do 
            (set x 10)
            (+ x 20)
            (* x 2)))
0.128
```

!!! info "Syntax"
    ```clj
    (timeit expr)
    ```
    - `expr`: Expression to measure
    - Returns execution time in milliseconds

!!! warning
    - Time includes parsing and evaluation
    - Results may vary between runs
    - Precision depends on system timer resolution
    - Special form - does not evaluate argument first

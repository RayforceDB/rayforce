# Parse `parse`

Parses a string into an expression without evaluating it.

```clj
;; Parse a simple expression
↪ (parse "(+ 1 2)")
[+ 1 2]

;; Parse a function definition
↪ (parse "(fn [x] (* x 2))")
[fn [x] [* x 2]]

;; Parse multiple expressions
↪ (parse "
   (set x 10)
   (+ x 20)
   ")
[[set x 10] [+ x 20]]
```

!!! info "Syntax"
    ```clj
    (parse str)
    ```
    - `str`: String to parse

!!! warning
    - Input must be a string
    - Returns parsed expression without evaluating
    - Useful for inspecting code structure

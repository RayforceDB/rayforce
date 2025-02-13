# Eval `eval`

Evaluates a string or expression.

```clj
;; Evaluate a string
↪ (eval "(+ 1 2)")
3

;; Evaluate a list
↪ (set expr (list + 1 2))
[+ 1 2]
↪ (eval expr)
3

;; Evaluate with variables 
↪ (set x 10)
10
↪ (eval "(+ x 5)")
15

;; Evaluate multiple expressions
↪ (eval "(set y 20) (+ x y)")
30
```

!!! info "Syntax"
    ```clj
    (eval expr)
    ```
    - `expr`: String or expression to evaluate

!!! warning
    - String expressions must be valid Rayforce code
    - Returns the result of evaluation
    - Can evaluate both strings and expressions

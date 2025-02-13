# Quote `quote`

Prevents evaluation of an expression.

```clj
;; Quote a symbol
↪ (quote x)
'x

;; Quote a list
↪ (quote [+ 1 2])
[+ 1 2]

;; Quote prevents evaluation
↪ (set x 10)
↪ (quote x)
'x  ;; Returns symbol rather than value

;; Shorthand quote syntax
↪ 'x
'x
```

!!! info "Syntax"
    ```clj
    (quote expr)
    ```
    - `expr`: Expression to quote

!!! warning
    - Returns expression without evaluating
    - Useful for manipulating code as data
    - Can use ' as shorthand for quote

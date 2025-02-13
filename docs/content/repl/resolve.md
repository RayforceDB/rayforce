# Resolve `resolve`

Resolves a symbol to its bound value.

```clj
;; Resolve a variable
↪ (set x 10)
↪ (resolve 'x)
10

;; Resolve a function
↪ (resolve '+)
<function:+>

;; Resolve an undefined symbol
↪ (resolve 'undefined)
null
```

!!! info "Syntax"
    ```clj
    (resolve sym)
    ```
    - `sym`: Symbol to resolve

!!! warning
    - Symbol must be quoted
    - Returns bound value or null if unbound
    - Works for variables and functions

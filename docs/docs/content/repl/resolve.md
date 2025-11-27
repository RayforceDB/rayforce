# Resolve `resolve`

Resolves a symbol to its bound value.

```clj
;; Resolve a variable
↪ (set x 10)
10

↪ (resolve 'x)
10
```

```clj
;; Resolve a function (returns function reference)
(resolve '+)

;; Resolve an undefined symbol (returns null)
(resolve 'undefined)
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

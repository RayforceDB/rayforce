# Alter `alter`

Modifies an element at a specific index using a function.

```clj
;; Basic syntax: (alter obj func index value)
;; Applies (func element value) at the specified index

;; Set element at index
↪ (set arr [10 20 30])
↪ (alter arr set 0 99)
[99 20 30]

;; Add to element at index
↪ (set arr [10 20 30])
↪ (alter arr + 1 100)
[10 120 30]

;; Multiply element at index
↪ (set arr [10 20 30])
↪ (alter arr * 2 5)
[10 20 150]
```

!!! warning "Returns New Value"
    `alter` returns a modified copy - it does NOT modify the original.
    To persist changes, reassign:
    ```clj
    (set arr (alter arr set 0 99))
    ```

!!! info "Syntax"
    ```clj
    (alter object function index value)
    ```
    - `object`: Array, list, or table to modify
    - `function`: Binary function (`+`, `*`, `set`, etc.)
    - `index`: Index of element to modify
    - `value`: Value to apply with function

!!! tip "Common Functions"
    - `set` - Replace element: `(alter arr set 0 99)`
    - `+` - Add to element: `(alter arr + 0 10)`
    - `*` - Multiply element: `(alter arr * 0 2)`
    - `concat` - Append to element (for lists)

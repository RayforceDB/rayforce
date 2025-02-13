# Set Floating Point Resolution `set-fpr`

Sets the number of decimal places shown for floating point numbers.

```clj
;; Set floating point resolution to 2 decimals
↪ (set-fpr 2)
2

;; Demonstrate with calculations
↪ (/ 10 3)
3.33

;; Set to higher precision
↪ (set-fpr 4)
4
↪ (/ 10 3)
3.3333
```

!!! info "Syntax"
    ```clj
    (set-fpr precision)
    ```
    - `precision`: Number of decimal places to display

!!! warning
    - Precision must be a non-negative integer
    - Only affects display, not internal precision
    - Applies to all subsequent floating point output

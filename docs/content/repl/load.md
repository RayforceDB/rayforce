# Load `load`

Loads and evaluates code from a file.

```clj
;; Load a file
↪ (load "lib.rf")
null

;; Load with error handling
↪ (try (load "missing.rf") {e: "File not found"})
"File not found"
```

!!! info "Syntax"
    ```clj
    (load filename)
    ```
    - `filename`: Path to file to load

!!! warning
    - File must contain valid Rayforce code
    - Returns result of last evaluated expression
    - Throws error if file not found or contains invalid code

# Set Display Width `set-display-width`

Sets the maximum width for displaying tables and other formatted output.

```clj
;; Set display width to 40 characters
↪ (set-display-width 40)
40

;; Create a wide table to demonstrate
↪ (set t (table [name location department salary] 
                (list (list "Alice Smith" "New York" "Engineering" "75000")
                      (list "Bob Johnson" "San Francisco" "Marketing" "65000"))))

;; Display with width limit
↪ t
┌────────────┬──────────┬────────────┐
│ name       │ location │ department │
├────────────┼──────────┼────────────┤
│ Alice Smith│ New York │Engineering │
│Bob Johnson │San Fran..│ Marketing  │
└────────────┴──────────┴────────────┘
```

!!! info "Syntax"
    ```clj
    (set-display-width width)
    ```
    - `width`: Maximum display width in characters

!!! warning
    - Width must be a positive integer
    - Affects all subsequent table displays
    - Long values will be truncated with ".."

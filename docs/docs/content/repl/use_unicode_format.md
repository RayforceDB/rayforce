# Use Unicode Format `use-unicode-format`

Toggles between Unicode and ASCII characters for table borders and other formatting.

```clj
;; Enable Unicode formatting (if available)
(use-unicode-format true)
true

;; Disable Unicode formatting
(use-unicode-format false)
false
```

```clj
;; Create a sample table
↪ (table [name value] (list (list "A" "B") [1 2]))
┌──────┬───────────────────────────────┐
│ name │ value                         │
├──────┼───────────────────────────────┤
│ A    │ 1                             │
│ B    │ 2                             │
├──────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘
```

!!! info "Syntax"
    ```clj
    (use-unicode-format enabled)
    ```
    - `enabled`: Boolean to enable/disable Unicode formatting

!!! warning
    - Takes effect immediately for all subsequent output
    - May not display correctly in all terminals
    - Default is Unicode enabled

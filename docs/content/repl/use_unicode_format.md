# Use Unicode Format `use-unicode-format`

Toggles between Unicode and ASCII characters for table borders and other formatting.

```clj
;; Enable Unicode formatting
↪ (use-unicode-format true)
true

;; Create a sample table
↪ (table [name value] (list (list "A" "B") [1 2]))
┌──────┬───────┐
│ name │ value │
├──────┼───────┤
│ A    │ 1     │
│ B    │ 2     │
└──────┴───────┘

;; Disable Unicode formatting
↪ (use-unicode-format false)
false

;; Same table with ASCII
↪ (table [name value] (list (list "A" "B") [1 2]))
+------+-------+
| name | value |
+------+-------+
| A    | 1     |
| B    | 2     |
+------+-------+
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

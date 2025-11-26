# Update `update`

Updates columns in a table using select-style dict syntax.

```clj
;; Create table
(set t (table [name age] (list (list "Alice" "Bob" "Charlie") [25 30 35])))
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 30  │
│ Charlie │ 35  │
└─────────┴─────┘

;; Update all rows - multiply age by 2
(set t (update {age: (* age 2) from: t}))
┌─────────┬────────────────────────────┐
│ name    │ age                        │
├─────────┼────────────────────────────┤
│ Alice   │ 50                         │
│ Bob     │ 60                         │
│ Charlie │ 70                         │
├─────────┴────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘

;; Update with expression referencing column
(set t (table [x y] (list [1 2 3] [10 20 30])))
┌────┬──────────────────────────────────┐
│ x  │ y                                │
├────┼──────────────────────────────────┤
│ 1  │ 10                               │
│ 2  │ 20                               │
│ 3  │ 30                               │
├────┴──────────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)  │
└───────────────────────────────────────┘
(set t (update {y: (+ y x) from: t}))
┌────┬──────────────────────────────────┐
│ x  │ y                                │
├────┼──────────────────────────────────┤
│ 1  │ 11                               │
│ 2  │ 22                               │
│ 3  │ 33                               │
├────┴──────────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)  │
└───────────────────────────────────────┘

```

!!! warning "Returns New Table"
    `update` returns a new table - it does NOT modify in place.
    You must reassign to persist changes:
    ```clj
    (set t (update {col: value from: t}))
    ```

!!! info "Syntax"
    ```clj
    (update {column: expression from: table})
    ```
    - Uses dictionary syntax like `select`
    - `from:` is required
    - Column expressions can reference other columns

!!! note "Where Clause Limitations"
    The `where:` clause has limitations with complex expressions.
    For conditional updates, consider using `select` to filter rows first,
    then apply updates to the filtered subset.

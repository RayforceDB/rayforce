# Left Join `left-join`

Performs a left join between two tables, keeping all rows from the left table and matching rows from the right table.

```clj
;; Create sample tables
↪ (set x (table [a b c d] 
    (list (list "aaaaaa" "bbbbbbbbbb" "cc" "dd")
          ['I 'J 'K 'I] 
          [0 1 2 3]
          [0 1 2 3])))

↪ (set y (table [a b c d e] 
    (list (list "aa" "bb" "cc" "dd" "ee")
          ['I 'J 'K 'I 'J]
          [0 1 2 3 4]
          [0 1 2 3 4]
          [0 1 2 3 4])))

;; Left join on single column
↪ (left-join [a] x y)
┌────────────┬───┬───┬───┬─────┐
│ a          │ b │ c │ d │ e   │
├────────────┼───┼───┼───┼─────┤
│ aaaaaa     │ I │ 0 │ 0 │ null│
│ bbbbbbbbbb │ J │ 1 │ 1 │ null│
│ cc         │ K │ 2 │ 2 │ 2   │
│ dd         │ I │ 3 │ 3 │ 3   │
└────────────┴───┴───┴───┴─────┘

;; Left join on multiple columns
↪ (left-join [a b] x y)
┌────────────┬───┬───┬───┬─────┐
│ a          │ b │ c │ d │ e   │
├────────────┼───┼───┼───┼─────┤
│ aaaaaa     │ I │ 0 │ 0 │ null│
│ bbbbbbbbbb │ J │ 1 │ 1 │ null│
│ cc         │ K │ 2 │ 2 │ 2   │
│ dd         │ I │ 3 │ 3 │ null│
└────────────┴───┴───┴───┴─────┘
```

!!! info "Syntax"
    ```clj
    (left-join columns left-table right-table)
    ```
    - `columns`: Vector of column names to join on
    - `left-table`: First table to join (all rows preserved)
    - `right-table`: Second table to join (matching rows only)

!!! note
    - Keeps all rows from left table
    - Fills missing matches with null
    - Column names from both tables are preserved
    - Join columns must exist in both tables
    - Can join on multiple columns by providing a vector of column names

!!! tip
    Use left join when you want to keep all rows from the first table, even if they don't have matches

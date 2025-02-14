# Inner Join `inner-join`

Performs an inner join between two tables based on matching key columns.

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

;; Inner join on single column
↪ (inner-join [a] x y)
┌──────┬───┬───┬───┬───┐
│ a    │ b │ c │ d │ e │
├──────┼───┼───┼───┼───┤
│ cc   │ K │ 2 │ 2 │ 2 │
│ dd   │ I │ 3 │ 3 │ 3 │
└──────┴───┴───┴───┴───┘

;; Inner join on multiple columns
↪ (inner-join [a b] x y)
┌──────┬───┬───┬───┬───┐
│ a    │ b │ c │ d │ e │
├──────┼───┼───┼───┼───┤
│ cc   │ K │ 2 │ 2 │ 2 │
└──────┴───┴───┴───┴───┘
```

!!! info "Syntax"
    ```clj
    (inner-join columns left-table right-table)
    ```
    - `columns`: Vector of column names to join on
    - `left-table`: First table to join
    - `right-table`: Second table to join

!!! note
    - Only keeps rows where the join columns match
    - Rows without matches are excluded
    - Column names from both tables are preserved
    - Join columns must exist in both tables
    - Can join on multiple columns by providing a vector of column names

!!! tip
    Use inner join when you only want rows that have matches in both tables

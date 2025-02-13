# Cross Descending `xdesc`

Sorts a table in descending order based on specified columns.

```clj
↪ (set t (table [name age score] (list ["Bob" "Alice" "Charlie"] [25 30 20] [85 90 95])))
↪ (xdesc [score] t)
┌─────────┬─────┬───────┐
│ name    │ age │ score │
├─────────┼─────┼───────┤
│ Charlie │ 20  │ 95    │
│ Alice   │ 30  │ 90    │
│ Bob     │ 25  │ 85    │
└─────────┴─────┴───────┘
```

!!! info
    - First argument is a vector of column names to sort by
    - Supports multiple columns for hierarchical sorting
    - Modifies the table in place

!!! tip
    Use xdesc for sorting tables in descending order by one or more columns

!!! note
    For multiple columns, each column is sorted in descending order

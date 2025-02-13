# Cross Ascending `xasc`

Sorts a table in ascending order based on specified columns.

```clj
↪ (set t (table [name age score] (list ["Bob" "Alice" "Charlie"] [25 30 20] [85 90 95])))
↪ (xasc [name] t)
┌─────────┬─────┬───────┐
│ name    │ age │ score │
├─────────┼─────┼───────┤
│ Alice   │ 30  │ 90    │
│ Bob     │ 25  │ 85    │
│ Charlie │ 20  │ 95    │
└─────────┴─────┴───────┘
```

!!! info
    - First argument is a vector of column names to sort by
    - Supports multiple columns for hierarchical sorting
    - Modifies the table in place

!!! tip
    Use xasc for sorting tables by one or more columns

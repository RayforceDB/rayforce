# Insert `insert`

Adds new rows to a table.

```clj
↪ (set t (table [name age] (list (list "Alice" "Bob") [25 30])))
↪ (set t (insert t {name: 'Charlie age: 35}))
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 30  │
│ Charlie │ 35  │
└─────────┴─────┘

↪ (set t (insert t {name: ['David 'Eve] age:[40 25]}))
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 30  │
│ Charlie │ 35  │
│ David   │ 40  │
│ Eve     │ 45  │
└─────────┴─────┘
```

!!! info
    - Can insert a single row or multiple rows
    - Rows are added at the end of the table
    - Returns a new table (use `set` to reassign)

!!! warning
    New rows must have all required columns with correct types

!!! tip
    Insert multiple rows by providing lists as column values

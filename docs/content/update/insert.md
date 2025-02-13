# Insert `insert`

Adds new rows to a table.

```clj
↪ (set t (table [name age] [["Alice" "Bob"] [25 30]]))
↪ (insert t {name: "Charlie" age: 35})
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 30  │
│ Charlie │ 35  │
└─────────┴─────┘

↪ (insert t [{name: "David" age: 40} {name: "Eve" age: 45}])
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
    - Modifies the table in place

!!! warning
    New rows must have all required columns with correct types

!!! tip
    Use insert when adding new data to existing tables

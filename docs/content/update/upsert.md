# Upsert `upsert`

Updates existing rows or inserts new ones based on a key.

```clj
↪ (set t (table [id name age] [[1 2] ["Alice" "Bob"] [25 30]]))
↪ (upsert t id {id: 2 name: "Robert" age: 31})
┌────┬────────┬─────┐
│ id │ name   │ age │
├────┼────────┼─────┤
│ 1  │ Alice  │ 25  │
│ 2  │ Robert │ 31  │
└────┴────────┴─────┘

↪ (upsert t id {id: 3 name: "Charlie" age: 35})
┌────┬─────────┬─────┐
│ id │ name    │ age │
├────┼─────────┼─────┤
│ 1  │ Alice   │ 25  │
│ 2  │ Robert  │ 31  │
│ 3  │ Charlie │ 35  │
└────┴─────────┴─────┘
```

!!! info
    - First argument: table to modify
    - Second argument: key column(s) for matching
    - Third argument: row data to upsert
    - Updates existing row if key matches, otherwise inserts new row

!!! warning
    Key columns must uniquely identify rows

!!! tip
    Use upsert when you want to update existing rows or add new ones in a single operation

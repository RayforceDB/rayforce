# Update `update`

Updates existing rows in a table based on a condition.

```clj
↪ (set t (table [name age] [["Alice" "Bob" "Charlie"] [25 30 35]]))
↪ (update t (= name "Bob") {age: 31})
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 31  │
│ Charlie │ 35  │
└─────────┴─────┘

↪ (update t (> age 30) {age: (+ age 1)})
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 32  │
│ Charlie │ 36  │
└─────────┴─────┘
```

!!! info
    - First argument: table to modify
    - Second argument: condition for selecting rows
    - Third argument: updates to apply
    - Modifies the table in place

!!! warning
    Updates must maintain column types and constraints

!!! tip
    Use update for conditional modifications of existing rows

# Alter `alter`

Modifies elements in a collection based on a condition.

```clj
↪ (set arr [1 2 3 4 5])
↪ (alter arr (> arr 3) (* arr 2))
[1 2 3 8 10]

↪ (set t (table [name age] [["Alice" "Bob" "Charlie"] [25 30 35]]))
↪ (alter t (> age 28) {age: (+ age 1)})
┌─────────┬─────┐
│ name    │ age │
├─────────┼─────┤
│ Alice   │ 25  │
│ Bob     │ 31  │
│ Charlie │ 36  │
└─────────┴─────┘
```

!!! info
    - First argument: collection to modify
    - Second argument: condition for selecting elements
    - Third argument: transformation to apply
    - Modifies the collection in place

!!! warning
    The condition and transformation must be compatible with the collection's type

!!! tip
    Use alter for conditional updates on arrays or tables

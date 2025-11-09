# Filter `filter`

Filter a list by a boolean vector mask.

```clj
↪ (filter [1 2 3 4 5] [true false true false true])
[1 3 5]
↪ (filter "test" [true false true false])
"ts"
↪ (filter ['a 'b 'c 'dd] [true false false true])
[a dd]
```

!!! info
    - First argument is the collection to filter
    - Second argument is a boolean mask of the same length
    - Returns only elements where the corresponding mask value is `true`
    - Both arguments must have the same length

!!! tip
    Use `filter` to select elements based on a boolean condition mask
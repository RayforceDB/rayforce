# Index Descending `idesc`

Returns indices that would sort the input in descending order.

```clj
↪ (idesc [3 1 4 1 5])
[4 2 0 1 3]
↪ (idesc ["banana" "apple" "cherry"])
[2 0 1]
```

!!! info
    - Returns indices instead of sorting the actual data
    - Result can be used to reorder multiple related arrays
    - Works with any comparable types

!!! tip
    Use idesc when you need to sort multiple arrays in reverse order based on one array's order

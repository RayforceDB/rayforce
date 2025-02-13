# Index Ascending `iasc`

Returns indices that would sort the input in ascending order.

```clj
↪ (iasc [3 1 4 1 5])
[1 3 0 2 4]
↪ (iasc ["banana" "apple" "cherry"])
[1 0 2]
```

!!! info
    - Returns indices instead of sorting the actual data
    - Result can be used to reorder multiple related arrays
    - Works with any comparable types

!!! tip
    Use iasc when you need to sort multiple arrays based on one array's order

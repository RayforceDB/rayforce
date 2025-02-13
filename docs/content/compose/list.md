# List `list`

Creates a heterogeneous list from the provided arguments.

```clj
↪ (list 1 2 3)
(
  1
  2
  3
)
↪ (list 1 2 [3 4])
(
  1
  2
  [3 4]
)
```

!!! info
    - Always creates a list, never folds into a vector
    - Preserves nested data structures
    - Can hold elements of different types

!!! tip
    Use list when you specifically need a heterogeneous collection
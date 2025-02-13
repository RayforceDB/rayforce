# Enlist `enlist`

Creates a list from multiple arguments, folding into a vector if all arguments have the same type.

```clj
↪ (enlist 1 2 3)
[1 2 3]
↪ (enlist 1 2 [3 4])
(
  1
  2
  [3 4]
)
↪ (enlist 1 2 "asd")
(
  1
  2
  asd
)
```

!!! info
    - Creates a vector if all arguments are of the same type
    - Creates a list if arguments have different types
    - Preserves nested structures

!!! tip
    Use enlist when you need to combine multiple values into a single collection

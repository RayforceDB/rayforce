# Concat `concat`

Concatenates two values into a single list or array.

```clj
↪ (concat 1 2)
[1 2]
↪ (concat 1 [2 3])
[1 2 3]
↪ (concat [1 2] 3)
[1 2 3]
↪ (concat (list 1 2 "asd") 7)
(
  1
  2
  asd
  7
)
```

!!! info
    - When concatenating arrays of the same type, the result is an array
    - When concatenating different types, the result is a list
    - Single values are treated as one-element lists/arrays

!!! tip
    Use concat to build arrays incrementally or combine heterogeneous data

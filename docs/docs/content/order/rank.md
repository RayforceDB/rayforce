# Rank `rank`

Returns the rank (position in sorted order) of each element in a vector.

```clj
? (rank [30 10 20])
[2 0 1]
? (rank [5 3 1 4 2])
[4 2 0 3 1]
? (rank [1 2 3 4 5])
[0 1 2 3 4]
```

!!! info
    - Equivalent to `(iasc (iasc x))`


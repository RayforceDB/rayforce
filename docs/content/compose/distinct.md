# Distinct `distinct`

Returns a sorted list of unique elements from the input.

```clj
↪ (distinct [1 2 3 1 2 3 4 5 6 7 8 9 0])
[0 1 2 3 4 5 6 7 8 9]
```

!!! info
    - The output is always sorted in ascending order
    - Works with numbers, strings, and other comparable types
    - Maintains the type of the input (array → array, list → list)

!!! tip
    Use distinct to remove duplicates from data sets

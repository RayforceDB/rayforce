# Descending `desc`

Sorts elements in descending order.

```clj
↪ (desc [3 1 4 1 5 9 2 6])
[9 6 5 4 3 2 1 1]
↪ (desc ["banana" "apple" "cherry"])
[cherry banana apple]
```

!!! info
    - Works with numbers, strings, and other comparable types
    - Returns a new sorted array/list
    - Maintains the original data type (array → array, list → list)

!!! tip
    Use desc for simple descending sorts of single collections

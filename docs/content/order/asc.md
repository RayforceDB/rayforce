# Ascending `asc`

Sorts elements in ascending order.

```clj
↪ (asc [3 1 4 1 5 9 2 6])
[1 1 2 3 4 5 6 9]
↪ (asc ["banana" "apple" "cherry"])
[apple banana cherry]
```

!!! info
    - Works with numbers, strings, and other comparable types
    - Returns a new sorted array/list
    - Maintains the original data type (array → array, list → list)

!!! tip
    Use asc for simple ascending sorts of single collections

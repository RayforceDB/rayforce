# Reverse `reverse`

Reverses the order of elements in a list or array.

```clj
↪ (reverse [1 2 3 4 5])
[5 4 3 2 1]
```

!!! info
    - Works with both arrays and lists
    - Preserves the original data type (array stays array, list stays list)
    - Returns a new collection, doesn't modify the original

!!! tip
    Use reverse when you need to process elements in reverse order
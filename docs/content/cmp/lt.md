# Less Than `<`

The `<` function compares two values and returns `true` if the first value is less than the second value.

```clj
;; Number comparisons
↪ (< 1 2)
true
↪ (< 2 1)
false
↪ (< 1 1)
false

;; Array comparisons
↪ (< [1 2 3] 2)
[true false false]
↪ (< [1 2 3] [2 2 4])
[true false true]

;; Float comparisons
↪ (< 3.0 3.14)
true
↪ (< 3.14 3.0)
false

;; String comparisons (lexicographical order)
↪ (< "apple" "zoo")
true
↪ (< "zoo" "apple")
false
```

!!! warning
    Comparing values of different types may lead to unexpected results. Always ensure you're comparing compatible types.

!!! info
    When comparing arrays with a scalar value, the comparison is performed element-wise.

```clj
↪ (< 1 1)
false
↪ (< 2 1)
false
↪ (< 1 2)
true
↪ (< [1 2 3] 1)
[false false false]
↪ (< [1 2 3] [1 2 3])
[false false false]
↪ (< [1 2 3] [1 2 4])
[false false true]
```

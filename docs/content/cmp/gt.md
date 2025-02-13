# Greater Than `>`

The `>` function compares two values and returns `true` if the first value is greater than the second value.

```clj
;; Number comparisons
↪ (> 2 1)
true
↪ (> 1 2)
false
↪ (> 1 1)
false

;; Array comparisons
↪ (> [3 2 1] 2)
[true false false]
↪ (> [3 2 1] [2 2 0])
[true false true]

;; Float comparisons
↪ (> 3.14 3.0)
true
↪ (> 3.0 3.14)
false

;; String comparisons (lexicographical order)
↪ (> "zoo" "apple")
true
↪ (> "apple" "zoo")
false
```

!!! warning
    Comparing values of different types may lead to unexpected results. Always ensure you're comparing compatible types.

!!! info
    When comparing arrays with a scalar value, the comparison is performed element-wise.

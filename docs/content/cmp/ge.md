# Greater than or Equal `>=`

The `>=` function compares two values and returns `true` if the first value is greater than or equal to the second value.

```clj
;; Number comparisons
↪ (>= 2 1)
true
↪ (>= 1 1)
true
↪ (>= 1 2)
false

;; Array comparisons
↪ (>= [3 2 1] 2)
[true true false]
↪ (>= [3 2 1] [3 1 1])
[true true true]

;; Float comparisons
↪ (>= 3.14 3.14)
true
↪ (>= 3.0 3.14)
false

;; String comparisons (lexicographical order)
↪ (>= "zoo" "zoo")
true
↪ (>= "apple" "zoo")
false
```

!!! note "Important"
    The `>=` operator is equivalent to `(or (> a b) (== a b))` but is more efficient.

!!! info
    When comparing arrays with a scalar value, the comparison is performed element-wise.

# Equal `==`

The `==` function compares two values and returns `true` if the values are equal.

```clj
;; Number comparisons
↪ (== 1 1)
true
↪ (== 1 2)
false

;; Array comparisons
↪ (== [1 2 3] 1)
[true false false]
↪ (== [1 2 3] [1 2 3])
[true true true]
↪ (== [1 2 3] [1 2 4])
[true true false]

;; String comparisons
↪ (== "hello" "hello")
true
↪ (== "hello" "world")
false

;; Float comparisons
↪ (== 3.14 3.14)
true
↪ (== 3.14 3.140)
true

;; Mixed type comparisons
↪ (== 1 1.0)
true
↪ (== "1" 1)
false

;; Array comparisons
↪ (== [1 2 3] [1.0 2.0 3.0])
[true true true]

;; Comparing with null
↪ (== null null)
true
↪ (== 1 null)
false
```

!!! warning
    Comparing values of different types may lead to unexpected results. Always ensure you're comparing compatible types.

!!! info
    When comparing arrays with a scalar value, the comparison is performed element-wise.

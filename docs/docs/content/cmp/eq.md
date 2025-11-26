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

;; Mixed numeric type comparisons
↪ (== 1 1.0)
true

;; Array comparisons
↪ (== [1 2 3] [1.0 2.0 3.0])
[true true true]

;; Comparing with null
↪ (== null null)
true
```

!!! warning "Use `==` Not `=`"
    RayFall uses `==` for equality comparison, NOT `=`. The `=` operator does not exist for equality checks.

    ```clj
    ;; CORRECT
    (== x 5)

    ;; WRONG - will cause an error
    ;; (= x 5)
    ```

!!! warning "Type Compatibility"
    Comparing values of different types may lead to unexpected results. Always ensure you're comparing compatible types.

!!! info
    When comparing arrays with a scalar value, the comparison is performed element-wise.

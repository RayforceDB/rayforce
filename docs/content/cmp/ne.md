# Not Equal `!=`

The `!=` function compares two values and returns `true` if the values are not equal.

```clj
;; Number comparisons
↪ (!= 1 2)
true
↪ (!= 1 1)
false

;; Array comparisons
↪ (!= [1 2 3] 2)
[false true true]
↪ (!= [1 2 3] [1 2 4])
[false false true]

;; String comparisons
↪ (!= "hello" "world")
true
↪ (!= "hello" "hello")
false

;; Float comparisons
↪ (!= 3.14 3.0)
true
↪ (!= 3.14 3.140)
false

;; Mixed type comparisons
↪ (!= "1" 1)
true
↪ (!= 1 1.0)
false

;; Comparing with null
↪ (!= null null)
false
↪ (!= 1 null)
true
```

!!! note "Important"
    The `!=` operator is equivalent to `(not (== a b))` but may be more readable in some contexts.

!!! warning
    Be careful when comparing different types - the result might not always be what you expect.

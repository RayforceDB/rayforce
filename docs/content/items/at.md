# At `at`

The `at` function returns the element at the specified index.

```clj
↪ (at [1 2 3 4 5] 2)
3
(at (dict [a b c] (list [1 2] [3 4] [5 6])) 'b)
[3 4]
```
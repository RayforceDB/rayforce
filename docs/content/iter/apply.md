# Apply `apply`

Applies a function to each element of a list.

```clj  
↪ (apply (fn [x y] (+ x y)) 1 [1 2 34])
[2 3 35]
↪ (apply (fn [x y] (+ x y)) [1 2 3] [4 5 6])
[5 7 9]
```
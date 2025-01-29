# Do `do`

Evaluates the given expressions list and returns the value of the last one.

```clj
↪ (do 1 2 3)
3
↪ (do (let c (+ 1 2)) 6 (+ c 5))
8
```

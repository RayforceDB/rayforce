# Not equal `!=`

The `!=` function is used to compare two values. It returns `true` if the values are not equal, otherwise it returns `false`.

```clj
↪ (!= 1 1)
false
↪ (!= 1 2)
true
↪ (!= [1 2 3] 1)
[false true true]
↪ (!= [1 2 3] [1 2 3])
[false false false]
↪ (!= [1 2 3] [1 2 4])
[false false true]
```

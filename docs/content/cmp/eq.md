# Equal `==`

The `==` function is used to compare two values. It returns `true` if the values are equal, otherwise it returns `false`.

```clj
↪ (== 1 1)
true
↪ (== 1 2)
false
↪ (== [1 2 3] 1)
[true false false]
↪ (== [1 2 3] [1 2 3])
[true true true]
↪ (== [1 2 3] [1 2 4])
[true true false]
```

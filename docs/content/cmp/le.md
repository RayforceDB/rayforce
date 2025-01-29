# Lower or equal to `<=`

The `<=` function is used to compare two values. It returns `true` if the first value is lower than or equal to the second value, otherwise it returns `false`.

```clj
↪ (<= 1 1)
true
↪ (<= 2 1)
false
↪ (<= 1 2)
true
↪ (<= [1 2 3] 1)
[true false false]
↪ (<= [1 2 3] [1 2 3])
[true true true]
↪ (<= [1 2 3] [1 2 4])
[true true true]
```

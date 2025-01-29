# Lower than `<`

The `<` function is used to compare two values. It returns `true` if the first value is lower than the second value, otherwise it returns `false`.

```clj
> (< 1 1)
false
> (< 2 1)
false
> (< 1 2)
true
> (< [1 2 3] 1)
[false false false]
> (< [1 2 3] [1 2 3])
[false false false]
> (< [1 2 3] [1 2 4])
[false false true]
```

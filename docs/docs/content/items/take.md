# Take `take`

`(take x y)` returns the first `x` elements of a `y`.
If `x` < 0, returns the last `x` elements of a `y`.

```clj
↪ (take 3 [1 2 3 4 5])
[1 2 3]

↪ (take -3 [1 2 3 4 5])
[3 4 5]
```

If a `y` is atom, it will be repeat `x` times:

```clj
↪ (take 2 'a')
"aa"

↪ (take 0 'a')
""
```

If `x` > (count `y`), `y` is treated as circular:

```clj
↪ (take 5 (list 1 'a' "abc"))
(
  1
  a
  abc
  1
  a
)
```

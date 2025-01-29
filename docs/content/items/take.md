# Take `take`

`take` returns the first `n` elements of a list.

```clj
(take 3 [1 2 3 4 5])
[1 2 3]
```

If the second argument is atom, it will repeat the first argument `n` times.

```clj
(take 3 1)
[1 1 1]
```
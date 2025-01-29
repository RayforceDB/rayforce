# List `list`

Constructs a list from the arguments without folding to a vector.

```clj
> (list 1 2 3)
(
  1
  2
  3
)
> (list 1 2 [3 4])
(
  1
  2
  [3 4]
)
```
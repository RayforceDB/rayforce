# Within `within`

`within` is a function that returns a boolean list indicating whether the elements of the first list are within the corresponding elements of the second list.

```clj
(within 6 [1 20])
[true]
↪ (within [1 2 3] [1 100])
[true true true]
```